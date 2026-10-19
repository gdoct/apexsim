#!/usr/bin/env python3
"""Fail when the content reaches into the code.

The code (../apexsim) depends on the content and never the other way round:
nothing under the content root may read, build or link a path in the code
root. This walks every text file the content tracks in git and looks for a
path into the code root (`../apexsim/`, `..\\apexsim\\`, a Python path built
as `... / "apexsim" / ...`) or a Cargo dependency on the server or simulation crate.

The formats crate (`../apexsim-lib`) is not code: the content builds
against it (`track-core` depends on `../../../apexsim-lib/formats`), and no
pattern matches a path into it. The edge in ALLOWED is known
(apexsim/docs/proposals/split-off-content.md); a new one fails the check.
This file names the patterns and is not checked itself.

    python tools/check_direction.py        # from the content root; exit 1 on an edge
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# A path into the sibling code root, written any of the ways the tools do.
PATTERNS = [
    re.compile(r"\.\./apexsim/"),
    re.compile(r"\.\.\\apexsim\\"),
    re.compile(r"/\s*[\"']apexsim[\"']\s*/"),
    re.compile(r"^\s*apexsim-(server|sim)\s*=", re.MULTILINE),
]

# (file relative to the content root, why it may) - test-only edges.
ALLOWED = {
    "tools/ac_car_import/tests/test_ac_car_import.py": "reads the simulation's brake constants, skips without them",
}

# Prose may name code paths; generated scene data carries the "apexsim" sign text.
SKIP_SUFFIXES = {".md", ".ats", ".lock"}
# The check itself spells out the patterns in its docstring.
SELF = "tools/check_direction.py"


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                         capture_output=True).stdout
    return [ROOT / p for p in out.decode("utf-8").split("\0") if p]


def main() -> int:
    found = []
    for path in tracked_files():
        rel = path.relative_to(ROOT).as_posix()
        if (path.suffix.lower() in SKIP_SUFFIXES or rel in ALLOWED or rel == SELF
                or not path.is_file()):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # binary (LFS pointers are text and harmless)
        for pattern in PATTERNS:
            for m in pattern.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                found.append(f"{rel}:{line}: {text.splitlines()[line - 1].strip()[:120]}")
    if found:
        print("the content reaches into the code (../apexsim):", file=sys.stderr)
        for f in found:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"no content->code edges beyond the {len(ALLOWED)} allowed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
