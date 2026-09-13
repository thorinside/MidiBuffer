# MidiBuffer

MidiBuffer is an Expert Sleepers disting NT plug-in targeting firmware 1.18 and later through the pinned API v13 SDK.

## Current behavior

MidiBuffer retains eligible channel MIDI as pulse-relative events in a fixed rolling history:

- Capture starts stopped. Set **Capture** to **Start Capture** or **Stop Capture** explicitly.
- The patched **Clock** input is tracked continuously, including while capture is stopped.
- Capture waits for two clock pulses when no interval is known. After two last-measured intervals pass without a pulse, clock is considered lost; capture or playback waits for two fresh pulses to establish a new interval before resuming.
- **Record Ch** selects **Omni** or one MIDI channel from 1 through 16.
- Notes, CC, pitch bend, channel pressure, and polyphonic pressure are retained. Program changes, MIDI realtime/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are excluded.
- Every event stores its clock-pulse identity, sample offset from that pulse, and the measured source interval needed to retain a fractional position. When capacity is reached, the oldest event is replaced.
- If replacement touches a selected pulse range, the selection is cleared while capture continues. The playback-entry seam refuses a missing or invalidated selection.
- The 256×64 custom timeline shows compact retained availability in beats, note marks, pulse-aligned selection brackets, and selected length. It intentionally omits the prototype's history heading and bottom oldest-history number.
- **Pulses/Beat** is runtime-adjustable among **1**, **2**, **4**, **8**, **16**, **24**, and **48**, defaulting to **1**. It converts retained and selected pulse intervals to displayed beats only; recorded timestamps and loop boundaries do not change. For example, 64 retained pulse intervals display as 16 beats at 4 pulses per beat.
- The left, centre, and right pots move selection start, move selection end, and zoom the timeline. The left encoder scrolls toward older or newer retained history; the right encoder trims the last-moved boundary by one clock pulse per detent. Scrolling and zooming preserve both selected pulse coordinates. Boundary edits made during playback use the same atomic next-wrap transition as other selection edits.
- **MIDI Out** selects **Breakout**, **USB**, **Select Bus**, **Internal**, or **All** for retained-event replay. **All** sends with the four documented destination bits (0x0f).
- **Play Ch** defaults to **Original**, preserving each recorded channel. Values 1–16 replace the channel of every replayed channel message with the selected channel.
- **Filter CC**, **Filter Pitch Bend**, and **Filter Aftertouch** affect retained-event playback only. Each defaults to **Off**, so recorded expression plays by default. **Filter CC** covers every eligible controller as one group, while **Filter Aftertouch** covers both channel and polyphonic pressure. Turning a filter on suppresses output without changing history; turning it off restores the original recorded bytes.
- Program changes, SysEx, MIDI clock/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are rejected at capture and guarded against again at retained-event dispatch.
- Stopping capture appends finite endings at one final capture timestamp for recorded state that remains active: outstanding note-offs; CC64–69 value 0; CC1/33 value 0; CC11/43 value 127; centred pitch bend; and zero channel/polyphonic pressure. Volume, pan, sound settings, effects, Channel Mode, and RPN/NRPN are not synthesized. Starting playback uses the same capture-finalization path.
- Capture-stop endings are stored in the rolling history—even when it is full—and are not sent to live outputs merely because capture stopped. The finalized current pulse is immediately selectable and its endings replay through the ordinary scheduler.
- Safety-output dispatch is separate from recorded-event filtering: cleanup CC and note-off messages retain the selected destination but bypass expression filters and playback channel rewriting.
- Playback entry finalizes and pauses active capture. Incoming MIDI cannot change retained history during playback, and manual playback stop never restarts capture; **Start Capture** is required explicitly.
- Replay arms through the timeline/playback core seam. With a current measured interval it starts on the next received pulse; without one it waits for the second acquisition pulse. Selected boundaries remain pulse-aligned, zero-offset events dispatch on the actual pulse, and other retained offsets are proportionally scheduled between pulses.
- Playback tracks the notes and CC64 sustain state it actually emits after playback filtering and channel routing. Manual stop immediately sends all held note-offs followed by sustain-off on every affected output channel, then preserves the stopped position for the next playback start.
- A note-on inside the selected range owns its recorded note-off even when that ending lies beyond the range. The ending follows the same pulse-relative tempo scaling after an ordinary wrap. Notes begun before selection are not reconstructed, and their in-range note-offs do not create attacks.
- Before an in-range note-on retriggers the same pitch on the same routed output channel, playback sends the older occurrence's note-off and cancels that occurrence's pending ending. This collision rule therefore also covers recorded channels combined by **Play Ch** override.
- An in-range CC64 press likewise owns its recorded release beyond the range. Ordinary wrap does not force sustain off. A newer press on the same routed output channel cancels the older pending release, so sufficiently long overlapping pedal passages can remain continuously sustained.
- Loop selection edits made during playback remain pending while the current active range finishes. At that range's next wrap, the latest complete start/end pair becomes active without stopping playback; held note-offs and sustain-off are sent before any event from the new range. Further unchanged wraps preserve pending note and sustain endings. Manual stop, reset, clock loss, and range adoption cancel all pending endings as part of their existing cleanup.
- Reset is independent of clock. It performs the same ordered cleanup immediately, returns playback to the active range start, and emits no new attack until a clock pulse. When reset and clock rise on the same audio frame, cleanup and repositioning happen first and the clock then opens the unskipped first beat.
- Clock loss is declared exactly two last-measured intervals after the last pulse. It performs the same cleanup and pauses at the current playback position. The old running-average tempo is discarded; the first returning pulse starts acquisition and the second establishes the new tempo and continues from the saved position without fabricating replacement note-ons for silenced notes.
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

Conventional output-state interpretation follows the MIDI Manufacturers Association’s *MIDI 1.0 Detailed Specification*: `8n` is Note Off, `9n` with velocity zero is treated as Note Off, and CC64 values 0–63 are sustain off while 64–127 are sustain on. Separately, sending CC64 off to a whole output channel during transport cleanup is the approved MidiBuffer product tradeoff; it can also release live sustained notes sharing that channel.

Persistence of pending range edits and timeline navigation, encoder press/hold actions, emergency all-channel silence, preset persistence, packaging/publication, external-routing redesign, selection-context reconstruction, instrument-specific silencing guarantees, and physical hardware validation remain outside this delivery slice.

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

Native checks cover stopped/unclocked gating, initial acquisition, continuous stopped-clock tracking, loss/reacquisition and averaging-history discard, pulse-relative timestamps, proportional quarter/halfway timing at multiple steady tempos, actual-pulse aligned dispatch, ordered early-pulse catch-up, all recording-channel settings, approved event exclusions, fixed allocation and capacity accounting, mixed-density retention, ring wraparound, selection invalidation, continued capture, and playback-entry refusal. Timeline callback traces and framebuffer assertions cover note marks, brackets, selected length, compact rolling-history availability while capturing and overwriting, the 64-pulse/16-beat conversion at 4 pulses per beat, all seven runtime beat conversions, independent pot boundary edits, last-boundary pulse trimming, zoom/scroll selection invariance, and live edits entering the next-wrap transition seam during playback. Capture-stop traces verify manual and playback-start finalization, exact targeted ending bytes at one timestamp, immediate selection and scheduler replay, full-buffer retention, untouched controller classes, exclusions, and the absence of live output at stop. Transport traces verify playback/capture exclusion and explicit-only capture restart; immediate manual-stop cleanup and saved-position continuation; atomic latest-pair range adoption at the current loop wrap without a mid-phrase jump; changed-range note/sustain cleanup before the new attack; unchanged ordinary wraps; independent and coincident reset cleanup-before-first-beat ordering; held-note and sustain cleanup with active expressive filters; exact one-second loss at a last 500 ms interval; fresh two-pulse reacquisition; and continuation without a fabricated attack. Multi-pass ownership traces verify tempo-scaled note-offs and sustain releases outside the selection, no selection-start attack for a pre-range note, same-pitch collisions after channel override, cancellation of stale note endings, newest-press sustain ownership, continuous overlapping sustain, and pending-ending cancellation on stop, reset, clock loss, and range adoption. A retained-event output trace matrix exercises the production clock-to-NT-adapter call path for all five destinations, Original plus every 1–16 channel override, every retained message class, and routed held-note cleanup. A second matrix records one mixed eligible/excluded vocabulary while all playback filters are enabled, then replays the unchanged history through all eight CC/bend/aftertouch filter combinations, restores exact expression bytes, and verifies that safety dispatch bypasses those filters. It verifies that All is exactly Breakout, USB, Select Bus, and Internal. Tests instantiate MidiBuffer through the production `pluginEntry`/factory path and check callback-time C++ heap allocation.

These automated checks do not claim that the plug-in has been loaded on a physical disting NT.
