PLUGIN_NAME := MidiBuffer
API_INCLUDE := distingNT_API/include
SOURCE := src/midibuffer.cpp
NATIVE_TEST := build/callback_contract_test
SANITIZER_TEST := build/callback_contract_asan_ubsan
QUALIFICATION_TEST := build/qualification_test
QUALIFICATION_SANITIZER_TEST := build/qualification_asan_ubsan
RANGE_MOTION_TEST := build/range_motion_test
ARM_OBJECT := plugins/$(PLUGIN_NAME).o
ARM_RANGE_MOTION_OBJECT := build/range_motion_arm.o
ARM_LAYOUT_OBJECT := build/resource_layout_arm.o
TRACEABILITY_MANIFEST := docs/MIDIBUFFER_THREE_TRACEABILITY.json
TRACEABILITY_CHECK := tests/verify_traceability.py

NATIVE_CXX ?= clang++
ARM_CXX ?= arm-none-eabi-g++
ARM_READELF ?= arm-none-eabi-readelf
ARM_NM ?= arm-none-eabi-nm
ARM_SIZE ?= arm-none-eabi-size
CPPCHECK ?= cppcheck
PYTHON ?= python3

COMMON_WARNINGS := -Wall -Wextra -Werror
COMMON_FLAGS := -std=gnu++11 $(COMMON_WARNINGS) -fno-exceptions -fno-rtti -I$(API_INCLUDE) -Isrc
NATIVE_FLAGS := $(COMMON_FLAGS) -O2 -DMIDIBUFFER_NATIVE_TEST=1 -D_DISTINGNT_SERIALISATION_INTERNAL=1
SANITIZER_FLAGS := $(COMMON_FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	-DMIDIBUFFER_NATIVE_TEST=1 -D_DISTINGNT_SERIALISATION_INTERNAL=1
ARM_FLAGS := $(COMMON_FLAGS) -Os -fPIC -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb \
	-ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables

.PHONY: all test dylib sanitize static-check traceability hardware inspect verify clean

all: verify

test: $(NATIVE_TEST) $(RANGE_MOTION_TEST) $(QUALIFICATION_TEST)
	./$(NATIVE_TEST)
	./$(RANGE_MOTION_TEST)
	./$(QUALIFICATION_TEST)

sanitize: $(SANITIZER_TEST) $(QUALIFICATION_SANITIZER_TEST)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./$(SANITIZER_TEST)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./$(QUALIFICATION_SANITIZER_TEST)
	@echo "PASS: native production callbacks pass ASan/UBSan"

static-check:
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c++11 \
		--suppress=missingIncludeSystem -DMIDIBUFFER_NATIVE_TEST=1 \
		-D_DISTINGNT_SERIALISATION_INTERNAL=1 '-DARRAY_SIZE(x)=(sizeof(x)/sizeof((x)[0]))' \
		-I$(API_INCLUDE) -Isrc -Itests src tests
	$(PYTHON) tests/verify_realtime_source.py src/*.cpp src/*.hpp
	@echo "PASS: focused static analysis and production blocking-I/O/logging inspection"

traceability: $(TRACEABILITY_MANIFEST) $(TRACEABILITY_CHECK)
	$(PYTHON) $(TRACEABILITY_CHECK) $(TRACEABILITY_MANIFEST)

# Native host build for nt_emu / VCV Rack (not part of the hardware build)
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
test: vital-receiver-smoke

.PHONY: vital-receiver-smoke
vital-receiver-smoke: build/vital_receiver_smoke
	./build/vital_receiver_smoke
	$(PYTHON) tests/verify_vital_state.py

build/vital_receiver_smoke: tests/vital_receiver_smoke.cpp tests/vital-smoke-state.plist | build
	$(NATIVE_CXX) -std=c++11 $(COMMON_WARNINGS) -Wno-multichar -Wno-deprecated-declarations $< \
		-framework AudioToolbox -framework CoreMIDI -framework CoreFoundation -o $@

# req-014/ac-015 local qualification: direct versus replayed MPE in Vital 1.5.5
# (needs installed Vital and numpy; logs/renders/report in build/receiver).
.PHONY: vital-receiver-compare
vital-receiver-compare: build/receiver_fixture build/vital_receiver_render
	$(PYTHON) tests/vital_receiver_compare.py

build/receiver_fixture: $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/host_double.cpp tests/host_double.hpp tests/receiver_fixture.cpp | build
	$(NATIVE_CXX) $(NATIVE_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/receiver_fixture.cpp -o $@

build/vital_receiver_render: tests/vital_receiver_render.cpp | build
	$(NATIVE_CXX) -std=c++11 -O2 $(COMMON_WARNINGS) -Wno-multichar -Wno-deprecated-declarations $< \
		-framework AudioToolbox -framework CoreMIDI -framework CoreFoundation -o $@

HOST_DYLIB_EXT := dylib
HOST_DYLIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup
else
HOST_DYLIB_EXT := so
HOST_DYLIB_LDFLAGS := -shared
endif
HOST_DYLIB := plugins/$(PLUGIN_NAME).$(HOST_DYLIB_EXT)

dylib: $(HOST_DYLIB)

$(HOST_DYLIB): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp
	mkdir -p plugins
	$(NATIVE_CXX) $(COMMON_FLAGS) -O2 -fPIC $(HOST_DYLIB_LDFLAGS) -o $@ $(SOURCE)
	@echo "Built host plugin: $@"

hardware: $(ARM_OBJECT) $(ARM_RANGE_MOTION_OBJECT) $(ARM_LAYOUT_OBJECT)

inspect: $(ARM_OBJECT) $(ARM_RANGE_MOTION_OBJECT) $(ARM_LAYOUT_OBJECT)
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Class:[[:space:]]+ELF32'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Data:[[:space:]]+2.s complement, little endian'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Type:[[:space:]]+REL \(Relocatable file\)'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Machine:[[:space:]]+ARM'
	@$(ARM_NM) --defined-only $(ARM_OBJECT) | grep -Eq '[[:space:]]T[[:space:]]pluginEntry$$'
	@$(ARM_READELF) -h $(ARM_RANGE_MOTION_OBJECT) | grep -Eq 'Machine:[[:space:]]+ARM'
	@test -z "$$($(ARM_NM) -u $(ARM_RANGE_MOTION_OBJECT))" || { echo "Range motion ARM object has unexpected undefined symbols:"; $(ARM_NM) -u $(ARM_RANGE_MOTION_OBJECT); exit 1; }
	@# These four parameter symbols are the reviewed API v13 host-setter seam.
	@unexpected="$$( $(ARM_NM) -u $(ARM_OBJECT) | awk '{print $$2}' | grep -Ev '^(_GLOBAL_OFFSET_TABLE_|NT_algorithmIndex|NT_drawShapeI|NT_drawText|NT_globals|NT_parameterOffset|NT_sendMidiByte|NT_sendMidi2ByteMessage|NT_sendMidi3ByteMessage|NT_setParameterFromAudio|NT_setParameterFromUi|memset|_ZN13_NT_jsonParse.*|_ZN14_NT_jsonStream.*)$$' || true )"; \
		test -z "$$unexpected" || { echo "Unexpected undefined symbols:"; echo "$$unexpected"; exit 1; }
	@$(ARM_SIZE) $(ARM_OBJECT)
	@echo "PASS: ARM ELF/pluginEntry inspection and reviewed API v13 undefined-symbol allowlist"
	@# ARM-ABI resource layout: SRAM <= 50,360 B; sidecar exactly 524,288 B
	@# (489,528 B used); 24-byte events; DRAM = decimal buffer + sidecar at
	@# every size, including ordinary mode; canonical MPE preset data bound.
	@$(ARM_NM) -S -t d $(ARM_LAYOUT_OBJECT) | awk '/midibufferLayout/ { size[$$4] = $$2 + 0 } \
		END { sram = size["midibufferLayoutSram"]; side = size["midibufferLayoutSidecar"]; \
		used = size["midibufferLayoutSidecarUsed"]; event = size["midibufferLayoutEvent"]; \
		preset = size["midibufferLayoutPresetMaximum"]; \
		if (sram == 0 || sram > 50360 || side != 524288 || used != 489528 || event != 24 || preset > 405976) { \
			printf "FAIL: ARM layout SRAM=%d sidecar=%d used=%d event=%d preset=%d\n", sram, side, used, event, preset; exit 1 } \
		printf "RESOURCE: ARM SRAM=%d B (<= 50,360); sidecar=%d B (used %d B); event=%d B; MPE preset max=%d B binary, %d hex chars\n", sram, side, used, event, preset, 2 * preset; \
		for (mb = 1; mb <= 5; ++mb) printf "RESOURCE: %d MB: %d events, DRAM %d B (events %d + sidecar %d)\n", mb, int(mb * 1000000 / event), mb * 1000000 + side, mb * 1000000, side }'

verify: test sanitize static-check traceability hardware inspect
	@echo "PASS: integrated native/emulator, sanitizer/static, traceability, and ARM regression gate"
	@echo "EVIDENCE: physical disting NT checks are separate and were not run by this gate"

$(NATIVE_TEST): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/host_double.cpp tests/host_double.hpp tests/callback_contract_test.cpp | build
	$(NATIVE_CXX) $(NATIVE_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/callback_contract_test.cpp -o $@

$(SANITIZER_TEST): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/host_double.cpp tests/host_double.hpp tests/callback_contract_test.cpp | build
	$(NATIVE_CXX) $(SANITIZER_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/callback_contract_test.cpp -o $@

$(QUALIFICATION_TEST): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/host_double.cpp tests/host_double.hpp tests/qualification_test.cpp | build
	$(NATIVE_CXX) $(NATIVE_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/qualification_test.cpp -o $@

$(QUALIFICATION_SANITIZER_TEST): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/host_double.cpp tests/host_double.hpp tests/qualification_test.cpp | build
	$(NATIVE_CXX) $(SANITIZER_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/qualification_test.cpp -o $@

$(RANGE_MOTION_TEST): src/range_motion.hpp tests/range_motion_test.cpp | build
	$(NATIVE_CXX) $(COMMON_FLAGS) tests/range_motion_test.cpp -o $@

$(ARM_OBJECT): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp | plugins
	$(ARM_CXX) $(ARM_FLAGS) -c $(SOURCE) -o $@

$(ARM_LAYOUT_OBJECT): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp src/range_motion.hpp tests/resource_layout_arm.cpp | build
	$(ARM_CXX) $(ARM_FLAGS) -c tests/resource_layout_arm.cpp -o $@

$(ARM_RANGE_MOTION_OBJECT): src/range_motion.hpp tests/range_motion_arm_compile.cpp | build
	$(ARM_CXX) $(ARM_FLAGS) -c tests/range_motion_arm_compile.cpp -o $@

build plugins:
	mkdir -p $@

clean:
	rm -rf build plugins
