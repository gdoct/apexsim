r"""Batch F - terrain and horizon (docs/PROPS.md step 3):

  tree/forest_impostor          40 x 40 x 18 m flat-shaded wood cluster, one per forest polygon
  tree/forest_impostor_conifer  same footprint, all spruce (Styrian hillsides)
  building/village_house_a/b/c  Styrian gabled farmhouses
  building/barn                 timber barn
  building/chapel               village church with a pointed spire
  misc/power_pylon              35 m lattice pylon

    ASSET = "all"
    exec(open(r"D:\apexsim\content\props\_batches\build_batch_f_terrain.py").read())
"""
import bpy, math, os, importlib.util, sys, random
from mathutils import Vector

_ROOT = "D:\\apexsim"
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "content\\props\\_tools\\apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
_t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "content\\props\\_tools\\apex_tex.py"))
tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
B, M = apex.Builder, tex.kit_material   # baked slot where the kit has one, flat otherwise

try:
    ASSET
except NameError:
    ASSET = "all"


def mats():
    return dict(
        fol_a=M("tree_foliage_a", (0.10, 0.30, 0.06), roughness=1.0),
        fol_b=M("tree_foliage_b", (0.14, 0.36, 0.09), roughness=1.0),
        con_a=M("tree_conifer_a", (0.06, 0.20, 0.08), roughness=1.0),
        con_b=M("tree_conifer_b", (0.08, 0.25, 0.10), roughness=1.0),
        floor=M("forest_floor", (0.12, 0.14, 0.07), roughness=1.0),
        render=M("house_render", (0.92, 0.9, 0.84), roughness=0.85),
        render_b=M("house_render_b", (0.85, 0.8, 0.62), roughness=0.85),
        timber=M("house_timber", (0.42, 0.28, 0.15), roughness=0.8),
        timber_d=M("house_timber_dark", (0.25, 0.17, 0.1), roughness=0.8),
        roof=M("house_roof_tile", (0.55, 0.25, 0.15), roughness=0.8),
        roof_d=M("house_roof_dark", (0.25, 0.24, 0.23), roughness=0.7),
        glass=M("pit_glass", (0.05, 0.08, 0.1), metallic=0.2, roughness=0.15),
        stone=M("house_stone", (0.62, 0.6, 0.55), roughness=0.9),
        galv=M("armco_galv", (0.62, 0.64, 0.66), metallic=0.7, roughness=0.45),
        insul=M("pylon_insulator", (0.5, 0.55, 0.5), roughness=0.4),
    )


# ------------------------------------------------------------- forest impostor
def forest(name, conifer_share=0.6, seed=3, size=40.0, top=18.0):
    """A 40 x 40 m patch of wood as one cluster mesh: broadleaf blobs (ico,
    subdiv 1) and spruce cones, denser in the middle, on a dark floor
    skirt. Centred pivot. Flat shaded, solid, ~2.5 K tris."""
    m = mats()
    b = B(name)
    rng = random.Random(seed)
    half = size / 2
    b.box(m["floor"], (-half, -half, 0), (half, half, 0.3))
    n = 46
    for i in range(n):
        # jittered grid so the canopy has no holes
        gx, gy = i % 7, i // 7
        x = -half + size * (gx + 0.5 + rng.uniform(-0.35, 0.35)) / 7
        y = -half + size * (gy + 0.5 + rng.uniform(-0.35, 0.35)) / 7
        edge = max(abs(x), abs(y)) / half          # 0 centre .. 1 edge
        h = top * (1.0 - 0.45 * edge ** 2) * rng.uniform(0.8, 1.0)
        if rng.random() < conifer_share:
            r = h * 0.28
            mat = m["con_a"] if rng.random() < 0.6 else m["con_b"]
            b.cone(mat, (x, y, 0.3), r, r * 0.25, h * 0.45, segs=7, cap=False)
            b.cone(mat, (x, y, 0.3 + h * 0.35), r * 0.75, 0.0, h * 0.65, segs=7, cap=False)
        else:
            r = h * 0.3
            mat = m["fol_a"] if rng.random() < 0.5 else m["fol_b"]
            b.ico(mat, (x, y, h * 0.6), r, subdiv=1, scale=(1, 1, 0.75), jitter=0.12, seed=i)
    ob = b.finish(recalc=True)
    ob.data.shade_flat()
    return ob


# --------------------------------------------------------------------- houses
def gable_house(b, m, L, D, H_wall, roof_pitch, x0=0.0, y0=0.0, wall=None, roof=None,
                overhang=0.6, upper_timber=True, ridge_along_x=True, dormer=False):
    """Rectangular block (x0..x0+L along X, y0..y0+D along Y, pivot-relative)
    with a gabled roof. Styrian look: rendered ground floor, timber gable,
    deep eaves."""
    wall = wall or m["render"]
    roof = roof or m["roof"]
    x1, y1 = x0 + L, y0 + D
    b.box(wall, (x0, y0, 0), (x1, y1, H_wall))
    if ridge_along_x:
        rise = (D / 2) * math.tan(roof_pitch)
        ridge_z = H_wall + rise
        # gable triangles (timber)
        for x, flip in ((x0, True), (x1, False)):
            pts = [(x, y0, H_wall), (x, y1, H_wall), (x, (y0 + y1) / 2, ridge_z)]
            if flip:
                pts.reverse()
            f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in pts]); f.material_index = b.slot(m["timber"] if upper_timber else wall)
        # roof planes with overhang
        ox = overhang
        oy = overhang * math.cos(roof_pitch)
        oz = overhang * math.sin(roof_pitch)
        for s in (-1, 1):
            ye = y0 if s < 0 else y1
            ye_o = ye - oy * (1 if s < 0 else -1)
            pts = [(x0 - ox, ye_o, H_wall - oz), (x1 + ox, ye_o, H_wall - oz), (x1 + ox, (y0 + y1) / 2, ridge_z + 0.05), (x0 - ox, (y0 + y1) / 2, ridge_z + 0.05)]
            if s > 0:
                pts.reverse()
            b.sheet(roof, *pts)
            pts2 = [(p[0], p[1], p[2] - 0.12) for p in pts]
            b.sheet(m["timber_d"], *reversed(pts2))
    else:
        rise = (L / 2) * math.tan(roof_pitch)
        ridge_z = H_wall + rise
        for y, flip in ((y0, False), (y1, True)):
            pts = [(x0, y, H_wall), (x1, y, H_wall), ((x0 + x1) / 2, y, ridge_z)]
            if flip:
                pts.reverse()
            f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in pts]); f.material_index = b.slot(m["timber"] if upper_timber else wall)
        oy = overhang
        ox = overhang * math.cos(roof_pitch)
        oz = overhang * math.sin(roof_pitch)
        for s in (-1, 1):
            xe = x0 if s < 0 else x1
            xe_o = xe - ox * (1 if s < 0 else -1)
            pts = [(xe_o, y0 - oy, H_wall - oz), (xe_o, y1 + oy, H_wall - oz), ((x0 + x1) / 2, y1 + oy, ridge_z + 0.05), ((x0 + x1) / 2, y0 - oy, ridge_z + 0.05)]
            if s < 0:
                pts.reverse()
            b.sheet(roof, *pts)
            pts2 = [(p[0], p[1], p[2] - 0.12) for p in pts]
            b.sheet(m["timber_d"], *reversed(pts2))
    return ridge_z


def windows(b, m, x0, x1, y, z, n, w=1.0, h=1.2, face=-1, shutters=True):
    """Row of n windows on a wall at y (face -1 = window faces -Y)."""
    pitch = (x1 - x0) / n
    for i in range(n):
        cx = x0 + pitch * (i + 0.5)
        yy = y - 0.04 * face
        b.box(m["glass"], (cx - w / 2, min(y, yy) - 0.01, z), (cx + w / 2, max(y, yy) + 0.01, z + h))
        b.box(m["timber_d"], (cx - w / 2 - 0.08, min(y, yy) - 0.02, z - 0.08), (cx + w / 2 + 0.08, max(y, yy) + 0.02, z - 0.0))
        if shutters:
            for s in (-1, 1):
                b.box(m["timber"], (cx + s * (w / 2 + 0.05) - 0.2 * (s < 0), min(y, yy) - 0.03, z), (cx + s * (w / 2 + 0.05) + 0.2 * (s > 0), max(y, yy) + 0.03, z + h))


def door(b, m, cx, y, w=1.1, h=2.1):
    b.box(m["timber_d"], (cx - w / 2, y - 0.02, 0), (cx + w / 2, y + 0.05, h))


def village_house_a():
    """12 x 9 m farmhouse, 1.5 storeys, 45° roof, ridge along the road,
    timber upper gable, balcony on the road side. Pivot road edge (y=0)."""
    m = mats()
    b = B("village_house_a")
    L, D = 12.0, 9.0
    rz = gable_house(b, m, L, D, 3.4, math.radians(42))
    windows(b, m, 0.5, L - 0.5, 0.0, 1.0, 4, face=-1)
    windows(b, m, 0.5, L - 0.5, D, 1.0, 4, face=1)
    door(b, m, L / 2, 0.0)
    # timber balcony along the road face under the eaves
    b.box(m["timber"], (0.3, -1.1, 3.35), (L - 0.3, 0.0, 3.5))
    for x in [0.4 + i * 0.4 for i in range(int((L - 0.8) / 0.4) + 1)]:
        b.box(m["timber"], (x - 0.03, -1.05, 3.5), (x + 0.03, -0.99, 4.4))
    b.box(m["timber"], (0.3, -1.1, 4.4), (L - 0.3, -0.99, 4.5))
    b.box(m["stone"], (5.0, 3.0, rz - 0.6), (5.8, 3.8, rz + 0.8))   # chimney
    b.translate(-L / 2)
    return b.finish()


def village_house_b():
    """10 x 8 m, ridge across the road (gable to the street), timber upper
    storey on a rendered base, dark roof."""
    m = mats()
    b = B("village_house_b")
    L, D = 10.0, 8.0
    b.box(m["timber"], (0, 0, 2.9), (L, D, 3.0))
    rz = gable_house(b, m, L, D, 2.9, math.radians(45), roof=m["roof_d"], ridge_along_x=False, wall=m["render_b"])
    windows(b, m, 0.5, L - 0.5, 0.0, 0.9, 3, face=-1)
    windows(b, m, 0.5, L - 0.5, -0.05, 3.4, 3, face=-1, shutters=False)
    door(b, m, 2.0, 0.0)
    b.box(m["stone"], (L / 2 + 1.5, 2.5, rz - 1.0), (L / 2 + 2.2, 3.2, rz + 0.6))
    b.translate(-L / 2)
    return b.finish()


def village_house_c():
    """L-shaped 14 x 10 m farmyard house: main wing along the road plus a
    lower stable wing behind, both tiled."""
    m = mats()
    b = B("village_house_c")
    gable_house(b, m, 14.0, 6.5, 3.2, math.radians(40))
    gable_house(b, m, 6.0, 6.0, 2.8, math.radians(38), x0=8.0, y0=6.0, ridge_along_x=False, wall=m["render_b"], upper_timber=True)
    windows(b, m, 0.5, 13.5, 0.0, 1.0, 5, face=-1)
    door(b, m, 3.0, 0.0)
    b.box(m["stone"], (10.5, 2.5, 5.2), (11.2, 3.2, 6.9))
    # small yard wall and gate post
    b.box(m["stone"], (14.0, 0.0, 0), (14.3, 3.5, 1.2))
    b.translate(-7.0)
    return b.finish()


def barn():
    """18 x 10 m timber barn, 7.8 m to the ridge, big doors on the road side."""
    m = mats()
    b = B("barn")
    L, D = 18.0, 10.0
    b.box(m["stone"], (0, 0, 0), (L, D, 0.8))
    gable_house(b, m, L, D, 4.5, math.radians(36), wall=m["timber"], roof=m["roof_d"], upper_timber=True, overhang=0.8)
    # door pair + hay loft hatch
    b.box(m["timber_d"], (5.5, -0.03, 0), (9.5, 0.03, 3.6))
    b.box(m["timber_d"], (9.6, -0.03, 0), (12.5, 0.03, 3.6))
    for x in (0.0, 4.0, 8.0, 12.0, 16.0):
        b.box(m["timber_d"], (x - 0.05, -0.06, 0.8), (x + 0.15, 0.0, 4.5))   # board-and-batten studs
    b.translate(-L / 2)
    return b.finish()


def chapel():
    """8 x 14 m nave with a 5 x 5 m tower on the road end, pointed spire to
    22 m. White render, red tile nave, dark spire. Pivot road edge."""
    m = mats()
    b = B("chapel")
    gable_house(b, m, 8.0, 14.0, 6.0, math.radians(50), y0=5.0, ridge_along_x=False, upper_timber=False, overhang=0.4)
    # apse
    b.cylinder(m["render"], (4.0, 19.0, 0), 3.2, 6.0, segs=8)
    b.cone(m["roof"], (4.0, 19.0, 6.0), 3.5, 0.1, 2.6, segs=8, cap=False)
    # tower on the road end
    tx0, ty0, T = 1.5, 0.0, 5.0
    b.box(m["render"], (tx0, ty0, 0), (tx0 + T, ty0 + T, 15.0))
    b.box(m["timber_d"], (tx0 - 0.05, ty0 - 0.05, 14.4), (tx0 + T + 0.05, ty0 + T + 0.05, 15.0))
    # louvred bell openings
    for (x0, x1, y0, y1) in ((tx0 + 1.8, tx0 + 3.2, ty0 - 0.02, ty0), (tx0 + 1.8, tx0 + 3.2, ty0 + T, ty0 + T + 0.02),
                             (tx0 - 0.02, tx0, ty0 + 1.8, ty0 + 3.2), (tx0 + T, tx0 + T + 0.02, ty0 + 1.8, ty0 + 3.2)):
        b.box(m["timber_d"], (x0, y0, 11.5), (x1, y1, 13.8))
    # clock faces
    b.cylinder(m["render_b"], (tx0 + T / 2, ty0 - 0.02, 10.0), 0.9, 0.02, segs=12, axis='Y')
    # spire: square base to point
    base = [(tx0, ty0, 15.0), (tx0 + T, ty0, 15.0), (tx0 + T, ty0 + T, 15.0), (tx0, ty0 + T, 15.0)]
    mid = [(tx0 + 0.6, ty0 + 0.6, 17.0), (tx0 + T - 0.6, ty0 + 0.6, 17.0), (tx0 + T - 0.6, ty0 + T - 0.6, 17.0), (tx0 + 0.6, ty0 + T - 0.6, 17.0)]
    top = [(tx0 + T / 2 - 0.01, ty0 + T / 2 - 0.01, 22.0), (tx0 + T / 2 + 0.01, ty0 + T / 2 - 0.01, 22.0), (tx0 + T / 2 + 0.01, ty0 + T / 2 + 0.01, 22.0), (tx0 + T / 2 - 0.01, ty0 + T / 2 + 0.01, 22.0)]
    b.loft(m["roof_d"], [base, mid, top], closed=True, cap_ends=False)
    b.bar(m["galv"], (tx0 + T / 2, ty0 + T / 2, 22.0), (tx0 + T / 2, ty0 + T / 2, 23.5), 0.04)
    b.box(m["galv"], (tx0 + T / 2 - 0.35, ty0 + T / 2 - 0.02, 22.9), (tx0 + T / 2 + 0.35, ty0 + T / 2 + 0.02, 22.98))
    # door and nave windows
    door(b, m, tx0 + T / 2, ty0, w=1.6, h=3.0)
    for s, y in ((-1, 0.0), (1, 8.0)):
        pass
    for y in (8.0, 11.5, 15.0):
        for x in (0.0, 8.0):
            b.box(m["glass"], (x - 0.01, y - 0.5, 2.0), (x + 0.01, y + 0.5, 4.8))
    b.translate(-4.0)
    return b.finish()


# ----------------------------------------------------------------- power pylon
def power_pylon():
    """35 m lattice suspension tower: tapered 4-leg body (7 m base, 1.6 m at
    the waist), two crossarm levels carrying three phases a side, earth peak.
    Centred pivot; the line runs along X."""
    m = mats()
    b = B("power_pylon")
    g = m["galv"]
    levels = [(0, 3.5), (6, 2.9), (12, 2.3), (18, 1.8), (24, 1.3), (28, 1.0), (32, 0.8), (35, 0.6)]
    prev = None
    for z, hw in levels:
        cur = [(sx * hw, sy * hw, z) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        if prev:
            for k in range(4):
                b.bar(g, prev[k], cur[k], 0.09, segs=5)                              # legs
                b.bar(g, cur[k], cur[(k + 1) % 4], 0.03, segs=4)                     # ring
                b.bar(g, prev[k], cur[(k + 1) % 4], 0.025, segs=4)                   # diagonals
                b.bar(g, prev[(k + 1) % 4], cur[k], 0.025, segs=4)
        else:
            for k in range(4):
                b.box(m["stone"], (cur[k][0] - 0.5, cur[k][1] - 0.5, 0.0), (cur[k][0] + 0.5, cur[k][1] + 0.5, 0.25))
        prev = cur
    # crossarms (along Y, out to ±7 and ±5.5 m) at 22 and 29 m, insulators hanging
    for z, reach in ((22.0, 7.0), (29.0, 5.5)):
        for s in (-1, 1):
            tip = (0, s * reach, z)
            hw = 1.3 if z < 25 else 1.0
            for x in (-hw, hw):
                b.bar(g, (x, s * hw, z), tip, 0.05, segs=5)
                b.bar(g, (x, s * hw, z - 2.5), tip, 0.04, segs=5)
            b.bar(m["insul"], (0, s * (reach - 0.4), z - 0.05), (0, s * (reach - 0.4), z - 2.2), 0.12, segs=6)
            b.bar(m["insul"], (0, s * (reach - 0.4), z - 2.2), (0, s * (reach - 0.4), z - 2.4), 0.05, segs=6)
    # earth-wire peak
    b.bar(g, (0, 0, 35), (0, 0, 38), 0.06)
    return b.finish()


BUILD = {
    "tree/forest_impostor": lambda: forest("forest_impostor", 0.6),
    "tree/forest_impostor_conifer": lambda: forest("forest_impostor_conifer", 1.0, seed=5),
    "building/village_house_a": village_house_a,
    "building/village_house_b": village_house_b,
    "building/village_house_c": village_house_c,
    "building/barn": barn,
    "building/chapel": chapel,
    "misc/power_pylon": power_pylon,
}
keys = list(BUILD) if ASSET == "all" else [k for k in BUILD if k.endswith("/" + ASSET) or k == ASSET]
apex.reset_scene()
tex.reset_cache()
exported = {}
for k in keys:
    BUILD[k]()
    kind, asset = k.split("/")
    exported[k] = apex.export_one(kind, asset)
result = {"exported": exported, "stats": apex.stats()}
