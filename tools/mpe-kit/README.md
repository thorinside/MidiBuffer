# MidiBuffer Mac MPE kit — preparation only

Actual-device result: **NOT ATTEMPTED**. No controller or Vital is required. USB Mac↔NT is the only later physical MIDI connection. This kit changes no device or system routing. Preparation exercises production callbacks on the Mac and private virtual CoreMIDI endpoints; it cannot establish firmware or hardware behavior.

The kit originated on branch `tools/mpe-kit-20261119-4fa6eb36` at `bab34a8` and is now included with the reviewed MidiBuffer implementation. Run the copy from the same source checkout you intend to verify, not a stale preparation checkout. The pinned SDK is `5a4910d1d4233180114d6aee5ddaa4b8aec577e8`; the self-test validates it. Record the tested source HEAD and dirty state with each trial.

## Safe first commands

Run in a foreground terminal. The existing environment is used read-only. No install is needed: Python 3.12, mido 1.3.3 and python-rtmidi 1.5.8 were observed at preparation. `clang++` and the existing pinned SDK are needed only for self-test.

```sh
cd /path/to/MidiBuffer
PY=/Users/nealsanche/.venvs/ntpush/bin/python
"$PY" -c 'import mido, rtmidi; print(mido.__file__, rtmidi.__file__)'
"$PY" tools/mpe-kit/kit.py list-ports
mkdir -p build/my-first-mpe-test
"$PY" tools/mpe-kit/kit.py dry-run --log build/my-first-mpe-test/expected.json
"$PY" tools/mpe-kit/selftest.py --sdk "$PWD/distingNT_API"
```

An aborted take and its cleanup invalidate the strict fresh-instance assumption. Clear Recording does **not** reset the implementation's observed input context; retries on that instance can correctly emit known context before music. Keep the original strict FAIL. A context-aware trial needs a separately declared exact context envelope and adequate capture window, not prefix stripping or a widened tolerance. Any physical remove/add, preset operation or new trial still requires explicit owner authorization.

Use a fresh evidence directory for each trial; existing logs/reports are never overwritten. The self-test uses random self-created virtual endpoints, opens only those identities through an allowlist, then disposes them. It compiles a small fixture against the existing production host-double seam, with read-only SDK access. It runs foreground for about ten seconds. It prints its unique `build/mpe-kit-...` evidence path. No DAW, IAC or audio setup is needed.

## Phrase and configuration policy

The deterministic phrase lasts 2.65 seconds: 22 channel messages, channels **2 and 3** (wire nibbles **1 and 2**), lower manager channel **1**, two lower members, upper zone disabled. Notes 57 and 64 overlap from 0.75 to 2.00 seconds; endings are at 2.00 and 2.25. Each member gets independent bend, Channel Pressure and CC74 before and during its note. Both members finish with bend 8192, pressure 0, CC74 64.

Logs use MIDI 1 channel numbers 1–16. `pitch14 = LSB + 128*MSB` in 0–16383, centre 8192; mido's signed pitch equals pitch14−8192. Initial bends are 9216 and 7168, later 10240 and 6144. At a manually matched ±48-semitone receiver range these mean +6/−6 and +12/−12 semitones. Payload comparison does not depend on an audible receiver or range.

**Manager setup is separate from expression.** The generator sends no MCM (RPN 0/6), bend-range setup (RPN 0/0), transport or SysEx. All 22 phrase messages, including pre-note expression and final neutral values, are music. Emergency generator cleanup is separately labeled `cleanup`, restricted to attempted owned notes and touched non-neutral expression on channels 2/3; there is no panic/autoreset or all-channel cleanup. Normal completion needs no extra cleanup. Capture has no output handle and sends nothing, including on failure.

The first topology uses NT **MPE Setup Manual**, **Lower Members 2**, **Upper Members 0**. A fresh candidate instance has unknown bend ranges; Manual declares the zone but does not invent ranges or emit an MCM. There is no manual bend-range parameter in the current MidiBuffer UI. Manual settings alone do not prove automatic MCM/RPN interoperability. If a later audible receiver is used, its range must be matched separately. Do not send setup via another tool under preparation authority.

## Later owner-only physical sequence

These commands are a recipe for Neal after explicitly authorizing a physical test. They were **not run on a physical output**. The selected NT USB endpoint must be independently identified from fresh inventory. Names in commands below are placeholders: copy the complete exact names and integer UIDs from `list-ports`. Each direction has its own UID; never select a generic H12, Network or IAC port by resemblance or by index. The preparation inventory contained `Network NT Session Test` and IAC/H12 ports but **no NT USB endpoint**. Always re-identify both directions from a fresh inventory after the owner connects the USB cable (UIDs are not stable across reconnects); a Network-named NT session is not the USB endpoint.

1. Connect the working USB Mac↔NT cable. Close MIDI monitors/DAWs that forward input, disable MIDI-thru/echo paths for this test, and keep IAC/Network routes out of the topology. Capture receives **NT→Mac** only; send targets **Mac→NT** only. Run send and replay capture in separate phases. Direct monitored performance must never enter the replay log.
2. **Prerequisites:** the owner must already have an explicitly identified compatible MPE candidate installed and an empty dedicated MidiBuffer instance. Published v0.5.0 alone does not establish candidate MPE support. Candidate loading, backup/restoration, installation, preset creation and parameter changes are owner actions outside preparation; this kit silently loads nothing. Follow the main README's loading handoff separately if necessary. Never clear an existing performance just to try this kit.
3. On the dedicated fresh instance, owner sets **Playback Off**, **MPE Mode On**, **MPE Setup** = **Manual** (index 13 of the product's own list, shown with a hardware-prepended Bypass at index 0), **Lower Members 2**, **Upper Members 0**, **Record Ch Omni**, **Play Ch Original**, **Filter CC Off**, **Filter Pitch Bend Off**, **Filter Aftertouch Off**, **MIDI Out USB**. A previously observed conflicting zone blocks Manual entry: use the intended empty setup rather than hiding the warning. Keep other MIDI sources quiet.
4. Owner supplies a stable routed **CV Clock** bus (>1 V rising edges), 0.5 seconds per pulse throughout record and replay; allow at least two acquisition pulses. **Reset** must use a quiet distinct bus. Set **Pulses/Beat 1** for a straightforward pulse display. It changes display conversion only. MidiBuffer ignores MIDI Clock and MIDI transport. An already configured onboard NT Clock may provide separate clock/reset buses; external cabling is optional for that owner-configured topology. If that clock topology is absent, it is a prerequisite for the owner, not a kit automation step. Historical clock playbook `62605411-eb27-49ac-b31d-e1880ace7b86` documents it; do not execute its device-write steps during preparation.
5. Set **Capture Start Capture**. Send once using the exact **output** identity. The command takes about 2.65 seconds and writes every attempt, confirmed send and cleanup separately. Stop if it returns nonzero; retain the log and inspect cleanup errors before another take. A nonzero send whose error is `sender missed event deadline by more than 25ms` is the fail-closed host-scheduling abort (a single OS stall of roughly 20 ms), not device evidence: run the take again in a quiet foreground session. Do not widen the tolerance or re-time the phrase to work around it.

```sh
"$PY" tools/mpe-kit/kit.py send \
  --output 'EXACT NT USB OUTPUT NAME' --uid OUTPUT_UID \
  --confirm I_AUTHORIZE_PHYSICAL_MIDI_TEST \
  --log build/my-first-mpe-test/sent.jsonl
```

6. Owner sets **Capture Stop Capture**. Select a pulse-aligned range containing the **whole** take, including the expression before the first note and neutral expression after the last note. Production playback starts at the selection start and wraps at the range end, so the operative requirement is that the range end be **at least 2.65 s (six 0.5-second pulses) after the first phrase event**; preferring full captured history satisfies it on this empty instance. Left/centre pots adjust Start/End; the right pot moves the range and hold+turn zooms. Do not trim to the visible note stems: expression precedes them. The clock continues at the same interval. Incomplete initial-context warnings are expected for genuinely unknown prehistory, not permission to discard expression.
7. Arm replay capture in the terminal **before** turning NT Playback On. It prints `ARMED`; then owner turns **Playback On** (or presses the left encoder). Capture waits at most 15 seconds and closes 2.8 seconds after the first incoming message, before a six-pulse-or-longer loop repeats. The entire first pass must fit this window. Start playback promptly. Immediately after capture finishes, owner sets **Playback Off**. Capture never stops the device itself.

```sh
"$PY" tools/mpe-kit/kit.py capture \
  --input 'EXACT NT USB INPUT NAME' --uid INPUT_UID \
  --seconds 15 --active-seconds 2.8 \
  --log build/my-first-mpe-test/replay.jsonl
"$PY" tools/mpe-kit/kit.py compare \
  --expected build/my-first-mpe-test/sent.jsonl \
  --actual build/my-first-mpe-test/replay.jsonl \
  --require-replay \
  --report build/my-first-mpe-test/report.json
```

8. Read the report. **PASS** means exact gate/expression count, channel, values and order plus timing within the stated tolerance for this log pair. Every report now records `provenance` (`expectedKind`/`actualKind` and both endpoints). With `--require-replay` (as in the owner command above) the CLI refuses to certify an actual log whose `kind` is not `replay`; a same-file or sent-as-actual pairing therefore fails instead of reading PASS. Without that flag it is a generic structural comparison, not replay certification, even when it reads PASS. It establishes only this trial on the identified device/configuration. **FAIL** reports index, type/channel and expected/actual payload, or timing error; nonzero exit is intentional. Missing notes/expression suggest range/filter/channel/capture settings; duplicated events suggest monitoring/feedback/repeat capture; a missing prefix suggests selection began too late. Extra setup/context is retained and fails this strict first topology. Diagnose from raw logs and actual settings; never delete mismatches to manufacture PASS. For another supported setup, prepare an explicit separately reviewed envelope oracle before comparison.

## Evidence, timing and limits

Expected JSON is generated offline. Sent and received journals contain timestamps, raw bytes, semantic type/channel/data, phase, count, completion status and endpoint. Reports are separate. Each journal ends with a final record; a missing final, count mismatch or count beyond 4096 fails. Writes use exclusive creation; journals flush each event. Abrupt process death/SIGKILL or power loss can leave an incomplete journal and cannot guarantee note cleanup. SIGINT/SIGTERM, send errors, input faults and disconnects close handles and report incomplete cleanup; a disconnected endpoint cannot be silenced by software.

Nothing is excluded in the default comparator: `exclusions: []`. Real-time/system messages, setup, entry/wrap context and cleanup remain visible and will fail as extra events unless explicitly present in the expected sequence. Pressure, pitch bend and CC74 are never broadly dropped. Raw `9n note 0` and `8n note releaseVelocity` normalize to the same note-ending gate; release velocity is retained raw but not asserted semantically because this kit tests gate ownership. Note-on attack velocity stays exact. Other values and event order stay exact.

Timing is assessed independently. One global offset aligns **first event to first event**; every later relative time is compared without per-event alignment or fitting. Default tolerance is ±25 ms for host MIDI receipt/scheduling, not exact sample/byte hardware timing. The production fixture has simulated 48 kHz samples, 0.5-second CV pulses and four-sample host steps, with ≤1-sample replay differences. Actual NT MIDI callback timing is unstamped, clock edges are sample-scanned, offsets scale by a running average of up to eight CV pulse intervals (main README “Timing method”). The 25 ms host budget is intentionally much larger than native quantization and still catches the provided +50 ms control. It is a practical first USB trial budget, not a measured hardware guarantee. A timing-only failure keeps semantic PASS visible. Slow host scheduling can cause a real failure; the sender uses short sleeps against one deadline to avoid large macOS timer coalescing.

Keep tempo steady for the first test. For a deliberate fixed tempo change, `--tempo-scale CAPTURE_INTERVAL/RECORD_INTERVAL` scales expected relative times uniformly (0.25–4); disclose the chosen ratio. It is never estimated from received events. Adjust `--active-seconds` to cover the scaled phrase before the next wrap, and `--seconds` for the arm deadline (maximum 30 seconds). Changing tempo mid-phrase or clock jitter requires an interval-aware oracle; increasing tolerance does not prove the predictor. The sender is bounded to 10 seconds/4096 events and ≥5 ms requested spacing, and the fixed phrase is much smaller. An event more than 25 ms late aborts the take with scoped cleanup; actual music dispatch is also capped at 200 events/second to prevent catch-up bursts. Safety cleanup remains immediate and labeled. Input callbacks use a bounded buffer; overflow or an oversized/invalid packet marks capture incomplete. No listening input is ever forwarded to an output.

## Optional Vital later

Vital is unnecessary for this first test. Existing `make vital-receiver-compare` uses the installed `/Library/Audio/Plug-Ins/Components/Vital.component` (previously observed 1.5.5), private CoreMIDI endpoints, offline AudioUnit rendering and numpy; this kit neither installs nor invokes it. `tests/vital_receiver_render.cpp` contains the existing isolated bridge, and `tests/receiver_fixture.cpp` supplies native production traces. Prior receiver evidence `1e444eef-6aba-49b4-b5d5-80917a1e3af7` found channel-local bend/pressure/CC74 behavior, ignored MCM/RPN0, ignored manager bend/sustain, and unsupported automatic upper/dual-zone semantics. A manually matched patch range proves only matched configuration. Recheck the installed version before any later audible test; do not infer automatic MPE interoperability or physical audio from this kit.
