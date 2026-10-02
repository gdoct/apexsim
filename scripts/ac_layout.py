#!/usr/bin/env python3
"""Correct a native circuit's layout dossier from an Assetto Corsa survey.

The native circuits' roads are GPS traces with real elevation, and their
dossiers (`<Stem>.layout.json`, `scripts/osm_layout.py`) place stands,
buildings, the pit lane, bridges, landmarks and woods from OpenStreetMap.
An AC version of the same circuit places those things far better. This
script reads the AC track (`scripts/ac_import/features.py`), fits it onto
the native road, and writes the AC positions into the dossier as boxes,
lines and rings; `ats-dress` then lays kit props there. Nothing of AC's
geometry or textures is kept.

    python scripts/ac_layout.py --pairs                    # the pairs and their fits
    python scripts/ac_layout.py Zandvoort --dry-run        # report + overlay.png only
    python scripts/ac_layout.py Zandvoort                  # write the overlay, apply it
    python scripts/ac_layout.py --all [--dry-run]
    python scripts/ac_layout.py Zandvoort --unapply        # back to the OSM dossier

`--ac-root` (or `APEXSIM_AC_TRACKS`) is the AC `content/tracks` folder.

How the AC frame is put on the native road (docs/AC_LAYOUT_SURVEY.md):

1. **Rigid fit.** The AC centerline is fitted onto the native one by the
   dossier's own search (`osm_layout.coarse_fit`, then trimmed ICP). The
   two frames are far apart: rotated 50-170 degrees, start lines up to
   300 m apart.
2. **Rubber sheet.** A rigid fit leaves stretches where one trace is bent
   against the other: 10 m at Zandvoort's back section, 11 m at
   Interlagos. Locally the shapes agree, so it is a shift, and since
   `ats-dress` places everything relative to the *native* road, every AC
   point is moved by the displacement of the road beside it (a Gaussian
   blend of the centerline's displacements, sigma 60 m, faded out between
   150 and 300 m from the road).
3. **Reshape check.** Where the two roads disagree even after a local fit
   (a corner rebuilt between the AC version and today), features within
   50 m are held back for review, never applied.

Then the AC entries are matched against the dossier's (`merge`): a match
takes AC's geometry and keeps the dossier's name; AC-only entries are
added; dossier-only entries stay (the AC version may be older); manual
entries are never replaced. `<Stem>.layout.ac.json` keeps the result so
`osm_layout.py` re-applies it after a refetch.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import zlib
from pathlib import Path

import numpy as np

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

from osm_layout import (  # noqa: E402
    MANUAL_PIT_LANE,
    MANUAL_STRUCTURES,
    STAND_RANGE_M,
    STRUCTURE_RANGE_M,
    WOOD_RANGE_M,
    NearestGrid,
    Track,
    coarse_fit,
    densify,
    front_edge,
    icp,
    oriented_box,
    round_pts,
    simplify,
)
from track_dirs import REPO, track_dir  # noqa: E402

OVERLAY_FORMAT = "apex-track-layout-ac"
OVERLAY_VERSION = 1
PAIRS_FILE = REPO / "content" / "tracks" / "ac_pairs.json"
CACHE_DIR = REPO / ".cache" / "ac_layout"
REPORT_DIR = REPO / "build" / "ac_layout"
DEFAULT_AC_ROOT = Path(r"E:\SteamLibrary\steamapps\common\assettocorsa\content\tracks")

# Fit gates.
FIT_INLIER_M = 3.0
MIN_RIGID_COVER = 0.40
MIN_FIELD_COVER = 0.90
MAX_SCALE_ERROR = 0.005
MAX_RESHAPE_SHARE = 0.15
# The displacement field.
FIELD_SIGMA_M = 60.0               # at FIELD_SIGMA_M / FIELD_SIGMA_SHARE from the road and beyond
FIELD_SIGMA_MIN_M = 12.0           # on the road itself
FIELD_SIGMA_SHARE = 0.5
FIELD_FULL_M = 150.0
FIELD_ZERO_M = 300.0
FIELD_SMOOTH_SAMPLES = 12          # +-24 m running median along the lap
# Reshape detection: where even the field leaves the roads this far apart.
RESHAPE_RESIDUAL_M = 3.0
RESHAPE_MARGIN_M = 50.0
SHIFT_REPORT_M = 3.0
# Matching.
MATCH_OVERLAP = 0.3
#: Footprints this different in size are not the same thing: a big AC
#: outline over several dossier buildings (Suzuka's hairpin paddock came
#: out as one 130 m square) or a detail inside a mapped complex.
MIN_SIZE_RATIO = 0.2
DROP_OVERLAP = 0.1
CROSSING_MATCH_M = 30.0
LANDMARK_MATCH_M = 40.0
START_GANTRY_M = 40.0
LANDMARK_RANGE_M = 300.0
LANDMARK_KINDS = {"floodlight", "big_wheel", "screen", "camera_tower", "tower"}
# Woods.
WOOD_CELL_M = 10.0
WOOD_MIN_TREES = 2
WOOD_CLOSE_CELLS = 2
WOOD_MIN_AREA_M2 = 400.0
WOOD_SIMPLIFY_M = 6.0
# Pit lane.
#: AC's pit spline is the AI's route: it often rejoins the track before the
#: real exit lane ends (Interlagos' is 640 m against OSM's 1380). Unless it
#: reaches the dossier's lane's entry and exit within this, the dossier's
#: lane is kept and only AC's garage count is taken. `"pit": "ac"` in the
#: pairs file takes AC's lane regardless, `"pit": "native"` never does.
PIT_SPAN_TOLERANCE_M = 100.0
PIT_JOIN_GAP_M = 2.0
PIT_JOIN_BACK_M = 40.0
LEVEL_HEIGHT_M = 3.5
LAYERS = ("stands", "structures", "crossings", "landmarks", "woods")


class LayoutError(Exception):
    pass


# ------------------------------------------------------------------ helpers


def geometry_crc(yaml_path: Path) -> int:
    """CRC-32 of the centerline's plan (node x, y to the centimetre): what
    the fit depends on. Rewrites that leave the road where it is (DRS
    zones, metadata, banking, elevation) keep an overlay valid."""
    import yaml

    data = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    nodes = [[round(float(n["x"]), 2), round(float(n["y"]), 2)] for n in data["nodes"]]
    return zlib.crc32(json.dumps(nodes, separators=(",", ":")).encode()) & 0xFFFFFFFF


def closed(ring: np.ndarray) -> np.ndarray:
    ring = np.asarray(ring, dtype=float)
    return ring if len(ring) and np.allclose(ring[0], ring[-1]) else np.vstack([ring, ring[:1]])


def rect(centre, length: float, depth: float, yaw: float) -> np.ndarray:
    """Corners of a box, counter-clockwise."""
    c, s = math.cos(yaw), math.sin(yaw)
    u, v = np.array([c, s]) * length / 2, np.array([-s, c]) * depth / 2
    p = np.asarray(centre, dtype=float)
    return np.array([p - u - v, p + u - v, p + u + v, p - u + v])


def poly_area(p: np.ndarray) -> float:
    if len(p) < 3:
        return 0.0
    x, y = p[:, 0], p[:, 1]
    return float(abs(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))) / 2)


def clip_convex(subject: np.ndarray, clipper: np.ndarray) -> np.ndarray:
    """Sutherland-Hodgman: the part of `subject` inside convex `clipper`
    (both counter-clockwise)."""
    out = [tuple(p) for p in subject]
    n = len(clipper)
    for i in range(n):
        a, b = clipper[i], clipper[(i + 1) % n]
        inp, out = out, []
        if not inp:
            break

        def inside(p):
            return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]) >= 0

        def cut(p, q):
            x1, y1, x2, y2 = p[0], p[1], q[0], q[1]
            x3, y3, x4, y4 = a[0], a[1], b[0], b[1]
            den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
            if abs(den) < 1e-12:
                return q
            t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
            return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))

        for j in range(len(inp)):
            p, q = inp[j - 1], inp[j]
            if inside(q):
                if not inside(p):
                    out.append(cut(p, q))
                out.append(q)
            elif inside(p):
                out.append(cut(p, q))
    return np.array(out) if out else np.zeros((0, 2))


def overlap_ratio(a: np.ndarray, b: np.ndarray) -> float:
    """Intersection over the smaller of two convex polygons."""
    small = min(poly_area(a), poly_area(b))
    if small <= 0:
        return 0.0
    return poly_area(clip_convex(a, b)) / small


def footprint_of(entry: dict) -> np.ndarray:
    return rect(entry["centre"], float(entry.get("length_m", 1.0)), float(entry.get("depth_m", 1.0)),
                float(entry.get("yaw_rad", 0.0)))


def point_in_ring(pts: np.ndarray, ring: np.ndarray) -> np.ndarray:
    x, y = pts[:, 0], pts[:, 1]
    inside = np.zeros(len(pts), dtype=bool)
    j = len(ring) - 1
    for i in range(len(ring)):
        xi, yi = ring[i]
        xj, yj = ring[j]
        inside ^= ((yi > y) != (yj > y)) & (x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-12) + xi)
        j = i
    return inside


# ----------------------------------------------------------------- the pairs


def load_pairs() -> dict:
    data = json.loads(PAIRS_FILE.read_text(encoding="utf-8"))
    return data["pairs"]


def ac_root(arg: str | None) -> Path:
    for cand in (arg, os.environ.get("APEXSIM_AC_TRACKS")):
        if cand:
            return Path(cand)
    return DEFAULT_AC_ROOT


# ---------------------------------------------------------------- the survey


def survey(stem: str, pair: dict, root: Path, use_cache: bool) -> dict:
    """The AC survey as JSON, cached under .cache/ac_layout keyed by the
    source files' sizes and times and the survey code's version."""
    from ac_import import features, ini

    folder = root / pair["folder"]
    if not folder.is_dir():
        raise LayoutError(f"{stem}: AC folder {folder} not found (set --ac-root or APEXSIM_AC_TRACKS)")
    layout_name = pair.get("layout") or ""
    stamp = []
    lay = next((l for l in ini.find_layouts(folder) if l.name == layout_name), None)
    if lay is None:
        raise LayoutError(f"{stem}: {pair['folder']} has no layout {layout_name!r}")
    for fn in ini.model_files(lay):
        p = folder / fn
        if p.is_file():
            st = p.stat()
            stamp.append([fn, st.st_size, int(st.st_mtime)])
    for extra in ("fast_lane.ai", "pit_lane.ai"):
        p = lay.ai_dir / extra
        if p.is_file():
            st = p.stat()
            stamp.append([extra, st.st_size, int(st.st_mtime)])
    code = (SCRIPTS / "ac_import" / "features.py").read_bytes().replace(b"\r", b"")
    key = {"folder": pair["folder"], "layout": layout_name, "files": stamp,
           "version": features.SURVEY_VERSION, "code": zlib.crc32(code) & 0xFFFFFFFF}
    cache = CACHE_DIR / f"{stem}.survey.json"
    if use_cache and cache.is_file():
        data = json.loads(cache.read_text(encoding="utf-8"))
        if data.get("key") == key:
            return data["survey"]
    s = features.survey_layout(folder, layout_name or None).to_json()
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps({"key": key, "survey": s}), encoding="utf-8")
    return s


# ------------------------------------------------------------------- the fit


class Fit:
    """AC track frame -> native track frame: the rigid fit, then the
    displacement field that pulls the AC road onto the native one."""

    def __init__(self, track: Track, ac_centre: np.ndarray):
        self.track = track
        cloud = densify(ac_centre[::2], 2.0, closed=True)
        best = None
        for a0, t0 in coarse_fit(cloud, track.pts):
            a, t, rep = icp(cloud, track.pts, a0, t0, iters=30)
            if best is None or rep["centerline_covered"] > best[2]["centerline_covered"]:
                best = (a, t, rep)
        self.a, self.t, self.icp_report = best
        self.ac = cloud
        fitted = cloud @ self.a.T + self.t
        grid = NearestGrid(fitted, cell=12.0)
        d, j = grid.query(track.pts, max_rings=20)
        self.rigid_d = d
        # The field's anchors: for every native sample, the fitted AC point
        # beside it and how far the native road is from it.
        found = np.isfinite(d)
        anchor = np.where(found[:, None], fitted[j], track.pts)
        delta = np.where(found[:, None], track.pts - anchor, 0.0)
        self.delta = _running_median(delta, FIELD_SMOOTH_SAMPLES)
        self.anchor = anchor
        # Scale: the matched pairs' spread about their centres.
        ok = d < FIT_INLIER_M * 3
        pa, pb = fitted[j[ok]], track.pts[ok]
        ra = np.sqrt(((pa - pa.mean(0)) ** 2).sum(1).mean())
        rb = np.sqrt(((pb - pb.mean(0)) ** 2).sum(1).mean())
        self.scale = float(rb / ra) if ra > 0 else 1.0
        moved = fitted + self.field(fitted)
        d2, _ = NearestGrid(moved, cell=12.0).query(track.pts, max_rings=20)
        self.field_d = d2
        self.rigid_cover = float((d < FIT_INLIER_M).mean())
        self.field_cover = float((d2 < FIT_INLIER_M).mean())
        self.reshape = self._runs(d2 > RESHAPE_RESIDUAL_M)
        self.shift = self._runs(np.hypot(*self.delta.T) > SHIFT_REPORT_M)

    def rigid(self, p: np.ndarray) -> np.ndarray:
        return np.asarray(p, dtype=float).reshape(-1, 2) @ self.a.T + self.t

    def field(self, p: np.ndarray) -> np.ndarray:
        """Displacement at rigidly fitted points."""
        p = np.asarray(p, dtype=float).reshape(-1, 2)
        out = np.zeros_like(p)
        for k in range(0, len(p), 512):
            q = p[k:k + 512]
            d2 = ((q[:, None, :] - self.anchor[None, :, :]) ** 2).sum(-1)
            # A point on the road follows the road beside it; one further
            # out follows a wider stretch, so a building is not twisted by
            # the different shifts of a bend's two ends.
            sigma = np.clip(FIELD_SIGMA_SHARE * np.sqrt(d2.min(1)), FIELD_SIGMA_MIN_M, FIELD_SIGMA_M)
            w = np.exp(-d2 / (2 * sigma[:, None] ** 2))
            ws = w.sum(1)
            disp = (w @ self.delta) / np.maximum(ws, 1e-12)[:, None]
            dmin = np.sqrt(d2.min(1))
            fade = np.clip((FIELD_ZERO_M - dmin) / (FIELD_ZERO_M - FIELD_FULL_M), 0.0, 1.0)
            out[k:k + 512] = disp * fade[:, None]
        return out

    def apply(self, p: np.ndarray) -> np.ndarray:
        r = self.rigid(p)
        return r + self.field(r)

    @property
    def rotation(self) -> float:
        return math.atan2(self.a[1, 0], self.a[0, 0])

    def _runs(self, flag: np.ndarray) -> list[list[float]]:
        st = self.track.station
        out = []
        i, n = 0, len(flag)
        while i < n:
            if not flag[i]:
                i += 1
                continue
            j = i
            while j + 1 < n and flag[j + 1]:
                j += 1
            if st[j] - st[i] >= 50.0:
                out.append([round(float(st[i]), 1), round(float(st[j]), 1)])
            i = j + 1
        return out

    def report(self) -> dict:
        return {
            "rotation_deg": round(math.degrees(self.rotation), 2),
            "translation_m": [round(float(self.t[0]), 2), round(float(self.t[1]), 2)],
            "scale": round(self.scale, 4),
            "rigid_rmse_m": self.icp_report["rmse_m"],
            "rigid_within_3m": round(self.rigid_cover, 3),
            "field_within_3m": round(self.field_cover, 3),
            "median_shift_m": round(float(np.median(np.hypot(*self.delta.T))), 2),
            "max_shift_m": round(float(np.hypot(*self.delta.T).max()), 1),
            "shift_segments": self.shift,
            "reshape_segments": self.reshape,
        }

    def gate(self) -> list[str]:
        fails = []
        if self.rigid_cover < MIN_RIGID_COVER:
            fails.append(f"rigid fit explains {self.rigid_cover:.0%} of the lap within 3 m (< {MIN_RIGID_COVER:.0%})")
        if self.field_cover < MIN_FIELD_COVER:
            fails.append(f"the field explains {self.field_cover:.0%} within 3 m (< {MIN_FIELD_COVER:.0%})")
        if abs(self.scale - 1.0) > MAX_SCALE_ERROR:
            fails.append(f"scale {self.scale:.4f}: the AC track is not to scale")
        reshape = sum(b - a for a, b in self.reshape)
        if reshape > MAX_RESHAPE_SHARE * self.track.total:
            fails.append(f"{reshape:.0f} m of the lap is reshaped (> {MAX_RESHAPE_SHARE:.0%})")
        return fails

    def near_reshape(self, station: float) -> bool:
        total = self.track.total
        for a, b in self.reshape:
            lo, hi = a - RESHAPE_MARGIN_M, b + RESHAPE_MARGIN_M
            s = station
            if lo <= s <= hi or lo <= s + total <= hi or lo <= s - total <= hi:
                return True
        return False


def _running_median(v: np.ndarray, half: int) -> np.ndarray:
    n = len(v)
    idx = (np.arange(n)[:, None] + np.arange(-half, half + 1)[None, :]) % n
    return np.median(v[idx], axis=1)


# ------------------------------------------------------- the native road


def road_gap(track: Track, pts: np.ndarray) -> np.ndarray:
    """Distance past the native road's edge for each point (negative on
    the road), like `dress::footprint_road_gap` per probe."""
    _, lat = track.locate(pts)
    _, j = track.grid.query(pts)
    half = np.where(lat >= 0, track.width_left[j], track.width_right[j])
    return np.abs(lat) - half


def footprint_probes(entry: dict) -> np.ndarray:
    c, s = math.cos(entry["yaw_rad"]), math.sin(entry["yaw_rad"])
    u, v = np.array([c, s]), np.array([-s, c])
    a = np.linspace(-0.5, 0.5, 9) * entry["length_m"]
    b = np.linspace(-0.5, 0.5, 9) * entry["depth_m"]
    g = np.array(entry["centre"])[None, None, :] + a[:, None, None] * u + b[None, :, None] * v
    return g.reshape(-1, 2)


# ------------------------------------------------------------ the entries


def build_entries(stem: str, track: Track, fit: Fit, s: dict) -> tuple[dict, list[dict], dict]:
    """The AC features as dossier entries in the native frame, the ones
    held back for review, and counts for the report."""
    entries: dict = {k: [] for k in LAYERS}
    entries["pit_lane"] = None
    review: list[dict] = []
    counts: dict = {}
    total = track.total

    def hold(kind, entry, why):
        review.append({"layer": kind, "why": why, "entry": entry})

    lap_ac = float(s["lap_m"])
    centre_ac = np.array(s["centre"])[:, :2]
    for o in s["objects"]:
        cls = o["cls"]
        if cls not in ("stand", "structure", "crossing", "landmark"):
            continue
        ring = fit.apply(np.array(o["ring"]))
        ring_c = closed(ring)
        centre, length, depth, yaw = oriented_box(ring_c)
        near = float(track.grid.query(ring)[0].min())
        st, lat = track.locate(centre[None, :])
        side = "left" if lat[0] > 0 else "right"
        base = {
            "name": None,
            "source": "ac",
            "station_m": round(float(st[0]), 1),
            "side": side,
        }
        if cls == "crossing":
            k = int(round(o["station_m"] / lap_ac * len(centre_ac))) % len(centre_ac)
            p = fit.apply(centre_ac[k:k + 1])
            cs, _ = track.locate(p)
            s0 = float(cs[0])
            if min(s0, total - s0) < START_GANTRY_M:
                counts["start_gantry_skipped"] = counts.get("start_gantry_skipped", 0) + 1
                continue
            entry = {"name": None, "source": "ac", "station_m": round(s0, 1), "kind": o.get("kind", "road")}
            (hold("crossings", entry, "near a reshaped stretch") if fit.near_reshape(s0)
             else entries["crossings"].append(entry))
            continue
        if cls == "landmark":
            if o.get("kind") not in LANDMARK_KINDS or near > LANDMARK_RANGE_M:
                continue
            entry = {"kind": o["kind"], **base, "centre": round_pts(centre[None, :])[0]}
            (hold("landmarks", entry, "near a reshaped stretch") if fit.near_reshape(entry["station_m"])
             else entries["landmarks"].append(entry))
            continue
        entry = {
            **base,
            "offset_m": round(near, 1),
            "length_m": round(length, 1),
            "depth_m": round(depth, 1),
            "yaw_rad": round(yaw, 4),
            "centre": round_pts(centre[None, :])[0],
        }
        if cls == "stand":
            if near > STAND_RANGE_M:
                continue
            entry["covered"] = bool(o.get("covered", False))
            entry["front"] = round_pts(simplify(front_edge(ring_c, track), 2.0))
            layer = "stands"
        else:
            if near > STRUCTURE_RANGE_M:
                continue
            entry["levels"] = max(int(round(float(o["height_m"]) / LEVEL_HEIGHT_M)), 1)
            entry["osm_building"] = "ac"
            entry["area_m2"] = round(length * depth)
            layer = "structures"
        gap = float(road_gap(track, footprint_probes(entry)).min())
        if gap < 0.0:
            hold(layer, entry, f"overlaps the native road by {-gap:.1f} m")
        elif fit.near_reshape(entry["station_m"]):
            hold(layer, entry, "near a reshaped stretch")
        else:
            entries[layer].append(entry)

    entries["woods"] = woods_from_trees(track, fit, np.array(s["trees"]).reshape(-1, 3))
    pit, why = pit_lane_from(track, fit, s)
    if pit is not None and why is None:
        entries["pit_lane"] = pit
    elif pit is not None:
        hold("pit_lane", pit, why)
    for k in LAYERS:
        entries[k].sort(key=lambda e: e.get("station_m", 0.0))
    return entries, review, counts


def woods_from_trees(track: Track, fit: Fit, trees: np.ndarray) -> list[dict]:
    if len(trees) == 0:
        return []
    xy = fit.apply(trees[:, :2])
    d, _ = track.grid.query(xy, max_rings=40)
    keep = d <= WOOD_RANGE_M + 40.0
    xy, leaf = xy[keep], trees[keep, 2].astype(int)
    if len(xy) == 0:
        return []
    lo = xy.min(0) - WOOD_CELL_M * (WOOD_CLOSE_CELLS + 2)
    idx = ((xy - lo) // WOOD_CELL_M).astype(int)
    shape = tuple(idx.max(0) + WOOD_CLOSE_CELLS + 3)
    count = np.zeros(shape, dtype=int)
    np.add.at(count, (idx[:, 0], idx[:, 1]), 1)
    mask = count >= WOOD_MIN_TREES
    for _ in range(WOOD_CLOSE_CELLS):
        mask = _dilate(mask)
    for _ in range(WOOD_CLOSE_CELLS):
        mask = _erode(mask)
    labels, n = _label(mask)
    out = []
    for lab in range(1, n + 1):
        cells = labels == lab
        if cells.sum() * WOOD_CELL_M ** 2 < WOOD_MIN_AREA_M2:
            continue
        ring = _outline(cells)
        if ring is None:
            continue
        ring = ring * WOOD_CELL_M + lo
        ring = simplify(closed(ring), WOOD_SIMPLIFY_M)
        if len(ring) < 4 or poly_area(ring) < WOOD_MIN_AREA_M2:
            continue
        if float(track.grid.query(ring, max_rings=40)[0].min()) > WOOD_RANGE_M:
            continue
        inside = cells[idx[:, 0].clip(0, shape[0] - 1), idx[:, 1].clip(0, shape[1] - 1)]
        kinds = np.bincount(leaf[inside], minlength=3)
        kind = ["mixed", "broadleaved", "needleleaved"][int(kinds.argmax())] if kinds[1:].sum() > kinds[0] else "mixed"
        out.append({"leaf": kind, "source": "ac", "ring": round_pts(ring, 1)})
    return out


def _dilate(m):
    o = m.copy()
    o[1:] |= m[:-1]
    o[:-1] |= m[1:]
    o[:, 1:] |= m[:, :-1]
    o[:, :-1] |= m[:, 1:]
    return o


def _erode(m):
    o = m.copy()
    o[1:] &= m[:-1]
    o[:-1] &= m[1:]
    o[:, 1:] &= m[:, :-1]
    o[:, :-1] &= m[:, 1:]
    o[0, :] = o[-1, :] = o[:, 0] = o[:, -1] = False
    return o


def _label(mask):
    labels = np.zeros(mask.shape, dtype=int)
    n = 0
    for start in zip(*np.nonzero(mask)):
        if labels[start]:
            continue
        n += 1
        stack = [start]
        labels[start] = n
        while stack:
            i, j = stack.pop()
            for a, b in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                if 0 <= a < mask.shape[0] and 0 <= b < mask.shape[1] and mask[a, b] and not labels[a, b]:
                    labels[a, b] = n
                    stack.append((a, b))
    return labels, n


def _outline(cells: np.ndarray) -> np.ndarray | None:
    """The outer boundary of a 4-connected patch of cells, as cell-corner
    coordinates: every cell edge with outside on one side, chained."""
    edges: dict[tuple[int, int], list[tuple[int, int]]] = {}
    pad = np.pad(cells, 1)
    for i, j in zip(*np.nonzero(pad)):
        x, y = i - 1, j - 1
        # Counter-clockwise around the cell, keeping only edges facing out.
        if not pad[i, j - 1]:
            edges.setdefault((x, y), []).append((x + 1, y))
        if not pad[i + 1, j]:
            edges.setdefault((x + 1, y), []).append((x + 1, y + 1))
        if not pad[i, j + 1]:
            edges.setdefault((x + 1, y + 1), []).append((x, y + 1))
        if not pad[i - 1, j]:
            edges.setdefault((x, y + 1), []).append((x, y))
    loops = []
    while edges:
        start = next(iter(edges))
        loop = [start]
        cur = start
        while True:
            nxt = edges[cur].pop()
            if not edges[cur]:
                del edges[cur]
            if nxt == start:
                break
            loop.append(nxt)
            cur = nxt
            if cur not in edges:
                break
        loops.append(np.array(loop, dtype=float))
    if not loops:
        return None
    return max(loops, key=poly_area)


def pit_lane_from(track: Track, fit: Fit, s: dict) -> tuple[dict | None, str | None]:
    if not s.get("pit_lane"):
        return None, None
    lane = fit.apply(np.array(s["pit_lane"]))
    st, lat = track.locate(lane)
    side = "left" if float(np.median(lat)) > 0 else "right"
    ang = np.unwrap(st / track.total * 2 * math.pi)
    if float(np.median(np.diff(ang))) < 0:
        lane, st, lat = lane[::-1], st[::-1], lat[::-1]
    gap = road_gap(track, lane)
    sign = 1.0 if side == "left" else -1.0
    nodes = [p for p in lane]

    def edge_point(station, which):
        i = track.index_at(station)
        half = track.width_left[i] if which == "left" else track.width_right[i]
        h = track.heading[i]
        return track.pts[i] + (1.0 if which == "left" else -1.0) * half * np.array([-math.sin(h), math.cos(h)])

    joined = []
    if gap[0] > PIT_JOIN_GAP_M:
        nodes.insert(0, edge_point(float(st[0]) - PIT_JOIN_BACK_M, side))
        joined.append("entry")
    if gap[-1] > PIT_JOIN_GAP_M:
        nodes.append(edge_point(float(st[-1]) + PIT_JOIN_BACK_M, side))
        joined.append("exit")
    nodes = np.array(nodes)
    dense = densify(nodes, 12.0, closed=False)
    length = float(np.hypot(*np.diff(nodes, axis=0).T).sum())
    pit = {
        "side": side,
        "length_m": round(length, 1),
        "nodes": round_pts(dense),
        "source": "ac",
    }
    if s.get("pit_boxes"):
        pit["box_count"] = int(s["pit_boxes"])
    # The lane must stay on its own side and off the road between its ends.
    inner = lane[2:-2] if len(lane) > 6 else lane
    _, ilat = track.locate(inner)
    igap = road_gap(track, inner)
    if (np.sign(ilat) != sign).any() and (igap < 0).any():
        return pit, f"crosses the native road ({int((igap < 0).sum())} nodes on it)"
    if (igap < -1.0).sum() > 2:
        return pit, f"{int((igap < -1.0).sum())} nodes lie on the native road"
    if fit.near_reshape(float(st[0])) or fit.near_reshape(float(st[-1])):
        pit["joined"] = joined
        return pit, "an end is near a reshaped stretch"
    return pit, None


# ------------------------------------------------------------------ merging


def _manual_structure_names(stem: str) -> set[str]:
    return {m.get("name") for m in MANUAL_STRUCTURES.get(stem, []) if m.get("name")}


def _protected(stem: str, entry: dict, layer: str) -> bool:
    src = entry.get("source")
    if layer == "stands":
        return src not in (None, "osm", "ac")
    if layer == "structures":
        return entry.get("name") in _manual_structure_names(stem)
    return src == "authored"


def merge(layout: dict, overlay: dict, stem: str, track: Track | None = None) -> tuple[dict, dict]:
    """Apply an overlay's entries to a dossier (one without AC entries).
    Returns the new dossier and what happened, layer by layer."""
    out = json.loads(json.dumps(layout))
    entries = overlay["entries"]
    total = track.total if track is not None else None
    removed: dict = {k: [] for k in LAYERS}
    rep: dict = {}

    # Stands and buildings, matched across both layers by footprint.
    natives = [("stands", e) for e in out.get("stands", [])] + [("structures", e) for e in out.get("structures", [])]
    acs = [("stands", dict(e)) for e in entries.get("stands", [])] + \
          [("structures", dict(e)) for e in entries.get("structures", [])]
    nat_fp = [footprint_of(e) for _, e in natives]
    ac_fp = [footprint_of(e) for _, e in acs]
    drop_native: set[int] = set()
    drop_ac: set[int] = set()
    suggestions = []
    matched = 0
    moves: list[float] = []
    mismatched = []
    for ai, (alayer, ae) in enumerate(acs):
        clash = [ni for ni in range(len(natives)) if overlap_ratio(ac_fp[ai], nat_fp[ni]) >= DROP_OVERLAP]
        if any(min(poly_area(ac_fp[ai]), poly_area(nat_fp[ni])) / max(poly_area(ac_fp[ai]), poly_area(nat_fp[ni]))
               < MIN_SIZE_RATIO for ni in clash):
            drop_ac.add(ai)
            mismatched.append({"ac_centre": ae["centre"], "ac_layer": alayer,
                               "natives": [natives[ni][1].get("name") or natives[ni][1]["centre"] for ni in clash]})
            continue
        best = None
        for ni, (nlayer, ne) in enumerate(natives):
            r = overlap_ratio(ac_fp[ai], nat_fp[ni])
            if r <= 0.0:
                continue
            if best is None or r > best[0]:
                best = (r, ni)
            if r >= MATCH_OVERLAP or r >= DROP_OVERLAP:
                if _protected(stem, ne, nlayer):
                    if r >= DROP_OVERLAP:
                        drop_ac.add(ai)
                        suggestions.append({"manual": ne.get("name"), "ac_centre": ae["centre"],
                                            "overlap": round(r, 2)})
                    continue
                drop_native.add(ni)
        if ai in drop_ac or best is None:
            continue
        r, ni = best
        if r >= MATCH_OVERLAP:
            nlayer, ne = natives[ni]
            if ne.get("name") and not ae.get("name"):
                ae["name"] = ne["name"]
            if alayer == "structures" and nlayer == "stands" and track is not None:
                # OSM tags it as seating: trust that over AC's shape (the
                # survey's own seating test misses roofed or flat stands).
                ring = closed(footprint_of(ae))
                ae = {k: v for k, v in ae.items() if k not in ("levels", "osm_building", "area_m2")}
                ae["covered"] = bool(ne.get("covered", False))
                ae["front"] = round_pts(simplify(front_edge(ring, track), 2.0))
                alayer = "stands"
            if alayer == "structures" and nlayer == "structures" and ne.get("osm_building"):
                ae["osm_building"] = ne["osm_building"]
            if alayer == "structures" and ne.get("levels"):
                ae["levels"] = ne["levels"]
            matched += 1
            moves.append(float(np.hypot(*(np.array(ae["centre"]) - np.array(ne["centre"])))))
            acs[ai] = (alayer, ae)
    n_stands = len(out.get("stands", []))
    for ni in sorted(drop_native):
        nlayer, ne = natives[ni]
        index = ni if nlayer == "stands" else ni - n_stands
        removed[nlayer].append({"index": index, "entry": ne})
    out["stands"] = [e for i, (l, e) in enumerate(natives) if l == "stands" and i not in drop_native]
    out["structures"] = [e for i, (l, e) in enumerate(natives) if l == "structures" and i not in drop_native]
    for ai, (alayer, ae) in enumerate(acs):
        if ai not in drop_ac:
            out[alayer].append(ae)
    for k in ("stands", "structures"):
        out[k].sort(key=lambda e: e["station_m"])
    rep["stands_structures"] = {
        "matched": matched,
        "median_move_m": round(float(np.median(moves)), 1) if moves else None,
        "max_move_m": round(float(max(moves)), 1) if moves else None,
        "ac_added": len(acs) - len(drop_ac) - matched,
        "native_removed": {k: len(removed[k]) for k in ("stands", "structures")},
        "kept_for_manual": suggestions,
        "size_mismatch_kept_native": mismatched,
    }
    def sdist(a, b):
        d = abs(a - b)
        return min(d, total - d) if total else d

    # Crossings.
    cr_native = out.get("crossings", [])
    keep, added, matched_c = [], [], 0
    used = set()
    for ae in entries.get("crossings", []):
        hit = None
        for ni, ne in enumerate(cr_native):
            if sdist(ne["station_m"], ae["station_m"]) <= CROSSING_MATCH_M:
                hit = ni
                break
        if hit is None:
            added.append(dict(ae))
            continue
        ne = cr_native[hit]
        if _protected(stem, ne, "crossings"):
            continue
        used.add(hit)
        merged = dict(ne)
        merged["station_m"] = ae["station_m"]
        merged["source"] = "ac"
        merged.setdefault("kind", ae.get("kind"))
        added.append(merged)
        removed["crossings"].append({"index": hit, "entry": ne})
        matched_c += 1
    keep = [e for i, e in enumerate(cr_native) if i not in used]
    out["crossings"] = sorted(keep + added, key=lambda e: e["station_m"])
    rep["crossings"] = {"matched": matched_c, "ac_added": len(added) - matched_c}

    # Landmarks.
    lm_native = out.get("landmarks", [])
    used, added, matched_l = set(), [], 0
    for ae in entries.get("landmarks", []):
        hit = None
        for ni, ne in enumerate(lm_native):
            if ne.get("kind") == ae["kind"] and math.hypot(*(np.array(ne["centre"]) - np.array(ae["centre"]))) \
                    <= LANDMARK_MATCH_M:
                hit = ni
                break
        if hit is None:
            added.append(dict(ae))
            continue
        ne = lm_native[hit]
        if _protected(stem, ne, "landmarks"):
            continue
        used.add(hit)
        merged = dict(ne)
        for k in ("centre", "station_m", "side"):
            merged[k] = ae[k]
        merged["source"] = "ac"
        added.append(merged)
        removed["landmarks"].append({"index": hit, "entry": ne})
        matched_l += 1
    out["landmarks"] = sorted([e for i, e in enumerate(lm_native) if i not in used] + added,
                              key=lambda e: e["station_m"])
    rep["landmarks"] = {"matched": matched_l, "ac_added": len(added) - matched_l}

    # Woods: a union.
    ac_woods = entries.get("woods", [])
    out["woods"] = list(out.get("woods", [])) + [dict(w) for w in ac_woods]
    rep["woods"] = {"ac_added": len(ac_woods), "native_kept": len(layout.get("woods", []))}

    # The pit lane.
    pit = entries.get("pit_lane")
    native_pit = out.get("pit_lane")
    pit_touched = False
    if pit is None:
        rep["pit_lane"] = "kept (no AC lane)"
    elif stem in MANUAL_PIT_LANE:
        rep["pit_lane"] = "kept (manual)"
    else:
        mode = overlay.get("pit_mode", "auto")
        if mode == "native" and native_pit:
            span = "the pairs file keeps the dossier's lane"
        elif mode == "auto":
            span = _pit_span_check(native_pit, pit, track)
        else:
            span = None
        pit_touched = True
        if span is None:
            out["pit_lane"] = dict(pit)
            rep["pit_lane"] = "replaced" if native_pit else "added"
        else:
            kept = dict(native_pit)
            if pit.get("box_count"):
                kept["box_count"] = pit["box_count"]
            out["pit_lane"] = kept
            rep["pit_lane"] = f"kept the dossier's lane ({span}); garage count from AC"

    out["ac_survey"] = {
        "overlay": f"{stem}.layout.ac.json",
        "removed": removed,
        "pit_lane_before": native_pit if pit_touched else None,
        "pit_lane_touched": pit_touched,
        "attribution_before": layout.get("attribution", ""),
    }
    return out, rep


def _pit_span_check(native: dict | None, ac: dict, track: Track | None) -> str | None:
    """None when AC's lane may replace the dossier's: there is none, or
    AC's reaches the dossier's entry and exit. Otherwise why not."""
    if not native or track is None:
        return None
    total = track.total
    ns, _ = track.locate(np.array(native["nodes"])[[0, -1]])
    as_, _ = track.locate(np.array(ac["nodes"])[[0, -1]])

    def ahead(a, b):  # how far a is past b along the lap, signed
        return (a - b + total / 2) % total - total / 2

    late_entry = ahead(as_[0], ns[0])
    early_exit = -ahead(as_[1], ns[1])
    why = []
    if late_entry > PIT_SPAN_TOLERANCE_M:
        why.append(f"AC's entry {late_entry:.0f} m later")
    if early_exit > PIT_SPAN_TOLERANCE_M:
        why.append(f"AC's exit {early_exit:.0f} m earlier")
    return ", ".join(why) or None


def unapply(layout: dict) -> dict:
    """The dossier as it was before an overlay was applied."""
    info = layout.get("ac_survey")
    if not info:
        return layout
    out = {k: v for k, v in layout.items() if k != "ac_survey"}
    for k in LAYERS:
        kept = [e for e in out.get(k, []) if e.get("source") != "ac"]
        # Back where each was: the dossier's own order is untouched by a
        # merge (it only filters it and sorts by station, stably).
        for item in sorted(info["removed"].get(k, []), key=lambda r: r["index"]):
            kept.insert(min(item["index"], len(kept)), item["entry"])
        out[k] = kept
    if info.get("pit_lane_touched") or (out.get("pit_lane") or {}).get("source") == "ac":
        out["pit_lane"] = info.get("pit_lane_before")
    out["attribution"] = info.get("attribution_before", out.get("attribution"))
    # Keep the key order osm_layout writes.
    return {k: out[k] for k in layout if k in out} | {k: v for k, v in out.items() if k not in layout}


def apply_overlay_file(stem: str, layout: dict, strict: bool = True) -> dict:
    """`osm_layout.py`'s last stage: re-apply a checked-in overlay to a
    freshly extracted dossier."""
    path = track_dir(stem) / f"{stem}.layout.ac.json"
    if not path.is_file():
        return layout
    overlay = json.loads(path.read_text(encoding="utf-8"))
    crc = geometry_crc(track_dir(stem) / f"{stem}.yaml")
    if overlay.get("native_geometry_crc") != crc:
        msg = (f"{stem}: {path.name} was fitted to another centerline than {stem}.yaml's "
               f"(crc {overlay.get('native_geometry_crc')} != {crc}); the dossier is written without it: "
               f"re-run python scripts/ac_layout.py {stem}")
        if strict:
            raise SystemExit(msg)
        print("   warning:", msg)
        return layout
    merged, rep = merge(unapply(layout), overlay, stem, Track(stem))
    print(f"   AC overlay: {json.dumps(rep, ensure_ascii=False)[:300]}")
    return merged


# ------------------------------------------------------------------ the run


def process(stem: str, pair: dict, root: Path, dry_run: bool, use_cache: bool, force: bool) -> dict:
    tdir = track_dir(stem)
    yaml_path = tdir / f"{stem}.yaml"
    layout_path = tdir / f"{stem}.layout.json"
    if not layout_path.is_file():
        raise LayoutError(f"{stem}: no dossier to correct ({layout_path.name}); run osm_layout.py first")
    print(f"== {stem} <- {pair['folder']}{'/' + pair['layout'] if pair.get('layout') else ''}")
    s = survey(stem, pair, root, use_cache)
    track = Track(stem)
    fit = Fit(track, np.array(s["centre"])[:, :2])
    fr = fit.report()
    print(f"   fit: rot {fr['rotation_deg']} deg, scale {fr['scale']}, rigid {fr['rigid_within_3m']:.0%}"
          f" -> field {fr['field_within_3m']:.0%} within 3 m, median shift {fr['median_shift_m']} m"
          f" (max {fr['max_shift_m']}), reshaped {fr['reshape_segments']}")
    fails = fit.gate()
    entries, review, counts = build_entries(stem, track, fit, s)
    layout = json.loads(layout_path.read_text(encoding="utf-8"))
    pristine = unapply(layout)
    overlay = {
        "format": OVERLAY_FORMAT,
        "version": OVERLAY_VERSION,
        "source_track": f"{stem}.yaml",
        "native_geometry_crc": geometry_crc(yaml_path),
        "ac": {"folder": pair["folder"], "layout": pair.get("layout") or "", "origin": pair.get("origin"),
               "files": s["files"]},
        "fit": fr,
        "field": {"sigma_m": [FIELD_SIGMA_MIN_M, FIELD_SIGMA_M], "full_m": FIELD_FULL_M, "zero_m": FIELD_ZERO_M},
        "pit_mode": pair.get("pit", "auto"),
        "entries": entries,
        "review": review,
    }
    merged, rep = merge(pristine, overlay, stem, track)
    report = {
        "stem": stem,
        "pair": pair,
        "fit": fr,
        "gate": fails or "pass",
        "survey": {
            "objects": {c: sum(1 for o in s["objects"] if o["cls"] == c)
                        for c in sorted({o["cls"] for o in s["objects"]})},
            "trees": len(s["trees"]),
            "pit_boxes": s.get("pit_boxes"),
            "warnings": s.get("warnings", []),
            **counts,
        },
        "entries": {k: len(v) for k, v in entries.items() if isinstance(v, list)},
        "review": [{"layer": r["layer"], "why": r["why"], "station_m": r["entry"].get("station_m")} for r in review],
        "merge": rep,
        "objects": [{k: v for k, v in o.items() if k != "ring"} | {"centre": np.round(np.mean(o["ring"], axis=0), 1).tolist()
                                                                  if o["ring"] else None}
                    for o in s["objects"] if o["cls"] != "ignored"],
    }
    out_dir = REPORT_DIR / stem
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "report.json").write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    try:
        draw(out_dir / "overlay.png", stem, track, fit, s, pristine, merged, review)
    except ImportError:
        print("   (Pillow missing: no overlay.png)")
    print(f"   entries: {report['entries']}, review {len(review)}; merge {json.dumps(rep['stands_structures'])[:200]}")
    print(f"   crossings {rep['crossings']}, landmarks {rep['landmarks']}, woods {rep['woods']}, pit lane {rep['pit_lane']}")
    if fails:
        print("   GATE FAILED: " + "; ".join(fails))
        if not force:
            print(f"   nothing written (report in {out_dir.relative_to(REPO)})")
            return report
    if dry_run:
        print(f"   dry run: report and overlay.png in {out_dir.relative_to(REPO)}")
        return report
    (tdir / f"{stem}.layout.ac.json").write_text(json.dumps(overlay, indent=1, ensure_ascii=False) + "\n",
                                                 encoding="utf-8")
    layout_path.write_text(json.dumps(merged, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"   wrote {stem}.layout.ac.json and {stem}.layout.json")
    return report


# ----------------------------------------------------------------- drawing


def draw(path: Path, stem: str, track: Track, fit: Fit, s: dict, before: dict, after: dict,
         review: list[dict]) -> None:
    from PIL import Image, ImageDraw

    pts = track.pts
    lo, hi = pts.min(0) - 300.0, pts.max(0) + 300.0
    size = 2400
    k = size / float(max(hi - lo))
    w, h = int((hi - lo)[0] * k) + 1, int((hi - lo)[1] * k) + 1
    img = Image.new("RGB", (w, h), (250, 250, 247))
    dr = ImageDraw.Draw(img)

    def px(p):
        p = np.asarray(p, dtype=float).reshape(-1, 2)
        return [((x - lo[0]) * k, h - (y - lo[1]) * k) for x, y in p]

    for wd in after.get("woods", []):
        col = (205, 230, 200) if wd.get("source") == "ac" else (225, 238, 220)
        if len(wd["ring"]) >= 3:
            dr.polygon(px(wd["ring"]), fill=col, outline=(150, 190, 140))
    # Road, with the shifted and reshaped stretches.
    half = (track.width_left + track.width_right) / 2
    dr.line(px(np.vstack([pts, pts[:1]])), fill=(150, 150, 150), width=max(int(float(half.mean()) * 2 * k), 2))
    for segs, col in ((fit.shift, (90, 140, 230)), (fit.reshape, (240, 140, 30))):
        for a, b in segs:
            m = (track.station >= a) & (track.station <= b)
            if m.sum() > 1:
                dr.line(px(pts[m]), fill=col, width=6)
    ac_line = fit.apply(np.array(s["centre"])[::4, :2])
    dr.line(px(ac_line), fill=(60, 60, 200), width=1)
    for o in s["objects"]:
        if o["cls"] in ("ignored",):
            continue
        dr.polygon(px(fit.apply(np.array(o["ring"]))), outline=(170, 170, 220))
    for layer, col in (("stands", (200, 40, 40)), ("structures", (200, 40, 40))):
        for e in before.get(layer, []):
            dr.polygon(px(footprint_of(e)), outline=col)
    for layer in ("stands", "structures"):
        for e in after.get(layer, []):
            if e.get("source") != "ac":
                continue
            dr.polygon(px(footprint_of(e)), outline=(20, 150, 40))
            if e.get("front"):
                dr.line(px(e["front"]), fill=(20, 150, 40), width=3)
            if e.get("name"):
                x, y = px(e["centre"])[0]
                dr.text((x + 3, y + 3), str(e["name"])[:24], fill=(10, 90, 20))
    for r in review:
        e = r["entry"]
        if "centre" in e and "length_m" in e:
            dr.polygon(px(footprint_of(e)), outline=(230, 180, 0))
    for which, col in ((before, (200, 40, 40)), (after, (20, 150, 40))):
        pit = which.get("pit_lane")
        if pit and pit.get("nodes"):
            dr.line(px(pit["nodes"]), fill=col, width=3)
    for c in after.get("crossings", []):
        p = pts[track.index_at(c["station_m"])]
        x, y = px(p)[0]
        col = (20, 150, 40) if c.get("source") == "ac" else (200, 40, 40)
        dr.rectangle([x - 5, y - 5, x + 5, y + 5], outline=col, width=2)
    for l in after.get("landmarks", []):
        x, y = px(l["centre"])[0]
        col = (20, 150, 40) if l.get("source") == "ac" else (200, 40, 40)
        dr.ellipse([x - 5, y - 5, x + 5, y + 5], outline=col, width=2)
    for st in range(0, int(track.total), 500):
        x, y = px(pts[track.index_at(st)])[0]
        dr.text((x + 4, y - 12), f"{st}", fill=(80, 80, 80))
    dr.text((10, 10), f"{stem}: grey native road, blue AC line, blue/orange shifted/reshaped stretches; red = dossier "
                      f"before, green = from AC, yellow = held for review", fill=(0, 0, 0))
    img.save(path)


# --------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks", nargs="*", help="native track stems, e.g. Zandvoort")
    ap.add_argument("--all", action="store_true", help="every pair in content/tracks/ac_pairs.json")
    ap.add_argument("--pairs", action="store_true", help="list the pairs and check their AC folders")
    ap.add_argument("--dry-run", action="store_true", help="report and overlay.png only")
    ap.add_argument("--unapply", action="store_true", help="remove the AC entries from the dossier")
    ap.add_argument("--force", action="store_true", help="write even when a fit gate fails")
    ap.add_argument("--no-cache", action="store_true", help="survey the AC track again")
    ap.add_argument("--ac-root", help="AC content/tracks folder (default: APEXSIM_AC_TRACKS)")
    args = ap.parse_args()

    pairs = load_pairs()
    root = ac_root(args.ac_root)
    if args.pairs:
        for stem, pair in pairs.items():
            folder = root / pair["folder"]
            have = "ok" if folder.is_dir() else "MISSING"
            applied = (track_dir(stem) / f"{stem}.layout.ac.json").is_file()
            print(f"  {stem:<14} {pair['folder']}{'/' + pair['layout'] if pair.get('layout') else '':<28}"
                  f" {pair.get('origin', '?'):<6} {have:<8} {'applied' if applied else ''}")
        return 0
    stems = list(pairs) if args.all else args.tracks
    if not stems:
        ap.print_help()
        return 1
    failed = 0
    for stem in stems:
        if args.unapply:
            path = track_dir(stem) / f"{stem}.layout.json"
            layout = json.loads(path.read_text(encoding="utf-8"))
            if "ac_survey" in layout and not args.dry_run:
                path.write_text(json.dumps(unapply(layout), indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
                ov = track_dir(stem) / f"{stem}.layout.ac.json"
                print(f"{stem}: AC entries removed from {path.name}; {ov.name} left in place")
            continue
        if stem not in pairs:
            print(f"{stem}: not in {PAIRS_FILE.name}")
            failed += 1
            continue
        try:
            rep = process(stem, pairs[stem], root, args.dry_run, not args.no_cache, args.force)
            if rep.get("gate") != "pass":
                failed += 1
        except LayoutError as e:
            print(f"   {e}")
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
