#!/usr/bin/env python3
"""Where each circuit is: `metadata.altitude_m`, `latitude_deg` and
`longitude_deg` in its track YAML, from its elevation sidecar.

`<Stem>.dem.msgpack` (scripts/dem_fetch.py) is georeferenced onto the
track's frame: its `georef` carries the origin's latitude and longitude and
`datum_offset_m`, the YAML's z at the start line less the Copernicus height
there (dem_fetch.py), so the start line stands `z0 - datum_offset_m` above
sea level. The server thins the session's air by that height
(`SessionConditions::air_density_ratio`). A shipped circuit without a
sidecar takes its published elevation from `MANUAL`.

    python scripts/track_location.py --all        # every track with a DEM
    python scripts/track_location.py Spielberg    # or a few, [--dry-run]

The keys go at the end of the metadata block, in the order and the float
format (an f32's shortest form) the track editor writes them, so a YAML the
Rust tools rewrite comes back byte-identical. A track without a sidecar is
left alone: the server takes it to be at sea level.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import msgpack
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from track_dirs import TRACK_DIRS, track_dir  # noqa: E402

KEYS = ("altitude_m", "latitude_deg", "longitude_deg")

#: Circuits with no elevation sidecar: published elevation, m, and position
#: (approximate; the height is what matters, and to the density a few tens
#: of metres are well under a percent). None leaves a key out.
MANUAL: dict[str, tuple[float, float | None, float | None]] = {
    "MexicoCity": (2240.0, 19.4042, -99.0907),
    "IMS": (218.0, 39.795, -86.2347),
    "YasMarina": (3.0, 24.4672, 54.6031),
    "MoscowRaceway": (190.0, None, None),
}


def f32(value: float, places: int) -> str:
    """`value` rounded to `places`, written as an f32's shortest form (what
    serde_yaml writes for an f32 field)."""
    return np.format_float_positional(np.float32(round(value, places)), unique=True, trim="0")


def georef(stem: str) -> dict | None:
    path = track_dir(stem) / f"{stem}.dem.msgpack"
    if not path.is_file():
        return None
    data = msgpack.unpackb(path.read_bytes(), raw=False)
    return data.get("georef")


def first_node_z(text: str) -> float:
    """The z of the YAML's first centerline node: where the DEM's datum was
    matched."""
    m = re.search(r"^nodes:\s*\n-\s(?:.*\n)*?\s+z:\s*([-0-9.eE]+)", text, re.M)
    if not m:
        raise SystemExit("no first node z in the YAML")
    return float(m.group(1))


def location(ref: dict, z0: float) -> dict[str, str]:
    return {
        "altitude_m": f32(z0 - float(ref["datum_offset_m"]), 1),
        "latitude_deg": f32(float(ref["lat0"]), 5),
        "longitude_deg": f32(float(ref["lon0"]), 5),
    }


def manual_location(stem: str) -> dict[str, str] | None:
    if stem not in MANUAL:
        return None
    altitude, lat, lon = MANUAL[stem]
    out = {"altitude_m": f32(altitude, 1)}
    if lat is not None:
        out["latitude_deg"] = f32(lat, 5)
    if lon is not None:
        out["longitude_deg"] = f32(lon, 5)
    return out


def with_location(text: str, values: dict[str, str]) -> str:
    """The YAML with the three keys at the end of its `metadata:` block
    (replacing any there), or unchanged when it has no metadata block."""
    lines = text.split("\n")
    try:
        start = next(i for i, line in enumerate(lines) if line.rstrip() == "metadata:")
    except StopIteration:
        return text
    end = start + 1
    while end < len(lines) and (lines[end].startswith("  ") or lines[end] == ""):
        end += 1
    # Trailing blank lines belong after the block.
    while end > start + 1 and lines[end - 1] == "":
        end -= 1
    body = [line for line in lines[start + 1:end]
            if not re.match(r"^  (%s):" % "|".join(KEYS), line)]
    body += [f"  {key}: {values[key]}" for key in KEYS if key in values]
    return "\n".join(lines[:start + 1] + body + lines[end:])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stems", nargs="*")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.all:
        stems = sorted({p.stem for folder in TRACK_DIRS for p in folder.glob("*.yaml")})
    else:
        stems = args.stems
    if not stems:
        parser.error("name a track or pass --all")

    changed = 0
    for stem in stems:
        path = track_dir(stem) / f"{stem}.yaml"
        text = path.read_text(encoding="utf-8")
        ref = georef(stem)
        values = location(ref, first_node_z(text)) if ref is not None else manual_location(stem)
        if values is None:
            print(f"  {stem:28} no elevation sidecar: left at sea level")
            continue
        new = with_location(text, values)
        state = "unchanged" if new == text else ("would write" if args.dry_run else "written")
        print(f"  {stem:28} {values['altitude_m']:>7} m  {values.get('latitude_deg', '-'):>9} N "
              f"{values.get('longitude_deg', '-'):>10} E  {state}")
        if new != text and not args.dry_run:
            path.write_text(new, encoding="utf-8", newline="\n")
            changed += 1
    print(f"{changed} track(s) written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
