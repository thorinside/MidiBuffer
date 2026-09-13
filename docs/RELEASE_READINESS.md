# Release readiness audit

- Audit date: 2026-09-13 UTC
- Approved Spec SHA-256: `921f5735e68d9667ac287438ce2959e4691e2acba70b307c62be54e666b23e62`
- Integrated candidate before this audit: `52e2bcf2a552dd24c6ff178f38702c8edf4a93df`
- Implementation/test/build source tip: `fe0efafc5e85598677b0f1e40adc59cbedb11c67`
- Pinned SDK: `5a4910d1d4233180114d6aee5ddaa4b8aec577e8` (`v1.18.0-1-g5a4910d`, API v13)

## Decision

**Automated delivery is complete, but the candidate is not ready for public release until the owner records a passing physical smoke test.**

The complete native callback suite, sanitizer run, focused static analysis, strict ARM build, and object inspection pass. The fresh ARM object is byte-identical to the object named in [`HARDWARE_SMOKE_TEST.md`](HARDWARE_SMOKE_TEST.md):

```text
6b44cb2644f9168f356e4920cfcd4c913f9f8cc0009b944ffae50dc6bf95190e  plugins/MidiBuffer.o
```

No physical result is recorded. Therefore AC-001, AC-044, and AC-048 retain their prior partial status and owner-evidence boundary. A native/emulated result has not been substituted for those observations.

## Verification performed

- `make clean && make verify` passed the complete production callback/host-double suite, rebuilt the ARM product with strict warnings, `-fPIC`, no exceptions, and no RTTI, and passed the Makefile object-inspection gate.
- An independent Clang ASan/UBSan build of the same native sources and complete callback suite passed.
- Focused Cppcheck `warning`, `performance`, and `portability` analysis passed with the project/API macros supplied. Cppcheck's non-gating style profile reports const-suggestion diagnostics and cannot parse the API's `ARRAY_SIZE` macro without that explicit definition; neither is a runtime or build defect.
- `arm-none-eabi-readelf -h plugins/MidiBuffer.o` identifies ELF32, little-endian, ARM, EABI5, relocatable (`REL`).
- `arm-none-eabi-nm` confirms global `pluginEntry` and only the understood NT host, NT JSON, GOT, and `memset` undefined symbols.
- `arm-none-eabi-size -A` reports 14,128 bytes across object sections. This is an observation, not a timeless firmware admission limit.
- The rebuilt object SHA-256 is the exact digest already frozen into the physical test procedure.
- Repository-wide release-surface searches found no direct MIDI-file/filesystem export implementation and no revival of retired AC-046.
- README, hardware procedure, API selector, pinned SDK, and build settings agree on API v13 and the firmware 1.18+ support target. They label actual physical firmware testing as pending rather than claiming it occurred.

The native suite reported zero scheduler-model sample error for the deterministic 16- and 24-sample steady-interval traces. This remains native scheduler evidence, not physical end-to-end latency evidence. The full-5 MB valid-preset round trip also passed; its approximately 10.1 MB host-double JSON payload remains a measurement rather than a firmware guarantee.

## Requirement audit

Every approved requirement is represented in the integrated candidate and its retained evidence:

| Requirements | Integrated evidence | Result |
| --- | --- | --- |
| REQ-001–005 | API v13 factory/callback boundary; independent clock/reset scanning; clocked proportional scheduling; loss/reset cleanup and continuation traces | Automated checks pass; physical load/run remains owner evidence under AC-001/048 |
| REQ-006 | No export API, command, control, implementation, or promise; README explicitly excludes direct MIDI-file export | Pass |
| REQ-007–011 | Clock-gated capture; complete five-destination matrix; eligible MIDI vocabulary; grouped filters; Omni/1–16 capture and Original/1–16 playback channel matrices | Pass |
| REQ-012–016 | Multi-pass note/sustain ownership, same-pitch retrigger cancellation, capture-stop endings, selection-start exclusion, and pulse-boundary traces | Pass |
| REQ-017–021 | All seven pulse/beat values; expressive defaults; timeline framebuffer/control traces; atomic range adoption; stop cleanup and resume traces | Pass |
| REQ-022–023 | Valid-preset serialization and exhaustive state/continuation round trips; corrupt-preset recovery remains excluded and retired AC-046 stays absent | Automated valid-preset checks pass; physical power-cycle observation remains owner evidence under AC-044/048 |
| REQ-024–026 | Player guide targets other musicians, states 1.18+ minimum honestly, and supplies the modest post-build procedure tied to the rebuilt digest | Documentation/build identity pass; REQ-026 physical execution remains pending |
| REQ-027–031 | Rolling-selection invalidation, exact loss threshold, two-pulse reacquisition, eight-interval prediction, pulse catch-up ordering, and stopped-clock acquisition traces | Pass |
| REQ-032–036 | Channel Mode/RPN exclusions; all-channel selected-destination panic matrix; stopped transport after panic; encoder controls; unified default-off aftertouch filter | Pass |
| REQ-037 | Clamped 1–5 decimal-MB pre-instantiation DRAM request, fixed runtime capacity, oldest replacement, density variation, and full-capacity tests | Pass |

No non-deferred requirement, state category, or active acceptance criterion is omitted. Publication, launch, licensing/distribution logistics, corrupt-preset recovery, and an expanded hardware programme remain outside this audit.

## Active acceptance-criterion reconciliation

The following groups enumerate all 66 active criteria. AC-046 is retired and intentionally absent.

| Criteria | Current audit result | Evidence seam |
| --- | --- | --- |
| AC-001 | **Partial — owner evidence pending** | ARM product/entry point are proven; physical supported-firmware load/run is not recorded |
| AC-002–013 | Met | Playback/capture exclusion, clock/reset, proportional timing, loss/reacquisition, and ordered cleanup callback traces |
| AC-014 | Met | No direct MIDI-file export surface; explicit version-one documentation |
| AC-015–023 | Met | Clocked capture, destination/channel matrices, vocabulary exclusions, and recoverable filter matrix |
| AC-024–030 | Met | Loop-tail note/sustain ownership, capture endings, selection-start exclusion, and retrigger traces |
| AC-031–043 | Met | Pulse-aligned range, rolling history/timeline controls, atomic range changes, stop/reset cleanup, and resume traces |
| AC-044 | **Partial — owner evidence pending** | Exact native valid-preset round trips pass; prescribed physical save/power-cycle/load comparison is not recorded |
| AC-045 | Met | Exhaustive complete-state serialization equality and in-flight continuation at 1–5 MB, including full 5 MB |
| AC-047 | Met | README and hardware procedure consistently require firmware 1.18+ and mark earlier firmware unsupported |
| AC-048 | **Partial — owner evidence pending** | Exact modest procedure and matching build exist; no physical result is recorded |
| AC-049–067 | Met | Sustain ownership; overwrite invalidation; loss/acquisition/prediction; panic; transport controls; aftertouch; capacity; pulse/beat matrices |

## Remaining owner gate

Run every section of [`HARDWARE_SMOKE_TEST.md`](HARDWARE_SMOKE_TEST.md) unchanged on one physical disting NT running firmware 1.18 or later, using the object digest above. Record the exact firmware, load/run, capture and clocked looping, physical controls, stop/reset cleanup, and complete preset save/power-cycle/load comparison. Do not publish if any section fails or if the object digest differs.
