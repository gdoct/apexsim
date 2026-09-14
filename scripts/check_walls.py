#!/usr/bin/env python3
"""Sanity-scan a track's baked walls for scenery standing on the road.

`ats-export` writes `<Stem>.walls.msgpack` next to the track YAML
(`server/src/walls.rs`: `WallFile { version, segments: Vec<WallSegment> }`,
MessagePack via `rmp_serde::to_vec_named`, so every segment is a map with
named fields `x0, y0, x1, y1, z, height_m, kind`) -- one line segment per
barrier face, stand side, building side and pit wall, in the track's own
frame (metres, +Y left).

A segment is scenery standing on the track if its midpoint falls *inside*
the road at its own station: within the half-width on the side it is on,
and within 2 m of road height there. This script locates every segment's
midpoint against the YAML centerline with the same nearest-point logic as
`osm_layout.Track.locate` (imported from there, so the two never drift
apart) and flags any that qualify.

    python scripts/check_walls.py Oschersleben       # one track
    python scripts/check_walls.py --all              # every track with a
                                                       # walls.msgpack sidecar
    python scripts/check_walls.py Oschersleben --z-tolerance 2.0

Exits non-zero (and prints a report) if any segment is flagged. A handful of
flags right at a pit lane's entry/exit taper is a known cosmetic issue --
still reported, not silently excused, but §4.3 of
`docs/ADDITIONAL_TRACKS.md` allows it as long as it is called out by hand in
the hand-back.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import msgpack
import numpy as np

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from osm_layout import Track  # noqa: E402  (after sys.path tweak)

Z_TOLERANCE_M = 2.0


def load_walls(stem: str) -> list[dict]:
    path = TRACK_DIR / f"{stem}.walls.msgpack"
    if not path.exists():
        raise SystemExit(
            f"{stem}: no {path.name} -- run ats-export for this track first"
        )
    with open(path, "rb") as f:
        data = msgpack.unpack(f, raw=False)
    if data.get("version") != 1:
        raise SystemExit(f"{stem}: unsupported walls.msgpack version {data.get('version')}")
    return data["segments"]


def check_track(stem: str, z_tolerance: float, verbose: bool) -> list[dict]:
    track = Track(stem)
    segments = load_walls(stem)
    if not segments:
        print(f"{stem}: 0 segments in walls.msgpack")
        return []

    mid = np.array(
        [[(s["x0"] + s["x1"]) / 2.0, (s["y0"] + s["y1"]) / 2.0] for s in segments]
    )
    station, lateral = track.locate(mid)

    flagged = []
    for seg, s, lat in zip(segments, station, lateral):
        i = track.index_at(float(s))
        half_left = float(track.width_left[i])
        half_right = float(track.width_right[i])
        on_road_laterally = (
            (0.0 <= lat < half_left) if lat >= 0 else (-half_right < lat < 0)
        )
        if not on_road_laterally:
            continue
        # Road height at this station: the centerline's own z, interpolated
        # the same way the width arrays are (station-indexed nodes).
        road_z = float(_road_z(track, float(s)))
        dz = seg["z"] - road_z
        if abs(dz) < z_tolerance:
            flagged.append(
                dict(
                    x0=seg["x0"], y0=seg["y0"], x1=seg["x1"], y1=seg["y1"],
                    z=seg["z"], height_m=seg["height_m"], kind=seg["kind"],
                    station_m=round(float(s), 1), lateral_m=round(float(lat), 2),
                    dz_m=round(dz, 2),
                )
            )

    print(
        f"{stem}: {len(segments)} segment(s), {len(flagged)} flagged "
        f"(z tolerance {z_tolerance} m)"
    )
    if flagged:
        near_pit = _near_pit_lane_taper(track, flagged)
        for f, is_taper in zip(flagged, near_pit):
            tag = " [pit lane taper]" if is_taper else ""
            print(
                f"    station {f['station_m']:>7.1f}m lateral {f['lateral_m']:+6.2f}m "
                f"dz {f['dz_m']:+5.2f}m kind={f['kind']} "
                f"({f['x0']:.1f},{f['y0']:.1f})-({f['x1']:.1f},{f['y1']:.1f}){tag}"
            )
        n_taper = sum(near_pit)
        if n_taper:
            print(
                f"    {n_taper} of {len(flagged)} flagged segment(s) are within 60 m "
                "of the pit lane's entry/exit nodes -- a known cosmetic issue "
                "(docs/ADDITIONAL_TRACKS.md §4.3), still reported, not excused."
            )
    elif verbose:
        print("    clean")
    return flagged


def _road_z(track: Track, station: float) -> float:
    """Ground z at a station, from the YAML nodes (same stations the width
    arrays are resampled onto)."""
    nodes = track.data["nodes"]
    z = np.array([n.get("z", 0.0) for n in nodes])
    # Same resampling as __init__: raw node stations -> track.station grid.
    raw = np.array([[n["x"], n["y"]] for n in nodes])
    seg = np.hypot(
        *np.diff(
            np.vstack([raw, raw[:1]]) if track.closed else raw, axis=0
        ).T
    )
    s_raw = np.concatenate([[0.0], np.cumsum(seg)])[: len(raw)]
    return float(np.interp(station % track.total, s_raw, z, period=track.total))


def _near_pit_lane_taper(track: Track, flagged: list[dict], range_m: float = 60.0) -> list[bool]:
    layout_path = TRACK_DIR / f"{track.stem}.layout.json"
    if not layout_path.exists():
        return [False] * len(flagged)
    import json

    layout = json.loads(layout_path.read_text(encoding="utf-8"))
    pit = layout.get("pit_lane")
    if not pit or not pit.get("nodes"):
        return [False] * len(flagged)
    ends = np.array([pit["nodes"][0], pit["nodes"][-1]])
    s_ends, _ = track.locate(ends)
    out = []
    for f in flagged:
        s = f["station_m"]
        d = min(
            min(abs(s - e), track.total - abs(s - e)) for e in s_ends
        )
        out.append(d < range_m)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracks", nargs="*", help="track stem(s), e.g. Oschersleben")
    ap.add_argument("--all", action="store_true", help="check every track with a walls.msgpack sidecar")
    ap.add_argument("--z-tolerance", type=float, default=Z_TOLERANCE_M, help=f"metres (default {Z_TOLERANCE_M})")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.all:
        stems = sorted(p.stem.rsplit(".walls", 1)[0] for p in TRACK_DIR.glob("*.walls.msgpack"))
    else:
        stems = args.tracks
    if not stems:
        raise SystemExit("no tracks given; pass a stem or --all")

    total_flagged = 0
    for stem in stems:
        total_flagged += len(check_track(stem, args.z_tolerance, args.verbose))

    if total_flagged:
        print(f"\nFAIL: {total_flagged} flagged segment(s) across {len(stems)} track(s)")
        sys.exit(1)
    print(f"\nOK: 0 flagged segments across {len(stems)} track(s)")


if __name__ == "__main__":
    main()
