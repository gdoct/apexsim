#!/usr/bin/env python3
"""Take the marketing page's in-engine pictures from site/shots.yml.

    python scripts/site/make_shots.py                 # every shot
    python scripts/site/make_shots.py eau-rouge garage
    python scripts/site/make_shots.py --list
    python scripts/site/make_shots.py --pick          # re-apply each `pick` to the candidates on disk
    python scripts/site/make_shots.py --dry-run       # print the game's command lines
    python scripts/site/build_site.py                 # then put them on the page

Two kinds of shot, because they want opposite things:

  action  A race nobody drove, filmed by the replay cameras: no HUD, no
          menu, no server. The promo video's pipeline (scripts/promo/
          make_clips.py, docs/PROMO_VIDEO.md) simulates a seeded AI race,
          finds the moment the field runs through a corner together, cuts
          it as a clip and plays it in the game with `-ApexReplay` under a
          broadcast or tripod camera, recording every frame. Here a few
          seconds are filmed, `frames` of them kept as candidates
          (out/site/candidates/<id>/, with a contact sheet), and the one
          the shot's `pick` names becomes the master. A seeded race replays
          bit for bit, so after a change to how the game looks the same
          `pick` is the same moment in the new look.

  ui      The game as a player sees it: HUD, garage, menus. One unattended
          run with the shot's switches, `-ApexScreenshotAfter` for when to
          grab and `r.SetRes` for the size (settings.yml's window mode
          would otherwise decide it). A list of times grabs at each; `pick`
          chooses. Needs a server: one is started from
          server/target/release when nothing is listening where
          game-unreal/settings.yml points, and stopped afterwards.

The masters are site/shots/<id>.webp, checked in so that CI can verify the
page without an engine; site/media.yml cuts the page's pictures from them.

Needs the editor closed, the ApexSimEditor target built
(`./scripts/play_editor.ps1 -Build`), `cargo build --release` in server/
and the content initialised (`./scripts/initialize_content.ps1`).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "promo"))
import make_clips  # noqa: E402  (the replay planner and renderer)

SHOTS_YML = REPO / "site" / "shots.yml"
MASTERS = REPO / "site" / "shots"
WORK = REPO / "out" / "site"
CANDIDATES = WORK / "candidates"
UPROJECT = REPO / "game-unreal" / "ApexSim.uproject"
SETTINGS = REPO / "game-unreal" / "settings.yml"
GRABS = REPO / "game-unreal" / "Saved" / "Screenshots" / "WindowsEditor"
SERVER_DIR = REPO / "server"
SERVER_EXE = SERVER_DIR / "target" / "release" / "apexsim-server.exe"
REPLAY_EXE = SERVER_DIR / "target" / "release" / "apexsim-replay.exe"

# How long past its last grab time a ui run may take before it is given up
# on: engine start, shader warm-up and the track build come before the
# clock the grab times are counted on.
STARTUP_ALLOWANCE_S = 150.0
MASTER_QUALITY = 92
# Frames of an action shot left out at either end when the candidates are
# spread over it: the first ones still carry the cut onto the camera.
EDGE_FRAMES = 6


def log(message: str) -> None:
    print(message, flush=True)


def engine_editor() -> Path | None:
    """UnrealEditor.exe of the .uproject's engine, found the way
    scripts/lib/ApexEngine.ps1 finds it: $UE / $UE_ROOT / $UE5_ROOT, the
    registry entry for the EngineAssociation, the launcher's default folders."""
    candidates = [os.environ[name] for name in ("UE", "UE_ROOT", "UE5_ROOT") if os.environ.get(name)]
    try:
        association = json.loads(UPROJECT.read_text(encoding="utf-8-sig"))["EngineAssociation"]
    except (OSError, KeyError, ValueError):
        association = None
    if association:
        try:
            import winreg
            for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
                try:
                    with winreg.OpenKey(hive, "SOFTWARE\\EpicGames\\Unreal Engine\\" + association) as key:
                        candidates.append(winreg.QueryValueEx(key, "InstalledDirectory")[0])
                except OSError:
                    pass
        except ImportError:
            pass
        candidates += ["C:/Program Files/Epic Games/UE_" + association, "C:/Epic Games/UE_" + association]
    for root in candidates:
        exe = Path(root) / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
        if exe.is_file():
            return exe
    return None


def parse_resolution(text) -> tuple[int, int]:
    width, height = (int(v) for v in str(text).split("x"))
    return width, height


def save_master(source: Path, target: Path) -> tuple[int, int]:
    from PIL import Image

    with Image.open(source) as opened:
        image = opened.convert("RGB")
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target, "WEBP", quality=MASTER_QUALITY, method=6)
    return image.size


def contact_sheet(files: list[Path], target: Path) -> None:
    """The candidates side by side, numbered as `pick` counts them."""
    from PIL import Image, ImageDraw

    cols = min(3, len(files))
    rows = (len(files) + cols - 1) // cols
    w, h = 640, 360
    sheet = Image.new("RGB", (cols * w, rows * h))
    for i, file in enumerate(files):
        with Image.open(file) as opened:
            tile = opened.convert("RGB").resize((w, h))
        draw = ImageDraw.Draw(tile)
        draw.rectangle((0, 0, 34, 22), fill=(0, 0, 0))
        draw.text((8, 5), str(i + 1), fill=(255, 220, 0))
        sheet.paste(tile, ((i % cols) * w, (i // cols) * h))
    sheet.save(target, quality=88)


def candidates_of(shot_id: str) -> list[Path]:
    return sorted((CANDIDATES / shot_id).glob("[0-9][0-9].webp"))


def apply_pick(shot_id: str, shot: dict) -> bool:
    """Copy the candidate the shot's `pick` names to site/shots/<id>.webp."""
    files = candidates_of(shot_id)
    if not files:
        log(f"  {shot_id}: no candidates under out/site/candidates: take the shot first")
        return False
    pick = shot.get("pick")
    if pick is None:
        log(f"  {shot_id}: {len(files)} candidates in out/site/candidates/{shot_id}/ (sheet.jpg); "
            f"set `pick` in site/shots.yml to the one to use")
        return True
    if not 1 <= int(pick) <= len(files):
        log(f"  {shot_id}: pick {pick} but there are {len(files)} candidates")
        return False
    MASTERS.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(files[int(pick) - 1], MASTERS / f"{shot_id}.webp")
    log(f"  site/shots/{shot_id}.webp  <-  candidate {pick} of {len(files)}")
    return True


def store_candidates(shot_id: str, sources: list[Path]) -> None:
    folder = CANDIDATES / shot_id
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for i, source in enumerate(sources):
        save_master(source, folder / f"{i + 1:02d}.webp")
    contact_sheet(candidates_of(shot_id), folder / "sheet.jpg")


# --- action: a replay under the cameras ---------------------------------

def take_action(shot: dict, planner, game: list[str], defaults: dict, dry_run: bool) -> bool:
    shot_id = shot["id"]
    log(f"{shot_id}: {shot.get('about', '')}".rstrip(": "))
    plan = planner.plan(shot)
    resolution = parse_resolution(shot.get("resolution", defaults.get("resolution", "1920x1080")))
    if not make_clips.render(plan, game, WORK, resolution, 900.0, [], dry_run):
        return False
    if dry_run:
        return True
    frames_dir = WORK / "frames" / shot_id
    frames = sorted(frames_dir.glob("frame_*.png"))
    usable = frames[EDGE_FRAMES:-EDGE_FRAMES] if len(frames) > 4 * EDGE_FRAMES else frames
    count = max(1, min(int(shot.get("frames", defaults.get("frames", 6))), len(usable)))
    step = (len(usable) - 1) / max(count - 1, 1)
    store_candidates(shot_id, [usable[round(i * step)] for i in range(count)])
    shutil.rmtree(frames_dir, ignore_errors=True)
    return apply_pick(shot_id, shot)


# --- ui: the live game ---------------------------------------------------

def server_address() -> tuple[str, int]:
    """Where the game will connect: the `server:` block of settings.yml."""
    host, port = "127.0.0.1", 9000
    if SETTINGS.is_file():
        in_server = False
        for line in SETTINGS.read_text(encoding="utf-8").splitlines():
            if re.match(r"^\S", line):
                in_server = line.startswith("server:")
            elif in_server:
                found = re.match(r"\s+(host|port):\s*(\S+)", line)
                if found and found.group(1) == "host":
                    host = found.group(2)
                elif found:
                    port = int(found.group(2))
    return host, port


def listening(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


class Server:
    """A local server for the ui shots, unless one is already up."""

    def __init__(self):
        self.process = None

    def __enter__(self):
        host, port = server_address()
        if listening(host, port):
            log(f"using the server already listening on {host}:{port}: "
                f"its AI field may include the player's own cars")
            return self
        if host not in ("127.0.0.1", "localhost"):
            raise SystemExit(f"settings.yml points at {host}:{port} and nothing answers there")
        if not SERVER_EXE.is_file():
            raise SystemExit(f"{SERVER_EXE} not found: cargo build --release in server/")
        log(f"starting {SERVER_EXE.name} for the ui shots")
        # The shipped cars only: an AI field must not bring a player's own
        # imports (real teams' colours) onto the page.
        env = {**os.environ, "APEXSIM_CONTENT_CARS_DIR": str(REPO / "content" / "cars" / "default")}
        self.process = subprocess.Popen([str(SERVER_EXE)], cwd=SERVER_DIR, env=env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 60.0
        while not listening(host, port):
            if self.process.poll() is not None:
                raise SystemExit("the server exited at once: run it by hand in server/ to see why")
            if time.monotonic() > deadline:
                self.process.kill()
                raise SystemExit(f"the server did not open {host}:{port} within a minute")
            time.sleep(0.5)
        return self

    def __exit__(self, *exc):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()


def grab_times(shot: dict) -> list[float]:
    at = shot["at"]
    return [float(t) for t in at] if isinstance(at, list) else [float(at)]


def ui_command(shot: dict, defaults: dict, game: list[str]) -> list[str]:
    width, height = parse_resolution(shot.get("resolution", defaults.get("resolution", "1920x1080")))
    times = ",".join(f"{t:g}" for t in grab_times(shot))
    return [
        *game,
        "-windowed", f"-ResX={width}", f"-ResY={height}",
        # the size, and none of the engine's own on-screen warnings in the grab
        f"-ExecCmds=r.SetRes {width}x{height}w, DisableAllScreenMessages",
        "-ApexNoSplashHold", "-unattended", "-nosplash",
        *[str(a) for a in shot.get("args", [])],
        f"-ApexScreenshotAfter={times}",
        f"-log=SiteShot_{shot['id']}.log",
    ]


def settled(path: Path) -> bool:
    """A grab is written in one go, but not in one instant."""
    first = path.stat().st_size
    time.sleep(0.6)
    return first > 0 and path.stat().st_size == first


def take_ui(shot: dict, game: list[str], defaults: dict, dry_run: bool) -> bool:
    shot_id = shot["id"]
    times = grab_times(shot)
    command = ui_command(shot, defaults, game)
    log(f"{shot_id}: {shot.get('about', '')}".rstrip(": "))
    if dry_run:
        log(subprocess.list2cmdline(command))
        return True
    before = set(GRABS.glob("*.png")) if GRABS.is_dir() else set()
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    grabs: list[Path] = []
    try:
        deadline = started + max(times) + STARTUP_ALLOWANCE_S
        while len(grabs) < len(times):
            if process.poll() is not None:
                log(f"  the game exited early (code {process.returncode}): "
                    f"see game-unreal/Saved/Logs/SiteShot_{shot_id}.log")
                return False
            if time.monotonic() > deadline:
                log(f"  no picture after {deadline - started:.0f} s: "
                    f"see game-unreal/Saved/Logs/SiteShot_{shot_id}.log")
                return False
            time.sleep(1.0)
            fresh = sorted(set(GRABS.glob("*.png")) - before - set(grabs), key=lambda p: p.stat().st_mtime) \
                if GRABS.is_dir() else []
            grabs += [p for p in fresh if settled(p)]
    finally:
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
    store_candidates(shot_id, grabs)
    for grab in grabs:
        grab.unlink()
    log(f"  {len(grabs)} grab(s) in {time.monotonic() - started:.0f} s")
    if len(grabs) == 1 and shot.get("pick") is None:
        shot = {**shot, "pick": 1}
    return apply_pick(shot_id, shot)


# -------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Take the marketing page's in-engine pictures.")
    parser.add_argument("shots", nargs="*", help="shot ids from site/shots.yml (default: all)")
    parser.add_argument("--list", action="store_true", help="list the shots and stop")
    parser.add_argument("--pick", action="store_true", help="only copy each shot's `pick` from the candidates on disk")
    parser.add_argument("--resimulate", action="store_true", help="run the races again even when a replay is cached")
    parser.add_argument("--dry-run", action="store_true", help="plan, and print the game's command lines")
    args = parser.parse_args(argv)

    with open(SHOTS_YML, encoding="utf-8") as f:
        config = yaml.safe_load(f)
    defaults = config.get("defaults") or {}
    action = [dict(s, kind="action") for s in config.get("action") or []]
    ui = [dict(s, kind="ui") for s in config.get("ui") or []]
    every = action + ui
    ids = [s["id"] for s in every]
    if len(set(ids)) != len(ids):
        raise SystemExit("site/shots.yml: two shots share an id")
    unknown = [s for s in args.shots if s not in ids]
    if unknown:
        raise SystemExit(f"no such shot in site/shots.yml: {', '.join(unknown)}")
    chosen = [s for s in every if not args.shots or s["id"] in args.shots]

    if args.list:
        for shot in chosen:
            pick = shot.get("pick")
            log(f"{shot['id']:20} {shot['kind']:7} pick {pick if pick is not None else '-':<3} {shot.get('about', '')}")
        return 0
    if args.pick:
        return 0 if all([apply_pick(s["id"], s) for s in chosen]) else 1

    editor = engine_editor()
    if not editor:
        raise SystemExit("no Unreal Engine found: set UE to the folder that holds Engine/")
    game = [str(editor), str(UPROJECT), "-game"]

    failed = []
    chosen_action = [s for s in chosen if s["kind"] == "action"]
    if chosen_action:
        if not REPLAY_EXE.is_file():
            raise SystemExit(f"{REPLAY_EXE} not found: cargo build --release in server/")
        tool = make_clips.ReplayTool(str(REPLAY_EXE), False, args.dry_run)
        planner = make_clips.Planner({"defaults": defaults, "races": config.get("races") or {}},
                                     tool, WORK, args.resimulate)
        for shot in chosen_action:
            if not take_action(shot, planner, game, defaults, args.dry_run):
                failed.append(shot["id"])

    chosen_ui = [s for s in chosen if s["kind"] == "ui"]
    if chosen_ui and args.dry_run:
        for shot in chosen_ui:
            take_ui(shot, game, defaults, True)
    elif chosen_ui:
        with Server():
            for shot in chosen_ui:
                if not take_ui(shot, game, defaults, False):
                    failed.append(shot["id"])

    newest = max(p.stat().st_mtime for p in (SERVER_DIR / "src").rglob("*.rs"))
    if not args.dry_run and REPLAY_EXE.is_file() and newest > REPLAY_EXE.stat().st_mtime:
        log("note: server/target/release is older than server/src: the races were run by the older simulation")
    if failed:
        log(f"failed: {', '.join(failed)}")
        return 1
    if not args.dry_run:
        log("done: python scripts/site/build_site.py puts the picked shots on the page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
