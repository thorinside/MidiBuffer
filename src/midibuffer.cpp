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
const uint32_t kClearHistoryMetadataOperations = 4U;

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
    kParameterPlayback,
    kParameterClearRecording,
    kParameterMpeMode,
    kParameterMpeSetupSource,
    kParameterLowerMembers,
    kParameterUpperMembers,
    kNumParameters,
};

static_assert(kParameterPulsesPerDisplayedBeat == 9 &&
                  kParameterPlayback == 10 &&
                  kParameterClearRecording == 11,
              "appended controls must not renumber inherited parameters");
static_assert(kParameterMpeMode == 12 && kParameterMpeSetupSource == 13 &&
                  kParameterLowerMembers == 14 &&
                  kParameterUpperMembers == 15,
              "MPE controls append after the existing twelve parameters");

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
          pendingEndings(), rangeMotion(), startBoundaryMotion(0.0),
          endBoundaryMotion(1.0), transportState(kTransportStopped),
          playbackIntervalOrdinal(0), timelineScrollPulses(0),
          timelineVisiblePulses(kDefaultTimelineVisiblePulses),
          zoomPressVisiblePulses(0), zoomPressManualVisiblePulses(0),
          rangeMotionHistoryStart(0), rangeMotionHistoryEnd(0),
          zoomPressCoordinate(0), clearRecordingTransitions(0),
          clearHistoryMetadataOperations(0), restoredPlaybackValue(0),
          restoredClearRecordingValue(0), captureEnabled(false),
          clearRecordingArmed(true), processingStarted(false),
          playbackRestorePending(false),
          clearRecordingRestorePending(false), clockRunning(false),
          haveAcquisitionPulse(false), clockHigh(false), resetHigh(false),
          selectionValid(false), activeSelectionValid(false),
          rangeTransitionPending(false), playbackPositionValid(false),
          playbackIntervalOpen(false), playbackNextEventScheduled(false),
          pendingNextEndingScheduled(false),
          selectionFineTarget(kSelectionFineTargetEnd),
          timelineShowAll(true), zoomPressShowAll(true),
          rightPotZoomActive(false), rangeMotionDomainKnown(false),
          rangeMotionNeedsPhysicalSeed(true),
          boundaryMotionNeedsPhysicalSeed(false),
          rightEncoderHoldActive(false),
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
    BoundaryMotionState startBoundaryMotion;
    BoundaryMotionState endBoundaryMotion;
    TransportState transportState;
    uint64_t playbackIntervalOrdinal;
    uint64_t timelineScrollPulses;
    uint64_t timelineVisiblePulses;
    uint64_t zoomPressVisiblePulses;
    uint64_t zoomPressManualVisiblePulses;
    uint64_t rangeMotionHistoryStart;
    uint64_t rangeMotionHistoryEnd;
    uint32_t zoomPressCoordinate;
    uint32_t clearRecordingTransitions;
    uint32_t clearHistoryMetadataOperations;
    int16_t restoredPlaybackValue;
    int16_t restoredClearRecordingValue;

    bool captureEnabled;
    bool clearRecordingArmed;
    bool processingStarted;
    bool playbackRestorePending;
    bool clearRecordingRestorePending;
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
    bool boundaryMotionNeedsPhysicalSeed;
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

static const char* const kBooleanStrings[] = {
    "Off",
    "On",
};

static const char* const kMpeSetupSourceStrings[] = {
    "Observed",
    "Manual",
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
        .enumStrings = kBooleanStrings,
    },
    {
        .name = "Filter Pitch Bend",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kBooleanStrings,
    },
    {
        .name = "Filter Aftertouch",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kBooleanStrings,
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
    {
        .name = "Playback",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kBooleanStrings,
    },
    {
        .name = "Clear Recording",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kBooleanStrings,
    },
    {
        .name = "MPE Mode",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kBooleanStrings,
    },
    {
        .name = "MPE Setup",
        .min = 0,
        .max = 1,
        .def = 0,
        .unit = kNT_unitEnum,
        .scaling = kNT_scalingNone,
        .enumStrings = kMpeSetupSourceStrings,
    },
    {
        .name = "Lower Members",
        .min = 0,
        .max = 15,
        .def = 0,
        .unit = kNT_unitNone,
        .scaling = kNT_scalingNone,
        .enumStrings = NULL,
    },
    {
        .name = "Upper Members",
        .min = 0,
        .max = 15,
        .def = 0,
        .unit = kNT_unitNone,
        .scaling = kNT_scalingNone,
        .enumStrings = NULL,
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
    kParameterClearRecording,
};

static const uint8_t kPlaybackPageParameters[] = {
    kParameterPlayback,
    kParameterPlaybackDestination,
    kParameterPlaybackChannel,
    kParameterFilterControlChange,
    kParameterFilterPitchBend,
    kParameterFilterAftertouch,
};

static const uint8_t kTimelinePageParameters[] = {
    kParameterPulsesPerDisplayedBeat,
};

static const uint8_t kMpePageParameters[] = {
    kParameterMpeMode,
    kParameterMpeSetupSource,
    kParameterLowerMembers,
    kParameterUpperMembers,
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
    {
        .name = "MPE",
        .numParams = ARRAY_SIZE(kMpePageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kMpePageParameters,
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

// The event ring retains its decimal capacity; the trailing DRAM arena is
// reserved independently for MPE history, contexts, ownership, and parser data.
const uint32_t kMpeArenaBytes = 524288U;

static_assert(sizeof(HistoryState) == 512U,
              "historical state is the approved 512-byte full state");

// Observations are keyed by pulse, offset, receipt sequence and ordinal. For
// a checkpoint the ordinal is the first event it does not include; for a
// journal entry it is the next event ordinal at the time of the change.
struct HistoryKey {
    uint64_t ordinal;
    uint64_t pulse;
    uint32_t offsetSamples;
    uint32_t receipt;
};

struct HistoryCheckpoint {
    HistoryKey key;
    HistoryState state;
};

enum HistoryJournalKind : uint32_t {
    kHistoryJournalResumption = 1, // capture/clock/admission gap seed
    kHistoryJournalSetup = 2,      // setup change while capture is recording
};

struct HistoryJournalEntry {
    HistoryKey key;
    uint32_t kind;
    uint32_t reserved0;
    uint8_t reserved[96];
    HistoryState state;
};
static_assert(sizeof(HistoryCheckpoint) == 536U &&
                  sizeof(HistoryJournalEntry) == 640U,
              "history records match the approved arena accounting");

const uint32_t kHistoryCheckpointInterval = 512U;
const uint32_t kHistoryMaximumCheckpoints = 409U;
const uint32_t kHistoryJournalEntries = 256U;
const uint32_t kHistoryMaximumForwardEvents = kHistoryCheckpointInterval - 1U;
const uint32_t kFinalizeAppendsPerStep = 128U;
const uint32_t kPreparationVisitsPerStep = 512U;
const uint32_t kHistoryNoIndex = 0xffffffffU;

enum HistoryContextSlot {
    kHistoryContextActive,
    kHistoryContextCandidate,
    kHistoryContextWork,
    kHistoryContextReserved, // sent-context storage for MPE playback
};

struct HistoryContextMeta {
    PulseRange range;
    uint64_t generation;
    uint32_t setupFingerprint;
    bool ready;
    bool coverageComplete;
    bool setupChangesInside;
};

enum HistoryPreparationPhase : uint8_t {
    kPreparationIdle,
    kPreparationSearch,
    kPreparationFold,
    kPreparationInside,
};

struct HistoryControl {
    HistoryState live;     // admitted observations, independent of capture
    HistoryState recorded; // state after the newest appended event
    HistoryState seed;     // state after the newest evicted event
    HistoryContextMeta contexts[2];
    HistoryContextMeta work;
    RecordedEvent endingTemplate;
    uint64_t baseOrdinal;
    uint64_t generation;
    uint64_t pairedGeneration;
    uint64_t pairingGeneration;
    uint64_t coverageStartOrdinal;
    uint64_t seedLastPulse;
    uint64_t workOrdinal;
    uint32_t workForwardEvents;
    uint32_t workJournalCursor;
    uint32_t workJournalVisits;
    uint32_t checkpointCount;
    uint32_t journalHead;
    uint32_t journalCount;
    uint32_t journalEvictions;
    uint32_t receipt;
    uint32_t membershipGeneration[16];
    uint32_t finalizeCursor;
    uint32_t pairingCursor;
    uint32_t pairingWalkPress;
    uint32_t pairingWalkRelease;
    // CC66/CC69 FIFO heads and tails; notes and CC64 reuse PendingPlaybackEndings.
    uint32_t pedalHeads[16][2];
    uint64_t pedalTails[16][2];
    uint32_t lastStepFinalizeAppends;
    uint32_t maximumStepFinalizeAppends;
    uint32_t lastStepPreparationVisits;
    uint32_t maximumStepPreparationVisits;
    uint32_t maximumSeedSearchComparisons;
    uint32_t maximumCheckpointSearchComparisons;
    uint32_t maximumForwardEventVisits;
    uint32_t maximumJournalVisits;
    int32_t admission;
    uint8_t workPhase;
    bool seedValid;
    bool seedFolded;
    bool finalizing;
    bool pairing;
    bool pairingComplete;
    bool candidateRequested;
    uint8_t reserved[1];
};

// MPE playback ownership uses 2,096 indexed keys: 16x128 notes, then
// CC64/66/69 per channel. Each key latches the route it was emitted on and
// may own one pending recorded ending in a fixed binary heap.
const uint32_t kMpeNoteKeys = 16U * 128U;
const uint32_t kMpeOwnerKeys = kMpeNoteKeys + 16U * 3U;
static_assert(kMpeOwnerKeys == 2096U, "approved indexed owner/heap keys");
const uint16_t kMpeNoSlot = 0xffffU;
const uint32_t kMpeNoEnding = 0xffffffffU;
const uint32_t kMpeStepVisits = 32768U;
const uint32_t kMpeStepSends = 8192U;
const uint32_t kMpeStepFanout = 32768U;
const uint32_t kMpeStepHeap = 262144U;
const uint32_t kMpeCleanupCalls = 2304U;
const uint32_t kMpeCleanupFanout = 9216U;
// Cleanup visits each owned key once and sends one ending per key, to at
// most four destinations, so prior-ownership cleanup always fits the reserve.
static_assert(kMpeOwnerKeys <= kMpeCleanupCalls &&
                  kMpeOwnerKeys * 4U <= kMpeCleanupFanout,
              "owned note/hold cleanup fits the separate reserve");
// A 2,096-entry heap has 12 levels. A push sifts up (one comparison and one
// move per level); a removal may sift up and then down (two comparisons and
// one move per level), so these are upper bounds for admission preflight.
const uint32_t kMpeHeapLevels = 12U;
const uint32_t kMpeHeapPushCost = 2U * kMpeHeapLevels;
const uint32_t kMpeHeapRemoveCost = 5U * kMpeHeapLevels;

struct MpeOwner {
    uint64_t dueInterval;
    uint32_t endingIndex;   // pending recorded ending, kMpeNoEnding if none
    uint16_t heapPosition;  // kMpeNoSlot when no ending is pending
    uint16_t ownedPosition; // owned-note list position, kMpeNoSlot if free
    uint8_t destination;    // route latched when ownership was emitted
    uint8_t reserved[7];
};

struct MpePlayback {
    MpeOwner owners[kMpeOwnerKeys];
    uint16_t heap[kMpeOwnerKeys];
    uint16_t ownedNotes[kMpeNoteKeys];
    uint16_t scratch[kMpeOwnerKeys]; // admission preflight heap traversal
    MpeWorkCounters step;
    MpeWorkCounters lastStep;
    MpeWorkCounters maximum;
    uint64_t holdMask; // owned hold keys, bit = key - kMpeNoteKeys
    uint64_t admittedSample;
    uint32_t heapSize;
    uint32_t ownedNoteCount;
    uint32_t rejectedClusters;
    uint32_t generatedSetup;
    uint32_t generatedContext;
    uint32_t generatedCleanup;
    int16_t controls[kNumParameters]; // last observed MPE-relevant values
    uint16_t scopeMask;
    uint8_t lower;
    uint8_t upper;
    uint8_t status;
    bool engaged;         // the active playback uses MPE semantics
    bool deliberate;      // ephemeral capability from a fresh live On
    bool entryPending;    // Finalizing/Preparing toward Armed
    bool fault;           // overload latched until a fresh Off/On
    bool restartRequired; // incompatible candidate kept the active range
    bool contextPending;  // send known context before the next pass
    bool admittedValid;
    bool setupIncomplete;
    bool contextIncomplete;
    bool restoreWindow;   // host restore callbacks are action-free
};

// Parser state shares the remaining planned budget.
struct MpeArenaReservation {
    HistoryCheckpoint checkpoints[kHistoryMaximumCheckpoints];
    HistoryJournalEntry journal[kHistoryJournalEntries];
    HistoryState contexts[4];
    HistoryControl history;
    MpePlayback mpe; // 8-byte aligned directly after the history control
    SetupObserver setup;
    uint8_t ownersAndControl[104416U - sizeof(SetupObserver) -
                             sizeof(HistoryControl) - sizeof(MpePlayback)];
    uint8_t alignmentAndSlack[34760U];
};
static_assert(sizeof(MpeArenaReservation) == kMpeArenaBytes,
              "MPE reservation must fit the separately accounted DRAM arena");

MpeArenaReservation& mpeArena(const Algorithm& algorithm) {
    return *reinterpret_cast<MpeArenaReservation*>(
        reinterpret_cast<uint8_t*>(algorithm.recordingEvents) +
        algorithm.recordingBufferBytes);
}

HistoryControl& historyControl(const Algorithm& algorithm) {
    return mpeArena(algorithm).history;
}

MpePlayback& mpeState(const Algorithm& algorithm) {
    return mpeArena(algorithm).mpe;
}

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t* specifications) {
    requirements.numParameters = kNumParameters;
    requirements.sram = sizeof(Algorithm);
    requirements.dram = requestedRecordingBytes(specifications) + kMpeArenaBytes;
    requirements.dtc = 0;
    requirements.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements& requirements,
                         const int32_t* specifications) {
    const uint32_t recordingBytes = requestedRecordingBytes(specifications);
    if (memory.sram == NULL || memory.dram == NULL ||
        requirements.sram < sizeof(Algorithm) ||
        requirements.dram < recordingBytes + kMpeArenaBytes) {
        return NULL;
    }

    Algorithm* algorithm =
        new (memory.sram) Algorithm(memory.dram, recordingBytes);
    MpeArenaReservation* arena = reinterpret_cast<MpeArenaReservation*>(
        memory.dram + recordingBytes);
    new (&arena->setup) SetupObserver();
    for (unsigned channel = 0; channel < 16; ++channel) {
        arena->setup.roles[channel] = kSetupRoleUnknown;
    }
    HistoryControl* history = new (&arena->history) HistoryControl();
    history->checkpointCount =
        (algorithm->eventCapacity + kHistoryCheckpointInterval - 1U) /
            kHistoryCheckpointInterval + 2U;
    history->admission = -1;
    history->seedValid = true;
    MpePlayback* mpe = new (&arena->mpe) MpePlayback();
    for (uint32_t key = 0; key < kMpeOwnerKeys; ++key) {
        mpe->owners[key].endingIndex = kMpeNoEnding;
        mpe->owners[key].heapPosition = kMpeNoSlot;
        mpe->owners[key].ownedPosition = kMpeNoSlot;
    }
    for (uint32_t parameter = 0; parameter < kNumParameters; ++parameter) {
        mpe->controls[parameter] = kParameters[parameter].def;
    }
    algorithm->parameters = kParameters;
    algorithm->parameterPages = &kParameterPages;
    return algorithm;
}

void finalizeCapture(Algorithm& algorithm);
void completeCaptureFinalization(Algorithm& algorithm);
uint64_t nextHistoryOrdinal(const Algorithm& algorithm);
void restartHistory(Algorithm& algorithm, uint64_t retiredOrdinal,
                    bool coverageKnown);
void updateHistoryAdmission(Algorithm& algorithm);
void cancelHistoryCandidate(Algorithm& algorithm);
void clearPendingPlaybackEndings(Algorithm& algorithm);
void releasePlaybackOutput(Algorithm& algorithm, uint64_t dispatchSample);
void clearRecording(_NT_algorithm* self, Algorithm& algorithm);
void mpeReleaseOwned(Algorithm& algorithm, uint64_t sample, bool cleanup,
                     uint16_t channelMask);
void mpeForgetOwners(Algorithm& algorithm);
void mpeControlChanged(_NT_algorithm* self, Algorithm& algorithm,
                       int parameter);
bool sameHistoryRange(const PulseRange& left, const PulseRange& right);
void requestHistoryCandidate(Algorithm& algorithm, const PulseRange& range);

void stopTransport(Algorithm& algorithm) {
    releasePlaybackOutput(algorithm, algorithm.sampleCursor);
    mpeState(algorithm).engaged = false;
    mpeState(algorithm).entryPending = false;
    mpeState(algorithm).contextPending = false;
    algorithm.transportState = kTransportStopped;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
}

void parameterChanged(_NT_algorithm* self, int parameter) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || parameter < 0 || parameter >= kNumParameters) {
        return;
    }

    ++algorithm->state.parameterChanges;
    if (parameter == kParameterPlayback && algorithm->v != NULL &&
        algorithm->playbackRestorePending &&
        algorithm->v[kParameterPlayback] ==
            algorithm->restoredPlaybackValue) {
        // Generic parameters may be restored after custom state. Consume the
        // matching host callback without replaying the already-restored
        // transport transition or finalizing capture a second time.
        algorithm->playbackRestorePending = false;
    } else if (parameter == kParameterClearRecording && algorithm->v != NULL &&
               algorithm->clearRecordingRestorePending &&
               algorithm->v[kParameterClearRecording] ==
                   algorithm->restoredClearRecordingValue) {
        // A restored On is state, not a new Off-to-On gesture. In particular,
        // it must not erase the history reconstructed by deserialise().
        algorithm->clearRecordingRestorePending = false;
    } else if (parameter == kParameterPlayback && algorithm->v != NULL) {
        const bool requested = algorithm->v[kParameterPlayback] != 0;
        MpePlayback& mpe = mpeState(*algorithm);
        // Only a fresh live On creates the ephemeral deliberate-entry
        // capability; it also clears a latched overload.
        mpe.deliberate = requested;
        if (requested) {
            mpe.fault = false;
            mpe.status = kMpeStatusReady;
            mpe.restartRequired = false;
            startPlayback(self);
        } else {
            stopTransport(*algorithm);
        }
    } else if (parameter == kParameterCapture && algorithm->v != NULL) {
        const bool requested = algorithm->v[kParameterCapture] !=
                               kParameters[kParameterCapture].min;
        if (!requested) {
            finalizeCapture(*algorithm);
        } else if (!algorithm->captureEnabled &&
                   algorithm->transportState == kTransportStopped) {
            completeCaptureFinalization(*algorithm);
            algorithm->recordedState = RecordedState();
            algorithm->captureEnabled = true;
        }
    } else if (parameter == kParameterClearRecording && algorithm->v != NULL) {
        const bool requested = algorithm->v[kParameterClearRecording] != 0;
        // Before the first audio block these are host initialization/restore
        // callbacks, not physical clear gestures. In particular, generic
        // Clear On may arrive before deserialise can restore its consumed
        // latch. Never clear or call a host setter during that phase.
        if (!algorithm->processingStarted) {
            algorithm->clearRecordingArmed = !requested;
            return;
        }
        if (!requested) {
            algorithm->clearRecordingArmed = true;
        } else if (algorithm->clearRecordingArmed) {
            algorithm->clearRecordingArmed = false;
            clearRecording(self, *algorithm);
        }
    } else if (parameter == kParameterRecordingChannel) {
        updateHistoryAdmission(*algorithm);
    } else if (parameter == kParameterClock) {
        // A newly routed input must acquire its own edge rather than inherit
        // the previous input's gate level.
        algorithm->clockHigh = false;
    } else if (parameter == kParameterReset) {
        algorithm->resetHigh = false;
    }
    mpeControlChanged(self, *algorithm, parameter);
}

void clearSelection(Algorithm& algorithm) {
    cancelHistoryCandidate(algorithm);
    mpeForgetOwners(algorithm);
    mpeState(algorithm).engaged = false;
    mpeState(algorithm).entryPending = false;
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

void clearRecording(_NT_algorithm* self, Algorithm& algorithm) {
    // Clear is a deliberate transport stop. Publish Playback Off through the
    // host so mappings and UI observe the same state; the nested callback owns
    // ordered note-off-then-sustain cleanup. The direct stop is a harmless
    // fixed-work fallback when no host binding is available.
    if (algorithm.v != NULL && algorithm.v[kParameterPlayback] != 0) {
        nt_host::setParameterFromAudio(self, kParameterPlayback, 0);
    }
    stopTransport(algorithm);

    // History invalidation is exactly four fixed metadata writes regardless
    // of the configured event capacity. The backing event allocation is not
    // scanned or overwritten. Resetting recorded ownership establishes the
    // post-clear capture boundary without changing Capture itself.
    const uint64_t retiredOrdinal = nextHistoryOrdinal(algorithm);
    algorithm.eventHead = 0;
    algorithm.eventCount = 0;
    algorithm.historyEndPulseExclusive = 0;
    algorithm.recordedState = RecordedState();
    restartHistory(algorithm, retiredOrdinal, true);
    algorithm.clearHistoryMetadataOperations =
        kClearHistoryMetadataOperations;
    ++algorithm.clearRecordingTransitions;

    clearSelection(algorithm);
    algorithm.playbackIntervalStartSample = 0;
    algorithm.playbackIntervalOrdinal = 0;
    algorithm.playbackNextEventSample = 0;
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

// Explicit byte loops keep 512-byte state copies free of extra libc imports.
void copyHistoryState(HistoryState& target, const HistoryState& source) {
    const volatile uint8_t* from =
        reinterpret_cast<const volatile uint8_t*>(&source);
    uint8_t* to = reinterpret_cast<uint8_t*>(&target);
    for (uint32_t index = 0; index < sizeof(HistoryState); ++index) {
        to[index] = from[index];
    }
}

void zeroHistoryState(HistoryState& state) {
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&state);
    for (uint32_t index = 0; index < sizeof(HistoryState); ++index) {
        bytes[index] = 0U;
    }
}

bool sameHistoryBytes(const HistoryState& left, const HistoryState& right,
                      uint32_t begin, uint32_t end) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(&left);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&right);
    for (uint32_t index = begin; index < end; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

const uint32_t kHistorySetupBegin = offsetof(HistoryState, managerCents);
const uint32_t kHistorySetupEnd = offsetof(HistoryState, reserved);

bool sameHistoryState(const HistoryState& left, const HistoryState& right) {
    return sameHistoryBytes(left, right, 0U, sizeof(HistoryState));
}

void forgetHistoryChannel(HistoryState& state, uint8_t channel) {
    for (uint8_t index = 0; index < kHistoryValueCount; ++index) {
        state.values[channel][index] = 0U;
    }
    state.known[channel] = 0U;
}

void setHistoryValue(HistoryState& state, uint8_t channel,
                     HistoryValue index, uint8_t value) {
    state.values[channel][index] = static_cast<uint8_t>(value & 0x7fU);
    state.known[channel] =
        static_cast<uint16_t>(state.known[channel] | (1U << index));
}

void applyHistoryMessage(HistoryState& state, const uint8_t bytes[3]) {
    const uint8_t type = bytes[0] & 0xf0U;
    const uint8_t channel = bytes[0] & 0x0fU;
    if (type == 0xe0U) {
        setHistoryValue(state, channel, kHistoryBendLsb, bytes[1]);
        setHistoryValue(state, channel, kHistoryBendMsb, bytes[2]);
    } else if (type == 0xd0U) {
        setHistoryValue(state, channel, kHistoryChannelPressure, bytes[1]);
    } else if (type == 0xb0U) {
        HistoryValue index = kHistoryValueCount;
        switch (bytes[1]) {
        case 74U: index = kHistoryTimbre; break;
        case 1U: index = kHistoryModulationMsb; break;
        case 33U: index = kHistoryModulationLsb; break;
        case 11U: index = kHistoryExpressionMsb; break;
        case 43U: index = kHistoryExpressionLsb; break;
        case 64U: index = kHistorySustain; break;
        case 66U: index = kHistorySostenuto; break;
        case 69U: index = kHistoryHold2; break;
        default: break;
        }
        if (index != kHistoryValueCount) {
            setHistoryValue(state, channel, index, bytes[2]);
        }
    }
}

// Generated capture endings are output safety, never observed evidence.
void applyHistoryEvent(HistoryState& state, const RecordedEvent& event) {
    if ((event.flags & kRecordedEventFlagCaptureEnding) == 0U) {
        applyHistoryMessage(state, event.bytes);
    }
}

void copyHistorySetup(HistoryState& state, const SetupObserver& setup) {
    for (unsigned zone = 0; zone < 2U; ++zone) {
        state.zoneCount[zone] = setup.zones[zone].count;
        state.zoneCountState[zone] = setup.zones[zone].countState;
        state.managerCents[zone] = setup.zones[zone].manager.cents;
        state.managerState[zone] = setup.zones[zone].manager.state;
        state.memberCents[zone] = setup.zones[zone].member.cents;
        state.memberState[zone] = setup.zones[zone].member.state;
    }
    for (unsigned channel = 0; channel < 16U; ++channel) {
        state.roles[channel] = setup.roles[channel];
        state.channelCents[channel] = setup.channelSensitivity[channel].cents;
        state.channelCentsState[channel] =
            setup.channelSensitivity[channel].state;
    }
}

HistoryCheckpoint& historyCheckpoint(const Algorithm& algorithm,
                                     uint64_t checkpointNumber) {
    uint32_t slot = 0;
    divideUnsigned64By32(checkpointNumber,
                         historyControl(algorithm).checkpointCount, slot);
    return mpeArena(algorithm).checkpoints[slot];
}

uint64_t nextHistoryOrdinal(const Algorithm& algorithm) {
    return historyControl(algorithm).baseOrdinal + algorithm.eventCount;
}

HistoryJournalEntry& journalEntry(const Algorithm& algorithm,
                                  uint32_t oldestFirst) {
    const HistoryControl& history = historyControl(algorithm);
    return mpeArena(algorithm).journal[(history.journalHead + oldestFirst) %
                                       kHistoryJournalEntries];
}

// Binary search over the ordinal-sorted journal: the number of entries keyed
// at most `ordinal` (inclusive) or below it. 256 entries need <= 9 probes.
uint32_t journalBound(const Algorithm& algorithm, uint64_t ordinal,
                      bool inclusive, uint32_t& comparisons) {
    uint32_t low = 0;
    uint32_t high = historyControl(algorithm).journalCount;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2U;
        const uint64_t key = journalEntry(algorithm, middle).key.ordinal;
        ++comparisons;
        if (inclusive ? key <= ordinal : key < ordinal) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

void noteMaximum(uint32_t& maximum, uint32_t value) {
    if (value > maximum) {
        maximum = value;
    }
}

void writeHistoryJournal(Algorithm& algorithm, const HistoryState& state,
                         uint64_t pulse, uint32_t offsetSamples,
                         uint32_t kind) {
    HistoryControl& history = historyControl(algorithm);
    if (history.journalCount == kHistoryJournalEntries) {
        // Overflow drops an unfolded change. Coverage now starts after it and
        // the seed cannot certify history until it folds a later full entry.
        const uint64_t lost = journalEntry(algorithm, 0).key.ordinal + 1U;
        if (lost > history.coverageStartOrdinal) {
            history.coverageStartOrdinal = lost;
        }
        history.seedValid = false;
        history.journalHead = (history.journalHead + 1U) %
                              kHistoryJournalEntries;
        --history.journalCount;
        ++history.journalEvictions;
    }
    HistoryJournalEntry& entry = journalEntry(algorithm, history.journalCount);
    entry.key.ordinal = nextHistoryOrdinal(algorithm);
    entry.key.pulse = pulse;
    entry.key.offsetSamples = offsetSamples;
    entry.key.receipt = history.receipt;
    entry.kind = kind;
    copyHistoryState(entry.state, state);
    ++history.journalCount;
    copyHistoryState(history.recorded, state);
    ++history.generation;
}

// Before the oldest event is overwritten, fold it (and the journal entries
// keyed before it) into the seed so the oldest retained point stays known.
void advanceHistorySeed(Algorithm& algorithm, const RecordedEvent& evicted) {
    HistoryControl& history = historyControl(algorithm);
    uint32_t comparisons = 0;
    const uint32_t folded =
        journalBound(algorithm, history.baseOrdinal, true, comparisons);
    noteMaximum(history.maximumSeedSearchComparisons, comparisons);
    if (folded != 0U) {
        copyHistoryState(history.seed,
                         journalEntry(algorithm, folded - 1U).state);
        history.seedValid = true;
        history.journalHead =
            (history.journalHead + folded) % kHistoryJournalEntries;
        history.journalCount -= folded;
    }
    applyHistoryEvent(history.seed, evicted);
    history.seedLastPulse = evicted.pulse;
    history.seedFolded = true;
    ++history.baseOrdinal;
}

void recordHistoryAppend(Algorithm& algorithm, const RecordedEvent& event) {
    HistoryControl& history = historyControl(algorithm);
    applyHistoryEvent(history.recorded, event);
    ++history.generation;
    const uint64_t total = nextHistoryOrdinal(algorithm);
    if (total % kHistoryCheckpointInterval == 0U) {
        HistoryCheckpoint& checkpoint =
            historyCheckpoint(algorithm, total / kHistoryCheckpointInterval);
        checkpoint.key.ordinal = total;
        checkpoint.key.pulse = event.pulse;
        checkpoint.key.offsetSamples = event.offsetSamples;
        checkpoint.key.receipt = history.receipt;
        copyHistoryState(checkpoint.state, history.recorded);
    }
}

// Clear and load retire every old ordinal, so stale checkpoint slots can never
// match. A loaded history has no retained observation coverage yet.
void restartHistory(Algorithm& algorithm, uint64_t retiredOrdinal,
                    bool coverageKnown) {
    HistoryControl& history = historyControl(algorithm);
    history.baseOrdinal = retiredOrdinal;
    history.journalHead = 0;
    history.journalCount = 0;
    zeroHistoryState(history.recorded);
    zeroHistoryState(history.seed);
    history.seedValid = coverageKnown;
    history.seedFolded = false;
    history.coverageStartOrdinal =
        coverageKnown ? retiredOrdinal : retiredOrdinal + algorithm.eventCount;
    history.finalizing = false;
    history.pairing = false;
    history.pairingComplete = false;
    history.candidateRequested = false;
    history.workPhase = kPreparationIdle;
    history.contexts[kHistoryContextActive].ready = false;
    history.contexts[kHistoryContextCandidate].ready = false;
    ++history.generation;
}

int recordingAdmission(const Algorithm& algorithm) {
    int admitted =
        algorithm.v == NULL ? 0 : algorithm.v[kParameterRecordingChannel];
    return admitted < 0 || admitted > 16 ? 0 : admitted;
}

// A recording-channel change is an admission boundary: channels that were
// not observed under both settings become unknown rather than stale.
void updateHistoryAdmission(Algorithm& algorithm) {
    HistoryControl& history = historyControl(algorithm);
    const int admission = recordingAdmission(algorithm);
    if (history.admission >= 0 && history.admission != admission) {
        for (uint8_t channel = 0; channel < 16U; ++channel) {
            const bool before =
                history.admission == 0 || history.admission == channel + 1;
            const bool after = admission == 0 || admission == channel + 1;
            if (!before || !after) {
                forgetHistoryChannel(history.live, channel);
            }
        }
    }
    history.admission = admission;
}

// Live observation runs for every admitted message before capture/clock
// gating. A recorded event first seeds any gap since the last recorded item;
// an unrecorded change while recording (setup) is journaled at its receipt.
void observeLiveHistory(Algorithm& algorithm, const RecordedEvent& event,
                        bool recorded) {
    HistoryControl& history = historyControl(algorithm);
    updateHistoryAdmission(algorithm);
    ++history.receipt;
    if (recorded && !sameHistoryState(history.live, history.recorded)) {
        writeHistoryJournal(algorithm, history.live, event.pulse,
                            event.offsetSamples, kHistoryJournalResumption);
    }
    const SetupObserver& setup = mpeArena(algorithm).setup;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        if (setup.membershipGeneration[channel] !=
            history.membershipGeneration[channel]) {
            history.membershipGeneration[channel] =
                setup.membershipGeneration[channel];
            forgetHistoryChannel(history.live, channel);
        }
    }
    copyHistorySetup(history.live, setup);
    applyHistoryMessage(history.live, event.bytes);
    if (!recorded && algorithm.captureEnabled && algorithm.clockRunning &&
        !sameHistoryState(history.live, history.recorded)) {
        writeHistoryJournal(algorithm, history.live, event.pulse,
                            event.offsetSamples, kHistoryJournalSetup);
    }
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
        advanceHistorySeed(algorithm, algorithm.recordingEvents[writeIndex]);
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
    recordHistoryAppend(algorithm, event);
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

// Removes and returns the next non-neutral controller ending for a channel
// in the legacy order: pedals, CC1/11, CC33/43, bend, pressure, polyphonic.
bool takeChannelEnding(RecordedState& state, uint8_t channel,
                       uint8_t bytes[3], uint8_t& size) {
    size = 3U;
    bytes[0] = static_cast<uint8_t>(0xb0U | channel);
    bytes[2] = 0U;
    for (uint8_t controller = 64U; controller <= 69U; ++controller) {
        const uint8_t bit = static_cast<uint8_t>(1U << (controller - 64U));
        if ((state.pedalMasks[channel] & bit) != 0U) {
            state.pedalMasks[channel] =
                static_cast<uint8_t>(state.pedalMasks[channel] & ~bit);
            bytes[1] = controller;
            return true;
        }
    }
    const uint8_t modulationControllers[] = {1U, 33U};
    const uint8_t expressionControllers[] = {11U, 43U};
    for (uint8_t index = 0; index < 2U; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1U << index);
        if ((state.modulationMasks[channel] & bit) != 0U) {
            state.modulationMasks[channel] =
                static_cast<uint8_t>(state.modulationMasks[channel] & ~bit);
            bytes[1] = modulationControllers[index];
            return true;
        }
        if ((state.expressionMasks[channel] & bit) != 0U) {
            state.expressionMasks[channel] =
                static_cast<uint8_t>(state.expressionMasks[channel] & ~bit);
            bytes[1] = expressionControllers[index];
            bytes[2] = 127U;
            return true;
        }
    }
    const uint16_t channelBit = static_cast<uint16_t>(1U << channel);
    if ((state.pitchBendMask & channelBit) != 0U) {
        state.pitchBendMask =
            static_cast<uint16_t>(state.pitchBendMask & ~channelBit);
        bytes[0] = static_cast<uint8_t>(0xe0U | channel);
        bytes[1] = 0U;
        bytes[2] = 64U;
        return true;
    }
    if ((state.channelPressureMask & channelBit) != 0U) {
        state.channelPressureMask =
            static_cast<uint16_t>(state.channelPressureMask & ~channelBit);
        bytes[0] = static_cast<uint8_t>(0xd0U | channel);
        bytes[1] = 0U;
        size = 2U;
        return true;
    }
    for (uint8_t word = 0; word < 4U; ++word) {
        uint32_t& mask = state.polyPressureMasks[channel][word];
        for (uint8_t bit = 0; bit < 32U; ++bit) {
            const uint32_t value = static_cast<uint32_t>(1U) << bit;
            if ((mask & value) != 0U) {
                mask &= ~value;
                bytes[0] = static_cast<uint8_t>(0xa0U | channel);
                bytes[1] = static_cast<uint8_t>(word * 32U + bit);
                return true;
            }
        }
    }
    return false;
}

// Freezes capture at one ending timestamp. Generated endings are appended by
// bounded progress, never interleaved with newly captured events.
bool beginFinalization(Algorithm& algorithm) {
    if (!algorithm.captureEnabled) {
        return false;
    }
    HistoryControl& history = historyControl(algorithm);
    history.endingTemplate = captureEndingTemplate(algorithm);
    history.finalizeCursor = 0;
    history.finalizing = true;
    algorithm.captureEnabled = false;
    return true;
}

const uint32_t kFinalizeNoteSlots = 16U * 128U;

// Appends at most `budget` generated endings in the legacy order by consuming
// the recorded ownership state behind a resumable cursor.
bool finalizeCaptureStep(Algorithm& algorithm, uint32_t budget,
                         uint32_t& appended) {
    HistoryControl& history = historyControl(algorithm);
    if (!history.finalizing) {
        return true;
    }
    RecordedState& state = algorithm.recordedState;
    while (history.finalizeCursor < kFinalizeNoteSlots) {
        const uint8_t channel =
            static_cast<uint8_t>(history.finalizeCursor / 128U);
        const uint8_t note = static_cast<uint8_t>(history.finalizeCursor % 128U);
        uint32_t& count = state.heldNoteCounts[channel][note];
        if (count == 0U) {
            ++history.finalizeCursor;
            continue;
        }
        if (appended == budget) {
            return false;
        }
        appendEnding(algorithm, history.endingTemplate,
                     static_cast<uint8_t>(0x80U | channel), note, 0);
        --count;
        ++appended;
    }
    while (history.finalizeCursor < kFinalizeNoteSlots + 16U) {
        if (appended == budget) {
            return false;
        }
        uint8_t bytes[3] = {0, 0, 0};
        uint8_t size = 3U;
        if (!takeChannelEnding(
                state,
                static_cast<uint8_t>(history.finalizeCursor - kFinalizeNoteSlots),
                bytes, size)) {
            ++history.finalizeCursor;
            continue;
        }
        appendEnding(algorithm, history.endingTemplate, bytes[0], bytes[1],
                     bytes[2], size);
        ++appended;
    }

    algorithm.recordedState = RecordedState();
    history.finalizing = false;
    return true;
}

void completeCaptureFinalization(Algorithm& algorithm) {
    uint32_t appended = 0;
    while (!finalizeCaptureStep(algorithm, kFinalizeAppendsPerStep, appended)) {
        appended = 0;
    }
}

// Legacy synchronous capture stop: the same bounded steps run to completion.
void finalizeCapture(Algorithm& algorithm) {
    beginFinalization(algorithm);
    completeCaptureFinalization(algorithm);
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
    // MPE ownership is cleaned on its latched routes from the cleanup reserve.
    mpeReleaseOwned(algorithm, dispatchSample, true, 0xffffU);
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

uint64_t dispatchSampleFrom(const Algorithm& algorithm,
                            const RecordedEvent& event,
                            uint64_t intervalStart) {
    const uint32_t prediction = boundedInterval(
        predictedClockInterval(algorithm));
    const uint64_t scaledOffset = multiplyDivideRounded(
        event.offsetSamples, prediction, event.sourceIntervalSamples);
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    return scaledOffset > maximum - intervalStart ? maximum
                                                  : intervalStart + scaledOffset;
}

uint64_t eventDispatchSample(const Algorithm& algorithm,
                             const RecordedEvent& event) {
    return dispatchSampleFrom(algorithm, event,
                              algorithm.playbackIntervalStartSample);
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

// Bit zero is the generated-ending tag. Bits 1..31 hold a checked index+1
// (zero means absent); oversized indices are never encoded.
uint32_t encodedEndingIndex(uint32_t eventIndex) {
    return eventIndex < 0x7fffffffU
               ? (eventIndex + 1U) << kRecordedEventEndingIndexShift
               : 0U;
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

void linkRecordedEvent(RecordedEvent& event, uint32_t targetIndex) {
    event.flags &= kRecordedEventFlagCaptureEnding;
    event.flags |= encodedEndingIndex(targetIndex);
}

void beginRecordedEndingPairing(Algorithm& algorithm) {
    HistoryControl& history = historyControl(algorithm);
    clearPendingPlaybackEndings(algorithm);
    const uint32_t none = algorithm.eventCount;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        for (uint8_t note = 0; note < 128U; ++note) {
            algorithm.pendingEndings.noteEventIndices[channel][note] = none;
            algorithm.pendingEndings.noteDueIntervals[channel][note] = none;
        }
        algorithm.pendingEndings.sustainEventIndices[channel] = none;
        algorithm.pendingEndings.sustainDueIntervals[channel] = none;
        for (uint8_t pedal = 0; pedal < 2U; ++pedal) {
            history.pedalHeads[channel][pedal] = none;
            history.pedalTails[channel][pedal] = none;
        }
    }
    history.pairingCursor = 0;
    history.pairingWalkPress = none;
    history.pairingWalkRelease = none;
    history.pairingGeneration = history.generation;
    history.pairingComplete = false;
    history.pairing = true;
}

// A press queues FIFO behind earlier presses; a release starts a resumable
// walk that links every queued press to it.
void pairPedalEvent(Algorithm& algorithm, RecordedEvent& event,
                    uint32_t index, uint32_t& head, uint64_t& tail) {
    const uint32_t none = algorithm.eventCount;
    if (event.bytes[2] >= 64U) {
        if (tail == none) {
            head = index;
        } else {
            recordedEventByIndex(algorithm, static_cast<uint32_t>(tail))
                ->flags |= encodedEndingIndex(index);
        }
        tail = index;
        return;
    }
    HistoryControl& history = historyControl(algorithm);
    history.pairingWalkPress = head;
    history.pairingWalkRelease = index;
    head = none;
    tail = none;
}

// One record visit of the existing FIFO occurrence pairing. A queued note-on
// temporarily links to the next queued attack until its note-off pops it.
void pairRecordedEvent(Algorithm& algorithm, uint32_t index) {
    const uint32_t none = algorithm.eventCount;
    RecordedEvent* event = recordedEventByIndex(algorithm, index);
    event->flags &= kRecordedEventFlagCaptureEnding;
    const uint8_t type = event->bytes[0] & 0xf0U;
    const uint8_t channel = event->bytes[0] & 0x0fU;
    const uint8_t note = event->bytes[1] & 0x7fU;
    const bool noteOn = type == 0x90U && event->bytes[2] != 0U;
    const bool noteOff = type == 0x80U ||
                         (type == 0x90U && event->bytes[2] == 0U);
    PendingPlaybackEndings& work = algorithm.pendingEndings;
    if (noteOn) {
        const uint32_t tail =
            static_cast<uint32_t>(work.noteDueIntervals[channel][note]);
        if (tail == none) {
            work.noteEventIndices[channel][note] = index;
        } else {
            recordedEventByIndex(algorithm, tail)->flags |=
                encodedEndingIndex(index);
        }
        work.noteDueIntervals[channel][note] = index;
    } else if (noteOff) {
        const uint32_t head = work.noteEventIndices[channel][note];
        if (head != none) {
            RecordedEvent* attack = recordedEventByIndex(algorithm, head);
            uint32_t next = none;
            decodedEndingIndex(*attack, next);
            linkRecordedEvent(*attack, index);
            work.noteEventIndices[channel][note] = next;
            if (next == none) {
                work.noteDueIntervals[channel][note] = none;
            }
        }
    } else if (type == 0xb0U && event->bytes[1] == 64U) {
        pairPedalEvent(algorithm, *event, index,
                       work.sustainEventIndices[channel],
                       work.sustainDueIntervals[channel]);
    } else if (type == 0xb0U &&
               (event->bytes[1] == 66U || event->bytes[1] == 69U)) {
        HistoryControl& history = historyControl(algorithm);
        const uint8_t pedal = event->bytes[1] == 66U ? 0U : 1U;
        pairPedalEvent(algorithm, *event, index,
                       history.pedalHeads[channel][pedal],
                       history.pedalTails[channel][pedal]);
    }
}

// Pairs the frozen history once per generation, charging every record and
// release-walk visit to `budget`. Returns true once links are complete.
bool pairRecordedEndingsStep(Algorithm& algorithm, uint32_t& budget) {
    HistoryControl& history = historyControl(algorithm);
    if (history.pairingComplete &&
        history.pairedGeneration == history.generation) {
        return true;
    }
    if (!history.pairing || history.pairingGeneration != history.generation) {
        beginRecordedEndingPairing(algorithm);
    }
    const uint32_t none = algorithm.eventCount;
    while (budget != 0U) {
        if (history.pairingWalkPress != none) {
            RecordedEvent* press =
                recordedEventByIndex(algorithm, history.pairingWalkPress);
            uint32_t next = none;
            decodedEndingIndex(*press, next);
            linkRecordedEvent(*press, history.pairingWalkRelease);
            history.pairingWalkPress = next;
        } else if (history.pairingCursor < algorithm.eventCount) {
            pairRecordedEvent(algorithm, history.pairingCursor);
            ++history.pairingCursor;
        } else {
            break;
        }
        --budget;
    }
    if (history.pairingWalkPress != none ||
        history.pairingCursor < algorithm.eventCount) {
        return false;
    }
    clearPendingPlaybackEndings(algorithm);
    history.pairing = false;
    history.pairingComplete = true;
    history.pairedGeneration = history.generation;
    return true;
}

// Legacy synchronous entry: completes (or reuses) this generation's pairing.
void prepareRecordedEndingOwnership(Algorithm& algorithm) {
    uint32_t unlimited = kHistoryNoIndex;
    pairRecordedEndingsStep(algorithm, unlimited);
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

// ---------------------------------------------------------------------------
// MPE playback: indexed ownership, bounded pending heap and cluster admission.
// Ordinary mode never enters these paths.

bool mpeModeOn(const Algorithm& algorithm) {
    return algorithm.v != NULL && algorithm.v[kParameterMpeMode] != 0;
}

uint32_t destinationFanout(uint32_t destination) {
    uint32_t count = 0U;
    for (uint32_t bit = 1U; bit <= 8U; bit <<= 1U) {
        if ((destination & bit) != 0U) {
            ++count;
        }
    }
    return count;
}

void noteMpeMaximum(MpePlayback& mpe) {
    noteMaximum(mpe.maximum.visits, mpe.step.visits);
    noteMaximum(mpe.maximum.sends, mpe.step.sends);
    noteMaximum(mpe.maximum.fanout, mpe.step.fanout);
    noteMaximum(mpe.maximum.heap, mpe.step.heap);
    noteMaximum(mpe.maximum.cleanupCalls, mpe.step.cleanupCalls);
    noteMaximum(mpe.maximum.cleanupFanout, mpe.step.cleanupFanout);
}

void mpeSend(Algorithm& algorithm, uint32_t destination, uint8_t status,
             uint8_t data1, uint8_t data2, uint8_t size, uint64_t sample,
             bool cleanup) {
    MpePlayback& mpe = mpeState(algorithm);
    if (size == 2U) {
        nt_host::sendMidi2(destination, status, data1, sample);
    } else {
        nt_host::sendMidi3(destination, status, data1, data2, sample);
    }
    const uint32_t fanout = destinationFanout(destination);
    if (cleanup) {
        ++mpe.step.cleanupCalls;
        mpe.step.cleanupFanout += fanout;
    } else {
        ++mpe.step.sends;
        mpe.step.fanout += fanout;
    }
    noteMpeMaximum(mpe);
}

bool mpeHoldController(uint8_t controller) {
    return controller == 64U || controller == 66U || controller == 69U;
}

uint16_t mpeNoteKey(uint8_t channel, uint8_t note) {
    return static_cast<uint16_t>(channel * 128U + (note & 0x7fU));
}

uint16_t mpeHoldKey(uint8_t channel, uint8_t controller) {
    const uint8_t pedal = controller == 64U ? 0U : controller == 66U ? 1U : 2U;
    return static_cast<uint16_t>(kMpeNoteKeys + channel * 3U + pedal);
}

uint8_t mpeKeyChannel(uint16_t key) {
    return static_cast<uint8_t>(key < kMpeNoteKeys ? key / 128U
                                                   : (key - kMpeNoteKeys) / 3U);
}

uint8_t mpeHoldKeyController(uint16_t key) {
    static const uint8_t kControllers[3] = {64U, 66U, 69U};
    return kControllers[(key - kMpeNoteKeys) % 3U];
}

bool mpeOwned(const MpePlayback& mpe, uint16_t key) {
    return key < kMpeNoteKeys
               ? mpe.owners[key].ownedPosition != kMpeNoSlot
               : ((mpe.holdMask >> (key - kMpeNoteKeys)) & 1U) != 0U;
}

void mpeOwn(MpePlayback& mpe, uint16_t key, uint32_t destination) {
    MpeOwner& owner = mpe.owners[key];
    owner.destination = static_cast<uint8_t>(destination);
    if (key >= kMpeNoteKeys) {
        mpe.holdMask |= static_cast<uint64_t>(1U) << (key - kMpeNoteKeys);
    } else if (owner.ownedPosition == kMpeNoSlot) {
        owner.ownedPosition = static_cast<uint16_t>(mpe.ownedNoteCount);
        mpe.ownedNotes[mpe.ownedNoteCount++] = key;
    }
}

void mpeDisown(MpePlayback& mpe, uint16_t key) {
    if (key >= kMpeNoteKeys) {
        mpe.holdMask &= ~(static_cast<uint64_t>(1U) << (key - kMpeNoteKeys));
        return;
    }
    MpeOwner& owner = mpe.owners[key];
    if (owner.ownedPosition == kMpeNoSlot) {
        return;
    }
    const uint16_t last = mpe.ownedNotes[--mpe.ownedNoteCount];
    mpe.ownedNotes[owner.ownedPosition] = last;
    mpe.owners[last].ownedPosition = owner.ownedPosition;
    owner.ownedPosition = kMpeNoSlot;
}

uint32_t mpeOwnedCount(const MpePlayback& mpe) {
    uint32_t holds = 0U;
    for (uint64_t mask = mpe.holdMask; mask != 0U; mask &= mask - 1U) {
        ++holds;
    }
    return mpe.ownedNoteCount + holds;
}

// Pending endings order by due interval, then the exact recorded phase
// offset/sourceInterval (cross-multiplied), then receipt index and key.
bool mpeEarlier(Algorithm& algorithm, uint16_t left, uint16_t right) {
    MpePlayback& mpe = mpeState(algorithm);
    ++mpe.step.heap;
    const MpeOwner& a = mpe.owners[left];
    const MpeOwner& b = mpe.owners[right];
    if (a.dueInterval != b.dueInterval) {
        return a.dueInterval < b.dueInterval;
    }
    const RecordedEvent* ea = recordedEventByIndex(algorithm, a.endingIndex);
    const RecordedEvent* eb = recordedEventByIndex(algorithm, b.endingIndex);
    if (ea != NULL && eb != NULL) {
        const uint64_t na = ea->sourceIntervalSamples == 0U ? 0U : ea->offsetSamples;
        const uint64_t da = ea->sourceIntervalSamples == 0U ? 1U : ea->sourceIntervalSamples;
        const uint64_t nb = eb->sourceIntervalSamples == 0U ? 0U : eb->offsetSamples;
        const uint64_t db = eb->sourceIntervalSamples == 0U ? 1U : eb->sourceIntervalSamples;
        if (na * db != nb * da) {
            return na * db < nb * da;
        }
    }
    if (a.endingIndex != b.endingIndex) {
        return a.endingIndex < b.endingIndex;
    }
    return left < right;
}

void mpeHeapSet(MpePlayback& mpe, uint32_t position, uint16_t key) {
    mpe.heap[position] = key;
    mpe.owners[key].heapPosition = static_cast<uint16_t>(position);
}

void mpeHeapUp(Algorithm& algorithm, uint32_t position) {
    MpePlayback& mpe = mpeState(algorithm);
    const uint16_t key = mpe.heap[position];
    while (position != 0U) {
        const uint32_t parent = (position - 1U) / 2U;
        if (!mpeEarlier(algorithm, key, mpe.heap[parent])) {
            break;
        }
        mpeHeapSet(mpe, position, mpe.heap[parent]);
        ++mpe.step.heap;
        position = parent;
    }
    mpeHeapSet(mpe, position, key);
}

void mpeHeapDown(Algorithm& algorithm, uint32_t position) {
    MpePlayback& mpe = mpeState(algorithm);
    const uint16_t key = mpe.heap[position];
    while (true) {
        uint32_t child = position * 2U + 1U;
        if (child >= mpe.heapSize) {
            break;
        }
        if (child + 1U < mpe.heapSize &&
            mpeEarlier(algorithm, mpe.heap[child + 1U], mpe.heap[child])) {
            ++child;
        }
        if (!mpeEarlier(algorithm, mpe.heap[child], key)) {
            break;
        }
        mpeHeapSet(mpe, position, mpe.heap[child]);
        ++mpe.step.heap;
        position = child;
    }
    mpeHeapSet(mpe, position, key);
}

void mpeCancelPending(Algorithm& algorithm, uint16_t key) {
    MpePlayback& mpe = mpeState(algorithm);
    MpeOwner& owner = mpe.owners[key];
    if (owner.heapPosition == kMpeNoSlot) {
        return;
    }
    const uint32_t position = owner.heapPosition;
    owner.heapPosition = kMpeNoSlot;
    owner.endingIndex = kMpeNoEnding;
    --mpe.heapSize;
    if (position == mpe.heapSize) {
        return;
    }
    const uint16_t moved = mpe.heap[mpe.heapSize];
    mpeHeapSet(mpe, position, moved);
    mpeHeapUp(algorithm, position);
    mpeHeapDown(algorithm, mpe.owners[moved].heapPosition);
}

void mpeSchedulePending(Algorithm& algorithm, uint16_t key,
                        const RecordedEvent& beginning) {
    uint32_t endingIndex = 0U;
    if (!findRecordedEnding(algorithm, beginning, endingIndex)) {
        return;
    }
    const RecordedEvent* ending = recordedEventByIndex(algorithm, endingIndex);
    MpePlayback& mpe = mpeState(algorithm);
    mpeCancelPending(algorithm, key);
    MpeOwner& owner = mpe.owners[key];
    owner.endingIndex = endingIndex;
    owner.dueInterval =
        algorithm.playbackIntervalOrdinal + ending->pulse - beginning.pulse;
    mpeHeapSet(mpe, mpe.heapSize, key);
    ++mpe.heapSize;
    mpeHeapUp(algorithm, mpe.heapSize - 1U);
}

// Ends latched ownership on its original channel and emitted route. Cleanup
// bypasses filters/remapping and discards pending endings without heap work.
// Notes end before CC64/66/69 holds.
void mpeReleaseOwned(Algorithm& algorithm, uint64_t sample, bool cleanup,
                     uint16_t channelMask) {
    MpePlayback& mpe = mpeState(algorithm);
    for (uint32_t index = mpe.ownedNoteCount; index-- > 0U;) {
        const uint16_t key = mpe.ownedNotes[index];
        const uint8_t channel = mpeKeyChannel(key);
        if (((channelMask >> channel) & 1U) == 0U) {
            continue;
        }
        MpeOwner& owner = mpe.owners[key];
        if (cleanup) {
            owner.heapPosition = kMpeNoSlot;
            owner.endingIndex = kMpeNoEnding;
        } else {
            mpeCancelPending(algorithm, key);
        }
        mpeSend(algorithm, owner.destination,
                static_cast<uint8_t>(0x80U | channel),
                static_cast<uint8_t>(key % 128U), 0U, 3U, sample, cleanup);
        ++mpe.generatedCleanup;
        mpeDisown(mpe, key);
    }
    for (uint32_t bit = 0; bit < 48U; ++bit) {
        if (((mpe.holdMask >> bit) & 1U) == 0U) {
            continue;
        }
        const uint16_t key = static_cast<uint16_t>(kMpeNoteKeys + bit);
        const uint8_t channel = mpeKeyChannel(key);
        if (((channelMask >> channel) & 1U) == 0U) {
            continue;
        }
        MpeOwner& owner = mpe.owners[key];
        if (cleanup) {
            owner.heapPosition = kMpeNoSlot;
            owner.endingIndex = kMpeNoEnding;
        } else {
            mpeCancelPending(algorithm, key);
        }
        mpeSend(algorithm, owner.destination,
                static_cast<uint8_t>(0xb0U | channel),
                mpeHoldKeyController(key), 0U, 3U, sample, cleanup);
        ++mpe.generatedCleanup;
        mpeDisown(mpe, key);
    }
    if (cleanup) {
        // Pending endings exist only for owned keys, all discarded above.
        mpe.heapSize = 0U;
    }
}

// Silent forgetting for discarded state (load/clear). No output is sent.
void mpeForgetOwners(Algorithm& algorithm) {
    MpePlayback& mpe = mpeState(algorithm);
    for (uint32_t index = 0; index < mpe.ownedNoteCount; ++index) {
        mpe.owners[mpe.ownedNotes[index]].ownedPosition = kMpeNoSlot;
    }
    for (uint32_t index = 0; index < mpe.heapSize; ++index) {
        mpe.owners[mpe.heap[index]].heapPosition = kMpeNoSlot;
        mpe.owners[mpe.heap[index]].endingIndex = kMpeNoEnding;
    }
    mpe.ownedNoteCount = 0U;
    mpe.heapSize = 0U;
    mpe.holdMask = 0U;
}

// Binary search over the pulse-ordered ring (<= 18 visits for 208,333).
uint32_t mpeLowerBound(Algorithm& algorithm, uint64_t pulse) {
    MpePlayback& mpe = mpeState(algorithm);
    uint32_t low = 0U;
    uint32_t high = algorithm.eventCount;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2U;
        ++mpe.step.visits;
        if (recordedEventByIndex(algorithm, middle)->pulse < pulse) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    noteMpeMaximum(mpe);
    return low;
}

uint8_t mpeControlStatus(const Algorithm& algorithm) {
    const int16_t* v = algorithm.v;
    if (v == NULL) {
        return kMpeStatusInvalidScope;
    }
    if (v[kParameterRecordingChannel] != 0) {
        return kMpeStatusRecordChannel;
    }
    if (v[kParameterPlaybackChannel] != 0) {
        return kMpeStatusPlayChannel;
    }
    if (v[kParameterFilterControlChange] != 0 ||
        v[kParameterFilterPitchBend] != 0 ||
        v[kParameterFilterAftertouch] != 0) {
        return kMpeStatusFilters;
    }
    if (v[kParameterMpeSetupSource] != 0) {
        const int lower = v[kParameterLowerMembers];
        const int upper = v[kParameterUpperMembers];
        if (lower < 0 || lower > 15 || upper < 0 || upper > 15 ||
            (lower == 0 && upper == 0) ||
            (lower != 0 && upper != 0 && lower + upper > 14)) {
            return kMpeStatusInvalidScope;
        }
    }
    return kMpeStatusReady;
}

// Observed scope uses known historical counts; Manual is a receiver-matching
// assertion that must agree with every known (including disabled) zone.
uint8_t mpeResolveScope(const Algorithm& algorithm, const HistoryState& context,
                        uint8_t& lower, uint8_t& upper) {
    const uint8_t status = mpeControlStatus(algorithm);
    if (status != kMpeStatusReady) {
        return status;
    }
    if (algorithm.v[kParameterMpeSetupSource] != 0) {
        lower = static_cast<uint8_t>(algorithm.v[kParameterLowerMembers]);
        upper = static_cast<uint8_t>(algorithm.v[kParameterUpperMembers]);
        const uint8_t manual[2] = {lower, upper};
        for (unsigned zone = 0; zone < 2U; ++zone) {
            if (context.zoneCountState[zone] == kSetupKnown &&
                context.zoneCount[zone] != manual[zone]) {
                return kMpeStatusMismatch;
            }
        }
        return kMpeStatusReady;
    }
    lower = context.zoneCountState[0] == kSetupKnown ? context.zoneCount[0] : 0U;
    upper = context.zoneCountState[1] == kSetupKnown ? context.zoneCount[1] : 0U;
    return lower == 0U && upper == 0U ? kMpeStatusScopeUnknown
                                      : kMpeStatusReady;
}

uint16_t mpeScopeMask(uint8_t lower, uint8_t upper) {
    uint16_t mask = 0U;
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        if ((lower != 0U && channel <= lower) ||
            (upper != 0U && channel >= 15U - upper)) {
            mask = static_cast<uint16_t>(mask | (1U << channel));
        }
    }
    return mask;
}

// Generated setup/context output. With emit=false it only counts sends so
// admission can preflight the identical sequence.
struct MpeEmitter {
    Algorithm* algorithm;
    uint32_t destination;
    uint64_t sample;
    uint32_t* generated;
    uint32_t count;
    bool emit;
};

void mpeEmit(MpeEmitter& out, uint8_t status, uint8_t data1, uint8_t data2,
             uint8_t size = 3U) {
    if (out.emit) {
        mpeSend(*out.algorithm, out.destination, status, data1, data2, size,
                out.sample, false);
        ++*out.generated;
    }
    ++out.count;
}

// Absolute RPN 0/x transaction with explicit selectors and null hygiene.
void mpeRpn(MpeEmitter& out, uint8_t channel, uint8_t parameter, uint8_t msb,
            int lsb) {
    const uint8_t status = static_cast<uint8_t>(0xb0U | channel);
    mpeEmit(out, status, 101U, 0U);
    mpeEmit(out, status, 100U, parameter);
    mpeEmit(out, status, 6U, msb);
    if (lsb >= 0) {
        mpeEmit(out, status, 38U, static_cast<uint8_t>(lsb));
    }
    mpeEmit(out, status, 101U, 127U);
    mpeEmit(out, status, 100U, 127U);
}

void mpeRange(MpeEmitter& out, uint8_t channel, uint16_t cents) {
    mpeRpn(out, channel, 0U, static_cast<uint8_t>(cents / 100U),
           cents % 100U);
}

void mpeMemberRanges(MpeEmitter& out, unsigned zone, uint8_t count,
                     uint16_t cents) {
    for (uint8_t member = 1; member <= count; ++member) {
        mpeRange(out, static_cast<uint8_t>(zone == 0U ? member : 15U - member),
                 cents);
    }
}

// Restores each independently safe known zone of the selected take. An MCM
// is sent only when it cannot reset an indeterminate range; Manual scope
// never sends MCM and restores only unambiguous known RPN0 ranges.
uint32_t mpeSetupPlan(Algorithm& algorithm, const HistoryState& context,
                      bool emit, uint64_t sample, bool& incomplete) {
    MpePlayback& mpe = mpeState(algorithm);
    MpeEmitter out = {&algorithm, playbackDestinationMask(algorithm), sample,
                      &mpe.generatedSetup, 0U, emit};
    incomplete = false;
    if (algorithm.v[kParameterMpeSetupSource] == 0) {
        // Disabled zones first, so no later MCM can shrink a restored zone.
        for (unsigned pass = 0; pass < 2U; ++pass) {
            for (unsigned zone = 0; zone < 2U; ++zone) {
                if (context.zoneCountState[zone] != kSetupKnown) {
                    incomplete = incomplete ||
                                 (pass == 0U && context.zoneCountState[zone] ==
                                                    kSetupIndeterminate);
                    continue;
                }
                const uint8_t count = context.zoneCount[zone];
                if ((count != 0U) != (pass == 1U)) {
                    continue;
                }
                const uint8_t manager = zone == 0U ? 0U : 15U;
                if (count == 0U) {
                    // Implied by an opposite count of 14/15, whose MCM follows.
                    const unsigned opposite = 1U - zone;
                    if (context.zoneCountState[opposite] != kSetupKnown ||
                        context.zoneCount[opposite] < 14U) {
                        mpeRpn(out, manager, 6U, 0U, -1);
                    }
                    continue;
                }
                const bool managerKnown = context.managerState[zone] == kSetupKnown;
                const bool memberKnown = context.memberState[zone] == kSetupKnown;
                if (managerKnown && memberKnown) {
                    mpeRpn(out, manager, 6U, count, -1);
                } else {
                    incomplete = true;
                }
                if (managerKnown) {
                    mpeRange(out, manager, context.managerCents[zone]);
                }
                if (memberKnown) {
                    mpeMemberRanges(out, zone, count, context.memberCents[zone]);
                }
            }
        }
        return out.count;
    }
    for (unsigned zone = 0; zone < 2U; ++zone) {
        const uint8_t count = static_cast<uint8_t>(
            algorithm.v[zone == 0U ? kParameterLowerMembers
                                   : kParameterUpperMembers]);
        if (count == 0U) {
            continue;
        }
        const uint8_t manager = zone == 0U ? 0U : 15U;
        bool managerKnown = false;
        bool memberKnown = false;
        uint16_t managerCents = 0U;
        uint16_t memberCents = 0U;
        if (context.zoneCountState[zone] == kSetupKnown) {
            managerKnown = context.managerState[zone] == kSetupKnown;
            memberKnown = context.memberState[zone] == kSetupKnown;
            managerCents = context.managerCents[zone];
            memberCents = context.memberCents[zone];
        } else if (context.zoneCountState[zone] == kSetupUnknown) {
            managerKnown = context.channelCentsState[manager] == kSetupKnown;
            managerCents = context.channelCents[manager];
            bool conflict = false;
            for (uint8_t member = 1; member <= count; ++member) {
                const uint8_t channel =
                    static_cast<uint8_t>(zone == 0U ? member : 15U - member);
                if (context.channelCentsState[channel] != kSetupKnown) {
                    continue;
                }
                if (memberKnown && context.channelCents[channel] != memberCents) {
                    conflict = true;
                }
                memberKnown = true;
                memberCents = context.channelCents[channel];
            }
            memberKnown = memberKnown && !conflict;
        }
        incomplete = incomplete || !managerKnown || !memberKnown;
        if (managerKnown) {
            mpeRange(out, manager, managerCents);
        }
        if (memberKnown) {
            mpeMemberRanges(out, zone, count, memberCents);
        }
    }
    return out.count;
}

bool historyKnown(const HistoryState& state, uint8_t channel, HistoryValue value) {
    return ((state.known[channel] >> value) & 1U) != 0U;
}

// Known starting context for in-scope channels. Partial 14-bit bend is never
// sent; an LSB is sent only after its known MSB. Known pressed holds latch
// ownership so the next boundary releases them.
uint32_t mpeContextPlan(Algorithm& algorithm, const HistoryState& context,
                        const HistoryContextMeta& meta, bool emit,
                        uint64_t sample, bool& incomplete) {
    MpePlayback& mpe = mpeState(algorithm);
    MpeEmitter out = {&algorithm, playbackDestinationMask(algorithm), sample,
                      &mpe.generatedContext, 0U, emit};
    incomplete = !meta.coverageComplete;
    static const uint8_t kControllers[4][2] = {
        {74U, kHistoryTimbre}, {1U, kHistoryModulationMsb},
        {33U, kHistoryModulationLsb}, {11U, kHistoryExpressionMsb}};
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        if (((mpe.scopeMask >> channel) & 1U) == 0U) {
            continue;
        }
        const uint8_t* values = context.values[channel];
        const uint8_t control = static_cast<uint8_t>(0xb0U | channel);
        const bool bend = historyKnown(context, channel, kHistoryBendLsb) &&
                          historyKnown(context, channel, kHistoryBendMsb);
        const bool pressure =
            historyKnown(context, channel, kHistoryChannelPressure);
        if (bend) {
            mpeEmit(out, static_cast<uint8_t>(0xe0U | channel),
                    values[kHistoryBendLsb], values[kHistoryBendMsb]);
        }
        if (pressure) {
            mpeEmit(out, static_cast<uint8_t>(0xd0U | channel),
                    values[kHistoryChannelPressure], 0U, 2U);
        }
        for (unsigned index = 0; index < 4U; ++index) {
            const HistoryValue value =
                static_cast<HistoryValue>(kControllers[index][1]);
            const bool lsbWithoutMsb =
                value == kHistoryModulationLsb &&
                !historyKnown(context, channel, kHistoryModulationMsb);
            if (historyKnown(context, channel, value) && !lsbWithoutMsb) {
                mpeEmit(out, control, kControllers[index][0], values[value]);
            }
        }
        if (historyKnown(context, channel, kHistoryExpressionMsb) &&
            historyKnown(context, channel, kHistoryExpressionLsb)) {
            mpeEmit(out, control, 43U, values[kHistoryExpressionLsb]);
        }
        static const uint8_t kHolds[3][2] = {
            {64U, kHistorySustain}, {66U, kHistorySostenuto}, {69U, kHistoryHold2}};
        for (unsigned index = 0; index < 3U; ++index) {
            const HistoryValue value = static_cast<HistoryValue>(kHolds[index][1]);
            if (historyKnown(context, channel, value) && values[value] >= 64U) {
                mpeEmit(out, control, kHolds[index][0], values[value]);
                if (emit) {
                    const uint16_t key = mpeHoldKey(channel, kHolds[index][0]);
                    mpeCancelPending(algorithm, key);
                    mpeOwn(mpe, key, out.destination);
                }
            }
        }
        incomplete = incomplete || !bend || !pressure ||
                     !historyKnown(context, channel, kHistoryTimbre);
    }
    return out.count;
}

uint64_t dispatchSampleFrom(const Algorithm& algorithm,
                            const RecordedEvent& event, uint64_t intervalStart);

// A rejected cluster emits none of its normal bytes: prior emitted ownership
// is cleaned on latched routes within the reserve, pending/attacks are
// cancelled, Playback is set Off and the overload stays visible until a fresh
// Off/On.
void mpeRejectCluster(Algorithm& algorithm, uint64_t sample) {
    MpePlayback& mpe = mpeState(algorithm);
    ++mpe.rejectedClusters;
    mpe.fault = true;
    mpe.status = kMpeStatusOverload;
    mpeReleaseOwned(algorithm, sample, true, 0xffffU);
    algorithm.transportState = kTransportStopped;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
    mpe.engaged = false;
    mpe.deliberate = false;
    mpe.entryPending = false;
    mpe.contextPending = false;
    if (algorithm.v != NULL && algorithm.v[kParameterPlayback] != 0) {
        nt_host::setParameterFromAudio(&algorithm, kParameterPlayback, 0);
    }
}

// Preflights one due-frame cluster (plus its entry/wrap work) against the
// remaining step budgets before any output or cursor consumption. Costs are
// upper bounds; source and heap visits made here are charged once.
bool mpeAdmitCluster(Algorithm& algorithm, uint64_t sample, bool force,
                     uint32_t cursor, uint64_t pulse, const PulseRange& range,
                     uint64_t intervalStart, uint64_t ordinal,
                     uint64_t extraSends, uint64_t extraHeap) {
    MpePlayback& mpe = mpeState(algorithm);
    uint64_t sends = extraSends;
    uint64_t heap = extraHeap;
    bool fits = true;
    for (uint32_t index = cursor; fits; ++index) {
        const RecordedEvent* event = recordedEventByIndex(algorithm, index);
        if (event == NULL || event->pulse != pulse || pulse >= range.endPulse ||
            (!force && dispatchSampleFrom(algorithm, *event, intervalStart) >
                           sample)) {
            break;
        }
        if (mpe.step.visits >= kMpeStepVisits) {
            fits = false;
            break;
        }
        ++mpe.step.visits;
        const uint8_t type = event->bytes[0] & 0xf0U;
        const bool hold = type == 0xb0U && mpeHoldController(event->bytes[1]);
        if (type == 0x90U && event->bytes[2] != 0U) {
            sends += 2U; // collision ending plus attack
            heap += kMpeHeapRemoveCost + kMpeHeapPushCost;
        } else if (hold && event->bytes[2] >= 64U) {
            sends += 1U;
            heap += kMpeHeapRemoveCost + kMpeHeapPushCost;
        } else if (type == 0x80U || type == 0x90U || hold) {
            sends += 1U;
            heap += kMpeHeapRemoveCost;
        } else {
            sends += 1U;
        }
        fits = mpe.step.sends + sends <= kMpeStepSends;
    }
    // Due pending endings form a heap prefix: visit only due nodes and their
    // immediate children.
    uint32_t top = 0U;
    if (fits && mpe.heapSize != 0U) {
        mpe.scratch[top++] = 0U;
    }
    while (fits && top != 0U) {
        const uint32_t position = mpe.scratch[--top];
        if (mpe.step.heap >= kMpeStepHeap) {
            fits = false;
            break;
        }
        ++mpe.step.heap;
        const MpeOwner& owner = mpe.owners[mpe.heap[position]];
        const RecordedEvent* ending =
            recordedEventByIndex(algorithm, owner.endingIndex);
        const bool due =
            owner.dueInterval < ordinal ||
            (owner.dueInterval == ordinal &&
             (force || ending == NULL ||
              dispatchSampleFrom(algorithm, *ending, intervalStart) <= sample));
        if (!due) {
            continue;
        }
        sends += 1U;
        heap += kMpeHeapRemoveCost;
        for (uint32_t child = position * 2U + 1U;
             child <= position * 2U + 2U && child < mpe.heapSize; ++child) {
            mpe.scratch[top++] = static_cast<uint16_t>(child);
        }
    }
    const uint64_t fanout =
        sends * destinationFanout(playbackDestinationMask(algorithm));
    fits = fits && mpe.step.sends + sends <= kMpeStepSends &&
           mpe.step.fanout + fanout <= kMpeStepFanout &&
           mpe.step.heap + heap <= kMpeStepHeap;
    noteMpeMaximum(mpe);
    if (!fits) {
        mpeRejectCluster(algorithm, sample);
    }
    return fits;
}

void mpeEmitSelectedEvent(Algorithm& algorithm, const RecordedEvent& event,
                          uint32_t eventIndex, uint64_t sample) {
    if (!recordedEventPassesPlaybackFilters(algorithm, event)) {
        return;
    }
    MpePlayback& mpe = mpeState(algorithm);
    const uint32_t destination = playbackDestinationMask(algorithm);
    const uint8_t type = event.bytes[0] & 0xf0U;
    const uint8_t channel = event.bytes[0] & 0x0fU;
    if (type == 0x80U || type == 0x90U) {
        const uint16_t key = mpeNoteKey(channel, event.bytes[1]);
        MpeOwner& owner = mpe.owners[key];
        if (type == 0x80U || event.bytes[2] == 0U) {
            // Only the ending owned by an emitted in-range attack sounds.
            if (owner.heapPosition != kMpeNoSlot &&
                owner.endingIndex == eventIndex &&
                owner.dueInterval == algorithm.playbackIntervalOrdinal) {
                mpeCancelPending(algorithm, key);
                mpeSend(algorithm, owner.destination, event.bytes[0],
                        event.bytes[1], event.bytes[2], 3U, sample, false);
                mpeDisown(mpe, key);
            }
            return;
        }
        if (mpeOwned(mpe, key)) {
            // A same-key retrigger ends the older occurrence first; it cannot
            // inherit the older occurrence's delayed ending.
            mpeCancelPending(algorithm, key);
            mpeSend(algorithm, owner.destination,
                    static_cast<uint8_t>(0x80U | channel), event.bytes[1], 0U,
                    3U, sample, false);
            ++mpe.generatedCleanup;
            mpeDisown(mpe, key);
        }
        mpeSend(algorithm, destination, event.bytes[0], event.bytes[1],
                event.bytes[2], 3U, sample, false);
        mpeOwn(mpe, key, destination);
        mpeSchedulePending(algorithm, key, event);
        return;
    }
    if (type == 0xb0U && mpeHoldController(event.bytes[1])) {
        const uint16_t key = mpeHoldKey(channel, event.bytes[1]);
        MpeOwner& owner = mpe.owners[key];
        if (event.bytes[2] >= 64U) {
            // A newer press silently takes release ownership.
            mpeCancelPending(algorithm, key);
            mpeSend(algorithm, destination, event.bytes[0], event.bytes[1],
                    event.bytes[2], 3U, sample, false);
            mpeOwn(mpe, key, destination);
            mpeSchedulePending(algorithm, key, event);
            return;
        }
        if (owner.heapPosition != kMpeNoSlot) {
            if (owner.endingIndex == eventIndex &&
                owner.dueInterval == algorithm.playbackIntervalOrdinal) {
                mpeCancelPending(algorithm, key);
                mpeSend(algorithm, owner.destination, event.bytes[0],
                        event.bytes[1], event.bytes[2], 3U, sample, false);
                mpeDisown(mpe, key);
            }
            // Otherwise this stale release belongs to a cancelled older press
            // and cannot release the newer owner.
            return;
        }
        mpeSend(algorithm,
                mpeOwned(mpe, key) ? owner.destination : destination,
                event.bytes[0], event.bytes[1], event.bytes[2], 3U, sample,
                false);
        mpeDisown(mpe, key);
        return;
    }
    mpeSend(algorithm, destination, event.bytes[0], event.bytes[1],
            event.bytes[2], event.size, sample, false);
}

bool mpePendingDue(Algorithm& algorithm, uint64_t sample, bool force,
                   uint64_t& dueSample) {
    const MpePlayback& mpe = mpeState(algorithm);
    if (mpe.heapSize == 0U) {
        return false;
    }
    const MpeOwner& owner = mpe.owners[mpe.heap[0]];
    if (owner.dueInterval > algorithm.playbackIntervalOrdinal) {
        return false;
    }
    const RecordedEvent* ending =
        recordedEventByIndex(algorithm, owner.endingIndex);
    dueSample = owner.dueInterval < algorithm.playbackIntervalOrdinal ||
                        ending == NULL
                    ? algorithm.playbackIntervalStartSample
                    : eventDispatchSample(algorithm, *ending);
    return force || dueSample <= sample;
}

void mpeEmitPendingTop(Algorithm& algorithm, uint64_t sample) {
    MpePlayback& mpe = mpeState(algorithm);
    const uint16_t key = mpe.heap[0];
    const MpeOwner& owner = mpe.owners[key];
    const RecordedEvent* ending =
        recordedEventByIndex(algorithm, owner.endingIndex);
    const uint32_t destination = owner.destination;
    mpeCancelPending(algorithm, key);
    if (ending != NULL) {
        mpeSend(algorithm, destination, ending->bytes[0], ending->bytes[1],
                ending->bytes[2], 3U, sample, false);
    }
    mpeDisown(mpe, key);
}

void mpeDispatchPlaybackEvents(Algorithm& algorithm, uint64_t sample,
                               bool force) {
    MpePlayback& mpe = mpeState(algorithm);
    uint64_t pendingSample = 0U;
    const bool regularDue =
        algorithm.playbackNextEventScheduled &&
        (force || algorithm.playbackNextEventSample <= sample);
    if (!regularDue && !mpePendingDue(algorithm, sample, force, pendingSample)) {
        return;
    }
    if (force || !mpe.admittedValid || mpe.admittedSample != sample) {
        if (!mpeAdmitCluster(algorithm, sample, force,
                             algorithm.playbackEventIndex,
                             algorithm.playbackPulse, algorithm.activeSelection,
                             algorithm.playbackIntervalStartSample,
                             algorithm.playbackIntervalOrdinal, 0U, 0U)) {
            return;
        }
        if (!force) {
            mpe.admittedValid = true;
            mpe.admittedSample = sample;
        }
    }
    while (algorithm.transportState == kTransportPlaying) {
        const bool eventDue =
            algorithm.playbackNextEventScheduled &&
            (force || algorithm.playbackNextEventSample <= sample);
        const bool pendingDue =
            mpePendingDue(algorithm, sample, force, pendingSample);
        if (!eventDue && !pendingDue) {
            break;
        }
        // Selected recorded messages win ties with pending endings.
        if (eventDue && (!pendingDue ||
                         algorithm.playbackNextEventSample <= pendingSample)) {
            const uint32_t eventIndex = algorithm.playbackEventIndex;
            const RecordedEvent* event = nextPlaybackEvent(algorithm);
            ++algorithm.playbackEventIndex;
            mpeEmitSelectedEvent(algorithm, *event, eventIndex, sample);
            scheduleNextPlaybackEvent(algorithm);
        } else {
            mpeEmitPendingTop(algorithm, sample);
        }
    }
}

// MPE wrap: owned in-scope note endings, then owned holds, stale-occurrence
// cancellation, known starting context, then the next pass's attacks. No
// MCM/RPN or CC120/123 is emitted. A ready, setup-compatible candidate is
// adopted here; an incompatible one keeps the active range and asks for a
// restart; an unready one waits another wrap. Returns false when rejected.
bool mpeWrap(Algorithm& algorithm, uint64_t sample) {
    MpePlayback& mpe = mpeState(algorithm);
    HistoryControl& history = historyControl(algorithm);
    MpeArenaReservation& arena = mpeArena(algorithm);
    const HistoryContextMeta& candidate =
        history.contexts[kHistoryContextCandidate];
    bool adopt = false;
    bool restart = false;
    if (algorithm.rangeTransitionPending && candidate.ready &&
        candidate.generation == history.generation &&
        sameHistoryRange(candidate.range, algorithm.selection)) {
        adopt = candidate.setupFingerprint ==
                history.contexts[kHistoryContextActive].setupFingerprint;
        restart = !adopt;
    }
    const PulseRange range = adopt ? algorithm.selection
                                   : algorithm.activeSelection;
    const HistoryContextSlot slot =
        adopt ? kHistoryContextCandidate : kHistoryContextActive;
    const uint32_t cursor = mpeLowerBound(algorithm, range.startPulse);
    bool incomplete = false;
    const uint32_t contextSends =
        mpeContextPlan(algorithm, arena.contexts[slot], history.contexts[slot],
                       false, sample, incomplete);
    if (!mpeAdmitCluster(algorithm, sample, false, cursor, range.startPulse,
                         range, sample, algorithm.playbackIntervalOrdinal + 1U,
                         mpeOwnedCount(mpe) + contextSends,
                         static_cast<uint64_t>(mpe.heapSize) *
                             kMpeHeapRemoveCost)) {
        return false;
    }
    mpe.admittedValid = true;
    mpe.admittedSample = sample;
    mpeReleaseOwned(algorithm, sample, false, adopt ? 0xffffU : mpe.scopeMask);
    if (adopt) {
        copyHistoryState(arena.contexts[kHistoryContextActive],
                         arena.contexts[kHistoryContextCandidate]);
        history.contexts[kHistoryContextActive] = candidate;
        algorithm.activeSelection = algorithm.selection;
    }
    if (adopt || restart) {
        algorithm.rangeTransitionPending = false;
    }
    mpe.restartRequired = mpe.restartRequired || restart;
    mpeContextPlan(algorithm, arena.contexts[kHistoryContextActive],
                   history.contexts[kHistoryContextActive], true, sample,
                   mpe.contextIncomplete);
    algorithm.playbackPulse = algorithm.activeSelection.startPulse;
    algorithm.playbackEventIndex = cursor;
    return true;
}

// Entry/re-entry cluster: old ownership cleanup, safe known setup (only with
// a deliberate-entry capability), known context, then recorded music.
bool mpeActivate(Algorithm& algorithm, uint64_t sample, bool advance) {
    MpePlayback& mpe = mpeState(algorithm);
    HistoryControl& history = historyControl(algorithm);
    MpeArenaReservation& arena = mpeArena(algorithm);
    const HistoryState& context = arena.contexts[kHistoryContextActive];
    const HistoryContextMeta& meta = history.contexts[kHistoryContextActive];
    bool incomplete = false;
    const uint32_t setupSends =
        mpe.deliberate
            ? mpeSetupPlan(algorithm, context, false, sample, incomplete)
            : 0U;
    const uint32_t contextSends =
        mpe.contextPending
            ? mpeContextPlan(algorithm, context, meta, false, sample, incomplete)
            : 0U;
    if (!mpeAdmitCluster(algorithm, sample, false, algorithm.playbackEventIndex,
                         algorithm.playbackPulse, algorithm.activeSelection,
                         sample,
                         algorithm.playbackIntervalOrdinal + (advance ? 1U : 0U),
                         mpeOwnedCount(mpe) + setupSends + contextSends,
                         static_cast<uint64_t>(mpe.heapSize) *
                             kMpeHeapRemoveCost)) {
        return false;
    }
    mpe.admittedValid = true;
    mpe.admittedSample = sample;
    mpeReleaseOwned(algorithm, sample, false, 0xffffU);
    if (mpe.deliberate) {
        mpeSetupPlan(algorithm, context, true, sample, mpe.setupIncomplete);
        mpe.deliberate = false;
    }
    if (mpe.contextPending) {
        mpeContextPlan(algorithm, context, meta, true, sample,
                       mpe.contextIncomplete);
        mpe.contextPending = false;
    }
    return true;
}

// Bounded entry progress after Playback On: Finalizing, then Preparing
// (pairing and candidate context share the 512-visit step budget), then
// validation against the prepared historical scope. A blocked request stays
// visibly blocked and keeps its deliberate capability for a live correction.
void mpeAdvanceEntry(Algorithm& algorithm) {
    MpePlayback& mpe = mpeState(algorithm);
    if (!mpe.entryPending) {
        return;
    }
    HistoryControl& history = historyControl(algorithm);
    if (history.finalizing || !algorithm.selectionValid ||
        !history.pairingComplete ||
        history.pairedGeneration != history.generation) {
        return;
    }
    const HistoryContextMeta& candidate =
        history.contexts[kHistoryContextCandidate];
    if (!candidate.ready || candidate.generation != history.generation ||
        !sameHistoryRange(candidate.range, algorithm.selection)) {
        requestHistoryCandidate(algorithm, algorithm.selection);
        return;
    }
    MpeArenaReservation& arena = mpeArena(algorithm);
    uint8_t lower = 0U;
    uint8_t upper = 0U;
    mpe.entryPending = false;
    mpe.status = mpeResolveScope(algorithm,
                                 arena.contexts[kHistoryContextCandidate],
                                 lower, upper);
    if (mpe.status != kMpeStatusReady) {
        return;
    }
    copyHistoryState(arena.contexts[kHistoryContextActive],
                     arena.contexts[kHistoryContextCandidate]);
    history.contexts[kHistoryContextActive] = candidate;
    mpe.lower = lower;
    mpe.upper = upper;
    mpe.scopeMask = mpeScopeMask(lower, upper);
    mpe.engaged = true;
    mpe.restartRequired = false;
    mpe.contextPending = true;
    algorithm.activeSelection = algorithm.selection;
    algorithm.activeSelectionValid = true;
    algorithm.rangeTransitionPending = false;
    algorithm.playbackPulse = algorithm.selection.startPulse;
    algorithm.playbackEventIndex =
        mpeLowerBound(algorithm, algorithm.playbackPulse);
    algorithm.playbackPositionValid = true;
    algorithm.playbackIntervalOpen = false;
    algorithm.playbackNextEventScheduled = false;
    algorithm.transportState = kTransportArmed;
}

bool mpeStartPlayback(Algorithm& algorithm, const PulseRange& range) {
    MpePlayback& mpe = mpeState(algorithm);
    if (algorithm.transportState != kTransportStopped) {
        return true;
    }
    if (mpe.fault) {
        return false;
    }
    for (uint32_t parameter = 0; parameter < kNumParameters; ++parameter) {
        mpe.controls[parameter] = algorithm.v[parameter];
    }
    mpe.status = mpeControlStatus(algorithm);
    if (mpe.status != kMpeStatusReady) {
        // Visibly blocked before any preparation or output.
        return false;
    }
    beginFinalization(algorithm);
    requestHistoryCandidate(algorithm, range);
    mpe.entryPending = true;
    mpe.engaged = false;
    algorithm.playbackPositionValid = false;
    mpeAdvanceEntry(algorithm);
    return true;
}

uint8_t mpePhase(const Algorithm& algorithm) {
    const MpePlayback& mpe = mpeState(algorithm);
    if (mpe.fault) {
        return kMpePhaseFault;
    }
    if (mpe.entryPending) {
        return historyControl(algorithm).finalizing ? kMpePhaseFinalizing
                                                    : kMpePhasePreparing;
    }
    if (!mpe.engaged) {
        return kMpePhaseIdle;
    }
    switch (algorithm.transportState) {
    case kTransportArmed:
    case kTransportClockLossPaused:
        return kMpePhaseArmed;
    case kTransportPlaying:
        return kMpePhasePlaying;
    default:
        return kMpePhaseIdle;
    }
}

bool mpeRelevantParameter(int parameter) {
    return parameter == kParameterRecordingChannel ||
           parameter == kParameterPlaybackDestination ||
           parameter == kParameterPlaybackChannel ||
           parameter == kParameterFilterControlChange ||
           parameter == kParameterFilterPitchBend ||
           parameter == kParameterFilterAftertouch ||
           parameter == kParameterMpeMode ||
           parameter == kParameterMpeSetupSource ||
           parameter == kParameterLowerMembers ||
           parameter == kParameterUpperMembers;
}

// Live relevant changes during MPE play (or an MPE Mode change during
// ordinary play) clean the stored old routes, cancel ownership and stop
// intent. While stopped, a live correction may complete a blocked request.
void mpeControlChanged(_NT_algorithm* self, Algorithm& algorithm,
                       int parameter) {
    MpePlayback& mpe = mpeState(algorithm);
    if (algorithm.v == NULL || !mpeRelevantParameter(parameter)) {
        return;
    }
    const int16_t value = algorithm.v[parameter];
    const bool changed = value != mpe.controls[parameter];
    mpe.controls[parameter] = value;
    if (!changed || mpe.restoreWindow) {
        return;
    }
    const bool modeChange = parameter == kParameterMpeMode;
    if (algorithm.transportState != kTransportStopped &&
        (mpe.engaged || modeChange)) {
        stopTransport(algorithm);
        mpe.deliberate = false;
        if (algorithm.v[kParameterPlayback] != 0) {
            nt_host::setParameterFromAudio(self, kParameterPlayback, 0);
        }
        return;
    }
    if (mpe.entryPending) {
        mpe.status = mpeControlStatus(algorithm);
        if (mpe.status != kMpeStatusReady || !mpeModeOn(algorithm)) {
            mpe.entryPending = false;
        }
        return;
    }
    if (algorithm.transportState == kTransportStopped && !mpe.fault &&
        algorithm.v[kParameterPlayback] != 0 &&
        (modeChange || (mpeModeOn(algorithm) && mpe.status != kMpeStatusReady))) {
        startPlayback(self);
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
    if (mpeState(algorithm).engaged) {
        mpeDispatchPlaybackEvents(algorithm, sample, forceCurrentInterval);
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
    if (mpeState(algorithm).engaged) {
        mpeWrap(algorithm, sample);
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
    if (!mpeState(algorithm).engaged) {
        refreshPendingEndingSchedule(algorithm);
    }
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
    if (mpeState(algorithm).engaged &&
        !mpeActivate(algorithm, sample, !resumeSavedInterval)) {
        return;
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
        if (algorithm.transportState == kTransportPlaying) {
            beginPlaybackInterval(algorithm, sample);
        }
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
    MpePlayback& mpe = mpeState(algorithm);
    if (mpe.engaged) {
        // Re-entry at the range start resends known context, never setup.
        algorithm.playbackEventIndex =
            mpeLowerBound(algorithm, algorithm.playbackPulse);
        mpe.contextPending = true;
    } else {
        algorithm.playbackEventIndex =
            findPlaybackEventIndex(algorithm, algorithm.playbackPulse);
    }
    algorithm.playbackPositionValid = true;
    if (algorithm.transportState == kTransportPlaying) {
        algorithm.transportState = kTransportArmed;
    }
}

uint32_t historySetupFingerprint(const HistoryState& state,
                                 const HistoryContextMeta& meta) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    uint32_t hash = 2166136261U;
    for (uint32_t index = kHistorySetupBegin; index < kHistorySetupEnd;
         ++index) {
        hash = (hash ^ bytes[index]) * 16777619U;
    }
    hash = (hash ^ (meta.coverageComplete ? 1U : 0U)) * 16777619U;
    return (hash ^ (meta.setupChangesInside ? 1U : 0U)) * 16777619U;
}

bool sameHistoryRange(const PulseRange& left, const PulseRange& right) {
    return left.startPulse == right.startPulse &&
           left.endPulse == right.endPulse;
}

void requestHistoryCandidate(Algorithm& algorithm, const PulseRange& range) {
    HistoryControl& history = historyControl(algorithm);
    if (history.candidateRequested &&
        sameHistoryRange(history.work.range, range)) {
        return;
    }
    history.candidateRequested = true;
    history.contexts[kHistoryContextCandidate].ready = false;
    history.work.range = range;
    history.workPhase = kPreparationSearch;
}

void cancelHistoryCandidate(Algorithm& algorithm) {
    HistoryControl& history = historyControl(algorithm);
    history.candidateRequested = false;
    history.contexts[kHistoryContextCandidate].ready = false;
    history.workPhase = kPreparationIdle;
}

// Starts reconstruction at the newest checkpoint wholly before the selection
// (<= 9 probes), else at the eviction seed, and positions the journal cursor
// at its first entry not already folded into that state (<= 9 probes).
void beginHistoryReconstruction(Algorithm& algorithm, uint32_t& budget) {
    HistoryControl& history = historyControl(algorithm);
    MpeArenaReservation& arena = mpeArena(algorithm);
    HistoryState& work = arena.contexts[kHistoryContextWork];
    const uint64_t start = history.work.range.startPulse;
    const uint64_t total = nextHistoryOrdinal(algorithm);
    uint64_t low = history.baseOrdinal / kHistoryCheckpointInterval + 1U;
    uint64_t high = total / kHistoryCheckpointInterval + 1U;
    uint64_t found = 0;
    uint32_t comparisons = 0;
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2U;
        const HistoryCheckpoint& checkpoint =
            historyCheckpoint(algorithm, middle);
        ++comparisons;
        if (checkpoint.key.ordinal == middle * kHistoryCheckpointInterval &&
            checkpoint.key.pulse < start) {
            found = middle;
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    noteMaximum(history.maximumCheckpointSearchComparisons, comparisons);
    if (found != 0U) {
        copyHistoryState(work, historyCheckpoint(algorithm, found).state);
        history.workOrdinal = found * kHistoryCheckpointInterval;
        history.work.coverageComplete =
            history.workOrdinal >= history.coverageStartOrdinal;
    } else {
        copyHistoryState(work, history.seed);
        history.workOrdinal = history.baseOrdinal;
        // A seed that already folded the selection's own pulse cannot
        // represent the state before it.
        history.work.coverageComplete =
            history.seedValid &&
            history.baseOrdinal >= history.coverageStartOrdinal &&
            !(history.seedFolded && history.seedLastPulse >= start);
    }
    history.workJournalCursor =
        journalBound(algorithm, history.workOrdinal, false, comparisons);
    budget = budget > comparisons ? budget - comparisons : 0U;
    history.work.generation = history.generation;
    history.work.setupChangesInside = false;
    history.workForwardEvents = 0;
    history.workJournalVisits = 0;
    history.workPhase = kPreparationFold;
}

// Bounded, resumable candidate preparation over immutable history: at most
// 511 forward events plus the retained journal, never touching event links
// or the active context. Returns true when the candidate is ready.
bool prepareHistoryCandidate(Algorithm& algorithm, uint32_t& budget) {
    HistoryControl& history = historyControl(algorithm);
    if (!history.candidateRequested) {
        return false;
    }
    HistoryContextMeta& candidate = history.contexts[kHistoryContextCandidate];
    if (candidate.ready && candidate.generation == history.generation &&
        sameHistoryRange(candidate.range, history.work.range)) {
        return true;
    }
    if (history.workPhase == kPreparationIdle ||
        history.work.generation != history.generation) {
        history.workPhase = kPreparationSearch;
    }
    if (history.workPhase == kPreparationSearch) {
        if (budget < 18U) {
            return false;
        }
        beginHistoryReconstruction(algorithm, budget);
    }

    MpeArenaReservation& arena = mpeArena(algorithm);
    HistoryState& work = arena.contexts[kHistoryContextWork];
    const uint64_t start = history.work.range.startPulse;
    const uint64_t total = nextHistoryOrdinal(algorithm);
    while (history.workPhase == kPreparationFold) {
        if (budget == 0U) {
            return false;
        }
        const RecordedEvent* event =
            history.workOrdinal < total
                ? recordedEventByIndex(
                      algorithm, static_cast<uint32_t>(history.workOrdinal -
                                                       history.baseOrdinal))
                : NULL;
        if (history.workJournalCursor < history.journalCount) {
            const HistoryJournalEntry& entry =
                journalEntry(algorithm, history.workJournalCursor);
            // Entries received before the selection's first own event are
            // its context, including gap seeds stamped at that event's pulse.
            if (entry.key.ordinal <= history.workOrdinal) {
                if (entry.key.pulse >= start &&
                    (event == NULL ||
                     event->pulse >= history.work.range.endPulse)) {
                    history.workPhase = kPreparationInside;
                    break;
                }
                copyHistoryState(work, entry.state);
                ++history.workJournalCursor;
                ++history.workJournalVisits;
                --budget;
                continue;
            }
        }
        if (event == NULL || event->pulse >= start) {
            history.workPhase = kPreparationInside;
            break;
        }
        if (history.workForwardEvents == kHistoryMaximumForwardEvents) {
            history.work.coverageComplete = false;
            history.workPhase = kPreparationInside;
            break;
        }
        applyHistoryEvent(work, *event);
        ++history.workOrdinal;
        ++history.workForwardEvents;
        --budget;
    }

    // Setup changes journaled strictly inside the selection are flagged.
    while (history.workJournalCursor < history.journalCount) {
        const HistoryJournalEntry& entry =
            journalEntry(algorithm, history.workJournalCursor);
        if (entry.key.pulse >= history.work.range.endPulse) {
            break;
        }
        if (budget == 0U) {
            return false;
        }
        if (entry.key.pulse >= start &&
            !sameHistoryBytes(entry.state, work, kHistorySetupBegin,
                              kHistorySetupEnd)) {
            history.work.setupChangesInside = true;
        }
        ++history.workJournalCursor;
        ++history.workJournalVisits;
        --budget;
    }

    noteMaximum(history.maximumForwardEventVisits, history.workForwardEvents);
    noteMaximum(history.maximumJournalVisits, history.workJournalVisits);
    if (!history.work.coverageComplete) {
        // Missing coverage is unknown, never certified by a checkpoint alone.
        zeroHistoryState(work);
    }
    history.work.setupFingerprint = historySetupFingerprint(work, history.work);
    history.work.ready = true;
    copyHistoryState(arena.contexts[kHistoryContextCandidate], work);
    candidate = history.work;
    history.workPhase = kPreparationIdle;
    return true;
}

// Playback entry adopts the selected range's prepared context as the active,
// immutable copy. Preparation is bounded, so completing it here is fixed work.
void adoptActiveHistoryContext(Algorithm& algorithm, const PulseRange& range) {
    HistoryControl& history = historyControl(algorithm);
    requestHistoryCandidate(algorithm, range);
    uint32_t unlimited = kHistoryNoIndex;
    prepareHistoryCandidate(algorithm, unlimited);
    MpeArenaReservation& arena = mpeArena(algorithm);
    copyHistoryState(arena.contexts[kHistoryContextActive],
                     arena.contexts[kHistoryContextCandidate]);
    history.contexts[kHistoryContextActive] =
        history.contexts[kHistoryContextCandidate];
}

// Once per host step: finalize at most 128 generated appends, then spend at
// most 512 record visits on frozen-history pairing and candidate preparation.
void advanceHistoryWork(Algorithm& algorithm) {
    HistoryControl& history = historyControl(algorithm);
    history.lastStepFinalizeAppends = 0;
    history.lastStepPreparationVisits = 0;
    if (history.finalizing) {
        uint32_t appended = 0;
        finalizeCaptureStep(algorithm, kFinalizeAppendsPerStep, appended);
        history.lastStepFinalizeAppends = appended;
        noteMaximum(history.maximumStepFinalizeAppends, appended);
        if (history.finalizing) {
            return;
        }
    }
    uint32_t budget = kPreparationVisitsPerStep;
    // Pairing reuses note/pedal work storage, so it runs only while that
    // storage is free and the history is frozen.
    if (!algorithm.captureEnabled && algorithm.selectionValid &&
        algorithm.transportState == kTransportStopped &&
        algorithm.pendingEndings.noteCount == 0U &&
        algorithm.pendingEndings.sustainCount == 0U) {
        pairRecordedEndingsStep(algorithm, budget);
    }
    if (budget != 0U) {
        prepareHistoryCandidate(algorithm, budget);
    }
    history.lastStepPreparationVisits = kPreparationVisitsPerStep - budget;
    noteMaximum(history.maximumStepPreparationVisits,
                history.lastStepPreparationVisits);
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || algorithm->v == NULL || busFrames == NULL ||
        numFramesBy4 <= 0 || numFramesBy4 > 0x1fffffff) {
        return;
    }

    const int numFrames = numFramesBy4 * 4;
    algorithm->processingStarted = true;
    // MPE normal budgets reset once per host step, across all its frames.
    MpePlayback& mpe = mpeState(*algorithm);
    mpe.step = MpeWorkCounters();
    mpe.admittedValid = false;
    mpe.restoreWindow = false;
    advanceHistoryWork(*algorithm);
    mpeAdvanceEntry(*algorithm);
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
    mpe.lastStep = mpe.step;
}

void setPlaybackRequest(_NT_algorithm* self, bool requested,
                        bool fromUi) {
    if (self == NULL) {
        return;
    }
    const int16_t value = requested ? 1 : 0;
    if (fromUi) {
        nt_host::setParameterFromUi(self, kParameterPlayback, value);
    } else {
        nt_host::setParameterFromAudio(self, kParameterPlayback, value);
    }
}

void emergencySilence(_NT_algorithm* self, Algorithm& algorithm,
                      bool fromUi) {
    // Publish Off through the host first. Its parameter callback performs the
    // same ordered note/sustain cleanup as every explicit shared-state stop.
    // The fallback keeps the legacy low-level transport seam safe when its
    // host value was already Off.
    if (algorithm.v != NULL && algorithm.v[kParameterPlayback] != 0) {
        setPlaybackRequest(self, false, fromUi);
    } else {
        stopTransport(algorithm);
    }

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
    mpeForgetOwners(algorithm);
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

SetupObserver& setupObserver(Algorithm& algorithm) {
    return reinterpret_cast<MpeArenaReservation*>(
               reinterpret_cast<uint8_t*>(algorithm.recordingEvents) +
               algorithm.recordingBufferBytes)
        ->setup;
}

bool zoneActive(const SetupZone& zone) {
    return zone.countState == kSetupKnown && zone.count > 0U;
}

uint8_t setupRole(const SetupObserver& setup, uint8_t channel) {
    const SetupZone& lower = setup.zones[0];
    const SetupZone& upper = setup.zones[1];
    if (zoneActive(lower) && channel <= lower.count) {
        return channel == 0U ? kSetupRoleLowerManager : kSetupRoleLowerMember;
    }
    if (zoneActive(upper) && channel >= 15U - upper.count) {
        return channel == 15U ? kSetupRoleUpperManager : kSetupRoleUpperMember;
    }
    if (lower.countState != kSetupKnown || upper.countState != kSetupKnown) {
        return kSetupRoleUnknown;
    }
    return kSetupRoleNone;
}

// Channels whose zone membership changed (including to unknown) advance their
// generation so downstream expression context is invalidated, never neutralized.
void refreshSetupRoles(SetupObserver& setup) {
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        const uint8_t role = setupRole(setup, channel);
        if (role != setup.roles[channel]) {
            setup.roles[channel] = role;
            ++setup.membershipGeneration[channel];
        }
    }
}

// A received MCM resets receiver ranges, so role-free receipts are forgotten.
void forgetChannelSensitivities(SetupObserver& setup) {
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        setup.channelSensitivity[channel] = {0U, kSetupUnknown};
    }
}

void applyManagerConfiguration(SetupObserver& setup, unsigned zoneIndex,
                               uint8_t count) {
    forgetChannelSensitivities(setup);
    SetupZone& zone = setup.zones[zoneIndex];
    SetupZone& opposite = setup.zones[1U - zoneIndex];
    zone.count = count;
    zone.countState = kSetupKnown;
    zone.manager = {200U, kSetupKnown};
    zone.member = {4800U, kSetupKnown};
    if (count >= 14U) {
        opposite.count = 0;
        opposite.countState = kSetupKnown;
    } else if (zoneActive(opposite) && count + opposite.count > 14U) {
        opposite.count = static_cast<uint8_t>(14U - count);
    }
    refreshSetupRoles(setup);
}

// The receiver's result of a relative or out-of-range MCM cannot be known.
// An active opposite zone could have been shrunk or disabled by it.
void invalidateManagerConfiguration(SetupObserver& setup, unsigned zoneIndex) {
    forgetChannelSensitivities(setup);
    SetupZone& zone = setup.zones[zoneIndex];
    SetupZone& opposite = setup.zones[1U - zoneIndex];
    zone.countState = kSetupIndeterminate;
    zone.manager.state = kSetupIndeterminate;
    zone.member.state = kSetupIndeterminate;
    if (zoneActive(opposite)) {
        opposite.countState = kSetupIndeterminate;
    }
    refreshSetupRoles(setup);
}

void writeSensitivity(SetupObserver& setup, uint8_t channel, uint16_t cents,
                      uint8_t state) {
    SetupValue value = {state == kSetupKnown ? cents : uint16_t(0), state};
    setup.channelSensitivity[channel] = value;
    switch (setup.roles[channel]) {
    case kSetupRoleLowerManager: setup.zones[0].manager = value; break;
    case kSetupRoleLowerMember: setup.zones[0].member = value; break;
    case kSetupRoleUpperManager: setup.zones[1].manager = value; break;
    case kSetupRoleUpperMember: setup.zones[1].member = value; break;
    default: break;
    }
}

// Observes only absolute MCM (RPN 0/6) and Pitch Bend Sensitivity (RPN 0/0)
// before capture/clock gating, under the recording-channel admission rule.
void observeSetup(Algorithm& algorithm, uint8_t status, uint8_t controller,
                  uint8_t value) {
    if ((status & 0xf0U) != 0xb0U || controller > 127U || value > 127U) return;
    const uint8_t channel = status & 15U;
    int admitted = algorithm.v == NULL ? 0 : algorithm.v[kParameterRecordingChannel];
    if (admitted < 0 || admitted > 16) admitted = 0;
    if (admitted != 0 && admitted != channel + 1) return;
    SetupObserver& setup = setupObserver(algorithm);
    SetupSelector& selector = setup.selectors[channel];
    uint8_t family = 0;
    if (controller == 101U || controller == 100U) family = 1;
    if (controller == 99U || controller == 98U) family = 2;
    if (family != 0) {
        const uint8_t mask = (controller == 101U || controller == 99U) ? 1U : 2U;
        if (selector.family != family) {
            selector.family = family;
            selector.validMask = 0;
            selector.dataValid = false;
            ++selector.generation;
        }
        uint8_t& part = mask == 1U ? selector.msb : selector.lsb;
        if ((selector.validMask & mask) == 0 || part != value) {
            selector.dataValid = false;
            ++selector.generation;
        }
        part = value;
        selector.validMask |= mask;
        return;
    }
    if (controller != 6U && controller != 38U && controller != 96U &&
        controller != 97U) {
        return;
    }
    const bool rpnSelected = selector.family == 1U && selector.validMask == 3U &&
                             selector.msb == 0U;
    const bool sensitivity = rpnSelected && selector.lsb == 0U;
    const bool manager = channel == 0U || channel == 15U;
    const bool configuration = rpnSelected && selector.lsb == 6U && manager;
    const unsigned zoneIndex = channel == 0U ? 0U : 1U;
    if (controller == 6U) {
        selector.dataValid = rpnSelected;
        selector.dataMsb = value;
        if (sensitivity) {
            writeSensitivity(setup, channel, static_cast<uint16_t>(value * 100U),
                             value <= 96U ? kSetupKnown : kSetupUnsupported);
        } else if (configuration) {
            if (value <= 15U) {
                applyManagerConfiguration(setup, zoneIndex, value);
            } else {
                invalidateManagerConfiguration(setup, zoneIndex);
            }
        }
    } else if (controller == 38U) {
        if (!sensitivity) return;  // CC38 carries no MCM meaning.
        if (!selector.dataValid) {
            // LSB before MSB combines with receiver data we must not reuse.
            writeSensitivity(setup, channel, 0, kSetupIndeterminate);
            return;
        }
        const uint16_t cents = static_cast<uint16_t>(selector.dataMsb * 100U + value);
        const bool supported = selector.dataMsb <= 96U && value <= 99U && cents <= 9600U;
        writeSensitivity(setup, channel, cents,
                         supported ? kSetupKnown : kSetupUnsupported);
    } else {
        selector.dataValid = false;
        if (sensitivity) {
            writeSensitivity(setup, channel, 0, kSetupIndeterminate);
        } else if (configuration) {
            invalidateManagerConfiguration(setup, zoneIndex);
        }
    }
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

    observeSetup(*algorithm, byte0, byte1, byte2);

    uint64_t offset = algorithm->sampleCursor - algorithm->lastPulseSample;
    if (offset > 0xffffffffULL) {
        offset = 0xffffffffULL;
    }
    // The MIDI callback carries no arrival timestamp. A callback at the first
    // sample boundary after a scanned pulse is the finest observable
    // pulse-aligned case; normalize that one-sample ambiguity to phase zero.
    RecordedEvent event = {
        algorithm->currentPulse,
        offset <= 1U ? 0U : static_cast<uint32_t>(offset),
        boundedInterval(algorithm->lastClockIntervalSamples),
        {byte0, byte1, byte2},
        static_cast<uint8_t>((byte0 & 0xf0U) == 0xd0U ? 2U : 3U),
        0,
    };
    const int recordingChannel = recordingAdmission(*algorithm);
    const bool admitted =
        recordingChannel == 0 || static_cast<int>(byte0 & 0x0fU) + 1 == recordingChannel;
    const bool recorded = admitted && algorithm->captureEnabled &&
                          algorithm->clockRunning &&
                          eligiblePerformanceEvent(event);
    if (admitted) {
        observeLiveHistory(*algorithm, event, recorded);
    }

    const bool emergencyController =
        (byte0 & 0xf0U) == 0xb0U && (byte1 == 120U || byte1 == 123U);
    if (emergencyController) {
        const bool playbackOn =
            algorithm->v != NULL && algorithm->v[kParameterPlayback] != 0;
        if (algorithm->transportState != kTransportStopped) {
            emergencySilence(self, *algorithm, false);
        } else if (playbackOn) {
            // Preserve the existing no-output behavior when no transport ever
            // started, while still clearing the continuing host request.
            setPlaybackRequest(self, false, false);
        }
        return;
    }

    if (!recorded) {
        return;
    }
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
                          bool startBoundary, float boundaryPotPhysical,
                          float rightPotPhysical) {
    if (!ensureTimelineSelection(self, algorithm)) {
        return;
    }

    uint64_t retainedStart = 0;
    uint64_t retainedEnd = 0;
    if (!retainedTimelineBounds(algorithm, retainedStart, retainedEnd)) {
        return;
    }

    uint64_t viewStart = 0;
    uint64_t viewEnd = 0;
    timelineViewBounds(algorithm, viewStart, viewEnd);
    uint64_t start = algorithm.selection.startPulse;
    uint64_t end = algorithm.selection.endPulse;
    BoundaryMotionState& motion = startBoundary
                                      ? algorithm.startBoundaryMotion
                                      : algorithm.endBoundaryMotion;
    uint64_t& boundary = startBoundary ? start : end;
    const uint64_t legalMinimum = startBoundary ? retainedStart : start + 1U;
    const uint64_t legalMaximum = startBoundary ? end - 1U : retainedEnd;
    if (!moveBoundaryMotion(motion, legalMinimum, legalMaximum,
                            viewStart, viewEnd, boundary,
                            boundaryPotPhysical) ||
        (start == algorithm.selection.startPulse &&
         end == algorithm.selection.endPulse)) {
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
    if (startBoundary && requested >= algorithm.selection.endPulse) {
        requested = algorithm.selection.endPulse - 1U;
    } else if (!startBoundary &&
               requested <= algorithm.selection.startPulse) {
        requested = algorithm.selection.startPulse + 1U;
    }
    const uint64_t current = startBoundary
                                 ? algorithm.selection.startPulse
                                 : algorithm.selection.endPulse;
    if (requested == current) {
        return;
    }
    if (startBoundary) {
        algorithm.startBoundaryMotion.pulseResidual = 0.0;
    } else {
        algorithm.endBoundaryMotion.pulseResidual = 0.0;
    }
    if (setPulseSelection(self, startBoundary ? requested
                                              : algorithm.selection.startPulse,
                          startBoundary ? algorithm.selection.endPulse
                                        : requested)) {
        algorithm.selectionFineTarget = startBoundary
                                            ? kSelectionFineTargetStart
                                            : kSelectionFineTargetEnd;
        rebaseRangeMotionAfterEdit(algorithm, rightPotPhysical);
    }
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

    // There is no smaller valid viewport for a retained span at or below the
    // manual minimum. Keep the existing mode and bounds rather than creating a
    // nominal manual width that cannot change what is visible.
    if (retained <= kTimelineMinimumVisiblePulses) {
        return;
    }

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
        emergencySilence(&algorithm, algorithm, true);
        algorithm.rightEncoderPanicFired = true;
    }
}

void toggleTimelinePlayback(_NT_algorithm* self, Algorithm& algorithm) {
    if (algorithm.v == NULL) {
        return;
    }
    setPlaybackRequest(self, algorithm.v[kParameterPlayback] == 0, true);
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
    bool suppressBoundaryTranslation = false;
    if (algorithm->boundaryMotionNeedsPhysicalSeed) {
        seedBoundaryMotionPhysical(algorithm->startBoundaryMotion,
                                   data.pots[0]);
        seedBoundaryMotionPhysical(algorithm->endBoundaryMotion,
                                   data.pots[1]);
        algorithm->boundaryMotionNeedsPhysicalSeed = false;
        suppressBoundaryTranslation = true;
    }

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

    if (!suppressBoundaryTranslation &&
        (data.controls & kNT_potL) != 0U) {
        moveTimelineBoundary(self, *algorithm, true, data.pots[0],
                             data.pots[2]);
    }
    if (!suppressBoundaryTranslation &&
        (data.controls & kNT_potC) != 0U) {
        moveTimelineBoundary(self, *algorithm, false, data.pots[1],
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
    algorithm->boundaryMotionNeedsPhysicalSeed = true;
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
        // Match the approved retrieval prototype with a handle that points
        // toward the actual boundary. Start owns the upper handle and End the
        // lower one, so both remain identifiable when they share an edge.
        const bool older = pulse < viewStart;
        const int edgeX = older ? 4 : 251;
        const int innerX = older ? 8 : 247;
        const int top = start ? 16 : 48;
        const int middle = top + 4;
        const int bottom = top + 8;
        nt_host::drawShape(kNT_line, innerX, top, edgeX, middle, 15);
        nt_host::drawShape(kNT_line, edgeX, middle, innerX, bottom, 15);
        nt_host::drawShape(kNT_line, innerX, top, innerX, bottom, 15);
        return;
    }
    const int x = pulseTimelineX(pulse, viewStart, viewEnd);
    nt_host::drawShape(kNT_line, x, 16, x, 56, 15);
    nt_host::drawShape(kNT_line, x, 16, x + (start ? 4 : -4), 16, 15);
    nt_host::drawShape(kNT_line, x, 56, x + (start ? 4 : -4), 56, 15);
}

// MPE lifecycle/blocking status. Blocking and overload reasons take priority
// over warnings, which never stop playback.
void drawMpeStatus(const Algorithm& algorithm) {
    const MpePlayback& mpe = mpeState(algorithm);
    if (!mpeModeOn(algorithm) && !mpe.fault) {
        return;
    }
    static const char* const kPhases[] = {
        "Idle", "Finalizing", "Preparing", "Armed", "Playing", "Fault"};
    static const char* const kBlocked[] = {
        "",
        "Blocked: Record Ch not Omni",
        "Blocked: Play Ch not Original",
        "Blocked: filters On",
        "Blocked: invalid zones",
        "Blocked: zones unknown",
        "Blocked: zones mismatch",
        "Overload: Off/On to restart"};
    const HistoryContextMeta& active =
        historyControl(algorithm).contexts[kHistoryContextActive];
    char text[48];
    BoundedText status(text, sizeof(text));
    status.append("MPE ");
    if (mpe.status != kMpeStatusReady && mpe.status < ARRAY_SIZE(kBlocked)) {
        status.append(kBlocked[mpe.status]);
    } else {
        status.append(kPhases[mpePhase(algorithm)]);
        if (mpe.restartRequired) {
            status.append(" - Restart to apply setup");
        } else if (mpe.engaged && active.setupChangesInside) {
            status.append(" - setup change unsupported");
        } else if (mpe.engaged && mpe.contextIncomplete) {
            status.append(" - context incomplete");
        } else if (mpe.engaged && mpe.setupIncomplete) {
            status.append(" - setup incomplete");
        }
    }
    nt_host::drawTinyText(0, 63, text);
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
    availabilityText.append(" PPQN");
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
        // All visible attacks contribute to the same vertical-stem image as
        // individual drawing. Overlapping stems share a column, so keeping
        // its highest pitch produces their exact union with bounded drawing.
        uint8_t columnTop[248];
        for (uint32_t column = 0; column < ARRAY_SIZE(columnTop); ++column) {
            columnTop[column] = 52U;
        }
        uint32_t eventIndex =
            findPlaybackEventIndex(*algorithm, observed.viewStart);
        while (eventIndex < algorithm->eventCount) {
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
            if (y >= 0 && y < columnTop[x - 4]) {
                columnTop[x - 4] = static_cast<uint8_t>(y);
            }
        }
        for (uint32_t column = 0; column < ARRAY_SIZE(columnTop); ++column) {
            if (columnTop[column] < 52U) {
                const int x = static_cast<int>(column) + 4;
                nt_host::drawShape(kNT_line, x, columnTop[column], x, 51, 9);
            }
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
    drawMpeStatus(*algorithm);
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

    // The host remains authoritative and persists these as ordinary generic
    // parameters. This copy exists only to reconcile either host callback
    // order and to distinguish new images from ten-parameter legacy images.
    stream.addMemberName("sharedControls");
    stream.openArray();
    stream.addNumber(1);  // shared-control reconciliation revision
    stream.addBoolean(algorithm->v != NULL &&
                      algorithm->v[kParameterPlayback] != 0);
    stream.addBoolean(algorithm->v != NULL &&
                      algorithm->v[kParameterClearRecording] != 0);
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

struct SharedControlPresetState {
    SharedControlPresetState() : playback(false), clearRecording(false) {}

    bool playback;
    bool clearRecording;
};

bool parseSharedControls(_NT_jsonParse& parse,
                         SharedControlPresetState& state) {
    int count = 0;
    int representation = 0;
    return parse.numberOfArrayElements(count) && count == 3 &&
           parse.number(representation) && representation == 1 &&
           parse.boolean(state.playback) &&
           parse.boolean(state.clearRecording);
}

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
    bool sharedControlsSeen = false;
    SharedControlPresetState sharedControls;
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
        } else if (parse.matchName("sharedControls")) {
            if (!parseSharedControls(parse, sharedControls)) {
                return false;
            }
            sharedControlsSeen = true;
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

    if (sharedControlsSeen) {
        algorithm.restoredPlaybackValue = sharedControls.playback ? 1 : 0;
        algorithm.restoredClearRecordingValue =
            sharedControls.clearRecording ? 1 : 0;
        const bool genericParametersAlreadyRestored =
            algorithm.v != NULL &&
            ((algorithm.restoredPlaybackValue !=
                  kParameters[kParameterPlayback].def &&
              algorithm.v[kParameterPlayback] ==
                  algorithm.restoredPlaybackValue) ||
             (algorithm.restoredClearRecordingValue !=
                  kParameters[kParameterClearRecording].def &&
              algorithm.v[kParameterClearRecording] ==
                  algorithm.restoredClearRecordingValue));
        algorithm.playbackRestorePending =
            !genericParametersAlreadyRestored && algorithm.v != NULL &&
            algorithm.v[kParameterPlayback] !=
                algorithm.restoredPlaybackValue;
        algorithm.clearRecordingRestorePending =
            !genericParametersAlreadyRestored && algorithm.v != NULL &&
            algorithm.v[kParameterClearRecording] !=
                algorithm.restoredClearRecordingValue;
        // Both On and Off have already been interpreted in the saved image.
        // On therefore restores consumed; Off restores armed.
        algorithm.clearRecordingArmed = !sharedControls.clearRecording;
    } else {
        // Valid legacy images had ten generic parameters. Their complete
        // custom transport intent is the migration source for Playback, and
        // Clear Recording did not exist.
        algorithm.restoredPlaybackValue =
            algorithm.transportState == kTransportStopped ? 0 : 1;
        algorithm.restoredClearRecordingValue = 0;
        algorithm.playbackRestorePending = false;
        algorithm.clearRecordingRestorePending = false;
        algorithm.clearRecordingArmed = true;
    }
    return true;
}

bool deserialiseState(_NT_algorithm* self, _NT_jsonParse& parse) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
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

    // Clear diagnostics are deliberately not preset state. Resetting them also
    // makes a generic-parameters-before-custom-state Clear On callback
    // observationally disappear after the complete image is reconstructed.
    algorithm->clearRecordingTransitions = 0;
    algorithm->clearHistoryMetadataOperations = 0;

    const bool playbackNeedsImmediatePublish =
        algorithm->v != NULL &&
        algorithm->v[kParameterPlayback] !=
            algorithm->restoredPlaybackValue &&
        !algorithm->playbackRestorePending;
    if (playbackNeedsImmediatePublish) {
        // This covers legacy migration and the generic-before order where a
        // restored Clear On briefly stopped Playback before custom state was
        // available. API v13's UI setter is valid from any callback, keeping
        // host mappings, UI, and future generic saves authoritative without
        // directly writing host-owned v[].
        algorithm->playbackRestorePending = true;
        nt_host::setParameterFromUi(self, kParameterPlayback,
                                    algorithm->restoredPlaybackValue);
        if (algorithm->v[kParameterPlayback] !=
            algorithm->restoredPlaybackValue) {
            return false;
        }
    }

    return !algorithm->captureEnabled ||
           algorithm->transportState == kTransportStopped;
}

// Restored events carry no retained observation coverage until the MPE
// extension is loaded, so a non-empty load is honestly unknown history.
bool deserialise(_NT_algorithm* self, _NT_jsonParse& parse) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return false;
    }
    const uint64_t retiredOrdinal = nextHistoryOrdinal(*algorithm);
    // Base-preset restoration never carries deliberate-entry origin; loaded
    // state is continuation-only and its restore callbacks are action-free.
    MpePlayback& mpe = mpeState(*algorithm);
    mpeForgetOwners(*algorithm);
    mpe.deliberate = false;
    mpe.engaged = false;
    mpe.entryPending = false;
    mpe.contextPending = false;
    mpe.fault = false;
    mpe.restartRequired = false;
    mpe.status = kMpeStatusReady;
    mpe.restoreWindow = true;
    const bool restored = deserialiseState(self, parse);
    restartHistory(*algorithm, retiredOrdinal, algorithm->eventCount == 0U);
    return restored;
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('T', 'h', 'M', 'b'),
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

#if defined(MIDIBUFFER_NATIVE_TEST)
bool historySnapshot(const _NT_algorithm* self, HistorySnapshot& snapshot) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return false;
    }
    const HistoryControl& history = historyControl(*algorithm);
    snapshot.baseOrdinal = history.baseOrdinal;
    snapshot.nextOrdinal = nextHistoryOrdinal(*algorithm);
    snapshot.generation = history.generation;
    snapshot.pairedGeneration = history.pairedGeneration;
    snapshot.coverageStartOrdinal = history.coverageStartOrdinal;
    snapshot.seedLastPulse = history.seedLastPulse;
    snapshot.checkpointCount = history.checkpointCount;
    snapshot.journalCount = history.journalCount;
    snapshot.journalEvictions = history.journalEvictions;
    snapshot.lastStepFinalizeAppends = history.lastStepFinalizeAppends;
    snapshot.maximumStepFinalizeAppends = history.maximumStepFinalizeAppends;
    snapshot.lastStepPreparationVisits = history.lastStepPreparationVisits;
    snapshot.maximumStepPreparationVisits =
        history.maximumStepPreparationVisits;
    snapshot.maximumSeedSearchComparisons =
        history.maximumSeedSearchComparisons;
    snapshot.maximumCheckpointSearchComparisons =
        history.maximumCheckpointSearchComparisons;
    snapshot.maximumForwardEventVisits = history.maximumForwardEventVisits;
    snapshot.maximumJournalVisits = history.maximumJournalVisits;
    snapshot.seedValid = history.seedValid;
    snapshot.seedFolded = history.seedFolded;
    snapshot.finalizing = history.finalizing;
    snapshot.pairingComplete = history.pairingComplete &&
                               history.pairedGeneration == history.generation;
    copyHistoryState(snapshot.live, history.live);
    copyHistoryState(snapshot.recorded, history.recorded);
    copyHistoryState(snapshot.seed, history.seed);
    return true;
}

bool setupObserverSnapshot(const _NT_algorithm* self, SetupObserver& snapshot) {
    if (self == NULL) return false;
    const Algorithm* algorithm = static_cast<const Algorithm*>(self);
    snapshot = reinterpret_cast<const MpeArenaReservation*>(
        reinterpret_cast<const uint8_t*>(algorithm->recordingEvents) +
        algorithm->recordingBufferBytes)->setup;
    return true;
}
#endif

bool mpePlaybackSnapshot(const _NT_algorithm* self,
                         MpePlaybackSnapshot& snapshot) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return false;
    }
    const MpePlayback& mpe = mpeState(*algorithm);
    snapshot = MpePlaybackSnapshot();
    snapshot.lastStep = mpe.lastStep;
    snapshot.maximumStep = mpe.maximum;
    snapshot.ownedNotes = mpe.ownedNoteCount;
    snapshot.ownedHolds = mpeOwnedCount(mpe) - mpe.ownedNoteCount;
    snapshot.pendingEndings = mpe.heapSize;
    snapshot.rejectedClusters = mpe.rejectedClusters;
    snapshot.generatedSetup = mpe.generatedSetup;
    snapshot.generatedContext = mpe.generatedContext;
    snapshot.generatedCleanup = mpe.generatedCleanup;
    snapshot.scopeMask = mpe.scopeMask;
    snapshot.lowerMembers = mpe.lower;
    snapshot.upperMembers = mpe.upper;
    snapshot.phase = mpePhase(*algorithm);
    snapshot.status = mpe.status;
    snapshot.engaged = mpe.engaged;
    snapshot.deliberate = mpe.deliberate;
    snapshot.restartRequired = mpe.restartRequired;
    snapshot.setupChangesInside =
        historyControl(*algorithm).contexts[kHistoryContextActive]
            .setupChangesInside;
    snapshot.setupIncomplete = mpe.setupIncomplete;
    snapshot.contextIncomplete = mpe.contextIncomplete;
    return true;
}

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
    snapshot.clearRecordingTransitions =
        algorithm->clearRecordingTransitions;
    snapshot.clearHistoryMetadataOperations =
        algorithm->clearHistoryMetadataOperations;
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
    snapshot.clearRecordingArmed = algorithm->clearRecordingArmed;
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
    const uint64_t retiredOrdinal = nextHistoryOrdinal(*algorithm);
    algorithm->eventHead = 0U;
    algorithm->eventCount = endPulse > startPulse ? 1U : 0U;
    restartHistory(*algorithm, retiredOrdinal, false);
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
    ++historyControl(*algorithm).generation;
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

bool historyContext(const _NT_algorithm* self, bool candidate,
                    HistoryContextSnapshot& context) {
    const Algorithm* algorithm = asAlgorithm(self);
    if (algorithm == NULL) {
        return false;
    }
    const HistoryContextSlot slot =
        candidate ? kHistoryContextCandidate : kHistoryContextActive;
    const HistoryControl& history = historyControl(*algorithm);
    const HistoryContextMeta& meta = history.contexts[slot];
    context.range = meta.range;
    context.generation = meta.generation;
    context.setupFingerprint = meta.setupFingerprint;
    // A candidate is stale once history changes; the adopted active context
    // remains its immutable copy.
    context.ready = meta.ready && (!candidate ||
                                   meta.generation == history.generation);
    context.coverageComplete = meta.coverageComplete;
    context.setupChangesInside = meta.setupChangesInside;
    copyHistoryState(context.state, mpeArena(*algorithm).contexts[slot]);
    return context.ready;
}

bool beginCaptureFinalization(_NT_algorithm* self) {
    Algorithm* algorithm = asAlgorithm(self);
    return algorithm != NULL && beginFinalization(*algorithm);
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
    requestHistoryCandidate(*algorithm, algorithm->selection);
    if (algorithm->transportState != kTransportStopped &&
        algorithm->activeSelectionValid) {
        algorithm->rangeTransitionPending =
            !sameRange(algorithm->selection, algorithm->activeSelection);
    } else {
        algorithm->rangeTransitionPending = false;
        algorithm->playbackPositionValid = false;
    }
    if (algorithm->transportState == kTransportStopped &&
        algorithm->v != NULL && algorithm->v[kParameterPlayback] != 0) {
        startPlayback(self);
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
    PulseRange range = {};
    if (!acquirePlaybackSelection(self, range)) {
        return false;
    }
    if (mpeModeOn(*algorithm)) {
        return mpeStartPlayback(*algorithm, range);
    }

    // A valid continuing request is a capture-stop path: freeze a finite
    // performance without transmitting live cleanup. An unavailable request
    // remains On but leaves capture untouched until a valid range exists.
    finalizeCapture(*algorithm);
    if (algorithm->transportState != kTransportStopped) {
        return true;
    }

    prepareRecordedEndingOwnership(*algorithm);
    adoptActiveHistoryContext(*algorithm, range);
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
    if (algorithm == NULL) {
        return;
    }
    if (algorithm->v != NULL && algorithm->v[kParameterPlayback] != 0) {
        nt_host::setParameterFromAudio(self, kParameterPlayback, 0);
    } else {
        // Keep the low-level seam safe for an already-Off or unattached host.
        stopTransport(*algorithm);
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

void setParameterFromAudio(_NT_algorithm* algorithm, uint32_t parameter,
                           int16_t value) {
    const int32_t algorithmIndex = NT_algorithmIndex(algorithm);
    if (algorithmIndex >= 0) {
        NT_setParameterFromAudio(
            static_cast<uint32_t>(algorithmIndex),
            parameter + NT_parameterOffset(), value);
    }
}

void setParameterFromUi(_NT_algorithm* algorithm, uint32_t parameter,
                        int16_t value) {
    const int32_t algorithmIndex = NT_algorithmIndex(algorithm);
    if (algorithmIndex >= 0) {
        NT_setParameterFromUi(
            static_cast<uint32_t>(algorithmIndex),
            parameter + NT_parameterOffset(), value);
    }
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
