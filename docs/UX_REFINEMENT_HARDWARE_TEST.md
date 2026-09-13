# Post-build UX refinement hardware test

## Status

**Owner physical evidence pending.** This procedure validates only the approved post-build navigation/display refinement. It is separate from the original v1 release gate in [`HARDWARE_SMOKE_TEST.md`](HARDWARE_SMOKE_TEST.md), and completing it does not alter or replace any v1 evidence.

## Build under test

- Product: `plugins/MidiBuffer.o`
- Approved add-on Spec SHA-256: `fe17d6067caf1f3066a1b173ad5bb3f0d120774a3631102f50273be1b6456dfc`
- ARM object SHA-256: `cdf5d035b2418daea44b30b6459d84835f4b32eee2f5a9d98ae2ced32dc66daa`
- Pinned SDK: `5a4910d1d4233180114d6aee5ddaa4b8aec577e8` (API v13)
- Supported firmware: disting NT firmware 1.18 or later

Confirm the object digest before copying it to `/programs/plug-ins/MidiBuffer.o`. Record the exact firmware actually used. Do not treat native host-double, framebuffer, arithmetic, or ARM object checks as physical evidence.

## Setup

Use one physical disting NT, steady Clock and separate Reset sources, and enough captured MIDI to create a range shorter than retained history. Include at least one held note and sustain passage. Use a range and retained history wide enough to see zoom and scroll changes clearly. Start from a newly added MidiBuffer instance so fresh Show All can also be observed.

## Procedure

### 1. Entry and ordinary range movement

1. Load the plug-in and confirm the new timeline initially shows all retained history as capture grows.
2. Create a valid selection with the left and centre pots. Record its displayed `Len`.
3. Turn the unpressed right pot both directions. Confirm the whole range moves older/newer, keeps the same `Len`, remains within retained history, and does not pan the viewport.
4. Move to each retained-history limit. Confirm movement clamps without wrapping, shortening, endpoint snapping, or accumulated outward debt; reverse direction and confirm the range responds immediately.

### 2. Held zoom and takeover

At low, middle, and high right-pot press positions, perform every direction physically available from that position.

1. Press the right pot without turning. Confirm neither selection nor view jumps.
2. While held, turn toward zoom-in and zoom-out where physical travel permits. Confirm held movement changes only the view, starts from the current view, and does not move selection or change the right encoder's prior Start/End/Range target.
3. Release once without movement and once while the pot value is changing. Confirm release does not move the range.
4. After each release, reverse and continue unpressed motion. Confirm the first delta is measured from the release position: there is no absolute remap, release snap, host pickup dead zone, second reconciliation jump, or endpoint snap.
5. Before catch-up has finished, repeat the press/held-turn/release cycle. Confirm the remaining mismatch is preserved and subsequent motion feels continuous. Record any discontinuity, stall, or direction error as a failure.
6. Zoom fully out and confirm Show All resumes following retained-history growth/eviction. Zoom in, scroll manually, and confirm a manual view does not silently become Show All merely because retained history later clips it.

### 3. Established gestures and playback

1. After effective left-pot, centre-pot, and unpressed-right-pot movement, turn the right encoder by one detent each way. Confirm it adjusts Start only, End only, or both Range boundaries respectively by exactly one pulse. Confirm held zoom and left-encoder scroll retain the prior target.
2. Turn the left encoder quickly and slowly. Confirm one-pulse-per-detent manual scrolling with no acceleration; confirm it is a no-op in Show All.
3. Press and hold the left encoder. Confirm playback toggles only on the rising edge and does not toggle repeatedly while held; an invalid selection remains a no-op.
4. Hold the right encoder for less than one second, then for at least one second while also rotating an encoder and while holding pot 3. Confirm panic never fires early, fires once per hold at/after one second, and rearms only after release.
5. During playback, edit the complete range more than once. Confirm the latest pair remains pending until wrap, then adopts together after established cleanup; editing back to the active pair cancels the pending transition.
6. Confirm Reset, stop, clock loss/reacquisition, cleanup ordering, and playback continuation still match the established behavior.

### 4. Physical display

1. With ordinary retained history and a valid selection, verify `Avail` remains at the original left origin and `Len` at the original right origin. Confirm both are readable as `bars:beats:ticks`, colons are visible, ticks have exactly three digits, and all tiny-glyph ink remains inside the top eight-pixel band and its left/right field.
2. Set Pulses/Beat to 48 and repeat with a valid and invalid selection. Confirm invalid length reads `Len --`; no decimal `b` suffix, ruler, history heading, or extra clock label appears.
3. During playback, confirm exactly one vertical head is visible only for an on-screen active pulse. Confirm selection brackets remain visually dominant at coincidence and their caps remain intact. Stop, arm, lose clock, and move the pulse offscreen; confirm no stale line, edge pin, arrow, or viewport following appears.
4. Judge readability at the normal viewing distance and lighting used for the instrument. Synthetic cell budgets and native framebuffer pixels do not answer this physical legibility check.

## Result record

Complete every field from direct observation without changing the procedure afterward.

- Date:
- Tester:
- Exact firmware version:
- ARM object SHA-256 confirmed: **yes / no**
- Load and fresh Show All result: **PASS / FAIL**
- Unpressed whole-range motion and clamps: **PASS / FAIL**
- Held zoom from low/middle/high press positions: **PASS / FAIL**
- Release-with-change and first-unpressed-delta takeover: **PASS / FAIL**
- Repeated-hold unfinished catch-up feel: **PASS / FAIL**
- Encoder Start/End/Range targeting and exact scroll: **PASS / FAIL**
- Playback, panic, reset, clock-loss, and pending-wrap gestures: **PASS / FAIL**
- Avail/Len readability and top-field containment: **PASS / FAIL**
- Playback-head visibility/layering/state behavior: **PASS / FAIL**
- Overall result: **PASS / FAIL**
- Deviations, failures, or notes:

Any failed step is evidence to diagnose, not a result that native or synthetic checks can override.
