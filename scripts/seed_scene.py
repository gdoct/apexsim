#!/usr/bin/env python3
"""Start a new circuit's `.ats` scene: the start line, the grass and the curbs.

    python scripts/seed_scene.py Nordschleife            # writes content/tracks/real/Nordschleife.ats
    python scripts/seed_scene.py Nordschleife --force    # replace an existing scene's curbs and grass
    python scripts/seed_scene.py Zandvoort --from survey.json   # curbs and bands measured elsewhere

The shipped circuits' curbs, run-off and grass came from a one-off
enrichment pass that is no longer in the repo; a circuit added since needs
the same starting point before `ats-dress` (stands, buildings, pit lane,
graffiti from the dossier) and its groom (barriers, boards, trees) can
dress it. This lays only what neither of those owns:

- the start/finish marking at station 0, across the road;
- a 130 m grass band either side, the whole lap (as every other scene);
- a red/white curb on the **inside** of every corner over its apex, and
  on the **outside** of its exit, found from the centerline's own
  curvature (a corner is a sustained run tighter than `CORNER_RADIUS_M`).

Without `--force` an existing scene is left alone. With it, the scene's
surfaces and curbs are replaced and everything else (props, pit lane,
decals, markings, ids) is kept, so it can be re-run after a centerline
change. `ats-dress` rewrites the file in its own formatting afterwards.

`--from <file.json>` takes the curbs and bands from a survey instead of
guessing them (an importer that measured the real kerbs, gravel traps and
run-off, e.g. from an Assetto Corsa physics mesh), and implies `--force`:

    {"curbs":    [{"side": "left", "start_m": 120.0, "end_m": 160.5,
                   "width_m": 1.2, "style": "red_white"}, ...],
     "surfaces": [{"kind": "gravel", "side": "right", "start_m": 300.0,
                   "end_m": 380.0, "inner_m": 1.5, "width_m": 25.0,
                   "end_width_m": 10.0, "paint": null}, ...]}

Each entry is the `.ats` record without its `id` (ids are allocated
here); `width_m` defaults to 1.0 and `style` to `red_white` on a curb,
`inner_m` to 0 on a band. Either key may be left out, and the curbs or
bands for it are then generated as without `--from`. The 130 m grass
apron is always laid first and the authored bands after it, so they are
drawn and driven over it (the bake layers bands in order).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import yaml

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"

CORNER_RADIUS_M = 220.0
MIN_CORNER_M = 25.0
SMOOTH_M = 20.0
GRASS_WIDTH_M = 130.0
SIDES = ("left", "right")
SURFACE_KINDS = ("grass", "gravel", "asphalt_runoff", "concrete", "sand", "astroturf")


def centerline(data: dict):
    nodes = data["nodes"]
    p = np.array([[n["x"], n["y"]] for n in nodes], float)
    dw = data.get("default_width", 10.0)
    wl = np.array([n.get("width_left") or dw / 2 for n in nodes])
    wr = np.array([n.get("width_right") or dw / 2 for n in nodes])
    seg = np.hypot(*np.diff(np.vstack([p, p[:1]]), axis=0).T)
    s = np.concatenate([[0.0], np.cumsum(seg)])[:-1]
    total = float(seg.sum())
    d = np.roll(p, -1, 0) - np.roll(p, 1, 0)
    h = np.unwrap(np.arctan2(d[:, 1], d[:, 0]))
    ds = np.roll(s, -1) - np.roll(s, 1)
    ds[0] += total
    ds[-1] += total
    kappa = np.gradient(h) / np.maximum(ds / 2, 1e-3)
    step = total / len(p)
    k = max(int(SMOOTH_M / step), 1)
    pad = np.concatenate([kappa[-k:], kappa, kappa[:k]])
    kappa = np.convolve(pad, np.ones(k) / k, mode="same")[k:-k]
    return s, total, kappa, wl, wr


def corners(s, total, kappa):
    """(entry_m, apex_m, exit_m, sign) of every sustained corner."""
    tight = np.abs(kappa) > 1.0 / CORNER_RADIUS_M
    sign = np.sign(kappa)
    out = []
    n = len(s)
    i = 0
    # Start the scan on a straight so no corner is split by the wrap.
    start = int(np.argmin(np.abs(kappa)))
    idx = [(start + j) % n for j in range(n)]
    while i < n:
        a = idx[i]
        if not tight[a]:
            i += 1
            continue
        j = i
        while j + 1 < n and tight[idx[j + 1]] and sign[idx[j + 1]] == sign[a]:
            j += 1
        run = [idx[t] for t in range(i, j + 1)]
        length = (s[run[-1]] - s[run[0]]) % total
        if length >= MIN_CORNER_M:
            apex = run[int(np.argmax(np.abs(kappa[run])))]
            out.append((float(s[run[0]]), float(s[apex]), float(s[run[-1]]), int(sign[a]),
                        float(1.0 / np.abs(kappa[apex]))))
        i = j + 1
    return sorted(out, key=lambda c: c[1])


def authored(path: Path, total: float) -> dict:
    """The `--from` file's curbs and surfaces, checked and without ids."""
    data = json.loads(path.read_text(encoding="utf-8"))
    unknown = set(data) - {"curbs", "surfaces"}
    if unknown:
        raise SystemExit(f"{path.name}: unknown keys {sorted(unknown)} (curbs, surfaces)")

    def number(entry: dict, key: str, what: str) -> float:
        value = entry.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise SystemExit(f"{path.name}: {what} needs a number {key!r}: {entry}")
        return float(value)

    def side(entry: dict, what: str) -> str:
        if entry.get("side") not in SIDES:
            raise SystemExit(f"{path.name}: {what} needs side left|right: {entry}")
        return entry["side"]

    out = {}
    if "curbs" in data:
        out["curbs"] = [
            {
                "side": side(c, "curb"),
                "start_m": round(number(c, "start_m", "curb") % total, 2),
                "end_m": round(number(c, "end_m", "curb") % total, 2),
                "width_m": float(c.get("width_m", 1.0)),
                "style": str(c.get("style", "red_white")),
            }
            for c in data["curbs"]
        ]
    if "surfaces" in data:
        bands = []
        for b in data["surfaces"]:
            if b.get("kind") not in SURFACE_KINDS:
                raise SystemExit(f"{path.name}: band kind must be one of {SURFACE_KINDS}: {b}")
            width = number(b, "width_m", "band")
            if width <= 0:
                raise SystemExit(f"{path.name}: band needs a positive width_m: {b}")
            band = {
                "kind": b["kind"],
                "side": side(b, "band"),
                "start_m": round(number(b, "start_m", "band") % total, 2),
                "end_m": round(number(b, "end_m", "band") % total, 2),
                "inner_m": float(b.get("inner_m", 0.0)),
                "width_m": width,
                "end_width_m": None if b.get("end_width_m") is None else float(b["end_width_m"]),
            }
            if b.get("paint") is not None:
                band["paint"] = str(b["paint"])
            bands.append(band)
        out["surfaces"] = bands
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stem")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--from", dest="source", type=Path,
                    help="JSON with measured curbs and/or surfaces to lay instead of guessing")
    args = ap.parse_args()
    yaml_path = TRACK_DIR / f"{args.stem}.yaml"
    ats_path = TRACK_DIR / f"{args.stem}.ats"
    data = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    if ats_path.exists() and not (args.force or args.source):
        print(f"{ats_path.name} exists; --force to replace its curbs and grass")
        return 0
    s, total, kappa, wl, wr = centerline(data)
    given = authored(args.source, total) if args.source else {}

    if ats_path.exists():
        scene = json.loads(ats_path.read_text(encoding="utf-8"))
    else:
        scene = {
            "format": "apex-track-scene",
            "version": 2,
            "source_track": yaml_path.name,
            "track_name": data["name"],
            "surfaces": [],
            "curbs": [],
            "markings": [
                {
                    "id": 1,
                    "kind": "start_finish",
                    "start_m": 0.0,
                    "end_m": 1.0,
                    "lat_from_m": -round(float(wr[0]), 3),
                    "lat_to_m": round(float(wl[0]), 3),
                    "color": [1.0, 1.0, 1.0, 1.0],
                }
            ],
            "pit_lane": None,
            "props": [],
            "next_id": 2,
        }

    def alloc() -> int:
        scene["next_id"] += 1
        return scene["next_id"] - 1

    scene["surfaces"] = [
        {
            "id": alloc(),
            "kind": "grass",
            "side": side,
            "start_m": 0.0,
            "end_m": 0.0,
            "inner_m": 0.0,
            "width_m": GRASS_WIDTH_M,
            "end_width_m": None,
        }
        for side in SIDES
    ]
    scene["surfaces"] += [{"id": alloc(), **band} for band in given.get("surfaces", [])]

    if "curbs" in given:
        scene["curbs"] = [{"id": alloc(), **curb} for curb in given["curbs"]]
        origin = f"{len(scene['curbs'])} curbs from {args.source.name}"
    else:
        curbs = []
        found = corners(s, total, kappa)
        for entry, apex, exit_, sign, radius in found:
            inside = "left" if sign > 0 else "right"
            outside = "right" if sign > 0 else "left"
            width = 1.2 if radius < 120.0 else 1.0
            half = float(np.clip(((exit_ - entry) % total) * 0.35, 12.0, 45.0))
            curbs.append((inside, apex - half, apex + half, width))
            curbs.append((outside, apex + half * 0.4, (exit_ + 20.0), width))
        scene["curbs"] = [
            {
                "id": alloc(),
                "side": side,
                "start_m": round(a % total, 2),
                "end_m": round(b % total, 2),
                "width_m": w,
                "style": "red_white",
            }
            for side, a, b, w in curbs
        ]
        origin = f"{len(found)} corners, {len(curbs)} curbs"
    ats_path.write_text(json.dumps(scene, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{ats_path.name}: {origin}, {len(scene['surfaces'])} bands")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
