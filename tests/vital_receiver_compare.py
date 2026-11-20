#!/usr/bin/env python3
"""Direct versus MidiBuffer-replayed MPE comparison in Vital 1.5.5 (req-014, ac-015).

build/receiver_fixture writes the direct controller stream and the production
MidiCall replay trace; build/vital_receiver_render renders each exact log through
private virtual CoreMIDI endpoints into the installed Vital AudioUnit offline.
Vital, not a hand-written synth, is the behavior oracle. This script records the
receiver configuration, checks the exact logs, compares per-voice rendering
features, proves stop silence against negative controls and probes which MPE
setup/zone capabilities Vital actually implements.
"""
import hashlib
import json
import os
import plistlib
import subprocess
import sys

import numpy as np

OUT = "build/receiver"
RATE, PULSE, LEAD = 48000, 24000, 48000
STOP = LEAD + 5 * PULSE + 6000  # replay stop sample (checked against the fixture)
FRAMES = 300000
VITAL = "/Library/Audio/Plug-Ins/Components/Vital.component"
MPE_PARAMETER = "52567"  # Vital AU parameter "MPE Enabled"
failures = []
report = {"receiver": {}, "configuration": {}, "zones": {}, "probes": {}}


def expect(ok, message):
    if not ok:
        failures.append(message)
        print("FAIL: " + message)


def sha(path):
    with open(path, "rb") as stream:
        return hashlib.sha256(stream.read()).hexdigest()


# Pressure and timbre are made audible per voice; MPE is enabled and the bend
# range manually matched to MidiBuffer's restored 48-semitone member range,
# because Vital ignores MCM/RPN0 (see probes).
MAPPINGS = [("aftertouch", "osc_1_level", 0.6, 0.0), ("slide", "osc_1_pan", 1.0, 1.0)]


def make_state(name, mpe, bend_range):
    with open("tests/vital-smoke-state.plist", "rb") as stream:
        plist = plistlib.load(stream)
    state = json.loads(plist["jucePluginState"].rstrip(b"\0"))
    values = state["settings"]
    values.update(mpe_enabled=float(mpe), pitch_bend_range=float(bend_range),
                  osc_1_random_phase=0.0, osc_1_level=0.25)
    for index, (source, destination, amount, bipolar) in enumerate(MAPPINGS):
        values["modulations"][index] = {"source": source, "destination": destination}
        values["modulation_%d_amount" % (index + 1)] = amount
        values["modulation_%d_bipolar" % (index + 1)] = bipolar
    state["preset_name"] = "MidiBuffer receiver " + name
    plist["jucePluginState"] = json.dumps(state).encode() + b"\0"
    path = "%s/state-%s.plist" % (OUT, name)
    with open(path, "wb") as stream:
        plistlib.dump(plist, stream, fmt=plistlib.FMT_BINARY)
    return path


def read_log(path):
    header, events = {}, []
    with open(path) as stream:
        for line in stream:
            parts = line.split()
            if parts[0] == "#":
                header[parts[1]] = parts[2]
            else:
                events.append((int(parts[0]), tuple(int(b, 16) for b in parts[2:])))
    return header, events


def write_log(path, events):
    with open(path, "w") as stream:
        for sample, data in events:
            stream.write("%d 0 %s\n" % (sample, " ".join("%02x" % b for b in data)))


def render(state, log, tag, mpe_parameter=None):
    base = "%s/%s" % (OUT, tag)
    command = ["build/vital_receiver_render", state, base + ".readback.plist", log,
               base + ".received", base + ".f32", str(FRAMES)]
    subprocess.run(command + ([mpe_parameter] if mpe_parameter else []), check=True,
                   stdout=subprocess.DEVNULL)
    _, sent = read_log(log)
    with open(base + ".received") as stream:
        received = [(int(p[0]), tuple(int(b, 16) for b in p[1:]))
                    for p in (line.split() for line in stream)]
    expect(received == sent, tag + ": receiver input log equals sent log exactly")
    audio = np.fromfile(base + ".f32", dtype="<f4").reshape(-1, 2)
    return audio, sha(base + ".f32")


def peak(audio, start, end, low, high):
    """Frequency and per-channel magnitude of the strongest partial in band."""
    segment = audio[start:end]
    window = np.hanning(len(segment))
    size = 8 * len(segment)
    freqs = np.fft.rfftfreq(size, 1.0 / RATE)
    band = (freqs > low) & (freqs < high)
    index = int(np.argmax(np.where(band, np.abs(np.fft.rfft(segment.sum(1) * window, size)), 0)))
    mags = [float(np.abs(np.fft.rfft(segment[:, c] * window, size))[index] * 2 / window.sum())
            for c in range(2)]
    return {"hz": round(float(freqs[index]), 2), "left": round(mags[0], 5), "right": round(mags[1], 5)}


# Voice fundamentals (A 220/246.94, B 329.63, C 587.33 Hz) in bands clear of
# the other voices' harmonics.
VOICES = {"A": (200, 275), "B": (300, 370), "C": (560, 640)}
WINDOWS = {  # take-relative analysis windows
    "W0 attack": (2000, 10000),
    "W1 A pressure": (14000, 22000),
    "W2 A bend, A/B timbre": (PULSE + 14000, PULSE + 22000),
    "W3 manager bend, C": (2 * PULSE + 6000, 2 * PULSE + 22000),
    "W4 C released, manager centre": (3 * PULSE + 6000, 3 * PULSE + 22000),
}
PASS2 = ("W0 attack", "W1 A pressure")  # replay pass 2 windows before the stop


def features(audio, origin, names=WINDOWS):
    return {name: {voice: peak(audio, origin + WINDOWS[name][0], origin + WINDOWS[name][1], *band)
                   for voice, band in VOICES.items()} for name in names}


def level(f):
    return f["left"] + f["right"]


def same(a, b):
    if level(a) < 1e-3 and level(b) < 1e-3:
        return True  # voice silent in both renders
    return abs(a["hz"] - b["hz"]) <= 0.5 and all(
        abs(a[c] - b[c]) <= 0.002 + 0.03 * max(a[c], b[c]) for c in ("left", "right"))


def rms(audio, start, end):
    return float(np.sqrt((audio[start:end] ** 2).mean()))


def channel_state(events):
    """Receiver-visible state: notes on, bend, pressure, CC74 and held pedals."""
    notes, values = set(), {}
    for _, data in events:
        kind, ch = data[0] & 0xF0, data[0] & 0x0F
        if kind == 0x90 and data[2]:
            notes.add((ch, data[1]))
        elif kind in (0x80, 0x90):
            notes.discard((ch, data[1]))
        elif kind == 0xE0:
            values[(ch, "bend")] = data[1] | data[2] << 7
        elif kind == 0xD0:
            values[(ch, "pressure")] = data[1]
        elif kind == 0xB0 and data[1] in (64, 74):
            values[(ch, data[1])] = data[2]
    # Pedal up and centred bend are receiver defaults, not distinguishing state.
    return notes, {k: v for k, v in values.items()
                   if not (k[1] == 64 and v < 64) and not (k[1] == "bend" and v == 8192 and k[0] in (0, 15))}


def musical(events, start, end):
    return [(s - start, d) for s, d in events if start <= s < end]


def first_attack(events, start):
    return next(i for i, (s, d) in enumerate(events) if s >= start and d[0] & 0xF0 == 0x90 and d[2])


def same_music(a, b):
    return [d for _, d in a] == [d for _, d in b] and all(abs(x[0] - y[0]) <= 3 for x, y in zip(a, b))


def hexes(events, origin):
    return [[s - origin, " ".join("%02x" % b for b in d)] for s, d in events]


def compare_zone(zone, state):
    direct_log = "%s/direct_%s.log" % (OUT, zone)
    replay_log = "%s/replay_%s.log" % (OUT, zone)
    fixture = subprocess.run(["build/receiver_fixture", zone, direct_log, replay_log],
                             check=True, capture_output=True, text=True).stdout.strip()
    print(fixture)
    header, replay = read_log(replay_log)
    _, direct = read_log(direct_log)
    expect(int(header["stop"]) == STOP, zone + ": fixture stop sample")

    # Exact logs. Generated setup/context precede the first attack of each pass.
    entry, wrap = first_attack(replay, LEAD), first_attack(replay, LEAD + 4 * PULSE)
    take = musical(direct, LEAD, LEAD + 4 * PULSE)
    pass1 = musical(replay[entry:], LEAD, LEAD + 4 * PULSE)
    pass2 = musical(replay[wrap:], LEAD + 4 * PULSE, STOP)
    early = musical(direct, LEAD, LEAD + PULSE + 6000)
    expect(same_music(take, pass1), zone + ": replay pass 1 equals the direct take (bytes/order, <=3 samples)")
    expect(same_music(early, pass2), zone + ": replay pass 2 before stop equals the direct take")
    d_notes, d_values = channel_state([e for e in direct if e[0] < LEAD])
    r_notes, r_values = channel_state(replay[:entry])
    expect(d_values == r_values and not d_notes and not r_notes,
           zone + ": deliberate-entry setup/context equals the direct pre-take state")
    w_notes, w_values = channel_state(replay[:wrap])
    expect(not w_notes and w_values == d_values,
           zone + ": wrap endings and restored context equal the direct pre-take state")
    s_notes, s_values = channel_state(replay)
    expect(not s_notes and not any(k[1] == 64 for k in s_values),
           zone + ": replay leaves no note or pedal held after stop")
    expect(header["ownedNotesAfterStop"] == "0" and header["ownedHoldsAfterStop"] == "0",
           zone + ": plug-in reports no owned notes/holds after stop")
    manager = 15 if zone == "upper" else 0
    managers = lambda events: [d for _, d in events if d[0] & 0x0F == manager]
    expect(managers(take) == managers(pass1), zone + ": manager-channel messages replay exactly")
    cleanup = [(s, d) for s, d in replay if s >= STOP]
    zr = {"fixture": fixture, "replayHeader": header,
          "logSha256": {"direct": sha(direct_log), "replay": sha(replay_log)},
          "setupAndContextAtEntry": hexes(replay[:entry], LEAD),
          "wrapEndingsAndContext": hexes([e for e in replay[:wrap] if e[0] >= LEAD + 4 * PULSE], LEAD + 4 * PULSE),
          "stopCleanup": hexes(cleanup, STOP),
          "takeMessages": len(take), "pass1Messages": len(pass1),
          "maxTimingDifferenceSamples": max(abs(a[0] - b[0]) for a, b in zip(take, pass1))}

    # Rendering: features, residual, stop silence and negative controls.
    no_cleanup = "%s/replay_%s_nocleanup.log" % (OUT, zone)
    no_pedal = "%s/replay_%s_nopedalrelease.log" % (OUT, zone)
    write_log(no_cleanup, [e for e in replay if e[0] < STOP])
    write_log(no_pedal, [e for e in replay if e[0] < STOP or e[1][0] & 0xF0 != 0xB0])
    d_audio, d_hash = render(state, direct_log, "direct_" + zone)
    r_audio, r_hash = render(state, replay_log, "replay_" + zone)
    n_audio, _ = render(state, no_cleanup, "nocleanup_" + zone)
    p_audio, _ = render(state, no_pedal, "nopedalrelease_" + zone)
    df, rf = features(d_audio, LEAD), features(r_audio, LEAD)
    r2 = features(r_audio, LEAD + 4 * PULSE, PASS2)
    for name in WINDOWS:
        for voice in VOICES:
            expect(same(df[name][voice], rf[name][voice]),
                   "%s %s %s: replay pass 1 render matches direct" % (zone, name, voice))
    for name in PASS2:
        for voice in VOICES:
            expect(same(df[name][voice], r2[name][voice]),
                   "%s %s %s: replay pass 2 render matches direct" % (zone, name, voice))
    span = slice(LEAD, LEAD + 4 * PULSE)
    relative = lambda x, y: float(np.sqrt(((x[span] - y[span]) ** 2).mean()) / np.sqrt((x[span] ** 2).mean()))
    residual = relative(d_audio, r_audio)
    # The residual comes from <=3-sample host-step receipt quantization: the
    # same direct bytes at the replay's dispatch samples render like replay.
    retimed_log = "%s/direct_%s_retimed.log" % (OUT, zone)
    pre = [e for e in direct if e[0] < LEAD]
    write_log(retimed_log, pre + [(LEAD + t, d) for t, d in pass1] +
              [e for e in direct if e[0] >= LEAD + 4 * PULSE])
    t_audio, _ = render(state, retimed_log, "retimed_" + zone)
    retimed = relative(t_audio, r_audio)
    expect(retimed < 1e-3, zone + ": direct bytes at replay timing render like replay")
    tail = (STOP + 36000, FRAMES)
    tails = {"replayAfterStop": rms(r_audio, *tail), "noCleanupControl": rms(n_audio, *tail),
             "noPedalReleaseControl": rms(p_audio, *tail),
             "directAfterPlayerRelease": rms(d_audio, LEAD + 4 * PULSE + 18000 + 36000, FRAMES)}
    pedal_b = peak(p_audio, tail[0], tail[0] + 16000, *VOICES["B"])
    expect(tails["replayAfterStop"] < 1e-5, zone + ": replay silent after stop")
    expect(tails["noCleanupControl"] > 1e-3, zone + ": control without stop cleanup keeps sounding")
    expect(tails["noPedalReleaseControl"] > 1e-3 and abs(pedal_b["hz"] - 329.63) < 1,
           zone + ": control without pedal release keeps member B sustained")
    expect(tails["directAfterPlayerRelease"] < 1e-5, zone + ": direct silent after player releases")

    # Intended expression, as rendered by the receiver from the direct stream.
    w = df
    expect(level(w["W1 A pressure"]["A"]) > 5 * level(w["W0 attack"]["A"]) and
           abs(level(w["W1 A pressure"]["B"]) - level(w["W0 attack"]["B"])) < 0.002,
           zone + ": pressure changes only member A")
    expect(abs(w["W2 A bend, A/B timbre"]["A"]["hz"] - 246.94) < 1.0 and
           abs(w["W2 A bend, A/B timbre"]["B"]["hz"] - 329.63) < 1.0,
           zone + ": member A bends +2 st while B stays")
    a, b = w["W2 A bend, A/B timbre"]["A"], w["W2 A bend, A/B timbre"]["B"]
    expect(a["right"] < 0.02 * a["left"] and b["left"] < 0.02 * b["right"],
           zone + ": CC74 timbre pans A left and B right independently")
    expect(abs(w["W3 manager bend, C"]["C"]["hz"] - 587.33) < 1.0 and
           abs(w["W3 manager bend, C"]["B"]["hz"] - 329.63) < 1.0,
           zone + ": Vital applies no manager bend (receiver limitation)")
    expect(level(w["W4 C released, manager centre"]["C"]) < 0.1 * level(w["W3 manager bend, C"]["C"]),
           zone + ": C released; Vital ignores manager sustain (receiver limitation)")
    zr.update(renderSha256={"direct": d_hash, "replay": r_hash}, direct=df, replayPass1=rf,
              replayPass2=r2, pass1RelativeResidual=round(residual, 6),
              retimedDirectRelativeResidual=retimed, tailRms=tails,
              noPedalReleaseTailB=pedal_b)
    print("RECEIVER %s: %d windows x 3 voices match; pass-1 residual %.4f (%.1e at replay timing); "
          "stop tail %.1e (controls %.1e/%.1e)" % (zone, len(WINDOWS), residual, retimed, tails["replayAfterStop"],
                                    tails["noCleanupControl"], tails["noPedalReleaseControl"]))
    report["zones"][zone] = zr


# Capability probes: direct streams only, no MidiBuffer involved.
def bend(ch, v): return (0xE0 | ch, v & 127, v >> 7)
def cc(ch, c, v): return (0xB0 | ch, c, v)
def on(ch, n): return (0x90 | ch, n, 100)
def mcm(ch, n): return [cc(ch, 101, 0), cc(ch, 100, 6), cc(ch, 6, n)]
def rpn0(ch, n): return [cc(ch, 101, 0), cc(ch, 100, 0), cc(ch, 6, n), cc(ch, 38, 0), cc(ch, 101, 127), cc(ch, 100, 127)]


def probe(name, state, a, b, setup, changes, mpe_parameter=None):
    """Notes 220 Hz on channel a and 329.63 Hz on b; one change per pulse.
    Returns [A Hz, B Hz, A level] per window."""
    events = [(0, m) for m in setup] + [(100, on(a, 57)), (100, on(b, 64))]
    events += [(PULSE * (i + 1), m) for i, m in enumerate(changes)]
    path = "%s/probe_%s.log" % (OUT, name)
    write_log(path, events)
    audio, _ = render(state, path, "probe_" + name, mpe_parameter)
    windows = range(12000, PULSE * (len(changes) + 1), PULSE)
    return [[peak(audio, s, s + 12000, *VOICES[v])["hz"] for v in "AB"] +
            [round(level(peak(audio, s, s + 12000, *VOICES["A"])), 5)] for s in windows]


def probes(mpe48, mpe2, flag_off):
    up = 12288  # +1 semitone at range 2, +24 at 48
    r = {
        "lowerNoMcm": probe("lower_nomcm", mpe2, 1, 2, [], [bend(0, up), bend(1, up)]),
        "lowerMcm3": probe("lower_mcm3", mpe2, 1, 2, mcm(0, 3), [bend(0, up), bend(1, up)]),
        "upperMcm3": probe("upper_mcm3", mpe2, 14, 13, mcm(15, 3), [bend(15, up), bend(14, up)]),
        "patchMpeOff": probe("patch_mpe_off", flag_off, 1, 2, mcm(0, 3), [bend(0, up), bend(1, up)]),
        "auParameterMpeOn": probe("au_mpe_on", mpe2, 1, 2, mcm(0, 3), [bend(0, up), bend(1, up)], "1"),
        "dual7x7": probe("dual", mpe2, 1, 14, mcm(0, 7) + mcm(15, 7), [bend(0, up), bend(15, up)]),
        "memberRpn0To48": probe("rpn0", mpe2, 1, 2, mcm(0, 3) + rpn0(1, 48) + rpn0(2, 48), [bend(1, 8533)]),
        "patchRange48": probe("range48", mpe48, 1, 2, [], [bend(1, 8533), bend(0, up)]),
        "managerSustain": probe("manager_sustain", mpe2, 1, 2, mcm(0, 3), [cc(0, 64, 127), (0x81, 57, 64)]),
        "memberSustain": probe("member_sustain", mpe2, 1, 2, mcm(0, 3), [cc(1, 64, 127), (0x81, 57, 64)]),
    }
    near = lambda x, y: abs(x - y) < 1
    for key in ("lowerNoMcm", "lowerMcm3", "upperMcm3", "patchMpeOff", "auParameterMpeOn"):
        expect(near(r[key][1][0], 220) and near(r[key][1][1], 329.6), key + ": manager bend ignored")
        expect(near(r[key][2][0], 233.08) and near(r[key][2][1], 329.6), key + ": member bend channel-local")
    expect(near(r["dual7x7"][1][0], 220) and near(r["dual7x7"][2][1], 329.6), "dual7x7: managers ignored")
    expect(near(r["memberRpn0To48"][1][0], 221.06), "memberRpn0To48: RPN0 ignored, range stays 2")
    expect(near(r["patchRange48"][1][0], 246.94) and near(r["patchRange48"][2][1], 329.6),
           "patchRange48: manual range matches +2 st member bend; manager still ignored")
    expect(r["managerSustain"][2][2] < 1e-4, "manager sustain does not hold member A")
    expect(near(r["memberSustain"][2][0], 220) and r["memberSustain"][2][2] > 5e-3,
           "member sustain holds its own note")
    report["probes"] = {"[A Hz, B Hz, A level] per window (baseline, after change 1, after change 2)": r, "findings": [
        "Vital 1.5.5 applies bend, pressure, CC74 and CC64 only to notes on the same channel",
        "No manager channel: default, lower MCM, upper MCM and dual 7+7 layouts all ignore manager bend; "
        "manager sustain does not reach member notes",
        "Patch mpe_enabled off and AU parameter 52567 'MPE Enabled' on give the same channel-local behavior",
        "MCM and RPN0 are delivered but ignored; member bend range is the patch pitch_bend_range (manual only)",
        "Automatic MPE setup interoperability (MCM/RPN0, upper/dual zones, manager effects) is unsupported by "
        "this receiver; the manual 48-semitone match proves only that matched configuration"]}


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(VITAL + "/Contents/Info.plist", "rb") as stream:
        bundle = plistlib.load(stream)
    report["receiver"] = {
        "component": VITAL, "bundleVersion": bundle.get("CFBundleShortVersionString"),
        "bundleId": bundle.get("CFBundleIdentifier"),
        "binarySha256": sha(VITAL + "/Contents/MacOS/" + bundle["CFBundleExecutable"]),
        "audioUnit": "aumu/Vita/Tyte version 66821 (1.5.5); offline AudioUnitRender 48 kHz stereo, 64-frame "
                     "blocks, sample-accurate MusicDeviceMIDIEvent offsets",
        "ports": "private CoreMIDI client 'MidiBuffer receiver isolated': source 'MidiBuffer receiver source' -> "
                 "input port -> output port -> destination 'MidiBuffer receiver destination'; no physical "
                 "MIDI or audio device",
        "setup": "manual: MPE enable and 48-semitone bend range are patch settings; fixture MCM/RPN0 are delivered "
                 "but ignored by Vital (automatic setup unsupported)"}
    mpe48 = make_state("mpe48", True, 48)
    mpe2, flag_off = make_state("mpe2", True, 2), make_state("mpe-off", False, 2)
    write_log(OUT + "/empty.log", [])
    render(mpe48, OUT + "/empty.log", "readback")
    with open(OUT + "/readback.readback.plist", "rb") as stream:
        actual = json.loads(plistlib.load(stream)["jucePluginState"].rstrip(b"\0"))
    values = actual["settings"]
    expect(actual["synth_version"] == "1.5.5", "receiver reports Vital 1.5.5")
    expect(values["mpe_enabled"] == 1 and values["pitch_bend_range"] == 48, "MPE/range readback")
    expect([(m["source"], m["destination"]) for m in values["modulations"]] ==
           [(s, d) for s, d, _, _ in MAPPINGS] + [("", "")] * (len(values["modulations"]) - 2),
           "pressure/timbre mapping readback")
    report["configuration"] = {
        "stateSha256": sha(mpe48), "readbackSha256": sha(OUT + "/readback.readback.plist"),
        "patch": "Vital init patch, osc 1 only, random phase off, polyphony %g, filter off" % values["polyphony"],
        "mpe_enabled": values["mpe_enabled"], "pitch_bend_range": values["pitch_bend_range"],
        "pressure": "aftertouch -> osc_1_level +%.2f over base %.2f" % (values["modulation_1_amount"], values["osc_1_level"]),
        "timbre": "slide (CC74) -> osc_1_pan, bipolar amount %.2f" % values["modulation_2_amount"],
        "zones": {"lower": "MCM ch1 = 3 members (ch2-4)", "upper": "MCM ch16 = 3 members (ch15-13)"}}
    for zone in ("lower", "upper"):
        compare_zone(zone, mpe48)
    _, again = render(mpe48, OUT + "/direct_lower.log", "determinism")
    expect(again == report["zones"]["lower"]["renderSha256"]["direct"], "repeated offline render is bit-identical")
    probes(mpe48, mpe2, flag_off)
    report["failures"] = failures
    with open(OUT + "/report.json", "w") as stream:
        json.dump(report, stream, indent=1, sort_keys=True)
    if failures:
        print("%d receiver comparison check(s) failed" % len(failures))
        return 1
    print("PASS: Vital 1.5.5 direct/replayed lower+upper comparison: exact logs, per-voice pressure/bend/timbre "
          "renders, manager messages, stop silence vs controls; probes: no MCM/RPN0/manager/upper/dual setup support")
    return 0


if __name__ == "__main__":
    sys.exit(main())
