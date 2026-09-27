"""Where a track's files live: the shipped circuits, then the player's own.

    content/tracks/default/   the circuits that ship with the game (in git)
    content/tracks/custom/    the player's own: imported or hand-made
                              (gitignored; see its README.md)

Every file of a track (`<Stem>.yaml`, `.ats`, `.layout.json`, the
sidecars) sits in the one folder its YAML is in, and a stem is unique
across both folders, because the exports, previews and `-ApexTrack=`
switches are keyed by the stem alone. The Rust tools read the same two
folders (`track_core::ue_export_io::TRACK_DIRS`).
"""

from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TRACKS = REPO / "content" / "tracks"
DEFAULT_DIR = TRACKS / "default"
CUSTOM_DIR = TRACKS / "custom"
TRACK_DIRS = (DEFAULT_DIR, CUSTOM_DIR)


def track_dir(stem: str) -> Path:
    """The folder holding `<stem>.yaml`; the default folder for a track
    that has no YAML yet (a tool writing a new circuit)."""
    for folder in TRACK_DIRS:
        if (folder / f"{stem}.yaml").is_file():
            return folder
    return DEFAULT_DIR


def track_glob(pattern: str) -> list[Path]:
    """`pattern` matched in both folders, default first, sorted within each."""
    return [p for folder in TRACK_DIRS if folder.is_dir() for p in sorted(folder.glob(pattern))]
