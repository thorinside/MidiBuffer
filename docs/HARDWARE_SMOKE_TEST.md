# Post-build disting NT hardware smoke test

## Status

**Physical run pending.** This document records the exact modest release-gate procedure and the verified ARM build to use. It is not evidence that a physical disting NT has loaded or run the plug-in.

## Build under test

- Product: `plugins/MidiBuffer.o`
- Current candidate source commit: `a460387134ef225e6400d4feba962d2ea58e128d`
- ARM object SHA-256: `d15ab42d2ebd9f53b6c7eadc81f52a4edffffc0063038df973ea7bf90b6967a2`
- Pinned SDK commit: `5a4910d1d4233180114d6aee5ddaa4b8aec577e8` (`v1.18.0-1-g5a4910d`, API v13)
- Supported firmware range: disting NT firmware **1.18 and later**; earlier firmware is unsupported.
- Firmware actually tested: **pending — record the module's exact displayed version below.** The supported range is not a claim that every version in it has been tested.

The build was produced with `make clean && make verify`. Native callback tests passed, including exhaustive preset-state equality and continuation, and ARM inspection identified an ELF32 little-endian ARM relocatable object exporting `pluginEntry`. Those automated results do not replace the physical checks below.

This remains the inherited original v1 procedure for AC-001, AC-044, and AC-048. Updating its build identity does not expand its observations and does not satisfy or replace the separate UX-AC-002, UX-AC-008, and UX-AC-012 procedure in [`UX_REFINEMENT_HARDWARE_TEST.md`](UX_REFINEMENT_HARDWARE_TEST.md).

## Equipment and observation

Use one physical disting NT on supported firmware, a steady patched clock, a reset gate/button source, a MIDI input source, and a destination whose MIDI can be heard and preferably logged with timestamps. Keep the same clock rate before and after the power cycle. Copy the object above to `/programs/plug-ins/MidiBuffer.o`.

Use a short, recognizable performance containing, in this order:

1. two different notes with unequal spacing inside a clock interval;
2. sustain on, a sustained note, and sustain off;
3. one eligible CC movement; and
4. one pitch-bend or aftertouch movement.

A timestamped MIDI monitor is preferred for comparing contents, order, relative timing, note-offs, and CC64 value 0. If none is available, record exactly how those facts were observed; an audible loop by itself does not establish preset restoration.

## Procedure

### 1. Load and basic capture

1. Record the exact firmware version shown by the module.
2. Load `MidiBuffer.o`, add **MidiBuffer** with **Buffer MB = 1**, and record whether it loads and remains running without a loader error, crash, or removal.
3. Patch a steady external pulse to **Clock** and a separate source to **Reset**. Set **Pulses/Beat = 4**.
4. Set distinctive non-default values that can be checked after reload: **Record Ch**, **MIDI Out**, **Play Ch**, one playback filter, and **Pulses/Beat**. Record the chosen values.
5. Select **Start Capture**, wait for two clock pulses, play the recognizable performance above, then select **Stop Capture**. Confirm that the timeline contains the performance and that capture stop itself sends no live cleanup output.

### 2. Patched-clock looping and physical controls

1. Use the left and centre pots to select the captured phrase, and the right pot to choose a recognizable zoom.
2. Turn the left encoder to a recognizable scroll position. Turn the right encoder to trim the last-moved boundary by one pulse. Record the selected length and visible timeline state.
3. Press the left encoder to start playback. Confirm the loop follows the patched clock and preserves the event order and visibly/audibly unequal within-pulse spacing rather than snapping all events to pulses.
4. While playback runs, change both boundaries to queue a different valid range. Confirm the new pair takes effect together at the next wrap, not mid-phrase. Section 4 repeats this action and saves before that wrap so pending state is included.

### 3. Stop and reset cleanup

Use the retained sustained-note passage so a note and sustain are active at the observation point.

1. Stop playback with the left encoder while the note/sustain is active. Confirm immediate note-off for the held note followed by CC64 value 0 on the affected output channel; confirm no stuck note or sustain remains.
2. Restart playback and confirm it continues from the stopped position rather than silently restarting at the selection beginning.
3. While a note/sustain is active, trigger **Reset**. Confirm the same note/sustain cleanup, confirm playback returns to the active range beginning, and confirm no replacement attack occurs until the next clock pulse. On that pulse, confirm the first beat is not skipped.

### 4. Preset save, power cycle, load, and exact comparison

1. Start playback again. Create the pending range edit from section 2, then save the NT preset during the current range before it wraps. Save while the recognizable performance still has later events and a note/sustain ending to emit in the current interval.
2. Before powering down, record:
   - retained event contents, oldest-to-newest order, and pulse-relative timing;
   - selected range, currently active range, and pending edited range;
   - playback position and which events/endings in the interval remain unconsumed;
   - playback/capture state;
   - all ten runtime parameter values, including input routing, record/play channels, destination, three filters, and **Pulses/Beat**;
   - timeline zoom, scroll, and which boundary was last moved; and
   - the expected pending note-off, sustain-off, and next-wrap range adoption.
3. Power the disting NT completely off, then on. Load the saved preset.
4. Before sending clock, confirm the saved parameters, selection brackets/length, zoom, and scroll returned. Confirm no MIDI is emitted merely by loading.
5. Send one fresh clock pulse and confirm there is still no resumed output. Send the second pulse to re-establish the interval. Continue clocking at the same rate.
6. Compare the post-load stream with the pre-save record. Confirm all of the following, not merely that a loop is audible:
   - retained MIDI bytes/events are present in the same oldest-to-newest order;
   - relative event placement within clock intervals matches the saved performance;
   - playback continues from the saved unconsumed cursor rather than fabricating an attack or restarting;
   - the pending note and sustain endings occur in the expected order and timing;
   - the pending edited range is adopted at the next wrap, with cleanup before its first event; and
   - the resulting loop boundaries, transport behavior, routing, filters, timeline state, and subsequent stop/reset cleanup match the pre-save state.

This comparison covers the implementation's retained-event data and timing; selected, active, and pending ranges; transport intent; playback cursor and scheduler; note, sustain, and pending-ending ownership; all host parameters; and timeline view/adjustment state. Live clock/reset levels, partial clock acquisition, current button holds, external routing, and connected instrument state are intentionally not preset data.

## Result record

Complete this section without changing the procedure after observing a result.

- Date:
- Tester:
- Exact firmware version:
- ARM object SHA-256 confirmed:
- Load/run result:
- Basic capture and patched-clock loop result:
- Physical pots/encoders result:
- Stop/reset note and sustain cleanup result:
- Preset save, full power cycle, and load result:
- Saved event contents/order/timing comparison:
- Complete saved-state and continuation comparison, including pending state:
- Overall result: **PASS / FAIL**
- Deviations, failures, or notes:

A **PASS** requires every section above to pass on the recorded supported firmware. A failure is release-gate evidence to diagnose; it must not be converted into a pass by relying on native tests.
