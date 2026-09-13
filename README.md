# MidiBuffer

MidiBuffer is an Expert Sleepers disting NT plug-in targeting firmware 1.18 and later through the pinned API v13 SDK.

## Current behavior

MidiBuffer retains eligible channel MIDI as pulse-relative events in a fixed rolling history:

- Capture starts stopped. Set **Capture** to **Start Capture** or **Stop Capture** explicitly.
- The patched **Clock** input is tracked continuously, including while capture is stopped.
- Capture waits for two clock pulses when no interval is known. After two measured intervals pass without a pulse, clock is considered lost; capture waits for two fresh pulses before resuming.
- **Record Ch** selects **Omni** or one MIDI channel from 1 through 16.
- Notes, CC, pitch bend, channel pressure, and polyphonic pressure are retained. Program changes, MIDI realtime/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are excluded.
- Every event stores its clock-pulse identity and sample offset from that pulse. When capacity is reached, the oldest event is replaced.
- If replacement touches a selected pulse range, the selection is cleared while capture continues. The playback-entry seam refuses a missing or invalidated selection.
- **MIDI Out** selects **Breakout**, **USB**, **Select Bus**, **Internal**, or **All** for retained-event replay. **All** sends with the four documented destination bits (0x0f).
- **Play Ch** defaults to **Original**, preserving each recorded channel. Values 1–16 replace the channel of every replayed channel message with the selected channel.
- The current minimal replay transport starts through the timeline/playback core seam and advances selected retained pulses from external clock edges. Between-pulse proportional scheduling, playback filtering, note-tail ownership, cleanup, and final timeline controls are intentionally deferred.

The **Buffer MB** specification is chosen before the algorithm instance is created. It supports 1–5 MB using decimal megabytes, has a hard maximum of 5,000,000 recording bytes, and cannot resize at runtime. Rolling-history metadata is requested separately in SRAM. Each current event record occupies 16 bytes, so the event capacities are 62,500 through 312,500; the display reports the actual capacity. Retained time is not fixed: denser MIDI consumes the same event capacity sooner and therefore retains fewer clock pulses.

The implementation also provides the executable host boundary:

- `pluginEntry`, factory discovery, instance requirements, and construction
- clock/reset CV parameters and callback-block edge scanning
- MIDI input/realtime, drawing, and custom-control callback adapters
- MIDI output and drawing host adapters
- a native host double with deterministic callback traces and heap-allocation accounting
- a PIC ARM Cortex-M7 relocatable object build

The retained history contains only supported channel messages, so the replay path sends two-byte channel pressure and three-byte note, CC, polyphonic pressure, and pitch-bend messages; it does not send SysEx, program changes, or recorded system/realtime messages. Destination support can vary by message class. External or internal MIDI forwarding can create duplicates or feedback; MidiBuffer's destination selector does not prevent feedback outside the plug-in.

Proportional between-pulse scheduling and filtering, full timeline controls/display, stored stop endings, note-tail ownership, emergency silence, preset persistence, packaging/publication, external-routing redesign, and physical hardware validation remain outside this delivery slice.

## Prerequisites

Initialize the pinned SDK submodule and install an ARM GNU toolchain:

```sh
git submodule update --init --recursive
arm-none-eabi-g++ --version
```

## Verify

```sh
make verify
```

This runs native host-callback traces, builds `plugins/MidiBuffer.o`, and verifies that it is an ELF32 little-endian ARM relocatable object exporting `pluginEntry`.

Native checks cover stopped/unclocked gating, initial acquisition, loss and reacquisition, pulse-relative timestamps, all recording-channel settings, approved event exclusions, fixed allocation and capacity accounting, mixed-density retention, ring wraparound, selection invalidation, continued capture, and playback-entry refusal. A retained-event output trace matrix exercises the production clock-to-NT-adapter call path for all five destinations, Original plus every 1–16 channel override, and every currently retained message class. It verifies that All is exactly Breakout, USB, Select Bus, and Internal. Tests instantiate MidiBuffer through the production `pluginEntry`/factory path and check callback-time C++ heap allocation.

These automated checks do not claim that the plug-in has been loaded on a physical disting NT.
