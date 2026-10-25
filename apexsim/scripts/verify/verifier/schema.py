"""The verification backlog's model: docs/verification/verification-items.yml.

The file is a YAML list of items (docs/verifier.md has the schema). Two
writers share it, never at once: the agent (the CLI, or by hand) owns every
key but `status` and `review`, the review page owns those two. Every write
goes through `ItemsFile`, which edits one top-level key of one item as text,
so the rest of the file (its order, its comments, the other keys' layout)
stays byte for byte as it was.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
from datetime import date as _date
from datetime import datetime
from functools import lru_cache
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[3]
ITEMS_YML = REPO / "docs" / "verification" / "verification-items.yml"
CLOSED_YML = REPO / "docs" / "verification" / "verification-items.closed.yml"
VERIFY_OUT = REPO / "out" / "verify"
SIT_OUT = REPO / "out" / "sit"
SITTINGS_YML = REPO / "scripts" / "verify" / "sittings.yml"
# A sitting check's answer words when sittings.yml names none; the first is the confirming one.
SIT_ANSWERS = ["OK", "weak", "strong", "late", "wrong", "not felt"]
# A run's earlier shots kept under out/verify/<run>/history/ for before and after.
HISTORY_KEEP = 3

AREAS = [
    "cars", "tyres", "track", "sky-weather", "hud", "menus", "cameras", "audio",
    "ffb-input", "ai", "race-rules", "pit", "spectator-replay", "network", "platform",
]
KINDS = ["screen", "sitting", "hardware", "listen", "platform"]
STATUSES = ["open", "judged", "confirmed", "not-right", "cant-tell", "later", "blocked"]
# What a review sets; the rest are the agent's.
REVIEWED = ["confirmed", "not-right", "cant-tell", "later"]
EVIDENCE_KINDS = ["shot", "sheet", "marks", "log", "replay", "video"]
SAYS = ["looks-right", "not-right", "not-readable"]

KEY_ORDER = [
    "id", "title", "area", "kind", "check_by", "created", "commit", "plan", "doc",
    "what", "status", "needs", "evidence", "verdict", "notes", "review",
]
REQUIRED = ["id", "title", "area", "kind", "check_by", "status", "what"]
ID_RE = re.compile(r"^v-[a-z0-9]+(-[a-z0-9]+)*$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}([T ]\d{2}:\d{2}(:\d{2})?)?$")
# Long prose keys are written folded.
PROSE = {"what", "text", "comment", "note", "title"}

HEADER = """\
# The verification backlog (docs/verification.md): every built feature not yet
# seen in the game, heard, or felt on hardware, one item each. The schema and
# the workflow are in docs/verifier.md; `python scripts/verify/verifier.py
# check` validates it. The agent owns every key but `status` and `review`;
# the review page (`verifier.py serve`) writes those two and nothing else.
"""


# --- reading ------------------------------------------------------------------------

class _Loader(yaml.SafeLoader):
    """Dates and times stay the strings they were written as."""


def _timestamp_as_text(loader, node):
    return loader.construct_scalar(node)


_Loader.add_constructor("tag:yaml.org,2002:timestamp", _timestamp_as_text)


def parse(text: str):
    return yaml.load(text, Loader=_Loader)


# --- writing ------------------------------------------------------------------------

class _Dumper(yaml.SafeDumper):
    pass


def _str(dumper, value: str):
    if DATE_RE.match(value):
        # Written plain so it reads as a date; read back as text by _Loader.
        return dumper.represent_scalar("tag:yaml.org,2002:timestamp", value)
    if "\n" in value:
        return dumper.represent_scalar("tag:yaml.org,2002:str", value, style="|")
    return dumper.represent_scalar("tag:yaml.org,2002:str", value)


def _list(dumper, value: list):
    def short(v):
        return not isinstance(v, (list, dict)) and len(str(v)) <= 40
    flow = all(short(v) for v in value) or all(
        isinstance(v, dict) and len(v) <= 3 and all(short(x) for x in v.values()) for v in value)
    return dumper.represent_sequence("tag:yaml.org,2002:seq", value, flow_style=flow and len(value) > 0)


class Folded(str):
    pass


def _folded(dumper, value):
    return dumper.represent_scalar("tag:yaml.org,2002:str", str(value), style=">")


_Dumper.add_representer(str, _str)
_Dumper.add_representer(list, _list)
_Dumper.add_representer(Folded, _folded)


def _fold(key, value):
    """Long prose under PROSE keys as `>-` blocks, recursively."""
    if isinstance(value, dict):
        return {k: _fold(k, v) for k, v in value.items()}
    if isinstance(value, list):
        return [_fold(key, v) for v in value]
    if isinstance(value, str) and key in PROSE and len(value) > 60 and "\n" not in value:
        return Folded(value)
    return value


def dump_key(key: str, value, first: bool = False) -> str:
    """One top-level key of an item as the text it is written as."""
    text = yaml.dump({key: _fold(key, value)}, Dumper=_Dumper, sort_keys=False,
                     allow_unicode=True, width=72, indent=2, default_flow_style=False)
    lines = text.rstrip("\n").split("\n")
    out = []
    for i, line in enumerate(lines):
        out.append(("- " if first and i == 0 else "  ") + line)
    return "\n".join(out) + "\n"


def dump_item(item: dict) -> str:
    keys = [k for k in KEY_ORDER if k in item] + [k for k in item if k not in KEY_ORDER]
    return "".join(dump_key(k, item[k], first=(i == 0)) for i, k in enumerate(keys))


# --- the file -----------------------------------------------------------------------

_ITEM_START = re.compile(r"^- ")
_KEY_LINE = re.compile(r"^(?:- |  )([A-Za-z_][A-Za-z0-9_]*):(?:\s|$)")


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


class ItemsFile:
    """The items file as text blocks, one per item, edited key by key."""

    def __init__(self, path: Path | None = None):
        self.path = path = path or ITEMS_YML
        text = path.read_text(encoding="utf-8") if path.exists() else HEADER
        self.loaded_digest = digest(text)
        self._split(text)

    def _split(self, text: str):
        lines = text.splitlines(keepends=True)
        self.head: list[str] = []
        self.blocks: list[list[str]] = []
        for line in lines:
            if _ITEM_START.match(line):
                self.blocks.append([line])
            elif self.blocks:
                self.blocks[-1].append(line)
            else:
                self.head.append(line)

    def text(self) -> str:
        return "".join(self.head) + "".join("".join(b) for b in self.blocks)

    def items(self) -> list[dict]:
        out = []
        for block in self.blocks:
            parsed = parse("".join(block))
            if isinstance(parsed, list) and parsed and isinstance(parsed[0], dict):
                out.append(parsed[0])
            else:
                raise ValueError(f"not an item: {block[0].strip()}")
        return out

    def index(self, item_id: str) -> int:
        for i, block in enumerate(self.blocks):
            m = re.match(r"^- id:\s*(\S+)", block[0])
            if m and m.group(1).strip("'\"") == item_id:
                return i
        raise KeyError(item_id)

    def get(self, item_id: str) -> dict:
        return parse("".join(self.blocks[self.index(item_id)]))[0]

    def add(self, item: dict) -> None:
        if any(self._id(b) == item["id"] for b in self.blocks):
            raise ValueError(f"{item['id']} exists")
        text = dump_item(item)
        if self.blocks and not "".join(self.blocks[-1]).endswith("\n\n"):
            self.blocks[-1].append("\n")
        elif not self.blocks and self.head and self.head[-1].strip():
            self.head.append("\n")
        self.blocks.append(text.splitlines(keepends=True) + ["\n"])

    def remove(self, item_id: str) -> str:
        block = self.blocks.pop(self.index(item_id))
        return "".join(block)

    @staticmethod
    def _id(block) -> str:
        m = re.match(r"^- id:\s*(\S+)", block[0])
        return m.group(1).strip("'\"") if m else ""

    def set(self, item_id: str, key: str, value) -> None:
        """Replace one top-level key (None removes it), in place or at the end."""
        i = self.index(item_id)
        block = self.blocks[i]
        spans = self._spans(block)
        new = [] if value is None else dump_key(key, value).splitlines(keepends=True)
        if key in spans:
            start, end = spans[key]
            block[start:end] = new
        elif new:
            # Before the next key in schema order that is present, else at the end.
            later = [spans[k][0] for k in KEY_ORDER[KEY_ORDER.index(key) + 1:] if k in spans] \
                if key in KEY_ORDER else []
            at = min(later) if later else self._content_end(block)
            block[at:at] = new

    @staticmethod
    def _content_end(block) -> int:
        # Trailing blank lines, and comments heading the next item, are not this item's.
        end = len(block)
        while end > 1 and (not block[end - 1].strip() or block[end - 1].lstrip().startswith("#")):
            end -= 1
        return end

    def _spans(self, block) -> dict[str, tuple[int, int]]:
        starts = []
        for n, line in enumerate(block):
            m = _KEY_LINE.match(line)
            if m:
                starts.append((m.group(1), n))
        spans = {}
        end_all = self._content_end(block)
        for k, (key, start) in enumerate(starts):
            end = starts[k + 1][1] if k + 1 < len(starts) else end_all
            # Comment lines just before the next key belong to it.
            while end - 1 > start and block[end - 1].lstrip().startswith("#"):
                end -= 1
            spans[key] = (start, end)
        return spans

    def save(self, expect_digest: str | None = None) -> str:
        """Write atomically; refuses when the file changed since `expect_digest`."""
        if expect_digest is not None and self.path.exists():
            on_disk = digest(self.path.read_text(encoding="utf-8"))
            if on_disk != expect_digest:
                raise Changed(on_disk)
        text = self.text()
        parse(text)  # never write a file that does not read back
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".yml.tmp")
        tmp.write_text(text, encoding="utf-8", newline="\n")
        os.replace(tmp, self.path)
        self.loaded_digest = digest(text)
        return self.loaded_digest


class Changed(Exception):
    """The file on disk is not the one that was loaded."""


# --- rules --------------------------------------------------------------------------

def review_problem(status: str, review: dict) -> str | None:
    """Why a review may not be held, or None. The page and the server both ask."""
    if status not in REVIEWED:
        return f"a review sets one of {', '.join(REVIEWED)}"
    comment = (review.get("comment") or "").strip()
    score = review.get("score")
    if score is not None and (not isinstance(score, int) or not 0 <= score <= 5):
        return "score is 1-5 (0 for none)"
    if status in ("not-right", "cant-tell") and not comment:
        return f"{status} needs a comment"
    if status == "confirmed" and score is not None and 0 < score < 3:
        return "confirmed under 3 is a not-right"
    return None


def answer_review(answers: list[str], word: str, at: str, note: str = "") -> tuple[str, dict]:
    """A sitting answer as a review: the block's first answer word confirms, any
    other is a not-right with the word (and the note) as its comment."""
    answers = answers or SIT_ANSWERS
    if word not in answers:
        raise ValueError(f"{word!r} is not one of {', '.join(answers)}")
    note = note.strip()
    if word == answers[0]:
        return "confirmed", {"at": at, "answer": word, **({"comment": note} if note else {})}
    return "not-right", {"at": at, "answer": word, "comment": f"{word}: {note}" if note else word}


def latest_evidence(item: dict) -> dict | None:
    ev = item.get("evidence") or []
    return max(ev, key=lambda e: str(e.get("date", ""))) if ev else None


@lru_cache(maxsize=4096)
def git_is_ancestor(older: str, newer: str) -> bool | None:
    """Whether commit `older` is in `newer`'s history; None when git does not know one of them."""
    try:
        result = subprocess.run(["git", "merge-base", "--is-ancestor", older, newer], cwd=REPO,
                                capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return {0: True, 1: False}.get(result.returncode)


def stale(item: dict, is_ancestor=git_is_ancestor) -> str | None:
    """The run to repeat when the item's newest evidence is older than its own
    `commit` (the feature changed since it was shot), else None. Without
    commits to compare, the evidence's date against `created`."""
    ev = latest_evidence(item)
    if not ev:
        return None
    ours, theirs = str(item.get("commit") or ""), str(ev.get("commit") or "")
    if ours and theirs and ours != theirs:
        known = is_ancestor(ours, theirs)
        if known is not None:
            return None if known else ev["run"]
    if ours and theirs == ours:
        return None
    return ev["run"] if str(ev.get("date", "")) < str(item.get("created") or "")[:10] else None


def validate(items: list[dict], closed: list[dict] = (), check_paths: bool = True) -> tuple[list[str], list[str]]:
    """(errors, warnings) for the items file."""
    errors, warnings = [], []
    seen = {i.get("id") for i in closed}
    for item in items:
        iid = item.get("id", "?")
        def err(msg):
            errors.append(f"{iid}: {msg}")
        for key in REQUIRED:
            if not item.get(key):
                err(f"no {key}")
        for key in item:
            if key not in KEY_ORDER:
                err(f"unknown key {key}")
        if not ID_RE.match(str(iid)):
            err("id is not v-<run or area>-<slug>")
        if iid in seen:
            err("id used twice (or by a closed item)")
        seen.add(iid)
        if item.get("area") not in AREAS:
            err(f"area {item.get('area')!r} is not one of {', '.join(AREAS)}")
        if item.get("kind") not in KINDS:
            err(f"kind {item.get('kind')!r} is not one of {', '.join(KINDS)}")
        status = item.get("status")
        if status not in STATUSES:
            err(f"status {status!r} is not one of {', '.join(STATUSES)}")
        for key in ("created",):
            if item.get(key) and not DATE_RE.match(str(item[key])):
                err(f"{key} is not a date")
        needs = item.get("needs")
        if needs is not None and (not isinstance(needs, list) or not all(isinstance(n, str) for n in needs)):
            err("needs is a list of tags")
        for ev in item.get("evidence") or []:
            if not ev.get("run"):
                err("evidence without a run")
                continue
            if ev.get("kind", "shot") not in EVIDENCE_KINDS:
                err(f"evidence kind {ev.get('kind')!r}")
            shots = ev.get("shots") or []
            if not all(isinstance(s, int) for s in shots):
                err(f"evidence {ev['run']}: shots are numbers")
            if check_paths:
                folder = evidence_folder(ev)
                if not folder.exists():
                    warnings.append(f"{iid}: evidence {ev['run']}: {folder.relative_to(REPO)} not on disk")
                else:
                    for s in shots:
                        if not shot_path(ev["run"], s).exists():
                            warnings.append(f"{iid}: evidence {ev['run']}: shot {s} not on disk")
        verdict = item.get("verdict")
        if verdict:
            if verdict.get("says") not in SAYS:
                err(f"verdict says {verdict.get('says')!r}, not one of {', '.join(SAYS)}")
            if not verdict.get("text"):
                err("verdict without text")
        if status == "judged" and not verdict:
            err("judged without a verdict")
        review = item.get("review")
        if status in REVIEWED and not review:
            err(f"{status} without a review")
        if review:
            if status not in REVIEWED:
                err(f"a review on a {status} item")
            else:
                problem = review_problem(status, review)
                if problem:
                    err(problem)
    return errors, warnings


# --- evidence on disk ---------------------------------------------------------------

def evidence_folder(ev: dict) -> Path:
    if ev.get("kind") == "marks":
        return SIT_OUT / str(ev.get("date", "")) / ev["run"]
    return shots_folder(ev["run"], ev.get("commit"))


def run_stamp(folder: Path) -> dict:
    """run.json: the commit and date the shots in the folder were taken at."""
    try:
        return json.loads((folder / "run.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def shots_folder(run: str, commit: str | None = None) -> Path:
    """The folder holding a run's shots taken at `commit`: the run's own when
    its latest shots are that commit's (or no commit is asked), else the
    newest kept generation of it under history/."""
    folder = VERIFY_OUT / run
    if commit and run_stamp(folder).get("commit") not in (None, commit):
        kept = sorted((folder / "history").glob(f"*-{commit}"), reverse=True)
        if kept:
            return kept[0]
    return folder


def history(run: str) -> list[dict]:
    """The run's shot generations, newest first: {commit, date, current}."""
    folder = VERIFY_OUT / run
    out = []
    if (folder / "shots").is_dir():
        stamp = run_stamp(folder)
        out.append({"commit": stamp.get("commit", ""), "date": stamp.get("date", ""), "current": True})
    for kept in sorted((folder / "history").glob("*-*"), reverse=True):
        stamp = run_stamp(kept)
        out.append({"commit": stamp.get("commit", ""), "date": stamp.get("date", ""), "current": False})
    return out


def keep_previous(work: Path) -> Path | None:
    """Before a run shoots again: its last shots move to history/<date>-<commit>/
    (the newest HISTORY_KEEP are kept), so before and after can be compared."""
    shots = work / "shots"
    if not any(shots.glob("*.jpg")):
        return None
    stamp = run_stamp(work)
    when = stamp.get("date") or datetime.fromtimestamp(shots.stat().st_mtime).strftime("%Y-%m-%d")
    stamp = {"commit": stamp.get("commit") or "unknown", "date": when}
    target = work / "history" / f"{when.replace(':', '').replace(' ', 'T')}-{stamp['commit']}"
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True)
    shutil.move(str(shots), str(target / "shots"))
    for name in ("shots.md",):
        if (work / name).is_file():
            shutil.move(str(work / name), str(target / name))
    (target / "run.json").write_text(json.dumps(stamp), encoding="utf-8")
    for old in sorted((work / "history").glob("*-*"), reverse=True)[HISTORY_KEEP:]:
        shutil.rmtree(old, ignore_errors=True)
    return target


def stamp_run(work: Path, commit: str, when: str | None = None) -> None:
    (work / "run.json").write_text(json.dumps({"commit": commit, "date": when or _date.today().isoformat()}),
                                   encoding="utf-8")


def shot_path(run: str, n: int, commit: str | None = None) -> Path:
    return shots_folder(run, commit) / "shots" / f"{n:02d}.jpg"


_SHOT_LINE = re.compile(r"^\s*(\d+)\.\s+([\d.]+) s\s+(.*)$")


def run_shots(run: str, commit: str | None = None) -> list[dict]:
    """A run's shots as its shots.md lists them, and any jpg it does not."""
    folder = shots_folder(run, commit)
    shots: dict[int, dict] = {}
    md = folder / "shots.md"
    if md.exists():
        current = None
        for line in md.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _SHOT_LINE.match(line)
            if m:
                current = {"n": int(m.group(1)), "at": float(m.group(2)), "label": m.group(3).strip(), "checks": ""}
                shots[current["n"]] = current
            elif current and line.strip().startswith("checks:"):
                current["checks"] = line.strip()[len("checks:"):].strip()
    for jpg in sorted((folder / "shots").glob("*.jpg")):
        if jpg.stem.isdigit():
            shots.setdefault(int(jpg.stem), {"n": int(jpg.stem), "at": None, "label": "", "checks": ""})
    for n, shot in shots.items():
        shot["file"] = shot_path(run, n).exists()
    return [shots[n] for n in sorted(shots)]


# --- the changes each writer makes --------------------------------------------------

def add_evidence(f: ItemsFile, item_id: str, ev: dict) -> dict:
    """Append (or, for the same run on the same day, replace) one piece of evidence.

    A reviewed item that gets new evidence goes back to `judged` (to `open`
    without a verdict) and keeps its old review under `notes`."""
    item = f.get(item_id)
    evidence = [e for e in item.get("evidence") or []
                if not (e.get("run") == ev["run"] and str(e.get("date")) == str(ev.get("date")))]
    evidence.append(ev)
    f.set(item_id, "evidence", evidence)
    review = item.get("review")
    if review and item.get("status") in REVIEWED:
        note = f"Review ({item['status']}" + (f", score {review['score']}" if review.get("score") else "") + ")"
        if review.get("comment"):
            note += f": {review['comment']}"
        notes = list(item.get("notes") or []) + [{"date": str(review.get("at", ""))[:10], "text": note}]
        f.set(item_id, "notes", notes)
        f.set(item_id, "review", None)
        f.set(item_id, "status", "judged" if item.get("verdict") else "open")
    return f.get(item_id)


def set_verdict(f: ItemsFile, item_id: str, verdict: dict) -> dict:
    item = f.get(item_id)
    f.set(item_id, "verdict", verdict)
    if item.get("status") in ("open", "judged", "blocked"):
        f.set(item_id, "status", "judged")
    return f.get(item_id)


def apply_review(f: ItemsFile, item_id: str, status: str, review: dict) -> None:
    """The page's half: `status` and `review`, nothing else."""
    problem = review_problem(status, review)
    if problem:
        raise ValueError(f"{item_id}: {problem}")
    clean = {k: review[k] for k in ("at", "answer", "score", "comment", "shots", "flag", "processed") if k in review}
    if not clean.get("answer"):
        clean.pop("answer", None)
    if not clean.get("score"):
        clean.pop("score", None)
    if not (clean.get("comment") or "").strip():
        clean.pop("comment", None)
    if not clean.get("shots"):
        clean.pop("shots", None)
    if not clean.get("flag"):
        clean.pop("flag", None)
    clean["processed"] = False
    f.set(item_id, "status", status)
    f.set(item_id, "review", clean)


def load_sittings() -> list[dict]:
    """sittings.yml's blocks, each check's answer words filled in."""
    blocks = yaml.safe_load(SITTINGS_YML.read_text(encoding="utf-8"))["sittings"] if SITTINGS_YML.exists() else []
    for block in blocks:
        for check in block.get("checks") or []:
            check["answers"] = [str(a) for a in check.get("answers") or SIT_ANSWERS]
            check["item"] = f"v-sit-{block['id']}-{check['id']}"
    return blocks


def latest_marks(block: str) -> dict | None:
    """The newest sitting folder's marks.md for a block: its date and its MARK and force lines."""
    found = sorted(SIT_OUT.glob(f"*/{block}/marks.md")) if SIT_OUT.is_dir() else []
    if not found:
        return None
    text = found[-1].read_text(encoding="utf-8", errors="replace")
    lines = [m.group(1) for m in re.finditer(r"^- `(.*)`$", text, re.M)]
    return {"date": found[-1].parent.parent.name, "lines": lines}


def register_run(run: str, shots: list[dict], commit: str, when: str | None = None) -> list[str]:
    """run_checks.py, at the end of a run: each shot's `item:` ids get the run as evidence."""
    by_item: dict[str, list[int]] = {}
    for n, shot in enumerate(shots, 1):
        ids = shot.get("item") or []
        for item_id in [ids] if isinstance(ids, str) else ids:
            by_item.setdefault(item_id, []).append(n)
    if not by_item:
        return []
    f = ItemsFile()
    known = {i["id"] for i in f.items()}
    done = []
    for item_id, numbers in by_item.items():
        if item_id not in known:
            done.append(f"{item_id}: no such item")
            continue
        add_evidence(f, item_id, {"run": run, "date": when or _date.today().isoformat(), "commit": commit,
                                  "shots": numbers, "kind": "shot"})
        done.append(f"{item_id}: shots {', '.join(map(str, numbers))}")
    f.save()
    return done
