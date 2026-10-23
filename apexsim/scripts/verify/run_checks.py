#!/usr/bin/env python3
"""Run the unattended screen checks of docs/verification.md.

    python scripts/verify/run_checks.py --list
    python scripts/verify/run_checks.py r1-spa-wet          # one run
    python scripts/verify/run_checks.py r1-spa-wet --dry-run
    python scripts/verify/run_checks.py --sheets r1-spa-wet # re-tile the shots on disk
    python scripts/verify/run_checks.py "r8-*" --repeat 5 --game-arg=-gpucrashdebugging

A run id may be a glob (`r8-*`). `--repeat N` runs the chosen runs N times
over, each pass into out/verify/<run>/pass_NN/, and tallies each pass
(clean, incomplete, or the GPU/crash line, and the client's first
"Bounds watch" error if any) in out/verify/soak.log: the GPU stability
soak. A run's args may say `{repo}` for the repository root. `--game-arg` (repeatable) adds a switch to the game's
command line.

A run (scripts/verify/runs.yml) is one launch of the game against a server
of its own: a circuit, a sky, debug events and a list of shots, each with a
label and the console commands that frame it. What it leaves in
out/verify/<run>/:

  shots/NN.jpg      every grab, full size, in shot order
  sheet_N.jpg       the grabs tiled 2x2 with their labels: what gets judged
  shots.md          the shot list (number, time, label, the item it checks)
  client.log, server.log

The server is started from server/server/target/release with a server.toml of the
run's own (ports from 9400, records and replays inside the run's folder,
the dashboard on 9403 with the token `verify`), so a server the user has
running on 9000 is never touched and its records never written. The game
reads a settings.yml of the run's own (-ApexSettingsDir): windowed, the
run's resolution, that server.

Times: -ApexScreenshotAfter and -ApexExecAfter count from the race view's
clock, which starts as the session is created; the server's debug events
count from the green, -ApexCountdown seconds (and about one more) later.
A shot's `exec` commands run `lead` seconds (default 3) before its grab.

A run may also drive the dashboard (`dashboard:` entries: `at`, `action`
broadcast|stop, `message`): the runner logs in with the run's token and
fires each at its game-clock time, which it reads off a marker command the
run executes at 1 s and the client logs.

A sitting (`--sit <block>`, scripts/verify/sittings.yml) is the same
launch with a human driving: docs/wheel-sitting.md. No shots and no time
limit; the game keeps the player's own display settings and save (the
save games are backed up once per sitting folder), and when it is closed
(or killed) the block's client and server logs, the MARK CSVs
(Saved/Marks) and the lap traces (Saved/Telemetry) written meanwhile are
gathered into out/sit/<date>/<block>/, with marks.md listing the MARK and
force lines. `--sit-sheet` writes out/sit/<date>/sheet.md, every block's
checks with their answer words.

Needs the ApexSimEditor target built and `cargo build --release` in server/server/
(both are checked against the source and warned about), the content
initialised, and the editor closed.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "site"))
from make_shots import GRABS, UPROJECT, engine_editor, listening, settled  # noqa: E402

RUNS_YML = Path(__file__).resolve().parent / "runs.yml"
SITTINGS_YML = Path(__file__).resolve().parent / "sittings.yml"
OUT = REPO / "out" / "verify"
SIT_OUT = REPO / "out" / "sit"
SAVED = REPO / "game-unreal" / "Saved"
SERVER_DIR = REPO / "server" / "server"
SERVER_EXE = SERVER_DIR / "target" / "release" / "apexsim-server.exe"
CLIENT_LOGS = REPO / "game-unreal" / "Saved" / "Logs"
EDITOR_BINARIES = REPO / "game-unreal" / "Binaries" / "Win64"

BASE_PORT = 9400
ADMIN_TOKEN = "verify"
# Engine start, shader warm-up and the track build come before the clock the
# grab times are counted on.
STARTUP_ALLOWANCE_S = 180.0
TILE = (960, 540)


def log(message: str) -> None:
    print(message, flush=True)


def newest(folder: Path, pattern: str) -> float:
    return max((p.stat().st_mtime for p in folder.rglob(pattern)), default=0.0)


def warn_stale() -> None:
    if not SERVER_EXE.is_file():
        raise SystemExit(f"{SERVER_EXE} not found: cargo build --release in server/server/")
    if max(newest(SERVER_DIR / "src", "*.rs"), newest(REPO / "server" / "sim" / "src", "*.rs")) > SERVER_EXE.stat().st_mtime:
        log("warning: server/server/target/release is older than server/server/src or server/sim/src")
    # Each module's source against its own DLL: a change to one module
    # rebuilds that DLL alone.
    for module in (REPO / "game-unreal" / "Source").iterdir():
        dll = EDITOR_BINARIES / f"UnrealEditor-{module.name}.dll"
        if module.is_dir() and dll.is_file()                 and max(newest(module, "*.cpp"), newest(module, "*.h")) > dll.stat().st_mtime:
            log(f"warning: UnrealEditor-{module.name}.dll is older than its source: build ApexSimEditor")


# --- the server ---------------------------------------------------------------

def server_toml(work: Path, port: int) -> Path:
    """server/server.toml with the run's own ports, folders and dashboard:
    the checked-in file stays the one source of every other key."""
    content = (REPO / ".." / "apexsim-content").resolve()
    replace = {
        ("network", "tcp_bind"): f'"127.0.0.1:{port}"',
        ("network", "udp_bind"): f'"127.0.0.1:{port + 1}"',
        ("network", "health_bind"): f'"127.0.0.1:{port + 2}"',
        ("content", "cars_dir"): f'"{(content / "cars").as_posix()}"',
        ("content", "tracks_dir"): f'"{(content / "tracks").as_posix()}"',
        ("records", "dir"): '"./records"',
        ("records", "replay_dir"): '"./replays"',
        ("showcase", "enabled"): "false",
        ("admin", "enabled"): "true",
        ("admin", "http_bind"): f'"127.0.0.1:{port + 3}"',
        ("admin", "https_bind"): f'"127.0.0.1:{port + 4}"',
        ("admin", "token"): f'"{ADMIN_TOKEN}"',
    }
    lines, section, seen = [], None, set()

    def missing(name):
        return [f"{k} = {v}" for (s, k), v in replace.items() if s == name and (s, k) not in seen]

    for line in (SERVER_DIR / "server.toml").read_text(encoding="utf-8").splitlines():
        header = re.match(r"^\[(\w+)\]\s*$", line)
        if header:
            lines += missing(section)
            section = header.group(1)
            lines.append(line)
            continue
        # A commented default (`#enabled = true`) counts as the key.
        key = re.match(r"^#?\s*(\w+)\s*=", line)
        if key and (section, key.group(1)) in replace:
            seen.add((section, key.group(1)))
            lines.append(f"{key.group(1)} = {replace[(section, key.group(1))]}")
            continue
        lines.append(line)
    lines += missing(section)
    path = work / "server.toml"
    path.write_text("# Written by scripts/verify/run_checks.py from server/server.toml.\n"
                    + "\n".join(lines) + "\n", encoding="utf-8")
    return path


class Server:
    def __init__(self, run: dict, work: Path, port: int):
        self.run, self.work, self.port = run, work, port
        self.process = None
        self.log_file = None

    def __enter__(self):
        if listening("127.0.0.1", self.port):
            raise SystemExit(f"something already listens on {self.port}: stop it or pass --port")
        config = server_toml(self.work, self.port)
        env = dict(os.environ)
        server = self.run.get("server") or {}
        if server.get("stand_in"):
            env["APEXSIM_DEBUG_STAND_IN_DRIVER"] = "true"
        if server.get("events"):
            events = server["events"]
            env["APEXSIM_DEBUG_EVENTS"] = ";".join(events) if isinstance(events, list) else str(events)
        for key, value in (server.get("env") or {}).items():
            env[key] = str(value)
        self.log_file = open(self.work / "server.log", "w", encoding="utf-8")
        self.process = subprocess.Popen([str(SERVER_EXE), "--config", str(config)], cwd=self.work, env=env,
                                        stdout=self.log_file, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60.0
        while not listening("127.0.0.1", self.port):
            if self.process.poll() is not None:
                raise SystemExit(f"the server exited at once: see {self.work / 'server.log'}")
            if time.monotonic() > deadline:
                self.process.kill()
                raise SystemExit(f"the server did not open {self.port} within a minute")
            time.sleep(0.5)
        return self

    def __exit__(self, *exc):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
        if self.log_file:
            self.log_file.close()


# --- the game -------------------------------------------------------------------

def settings_dir(work: Path, port: int, width: int, height: int) -> Path:
    folder = work / "settings"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "settings.yml").write_text(
        "# Written by scripts/verify/run_checks.py for one run.\n"
        "display:\n"
        f"  resolution: {width}x{height}\n"
        "  window_mode: windowed\n"
        "  vsync: false\n"
        "  frame_limit: 60\n"
        "  screens: 1\n"
        "server:\n"
        "  host: 127.0.0.1\n"
        f"  port: {port}\n",
        encoding="utf-8")
    return folder


def shot_list(run: dict) -> list[dict]:
    shots = [dict(s) for s in run.get("shots") or []]
    for i, shot in enumerate(shots):
        shot["n"] = i + 1
        shot["at"] = float(shot["at"])
    return shots


# Executed at 1 s by every run that drives the dashboard; its log line tells
# the runner where the game clock stands.
CLOCK_MARKER = "log LogApexVerifyClock Log"


def exec_after(run: dict, shots: list[dict]) -> str:
    entries = []
    if run.get("dashboard"):
        entries.append((1.0, CLOCK_MARKER))
    for t, command in (run.get("exec") or {}).items() if isinstance(run.get("exec"), dict) else []:
        for c in command if isinstance(command, list) else [command]:
            entries.append((float(t), c))
    for shot in shots:
        commands = shot.get("exec") or []
        if isinstance(commands, str):
            commands = [commands]
        lead = float(shot.get("lead", 3))
        for k, c in enumerate(commands):
            # Several commands for one shot keep their order a tenth apart.
            entries.append((max(0.5, shot["at"] - lead + 0.1 * k), c))
    for _, c in entries:
        if "|" in c:
            raise SystemExit(f"a command may not hold '|': {c}")
    entries.sort(key=lambda e: e[0])
    return "|".join(f"{t:g}={c}" for t, c in entries)


def game_command(run: dict, shots: list[dict], game: list[str], work: Path, port: int,
                 extra: list[str] | None = None) -> list[str]:
    width, height = (int(v) for v in str(run.get("resolution", "1920x1080")).split("x"))
    times = ",".join(f"{s['at']:g}" for s in shots)
    command = [
        *game,
        "-windowed", f"-ResX={width}", f"-ResY={height}",
        f"-ExecCmds=r.SetRes {width}x{height}w, DisableAllScreenMessages",
        "-ApexNoSplashHold", "-unattended", "-nosplash",
        f"-ApexSettingsDir={settings_dir(work, port, width, height)}",
        *[str(a).replace("{repo}", REPO.as_posix()) for a in run.get("args") or []],
        *(extra or []),
        f"-ApexScreenshotAfter={times}",
        f"-log=Verify_{run['id']}.log",
    ]
    execs = exec_after(run, shots)
    if execs:
        command.append(f"-ApexExecAfter={execs}")
    return command


class Dashboard:
    """The run's server's dashboard over HTTP, logged in with its token."""

    def __init__(self, port: int):
        import http.cookiejar
        import urllib.request
        self.base = f"http://127.0.0.1:{port + 3}"
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.call("POST", "/api/login", {"token": ADMIN_TOKEN})

    def call(self, method: str, path: str, body: dict | None = None):
        import json
        import urllib.request
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.base + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
        with self.opener.open(request, timeout=10) as response:
            text = response.read().decode() or "null"
        return json.loads(text)

    def act(self, entry: dict) -> str:
        action = entry["action"]
        if action == "broadcast":
            sent = self.call("POST", "/api/broadcast", {"message": entry.get("message", "Verification run")})
            return f"broadcast sent to {sent.get('sent')}"
        if action == "stop":
            sessions = self.call("GET", "/api/sessions").get("sessions") or []
            ids = [s.get("id") for s in sessions if s.get("id")]
            for sid in ids:
                self.call("POST", f"/api/sessions/{sid}/stop", {"reason": entry.get("message", "Verification run")})
            return f"stopped {len(ids)} session(s)"
        raise ValueError(f"unknown dashboard action {action}")


def drive_dashboard(run: dict, port: int, client_log: Path, stop) -> None:
    """Fire the run's dashboard actions at their game-clock times (a thread)."""
    entries = sorted(run.get("dashboard") or [], key=lambda e: float(e["at"]))
    if not entries:
        return
    origin = None
    while origin is None and not stop.is_set():
        # The executed command's own line: the command line, logged at
        # start-up, holds the marker's text too.
        if client_log.is_file() and f"-ApexExecAfter: {CLOCK_MARKER}" in client_log.read_text(encoding="utf-8", errors="replace"):
            # The marker runs at 1 s of the game clock.
            origin = time.monotonic() - 1.0
        else:
            time.sleep(0.25)
    if origin is None:
        return
    dashboard = Dashboard(port)
    for entry in entries:
        while not stop.is_set() and time.monotonic() - origin < float(entry["at"]):
            time.sleep(0.1)
        if stop.is_set():
            return
        try:
            log(f"  dashboard at {entry['at']} s: {dashboard.act(entry)}")
        except Exception as error:  # noqa: BLE001 - reported, the run goes on
            log(f"  dashboard at {entry['at']} s failed: {error}")


def take(run: dict, game: list[str], work: Path, port: int, dry_run: bool,
         extra: list[str] | None = None) -> bool:
    shots = shot_list(run)
    command = game_command(run, shots, game, work, port, extra)
    if dry_run:
        log(subprocess.list2cmdline(command))
        return True
    folder = work / "shots"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    before = set(GRABS.glob("*.png")) if GRABS.is_dir() else set()
    started = time.monotonic()
    client_log = CLIENT_LOGS / f"Verify_{run['id']}.log"
    if client_log.is_file():
        client_log.unlink()
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import threading
    stop_dashboard = threading.Event()
    dashboard = threading.Thread(target=drive_dashboard, args=(run, port, client_log, stop_dashboard), daemon=True)
    dashboard.start()
    grabs: list[Path] = []
    ok = True
    try:
        deadline = started + max(s["at"] for s in shots) + STARTUP_ALLOWANCE_S
        while len(grabs) < len(shots):
            if process.poll() is not None:
                log(f"  the game exited early (code {process.returncode}) after {len(grabs)} grab(s)")
                ok = False
                break
            if time.monotonic() > deadline:
                log(f"  gave up after {deadline - started:.0f} s with {len(grabs)} of {len(shots)} grab(s)")
                ok = False
                break
            time.sleep(1.0)
            fresh = sorted(set(GRABS.glob("*.png")) - before - set(grabs), key=lambda p: p.stat().st_mtime) \
                if GRABS.is_dir() else []
            for grab in fresh:
                if settled(grab):
                    grabs.append(grab)
                    log(f"  grab {len(grabs)}/{len(shots)} at {time.monotonic() - started:.0f} s: "
                        f"{shots[len(grabs) - 1].get('label', '')}")
    finally:
        stop_dashboard.set()
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
    from PIL import Image
    for i, grab in enumerate(grabs):
        with Image.open(grab) as opened:
            opened.convert("RGB").save(folder / f"{i + 1:02d}.jpg", quality=90)
        grab.unlink()
    client_log = CLIENT_LOGS / f"Verify_{run['id']}.log"
    if client_log.is_file():
        shutil.copyfile(client_log, work / "client.log")
    return ok


# --- the review sheets ------------------------------------------------------------

def write_sheets(run: dict, work: Path) -> list[Path]:
    from PIL import Image, ImageDraw, ImageFont

    shots = shot_list(run)
    files = sorted((work / "shots").glob("[0-9][0-9].jpg"))
    for old in work.glob("sheet_*.jpg"):
        old.unlink()
    try:
        font = ImageFont.truetype("arial.ttf", 20)
    except OSError:
        font = ImageFont.load_default()
    sheets = []
    w, h = TILE
    for start in range(0, len(files), 4):
        batch = files[start:start + 4]
        sheet = Image.new("RGB", (2 * w, 2 * h if len(batch) > 2 else h))
        for k, file in enumerate(batch):
            index = start + k
            label = shots[index].get("label", "") if index < len(shots) else ""
            with Image.open(file) as opened:
                tile = opened.convert("RGB").resize((w, h))
            draw = ImageDraw.Draw(tile)
            text = f"{index + 1}  {label}"
            box = draw.textbbox((0, 0), text, font=font)
            draw.rectangle((0, 0, box[2] + 16, box[3] + 10), fill=(0, 0, 0))
            draw.text((8, 4), text, fill=(255, 220, 0), font=font)
            sheet.paste(tile, ((k % 2) * w, (k // 2) * h))
        path = work / f"sheet_{start // 4 + 1}.jpg"
        sheet.save(path, quality=88)
        sheets.append(path)
    lines = [f"# {run['id']}: {run.get('about', '')}", ""]
    for shot in shots:
        taken = (work / "shots" / f"{shot['n']:02d}.jpg").is_file()
        lines.append(f"{shot['n']:2}. {shot['at']:6g} s  {shot.get('label', '')}"
                     f"{'' if taken else '  (not taken)'}")
        if shot.get("checks"):
            lines.append(f"      checks: {shot['checks']}")
    (work / "shots.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return sheets


#: Client log lines that mean the GPU or the process went down: a soak's tally.
GPU_FAILURES = ("DEVICE_HUNG", "DEVICE_REMOVED", "GPU crash", "Fatal error")


def log_findings(work: Path) -> list[str]:
    """Lines of the client log worth a look before judging the pictures;
    returns those that are a GPU failure or a crash."""
    client_log = work / "client.log"
    if not client_log.is_file():
        log("  no client log")
        return []
    keys = ("Fatal error", "DEVICE_HUNG", "LogApexSim: Error", "LogApexSimNet: Error", "Disconnected",
            "AutoRace", "-ApexExecAfter", "Unknown command", "Bounds watch", "Radio:")
    hits = [line.rstrip() for line in client_log.read_text(encoding="utf-8", errors="replace").splitlines()
            if any(k in line for k in keys)]
    (work / "client_notes.log").write_text("\n".join(hits) + "\n", encoding="utf-8")
    log(f"  {len(hits)} notable client log lines in client_notes.log")
    return [line for line in hits if any(k in line for k in GPU_FAILURES)]


def bad_bounds(work: Path) -> list[str]:
    """The bounds watch's error lines (a NaN, infinite or out-of-world
    primitive), from the notes log_findings wrote."""
    notes = work / "client_notes.log"
    if not notes.is_file():
        return []
    return [line for line in notes.read_text(encoding="utf-8").splitlines()
            if "Bounds watch" in line and "Error" in line]


# ------------------------------------------------------------------------------

# --- a sitting: a human drives -----------------------------------------------------

DEFAULT_ANSWERS = ["OK", "weak", "strong", "late", "wrong", "not felt"]


def load_sittings() -> list[dict]:
    with open(SITTINGS_YML, encoding="utf-8") as f:
        return yaml.safe_load(f)["sittings"]


def sit_settings_dir(work: Path, port: int) -> Path:
    """The player's own settings.yml (display, screens) with the block's server."""
    folder = work / "settings"
    folder.mkdir(parents=True, exist_ok=True)
    own = REPO / "game-unreal" / "settings.yml"
    server = ["  host: 127.0.0.1", f"  port: {port}", "  tls: auto", "  tls_fingerprint: "]
    lines, section, sections = [], None, set()
    for line in (own.read_text(encoding="utf-8").splitlines() if own.is_file() else []):
        header = re.match(r"^(\w+):\s*$", line)
        if header:
            section = header.group(1)
            sections.add(section)
            lines.append(line)
            if section == "server":
                lines += server
            continue
        if section == "server" and re.match(r"^\s+(host|port|tls|tls_fingerprint):", line):
            continue
        lines.append(line)
    if "server" not in sections:
        lines += ["server:", *server]
    (folder / "settings.yml").write_text(
        "# Written by scripts/verify/run_checks.py --sit from game-unreal/settings.yml.\n"
        + "\n".join(lines) + "\n", encoding="utf-8")
    return folder


def sit_command(block: dict, game: list[str], work: Path, port: int, extra: list[str]) -> list[str]:
    command = [
        *game,
        "-ApexNoSplashHold", "-nosplash",
        f"-ApexSettingsDir={sit_settings_dir(work, port)}",
        *[str(a).replace("{repo}", REPO.as_posix()) for a in block.get("args") or []],
        *extra,
        f"-log=Sit_{block['id']}.log",
    ]
    execs = exec_after(block, [])
    if execs:
        command.append(f"-ApexExecAfter={execs}")
    return command


def back_up_save(day: Path) -> None:
    """The player's save games, once per sitting folder: blocks rebind and paste setups."""
    backup = day / "SaveGames.bak"
    source = SAVED / "SaveGames"
    if source.is_dir() and not backup.exists():
        shutil.copytree(source, backup)
        log(f"  save games backed up to {backup}")


def gather_new(source: Path, since: float, target: Path) -> list[Path]:
    """Files under `source` written since `since`, copied under `target`."""
    copied = []
    if not source.is_dir():
        return copied
    for path in source.rglob("*"):
        if path.is_file() and path.stat().st_mtime >= since:
            destination = target / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            copied.append(destination)
    return copied


MARK_LINE = re.compile(r"(MARK \d+ .*|Wheel forces over .*)$")


def write_marks(block: dict, work: Path) -> Path:
    """marks.md: the client log's MARK and 15 s force lines, in order."""
    client = work / "client.log"
    found = []
    if client.is_file():
        for line in client.read_text(encoding="utf-8", errors="replace").splitlines():
            match = MARK_LINE.search(line)
            if match:
                found.append(match.group(1))
    marks = [line for line in found if line.startswith("MARK")]
    path = work / "marks.md"
    path.write_text(f"# {block['id']}: {block.get('about', '')}\n\n"
                    f"{len(marks)} mark(s), {len(found) - len(marks)} force line(s).\n\n"
                    + "".join(f"- `{line}`\n" for line in found), encoding="utf-8")
    return path


def sit(block: dict, game: list[str], day: Path, port: int, dry_run: bool, extra: list[str]) -> None:
    work = day / block["id"]
    work.mkdir(parents=True, exist_ok=True)
    command = sit_command(block, game, work, port, extra)
    if dry_run:
        log(subprocess.list2cmdline(command))
        return
    log(f"\n{block['id']} (sitting {block.get('sitting', '?')}, ~{block.get('minutes', '?')} min): "
        f"{block.get('about', '')}")
    if block.get("setup"):
        log(f"  {block['setup']}")
    for line in block.get("before") or []:
        result = subprocess.run(line, shell=True, cwd=REPO, capture_output=True, text=True)
        with open(work / "before.txt", "a", encoding="utf-8") as f:
            f.write(f"$ {line}\n{result.stdout}{result.stderr}\n")
        log(result.stdout.rstrip())
    for n, line in enumerate(block.get("paste") or []):
        log(f"  paste {n + 1}: {line}")
    if block.get("paste") and sys.platform == "win32":
        subprocess.run("clip", input=block["paste"][0], text=True, shell=True)
        log("  (paste 1 is on the clipboard)")
    for check in block.get("checks") or []:
        log(f"  [ ] {check['check']}")
    back_up_save(day)
    started = time.time()
    client_log = CLIENT_LOGS / f"Sit_{block['id']}.log"
    if client_log.is_file():
        client_log.unlink()
    with Server(block, work, port):
        process = subprocess.Popen(command)
        try:
            process.wait()
        except KeyboardInterrupt:
            process.terminate()
            process.wait(timeout=20)
    if client_log.is_file():
        shutil.copyfile(client_log, work / "client.log")
    marks = gather_new(SAVED / "Marks", started, work / "marks")
    traces = gather_new(SAVED / "Telemetry", started, work / "telemetry")
    summary = write_marks(block, work)
    log(f"  {(time.time() - started) / 60:.0f} min; {len(marks)} mark CSV(s), "
        f"{len(traces)} trace file(s); {summary}")


def write_sit_sheet(sittings: list[dict], day: Path) -> Path:
    day.mkdir(parents=True, exist_ok=True)
    lines = [f"# Wheel sitting answer sheet ({day.name})", "",
             "One answer word per row, a line of note if needed, and the MARK number if one was pressed.",
             "", "Base menu: FF ___  NDP ___  NFR ___  SEN ___", ""]
    for number in sorted({b.get("sitting", 1) for b in sittings}):
        lines += [f"## Sitting {number}", ""]
        for block in (b for b in sittings if b.get("sitting", 1) == number):
            lines += [f"### {block['id']}: {block.get('about', '')}", "",
                      f"`python scripts/verify/run_checks.py --sit {block['id']}`", "",
                      "| Check | Answers | Answer | Mark | Note |", "|---|---|---|---|---|"]
            for check in block.get("checks") or []:
                answers = " / ".join(check.get("answers") or DEFAULT_ANSWERS)
                lines.append(f"| {check['check']} | {answers} |  |  |  |")
            lines.append("")
    path = day / "sheet.md"
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def main_sit(args) -> int:
    sittings = load_sittings()
    day = SIT_OUT / args.sit_dir
    if args.sit_sheet:
        log(str(write_sit_sheet(sittings, day)))
        return 0
    if args.list or not args.runs:
        for block in sittings:
            log(f"{block['id']:16} sitting {block.get('sitting', '?')}  ~{block.get('minutes', '?'):>2} min  "
                f"{block.get('about', '')}")
        return 0
    ids = [b["id"] for b in sittings]
    unknown = [r for r in args.runs if not fnmatch.filter(ids, r)]
    if unknown:
        raise SystemExit(f"no such block: {', '.join(unknown)}")
    chosen = [b for b in sittings if any(fnmatch.fnmatchcase(b["id"], pattern) for pattern in args.runs)]
    if not args.dry_run:
        warn_stale()
    editor = engine_editor()
    if not editor:
        raise SystemExit("no Unreal Engine found: set UE to the folder that holds Engine/")
    game = [str(editor), str(UPROJECT), "-game"]
    for block in chosen:
        sit(block, game, day, args.port, args.dry_run, args.game_arg)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Run the unattended screen checks.")
    parser.add_argument("runs", nargs="*", help="run ids from scripts/verify/runs.yml")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="print the game's command line")
    parser.add_argument("--sheets", action="store_true", help="only re-tile the shots on disk")
    parser.add_argument("--port", type=int, default=BASE_PORT)
    parser.add_argument("--repeat", type=int, default=1, help="run the chosen runs this many times over")
    parser.add_argument("--game-arg", action="append", default=[],
                        help="a switch added to the game's command line (repeatable)")
    parser.add_argument("--sit", action="store_true",
                        help="the runs are sitting blocks (scripts/verify/sittings.yml): a human drives")
    parser.add_argument("--sit-sheet", action="store_true", help="write the sitting's answer sheet")
    parser.add_argument("--sit-dir", default=time.strftime("%Y-%m-%d"),
                        help="the sitting's folder under out/sit (default: today)")
    args = parser.parse_args(argv)

    if args.sit or args.sit_sheet:
        return main_sit(args)

    with open(RUNS_YML, encoding="utf-8") as f:
        runs = yaml.safe_load(f)["runs"]
    ids = [r["id"] for r in runs]
    if args.list or not args.runs:
        for run in runs:
            log(f"{run['id']:22} {len(run.get('shots') or []):3} shots  {run.get('about', '')}")
        return 0
    unknown = [r for r in args.runs if not fnmatch.filter(ids, r)]
    if unknown:
        raise SystemExit(f"no such run: {', '.join(unknown)}")
    chosen = [r for r in runs if any(fnmatch.fnmatchcase(r["id"], pattern) for pattern in args.runs)]

    if args.sheets:
        for run in chosen:
            for sheet in write_sheets(run, OUT / run["id"]):
                log(str(sheet))
        return 0

    warn_stale()
    editor = engine_editor()
    if not editor:
        raise SystemExit("no Unreal Engine found: set UE to the folder that holds Engine/")
    game = [str(editor), str(UPROJECT), "-game"]

    failed = []
    soak = OUT / "soak.log" if args.repeat > 1 else None
    for n in range(args.repeat):
        for run in chosen:
            work = OUT / run["id"]
            if args.repeat > 1:
                work = work / f"pass_{n + 1:02d}"
            work.mkdir(parents=True, exist_ok=True)
            of = f" (pass {n + 1} of {args.repeat})" if args.repeat > 1 else ""
            log(f"{run['id']}{of}: {run.get('about', '')}")
            if args.dry_run:
                take(run, game, work, args.port, True, args.game_arg)
                continue
            started = time.monotonic()
            with Server(run, work, args.port):
                ok = take(run, game, work, args.port, False, args.game_arg)
            sheets = write_sheets(run, work)
            gpu = log_findings(work)
            log(f"  {time.monotonic() - started:.0f} s; sheets: {', '.join(str(s) for s in sheets) or 'none'}")
            if not ok:
                failed.append(run["id"])
            if soak:
                state = "GPU/CRASH | " + gpu[0][:200] if gpu else ("incomplete" if not ok else "clean")
                bounds = bad_bounds(work)
                if bounds:
                    state += " | BOUNDS | " + bounds[0][:300]
                with open(soak, "a", encoding="utf-8") as f:
                    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} pass {n + 1} {run['id']}: {state}\n")
    if failed:
        log(f"incomplete: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
