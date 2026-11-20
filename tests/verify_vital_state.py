#!/usr/bin/env python3
"""Verify the restored Vital patch and its explicit smoke-test mapping."""
import hashlib
import json
import plistlib


def load(path):
    with open(path, "rb") as stream:
        raw = stream.read()
    plist = plistlib.loads(raw)
    state = json.loads(plist["jucePluginState"].rstrip(b"\0"))
    return raw, state


fixture, expected = load("tests/vital-smoke-state.plist")
readback, actual = load("build/vital-smoke-state.plist")
# Vital regenerates the unused White Noise sample on state restoration.
for state in (expected, actual):
    assert state["synth_version"] == "1.5.5"
    settings = state["settings"]
    assert settings["mpe_enabled"] == 0 and settings["pitch_bend_range"] == 2
    assert all(not item["source"] and not item["destination"] for item in settings["modulations"])
    del settings["sample"]["samples"]
assert expected == actual, "Vital patch or mapping readback differs from fixture"
print("PASS Vital state readback: init patch, MPE off, bend +/-2 semitones, no pressure/timbre modulation mappings")
print("STATE fixture sha256=" + hashlib.sha256(fixture).hexdigest() +
      " readback sha256=" + hashlib.sha256(readback).hexdigest())
