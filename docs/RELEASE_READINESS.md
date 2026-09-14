# Release readiness audit

## MidiBuffer Three incremental traceability — 2026-09-14 UTC

- Approved MidiBuffer Three Spec SHA-256: `aefc6eaf7fc3736d253e6b929e2efed368d2fe9076d0bd9fd1f189a89ec5d05b`
- Implementation source tip reviewed: `a460387134ef225e6400d4feba962d2ea58e128d`
- Pinned SDK: `5a4910d1d4233180114d6aee5ddaa4b8aec577e8` (`v1.18.0-1-g5a4910d`, API v13)
- Rebuilt ARM object SHA-256: `d15ab42d2ebd9f53b6c7eadc81f52a4edffffc0063038df973ea7bf90b6967a2`

**Automated delivery for the increment is complete, but this is not a public-release approval.** The original physical gate and the post-build UX physical gate remain pending as separate owner observations. No native, sanitizer, framebuffer-emulator, source-review, or ARM-inspection result below is physical disting NT evidence.

### Inherited-contract checklist

| Contract | Production/test/document trace | Review result |
| --- | --- | --- |
| Firmware and host boundary | `README.md`, API-v13 parameter/factory adapters in `src/midibuffer.cpp`, pinned SDK, strict ARM build and object inspection | **Retained:** firmware 1.18 and later only; older firmware remains unsupported. The support range is not a hardware-test claim. |
| Local memory and valid presets | `src/midibuffer.cpp` serialization/reconciliation; preset images and 1–5 MB/full-5 MB round trips in `tests/callback_contract_test.cpp` | **Retained:** recorded MIDI stays in local rolling memory and ordinary NT presets. Parameters 0–9 keep their indices; Playback 10 and Clear Recording 11 are additive. Both restoration orders and legacy images pass without load-time output, erasure, or capture finalization. |
| Routing and filtering | Production destination/channel/filter adapters and complete output/filter matrices in `tests/callback_contract_test.cpp`; `README.md` routing warnings | **Retained:** five destinations, Original/1–16 playback channels, Omni/1–16 recording channels, approved event exclusions, expression filters, and safety-output bypass are unchanged. |
| Capture/playback rules | Production clock, selection, scheduler, cleanup, and capture-finalization paths; callback transport, ownership, cleanup, and draw-invariance traces | **Retained:** patched-clock acquisition/loss, capture/playback exclusion, pulse-relative playback, pending edits at wrap, ordered note/sustain cleanup, reset, panic, and explicit-only capture restart remain in force. Starting Playback pauses Capture; ordinary stopping does not restart it. Clear preserves the actual Capture state. |
| Pot 3, navigation, and playback head | `src/range_motion.hpp`, production UI/draw callbacks, `tests/range_motion_test.cpp`, and callback/framebuffer traces | **Retained:** unpressed pot-3 fixed-length range movement, held relative zoom/takeover, Show All/manual scrolling, encoder Start/End/Range targeting, and clock-gated playback-head visibility/layering. |
| Accepted post-build UX wording | The preserved historical report below and [`UX_REFINEMENT_VERIFICATION.md`](UX_REFINEMENT_VERIFICATION.md); current player wording in `README.md` | **Retained except only the explicit supersessions listed next.** Existing UX-AC identifiers and physical evidence boundary are unchanged. |

### Explicitly superseded behavior only

1. The left encoder's former invalid-selection no-op is superseded: it now toggles the shared host-managed **Playback** value. Playback may remain On without producing output, and a later valid selection starts through the inherited clock gate.
2. Start/End remain the left/centre-pot functions, but their former non-zoom-aware adjustment is superseded by pulse-aligned viewport-scaled movement. An offscreen actual boundary has a directional edge handle; the first deliberate associated-pot movement retrieves it to the nearest legal visible pulse without scrolling, then later movement uses the zoom scale.
3. The configured resolution suffix `ppb` is superseded by `PPQN`, for example `16 PPQN`. Duration conversion and clock timing are unchanged.
4. **Clear Recording** is an additive recording-page Boolean and the specified exception to ordinary unavailable-playback behavior: an armed Off-to-On erases once, preserves Capture, performs playback cleanup, and sets Playback Off. It has no faceplate shortcut; restored On is consumed and cannot erase until Off rearms it.

No other discovery or post-build UX behavior is superseded by this increment.

### Scope and evidence boundary

- Direct MIDI-file export, support before firmware 1.18, firmware-owned corrupt/incomplete-preset handling, publication, and an expanded physical-test programme remain excluded.
- The original AC-001, AC-044, and AC-048 procedure remains [`HARDWARE_SMOKE_TEST.md`](HARDWARE_SMOKE_TEST.md).
- UX-AC-002, UX-AC-008, and UX-AC-012 remain a separate unwaived gate under [`UX_REFINEMENT_HARDWARE_TEST.md`](UX_REFINEMENT_HARDWARE_TEST.md).
- The two procedures retain separate result records. Passing one does not satisfy the other, and no current automated evidence is relabelled as physical evidence.

### Increment verification

`make clean && make verify` is the integrated gate: it runs the complete optimized native callback/range-motion suites, the complete callback suite under ASan/UBSan, focused Cppcheck, a comment/string-aware production blocking-I/O and logging scan, the frozen-Spec traceability-manifest validator, both ARM builds, and ELF/export/undefined-symbol inspection. The strict ARM allowlist adds only the justified API v13 parameter synchronization imports (`NT_algorithmIndex`, `NT_parameterOffset`, `NT_setParameterFromAudio`, and `NT_setParameterFromUi`) beyond the inherited host/JSON/libc surface.

The clear regression fills the complete 5 MB event capacity, reaches every fixed output-ownership slot, and proves that Clear performs the same four history metadata operations as a small history. Cleanup is bounded at 2,048 note slots plus 16 sustain slots, remains ordered note-off then CC64-off, and allocates nothing. Production step, MIDI/realtime, parameter, UI/setup and draw paths are allocation-accounted; static inspection finds no blocking I/O or logging API in product source.

The candidate product is reported separately as an ELF32 little-endian ARM EABI5 relocatable object exporting `pluginEntry`. Native traces separately report zero deterministic scheduler-model sample error, an approximately 10.1 MB full-5 MB host-double JSON payload with save/load process-CPU times, and comparative draw process-CPU times; none is a firmware budget or physical guarantee. The exact requirement/criterion inventory and evidence seams are maintained in [`MIDIBUFFER_THREE_TRACEABILITY.json`](MIDIBUFFER_THREE_TRACEABILITY.json), using only the supplied registry keys and normative Spec identifiers.

Repository review found no MIDI-file/filesystem export implementation, no older-firmware compatibility path or claim, no plug-in-owned corrupt-preset recovery promise, and no new physical criterion. The current `README.md` describes the four changes while retaining the inherited routing, filtering, memory, preset, timeline, transport, capture, and cleanup contracts.

## Preserved original v1 audit

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
