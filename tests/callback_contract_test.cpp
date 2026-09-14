#include "host_double.hpp"

#include "../src/midibuffer_core.hpp"
#include "../src/nt_host.hpp"
#include "../src/range_motion.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
const size_t kPulsesPerDisplayedBeatParameter = 9;
const size_t kPlaybackParameter = 10;
const size_t kClearRecordingParameter = 11;

const uint32_t kPulsesPerDisplayedBeatValues[] = {1, 2, 4, 8, 16, 24, 48};

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

void expectNear(double actual, double expected, double tolerance,
                const char* message) {
    const double difference = actual < expected ? expected - actual
                                                : actual - expected;
    expect(difference <= tolerance, message);
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

bool drawContainsSubstring(const char* expected) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    for (size_t index = 0; index < current.drawCallCount; ++index) {
        if (std::strstr(current.drawCalls[index].text, expected) != NULL) {
            return true;
        }
    }
    return false;
}

const midibuffer_test::DrawCall* drawAt(int x, int y) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    for (size_t index = 0; index < current.drawCallCount; ++index) {
        if (current.drawCalls[index].x == x &&
            current.drawCalls[index].y == y) {
            return &current.drawCalls[index];
        }
    }
    return NULL;
}

bool framebufferHasInk(int left, int top, int right, int bottom) {
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            if (midibuffer_test::framebufferPixel(x, y) != 0U) {
                return true;
            }
        }
    }
    return false;
}

bool framebufferHasColour(uint8_t colour) {
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            if (midibuffer_test::framebufferPixel(x, y) == colour) {
                return true;
            }
        }
    }
    return false;
}

size_t framebufferColourPixelCount(uint8_t colour) {
    size_t count = 0U;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            if (midibuffer_test::framebufferPixel(x, y) == colour) {
                ++count;
            }
        }
    }
    return count;
}

size_t matchingShapeCount(int x0, int y0, int x1, int y1, int colour) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    size_t count = 0U;
    for (size_t index = 0; index < current.shapeCallCount; ++index) {
        const midibuffer_test::ShapeCall& call = current.shapeCalls[index];
        if (call.shape == kNT_line && call.x0 == x0 && call.y0 == y0 &&
            call.x1 == x1 && call.y1 == y1 && call.colour == colour) {
            ++count;
        }
    }
    return count;
}

size_t directionalEdgeHandleLineCount(bool older, bool start) {
    const int edgeX = older ? 4 : 251;
    const int innerX = older ? 8 : 247;
    const int top = start ? 16 : 48;
    const int middle = top + 4;
    const int bottom = top + 8;
    return matchingShapeCount(innerX, top, edgeX, middle, 15) +
           matchingShapeCount(edgeX, middle, innerX, bottom, 15) +
           matchingShapeCount(innerX, top, innerX, bottom, 15);
}

size_t shapeColourCount(int colour) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    size_t count = 0U;
    for (size_t index = 0; index < current.shapeCallCount; ++index) {
        if (current.shapeCalls[index].colour == colour) {
            ++count;
        }
    }
    return count;
}

size_t playbackHeadLineCount(int* x = NULL, size_t* traceIndex = NULL) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    size_t count = 0U;
    for (size_t index = 0; index < current.shapeCallCount; ++index) {
        const midibuffer_test::ShapeCall& call = current.shapeCalls[index];
        if (call.shape == kNT_line && call.y0 == 17 && call.y1 == 55 &&
            call.x0 == call.x1 && call.colour == 12) {
            if (count == 0U) {
                if (x != NULL) {
                    *x = call.x0;
                }
                if (traceIndex != NULL) {
                    *traceIndex = index;
                }
            }
            ++count;
        }
    }
    return count;
}

bool drawHasHeadAt(midibuffer_test::HostDouble& host, int expectedX) {
    midibuffer_test::resetTrace();
    if (!host.factory()->draw(host.algorithm())) {
        return false;
    }
    int x = -1;
    return playbackHeadLineCount(&x) == 1U && x == expectedX;
}

bool drawHasNoHead(midibuffer_test::HostDouble& host) {
    midibuffer_test::resetTrace();
    return host.factory()->draw(host.algorithm()) &&
           playbackHeadLineCount() == 0U && !framebufferHasColour(12U);
}

bool drawReadouts(midibuffer_test::HostDouble& host,
                  const char* availability, const char* length) {
    midibuffer_test::resetTrace();
    if (!host.factory()->draw(host.algorithm())) {
        return false;
    }
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    const midibuffer_test::DrawCall* availabilityCall = drawAt(0, 7);
    const midibuffer_test::DrawCall* lengthCall = drawAt(176, 7);
    return current.drawCallCount == 2U && availabilityCall != NULL &&
           lengthCall != NULL &&
           availabilityCall->size == kNT_textTiny &&
           lengthCall->size == kNT_textTiny &&
           std::strcmp(availabilityCall->text, availability) == 0 &&
           std::strcmp(lengthCall->text, length) == 0 &&
           framebufferHasInk(0, 0, 175, 7) &&
           framebufferHasInk(176, 0, 255, 7);
}

bool formatEquals(uint64_t pulseIntervals, uint32_t pulsesPerBeat,
                  const char* expected) {
    char actual[17];
    return midibuffer::formatMusicalDuration(
               actual, sizeof(actual), pulseIntervals, pulsesPerBeat) &&
           std::strcmp(actual, expected) == 0;
}

void moveUi(midibuffer_test::HostDouble& host, uint16_t controls,
            float leftPot, float centrePot, float rightPot,
            int8_t leftEncoder = 0, int8_t rightEncoder = 0,
            uint16_t lastButtons = 0) {
    _NT_uiData data = {};
    data.controls = controls;
    data.lastButtons = lastButtons;
    data.pots[0] = leftPot;
    data.pots[1] = centrePot;
    data.pots[2] = rightPot;
    data.encoders[0] = leftEncoder;
    data.encoders[1] = rightEncoder;
    host.factory()->customUi(host.algorithm(), data);
}

void seedRightPot(midibuffer_test::HostDouble& host, float pot) {
    moveUi(host, 0U, 0.0f, 0.0f, pot);
}

void moveRightPot(midibuffer_test::HostDouble& host, float pot) {
    moveUi(host, kNT_potR, 0.0f, 0.0f, pot);
}

void beginRightPotZoom(midibuffer_test::HostDouble& host, float pot) {
    moveUi(host, kNT_potButtonR, 0.0f, 0.0f, pot);
}

void continueRightPotZoom(midibuffer_test::HostDouble& host, float pot) {
    moveUi(host, kNT_potButtonR | kNT_potR, 0.0f, 0.0f, pot, 0, 0,
           kNT_potButtonR);
}

void endRightPotZoom(midibuffer_test::HostDouble& host, float pot) {
    moveUi(host, 0U, 0.0f, 0.0f, pot, 0, 0, kNT_potButtonR);
}

void seedBoundaryPots(midibuffer_test::HostDouble& host, float left,
                      float centre, float right = 0.0f) {
    _NT_float3 setupPots = {};
    host.factory()->setupUi(host.algorithm(), setupPots);
    moveUi(host, 0U, left, centre, right);
}

bool installRangeFixture(midibuffer_test::HostDouble& host,
                         uint64_t historyStart, uint64_t historyEnd,
                         uint64_t selectionStart, uint64_t selectionEnd) {
    return host.instantiate(1) &&
           midibuffer::setRetainedTimelineFixture(
               host.algorithm(), historyStart, historyEnd) &&
           midibuffer::setPulseSelection(
               host.algorithm(), selectionStart, selectionEnd);
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
            (actual.flags & 1U) != (expected.events[index].flags & 1U) ||
            std::memcmp(actual.bytes, expected.events[index].bytes,
                        sizeof(actual.bytes)) != 0) {
            return false;
        }
    }
    return true;
}

bool sameSelectionAndTransport(
    const midibuffer::CaptureSnapshot& expected,
    const midibuffer::CaptureSnapshot& actual) {
    return actual.selectionValid == expected.selectionValid &&
           actual.selection.startPulse == expected.selection.startPulse &&
           actual.selection.endPulse == expected.selection.endPulse &&
           actual.activeSelectionValid == expected.activeSelectionValid &&
           actual.activeSelection.startPulse ==
               expected.activeSelection.startPulse &&
           actual.activeSelection.endPulse == expected.activeSelection.endPulse &&
           actual.rangeTransitionPending == expected.rangeTransitionPending &&
           actual.playbackArmed == expected.playbackArmed &&
           actual.playbackActive == expected.playbackActive &&
           actual.playbackClockLossPaused ==
               expected.playbackClockLossPaused &&
           actual.playbackPulse == expected.playbackPulse &&
           actual.playbackIntervalOrdinal ==
               expected.playbackIntervalOrdinal &&
           actual.playbackEventIndex == expected.playbackEventIndex &&
           actual.playbackIntervalOpen == expected.playbackIntervalOpen &&
           actual.playbackNextEventScheduled ==
               expected.playbackNextEventScheduled &&
           actual.pendingNextEndingScheduled ==
               expected.pendingNextEndingScheduled;
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
    expect(host.requirements().numParameters == 12,
           "indices 0 through 9 stay stable and Playback/Clear Recording append at 10 and 11");
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
               !initial.clockRunning && host.parameter(kCaptureParameter) == 0 &&
               host.parameter(kPlaybackParameter) == 0,
           "fresh instances start empty with Capture and Playback Off and no acquired clock");

    expect(host.algorithm()->parameters != NULL &&
               host.algorithm()->parameterPages != NULL,
           "constructed instance publishes parameter definitions and pages");
    expect(host.factory()->parameterChanged != NULL &&
               host.factory()->step != NULL && host.factory()->draw != NULL &&
               host.factory()->midiMessage != NULL &&
               host.factory()->midiRealtime != NULL &&
               host.factory()->hasCustomUi != NULL &&
               host.factory()->customUi != NULL &&
               host.factory()->setupUi != NULL &&
               host.factory()->serialise != NULL &&
               host.factory()->deserialise != NULL,
           "factory registers every callback adapter including native preset persistence");
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
    expect(std::strcmp(host.algorithm()
                           ->parameters[kPulsesPerDisplayedBeatParameter]
                           .name,
                       "Pulses/Beat") == 0 &&
               host.algorithm()
                       ->parameters[kPulsesPerDisplayedBeatParameter]
                       .def == 0 &&
               host.algorithm()
                       ->parameters[kPulsesPerDisplayedBeatParameter]
                       .max == 6,
           "pulses per displayed beat defaults to 1 and offers seven choices");
    for (size_t index = 0;
         index < ARRAY_SIZE(kPulsesPerDisplayedBeatValues); ++index) {
        expect(static_cast<uint32_t>(std::atoi(
                   host.algorithm()
                       ->parameters[kPulsesPerDisplayedBeatParameter]
                       .enumStrings[index])) ==
                   kPulsesPerDisplayedBeatValues[index],
               "beat-display enum exposes the approved runtime value");
    }
    expect(std::strcmp(host.algorithm()->parameters[kPlaybackParameter].name,
                       "Playback") == 0 &&
               host.algorithm()->parameters[kPlaybackParameter].min == 0 &&
               host.algorithm()->parameters[kPlaybackParameter].max == 1 &&
               host.algorithm()->parameters[kPlaybackParameter].def == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackParameter]
                               .enumStrings[0],
                           "Off") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kPlaybackParameter]
                               .enumStrings[1],
                           "On") == 0 &&
               std::strcmp(host.algorithm()
                               ->parameters[kClearRecordingParameter]
                               .name,
                           "Clear Recording") == 0 &&
               host.algorithm()->parameters[kClearRecordingParameter].min == 0 &&
               host.algorithm()->parameters[kClearRecordingParameter].max == 1 &&
               host.algorithm()->parameters[kClearRecordingParameter].def == 0,
           "appended Playback and reserved Clear Recording expose Boolean Off/On values");
    const _NT_parameterPages* pages = host.algorithm()->parameterPages;
    expect(pages->numPages == 4 && pages->pages[1].numParams == 3 &&
               pages->pages[1].params[2] == kClearRecordingParameter &&
               pages->pages[2].numParams == 6 &&
               pages->pages[2].params[0] == kPlaybackParameter &&
               pages->pages[2].params[3] ==
                   kFilterControlChangeParameter &&
               pages->pages[2].params[4] == kFilterPitchBendParameter &&
               pages->pages[2].params[5] == kFilterAftertouchParameter &&
               pages->pages[3].numParams == 1 &&
               pages->pages[3].params[0] ==
                   kPulsesPerDisplayedBeatParameter,
           "new controls join their pages without renumbering inherited controls");
}

void verifyMusicalDurationFormattingAndReadouts() {
    bool exhaustiveFormattingMatches = true;
    for (size_t index = 0;
         index < ARRAY_SIZE(kPulsesPerDisplayedBeatValues); ++index) {
        const uint32_t pulsesPerBeat =
            kPulsesPerDisplayedBeatValues[index];
        for (uint64_t pulseIntervals = 0U;
             pulseIntervals <= 8U * pulsesPerBeat; ++pulseIntervals) {
            midibuffer::MusicalDuration duration = {};
            uint64_t inverse = 0U;
            const uint64_t wholeBeats = pulseIntervals / pulsesPerBeat;
            const uint32_t remainder = static_cast<uint32_t>(
                pulseIntervals % pulsesPerBeat);
            char expected[17];
            std::snprintf(
                expected, sizeof(expected), "%llu:%u:%03u",
                static_cast<unsigned long long>(wholeBeats / 4U),
                static_cast<unsigned>(wholeBeats % 4U),
                static_cast<unsigned>(remainder * (480U / pulsesPerBeat)));
            char actual[17];
            exhaustiveFormattingMatches =
                midibuffer::musicalDurationFromPulses(
                    pulseIntervals, pulsesPerBeat, duration) &&
                duration.bars == wholeBeats / 4U &&
                duration.beats == wholeBeats % 4U &&
                duration.ticks == remainder * (480U / pulsesPerBeat) &&
                midibuffer::pulsesFromMusicalDuration(
                    duration, pulsesPerBeat, inverse) &&
                inverse == pulseIntervals &&
                midibuffer::formatMusicalDuration(
                    actual, sizeof(actual), pulseIntervals, pulsesPerBeat) &&
                std::strcmp(actual, expected) == 0 &&
                exhaustiveFormattingMatches;
        }
    }
    expect(exhaustiveFormattingMatches,
           "all supported P and N=0..8P convert exactly, roll beats/bars, format three tick digits, and invert to N");

    expect(formatEquals(64U, 4U, "4:0:000") &&
               formatEquals(65U, 4U, "4:0:120") &&
               formatEquals(64U, 48U, "0:1:160") &&
               formatEquals(191U, 48U, "0:3:470") &&
               formatEquals(192U, 48U, "1:0:000"),
           "listed 64/65 and 191/192 musical-duration vectors are exact");

    bool wideFormattingMatches = true;
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    for (size_t index = 0;
         index < ARRAY_SIZE(kPulsesPerDisplayedBeatValues); ++index) {
        const uint32_t pulsesPerBeat =
            kPulsesPerDisplayedBeatValues[index];
        const uint64_t vectors[] = {
            static_cast<uint64_t>(1U) << 32U,
            maximum,
        };
        for (size_t vector = 0; vector < ARRAY_SIZE(vectors); ++vector) {
            midibuffer::MusicalDuration duration = {};
            uint64_t inverse = 0U;
            char output[17];
            char expected[17];
            const uint64_t wholeBeats = vectors[vector] / pulsesPerBeat;
            const uint64_t expectedBars = wholeBeats / 4U;
            if (expectedBars > 9999999999ULL) {
                std::snprintf(expected, sizeof(expected),
                              ">9999999999bar");
            } else {
                std::snprintf(
                    expected, sizeof(expected), "%llu:%u:%03u",
                    static_cast<unsigned long long>(expectedBars),
                    static_cast<unsigned>(wholeBeats % 4U),
                    static_cast<unsigned>(
                        (vectors[vector] % pulsesPerBeat) *
                        (480U / pulsesPerBeat)));
            }
            wideFormattingMatches =
                midibuffer::musicalDurationFromPulses(
                    vectors[vector], pulsesPerBeat, duration) &&
                duration.bars == expectedBars &&
                duration.beats == wholeBeats % 4U &&
                duration.ticks ==
                    (vectors[vector] % pulsesPerBeat) *
                        (480U / pulsesPerBeat) &&
                midibuffer::pulsesFromMusicalDuration(
                    duration, pulsesPerBeat, inverse) &&
                inverse == vectors[vector] &&
                midibuffer::formatMusicalDuration(
                    output, sizeof(output), vectors[vector],
                    pulsesPerBeat) &&
                std::strcmp(output, expected) == 0 &&
                wideFormattingMatches;
        }

        const uint64_t maximumExact =
            (9999999999ULL * 4U + 3U) * pulsesPerBeat +
            (pulsesPerBeat - 1U);
        char expected[17];
        std::snprintf(expected, sizeof(expected),
                      "9999999999:3:%03u",
                      static_cast<unsigned>((pulsesPerBeat - 1U) *
                                            (480U / pulsesPerBeat)));
        char actual[17];
        char overflow[15];
        wideFormattingMatches =
            midibuffer::formatMusicalDuration(
                actual, sizeof(actual), maximumExact, pulsesPerBeat) &&
            std::strcmp(actual, expected) == 0 &&
            midibuffer::formatMusicalDuration(
                overflow, sizeof(overflow), maximumExact + 1U,
                pulsesPerBeat) &&
            std::strcmp(overflow, ">9999999999bar") == 0 &&
            wideFormattingMatches;
    }
    expect(wideFormattingMatches,
           "2^32 and UINT64_MAX invert exactly before presentation; every P shows the last exact bar and whole overflow marker");

    struct GuardedText {
        char before;
        char output[17];
        char after;
    } guarded = {'L', {}, 'R'};
    expect(midibuffer::formatMusicalDuration(
               guarded.output, sizeof(guarded.output),
               (9999999999ULL * 4U + 3U) * 48U + 47U, 48U) &&
               std::strcmp(guarded.output, "9999999999:3:470") == 0 &&
               guarded.before == 'L' && guarded.after == 'R',
           "largest exact amount fits its 17-byte buffer including NUL");
    char undersized[14];
    std::memset(undersized, 'x', sizeof(undersized));
    expect(!midibuffer::formatMusicalDuration(
               undersized, sizeof(undersized), maximum, 48U) &&
               undersized[sizeof(undersized) - 1U] == '\0',
           "undersized formatting fails safely with a bounded NUL-terminated prefix");
    char invalid[17] = {'x', '\0'};
    expect(!midibuffer::formatMusicalDuration(
               invalid, sizeof(invalid), 1U, 3U) && invalid[0] == '\0',
           "formatter rejects unsupported P without an unbounded fallback");

    midibuffer_test::HostDouble empty;
    expect(empty.instantiate(1) &&
               drawReadouts(empty, "Avail 0:0:000  1ppb", "Len --"),
           "before clock, empty readouts show exact zero availability and invalid Len");
    clockPulse(empty);
    expect(!snapshot(empty).clockRunning &&
               drawReadouts(empty, "Avail 0:0:000  1ppb", "Len --"),
           "during one-pulse clock acquisition, empty readouts stay zero and invalid");
    changeParameter(empty, kPulsesPerDisplayedBeatParameter, 6);
    expect(drawReadouts(empty, "Avail 0:0:000  48ppb", "Len --") &&
               !framebufferHasInk(0, 0, 255, 2),
           "invalid Len and a normal zero Avail use the original tiny-font regions at 48ppb");

    const uint64_t retainedStart = 100U;
    const uint64_t retainedEnd = 164U;
    const uint64_t selectionStart = 108U;
    const uint64_t selectionEnd = 124U;
    midibuffer_test::HostDouble source;
    expect(installRangeFixture(source, retainedStart, retainedEnd,
                               selectionStart, selectionEnd),
           "saved-duration source fixture installs");
    changeParameter(source, kPulsesPerDisplayedBeatParameter, 2);
    midibuffer_test::PresetImage image;
    expect(source.savePreset(image),
           "new saved-duration fixture serializes through the production callback");

    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(1) && restored.loadPreset(image) &&
               !snapshot(restored).clockRunning &&
               drawReadouts(restored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "new valid saved durations render without a live clock");
    const HistoryImage restoredHistory = captureHistory(restored);
    clockPulse(restored);
    expect(!snapshot(restored).clockRunning &&
               drawReadouts(restored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "restored durations survive the first reacquisition pulse");
    clockPulse(restored);
    expect(snapshot(restored).clockRunning &&
               drawReadouts(restored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "restored durations survive completed clock reacquisition");
    noClockBlock(restored);
    noClockBlock(restored);
    expect(!snapshot(restored).clockRunning &&
               drawReadouts(restored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "retained and edited saved durations survive declared clock loss");
    clockPulse(restored);
    noClockBlock(restored);
    noClockBlock(restored);
    clockPulse(restored);
    expect(snapshot(restored).clockRunning &&
               drawReadouts(restored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "different-tempo reacquisition does not alter pulse-derived readouts");

    const midibuffer::CaptureSnapshot beforePresentationChange =
        snapshot(restored);
    changeParameter(restored, kPulsesPerDisplayedBeatParameter, 6);
    const midibuffer::CaptureSnapshot afterPresentationChange =
        snapshot(restored);
    expect(drawReadouts(restored, "Avail 0:1:160  48ppb",
                        "Len 0:0:160") &&
               afterPresentationChange.currentPulse ==
                   beforePresentationChange.currentPulse &&
               afterPresentationChange.retainedPulseIntervals ==
                   beforePresentationChange.retainedPulseIntervals &&
               afterPresentationChange.timelineViewStartPulse ==
                   beforePresentationChange.timelineViewStartPulse &&
               afterPresentationChange.timelineViewEndPulse ==
                   beforePresentationChange.timelineViewEndPulse &&
               afterPresentationChange.selection.startPulse ==
                   beforePresentationChange.selection.startPulse &&
               afterPresentationChange.selection.endPulse ==
                   beforePresentationChange.selection.endPulse &&
               sameSelectionAndTransport(beforePresentationChange,
                                         afterPresentationChange) &&
               sameHistory(restoredHistory, restored),
           "P changes only exact presentation, not retained pulses or selection");

    midibuffer_test::PresetImage legacyImage;
    expect(source.savePreset(legacyImage) &&
               legacyImage.removeNavigation(),
           "legacy saved-duration image removes only the navigation extension");
    midibuffer_test::HostDouble legacyRestored;
    expect(legacyRestored.instantiate(1) &&
               legacyRestored.loadPreset(legacyImage) &&
               !snapshot(legacyRestored).clockRunning &&
               drawReadouts(legacyRestored, "Avail 4:0:000  4ppb",
                            "Len 1:0:000"),
           "legacy valid saved durations remain visible without clock");

    expect(midibuffer::startPlayback(restored.algorithm()),
           "edited-versus-active duration fixture arms playback");
    clockPulse(restored);
    expect(snapshot(restored).playbackActive &&
               snapshot(restored).activeSelection.endPulse -
                       snapshot(restored).activeSelection.startPulse ==
                   16U,
           "duration fixture activates the original 16-pulse range");
    expect(midibuffer::setPulseSelection(restored.algorithm(),
                                         selectionStart,
                                         selectionStart + 20U),
           "duration fixture installs a pending 20-pulse edit");
    const midibuffer::CaptureSnapshot pending = snapshot(restored);
    expect(pending.rangeTransitionPending &&
               pending.activeSelection.endPulse -
                       pending.activeSelection.startPulse == 16U &&
               pending.selection.endPulse - pending.selection.startPulse ==
                   20U &&
               drawReadouts(restored, "Avail 0:1:160  48ppb",
                            "Len 0:0:200"),
           "Len reads edited E-S without +1 while the active range remains pending");

    const uint64_t maximumExact =
        (9999999999ULL * 4U + 3U) * 48U + 47U;
    midibuffer_test::HostDouble exactLayout;
    expect(installRangeFixture(exactLayout, 0U, maximumExact,
                               0U, maximumExact),
           "maximum exact readout layout fixture installs");
    changeParameter(exactLayout, kPulsesPerDisplayedBeatParameter, 6);
    const midibuffer::CaptureSnapshot exactBefore = snapshot(exactLayout);
    const HistoryImage exactHistory = captureHistory(exactLayout);
    expect(drawReadouts(
               exactLayout,
               "Avail 9999999999:3:470  48ppb",
               "Len 9999999999:3:470") &&
               framebufferHasInk(0, 0, 175, 7) &&
               framebufferHasInk(176, 0, 255, 7) &&
               midibuffer_test::framebufferPixel(254, 7) != 0U &&
               midibuffer_test::framebufferPixel(255, 7) == 0U &&
               !framebufferHasInk(0, 0, 255, 2) &&
               !framebufferHasInk(115, 3, 175, 7) &&
               sameSelectionAndTransport(exactBefore,
                                         snapshot(exactLayout)) &&
               sameHistory(exactHistory, exactLayout),
           "real draw trace and tiny 3x5 framebuffer keep maximum exact readouts in fixed four-pixel cells at both original regions without mutation");

    midibuffer_test::HostDouble overflowLayout;
    expect(installRangeFixture(overflowLayout, 0U, maximumExact + 1U,
                               0U, maximumExact + 1U),
           "first overflowing-bar layout fixture installs");
    changeParameter(overflowLayout, kPulsesPerDisplayedBeatParameter, 6);
    const midibuffer::CaptureSnapshot overflowBefore = snapshot(overflowLayout);
    expect(drawReadouts(
               overflowLayout,
               "Avail >9999999999bar  48ppb",
               "Len >9999999999bar") &&
               framebufferHasInk(0, 0, 175, 7) &&
               framebufferHasInk(176, 0, 255, 7) &&
               snapshot(overflowLayout).retainedPulseIntervals ==
                   overflowBefore.retainedPulseIntervals &&
               sameSelectionAndTransport(overflowBefore,
                                         snapshot(overflowLayout)),
           "first amount above the bar limit uses the complete overflow marker in both real draw regions without pulse-state mutation");

    midibuffer_test::HostDouble maximumLayout;
    expect(installRangeFixture(maximumLayout, 0U, maximum, 0U, maximum),
           "UINT64_MAX layout fixture installs");
    changeParameter(maximumLayout, kPulsesPerDisplayedBeatParameter, 6);
    const midibuffer::CaptureSnapshot maximumBefore = snapshot(maximumLayout);
    expect(drawReadouts(
               maximumLayout,
               "Avail >9999999999bar  48ppb",
               "Len >9999999999bar") &&
               snapshot(maximumLayout).retainedPulseIntervals ==
                   maximumBefore.retainedPulseIntervals &&
               sameSelectionAndTransport(maximumBefore,
                                         snapshot(maximumLayout)) &&
               !drawContainsSubstring("NO CLK") &&
               !drawContainsSubstring("History") &&
               !drawContainsSubstring(".00b"),
           "UINT64_MAX uses bounded overflow text with no decimal suffix or new clock/history labels");
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
    expect(pots[0] == 0.0f && pots[1] == 1.0f && pots[2] == 0.0f,
           "setupUi supplies deterministic start, end, and normal-range positions");
    const uint32_t customMask = host.factory()->hasCustomUi(host.algorithm());
    const uint32_t expectedCustomMask =
        kNT_potL | kNT_potC | kNT_potR | kNT_potButtonR |
        kNT_encoderL | kNT_encoderR | kNT_encoderButtonL |
        kNT_encoderButtonR;
    expect(customMask == expectedCustomMask,
           "custom control mask remains exactly the inherited timeline, playback-toggle, zoom, and panic mapping with no Clear Recording shortcut");

    midibuffer_test::resetTrace();
    expect(host.factory()->draw(host.algorithm()),
           "timeline draw owns the complete 256 by 64 display");
    expect(drawContains("Avail 0:0:000  1ppb"),
           "empty timeline reports exact zero musical availability");
    expect(drawContains("Len --"),
           "empty timeline reports no selected phrase length");
    expect(midibuffer_test::framebufferPixel(100, 52) != 0U,
           "timeline draw reaches the real framebuffer seam");
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

void verifyTimelineSelectionDisplayAndControls() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "timeline host constructs");
    startCapture(host);
    acquireClock(host);

    sendMidi(host, 0x90, 48, 100);
    sendMidi(host, 0x80, 48, 0);
    expect(snapshot(host).retainedPulseIntervals == 1U,
           "first captured pulse exposes one retained interval");
    for (uint32_t interval = 1; interval <= 63U; ++interval) {
        clockPulse(host);
        if ((interval % 8U) == 0U) {
            const uint8_t note = static_cast<uint8_t>(48U + interval / 8U);
            sendMidi(host, 0x90, note, 100);
            sendMidi(host, 0x80, note, 0);
        }
    }

    changeParameter(host, kPulsesPerDisplayedBeatParameter, 2);
    midibuffer::CaptureSnapshot current = snapshot(host);
    expect(current.retainedPulseIntervals == 64U &&
               current.pulsesPerDisplayedBeat == 4U &&
               current.timelineVisiblePulses == 64U &&
               current.timelineViewStartPulse == current.oldestPulse &&
               current.timelineViewEndPulse == current.oldestPulse + 64U,
           "64 retained pulse intervals display as a full 16-beat view at conversion 4");

    midibuffer_test::resetTrace();
    expect(host.factory()->draw(host.algorithm()),
           "capturing timeline draws through the production callback");
    expect(drawContains("Avail 4:0:000  4ppb") && drawContains("Len --"),
           "capturing timeline exposes exact musical availability and no phantom selection");
    expect(!drawContainsSubstring("History") &&
               !drawContainsSubstring("Oldest"),
           "timeline omits the history heading and oldest-history label");
    expect(midibuffer_test::trace().shapeCallCount >= 9U &&
               midibuffer_test::litFramebufferPixelCount() >= 248U,
           "capturing framebuffer contains the axis and recorded note marks");

    moveUi(host, kNT_potL, 0.25f, 0.0f, 0.0f);
    const midibuffer::CaptureSnapshot startMoved = snapshot(host);
    expect(startMoved.selectionValid && startMoved.lastMovedBoundaryIsStart &&
               startMoved.selection.startPulse > current.oldestPulse &&
               startMoved.selection.endPulse == current.oldestPulse + 64U,
           "left pot independently moves the pulse-snapped selection start");

    moveUi(host, kNT_potC, 0.0f, 0.75f, 0.0f);
    const midibuffer::CaptureSnapshot endMoved = snapshot(host);
    expect(!endMoved.lastMovedBoundaryIsStart &&
               endMoved.selection.startPulse ==
                   startMoved.selection.startPulse &&
               endMoved.selection.endPulse < startMoved.selection.endPulse,
           "centre pot independently moves the pulse-snapped selection end");

    moveUi(host, 0, 0.0f, 0.0f, 0.0f, 0, 1);
    const midibuffer::CaptureSnapshot fineEnd = snapshot(host);
    expect(fineEnd.selection.startPulse == endMoved.selection.startPulse &&
               fineEnd.selection.endPulse ==
                   endMoved.selection.endPulse + 1U,
           "right encoder fine-adjusts the last-moved end by one pulse");

    moveUi(host, kNT_potL, 0.50f, 0.0f, 0.0f);
    const midibuffer::CaptureSnapshot secondStartMoved = snapshot(host);
    moveUi(host, 0, 0.0f, 0.0f, 0.0f, 0, -1);
    const midibuffer::CaptureSnapshot fineStart = snapshot(host);
    expect(fineStart.lastMovedBoundaryIsStart &&
               fineStart.selection.startPulse + 1U ==
                   secondStartMoved.selection.startPulse &&
               fineStart.selection.endPulse ==
                   secondStartMoved.selection.endPulse,
           "right encoder follows the last-moved start boundary by one pulse");

    const midibuffer::PulseRange navigationSelection = fineStart.selection;
    beginRightPotZoom(host, 0.5f);
    continueRightPotZoom(host, 0.0f);
    endRightPotZoom(host, 0.0f);
    const midibuffer::CaptureSnapshot zoomed = snapshot(host);
    expect(zoomed.timelineVisiblePulses == 4U &&
               zoomed.selection.startPulse ==
                   navigationSelection.startPulse &&
               zoomed.selection.endPulse == navigationSelection.endPulse,
           "right pot zooms in without moving either selected boundary");
    moveUi(host, 0, 0.0f, 0.0f, 0.0f, 3, 0);
    const midibuffer::CaptureSnapshot scrolled = snapshot(host);
    expect(scrolled.timelineViewStartPulse + 3U ==
                   zoomed.timelineViewStartPulse &&
               scrolled.timelineViewEndPulse + 3U ==
                   zoomed.timelineViewEndPulse &&
               scrolled.selection.startPulse ==
                   navigationSelection.startPulse &&
               scrolled.selection.endPulse == navigationSelection.endPulse,
           "left encoder scrolls three history pulses without moving the selection");
    beginRightPotZoom(host, 0.0f);
    continueRightPotZoom(host, 1.0f);
    endRightPotZoom(host, 1.0f);
    expect(snapshot(host).timelineShowAll &&
               snapshot(host).timelineViewStartPulse == current.oldestPulse &&
               snapshot(host).timelineViewEndPulse == current.oldestPulse + 64U,
           "explicit full zoom-out returns the timeline to Show All");

    const uint64_t selectedStart = current.oldestPulse + 8U;
    const uint64_t selectedEnd = current.oldestPulse + 24U;
    expect(midibuffer::setPulseSelection(host.algorithm(), selectedStart,
                                         selectedEnd),
           "timeline fixture selects a 16-pulse phrase");
    const HistoryImage immutableHistory = captureHistory(host);
    for (size_t index = 0;
         index < ARRAY_SIZE(kPulsesPerDisplayedBeatValues); ++index) {
        changeParameter(host, kPulsesPerDisplayedBeatParameter,
                        static_cast<int16_t>(index));
        const midibuffer::CaptureSnapshot converted = snapshot(host);
        expect(converted.pulsesPerDisplayedBeat ==
                       kPulsesPerDisplayedBeatValues[index] &&
                   converted.selection.startPulse == selectedStart &&
                   converted.selection.endPulse == selectedEnd &&
                   sameHistory(immutableHistory, host),
               "runtime beat conversion preserves event timestamps and pulse boundaries");
    }
    changeParameter(host, kPulsesPerDisplayedBeatParameter, 2);

    midibuffer_test::resetTrace();
    host.factory()->draw(host.algorithm());
    expect(drawContains("Avail 4:0:000  4ppb") &&
               drawContains("Len 1:0:000"),
           "selected timeline reports retained availability and zero-based phrase duration");
    expect(midibuffer_test::framebufferPixel(34, 16) == 15U &&
               midibuffer_test::framebufferPixel(96, 16) == 15U,
           "framebuffer contains the selected start and end brackets");

    expect(midibuffer::startPlayback(host.algorithm()),
           "selected timeline phrase arms playback");
    clockPulse(host);
    const midibuffer::CaptureSnapshot playing = snapshot(host);
    expect(playing.playbackActive && playing.activeSelection.startPulse ==
                                        selectedStart &&
               playing.activeSelection.endPulse == selectedEnd,
           "timeline phrase enters actual clocked playback");
    moveUi(host, kNT_potC, 0.0f, 0.90f, 0.0f);
    const midibuffer::CaptureSnapshot pending = snapshot(host);
    expect(pending.rangeTransitionPending &&
               pending.activeSelection.startPulse == selectedStart &&
               pending.activeSelection.endPulse == selectedEnd &&
               pending.selection.endPulse != selectedEnd,
           "live timeline edit routes through the next-wrap transition contract");
    midibuffer_test::resetTrace();
    host.factory()->draw(host.algorithm());
    expect(midibuffer_test::trace().shapeCallCount != 0U &&
               midibuffer_test::litFramebufferPixelCount() != 0U,
           "playing timeline remains rendered through draw traces and framebuffer");
}

void verifyDirectionalBoundaryHandles() {
    struct DrawCase {
        uint64_t selectionStart;
        uint64_t selectionEnd;
        bool startOlder;
        bool startNewer;
        bool endOlder;
        bool endNewer;
        const char* description;
    };
    const DrawCase cases[] = {
        {150U, 170U, false, false, false, false,
         "visible boundaries retain full selection brackets"},
        {120U, 170U, true, false, false, false,
         "an older offscreen Start gets the upper left-pointing handle"},
        {150U, 200U, false, false, false, true,
         "a newer offscreen End gets the lower right-pointing handle"},
        {120U, 200U, true, false, false, true,
         "simultaneous offscreen boundaries get opposite directional handles"},
        {100U, 120U, true, false, true, false,
         "offscreen Start and End remain distinct on the older edge"},
        {200U, 240U, false, true, false, true,
         "offscreen Start and End remain distinct on the newer edge"},
    };
    for (size_t index = 0; index < ARRAY_SIZE(cases); ++index) {
        midibuffer_test::HostDouble host;
        expect(installRangeFixture(host, 100U, 300U,
                                   cases[index].selectionStart,
                                   cases[index].selectionEnd) &&
                   midibuffer::setTimelineNavigationFixture(
                       host.algorithm(), false, 40U, 120U,
                       midibuffer::kSelectionFineTargetEnd),
               "directional boundary draw fixture installs");
        const midibuffer::CaptureSnapshot before = snapshot(host);
        midibuffer_test::resetTrace();
        const bool drew = host.factory()->draw(host.algorithm());
        const midibuffer::CaptureSnapshot after = snapshot(host);
        const size_t startOlderLines =
            directionalEdgeHandleLineCount(true, true);
        const size_t startNewerLines =
            directionalEdgeHandleLineCount(false, true);
        const size_t endOlderLines =
            directionalEdgeHandleLineCount(true, false);
        const size_t endNewerLines =
            directionalEdgeHandleLineCount(false, false);
        const bool expectedHandles =
            startOlderLines == (cases[index].startOlder ? 3U : 0U) &&
            startNewerLines == (cases[index].startNewer ? 3U : 0U) &&
            endOlderLines == (cases[index].endOlder ? 3U : 0U) &&
            endNewerLines == (cases[index].endNewer ? 3U : 0U);
        const bool visibleStart = cases[index].selectionStart >= 140U &&
                                  cases[index].selectionStart <= 180U;
        const bool visibleEnd = cases[index].selectionEnd >= 140U &&
                                cases[index].selectionEnd <= 180U;
        const int startX = visibleStart
                               ? 4 + static_cast<int>(
                                         (cases[index].selectionStart - 140U) *
                                         247U / 40U)
                               : -1;
        const int endX = visibleEnd
                             ? 4 + static_cast<int>(
                                       (cases[index].selectionEnd - 140U) *
                                       247U / 40U)
                             : -1;
        const bool expectedBrackets =
            (!visibleStart ||
             matchingShapeCount(startX, 16, startX, 56, 15) == 1U) &&
            (!visibleEnd ||
             matchingShapeCount(endX, 16, endX, 56, 15) == 1U);
        const bool framebufferEvidence =
            (!cases[index].startOlder ||
             midibuffer_test::framebufferPixel(4, 20) == 15U) &&
            (!cases[index].startNewer ||
             midibuffer_test::framebufferPixel(251, 20) == 15U) &&
            (!cases[index].endOlder ||
             midibuffer_test::framebufferPixel(4, 52) == 15U) &&
            (!cases[index].endNewer ||
             midibuffer_test::framebufferPixel(251, 52) == 15U);
        expect(drew && expectedHandles && expectedBrackets &&
                   framebufferEvidence &&
                   after.timelineViewStartPulse == before.timelineViewStartPulse &&
                   after.timelineViewEndPulse == before.timelineViewEndPulse &&
                   after.timelineVisiblePulses == before.timelineVisiblePulses &&
                   after.timelineScrollPulses == before.timelineScrollPulses &&
                   after.timelineShowAll == before.timelineShowAll &&
                   sameSelectionAndTransport(before, after),
               cases[index].description);
    }

    struct RetrievalCase {
        bool startBoundary;
        uint64_t selectionStart;
        uint64_t selectionEnd;
        bool older;
        uint64_t expectedBoundary;
    };
    const RetrievalCase retrievals[] = {
        {true, 120U, 170U, true, 140U},
        {false, 150U, 200U, false, 180U},
        {true, 200U, 240U, false, 180U},
        {false, 100U, 120U, true, 140U},
    };
    for (size_t index = 0; index < ARRAY_SIZE(retrievals); ++index) {
        midibuffer_test::HostDouble host;
        expect(installRangeFixture(host, 100U, 300U,
                                   retrievals[index].selectionStart,
                                   retrievals[index].selectionEnd) &&
                   midibuffer::setTimelineNavigationFixture(
                       host.algorithm(), false, 40U, 120U,
                       midibuffer::kSelectionFineTargetEnd),
               "boundary handle retrieval fixture installs");
        seedBoundaryPots(host, 0.4f, 0.6f);
        midibuffer_test::resetTrace();
        host.factory()->draw(host.algorithm());
        expect(directionalEdgeHandleLineCount(retrievals[index].older,
                                              retrievals[index].startBoundary) ==
                   3U,
               "offscreen boundary handle is present before its associated gesture");
        const midibuffer::CaptureSnapshot beforeGesture = snapshot(host);
        moveUi(host, retrievals[index].startBoundary ? kNT_potL : kNT_potC,
               retrievals[index].startBoundary ? 0.41f : 0.4f,
               retrievals[index].startBoundary ? 0.6f : 0.61f, 0.0f);
        const midibuffer::CaptureSnapshot retrieved = snapshot(host);
        midibuffer_test::resetTrace();
        host.factory()->draw(host.algorithm());
        const uint64_t actualBoundary = retrievals[index].startBoundary
                                            ? retrieved.selection.startPulse
                                            : retrieved.selection.endPulse;
        const int edgeX = retrievals[index].older ? 4 : 251;
        expect(actualBoundary == retrievals[index].expectedBoundary &&
                   retrieved.selection.startPulse < retrieved.selection.endPulse &&
                   retrieved.timelineViewStartPulse ==
                       beforeGesture.timelineViewStartPulse &&
                   retrieved.timelineViewEndPulse ==
                       beforeGesture.timelineViewEndPulse &&
                   retrieved.timelineScrollPulses ==
                       beforeGesture.timelineScrollPulses &&
                   directionalEdgeHandleLineCount(
                       retrievals[index].older,
                       retrievals[index].startBoundary) == 0U &&
                   matchingShapeCount(edgeX, 16, edgeX, 56, 15) == 1U,
               "the associated boundary pot retrieves its indicated boundary to the nearest valid visible edge without scrolling");
    }
}

void preparePlaybackHeadHistory(midibuffer_test::HostDouble& host,
                                uint64_t startPulse, uint64_t endPulse) {
    expect(host.instantiate(1), "playback-head callback host constructs");
    startCapture(host);
    acquireClock(host);
    while (snapshot(host).currentPulse < startPulse) {
        clockPulse(host);
    }
    sendMidi(host, 0x90U, 48U, 100U);
    while (snapshot(host).currentPulse + 1U < endPulse) {
        clockPulse(host);
        const uint64_t pulse = snapshot(host).currentPulse;
        if (pulse == startPulse + 4U) {
            sendMidi(host, 0x91U, 72U, 100U);
        }
        if (pulse + 1U == endPulse) {
            sendMidi(host, 0x92U, 84U, 100U);
        }
    }
    stopCapture(host);
    const midibuffer::CaptureSnapshot captured = snapshot(host);
    expect(captured.timelineViewStartPulse == startPulse &&
               captured.timelineViewEndPulse == endPulse &&
               midibuffer::setPulseSelection(host.algorithm(), startPulse,
                                              endPulse),
           "callback MIDI and clock sequence creates the requested visible selection");
}

void verifyPlaybackHeadObservationAndWideMapping() {
    midibuffer_test::HostDouble head;
    preparePlaybackHeadHistory(head, 100U, 108U);
    expect(drawHasNoHead(head),
           "stopped transport draws no playback head");

    expect(midibuffer::startPlayback(head.algorithm()) &&
               snapshot(head).playbackArmed && drawHasNoHead(head),
           "armed transport draws no playback head before its opening clock");
    clockPulse(head);
    expect(snapshot(head).playbackActive &&
               snapshot(head).clockRunning &&
               snapshot(head).playbackIntervalOpen &&
               snapshot(head).activeSelectionValid &&
               snapshot(head).playbackPulse == 100U &&
               drawHasHeadAt(head, 4),
           "the actual opened playback pulse 100 maps to x4");

    const midibuffer_test::Trace& coincident = midibuffer_test::trace();
    size_t headTraceIndex = 0U;
    int coincidentX = -1;
    const bool oneHead =
        playbackHeadLineCount(&coincidentX, &headTraceIndex) == 1U;
    bool layerOrder = oneHead;
    for (size_t index = 0; index < coincident.shapeCallCount; ++index) {
        if (index < headTraceIndex) {
            layerOrder = layerOrder &&
                         (coincident.shapeCalls[index].colour == 5 ||
                          coincident.shapeCalls[index].colour == 9);
        } else if (index > headTraceIndex) {
            layerOrder =
                layerOrder && coincident.shapeCalls[index].colour == 15;
        }
    }
    bool coincidentColumnOccluded = true;
    for (int y = 17; y <= 55; ++y) {
        coincidentColumnOccluded = coincidentColumnOccluded &&
            midibuffer_test::framebufferPixel(4, y) == 15U;
    }
    expect(oneHead && shapeColourCount(12) == 1U &&
               coincidentX == 4 && layerOrder && coincidentColumnOccluded &&
               matchingShapeCount(4, 16, 8, 16, 15) == 1U &&
               matchingShapeCount(4, 56, 8, 56, 15) == 1U &&
               matchingShapeCount(251, 16, 247, 16, 15) == 1U &&
               matchingShapeCount(251, 56, 247, 56, 15) == 1U,
           "one y17..55 intensity-12 head follows baseline and notes, then coincident brackets fully occlude it with intact caps");

    for (uint32_t pulse = 0U; pulse < 4U; ++pulse) {
        clockPulse(head);
    }
    expect(snapshot(head).playbackPulse == 104U &&
               drawHasHeadAt(head, 127),
           "actual playback pulse 104 maps to x127");
    bool visibleColumnIsHead = true;
    for (int y = 17; y <= 55; ++y) {
        visibleColumnIsHead = visibleColumnIsHead &&
            midibuffer_test::framebufferPixel(127, y) == 12U;
    }
    expect(visibleColumnIsHead,
           "the unobscured head is exactly one solid 39-pixel intensity-12 column");

    const midibuffer::CaptureSnapshot beforeBetweenPulse = snapshot(head);
    midibuffer_test::resetTrace();
    sendMidi(head, 0x93U, 96U, 100U);
    expect(midibuffer_test::trace().shapeCallCount == 0U &&
               sameSelectionAndTransport(beforeBetweenPulse, snapshot(head)) &&
               drawHasHeadAt(head, 127) && drawHasHeadAt(head, 127),
           "between-pulse MIDI and repeated host draws neither force a frame nor move transport/head");

    const midibuffer::CaptureSnapshot beforeViewChange = snapshot(head);
    beginRightPotZoom(head, 0.5f);
    continueRightPotZoom(head, 0.0f);
    endRightPotZoom(head, 0.0f);
    const midibuffer::CaptureSnapshot remapped = snapshot(head);
    expect(!remapped.timelineShowAll &&
               remapped.timelineViewStartPulse == 104U &&
               remapped.timelineViewEndPulse == 108U &&
               sameSelectionAndTransport(beforeViewChange, remapped) &&
               drawHasHeadAt(head, 4),
           "view-only navigation remaps the observed pulse on the next draw without moving transport");
    beginRightPotZoom(head, 0.0f);
    continueRightPotZoom(head, 1.0f);
    endRightPotZoom(head, 1.0f);
    expect(snapshot(head).timelineShowAll &&
               snapshot(head).timelineViewStartPulse == 100U &&
               snapshot(head).timelineViewEndPulse == 108U,
           "head-state fixture returns to exact Show All without follow behavior");

    for (uint32_t pulse = 0U; pulse < 3U; ++pulse) {
        clockPulse(head);
    }
    expect(snapshot(head).playbackPulse == 107U &&
               drawHasHeadAt(head, 220),
           "actual playback pulse 107 maps to x220");
    clockPulse(head);
    expect(snapshot(head).playbackPulse == 100U &&
               drawHasHeadAt(head, 4),
           "ordinary wrap observes the committed active start without interpolation");

    noClockBlock(head);
    expect(snapshot(head).clockRunning && drawHasHeadAt(head, 4),
           "before loss is declared the last eligible open pulse remains visible");
    midibuffer_test::resetTrace();
    noClockBlock(head);
    expect(midibuffer_test::trace().shapeCallCount == 0U &&
               !snapshot(head).clockRunning &&
               snapshot(head).playbackClockLossPaused &&
               drawHasNoHead(head),
           "declared clock loss forces no frame and the next host draw has no stale head pixels");
    clockPulse(head);
    expect(snapshot(head).playbackClockLossPaused && drawHasNoHead(head),
           "the first reacquisition pulse remains ineligible");
    clockPulse(head);
    expect(snapshot(head).playbackActive && snapshot(head).clockRunning &&
               snapshot(head).playbackPulse == 100U &&
               drawHasHeadAt(head, 4),
           "the second reacquisition pulse reopens the saved playback interval");

    resetPulse(head);
    expect(snapshot(head).playbackArmed && drawHasNoHead(head),
           "reset alone repositions and arms playback without a head");
    coincidentResetAndClockPulse(head);
    expect(snapshot(head).playbackActive &&
               snapshot(head).playbackPulse == 100U &&
               drawHasHeadAt(head, 4),
           "coincident reset and valid clock exposes the actually opened start interval");

    expect(midibuffer::setPulseSelection(head.algorithm(), 104U, 108U),
           "pending playback-head range installs through the public selection seam");
    const midibuffer::CaptureSnapshot pending = snapshot(head);
    const bool pendingHeadAtOldActive = drawHasHeadAt(head, 4);
    expect(pending.rangeTransitionPending &&
               pending.activeSelection.startPulse == 100U &&
               pending.playbackPulse == 100U && pendingHeadAtOldActive &&
               matchingShapeCount(4, 16, 4, 56, 15) == 0U &&
               matchingShapeCount(127, 16, 127, 56, 15) == 1U &&
               matchingShapeCount(251, 16, 251, 56, 15) == 1U &&
               midibuffer_test::framebufferPixel(4, 17) == 12U,
           "pending brackets replace rather than duplicate active brackets while the head stays on old active playback");
    for (uint32_t pulse = 0U; pulse < 7U; ++pulse) {
        clockPulse(head);
    }
    expect(snapshot(head).rangeTransitionPending &&
               snapshot(head).activeSelection.startPulse == 100U &&
               snapshot(head).playbackPulse == 107U &&
               drawHasHeadAt(head, 220),
           "the old active head remains observable through the final pre-adoption pulse");
    clockPulse(head);
    expect(!snapshot(head).rangeTransitionPending &&
               snapshot(head).activeSelection.startPulse == 104U &&
               snapshot(head).playbackPulse == 104U &&
               drawHasHeadAt(head, 127),
           "pending-range wrap observes the new range only after transport adopts it");

    midibuffer_test::PresetImage playingImage;
    expect(head.savePreset(playingImage),
           "open playing head state saves through the production callback");
    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(1) && restored.loadPreset(playingImage),
           "open playing head state restores through the production callback");
    const midibuffer::CaptureSnapshot restoredWithoutClock = snapshot(restored);
    expect(restoredWithoutClock.playbackActive &&
               restoredWithoutClock.playbackIntervalOpen &&
               !restoredWithoutClock.clockRunning &&
               drawHasNoHead(restored),
           "restored logical Playing without acquired clock has no stale head");
    clockPulse(restored);
    expect(drawHasNoHead(restored),
           "restored Playing remains hidden on its first acquisition pulse");
    clockPulse(restored);
    expect(snapshot(restored).clockRunning &&
               snapshot(restored).playbackPulse == 104U &&
               drawHasHeadAt(restored, 127),
           "restored Playing shows its retained open pulse after second acquisition");
    midibuffer::stopPlayback(restored.algorithm());
    expect(drawHasNoHead(restored),
           "manual stop removes the head on the next host draw");

    midibuffer_test::HostDouble empty;
    expect(empty.instantiate(1) && drawHasNoHead(empty),
           "empty history draws only its existing baseline and no head");

    midibuffer_test::HostDouble clipped;
    preparePlaybackHeadHistory(clipped, 99U, 109U);
    expect(midibuffer::setTimelineNavigationFixture(
               clipped.algorithm(), false, 8U, 1U,
               midibuffer::kSelectionFineTargetEnd) &&
               snapshot(clipped).timelineViewStartPulse == 100U &&
               snapshot(clipped).timelineViewEndPulse == 108U &&
               midibuffer::startPlayback(clipped.algorithm()),
           "offscreen callback fixture fixes the visible half-open interval at [100,108)");
    clockPulse(clipped);
    expect(snapshot(clipped).playbackPulse == 99U && drawHasNoHead(clipped),
           "playback pulse 99 is excluded rather than pinned or marked with an arrow");
    clockPulse(clipped);
    expect(snapshot(clipped).playbackPulse == 100U &&
               drawHasHeadAt(clipped, 4) &&
               playbackHeadLineCount() == 1U &&
               directionalEdgeHandleLineCount(true, true) == 3U &&
               directionalEdgeHandleLineCount(false, false) == 3U,
           "the same fixed view includes one unchanged pulse-100 head with simultaneous offscreen boundary handles layered afterward");
    for (uint32_t pulse = 0U; pulse < 8U; ++pulse) {
        clockPulse(clipped);
    }
    expect(snapshot(clipped).playbackPulse == 108U &&
               snapshot(clipped).timelineViewStartPulse == 100U &&
               snapshot(clipped).timelineViewEndPulse == 108U &&
               drawHasNoHead(clipped),
           "playback pulse 108 is excluded with no edge pinning, arrow, or head-follow view change");

    const uint64_t wideStart = 0U;
    const uint64_t wideSpan = ~static_cast<uint64_t>(0);
    const uint64_t wideEnd = wideSpan;
    midibuffer_test::HostDouble wide;
    expect(wide.instantiate(1) &&
               midibuffer::setRetainedTimelineFixture(
                   wide.algorithm(), wideStart, wideEnd) &&
               midibuffer::addRetainedTimelineNoteFixture(
                   wide.algorithm(), wideStart + wideSpan / 2U, 72U) &&
               midibuffer::addRetainedTimelineNoteFixture(
                   wide.algorithm(), wideEnd - 1U, 84U) &&
               midibuffer::setPulseSelection(
                   wide.algorithm(), wideStart + wideSpan / 4U,
                   wideStart + (wideSpan / 4U) * 3U),
           "wide draw fixture installs notes and brackets across a span beyond uint32 without event traversal expansion");
    midibuffer_test::resetTrace();
    expect(wide.factory()->draw(wide.algorithm()) &&
               matchingShapeCount(4, 36, 4, 51, 9) == 1U &&
               matchingShapeCount(127, 34, 127, 51, 9) == 1U &&
               matchingShapeCount(250, 32, 250, 51, 9) == 1U &&
               matchingShapeCount(65, 16, 65, 56, 15) == 1U &&
               matchingShapeCount(189, 16, 189, 56, 15) == 1U &&
               matchingShapeCount(65, 16, 69, 16, 15) == 1U &&
               matchingShapeCount(189, 56, 185, 56, 15) == 1U &&
               playbackHeadLineCount() == 0U &&
               midibuffer_test::framebufferPixel(127, 34) == 9U &&
               midibuffer_test::framebufferPixel(65, 16) == 15U &&
               midibuffer_test::framebufferPixel(189, 56) == 15U,
           "shared full-uint64-safe mapping places wide notes at x4/x127/x250 and quarter brackets at x65/x189 with intact caps");
}

void verifyShowAllAndRelativeTimelineNavigation() {
    const uint64_t spans[] = {
        1U,
        64U,
        257U,
        (static_cast<uint64_t>(1U) << 32U) + 257U,
    };
    for (int megabytes = 1; megabytes <= 5; ++megabytes) {
        midibuffer_test::HostDouble host;
        expect(host.instantiate(megabytes),
               "Show All fixture constructs at an admitted buffer size");
        expect(midibuffer::setRetainedTimelineFixture(host.algorithm(), 0U,
                                                       0U),
               "empty retained-history fixture installs");
        const midibuffer::CaptureSnapshot empty = snapshot(host);
        expect(empty.timelineShowAll && empty.retainedPulseIntervals == 0U &&
                   empty.timelineViewStartPulse == 0U &&
                   empty.timelineViewEndPulse == 0U,
               "fresh empty history has Show All state and only the zero-width baseline");

        for (size_t index = 0; index < ARRAY_SIZE(spans); ++index) {
            const uint64_t start = 100U + static_cast<uint64_t>(megabytes);
            const uint64_t end = start + spans[index];
            expect(midibuffer::setRetainedTimelineFixture(host.algorithm(),
                                                           start, end),
                   "retained-span fixture installs without pulse iteration");
            const midibuffer::CaptureSnapshot current = snapshot(host);
            expect(current.timelineShowAll &&
                       current.retainedPulseIntervals == spans[index] &&
                       current.timelineViewStartPulse == start &&
                       current.timelineViewEndPulse == end,
                   "fresh Show All exactly fits every required retained span and buffer size");
        }
    }

    for (uint64_t span = 1U; span <= 4U; ++span) {
        midibuffer_test::HostDouble stationary;
        const uint64_t start = 100U;
        const uint64_t end = start + span;
        expect(stationary.instantiate(1) &&
                   midibuffer::setRetainedTimelineFixture(
                       stationary.algorithm(), start, end) &&
                   midibuffer::setPulseSelection(stationary.algorithm(),
                                                  start, start + 1U),
               "short-span stationary held-zoom fixture installs");
        const midibuffer::CaptureSnapshot before = snapshot(stationary);
        const HistoryImage history = captureHistory(stationary);

        beginRightPotZoom(stationary, 0.5f);
        continueRightPotZoom(stationary, 0.5f);
        continueRightPotZoom(stationary, 0.5f);
        endRightPotZoom(stationary, 0.5f);
        const midibuffer::CaptureSnapshot released = snapshot(stationary);
        expect(released.timelineShowAll &&
                   released.timelineViewStartPulse == start &&
                   released.timelineViewEndPulse == end &&
                   released.selection.startPulse ==
                       before.selection.startPulse &&
                   released.selection.endPulse == before.selection.endPulse &&
                   released.playbackArmed == before.playbackArmed &&
                   released.playbackActive == before.playbackActive &&
                   released.playbackClockLossPaused ==
                       before.playbackClockLossPaused &&
                   sameHistory(history, stationary),
               "repeated stationary held callbacks and release preserve Show All for retained spans one through four");

        expect(midibuffer::setRetainedTimelineFixture(stationary.algorithm(),
                                                       start, start + 8U),
               "short-span retained history grows beyond four pulses");
        const midibuffer::CaptureSnapshot grown = snapshot(stationary);
        expect(grown.timelineShowAll &&
                   grown.timelineViewStartPulse == start &&
                   grown.timelineViewEndPulse == start + 8U &&
                   grown.selection.startPulse == before.selection.startPulse &&
                   grown.selection.endPulse == before.selection.endPulse &&
                   grown.playbackArmed == before.playbackArmed &&
                   grown.playbackActive == before.playbackActive &&
                   grown.playbackClockLossPaused ==
                       before.playbackClockLossPaused &&
                   sameHistory(history, stationary),
               "stationary short-span Show All follows retained growth without selection, event, or transport mutation");
    }

    midibuffer_test::HostDouble growth;
    expect(growth.instantiate(1), "Show All growth host constructs");
    startCapture(growth);
    acquireClock(growth);
    sendMidi(growth, 0x90, 60, 100);
    const midibuffer::CaptureSnapshot first = snapshot(growth);
    expect(first.retainedPulseIntervals == 1U && first.timelineShowAll &&
               first.timelineViewStartPulse == first.oldestPulse &&
               first.timelineViewEndPulse == first.oldestPulse + 1U,
           "one event keeps its one-interval envelope in Show All");
    expect(midibuffer::setPulseSelection(growth.algorithm(), first.oldestPulse,
                                         first.oldestPulse + 1U),
           "growth fixture selects its initial event interval");
    const HistoryImage growthHistory = captureHistory(growth);
    for (uint32_t pulse = 0; pulse < 20U; ++pulse) {
        clockPulse(growth);
    }
    const midibuffer::CaptureSnapshot extended = snapshot(growth);
    expect(extended.timelineShowAll &&
               extended.timelineViewStartPulse == first.oldestPulse &&
               extended.timelineViewEndPulse ==
                   first.oldestPulse + extended.retainedPulseIntervals &&
               extended.retainedPulseIntervals > first.retainedPulseIntervals &&
               extended.selection.startPulse == first.oldestPulse &&
               extended.selection.endPulse == first.oldestPulse + 1U &&
               sameHistory(growthHistory, growth),
           "Show All follows capture-end growth without mutating selection or retained events");

    midibuffer_test::HostDouble navigation;
    expect(navigation.instantiate(1),
           "relative held-zoom navigation host constructs");
    expect(midibuffer::setRetainedTimelineFixture(navigation.algorithm(),
                                                   100U, 357U) &&
               midibuffer::setPulseSelection(navigation.algorithm(), 120U,
                                              140U),
           "relative zoom fixture installs retained bounds and selection");
    const HistoryImage navigationHistory = captureHistory(navigation);
    const midibuffer::CaptureSnapshot beforePress = snapshot(navigation);

    beginRightPotZoom(navigation, 0.37f);
    const midibuffer::CaptureSnapshot pressed = snapshot(navigation);
    expect(pressed.timelineShowAll &&
               pressed.timelineViewStartPulse ==
                   beforePress.timelineViewStartPulse &&
               pressed.timelineViewEndPulse == beforePress.timelineViewEndPulse &&
               pressed.selection.startPulse == beforePress.selection.startPulse &&
               pressed.selection.endPulse == beforePress.selection.endPulse &&
               sameHistory(navigationHistory, navigation),
           "held zoom starts relative to the current view with no press-entry jump");

    continueRightPotZoom(navigation, 0.0f);
    const midibuffer::CaptureSnapshot zoomedIn = snapshot(navigation);
    expect(!zoomedIn.timelineShowAll &&
               zoomedIn.timelineVisiblePulses == 4U &&
               zoomedIn.timelineViewEndPulse -
                       zoomedIn.timelineViewStartPulse ==
                   4U &&
               zoomedIn.selection.startPulse == beforePress.selection.startPulse &&
               zoomedIn.selection.endPulse == beforePress.selection.endPulse &&
               !zoomedIn.playbackArmed && !zoomedIn.playbackActive &&
               sameHistory(navigationHistory, navigation),
           "decreasing a held pot zooms in to the four-pulse manual minimum without transport or data mutation");
    endRightPotZoom(navigation, 0.0f);

    moveUi(navigation, 0U, 0.0f, 0.0f, 0.0f, 3, 0);
    const midibuffer::CaptureSnapshot older = snapshot(navigation);
    expect(older.timelineViewStartPulse + 3U ==
                   zoomedIn.timelineViewStartPulse &&
               older.timelineViewEndPulse + 3U == zoomedIn.timelineViewEndPulse,
           "encoder 1 positive delta scrolls exactly one pulse per unit toward older history");
    moveUi(navigation, 0U, 0.0f, 0.0f, 0.0f, -3, 0);
    const midibuffer::CaptureSnapshot newer = snapshot(navigation);
    expect(newer.timelineViewStartPulse == zoomedIn.timelineViewStartPulse &&
               newer.timelineViewEndPulse == zoomedIn.timelineViewEndPulse,
           "encoder 1 negative delta reverses precise manual scrolling toward newer history");

    beginRightPotZoom(navigation, 0.0f);
    const midibuffer::CaptureSnapshot secondPress = snapshot(navigation);
    expect(!secondPress.timelineShowAll &&
               secondPress.timelineViewStartPulse == newer.timelineViewStartPulse &&
               secondPress.timelineViewEndPulse == newer.timelineViewEndPulse,
           "a second held-zoom press also preserves the current manual view");
    continueRightPotZoom(navigation, 1.0f);
    endRightPotZoom(navigation, 1.0f);
    const midibuffer::CaptureSnapshot shownAll = snapshot(navigation);
    expect(shownAll.timelineShowAll && shownAll.timelineScrollPulses == 0U &&
               shownAll.timelineViewStartPulse == 100U &&
               shownAll.timelineViewEndPulse == 357U &&
               shownAll.selection.startPulse == beforePress.selection.startPulse &&
               shownAll.selection.endPulse == beforePress.selection.endPulse &&
               sameHistory(navigationHistory, navigation),
           "increasing held movement reaches exact full-span Show All and clears effective scroll");
    moveUi(navigation, 0U, 0.0f, 0.0f, 0.0f, 127, 0);
    const midibuffer::CaptureSnapshot showAllNoOp = snapshot(navigation);
    expect(showAllNoOp.timelineShowAll &&
               showAllNoOp.timelineScrollPulses == 0U &&
               showAllNoOp.timelineViewStartPulse == 100U &&
               showAllNoOp.timelineViewEndPulse == 357U,
           "encoder scrolling is a no-op that preserves automatic Show All");

    beginRightPotZoom(navigation, 0.5f);
    continueRightPotZoom(navigation, 0.49f);
    endRightPotZoom(navigation, 0.49f);
    const midibuffer::CaptureSnapshot manual = snapshot(navigation);
    expect(!manual.timelineShowAll &&
               manual.timelineVisiblePulses == 128U,
           "a small relative zoom-in leaves Show All at the next manual power-of-two span");
    moveUi(navigation, 0U, 0.0f, 0.0f, 0.0f, 10, 0);
    expect(midibuffer::setPulseSelection(navigation.algorithm(), 320U, 330U) &&
               midibuffer::setRetainedTimelineFixture(navigation.algorithm(),
                                                       300U, 364U),
           "eviction fixture preserves a still-retained selection while shrinking below manual width");
    moveUi(navigation, 0U, 0.0f, 0.0f, 0.0f);
    const midibuffer::CaptureSnapshot evicted = snapshot(navigation);
    expect(!evicted.timelineShowAll && evicted.timelineVisiblePulses == 128U &&
               evicted.timelineScrollPulses == 0U &&
               evicted.timelineViewStartPulse == 300U &&
               evicted.timelineViewEndPulse == 364U &&
               evicted.selection.startPulse == 320U &&
               evicted.selection.endPulse == 330U,
           "eviction clips an end-relative manual view without changing its configured width or mode");

    const HistoryImage clippedHistory = captureHistory(navigation);
    beginRightPotZoom(navigation, 0.5f);
    continueRightPotZoom(navigation, 0.5f);
    continueRightPotZoom(navigation, 0.5f);
    endRightPotZoom(navigation, 0.5f);
    const midibuffer::CaptureSnapshot clippedReleased = snapshot(navigation);
    expect(!clippedReleased.timelineShowAll &&
               clippedReleased.timelineVisiblePulses == 128U &&
               clippedReleased.timelineViewStartPulse == 300U &&
               clippedReleased.timelineViewEndPulse == 364U &&
               sameSelectionAndTransport(evicted, clippedReleased) &&
               sameHistory(clippedHistory, navigation),
           "stationary press, repeated held callbacks, and release preserve a clipped manual width, selection, events, and transport");

    expect(midibuffer::setRetainedTimelineFixture(navigation.algorithm(),
                                                   300U, 500U),
           "post-eviction growth fixture installs");
    const midibuffer::CaptureSnapshot regrown = snapshot(navigation);
    expect(!regrown.timelineShowAll &&
               regrown.timelineVisiblePulses == 128U &&
               regrown.timelineViewStartPulse == 372U &&
               regrown.timelineViewEndPulse == 500U &&
               sameSelectionAndTransport(evicted, regrown) &&
               sameHistory(clippedHistory, navigation),
           "the retained span can regrow to [300,500) and restore the configured 128-pulse end-relative view without mutation");

    const HistoryImage reversalHistory = captureHistory(navigation);
    beginRightPotZoom(navigation, 0.5f);
    continueRightPotZoom(navigation, 0.5f);
    const midibuffer::CaptureSnapshot stationaryPress = snapshot(navigation);
    expect(!stationaryPress.timelineShowAll &&
               stationaryPress.timelineVisiblePulses == 128U &&
               stationaryPress.timelineViewStartPulse ==
                   regrown.timelineViewStartPulse &&
               stationaryPress.timelineViewEndPulse ==
                   regrown.timelineViewEndPulse &&
               stationaryPress.selection.startPulse ==
                   regrown.selection.startPulse &&
               stationaryPress.selection.endPulse ==
                   regrown.selection.endPulse &&
               !stationaryPress.playbackArmed &&
               !stationaryPress.playbackActive &&
               sameHistory(reversalHistory, navigation),
           "a stationary initial held press leaves the manual view and capture state unchanged");
    continueRightPotZoom(navigation, 0.0f);
    const midibuffer::CaptureSnapshot reversalZoomedIn = snapshot(navigation);
    expect(!reversalZoomedIn.timelineShowAll &&
               reversalZoomedIn.timelineVisiblePulses == 4U,
           "manual reversal fixture first zooms in from the press-relative span");
    continueRightPotZoom(navigation, 0.5f);
    endRightPotZoom(navigation, 0.5f);
    const midibuffer::CaptureSnapshot reversedFromZoomIn = snapshot(navigation);
    expect(!reversedFromZoomIn.timelineShowAll &&
               reversedFromZoomIn.timelineVisiblePulses == 128U &&
               reversedFromZoomIn.timelineViewEndPulse -
                       reversedFromZoomIn.timelineViewStartPulse ==
                   128U &&
               reversedFromZoomIn.selection.startPulse ==
                   regrown.selection.startPulse &&
               reversedFromZoomIn.selection.endPulse ==
                   regrown.selection.endPulse &&
               !reversedFromZoomIn.playbackArmed &&
               !reversedFromZoomIn.playbackActive &&
               sameHistory(reversalHistory, navigation),
           "returning to the press coordinate after zoom-in restores the manual press-relative span without mutation");

    beginRightPotZoom(navigation, 0.5f);
    continueRightPotZoom(navigation, 1.0f);
    const midibuffer::CaptureSnapshot reversalZoomedOut = snapshot(navigation);
    expect(reversalZoomedOut.timelineShowAll &&
               reversalZoomedOut.timelineViewStartPulse == 300U &&
               reversalZoomedOut.timelineViewEndPulse == 500U,
           "manual reversal fixture first zooms out to exact Show All");
    continueRightPotZoom(navigation, 0.5f);
    endRightPotZoom(navigation, 0.5f);
    const midibuffer::CaptureSnapshot reversedFromZoomOut = snapshot(navigation);
    expect(!reversedFromZoomOut.timelineShowAll &&
               reversedFromZoomOut.timelineVisiblePulses == 128U &&
               reversedFromZoomOut.timelineViewEndPulse -
                       reversedFromZoomOut.timelineViewStartPulse ==
                   128U &&
               reversedFromZoomOut.selection.startPulse ==
                   regrown.selection.startPulse &&
               reversedFromZoomOut.selection.endPulse ==
                   regrown.selection.endPulse &&
               !reversedFromZoomOut.playbackArmed &&
               !reversedFromZoomOut.playbackActive &&
               sameHistory(reversalHistory, navigation),
           "returning to the press coordinate after zoom-out restores the manual press-relative span without mutation");

    midibuffer_test::HostDouble wideZoom;
    const uint64_t wideSpan =
        (static_cast<uint64_t>(1U) << 32U) + 257U;
    expect(wideZoom.instantiate(5) &&
               midibuffer::setRetainedTimelineFixture(wideZoom.algorithm(),
                                                       100U,
                                                       100U + wideSpan),
           "wide held-zoom fixture installs beyond the uint32 range");
    beginRightPotZoom(wideZoom, 0.5f);
    continueRightPotZoom(wideZoom, 0.0f);
    endRightPotZoom(wideZoom, 0.0f);
    expect(!snapshot(wideZoom).timelineShowAll &&
               snapshot(wideZoom).timelineVisiblePulses == 4U,
           "held zoom reaches the manual minimum from a span beyond 2^32");
    beginRightPotZoom(wideZoom, 0.0f);
    continueRightPotZoom(wideZoom, 1.0f);
    endRightPotZoom(wideZoom, 1.0f);
    const midibuffer::CaptureSnapshot wideShownAll = snapshot(wideZoom);
    expect(wideShownAll.timelineShowAll &&
               wideShownAll.timelineScrollPulses == 0U &&
               wideShownAll.timelineViewStartPulse == 100U &&
               wideShownAll.timelineViewEndPulse == 100U + wideSpan,
           "full held zoom-out reaches exact Show All beyond 2^32 without a 256 or uint32 cap");
}

void verifyZoomAwareBoundaryEditing() {
    struct RetrievalCase {
        bool startBoundary;
        uint64_t selectionStart;
        uint64_t selectionEnd;
        uint64_t scroll;
        uint64_t expected;
    };
    const RetrievalCase cases[] = {
        {true, 120U, 280U, 0U, 260U},
        {true, 180U, 220U, 160U, 140U},
        {false, 120U, 180U, 0U, 260U},
        {false, 120U, 280U, 160U, 140U},
        {true, 120U, 200U, 0U, 199U},
        {false, 200U, 280U, 160U, 201U},
    };
    for (size_t index = 0; index < ARRAY_SIZE(cases); ++index) {
        midibuffer_test::HostDouble host;
        expect(installRangeFixture(host, 100U, 300U,
                                   cases[index].selectionStart,
                                   cases[index].selectionEnd) &&
                   midibuffer::setTimelineNavigationFixture(
                       host.algorithm(), false, 40U, cases[index].scroll,
                       midibuffer::kSelectionFineTargetEnd),
               "offscreen boundary callback fixture installs");
        seedBoundaryPots(host, 0.4f, 0.6f);
        const midibuffer::CaptureSnapshot seeded = snapshot(host);
        const uint64_t scrollBefore = seeded.timelineScrollPulses;
        moveUi(host, cases[index].startBoundary ? kNT_potL : kNT_potC,
               0.4f, 0.6f, 0.0f);
        const midibuffer::CaptureSnapshot stationary = snapshot(host);
        expect(stationary.selection.startPulse == seeded.selection.startPulse &&
                   stationary.selection.endPulse == seeded.selection.endPulse &&
                   stationary.timelineScrollPulses == scrollBefore,
               "stationary changed-mask input does not retrieve an offscreen actual boundary");
        moveUi(host, cases[index].startBoundary ? kNT_potL : kNT_potC,
               cases[index].startBoundary ? 0.41f : 0.4f,
               cases[index].startBoundary ? 0.6f : 0.61f, 0.0f);
        const midibuffer::CaptureSnapshot retrieved = snapshot(host);
        const uint64_t actual = cases[index].startBoundary
                                    ? retrieved.selection.startPulse
                                    : retrieved.selection.endPulse;
        expect(actual == cases[index].expected &&
                   retrieved.selection.startPulse <
                       retrieved.selection.endPulse &&
                   retrieved.timelineScrollPulses == scrollBefore,
               "first deliberate callback retrieves either offscreen boundary with ordering and no viewport scroll");
    }

    midibuffer_test::HostDouble subsequent;
    expect(installRangeFixture(subsequent, 100U, 300U, 120U, 280U) &&
               midibuffer::setTimelineNavigationFixture(
                   subsequent.algorithm(), false, 40U, 0U,
                   midibuffer::kSelectionFineTargetEnd),
           "post-retrieval motion fixture installs");
    seedBoundaryPots(subsequent, 0.4f, 0.6f);
    moveUi(subsequent, kNT_potL, 0.41f, 0.6f, 0.0f);
    moveUi(subsequent, kNT_potL, 0.44f, 0.6f, 0.0f);
    const midibuffer::CaptureSnapshot afterFineMotion = snapshot(subsequent);
    expect(afterFineMotion.selection.startPulse == 261U &&
               afterFineMotion.selection.endPulse == 280U &&
               afterFineMotion.timelineViewStartPulse == 260U &&
               afterFineMotion.timelineViewEndPulse == 300U &&
               afterFineMotion.timelineScrollPulses == 0U,
           "motion after retrieval uses the forty-pulse viewport scale and remains pulse-aligned without scrolling");

    midibuffer_test::HostDouble large;
    midibuffer_test::HostDouble small;
    expect(installRangeFixture(large, 900U, 1000U, 997U, 1000U) &&
               installRangeFixture(small, 900U, 1000U, 997U, 1000U) &&
               midibuffer::setTimelineNavigationFixture(
                   large.algorithm(), false, 100U, 0U,
                   midibuffer::kSelectionFineTargetStart) &&
               midibuffer::setTimelineNavigationFixture(
                   small.algorithm(), false, 4U, 0U,
                   midibuffer::kSelectionFineTargetStart),
           "large and small viewport scaling fixtures install");
    seedBoundaryPots(large, 0.5f, 1.0f);
    seedBoundaryPots(small, 0.5f, 1.0f);
    moveUi(large, kNT_potL, 0.25f, 1.0f, 0.0f);
    moveUi(small, kNT_potL, 0.25f, 1.0f, 0.0f);
    expect(snapshot(large).selection.startPulse == 972U &&
               snapshot(small).selection.startPulse == 996U &&
               snapshot(large).selection.endPulse == 1000U &&
               snapshot(small).selection.endPulse == 1000U,
           "identical deliberate Start travel is finer when zoomed in and both results are whole pulses");

    midibuffer_test::HostDouble largeEnd;
    midibuffer_test::HostDouble smallEnd;
    expect(installRangeFixture(largeEnd, 900U, 1000U, 900U, 903U) &&
               installRangeFixture(smallEnd, 900U, 1000U, 900U, 903U) &&
               midibuffer::setTimelineNavigationFixture(
                   largeEnd.algorithm(), false, 100U, 0U,
                   midibuffer::kSelectionFineTargetEnd) &&
               midibuffer::setTimelineNavigationFixture(
                   smallEnd.algorithm(), false, 4U, 96U,
                   midibuffer::kSelectionFineTargetEnd),
           "End scaling fixtures install with the boundary visible at both scales");
    seedBoundaryPots(largeEnd, 0.0f, 0.5f);
    seedBoundaryPots(smallEnd, 0.0f, 0.5f);
    moveUi(largeEnd, kNT_potC, 0.0f, 0.75f, 0.0f);
    moveUi(smallEnd, kNT_potC, 0.0f, 0.75f, 0.0f);
    expect(snapshot(largeEnd).selection.endPulse == 928U &&
               snapshot(smallEnd).selection.endPulse == 904U &&
               snapshot(largeEnd).selection.startPulse == 900U &&
               snapshot(smallEnd).selection.startPulse == 900U,
           "identical deliberate End travel is finer when zoomed in and both results are whole pulses");

    midibuffer_test::HostDouble wide;
    const uint64_t wideEnd = static_cast<uint64_t>(1U) << 40U;
    expect(installRangeFixture(wide, 0U, wideEnd, 0U, wideEnd),
           "wide boundary callback fixture installs beyond uint32");
    seedBoundaryPots(wide, 0.5f, 0.5f);
    moveUi(wide, kNT_potL, 0.75f, 0.5f, 0.0f);
    expect(snapshot(wide).selection.startPulse ==
                   (static_cast<uint64_t>(1U) << 38U) &&
               snapshot(wide).selection.endPulse == wideEnd,
           "production Start callback scales a wide viewport without overflow or fractional pulses");

    midibuffer_test::HostDouble shortSpan;
    expect(installRangeFixture(shortSpan, 100U, 103U, 100U, 103U),
           "sub-minimum zoom-only fixture installs");
    const midibuffer::CaptureSnapshot shortBefore = snapshot(shortSpan);
    beginRightPotZoom(shortSpan, 0.5f);
    continueRightPotZoom(shortSpan, 0.0f);
    endRightPotZoom(shortSpan, 0.0f);
    const midibuffer::CaptureSnapshot shortAfter = snapshot(shortSpan);
    expect(shortAfter.timelineShowAll &&
               shortAfter.timelineViewStartPulse == 100U &&
               shortAfter.timelineViewEndPulse == 103U &&
               sameSelectionAndTransport(shortBefore, shortAfter),
           "deliberate zoom on a sub-minimum span preserves the only valid viewport and phrase");

    midibuffer_test::HostDouble playing;
    expect(installRangeFixture(playing, 100U, 300U, 120U, 180U),
           "playing zoom-invariance fixture installs");
    seedBoundaryPots(playing, 0.4f, 0.6f);
    clockPulse(playing);
    clockPulse(playing);
    expect(midibuffer::startPlayback(playing.algorithm()),
           "zoom-invariance fixture arms playback");
    clockPulse(playing);
    expect(midibuffer::setTimelineNavigationFixture(
               playing.algorithm(), false, 40U, 0U,
               midibuffer::kSelectionFineTargetEnd),
           "playing fixture fixes an end-relative manual viewport");
    const midibuffer::CaptureSnapshot beforeZoom = snapshot(playing);
    beginRightPotZoom(playing, 0.5f);
    continueRightPotZoom(playing, 0.0f);
    endRightPotZoom(playing, 0.0f);
    const midibuffer::CaptureSnapshot zoomed = snapshot(playing);
    expect(beforeZoom.playbackActive && beforeZoom.activeSelectionValid &&
               zoomed.timelineVisiblePulses == 4U &&
               zoomed.timelineViewStartPulse !=
                   beforeZoom.timelineViewStartPulse &&
               sameSelectionAndTransport(beforeZoom, zoomed),
           "zoom-only production callbacks change the viewport while selected and active playing phrases remain invariant");

    expect(midibuffer::setTimelineNavigationFixture(
               playing.algorithm(), false, 40U, 0U,
               midibuffer::kSelectionFineTargetEnd),
           "pending-edit fixture restores the forty-pulse viewport");
    moveUi(playing, kNT_potC, 0.4f, 0.61f, 0.0f);
    const midibuffer::CaptureSnapshot pending = snapshot(playing);
    expect(pending.selection.startPulse == 120U &&
               pending.selection.endPulse == 260U &&
               pending.activeSelection.startPulse == 120U &&
               pending.activeSelection.endPulse == 180U &&
               pending.rangeTransitionPending &&
               pending.timelineScrollPulses == 0U,
           "deliberate offscreen End retrieval updates only the pending selected phrase for next-wrap adoption");
    beginRightPotZoom(playing, 0.5f);
    continueRightPotZoom(playing, 0.0f);
    endRightPotZoom(playing, 0.0f);
    const midibuffer::CaptureSnapshot pendingZoomed = snapshot(playing);
    expect(pendingZoomed.timelineVisiblePulses == 4U &&
               sameSelectionAndTransport(pending, pendingZoomed),
           "zoom-only callbacks preserve both active and pending ranges before next-wrap adoption");
}

void verifyIntegratedRangeMotionAndFineTargets() {
    midibuffer_test::HostDouble range;
    expect(installRangeFixture(range, 100U, 200U, 120U, 140U),
           "integrated [100,200)/[120,140) range fixture installs");

    _NT_float3 setupPots = {-1.0f, -1.0f, -1.0f};
    range.factory()->setupUi(range.algorithm(), setupPots);
    expectNear(setupPots[2], 0.25, 1.0e-6,
               "setupUi reports the logical normal range position rather than zoom");
    const midibuffer::CaptureSnapshot beforeEntry = snapshot(range);
    moveRightPot(range, 0.25f);
    const midibuffer::CaptureSnapshot entered = snapshot(range);
    expect(entered.rangeMotionEstablished &&
               entered.selection.startPulse == beforeEntry.selection.startPulse &&
               entered.selection.endPulse == beforeEntry.selection.endPulse,
           "UI entry suppresses even a changed-pot callback while sampling the physical pot");

    moveRightPot(range, 0.75f);
    midibuffer::CaptureSnapshot moved = snapshot(range);
    expect(moved.selection.startPulse == 160U &&
               moved.selection.endPulse == 180U &&
               moved.selectionFineTarget ==
                   midibuffer::kSelectionFineTargetRange,
           "unpressed pot 3 translates the exact 20-pulse pair and selects Range");
    moveRightPot(range, 1.0f);
    moved = snapshot(range);
    expect(moved.selection.startPulse == 180U &&
               moved.selection.endPulse == 200U,
           "range motion clamps at [180,200) without wrapping or shortening");
    moveRightPot(range, 0.0f);
    moved = snapshot(range);
    expect(moved.selection.startPulse == 100U &&
               moved.selection.endPulse == 120U,
           "range reversal reaches [100,120) without clamp debt");

    for (size_t index = 0;
         index < ARRAY_SIZE(kPulsesPerDisplayedBeatValues); ++index) {
        const uint64_t phraseLength =
            8U * kPulsesPerDisplayedBeatValues[index];
        midibuffer_test::HostDouble musical;
        expect(installRangeFixture(musical, 100U, 1000U, 200U,
                                   200U + phraseLength),
               "two-bar range fixture installs for a supported pulse domain");
        changeParameter(musical, kPulsesPerDisplayedBeatParameter,
                        static_cast<int16_t>(index));
        seedRightPot(musical, 0.2f);
        moveRightPot(musical, 0.3f);
        moveUi(musical, 0U, 0.0f, 0.0f, 0.3f, 0, 1);
        const midibuffer::CaptureSnapshot current = snapshot(musical);
        expect(current.selection.endPulse - current.selection.startPulse ==
                       phraseLength &&
                   current.selection.startPulse >= 100U &&
                   current.selection.endPulse <= 1000U,
               "pot and encoder Range motion retain eight beats for every supported P");
    }

    struct CatchUpVector {
        double logical;
        float physical;
        float nextPhysical;
        double expectedLogical;
    };
    const CatchUpVector vectors[] = {
        {0.2, 0.8f, 0.81f, 0.2175},
        {0.8, 0.2f, 0.19f, 0.7825},
        {0.2, 0.8f, 0.79f, 0.1975},
    };
    for (size_t index = 0; index < ARRAY_SIZE(vectors); ++index) {
        midibuffer_test::HostDouble vector;
        const uint64_t start =
            static_cast<uint64_t>(vectors[index].logical * 100.0 + 0.5);
        expect(installRangeFixture(vector, 0U, 110U, start, start + 10U),
               "production callback catch-up vector fixture installs");
        seedRightPot(vector, vectors[index].physical);
        moveRightPot(vector, vectors[index].nextPhysical);
        expectNear(snapshot(vector).rangeLogicalPosition,
                   vectors[index].expectedLogical, 2.0e-7,
                   "production pot callback applies the normative catch-up vector");
    }

    midibuffer_test::HostDouble hold;
    expect(installRangeFixture(hold, 100U, 200U, 120U, 140U),
           "repeated held-zoom reconciliation fixture installs");
    seedRightPot(hold, 0.8f);
    moveRightPot(hold, 0.81f);
    const midibuffer::CaptureSnapshot unfinished = snapshot(hold);
    expect(unfinished.rangePulseResidual != 0.0 &&
               unfinished.rangePhysicalPosition >
                   unfinished.rangeLogicalPosition,
           "production callback retains sub-pulse residual and unfinished mismatch");
    beginRightPotZoom(hold, 0.81f);
    continueRightPotZoom(hold, 0.95f);
    endRightPotZoom(hold, 0.7f);
    beginRightPotZoom(hold, 0.7f);
    continueRightPotZoom(hold, 0.6f);
    endRightPotZoom(hold, 0.6f);
    const midibuffer::CaptureSnapshot repeatedlyHeld = snapshot(hold);
    expectNear(repeatedlyHeld.rangeLogicalPosition,
               unfinished.rangeLogicalPosition, 1.0e-12,
               "repeated holds preserve unfinished logical reconciliation");
    expectNear(repeatedlyHeld.rangePulseResidual,
               unfinished.rangePulseResidual, 1.0e-12,
               "repeated holds preserve unfinished pulse residual");
    expect(repeatedlyHeld.selectionFineTarget ==
               unfinished.selectionFineTarget,
           "repeated held zoom preserves the prior fine target");
    const uint64_t beforeReversal = repeatedlyHeld.selection.startPulse;
    moveRightPot(hold, 0.5f);
    expect(snapshot(hold).selection.startPulse < beforeReversal,
           "first post-release travel reverses immediately from the actual release sample");
    const midibuffer::CaptureSnapshot afterReversal = snapshot(hold);
    moveRightPot(hold, 0.5f);
    expect(snapshot(hold).selection.startPulse ==
                   afterReversal.selection.startPulse &&
               snapshot(hold).rangeLogicalPosition ==
                   afterReversal.rangeLogicalPosition,
           "stationary reconciliation has no second jump");

    midibuffer_test::HostDouble releaseEdge;
    expect(installRangeFixture(releaseEdge, 100U, 200U, 120U, 140U),
           "release-edge suppression fixture installs");
    seedRightPot(releaseEdge, 0.25f);
    beginRightPotZoom(releaseEdge, 0.25f);
    continueRightPotZoom(releaseEdge, 0.9f);
    moveUi(releaseEdge, kNT_potR, 0.0f, 0.0f, 0.4f, 0, 0,
           kNT_potButtonR);
    expect(snapshot(releaseEdge).selection.startPulse == 120U &&
               snapshot(releaseEdge).selection.endPulse == 140U,
           "release-with-pot-change seeds from release and suppresses edge translation");
    moveRightPot(releaseEdge, 0.3f);
    expect(snapshot(releaseEdge).selection.startPulse < 120U,
           "first unpressed range delta is measured from the release sample");

    midibuffer_test::HostDouble repeatedEntry;
    expect(installRangeFixture(repeatedEntry, 100U, 200U, 120U, 140U),
           "repeated setupUi takeover fixture installs");
    _NT_float3 firstSetup = {-1.0f, -1.0f, -1.0f};
    _NT_float3 secondSetup = {-1.0f, -1.0f, -1.0f};
    repeatedEntry.factory()->setupUi(repeatedEntry.algorithm(), firstSetup);
    repeatedEntry.factory()->setupUi(repeatedEntry.algorithm(), secondSetup);
    const midibuffer::CaptureSnapshot beforePhysicalSeed = snapshot(repeatedEntry);
    moveRightPot(repeatedEntry, 0.8f);
    const midibuffer::CaptureSnapshot seededEntry = snapshot(repeatedEntry);
    moveRightPot(repeatedEntry, 0.81f);
    const midibuffer::CaptureSnapshot movedAfterEntry = snapshot(repeatedEntry);
    expectNear(firstSetup[2], 0.25, 1.0e-6,
               "first setupUi switch reports the logical Range position");
    expectNear(secondSetup[2], 0.25, 1.0e-6,
               "repeated setupUi switches reuse the logical Range position");
    expect(seededEntry.selection.startPulse ==
                   beforePhysicalSeed.selection.startPulse &&
               seededEntry.selection.endPulse ==
                   beforePhysicalSeed.selection.endPulse &&
               movedAfterEntry.rangeLogicalPosition >
                   seededEntry.rangeLogicalPosition,
           "repeated UI entry samples the actual pot without remap and the first following delta has no host pickup dead zone");

    midibuffer_test::PresetImage takeoverImage;
    expect(repeatedEntry.savePreset(takeoverImage),
           "takeover state serializes for fresh-load callback verification");
    midibuffer_test::HostDouble loadedTakeover;
    expect(loadedTakeover.instantiate(1) &&
               loadedTakeover.loadPreset(takeoverImage),
           "takeover state loads into a fresh actual callback host");
    _NT_float3 loadedSetup = {-1.0f, -1.0f, -1.0f};
    loadedTakeover.factory()->setupUi(loadedTakeover.algorithm(), loadedSetup);
    const midibuffer::CaptureSnapshot loadedBeforeSeed = snapshot(loadedTakeover);
    moveRightPot(loadedTakeover, 0.1f);
    const midibuffer::CaptureSnapshot loadedSeeded = snapshot(loadedTakeover);
    moveRightPot(loadedTakeover, 0.09f);
    const midibuffer::CaptureSnapshot loadedMoved = snapshot(loadedTakeover);
    expect(loadedSeeded.selection.startPulse ==
                   loadedBeforeSeed.selection.startPulse &&
               loadedSeeded.selection.endPulse ==
                   loadedBeforeSeed.selection.endPulse &&
               loadedMoved.rangeLogicalPosition <
                   loadedSeeded.rangeLogicalPosition,
           "valid-preset load seeds from the actual sampled pot and responds to the first subsequent delta without replay or pickup");

    midibuffer_test::HostDouble targets;
    expect(installRangeFixture(targets, 100U, 300U, 120U, 140U),
           "fine-target and callback-order fixture installs");
    seedBoundaryPots(targets, 0.5f, 0.5f, 0.1f);
    moveUi(targets, 0U, 0.5f, 0.5f, 0.1f, 0, 1);
    expect(snapshot(targets).selection.startPulse == 120U &&
               snapshot(targets).selection.endPulse == 141U &&
               snapshot(targets).selectionFineTarget ==
                   midibuffer::kSelectionFineTargetEnd,
           "fresh encoder target is End and steps it by one pulse");
    moveUi(targets, kNT_potL, 0.49f, 0.5f, 0.1f);
    const midibuffer::CaptureSnapshot startTarget = snapshot(targets);
    moveUi(targets, 0U, 0.49f, 0.5f, 0.1f, 0, 1);
    expect(snapshot(targets).selection.startPulse ==
                   startTarget.selection.startPulse + 1U &&
               snapshot(targets).selection.endPulse == 141U,
           "effective pot 1 selects Start for an independent one-pulse nudge");
    moveUi(targets, kNT_potL, 0.49f, 0.5f, 0.1f);
    moveUi(targets, kNT_potC, 0.49f, 0.6f, 0.1f);
    const midibuffer::CaptureSnapshot endTarget = snapshot(targets);
    moveUi(targets, kNT_potL, 0.49f, 0.6f, 0.1f);
    moveUi(targets, 0U, 0.49f, 0.6f, 0.1f, 0, -1);
    expect(snapshot(targets).selection.startPulse ==
                   endTarget.selection.startPulse &&
               snapshot(targets).selection.endPulse + 1U ==
                   endTarget.selection.endPulse,
           "a stationary pot boundary does not steal the prior effective End target");

    expect(midibuffer::setPulseSelection(targets.algorithm(), 120U, 140U),
           "Range fine-target pair resets through the public selection seam");
    seedRightPot(targets, 0.1f);
    moveRightPot(targets, 0.3f);
    const midibuffer::CaptureSnapshot rangeTarget = snapshot(targets);
    moveUi(targets, 0U, 0.0f, 0.0f, 0.3f, 0, 1);
    const midibuffer::CaptureSnapshot nudged = snapshot(targets);
    expect(nudged.selection.startPulse == rangeTarget.selection.startPulse + 1U &&
               nudged.selection.endPulse == rangeTarget.selection.endPulse + 1U &&
               nudged.selection.endPulse - nudged.selection.startPulse == 20U &&
               nudged.rangePhysicalPosition ==
                   rangeTarget.rangePhysicalPosition &&
               nudged.rangePulseResidual == 0.0,
           "Range nudge moves one pulse, retains length and physical baseline, and clears residual");
    beginRightPotZoom(targets, 0.3f);
    continueRightPotZoom(targets, 0.0f);
    endRightPotZoom(targets, 0.0f);
    moveUi(targets, 0U, 0.0f, 0.0f, 0.0f, 3, 0);
    const midibuffer::CaptureSnapshot beforeRetainedNudge = snapshot(targets);
    moveUi(targets, 0U, 0.0f, 0.0f, 0.0f, 0, -1);
    expect(snapshot(targets).selection.startPulse + 1U ==
                   beforeRetainedNudge.selection.startPulse &&
               snapshot(targets).selection.endPulse + 1U ==
                   beforeRetainedNudge.selection.endPulse,
           "held zoom and encoder-1 scroll preserve the Range fine target");

    midibuffer_test::HostDouble clampedTarget;
    expect(installRangeFixture(clampedTarget, 100U, 200U, 180U, 200U),
           "clamped target fixture installs");
    seedRightPot(clampedTarget, 0.8f);
    moveRightPot(clampedTarget, 0.9f);
    moveUi(clampedTarget, 0U, 0.0f, 0.0f, 0.9f, 0, -1);
    expect(snapshot(clampedTarget).selection.startPulse == 180U &&
               snapshot(clampedTarget).selection.endPulse == 199U,
           "clamped ineffective Range motion does not steal fresh End target");

    midibuffer_test::HostDouble ordered;
    expect(installRangeFixture(ordered, 100U, 200U, 120U, 140U),
           "same-callback ordering fixture installs");
    seedRightPot(ordered, 0.25f);
    moveUi(ordered, kNT_potC | kNT_potR, 0.0f, 0.0f, 0.75f,
           0, 1);
    const midibuffer::CaptureSnapshot orderedResult = snapshot(ordered);
    expect(orderedResult.selection.startPulse == 175U &&
               orderedResult.selection.endPulse == 176U &&
               orderedResult.selectionFineTarget ==
                   midibuffer::kSelectionFineTargetRange,
           "same callback applies pot 2, post-edit-length Range motion, then encoder 2");

    midibuffer_test::HostDouble invalid;
    expect(installRangeFixture(invalid, 100U, 200U, 120U, 140U),
           "invalid Range target fixture installs");
    seedRightPot(invalid, 0.25f);
    moveRightPot(invalid, 0.5f);
    midibuffer::clearPulseSelection(invalid.algorithm());
    moveRightPot(invalid, 0.6f);
    moveUi(invalid, 0U, 0.0f, 0.0f, 0.6f, 0, 1);
    expect(!snapshot(invalid).selectionValid,
           "invalid Range target cannot recreate an overwritten selection");

    midibuffer_test::HostDouble full;
    expect(installRangeFixture(full, 100U, 200U, 100U, 200U),
           "full-history range fixture installs");
    seedRightPot(full, 0.2f);
    moveRightPot(full, 0.9f);
    expect(snapshot(full).selection.startPulse == 100U &&
               snapshot(full).selection.endPulse == 200U,
           "full-history selection is a Range no-op");
    midibuffer::setRetainedTimelineFixture(full.algorithm(), 0U, 0U);
    moveRightPot(full, 0.1f);
    expect(snapshot(full).selection.startPulse == 100U &&
               snapshot(full).selection.endPulse == 200U,
           "empty retained history does not move or rescue the stale pair");

    midibuffer_test::HostDouble stationary;
    expect(installRangeFixture(stationary, 100U, 200U, 120U, 140U),
           "stationary domain-rebase fixture installs");
    seedRightPot(stationary, 0.8f);
    moveRightPot(stationary, 0.801f);
    const midibuffer::PulseRange stationaryPair = snapshot(stationary).selection;
    expect(midibuffer::setRetainedTimelineFixture(stationary.algorithm(),
                                                   90U, 210U),
           "retained domain grows around the selected pair");
    seedRightPot(stationary, 0.801f);
    const midibuffer::CaptureSnapshot rebased = snapshot(stationary);
    expect(rebased.selection.startPulse == stationaryPair.startPulse &&
               rebased.selection.endPulse == stationaryPair.endPulse &&
               rebased.rangePulseResidual == 0.0 &&
               rebased.rangePhysicalPosition ==
                   static_cast<double>(0.801f),
           "stationary retained-domain change rebases the pair, retains physical baseline, and clears residual");
    changeParameter(stationary, kPulsesPerDisplayedBeatParameter, 6);
    beginRightPotZoom(stationary, 0.801f);
    continueRightPotZoom(stationary, 0.7f);
    endRightPotZoom(stationary, 0.7f);
    expect(snapshot(stationary).selection.startPulse == stationaryPair.startPulse &&
               snapshot(stationary).selection.endPulse == stationaryPair.endPulse,
           "stationary P and view changes do not remap selection");

    const uint64_t maximum = ~static_cast<uint64_t>(0);
    const uint64_t wideTravel = static_cast<uint64_t>(1U) << 40U;
    const uint64_t wideStart = maximum - wideTravel - 100U;
    midibuffer_test::HostDouble wide;
    expect(installRangeFixture(wide, wideStart, maximum,
                               wideStart + 100U,
                               wideStart + 120U),
           "wide production-callback range fixture installs near UINT64_MAX");
    seedRightPot(wide, 0.0f);
    moveRightPot(wide, 0.5f);
    const midibuffer::CaptureSnapshot wideMoved = snapshot(wide);
    expect(wideMoved.selection.endPulse - wideMoved.selection.startPulse == 20U &&
               wideMoved.selection.startPulse >= wideStart &&
               wideMoved.selection.endPulse <= maximum,
           "wide callback motion preserves exact integer length and legal bounds");
    moveUi(wide, 0U, 0.0f, 0.0f, 0.5f, 0, 127);
    expect(snapshot(wide).selection.startPulse ==
                   wideMoved.selection.startPulse + 127U &&
               snapshot(wide).selection.endPulse ==
                   wideMoved.selection.endPulse + 127U,
           "large encoder delta remains one pulse per unit without acceleration");
    const midibuffer::PulseRange beforeWideRebase = snapshot(wide).selection;
    expect(midibuffer::setRetainedTimelineFixture(wide.algorithm(),
                                                   wideStart - 10U,
                                                   maximum),
           "wide retained-domain rebase fixture expands safely");
    seedRightPot(wide, 0.5f);
    const midibuffer::CaptureSnapshot wideRebased = snapshot(wide);
    expect(wideRebased.selection.startPulse == beforeWideRebase.startPulse &&
               wideRebased.selection.endPulse == beforeWideRebase.endPulse &&
               wideRebased.selection.endPulse -
                       wideRebased.selection.startPulse ==
                   20U &&
               wideRebased.rangePulseResidual == 0.0,
           "wide stationary domain rebase keeps the actual pair and clears residual");
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
    seedRightPot(sparse, 0.0f);
    moveRightPot(sparse, 0.1f);
    expect(snapshot(sparse).selectionFineTarget ==
               midibuffer::kSelectionFineTargetRange &&
               midibuffer::setPulseSelection(
                   sparse.algorithm(), full.oldestPulse,
                   full.oldestPulse + 1U),
           "rolling-history fixture selects Range then restores the oldest pair");
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
    moveRightPot(sparse, 0.2f);
    moveUi(sparse, 0U, 0.0f, 0.0f, 0.2f, 0, 1);
    expect(!snapshot(sparse).selectionValid,
           "pot and encoder Range controls do not rescue overwritten history");
    expect(!midibuffer::acquirePlaybackSelection(sparse.algorithm(),
                                                 playbackRange),
           "playback-entry contract refuses an invalidated selection");
    midibuffer_test::resetTrace();
    sparse.factory()->draw(sparse.algorithm());
    expect(drawContainsSubstring("Avail ") &&
               midibuffer_test::trace().shapeCallCount != 0U &&
               midibuffer_test::litFramebufferPixelCount() != 0U,
           "overwritten rolling history redraws current availability and timeline pixels");

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
        {0x91, 48, 100}, {0x81, 48, 0},   {0x92, 49, 100},
        {0xA3, 50, 60},  {0xB4, 1, 72},   {0xD5, 80, 0},
        {0xE6, 0, 64},
    };
    const uint8_t messageSizes[] = {3, 3, 3, 3, 3, 2, 3};
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
    changeParameter(host, kPlaybackParameter, 1);
    expect(snapshot(host).playbackArmed && host.parameter(kPlaybackParameter) == 1,
           "transport playback arms through the shared Playback parameter");
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

void verifyAtomicRangeTransitionAtWrap() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "range-transition trace host constructs");
    startCapture(host);
    expect(clockPulseAt(host, 0U) && clockPulseAt(host, 16U),
           "range-transition history acquires its source clock");
    const uint64_t originalStart = snapshot(host).currentPulse;
    sendMidi(host, 0x92, 60, 100);
    sendMidi(host, 0xB2, 64, 127);
    expect(clockPulseAt(host, 32U) && clockPulseAt(host, 47U),
           "original range records a complete two-pulse phrase");
    const uint64_t replacementStart = snapshot(host).currentPulse;
    sendMidi(host, 0x93, 70, 100);
    expect(clockPulseAt(host, 63U),
           "replacement range advances to its release pulse");
    sendMidi(host, 0x83, 70, 0);
    expect(clockPulseAt(host, 79U),
           "range-transition history closes both selectable phrases");
    stopCapture(host);

    expect(midibuffer::setPulseSelection(host.algorithm(), originalStart,
                                         originalStart + 2U),
           "original playing range is selected");
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "range-transition playback arms without stopping later edits");
    expect(clockPulseAt(host, 95U),
           "original range starts on the next clock pulse");
    stepThrough(host, 103U);
    const midibuffer::CaptureSnapshot playingOriginal = snapshot(host);
    expect(playingOriginal.playbackActive &&
               playingOriginal.activeSelectionValid &&
               playingOriginal.activeSelection.startPulse == originalStart &&
               playingOriginal.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 2U,
           "active range emits its held note and sustain before any edit");

    midibuffer_test::resetTrace();
    expect(midibuffer::setPulseSelection(host.algorithm(),
                                         originalStart + 1U,
                                         originalStart + 3U) &&
               midibuffer::setPulseSelection(host.algorithm(),
                                             replacementStart,
                                             replacementStart + 2U),
           "repeated in-phrase edits accept complete boundary pairs");
    midibuffer::CaptureSnapshot pending = snapshot(host);
    expect(pending.playbackActive && pending.rangeTransitionPending &&
               pending.playbackPulse == originalStart &&
               pending.selection.startPulse == replacementStart &&
               pending.selection.endPulse == replacementStart + 2U &&
               pending.activeSelection.startPulse == originalStart &&
               pending.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "latest selected pair remains pending while the active pair keeps "
           "playing");

    expect(clockPulseAt(host, 111U),
           "original range reaches its final pulse after the edits");
    pending = snapshot(host);
    expect(pending.playbackActive && pending.rangeTransitionPending &&
               pending.playbackPulse == originalStart + 1U &&
               pending.activeSelection.startPulse == originalStart &&
               pending.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "pending edits cause no mid-phrase jump or output");

    midibuffer_test::resetTrace();
    expect(clockPulseAt(host, 127U),
           "current range reaches the wrap that adopts the edit");
    const midibuffer_test::Trace& transition = midibuffer_test::trace();
    const midibuffer::CaptureSnapshot adopted = snapshot(host);
    expect(transition.midiCallCount == 3U &&
               transition.midiCalls[0].dispatchSample == 127U &&
               transition.midiCalls[0].bytes[0] == 0x82U &&
               transition.midiCalls[0].bytes[1] == 60U &&
               transition.midiCalls[1].dispatchSample == 127U &&
               transition.midiCalls[1].bytes[0] == 0xB2U &&
               transition.midiCalls[1].bytes[1] == 64U &&
               transition.midiCalls[1].bytes[2] == 0U &&
               transition.midiCalls[2].dispatchSample == 127U &&
               transition.midiCalls[2].bytes[0] == 0x93U &&
               transition.midiCalls[2].bytes[1] == 70U,
           "changed wrap orders old note-off and sustain-off before the latest "
           "range's first attack");
    expect(adopted.playbackActive && !adopted.rangeTransitionPending &&
               adopted.playbackPulse == replacementStart &&
               adopted.activeSelection.startPulse == replacementStart &&
               adopted.activeSelection.endPulse == replacementStart + 2U,
           "latest boundary pair becomes active atomically without restart");

    expect(clockPulseAt(host, 143U),
           "replacement range reaches its recorded note-off");
    midibuffer_test::resetTrace();
    expect(clockPulseAt(host, 159U),
           "unchanged replacement range reaches its ordinary wrap");
    const midibuffer_test::Trace& unchanged = midibuffer_test::trace();
    expect(unchanged.midiCallCount == 1U &&
               unchanged.midiCalls[0].bytes[0] == 0x93U &&
               unchanged.midiCalls[0].bytes[1] == 70U &&
               snapshot(host).activeSelection.startPulse == replacementStart &&
               !snapshot(host).rangeTransitionPending,
           "unchanged wrap follows ordinary playback without transition "
           "cleanup");
}

void verifyCallbackRangeEditsAtWrap() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1),
           "callback range-transition trace host constructs");
    startCapture(host);
    expect(clockPulseAt(host, 0U) && clockPulseAt(host, 16U),
           "callback range history acquires its source clock");
    const uint64_t originalStart = snapshot(host).currentPulse;
    sendMidi(host, 0x92U, 60U, 100U);
    sendMidi(host, 0xB2U, 64U, 127U);
    expect(clockPulseAt(host, 32U) && clockPulseAt(host, 47U),
           "callback range history records the original held phrase");
    const uint64_t replacementStart = snapshot(host).currentPulse;
    sendMidi(host, 0x93U, 70U, 100U);
    expect(clockPulseAt(host, 63U),
           "callback range history advances to the replacement release");
    sendMidi(host, 0x83U, 70U, 0U);
    expect(clockPulseAt(host, 79U),
           "callback range history closes its retained envelope");
    stopCapture(host);

    expect(midibuffer::setPulseSelection(host.algorithm(), originalStart,
                                         originalStart + 2U),
           "callback range fixture selects the original pair");
    seedRightPot(host, 0.0f);
    moveRightPot(host, 0.34f);
    const midibuffer::CaptureSnapshot stoppedMoved = snapshot(host);
    expect(stoppedMoved.selection.startPulse == originalStart + 1U &&
               stoppedMoved.selection.endPulse == originalStart + 3U &&
               stoppedMoved.selectionFineTarget ==
                   midibuffer::kSelectionFineTargetRange &&
               !stoppedMoved.activeSelectionValid &&
               !stoppedMoved.rangeTransitionPending,
           "stopped pot-3 Range motion publishes immediately without a pending transport transition");
    moveUi(host, 0U, 0.0f, 0.0f, 0.34f, 0, -1);
    const midibuffer::CaptureSnapshot stoppedRestored = snapshot(host);
    expect(stoppedRestored.selection.startPulse == originalStart &&
               stoppedRestored.selection.endPulse == originalStart + 2U &&
               !stoppedRestored.rangeTransitionPending &&
               !stoppedRestored.playbackArmed &&
               !stoppedRestored.playbackActive,
           "stopped Range nudge updates the selected pair directly and retains stopped handling");

    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "callback range fixture arms through the production transport seam");
    expect(clockPulseAt(host, 95U),
           "callback range fixture starts the original pass");
    stepThrough(host, 103U);
    const midibuffer::CaptureSnapshot beforeEdits = snapshot(host);
    expect(beforeEdits.playbackActive &&
               beforeEdits.playbackPulse == originalStart &&
               beforeEdits.activeSelection.startPulse == originalStart &&
               beforeEdits.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 2U,
           "callback range fixture opens with note and sustain held");

    midibuffer_test::resetTrace();
    moveRightPot(host, 0.70f);
    const midibuffer::CaptureSnapshot potPending = snapshot(host);
    expect(potPending.selection.startPulse == replacementStart &&
               potPending.selection.endPulse == replacementStart + 2U &&
               potPending.rangeTransitionPending &&
               potPending.playbackPulse == beforeEdits.playbackPulse &&
               potPending.playbackEventIndex == beforeEdits.playbackEventIndex &&
               potPending.activeSelection.startPulse == originalStart &&
               potPending.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "running pot-3 Range motion changes only the latest pending pair mid-pass");

    moveUi(host, 0U, 0.0f, 0.0f, 0.70f, 0, -2);
    const midibuffer::CaptureSnapshot canceled = snapshot(host);
    expect(canceled.selection.startPulse == originalStart &&
               canceled.selection.endPulse == originalStart + 2U &&
               !canceled.rangeTransitionPending &&
               canceled.playbackPulse == beforeEdits.playbackPulse &&
               canceled.activeSelection.startPulse == originalStart &&
               canceled.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "returning through the Range control to the active pair cancels the pending transition");

    moveUi(host, 0U, 0.0f, 0.0f, 0.70f, 0, 1);
    const midibuffer::CaptureSnapshot firstReplacement = snapshot(host);
    moveUi(host, 0U, 0.0f, 0.0f, 0.70f, 0, 1);
    const midibuffer::CaptureSnapshot latestReplacement = snapshot(host);
    expect(firstReplacement.selection.startPulse == originalStart + 1U &&
               firstReplacement.rangeTransitionPending &&
               latestReplacement.selection.startPulse == replacementStart &&
               latestReplacement.selection.endPulse == replacementStart + 2U &&
               latestReplacement.rangeTransitionPending &&
               latestReplacement.playbackPulse == beforeEdits.playbackPulse &&
               latestReplacement.activeSelection.startPulse == originalStart &&
               latestReplacement.activeSelection.endPulse ==
                   originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "several running Range nudges retain only the latest complete pair without moving the active cursor");

    expect(clockPulseAt(host, 111U),
           "callback range fixture reaches the final original pulse");
    const midibuffer::CaptureSnapshot beforeWrap = snapshot(host);
    expect(beforeWrap.rangeTransitionPending &&
               beforeWrap.playbackPulse == originalStart + 1U &&
               beforeWrap.activeSelection.startPulse == originalStart &&
               beforeWrap.activeSelection.endPulse == originalStart + 2U &&
               midibuffer_test::trace().midiCallCount == 0U,
           "latest callback edit remains pending through the final active pulse");

    midibuffer_test::resetTrace();
    expect(clockPulseAt(host, 127U),
           "callback range fixture reaches its publication wrap");
    const midibuffer_test::Trace& transition = midibuffer_test::trace();
    const midibuffer::CaptureSnapshot adopted = snapshot(host);
    expect(transition.midiCallCount == 3U &&
               transition.midiCalls[0].dispatchSample == 127U &&
               transition.midiCalls[0].bytes[0] == 0x82U &&
               transition.midiCalls[0].bytes[1] == 60U &&
               transition.midiCalls[1].dispatchSample == 127U &&
               transition.midiCalls[1].bytes[0] == 0xB2U &&
               transition.midiCalls[1].bytes[1] == 64U &&
               transition.midiCalls[1].bytes[2] == 0U &&
               transition.midiCalls[2].dispatchSample == 127U &&
               transition.midiCalls[2].bytes[0] == 0x93U &&
               transition.midiCalls[2].bytes[1] == 70U,
           "callback publication keeps old note-off and sustain-off ahead of the latest range attack at one wrap timestamp");
    expect(adopted.playbackActive && !adopted.rangeTransitionPending &&
               adopted.playbackPulse == replacementStart &&
               adopted.selection.startPulse == replacementStart &&
               adopted.selection.endPulse == replacementStart + 2U &&
               adopted.activeSelection.startPulse == replacementStart &&
               adopted.activeSelection.endPulse == replacementStart + 2U,
           "the latest callback-selected pair becomes active exactly at wrap");
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
               cleanup.midiCalls[1].bytes[2] == 0U &&
               cleanup.parameterSetCallCount == 1U &&
               cleanup.parameterSetCalls[0].source ==
                   midibuffer_test::kParameterSetFromAudio &&
               cleanup.parameterSetCalls[0].parameter ==
                   kPlaybackParameter + NT_parameterOffset() &&
               cleanup.parameterSetCalls[0].value == 0 &&
               cleanup.maximumParameterCallbackDepth == 1U,
           "manual stop publishes host Playback Off through the offset audio setter before routed note-off then sustain-off cleanup");
    expect(!snapshot(stopHost).playbackActive &&
               stopHost.parameter(kPlaybackParameter) == 0 &&
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
               host.parameter(kPlaybackParameter) == 1 &&
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

struct LoopTailFixture {
    uint64_t selectionStart;
    uint64_t lastClockSample;
};

LoopTailFixture prepareLoopTailHistory(midibuffer_test::HostDouble& host) {
    LoopTailFixture fixture = {};
    expect(host.instantiate(1), "loop-tail ownership host constructs");
    expect(clockPulseAt(host, 0U) && clockPulseAt(host, 16U),
           "loop-tail history acquires a 16-sample source clock");
    startCapture(host);

    // This attack precedes the selection. Its release lies inside, but neither
    // event may fabricate or otherwise include the note in playback.
    sendMidi(host, 0x90, 50, 100);
    expect(clockPulseAt(host, 32U),
           "loop-tail history reaches the selected start pulse");
    fixture.selectionStart = snapshot(host).currentPulse;
    sendMidi(host, 0x90, 60, 100);

    expect(clockPulseAt(host, 48U),
           "loop-tail history reaches its second selected pulse");
    sendMidi(host, 0x80, 50, 0);
    sendMidi(host, 0x91, 61, 100);
    sendMidi(host, 0xB2, 64, 127);
    sendMidi(host, 0xB3, 64, 127);

    expect(clockPulseAt(host, 64U),
           "short note and pedal endings are recorded outside selection");
    sendMidi(host, 0x81, 61, 0);
    sendMidi(host, 0xB2, 64, 0);
    expect(clockPulseAt(host, 80U),
           "long note and overlapping pedal endings remain retained");
    sendMidi(host, 0x80, 60, 0);
    sendMidi(host, 0xB3, 64, 0);
    expect(clockPulseAt(host, 96U),
           "loop-tail history closes after every outside ending");
    stopCapture(host);
    expect(midibuffer::setPulseSelection(host.algorithm(),
                                         fixture.selectionStart,
                                         fixture.selectionStart + 2U),
           "two-pulse loop-tail range is selectable");

    fixture.lastClockSample = 96U;
    for (uint32_t index = 0; index < 8U; ++index) {
        fixture.lastClockSample += 24U;
        expect(clockPulseAt(host, fixture.lastClockSample),
               "loop-tail playback tempo priming pulse is delivered");
    }
    expect(snapshot(host).predictedClockIntervalSamples == 24U,
           "loop-tail playback clock converges to the slower tempo");
    return fixture;
}

void openLoopTailFirstPass(midibuffer_test::HostDouble& host,
                           LoopTailFixture& fixture) {
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "loop-tail playback arms");
    fixture.lastClockSample += 24U;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "loop-tail playback starts on its first selected pulse");
    stepThrough(host, fixture.lastClockSample + 12U);
    fixture.lastClockSample += 24U;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "loop-tail playback reaches its second selected pulse");
    stepThrough(host, fixture.lastClockSample + 12U);
}

void verifyLoopTailDurationsAndSustainOwnership() {
    midibuffer_test::HostDouble host;
    LoopTailFixture fixture = prepareLoopTailHistory(host);
    openLoopTailFirstPass(host, fixture);

    const uint64_t playbackStart = fixture.lastClockSample - 24U;
    const midibuffer_test::Trace& firstPass = midibuffer_test::trace();
    expect(firstPass.midiCallCount == 4U &&
               firstPass.midiCalls[0].dispatchSample == playbackStart + 12U &&
               firstPass.midiCalls[0].bytes[0] == 0x90U &&
               firstPass.midiCalls[0].bytes[1] == 60U &&
               firstPass.midiCalls[1].dispatchSample == playbackStart + 36U &&
               firstPass.midiCalls[1].bytes[0] == 0x91U &&
               firstPass.midiCalls[1].bytes[1] == 61U &&
               firstPass.midiCalls[2].bytes[0] == 0xB2U &&
               firstPass.midiCalls[2].bytes[2] == 127U &&
               firstPass.midiCalls[3].bytes[0] == 0xB3U &&
               firstPass.midiCalls[3].bytes[2] == 127U,
           "only in-range attacks and presses play at tempo-scaled offsets");
    bool preSelectionNoteAbsent = true;
    for (size_t index = 0; index < firstPass.midiCallCount; ++index) {
        preSelectionNoteAbsent =
            preSelectionNoteAbsent && firstPass.midiCalls[index].bytes[1] != 50U;
    }
    expect(preSelectionNoteAbsent,
           "a note begun before selection receives no fabricated attack or release");
    expect(snapshot(host).pendingNoteEndingCount == 2U &&
               snapshot(host).pendingSustainReleaseCount == 2U,
           "in-range attacks and presses retain their outside-range endings");

    fixture.lastClockSample += 24U;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "ordinary wrap keeps pending note and sustain ownership");
    stepThrough(host, fixture.lastClockSample + 12U);
    const midibuffer_test::Trace& wrapped = midibuffer_test::trace();
    expect(wrapped.midiCallCount == 8U &&
               wrapped.midiCalls[4].dispatchSample == playbackStart + 60U &&
               wrapped.midiCalls[4].bytes[0] == 0x80U &&
               wrapped.midiCalls[4].bytes[1] == 60U &&
               wrapped.midiCalls[5].bytes[0] == 0x90U &&
               wrapped.midiCalls[5].bytes[1] == 60U &&
               wrapped.midiCalls[6].bytes[0] == 0x81U &&
               wrapped.midiCalls[6].bytes[1] == 61U &&
               wrapped.midiCalls[7].bytes[0] == 0xB2U &&
               wrapped.midiCalls[7].bytes[1] == 64U &&
               wrapped.midiCalls[7].bytes[2] == 0U,
           "wrap retriggers the long note safely while the short note and pedal "
           "keep their recorded 24-sample scaled lifetimes");

    fixture.lastClockSample += 24U;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "second pass reaches overlapping presses");
    stepThrough(host, fixture.lastClockSample + 12U);
    const midibuffer_test::Trace& overlap = midibuffer_test::trace();
    bool staleLongNoteOffAbsent = true;
    bool staleSustainOffAbsent = true;
    for (size_t index = 8U; index < overlap.midiCallCount; ++index) {
        const midibuffer_test::MidiCall& call = overlap.midiCalls[index];
        if (call.dispatchSample == playbackStart + 84U &&
            call.bytes[0] == 0x80U && call.bytes[1] == 60U) {
            staleLongNoteOffAbsent = false;
        }
        if (call.dispatchSample == playbackStart + 84U &&
            call.bytes[0] == 0xB3U && call.bytes[1] == 64U &&
            call.bytes[2] < 64U) {
            staleSustainOffAbsent = false;
        }
    }
    expect(staleLongNoteOffAbsent,
           "a canceled older note ending cannot cut its replacement short");
    expect(staleSustainOffAbsent &&
               snapshot(host).pendingSustainReleaseCount == 2U,
           "the newest overlapping pedal press owns release and may sustain continuously");
}

void verifyOverriddenChannelSustainReleaseOwnership() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "override sustain ownership host constructs");
    expect(clockPulseAt(host, 0U) && clockPulseAt(host, 16U),
           "override sustain history acquires source clock");
    startCapture(host);

    expect(clockPulseAt(host, 32U),
           "override sustain reaches selection start");
    const uint64_t startPulse = snapshot(host).currentPulse;
    sendMidi(host, 0xB0, 64, 127);
    expect(clockPulseAt(host, 48U),
           "override sustain records newer source-channel press");
    sendMidi(host, 0xB1, 64, 127);
    expect(clockPulseAt(host, 64U),
           "override sustain records older release inside selection");
    sendMidi(host, 0xB0, 64, 0);
    expect(clockPulseAt(host, 80U),
           "override sustain records newest release beyond selection");
    sendMidi(host, 0xB1, 64, 0);
    expect(clockPulseAt(host, 96U),
           "override sustain history closes");
    stopCapture(host);

    expect(midibuffer::setPulseSelection(host.algorithm(), startPulse,
                                         startPulse + 3U),
           "override sustain range includes the canceled older release");
    changeParameter(host, kPlaybackChannelParameter, 5);

    uint64_t lastClock = 96U;
    for (uint32_t index = 0; index < 8U; ++index) {
        lastClock += 16U;
        expect(clockPulseAt(host, lastClock),
               "override sustain tempo priming pulse is delivered");
    }
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "override sustain playback arms");
    const uint64_t playbackStart = lastClock + 16U;
    for (uint32_t interval = 0; interval < 7U; ++interval) {
        lastClock += 16U;
        expect(clockPulseAt(host, lastClock),
               "override sustain multi-pass clock is delivered");
        stepThrough(host, lastClock + 8U);
    }

    const uint64_t expectedPressSamples[] = {
        playbackStart + 8U,
        playbackStart + 24U,
        playbackStart + 56U,
        playbackStart + 72U,
        playbackStart + 104U,
    };
    const midibuffer_test::Trace& trace = midibuffer_test::trace();
    bool onlyNewestOwnedPresses =
        trace.midiCallCount == ARRAY_SIZE(expectedPressSamples);
    for (size_t index = 0;
         index < trace.midiCallCount &&
         index < ARRAY_SIZE(expectedPressSamples);
         ++index) {
        const midibuffer_test::MidiCall& call = trace.midiCalls[index];
        onlyNewestOwnedPresses =
            onlyNewestOwnedPresses &&
            call.dispatchSample == expectedPressSamples[index] &&
            call.bytes[0] == 0xB4U && call.bytes[1] == 64U &&
            call.bytes[2] == 127U;
    }
    expect(onlyNewestOwnedPresses,
           "canceled in-range sustain releases cannot bypass routed newest-press ownership across wraps");
    expect(snapshot(host).pendingSustainReleaseCount == 1U,
           "newest routed sustain press retains sole release ownership after repeated wraps");
}

void verifyOverriddenChannelRetriggerOwnership() {
    midibuffer_test::HostDouble host;
    expect(host.instantiate(1), "override collision host constructs");
    expect(clockPulseAt(host, 0U) && clockPulseAt(host, 16U),
           "override collision history acquires source clock");
    startCapture(host);
    expect(clockPulseAt(host, 32U),
           "override collision reaches selection start");
    const uint64_t startPulse = snapshot(host).currentPulse;
    sendMidi(host, 0x90, 64, 100);
    expect(clockPulseAt(host, 48U),
           "override collision reaches second source channel attack");
    sendMidi(host, 0x91, 64, 110);
    expect(clockPulseAt(host, 64U),
           "override collision records first outside ending");
    sendMidi(host, 0x81, 64, 0);
    expect(clockPulseAt(host, 80U),
           "override collision records second outside ending");
    sendMidi(host, 0x80, 64, 0);
    expect(clockPulseAt(host, 96U),
           "override collision history closes");
    stopCapture(host);
    expect(midibuffer::setPulseSelection(host.algorithm(), startPulse,
                                         startPulse + 2U),
           "override collision range is selectable");
    changeParameter(host, kPlaybackChannelParameter, 5);

    uint64_t lastClock = 96U;
    for (uint32_t index = 0; index < 8U; ++index) {
        lastClock += 16U;
        expect(clockPulseAt(host, lastClock),
               "override collision tempo priming pulse is delivered");
    }
    midibuffer_test::resetTrace();
    expect(midibuffer::startPlayback(host.algorithm()),
           "override collision playback arms");
    lastClock += 16U;
    expect(clockPulseAt(host, lastClock),
           "override collision starts first source attack");
    stepThrough(host, lastClock + 8U);
    lastClock += 16U;
    expect(clockPulseAt(host, lastClock),
           "override collision reaches replacement attack");
    stepThrough(host, lastClock + 8U);
    const midibuffer_test::Trace& collision = midibuffer_test::trace();
    expect(collision.midiCallCount == 3U &&
               collision.midiCalls[0].bytes[0] == 0x94U &&
               collision.midiCalls[1].bytes[0] == 0x84U &&
               collision.midiCalls[1].bytes[1] == 64U &&
               collision.midiCalls[2].bytes[0] == 0x94U &&
               collision.midiCalls[2].bytes[1] == 64U,
           "channel override releases the older same-pitch owner before replacement");

    lastClock += 16U;
    expect(clockPulseAt(host, lastClock),
           "override collision reaches the older ending interval");
    stepThrough(host, lastClock + 8U);
    const midibuffer_test::Trace& afterWrap = midibuffer_test::trace();
    expect(afterWrap.midiCallCount == 5U &&
               afterWrap.midiCalls[3].bytes[0] == 0x84U &&
               afterWrap.midiCalls[4].bytes[0] == 0x94U,
           "next-pass retrigger has exactly one release before one attack and no stale ending");
}

void expectNoPendingEndings(midibuffer_test::HostDouble& host,
                            const char* message) {
    expect(snapshot(host).pendingNoteEndingCount == 0U &&
               snapshot(host).pendingSustainReleaseCount == 0U,
           message);
}

void verifyPendingEndingDiscontinuityCleanup() {
    {
        midibuffer_test::HostDouble host;
        LoopTailFixture fixture = prepareLoopTailHistory(host);
        openLoopTailFirstPass(host, fixture);
        midibuffer::stopPlayback(host.algorithm());
        expectNoPendingEndings(host,
                               "manual stop cancels every pending recorded ending");
    }
    {
        midibuffer_test::HostDouble host;
        LoopTailFixture fixture = prepareLoopTailHistory(host);
        openLoopTailFirstPass(host, fixture);
        resetPulse(host);
        expectNoPendingEndings(host,
                               "reset cancels every pending recorded ending");
    }
    {
        midibuffer_test::HostDouble host;
        LoopTailFixture fixture = prepareLoopTailHistory(host);
        openLoopTailFirstPass(host, fixture);
        while (snapshot(host).clockRunning) {
            noClockBlock(host);
        }
        expect(snapshot(host).playbackClockLossPaused,
               "pending-ending clock-loss case reaches paused cleanup");
        expectNoPendingEndings(host,
                               "clock loss cancels every pending recorded ending");
    }
    {
        midibuffer_test::HostDouble host;
        LoopTailFixture fixture = prepareLoopTailHistory(host);
        openLoopTailFirstPass(host, fixture);
        expect(midibuffer::setPulseSelection(host.algorithm(),
                                             fixture.selectionStart + 2U,
                                             fixture.selectionStart + 4U),
               "pending-ending range replacement is accepted");
        fixture.lastClockSample += 24U;
        expect(clockPulseAt(host, fixture.lastClockSample),
               "pending-ending range replacement reaches adoption wrap");
        expectNoPendingEndings(host,
                               "range transition cancels previous-range endings");
    }
}

bool panicTraceMatches(uint32_t destination) {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    if (current.midiCallCount < 32U) {
        return false;
    }
    const size_t panicStart = current.midiCallCount - 32U;
    bool matches = true;
    bool sustainCleanupSeen = false;
    for (size_t index = 0; index < panicStart; ++index) {
        const uint8_t type = current.midiCalls[index].bytes[0] & 0xf0U;
        if (type == 0xb0U && current.midiCalls[index].bytes[1] == 64U) {
            sustainCleanupSeen = true;
        } else {
            matches = matches && type == 0x80U && !sustainCleanupSeen;
        }
        matches = matches &&
                  current.midiCalls[index].destination == destination;
    }
    for (uint8_t channel = 0; channel < 16U; ++channel) {
        const size_t soundOffIndex =
            panicStart + static_cast<size_t>(channel) * 2U;
        const size_t notesOffIndex = soundOffIndex + 1U;
        if (notesOffIndex >= current.midiCallCount) {
            matches = false;
            continue;
        }
        const uint8_t status = static_cast<uint8_t>(0xb0U | channel);
        matches = matches &&
                  current.midiCalls[soundOffIndex].destination == destination &&
                  current.midiCalls[soundOffIndex].bytes[0] == status &&
                  current.midiCalls[soundOffIndex].bytes[1] == 120U &&
                  current.midiCalls[soundOffIndex].bytes[2] == 0U &&
                  current.midiCalls[notesOffIndex].destination == destination &&
                  current.midiCalls[notesOffIndex].bytes[0] == status &&
                  current.midiCalls[notesOffIndex].bytes[1] == 123U &&
                  current.midiCalls[notesOffIndex].bytes[2] == 0U;
    }
    return matches;
}

void setEncoderButton(midibuffer_test::HostDouble& host, uint16_t button,
                      bool heldPreviously) {
    moveUi(host, button, 0.0f, 0.0f, 0.0f, 0, 0,
           heldPreviously ? button : 0U);
}

void releaseEncoderButton(midibuffer_test::HostDouble& host,
                          uint16_t button) {
    moveUi(host, 0U, 0.0f, 0.0f, 0.0f, 0, 0, button);
}

void verifyLiveEmergencySilenceMatrix() {
    midibuffer_test::HostDouble host;
    prepareTransportHistory(host);
    const uint32_t destinations[] = {
        kNT_destinationBreakout,
        kNT_destinationUSB,
        kNT_destinationSelectBus,
        kNT_destinationInternal,
        kNT_destinationBreakout | kNT_destinationUSB |
            kNT_destinationSelectBus | kNT_destinationInternal,
    };

    bool completeMatrixMatches = true;
    for (int destinationSetting = 0; destinationSetting < 5;
         ++destinationSetting) {
        changeParameter(host, kPlaybackDestinationParameter,
                        static_cast<int16_t>(destinationSetting));
        for (uint8_t incomingChannel = 0; incomingChannel < 16U;
             ++incomingChannel) {
            const uint8_t controllers[] = {120U, 123U};
            for (size_t controllerIndex = 0;
                 controllerIndex < ARRAY_SIZE(controllers);
                 ++controllerIndex) {
                changeParameter(host, kPlaybackParameter, 1);
                completeMatrixMatches =
                    snapshot(host).playbackArmed &&
                    host.parameter(kPlaybackParameter) == 1 &&
                    completeMatrixMatches;
                clockPulse(host);
                completeMatrixMatches = snapshot(host).playbackActive &&
                                        completeMatrixMatches;
                const uint64_t savedPulse = snapshot(host).playbackPulse;

                midibuffer_test::resetTrace();
                sendMidi(host,
                         static_cast<uint8_t>(0xb0U | incomingChannel),
                         controllers[controllerIndex], 99U);
                completeMatrixMatches =
                    panicTraceMatches(destinations[destinationSetting]) &&
                    host.parameter(kPlaybackParameter) == 0 &&
                    midibuffer_test::trace().parameterSetCallCount == 1U &&
                    midibuffer_test::trace().parameterSetCalls[0].source ==
                        midibuffer_test::kParameterSetFromAudio &&
                    midibuffer_test::trace().parameterSetCalls[0].parameter ==
                        kPlaybackParameter + NT_parameterOffset() &&
                    !snapshot(host).playbackArmed &&
                    !snapshot(host).playbackActive &&
                    !snapshot(host).playbackClockLossPaused &&
                    !snapshot(host).captureEnabled &&
                    snapshot(host).playbackPulse == savedPulse &&
                    completeMatrixMatches;

                midibuffer_test::resetTrace();
                clockPulse(host);
                clockPulse(host);
                completeMatrixMatches =
                    midibuffer_test::trace().midiCallCount == 0U &&
                    !snapshot(host).playbackActive && completeMatrixMatches;
            }
        }
    }
    expect(completeMatrixMatches,
           "incoming CC120/123 on every channel panic all 16 channels only on each selected destination, preserve position, and stay stopped across later clocks");

    midibuffer_test::HostDouble stopHost;
    TransportFixture stopFixture = prepareTransportHistory(stopHost);
    beginTransportOnHeldFirstBeat(stopHost, stopFixture);
    midibuffer_test::resetTrace();
    stopHost.factory()->midiRealtime(stopHost.algorithm(), 0xFCU);
    expect(snapshot(stopHost).playbackActive &&
               midibuffer_test::trace().midiCallCount == 0U,
           "incoming MIDI Stop neither panics nor stops patched-clock playback");
    const uint64_t pulseBeforeStopMessage = snapshot(stopHost).playbackPulse;
    clockPulse(stopHost);
    bool noPanicMessages = true;
    for (size_t index = 0;
         index < midibuffer_test::trace().midiCallCount; ++index) {
        const uint8_t controller =
            midibuffer_test::trace().midiCalls[index].bytes[1];
        noPanicMessages = noPanicMessages && controller != 120U &&
                          controller != 123U;
    }
    expect(snapshot(stopHost).playbackActive &&
               snapshot(stopHost).playbackPulse != pulseBeforeStopMessage &&
               noPanicMessages,
           "playback advances without panic on the pulse after ignored MIDI Stop");
}

void verifyTimelinePlaybackToggle() {
    midibuffer_test::HostDouble invalid;
    expect(invalid.instantiate(1), "invalid-selection toggle host constructs");
    startCapture(invalid);
    acquireClock(invalid);
    sendMidi(invalid, 0x90U, 48U, 100U);
    const uint32_t eventCountBefore = snapshot(invalid).eventCount;
    setEncoderButton(invalid, kNT_encoderButtonL, false);
    setEncoderButton(invalid, kNT_encoderButtonL, true);
    expect(!snapshot(invalid).playbackArmed &&
               !snapshot(invalid).playbackActive &&
               snapshot(invalid).captureEnabled &&
               snapshot(invalid).eventCount == eventCountBefore &&
               invalid.parameter(kPlaybackParameter) == 1,
           "left-encoder rising edge leaves an unavailable continuing request On while held input neither repeats nor stops capture");
    releaseEncoderButton(invalid, kNT_encoderButtonL);

    midibuffer_test::HostDouble host;
    prepareTransportHistory(host);
    setEncoderButton(host, kNT_encoderButtonL, false);
    setEncoderButton(host, kNT_encoderButtonL, true);
    expect(snapshot(host).playbackArmed && !snapshot(host).captureEnabled &&
               host.parameter(kPlaybackParameter) == 1,
           "one left-encoder rising edge sets Playback On and a held callback does not immediately stop");
    releaseEncoderButton(host, kNT_encoderButtonL);

    midibuffer_test::resetTrace();
    setEncoderButton(host, kNT_encoderButtonL, false);
    setEncoderButton(host, kNT_encoderButtonL, true);
    expect(!snapshot(host).playbackArmed &&
               !snapshot(host).playbackActive &&
               host.parameter(kPlaybackParameter) == 0 &&
               midibuffer_test::trace().midiCallCount == 0U,
           "a new rising edge sets Playback Off and its held callback does not rearm");
    releaseEncoderButton(host, kNT_encoderButtonL);

    setEncoderButton(host, kNT_encoderButtonL, false);
    setEncoderButton(host, kNT_encoderButtonL, true);
    expect(snapshot(host).playbackArmed &&
               host.parameter(kPlaybackParameter) == 1,
           "a later rising edge restores shared Playback On after release");
    releaseEncoderButton(host, kNT_encoderButtonL);
    clockPulse(host);
    expect(snapshot(host).playbackActive,
           "left-encoder playback waits for and starts on the next valid clock pulse");

    const uint64_t savedPulse = snapshot(host).playbackPulse;
    midibuffer_test::resetTrace();
    setEncoderButton(host, kNT_encoderButtonL, false);
    setEncoderButton(host, kNT_encoderButtonL, true);
    expect(!snapshot(host).playbackActive &&
               !snapshot(host).playbackArmed &&
               host.parameter(kPlaybackParameter) == 0 &&
               snapshot(host).playbackPulse == savedPulse &&
               !snapshot(host).captureEnabled &&
               midibuffer_test::trace().midiCallCount == 2U,
           "one running-stop edge performs normal cleanup while the held callback neither toggles nor repeats cleanup");
    releaseEncoderButton(host, kNT_encoderButtonL);

    midibuffer_test::resetTrace();
    clockPulse(host);
    expect(midibuffer_test::trace().midiCallCount == 0U &&
               !snapshot(host).playbackActive,
           "left-encoder stop stays silent until another explicit rising edge");
}

void verifySharedPlaybackParameter() {
    midibuffer_test::HostDouble silentEmergency;
    expect(silentEmergency.instantiate(1),
           "unavailable emergency host constructs");
    changeParameter(silentEmergency, kPlaybackParameter, 1);
    midibuffer_test::resetTrace();
    sendMidi(silentEmergency, 0xB5U, 120U, 99U);
    expect(silentEmergency.parameter(kPlaybackParameter) == 0 &&
               !snapshot(silentEmergency).playbackArmed &&
               midibuffer_test::trace().midiCallCount == 0U &&
               midibuffer_test::trace().parameterSetCallCount == 1U &&
               midibuffer_test::trace().parameterSetCalls[0].source ==
                   midibuffer_test::kParameterSetFromAudio,
           "incoming emergency control clears unavailable Playback On without producing output");

    midibuffer_test::HostDouble unavailable;
    expect(unavailable.instantiate(1),
           "continuing-request host constructs");
    startCapture(unavailable);
    midibuffer_test::resetTrace();
    changeParameter(unavailable, kPlaybackParameter, 1);
    const midibuffer::CaptureSnapshot unavailableOn = snapshot(unavailable);
    changeParameter(unavailable, kPlaybackParameter, 1);
    expect(unavailable.parameter(kPlaybackParameter) == 1 &&
               !unavailableOn.playbackArmed &&
               !unavailableOn.playbackActive &&
               unavailableOn.captureEnabled &&
               snapshot(unavailable).captureEnabled &&
               midibuffer_test::trace().midiCallCount == 0U,
           "remote On writes are idempotent and remain On without a valid selection or output");

    expect(midibuffer::setRetainedTimelineFixture(
               unavailable.algorithm(), 10U, 12U) &&
               midibuffer::setPulseSelection(unavailable.algorithm(), 10U,
                                             12U),
           "a valid range can arrive while Playback remains On");
    expect(unavailable.parameter(kPlaybackParameter) == 1 &&
               snapshot(unavailable).playbackArmed &&
               !snapshot(unavailable).captureEnabled,
           "the inherited continuing request arms automatically and enforces capture exclusion");

    midibuffer_test::resetTrace();
    clockPulse(unavailable);
    expect(!snapshot(unavailable).clockRunning &&
               snapshot(unavailable).playbackArmed &&
               midibuffer_test::trace().midiCallCount == 0U,
           "the first clock edge only begins acquisition for inherited Playback On");
    clockPulse(unavailable);
    expect(snapshot(unavailable).clockRunning &&
               snapshot(unavailable).playbackActive &&
               unavailable.parameter(kPlaybackParameter) == 1 &&
               midibuffer_test::trace().midiCallCount == 1U &&
               midibuffer_test::trace().midiCalls[0].bytes[0] == 0x90U,
           "the second clock edge starts the later-selected loop without another On write");

    const midibuffer::CaptureSnapshot beforeRepeatedOn = snapshot(unavailable);
    midibuffer_test::resetTrace();
    changeParameter(unavailable, kPlaybackParameter, 1);
    const midibuffer::CaptureSnapshot afterRepeatedOn = snapshot(unavailable);
    expect(afterRepeatedOn.playbackActive &&
               afterRepeatedOn.playbackPulse ==
                   beforeRepeatedOn.playbackPulse &&
               afterRepeatedOn.playbackIntervalOrdinal ==
                   beforeRepeatedOn.playbackIntervalOrdinal &&
               midibuffer_test::trace().midiCallCount == 0U,
           "repeated remote On neither toggles nor duplicates an active start");

    midibuffer_test::resetTrace();
    changeParameter(unavailable, kPlaybackParameter, 0);
    expect(unavailable.parameter(kPlaybackParameter) == 0 &&
               !snapshot(unavailable).playbackArmed &&
               !snapshot(unavailable).playbackActive &&
               midibuffer_test::trace().midiCallCount == 1U &&
               (midibuffer_test::trace().midiCalls[0].bytes[0] & 0xf0U) ==
                   0x80U,
           "remote Off stops rather than toggles and releases playback-owned output");
    midibuffer_test::resetTrace();
    changeParameter(unavailable, kPlaybackParameter, 0);
    clockPulse(unavailable);
    expect(midibuffer_test::trace().midiCallCount == 0U &&
               !snapshot(unavailable).playbackActive,
           "repeated Off is idempotent and later clocks do not restart playback");

    midibuffer_test::HostDouble encoder;
    prepareTransportHistory(encoder);
    midibuffer_test::resetTrace();
    const uint64_t allocationsBeforeEncoder =
        midibuffer_test::heapAllocationCount();
    setEncoderButton(encoder, kNT_encoderButtonL, false);
    setEncoderButton(encoder, kNT_encoderButtonL, true);
    releaseEncoderButton(encoder, kNT_encoderButtonL);
    setEncoderButton(encoder, kNT_encoderButtonL, false);
    setEncoderButton(encoder, kNT_encoderButtonL, true);
    const midibuffer_test::Trace& setterTrace = midibuffer_test::trace();
    expect(encoder.parameter(kPlaybackParameter) == 0 &&
               !snapshot(encoder).playbackArmed &&
               setterTrace.parameterSetCallCount == 2U &&
               setterTrace.parameterSetCalls[0].source ==
                   midibuffer_test::kParameterSetFromUi &&
               setterTrace.parameterSetCalls[0].algorithmIndex ==
                   static_cast<uint32_t>(NT_algorithmIndex(
                       encoder.algorithm())) &&
               setterTrace.parameterSetCalls[0].parameter ==
                   kPlaybackParameter + NT_parameterOffset() &&
               setterTrace.parameterSetCalls[0].value == 1 &&
               setterTrace.parameterSetCalls[1].source ==
                   midibuffer_test::kParameterSetFromUi &&
               setterTrace.parameterSetCalls[1].parameter ==
                   kPlaybackParameter + NT_parameterOffset() &&
               setterTrace.parameterSetCalls[1].value == 0 &&
               setterTrace.parameterSetCalls[0].callbackDepth == 0U &&
               setterTrace.parameterSetCalls[1].callbackDepth == 0U &&
               setterTrace.maximumParameterCallbackDepth == 1U &&
               midibuffer_test::heapAllocationCount() ==
                   allocationsBeforeEncoder,
           "successive left presses toggle one host value through offset API v13 UI setters without recursion or allocation");
}

void verifyOneShotClearRecording() {
    midibuffer_test::HostDouble enabled;
    expect(enabled.instantiate(1),
           "enabled-clear host constructs at minimum capacity");
    changeParameter(enabled, kRecordingChannelParameter, 3);
    startCapture(enabled);
    acquireClock(enabled);
    const uint64_t erasedPulse = snapshot(enabled).currentPulse;
    sendMidi(enabled, 0x92U, 60U, 100U);
    expect(midibuffer::setPulseSelection(enabled.algorithm(), erasedPulse,
                                         erasedPulse + 1U),
           "small clear fixture has selected recorded history");

    midibuffer_test::resetTrace();
    const uint64_t allocationsBeforeSmall =
        midibuffer_test::heapAllocationCount();
    changeParameter(enabled, kClearRecordingParameter, 1);
    const midibuffer::CaptureSnapshot cleared = snapshot(enabled);
    midibuffer::RecordedEvent erased = {};
    expect(enabled.parameter(kClearRecordingParameter) == 1 &&
               enabled.parameter(kCaptureParameter) == 1 &&
               cleared.clearRecordingTransitions == 1U &&
               cleared.clearHistoryMetadataOperations == 4U &&
               !cleared.clearRecordingArmed && cleared.eventCount == 0U &&
               !midibuffer::recordedEventAt(enabled.algorithm(), 0U, erased) &&
               cleared.captureEnabled && cleared.clockRunning &&
               !cleared.selectionValid && !cleared.activeSelectionValid &&
               !cleared.rangeTransitionPending &&
               midibuffer_test::trace().midiCallCount == 0U &&
               midibuffer_test::heapAllocationCount() == allocationsBeforeSmall,
           "one armed On write invalidates small history in four metadata operations, stays consumed On, and preserves enabled Capture and clock state without allocation");

    sendMidi(enabled, 0x91U, 61U, 100U);
    expect(snapshot(enabled).eventCount == 0U,
           "clear preserves the selected recording-channel eligibility gate");
    sendMidi(enabled, 0x92U, 62U, 101U);
    midibuffer::RecordedEvent fresh = {};
    expect(snapshot(enabled).eventCount == 1U &&
               midibuffer::recordedEventAt(enabled.algorithm(), 0U, fresh) &&
               eventBytesMatch(fresh, 0x92U, 62U, 101U),
           "enabled Capture immediately retains fresh eligible MIDI after clear");

    moveUi(enabled, kNT_encoderR, 0.0f, 0.0f, 0.0f, 0, 1);
    noClockBlock(enabled);
    changeParameter(enabled, kClearRecordingParameter, 1);
    midibuffer::RecordedEvent retainedFresh = {};
    const midibuffer::CaptureSnapshot repeatedOn = snapshot(enabled);
    expect(repeatedOn.clearRecordingTransitions == 1U &&
               repeatedOn.eventCount == 1U && repeatedOn.captureEnabled &&
               midibuffer::recordedEventAt(enabled.algorithm(), 0U,
                                           retainedFresh) &&
               eventBytesMatch(retainedFresh, 0x92U, 62U, 101U),
           "production UI, step, and repeated On callbacks leave post-clear recording intact while Clear Recording remains consumed");

    sendMidi(enabled, 0x82U, 62U, 0U);
    stopCapture(enabled);
    expect(midibuffer::setPulseSelection(enabled.algorithm(), fresh.pulse,
                                         fresh.pulse + 1U),
           "post-clear events form a new valid selection");
    midibuffer_test::resetTrace();
    clockPulse(enabled);
    expect(midibuffer_test::trace().midiCallCount == 0U &&
               enabled.parameter(kPlaybackParameter) == 0 &&
               !snapshot(enabled).playbackActive,
           "a new selection and later clock do not implicitly restart playback after clear");
    changeParameter(enabled, kPlaybackParameter, 1);
    clockPulse(enabled);
    stepThrough(enabled, enabled.elapsedSamples() + 4U);
    bool onlyFreshPlayback =
        midibuffer_test::trace().midiCallCount != 0U;
    for (size_t index = 0U;
         index < midibuffer_test::trace().midiCallCount; ++index) {
        const midibuffer_test::MidiCall& call =
            midibuffer_test::trace().midiCalls[index];
        const uint8_t type = call.bytes[0] & 0xf0U;
        onlyFreshPlayback =
            onlyFreshPlayback &&
            ((type != 0x80U && type != 0x90U) || call.bytes[1] == 62U);
    }
    expect(snapshot(enabled).playbackActive && onlyFreshPlayback,
           "an explicit Playback On action replays only newly selected post-clear events");

    changeParameter(enabled, kClearRecordingParameter, 0);
    expect(snapshot(enabled).clearRecordingArmed &&
               snapshot(enabled).eventCount == 2U &&
               enabled.parameter(kClearRecordingParameter) == 0,
           "Off rearms Clear Recording without erasing retained fresh events");
    changeParameter(enabled, kClearRecordingParameter, 1);
    expect(snapshot(enabled).clearRecordingTransitions == 2U &&
               snapshot(enabled).eventCount == 0U &&
               !snapshot(enabled).clearRecordingArmed &&
               !snapshot(enabled).captureEnabled &&
               enabled.parameter(kPlaybackParameter) == 0,
           "the next armed Off-to-On transition performs exactly one further erase while preserving stopped Capture");

    midibuffer_test::HostDouble disabled;
    expect(disabled.instantiate(1),
           "disabled-clear host constructs");
    startCapture(disabled);
    acquireClock(disabled);
    sendMidi(disabled, 0x90U, 48U, 100U);
    stopCapture(disabled);
    changeParameter(disabled, kClearRecordingParameter, 1);
    expect(!snapshot(disabled).captureEnabled &&
               disabled.parameter(kCaptureParameter) == 0 &&
               snapshot(disabled).eventCount == 0U,
           "clear preserves disabled Capture and does not implicitly restart it");

    midibuffer_test::HostDouble playing;
    TransportFixture fixture = prepareTransportHistory(playing);
    changeParameter(playing, kPlaybackDestinationParameter, 1);
    changeParameter(playing, kPlaybackChannelParameter, 5);
    beginTransportOnHeldFirstBeat(playing, fixture, 16U, 4U);
    expect(midibuffer::setPulseSelection(
               playing.algorithm(), fixture.firstSelectedPulse,
               fixture.firstSelectedPulse + 1U) &&
               snapshot(playing).rangeTransitionPending,
           "playing clear fixture includes active and pending ranges");

    midibuffer_test::resetTrace();
    const uint64_t allocationsBeforePlaying =
        midibuffer_test::heapAllocationCount();
    changeParameter(playing, kClearRecordingParameter, 1);
    const midibuffer::CaptureSnapshot stopped = snapshot(playing);
    const midibuffer_test::Trace& cleanup = midibuffer_test::trace();
    expect(playing.parameter(kClearRecordingParameter) == 1 &&
               playing.parameter(kPlaybackParameter) == 0 &&
               cleanup.parameterSetCallCount == 1U &&
               cleanup.parameterSetCalls[0].source ==
                   midibuffer_test::kParameterSetFromAudio &&
               cleanup.parameterSetCalls[0].parameter ==
                   kPlaybackParameter + NT_parameterOffset() &&
               cleanup.parameterSetCalls[0].value == 0 &&
               cleanup.maximumParameterCallbackDepth == 1U &&
               cleanup.midiCallCount == 2U &&
               cleanup.midiCalls[0].destination == kNT_destinationUSB &&
               cleanup.midiCalls[0].bytes[0] == 0x84U &&
               cleanup.midiCalls[0].bytes[1] == 60U &&
               cleanup.midiCalls[1].destination == kNT_destinationUSB &&
               cleanup.midiCalls[1].bytes[0] == 0xB4U &&
               cleanup.midiCalls[1].bytes[1] == 64U &&
               cleanup.midiCalls[1].bytes[2] == 0U,
           "clear publishes Playback Off and releases held note then sustain through the selected destination");
    expect(stopped.eventCount == 0U && !stopped.selectionValid &&
               !stopped.activeSelectionValid &&
               !stopped.rangeTransitionPending &&
               !stopped.playbackArmed && !stopped.playbackActive &&
               !stopped.playbackClockLossPaused &&
               !stopped.playbackIntervalOpen &&
               !stopped.playbackNextEventScheduled &&
               !stopped.pendingNextEndingScheduled &&
               stopped.playbackPulse == 0U &&
               stopped.playbackIntervalStartSample == 0U &&
               stopped.playbackIntervalOrdinal == 0U &&
               stopped.playbackNextEventSample == 0U &&
               stopped.playbackEventIndex == 0U &&
               stopped.pendingNextEndingSample == 0U &&
               stopped.pendingNoteEndingCount == 0U &&
               stopped.pendingSustainReleaseCount == 0U &&
               !stopped.captureEnabled &&
               midibuffer_test::heapAllocationCount() ==
                   allocationsBeforePlaying,
           "clear invalidates selected, active, pending, cursor, queued-event, and ending state without allocation or Capture restart");

    midibuffer_test::resetTrace();
    clockPulse(playing);
    sendMidi(playing, 0x94U, 99U, 100U);
    moveUi(playing, 0U, 0.0f, 0.0f, 0.0f);
    expect(midibuffer_test::trace().midiCallCount == 0U &&
               snapshot(playing).eventCount == 0U &&
               !snapshot(playing).playbackActive &&
               playing.parameter(kPlaybackParameter) == 0,
           "post-clear production step, MIDI, and UI callbacks emit no erased events and do not restart playback");

    midibuffer_test::HostDouble maximum;
    expect(maximum.instantiate(5),
           "full-capacity clear host constructs");
    startCapture(maximum);
    acquireClock(maximum);
    const uint32_t capacity = snapshot(maximum).eventCapacity;
    for (uint32_t index = 0; index < capacity; ++index) {
        sendMidi(maximum, 0xB0U, 7U,
                 static_cast<uint8_t>(index & 0x7fU));
    }
    expect(snapshot(maximum).eventCount == capacity,
           "clear cost fixture fills the complete 5 MB event capacity");
    midibuffer_test::resetTrace();
    const uint64_t allocationsBeforeMaximum =
        midibuffer_test::heapAllocationCount();
    changeParameter(maximum, kClearRecordingParameter, 1);
    midibuffer::RecordedEvent removedMaximum = {};
    expect(snapshot(maximum).eventCount == 0U &&
               snapshot(maximum).clearRecordingTransitions == 1U &&
               snapshot(maximum).clearHistoryMetadataOperations ==
                   cleared.clearHistoryMetadataOperations &&
               !midibuffer::recordedEventAt(maximum.algorithm(), 0U,
                                            removedMaximum) &&
               snapshot(maximum).captureEnabled &&
               snapshot(maximum).clockRunning &&
               midibuffer_test::heapAllocationCount() ==
                   allocationsBeforeMaximum,
           "full 5 MB clear performs the same four metadata operations as small history, exposes no erased event, allocates nothing, and preserves live capture eligibility");
}

void verifySharedControlPresetRestoration() {
    midibuffer_test::HostDouble source;
    expect(source.instantiate(1),
           "shared-control preset source constructs");

    // Consume Clear while empty, then retain a complete playable history. This
    // is the valid state that distinguishes restoration from a live clear
    // gesture: Clear is On, but the later recording must survive.
    changeParameter(source, kClearRecordingParameter, 1);
    startCapture(source);
    expect(clockPulseAt(source, 0U) && clockPulseAt(source, 16U),
           "consumed-clear preset source acquires its recording clock");
    const uint64_t selectionStart = snapshot(source).currentPulse;
    sendMidi(source, 0x92U, 60U, 100U);
    sendMidi(source, 0xB2U, 64U, 127U);
    expect(clockPulseAt(source, 32U),
           "consumed-clear preset source reaches its ending pulse");
    sendMidi(source, 0x82U, 60U, 0U);
    sendMidi(source, 0xB2U, 64U, 0U);
    expect(clockPulseAt(source, 48U),
           "consumed-clear preset source closes retained history");
    stopCapture(source);
    expect(midibuffer::setPulseSelection(source.algorithm(), selectionStart,
                                         selectionStart + 2U),
           "consumed-clear preset source selects retained history");
    changeParameter(source, kPlaybackDestinationParameter, 1);
    changeParameter(source, kPlaybackChannelParameter, 5);
    changeParameter(source, kPlaybackParameter, 1);
    expect(clockPulseAt(source, 64U),
           "consumed-clear preset source starts shared Playback");
    stepThrough(source, 72U);

    const midibuffer::CaptureSnapshot saved = snapshot(source);
    const HistoryImage savedHistory = captureHistory(source);
    expect(saved.playbackActive && !saved.captureEnabled &&
               !saved.clearRecordingArmed && saved.eventCount == 4U,
           "new preset fixture combines active Playback, consumed Clear, and retained history");

    midibuffer_test::PresetImage image;
    int16_t savedPlayback = -1;
    int16_t savedClear = -1;
    expect(source.savePreset(image) && image.parameterCount() == 12U &&
               image.parameter(kPlaybackParameter, savedPlayback) &&
               image.parameter(kClearRecordingParameter, savedClear) &&
               savedPlayback == 1 && savedClear == 1,
           "normal host parameter persistence saves Playback and Clear Recording On outside custom state");

    for (int order = 0; order < 2; ++order) {
        midibuffer_test::HostDouble restored;
        midibuffer_test::resetTrace();
        expect(restored.instantiate(1) &&
                   restored.loadPreset(image, order != 0),
               order == 0
                   ? "new shared controls load with generic parameters before custom state"
                   : "new shared controls load with generic parameters after custom state");
        const midibuffer::CaptureSnapshot loaded = snapshot(restored);
        expect(restored.parameter(kPlaybackParameter) == 1 &&
                   restored.parameter(kClearRecordingParameter) == 1 &&
                   !loaded.clearRecordingArmed &&
                   loaded.clearRecordingTransitions == 0U &&
                   loaded.clearHistoryMetadataOperations == 0U &&
                   sameSelectionAndTransport(saved, loaded) &&
                   loaded.pendingNoteEndingCount ==
                       saved.pendingNoteEndingCount &&
                   loaded.pendingSustainReleaseCount ==
                       saved.pendingSustainReleaseCount &&
                   loaded.timelineVisiblePulses ==
                       saved.timelineVisiblePulses &&
                   loaded.timelineScrollPulses ==
                       saved.timelineScrollPulses &&
                   sameHistory(savedHistory, restored),
               "both restoration orders reconcile shared values while preserving complete selection, scheduling, ownership, navigation, and event state");
        expect(midibuffer_test::trace().midiCallCount == 0U,
               "shared-control load emits no cleanup or playback MIDI");

        midibuffer_test::PresetImage roundTrip;
        expect(restored.savePreset(roundTrip) && image.equals(roundTrip),
               "new shared-control image round-trips every generic parameter and custom JSON value exactly");

        midibuffer_test::resetTrace();
        changeParameter(restored, kClearRecordingParameter, 1);
        expect(snapshot(restored).eventCount == saved.eventCount &&
                   snapshot(restored).clearRecordingTransitions == 0U &&
                   midibuffer_test::trace().midiCallCount == 0U,
               "restored Clear Recording On is consumed and repeated On cannot erase or clean up");

        const uint64_t firstFreshPulse = restored.elapsedSamples();
        expect(clockPulseAt(restored, firstFreshPulse) &&
                   !snapshot(restored).clockRunning &&
                   midibuffer_test::trace().midiCallCount == 0U,
               "restored transport stays silent on its first fresh clock pulse");
        expect(clockPulseAt(restored, firstFreshPulse + 16U) &&
                   snapshot(restored).clockRunning,
               "restored transport resumes only after its second fresh clock pulse");

        changeParameter(restored, kClearRecordingParameter, 0);
        expect(snapshot(restored).clearRecordingArmed &&
                   snapshot(restored).eventCount == saved.eventCount,
               "post-load Clear Off rearms without changing retained history");
        changeParameter(restored, kClearRecordingParameter, 1);
        expect(snapshot(restored).eventCount == 0U &&
                   snapshot(restored).clearRecordingTransitions == 1U &&
                   !snapshot(restored).clearRecordingArmed &&
                   restored.parameter(kPlaybackParameter) == 0,
               "only a post-load Off-to-On Clear transition erases and stops Playback");
    }
}

void verifyLegacySharedControlPresetMigration() {
    enum LegacyIntent {
        kLegacyStopped,
        kLegacyArmed,
        kLegacyPlaying,
        kLegacyClockLossPaused,
    };
    const LegacyIntent intents[] = {
        kLegacyStopped,
        kLegacyArmed,
        kLegacyPlaying,
        kLegacyClockLossPaused,
    };

    for (size_t intentIndex = 0; intentIndex < ARRAY_SIZE(intents);
         ++intentIndex) {
        midibuffer_test::HostDouble source;
        TransportFixture fixture = prepareTransportHistory(source);
        if (intents[intentIndex] != kLegacyStopped) {
            changeParameter(source, kPlaybackParameter, 1);
        }
        if (intents[intentIndex] == kLegacyPlaying ||
            intents[intentIndex] == kLegacyClockLossPaused) {
            fixture.lastClockSample += 16U;
            expect(clockPulseAt(source, fixture.lastClockSample),
                   "legacy playing fixture starts on its next clock");
        }
        if (intents[intentIndex] == kLegacyClockLossPaused) {
            while (!snapshot(source).playbackClockLossPaused) {
                noClockBlock(source);
            }
        }
        const midibuffer::CaptureSnapshot saved = snapshot(source);
        const HistoryImage savedHistory = captureHistory(source);
        const bool expectedPlayback = intents[intentIndex] != kLegacyStopped;
        expect((intents[intentIndex] == kLegacyStopped &&
                !saved.playbackArmed && !saved.playbackActive &&
                !saved.playbackClockLossPaused) ||
                   (intents[intentIndex] == kLegacyArmed &&
                    saved.playbackArmed) ||
                   (intents[intentIndex] == kLegacyPlaying &&
                    saved.playbackActive) ||
                   (intents[intentIndex] == kLegacyClockLossPaused &&
                    saved.playbackClockLossPaused),
               "legacy fixture reaches its requested transport intent");

        midibuffer_test::PresetImage legacyImage;
        expect(source.savePreset(legacyImage) &&
                   legacyImage.makeLegacyWithoutAppendedParameters() &&
                   legacyImage.parameterCount() == 10U,
               "valid legacy image omits both appended generic parameters and shared-control reconciliation state");

        for (int order = 0; order < 2; ++order) {
            midibuffer_test::HostDouble restored;
            midibuffer_test::resetTrace();
            expect(restored.instantiate(1) &&
                       restored.loadPreset(legacyImage, order != 0),
                   "legacy image loads under either generic/custom restoration order");
            const midibuffer::CaptureSnapshot loaded = snapshot(restored);
            expect(restored.parameter(kPlaybackParameter) ==
                       (expectedPlayback ? 1 : 0) &&
                       restored.parameter(kClearRecordingParameter) == 0 &&
                       loaded.clearRecordingArmed &&
                       loaded.clearRecordingTransitions == 0U &&
                       sameSelectionAndTransport(saved, loaded) &&
                       loaded.pendingNoteEndingCount ==
                           saved.pendingNoteEndingCount &&
                       loaded.pendingSustainReleaseCount ==
                           saved.pendingSustainReleaseCount &&
                       sameHistory(savedHistory, restored),
                   "legacy Stopped maps Playback Off; Armed, Playing, and ClockLossPaused map On while Clear defaults Off and armed with custom state intact");
            expect(midibuffer_test::trace().midiCallCount == 0U,
                   "legacy migration emits no cleanup or playback MIDI during load");

            if (expectedPlayback) {
                const uint64_t firstFreshPulse = restored.elapsedSamples();
                midibuffer_test::resetTrace();
                expect(clockPulseAt(restored, firstFreshPulse) &&
                           !snapshot(restored).clockRunning &&
                           midibuffer_test::trace().midiCallCount == 0U,
                       "legacy restored transport remains gated on the first fresh pulse");
                expect(clockPulseAt(restored, firstFreshPulse + 16U) &&
                           snapshot(restored).clockRunning,
                       "legacy restored transport requires the second fresh pulse before resuming");
            }
        }
    }
}

void verifyTimedRightEncoderPanic() {
    const uint32_t destinations[] = {
        kNT_destinationBreakout,
        kNT_destinationUSB,
        kNT_destinationSelectBus,
        kNT_destinationInternal,
        kNT_destinationBreakout | kNT_destinationUSB |
            kNT_destinationSelectBus | kNT_destinationInternal,
    };

    bool allDestinationsMatch = true;
    for (int destinationSetting = 0; destinationSetting < 5;
         ++destinationSetting) {
        midibuffer_test::HostDouble host;
        TransportFixture fixture = prepareTransportHistory(host);
        changeParameter(host, kPlaybackDestinationParameter,
                        static_cast<int16_t>(destinationSetting));
        beginTransportOnHeldFirstBeat(host, fixture);

        midibuffer_test::resetTrace();
        setEncoderButton(host, kNT_encoderButtonR, false);
        releaseEncoderButton(host, kNT_encoderButtonR);
        allDestinationsMatch =
            midibuffer_test::trace().midiCallCount == 0U &&
            snapshot(host).playbackActive && allDestinationsMatch;

        moveUi(host, kNT_encoderButtonR | kNT_potButtonR,
               0.0f, 0.0f, 0.50f, 1, 1, 0U);
        const uint64_t deadline =
            host.elapsedSamples() + NT_globals.sampleRate;
        uint32_t block = 0U;
        while (host.elapsedSamples() + 8U < deadline) {
            if ((block++ & 1U) == 0U) {
                clockPulse(host);
            } else {
                noClockBlock(host);
            }
        }
        midibuffer_test::resetTrace();
        moveUi(host,
               kNT_encoderButtonR | kNT_potButtonR | kNT_potR,
               0.0f, 0.0f, 0.25f, -1, 1,
               kNT_encoderButtonR | kNT_potButtonR);
        allDestinationsMatch =
            midibuffer_test::trace().midiCallCount == 0U &&
            snapshot(host).playbackActive && allDestinationsMatch;

        if ((block & 1U) == 0U) {
            clockPulse(host);
        } else {
            noClockBlock(host);
        }
        const uint64_t savedPulse = snapshot(host).playbackPulse;
        midibuffer_test::resetTrace();
        moveUi(host,
               kNT_encoderButtonR | kNT_potButtonR | kNT_potR,
               0.0f, 0.0f, 0.75f, 1, -1,
               kNT_encoderButtonR | kNT_potButtonR);
        bool exactTimestamp = midibuffer_test::trace().midiCallCount == 32U;
        for (size_t index = 0;
             index < midibuffer_test::trace().midiCallCount; ++index) {
            exactTimestamp =
                midibuffer_test::trace().midiCalls[index].dispatchSample ==
                    deadline &&
                exactTimestamp;
        }
        allDestinationsMatch =
            host.elapsedSamples() == deadline && exactTimestamp &&
            panicTraceMatches(destinations[destinationSetting]) &&
            host.parameter(kPlaybackParameter) == 0 &&
            midibuffer_test::trace().parameterSetCallCount == 1U &&
            midibuffer_test::trace().parameterSetCalls[0].source ==
                midibuffer_test::kParameterSetFromUi &&
            midibuffer_test::trace().parameterSetCalls[0].parameter ==
                kPlaybackParameter + NT_parameterOffset() &&
            !snapshot(host).playbackActive &&
            !snapshot(host).captureEnabled &&
            snapshot(host).playbackPulse == savedPulse &&
            allDestinationsMatch;

        moveUi(host,
               kNT_encoderButtonR | kNT_potButtonR | kNT_potR,
               0.0f, 0.0f, 0.60f, -1, 1,
               kNT_encoderButtonR | kNT_potButtonR);
        allDestinationsMatch =
            midibuffer_test::trace().midiCallCount == 32U &&
            allDestinationsMatch;

        if (destinationSetting == 0) {
            const uint64_t stillHeldDeadline =
                host.elapsedSamples() + NT_globals.sampleRate;
            while (host.elapsedSamples() < stillHeldDeadline) {
                noClockBlock(host);
            }
            moveUi(host, kNT_encoderButtonR | kNT_potButtonR,
                   0.0f, 0.0f, 0.60f, 0, 0,
                   kNT_encoderButtonR | kNT_potButtonR);
            allDestinationsMatch =
                midibuffer_test::trace().midiCallCount == 32U &&
                allDestinationsMatch;
        }

        moveUi(host, 0U, 0.0f, 0.0f, 0.60f, 0, 0,
               kNT_encoderButtonR | kNT_potButtonR);
        midibuffer_test::resetTrace();
        if (destinationSetting == 0) {
            setEncoderButton(host, kNT_encoderButtonR, false);
            const uint64_t rearmedDeadline =
                host.elapsedSamples() + NT_globals.sampleRate;
            while (host.elapsedSamples() < rearmedDeadline) {
                noClockBlock(host);
            }
            setEncoderButton(host, kNT_encoderButtonR, true);
            bool rearmedTimestamp =
                midibuffer_test::trace().midiCallCount == 32U;
            for (size_t index = 0;
                 index < midibuffer_test::trace().midiCallCount; ++index) {
                rearmedTimestamp =
                    midibuffer_test::trace().midiCalls[index].dispatchSample ==
                        rearmedDeadline &&
                    rearmedTimestamp;
            }
            allDestinationsMatch =
                rearmedTimestamp &&
                panicTraceMatches(destinations[destinationSetting]) &&
                allDestinationsMatch;
            releaseEncoderButton(host, kNT_encoderButtonR);
            midibuffer_test::resetTrace();
        }
        clockPulse(host);
        allDestinationsMatch =
            midibuffer_test::trace().midiCallCount == 0U &&
            !snapshot(host).playbackActive && allDestinationsMatch;
    }
    expect(allDestinationsMatch,
           "right-encoder short press is inert; held panic stays silent before sampleRate, fires once at the exact sample during rotations and pot-3 hold, and rearms only after release");
}

bool sameMidiTrace(const midibuffer_test::Trace& left,
                   const midibuffer_test::Trace& right) {
    if (left.midiCallCount != right.midiCallCount) {
        return false;
    }
    for (size_t index = 0; index < left.midiCallCount; ++index) {
        if (left.midiCalls[index].destination !=
                right.midiCalls[index].destination ||
            left.midiCalls[index].size != right.midiCalls[index].size ||
            std::memcmp(left.midiCalls[index].bytes,
                        right.midiCalls[index].bytes,
                        sizeof(left.midiCalls[index].bytes)) != 0) {
            return false;
        }
    }
    return true;
}

bool sameExactMidiTrace(const midibuffer_test::Trace& left,
                        const midibuffer_test::Trace& right) {
    if (!sameMidiTrace(left, right)) {
        return false;
    }
    for (size_t index = 0; index < left.midiCallCount; ++index) {
        if (left.midiCalls[index].dispatchSample !=
            right.midiCalls[index].dispatchSample) {
            return false;
        }
    }
    return true;
}

bool sameNormalizedMidiTrace(const midibuffer_test::Trace& left,
                             uint64_t leftIntervalOrigin,
                             const midibuffer_test::Trace& right,
                             uint64_t rightIntervalOrigin) {
    if (!sameMidiTrace(left, right)) {
        return false;
    }
    for (size_t index = 0; index < left.midiCallCount; ++index) {
        if (left.midiCalls[index].dispatchSample < leftIntervalOrigin ||
            right.midiCalls[index].dispatchSample < rightIntervalOrigin ||
            left.midiCalls[index].dispatchSample - leftIntervalOrigin !=
                right.midiCalls[index].dispatchSample - rightIntervalOrigin) {
            return false;
        }
    }
    return true;
}

void verifyLegacyNavigationPresetCompatibility() {
    const uint64_t historyStart = 100U;
    const uint64_t historyEnd = 612U;
    const uint64_t widths[] = {64U, 256U};
    const uint64_t scrolls[] = {11U, 17U};
    const midibuffer::SelectionFineTarget targets[] = {
        midibuffer::kSelectionFineTargetStart,
        midibuffer::kSelectionFineTargetEnd,
    };

    for (size_t fixture = 0; fixture < ARRAY_SIZE(widths); ++fixture) {
        midibuffer_test::HostDouble source;
        expect(installRangeFixture(source, historyStart, historyEnd, 180U,
                                   260U) &&
                   midibuffer::setTimelineNavigationFixture(
                       source.algorithm(), false, widths[fixture],
                       scrolls[fixture], targets[fixture]),
               "legacy manual-view source fixture constructs");
        const midibuffer::CaptureSnapshot saved = snapshot(source);
        const HistoryImage savedHistory = captureHistory(source);
        midibuffer_test::PresetImage legacyImage;
        bool encodedShowAll = true;
        uint64_t encodedSpan = 0U;
        int encodedTarget = -1;
        expect(source.savePreset(legacyImage) &&
                   legacyImage.navigation(encodedShowAll, encodedSpan,
                                          encodedTarget) &&
                   !encodedShowAll && encodedSpan == widths[fixture] &&
                   encodedTarget == static_cast<int>(targets[fixture]) &&
                   legacyImage.removeNavigation(),
               "complete v1 fixture is reduced to the actual legacy representation");

        midibuffer_test::HostDouble restored;
        expect(restored.instantiate(1) && restored.loadPreset(legacyImage),
               "legacy complete v1 state loads through the production parser");
        const midibuffer::CaptureSnapshot loaded = snapshot(restored);
        const uint64_t expectedViewEnd = historyEnd - scrolls[fixture];
        expect(!loaded.timelineShowAll &&
                   loaded.timelineVisiblePulses == widths[fixture] &&
                   loaded.timelineScrollPulses == scrolls[fixture] &&
                   loaded.timelineViewStartPulse ==
                       expectedViewEnd - widths[fixture] &&
                   loaded.timelineViewEndPulse == expectedViewEnd &&
                   loaded.selectionFineTarget == targets[fixture] &&
                   sameSelectionAndTransport(saved, loaded) &&
                   sameHistory(savedHistory, restored),
               "legacy 64/256 widths, end-relative scroll, Start/End target, selection, transport, and event bytes restore as manual state");

        _NT_float3 pots = {-1.0f, -1.0f, -1.0f};
        restored.factory()->setupUi(restored.algorithm(), pots);
        moveUi(restored, 0U, pots[0], pots[1], pots[2]);
        const midibuffer::CaptureSnapshot afterUi = snapshot(restored);
        expect(!afterUi.timelineShowAll &&
                   afterUi.timelineVisiblePulses == widths[fixture] &&
                   afterUi.timelineScrollPulses == scrolls[fixture] &&
                   afterUi.selection.startPulse == loaded.selection.startPulse &&
                   afterUi.selection.endPulse == loaded.selection.endPulse &&
                   afterUi.selectionFineTarget == targets[fixture],
               "setupUi and first customUi seed physical state without replacing a restored legacy view or selection");
    }
}

void verifyShowAllPresetPolicy() {
    const uint64_t backingSpan = 0x100000000ULL + 4096U;
    midibuffer_test::HostDouble source;
    expect(installRangeFixture(source, 100U, 612U, 180U, 260U) &&
               midibuffer::setTimelineNavigationFixture(
                   source.algorithm(), true, backingSpan, 23U,
                   midibuffer::kSelectionFineTargetRange),
           "Show All compatibility source fixture constructs");
    const midibuffer::CaptureSnapshot saved = snapshot(source);
    const HistoryImage savedHistory = captureHistory(source);
    midibuffer_test::PresetImage image;
    bool encodedShowAll = false;
    uint64_t encodedSpan = 0U;
    int encodedTarget = -1;
    expect(source.savePreset(image) &&
               image.navigation(encodedShowAll, encodedSpan, encodedTarget) &&
               encodedShowAll && encodedSpan == backingSpan &&
               encodedTarget ==
                   static_cast<int>(midibuffer::kSelectionFineTargetRange),
           "v1 navigation extension explicitly carries Show All, uint64 backing span, and Range target");

    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(1) && restored.loadPreset(image),
           "new Show All state round-trips through native callbacks");
    const midibuffer::CaptureSnapshot loaded = snapshot(restored);
    expect(loaded.timelineShowAll &&
               loaded.timelineVisiblePulses == backingSpan &&
               loaded.timelineScrollPulses == saved.timelineScrollPulses &&
               loaded.timelineViewStartPulse == 100U &&
               loaded.timelineViewEndPulse == 612U &&
               loaded.selectionFineTarget ==
                   midibuffer::kSelectionFineTargetRange &&
               sameSelectionAndTransport(saved, loaded) &&
               sameHistory(savedHistory, restored),
           "Show All restores automatic fit without narrowing its manual backing or changing complete state");

    expect(midibuffer::setRetainedTimelineFixture(restored.algorithm(), 90U,
                                                   900U),
           "restored Show All history-growth fixture installs");
    const midibuffer::CaptureSnapshot grown = snapshot(restored);
    expect(grown.timelineShowAll && grown.timelineViewStartPulse == 90U &&
               grown.timelineViewEndPulse == 900U &&
               grown.timelineVisiblePulses == backingSpan,
           "restored Show All continues fitting a changed retained envelope automatically");

    midibuffer_test::HostDouble corruptTarget;
    expect(image.corruptNavigationTarget() && corruptTarget.instantiate(1) &&
               !corruptTarget.loadPreset(image),
           "an invalid appended navigation target rejects the complete preset under the existing corruption policy");
}

void verifyCompletePresetRoundTripsAndContinuation() {
    midibuffer_test::HostDouble source;
    TransportFixture fixture = prepareTransportHistory(source);
    changeParameter(source, kPlaybackDestinationParameter, 1);
    changeParameter(source, kPlaybackChannelParameter, 5);
    changeParameter(source, kFilterPitchBendParameter, 1);
    changeParameter(source, kPulsesPerDisplayedBeatParameter, 5);
    beginTransportOnHeldFirstBeat(source, fixture, 16U, 4U);
    beginRightPotZoom(source, 0.5f);
    continueRightPotZoom(source, 0.0f);
    endRightPotZoom(source, 0.0f);
    moveUi(source, kNT_potL, 0.75f, 0.0f, 0.0f);
    beginRightPotZoom(source, 0.6f);
    moveUi(source, kNT_potButtonR | kNT_encoderButtonR,
           0.0f, 0.0f, 0.6f, 0, 0, kNT_potButtonR);
    const uint64_t longManualSpan = 0x100000000ULL + 12345U;
    expect(midibuffer::setTimelineNavigationFixture(
               source.algorithm(), false, longManualSpan, 0U,
               midibuffer::kSelectionFineTargetRange),
           "active preset fixture installs an exact long manual view and Range target");
    const midibuffer::CaptureSnapshot saved = snapshot(source);
    const HistoryImage savedHistory = captureHistory(source);
    expect(saved.playbackActive && saved.rangeTransitionPending &&
               saved.pendingNoteEndingCount == 1U &&
               saved.pendingSustainReleaseCount == 1U &&
               !saved.timelineShowAll &&
               saved.timelineVisiblePulses == longManualSpan &&
               saved.selectionFineTarget ==
                   midibuffer::kSelectionFineTargetRange,
           "active preset fixture contains pending range, long manual navigation, note, sustain, and scheduler state");

    midibuffer_test::PresetImage image;
    bool encodedShowAll = true;
    uint64_t encodedSpan = 0U;
    int encodedTarget = -1;
    expect(source.savePreset(image) && image.payloadBytes() != 0U &&
               image.valueCount() != 0U &&
               image.navigation(encodedShowAll, encodedSpan, encodedTarget) &&
               !encodedShowAll && encodedSpan == longManualSpan &&
               encodedTarget ==
                   static_cast<int>(midibuffer::kSelectionFineTargetRange),
           "actual NT serialise callback writes the reviewed navigation extension without uint32/256 truncation");
    const midibuffer::CaptureSnapshot afterPlaybackSave = snapshot(source);
    expect(afterPlaybackSave.eventCount == saved.eventCount &&
               afterPlaybackSave.playbackPulse == saved.playbackPulse &&
               afterPlaybackSave.pendingNoteEndingCount ==
                   saved.pendingNoteEndingCount &&
               afterPlaybackSave.pendingSustainReleaseCount ==
                   saved.pendingSustainReleaseCount &&
               afterPlaybackSave.playbackActive,
           "save-time snapshot is coherent and non-mutating during playback");

    midibuffer_test::resetTrace();
    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(1) && restored.loadPreset(image),
           "actual NT deserialise callback reconstructs a fresh instance");
    const midibuffer::CaptureSnapshot loaded = snapshot(restored);
    bool allParametersMatch = true;
    for (size_t index = 0; index <= kPulsesPerDisplayedBeatParameter;
         ++index) {
        allParametersMatch = allParametersMatch &&
                             source.parameter(index) ==
                                 restored.parameter(index);
    }
    expect(allParametersMatch && loaded.eventCount == saved.eventCount &&
               loaded.currentPulse == saved.currentPulse &&
               loaded.playbackPulse == saved.playbackPulse &&
               loaded.selectionValid == saved.selectionValid &&
               loaded.selection.startPulse == saved.selection.startPulse &&
               loaded.selection.endPulse == saved.selection.endPulse &&
               loaded.activeSelectionValid == saved.activeSelectionValid &&
               loaded.activeSelection.startPulse ==
                   saved.activeSelection.startPulse &&
               loaded.activeSelection.endPulse ==
                   saved.activeSelection.endPulse &&
               loaded.rangeTransitionPending == saved.rangeTransitionPending &&
               loaded.pendingNoteEndingCount ==
                   saved.pendingNoteEndingCount &&
               loaded.pendingSustainReleaseCount ==
                   saved.pendingSustainReleaseCount &&
               loaded.timelineVisiblePulses == saved.timelineVisiblePulses &&
               loaded.timelineScrollPulses == saved.timelineScrollPulses &&
               loaded.timelineShowAll == saved.timelineShowAll &&
               loaded.timelineViewStartPulse == saved.timelineViewStartPulse &&
               loaded.timelineViewEndPulse == saved.timelineViewEndPulse &&
               loaded.selectionFineTarget == saved.selectionFineTarget &&
               loaded.playbackActive && !loaded.captureEnabled &&
               !loaded.clockRunning && sameHistory(savedHistory, restored),
           "fresh reconstruction restores events/timing, ranges, cursor, transport intent, parameters, routing, filters, exact manual navigation, and pending ownership while gating on live clock");
    expect(midibuffer_test::trace().midiCallCount == 0U,
           "preset load does not pretend to restore or transmit external instrument state");

    midibuffer_test::PresetImage reconstructedImage;
    expect(restored.savePreset(reconstructedImage) &&
               image.equals(reconstructedImage),
           "exhaustive canonical saved-state equality has no silently omitted category");

    _NT_float3 restoredPots = {-1.0f, -1.0f, -1.0f};
    restored.factory()->setupUi(restored.algorithm(), restoredPots);
    while (restored.elapsedSamples() <= NT_globals.sampleRate) {
        noClockBlock(restored);
    }
    midibuffer_test::resetTrace();
    moveUi(restored, kNT_potButtonR | kNT_potR | kNT_encoderButtonR,
           restoredPots[0], restoredPots[1], 0.2f, 0, 0,
           kNT_potButtonR | kNT_encoderButtonR);
    const midibuffer::CaptureSnapshot afterUi = snapshot(restored);
    expect(afterUi.timelineVisiblePulses == loaded.timelineVisiblePulses &&
               afterUi.timelineScrollPulses == loaded.timelineScrollPulses &&
               afterUi.timelineShowAll == loaded.timelineShowAll &&
               afterUi.selectionFineTarget == loaded.selectionFineTarget,
           "load followed by setupUi/customUi preserves exact navigation fields");
    expect(afterUi.selection.startPulse == loaded.selection.startPulse &&
               afterUi.selection.endPulse == loaded.selection.endPulse &&
               sameSelectionAndTransport(loaded, afterUi),
           "load followed by setupUi/customUi does not move selection or scheduler state");
    expect(sameHistory(savedHistory, restored) &&
               midibuffer_test::trace().midiCallCount == 0U,
           "load followed by setupUi/customUi neither changes event bytes nor replays physical panic/hold state");

    midibuffer_test::HostDouble parametersAfter;
    expect(parametersAfter.instantiate(1) &&
               parametersAfter.loadPreset(image, true),
           "valid custom state loads when generic parameters are restored after deserialise");
    bool afterOrderMatches = true;
    for (size_t index = 0; index <= kPulsesPerDisplayedBeatParameter;
         ++index) {
        afterOrderMatches = afterOrderMatches &&
                            source.parameter(index) ==
                                parametersAfter.parameter(index);
    }
    midibuffer_test::PresetImage afterOrderImage;
    expect(afterOrderMatches && parametersAfter.savePreset(afterOrderImage) &&
               image.equals(afterOrderImage),
           "parameter-before and parameter-after callback orders reconstruct the same saved state");

    midibuffer_test::resetTrace();
    fixture.lastClockSample += 16U;
    expect(clockPulseAt(source, fixture.lastClockSample),
           "original active state receives its next continuation clock");
    stepThrough(source, fixture.lastClockSample + 8U);
    const midibuffer_test::Trace originalContinuation =
        midibuffer_test::trace();

    midibuffer_test::resetTrace();
    const uint64_t restoredAcquisitionStart = restored.elapsedSamples();
    expect(clockPulseAt(restored, restoredAcquisitionStart) &&
               midibuffer_test::trace().midiCallCount == 0U &&
               !snapshot(restored).clockRunning,
           "restored playback emits nothing on the first external acquisition pulse");
    expect(clockPulseAt(restored, restoredAcquisitionStart + 16U),
           "restored playback accepts the second external acquisition pulse");
    stepThrough(restored, restoredAcquisitionStart + 24U);
    expect(midibuffer_test::trace().midiCallCount == 0U,
           "reacquisition reopens the saved interval without advancing past it");
    expect(clockPulseAt(restored, restoredAcquisitionStart + 32U),
           "restored playback receives the saved interval's continuation pulse");
    stepThrough(restored, restoredAcquisitionStart + 40U);
    const midibuffer_test::Trace restoredContinuation =
        midibuffer_test::trace();
    expect(snapshot(restored).clockRunning &&
               sameMidiTrace(originalContinuation, restoredContinuation),
           "after saved-interval reconstruction, subsequent output bytes and routing equal uninterrupted continuation");

    midibuffer_test::HostDouble captureSource;
    expect(captureSource.instantiate(1),
           "capture-enabled preset source constructs");
    startCapture(captureSource);
    acquireClock(captureSource);
    sendMidi(captureSource, 0x93U, 72U, 100U);
    const uint32_t captureSavedCount = snapshot(captureSource).eventCount;
    midibuffer_test::PresetImage captureImage;
    expect(captureSource.savePreset(captureImage),
           "capture-enabled state saves without finalizing or mutating history");
    expect(snapshot(captureSource).captureEnabled &&
               snapshot(captureSource).eventCount == captureSavedCount,
           "save-time snapshot is coherent and non-mutating during capture");

    midibuffer_test::HostDouble captureRestored;
    expect(captureRestored.instantiate(1) &&
               captureRestored.loadPreset(captureImage, true) &&
               snapshot(captureRestored).captureEnabled &&
               !snapshot(captureRestored).clockRunning,
           "saved capture-enabled state restores under parameter-after ordering with live-clock gating");
    sendMidi(captureRestored, 0x94U, 73U, 100U);
    expect(snapshot(captureRestored).eventCount == captureSavedCount,
           "restored capture rejects input before a fresh external clock");
    clockPulse(captureRestored);
    sendMidi(captureRestored, 0x94U, 73U, 100U);
    expect(snapshot(captureRestored).eventCount == captureSavedCount,
           "first fresh clock pulse still gates restored capture");
    clockPulse(captureRestored);
    sendMidi(captureRestored, 0x94U, 73U, 100U);
    expect(snapshot(captureRestored).eventCount == captureSavedCount + 1U,
           "second fresh clock pulse resumes the restored capture state");
    stopCapture(captureRestored);
    expect(snapshot(captureRestored).eventCount == captureSavedCount + 3U,
           "restored recorded-note ownership contributes its ending alongside subsequent input");
}

void verifyInflightPresetSchedulingContinuation() {
    const uint32_t interval = 64U;
    midibuffer_test::HostDouble source;
    expect(source.instantiate(1),
           "in-flight preset scheduling source constructs");
    startCapture(source);
    expect(clockPulseAt(source, 0U) && clockPulseAt(source, interval),
           "in-flight preset history acquires a steady source interval");
    const uint64_t originalStart = snapshot(source).currentPulse;

    // One pulse deliberately contains multiple scheduled events, including
    // note/sustain endings after the point where the preset will be saved.
    sendMidi(source, 0x92U, 60U, 100U);  // offset 8
    sendMidi(source, 0xB2U, 64U, 127U);  // offset 8
    noClockBlock(source);
    sendMidi(source, 0xB2U, 1U, 23U);  // offset 16
    noClockBlock(source);
    sendMidi(source, 0xD2U, 31U, 0U);  // offset 24
    noClockBlock(source);
    noClockBlock(source);
    sendMidi(source, 0x82U, 60U, 0U);  // offset 40
    sendMidi(source, 0xB2U, 64U, 0U);  // offset 40
    noClockBlock(source);
    noClockBlock(source);
    sendMidi(source, 0xB2U, 11U, 79U);  // offset 56

    expect(clockPulseAt(source, interval * 2U),
           "in-flight history advances to the original range tail");
    sendMidi(source, 0x92U, 62U, 101U);  // offset 8
    noClockBlock(source);
    sendMidi(source, 0xA2U, 62U, 17U);  // offset 16
    noClockBlock(source);
    noClockBlock(source);
    noClockBlock(source);
    sendMidi(source, 0x82U, 62U, 0U);  // offset 40

    expect(clockPulseAt(source, interval * 3U),
           "in-flight history reaches the replacement range");
    const uint64_t replacementStart = snapshot(source).currentPulse;
    sendMidi(source, 0x93U, 70U, 102U);  // offset 8
    noClockBlock(source);
    noClockBlock(source);
    noClockBlock(source);
    noClockBlock(source);
    sendMidi(source, 0x83U, 70U, 0U);  // offset 40
    expect(clockPulseAt(source, interval * 4U) &&
               clockPulseAt(source, interval * 5U),
           "in-flight history closes both selectable ranges");
    stopCapture(source);

    changeParameter(source, kPlaybackDestinationParameter, 1);
    changeParameter(source, kPlaybackChannelParameter, 7);
    expect(midibuffer::setPulseSelection(source.algorithm(), originalStart,
                                         originalStart + 2U) &&
               midibuffer::startPlayback(source.algorithm()),
           "in-flight preset fixture selects and starts the original range");
    const uint64_t sourceIntervalOrigin = interval * 6U;
    expect(clockPulseAt(source, sourceIntervalOrigin),
           "in-flight preset fixture opens its saved playback interval");
    stepThrough(source, sourceIntervalOrigin + 16U);
    expect(midibuffer::setPulseSelection(source.algorithm(), replacementStart,
                                         replacementStart + 2U),
           "in-flight preset fixture queues a range change before save");

    const midibuffer::CaptureSnapshot saved = snapshot(source);
    expect(saved.playbackActive && saved.playbackIntervalOpen &&
               saved.playbackNextEventScheduled &&
               saved.pendingNextEndingScheduled &&
               saved.pendingNoteEndingCount == 1U &&
               saved.pendingSustainReleaseCount == 1U &&
               saved.rangeTransitionPending &&
               saved.playbackPulse == originalStart &&
               saved.playbackNextEventSample == sourceIntervalOrigin + 24U &&
               saved.pendingNextEndingSample == sourceIntervalOrigin + 40U,
           "save boundary lies between regular events and before same-interval note/sustain endings");

    midibuffer_test::PresetImage image;
    expect(source.savePreset(image),
           "in-flight scheduler state saves through the actual callback");
    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(1) && restored.loadPreset(image),
           "in-flight scheduler state loads into a fresh instance");
    const midibuffer::CaptureSnapshot loaded = snapshot(restored);
    expect(loaded.playbackIntervalOpen &&
               loaded.playbackNextEventScheduled &&
               loaded.pendingNextEndingScheduled &&
               loaded.playbackIntervalOrdinal == saved.playbackIntervalOrdinal &&
               loaded.playbackEventIndex == saved.playbackEventIndex &&
               loaded.playbackNextEventSample ==
                   saved.playbackNextEventSample &&
               loaded.pendingNextEndingSample ==
                   saved.pendingNextEndingSample,
           "callback round trip retains the exact in-flight cursor, ordinal, and schedules while clock-gated");

    midibuffer_test::resetTrace();
    stepThrough(source, sourceIntervalOrigin + 56U);
    expect(clockPulseAt(source, sourceIntervalOrigin + interval),
           "uninterrupted control advances through the original range tail");
    stepThrough(source, sourceIntervalOrigin + interval + 56U);
    expect(clockPulseAt(source, sourceIntervalOrigin + interval * 2U),
           "uninterrupted control reaches the pending range wrap");
    stepThrough(source, sourceIntervalOrigin + interval * 2U + 56U);
    const midibuffer_test::Trace uninterrupted = midibuffer_test::trace();
    const midibuffer::CaptureSnapshot uninterruptedState = snapshot(source);

    midibuffer_test::resetTrace();
    expect(clockPulseAt(restored, 0U) &&
               midibuffer_test::trace().midiCallCount == 0U &&
               clockPulseAt(restored, interval),
           "restored in-flight playback remains silent until clock reacquisition");
    const uint64_t restoredIntervalOrigin =
        loaded.sampleCursor + static_cast<uint64_t>(interval);
    stepThrough(restored, interval + 56U);
    expect(clockPulseAt(restored, interval * 2U),
           "restored continuation advances through the original range tail");
    stepThrough(restored, interval * 2U + 56U);
    expect(clockPulseAt(restored, interval * 3U),
           "restored continuation reaches the pending range wrap");
    stepThrough(restored, interval * 3U + 56U);
    const midibuffer_test::Trace resumed = midibuffer_test::trace();
    const midibuffer::CaptureSnapshot resumedState = snapshot(restored);

    expect(uninterrupted.midiCallCount >= 8U &&
               sameNormalizedMidiTrace(
                   uninterrupted, sourceIntervalOrigin, resumed,
                   restoredIntervalOrigin),
           "multiple resumed pulses preserve normalized timestamps, bytes, routing, same-interval endings, and range-wrap output");
    expect(resumedState.playbackPulse == uninterruptedState.playbackPulse &&
               resumedState.playbackEventIndex ==
                   uninterruptedState.playbackEventIndex &&
               resumedState.playbackIntervalOrdinal ==
                   uninterruptedState.playbackIntervalOrdinal &&
               resumedState.pendingNoteEndingCount ==
                   uninterruptedState.pendingNoteEndingCount &&
               resumedState.pendingSustainReleaseCount ==
                   uninterruptedState.pendingSustainReleaseCount &&
               resumedState.activeSelection.startPulse == replacementStart &&
               resumedState.activeSelection.endPulse ==
                   replacementStart + 2U &&
               !resumedState.rangeTransitionPending,
           "restored scheduler state remains aligned after subsequent pulses and pending range adoption");
}

void verifyPresetSupportedBufferRangeAndCost() {
    for (int32_t megabytes = 1; megabytes <= 5; ++megabytes) {
        midibuffer_test::HostDouble source;
        expect(source.instantiate(megabytes),
               "supported-range preset source constructs");
        changeParameter(source, kClearRecordingParameter, 1);
        startCapture(source);
        acquireClock(source);
        sendMidi(source, 0xB0U, 7U,
                 static_cast<uint8_t>(megabytes * 10));
        changeParameter(source, kPlaybackParameter, 1);
        midibuffer_test::PresetImage image;
        midibuffer_test::HostDouble restored;
        expect(source.savePreset(image) && restored.instantiate(megabytes) &&
                   restored.loadPreset(image) &&
                   restored.parameter(kPlaybackParameter) == 1 &&
                   restored.parameter(kClearRecordingParameter) == 1 &&
                   !snapshot(restored).clearRecordingArmed &&
                   snapshot(restored).eventCount == 1U,
               "valid preset round-trips both shared controls and retained custom state at each supported buffer specification");
        midibuffer_test::PresetImage secondImage;
        expect(restored.savePreset(secondImage) && image.equals(secondImage),
               "each supported buffer specification has exhaustive saved-state equality");
    }

    midibuffer_test::HostDouble maximum;
    expect(maximum.instantiate(5),
           "maximum-size preset cost fixture constructs");
    changeParameter(maximum, kClearRecordingParameter, 1);
    startCapture(maximum);
    acquireClock(maximum);
    const uint32_t capacity = snapshot(maximum).eventCapacity;
    for (uint32_t index = 0; index < capacity; ++index) {
        sendMidi(maximum, 0xB0U, 7U,
                 static_cast<uint8_t>(index & 0x7fU));
    }
    changeParameter(maximum, kPlaybackParameter, 1);
    midibuffer_test::PresetImage maximumImage;
    const std::clock_t saveStart = std::clock();
    expect(maximum.savePreset(maximumImage),
           "full 5 MB retained history serializes through the production callback");
    const std::clock_t saveEnd = std::clock();
    midibuffer_test::HostDouble restored;
    expect(restored.instantiate(5),
           "full 5 MB fresh restore instance constructs");
    const std::clock_t loadStart = std::clock();
    expect(restored.loadPreset(maximumImage),
           "full 5 MB retained history deserializes through the production callback");
    const std::clock_t loadEnd = std::clock();
    midibuffer_test::PresetImage maximumRoundTrip;
    expect(snapshot(restored).eventCount == capacity &&
               restored.parameter(kPlaybackParameter) == 1 &&
               restored.parameter(kClearRecordingParameter) == 1 &&
               !snapshot(restored).clearRecordingArmed &&
               restored.savePreset(maximumRoundTrip) &&
               maximumImage.equals(maximumRoundTrip),
           "full supported payload preserves both shared controls, every retained event, and all saved state");
    const double saveMilliseconds =
        1000.0 * static_cast<double>(saveEnd - saveStart) / CLOCKS_PER_SEC;
    const double loadMilliseconds =
        1000.0 * static_cast<double>(loadEnd - loadStart) / CLOCKS_PER_SEC;
    std::printf(
        "MEASURE: native full-5MB preset payload ~= %llu bytes, save %.2f ms, load %.2f ms (host-double JSON overhead; no firmware budget published)\n",
        static_cast<unsigned long long>(maximumImage.payloadBytes()),
        saveMilliseconds, loadMilliseconds);
}

enum DrawCadence {
    kNoDraws,
    kSparseDraws,
    kFrequentDraws,
};

void drawForCadence(midibuffer_test::HostDouble& host, DrawCadence cadence,
                    unsigned stage) {
    unsigned count = 0U;
    if (cadence == kFrequentDraws) {
        count = 3U;
    } else if (cadence == kSparseDraws && (stage % 3U) == 0U) {
        count = 1U;
    }
    while (count-- != 0U) {
        expect(host.factory()->draw(host.algorithm()),
               "cadence scenario draw callback completes");
    }
}

struct DrawCadenceResult {
    midibuffer_test::Trace trace;
    midibuffer::CaptureSnapshot state;
    midibuffer_test::PresetImage image;
    bool pendingObserved;
};

void runDrawCadenceScenario(DrawCadence cadence, DrawCadenceResult& result) {
    midibuffer_test::HostDouble host;
    TransportFixture fixture = prepareTransportHistory(host);
    drawForCadence(host, cadence, 0U);
    beginTransportOnHeldFirstBeat(host, fixture);
    drawForCadence(host, cadence, 1U);

    resetPulse(host);
    drawForCadence(host, cadence, 2U);
    fixture.lastClockSample = host.elapsedSamples() + 8U;
    expect(clockPulseAt(host, fixture.lastClockSample),
           "cadence scenario resumes from reset on an identical clock");
    stepThrough(host, fixture.lastClockSample + 8U);
    drawForCadence(host, cadence, 3U);

    _NT_float3 pots = {-1.0f, -1.0f, -1.0f};
    host.factory()->setupUi(host.algorithm(), pots);
    moveUi(host, kNT_potButtonR, pots[0], pots[1], 0.5f);
    drawForCadence(host, cadence, 4U);
    moveUi(host, kNT_potButtonR | kNT_potR,
           pots[0], pots[1], 0.25f, 0, 0, kNT_potButtonR);
    moveUi(host, 0U, pots[0], pots[1], 0.25f, 0, 0, kNT_potButtonR);
    drawForCadence(host, cadence, 5U);

    expect(midibuffer::setPulseSelection(
               host.algorithm(), fixture.firstSelectedPulse + 1U,
               fixture.firstSelectedPulse + 3U),
           "cadence scenario queues the same valid pending range");
    result.pendingObserved = snapshot(host).rangeTransitionPending;
    sendMidi(host, 0x94U, 72U, 96U);
    noClockBlock(host);
    drawForCadence(host, cadence, 6U);

    result.trace = midibuffer_test::trace();
    result.state = snapshot(host);
    expect(result.trace.midiCallCount != 0U,
           "cadence scenario emits a non-empty timestamped MIDI stream");
    expect(result.pendingObserved && result.state.rangeTransitionPending &&
               result.state.playbackActive && !result.state.captureEnabled,
           "cadence scenario retains capture, transport, and pending-transition evidence");
    expect(host.savePreset(result.image),
           "cadence scenario captures complete scheduler state");
}

void verifyDrawNoninterferenceAndIncrementalCost() {
    midibuffer_test::resetTrace();
    DrawCadenceResult zero = {};
    runDrawCadenceScenario(kNoDraws, zero);
    midibuffer_test::resetTrace();
    DrawCadenceResult sparse = {};
    runDrawCadenceScenario(kSparseDraws, sparse);
    midibuffer_test::resetTrace();
    DrawCadenceResult frequent = {};
    runDrawCadenceScenario(kFrequentDraws, frequent);

    expect(sameExactMidiTrace(zero.trace, sparse.trace) &&
               sameExactMidiTrace(zero.trace, frequent.trace),
           "zero, sparse, and frequent draws preserve outgoing MIDI bytes, order, destinations, and exact scheduler timestamps");
    expect(zero.image.equals(sparse.image) &&
               zero.image.equals(frequent.image) &&
               sameSelectionAndTransport(zero.state, sparse.state) &&
               sameSelectionAndTransport(zero.state, frequent.state) &&
               zero.state.captureEnabled == sparse.state.captureEnabled &&
               zero.state.captureEnabled == frequent.state.captureEnabled &&
               zero.state.clockRunning == sparse.state.clockRunning &&
               zero.state.clockRunning == frequent.state.clockRunning &&
               zero.state.eventCount == sparse.state.eventCount &&
               zero.state.eventCount == frequent.state.eventCount,
           "draw cadence preserves canonical capture, transport, pending range, scheduler, and retained-event state exactly");

    midibuffer_test::HostDouble benchmark;
    expect(installRangeFixture(benchmark, 100U, 108U, 100U, 108U) &&
               midibuffer::startPlayback(benchmark.algorithm()),
           "incremental head-cost fixture constructs and arms");
    acquireClock(benchmark);
    clockPulse(benchmark);
    expect(drawHasHeadAt(benchmark, 34),
           "incremental head-cost fixture reaches an eligible non-bracket frame");
    midibuffer_test::resetTrace();
    expect(benchmark.factory()->draw(benchmark.algorithm()) &&
               playbackHeadLineCount() == 1U &&
               framebufferColourPixelCount(12U) == 39U,
           "head adds exactly one intensity-12 line and 39 inclusive pixels");

    const unsigned iterations = 20000U;
    const uint64_t allocationsBefore = midibuffer_test::heapAllocationCount();
    std::clock_t headStart = std::clock();
    for (unsigned index = 0; index < iterations; ++index) {
        benchmark.factory()->draw(benchmark.algorithm());
    }
    const std::clock_t headEnd = std::clock();
    midibuffer::stopPlayback(benchmark.algorithm());
    const std::clock_t stoppedStart = std::clock();
    for (unsigned index = 0; index < iterations; ++index) {
        benchmark.factory()->draw(benchmark.algorithm());
    }
    const std::clock_t stoppedEnd = std::clock();
    const double headNanoseconds =
        1000000000.0 * static_cast<double>(headEnd - headStart) /
        (static_cast<double>(CLOCKS_PER_SEC) * iterations);
    const double stoppedNanoseconds =
        1000000000.0 * static_cast<double>(stoppedEnd - stoppedStart) /
        (static_cast<double>(CLOCKS_PER_SEC) * iterations);
    expect(midibuffer_test::heapAllocationCount() == allocationsBefore,
           "measured draw paths use fixed storage and perform no allocation");
    std::printf(
        "MEASURE: native framebuffer draw over %u iterations: eligible head %.1f ns/draw, stopped %.1f ns/draw, delta %.1f ns/draw (process CPU clock; no FPS or hardware claim)\n",
        iterations, headNanoseconds, stoppedNanoseconds,
        headNanoseconds - stoppedNanoseconds);
    std::printf(
        "MEASURE: fixed native RangeMotionState storage = %llu bytes; head trace = one coordinate call site and one 39-pixel line (source/trace audit)\n",
        static_cast<unsigned long long>(sizeof(midibuffer::RangeMotionState)));
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
    verifyMusicalDurationFormattingAndReadouts();
    if (host.algorithm() != NULL) {
        verifyBoundaryCallbacks(host);
    }
    verifyClockedCaptureAndReacquisition();
    verifyChannelAndEventEligibility();
    verifyTimelineSelectionDisplayAndControls();
    verifyDirectionalBoundaryHandles();
    verifyPlaybackHeadObservationAndWideMapping();
    verifyShowAllAndRelativeTimelineNavigation();
    verifyZoomAwareBoundaryEditing();
    verifyIntegratedRangeMotionAndFineTargets();
    verifyCaptureStopEndings();
    verifyRollingHistoryAndSelectionInvalidation();
    verifyRetainedReplayRoutingMatrix();
    verifyRecoverableExpressionFiltering();
    verifyRunningAverageAndProportionalScheduling();
    verifyEarlyPulseCatchUpOrder();
    verifyPlaybackClockAcquisition();
    verifyAtomicRangeTransitionAtWrap();
    verifyCallbackRangeEditsAtWrap();
    verifyPlaybackCaptureExclusionAndManualStop();
    verifyResetCleanupAndCoincidence();
    verifyClockLossCleanupAndContinuation();
    verifyLoopTailDurationsAndSustainOwnership();
    verifyOverriddenChannelSustainReleaseOwnership();
    verifyOverriddenChannelRetriggerOwnership();
    verifyPendingEndingDiscontinuityCleanup();
    verifyLiveEmergencySilenceMatrix();
    verifyTimelinePlaybackToggle();
    verifySharedPlaybackParameter();
    verifyOneShotClearRecording();
    verifySharedControlPresetRestoration();
    verifyLegacySharedControlPresetMigration();
    verifyTimedRightEncoderPanic();
    verifyLegacyNavigationPresetCompatibility();
    verifyShowAllPresetPolicy();
    verifyCompletePresetRoundTripsAndContinuation();
    verifyInflightPresetSchedulingContinuation();
    verifyPresetSupportedBufferRangeAndCost();
    verifyDrawNoninterferenceAndIncrementalCost();
    verifyHostOutputTrace();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d callback contract checks failed\n", gFailures);
        return 1;
    }
    std::printf("PASS: NT adapter and bounded capture-history contract\n");
    return 0;
}
