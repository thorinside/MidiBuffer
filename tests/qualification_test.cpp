// Exact-candidate qualification (req-012/req-013, ac-013/ac-014).
//
// Drives the production factory callbacks through the existing host double
// with simulated 48 kHz sample time and patched clock edges. Expected bytes,
// lengths, channels, order, destinations and integer dispatch samples come
// from oracles written here from the approved Spec: positive-half-up
// proportional scaling, half-open pulse selections, a receiver-state model for
// starting context and an input generator for the defined reference template.
// No production rounding, pairing or scheduling code is reused.
//
// Existing callback_contract_test cases prove individual behaviors at small
// scale; they do not replay the exact 3,000/4,500/10,000 event/s templates,
// rolling 1/5 MB overwrite, 1,000-wrap drift or the frame/cluster matrices
// against an independent oracle, which is the coverage gap closed here.
//
// Operation counts are algorithmic evidence only: they are not hardware
// callback deadlines, DIN throughput or downstream delivery guarantees.

#include "host_double.hpp"

#include "../src/midibuffer_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const size_t kCaptureParameter = 2;
const size_t kPlaybackDestinationParameter = 4;
const size_t kPlaybackParameter = 10;
const size_t kMpeModeParameter = 12;

const uint32_t kSampleRate = 48000U;
const uint32_t kSourceInterval = 24000U;

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

// ---------------------------------------------------------------------------
// Independent oracle arithmetic.

// Positive halves round upward: 6001 x 12000 / 24000 = 3000.5 -> 3001.
uint64_t halfUpScale(uint64_t offset, uint64_t played, uint64_t source) {
    return (2U * offset * played + source) / (2U * source);
}

uint8_t messageLength(uint8_t status) {
    const uint8_t type = status & 0xf0U;
    return type == 0xc0U || type == 0xd0U ? 2U : 3U;
}

bool isNoteEnding(const uint8_t* bytes) {
    const uint8_t type = bytes[0] & 0xf0U;
    return type == 0x80U || (type == 0x90U && bytes[2] == 0U);
}

bool isAttack(const uint8_t* bytes) {
    return (bytes[0] & 0xf0U) == 0x90U && bytes[2] != 0U;
}

// Playback Destination enum "Breakout, USB, Select Bus, Internal, All".
uint32_t destinationMask(int16_t value) {
    static const uint32_t kMasks[] = {
        kNT_destinationBreakout, kNT_destinationUSB, kNT_destinationSelectBus,
        kNT_destinationInternal,
        kNT_destinationBreakout | kNT_destinationUSB | kNT_destinationSelectBus |
            kNT_destinationInternal};
    return kMasks[value < 0 ? 0 : value];
}

unsigned bitCount(uint32_t value) {
    unsigned count = 0U;
    for (; value != 0U; value &= value - 1U) {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Complete MIDI trace collection. The host-double trace drains full batches
// here, so observation capacity cannot masquerade as plug-in loss.

struct Call {
    uint64_t sample;
    uint32_t destination;
    uint8_t bytes[3];
    uint8_t size;
};

const size_t kCallCapacity = 600000U;
Call* gCalls = NULL;
size_t gCallCount = 0U;
bool gCallOverflow = false;

// Trace-derived per-step NT send calls and destination bits, kept separately
// from the plug-in's own MpeWorkCounters so the two can be cross-checked.
uint64_t gDrainedFanout = 0U;
size_t gTraceSummed = 0U;
uint64_t gTraceFanout = 0U;
uint64_t gStepMaximumSends = 0U;
uint64_t gStepMaximumFanout = 0U;

void appendCalls(const midibuffer_test::MidiCall* calls, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        gDrainedFanout += bitCount(calls[index].destination);
    }
    gTraceSummed = 0U;
    gTraceFanout = 0U;
    for (size_t index = 0; index < count; ++index) {
        if (gCallCount >= kCallCapacity) {
            gCallOverflow = true;
            return;
        }
        Call& call = gCalls[gCallCount++];
        call.sample = calls[index].dispatchSample;
        call.destination = calls[index].destination;
        std::memcpy(call.bytes, calls[index].bytes, 3U);
        call.size = calls[index].size;
    }
}

void beginCollection() {
    midibuffer_test::resetTrace();
    midibuffer_test::setMidiTraceDrain(appendCalls);
    gCallCount = 0U;
    gCallOverflow = false;
    gDrainedFanout = 0U;
    gTraceSummed = 0U;
    gTraceFanout = 0U;
    gStepMaximumSends = 0U;
    gStepMaximumFanout = 0U;
}

uint64_t sentCalls() {
    return gCallCount + midibuffer_test::trace().midiCallCount;
}

uint64_t sentFanout() {
    const midibuffer_test::Trace& current = midibuffer_test::trace();
    for (; gTraceSummed < current.midiCallCount; ++gTraceSummed) {
        gTraceFanout += bitCount(current.midiCalls[gTraceSummed].destination);
    }
    return gDrainedFanout + gTraceFanout;
}

void finishCollection() {
    appendCalls(midibuffer_test::trace().midiCalls,
                midibuffer_test::trace().midiCallCount);
    midibuffer_test::resetTrace();
    midibuffer_test::setMidiTraceDrain(NULL);
}

// ---------------------------------------------------------------------------
// Simulated host: 4-sample host boundaries, at most `frames` per step, and
// one-frame clock gates at exact samples.

struct Rig {
    midibuffer_test::HostDouble host;
    uint64_t nextPulse;
    uint32_t interval;
    uint32_t frames;
    bool clockFault;
};

void setParameter(Rig& rig, size_t parameter, int16_t value) {
    rig.host.setParameter(parameter, value);
    rig.host.factory()->parameterChanged(rig.host.algorithm(),
                                         static_cast<int>(parameter));
}

void send(Rig& rig, const uint8_t* bytes) {
    rig.host.factory()->midiMessage(rig.host.algorithm(), bytes[0], bytes[1],
                                    bytes[2]);
}

void send3(Rig& rig, uint8_t status, uint8_t data1, uint8_t data2) {
    const uint8_t bytes[3] = {status, data1, data2};
    send(rig, bytes);
}

void advanceTo(Rig& rig, uint64_t target) {
    midibuffer_test::HostDouble& host = rig.host;
    while (host.elapsedSamples() < target) {
        const uint64_t base = host.elapsedSamples();
        uint64_t count = target - base;
        if (count > rig.frames) {
            count = rig.frames;
        }
        if (count % 4U != 0U || rig.nextPulse < base) {
            rig.clockFault = true;
            return;
        }
        host.clearFrames();
        while (rig.nextPulse < base + count) {
            host.bus(1)[rig.nextPulse - base] = 2.0f;
            rig.nextPulse += rig.interval;
        }
        const uint64_t calls = sentCalls();
        const uint64_t fanout = sentFanout();
        host.step(static_cast<int>(count / 4U));
        if (sentCalls() - calls > gStepMaximumSends) {
            gStepMaximumSends = sentCalls() - calls;
        }
        if (sentFanout() - fanout > gStepMaximumFanout) {
            gStepMaximumFanout = sentFanout() - fanout;
        }
    }
}

uint64_t alignUp4(uint64_t sample) {
    return (sample + 3U) & ~static_cast<uint64_t>(3U);
}

// Delivers the next clock edge and the rest of its host step; returns it.
uint64_t pulse(Rig& rig) {
    const uint64_t sample = rig.nextPulse;
    advanceTo(rig, alignUp4(sample + 1U));
    return sample;
}

bool construct(Rig& rig, int32_t megabytes, uint64_t firstPulse,
               uint32_t frames) {
    rig.nextPulse = firstPulse;
    rig.interval = kSourceInterval;
    rig.frames = frames;
    rig.clockFault = false;
    return rig.host.instantiate(megabytes);
}

midibuffer::CaptureSnapshot capture(Rig& rig) {
    return midibuffer::captureSnapshot(rig.host.algorithm());
}

midibuffer::MpePlaybackSnapshot mpeState(Rig& rig) {
    midibuffer::MpePlaybackSnapshot state = {};
    midibuffer::mpePlaybackSnapshot(rig.host.algorithm(), state);
    return state;
}

midibuffer::HistorySnapshot historyState(Rig& rig) {
    midibuffer::HistorySnapshot state = {};
    midibuffer::historySnapshot(rig.host.algorithm(), state);
    return state;
}

// Changes the clock interval from the next edge and primes the eight-interval
// tempo window with steady edges.
void retime(Rig& rig, uint32_t interval) {
    pulse(rig);
    rig.interval = interval;
    for (unsigned index = 0; index < 10U; ++index) {
        pulse(rig);
    }
}

// Playback On, then whole clock edges until the selection starts. Returns
// the start pulse sample, or 0 if playback never began.
uint64_t enterPlayback(Rig& rig, bool mpe) {
    setParameter(rig, kPlaybackParameter, 1);
    for (unsigned index = 0; index < 400U; ++index) {
        const uint64_t sample = pulse(rig);
        const bool playing =
            mpe ? mpeState(rig).phase == midibuffer::kMpePhasePlaying
                : capture(rig).playbackActive;
        if (playing) {
            return sample;
        }
    }
    return 0U;
}

// ---------------------------------------------------------------------------
// Oracle expectations and comparison.

struct Expected {
    uint64_t sample;
    uint8_t bytes[3];
    uint8_t size;
};

const size_t kExpectedCapacity = 300000U;
Expected* gExpected = NULL;
size_t gExpectedCount = 0U;

void expectMessage(uint64_t sample, const uint8_t* bytes) {
    if (gExpectedCount < kExpectedCapacity) {
        Expected& item = gExpected[gExpectedCount];
        item.sample = sample;
        std::memcpy(item.bytes, bytes, 3U);
        item.size = messageLength(bytes[0]);
        if (item.size == 2U) {
            item.bytes[2] = 0U;
        }
    }
    ++gExpectedCount;
}

struct Comparison {
    size_t compared;
    size_t disagreements;
    size_t extra;
    size_t missing;
};

// Compares collected calls [first, end) with every expected message. `end`
// is the first call at or after `limit`, so later wrap/cleanup calls are
// excluded only by sample, never by count.
Comparison compareFrom(size_t first, uint64_t limit, uint32_t destination,
                       const char* label) {
    Comparison result = {};
    size_t end = first;
    while (end < gCallCount && gCalls[end].sample < limit) {
        ++end;
    }
    const size_t actual = end - first;
    if (gExpectedCount > kExpectedCapacity) {
        ++result.disagreements;
        return result;
    }
    const size_t count = gExpectedCount < actual ? gExpectedCount : actual;
    for (size_t index = 0; index < count; ++index) {
        const Call& call = gCalls[first + index];
        const Expected& want = gExpected[index];
        const bool same = call.sample == want.sample &&
                          call.size == want.size &&
                          call.destination == destination &&
                          call.bytes[0] == want.bytes[0] &&
                          call.bytes[1] == want.bytes[1] &&
                          (want.size == 2U || call.bytes[2] == want.bytes[2]);
        if (!same) {
            if (result.disagreements == 0U) {
                std::fprintf(stderr,
                             "DISAGREE %s #%zu: got @%llu %02X %02X %02X/%u "
                             "dest %u, want @%llu %02X %02X %02X/%u dest %u\n",
                             label, index,
                             static_cast<unsigned long long>(call.sample),
                             call.bytes[0], call.bytes[1], call.bytes[2],
                             call.size, call.destination,
                             static_cast<unsigned long long>(want.sample),
                             want.bytes[0], want.bytes[1], want.bytes[2],
                             want.size, destination);
            }
            ++result.disagreements;
        }
    }
    result.compared = count;
    if (actual != gExpectedCount) {
        std::fprintf(stderr, "COUNT %s: got %zu want %zu\n", label, actual,
                     gExpectedCount);
        for (size_t index = count; index < actual && index < count + 4U; ++index) {
            const Call& call = gCalls[first + index];
            std::fprintf(stderr, "  extra @%llu %02X %02X %02X\n",
                         static_cast<unsigned long long>(call.sample),
                         call.bytes[0], call.bytes[1], call.bytes[2]);
        }
        for (size_t index = count; index < gExpectedCount && index < count + 4U; ++index) {
            std::fprintf(stderr, "  missing @%llu %02X %02X %02X\n",
                         static_cast<unsigned long long>(gExpected[index].sample),
                         gExpected[index].bytes[0], gExpected[index].bytes[1],
                         gExpected[index].bytes[2]);
        }
    }
    result.extra = actual > gExpectedCount ? actual - gExpectedCount : 0U;
    result.missing = gExpectedCount > actual ? gExpectedCount - actual : 0U;
    return result;
}

bool budgetsHold(const midibuffer::MpeWorkCounters& step) {
    return step.visits <= 32768U && step.sends <= 8192U &&
           step.fanout <= 32768U && step.heap <= 262144U &&
           step.cleanupCalls <= 2304U && step.cleanupFanout <= 9216U;
}

void printCounters(const char* label, const midibuffer::MpeWorkCounters& step) {
    std::printf("QUALIFY: %s max-step visits=%u sends=%u fanout=%u heap=%u "
                "cleanupCalls=%u cleanupFanout=%u\n",
                label, step.visits, step.sends, step.fanout, step.heap,
                step.cleanupCalls, step.cleanupFanout);
}

// ---------------------------------------------------------------------------
// Receiver-state model for the generated starting context.

const uint8_t kTrackedControllers[] = {74U, 1U, 33U, 11U, 43U};
const unsigned kChannelValues = 3U + ARRAY_SIZE(kTrackedControllers);

struct Receiver {
    int value[16][kChannelValues]; // bend14, pressure, then controllers
};

void forget(Receiver& receiver) {
    for (unsigned channel = 0; channel < 16U; ++channel) {
        for (unsigned index = 0; index < kChannelValues; ++index) {
            receiver.value[channel][index] = -1;
        }
    }
}

// Returns the tracked slot a message sets, or -1 when it sets none.
int apply(Receiver& receiver, const uint8_t* bytes) {
    const uint8_t channel = bytes[0] & 0x0fU;
    int slot = -1;
    int value = bytes[1];
    switch (bytes[0] & 0xf0U) {
    case 0xe0U:
        slot = 0;
        value = bytes[1] | (bytes[2] << 7);
        break;
    case 0xd0U:
        slot = 1;
        break;
    case 0xb0U:
        for (unsigned index = 0; index < ARRAY_SIZE(kTrackedControllers);
             ++index) {
            if (bytes[1] == kTrackedControllers[index]) {
                slot = static_cast<int>(2U + index);
                value = bytes[2];
            }
        }
        break;
    default:
        break;
    }
    if (slot >= 0) {
        receiver.value[channel][slot] = value;
    }
    return slot;
}

// ---------------------------------------------------------------------------
// Reference template generator (Spec "Testing Decisions"): eight attacks,
// then repeating 30-slot templates of one round-robin ending, 24 XYZ messages
// for eight member voices, one attack and manager CC1/33/11/43. Endings
// alternate Note Off velocity 45 and zero-velocity On; low bits vary. Event k
// occurs at floor(k x 48000 / R) after the origin (6 samples after the take's
// first edge); host receipt is the 4-sample host boundary at or before that
// sample, exact for R = 3,000 and at most 3 samples early otherwise.

struct Input {
    uint64_t sample;
    uint8_t bytes[3];
};

const size_t kInputCapacity = 270000U;
Input* gInputs = NULL;

struct Template {
    uint32_t count;
    uint32_t fullTemplates;
    uint32_t truncatedSlots;
    uint64_t bytes;
};

uint8_t member(unsigned voice) {
    return static_cast<uint8_t>(1U + voice);
}

Template generate(uint32_t rate, uint32_t count, uint64_t origin) {
    Template shape = {};
    uint8_t notes[8];
    for (unsigned voice = 0; voice < 8U; ++voice) {
        notes[voice] = static_cast<uint8_t>(36U + 8U * voice);
    }
    for (uint32_t k = 0; k < count; ++k) {
        Input& input = gInputs[k];
        input.sample = origin + ((static_cast<uint64_t>(k) * kSampleRate / rate) &
                                 ~static_cast<uint64_t>(3U));
        uint8_t* b = input.bytes;
        if (k < 8U) {
            b[0] = static_cast<uint8_t>(0x90U | member(k));
            b[1] = notes[k];
            b[2] = static_cast<uint8_t>(0x40U + k);
        } else {
            const uint32_t slot = (k - 8U) % 30U;
            const uint32_t cycle = (k - 8U) / 30U;
            const unsigned voice = cycle % 8U;
            const uint8_t channel = member(voice);
            if (slot == 0U) {
                b[0] = static_cast<uint8_t>(((cycle & 1U) ? 0x80U : 0x90U) |
                                            channel);
                b[1] = notes[voice];
                b[2] = (cycle & 1U) ? 45U : 0U;
            } else if (slot <= 24U) {
                const unsigned target = (slot - 1U) / 3U;
                const uint8_t ch = member(target);
                switch ((slot - 1U) % 3U) {
                case 0U:
                    b[0] = static_cast<uint8_t>(0xe0U | ch);
                    b[1] = static_cast<uint8_t>((cycle * 7U + target * 3U) & 0x7fU);
                    b[2] = static_cast<uint8_t>(0x40U + ((cycle + target) & 0x0fU));
                    break;
                case 1U:
                    b[0] = static_cast<uint8_t>(0xd0U | ch);
                    b[1] = static_cast<uint8_t>((cycle * 5U + target) & 0x7fU);
                    b[2] = 0U;
                    break;
                default:
                    b[0] = static_cast<uint8_t>(0xb0U | ch);
                    b[1] = 74U;
                    b[2] = static_cast<uint8_t>((cycle * 3U + target * 11U) & 0x7fU);
                    break;
                }
            } else if (slot == 25U) {
                notes[voice] = static_cast<uint8_t>(
                    36U + 8U * voice + ((notes[voice] - 36U - 8U * voice + 1U) % 8U));
                b[0] = static_cast<uint8_t>(0x90U | channel);
                b[1] = notes[voice];
                b[2] = static_cast<uint8_t>(0x20U + (cycle & 0x3fU));
            } else {
                static const uint8_t kManager[4] = {1U, 33U, 11U, 43U};
                static const uint8_t kStride[4] = {1U, 3U, 5U, 9U};
                b[0] = 0xb0U;
                b[1] = kManager[slot - 26U];
                b[2] = static_cast<uint8_t>((cycle * kStride[slot - 26U]) & 0x7fU);
            }
        }
        shape.bytes += messageLength(b[0]);
    }
    shape.count = count;
    shape.fullTemplates = count > 8U ? (count - 8U) / 30U : 0U;
    shape.truncatedSlots = count > 8U ? (count - 8U) % 30U : 0U;
    return shape;
}

// Observed before the take's first recorded pulse: lower-zone MCM with eight
// members, then every tracked value for the manager and the eight voices.
void sendPreClockState(Rig& rig, Receiver& truth) {
    forget(truth);
    send3(rig, 0xb0U, 101U, 0U);
    send3(rig, 0xb0U, 100U, 6U);
    send3(rig, 0xb0U, 6U, 8U);
    for (uint8_t channel = 0; channel <= 8U; ++channel) {
        const uint8_t messages[][3] = {
            {static_cast<uint8_t>(0xe0U | channel), static_cast<uint8_t>(channel * 9U), 0x40U},
            {static_cast<uint8_t>(0xd0U | channel), static_cast<uint8_t>(0x10U + channel), 0U},
            {static_cast<uint8_t>(0xb0U | channel), 74U, static_cast<uint8_t>(0x30U + channel)},
            {0xb0U, 1U, 0x11U}, {0xb0U, 33U, 0x22U}, {0xb0U, 11U, 0x33U},
            {0xb0U, 43U, 0x44U},
        };
        const unsigned count = channel == 0U ? 7U : 3U;
        for (unsigned index = 0; index < count; ++index) {
            send(rig, messages[index]);
            apply(truth, messages[index]);
        }
    }
}

struct TemplateCase {
    const char* label;
    uint32_t rate;
    uint32_t count;
    int32_t megabytes;
    uint32_t selectedPulses;
    uint32_t frames;
    uint32_t playInterval;
    int16_t destination; // -1 keeps the default destination
    bool earlyPulse;
    bool rolling;        // must overwrite before selection
};

// Lower-zone MCM for eight members: MCM, manager range and member ranges.
size_t expectedSetup(uint8_t* bytes) {
    size_t count = 0U;
    const uint8_t rows[][4] = {{0U, 6U, 8U, 0xffU}, {0U, 0U, 2U, 0U}};
    for (unsigned item = 0; item <= 9U; ++item) {
        const uint8_t* row = item == 0U ? rows[0] : rows[1];
        const uint8_t channel = item <= 1U ? 0U : static_cast<uint8_t>(item - 1U);
        const uint8_t msb = item <= 1U ? row[2] : 48U;
        const uint8_t status = static_cast<uint8_t>(0xb0U | channel);
        const uint8_t sequence[][2] = {{101U, 0U}, {100U, row[1]}, {6U, msb},
                                       {38U, 0U}, {101U, 127U}, {100U, 127U}};
        for (unsigned index = 0; index < 6U; ++index) {
            if (index == 3U && row[3] == 0xffU) {
                continue;
            }
            bytes[count * 3U] = status;
            bytes[count * 3U + 1U] = sequence[index][0];
            bytes[count * 3U + 2U] = sequence[index][1];
            ++count;
        }
    }
    return count;
}

void runTemplateCase(const TemplateCase& spec) {
    Rig rig;
    const char* label = spec.label;
    // Edges at sample 2 mod 4 never coincide with a 4-sample host boundary.
    expect(construct(rig, spec.megabytes, 2U, spec.frames), "template host constructs");
    const uint64_t allocations = midibuffer_test::heapAllocationCount();
    for (unsigned index = 0; index < 9U; ++index) {
        pulse(rig);
    }
    Receiver truth;
    sendPreClockState(rig, truth);
    setParameter(rig, kCaptureParameter, 1);
    const uint64_t takeStart = pulse(rig);
    const uint64_t firstPulse = capture(rig).currentPulse;
    const uint64_t origin = takeStart + 6U;
    const Template shape = generate(spec.rate, spec.count, origin);
    for (uint32_t k = 0; k < spec.count; ++k) {
        advanceTo(rig, gInputs[k].sample);
        send(rig, gInputs[k].bytes);
    }
    const uint64_t duration =
        static_cast<uint64_t>(spec.count) * kSampleRate / spec.rate;
    const uint64_t takePulses = duration / kSourceInterval;
    expect(duration % kSourceInterval == 0U &&
               gInputs[spec.count - 1U].sample < takeStart + duration,
           "template take spans whole source pulses");
    advanceTo(rig, alignUp4(takeStart + duration + 1U) + 4U);
    const midibuffer::HistorySnapshot beforeStop = historyState(rig);
    setParameter(rig, kCaptureParameter, 0);
    const midibuffer::HistorySnapshot afterStop = historyState(rig);
    const midibuffer::CaptureSnapshot stored = capture(rig);
    const uint64_t endings = afterStop.nextOrdinal - beforeStop.nextOrdinal;
    midibuffer::RecordedEvent oldest = {};
    midibuffer::recordedEventAt(rig.host.algorithm(), 0U, oldest);
    const uint64_t endPulse = firstPulse + takePulses;
    const uint64_t startPulse = endPulse - spec.selectedPulses;
    expect(beforeStop.nextOrdinal == spec.count,
           "every reference input is appended without thinning");
    expect(endings <= 224U, "generated capture endings are measured and <= 224");
    expect(!spec.rolling || (afterStop.baseOrdinal > 0U &&
                             afterStop.nextOrdinal > stored.eventCapacity),
           "rolling fixture actually overwrites the ring");
    expect(spec.rolling || afterStop.baseOrdinal == 0U,
           "short fixture keeps the complete take");
    expect(midibuffer::setPulseSelection(rig.host.algorithm(), startPulse, endPulse),
           "final pulse-aligned selection is accepted");
    expect(oldest.pulse < startPulse ||
               (oldest.pulse == startPulse && afterStop.baseOrdinal == 0U),
           "the oldest retained event precedes or starts the final selection");

    // Independent recorded identity, selection membership and context truth.
    uint64_t selectedBytes = 0U;
    uint32_t selectedInputs = 0U;
    uint32_t firstSelected = spec.count;
    struct Recorded {
        uint64_t pulse;
        uint64_t offset;
    };
    for (uint32_t k = 0; k < spec.count; ++k) {
        const uint64_t relative = gInputs[k].sample - takeStart;
        Recorded at = {firstPulse + relative / kSourceInterval,
                       relative % kSourceInterval};
        if (at.offset == 0U) { // received before the coincident edge
            --at.pulse;
            at.offset = kSourceInterval;
        }
        if (at.pulse < startPulse) {
            apply(truth, gInputs[k].bytes);
        } else if (at.pulse < endPulse) {
            if (firstSelected == spec.count) {
                firstSelected = k;
            }
            ++selectedInputs;
            selectedBytes += messageLength(gInputs[k].bytes[0]);
        }
    }

    if (spec.playInterval != kSourceInterval) {
        retime(rig, spec.playInterval);
    }
    if (spec.destination >= 0) {
        setParameter(rig, kPlaybackDestinationParameter, spec.destination);
    }
    setParameter(rig, kMpeModeParameter, 1);
    beginCollection();
    const uint64_t playbackAllocations = midibuffer_test::heapAllocationCount();
    const uint64_t entry = enterPlayback(rig, true);
    expect(entry != 0U, "template selection enters MPE playback");

    // Pulse schedule for one pass, optionally with one early edge halfway.
    const uint32_t played = spec.playInterval;
    const uint32_t earlyIndex = spec.selectedPulses / 2U;
    const uint32_t early = played * 2U / 3U;
    uint64_t starts[400];
    uint64_t predicted[400];
    starts[0] = entry;
    for (uint32_t index = 1; index <= spec.selectedPulses; ++index) {
        const bool isEarly = spec.earlyPulse && index == earlyIndex;
        starts[index] = starts[index - 1U] + (isEarly ? early : played);
    }
    for (uint32_t index = 0; index < spec.selectedPulses; ++index) {
        const bool window = spec.earlyPulse && index >= earlyIndex &&
                            index < earlyIndex + 8U;
        // Eight-interval running average containing one early interval.
        predicted[index] = window ? (7U * played + early) / 8U : played;
    }

    gExpectedCount = 0U;
    for (uint32_t k = firstSelected; k < spec.count; ++k) {
        const uint64_t relative = gInputs[k].sample - takeStart;
        uint64_t pulseIndex = firstPulse + relative / kSourceInterval;
        uint64_t offset = relative % kSourceInterval;
        if (offset == 0U) {
            --pulseIndex;
            offset = kSourceInterval;
        }
        if (pulseIndex >= endPulse) {
            break;
        }
        const uint32_t index = static_cast<uint32_t>(pulseIndex - startPulse);
        uint64_t due = starts[index] +
                       halfUpScale(offset, predicted[index], kSourceInterval);
        if (due > starts[index + 1U]) {
            due = starts[index + 1U]; // early-edge catch-up
        }
        expectMessage(due, gInputs[k].bytes);
    }
    // MPE ownership: an ending sounds only after its emitted in-range attack.
    {
        bool owned[16][128];
        std::memset(owned, 0, sizeof(owned));
        size_t kept = 0U;
        for (size_t index = 0; index < gExpectedCount; ++index) {
            const uint8_t* b = gExpected[index].bytes;
            const uint8_t channel = b[0] & 0x0fU;
            if (isAttack(b)) {
                owned[channel][b[1]] = true;
            } else if (isNoteEnding(b)) {
                if (!owned[channel][b[1]]) {
                    continue;
                }
                owned[channel][b[1]] = false;
            }
            gExpected[kept++] = gExpected[index];
        }
        gExpectedCount = kept;
    }

    for (uint32_t index = 1; index < spec.selectedPulses; ++index) {
        if (spec.earlyPulse && index == earlyIndex) {
            rig.nextPulse = starts[index];
        }
        advanceTo(rig, alignUp4(starts[index] + 1U));
    }
    const uint64_t passEnd = starts[spec.selectedPulses];
    advanceTo(rig, passEnd & ~static_cast<uint64_t>(3U));
    const midibuffer::MpePlaybackSnapshot state = mpeState(rig);
    const uint64_t observedSends = gStepMaximumSends;
    const uint64_t observedFanout = gStepMaximumFanout;
    advanceTo(rig, alignUp4(passEnd + 1U) + 64U);
    finishCollection();
    const uint64_t callbackAllocations =
        midibuffer_test::heapAllocationCount() - playbackAllocations;
    setParameter(rig, kPlaybackParameter, 0);

    // Generated setup and starting context precede the unchanged input.
    uint8_t setup[64 * 3];
    const size_t setupCount = expectedSetup(setup);
    bool setupMatches = state.generatedSetup == setupCount &&
                        gCallCount >= setupCount;
    const uint32_t destination = destinationMask(spec.destination);
    for (size_t index = 0; setupMatches && index < setupCount; ++index) {
        setupMatches = gCalls[index].sample == entry &&
                       gCalls[index].destination == destination &&
                       std::memcmp(gCalls[index].bytes, setup + index * 3U, 3U) == 0;
    }
    // Each known pre-selection value is restored exactly once, nothing else.
    Receiver receiver;
    forget(receiver);
    const size_t contextEnd = setupCount + state.generatedContext;
    bool contextMatches = gCallCount >= contextEnd;
    bool restored[16][kChannelValues];
    std::memset(restored, 0, sizeof(restored));
    for (size_t index = setupCount; contextMatches && index < contextEnd; ++index) {
        const uint8_t channel = gCalls[index].bytes[0] & 0x0fU;
        const int slot = apply(receiver, gCalls[index].bytes);
        contextMatches = gCalls[index].sample == entry &&
                         gCalls[index].destination == destination && slot >= 0 &&
                         !restored[channel][slot];
        if (slot >= 0) {
            restored[channel][slot] = true;
        }
    }
    uint32_t knownValues = 0U;
    for (unsigned channel = 0; channel < 16U; ++channel) {
        for (unsigned index = 0; index < kChannelValues; ++index) {
            knownValues += truth.value[channel][index] >= 0 ? 1U : 0U;
            contextMatches = contextMatches &&
                             receiver.value[channel][index] ==
                                 truth.value[channel][index];
        }
    }
    contextMatches = contextMatches && state.generatedContext == knownValues;
    midibuffer::HistoryContextSnapshot context = {};
    const bool contextRead =
        midibuffer::historyContext(rig.host.algorithm(), false, context);
    const Comparison comparison =
        compareFrom(contextEnd, passEnd, destination, label);

    expect(!rig.clockFault && !gCallOverflow &&
               gExpectedCount <= kExpectedCapacity,
           "simulation and trace collection are complete");
    expect(setupMatches, "generated setup is exactly the observed lower-8 MCM and ranges");
    expect(contextRead && context.ready && context.coverageComplete &&
               !state.contextIncomplete && contextMatches,
           "starting context is valid and restores the true pre-selection state");
    expect(comparison.disagreements == 0U && comparison.extra == 0U &&
               comparison.missing == 0U,
           "replayed pass has zero byte/length/channel/order/sample disagreement");
    expect(state.generatedCleanup == 0U && state.rejectedClusters == 0U,
           "reference pass needs no collision cleanup and rejects no cluster");
    expect(budgetsHold(state.maximumStep), "every per-step budget holds");
    expect(state.maximumStep.sends == observedSends &&
               state.maximumStep.fanout == observedFanout,
           "plug-in send/fanout counters equal the trace-derived per-step maxima");
    expect(spec.destination != 4 ||
               (bitCount(destination) == 4U &&
                state.maximumStep.fanout == 4U * state.maximumStep.sends),
           "four-destination fanout counts every destination bit");
    expect(callbackAllocations == 0U &&
               midibuffer_test::heapAllocationCount() == allocations,
           "capture and playback callbacks perform no heap allocation");

    std::printf("QUALIFY: %s R=%u ev/s N=%u (prefix 8 + %u full 30-event templates "
                "+ %u truncated slots) input=%llu B frames=%u play=%u%s%s\n",
                label, spec.rate, spec.count, shape.fullTemplates,
                shape.truncatedSlots, static_cast<unsigned long long>(shape.bytes),
                spec.frames, played, spec.earlyPulse ? " early-edge" : "",
                spec.destination == 4 ? " fanout=4" : "");
    std::printf("QUALIFY: %s ring %d MB capacity=%u events appended=%llu "
                "overwritten=%llu retained=%u endings=%llu\n",
                label, spec.megabytes, stored.eventCapacity,
                static_cast<unsigned long long>(afterStop.nextOrdinal),
                static_cast<unsigned long long>(afterStop.baseOrdinal),
                stored.eventCount, static_cast<unsigned long long>(endings));
    const double earliest =
        (static_cast<double>(endPulse - oldest.pulse) * kSourceInterval -
         oldest.offsetSamples) / kSampleRate;
    std::printf("QUALIFY: %s selection [%llu,%llu) %u pulses = %.1f s; "
                "%u inputs %llu B; earliest retained point pulse %llu+%u "
                "= %.4f s before selection end; replayed %zu messages after "
                "%u setup + %u context sends; disagreements=%zu\n",
                label, static_cast<unsigned long long>(startPulse),
                static_cast<unsigned long long>(endPulse), spec.selectedPulses,
                spec.selectedPulses * 0.5, selectedInputs,
                static_cast<unsigned long long>(selectedBytes),
                static_cast<unsigned long long>(oldest.pulse),
                oldest.offsetSamples, earliest, comparison.compared,
                state.generatedSetup, state.generatedContext,
                comparison.disagreements);
    printCounters(label, state.maximumStep);
}

// ---------------------------------------------------------------------------
// ac-014 timing: one two-pulse take with offsets chosen to land on exact
// halves, replayed at same/half/double tempo, across 1,000 wraps, and with an
// early edge. Pulses fall on sample 3 mod 4 so host boundaries reach odd
// offsets; a callback one sample after an edge records phase zero.

struct TimedInput {
    uint32_t pulse;  // relative to the selection start
    uint32_t offset; // 0 means immediately after the edge
    uint8_t bytes[3];
};

const TimedInput kTimedTake[] = {
    {0U, 0U, {0x91U, 0x3cU, 0x64U}},
    {0U, 6001U, {0xb1U, 74U, 0x01U}},
    {0U, 12001U, {0xd1U, 0x20U, 0U}},
    {0U, 18001U, {0xe1U, 0x05U, 0x40U}},
    {0U, 23997U, {0x81U, 0x3cU, 0x2dU}},
    {1U, 0U, {0x92U, 0x40U, 0x50U}},
    {1U, 5U, {0xb2U, 74U, 0x02U}},
    {1U, 23997U, {0x92U, 0x40U, 0x00U}},
    // Recorded at the excluded end pulse: never part of [start, end).
    {2U, 0U, {0x93U, 0x41U, 0x60U}},
    {2U, 5U, {0x94U, 0x42U, 0x61U}},
    {2U, 9U, {0x83U, 0x41U, 0x00U}},
    {2U, 13U, {0x84U, 0x42U, 0x00U}},
};

struct TimingCase {
    const char* label;
    bool mpe;
    uint32_t playInterval;
    uint32_t passes;
    bool earlyPulse; // pass 1, second pulse arrives at 2/3 interval
};

void runTimingCase(const TimingCase& spec) {
    Rig rig;
    expect(construct(rig, 1, 3U, 64U), "timing host constructs");
    const uint64_t allocations = midibuffer_test::heapAllocationCount();
    for (unsigned index = 0; index < 9U; ++index) {
        pulse(rig);
    }
    if (spec.mpe) {
        send3(rig, 0xb0U, 101U, 0U);
        send3(rig, 0xb0U, 100U, 6U);
        send3(rig, 0xb0U, 6U, 15U);
    }
    setParameter(rig, kCaptureParameter, 1);
    uint64_t starts[3] = {};
    uint64_t startPulse = 0U;
    for (uint32_t index = 0; index < ARRAY_SIZE(kTimedTake); ++index) {
        const TimedInput& input = kTimedTake[index];
        while (input.pulse >= 1U && starts[input.pulse] == 0U) {
            starts[input.pulse] = rig.nextPulse;
            pulse(rig);
        }
        if (input.pulse == 0U && starts[0] == 0U) {
            starts[0] = pulse(rig);
            startPulse = capture(rig).currentPulse;
        }
        advanceTo(rig, starts[input.pulse] +
                           (input.offset == 0U ? 1U : input.offset));
        send(rig, input.bytes);
    }
    pulse(rig);
    setParameter(rig, kCaptureParameter, 0);
    expect(midibuffer::setPulseSelection(rig.host.algorithm(), startPulse,
                                         startPulse + 2U),
           "timing selection is accepted");
    retime(rig, spec.playInterval);
    if (spec.mpe) {
        setParameter(rig, kMpeModeParameter, 1);
    }
    beginCollection();
    const uint64_t entry = enterPlayback(rig, spec.mpe);
    expect(entry != 0U, "timing selection enters playback");

    const uint32_t pulses = spec.passes * 2U;
    const uint32_t earlyIndex = 3U;
    const uint32_t early = spec.playInterval * 2U / 3U;
    gExpectedCount = 0U;
    uint64_t start = entry;
    for (uint32_t index = 0; index < pulses; ++index) {
        const bool window = spec.earlyPulse && index >= earlyIndex &&
                            index < earlyIndex + 8U;
        const uint64_t predicted =
            window ? (7U * spec.playInterval + early) / 8U : spec.playInterval;
        const uint64_t next =
            start + (spec.earlyPulse && index + 1U == earlyIndex
                         ? early : spec.playInterval);
        for (uint32_t input = 0; input < ARRAY_SIZE(kTimedTake); ++input) {
            const TimedInput& item = kTimedTake[input];
            if (item.pulse != index % 2U) {
                continue;
            }
            uint64_t due = start + halfUpScale(item.offset, predicted,
                                               kSourceInterval);
            if (due > next) {
                due = next;
            }
            expectMessage(due, item.bytes);
        }
        if (index + 1U < pulses) {
            if (spec.earlyPulse && index + 1U == earlyIndex) {
                rig.nextPulse = next;
            }
            advanceTo(rig, alignUp4(next + 1U));
        }
        start = next;
    }
    const uint64_t end = start;
    advanceTo(rig, alignUp4(end + 1U));
    finishCollection();
    setParameter(rig, kPlaybackParameter, 0);

    const midibuffer::MpePlaybackSnapshot state = mpeState(rig);
    const size_t first = spec.mpe ? state.generatedSetup + state.generatedContext : 0U;
    const Comparison comparison =
        compareFrom(first, end, destinationMask(-1), spec.label);
    bool excluded = true;
    for (size_t index = 0; index < gCallCount; ++index) {
        const uint8_t status = gCalls[index].bytes[0];
        const bool note = (status & 0xe0U) == 0x80U;
        excluded = excluded &&
                   !(note && ((status & 0x0fU) == 3U || (status & 0x0fU) == 4U));
    }
    expect(!rig.clockFault && !gCallOverflow, "timing simulation and trace are complete");
    expect(comparison.disagreements == 0U && comparison.extra == 0U &&
               comparison.missing == 0U,
           "timing replay has zero byte/length/channel/order/sample disagreement");
    expect(excluded, "attacks recorded at the half-open end pulse never play");
    expect(midibuffer_test::heapAllocationCount() == allocations,
           "timing callbacks perform no heap allocation");
    std::printf("QUALIFY: %s %s play=%u passes=%u wraps=%u%s compared=%zu "
                "disagreements=%zu final-pass-start=%llu\n",
                spec.label, spec.mpe ? "MPE" : "ordinary", spec.playInterval,
                spec.passes, spec.passes - 1U,
                spec.earlyPulse ? " early-edge" : "", comparison.compared,
                comparison.disagreements,
                static_cast<unsigned long long>(end - 2U * spec.playInterval));
}

// ---------------------------------------------------------------------------
// ac-014 same-sample cluster/collision/fanout/frame matrix.

void clusterMessage(uint32_t index, uint8_t* b) {
    const uint8_t channel = static_cast<uint8_t>(1U + index % 15U);
    switch ((index / 15U) % 3U) {
    case 0U:
        b[0] = static_cast<uint8_t>(0xe0U | channel);
        b[1] = static_cast<uint8_t>(index & 0x7fU);
        b[2] = static_cast<uint8_t>((index >> 7) & 0x7fU);
        break;
    case 1U:
        b[0] = static_cast<uint8_t>(0xd0U | channel);
        b[1] = static_cast<uint8_t>(index & 0x7fU);
        b[2] = 0U;
        break;
    default:
        b[0] = static_cast<uint8_t>(0xb0U | channel);
        b[1] = 74U;
        b[2] = static_cast<uint8_t>(index & 0x7fU);
        break;
    }
}

struct ClusterCase {
    uint32_t count;
    bool collisions; // count attacks retriggering count/2 keys
    int16_t destination;
    uint32_t frames;
    uint32_t playInterval;
    bool fullRing;   // overwrite the 1 MB ring before the cluster take
};

void runClusterCase(const ClusterCase& spec) {
    Rig rig;
    char label[96];
    std::snprintf(label, sizeof(label), "cluster %u %s dest=%d frames=%u play=%u%s",
                  spec.count, spec.collisions ? "attacks" : "expressions",
                  spec.destination, spec.frames, spec.playInterval,
                  spec.fullRing ? " full-ring" : "");
    expect(construct(rig, 1, 0U, spec.frames), "cluster host constructs");
    for (unsigned index = 0; index < 9U; ++index) {
        pulse(rig);
    }
    send3(rig, 0xb0U, 101U, 0U);
    send3(rig, 0xb0U, 100U, 6U);
    send3(rig, 0xb0U, 6U, 15U);
    setParameter(rig, kCaptureParameter, 1);
    if (spec.fullRing) {
        // 45,000 untracked manager CC7 messages, one per host boundary.
        const uint64_t base = alignUp4(rig.nextPulse + 1U);
        for (uint32_t index = 0; index < 45000U; ++index) {
            advanceTo(rig, base + 4U * index);
            send3(rig, 0xb0U, 7U, static_cast<uint8_t>(index & 0x7fU));
        }
    }
    const uint64_t take = pulse(rig);
    const uint64_t startPulse = capture(rig).currentPulse;
    const uint8_t attack[3] = {0x9fU, 0x3cU, 0x64U};
    const uint8_t ending[3] = {0x8fU, 0x3cU, 0x2dU};
    advanceTo(rig, take + 8U);
    send(rig, attack);
    advanceTo(rig, take + 16U);
    uint8_t message[3];
    for (uint32_t index = 0; index < spec.count; ++index) {
        if (spec.collisions) {
            const uint32_t key = index % (spec.count / 2U);
            message[0] = static_cast<uint8_t>(0x90U | (1U + key % 8U));
            message[1] = static_cast<uint8_t>(0x30U + key / 8U);
            message[2] = static_cast<uint8_t>(0x40U + index / (spec.count / 2U));
        } else {
            clusterMessage(index, message);
        }
        send(rig, message);
    }
    advanceTo(rig, take + kSourceInterval + 8U);
    send(rig, ending);
    pulse(rig);
    pulse(rig);
    setParameter(rig, kCaptureParameter, 0);
    const midibuffer::HistorySnapshot stored = historyState(rig);
    expect(!spec.fullRing || stored.baseOrdinal > 0U,
           "full-ring cluster take follows actual overwrite");
    expect(midibuffer::setPulseSelection(rig.host.algorithm(), startPulse,
                                         startPulse + 2U),
           "cluster selection is accepted");
    if (spec.playInterval != kSourceInterval) {
        retime(rig, spec.playInterval);
    }
    if (spec.destination >= 0) {
        setParameter(rig, kPlaybackDestinationParameter, spec.destination);
    }
    setParameter(rig, kMpeModeParameter, 1);
    beginCollection();
    const uint64_t allocations = midibuffer_test::heapAllocationCount();
    const uint64_t entry = enterPlayback(rig, true);
    expect(entry != 0U, "cluster selection enters MPE playback");
    const uint32_t played = spec.playInterval;
    const uint64_t attackAt = entry + halfUpScale(8U, played, kSourceInterval);
    const uint64_t clusterAt = entry + halfUpScale(16U, played, kSourceInterval);
    const uint64_t endingAt = entry + played + halfUpScale(8U, played, kSourceInterval);
    advanceTo(rig, alignUp4(endingAt + 1U) + 16U);
    const midibuffer::MpePlaybackSnapshot state = mpeState(rig);
    const uint64_t observedSends = gStepMaximumSends;
    const uint64_t observedFanout = gStepMaximumFanout;
    finishCollection();
    const bool reject = spec.count > 8192U;

    gExpectedCount = 0U;
    expectMessage(attackAt, attack);
    if (!reject) {
        for (uint32_t index = 0; index < spec.count; ++index) {
            if (spec.collisions) {
                const uint32_t key = index % (spec.count / 2U);
                message[0] = static_cast<uint8_t>(0x90U | (1U + key % 8U));
                message[1] = static_cast<uint8_t>(0x30U + key / 8U);
                message[2] = static_cast<uint8_t>(0x40U + index / (spec.count / 2U));
                if (index >= spec.count / 2U) {
                    const uint8_t cut[3] = {static_cast<uint8_t>(message[0] & 0x8fU),
                                            message[1], 0U};
                    expectMessage(clusterAt, cut); // generated collision ending
                }
            } else {
                clusterMessage(index, message);
            }
            expectMessage(clusterAt, message);
        }
        expectMessage(endingAt, ending);
    } else {
        const uint8_t cleanup[3] = {0x8fU, 0x3cU, 0x00U};
        expectMessage(clusterAt, cleanup); // reserve cleanup of prior ownership
    }
    const size_t first = state.generatedSetup + state.generatedContext;
    const uint64_t limit = spec.collisions && !reject ? clusterAt + 1U : endingAt + 1U;
    if (spec.collisions && !reject) {
        --gExpectedCount; // the recorded ending is outside this window
    }
    const uint32_t destination = destinationMask(spec.destination);
    bool generatedRouted = gCallCount >= first;
    for (size_t index = 0; generatedRouted && index < first; ++index) {
        generatedRouted = gCalls[index].destination == destination &&
                          gCalls[index].sample == entry;
    }
    const Comparison comparison = compareFrom(first, limit, destination, label);
    expect(!rig.clockFault && !gCallOverflow, "cluster simulation and trace are complete");
    expect(comparison.disagreements == 0U && comparison.extra == 0U &&
               comparison.missing == 0U,
           "cluster replay has zero byte/length/channel/order/sample disagreement");
    expect(budgetsHold(state.maximumStep), "cluster per-step budgets hold");
    expect(generatedRouted, "cluster setup/context use the selected destination at entry");
    // Admitting or rejecting a same-sample cluster must inspect every member.
    expect(state.maximumStep.visits >= spec.count,
           "visit counter covers every message of the admitted or rejected cluster");
    expect(state.maximumStep.sends + state.maximumStep.cleanupCalls >= observedSends &&
               state.maximumStep.sends <= observedSends &&
               state.maximumStep.fanout + state.maximumStep.cleanupFanout >= observedFanout &&
               state.maximumStep.fanout <= observedFanout &&
               (reject || (state.maximumStep.sends == observedSends &&
                           state.maximumStep.fanout == observedFanout)),
           "plug-in send/fanout counters agree with the trace-derived per-step maxima");
    expect(spec.destination != 4 || bitCount(destination) == 4U,
           "cluster fanout uses all four destinations");
    expect(reject == (state.rejectedClusters == 1U) &&
               (!reject || (state.phase == midibuffer::kMpePhaseFault &&
                            rig.host.parameter(kPlaybackParameter) == 0)),
           "only the 8,193-message cluster is rejected and latches Off");
    expect(!spec.collisions || state.generatedCleanup == spec.count / 2U,
           "same-sample retriggers emit counted collision endings");
    expect(midibuffer_test::heapAllocationCount() == allocations,
           "cluster callbacks perform no heap allocation");
    std::printf("QUALIFY: %s %s compared=%zu disagreements=%zu rejected=%u "
                "cleanup=%u\n",
                label, reject ? "REJECTED" : "admitted", comparison.compared,
                comparison.disagreements, state.rejectedClusters,
                state.generatedCleanup);
    printCounters(label, state.maximumStep);
}

} // namespace

int main() {
    gCalls = static_cast<Call*>(std::malloc(kCallCapacity * sizeof(Call)));
    gExpected = static_cast<Expected*>(std::malloc(kExpectedCapacity * sizeof(Expected)));
    gInputs = static_cast<Input*>(std::malloc(kInputCapacity * sizeof(Input)));
    if (gCalls == NULL || gExpected == NULL || gInputs == NULL) {
        std::fprintf(stderr, "qualification buffers unavailable\n");
        return 1;
    }
    std::printf("QUALIFY: steady template payload = 82 B per 30 events "
                "(1 ending 3 B + 8x(bend 3 + pressure 2 + CC74 3) + attack 3 "
                "+ 4 manager CC x 3): 3,000 ev/s = 8,200 B/s, 4,500 = 12,300 B/s, "
                "10,000 = 27,333.33 B/s (native/virtual, not DIN 3,125 B/s)\n");
    std::printf("QUALIFY: assumptions: simulated 48 kHz, 24,000-sample source clock; "
                "MIDI callbacks occur between host steps, so receipt is the 4-sample "
                "host boundary at or before floor(k x 48000 / R) after an origin 6 "
                "samples past the take's first edge (exact at 3,000 ev/s, <= 3 "
                "samples early at 4,500/10,000); template edges sit at sample 2 mod 4; "
                "operation counts are algorithmic, not hardware deadlines\n");

    // ac-014 timing oracle cases in both playback modes.
    for (unsigned mode = 0; mode < 2U; ++mode) {
        const bool mpe = mode == 1U;
        const TimingCase cases[] = {
            {"timing same tempo", mpe, 24000U, 3U, false},
            {"timing half tempo", mpe, 48000U, 3U, false},
            {"timing double tempo sample3001 1000-wrap", mpe, 12000U, 1001U, false},
            {"timing early catch-up", mpe, 24000U, 3U, true},
        };
        for (size_t index = 0; index < ARRAY_SIZE(cases); ++index) {
            runTimingCase(cases[index]);
        }
    }

    // ac-013 retention fixtures (3,000 ev/s; final 10/60 s at 1/5 MB).
    const TemplateCase retention[] = {
        {"retention short 1MB", 3000U, 30000U, 1, 20U, 16U, 24000U, -1, false, false},
        {"retention short 5MB", 3000U, 180000U, 5, 120U, 16U, 24000U, -1, false, false},
        {"retention rolling 1MB", 3000U, 90000U, 1, 20U, 16U, 24000U, -1, false, true},
        {"retention rolling 5MB", 3000U, 270000U, 5, 120U, 16U, 24000U, -1, false, true},
    };
    for (size_t index = 0; index < ARRAY_SIZE(retention); ++index) {
        runTemplateCase(retention[index]);
    }

    // ac-014 dense/frame/tempo/fanout/catch-up matrix over full 1 MB rings.
    const TemplateCase dense[] = {
        {"dense 4500 frames4", 4500U, 45000U, 1, 16U, 4U, 24000U, -1, false, true},
        {"dense 4500 frames16", 4500U, 45000U, 1, 16U, 16U, 24000U, -1, false, true},
        {"dense 4500 frames64", 4500U, 45000U, 1, 16U, 64U, 24000U, -1, false, true},
        {"dense 4500 double-tempo", 4500U, 45000U, 1, 16U, 64U, 12000U, -1, false, true},
        {"dense 4500 half-tempo early-edge", 4500U, 45000U, 1, 16U, 64U, 48000U, -1, true, true},
        {"dense 10000 frames4", 10000U, 60000U, 1, 8U, 4U, 24000U, -1, false, true},
        {"dense 10000 frames16", 10000U, 60000U, 1, 8U, 16U, 24000U, -1, false, true},
        {"dense 10000 frames64 fanout", 10000U, 60000U, 1, 8U, 64U, 24000U, 4, false, true},
        {"dense 10000 double-tempo", 10000U, 60000U, 1, 8U, 64U, 12000U, -1, false, true},
        {"dense 10000 early-edge", 10000U, 60000U, 1, 8U, 64U, 24000U, 4, true, true},
    };
    for (size_t index = 0; index < ARRAY_SIZE(dense); ++index) {
        runTemplateCase(dense[index]);
    }

    const uint32_t clusterSizes[] = {0U, 1U, 32U, 256U, 2250U, 5000U, 8193U};
    for (size_t index = 0; index < ARRAY_SIZE(clusterSizes); ++index) {
        const ClusterCase single = {clusterSizes[index], false, -1, 64U, 24000U, false};
        const ClusterCase fanout = {clusterSizes[index], false, 4, 4U, 24000U, false};
        runClusterCase(single);
        runClusterCase(fanout);
    }
    const ClusterCase combined[] = {
        {32U, true, -1, 64U, 24000U, false},
        {256U, true, 4, 4U, 24000U, false},
        // Bursts after real overwrite, at double tempo, with four-way fanout.
        {5000U, false, 4, 16U, 12000U, true},
        {8193U, false, 4, 16U, 12000U, true},
        {256U, true, 4, 64U, 48000U, true},
    };
    for (size_t index = 0; index < ARRAY_SIZE(combined); ++index) {
        runClusterCase(combined[index]);
    }

    std::free(gInputs);
    std::free(gExpected);
    std::free(gCalls);
    if (gFailures != 0) {
        std::fprintf(stderr, "%d qualification check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("PASS: exact-candidate timing, retention, density, cluster and "
                "frame qualification against independent oracles\n");
    return 0;
}
