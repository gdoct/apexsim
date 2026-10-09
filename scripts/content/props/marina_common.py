r"""Shared helpers for the Marina Bay ("Mandarina Bay") prop batches.

Loaded by build_marina_bay_*.py:
    exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())

Gives: apex, tex, B, PR, np, the material factory `mb_mats()`, facade
textures with a night-window emissive map, shell/torus/dome geometry helpers,
a scene reset and a preview+export driver.
"""
import bpy, bmesh, math, os, sys, importlib.util, random
import numpy as np
from mathutils import Vector

_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")
_PD = os.path.join(_ROOT, "scripts", "content", "props")
def _load(name, fn):
    s = importlib.util.spec_from_file_location(name, os.path.join(_PD, fn))
    m = importlib.util.module_from_spec(s); sys.modules[name] = m; s.loader.exec_module(m); return m
apex = _load("apex", "apex_props.py")
tex = _load("apex_tex", "apex_tex.py")
B, PR = apex.Builder, apex.PROPS_ROOT
PREVIEW_DIR = os.path.join(_ROOT, ".cache", "previews", "mb")
os.makedirs(PREVIEW_DIR, exist_ok=True)
NIGHT = False          # preview switch: emissive strength 0/1


# ------------------------------------------------------------ facade textures
def facade_material(name, glass, frame, cols=8, rows=4, tile_m=16.0, lit=0.35, seed=1,
                    mullion=0.07, sill=0.30, tint=0.10, metallic=0.25, size=1024):
    """Curtain-wall tile: cols x rows window cells over tile_m metres (UV = metres).
    Colour + roughness + a warm 'lit window' emissive map (strength is driven
    at runtime on pit_glass-style slots; 1.0 here, 0 by day in previews)."""
    rng = np.random.default_rng(seed)
    col = np.zeros((size, size, 3), np.float32)
    rough = np.full((size, size), 0.5, np.float32)
    emis = np.zeros((size, size, 3), np.float32)
    col[:] = frame
    cw, ch = size // cols, size // rows
    glass = np.array(glass, np.float32); frame = np.array(frame, np.float32)
    for r in range(rows):
        for c in range(cols):
            x0, y0 = c * cw, r * ch
            mx = int(cw * mullion); sy0 = int(ch * sill); sy1 = int(ch * 0.04)
            ya, yb = y0 + sy1, y0 + ch - sy0   # image rows: y grows downward in the array
            xa, xb = x0 + mx, x0 + cw - mx
            n = 1.0 + rng.uniform(-tint, tint)
            g = np.linspace(1.18, 0.82, yb - ya)[:, None, None] * glass[None, None, :] * n
            if rng.random() < 0.12:                      # blinds drawn
                g = g * 0.4 + np.array([0.55, 0.52, 0.46], np.float32) * 0.6
            col[ya:yb, xa:xb] = np.clip(g, 0, 1)
            rough[ya:yb, xa:xb] = 0.10
            if rng.random() < lit:
                e = np.array([1.0, 0.82, 0.5], np.float32) * rng.uniform(0.5, 1.0)
                emis[ya:yb, xa:xb] = e
    # spandrel shading + bright mullion edges
    col += (rng.random((size, size, 1)).astype(np.float32) - 0.5) * 0.02
    img_col = tex.to_image(name + "_col", np.clip(col, 0, 1))
    img_r = tex.to_image(name + "_rough", np.stack([rough] * 3, -1), noncolor=True)
    img_e = tex.to_image(name + "_emis", emis)
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL'):
            nt.nodes.remove(n)
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = metallic
    co = nt.nodes.new("ShaderNodeTexCoord"); mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.0 / tile_m, 1.0 / tile_m, 1.0)
    nt.links.new(co.outputs["UV"], mp.inputs["Vector"])
    def tnode(img):
        t = nt.nodes.new("ShaderNodeTexImage"); t.image = img
        nt.links.new(mp.outputs["Vector"], t.inputs["Vector"]); return t
    nt.links.new(tnode(img_col).outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tnode(img_r).outputs["Color"], bsdf.inputs["Roughness"])
    nt.links.new(tnode(img_e).outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = 1.0
    return m


def set_night(on):
    global NIGHT
    NIGHT = on
    for m in bpy.data.materials:
        if m.use_nodes:
            b = m.node_tree.nodes.get("Principled BSDF")
            if not b:
                continue
            em = b.inputs["Emission Color"]
            if em.is_linked or m.name.startswith("ferris_lights") or m.name.startswith("mb_lit"):
                b.inputs["Emission Strength"].default_value = 1.0 if on else 0.0


def soft(gen, k=0.45):
    """Calm a generator's colour map: pull it towards its mean by factor k."""
    c, r, h, ns = gen
    mean = c.mean(axis=(0, 1), keepdims=True)
    return np.clip(mean + (c - mean) * k, 0, 1), r, h, ns


_MATS = {}
def mb_mats():
    """Shared slot set for all Marina Bay assets (cached per Blender session)."""
    if _MATS and all(v.name in bpy.data.materials for v in _MATS.values()):
        return _MATS
    M = tex.kit_material
    P = tex.pbr_material
    d = {}
    d["glass_blue"] = facade_material("mb_glass_blue", (0.16, 0.30, 0.42), (0.20, 0.22, 0.25), seed=1)
    d["glass_teal"] = facade_material("mb_glass_teal", (0.12, 0.34, 0.34), (0.18, 0.22, 0.22), seed=2)
    d["glass_bronze"] = facade_material("mb_glass_bronze", (0.38, 0.30, 0.20), (0.22, 0.20, 0.18), seed=3)
    d["glass_grey"] = facade_material("mb_glass_grey", (0.22, 0.27, 0.31), (0.24, 0.25, 0.27), seed=4, cols=6, rows=3, tile_m=18.0)
    d["glass_clear"] = facade_material("mb_glass_clear", (0.30, 0.42, 0.50), (0.55, 0.57, 0.6), seed=5, cols=4, rows=2, tile_m=12.0, mullion=0.04, sill=0.2, lit=0.25)
    d["classic"] = facade_material("mb_classic", (0.10, 0.14, 0.18), (0.90, 0.86, 0.76), cols=8, rows=3, tile_m=16.0, mullion=0.30, sill=0.34, lit=0.2, seed=6, metallic=0.1)
    d["colonial"] = facade_material("mb_colonial", (0.20, 0.30, 0.22), (0.94, 0.94, 0.92), cols=8, rows=4, tile_m=16.0, mullion=0.26, sill=0.30, lit=0.15, seed=7, metallic=0.05)
    d["deco"] = facade_material("mb_deco", (0.12, 0.14, 0.18), (0.82, 0.80, 0.74), cols=8, rows=4, tile_m=16.0, mullion=0.16, sill=0.22, lit=0.3, seed=8, metallic=0.1)
    d["stone"] = P("mb_stone", soft(tex.plaster(seed=101, rgb=(0.88, 0.86, 0.80), stains=0.12), 0.4), tile_m=3.0)
    d["white"] = P("mb_white", tex.cladding(seed=102, rgb=(0.9, 0.91, 0.92), panel=(1.5, 0.75)), tile_m=1.0)
    d["plaster"] = P("mb_plaster", soft(tex.plaster(seed=112, rgb=(0.93, 0.93, 0.90), stains=0.1), 0.4), tile_m=3.0)
    d["bronze"] = apex.material("mb_bronze", (0.42, 0.28, 0.12), metallic=0.85, roughness=0.35)
    d["concrete"] = P("mb_concrete", soft(tex.concrete(seed=103, rgb=(0.66, 0.65, 0.62)), 0.5), tile_m=3.0)
    d["concrete_dark"] = P("mb_concrete_dark", soft(tex.concrete(seed=104, rgb=(0.40, 0.40, 0.41), stains=0.35), 0.5), tile_m=3.0)
    d["steel"] = P("mb_steel", tex.painted_steel(seed=105, rgb=(0.80, 0.81, 0.83), chips=0.02), tile_m=1.0, metallic=0.7)
    d["steel_dark"] = P("mb_steel_dark", tex.painted_steel(seed=106, rgb=(0.14, 0.15, 0.17), chips=0.02), tile_m=1.0, metallic=0.7)
    d["alu"] = P("mb_alu", tex.galvanised(seed=107, rgb=(0.80, 0.82, 0.84), spangle=False), tile_m=1.5, metallic=0.9)
    d["roof"] = P("mb_roof", tex.concrete(seed=108, rgb=(0.35, 0.36, 0.37), stains=0.3, form_lines=False), tile_m=2.0)
    d["red"] = P("mb_red", tex.painted_steel(seed=109, rgb=(0.62, 0.07, 0.06), chips=0.02), tile_m=1.0, metallic=0.4)
    d["copper"] = P("mb_copper", tex.corten(seed=110, rgb=(0.30, 0.50, 0.42)), tile_m=2.0, metallic=0.3)
    d["tile"] = P("mb_roof_tile", tex.roof_tiles(seed=111, rgb=(0.30, 0.31, 0.32)), tile_m=1.0)
    d["door"] = M("pit_door", (0.7, 0.71, 0.72))
    d["pit_wall"] = M("pit_wall", (0.82, 0.82, 0.8))
    d["pit_floor"] = M("pit_floor", (0.55, 0.55, 0.54))
    d["glass_pit"] = tex.kit_material("pit_glass", (0.05, 0.08, 0.1), metallic=0.2, roughness=0.15)
    d["water"] = apex.material("mb_water", (0.10, 0.28, 0.36), metallic=0.1, roughness=0.08)
    d["lights_warm"] = apex.material("ferris_lights", (1.0, 0.8, 0.45), roughness=0.4, emission=(1.0, 0.75, 0.35))
    d["lights_rim"] = apex.material("ferris_lights_rim", (0.4, 0.6, 1.0), roughness=0.4, emission=(0.35, 0.55, 1.0))
    d["lights_hub"] = apex.material("ferris_lights_hub", (1.0, 0.9, 0.7), roughness=0.4, emission=(1.0, 0.9, 0.6))
    d["glass_cap"] = apex.material("mb_capsule_glass", (0.20, 0.35, 0.45), metallic=0.1, roughness=0.08)
    d["lit"] = apex.material("mb_lit_panel", (1.0, 0.9, 0.7), roughness=0.5, emission=(1.0, 0.85, 0.55))
    _MATS.clear(); _MATS.update(d)
    return _MATS


# ------------------------------------------------------------- geometry helpers
def _orient(f, out):
    """Flip face f so its normal points along `out`."""
    f.normal_update()
    if f.normal.dot(out) < 0:
        f.normal_flip()


def tri(b, mat, p0, p1, p2, out=None):
    s = b.slot(mat)
    try:
        f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in (p0, p1, p2)])
    except ValueError:
        return None
    f.material_index = s
    if out is not None:
        _orient(f, Vector(out))
    return f


def quad(b, mat, p0, p1, p2, p3, out=None):
    s = b.slot(mat)
    try:
        f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in (p0, p1, p2, p3)])
    except ValueError:
        return None
    f.material_index = s
    if out is not None:
        _orient(f, Vector(out))
    return f


def shell(b, mat, P, ref, closed_j=True, inward=False):
    """Skin a grid of points P[i][j] with quads, each oriented away from `ref`
    (a point inside the shape) so no recalc is needed."""
    ref = Vector(ref)
    n_i, n_j = len(P), len(P[0])
    rng = n_j if closed_j else n_j - 1
    for i in range(n_i - 1):
        for j in range(rng):
            k = (j + 1) % n_j
            pts = [Vector(P[i][j]), Vector(P[i][k]), Vector(P[i + 1][k]), Vector(P[i + 1][j])]
            c = sum(pts, Vector()) / 4
            out = (c - ref) * (-1 if inward else 1)
            if (pts[0] - pts[2]).length < 1e-6 or (pts[1] - pts[3]).length < 1e-6:
                # collapsed (pole) -> triangle
                uniq = []
                for p in pts:
                    if not any((p - q).length < 1e-6 for q in uniq):
                        uniq.append(p)
                if len(uniq) == 3:
                    tri(b, mat, *uniq, out=out)
                continue
            quad(b, mat, *pts, out=out)


def ellipsoid_pts(c, rx, ry, rz, nseg, nring, th0=0.0, th1=math.pi / 2):
    P = []
    for i in range(nring + 1):
        th = th0 + (th1 - th0) * i / nring
        ring = []
        for j in range(nseg):
            a = 2 * math.pi * j / nseg
            ring.append((c[0] + rx * math.cos(th) * math.cos(a), c[1] + ry * math.cos(th) * math.sin(a), c[2] + rz * math.sin(th)))
        P.append(ring)
    return P


def torus_y(b, mat, c, R, r, segs=48, rings=8):
    """Torus lying in the XZ plane (axis Y) centred at c."""
    cx, cy, cz = c
    P = []
    for i in range(segs):
        a = 2 * math.pi * i / segs
        row = []
        for j in range(rings):
            t = 2 * math.pi * j / rings
            rr = R + r * math.cos(t)
            row.append((cx + rr * math.cos(a), cy + r * math.sin(t), cz + rr * math.sin(a)))
        P.append(row)
    s = b.slot(mat)
    for i in range(segs):
        for j in range(rings):
            ni, nj = (i + 1) % segs, (j + 1) % rings
            ac = Vector((cx + R * math.cos(2 * math.pi * (i + 0.5) / segs), cy, cz + R * math.sin(2 * math.pi * (i + 0.5) / segs)))
            pts = [Vector(P[i][j]), Vector(P[ni][j]), Vector(P[ni][nj]), Vector(P[i][nj])]
            quad(b, mat, *pts, out=(sum(pts, Vector()) / 4 - ac))


def tube(b, mat, p0, p1, r, segs=8, cap=True):
    """Round bar between two arbitrary points (alias of Builder.bar)."""
    b.bar(mat, p0, p1, r, segs=segs)


def prism_ngon(b, mat, pts_xy, z0, z1):
    """Vertical prism from a convex polygon (counter-clockwise from above)."""
    n = len(pts_xy)
    c = Vector((sum(p[0] for p in pts_xy) / n, sum(p[1] for p in pts_xy) / n, (z0 + z1) / 2))
    for i in range(n):
        j = (i + 1) % n
        a, bb = pts_xy[i], pts_xy[j]
        quad(b, mat, (a[0], a[1], z0), (bb[0], bb[1], z0), (bb[0], bb[1], z1), (a[0], a[1], z1),
             out=Vector(((a[0] + bb[0]) / 2, (a[1] + bb[1]) / 2, 0)) - Vector((c.x, c.y, 0)))
    top = [b.bm.verts.new((p[0], p[1], z1)) for p in pts_xy]
    bot = [b.bm.verts.new((p[0], p[1], z0)) for p in pts_xy]
    s = b.slot(mat)
    for ring, nz in ((top, 1), (bot, -1)):
        f = b.bm.faces.new(ring); f.material_index = s; _orient(f, Vector((0, 0, nz)))


def ellipse_pts(cx, cy, rx, ry, n=48):
    return [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)]


# ------------------------------------------------------------- scene/preview/export
def clear_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for me in list(bpy.data.meshes):
        if me.users == 0:
            bpy.data.meshes.remove(me)


def ground(size=300, name="PreviewGround"):
    g = B(name)
    g.box(apex.material("preview_ground", (0.28, 0.33, 0.2), roughness=0.95), (-size, -size, -0.1), (size, size, 0))
    return g.finish()


def shoot(tag, asset, shots, res=(1280, 720), night=False):
    """shots: list of (label, look_from, look_at, fov). Returns written paths."""
    set_night(night)
    sc = bpy.context.scene
    out = []
    for label, lf, la, fov in shots:
        p = os.path.join(PREVIEW_DIR, f"{asset}_{label}{'_night' if night else ''}.png")
        apex.preview(p, look_from=lf, look_at=la, res=res, fov=fov)
        out.append(p)
    set_night(True)  # exported state: emissive on
    return out


def export_tree(kind, root_name):
    """Export root + children (e.g. a `rotor` child) as <kind>/<root>.glb, root moved to origin."""
    root = bpy.data.objects[root_name]
    objs = [root] + list(root.children_recursive)
    loc = root.location.copy(); root.location = (0, 0, 0)
    d = os.path.join(PR, kind); os.makedirs(d, exist_ok=True)
    glb = os.path.join(d, root_name + ".glb")
    for o in bpy.context.scene.objects:
        o.select_set(o in objs)
    bpy.context.view_layer.objects.active = root
    set_night(True)
    with apex._ui_override(root):
        bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', use_selection=True, export_apply=True,
                                  export_yup=True, export_texcoords=True, export_normals=True,
                                  export_materials='EXPORT', export_cameras=False, export_lights=False)
    root.location = loc
    return glb, os.path.getsize(glb)


def tri_count(root_name):
    root = bpy.data.objects[root_name]
    dg = bpy.context.evaluated_depsgraph_get()
    t = 0
    for o in [root] + list(root.children_recursive):
        if o.type == 'MESH':
            me = o.evaluated_get(dg).data
            t += sum(len(p.vertices) - 2 for p in me.polygons)
    return t


def bounds(root_name):
    root = bpy.data.objects[root_name]
    pts = []
    for o in [root] + list(root.children_recursive):
        if o.type == 'MESH':
            pts += [o.matrix_world @ v.co for v in o.data.vertices]
    return [round(min(p[i] for p in pts), 2) for i in range(3)], [round(max(p[i] for p in pts), 2) for i in range(3)]


def frustum(b, mat, lo, hi, z0, z1):
    """Rectangular frustum / hip roof: lo=(x0,x1,y0,y1) at z0, hi=(x0,x1,y0,y1) at z1."""
    (a0, a1, c0, c1), (d0, d1, e0, e1) = lo, hi
    bot = [(a0, c0, z0), (a1, c0, z0), (a1, c1, z0), (a0, c1, z0)]
    top = [(d0, e0, z1), (d1, e0, z1), (d1, e1, z1), (d0, e1, z1)]
    ctr = Vector(((a0 + a1) / 2, (c0 + c1) / 2, (z0 + z1) / 2))
    for i in range(4):
        j = (i + 1) % 4
        pts = [bot[i], bot[j], top[j], top[i]]
        cen = sum((Vector(p) for p in pts), Vector()) / 4
        if (Vector(top[j]) - Vector(top[i])).length < 1e-6:
            tri(b, mat, bot[i], bot[j], top[i], out=cen - ctr)
        else:
            quad(b, mat, *pts, out=cen - ctr)
    quad(b, mat, *top, out=(0, 0, 1))
    quad(b, mat, *bot, out=(0, 0, -1))


def gable(b, mat, x0, x1, y0, y1, z0, rise, ridge='y'):
    """Triangular-prism gable. ridge='y': triangles in the XZ plane (facing +-Y)."""
    if ridge == 'y':
        A, Bv, C = (x0, y0, z0), (x1, y0, z0), ((x0 + x1) / 2, y0, z0 + rise)
        D, E, F = (x0, y1, z0), (x1, y1, z0), ((x0 + x1) / 2, y1, z0 + rise)
    else:
        A, Bv, C = (x0, y0, z0), (x0, y1, z0), (x0, (y0 + y1) / 2, z0 + rise)
        D, E, F = (x1, y0, z0), (x1, y1, z0), (x1, (y0 + y1) / 2, z0 + rise)
    ref = sum((Vector(p) for p in (A, Bv, C, D, E, F)), Vector()) / 6
    cen = lambda *ps: sum((Vector(p) for p in ps), Vector()) / len(ps) - ref
    tri(b, mat, A, Bv, C, out=cen(A, Bv, C)); tri(b, mat, D, E, F, out=cen(D, E, F))
    quad(b, mat, A, C, F, D, out=cen(A, C, F, D)); quad(b, mat, Bv, E, F, C, out=cen(Bv, E, F, C))
    quad(b, mat, A, Bv, E, D, out=(0, 0, -1))


def colonnade(b, mat, cap_mat, xs, y, z0, z1, r=0.5):
    for x in xs:
        b.cylinder(mat, (x, y, z0), r, z1 - z0, segs=14)
        b.box(cap_mat, (x - r * 1.35, y - r * 1.35, z1 - 0.35), (x + r * 1.35, y + r * 1.35, z1))
        b.box(cap_mat, (x - r * 1.2, y - r * 1.2, z0), (x + r * 1.2, y + r * 1.2, z0 + 0.3))
