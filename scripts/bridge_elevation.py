#!/usr/bin/env python3
"""Lift a street circuit's bridges out of the flat centerline.

A reclaimed-land city circuit (Marina Bay) is flat except where the lap
crosses water: the DEM is a surface model that reads rooftops there, so
`dem_elevation.py` is no use, and the OSM relation carries no heights. What
it does carry is *where* the bridges are (`bridge=yes`, `layer=1`), which
`osm_layout.py` writes into the dossier as `deck_arch` / `deck_wide`
crossings with `from_m`..`to_m`. This script puts a road on each of them:
a smooth hump (a raised cosine) as high as the bridge's kind says over the
span, with an approach ramp either side, so the road climbs onto the deck
instead of stepping. The humps are estimates (the real decks stand a few
metres over the water; no survey is published), kept in `DECK_HEIGHT_M`.

    python scripts/bridge_elevation.py MarinaBay [--dry-run]

It rewrites only the `z:` lines of the nodes and the raceline, as text, so
nothing else in the YAML moves, and it sets z absolutely (flat base 0), so
running it twice changes nothing. Run it after `ats-smooth` and before
`osm_layout.py` / `ats-dress` (CLAUDE.md, refresh order): the dossier's
spans are stations along this centerline, which it does not change.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from track_dirs import track_dir  # noqa: E402

#: Height of the deck over the approaches at mid-span, metres, by crossing
#: kind: Anderson Bridge (70 m, a through-arch) and Esplanade Bridge (290 m).
DECK_HEIGHT_M = {"deck_arch": 2.5, "deck_wide": 4.0}
#: Road length either side of a span over which it climbs onto the deck.
RAMP_M = 60.0


def stations(nodes: list[tuple[float, float]]) -> list[float]:
    s = [0.0]
    for a, b in zip(nodes, nodes[1:]):
        s.append(s[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    return s


def hump(station: float, total: float, start: float, end: float, height: float) -> float:
    """Raised cosine over `start`..`end` (wrapping on a loop) plus ramps."""
    span = (end - start) % total
    # Position relative to the span's start, in -total/2..total/2.
    d = (station - start + total / 2) % total - total / 2
    lo, hi = -RAMP_M, span + RAMP_M
    if d <= lo or d >= hi:
        return 0.0
    u = (d - lo) / (hi - lo)  # 0..1 over ramp + span + ramp
    return height * 0.5 * (1.0 - math.cos(2.0 * math.pi * u))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stem")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    d = track_dir(args.stem)
    layout = json.loads((d / f"{args.stem}.layout.json").read_text(encoding="utf-8"))
    decks = [
        (c["from_m"], c["to_m"], DECK_HEIGHT_M[c["kind"]], c.get("name"))
        for c in layout["crossings"]
        if c["kind"] in DECK_HEIGHT_M and "from_m" in c
    ]
    if not decks:
        print(f"{args.stem}: no deck crossings in the dossier; nothing to lift")
        return 0

    path = d / f"{args.stem}.yaml"
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")
    i_nodes = lines.index("nodes:")
    i_race = lines.index("raceline:")
    i_end = next(i for i in range(i_race + 1, len(lines)) if lines[i] and not lines[i].startswith((" ", "-")))
    xy_re = re.compile(r"^(?:- |  )(x|y): (\S+)$")

    def block(lo: int, hi: int):
        pts, zlines, cur = [], [], {}
        for i in range(lo, hi):
            m = xy_re.match(lines[i])
            if m:
                cur[m.group(1)] = float(m.group(2))
            elif lines[i].startswith("  z: "):
                zlines.append(i)
                pts.append((cur["x"], cur["y"]))
        return pts, zlines

    node_pts, node_z = block(i_nodes, next(i for i in range(i_nodes + 1, len(lines)) if lines[i].startswith("checkpoints:")))
    race_pts, race_z = block(i_race, i_end)
    if len(node_pts) != len(race_pts):
        raise SystemExit(f"{len(node_pts)} nodes but {len(race_pts)} raceline points")
    s = stations(node_pts)
    total = s[-1] + math.hypot(node_pts[0][0] - node_pts[-1][0], node_pts[0][1] - node_pts[-1][1])
    zs = [round(sum(hump(si, total, a, b, h) for a, b, h, _ in decks), 3) + 0.0 for si in s]
    for (a, b, h, name) in decks:
        print(f"  {name or 'bridge'}: {a:.0f}-{b:.0f} m, {h} m high")
    print(f"{args.stem}: z {min(zs):.2f}..{max(zs):.2f} m over {len(zs)} nodes")
    if args.dry_run:
        return 0
    for idx, i in enumerate(node_z):
        lines[i] = f"  z: {zs[idx]}"
    for idx, i in enumerate(race_z):
        lines[i] = f"  z: {zs[idx]}"
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
