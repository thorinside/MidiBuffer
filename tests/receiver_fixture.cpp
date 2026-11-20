// Software-receiver fixture source (req-014, ac-015).
//
// Feeds one scripted MPE performance into the production callbacks through
// the host double and writes two exact logs for the external receiver bridge:
// the direct controller stream (as sent to MidiBuffer) and the production
// MidiCall trace (setup, context, replayed passes and stop cleanup). The
// receiver, not this file, is the behavior oracle; native byte/timing
// qualification lives in qualification_test.cpp.
//
// Usage: receiver_fixture <lower|upper> <directLog> <replayLog>

#include "host_double.hpp"

#include "../src/midibuffer_core.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const size_t kCaptureParameter = 2;
const size_t kPlaybackParameter = 10;
const size_t kMpeModeParameter = 12;
const uint32_t kInterval = 24000U; // 0.5 s pulses at simulated 48 kHz
const uint64_t kLead = 48000U;     // log origin before the take/pass start

struct Message {
    uint64_t sample;
    uint32_t destination;
    uint8_t bytes[3];
    uint8_t size;
};

std::vector<Message> gReplay;

void drain(const midibuffer_test::MidiCall* calls, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        Message message = {calls[index].dispatchSample, calls[index].destination,
                           {calls[index].bytes[0], calls[index].bytes[1],
                            calls[index].bytes[2]},
                           calls[index].size};
        gReplay.push_back(message);
    }
}

struct Rig {
    midibuffer_test::HostDouble host;
    uint64_t nextPulse;
    std::vector<Message> direct;
};

void setParameter(Rig& rig, size_t parameter, int16_t value) {
    rig.host.setParameter(parameter, value);
    rig.host.factory()->parameterChanged(rig.host.algorithm(),
                                         static_cast<int>(parameter));
}

void advanceTo(Rig& rig, uint64_t target) {
    target = (target + 3U) & ~static_cast<uint64_t>(3U); // 4-sample host steps
    while (rig.host.elapsedSamples() < target) {
        const uint64_t base = rig.host.elapsedSamples();
        const uint64_t count = target - base < 64U ? target - base : 64U;
        rig.host.clearFrames();
        while (rig.nextPulse < base + count) {
            rig.host.bus(1)[rig.nextPulse - base] = 2.0f;
            rig.nextPulse += kInterval;
        }
        rig.host.step(static_cast<int>(count / 4U));
    }
}

uint64_t pulse(Rig& rig) {
    const uint64_t sample = rig.nextPulse;
    advanceTo(rig, (sample + 4U) & ~static_cast<uint64_t>(3U));
    return sample;
}

void send(Rig& rig, uint8_t status, uint8_t data1, uint8_t data2) {
    const uint8_t type = status & 0xf0U;
    Message message = {rig.host.elapsedSamples(), 0U, {status, data1, data2},
                       static_cast<uint8_t>(type == 0xd0U || type == 0xc0U ? 2U : 3U)};
    if (message.size == 2U) {
        message.bytes[2] = 0U;
    }
    rig.direct.push_back(message);
    rig.host.factory()->midiMessage(rig.host.algorithm(), status, data1, data2);
}

// Performance offsets (time-ordered) relative to the take's pulse edges. Member indices are
// zone-relative (manager, first, second, third member).
struct Event {
    uint32_t pulse;
    uint32_t offset;
    uint8_t type;
    uint8_t voice; // 0 manager, 1..3 members
    uint8_t data1;
    uint8_t data2;
};

const Event kTake[] = {
    {0U, 1U, 0x90U, 1U, 57U, 100U},     // A 220 Hz
    {0U, 1U, 0x90U, 2U, 64U, 100U},     // B 329.63 Hz
    {0U, 12000U, 0xd0U, 1U, 127U, 0U},  // A pressure only
    {0U, 18000U, 0xb0U, 0U, 64U, 127U}, // manager sustain held to the end
    {1U, 0U, 0xe0U, 1U, 0x55U, 0x42U},  // A bend 8533 (+2 st at 48)
    {1U, 3000U, 0xb0U, 2U, 64U, 127U},  // B member sustain held to the end
    {1U, 12000U, 0xb0U, 1U, 74U, 0U},   // A timbre low
    {1U, 12000U, 0xb0U, 2U, 74U, 127U}, // B timbre high
    {2U, 0U, 0xe0U, 0U, 0x00U, 0x60U},  // manager bend 12288 (+1 st at 2)
    {2U, 1U, 0x90U, 3U, 74U, 90U},      // C 587.33 Hz
    {3U, 0U, 0x80U, 3U, 74U, 64U},      // C release
    {3U, 1U, 0xe0U, 0U, 0x00U, 0x40U},  // manager bend centre
};

uint8_t channel(bool upper, uint8_t voice) {
    return static_cast<uint8_t>(upper ? 15U - voice : voice);
}

void writeLog(const char* path, const std::vector<Message>& messages,
              uint64_t start, const char* zone, const char* kind,
              const midibuffer::MpePlaybackSnapshot* state) {
    FILE* file = std::fopen(path, "w");
    if (file == NULL) {
        std::perror(path);
        return;
    }
    const uint64_t origin = start - kLead;
    std::fprintf(file, "# kind %s\n# zone %s\n# sampleRate 48000\n# pulse %u\n"
                 "# start %llu\n# passPulses 4\n# stop %llu\n",
                 kind, zone, kInterval, static_cast<unsigned long long>(kLead),
                 static_cast<unsigned long long>(kLead + 5U * kInterval + 6000U));
    if (state != NULL) {
        std::fprintf(file, "# generatedSetup %u\n# generatedContext %u\n"
                     "# generatedCleanup %u\n# ownedNotesAfterStop %u\n"
                     "# ownedHoldsAfterStop %u\n",
                     state->generatedSetup, state->generatedContext,
                     state->generatedCleanup, state->ownedNotes, state->ownedHolds);
    }
    for (size_t index = 0; index < messages.size(); ++index) {
        const Message& message = messages[index];
        if (message.sample < origin) {
            std::fprintf(stderr, "%s message precedes log origin\n", kind);
            continue;
        }
        std::fprintf(file, "%llu %u %02x %02x",
                     static_cast<unsigned long long>(message.sample - origin),
                     message.destination, message.bytes[0], message.bytes[1]);
        if (message.size == 3U) {
            std::fprintf(file, " %02x", message.bytes[2]);
        }
        std::fprintf(file, "\n");
    }
    std::fclose(file);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 || (std::strcmp(argv[1], "lower") && std::strcmp(argv[1], "upper"))) {
        std::fprintf(stderr, "usage: receiver_fixture <lower|upper> <direct> <replay>\n");
        return 2;
    }
    const bool upper = std::strcmp(argv[1], "upper") == 0;
    Rig rig;
    rig.nextPulse = 3U;
    if (!rig.host.instantiate(1)) {
        std::fprintf(stderr, "host instantiate failed\n");
        return 1;
    }
    for (unsigned index = 0; index < 9U; ++index) {
        pulse(rig);
    }
    // Controller power-on setup (MCM, 3 members) and pre-capture context.
    const uint8_t manager = channel(upper, 0U);
    send(rig, 0xb0U | manager, 101U, 0U);
    send(rig, 0xb0U | manager, 100U, 6U);
    send(rig, 0xb0U | manager, 6U, 3U);
    send(rig, 0xb0U | manager, 64U, 0U);
    for (uint8_t voice = 1U; voice <= 3U; ++voice) {
        send(rig, 0xb0U | channel(upper, voice), 64U, 0U);
        send(rig, 0xe0U | channel(upper, voice), 0U, 0x40U);
        send(rig, 0xd0U | channel(upper, voice), 0U, 0U);
        send(rig, 0xb0U | channel(upper, voice), 74U, 64U);
    }
    pulse(rig);
    setParameter(rig, kCaptureParameter, 1);
    uint64_t starts[5] = {};
    starts[0] = pulse(rig);
    const uint64_t startPulse = midibuffer::captureSnapshot(rig.host.algorithm()).currentPulse;
    for (size_t index = 0; index < sizeof(kTake) / sizeof(kTake[0]); ++index) {
        const Event& event = kTake[index];
        for (uint32_t next = 1U; next <= event.pulse; ++next) {
            if (starts[next] == 0U) {
                starts[next] = pulse(rig);
            }
        }
        advanceTo(rig, starts[event.pulse] + (event.offset == 0U ? 1U : event.offset));
        send(rig, event.type | channel(upper, event.voice), event.data1, event.data2);
    }
    starts[4] = pulse(rig);
    // The player releases A and B after the selected interval ends.
    advanceTo(rig, starts[4] + 12000U);
    send(rig, 0x80U | channel(upper, 1U), 57U, 64U);
    send(rig, 0x80U | channel(upper, 2U), 64U, 64U);
    advanceTo(rig, starts[4] + 18000U);
    send(rig, 0xb0U | channel(upper, 0U), 64U, 0U);
    send(rig, 0xb0U | channel(upper, 2U), 64U, 0U);
    setParameter(rig, kCaptureParameter, 0);
    if (!midibuffer::setPulseSelection(rig.host.algorithm(), startPulse, startPulse + 4U)) {
        std::fprintf(stderr, "selection rejected\n");
        return 1;
    }
    for (unsigned index = 0; index < 10U; ++index) {
        pulse(rig);
    }
    setParameter(rig, kMpeModeParameter, 1);

    midibuffer_test::resetTrace();
    midibuffer_test::setMidiTraceDrain(drain);
    setParameter(rig, kPlaybackParameter, 1);
    uint64_t passStart = 0U;
    midibuffer::MpePlaybackSnapshot state = {};
    for (unsigned index = 0; index < 40U && passStart == 0U; ++index) {
        const uint64_t sample = pulse(rig);
        midibuffer::mpePlaybackSnapshot(rig.host.algorithm(), state);
        if (state.phase == midibuffer::kMpePhasePlaying) {
            passStart = sample;
        }
    }
    if (passStart == 0U) {
        std::fprintf(stderr, "MPE playback never started (status %u)\n", state.status);
        return 1;
    }
    // One full pass, then stop 1.25 pulses into the second while A/B sound.
    advanceTo(rig, passStart + 5U * kInterval + 6000U);
    setParameter(rig, kPlaybackParameter, 0);
    advanceTo(rig, passStart + 7U * kInterval);
    drain(midibuffer_test::trace().midiCalls, midibuffer_test::trace().midiCallCount);
    midibuffer_test::resetTrace();
    midibuffer_test::setMidiTraceDrain(NULL);
    midibuffer::mpePlaybackSnapshot(rig.host.algorithm(), state);

    writeLog(argv[2], rig.direct, starts[0], argv[1], "direct", NULL);
    writeLog(argv[3], gReplay, passStart, argv[1], "replay", &state);
    std::printf("FIXTURE %s: direct=%zu replay=%zu setup=%u context=%u cleanup=%u "
                "ownedNotes=%u ownedHolds=%u stopSample=%llu\n",
                argv[1], rig.direct.size(), gReplay.size(), state.generatedSetup,
                state.generatedContext, state.generatedCleanup, state.ownedNotes,
                state.ownedHolds,
                static_cast<unsigned long long>(kLead + 5U * kInterval + 6000U));
    return 0;
}
