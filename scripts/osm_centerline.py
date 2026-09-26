#!/usr/bin/env python3
"""Build a circuit's track YAML centerline from OpenStreetMap raceway ways.

Every other circuit's centerline came from a public GPS trace (the TUMFTM
racetrack database). The Nordschleife has none, but OpenStreetMap maps it
end to end as `highway=raceway`, surveyed to a metre or two - so its lap is
routed over the OSM raceway graph instead:

    python scripts/osm_centerline.py Nordschleife            # -> content/tracks/real/Nordschleife.yaml
    python scripts/osm_centerline.py Nordschleife --dry-run  # report only
    python scripts/osm_centerline.py Nordschleife --plot out.png

A lap is given in `LAPS` as an ordered list of waypoints (lon, lat) in race
direction. Consecutive waypoints are joined by the shortest path over the
raceway graph (ways joined wherever they share a node), so a waypoint only
has to sit near the right stretch of tarmac for the route to follow it;
listing enough of them pins the lap to one layout where several share the
tarmac (the Nordschleife meets the GP circuit at both ends). The route is
resampled to `STEP_M` nodes, the frame is the repo's (origin at the first
waypoint's projection, +X along the course there, +Y left), and the widths
come from the ways' `width` tags where OSM has them, `DEFAULT_WIDTH_M`
elsewhere, eased along the lap.

The raceline is a minimum-curvature line (the same objective as the TUMFTM
lines the other circuits carry): lateral offsets from the centerline,
bounded by the road less a margin, minimising the squared second
difference of the line, solved as one sparse bounded least-squares
problem. Elevation is left at zero here: `scripts/dem_elevation.py` derives
it from the DEM sidecar in the refresh order (CLAUDE.md), and the raceline
follows the road's change there.

The extract is the one `scripts/osm_layout.py` caches
(`content/tracks/osm-cache/<Stem>.<i>.json`); run that with `--offline`
after placing it, or let it fetch. Everything here is deterministic: same
extract, same YAML, byte for byte.
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import sys
import uuid
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from osm_layout import BBOXES, CACHE_DIR, R_EARTH, TRACK_DIR, is_pit_way, osm_name  # noqa: E402

STEP_M = 5.0
RACELINE_MARGIN_M = 1.2

# The lap, waypoint by waypoint in race direction, and what OSM does not
# say about it. `track_id` is fixed so the client's catalog row can match
# (CLAUDE.md, "Every track YAML needs a fixed track_id").
LAPS: dict[str, dict] = {
    # The Nordschleife on its own, 20.8 km, from the T13 start/finish
    # through the Hohenrain chicane (the layout the industry laps and the
    # touring-car records are set on). The waypoints are OSM's own
    # `place=locality` nodes for each section, which OSM puts beside the
    # stretch they name, in race (clockwise) order.
    "Nordschleife": {
        "name": "Nürburger Mordschleife",
        "track_id": "7648a87e-a67d-43c2-b92a-84022cc3ca75",
        "start_finish": (6.95077, 50.33783),  # "Start-Ziel T13"
        # OSM's T13 node projects onto the lap right where the road comes
        # out of the 70-degree bend from Hohenrain, and the fallback grid
        # is laid straight back from the line for 64 m: rows 8-16 stood in
        # the barriers. 75 m up the T13 straight the whole grid is on it.
        "start_offset_m": 75.0,
        # (from, to, radians) in the final stations: the Karussell's
        # concrete bowl (about 12 degrees across the road on average; the
        # inside half is steeper) and the Mini-Karussell's lip. Both are
        # left-handers, so negative (the right edge lifted); `ats-bank`
        # re-lays them over their bends.
        "banking": [(11905.0, 12105.0, -0.21), (16845.0, 16965.0, -0.12)],
        "waypoints": [
            (6.95077, 50.33783),  # Start-Ziel T13
            (6.94162, 50.33820),  # Hatzenbach
            (6.93396, 50.34052),  # Hocheichen
            (6.92595, 50.34659),  # Flugplatz
            (6.92414, 50.35605),  # Schwedenkreuz
            (6.92826, 50.36202),  # Fuchsröhre
            (6.93087, 50.36629),  # Adenauer Forst
            (6.93737, 50.37219),  # Metzgesfeld
            (6.93416, 50.37408),  # Kallenhard
            (6.94347, 50.37622),  # Wehrseifen
            (6.95041, 50.37638),  # Breidscheid
            (6.96053, 50.38072),  # Bergwerk
            (6.97073, 50.37421),  # Kesselchen
            (6.98480, 50.37402),  # Klostertal
            (6.98599, 50.37192),  # Caracciola-Karussell
            (6.99525, 50.37667),  # Hohe Acht
            (7.00250, 50.37503),  # Wippermann
            (7.00242, 50.37170),  # Eschbach
            (7.00541, 50.36966),  # Brünnchen
            (6.99911, 50.36532),  # Pflanzgarten
            (6.98417, 50.35871),  # Schwalbenschwanz
            (6.98589, 50.35540),  # Galgenkopf
            (6.98243, 50.35005),  # Döttinger Höhe
            (6.96030, 50.34379),  # Antoniusbuche
            (6.95608, 50.34052),  # Tiergarten
            (6.95304, 50.33822),  # Hohenrain-Schikane
        ],
        # The Nordschleife is narrow: 8-9 m for most of the lap, wider on
        # the Döttinger Höhe. OSM `width` tags win where a way has one.
        "default_width_m": 9.0,
        "min_osm_width_m": 7.0,
        "metadata": {
            "country": "Germany",
            "city": "Nürburg",
            "description": "Modelled on the 20.8 km northern loop at Nürburg, through the forests of Germany's Eifel hills.",
            "year_built": 1927,
            "category": "Endurance",
            "environment_type": "forest",
            "terrain_seed": None,
            "terrain_scale": None,
            "terrain_detail": None,
            "terrain_blend_width": None,
            "object_density": None,
            "decal_profile": None,
        },
    },
}


def _wgs(lon, lat, lon0, lat0):
    return (
        math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0)),
        math.radians(lat - lat0) * R_EARTH,
    )


def load_extract(stem: str) -> dict:
    nodes: dict[int, tuple[float, float]] = {}
    ways: dict[int, dict] = {}
    for p in sorted(CACHE_DIR.glob(f"{stem}.*.json")):
        for e in json.loads(p.read_text(encoding="utf-8"))["elements"]:
            if e["type"] == "node":
                nodes[e["id"]] = (e["lon"], e["lat"])
            elif e["type"] == "way":
                ways.setdefault(e["id"], e)
    if not nodes:
        raise SystemExit(f"{stem}: no extract under {CACHE_DIR}")
    return {"nodes": nodes, "ways": ways}


class RacewayGraph:
    """Every raceway node, and an edge between consecutive nodes of a way."""

    def __init__(self, ext: dict, spec: dict, lon0: float, lat0: float):
        self.lon0, self.lat0 = lon0, lat0
        self.xy: dict[int, np.ndarray] = {}
        self.adj: dict[int, list[tuple[int, float, int]]] = {}
        self.way_width: dict[int, float] = {}
        exclude = set(spec.get("exclude_ways", ()))
        for wid, w in ext["ways"].items():
            t = w.get("tags") or {}
            if t.get("highway") != "raceway" or wid in exclude:
                continue
            if t.get("area") == "yes" or is_pit_way(t) or "kart" in (osm_name(t) or "").lower():
                continue
            ids = [n for n in w["nodes"] if n in ext["nodes"]]
            for n in ids:
                if n not in self.xy:
                    self.xy[n] = np.array(_wgs(*ext["nodes"][n], lon0, lat0))
            try:
                width = float(str(t.get("width", "")).replace("m", "").strip())
                # The Nordschleife's sections are tagged `width=5`, which
                # is a lane, not the road; below the floor OSM's figure is
                # not the carriageway and the default stands.
                if width >= spec.get("min_osm_width_m", 0.0):
                    self.way_width[wid] = width
            except ValueError:
                pass
            oneway = t.get("oneway") in ("yes", "1")
            for a, b in zip(ids, ids[1:]):
                d = float(np.hypot(*(self.xy[b] - self.xy[a])))
                self.adj.setdefault(a, []).append((b, d, wid))
                if not oneway:
                    self.adj.setdefault(b, []).append((a, d, wid))
        self.ids = np.array(sorted(self.xy))
        self.pts = np.array([self.xy[i] for i in self.ids])

    def nearest(self, p) -> int:
        d = np.hypot(*(self.pts - p).T)
        return int(self.ids[int(d.argmin())])

    def route(self, a: int, b: int, avoid: set[int]) -> list[tuple[int, int]]:
        """Shortest path a -> b as (node, way-it-was-reached-by) pairs,
        never through `avoid` (the route so far, so the lap stays simple)."""
        dist = {a: 0.0}
        prev: dict[int, tuple[int, int]] = {}
        heap = [(0.0, a)]
        while heap:
            d, n = heapq.heappop(heap)
            if n == b:
                break
            if d > dist.get(n, math.inf):
                continue
            for m, w, wid in self.adj.get(n, ()):
                if m in avoid and m != b:
                    continue
                nd = d + w
                if nd < dist.get(m, math.inf):
                    dist[m] = nd
                    prev[m] = (n, wid)
                    heapq.heappush(heap, (nd, m))
        if b not in dist:
            raise SystemExit(f"no raceway route from node {a} to node {b}")
        out = []
        n = b
        while n != a:
            p, wid = prev[n]
            out.append((n, wid))
            n = p
        return out[::-1]


def resample(pts: np.ndarray, step: float) -> tuple[np.ndarray, np.ndarray]:
    """Closed polyline -> nodes `step` apart (the last one short of the
    first); also each new node's parameter along the old polyline."""
    closed = np.vstack([pts, pts[:1]])
    seg = np.hypot(*np.diff(closed, axis=0).T)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    total = s[-1]
    n = int(round(total / step))
    q = np.linspace(0.0, total, n, endpoint=False)
    x = np.interp(q, s, closed[:, 0])
    y = np.interp(q, s, closed[:, 1])
    return np.column_stack([x, y]), q


def smooth_closed(v: np.ndarray, window: int) -> np.ndarray:
    k = np.ones(window) / window
    pad = np.concatenate([v[-window:], v, v[:window]])
    return np.convolve(pad, k, mode="same")[window:-window]


def left_normals(p: np.ndarray) -> np.ndarray:
    d = np.roll(p, -1, axis=0) - np.roll(p, 1, axis=0)
    d /= np.hypot(*d.T)[:, None]
    return np.column_stack([-d[:, 1], d[:, 0]])


def min_curvature_offsets(p: np.ndarray, wl: np.ndarray, wr: np.ndarray, margin: float) -> np.ndarray:
    """Lateral offsets (positive left) minimising the summed squared second
    difference of `p + a * n`, within the road less `margin`."""
    from scipy.optimize import lsq_linear
    from scipy.sparse import csr_matrix

    n = left_normals(p)
    N = len(p)
    rows, cols, vals = [], [], []
    rhs = np.zeros(2 * N)
    for i in range(N):
        for k, c in ((i - 1, 1.0), (i, -2.0), (i + 1, 1.0)):
            k %= N
            for dim in (0, 1):
                rows.append(2 * i + dim)
                cols.append(k)
                vals.append(c * n[k, dim])
        for dim in (0, 1):
            rhs[2 * i + dim] = -(p[(i - 1) % N, dim] - 2 * p[i, dim] + p[(i + 1) % N, dim])
    A = csr_matrix((vals, (rows, cols)), shape=(2 * N, N))
    lo = -np.maximum(wr - margin, 0.0)
    hi = np.maximum(wl - margin, 0.0)
    res = lsq_linear(A, rhs, bounds=(lo - 1e-6, hi + 1e-6), method="bvls" if N < 600 else "trf",
                     lsmr_tol="auto", max_iter=5000)
    return np.clip(res.x, lo, hi)


def build(stem: str) -> dict:
    spec = LAPS[stem]
    ext = load_extract(stem)
    box = BBOXES[stem][0]
    lon0, lat0 = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    g = RacewayGraph(ext, spec, lon0, lat0)
    wps = [g.nearest(np.array(_wgs(lon, lat, lon0, lat0))) for lon, lat in spec["waypoints"]]
    route: list[tuple[int, int]] = []
    seen: set[int] = set()
    for i, a in enumerate(wps):
        b = wps[(i + 1) % len(wps)]
        leg = g.route(a, b, seen)
        route += leg
        seen.update(n for n, _ in leg)
    raw = np.array([g.xy[n] for n, _ in route])
    raw_way = np.array([w for _, w in route])
    # Drop repeated points (ways meeting end to end).
    keep = np.concatenate([[True], np.hypot(*np.diff(raw, axis=0).T) > 0.05])
    raw, raw_way = raw[keep], raw_way[keep]

    # Rotate the loop to start at the start/finish waypoint's projection.
    sf = np.array(_wgs(*spec["start_finish"], lon0, lat0))
    i0 = int(np.hypot(*(raw - sf).T).argmin())
    raw = np.roll(raw, -i0, axis=0)
    raw_way = np.roll(raw_way, -i0)

    pts, _ = resample(raw, STEP_M)
    shift = int(round(spec.get("start_offset_m", 0.0) / STEP_M))
    pts = np.roll(pts, -shift, axis=0)
    # The way under each resampled node, for its width.
    j = np.array([int(np.hypot(*(raw - q).T).argmin()) for q in pts])
    widths = np.array([g.way_width.get(int(raw_way[k]), spec["default_width_m"]) for k in j])
    for lo_m, hi_m, w in spec.get("widths", ()):
        s = np.arange(len(pts)) * STEP_M
        widths[(s >= lo_m) & (s < hi_m)] = w
    widths = smooth_closed(widths, 13)

    # The frame: origin at node 0, +X along the course there.
    h = pts[1] - pts[0]
    ang = math.atan2(h[1], h[0])
    c, s_ = math.cos(-ang), math.sin(-ang)
    rot = np.array([[c, -s_], [s_, c]])
    local = (pts - pts[0]) @ rot.T

    half = widths / 2.0
    off = min_curvature_offsets(local, half, half, RACELINE_MARGIN_M)
    race = local + left_normals(local) * off[:, None]

    seg = np.hypot(*np.diff(np.vstack([local, local[:1]]), axis=0).T)
    length = float(seg.sum())
    turn = np.degrees(np.arctan2(race[1, 1] - race[0, 1], race[1, 0] - race[0, 0]))
    return {
        "local": local,
        "half": half,
        "banking": banking_of(spec, len(local)),
        "race": race,
        "length": length,
        "lonlat0": (lon0, lat0),
        "report": {
            "raw_points": len(raw),
            "nodes": len(local),
            "length_m": round(length, 1),
            "width_m": (round(float(widths.min()), 2), round(float(widths.max()), 2)),
            "raceline_heading0_deg": round(float(turn), 1),
            "ways": len(set(int(w) for w in raw_way)),
        },
    }


def banking_of(spec: dict, n: int) -> np.ndarray:
    bank = np.zeros(n)
    s = np.arange(n) * STEP_M
    for lo, hi, rad in spec.get("banking", ()):
        bank[(s >= lo) & (s <= hi)] = rad
    return bank


def write_yaml(stem: str, spec: dict, b: dict) -> Path:
    r2 = lambda v: round(float(v), 2) + 0.0  # no "-0.0"
    nodes = [
        {
            "x": r2(p[0]),
            "y": r2(p[1]),
            "z": 0.0,
            "width": None,
            "width_left": round(float(hw), 3),
            "width_right": round(float(hw), 3),
            "banking": round(float(bank), 4) + 0.0,
            "friction": 1.0,
            "surface_type": "Asphalt",
        }
        for p, hw, bank in zip(b["local"], b["half"], b["banking"])
    ]
    raceline = [{"x": r2(p[0]), "y": r2(p[1]), "z": 0.0} for p in b["race"]]
    data = {
        "name": spec["name"],
        "track_id": spec["track_id"],
        "nodes": nodes,
        "checkpoints": [],
        "spawn_points": [],
        "default_width": round(float(np.mean(b["half"]) * 2), 6),
        "closed_loop": True,
        "raceline": raceline,
        "drs_zones": [],
        "metadata": {**spec["metadata"], "length_m": round(b["length"], 3)},
    }
    out = TRACK_DIR / f"{stem}.yaml"
    out.write_text(
        yaml.safe_dump(data, sort_keys=False, allow_unicode=True, default_flow_style=False, width=1000),
        encoding="utf-8",
    )
    return out


def plot(b: dict, path: str) -> None:
    from PIL import Image, ImageDraw

    pts = b["local"]
    lo, hi = pts.min(0) - 50, pts.max(0) + 50
    scale = 2000 / max(hi - lo)
    size = tuple(int(v) for v in (hi - lo) * scale)
    img = Image.new("RGB", size, "white")
    d = ImageDraw.Draw(img)
    f = lambda p: ((p[0] - lo[0]) * scale, size[1] - (p[1] - lo[1]) * scale)
    d.line([f(p) for p in np.vstack([pts, pts[:1]])], fill=(0, 0, 0), width=3)
    d.line([f(p) for p in np.vstack([b["race"], b["race"][:1]])], fill=(220, 0, 0), width=1)
    for k in range(0, len(pts), 200):
        d.text(f(pts[k]), f"{k * STEP_M / 1000:.0f}km", fill=(0, 0, 200))
    img.save(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stem")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--plot")
    args = ap.parse_args()
    if args.stem not in LAPS:
        raise SystemExit(f"no lap spec for {args.stem}; add one to LAPS")
    b = build(args.stem)
    print(f"== {args.stem}: {b['report']}")
    if args.plot:
        plot(b, args.plot)
    if not args.dry_run:
        out = write_yaml(args.stem, LAPS[args.stem], b)
        print(f"   wrote {out.relative_to(TRACK_DIR.parent.parent.parent)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
