#!/usr/bin/env python3
"""Build the marketing page (docs/, published by GitHub Pages) from site/.

    python scripts/site/build_site.py              # write docs/index.html and docs/assets
    python scripts/site/build_site.py --check      # CI: exit 1 if the committed page is out of date
    python scripts/site/build_site.py --stale      # what changed in the feature docs since the copy was reviewed
    python scripts/site/build_site.py --reviewed   # record the feature docs as covered by the copy
    python scripts/site/build_site.py --force      # encode every picture again

The page has three kinds of content, kept apart so each can be refreshed
the way it has to be:

  facts   the car cards, the circuit list and every number in the text:
          read from content/ and the server source on each build
          (scripts/site/facts.py). Never typed.
  words   site/copy.yml, written by hand against the feature docs in docs/. The
          build cannot tell whether they still describe the game, so it
          keeps a hash of every section of those documents in
          site/copy.lock.json and `--stale` lists the ones that have
          changed since: the list to rewrite the copy from.
  media   site/media.yml: each picture's source in the repo (the Blender
          car previews, the splash, the screenshots) and how to cut it.
          docs/assets/manifest.json holds the hash of the source each
          output was made from; a source that changes is encoded again.

docs/ is committed, because Pages serves it from the branch. `--check`
renders everything in memory and compares: the text files byte for byte
(carriage returns aside), the pictures by their source hashes, since two
WebP encoders need not agree on the bytes.

Needs Python 3.11+, PyYAML, Jinja2 and Pillow.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

import jinja2
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import facts  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
SITE = REPO / "site"
OUT = REPO / "docs"
ASSETS = OUT / "assets"
MANIFEST = ASSETS / "manifest.json"
LOCK = SITE / "copy.lock.json"

# The documents the copy is written from; `--stale` watches their sections.
# Every doc under docs/ but the unbuilt proposals and the index.
FEATURE_DOCS = sorted(
    p.relative_to(REPO).as_posix()
    for p in (REPO / "docs").rglob("*.md")
    if "proposals" not in p.parts and p.name != "README.md"
)

# The text files the build writes into docs/assets beside the pictures.
TEXT_ASSETS = ["site.css", "site.js", "data.js"]
# WebP quality of the large version a click on a picture opens.
ZOOM_QUALITY = 86

_WORDS_ONES = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
               "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
               "eighteen", "nineteen"]
_WORDS_TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]


class SiteError(Exception):
    """Something in site/ the build cannot use; the message says what."""


def words(value) -> str:
    """14 -> "fourteen"; a number past 99 stays digits."""
    number = int(value)
    if 0 <= number < 20:
        return _WORDS_ONES[number]
    if number < 100:
        tens, ones = divmod(number, 10)
        return _WORDS_TENS[tens] + ("-" + _WORDS_ONES[ones] if ones else "")
    return str(number)


def attr(value) -> str:
    """Copy as an attribute value: tags dropped, quotes escaped."""
    text = re.sub(r"<[^>]+>", " ", str(value))
    text = re.sub(r"\s+", " ", text).strip()
    return text.replace('"', "&quot;")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_yaml(path: Path):
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


# --- media ---------------------------------------------------------------

def image_plan(media: dict, car_list: list[dict]) -> tuple[dict[str, dict], dict[str, str]]:
    """Output name -> {src, params}: every picture the build makes; and
    picture -> the large version of it a click opens (`zoom` in media.yml)."""
    plan: dict[str, dict] = {}
    zooms: dict[str, str] = {}

    def add(out: str, entry: dict, src: str):
        params = {k: entry[k] for k in ("width", "aspect", "focus", "quality") if k in entry}
        if out in plan:
            raise SiteError(f"site/media.yml: two pictures are named {out}")
        plan[out] = {"src": src, "params": params}
        if entry.get("zoom"):
            # The whole frame, uncropped: what the page shows is a cut of it.
            large = out.rsplit(".", 1)[0] + "-zoom.webp"
            plan[large] = {"src": src, "params": {"width": int(entry["zoom"]), "quality": ZOOM_QUALITY}}
            zooms[out] = large

    per_car = media.get("cars")
    if per_car:
        for car in car_list:
            src = per_car["src"].format(folder=car["folder"])
            if (REPO / src).is_file():
                out = per_car["out"].format(folder=car["folder"])
                add(out, per_car, src)
                car["img"] = out
                if out in zooms:
                    car["zoom"] = zooms[out]
    for out, entry in (media.get("images") or {}).items():
        if not (REPO / entry["src"]).is_file():
            raise SiteError(f"site/media.yml: {out}: no such file {entry['src']}")
        add(out, entry, entry["src"])
    return plan, zooms


def encode(src: Path, out: Path, width=None, aspect=None, focus=(0.5, 0.5), quality=82):
    from PIL import Image

    with Image.open(src) as opened:
        image = opened.convert("RGB")
    if aspect:
        w, h = image.size
        if w / h > aspect:
            crop_w, crop_h = round(h * aspect), h
        else:
            crop_w, crop_h = w, round(w / aspect)
        left = min(max(round(focus[0] * w - crop_w / 2), 0), w - crop_w)
        top = min(max(round(focus[1] * h - crop_h / 2), 0), h - crop_h)
        image = image.crop((left, top, left + crop_w, top + crop_h))
    if width and image.width > width:
        image = image.resize((width, round(image.height * width / image.width)), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out, "WEBP", quality=quality, method=6)


def read_manifest() -> dict:
    if not MANIFEST.is_file():
        return {}
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f).get("images", {})


def manifest_for(plan: dict[str, dict]) -> dict:
    return {out: {"src": item["src"], "sha256": sha256(REPO / item["src"]), "params": item["params"]}
            for out, item in sorted(plan.items())}


def manifest_text(images: dict) -> str:
    return json.dumps({"comment": "Written by scripts/site/build_site.py: the source each picture was made from.",
                       "images": images}, indent=2, ensure_ascii=False) + "\n"


# --- words ---------------------------------------------------------------

def render_copy(copy, env: jinja2.Environment, context: dict, where="copy.yml"):
    """Every string of the copy with its {{ figures }} filled in."""
    if isinstance(copy, str):
        try:
            return env.from_string(copy).render(**context)
        except jinja2.TemplateError as error:
            raise SiteError(f"site/{where}: {error}") from error
    if isinstance(copy, list):
        return [render_copy(item, env, context, f"{where}[{i}]") for i, item in enumerate(copy)]
    if isinstance(copy, dict):
        return {key: render_copy(item, env, context, f"{where}.{key}") for key, item in copy.items()}
    return copy


def sections(path: Path) -> dict[str, str]:
    """A markdown file as heading -> hash of the text under it (down to the
    next heading of any level)."""
    out: dict[str, str] = {}
    heading, body = None, []

    def close():
        if heading is not None:
            text = "\n".join(line.rstrip() for line in body).strip()
            out[heading] = hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]

    fenced = False
    for line in path.read_text(encoding="utf-8").replace("\r", "").split("\n"):
        if line.startswith("```"):
            fenced = not fenced
        if not fenced and re.match(r"#{1,4} ", line):
            close()
            heading, body = line.lstrip("#").strip(), []
        else:
            body.append(line)
    close()
    return out


def doc_state() -> dict:
    return {doc: sections(REPO / doc) for doc in FEATURE_DOCS if (REPO / doc).is_file()}


def stale_report() -> list[str]:
    """Sections of the feature docs added, changed or gone since `--reviewed`."""
    if not LOCK.is_file():
        return ["site/copy.lock.json is missing: run --reviewed once the copy matches the docs"]
    with open(LOCK, encoding="utf-8") as f:
        seen = json.load(f).get("docs", {})
    lines = []
    for doc, now in doc_state().items():
        before = seen.get(doc, {})
        for heading, digest in now.items():
            if heading not in before:
                lines.append(f"new      {doc}: {heading}")
            elif before[heading] != digest:
                lines.append(f"changed  {doc}: {heading}")
        for heading in before:
            if heading not in now:
                lines.append(f"removed  {doc}: {heading}")
    return lines


def write_lock():
    text = json.dumps({"comment": "Written by build_site.py --reviewed: the feature docs as site/copy.yml last covered them.",
                       "docs": doc_state()}, indent=1, ensure_ascii=False) + "\n"
    LOCK.write_text(text, encoding="utf-8", newline="\n")


# --- the page ------------------------------------------------------------

def render() -> tuple[dict[str, str], dict[str, dict], list[str]]:
    """The text files of the site (path under docs/ -> content), the picture
    plan and the static files the page needs."""
    data = facts.gather()
    media = load_yaml(SITE / "media.yml")
    plan, zooms = image_plan(media, data["cars"])
    static = list(media.get("static") or [])
    known = set(plan) | set(static)

    by_stem = {t["stem"]: t for t in data["tracks"]}
    by_folder = {c["folder"]: c for c in data["cars"]}

    def name(stem: str) -> str:
        if stem not in by_stem:
            raise SiteError(f"name('{stem}'): no such circuit under content/tracks/default")
        return by_stem[stem]["name"]

    def car(folder: str) -> str:
        if folder not in by_folder:
            raise SiteError(f"car('{folder}'): no such car under content/cars/default")
        return by_folder[folder]["name"]

    env = jinja2.Environment(loader=jinja2.FileSystemLoader(str(SITE)), undefined=jinja2.StrictUndefined,
                             autoescape=False, keep_trailing_newline=True)
    env.filters["words"] = words
    env.filters["attr"] = attr

    raw = load_yaml(SITE / "copy.yml")
    class_titles = {key: value["title"] for key, value in raw["cars"]["classes"].items()}

    def car_class(folder: str) -> str:
        car(folder)
        cls = by_folder[folder]["cls"]
        return class_titles.get(cls, facts.display_class(cls))

    copy = render_copy(raw, env, {"n": data["n"], "name": name, "car": car, "car_class": car_class})

    missing = [c for c in data["classes"] if c not in copy["cars"]["classes"]]
    if missing:
        raise SiteError(f"site/copy.yml: cars.classes has no blurb for {', '.join(missing)}")
    if copy["tracks"]["featured"] not in by_stem:
        raise SiteError(f"site/copy.yml: tracks.featured names no circuit: {copy['tracks']['featured']}")

    hero_loop = []
    for item in media.get("hero_loop") or []:
        if item["file"] not in static:
            raise SiteError(f"site/media.yml: hero_loop names {item['file']}, which is not under `static`")
        hero_loop.append([f"assets/{item['file']}", item["type"]])

    site_data = {
        "cars": data["cars"],
        "tracks": data["tracks"],
        "classes": {key: copy["cars"]["classes"][key] for key in data["classes"]},
        "order": data["classes"],
        "featured": copy["tracks"]["featured"],
        "heroLoop": hero_loop,
    }
    texts = {
        "assets/site.css": (SITE / "site.css").read_text(encoding="utf-8"),
        "assets/site.js": (SITE / "site.js").read_text(encoding="utf-8"),
        "assets/data.js": ("// Generated by scripts/site/build_site.py from content/. Do not edit.\n"
                           "const SITE = " + json.dumps(site_data, ensure_ascii=False, separators=(",", ":")) + ";\n"),
    }

    def asset(file: str) -> str:
        if file not in known:
            raise SiteError(f"the page uses assets/{file}, which site/media.yml does not list")
        return f"assets/{file}"

    def zoom_attr(file: str) -> str:
        """The attributes that make a picture open large on a click (the
        page's script picks up any `img[data-zoom]`), or nothing."""
        if file not in zooms:
            return ""
        return f' data-zoom="assets/{zooms[file]}" tabindex="0"'

    def versioned(file: str) -> str:
        digest = hashlib.sha1(texts[f"assets/{file}"].replace("\r", "").encode("utf-8")).hexdigest()[:8]
        return f"assets/{file}?v={digest}"

    try:
        texts["index.html"] = env.get_template("template.html").render(
            n=data["n"], asset=asset, zoom_attr=zoom_attr, versioned=versioned, **copy)
    except jinja2.TemplateError as error:
        raise SiteError(f"site/template.html: {error}") from error
    return {path: text.replace("\r", "") for path, text in texts.items()}, plan, static


def build(force: bool) -> int:
    texts, plan, static = render()
    ASSETS.mkdir(parents=True, exist_ok=True)

    before = read_manifest()
    after = manifest_for(plan)
    made = 0
    for out, entry in after.items():
        target = ASSETS / out
        if force or not target.is_file() or before.get(out) != entry:
            encode(REPO / entry["src"], target, **{**entry["params"],
                                                    "focus": tuple(entry["params"].get("focus", (0.5, 0.5)))})
            made += 1
            print(f"  picture  assets/{out}  <-  {entry['src']}")
    for out in before:
        if out not in after and (ASSETS / out).is_file():
            (ASSETS / out).unlink()
            print(f"  removed  assets/{out}  (no longer in site/media.yml)")
    texts["assets/manifest.json"] = manifest_text(after)

    written = 0
    for path, text in texts.items():
        target = OUT / path
        if not target.is_file() or target.read_text(encoding="utf-8").replace("\r", "") != text:
            target.write_text(text, encoding="utf-8", newline="\n")
            written += 1
            print(f"  wrote    docs/{path}")

    problems = [f"docs/assets/{file} is missing (site/media.yml lists it under `static`)"
                for file in static if not (ASSETS / file).is_file()]
    known = set(after) | set(static) | set(TEXT_ASSETS) | {"manifest.json"}
    stray = sorted(p.name for p in ASSETS.iterdir() if p.is_file() and p.name not in known)
    for file in stray:
        print(f"  note     docs/assets/{file} is not used by the page")
    for problem in problems:
        print(f"  ERROR    {problem}")

    stale = stale_report()
    size = len(texts["index.html"].encode("utf-8"))
    print(f"docs/index.html: {size / 1024:.0f} KB, {written} text file(s) written, {made} picture(s) encoded, "
          f"{len(after)} pictures in all")
    if stale:
        print(f"The copy may be behind the game: {len(stale)} section(s) of the feature docs changed "
              f"since it was reviewed (--stale lists them).")
    return 1 if problems else 0


def check() -> int:
    texts, plan, static = render()
    after = manifest_for(plan)
    texts["assets/manifest.json"] = manifest_text(after)
    problems = []
    for path, text in texts.items():
        target = OUT / path
        if not target.is_file():
            problems.append(f"docs/{path} is missing")
        elif target.read_text(encoding="utf-8").replace("\r", "") != text:
            problems.append(f"docs/{path} is out of date")
    for out in after:
        if not (ASSETS / out).is_file():
            problems.append(f"docs/assets/{out} is missing")
    for file in static:
        if not (ASSETS / file).is_file():
            problems.append(f"docs/assets/{file} is missing")
    if problems:
        print("The marketing page is behind the content it is generated from:")
        for problem in problems:
            print(f"  {problem}")
        print("Run `python scripts/site/build_site.py` and commit docs/.")
        return 1
    stale = stale_report()
    if stale:
        note = (f"site/copy.yml may be behind the game: {len(stale)} section(s) of the feature docs "
                f"changed since it was reviewed (python scripts/site/build_site.py --stale)")
        print(f"::warning::{note}" if os.environ.get("GITHUB_ACTIONS") else f"note: {note}")
    print("docs/ is up to date.")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Build the marketing page into docs/.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="exit 1 if docs/ is not what a build would write")
    mode.add_argument("--stale", action="store_true", help="list the feature-doc sections changed since --reviewed")
    mode.add_argument("--reviewed", action="store_true", help="record the feature docs as covered by site/copy.yml")
    parser.add_argument("--force", action="store_true", help="encode every picture again")
    args = parser.parse_args(argv)
    try:
        if args.stale:
            lines = stale_report()
            print("\n".join(lines) if lines else "The copy has been reviewed against the feature docs as they are.")
            return 0
        if args.reviewed:
            write_lock()
            print(f"Recorded {sum(len(s) for s in doc_state().values())} sections in site/copy.lock.json.")
            return 0
        return check() if args.check else build(args.force)
    except SiteError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
