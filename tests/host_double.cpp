#include "host_double.hpp"

#include <distingnt/serialisation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

uint64_t gHeapAllocationCount = 0;
uint64_t gPendingDispatchSample = 0;
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

void setFramebufferPixel(int x, int y, int colour) {
    if (x < 0 || x >= 256 || y < 0 || y >= 64) {
        return;
    }
    uint8_t& byte = NT_screen[y * 128 + x / 2];
    const uint8_t nibble = static_cast<uint8_t>(colour) & 0x0fU;
    byte = (x & 1) == 0
               ? static_cast<uint8_t>((byte & 0x0fU) | (nibble << 4U))
               : static_cast<uint8_t>((byte & 0xf0U) | nibble);
}

void drawFramebufferLine(int x0, int y0, int x1, int y1, int colour) {
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dyMagnitude = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int dy = -dyMagnitude;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        setFramebufferPixel(x0, y0, colour);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = error * 2;
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void recordMidi(uint32_t destination, uint8_t size, uint8_t byte0,
                uint8_t byte1, uint8_t byte2) {
    if (gTrace.midiCallCount >= ARRAY_SIZE(gTrace.midiCalls)) {
        return;
    }
    midibuffer_test::MidiCall& call =
        gTrace.midiCalls[gTrace.midiCallCount++];
    call.dispatchSample = gPendingDispatchSample;
    call.destination = destination;
    call.size = size;
    call.bytes[0] = byte0;
    call.bytes[1] = byte1;
    call.bytes[2] = byte2;
}

enum JsonNodeType {
    kJsonObject,
    kJsonArray,
    kJsonInt,
    kJsonFloat,
    kJsonString,
    kJsonBoolean,
    kJsonNull,
};

struct JsonNode {
    JsonNodeType type;
    std::string name;
    int intValue;
    float floatValue;
    bool boolValue;
    std::string stringValue;
    std::vector<JsonNode> children;

    explicit JsonNode(JsonNodeType nodeType = kJsonNull)
        : type(nodeType), name(), intValue(0), floatValue(0.0f),
          boolValue(false), stringValue(), children() {}
};

struct JsonDocument {
    JsonNode root;
    uint64_t payloadBytes;
    int16_t parameters[16];
    uint32_t parameterCount;

    JsonDocument()
        : root(kJsonObject), payloadBytes(2U), parameters(),
          parameterCount(0U) {}
};

struct JsonStreamState {
    JsonDocument* document;
    std::vector<JsonNode*> stack;
    std::string pendingName;
};

struct JsonParseFrame {
    const JsonNode* node;
    size_t next;
};

struct JsonParseState {
    const JsonNode* current;
    std::vector<JsonParseFrame> stack;
};

JsonNode* appendStreamNode(JsonStreamState& state, JsonNodeType type) {
    JsonNode node(type);
    node.name = state.pendingName;
    state.pendingName.clear();
    JsonNode* parent = state.stack.back();
    parent->children.push_back(node);
    return &parent->children.back();
}

void finishParseFrames(JsonParseState& state) {
    while (!state.stack.empty() &&
           state.stack.back().next >= state.stack.back().node->children.size()) {
        state.stack.pop_back();
    }
}

const JsonNode* selectParseValue(JsonParseState& state) {
    if (state.current != NULL) {
        const JsonNode* selected = state.current;
        state.current = NULL;
        return selected;
    }
    finishParseFrames(state);
    if (state.stack.empty()) {
        return NULL;
    }
    JsonParseFrame& frame = state.stack.back();
    if (frame.node->type != kJsonArray ||
        frame.next >= frame.node->children.size()) {
        return NULL;
    }
    return &frame.node->children[frame.next++];
}

uint64_t jsonValueCount(const JsonNode& node) {
    uint64_t count = 1U;
    for (size_t index = 0; index < node.children.size(); ++index) {
        count += jsonValueCount(node.children[index]);
    }
    return count;
}

JsonNode* namedChild(JsonNode& parent, const char* name) {
    for (size_t index = 0; index < parent.children.size(); ++index) {
        if (parent.children[index].name == name) {
            return &parent.children[index];
        }
    }
    return NULL;
}

const JsonNode* namedChild(const JsonNode& parent, const char* name) {
    for (size_t index = 0; index < parent.children.size(); ++index) {
        if (parent.children[index].name == name) {
            return &parent.children[index];
        }
    }
    return NULL;
}

bool sameJsonNode(const JsonNode& left, const JsonNode& right) {
    if (left.type != right.type || left.name != right.name ||
        left.intValue != right.intValue ||
        left.floatValue != right.floatValue ||
        left.boolValue != right.boolValue ||
        left.stringValue != right.stringValue ||
        left.children.size() != right.children.size()) {
        return false;
    }
    for (size_t index = 0; index < left.children.size(); ++index) {
        if (!sameJsonNode(left.children[index], right.children[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

_NT_jsonStream::_NT_jsonStream(void* state) : refCon(state) {}
_NT_jsonStream::~_NT_jsonStream() {}

void _NT_jsonStream::addMemberName(const char* name) {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    state.pendingName = name == NULL ? "" : name;
    state.document->payloadBytes += state.pendingName.size() + 3U;
}

void _NT_jsonStream::openArray() {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonArray);
    state.stack.push_back(node);
    state.document->payloadBytes += 2U;
}

void _NT_jsonStream::closeArray() {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    if (state.stack.size() > 1U) {
        state.stack.pop_back();
    }
}

void _NT_jsonStream::openObject() {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonObject);
    state.stack.push_back(node);
    state.document->payloadBytes += 2U;
}

void _NT_jsonStream::closeObject() {
    closeArray();
}

void _NT_jsonStream::addNumber(int value) {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonInt);
    node->intValue = value;
    state.document->payloadBytes += 12U;
}

void _NT_jsonStream::addNumber(float value) {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonFloat);
    node->floatValue = value;
    state.document->payloadBytes += 16U;
}

void _NT_jsonStream::addString(const char* value) {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonString);
    node->stringValue = value == NULL ? "" : value;
    state.document->payloadBytes += node->stringValue.size() + 3U;
}

void _NT_jsonStream::addFourCC(uint32_t value) {
    char text[5] = {
        static_cast<char>((value >> 24U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
        '\0',
    };
    addString(text);
}

void _NT_jsonStream::addBoolean(bool value) {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    JsonNode* node = appendStreamNode(state, kJsonBoolean);
    node->boolValue = value;
    state.document->payloadBytes += value ? 5U : 6U;
}

void _NT_jsonStream::addNull() {
    JsonStreamState& state = *static_cast<JsonStreamState*>(refCon);
    appendStreamNode(state, kJsonNull);
    state.document->payloadBytes += 5U;
}

_NT_jsonParse::_NT_jsonParse(void* state, int index)
    : refCon(state), i(index) {}
_NT_jsonParse::~_NT_jsonParse() {}

bool _NT_jsonParse::numberOfArrayElements(int& num) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonArray ||
        node->children.size() > static_cast<size_t>(0x7fffffff)) {
        return false;
    }
    num = static_cast<int>(node->children.size());
    state.stack.push_back(JsonParseFrame{node, 0U});
    return true;
}

bool _NT_jsonParse::numberOfObjectMembers(int& num) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonObject ||
        node->children.size() > static_cast<size_t>(0x7fffffff)) {
        return false;
    }
    num = static_cast<int>(node->children.size());
    state.stack.push_back(JsonParseFrame{node, 0U});
    return true;
}

bool _NT_jsonParse::matchName(const char* name) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    finishParseFrames(state);
    if (state.stack.empty()) {
        return false;
    }
    JsonParseFrame& frame = state.stack.back();
    if (frame.node->type != kJsonObject ||
        frame.next >= frame.node->children.size()) {
        return false;
    }
    const JsonNode* child = &frame.node->children[frame.next];
    if (child->name != (name == NULL ? "" : name)) {
        return false;
    }
    ++frame.next;
    state.current = child;
    return true;
}

bool _NT_jsonParse::skipMember() {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    if (state.current != NULL) {
        state.current = NULL;
        return true;
    }
    finishParseFrames(state);
    if (state.stack.empty()) {
        return false;
    }
    JsonParseFrame& frame = state.stack.back();
    if (frame.node->type != kJsonObject ||
        frame.next >= frame.node->children.size()) {
        return false;
    }
    ++frame.next;
    return true;
}

bool _NT_jsonParse::number(int& value) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonInt) {
        return false;
    }
    value = node->intValue;
    return true;
}

bool _NT_jsonParse::number(float& value) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonFloat) {
        return false;
    }
    value = node->floatValue;
    return true;
}

bool _NT_jsonParse::string(const char*& value) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonString) {
        return false;
    }
    value = node->stringValue.c_str();
    return true;
}

bool _NT_jsonParse::boolean(bool& value) {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    if (node == NULL || node->type != kJsonBoolean) {
        return false;
    }
    value = node->boolValue;
    return true;
}

bool _NT_jsonParse::null() {
    JsonParseState& state = *static_cast<JsonParseState*>(refCon);
    const JsonNode* node = selectParseValue(state);
    return node != NULL && node->type == kJsonNull;
}

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

void midibufferTestSetDispatchSample(uint64_t sample) {
    gPendingDispatchSample = sample;
}

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

void NT_drawShapeI(_NT_shape shape, int x0, int y0, int x1, int y1,
                   int colour) {
    if (gTrace.shapeCallCount < ARRAY_SIZE(gTrace.shapeCalls)) {
        midibuffer_test::ShapeCall& call =
            gTrace.shapeCalls[gTrace.shapeCallCount++];
        call.shape = shape;
        call.x0 = x0;
        call.y0 = y0;
        call.x1 = x1;
        call.y1 = y1;
        call.colour = colour;
    }
    if (shape == kNT_point) {
        setFramebufferPixel(x0, y0, colour);
    } else if (shape == kNT_line) {
        drawFramebufferLine(x0, y0, x1, y1, colour);
    } else if (shape == kNT_box || shape == kNT_rectangle) {
        const int left = x0 < x1 ? x0 : x1;
        const int right = x0 < x1 ? x1 : x0;
        const int top = y0 < y1 ? y0 : y1;
        const int bottom = y0 < y1 ? y1 : y0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                if (shape == kNT_rectangle || x == left || x == right ||
                    y == top || y == bottom) {
                    setFramebufferPixel(x, y, colour);
                }
            }
        }
    }
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
    std::memset(NT_screen, 0, sizeof(NT_screen));
    gPendingDispatchSample = 0;
}

const Trace& trace() {
    return gTrace;
}

uint8_t framebufferPixel(int x, int y) {
    if (x < 0 || x >= 256 || y < 0 || y >= 64) {
        return 0;
    }
    const uint8_t byte = NT_screen[y * 128 + x / 2];
    return (x & 1) == 0 ? static_cast<uint8_t>(byte >> 4U)
                        : static_cast<uint8_t>(byte & 0x0fU);
}

size_t litFramebufferPixelCount() {
    size_t count = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            if (framebufferPixel(x, y) != 0U) {
                ++count;
            }
        }
    }
    return count;
}

uint64_t heapAllocationCount() {
    return gHeapAllocationCount;
}

PresetImage::PresetImage() : document_(new JsonDocument()) {}

PresetImage::~PresetImage() {
    delete static_cast<JsonDocument*>(document_);
}

uint64_t PresetImage::payloadBytes() const {
    return static_cast<const JsonDocument*>(document_)->payloadBytes;
}

uint64_t PresetImage::valueCount() const {
    return jsonValueCount(static_cast<const JsonDocument*>(document_)->root);
}

bool PresetImage::equals(const PresetImage& other) const {
    const JsonDocument* left = static_cast<const JsonDocument*>(document_);
    const JsonDocument* right =
        static_cast<const JsonDocument*>(other.document_);
    return left->parameterCount == right->parameterCount &&
           std::memcmp(left->parameters, right->parameters,
                       sizeof(left->parameters)) == 0 &&
           sameJsonNode(left->root, right->root);
}

bool PresetImage::navigation(bool& showAll, uint64_t& manualSpan,
                             int& target) const {
    const JsonDocument* document =
        static_cast<const JsonDocument*>(document_);
    const JsonNode* state = namedChild(document->root, "midibufferState");
    const JsonNode* navigation =
        state == NULL ? NULL : namedChild(*state, "navigation");
    if (navigation == NULL || navigation->type != kJsonArray ||
        navigation->children.size() != 7U ||
        navigation->children[0].type != kJsonInt ||
        navigation->children[0].intValue != 1 ||
        navigation->children[1].type != kJsonBoolean ||
        navigation->children[6].type != kJsonInt) {
        return false;
    }
    manualSpan = 0U;
    for (uint32_t part = 0; part < 4U; ++part) {
        const JsonNode& node = navigation->children[part + 2U];
        if (node.type != kJsonInt || node.intValue < 0 ||
            node.intValue > 65535) {
            return false;
        }
        manualSpan |= static_cast<uint64_t>(node.intValue) << (part * 16U);
    }
    showAll = navigation->children[1].boolValue;
    target = navigation->children[6].intValue;
    return true;
}

bool PresetImage::removeNavigation() {
    JsonDocument* document = static_cast<JsonDocument*>(document_);
    JsonNode* state = namedChild(document->root, "midibufferState");
    if (state == NULL) {
        return false;
    }
    for (std::vector<JsonNode>::iterator child = state->children.begin();
         child != state->children.end(); ++child) {
        if (child->name == "navigation") {
            state->children.erase(child);
            return true;
        }
    }
    return false;
}

bool PresetImage::corruptNavigationTarget() {
    JsonDocument* document = static_cast<JsonDocument*>(document_);
    JsonNode* state = namedChild(document->root, "midibufferState");
    JsonNode* navigation =
        state == NULL ? NULL : namedChild(*state, "navigation");
    if (navigation == NULL || navigation->type != kJsonArray ||
        navigation->children.size() != 7U) {
        return false;
    }
    navigation->children[6].type = kJsonInt;
    navigation->children[6].intValue = 3;
    return true;
}

HostDouble::HostDouble()
    : factory_(NULL),
      requirements_(),
      algorithm_(NULL),
      sram_(NULL),
      dram_(NULL),
      values_(),
      frames_(),
      hostAllocatedBytes_(0),
      elapsedSamples_(0) {}

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

uint64_t HostDouble::elapsedSamples() const {
    return elapsedSamples_;
}

void HostDouble::setParameter(size_t index, int16_t value) {
    if (index < ARRAY_SIZE(values_)) {
        values_[index] = value;
    }
}

int16_t HostDouble::parameter(size_t index) const {
    return index < ARRAY_SIZE(values_) ? values_[index] : 0;
}

bool HostDouble::savePreset(PresetImage& image) {
    if (algorithm_ == NULL || factory_ == NULL || factory_->serialise == NULL) {
        return false;
    }
    JsonDocument* document = static_cast<JsonDocument*>(image.document_);
    *document = JsonDocument();
    document->parameterCount = requirements_.numParameters;
    if (document->parameterCount > ARRAY_SIZE(document->parameters)) {
        return false;
    }
    for (uint32_t index = 0; index < document->parameterCount; ++index) {
        document->parameters[index] = values_[index];
    }
    JsonStreamState state = {document, std::vector<JsonNode*>(), std::string()};
    state.stack.push_back(&document->root);
    _NT_jsonStream stream(&state);
    factory_->serialise(algorithm_, stream);
    return state.stack.size() == 1U && state.pendingName.empty();
}

bool HostDouble::loadPreset(const PresetImage& image,
                            bool restoreParametersAfterCustomState) {
    if (algorithm_ == NULL || factory_ == NULL || factory_->deserialise == NULL) {
        return false;
    }
    const JsonDocument* document =
        static_cast<const JsonDocument*>(image.document_);
    if (document->parameterCount != requirements_.numParameters ||
        document->parameterCount > ARRAY_SIZE(values_)) {
        return false;
    }
    if (!restoreParametersAfterCustomState) {
        for (uint32_t index = 0; index < document->parameterCount; ++index) {
            values_[index] = document->parameters[index];
            factory_->parameterChanged(algorithm_, static_cast<int>(index));
        }
    }
    JsonParseState state = {&document->root, std::vector<JsonParseFrame>()};
    _NT_jsonParse parse(&state, 0);
    if (!factory_->deserialise(algorithm_, parse)) {
        return false;
    }
    if (restoreParametersAfterCustomState) {
        for (uint32_t index = 0; index < document->parameterCount; ++index) {
            values_[index] = document->parameters[index];
            factory_->parameterChanged(algorithm_, static_cast<int>(index));
        }
    }
    return true;
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
    if (numFramesBy4 > 0) {
        elapsedSamples_ += static_cast<uint64_t>(numFramesBy4) * 4U;
    }
}

}  // namespace midibuffer_test
