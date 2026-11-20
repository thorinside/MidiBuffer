// Reuse the existing host-double receiver rig, not a second looper.
#define main original_receiver_fixture_main
#include "../../tests/receiver_fixture.cpp"
#undef main

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Rig rig;
    rig.nextPulse = 3U;
    if (!rig.host.instantiate(1)) return 1;
    for (unsigned i = 0; i < 9; ++i) pulse(rig);
    setParameter(rig, 13, 1); // Manual, two lower members, unknown ranges
    setParameter(rig, 14, 2);
    setParameter(rig, 12, 1);
    setParameter(rig, 2, 1);
    const uint64_t start = pulse(rig);
    const uint64_t first = midibuffer::captureSnapshot(rig.host.algorithm()).currentPulse;
    // time in 1/100 seconds; same explicit MIDI1 bytes as kit.phrase().
    const unsigned rows[][4] = {
        {0,0xe1,0,72},{5,0xd1,20,0},{10,0xb1,74,32},
        {15,0xe2,0,56},{20,0xd2,40,0},{25,0xb2,74,96},
        {50,0x91,57,100},{75,0x92,64,90},
        {100,0xe1,0,80},{110,0xe2,0,48},{120,0xd1,100,0},
        {130,0xd2,60,0},{140,0xb1,74,10},{150,0xb2,74,120},
        {200,0x81,57,64},{225,0x82,64,32},
        {240,0xe1,0,64},{245,0xe2,0,64},{250,0xd1,0,0},{255,0xd2,0,0},{260,0xb1,74,64},{265,0xb2,74,64}
    };
    for (const auto& row : rows) {
        advanceTo(rig, start + row[0] * 480U + 1U);
        send(rig, row[1], row[2], row[3]);
    }
    advanceTo(rig, start + 132000U); // finalize at 2.75 seconds
    setParameter(rig, 2, 0);
    if (!midibuffer::setPulseSelection(rig.host.algorithm(), first, first + 6)) return 1;
    for (unsigned i = 0; i < 10; ++i) pulse(rig);
    midibuffer_test::resetTrace();
    midibuffer_test::setMidiTraceDrain(drain);
    setParameter(rig, 10, 1);
    uint64_t passStart = 0;
    midibuffer::MpePlaybackSnapshot state = {};
    for (unsigned i = 0; i < 40 && !passStart; ++i) {
        uint64_t at = pulse(rig);
        midibuffer::mpePlaybackSnapshot(rig.host.algorithm(), state);
        if (state.phase == midibuffer::kMpePhasePlaying) passStart = at;
    }
    if (!passStart) return 1;
    advanceTo(rig, passStart + 134400U); // 2.8 s, before wrap
    setParameter(rig, 10, 0);
    advanceTo(rig, passStart + 138000U);
    drain(midibuffer_test::trace().midiCalls, midibuffer_test::trace().midiCallCount);
    midibuffer_test::setMidiTraceDrain(NULL);
    midibuffer::mpePlaybackSnapshot(rig.host.algorithm(), state);
    const std::vector<Message>* streams[] = {&rig.direct, &gReplay};
    const uint64_t origins[] = {start - kLead, passStart - kLead};
    for (unsigned j = 0; j < 2; ++j) {
        FILE* f = std::fopen(argv[j + 1], "w");
        if (!f) return 1;
        std::fprintf(f, "# sampleRate 48000\n# start 48000\n# passPulses 6\n");
        for (const Message& m : *streams[j]) {
            std::fprintf(f, "%llu %u %02x %02x", (unsigned long long)(m.sample - origins[j]),
                         m.destination, m.bytes[0], m.bytes[1]);
            if (m.size == 3) std::fprintf(f, " %02x", m.bytes[2]);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }
    std::printf("KIT production fixture direct=%zu replay=%zu setup=%u context=%u cleanup=%u owned=%u/%u\n",
                rig.direct.size(), gReplay.size(), state.generatedSetup, state.generatedContext,
                state.generatedCleanup, state.ownedNotes, state.ownedHolds);
    return state.ownedNotes || state.ownedHolds;
}
