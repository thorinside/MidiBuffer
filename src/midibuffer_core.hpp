#ifndef MIDIBUFFER_CORE_HPP
#define MIDIBUFFER_CORE_HPP

#include <distingnt/api.h>

#include <stddef.h>
#include <stdint.h>

namespace midibuffer {

// The recording allocation contains only this fixed-width event representation.
// Ring and transport metadata live in the separately requested SRAM block.
struct RecordedEvent {
    uint64_t pulse;
    uint32_t offsetSamples;
    uint32_t sourceIntervalSamples;
    uint8_t bytes[3];
    uint8_t size;
    // Bit zero marks a capture ending; remaining bits cache its owner pairing.
    uint32_t flags;
};

struct PulseRange {
    uint64_t startPulse;
    uint64_t endPulse;
};

struct MusicalDuration {
    uint64_t bars;
    uint32_t beats;
    uint32_t ticks;
};

// Exact display conversion for supported Pulses/Beat values. Formatting is
// bounded (capacity includes the NUL) and replaces amounts above the approved
// bar limit with the complete lower-bound marker rather than truncating them.
bool musicalDurationFromPulses(uint64_t pulseIntervals,
                               uint32_t pulsesPerBeat,
                               MusicalDuration& duration);
bool pulsesFromMusicalDuration(const MusicalDuration& duration,
                               uint32_t pulsesPerBeat,
                               uint64_t& pulseIntervals);
bool formatMusicalDuration(char* output, size_t capacity,
                           uint64_t pulseIntervals,
                           uint32_t pulsesPerBeat);

enum SelectionFineTarget {
    kSelectionFineTargetStart,
    kSelectionFineTargetEnd,
    kSelectionFineTargetRange,
};

struct CaptureSnapshot {
    uint32_t recordingAllocationBytes;
    uint32_t metadataBytes;
    uint32_t eventCapacity;
    uint32_t eventCount;
    uint32_t clearRecordingTransitions;
    uint32_t clearHistoryMetadataOperations;
    uint64_t sampleCursor;
    uint64_t currentPulse;
    uint64_t oldestPulse;
    uint64_t newestPulse;
    uint64_t lastClockIntervalSamples;
    uint64_t predictedClockIntervalSamples;
    uint64_t playbackPulse;
    uint64_t playbackIntervalStartSample;
    uint64_t playbackIntervalOrdinal;
    uint64_t playbackNextEventSample;
    uint64_t pendingNextEndingSample;
    uint32_t playbackEventIndex;
    uint32_t clockAverageIntervalCount;
    uint32_t pendingNoteEndingCount;
    uint32_t pendingSustainReleaseCount;
    uint32_t pulsesPerDisplayedBeat;
    uint64_t timelineVisiblePulses;
    uint64_t timelineScrollPulses;
    uint64_t retainedPulseIntervals;
    uint64_t timelineViewStartPulse;
    uint64_t timelineViewEndPulse;
    bool timelineShowAll;
    bool captureEnabled;
    bool clearRecordingArmed;
    bool clockRunning;
    bool selectionValid;
    bool playbackArmed;
    bool playbackActive;
    bool playbackClockLossPaused;
    bool playbackIntervalOpen;
    bool playbackNextEventScheduled;
    bool pendingNextEndingScheduled;
    bool activeSelectionValid;
    bool rangeTransitionPending;
    bool lastMovedBoundaryIsStart;
    bool rangeMotionEstablished;
    SelectionFineTarget selectionFineTarget;
    double rangeLogicalPosition;
    double rangePhysicalPosition;
    double rangePulseResidual;
    PulseRange selection;
    PulseRange activeSelection;
};

// Per-channel absolute RPN0/0 and MCM RPN0/6 observer state. It lives in the
// separately reserved DRAM sidecar and is never part of the performance ring.
enum SetupKnowledge : uint8_t {
    kSetupUnknown = 0,
    kSetupKnown = 1,
    kSetupUnsupported = 2,     // received encoding outside 0..9600 cents
    kSetupIndeterminate = 3,   // relative, LSB-before-MSB or invalid MCM count
};

enum SetupRole : uint8_t {
    kSetupRoleUnknown = 0,
    kSetupRoleNone,
    kSetupRoleLowerManager,
    kSetupRoleLowerMember,
    kSetupRoleUpperManager,
    kSetupRoleUpperMember,
};

struct SetupSelector {
    uint8_t family;      // 0 none, 1 RPN, 2 NRPN
    uint8_t msb, lsb, validMask, dataMsb;
    bool dataValid;      // CC6 received for the current selection
    uint32_t generation; // increments whenever the selection changes
};

struct SetupValue {
    uint16_t cents;
    uint8_t state;       // SetupKnowledge
};

struct SetupZone {
    uint8_t count;       // member channels, 0 = disabled
    uint8_t countState;  // SetupKnowledge
    SetupValue manager;
    SetupValue member;   // latest supported member receipt, zone-wide
};

struct SetupObserver {
    SetupSelector selectors[16];
    SetupZone zones[2];  // 0 lower (manager channel 1), 1 upper (channel 16)
    uint8_t roles[16];   // SetupRole
    uint32_t membershipGeneration[16];
    // Latest RPN0/0 receipt per channel regardless of role. Any MCM clears
    // these, so they only describe channels no received MCM has reset; Manual
    // scope uses them for RPN0-only restoration without guessing MCM.
    SetupValue channelSensitivity[16];
};

// Historical expression values tracked with per-byte knownness.
enum HistoryValue : uint8_t {
    kHistoryBendLsb,
    kHistoryBendMsb,
    kHistoryChannelPressure,
    kHistoryTimbre,          // CC74
    kHistoryModulationMsb,   // CC1
    kHistoryModulationLsb,   // CC33
    kHistoryExpressionMsb,   // CC11
    kHistoryExpressionLsb,   // CC43
    kHistorySustain,         // CC64
    kHistorySostenuto,       // CC66
    kHistoryHold2,           // CC69
    kHistoryValueCount,
};

// One full 512-byte observation state used by live tracking, seeds,
// checkpoints, journal entries and contexts. It is padding-free so equal
// states compare bytewise. Setup fields mirror SetupObserver zones/roles.
struct HistoryState {
    uint8_t values[16][kHistoryValueCount];
    uint16_t known[16];        // bit per HistoryValue; clear means unknown
    uint16_t managerCents[2];
    uint16_t memberCents[2];
    uint8_t zoneCount[2];
    uint8_t zoneCountState[2]; // SetupKnowledge
    uint8_t managerState[2];
    uint8_t memberState[2];
    uint8_t roles[16];         // SetupRole
    uint16_t channelCents[16]; // SetupObserver::channelSensitivity
    uint8_t channelCentsState[16];
    uint8_t reserved[224];
};

// Immutable selection-start context: the state immediately before the
// selection's own events in [start,end). Incomplete coverage clears all
// knownness instead of certifying a checkpoint as stable history.
struct HistoryContextSnapshot {
    PulseRange range;
    uint64_t generation;
    uint32_t setupFingerprint;
    bool ready;
    bool coverageComplete;
    bool setupChangesInside;
    HistoryState state;
};

// Read-only seams used by the timeline/playback layers and native host tests.
CaptureSnapshot captureSnapshot(const _NT_algorithm* self);
bool recordedEventAt(const _NT_algorithm* self, uint32_t oldestFirstIndex,
                     RecordedEvent& event);
// Active is the context adopted by the current playback entry; candidate is
// the latest selection's prepared context. Neither mutates recorded links.
bool historyContext(const _NT_algorithm* self, bool candidate,
                    HistoryContextSnapshot& context);
// Finalizing-state entry: freezes capture and leaves its generated endings to
// bounded per-step progress (at most 128 appends per step). Legacy capture
// stop, playback start, clear and save complete any pending work themselves.
bool beginCaptureFinalization(_NT_algorithm* self);

// MPE playback lifecycle, visible status and bounded-work instrumentation.
enum MpePhase : uint8_t {
    kMpePhaseIdle,
    kMpePhaseFinalizing,
    kMpePhasePreparing,
    kMpePhaseArmed,
    kMpePhasePlaying,
    kMpePhaseFault,      // overload: Playback Off, fresh Off/On required
};

enum MpeStatus : uint8_t {
    kMpeStatusReady,
    kMpeStatusRecordChannel, // Record Ch must be Omni
    kMpeStatusPlayChannel,   // Play Ch must be Original
    kMpeStatusFilters,       // expression filters must be Off
    kMpeStatusInvalidScope,  // manual zero/overlapping or no known zone
    kMpeStatusScopeUnknown,  // Observed source without a known active zone
    kMpeStatusMismatch,      // known observed count disagrees with Manual
    kMpeStatusOverload,      // rejected cluster
};

struct MpeWorkCounters {
    uint32_t visits;        // source/heap visits
    uint32_t sends;         // NT send calls (setup/context/wrap/collision)
    uint32_t fanout;        // destination bits across sends
    uint32_t heap;          // heap comparisons/swaps
    uint32_t cleanupCalls;  // reserved cleanup calls/slot visits
    uint32_t cleanupFanout; // reserved cleanup destination bits
};

struct MpePlaybackSnapshot {
    MpeWorkCounters lastStep;
    MpeWorkCounters maximumStep;
    uint32_t ownedNotes;
    uint32_t ownedHolds;
    uint32_t pendingEndings;
    uint32_t rejectedClusters;
    uint32_t generatedSetup;     // generated setup sends (MCM/RPN0/null)
    uint32_t generatedContext;   // generated starting-context sends
    uint32_t generatedCleanup;   // generated wrap/stop/collision endings
    uint16_t scopeMask;          // channels owned by MPE wrap semantics
    uint8_t lowerMembers;
    uint8_t upperMembers;
    uint8_t phase;               // MpePhase
    uint8_t status;              // MpeStatus
    bool engaged;
    bool deliberate;
    bool restartRequired;        // incompatible candidate kept active range
    bool setupChangesInside;     // selected take changes setup (unsupported)
    bool setupIncomplete;
    bool contextIncomplete;
};
bool mpePlaybackSnapshot(const _NT_algorithm* self,
                         MpePlaybackSnapshot& snapshot);

#if defined(MIDIBUFFER_NATIVE_TEST)
struct HistorySnapshot {
    uint64_t baseOrdinal;          // ordinal of the oldest retained event
    uint64_t nextOrdinal;          // ordinal the next append receives
    uint64_t generation;
    uint64_t pairedGeneration;
    uint64_t coverageStartOrdinal; // journal coverage watermark
    uint64_t seedLastPulse;
    uint32_t checkpointCount;
    uint32_t journalCount;
    uint32_t journalEvictions;
    uint32_t lastStepFinalizeAppends;
    uint32_t maximumStepFinalizeAppends;
    uint32_t lastStepPreparationVisits;
    uint32_t maximumStepPreparationVisits;
    uint32_t maximumSeedSearchComparisons;
    uint32_t maximumCheckpointSearchComparisons;
    uint32_t maximumForwardEventVisits;
    uint32_t maximumJournalVisits;
    bool seedValid;
    bool seedFolded;
    bool finalizing;
    bool pairingComplete;
    HistoryState live;
    HistoryState recorded;
    HistoryState seed;
};
bool historySnapshot(const _NT_algorithm* self, HistorySnapshot& snapshot);
// Bounded read-only copy of the production setup observer state.
bool setupObserverSnapshot(const _NT_algorithm* self, SetupObserver& snapshot);
// Installs one synthetic retained interval envelope without iterating through
// its pulse span. This keeps wide-span callback tests practical while using
// the production view/navigation implementation.
bool setRetainedTimelineFixture(_NT_algorithm* self, uint64_t startPulse,
                                uint64_t endPulse);
bool addRetainedTimelineNoteFixture(_NT_algorithm* self, uint64_t pulse,
                                    uint8_t note);
bool setTimelineNavigationFixture(_NT_algorithm* self, bool showAll,
                                  uint64_t manualSpan, uint64_t scrollPulses,
                                  SelectionFineTarget target);
#endif

// Timeline selection is pulse-identity based. Playback callers must acquire a
// range through this contract; it refuses absent, malformed, or stale ranges.
// While transport is running, selection edits remain pending and the latest
// complete range replaces the active range atomically at its next loop wrap.
bool setPulseSelection(_NT_algorithm* self, uint64_t startPulse,
                       uint64_t endPulse);
void clearPulseSelection(_NT_algorithm* self);
bool acquirePlaybackSelection(const _NT_algorithm* self, PulseRange& range);

// Transport seam for pulse-boundary arming and proportional between-pulse
// replay. Starting pauses/finalizes capture. Manual stop releases playback-held
// notes/sustain while preserving position; only reset returns to range start.
bool startPlayback(_NT_algorithm* self);
void stopPlayback(_NT_algorithm* self);

// Safety cleanup is output-state work, not replay of recorded performance.
// The caller supplies the final status/channel; playback filters and channel
// override are deliberately bypassed while the selected destination is kept.
void dispatchSafetyMidi3(_NT_algorithm* self, uint8_t status, uint8_t data1,
                         uint8_t data2);

} // namespace midibuffer

#endif
