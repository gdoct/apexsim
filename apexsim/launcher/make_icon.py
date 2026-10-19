"""Draws launcher.ico: the logo's three chevrons on a dark rounded square.

    python launcher/make_icon.py

Geometry is the mark from docs/assets/logo.png (x 60..330, y 100..320 there).
Rendered at 1024 px and downsampled per size; PNG-compressed ICO entries.
"""
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 24, 32, 48, 64, 128, 256]
BG = (20, 22, 26, 255)
CHEVRONS = [((152, 157, 161, 255), 60), ((245, 246, 248, 255), 130), ((219, 21, 25, 255), 200)]


def render(px=1024):
    im = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, px - 1, px - 1], radius=px // 5, fill=BG)
    # Mark is 270 x 220 in logo units; scale it to 62% of the tile and centre it.
    s = px * 0.62 / 270
    ox = (px - 270 * s) / 2
    oy = (px - 220 * s) / 2
    for colour, x0 in CHEVRONS:
        x = x0 - 60
        pts = [(x, 0), (x + 70, 0), (x + 130, 110), (x + 70, 220), (x, 220), (x + 60, 110)]
        d.polygon([(ox + a * s, oy + b * s) for a, b in pts], fill=colour)
    return im


if __name__ == "__main__":
    out = Path(__file__).with_name("launcher.ico")
    render().save(out, format="ICO", sizes=[(n, n) for n in SIZES])
    print(f"wrote {out} ({out.stat().st_size} bytes)")
