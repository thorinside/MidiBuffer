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
    uint8_t flags;
};

struct PulseRange {
    uint64_t startPulse;
    uint64_t endPulse;
};

struct CaptureSnapshot {
    uint32_t recordingAllocationBytes;
    uint32_t metadataBytes;
    uint32_t eventCapacity;
    uint32_t eventCount;
    uint64_t currentPulse;
    uint64_t oldestPulse;
    uint64_t newestPulse;
    uint64_t lastClockIntervalSamples;
    uint64_t predictedClockIntervalSamples;
    uint64_t playbackPulse;
    uint32_t clockAverageIntervalCount;
    bool captureEnabled;
    bool clockRunning;
    bool selectionValid;
    bool playbackArmed;
    bool playbackActive;
    PulseRange selection;
};

// Read-only seams used by the timeline/playback layers and native host tests.
CaptureSnapshot captureSnapshot(const _NT_algorithm* self);
bool recordedEventAt(const _NT_algorithm* self, uint32_t oldestFirstIndex,
                     RecordedEvent& event);

// Timeline selection is pulse-identity based. Playback callers must acquire a
// range through this contract; it refuses absent, malformed, or stale ranges.
bool setPulseSelection(_NT_algorithm* self, uint64_t startPulse,
                       uint64_t endPulse);
void clearPulseSelection(_NT_algorithm* self);
bool acquirePlaybackSelection(const _NT_algorithm* self, PulseRange& range);

// Transport seam for pulse-boundary arming and proportional between-pulse
// replay. Timeline controls will own this contract; starting is refused without
// a valid retained selection.
bool startPlayback(_NT_algorithm* self);
void stopPlayback(_NT_algorithm* self);

// Safety cleanup is output-state work, not replay of recorded performance.
// The caller supplies the final status/channel; playback filters and channel
// override are deliberately bypassed while the selected destination is kept.
void dispatchSafetyMidi3(_NT_algorithm* self, uint8_t status, uint8_t data1,
                         uint8_t data2);

} // namespace midibuffer

#endif
