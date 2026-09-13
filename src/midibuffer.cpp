#include <cstddef>
#include <distingnt/api.h>
#include <distingnt/serialisation.h>

#include <new>
#include <stdint.h>

#include "midibuffer_core.hpp"
#include "nt_host.hpp"
#include "range_motion.hpp"

namespace midibuffer {
namespace {

const uint64_t kMaximumDisplayedBars = 9999999999ULL;

bool supportedPulsesPerBeat(uint32_t pulsesPerBeat) {
    return pulsesPerBeat == 1U || pulsesPerBeat == 2U ||
           pulsesPerBeat == 4U || pulsesPerBeat == 8U ||
           pulsesPerBeat == 16U || pulsesPerBeat == 24U ||
           pulsesPerBeat == 48U;
}

uint64_t divideUnsigned64By32(uint64_t value, uint32_t divisor,
                              uint32_t& remainder) {
    uint64_t quotient = 0;
    uint64_t workingRemainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        workingRemainder =
            (workingRemainder << 1U) | ((value >> bit) & 1U);
        if (workingRemainder >= divisor) {
            workingRemainder -= divisor;
            quotient |= static_cast<uint64_t>(1) << bit;
        }
    }
    remainder = static_cast<uint32_t>(workingRemainder);
    return quotient;
}

class BoundedText {
  public:
    BoundedText(char* output, size_t capacity)
        : output_(output), capacity_(capacity), length_(0U),
          valid_(output != NULL && capacity != 0U) {
        if (valid_) {
            output_[0] = '\0';
        }
    }

    void append(char value) {
        if (!valid_ || length_ + 1U >= capacity_) {
            valid_ = false;
            return;
        }
        output_[length_++] = value;
        output_[length_] = '\0';
    }

    void append(const char* text) {
        if (text == NULL) {
            valid_ = false;
            return;
        }
        while (*text != '\0') {
            append(*text++);
        }
    }

    void appendUnsigned64(uint64_t value) {
        char reversed[20];
        size_t count = 0U;
        do {
            uint32_t remainder = 0U;
            value = divideUnsigned64By32(value, 10U, remainder);
            reversed[count++] = static_cast<char>('0' + remainder);
        } while (value != 0U);
        while (count != 0U) {
            append(reversed[--count]);
        }
    }

    bool valid() const { return valid_; }

  private:
    char* output_;
    size_t capacity_;
    size_t length_;
    bool valid_;
};

const uint32_t kBytesPerMegabyte = 1000000U;
const int32_t kDefaultBufferMegabytes = 1;
const float kGateThresholdVolts = 1.0f;
const uint32_t kClockAverageWindow = 8U;
const uint32_t kRecordedEventFlagCaptureEnding = 0x01U;
const uint32_t kRecordedEventEndingIndexShift = 1U;
const uint64_t kDefaultTimelineVisiblePulses = 64U;
const uint64_t kTimelineMinimumVisiblePulses = 4U;
const uint32_t kTimelineLegacyMaximumVisiblePulses = 256U;
const uint32_t kTimelineCoordinateMaximum = 65535U;

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
    kParameterPulsesPerDisplayedBeat,
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

struct RecordedState {
    uint32_t heldNoteCounts[16][128];
    uint32_t heldNoteTotal;
    uint32_t polyPressureMasks[16][4];
    uint16_t pitchBendMask;
    uint16_t channelPressureMask;
    uint8_t pedalMasks[16];
    uint8_t modulationMasks[16];
    uint8_t expressionMasks[16];
};

enum TransportState {
    kTransportStopped,
    kTransportArmed,
    kTransportPlaying,
    kTransportClockLossPaused,
};

struct PlaybackOutputState {
    uint32_t heldNoteCounts[16][128];
    uint16_t sustainChannels;
};

// Note tails and sustain releases are owned by the attack/press that admitted
// them into playback. The event indices remain stable while playback excludes
// capture, and split arrays avoid padding thousands of small records.
struct PendingPlaybackEndings {
    uint64_t noteDueIntervals[16][128];
    uint32_t noteEventIndices[16][128];
    uint32_t noteActiveMasks[16][4];
    uint64_t sustainDueIntervals[16];
    uint32_t sustainEventIndices[16];
    uint32_t noteCount;
    uint32_t sustainCount;
    uint16_t sustainActiveChannels;
};

struct Algorithm : public _NT_algorithm {
    Algorithm(uint8_t* recordingBuffer, uint32_t recordingBufferBytesValue)
        : _NT_algorithm(),
          recordingEvents(reinterpret_cast<RecordedEvent*>(recordingBuffer)),
          recordingBufferBytes(recordingBufferBytesValue),
          eventCapacity(recordingBufferBytesValue / sizeof(RecordedEvent)),
          eventHead(0), eventCount(0), sampleCursor(0), currentPulse(0),
          historyEndPulseExclusive(0), lastPulseSample(0),
          lastClockIntervalSamples(0), clockIntervalSum(0), playbackPulse(0),
          playbackIntervalStartSample(0), playbackNextEventSample(0),
          playbackEventIndex(0), clockIntervalWriteIndex(0),
          clockIntervalCount(0), clockIntervals(), selection(),
          activeSelection(), state(), recordedState(), playbackOutputState(),
          pendingEndings(), rangeMotion(), transportState(kTransportStopped),
          playbackIntervalOrdinal(0), timelineScrollPulses(0),
          timelineVisiblePulses(kDefaultTimelineVisiblePulses),
          zoomPressVisiblePulses(0), zoomPressManualVisiblePulses(0),
          rangeMotionHistoryStart(0), rangeMotionHistoryEnd(0),
          zoomPressCoordinate(0),
          captureEnabled(false), clockRunning(false),
          haveAcquisitionPulse(false), clockHigh(false), resetHigh(false),
          selectionValid(false), activeSelectionValid(false),
          rangeTransitionPending(false), playbackPositionValid(false),
          playbackIntervalOpen(false), playbackNextEventScheduled(false),
          pendingNextEndingScheduled(false),
          selectionFineTarget(kSelectionFineTargetEnd),
          timelineShowAll(true), zoomPressShowAll(true),
          rightPotZoomActive(false), rangeMotionDomainKnown(false),
          rangeMotionNeedsPhysicalSeed(true), rightEncoderHoldActive(false),
          rightEncoderPanicFired(false),
          pendingNextEndingSample(0), rightEncoderHoldStartSample(0) {}

    RecordedEvent* recordingEvents;
    uint32_t recordingBufferBytes;
    uint32_t eventCapacity;
    uint32_t eventHead;
    uint32_t eventCount;

    uint64_t sampleCursor;
    uint64_t currentPulse;
    uint64_t historyEndPulseExclusive;
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
    PulseRange activeSelection;
    CallbackState state;
    RecordedState recordedState;
    PlaybackOutputState playbackOutputState;
    PendingPlaybackEndings pendingEndings;
    RangeMotionState rangeMotion;
    TransportState transportState;
    uint64_t playbackIntervalOrdinal;
    uint64_t timelineScrollPulses;
    uint64_t timelineVisiblePulses;
    uint64_t zoomPressVisiblePulses;
    uint64_t zoomPressManualVisiblePulses;
    uint64_t rangeMotionHistoryStart;
    uint64_t rangeMotionHistoryEnd;
    uint32_t zoomPressCoordinate;

    bool captureEnabled;
    bool clockRunning;
    bool haveAcquisitionPulse;
    bool clockHigh;
    bool resetHigh;
    bool selectionValid;
    bool activeSelectionValid;
    bool rangeTransitionPending;
    bool playbackPositionValid;
    bool playbackIntervalOpen;
    bool playbackNextEventScheduled;
    bool pendingNextEndingScheduled;
    SelectionFineTarget selectionFineTarget;
    bool timelineShowAll;
    bool zoomPressShowAll;
    bool rightPotZoomActive;
    bool rangeMotionDomainKnown;
    bool rangeMotionNeedsPhysicalSeed;
    bool rightEncoderHoldActive;
    bool rightEncoderPanicFired;
    uint64_t pendingNextEndingSample;
    uint64_t rightEncoderHoldStartSample;
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

static const char* const kPulsesPerDisplayedBeatStrings[] = {
    "1", "2", "4", "8", "16", "24", "48",
};

static const uint8_t kPulsesPerDisplayedBeatValues[] = {
    1, 2, 4, 8, 16, 24, 48,
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
    {
        .name = "Pulses/Beat",
        .min = 0,
        .max = 6,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kPulsesPerDisplayedBeatStrings,
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

static const uint8_t kTimelinePageParameters[] = {
    kParameterPulsesPerDisplayedBeat,
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
    {
        .name = "Timeline",
        .numParams = ARRAY_SIZE(kTimelinePageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kTimelinePageParameters,
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

void finalizeCapture(Algorithm& algorithm);
void clearPendingPlaybackEndings(Algorithm& algorithm);

void parameterChanged(_NT_algorithm* self, int parameter) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || parameter < 0 || parameter >= kNumParameters) {
        return;
    }

    ++algorithm->state.parameterChanges;
    if (parameter == kParameterCapture && algorithm->v != NULL) {
        const bool requested = algorithm->v[kParameterCapture] !=
                               kParameters[kParameterCapture].min;
        if (!requested) {
            finalizeCapture(*algorithm);
        } else if (!algorithm->captureEnabled &&
                   algorithm->transportState == kTransportStopped) {
            algorithm->recordedState = RecordedState();
            algorithm->captureEnabled = true;
        }
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
    algorithm.activeSelectionValid = false;
    algorithm.rangeTransitionPending = false;
    algorithm.transportState = kTransportStopped;
    algorithm.playbackOutputState = PlaybackOutputState();
    clearPendingPlaybackEndings(algorithm);
    algorithm.playbackPositionValid = false;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
    algorithm.playbackPulse = 0;
    algorithm.playbackEventIndex = 0;
    algorithm.selection.startPulse = 0;
    algorithm.selection.endPulse = 0;
    algorithm.activeSelection.startPulse = 0;
    algorithm.activeSelection.endPulse = 0;
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
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    const uint64_t eventEnd = event.pulse == maximum ? maximum
                                                      : event.pulse + 1U;
    if (eventEnd > algorithm.historyEndPulseExclusive) {
        algorithm.historyEndPulseExclusive = eventEnd;
    }
}

void setMaskBit(uint32_t& mask, uint8_t bit, bool set) {
    const uint32_t value = static_cast<uint32_t>(1U) << bit;
    mask = set ? mask | value : mask & ~value;
}

uint32_t boundedInterval(uint64_t interval);

void trackRecordedState(Algorithm& algorithm, const RecordedEvent& event) {
    const uint8_t type = event.bytes[0] & 0xf0U;
    const uint8_t channel = event.bytes[0] & 0x0fU;
    RecordedState& state = algorithm.recordedState;

    if (type == 0x80U || type == 0x90U) {
        const uint8_t note = event.bytes[1] & 0x7fU;
        uint32_t& count = state.heldNoteCounts[channel][note];
        const bool noteOn = type == 0x90U && event.bytes[2] != 0U;
        if (noteOn && state.heldNoteTotal < algorithm.eventCapacity) {
            ++count;
            ++state.heldNoteTotal;
        } else if (!noteOn && count != 0U) {
            --count;
            --state.heldNoteTotal;
        }
        return;
    }

    if (type == 0xa0U) {
        const uint8_t note = event.bytes[1] & 0x7fU;
        setMaskBit(state.polyPressureMasks[channel][note / 32U],
                   note % 32U, event.bytes[2] != 0U);
        return;
    }

    if (type == 0xb0U) {
        const uint8_t controller = event.bytes[1];
        if (controller >= 64U && controller <= 69U) {
            const uint8_t bit = controller - 64U;
            const uint8_t value = static_cast<uint8_t>(1U << bit);
            state.pedalMasks[channel] =
                event.bytes[2] >= 64U
                    ? static_cast<uint8_t>(state.pedalMasks[channel] | value)
                    : static_cast<uint8_t>(state.pedalMasks[channel] & ~value);
        } else if (controller == 1U || controller == 33U) {
            const uint8_t value = static_cast<uint8_t>(
                1U << static_cast<uint8_t>(controller == 33U));
            state.modulationMasks[channel] =
                event.bytes[2] != 0U
                    ? static_cast<uint8_t>(state.modulationMasks[channel] |
                                           value)
                    : static_cast<uint8_t>(state.modulationMasks[channel] &
                                           ~value);
        } else if (controller == 11U || controller == 43U) {
            const uint8_t value = static_cast<uint8_t>(
                1U << static_cast<uint8_t>(controller == 43U));
            state.expressionMasks[channel] =
                event.bytes[2] != 127U
                    ? static_cast<uint8_t>(state.expressionMasks[channel] |
                                           value)
                    : static_cast<uint8_t>(state.expressionMasks[channel] &
                                           ~value);
        }
        return;
    }

    if (type == 0xd0U) {
        const uint16_t value = static_cast<uint16_t>(1U << channel);
        state.channelPressureMask =
            event.bytes[1] != 0U
                ? static_cast<uint16_t>(state.channelPressureMask | value)
                : static_cast<uint16_t>(state.channelPressureMask & ~value);
    } else if (type == 0xe0U) {
        const uint16_t value = static_cast<uint16_t>(1U << channel);
        const uint16_t bend = static_cast<uint16_t>(event.bytes[1] & 0x7fU) |
                              static_cast<uint16_t>(
                                  (event.bytes[2] & 0x7fU) << 7U);
        state.pitchBendMask =
            bend != 8192U
                ? static_cast<uint16_t>(state.pitchBendMask | value)
                : static_cast<uint16_t>(state.pitchBendMask & ~value);
    }
}

RecordedEvent captureEndingTemplate(const Algorithm& algorithm) {
    uint64_t offset = algorithm.sampleCursor - algorithm.lastPulseSample;
    if (offset > 0xffffffffULL) {
        offset = 0xffffffffULL;
    }
    return RecordedEvent{
        algorithm.currentPulse,
        offset <= 1U ? 0U : static_cast<uint32_t>(offset),
        boundedInterval(algorithm.lastClockIntervalSamples),
        {0, 0, 0},
        3,
        kRecordedEventFlagCaptureEnding,
    };
}

void appendEnding(Algorithm& algorithm, const RecordedEvent& endingTemplate,
                  uint8_t status, uint8_t data1, uint8_t data2,
                  uint8_t size = 3U) {
    RecordedEvent event = endingTemplate;
    event.bytes[0] = status;
    event.bytes[1] = data1;
    event.bytes[2] = data2;
    event.size = size;
    appendEvent(algorithm, event);
}

void finalizeCapture(Algorithm& algorithm) {
    if (!algorithm.captureEnabled) {
        return;
    }

    const RecordedEvent ending = captureEndingTemplate(algorithm);
    const RecordedState& state = algorithm.recordedState;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            for (uint32_t occurrence = 0;
                 occurrence < state.heldNoteCounts[channel][note];
                 ++occurrence) {
                appendEnding(algorithm, ending,
                             static_cast<uint8_t>(0x80U | channel), note, 0);
            }
        }
    }
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t controller = 64U; controller <= 69U; ++controller) {
            if ((state.pedalMasks[channel] &
                 static_cast<uint8_t>(1U << (controller - 64U))) != 0U) {
                appendEnding(algorithm, ending,
                             static_cast<uint8_t>(0xb0U | channel),
                             controller, 0);
            }
        }
        const uint8_t modulationControllers[] = {1U, 33U};
        const uint8_t expressionControllers[] = {11U, 43U};
        for (uint8_t index = 0; index < 2U; ++index) {
            if ((state.modulationMasks[channel] & (1U << index)) != 0U) {
                appendEnding(algorithm, ending,
                             static_cast<uint8_t>(0xb0U | channel),
                             modulationControllers[index], 0);
            }
            if ((state.expressionMasks[channel] & (1U << index)) != 0U) {
                appendEnding(algorithm, ending,
                             static_cast<uint8_t>(0xb0U | channel),
                             expressionControllers[index], 127);
            }
        }
        if ((state.pitchBendMask & (1U << channel)) != 0U) {
            appendEnding(algorithm, ending,
                         static_cast<uint8_t>(0xe0U | channel), 0, 64);
        }
        if ((state.channelPressureMask & (1U << channel)) != 0U) {
            appendEnding(algorithm, ending,
                         static_cast<uint8_t>(0xd0U | channel), 0, 0, 2);
        }
        for (uint8_t word = 0; word < 4U; ++word) {
            uint32_t mask = state.polyPressureMasks[channel][word];
            for (uint8_t bit = 0; bit < 32U; ++bit) {
                if ((mask & (static_cast<uint32_t>(1U) << bit)) != 0U) {
                    appendEnding(
                        algorithm, ending,
                        static_cast<uint8_t>(0xa0U | channel),
                        static_cast<uint8_t>(word * 32U + bit), 0);
                }
            }
        }
    }

    algorithm.recordedState = RecordedState();
    algorithm.captureEnabled = false;
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
    // Synthetic endings are performance-safety boundaries, not optional
    // expression. They remain effective if filters change after capture.
    if ((event.flags & kRecordedEventFlagCaptureEnding) != 0U) {
        return true;
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

void clearPendingPlaybackEndings(Algorithm& algorithm) {
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t word = 0; word < 4U; ++word) {
            algorithm.pendingEndings.noteActiveMasks[channel][word] = 0U;
        }
    }
    algorithm.pendingEndings.noteCount = 0;
    algorithm.pendingEndings.sustainCount = 0;
    algorithm.pendingEndings.sustainActiveChannels = 0;
    algorithm.pendingNextEndingScheduled = false;
    algorithm.pendingNextEndingSample = 0;
}

void trackPlaybackOutput(Algorithm& algorithm, uint8_t status, uint8_t data1,
                         uint8_t data2, uint8_t size) {
    if (size != 3U) {
        return;
    }

    const uint8_t type = status & 0xf0U;
    const uint8_t channel = status & 0x0fU;
    if (type == 0x80U || type == 0x90U) {
        uint32_t& count =
            algorithm.playbackOutputState.heldNoteCounts[channel][data1 & 0x7fU];
        const bool noteOn = type == 0x90U && data2 != 0U;
        if (noteOn && count != 0xffffffffU) {
            ++count;
        } else if (!noteOn && count != 0U) {
            --count;
        }
    } else if (type == 0xb0U && data1 == 64U) {
        const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
        if (data2 >= 64U) {
            algorithm.playbackOutputState.sustainChannels =
                static_cast<uint16_t>(
                    algorithm.playbackOutputState.sustainChannels |
                    channelBit);
        } else {
            algorithm.playbackOutputState.sustainChannels =
                static_cast<uint16_t>(
                    algorithm.playbackOutputState.sustainChannels &
                    ~channelBit);
        }
    }
}

void emitRecordedEvent(Algorithm& algorithm, const RecordedEvent& event,
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
    trackPlaybackOutput(algorithm, status, event.bytes[1], event.bytes[2],
                        event.size);
}

void releasePlaybackOutput(Algorithm& algorithm, uint64_t dispatchSample) {
    const uint32_t destination = playbackDestinationMask(algorithm);
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            const uint32_t count =
                algorithm.playbackOutputState.heldNoteCounts[channel][note];
            for (uint32_t occurrence = 0; occurrence < count; ++occurrence) {
                nt_host::sendMidi3(destination,
                                   static_cast<uint8_t>(0x80U | channel),
                                   note, 0, dispatchSample);
            }
        }
    }
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        if ((algorithm.playbackOutputState.sustainChannels &
             static_cast<uint16_t>(1U << channel)) != 0U) {
            nt_host::sendMidi3(destination,
                               static_cast<uint8_t>(0xb0U | channel), 64, 0,
                               dispatchSample);
        }
    }
    algorithm.playbackOutputState = PlaybackOutputState();
    clearPendingPlaybackEndings(algorithm);
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

RecordedEvent* recordedEventByIndex(Algorithm& algorithm,
                                    uint32_t oldestFirstIndex) {
    if (oldestFirstIndex >= algorithm.eventCount) {
        return NULL;
    }
    uint32_t ringIndex = algorithm.eventHead + oldestFirstIndex;
    if (ringIndex >= algorithm.eventCapacity) {
        ringIndex -= algorithm.eventCapacity;
    }
    return &algorithm.recordingEvents[ringIndex];
}

bool notePending(const Algorithm& algorithm, uint8_t channel, uint8_t note) {
    return (algorithm.pendingEndings.noteActiveMasks[channel][note / 32U] &
            (static_cast<uint32_t>(1U) << (note % 32U))) != 0U;
}

void setNotePending(Algorithm& algorithm, uint8_t channel, uint8_t note,
                    bool active) {
    uint32_t& mask =
        algorithm.pendingEndings.noteActiveMasks[channel][note / 32U];
    const uint32_t bit = static_cast<uint32_t>(1U) << (note % 32U);
    const bool wasActive = (mask & bit) != 0U;
    if (wasActive == active) {
        return;
    }
    mask = active ? mask | bit : mask & ~bit;
    if (active) {
        ++algorithm.pendingEndings.noteCount;
    } else {
        --algorithm.pendingEndings.noteCount;
    }
}

uint32_t encodedEndingIndex(uint32_t eventIndex) {
    return (eventIndex + 1U) << kRecordedEventEndingIndexShift;
}

bool decodedEndingIndex(const RecordedEvent& event, uint32_t& eventIndex) {
    const uint32_t encoded =
        event.flags >> kRecordedEventEndingIndexShift;
    if (encoded == 0U) {
        return false;
    }
    eventIndex = encoded - 1U;
    return true;
}

void prepareRecordedEndingOwnership(Algorithm& algorithm) {
    clearPendingPlaybackEndings(algorithm);
    const uint32_t none = algorithm.eventCount;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            algorithm.pendingEndings.noteEventIndices[channel][note] = none;
            algorithm.pendingEndings.noteDueIntervals[channel][note] = none;
        }
        algorithm.pendingEndings.sustainEventIndices[channel] = none;
        algorithm.pendingEndings.sustainDueIntervals[channel] = none;
    }

    // Build FIFO note occurrence pairs in one bounded pass. During this pass a
    // queued note-on's metadata temporarily links to the next queued attack;
    // popping it replaces that link with its recorded note-off index.
    for (uint32_t index = 0; index < algorithm.eventCount; ++index) {
        RecordedEvent* event = recordedEventByIndex(algorithm, index);
        event->flags &= kRecordedEventFlagCaptureEnding;
        const uint8_t type = event->bytes[0] & 0xf0U;
        const uint8_t channel = event->bytes[0] & 0x0fU;
        const uint8_t note = event->bytes[1] & 0x7fU;
        const bool noteOn = type == 0x90U && event->bytes[2] != 0U;
        const bool noteOff = type == 0x80U ||
                             (type == 0x90U && event->bytes[2] == 0U);
        if (noteOn) {
            const uint32_t tail = static_cast<uint32_t>(
                algorithm.pendingEndings.noteDueIntervals[channel][note]);
            if (tail == none) {
                algorithm.pendingEndings.noteEventIndices[channel][note] =
                    index;
            } else {
                RecordedEvent* tailEvent =
                    recordedEventByIndex(algorithm, tail);
                tailEvent->flags |= encodedEndingIndex(index);
            }
            algorithm.pendingEndings.noteDueIntervals[channel][note] = index;
        } else if (noteOff) {
            const uint32_t head =
                algorithm.pendingEndings.noteEventIndices[channel][note];
            if (head != none) {
                RecordedEvent* attack = recordedEventByIndex(algorithm, head);
                uint32_t next = none;
                decodedEndingIndex(*attack, next);
                attack->flags &= kRecordedEventFlagCaptureEnding;
                attack->flags |= encodedEndingIndex(index);
                algorithm.pendingEndings.noteEventIndices[channel][note] = next;
                if (next == none) {
                    algorithm.pendingEndings.noteDueIntervals[channel][note] =
                        none;
                }
            }
        } else if (type == 0xb0U && event->bytes[1] == 64U) {
            if (event->bytes[2] >= 64U) {
                const uint32_t tail = static_cast<uint32_t>(
                    algorithm.pendingEndings.sustainDueIntervals[channel]);
                if (tail == none) {
                    algorithm.pendingEndings.sustainEventIndices[channel] =
                        index;
                } else {
                    RecordedEvent* tailEvent =
                        recordedEventByIndex(algorithm, tail);
                    tailEvent->flags |= encodedEndingIndex(index);
                }
                algorithm.pendingEndings.sustainDueIntervals[channel] = index;
            } else {
                uint32_t press =
                    algorithm.pendingEndings.sustainEventIndices[channel];
                while (press != none) {
                    RecordedEvent* pressEvent =
                        recordedEventByIndex(algorithm, press);
                    uint32_t next = none;
                    decodedEndingIndex(*pressEvent, next);
                    pressEvent->flags &= kRecordedEventFlagCaptureEnding;
                    pressEvent->flags |= encodedEndingIndex(index);
                    press = next;
                }
                algorithm.pendingEndings.sustainEventIndices[channel] = none;
                algorithm.pendingEndings.sustainDueIntervals[channel] = none;
            }
        }
    }
    clearPendingPlaybackEndings(algorithm);
}

bool findRecordedEnding(const Algorithm& algorithm,
                        const RecordedEvent& beginning,
                        uint32_t& endingIndex) {
    return decodedEndingIndex(beginning, endingIndex) &&
           endingIndex < algorithm.eventCount;
}

void emitTrackedMidi3(Algorithm& algorithm, uint8_t status, uint8_t data1,
                      uint8_t data2, uint64_t dispatchSample) {
    nt_host::sendMidi3(playbackDestinationMask(algorithm), status, data1,
                       data2, dispatchSample);
    trackPlaybackOutput(algorithm, status, data1, data2, 3U);
}

void refreshPendingEndingSchedule(Algorithm& algorithm) {
    algorithm.pendingNextEndingScheduled = false;
    uint64_t nextSample = 0;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            if (!notePending(algorithm, channel, note) ||
                algorithm.pendingEndings.noteDueIntervals[channel][note] !=
                    algorithm.playbackIntervalOrdinal) {
                continue;
            }
            const RecordedEvent* ending = recordedEventByIndex(
                algorithm,
                algorithm.pendingEndings.noteEventIndices[channel][note]);
            if (ending == NULL) {
                continue;
            }
            const uint64_t sample = eventDispatchSample(algorithm, *ending);
            if (!algorithm.pendingNextEndingScheduled || sample < nextSample) {
                nextSample = sample;
                algorithm.pendingNextEndingScheduled = true;
            }
        }
        const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
        if ((algorithm.pendingEndings.sustainActiveChannels & channelBit) !=
                0U &&
            algorithm.pendingEndings.sustainDueIntervals[channel] ==
                algorithm.playbackIntervalOrdinal) {
            const RecordedEvent* release = recordedEventByIndex(
                algorithm,
                algorithm.pendingEndings.sustainEventIndices[channel]);
            if (release != NULL) {
                const uint64_t sample = eventDispatchSample(algorithm, *release);
                if (!algorithm.pendingNextEndingScheduled ||
                    sample < nextSample) {
                    nextSample = sample;
                    algorithm.pendingNextEndingScheduled = true;
                }
            }
        }
    }
    algorithm.pendingNextEndingSample = nextSample;
}

void cancelPendingNote(Algorithm& algorithm, uint8_t channel, uint8_t note,
                       uint64_t dispatchSample, bool releaseNow) {
    if (!notePending(algorithm, channel, note)) {
        return;
    }
    setNotePending(algorithm, channel, note, false);
    if (releaseNow) {
        emitTrackedMidi3(algorithm, static_cast<uint8_t>(0x80U | channel),
                         note, 0U, dispatchSample);
    }
    refreshPendingEndingSchedule(algorithm);
}

void schedulePendingNote(Algorithm& algorithm, uint8_t outputChannel,
                         uint8_t note, const RecordedEvent& attack) {
    uint32_t endingIndex = 0;
    if (!findRecordedEnding(algorithm, attack, endingIndex)) {
        return;
    }
    const RecordedEvent* ending = recordedEventByIndex(algorithm, endingIndex);
    algorithm.pendingEndings.noteEventIndices[outputChannel][note] =
        endingIndex;
    algorithm.pendingEndings.noteDueIntervals[outputChannel][note] =
        algorithm.playbackIntervalOrdinal + ending->pulse - attack.pulse;
    setNotePending(algorithm, outputChannel, note, true);
    refreshPendingEndingSchedule(algorithm);
}

void cancelPendingSustain(Algorithm& algorithm, uint8_t channel) {
    const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
    if ((algorithm.pendingEndings.sustainActiveChannels & channelBit) == 0U) {
        return;
    }
    algorithm.pendingEndings.sustainActiveChannels =
        static_cast<uint16_t>(algorithm.pendingEndings.sustainActiveChannels &
                              ~channelBit);
    --algorithm.pendingEndings.sustainCount;
    refreshPendingEndingSchedule(algorithm);
}

void schedulePendingSustain(Algorithm& algorithm, uint8_t outputChannel,
                            const RecordedEvent& press) {
    uint32_t releaseIndex = 0;
    if (!findRecordedEnding(algorithm, press, releaseIndex)) {
        return;
    }
    const RecordedEvent* release =
        recordedEventByIndex(algorithm, releaseIndex);
    const uint16_t channelBit = static_cast<uint16_t>(1U << outputChannel);
    if ((algorithm.pendingEndings.sustainActiveChannels & channelBit) == 0U) {
        algorithm.pendingEndings.sustainActiveChannels =
            static_cast<uint16_t>(
                algorithm.pendingEndings.sustainActiveChannels | channelBit);
        ++algorithm.pendingEndings.sustainCount;
    }
    algorithm.pendingEndings.sustainEventIndices[outputChannel] = releaseIndex;
    algorithm.pendingEndings.sustainDueIntervals[outputChannel] =
        algorithm.playbackIntervalOrdinal + release->pulse - press.pulse;
    refreshPendingEndingSchedule(algorithm);
}

bool emitOwnedEnding(Algorithm& algorithm, uint32_t eventIndex,
                     uint64_t dispatchSample) {
    const RecordedEvent* ending = recordedEventByIndex(algorithm, eventIndex);
    if (ending == NULL) {
        return false;
    }
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            if (notePending(algorithm, channel, note) &&
                algorithm.pendingEndings.noteEventIndices[channel][note] ==
                    eventIndex &&
                algorithm.pendingEndings.noteDueIntervals[channel][note] ==
                    algorithm.playbackIntervalOrdinal) {
                setNotePending(algorithm, channel, note, false);
                emitTrackedMidi3(
                    algorithm,
                    static_cast<uint8_t>((ending->bytes[0] & 0xf0U) | channel),
                    note, ending->bytes[2], dispatchSample);
                refreshPendingEndingSchedule(algorithm);
                return true;
            }
        }
        const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
        if ((algorithm.pendingEndings.sustainActiveChannels & channelBit) !=
                0U &&
            algorithm.pendingEndings.sustainEventIndices[channel] ==
                eventIndex &&
            algorithm.pendingEndings.sustainDueIntervals[channel] ==
                algorithm.playbackIntervalOrdinal) {
            algorithm.pendingEndings.sustainActiveChannels =
                static_cast<uint16_t>(
                    algorithm.pendingEndings.sustainActiveChannels &
                    ~channelBit);
            --algorithm.pendingEndings.sustainCount;
            emitTrackedMidi3(algorithm, static_cast<uint8_t>(0xb0U | channel),
                             64U, ending->bytes[2], dispatchSample);
            refreshPendingEndingSchedule(algorithm);
            return true;
        }
    }
    return false;
}

void emitSelectedEvent(Algorithm& algorithm, const RecordedEvent& event,
                       uint32_t eventIndex, uint64_t dispatchSample) {
    const uint8_t type = event.bytes[0] & 0xf0U;
    const bool noteOn = type == 0x90U && event.bytes[2] != 0U;
    const bool noteEnding = type == 0x80U ||
                            (type == 0x90U && event.bytes[2] == 0U);
    if (noteEnding) {
        // Only an ending owned by an in-range attack is eligible. This also
        // suppresses releases for notes that began before the selection.
        emitOwnedEnding(algorithm, eventIndex, dispatchSample);
        return;
    }
    if (noteOn) {
        if (!recordedEventPassesPlaybackFilters(algorithm, event)) {
            return;
        }
        const uint8_t status = playbackStatus(algorithm, event.bytes[0]);
        const uint8_t channel = status & 0x0fU;
        const uint8_t note = event.bytes[1] & 0x7fU;
        // Output-channel ownership is resolved after override. A replacement
        // cannot inherit the older occurrence's delayed ending.
        cancelPendingNote(algorithm, channel, note, dispatchSample, true);
        emitTrackedMidi3(algorithm, status, event.bytes[1], event.bytes[2],
                         dispatchSample);
        schedulePendingNote(algorithm, channel, note, event);
        return;
    }
    if (type == 0xb0U && event.bytes[1] == 64U) {
        if (event.bytes[2] < 64U) {
            if (emitOwnedEnding(algorithm, eventIndex, dispatchSample)) {
                return;
            }
            const uint8_t outputChannel =
                playbackStatus(algorithm, event.bytes[0]) & 0x0fU;
            const uint16_t outputChannelBit =
                static_cast<uint16_t>(1U << outputChannel);
            if ((algorithm.pendingEndings.sustainActiveChannels &
                 outputChannelBit) != 0U) {
                // This release belonged to a canceled older press. It cannot
                // fall through as an ordinary CC and disable the newer routed
                // owner on the same output channel.
                return;
            }
            // An in-range release without playback-owned sustain remains an
            // ordinary recorded CC. Capture-stop endings also retain their
            // filter-bypassing safety semantics.
            emitRecordedEvent(algorithm, event, dispatchSample);
            return;
        }
        if (!recordedEventPassesPlaybackFilters(algorithm, event)) {
            return;
        }
        const uint8_t status = playbackStatus(algorithm, event.bytes[0]);
        const uint8_t channel = status & 0x0fU;
        // A newer press silently takes release ownership; sustain may remain
        // continuously enabled across repeated overlapping loop passes.
        cancelPendingSustain(algorithm, channel);
        emitTrackedMidi3(algorithm, status, event.bytes[1], event.bytes[2],
                         dispatchSample);
        schedulePendingSustain(algorithm, channel, event);
        return;
    }
    emitRecordedEvent(algorithm, event, dispatchSample);
}

bool sameRange(const PulseRange& left, const PulseRange& right) {
    return left.startPulse == right.startPulse &&
           left.endPulse == right.endPulse;
}

void scheduleNextPlaybackEvent(Algorithm& algorithm) {
    const RecordedEvent* event = nextPlaybackEvent(algorithm);
    algorithm.playbackNextEventScheduled =
        algorithm.activeSelectionValid && event != NULL &&
        event->pulse == algorithm.playbackPulse &&
        event->pulse < algorithm.activeSelection.endPulse;
    if (algorithm.playbackNextEventScheduled) {
        algorithm.playbackNextEventSample =
            eventDispatchSample(algorithm, *event);
    }
}

void dispatchPendingEndingsAt(Algorithm& algorithm, uint64_t sample,
                              bool forceCurrentInterval) {
    if (!algorithm.pendingNextEndingScheduled ||
        (!forceCurrentInterval &&
         algorithm.pendingNextEndingSample > sample)) {
        return;
    }
    const uint64_t dueSample = algorithm.pendingNextEndingSample;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            if (!notePending(algorithm, channel, note) ||
                algorithm.pendingEndings.noteDueIntervals[channel][note] !=
                    algorithm.playbackIntervalOrdinal) {
                continue;
            }
            const uint32_t eventIndex =
                algorithm.pendingEndings.noteEventIndices[channel][note];
            const RecordedEvent* ending =
                recordedEventByIndex(algorithm, eventIndex);
            if (ending != NULL &&
                eventDispatchSample(algorithm, *ending) == dueSample) {
                emitOwnedEnding(algorithm, eventIndex, sample);
            }
        }
        const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
        if ((algorithm.pendingEndings.sustainActiveChannels & channelBit) !=
                0U &&
            algorithm.pendingEndings.sustainDueIntervals[channel] ==
                algorithm.playbackIntervalOrdinal) {
            const uint32_t eventIndex =
                algorithm.pendingEndings.sustainEventIndices[channel];
            const RecordedEvent* release =
                recordedEventByIndex(algorithm, eventIndex);
            if (release != NULL &&
                eventDispatchSample(algorithm, *release) == dueSample) {
                emitOwnedEnding(algorithm, eventIndex, sample);
            }
        }
    }
    refreshPendingEndingSchedule(algorithm);
}

void dispatchPlaybackEvents(Algorithm& algorithm, uint64_t sample,
                            bool forceCurrentInterval) {
    if (algorithm.transportState != kTransportPlaying ||
        !algorithm.playbackIntervalOpen ||
        !algorithm.activeSelectionValid) {
        return;
    }

    while (true) {
        const bool regularDue =
            algorithm.playbackNextEventScheduled &&
            (forceCurrentInterval ||
             algorithm.playbackNextEventSample <= sample);
        const bool pendingDue =
            algorithm.pendingNextEndingScheduled &&
            (forceCurrentInterval ||
             algorithm.pendingNextEndingSample <= sample);
        if (!regularDue && !pendingDue) {
            break;
        }

        // At an equal timestamp, process the new selected event first. This
        // lets a same-time retrigger/new sustain press cancel the older pass's
        // ending exactly as ownership requires.
        if (regularDue &&
            (!pendingDue || algorithm.playbackNextEventSample <=
                                algorithm.pendingNextEndingSample)) {
            const uint32_t eventIndex = algorithm.playbackEventIndex;
            const RecordedEvent* event = nextPlaybackEvent(algorithm);
            ++algorithm.playbackEventIndex;
            emitSelectedEvent(algorithm, *event, eventIndex, sample);
            scheduleNextPlaybackEvent(algorithm);
        } else {
            dispatchPendingEndingsAt(algorithm, sample,
                                     forceCurrentInterval);
        }
    }
}

void flushPendingPlaybackEvents(Algorithm& algorithm, uint64_t sample) {
    dispatchPlaybackEvents(algorithm, sample, true);
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
    algorithm.pendingNextEndingScheduled = false;
}

void advancePlaybackPulse(Algorithm& algorithm, uint64_t sample) {
    ++algorithm.playbackPulse;
    if (algorithm.playbackPulse < algorithm.activeSelection.endPulse) {
        return;
    }

    if (algorithm.rangeTransitionPending) {
        // A changed range owns a clean phrase boundary. Release all output
        // still held by the previous range before the new range can emit its
        // first event at this same clock edge.
        releasePlaybackOutput(algorithm, sample);
        algorithm.activeSelection = algorithm.selection;
        algorithm.rangeTransitionPending = false;
    }
    algorithm.playbackPulse = algorithm.activeSelection.startPulse;
    algorithm.playbackEventIndex =
        findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
}

void openPlaybackInterval(Algorithm& algorithm, uint64_t sample,
                          bool advanceOrdinal) {
    if (advanceOrdinal) {
        ++algorithm.playbackIntervalOrdinal;
    }
    algorithm.playbackIntervalStartSample = sample;
    algorithm.playbackIntervalOpen = true;
    scheduleNextPlaybackEvent(algorithm);
    refreshPendingEndingSchedule(algorithm);
    dispatchPlaybackEvents(algorithm, sample, false);
}

void beginPlaybackInterval(Algorithm& algorithm, uint64_t sample) {
    openPlaybackInterval(algorithm, sample, true);
}

void activatePendingPlayback(Algorithm& algorithm, uint64_t sample) {
    const bool resumeSavedInterval =
        algorithm.transportState == kTransportPlaying &&
        algorithm.playbackIntervalOpen && algorithm.playbackPositionValid;
    if (!algorithm.playbackPositionValid) {
        algorithm.playbackPulse = algorithm.activeSelection.startPulse;
        algorithm.playbackEventIndex =
            findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
        algorithm.playbackPositionValid = true;
    }
    algorithm.transportState = kTransportPlaying;
    // A loaded active preset retains the logical interval, event cursor, and
    // pending-ending ordinal from its save boundary. The second fresh clock
    // pulse supplies a new physical interval origin; it must not advance the
    // logical interval before the unconsumed events and endings are replayed.
    openPlaybackInterval(algorithm, sample, !resumeSavedInterval);
}

void beginOrAdvancePlayback(Algorithm& algorithm, uint64_t sample) {
    if (algorithm.transportState == kTransportArmed) {
        activatePendingPlayback(algorithm, sample);
    } else if (algorithm.transportState == kTransportPlaying) {
        advancePlaybackPulse(algorithm, sample);
        beginPlaybackInterval(algorithm, sample);
    }
}

void handleClockEdge(Algorithm& algorithm, uint64_t sample) {
    ++algorithm.state.clockEdges;
    ++algorithm.currentPulse;
    if (algorithm.captureEnabled && algorithm.eventCount != 0 &&
        algorithm.currentPulse != ~static_cast<uint64_t>(0)) {
        const uint64_t captureEnd = algorithm.currentPulse + 1U;
        if (captureEnd > algorithm.historyEndPulseExclusive) {
            algorithm.historyEndPulseExclusive = captureEnd;
        }
    }

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
        if (algorithm.transportState == kTransportArmed ||
            algorithm.transportState == kTransportPlaying ||
            algorithm.transportState == kTransportClockLossPaused) {
            activatePendingPlayback(algorithm, sample);
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
        if (algorithm.transportState == kTransportPlaying ||
            algorithm.transportState == kTransportArmed) {
            releasePlaybackOutput(algorithm, sample);
            algorithm.transportState = kTransportClockLossPaused;
        }
        algorithm.clockRunning = false;
        algorithm.haveAcquisitionPulse = false;
        algorithm.lastClockIntervalSamples = 0;
        algorithm.playbackIntervalOpen = false;
        algorithm.playbackNextEventScheduled = false;
        clearClockAverage(algorithm);
    }
}

void handleResetEdge(Algorithm& algorithm, uint64_t sample) {
    ++algorithm.state.resetEdges;
    releasePlaybackOutput(algorithm, sample);
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;

    if (!algorithm.selectionValid) {
        algorithm.playbackPositionValid = false;
        algorithm.transportState = kTransportStopped;
        return;
    }

    const PulseRange& resetRange =
        algorithm.activeSelectionValid &&
                algorithm.transportState != kTransportStopped
            ? algorithm.activeSelection
            : algorithm.selection;
    algorithm.playbackPulse = resetRange.startPulse;
    algorithm.playbackEventIndex =
        findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
    algorithm.playbackPositionValid = true;
    if (algorithm.transportState == kTransportPlaying) {
        algorithm.transportState = kTransportArmed;
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
        const bool resetHigh =
            resetFrames != NULL && resetFrames[frame] > kGateThresholdVolts;
        const bool clockEdge = clockHigh && !algorithm->clockHigh;
        const bool resetEdge = resetHigh && !algorithm->resetHigh;
        algorithm->clockHigh = clockHigh;
        algorithm->resetHigh = resetHigh;

        // Reset is intentionally ordered before clock at the same frame. It
        // releases old output ownership and repositions first, so the clock can
        // then open (and only then emit) the selected loop's first beat.
        if (resetEdge) {
            handleResetEdge(*algorithm, algorithm->sampleCursor);
        }
        if (clockEdge) {
            handleClockEdge(*algorithm, algorithm->sampleCursor);
        } else {
            detectClockLoss(*algorithm, algorithm->sampleCursor);
        }
        if (algorithm->clockRunning) {
            dispatchPlaybackEvents(*algorithm, algorithm->sampleCursor,
                                   false);
        }
        ++algorithm->sampleCursor;
    }
}

void emergencySilence(Algorithm& algorithm) {
    // Manual panic can be reached while capture is active; use the established
    // finite capture-stop path so emergency silence always leaves it paused.
    finalizeCapture(algorithm);

    const uint32_t destination = playbackDestinationMask(algorithm);
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        const uint8_t status = static_cast<uint8_t>(0xb0U | channel);
        nt_host::sendMidi3(destination, status, 120U, 0U,
                           algorithm.sampleCursor);
        nt_host::sendMidi3(destination, status, 123U, 0U,
                           algorithm.sampleCursor);
    }

    // Panic is a transport stop, not a reset. Discard output ownership and
    // scheduled work while preserving the saved loop position for an explicit
    // restart through the ordinary playback-entry path.
    algorithm.playbackOutputState = PlaybackOutputState();
    clearPendingPlaybackEndings(algorithm);
    algorithm.transportState = kTransportStopped;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
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

    const bool emergencyController =
        (byte0 & 0xf0U) == 0xb0U && (byte1 == 120U || byte1 == 123U);
    if (emergencyController &&
        algorithm->transportState != kTransportStopped) {
        emergencySilence(*algorithm);
        return;
    }

    if (!algorithm->captureEnabled || !algorithm->clockRunning) {
        return;
    }

    RecordedEvent event = {
        algorithm->currentPulse,
        0,
        boundedInterval(algorithm->lastClockIntervalSamples),
        {byte0, byte1, byte2},
        static_cast<uint8_t>((byte0 & 0xf0U) == 0xd0U ? 2U : 3U),
        0,
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
    trackRecordedState(*algorithm, event);
}

uint32_t pulsesPerDisplayedBeat(const Algorithm& algorithm) {
    int value = 0;
    if (algorithm.v != NULL) {
        value = algorithm.v[kParameterPulsesPerDisplayedBeat];
    }
    if (value < 0 || value >= static_cast<int>(
                                ARRAY_SIZE(kPulsesPerDisplayedBeatValues))) {
        value = 0;
    }
    return kPulsesPerDisplayedBeatValues[value];
}

bool retainedTimelineBounds(const Algorithm& algorithm, uint64_t& start,
                            uint64_t& end) {
    if (algorithm.eventCount == 0) {
        start = 0;
        end = 0;
        return false;
    }
    start = algorithm.recordingEvents[algorithm.eventHead].pulse;
    end = algorithm.historyEndPulseExclusive;
    return end > start;
}

uint64_t scaleTimelineDistance(uint64_t distance, uint32_t numerator,
                               uint32_t denominator) {
    uint32_t remainder = 0;
    const uint64_t quotient =
        divideUnsigned64By32(distance, denominator, remainder);
    const uint32_t scaledRemainder =
        (remainder * numerator + denominator / 2U) / denominator;
    return quotient * numerator + scaledRemainder;
}

uint32_t normalizedTimelinePot(float value) {
    if (!(value >= 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return kTimelineCoordinateMaximum;
    }
    return static_cast<uint32_t>(
        value * static_cast<float>(kTimelineCoordinateMaximum) + 0.5f);
}

uint64_t pulseFromPot(uint64_t minimum, uint64_t maximum, float pot) {
    if (maximum <= minimum) {
        return minimum;
    }
    return minimum + scaleTimelineDistance(
                         maximum - minimum, normalizedTimelinePot(pot),
                         kTimelineCoordinateMaximum);
}

uint64_t retainedTimelineIntervals(const Algorithm& algorithm) {
    uint64_t start = 0;
    uint64_t end = 0;
    return retainedTimelineBounds(algorithm, start, end) ? end - start : 0;
}

uint64_t timelineVisibleIntervals(const Algorithm& algorithm,
                                  uint64_t retained) {
    if (algorithm.timelineShowAll) {
        return retained;
    }
    return algorithm.timelineVisiblePulses < retained
               ? algorithm.timelineVisiblePulses
               : retained;
}

void timelineViewBounds(const Algorithm& algorithm, uint64_t& start,
                        uint64_t& end) {
    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(algorithm, retainedStart, retainedEnd)) {
        start = 0;
        end = 0;
        return;
    }

    if (algorithm.timelineShowAll) {
        start = retainedStart;
        end = retainedEnd;
        return;
    }

    const uint64_t retained = retainedEnd - retainedStart;
    const uint64_t visible =
        timelineVisibleIntervals(algorithm, retained);
    const uint64_t maximumScroll = retained - visible;
    const uint64_t scroll = algorithm.timelineScrollPulses < maximumScroll
                                ? algorithm.timelineScrollPulses
                                : maximumScroll;
    end = retainedEnd - scroll;
    start = end - visible;
}

bool currentRangeMotionPair(const Algorithm& algorithm,
                            RangeMotionBounds& bounds,
                            RangeMotionSelection& selection) {
    if (!algorithm.selectionValid ||
        !retainedTimelineBounds(algorithm, bounds.historyStart,
                                bounds.historyEnd)) {
        return false;
    }
    selection.start = algorithm.selection.startPulse;
    selection.end = algorithm.selection.endPulse;
    return true;
}

void rememberRangeMotionDomain(Algorithm& algorithm,
                               const RangeMotionBounds& bounds) {
    algorithm.rangeMotionHistoryStart = bounds.historyStart;
    algorithm.rangeMotionHistoryEnd = bounds.historyEnd;
    algorithm.rangeMotionDomainKnown = true;
}

// Called once before UI control processing. It establishes from the selected
// pair, notices retained-domain changes without moving that pair, and consumes
// setupUi's request to sample the actual physical pot without translation.
bool prepareRangeMotion(Algorithm& algorithm, float physicalPosition) {
    RangeMotionBounds bounds = {};
    RangeMotionSelection selection = {};
    if (!currentRangeMotionPair(algorithm, bounds, selection)) {
        return false;
    }

    bool seededWithoutMotion = false;
    if (!algorithm.rangeMotion.established) {
        if (establishRangeMotion(algorithm.rangeMotion, bounds, selection,
                                 physicalPosition)) {
            rememberRangeMotionDomain(algorithm, bounds);
            seededWithoutMotion = true;
        }
    } else if (!algorithm.rangeMotionDomainKnown ||
               bounds.historyStart != algorithm.rangeMotionHistoryStart ||
               bounds.historyEnd != algorithm.rangeMotionHistoryEnd) {
        rebaseRangeMotion(algorithm.rangeMotion, bounds, selection);
        rememberRangeMotionDomain(algorithm, bounds);
    }

    if (algorithm.rangeMotion.established &&
        algorithm.rangeMotionNeedsPhysicalSeed) {
        seedRangeMotionPhysical(algorithm.rangeMotion, physicalPosition);
        algorithm.rangeMotionNeedsPhysicalSeed = false;
        seededWithoutMotion = true;
    }
    return seededWithoutMotion;
}

void rebaseRangeMotionAfterEdit(Algorithm& algorithm,
                                float physicalPosition) {
    RangeMotionBounds bounds = {};
    RangeMotionSelection selection = {};
    if (!currentRangeMotionPair(algorithm, bounds, selection)) {
        return;
    }
    if (!rebaseRangeMotion(algorithm.rangeMotion, bounds, selection)) {
        establishRangeMotion(algorithm.rangeMotion, bounds, selection,
                             physicalPosition);
    }
    if (algorithm.rangeMotion.established) {
        rememberRangeMotionDomain(algorithm, bounds);
        algorithm.rangeMotionNeedsPhysicalSeed = false;
    }
}

bool ensureTimelineSelection(_NT_algorithm* self, Algorithm& algorithm) {
    if (algorithm.selectionValid) {
        return true;
    }
    uint64_t start = 0;
    uint64_t end = 0;
    return retainedTimelineBounds(algorithm, start, end) &&
           setPulseSelection(self, start, end);
}

void moveTimelineBoundary(_NT_algorithm* self, Algorithm& algorithm,
                          bool startBoundary, uint64_t requested,
                          float rightPotPhysical) {
    if (!ensureTimelineSelection(self, algorithm)) {
        return;
    }

    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(algorithm, retainedStart, retainedEnd)) {
        return;
    }

    uint64_t start = algorithm.selection.startPulse;
    uint64_t end = algorithm.selection.endPulse;
    if (startBoundary) {
        const uint64_t maximum = end - 1U;
        start = requested < retainedStart
                    ? retainedStart
                    : (requested > maximum ? maximum : requested);
    } else {
        const uint64_t minimum = start + 1U;
        end = requested < minimum
                  ? minimum
                  : (requested > retainedEnd ? retainedEnd : requested);
    }
    if (start == algorithm.selection.startPulse &&
        end == algorithm.selection.endPulse) {
        return;
    }
    if (setPulseSelection(self, start, end)) {
        algorithm.selectionFineTarget =
            startBoundary ? kSelectionFineTargetStart
                          : kSelectionFineTargetEnd;
        rebaseRangeMotionAfterEdit(algorithm, rightPotPhysical);
    }
}

void moveTimelineRange(_NT_algorithm* self, Algorithm& algorithm,
                       float physicalPosition) {
    RangeMotionBounds bounds = {};
    RangeMotionSelection selection = {};
    if (!currentRangeMotionPair(algorithm, bounds, selection)) {
        return;
    }
    if (!algorithm.rangeMotion.established &&
        !establishRangeMotion(algorithm.rangeMotion, bounds, selection,
                              physicalPosition)) {
        return;
    }
    rememberRangeMotionDomain(algorithm, bounds);
    if (moveRangeMotion(algorithm.rangeMotion, bounds, selection,
                        physicalPosition) &&
        setPulseSelection(self, selection.start, selection.end)) {
        algorithm.selectionFineTarget = kSelectionFineTargetRange;
    }
}

void nudgeTimelineRange(_NT_algorithm* self, Algorithm& algorithm,
                        int delta, float rightPotPhysical) {
    if (delta == 0 || !algorithm.selectionValid) {
        return;
    }
    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(algorithm, retainedStart, retainedEnd) ||
        algorithm.selection.startPulse < retainedStart ||
        algorithm.selection.endPulse > retainedEnd ||
        algorithm.selection.startPulse >= algorithm.selection.endPulse) {
        return;
    }

    const uint64_t length = algorithm.selection.endPulse -
                            algorithm.selection.startPulse;
    const uint64_t maximumStart = retainedEnd - length;
    const uint64_t amount = static_cast<uint32_t>(
        delta > 0 ? delta : -delta);
    uint64_t start = algorithm.selection.startPulse;
    if (delta > 0) {
        start = amount > maximumStart - start ? maximumStart : start + amount;
    } else {
        start = amount > start - retainedStart ? retainedStart : start - amount;
    }
    if (start == algorithm.selection.startPulse) {
        return;
    }
    if (setPulseSelection(self, start, start + length)) {
        rebaseRangeMotionAfterEdit(algorithm, rightPotPhysical);
    }
}

void adjustTimelineSelection(_NT_algorithm* self, Algorithm& algorithm,
                             int delta, float rightPotPhysical) {
    if (delta == 0) {
        return;
    }
    if (algorithm.selectionFineTarget == kSelectionFineTargetRange) {
        nudgeTimelineRange(self, algorithm, delta, rightPotPhysical);
        return;
    }
    if (!ensureTimelineSelection(self, algorithm)) {
        return;
    }

    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(algorithm, retainedStart, retainedEnd)) {
        return;
    }

    const bool startBoundary =
        algorithm.selectionFineTarget == kSelectionFineTargetStart;
    uint64_t requested = startBoundary ? algorithm.selection.startPulse
                                       : algorithm.selection.endPulse;
    int steps = delta > 0 ? delta : -delta;
    while (steps-- > 0) {
        if (delta > 0 && requested < retainedEnd) {
            ++requested;
        } else if (delta < 0 && requested > retainedStart) {
            --requested;
        }
    }
    moveTimelineBoundary(self, algorithm, startBoundary, requested,
                         rightPotPhysical);
}

void scrollTimeline(Algorithm& algorithm, int delta) {
    if (algorithm.timelineShowAll) {
        algorithm.timelineScrollPulses = 0;
        return;
    }

    const uint64_t retained = retainedTimelineIntervals(algorithm);
    const uint64_t visible = timelineVisibleIntervals(algorithm, retained);
    const uint64_t maximumScroll = retained - visible;
    if (algorithm.timelineScrollPulses > maximumScroll) {
        algorithm.timelineScrollPulses = maximumScroll;
    }
    if (delta > 0) {
        const uint64_t amount = static_cast<uint32_t>(delta);
        algorithm.timelineScrollPulses =
            amount > maximumScroll - algorithm.timelineScrollPulses
                ? maximumScroll
                : algorithm.timelineScrollPulses + amount;
    } else if (delta < 0) {
        const uint64_t amount = static_cast<uint32_t>(-delta);
        algorithm.timelineScrollPulses =
            amount > algorithm.timelineScrollPulses
                ? 0
                : algorithm.timelineScrollPulses - amount;
    }
}

uint32_t zoomStepCount(uint64_t start, uint64_t target, bool zoomOut) {
    uint32_t count = 0;
    while (start != target) {
        if (zoomOut) {
            start = start > target / 2U ? target : start * 2U;
        } else {
            const uint64_t halved = start / 2U;
            start = halved < target ? target : halved;
        }
        ++count;
    }
    return count;
}

uint32_t relativeZoomSteps(uint32_t distance, uint32_t available,
                           uint32_t stepCount) {
    if (distance == 0U || available == 0U || stepCount == 0U) {
        return 0U;
    }
    uint32_t remainder = 0;
    return static_cast<uint32_t>(divideUnsigned64By32(
        static_cast<uint64_t>(distance) * stepCount + available - 1U,
        available, remainder));
}

uint64_t applyZoomSteps(uint64_t span, uint64_t limit, uint32_t steps,
                        bool zoomOut) {
    while (steps-- != 0U && span != limit) {
        if (zoomOut) {
            span = span > limit / 2U ? limit : span * 2U;
        } else {
            const uint64_t halved = span / 2U;
            span = halved < limit ? limit : halved;
        }
    }
    return span;
}

void beginTimelineZoom(Algorithm& algorithm, uint32_t coordinate) {
    const uint64_t retained = retainedTimelineIntervals(algorithm);
    algorithm.rightPotZoomActive = true;
    algorithm.zoomPressCoordinate = coordinate;
    algorithm.zoomPressVisiblePulses =
        timelineVisibleIntervals(algorithm, retained);
    algorithm.zoomPressManualVisiblePulses =
        algorithm.timelineVisiblePulses;
    algorithm.zoomPressShowAll = algorithm.timelineShowAll;
}

void updateTimelineZoom(Algorithm& algorithm, uint32_t coordinate) {
    if (!algorithm.rightPotZoomActive) {
        return;
    }

    const uint64_t retained = retainedTimelineIntervals(algorithm);
    if (retained == 0U) {
        return;
    }
    uint64_t startSpan =
        algorithm.zoomPressVisiblePulses < retained
            ? algorithm.zoomPressVisiblePulses
            : retained;
    if (startSpan == 0U) {
        startSpan = retained;
    }
    const uint64_t minimum = retained < kTimelineMinimumVisiblePulses
                                 ? retained
                                 : kTimelineMinimumVisiblePulses;

    if (coordinate == algorithm.zoomPressCoordinate) {
        algorithm.timelineShowAll = algorithm.zoomPressShowAll;
        if (algorithm.zoomPressShowAll) {
            algorithm.timelineScrollPulses = 0U;
        } else {
            algorithm.timelineVisiblePulses =
                algorithm.zoomPressManualVisiblePulses;
            scrollTimeline(algorithm, 0);
        }
        return;
    }

    if (coordinate > algorithm.zoomPressCoordinate) {
        if (startSpan >= retained) {
            algorithm.timelineShowAll = true;
            algorithm.timelineScrollPulses = 0;
            return;
        }
        const uint32_t totalSteps =
            zoomStepCount(startSpan, retained, true);
        const uint32_t steps = relativeZoomSteps(
            coordinate - algorithm.zoomPressCoordinate,
            kTimelineCoordinateMaximum - algorithm.zoomPressCoordinate,
            totalSteps);
        const uint64_t span =
            applyZoomSteps(startSpan, retained, steps, true);
        if (span == retained) {
            algorithm.timelineShowAll = true;
            algorithm.timelineScrollPulses = 0;
        } else {
            algorithm.timelineShowAll = false;
            algorithm.timelineVisiblePulses = span;
            scrollTimeline(algorithm, 0);
        }
        return;
    }

    if (startSpan <= minimum) {
        algorithm.timelineShowAll = false;
        algorithm.timelineVisiblePulses = kTimelineMinimumVisiblePulses;
        scrollTimeline(algorithm, 0);
        return;
    }
    const uint32_t totalSteps =
        zoomStepCount(startSpan, minimum, false);
    const uint32_t steps = relativeZoomSteps(
        algorithm.zoomPressCoordinate - coordinate,
        algorithm.zoomPressCoordinate, totalSteps);
    const uint64_t span =
        applyZoomSteps(startSpan, minimum, steps, false);
    if (span < retained) {
        algorithm.timelineShowAll = false;
        algorithm.timelineVisiblePulses = span;
        scrollTimeline(algorithm, 0);
    }
}

uint32_t hasCustomUi(_NT_algorithm*) {
    return kNT_potL | kNT_potC | kNT_potR | kNT_potButtonR |
           kNT_encoderL | kNT_encoderR | kNT_encoderButtonL |
           kNT_encoderButtonR;
}

void updateRightEncoderPanic(Algorithm& algorithm, const _NT_uiData& data) {
    const bool held = (data.controls & kNT_encoderButtonR) != 0U;
    const bool wasHeld = (data.lastButtons & kNT_encoderButtonR) != 0U;
    if (!held) {
        algorithm.rightEncoderHoldActive = false;
        algorithm.rightEncoderPanicFired = false;
        return;
    }
    if (!wasHeld || !algorithm.rightEncoderHoldActive) {
        algorithm.rightEncoderHoldActive = true;
        algorithm.rightEncoderPanicFired = false;
        algorithm.rightEncoderHoldStartSample = algorithm.sampleCursor;
        return;
    }
    if (!algorithm.rightEncoderPanicFired && NT_globals.sampleRate != 0U &&
        algorithm.sampleCursor - algorithm.rightEncoderHoldStartSample >=
            NT_globals.sampleRate) {
        emergencySilence(algorithm);
        algorithm.rightEncoderPanicFired = true;
    }
}

void toggleTimelinePlayback(_NT_algorithm* self, Algorithm& algorithm) {
    if (algorithm.transportState != kTransportStopped) {
        stopPlayback(self);
        return;
    }

    // Refuse the gesture before startPlayback's capture-finalization path so
    // an invalid selection makes the button a true no-op.
    PulseRange range = {};
    if (acquirePlaybackSelection(self, range)) {
        startPlayback(self);
    }
}

void customUi(_NT_algorithm* self, const _NT_uiData& data) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    if (data.controls != 0 || data.encoders[0] != 0 || data.encoders[1] != 0) {
        ++algorithm->state.uiChanges;
    }

    bool suppressRightPotTranslation =
        prepareRangeMotion(*algorithm, data.pots[2]);

    // Buttons are processed before pots and rotations. In particular, the
    // pot-3 release sample becomes the next relative baseline before a changed
    // pot bit in the same callback can be interpreted as translation.
    const bool leftEncoderPressed =
        (data.controls & kNT_encoderButtonL) != 0U &&
        (data.lastButtons & kNT_encoderButtonL) == 0U;
    if (leftEncoderPressed) {
        toggleTimelinePlayback(self, *algorithm);
    }
    updateRightEncoderPanic(*algorithm, data);

    const bool rightPotHeld =
        (data.controls & kNT_potButtonR) != 0U;
    const bool rightPotWasHeld =
        (data.lastButtons & kNT_potButtonR) != 0U;
    if (rightPotHeld && !rightPotWasHeld) {
        seedRangeMotionPhysical(algorithm->rangeMotion, data.pots[2]);
        beginTimelineZoom(*algorithm, normalizedTimelinePot(data.pots[2]));
        suppressRightPotTranslation = true;
    } else if (!rightPotHeld && rightPotWasHeld) {
        seedRangeMotionPhysical(algorithm->rangeMotion, data.pots[2]);
        algorithm->rightPotZoomActive = false;
        suppressRightPotTranslation = true;
    }

    if ((data.controls & kNT_potL) != 0U &&
        ensureTimelineSelection(self, *algorithm)) {
        moveTimelineBoundary(
            self, *algorithm, true,
            pulseFromPot(
                algorithm->recordingEvents[algorithm->eventHead].pulse,
                algorithm->selection.endPulse - 1U, data.pots[0]),
            data.pots[2]);
    }
    if ((data.controls & kNT_potC) != 0U &&
        ensureTimelineSelection(self, *algorithm)) {
        moveTimelineBoundary(
            self, *algorithm, false,
            pulseFromPot(algorithm->selection.startPulse + 1U,
                         algorithm->historyEndPulseExclusive, data.pots[1]),
            data.pots[2]);
    }

    if (rightPotHeld) {
        if (rightPotWasHeld) {
            updateTimelineZoom(*algorithm,
                               normalizedTimelinePot(data.pots[2]));
        }
        // Held movement is zoom-only, but every actual sample advances the
        // physical baseline while logical catch-up and residual remain intact.
        seedRangeMotionPhysical(algorithm->rangeMotion, data.pots[2]);
    } else if ((data.controls & kNT_potR) != 0U &&
               !suppressRightPotTranslation) {
        moveTimelineRange(self, *algorithm, data.pots[2]);
    }

    scrollTimeline(*algorithm, data.encoders[0]);
    adjustTimelineSelection(self, *algorithm, data.encoders[1], data.pots[2]);
}

void setupUi(_NT_algorithm* self, _NT_float3& pots) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    pots[0] = 0.0f;
    pots[1] = 1.0f;
    pots[2] = 0.0f;
    if (algorithm == NULL) {
        return;
    }
    algorithm->rangeMotionNeedsPhysicalSeed = true;
    if (!algorithm->selectionValid) {
        return;
    }

    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(*algorithm, retainedStart, retainedEnd) ||
        algorithm->selection.startPulse < retainedStart ||
        algorithm->selection.endPulse > retainedEnd ||
        algorithm->selection.startPulse >= algorithm->selection.endPulse) {
        return;
    }
    const uint64_t startRange = algorithm->selection.endPulse - 1U -
                                retainedStart;
    const uint64_t endRange = retainedEnd -
                              (algorithm->selection.startPulse + 1U);
    if (startRange != 0U && startRange <= 0xffffffffULL) {
        pots[0] = static_cast<float>(static_cast<uint32_t>(
                      algorithm->selection.startPulse - retainedStart)) /
                  static_cast<float>(static_cast<uint32_t>(startRange));
    }
    if (endRange != 0U && endRange <= 0xffffffffULL) {
        pots[1] = static_cast<float>(static_cast<uint32_t>(
                      algorithm->selection.endPulse -
                      (algorithm->selection.startPulse + 1U))) /
                  static_cast<float>(static_cast<uint32_t>(endRange));
    }

    const RangeMotionBounds bounds = {retainedStart, retainedEnd};
    const RangeMotionSelection selection = {
        algorithm->selection.startPulse,
        algorithm->selection.endPulse,
    };
    double logicalPosition = 0.0;
    if (rangeMotionLogicalPosition(bounds, selection, logicalPosition)) {
        pots[2] = static_cast<float>(logicalPosition);
    }
}

uint32_t scaleTimelineOffset(uint64_t offset, uint64_t span) {
    if (offset >= span) {
        return 247U;
    }

    // Find floor(offset * 247 / span) without forming the overflowing
    // product.  For candidate x, ceil(x * span / 247) is the first pulse
    // offset that maps to x.  Splitting span by 247 keeps every intermediate
    // within uint64 while the eight-step search stays bounded.
    uint32_t spanRemainder = 0U;
    const uint64_t spanQuotient =
        divideUnsigned64By32(span, 247U, spanRemainder);
    uint32_t lower = 0U;
    uint32_t upper = 246U;
    while (lower < upper) {
        const uint32_t candidate = (lower + upper + 1U) / 2U;
        const uint32_t remainderProduct = candidate * spanRemainder;
        const uint64_t firstOffset =
            spanQuotient * candidate +
            (remainderProduct + 246U) / 247U;
        if (firstOffset <= offset) {
            lower = candidate;
        } else {
            upper = candidate - 1U;
        }
    }
    return lower;
}

int pulseTimelineX(uint64_t pulse, uint64_t viewStart, uint64_t viewEnd) {
    if (viewEnd <= viewStart || pulse <= viewStart) {
        return 4;
    }
    if (pulse >= viewEnd) {
        return 251;
    }
    return 4 + static_cast<int>(
                   scaleTimelineOffset(pulse - viewStart,
                                       viewEnd - viewStart));
}

struct TimelineDrawSnapshot {
    uint64_t viewStart;
    uint64_t viewEnd;
    uint64_t retainedIntervals;
    uint64_t playbackPulse;
    PulseRange selection;
    uint32_t pulsesPerBeat;
    bool selectionValid;
    bool headEligible;
};

TimelineDrawSnapshot observeTimelineDraw(const Algorithm& algorithm) {
    TimelineDrawSnapshot observed = {};
    timelineViewBounds(algorithm, observed.viewStart, observed.viewEnd);
    observed.retainedIntervals = retainedTimelineIntervals(algorithm);
    observed.playbackPulse = algorithm.playbackPulse;
    observed.selection = algorithm.selection;
    observed.pulsesPerBeat = pulsesPerDisplayedBeat(algorithm);
    observed.selectionValid = algorithm.selectionValid;
    observed.headEligible =
        algorithm.transportState == kTransportPlaying &&
        algorithm.clockRunning && algorithm.playbackPositionValid &&
        algorithm.playbackIntervalOpen && algorithm.activeSelectionValid;
    return observed;
}

void drawSelectionBracket(uint64_t pulse, uint64_t viewStart,
                          uint64_t viewEnd, bool start) {
    if (pulse < viewStart || pulse > viewEnd) {
        return;
    }
    const int x = pulseTimelineX(pulse, viewStart, viewEnd);
    nt_host::drawShape(kNT_line, x, 16, x, 56, 15);
    nt_host::drawShape(kNT_line, x, 16, x + (start ? 4 : -4), 16, 15);
    nt_host::drawShape(kNT_line, x, 56, x + (start ? 4 : -4), 56, 15);
}

bool draw(_NT_algorithm* self) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return true;
    }

    const TimelineDrawSnapshot observed = observeTimelineDraw(*algorithm);
    char amount[17];
    formatMusicalDuration(amount, sizeof(amount),
                          observed.retainedIntervals,
                          observed.pulsesPerBeat);
    char availability[40];
    BoundedText availabilityText(availability, sizeof(availability));
    availabilityText.append("Avail ");
    availabilityText.append(amount);
    availabilityText.append("  ");
    availabilityText.appendUnsigned64(observed.pulsesPerBeat);
    availabilityText.append("ppb");
    nt_host::drawTinyText(0, 7, availability);

    char length[32];
    BoundedText lengthText(length, sizeof(length));
    lengthText.append("Len ");
    if (observed.selectionValid) {
        formatMusicalDuration(
            amount, sizeof(amount),
            observed.selection.endPulse - observed.selection.startPulse,
            observed.pulsesPerBeat);
        lengthText.append(amount);
    } else {
        lengthText.append("--");
    }
    nt_host::drawTinyText(176, 7, length);

    nt_host::drawShape(kNT_line, 4, 52, 251, 52, 5);
    if (observed.viewEnd > observed.viewStart) {
        uint32_t eventIndex =
            findPlaybackEventIndex(*algorithm, observed.viewStart);
        uint32_t drawnNotes = 0;
        while (eventIndex < algorithm->eventCount && drawnNotes < 256U) {
            const RecordedEvent* event = recordedEventByIndex(
                *algorithm, eventIndex++);
            if (event == NULL || event->pulse >= observed.viewEnd) {
                break;
            }
            if ((event->bytes[0] & 0xf0U) != 0x90U ||
                event->bytes[2] == 0U) {
                continue;
            }
            const int x = pulseTimelineX(event->pulse, observed.viewStart,
                                         observed.viewEnd);
            const int y = 47 - static_cast<int>(event->bytes[1]) * 24 / 127;
            nt_host::drawShape(kNT_line, x, y, x, 51, 9);
            ++drawnNotes;
        }
        if (observed.headEligible &&
            observed.playbackPulse >= observed.viewStart &&
            observed.playbackPulse < observed.viewEnd) {
            const int x = pulseTimelineX(observed.playbackPulse,
                                         observed.viewStart,
                                         observed.viewEnd);
            nt_host::drawShape(kNT_line, x, 17, x, 55, 12);
        }
        if (observed.selectionValid) {
            drawSelectionBracket(observed.selection.startPulse,
                                 observed.viewStart, observed.viewEnd, true);
            drawSelectionBracket(observed.selection.endPulse,
                                 observed.viewStart, observed.viewEnd, false);
        }
    }
    return true;
}

const uint32_t kPresetVersion = 1U;
const uint32_t kPresetHexChunkBytes = 512U;

char hexDigit(uint8_t value) {
    return static_cast<char>(value < 10U ? '0' + value
                                        : 'a' + value - 10U);
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

void serialiseBytes(_NT_jsonStream& stream, const char* name,
                    const void* bytes, uint32_t byteCount) {
    const uint8_t* source = static_cast<const uint8_t*>(bytes);
    char encoded[kPresetHexChunkBytes * 2U + 1U];
    stream.addMemberName(name);
    stream.openArray();
    for (uint32_t offset = 0; offset < byteCount;) {
        const uint32_t remaining = byteCount - offset;
        const uint32_t chunk = remaining < kPresetHexChunkBytes
                                   ? remaining
                                   : kPresetHexChunkBytes;
        for (uint32_t index = 0; index < chunk; ++index) {
            const uint8_t value = source[offset + index];
            encoded[index * 2U] = hexDigit(value >> 4U);
            encoded[index * 2U + 1U] = hexDigit(value & 0x0fU);
        }
        encoded[chunk * 2U] = '\0';
        stream.addString(encoded);
        offset += chunk;
    }
    stream.closeArray();
}

bool deserialiseBytes(_NT_jsonParse& parse, void* bytes,
                      uint32_t byteCount) {
    uint8_t* destination = static_cast<uint8_t*>(bytes);
    int chunks = 0;
    if (!parse.numberOfArrayElements(chunks) || chunks < 0) {
        return false;
    }
    uint32_t offset = 0;
    for (int chunk = 0; chunk < chunks; ++chunk) {
        const char* encoded = NULL;
        if (!parse.string(encoded) || encoded == NULL) {
            return false;
        }
        uint32_t index = 0;
        while (encoded[index] != '\0') {
            if (encoded[index + 1U] == '\0' || offset >= byteCount) {
                return false;
            }
            const int high = hexValue(encoded[index]);
            const int low = hexValue(encoded[index + 1U]);
            if (high < 0 || low < 0) {
                return false;
            }
            destination[offset++] =
                static_cast<uint8_t>((high << 4U) | low);
            index += 2U;
        }
    }
    return offset == byteCount;
}

void addUnsigned32(_NT_jsonStream& stream, uint32_t value) {
    stream.addNumber(static_cast<int>(value & 0xffffU));
    stream.addNumber(static_cast<int>(value >> 16U));
}

void addUnsigned64(_NT_jsonStream& stream, uint64_t value) {
    for (uint32_t shift = 0; shift < 64U; shift += 16U) {
        stream.addNumber(static_cast<int>((value >> shift) & 0xffffU));
    }
}

bool parseUnsigned32(_NT_jsonParse& parse, uint32_t& value) {
    int low = 0;
    int high = 0;
    if (!parse.number(low) || !parse.number(high) || low < 0 || low > 65535 ||
        high < 0 || high > 65535) {
        return false;
    }
    value = static_cast<uint32_t>(low) |
            (static_cast<uint32_t>(high) << 16U);
    return true;
}

bool parseUnsigned64(_NT_jsonParse& parse, uint64_t& value) {
    value = 0;
    for (uint32_t shift = 0; shift < 64U; shift += 16U) {
        int part = 0;
        if (!parse.number(part) || part < 0 || part > 65535) {
            return false;
        }
        value |= static_cast<uint64_t>(static_cast<uint32_t>(part)) << shift;
    }
    return true;
}

const RecordedEvent& serialisedEventAt(const Algorithm& algorithm,
                                       uint32_t oldestFirstIndex) {
    uint32_t ringIndex = algorithm.eventHead + oldestFirstIndex;
    if (ringIndex >= algorithm.eventCapacity) {
        ringIndex -= algorithm.eventCapacity;
    }
    return algorithm.recordingEvents[ringIndex];
}

void serialiseEvents(_NT_jsonStream& stream, const Algorithm& algorithm) {
    char encoded[kPresetHexChunkBytes * 2U + 1U];
    const uint64_t byteCount =
        static_cast<uint64_t>(algorithm.eventCount) * sizeof(RecordedEvent);
    stream.addMemberName("events");
    stream.openArray();
    uint32_t eventIndex = 0U;
    uint32_t eventByte = 0U;
    for (uint64_t offset = 0; offset < byteCount;) {
        const uint64_t remaining = byteCount - offset;
        const uint32_t chunk = remaining < kPresetHexChunkBytes
                                   ? static_cast<uint32_t>(remaining)
                                   : kPresetHexChunkBytes;
        for (uint32_t index = 0; index < chunk; ++index) {
            const uint8_t value = reinterpret_cast<const uint8_t*>(
                &serialisedEventAt(algorithm, eventIndex))[eventByte];
            encoded[index * 2U] = hexDigit(value >> 4U);
            encoded[index * 2U + 1U] = hexDigit(value & 0x0fU);
            if (++eventByte == sizeof(RecordedEvent)) {
                eventByte = 0U;
                ++eventIndex;
            }
        }
        encoded[chunk * 2U] = '\0';
        stream.addString(encoded);
        offset += chunk;
    }
    stream.closeArray();
}

bool deserialiseEvents(_NT_jsonParse& parse, Algorithm& algorithm) {
    int chunks = 0;
    if (!parse.numberOfArrayElements(chunks) || chunks < 0) {
        return false;
    }
    const uint64_t byteCount =
        static_cast<uint64_t>(algorithm.eventCount) * sizeof(RecordedEvent);
    uint64_t offset = 0;
    uint32_t eventIndex = 0U;
    uint32_t eventByte = 0U;
    for (int chunk = 0; chunk < chunks; ++chunk) {
        const char* encoded = NULL;
        if (!parse.string(encoded) || encoded == NULL) {
            return false;
        }
        uint32_t index = 0;
        while (encoded[index] != '\0') {
            if (encoded[index + 1U] == '\0' || offset >= byteCount) {
                return false;
            }
            const int high = hexValue(encoded[index]);
            const int low = hexValue(encoded[index + 1U]);
            if (high < 0 || low < 0) {
                return false;
            }
            uint32_t ringIndex = algorithm.eventHead + eventIndex;
            if (ringIndex >= algorithm.eventCapacity) {
                ringIndex -= algorithm.eventCapacity;
            }
            reinterpret_cast<uint8_t*>(
                &algorithm.recordingEvents[ringIndex])[eventByte] =
                    static_cast<uint8_t>((high << 4U) | low);
            if (++eventByte == sizeof(RecordedEvent)) {
                eventByte = 0U;
                ++eventIndex;
            }
            ++offset;
            index += 2U;
        }
    }
    return offset == byteCount;
}

uint32_t legacyTimelineVisiblePulses(const Algorithm& algorithm) {
    // The appended navigation member is authoritative for new readers. Keep
    // the original v1 slot valid for legacy readers without narrowing or
    // silently clamping an unlimited manual span to 256 pulses.
    return algorithm.timelineVisiblePulses >= kTimelineMinimumVisiblePulses &&
                   algorithm.timelineVisiblePulses <=
                       kTimelineLegacyMaximumVisiblePulses
               ? static_cast<uint32_t>(algorithm.timelineVisiblePulses)
               : static_cast<uint32_t>(kDefaultTimelineVisiblePulses);
}

void serialise(_NT_algorithm* self, _NT_jsonStream& stream) {
    const Algorithm* algorithm = static_cast<const Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }

    // NT invokes this synchronously inside one algorithm JSON object. The
    // callback never mutates live state, so every field and retained event is
    // read from the same callback-order boundary during capture or playback.
    stream.addMemberName("midibufferState");
    stream.openObject();
    stream.addMemberName("version");
    stream.addNumber(static_cast<int>(kPresetVersion));

    stream.addMemberName("u64");
    stream.openArray();
    addUnsigned64(stream, algorithm->sampleCursor);
    addUnsigned64(stream, algorithm->currentPulse);
    addUnsigned64(stream, algorithm->historyEndPulseExclusive);
    addUnsigned64(stream, algorithm->lastPulseSample);
    addUnsigned64(stream, algorithm->lastClockIntervalSamples);
    addUnsigned64(stream, algorithm->playbackPulse);
    addUnsigned64(stream, algorithm->playbackIntervalStartSample);
    addUnsigned64(stream, algorithm->playbackNextEventSample);
    addUnsigned64(stream, algorithm->playbackIntervalOrdinal);
    addUnsigned64(stream, algorithm->timelineScrollPulses);
    addUnsigned64(stream, algorithm->pendingNextEndingSample);
    addUnsigned64(stream, algorithm->rightEncoderHoldStartSample);
    addUnsigned64(stream, algorithm->selection.startPulse);
    addUnsigned64(stream, algorithm->selection.endPulse);
    addUnsigned64(stream, algorithm->activeSelection.startPulse);
    addUnsigned64(stream, algorithm->activeSelection.endPulse);
    stream.closeArray();

    stream.addMemberName("u32");
    stream.openArray();
    addUnsigned32(stream, algorithm->recordingBufferBytes);
    addUnsigned32(stream, algorithm->eventCapacity);
    addUnsigned32(stream, algorithm->eventHead);
    addUnsigned32(stream, algorithm->eventCount);
    addUnsigned32(stream, algorithm->clockIntervalSum);
    addUnsigned32(stream, algorithm->playbackEventIndex);
    addUnsigned32(stream, algorithm->clockIntervalWriteIndex);
    addUnsigned32(stream, algorithm->clockIntervalCount);
    addUnsigned32(stream, legacyTimelineVisiblePulses(*algorithm));
    stream.closeArray();

    stream.addMemberName("flags");
    stream.openArray();
    stream.addBoolean(algorithm->captureEnabled);
    stream.addBoolean(false);  // live external clock presence is not a preset
    stream.addBoolean(false);  // clock acquisition edge is physical input
    stream.addBoolean(false);  // current clock gate level is physical input
    stream.addBoolean(false);  // current reset gate level is physical input
    stream.addBoolean(algorithm->selectionValid);
    stream.addBoolean(algorithm->activeSelectionValid);
    stream.addBoolean(algorithm->rangeTransitionPending);
    stream.addBoolean(algorithm->playbackPositionValid);
    // These are logical scheduler state, not external clock state. Keeping the
    // saved interval open lets clock reacquisition rebase (rather than skip)
    // its unconsumed event cursor and pending-ending ordinal.
    stream.addBoolean(algorithm->playbackIntervalOpen);
    stream.addBoolean(algorithm->playbackNextEventScheduled);
    stream.addBoolean(algorithm->pendingNextEndingScheduled);
    stream.addBoolean(algorithm->selectionFineTarget ==
                      kSelectionFineTargetStart);
    stream.addBoolean(false);  // encoder button state is physical input
    stream.addBoolean(false);
    stream.addNumber(static_cast<int>(algorithm->transportState));
    stream.closeArray();

    // Approved v1 compatibility extension. The unchanged u32/flags fields
    // above let old complete presets keep loading and give legacy readers a
    // valid fallback. This authoritative member adds explicit view policy, an
    // exact uint64 manual span, and all three fine-target values.
    stream.addMemberName("navigation");
    stream.openArray();
    stream.addNumber(1);  // navigation representation revision
    stream.addBoolean(algorithm->timelineShowAll);
    addUnsigned64(stream, algorithm->timelineVisiblePulses);
    stream.addNumber(static_cast<int>(algorithm->selectionFineTarget));
    stream.closeArray();

    serialiseBytes(stream, "recorded", &algorithm->recordedState,
                   sizeof(algorithm->recordedState));
    serialiseBytes(stream, "output", &algorithm->playbackOutputState,
                   sizeof(algorithm->playbackOutputState));
    serialiseBytes(stream, "pending", &algorithm->pendingEndings,
                   sizeof(algorithm->pendingEndings));
    serialiseBytes(stream, "clockIntervals", algorithm->clockIntervals,
                   sizeof(algorithm->clockIntervals));
    serialiseEvents(stream, *algorithm);
    stream.closeObject();
}

bool parseU64State(_NT_jsonParse& parse, Algorithm& algorithm) {
    int count = 0;
    uint64_t value[16] = {};
    if (!parse.numberOfArrayElements(count) || count != 64) {
        return false;
    }
    for (uint32_t index = 0; index < ARRAY_SIZE(value); ++index) {
        if (!parseUnsigned64(parse, value[index])) {
            return false;
        }
    }
    algorithm.sampleCursor = value[0];
    algorithm.currentPulse = value[1];
    algorithm.historyEndPulseExclusive = value[2];
    algorithm.lastPulseSample = value[3];
    algorithm.lastClockIntervalSamples = value[4];
    algorithm.playbackPulse = value[5];
    algorithm.playbackIntervalStartSample = value[6];
    algorithm.playbackNextEventSample = value[7];
    algorithm.playbackIntervalOrdinal = value[8];
    algorithm.timelineScrollPulses = value[9];
    algorithm.pendingNextEndingSample = value[10];
    algorithm.rightEncoderHoldStartSample = value[11];
    algorithm.selection.startPulse = value[12];
    algorithm.selection.endPulse = value[13];
    algorithm.activeSelection.startPulse = value[14];
    algorithm.activeSelection.endPulse = value[15];
    return true;
}

bool parseU32State(_NT_jsonParse& parse, Algorithm& algorithm) {
    int count = 0;
    uint32_t value[9] = {};
    if (!parse.numberOfArrayElements(count) || count != 18) {
        return false;
    }
    for (uint32_t index = 0; index < ARRAY_SIZE(value); ++index) {
        if (!parseUnsigned32(parse, value[index])) {
            return false;
        }
    }
    if (value[0] != algorithm.recordingBufferBytes ||
        value[1] != algorithm.eventCapacity || value[3] > value[1] ||
        (value[1] != 0U && value[2] >= value[1]) ||
        value[6] >= kClockAverageWindow || value[7] > kClockAverageWindow ||
        value[5] > value[3] || value[8] < kTimelineMinimumVisiblePulses ||
        value[8] > kTimelineLegacyMaximumVisiblePulses) {
        return false;
    }
    algorithm.eventHead = value[2];
    algorithm.eventCount = value[3];
    algorithm.clockIntervalSum = value[4];
    algorithm.playbackEventIndex = value[5];
    algorithm.clockIntervalWriteIndex = value[6];
    algorithm.clockIntervalCount = value[7];
    algorithm.timelineVisiblePulses = value[8];
    return true;
}

struct NavigationPresetState {
    NavigationPresetState()
        : showAll(false), manualSpan(0U), target(kSelectionFineTargetEnd) {}

    bool showAll;
    uint64_t manualSpan;
    SelectionFineTarget target;
};

bool parseNavigation(_NT_jsonParse& parse, NavigationPresetState& state) {
    int count = 0;
    int representation = 0;
    int target = 0;
    if (!parse.numberOfArrayElements(count) || count != 7 ||
        !parse.number(representation) || representation != 1 ||
        !parse.boolean(state.showAll) ||
        !parseUnsigned64(parse, state.manualSpan) ||
        state.manualSpan < kTimelineMinimumVisiblePulses ||
        !parse.number(target) || target < kSelectionFineTargetStart ||
        target > kSelectionFineTargetRange) {
        return false;
    }
    state.target = static_cast<SelectionFineTarget>(target);
    return true;
}

bool parseFlags(_NT_jsonParse& parse, Algorithm& algorithm) {
    int count = 0;
    bool value[15] = {};
    if (!parse.numberOfArrayElements(count) || count != 16) {
        return false;
    }
    for (uint32_t index = 0; index < ARRAY_SIZE(value); ++index) {
        if (!parse.boolean(value[index])) {
            return false;
        }
    }
    int transport = 0;
    if (!parse.number(transport) || transport < kTransportStopped ||
        transport > kTransportClockLossPaused) {
        return false;
    }
    algorithm.captureEnabled = value[0];
    algorithm.clockRunning = value[1];
    algorithm.haveAcquisitionPulse = value[2];
    algorithm.clockHigh = value[3];
    algorithm.resetHigh = value[4];
    algorithm.selectionValid = value[5];
    algorithm.activeSelectionValid = value[6];
    algorithm.rangeTransitionPending = value[7];
    algorithm.playbackPositionValid = value[8];
    algorithm.playbackIntervalOpen = value[9];
    algorithm.playbackNextEventScheduled = value[10];
    algorithm.pendingNextEndingScheduled = value[11];
    algorithm.selectionFineTarget =
        value[12] ? kSelectionFineTargetStart : kSelectionFineTargetEnd;
    algorithm.rightEncoderHoldActive = value[13];
    algorithm.rightEncoderPanicFired = value[14];
    algorithm.transportState = static_cast<TransportState>(transport);
    return true;
}

bool parsePresetState(_NT_jsonParse& parse, Algorithm& algorithm) {
    int members = 0;
    bool versionSeen = false;
    bool u64Seen = false;
    bool u32Seen = false;
    bool flagsSeen = false;
    bool navigationSeen = false;
    NavigationPresetState navigation;
    bool recordedSeen = false;
    bool outputSeen = false;
    bool pendingSeen = false;
    bool intervalsSeen = false;
    bool eventsSeen = false;
    if (!parse.numberOfObjectMembers(members)) {
        return false;
    }
    for (int member = 0; member < members; ++member) {
        if (parse.matchName("version")) {
            int version = 0;
            if (!parse.number(version) ||
                version != static_cast<int>(kPresetVersion)) {
                return false;
            }
            versionSeen = true;
        } else if (parse.matchName("u64")) {
            if (!parseU64State(parse, algorithm)) {
                return false;
            }
            u64Seen = true;
        } else if (parse.matchName("u32")) {
            if (!parseU32State(parse, algorithm)) {
                return false;
            }
            u32Seen = true;
        } else if (parse.matchName("flags")) {
            if (!parseFlags(parse, algorithm)) {
                return false;
            }
            flagsSeen = true;
        } else if (parse.matchName("navigation")) {
            if (!parseNavigation(parse, navigation)) {
                return false;
            }
            navigationSeen = true;
        } else if (parse.matchName("recorded")) {
            if (!deserialiseBytes(parse, &algorithm.recordedState,
                                  sizeof(algorithm.recordedState))) {
                return false;
            }
            recordedSeen = true;
        } else if (parse.matchName("output")) {
            if (!deserialiseBytes(parse, &algorithm.playbackOutputState,
                                  sizeof(algorithm.playbackOutputState))) {
                return false;
            }
            outputSeen = true;
        } else if (parse.matchName("pending")) {
            if (!deserialiseBytes(parse, &algorithm.pendingEndings,
                                  sizeof(algorithm.pendingEndings))) {
                return false;
            }
            pendingSeen = true;
        } else if (parse.matchName("clockIntervals")) {
            if (!deserialiseBytes(parse, algorithm.clockIntervals,
                                  sizeof(algorithm.clockIntervals))) {
                return false;
            }
            intervalsSeen = true;
        } else if (parse.matchName("events")) {
            if (!u32Seen || !deserialiseEvents(parse, algorithm)) {
                return false;
            }
            eventsSeen = true;
        } else if (!parse.skipMember()) {
            return false;
        }
    }
    const bool complete =
        versionSeen && u64Seen && u32Seen && flagsSeen && recordedSeen &&
        outputSeen && pendingSeen && intervalsSeen && eventsSeen;
    if (!complete) {
        return false;
    }
    if (navigationSeen) {
        algorithm.timelineShowAll = navigation.showAll;
        algorithm.timelineVisiblePulses = navigation.manualSpan;
        algorithm.selectionFineTarget = navigation.target;
    } else {
        // Before the extension every accepted 4..256 width represented a
        // manual view. Never reinterpret a legacy width as the fresh default.
        algorithm.timelineShowAll = false;
    }
    return true;
}

bool deserialise(_NT_algorithm* self, _NT_jsonParse& parse) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return false;
    }
    int members = 0;
    bool restored = false;
    if (!parse.numberOfObjectMembers(members)) {
        return false;
    }
    for (int member = 0; member < members; ++member) {
        if (parse.matchName("midibufferState")) {
            if (!parsePresetState(parse, *algorithm)) {
                return false;
            }
            restored = true;
        } else if (!parse.skipMember()) {
            return false;
        }
    }
    if (!restored) {
        return false;
    }

    // Clock/reset levels and instrument state belong to the live patch, not
    // the preset. Retain all internal timing, cursor, ownership, and transport
    // intent, but require two fresh physical clock edges before capture or
    // resumed playback can emit anything. Saved scheduling remains inert while
    // the clock is gated, then is rebased onto the newly acquired interval.
    algorithm->clockRunning = false;
    algorithm->haveAcquisitionPulse = false;
    algorithm->clockHigh = false;
    algorithm->resetHigh = false;
    algorithm->rightEncoderHoldActive = false;
    algorithm->rightEncoderPanicFired = false;
    algorithm->rightPotZoomActive = false;
    algorithm->rangeMotion = RangeMotionState();
    algorithm->rangeMotionDomainKnown = false;
    algorithm->rangeMotionNeedsPhysicalSeed = true;
    return !algorithm->captureEnabled ||
           algorithm->transportState == kTransportStopped;
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
    .serialise = serialise,
    .deserialise = deserialise,
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

bool musicalDurationFromPulses(uint64_t pulseIntervals,
                               uint32_t pulsesPerBeat,
                               MusicalDuration& duration) {
    duration = MusicalDuration();
    if (!supportedPulsesPerBeat(pulsesPerBeat)) {
        return false;
    }

    uint32_t pulseRemainder = 0U;
    const uint64_t wholeBeats = divideUnsigned64By32(
        pulseIntervals, pulsesPerBeat, pulseRemainder);
    uint32_t beatRemainder = 0U;
    duration.bars =
        divideUnsigned64By32(wholeBeats, 4U, beatRemainder);
    duration.beats = beatRemainder;
    duration.ticks = pulseRemainder * (480U / pulsesPerBeat);
    return true;
}

bool pulsesFromMusicalDuration(const MusicalDuration& duration,
                               uint32_t pulsesPerBeat,
                               uint64_t& pulseIntervals) {
    pulseIntervals = 0U;
    if (!supportedPulsesPerBeat(pulsesPerBeat) || duration.beats > 3U) {
        return false;
    }

    const uint32_t ticksPerPulse = 480U / pulsesPerBeat;
    if (duration.ticks >= 480U || duration.ticks % ticksPerPulse != 0U) {
        return false;
    }
    const uint32_t pulseRemainder = duration.ticks / ticksPerPulse;
    if (pulseRemainder >= pulsesPerBeat) {
        return false;
    }

    const uint64_t maximum = ~static_cast<uint64_t>(0);
    uint32_t ignoredRemainder = 0U;
    const uint64_t maximumBars = divideUnsigned64By32(
        maximum - duration.beats, 4U, ignoredRemainder);
    if (duration.bars > maximumBars) {
        return false;
    }
    const uint64_t wholeBeats = duration.bars * 4U + duration.beats;
    const uint64_t maximumWholeBeats = divideUnsigned64By32(
        maximum - pulseRemainder, pulsesPerBeat, ignoredRemainder);
    if (wholeBeats > maximumWholeBeats) {
        return false;
    }
    pulseIntervals = wholeBeats * pulsesPerBeat + pulseRemainder;
    return true;
}

bool formatMusicalDuration(char* output, size_t capacity,
                           uint64_t pulseIntervals,
                           uint32_t pulsesPerBeat) {
    BoundedText text(output, capacity);
    MusicalDuration duration = {};
    if (!musicalDurationFromPulses(pulseIntervals, pulsesPerBeat, duration)) {
        return false;
    }
    if (duration.bars > kMaximumDisplayedBars) {
        text.append(">9999999999bar");
        return text.valid();
    }

    text.appendUnsigned64(duration.bars);
    text.append(':');
    text.append(static_cast<char>('0' + duration.beats));
    text.append(':');
    text.append(static_cast<char>('0' + duration.ticks / 100U));
    text.append(static_cast<char>('0' + (duration.ticks / 10U) % 10U));
    text.append(static_cast<char>('0' + duration.ticks % 10U));
    return text.valid();
}

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
    snapshot.sampleCursor = algorithm->sampleCursor;
    snapshot.currentPulse = algorithm->currentPulse;
    snapshot.lastClockIntervalSamples = algorithm->lastClockIntervalSamples;
    snapshot.predictedClockIntervalSamples =
        predictedClockInterval(*algorithm);
    snapshot.playbackPulse = algorithm->playbackPulse;
    snapshot.playbackIntervalStartSample =
        algorithm->playbackIntervalStartSample;
    snapshot.playbackIntervalOrdinal = algorithm->playbackIntervalOrdinal;
    snapshot.playbackNextEventSample = algorithm->playbackNextEventSample;
    snapshot.pendingNextEndingSample = algorithm->pendingNextEndingSample;
    snapshot.playbackEventIndex = algorithm->playbackEventIndex;
    snapshot.clockAverageIntervalCount = algorithm->clockIntervalCount;
    snapshot.pendingNoteEndingCount = algorithm->pendingEndings.noteCount;
    snapshot.pendingSustainReleaseCount =
        algorithm->pendingEndings.sustainCount;
    snapshot.pulsesPerDisplayedBeat = pulsesPerDisplayedBeat(*algorithm);
    snapshot.timelineVisiblePulses = algorithm->timelineVisiblePulses;
    snapshot.timelineScrollPulses = algorithm->timelineScrollPulses;
    snapshot.retainedPulseIntervals =
        retainedTimelineIntervals(*algorithm);
    timelineViewBounds(*algorithm, snapshot.timelineViewStartPulse,
                       snapshot.timelineViewEndPulse);
    snapshot.timelineShowAll = algorithm->timelineShowAll;
    snapshot.captureEnabled = algorithm->captureEnabled;
    snapshot.clockRunning = algorithm->clockRunning;
    snapshot.selectionValid = algorithm->selectionValid;
    snapshot.playbackArmed =
        algorithm->transportState == kTransportArmed;
    snapshot.playbackActive =
        algorithm->transportState == kTransportPlaying;
    snapshot.playbackClockLossPaused =
        algorithm->transportState == kTransportClockLossPaused;
    snapshot.playbackIntervalOpen = algorithm->playbackIntervalOpen;
    snapshot.playbackNextEventScheduled =
        algorithm->playbackNextEventScheduled;
    snapshot.pendingNextEndingScheduled =
        algorithm->pendingNextEndingScheduled;
    snapshot.activeSelectionValid = algorithm->activeSelectionValid;
    snapshot.rangeTransitionPending = algorithm->rangeTransitionPending;
    snapshot.lastMovedBoundaryIsStart =
        algorithm->selectionFineTarget == kSelectionFineTargetStart;
    snapshot.rangeMotionEstablished = algorithm->rangeMotion.established;
    snapshot.selectionFineTarget = algorithm->selectionFineTarget;
    snapshot.rangeLogicalPosition = algorithm->rangeMotion.logicalPosition;
    snapshot.rangePhysicalPosition = algorithm->rangeMotion.physicalPosition;
    snapshot.rangePulseResidual = algorithm->rangeMotion.pulseResidual;
    snapshot.selection = algorithm->selection;
    snapshot.activeSelection = algorithm->activeSelection;
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

#if defined(MIDIBUFFER_NATIVE_TEST)
bool setRetainedTimelineFixture(_NT_algorithm* self, uint64_t startPulse,
                                uint64_t endPulse) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || algorithm->eventCapacity == 0U) {
        return false;
    }
    algorithm->eventHead = 0U;
    algorithm->eventCount = endPulse > startPulse ? 1U : 0U;
    algorithm->historyEndPulseExclusive =
        endPulse > startPulse ? endPulse : 0U;
    if (algorithm->eventCount != 0U) {
        RecordedEvent event = {};
        event.pulse = startPulse;
        event.sourceIntervalSamples = 1U;
        event.bytes[0] = 0x90U;
        event.bytes[1] = 60U;
        event.bytes[2] = 100U;
        event.size = 3U;
        algorithm->recordingEvents[0] = event;
    }
    return true;
}

bool addRetainedTimelineNoteFixture(_NT_algorithm* self, uint64_t pulse,
                                    uint8_t note) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || algorithm->eventCount == 0U ||
        algorithm->eventCount >= algorithm->eventCapacity) {
        return false;
    }
    uint32_t lastIndex =
        algorithm->eventHead + algorithm->eventCount - 1U;
    if (lastIndex >= algorithm->eventCapacity) {
        lastIndex -= algorithm->eventCapacity;
    }
    if (pulse < algorithm->recordingEvents[lastIndex].pulse ||
        pulse >= algorithm->historyEndPulseExclusive) {
        return false;
    }
    RecordedEvent event = {};
    event.pulse = pulse;
    event.sourceIntervalSamples = 1U;
    event.bytes[0] = 0x90U;
    event.bytes[1] = note;
    event.bytes[2] = 100U;
    event.size = 3U;
    uint32_t writeIndex = algorithm->eventHead + algorithm->eventCount;
    if (writeIndex >= algorithm->eventCapacity) {
        writeIndex -= algorithm->eventCapacity;
    }
    algorithm->recordingEvents[writeIndex] = event;
    ++algorithm->eventCount;
    return true;
}

bool setTimelineNavigationFixture(_NT_algorithm* self, bool showAll,
                                  uint64_t manualSpan, uint64_t scrollPulses,
                                  SelectionFineTarget target) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || manualSpan < kTimelineMinimumVisiblePulses ||
        target < kSelectionFineTargetStart ||
        target > kSelectionFineTargetRange) {
        return false;
    }
    algorithm->timelineShowAll = showAll;
    algorithm->timelineVisiblePulses = manualSpan;
    algorithm->timelineScrollPulses = scrollPulses;
    algorithm->selectionFineTarget = target;
    return true;
}
#endif

bool setPulseSelection(_NT_algorithm* self, uint64_t startPulse,
                       uint64_t endPulse) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL || algorithm->eventCount == 0 ||
        startPulse >= endPulse) {
        return false;
    }

    const uint64_t oldestPulse =
        algorithm->recordingEvents[algorithm->eventHead].pulse;
    if (startPulse < oldestPulse ||
        endPulse > algorithm->historyEndPulseExclusive) {
        return false;
    }

    algorithm->selection.startPulse = startPulse;
    algorithm->selection.endPulse = endPulse;
    algorithm->selectionValid = true;
    if (algorithm->transportState != kTransportStopped &&
        algorithm->activeSelectionValid) {
        algorithm->rangeTransitionPending =
            !sameRange(algorithm->selection, algorithm->activeSelection);
    } else {
        algorithm->rangeTransitionPending = false;
        algorithm->playbackPositionValid = false;
    }
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
        algorithm->selection.endPulse >
            algorithm->historyEndPulseExclusive) {
        return false;
    }

    range = algorithm->selection;
    return true;
}

bool startPlayback(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return false;
    }
    // Playback is a capture-stop path: freeze a finite performance before
    // validating its retained selection, without transmitting live cleanup.
    // Capture never restarts as a side effect of any transport transition.
    finalizeCapture(*algorithm);

    PulseRange range = {};
    if (!acquirePlaybackSelection(self, range)) {
        return false;
    }
    if (algorithm->transportState != kTransportStopped) {
        return true;
    }

    prepareRecordedEndingOwnership(*algorithm);
    algorithm->activeSelection = range;
    algorithm->activeSelectionValid = true;
    algorithm->rangeTransitionPending = false;
    if (!algorithm->playbackPositionValid ||
        algorithm->playbackPulse < range.startPulse ||
        algorithm->playbackPulse >= range.endPulse) {
        algorithm->playbackPulse = range.startPulse;
        algorithm->playbackEventIndex =
            findPlaybackEventIndex(*algorithm, algorithm->playbackPulse);
        algorithm->playbackPositionValid = true;
    }
    algorithm->playbackIntervalOpen = false;
    algorithm->playbackNextEventScheduled = false;
    algorithm->transportState = kTransportArmed;
    return true;
}

void stopPlayback(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    if (algorithm != NULL) {
        releasePlaybackOutput(*algorithm, algorithm->sampleCursor);
        algorithm->transportState = kTransportStopped;
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

void drawTinyText(int x, int y, const char* text) {
    NT_drawText(x, y, text, 15, kNT_textLeft, kNT_textTiny);
}

void drawShape(_NT_shape shape, int x0, int y0, int x1, int y1,
               int colour) {
    NT_drawShapeI(shape, x0, y0, x1, y1, colour);
}

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
