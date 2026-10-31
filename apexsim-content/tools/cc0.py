"""CC0 texture sources: fetch, verify and unpack them.

    python tools/cc0.py              # fetch what is missing, check every hash
    python tools/cc0.py --pin ID     # record a new source's hash (first fetch)

The sources are listed in `textures/cc0/sources.json`: each an id, the
ambientCG download, the zip's SHA-256 and its licence (CC0 1.0: no
attribution needed, recorded anyway). Downloads go to `.cache/cc0/` and are
never committed; the generators that use them (`color_map`) write their own
derived textures, which are. A fresh clone needs this only to rebuild those.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / "textures" / "cc0" / "sources.json"
CACHE = ROOT / ".cache" / "cc0"


def sources() -> dict[str, dict]:
    return {s["id"]: s for s in json.loads(SOURCES.read_text(encoding="utf-8"))["sources"]}


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(source_id: str, pin: bool = False) -> Path:
    """The source's unpacked folder, downloading and checking it first."""
    src = sources()[source_id]
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / f"{source_id}.zip"
    if not archive.exists():
        print(f"  fetching {source_id}")
        request = urllib.request.Request(src["url"], headers={"User-Agent": "apexsim-content"})
        with urllib.request.urlopen(request, timeout=300) as r:
            archive.write_bytes(r.read())
    digest = _sha256(archive)
    if pin:
        data = json.loads(SOURCES.read_text(encoding="utf-8"))
        for s in data["sources"]:
            if s["id"] == source_id:
                s["sha256"] = digest
        SOURCES.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
    elif digest != src.get("sha256"):
        archive.unlink()
        raise SystemExit(f"{source_id}: hash {digest} is not the pinned {src.get('sha256')}; download removed")
    folder = CACHE / source_id
    if not folder.exists():
        with zipfile.ZipFile(archive) as z:
            z.extractall(folder)
    return folder


def color_map(source_id: str) -> Path:
    """The source's base-colour image."""
    folder = fetch(source_id)
    found = sorted(folder.glob("*_Color.*"))
    if not found:
        raise SystemExit(f"{source_id}: no *_Color map in {folder}")
    return found[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pin", metavar="ID", help="record the hash of a source fetched for the first time")
    args = ap.parse_args()
    if args.pin:
        fetch(args.pin, pin=True)
        print(f"{args.pin}: pinned")
        return 0
    for source_id in sources():
        fetch(source_id)
        print(f"{source_id}: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
