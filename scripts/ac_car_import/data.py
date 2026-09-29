"""An AC car's data files (from `data.acd` or `data/`) and its `ui/` JSON,
read into plain structures: INI sections, LUTs and the few parsed figures
the rest of the importer needs.

Every INI is `[SECTION]` / `KEY=VALUE` with `;` comments (the track
importer's parser); a LUT is `x|y` per line. AC is forgiving about both
(tabs, trailing comments, a stray blank key), so is this.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from ac_import.ini import _loads_forgiving, parse_ini, read_text

from .acd import read_car_data


def _decode(blob: bytes) -> str:
    if blob[:3] == bytes([0xEF, 0xBB, 0xBF]):
        blob = blob[3:]
    for enc in ("utf-8", "cp1252", "latin-1"):
        try:
            return blob.decode(enc)
        except UnicodeDecodeError:
            continue
    return blob.decode("latin-1", "replace")


def number(value: str | None, default: float | None = None) -> float | None:
    """The leading number of an INI value (`0.30  ; comment` -> 0.3)."""
    if value is None:
        return default
    m = re.match(r"^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)", value)
    return float(m.group(1)) if m else default


def vector(value: str | None, n: int = 3) -> list[float] | None:
    """`0 ,-0.27 ,0.4` -> [0.0, -0.27, 0.4]."""
    if value is None:
        return None
    parts = [number(p) for p in value.split(",")]
    if len(parts) < n or any(p is None for p in parts[:n]):
        return None
    return [float(p) for p in parts[:n]]


@dataclass
class Lut:
    x: np.ndarray
    y: np.ndarray

    def __call__(self, at: float) -> float:
        """Linear between points, held flat past either end (as AC does)."""
        if self.x.size == 0:
            return 0.0
        return float(np.interp(at, self.x, self.y))

    @property
    def points(self) -> list[tuple[float, float]]:
        return [(float(a), float(b)) for a, b in zip(self.x, self.y)]


def parse_lut(text: str) -> Lut:
    xs: list[float] = []
    ys: list[float] = []
    for line in text.splitlines():
        line = line.split(";", 1)[0].split("//", 1)[0].strip()
        if "|" not in line:
            continue
        a, b = line.split("|", 1)
        x, y = number(a), number(b)
        if x is None or y is None:
            continue
        xs.append(x)
        ys.append(y)
    order = np.argsort(np.asarray(xs, dtype=np.float64), kind="stable")
    return Lut(np.asarray(xs, dtype=np.float64)[order], np.asarray(ys, dtype=np.float64)[order])


@dataclass
class UiCar:
    name: str = ""
    brand: str = ""
    description: str = ""
    car_class: str = ""
    tags: list[str] = field(default_factory=list)
    country: str = ""
    year: int | None = None
    specs: dict = field(default_factory=dict)


def read_ui_car(path: Path) -> UiCar:
    out = UiCar()
    if not path.is_file():
        return out
    data = _loads_forgiving(read_text(path))
    if not isinstance(data, dict):
        return out
    out.name = str(data.get("name") or "").strip()
    out.brand = str(data.get("brand") or "").strip()
    out.description = re.sub(r"<[^>]+>", " ", str(data.get("description") or ""))
    out.car_class = str(data.get("class") or "").strip()
    tags = data.get("tags")
    if isinstance(tags, list):
        out.tags = [str(t).strip() for t in tags]
    out.country = str(data.get("country") or "").strip()
    year = data.get("year")
    if isinstance(year, (int, float)) and 1880 < year < 2100:
        out.year = int(year)
    specs = data.get("specs")
    if isinstance(specs, dict):
        out.specs = specs
    return out


@dataclass
class Skin:
    name: str
    folder: Path
    display_name: str
    #: Every file in the skin folder, by lower-cased name.
    files: dict[str, Path]
    preview: Path | None


def read_skins(car_dir: Path) -> list[Skin]:
    """The car's skins in AC's order (folder names, sorted)."""
    skins_dir = car_dir / "skins"
    out: list[Skin] = []
    if not skins_dir.is_dir():
        return out
    for folder in sorted((p for p in skins_dir.iterdir() if p.is_dir()), key=lambda p: p.name.lower()):
        files = {p.name.lower(): p for p in sorted(folder.iterdir()) if p.is_file()}
        display = folder.name
        ui = folder / "ui_skin.json"
        if ui.is_file():
            data = _loads_forgiving(read_text(ui))
            if isinstance(data, dict) and str(data.get("skinname") or "").strip():
                display = str(data["skinname"]).strip()
        preview = files.get("preview.jpg") or files.get("preview.png")
        out.append(Skin(folder.name, folder, display, files, preview))
    return out


@dataclass
class CarData:
    car_dir: Path
    #: Raw data files, lower-cased names.
    files: dict[str, bytes]
    source: str
    ui: UiCar
    skins: list[Skin]

    def text(self, name: str) -> str | None:
        blob = self.files.get(name.lower())
        return None if blob is None else _decode(blob)

    def ini(self, name: str) -> dict[str, dict[str, str]]:
        text = self.text(name)
        return parse_ini(text) if text is not None else {}

    def lut(self, name: str | None) -> Lut | None:
        if not name:
            return None
        text = self.text(name.strip())
        return parse_lut(text) if text is not None else None


def read_car(car_dir: Path) -> CarData:
    car_dir = Path(car_dir)
    files, source = read_car_data(car_dir)
    return CarData(
        car_dir=car_dir,
        files=files,
        source=source,
        ui=read_ui_car(car_dir / "ui" / "ui_car.json"),
        skins=read_skins(car_dir),
    )


def main_kn5(car_dir: Path) -> Path | None:
    """The car's visual kn5: the largest one that is not a LOD or the
    collider (Kunos name it after the car; mods do not always)."""
    candidates = [
        p for p in sorted(Path(car_dir).glob("*.kn5"))
        if p.name.lower() != "collider.kn5" and not re.search(r"_lod_?[b-z]\.kn5$", p.name.lower())
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda p: (p.stat().st_size, p.name))


def lod_kn5(car_dir: Path, lod: str) -> Path | None:
    for p in sorted(Path(car_dir).glob("*.kn5")):
        if re.search(rf"_lod_?{lod.lower()}\.kn5$", p.name.lower()):
            return p
    return None
