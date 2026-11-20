// Test-only bridge: renders one exact MIDI log through installed Vital 1.5.5
// (req-014, ac-015). Each message travels a self-created virtual CoreMIDI
// source -> private input port -> private output port -> private destination
// and is logged as received before MusicDeviceMIDIEvent schedules it at its
// exact sample inside an offline AudioUnitRender block. No physical MIDI or
// audio device is opened.
//
// Usage: vital_receiver_render <state.plist> <readback.plist> <log> <received>
//                              <render.f32> <frames> [mpeParameter]
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void check(OSStatus result, const char* operation) {
    if (result != noErr) { std::fprintf(stderr, "%s failed: %d\n", operation, (int)result); std::exit(1); }
}
struct Inbox { dispatch_semaphore_t ready; Byte bytes[3]; UInt16 length; };
static void receive(const MIDIPacketList* list, void* context, void*) {
    Inbox* inbox = static_cast<Inbox*>(context);
    const MIDIPacket* packet = &list->packet[0];
    if (list->numPackets == 1 && packet->length <= sizeof(inbox->bytes)) {
        inbox->length = packet->length;
        std::memcpy(inbox->bytes, packet->data, packet->length);
        dispatch_semaphore_signal(inbox->ready);
    }
}
static void waitFor(Inbox& inbox, const Byte* expected, UInt16 length) {
    if (dispatch_semaphore_wait(inbox.ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
        inbox.length != length || std::memcmp(inbox.bytes, expected, length)) {
        std::fprintf(stderr, "virtual MIDI delivery mismatch or timeout\n"); std::exit(1);
    }
}
static MIDIPacketList* packet(Byte (&storage)[256], const Byte* bytes, UInt16 length) {
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket* first = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof(storage), first, 0, length, bytes)) std::abort();
    return list;
}
static CFPropertyListRef readPlist(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return nullptr;
    std::vector<UInt8> bytes;
    for (int c; (c = std::fgetc(file)) != EOF;) bytes.push_back(static_cast<UInt8>(c));
    std::fclose(file);
    CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
    CFPropertyListRef list = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    return list;
}
struct Event { unsigned long long sample; Byte bytes[3]; UInt16 length; };

int main(int argc, char** argv) {
    if (argc != 7 && argc != 8) { std::fprintf(stderr, "usage: see source header\n"); return 2; }
    const long frames = std::atol(argv[6]);
    std::vector<Event> events;
    {
        FILE* log = std::fopen(argv[3], "r");
        if (!log) { std::perror(argv[3]); return 1; }
        char line[256];
        while (std::fgets(line, sizeof(line), log)) {
            if (line[0] == '#') continue;
            Event event = {}; unsigned destination = 0, b[3] = {};
            const int fields = std::sscanf(line, "%llu %u %x %x %x", &event.sample, &destination, &b[0], &b[1], &b[2]);
            if (fields < 4) continue;
            event.length = static_cast<UInt16>(fields - 2);
            for (int i = 0; i < 3; ++i) event.bytes[i] = static_cast<Byte>(b[i]);
            if (!events.empty() && event.sample < events.back().sample) { std::fprintf(stderr, "unordered log\n"); return 1; }
            events.push_back(event);
        }
        std::fclose(log);
    }

    AudioComponentDescription description = {kAudioUnitType_MusicDevice, 'Vita', 'Tyte', 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component) { std::fprintf(stderr, "Vital AudioUnit unavailable\n"); return 1; }
    UInt32 version = 0; check(AudioComponentGetVersion(component, &version), "AudioComponentGetVersion");
    if (version != 66821) { std::fprintf(stderr, "Expected Vital 1.5.5, got %u\n", (unsigned)version); return 1; }
    AudioUnit unit = nullptr;
    check(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew");
    AudioStreamBasicDescription format = {};
    format.mSampleRate = 48000; format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = format.mBytesPerFrame = sizeof(float);
    format.mFramesPerPacket = 1; format.mChannelsPerFrame = 2; format.mBitsPerChannel = 32;
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format)), "stream format");
    check(AudioUnitInitialize(unit), "AudioUnitInitialize");
    CFPropertyListRef fixture = readPlist(argv[1]);
    if (!fixture) { std::fprintf(stderr, "invalid state fixture\n"); return 1; }
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &fixture, sizeof(fixture)), "load state");
    CFRelease(fixture);
    // Optional probe: Vital's "MPE Enabled" AU parameter (ID 52567).
    if (argc == 8) check(AudioUnitSetParameter(unit, 52567, kAudioUnitScope_Global, 0, std::atof(argv[7]), 0), "MPE parameter");
    CFPropertyListRef state = nullptr; UInt32 stateSize = sizeof(state);
    check(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, &stateSize), "capture state");
    CFDataRef data = state ? CFPropertyListCreateData(nullptr, state, kCFPropertyListBinaryFormat_v1_0, 0, nullptr) : nullptr;
    FILE* readback = data ? std::fopen(argv[2], "wb") : nullptr;
    if (!readback || std::fwrite(CFDataGetBytePtr(data), 1, CFDataGetLength(data), readback) != (size_t)CFDataGetLength(data) || std::fclose(readback)) {
        std::fprintf(stderr, "state readback failed\n"); return 1;
    }
    CFRelease(data); CFRelease(state);

    MIDIClientRef client = 0; MIDIPortRef input = 0, output = 0; MIDIEndpointRef source = 0, destination = 0;
    Inbox fromSource = {dispatch_semaphore_create(0), {}, 0}, fromDestination = {dispatch_semaphore_create(0), {}, 0};
    check(MIDIClientCreate(CFSTR("MidiBuffer receiver isolated"), nullptr, nullptr, &client), "MIDIClientCreate");
    check(MIDISourceCreate(client, CFSTR("MidiBuffer receiver source"), &source), "MIDISourceCreate");
    check(MIDIDestinationCreate(client, CFSTR("MidiBuffer receiver destination"), receive, &fromDestination, &destination), "MIDIDestinationCreate");
    check(MIDIInputPortCreate(client, CFSTR("MidiBuffer receiver input"), receive, &fromSource, &input), "MIDIInputPortCreate");
    check(MIDIOutputPortCreate(client, CFSTR("MidiBuffer receiver output"), &output), "MIDIOutputPortCreate");
    check(MIDIPortConnectSource(input, source, nullptr), "MIDIPortConnectSource");

    FILE* received = std::fopen(argv[4], "w");
    FILE* render = std::fopen(argv[5], "wb");
    if (!received || !render) { std::fprintf(stderr, "open outputs failed\n"); return 1; }
    const int kBlock = 64;
    float left[kBlock], right[kBlock], interleaved[2 * kBlock];
    struct { UInt32 count; AudioBuffer buffers[2]; } audio = {2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
    AudioTimeStamp timestamp = {}; timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    alignas(MIDIPacketList) Byte storage[256];
    size_t next = 0;
    for (long base = 0; base < frames; base += kBlock) {
        for (; next < events.size() && events[next].sample < (unsigned long long)(base + kBlock); ++next) {
            const Event& event = events[next];
            check(MIDIReceived(source, packet(storage, event.bytes, event.length)), "MIDIReceived");
            waitFor(fromSource, event.bytes, event.length);
            check(MIDISend(output, destination, packet(storage, fromSource.bytes, fromSource.length)), "MIDISend");
            waitFor(fromDestination, event.bytes, event.length);
            std::fprintf(received, "%llu", event.sample);
            for (UInt16 i = 0; i < fromDestination.length; ++i) std::fprintf(received, " %02x", fromDestination.bytes[i]);
            std::fprintf(received, "\n");
            check(MusicDeviceMIDIEvent(unit, fromDestination.bytes[0], fromDestination.bytes[1],
                                       fromDestination.length > 2 ? fromDestination.bytes[2] : 0,
                                       static_cast<UInt32>(event.sample - base)), "MusicDeviceMIDIEvent");
        }
        AudioUnitRenderActionFlags flags = 0; timestamp.mSampleTime = base;
        check(AudioUnitRender(unit, &flags, &timestamp, 0, kBlock, reinterpret_cast<AudioBufferList*>(&audio)), "AudioUnitRender");
        for (int i = 0; i < kBlock; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) { std::fprintf(stderr, "non-finite render\n"); return 1; }
            interleaved[2 * i] = left[i]; interleaved[2 * i + 1] = right[i];
        }
        if (std::fwrite(interleaved, sizeof(float), 2 * kBlock, render) != 2 * kBlock) { std::fprintf(stderr, "write render failed\n"); return 1; }
    }
    if (next != events.size()) { std::fprintf(stderr, "events beyond render length\n"); return 1; }
    std::fclose(received); std::fclose(render);
    MIDIPortDisconnectSource(input, source); MIDIPortDispose(input); MIDIPortDispose(output);
    MIDIEndpointDispose(source); MIDIEndpointDispose(destination); MIDIClientDispose(client);
    AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit);
    std::printf("RENDER %s: %zu events via private virtual endpoints, %ld frames @48 kHz\n", argv[3], events.size(), frames);
    return 0;
}
