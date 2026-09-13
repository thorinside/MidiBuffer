#ifndef MIDIBUFFER_TESTS_HOST_DOUBLE_HPP
#define MIDIBUFFER_TESTS_HOST_DOUBLE_HPP

#include <distingnt/api.h>

#include <stddef.h>
#include <stdint.h>

namespace midibuffer_test {

struct DrawCall {
    int x;
    int y;
    char text[48];
};

struct ShapeCall {
    _NT_shape shape;
    int x0;
    int y0;
    int x1;
    int y1;
    int colour;
};

struct MidiCall {
    uint64_t dispatchSample;
    uint32_t destination;
    uint8_t bytes[3];
    uint8_t size;
};

struct Trace {
    DrawCall drawCalls[16];
    size_t drawCallCount;
    ShapeCall shapeCalls[512];
    size_t shapeCallCount;
    MidiCall midiCalls[128];
    size_t midiCallCount;
};

void resetTrace();
const Trace& trace();
uint8_t framebufferPixel(int x, int y);
size_t litFramebufferPixelCount();
uint64_t heapAllocationCount();

class HostDouble {
  public:
    HostDouble();
    ~HostDouble();
    HostDouble(const HostDouble&) = delete;
    HostDouble& operator=(const HostDouble&) = delete;

    bool instantiate(int32_t bufferMegabytes);
    _NT_algorithm* algorithm();
    const _NT_factory* factory() const;
    const _NT_algorithmRequirements& requirements() const;
    uint64_t hostAllocatedBytes() const;
    uint64_t elapsedSamples() const;

    void setParameter(size_t index, int16_t value);
    void clearFrames();
    float* bus(size_t oneBasedBus);
    void step(int numFramesBy4);

  private:
    const _NT_factory* factory_;
    _NT_algorithmRequirements requirements_;
    _NT_algorithm* algorithm_;
    uint8_t* sram_;
    uint8_t* dram_;
    int16_t values_[16];
    float frames_[kNT_lastBus * 8];
    uint64_t hostAllocatedBytes_;
    uint64_t elapsedSamples_;
};

}  // namespace midibuffer_test

#endif
