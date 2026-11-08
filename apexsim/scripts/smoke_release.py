#!/usr/bin/env python3
"""Smoke-test a built release package before it is published.

    python scripts/smoke_release.py 0.4.5            # artifacts/release/ApexSim-0.4.5-Win64
    python scripts/smoke_release.py 0.4.5 --no-game  # server and pack only

Run from the code root (apexsim/). It touches nothing you run yourself: the
package's own server starts on ports 9100-9104 (APEXSIM_*_PORT), and the
package's game reads a scratch settings.yml (-ApexSettingsDir) pointing at
it. Checks, in order:

1. release.json names this version, and the package has its executables.
2. `apexsim-server content install <pack> --dry-run` lists the content
   pack's items and refuses none of them.
3. The packaged server starts and answers /ready.
4. The packaged game logs in, races a short auto race on a shipped circuit
   and grabs the screen twice (-ApexScreenshotAfter); the grabs are copied
   to out/release-smoke/<version>/ (from %LOCALAPPDATA%\ApexSim\Saved, where
   a package saves) for a look: they show whether it raced.

Exit code 0 when every check passes. The grabs still want a human (or
agent) look: a black or torn frame passes the script.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

CODE = Path(__file__).resolve().parent.parent
ARTIFACTS = CODE / "artifacts" / "release"
OUT = CODE / "out" / "release-smoke"
PORTS = {
    "APEXSIM_NETWORK_TCP_PORT": 9100,
    "APEXSIM_NETWORK_UDP_PORT": 9101,
    "APEXSIM_NETWORK_HEALTH_PORT": 9102,
    "APEXSIM_ADMIN_HTTP_PORT": 9103,
    "APEXSIM_ADMIN_HTTPS_PORT": 9104,
}


def fail(msg: str) -> None:
    print(f"FAIL  {msg}", flush=True)
    raise SystemExit(1)


def ok(msg: str) -> None:
    print(f"ok    {msg}", flush=True)


def check_layout(folder: Path, version: str) -> dict:
    meta = folder / "release.json"
    if not meta.is_file():
        fail(f"no {meta}")
    info = json.loads(meta.read_text(encoding="utf-8-sig"))
    if info.get("version") != version:
        fail(f"release.json is version {info.get('version')}, not {version}")
    for rel in ("launcher.exe", "Game/ApexSim.exe", "Server/apexsim-server.exe",
                "Server/apexsim-replay.exe", "Server/server.toml", "README.txt", "unblock.txt"):
        if not (folder / rel).is_file():
            fail(f"the package has no {rel}")
    readme = (folder / "README.txt").read_text(encoding="utf-8", errors="replace")
    if version not in readme:
        fail(f"README.txt does not name {version}")
    ok(f"layout and release.json {version} (built at {info.get('commit')}, {info.get('configuration')})")
    return info


def check_pack(folder: Path, version: str, info: dict) -> None:
    pack = ARTIFACTS / f"apexsim-content-{version}.acp"
    if not (info.get("contents") or {}).get("pack"):
        ok("no content pack in this release")
        return
    if not pack.is_file():
        fail(f"release.json names a pack, but {pack.name} is missing")
    out = subprocess.run(
        [str(folder / "Server" / "apexsim-server.exe"), "content", "install", str(pack),
         "--root", str(folder), "--dry-run"],
        cwd=folder / "Server", capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        fail(f"content install --dry-run: {out.stderr.strip()}")
    report = json.loads(out.stdout)
    items = report.get("items", [])
    refused = [i["id"] for i in items if i.get("refused")]
    if refused:
        fail(f"the pack's items refused: {refused}")
    cars = sum(1 for i in items if i["kind"] == "car")
    tracks = sum(1 for i in items if i["kind"] == "track")
    ok(f"{pack.name}: {cars} cars, {tracks} tracks, none refused ({len(report.get('ignored', []))} files ignored)")


def start_server(folder: Path, log_path: Path) -> subprocess.Popen:
    env = dict(os.environ, **{k: str(v) for k, v in PORTS.items()})
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen([str(folder / "Server" / "apexsim-server.exe")], cwd=folder / "Server",
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    url = f"http://127.0.0.1:{PORTS['APEXSIM_NETWORK_HEALTH_PORT']}/ready"
    for _ in range(120):
        if proc.poll() is not None:
            fail(f"the server exited with {proc.returncode} (log: {log_path})")
        try:
            with urllib.request.urlopen(url, timeout=1) as r:
                if r.status == 200:
                    ok(f"the server is ready on port {PORTS['APEXSIM_NETWORK_TCP_PORT']}")
                    return proc
        except Exception:
            pass
        time.sleep(1)
    proc.kill()
    fail(f"the server never answered {url} (log: {log_path})")
    raise AssertionError


def shipped_track(folder: Path) -> str:
    tracks = sorted(p.name for p in (folder / "Server" / "content" / "tracks" / "default").iterdir() if p.is_dir())
    if not tracks:
        fail("no shipped track in Server/content/tracks/default")
    return tracks[0]


def race(folder: Path, work: Path, track: str, seconds: int) -> list[Path]:
    settings = work / "settings"
    settings.mkdir(parents=True, exist_ok=True)
    (settings / "settings.yml").write_text(
        "# Written by scripts/smoke_release.py.\n"
        "display:\n  resolution: 1280x720\n  window_mode: windowed\n  vsync: false\n"
        "  frame_limit: 60\n  screens: 1\n"
        f"server:\n  host: 127.0.0.1\n  port: {PORTS['APEXSIM_NETWORK_TCP_PORT']}\n",
        encoding="utf-8")
    game_dir = folder / "Game"
    started = time.time()
    first, second = max(20, seconds // 2), seconds
    args = [str(game_dir / "ApexSim.exe"), f"-ApexSettingsDir={settings}", "-ApexAutoRace",
            f"-ApexTrack={track}", "-ApexAiCount=3", "-ApexLaps=2", "-ApexCountdown=5",
            f"-ApexScreenshotAfter={first},{second}", f"-ApexExecAfter={second + 10}=quit",
            "-windowed", "-ResX=1280", "-ResY=720", "-nosplash"]
    proc = subprocess.Popen(args, cwd=game_dir)
    try:
        proc.wait(timeout=seconds + 240)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("note  the game did not quit on its own; stopped", flush=True)
    # A packaged game saves under %LOCALAPPDATA%\ApexSim\Saved, not beside the exe.
    roots = [game_dir]
    if os.environ.get("LOCALAPPDATA"):
        roots.append(Path(os.environ["LOCALAPPDATA"]) / "ApexSim" / "Saved" / "Screenshots")
    grabs = [p for root in roots if root.is_dir() for p in root.rglob("*.png") if p.stat().st_mtime >= started]
    return sorted(grabs, key=lambda p: p.stat().st_mtime)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version")
    ap.add_argument("--no-game", action="store_true", help="check the server and the pack only")
    ap.add_argument("--track", help="the circuit stem to race (default: the first shipped one)")
    ap.add_argument("--seconds", type=int, default=60, help="race view seconds to the last grab")
    args = ap.parse_args()

    folder = ARTIFACTS / f"ApexSim-{args.version}-Win64"
    if not folder.is_dir():
        fail(f"no {folder}: build it with scripts/build_release.ps1")
    out = OUT / args.version
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    info = check_layout(folder, args.version)
    check_pack(folder, args.version, info)
    server_log = out / "server.log"
    server = start_server(folder, server_log)
    try:
        if not args.no_game:
            track = args.track or shipped_track(folder)
            with tempfile.TemporaryDirectory(prefix="apexsim-smoke-") as tmp:
                grabs = race(folder, Path(tmp), track, args.seconds)
            for i, g in enumerate(grabs, 1):
                shutil.copy2(g, out / f"grab_{i}.png")
                g.unlink()
            if len(grabs) < 2:
                fail(f"{len(grabs)} grab(s) of 2 at {track} (server log: {server_log})")
            ok(f"raced at {track}: {len(grabs)} grabs in {out}")
    finally:
        server.terminate()
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()
    print(f"smoke test of {args.version} passed; look at {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
