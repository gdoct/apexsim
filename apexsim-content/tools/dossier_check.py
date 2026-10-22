"""Re-apply a circuit's hand-authored dossier layers and show what they do.

    python tools/dossier_check.py <Stem> [<Stem> ...]

After editing a circuit's tables in osm_layout.py (MANUAL_STANDS,
STAND_COVER, MANUAL_LANDMARKS, ...): writes the dossier with
`osm_layout.py --manual-only` (MandarinaBay, which has no DEM sidecar, with
`--offline` from the cached extract), flags a STAND_COVER key that names no
stand, prints the stand and landmark lines of `ats-dress --explain` that
the edit changed (and its `check` lines: a stand laid against another leg), and lists the structures the dossier lost against HEAD
(an authored landmark drops every OSM structure within its reach) and
whether the dress pass laid them before. Needs the release ats-dress
(`cargo build --release --manifest-path track-editor/Cargo.toml --bin ats-dress`).
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import osm_layout as ol  # noqa: E402

DRESS = ROOT / "track-editor" / "target" / "release" / "ats-dress.exe"
if not DRESS.exists():
    DRESS = DRESS.with_suffix("")
KEEP = ("stand ", "landmark ", "skipped stand", "skipped landmark", "skipped building", "check ")


def explain(yaml: pathlib.Path) -> tuple[list[str], str]:
    out = subprocess.run([str(DRESS), "--dry-run", "--verbose", "--explain", str(yaml)],
                         capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout
    lines = out.splitlines()
    return [l.strip() for l in lines if l.strip().startswith(KEEP)], (lines[0] if lines else "")


def check(stem: str) -> None:
    folder = ol.track_dir(stem)
    dossier, yaml = folder / f"{stem}.layout.json", folder / f"{stem}.yaml"
    before, head_before = explain(yaml)
    mode = "--offline" if not (folder / f"{stem}.dem.msgpack").exists() else "--manual-only"
    run = subprocess.run([sys.executable, str(ROOT / "tools" / "osm_layout.py"), stem, mode],
                         capture_output=True, text=True, encoding="utf-8", cwd=ROOT)
    print(f"== {stem} ({mode})")
    print((run.stdout.strip().splitlines() or [""])[-1])
    layout = json.loads(dossier.read_text(encoding="utf-8"))
    names = {s.get("name") for s in layout["stands"]}
    stations = {f"@{s['station_m']}" for s in layout["stands"]}
    for key in ol.STAND_COVER.get(stem, {}):
        if key not in names and key not in stations:
            print(f"!! STAND_COVER names {key!r}, which no stand has")
    after, head_after = explain(yaml)
    print("before:", head_before.split(": ", 1)[-1])
    print("after: ", head_after.split(": ", 1)[-1])
    for l in sorted(set(before) - set(after)):
        print("  -", l)
    for l in sorted(set(after) - set(before)):
        print("  +", l)
    rel = dossier.relative_to(ROOT.parent).as_posix()
    head = subprocess.run(["git", "show", f"HEAD:{rel}"], capture_output=True, cwd=ROOT).stdout
    if head:
        now = {(s.get("name"), s["station_m"]) for s in layout["structures"]}
        for s in json.loads(head.decode("utf-8"))["structures"]:
            if (s.get("name"), s["station_m"]) not in now:
                name = s.get("name") or "(unnamed)"
                was = " - was skipped anyway" if f"skipped building {name}" in before else ""
                print(f"  lost structure {name} at {s['station_m']} {s['side']}{was}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for stem in sys.argv[1:]:
        check(stem)
