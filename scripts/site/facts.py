"""The marketing page's facts, read from the content the game itself reads.

Nothing here is authored: every figure comes from a car.toml, a track YAML,
a layout dossier or the server source, so the page cannot disagree with the
game. `gather()` returns

    cars    one dict per shipped car (content/cars/default)
    tracks  one dict per shipped circuit (content/tracks/default), with an
            SVG outline of its centerline
    n       the counts the copy quotes: {{ n.cars }}, {{ n.tracks }}, ...

Names are the ones the player sees (the YAML's `display_name`, a dossier
corner's `display_name`, a class through `display_class`), never the real,
trademarked ones: see "No trademarks on screen" in CLAUDE.md.
"""

from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

import yaml

if sys.version_info >= (3, 11):
    import tomllib
else:  # pragma: no cover
    import tomli as tomllib

REPO = Path(__file__).resolve().parent.parent.parent
CARS_DIR = REPO / "content" / "cars" / "default"
TRACKS_DIR = REPO / "content" / "tracks" / "default"
HUD_DIR = REPO / "content" / "hud" / "default"
CAR_SETUP_RS = REPO / "server" / "src" / "car_setup.rs"

# Side of the square the track outlines are drawn in (the page's viewBox is
# "-6 -6 212 212"), and the room left round an outline inside it.
MAP_BOX = 200.0
MAP_MARGIN = 8.0
# How far an outline may stray from the centerline, in map units, when it is
# thinned: under a third of the thinnest stroke.
MAP_TOLERANCE = 0.12

W_PER_HP = 745.7

# The order the page shows the classes in.
CLASS_ORDER = ["GT3", "LMP2", "Hypercar", "F1"]

_Loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)


def display_class(raw: str) -> str:
    """`ApexCatalog::DisplayClass`: what a car class or track category is
    shown as (ApexCatalogRows.h)."""
    key = (raw or "").strip()
    low = key.lower()
    if low == "f1":
        return "Formula"
    if low in ("wec", "endurance"):
        return "Endurance"
    if low in ("dtm", "gt3"):
        return "GT3"
    if low in ("indycar", "indy"):
        return "Independent"
    return key


def cars() -> list[dict]:
    out = []
    for toml_path in sorted(CARS_DIR.glob("*/car.toml")):
        with open(toml_path, "rb") as f:
            cfg = tomllib.load(f)
        engine = cfg.get("engine", {})
        sound = cfg.get("sound", {})
        hybrid = cfg.get("hybrid", {})
        hybrid_kw = float(hybrid.get("motor_max_power_kw", 0.0)) if hybrid.get("enabled") else 0.0
        out.append({
            "folder": toml_path.parent.name,
            "name": cfg["name"],
            "brand": cfg.get("brand", ""),
            "cls": cfg.get("class", ""),
            "mass": round(cfg["physics"]["mass_kg"]),
            "hp": round(engine.get("max_power_w", 0.0) / W_PER_HP),
            "redline": round(engine.get("redline_rpm", 0.0)),
            "cyl": int(sound.get("cylinders", 0)),
            "turbo": bool(sound.get("turbo", engine.get("forced_induction", False))),
            "cross": bool(sound.get("crossplane", False)),
            "hybrid": round(hybrid_kw),
            "liv": len(cfg.get("livery", [])),
        })
    return out


def _thin(points: list[tuple[float, float]], tolerance: float) -> list[tuple[float, float]]:
    """Ramer-Douglas-Peucker, iterative: a straight keeps its two ends."""
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        length = math.hypot(dx, dy)
        worst, at = 0.0, -1
        for i in range(a + 1, b):
            px, py = points[i]
            if length > 1e-9:
                d = abs(dx * (py - ay) - dy * (px - ax)) / length
            else:
                d = math.hypot(px - ax, py - ay)
            if d > worst:
                worst, at = d, i
        if worst > tolerance:
            keep[at] = True
            stack.append((a, at))
            stack.append((at, b))
    return [p for p, k in zip(points, keep) if k]


def outline(nodes: list[dict], closed: bool) -> tuple[str, float, float]:
    """The centerline as an SVG path fitted into the map box, north up
    (the server frame is x east, y north; SVG's y runs down), with where
    the start line is."""
    xs = [float(n["x"]) for n in nodes]
    ys = [-float(n["y"]) for n in nodes]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1.0
    scale = (MAP_BOX - 2 * MAP_MARGIN) / span
    ox = (MAP_BOX - (max(xs) - min(xs)) * scale) / 2 - min(xs) * scale
    oy = (MAP_BOX - (max(ys) - min(ys)) * scale) / 2 - min(ys) * scale
    points = [(x * scale + ox, y * scale + oy) for x, y in zip(xs, ys)]
    start = points[0]
    if closed:
        points.append(points[0])
    points = _thin(points, MAP_TOLERANCE)
    if closed:
        points.pop()
    path = "M" + "L".join(f"{x:.1f} {y:.1f}" for x, y in points) + ("Z" if closed else "")
    return path, round(start[0], 1), round(start[1], 1)


def _corners(stem: str) -> tuple[bool, list[str]]:
    """Whether the circuit has a layout dossier, and its corners as the
    player sees them, in lap order."""
    path = TRACKS_DIR / stem / f"{stem}.layout.json"
    if not path.is_file():
        return False, []
    with open(path, encoding="utf-8") as f:
        dossier = json.load(f)
    names: list[str] = []
    for corner in sorted(dossier.get("corners", []), key=lambda c: c.get("station_m", 0.0)):
        shown = corner.get("display_name")
        if shown and shown not in names:
            names.append(shown)
    return True, names


def tracks() -> list[dict]:
    out = []
    for yaml_path in sorted(TRACKS_DIR.glob("*/*.yaml")):
        with open(yaml_path, encoding="utf-8") as f:
            data = yaml.load(f, Loader=_Loader)
        nodes = data.get("nodes") or []
        if not nodes:
            continue
        meta = data.get("metadata") or {}
        stem = yaml_path.stem
        zs = [float(n.get("z") or 0.0) for n in nodes]
        path, sx, sy = outline(nodes, bool(data.get("closed_loop", True)))
        dossier, corner_names = _corners(stem)
        out.append({
            "stem": stem,
            "name": data.get("display_name") or data.get("name") or stem,
            "country": meta.get("country") or "",
            "city": meta.get("city") or "",
            "about": meta.get("description") or "",
            "length": round(float(meta.get("length_m") or 0.0)),
            "elev": round(max(zs) - min(zs)),
            "year": meta.get("year_built"),
            "cat": display_class(meta.get("category") or ""),
            "env": meta.get("environment_type") or "",
            "corners": corner_names,
            "dossier": dossier,
            "dem": (TRACKS_DIR / stem / f"{stem}.dem.msgpack").is_file(),
            "path": path,
            "sx": sx,
            "sy": sy,
        })
    return out


def setup_knobs() -> int:
    """How many knobs a car setup has: the server's `KNOB_COUNT`."""
    found = re.search(r"pub const KNOB_COUNT: usize = (\d+);", CAR_SETUP_RS.read_text(encoding="utf-8"))
    if not found:
        raise RuntimeError(f"{CAR_SETUP_RS}: KNOB_COUNT not found; the setup table has moved")
    return int(found.group(1))


def gather() -> dict:
    car_list = cars()
    track_list = tracks()
    if not car_list or not track_list:
        raise RuntimeError("no cars or no tracks under content/: run from a full checkout")
    classes = [c for c in CLASS_ORDER if any(car["cls"] == c for car in car_list)]
    classes += sorted({car["cls"] for car in car_list} - set(classes))
    longest = max(track_list, key=lambda t: t["length"])
    n = {
        "cars": len(car_list),
        "classes": len(classes),
        "tracks": len(track_list),
        "km": round(sum(t["length"] for t in track_list) / 1000.0),
        "dossiers": sum(1 for t in track_list if t["dossier"]),
        "dems": sum(1 for t in track_list if t["dem"]),
        "countries": len({t["country"] for t in track_list if t["country"]}),
        "liveries": max((car["liv"] for car in car_list), default=0),
        "setup_knobs": setup_knobs(),
        "hud_components": sum(1 for p in HUD_DIR.glob("*/component.json")),
        "longest_km": round(longest["length"] / 1000.0, 1),
        "longest_relief": longest["elev"],
        "tick_hz": 420,
        "telemetry_hz": 60,
    }
    return {"cars": car_list, "tracks": track_list, "classes": classes, "n": n}


if __name__ == "__main__":
    facts = gather()
    json.dump(facts["n"], sys.stdout, indent=2)
    print()
