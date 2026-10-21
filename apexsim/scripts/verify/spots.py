#!/usr/bin/env python3
"""Camera poses for the verification runs, worked out from a track YAML.

    python scripts/verify/spots.py SpaFrankenchamps --puddles 3      # the lowest cells (where rain stands)
    python scripts/verify/spots.py SpaFrankenchamps --grade 2        # the steepest climbs and drops
    python scripts/verify/spots.py SpaFrankenchamps --bank 2         # the most banked stretches
    python scripts/verify/spots.py SpaFrankenchamps --corners 3      # the tightest bends
    python scripts/verify/spots.py SpaFrankenchamps --at 2950        # a station on the lap, m
    python scripts/verify/spots.py SpaFrankenchamps --near 120.5,-340  # the station nearest a point
    python scripts/verify/spots.py Mordschleife --prop rail_tag_eifel 2  # props of the .ats, from the road
    python scripts/verify/spots.py Mordschleife --props   # every prop asset and its count

Each spot prints its station and an `apexsim.cam.LookAt` command for
-ApexExecAfter: a camera beside the road, a little back along the lap,
looking at the centerline there. Every figure is in the server frame
(docs/game/cameras.md, Screenshot camera), which is the YAML's frame.

The puddle cells follow `RoadState::with_road` (server/sim/src/road_state.rs):
10 m cells, a cell's elevation against the mean of its neighbours 100 m
either side, fully low 1.5 m under it. The YAML is resampled linearly,
which is close to, not exactly, the server's centerline.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent.parent
CONTENT = (REPO / ".." / "apexsim-content").resolve()
TRACKS = [CONTENT / "tracks" / "default", CONTENT / "tracks" / "custom"]

CELL_M = 10.0
LOW_REACH_M = 100.0
LOW_DEPTH_M = 1.5


def load(stem: str) -> dict:
    for root in TRACKS:
        path = root / stem / f"{stem}.yaml"
        if path.is_file():
            with open(path, encoding="utf-8") as f:
                return yaml.safe_load(f)
    raise SystemExit(f"no track {stem} under apexsim-content/tracks")


class Lap:
    """The centerline as stations: x, y, z, heading, widths and banking."""

    def __init__(self, nodes: list[dict], default_width: float):
        self.nodes = nodes
        self.s = [0.0]
        for a, b in zip(nodes, nodes[1:] + nodes[:1]):
            self.s.append(self.s[-1] + math.hypot(b["x"] - a["x"], b["y"] - a["y"]))
        self.length = self.s[-1]
        self.default_half = default_width / 2.0

    def at(self, station: float) -> dict:
        station %= self.length
        lo, hi = 0, len(self.nodes)
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.s[mid] <= station:
                lo = mid
            else:
                hi = mid
        a = self.nodes[lo]
        b = self.nodes[(lo + 1) % len(self.nodes)]
        seg = max(self.s[lo + 1] - self.s[lo], 1e-6)
        t = (station - self.s[lo]) / seg
        lerp = lambda k, d=0.0: (a.get(k) or d) + ((b.get(k) or d) - (a.get(k) or d)) * t
        half_l = a.get("width_left") or self.default_half
        half_r = a.get("width_right") or self.default_half
        return {
            "station": station,
            "x": lerp("x"), "y": lerp("y"), "z": lerp("z"),
            "heading": math.atan2(b["y"] - a["y"], b["x"] - a["x"]),
            "left": half_l, "right": half_r,
            "banking": lerp("banking"),
        }

    def nearest(self, x: float, y: float) -> float:
        best = min(range(len(self.nodes)),
                   key=lambda i: (self.nodes[i]["x"] - x) ** 2 + (self.nodes[i]["y"] - y) ** 2)
        return self.s[best]


def look_at(p: dict, side: str, back: float, out: float, height: float, lateral: float = 0.0) -> str:
    """A camera `out` m beyond the road's `side` edge and `back` m behind
    the spot along the lap, `height` m over the road, aimed at the spot
    (`lateral` m right of the centerline)."""
    h = p["heading"]
    fx, fy = math.cos(h), math.sin(h)
    lx, ly = -fy, fx  # +Y left
    edge = p["left"] + out if side == "left" else -(p["right"] + out)
    cx = p["x"] - fx * back + lx * edge
    cy = p["y"] - fy * back + ly * edge
    cz = p["z"] + height
    tx = p["x"] - lx * lateral
    ty = p["y"] - ly * lateral
    return f"apexsim.cam.LookAt {cx:.1f} {cy:.1f} {cz:.1f} {tx:.1f} {ty:.1f} {p['z']:.1f}"


def scene(stem: str) -> dict:
    import json
    for root in TRACKS:
        path = root / stem / f"{stem}.ats"
        if path.is_file():
            with open(path, encoding="utf-8") as f:
                return json.load(f)
    raise SystemExit(f"no {stem}.ats under apexsim-content/tracks")


def prop_shot(lap: Lap, prop: dict, back: float, height: float) -> tuple[dict, str]:
    """A driver's-eye camera: on the road `back` m before the prop's
    station, `height` m up, looking at the prop a metre over its base."""
    station = lap.nearest(prop["x"], prop["y"])
    eye = lap.at(station - back)
    z = prop.get("z", eye["z"])
    return lap.at(station), (f"apexsim.cam.LookAt {eye['x']:.1f} {eye['y']:.1f} {eye['z'] + height:.1f} "
                             f"{prop['x']:.1f} {prop['y']:.1f} {z + 1.0:.1f}")


def cells(lap: Lap) -> list[dict]:
    n = max(1, math.ceil(lap.length / CELL_M))
    return [lap.at((i + 0.5) * CELL_M) for i in range(n)]


def low_cells(lap: Lap) -> list[tuple[float, dict]]:
    cs = cells(lap)
    n = len(cs)
    reach = max(1, round(LOW_REACH_M / CELL_M))
    out = []
    for i, c in enumerate(cs):
        mean = sum(cs[(i + k) % n]["z"] for k in range(-reach, reach + 1)) / (2 * reach + 1)
        low = max(0.0, min(1.0, (mean - c["z"]) / LOW_DEPTH_M))
        out.append((low, c))
    return out


def spread(picked: list[tuple[float, dict]], count: int, gap_m: float = 200.0) -> list[tuple[float, dict]]:
    """The best `count`, each at least `gap_m` from the ones before."""
    chosen: list[tuple[float, dict]] = []
    for score, c in picked:
        if all(abs(c["station"] - o["station"]) >= gap_m for _, o in chosen):
            chosen.append((score, c))
        if len(chosen) == count:
            break
    return chosen


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Camera poses from a track YAML.")
    parser.add_argument("track", help="track stem, e.g. SpaFrankenchamps")
    parser.add_argument("--puddles", type=int, metavar="N", help="the N lowest cells")
    parser.add_argument("--grade", type=int, metavar="N", help="the N steepest stretches")
    parser.add_argument("--bank", type=int, metavar="N", help="the N most banked stretches")
    parser.add_argument("--corners", type=int, metavar="N", help="the N tightest bends (apexes)")
    parser.add_argument("--prop", nargs="+", metavar=("ASSET", "N"),
                        help="the first N (default 2) props of this asset in the .ats, seen from the road")
    parser.add_argument("--props", action="store_true", help="list the .ats's prop assets")
    parser.add_argument("--at", type=float, action="append", default=[], metavar="M", help="a station, m")
    parser.add_argument("--near", action="append", default=[], metavar="X,Y", help="the station nearest a point")
    parser.add_argument("--side", choices=("left", "right"), default="left")
    parser.add_argument("--back", type=float, default=18.0, help="m back along the lap (negative: ahead)")
    parser.add_argument("--out", type=float, default=3.0, help="m beyond the road edge")
    parser.add_argument("--height", type=float, default=2.0, help="m over the road")
    args = parser.parse_args(argv)

    track = load(args.track)
    lap = Lap(track["nodes"], float(track.get("default_width") or 12.0))
    print(f"# {args.track}: {lap.length:.0f} m, {len(track['nodes'])} nodes")
    spots: list[tuple[str, dict, float]] = []

    if args.puddles:
        ranked = sorted(low_cells(lap), key=lambda lc: -lc[0])
        for low, c in spread(ranked, args.puddles):
            lateral = 0.0
            if c["banking"] > 0.009:
                lateral = c["right"] - 1.0
            elif c["banking"] < -0.009:
                lateral = -(c["left"] - 1.0)
            spots.append((f"puddle low {low:.2f}", c, lateral))
    if args.grade:
        cs = cells(lap)
        n = len(cs)
        graded = sorted(((abs(cs[(i + 2) % n]["z"] - cs[i - 2]["z"]) / (4 * CELL_M), cs[i]) for i in range(n)),
                        key=lambda gc: -gc[0])
        for g, c in spread(graded, args.grade, 400.0):
            spots.append((f"grade {100 * g:.1f}%", c, 0.0))
    if args.bank:
        ranked = sorted(((abs(c["banking"]), c) for c in cells(lap)), key=lambda bc: -bc[0])
        for b, c in spread(ranked, args.bank, 400.0):
            spots.append((f"banking {math.degrees(b):.1f} deg", c, 0.0))
    if args.corners:
        cs = cells(lap)
        n = len(cs)

        def turn(i):
            d = cs[(i + 2) % n]["heading"] - cs[i - 2]["heading"]
            return abs((d + math.pi) % (2 * math.pi) - math.pi)

        ranked = sorted(((turn(i) / (4 * CELL_M), cs[i]) for i in range(n)), key=lambda tc: -tc[0])
        for k, c in spread(ranked, args.corners, 300.0):
            radius = 1.0 / k if k > 1e-6 else float("inf")
            spots.append((f"bend radius {radius:.0f} m", c, 0.0))
    for station in args.at:
        spots.append((f"station {station:.0f}", lap.at(station), 0.0))
    for text in args.near:
        x, y = (float(v) for v in text.split(","))
        spots.append((f"near {x:.0f},{y:.0f}", lap.at(lap.nearest(x, y)), 0.0))

    if args.props:
        import collections
        counts = collections.Counter((p["kind"], p.get("asset")) for p in scene(args.track)["props"])
        for (kind, asset), n in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"{n:6} {kind:10} {asset}")
        return 0
    if args.prop:
        asset = args.prop[0]
        count = int(args.prop[1]) if len(args.prop) > 1 else 2
        props = [p for p in scene(args.track)["props"] if p.get("asset") == asset]
        if not props:
            raise SystemExit(f"no prop {asset} in {args.track}.ats (--props lists them)")
        ranked = [(0.0, {**lap.at(lap.nearest(p["x"], p["y"])), "prop": p}) for p in props]
        for _, c in spread(ranked, count, 300.0):
            p = c["prop"]
            at, command = prop_shot(lap, p, args.back, args.height)
            print(f"{asset:24} station {at['station']:7.0f} m  at {p['x']:.0f},{p['y']:.0f}  {p.get('text') or ''}")
            print(f"    {command}")
        return 0
    if not spots:
        parser.print_help()
        return 1
    for label, c, lateral in spots:
        print(f"{label:24} station {c['station']:7.0f} m  z {c['z']:6.1f}")
        print(f"    {look_at(c, args.side, args.back, args.out, args.height, lateral)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
