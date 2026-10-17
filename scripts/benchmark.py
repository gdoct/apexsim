#!/usr/bin/env python3
"""The benchmark: the same seeded race on every machine, filmed by the TV
camera, timed frame by frame.

    python scripts/benchmark.py                      # Spa, 1920x1080, the settings the game has
    python scripts/benchmark.py --track Monza --res 2560x1440
    python scripts/benchmark.py --rerender           # the clip again (after a physics or track change)

The clip is a race `apexsim-replay render` simulates from a fixed seed
(12 GT3s, sunny, 13:00), kept in build/benchmark/<Track>.apxs: identical
wherever it is rendered from the same commit, so two machines, or two
builds on one, film the same cars through the same corners. The game
plays it with `-ApexReplayBench` (docs/game/client.md): one minute from 20 s
in, the TV director on its seed, and writes the mean fps, the 1% low and
the frame-time percentiles with the GPU's name to
game-unreal/Saved/Benchmarks/, which this prints.

Needs `cargo build --release` in server/ (for apexsim-replay), the
ApexSimEditor target built, the content initialised and the editor closed.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts" / "site"))
from make_shots import UPROJECT, engine_editor  # noqa: E402

REPLAY_TOOL = REPO / "server" / "target" / "release" / "apexsim-replay.exe"
OUT = REPO / "build" / "benchmark"
RESULTS = REPO / "game-unreal" / "Saved" / "Benchmarks"
SEED = 7


def render_clip(track: str, clip: Path) -> None:
    yaml = REPO / "content" / "tracks" / "default" / track / f"{track}.yaml"
    if not yaml.is_file():
        raise SystemExit(f"no track {yaml}")
    if not REPLAY_TOOL.is_file():
        raise SystemExit(f"{REPLAY_TOOL} not found: cargo build --release in server/")
    clip.parent.mkdir(parents=True, exist_ok=True)
    command = [str(REPLAY_TOOL), "render", "--track", str(yaml), "--class", "GT3", "--cars", "12",
               "--laps", "2", "--weather", "sunny", "--time", "13:00", "--seed", str(SEED),
               "--cars-dir", str(REPO / "content" / "cars" / "default"), "--out", str(clip)]
    print("rendering the clip:", " ".join(command[1:3]), track, flush=True)
    subprocess.run(command, check=True, cwd=REPO)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Time the game on a fixed seeded race.")
    parser.add_argument("--track", default="Spa")
    parser.add_argument("--res", default="1920x1080", help="window size, WxH")
    parser.add_argument("--start", type=float, default=20.0, help="seconds into the clip the timing starts")
    parser.add_argument("--seconds", type=float, default=60.0, help="how long it times")
    parser.add_argument("--rerender", action="store_true", help="render the clip again")
    parser.add_argument("--game-arg", action="append", default=[], help="a switch added to the game's command line")
    args = parser.parse_args(argv)

    clip = OUT / f"{args.track}.apxs"
    if args.rerender or not clip.is_file():
        render_clip(args.track, clip)
    editor = engine_editor()
    if not editor:
        raise SystemExit("no Unreal Engine found: set UE to the folder that holds Engine/")
    width, height = (int(v) for v in args.res.lower().split("x"))
    result = RESULTS / f"{args.track}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    command = [str(editor), str(UPROJECT), "-game", "-windowed", f"-ResX={width}", f"-ResY={height}",
               "-unattended", "-nosplash", "-ApexNoSplashHold", f"-ApexReplay={clip}", "-ApexReplayCam=tv",
               f"-ApexReplaySeed={SEED}", f"-ApexReplayStart={args.start:g}", f"-ApexReplayDuration={args.seconds:g}",
               f"-ApexReplayBench={result}", *args.game_arg, "-log=Benchmark.log"]
    print(f"timing {args.seconds:g} s of {clip.name} at {width}x{height}...", flush=True)
    subprocess.run(command, check=False)
    if not result.is_file():
        print(f"no result: see game-unreal/Saved/Logs/Benchmark.log", file=sys.stderr)
        return 1
    data = json.loads(result.read_text(encoding="utf-8"))
    print(f"{data['gpu']} ({data['rhi']}) at {data['width']}x{data['height']}, preset {data['scalability']}: "
          f"{data['mean_fps']:.1f} fps mean, {data['low_1pct_fps']:.1f} fps 1% low; frame time p50 "
          f"{data['p50_ms']:.2f} ms, p95 {data['p95_ms']:.2f}, p99 {data['p99_ms']:.2f}, worst {data['max_ms']:.1f}")
    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
