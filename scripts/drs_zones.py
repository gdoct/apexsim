#!/usr/bin/env python3
"""Write each circuit's DRS zones into its track YAML.

A zone is three stations along the centerline: the *detection* point
(where the gap to the car ahead is measured), the *activation* point
(from where the flap may be opened) and the *end* (the braking point of
the next corner, where it closes). The server enforces the rule
(`server/src/drs.rs`), the AI uses it, the racing line profiles it, and
the bake paints the detection and activation lines and stands a board
at each (`ue_export::drs`).

The zones are the real ones, from the FIA event notes of the last DRS
season each circuit ran: given as "N m before/after turn K", with the
turns in the FIA's numbering. The turns themselves are found here from
the centerline — a sustained run of curvature is a corner — so every
zone is expressed as `(reference corner, offset)` and snapped onto the
corner runs this script detects, which is what keeps the zones on the
road after `ats-smooth` moves it. The corner each entry refers to is
named in `ZONES` by an *approximate station* (the turn's rough place
along the lap, read off the layout), and the nearest detected corner run
to it is the one used. `--report` prints every circuit's detected
corners so a new entry can be written.

    python scripts/drs_zones.py --all             # write drs_zones: into every YAML
    python scripts/drs_zones.py Monza --dry-run   # print what Monza would get
    python scripts/drs_zones.py --report Monza    # the detected corners, to author against

The YAML is edited in place as text: only the `drs_zones:` block is
replaced, nothing else in the file is touched (rewriting 14 000 lines
through a YAML dumper would churn every float).
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np
import yaml

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"

#: Curvature above this, sustained, is a corner (radius 400 m).
CORNER_KAPPA = 1.0 / 400.0
#: A corner run shorter than this is a kink, not a turn.
MIN_CORNER_M = 40.0
#: Two runs closer than this are one corner (a chicane's two halves are
#: still two corners: they turn opposite ways).
MERGE_GAP_M = 15.0
#: Window the curvature is averaged over, metres, to ride out the trace.
SMOOTH_M = 24.0
#: A corner starting within this of the activation point is the one the
#: zone runs out of, not the one it ends at.
END_CLEAR_M = 60.0


def load(stem: str) -> dict:
    with open(TRACK_DIR / f"{stem}.yaml", encoding="utf-8") as f:
        return yaml.safe_load(f)


class Corners:
    """The corner runs of a centerline: `(entry_m, exit_m, sign)`."""

    def __init__(self, data: dict):
        pts = np.array([[n["x"], n["y"]] for n in data["nodes"]], dtype=float)
        closed = data.get("closed_loop", True)
        if closed:
            pts = np.vstack([pts, pts[:1]])
        seg = np.hypot(*np.diff(pts, axis=0).T)
        self.station = np.concatenate([[0.0], np.cumsum(seg)])
        self.total = float(self.station[-1])
        self.closed = closed
        # Heading and curvature per node, smoothed.
        d = np.diff(pts, axis=0)
        heading = np.unwrap(np.arctan2(d[:, 1], d[:, 0]))
        mid = (self.station[:-1] + self.station[1:]) / 2
        kappa = np.gradient(heading, mid)
        w = max(1, int(SMOOTH_M / max(np.median(seg), 1e-3)))
        kernel = np.ones(w) / w
        if closed:
            kappa = np.convolve(np.concatenate([kappa[-w:], kappa, kappa[:w]]), kernel, "same")[w:-w]
        else:
            kappa = np.convolve(kappa, kernel, "same")
        self.mid = mid
        self.kappa = kappa
        self.runs = self._runs()

    def _runs(self) -> list[tuple[float, float, int]]:
        runs = []
        on = None
        sign = 0
        for s, k in zip(self.mid, self.kappa):
            here = int(np.sign(k)) if abs(k) >= CORNER_KAPPA else 0
            if here and on is None:
                on, sign = s, here
            elif on is not None and here != sign:
                runs.append((on, s, sign))
                on = (s if here else None)
                sign = here
        if on is not None:
            runs.append((on, float(self.mid[-1]), sign))
        # Merge same-sign runs separated by a short gap, drop kinks.
        merged: list[list] = []
        for a, b, sg in runs:
            if merged and merged[-1][2] == sg and a - merged[-1][1] <= MERGE_GAP_M:
                merged[-1][1] = b
            else:
                merged.append([a, b, sg])
        return [(a, b, sg) for a, b, sg in merged if b - a >= MIN_CORNER_M]

    def nearest(self, station: float) -> tuple[float, float, int]:
        def gap(run):
            c = (run[0] + run[1]) / 2
            d = abs(c - station)
            return min(d, self.total - d) if self.closed else d
        return min(self.runs, key=gap)

    def wrap(self, s: float) -> float:
        return s % self.total if self.closed else min(max(s, 0.0), self.total)


# Zone entries: (detection, activation), each of the form
#   ("before", corner_station, metres)  metres before the corner's entry
#   ("after", corner_station, metres)   metres after the corner's exit
#   ("sfl", metres)                     metres after the start/finish line
#   ("at", station)                     a station outright
# The zone ends at the entry of the first corner after activation, unless a
# third spec names the end outright (a flat-out curve the zone runs through).
# `corner_station` is the approximate station of the FIA-numbered turn
# the notes refer to (see --report); the nearest detected corner is used.
# Sources: FIA event notes as summarised by formularapida.net /
# f1technical.net for the last DRS season of each circuit; entries marked
# `recalled` had only the turns sourced, the metres are from memory.
ZONES: dict[str, list[tuple]] = {
    "Monza": [
        (("before", 2877, 95), ("after", 2877, 170)),    # T7 Lesmo 2 -> Ascari
        (("after", 5290, 20), ("sfl", 120)),            # T11 Parabolica -> SFL
    ],
    "Spa": [
        (("before", 1056, 240), ("after", 1290, 305)),   # T2 Eau Rouge det, T4 act: Kemmel
        (("before", 6743, 160), ("after", 6810, 30)),    # T18/T19 chicane -> pit straight
    ],
    "Zandvoort": [
        (("before", 2565, 0), ("after", 2565, 50)),      # T10 -> T11
        (("after", 3269, 20), ("after", 3606, 40), ("at", 342)),  # T12 det, T13 act, through T14 to T1
    ],
    "Silverstone": [
        (("before", 900, 25), ("after", 1204, 30)),      # T3 Village det, T5 Aintree act: Wellington
        (("before", 3628, 0), ("after", 4040, 0), ("at", 4958)),  # T11 det, T14 Chapel act: Hangar to Stowe
    ],
    "Spielberg": [
        (("before", 451, 160), ("after", 451, 102)),     # T1
        (("before", 1392, 40), ("after", 1392, 100)),    # T3 Remus
        (("before", 4006, 120), ("after", 4100, 106)),   # T10 -> pit straight
    ],
    "Suzuka": [
        (("before", 5391, 50), ("sfl", -100)),           # T16 chicane det, 100 m before SFL act
    ],
    "Austin": [
        (("after", 2187, 150), ("after", 2590, 345)),    # T10 det, T11 act: back straight
        (("after", 4760, 65), ("after", 5344, 80)),      # T18 det, T20 act: pit straight
    ],
    "Budapest": [
        (("before", 4104, 5), ("after", 4104, 40)),      # T14 -> pit straight
        (("before", 4104, 5), ("after", 734, 6)),        # shared det, T1 -> T2
    ],
    "Catalunya": [
        (("before", 2912, 86), ("after", 2912, 40)),     # T9 -> T10
        (("before", 4335, 60), ("after", 4335, 162)),    # T14 -> pit straight (det at SC line 1)
    ],
    "Sakhir": [
        (("before", 741, 50), ("after", 959, 23)),       # T1 det, T3 act
        (("before", 2673, 10), ("after", 2673, 50)),     # T9 det, T10 act
        (("before", 4923, 110), ("after", 5006, 170)),   # T14 det, T15 act: pit straight
    ],
    "Melbourne": [
        (("before", 4678, 90), ("after", 489, 30)),      # T13 det (shared), T2 act: to T3
        (("before", 4678, 90), ("after", 4829, 30)),     # shared det, T14 act: pit straight
    ],
    "Montreal": [
        (("after", 1033, 15), ("after", 1371, 95)),      # T5 det, T7 act
        (("before", 3212, 110), ("after", 3212, 80), ("at", 3876)),  # T9 det (shared), Casino straight to T13
        (("before", 3212, 110), ("after", 3941, 70)),    # shared det, T14 act: pit straight
    ],
    "Shanghai": [
        (("before", 3149, 0), ("after", 3387, 375)),     # T12 det, T13 act: back straight
        (("before", 5169, 35), ("after", 5169, 98)),     # T16 -> pit straight
    ],
    "SaoPaulo": [
        (("at", 454), ("after", 633, 30)),               # T2 apex det, T3 act: Reta Oposta
        (("after", 3391, 30), ("before", 4031, 160), ("at", 272)),  # T13 det, 160 m before T15, to T1
    ],
    "MexicoCity": [
        (("before", 3876, 200), ("after", 3876, 240)),   # T15 det (shared), T17 act: pit straight
        (("before", 3876, 200), ("after", 1257, 115)),   # shared det, T5 act: to T6
    ],
    "YasMarina": [                                       # pre-2021 layout in the data
        (("before", 1530, 60), ("after", 1530, 100)),    # T7 hairpin -> T8
        (("before", 3066, 40), ("after", 3066, 60)),     # T13 -> T14
    ],
    "Sochi": [                                           # recalled
        (("before", 2929, 15), ("after", 2929, 90)),     # T10 -> T13
        (("before", 5735, 25), ("after", 5735, 155)),    # T18 -> T2
    ],
    "Sepang": [                                          # 2017
        (("after", 3415, 54), ("after", 4130, 104)),     # T12 det, T14 act: back straight
        (("after", 3415, 54), ("after", 5160, 28)),      # shared det, T15 act: pit straight
    ],
    "Hockenheim": [                                      # recalled metres
        (("sfl", 45), ("after", 272, 70), ("at", 807)),  # T1 -> T2
        (("before", 985, 50), ("after", 985, 40)),       # T4 -> Parabolika -> T6
    ],
    "Nuerburgring": [                                    # recalled metres
        (("before", 3843, 40), ("after", 3843, 50)),     # T10/T11 -> NGK chicane
        (("before", 4625, 70), ("after", 4625, 90)),     # T15 -> pit straight
    ],
    # DTM ran one zone on the pit straight at these.
    "Norisring": [(("before", 1878, 40), ("after", 1878, 60), ("at", 60))],
    "Oschersleben": [(("before", 3330, 40), ("after", 3330, 60))],
    "MoscowRaceway": [(("before", 3923, 40), ("after", 3923, 60))],
    "BrandsHatch": [(("before", 3663, 40), ("after", 3663, 60), ("at", 208))],
}


def resolve(corners: Corners, spec: tuple) -> float:
    kind = spec[0]
    if kind == "sfl":
        return corners.wrap(spec[1])
    if kind == "at":
        return corners.wrap(spec[1])
    _, station, metres = spec
    entry, exit_, _ = corners.nearest(station)
    if kind == "before":
        return corners.wrap(entry - metres)
    if kind == "after":
        return corners.wrap(exit_ + metres)
    raise ValueError(spec)


def zone_end(corners: Corners, activation: float) -> float:
    """The entry of the first corner after activation — past the corner
    the activation point itself follows out of (a flat-out exit curve
    within `END_CLEAR_M` is part of the run, not its end)."""
    ahead = []
    for entry, _, _ in corners.runs:
        d = entry - activation
        if corners.closed:
            d %= corners.total
        if d > END_CLEAR_M:
            ahead.append((d, entry))
    if not ahead:
        return corners.wrap(activation + 300.0)
    return corners.wrap(activation + min(ahead)[0])


def zones_for(stem: str, data: dict) -> list[dict]:
    corners = Corners(data)
    out = []
    for zone in ZONES.get(stem, []):
        det, act = zone[0], zone[1]
        d = resolve(corners, det)
        a = resolve(corners, act)
        e = resolve(corners, zone[2]) if len(zone) > 2 else zone_end(corners, a)
        out.append({
            "detection_m": round(float(d), 1),
            "start_m": round(float(a), 1),
            "end_m": round(float(e), 1),
        })
    return out


def write_yaml(stem: str, zones: list[dict]) -> bool:
    path = TRACK_DIR / f"{stem}.yaml"
    text = path.read_text(encoding="utf-8")
    block = "drs_zones:\n" + "".join(
        f"- detection_m: {z['detection_m']}\n  start_m: {z['start_m']}\n  end_m: {z['end_m']}\n"
        for z in zones
    ) if zones else "drs_zones: []\n"
    # Replace an existing block, else insert before `metadata:` (or append).
    # The block ends at the next top-level *key*: its own `- detection_m`
    # items also start in column 0, and stopping at those replaced only the
    # header, so every re-run appended another copy of the zones.
    pattern = re.compile(r"^drs_zones:.*?(?=^[A-Za-z_]|\Z)", re.S | re.M)
    if pattern.search(text):
        new = pattern.sub(lambda _m: block, text, count=1)
    elif re.search(r"^metadata:", text, re.M):
        new = re.sub(r"^metadata:", block + "metadata:", text, count=1, flags=re.M)
    else:
        new = text.rstrip("\n") + "\n" + block
    if new == text:
        return False
    path.write_text(new, encoding="utf-8", newline="\n")
    return True


def report(stem: str, data: dict) -> None:
    corners = Corners(data)
    layout = TRACK_DIR / f"{stem}.layout.json"
    names = []
    if layout.exists():
        import json
        names = [(c.get("station_m", 0.0), c.get("name", "")) for c in json.loads(layout.read_text(encoding="utf-8")).get("corners", [])]
    print(f"{stem}: lap {corners.total:.0f} m, {len(corners.runs)} corners")
    for i, (a, b, sg) in enumerate(corners.runs, 1):
        near = ""
        if names:
            st, nm = min(names, key=lambda c: min(abs(c[0] - (a + b) / 2), corners.total - abs(c[0] - (a + b) / 2)))
            if min(abs(st - (a + b) / 2), corners.total - abs(st - (a + b) / 2)) < 120:
                near = f"  ~ {nm}"
        print(f"  #{i:>2} {'L' if sg > 0 else 'R'} {a:7.0f}-{b:<7.0f} ({b - a:4.0f} m){near}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", action="store_true", help="print the detected corners instead")
    args = ap.parse_args()
    stems = sorted(p.stem for p in TRACK_DIR.glob("*.yaml")) if args.all else args.tracks
    if not stems:
        raise SystemExit("no tracks given; pass a stem or --all")
    written = 0
    for stem in stems:
        data = load(stem)
        if args.report:
            report(stem, data)
            continue
        zones = zones_for(stem, data)
        desc = ", ".join(f"det {z['detection_m']:.0f} act {z['start_m']:.0f} end {z['end_m']:.0f}" for z in zones) or "none"
        print(f"{stem}: {len(zones)} zone(s): {desc}")
        if not args.dry_run and write_yaml(stem, zones):
            written += 1
    if not args.report:
        print(f"{written} file(s) written")


if __name__ == "__main__":
    main()
