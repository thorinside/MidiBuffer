#!/usr/bin/env python3
"""Validate the frozen MidiBuffer Three requirement/criterion manifest."""

import json
import re
import sys
from pathlib import Path

SPEC_SHA256 = "aefc6eaf7fc3736d253e6b929e2efed368d2fe9076d0bd9fd1f189a89ec5d05b"
REQUIREMENT_KEYS = [
    "requirement-790534bede8341f9ecad7b7e",
    "requirement-45386de0d79316203bbc2d51",
    "requirement-107312bfd6d8197c5ef509d0",
]
TRACKED_CRITERIA = {
    "ac-pb-001": (
        "AC-PB-001",
        "**AC-PB-001:** The Playback parameter exposes Off = 0 and On = 1; setting 1 requests playback on, and setting 0 stops playback, rather than treating each trigger as a toggle.",
    ),
    "ac-pb-002": (
        "AC-PB-002",
        "**AC-PB-002:** The left encoder and Playback parameter control one shared on/off state; successive left-encoder presses toggle the parameter between On = 1 and Off = 0.",
    ),
    "ac-pb-003": (
        "AC-PB-003",
        "**AC-PB-003:** Preset saving includes the shared Playback parameter value through normal parameter persistence.",
    ),
    "ac-pb-004": (
        "AC-PB-004",
        "**AC-PB-004:** Setting Playback to On when there is nothing to play leaves its value at 1 and produces no playback; it does not automatically reset to Off.",
    ),
    "ac-pb-005": (
        "AC-PB-005",
        "**AC-PB-005:** Selecting a valid loop while Playback remains On begins playback under existing clock timing without another Off/On action.",
    ),
}
REQUIREMENT_LABELS = [
    "1. Shared Playback control",
    "2. Zoom-aware boundary editing",
    "3. Resolution label",
    "4. One-shot Clear Recording",
    "5. Host compatibility, persistence and real-time safety",
    "6. Inherited scope and release boundaries",
]
INCREMENTAL_CRITERIA = [
    *[f"AC-PB-{number:03d}" for number in range(1, 8)],
    *[f"AC-ZM-{number:03d}" for number in range(1, 5)],
    "AC-RD-001",
    *[f"AC-CR-{number:03d}" for number in range(1, 7)],
    *[f"AC-IC-{number:03d}" for number in range(1, 11)],
]
INHERITED_REQUIREMENTS = [f"REQ-{number:03d}" for number in range(1, 38)]
INHERITED_CRITERIA = [
    f"AC-{number:03d}" for number in range(1, 68) if number != 46
]
UX_REQUIREMENTS = [f"UX-REQ-{number:03d}" for number in range(1, 12)]
UX_CRITERIA = [f"UX-AC-{number:03d}" for number in range(1, 13)]


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: traceability manifest {message}")


def registry_values(value):
    if isinstance(value, dict):
        for child in value.values():
            yield from registry_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from registry_values(child)
    elif isinstance(value, str) and re.fullmatch(
        r"(?:requirement-[0-9a-f]+|ac-[a-z]+-[0-9]+)", value
    ):
        yield value


def main() -> None:
    if len(sys.argv) != 2:
        fail("validator expects exactly one manifest path")
    manifest_path = Path(sys.argv[1])
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot be read as JSON: {error}")

    if manifest.get("version") != 1:
        fail("version must remain 1")
    if manifest.get("approvedSpecSha256") != SPEC_SHA256:
        fail("does not name the frozen approved Spec revision")
    if manifest.get("registryRequirementKeys") != REQUIREMENT_KEYS:
        fail("requirement keys differ from the three supplied keys")

    tracked = manifest.get("trackedAcceptanceCriteria")
    if not isinstance(tracked, list) or len(tracked) != len(TRACKED_CRITERIA):
        fail("must contain exactly the five supplied criterion-key records")
    actual_tracked = {}
    for record in tracked:
        key = record.get("criterionKey")
        if key in actual_tracked:
            fail(f"duplicates criterion key {key}")
        actual_tracked[key] = (record.get("specIdentifier"), record.get("text"))
        if not record.get("evidence"):
            fail(f"has no evidence seam for {key}")
    if actual_tracked != TRACKED_CRITERIA:
        fail("changes a supplied criterion key, identifier, or exact wording")

    allowed_registry_values = set(REQUIREMENT_KEYS) | set(TRACKED_CRITERIA)
    actual_registry_values = set(registry_values(manifest))
    if actual_registry_values != allowed_registry_values:
        fail("omits a supplied registry key or invents an unsupplied registry key")

    requirements = manifest.get("specRequirements")
    if not isinstance(requirements, list) or [
        requirement.get("label") for requirement in requirements
    ] != REQUIREMENT_LABELS:
        fail("does not preserve all six textual Spec requirement labels in order")
    actual_incremental = []
    for requirement in requirements[:5]:
        actual_incremental.extend(requirement.get("criterionIdentifiers", []))
    if actual_incremental != INCREMENTAL_CRITERIA:
        fail("incremental normative criterion identifiers are incomplete or reordered")

    inherited = manifest.get("inheritedNormativeIdentifiers", {})
    expected_inherited = {
        "requirements": INHERITED_REQUIREMENTS,
        "acceptanceCriteria": INHERITED_CRITERIA,
        "uxRequirements": UX_REQUIREMENTS,
        "uxAcceptanceCriteria": UX_CRITERIA,
        "retiredIdentifiers": ["AC-046"],
    }
    if inherited != expected_inherited:
        fail("inherited normative or retired identifier inventory is incomplete")

    seams = manifest.get("verificationSeams")
    if not isinstance(seams, list) or len(seams) != 6:
        fail("must map all six verification scopes")
    if any(not seam.get("scope") or not seam.get("evidence") for seam in seams):
        fail("contains an empty verification scope or evidence seam")

    boundaries = manifest.get("evidenceBoundaries", {})
    if set(boundaries) != {"nativeAndEmulator", "arm", "physical", "excluded"}:
        fail("must report native/emulator, ARM, physical, and excluded evidence separately")
    if any(not text for text in boundaries.values()):
        fail("contains an empty evidence boundary")

    print(
        "PASS: traceability manifest preserves 3 supplied requirement keys, "
        "5 supplied criterion keys, 28 incremental criteria, and all inherited identifiers"
    )


if __name__ == "__main__":
    main()
