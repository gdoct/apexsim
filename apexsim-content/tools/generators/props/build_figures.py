"""The people kit (kit gap 1, the People column): a grass spectator bank with
and without its standing crowd, loose groups of standing spectators, and a
marshal. All figures are masked cards from props/misc/T_crowd_standing.png
(gen_crowd.py; run that first), slot `crowd_standing`.

Headless:  blender -b --python tools/generators/props/build_figures.py
(from apexsim-content/). ASSET = "all" or one key. Writes the GLBs, a .blend
per asset and previews in props/_preview/.

- grandstand/grass_bank_10m, grandstand/grass_bank_10m_crowd: a 10 m
  module of turfed bank rising away from the road (pivot on the toe at the
  road-facing edge, the bank reaching +Y), tiled by the grandstand layout
  like scaffold_10m; the client picks the `_crowd` twin when the scene has
  spectators. Turf is the baked `bank_turf` slot (apex_tex.KIT_SLOTS).
- misc/crowd_group: a dozen low-poly standing figures in a loose knot (real
  geometry, not cards: it stands on open ground and is seen from every
  side), for general-admission areas, gates, fan zones and the paddock.
  Centred.
- misc/marshal_figure: one marshal in orange overalls holding a furled flag,
  one card facing the road. Centred on the card's foot.
"""
import importlib.util, math, os, random, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)
_tspec = importlib.util.spec_from_file_location("apex_tex", os.path.join(_HERE, "apex_tex.py"))
tex = importlib.util.module_from_spec(_tspec)
sys.modules["apex_tex"] = tex
_tspec.loader.exec_module(tex)
_gspec = importlib.util.spec_from_file_location("gen_crowd", os.path.join(_HERE, "gen_crowd.py"))
gen_crowd = importlib.util.module_from_spec(_gspec)
_gspec.loader.exec_module(gen_crowd)

import bpy

ASSET = os.environ.get("ASSET", globals().get("ASSET", "all"))
SHEET = os.path.join(apex.PROPS_ROOT, "misc", "T_crowd_standing.png")
STRIP_M = 8.0          # the sheet's top half: a wrapping 8 m strip ...
STRIP_H = 1.9          # ... 1.9 m tall, in v 0.5..1
CELL_H = 1.95          # the bottom half's cells, v 0..0.5
CELLS = {name: (u0, u1) for name, u0, u1 in gen_crowd.SHEET_CELLS}

# The bank: toe at the road edge, a 1:2.6 slope to a flat crest, a short
# back slope. Profile in (y, z).
BANK = [(0.0, 0.0), (0.6, 0.12), (7.2, 2.6), (8.6, 2.75), (10.0, 2.6), (11.4, 0.0)]
PITCH = 10.0


def bank_z(y):
    for (y0, z0), (y1, z1) in zip(BANK, BANK[1:]):
        if y0 <= y <= y1:
            return z0 + (z1 - z0) * (y - y0) / (y1 - y0)
    return 0.0


def grass_material():
    """The bank's own turf (banking_seats' `bank_grass` came out a pale lime
    in the game, which glowed on the terrain grass)."""
    tex.reset_cache()
    return tex.kit_material("bank_turf")


def crowd_material():
    return apex.image_material("crowd_standing", SHEET, roughness=0.85, masked=True)


def bank(b, grass):
    # The turf skin, subdivided across so the slope shades, with skirts at
    # the module ends (hidden by the neighbour when tiled, closed at a run's end).
    xs = (-PITCH / 2, PITCH / 2)
    for (y0, z0), (y1, z1) in zip(BANK, BANK[1:]):
        b.sheet(grass, (xs[0], y0, z0), (xs[1], y0, z0), (xs[1], y1, z1), (xs[0], y1, z1))
    for x in xs:
        for (y0, z0), (y1, z1) in zip(BANK, BANK[1:]):
            if z0 + z1 > 0:
                b.sheet(grass, (x, y0, 0), (x, y1, 0), (x, y1, z1), (x, y0, z0))


def crowd_rows(b, mat, seed=5):
    """Standing spectators on the slope and the crest, a row every 1.3 m up
    the bank; each row is two or three cards with gaps, each card a window
    of the wrapping strip at its own offset."""
    rng = random.Random(seed)
    v0, v1 = 0.5, 1.0
    for y in (1.4, 2.7, 4.0, 5.3, 6.6, 8.0):
        x = -PITCH / 2 + rng.uniform(0.0, 0.6)
        while x < PITCH / 2 - 1.2:
            w = min(rng.uniform(2.4, 4.4), PITCH / 2 - x)
            u = rng.uniform(0, 1)
            z = bank_z(y) - 0.04
            b.quad_uv(mat, [(x, y, z), (x + w, y, z), (x + w, y, z + STRIP_H), (x, y, z + STRIP_H)],
                      [(u, v0), (u + w / STRIP_M, v0), (u + w / STRIP_M, v1), (u, v1)])
            x += w + rng.uniform(0.3, 1.6)


def build_bank(crowd):
    apex.reset_scene()
    name = "grass_bank_10m" + ("_crowd" if crowd else "")
    grass = grass_material()
    b = apex.Builder(name)
    bank(b, grass)
    if crowd:
        crowd_rows(b, crowd_material())
    return b.finish(planar_uv=True, uv_scale=1.0), "grandstand", name


SHIRTS = [(0.62, 0.08, 0.07), (0.95, 0.45, 0.05), (0.10, 0.22, 0.55), (0.85, 0.85, 0.82),
          (0.12, 0.42, 0.20), (0.92, 0.75, 0.10), (0.20, 0.20, 0.22), (0.45, 0.60, 0.80)]
LEGS = [(0.10, 0.13, 0.22), (0.28, 0.27, 0.25), (0.55, 0.50, 0.42)]
SKIN = [(0.93, 0.76, 0.62), (0.72, 0.52, 0.38), (0.42, 0.28, 0.20)]
HAIR = [(0.08, 0.06, 0.05), (0.40, 0.26, 0.12), (0.80, 0.70, 0.45)]
LIN = lambda c: tuple(v ** 2.2 for v in c)   # the tables are sRGB; Blender's base colour is linear


def figure(b, rng, x, y, yaw, h):
    """One standing spectator, `h` tall, feet at (x, y), facing -Y turned by
    `yaw`: legs, a torso, arms hanging or one or both raised, a head with
    hair or a cap. Built in its own frame and turned into place."""
    k = h / 1.75
    ti = rng.randrange(len(SHIRTS))
    li, si, hi = rng.randrange(len(LEGS)), rng.randrange(len(SKIN)), rng.randrange(len(HAIR))
    shirt = apex.material("crowd_shirt_%d" % ti, LIN(SHIRTS[ti]), roughness=0.85)
    legs = apex.material("crowd_legs_%d" % li, LIN(LEGS[li]), roughness=0.9)
    skin = apex.material("crowd_skin_%d" % si, LIN(SKIN[si]), roughness=0.7)
    hair = apex.material("crowd_hair_%d" % hi, LIN(HAIR[hi]), roughness=0.9)
    c, s_ = math.cos(yaw), math.sin(yaw)
    place = lambda p: (x + p[0] * c - p[1] * s_, y + p[0] * s_ + p[1] * c, p[2])

    def box(mat, lo, hi):
        for v in b.box(mat, lo, hi):
            v.co = place(v.co)

    hip, sh, top = 0.86 * k, 1.42 * k, h
    for sx in (-0.1, 0.1):                                   # legs
        box(legs, (sx * k - 0.07, -0.08, 0.0), (sx * k + 0.07, 0.08, hip))
    box(shirt, (-0.2 * k, -0.11, hip), (0.2 * k, 0.11, sh))    # torso
    box(shirt, (-0.08, -0.07, sh), (0.08, 0.07, sh + 0.06))   # neck line
    box(skin, (-0.1, -0.11, sh + 0.04), (0.1, 0.1, top - 0.03))   # head
    if rng.random() < 0.35:                                     # a cap
        box(shirt, (-0.11, -0.2, top - 0.08), (0.11, 0.11, top + 0.01))
    else:
        box(hair, (-0.11, -0.05, top - 0.1), (0.11, 0.12, top + 0.01))
    raise_l, raise_r = rng.random() < 0.1, rng.random() < 0.12
    for side, up in ((-1, raise_l), (1, raise_r)):
        p0 = place((side * 0.25 * k, 0.0, sh - 0.06))
        if up:
            p1 = place((side * 0.36 * k, -0.04, top + 0.12))
        else:
            p1 = place((side * 0.28 * k, 0.03, hip + 0.02))
        b.bar(shirt, p0, p1, 0.055, segs=4)
        if up:   # the hand
            box(skin, (side * 0.36 * k - 0.05, -0.09, top + 0.1), (side * 0.36 * k + 0.05, 0.01, top + 0.22))


def build_group():
    """A loose knot of standing spectators facing the road (-Y), real
    low-poly figures rather than cards so it reads from any side: about a
    dozen in the 4.2 x 2.2 m footprint, mixed heights and turns."""
    apex.reset_scene()
    b = apex.Builder("crowd_group")
    rng = random.Random(7)
    spots = []
    while len(spots) < 12:
        x, y = rng.uniform(-1.9, 1.9), rng.uniform(-0.9, 1.0)
        if all(math.hypot(x - a, y - c) > 0.55 for a, c in spots):
            spots.append((x, y))
    for x, y in spots:
        figure(b, rng, x, y, math.radians(rng.uniform(-30, 30)), rng.uniform(1.6, 1.88))
    return b.finish(planar_uv=True, uv_scale=1.0), "misc", "crowd_group"


def build_marshal():
    apex.reset_scene()
    b = apex.Builder("marshal_figure")
    mat = crowd_material()
    # The figure stands left of the cell's centre (gen_crowd puts it at
    # w/2 - 0.2 m to leave room for the flag), so shift the card to put the
    # feet on the origin.
    u0, u1 = CELLS["marshal_a"]
    w = (u1 - u0) * STRIP_M
    b.card(mat, (0.2, 0.0, 0.0), w, CELL_H, uv=(u0, 0.0, u1, 0.5))
    return b.finish(planar_uv=False, recalc=False), "misc", "marshal_figure"


BUILDS = {
    "grass_bank_10m": lambda: build_bank(False),
    "grass_bank_10m_crowd": lambda: build_bank(True),
    "crowd_group": build_group,
    "marshal_figure": build_marshal,
}
VIEWS = {
    "grass_bank_10m": ((-6, -14, 3), (2, 5, 1.5)),
    "grass_bank_10m_crowd": ((-6, -14, 3), (2, 5, 1.5)),
    "crowd_group": ((-4, -8, 1.8), (0, 1, 1.0)),
    "marshal_figure": ((-1.5, -4, 1.3), (0, 0, 1.0)),
}

if __name__ == "__main__":
    out = {}
    for key, fn in BUILDS.items():
        if ASSET not in ("all", key):
            continue
        ob, kind, name = fn()
        out[name] = apex.stats()[name]
        apex.export_one(kind, name)
        apex.save_blend(kind, name)
        apex.preview_ground()
        lf, la = VIEWS[key]
        apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", name + ".png"), look_from=lf, look_at=la)
    print("STATS", out)
