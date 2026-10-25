#!/usr/bin/env python3
"""The verification backlog's tool: docs/verifier.md.

    python scripts/verify/verifier.py check                 # validate the items file
    python scripts/verify/verifier.py serve                 # the review page on :9410
    python scripts/verify/verifier.py serve --lan           # and on the LAN: sitting mode from a tablet
    python scripts/verify/verifier.py add --id v-r1-x --title ... --area cars --what ...
    python scripts/verify/verifier.py add - < item.yml      # or a YAML item (or list) on stdin
    python scripts/verify/verifier.py evidence <item> --run r1-spa-wet --shots 3,4
    python scripts/verify/verifier.py judge <item> --says looks-right --text "..." --shots 3
    python scripts/verify/verifier.py reviewed              # saved reviews not yet processed
    python scripts/verify/verifier.py processed <item>...   # the agent has acted on them
    python scripts/verify/verifier.py close <item>...       # into verification-items.closed.yml
    python scripts/verify/verifier.py import                # once: docs/verification.md's bullets

The items file is docs/verification/verification-items.yml. The agent edits
it through these commands (or by hand within the schema); the page writes
only `status` and `review`, and refuses to save over a file that changed on
disk since it loaded. The tool runs nothing and judges nothing.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import threading
import webbrowser
from datetime import date, datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verifier import schema  # noqa: E402

REPO = schema.REPO
WEB = Path(__file__).resolve().parent / "verifier" / "web"
FONTS = REPO / "server" / "server" / "src" / "admin" / "web" / "fonts"
RUNS_YML = Path(__file__).resolve().parent / "runs.yml"
PORT = 9410
THUMB_WIDTH = 480
LOG_TAIL = 400_000


def head_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=REPO, capture_output=True,
                              text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def shot_list(text: str | None) -> list[int]:
    return [int(s) for s in re.split(r"[,\s]+", text or "") if s]


# --- the commands -------------------------------------------------------------------

def cmd_check(args) -> int:
    f = schema.ItemsFile()
    closed = schema.ItemsFile(schema.CLOSED_YML).items() if schema.CLOSED_YML.exists() else []
    items = f.items()
    errors, warnings = schema.validate(items, closed, check_paths=not args.no_paths)
    for w in warnings if args.warnings else []:
        print(f"warning: {w}")
    for e in errors:
        print(f"error: {e}")
    counts = {s: sum(1 for i in items if i.get("status") == s) for s in schema.STATUSES}
    summary = ", ".join(f"{n} {s}" for s, n in counts.items() if n)
    print(f"{len(items)} items ({summary}), {len(closed)} closed; "
          f"{len(errors)} errors, {len(warnings)} warnings" + ("" if args.warnings or not warnings else " (--warnings)"))
    return 1 if errors else 0


def cmd_add(args) -> int:
    f = schema.ItemsFile()
    if args.stdin == "-":
        loaded = schema.parse(sys.stdin.read())
        new = loaded if isinstance(loaded, list) else [loaded]
    else:
        item = {"id": args.id, "title": args.title, "area": args.area, "kind": args.kind,
                "check_by": args.check_by, "created": args.created or date.today().isoformat(),
                "commit": args.commit or head_commit(), "plan": args.plan, "doc": args.doc,
                "what": args.what, "status": "blocked" if args.blocked else "open",
                "needs": [n for n in (args.needs or "").split(",") if n] or None}
        new = [{k: v for k, v in item.items() if v}]
    for item in new:
        item.setdefault("status", "open")
        item.setdefault("created", date.today().isoformat())
        item.setdefault("commit", head_commit())
        errors, _ = schema.validate([item], f.items(), check_paths=False)
        if errors:
            print("\n".join(f"error: {e}" for e in errors))
            return 1
        f.add(item)
        print(f"added {item['id']}")
    f.save()
    return 0


def cmd_evidence(args) -> int:
    f = schema.ItemsFile()
    ev = {"run": args.run, "date": args.date or date.today().isoformat(), "commit": args.commit or head_commit(),
          "shots": shot_list(args.shots), "kind": args.kind}
    if args.note:
        ev["note"] = args.note
    ev = {k: v for k, v in ev.items() if v not in (None, "", [])}
    for item_id in args.item:
        schema.add_evidence(f, item_id, ev)
        print(f"{item_id}: evidence {args.run}" + (f" shots {args.shots}" if args.shots else ""))
    f.save()
    return 0


def cmd_judge(args) -> int:
    f = schema.ItemsFile()
    verdict = {"date": args.date or date.today().isoformat(), "by": "agent", "says": args.says, "text": args.text}
    if args.shots:
        verdict["shots"] = shot_list(args.shots)
    schema.set_verdict(f, args.item, verdict)
    f.save()
    print(f"{args.item}: {args.says}")
    return 0


def cmd_reviewed(args) -> int:
    items = [i for i in schema.ItemsFile().items()
             if i.get("review") and (args.all or not i["review"].get("processed"))]
    if args.yaml:
        print(yaml.safe_dump([{k: i.get(k) for k in ("id", "title", "status", "check_by", "plan", "doc", "verdict",
                                                     "review")} for i in items], sort_keys=False, allow_unicode=True))
        return 0
    for i in items:
        r = i["review"]
        marks = " ".join(x for x in (f"score {r['score']}" if r.get("score") else "", "FLAG" if r.get("flag") else "") if x)
        print(f"{i['id']}  {i['status'].upper()}  {marks}".rstrip())
        print(f"  {i['title']}  ({i.get('check_by')}{', ' + i['plan'] if i.get('plan') else ''})")
        if r.get("answer"):
            print(f"  answered: {r['answer']}")
        if r.get("comment"):
            print(f"  says: {r['comment']}")
        if r.get("shots"):
            print("  about: " + ", ".join(f"{s['run']} #{s['shot']}" for s in r["shots"]))
        print()
    print(f"{len(items)} reviewed, not processed" if not args.all else f"{len(items)} reviewed")
    return 0


def cmd_processed(args) -> int:
    f = schema.ItemsFile()
    for item_id in args.item:
        review = f.get(item_id).get("review")
        if not review:
            print(f"{item_id}: no review")
            return 1
        f.set(item_id, "review", {**review, "processed": True})
        print(f"{item_id}: processed")
    f.save()
    return 0


def cmd_close(args) -> int:
    f = schema.ItemsFile()
    closed = schema.ItemsFile(schema.CLOSED_YML)
    if not schema.CLOSED_YML.exists():
        closed.head = ["# Closed verification items (docs/verifier.md), kept for the record and the totals.\n", "\n"]
    for item_id in args.item:
        item = f.get(item_id)
        if item.get("review") and not item["review"].get("processed") and not args.force:
            print(f"{item_id}: its review is not processed yet (--force to close anyway)")
            return 1
        block = f.remove(item_id)
        closed.blocks.append(block.splitlines(keepends=True) + ([] if block.endswith("\n\n") else ["\n"]))
        print(f"closed {item_id}")
    closed.save()
    f.save()
    return 0


def cmd_import(args) -> int:
    from verifier import importer
    if schema.ITEMS_YML.exists() and schema.ItemsFile().blocks and not args.force:
        print(f"{schema.ITEMS_YML.relative_to(REPO)} has items already; the import runs once (--force to redo)")
        return 1
    f = schema.ItemsFile.__new__(schema.ItemsFile)
    f.path = schema.ITEMS_YML
    f.head, f.blocks = schema.HEADER.splitlines(keepends=True) + ["\n"], []
    f.loaded_digest = None
    for item in importer.run_import():
        f.add(item)
    f.save()
    print(f"{len(f.blocks)} items into {schema.ITEMS_YML.relative_to(REPO)}")
    return 0


# --- the page -----------------------------------------------------------------------

SAFE = re.compile(r"^[A-Za-z0-9_.-]+$")
_save_lock = threading.Lock()
_runs_cache: dict = {"mtime": None, "runs": {}}


def runs_by_id() -> dict:
    mtime = RUNS_YML.stat().st_mtime if RUNS_YML.exists() else None
    if mtime != _runs_cache["mtime"]:
        runs = yaml.safe_load(RUNS_YML.read_text(encoding="utf-8")).get("runs", []) if mtime else []
        _runs_cache.update(mtime=mtime, runs={r["id"]: r for r in runs})
    return _runs_cache["runs"]


def replay_command(run: str) -> str:
    """The command that opens the run's newest kept replay in the game's watch view, or ""."""
    replays = sorted((schema.VERIFY_OUT / run / "replays").glob("*.apxs"), key=lambda p: p.stat().st_mtime)
    if not replays:
        return ""
    script = REPO / "scripts" / "play_editor.ps1"
    return (f'powershell -ExecutionPolicy Bypass -File "{script}" -NoServer -ApexNoDemo '
            f'-ApexWatchReplay="{replays[-1]}"')


def run_info(run: str, commit: str | None = None, gen: str | None = None) -> dict:
    """A run's shots (at `commit`, or the kept generation `gen`), logs and history."""
    folder = schema.VERIFY_OUT / run
    shots_at = schema.shots_folder(run, commit, gen)
    shots = schema.run_shots(run, commit, gen)
    recipe = runs_by_id().get(run, {})
    when = ""
    if (shots_at / "shots").exists():
        when = datetime.fromtimestamp((shots_at / "shots").stat().st_mtime).strftime("%Y-%m-%d %H:%M")
    logs = [name for name, file in (("client", "client.log"), ("notes", "client_notes.log"), ("server", "server.log"))
            if (folder / file).exists()]
    return {"run": run, "about": recipe.get("about", ""), "exists": folder.exists(), "date": when,
            "commit": schema.run_stamp(shots_at).get("commit", ""), "kept": shots_at != folder,
            "shots": shots, "logs": logs, "history": schema.history(run), "replay": replay_command(run)}


def thumbnail(run: str, n: int, commit: str | None = None, gen: str | None = None) -> Path | None:
    source = schema.shot_path(run, n, commit, gen)
    if not source.exists():
        return None
    thumb = source.parent.parent / "thumbs" / source.name
    if not thumb.exists() or thumb.stat().st_mtime < source.stat().st_mtime:
        from PIL import Image
        thumb.parent.mkdir(exist_ok=True)
        with Image.open(source) as image:
            image.thumbnail((THUMB_WIDTH, THUMB_WIDTH))
            image.convert("RGB").save(thumb, quality=82)
    return thumb


class Handler(BaseHTTPRequestHandler):
    server_version = "ApexSimVerifier/1"

    def log_message(self, fmt, *args):  # quiet: the page polls nothing
        if self.command == "POST":
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    def send(self, status, body: bytes, kind: str, cache: bool = False):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "max-age=3600" if cache else "no-store")
        self.end_headers()
        self.wfile.write(body)

    def json(self, value, status=HTTPStatus.OK):
        self.send(status, json.dumps(value, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

    def file(self, path: Path | None, kind: str, cache=False):
        if not path or not path.is_file():
            return self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")
        self.send(HTTPStatus.OK, path.read_bytes(), kind, cache)

    def do_GET(self):
        url = urlparse(self.path)
        parts = [p for p in url.path.split("/") if p]
        try:
            if not parts:
                return self.file(WEB / "index.html", "text/html; charset=utf-8")
            if parts == ["favicon.ico"]:
                return self.send(HTTPStatus.NO_CONTENT, b"", "image/x-icon")
            if parts[0] in ("app.js", "app.css") and len(parts) == 1:
                kind = "text/javascript" if parts[0].endswith(".js") else "text/css"
                return self.file(WEB / parts[0], kind + "; charset=utf-8")
            if parts[0] == "fonts" and len(parts) == 2 and SAFE.match(parts[1]):
                return self.file(FONTS / parts[1], "font/woff2", cache=True)
            query = parse_qs(url.query)
            commit, gen = (query.get(k, [""])[0] for k in ("commit", "gen"))
            commit = commit if SAFE.match(commit) else None
            gen = gen if SAFE.match(gen) else None
            if parts[:2] == ["api", "items"]:
                f = schema.ItemsFile()
                items = f.items()
                closed = len(schema.ItemsFile(schema.CLOSED_YML).blocks) if schema.CLOSED_YML.exists() else 0
                stale = {i["id"]: run for i in items if (run := schema.stale(i))}
                return self.json({"items": items, "digest": f.loaded_digest, "closed": closed, "stale": stale,
                                  "areas": schema.AREAS, "kinds": schema.KINDS, "statuses": schema.STATUSES,
                                  "file": schema.ITEMS_YML.name})
            if parts[:2] == ["api", "sittings"]:
                blocks = schema.load_sittings()
                return self.json({"blocks": [{**{k: b.get(k) for k in ("id", "sitting", "minutes", "about", "setup",
                                                                        "checks")},
                                              "marks": schema.latest_marks(b["id"])} for b in blocks]})
            if parts[:2] == ["api", "runs"] and len(parts) == 3 and SAFE.match(parts[2]):
                return self.json(run_info(parts[2], commit, gen))
            if parts[0] in ("thumb", "shot") and len(parts) == 3 and SAFE.match(parts[1]) and parts[2].isdigit():
                n = int(parts[2])
                path = (thumbnail(parts[1], n, commit, gen) if parts[0] == "thumb"
                        else schema.shot_path(parts[1], n, commit, gen))
                return self.file(path, "image/jpeg", cache=True)
            if parts[0] == "log" and len(parts) == 3 and SAFE.match(parts[1]):
                name = {"client": "client.log", "server": "server.log", "notes": "client_notes.log"}.get(parts[2])
                path = schema.VERIFY_OUT / parts[1] / name if name else None
                if not path or not path.is_file():
                    return self.send(HTTPStatus.NOT_FOUND, b"no such log", "text/plain")
                data = path.read_bytes()
                whole = "all" in parse_qs(url.query)
                if len(data) > LOG_TAIL and not whole:
                    data = b"[... the first %d bytes; ?all for the whole log]\n" % (len(data) - LOG_TAIL) + data[-LOG_TAIL:]
                return self.send(HTTPStatus.OK, data, "text/plain; charset=utf-8")
            if parts == ["doc"]:
                # The item's `doc` link as text: a path under docs/ (or ../apexsim-content/docs/...).
                rel = parse_qs(url.query).get("p", [""])[0].split("#")[0]
                path = (REPO / "docs" / rel).resolve()
                if path.suffix != ".md" or not (path.is_relative_to(REPO.parent.resolve())):
                    return self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")
                return self.file(path, "text/plain; charset=utf-8")
        except (OSError, ValueError, yaml.YAMLError) as e:
            return self.json({"error": str(e)}, HTTPStatus.INTERNAL_SERVER_ERROR)
        self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")

    def do_POST(self):
        if urlparse(self.path).path != "/api/reviews":
            return self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            with _save_lock:
                f = schema.ItemsFile()
                if f.loaded_digest != body.get("digest"):
                    return self.json({"error": "the items file changed on disk since the page loaded",
                                      "digest": f.loaded_digest}, HTTPStatus.CONFLICT)
                answers = None
                for r in body.get("reviews", []):
                    if "answer" in r:
                        # A sitting answer: the check's answer words in sittings.yml decide the status.
                        if answers is None:
                            answers = {c["item"]: c["answers"] for b in schema.load_sittings() for c in b["checks"]}
                        if r["id"] not in answers:
                            raise ValueError(f"{r['id']} is not a sitting check")
                        status, review = schema.answer_review(answers[r["id"]], r["answer"], r.get("at", ""),
                                                              r.get("note", ""))
                        schema.apply_review(f, r["id"], status, {**review, "flag": r.get("flag", False)})
                    else:
                        schema.apply_review(f, r["id"], r["status"], r["review"])
                errors, _ = schema.validate(f.items(), check_paths=False)
                if errors:
                    return self.json({"error": "; ".join(errors[:5])}, HTTPStatus.BAD_REQUEST)
                new = f.save(expect_digest=body.get("digest"))
            return self.json({"digest": new, "saved": len(body.get("reviews", []))})
        except schema.Changed as e:
            return self.json({"error": "the items file changed on disk", "digest": str(e)}, HTTPStatus.CONFLICT)
        except (KeyError, ValueError) as e:
            return self.json({"error": str(e)}, HTTPStatus.BAD_REQUEST)


def cmd_serve(args) -> int:
    if args.items:
        # Another items file (a scratch copy to try the page on); the closed file beside it.
        schema.ITEMS_YML = Path(args.items).resolve()
        schema.CLOSED_YML = schema.ITEMS_YML.with_name(schema.ITEMS_YML.stem + ".closed.yml")
    host = "0.0.0.0" if args.lan else "127.0.0.1"
    httpd = ThreadingHTTPServer((host, args.port), Handler)
    url = f"http://localhost:{args.port}/"
    print(f"verifier on {url} ({schema.ITEMS_YML}); Ctrl+C stops it")
    if args.lan:
        import socket
        addresses = sorted({a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
                            if not a[4][0].startswith("127.")})
        for address in addresses:
            print(f"  on the LAN: http://{address}:{args.port}/#/sit  (anyone on the network can save reviews)")
    if not args.no_open:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("check", help="validate the items file")
    p.add_argument("--no-paths", action="store_true", help="do not look for the evidence on disk (CI)")
    p.add_argument("--warnings", action="store_true", help="list the warnings (evidence not on disk)")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("add", help="add an item (flags, or `-` and YAML on stdin)")
    p.add_argument("stdin", nargs="?")
    for flag in ("id", "title", "what", "check-by", "doc", "plan", "needs", "created", "commit"):
        p.add_argument(f"--{flag}")
    p.add_argument("--area", choices=schema.AREAS)
    p.add_argument("--kind", choices=schema.KINDS, default="screen")
    p.add_argument("--blocked", action="store_true")
    p.set_defaults(fn=cmd_add)

    p = sub.add_parser("evidence", help="attach a run's shots to items")
    p.add_argument("item", nargs="+")
    p.add_argument("--run", required=True)
    p.add_argument("--shots", help="3,4 (none: the whole run)")
    p.add_argument("--kind", choices=schema.EVIDENCE_KINDS, default="shot")
    p.add_argument("--note")
    p.add_argument("--date")
    p.add_argument("--commit")
    p.set_defaults(fn=cmd_evidence)

    p = sub.add_parser("judge", help="the agent's verdict on an item")
    p.add_argument("item")
    p.add_argument("--says", choices=schema.SAYS, required=True)
    p.add_argument("--text", required=True)
    p.add_argument("--shots")
    p.add_argument("--date")
    p.set_defaults(fn=cmd_judge)

    p = sub.add_parser("reviewed", help="saved reviews the agent has not processed")
    p.add_argument("--all", action="store_true", help="processed ones too")
    p.add_argument("--yaml", action="store_true")
    p.set_defaults(fn=cmd_reviewed)

    p = sub.add_parser("processed", help="mark items' reviews as acted on")
    p.add_argument("item", nargs="+")
    p.set_defaults(fn=cmd_processed)

    p = sub.add_parser("close", help="move items to verification-items.closed.yml")
    p.add_argument("item", nargs="+")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_close)

    p = sub.add_parser("import", help="once: docs/verification.md and sittings.yml into items")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_import)

    p = sub.add_parser("serve", help="the review page")
    p.add_argument("--port", type=int, default=PORT)
    p.add_argument("--no-open", action="store_true")
    p.add_argument("--items", help="another items file, e.g. a scratch copy")
    p.add_argument("--lan", action="store_true", help="listen on every address: sitting mode from a tablet")
    p.set_defaults(fn=cmd_serve)

    args = parser.parse_args(argv)
    try:
        return args.fn(args)
    except KeyError as e:
        print(f"no item {e}")
        return 1
    except (ValueError, schema.Changed) as e:
        print(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
