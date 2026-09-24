"""Extra liveries for the generated cars: writes each car's `[[livery]]`
tables into its car.toml and draws the sponsor logos they use.

    python content/cars/liveries.py            # every car below
    python content/cars/liveries.py bugotti-chiffon-hypercar

Needs Pillow. Plain Python, not Blender: the logos are 2D.

A livery is a repaint of the same mesh (docs/CAR_MODELS.md, Liveries): the
client sets `car_paint` and `car_accent` to the livery's colours and swaps the
`car_logo` texture, so a scheme is two colours, the paint's metallic and one
wordmark. Colours are linear RGB, the same numbers the build scripts give
Blender's Principled BSDF; the logo PNGs are ordinary sRGB images.

Everything from the marker line to the end of a car.toml belongs to this
script and is rewritten on every run; keep hand edits above it.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
MARKER = "# --- liveries: written by content/cars/liveries.py, edits below this line are lost ---"

# Fonts: the first file found wins. Drop TTFs into content/cars/_fonts to pin
# the look; otherwise Windows' and Linux's usual faces stand in.
FONT_DIRS = [os.path.join(HERE, "_fonts"), r"C:\Windows\Fonts", "/usr/share/fonts/truetype/google-fonts",
             "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/liberation2"]
FONTS = {
    "bold": ["Poppins-Bold.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"],
    "bolditalic": ["Poppins-BoldItalic.ttf", "arialbi.ttf", "DejaVuSans-BoldOblique.ttf"],
    "medium": ["Poppins-Medium.ttf", "arial.ttf", "DejaVuSans.ttf"],
    "light": ["Poppins-Light.ttf", "arial.ttf", "DejaVuSans.ttf"],
    "serif": ["Lora-Variable.ttf", "georgiab.ttf", "DejaVuSerif-Bold.ttf"],
}


def font(kind, size):
    for name in FONTS[kind]:
        for d in FONT_DIRS:
            path = os.path.join(d, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


# ---------------------------------------------------------------- schemes
# paint / accent: linear RGB. logo: word, sub, colours (sRGB 0-255), face,
# and an optional plate the word sits on (rounded bar, like a sponsor patch).
SCHEMES = {
    "heritage": dict(name="Orbital Heritage", paint=(0.23, 0.52, 0.78), accent=(0.95, 0.30, 0.02), metallic=0.25,
                     logo=dict(word="ORBITAL", sub="LUBRICANTS", fg=(20, 32, 70), sub_fg=(20, 32, 70),
                               face="bold", plate=(240, 120, 20))),
    "blackgold": dict(name="Aurum Black", paint=(0.010, 0.010, 0.012), accent=(0.72, 0.50, 0.12), metallic=0.55,
                      logo=dict(word="AURUM", sub="PRIVATE BANK", fg=(214, 172, 90), sub_fg=(214, 172, 90),
                                face="serif", plate=None, track=28)),
    "tricolore": dict(name="Veloce Tricolore", paint=(0.86, 0.86, 0.85), accent=(0.62, 0.02, 0.03), metallic=0.20,
                      logo=dict(word="VELOCE", sub="CAFFÈ  ESPRESSO", fg=(200, 20, 30), sub_fg=(15, 40, 110),
                                face="bolditalic", plate=None)),
    "volt": dict(name="Kraken Volt", paint=(0.80, 0.72, 0.00), accent=(0.012, 0.012, 0.014), metallic=0.20,
                 logo=dict(word="KRAKEN", sub="ENERGY  DRINK", fg=(250, 230, 20), sub_fg=(250, 230, 20),
                           face="bolditalic", plate=(12, 12, 14))),
    "midnight": dict(name="Nebula Midnight", paint=(0.07, 0.02, 0.17), accent=(0.00, 0.58, 0.68), metallic=0.85,
                     logo=dict(word="NEBULA", sub="CLOUD  COMPUTING", fg=(40, 220, 235), sub_fg=(200, 190, 250),
                               face="light", plate=None, track=36)),
    "arctic": dict(name="Polaris Arctic", paint=(0.74, 0.77, 0.81), accent=(0.03, 0.30, 0.62), metallic=0.75,
                   logo=dict(word="POLARIS", sub="TELECOM", fg=(245, 248, 252), sub_fg=(245, 248, 252),
                             face="bold", plate=(10, 70, 150))),
    "lava": dict(name="Magma Lava", paint=(0.80, 0.10, 0.008), accent=(0.030, 0.030, 0.034), metallic=0.40,
                 logo=dict(word="MAGMA", sub="PERFORMANCE  TYRES", fg=(252, 252, 250), sub_fg=(255, 200, 60),
                           face="bolditalic", plate=None)),
    "forest": dict(name="Greenline Forest", paint=(0.008, 0.09, 0.035), accent=(0.78, 0.68, 0.42), metallic=0.60,
                   logo=dict(word="GREENLINE", sub="EST. 1961", fg=(236, 222, 180), sub_fg=(236, 222, 180),
                             face="serif", plate=None, track=10)),
    "flamingo": dict(name="Flamingo Pink", paint=(0.88, 0.16, 0.40), accent=(0.010, 0.020, 0.075), metallic=0.30,
                     logo=dict(word="FLAMINGO", sub="PAY", fg=(252, 250, 250), sub_fg=(252, 250, 250),
                               face="bold", plate=(18, 30, 80))),
    "tide": dict(name="Tide Sunset", paint=(0.00, 0.30, 0.32), accent=(0.95, 0.40, 0.02), metallic=0.50,
                 logo=dict(word="TIDE", sub="SURF  &  TRAVEL", fg=(255, 150, 30), sub_fg=(235, 245, 245),
                           face="bolditalic", plate=None, track=20)),
    "gunmetal": dict(name="Helix Gunmetal", paint=(0.09, 0.10, 0.11), accent=(0.42, 0.82, 0.02), metallic=0.90,
                     logo=dict(word="HELIX", sub="BIOTECH LABS", fg=(170, 245, 40), sub_fg=(210, 214, 220),
                               face="medium", plate=None, track=30)),
    "royal": dict(name="Crown Royal", paint=(0.015, 0.07, 0.42), accent=(0.88, 0.66, 0.02), metallic=0.65,
                  logo=dict(word="CROWN", sub="AIRWAYS", fg=(20, 40, 120), sub_fg=(20, 40, 120),
                            face="serif", plate=(245, 200, 40), track=12)),
}

# Three per car, none a near copy of the car's own colours.
CARS = {
    "posh-gt3rs": ["heritage", "flamingo", "volt"],
    "limbotiti-caravan-gt3": ["blackgold", "tide", "arctic"],
    "murcetes-amd-gt3": ["lava", "royal", "midnight"],
    "yotota-lmp2": ["royal", "blackgold", "tide"],
    "posh-lmp2": ["heritage", "lava", "forest"],
    "fugazzi-lmp2": ["arctic", "blackgold", "volt"],
    "jeanetti-lmp2": ["flamingo", "gunmetal", "tricolore"],
    "panini-zomba-hypercar": ["tricolore", "lava", "gunmetal"],
    "fugazzi-994p-hypercar": ["blackgold", "arctic", "midnight"],
    "bugotti-chiffon-hypercar": ["forest", "blackgold", "flamingo"],
    "fugazzi-sf26": ["arctic", "blackgold", "royal"],
    "murcetes-amd-w17": ["midnight", "volt", "tide"],
    "mclarsen-mcl40": ["heritage", "gunmetal", "tricolore"],
    "ashton-marvin-amr26": ["lava", "flamingo", "royal"],
}


def draw_logo(spec, path):
    """1536 x 512 RGBA, like the works logos: the word, a rule, the sub line."""
    W, H = 1536, 512
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    word, track = spec["word"], spec.get("track", 0)
    size = 250
    while True:                                    # fit the word inside the plate
        f = font(spec["face"], size)
        widths = [d.textlength(ch, font=f) for ch in word]
        tw = sum(widths) + track * (len(word) - 1)
        if tw <= W - 260 or size <= 90:
            break
        size -= 10
    if spec.get("plate"):
        d.rounded_rectangle((40, 40, W - 40, H - 40), radius=70, fill=tuple(spec["plate"]) + (255,))
    x, y = (W - tw) / 2, 70 + (250 - size) * 0.55
    for ch, w in zip(word, widths):
        d.text((x, y), ch, font=f, fill=tuple(spec["fg"]) + (255,))
        x += w + track
    d.polygon([(190, 372), (W - 170, 372), (W - 182, 384), (178, 384)], fill=tuple(spec["sub_fg"]) + (255,))
    fs = font("medium", 60)
    sub = spec["sub"]
    sw = sum(d.textlength(ch, font=fs) for ch in sub) + 16 * (len(sub) - 1)
    x = (W - sw) / 2
    for ch in sub:
        d.text((x, 402), ch, font=fs, fill=tuple(spec["sub_fg"]) + (255,))
        x += d.textlength(ch, font=fs) + 16
    im.save(path)


def fmt(rgb):
    return "[%s]" % ", ".join("%.3f" % c for c in rgb)


def write_car(folder):
    car_dir = os.path.join(HERE, folder)
    toml = os.path.join(car_dir, "car.toml")
    with open(toml, encoding="utf-8", newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    text = text.replace("\r\n", "\n")
    if MARKER in text:
        text = text[:text.index(MARKER)]
    text = text.rstrip("\n") + "\n\n"
    os.makedirs(os.path.join(car_dir, "textures"), exist_ok=True)
    blocks = [MARKER, "# Livery 0 is the model as authored; these are 1, 2, 3 on the wire.", ""]
    for key in CARS[folder]:
        s = SCHEMES[key]
        logo = "textures/livery_%s.png" % key
        draw_logo(s["logo"], os.path.join(car_dir, logo))
        blocks += ["[[livery]]", 'name = "%s"' % s["name"], "paint = %s" % fmt(s["paint"]),
                   "accent = %s" % fmt(s["accent"]), "metallic = %.2f" % s["metallic"],
                   'logo = "%s"' % logo, ""]
    text += "\n".join(blocks)
    with open(toml, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\n", nl))
    return len(CARS[folder])


if __name__ == "__main__":
    wanted = sys.argv[1:] or list(CARS)
    for folder in wanted:
        if folder not in CARS:
            sys.exit("no liveries defined for %s" % folder)
        print(folder, write_car(folder), "liveries")
