r"""Kit gaps 2, 4, 6, 7, 8 and 9 (docs/track-improvements.md "Kit gaps"):
the stands, pit storeys and bridges the dressing audit found no kit piece for.

  pit/garage_upper_6m               hospitality storey on a garage_6m: glazed
                                    front, balcony, roof terrace, plant screen
  pit/garage_roof_membrane_6m       white tensioned-fabric peak on a garage_6m
  grandstand/tent_roof_10m          bay_10m-family stand under a fabric peak
  grandstand/palm_canopy_10m        bay_10m-family stand under a frond leaf on
                                    a branching steel trunk
  grandstand/lotus_canopy_10m       bay_10m-family stand under a white shell
                                    sweeping up and out to a raised lip
  grandstand/stone_tribune_10m      open 1930s stone terrace, no seats or roof
  grandstand/stone_tribune_centre_10m  the same with the speaker's tower block
  bridge/sky_platform               two glazed wings cantilevered over the road
                                    from either side, nearly meeting
  bridge/banked_oval_span           a steeply banked concrete oval deck on
                                    piers, crossing over the road
  building/banked_oval_10m          a straight 10 m module of that deck beside
                                    the road, tiled along the oval's line

Headless, from apexsim-content/ (one Blender at a time):

    "C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" -b --python tools/generators/props/build_kit_gaps_stands.py
    APEX_ASSET=sky_platform  (env var) builds one key of BUILD instead of all

Writes props/<kind>/<asset>.{glb,blend} and props/_preview/<asset>.png (3/4)
and <asset>_b.png (from the road). The previews render with back-face
culling on, so a face wound the wrong way shows as a hole; the neighbours
either side (and the garage under a pit storey) are preview-only context.

Conventions (docs/props.md): metres, Z up, +X along the track, the road on
-Y. Stands and pit pieces pivot on the road-facing edge (y = 0) and reach
toward +Y; they tile on their pitch. Anything on a module boundary is split
in half at x = +-pitch/2 (a half box each side), so a run never doubles a
face and a run's end is still closed. Bridges: road centre on the ground,
road along X, span across Y, authored for a 15 m road.

The pit storeys stack on garage_6m: its roof deck is at z 9.4 inside 0.5 m
parapets (front y -0.1..0.15, back 11.75..12.0) whose tops are at 9.9, and
the storeys bear on those. pit_glass and pit_interior are taken from the
garage GLB itself (its window-glow texture and emissive figures), so the
shared slots mean the same material; the window panes map one cell of the
glow texture each, the balustrades the texture's black gutter (unlit).
"""
import importlib.util, math, os, sys
import bpy
import numpy as np
from mathutils import Vector

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)
_tspec = importlib.util.spec_from_file_location("apex_tex", os.path.join(_HERE, "apex_tex.py"))
tex = importlib.util.module_from_spec(_tspec)
sys.modules["apex_tex"] = tex
_tspec.loader.exec_module(tex)
M = tex.kit_material
PREVIEW = os.path.join(apex.PROPS_ROOT, "_preview")

try:
    ASSET
except NameError:
    ASSET = os.environ.get("APEX_ASSET", "all")


# ===================================================================== helpers
def oface(b, mat, pts, out=None):
    """A face from points, flipped to look along `out` if given."""
    s = b.slot(mat)
    try:
        f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in pts])
    except ValueError:
        return None
    f.material_index = s
    if out is not None:
        f.normal_update()
        if f.normal.dot(Vector(out)) < 0:
            f.normal_flip()
    return f


def _frame(axis):
    # (u, v, w) -> (x, y, z), right-handed so a CCW profile caps +w.
    return {'X': lambda u, v, w: (w, u, v),
            'Y': lambda u, v, w: (v, w, u),
            'Z': lambda u, v, w: (u, v, w)}[axis]


def prism(b, mat, prof, axis, w0, w1, caps=True):
    """Extrude a 2D profile (any winding, may be concave) along `axis` from
    w0 to w1, closed and wound outward. Profile axes: X -> (y, z),
    Y -> (z, x), Z -> (x, y)."""
    area = sum(prof[i][0] * prof[(i + 1) % len(prof)][1] - prof[(i + 1) % len(prof)][0] * prof[i][1]
               for i in range(len(prof)))
    if area < 0:
        prof = prof[::-1]
    P = _frame(axis)
    s = b.slot(mat)
    A = [b.bm.verts.new(P(u, v, w0)) for (u, v) in prof]
    B = [b.bm.verts.new(P(u, v, w1)) for (u, v) in prof]
    n = len(prof)
    for i in range(n):
        j = (i + 1) % n
        f = b.bm.faces.new((A[i], A[j], B[j], B[i]))
        f.material_index = s
    if caps:
        f = b.bm.faces.new(B); f.material_index = s
        f = b.bm.faces.new(A[::-1]); f.material_index = s


def chamfer_rect(cu, cv, hu, hv, c):
    """Octagon: a rectangle centred (cu, cv), half sizes hu, hv, corners cut by c."""
    return [(cu - hu + c, cv - hv), (cu + hu - c, cv - hv), (cu + hu, cv - hv + c), (cu + hu, cv + hv - c),
            (cu + hu - c, cv + hv), (cu - hu + c, cv + hv), (cu - hu, cv + hv - c), (cu - hu, cv - hv + c)]


def uv_box(b, mat, lo, hi, uv=(0.002, 0.002, 0.008, 0.008)):
    """A box whose every face maps one small UV rect (a texture's flat patch)."""
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    u0, v0, u1, v1 = uv
    q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    v = [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
    for quad in q:
        b.quad_uv(mat, [v[i] for i in quad], [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])


def ring_tube(b, mat, pts, r, segs=8, closed=False):
    """A tube of radius r along a polyline (parallel-transported rings),
    wound outward, open ends."""
    pts = [Vector(p) for p in pts]
    n = len(pts)
    rings = []
    for i in range(n):
        if closed:
            t = (pts[(i + 1) % n] - pts[i - 1]).normalized()
        else:
            t = (pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)]).normalized()
        a = Vector((0, 0, 1)).cross(t)
        if a.length < 1e-4:
            a = Vector((1, 0, 0)).cross(t)
        a.normalize()
        c = t.cross(a).normalized()
        rings.append([pts[i] + (a * math.cos(2 * math.pi * k / segs) + c * math.sin(2 * math.pi * k / segs)) * r
                      for k in range(segs)])
    for i in range(n if closed else n - 1):
        i2 = (i + 1) % n
        for k in range(segs):
            k2 = (k + 1) % segs
            q = [rings[i][k], rings[i][k2], rings[i2][k2], rings[i2][k]]
            cen = sum(q, Vector()) / 4
            axis_c = (pts[i] + pts[i2]) / 2
            oface(b, mat, q, out=cen - axis_c)


def torus(b, mat, center, R, r, segs=20, rings=6):
    c = Vector(center)
    P = [[c + Vector((math.cos(2 * math.pi * i / segs) * (R + r * math.cos(2 * math.pi * j / rings)),
                      math.sin(2 * math.pi * i / segs) * (R + r * math.cos(2 * math.pi * j / rings)),
                      r * math.sin(2 * math.pi * j / rings))) for j in range(rings)] for i in range(segs)]
    for i in range(segs):
        i2 = (i + 1) % segs
        tube_c = c + Vector((math.cos(2 * math.pi * (i + 0.5) / segs), math.sin(2 * math.pi * (i + 0.5) / segs), 0)) * R
        for j in range(rings):
            j2 = (j + 1) % rings
            q = [P[i][j], P[i2][j], P[i2][j2], P[i][j2]]
            oface(b, mat, q, out=sum(q, Vector()) / 4 - tube_c)


def shell(b, mat, grid, thk, edge_out):
    """A thin closed membrane from a grid of top-surface points grid[i][j]
    (i across u, j across v): top up, bottom `thk` below, edge strips.
    edge_out(side) -> outward vector for side in 'u0','u1','v0','v1'."""
    ni, nj = len(grid), len(grid[0])
    top = [[Vector(p) for p in row] for row in grid]
    bot = [[p - Vector((0, 0, thk)) for p in row] for row in top]
    for i in range(ni - 1):
        for j in range(nj - 1):
            oface(b, mat, [top[i][j], top[i + 1][j], top[i + 1][j + 1], top[i][j + 1]], out=(0, 0, 1))
            oface(b, mat, [bot[i][j], bot[i + 1][j], bot[i + 1][j + 1], bot[i][j + 1]], out=(0, 0, -1))
    for side, line in (('u0', [(0, j) for j in range(nj)]), ('u1', [(ni - 1, j) for j in range(nj)]),
                       ('v0', [(i, 0) for i in range(ni)]), ('v1', [(i, nj - 1) for i in range(ni)])):
        for (a, c), (d, e) in zip(line, line[1:]):
            oface(b, mat, [top[a][c], top[d][e], bot[d][e], bot[a][c]], out=edge_out(side))


def peak(t, flat, p):
    """1 at |t| <= flat, falling to 0 at |t| = 1, concave like a pulled tent."""
    t = abs(t)
    if t <= flat:
        return 1.0
    return max(0.0, (1.0 - t) / (1.0 - flat)) ** p


def membrane_mat(name, seed):
    # White PVC fabric: panel seams every 1.25 m along x in planar UVs,
    # little grime. Opaque.
    gen = tex.cladding(seed=seed, rgb=(0.95, 0.95, 0.93), panel=(0.5, 4.0))
    m = tex.pbr_material(name, gen, tile_m=2.5, metallic=0.0)
    return m


def ashlar(seed, rgb, rows=6, cols=3, mortar=(0.42, 0.40, 0.36)):
    """Coursed stone blocks, tileable: `rows` courses and `cols` blocks per
    tile, alternate courses offset half a block; a tone per block."""
    X, Y = tex.grid()
    r = np.floor(Y * rows)
    off = (r % 2) * (0.5 / cols)
    xs = (X + off) % 1.0
    c = np.floor(xs * cols)
    h = np.sin(r * 12.9898 + c * 78.233 + seed * 3.17) * 43758.5453
    tone = 0.93 + 0.12 * (h - np.floor(h))
    fy, fx = (Y * rows) % 1.0, (xs * cols) % 1.0
    joint = ((fy < 0.035) | (fy > 0.965) | (fx < 0.012) | (fx > 0.988)).astype(float)
    grain = tex.fbm(seed, scales=(16, 48, 128), weights=(0.4, 0.35, 0.25))
    weather = tex.fbm(seed + 5, scales=(2, 5), weights=(0.6, 0.4))
    stone = np.array(rgb).reshape(1, 1, 3) * tone[..., None] * (0.9 + 0.2 * grain[..., None]) \
        * (0.92 + 0.12 * weather[..., None])
    color = stone * (1 - joint[..., None]) + np.array(mortar).reshape(1, 1, 3) * joint[..., None]
    rough = np.clip(0.75 + 0.15 * grain + 0.1 * joint, 0.6, 0.97)
    height = 0.6 + 0.08 * (grain - 0.5) - 0.45 * joint
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.0


def steal_garage():
    """Import garage_6m as context; its pit_glass / pit_interior are the
    kind's own materials (window-glow texture and all)."""
    bpy.ops.import_scene.gltf(filepath=os.path.join(apex.PROPS_ROOT, "pit", "garage_6m.glb"))
    ob = [o for o in bpy.context.selected_objects if o.type == 'MESH'][0]
    ob.name = "ctx_garage_6m"
    return ob, bpy.data.materials["pit_glass"], bpy.data.materials["pit_interior"]


# ------------------------------------------------------------- preview tools
_CTX = []


def ctx_copy(ob, dx=0.0, dy=0.0):
    c = ob.copy()
    c.location = (ob.location.x + dx, ob.location.y + dy, ob.location.z)
    c.name = "ctx_" + ob.name + "_%d" % len(_CTX)
    bpy.context.scene.collection.objects.link(c)
    _CTX.append(c)
    return c


def ctx_road(width=15.0, length=80.0):
    g = apex.Builder("ctx_road")
    g.box(apex.material("preview_asphalt", (0.07, 0.07, 0.075), roughness=0.9),
          (-length / 2, -width / 2, 0.0), (length / 2, width / 2, 0.02))
    _CTX.append(g.finish())


def shots(asset, three_q, driver, extra=()):
    """Render with back-face culling (a wrong winding shows as a hole) and the
    emission off (daylight; the glow is the client's night switch)."""
    saved = []
    for m in bpy.data.materials:
        saved.append((m, m.use_backface_culling))
        m.use_backface_culling = not m.name.endswith("_frond")
        if m.node_tree:
            for n in m.node_tree.nodes:
                if n.type == 'BSDF_PRINCIPLED':
                    s = n.inputs["Emission Strength"]
                    saved.append((s, s.default_value))
                    s.default_value = 0.0
    apex.preview_ground()
    paths = []
    for suffix, (lf, la) in [("", three_q), ("_b", driver)] + list(extra):
        p = os.path.join(PREVIEW, asset + suffix + ".png")
        apex.preview(p, look_from=lf, look_at=la)
        paths.append(p)
    for item, val in saved:
        if isinstance(item, bpy.types.Material):
            item.use_backface_culling = val
        else:
            item.default_value = val
    return paths


def clip_pitch(ob, half):
    """Pull the last centimetres of tube ends that poke past the module's
    edge back onto it, so a run tiles exactly."""
    for v in ob.data.vertices:
        if abs(v.co.x) > half:
            assert abs(v.co.x) - half < 0.05, (ob.name, tuple(v.co))
            v.co.x = math.copysign(half, v.co.x)


def clearance(ob, half_road=7.5):
    """Lowest point of anything over the road (|y| < half_road, above 0.5 m)."""
    zs = [v.co.z for v in ob.data.vertices if abs(v.co.y) < half_road and v.co.z > 0.5]
    return round(min(zs), 3) if zs else None


def finish_asset(kind, asset, ob, three_q, driver, extra=(), pitch=None):
    if pitch:
        clip_pitch(ob, pitch / 2)
    if kind == "bridge":
        print("CLEARANCE", asset, clearance(ob))
    st = apex.stats()[asset]
    print("STATS", asset, st)
    glb, size = apex.export_one(kind, asset)
    print("GLB", glb, size)
    shots(asset, three_q, driver, extra)
    for o in list(bpy.context.scene.objects):
        if o.name.startswith("ctx_") or o.name.startswith("Preview"):
            bpy.data.objects.remove(o)
    _CTX.clear()
    apex.save_blend(kind, asset)
    return st


def new_scene():
    apex.reset_scene()
    tex.reset_cache()
    _CTX.clear()


# ======================================================== the bay_10m family
# bay_10m's section (imported and measured): a 0.25 m front plinth to 0.9,
# eight 0.9 m treads rising 0.45 from 0.9 (the top one runs to 8.5), a back
# wall 8.5-8.8 to 5.6; seats on every tread (blue on even rows, red on odd),
# a 1.2 m aisle of half steps at x = 0, a front rail at 1.4 / 1.9.
TREAD, RISE, Z0, ROWS = 0.9, 0.45, 0.9, 8
BACK_Y0, BACK_Y1, BACK_TOP = 8.5, 8.8, 5.6
AISLE = 0.6
SEAT_PROFILE = [(0.52, 0.00), (0.66, 0.00), (0.66, 0.17), (0.79, 0.19), (0.86, 0.46), (0.82, 0.47),
                (0.75, 0.24), (0.40, 0.23), (0.40, 0.19), (0.52, 0.17)]


def tread_z(k):
    return Z0 + RISE * k


def stand_base(b, m, seats=True):
    conc, dark = m["conc"], m["conc_dark"]
    prof = [(-0.25, 0.0), (BACK_Y1, 0.0), (BACK_Y1, BACK_TOP), (BACK_Y0, BACK_TOP), (BACK_Y0, tread_z(7))]
    for k in range(7, 0, -1):
        prof += [(TREAD * k, tread_z(k)), (TREAD * k, tread_z(k - 1))]
    prof += [(-0.25, Z0)]
    prism(b, conc, prof, 'X', -5.0, 5.0)
    # The aisle: a half step on the back half of every tread.
    for k in range(ROWS):
        y1 = TREAD * (k + 1) if k < 7 else BACK_Y0
        b.box(dark, (-AISLE, TREAD * k + 0.45, tread_z(k)), (AISLE, min(TREAD * k + 0.9, y1), tread_z(k) + RISE / 2))
    if seats:
        n = int((4.97 - AISLE - 0.06) / 0.48)
        for k in range(ROWS):
            mat = m["seat_a"] if k % 2 == 0 else m["seat_b"]
            z = tread_z(k)
            for side in (-1, 1):
                for i in range(n):
                    xc = side * (AISLE + 0.30 + 0.48 * i)
                    prism(b, mat, [(TREAD * k + py, z + pz) for (py, pz) in SEAT_PROFILE], 'X', xc - 0.22, xc + 0.22)
    # Front rail on the plinth, posts clear of the module edges.
    rail = m["rail"]
    for z in (1.4, 1.9):
        b.bar(rail, (-5.0, -0.1, z), (5.0, -0.1, z), 0.03, segs=8)
    for x in (-4.0, -2.0, 2.0, 4.0):
        b.bar(rail, (x, -0.1, Z0), (x, -0.1, 1.93), 0.03, segs=8)
    # Aisle handrail up the middle, a post every other tread.
    ys, ye = 0.3, BACK_Y0 - 0.4
    zr = lambda y: Z0 + RISE * min(int(y / TREAD), 7) + 0.95
    b.bar(rail, (0, ys, zr(ys)), (0, TREAD * 7 + 0.3, zr(TREAD * 7 + 0.3)), 0.025, segs=8)
    for k in range(0, 8, 2):
        y = TREAD * k + 0.3
        b.bar(rail, (0, y, tread_z(k)), (0, y, zr(y)), 0.022, segs=6)
    # back-wall top rail
    b.bar(rail, (-5.0, BACK_Y0 + 0.15, BACK_TOP + 0.9), (5.0, BACK_Y0 + 0.15, BACK_TOP + 0.9), 0.03, segs=8)
    for x in (-3.0, 0.0, 3.0):
        b.bar(rail, (x, BACK_Y0 + 0.15, BACK_TOP), (x, BACK_Y0 + 0.15, BACK_TOP + 0.9), 0.025, segs=6)


def stand_mats():
    return dict(conc=M("grandstand_concrete"), conc_dark=M("grandstand_concrete_dark"),
                seat_a=M("grandstand_seat_a"), seat_b=M("grandstand_seat_b"),
                rail=M("grandstand_rail"), steel=M("grandstand_steel"))


STAND_VIEWS = dict(three_q=((24, -25, 15), (0, 4, 5)), driver=((-22, -16, 1.4), (0, 3, 5)), pitch=10.0)


def stand_context(ob, pitch=10.0):
    ctx_copy(ob, -pitch)
    ctx_copy(ob, pitch)


# ------------------------------------------------------- grandstand/tent_roof
def tent_roof_10m():
    new_scene()
    m = stand_mats()
    m["membrane"] = membrane_mat("grandstand_membrane", 71)
    b = apex.Builder("tent_roof_10m")
    stand_base(b, m)
    steel, rail = m["steel"], m["rail"]

    Y_F, Y_B = -1.8, 9.0          # membrane front / back edge at the rafters
    Z_F, Z_B = 9.0, 10.2          # rafter line heights there
    COL_Y0, COL_Y1 = 8.85, 9.25   # back columns behind the back wall
    MAST = (0.0, 9.9)             # the peak's mast behind the stand
    MAST_TOP = 17.5
    VP = 0.45                     # the peak, as a share of the depth
    H = 4.3

    # Rafters and back columns on the module edges, split in half.
    for s in (-1, 1):
        x0, x1 = sorted((s * 5.0, s * 4.86))
        prism(b, steel, [(Y_F, Z_F - 0.12), (Y_B + 0.25, Z_B - 0.12), (Y_B + 0.25, Z_B + 0.18), (Y_F, Z_F + 0.06)],
              'X', x0, x1)
        b.box(steel, (x0, COL_Y0, 0.0), (x1, COL_Y1, Z_B - 0.1))
        b.box(m["conc"], (min(s * 5.0, s * 4.7), COL_Y0 - 0.15, 0.0), (max(s * 5.0, s * 4.7), COL_Y1 + 0.15, 0.4))
        # knee brace column -> rafter
        b.bar(steel, (s * 4.93, COL_Y0, Z_B - 2.2), (s * 4.93, Y_B - 2.4, Z_B - 0.2), 0.07, segs=8)

    # Back edge beam (tiles end to end), front edge cable.
    b.bar(steel, (-5.0, Y_B + 0.05, Z_B + 0.02), (5.0, Y_B + 0.05, Z_B + 0.02), 0.12, segs=10)

    def yf(u):
        return Y_F + 0.7 * (1 - u * u)

    def base(u, v):
        zf = Z_F + 0.55 * (1 - u * u)
        return zf + (Z_B - zf) * v

    def P(u, v):
        y = yf(u) + (Y_B - yf(u)) * v
        hv = (v - VP) / (VP if v < VP else 1 - VP)
        return (5.0 * u, y, base(u, v) + H * peak(u, 0.06, 1.7) * peak(hv, 0.07, 1.7))

    nu, nv = 34, 30
    us = [-1 + 2 * i / nu for i in range(nu + 1)]
    vs = [j / nv for j in range(nv + 1)]
    grid = [[P(u, v) for v in vs] for u in us]
    shell(b, m["membrane"], grid, 0.05,
          lambda side: {'u0': (-1, 0, 0), 'u1': (1, 0, 0), 'v0': (0, -1, 0.2), 'v1': (0, 1, 0)}[side])
    ring_tube(b, steel, [grid[i][0] for i in range(nu + 1)], 0.05, segs=6)   # front edge cable

    apex_pt = Vector(P(0, VP))
    torus(b, steel, apex_pt + Vector((0, 0, 0.06)), 0.5, 0.09, segs=18, rings=6)
    b.cylinder(steel, apex_pt - Vector((0, 0, 0.1)), 0.42, 0.5, segs=18)

    # The mast, its suspension and the rafter-tip stays.
    mx, my = MAST
    b.box(m["conc"], (mx - 0.6, my - 0.6, 0.0), (mx + 0.6, my + 0.6, 0.4))
    b.cylinder(steel, (mx, my, 0.4), 0.26, MAST_TOP - 0.4, segs=14)
    b.cylinder(steel, (mx, my, MAST_TOP), 0.32, 0.2, segs=14)
    top = Vector((mx, my, MAST_TOP - 0.1))
    b.bar(steel, top, apex_pt + Vector((0, 0, 0.4)), 0.045, segs=6)
    for s in (-1, 1):
        b.bar(steel, top, (s * 4.93, Y_F + 0.05, Z_F + 0.1), 0.035, segs=6)
        b.bar(steel, top, (s * 4.93, Y_B + 0.1, Z_B + 0.2), 0.035, segs=6)
    # back stay to the ground
    b.bar(steel, top, (mx, my + 1.9, 0.3), 0.05, segs=6)
    b.box(m["conc"], (mx - 0.4, my + 1.6, 0.0), (mx + 0.4, my + 2.2, 0.35))
    # mast to back-wall tie
    b.bar(steel, (mx, my, BACK_TOP + 1.0), (mx, BACK_Y1, BACK_TOP - 0.2), 0.08, segs=8)

    ob = b.finish(recalc=False)
    stand_context(ob)
    return finish_asset("grandstand", "tent_roof_10m", ob, **STAND_VIEWS,
                        extra=[("_c", ((6, 24, 22), (0, 3, 8)))])


# ------------------------------------------------------ grandstand/palm_canopy
def palm_canopy_10m():
    new_scene()
    m = stand_mats()
    frond_gen = tex.painted_steel(seed=73, rgb=(0.16, 0.34, 0.10), chips=0.0)
    m["frond"] = tex.pbr_material("grandstand_frond", frond_gen, tile_m=2.0, metallic=0.1)
    b = apex.Builder("palm_canopy_10m")
    stand_base(b, m)
    steel, frond = m["steel"], m["frond"]

    Y_BACK, Y_TIP = 9.4, -2.6      # midrib ends
    L = Y_BACK - Y_TIP

    def mid(t):  # t 0 at the back, 1 at the tip
        return Vector((0.0, Y_BACK - L * t, 9.4 + 3.0 * t - 1.6 * t * t))

    def half_w(t):
        return 4.92 * max(0.0, math.sin(math.pi * min(1.0, t ** 0.85))) ** 0.8

    def leaf_pt(t, x):
        p = mid(t)
        return Vector((x, p.y, p.z - 0.05 * x * x))

    # Midrib: a tapering steel spine.
    spine = [mid(i / 24) for i in range(25)]
    for i in range(24):
        r = 0.2 - 0.14 * (i / 24)
        b.bar(steel, spine[i], spine[i + 1], r, segs=8)
    # Leaflets: folded (V) plates from the midrib out to the edge, raked forward.
    N = 22
    for side in (-1, 1):
        for i in range(N):
            t0 = 0.04 + 0.92 * (i + 0.5) / N
            w = half_w(t0)
            if w < 0.5:
                continue
            t1 = min(0.995, t0 + 0.11 * (w / 4.9) + 0.02)
            segs = 8
            rows = []
            for k in range(segs + 1):
                f = k / segs
                t = t0 + (t1 - t0) * f
                x = side * (0.18 + (half_w(t1) * 0.98 - 0.18) * f)
                c = leaf_pt(t, x)
                taper = (1 - 0.4 * f) * (0.7 + 0.3 * math.sin(math.pi * min(1, f * 1.3 + 0.15)))
                hw = 0.5 * 0.97 * (0.92 / N) * taper   # in t units: the leaflets all but close up
                a = leaf_pt(t - hw, x)
                d = leaf_pt(t + hw, x)
                a.z -= 0.06 * taper
                d.z -= 0.06 * taper
                c.z += 0.10 * taper
                rows.append((a, c, d))
            for k in range(segs):
                for e in (0, 1):
                    q = [rows[k][e], rows[k + 1][e], rows[k + 1][e + 1], rows[k][e + 1]]
                    oface(b, frond, q, out=(0, 0, 1))
    # Edge ribs on the outline (steel), keeps the leaf crisp from below.
    for side in (-1, 1):
        pts = [leaf_pt(t, side * half_w(t) * 0.98) for t in [0.02 + 0.97 * i / 30 for i in range(31)]]
        ring_tube(b, steel, pts, 0.05, segs=6)

    # Branching trunk behind the back wall.
    base = Vector((0.0, Y_BACK + 0.6, 0.0))
    node = Vector((0.0, Y_BACK + 0.3, 6.6))
    b.box(m["conc"], (-0.8, base.y - 0.8, 0.0), (0.8, base.y + 0.8, 0.4))
    trunk = [base + Vector((0, 0, 0.4)), Vector((0, base.y - 0.05, 3.0)), node]
    for (p, q), (r0, r1) in zip(zip(trunk, trunk[1:]), ((0.42, 0.34), (0.34, 0.3))):
        seg = 6
        for k in range(seg):
            a = p + (q - p) * (k / seg)
            c = p + (q - p) * ((k + 1) / seg)
            b.bar(steel, a, c, r0 + (r1 - r0) * (k / seg), segs=12)
    b.cylinder(steel, node - Vector((0, 0, 0.3)), 0.45, 0.6, segs=12)
    targets = [mid(0.04), mid(0.28), mid(0.55)]
    for tgt in targets:
        b.bar(steel, node, tgt - Vector((0, 0, 0.12)), 0.17, segs=10)
    # Side branches fork from the main ones to the leaf's shoulders.
    for side in (-1, 1):
        for (t_from, t_to, f) in ((0.28, 0.40, 0.45), (0.55, 0.62, 0.5)):
            src = node + (mid(t_from) - node) * f
            dst = leaf_pt(t_to, side * half_w(t_to) * 0.55) - Vector((0, 0, 0.1))
            b.bar(steel, src, dst, 0.11, segs=8)
            b.ico(steel, src, 0.2, subdiv=1)
        src = node + (mid(0.04) - node) * 0.6
        dst = leaf_pt(0.16, side * half_w(0.16) * 0.7) - Vector((0, 0, 0.1))
        b.bar(steel, src, dst, 0.1, segs=8)
    # tie the trunk to the back wall
    b.bar(steel, Vector((0, Y_BACK + 0.3, 4.6)), Vector((0, BACK_Y1, 4.6)), 0.12, segs=8)

    ob = b.finish(recalc=False)
    stand_context(ob)
    return finish_asset("grandstand", "palm_canopy_10m", ob, **STAND_VIEWS,
                        extra=[("_c", ((6, 24, 22), (0, 3, 8)))])


# ---------------------------------------------------- grandstand/lotus_canopy
def lotus_canopy_10m():
    """bay_10m's seating under a smooth white shell that sweeps up and out
    over the seats from columns behind the back wall, its lip raised like
    a leaf's edge: Shanghai's Grandstand A. Tiles along X."""
    new_scene()
    m = stand_mats()
    m["shell"] = membrane_mat("grandstand_lotus", 83)
    b = apex.Builder("lotus_canopy_10m")
    stand_base(b, m)
    steel = m["steel"]

    Y_B, Y_F = 10.2, -4.5          # shell's back edge / front lip
    Z_B, Z_F = 9.6, 17.2           # their heights

    def P(u, v):                   # u -1..1 along X, v 0 back .. 1 lip
        y = Y_B + (Y_F - Y_B) * v
        z = Z_B + (Z_F - Z_B) * v ** 1.6 + 1.1 * math.sin(math.pi * v) + 0.9 * max(0.0, v - 0.85) / 0.15
        return (5.0 * u, y, z)

    nu, nv = 10, 32
    grid = [[P(-1 + 2 * i / nu, j / nv) for j in range(nv + 1)] for i in range(nu + 1)]
    shell(b, m["shell"], grid, 0.22,
          lambda side: {'u0': (-1, 0, 0), 'u1': (1, 0, 0), 'v0': (0, 1, -0.3), 'v1': (0, -1, 0.3)}[side])

    # Ribs under the shell on the module edges (split in half), and a
    # column behind the back wall carrying each, with a raking strut.
    for s in (-1, 1):
        x = s * 4.93
        rib = [Vector(P(s, j / 16)) - Vector((0, 0, 0.25)) for j in range(17)]
        rib = [Vector((x, r.y, r.z)) for r in rib]
        for a, c in zip(rib, rib[1:]):
            b.bar(steel, a, c, 0.07, segs=6)
        b.box(steel, (min(s * 5.0, s * 4.8), 9.6, 0.0), (max(s * 5.0, s * 4.8), 10.1, Z_B))
        b.box(m["conc"], (min(s * 5.0, s * 4.6), 9.4, 0.0), (max(s * 5.0, s * 4.6), 10.3, 0.4))
        mid = P(s, 0.45)
        b.bar(steel, (x, 9.85, 3.0), (x, mid[1], mid[2] - 0.3), 0.08, segs=8)
    # Tie the columns to the back wall.
    b.bar(steel, (-5.0, 9.85, BACK_TOP), (5.0, 9.85, BACK_TOP), 0.08, segs=8)

    ob = b.finish(recalc=False)
    stand_context(ob)
    return finish_asset("grandstand", "lotus_canopy_10m", ob, **STAND_VIEWS,
                        extra=[("_c", ((6, 24, 22), (0, 3, 8)))])


# ---------------------------------------------------- grandstand/stone_tribune
# Wide, shallow stone steps, no seats or roof: a 0.6 m parapet to 1.3 at the
# front, a 2 m promenade, fourteen 0.8 m steps rising 0.38, a top promenade
# and the back wall with its coping.
ST_PARAPET = 0.6
ST_PROM_Y, ST_PROM_Z = 2.6, 0.3
ST_TREAD, ST_RISE, ST_N = 0.8, 0.38, 14
ST_TOP_Y0, ST_BACK_Y0, ST_BACK_Y1 = None, 16.0, 16.6


def st_z(k):  # top of step k (1..N)
    return ST_PROM_Z + ST_RISE * k


def st_y(k):  # front edge of step k
    return ST_PROM_Y + ST_TREAD * (k - 1)


def stone_mats():
    return dict(stone=tex.pbr_material("grandstand_stone", ashlar(81, (0.76, 0.70, 0.59)), tile_m=2.1),
                dark=tex.pbr_material("grandstand_stone_dark",
                                      ashlar(83, (0.52, 0.49, 0.43), rows=3, cols=2, mortar=(0.36, 0.34, 0.31)),
                                      tile_m=2.1),
                rail=M("grandstand_rail"))


def stone_terrace(b, m, aisle=True):
    stone, dark = m["stone"], m["dark"]
    back_top = st_z(ST_N) + 1.15
    prof = [(0.0, 0.0), (ST_BACK_Y1, 0.0), (ST_BACK_Y1, back_top), (ST_BACK_Y0, back_top), (ST_BACK_Y0, st_z(ST_N))]
    for k in range(ST_N, 0, -1):
        prof += [(st_y(k), st_z(k)), (st_y(k), st_z(k - 1))]
    prof += [(ST_PARAPET, ST_PROM_Z), (ST_PARAPET, 1.3), (0.0, 1.3)]
    prism(b, stone, prof, 'X', -5.0, 5.0)
    # Copings and step nosings, dark stone, standing proud.
    b.box(dark, (-5.0, -0.08, 1.3), (5.0, ST_PARAPET + 0.08, 1.45))
    b.box(dark, (-5.0, ST_BACK_Y0 - 0.08, back_top), (5.0, ST_BACK_Y1 + 0.08, back_top + 0.15))
    for k in range(1, ST_N + 1):
        b.box(dark, (-5.0, st_y(k) - 0.04, st_z(k) - 0.1), (5.0, st_y(k) + 0.12, st_z(k) + 0.02))
    # The parapet's front: pilasters and a plinth band.
    b.box(dark, (-5.0, -0.06, 0.0), (5.0, 0.0, 0.35))
    for x in (-3.75, -1.25, 1.25, 3.75):
        b.box(stone, (x - 0.3, -0.12, 0.0), (x + 0.3, 0.0, 1.3))
    if aisle:
        for k in range(1, ST_N):
            b.box(dark, (-0.8, st_y(k) + 0.4, st_z(k)), (0.8, st_y(k) + ST_TREAD, st_z(k) + ST_RISE / 2))
        # iron handrails either side of the aisle
        for x in (-0.85, 0.85):
            p0 = Vector((x, st_y(1) + 0.2, st_z(1) + 0.95))
            p1 = Vector((x, st_y(ST_N) + 0.2, st_z(ST_N) + 0.95))
            b.bar(m["rail"], p0, p1, 0.03, segs=8)
            for k in range(1, ST_N + 1, 3):
                y = st_y(k) + 0.2
                b.bar(m["rail"], (x, y, st_z(k)), (x, y, st_z(k) + 0.95 + 0.0), 0.025, segs=6)
    # stairs from the promenade up to step 1 at the aisle
    return back_top


STONE_VIEWS = dict(three_q=((22, -24, 14), (0, 7, 3)), driver=((-24, -14, 1.4), (0, 5, 3)), pitch=10.0)


def stone_tribune_10m():
    new_scene()
    m = stone_mats()
    b = apex.Builder("stone_tribune_10m")
    stone_terrace(b, m)
    ob = b.finish(recalc=False)
    stand_context(ob)
    return finish_asset("grandstand", "stone_tribune_10m", ob, **STONE_VIEWS)


def stone_tribune_centre_10m():
    new_scene()
    m = stone_mats()
    stone, dark = m["stone"], m["dark"]
    b = apex.Builder("stone_tribune_centre_10m")
    back_top = stone_terrace(b, m, aisle=False)
    # The tower block over the middle of the terrace.
    TX, TY0, TY1, TZ = 3.6, 4.2, ST_BACK_Y1, 13.2
    prism(b, stone, chamfer_rect(0, (TY0 + TY1) / 2, TX, (TY1 - TY0) / 2, 0.15), 'Z', 0.0, TZ)
    # cornice and attic
    b.box(dark, (-TX - 0.2, TY0 - 0.2, TZ - 0.5), (TX + 0.2, TY1 + 0.2, TZ - 0.1))
    b.box(stone, (-TX + 0.1, TY0 + 0.1, TZ - 0.1), (TX - 0.1, TY1 - 0.1, TZ + 0.9))
    b.box(dark, (-TX, TY0, TZ + 0.9), (TX, TY1, TZ + 1.05))
    # pilasters on the front face
    for x in (-TX + 0.35, -1.45, 1.45, TX - 0.35):
        b.box(stone, (x - 0.35, TY0 - 0.22, 0.0), (x + 0.35, TY0, TZ - 0.5))
    # tall dark recesses between them (doors / louvres)
    for x0, x1, z0 in ((-1.1, 1.1, 6.6), (-TX + 0.7, -1.8, 8.0), (1.8, TX - 0.7, 8.0)):
        b.box(dark, (x0, TY0 - 0.04, z0), (x1, TY0, TZ - 1.4))
    # The speaker's platform on its podium in front of the tower.
    PX, PY0, PZ = 2.6, 1.4, 6.6
    prism(b, stone, chamfer_rect(0, (PY0 + TY0) / 2, PX, (TY0 - PY0) / 2, 0.1), 'Z', 0.0, PZ)
    b.box(dark, (-PX - 0.15, PY0 - 0.15, PZ - 0.35), (PX + 0.15, TY0, PZ))
    for (x0, y0, x1, y1) in ((-PX - 0.1, PY0 - 0.1, PX + 0.1, PY0 + 0.35),
                             (-PX - 0.1, PY0 - 0.1, -PX + 0.25, TY0),
                             (PX - 0.25, PY0 - 0.1, PX + 0.1, TY0)):
        b.box(stone, (x0, y0, PZ), (x1, y1, PZ + 1.0))
        b.box(dark, (x0 - 0.05, y0 - 0.05, PZ + 1.0), (x1 + 0.05, y1 + 0.05, PZ + 1.12))
    # podium front: plinth band and a panel
    b.box(dark, (-PX - 0.05, PY0 - 0.05, 0.0), (PX + 0.05, TY0, 0.45))
    b.box(dark, (-1.6, PY0 - 0.03, 2.2), (1.6, PY0, 4.8))
    ob = b.finish(recalc=False)
    # neighbours are the plain terrace
    ctx = apex.Builder("ctx_terrace")
    stone_terrace(ctx, m)
    plain = ctx.finish(recalc=False)
    ctx_copy(plain, -10.0)
    ctx_copy(plain, 10.0)
    bpy.data.objects.remove(plain)
    return finish_asset("grandstand", "stone_tribune_centre_10m", ob,
                        three_q=((26, -30, 17), (0, 6, 5)), driver=((-26, -14, 1.4), (0, 5, 5)), pitch=10.0,
                        extra=[("_c", ((0, -34, 9), (0, 6, 6)))])


# ====================================================================== pits
GARAGE_ROOF = 9.9  # top of garage_6m's parapets (its deck is at 9.4)


def garage_upper_6m():
    new_scene()
    garage, glass, interior = steal_garage()
    wall, frame, conc, dark = M("pit_wall"), M("pit_frame"), M("pit_concrete"), M("pit_wall_dark")
    b = apex.Builder("garage_upper_6m")
    Z_F0 = GARAGE_ROOF            # floor slab underside on the parapets
    Z_F1 = Z_F0 + 0.25            # finished floor
    Z_C = 13.7                    # ceiling / roof slab underside
    Z_R = 14.0                    # roof terrace
    Y_G = 0.3                     # glazing line
    Y_BAL = -1.5                  # balcony edge
    Y_LIP = -1.95                 # canopy lip

    # Floor slab, back wall, end walls (half each side), roof slab.
    b.box(conc, (-3, Y_BAL, Z_F0), (3, 12.0, Z_F1))
    b.box(frame, (-3, Y_BAL - 0.05, Z_F0 - 0.05), (3, Y_BAL, Z_F1 + 0.03))
    b.box(wall, (-3, 11.75, Z_F1), (3, 12.0, Z_C))
    for s in (-1, 1):
        x0, x1 = sorted((s * 3.0, s * 2.9))
        b.box(wall, (x0, Y_G, Z_F1), (x1, 11.75, Z_C))
    b.box(conc, (-3, Y_G, Z_C), (3, 12.0, Z_R))
    b.box(interior, (-3, Y_LIP + 0.15, Z_C), (3, Y_G, Z_C + 0.05))      # lit soffit
    b.box(conc, (-3, Y_LIP + 0.15, Z_C + 0.05), (3, Y_G, Z_R))
    b.box(frame, (-3, Y_LIP, Z_C - 0.12), (3, Y_LIP + 0.15, Z_R + 0.08))  # thin lip
    # Glazing: mullions (split at the module edges), sill, transom, panes.
    for x0, x1 in ((-3.0, -2.95), (2.95, 3.0), (-1.54, -1.46), (-0.04, 0.04), (1.46, 1.54)):
        b.box(frame, (x0, Y_G - 0.05, Z_F1), (x1, Y_G + 0.05, Z_C))
    for z0, z1 in ((Z_F1, Z_F1 + 0.1), (12.82, 12.9)):
        b.box(frame, (-3.0, Y_G - 0.05, z0), (3.0, Y_G + 0.05, z1))
    cols = [(-2.95, -1.54), (-1.46, -0.04), (0.04, 1.46), (1.54, 2.95)]
    rows = [(Z_F1 + 0.1, 12.82), (12.9, Z_C)]
    lit = [(0, 3), (1, 0), (3, 3), (2, 1), (0, 0), (3, 2), (2, 2), (1, 1)]
    for ci, (x0, x1) in enumerate(cols):
        for ri, (z0, z1) in enumerate(rows):
            cx, cy = lit[(ci * 2 + ri) % len(lit)]
            u0, v0 = cx / 4 + 0.02, cy / 4 + 0.02
            u1, v1 = u0 + 0.21, v0 + 0.21
            b.quad_uv(glass, [(x0, Y_G, z0), (x1, Y_G, z0), (x1, Y_G, z1), (x0, Y_G, z1)],
                      [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    # Balcony: glass balustrade, posts, handrail.
    def balustrade(y, z0, h):
        uv_box(b, glass, (-3.0, y - 0.012, z0 + 0.06), (3.0, y + 0.012, z0 + h - 0.04))
        b.box(frame, (-3.0, y - 0.04, z0 + h - 0.04), (3.0, y + 0.04, z0 + h + 0.02))
        b.box(frame, (-3.0, y - 0.03, z0), (3.0, y + 0.03, z0 + 0.06))
        for x in (-2.25, -0.75, 0.75, 2.25):
            b.box(frame, (x - 0.025, y - 0.03, z0), (x + 0.025, y + 0.03, z0 + h))
    balustrade(Y_BAL + 0.08, Z_F1, 1.1)
    balustrade(Y_LIP + 0.25, Z_R + 0.08, 1.1)
    # Roof terrace: a deck kerb, the plant screen across the back with louvres.
    b.box(conc, (-3, Y_LIP + 0.15, Z_R + 0.08 - 0.08), (3, Y_LIP + 0.4, Z_R + 0.08))
    SY0, SY1, SZ = 8.9, 11.9, Z_R + 2.3
    b.box(dark, (-3, SY0 + 0.15, Z_R), (3, SY1, SZ))
    b.box(frame, (-3, SY0 + 0.05, SZ), (3, SY1 + 0.05, SZ + 0.1))
    for i in range(9):
        z = Z_R + 0.2 + i * 0.24
        prism(b, frame, [(SY0, z), (SY0 + 0.15, z + 0.0), (SY0 + 0.15, z + 0.06), (SY0, z + 0.14)], 'X', -3.0, 3.0)
    for x0, x1 in ((-3.0, -2.95), (2.95, 3.0), (-0.03, 0.03)):
        b.box(frame, (x0, SY0, Z_R), (x1, SY0 + 0.15, SZ))
    # A pair of planters on the terrace, so it reads as a terrace.
    for x in (-1.5, 1.5):
        b.box(dark, (x - 0.6, 6.4, Z_R), (x + 0.6, 7.0, Z_R + 0.55))
        b.box(apex.material("pit_planting", (0.12, 0.25, 0.08), roughness=0.9),
              (x - 0.55, 6.45, Z_R + 0.55), (x + 0.55, 6.95, Z_R + 0.6))

    ob = b.finish(recalc=False)
    for dx in (-6.0, 6.0):
        ctx_copy(ob, dx)
    for dx in (-6.0, 6.0):
        ctx_copy(garage, dx)
    return finish_asset("pit", "garage_upper_6m", ob,
                        three_q=((20, -24, 19), (0, 4, 10)), driver=((-24, -14, 1.3), (0, 4, 9)), pitch=6.0,
                        extra=[("_c", ((-12, 22, 20), (0, 5, 12)))])


def garage_roof_membrane_6m():
    new_scene()
    garage, _, _ = steal_garage()
    frame = M("pit_frame")
    mem = membrane_mat("pit_membrane", 75)
    b = apex.Builder("garage_roof_membrane_6m")
    Y_F, Y_B = -0.8, 12.6
    Z_C = 11.4          # corner height
    H = 3.6
    MAST_Y, MAST_TOP = 5.9, 16.6

    # Corner posts on the parapets and their outrigger arms, split in half.
    for s in (-1, 1):
        x0, x1 = sorted((s * 3.0, s * 2.88))
        for (py0, py1, ay0, ay1) in ((-0.1, 0.15, Y_F, 0.15), (11.75, 12.0, 11.75, Y_B)):
            b.box(frame, (x0, py0, GARAGE_ROOF), (x1, py1, Z_C - 0.1))
            b.box(frame, (x0, ay0, Z_C - 0.2), (x1, ay1, Z_C - 0.02))

    def ye(u, back):
        return (Y_B - 0.35 * (1 - u * u)) if back else (Y_F + 0.35 * (1 - u * u))

    def P(u, v):
        y = ye(u, False) + (ye(u, True) - ye(u, False)) * v
        sv = math.sin(math.pi * v)
        z = Z_C + 0.5 * (1 - u * u) * (1 - sv) - 0.45 * u * u * sv
        return (3.0 * u, y, z + H * peak(u, 0.07, 1.6) * peak((v - 0.5) / 0.5, 0.06, 1.6))

    nu, nv = 24, 40
    grid = [[P(-1 + 2 * i / nu, j / nv) for j in range(nv + 1)] for i in range(nu + 1)]
    shell(b, mem, grid, 0.04,
          lambda side: {'u0': (-1, 0, 0), 'u1': (1, 0, 0), 'v0': (0, -1, 0), 'v1': (0, 1, 0)}[side])
    ring_tube(b, frame, [grid[i][0] for i in range(nu + 1)], 0.04, segs=6)
    ring_tube(b, frame, [grid[i][nv] for i in range(nu + 1)], 0.04, segs=6)

    apex_pt = Vector(P(0, 0.5))
    b.box(frame, (-0.4, MAST_Y - 0.4, 9.4), (0.4, MAST_Y + 0.4, 9.55))
    b.cylinder(frame, (0, MAST_Y, 9.55), 0.17, MAST_TOP - 9.55, segs=12)
    b.cylinder(frame, (0, MAST_Y, MAST_TOP), 0.22, 0.15, segs=12)
    torus(b, frame, apex_pt + Vector((0, 0, 0.05)), 0.42, 0.07, segs=16, rings=6)
    top = Vector((0, MAST_Y, MAST_TOP - 0.05))
    for s in (-1, 1):
        for yy in (Y_F, Y_B):
            b.bar(frame, top, (s * 2.97, yy, Z_C), 0.025, segs=6)
    for yy in (Y_F + 0.35, Y_B - 0.35):
        b.bar(frame, top, (0, yy, Z_C + 0.5), 0.025, segs=6)

    ob = b.finish(recalc=False)
    for dx in (-6.0, 6.0):
        ctx_copy(ob, dx)
        ctx_copy(garage, dx)
    return finish_asset("pit", "garage_roof_membrane_6m", ob,
                        three_q=((20, -24, 19), (0, 4, 10)), driver=((-24, -14, 1.3), (0, 4, 9)), pitch=6.0,
                        extra=[("_c", ((-12, 22, 20), (0, 5, 12)))])


# ==================================================================== bridges
def bridge_mats():
    return dict(steel=M("bridge_steel"), dark=M("bridge_dark"),
                glass=apex.material("bridge_glass", (0.04, 0.06, 0.09), metallic=0.2, roughness=0.1),
                conc=apex.material("bridge_concrete", (0.55, 0.54, 0.52), roughness=0.9))


def sky_platform():
    """Two glazed wings, one from each side, cantilevered from a concrete
    core and propped by raking struts, their tips 1.6 m apart over the road
    centre. Underside >= 9.8 m over the road."""
    new_scene()
    m = bridge_mats()
    steel, dark, glass, conc = m["steel"], m["dark"], m["glass"], m["conc"]
    b = apex.Builder("sky_platform")
    Y_ROOT, Y_CORE, Y_TIP = 15.0, 12.0, 0.8
    NS, NR = 40, 36   # stations along the span, points round the section

    Q_ROOT = -(Y_ROOT - Y_CORE) / (Y_CORE - Y_TIP)

    def station(q):
        """q 0 at the core face (|y| = Y_CORE) .. 1 at the tip; below 0 the
        wing's back end rounds off into the core."""
        if q < 0:
            r = q / Q_ROOT
            chord = 9.0 + 17.0 * math.sqrt(max(0.0, 1.0 - r * r))
            return chord, 3.4, 11.6, 0.0
        chord = 26.0 - 13.0 * q ** 1.2
        thick = 3.4 - 1.0 * q
        zm = 11.6 + 1.6 * q * q
        sweep = 1.2 * q * q   # tips drift toward +x a little
        return chord, thick, zm, sweep

    def section(q):
        chord, thick, zm, sweep = station(q)
        pts = []
        for k in range(NR):
            th = 2 * math.pi * k / NR
            c, s = math.cos(th), math.sin(th)
            x = chord / 2 * math.copysign(abs(c) ** 0.35, c)
            # flatter top, fuller belly
            z = zm + (thick * 0.42 if s > 0 else thick * 0.58) * math.copysign(abs(s) ** 0.7, s)
            pts.append((x + sweep, z, k))
        return pts

    def seg_mat(k):
        th = 2 * math.pi * (k + 0.5) / NR
        s = math.sin(th)
        if abs(s) < 0.62:
            return glass       # the gallery's side glazing
        return steel

    for side in (-1, 1):
        qs = [Q_ROOT * (1 - i / 6) for i in range(6)] + [i / NS for i in range(NS + 1)]
        rings = []
        for q in qs:
            ay = Y_CORE - (Y_CORE - Y_TIP) * q
            rings.append([Vector((x, side * ay, z)) for (x, z, _) in section(q)])
        for a, c in zip(rings, rings[1:]):
            cen = (sum(a, Vector()) + sum(c, Vector())) / (2 * NR)
            for k in range(NR):
                k2 = (k + 1) % NR
                q4 = [a[k], a[k2], c[k2], c[k]]
                fc = sum(q4, Vector()) / 4
                out = fc - cen
                out.y = 0
                oface(b, seg_mat(k), q4, out=out)
        # Caps: the tip is a glazed end, the root is closed against the core.
        oface(b, glass, rings[-1], out=(0, -side, 0))
        oface(b, dark, rings[0], out=(0, side, 0))
        # Mullion fins on the glazing, following the section.
        for i in range(1, NS, 2):
            q = i / NS
            ay = Y_CORE - (Y_CORE - Y_TIP) * q
            sec = section(q)
            for half in (range(-NR // 4, NR // 4 + 1), range(NR // 4, 3 * NR // 4 + 1)):
                pts = []
                for k in half:
                    th = 2 * math.pi * k / NR
                    if abs(math.sin(th)) > 0.66:
                        continue
                    x, z, _ = sec[k % NR]
                    sx = 1 if math.cos(th) > 0 else -1
                    pts.append(Vector((x + sx * 0.04, side * ay, z)))
                if len(pts) > 1:
                    ring_tube(b, steel, pts, 0.05, segs=4)
        # The concrete core and its lift glazing.
        y0, y1 = sorted((side * 11.0, side * 16.0))
        prism(b, conc, chamfer_rect(0, side * 13.5, 5.0, 2.5, 0.8), 'Z', 0.0, 15.0)
        prism(b, conc, chamfer_rect(0, side * 13.5, 5.2, 2.7, 0.85), 'Z', 15.0, 15.4)
        prism(b, glass, chamfer_rect(0, side * 13.5, 5.04, 2.54, 0.8), 'Z', 13.5, 14.6)
        prism(b, dark, chamfer_rect(0, side * 13.5, 5.06, 2.56, 0.8), 'Z', 9.2, 9.5)
        b.box(dark, (-1.2, min(side * 10.95, side * 11.0), 0.6), (1.2, max(side * 10.95, side * 11.0), 10.2))
        uv_box(b, glass, (-1.0, min(side * 10.9, side * 10.95), 0.8), (1.0, max(side * 10.9, side * 10.95), 10.0),
               uv=(0.1, 0.1, 0.9, 0.9))
        # Raking struts (outside the road) under the wing's outer chord.
        for sx in (-1, 1):
            q = (Y_CORE - 9.5) / (Y_CORE - Y_TIP)
            chord, thick, zm, sweep = station(q)
            under = zm - thick * 0.58 * 0.75
            foot = Vector((sx * 9.5, side * 13.0, 0.0))
            head = Vector((sweep + sx * chord * 0.33, side * 9.5, under))
            b.bar(steel, foot + Vector((0, 0, 0.4)), head, 0.32, segs=12)
            b.box(conc, (foot.x - 0.8, foot.y - 0.8, 0.0), (foot.x + 0.8, foot.y + 0.8, 0.45))
            b.ico(dark, head, 0.45, subdiv=1)
            # a tie from the strut to the core
            mid = foot + (head - foot) * 0.45
            b.bar(steel, mid, Vector((sx * 5.0, side * 13.5, mid.z)), 0.12, segs=8)

    ob = b.finish(recalc=False)
    ctx_road()
    return finish_asset("bridge", "sky_platform", ob,
                        three_q=((-40, -34, 18), (0, 0, 9)), driver=((-45, -2.5, 1.2), (0, 0, 9)),
                        extra=[("_c", ((34, -12, 32), (0, 0, 10)))])


def banked_oval_10m():
    """A straight 10 m module of the old banked oval beside the road: the
    13 m concrete deck banked from 8 degrees at its inner edge to 40 at
    the outer (the outside of the curve on +Y), its inner edge 1 m up, on
    two rows of piers, armco along the top lip. Tiled along the oval's
    mapped line, which runs through the module's middle."""
    new_scene()
    m = bridge_mats()
    conc, dark = m["conc"], m["dark"]
    galv = M("armco_galv")
    b = apex.Builder("banked_oval_10m")
    W, Z_IN, SLAB, NW = 13.0, 1.0, 0.55, 16
    prof = [(0.0, 0.0)]
    steps = 160
    for i in range(1, steps + 1):
        a = math.radians(8 + 32 * (i / steps) ** 1.6)
        h0, z0 = prof[-1]
        prof.append((h0 + math.cos(a) * W / steps, z0 + math.sin(a) * W / steps))
    HW = prof[-1][0]

    def pt(x, s, dz=0.0):
        i = min(steps - 1, int(s / W * steps))
        f = s / W * steps - i
        (h0, z0), (h1, z1) = prof[i], prof[i + 1]
        return Vector((x, -HW / 2 + h0 + (h1 - h0) * f, Z_IN + z0 + (z1 - z0) * f + dz))

    ss = [W * j / NW for j in range(NW + 1)]
    for j in range(NW):
        a0, a1 = ss[j], ss[j + 1]
        oface(b, conc, [pt(-5, a0), pt(5, a0), pt(5, a1), pt(-5, a1)], out=(0, 0, 1))
        oface(b, conc, [pt(-5, a0, -SLAB), pt(5, a0, -SLAB), pt(5, a1, -SLAB), pt(-5, a1, -SLAB)], out=(0, 0, -1))
        for x, sx in ((-5, -1), (5, 1)):   # module ends, closed (a run's end is open otherwise)
            oface(b, conc, [pt(x, a0), pt(x, a1), pt(x, a1, -SLAB), pt(x, a0, -SLAB)], out=(sx, 0, 0))
    oface(b, conc, [pt(-5, 0), pt(5, 0), pt(5, 0, -SLAB), pt(-5, 0, -SLAB)], out=(0, -1, 0))
    oface(b, conc, [pt(-5, W), pt(5, W), pt(5, W, -SLAB), pt(-5, W, -SLAB)], out=(0, 1, 0))
    # An expansion joint across the deck.
    for j in range(NW):
        oface(b, dark, [pt(-0.06, ss[j], 0.03), pt(0.06, ss[j], 0.03), pt(0.06, ss[j + 1], 0.03),
                        pt(-0.06, ss[j + 1], 0.03)], out=(0, 0, 1))
    # The inner kerb wall down to the ground and piers under the deck.
    b.box(conc, (-5.0, -HW / 2 - 0.3, 0.0), (5.0, -HW / 2 + 0.2, Z_IN))
    for s_at in (W * 0.45, W * 0.85):
        top = pt(0, s_at, -SLAB)
        for x in (-2.5, 2.5):
            b.box(conc, (x - 0.45, top.y - 0.45, 0.0), (x + 0.45, top.y + 0.45, top.z))
    # Lip parapet and armco on it.
    lip = pt(0, W)
    b.box(conc, (-5.0, lip.y, lip.z - SLAB), (5.0, lip.y + 0.35, lip.z + 0.9))
    b.box(galv, (-5.0, lip.y - 0.12, lip.z + 0.45), (5.0, lip.y - 0.02, lip.z + 0.8))
    for x in (-3.75, -1.25, 1.25, 3.75):
        b.box(dark, (x - 0.05, lip.y - 0.1, lip.z), (x + 0.05, lip.y, lip.z + 0.85))
    ob = b.finish(recalc=False)
    return finish_asset("building", "banked_oval_10m", ob,
                        three_q=((-18, -22, 9), (0, 2, 3)), driver=((-25, -3, 1.2), (0, 0, 3)))


def banked_oval_span():
    """The old banked oval crossing over the road: a 13 m concrete deck
    curving in plan (radius 60 m) and banked from 8 degrees at its inner
    edge to 40 at the outer, on rows of piers clear of the road, with deep
    girders over the road gap. Inner edge 8 m up over the road."""
    new_scene()
    m = bridge_mats()
    conc, dark = m["conc"], m["dark"]
    galv = M("armco_galv")
    b = apex.Builder("banked_oval_span")
    Y_HALF = 22.0
    W = 13.0
    Z_IN = 8.0
    SLAB = 0.55
    R = 60.0
    NY, NW = 44, 16

    # The banking profile: horizontal distance h(s) and rise z(s) along the deck width.
    prof = [(0.0, 0.0)]
    steps = 160
    for i in range(1, steps + 1):
        s = W * i / steps
        a = math.radians(8 + 32 * (s / W) ** 1.6)
        h0, z0 = prof[-1]
        prof.append((h0 + math.cos(a) * W / steps, z0 + math.sin(a) * W / steps))
    HW = prof[-1][0]

    def prof_at(s):
        i = min(steps - 1, int(s / W * steps))
        f = s / W * steps - i
        (h0, z0), (h1, z1) = prof[i], prof[i + 1]
        return h0 + (h1 - h0) * f, z0 + (z1 - z0) * f

    def frame(y):
        """Centre (x, y) of the inner edge and the outward horizontal normal."""
        x = y * y / (2 * R)
        t = Vector((y / R, 1.0, 0.0)).normalized()
        n = Vector((-t.y, t.x, 0.0))   # toward -x: the outside of the curve
        return Vector((x + HW * 0.5, y, 0.0)), n

    def pt(y, s, dz=0.0):
        c, n = frame(y)
        h, z = prof_at(s)
        return c + n * h + Vector((0, 0, Z_IN + z + dz))

    ys = [-Y_HALF + 2 * Y_HALF * i / NY for i in range(NY + 1)]
    ss = [W * j / NW for j in range(NW + 1)]
    top = [[pt(y, s) for s in ss] for y in ys]
    bot = [[pt(y, s, -SLAB) for s in ss] for y in ys]
    for i in range(NY):
        for j in range(NW):
            oface(b, conc, [top[i][j], top[i + 1][j], top[i + 1][j + 1], top[i][j + 1]], out=(0, 0, 1))
            oface(b, conc, [bot[i][j], bot[i + 1][j], bot[i + 1][j + 1], bot[i][j + 1]], out=(0, 0, -1))
    for i in range(NY):
        _, n = frame((ys[i] + ys[i + 1]) / 2)
        oface(b, conc, [top[i][0], top[i + 1][0], bot[i + 1][0], bot[i][0]], out=-n)
        oface(b, conc, [top[i][NW], top[i + 1][NW], bot[i + 1][NW], bot[i][NW]], out=n)
    for i, sgn in ((0, -1), (NY, 1)):
        for j in range(NW):
            oface(b, conc, [top[i][j], top[i][j + 1], bot[i][j + 1], bot[i][j]], out=(0, sgn, 0))
    # Expansion joints across the deck, dark, a hair proud.
    for y in (-15.0, -9.0, 9.0, 15.0):
        for j in range(NW):
            a, c = pt(y - 0.06, ss[j], 0.03), pt(y + 0.06, ss[j], 0.03)
            d, e = pt(y + 0.06, ss[j + 1], 0.03), pt(y - 0.06, ss[j + 1], 0.03)
            oface(b, dark, [a, c, d, e], out=(0, 0, 1))
    # Outer parapet lip and the armco on posts along the top.
    lip = []
    for y in ys:
        c, n = frame(y)
        p = pt(y, W)
        lip.append((p, n))
    for (p0, n0), (p1, n1) in zip(lip, lip[1:]):
        q_out = [p0 + n0 * 0.35, p1 + n1 * 0.35]
        corners0 = [p0 - Vector((0, 0, SLAB)), p0 + n0 * 0.35 - Vector((0, 0, SLAB)), p0 + n0 * 0.35 + Vector((0, 0, 0.9)), p0 + Vector((0, 0, 0.9))]
        corners1 = [p1 - Vector((0, 0, SLAB)), p1 + n1 * 0.35 - Vector((0, 0, SLAB)), p1 + n1 * 0.35 + Vector((0, 0, 0.9)), p1 + Vector((0, 0, 0.9))]
        cen = (sum(corners0, Vector()) + sum(corners1, Vector())) / 8
        for k in range(4):
            k2 = (k + 1) % 4
            q4 = [corners0[k], corners0[k2], corners1[k2], corners1[k]]
            oface(b, conc, q4, out=sum(q4, Vector()) / 4 - cen)
    oface(b, conc, [p for p in [lip[0][0] - Vector((0, 0, SLAB)), lip[0][0] + lip[0][1] * 0.35 - Vector((0, 0, SLAB)),
                                lip[0][0] + lip[0][1] * 0.35 + Vector((0, 0, 0.9)), lip[0][0] + Vector((0, 0, 0.9))]], out=(0, -1, 0))
    oface(b, conc, [p for p in [lip[-1][0] - Vector((0, 0, SLAB)), lip[-1][0] + lip[-1][1] * 0.35 - Vector((0, 0, SLAB)),
                                lip[-1][0] + lip[-1][1] * 0.35 + Vector((0, 0, 0.9)), lip[-1][0] + Vector((0, 0, 0.9))]], out=(0, 1, 0))
    # Armco: a W-beam swept along the top, 0.6 m inboard of the lip, on posts.
    wprof = [(0.0, 0.0), (0.1, 0.0), (0.1, 0.32), (0.0, 0.32), (-0.04, 0.24), (-0.04, 0.08)]
    rail_ys = [-Y_HALF + 0.3 + (2 * Y_HALF - 0.6) * i / 60 for i in range(61)]
    rings = []
    for y in rail_ys:
        c, n = frame(y)
        base = pt(y, W - 0.6) + Vector((0, 0, 0.55))
        # beam faces the deck (inboard, -n)
        rings.append([base + n * dx + Vector((0, 0, dz)) for (dx, dz) in wprof])
    for a, c in zip(rings, rings[1:]):
        for k in range(len(wprof)):
            k2 = (k + 1) % len(wprof)
            q4 = [a[k], a[k2], c[k2], c[k]]
            fc = sum(q4, Vector()) / 4
            mid = (sum(a, Vector()) + sum(c, Vector())) / (2 * len(wprof))
            oface(b, galv, q4, out=fc - mid)
    for y in rail_ys[::4]:
        c, n = frame(y)
        p = pt(y, W - 0.6) + n * 0.12
        prism(b, galv, chamfer_rect(p.x, p.y, 0.06, 0.06, 0.02), 'Z', p.z - 0.3, p.z + 0.85)
    # Inner kerb (dark), low.
    kerb = [pt(y, 0.0) for y in ys]
    for p0, p1, y0, y1 in zip(kerb, kerb[1:], ys, ys[1:]):
        _, n = frame((y0 + y1) / 2)
        c0 = [p0, p0 + n * 0.5, p0 + n * 0.5 + Vector((0, 0, 0.12)), p0 + Vector((0, 0, 0.12))]
        c1 = [p1, p1 + n * 0.5, p1 + n * 0.5 + Vector((0, 0, 0.12)), p1 + Vector((0, 0, 0.12))]
        cen = (sum(c0, Vector()) + sum(c1, Vector())) / 8
        for k in range(4):
            k2 = (k + 1) % 4
            q4 = [c0[k], c0[k2], c1[k2], c1[k]]
            oface(b, dark, q4, out=sum(q4, Vector()) / 4 - cen)

    # Longitudinal girders under the deck (the span over the road).
    for s in (2.0, 6.5, 11.0):
        g_ys = [-Y_HALF + 0.2 + (2 * Y_HALF - 0.4) * i / 22 for i in range(23)]
        depth = 1.1
        rr = []
        for y in g_ys:
            c, n = frame(y)
            p = pt(y, s, -SLAB + 0.02)
            t = Vector((-n.y, n.x, 0.0))
            rr.append([p + n * dx + Vector((0, 0, dz)) for (dx, dz) in
                       ((-0.35, 0.0), (0.35, 0.0), (0.3, -depth), (-0.3, -depth))])
        for a, c in zip(rr, rr[1:]):
            mid = (sum(a, Vector()) + sum(c, Vector())) / 8
            for k in range(4):
                k2 = (k + 1) % 4
                q4 = [a[k], a[k2], c[k2], c[k]]
                oface(b, conc, q4, out=sum(q4, Vector()) / 4 - mid)
        for ring, sgn in ((rr[0], -1), (rr[-1], 1)):
            oface(b, conc, ring, out=(0, sgn, 0))

    # Pier frames clear of the road: columns under the girders, a cap beam.
    road_clear = 9.0
    for y in (-21.0, -15.0, -road_clear, road_clear, 15.0, 21.0):
        c, n = frame(y)
        caps = []
        for s in (2.0, 6.5, 11.0):
            p = pt(y, s, -SLAB - 1.1)
            prism(b, conc, chamfer_rect(p.x, p.y, 0.4, 0.4, 0.12), 'Z', 0.0, p.z - 0.6)
            # flared capital
            prism(b, conc, chamfer_rect(p.x, p.y, 0.55, 0.55, 0.15), 'Z', p.z - 0.6, p.z + 0.02)
            # footing
            b.box(conc, (p.x - 0.8, p.y - 0.8, 0.0), (p.x + 0.8, p.y + 0.8, 0.3))
            caps.append(p)
        # transverse ties between the columns at mid height
        for p0, p1 in zip(caps, caps[1:]):
            zt = min(p0.z, p1.z) * 0.45
            q0 = Vector((p0.x, p0.y, zt))
            q1 = Vector((p1.x, p1.y, zt))
            d = (q1 - q0).normalized()
            side = Vector((-d.y, d.x, 0)) * 0.2
            prism_pts = [q0 - side, q1 - side, q1 + side, q0 + side]
            lo = [p - Vector((0, 0, 0.25)) for p in prism_pts]
            hi = [p + Vector((0, 0, 0.25)) for p in prism_pts]
            oface(b, conc, lo, out=(0, 0, -1))
            oface(b, conc, hi, out=(0, 0, 1))
            for k in range(4):
                k2 = (k + 1) % 4
                q4 = [lo[k], lo[k2], hi[k2], hi[k]]
                oface(b, conc, q4, out=sum(q4, Vector()) / 4 - (q0 + q1) / 2)

    ob = b.finish(recalc=False)
    ctx_road()
    return finish_asset("bridge", "banked_oval_span", ob,
                        three_q=((-36, -34, 16), (0, 0, 7)), driver=((-45, -2.5, 1.2), (0, 0, 8)),
                        extra=[("_c", ((40, 30, 22), (0, 0, 8)))])


BUILD = {
    "garage_upper_6m": garage_upper_6m,
    "garage_roof_membrane_6m": garage_roof_membrane_6m,
    "tent_roof_10m": tent_roof_10m,
    "palm_canopy_10m": palm_canopy_10m,
    "lotus_canopy_10m": lotus_canopy_10m,
    "stone_tribune_10m": stone_tribune_10m,
    "stone_tribune_centre_10m": stone_tribune_centre_10m,
    "sky_platform": sky_platform,
    "banked_oval_span": banked_oval_span,
    "banked_oval_10m": banked_oval_10m,
}

if __name__ == "__main__":
    keys = list(BUILD) if ASSET == "all" else [k.strip() for k in ASSET.split(",")]
    for k in keys:
        BUILD[k]()
