"""`verifier.py import`: today's prose backlog into items, once.

docs/verification.md's bullets become items: section 1's run headings give
`check_by` and a first `area`, section 2's bold groups the hardware kinds,
section 3 is the platforms. A bullet's sentences up to the first dated one
are its `what`; the dated rest, and the run paragraphs after a section's
bullets, become `notes`, and the latest of those an agent `verdict` with the
runs it names as evidence. scripts/verify/sittings.yml's checks become items
of kind `sitting`. The result is a first guess: the agent reads it through
and fixes titles and areas by hand, once.
"""

from __future__ import annotations

import re
import subprocess
from datetime import date, datetime
from pathlib import Path

import yaml

from . import schema

DOC = schema.REPO / "docs" / "verification.md"
RUNS_YML = schema.REPO / "scripts" / "verify" / "runs.yml"
SITTINGS_YML = schema.REPO / "scripts" / "verify" / "sittings.yml"
GIT_ROOT = schema.REPO.parent
DOC_PATHS = ["apexsim/docs/verification.md", "docs/verification.md", "docs/VERIFICATION.md", "VERIFICATION.md"]

DATE = re.compile(r"20\d\d-\d\d-\d\d")
TICKED = re.compile(r"`([a-z0-9][a-z0-9-]*[a-z0-9])`")

# Section 1's runs: the area most of its items belong to.
RUN_AREA = {
    "R1": "tyres", "R2": "tyres", "R4": "sky-weather", "R5": "track", "R7": "spectator-replay",
    "R9": "menus", "R10": "sky-weather", "R11": "pit", "R12": "menus", "R13": "race-rules",
    "R14": "menus", "R15": "menus", "R16": "race-rules", "R17": "audio", "R18": "sky-weather",
    "R19": "audio", "R21": "cameras", "R22": "race-rules", "R23": "spectator-replay",
    "R24": "cameras", "R25": "race-rules", "R26": "menus", "R27": "spectator-replay",
    "R28": "cars", "R29": "race-rules",
}
# Section 2's bold groups: (id prefix, area, kind, needs).
GROUPS = [
    ("own liveries", "livery", "network", "hardware", ["second-pc"]),
    ("client prediction", "predict", "network", "hardware", ["wheel"]),
    ("wheel, pedals", "wheel", "ffb-input", "hardware", ["wheel"]),
    ("listen", "listen", "audio", "listen", ["speakers"]),
    ("pad and mouse", "pad", "menus", "hardware", ["pad"]),
    ("racing the ai", "race-ai", "ai", "hardware", ["wheel"]),
    ("an imported car", "import-car", "cars", "hardware", ["wheel"]),
    ("an imported circuit", "import-pit", "pit", "hardware", ["wheel"]),
    ("telemetry output", "telemetry", "platform", "hardware", ["simhub"]),
    ("setup sharing", "setup-share", "menus", "hardware", []),
    ("lap traces", "lap-traces", "hud", "hardware", ["wheel"]),
    ("render scale", "render", "platform", "hardware", ["hdr-display"]),
    ("chat", "chat", "network", "hardware", ["second-pc"]),
    ("last, with the network off", "offline", "menus", "hardware", ["network-off"]),
]
BLOCKED_NEEDS = {"second-pc", "gpu-integrated", "arm-pc", "motec"}

NOT_READABLE = re.compile(
    r"not readable|not yet|never|still to|left:|unseen|is lost|was lost|not seen|"
    r"nobody|blur|to judge|to see|to shoot|only .* have run|not shown|no shot", re.I)
NOT_RIGHT = re.compile(r"\bwrong\b|blow[ns]? .* white|white and a black|follow-ups", re.I)


def _today() -> str:
    return date.today().isoformat()


def _first_added(snippet: str) -> tuple[str, str]:
    """(commit, date) of the first commit whose verification.md had `snippet`."""
    try:
        out = subprocess.run(
            ["git", "log", "--reverse", "--format=%h %ad", "--date=short", "-S", snippet, "--", *DOC_PATHS],
            cwd=GIT_ROOT, capture_output=True, text=True, encoding="utf-8", timeout=60).stdout.split("\n")[0]
        commit, day = out.split()
        return commit, day
    except (ValueError, subprocess.SubprocessError):
        return "", ""


def _sentences(text: str) -> list[str]:
    parts = re.split(r"(?<=[.!?)])\s+(?=[A-Z`(\"])", text)
    return [p for p in parts if p]


def _clean(text: str) -> str:
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)  # links to their text
    return re.sub(r"\s+", " ", text).strip()


def _title(what: str) -> str:
    plain = what.replace("`", "")
    plain = re.sub(r"\s*\([^)]*\)", "", plain)
    cut = re.split(r"[:;.]\s|, (?=whether|and the)", plain, maxsplit=1)[0].strip()
    if len(cut) > 70:
        cut = cut[:70].rsplit(" ", 1)[0] + "..."
    return cut[:1].upper() + cut[1:]


def _slug(text: str, words: int = 4) -> str:
    stop = {"the", "a", "an", "of", "and", "on", "in", "its", "at", "to", "by", "from", "with", "for",
            "s", "as", "it", "is", "or", "one", "whether", "how"}
    tokens = [t for t in re.findall(r"[a-z0-9]+", text.lower().replace("'", "")) if t not in stop]
    return "-".join(tokens[:words]) or "item"


def _blocks(lines: list[str]) -> list[tuple[str, str]]:
    """('bullet'|'para'|'bold', text) in order; bold is a `**Group.**` paragraph."""
    out, cur, kind = [], [], None
    def flush():
        nonlocal cur, kind
        if cur:
            out.append((kind, " ".join(s.strip() for s in cur)))
        cur, kind = [], None
    for line in lines:
        if not line.strip():
            flush()
        elif line.startswith("- "):
            flush()
            kind, cur = "bullet", [line[2:]]
        elif line.startswith("  ") and kind == "bullet":
            cur.append(line)
        else:
            if kind == "bullet":
                flush()
            if kind is None:
                kind = "bold" if line.startswith("**") else "para"
            cur.append(line)
    flush()
    return out


def _known_runs() -> tuple[dict, set]:
    runs = yaml.safe_load(RUNS_YML.read_text(encoding="utf-8"))["runs"]
    on_disk = {p.name for p in schema.VERIFY_OUT.iterdir() if p.is_dir()} if schema.VERIFY_OUT.exists() else set()
    return {r["id"]: r for r in runs}, on_disk


def _run_date(run: str) -> str:
    folder = schema.VERIFY_OUT / run / "shots"
    if folder.exists():
        return datetime.fromtimestamp(folder.stat().st_mtime).date().isoformat()
    return ""


def _split_what(text: str) -> tuple[str, list[str]]:
    """A bullet's check, and its dated sentences onwards as one note."""
    sentences = _sentences(text)
    for k, s in enumerate(sentences):
        if DATE.search(s) or re.match(r"(Run|Seen|Shot) ", s):
            if k == 0:
                # A bullet that opens with its run: the check is its first sentence, the note all of it.
                return sentences[0], [text]
            return " ".join(sentences[:k]), [" ".join(sentences[k:])]
    return text, []


def _note(text: str) -> dict:
    dates = DATE.findall(text)
    return {"date": max(dates) if dates else "", "text": text}


def _says(text: str) -> str:
    if NOT_RIGHT.search(text):
        return "not-right"
    if NOT_READABLE.search(text):
        return "not-readable"
    return "looks-right"


def _evidence(texts: list[str], runs: dict, on_disk: set, fallback_date: str) -> list[dict]:
    out, seen = [], set()
    for text in texts:
        for run in TICKED.findall(text):
            if run in on_disk and run not in seen and (run in runs or (schema.VERIFY_OUT / run / "shots").exists()):
                seen.add(run)
                out.append({"run": run, "date": _run_date(run) or fallback_date, "kind": "shot"})
    return out


def _item(iid, title, area, kind, check_by, what, notes, needs, runs, on_disk, doc=None, plan=None, snippet=None):
    commit, created = _first_added(snippet or what[:50].replace("`", "`"))
    item = {"id": iid, "title": title, "area": area, "kind": kind, "check_by": check_by}
    if created:
        item["created"] = created
    if commit:
        item["commit"] = commit
    if plan:
        item["plan"] = plan
    if doc:
        item["doc"] = doc
    item["what"] = _clean(what)
    status = "blocked" if BLOCKED_NEEDS & set(needs) else "open"
    item["status"] = status
    if needs:
        item["needs"] = list(needs)
    clean_notes = [{"date": n["date"], "text": _clean(n["text"])} if n["date"] else {"text": _clean(n["text"])}
                   for n in notes if n["text"].strip()]
    # A run's preamble ("Seen on ...") is about items already cleared: a note, not a verdict.
    dated = [{"date": n["date"], "text": _clean(n["text"])} for n in notes
             if n["date"] and n["text"].strip() and not n.get("pre")]
    evidence = _evidence([n["text"] for n in dated], runs, on_disk, dated[-1]["date"] if dated else _today())
    if evidence:
        item["evidence"] = evidence
    if dated:
        latest = max(dated, key=lambda n: n["date"])
        item["verdict"] = {"date": latest["date"], "by": "agent", "says": _says(latest["text"]), "text": latest["text"]}
        if status == "open":
            item["status"] = "judged"
    if clean_notes:
        item["notes"] = clean_notes
    return item


def _doc_link(text: str) -> str | None:
    m = re.search(r"\]\(((?:\.\./)*[\w/.-]+\.md(?:#[\w-]+)?)\)", text)
    if not m:
        return None
    link = m.group(1)
    return link.replace("../../apexsim-content/", "../apexsim-content/")


def import_doc() -> list[dict]:
    runs, on_disk = _known_runs()
    lines = DOC.read_text(encoding="utf-8").splitlines()
    items, used = [], set()

    def unique(base):
        iid, n = base, 2
        while iid in used:
            iid, n = f"{base}-{n}", n + 1
        used.add(iid)
        return iid

    # Cut into sections: ("R1", title, lines) for section 1, ("hw", ...) and ("platform", ...).
    sections, cur = [], None
    part = 0
    for line in lines:
        m2 = re.match(r"^## (\d)\. ", line)
        m3 = re.match(r"^### (R\d+)\. (.*)", line)
        if m2:
            part = int(m2.group(1))
            cur = None
            if part in (2, 3):
                cur = ("hw" if part == 2 else "platform", line[6:], [])
                sections.append(cur)
            continue
        if m3 and part == 1:
            cur = (m3.group(1), m3.group(2), [])
            sections.append(cur)
            continue
        if cur is not None:
            cur[2].append(line)

    for key, heading, body in sections:
        blocks = _blocks(body)
        if key.startswith("R"):
            area = RUN_AREA.get(key, "track")
            by_about = [rid for rid, r in runs.items() if str(r.get("about", "")).startswith(key + ".")]
            preamble = [t for k, t in blocks[: next((i for i, (k, _) in enumerate(blocks) if k == "bullet"), len(blocks))]
                        if k != "bullet"]
            first_bullet = next((i for i, (k, _) in enumerate(blocks) if k == "bullet"), len(blocks))
            after = [t for k, t in blocks[first_bullet:] if k != "bullet"]
            run_bullets = [t for k, t in blocks if k == "bullet" and re.match(r"Run 20", t)]
            shared = [{**_note(f"{key} ({heading}): {t}"), "pre": True} for t in preamble] \
                + [_note(t) for t in after + run_bullets]
            if not any(k == "bullet" and not re.match(r"Run 20", t) for k, t in blocks):
                # A run written as prose: its open checks are its "Left:" / "By hand:" sentences.
                prose = " ".join(t for k, t in blocks if k != "bullet")
                left = [s for s in _sentences(_clean(prose)) if re.search(r"\b(Left(, by hand)?|By hand):", s)]
                what = " ".join(re.sub(r"^.*?\b(Left(, by hand)?|By hand):\s*", "", s) for s in left) or _clean(prose)
                notes = [_note(f"{key} ({heading}): {t}") for k, t in blocks if k != "bullet"]
                items.append(_item(unique(f"v-{key.lower()}-{_slug(heading)}"), heading, area, "screen",
                                   by_about[0] if by_about else "human", what, notes, [], runs, on_disk,
                                   doc=_doc_link(prose), snippet=blocks[0][1][:50] if blocks else heading))
                continue
            for kind, text in blocks:
                if kind != "bullet" or re.match(r"Run 20", text):
                    continue
                what, own = _split_what(text)
                named = [r for r in TICKED.findall(text) if r in runs]
                check_by = named[0] if named else (by_about[0] if by_about else "human")
                notes = [n for n in shared if n["date"] or n["text"].startswith(key + " (")] + [_note(t) for t in own]
                title = _title(_clean(what))
                items.append(_item(unique(f"v-{key.lower()}-{_slug(title)}"), title, area, "screen", check_by,
                                   what, notes, [], runs, on_disk, doc=_doc_link(text) or _doc_link(" ".join(preamble)),
                                   snippet=text[:50]))
        else:
            group = ("", "hw", "platform", "platform", [])
            group_text = ""
            pending_para = None
            for kind, text in blocks + [("end", "")]:
                if kind in ("bold", "end"):
                    if pending_para and group[0]:
                        # A group with no bullets is one item: its paragraph.
                        title = re.sub(r"\*\*", "", pending_para).split(".")[0]
                        items.append(_item(unique(f"v-{group[1]}-{_slug(title)}"), title, group[2], group[3], "human",
                                           re.sub(r"^\*\*[^*]+\*\*\s*(\([^)]*\)\s*)?", "", pending_para), [], group[4],
                                           runs, on_disk, doc=_doc_link(pending_para), snippet=pending_para[:50]))
                    pending_para = None
                    if kind == "end":
                        break
                    name = re.match(r"\*\*([^*]+)\*\*", text).group(1).lower()
                    group = next(((name, *g[1:]) for g in GROUPS if name.startswith(g[0])),
                                 (name, _slug(name, 2), "platform", "hardware", []))
                    group_text = text
                    pending_para = text
                elif kind == "bullet":
                    pending_para = None
                    if key == "platform":
                        prefix, area, ikind, needs = "platform", "platform", "platform", _platform_needs(text)
                    else:
                        prefix, area, ikind, needs = group[1], group[2], group[3], group[4]
                    what, own = _split_what(text)
                    title = _title(_clean(what))
                    items.append(_item(unique(f"v-{prefix}-{_slug(title)}"), title, area, ikind,
                                       "human", what, [_note(t) for t in own], needs, runs, on_disk,
                                       doc=_doc_link(text) or _doc_link(group_text), snippet=text[:50]))
    return items


def _platform_needs(text: str) -> list[str]:
    low = text.lower()
    if "integrated gpu" in low:
        return ["gpu-integrated"]
    if "motec" in low:
        return ["motec"]
    if "snapdragon" in low or "windows on arm" in low:
        return ["arm-pc"]
    return ["wheel"]


def import_sittings() -> list[dict]:
    sittings = yaml.safe_load(SITTINGS_YML.read_text(encoding="utf-8"))["sittings"]
    runs, on_disk = _known_runs()
    items = []
    for block in sittings:
        needs = ["pad"] if "pad" in block["id"] else ["wheel"]
        for check in block.get("checks") or []:
            what = check["check"]
            title = _title(what)
            item = _item(f"v-sit-{block['id']}-{_slug(check['id'], 3)}", title, _sit_area(block, what), "sitting",
                         block["id"], what, [], needs, runs, on_disk, doc="wheel-sitting.md",
                         snippet=f"id: {check['id']}")
            item.pop("created", None)
            item.pop("commit", None)
            commit, created = _first_added_in(f"id: {check['id']}, check:", "apexsim/scripts/verify/sittings.yml")
            if created:
                item = {**{k: item[k] for k in ("id", "title", "area", "kind", "check_by")},
                        "created": created, "commit": commit,
                        **{k: v for k, v in item.items() if k not in ("id", "title", "area", "kind", "check_by")}}
            items.append(item)
    return items


def _first_added_in(snippet: str, path: str) -> tuple[str, str]:
    try:
        out = subprocess.run(["git", "log", "--reverse", "--format=%h %ad", "--date=short", "-S", snippet, "--", path,
                              "scripts/verify/sittings.yml"],
                             cwd=GIT_ROOT, capture_output=True, text=True, encoding="utf-8", timeout=60).stdout
        commit, day = out.split("\n")[0].split()
        return commit, day
    except (ValueError, subprocess.SubprocessError):
        return "", ""


def _sit_area(block: dict, what: str) -> str:
    low = what.lower()
    if "sound" in low or "hear" in low or "engine note" in low:
        return "audio"
    if block["id"] in ("i-imported-pit",):
        return "pit"
    return "ffb-input"


def run_import() -> list[dict]:
    return import_doc() + import_sittings()
