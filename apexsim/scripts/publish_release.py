#!/usr/bin/env python3
"""The mechanical half of a GitHub release (the `/release` command runs the rest).

    python scripts/publish_release.py next                 # the version after the latest release (patch; --minor)
    python scripts/publish_release.py bump 0.4.5           # ProjectVersion in DefaultGame.ini, committed
    python scripts/publish_release.py since                # material for the notes: the latest release, the
                                                           # features-by-date days and commits since it
    python scripts/publish_release.py check 0.4.5          # everything publish needs, nothing changed
    python scripts/publish_release.py publish 0.4.5        # push main, tag v0.4.5, create the release
    python scripts/publish_release.py publish 0.4.5 --draft

Run from the code root (apexsim/). The build itself is
`scripts/build_release.ps1` (docs/building.md, Release package); its output in
artifacts/release/ is what `check` and `publish` read:

    ApexSim-<v>-Win64/release.json   its version and commit must be <v> and HEAD
    apexsim-<v>.zip                  uploaded
    apexsim-content-<v>.acp          uploaded (absent with -NoContentPack / -AllContent)
    RELEASE_NOTES-<v>.md             the release's body, also uploaded

`publish` refuses unless `check` passes; it never builds, never edits the
notes and never moves an existing tag.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path

CODE = Path(__file__).resolve().parent.parent
REPO = CODE.parent
INI = CODE / "game-unreal" / "Config" / "DefaultGame.ini"
ARTIFACTS = CODE / "artifacts" / "release"
FEATURES = CODE / "docs" / "features-by-date.md"
REMOTE = "origin"
BRANCH = "main"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


def git(*args: str, check: bool = True) -> str:
    out = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)
    if check and out.returncode != 0:
        raise SystemExit(f"git {' '.join(args)}: {out.stderr.strip()}")
    return out.stdout.strip()


def gh(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    out = subprocess.run(["gh", *args], cwd=REPO, capture_output=True, text=True)
    if check and out.returncode != 0:
        raise SystemExit(f"gh {' '.join(args)}: {out.stderr.strip()}")
    return out


def ini_version() -> str:
    m = re.search(r"^ProjectVersion=(.+)$", INI.read_text(encoding="utf-8"), re.M)
    return m.group(1).strip() if m else ""


def last_tag() -> str:
    """The latest GitHub release's tag: a stray local tag (a v4.0.1 on a
    site commit) is not a release to count from."""
    out = gh("release", "view", "--json", "tagName", check=False)
    if out.returncode == 0:
        return json.loads(out.stdout)["tagName"]
    tags = git("tag", "--list", "v*", "--sort=-v:refname").splitlines()
    return tags[0] if tags else ""


def files_for(version: str) -> dict[str, Path]:
    return {
        "folder": ARTIFACTS / f"ApexSim-{version}-Win64",
        "zip": ARTIFACTS / f"apexsim-{version}.zip",
        "pack": ARTIFACTS / f"apexsim-content-{version}.acp",
        "notes": ARTIFACTS / f"RELEASE_NOTES-{version}.md",
    }


def cmd_next(args) -> int:
    tag = last_tag().lstrip("v") or ini_version()
    major, minor, patch = (int(x) for x in tag.split("."))
    print(f"{major}.{minor + 1}.0" if args.minor else f"{major}.{minor}.{patch + 1}")
    return 0


def cmd_bump(args) -> int:
    v = args.version
    if not VERSION_RE.match(v):
        raise SystemExit(f"{v}: not a version (x.y.z)")
    if git("status", "--porcelain", "--", str(INI)):
        raise SystemExit(f"{INI}: has edits of its own; commit or drop them first")
    text = INI.read_text(encoding="utf-8")
    if ini_version() == v:
        print(f"ProjectVersion is {v} already")
        return 0
    INI.write_bytes(re.sub(r"^ProjectVersion=.*$", f"ProjectVersion={v}", text, flags=re.M).encode("utf-8"))
    git("add", str(INI))
    git("commit", "-q", "-m", f"Release: update version to {v}", "--", str(INI))
    print(f"ProjectVersion {v}, committed {git('rev-parse', '--short', 'HEAD')}")
    return 0


def cmd_since(args) -> int:
    tag = args.tag or last_tag()
    if not tag:
        raise SystemExit("no v* tag to start from")
    when = git("log", "-1", "--format=%cs", tag)
    print(f"# Since {tag} ({when}, {git('rev-parse', '--short', tag)})\n")
    # The features-by-date days after the tag's: the notes' main source.
    text = FEATURES.read_text(encoding="utf-8")
    days = re.split(r"^(?=## \d{4}-\d{2}-\d{2})", text, flags=re.M)
    picked = [d for d in days if d.startswith("## ") and d[3:13] >= when]
    print("## From docs/features-by-date.md\n")
    print("".join(picked).strip() or "(no days after the tag: add them first)")
    print(f"\n## Commits {tag}..HEAD\n")
    print(git("log", "--no-merges", "--format=%h %s", f"{tag}..HEAD"))
    return 0


def problems(version: str) -> list[str]:
    out: list[str] = []
    if not VERSION_RE.match(version):
        return [f"{version}: not a version (x.y.z)"]
    if git("rev-parse", "--abbrev-ref", "HEAD") != BRANCH:
        out.append(f"not on {BRANCH}")
    dirty = [l for l in git("status", "--porcelain", "--untracked-files=no").splitlines() if l.strip()]
    if dirty:
        out.append("uncommitted changes to tracked files:\n    " + "\n    ".join(dirty[:10]))
    if ini_version() != version:
        out.append(f"DefaultGame.ini says {ini_version()}, not {version} (publish_release.py bump {version})")
    tag = f"v{version}"
    if git("tag", "--list", tag):
        out.append(f"tag {tag} exists already")
    if git("ls-remote", "--tags", REMOTE, tag):
        out.append(f"tag {tag} exists on {REMOTE}")
    if gh("auth", "status", check=False).returncode != 0:
        out.append("gh is not logged in (gh auth login)")
    if gh("release", "view", tag, check=False).returncode == 0:
        out.append(f"a GitHub release {tag} exists already")
    f = files_for(version)
    meta = f["folder"] / "release.json"
    if not meta.is_file():
        out.append(f"no {meta.relative_to(CODE)}: run scripts/build_release.ps1")
    else:
        info = json.loads(meta.read_text(encoding="utf-8-sig"))
        if info.get("version") != version:
            out.append(f"release.json is version {info.get('version')}")
        head = git("rev-parse", "HEAD")
        built = str(info.get("commit", ""))
        if not built or not head.startswith(built):
            between = git("log", "--format=%h %s", f"{built}..HEAD", check=False) if built else ""
            out.append(
                f"the package was built at {built or '?'}, HEAD is {head[:8]}"
                + (f"; commits since:\n    " + "\n    ".join(between.splitlines()[:10]) if between else "")
            )
        pack_info = (info.get("contents") or {}).get("pack")
        if pack_info and not f["pack"].is_file():
            out.append(f"release.json names a pack, but {f['pack'].name} is missing")
    if not f["zip"].is_file():
        out.append(f"no {f['zip'].name}")
    if not f["notes"].is_file():
        out.append(f"no {f['notes'].name}: write the notes (the /release command)")
    elif f"ApexSim {version}" not in f["notes"].read_text(encoding="utf-8"):
        out.append(f"{f['notes'].name} has no '# ApexSim {version}' heading")
    for path in (f["zip"], f["pack"]):
        if path.is_file() and path.stat().st_size >= 2 * 1024**3:
            out.append(f"{path.name} is 2 GiB or more: GitHub refuses release assets that large")
    return out


def cmd_check(args) -> int:
    found = problems(args.version)
    for p in found:
        print(f"- {p}")
    if found:
        return 1
    f = files_for(args.version)
    for k in ("zip", "pack", "notes"):
        if f[k].is_file():
            print(f"ok  {f[k].name}  {f[k].stat().st_size / 1e6:,.1f} MB")
    print(f"ready to publish v{args.version} at {git('rev-parse', '--short', 'HEAD')}")
    return 0


def cmd_publish(args) -> int:
    v = args.version
    found = problems(v)
    if found:
        for p in found:
            print(f"- {p}")
        raise SystemExit("not published")
    tag = f"v{v}"
    f = files_for(v)
    print(f"pushing {BRANCH} to {REMOTE}")
    git("push", REMOTE, BRANCH)
    git("tag", "-a", tag, "-m", f"ApexSim {v}")
    git("push", REMOTE, tag)
    assets = [str(p) for p in (f["zip"], f["pack"], f["notes"]) if p.is_file()]
    create = ["release", "create", tag, *assets, "--title", tag, "--notes-file", str(f["notes"]),
              "--verify-tag", "--latest"]
    if args.draft:
        create.append("--draft")
    print(f"creating the release {tag} with {len(assets)} file(s); the upload takes a while")
    out = gh(*create)
    print(out.stdout.strip())
    view = json.loads(gh("release", "view", tag, "--json", "assets,url,isDraft").stdout)
    sizes = {a["name"]: a["size"] for a in view["assets"]}
    missing = [Path(p).name for p in assets if sizes.get(Path(p).name) != Path(p).stat().st_size]
    if missing:
        raise SystemExit(f"uploaded, but these do not match their local size: {missing}")
    print(f"published {view['url']}" + (" (draft)" if view["isDraft"] else "")
          + f" at {dt.datetime.now():%Y-%m-%d %H:%M}")
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("next")
    p.add_argument("--minor", action="store_true", help="the next minor version, not the next patch")
    p.set_defaults(fn=cmd_next)
    p = sub.add_parser("bump")
    p.add_argument("version")
    p.set_defaults(fn=cmd_bump)
    p = sub.add_parser("since")
    p.add_argument("--tag", help="start from this tag instead of the latest release's")
    p.set_defaults(fn=cmd_since)
    p = sub.add_parser("check")
    p.add_argument("version")
    p.set_defaults(fn=cmd_check)
    p = sub.add_parser("publish")
    p.add_argument("version")
    p.add_argument("--draft", action="store_true", help="create it as a draft release")
    p.set_defaults(fn=cmd_publish)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
