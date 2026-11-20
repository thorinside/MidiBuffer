// ARM resource inspection only: compiled for the plug-in target but never
// linked or shipped. Each symbol's size (arm-none-eabi-nm -S) is the ARM-ABI
// layout value `make inspect` checks against the approved resource ceilings.
#include "midibuffer.cpp"

extern "C" {
extern const uint8_t midibufferLayoutSram[sizeof(midibuffer::Algorithm)];
const uint8_t midibufferLayoutSram[sizeof(midibuffer::Algorithm)] = {};
extern const uint8_t
    midibufferLayoutSidecar[sizeof(midibuffer::MpeArenaReservation)];
const uint8_t
    midibufferLayoutSidecar[sizeof(midibuffer::MpeArenaReservation)] = {};
extern const uint8_t midibufferLayoutSidecarUsed[offsetof(
    midibuffer::MpeArenaReservation, alignmentAndSlack)];
const uint8_t midibufferLayoutSidecarUsed[offsetof(
    midibuffer::MpeArenaReservation, alignmentAndSlack)] = {};
extern const uint8_t
    midibufferLayoutEvent[sizeof(midibuffer::RecordedEvent)];
const uint8_t midibufferLayoutEvent[sizeof(midibuffer::RecordedEvent)] = {};
extern const uint8_t
    midibufferLayoutPresetMaximum[midibuffer::kMpeExtensionMaximumBytes];
const uint8_t
    midibufferLayoutPresetMaximum[midibuffer::kMpeExtensionMaximumBytes] = {};
}
