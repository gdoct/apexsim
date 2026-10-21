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
  spectators. Turf is `bank_grass`, the slot banking_seats already uses.
- misc/crowd_group: three cards of standing people in a loose knot, for
  gates, fan zones and the paddock. Centred.
- misc/marshal_figure: one marshal in orange overalls holding a furled flag,
  one card facing the road. Centred on the card's foot.
"""
import importlib.util, math, os, random, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)
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
    """bank_grass as banking_seats has it (baked turf), so the slot means one
    material across the grandstand kind."""
    path = os.path.join(apex.PROPS_ROOT, "grandstand", "banking_seats.glb")
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=path)
    src = next(o for o in bpy.data.objects if o.type == 'MESH' and o.name.startswith("banking_seats"))
    m = next(m for m in src.data.materials if m.name.split(".")[0] == "bank_grass")
    bpy.data.objects.remove(src)
    m.name = "bank_grass"
    return m


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
    return b.finish(planar_uv=True, uv_scale=0.25), "grandstand", name


def cell_card(b, mat, cell, x, y, yaw, scale=1.0):
    u0, u1 = CELLS[cell]
    w = (u1 - u0) * STRIP_M * scale
    h = CELL_H * scale
    b.card(mat, (x, y, 0.0), w, h, yaw=yaw, uv=(u0, 0.0, u1, 0.5))


def build_group():
    apex.reset_scene()
    b = apex.Builder("crowd_group")
    mat = crowd_material()
    # A loose knot: three groups facing roughly the same way (the road, -Y),
    # turned a little so the knot has depth from any side.
    cell_card(b, mat, "group_a", -1.3, 0.0, math.radians(8))
    cell_card(b, mat, "group_b", 0.9, 0.9, math.radians(-14))
    cell_card(b, mat, "group_c", -0.2, 2.0, math.radians(4))
    return b.finish(planar_uv=False, recalc=False), "misc", "crowd_group"


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
