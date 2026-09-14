#!/usr/bin/env python3
"""Reject production blocking-I/O and logging APIs after stripping comments/strings."""

import re
import sys
from pathlib import Path

DISALLOWED = re.compile(
    r"\b(?:printf|fprintf|sprintf|snprintf|puts|fputs|fopen|fclose|fread|fwrite|"
    r"open|close|read|write|sleep|usleep|system|popen|fsync)\s*\("
    r"|\bstd::(?:cout|cerr|clog|cin|fstream|ifstream|ofstream)\b"
)
COMMENTS_AND_LITERALS = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)


def main() -> None:
    paths = [Path(argument) for argument in sys.argv[1:]]
    if not paths:
        raise SystemExit("FAIL: realtime source inspection requires source paths")
    failures = []
    for path in paths:
        source = path.read_text(encoding="utf-8")
        code = COMMENTS_AND_LITERALS.sub(" ", source)
        for match in DISALLOWED.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            failures.append(f"{path}:{line}: {match.group(0).strip()}")
    if failures:
        print("FAIL: production source contains blocking-I/O or logging candidates:")
        print("\n".join(failures))
        raise SystemExit(1)
    print("PASS: production sources contain no blocking-I/O or logging APIs")


if __name__ == "__main__":
    main()
