"""Paint the rally cars' liveries as skins: one PNG per scheme in the
projected layout of skin_layout.py, drawn in metres on the car's flanks,
plan and ends from the shape data in rally_cars.py.

Plain Python and Pillow; `liveries.py` calls `paint()` for every rally car
and writes the `[[livery]]` tables, and the works skin it draws is what
build_rally.py puts in the GLB. Every name here is invented
(docs/naming.md).

A scheme is a design (how the colours are laid on: a sweep, stripes,
chevrons, a split, a solid car) and the colours and sponsors to lay on it.
Every scheme also carries what every rally car wore in period: a number on
a white roundel on the front doors, the event's plate on the bonnet and the
boot, a banner across the top of the screen, the maker over the sills and
the crew's names under the side windows.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

import rally_cars
import skin_layout as SL

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
FONT_DIRS = [os.environ.get("APEX_FONTS", ""), os.path.join(ROOT, "cars", "_fonts"),
             r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/liberation2"]
FONTS = {
    "bold": ["Poppins-Bold.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"],
    "bolditalic": ["Poppins-BoldItalic.ttf", "arialbi.ttf", "DejaVuSans-BoldOblique.ttf"],
    "black": ["Poppins-Black.ttf", "ariblk.ttf", "Poppins-Bold.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"],
    "medium": ["Poppins-Medium.ttf", "arial.ttf", "DejaVuSans.ttf"],
    "serif": ["Lora-Variable.ttf", "georgiab.ttf", "DejaVuSerif-Bold.ttf"],
}

EVENT = "RALLYE DES CIMES"           # the plate on the bonnet: an invented event


def font(kind, size):
    for name in FONTS[kind]:
        for d in FONT_DIRS:
            path = os.path.join(d, name)
            if d and os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def srgb(c):
    """Linear 0..1 RGB -> sRGB 0..255 (the schemes are written in linear,
    like the car.toml colours)."""
    def one(v):
        v = min(max(v, 0.0), 1.0)
        v = 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
        return int(round(v * 255))
    return tuple(one(v) for v in c)


WHITE = (0.86, 0.86, 0.84)
BLACK = (0.012, 0.012, 0.014)


# ------------------------------------------------------------------ the sheet
class Sheet:
    """A skin being painted: drawing calls take metres in the build frame
    and land on every region that shows that part of the car."""

    def __init__(self, car, base, size=SL.SIZE):
        self.car = car
        self.box = car["skin_box"]
        self.size = size
        self.im = Image.new("RGB", (size, size), srgb(base))
        self.d = ImageDraw.Draw(self.im)

    def px(self, region, x, y, z):
        return SL.to_px(region, *SL.local(region, self.box, x, y, z), size=self.size)

    # ---- shapes
    def side(self, pts_yz, fill):
        """A polygon on both flanks, (y, z) metres."""
        for region in ("left", "right"):
            self.d.polygon([self.px(region, 0.0, y, z) for (y, z) in pts_yz], fill=srgb(fill))

    def top(self, pts_yx, fill):
        """A polygon on the plan, (y, x) metres, x signed (+X the car's left)."""
        self.d.polygon([self.px("top", x, y, 0.0) for (y, x) in pts_yx], fill=srgb(fill))

    def end(self, region, pts_xz, fill):
        self.d.polygon([self.px(region, x, 0.0, z) for (x, z) in pts_xz], fill=srgb(fill))

    def band(self, z0, z1, fill, y0=None, y1=None, ends=True):
        """A band round the car between two heights (a flank stripe that
        carries on across the nose and the tail)."""
        y0 = self.box[0] - 0.1 if y0 is None else y0
        y1 = self.box[1] + 0.1 if y1 is None else y1
        self.side([(y0, z0), (y1, z0), (y1, z1), (y0, z1)], fill)
        if ends:
            hw = self.box[2] + 0.1
            for region in ("front", "rear"):
                self.end(region, [(-hw, z0), (hw, z0), (hw, z1), (-hw, z1)], fill)

    def ellipse_side(self, yc, zc, r, fill, outline=None, width=0):
        for region in ("left", "right"):
            a = self.px(region, 0.0, yc - r, zc - r)
            b = self.px(region, 0.0, yc + r, zc + r)
            box = (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1]))
            self.d.ellipse(box, fill=srgb(fill), outline=srgb(outline) if outline else None, width=width)

    # ---- text, fitted to a box in metres (each region's pixels are not
    # square, so the words are set at the box's own aspect and then sized)
    def _text_image(self, text, w_px, h_px, fg, face, bg=None, pad=0.10, italic_bg=False):
        scale = 4
        W, H = max(8, int(w_px * scale)), max(8, int(h_px * scale))
        im = Image.new("RGBA", (W, H), srgb(bg) + (255,) if bg else (0, 0, 0, 0))
        dd = ImageDraw.Draw(im)
        size = int(H * (1 - 2 * pad))
        while size > 6:
            f = font(face, size)
            l, t, r, b = dd.textbbox((0, 0), text, font=f)
            if r - l <= W * (1 - pad) and b - t <= H * (1 - 2 * pad):
                break
            size = int(size * 0.93)
        dd.text(((W - (r - l)) / 2 - l, (H - (b - t)) / 2 - t), text, font=f, fill=srgb(fg) + (255,))
        return im

    def _paste(self, im, box):
        x0, y0, x1, y1 = [int(round(v)) for v in box]
        if x1 - x0 < 2 or y1 - y0 < 2:
            return
        im = im.resize((x1 - x0, y1 - y0), Image.LANCZOS)
        self.im.paste(im, (x0, y0), im)

    def side_text(self, text, y0, y1, z0, z1, fg, face="bold", bg=None):
        # set on a box as long as it is on the car: metres -> a square-ish
        # proxy, then resized onto each flank's pixels
        for region in ("left", "right"):
            a = self.px(region, 0.0, y0, z1)
            b = self.px(region, 0.0, y1, z0)
            box = (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1]))
            im = self._text_image(text, (y1 - y0) * 400, (z1 - z0) * 400, fg, face, bg)
            self._paste(im, box)

    def top_text(self, text, y0, y1, x0, x1, fg, face="bold", bg=None):
        """Text on the plan reading from ahead of the car (the way a bonnet
        is read standing at the nose): along +X, letters' tops to the tail."""
        im = self._text_image(text, (x1 - x0) * 400, (y1 - y0) * 400, fg, face, bg)
        im = im.rotate(-90, expand=True)
        a = self.px("top", x1, y0, 0.0)
        b = self.px("top", x0, y1, 0.0)
        self._paste(im, (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])))

    def end_text(self, region, text, x0, x1, z0, z1, fg, face="bold", bg=None):
        im = self._text_image(text, (x1 - x0) * 400, (z1 - z0) * 400, fg, face, bg)
        a = self.px(region, x0, 0.0, z1)
        b = self.px(region, x1, 0.0, z0)
        self._paste(im, (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])))

    def save(self, path):
        self.im.save(path, optimize=True)


# ------------------------------------------------------------------ anchors
def anchors(car):
    """Where things go on this car: the door, the clear flank between the
    arches, the bonnet, the screen top, the sill and the belt."""
    ax_f, ax_r = car["axles"]
    t_f, t_r = car["tyres"]
    sides = sorted(y for (y, k) in car["shutlines"] if k == "side")
    d0, d1 = sides[0], sides[-1]
    return dict(
        nose=car["skin_box"][0], tail=car["skin_box"][1], hw=car["skin_box"][2], h=car["skin_box"][3],
        arch_f=(ax_f - t_f - 0.06, ax_f + t_f + 0.06), arch_r=(ax_r - t_r - 0.06, ax_r + t_r + 0.06),
        door=(d0, d1), sill=car["sill_z"], belt=car["belt_z"],
        bonnet=(car["skin_box"][0] + 0.12, car["screen_y"][0] - 0.06),
        screen_top=car["screen_y"][1], roof=(car["screen_y"][1], car["rear_glass_y"][0]),
        deck=(car["rear_glass_y"][1] + 0.05, car["skin_box"][1] - 0.08),
    )


# ------------------------------------------------------------------ designs
def design_sweep(s, A, c):
    """A broad band low on the flank that sweeps up over the rear wheel,
    edged by two pinstripes and carried across the nose."""
    lo, hi = A["sill"] + 0.02, A["sill"] + 0.20
    rise = 0.26
    y_up = A["arch_r"][0] - 0.30
    band = [(A["nose"] - 0.1, lo), (y_up, lo), (A["tail"] + 0.1, lo + rise), (A["tail"] + 0.1, hi + rise),
            (y_up, hi), (A["nose"] - 0.1, hi)]
    s.side(band, c["c1"])
    for dz, col in ((0.03, c["c2"]), (-0.03, c["c3"])):
        edge = hi if dz > 0 else lo
        strip = [(A["nose"] - 0.1, edge + dz * 0.4), (y_up, edge + dz * 0.4), (A["tail"] + 0.1, edge + rise + dz * 0.4),
                 (A["tail"] + 0.1, edge + rise + dz * 1.3), (y_up, edge + dz * 1.3), (A["nose"] - 0.1, edge + dz * 1.3)]
        s.side(strip, col)
    hw = A["hw"]
    s.end("front", [(-hw, lo), (hw, lo), (hw, hi), (-hw, hi)], c["c1"])


def design_stripes(s, A, c):
    """Three stripes along the car at the waist, over the nose and the
    tail, and three down the bonnet, the roof and the deck off-centre."""
    z = A["sill"] + 0.17
    for k, col in enumerate((c["c1"], c["c2"], c["c3"])):
        s.band(z + k * 0.07, z + k * 0.07 + 0.055, col)
    hw = A["hw"]
    for k, col in enumerate((c["c1"], c["c2"], c["c3"])):
        x0 = hw * 0.16 + k * 0.10
        s.top([(A["nose"] - 0.1, x0), (A["tail"] + 0.1, x0), (A["tail"] + 0.1, x0 + 0.075),
               (A["nose"] - 0.1, x0 + 0.075)], col)


def design_chevron(s, A, c):
    """Three diagonal bands across the doors and the rear quarter, leaning
    back, and a stripe of each down the bonnet."""
    d0, d1 = A["door"]
    lo, hi = A["sill"] - 0.15, A["belt"] + 0.40
    lean = (hi - lo) * 0.55
    w = 0.14
    for k, col in enumerate((c["c1"], c["c2"], c["c3"])):
        y = d0 + 0.45 + k * (w + 0.03)
        s.side([(y, lo), (y + w, lo), (y + w + lean, hi), (y + lean, hi)], col)
    hw = A["hw"]
    for k, col in enumerate((c["c1"], c["c2"], c["c3"])):
        x0 = -0.17 + k * 0.12
        s.top([(A["nose"] - 0.1, x0), (A["deck"][1] + 0.2, x0), (A["deck"][1] + 0.2, x0 + 0.10),
               (A["nose"] - 0.1, x0 + 0.10)], col)


def design_split(s, A, c):
    """The lower body one colour, the upper the base, a fine stripe where
    they meet and another along the sill."""
    z = A["sill"] + 0.24
    s.band(-0.1, z, c["c1"])
    s.band(z, z + 0.022, c["c2"])
    s.band(A["sill"] - 0.02, A["sill"], c["c2"])
    hw = A["hw"]
    s.top([(A["nose"] - 0.1, -hw - 0.1), (A["bonnet"][0] + 0.30, -hw - 0.1),
           (A["bonnet"][0] + 0.30, hw + 0.1), (A["nose"] - 0.1, hw + 0.1)], c["c1"])


def design_solid(s, A, c):
    """One colour all over, a contrasting roof and a pinstripe along the
    waist."""
    s.band(A["sill"] + 0.30, A["sill"] + 0.325, c["c2"])
    hw = A["hw"]
    r0, r1 = A["roof"]
    s.top([(r0 + 0.05, -hw * 0.7), (r1, -hw * 0.7), (r1, hw * 0.7), (r0 + 0.05, hw * 0.7)], c["c1"])


DESIGNS = dict(sweep=design_sweep, stripes=design_stripes, chevron=design_chevron,
               split=design_split, solid=design_solid)


# ------------------------------------------------------------------ the kit
def furniture(s, A, car, sc):
    """What every scheme carries: numbers, plates, banner, maker, crew."""
    fg, panel = sc.get("text", BLACK), sc.get("panel", WHITE)
    d0, d1 = A["door"]
    zc = (A["sill"] + A["belt"]) / 2 + 0.03
    # the door number on a white roundel, high on the door and behind the
    # door mirror (build_rally.py puts it 0.30 m behind the screen's foot),
    # which hid it from the side
    r = min(0.14, (A["belt"] - A["sill"]) * 0.26)
    zr = A["belt"] - 0.06 - r
    yc = max(d0 + r + 0.06, car["screen_y"][0] + 0.30 + 0.12 + r)
    s.ellipse_side(yc, zr, r, WHITE, outline=BLACK, width=6)
    s.side_text(car["number"], yc - r * 0.75, yc + r * 0.75, zr - r * 0.70, zr + r * 0.70, BLACK, face="black")
    # the main sponsor behind the roundel when the door is long enough,
    # else across the door under it
    y0 = yc + r + 0.05
    if d1 - 0.04 - y0 >= 0.40:
        box = (y0, d1 - 0.04, zr - 0.09, zr + 0.09)
    else:
        top = zr - r - 0.03
        box = (d0 + 0.06, d1 - 0.06, max(A["sill"] + 0.13, top - 0.16), top)
    s.side_text(sc["sponsor"], *box, sc.get("sponsor_fg", fg), face=sc.get("face", "bolditalic"),
                bg=sc.get("sponsor_bg"))
    # the crew's names under the side glass, behind the roundel
    if d1 - 0.04 - y0 > 0.20:
        s.side_text(car["crew"], y0, d1 - 0.04, A["belt"] - 0.07, A["belt"] - 0.025, fg, face="medium")
    # the second sponsor low on the rear quarter (under its intake), the
    # third ahead of the door
    q0, q1 = d1 + 0.06, A["arch_r"][0] - 0.02
    if q1 - q0 > 0.22:
        s.side_text(sc["sponsor2"], q0, q1, A["sill"] + 0.02, A["sill"] + 0.11, fg, face="bold")
    f0, f1 = A["arch_f"][1] + 0.02, d0 - 0.04
    if f1 - f0 > 0.18:
        s.side_text(sc.get("sponsor3", "APEX"), f0, f1, zc + 0.02, zc + 0.10, fg, face="bold")
    # the maker over the sills
    s.side_text(car["maker"], d0 + 0.08, d1 - 0.08, A["sill"] + 0.035, A["sill"] + 0.105, fg, face="black")
    hw = A["hw"]
    # the event plate on the bonnet's leading edge, and the rear plate
    b0, b1 = A["bonnet"]
    plate_y = (b0 + 0.04, min(b0 + 0.34, b1 - 0.05))
    s.top_text(EVENT, plate_y[0], plate_y[1], -hw * 0.55, hw * 0.55, BLACK, face="bold", bg=WHITE)
    # the bonnet sponsor
    if b1 - plate_y[1] > 0.30:
        s.top_text(sc["sponsor"], plate_y[1] + 0.06, b1 - 0.06, -hw * 0.62, hw * 0.62,
                   sc.get("sponsor_fg", fg), face=sc.get("face", "bolditalic"), bg=sc.get("sponsor_bg"))
    # the screen banner: the maker on the sun strip
    st = A["screen_top"]
    s.top([(st - 0.16, -hw), (st + 0.02, -hw), (st + 0.02, hw), (st - 0.16, hw)], sc.get("banner", BLACK))
    s.top_text(car["maker"], st - 0.15, st + 0.01, -hw * 0.62, hw * 0.62, sc.get("banner_fg", WHITE), face="black")
    # the ends: the event plate on the tail, the maker across the nose
    tz = car["ends"]["tail_bot"] + 0.20
    s.end_text("rear", EVENT, -0.36, 0.36, tz, tz + 0.11, BLACK, face="bold", bg=WHITE)
    nz = car["ends"]["nose_top"] - 0.20
    s.end_text("front", car["maker"], -0.30, 0.30, nz - 0.02, nz + 0.05, fg, face="black")


# ------------------------------------------------------------------ schemes
# c1..c3: the design's colours; paint: the mirrors and the driver's helmet;
# accent: the wing, the scoop and the mud flaps. Linear RGB.
SCHEMES = {
    # the works schemes, one per car (livery 0, the GLB's own skin)
    "works_oudi": dict(name="Kwattro Works", design="sweep", base=WHITE,
                       c1=(0.62, 0.015, 0.02), c2=(0.80, 0.55, 0.04), c3=BLACK,
                       sponsor="HALBERD", sponsor2="OUDI SPORT", sponsor3="KÖNIGSRING",
                       paint=WHITE, accent=BLACK),
    "works_pugeot": dict(name="Talon Sport", design="chevron", base=WHITE,
                         c1=(0.02, 0.10, 0.55), c2=(0.85, 0.62, 0.02), c3=(0.70, 0.03, 0.03),
                         sponsor="TALON SPORT", sponsor2="MIROIR", sponsor3="VELO",
                         paint=WHITE, accent=(0.02, 0.10, 0.55)),
    "works_lancha": dict(name="Martello Corse", design="stripes", base=WHITE,
                         c1=(0.010, 0.04, 0.22), c2=(0.0, 0.40, 0.48), c3=(0.75, 0.10, 0.02),
                         sponsor="MARTELLO", sponsor2="APERITIVO", sponsor3="CORSE",
                         face="serif", paint=WHITE, accent=(0.010, 0.04, 0.22)),
    # the field
    "zenith": dict(name="Zenith Nine", design="sweep", base=(0.012, 0.06, 0.40),
                   c1=(0.85, 0.62, 0.02), c2=WHITE, c3=(0.85, 0.62, 0.02),
                   sponsor="ZENITH 9", sponsor2="RADIO", sponsor3="Z9", text=WHITE,
                   sponsor_fg=(0.85, 0.62, 0.02), banner=(0.85, 0.62, 0.02), banner_fg=(0.012, 0.06, 0.40),
                   paint=(0.012, 0.06, 0.40), accent=(0.85, 0.62, 0.02)),
    "hirsch": dict(name="Hirschbrand Orange", design="solid", base=(0.85, 0.20, 0.01),
                   c1=WHITE, c2=WHITE, c3=WHITE,
                   sponsor="HIRSCHBRAND", sponsor2="KRÄUTERLIKÖR", sponsor3="HB", text=WHITE,
                   face="serif", paint=(0.85, 0.20, 0.01), accent=BLACK),
    "harbour": dict(name="Royal Harbour", design="split", base=WHITE,
                    c1=(0.010, 0.035, 0.16), c2=(0.70, 0.50, 0.12), c3=WHITE,
                    sponsor="ROYAL HARBOUR", sponsor2="SHIPPING", sponsor3="RH",
                    sponsor_fg=(0.010, 0.035, 0.16), paint=(0.010, 0.035, 0.16), accent=(0.010, 0.035, 0.16),
                    banner=(0.010, 0.035, 0.16)),
    "sable": dict(name="Sable & Gold", design="solid", base=(0.012, 0.012, 0.014),
                  c1=(0.012, 0.012, 0.014), c2=(0.70, 0.50, 0.12), c3=(0.70, 0.50, 0.12),
                  sponsor="SABLE & GOLD", sponsor2="FINE GOODS", sponsor3="S&G", text=(0.70, 0.50, 0.12),
                  face="serif", banner=(0.70, 0.50, 0.12), banner_fg=BLACK,
                  paint=(0.012, 0.012, 0.014), accent=(0.70, 0.50, 0.12)),
    "totop": dict(name="Totop Yellow", design="split", base=(0.90, 0.68, 0.01),
                  c1=(0.010, 0.10, 0.48), c2=WHITE, c3=WHITE,
                  sponsor="TOTOP", sponsor2="LOTTERIA", sponsor3="T", text=(0.010, 0.10, 0.48),
                  banner=(0.010, 0.10, 0.48), paint=(0.90, 0.68, 0.01), accent=(0.010, 0.10, 0.48)),
    "ostra": dict(name="Ostra Oil", design="chevron", base=WHITE,
                  c1=(0.70, 0.03, 0.03), c2=(0.02, 0.10, 0.55), c3=(0.70, 0.03, 0.03),
                  sponsor="OSTRA", sponsor2="MOTOR OIL", sponsor3="OSTRA",
                  sponsor_fg=(0.70, 0.03, 0.03), paint=WHITE, accent=(0.70, 0.03, 0.03)),
    "alpen": dict(name="Alpenklub", design="stripes", base=(0.66, 0.62, 0.48),
                  c1=(0.01, 0.18, 0.06), c2=(0.01, 0.18, 0.06), c3=(0.60, 0.04, 0.03),
                  sponsor="ALPENKLUB", sponsor2="BERGSPORT", sponsor3="AK",
                  sponsor_fg=(0.01, 0.18, 0.06), face="serif", banner=(0.01, 0.18, 0.06),
                  paint=(0.01, 0.18, 0.06), accent=(0.01, 0.18, 0.06)),
    "fjord": dict(name="Fjord Airways", design="sweep", base=WHITE,
                  c1=(0.02, 0.30, 0.70), c2=(0.70, 0.03, 0.03), c3=(0.02, 0.30, 0.70),
                  sponsor="FJORD", sponsor2="AIRWAYS", sponsor3="FJ",
                  sponsor_fg=(0.02, 0.30, 0.70), banner=(0.02, 0.30, 0.70),
                  paint=WHITE, accent=(0.02, 0.30, 0.70)),
    "bordeaux": dict(name="Château Rouge", design="split", base=(0.80, 0.74, 0.58),
                     c1=(0.22, 0.010, 0.025), c2=(0.70, 0.50, 0.12), c3=WHITE,
                     sponsor="CHÂTEAU ROUGE", sponsor2="GRAND CRU", sponsor3="CR",
                     sponsor_fg=(0.22, 0.010, 0.025), face="serif", banner=(0.22, 0.010, 0.025),
                     paint=(0.22, 0.010, 0.025), accent=(0.22, 0.010, 0.025)),
    "glacier": dict(name="Glacier Blue", design="stripes", base=(0.30, 0.55, 0.75),
                    c1=WHITE, c2=(0.010, 0.04, 0.22), c3=WHITE,
                    sponsor="GLETSCHER", sponsor2="EAU MINÉRALE", sponsor3="GL",
                    text=WHITE, banner=(0.010, 0.04, 0.22), paint=(0.30, 0.55, 0.75), accent=WHITE),
}

CARS = {
    "oudi": dict(works="works_oudi", maker="OUDI", crew="S. HALVORSEN · E. NYBERG",
                 extra=["zenith", "fjord", "harbour", "hirsch", "sable"]),
    "pugeot": dict(works="works_pugeot", maker="PUGEOT", crew="J. MOREAU · C. DELAGE",
                   extra=["ostra", "totop", "glacier", "alpen", "bordeaux"]),
    "lancha": dict(works="works_lancha", maker="LANCHA", crew="R. BRUNETTI · F. CASATI",
                   extra=["sable", "totop", "hirsch", "zenith", "glacier"]),
}


def paint(variant, key, path):
    """Draw scheme `key` for the rally car `variant` into `path`."""
    car = dict(rally_cars.VARIANTS[variant])
    car.update(maker=CARS[variant]["maker"], crew=CARS[variant]["crew"])
    sc = SCHEMES[key]
    s = Sheet(car, sc["base"])
    A = anchors(car)
    DESIGNS[sc["design"]](s, A, sc)
    furniture(s, A, car, sc)
    s.save(path)
    return path


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for v, spec in CARS.items():
        for key in [spec["works"]] + spec["extra"]:
            print(paint(v, key, os.path.join(out, "%s_%s.png" % (v, key))))
