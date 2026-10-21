"""The parked spectator vehicles and the camp tent, rebuilt in place:
`vehicle/car_a` (a small hatchback), `vehicle/car_b` (a saloon),
`vehicle/car_c` (an SUV / estate), `vehicle/camper_van` (a high-roof panel
van camper), `vehicle/motorhome` (a coach-built motorhome with its awning
out) and `attraction/tent_6m` (a peaked pagoda marquee).

Why: the cars are laid ~20,000 times across the calendar in car parks and the
camper, motorhome and tent by the hundred in campsites and fan zones; they
were boxes and wedges that read as bricks from the grandstand. Now each has a
car's silhouette (bonnet, raked screen, roof, tail, wheel arches, wheels with
rims, mirrors, bumpers, front and rear light clusters), inside a budget an
instanced prop can carry (cars <= 1200 tris, camper / motorhome <= 3000,
tent <= 1500).

Nothing a scene, `props.rs` or the importer knows moves: each asset keeps
its key, its footprint (the old bbox within a few cm), its pivot (ground,
centred; front +X; the camper's awning cassette on +Y, the motorhome's
awning out on -Y, the tent open to the road on -Y and to +X) and every slot
it had, copied out of its own current GLB (`_own_mats`), so a slot means the
same material it did. The import shares a material by name across a kind (the
first GLB imported owns it), so a slot this script adds is defined once here
and is the same in every GLB that uses it: `vehicle_trim` (black plastic),
`vehicle_rim` (alloy), `vehicle_lamp_front`, `vehicle_lamp_rear`;
`vehicle_glass` comes from car_c.glb, where it already was.

Headless:  blender -b --python tools/generators/props/build_vehicles.py
(from apexsim-content/). ASSET = "all" or one key (env var or global).
Previews: props/_preview/<key>.png (front 3/4), <key>_b.png (rear 3/4),
<key>_far.png (a row of them from far off).
"""
import importlib.util, math, os, sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))
_spec = importlib.util.spec_from_file_location("apex", os.path.join(_HERE, "apex_props.py"))
apex = importlib.util.module_from_spec(_spec)
sys.modules["apex"] = apex
_spec.loader.exec_module(apex)

import bpy
from mathutils import Vector

ASSET = os.environ.get("ASSET", globals().get("ASSET", "all"))

# The slots each asset had before this rebuild, taken from its own GLB.
OWN_SLOTS = {
    "car_a": ["vehicle_paint_a", "vehicle_tyre"],
    "car_b": ["vehicle_paint_b", "vehicle_tyre"],
    "car_c": ["vehicle_paint_c", "vehicle_glass", "vehicle_tyre"],
    # The camper's off-white paint was named vehicle_paint_a, car_a's red,
    # so the first one imported painted both: it is its own slot now (the
    # old name is still read, for a GLB from before the rename).
    "camper_van": ["vehicle_paint_camper", "vehicle_paint_a", "vehicle_tyre", "bridge_dark", "pit_glass", "tent_white", "scaffold_plank"],
    "motorhome": ["truck_body", "pit_glass", "bridge_dark", "vehicle_tyre", "pit_wall", "bridge_steel"],
    "tent_6m": ["bridge_steel", "tent_white", "tent_colour", "scaffold_plank"],
}
KIND = {"tent_6m": "attraction"}
RENAMED = {"vehicle_paint_camper": "vehicle_paint_a"}


# ------------------------------------------------------------------ materials
def _import_mats(kind, glb, names):
    """Import `glb`, keep the materials named in `names` (exact names), drop
    its objects and every other material it brought in."""
    before_m = set(bpy.data.materials)
    before_o = set(bpy.data.objects)
    before_me = set(bpy.data.meshes)
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=os.path.join(apex.PROPS_ROOT, kind, glb))
    for ob in set(bpy.data.objects) - before_o:
        bpy.data.objects.remove(ob, do_unlink=True)
    for me in set(bpy.data.meshes) - before_me:
        bpy.data.meshes.remove(me)
    got = {}
    for m in set(bpy.data.materials) - before_m:
        base = m.name.split(".")[0]
        if base in names and base not in got and bpy.data.materials.get(base) in (None, m):
            m.name = base
            got[base] = m
    for m in set(bpy.data.materials) - before_m:
        if m not in got.values():
            bpy.data.materials.remove(m)
    return got


def _own_mats(key, extra=()):
    """The asset's own slots out of its current GLB, plus `extra` slots
    (name, kind, glb) taken from another GLB of the kind."""
    kind = KIND.get(key, "vehicle")
    got = _import_mats(kind, key + ".glb", set(OWN_SLOTS[key]))
    missing = [n for n in OWN_SLOTS[key] if n not in got]
    # A renamed slot needs only one of its names.
    for new, old in RENAMED.items():
        if new in got or old in got:
            missing = [n for n in missing if n not in (new, old)]
    if missing:
        raise KeyError("%s.glb lacks %s" % (key, missing))
    for name, k, glb in extra:
        if name not in got:
            got.update(_import_mats(k, glb, {name}))
    return got


def new_vehicle_slots():
    """The slots this script adds to the vehicle kind: one definition each."""
    return dict(
        trim=apex.material("vehicle_trim", (0.035, 0.036, 0.04), roughness=0.6),
        rim=apex.material("vehicle_rim", (0.62, 0.63, 0.65), metallic=0.8, roughness=0.3),
        lamp_f=apex.material("vehicle_lamp_front", (0.85, 0.87, 0.9), metallic=0.3, roughness=0.12),
        lamp_r=apex.material("vehicle_lamp_rear", (0.62, 0.03, 0.03), roughness=0.2),
    )


# ------------------------------------------------------------------- geometry
def interp(pts, x):
    """Piecewise-linear y at x over sorted (x, y) control points, clamped."""
    if x <= pts[0][0]:
        return pts[0][1]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            t = (x - x0) / (x1 - x0) if x1 > x0 else 0.0
            return y0 + (y1 - y0) * t
    return pts[-1][1]


def arch_z(x, arches, base):
    """The body's underside at x: `base` (sill / bumper bottom) except over a
    wheel arch, a half circle of radius ra round each run of wheel centres
    (xa..xb at height zc: one axle, or a twin axle under one arch)."""
    z = base
    for xa, xb, zc, ra in arches:
        if xa - ra < x < xb + ra:
            d = 0.0 if xa <= x <= xb else min(abs(x - xa), abs(x - xb))
            z = max(z, zc + math.sqrt(max(0.0, ra * ra - d * d)))
    return z


def arch_stations(arches, n=4):
    xs = []
    for xa, xb, zc, ra in arches:
        for k in range(-n, n + 1):
            a = math.pi / 2 * k / n
            dx = ra * math.sin(a)
            xs.append((xa if dx < 0 else xb) + dx)
        xs.append(xa - ra - 0.04)
        xs.append(xb + ra + 0.04)
        if xb > xa:
            xs.append((xa + xb) / 2)
    return xs


def merge_xs(xs, tol=0.025):
    out = []
    for x in sorted(xs):
        if not out or x - out[-1] > tol:
            out.append(x)
    return out


def loft(b, sections, edge_mats, cap_mats=(None, None)):
    """Skin rings of (y, z) sections along +X. `sections` is [(x, [(y, z)...])],
    each ring clockwise seen from +X (left side up, over the top, down the
    right) so the quads face out; `edge_mats(i_section, i_edge)` picks the
    material of the quad between section i and i+1 on edge i_edge (the last
    edge closes the ring along the bottom). Caps are n-gons, None to skip."""
    rings = []
    for x, sec in sections:
        rings.append([b.bm.verts.new((x, y, z)) for (y, z) in sec])
    n = len(rings[0])
    for s, (ra, rb) in enumerate(zip(rings, rings[1:])):
        for i in range(n):
            j = (i + 1) % n
            f = b.bm.faces.new((ra[i], rb[i], rb[j], ra[j]))
            f.material_index = b.slot(edge_mats(s, i))
    if cap_mats[0] is not None:
        f = b.bm.faces.new(rings[0]); f.material_index = b.slot(cap_mats[0])
    if cap_mats[1] is not None:
        f = b.bm.faces.new(list(reversed(rings[-1]))); f.material_index = b.slot(cap_mats[1])
    return rings


def poly(b, mat, pts):
    """A flat polygon with planar UVs set later (kept out of the normal
    recalculation and the merge: the winding is the facing)."""
    uvs = [(0, 0)] * len(pts)
    return b.quad_uv(mat, pts, uvs)


def side_quad(b, mat, y, x0, x1, z0, z1, x0t=None, x1t=None):
    """A quad on a side wall at y (facing -Y when y < 0, +Y when y > 0);
    x0t / x1t slant the top corners."""
    x0t = x0 if x0t is None else x0t
    x1t = x1 if x1t is None else x1t
    pts = [(x0, y, z0), (x1, y, z0), (x1t, y, z1), (x0t, y, z1)]
    if y > 0:
        pts = pts[::-1]
    return poly(b, mat, pts)


def side_strip(b, mat, y, x0, x1, z0, z1, arches, gap=0.03):
    """A side strip from x0 to x1, broken where it would cross a wheel
    arch (the arch's half circle reaching above z0)."""
    cuts = []
    for xa, xb, zc, ra in arches:
        if zc + ra > z0:
            h = math.sqrt(max(0.0, ra * ra - max(0.0, z0 - zc) ** 2)) + gap
            cuts.append((xa - h, xb + h))
    xs = [x0]
    for c0, c1 in sorted(cuts):
        xs += [c0, c1]
    xs.append(x1)
    for a, c in zip(xs[::2], xs[1::2]):
        a, c = max(a, x0), min(c, x1)
        if c - a > 0.05:
            side_quad(b, mat, y, a, c, z0, z1)


def end_quad(b, mat, x, y0, y1, z0, z1):
    """A quad on an end face at x (facing +X when x > 0, -X when x < 0)."""
    pts = [(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)]
    if x < 0:
        pts = pts[::-1]
    return poly(b, mat, pts)


def wheel(b, m, x, y_out, side, r, w, rim=0.6, segs=12, rim_segs=8, z=None):
    """A tyre (closed cylinder) whose outer face is at y_out, a rim disc just
    proud of it and a dark hub."""
    z = r if z is None else z
    y0 = y_out - w if side > 0 else y_out
    b.cylinder(m["tyre"], (x, y0, z), r, w, segs=segs, axis='Y')
    yo = y_out + 0.006 * side
    pts = [(x + math.cos(2 * math.pi * k / rim_segs + 0.2) * r * rim, yo,
            z + math.sin(2 * math.pi * k / rim_segs + 0.2) * r * rim) for k in range(rim_segs)]
    if side > 0:   # CCW in (x, z) faces -Y
        pts = pts[::-1]
    poly(b, m["rim"], pts)
    yh = y_out + 0.012 * side
    hp = [(x + math.cos(2 * math.pi * k / 5) * r * 0.16, yh, z + math.sin(2 * math.pi * k / 5) * r * 0.16) for k in range(5)]
    if side > 0:
        hp = hp[::-1]
    poly(b, m["trim"], hp)


def finish(b, uv_scale_by_mat=None):
    ob = b.finish(planar_uv=True, uv_scale=1.0, recalc=True)
    # planar UVs for the explicit faces too, and a per-slot UV scale (the
    # motorhome / camper `pit_glass` is 0.25 a metre: one window cell)
    me = ob.data
    uv = me.uv_layers[0].data
    scale = uv_scale_by_mat or {}
    for p in me.polygons:
        n = p.normal
        ax = max(range(3), key=lambda i: abs(n[i]))
        s = scale.get(me.materials[p.material_index].name, 1.0)
        for li in p.loop_indices:
            c = me.vertices[me.loops[li].vertex_index].co
            u = (c.y, c.z) if ax == 0 else (c.x, c.z) if ax == 1 else (c.x, c.y)
            uv[li].uv = (u[0] * s, u[1] * s)
    return ob


# ------------------------------------------------------------------------ cars
# Front +X, centred on y; x0 / x1 the old bbox ends. `top` is the body's top
# line (bumper face, bonnet, cowl, waist, boot or hatch), `halfw` its plan.
# `gh` the greenhouse stations rear to front: (x, height above the waist,
# roof half width) at rear base, roof end, C-pillar, screen top, screen base.
CARS = {
    "car_a": dict(   # small hatchback, 3.9 m
        paint="vehicle_paint_a", x0=-1.9, x1=2.0, w=0.835, r=0.30, tw=0.19,
        axles=(-1.28, 1.24), sill=0.26, zm=0.56, bump_lo=0.27,
        top=[(-1.9, 0.86), (-1.86, 0.93), (-1.74, 0.96), (0.95, 0.90), (1.55, 0.80), (1.86, 0.72), (2.0, 0.62)],
        halfw=[(-1.9, 0.70), (-1.82, 0.80), (-1.6, 0.835), (1.5, 0.835), (1.85, 0.79), (2.0, 0.68)],
        gh=[(-1.80, 0.02, 0.50), (-1.50, 0.43, 0.58), (-1.10, 0.46, 0.61), (0.30, 0.48, 0.62), (1.02, 0.02, 0.72)],
        roof_rails=False, z_roof_max=1.42,
    ),
    "car_b": dict(   # saloon, 4.7 m
        paint="vehicle_paint_b", x0=-2.3, x1=2.4, w=0.84, r=0.315, tw=0.21,
        axles=(-1.37, 1.43), sill=0.25, zm=0.54, bump_lo=0.26,
        top=[(-2.3, 0.82), (-2.26, 0.90), (-2.1, 0.94), (-1.6, 0.95), (1.25, 0.88), (1.95, 0.77), (2.27, 0.70), (2.4, 0.60)],
        halfw=[(-2.3, 0.70), (-2.2, 0.80), (-1.95, 0.84), (1.9, 0.84), (2.25, 0.79), (2.4, 0.66)],
        gh=[(-1.58, 0.02, 0.62), (-0.95, 0.43, 0.62), (-0.70, 0.45, 0.63), (0.55, 0.45, 0.63), (1.27, 0.02, 0.74)],
        roof_rails=False, z_roof_max=1.40,
    ),
    "car_c": dict(   # SUV / estate, 4.55 m
        paint="vehicle_paint_c", x0=-2.2, x1=2.35, w=0.84, r=0.36, tw=0.23,
        axles=(-1.32, 1.43), sill=0.38, zm=0.74, bump_lo=0.36,
        top=[(-2.2, 1.04), (-2.16, 1.10), (-2.04, 1.12), (1.20, 1.08), (1.80, 1.00), (2.2, 0.92), (2.35, 0.82)],
        halfw=[(-2.2, 0.74), (-2.1, 0.82), (-1.9, 0.84), (1.9, 0.84), (2.2, 0.80), (2.35, 0.70)],
        gh=[(-2.08, 0.02, 0.60), (-1.98, 0.58, 0.64), (-1.55, 0.60, 0.65), (0.45, 0.60, 0.65), (1.24, 0.02, 0.74)],
        roof_rails=True, z_roof_max=1.76,
    ),
}


def build_car(key):
    P = CARS[key]
    apex.reset_scene()
    own = _own_mats(key, extra=[("vehicle_glass", "vehicle", "car_c.glb")])
    m = dict(paint=own[P["paint"]], tyre=own["vehicle_tyre"], glass=own["vehicle_glass"], **new_vehicle_slots())
    b = apex.Builder(key)
    X0, X1, r = P["x0"], P["x1"], P["r"]
    ra = r + 0.055
    arches = [(x, x, r, ra) for x in P["axles"]]

    def zt(x):
        return interp(P["top"], x)

    def hw(x):
        return interp(P["halfw"], x)

    def zbot(x):
        base = interp([(X0, P["bump_lo"] + 0.06), (X0 + 0.12, P["bump_lo"]), (X0 + 0.5, P["sill"]),
                       (X1 - 0.5, P["sill"]), (X1 - 0.12, P["bump_lo"]), (X1, P["bump_lo"] + 0.06)], x)
        return arch_z(x, arches, base)

    # ---- the body: a 7-point section lofted along X
    xs = [X0, X0 + 0.04, X0 + 0.12, X0 + 0.3, X1 - 0.3, X1 - 0.12, X1 - 0.04, X1]
    xs += [px for px, _ in P["top"]] + [g[0] for g in P["gh"]] + arch_stations(arches)
    xs += [(P["axles"][0] + P["axles"][1]) / 2]
    xs = [x for x in merge_xs(xs) if X0 <= x <= X1]
    secs = []
    for x in xs:
        w, ztop, zb = hw(x), zt(x), zbot(x)
        zm = min(max(P["zm"], zb + 0.07), ztop - 0.1)
        wl, wt = w - 0.05, w - 0.085
        crown = 0.025 if X0 + 0.1 < x < X1 - 0.1 else 0.0
        secs.append((x, [(-wl, zb), (-w, zm), (-wt, ztop), (0.0, ztop + crown), (wt, ztop), (w, zm), (wl, zb)]))
    loft(b, secs, lambda s, i: m["trim"] if i == 6 else m["paint"], cap_mats=(m["paint"], m["paint"]))

    # ---- the greenhouse: rear base, roof end, C-pillar, screen top, screen base
    gh = []
    for (x, h, wr) in P["gh"]:
        zb0 = zt(x) - 0.01
        wb = hw(x) - 0.085 - 0.03
        k = min(1.0, h / 0.3)
        zr = zb0 + h
        we = min(wb - 0.01, wr + 0.07 * k)
        gh.append((x, [(-wb, zb0), (-we, zr - 0.08 * k), (-wr, zr), (0.0, zr + 0.02 * k), (wr, zr), (we, zr - 0.08 * k), (wb, zb0)]))

    def gh_mat(s, i):
        # strips: 0 rear window, 1 C-pillar, 2 side glass / roof, 3 windscreen
        if i == 6:
            return m["trim"]
        side = i in (0, 5)
        if s == 0:
            return m["paint"] if side else (m["glass"] if i in (2, 3) else m["paint"])
        if s == 1:
            return m["paint"]
        if s == 2:
            return m["glass"] if side else m["paint"]
        return m["glass"] if (side or i in (2, 3)) else m["paint"]
    loft(b, gh, gh_mat, cap_mats=(m["paint"], m["paint"]))

    # B-pillar and the lower window line: dark strips on the side glass
    xb = (P["gh"][2][0] + P["gh"][3][0]) / 2 + 0.1
    for sgn in (-1, 1):
        # the side glass plane between waist and roof edge at xb
        wb = hw(xb) - 0.085 - 0.03
        zb0 = zt(xb) - 0.01
        h = interp([(g[0], g[1]) for g in P["gh"]], xb)
        wr = interp([(g[0], g[2]) for g in P["gh"]], xb)
        we = min(wb - 0.01, wr + 0.07)
        za, zbt = zb0 + 0.02, zb0 + h - 0.1
        ya = sgn * (wb + 0.004 - (wb - we) * 0.02 / max(h - 0.08, 0.1))
        yb = sgn * (we + 0.004 + (wb - we) * 0.02 / max(h - 0.08, 0.1))
        pts = [(xb - 0.05, ya, za), (xb + 0.05, ya, za), (xb + 0.05, yb, zbt), (xb - 0.05, yb, zbt)]
        poly(b, m["trim"], pts if sgn < 0 else pts[::-1])

    # ---- lamps, grille, bumpers on the end caps
    zf = zt(X1)
    zbf = zbot(X1)
    wf = hw(X1)
    zl0, zl1 = zf - 0.13, zf - 0.03
    for sgn in (-1, 1):
        ya, yb = sgn * (wf - 0.25), sgn * (wf - 0.07)
        end_quad(b, m["trim"], X1 + 0.003, min(ya, yb), max(ya, yb), zl0 - 0.015, zl1 + 0.01)   # lamp housing
        ya, yb = sgn * (wf - 0.23), sgn * (wf - 0.09)
        end_quad(b, m["lamp_f"], X1 + 0.006, min(ya, yb), max(ya, yb), zl0, zl1 - 0.01)
    end_quad(b, m["trim"], X1 + 0.004, -(wf - 0.30), wf - 0.30, zl0 - 0.02, zl1 - 0.01)          # grille
    end_quad(b, m["trim"], X1 + 0.004, -(wf - 0.18), wf - 0.18, zbf + 0.03, zbf + 0.13)          # lower intake
    zr_ = zt(X0)
    zbr = zbot(X0)
    wr_ = hw(X0)
    for sgn in (-1, 1):
        ya, yb = sgn * (wr_ - 0.3), sgn * (wr_ - 0.06)
        end_quad(b, m["lamp_r"], X0 - 0.004, min(ya, yb), max(ya, yb), zr_ - 0.22, zr_ - 0.03)
    end_quad(b, m["trim"], X0 - 0.004, -(wr_ - 0.1), wr_ - 0.1, zbr + 0.02, zbr + 0.14)       # rear bumper
    # side repeaters / light wrap: a sliver of each lamp round the corner
    for sgn in (-1, 1):
        x = X1 - 0.07
        y = sgn * (interp(P["halfw"], x) - 0.07 + 0.012)
        side_quad(b, m["lamp_f"], y, x - 0.1, x, zt(x) - 0.12, zt(x) - 0.05)
        x = X0 + 0.07
        y = sgn * (interp(P["halfw"], x) - 0.07 + 0.012)
        side_quad(b, m["lamp_r"], y, x, x + 0.1, zt(x) - 0.17, zt(x) - 0.06)

    # ---- mirrors on the doors just behind the screen base
    xm = P["gh"][4][0] - 0.2
    zw = zt(xm)
    wbm = hw(xm) - 0.12
    for sgn in (-1, 1):
        y0, y1 = sorted((sgn * wbm, sgn * (P["w"] + 0.04)))
        b.box(m["paint"], (xm - 0.1, y0, zw + 0.03), (xm + 0.02, y1, zw + 0.13))

    # ---- wheels
    for x in P["axles"]:
        for sgn in (-1, 1):
            yo = sgn * (hw(x) - 0.03)
            wheel(b, m, x, yo, sgn, r, P["tw"])

    if P["roof_rails"]:
        x0r, x1r = P["gh"][1][0] + 0.05, P["gh"][3][0] - 0.1
        zr = min(zt(g[0]) - 0.01 + g[1] for g in P["gh"][1:4]) - 0.01
        for sgn in (-1, 1):
            y = sgn * 0.56
            b.box(m["trim"], (x0r, y - 0.025, zr + 0.05), (x1r, y + 0.025, zr + 0.09))
            for x in (x0r + 0.05, x1r - 0.1):
                b.box(m["trim"], (x, y - 0.03, zr), (x + 0.06, y + 0.03, zr + 0.06))
    return finish(b)


# ------------------------------------------------------------- camper van
def van_section(w, zb, zs, zr, tuck=0.18, shoulder=0.14):
    """8-point van section: tuck under, flat side to zs, rounded shoulder to
    a flat roof at zr; clockwise from the left bottom."""
    sh = min(shoulder, max(0.02, zr - zs))
    return [(-(w - 0.04), zb), (-w, zb + tuck), (-w, zs), (-(w - sh), zr),
            ((w - sh), zr), (w, zs), (w, zb + tuck), ((w - 0.04), zb)]


def build_camper():
    key = "camper_van"
    apex.reset_scene()
    own = _own_mats(key, extra=[("vehicle_glass", "vehicle", "car_c.glb")])
    paint = own.get("vehicle_paint_camper") or own["vehicle_paint_a"]
    paint.name = "vehicle_paint_camper"
    m = dict(paint=paint, tyre=own["vehicle_tyre"], dark=own["bridge_dark"],
             win=own["pit_glass"], awning=own["tent_white"], plank=own["scaffold_plank"],
             glass=own["vehicle_glass"], **new_vehicle_slots())
    b = apex.Builder(key)
    X0, X1, W, r = -3.0, 3.0, 1.02, 0.36
    ra = r + 0.07
    axles = (-1.72, 1.95)
    arches = [(x, x, r, ra) for x in axles]
    # x, w, zs (top of the flat side), zr (roof / bonnet top)
    prof = [(-3.0, 0.97, 2.30, 2.58), (-2.94, 1.01, 2.36, 2.68), (-2.8, W, 2.38, 2.72),
            (1.05, W, 2.38, 2.72), (1.32, W, 2.20, 2.50), (1.52, W, 1.98, 2.10),
            (2.30, W, 1.10, 1.20), (2.72, 1.0, 1.02, 1.12), (2.92, 0.97, 0.94, 1.02), (3.0, 0.93, 0.80, 0.86)]

    def at(x, i):
        return interp([(p[0], p[i]) for p in prof], x)

    def zbot(x):
        base = interp([(X0, 0.42), (X0 + 0.2, 0.36), (X0 + 0.7, 0.40), (X1 - 0.7, 0.40), (X1 - 0.15, 0.34), (X1, 0.40)], x)
        return arch_z(x, arches, base)

    xs = merge_xs([p[0] for p in prof] + arch_stations(arches) + [0.0, -0.9])
    secs = [(x, van_section(at(x, 1), zbot(x), at(x, 2), at(x, 3))) for x in xs]
    i_ws = [k for k, x in enumerate(xs) if 1.52 <= x < 2.30]

    def body_mat(s, i):
        if i == 7:
            return m["trim"]
        if s in i_ws and i in (2, 3, 4):
            return m["glass"]          # the windscreen (shoulders and top of the screen strip)
        return m["paint"]
    loft(b, secs, body_mat, cap_mats=(m["paint"], m["paint"]))

    # cab side windows (sloped to the screen), living windows, sliding door
    for sgn in (-1, 1):
        y = sgn * (W + 0.006)
        side_quad(b, m["glass"], y, 0.95, 2.22, 1.18, 1.92, x0t=0.95, x1t=1.55)
        side_quad(b, m["trim"], y, 0.9, 0.95, 0.75, 1.95)                    # B-pillar line
        side_quad(b, m["win"], y, -2.55, -1.2, 1.45, 2.05)                    # rear living window
        side_strip(b, m["trim"], y, -2.9, 2.9, 0.62, 0.74, arches)             # rubbing strip
        if sgn > 0:
            side_quad(b, m["win"], y, -0.75, 0.55, 1.42, 2.02)                # sliding-door window
            side_quad(b, m["trim"], y + 0.002, -0.85, -0.81, 0.78, 2.15)      # door shut lines
            side_quad(b, m["trim"], y + 0.002, 0.81, 0.85, 0.78, 2.15)
        else:
            side_quad(b, m["win"], y, -0.75, 0.2, 1.45, 2.0)
        # high-roof side window strip (the bed window)
        side_quad(b, m["win"], sgn * (W + 0.006), -2.4, 0.6, 2.2, 2.32)
    # rear: two doors, windows, tail light columns, bumper, ladder
    for sgn in (-1, 1):
        end_quad(b, m["win"], X0 - 0.006, min(sgn * 0.12, sgn * 0.78), max(sgn * 0.12, sgn * 0.78), 1.45, 2.05)
        end_quad(b, m["lamp_r"], X0 - 0.006, min(sgn * 0.82, sgn * 0.93), max(sgn * 0.82, sgn * 0.93), 0.75, 1.4)
    end_quad(b, m["trim"], X0 - 0.006, -0.02, 0.02, 0.5, 2.25)
    b.box(m["trim"], (X0 - 0.08, -1.0, 0.36), (X0 + 0.05, 1.0, 0.58))
    for y in (-0.62, -0.32):
        b.box(m["dark"], (X0 - 0.12, y - 0.02, 0.9), (X0 - 0.08, y + 0.02, 2.62))
    for z in (1.1, 1.4, 1.7, 2.0, 2.3):
        b.box(m["dark"], (X0 - 0.12, -0.62, z), (X0 - 0.09, -0.32, z + 0.03))
    # front: grille, headlamps, bumper
    zf = 0.86
    end_quad(b, m["trim"], X1 + 0.006, -0.55, 0.55, 0.52, 0.80)
    for sgn in (-1, 1):
        end_quad(b, m["trim"], X1 + 0.004, min(sgn * 0.57, sgn * 0.9), max(sgn * 0.57, sgn * 0.9), 0.57, 0.83)
        end_quad(b, m["lamp_f"], X1 + 0.007, min(sgn * 0.6, sgn * 0.87), max(sgn * 0.6, sgn * 0.87), 0.6, 0.8)
    b.box(m["trim"], (X1 - 0.05, -0.98, 0.33), (X1 + 0.06, 0.98, 0.52))
    for sgn in (-1, 1):   # indicators at the bumper corners
        side_quad(b, m["lamp_f"], sgn * 0.985, 2.75, 2.92, 0.82, 0.92)
    # mirrors on arms
    for sgn in (-1, 1):
        b.bar(m["trim"], (2.15, sgn * W, 1.25), (2.2, sgn * 1.07, 1.3), 0.02, segs=4)
        y0, y1 = sorted((sgn * 1.05, sgn * 1.12))
        b.box(m["trim"], (2.12, y0, 1.22), (2.24, y1, 1.58))
    # roof: rack rails, slats, plank deck, vent; awning cassette on +Y
    zr = 2.72
    for y in (-0.8, 0.8):
        b.box(m["dark"], (-2.75, y - 0.03, zr), (0.95, y + 0.03, zr + 0.12))
    for x in (-2.6, -1.7, -0.8, 0.1, 0.8):
        b.box(m["dark"], (x - 0.03, -0.86, zr + 0.12), (x + 0.03, 0.86, zr + 0.17))
    b.box(m["plank"], (-2.6, -0.82, zr + 0.17), (-0.9, 0.82, zr + 0.21))
    b.box(m["trim"], (0.0, -0.3, zr), (0.55, 0.3, zr + 0.12))                     # roof vent
    b.box(m["dark"], (-0.6, 0.85, zr + 0.21), (-0.55, 0.9, zr + 0.33))            # aerial stub
    b.box(m["awning"], (-2.6, W, 2.22), (1.0, 1.12, 2.36))
    for x in (-2.55, 0.95):
        b.box(m["dark"], (x - 0.03, W, 2.2), (x + 0.03, 1.12, 2.38))
    # wheels (steel rims), a dark sill between them
    for x in axles:
        for sgn in (-1, 1):
            wheel(b, m, x, sgn * (W - 0.04), sgn, r, 0.23, rim=0.55)
    return finish(b, {"pit_glass": 0.25})


# ------------------------------------------------------------- motorhome
def build_motorhome():
    key = "motorhome"
    apex.reset_scene()
    own = _own_mats(key, extra=[("vehicle_glass", "vehicle", "car_c.glb")])
    m = dict(body=own["truck_body"], win=own["pit_glass"], dark=own["bridge_dark"], tyre=own["vehicle_tyre"],
             canvas=own["pit_wall"], steel=own["bridge_steel"], glass=own["vehicle_glass"], **new_vehicle_slots())
    b = apex.Builder(key)
    X0, X1, W, r = -5.0, 5.0, 1.27, 0.48
    ra = r + 0.08
    arches = [(3.25, 3.25, r, ra), (-3.95, -2.75, r, ra)]
    prof = [(-5.0, 1.18, 2.9, 3.05), (-4.94, 1.25, 3.0, 3.2), (-4.82, W, 3.05, 3.28),
            (4.55, W, 3.05, 3.28), (4.8, 1.25, 2.98, 3.2), (4.94, 1.2, 2.88, 3.05), (5.0, 1.12, 2.75, 2.9)]

    def at(x, i):
        return interp([(p[0], p[i]) for p in prof], x)

    def zbot(x):
        base = interp([(X0, 0.55), (X0 + 0.25, 0.45), (X1 - 0.25, 0.45), (X1, 0.5)], x)
        return arch_z(x, arches, base)

    xs = merge_xs([p[0] for p in prof] + arch_stations(arches) + [0.0])
    secs = [(x, van_section(at(x, 1), zbot(x), at(x, 2), at(x, 3), tuck=0.12, shoulder=0.22)) for x in xs]
    loft(b, secs, lambda s, i: m["dark"] if i == 7 else m["body"], cap_mats=(m["body"], m["body"]))

    # front: the big screen, a dark band over it, grille, lamps in housings,
    # bumper
    xf = X1 + 0.006
    end_quad(b, m["glass"], xf, -1.05, 1.05, 1.3, 2.55)
    end_quad(b, m["dark"], xf, -1.05, 1.05, 2.55, 2.72)
    end_quad(b, m["dark"], xf, -0.7, 0.7, 0.75, 1.08)
    for sgn in (-1, 1):
        end_quad(b, m["trim"], xf - 0.002, min(sgn * 0.75, sgn * 1.1), max(sgn * 0.75, sgn * 1.1), 0.74, 1.04)
        end_quad(b, m["lamp_f"], xf + 0.002, min(sgn * 0.78, sgn * 1.07), max(sgn * 0.78, sgn * 1.07), 0.78, 1.0)
    b.box(m["trim"], (X1 - 0.08, -1.2, 0.42), (X1 + 0.05, 1.2, 0.68))
    # rear: window, lamp columns, bumper, ladder
    xr = X0 - 0.006
    end_quad(b, m["win"], xr, -0.9, 0.9, 1.8, 2.5)
    for sgn in (-1, 1):
        end_quad(b, m["lamp_r"], xr, min(sgn * 0.95, sgn * 1.12), max(sgn * 0.95, sgn * 1.12), 0.85, 1.6)
    b.box(m["trim"], (X0 - 0.05, -1.2, 0.45), (X0 + 0.05, 1.2, 0.7))
    for y in (0.55, 0.9):
        b.box(m["steel"], (X0 - 0.1, y - 0.02, 1.0), (X0 - 0.06, y + 0.02, 3.35))
        b.box(m["steel"], (X0 - 0.1, y - 0.02, 3.31), (X0 + 0.25, y + 0.02, 3.35))
    for z in (1.3, 1.65, 2.0, 2.35, 2.7, 3.05):
        b.box(m["steel"], (X0 - 0.1, 0.55, z), (X0 - 0.07, 0.9, z + 0.03))
    # sides: cab glass, living windows (pit_glass: they glow at night),
    # skirt band, luggage bays, the door on the awning side
    for sgn in (-1, 1):
        y = sgn * (W + 0.006)
        side_quad(b, m["glass"], y, 3.75, 4.85, 1.3, 2.55, x1t=4.7)             # cab side glass
        side_strip(b, m["dark"], y, -4.75, 4.85, 0.45, 0.78, arches)            # skirt band
        for x0, x1 in ((-4.4, -3.3), (-2.6, -1.0), (1.2, 2.5)):
            side_quad(b, m["win"], y, x0, x1, 1.65, 2.45)
        if sgn > 0:
            side_quad(b, m["win"], y, -0.6, 0.8, 1.65, 2.45)
        for x0 in (-2.2, -0.6, 1.0):
            side_quad(b, m["dark"], y + sgn * 0.002, x0, x0 + 1.2, 0.85, 1.4)   # bay doors
            side_quad(b, m["body"], y + sgn * 0.004, x0 + 0.04, x0 + 1.16, 0.89, 1.36)
        side_quad(b, m["dark"], y, -4.75, 4.85, 2.72, 2.78)                     # a roof-line stripe
    y = -(W + 0.006)
    side_quad(b, m["dark"], y, 2.8, 3.55, 0.55, 2.62)                            # entrance door
    side_quad(b, m["win"], y - 0.002, 2.9, 3.45, 1.9, 2.5)
    b.box(m["steel"], (2.8, -W - 0.35, 0.3), (3.55, -W, 0.36))                   # step
    # rabbit-ear mirrors
    for sgn in (-1, 1):
        b.bar(m["trim"], (4.85, sgn * 1.18, 2.8), (5.15, sgn * 1.3, 2.62), 0.03, segs=4)
        y0, y1 = sorted((sgn * 1.24, sgn * 1.36))
        b.box(m["trim"], (5.1, y0, 2.05), (5.22, y1, 2.62))
    # roof: AC units, skylights, a vent
    zr = 3.28
    for x in (-3.0, 1.5):
        b.box(m["body"], (x - 0.4, -0.45, zr), (x + 0.4, 0.45, zr + 0.2))
        b.box(m["dark"], (x - 0.35, -0.4, zr + 0.2), (x + 0.35, 0.4, zr + 0.22))
    for x in (-1.2, 3.3):
        b.box(m["dark"], (x - 0.35, -0.35, zr), (x + 0.35, 0.35, zr + 0.1))
    b.box(m["trim"], (0.0, 0.3, zr), (0.4, 0.7, zr + 0.12))
    # the awning: cassette under the roof line, canvas sloping out to a
    # front bar on two legs at y -3.25, a scalloped valance
    xa0, xa1 = -3.0, 2.0
    b.box(m["body"], (xa0 - 0.1, -W - 0.14, 2.8), (xa1 + 0.1, -W, 2.98))
    yo, zo, zi = -3.22, 2.45, 2.86
    n, per = 20, 4                    # columns, columns a scallop
    sag = lambda t: 0.05 * math.sin(math.pi * t)
    for k in range(n):
        t0, t1 = k / n, (k + 1) / n
        xa, xb = xa0 + (xa1 - xa0) * t0, xa0 + (xa1 - xa0) * t1
        za, zb_ = zo - sag(t0), zo - sag(t1)
        poly(b, m["canvas"], [(xa, -W - 0.12, zi), (xb, -W - 0.12, zi), (xb, yo, zb_), (xa, yo, za)][::-1])
        da = 0.12 + 0.1 * math.sin(math.pi * ((k % per) / per))
        db = 0.12 + 0.1 * math.sin(math.pi * (((k % per) + 1) / per))
        poly(b, m["canvas"], [(xa, yo, za), (xb, yo, zb_), (xb, yo, zb_ - db), (xa, yo, za - da)][::-1])
    b.bar(m["steel"], (xa0, yo + 0.03, zo - 0.02), (xa1, yo + 0.03, zo - 0.02), 0.035, segs=6)
    for x in (xa0 + 0.05, xa1 - 0.05):
        b.bar(m["steel"], (x, yo + 0.03, 0.0), (x, yo + 0.03, zo - 0.02), 0.03, segs=6)
        b.bar(m["steel"], (x, -W - 0.05, 1.2), (x, yo + 0.05, zo - 0.12), 0.02, segs=4)
    # wheels: one front, a twin rear axle
    for x in (3.25, -2.75, -3.95):
        for sgn in (-1, 1):
            wheel(b, m, x, sgn * (W - 0.04), sgn, r, 0.3, rim=0.58)
    return finish(b, {"pit_glass": 0.25})


# ------------------------------------------------------------------ tent_6m
def build_tent():
    key = "tent_6m"
    apex.reset_scene()
    own = _own_mats(key)
    m = dict(steel=own["bridge_steel"], white=own["tent_white"], colour=own["tent_colour"], plank=own["scaffold_plank"])
    b = apex.Builder(key)
    E = 3.25            # the canvas edge (half side)
    P = 3.12            # the legs
    zE, zP = 2.4, 4.98  # eave, peak
    sag = 0.07

    def roof(x, y):
        """Pagoda roof height over (x, y): concave rise to the peak, the
        canvas sagging between the hips."""
        cheb = max(abs(x), abs(y))
        v = 1.0 - cheb / E                     # 0 at the eave, 1 at the peak
        u = (min(abs(x), abs(y)) / max(cheb, 1e-6))   # 0 mid-panel, 1 on a hip
        z = zE + (zP - zE) * (0.28 * v + 0.72 * v ** 2.2)
        return z - sag * (1.0 - u * u) * math.sin(math.pi * min(1.0, v / 0.92))

    nu = 10
    vs = [0.0, 0.16, 0.34, 0.52, 0.70, 0.84, 0.93]
    corners = [(-E, -E), (E, -E), (E, E), (-E, E)]
    for c in range(4):
        (ax, ay), (bx, by) = corners[c], corners[(c + 1) % 4]
        grid = []
        for j, v in enumerate(vs):
            row = []
            for i in range(nu + 1):
                t = i / nu
                ex, ey = ax + (bx - ax) * t, ay + (by - ay) * t
                x, y = ex * (1 - v), ey * (1 - v)
                row.append((x, y, roof(x, y)))
            grid.append(row)
        for j in range(len(vs) - 1):
            for i in range(nu):
                q = [grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]]
                b.sheet(m["white"], *q)
    # top ring closed by the finial collar
    v = vs[-1]
    h = E * (1 - v)
    zc = roof(h, 0.0)
    b.cone(m["colour"], (0, 0, zc - 0.05), h * 1.42, 0.06, zP + 0.05 - zc + 0.05, segs=8, cap=True)
    b.bar(m["steel"], (0, 0, zP), (0, 0, 5.2), 0.025, segs=5)
    b.ico(m["colour"], (0, 0, 5.18), 0.05, subdiv=1)

    # scalloped valance hanging from the eave on all four sides
    sc = 5          # scallops a side
    seg = 4
    drop_hi, drop_lo = 0.18, 0.34
    for c in range(4):
        (ax, ay), (bx, by) = corners[c], corners[(c + 1) % 4]
        n = sc * seg
        for i in range(n):
            t0, t1 = i / n, (i + 1) / n

            def pt(t, z):
                return (ax + (bx - ax) * t, ay + (by - ay) * t, z)

            def lo(t):
                ph = (t * sc) % 1.0
                return zE - drop_hi - (drop_lo - drop_hi) * math.sin(math.pi * ph)
            b.sheet(m["colour"], pt(t0, zE + 0.01), pt(t1, zE + 0.01), pt(t1, lo(t1)), pt(t0, lo(t0)))
    # eave frame and legs, foot plates
    for c in range(4):
        (ax, ay), (bx, by) = corners[c], corners[(c + 1) % 4]
        k = P / E
        b.bar(m["steel"], (ax * k, ay * k, zE - 0.06), (bx * k, by * k, zE - 0.06), 0.03, segs=6)
        b.bar(m["steel"], (ax * k, ay * k, 0.0), (ax * k, ay * k, zE - 0.03), 0.035, segs=6)
        b.box(m["steel"], (ax * k - 0.1, ay * k - 0.1, 0.0), (ax * k + 0.1, ay * k + 0.1, 0.02))
    # walls: back (+Y) and left (-X) hang in shallow pleats
    def wall(p0, p1, normal):
        n = 12
        for i in range(n):
            t0, t1 = i / n, (i + 1) / n
            d0 = 0.035 * (1 if i % 2 == 0 else -1)
            d1 = -d0
            a = (p0[0] + (p1[0] - p0[0]) * t0 + normal[0] * d0, p0[1] + (p1[1] - p0[1]) * t0 + normal[1] * d0)
            c_ = (p0[0] + (p1[0] - p0[0]) * t1 + normal[0] * d1, p0[1] + (p1[1] - p0[1]) * t1 + normal[1] * d1)
            b.sheet(m["white"], (a[0], a[1], 0.03), (c_[0], c_[1], 0.03), (c_[0], c_[1], zE - 0.02), (a[0], a[1], zE - 0.02))
    wall((-P, P - 0.02), (P, P - 0.02), (0, 1))
    wall((-P + 0.02, -P), (-P + 0.02, P), (-1, 0))
    # the counter: a plank top on a skirt, set back from the open front
    b.box(m["plank"], (-1.6, -0.35, 0.0), (1.6, 0.3, 0.86))
    b.box(m["plank"], (-1.7, -0.45, 0.86), (1.7, 0.35, 0.92))
    b.box(m["colour"], (-1.6, -0.36, 0.25), (1.6, -0.35, 0.8))
    return finish(b)


BUILDS = {"car_a": lambda: build_car("car_a"), "car_b": lambda: build_car("car_b"),
          "car_c": lambda: build_car("car_c"), "camper_van": build_camper,
          "motorhome": build_motorhome, "tent_6m": build_tent}
# (front 3/4, rear 3/4) views and a far row: (count, pitch along Y, camera)
VIEWS = {
    "car_a": [((5.5, -6, 2.2), (0.1, 0, 0.6)), ((-5.5, 6, 2.6), (0, 0, 0.6))],
    "car_b": [((6.5, -6.5, 2.2), (0.1, 0, 0.6)), ((-6.5, 6.5, 2.6), (0, 0, 0.6))],
    "car_c": [((6.5, -6.5, 2.5), (0.1, 0, 0.8)), ((-6.5, 6.5, 2.8), (0, 0, 0.8))],
    "camper_van": [((8, -8, 3), (0.2, 0, 1.4)), ((-8, 8, 3.5), (0, 0, 1.4))],
    "motorhome": [((12, -12, 4), (0, -0.8, 1.6)), ((-12, 10, 5), (0, -0.8, 1.6))],
    "tent_6m": [((9, -10, 4), (0, 0, 2.3)), ((-10, 9, 5), (0, 0, 2.3))],
}
ROW = {"car_a": (8, 2.6), "car_b": (8, 2.6), "car_c": (8, 2.7), "camper_van": (6, 3.4),
       "motorhome": (4, 6.5), "tent_6m": (4, 8.0)}


def far_row(key, ob):
    """Copies in a row across Y (a car park bay row), seen from ~60 m."""
    n, pitch = ROW[key]
    cps = []
    for i in range(1, n):
        cp = ob.copy()
        cp.location = (0, i * pitch, 0)
        bpy.context.scene.collection.objects.link(cp)
        cps.append(cp)
    mid = (n - 1) * pitch / 2
    d = 45 + n * pitch * 1.2
    apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", key + "_far.png"),
                 look_from=(d * 0.75, mid - d * 0.6, d * 0.18), look_at=(0, mid, 0.8), fov=22)
    for cp in cps:
        bpy.data.objects.remove(cp, do_unlink=True)


if __name__ == "__main__":
    out = {}
    for key, fn in BUILDS.items():
        if ASSET not in ("all", key):
            continue
        ob = fn()
        kind = KIND.get(key, "vehicle")
        out[key] = apex.stats()[key]
        apex.export_one(kind, key)
        apex.save_blend(kind, key)
        apex.preview_ground()
        for i, (lf, la) in enumerate(VIEWS[key]):
            apex.preview(os.path.join(apex.PROPS_ROOT, "_preview", key + ("" if i == 0 else "_b") + ".png"),
                         look_from=lf, look_at=la)
        far_row(key, ob)
    for k, v in out.items():
        print("STATS", k, v["tris"], v["bbox_min"], v["bbox_max"], v["mats"])
