# MidiBuffer preset and SD-card investigation

The latest MidiBuffer preset found on the supplied card is
`/Volumes/DISTING NT/presets/MBclk_A1.json` (117,303 bytes), saved from
firmware `v1.19.0beta`, build `Sep 11 2026 16:20:38`.

## Confirmed card corruption

`diskutil verifyVolume '/Volumes/DISTING NT'` ran the read-only
`fsck_exfat -n -x /dev/rdisk13s1` check and reported:

```text
Directory / contains a duplicate name (programs)
The bitmap needs to be repaired
The volume DISTING NT was found corrupt and needs to be repaired
File system check exit code is 203
```

Verification temporarily unmounted the card and restored its original mounted
state. No repair was performed. The card was mounted read-only and reported
media read-only. Directory enumeration listed two identical `programs` names;
opening that path resolved to an empty directory. The installed MidiBuffer
object therefore could not be inspected or compared with the current source.
Filesystem corruption is a potential cause of loading/file-access failure;
this investigation did not reproduce or establish the cause of the hardware
runtime hang. Back up readable card contents before filesystem repair or
replacement.

## Saved recording state

- Buffer specification: 1 MB, capacity 41,666 events of 24 bytes each.
- Retained recording: 663 events, 331 note-ons and 332 note-offs, all channel 3.
- Events are ordered by pulse, spanning pulses 64,825–65,097.
- Capture is Off; Playback and Clear Recording are On.
- MidiBuffer Clock is bus 63 and Reset is bus 64.

Clear Recording is a latched Off-to-On command. Leaving it On does not clear
again, and loading a saved On does not erase the recording. Clear preserves
the Capture setting: with this preset, capture must be explicitly enabled
after clearing. Buffer capacity is selected at instantiation and does not grow
after clear; at capacity the recording ring overwrites its oldest events.

## Native verification and limits

The existing native contract executable passed. A probe reconstructed the
actual preset's custom JSON state and generic parameters, invoked the
production deserialise/draw/parameter/MIDI/step callbacks, and ran with
ASan/UBSan against source revision `a0d2013`:

```text
PASS actual preset loads
PASS draw actual preset
PASS actual preset clock/playback callbacks
after rollover: events=41666 capacity=41666 oldest=125925 newest=167590
PASS clear after rollover accepts new recording
```

The probe supplies fresh clock edges, isolates Reset, clears with Off then On,
enables Capture, sends 100,000 alternating channel-3 note messages, verifies
rollover, then clears again and verifies a newly accepted note. This is native
evidence; it does not execute the firmware loader, built-in Clock algorithm,
physical MIDI output, or SD-card I/O. No plugin source change was justified by
these results. Hardware follow-up needs the exact hang trigger (preset load,
playback, capture, clear, or save) and a readable installed object.

The original preset, probe source, and results are preserved at
`/Users/nealsanche/nosuch/midibuffer-diagnostics/2026-09-15/`.

## Hardware trigger clarified

The user reports startup stops at `loading plugin:midibuffer.o`, with the
module configured to load its last preset. The official 1.18 manual (Startup
and Plug-ins sections) confirms that preset loading automatically loads any
required plugins. This makes automatic restoration a plausible trigger for
the plugin load, but does not establish that the saved recording state causes
the failure. The screen message alone does not identify the failing loader
operation or callback.

The current workspace ARM object has 20,322 bytes of sections, no `.bss`, and
no static recording buffer. Its entry point only reports API v13 and factory
metadata; recording allocation happens separately at instance construction.
This does not validate the unreadable installed object.

A useful hardware isolation step is to power off, remove the card, boot, and
disable Settings / Startup / Load last preset if the menu is accessible. Then
power off, reinsert the card, and boot again. If startup succeeds, manually
adding MidiBuffer without restoring the preset distinguishes a plugin-load
failure from a preset-restoration failure. No card modification is required
for this isolation step. Reset preset is an alternative documented way to
forget the last preset, but disabling automatic load preserves that reference.

Source: https://www.expert-sleepers.co.uk/downloads/manuals/disting_NT_user_manual_1.18.pdf

## Authorized repair and recovered object

After the user supplied a writable mount and explicitly approved repair,
`diskutil repairVolume '/Volumes/DISTING NT'` completed successfully with
fsck exit code 0. A separate `diskutil verifyVolume` then reported
`The volume DISTING NT appears to be OK`, also with exit code 0. Both commands
restored the mounted state after their checks.

The filesystem repair renamed the empty duplicate directory to `programs-1`.
The original `programs` directory is now accessible, with `plug-ins` and `lua`
subdirectories and 105 files. No files were manually removed or replaced.
All 543 readable preset/configuration files backed up before repair matched
their SHA-256 manifest afterward; none were missing or changed. This comparison
does not establish the integrity of all samples or files inaccessible before
repair.

The recovered `programs/plug-ins/MidiBuffer.o` is 54,300 bytes and exactly
matches the workspace object, with SHA-256:

```text
d15ab42d2ebd9f53b6c7eadc81f52a4edffffc0063038df973ea7bf90b6967a2
```

The recovered object is ELF32, little-endian, relocatable ARM, with the expected
host API/JSON and memset imports. A backup is stored under
`/Users/nealsanche/nosuch/midibuffer-diagnostics/2026-09-15/recovered/`.
There is no evidence requiring replacement of the installed object.
The next acceptance check is physical startup with the repaired card;
filesystem repair success does not yet establish that the reported hang is
resolved.

## Hang persists on explicit preset load

The user confirms that disabling Load last preset permits boot, but explicitly
loading MBclk_A1 still hangs. Filesystem repair therefore did not resolve the
reported preset-load failure. Successful boot without preset restoration does
not yet demonstrate that manually instantiating MidiBuffer succeeds.

ARM disassembly shows the deserialise callback reserves 252 bytes of local
stack plus saved registers; construction has no large local stack allocation.
This does not reveal a large plugin restoration stack frame. The native host
still cannot reproduce the hardware failure.

Two diagnostic copies are preserved in the external diagnostics directory:

- MBdiag_Clock.json contains only the original built-in Clock slot.
- MBdiag_Stop.json retains all original recorded events and selection, but
  restores Playback and Clear Off, stopped transport, closed scheduling, and
  empty playback-output/pending-ending ownership.

The stopped copy loads through production callbacks under ASan/UBSan, accepts
100,000 events with rollover, and accepts fresh recording after clear. It is a
diagnostic comparison, not a confirmed fix. Neither original preset nor plugin
was modified. At this follow-up the card was no longer mounted, no disting NT
USB MIDI port was available, and the previous local MCP endpoint was not
running, so hardware comparison could not be driven unattended.

Next distinguish manual MidiBuffer addition to an empty preset from loading
saved state. If manual addition works, compare the stopped diagnostic copy
with the original on hardware before attributing the failure to a callback,
serialization field, or firmware behavior.

## Physical isolation results

The user confirms fresh manual MidiBuffer instantiation succeeds. The
MBdiag_Stop copy was installed beside the original, read back and verified,
then the card was safely ejected. The user confirms this stopped diagnostic
preset loads on hardware. Its recorded event payload is unchanged, so the
recorded notes alone are insufficient to reproduce the hang. The differences
still include Playback/Clear values and active scheduler/output ownership;
none of these can yet be identified individually as the cause.

Two further local diagnostic copies are ready for hardware comparison:

- MBdiag_ClearOff: original preset with only Clear Off in generic parameters
  and sharedControls, plus its diagnostic preset name. Playback and all saved
  scheduling/output/recording data remain as in the failing original.
- MBdiag_ClearOn: working stopped diagnostic with only Clear On restored in
  generic parameters and sharedControls, plus its diagnostic name.

The card was not mounted when these copies were prepared. They have not yet
been installed or tested on hardware. Keep automatic last-preset loading
disabled while comparing them.

## Clear restoration defect and loading-delay correction

The user subsequently reports ClearOff loads and ClearOn initially appears
to hang, then clarifies ClearOn eventually loaded. There is no measured loading
time and no confirmed permanent hang for that diagnostic. The original active
preset's eventual outcome remains unknown.

Code inspection found that Clear is initially armed at construction, while
its consumed preset latch is only restored in deserialise. Generic parameters
arriving first therefore execute a live clear when Clear On is restored.
The old native tests masked that action because deserialise resets the clear
diagnostics. A new host trace observes clear transitions before that reset;
the shared-control restoration test failed before the fix:

```text
FAIL: restoring Clear On must not execute a live clear before custom state is loaded
```

The plugin now distinguishes initialization from running controls using a
non-persisted processingStarted flag. Until the first valid audio block,
Clear callbacks only initialize its armed/consumed latch; they do not clear
history, stop transport, or invoke a Playback setter. Once processing starts,
the existing synchronous live Off-to-On clear behavior is unchanged. Parameter
indices and preset representation remain unchanged.

The regression and all existing native, ASan/UBSan, static/realtime,
traceability, and ARM object-inspection checks pass with this fix. This proves
the unwanted startup action is removed, not that the hardware loading delay
is resolved. No patched plugin has yet been installed on the module/card.

Validation command used the direct CommandLineTools make and linker paths
because the default Apple shims are blocked by the unaccepted Xcode license:

```sh
/Library/Developer/CommandLineTools/usr/bin/make verify \
  NATIVE_CXX='/opt/homebrew/opt/llvm/bin/clang++ -B/Library/Developer/CommandLineTools/usr/bin -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'
```

## Recording beyond a previous cleared length

Source tracing found no admission/stop condition comparing incoming MIDI
against the previous recording end or selected loop end. Clear invalidates the
event head/count and history end, clears selection and output ownership, and
preserves the actual Capture and clock state. The pulse counter remains an
absolute clock identity; it is not a maximum recording duration. Incoming
events update the history end, and a full ring replaces its oldest events
instead of refusing new events.

The added production-callback regression
`verifyRecordingBeyondPreviousLengthAfterClear` exercises all five buffer
sizes. It records 32 pulses, installs a loop selection and a narrow/scrolled
manual viewport, clears while Capture is enabled, and records 104 fresh
pulses. It checks that the new history exceeds three times the old duration
and crosses the old absolute endpoint. It also repeats Clear On at the former
duration boundary without a new Off-to-On gesture, fills and wraps the real
event ring, clears the wrapped full ring, fills/wraps again, and verifies a
distinctive newly admitted MIDI note at the newest clock pulse.

The focused native test passes. This has not reproduced the user's reported
hardware stop and does not prove actual hardware capture remains active.
Clear preserves manual zoom and scroll, so a narrow timeline view can retain
its width while history grows. Starting playback or panic stops actual
capture, and clearing such a paused instance does not restart capture. The
user was asked whether Avail, timeline notes, or replayed MIDI stops changing
to distinguish these cases before making any production change for this
symptom.

## Reproduced display saturation and fix

The user clarifies that the observation was new note marks no longer
appearing on the display; Avail/replayed MIDI was not established to stop.
The renderer iterated oldest-first and stopped after drawing 256 visible
note-ons. Once that budget was occupied by old notes, newly captured notes
were never drawn. The original preset contains 331 note-ons, enough to
trigger this saturation in a view covering the whole recording.

The new `verifyNewestCapturedNotesRemainVisible` regression records 300
ordinary note attacks and then a distinctive high note through real MIDI and
clock callbacks. It verifies the distinctive note is stored, and separately
checks for its actual draw call. Before the rendering fix the drawing
assertion failed twice, once with fresh history and once after clear, while
the recording assertion passed. The renderer now walks newest-first and
draws the newest 256 visible attacks. It preserves viewport bounds and the
draw-call budget; dense views may omit older marks rather than newly arriving
notes. Recording and replay contents are untouched.

The regression and complete native, sanitizer, static/realtime, traceability,
and ARM gate pass. Player documentation now explains the dense-view drawing
limit. The patched object includes both this display correction and the
previous startup Clear guard; physical installation and acceptance remain
pending because the card is not mounted.

## Complete visible-range representation

The user prefers representation of all notes across the visible range rather
than retaining only the latest 256 marks, and reports saturation around four
bars at 16 PPQN. Four bars of four beats at 16 pulses per beat contain 256
pulses; one attack per pulse would fill the old 256-note drawing budget at
that point. This is consistent with the display defect, not evidence of a
four-bar capture limit.

The renderer now scans every visible note-on and merges overlapping vertical
stems into 248 screen-column bins. Each bin retains the topmost stem endpoint;
drawing it produces the exact union of all individual note stems in that
column. Thus every visible attack contributes, including both earliest and
latest notes, with at most 248 drawing calls and 248 bytes of fixed local
aggregation storage. There is no note-count cutoff or event sampling. Zoom
continues to separate events that overlap at a wider time scale.

The dense-history regression now uses 16 PPQN and distinctive earliest/latest
notes, before and after clear, and compares every rendered column with an
independent raster union calculated from all actual retained note events.
The earlier newest-only renderer fails these requirements. This change does
not alter recording, playback timing, parameter indices, or preset encoding.
