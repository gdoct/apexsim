#!/usr/bin/env python3
"""Build a circuit's real-world layout dossier from OpenStreetMap.

The centerlines under ``content/tracks/real`` come from public GPS traces,
so the road itself is where it should be -- but everything *beside* it
(which side the pit lane runs, where the grandstands stand and what they
are called, the pit building, the landmarks, where the trees actually are)
was invented by a procedural enrichment pass.  The scenes therefore look
like a circuit but not like *the* circuit.

This script fetches each circuit's surroundings from OpenStreetMap (public
data, ODbL), georeferences them onto the track's own coordinate frame, and
writes ``content/tracks/real/<Stem>.layout.json``: a small, readable,
reviewable dossier of real-world facts.  Turning a dossier into props is
``ats-dress``'s job (Rust, deterministic, tested); nothing here writes a
``.ats``.

    python scripts/osm_layout.py Zandvoort Spa   # refresh two dossiers
    python scripts/osm_layout.py --all           # every circuit with a bbox
    python scripts/osm_layout.py Monza --offline # reuse the cached extract

Georeferencing is a fit, not an assumption: the OSM raceway ways are
matched onto the YAML centerline by an FFT rotation/translation search
followed by trimmed ICP, and a dossier is only written when the fit
actually covers the centerline.  The ``fit`` block reports it.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np
import yaml

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"
CACHE_DIR = REPO / "content" / "tracks" / "osm-cache"
OSM_API = "https://api.openstreetmap.org/api/0.6/map.json"
R_EARTH = 6378137.0
ATTRIBUTION = (
    "OpenStreetMap contributors, ODbL 1.0 "
    "(https://www.openstreetmap.org/copyright)"
)
LAYOUT_FORMAT = "apex-track-layout"
LAYOUT_VERSION = 1

# Bounding boxes (min_lon, min_lat, max_lon, max_lat) around each circuit,
# split into tiles where the OSM API's 50k-node ceiling bites (Le Mans runs
# through a town and its forests).
BBOXES: dict[str, list[tuple[float, float, float, float]]] = {
    "Zandvoort": [(4.528, 52.383, 4.552, 52.396)],
    "Spa": [(5.955, 50.428, 5.988, 50.448)],
    "Monza": [(9.275, 45.612, 9.300, 45.635)],
    "Silverstone": [(-1.035, 52.063, -0.995, 52.083)],
    "LeMans": [
        (0.180, 47.910, 0.240, 47.945),
        (0.180, 47.940, 0.215, 47.960),
        (0.215, 47.940, 0.240, 47.960),
        (0.180, 47.958, 0.212, 47.966),
        (0.212, 47.958, 0.240, 47.966),
    ],
}

# Grandstands OSM does not have, from each circuit's own published
# seating map.  A run of seats is given as a station span and a side, and
# the script lays its front along the centerline there -- so a stand round
# a bend comes out curved, exactly like one traced from a building
# outline.  `gap_m` is the distance from the road edge to the front row.
MANUAL_STANDS: dict[str, list[dict]] = {
    # Only the Hoofdtribune is in OSM.  The rest are from the circuit's
    # own tribune map (https://dutchgp.com/en/tribunes/,
    # https://grandprixguides.com/circuit/netherlands): the Arena and
    # Hairpin stands are on the infield side, Eastside faces Arena-In
    # across the track, Ben Pon looks at the exit of the banked final
    # corner from the start of the straight.
    # Spa's own grandstand map (https://www.spa-francorchamps.be/) puts
    # seats at Les Combes, Bruxelles, Pouhon and the Bus Stop that OSM has
    # no outline for; each sits on the outside of its bend.
    "Spa": [
        dict(name="Tribune Les Combes", from_m=2320, to_m=2470, side="outside", depth_m=14),
        dict(name="Tribune Bruxelles", from_m=2990, to_m=3140, side="outside", depth_m=14),
        dict(name="Tribune Pouhon", from_m=3890, to_m=4060, side="outside", depth_m=14),
        dict(name="Tribune Bus Stop", from_m=6650, to_m=6820, side="outside", depth_m=16),
    ],
    # The Automobile Club de l'Ouest numbers its tribunes; those along the
    # pit straight are in OSM, the ones out on the public-road section are
    # not (https://www.lemans.org/ spectator map).
    "LeMans": [
        dict(name="Tribune Tertre Rouge", from_m=1840, to_m=1990, side="outside", depth_m=14),
        dict(name="Tribune Mulsanne", from_m=7500, to_m=7660, side="outside", depth_m=14),
        dict(name="Tribune Indianapolis", from_m=8950, to_m=9090, side="outside", depth_m=14),
        dict(name="Tribune Arnage", from_m=10060, to_m=10200, side="outside", depth_m=14),
        dict(name="Tribune Virage Porsche", from_m=11500, to_m=11700, side="outside", depth_m=14),
    ],
    "Zandvoort": [
        dict(name="Pit Grandstand", from_m=70, to_m=230, side="left", depth_m=18),
        dict(name="Tarzan", from_m=330, to_m=500, side="left", depth_m=26, covered=False),
        dict(name="Tarzan-In", from_m=380, to_m=470, side="right", depth_m=12),
        dict(name="Hairpin", from_m=3150, to_m=3330, side="right", depth_m=14),
        dict(name="Eastside", from_m=3330, to_m=3800, side="left", depth_m=16),
        dict(name="Arena", from_m=3300, to_m=3760, side="right", depth_m=16),
        dict(name="Ben Pon", from_m=4030, to_m=4240, side="left", depth_m=20),
    ],
}

# Landmarks that span the road and are not in OSM as bridges.
MANUAL_CROSSINGS: dict[str, list[dict]] = {
    # The Dunlop bridge has arched over the climb out of the start straight
    # since 1932 and is the one structure everyone draws when they draw Le
    # Mans; OSM maps the footbridges beside it but not the arch itself.
    # The tyre bridge: the arch shaped like a tyre that the lap passes under
    # on the crest just after the Dunlop chicane. OSM has it (as
    # man_made=bridge, "Passerelle Goodyear") but only as a footbridge, so it
    # would come out as a plain truss; this entry takes its place and wears
    # the kit's tyre brand, the real sponsor not being in the kit's signage.
    "LeMans": [dict(name="Tyre bridge", station_m=1043.0, kind="arch", brand="piretti")],
}

# Point features OSM does not carry, but that are part of what the place
# looks like on a race weekend.
MANUAL_LANDMARKS: dict[str, list[dict]] = {
    # The airship over the pits is as much a part of the 24 Hours as the
    # fairground at the Esses (which OSM does have, as a big wheel).
    "LeMans": [
        # The airship loiters over the Dunlop crest, where the pit straight,
        # its tribunes and every car on the grid look straight at it, and it
        # is turned broadside to the start line so it shows its length (and
        # its brand) rather than its nose. 150 m up keeps it inside a
        # cockpit's view from the grid.
        dict(kind="blimp", name="Airship", station_m=900.0, side="right", offset_m=90.0,
             altitude_m=150.0, broadside_to_m=0.0, brand="piretti"),
        # The fun fair inside the Esses, which runs all through the night.
        dict(kind="big_wheel", station_m=1010.0, side="right", offset_m=110.0),
    ],
}

# How far from the road a feature still belongs to the circuit.
STAND_RANGE_M = 260.0
STRUCTURE_RANGE_M = 140.0
WOOD_RANGE_M = 260.0


# ---------------------------------------------------------------- fetching


def fetch(stem: str, offline: bool) -> list[Path]:
    import urllib.request

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out = []
    for i, box in enumerate(BBOXES[stem]):
        path = CACHE_DIR / f"{stem}.{i}.json"
        if not path.exists():
            if offline:
                raise SystemExit(f"{stem}: no cached extract at {path}")
            url = f"{OSM_API}?bbox={box[0]},{box[1]},{box[2]},{box[3]}"
            print(f"  fetching {url}")
            with urllib.request.urlopen(url, timeout=300) as r:
                body = r.read()
            if not body.lstrip().startswith(b"{"):
                raise SystemExit(f"{stem}: OSM refused tile {i}: {body[:200]!r}")
            path.write_bytes(body)
            time.sleep(1.0)
        out.append(path)
    return out


# ---------------------------------------------------------------- geometry


def enu(lon, lat, lon0, lat0):
    return (
        math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0)),
        math.radians(lat - lat0) * R_EARTH,
    )


def densify(pts: np.ndarray, step: float, closed: bool) -> np.ndarray:
    out = []
    n = len(pts)
    for i in range(n if closed else n - 1):
        a, b = pts[i], pts[(i + 1) % n]
        d = float(np.hypot(*(b - a)))
        for j in range(max(int(d / step), 1)):
            out.append(a + (b - a) * (j / max(int(d / step), 1)))
    out.append(pts[0] if closed else pts[-1])
    return np.array(out)


def simplify(pts: np.ndarray, tol: float) -> np.ndarray:
    """Douglas-Peucker, so a wood's outline costs a handful of vertices."""
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    ab = b - a
    n = float(np.hypot(*ab))
    if n < 1e-6:
        d = np.hypot(*(pts - a).T)
    else:
        d = np.abs(np.cross(np.tile(ab, (len(pts), 1)), pts - a)) / n
    i = int(d.argmax())
    if d[i] <= tol:
        return np.array([a, b])
    return np.vstack([simplify(pts[: i + 1], tol)[:-1], simplify(pts[i:], tol)])


class NearestGrid:
    """Uniform-grid nearest neighbour.  Every point set here is a densely
    sampled polyline, so a 3x3 cell probe at the sample spacing finds the
    true nearest point and the whole fit stays linear in the node count."""

    def __init__(self, pts: np.ndarray, cell: float = 12.0):
        self.pts = pts
        self.cell = cell
        self.lo = pts.min(0) - cell
        self.buckets: dict[tuple[int, int], list[int]] = {}
        for i, (cx, cy) in enumerate(((pts - self.lo) / cell).astype(int)):
            self.buckets.setdefault((int(cx), int(cy)), []).append(i)

    def query(self, q: np.ndarray, max_rings: int = 64) -> tuple[np.ndarray, np.ndarray]:
        """Nearest point and its distance.  The ring search widens until it
        finds something, so this answers for a wood 200 m off the road as
        well as for a point on it."""
        q = np.asarray(q, dtype=float).reshape(-1, 2)
        idx = ((q - self.lo) / self.cell).astype(int)
        best_d = np.full(len(q), np.inf)
        best_i = np.zeros(len(q), dtype=int)
        for i, (cx, cy) in enumerate(idx):
            cx, cy = int(cx), int(cy)
            cand: list[int] = []
            hit_ring = None
            for r in range(max_rings + 1):
                for dx in range(-r, r + 1):
                    for dy in range(-r, r + 1):
                        if r and max(abs(dx), abs(dy)) != r:
                            continue
                        cand += self.buckets.get((cx + dx, cy + dy), ())
                if cand and hit_ring is None:
                    hit_ring = r
                # one ring past the first hit, so a diagonal neighbour wins
                if hit_ring is not None and r > hit_ring:
                    break
            if not cand:
                continue
            c = np.asarray(cand)
            d = ((self.pts[c] - q[i]) ** 2).sum(1)
            j = int(d.argmin())
            best_d[i] = math.sqrt(d[j])
            best_i[i] = c[j]
        return best_d, best_i


# ------------------------------------------------------------- the track


class Track:
    """The YAML centerline, resampled, with station / side lookups."""

    STEP = 2.0

    def __init__(self, stem: str):
        self.stem = stem
        self.data = yaml.safe_load(
            (TRACK_DIR / f"{stem}.yaml").read_text(encoding="utf-8")
        )
        nodes = self.data["nodes"]
        raw = np.array([[n["x"], n["y"]] for n in nodes])
        dw = self.data.get("default_width", 10.0)
        wl = np.array([n.get("width_left") or dw / 2 for n in nodes])
        wr = np.array([n.get("width_right") or dw / 2 for n in nodes])
        self.closed = bool(self.data.get("closed_loop", True))
        self.pts = densify(raw, self.STEP, self.closed)
        # widths resampled onto the same stations
        seg = np.hypot(*np.diff(np.vstack([raw, raw[:1]]) if self.closed else raw, axis=0).T)
        s_raw = np.concatenate([[0.0], np.cumsum(seg)])[: len(raw)]
        self.station = np.concatenate(
            [[0.0], np.cumsum(np.hypot(*np.diff(self.pts, axis=0).T))]
        )
        self.total = float(self.station[-1])
        self.width_left = np.interp(self.station, s_raw, wl, period=self.total)
        self.width_right = np.interp(self.station, s_raw, wr, period=self.total)
        d = np.gradient(self.pts, axis=0)
        self.heading = np.arctan2(d[:, 1], d[:, 0])
        # A coarse cell keeps the ring search shallow for the thousands
        # of town buildings inside the Sarthe's bounding box; the search
        # always scans one ring past the first hit, so the answer is still
        # the true nearest.
        self.grid = NearestGrid(self.pts, cell=40.0)

    def locate(self, q) -> tuple[np.ndarray, np.ndarray]:
        """(station, signed lateral: positive left) for each query point."""
        q = np.asarray(q, dtype=float).reshape(-1, 2)
        _, j = self.grid.query(q)
        h = self.heading[j]
        d = q - self.pts[j]
        lat = -np.sin(h) * d[:, 0] + np.cos(h) * d[:, 1]
        along = np.cos(h) * d[:, 0] + np.sin(h) * d[:, 1]
        return (self.station[j] + along) % self.total, lat

    def index_at(self, station: float) -> int:
        """The sample nearest a station. Densifying splits each source
        segment into equal parts, so the spacing is only *about* STEP and
        the index has to be searched, not divided for."""
        return int(np.searchsorted(self.station, station % self.total).clip(0, len(self.pts) - 1))

    def half_width(self, station: float, side: str) -> float:
        i = self.index_at(station)
        return float(self.width_left[i] if side == "left" else self.width_right[i])

    def turn_side(self, from_m: float, to_m: float, which: str) -> str:
        """`outside` / `inside` of the bend over a station span, resolved
        from the course's own curvature - so a dossier entry can say where
        a stand is the way a spectator map does."""
        span = (to_m - from_m) % self.total if to_m < from_m else to_m - from_m
        i0, i1 = self.index_at(from_m), self.index_at(from_m + span)
        h0, h1 = self.heading[i0], self.heading[i1]
        turn = (h1 - h0 + math.pi) % (2 * math.pi) - math.pi
        left_turn = turn > 0
        if which == "inside":
            return "left" if left_turn else "right"
        return "right" if left_turn else "left"

    def offset_point(self, station: float, side: str, beyond_edge: float) -> np.ndarray:
        """A point `beyond_edge` metres past the road edge on `side`."""
        i = self.index_at(station)
        lat = self.half_width(station, side) + beyond_edge
        sign = 1.0 if side == "left" else -1.0
        h = self.heading[i]
        return self.pts[i] + sign * lat * np.array([-math.sin(h), math.cos(h)])

    def edge_run(self, from_m: float, to_m: float, side: str, beyond_edge: float) -> np.ndarray:
        """A polyline following the road edge over a station span, which is
        how a hand-placed stand gets the same curve as a traced one."""
        span = (to_m - from_m) % self.total if to_m < from_m else to_m - from_m
        steps = max(int(span / 10.0), 1)
        return np.array(
            [
                self.offset_point(from_m + span * i / steps, side, beyond_edge)
                for i in range(steps + 1)
            ]
        )


# --------------------------------------------------------------- OSM reading


class Osm:
    def __init__(self, paths: list[Path]):
        self.nodes: dict[int, tuple[float, float]] = {}
        self.node_tags: dict[int, dict] = {}
        self.ways: list[dict] = []
        # The tiles overlap at their seams, so the same way arrives more
        # than once; a duplicated way breaks chaining (a pit lane joined to
        # its own copy doubles back) and double-counts buildings.
        seen_ways: set[int] = set()
        for p in paths:
            for e in json.loads(p.read_text(encoding="utf-8"))["elements"]:
                if e["type"] == "node":
                    self.nodes[e["id"]] = (e["lon"], e["lat"])
                    if e.get("tags"):
                        self.node_tags[e["id"]] = e["tags"]
                elif e["type"] == "way":
                    if e["id"] in seen_ways:
                        continue
                    seen_ways.add(e["id"])
                    self.ways.append(e)
        lons = [n[0] for n in self.nodes.values()]
        lats = [n[1] for n in self.nodes.values()]
        self.lon0 = (min(lons) + max(lons)) / 2
        self.lat0 = (min(lats) + max(lats)) / 2

    def way_xy(self, w) -> np.ndarray | None:
        pts = [self.nodes[n] for n in w["nodes"] if n in self.nodes]
        if len(pts) < 2:
            return None
        return np.array([enu(p[0], p[1], self.lon0, self.lat0) for p in pts])


def is_pit_way(t: dict) -> bool:
    name = (t.get("name") or "").lower()
    return t.get("raceway") in ("pitlane", "pit_lane") or "pit" in name


def raceway_cloud(osm: Osm) -> np.ndarray:
    pts = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("highway") != "raceway":
            continue
        if is_pit_way(t) or "kart" in (t.get("name") or "").lower():
            continue
        xy = osm.way_xy(w)
        if xy is not None:
            pts.append(densify(xy, 4.0, closed=False))
    if not pts:
        raise SystemExit("no raceway ways in the extract")
    return np.concatenate(pts)


# ------------------------------------------------------------------ the fit


def coarse_fit(cloud: np.ndarray, cl: np.ndarray, steps=720, res=None, keep=10):
    span = max(np.ptp(cl[:, 0]), np.ptp(cl[:, 1]))
    # The correlation grid covers the whole circuit, so its cell grows with
    # the circuit: a 6 m cell over the Sarthe's 5 km is a 2000-square FFT
    # per rotation.  ICP takes the fit the rest of the way regardless.
    if res is None:
        res = max(6.0, span / 400.0)
    lo = np.array([-span * 1.2, -span * 1.2])
    hi = -lo
    shape = (int((hi[1] - lo[1]) / res) + 1, int((hi[0] - lo[0]) / res) + 1)

    def grid(pts):
        g = np.zeros(shape)
        ix = ((pts[:, 0] - lo[0]) / res).astype(int)
        iy = ((pts[:, 1] - lo[1]) / res).astype(int)
        ok = (ix >= 0) & (iy >= 0) & (ix < shape[1]) & (iy < shape[0])
        np.add.at(g, (iy[ok], ix[ok]), 1.0)
        return np.minimum(g, 1.0)

    c_osm = cloud.mean(0)
    f_osm = np.fft.rfft2(grid(cloud - c_osm))
    c_cl = cl.mean(0)
    # A site with several layouts on it (Silverstone's GP, National, Stowe
    # and karting tracks share tarmac) gives the correlation more than one
    # strong peak, and the tallest is not always the right one - so keep
    # the best few and let the caller score them by how much of the
    # centerline each one actually explains.
    cands = []
    for k in range(steps):
        th = 2 * math.pi * k / steps
        c, s = math.cos(th), math.sin(th)
        corr = np.fft.irfft2(
            f_osm * np.conj(np.fft.rfft2(grid((cl - c_cl) @ np.array([[c, s], [-s, c]])))),
            shape,
        )
        iy, ix = divmod(int(np.argmax(corr)), corr.shape[1])
        dy = iy if iy < shape[0] // 2 else iy - shape[0]
        dx = ix if ix < shape[1] // 2 else ix - shape[1]
        cands.append((float(corr[iy, ix]), th, np.array([dx * res, dy * res])))
    cands.sort(key=lambda c: -c[0])
    out = []
    for score, th, shift in cands:
        # non-maximum suppression: candidates within 6 deg of one already
        # kept are the same peak seen again
        if any(abs((th - o[0] + math.pi) % (2 * math.pi) - math.pi) < math.radians(6) for o in out):
            continue
        c, s = math.cos(-th), math.sin(-th)
        a = np.array([[c, -s], [s, c]])  # track = osm @ a.T + t
        out.append((th, a, -(c_osm + shift) @ a.T + c_cl))
        if len(out) >= keep:
            break
    return [(a, t) for _, a, t in out]


def icp(cloud: np.ndarray, cl: np.ndarray, a, t, iters=15, trim=0.55):
    grid = NearestGrid(cl)
    for _ in range(iters):
        # shallow probes: a point with no track cell nearby is an extra
        # circuit on the same site and gets trimmed away anyway
        d, j = grid.query(cloud @ a.T + t, max_rings=2)
        finite = d[np.isfinite(d)]
        cut = float(np.quantile(finite, trim)) if len(finite) else 3.0
        keep = d <= max(cut, 3.0)
        if keep.sum() < 8:
            break
        x, y = cloud[keep], cl[j[keep]]
        mx, my = x.mean(0), y.mean(0)
        u, _, vt = np.linalg.svd((x - mx).T @ (y - my))
        r = (u @ vt).T
        if np.linalg.det(r) < 0:
            vt[-1] *= -1
            r = (u @ vt).T
        a, t = r, my - mx @ r.T
    p = cloud @ a.T + t
    d, _ = grid.query(p, max_rings=2)
    inl = d[d < 10.0]
    cov, _ = NearestGrid(p).query(cl, max_rings=2)
    return a, t, {
        "rmse_m": round(float(np.sqrt((inl**2).mean())) if len(inl) else 999.0, 2),
        "raceway_matched": round(float((d < 10).mean()), 3),
        "centerline_covered": round(float((cov < 6.0).mean()), 3),
        "centerline_covered_m": round(float((cov < 6.0).sum()) * Track.STEP),
    }


# --------------------------------------------------------------- extraction


def oriented_box(poly: np.ndarray):
    """(centre, length, depth, yaw) of the minimum-area box round a ring."""
    c = poly.mean(0)
    d = poly - c
    best = None
    for i in range(len(poly) - 1):
        e = poly[i + 1] - poly[i]
        n = float(np.hypot(*e))
        if n < 0.5:
            continue
        u = e / n
        r = np.array([[u[0], u[1]], [-u[1], u[0]]])
        p = d @ r.T
        ext = np.ptp(p, axis=0)
        area = ext[0] * ext[1]
        if best is None or area < best[0]:
            best = (area, ext, math.atan2(u[1], u[0]), p.mean(0) @ r + c)
    if best is None:
        return c, 1.0, 1.0, 0.0
    _, ext, yaw, centre = best
    if ext[1] > ext[0]:
        ext = ext[::-1]
        yaw += math.pi / 2
    return centre, float(ext[0]), float(ext[1]), float((yaw + math.pi) % math.pi)


def front_edge(poly: np.ndarray, track: Track) -> np.ndarray:
    """The run of the outline that faces the road: the longest chain of
    vertices nearer the track than the outline's midpoint distance.  A
    rectangular stand yields its long side, a stand built round a bend
    yields the curve, and laying bays along it reproduces the shape."""
    ring = poly[:-1] if np.allclose(poly[0], poly[-1]) else poly
    d, _ = track.grid.query(ring)
    thresh = (d.min() + d.max()) / 2
    near = d <= thresh
    n = len(ring)
    if not near.any():
        return ring
    best = (0, 0, 0)
    for start in range(n):
        if not near[start] or near[(start - 1) % n]:
            continue
        k = 0
        while k < n and near[(start + k) % n]:
            k += 1
        chain = np.array([ring[(start + i) % n] for i in range(k)])
        length = float(np.hypot(*np.diff(chain, axis=0).T).sum()) if k > 1 else 0.0
        if length > best[0]:
            best = (length, start, k)
    _, start, k = best
    if k < 2:
        return ring
    return np.array([ring[(start + i) % n] for i in range(k)])


def ring_area(poly: np.ndarray) -> float:
    x, y = poly[:, 0], poly[:, 1]
    return float(abs(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))) / 2)


def round_pts(p, nd=2):
    return [[round(float(x), nd), round(float(y), nd)] for x, y in np.asarray(p)]


def chain_ways(ways: list[np.ndarray], tol=3.0) -> list[np.ndarray]:
    """Join polylines that share an endpoint into runs."""
    runs = [w.copy() for w in ways]
    merged = True
    while merged:
        merged = False
        for i in range(len(runs)):
            for j in range(len(runs)):
                if i == j or runs[i] is None or runs[j] is None:
                    continue
                a, b = runs[i], runs[j]
                if np.hypot(*(a[-1] - b[0])) < tol:
                    runs[i] = np.vstack([a, b[1:]])
                elif np.hypot(*(a[-1] - b[-1])) < tol:
                    runs[i] = np.vstack([a, b[::-1][1:]])
                elif np.hypot(*(a[0] - b[0])) < tol:
                    runs[i] = np.vstack([a[::-1], b[1:]])
                elif np.hypot(*(a[0] - b[-1])) < tol:
                    runs[i] = np.vstack([b, a[1:]])
                else:
                    continue
                runs[j] = None
                merged = True
                break
            if merged:
                break
        runs = [r for r in runs if r is not None]
    return runs


def extract(stem: str, track: Track, osm: Osm, to_track, fit_report) -> dict:
    lo = track.pts.min(0) - 400.0
    hi = track.pts.max(0) + 400.0

    def xy(w):
        p = osm.way_xy(w)
        if p is None:
            return None
        p = to_track(p)
        # Nothing this far outside the circuit's own bounding box can be
        # part of it; skipping it here keeps the distance queries shallow.
        if (p.max(0) < lo).any() or (p.min(0) > hi).any():
            return None
        return p

    corners = []
    pit_ways: list[np.ndarray] = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("highway") != "raceway":
            continue
        p = xy(w)
        if p is None:
            continue
        d, _ = track.grid.query(p, max_rings=30)
        if is_pit_way(t):
            if float(np.median(d)) < 80.0 and len(p) > 2:
                pit_ways.append(p)
            continue
        name = t.get("name")
        if not name or float(np.median(d)) > 12.0:
            continue
        mid = p[len(p) // 2 : len(p) // 2 + 1]
        s0, _ = track.locate(p[:1])
        s1, _ = track.locate(p[-1:])
        sm, _ = track.locate(mid)
        corners.append(
            {
                "name": name,
                "station_m": round(float(sm[0]), 1),
                "from_m": round(float(s0[0]), 1),
                "to_m": round(float(s1[0]), 1),
            }
        )
    corners.sort(key=lambda c: c["station_m"])
    # one entry per name (OSM splits long sections)
    seen: dict[str, dict] = {}
    for c in corners:
        seen.setdefault(c["name"], c)
    corners = sorted(seen.values(), key=lambda c: c["station_m"])

    pit = None
    # A circuit has more than one lane tagged "pit" (Spa's support pit
    # lane, the Bugatti lanes at Le Mans, the roads behind the garages),
    # and chaining joins whatever shares an end. Clip every chain to the
    # part that runs beside the track, keep the ones of a pit lane's
    # length, and take whichever reaches the start/finish line.
    candidates = []
    for run in chain_ways(pit_ways):
        run = densify(run, 8.0, closed=False)
        d, _ = track.grid.query(run, max_rings=4)
        near = d < 70.0
        span = None
        i = 0
        while i < len(run):
            if not near[i]:
                i += 1
                continue
            j = i
            while j + 1 < len(run) and near[j + 1]:
                j += 1
            if span is None or (j - i) > (span[1] - span[0]):
                span = (i, j)
            i = j + 1
        if span is None or span[1] - span[0] < 2:
            continue
        clipped = run[span[0] : span[1] + 1]
        length = float(np.hypot(*np.diff(clipped, axis=0).T).sum())
        if not 120.0 <= length <= 1200.0:
            continue
        s_here, _ = track.locate(clipped)
        to_line = float(np.minimum(s_here, track.total - s_here).min())
        candidates.append((to_line, length, clipped))
    if candidates:
        # Of the lanes that reach the start/finish line, the circuit's own
        # is the long one: the others are a support paddock's or a link
        # road that happens to pass it.
        at_line = [c for c in candidates if c[0] < 60.0]
        if at_line:
            at_line.sort(key=lambda c: -c[1])
            candidates = at_line
        else:
            candidates.sort(key=lambda c: c[0])
        best = candidates[0][2]
        s_best, lat = track.locate(best)
        side = "left" if float(np.median(lat)) > 0 else "right"
        # In course order, so the entry is the first node.
        ang = np.unwrap(s_best / track.total * 2 * math.pi)
        if float(np.median(np.diff(ang))) < 0:
            best = best[::-1]
        pit = {
            "side": side,
            "length_m": round(candidates[0][1], 1),
            "nodes": round_pts(densify(best, 12.0, closed=False)),
        }

    stands = []
    structures = []
    for w in osm.ways:
        t = w.get("tags") or {}
        p = xy(w)
        if p is None or len(p) < 4:
            continue
        is_stand = t.get("building") == "grandstand" or t.get("leisure") == "grandstand"
        is_building = "building" in t and not is_stand
        if not (is_stand or is_building):
            continue
        d, _ = track.grid.query(p, max_rings=30)
        near = float(d.min())
        if is_stand and near > STAND_RANGE_M:
            continue
        if is_building and (near > STRUCTURE_RANGE_M or ring_area(p) < 250.0):
            continue
        centre, length, depth, yaw = oriented_box(p)
        s, lat = track.locate(centre[None, :])
        entry = {
            "name": t.get("name"),
            "station_m": round(float(s[0]), 1),
            "side": "left" if lat[0] > 0 else "right",
            "offset_m": round(near, 1),
            "length_m": round(length, 1),
            "depth_m": round(depth, 1),
            "yaw_rad": round(yaw, 4),
            "centre": round_pts(centre[None, :])[0],
        }
        if is_stand:
            entry["source"] = "osm"
            entry["covered"] = t.get("covered") == "yes" or "roof:shape" in t
            entry["front"] = round_pts(simplify(front_edge(p, track), 2.0))
            stands.append(entry)
        else:
            entry["levels"] = int(t.get("building:levels", 0) or 0)
            entry["osm_building"] = t.get("building")
            entry["area_m2"] = round(ring_area(p))
            structures.append(entry)
    for spec in MANUAL_STANDS.get(stem, []):
        side = spec["side"]
        if side in ("outside", "inside"):
            side = track.turn_side(spec["from_m"], spec["to_m"], side)
        front = track.edge_run(spec["from_m"], spec["to_m"], side, spec.get("gap_m", 10.0))
        length = float(np.hypot(*np.diff(front, axis=0).T).sum())
        centre = front.mean(0)
        s, _ = track.locate(centre[None, :])
        head = front[-1] - front[0]
        stands.append(
            {
                "name": spec["name"],
                "source": "seating map",
                "station_m": round(float(s[0]), 1),
                "side": side,
                "offset_m": spec.get("gap_m", 10.0),
                "length_m": round(length, 1),
                "depth_m": float(spec.get("depth_m", 16.0)),
                "covered": bool(spec.get("covered", False)),
                "yaw_rad": round(float(math.atan2(head[1], head[0])), 4),
                "centre": round_pts(centre[None, :])[0],
                "front": round_pts(front),
            }
        )
    stands.sort(key=lambda e: e["station_m"])
    structures.sort(key=lambda e: e["station_m"])

    crossings = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if not t.get("bridge") or t.get("bridge") == "no":
            continue
        p = xy(w)
        if p is None:
            continue
        d, _ = track.grid.query(densify(p, 3.0, closed=False), max_rings=4)
        if d.min() > 6.0:
            continue
        if t.get("highway") == "raceway":
            continue  # the road's own bridge, not something over it
        hit = densify(p, 3.0, closed=False)[int(d.argmin())][None, :]
        s, _ = track.locate(hit)
        crossings.append(
            {
                "name": t.get("name"),
                "station_m": round(float(s[0]), 1),
                "kind": "footbridge" if t.get("highway") in ("footway", "path", "steps") else "road",
            }
        )
    for spec in MANUAL_CROSSINGS.get(stem, []):
        crossings.append(dict(spec, source="authored"))
    crossings.sort(key=lambda c: c["station_m"])
    dedup = []
    for c in crossings:
        if dedup and abs(c["station_m"] - dedup[-1]["station_m"]) < 25:
            # The same crossing mapped twice: an authored entry is there on
            # purpose (it says what the structure *is*), so it wins.
            if c.get("source") == "authored" and dedup[-1].get("source") != "authored":
                dedup[-1] = c
            continue
        dedup.append(c)
    crossings = dedup

    landmarks = []
    for nid, t in osm.node_tags.items():
        kind = None
        if t.get("attraction") == "big_wheel":
            kind = "big_wheel"
        elif t.get("man_made") in ("communications_tower", "tower") and t.get("tower:type") != "lighting":
            kind = "tower"
        elif t.get("tower:type") == "lighting" or t.get("highway") == "floodlight":
            kind = "floodlight"
        if not kind:
            continue
        p = to_track(np.array([enu(*osm.nodes[nid], osm.lon0, osm.lat0)]))
        d, _ = track.grid.query(p, max_rings=30)
        if d[0] > 300:
            continue
        s, lat = track.locate(p)
        landmarks.append(
            {
                "kind": kind,
                "name": t.get("name"),
                "station_m": round(float(s[0]), 1),
                "side": "left" if lat[0] > 0 else "right",
                "centre": round_pts(p)[0],
            }
        )
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("attraction") != "big_wheel":
            continue
        p = xy(w)
        if p is None:
            continue
        c = p.mean(0)[None, :]
        d, _ = track.grid.query(c, max_rings=30)
        if d[0] > 300:
            continue
        s, lat = track.locate(c)
        landmarks.append(
            {
                "kind": "big_wheel",
                "name": t.get("name"),
                "station_m": round(float(s[0]), 1),
                "side": "left" if lat[0] > 0 else "right",
                "centre": round_pts(c)[0],
            }
        )
    for spec in MANUAL_LANDMARKS.get(stem, []):
        p = track.offset_point(spec["station_m"], spec["side"], spec.get("offset_m", 40.0))
        entry = {
            "kind": spec["kind"],
            "name": spec.get("name"),
            "source": "authored",
            "station_m": spec["station_m"],
            "side": spec["side"],
            "centre": round_pts(p[None, :])[0],
        }
        if spec.get("brand"):
            entry["brand"] = spec["brand"]
        if "altitude_m" in spec:
            entry["altitude_m"] = spec["altitude_m"]
        if "broadside_to_m" in spec:
            # Long axis across the line of sight from that station.
            eye = track.offset_point(spec["broadside_to_m"], "left", -track.half_width(spec["broadside_to_m"], "left"))
            sight = p - eye
            entry["yaw_rad"] = round(math.atan2(sight[1], sight[0]) + math.pi / 2, 4)
        landmarks.append(entry)
    landmarks.sort(key=lambda e: e["station_m"])

    woods = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("natural") != "wood" and t.get("landuse") not in ("forest",):
            continue
        leaf = t.get("leaf_type") or ""
        leaf = leaf if leaf in ("broadleaved", "needleleaved", "mixed") else "mixed"
        p = xy(w)
        if p is None or len(p) < 4:
            continue
        d, _ = track.grid.query(p, max_rings=30)
        if d.min() > WOOD_RANGE_M:
            continue
        ring = simplify(p, 6.0)
        if len(ring) < 4 or ring_area(ring) < 400:
            continue
        woods.append({"leaf": leaf, "ring": round_pts(ring, 1)})

    return {
        "format": LAYOUT_FORMAT,
        "version": LAYOUT_VERSION,
        "source_track": f"{stem}.yaml",
        "track_name": track.data.get("name", stem),
        "attribution": ATTRIBUTION,
        "osm_bbox": [list(b) for b in BBOXES[stem]],
        "fit": fit_report,
        "corners": corners,
        "pit_lane": pit,
        "stands": stands,
        "structures": structures,
        "crossings": crossings,
        "landmarks": landmarks,
        "woods": woods,
    }


def build(stem: str, offline: bool) -> dict:
    print(f"== {stem}")
    paths = fetch(stem, offline)
    track = Track(stem)
    osm = Osm(paths)
    cloud = raceway_cloud(osm)
    t0 = time.time()
    best = None
    for a0, t0_ in coarse_fit(cloud, track.pts):
        a, t, report = icp(cloud, track.pts, a0, t0_)
        if best is None or report["centerline_covered"] > best[2]["centerline_covered"]:
            best = (a, t, report)
        # Good enough to stop searching: either the fit explains the whole
        # centerline, or (Le Mans, where most of the lap is public road in
        # OSM) it explains kilometres of it to within a couple of metres.
        if report["centerline_covered"] > 0.98 or (
            report["centerline_covered_m"] >= 1500 and report["rmse_m"] <= 2.0
        ):
            break
    a, t, report = best
    print(
        f"   fit: rmse {report['rmse_m']} m, centerline covered "
        f"{report['centerline_covered'] * 100:.1f}%, raceway matched "
        f"{report['raceway_matched'] * 100:.1f}% ({time.time() - t0:.0f}s)"
    )
    # Most circuits are raceway end to end, so the fit should explain the
    # whole centerline.  Le Mans is not: two thirds of the Sarthe is public
    # road in OSM (the N138 down to Mulsanne), so there the test is that
    # the permanent section matches to within a couple of metres over
    # kilometres - which no wrong alignment can do.
    ok = report["centerline_covered"] >= 0.9 or (
        report["centerline_covered_m"] >= 1500 and report["rmse_m"] <= 3.0
    )
    if not ok:
        raise SystemExit(
            f"{stem}: fit explains only {report['centerline_covered_m']} m of "
            f"centerline at {report['rmse_m']} m rmse - refusing to write a "
            "dossier from it"
        )
    to_track = lambda p: np.asarray(p, dtype=float).reshape(-1, 2) @ a.T + t
    layout = extract(stem, track, osm, to_track, report)
    print(
        f"   {len(layout['corners'])} named corners, "
        f"{len(layout['stands'])} stands, {len(layout['structures'])} structures, "
        f"{len(layout['crossings'])} crossings, {len(layout['landmarks'])} landmarks, "
        f"{len(layout['woods'])} woods, pit lane "
        + (layout["pit_lane"]["side"] if layout["pit_lane"] else "MISSING")
    )
    return layout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tracks", nargs="*", help="track stems, e.g. Monza")
    ap.add_argument("--all", action="store_true", help="every circuit with a bbox")
    ap.add_argument("--offline", action="store_true", help="use the cache only")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    args = ap.parse_args()

    stems = sorted(BBOXES) if args.all else args.tracks
    if not stems:
        ap.print_help()
        return 1
    for stem in stems:
        if stem not in BBOXES:
            raise SystemExit(f"no bbox for {stem}; add one to BBOXES")
        layout = build(stem, args.offline)
        if args.dry_run:
            continue
        out = TRACK_DIR / f"{stem}.layout.json"
        out.write_text(json.dumps(layout, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"   wrote {out.relative_to(REPO)} ({out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
