// Test-only, virtual MIDI to offline AudioUnit receiver bridge.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
static void waitFor(Inbox& inbox, const Byte* expected) {
    if (dispatch_semaphore_wait(inbox.ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
        inbox.length != 3 || std::memcmp(inbox.bytes, expected, 3)) {
        std::fprintf(stderr, "virtual MIDI delivery mismatch or timeout\n"); std::exit(1);
    }
}
static MIDIPacketList* packet(Byte (&storage)[256], const Byte* bytes) {
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket* first = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof(storage), first, 0, 3, bytes)) std::abort();
    return list;
}
int main() {
    AudioComponentDescription description = {kAudioUnitType_MusicDevice, 'Vita', 'Tyte', 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component) { std::fprintf(stderr, "Vital AudioUnit unavailable\n"); return 1; }
    UInt32 version = 0; check(AudioComponentGetVersion(component, &version), "AudioComponentGetVersion");
    if (version != 66821) { std::fprintf(stderr, "Expected Vital 1.5.5, got %u\n", (unsigned)version); return 1; }
    AudioUnit unit = nullptr;
    check(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew");
    AudioStreamBasicDescription format = {};
    format.mSampleRate = 44100; format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = format.mBytesPerFrame = sizeof(float);
    format.mFramesPerPacket = 1; format.mChannelsPerFrame = 2; format.mBitsPerChannel = 32;
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format)), "stream format");
    check(AudioUnitInitialize(unit), "AudioUnitInitialize");

    // Restore the checked-in class state before recording the receiver's readback.
    FILE* fixture = std::fopen("tests/vital-smoke-state.plist", "rb");
    if (!fixture || std::fseek(fixture, 0, SEEK_END)) { std::fprintf(stderr, "open state fixture failed\n"); return 1; }
    long length = std::ftell(fixture);
    if (length <= 0 || std::fseek(fixture, 0, SEEK_SET)) return 1;
    UInt8* bytes = static_cast<UInt8*>(std::malloc(static_cast<size_t>(length)));
    if (!bytes || std::fread(bytes, 1, static_cast<size_t>(length), fixture) != static_cast<size_t>(length) || std::fclose(fixture)) return 1;
    CFDataRef fixtureData = CFDataCreate(nullptr, bytes, length); std::free(bytes);
    CFErrorRef error = nullptr;
    CFPropertyListRef fixtureState = CFPropertyListCreateWithData(nullptr, fixtureData, kCFPropertyListImmutable, nullptr, &error);
    CFRelease(fixtureData);
    if (!fixtureState) { std::fprintf(stderr, "invalid state fixture\n"); return 1; }
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &fixtureState, sizeof(fixtureState)), "load fixture state");
    CFRelease(fixtureState);
    CFPropertyListRef state = nullptr; UInt32 stateSize = sizeof(state);
    check(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, &stateSize), "capture AU state");
    if (!state) { std::fprintf(stderr, "empty AU state\n"); return 1; }
    CFDataRef data = CFPropertyListCreateData(nullptr, state, kCFPropertyListBinaryFormat_v1_0, 0, &error);
    if (!data) { std::fprintf(stderr, "serialize AU state failed\n"); return 1; }
    FILE* file = std::fopen("build/vital-smoke-state.plist", "wb");
    if (!file || std::fwrite(CFDataGetBytePtr(data), 1, CFDataGetLength(data), file) != (size_t)CFDataGetLength(data) || std::fclose(file)) {
        std::fprintf(stderr, "write AU state failed\n"); return 1;
    }
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, sizeof(state)), "restore AU state");
    CFRelease(data); CFRelease(state);

    MIDIClientRef client = 0; MIDIPortRef input = 0, output = 0; MIDIEndpointRef source = 0, destination = 0;
    Inbox fromSource = {dispatch_semaphore_create(0), {}, 0}, fromDestination = {dispatch_semaphore_create(0), {}, 0};
    check(MIDIClientCreate(CFSTR("MidiBuffer smoke isolated"), nullptr, nullptr, &client), "MIDIClientCreate");
    check(MIDISourceCreate(client, CFSTR("MidiBuffer smoke source"), &source), "MIDISourceCreate");
    check(MIDIDestinationCreate(client, CFSTR("MidiBuffer smoke destination"), receive, &fromDestination, &destination), "MIDIDestinationCreate");
    check(MIDIInputPortCreate(client, CFSTR("MidiBuffer smoke input"), receive, &fromSource, &input), "MIDIInputPortCreate");
    check(MIDIOutputPortCreate(client, CFSTR("MidiBuffer smoke output"), &output), "MIDIOutputPortCreate");
    check(MIDIPortConnectSource(input, source, nullptr), "MIDIPortConnectSource");
    const Byte note[] = {0x90, 60, 100}; alignas(MIDIPacketList) Byte storage[256];
    check(MIDIReceived(source, packet(storage, note)), "MIDIReceived"); waitFor(fromSource, note);
    check(MIDISend(output, destination, packet(storage, fromSource.bytes)), "MIDISend"); waitFor(fromDestination, note);
    check(MusicDeviceMIDIEvent(unit, fromDestination.bytes[0], fromDestination.bytes[1], fromDestination.bytes[2], 0), "MusicDeviceMIDIEvent");
    float left[512] = {}, right[512] = {};
    struct { UInt32 count; AudioBuffer buffers[2]; } audio = {2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
    AudioTimeStamp timestamp = {}; timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    double peak = 0;
    for (int block = 0; block < 32; ++block) {
        AudioUnitRenderActionFlags flags = 0; timestamp.mSampleTime = block * 512;
        check(AudioUnitRender(unit, &flags, &timestamp, 0, 512, reinterpret_cast<AudioBufferList*>(&audio)), "AudioUnitRender");
        for (int i = 0; i < 512; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) { std::fprintf(stderr, "non-finite render\n"); return 1; }
            peak = std::fmax(peak, std::fmax(std::fabs(left[i]), std::fabs(right[i])));
        }
    }
    if (peak < 1e-7) { std::fprintf(stderr, "silent render\n"); return 1; }
    std::printf("PASS Vital 1.5.5 virtual source -> private input -> private destination -> AU; MIDI 90 3c 64; 16384 frames/44100 Hz peak %.9g; fixture tests/vital-smoke-state.plist, readback build/vital-smoke-state.plist; MPE off, bend 2 semitones, pressure/timbre modulation unmapped; no hardware route\n", peak);
    MIDIPortDisconnectSource(input, source); MIDIPortDispose(input); MIDIPortDispose(output);
    MIDIEndpointDispose(source); MIDIEndpointDispose(destination); MIDIClientDispose(client);
    AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit);
    return 0;
}
