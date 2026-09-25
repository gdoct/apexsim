#!/usr/bin/env python3
"""Make the promo video's clips: simulate, find, cut, render, encode.

Every shot in `scripts/promo/shots.yml` goes through four stages:

  plan    simulate its race headless (`apexsim-replay simulate`, cached per
          race), find the moment (`find` at a corner, or the start/finish),
          work out the camera (`pose`) and cut a short clip file
          (`cut ... --out <id>.clip.json`)                    -> out/promo/cuts/
  render  launch the game on the clip (`-ApexReplay=`), which plays it at a
          fixed timestep and writes every frame as a PNG     -> out/promo/frames/<id>/
  encode  ffmpeg the frames into a clip                       -> out/promo/clips/<id>.mp4
  (stitch_video.py then cuts the clips into the video.)

Planning runs anywhere the Rust toolchain does. Rendering needs the game
built with the replay support (the editor build or a packaged one) and the
tracks' levels imported; encoding needs ffmpeg on PATH (or --ffmpeg).

    python scripts/promo/make_clips.py                         # everything
    python scripts/promo/make_clips.py --only 05_lemans_wheel  # one shot
    python scripts/promo/make_clips.py --stage plan            # no game needed
    python scripts/promo/make_clips.py --preview 05_lemans_wheel
                                       # play a shot live, looping, to tune its camera
    python scripts/promo/make_clips.py --dry-run               # print the game command lines

See docs/PROMO_VIDEO.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - a helpful message beats a traceback
    sys.exit("make_clips.py needs PyYAML: pip install pyyaml")

REPO = Path(__file__).resolve().parents[2]
TRACKS = REPO / "content" / "tracks" / "real"
UPROJECT = REPO / "game-unreal" / "ApexSim.uproject"
EXE = ".exe" if platform.system() == "Windows" else ""


# --- small helpers -------------------------------------------------------------------


def log(message: str) -> None:
    print(message, flush=True)


def fail(message: str) -> None:
    sys.exit(f"make_clips: {message}")


def load_config(path: Path) -> dict:
    with open(path, encoding="utf-8") as handle:
        config = yaml.safe_load(handle)
    for key in ("races", "shots"):
        if key not in config:
            fail(f"{path} has no '{key}' section")
    return config


def parse_resolution(text: str) -> tuple[int, int]:
    width, _, height = str(text).lower().partition("x")
    return int(width), int(height)


def spec_hash(spec: dict) -> str:
    return hashlib.sha1(json.dumps(spec, sort_keys=True).encode()).hexdigest()[:12]


# --- the replay tool -----------------------------------------------------------------


class ReplayTool:
    """`apexsim-replay`, built from the server crate when it is missing or stale."""

    def __init__(self, explicit: str | None, build: bool, dry_run: bool):
        self.dry_run = dry_run
        if explicit:
            self.path = Path(explicit)
        else:
            self.path = REPO / "server" / "target" / "release" / f"apexsim-replay{EXE}"
            # Incremental, so a second or two when nothing changed; a stale
            # binary would plan with yesterday's physics.
            if build or not self.path.exists():
                log("building apexsim-replay (cargo build --release) ...")
                subprocess.run(
                    ["cargo", "build", "--release", "--bin", "apexsim-replay"],
                    cwd=REPO / "server",
                    check=True,
                )
        if not self.path.exists():
            fail(f"{self.path} not found; build it with `cargo build --release --bin apexsim-replay` in server/")

    def run(self, *argv: str) -> dict:
        command = [str(self.path), *argv]
        result = subprocess.run(command, cwd=REPO, capture_output=True, text=True, encoding="utf-8")
        if result.returncode != 0:
            fail(f"{' '.join(command)}\n{result.stderr.strip()}")
        return json.loads(result.stdout)


# --- planning ------------------------------------------------------------------------


class Planner:
    def __init__(self, config: dict, tool: ReplayTool, out: Path, resimulate: bool):
        self.config = config
        self.defaults = config.get("defaults", {})
        self.tool = tool
        self.out = out
        self.resimulate = resimulate
        self.race_info: dict[str, dict] = {}

    def track_yaml(self, race: dict) -> Path:
        path = TRACKS / f"{race['track']}.yaml"
        if not path.exists():
            fail(f"no track {path}")
        return path

    def race_replay(self, name: str) -> tuple[Path, dict]:
        """The race's replay, simulated unless a replay of the same spec exists."""
        if name in self.race_info:
            return self.out / "races" / f"{name}.bin", self.race_info[name]
        race = self.config["races"].get(name)
        if race is None:
            fail(f"no race '{name}' in the shot list")
        spec = {
            "track": race["track"],
            "car": race.get("car", "yotota-lmp2"),
            "same_car": bool(race.get("same_car", False)),
            "ai": int(race.get("ai", self.defaults.get("ai", 12))),
            "laps": int(race.get("laps", self.defaults.get("laps", 3))),
            "max_seconds": float(race.get("max_seconds", 900)),
            "weather": str(race.get("weather", "sunny")),
            "time": str(race.get("time", "13:00")),
            "record_hz": int(race.get("record_hz", 60)),
            # Long enough for all five start lights and a crane down the grid.
            "countdown": int(race.get("countdown", self.defaults.get("countdown_s", 7))),
            # The same seed deals the same grid: the race, and so every
            # planned moment in it, repeats exactly.
            "seed": int(race.get("seed", self.defaults.get("seed", 1))),
        }
        replay = self.out / "races" / f"{name}.bin"
        meta = self.out / "races" / f"{name}.json"
        wanted = spec_hash(spec)
        if not self.resimulate and replay.exists() and meta.exists():
            cached = json.loads(meta.read_text(encoding="utf-8"))
            if cached.get("spec_hash") == wanted:
                self.race_info[name] = cached["info"]
                return replay, cached["info"]
        replay.parent.mkdir(parents=True, exist_ok=True)
        log(f"simulating race '{name}': {spec['ai']} x {spec['car']} at {spec['track']}, "
            f"{spec['laps']} laps, {spec['weather']} {spec['time']}")
        argv = [
            "simulate",
            "--track", str(self.track_yaml(race)),
            "--car", spec["car"],
            "--cars-dir", str(REPO / "content" / "cars"),
            "--ai", str(spec["ai"]),
            "--laps", str(spec["laps"]),
            "--max-seconds", str(spec["max_seconds"]),
            "--weather", spec["weather"],
            "--time", spec["time"],
            "--record-hz", str(spec["record_hz"]),
            "--countdown", str(spec["countdown"]),
            "--seed", str(spec["seed"]),
            "--out", str(replay),
        ]
        if spec["same_car"]:
            argv.append("--same-car")
        info = self.tool.run(*argv)
        meta.write_text(json.dumps({"spec": spec, "spec_hash": wanted, "info": info}, indent=2), encoding="utf-8")
        self.race_info[name] = info
        return replay, info

    # --- the window ---

    def shot_window(self, shot: dict, replay: Path, info: dict) -> tuple[float, int | None]:
        """(seconds into the recording the shot starts, the car it is about)."""
        rate = info["tick_rate"]
        length = float(shot["length"])
        window = shot.get("window", "start")
        offset = float(shot.get("start_offset_s", 0.0))

        if window == "start":
            if info.get("race_start_tick") is None:
                fail(f"{shot['id']}: the race has no start in its recording")
            leader = next((p["index"] for p in info["participants"] if p.get("finish_position") == 1), None)
            return info["race_start_tick"] / rate + offset, leader
        if window == "finish":
            winner = next((p for p in info["participants"] if p.get("finish_position") == 1), None)
            if not winner or not winner["lap_ticks"]:
                fail(f"{shot['id']}: nobody finished the race (raise max_seconds or lower laps)")
            return winner["lap_ticks"][-1] / rate + offset, winner["index"]
        if isinstance(window, dict) and "at_s" in window:
            return float(window["at_s"]) + offset, None

        if not isinstance(window, dict):
            fail(f"{shot['id']}: window must be start, finish, {{at_s}} or {{corner|station, ...}}")
        argv = ["find", str(replay), "--track", str(self.track_yaml(self.config["races"][shot["race"]]))]
        if "corner" in window:
            argv += ["--corner", str(window["corner"])]
        elif "station" in window:
            argv += ["--station", str(window["station"])]
        else:
            fail(f"{shot['id']}: a window needs a corner or a station")
        argv += [
            "--before", str(window.get("before", 150)),
            "--after", str(window.get("after", 100)),
            "--min-cars", str(window.get("min_cars", 2)),
            "--pad", "0",
            "--top", "20",
        ]
        if window.get("last_laps"):
            argv += ["--last-laps", str(window["last_laps"])]
        if window.get("include_lap_1"):
            argv.append("--include-lap-1")
        found = self.tool.run(*argv)["windows"]
        if not found:
            fail(f"{shot['id']}: no moment with {window.get('min_cars', 2)}+ cars there; "
                 "lower min_cars or widen before/after")
        pick = min(int(window.get("pick", 0)), len(found) - 1)
        best = found[pick]
        # The busiest frame sits `anchor` of the way into the shot.
        anchor = float(window.get("anchor", 0.5))
        peak = best["peak_tick"] / rate
        start = peak - anchor * length
        # Keep the shot on the action when the window is long enough.
        start = min(max(start, best["from_s"] - 0.5), max(best["to_s"] - length + 0.5, best["from_s"] - 0.5))
        return start + offset, best["lead_car"]

    # --- the camera ---

    def point(self, race: dict, spec: dict, what: str) -> list[float]:
        """A camera point from {corner|station, offset, lateral, side, height} or {landmark, height}."""
        track = str(self.track_yaml(race))
        if "landmark" in spec:
            reply = self.tool.run(
                "pose", "--track", track, "--station", "0",
                "--look-landmark", str(spec["landmark"]),
                "--look-height", str(spec.get("height", 5)),
            )
            target = reply["target"]
            return [target["x"], target["y"], target["z"]]
        argv = ["pose", "--track", track]
        if "corner" in spec:
            argv += ["--corner", str(spec["corner"])]
        elif "station" in spec:
            argv += ["--station", str(spec["station"])]
        else:
            fail(f"a camera {what} needs a corner, a station or a landmark")
        argv += [
            "--offset", str(spec.get("offset", 0)),
            "--lateral", str(spec.get("lateral", 0)),
            "--height", str(spec.get("height", 2)),
        ]
        if spec.get("side"):
            argv += ["--side", str(spec["side"])]
        eye = self.tool.run(*argv)["eye"]
        return [eye["x"], eye["y"], eye["z"]]

    def camera_args(self, shot: dict, lead: int | None, info: dict) -> list[str]:
        camera = shot.get("camera", {"mode": "tv"})
        race = self.config["races"][shot["race"]]
        mode = camera.get("mode", "tv")
        args = [f"-ApexReplayCam={mode}"]

        follow = camera.get("follow")
        if follow == "lead":
            follow = lead if lead is not None else "leader"
        elif follow == "winner":
            winner = next((p["index"] for p in info["participants"] if p.get("finish_position") == 1), None)
            follow = winner if winner is not None else "leader"
        if follow is not None:
            args.append(f"-ApexReplayFollow={follow}")
        if camera.get("shot"):
            args.append(f"-ApexReplayShot={camera['shot']}")
        if camera.get("chase"):
            args.append(f"-ApexReplayChase={camera['chase']}")

        if mode in ("fixed", "pan"):
            if "eye" not in camera:
                fail(f"{shot['id']}: a {mode} camera needs an eye")
            eye = self.point(race, camera["eye"], "eye")
            if "look" in camera:
                look = self.point(race, camera["look"], "look")
            else:
                # Straight across the road from the eye, a little down the lap.
                spec = dict(camera["eye"])
                spec.update({"lateral": 0, "height": 1, "side": None})
                look = self.point(race, spec, "look")
            numbers = ",".join(f"{v:.2f}" for v in eye + look)
            args.append(f"-ApexCameraLookAt={numbers}")
        for key, flag in (
            ("fov", "ApexCameraFov"),
            ("look_bias", "ApexReplayLookBias"),
            ("pan_speed", "ApexReplayPanSpeed"),
            ("frame_width", "ApexReplayFrameWidth"),
            ("min_fov", "ApexReplayMinFov"),
            ("lead_s", "ApexReplayLead"),
            ("ground_clearance", "ApexReplayGroundClearance"),
            ("seed", "ApexReplaySeed"),
        ):
            if camera.get(key) is not None:
                args.append(f"-{flag}={camera[key]}")
        return args

    # --- the plan ---

    def plan(self, shot: dict) -> dict:
        shot_id = shot["id"]
        replay, info = self.race_replay(shot["race"])
        length = float(shot["length"])
        start, lead = self.shot_window(shot, replay, info)
        rate = info["tick_rate"]
        first = info["first_tick"] / rate
        last = info["last_tick"] / rate
        pad = float(shot.get("pad_s", self.defaults.get("pad_s", 1.5)))
        preroll = float(shot.get("preroll_s", self.defaults.get("preroll_s", 1.0)))
        start = min(max(start, first + preroll), last - length)
        cut_from = max(first, start - preroll - pad)
        cut_to = min(last, start + length + pad)

        cuts = self.out / "cuts"
        cuts.mkdir(parents=True, exist_ok=True)
        clip = cuts / f"{shot_id}.clip.json"
        self.tool.run(
            "cut", str(replay),
            "--from-s", f"{cut_from:.4f}", "--to-s", f"{cut_to:.4f}",
            "--track", str(self.track_yaml(self.config["races"][shot["race"]])),
            "--out", str(clip),
        )
        # The cut starts on the first frame at or after `cut_from`.
        clip_first = json.loads(clip.read_text(encoding="utf-8"))["frames"][0]["tick"] / rate

        fps = int(shot.get("fps", self.defaults.get("fps", 60)))
        slowmo = float(shot.get("slowmo", 1.0))
        if not 0.05 <= slowmo <= 1.0:
            fail(f"{shot_id}: slowmo must be between 0.05 and 1")
        record_fps = round(fps / slowmo)
        args = [
            f"-ApexReplayStart={start - clip_first:.4f}",
            f"-ApexReplayDuration={length:.4f}",
            f"-ApexReplayPreroll={preroll:.3f}",
            f"-ApexReplayWarmup={float(shot.get('warmup_s', self.defaults.get('warmup_s', 3.0))):.2f}",
        ]
        args += self.camera_args(shot, lead, info)
        if shot.get("time"):
            args.append(f"-ApexReplayTimeOfDay={shot['time']}")
        if shot.get("weather"):
            args.append(f"-ApexReplayWeather={shot['weather']}")
        args += [str(a) for a in shot.get("extra_args", [])]

        plan = {
            "id": shot_id,
            "race": shot["race"],
            "track": self.config["races"][shot["race"]]["track"],
            "clip": str(clip),
            "start_s": start,
            "length_s": length,
            "slowmo": slowmo,
            "fps": fps,
            "record_fps": record_fps,
            "screen_seconds": length / slowmo,
            "caption": shot.get("caption"),
            "game_args": args,
        }
        (cuts / f"{shot_id}.plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
        log(f"planned {shot_id}: {plan['track']} at {start:.1f} s for {length:.1f} s"
            f"{f' (slow motion x{1 / slowmo:.1f})' if slowmo < 1 else ''}, lead car {lead}")
        return plan


# --- the game ------------------------------------------------------------------------


def engine_editor() -> Path | None:
    """UnrealEditor for the .uproject's engine: $UE_ROOT, else the launcher's default install."""
    candidates = []
    if os.environ.get("UE_ROOT"):
        candidates.append(Path(os.environ["UE_ROOT"]))
    try:
        association = json.loads(UPROJECT.read_text(encoding="utf-8"))["EngineAssociation"]
        candidates.append(Path(f"C:/Program Files/Epic Games/UE_{association}"))
    except (OSError, KeyError, ValueError):
        pass
    for root in candidates:
        exe = root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
        if exe.exists():
            return exe
    return None


def game_command(explicit: str | None) -> list[str]:
    """How to start the game: --game, $APEXSIM_GAME, a packaged build, or the editor build (-game)."""
    choice = explicit or os.environ.get("APEXSIM_GAME")
    if not choice:
        packaged = sorted((REPO / "artifacts").glob("release/*/Game/ApexSim.exe"), key=os.path.getmtime)
        packaged += list((REPO / "artifacts" / "ApexSim-Win64").glob("**/ApexSim.exe"))
        if packaged:
            choice = str(packaged[-1])
    if not choice:
        editor = engine_editor()
        if editor:
            choice = str(editor)
    if not choice:
        fail("no game to render with: pass --game <ApexSim.exe or UnrealEditor.exe>, or set APEXSIM_GAME / UE_ROOT")
    exe = Path(choice)
    if exe.name.lower().startswith("unrealeditor"):
        return [str(exe), str(UPROJECT), "-game"]
    return [str(exe)]


def render(plan: dict, game: list[str], out: Path, resolution: tuple[int, int], timeout: float,
           extra: list[str], dry_run: bool, preview: bool = False) -> bool:
    frames = out / "frames" / plan["id"]
    width, height = resolution
    args = [
        f"-ApexReplay={plan['clip']}",
        *plan["game_args"],
        f"-ApexReplayRes={width}x{height}",
        "-windowed", f"-ResX={width}", f"-ResY={height}",
        "-ApexNoDemo", "-ApexNoSplashHold",
        *extra,
    ]
    if preview:
        args.append("-ApexReplayLoop")
    else:
        args += [
            f"-ApexReplayRecord={frames}",
            f"-ApexReplayFps={plan['record_fps']}",
            "-nosound", "-unattended",
            f"-log=ApexReplay_{plan['id']}.log",
        ]
    command = [*game, *args]
    if dry_run:
        log(subprocess.list2cmdline(command))
        return True
    if preview:
        log(f"previewing {plan['id']} (close the game window to stop)")
        subprocess.run(command)
        return True

    log(f"rendering {plan['id']}: {round(plan['length_s'] * plan['record_fps'])} frames at "
        f"{plan['record_fps']} fps, {width}x{height}")
    started = time.monotonic()
    try:
        subprocess.run(command, timeout=timeout)
    except subprocess.TimeoutExpired:
        log(f"  {plan['id']}: the game did not finish in {timeout:.0f} s")
        return False
    done = frames / "replay_done.json"
    if not done.exists():
        log(f"  {plan['id']}: no {done} - see the game log (Saved/Logs/ApexReplay_{plan['id']}.log)")
        return False
    result = json.loads(done.read_text(encoding="utf-8"))
    if not result.get("ok"):
        log(f"  {plan['id']}: the game reported: {result.get('error')}")
        return False
    count = len(list(frames.glob("frame_*.png")))
    log(f"  {count} frames in {time.monotonic() - started:.0f} s")
    return count > 0


def encode(plan: dict, out: Path, ffmpeg: str, codec: str, dry_run: bool) -> Path | None:
    frames = out / "frames" / plan["id"]
    clips = out / "clips"
    clips.mkdir(parents=True, exist_ok=True)
    if codec == "prores":
        target = clips / f"{plan['id']}.mov"
        video = ["-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"]
    else:
        target = clips / f"{plan['id']}.mp4"
        video = ["-c:v", "libx264", "-preset", "slow", "-crf", "14", "-pix_fmt", "yuv420p", "-movflags", "+faststart"]
    # Recorded at record_fps, played at fps: that is the slow motion.
    command = [
        ffmpeg, "-y", "-loglevel", "error",
        "-framerate", str(plan["fps"]),
        "-i", str(frames / "frame_%06d.png"),
        *video,
        "-r", str(plan["fps"]),
        str(target),
    ]
    if dry_run:
        log(subprocess.list2cmdline(command))
        return target
    if shutil.which(ffmpeg) is None and not Path(ffmpeg).exists():
        fail(f"ffmpeg not found ('{ffmpeg}'); install it or pass --ffmpeg")
    subprocess.run(command, check=True)
    log(f"  encoded {target}")
    return target


# --- main ----------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--shots", default=str(REPO / "scripts" / "promo" / "shots.yml"))
    parser.add_argument("--out", default=str(REPO / "out" / "promo"))
    parser.add_argument("--only", help="comma list of shot ids")
    parser.add_argument("--stage", choices=["plan", "render", "encode", "all"], default="all",
                        help="stop after this stage (render and encode reuse an existing plan)")
    parser.add_argument("--preview", metavar="ID", help="play one shot live and looping, to tune its camera")
    parser.add_argument("--resimulate", action="store_true", help="re-run races even when a replay is cached")
    parser.add_argument("--drop-races", action="store_true",
                        help="delete the full race replays (~200 MB each) once every shot is cut")
    parser.add_argument("--game", help="ApexSim.exe (packaged) or UnrealEditor.exe (runs the .uproject with -game)")
    parser.add_argument("--game-args", default="", help="extra arguments for the game, e.g. \"-dx12\"")
    parser.add_argument("--resolution", help="WxH; default from shots.yml")
    parser.add_argument("--timeout", type=float, default=1800.0, help="seconds per render")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--codec", choices=["h264", "prores"], default="h264")
    parser.add_argument("--keep-frames", action="store_true", help="keep the PNG frames after encoding")
    parser.add_argument("--replay-tool", help="path to apexsim-replay")
    parser.add_argument("--no-build", action="store_true",
                        help="use the apexsim-replay already built instead of an incremental cargo build")
    parser.add_argument("--dry-run", action="store_true", help="plan, then print the game and ffmpeg commands")
    args = parser.parse_args()

    config = load_config(Path(args.shots))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    defaults = config.get("defaults", {})
    resolution = parse_resolution(args.resolution or defaults.get("resolution", "1920x1080"))

    shots = config["shots"]
    wanted = [args.preview] if args.preview else (args.only.split(",") if args.only else None)
    if wanted:
        unknown = set(wanted) - {s["id"] for s in shots}
        if unknown:
            fail(f"no shot(s) {', '.join(sorted(unknown))}")
        shots = [s for s in shots if s["id"] in wanted]

    tool = ReplayTool(args.replay_tool, not args.no_build, args.dry_run)
    planner = Planner(config, tool, out, args.resimulate)

    plans = []
    for shot in shots:
        plan_file = out / "cuts" / f"{shot['id']}.plan.json"
        if args.stage in ("render", "encode") and plan_file.exists():
            plans.append(json.loads(plan_file.read_text(encoding="utf-8")))
        else:
            plans.append(planner.plan(shot))

    if args.drop_races:
        for race in (out / "races").glob("*.bin"):
            race.unlink()
            log(f"dropped {race}")

    if args.preview:
        render(plans[0], game_command(args.game), out, resolution, args.timeout,
               args.game_args.split(), args.dry_run, preview=True)
        return
    if args.stage == "plan":
        log(f"planned {len(plans)} shot(s) into {out / 'cuts'}")
        return

    game = game_command(args.game) if args.stage in ("render", "all") else []
    manifest_path = out / "clips" / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    failed = []
    for plan in plans:
        if args.stage in ("render", "all"):
            if not render(plan, game, out, resolution, args.timeout, args.game_args.split(), args.dry_run):
                failed.append(plan["id"])
                continue
            if args.stage == "render":
                continue
        target = encode(plan, out, args.ffmpeg, args.codec, args.dry_run)
        if target and not args.dry_run:
            manifest[plan["id"]] = {
                "file": str(target),
                "seconds": plan["screen_seconds"],
                "caption": plan.get("caption"),
                "track": plan["track"],
            }
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            if not args.keep_frames:
                shutil.rmtree(out / "frames" / plan["id"], ignore_errors=True)

    if failed:
        fail(f"{len(failed)} shot(s) did not render: {', '.join(failed)}")
    log("done" + ("" if args.dry_run else f"; clips in {out / 'clips'}"))


if __name__ == "__main__":
    main()
