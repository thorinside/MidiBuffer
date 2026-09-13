# MidiBuffer

MidiBuffer is an Expert Sleepers disting NT plug-in targeting firmware 1.18 and later through the pinned API v13 SDK.

This initial implementation establishes only the executable host boundary:

- `pluginEntry`, factory discovery, instance requirements, and construction
- fixed pre-instantiation recording-buffer allocation from 1–5 MB (1 MB = 1,000,000 bytes)
- clock/reset CV parameters and callback-block edge scanning
- MIDI input/realtime, drawing, and custom-control callback adapters
- MIDI output and drawing host adapters
- a native host double with deterministic callback traces and heap-allocation accounting
- a PIC ARM Cortex-M7 relocatable object build

Capture, musical playback, preset persistence, packaging/publication, and physical hardware validation are intentionally not implemented by this boundary slice.

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

This runs the native callback-contract test, builds `plugins/MidiBuffer.o`, and verifies that it is an ELF32 little-endian ARM relocatable object exporting `pluginEntry`.

Native tests instantiate MidiBuffer through the production `pluginEntry`/factory path. They drive clock and reset samples, channel and realtime MIDI callbacks, custom controls, drawing, and the production MIDI-output adapters. The host double records requested SRAM/DRAM and checks callback-time C++ heap allocation.

These automated checks do not claim that the plug-in has been loaded on a physical disting NT.
