# Integrated UX refinement verification

## Scope and result

- Verification date: 2026-09-13 UTC
- Approved add-on Spec SHA-256: `fe17d6067caf1f3066a1b173ad5bb3f0d120774a3631102f50273be1b6456dfc`
- Integrated implementation commits: `131e73a` through `e0b2147`
- Pinned SDK: `5a4910d1d4233180114d6aee5ddaa4b8aec577e8` (API v13)
- ARM product: `plugins/MidiBuffer.o`
- ARM product SHA-256: `cdf5d035b2418daea44b30b6459d84835f4b32eee2f5a9d98ae2ced32dc66daa`

All agent-producible implementation, native callback/framebuffer, arithmetic, sanitizer, static, ARM build, and object-inspection checks pass. Physical disting NT evidence is still pending and is deliberately reported separately below. This report does not change the original v1 Spec, approval, release audit, hardware procedure, or AC-048 evidence.

## Requirement synthesis

| Approved requirements | Integrated evidence |
| --- | --- |
| UX-REQ-001–004 | Production callback tests retain independent pot-1 Start/pot-2 End behavior, add clamped fixed-length unpressed pot-3 Range movement, and preserve Start/End/Range one-pulse encoder-2 targeting through deterministic callback order. |
| UX-REQ-005–006 | Production draw tests add the observational playback line and exact bounded `bars:beats:ticks` Avail/Len strings at the original origins. |
| UX-REQ-007 | Range-motion unit and callback tests cover preserved logical/physical state, residual accumulation, normative accelerated catch-up, release sampling, reversal, clamps, repeated holds, setupUi entry, valid-preset load, and first post-seed movement without a pickup zone. |
| UX-REQ-008–011 | Fresh Show All, retained growth/eviction, full-span re-entry, relative held zoom, end-relative manual policy, selection/event/transport invariance, and spans beyond `uint32_t` are exercised through production callbacks. |
| UX-REQ-012–015 | Exact unaccelerated encoder-1 scrolling, rising-edge playback toggle, Start/End/Range targeting, and sample-clock panic threshold/rearm behavior are covered together with simultaneous rotations and pot-3 holds. |

No menu, parameter, integration, export, telemetry, capture, scheduler, routing, corruption-policy, or unrelated persistence change was introduced.

## Synthetic arithmetic evidence

`build/range_motion_test`, compiled from `tests/range_motion_test.cpp`, passes the standalone fixed-size range-motion model. It covers the three normative catch-up vectors, positive/negative mismatch, tiny accumulated deltas, reversal, logical and physical limits, clamp residual discard, repeated holds, rebases, no-delta behavior, convergence travel, and exact wide-coordinate length preservation. These results establish arithmetic behavior only; they are not callback, emulator, or hardware evidence.

## Native callback and framebuffer-emulator evidence

`build/callback_contract_test` invokes the production factory, callback, serializer, MIDI adapters, draw calls, and a deterministic 256×64 host-double framebuffer. The final integrated run covers all twelve UX criteria, including prior callback/panic/pending-range/preset regression seams plus new coverage rather than relying only on prior approval.

### Modifier/API and takeover

- `hasCustomUi` includes `kNT_potR` and `kNT_potButtonR` with both encoders and their buttons.
- Tests construct real API-v13 `_NT_uiData` using current `controls`, `lastButtons`, and all three actual `pots` samples.
- Press, held zoom, release-only, release-with-`kNT_potR`, first unpressed delta, repeated holds before catch-up, repeated `setupUi` entry, and valid-preset load all pass without selection remap or a second pickup dead zone.

### Draw traces and emulated pixels

| Case | Production draw trace | Native framebuffer evidence |
| --- | --- | --- |
| Normal/invalid at ppb48 | `(0,7) "Avail 0:0:000  48ppb"`; `(176,7) "Len --"` | tiny 3×5 cells use y3..7 only and remain in their original left/right regions |
| Maximum exact | `(0,7) "Avail 9999999999:3:470  48ppb"`; `(176,7) "Len 9999999999:3:470"` | left text ends by x114; right text has ink at x254 and none at x255; no ink above y3 |
| First overflow bar | `(0,7) "Avail >9999999999bar  48ppb"`; `(176,7) "Len >9999999999bar"` | complete marker remains bounded in both regions and does not mutate pulse state |
| Maximum `uint64_t` | same complete overflow strings | no decimal `b` suffix, relocated field, ruler/history text, or `NO CLK` label |

The framebuffer is a native deterministic emulator of the API draw calls and tiny-cell budget. It is not represented as an nt_emu screenshot or physical OLED observation.

### Draw noninterference

One deterministic scenario replays identical captured MIDI, clocks, reset, held-pot controls, external MIDI input, and a pending range edit with zero, sparse, and frequent draws. The three runs have exact equality for:

- outgoing MIDI bytes, destination, order, and scheduler dispatch sample;
- canonical serialized retained-event and scheduler state;
- capture/transport flags, active and edited selections, pending transition, playback pulse/cursor, next event/ending scheduling, and clock state.

### Runtime and fixed storage

A native process-CPU-clock measurement over 20,000 production draws observed 2,136.3 ns/draw with an eligible head and 1,898.8 ns/draw stopped, an incremental 237.5 ns/draw in the final `make verify` run. This is a local comparative measurement, not a display-FPS, firmware, wall-clock, or hardware-performance claim.

`RangeMotionState` is a fixed 32 bytes in the native build. Callback allocation accounting remains unchanged across real-time, MIDI, UI, serializer-independent draw, and the benchmark paths. Source/trace audit confirms the eligible head adds one `timelinePulseX` coordinate conversion call site and exactly one intensity-12 line from y17 through y55 (39 pixels). Existing note traversal remains bounded at 256 drawn notes; the head adds no event traversal. Product source contains no new blocking I/O or callback logging. The current ARM object reports 17,952 text bytes, 528 data bytes, zero BSS, and 18,480 bytes in the ordinary `arm-none-eabi-size` summary (18,649 bytes across all reported sections).

## Verification commands

The final gate consists of:

```sh
make test
make verify
clang++ -std=gnu++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DMIDIBUFFER_NATIVE_TEST=1 -D_DISTINGNT_SERIALISATION_INTERNAL=1 \
  -IdistingNT_API/include -Isrc -Itests \
  src/midibuffer.cpp tests/host_double.cpp tests/callback_contract_test.cpp \
  -o build/callback_contract_asan_ubsan
./build/callback_contract_asan_ubsan
cppcheck --enable=warning,performance,portability --std=c++11 \
  --suppress=missingIncludeSystem -DMIDIBUFFER_NATIVE_TEST=1 \
  -D_DISTINGNT_SERIALISATION_INTERNAL=1 '-DARRAY_SIZE(x)=(sizeof(x)/sizeof((x)[0]))' \
  -IdistingNT_API/include -Isrc -Itests src tests
arm-none-eabi-size -A plugins/MidiBuffer.o
arm-none-eabi-size plugins/MidiBuffer.o
```

`make verify` rebuilds the ARM object, identifies it as ELF32 little-endian ARM EABI5 relocatable, confirms global `pluginEntry`, and restricts undefined symbols to the reviewed NT host/JSON, GOT, and `memset` set.

## Physical evidence — pending owner action

No physical disting NT was accessed during this iteration. Therefore native and emulated evidence does not establish actual-host pot-button delivery/takeover feel or OLED readability. The owner should run [`UX_REFINEMENT_HARDWARE_TEST.md`](UX_REFINEMENT_HARDWARE_TEST.md) unchanged against the exact object digest above and record every result field. It separately checks:

- load and fresh Show All on supported firmware;
- whole-range clamps, reversal, release-with-change, first unpressed movement, repeated unfinished catch-up, and absence of host pickup dead zones;
- both available zoom directions from low/middle/high press positions;
- all established encoder, playback, panic, reset, clock-loss, and pending-wrap gestures; and
- physical tiny-glyph readability/containment and playback-head states at the original display origins.

Until that owner record passes, UX-AC-002, UX-AC-008, and UX-AC-012 remain partial even though their implementation and agent-producible checks pass. All other UX criteria retain their proved status after the integrated rerun.
