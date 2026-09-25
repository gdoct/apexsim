#!/usr/bin/env python3
"""Paint the road graffiti the Nordschleife is known for.

    python content/props/_tools/gen_graffiti.py          # -> content/props/decal/graffiti/*.png
    python content/props/_tools/gen_graffiti.py --sheet  # also a contact sheet in _preview/

Fans paint the Nordschleife's tarmac before every 24-hour race: names,
slogans, hearts, arrows and flags, in spray paint straight onto the road.
Each picture here is one `.ats` `decals` entry's image (`graffiti/<name>`),
laid on the road by `ats-export` as a road-hugging mesh and drawn in Unreal
by `M_ApexDecal` (masked by the alpha). `ApexPropImport -kind=decal` imports
the PNGs as `/Game/Props/decal/Graffiti/T_graffiti_<name>`.

The letters are strokes, not a font: a spray can draws lines, and a font
file would make the output depend on the machine it was baked on. Every
letter is a handful of polylines on a 4 x 6 grid, painted with a soft round
nozzle whose width and pressure wander along the stroke, with overspray
flecks around it and the whole coat worn thin by tyres where the racing line
crosses it. All randomness is seeded from the picture's name, so a re-run
writes byte-identical files.

An image is drawn the way a driver sees it: its top is the far end of the
decal, its left the left of the road. `ats-export` stretches it along the
road (a 1024 x 512 picture usually lies 7 m across and 14-20 m along), which
is exactly how fans paint for a driver whose eye is a metre off the road.

Custom art: any 1024 x 512 RGBA PNG dropped into `content/props/decal/graffiti/`
(white/colour paint, transparent elsewhere) works the same way, e.g. painted
in Blender's texture paint or any image editor.
"""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "decal" / "graffiti"
PREVIEW_DIR = ROOT / "_preview"
W, H = 1024, 512

WHITE = (0.93, 0.93, 0.90)
YELLOW = (0.95, 0.80, 0.10)
ORANGE = (0.98, 0.45, 0.05)
RED = (0.85, 0.10, 0.08)
BLUE = (0.12, 0.35, 0.85)
GREEN = (0.15, 0.65, 0.25)
BLACK = (0.05, 0.05, 0.05)

# ------------------------------------------------------------------ the letters
# Strokes on a 4-wide, 6-tall grid, y up. Curves are short polylines.
_O = [[(0, 1), (0, 5), (1, 6), (3, 6), (4, 5), (4, 1), (3, 0), (1, 0), (0, 1)]]
GLYPHS: dict[str, list[list[tuple[float, float]]]] = {
    "A": [[(0, 0), (0, 4), (2, 6), (4, 4), (4, 0)], [(0, 3), (4, 3)]],
    "B": [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)], [(3, 3), (4, 2), (4, 1), (3, 0), (0, 0)]],
    "C": [[(4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0), (3, 0), (4, 1)]],
    "D": [[(0, 0), (0, 6), (2.5, 6), (4, 4.5), (4, 1.5), (2.5, 0), (0, 0)]],
    "E": [[(4, 6), (0, 6), (0, 0), (4, 0)], [(0, 3), (3, 3)]],
    "F": [[(4, 6), (0, 6), (0, 0)], [(0, 3), (3, 3)]],
    "G": [[(4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0), (3, 0), (4, 1), (4, 3), (2, 3)]],
    "H": [[(0, 0), (0, 6)], [(4, 0), (4, 6)], [(0, 3), (4, 3)]],
    "I": [[(2, 0), (2, 6)], [(1, 6), (3, 6)], [(1, 0), (3, 0)]],
    "J": [[(4, 6), (4, 1), (3, 0), (1, 0), (0, 1)]],
    "K": [[(0, 0), (0, 6)], [(4, 6), (0, 2.5)], [(1.3, 3.6), (4, 0)]],
    "L": [[(0, 6), (0, 0), (4, 0)]],
    "M": [[(0, 0), (0, 6), (2, 3), (4, 6), (4, 0)]],
    "N": [[(0, 0), (0, 6), (4, 0), (4, 6)]],
    "O": _O,
    "P": [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)]],
    "Q": _O + [[(2.5, 1.5), (4.3, -0.3)]],
    "R": [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)], [(2, 3), (4, 0)]],
    "S": [[(4, 5), (3, 6), (1, 6), (0, 5), (0, 4), (1, 3), (3, 3), (4, 2), (4, 1), (3, 0), (1, 0), (0, 1)]],
    "T": [[(0, 6), (4, 6)], [(2, 6), (2, 0)]],
    "U": [[(0, 6), (0, 1), (1, 0), (3, 0), (4, 1), (4, 6)]],
    "V": [[(0, 6), (2, 0), (4, 6)]],
    "W": [[(0, 6), (1, 0), (2, 4), (3, 0), (4, 6)]],
    "X": [[(0, 6), (4, 0)], [(0, 0), (4, 6)]],
    "Y": [[(0, 6), (2, 3), (4, 6)], [(2, 3), (2, 0)]],
    "Z": [[(0, 6), (4, 6), (0, 0), (4, 0)]],
    "0": _O + [[(0.5, 1), (3.5, 5)]],
    "1": [[(1, 5), (2, 6), (2, 0)], [(1, 0), (3, 0)]],
    "2": [[(0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (0, 0), (4, 0)]],
    "3": [[(0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (3, 3), (1.5, 3)], [(3, 3), (4, 2), (4, 1), (3, 0), (1, 0), (0, 1)]],
    "4": [[(3, 0), (3, 6), (0, 2), (4, 2)]],
    "5": [[(4, 6), (0, 6), (0, 3.5), (3, 3.5), (4, 2.5), (4, 1), (3, 0), (1, 0), (0, 1)]],
    "6": [[(4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)]],
    "7": [[(0, 6), (4, 6), (1.5, 0)]],
    "8": [[(1, 3), (0, 4), (0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (3, 3), (1, 3), (0, 2), (0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (3, 3)]],
    "9": [[(4, 3), (1, 3), (0, 4), (0, 5), (1, 6), (3, 6), (4, 5), (4, 1), (3, 0), (1, 0)]],
    "!": [[(2, 6), (2, 1.8)], [(2, 0.3), (2, 0)]],
    "?": [[(0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (2, 2.6), (2, 1.8)], [(2, 0.3), (2, 0)]],
    "+": [[(0.5, 3), (3.5, 3)], [(2, 1.5), (2, 4.5)]],
    "#": [[(1.3, 0), (1.7, 6)], [(2.8, 0), (3.2, 6)], [(0, 2), (4, 2)], [(0, 4), (4, 4)]],
    "-": [[(0.5, 3), (3.5, 3)]],
    ".": [[(2, 0.3), (2, 0)]],
    "&": [[(4, 0), (1, 4), (1, 5), (2, 6), (3, 5), (3, 4), (0, 2), (0, 1), (1, 0), (2.5, 0), (4, 2)]],
    " ": [],
}
# Umlauts: the letter plus two dabs over it (the grid runs to y = 7.5).
for plain, uml in (("A", "Ä"), ("O", "Ö"), ("U", "Ü")):
    GLYPHS[uml] = GLYPHS[plain] + [[(1, 7.2), (1, 7.0)], [(3, 7.2), (3, 7.0)]]

HEART = [[(2, 0), (0, 2.6), (0, 4.4), (1, 5.4), (2, 4.8), (3, 5.4), (4, 4.4), (4, 2.6), (2, 0)]]


def _rng(name: str) -> np.random.Generator:
    return np.random.default_rng(int.from_bytes(hashlib.sha256(name.encode()).digest()[:8], "little"))


# ------------------------------------------------------------------ the canvas


class Canvas:
    """Paint coverage per colour layer, composited at the end."""

    def __init__(self, name: str):
        self.name = name
        self.rng = _rng(name)
        self.layers: list[tuple[tuple[float, float, float], np.ndarray]] = []
        yy, xx = np.mgrid[0:H, 0:W]
        self.xx = xx.astype(np.float32)
        self.yy = yy.astype(np.float32)

    def layer(self, color) -> np.ndarray:
        a = np.zeros((H, W), np.float32)
        self.layers.append((color, a))
        return a

    def stroke(self, a: np.ndarray, pts, width: float, pressure: float = 0.95):
        """A sprayed line through `pts` (pixels): a soft round nozzle stamped
        along it, its width and pressure wandering, with flecks either side."""
        rng = self.rng
        pts = [np.asarray(p, np.float32) for p in pts]
        if len(pts) == 1:
            pts = pts * 2
        wobble = rng.uniform(0, 6.28)
        for p0, p1 in zip(pts[:-1], pts[1:]):
            seg = float(np.hypot(*(p1 - p0)))
            n = max(int(seg / (width * 0.18)), 1)
            for i in range(n + 1):
                t = i / n
                c = p0 + (p1 - p0) * t
                wobble += 0.05
                r = width * 0.5 * (1.0 + 0.12 * math.sin(wobble * 1.7))
                x0, x1 = int(max(c[0] - 2 * r, 0)), int(min(c[0] + 2 * r + 1, W))
                y0, y1 = int(max(c[1] - 2 * r, 0)), int(min(c[1] + 2 * r + 1, H))
                if x0 >= x1 or y0 >= y1:
                    continue
                d = np.hypot(self.xx[y0:y1, x0:x1] - c[0], self.yy[y0:y1, x0:x1] - c[1]) / r
                # Solid core, soft sprayed edge.
                dab = np.clip(1.25 - d * d * 0.9, 0.0, 1.0) * pressure
                np.maximum(a[y0:y1, x0:x1], dab, out=a[y0:y1, x0:x1])
            # Overspray: flecks scattered round the segment.
            k = int(seg * width * 0.02) + 4
            f = rng.uniform(0, 1, k)
            off = rng.normal(0, width * 0.9, (k, 2))
            for (fx, fy), ft in zip(off, f):
                x = int(p0[0] + (p1[0] - p0[0]) * ft + fx)
                y = int(p0[1] + (p1[1] - p0[1]) * ft + fy)
                if 0 <= x < W and 0 <= y < H:
                    a[y, x] = max(a[y, x], rng.uniform(0.3, 0.8))

    def text(self, a, s: str, cx: float, cy: float, height: float, width: float,
             slant: float = 0.12, jitter: float = 0.25, outline: np.ndarray | None = None):
        """Paint `s` centred on (cx, cy): letters `height` px tall and as
        wide as fits `width`, slanted and each a little off its line."""
        rng = self.rng
        n = len(s)
        adv = min(width / max(n, 1), height * 0.95)
        gw = adv * 0.72
        sx, sy = gw / 4.0, height / 6.0
        stroke_w = max(min(sx, sy) * 1.15, 6.0)
        x = cx - adv * n / 2 + (adv - gw) / 2
        for ch in s:
            dy = rng.normal(0, height * 0.04 * jitter / 0.25)
            dx = rng.normal(0, adv * 0.03)
            for poly in GLYPHS.get(ch, []):
                pts = []
                for gx, gy in poly:
                    gx += rng.normal(0, jitter * 0.25)
                    gy += rng.normal(0, jitter * 0.25)
                    px = x + dx + gx * sx + gy * sy * slant
                    py = cy + height / 2 + dy - gy * sy
                    pts.append((px, py))
                if outline is not None:
                    self.stroke(outline, pts, stroke_w * 1.9, 0.95)
                self.stroke(a, pts, stroke_w)
            x += adv

    def shape(self, a, polys, cx, cy, size, width_px=None, fill=False):
        s = size / 6.0
        pts_all = []
        for poly in polys:
            pts = [(cx + (gx - 2) * s, cy - (gy - 3) * s) for gx, gy in poly]
            pts_all.append(pts)
            self.stroke(a, pts, width_px or s * 0.9)
        if fill:
            from PIL import ImageDraw

            m = Image.new("L", (W, H), 0)
            for pts in pts_all:
                ImageDraw.Draw(m).polygon(pts, fill=230)
            np.maximum(a, np.asarray(m, np.float32) / 255.0, out=a)

    def finish(self) -> Image.Image:
        """Composite the layers and wear the coat: paint thins in blotches,
        and most where the tyres run (bands along the road, i.e. vertical
        in the picture)."""
        rng = self.rng
        # Blotchy wear from two octaves of value noise.
        wear = np.ones((H, W), np.float32)
        for cells, amp in ((8, 0.35), (32, 0.25)):
            g = rng.uniform(0, 1, (cells // 2 + 2, cells + 2)).astype(np.float32)
            img = Image.fromarray((g * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)
            wear -= amp * (np.asarray(img, np.float32) / 255.0)
        # Two tyre tracks at random places across the road.
        for _ in range(2):
            x0 = rng.uniform(0.2, 0.8) * W
            band = np.exp(-(((self.xx - x0) / (W * 0.07)) ** 2))
            wear -= 0.35 * band
        # Grain: asphalt pores the paint never reaches.
        grain = rng.uniform(0, 1, (H, W)).astype(np.float32)
        wear = np.clip(wear + 0.35, 0, 1) * (grain > 0.12)

        rgb = np.zeros((H, W, 3), np.float32)
        alpha = np.zeros((H, W), np.float32)
        for color, a in self.layers:
            a = np.clip(a, 0, 1)
            rgb = rgb * (1 - a[..., None]) + np.asarray(color, np.float32) * a[..., None]
            alpha = alpha + a * (1 - alpha)
        alpha *= wear
        # Sixteen levels of coverage are all a clip-masked material can tell
        # apart, and they keep the PNGs small.
        alpha = np.round(np.clip(alpha, 0, 1) * 15) / 15
        rgb = np.round(np.clip(rgb, 0, 1) * 31) / 31
        out = np.dstack([rgb, alpha])
        # Colour where there is no paint does not matter to a mask, but a
        # mip of it does: bleed the paint colour out so the edges stay clean.
        mean = (rgb * alpha[..., None]).sum((0, 1)) / max(alpha.sum(), 1.0)
        out[..., :3] = np.where(alpha[..., None] > 0.02, out[..., :3], mean)
        return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA")


# ------------------------------------------------------------------ the pictures
# name -> painter. Every text is invented: fan slogans and first names, no
# real person, team or brand.


def _lines(lines, color=WHITE, outline=None, heights=None):
    def paint(c: Canvas):
        n = len(lines)
        out = c.layer(outline) if outline else None
        a = c.layer(color)
        hs = heights or [H * (0.62 if n == 1 else 0.36)] * n
        total = sum(hs) * 1.18
        y = H / 2 - total / 2
        for line, h in zip(lines, hs):
            y += h * 0.59
            c.text(a, line, W / 2, y, h, W * 0.9, outline=out)
            y += h * 0.59

    return paint


def _heart_names(left, right, color=RED):
    def paint(c: Canvas):
        a = c.layer(WHITE)
        h = c.layer(color)
        c.shape(h, HEART, W / 2, H / 2, H * 0.62, fill=True)
        c.text(a, left, W * 0.2, H * 0.5, H * 0.3, W * 0.34)
        c.text(a, right, W * 0.8, H * 0.5, H * 0.3, W * 0.34)

    return paint


def _arrows(color=YELLOW, direction=1):
    def paint(c: Canvas):
        a = c.layer(color)
        for i in range(3):
            x = W * (0.3 + 0.2 * i)
            pts = [(x - direction * 60, H * 0.2), (x + direction * 60, H * 0.5), (x - direction * 60, H * 0.8)]
            c.stroke(a, pts, 46)

    return paint


def _chequer(c: Canvas):
    a = c.layer(WHITE)
    k = c.layer(BLACK)
    n, m = 6, 3
    cw, ch = W * 0.7 / n, H * 0.7 / m
    for i in range(n):
        for j in range(m):
            x, y = W * 0.15 + i * cw, H * 0.15 + j * ch
            layer = a if (i + j) % 2 == 0 else k
            for t in range(0, int(ch), 18):
                c.stroke(layer, [(x + 8, y + t + 8), (x + cw - 8, y + t + 8)], 22)


def _smiley(c: Canvas):
    a = c.layer(YELLOW)
    r = H * 0.38
    circle = [(W / 2 + r * math.cos(t), H / 2 + r * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 40)]
    c.stroke(a, circle, 34)
    c.stroke(a, [(W / 2 - r * 0.35, H / 2 - r * 0.3)], 50)
    c.stroke(a, [(W / 2 + r * 0.35, H / 2 - r * 0.3)], 50)
    smile = [(W / 2 + r * 0.6 * math.cos(t), H / 2 + r * 0.6 * math.sin(t)) for t in np.linspace(0.3, math.pi - 0.3, 16)]
    c.stroke(a, smile, 30)


def _number(text, color=WHITE, outline=RED):
    return _lines([text], color=color, outline=outline, heights=[H * 0.72])


PICTURES = {
    "gruene_hoelle": _lines(["GRÜNE", "HÖLLE"], WHITE, outline=GREEN),
    "vollgas": _lines(["VOLLGAS"]),
    "ring_frei": _lines(["RING FREI"], YELLOW),
    "eifel": _lines(["EIFEL"], WHITE, outline=BLUE),
    "nix_bremsen": _lines(["NIX", "BREMSEN!"]),
    "flat_out": _lines(["FLAT OUT"], YELLOW),
    "late_apex": _lines(["LATE", "APEX"]),
    "hup_holland": _lines(["HUP", "HOLLAND"], ORANGE),
    "allez_allez": _lines(["ALLEZ", "ALLEZ"], BLUE, outline=WHITE),
    "kalle": _lines(["KALLE"], RED),
    "jens_mia": _heart_names("JENS", "MIA"),
    "tom_lea": _heart_names("TOM", "LEA", ORANGE),
    "team_wurst": _lines(["TEAM", "WURST"], YELLOW),
    "danke_ring": _lines(["DANKE", "RING"]),
    "vierundzwanzig": _number("24H"),
    "kein_limit": _lines(["KEIN LIMIT"], RED, outline=WHITE),
    "ich_war_hier": _lines(["ICH WAR", "HIER"]),
    "go_go_go": _lines(["GO GO GO"], GREEN, outline=WHITE),
    "opa_heinz": _lines(["OPA", "HEINZ 67"], YELLOW),
    "moppel": _lines(["MOPPEL"], WHITE, outline=RED),
    "benni_79": _number("BENNI 79", WHITE, None),
    "lift_nein": _lines(["LIFT?", "NEIN!"], WHITE, outline=RED),
    "schneller": _lines(["SCHNELLER"]),
    "arrows_right": _arrows(YELLOW, 1),
    "arrows_left": _arrows(WHITE, -1),
    "smiley": _smiley,
    "chequer": _chequer,
    "heart": lambda c: c.shape(c.layer(RED), HEART, W / 2, H / 2, H * 0.8, fill=True),
    "hallo_mama": _lines(["HALLO", "MAMA!"], WHITE, outline=BLUE),
    "bambini": _lines(["BAMBINI"], ORANGE),
}


def paint(name: str) -> Image.Image:
    c = Canvas(name)
    PICTURES[name](c)
    return c.finish()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sheet", action="store_true", help="also write _preview/graffiti_sheet.png")
    ap.add_argument("names", nargs="*", help="only these pictures")
    args = ap.parse_args()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    names = args.names or sorted(PICTURES)
    images = []
    for name in names:
        img = paint(name)
        img.save(OUT_DIR / f"{name}.png", optimize=True)
        images.append((name, img))
        print(f"  {name}.png")
    if args.sheet:
        cols = 5
        rows = (len(images) + cols - 1) // cols
        tw, th = 256, 128
        sheet = Image.new("RGB", (cols * tw, rows * th), (46, 46, 50))
        for i, (_, img) in enumerate(images):
            tile = img.resize((tw, th), Image.LANCZOS)
            sheet.paste(tile, ((i % cols) * tw, (i // cols) * th), tile)
        PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
        sheet.save(PREVIEW_DIR / "graffiti_sheet.png")
        print(f"  sheet -> {PREVIEW_DIR / 'graffiti_sheet.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
