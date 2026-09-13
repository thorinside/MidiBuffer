#include "host_double.hpp"

#include "../src/midibuffer_core.hpp"
#include "../src/nt_host.hpp"

#include <cstdio>
#include <cstring>

namespace {

const size_t kClockParameter = 0;
const size_t kResetParameter = 1;
const size_t kCaptureParameter = 2;
const size_t kRecordingChannelParameter = 3;
const size_t kPlaybackDestinationParameter = 4;
const size_t kPlaybackChannelParameter = 5;
const size_t kFilterControlChangeParameter = 6;
const size_t kFilterPitchBendParameter = 7;
const size_t kFilterAftertouchParameter = 8;

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

void changeParameter(midibuffer_test::HostDouble& host, size_t parameter,
                     int16_t value) {
    host.setParameter(parameter, value);
    host.factory()->parameterChanged(host.algorithm(),
                                     static_cast<int>(parameter));
}

void startCapture(midibuffer_test::HostDouble& host) {
    changeParameter(host, kCaptureParameter, 1);
}

void stopCapture(midibuffer_test::HostDouble& host) {
    changeParameter(host, kCaptureParameter, 0);
}

void clockPulse(midibuffer_test::HostDouble& host) {
    host.clearFrames();
    host.bus(1)[0] = 2.0f;
    host.step(2);
}

void noClockBlock(midibuffer_test::HostDouble& host) {
    host.clearFrames();
    host.step(2);
}

void resetPulse(midibuffer_test::HostDouble& host) {
    host.clearFrames();
    host.bus(2)[0] = 2.0f;
    host.step(2);
}

void coincidentResetAndClockPulse(midibuffer_test::HostDouble& host) {
    host.clearFrames();
    host.bus(1)[0] = 2.0f;
    host.bus(2)[0] = 2.0f;
    host.step(2);
}

bool clockPulseAt(midibuffer_test::HostDouble& host, uint64_t sample) {
    if (sample < host.elapsedSamples()) {
        return false;
    }
    while (host.elapsedSamples() + 8U <= sample) {
        noClockBlock(host);
    }
    const uint64_t frame = sample - host.elapsedSamples();
    if (frame >= 8U) {
        return false;
    }
    host.clearFrames();
    host.bus(1)[frame] = 2.0f;
    host.step(2);
    return true;
}

void stepThrough(midibuffer_test::HostDouble& host, uint64_t sample) {
    while (host.elapsedSamples() <= sample) {
        noClockBlock(host);
    }
}

void sendMidi(midibuffer_test::HostDouble& host, uint8_t status, uint8_t data1,
              uint8_t data2) {
    host.factory()->midiMessage(host.algorithm(), status, data1, data2);
}

void acquireClock(midibuffer_test::HostDouble& host) {
    clockPulse(host);
    clockPulse(host);
}

midibuffer::CaptureSnapshot snapshot(midibuffer_test::HostDouble& host) {
    return midibuffer::captureSnapshot(host.algorithm());
}

struct HistoryImage {
    midibuffer::RecordedEvent events[16];
    uint32_t count;
};

HistoryImage captureHistory(midibuffer_test::HostDouble& host) {
    HistoryImage image = {};
    const uint32_t eventCount = snapshot(host).eventCount;
    image.count = eventCount < ARRAY_SIZE(image.events)
                      ? eventCount
                      : ARRAY_SIZE(image.events);
    for (uint32_t index = 0; index < image.count; ++index) {
        expect(midibuffer::recordedEventAt(host.algorithm(), index,
                                           image.events[index]),
               "retained history can be read for byte comparison");
    }
    return image;
}

bool sameHistory(const HistoryImage& expected,
                 midibuffer_test::HostDouble& host) {
    if (snapshot(host).eventCount != expected.count) {
        return false;
    }
    for (uint32_t index = 0; index < expected.count; ++index) {
        midibuffer::RecordedEvent actual = {};
        if (!midibuffer::recordedEventAt(host.algorithm(), index, actual) ||
            actual.pulse != expected.events[index].pulse ||
            actual.offsetSamples != expected.events[index].offsetSamples ||
            actual.sourceIntervalSamples !=
                expected.events[index].sourceIntervalSamples ||
            actual.size != expected.events[index].size ||
            actual.flags != expected.events[index].flags ||
            std::memcmp(actual.bytes, expected.events[index].bytes,
                        sizeof(actual.bytes)) != 0) {
            return false;
        }
    }
    return true;
}

bool eventBytesMatch(const midibuffer::RecordedEvent& event, uint8_t status,
                     uint8_t data1, uint8_t data2, uint8_t size = 3U) {
    return event.size == size && event.bytes[0] == status &&
           event.bytes[1] == data1 &&
           (size == 2U || event.bytes[2] == data2);
}

void verifyEntryAndLifecycle(midibuffer_test::HostDouble& host) {
    expect(host.instantiate(3),
           "instance constructs through pluginEntry factory");
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
    expect(
        defaultRequirements.dram == 1000000U &&
            minimumRequirements.dram == 1000000U &&
            maximumRequirements.dram == 5000000U,
        "allocation requirements default and clamp to the approved byte range");
    expect(host.requirements().numParameters == 9,
           "stable routing controls plus three playback filters are requested");
    expect(host.requirements().dram == 3000000U,
           "selected recording bytes are requested from DRAM");
    expect(host.hostAllocatedBytes() ==
               static_cast<uint64_t>(host.requirements().sram) + 3000000U,
           "host allocation accounting includes separate metadata and events");

    const midibuffer::CaptureSnapshot initial = snapshot(host);
    expect(initial.recordingAllocationBytes == 3000000U,
           "instance fixes the selected recording allocation at construction");
    expect(initial.metadataBytes == host.requirements().sram,
           "rolling-history metadata is accounted separately in SRAM");
    expect(initial.eventCapacity ==
               3000000U / sizeof(midibuffer::RecordedEvent),
           "actual event capacity is derived and reported");
    expect(initial.eventCount == 0 && !initial.captureEnabled &&
               !initial.clockRunning,
           "fresh instances start empty, stopped, and without acquired clock");

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
           "factory registers every callback adapter");
    expect(std::strcmp(host.algorithm()->parameters[kClockParameter].name,
                       "Clock") == 0 &&
               std::strcmp(host.algorithm()->parameters[kResetParameter].name,
                           "Reset") == 0 &&
               host.algorithm()->parameters[kCaptureParameter].def == 0,
           "inputs and initially stopped capture remain in stable order");
    expect(host.algorithm()->parameters[kRecordingChannelParameter].min == 0 &&
               host.algorithm()->parameters[kRecordingChannelParameter].max ==
                   16 &&
               std::strcmp(host.algorithm()
                               ->parameters[kRecordingChannelParameter]
                               .enumStrings[0],
                           "Omni") == 0,
           "recording channel offers Omni and channels 1 through 16");
    expect(host.algorithm()->parameters[kPlaybackDestinationParameter].min ==
                   0 &&
               host.algorithm()->parameters[kPlaybackDestinationParameter]
                       .max == 4 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackDestinationParameter]
                               .enumStrings[0],
                           "Breakout") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackDestinationParameter]
                               .enumStrings[1],
                           "USB") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackDestinationParameter]
                               .enumStrings[2],
                           "Select Bus") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackDestinationParameter]
                               .enumStrings[3],
                           "Internal") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackDestinationParameter]
                               .enumStrings[4],
                           "All") == 0,
           "playback destination exposes all five approved choices");
    expect(host.algorithm()->parameters[kPlaybackChannelParameter].min == 0 &&
               host.algorithm()->parameters[kPlaybackChannelParameter].max ==
                   16 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackChannelParameter]
                               .enumStrings[0],
                           "Original") == 0,
           "playback channel defaults to Original and offers 1 through 16");
    expect(std::strcmp(host.algorithm()
                           ->parameters[kFilterControlChangeParameter]
                           .name,
                       "Filter CC") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kFilterPitchBendParameter]
                               .name,
                           "Filter Pitch Bend") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kFilterAftertouchParameter]
                               .name,
                           "Filter Aftertouch") == 0 &&
               host.algorithm()
                       ->parameters[kFilterControlChangeParameter]
                       .def == 0 &&
               host.algorithm()->parameters[kFilterPitchBendParameter].def ==
                   0 &&
               host.algorithm()->parameters[kFilterAftertouchParameter].def ==
                   0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kFilterAftertouchParameter]
                               .enumStrings[0],
                           "Off") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kFilterAftertouchParameter]
                               .enumStrings[1],
                           "On") == 0,
           "CC, bend, and combined aftertouch filters default off so all "
           "expression plays");
    const _NT_parameterPages* pages = host.algorithm()->parameterPages;
    expect(pages->numPages == 3 && pages->pages[1].numParams == 2 &&
               pages->pages[2].numParams == 5 &&
               pages->pages[2].params[2] ==
                   kFilterControlChangeParameter &&
               pages->pages[2].params[3] == kFilterPitchBendParameter &&
               pages->pages[2].params[4] == kFilterAftertouchParameter,
           "expressive controls exist only as three playback-page filters");
}

void verifyBoundaryCallbacks(midibuffer_test::HostDouble& host) {
    changeParameter(host, kClockParameter, 1);
    changeParameter(host, kResetParameter, 2);
    host.clearFrames();
    host.bus(1)[1] = 2.0f;
    host.bus(1)[2] = 2.0f;
    host.bus(2)[5] = 5.0f;

    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    host.step(2);
    sendMidi(host, 0x92, 60, 100);
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
    expect(drawContains("Capture: Stopped"),
           "draw reports explicit capture state");
    expect(drawContains("Events: 0"),
           "MIDI remains unrecorded while capture is stopped");
    expect(drawContains("Capacity: 125000"),
           "draw reports actual event capacity for the selected allocation");
    expect(
        midibuffer_test::heapAllocationCount() == allocationsBefore,
        "real-time, MIDI, UI, and draw callbacks perform no heap allocation");
}

void verifyClockedCaptureAndReacquisition() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "clock/capture trace host constructs");
    startCapture(host);

    sendMidi(host, 0x90, 48, 100);
    expect(snapshot(host).eventCount == 0,
           "enabled capture rejects MIDI before any clock");

    clockPulse(host);
    sendMidi(host, 0x90, 49, 100);
    expect(snapshot(host).eventCount == 0,
           "first acquisition pulse does not admit unknown-tempo MIDI");

    clockPulse(host);
    sendMidi(host, 0x90, 50, 100);
    midibuffer::CaptureSnapshot acquired = snapshot(host);
    expect(acquired.clockRunning && acquired.lastClockIntervalSamples == 8 &&
               acquired.eventCount == 1,
           "second pulse measures tempo and begins enabled capture");

    midibuffer::RecordedEvent event = {};
    expect(midibuffer::recordedEventAt(host.algorithm(), 0, event) &&
               event.pulse == acquired.currentPulse &&
               event.offsetSamples == 8 && event.bytes[0] == 0x90 &&
               event.bytes[1] == 50 && event.size == 3,
           "captured events retain pulse identity and between-pulse offset");

    noClockBlock(host);
    noClockBlock(host);
    expect(!snapshot(host).clockRunning,
           "two last-measured intervals without a pulse declare clock loss");
    sendMidi(host, 0x80, 50, 0);
    expect(snapshot(host).eventCount == 1,
           "enabled capture rejects MIDI after clock loss");

    clockPulse(host);
    sendMidi(host, 0x90, 51, 100);
    expect(!snapshot(host).clockRunning && snapshot(host).eventCount == 1,
           "first returning pulse does not resume capture");
    clockPulse(host);
    sendMidi(host, 0x90, 52, 100);
    expect(snapshot(host).clockRunning && snapshot(host).eventCount == 2,
           "second returning pulse freshly measures tempo and resumes capture");

    stopCapture(host);
    sendMidi(host, 0x90, 53, 100);
    expect(snapshot(host).eventCount == 4,
           "Stop Capture stores outstanding endings and rejects later MIDI");

    // Clock tracking continues while stopped, so restarting capture does not
    // require another pair of pulses while the measured interval remains valid.
    clockPulse(host);
    startCapture(host);
    sendMidi(host, 0x90, 54, 100);
    expect(snapshot(host).eventCount == 5,
           "Start Capture uses a continuously tracked valid clock immediately");
}

void verifyChannelAndEventEligibility() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "eligibility trace host constructs");
    startCapture(host);
    acquireClock(host);

    for (uint8_t channel = 0; channel < 16; ++channel) {
        sendMidi(host, static_cast<uint8_t>(0x90U | channel),
                 static_cast<uint8_t>(48U + channel), 100);
    }
    expect(snapshot(host).eventCount == 16,
           "Omni capture accepts eligible events on all 16 channels");

    bool everySpecificChannelWorked = true;
    for (uint8_t selectedChannel = 1; selectedChannel <= 16;
         ++selectedChannel) {
        changeParameter(host, kRecordingChannelParameter, selectedChannel);
        const uint32_t beforeChannel = snapshot(host).eventCount;
        for (uint8_t incomingChannel = 0; incomingChannel < 16;
             ++incomingChannel) {
            sendMidi(host, static_cast<uint8_t>(0x80U | incomingChannel), 60,
                     0);
        }
        const midibuffer::CaptureSnapshot afterChannel = snapshot(host);
        midibuffer::RecordedEvent accepted = {};
        everySpecificChannelWorked =
            everySpecificChannelWorked &&
            afterChannel.eventCount == beforeChannel + 1U &&
            midibuffer::recordedEventAt(host.algorithm(), beforeChannel,
                                        accepted) &&
            (accepted.bytes[0] & 0x0fU) == selectedChannel - 1U;
    }
    expect(
        everySpecificChannelWorked,
        "each channel-specific setting 1 through 16 accepts only its channel");

    changeParameter(host, kRecordingChannelParameter, 0);
    const uint32_t beforeEligibleTypes = snapshot(host).eventCount;
    sendMidi(host, 0x80, 60, 0);
    sendMidi(host, 0x90, 60, 100);
    sendMidi(host, 0xA0, 60, 50);
    sendMidi(host, 0xB0, 1, 64);
    sendMidi(host, 0xD0, 70, 0);
    sendMidi(host, 0xE0, 0, 64);
    expect(snapshot(host).eventCount == beforeEligibleTypes + 6,
           "notes, CC, pitch bend, and both pressure types are retained");

    const uint32_t beforeExcludedTypes = snapshot(host).eventCount;
    sendMidi(host, 0xC0, 10, 0);
    const uint8_t excludedControllers[] = {
        6, 38, 96, 97, 98, 99, 100, 101, 120, 121, 122, 123, 124, 125, 126, 127,
    };
    for (size_t index = 0; index < ARRAY_SIZE(excludedControllers); ++index) {
        sendMidi(host, 0xB0, excludedControllers[index], 1);
    }
    host.factory()->midiRealtime(host.algorithm(), 0xF8);
    host.factory()->midiRealtime(host.algorithm(), 0xFA);
    host.factory()->midiRealtime(host.algorithm(), 0xFC);
    expect(snapshot(host).eventCount == beforeExcludedTypes,
           "program, realtime transport, Channel Mode, and RPN/NRPN data are "
           "excluded");

    midibuffer::RecordedEvent channelPressure = {};
    expect(midibuffer::recordedEventAt(
               host.algorithm(), beforeEligibleTypes + 4, channelPressure) &&
               channelPressure.bytes[0] == 0xD0 && channelPressure.size == 2,
           "two-byte channel pressure retains its MIDI message size");
}

void verifyCaptureStopEndings() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "capture-ending trace host constructs");
    startCapture(host);
    acquireClock(host);

    sendMidi(host, 0x90, 60, 100);
    sendMidi(host, 0x90, 60, 110);
    sendMidi(host, 0x80, 60, 0);
    sendMidi(host, 0x91, 61, 100);
    sendMidi(host, 0xB0, 64, 127);
    sendMidi(host, 0xB1, 64, 127);
    sendMidi(host, 0xB1, 64, 0);
    sendMidi(host, 0xB2, 65, 127);
    sendMidi(host, 0xB2, 66, 127);
    sendMidi(host, 0xB2, 67, 127);
    sendMidi(host, 0xB2, 68, 127);
    sendMidi(host, 0xB2, 69, 127);
    sendMidi(host, 0xBB, 66, 127);
    sendMidi(host, 0xBB, 66, 0);
    sendMidi(host, 0xB3, 1, 20);
    sendMidi(host, 0xB3, 33, 5);
    sendMidi(host, 0xBC, 33, 5);
    sendMidi(host, 0xBC, 33, 0);
    sendMidi(host, 0xB4, 11, 50);
    sendMidi(host, 0xB4, 43, 50);
    sendMidi(host, 0xBD, 43, 50);
    sendMidi(host, 0xBD, 43, 127);
    sendMidi(host, 0xE5, 1, 65);
    sendMidi(host, 0xE6, 0, 64);
    sendMidi(host, 0xD7, 80, 0);
    sendMidi(host, 0xD8, 80, 0);
    sendMidi(host, 0xD8, 0, 0);
    sendMidi(host, 0xA9, 70, 40);
    sendMidi(host, 0xA9, 71, 40);
    sendMidi(host, 0xA9, 71, 0);

    // These retained settings have no universally safe capture-stop reset.
    sendMidi(host, 0xBA, 7, 50);
    sendMidi(host, 0xBA, 10, 20);
    sendMidi(host, 0xBA, 74, 30);
    sendMidi(host, 0xBA, 91, 40);
    const uint32_t retainedBeforeStop = snapshot(host).eventCount;
    sendMidi(host, 0xB0, 6, 99);
    sendMidi(host, 0xB0, 98, 99);
    sendMidi(host, 0xB0, 120, 99);
    expect(snapshot(host).eventCount == retainedBeforeStop,
           "excluded RPN/NRPN and Channel Mode events never enter state");

    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    midibuffer_test::resetTrace();
    stopCapture(host);
    const midibuffer::CaptureSnapshot stopped = snapshot(host);
    expect(!stopped.captureEnabled &&
               stopped.eventCount == retainedBeforeStop + 15U,
           "manual stop appends exactly the targeted finite endings");
    expect(midibuffer_test::trace().midiCallCount == 0,
           "capture stop stores endings without sending live MIDI");

    const uint8_t expectedEndings[][4] = {
        {0x80, 60, 0, 3},   {0x81, 61, 0, 3},  {0xB0, 64, 0, 3},
        {0xB2, 65, 0, 3},   {0xB2, 66, 0, 3},  {0xB2, 67, 0, 3},
        {0xB2, 68, 0, 3},   {0xB2, 69, 0, 3},  {0xB3, 1, 0, 3},
        {0xB3, 33, 0, 3},   {0xB4, 11, 127, 3},
        {0xB4, 43, 127, 3}, {0xE5, 0, 64, 3},  {0xD7, 0, 0, 2},
        {0xA9, 70, 0, 3},
    };
    midibuffer::RecordedEvent firstEnding = {};
    bool storedEndingsMatch = midibuffer::recordedEventAt(
        host.algorithm(), retainedBeforeStop, firstEnding);
    for (uint32_t index = 0; index < ARRAY_SIZE(expectedEndings); ++index) {
        midibuffer::RecordedEvent event = {};
        storedEndingsMatch =
            midibuffer::recordedEventAt(host.algorithm(),
                                        retainedBeforeStop + index, event) &&
            event.pulse == firstEnding.pulse &&
            event.offsetSamples == firstEnding.offsetSamples &&
            event.sourceIntervalSamples ==
                firstEnding.sourceIntervalSamples &&
            eventBytesMatch(event, expectedEndings[index][0],
                            expectedEndings[index][1],
                            expectedEndings[index][2],
                            expectedEndings[index][3]) &&
            storedEndingsMatch;
    }
    expect(storedEndingsMatch && firstEnding.pulse == stopped.currentPulse &&
               firstEnding.flags != 0U,
           "notes and only non-neutral pedal, modulation, expression, bend, "
           "and pressure state end at one final timestamp");

    expect(midibuffer::setPulseSelection(host.algorithm(),
                                         stopped.currentPulse,
                                         stopped.currentPulse + 1U),
           "the finalized current pulse is immediately selectable");
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "final capture interval arms through the production scheduler");
    clockPulse(host);
    clockPulse(host);
    const midibuffer_test::Trace& replay = midibuffer_test::trace();
    bool replayedEndingsMatch =
        replay.midiCallCount == retainedBeforeStop + 15U;
    for (size_t index = 0; index < ARRAY_SIZE(expectedEndings); ++index) {
        const size_t callIndex = retainedBeforeStop + index;
        if (callIndex >= replay.midiCallCount) {
            replayedEndingsMatch = false;
            continue;
        }
        replayedEndingsMatch =
            replay.midiCalls[callIndex].bytes[0] ==
                expectedEndings[index][0] &&
            replay.midiCalls[callIndex].bytes[1] ==
                expectedEndings[index][1] &&
            replay.midiCalls[callIndex].size == expectedEndings[index][3] &&
            (replay.midiCalls[callIndex].size == 2U ||
             replay.midiCalls[callIndex].bytes[2] ==
                 expectedEndings[index][2]) &&
            replay.midiCalls[callIndex].dispatchSample ==
                replay.midiCalls[retainedBeforeStop].dispatchSample &&
            replayedEndingsMatch;
    }
    expect(replayedEndingsMatch,
           "stored endings replay together through the completed scheduler");

    midibuffer::stopPlayback(host.algorithm());
    changeParameter(host, kFilterControlChangeParameter, 1);
    changeParameter(host, kFilterPitchBendParameter, 1);
    changeParameter(host, kFilterAftertouchParameter, 1);
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "capture endings remain schedulable with expression filtered");
    clockPulse(host);
    clockPulse(host);
    const midibuffer_test::Trace& filteredReplay = midibuffer_test::trace();
    bool filteredEndingsMatch = filteredReplay.midiCallCount == 19U;
    for (size_t index = 0; index < ARRAY_SIZE(expectedEndings); ++index) {
        const size_t callIndex = 4U + index;
        if (callIndex >= filteredReplay.midiCallCount) {
            filteredEndingsMatch = false;
            continue;
        }
        filteredEndingsMatch =
            filteredReplay.midiCalls[callIndex].bytes[0] ==
                expectedEndings[index][0] &&
            filteredReplay.midiCalls[callIndex].bytes[1] ==
                expectedEndings[index][1] &&
            filteredReplay.midiCalls[callIndex].size ==
                expectedEndings[index][3] &&
            (filteredReplay.midiCalls[callIndex].size == 2U ||
             filteredReplay.midiCalls[callIndex].bytes[2] ==
                 expectedEndings[index][2]) &&
            filteredEndingsMatch;
    }
    expect(filteredEndingsMatch,
           "playback filters cannot suppress stored performance endings");
    expect(midibuffer_test::heapAllocationCount() == allocationsBefore,
           "state finalization and ending replay perform no heap allocation");

    midibuffer_test::HostDouble automatic;
    expect(automatic.instantiate(1), "automatic capture-stop host constructs");
    startCapture(automatic);
    acquireClock(automatic);
    const uint64_t notePulse = snapshot(automatic).currentPulse;
    sendMidi(automatic, 0x92, 72, 100);
    clockPulse(automatic);
    expect(midibuffer::setPulseSelection(automatic.algorithm(), notePulse,
                                         notePulse + 1U),
           "active capture history can be selected before playback");
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(automatic.algorithm()) &&
               !snapshot(automatic).captureEnabled &&
               snapshot(automatic).eventCount == 2U,
           "playback start finalizes active capture through the same path");
    expect(midibuffer_test::trace().midiCallCount == 0,
           "automatic capture stop also emits no immediate live cleanup");

    midibuffer_test::HostDouble full;
    expect(full.instantiate(1), "full-buffer ending host constructs");
    startCapture(full);
    acquireClock(full);
    const uint32_t capacity = snapshot(full).eventCapacity;
    for (uint32_t index = 0; index < capacity; ++index) {
        sendMidi(full, 0xB0, 7, static_cast<uint8_t>(index & 0x7fU));
    }
    sendMidi(full, 0x94, 84, 100);
    midibuffer_test::resetTrace();
    stopCapture(full);
    midibuffer::RecordedEvent finalEvent = {};
    expect(snapshot(full).eventCount == capacity &&
               midibuffer::recordedEventAt(full.algorithm(), capacity - 1U,
                                           finalEvent) &&
               eventBytesMatch(finalEvent, 0x84, 84, 0),
           "a full rolling buffer retains its final note ending in budget");
    expect(midibuffer_test::trace().midiCallCount == 0,
           "full-buffer finalization remains storage-only");
}

uint64_t fillHistoryAndMeasureSpan(midibuffer_test::HostDouble& host,
                                   uint32_t eventsPerPulse,
                                   uint32_t eventTarget) {
    uint32_t emitted = 0;
    while (emitted < eventTarget) {
        clockPulse(host);
        for (uint32_t density = 0;
             density < eventsPerPulse && emitted < eventTarget; ++density) {
            sendMidi(host, 0x90, static_cast<uint8_t>(36U + (emitted % 48U)),
                     100);
            ++emitted;
        }
    }
    const midibuffer::CaptureSnapshot current = snapshot(host);
    return current.newestPulse - current.oldestPulse;
}

void verifyRollingHistoryAndSelectionInvalidation() {
    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    midibuffer_test::HostDouble sparse;
    expect(sparse.instantiate(1), "sparse rolling-history host constructs");
    startCapture(sparse);
    acquireClock(sparse);
    const uint32_t capacity = snapshot(sparse).eventCapacity;
    const uint64_t sparseSpan = fillHistoryAndMeasureSpan(sparse, 1, capacity);
    midibuffer::CaptureSnapshot full = snapshot(sparse);
    expect(full.eventCount == capacity,
           "fixed history reaches reported event capacity without resizing");

    expect(midibuffer::setPulseSelection(sparse.algorithm(), full.oldestPulse,
                                         full.oldestPulse + 1U),
           "pulse-aligned retained range can be selected");
    midibuffer::PulseRange playbackRange = {};
    expect(midibuffer::acquirePlaybackSelection(sparse.algorithm(),
                                                playbackRange) &&
               playbackRange.startPulse == full.oldestPulse,
           "playback-entry contract admits a valid retained selection");

    const uint64_t newestBeforeOverwrite = full.newestPulse;
    clockPulse(sparse);
    sendMidi(sparse, 0x90, 84, 100);
    midibuffer::CaptureSnapshot overwritten = snapshot(sparse);
    expect(overwritten.eventCount == capacity &&
               overwritten.newestPulse > newestBeforeOverwrite,
           "full history replaces its oldest event and capture continues");
    expect(!overwritten.selectionValid,
           "overwriting selected history clears the selection");
    expect(!midibuffer::acquirePlaybackSelection(sparse.algorithm(),
                                                 playbackRange),
           "playback-entry contract refuses an invalidated selection");

    clockPulse(sparse);
    sendMidi(sparse, 0x90, 85, 100);
    expect(snapshot(sparse).eventCount == capacity &&
               snapshot(sparse).newestPulse > overwritten.newestPulse,
           "capture remains active after selection invalidation");

    midibuffer_test::HostDouble dense;
    expect(dense.instantiate(1), "dense rolling-history host constructs");
    startCapture(dense);
    acquireClock(dense);
    const uint64_t denseSpan = fillHistoryAndMeasureSpan(dense, 4, capacity);
    expect(denseSpan < sparseSpan,
           "higher event density retains a shorter pulse-history duration");
    expect(midibuffer_test::heapAllocationCount() == allocationsBefore,
           "mixed-density capture and ring wrap perform no heap allocation");
}

void verifyRetainedReplayRoutingMatrix() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "retained replay routing host constructs");
    startCapture(host);
    acquireClock(host);

    const uint8_t recordedMessages[][3] = {
        {0x81, 48, 0},   {0x92, 49, 100}, {0xA3, 50, 60},
        {0xB4, 1, 72},   {0xD5, 80, 0},   {0xE6, 0, 64},
    };
    const uint8_t messageSizes[] = {3, 3, 3, 3, 2, 3};
    for (size_t index = 0; index < ARRAY_SIZE(recordedMessages); ++index) {
        sendMidi(host, recordedMessages[index][0], recordedMessages[index][1],
                 recordedMessages[index][2]);
    }
    const uint64_t recordedPulse = snapshot(host).currentPulse;
    clockPulse(host);
    stopCapture(host);
    expect(midibuffer::setPulseSelection(host.algorithm(), recordedPulse,
                                         recordedPulse + 1U),
           "retained pulse is selectable for replay routing");

    const uint32_t destinations[] = {
        kNT_destinationBreakout,
        kNT_destinationUSB,
        kNT_destinationSelectBus,
        kNT_destinationInternal,
        kNT_destinationBreakout | kNT_destinationUSB |
            kNT_destinationSelectBus | kNT_destinationInternal,
    };
    expect(destinations[4] == 0x0fU,
           "All is exactly the four pinned destination bits");

    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    bool completeMatrixMatches = true;
    for (int destinationSetting = 0; destinationSetting < 5;
         ++destinationSetting) {
        changeParameter(host, kPlaybackDestinationParameter,
                        static_cast<int16_t>(destinationSetting));
        for (int channelSetting = 0; channelSetting <= 16; ++channelSetting) {
            changeParameter(host, kPlaybackChannelParameter,
                            static_cast<int16_t>(channelSetting));
            midibuffer_test::resetTrace();
            completeMatrixMatches =
                midibuffer::startPlayback(host.algorithm()) &&
                completeMatrixMatches;
            clockPulse(host);
            clockPulse(host);
            midibuffer::stopPlayback(host.algorithm());

            const midibuffer_test::Trace& current = midibuffer_test::trace();
            completeMatrixMatches =
                current.midiCallCount == ARRAY_SIZE(recordedMessages) + 1U &&
                completeMatrixMatches;
            const size_t callCount =
                current.midiCallCount < ARRAY_SIZE(recordedMessages)
                    ? current.midiCallCount
                    : ARRAY_SIZE(recordedMessages);
            for (size_t index = 0; index < callCount; ++index) {
                const uint8_t expectedStatus =
                    channelSetting == 0
                        ? recordedMessages[index][0]
                        : static_cast<uint8_t>(
                              (recordedMessages[index][0] & 0xf0U) |
                              static_cast<uint8_t>(channelSetting - 1));
                completeMatrixMatches =
                    current.midiCalls[index].destination ==
                            destinations[destinationSetting] &&
                    current.midiCalls[index].size == messageSizes[index] &&
                    current.midiCalls[index].bytes[0] == expectedStatus &&
                    current.midiCalls[index].bytes[1] ==
                        recordedMessages[index][1] &&
                    (messageSizes[index] == 2 ||
                     current.midiCalls[index].bytes[2] ==
                         recordedMessages[index][2]) &&
                    completeMatrixMatches;
            }
            if (current.midiCallCount > ARRAY_SIZE(recordedMessages)) {
                const midibuffer_test::MidiCall& cleanup =
                    current.midiCalls[ARRAY_SIZE(recordedMessages)];
                const uint8_t expectedCleanupStatus =
                    channelSetting == 0
                        ? 0x82U
                        : static_cast<uint8_t>(0x80U | channelSetting - 1U);
                completeMatrixMatches =
                    cleanup.destination == destinations[destinationSetting] &&
                    cleanup.bytes[0] == expectedCleanupStatus &&
                    cleanup.bytes[1] == 49U && cleanup.bytes[2] == 0U &&
                    completeMatrixMatches;
            }
        }
    }
    expect(completeMatrixMatches,
           "retained note, CC, pitch bend, and pressure traces route only to "
           "each selected destination and preserve or override all channels; "
           "manual stop releases the held routed note");
    expect(midibuffer_test::heapAllocationCount() == allocationsBefore,
           "clock-driven retained replay performs no heap allocation");

    midibuffer_test::resetTrace();
    clockPulse(host);
    expect(midibuffer_test::trace().midiCallCount == 0,
           "stopped playback emits no retained events on later clock pulses");
}

void verifyRecoverableExpressionFiltering() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "expression-filter trace host constructs");

    // Playback filters are deliberately already on while this one history is
    // captured. Eligible expression must still be retained.
    changeParameter(host, kFilterControlChangeParameter, 1);
    changeParameter(host, kFilterPitchBendParameter, 1);
    changeParameter(host, kFilterAftertouchParameter, 1);
    startCapture(host);
    acquireClock(host);

    const uint8_t recordedMessages[][3] = {
        {0x90, 60, 100}, {0x80, 60, 0}, {0xB1, 1, 0}, {0xB2, 64, 0},
        {0xE3, 0, 64},   {0xA4, 60, 0}, {0xD5, 0, 0},
    };
    const uint8_t messageSizes[] = {3, 3, 3, 3, 3, 3, 2};
    for (size_t index = 0; index < ARRAY_SIZE(recordedMessages); ++index) {
        sendMidi(host, recordedMessages[index][0], recordedMessages[index][1],
                 recordedMessages[index][2]);
    }

    const uint32_t retainedEligibleCount = snapshot(host).eventCount;
    sendMidi(host, 0xC0, 9, 0);
    sendMidi(host, 0xF0, 1, 2);
    const uint8_t excludedControllers[] = {
        6, 38, 96, 97, 98, 99, 100, 101, 120, 121, 122, 123, 124, 125, 126, 127,
    };
    for (size_t index = 0; index < ARRAY_SIZE(excludedControllers); ++index) {
        sendMidi(host, 0xB0, excludedControllers[index], 99);
    }
    const uint8_t excludedRealtime[] = {0xF8, 0xFA, 0xFB, 0xFC};
    for (size_t index = 0; index < ARRAY_SIZE(excludedRealtime); ++index) {
        host.factory()->midiRealtime(host.algorithm(), excludedRealtime[index]);
    }
    expect(retainedEligibleCount == ARRAY_SIZE(recordedMessages) &&
               snapshot(host).eventCount == retainedEligibleCount &&
               host.factory()->midiSysEx == NULL,
           "capture retains notes and eligible expression while filters are on "
           "but excludes program, SysEx, clock/transport, Channel Mode, and "
           "stateful parameter CCs");

    const uint64_t recordedPulse = snapshot(host).currentPulse;
    const HistoryImage recordedHistory = captureHistory(host);
    clockPulse(host);
    stopCapture(host);
    expect(midibuffer::setPulseSelection(host.algorithm(), recordedPulse,
                                         recordedPulse + 1U),
           "single expression history is selectable for filter replay matrix");

    bool allFilterCombinationsMatch = true;
    for (uint8_t mask = 0; mask < 8U; ++mask) {
        changeParameter(host, kFilterControlChangeParameter,
                        (mask & 1U) != 0U ? 1 : 0);
        changeParameter(host, kFilterPitchBendParameter,
                        (mask & 2U) != 0U ? 1 : 0);
        changeParameter(host, kFilterAftertouchParameter,
                        (mask & 4U) != 0U ? 1 : 0);
        midibuffer_test::resetTrace();
        allFilterCombinationsMatch =
            midibuffer::startPlayback(host.algorithm()) &&
            allFilterCombinationsMatch;
        clockPulse(host);
        clockPulse(host);
        midibuffer::stopPlayback(host.algorithm());

        size_t emittedIndex = 0;
        const midibuffer_test::Trace& current = midibuffer_test::trace();
        for (size_t eventIndex = 0;
             eventIndex < ARRAY_SIZE(recordedMessages); ++eventIndex) {
            const uint8_t type = recordedMessages[eventIndex][0] & 0xf0U;
            const bool filtered =
                (type == 0xb0U && (mask & 1U) != 0U) ||
                (type == 0xe0U && (mask & 2U) != 0U) ||
                ((type == 0xa0U || type == 0xd0U) && (mask & 4U) != 0U);
            if (filtered) {
                continue;
            }
            if (emittedIndex >= current.midiCallCount) {
                allFilterCombinationsMatch = false;
                continue;
            }
            const midibuffer_test::MidiCall& call =
                current.midiCalls[emittedIndex++];
            allFilterCombinationsMatch =
                call.destination == kNT_destinationBreakout &&
                call.size == messageSizes[eventIndex] &&
                call.bytes[0] == recordedMessages[eventIndex][0] &&
                call.bytes[1] == recordedMessages[eventIndex][1] &&
                (call.size == 2U ||
                 call.bytes[2] == recordedMessages[eventIndex][2]) &&
                allFilterCombinationsMatch;
        }
        allFilterCombinationsMatch =
            emittedIndex == current.midiCallCount &&
            sameHistory(recordedHistory, host) && allFilterCombinationsMatch;
    }
    expect(allFilterCombinationsMatch,
           "all eight playback-filter combinations suppress only their grouped "
           "eligible expression and leave retained history byte-identical");

    // Safety dispatch intentionally bypasses recorded-event filtering and the
    // playback channel override while retaining the selected destination.
    changeParameter(host, kPlaybackDestinationParameter, 1);
    changeParameter(host, kPlaybackChannelParameter, 16);
    changeParameter(host, kFilterControlChangeParameter, 1);
    changeParameter(host, kFilterPitchBendParameter, 1);
    changeParameter(host, kFilterAftertouchParameter, 1);
    midibuffer_test::resetTrace();
    midibuffer::dispatchSafetyMidi3(host.algorithm(), 0xB2, 64, 0);
    midibuffer::dispatchSafetyMidi3(host.algorithm(), 0x82, 60, 0);
    const midibuffer_test::Trace& safety = midibuffer_test::trace();
    expect(safety.midiCallCount == 2 &&
               safety.midiCalls[0].destination == kNT_destinationUSB &&
               safety.midiCalls[0].bytes[0] == 0xB2 &&
               safety.midiCalls[0].bytes[1] == 64 &&
               safety.midiCalls[0].bytes[2] == 0 &&
               safety.midiCalls[1].destination == kNT_destinationUSB &&
               safety.midiCalls[1].bytes[0] == 0x82,
           "safety CC and note-off dispatch bypasses expressive filters and "
           "recorded-event channel rewriting");

    changeParameter(host, kPlaybackDestinationParameter, 0);
    changeParameter(host, kPlaybackChannelParameter, 0);
    changeParameter(host, kFilterControlChangeParameter, 0);
    changeParameter(host, kFilterPitchBendParameter, 0);
    changeParameter(host, kFilterAftertouchParameter, 0);
    midibuffer_test::resetTrace();
    const bool restarted = midibuffer::startPlayback(host.algorithm());
    clockPulse(host);
    clockPulse(host);
    midibuffer::stopPlayback(host.algorithm());
    const midibuffer_test::Trace& restored = midibuffer_test::trace();
    bool restoredBytesMatch = restarted &&
                              restored.midiCallCount ==
                                  ARRAY_SIZE(recordedMessages);
    for (size_t index = 0;
         index < restored.midiCallCount &&
         index < ARRAY_SIZE(recordedMessages);
         ++index) {
        restoredBytesMatch =
            restored.midiCalls[index].size == messageSizes[index] &&
            std::memcmp(restored.midiCalls[index].bytes,
                        recordedMessages[index], messageSizes[index]) == 0 &&
            restoredBytesMatch;
    }
    expect(restoredBytesMatch && sameHistory(recordedHistory, host),
           "turning every filter back off restores exact expression output "
           "from the unchanged recorded bytes");
}

struct TimingFixture {
    uint64_t lastPulseSample;
};

TimingFixture prepareTimingHistory(midibuffer_test::HostDouble& host) {
    TimingFixture fixture = {};
    expect(host.instantiate(1), "timing trace host constructs");
    startCapture(host);

    expect(clockPulseAt(host, 0), "timing capture receives first pulse");
    expect(clockPulseAt(host, 32),
           "timing capture acquires a 32-sample source interval");
    sendMidi(host, 0x90, 60, 100);
    noClockBlock(host);
    sendMidi(host, 0x80, 60, 0);

    expect(clockPulseAt(host, 63),
           "timing capture receives a near-block-boundary pulse");
    sendMidi(host, 0x90, 67, 100);
    expect(clockPulseAt(host, 95),
           "timing capture closes the selected source range on a pulse");
    stopCapture(host);

    midibuffer::RecordedEvent quarter = {};
    midibuffer::RecordedEvent halfway = {};
    midibuffer::RecordedEvent aligned = {};
    expect(midibuffer::recordedEventAt(host.algorithm(), 0, quarter) &&
               quarter.pulse == 2 && quarter.offsetSamples == 8 &&
               quarter.sourceIntervalSamples == 32,
           "quarter-position input retains its source interval and offset");
    expect(midibuffer::recordedEventAt(host.algorithm(), 1, halfway) &&
               halfway.pulse == 2 && halfway.offsetSamples == 16 &&
               halfway.sourceIntervalSamples == 32,
           "halfway input retains its fractional source timing");
    expect(midibuffer::recordedEventAt(host.algorithm(), 2, aligned) &&
               aligned.pulse == 3 && aligned.offsetSamples == 0,
           "finest observable post-pulse callback is retained as aligned");
    expect(midibuffer::setPulseSelection(host.algorithm(), 2, 4),
           "timing selection uses pulse-aligned inclusive/exclusive bounds");
    fixture.lastPulseSample = 95;
    return fixture;
}

void primeSteadyClock(midibuffer_test::HostDouble& host,
                      TimingFixture& fixture, uint32_t interval) {
    for (uint32_t index = 0; index < 8U; ++index) {
        fixture.lastPulseSample += interval;
        expect(clockPulseAt(host, fixture.lastPulseSample),
               "steady clock priming pulse is delivered");
    }
    const midibuffer::CaptureSnapshot current = snapshot(host);
    expect(current.clockAverageIntervalCount == 8U &&
               current.predictedClockIntervalSamples == interval,
           "eight-interval running window converges to the steady tempo");
}

uint64_t absoluteError(uint64_t actual, uint64_t expected) {
    return actual >= expected ? actual - expected : expected - actual;
}

uint64_t verifyProportionalTrace(uint32_t interval) {
    midibuffer_test::HostDouble host;
    TimingFixture fixture = prepareTimingHistory(host);
    primeSteadyClock(host, fixture, interval);

    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "valid timing selection arms playback");
    expect(snapshot(host).playbackArmed && !snapshot(host).playbackActive,
           "known-tempo playback remains armed until the next received pulse");

    const uint64_t startPulse = fixture.lastPulseSample + interval;
    expect(clockPulseAt(host, startPulse),
           "known-tempo playback starts on the next actual pulse");
    expect(snapshot(host).playbackActive &&
               snapshot(host).playbackPulse == 2,
           "selection start is active on its pulse-aligned boundary");
    stepThrough(host, startPulse + interval / 2U);
    expect(clockPulseAt(host, startPulse + interval),
           "second selected source pulse follows the next actual pulse");

    const midibuffer_test::Trace& current = midibuffer_test::trace();
    const uint64_t expectedSamples[] = {
        startPulse + interval / 4U,
        startPulse + interval / 2U,
        startPulse + interval,
    };
    const uint8_t expectedStatuses[] = {0x90, 0x80, 0x90};
    bool traceMatches = current.midiCallCount == 3U;
    uint64_t maximumError = 0;
    for (size_t index = 0; index < 3U && index < current.midiCallCount;
         ++index) {
        const uint64_t error = absoluteError(
            current.midiCalls[index].dispatchSample, expectedSamples[index]);
        if (error > maximumError) {
            maximumError = error;
        }
        traceMatches =
            current.midiCalls[index].dispatchSample == expectedSamples[index] &&
            current.midiCalls[index].bytes[0] == expectedStatuses[index] &&
            traceMatches;
    }
    expect(traceMatches,
           "quarter, halfway, and aligned events retain proportional positions "
           "through the timestamped NT output adapter");

    expect(clockPulseAt(host, startPulse + 2U * interval),
           "selection end arrives on an actual pulse");
    expect(snapshot(host).playbackPulse == 2,
           "pulse-aligned selection end wraps exactly to its start boundary");
    midibuffer::stopPlayback(host.algorithm());
    return maximumError;
}

void verifyRunningAverageAndProportionalScheduling() {
    midibuffer_test::HostDouble averaging;
    expect(averaging.instantiate(1), "clock-average trace host constructs");
    expect(clockPulseAt(averaging, 0) && clockPulseAt(averaging, 16) &&
               clockPulseAt(averaging, 40),
           "clock-average trace receives 16- and 24-sample intervals");
    const midibuffer::CaptureSnapshot mixed = snapshot(averaging);
    expect(mixed.lastClockIntervalSamples == 24 &&
               mixed.predictedClockIntervalSamples == 20 &&
               mixed.clockAverageIntervalCount == 2,
           "prediction is the rounded running average, not the latest interval");
    while (snapshot(averaging).clockRunning) {
        noClockBlock(averaging);
    }
    expect(snapshot(averaging).predictedClockIntervalSamples == 0 &&
               snapshot(averaging).clockAverageIntervalCount == 0,
           "clock loss discards the complete averaging window");

    const uint64_t errorAt16 = verifyProportionalTrace(16);
    const uint64_t errorAt24 = verifyProportionalTrace(24);
    const uint64_t maximumError =
        errorAt16 > errorAt24 ? errorAt16 : errorAt24;
    expect(maximumError == 0,
           "native 48 kHz scheduler measurements match modeled due samples");
    std::printf("MEASURE: deterministic native NT-adapter scheduler max error "
                "= %llu samples across 16- and 24-sample steady intervals "
                "(not physical-hardware latency)\n",
                static_cast<unsigned long long>(maximumError));
}

void verifyEarlyPulseCatchUpOrder() {
    midibuffer_test::HostDouble host;
    TimingFixture fixture = prepareTimingHistory(host);
    primeSteadyClock(host, fixture, 32);
    noClockBlock(host);

    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "early-pulse trace arms playback");
    const uint64_t firstPulse = host.elapsedSamples();
    host.clearFrames();
    host.bus(1)[0] = 2.0f;
    host.bus(1)[4] = 2.0f;
    host.step(2);

    const midibuffer_test::Trace& current = midibuffer_test::trace();
    expect(current.midiCallCount == 3 &&
               current.midiCalls[0].dispatchSample == firstPulse + 4U &&
               current.midiCalls[0].bytes[0] == 0x90 &&
               current.midiCalls[0].bytes[1] == 60 &&
               current.midiCalls[1].dispatchSample == firstPulse + 4U &&
               current.midiCalls[1].bytes[0] == 0x80 &&
               current.midiCalls[1].bytes[1] == 60 &&
               current.midiCalls[2].dispatchSample == firstPulse + 4U &&
               current.midiCalls[2].bytes[0] == 0x90 &&
               current.midiCalls[2].bytes[1] == 67,
           "early pulse catches up pending note-on/note-off in recorded order "
           "before the next pulse-aligned event");
}

void verifyPlaybackClockAcquisition() {
    midibuffer_test::HostDouble known;
    TimingFixture knownFixture = prepareTimingHistory(known);
    primeSteadyClock(known, knownFixture, 16);
    expect(midibuffer::setPulseSelection(known.algorithm(), 3, 4),
           "known-tempo acquisition selects an aligned event");
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(known.algorithm()),
           "known-tempo aligned playback arms");
    const uint64_t knownStart = knownFixture.lastPulseSample + 16U;
    expect(clockPulseAt(known, knownStart) &&
               midibuffer_test::trace().midiCallCount == 1 &&
               midibuffer_test::trace().midiCalls[0].dispatchSample ==
                   knownStart,
           "valid tracked tempo starts armed playback on the next pulse");

    midibuffer_test::HostDouble unknown;
    TimingFixture unknownFixture = prepareTimingHistory(unknown);
    primeSteadyClock(unknown, unknownFixture, 16);
    expect(midibuffer::setPulseSelection(unknown.algorithm(), 3, 4),
           "unknown-tempo acquisition selects an aligned event");
    while (snapshot(unknown).clockRunning) {
        noClockBlock(unknown);
    }
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(unknown.algorithm()) &&
               snapshot(unknown).playbackArmed,
           "lost-tempo playback remains armed without emitting");
    const uint64_t firstReturn = unknown.elapsedSamples();
    expect(clockPulseAt(unknown, firstReturn) &&
               midibuffer_test::trace().midiCallCount == 0 &&
               snapshot(unknown).playbackArmed,
           "first acquisition pulse emits no unknown-tempo playback");
    const uint64_t secondReturn = firstReturn + 16U;
    expect(clockPulseAt(unknown, secondReturn) &&
               midibuffer_test::trace().midiCallCount == 1 &&
               midibuffer_test::trace().midiCalls[0].dispatchSample ==
                   secondReturn &&
               snapshot(unknown).playbackActive,
           "second acquisition pulse measures tempo and starts playback there");
}

struct TransportFixture {
    uint64_t firstSelectedPulse;
    uint64_t lastClockSample;
};

TransportFixture prepareTransportHistory(midibuffer_test::HostDouble& host,
                                         uint32_t interval = 16U) {
    TransportFixture fixture = {};
    expect(host.instantiate(1), "transport trace host constructs");
    startCapture(host);
    expect(clockPulseAt(host, 0), "transport history receives first pulse");
    expect(clockPulseAt(host, interval),
           "transport history acquires its source tempo");
    fixture.firstSelectedPulse = snapshot(host).currentPulse;
    sendMidi(host, 0x92, 60, 100);
    sendMidi(host, 0xB2, 64, 127);
    expect(clockPulseAt(host, interval * 2U),
           "transport history advances to its release pulse");
    sendMidi(host, 0x82, 60, 0);
    sendMidi(host, 0xB2, 64, 0);
    expect(clockPulseAt(host, interval * 3U),
           "transport history closes its selected range");
    fixture.lastClockSample = static_cast<uint64_t>(interval) * 3U;
    stopCapture(host);
    expect(midibuffer::setPulseSelection(
               host.algorithm(), fixture.firstSelectedPulse,
               fixture.firstSelectedPulse + 2U),
           "two-pulse transport history is selectable");
    return fixture;
}

void beginTransportOnHeldFirstBeat(midibuffer_test::HostDouble& host,
                                   TransportFixture& fixture,
                                   uint32_t interval = 16U,
                                   uint8_t outputChannel = 2U) {
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "transport playback arms");
    fixture.lastClockSample += interval;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "transport playback receives its start pulse");
    stepThrough(host, fixture.lastClockSample + interval / 2U);
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    expect(current.midiCallCount == 2U &&
               current.midiCalls[0].bytes[0] ==
                   static_cast<uint8_t>(0x90U | outputChannel) &&
               current.midiCalls[0].bytes[1] == 60U &&
               current.midiCalls[1].bytes[0] ==
                   static_cast<uint8_t>(0xB0U | outputChannel) &&
               current.midiCalls[1].bytes[1] == 64U &&
               current.midiCalls[1].bytes[2] == 127U,
           "first selected beat leaves one playback note and sustain held");
}

void verifyPlaybackCaptureExclusionAndManualStop() {
    midibuffer_test::HostDouble captureHost;
    expect(captureHost.instantiate(1),
           "playback/capture exclusion host constructs");
    startCapture(captureHost);
    acquireClock(captureHost);
    const uint64_t selectedPulse = snapshot(captureHost).currentPulse;
    sendMidi(captureHost, 0x90, 48, 100);
    clockPulse(captureHost);
    expect(midibuffer::setPulseSelection(captureHost.algorithm(),
                                         selectedPulse,
                                         selectedPulse + 1U),
           "active capture range is selected before playback entry");
    expect(midibuffer::startPlayback(captureHost.algorithm()),
           "playback entry finalizes active capture");
    const HistoryImage finalizedHistory = captureHistory(captureHost);
    expect(!snapshot(captureHost).captureEnabled &&
               finalizedHistory.count == 2U,
           "playback entry pauses capture and stores its outstanding ending");
    startCapture(captureHost);
    expect(!snapshot(captureHost).captureEnabled,
           "Start Capture cannot overlap enabled playback");
    sendMidi(captureHost, 0x90, 49, 100);
    clockPulse(captureHost);
    sendMidi(captureHost, 0x90, 50, 100);
    expect(sameHistory(finalizedHistory, captureHost),
           "incoming MIDI leaves finalized history stable during playback");
    midibuffer::stopPlayback(captureHost.algorithm());
    sendMidi(captureHost, 0x90, 51, 100);
    expect(!snapshot(captureHost).captureEnabled &&
               sameHistory(finalizedHistory, captureHost),
           "manual playback stop does not implicitly restart capture");
    startCapture(captureHost);
    sendMidi(captureHost, 0x90, 52, 100);
    expect(snapshot(captureHost).captureEnabled &&
               snapshot(captureHost).eventCount == finalizedHistory.count + 1U,
           "only explicit Start Capture admits MIDI after playback stop");

    midibuffer_test::HostDouble stopHost;
    TransportFixture stopFixture = prepareTransportHistory(stopHost);
    changeParameter(stopHost, kPlaybackDestinationParameter, 1);
    changeParameter(stopHost, kPlaybackChannelParameter, 5);
    beginTransportOnHeldFirstBeat(stopHost, stopFixture, 16U, 4U);
    changeParameter(stopHost, kFilterControlChangeParameter, 1);
    changeParameter(stopHost, kFilterPitchBendParameter, 1);
    changeParameter(stopHost, kFilterAftertouchParameter, 1);
    const uint64_t stoppedPulse = snapshot(stopHost).playbackPulse;
    const HistoryImage beforeStop = captureHistory(stopHost);
    midibuffer_test::resetTrace();
    midibuffer::stopPlayback(stopHost.algorithm());
    const midibuffer_test::Trace& cleanup = midibuffer_test::trace();
    expect(cleanup.midiCallCount == 2U &&
               cleanup.midiCalls[0].destination == kNT_destinationUSB &&
               cleanup.midiCalls[0].bytes[0] == 0x84U &&
               cleanup.midiCalls[0].bytes[1] == 60U &&
               cleanup.midiCalls[1].bytes[0] == 0xB4U &&
               cleanup.midiCalls[1].bytes[1] == 64U &&
               cleanup.midiCalls[1].bytes[2] == 0U,
           "manual stop immediately sends routed note-off then sustain-off "
           "through the filter-bypassing safety path");
    expect(!snapshot(stopHost).playbackActive &&
               snapshot(stopHost).playbackPulse == stoppedPulse &&
               sameHistory(beforeStop, stopHost),
           "manual stop preserves playback position and retained history");

    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(stopHost.algorithm()),
           "manual-stop playback rearms from its saved position");
    expect(clockPulseAt(stopHost, stopHost.elapsedSamples()),
           "saved-position playback receives its continuation pulse");
    stepThrough(stopHost, stopHost.elapsedSamples());
    bool noRetrigger = true;
    for (size_t index = 0; index < midibuffer_test::trace().midiCallCount;
         ++index) {
        noRetrigger = noRetrigger &&
                      (midibuffer_test::trace().midiCalls[index].bytes[0] &
                       0xf0U) != 0x90U;
    }
    expect(noRetrigger && snapshot(stopHost).playbackPulse == stoppedPulse,
           "manual restart continues the saved interval without replaying its "
           "already-consumed attack");
}

void verifyResetCleanupAndCoincidence() {
    midibuffer_test::HostDouble host;
    TransportFixture fixture = prepareTransportHistory(host);
    changeParameter(host, kPlaybackDestinationParameter, 1);
    changeParameter(host, kPlaybackChannelParameter, 5);
    beginTransportOnHeldFirstBeat(host, fixture, 16U, 4U);

    changeParameter(host, kFilterControlChangeParameter, 1);
    changeParameter(host, kFilterPitchBendParameter, 1);
    changeParameter(host, kFilterAftertouchParameter, 1);
    const HistoryImage beforeReset = captureHistory(host);
    midibuffer_test::resetTrace();
    resetPulse(host);
    const midibuffer_test::Trace& reset = midibuffer_test::trace();
    expect(reset.midiCallCount == 2U &&
               reset.midiCalls[0].destination == kNT_destinationUSB &&
               reset.midiCalls[0].bytes[0] == 0x84U &&
               reset.midiCalls[0].bytes[1] == 60U &&
               reset.midiCalls[1].bytes[0] == 0xB4U &&
               reset.midiCalls[1].bytes[1] == 64U &&
               reset.midiCalls[1].bytes[2] == 0U,
           "independent reset immediately orders note cleanup before sustain "
           "cleanup despite active expressive filters");
    expect(snapshot(host).playbackArmed &&
               snapshot(host).playbackPulse == fixture.firstSelectedPulse &&
               sameHistory(beforeReset, host),
           "reset repositions to the selection start without emitting attacks");

    changeParameter(host, kFilterControlChangeParameter, 0);
    midibuffer_test::resetTrace();
    expect(clockPulseAt(host, host.elapsedSamples()),
           "clock after independent reset opens the first selected beat");
    stepThrough(host, host.elapsedSamples());
    expect(midibuffer_test::trace().midiCallCount == 2U,
           "first beat after reset restores its note and sustain state");

    changeParameter(host, kFilterControlChangeParameter, 1);
    midibuffer_test::resetTrace();
    coincidentResetAndClockPulse(host);
    noClockBlock(host);
    const midibuffer_test::Trace& coincident = midibuffer_test::trace();
    expect(coincident.midiCallCount == 3U &&
               coincident.midiCalls[0].bytes[0] == 0x84U &&
               coincident.midiCalls[1].bytes[0] == 0xB4U &&
               coincident.midiCalls[1].bytes[1] == 64U &&
               coincident.midiCalls[1].bytes[2] == 0U &&
               coincident.midiCalls[2].bytes[0] == 0x94U &&
               coincident.midiCalls[2].bytes[1] == 60U,
           "coincident reset and clock emits note-off then sustain-off before "
           "the loop's unskipped first-beat attack");
}

void verifyClockLossCleanupAndContinuation() {
    midibuffer_test::HostDouble host;
    TransportFixture fixture = prepareTransportHistory(host);
    beginTransportOnHeldFirstBeat(host, fixture);
    changeParameter(host, kFilterControlChangeParameter, 1);
    changeParameter(host, kFilterPitchBendParameter, 1);
    changeParameter(host, kFilterAftertouchParameter, 1);
    const uint64_t pausedPulse = snapshot(host).playbackPulse;
    const HistoryImage beforeLoss = captureHistory(host);

    midibuffer_test::resetTrace();
    while (host.elapsedSamples() < fixture.lastClockSample + 32U) {
        noClockBlock(host);
    }
    expect(snapshot(host).clockRunning &&
               midibuffer_test::trace().midiCallCount == 0U,
           "clock remains running until two complete last-measured intervals");
    noClockBlock(host);
    const midibuffer_test::Trace& loss = midibuffer_test::trace();
    expect(!snapshot(host).clockRunning &&
               snapshot(host).playbackClockLossPaused &&
               snapshot(host).playbackPulse == pausedPulse &&
               loss.midiCallCount == 2U &&
               loss.midiCalls[0].bytes[0] == 0x82U &&
               loss.midiCalls[1].bytes[0] == 0xB2U &&
               loss.midiCalls[1].bytes[1] == 64U &&
               loss.midiCalls[1].bytes[2] == 0U,
           "declared clock loss pauses at the saved position and orders every "
           "held-note release before sustain-off");
    expect(sameHistory(beforeLoss, host),
           "clock-loss cleanup does not modify retained history");

    midibuffer_test::resetTrace();
    const uint64_t firstReturn = host.elapsedSamples();
    expect(clockPulseAt(host, firstReturn) &&
               snapshot(host).playbackClockLossPaused &&
               midibuffer_test::trace().midiCallCount == 0U,
           "first returning pulse only begins fresh tempo acquisition");
    const uint64_t secondReturn = firstReturn + 16U;
    expect(clockPulseAt(host, secondReturn) &&
               snapshot(host).clockRunning &&
               snapshot(host).playbackActive &&
               snapshot(host).playbackPulse == pausedPulse,
           "second returning pulse measures current tempo and continues the "
           "saved playback interval");
    stepThrough(host, secondReturn + 8U);
    bool noFabricatedAttack = true;
    for (size_t index = 0; index < midibuffer_test::trace().midiCallCount;
         ++index) {
        noFabricatedAttack =
            noFabricatedAttack &&
            (midibuffer_test::trace().midiCalls[index].bytes[0] & 0xf0U) !=
                0x90U;
    }
    expect(noFabricatedAttack,
           "clock reacquisition does not retrigger notes silenced at loss");
    const uint64_t continuedPulse = secondReturn + 16U;
    expect(clockPulseAt(host, continuedPulse),
           "reacquired clock advances from the saved interval");
    stepThrough(host, continuedPulse + 8U);
    noFabricatedAttack = true;
    for (size_t index = 0; index < midibuffer_test::trace().midiCallCount;
         ++index) {
        noFabricatedAttack =
            noFabricatedAttack &&
            (midibuffer_test::trace().midiCalls[index].bytes[0] & 0xf0U) !=
                0x90U;
    }
    expect(snapshot(host).playbackPulse == pausedPulse + 1U &&
               noFabricatedAttack,
           "continuation reaches the following recorded pulse without a "
           "replacement attack");

    midibuffer_test::HostDouble exactDelay;
    expect(exactDelay.instantiate(1),
           "500 ms loss-threshold host constructs");
    expect(clockPulseAt(exactDelay, 0U) &&
               clockPulseAt(exactDelay, 24000U),
           "48 kHz host measures a 500 ms clock interval");
    while (exactDelay.elapsedSamples() < 72000U) {
        noClockBlock(exactDelay);
    }
    expect(snapshot(exactDelay).clockRunning,
           "500 ms clock remains live through sample 71999");
    noClockBlock(exactDelay);
    expect(!snapshot(exactDelay).clockRunning,
           "500 ms last interval declares loss exactly at 1 second without a "
           "pulse");
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

} // namespace

int main() {
    midibuffer_test::resetTrace();
    midibuffer_test::HostDouble host;
    verifyEntryAndLifecycle(host);
    if (host.algorithm() != NULL) {
        verifyBoundaryCallbacks(host);
    }
    verifyClockedCaptureAndReacquisition();
    verifyChannelAndEventEligibility();
    verifyCaptureStopEndings();
    verifyRollingHistoryAndSelectionInvalidation();
    verifyRetainedReplayRoutingMatrix();
    verifyRecoverableExpressionFiltering();
    verifyRunningAverageAndProportionalScheduling();
    verifyEarlyPulseCatchUpOrder();
    verifyPlaybackClockAcquisition();
    verifyPlaybackCaptureExclusionAndManualStop();
    verifyResetCleanupAndCoincidence();
    verifyClockLossCleanupAndContinuation();
    verifyHostOutputTrace();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d callback contract checks failed\n", gFailures);
        return 1;
    }
    std::printf("PASS: NT adapter and bounded capture-history contract\n");
    return 0;
}
