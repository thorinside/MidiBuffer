PLUGIN_NAME := MidiBuffer
API_INCLUDE := distingNT_API/include
SOURCE := src/midibuffer.cpp
NATIVE_TEST := build/callback_contract_test
ARM_OBJECT := plugins/$(PLUGIN_NAME).o

NATIVE_CXX ?= clang++
ARM_CXX ?= arm-none-eabi-g++
ARM_READELF ?= arm-none-eabi-readelf
ARM_NM ?= arm-none-eabi-nm

COMMON_WARNINGS := -Wall -Wextra -Werror
COMMON_FLAGS := -std=gnu++11 $(COMMON_WARNINGS) -fno-exceptions -fno-rtti -I$(API_INCLUDE) -Isrc
NATIVE_FLAGS := $(COMMON_FLAGS) -O2
ARM_FLAGS := $(COMMON_FLAGS) -Os -fPIC -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb \
	-ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables

.PHONY: all test hardware inspect verify clean

all: verify

test: $(NATIVE_TEST)
	./$(NATIVE_TEST)

hardware: $(ARM_OBJECT)

inspect: $(ARM_OBJECT)
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Class:[[:space:]]+ELF32'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Data:[[:space:]]+2.s complement, little endian'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Type:[[:space:]]+REL \(Relocatable file\)'
	@$(ARM_READELF) -h $(ARM_OBJECT) | grep -Eq 'Machine:[[:space:]]+ARM'
	@$(ARM_NM) --defined-only $(ARM_OBJECT) | grep -Eq '[[:space:]]T[[:space:]]pluginEntry$$'
	@unexpected="$$( $(ARM_NM) -u $(ARM_OBJECT) | awk '{print $$2}' | grep -Ev '^(NT_drawText|NT_sendMidiByte|NT_sendMidi2ByteMessage|NT_sendMidi3ByteMessage|memset)$$' || true )"; \
		test -z "$$unexpected" || { echo "Unexpected undefined symbols:"; echo "$$unexpected"; exit 1; }
	@echo "PASS: valid ARM relocatable object exports pluginEntry; undefined symbols match the host/libc allowlist"

verify: test hardware inspect

$(NATIVE_TEST): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp tests/host_double.cpp tests/host_double.hpp tests/callback_contract_test.cpp | build
	$(NATIVE_CXX) $(NATIVE_FLAGS) -Itests $(SOURCE) tests/host_double.cpp tests/callback_contract_test.cpp -o $@

$(ARM_OBJECT): $(SOURCE) src/midibuffer_core.hpp src/nt_host.hpp | plugins
	$(ARM_CXX) $(ARM_FLAGS) -c $(SOURCE) -o $@

build plugins:
	mkdir -p $@

clean:
	rm -rf build plugins
