"""Paint the crowd card textures: seated spectators for the stands' `_crowd`
twins, standing spectators and marshals for the misc figure kit.

    python tools/generators/props/gen_crowd.py            (from apexsim-content/)

Writes:
  props/grandstand/T_crowd.png   2048 x 256: one row of seated spectators,
      8 m wide and 1.15 m tall, wrapping in u. A stand's crowd card is a
      4.4 x 1.15 m strip showing a 0.55-wide window of it at its own offset,
      so the scale (256 px a metre across) is fixed by the meshes.
  props/misc/T_crowd_standing.png  2048 x 512: the figure kit's sheet.
      Top half (v 0.5..1): a standing crowd, 8 m x 1.9 m, wrapping in u, for
      crowd_bank_10m. Bottom half: CELLS of loose groups (crowd_group) and
      single marshals in orange overalls (marshal_figure); `SHEET_CELLS`
      gives each cell's u range, which the mesh builder reads.

Figures are drawn flat from the front at 4x and filtered down; RGB is bled
out under the transparent texels so the mips do not pull a dark fringe in.
Seeded: the same run paints the same crowd.
"""
import math, os, random
import numpy as np

# build_figures.py reads SHEET_CELLS from inside Blender, whose Python has no
# PIL; only painting needs it.
try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = ImageDraw = None

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SS = 4  # supersampling

SKIN = [(245, 208, 175), (234, 190, 150), (214, 160, 120), (190, 135, 95), (150, 100, 66),
        (112, 74, 48), (86, 56, 38), (250, 220, 195)]
HAIR = [(28, 22, 18), (45, 32, 22), (70, 48, 30), (110, 78, 45), (180, 140, 80), (210, 180, 120),
        (120, 120, 118), (200, 200, 196), (40, 40, 40)]
# Shirts: mostly what people wear, a share of team colours.
SHIRT_PLAIN = [(24, 24, 26), (36, 40, 60), (60, 62, 66), (120, 122, 126), (230, 230, 228), (200, 196, 186),
               (52, 72, 110), (90, 110, 140), (70, 60, 50), (110, 40, 40), (40, 70, 50), (160, 150, 120)]
SHIRT_TEAM = [(200, 20, 24), (255, 130, 0), (0, 150, 160), (250, 205, 0), (0, 110, 60), (60, 140, 220),
              (20, 30, 110), (220, 220, 220), (130, 0, 40), (0, 60, 40)]
CAP = [(200, 20, 24), (255, 130, 0), (20, 30, 110), (250, 205, 0), (24, 24, 26), (230, 230, 228), (0, 150, 160)]
FLAGS = [((174, 28, 40), (255, 255, 255), (33, 70, 139), 'h'),   # tricolours, horizontal
         ((0, 0, 0), (221, 0, 0), (255, 206, 0), 'h'),
         ((0, 146, 70), (255, 255, 255), (206, 43, 55), 'v'),
         ((0, 85, 164), (255, 255, 255), (239, 65, 53), 'v'),
         ((255, 255, 255), (200, 16, 46), (255, 255, 255), 'h'),
         ((0, 106, 167), (254, 204, 0), (0, 106, 167), 'h'),
         ((255, 140, 0), (255, 255, 255), (255, 140, 0), 'h')]
ORANGE = (255, 110, 10)


class Canvas:
    """A wrapping RGBA canvas in metres: x across (wraps at width_m), z up
    from the card's bottom edge."""

    def __init__(self, w_px, h_px, width_m, height_m, wrap=True):
        self.w, self.h = w_px * SS, h_px * SS
        self.sx, self.sz = self.w / width_m, self.h / height_m
        self.width_m, self.wrap = width_m, wrap
        self.img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        self.x0 = 0.0   # x offset for a sub-canvas (sheet cells)

    def _pts(self, pts, dx):
        return [((x + dx + self.x0) * self.sx, self.h - z * self.sz) for (x, z) in pts]

    def _offsets(self):
        return (-self.width_m, 0.0, self.width_m) if self.wrap else (0.0,)

    def poly(self, pts, col):
        for dx in self._offsets():
            self.d.polygon(self._pts(pts, dx), fill=col + (255,))

    def ellipse(self, cx, cz, rx, rz, col):
        for dx in self._offsets():
            (x0, z1), (x1, z0) = self._pts([(cx - rx, cz - rz), (cx + rx, cz + rz)], dx)
            self.d.ellipse([x0, z1 if z1 < z0 else z0, x1, z0 if z1 < z0 else z1], fill=col + (255,))

    def rect(self, x0, z0, x1, z1, col):
        self.poly([(x0, z0), (x1, z0), (x1, z1), (x0, z1)], col)

    def line(self, x0, z0, x1, z1, w, col):
        """A thick segment (w metres) as a quad with round ends."""
        dx, dz = x1 - x0, z1 - z0
        L = math.hypot(dx, dz) or 1e-6
        nx, nz = -dz / L * w / 2, dx / L * w / 2
        self.poly([(x0 + nx, z0 + nz), (x1 + nx, z1 + nz), (x1 - nx, z1 - nz), (x0 - nx, z0 - nz)], col)
        self.ellipse(x0, z0, w / 2, w / 2, col)
        self.ellipse(x1, z1, w / 2, w / 2, col)

    def finish(self):
        """Downsample, then bleed colour under alpha (and tint it by height:
        lower down sits in the row in front's shadow)."""
        img = self.img.resize((self.w // SS, self.h // SS), Image.LANCZOS)
        a = np.asarray(img).astype(np.float32) / 255.0
        rgb, al = a[..., :3], a[..., 3:4]
        h = rgb.shape[0]
        shade = np.linspace(1.0, 0.72, h).reshape(h, 1, 1)     # top row lit, bottom in shadow
        rgb = rgb * shade
        # bleed: blur the premultiplied colour and alpha, use it where alpha is 0
        pre = np.concatenate([rgb * (al > 0.02), (al > 0.02).astype(np.float32)], -1)
        acc = pre.copy()
        for _ in range(6):
            acc = (acc + np.roll(acc, 1, 0) + np.roll(acc, -1, 0) + np.roll(acc, 1, 1) + np.roll(acc, -1, 1)) / 5
            acc = np.where(pre[..., 3:4] > 0, pre, acc)
        fill = acc[..., :3] / np.maximum(acc[..., 3:4], 1e-4)
        mean = rgb[al[..., 0] > 0.5].mean(0) if (al > 0.5).any() else np.array([0.3, 0.3, 0.3])
        fill = np.where(acc[..., 3:4] > 1e-4, fill, mean)
        rgb = np.where(al > 0.02, rgb, fill)
        out = np.concatenate([np.clip(rgb, 0, 1), np.clip(al, 0, 1)], -1)
        return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA")


def darker(c, k=0.75):
    return tuple(int(v * k) for v in c)


def head(cv, rng, cx, cz, scale=1.0, style=None):
    """Head with hair or headwear; cz is the chin. Returns the crown height."""
    skin = rng.choice(SKIN)
    hair = rng.choice(HAIR)
    hw, hh = 0.085 * scale, 0.115 * scale
    top = cz + 2 * hh
    style = style or rng.choices(["short", "long", "cap", "bald", "hat", "beanie", "ears"],
                                 [30, 18, 22, 5, 6, 6, 13])[0]
    if style == "long":
        cv.poly([(cx - hw * 1.15, cz + hh * 1.3), (cx + hw * 1.15, cz + hh * 1.3),
                 (cx + hw * 1.25, cz - 0.10 * scale), (cx - hw * 1.25, cz - 0.10 * scale)], hair)
    cv.rect(cx - 0.035 * scale, cz - 0.04 * scale, cx + 0.035 * scale, cz + 0.05 * scale, darker(skin, 0.85))  # neck
    cv.ellipse(cx, cz + hh, hw, hh, skin)
    if style in ("short", "long", "ears"):
        cv.ellipse(cx, cz + hh * 1.35, hw * 1.04, hh * 0.72, hair)
        cv.ellipse(cx, cz + hh * 0.95, hw * 0.92, hh * 0.45, skin)   # face back over the hairline
    elif style == "cap":
        c = rng.choice(CAP)
        cv.ellipse(cx, cz + hh * 1.45, hw * 1.08, hh * 0.68, c)
        cv.ellipse(cx, cz + hh * 1.0, hw * 0.9, hh * 0.5, skin)
        cv.rect(cx - hw * 1.1, cz + hh * 1.35, cx + hw * 1.1, cz + hh * 1.5, darker(c, 0.6))  # brim
    elif style == "hat":
        c = rng.choice([(230, 220, 190), (200, 180, 140), (240, 240, 236)])
        cv.ellipse(cx, cz + hh * 1.55, hw * 0.95, hh * 0.6, c)
        cv.rect(cx - hw * 1.8, cz + hh * 1.42, cx + hw * 1.8, cz + hh * 1.55, darker(c, 0.85))
        top = cz + hh * 2.15
    elif style == "beanie":
        c = rng.choice(CAP)
        cv.ellipse(cx, cz + hh * 1.5, hw * 1.05, hh * 0.75, c)
        cv.rect(cx - hw * 1.05, cz + hh * 1.3, cx + hw * 1.05, cz + hh * 1.5, darker(c, 0.8))
    if style == "ears":   # ear defenders: a must at a circuit
        c = rng.choice([(200, 20, 24), (24, 24, 26), (250, 205, 0), (60, 140, 220)])
        cv.rect(cx - hw * 0.15, top - 0.012, cx + hw * 0.15, top + 0.012, c)
        cv.ellipse(cx - hw * 1.05, cz + hh * 1.0, hw * 0.35, hh * 0.38, c)
        cv.ellipse(cx + hw * 1.05, cz + hh * 1.0, hw * 0.35, hh * 0.38, c)
    if rng.random() < 0.2:   # sunglasses
        for s in (-1, 1):
            cv.ellipse(cx + s * hw * 0.42, cz + hh * 1.12, hw * 0.3, hh * 0.13, (25, 25, 28))
    return top, skin


def torso(cv, rng, cx, z0, shoulder_z, width, shirt, sleeve=None, prints=True):
    sleeve = sleeve or shirt
    w = width / 2
    r = 0.06
    cv.poly([(cx - w * 0.92, z0), (cx + w * 0.92, z0), (cx + w, shoulder_z - r), (cx + w - r * 0.6, shoulder_z),
             (cx - w + r * 0.6, shoulder_z), (cx - w, shoulder_z - r)], shirt)
    # sleeves / arms down the sides, a shade darker so the outline reads
    cv.poly([(cx - w, shoulder_z - r), (cx - w + 0.075, shoulder_z - 0.02), (cx - w + 0.07, z0), (cx - w * 0.98, z0)],
            darker(sleeve, 0.8))
    cv.poly([(cx + w, shoulder_z - r), (cx + w - 0.075, shoulder_z - 0.02), (cx + w - 0.07, z0), (cx + w * 0.98, z0)],
            darker(sleeve, 0.8))
    if not prints:
        pass
    elif rng.random() < 0.3:   # a chest print or zip
        c = rng.choice([(240, 240, 240), (20, 20, 20), (250, 205, 0)])
        cv.rect(cx - 0.06, shoulder_z - 0.2, cx + 0.06, shoulder_z - 0.12, c)
    elif rng.random() < 0.2:
        cv.rect(cx - 0.008, z0, cx + 0.008, shoulder_z - 0.02, darker(shirt, 0.6))


def pose(cv, rng, cx, shoulder_z, width, shirt, skin, kind):
    w = width / 2
    if kind == "cheer":
        for s in (-1, 1):
            hx, hz = cx + s * (w + 0.08), shoulder_z + 0.42
            cv.line(cx + s * (w - 0.04), shoulder_z - 0.03, hx, hz, 0.075, darker(shirt, 0.85))
            cv.ellipse(hx, hz + 0.03, 0.035, 0.04, skin)
    elif kind == "phone":
        s = rng.choice((-1, 1))
        hx, hz = cx + s * 0.05, shoulder_z + 0.12
        cv.line(cx + s * (w - 0.04), shoulder_z - 0.05, hx, hz - 0.04, 0.07, darker(shirt, 0.85))
        cv.rect(hx - 0.035, hz - 0.03, hx + 0.035, hz + 0.1, (18, 18, 22))
    elif kind == "fold":
        cv.rect(cx - w + 0.03, shoulder_z - 0.3, cx + w - 0.03, shoulder_z - 0.22, darker(shirt, 0.7))
    elif kind == "wave":
        s = rng.choice((-1, 1))
        hx, hz = cx + s * (w + 0.15), shoulder_z + 0.3
        cv.line(cx + s * (w - 0.04), shoulder_z - 0.03, hx, hz, 0.075, darker(shirt, 0.85))
        cv.ellipse(hx, hz + 0.03, 0.035, 0.04, skin)


def flag(cv, rng, x, z0, z1, chequer=False):
    """A flag on a pole. The stands' strip shows only a chequered one: every
    card repeats the strip, so a national flag there would fill every stand
    at every circuit with one country."""
    cv.line(x, z0, x, z1, 0.022, (60, 60, 60))
    a, b, c, o = rng.choice(FLAGS)
    fw, fh = 0.42, 0.27
    s = rng.choice((-1, 1))
    xa, xb = (x, x + s * fw)
    lo, hi = min(xa, xb), max(xa, xb)
    zt = z1 - 0.01
    if chequer:
        n, m = 6, 4
        for i in range(n):
            for j in range(m):
                col = (240, 240, 240) if (i + j) % 2 else (20, 20, 20)
                cv.rect(lo + fw * i / n, zt - fh * (j + 1) / m, lo + fw * (i + 1) / n, zt - fh * j / m, col)
    elif o == 'h':
        for i, col in enumerate((a, b, c)):
            cv.rect(lo, zt - fh * (i + 1) / 3, hi, zt - fh * i / 3, col)
    else:
        for i, col in enumerate((a, b, c)):
            cv.rect(lo + fw * i / 3, zt - fh, lo + fw * (i + 1) / 3, zt, col)


def seated_strip(seed=11):
    """One row of seated spectators, 8 m x 1.15 m, card bottom at the seat."""
    rng = random.Random(seed)
    cv = Canvas(2048, 256, 8.0, 1.15)
    pitch = 0.5
    people = []
    for i in range(16):
        if rng.random() < 0.1:
            continue   # an empty seat
        cx = i * pitch + 0.25 + rng.uniform(-0.05, 0.05)
        people.append(cx)
    flags_at = set(rng.sample(range(len(people)), 1))
    for k, cx in enumerate(people):
        sc = rng.uniform(0.9, 1.08)
        shoulder = 0.56 * sc
        width = rng.uniform(0.38, 0.46)
        team = rng.random() < 0.35
        shirt = rng.choice(SHIRT_TEAM if team else SHIRT_PLAIN)
        sleeve = shirt if rng.random() < 0.7 else rng.choice(SHIRT_PLAIN)
        torso(cv, rng, cx, 0.0, shoulder, width, shirt, sleeve)
        top, skin = head(cv, rng, cx, shoulder + 0.02, sc)
        kind = rng.choices(["none", "cheer", "phone", "fold", "wave"], [55, 8, 12, 15, 10])[0]
        if k in flags_at:
            flag(cv, rng, cx + 0.2, 0.25, 1.12, chequer=True)
        else:
            pose(cv, rng, cx, shoulder, width, shirt, skin, kind)
    return cv.finish()


def standing_figure(cv, rng, cx, z0, height, shirt=None, marshal=False):
    """A standing person seen from the front, feet at z0, `height` tall."""
    k = height / 1.75
    skin_dummy = None
    legs = (34, 46, 80) if rng.random() < 0.5 else rng.choice([(30, 30, 32), (90, 84, 70), (60, 70, 90)])
    if marshal:
        shirt, legs = ORANGE, ORANGE
    shirt = shirt or rng.choice(SHIRT_TEAM if rng.random() < 0.35 else SHIRT_PLAIN)
    hip = z0 + 0.88 * k
    shoulder = z0 + 1.43 * k
    width = rng.uniform(0.4, 0.47) * k
    # legs and shoes
    for s in (-1, 1):
        cv.poly([(cx + s * 0.02, hip), (cx + s * 0.17 * k, hip), (cx + s * 0.15 * k, z0 + 0.06),
                 (cx + s * 0.04, z0 + 0.06)], darker(legs, 0.9))
        cv.rect(cx + s * 0.03 - (0.12 if s < 0 else 0) * k, z0, cx + s * 0.03 + (0.12 if s > 0 else 0) * k,
                z0 + 0.07, (30, 30, 30) if not marshal else (40, 40, 40))
    torso(cv, rng, cx, hip - 0.04, shoulder, width, shirt, prints=not marshal)
    if marshal:   # reflective bands and a white helmet-less cap
        cv.rect(cx - width / 2, hip + 0.18 * k, cx + width / 2, hip + 0.23 * k, (230, 230, 220))
        for s in (-1, 1):
            cv.rect(cx + s * 0.02, z0 + 0.35 * k, cx + s * 0.16 * k, z0 + 0.39 * k, (230, 230, 220))
    top, skin = head(cv, rng, cx, shoulder + 0.02, k, style="short" if marshal else None)
    if marshal:
        # a flag held down by the side
        c = rng.choice([(250, 205, 0), (0, 140, 60), (40, 90, 200), (240, 240, 240)])
        px0, pz0 = cx + width / 2 + 0.02, hip + 0.05
        px1, pz1 = cx + width / 2 + 0.12, hip - 0.55
        cv.poly([(px0, pz0), (px0 + 0.28, pz0 - 0.08), (px0 + 0.22, pz0 - 0.36), (px0 + 0.05, pz0 - 0.3)], c)
        cv.line(px0, pz0, px1, pz1, 0.025, (70, 70, 70))
    else:
        kind = rng.choices(["none", "cheer", "phone", "fold", "wave"], [55, 8, 12, 15, 10])[0]
        pose(cv, rng, cx, shoulder, width, shirt, skin, kind)
    return top


def standing_strip(cv, rng, x_from, x_to, rows=((0.0, 1.0), (0.35, 0.86)), density=1.0, flags=1):
    """Standing crowd: back row raised (they stand on a bank), drawn back to
    front so the front row overlaps. rows: (feet z, scale)."""
    for (z0, sc), r in zip(rows[::-1], range(len(rows))):
        x = x_from + rng.uniform(0.0, 0.3)
        while x < x_to - 0.2:
            if rng.random() < 0.92 * density:
                standing_figure(cv, rng, x, z0, rng.uniform(1.6, 1.88) * sc)
            x += rng.uniform(0.42, 0.62) / max(density, 0.3)
    for _ in range(flags):
        flag(cv, rng, rng.uniform(x_from + 0.3, x_to - 0.5), 1.0, 1.88, chequer=True)


# The figure sheet's bottom-half cells: (name, u0, u1). Groups are 2 m wide
# (512 px), marshals 1 m (256 px, room for the flag held at the side).
SHEET_CELLS = [("group_a", 0.0, 0.25), ("group_b", 0.25, 0.5), ("group_c", 0.5, 0.75),
               ("marshal_a", 0.75, 0.875), ("marshal_b", 0.875, 1.0)]


def figure_sheet(seed=23):
    rng = random.Random(seed)
    top = Canvas(2048, 256, 8.0, 1.9)
    standing_strip(top, rng, 0.0, 8.0, rows=((0.0, 1.0),), density=1.1, flags=2)
    top_img = top.finish()
    cells = Image.new("RGBA", (2048, 256), (0, 0, 0, 0))
    for name, u0, u1 in SHEET_CELLS:
        w_m = (u1 - u0) * 8.0
        cv = Canvas(int((u1 - u0) * 2048), 256, w_m, 1.95, wrap=False)
        if name.startswith("group"):
            x = 0.28
            while x < w_m - 0.25:
                standing_figure(cv, rng, x, 0.0, rng.uniform(1.62, 1.9))
                x += rng.uniform(0.38, 0.5)
        else:
            standing_figure(cv, rng, w_m / 2 - 0.2, 0.0, 1.78, marshal=True)
        cells.paste(cv.finish(), (int(u0 * 2048), 0))
    sheet = Image.new("RGBA", (2048, 512), (0, 0, 0, 0))
    sheet.paste(top_img, (0, 0))
    sheet.paste(cells, (0, 256))
    return sheet


if __name__ == "__main__":
    a = seated_strip()
    pa = os.path.join(ROOT, "props", "grandstand", "T_crowd.png")
    a.save(pa)
    b = figure_sheet()
    pb = os.path.join(ROOT, "props", "misc", "T_crowd_standing.png")
    b.save(pb)
    print(pa, a.size)
    print(pb, b.size)
