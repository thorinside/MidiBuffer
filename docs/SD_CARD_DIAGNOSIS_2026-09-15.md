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
