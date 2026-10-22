#!/usr/bin/env python3
"""Join two circuits that share a venue into one lap, with its dossier and
DEM sidecar: the Nurburgring's 24-hour layout is the Grand Prix circuit
joined to the Nordschleife by the two connecting roads OSM maps
("Anbindung zur Nordschleife", "Anbindung zum GP Kurs").

    python tools/combine_tracks.py Nuerburgerring24        (from apexsim-content/)
    python tools/combine_tracks.py Nuerburgerring24 --scene    (after seed_scene.py)
    python tools/combine_tracks.py Nuerburgerring24 --layout   (the dossier alone)

Writes tracks/default/<Stem>/<Stem>.{yaml,layout.json,dem.msgpack} in the
base circuit's frame and datum (its DEM georeference), so nothing is
refitted: the other circuit's nodes and dossier entries go through
WGS-84 from its own georeference. Then run the refresh order from
`dem_elevation.py` for the stem (track-pipeline.md "Adding a new
circuit"); `osm_layout.py` leaves a combined stem alone.

A part is a span of a circuit's own stations or a connector (lon/lat, the
OSM way's nodes, frozen here so the build needs no extract). The joins are
blended over JOIN_BLEND_M either side; a dossier entry anchored by station
only (a corner, a crossing, road graffiti) keeps its place when its
station is inside a kept span and is dropped otherwise; one with a
position is moved into the frame and re-stationed on the new lap.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import msgpack
import numpy as np
import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import dem_fetch  # noqa: E402

R_EARTH = 6378137.0
STEP_M = 5.0
JOIN_BLEND_M = 40.0

COMBINED: dict[str, dict] = {
    # The 24-hour layout: the GP circuit to the NGK chicane's exit, the
    # Anbindung zur Nordschleife onto the loop just past T13, the loop to
    # Hohenrain, the Anbindung zum GP Kurs back onto the GP before its pit
    # straight. The stations are where the connectors' ends meet each
    # centerline (within 3 m).
    "Nuerburgerring24": dict(
        base="Nuerburgerring",
        name="Nürburgring 24h",
        display_name="Nürburgerring 24h",
        track_id="3c1f6f4e-24a8-4e0b-9d5a-7f0d2c4b9e24",
        description="Modelled on the 24-hour layout at Nürburg: the Grand Prix "
        "circuit joined to the 20.8 km northern loop, through the forests of "
        "Germany's Eifel hills.",
        category="Endurance",
        parts=[
            ("track", "Nuerburgerring", 0.0, 4550.0),
            ("way", [(6.9491896, 50.3377052), (6.9493353, 50.3377414), (6.9494136, 50.3377721),
                     (6.9494855, 50.3378084), (6.9495582, 50.3378579), (6.9496162, 50.3379016),
                     (6.9496625, 50.3379467), (6.9496889, 50.3379847), (6.9497148, 50.3380338),
                     (6.9497305, 50.3380757), (6.9497344, 50.3381197), (6.9497302, 50.3381653),
                     (6.9497194, 50.3382209), (6.9496952, 50.3382811), (6.9496604, 50.338346),
                     (6.9496195, 50.3384123)]),
            ("track", "Mordschleife", 20.0, 20643.0),
            ("way", [(6.9512265, 50.3377306), (6.9509739, 50.3376032), (6.9497367, 50.3368937)]),
            ("track", "Nuerburgerring", 4695.0, None),
        ],
    ),
}


def track_dir(stem: str) -> Path:
    return ROOT / "tracks" / "default" / stem


def georef(stem: str) -> dict:
    return msgpack.unpackb((track_dir(stem) / f"{stem}.dem.msgpack").read_bytes(), raw=False)["georef"]


def load_yaml(stem: str) -> dict:
    return yaml.safe_load((track_dir(stem) / f"{stem}.yaml").read_text(encoding="utf-8"))


def to_lonlat(xy, g):
    a = np.array(g["a"]).reshape(2, 2)
    e = (np.asarray(xy, dtype=float).reshape(-1, 2) - np.array(g["t"])) @ np.linalg.inv(a.T)
    return (g["lon0"] + np.degrees(e[:, 0] / (R_EARTH * math.cos(math.radians(g["lat0"])))),
            g["lat0"] + np.degrees(e[:, 1] / R_EARTH))


def from_lonlat(lon, lat, g):
    a = np.array(g["a"]).reshape(2, 2)
    e = np.stack([np.radians(np.asarray(lon) - g["lon0"]) * R_EARTH * math.cos(math.radians(g["lat0"])),
                  np.radians(np.asarray(lat) - g["lat0"]) * R_EARTH], 1)
    return e @ a.T + np.array(g["t"])


class Mover:
    """One circuit's frame into the base's: xy through WGS-84, z through
    the two datums, a heading by the frames' relative turn."""

    def __init__(self, src: str, base: str):
        self.same = src == base
        self.gs, self.gb = georef(src), georef(base)
        if not self.same:
            p = np.array([[0.0, 0.0], [100.0, 0.0]])
            q = self.xy(p)
            self.turn = math.atan2(q[1, 1] - q[0, 1], q[1, 0] - q[0, 0])
        else:
            self.turn = 0.0

    def xy(self, p):
        p = np.asarray(p, dtype=float).reshape(-1, 2)
        if self.same:
            return p
        return from_lonlat(*to_lonlat(p, self.gs), self.gb)

    def z(self, z):
        return z if self.same else z - self.gs["datum_offset_m"] + self.gb["datum_offset_m"]


def stations(p):
    return np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(p[:, :2], axis=0).T))])


def resample(nodes: list[dict], step: float) -> list[dict]:
    """Nodes every `step` metres along the polyline, the other fields
    interpolated linearly (surface from the nearer node)."""
    p = np.array([[n["x"], n["y"]] for n in nodes])
    s = stations(np.column_stack([p, np.zeros(len(p))]))
    out = []
    for t in np.arange(0.0, s[-1], step):
        k = min(int(np.searchsorted(s, t, side="right")) - 1, len(nodes) - 2)
        f = (t - s[k]) / max(s[k + 1] - s[k], 1e-6)
        a, b = nodes[k], nodes[k + 1]
        n = {key: (a[key] + (b[key] - a[key]) * f) for key in ("x", "y", "z", "width_left", "width_right", "banking", "friction")}
        n["surface_type"] = (a if f < 0.5 else b)["surface_type"]
        out.append(n)
    return out


def node(x, y, z, wl, wr, banking=0.0, friction=1.0, surface="Asphalt"):
    return {"x": float(x), "y": float(y), "z": float(z), "width_left": float(wl), "width_right": float(wr),
            "banking": float(banking), "friction": float(friction), "surface_type": surface}


def build(stem: str):
    spec = COMBINED[stem]
    base = spec["base"]
    movers: dict[str, Mover] = {}
    pieces: list[tuple[str, str | None, float | None, list[dict]]] = []
    for part in spec["parts"]:
        if part[0] == "track":
            _, src, s0, s1 = part
            mv = movers.setdefault(src, Mover(src, base))
            y = load_yaml(src)
            raw = np.array([[n["x"], n["y"], n["z"]] for n in y["nodes"]])
            st = stations(raw)
            s1 = st[-1] if s1 is None else s1
            keep = [i for i in range(len(raw)) if s0 <= st[i] <= s1]
            xy = mv.xy(raw[keep, :2])
            nodes = []
            for j, i in enumerate(keep):
                n = y["nodes"][i]
                nodes.append(node(xy[j, 0], xy[j, 1], mv.z(n["z"]), n["width_left"], n["width_right"],
                                  n.get("banking", 0.0), n.get("friction", 1.0), n.get("surface_type", "Asphalt")))
            pieces.append(("track", src, s0, nodes))
        else:
            lon, lat = np.array(part[1]).T
            xy = from_lonlat(lon, lat, movers[base].gb if base in movers else georef(base))
            pieces.append(("way", None, None, [node(x, y, 0.0, 0.0, 0.0) for x, y in xy]))
    # A connector takes its heights and widths from the spans either side.
    for k, (kind, _, _, nodes) in enumerate(pieces):
        if kind != "way":
            continue
        a, b = pieces[k - 1][3][-1], pieces[(k + 1) % len(pieces)][3][0]
        pts = np.array([[n["x"], n["y"]] for n in nodes])
        s = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
        for n, t in zip(nodes, s / max(s[-1], 1e-6)):
            for key in ("z", "width_left", "width_right"):
                n[key] = a[key] + (b[key] - a[key]) * t
    # The lap, resampled, and each join blended (a moving average over the
    # nodes within JOIN_BLEND_M of it, weighted down to the edges).
    joined: list[dict] = []
    joins: list[int] = []
    for _, _, _, nodes in pieces:
        if joined:
            joins.append(len(joined))
        joined.extend(nodes)
    lap = resample(joined + [joined[0]], STEP_M)
    p = np.array([[n["x"], n["y"]] for n in lap])
    raw_join_xy = [np.array([joined[j]["x"], joined[j]["y"]]) for j in joins] + [np.array([joined[0]["x"], joined[0]["y"]])]
    half = int(JOIN_BLEND_M / STEP_M)
    sm = p.copy()
    for q in raw_join_xy:
        c = int(np.argmin(np.hypot(*(p - q).T)))
        for off in range(-half, half + 1):
            i = (c + off) % len(p)
            w = 1.0 - abs(off) / (half + 1)
            idx = [(i + d) % len(p) for d in range(-3, 4)]
            sm[i] = (1 - w) * p[i] + w * p[idx].mean(0)
    for n, (x, y) in zip(lap, sm):
        n["x"], n["y"] = round(float(x), 2), round(float(y), 2)
        n["z"] = round(n["z"], 3)
        n["width_left"], n["width_right"] = round(n["width_left"], 3), round(n["width_right"], 3)
        n["banking"], n["friction"] = round(n["banking"], 4), round(n["friction"], 3)
    return spec, movers, pieces, lap


def write_yaml(stem, spec, lap):
    by = load_yaml(spec["base"])
    meta = dict(by["metadata"])
    meta.update(description=spec["description"], category=spec["category"],
                length_m=float(round(float(stations(np.array([[n["x"], n["y"], 0.0] for n in lap] + [[lap[0]["x"], lap[0]["y"], 0.0]]))[-1]), 3)))
    doc = {
        "name": spec["name"],
        "display_name": spec["display_name"],
        "track_id": spec["track_id"],
        "nodes": [{"x": float(n["x"]), "y": float(n["y"]), "z": float(n["z"]), "width": None,
                   "width_left": float(n["width_left"]), "width_right": float(n["width_right"]),
                   "banking": float(n["banking"]), "friction": float(n["friction"]),
                   "surface_type": n["surface_type"]} for n in lap],
        "checkpoints": [],
        "spawn_points": [],
        "default_width": by.get("default_width", 12.0),
        "closed_loop": True,
        "raceline": [],
        "drs_zones": [],
        "metadata": meta,
    }
    out = track_dir(stem) / f"{stem}.yaml"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(yaml.safe_dump(doc, sort_keys=False, allow_unicode=True, width=1000), encoding="utf-8")
    print(f"   wrote {out.relative_to(ROOT)} ({len(lap)} nodes, {meta['length_m']:.0f} m)")


def station_map(pieces):
    """(src, s0, s1, new_s0) for every kept span, in the lap's order."""
    out, at = [], 0.0
    for kind, src, s0, nodes in pieces:
        pts = np.array([[n["x"], n["y"], 0.0] for n in nodes])
        length = float(stations(pts)[-1]) if len(pts) > 1 else 0.0
        if kind == "track":
            out.append((src, s0, s0 + length, at))
        at += length
    return out


def trim_pit_lane(pit, locate, last_span_at):
    """The base's pit lane from its first node beside the lap's last span:
    a lane whose entry leaves the base circuit in a stretch the lap cuts
    (the GP's, before the Coca-Cola curve) otherwise starts beside another
    span and is read against the wrong leg. The dress pass blends the new
    start onto the road edge as it does any lane's."""
    if not pit:
        return pit
    nodes = pit["nodes"]
    first = next((k for k, n in enumerate(nodes) if locate(np.array(n[:2]))[0] >= last_span_at), 0)
    kept = nodes[first:]
    length = float(np.hypot(*np.diff(np.array(kept)[:, :2], axis=0).T).sum())
    return dict(pit, nodes=kept, length_m=round(length, 1))


def merge_layout(stem, spec, movers, pieces, lap):
    base = spec["base"]
    P = np.array([[n["x"], n["y"]] for n in lap])
    S = stations(np.column_stack([P, np.zeros(len(P))]))
    total = float(S[-1] + np.hypot(*(P[0] - P[-1])))
    spans = station_map(pieces)

    def restation(src, s):
        for sp, s0, s1, n0 in spans:
            if sp == src and s0 <= s <= s1:
                return round(n0 + (s - s0), 1)
        return None

    def locate(xy):
        d = np.hypot(*(P - xy).T)
        k = int(np.argmin(d))
        a, b = P[k], P[(k + 1) % len(P)]
        side = "left" if (b[0] - a[0]) * (xy[1] - a[1]) - (b[1] - a[1]) * (xy[0] - a[0]) > 0 else "right"
        return round(float(S[k]), 1), side, float(d[k])

    def mv_pts(src, pts):
        return [[round(float(x), 2), round(float(y), 2)] for x, y in movers[src].xy(pts)]

    out = {
        "format": "apex-track-layout",
        "version": 1,
        "source_track": f"{stem}.yaml",
        "track_name": spec["name"],
        "track_display_name": spec["display_name"],
        "combined_from": [s for s in dict.fromkeys(sp for sp, *_ in spans)],
    }
    lists: dict[str, list] = {}
    for src in dict.fromkeys(sp for sp, *_ in spans):
        L = json.loads((track_dir(src) / f"{src}.layout.json").read_text(encoding="utf-8"))
        mv = movers[src]
        if src == base:
            out["attribution"] = L.get("attribution")
            out["osm_bbox"] = L.get("osm_bbox")
            out["fit"] = L.get("fit")
            out["pit_lane"] = trim_pit_lane(L.get("pit_lane"), locate, spans[-1][3])
        for key in ("corners", "crossings"):
            for e in L.get(key, []):
                s = restation(src, e["station_m"])
                if s is None:
                    continue
                e = dict(e, station_m=s)
                for k in ("from_m", "to_m"):
                    if e.get(k) is not None:
                        e[k] = restation(src, e[k])
                if any(e.get(k, 0.0) is None for k in ("from_m", "to_m") if k in e):
                    continue
                lists.setdefault(key, []).append(e)
        for e in L.get("graffiti", []):
            s = restation(src, e["station_m"])
            if s is not None:
                lists.setdefault("graffiti", []).append(dict(e, station_m=s))
        for key in ("stands", "structures", "landmarks", "poi"):
            for e in L.get(key, []):
                e = dict(e)
                c = mv.xy([e["centre"]])[0]
                e["centre"] = [round(float(c[0]), 2), round(float(c[1]), 2)]
                if "front" in e:
                    e["front"] = mv_pts(src, e["front"])
                if e.get("yaw_rad") is not None:
                    e["yaw_rad"] = round(e["yaw_rad"] + mv.turn, 4)
                s, side, d = locate(c)
                e["station_m"], e["side"] = s, side
                if "offset_m" in e:
                    e["offset_m"] = round(d, 1)
                lists.setdefault(key, []).append(e)
        for key in ("woods", "areas"):
            for e in L.get(key, []):
                lists.setdefault(key, []).append(dict(e, ring=mv_pts(src, e["ring"])))
        for key in ("barriers", "roads", "waterways", "tree_rows", "banking"):
            for e in L.get(key, []):
                lists.setdefault(key, []).append(dict(e, line=mv_pts(src, e["line"])))
        for e in L.get("water", []):
            lists.setdefault("water", []).append(dict(e, rings=[mv_pts(src, r) for r in e["rings"]]))
    for key, v in lists.items():
        if v and "station_m" in v[0]:
            v.sort(key=lambda e: e["station_m"])
        out[key] = v
    path = track_dir(stem) / f"{stem}.layout.json"
    path.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"   wrote {path.relative_to(ROOT)} ({path.stat().st_size // 1024} KiB, lap {total:.0f} m)")


def merge_scene(stem, spec, pieces):
    """The curbs and the run-off surfaces of both circuits' scenes onto the
    seeded scene (`seed_scene.py <Stem>` first: its grass bands and start
    line stay), re-stationed like the dossier's station-only entries; one
    reaching outside a kept span is left out. Props, decals and the pit
    lane are the dress pass's."""
    spans = station_map(pieces)

    def restation(src, s):
        for sp, s0, s1, n0 in spans:
            if sp == src and s0 <= s <= s1:
                return round(n0 + (s - s0), 2)
        return None

    path = track_dir(stem) / f"{stem}.ats"
    scene = json.loads(path.read_text(encoding="utf-8"))
    next_id = scene.get("next_id", 1)
    curbs, surfaces = [], [x for x in scene.get("surfaces", []) if x["kind"] == "grass"]
    for src in dict.fromkeys(sp for sp, *_ in spans):
        other = json.loads((track_dir(src) / f"{src}.ats").read_text(encoding="utf-8"))
        for key, out in (("curbs", curbs), ("surfaces", surfaces)):
            for e in other.get(key, []):
                if key == "surfaces" and e["kind"] == "grass":
                    continue
                a, b = restation(src, e["start_m"]), restation(src, e["end_m"])
                if a is None or b is None or b < a:
                    continue
                out.append(dict(e, id=next_id, start_m=a, end_m=b))
                next_id += 1
    scene["curbs"], scene["surfaces"] = curbs, surfaces
    scene["next_id"] = next_id
    path.write_text(json.dumps(scene, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"   wrote {path.relative_to(ROOT)} ({len(curbs)} curbs, {len(surfaces)} surfaces)")


def write_dem(stem, spec, lap):
    g = georef(spec["base"])

    class Fit:
        lon0, lat0 = g["lon0"], g["lat0"]

    class Lap:
        pts = np.array([[n["x"], n["y"]] for n in lap])
        data = {"nodes": [{"z": n["z"]} for n in lap]}

    frame = dem_fetch.Frame(Fit, np.array(g["a"]).reshape(2, 2), np.array(g["t"]))
    inner = dem_fetch.grid_spec(Lap.pts, dem_fetch.INNER_MARGIN_M, dem_fetch.INNER_CELL_M)
    outer = dem_fetch.grid_spec(Lap.pts, dem_fetch.OUTER_MARGIN_M, dem_fetch.OUTER_CELL_M)
    o = outer
    span = np.array([[o["origin_x"], o["origin_y"]],
                     [o["origin_x"] + (o["cols"] - 1) * o["cell_m"], o["origin_y"] + (o["rows"] - 1) * o["cell_m"]],
                     [o["origin_x"], o["origin_y"] + (o["rows"] - 1) * o["cell_m"]],
                     [o["origin_x"] + (o["cols"] - 1) * o["cell_m"], o["origin_y"]]])
    lon, lat = frame.to_lonlat(span)
    dem = dem_fetch.Dem(lon.min(), lon.max(), lat.min(), lat.max(), False)
    # The base's own datum: the base circuit's z and this lap's agree on
    # the GP part by construction.
    dz = g["datum_offset_m"]
    grids = {}
    for name, sp in (("inner", inner), ("outer", outer)):
        h = dem_fetch.sample_grid(dem, frame, sp)
        h[~np.isfinite(h)] = 0.0
        h += dz
        sp["heights"] = dem_fetch.pack_f32(h)
        grids[name] = sp
    doc = {
        "version": dem_fetch.DEM_VERSION,
        "source": dem_fetch.SOURCE,
        "georef": dict(g),
        "inner": grids["inner"],
        "outer": grids["outer"],
    }
    path = track_dir(stem) / f"{stem}.dem.msgpack"
    path.write_bytes(dem_fetch.pack(doc))
    print(f"   wrote {path.relative_to(ROOT)} ({path.stat().st_size // 1024} KiB)")


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for stem in args or list(COMBINED):
        print(f"== {stem}")
        spec, movers, pieces, lap = build(stem)
        if "--scene" in sys.argv:
            merge_scene(stem, spec, pieces)
            continue
        if "--layout" in sys.argv:
            merge_layout(stem, spec, movers, pieces, lap)
            continue
        write_yaml(stem, spec, lap)
        merge_layout(stem, spec, movers, pieces, lap)
        write_dem(stem, spec, lap)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
