"""Generate hoarding textures (3:1) for the fictional ApexSim brands and the
braking-distance boards. Output: out/brands/<brand>.png, out/markers/<n>.png"""
import os, math
from PIL import Image, ImageDraw, ImageFont

OUT = "/home/claude/props/out"
FONT_B = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"
FONT_I = "/usr/share/fonts/truetype/google-fonts/Poppins-BoldItalic.ttf"
FONT_M = "/usr/share/fonts/truetype/google-fonts/Poppins-Medium.ttf"

# name, bg, fg, accent, tagline, style
BRANDS = [
    ("piretti",   (232, 30, 30),   (255, 255, 255), (255, 210, 0),  "TYRES",       "italic"),
    ("rolux",     (12, 60, 40),    (220, 190, 110), (220, 190, 110),"OFFICIAL TIMEKEEPER", "serifcaps"),
    ("apexsim",   (18, 18, 20),    (255, 255, 255), (0, 200, 255),  "",            "bold"),
    ("velocet",   (0, 70, 160),    (255, 255, 255), (255, 120, 0),  "FUEL",        "italic"),
    ("kronos",    (245, 245, 240), (30, 30, 34),    (200, 30, 40),  "WATCHES",     "bold"),
    ("hexon",     (255, 200, 0),   (20, 20, 20),    (20, 20, 20),   "MOTOR OIL",   "bold"),
    ("northwind", (255, 255, 255), (0, 90, 170),    (0, 90, 170),   "AIRLINES",    "italic"),
    ("brix",      (230, 90, 0),    (255, 255, 255), (255, 255, 255),"TOOLS",       "bold"),
]

def fit_font(path, text, max_w, max_h):
    size = 10
    while True:
        f = ImageFont.truetype(path, size + 4)
        l, t, r, b = f.getbbox(text)
        if r - l > max_w or b - t > max_h:
            return ImageFont.truetype(path, size)
        size += 4

def brand(name, bg, fg, accent, tagline, style, W=3072, H=1024):
    img = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(img)
    # thin accent bar top/bottom
    d.rectangle([0, 0, W, 28], fill=accent)
    d.rectangle([0, H - 28, W, H], fill=accent)
    word = name.upper()
    fpath = FONT_I if style == "italic" else FONT_B
    if tagline:
        f = fit_font(fpath, word, W * 0.8, H * 0.50)
        d.text((W / 2, H * 0.42), word, font=f, fill=fg, anchor="mm")
        f2 = fit_font(FONT_M, tagline, W * 0.5, H * 0.13)
        d.text((W / 2, H * 0.80), tagline, font=f2, fill=accent, anchor="mm")
    else:
        f = fit_font(fpath, word, W * 0.8, H * 0.66)
        d.text((W / 2, H * 0.5), word, font=f, fill=fg, anchor="mm")
    if style == "serifcaps":
        # rolux gets a crown-ish mark: five dots
        cx, cy = W // 2, int(H * 0.16)
        for i in range(5):
            d.ellipse([cx - 120 + i * 60 - 12, cy - 12, cx - 120 + i * 60 + 12, cy + 12], fill=accent)
    return img

def marker(n, W=768, H=1024):
    img = Image.new("RGB", (W, H), (250, 250, 250))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, H], outline=(20, 20, 20), width=24)
    # count stripes: 50=1, 100=2, 150=3, 200=4 in the lower band (the FIA style)
    band_top = int(H * 0.66)
    stripes = n // 50
    d.rectangle([24, band_top, W - 24, H - 24], fill=(220, 30, 30))
    f = fit_font(FONT_B, str(n), W * 0.82, H * 0.5)
    l, t, r, b = f.getbbox(str(n))
    d.text(((W - (r - l)) // 2 - l, int(H * 0.33) - (b - t) // 2 - t), str(n), font=f, fill=(20, 20, 20))
    sw = (W - 48) / 4
    for i in range(stripes):
        d.rectangle([24 + i * sw + 10, band_top + 20, 24 + (i + 1) * sw - 10, H - 44], fill=(250, 250, 250))
    return img

os.makedirs(f"{OUT}/brands", exist_ok=True); os.makedirs(f"{OUT}/markers", exist_ok=True)
for spec in BRANDS:
    brand(*spec).save(f"{OUT}/brands/{spec[0]}.png", optimize=True)
for n in (50, 100, 150, 200):
    marker(n).save(f"{OUT}/markers/{n}.png", optimize=True)
# contact sheet
sheet = Image.new("RGB", (3072 // 4 * 4, 1024 // 4 * 2 + 1024 // 4), (80, 80, 80))
for i, spec in enumerate(BRANDS):
    im = Image.open(f"{OUT}/brands/{spec[0]}.png").resize((768, 256))
    sheet.paste(im, ((i % 4) * 768, (i // 4) * 256))
for i, n in enumerate((50, 100, 150, 200)):
    im = Image.open(f"{OUT}/markers/{n}.png").resize((192, 256))
    sheet.paste(im, (i * 200, 512))
sheet.save(f"{OUT}/contact.png")
print("ok")
