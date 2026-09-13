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
const uint32_t kRecordedEventFlagCaptureEnding = 0x01U;
const uint32_t kRecordedEventEndingIndexShift = 1U;

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
          pendingEndings(), transportState(kTransportStopped),
          playbackIntervalOrdinal(0), captureEnabled(false),
          clockRunning(false), haveAcquisitionPulse(false), clockHigh(false),
          resetHigh(false), selectionValid(false),
          activeSelectionValid(false), rangeTransitionPending(false),
          playbackPositionValid(false), playbackIntervalOpen(false),
          playbackNextEventScheduled(false), pendingNextEndingScheduled(false),
          pendingNextEndingSample(0) {}

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
    TransportState transportState;
    uint64_t playbackIntervalOrdinal;

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
    uint64_t pendingNextEndingSample;
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

void beginPlaybackInterval(Algorithm& algorithm, uint64_t sample) {
    ++algorithm.playbackIntervalOrdinal;
    algorithm.playbackIntervalStartSample = sample;
    algorithm.playbackIntervalOpen = true;
    scheduleNextPlaybackEvent(algorithm);
    refreshPendingEndingSchedule(algorithm);
    dispatchPlaybackEvents(algorithm, sample, false);
}

void activatePendingPlayback(Algorithm& algorithm, uint64_t sample) {
    if (!algorithm.playbackPositionValid) {
        algorithm.playbackPulse = algorithm.activeSelection.startPulse;
        algorithm.playbackEventIndex =
            findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
        algorithm.playbackPositionValid = true;
    }
    algorithm.transportState = kTransportPlaying;
    beginPlaybackInterval(algorithm, sample);
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
    snapshot.pendingNoteEndingCount = algorithm->pendingEndings.noteCount;
    snapshot.pendingSustainReleaseCount =
        algorithm->pendingEndings.sustainCount;
    snapshot.captureEnabled = algorithm->captureEnabled;
    snapshot.clockRunning = algorithm->clockRunning;
    snapshot.selectionValid = algorithm->selectionValid;
    snapshot.playbackArmed =
        algorithm->transportState == kTransportArmed;
    snapshot.playbackActive =
        algorithm->transportState == kTransportPlaying;
    snapshot.playbackClockLossPaused =
        algorithm->transportState == kTransportClockLossPaused;
    snapshot.activeSelectionValid = algorithm->activeSelectionValid;
    snapshot.rangeTransitionPending = algorithm->rangeTransitionPending;
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
