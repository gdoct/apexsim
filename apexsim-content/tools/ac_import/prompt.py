"""Asking the person at the console for another name when an import's is taken.

Both importers refuse a stem that is a shipped circuit or car, or an earlier
import of something else. Run from a terminal (the launcher's console is one)
they ask for a different name instead of stopping; with no terminal (a script,
a pipe) the refusal stands, as before.
"""

from __future__ import annotations

import re
import sys

STEM_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_\-]*$")
REPLACE = object()  # the answer "replace what is there"


class StemTaken(Exception):
    """The stem cannot be used. `replaceable`: the thing in the way is an earlier import."""

    def __init__(self, message: str, replaceable: bool = False):
        super().__init__(message)
        self.replaceable = replaceable


def interactive() -> bool:
    try:
        return bool(sys.stdin and sys.stdin.isatty())
    except (AttributeError, ValueError):
        return False


def ask_stem(what: str, reason: str, replaceable: bool):
    """A new stem (str), REPLACE, or None to skip this one."""
    print(f"{what}: {reason}")
    hint = "a different name"
    if replaceable:
        hint += ", ! to replace the existing import"
    while True:
        try:
            answer = input(f"  New name ({hint}; empty to skip): ").strip()
        except EOFError:
            return None
        if not answer:
            return None
        if answer == "!" and replaceable:
            return REPLACE
        if STEM_RE.match(answer):
            return answer
        print("  A name is letters, digits, _ or -, starting with a letter.")


def retry_with_new_stem(run, opts, what: str):
    """`run(opts)`, asking for another stem each time it raises StemTaken.
    Re-raises when there is no terminal or the answer is to skip."""
    import dataclasses
    while True:
        try:
            return run(opts)
        except StemTaken as e:
            if not interactive():
                raise
            answer = ask_stem(what, str(e), e.replaceable)
            if answer is None:
                raise
            opts = dataclasses.replace(opts, force=True) if answer is REPLACE else dataclasses.replace(opts, stem=answer)
