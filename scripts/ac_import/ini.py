"""AC's INI and JSON side files: `models*.ini`, `data/surfaces.ini`,
`data/drs_zones.ini`, `ui/ui_track.json`, plus the layout discovery.

AC INIs are plain `[SECTION]` / `KEY=VALUE` with `;` and `//` comments,
sometimes in Latin-1, sometimes with a BOM. `ui_track.json` is JSON in name
only: tabs inside strings, trailing commas and Latin-1 bytes all occur, so
it is read forgivingly.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path


def read_text(path: Path) -> str:
    raw = Path(path).read_bytes()
    if raw[:3] == bytes([0xEF, 0xBB, 0xBF]):
        # A UTF-8 BOM on a file that is not otherwise UTF-8 happens; strip
        # it before trying the encodings so it never survives as text.
        raw = raw[3:]
    for enc in ("utf-8", "cp1252", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1", "replace")


def parse_ini(text: str) -> dict[str, dict[str, str]]:
    """Sections in file order; keys upper-cased; later duplicates win."""
    sections: dict[str, dict[str, str]] = {}
    current: dict[str, str] | None = None
    for line in text.splitlines():
        line = line.split(";", 1)[0].split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1].strip().upper()
            current = sections.setdefault(name, {})
            continue
        if "=" in line and current is not None:
            key, value = line.split("=", 1)
            current[key.strip().upper()] = value.strip()
    return sections


def _number(value: str | None, default: float) -> float:
    if value is None:
        return default
    m = re.match(r"^\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)", value)
    return float(m.group(1)) if m else default


@dataclass
class Layout:
    """One drivable layout of an AC track folder."""
    track_dir: Path
    #: "" for a single-layout track, else the subfolder name (`layout_gp`).
    name: str
    models_ini: Path

    @property
    def data_dirs(self) -> list[Path]:
        """Where `surfaces.ini` and `drs_zones.ini` are looked for: the
        layout's own `data/` first, then the track's."""
        dirs = []
        if self.name:
            dirs.append(self.track_dir / self.name / "data")
        dirs.append(self.track_dir / "data")
        return dirs

    @property
    def ai_dir(self) -> Path:
        if self.name:
            return self.track_dir / self.name / "ai"
        return self.track_dir / "ai"

    @property
    def ui_dir(self) -> Path:
        if self.name:
            return self.track_dir / "ui" / self.name
        return self.track_dir / "ui"

    def find_data(self, file_name: str) -> Path | None:
        for d in self.data_dirs:
            p = d / file_name
            if p.is_file():
                return p
        return None

    @property
    def label(self) -> str:
        return f"{self.track_dir.name}/{self.name}" if self.name else self.track_dir.name


def find_layouts(track_dir: Path) -> list[Layout]:
    """Every layout of a track folder: `models.ini` is the single layout,
    `models_<name>.ini` one each. A folder with neither has no track."""
    track_dir = Path(track_dir)
    out: list[Layout] = []
    single = track_dir / "models.ini"
    if single.is_file():
        out.append(Layout(track_dir, "", single))
    for ini in sorted(track_dir.glob("models_*.ini")):
        name = ini.stem[len("models_"):]
        if name:
            out.append(Layout(track_dir, name, ini))
    return out


def model_files(layout: Layout) -> list[str]:
    """The kn5 file names a `models*.ini` lists, in order, unique."""
    sections = parse_ini(read_text(layout.models_ini))
    files: list[str] = []
    for name in sorted(sections, key=_model_order):
        if not name.startswith("MODEL"):
            continue
        f = sections[name].get("FILE", "").strip()
        if f and f not in files:
            files.append(f)
    return files


def _model_order(section: str) -> tuple[int, str]:
    m = re.match(r"^MODEL_?(\d+)$", section)
    return (int(m.group(1)) if m else 1 << 30, section)


@dataclass
class Surface:
    key: str
    friction: float
    valid_track: bool
    pit_lane: bool
    #: Where it came from: "track" or "system".
    origin: str = "track"


def read_surfaces(paths: list[Path]) -> dict[str, Surface]:
    """`surfaces.ini` entries keyed by upper-cased KEY. `paths` in
    precedence order (the track's first, AC's system defaults last)."""
    out: dict[str, Surface] = {}
    for i, path in enumerate(paths):
        if not path or not Path(path).is_file():
            continue
        origin = "track" if i == 0 else "system"
        for name, entry in parse_ini(read_text(path)).items():
            if not name.startswith("SURFACE"):
                continue
            key = entry.get("KEY", "").strip().upper()
            if not key or key in out:
                continue
            out[key] = Surface(
                key=key,
                friction=_number(entry.get("FRICTION"), 1.0),
                valid_track=_number(entry.get("IS_VALID_TRACK"), 1) != 0,
                pit_lane=_number(entry.get("IS_PITLANE"), 0) != 0,
                origin=origin,
            )
    return out


#: AC's own defaults (`system/data/surfaces.ini`), for an install that has
#: no system folder beside the content (a track copied on its own).
SYSTEM_SURFACES = {
    "ROAD": Surface("ROAD", 1.0, True, False, "system"),
    "GRASS": Surface("GRASS", 0.6, False, False, "system"),
    "KERB": Surface("KERB", 0.92, True, False, "system"),
    "SAND": Surface("SAND", 0.8, False, False, "system"),
}


@dataclass
class DrsZone:
    detection: float
    start: float
    end: float


def read_drs_zones(path: Path | None) -> list[DrsZone]:
    """Lap fractions, as AC stores them (relative to the AI spline)."""
    if not path or not Path(path).is_file():
        return []
    zones = []
    for name, entry in parse_ini(read_text(path)).items():
        if not name.startswith("ZONE"):
            continue
        try:
            zones.append(DrsZone(float(entry["DETECTION"]), float(entry["START"]), float(entry["END"])))
        except (KeyError, ValueError):
            continue
    return zones


@dataclass
class UiTrack:
    name: str = ""
    description: str = ""
    country: str = ""
    city: str = ""
    length_m: float | None = None
    pitboxes: int | None = None
    run: str = ""
    tags: list[str] = field(default_factory=list)


def read_ui_track(path: Path | None) -> UiTrack:
    out = UiTrack()
    if not path or not Path(path).is_file():
        return out
    text = read_text(path)
    data = _loads_forgiving(text)
    if not isinstance(data, dict):
        return out
    out.name = str(data.get("name") or "").strip()
    out.description = str(data.get("description") or "").strip()
    out.country = str(data.get("country") or "").strip()
    out.city = str(data.get("city") or "").strip()
    length = str(data.get("length") or "").strip().lower().replace(",", ".")
    m = re.match(r"^([\d.]+)\s*(km|m)?", length)
    if m:
        try:
            value = float(m.group(1))
            out.length_m = value * 1000.0 if m.group(2) == "km" or value < 100 else value
        except ValueError:
            pass
    try:
        out.pitboxes = int(str(data.get("pitboxes") or "").strip())
    except ValueError:
        pass
    out.run = str(data.get("run") or "").strip()
    tags = data.get("tags")
    if isinstance(tags, list):
        out.tags = [str(t) for t in tags]
    return out


def _loads_forgiving(text: str):
    try:
        return json.loads(text, strict=False)
    except json.JSONDecodeError:
        pass
    # Trailing commas and stray control characters are the usual faults.
    cleaned = re.sub(r",\s*([}\]])", r"\1", text)
    cleaned = "".join(ch if ch >= " " or ch in "\n\r\t" else " " for ch in cleaned)
    try:
        return json.loads(cleaned, strict=False)
    except json.JSONDecodeError:
        return None
