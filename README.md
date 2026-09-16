# MidiBuffer

MidiBuffer is a rolling, clock-relative MIDI recorder and looper for the Expert Sleepers disting NT. It is intended for musicians who want to capture a performance, select a pulse-aligned section on the module's timeline, and repeat it against a patched external clock without flattening expression between clock pulses.

## Compatibility and test status

- **Supported target:** disting NT firmware **1.18 and later**.
- Firmware earlier than 1.18 is unsupported.
- The plug-in builds against the pinned API v13 SDK checkout. Native host-double tests and ARM object inspection pass, but those checks do not run firmware.
- No physical disting NT firmware version has yet been recorded as tested. In particular, the 1.18 minimum is a support target, not a claim that 1.18 has already been exercised on hardware, and compatibility with later firmware versions has not yet been verified. The original v1 pre-publication procedure remains in [`docs/HARDWARE_SMOKE_TEST.md`](docs/HARDWARE_SMOKE_TEST.md); the separate post-build UX control/display procedure is in [`docs/UX_REFINEMENT_HARDWARE_TEST.md`](docs/UX_REFINEMENT_HARDWARE_TEST.md). The original AC-001/AC-044/AC-048 gate and the UX-AC-002/UX-AC-008/UX-AC-012 gate remain separate, pending, and unwaived; native or framebuffer-emulator results do not satisfy either gate.

## Install

Build or obtain `MidiBuffer.o`, then place it at:

```text
/programs/plug-ins/MidiBuffer.o
```

Load **MidiBuffer** using the disting NT's normal plug-in loading workflow. The repository build product is `plugins/MidiBuffer.o`.

## Controls and defaults

Choose **Buffer MB** when creating the algorithm; the other controls are ordinary run-time parameters.

| Page | Control | Values or range | Default | Purpose |
| --- | --- | --- | --- | --- |
| Specification | **Buffer MB** | 1–5 MB | 1 MB | Fixes rolling-event memory for this instance; it cannot be resized at run time. |
| Inputs | **Clock** | CV bus 1–64 | Input 1 | Rising pulses provide the recording grid and playback tempo. |
| Inputs | **Reset** | CV bus 1–64 | Input 2 | Immediately cleans up playback output and returns playback to the active range start. |
| Capture | **Capture** | Stop Capture, Start Capture | Stop Capture | Explicitly enables or stops recording. |
| Capture | **Record Ch** | Omni, 1–16 | Omni | Admits all channels or one selected incoming MIDI channel. |
| Capture | **Clear Recording** | Off, On | Off | An armed Off-to-On change erases retained history once; return it to Off to rearm. |
| Playback | **Playback** | Off, On | Off | Shares one persistent playback request with the left encoder; On waits when no valid loop or clock is available. |
| Playback | **MIDI Out** | Breakout, USB, Select Bus, Internal, All | Breakout | Chooses the replay, cleanup, and panic destination. |
| Playback | **Play Ch** | Original, 1–16 | Original | Preserves recorded channels or rewrites every replayed channel message to one channel. |
| Playback | **Filter CC** | Off, On | Off | Suppresses eligible recorded CC during playback when On. |
| Playback | **Filter Pitch Bend** | Off, On | Off | Suppresses recorded pitch bend during playback when On. |
| Playback | **Filter Aftertouch** | Off, On | Off | Suppresses channel and polyphonic pressure during playback when On. |
| Timeline | **Pulses/Beat** | 1, 2, 4, 8, 16, 24, 48 | 1 | Changes only the timeline's beat conversion, not timing or loop boundaries. |

The CV inputs respond to rising edges above 1 V. **Clock** and **Reset** require a routed bus; neither has a None setting.

Every recorded note-on in the visible range contributes to the timeline. Notes sharing a screen column merge into a stem reaching the highest pitch in that column, so dense recordings remain represented across the whole view. Zoom in to separate overlapping marks. This display merging does not limit recording or playback.

### Timeline controls

| Physical control | Action |
| --- | --- |
| Left pot | Move the selection Start. The first boundary edit creates a full-history selection before applying the edit. If Start is offscreen, its upper edge handle points toward it; the first deliberate turn retrieves it to the nearest valid visible pulse, then further turns make zoom-scaled adjustments. |
| Centre pot | Move the selection End. If End is offscreen, its lower edge handle points toward it; the first deliberate turn retrieves it to the nearest valid visible pulse, then further turns make zoom-scaled adjustments. |
| Right pot turn | Move the complete selected range older/newer without changing its pulse length. |
| Right pot hold + turn | Zoom relative to the view at press time; release returns to range movement without a jump or pickup dead zone. |
| Left encoder turn | Scroll a manual view toward older or newer retained history by one pulse per detent without moving the selection. Show All cannot scroll. |
| Left encoder press | Toggle the shared **Playback** parameter. On remains requested even when no valid selection exists; a later valid selection starts under the normal clock gate. |
| Right encoder turn | Adjust the last effective Start, End, or whole-Range function by one pulse per detent. |
| Right encoder hold for one second | Panic once: stop playback and send CC120 plus CC123 on all 16 channels to the selected destination. |

## Capture and loop workflow

1. Add MidiBuffer and choose **Buffer MB**. Patch the external pulse source to **Clock**, and patch **Reset** if wanted.
2. Set **Record Ch**, **MIDI Out**, **Play Ch**, and the three playback filters. The filters do not alter retained history.
3. Start the external clock. MidiBuffer needs two pulses to measure an interval when it has no current clock measurement.
4. Set **Capture** to **Start Capture**, then perform. Only eligible channel MIDI received while capture is enabled and the external clock is running is retained.
5. Set **Capture** to **Stop Capture**. MidiBuffer stores finite endings for active notes and selected expressive state; stopping capture does not transmit those endings to live outputs.
6. Touch a selection boundary control to create and trim a valid pulse-aligned range. Turn the unpressed right pot to move that fixed-length range. Hold the right pot while turning to zoom; release it to resume range movement. Fresh views use **Show All** and continue fitting retained history; zoom in before using the left encoder to inspect a narrower region. An upper **Start** or lower **End** edge handle points toward an offscreen boundary. Deliberately turn that boundary's pot once to retrieve the actual boundary to the nearest valid visible pulse; subsequent turns use the finer zoom scale. Retrieval changes the selected phrase but does not scroll the viewport, and always preserves Start before End with at least one pulse between them. **Pulses/Beat**, zoom, and scroll never change recorded timing or the selection.
7. Set **Playback** to On, or press the left encoder to toggle that same value On. If a clock interval is still known, playback begins on the next pulse; otherwise it waits for two fresh pulses. On remains requested if no valid loop exists, and a later valid selection starts under the same clock rule. Starting playback also finalizes and pauses active capture.
8. Set **Playback** to Off, or press the left encoder again, to stop. Playback position is preserved for continuation. Use **Reset** to clean up and return to the range start. Capture never restarts automatically; select **Start Capture** again when you want to record more.

To deliberately erase the buffer, change **Clear Recording** from **Off** to **On** on the Capture parameter page. It stays On after erasing and ignores repeated On writes, so new eligible input is retained if Capture was already enabled. Return it to Off before requesting another erase. Clear has no custom faceplate shortcut. Clearing stops playback, releases its held notes and sustain through the selected destination, sets Playback Off, and discards every loop selection and playback position; playback then needs a new valid selection and explicit On action. Clear preserves the actual Capture enabled/disabled state and never starts Capture. Preset loading is not a clear gesture: a restored On is already consumed, preserves the restored recording, and still requires a later Off then On before another erase.

Edits made while playing are adopted together at the next loop wrap. The current range finishes first, then held-note and sustain cleanup occurs before the new range emits anything.

## Memory and history length

Each retained event currently occupies 24 bytes. **Buffer MB** uses decimal megabytes and produces these event capacities:

| Buffer MB | Event capacity |
| ---: | ---: |
| 1 | 41,666 |
| 2 | 83,333 |
| 3 | 125,000 |
| 4 | 166,666 |
| 5 | 208,333 |

Capacity is an event count, not a duration. Dense streams of notes, controllers, pitch bend, or pressure consume it faster and retain fewer clock pulses than sparse playing. At capacity, the oldest event is replaced. If replacement reaches the selected range, MidiBuffer clears that selection while capture continues so stale history cannot be played.

Larger buffers also make larger presets. In the native host model, a full 5 MB event history serializes to approximately 10.1 MB of custom JSON string payload. API v13 publishes no payload-size or save/load-time guarantee, so this measurement is not a firmware guarantee.

## Routing and shared-channel safety

- **MIDI Out: All** sends to Breakout, USB, Select Bus, and Internal. Existing external or internal forwarding can therefore duplicate messages or create feedback; MidiBuffer cannot detect the rest of the routing graph.
- **Play Ch: Original** keeps recorded channels. A 1–16 override merges all recorded channels onto one output channel. Same-note and sustain ownership collisions are then resolved on that shared output channel, not on the original channels.
- Manual stop, reset, clock loss, panic, and adoption of a changed range clean up output state through the selected destination. Ordinary stop/reset cleanup sends note-offs for notes MidiBuffer still owns, followed by CC64 value 0 on channels where its playback left sustain enabled.
- CC64 cleanup is channel-wide MIDI. It may also release live sustained notes from another source sharing that output channel. Use separate channels or destinations where that tradeoff is unacceptable.
- Panic is broader than ordinary cleanup: it sends All Sound Off (CC120) and All Notes Off (CC123) on all 16 channels. Incoming CC120 or CC123 during playback invokes the same panic. Incoming MIDI Stop is ignored because transport follows the patched clock.

## Presets and continuation

Valid NT presets preserve the retained event contents, order, and pulse-relative timing; selected, active, and pending ranges; playback cursor and scheduler state; capture/playback intent; parameters, filters, and routing; timeline state; and MidiBuffer's internal note, controller, and pending-ending ownership. The host's normal generic-parameter persistence saves all 12 run-time parameters, including the shared **Playback** and **Clear Recording** values, while custom state reconciles either host restoration order without treating those values as load-time actions.

Valid legacy images with only parameters 0–9 retain those inherited values and all complete custom state. Their saved transport intent migrates to **Playback** On for Armed, Playing, or ClockLossPaused and Off for Stopped; **Clear Recording** defaults Off and armed. New images that restore Clear On keep it consumed and preserve the recording until a later Off-to-On transition.

**Saved-state compatibility flag:** the state remains version 1 and retains its original required arrays. New saves append an authoritative `navigation` representation revision containing explicit Show All/manual policy, the exact manual span as four 16-bit parts of a `uint64`, and Start/End/Range target. New builds read complete legacy v1 presets without that member: every accepted legacy 4–256 width remains manual (including 64 and 256), with its end-relative scroll and Start/End target. The original 32-bit width slot remains valid for older readers; when the authoritative span is not representable in the legacy 4–256 range it contains the 64-pulse compatibility fallback rather than a truncated or 256-clamped value. An older build can therefore parse a new preset but cannot reproduce navigation modes it predates; the current build restores the authoritative member exactly. No recorded MIDI, scheduler, capture, clock-gating, or corruption-rejection meaning is changed by this extension.

Loading is silent. The live clock and reset levels, partial clock acquisition, current button holds, external routing, and connected instruments are not preset data. Restored capture or playback waits for two fresh clock pulses. Saved in-flight playback then continues its unconsumed interval and pending endings before advancing the loop; loading does not synthesize replacement attacks for external notes that were sounding before the load.

Only valid saved state is in MidiBuffer's scope. If a preset is corrupt or incomplete and firmware cannot load it, the algorithm is not loaded; failure handling and error presentation belong to the firmware. MidiBuffer does not promise plug-in-specific recovery, partial restoration, or recovery of edits made after the preset was saved.

## Version-one limits

- Version one does **not** provide direct MIDI-file export.
- No MIDI-file export workaround is provided or required. Reconsidering export depends on suitable future firmware/API file-writing support.
- Program changes, SysEx, MIDI realtime/transport, Channel Mode CC120–127, and RPN/NRPN-related CC6, CC38, and CC96–101 are not recorded.
- The plug-in does not reconstruct notes that began before the selected range, serialize external clocks/instruments/routing, guarantee silence for other devices sharing a channel, recover corrupt/incomplete presets, or recover unsaved changes.
- Licensing, distribution/publication, routing redesign, extra panic gestures, MIDI transport following, and a broad hardware compatibility programme are outside version one. The remaining physical release check is intentionally modest rather than a firmware/hardware matrix.

## Detailed behavior reference

MidiBuffer retains eligible channel MIDI as pulse-relative events in a fixed rolling history:

- Capture starts stopped. Set **Capture** to **Start Capture** or **Stop Capture** explicitly.
- **Clear Recording** erases once on an armed Off-to-On transition and remains On but consumed until returned to Off. It preserves Capture and clock/channel eligibility, while stopping Playback, performing ordered held-note/sustain cleanup, and invalidating all retained history, selections, pending ranges, playback position, queued events, and pending endings. There is no faceplate shortcut and erased events cannot replay.
- The patched **Clock** input is tracked continuously, including while capture is stopped.
- Capture waits for two clock pulses when no interval is known. After two last-measured intervals pass without a pulse, clock is considered lost; capture or playback waits for two fresh pulses to establish a new interval before resuming.
- **Record Ch** selects **Omni** or one MIDI channel from 1 through 16.
- Notes, CC, pitch bend, channel pressure, and polyphonic pressure are retained. Program changes, MIDI realtime/transport, Channel Mode CC120–127, and CC6, CC38, and CC96–101 are excluded.
- Every event stores its clock-pulse identity, sample offset from that pulse, and the measured source interval needed to retain a fractional position. When capacity is reached, the oldest event is replaced.
- If replacement touches a selected pulse range, the selection is cleared while capture continues. The playback-entry seam refuses a missing or invalidated selection.
- The 256×64 custom timeline shows retained availability and edited selection length as exact `bars:beats:ticks` durations, plus note marks and pulse-aligned selection brackets. A bar is four beats, a beat has 480 display ticks, beats are zero-based 0–3, and ticks always have three digits. **Avail** is the retained interval envelope (or zero), followed by the configured musical resolution in PPQN (pulses per quarter note), such as `16 PPQN`; this is the **Pulses/Beat** setting, not a measured clock frequency. **Len** is edited end minus start without an inclusive `+1` (or `--` when invalid). These pulse-derived readouts remain available without a live clock. The timeline intentionally omits the prototype's history heading and bottom oldest-history number.
- **Pulses/Beat** is runtime-adjustable among **1**, **2**, **4**, **8**, **16**, **24**, and **48**, defaulting to **1**. It changes only duration presentation; recorded timestamps and loop boundaries do not change, and tempo changes do not change either duration. For example, 64 intervals at 4 pulses per beat display as `4:0:000`, while 65 display as `4:0:120`. Bars display exactly through `9999999999`; larger amounts use `>9999999999bar` without limiting retained history.
- The left and centre pots move selection Start and End. A visible boundary uses its full-height bracket. An offscreen boundary uses a directional edge handle pointing toward its actual pulse: Start is the upper handle and End is the lower handle, so both remain identifiable even when they lie beyond the same edge. The first deliberate movement of that boundary's pot retrieves it to the nearest valid pulse inside the viewport without scrolling; later movement uses the viewport's zoom-scaled adjustment. Retrieval preserves pulse alignment, Start-before-End ordering, and the one-pulse minimum. The unpressed right pot moves the complete selected range older/newer while preserving its exact pulse length and clamping at retained-history limits. Holding the right pot while turning performs relative zoom from the press-time view; press and release do not move the selection, and the first unpressed delta is measured from the release sample.
- A fresh timeline is automatic **Show All** over the retained interval envelope and follows history growth/eviction. Zooming in creates a manual view; explicit full zoom-out re-enters Show All. Manual views preserve their configured width and end-relative scroll even when clipping temporarily makes them cover all retained history.
- The left encoder scrolls a manual view one pulse per delta unit toward older or newer history without acceleration. In Show All it is a no-op. The right encoder adjusts the last effective Start, End, or whole-Range function by one pulse per delta unit. Scrolling and held zoom preserve that target. Navigation preserves selected pulse coordinates, retained events, and playback timing. Boundary/range edits made during playback use the same atomic next-wrap transition as other selection edits.
- The left encoder toggles the persistent host-managed **Playback** value shared with remote parameter writes. On uses the existing clock-acquisition and saved-position path; Off performs the existing routed note/sustain cleanup. With an invalid selection, On remains requested without starting playback or changing capture, so a later valid selection can start under the normal clock gate.
- Holding the right encoder for one second invokes MIDI panic once per hold. Panic sends All Sound Off (CC120) and All Notes Off (CC123) on all 16 channels to the selected **MIDI Out** destination, stops playback without resetting its saved position, and leaves capture paused.
- During playback, incoming CC120 or CC123 on any channel invokes that same all-channel panic. Incoming MIDI Stop remains ignored because playback follows the patched clock. Panic stays stopped across later clock pulses until playback is explicitly restarted.
- **MIDI Out** selects **Breakout**, **USB**, **Select Bus**, **Internal**, or **All** for retained-event replay and panic. **All** sends with the four documented destination bits (0x0f); unselected destinations receive nothing.
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
- NT preset save uses the native `serialise` callback to capture a coherent, non-mutating point-in-time image of every retained event (contents, ring order, pulse timing, and ownership flags) plus selected/active/pending ranges, playback cursor and scheduler state, capture/playback intent, clock-history timing, recorded and playback note/controller ownership, explicit Show All/manual policy, exact 64-bit manual span, timeline scroll, Start/End/Range target, and pending note/sustain endings. The host's normal preset parameter handling preserves all 12 parameters, including shared Playback, Clear Recording, filters, and routing; native tests exercise parameter restoration both before and after custom deserialization.
- Loading through the native `deserialise` callback reconstructs that image in a fresh same-specification instance. Parameter restoration is reconciled as state rather than repeated Playback or Clear actions: it does not finalize capture, erase history, duplicate transport transitions, or send cleanup MIDI. Capture and playback intent are retained, but live clock/reset gate levels, partial clock acquisition, current encoder-button holds, and external instrument state are not pretended to be preset data. Restored activity emits nothing until two fresh patched-clock pulses establish a live interval. If playback was saved in flight, that interval is rebased—not advanced—so its unconsumed event cursor and same-interval note/sustain endings continue before the next pulse advances the loop. Playback-held output ownership remains available for deterministic continuation and later stop/reset cleanup; loading itself sends no MIDI.
- Valid preset payloads are supported for every **Buffer MB** value from 1 through 5. The canonical event payload is chunked hexadecimal because API v13 JSON exposes no 64-bit or binary primitive; 64-bit timing uses lossless 16-bit components. A full 5 MB history is therefore approximately 10.1 MB of custom JSON string payload in the native host model. The SDK publishes no payload-size or save/load-time budget, so the native measurement is evidence rather than a firmware guarantee. Corrupt/incomplete preset recovery and recovery of changes made after the save are not promised.

The **Buffer MB** specification is chosen before the algorithm instance is created. It supports 1–5 MB using decimal megabytes, has a hard maximum of 5,000,000 recording bytes, and cannot resize at runtime. Rolling-history metadata is requested separately in SRAM. Each current event record occupies 24 bytes, so the event capacities are 41,666 through 208,333; the display reports the actual capacity. Retained time is not fixed: denser MIDI consumes the same event capacity sooner and therefore retains fewer clock pulses.

The implementation also provides the executable host boundary:

- `pluginEntry`, factory discovery, instance requirements, and construction
- clock/reset CV parameters and callback-block edge scanning
- MIDI input/realtime, drawing, and custom-control callback adapters
- MIDI output and drawing host adapters
- native preset JSON callback adapters with fresh-instance reconstruction
- a native host double with deterministic callback, preset, and heap-allocation accounting
- a PIC ARM Cortex-M7 relocatable object build

The retained history contains only supported channel messages, so the replay path sends two-byte channel pressure and three-byte note, CC, polyphonic pressure, and pitch-bend messages; it does not send SysEx, program changes, or recorded system/realtime messages. Destination support can vary by message class. External or internal MIDI forwarding can create duplicates or feedback; MidiBuffer's destination selector does not prevent feedback outside the plug-in.

Conventional output-state interpretation follows the MIDI Manufacturers Association’s *MIDI 1.0 Detailed Specification*: `8n` is Note Off, `9n` with velocity zero is treated as Note Off, and CC64 values 0–63 are sustain off while 64–127 are sustain on. Separately, sending CC64 off to a whole output channel during transport cleanup is the approved MidiBuffer product tradeoff; it can also release live sustained notes sharing that channel.

Packaging/publication, external-routing redesign, selection-context reconstruction, instrument-specific silencing guarantees, additional panic gestures, MIDI transport following, corrupt/incomplete preset recovery, and unsaved-change recovery remain outside this delivery slice. The inherited physical power-cycle gate in [`docs/HARDWARE_SMOKE_TEST.md`](docs/HARDWARE_SMOKE_TEST.md) and the post-build UX gate in [`docs/UX_REFINEMENT_HARDWARE_TEST.md`](docs/UX_REFINEMENT_HARDWARE_TEST.md) remain separate and pending until their own result records are completed on a supported disting NT.

## Timing method and measured precision

The predictor uses an unweighted arithmetic mean of up to the eight most recently measured positive clock intervals, rounded to the nearest sample. A one-interval predictor follows jitter too closely; a substantially larger window smooths jitter further but lags deliberate tempo changes longer. Eight intervals is the chosen bounded compromise. The window and its sum are fixed-size, update without allocation, and are cleared completely on clock loss so reacquisition cannot reuse stale tempo.

For each retained event, playback rounds `recorded offset × predicted interval ÷ recorded source interval` to the nearest sample using integer arithmetic. This avoids floating-point drift and preserves quarter, halfway, and other fractional positions as tempo changes. Zero-offset events are emitted in the received clock-edge path. If an actual pulse arrives before predicted events from the preceding interval, those events are emitted immediately in original order before the next source pulse is opened.

The MIDI input callback supplies no arrival timestamp, while clock CV is scanned once per audio frame. Capture therefore measures MIDI at the callback boundary; a callback at the first observable sample after a scanned pulse is normalized to zero because the host interface cannot distinguish it more finely. MIDI output likewise has no timestamp argument. The native test build instruments the production NT output adapter with the scheduler's dispatch sample immediately before the ordinary send call; the ARM build retains the ordinary untimestamped API.

At the pinned host double's 48 kHz sample rate, deterministic traces at steady 16- and 24-sample intervals measured a maximum scheduler error of 0 samples against the integer due-sample model. This measures the scheduler and callback adapter in a native synthetic environment only. The short intervals are test stimuli, not recommended clock rates, and the result is not an end-to-end timing guarantee: it excludes MIDI callback arrival ambiguity, firmware scheduling, destination transport, receiving equipment, emulator wall-clock behavior, and physical disting NT latency.

## Prerequisites

Initialize the pinned SDK submodule and install Clang (or a compatible sanitizer-capable `NATIVE_CXX`), Cppcheck, Python 3, and an ARM GNU toolchain:

```sh
git submodule update --init --recursive
clang++ --version
cppcheck --version
python3 --version
arm-none-eabi-g++ --version
```

## Verify

```sh
make verify
```

This is the reproducible production regression gate. It runs optimized native host-callback and range-motion traces, repeats the complete callback suite under ASan/UBSan, runs focused Cppcheck plus a production-source blocking-I/O/logging inspection, validates [`docs/MIDIBUFFER_THREE_TRACEABILITY.json`](docs/MIDIBUFFER_THREE_TRACEABILITY.json), builds `plugins/MidiBuffer.o`, and verifies that the product is an ELF32 little-endian ARM relocatable object exporting `pluginEntry` with only the reviewed API v13 host/JSON and libc imports.

Native checks cover stopped/unclocked gating, initial acquisition, continuous stopped-clock tracking, loss/reacquisition and averaging-history discard, pulse-relative timestamps, proportional quarter/halfway timing at multiple steady tempos, actual-pulse aligned dispatch, ordered early-pulse catch-up, all recording-channel settings, approved event exclusions, fixed allocation and capacity accounting, mixed-density retention, ring wraparound, selection invalidation, continued capture, and playback-entry refusal. Preset callback checks cover fresh stopped initialization; valid round trips at every 1–5 MB specification; a full 5 MB retained history; capture-enabled and active-playback snapshots; exact canonical reserialization equality; parameter-before/after callback ordering; retained parameters, routing, filters, ranges, pending edits, timeline state, cursor, note/controller ownership and pending endings; no output on load or first clock; capture/playback mutual exclusion; two-pulse external-clock gating; and normalized timestamp/byte/routing/state equivalence across saved same-interval endings, multiple subsequent pulses, and pending range adoption after reacquisition. Timeline callback traces and framebuffer assertions cover note marks, visible brackets, directional offscreen Start/End handles (including both handles beyond either shared edge), handle-to-pot retrieval, exact retained/edited `bars:beats:ticks` durations, every supported conversion across `N=0..8P`, rollover/wide/overflow vectors, restored durations through clock loss and reacquisition, fixed-cell tiny glyphs at the original readout origins, independent pot boundary edits, last-boundary pulse trimming, zoom/scroll selection invariance, and live edits entering the next-wrap transition seam during playback. Capture-stop traces verify manual and playback-start finalization, exact targeted ending bytes at one timestamp, immediate selection and scheduler replay, full-buffer retention, untouched controller classes, exclusions, and the absence of live output at stop. Transport traces verify playback/capture exclusion and explicit-only capture restart; immediate manual-stop cleanup and saved-position continuation; atomic latest-pair range adoption at the current loop wrap without a mid-phrase jump; changed-range note/sustain cleanup before the new attack; unchanged ordinary wraps; independent and coincident reset cleanup-before-first-beat ordering; held-note and sustain cleanup with active expressive filters; exact one-second loss at a last 500 ms interval; fresh two-pulse reacquisition; and continuation without a fabricated attack. Emergency-control traces cover incoming CC120/123 on all 16 channels across every destination, ignored MIDI Stop with subsequent pulse advancement, all-channel panic output, stopped-clock suppression until explicit restart, valid/invalid left-encoder toggles, and right-encoder panic at the exact 48 kHz one-second threshold without early or repeated firing. Multi-pass ownership traces verify tempo-scaled note-offs and sustain releases outside the selection, no selection-start attack for a pre-range note, same-pitch collisions after channel override, cancellation of stale note endings, newest-press sustain ownership, continuous overlapping sustain, and pending-ending cancellation on stop, reset, clock loss, and range adoption. A retained-event output trace matrix exercises the production clock-to-NT-adapter call path for all five destinations, Original plus every 1–16 channel override, every retained message class, and routed held-note cleanup. A second matrix records one mixed eligible/excluded vocabulary while all playback filters are enabled, then replays the unchanged history through all eight CC/bend/aftertouch filter combinations, restores exact expression bytes, and verifies that safety dispatch bypasses those filters. It verifies that All is exactly Breakout, USB, Select Bus, and Internal. Tests instantiate MidiBuffer through the production `pluginEntry`/factory path and check callback-time C++ heap allocation.

The gate reports native/framebuffer-emulator observations, ARM inspection, and pending physical evidence separately. Its native full-5 MB payload and process-CPU time measurements are observations, not invented firmware budgets. These automated checks do not claim that the plug-in has been loaded on a physical disting NT. The complete approved-Spec audit and remaining owner gate are recorded in [`docs/RELEASE_READINESS.md`](docs/RELEASE_READINESS.md).
