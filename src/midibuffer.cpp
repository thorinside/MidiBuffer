#include <cstddef>
#include <distingnt/api.h>

#include <new>
#include <stdint.h>

#include "nt_host.hpp"

namespace midibuffer {
namespace {

const uint32_t kBytesPerMegabyte = 1000000U;
const int32_t kDefaultBufferMegabytes = 1;
const float kGateThresholdVolts = 1.0f;

enum Parameter {
    kParameterClock,
    kParameterReset,
    kNumParameters,
};

enum Specification {
    kSpecificationBufferMegabytes,
    kNumSpecifications,
};

struct CallbackState {
    uint32_t clockEdges;
    uint32_t resetEdges;
    uint32_t channelMessages;
    uint32_t realtimeMessages;
    uint32_t parameterChanges;
    uint32_t uiChanges;
    uint8_t lastMidi[3];
    uint8_t lastRealtime;
    bool clockHigh;
    bool resetHigh;
};

struct Algorithm : public _NT_algorithm {
    Algorithm(uint8_t* recordingBufferValue, uint32_t recordingBufferBytesValue)
        : recordingBuffer(recordingBufferValue),
          recordingBufferBytes(recordingBufferBytesValue),
          state() {}

    uint8_t* recordingBuffer;
    uint32_t recordingBufferBytes;
    CallbackState state;
};

static const _NT_parameter kParameters[] = {
    NT_PARAMETER_CV_INPUT("Clock", 1, 1)
    NT_PARAMETER_CV_INPUT("Reset", 1, 2)
};

static const uint8_t kInputPageParameters[] = {
    kParameterClock,
    kParameterReset,
};

static const _NT_parameterPage kParameterPageDefinitions[] = {
    {
        .name = "Inputs",
        .numParams = ARRAY_SIZE(kInputPageParameters),
        .group = 0,
        .unused = {0, 0},
        .params = kInputPageParameters,
    },
};

static const _NT_parameterPages kParameterPages = {
    .numPages = ARRAY_SIZE(kParameterPageDefinitions),
    .pages = kParameterPageDefinitions,
};

static const _NT_specification kSpecifications[] = {
    {
        .name = "Buffer MB",
        .min = 1,
        .max = 5,
        .def = kDefaultBufferMegabytes,
        .type = kNT_typeGeneric,
    },
};

int32_t bufferMegabytes(const int32_t* specifications) {
    int32_t value = specifications == NULL
                        ? kDefaultBufferMegabytes
                        : specifications[kSpecificationBufferMegabytes];
    if (value < kSpecifications[kSpecificationBufferMegabytes].min) {
        return kSpecifications[kSpecificationBufferMegabytes].min;
    }
    if (value > kSpecifications[kSpecificationBufferMegabytes].max) {
        return kSpecifications[kSpecificationBufferMegabytes].max;
    }
    return value;
}

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t* specifications) {
    requirements.numParameters = kNumParameters;
    requirements.sram = sizeof(Algorithm);
    requirements.dram =
        static_cast<uint32_t>(bufferMegabytes(specifications)) * kBytesPerMegabyte;
    requirements.dtc = 0;
    requirements.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements& requirements,
                         const int32_t* specifications) {
    if (memory.sram == NULL || memory.dram == NULL ||
        requirements.sram < sizeof(Algorithm) ||
        requirements.dram < static_cast<uint32_t>(bufferMegabytes(specifications)) *
                                kBytesPerMegabyte) {
        return NULL;
    }

    Algorithm* algorithm =
        new (memory.sram) Algorithm(memory.dram, requirements.dram);
    algorithm->parameters = kParameters;
    algorithm->parameterPages = &kParameterPages;
    return algorithm;
}

void parameterChanged(_NT_algorithm* self, int parameter) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm != NULL && parameter >= 0 && parameter < kNumParameters) {
        ++algorithm->state.parameterChanges;
    }
}

void scanGate(const float* frames, int numFrames, bool& wasHigh,
              uint32_t& edgeCount) {
    for (int frame = 0; frame < numFrames; ++frame) {
        const bool high = frames[frame] > kGateThresholdVolts;
        if (high && !wasHigh) {
            ++edgeCount;
        }
        wasHigh = high;
    }
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL || algorithm->v == NULL || busFrames == NULL ||
        numFramesBy4 <= 0) {
        return;
    }

    const int numFrames = numFramesBy4 * 4;
    const int clockBus = algorithm->v[kParameterClock];
    const int resetBus = algorithm->v[kParameterReset];
    if (clockBus >= 1 && clockBus <= kNT_lastBus) {
        scanGate(busFrames + (clockBus - 1) * numFrames, numFrames,
                 algorithm->state.clockHigh, algorithm->state.clockEdges);
    }
    if (resetBus >= 1 && resetBus <= kNT_lastBus) {
        scanGate(busFrames + (resetBus - 1) * numFrames, numFrames,
                 algorithm->state.resetHigh, algorithm->state.resetEdges);
    }
}

void midiRealtime(_NT_algorithm* self, uint8_t byte) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    ++algorithm->state.realtimeMessages;
    algorithm->state.lastRealtime = byte;
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1,
                 uint8_t byte2) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    ++algorithm->state.channelMessages;
    algorithm->state.lastMidi[0] = byte0;
    algorithm->state.lastMidi[1] = byte1;
    algorithm->state.lastMidi[2] = byte2;
}

uint32_t hasCustomUi(_NT_algorithm*) {
    return kNT_potL | kNT_potC | kNT_potR | kNT_encoderL | kNT_encoderR |
           kNT_encoderButtonL | kNT_encoderButtonR;
}

void customUi(_NT_algorithm* self, const _NT_uiData& data) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return;
    }
    if (data.controls != 0 || data.encoders[0] != 0 || data.encoders[1] != 0) {
        ++algorithm->state.uiChanges;
    }
}

void setupUi(_NT_algorithm*, _NT_float3& pots) {
    pots[0] = 0.0f;
    pots[1] = 0.0f;
    pots[2] = 0.0f;
}

char* appendUnsigned(char* output, uint32_t value) {
    char reversed[10];
    int count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    while (count > 0) {
        *output++ = reversed[--count];
    }
    *output = '\0';
    return output;
}

void drawCounter(int y, const char* label, uint32_t value) {
    char text[32];
    char* cursor = text;
    while (*label != '\0') {
        *cursor++ = *label++;
    }
    appendUnsigned(cursor, value);
    nt_host::drawText(0, y, text);
}

bool draw(_NT_algorithm* self) {
    Algorithm* algorithm = static_cast<Algorithm*>(self);
    if (algorithm == NULL) {
        return false;
    }

    nt_host::drawText(0, 8, "MidiBuffer");
    drawCounter(18, "Clock: ", algorithm->state.clockEdges);
    drawCounter(28, "Reset: ", algorithm->state.resetEdges);
    drawCounter(38, "MIDI: ", algorithm->state.channelMessages);
    drawCounter(48, "RT: ", algorithm->state.realtimeMessages);
    drawCounter(58, "UI: ", algorithm->state.uiChanges);
    return false;
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('M', 'd', 'B', 'f'),
    .name = "MidiBuffer",
    .description = "Clocked MIDI history buffer",
    .numSpecifications = kNumSpecifications,
    .specifications = kSpecifications,
    .calculateStaticRequirements = NULL,
    .initialise = NULL,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = draw,
    .midiRealtime = midiRealtime,
    .midiMessage = midiMessage,
    .tags = kNT_tagUtility,
    .hasCustomUi = hasCustomUi,
    .customUi = customUi,
    .setupUi = setupUi,
    .serialise = NULL,
    .deserialise = NULL,
    .midiSysEx = NULL,
    .parameterUiPrefix = NULL,
    .parameterString = NULL,
};

}  // namespace

namespace nt_host {

void drawText(int x, int y, const char* text) {
    NT_drawText(x, y, text);
}

void sendMidiByte(uint32_t destination, uint8_t byte0) {
    NT_sendMidiByte(destination, byte0);
}

void sendMidi2(uint32_t destination, uint8_t byte0, uint8_t byte1) {
    NT_sendMidi2ByteMessage(destination, byte0, byte1);
}

void sendMidi3(uint32_t destination, uint8_t byte0, uint8_t byte1,
               uint8_t byte2) {
    NT_sendMidi3ByteMessage(destination, byte0, byte1, byte2);
}

}  // namespace nt_host
}  // namespace midibuffer

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
        case kNT_selector_version:
            return kNT_apiVersion13;
        case kNT_selector_numFactories:
            return 1;
        case kNT_selector_factoryInfo:
            return reinterpret_cast<uintptr_t>(data == 0 ? &midibuffer::kFactory
                                                         : NULL);
    }
    return 0;
}
