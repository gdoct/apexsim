"""Shared textures for the generated cars (plain Python: numpy + Pillow).

    python scripts/content/cars/textures.py

Writes into content/cars/_textures/:

* `carbon_twill.png` - a 2x2 twill weave, 512 px square, seamless. The build
  scripts map it at 4 tiles per metre (`carlib.CARBON_UV_SCALE`), so a tow is
  ~3.9 mm wide. Base colour only - the client's car parents take a base-colour
  texture and nothing else - so the weave is in the tows' brightness: the
  warp and the weft catch the light differently (anisotropic sheen) and each
  tow is rounded across its width.
* `tyre_marks.png` - the slick's sidewall lettering (2048 x 160, masked),
  wrapped twice round the sidewall by scripts/content/wheels/build_wheels.py.
* `numbers.png` - race-number panels, a 4 x 4 atlas (NUMBERS below;
  `carlib.number_uv(number)`), masked.
* `sponsors.png` - the sponsor atlas: 2048 x 2048, 2 columns x 8 rows of
  1024 x 256 cells, alpha-masked. Eight fictional sponsors, each drawn twice
  (cell 2k: light on dark or white, cell 2k+1: the dark version), so a car
  can pick whichever reads on its paint. `carlib.atlas_uv(cell)` gives a
  cell's UV rectangle. Every name here is invented.

The PNGs are checked in; the build scripts only load them. Re-run this when
the art changes (the fonts are Poppins / Lora - drop the TTFs into
content/cars/_fonts on Windows - with DejaVu as the fallback).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUT = os.path.join(REPO, "content", "cars", "_textures")
FONT_DIRS = [os.environ.get("APEX_FONTS", ""), os.path.join(REPO, "content", "cars", "_fonts"),
             r"C:\Windows\Fonts", "/usr/share/fonts/truetype/google-fonts", "/usr/share/fonts/truetype/dejavu"]
FONTS = {
    "bold": ["Poppins-Bold.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"],
    "bolditalic": ["Poppins-BoldItalic.ttf", "arialbi.ttf", "DejaVuSans-BoldOblique.ttf"],
    "medium": ["Poppins-Medium.ttf", "arial.ttf", "DejaVuSans.ttf"],
    "mediumitalic": ["Poppins-MediumItalic.ttf", "ariali.ttf", "DejaVuSans-Oblique.ttf"],
    "light": ["Poppins-Light.ttf", "arial.ttf", "DejaVuSans.ttf"],
    "serif": ["Lora-Variable.ttf", "georgiab.ttf", "DejaVuSerif-Bold.ttf"],
}


def font(kind, size):
    for name in FONTS[kind]:
        for d in FONT_DIRS:
            p = os.path.join(d, name)
            if d and os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


# ------------------------------------------------------------------ carbon
def carbon(size=512, tow=8):
    """2x2 twill. Over/under: at tow column i, row j the warp (vertical) tow
    is on top when (i - j) mod 4 < 2."""
    n = size // tow
    yy, xx = np.mgrid[0:size, 0:size]
    i, j = xx // tow, yy // tow
    u, v = (xx % tow + 0.5) / tow, (yy % tow + 0.5) / tow
    warp_top = ((i - j) % 4) < 2
    # rounded tow: bright along its middle, dark at the edges; fibres run
    # along the tow, so a faint streak across the other direction
    across = np.where(warp_top, u, v)
    # a float runs over two crossing tows: where along it this pixel is (0..1)
    ph = (i - j) % 4
    along = np.where(warp_top, ((1 - ph) + v) / 2.0, ((ph - 2) + u) / 2.0)
    crown = np.sin(np.pi * across) ** 0.7
    dip = 0.72 + 0.28 * np.sin(np.pi * along) ** 0.5      # the tow dives under at the float's ends
    sheen = np.where(warp_top, 1.0, 0.62)                  # warp and weft catch the light differently
    rng = np.random.default_rng(7)
    fibre = 1.0 + 0.05 * rng.standard_normal((size, size))
    lum = (0.30 + 0.70 * crown * dip) * sheen * fibre
    # sRGB 0..255: the carbon's linear albedo ~0.02..0.06 is sRGB ~38..68
    base = np.array([34.0, 35.0, 39.0])
    span = np.array([40.0, 41.0, 46.0])
    rgb = base[None, None, :] + span[None, None, :] * lum[..., None]
    assert n * tow == size
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")


# ---------------------------------------------------------------- sponsors
# word, sub, face, style. style: "plate" (word on a rounded plate),
# "outline" (word with a dark keyline), "bar" (word with a stripe under it),
# "badge" (a roundel mark before the word), "split" (two-colour word).
SPONSORS = [
    dict(word="VOLTARA", sub="ENERGY SYSTEMS", face="bolditalic", style="bar", hue=(255, 196, 0)),
    dict(word="NORDLINE", sub="LOGISTICS", face="bold", style="plate", hue=(0, 120, 215)),
    dict(word="KESTREL", sub="LUBRICANTS", face="bolditalic", style="badge", hue=(230, 40, 40)),
    dict(word="AXION", sub="QUANTUM CLOUD", face="light", style="outline", hue=(0, 200, 190), track=30),
    dict(word="SOLENNE", sub="MAISON DE MONTRES", face="serif", style="split", hue=(200, 160, 80), track=18),
    dict(word="HALCYON", sub="AIRWAYS", face="medium", style="bar", hue=(120, 80, 220)),
    dict(word="BRAVURA", sub="CAFFÈ", face="bolditalic", style="plate", hue=(200, 20, 30)),
    dict(word="APEX", sub="RACING TYRES", face="bold", style="badge", hue=(250, 210, 10)),
]
CELL_W, CELL_H = 1024, 256


def _text(draw, xy, text, fnt, fill, track=0, stroke=0, stroke_fill=None):
    x, y = xy
    if not track:
        draw.text((x, y), text, font=fnt, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)
        return
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)
        x += draw.textlength(ch, font=fnt) + track


def _width(draw, text, fnt, track=0):
    return draw.textlength(text, font=fnt) + track * max(len(text) - 1, 0)


def sponsor_cell(sp, dark):
    """One cell: `dark` False draws the light version (white/colour, for dark
    paint and carbon), True the dark one (near-black, for light paint)."""
    im = Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    ink = (14, 14, 16, 255) if dark else (250, 250, 248, 255)
    hue = tuple(sp["hue"]) + (255,)
    track = sp.get("track", 0)
    style = sp["style"]
    room_w = CELL_W * (0.66 if style == "badge" else 0.80)
    size = 140
    fw = font(sp["face"], size)
    fs = font("medium", 36)
    extra = 36 + (24 if style == "plate" else 6) + (22 if style == "bar" else 0)

    def word_h(f):
        bb = d.textbbox((0, 0), sp["word"], font=f)
        return bb[1], bb[3]
    while size > 50:
        top, bot = word_h(fw)
        if _width(d, sp["word"], fw, track) <= room_w and (bot - top) + extra <= CELL_H - 30:
            break
        size -= 4
        fw = font(sp["face"], size)
    ww = _width(d, sp["word"], fw, track)
    top, bot = word_h(fw)
    asc, desc = bot, 0
    x0 = (CELL_W - ww) / 2
    y0 = (CELL_H - ((bot - top) + extra)) / 2 - top
    if style == "badge":
        # a roundel before the word, the word shifted right
        r = 70
        x0 = (CELL_W - ww - 2 * r - 30) / 2 + 2 * r + 30
        cx, cy = x0 - 30 - r, CELL_H / 2
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hue)
        d.ellipse((cx - r * 0.55, cy - r * 0.55, cx + r * 0.55, cy + r * 0.55), fill=ink if not dark else (250, 250, 248, 255))
        d.polygon([(cx - r * 0.2, cy - r * 0.9), (cx + r * 0.9, cy), (cx - r * 0.2, cy + r * 0.9)], fill=hue)
    if style == "plate":
        pad = 26
        fill = hue if not dark else (14, 14, 16, 255)
        d.rounded_rectangle((x0 - pad * 2, y0 + top - pad, x0 + ww + pad * 2, y0 + bot + pad * 0.6),
                            radius=26, fill=fill)
        word_ink = (250, 250, 248, 255)
    else:
        word_ink = ink
    if style == "outline":
        _text(d, (x0, y0), sp["word"], fw, word_ink, track, stroke=5, stroke_fill=hue)
    elif style == "split":
        half = len(sp["word"]) // 2
        _text(d, (x0, y0), sp["word"][:half], fw, word_ink, track)
        x1 = x0 + _width(d, sp["word"][:half], fw, track) + track
        _text(d, (x1, y0), sp["word"][half:], fw, hue, track)
    else:
        _text(d, (x0, y0), sp["word"], fw, word_ink, track)
    sub_y = y0 + bot + (24 if style == "plate" else 6)
    if style == "bar":
        d.rectangle((x0, sub_y - 4, x0 + ww, sub_y + 10), fill=hue)
        sub_y += 18
    sw = _width(d, sp["sub"], fs, 10)
    _text(d, ((CELL_W - sw) / 2 if style != "badge" else x0 + (ww - sw) / 2, sub_y), sp["sub"], fs,
          ink if style != "plate" else (ink if dark else (250, 250, 248, 255)), 10)
    return im


def sponsors():
    atlas = Image.new("RGBA", (2 * CELL_W, 8 * CELL_H), (0, 0, 0, 0))
    for k, sp in enumerate(SPONSORS):
        for dark in (False, True):
            cell = 2 * k + (1 if dark else 0)
            col, row = cell % 2, cell // 2
            atlas.paste(sponsor_cell(sp, dark), (col * CELL_W, row * CELL_H))
    # hard alpha: the car parents mask at 0.5, so keep the edge where the
    # antialiasing put it but make it binary in the mips' eyes
    a = np.array(atlas)
    a[..., 3] = np.where(a[..., 3] > 110, 255, 0).astype(np.uint8)
    return Image.fromarray(a, "RGBA")


# ------------------------------------------------------------- tyre marks
def tyre_marks(w=2048, h=160):
    """The sidewall lettering: half the tyre's circumference (the wheel
    builder wraps it twice), white on clear, masked. A big wordmark, then
    the small print, as a real slick carries them."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    big = font("bold", 118)
    small = font("medium", 46)
    ink = (245, 245, 242, 255)
    word = "APEX"
    ww = _width(d, word, big, 14)
    x = (w * 0.30) - ww / 2
    bb = d.textbbox((0, 0), word, font=big)
    _text(d, (x, (h - (bb[3] - bb[1])) / 2 - bb[1]), word, big, ink, 14)
    for (txt, cx) in (("RACING SLICK", w * 0.66), ("305/670 R18", w * 0.90)):
        tw = _width(d, txt, small, 6)
        sb = d.textbbox((0, 0), txt, font=small)
        _text(d, (cx - tw / 2, (h - (sb[3] - sb[1])) / 2 - sb[1]), txt, small, ink, 6)
    a = np.array(im)
    a[..., 3] = np.where(a[..., 3] > 110, 255, 0).astype(np.uint8)
    return Image.fromarray(a, "RGBA")


# ------------------------------------------------------------- race numbers
# The numbers the generated cars carry (their VARIANTS' `number`); a cell
# each in a 4 x 4 atlas of 512 px squares, numbered like the sponsor cells.
NUMBERS = ["9", "50", "16", "7", "22", "51", "38", "91", "63", "88", "4", "14", "1", "2", "3", "5"]


def number_panel(num):
    """An endurance-style number panel: a white rounded square, a dark
    keyline, the numerals heavy and condensed in the middle."""
    im = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((8, 8, 504, 504), radius=70, fill=(245, 245, 242, 255))
    d.rounded_rectangle((26, 26, 486, 486), radius=56, outline=(16, 16, 18, 255), width=10)
    size = 380 if len(num) == 1 else 300
    f = font("bold", size)
    bb = d.textbbox((0, 0), num, font=f)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((512 - w) / 2 - bb[0], (512 - h) / 2 - bb[1]), num, font=f, fill=(14, 14, 16, 255))
    return im


def numbers():
    atlas = Image.new("RGBA", (2048, 2048), (0, 0, 0, 0))
    for k, num in enumerate(NUMBERS):
        atlas.paste(number_panel(num), ((k % 4) * 512, (k // 4) * 512))
    a = np.array(atlas)
    a[..., 3] = np.where(a[..., 3] > 110, 255, 0).astype(np.uint8)
    return Image.fromarray(a, "RGBA")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    carbon().save(os.path.join(OUT, "carbon_twill.png"), optimize=True)
    sponsors().save(os.path.join(OUT, "sponsors.png"), optimize=True)
    tyre_marks().save(os.path.join(OUT, "tyre_marks.png"), optimize=True)
    numbers().save(os.path.join(OUT, "numbers.png"), optimize=True)
    for f in ("carbon_twill.png", "sponsors.png", "tyre_marks.png", "numbers.png"):
        p = os.path.join(OUT, f)
        print(p, os.path.getsize(p))
    sys.exit(0)
