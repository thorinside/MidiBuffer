#include "host_double.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

uint64_t gHeapAllocationCount = 0;
midibuffer_test::Trace gTrace;

void copyText(char* destination, size_t capacity, const char* source) {
    if (capacity == 0) {
        return;
    }
    size_t index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void recordMidi(uint32_t destination, uint8_t size, uint8_t byte0,
                uint8_t byte1, uint8_t byte2) {
    if (gTrace.midiCallCount >= ARRAY_SIZE(gTrace.midiCalls)) {
        return;
    }
    midibuffer_test::MidiCall& call =
        gTrace.midiCalls[gTrace.midiCallCount++];
    call.destination = destination;
    call.size = size;
    call.bytes[0] = byte0;
    call.bytes[1] = byte1;
    call.bytes[2] = byte2;
}

}  // namespace

void* operator new(size_t size) {
    ++gHeapAllocationCount;
    void* memory = std::malloc(size);
    if (memory == NULL) {
        std::abort();
    }
    return memory;
}

void* operator new[](size_t size) {
    return operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

extern "C" {

const _NT_globals NT_globals = {
    48000,
    8,
    NULL,
    0,
    0,
    0,
};

uint8_t NT_screen[128 * 64];

void NT_drawText(int x, int y, const char* text, int, _NT_textAlignment,
                 _NT_textSize) {
    if (gTrace.drawCallCount >= ARRAY_SIZE(gTrace.drawCalls)) {
        return;
    }
    midibuffer_test::DrawCall& call =
        gTrace.drawCalls[gTrace.drawCallCount++];
    call.x = x;
    call.y = y;
    copyText(call.text, sizeof(call.text), text);
}

void NT_sendMidiByte(uint32_t destination, uint8_t byte0) {
    recordMidi(destination, 1, byte0, 0, 0);
}

void NT_sendMidi2ByteMessage(uint32_t destination, uint8_t byte0,
                            uint8_t byte1) {
    recordMidi(destination, 2, byte0, byte1, 0);
}

void NT_sendMidi3ByteMessage(uint32_t destination, uint8_t byte0,
                            uint8_t byte1, uint8_t byte2) {
    recordMidi(destination, 3, byte0, byte1, byte2);
}

}  // extern "C"

namespace midibuffer_test {

void resetTrace() {
    std::memset(&gTrace, 0, sizeof(gTrace));
}

const Trace& trace() {
    return gTrace;
}

uint64_t heapAllocationCount() {
    return gHeapAllocationCount;
}

HostDouble::HostDouble()
    : factory_(NULL),
      requirements_(),
      algorithm_(NULL),
      sram_(NULL),
      dram_(NULL),
      values_(),
      frames_(),
      hostAllocatedBytes_(0) {}

HostDouble::~HostDouble() {
    std::free(sram_);
    std::free(dram_);
}

bool HostDouble::instantiate(int32_t bufferMegabytes) {
    if (algorithm_ != NULL) {
        return false;
    }
    if (pluginEntry(kNT_selector_version, 0) != kNT_apiVersion13 ||
        pluginEntry(kNT_selector_numFactories, 0) != 1) {
        return false;
    }

    factory_ = reinterpret_cast<const _NT_factory*>(
        pluginEntry(kNT_selector_factoryInfo, 0));
    if (factory_ == NULL || factory_->calculateRequirements == NULL ||
        factory_->construct == NULL) {
        return false;
    }

    int32_t specifications[] = {bufferMegabytes};
    factory_->calculateRequirements(requirements_, specifications);
    sram_ = static_cast<uint8_t*>(std::malloc(requirements_.sram));
    dram_ = static_cast<uint8_t*>(std::malloc(requirements_.dram));
    if (sram_ == NULL || dram_ == NULL) {
        return false;
    }
    hostAllocatedBytes_ = static_cast<uint64_t>(requirements_.sram) +
                          requirements_.dram + requirements_.dtc +
                          requirements_.itc;

    _NT_algorithmMemoryPtrs memory = {
        sram_,
        dram_,
        NULL,
        NULL,
    };
    algorithm_ = factory_->construct(memory, requirements_, specifications);
    if (algorithm_ == NULL) {
        return false;
    }

    for (uint32_t index = 0;
         index < requirements_.numParameters && index < ARRAY_SIZE(values_);
         ++index) {
        values_[index] = algorithm_->parameters[index].def;
    }
    algorithm_->v = values_;
    algorithm_->vIncludingCommon = values_;
    return true;
}

_NT_algorithm* HostDouble::algorithm() {
    return algorithm_;
}

const _NT_factory* HostDouble::factory() const {
    return factory_;
}

const _NT_algorithmRequirements& HostDouble::requirements() const {
    return requirements_;
}

uint64_t HostDouble::hostAllocatedBytes() const {
    return hostAllocatedBytes_;
}

void HostDouble::setParameter(size_t index, int16_t value) {
    if (index < ARRAY_SIZE(values_)) {
        values_[index] = value;
    }
}

void HostDouble::clearFrames() {
    std::memset(frames_, 0, sizeof(frames_));
}

float* HostDouble::bus(size_t oneBasedBus) {
    if (oneBasedBus == 0 || oneBasedBus > kNT_lastBus) {
        return NULL;
    }
    return frames_ + (oneBasedBus - 1) * 8;
}

void HostDouble::step(int numFramesBy4) {
    factory_->step(algorithm_, frames_, numFramesBy4);
}

}  // namespace midibuffer_test
