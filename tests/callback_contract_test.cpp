#include "host_double.hpp"

#include "../src/nt_host.hpp"

#include <cstdio>
#include <cstring>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

bool drawContains(const char* expected) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    for (size_t index = 0; index < current.drawCallCount; ++index) {
        if (std::strcmp(current.drawCalls[index].text, expected) == 0) {
            return true;
        }
    }
    return false;
}

void verifyEntryAndLifecycle(midibuffer_test::HostDouble& host) {
    expect(host.instantiate(3), "instance constructs through pluginEntry factory");
    expect(host.factory() != NULL, "factory is registered");
    expect(std::strcmp(host.factory()->name, "MidiBuffer") == 0,
           "factory name is MidiBuffer");
    expect(host.factory()->numSpecifications == 1,
           "buffer allocation specification is registered");
    expect(host.factory()->specifications[0].min == 1 &&
               host.factory()->specifications[0].max == 5,
           "buffer specification supports 1 through 5 MB");
    expect(pluginEntry(kNT_selector_factoryInfo, 1) == 0,
           "factory selector rejects an out-of-range index");

    _NT_algorithmRequirements defaultRequirements = {};
    _NT_algorithmRequirements minimumRequirements = {};
    _NT_algorithmRequirements maximumRequirements = {};
    host.factory()->calculateRequirements(defaultRequirements, NULL);
    const int32_t belowMinimum[] = {0};
    const int32_t aboveMaximum[] = {6};
    host.factory()->calculateRequirements(minimumRequirements, belowMinimum);
    host.factory()->calculateRequirements(maximumRequirements, aboveMaximum);
    expect(defaultRequirements.dram == 1000000U &&
               minimumRequirements.dram == 1000000U &&
               maximumRequirements.dram == 5000000U,
           "allocation requirements default and clamp to the approved byte range");
    expect(host.requirements().numParameters == 2,
           "clock and reset parameters are requested");
    expect(host.requirements().dram == 3000000U,
           "selected recording buffer bytes are requested from DRAM");
    expect(host.hostAllocatedBytes() ==
               static_cast<uint64_t>(host.requirements().sram) + 3000000U,
           "host allocation accounting matches all requested memory");
    expect(host.algorithm()->parameters != NULL &&
               host.algorithm()->parameterPages != NULL,
           "constructed instance publishes parameter definitions and pages");
    expect(host.factory()->parameterChanged != NULL &&
               host.factory()->step != NULL && host.factory()->draw != NULL &&
               host.factory()->midiMessage != NULL &&
               host.factory()->midiRealtime != NULL &&
               host.factory()->hasCustomUi != NULL &&
               host.factory()->customUi != NULL &&
               host.factory()->setupUi != NULL,
           "the factory registers every callback adapter in this slice");
    expect(std::strcmp(host.algorithm()->parameters[0].name, "Clock") == 0 &&
               std::strcmp(host.algorithm()->parameters[1].name, "Reset") == 0,
           "clock and reset inputs are registered in stable order");
}

void verifyCallbacks(midibuffer_test::HostDouble& host) {
    host.setParameter(0, 1);
    host.setParameter(1, 2);
    host.clearFrames();
    host.bus(1)[1] = 2.0f;
    host.bus(1)[2] = 2.0f;
    host.bus(2)[5] = 5.0f;

    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    host.factory()->parameterChanged(host.algorithm(), 0);
    host.step(2);
    host.factory()->midiMessage(host.algorithm(), 0x92, 60, 100);
    host.factory()->midiRealtime(host.algorithm(), 0xF8);

    _NT_uiData controls = {};
    controls.controls = kNT_potL;
    controls.encoders[1] = 1;
    host.factory()->customUi(host.algorithm(), controls);

    _NT_float3 pots = {-1.0f, -1.0f, -1.0f};
    host.factory()->setupUi(host.algorithm(), pots);
    expect(pots[0] == 0.0f && pots[1] == 0.0f && pots[2] == 0.0f,
           "setupUi supplies deterministic initial pot positions");
    expect((host.factory()->hasCustomUi(host.algorithm()) & kNT_encoderL) != 0,
           "custom control mask includes the timeline encoders");

    midibuffer_test::resetTrace();
    expect(!host.factory()->draw(host.algorithm()),
           "draw preserves the standard parameter line");
    expect(drawContains("MidiBuffer"), "draw callback identifies the plugin");
    expect(drawContains("Clock: 1"),
           "step adapter sees one rising clock edge in controllable frames");
    expect(drawContains("Reset: 1"),
           "step adapter sees one rising reset edge in controllable frames");
    expect(drawContains("MIDI: 1"),
           "MIDI channel callback reaches instance state");
    expect(drawContains("RT: 1"),
           "MIDI realtime callback reaches instance state");
    expect(drawContains("UI: 1"),
           "custom UI callback reaches instance state");
    expect(midibuffer_test::heapAllocationCount() == allocationsBefore,
           "real-time and UI callbacks perform no heap allocation");

    host.clearFrames();
    host.step(2);
    host.clearFrames();
    host.bus(1)[7] = 2.0f;
    host.bus(2)[0] = 2.0f;
    host.step(2);
    midibuffer_test::resetTrace();
    host.factory()->draw(host.algorithm());
    expect(drawContains("Clock: 2") && drawContains("Reset: 2"),
           "gate state persists across callback blocks deterministically");
}

void verifyHostOutputTrace() {
    midibuffer_test::resetTrace();
    midibuffer::nt_host::sendMidiByte(kNT_destinationInternal, 0xF8);
    midibuffer::nt_host::sendMidi2(kNT_destinationUSB, 0xD0, 64);
    midibuffer::nt_host::sendMidi3(kNT_destinationBreakout, 0x90, 60, 100);

    const midibuffer_test::Trace& current = midibuffer_test::trace();
    expect(current.midiCallCount == 3,
           "host double records all production MIDI output adapters");
    expect(current.midiCalls[0].destination == kNT_destinationInternal &&
               current.midiCalls[0].size == 1 &&
               current.midiCalls[0].bytes[0] == 0xF8,
           "one-byte outgoing MIDI trace is deterministic");
    expect(current.midiCalls[1].destination == kNT_destinationUSB &&
               current.midiCalls[1].size == 2 &&
               current.midiCalls[1].bytes[0] == 0xD0 &&
               current.midiCalls[1].bytes[1] == 64,
           "two-byte outgoing MIDI trace is deterministic");
    expect(current.midiCalls[2].destination == kNT_destinationBreakout &&
               current.midiCalls[2].size == 3 &&
               current.midiCalls[2].bytes[0] == 0x90 &&
               current.midiCalls[2].bytes[1] == 60 &&
               current.midiCalls[2].bytes[2] == 100,
           "three-byte outgoing MIDI trace is deterministic");
}

}  // namespace

int main() {
    midibuffer_test::resetTrace();
    midibuffer_test::HostDouble host;
    verifyEntryAndLifecycle(host);
    if (host.algorithm() != NULL) {
        verifyCallbacks(host);
    }
    verifyHostOutputTrace();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d callback contract checks failed\n", gFailures);
        return 1;
    }
    std::printf("PASS: NT adapter callback contract\n");
    return 0;
}
