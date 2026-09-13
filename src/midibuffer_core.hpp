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
    uint8_t bytes[3];
    uint8_t size;
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
    bool captureEnabled;
    bool clockRunning;
    bool selectionValid;
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

// Minimal transport seam for the clock-driven replay path. The timeline
// controls will own this contract; starting is refused without a valid retained
// selection.
bool startPlayback(_NT_algorithm* self);
void stopPlayback(_NT_algorithm* self);

} // namespace midibuffer

#endif
