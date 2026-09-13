# MidiBuffer

MidiBuffer is an Expert Sleepers disting NT plug-in targeting firmware 1.18 and later through the pinned API v13 SDK.

## Current behavior

MidiBuffer retains eligible channel MIDI as pulse-relative events in a fixed rolling history:

- Capture starts stopped. Set **Capture** to **Start Capture** or **Stop Capture** explicitly.
- The patched **Clock** input is tracked continuously, including while capture is stopped.
- Capture waits for two clock pulses when no interval is known. After two measured intervals pass without a pulse, clock is considered lost; capture waits for two fresh pulses before resuming.
- **Record Ch** selects **Omni** or one MIDI channel from 1 through 16.
- Notes, CC, pitch bend, channel pressure, and polyphonic pressure are retained. Program changes, MIDI realtime/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are excluded.
- Every event stores its clock-pulse identity, sample offset from that pulse, and the measured source interval needed to retain a fractional position. When capacity is reached, the oldest event is replaced.
- If replacement touches a selected pulse range, the selection is cleared while capture continues. The playback-entry seam refuses a missing or invalidated selection.
- **MIDI Out** selects **Breakout**, **USB**, **Select Bus**, **Internal**, or **All** for retained-event replay. **All** sends with the four documented destination bits (0x0f).
- **Play Ch** defaults to **Original**, preserving each recorded channel. Values 1–16 replace the channel of every replayed channel message with the selected channel.
- **Filter CC**, **Filter Pitch Bend**, and **Filter Aftertouch** affect retained-event playback only. Each defaults to **Off**, so recorded expression plays by default. **Filter CC** covers every eligible controller as one group, while **Filter Aftertouch** covers both channel and polyphonic pressure. Turning a filter on suppresses output without changing history; turning it off restores the original recorded bytes.
- Program changes, SysEx, MIDI clock/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are rejected at capture and guarded against again at retained-event dispatch.
- Stopping capture appends finite endings at one final capture timestamp for recorded state that remains active: outstanding note-offs; CC64–69 value 0; CC1/33 value 0; CC11/43 value 127; centred pitch bend; and zero channel/polyphonic pressure. Volume, pan, sound settings, effects, Channel Mode, and RPN/NRPN are not synthesized. Starting playback uses the same capture-finalization path.
- Capture-stop endings are stored in the rolling history—even when it is full—and are not sent to live outputs merely because capture stopped. The finalized current pulse is immediately selectable and its endings replay through the ordinary scheduler.
- Safety-output dispatch is separate from recorded-event filtering: cleanup CC and note-off messages retain the selected destination but bypass expression filters and playback channel rewriting.
- Replay arms through the timeline/playback core seam. With a current measured interval it starts on the next received pulse; without one it waits for the second acquisition pulse. Selected boundaries remain pulse-aligned, zero-offset events dispatch on the actual pulse, and other retained offsets are proportionally scheduled between pulses.
- An early pulse first catches up every pending event from the preceding source interval in recorded order, then advances the selected range. Clock prediction uses the running-average method documented below.

The **Buffer MB** specification is chosen before the algorithm instance is created. It supports 1–5 MB using decimal megabytes, has a hard maximum of 5,000,000 recording bytes, and cannot resize at runtime. Rolling-history metadata is requested separately in SRAM. Each current event record occupies 24 bytes, so the event capacities are 41,666 through 208,333; the display reports the actual capacity. Retained time is not fixed: denser MIDI consumes the same event capacity sooner and therefore retains fewer clock pulses.

The implementation also provides the executable host boundary:

- `pluginEntry`, factory discovery, instance requirements, and construction
- clock/reset CV parameters and callback-block edge scanning
- MIDI input/realtime, drawing, and custom-control callback adapters
- MIDI output and drawing host adapters
- a native host double with deterministic callback traces and heap-allocation accounting
- a PIC ARM Cortex-M7 relocatable object build

The retained history contains only supported channel messages, so the replay path sends two-byte channel pressure and three-byte note, CC, polyphonic pressure, and pitch-bend messages; it does not send SysEx, program changes, or recorded system/realtime messages. Destination support can vary by message class. External or internal MIDI forwarding can create duplicates or feedback; MidiBuffer's destination selector does not prevent feedback outside the plug-in.

Full timeline controls/display, note-tail ownership, interruption cleanup, emergency silence, preset persistence, packaging/publication, external-routing redesign, and physical hardware validation remain outside this delivery slice.

## Timing method and measured precision

The predictor uses an unweighted arithmetic mean of up to the eight most recently measured positive clock intervals, rounded to the nearest sample. A one-interval predictor follows jitter too closely; a substantially larger window smooths jitter further but lags deliberate tempo changes longer. Eight intervals is the chosen bounded compromise. The window and its sum are fixed-size, update without allocation, and are cleared completely on clock loss so reacquisition cannot reuse stale tempo.

For each retained event, playback rounds `recorded offset × predicted interval ÷ recorded source interval` to the nearest sample using integer arithmetic. This avoids floating-point drift and preserves quarter, halfway, and other fractional positions as tempo changes. Zero-offset events are emitted in the received clock-edge path. If an actual pulse arrives before predicted events from the preceding interval, those events are emitted immediately in original order before the next source pulse is opened.

The MIDI input callback supplies no arrival timestamp, while clock CV is scanned once per audio frame. Capture therefore measures MIDI at the callback boundary; a callback at the first observable sample after a scanned pulse is normalized to zero because the host interface cannot distinguish it more finely. MIDI output likewise has no timestamp argument. The native test build instruments the production NT output adapter with the scheduler's dispatch sample immediately before the ordinary send call; the ARM build retains the ordinary untimestamped API.

At the pinned host double's 48 kHz sample rate, deterministic traces at steady 16- and 24-sample intervals measured a maximum scheduler error of 0 samples against the integer due-sample model. This measures the scheduler and callback adapter in a native synthetic environment only. The short intervals are test stimuli, not recommended clock rates, and the result is not an end-to-end timing guarantee: it excludes MIDI callback arrival ambiguity, firmware scheduling, destination transport, receiving equipment, emulator wall-clock behavior, and physical disting NT latency.

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

Native checks cover stopped/unclocked gating, initial acquisition, continuous stopped-clock tracking, loss/reacquisition and averaging-history discard, pulse-relative timestamps, proportional quarter/halfway timing at multiple steady tempos, actual-pulse aligned dispatch, ordered early-pulse catch-up, all recording-channel settings, approved event exclusions, fixed allocation and capacity accounting, mixed-density retention, ring wraparound, selection invalidation, continued capture, and playback-entry refusal. Capture-stop traces verify manual and playback-start finalization, exact targeted ending bytes at one timestamp, immediate selection and scheduler replay, full-buffer retention, untouched controller classes, exclusions, and the absence of live output at stop. A retained-event output trace matrix exercises the production clock-to-NT-adapter call path for all five destinations, Original plus every 1–16 channel override, and every retained message class. A second matrix records one mixed eligible/excluded vocabulary while all playback filters are enabled, then replays the unchanged history through all eight CC/bend/aftertouch filter combinations, restores exact expression bytes, and verifies that safety dispatch bypasses those filters. It verifies that All is exactly Breakout, USB, Select Bus, and Internal. Tests instantiate MidiBuffer through the production `pluginEntry`/factory path and check callback-time C++ heap allocation.

These automated checks do not claim that the plug-in has been loaded on a physical disting NT.
