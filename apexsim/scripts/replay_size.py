"""What spectator stream files (.apxs) take, and what this build would make of them.

    python scripts/replay_size.py build/showcase/*.apxs
    python scripts/replay_size.py --repack out/replay_size build/showcase/*.apxs

For each file: its container version, size, rate per second of race and per
16-car hour, and its records' CRC (`apexsim-replay info`). With --repack,
each is also rewritten as this build writes it (`apexsim-replay repack`,
docs/game/spectator.md, the file) into the folder given, and the two sizes
are set side by side; a repacked file whose records differ is an error.
A change to the row or block layout is measured with this, before and after.

Needs `cargo build --release` in server/server/ (for apexsim-replay).
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "server" / "server" / "target" / "release" / "apexsim-replay"


def info(exe: Path, path: Path) -> dict:
    out = subprocess.run([str(exe), "info", str(path)], capture_output=True, text=True,
                         stdin=subprocess.DEVNULL, check=True)
    return json.loads(out.stdout)


def line(name: str, i: dict) -> str:
    size = i["file_bytes"]
    seconds = max(i["duration_s"], 1e-3)
    cars = max(len(i["cars"]), 1)
    hour16 = size / seconds * 3600 * 16 / cars
    return (f"{name:44} v{i.get('file_version', 1)} {size / 1e6:7.2f} MB "
            f"{size / seconds / 1e3:6.1f} kB/s {hour16 / 1e6:6.1f} MB/16-car hour "
            f"crc {i.get('records_crc', 0):08x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--repack", type=Path, help="folder to write the repacked files to")
    parser.add_argument("--exe", type=Path, default=EXE, help="apexsim-replay to run")
    args = parser.parse_args()

    total_in = total_out = 0
    bad = 0
    for path in args.files:
        before = info(args.exe, path)
        total_in += before["file_bytes"]
        print(line(path.name, before))
        if not args.repack:
            continue
        args.repack.mkdir(parents=True, exist_ok=True)
        dest = args.repack / path.name
        subprocess.run([str(args.exe), "repack", str(path), "--out", str(dest)],
                       capture_output=True, stdin=subprocess.DEVNULL, check=True)
        after = info(args.exe, dest)
        total_out += after["file_bytes"]
        same = after["records_crc"] == before["records_crc"]
        bad += not same
        print(line("  repacked", after) + ("" if same else "  RECORDS DIFFER"))
    if args.repack:
        print(f"total {total_in / 1e6:.1f} MB -> {total_out / 1e6:.1f} MB "
              f"({total_out / max(total_in, 1):.0%})")
    else:
        print(f"total {total_in / 1e6:.1f} MB")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
