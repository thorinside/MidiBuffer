#include <cstddef>
#include <distingnt/api.h>

#include <new>
#include <stdint.h>

#include "midibuffer_core.hpp"
#include "nt_host.hpp"

namespace midibuffer {
namespace {

const uint32_t kBytesPerMegabyte = 1000000U;
const int32_t kDefaultBufferMegabytes = 1;
const float kGateThresholdVolts = 1.0f;
const uint32_t kClockAverageWindow = 8U;

static_assert(sizeof(RecordedEvent) == 24,
              "recording event size is part of capacity accounting");
static_assert(kNT_destinationBreakout == 0x01 &&
                  kNT_destinationSelectBus == 0x02 &&
                  kNT_destinationUSB == 0x04 &&
                  kNT_destinationInternal == 0x08,
              "playback destination mapping must match the pinned API");

enum Parameter {
    kParameterClock,
    kParameterReset,
    kParameterCapture,
    kParameterRecordingChannel,
    kParameterPlaybackDestination,
    kParameterPlaybackChannel,
    kParameterFilterControlChange,
    kParameterFilterPitchBend,
    kParameterFilterAftertouch,
    kNumParameters,
};

enum Specification {
    kSpecificationBufferMegabytes,
    kNumSpecifications,
};

struct CallbackState {
    uint32_t clockEdges;
    uint32_t resetEdges;
    uint32_t channelMessages;
    uint32_t realtimeMessages;
    uint32_t parameterChanges;
    uint32_t uiChanges;
    uint8_t lastMidi[3];
    uint8_t lastRealtime;
};

struct Algorithm : public _NT_algorithm {
    Algorithm(uint8_t* recordingBuffer, uint32_t recordingBufferBytesValue)
        : _NT_algorithm(),
          recordingEvents(reinterpret_cast<RecordedEvent*>(recordingBuffer)),
          recordingBufferBytes(recordingBufferBytesValue),
          eventCapacity(recordingBufferBytesValue / sizeof(RecordedEvent)),
          eventHead(0), eventCount(0), sampleCursor(0), currentPulse(0),
          lastPulseSample(0), lastClockIntervalSamples(0), clockIntervalSum(0),
          playbackPulse(0), playbackIntervalStartSample(0),
          playbackNextEventSample(0), playbackEventIndex(0),
          clockIntervalWriteIndex(0),
          clockIntervalCount(0), clockIntervals(), selection(), state(),
          captureEnabled(false), clockRunning(false),
          haveAcquisitionPulse(false), clockHigh(false), resetHigh(false),
          selectionValid(false), playbackArmed(false), playbackActive(false),
          playbackIntervalOpen(false), playbackNextEventScheduled(false) {}

    RecordedEvent* recordingEvents;
    uint32_t recordingBufferBytes;
    uint32_t eventCapacity;
    uint32_t eventHead;
    uint32_t eventCount;

    uint64_t sampleCursor;
    uint64_t currentPulse;
    uint64_t lastPulseSample;
    uint64_t lastClockIntervalSamples;
    uint32_t clockIntervalSum;
    uint64_t playbackPulse;
    uint64_t playbackIntervalStartSample;
    uint64_t playbackNextEventSample;
    uint32_t playbackEventIndex;
    uint32_t clockIntervalWriteIndex;
    uint32_t clockIntervalCount;
    uint32_t clockIntervals[kClockAverageWindow];
    PulseRange selection;
    CallbackState state;

    bool captureEnabled;
    bool clockRunning;
    bool haveAcquisitionPulse;
    bool clockHigh;
    bool resetHigh;
    bool selectionValid;
    bool playbackArmed;
    bool playbackActive;
    bool playbackIntervalOpen;
    bool playbackNextEventScheduled;
};

static const char* const kCaptureStrings[] = {
    "Stop Capture",
    "Start Capture",
};

static const char* const kRecordingChannelStrings[] = {
    "Omni", "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",    "10", "11", "12", "13", "14", "15", "16",
};

static const char* const kPlaybackDestinationStrings[] = {
    "Breakout", "USB", "Select Bus", "Internal", "All",
};

static const char* const kPlaybackChannelStrings[] = {
    "Original", "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",        "10", "11", "12", "13", "14", "15", "16",
};

static const char* const kFilterStrings[] = {
    "Off",
    "On",
};

// The API macros include their own trailing commas.
// clang-format off
static const _NT_parameter kParameters[] = {
    NT_PARAMETER_CV_INPUT("Clock", 1, 1)
    NT_PARAMETER_CV_INPUT("Reset", 1, 2)
    {
        .name = "Capture",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kCaptureStrings,
    },
    {
        .name = "Record Ch",
        .min = 0,
        .max = 16,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kRecordingChannelStrings,
    },
    {
        .name = "MIDI Out",
        .min = 0,
        .max = 4,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kPlaybackDestinationStrings,
    },
    {
        .name = "Play Ch",
        .min = 0,
        .max = 16,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kPlaybackChannelStrings,
    },
    {
        .name = "Filter CC",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kFilterStrings,
    },
    {
        .name = "Filter Pitch Bend",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kFilterStrings,
    },
    {
        .name = "Filter Aftertouch",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kFilterStrings,
    },
};
// clang-format on

static const uint8_t kInputPageParameters[] = {
    kParameterClock,
    kParameterReset,
};

static const uint8_t kCapturePageParameters[] = {
    kParameterCapture,
    kParameterRecordingChannel,
};

static const uint8_t kPlaybackPageParameters[] = {
    kParameterPlaybackDestination,
    kParameterPlaybackChannel,
    kParameterFilterControlChange,
    kParameterFilterPitchBend,
    kParameterFilterAftertouch,
};

static const _NT_parameterPage kParameterPageDefinitions[] = {
    {
        .name = "Inputs",
        .numParams = ARRAY_SIZE(kInputPageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kInputPageParameters,
    },
    {
        .name = "Capture",
        .numParams = ARRAY_SIZE(kCapturePageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kCapturePageParameters,
    },
    {
        .name = "Playback",
        .numParams = ARRAY_SIZE(kPlaybackPageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kPlaybackPageParameters,
    },
};

static const _NT_parameterPages kParameterPages = {
    .numPages = ARRAY_SIZE(kParameterPageDefinitions),
    .pages = kParameterPageDefinitions,
};

static const _NT_specification kSpecifications[] = {
    {
        .name = "Buffer MB",
        .min = 1,
        .max = 5,
        .def = kDefaultBufferMegabytes,
        .type = kNT_typeGeneric,
    },
};

int32_t bufferMegabytes(const int32_t* specifications) {
    int32_t value = specifications == NULL
                        ? kDefaultBufferMegabytes
                        : specifications[kSpecificationBufferMegabytes];
    if (value < kSpecifications[kSpecificationBufferMegabytes].min) {
        return kSpecifications[kSpecificationBufferMegabytes].min;
    }
    if (value > kSpecifications[kSpecificationBufferMegabytes].max) {
        return kSpecifications[kSpecificationBufferMegabytes].max;
    }
    return value;
}

uint32_t requestedRecordingBytes(const int32_t* specifications) {
    return static_cast<uint32_t>(bufferMegabytes(specifications)) *
           kBytesPerMegabyte;
}

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t* specifications) {
    requirements.numParameters = kNumParameters;
    requirements.sram = sizeof(Algorithm);
    requirements.dram = requestedRecordingBytes(specifications);
    requirements.dtc = 0;
    requirements.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements& requirements,
                         const int32_t* specifications) {
    const uint32_t recordingBytes = requestedRecordingBytes(specifications);
    if (memory.sram == NULL || memory.dram == NULL ||
        requirements.sram < sizeof(Algorithm) ||
        requirements.dram < recordingBytes) {
        return NULL;
    }

    Algorithm* algorithm =
        new (memory.sram) Algorithm(memory.dram, recordingBytes);
    algorithm->parameters = kParameters;
    algorithm->parameterPages = &kParameterPages;
    return algorithm;
}

void parameterChanged(_NT_algorithm* self, int parameter) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || parameter < 0 || parameter >= kNumParameters) {
        return;
    }

    ++algorithm->state.parameterChanges;
    if (parameter == kParameterCapture && algorithm->v != NULL) {
        algorithm->captureEnabled = algorithm->v[kParameterCapture] !=
                                    kParameters[kParameterCapture].min;
    } else if (parameter == kParameterClock) {
        // A newly routed input must acquire its own edge rather than inherit
        // the previous input's gate level.
        algorithm->clockHigh = false;
    } else if (parameter == kParameterReset) {
        algorithm->resetHigh = false;
    }
}

void clearSelection(Algorithm& algorithm) {
    algorithm.selectionValid = false;
    algorithm.playbackArmed = false;
    algorithm.playbackActive = false;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
    algorithm.playbackPulse = 0;
    algorithm.playbackEventIndex = 0;
    algorithm.selection.startPulse = 0;
    algorithm.selection.endPulse = 0;
}

bool pulseInsideSelection(const Algorithm& algorithm, uint64_t pulse) {
    return algorithm.selectionValid &&
           pulse >= algorithm.selection.startPulse &&
           pulse <= algorithm.selection.endPulse;
}

uint32_t nextRingIndex(uint32_t index, uint32_t capacity) {
    ++index;
    return index == capacity ? 0 : index;
}

void appendEvent(Algorithm& algorithm, const RecordedEvent& event) {
    if (algorithm.eventCapacity == 0) {
        return;
    }

    uint32_t writeIndex = 0;
    if (algorithm.eventCount == algorithm.eventCapacity) {
        writeIndex = algorithm.eventHead;
        if (pulseInsideSelection(algorithm,
                                 algorithm.recordingEvents[writeIndex].pulse)) {
            clearSelection(algorithm);
        }
        algorithm.eventHead =
            nextRingIndex(algorithm.eventHead, algorithm.eventCapacity);
    } else {
        writeIndex = algorithm.eventHead + algorithm.eventCount;
        if (writeIndex >= algorithm.eventCapacity) {
            writeIndex -= algorithm.eventCapacity;
        }
        ++algorithm.eventCount;
    }
    algorithm.recordingEvents[writeIndex] = event;
}

uint32_t playbackDestinationMask(const Algorithm& algorithm) {
    int destination = 0;
    if (algorithm.v != NULL) {
        destination = algorithm.v[kParameterPlaybackDestination];
    }

    switch (destination) {
    case 1:
        return kNT_destinationUSB;
    case 2:
        return kNT_destinationSelectBus;
    case 3:
        return kNT_destinationInternal;
    case 4:
        return kNT_destinationBreakout | kNT_destinationUSB |
               kNT_destinationSelectBus | kNT_destinationInternal;
    case 0:
    default:
        return kNT_destinationBreakout;
    }
}

uint8_t playbackStatus(const Algorithm& algorithm, uint8_t recordedStatus) {
    int channel = 0;
    if (algorithm.v != NULL) {
        channel = algorithm.v[kParameterPlaybackChannel];
    }
    if (channel < 1 || channel > 16) {
        return recordedStatus;
    }
    return static_cast<uint8_t>((recordedStatus & 0xf0U) |
                                static_cast<uint8_t>(channel - 1));
}

bool excludedController(uint8_t controller) {
    return controller == 6U || controller == 38U ||
           (controller >= 96U && controller <= 101U) || controller >= 120U;
}

bool eligiblePerformanceEvent(const RecordedEvent& event) {
    const uint8_t messageType = event.bytes[0] & 0xf0U;
    if (messageType == 0xd0U) {
        return event.size == 2U;
    }
    if (event.size != 3U) {
        return false;
    }
    if (messageType == 0xb0U) {
        return !excludedController(event.bytes[1]);
    }
    return messageType == 0x80U || messageType == 0x90U ||
           messageType == 0xa0U || messageType == 0xe0U;
}

bool filterEnabled(const Algorithm& algorithm, Parameter parameter) {
    return algorithm.v != NULL && algorithm.v[parameter] != 0;
}

bool recordedEventPassesPlaybackFilters(const Algorithm& algorithm,
                                        const RecordedEvent& event) {
    if (!eligiblePerformanceEvent(event)) {
        return false;
    }

    switch (event.bytes[0] & 0xf0U) {
    case 0xa0U:
    case 0xd0U:
        return !filterEnabled(algorithm, kParameterFilterAftertouch);
    case 0xb0U:
        return !filterEnabled(algorithm, kParameterFilterControlChange);
    case 0xe0U:
        return !filterEnabled(algorithm, kParameterFilterPitchBend);
    default:
        return true;
    }
}

void emitRecordedEvent(const Algorithm& algorithm, const RecordedEvent& event,
                       uint64_t dispatchSample) {
    if (!recordedEventPassesPlaybackFilters(algorithm, event)) {
        return;
    }

    const uint32_t destination = playbackDestinationMask(algorithm);
    const uint8_t status = playbackStatus(algorithm, event.bytes[0]);
    if (event.size == 2U) {
        nt_host::sendMidi2(destination, status, event.bytes[1],
                           dispatchSample);
    } else {
        nt_host::sendMidi3(destination, status, event.bytes[1], event.bytes[2],
                           dispatchSample);
    }
}

uint32_t findPlaybackEventIndex(const Algorithm& algorithm, uint64_t pulse) {
    for (uint32_t index = 0; index < algorithm.eventCount; ++index) {
        uint32_t ringIndex = algorithm.eventHead + index;
        if (ringIndex >= algorithm.eventCapacity) {
            ringIndex -= algorithm.eventCapacity;
        }
        if (algorithm.recordingEvents[ringIndex].pulse >= pulse) {
            return index;
        }
    }
    return algorithm.eventCount;
}

const RecordedEvent* nextPlaybackEvent(const Algorithm& algorithm) {
    if (algorithm.playbackEventIndex >= algorithm.eventCount) {
        return NULL;
    }
    uint32_t ringIndex = algorithm.eventHead + algorithm.playbackEventIndex;
    if (ringIndex >= algorithm.eventCapacity) {
        ringIndex -= algorithm.eventCapacity;
    }
    return &algorithm.recordingEvents[ringIndex];
}

void clearClockAverage(Algorithm& algorithm) {
    algorithm.clockIntervalSum = 0;
    algorithm.clockIntervalWriteIndex = 0;
    algorithm.clockIntervalCount = 0;
    for (uint32_t index = 0; index < kClockAverageWindow; ++index) {
        algorithm.clockIntervals[index] = 0;
    }
}

uint32_t boundedInterval(uint64_t interval) {
    // Eight capped values fit exactly in the 32-bit running sum. The cap is
    // over three hours at 48 kHz, far beyond a practical musical clock.
    const uint32_t maximumAveragedInterval = 0x1fffffffU;
    return interval > maximumAveragedInterval
               ? maximumAveragedInterval
               : static_cast<uint32_t>(interval);
}

void addClockInterval(Algorithm& algorithm, uint64_t measuredInterval) {
    const uint32_t interval = boundedInterval(measuredInterval);
    if (algorithm.clockIntervalCount == kClockAverageWindow) {
        algorithm.clockIntervalSum -=
            static_cast<uint32_t>(algorithm.clockIntervals[
                algorithm.clockIntervalWriteIndex]);
    } else {
        ++algorithm.clockIntervalCount;
    }

    algorithm.clockIntervals[algorithm.clockIntervalWriteIndex] = interval;
    algorithm.clockIntervalSum += interval;
    algorithm.clockIntervalWriteIndex =
        (algorithm.clockIntervalWriteIndex + 1U) &
        (kClockAverageWindow - 1U);
}

uint64_t predictedClockInterval(const Algorithm& algorithm) {
    if (algorithm.clockIntervalCount == 0) {
        return 0;
    }
    const uint32_t quotient =
        algorithm.clockIntervalSum / algorithm.clockIntervalCount;
    const uint32_t remainder =
        algorithm.clockIntervalSum % algorithm.clockIntervalCount;
    return quotient +
           (remainder * 2U >= algorithm.clockIntervalCount ? 1U : 0U);
}

uint64_t multiplyDivideRounded(uint32_t left, uint32_t right,
                               uint32_t divisor) {
    if (divisor == 0U) {
        return 0;
    }

    const uint64_t product = static_cast<uint64_t>(left) * right;
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1U) | ((product >> bit) & 1U);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= static_cast<uint64_t>(1) << bit;
        }
    }
    if (remainder * 2U >= divisor) {
        ++quotient;
    }
    return quotient;
}

uint64_t eventDispatchSample(const Algorithm& algorithm,
                             const RecordedEvent& event) {
    const uint32_t prediction = boundedInterval(
        predictedClockInterval(algorithm));
    const uint64_t scaledOffset = multiplyDivideRounded(
        event.offsetSamples, prediction, event.sourceIntervalSamples);
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    return scaledOffset > maximum - algorithm.playbackIntervalStartSample
               ? maximum
               : algorithm.playbackIntervalStartSample + scaledOffset;
}

void scheduleNextPlaybackEvent(Algorithm& algorithm) {
    const RecordedEvent* event = nextPlaybackEvent(algorithm);
    algorithm.playbackNextEventScheduled =
        event != NULL && event->pulse == algorithm.playbackPulse &&
        event->pulse < algorithm.selection.endPulse;
    if (algorithm.playbackNextEventScheduled) {
        algorithm.playbackNextEventSample =
            eventDispatchSample(algorithm, *event);
    }
}

void dispatchDuePlaybackEvents(Algorithm& algorithm, uint64_t sample) {
    if (!algorithm.playbackActive || !algorithm.playbackIntervalOpen ||
        !algorithm.selectionValid) {
        return;
    }

    while (algorithm.playbackNextEventScheduled &&
           algorithm.playbackNextEventSample <= sample) {
        const RecordedEvent* event = nextPlaybackEvent(algorithm);
        ++algorithm.playbackEventIndex;
        emitRecordedEvent(algorithm, *event, sample);
        scheduleNextPlaybackEvent(algorithm);
    }
}

void flushPendingPlaybackEvents(Algorithm& algorithm, uint64_t sample) {
    if (!algorithm.playbackActive || !algorithm.playbackIntervalOpen) {
        return;
    }

    const RecordedEvent* event = nextPlaybackEvent(algorithm);
    while (event != NULL && event->pulse == algorithm.playbackPulse &&
           event->pulse < algorithm.selection.endPulse) {
        ++algorithm.playbackEventIndex;
        emitRecordedEvent(algorithm, *event, sample);
        event = nextPlaybackEvent(algorithm);
    }
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
}

void advancePlaybackPulse(Algorithm& algorithm) {
    ++algorithm.playbackPulse;
    if (algorithm.playbackPulse >= algorithm.selection.endPulse) {
        algorithm.playbackPulse = algorithm.selection.startPulse;
        algorithm.playbackEventIndex =
            findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
    }
}

void beginPlaybackInterval(Algorithm& algorithm, uint64_t sample) {
    algorithm.playbackIntervalStartSample = sample;
    algorithm.playbackIntervalOpen = true;
    scheduleNextPlaybackEvent(algorithm);
    dispatchDuePlaybackEvents(algorithm, sample);
}

void activateArmedPlayback(Algorithm& algorithm, uint64_t sample) {
    algorithm.playbackArmed = false;
    algorithm.playbackActive = true;
    algorithm.playbackPulse = algorithm.selection.startPulse;
    algorithm.playbackEventIndex =
        findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
    beginPlaybackInterval(algorithm, sample);
}

void beginOrAdvancePlayback(Algorithm& algorithm, uint64_t sample) {
    if (algorithm.playbackArmed) {
        activateArmedPlayback(algorithm, sample);
    } else if (algorithm.playbackActive) {
        advancePlaybackPulse(algorithm);
        beginPlaybackInterval(algorithm, sample);
    }
}

void handleClockEdge(Algorithm& algorithm, uint64_t sample) {
    ++algorithm.state.clockEdges;
    ++algorithm.currentPulse;

    if (algorithm.clockRunning) {
        flushPendingPlaybackEvents(algorithm, sample);
        const uint64_t interval = sample - algorithm.lastPulseSample;
        if (interval != 0) {
            algorithm.lastClockIntervalSamples = interval;
            addClockInterval(algorithm, interval);
        }
        algorithm.lastPulseSample = sample;
        beginOrAdvancePlayback(algorithm, sample);
        return;
    }

    if (!algorithm.haveAcquisitionPulse) {
        algorithm.haveAcquisitionPulse = true;
        algorithm.lastPulseSample = sample;
        return;
    }

    const uint64_t interval = sample - algorithm.lastPulseSample;
    algorithm.lastPulseSample = sample;
    if (interval != 0) {
        algorithm.lastClockIntervalSamples = interval;
        addClockInterval(algorithm, interval);
        algorithm.clockRunning = true;
        algorithm.haveAcquisitionPulse = false;
        if (algorithm.playbackArmed) {
            activateArmedPlayback(algorithm, sample);
        } else if (algorithm.playbackActive) {
            beginPlaybackInterval(algorithm, sample);
        }
    }
}

void detectClockLoss(Algorithm& algorithm, uint64_t sample) {
    if (!algorithm.clockRunning) {
        return;
    }

    const uint64_t maximumInterval = ~static_cast<uint64_t>(0) / 2U;
    const uint64_t lossDelay =
        algorithm.lastClockIntervalSamples > maximumInterval
            ? ~static_cast<uint64_t>(0)
            : algorithm.lastClockIntervalSamples * 2U;
    if (sample - algorithm.lastPulseSample >= lossDelay) {
        algorithm.clockRunning = false;
        algorithm.haveAcquisitionPulse = false;
        algorithm.lastClockIntervalSamples = 0;
        algorithm.playbackIntervalOpen = false;
        algorithm.playbackNextEventScheduled = false;
        clearClockAverage(algorithm);
    }
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || algorithm->v == NULL || busFrames == NULL ||
        numFramesBy4 <= 0 || numFramesBy4 > 0x1fffffff) {
        return;
    }

    const int numFrames = numFramesBy4 * 4;
    const int clockBus = algorithm->v[kParameterClock];
    const int resetBus = algorithm->v[kParameterReset];
    const float* clockFrames = clockBus >= 1 && clockBus <= kNT_lastBus
                                   ? busFrames + (clockBus - 1) * numFrames
                                   : NULL;
    const float* resetFrames = resetBus >= 1 && resetBus <= kNT_lastBus
                                   ? busFrames + (resetBus - 1) * numFrames
                                   : NULL;

    for (int frame = 0; frame < numFrames; ++frame) {
        const bool clockHigh =
            clockFrames != NULL && clockFrames[frame] > kGateThresholdVolts;
        if (clockHigh && !algorithm->clockHigh) {
            handleClockEdge(*algorithm, algorithm->sampleCursor);
        } else {
            detectClockLoss(*algorithm, algorithm->sampleCursor);
        }
        algorithm->clockHigh = clockHigh;
        if (algorithm->clockRunning) {
            dispatchDuePlaybackEvents(*algorithm, algorithm->sampleCursor);
        }

        const bool resetHigh =
            resetFrames != NULL && resetFrames[frame] > kGateThresholdVolts;
        if (resetHigh && !algorithm->resetHigh) {
            ++algorithm->state.resetEdges;
        }
        algorithm->resetHigh = resetHigh;
        ++algorithm->sampleCursor;
    }
}

void midiRealtime(_NT_algorithm* self, uint8_t byte) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    ++algorithm->state.realtimeMessages;
    algorithm->state.lastRealtime = byte;
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1,
                 uint8_t byte2) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    ++algorithm->state.channelMessages;
    algorithm->state.lastMidi[0] = byte0;
    algorithm->state.lastMidi[1] = byte1;
    algorithm->state.lastMidi[2] = byte2;

    if (!algorithm->captureEnabled || !algorithm->clockRunning) {
        return;
    }

    RecordedEvent event = {
        algorithm->currentPulse,
        0,
        boundedInterval(algorithm->lastClockIntervalSamples),
        {byte0, byte1, byte2},
        static_cast<uint8_t>((byte0 & 0xf0U) == 0xd0U ? 2U : 3U),
    };
    if (!eligiblePerformanceEvent(event)) {
        return;
    }

    const uint8_t channel = static_cast<uint8_t>((byte0 & 0x0fU) + 1U);
    int recordingChannel = 0;
    if (algorithm->v != NULL) {
        recordingChannel = algorithm->v[kParameterRecordingChannel];
    }
    if (recordingChannel < 0 || recordingChannel > 16) {
        recordingChannel = 0;
    }
    if (recordingChannel != 0 && channel != recordingChannel) {
        return;
    }

    uint64_t offset = algorithm->sampleCursor - algorithm->lastPulseSample;
    if (offset > 0xffffffffULL) {
        offset = 0xffffffffULL;
    }
    // The MIDI callback carries no arrival timestamp. A callback at the first
    // sample boundary after a scanned pulse is the finest observable
    // pulse-aligned case; normalize that one-sample ambiguity to phase zero.
    event.offsetSamples =
        offset <= 1U ? 0U : static_cast<uint32_t>(offset);
    appendEvent(*algorithm, event);
}

uint32_t hasCustomUi(_NT_algorithm*) {
    return kNT_potL | kNT_potC | kNT_potR | kNT_encoderL | kNT_encoderR |
           kNT_encoderButtonL | kNT_encoderButtonR;
}

void customUi(_NT_algorithm* self, const _NT_uiData& data) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    if (data.controls != 0 || data.encoders[0] != 0 || data.encoders[1] != 0) {
        ++algorithm->state.uiChanges;
    }
}

void setupUi(_NT_algorithm*, _NT_float3& pots) {
    pots[0] = 0.0f;
    pots[1] = 0.0f;
    pots[2] = 0.0f;
}

char* appendUnsigned(char* output, uint32_t value) {
    char reversed[10];
    int count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    while (count > 0) {
        *output++ = reversed[--count];
    }
    *output = '\0';
    return output;
}

void drawCounter(int y, const char* label, uint32_t value) {
    char text[32];
    char* cursor = text;
    while (*label != '\0') {
        *cursor++ = *label++;
    }
    appendUnsigned(cursor, value);
    nt_host::drawText(0, y, text);
}

bool draw(_NT_algorithm* self) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return false;
    }

    nt_host::drawText(0, 8, "MidiBuffer");
    nt_host::drawText(0, 18,
                      algorithm->captureEnabled ? "Capture: Started"
                                                : "Capture: Stopped");
    nt_host::drawText(
        0, 28, algorithm->clockRunning ? "Clock: Running" : "Clock: Acquiring");
    drawCounter(38, "Events: ", algorithm->eventCount);
    drawCounter(48, "Capacity: ", algorithm->eventCapacity);
    nt_host::drawText(
        0, 58, algorithm->selectionValid ? "Selected: Yes" : "Selected: No");
    return false;
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('M', 'd', 'B', 'f'),
    .name = "MidiBuffer",
    .description = "Clocked MIDI history buffer",
    .numSpecifications = kNumSpecifications,
    .specifications = kSpecifications,
    .calculateStaticRequirements = NULL,
    .initialise = NULL,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = draw,
    .midiRealtime = midiRealtime,
    .midiMessage = midiMessage,
    .tags = kNT_tagUtility,
    .hasCustomUi = hasCustomUi,
    .customUi = customUi,
    .setupUi = setupUi,
    .serialise = NULL,
    .deserialise = NULL,
    .midiSysEx = NULL,
    .parameterUiPrefix = NULL,
    .parameterString = NULL,
};

const Algorithm* asAlgorithm(const _NT_algorithm* self) {
    return static_cast<const Algorithm*>(self);
}

Algorithm* asAlgorithm(_NT_algorithm* self) {
    return static_cast<Algorithm*>(self);
}

} // namespace

CaptureSnapshot captureSnapshot(const _NT_algorithm* self) {
    CaptureSnapshot snapshot = {};
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return snapshot;
    }

    snapshot.recordingAllocationBytes = algorithm->recordingBufferBytes;
    snapshot.metadataBytes = sizeof(Algorithm);
    snapshot.eventCapacity = algorithm->eventCapacity;
    snapshot.eventCount = algorithm->eventCount;
    snapshot.currentPulse = algorithm->currentPulse;
    snapshot.lastClockIntervalSamples = algorithm->lastClockIntervalSamples;
    snapshot.predictedClockIntervalSamples =
        predictedClockInterval(*algorithm);
    snapshot.playbackPulse = algorithm->playbackPulse;
    snapshot.clockAverageIntervalCount = algorithm->clockIntervalCount;
    snapshot.captureEnabled = algorithm->captureEnabled;
    snapshot.clockRunning = algorithm->clockRunning;
    snapshot.selectionValid = algorithm->selectionValid;
    snapshot.playbackArmed = algorithm->playbackArmed;
    snapshot.playbackActive = algorithm->playbackActive;
    snapshot.selection = algorithm->selection;
    if (algorithm->eventCount != 0) {
        snapshot.oldestPulse =
            algorithm->recordingEvents[algorithm->eventHead].pulse;
        uint32_t newestIndex =
            algorithm->eventHead + algorithm->eventCount - 1U;
        if (newestIndex >= algorithm->eventCapacity) {
            newestIndex -= algorithm->eventCapacity;
        }
        snapshot.newestPulse = algorithm->recordingEvents[newestIndex].pulse;
    }
    return snapshot;
}

bool recordedEventAt(const _NT_algorithm* self, uint32_t oldestFirstIndex,
                     RecordedEvent& event) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || oldestFirstIndex >= algorithm->eventCount) {
        return false;
    }

    uint32_t ringIndex = algorithm->eventHead + oldestFirstIndex;
    if (ringIndex >= algorithm->eventCapacity) {
        ringIndex -= algorithm->eventCapacity;
    }
    event = algorithm->recordingEvents[ringIndex];
    return true;
}

bool setPulseSelection(_NT_algorithm* self, uint64_t startPulse,
                       uint64_t endPulse) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || algorithm->eventCount == 0 ||
        startPulse >= endPulse) {
        return false;
    }

    const uint64_t oldestPulse =
        algorithm->recordingEvents[algorithm->eventHead].pulse;
    if (startPulse < oldestPulse || endPulse > algorithm->currentPulse) {
        return false;
    }

    algorithm->selection.startPulse = startPulse;
    algorithm->selection.endPulse = endPulse;
    algorithm->selectionValid = true;
    return true;
}

void clearPulseSelection(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm != NULL) {
        clearSelection(*algorithm);
    }
}

bool acquirePlaybackSelection(const _NT_algorithm* self, PulseRange& range) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || !algorithm->selectionValid ||
        algorithm->eventCount == 0 ||
        algorithm->selection.startPulse >= algorithm->selection.endPulse) {
        return false;
    }

    const uint64_t oldestPulse =
        algorithm->recordingEvents[algorithm->eventHead].pulse;
    if (algorithm->selection.startPulse < oldestPulse ||
        algorithm->selection.endPulse > algorithm->currentPulse) {
        return false;
    }

    range = algorithm->selection;
    return true;
}

bool startPlayback(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    PulseRange range = {};
    if (algorithm == NULL || !acquirePlaybackSelection(self, range)) {
        return false;
    }

    algorithm->playbackPulse = range.startPulse;
    algorithm->playbackEventIndex =
        findPlaybackEventIndex(*algorithm, algorithm->playbackPulse);
    algorithm->playbackIntervalOpen = false;
    algorithm->playbackNextEventScheduled = false;
    algorithm->playbackActive = false;
    algorithm->playbackArmed = true;
    return true;
}

void stopPlayback(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm != NULL) {
        algorithm->playbackArmed = false;
        algorithm->playbackActive = false;
        algorithm->playbackIntervalOpen = false;
        algorithm->playbackNextEventScheduled = false;
    }
}

void dispatchSafetyMidi3(_NT_algorithm* self, uint8_t status, uint8_t data1,
                         uint8_t data2) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return;
    }

    // Safety callers supply the final output channel. This path deliberately
    // shares only destination routing with replay: recorded-event eligibility,
    // expressive filters, and playback channel override never apply to cleanup.
    nt_host::sendMidi3(playbackDestinationMask(*algorithm), status, data1,
                       data2);
}

namespace nt_host {

void drawText(int x, int y, const char* text) { NT_drawText(x, y, text); }

#if defined(MIDIBUFFER_NATIVE_TEST)
extern "C" void midibufferTestSetDispatchSample(uint64_t sample);
#endif

void traceDispatchSample(uint64_t sample) {
#if defined(MIDIBUFFER_NATIVE_TEST)
    midibufferTestSetDispatchSample(sample);
#else
    (void)sample;
#endif
}

void sendMidiByte(uint32_t destination, uint8_t byte0,
                  uint64_t dispatchSample) {
    traceDispatchSample(dispatchSample);
    NT_sendMidiByte(destination, byte0);
}

void sendMidi2(uint32_t destination, uint8_t byte0, uint8_t byte1,
               uint64_t dispatchSample) {
    traceDispatchSample(dispatchSample);
    NT_sendMidi2ByteMessage(destination, byte0, byte1);
}

void sendMidi3(uint32_t destination, uint8_t byte0, uint8_t byte1,
               uint8_t byte2, uint64_t dispatchSample) {
    traceDispatchSample(dispatchSample);
    NT_sendMidi3ByteMessage(destination, byte0, byte1, byte2);
}

} // namespace nt_host
} // namespace midibuffer

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:
        return kNT_apiVersion13;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return reinterpret_cast<uintptr_t>(data == 0 ? &midibuffer::kFactory
                                                     : NULL);
    }
    return 0;
}
