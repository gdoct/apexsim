"""Build the shared wheel models, one per car class: content/wheels/<class>.glb.

Run inside Blender:

    exec(open(r"D:\\apexsim\\content\\wheels\\build_wheels.py").read())

Each wheel is built in a scratch scene and exported without touching the
open file. Frame: metres, hub centre at the origin, axle along X, the face
(spokes, centre nut) on +X. The whole wheel sits inside x in [-W/2, W/2] and
a radius of R, so the client can size it to any car from the mesh bounds:
width from X, diameter from Y/Z (car.toml `[wheels]`, docs/CAR_MODELS.md).
The client puts the face outboard on both sides of the car.

Material slots: `wheel_tyre`, `wheel_mark` (sidewall lettering, which is
also what shows the wheel turning), `wheel_band` (compound ring),
`wheel_rim`, `wheel_nut`, `wheel_brake`.
"""
import bpy, bmesh, contextlib, io, math, os
from mathutils import Matrix, Vector

OUT_DIR = os.path.join(os.path.dirname(r"D:\apexsim\content\wheels\build_wheels.py"))

CLASSES = {
    # R: tyre radius, W: tyre width, RR: rim flange radius (18" rims).
    "f1":   dict(R=0.360, W=0.380, RR=0.235, spokes=12, spoke_w=0.016, twin=False,
                 rim=(0.05, 0.05, 0.055), rim_metal=0.8, nut=(0.85, 0.05, 0.05),
                 band=(0.95, 0.80, 0.05), marks=(0.92, 0.92, 0.92)),
    "gt3":  dict(R=0.350, W=0.310, RR=0.235, spokes=5, spoke_w=0.028, twin=True,
                 rim=(0.62, 0.63, 0.65), rim_metal=1.0, nut=(0.85, 0.10, 0.05),
                 band=None, marks=(0.85, 0.85, 0.85)),
    "lmp2": dict(R=0.355, W=0.330, RR=0.235, spokes=10, spoke_w=0.020, twin=False,
                 rim=(0.03, 0.03, 0.035), rim_metal=0.6, nut=(0.95, 0.75, 0.05),
                 band=(0.95, 0.95, 0.95), marks=(0.92, 0.92, 0.92)),
    # LMH: one 18" size all round, a fine multi-spoke forged wheel in satin
    # graphite with a polished lip, which is most of what reads as "hypercar".
    "hypercar": dict(R=0.360, W=0.340, RR=0.238, spokes=14, spoke_w=0.013, twin=False,
                     rim=(0.10, 0.10, 0.11), rim_metal=0.9, nut=(0.80, 0.80, 0.82),
                     band=(0.95, 0.80, 0.05), marks=(0.92, 0.92, 0.92)),
}

SEGS = 64


def material(name, color, metallic=0.0, roughness=0.5):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    return m


def lathe(bm, profile, slot, segs=SEGS, a0=0.0, a1=2 * math.pi):
    """Revolve a list of (x, r) about X. A closed sweep (full turn) wraps around."""
    full = abs(a1 - a0 - 2 * math.pi) < 1e-6
    steps = segs if full else max(2, segs)
    rings = []
    for i in range(steps if full else steps + 1):
        a = a0 + (a1 - a0) * i / steps
        c, s = math.cos(a), math.sin(a)
        rings.append([bm.verts.new((x, r * c, r * s)) for (x, r) in profile])
    n = len(profile)
    for i in range(len(rings) if full else len(rings) - 1):
        A, B = rings[i], rings[(i + 1) % len(rings)]
        for j in range(n - 1):
            f = bm.faces.new((A[j], A[j + 1], B[j + 1], B[j]))
            f.material_index = slot
    return rings


def loop_lathe(bm, loop, slot, segs=SEGS):
    """Revolve a closed (x, r) loop: a solid ring."""
    return lathe(bm, list(loop) + [loop[0]], slot, segs)


def box(bm, slot, centre, size, rot=Matrix.Identity(3)):
    hx, hy, hz = (s / 2 for s in size)
    corners = [Vector((sx * hx, sy * hy, sz * hz)) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    v = [bm.verts.new(Vector(centre) + rot @ c) for c in corners]
    for q in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        f = bm.faces.new([v[i] for i in q])
        f.material_index = slot


def build(name, P):
    R, W, RR = P["R"], P["W"], P["RR"]
    h = W / 2
    mats = [
        material("wheel_tyre", (0.025, 0.025, 0.025), 0.0, 0.85),
        material("wheel_mark", P["marks"], 0.0, 0.6),
        material("wheel_band", P["band"] or (0.5, 0.5, 0.5), 0.0, 0.6),
        material("wheel_rim", P["rim"], P["rim_metal"], 0.3),
        material("wheel_nut", P["nut"], 0.6, 0.35),
        material("wheel_brake", (0.30, 0.28, 0.26), 1.0, 0.55),
    ]
    TYRE, MARK, BAND, RIM, NUT, BRAKE = range(6)
    bm = bmesh.new()

    # Tyre: bead at the rim, flat sidewalls, rounded shoulders, tread.
    sh = 0.035
    tyre = [(-h + 0.02, RR - 0.01), (-h, RR + 0.02), (-h, R - sh)]
    for k in range(1, 6):
        a = math.pi / 2 * k / 6
        tyre.append((-h + sh * (1 - math.sin(a)), R - sh * (1 - math.cos(a)) - 0.0))
    tyre += [(-h + sh, R), (h - sh, R)]
    for k in range(5, 0, -1):
        a = math.pi / 2 * k / 6
        tyre.append((h - sh * (1 - math.sin(a)), R - sh * (1 - math.cos(a))))
    tyre += [(h, R - sh), (h, RR + 0.02), (h - 0.02, RR - 0.01)]
    loop_lathe(bm, tyre, TYRE)

    # Sidewall graphics, a hair proud of both sidewalls: a full compound
    # band, and two lettering arcs, which are what shows the wheel turning.
    eps = 0.0015
    for side in (-1, 1):
        x = side * (h + eps)
        if P["band"]:
            lathe(bm, [(x, R - sh - 0.012), (x, R - sh - 0.004)][::side], BAND)
        for a in (0.0, math.pi):
            lathe(bm, [(x, RR + 0.035), (x, RR + 0.065)][::side], MARK, segs=12, a0=a + 0.2, a1=a + 0.2 + 0.9)

    # Rim barrel with a flange lip on the face.
    rim = [(-h + 0.02, RR - 0.012), (h - 0.012, RR - 0.012), (h - 0.004, RR + 0.004),
           (h - 0.018, RR + 0.004), (h - 0.03, RR - 0.03), (-h + 0.02, RR - 0.03)]
    loop_lathe(bm, rim, RIM)

    # Hub: dished centre, then spokes out to the barrel.
    hub_x = h - 0.055
    loop_lathe(bm, [(hub_x - 0.03, 0.02), (hub_x + 0.02, 0.02), (hub_x + 0.02, 0.075), (hub_x - 0.03, 0.075)], RIM, segs=24)
    n = P["spokes"]
    offsets = (-0.16, 0.16) if P["twin"] else (0.0,)
    for k in range(n):
        base = 2 * math.pi * k / n
        for off in offsets:
            a = base + off
            p0 = Vector((hub_x + 0.005, 0.065 * math.cos(a), 0.065 * math.sin(a)))
            a1 = base + off * 0.45
            p1 = Vector((h - 0.03, (RR - 0.025) * math.cos(a1), (RR - 0.025) * math.sin(a1)))
            d = p1 - p0
            fwd = d.normalized()
            side = Vector((0, -math.sin(a), math.cos(a)))
            up = fwd.cross(side).normalized()
            side = up.cross(fwd).normalized()
            rot = Matrix((fwd, side, up)).transposed()
            box(bm, RIM, (p0 + p1) / 2, (d.length, P["spoke_w"], 0.018), rot)
    # Centre-lock nut: hexagon, inside the tyre's width.
    loop_lathe(bm, [(hub_x + 0.02, 0.0), (h - 0.012, 0.0), (h - 0.012, 0.028), (hub_x + 0.02, 0.04)], NUT, segs=6)

    # Brake disc and bell, inboard, inside the barrel.
    loop_lathe(bm, [(-h + 0.05, 0.10), (-h + 0.078, 0.10), (-h + 0.078, 0.165), (-h + 0.05, 0.165)], BRAKE, segs=48)
    loop_lathe(bm, [(-h + 0.078, 0.06), (hub_x - 0.03, 0.05), (hub_x - 0.03, 0.07), (-h + 0.078, 0.105)], RIM, segs=24)

    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new("wheel_" + name)
    bm.to_mesh(me)
    bm.free()
    for m in mats:
        me.materials.append(m)
    for p in me.polygons:
        p.use_smooth = True
    return me, mats


def export_all(classes=None):
    win = bpy.context.window
    orig = win.scene
    made = []
    for name in classes or CLASSES:
        held = [m for m in bpy.data.materials if m.name.startswith("wheel_")]
        for m in held:
            m.name = "__held_" + m.name
        sc = bpy.data.scenes.new("build_wheels_tmp")
        win.scene = sc
        me, mats = build(name, CLASSES[name])
        ob = bpy.data.objects.new("wheel_" + name, me)
        sc.collection.objects.link(ob)
        path = os.path.join(OUT_DIR, name + ".glb")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                bpy.ops.export_scene.gltf(filepath=path, use_active_scene=True, export_format='GLB')
            made.append((path, [round(x, 3) for x in ob.dimensions]))
        finally:
            win.scene = orig
            bpy.data.scenes.remove(sc)
            bpy.data.objects.remove(ob)
            bpy.data.meshes.remove(me)
            for m in mats:
                bpy.data.materials.remove(m)
            for m in held:
                m.name = m.name[len("__held_"):]
    return made


if __name__ == "__main__" or True:
    # set WHEEL_CLASSES = ["hypercar"] before exec() to rebuild only some
    print(export_all(globals().get("WHEEL_CLASSES")))
