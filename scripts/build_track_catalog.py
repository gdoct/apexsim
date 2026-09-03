#!/usr/bin/env python3
"""
Bake the track catalog manifest and preview images for the Unreal client.

Reads every track YAML under content/tracks/real, draws a top-down outline
for each (reusing generate_track_previews.py) and writes a manifest the
`ApexTrackCatalogSync` commandlet turns into `DT_TrackCatalog` rows and
`T_Track_<Stem>` textures:

    python scripts/build_track_catalog.py            # every track
    python scripts/build_track_catalog.py LeMans     # one or more stems

Outputs (both gitignored build products, regenerated wholesale):

    content/tracks/export/previews/<Stem>.png
    content/tracks/export/track_catalog.json

A track without a `track_id` is skipped with a warning: the catalog is keyed
by that id, and without one the server mints a fresh UUID on every start, so
no row could ever match it.
"""

import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_track_previews import generate_preview_image, parse_track_file  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO_ROOT / "content" / "tracks" / "real"
EXPORT_DIR = REPO_ROOT / "content" / "tracks" / "export"
PREVIEW_DIR = EXPORT_DIR / "previews"
MANIFEST = EXPORT_DIR / "track_catalog.json"


def polyline_length(points, closed):
    total = 0.0
    for i in range(len(points) - 1):
        total += math.dist(points[i], points[i + 1])
    if closed and len(points) > 1:
        total += math.dist(points[-1], points[0])
    return total


def main(argv):
    wanted = {name.lower() for name in argv}
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)

    entries = []
    skipped = []
    for yaml_path in sorted(TRACK_DIR.glob("*.yaml")):
        stem = yaml_path.stem
        if wanted and stem.lower() not in wanted:
            continue

        track, _ = parse_track_file(yaml_path)
        if not track or not track["points"]:
            skipped.append(f"{stem}: no nodes")
            continue
        if not track["id"]:
            skipped.append(f"{stem}: no track_id in the YAML — add one, or the server "
                           "will mint a new id on every start and no catalog row can match")
            continue

        import yaml  # already a dependency of generate_track_previews
        with open(yaml_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
        meta = data.get("metadata") or {}

        png = PREVIEW_DIR / f"{stem}.png"
        if not generate_preview_image(track, png):
            skipped.append(f"{stem}: preview failed")
            continue

        length_m = meta.get("length_m")
        if not length_m:
            length_m = round(polyline_length(track["points"], bool(data.get("closed_loop", True))), 1)

        entries.append({
            "track_id": track["id"],
            "stem": stem,
            "display_name": track["name"] or stem,
            "country": meta.get("country") or "",
            "city": meta.get("city") or "",
            "category": meta.get("category") or "",
            "environment_type": meta.get("environment_type") or "",
            "length_m": float(length_m),
            "preview_png": str(png),
        })
        print(f"  {stem}: {track['name']} ({len(track['points'])} points, {length_m:.0f} m)")

    if wanted and not entries:
        print(f"no track matched {sorted(wanted)} in {TRACK_DIR}", file=sys.stderr)
        return 1

    # A filtered run must not throw away the other tracks' manifest entries.
    existing = []
    if wanted and MANIFEST.exists():
        with open(MANIFEST, "r", encoding="utf-8") as f:
            existing = [e for e in json.load(f) if e["stem"].lower() not in wanted]

    merged = sorted(existing + entries, key=lambda e: e["stem"].lower())
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2)
        f.write("\n")

    for line in skipped:
        print(f"  skipped {line}", file=sys.stderr)
    print(f"wrote {len(merged)} track(s) to {MANIFEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
