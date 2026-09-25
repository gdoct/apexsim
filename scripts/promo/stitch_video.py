#!/usr/bin/env python3
"""Cut the rendered clips into the promo video.

Reads the `edit` section of `scripts/promo/shots.yml` and the clips
`make_clips.py` encoded (`out/promo/clips/manifest.json`), and builds one
ffmpeg pass: a title card, every shot in `edit.sequence` with its
transition, a light grade, a lower-third caption per shot, an end card, and
the music (yours: `edit.music`) trimmed and faded to the cut.

The cards and captions are drawn with Pillow and overlaid as images, so any
ffmpeg build works (drawtext is not in every one).

    python scripts/promo/stitch_video.py
    python scripts/promo/stitch_video.py --music assets/music/theme.mp3
    python scripts/promo/stitch_video.py --draft          # half resolution, fast
    python scripts/promo/stitch_video.py --dry-run        # print the ffmpeg command

See docs/PROMO_VIDEO.md.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("stitch_video.py needs PyYAML: pip install pyyaml")
try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:  # pragma: no cover
    sys.exit("stitch_video.py needs Pillow for the cards and captions: pip install pillow")

REPO = Path(__file__).resolve().parents[2]

FONT_CANDIDATES = [
    # Bahnschrift is DIN-like: the motorsport look. Windows 10 and later.
    "C:/Windows/Fonts/bahnschrift.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
]
ACCENT = (225, 30, 45)


def fail(message: str) -> None:
    sys.exit(f"stitch_video: {message}")


def repo_path(text: str | None) -> Path | None:
    if not text:
        return None
    path = Path(text)
    return path if path.is_absolute() else REPO / path


# --- fonts and cards -----------------------------------------------------------------


def load_font(path: str | None, size: int, bold: bool = True) -> ImageFont.FreeTypeFont:
    candidates = [path] if path else FONT_CANDIDATES
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            font = ImageFont.truetype(candidate, size)
            if bold:
                try:  # a variable font (Bahnschrift) defaults to its regular instance
                    font.set_variation_by_name("Bold")
                except (OSError, ValueError, AttributeError):
                    pass
            return font
    if path:
        fail(f"font {path} not found")
    return ImageFont.load_default(size=size)


def spaced_text(draw: ImageDraw.ImageDraw, xy: tuple[float, float], text: str, font, fill, spacing: float,
                anchor_centre: bool = True) -> float:
    """Text with tracking (letter-spacing); returns its width."""
    widths = [draw.textlength(ch, font=font) for ch in text]
    total = sum(widths) + spacing * max(len(text) - 1, 0)
    x, y = xy
    if anchor_centre:
        x -= total / 2
    for ch, w in zip(text, widths):
        draw.text((x, y), ch, font=font, fill=fill, anchor="ls")
        x += w + spacing
    return total


def cover(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    width, height = size
    scale = max(width / image.width, height / image.height)
    resized = image.resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def make_card(path: Path, size: tuple[int, int], card: dict, font_path: str | None) -> None:
    width, height = size
    image = Image.new("RGB", size, (8, 8, 10))
    background = repo_path(card.get("background"))
    if background and background.exists():
        art = cover(Image.open(background).convert("RGB"), size)
        if card.get("blur", True):
            art = art.filter(ImageFilter.GaussianBlur(radius=width / 400))
        # Darkened so the type reads over any art; `darken: 0` shows it as is.
        shade = Image.new("RGB", size, (0, 0, 0))
        image = Image.blend(art, shade, float(card.get("darken", 0.55)))
    draw = ImageDraw.Draw(image)
    title = str(card.get("text") or "").upper()
    subtitle = str(card.get("subtitle") or "")
    small = load_font(font_path, round(height * 0.032), bold=False)
    if title:
        big = load_font(font_path, round(height * 0.13))
        centre_y = height * 0.52
        title_width = spaced_text(draw, (width / 2, centre_y), title, big, (255, 255, 255), spacing=height * 0.018)
        bar_y = centre_y + height * 0.035
        bar_half = max(title_width * 0.18, height * 0.05)
        draw.rectangle((width / 2 - bar_half, bar_y, width / 2 + bar_half, bar_y + max(3, height * 0.006)),
                       fill=ACCENT)
        subtitle_y = bar_y + height * 0.075
    else:
        # Art that carries its own logo: just the line, low, on a soft band.
        subtitle_y = height * 0.9
        if subtitle:
            band = Image.new("RGBA", size, (0, 0, 0, 0))
            ImageDraw.Draw(band).rectangle((0, subtitle_y - height * 0.06, width, subtitle_y + height * 0.03),
                                           fill=(0, 0, 0, 110))
            image = Image.alpha_composite(image.convert("RGBA"),
                                          band.filter(ImageFilter.GaussianBlur(height * 0.02))).convert("RGB")
            draw = ImageDraw.Draw(image)
    if subtitle:
        spaced_text(draw, (width / 2, subtitle_y), subtitle.upper(), small, (230, 230, 235),
                    spacing=height * 0.006)
    image.save(path)


def make_caption(path: Path, size: tuple[int, int], text: str, font_path: str | None) -> None:
    """A lower third: an accent bar and the words on a soft dark band, bottom left."""
    width, height = size
    image = Image.new("RGBA", size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = load_font(font_path, round(height * 0.034))
    words = text.upper()
    margin_x = width * 0.055
    baseline = height * 0.885
    spacing = height * 0.004
    text_width = sum(draw.textlength(ch, font=font) for ch in words) + spacing * max(len(words) - 1, 0)
    pad = height * 0.018
    band = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(band).rectangle(
        (margin_x - pad, baseline - height * 0.045, margin_x + text_width + pad * 2, baseline + pad),
        fill=(0, 0, 0, 120),
    )
    image = Image.alpha_composite(image, band.filter(ImageFilter.GaussianBlur(radius=height * 0.004)))
    draw = ImageDraw.Draw(image)
    draw.rectangle((margin_x - pad, baseline - height * 0.045, margin_x - pad + max(4, height * 0.006), baseline + pad),
                   fill=ACCENT + (255,))
    spaced_text(draw, (margin_x + pad * 0.6, baseline), words, font, (255, 255, 255, 255), spacing,
                anchor_centre=False)
    image.save(path)


# --- ffmpeg --------------------------------------------------------------------------


def probe_seconds(ffmpeg: str, path: Path) -> float:
    """A file's duration, read from ffmpeg's banner (no ffprobe needed)."""
    result = subprocess.run([ffmpeg, "-hide_banner", "-i", str(path)], capture_output=True, text=True)
    match = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", result.stderr)
    if not match:
        fail(f"cannot read the duration of {path}")
    hours, minutes, seconds = match.groups()
    return int(hours) * 3600 + int(minutes) * 60 + float(seconds)


def filter_script_args(ffmpeg: str, graph: Path) -> list[str]:
    """ffmpeg 7 reads a graph from a file with `-/filter_complex`; older ones with `-filter_complex_script`."""
    try:
        banner = subprocess.run([ffmpeg, "-hide_banner", "-version"], capture_output=True, text=True).stdout
        match = re.search(r"version\s+n?(\d+)", banner)
        major = int(match.group(1)) if match else 7
    except OSError:
        major = 7
    return ["-/filter_complex", str(graph)] if major >= 7 else ["-filter_complex_script", str(graph)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--shots", default=str(REPO / "scripts" / "promo" / "shots.yml"))
    parser.add_argument("--clips", default=str(REPO / "out" / "promo" / "clips"))
    parser.add_argument("--out", help="output file (default: edit.output)")
    parser.add_argument("--music", help="music file (default: edit.music)")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--draft", action="store_true", help="half resolution, fast encode")
    parser.add_argument("--no-captions", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="make the cards, print the ffmpeg command")
    args = parser.parse_args()

    with open(args.shots, encoding="utf-8") as handle:
        config = yaml.safe_load(handle)
    edit = config.get("edit") or fail("shots.yml has no 'edit' section")
    clips_dir = Path(args.clips)
    manifest_path = clips_dir / "manifest.json"
    if not manifest_path.exists():
        fail(f"no {manifest_path}: run make_clips.py first")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    ffmpeg = args.ffmpeg
    if not args.dry_run and shutil.which(ffmpeg) is None and not Path(ffmpeg).exists():
        fail(f"ffmpeg not found ('{ffmpeg}'); install it or pass --ffmpeg")

    width, _, height = str(edit.get("resolution", "1920x1080")).lower().partition("x")
    width, height = int(width), int(height)
    if args.draft:
        width, height = width // 2 // 2 * 2, height // 2 // 2 * 2
    size = (width, height)
    fps = int(edit.get("fps", 60))
    default_fade = float(edit.get("crossfade_s", 0.35))
    default_transition = str(edit.get("transition", "fade"))
    font = edit.get("font")
    captions_on = bool(edit.get("captions", True)) and not args.no_captions
    grade = edit.get("grade") or {}
    work = clips_dir / "_stitch"
    work.mkdir(parents=True, exist_ok=True)

    # --- the segments, in order ---
    segments: list[dict] = []
    title = edit.get("title")
    if title:
        card = work / "title.png"
        make_card(card, size, title, font)
        segments.append({"kind": "card", "file": card, "seconds": float(title.get("seconds", 3.0)),
                         "fade_in": True, "transition": default_transition, "crossfade": default_fade})
    for entry in edit.get("sequence", []):
        entry = {"id": entry} if isinstance(entry, str) else dict(entry)
        clip = manifest.get(entry["id"])
        if clip is None:
            print(f"stitch_video: skipping {entry['id']}: not in {manifest_path} (not rendered yet?)")
            continue
        file = Path(clip["file"])
        if not file.exists():
            print(f"stitch_video: skipping {entry['id']}: {file} is missing")
            continue
        seconds = probe_seconds(ffmpeg, file) if not args.dry_run else float(clip["seconds"])
        caption_text = entry.get("caption", clip.get("caption"))
        caption = None
        if captions_on and caption_text:
            caption = work / f"caption_{entry['id']}.png"
            make_caption(caption, size, caption_text, font)
        segments.append({
            "kind": "clip", "file": file, "seconds": seconds, "caption": caption,
            "transition": entry.get("transition", default_transition),
            "crossfade": float(entry.get("crossfade_s", default_fade)),
        })
    end = edit.get("end")
    if end:
        card = work / "end.png"
        make_card(card, size, end, font)
        segments.append({"kind": "card", "file": card, "seconds": float(end.get("seconds", 3.0)),
                         "fade_out": True, "transition": "fadeblack", "crossfade": 0.6})
    if sum(1 for s in segments if s["kind"] == "clip") == 0:
        fail("no clips to cut; render some with make_clips.py")

    # --- inputs and the filter graph ---
    inputs: list[str] = []
    filters: list[str] = []
    index = 0

    def add_input(*argv: str) -> int:
        nonlocal index
        inputs.extend(argv)
        index += 1
        return index - 1

    norm = (f"scale={width}:{height}:force_original_aspect_ratio=increase,crop={width}:{height},setsar=1,"
            f"fps={fps},format=yuv420p")
    graded = []
    if grade:
        graded.append(f"eq=contrast={float(grade.get('contrast', 1.0))}:saturation={float(grade.get('saturation', 1.0))}")
        if grade.get("vignette"):
            graded.append("vignette=angle=PI/5")
    for k, seg in enumerate(segments):
        seconds = seg["seconds"]
        if seg["kind"] == "card":
            i = add_input("-loop", "1", "-framerate", str(fps), "-t", f"{seconds:.3f}", "-i", str(seg["file"]))
            chain = f"[{i}:v]{norm},trim=duration={seconds:.3f},setpts=PTS-STARTPTS"
            if seg.get("fade_in"):
                chain += ",fade=t=in:st=0:d=0.8"
            if seg.get("fade_out"):
                chain += f",fade=t=out:st={max(seconds - 1.2, 0):.3f}:d=1.2"
            filters.append(f"{chain},settb=AVTB,fps={fps}[s{k}]")
            continue
        i = add_input("-i", str(seg["file"]))
        chain = f"[{i}:v]{norm},trim=duration={seconds:.3f},setpts=PTS-STARTPTS"
        if graded:
            chain += "," + ",".join(graded)
        if seg.get("caption"):
            c = add_input("-loop", "1", "-framerate", str(fps), "-t", f"{seconds:.3f}", "-i", str(seg["caption"]))
            show_from = min(0.5, seconds * 0.15)
            show_to = max(seconds - 0.9, show_from + 0.5)
            filters.append(
                f"[{c}:v]format=rgba,fade=t=in:st={show_from:.3f}:d=0.35:alpha=1,"
                f"fade=t=out:st={show_to:.3f}:d=0.35:alpha=1[c{k}]")
            filters.append(f"{chain}[b{k}]")
            filters.append(f"[b{k}][c{k}]overlay=0:0:shortest=1,format=yuv420p,settb=AVTB,fps={fps}[s{k}]")
        else:
            filters.append(f"{chain},settb=AVTB,fps={fps}[s{k}]")

    # Cross-fade each segment into the running cut. A transition may be no
    # longer than a third of either side.
    current = "s0"
    length = segments[0]["seconds"]
    for k in range(1, len(segments)):
        seg = segments[k]
        fade = min(seg["crossfade"], segments[k - 1]["seconds"] / 3, seg["seconds"] / 3)
        fade = max(fade, 1.0 / fps)
        offset = length - fade
        out = f"x{k}"
        filters.append(f"[{current}][s{k}]xfade=transition={seg['transition']}:duration={fade:.3f}:"
                       f"offset={offset:.3f}[{out}]")
        current = out
        length = offset + seg["seconds"]

    music = repo_path(args.music or edit.get("music"))
    if music and not music.exists():
        fail(f"music {music} not found")
    if music:
        m = add_input("-i", str(music))
        volume = float(edit.get("music_volume", 0.9))
        fade_out = min(3.0, length / 4)
        filters.append(
            f"[{m}:a]atrim=0:{length:.3f},asetpts=PTS-STARTPTS,afade=t=in:st=0:d=1.0,"
            f"afade=t=out:st={length - fade_out:.3f}:d={fade_out:.3f},volume={volume},"
            f"aformat=sample_rates=48000:channel_layouts=stereo[aout]")
    else:
        # A silent track: most players and uploads expect one.
        a = add_input("-f", "lavfi", "-t", f"{length:.3f}", "-i", "anullsrc=r=48000:cl=stereo")
        filters.append(f"[{a}:a]anull[aout]")

    output = Path(args.out) if args.out else repo_path(edit.get("output", "out/promo/ApexSim_promo.mp4"))
    output.parent.mkdir(parents=True, exist_ok=True)
    graph_file = work / "graph.txt"
    graph_file.write_text(";\n".join(filters), encoding="utf-8")
    encode = (["-c:v", "libx264", "-preset", "veryfast", "-crf", "23"] if args.draft
              else ["-c:v", "libx264", "-preset", "slow", "-crf", "16", "-profile:v", "high"])
    command = [
        ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-stats",
        *inputs,
        *filter_script_args(ffmpeg, graph_file),
        "-map", f"[{current}]", "-map", "[aout]",
        *encode, "-pix_fmt", "yuv420p", "-r", str(fps),
        "-c:a", "aac", "-b:a", "256k",
        "-t", f"{length:.3f}",
        "-movflags", "+faststart",
        str(output),
    ]
    print(f"{len(segments)} segments, {length:.1f} s -> {output}")
    if args.dry_run:
        print(subprocess.list2cmdline(command))
        print(f"filter graph: {graph_file}")
        return
    subprocess.run(command, check=True)
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
