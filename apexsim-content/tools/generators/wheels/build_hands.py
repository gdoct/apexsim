"""Build the driver's gloves and sleeves the cockpit rig draws on the wheel.

    python tools/generators/wheels/build_hands.py              # every class, and the sleeves
    python tools/generators/wheels/build_hands.py gt3 f1       # a few classes
    python tools/generators/wheels/build_hands.py --textures   # derive the maps again first
    python tools/generators/wheels/build_hands.py --preview gt3   # and render out/hands/*.png

Run with plain Python: it derives the CC0 scans' maps into
`wheels/hands/_textures/` when they are missing (the scans fetched by
`tools/cc0.py`), then runs itself in Blender headless (`$BLENDER`, else
Blender 5.1's default install) to model and export. Seeded, so a run writes
the same meshes.

Per wheel class, `wheels/steering/<class>_hands.glb`: both gloves closed round
that wheel's grips at 9 and 3 o'clock, in the wheel GLB's own frame, so the rig
hangs it on the wheel and it turns with the rim. Each hand is modelled as a
hand is built: a palm over four metacarpals with the thenar and hypothenar
pads, four fingers of three phalanges each curled round the grip's own
section joint by joint (a phalanx that would run into a spoke, a paddle or
the faceplate bends less and rests on it), the thumb over the spoke. Blended
as metaballs per finger, so the knuckles swell and the fingers stay apart
with a crease between them. Over that a race glove: a raised knuckle panel,
external seams round each finger, a gauntlet flaring toward the elbow with a
wrist strap and its tab. Some 9-10k triangles a glove. The gauntlet's open
end is the wrist the rig's arms start from, written into the class's
`<class>.json` as `wrist_cm` (the right hand, wheel pivot frame, cm; the left
mirrors it).

Class-free, `wheels/hands/forearm.glb` and `wheels/hands/upper_arm.glb`: a
race-suit sleeve each, modelled at its real length (forearm 27 cm, upper arm
30 cm, the rig's `ApexCockpit::ForearmCm` / `UpperArmCm`) and stretched to
1 m along glTF +X (the rig stretches it back joint to joint), hand end at 0.
Folds bunch toward the elbow on the inside of the bend (+Z, as the rig hangs
the elbow below wrist and shoulder), the cloth sags a little below, a seam
runs under the arm; the elbow ends are domed so the joint is closed. A ribbed
knit band at the forearm's hand end sits inside the gauntlet.

Materials (slot names): `glove_leather` (the back of the hand, the fingers),
`glove_palm` (suede, inside the grip), `glove_cuff` (the knuckle panel, the
strap and the gauntlet's edge), `suit` (the sleeves; the rig tints it with the
car's paint, so its map is near white) and `suit_cuff` (the knit band).
Leather from ambientCG Leather026, suede Leather039, fire-suit cloth
Fabric082A (`textures/cc0/sources.json`): base colour at a measured albedo,
roughness and normal maps, which the car parents draw (docs/cars.md).
Brand-free.
"""
from __future__ import annotations

import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("APEXSIM_CONTENT") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))
STEER_DIR = os.path.join(ROOT, "wheels", "steering")
HANDS_DIR = os.path.join(ROOT, "wheels", "hands")
TEX_DIR = os.path.join(HANDS_DIR, "_textures")
BLENDER = os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe")

# Each class wheel's right grip as build_steering_wheels.py lays it: its
# centre (u right, v up, w toward the driver, cm) at 3 o'clock, the grip
# section's half widths across the rim (u) and along the column (w), and the
# rim's centreline as (v, u) points, so a finger above or below 3 o'clock
# closes round the rim where it is.
_ROUND = lambda r: [(v, math.sqrt(r * r - v * v)) for v in range(-8, 9)]  # noqa: E731
GRIPS = {
    "f1": ((12.9, 0.0, -0.55), 1.96, 2.40, [(-7.2, 11.0), (-4.0, 12.4), (0.0, 12.9), (3.6, 12.6), (6.0, 11.6)]),
    "lmp2": ((13.3, 0.0, -0.5), 1.98, 2.42, [(-7.2, 11.8), (-4.5, 13.0), (0.0, 13.3), (4.2, 13.0), (6.6, 12.0)]),
    "hypercar": ((13.7, 0.0, -0.5), 1.98, 2.42, [(-7.4, 12.2), (-4.5, 13.4), (0.0, 13.7), (4.2, 13.4), (7.0, 12.4)]),
    "gt3": ((16.4, 0.0, -0.6), 1.80, 2.37, _ROUND(16.4)),
    "rally": ((17.5, 0.0, 4.2), 1.45, 1.75, _ROUND(17.5)),
}

# A gloved hand (cm). Fingers: index, middle, ring, little: the height on the
# grip (v), where the knuckle sits round the grip (degrees, the grip's own
# section angle: 0 outward, -90 toward the nose), the three phalanges' lengths,
# the half thickness at the knuckle and at the tip, and the metacarpal's.
FINGERS = [
    (2.95, -10.0, (4.5, 2.6, 2.1), 1.02, 0.86, 6.6),
    (0.95, -15.0, (4.9, 3.0, 2.25), 1.05, 0.88, 6.4),
    (-1.05, -11.0, (4.6, 2.85, 2.15), 1.0, 0.85, 5.9),
    (-2.95, 2.0, (3.7, 2.15, 1.95), 0.9, 0.77, 5.3),
]
THUMB = (3.05, 2.45)           # the thumb's two phalanges
CUFF_CM = 8.5                  # the gauntlet from the wrist toward the elbow
FOREARM_CM, UPPER_ARM_CM = 27.0, 30.0

TEXTURES = {
    # name: (scan, linear albedo, roughness range)
    "leather": ("Leather026", 0.032, (0.5, 0.7)),
    "suede": ("Leather039", 0.06, (0.86, 0.98)),
    "panel": ("Leather026", 0.085, (0.42, 0.62)),
    "suit": ("Fabric082A", 0.80, (0.74, 0.92)),
}


# ---------------------------------------------------------------------------
# Plain Python: the maps, then Blender
# ---------------------------------------------------------------------------

def make_textures():
    """Each scan's base colour, roughness (glTF packing: G roughness, B
    metallic 0) and normal map, 512 px, seamless as the scans are; and the
    knit band's rib normal."""
    import numpy as np
    from PIL import Image
    sys.path.insert(0, os.path.join(ROOT, "tools", "generators", "cars"))
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import cc0
    import textures
    os.makedirs(TEX_DIR, exist_ok=True)
    for name, (src, albedo, (r0, r1)) in TEXTURES.items():
        textures.scan_grain(src, albedo).save(os.path.join(TEX_DIR, f"{name}_col.png"), optimize=True)
        textures.scan_normal(src).save(os.path.join(TEX_DIR, f"{name}_nrm.png"), optimize=True)
        rough = sorted(cc0.fetch(src).glob("*_Roughness.*"))[0]
        g = np.asarray(Image.open(rough).convert("L").resize((512, 512), Image.LANCZOS), dtype=np.float64)
        lo, hi = np.percentile(g, 2), np.percentile(g, 98)
        g = r0 + (r1 - r0) * np.clip((g - lo) / max(hi - lo, 1.0), 0.0, 1.0)
        g8 = np.round(g * 255).astype(np.uint8)
        rgb = np.stack([np.full_like(g8, 255), g8, np.zeros_like(g8)], axis=2)
        Image.fromarray(rgb, "RGB").save(os.path.join(TEX_DIR, f"{name}_orm.png"), optimize=True)
    # 1x1 rib, 16 ribs across the map: a ridge's slope as the normal's x.
    x = np.linspace(0.0, 16 * 2 * math.pi, 256, endpoint=False)
    nx = -0.55 * np.cos(x)
    n = np.stack([nx, np.zeros_like(nx), np.ones_like(nx)], axis=1)
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    row = np.round((n * 0.5 + 0.5) * 255).astype(np.uint8)
    Image.fromarray(np.repeat(row[None, :, :], 64, axis=0), "RGB").save(
        os.path.join(TEX_DIR, "rib_nrm.png"), optimize=True)
    print(f"textures: {TEX_DIR}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for n in args:
        if n not in GRIPS:
            sys.exit(f"unknown class {n}; one of {', '.join(GRIPS)}")
    need = [f"{t}_{m}" for t in TEXTURES for m in ("col", "nrm", "orm")] + ["rib_nrm"]
    if "--textures" in sys.argv or any(not os.path.exists(os.path.join(TEX_DIR, f"{n}.png")) for n in need):
        make_textures()
    cmd = [BLENDER, "-b", "--factory-startup", "--python", os.path.abspath(__file__), "--"] + sys.argv[1:]
    env = dict(os.environ, APEXSIM_CONTENT=ROOT)
    sys.exit(subprocess.call(cmd, env=env))


try:
    import bpy  # noqa: F401
except ImportError:
    bpy = None

if bpy is None and __name__ == "__main__":
    main()


# ---------------------------------------------------------------------------
# Blender
# ---------------------------------------------------------------------------

import numpy as np  # noqa: E402

if bpy is not None:
    import bmesh
    from mathutils import Matrix, Quaternion, Vector
    from mathutils.bvhtree import BVHTree

RNG = np.random.default_rng(53)


def B(p):
    """Driver's frame (cm: u right, v up, w toward the driver) -> Blender (m).
    glTF is X = -u, Y = v, Z = -w and Blender's exporter writes glTF
    (x, z, -y), so Blender is (-u, w, v): a rotation, no mirror."""
    return Vector((-p[0] * 0.01, p[2] * 0.01, p[1] * 0.01))


def D(b):
    """Blender (m) -> driver's frame (cm)."""
    return np.array([-b[0] * 100.0, b[2] * 100.0, b[1] * 100.0])


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def log(msg):
    print(msg, flush=True)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


# ------------------------------------------------------------- materials
def image(name, colour):
    path = os.path.join(TEX_DIR, f"{name}.png")
    img = bpy.data.images.load(path, check_existing=True)
    if not colour:
        img.colorspace_settings.name = "Non-Color"
    img.pack()
    return img


def material(name, tex, tint=(1.0, 1.0, 1.0), normal_strength=1.0, rough_tex=True, normal="nrm"):
    """A Principled material on `tex`'s maps: base colour x `tint`, the
    roughness map through Separate Color's G (the exporter packs it as
    glTF's metallicRoughnessTexture), the normal map at `normal_strength`."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    b.inputs["Metallic"].default_value = 0.0
    col = nt.nodes.new("ShaderNodeTexImage")
    col.image = image(f"{tex}_col", True)
    if tint != (1.0, 1.0, 1.0):
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = 'RGBA'
        mix.blend_type = 'MULTIPLY'
        rgba = [i for i in mix.inputs if i.type == 'RGBA']
        mix.inputs["Factor"].default_value = 1.0
        nt.links.new(col.outputs["Color"], rgba[0])
        rgba[1].default_value = (*tint, 1.0)
        nt.links.new(next(o for o in mix.outputs if o.type == 'RGBA'), b.inputs["Base Color"])
    else:
        nt.links.new(col.outputs["Color"], b.inputs["Base Color"])
    if rough_tex:
        orm = nt.nodes.new("ShaderNodeTexImage")
        orm.image = image(f"{tex}_orm", False)
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(orm.outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs["Green"], b.inputs["Roughness"])
        nt.links.new(sep.outputs["Blue"], b.inputs["Metallic"])
    nrm = nt.nodes.new("ShaderNodeTexImage")
    nrm.image = image(normal if normal != "nrm" else f"{tex}_nrm", False)
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nmap.inputs["Strength"].default_value = normal_strength
    nt.links.new(nrm.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], b.inputs["Normal"])
    return m


def materials():
    return {
        "glove_leather": material("glove_leather", "leather", normal_strength=0.45),
        "glove_palm": material("glove_palm", "suede", normal_strength=0.8),
        # The panels and strap: the same leather, a shade lighter, stiffer.
        "glove_cuff": material("glove_cuff", "panel", normal_strength=0.35),
        "suit": material("suit", "suit", normal_strength=0.9),
        "suit_cuff": material("suit_cuff", "suit", tint=(0.045, 0.045, 0.05), normal_strength=1.0,
                              normal="rib_nrm"),
    }


# ------------------------------------------------------------- metaballs
_K = {}


def surface_ratio(stiffness=2.0, threshold=0.6):
    """A lone ball's surface radius over its element radius, measured: the
    hand is laid out in surface sizes."""
    key = (stiffness, threshold)
    if key not in _K:
        mb = bpy.data.metaballs.new("Calib")
        mb.resolution = 0.02
        mb.render_resolution = 0.02
        mb.threshold = threshold
        e = mb.elements.new()
        e.radius = 1.0
        e.stiffness = stiffness
        ob = bpy.data.objects.new("Calib", mb)
        bpy.context.scene.collection.objects.link(ob)
        dg = bpy.context.evaluated_depsgraph_get()
        me = ob.evaluated_get(dg).to_mesh()
        _K[key] = max(v.co.length for v in me.vertices)
        ob.evaluated_get(dg).to_mesh_clear()
        bpy.data.objects.remove(ob)
        bpy.data.metaballs.remove(mb)
    return _K[key]


class Blob:
    """One metaball family (its own object name, so families do not blend):
    elements given in the driver's frame and surface sizes, cm. Built ten
    times over size and shrunk once a mesh: Blender will not polygonise
    finer than 5 mm, a glove wants under 1."""

    S = 10.0

    def __init__(self, name, resolution=0.0008):
        self.mb = bpy.data.metaballs.new(name)
        self.mb.resolution = resolution * self.S
        self.mb.render_resolution = resolution * self.S
        self.mb.threshold = 0.6
        self.ob = bpy.data.objects.new(name, self.mb)
        bpy.context.scene.collection.objects.link(self.ob)
        self.k = surface_ratio()

    def _el(self, kind, at, r, stiffness=2.0):
        e = self.mb.elements.new(type=kind)
        e.co = B(at) * self.S
        e.radius = r * 0.01 * self.S / self.k
        e.stiffness = stiffness
        return e

    def ball(self, at, r, stiffness=2.0):
        return self._el('BALL', at, r, stiffness)

    def capsule(self, a, b, r, stiffness=2.0):
        """A capsule whose surface runs from `a` to `b` (ends rounded at r)."""
        a, b = np.asarray(a, float), np.asarray(b, float)
        mid = (a + b) / 2
        e = self._el('CAPSULE', mid, r, stiffness)
        half = max(np.linalg.norm(b - a) / 2 - r * 0.15, 0.0)
        e.size_x = half * 0.01 * self.S
        e.rotation = (B(b) - B(a)).to_track_quat('X', 'Z')
        return e

    def ellipsoid(self, at, axes, sizes, stiffness=2.0):
        """`axes`: three driver-frame directions; `sizes`: surface half
        lengths along them."""
        r = min(sizes)
        e = self._el('ELLIPSOID', at, r, stiffness)
        e.size_x, e.size_y, e.size_z = (s / r for s in sizes)
        bx = B(np.asarray(axes[0]) * 100.0).normalized()
        by = B(np.asarray(axes[1]) * 100.0).normalized()
        bz = bx.cross(by).normalized()
        by = bz.cross(bx)
        e.rotation = Matrix((bx, by, bz)).transposed().to_quaternion()
        return e

    def mesh(self):
        """The family's surface as a mesh object (the metaball removed)."""
        log(f"  polygonising {self.ob.name}: {len(self.mb.elements)} elements")
        dg = bpy.context.evaluated_depsgraph_get()
        me = bpy.data.meshes.new_from_object(self.ob.evaluated_get(dg))
        me.transform(Matrix.Scale(1.0 / self.S, 4))
        ob = bpy.data.objects.new(self.ob.name + "_mesh", me)
        bpy.context.scene.collection.objects.link(ob)
        bpy.data.objects.remove(self.ob)
        return ob


# ------------------------------------------------------------- the grip
class Grip:
    """The grip's section (build_steering_wheels.grip_section: a rounded,
    slightly squared oval, x across the rim, y along the column) and the
    curve a finger's centreline follows round it at a given offset."""

    def __init__(self, name, side=1):
        (gu, gv, gw), self.rx, self.ry, rim = GRIPS[name]
        self.gw = gw
        rim = np.array(rim, dtype=np.float64)
        self.rim_v, self.rim_u = rim[:, 0], rim[:, 1]
        self.a = np.radians(np.linspace(-400.0, 160.0, 2241))

    def centre(self, v):
        return np.array([np.interp(v, self.rim_v, self.rim_u), v, self.gw])

    def section(self, a):
        c, s = np.cos(a), np.sin(a)
        return (self.rx * np.sign(c) * np.abs(c) ** 0.8, self.ry * np.sign(s) * np.abs(s) ** 0.8)

    def curve(self, v, offset):
        """(points, angles): the offset curve at height v, by angle."""
        x, y = self.section(self.a)
        dx, dy = np.gradient(x), np.gradient(y)
        L = np.hypot(dx, dy) + 1e-12
        nx, ny = dy / L, -dx / L            # outward for a counter-clockwise section
        c = self.centre(v)
        P = np.stack([c[0] + x + offset * nx, np.full_like(x, v), c[2] + y + offset * ny], axis=1)
        return P, self.a


# ------------------------------------------------------------- the wheel
def wheel_obstacles(name):
    """The class wheel's parts a hand must not pass through (not the grip it
    holds, not the rim it wraps): a BVH in the driver's frame, cm."""
    path = os.path.join(STEER_DIR, f"{name}.glb")
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    verts, polys = [], []
    for o in new:
        if o.type != 'MESH':
            continue
        mw = o.matrix_world
        base = len(verts)
        verts += [tuple(D(mw @ v.co)) for v in o.data.vertices]
        for p in o.data.polygons:
            mname = o.data.materials[p.material_index].name if o.data.materials else ""
            if mname.startswith(("sw_grip", "sw_marker")):
                continue
            # Behind the rim (the paddles) the fingers wrap; only what
            # stands in front of it is in the thumb's way.
            if np.mean([verts[base + i][2] for i in p.vertices]) < GRIPS[name][0][2] - 0.5:
                continue
            polys.append([base + i for i in p.vertices])
    return BVHTree.FromPolygons(verts, polys), new


def clear_of(bvh, a, b, r):
    """How far a capsule from a to b of radius r is into the obstacles (0 when clear)."""
    worst = 0.0
    for t in np.linspace(0.0, 1.0, 9):
        p = a + (b - a) * t
        hit = bvh.find_nearest(Vector(p), r)
        if hit[0] is not None:
            worst = max(worst, r - hit[3])
    return worst


# ------------------------------------------------------------- the hand
def rotate(v, axis, deg):
    q = Quaternion(Vector(axis), math.radians(deg))
    return np.array(q @ Vector(v))


def curl(grip, bvh, v, a0, lengths, r0, r1):
    """A finger's joints from its knuckle round the grip: each phalanx a
    chord of the curve its centreline follows (the grip's section offset by
    the finger's thickness); one that would run into the wheel bends less."""
    radii = np.linspace(r0, r1, len(lengths) + 1)
    P, A = grip.curve(v, offset=radii[0] + 0.06)
    i = int(np.argmin(np.abs(A - math.radians(a0))))
    joints = [P[i].copy()]
    prev_dir = None
    for k, L in enumerate(lengths):
        p = joints[-1]
        # The chord: the first point back round the curve L away.
        d = np.linalg.norm(P[:i] - p, axis=1)
        j = np.where(d >= L)[0]
        if len(j):
            jj = j[-1]
            want = unit(P[jj] - p)
        else:
            want = prev_dir
            jj = 0
        cands = [want] if prev_dir is None or bvh is None else [unit(want * (1 - t) + prev_dir * t) for t in np.linspace(0, 1, 11)]
        best, best_pen, bent = None, None, False
        for n_c, c in enumerate(cands):
            pen = 0.0 if bvh is None else clear_of(bvh, p, p + c * L, radii[k + 1] * 0.92)
            if best_pen is None or pen < best_pen - 1e-6:
                best, best_pen, bent = c, pen, n_c > 0
            if pen <= 0.0:
                best, bent = c, n_c > 0
                break
        q = p + best * L
        joints.append(q)
        prev_dir = best
        # Carry on round the curve from the point nearest where this one ended.
        i = int(np.argmin(np.linalg.norm(P[:i + 1] - q, axis=1))) if bent else jj
    return joints, radii


def build_hand(name, mats, bvh):
    """The right glove round the class's right grip, as one mesh object."""
    grip = Grip(name)
    ev = np.array([0.0, 1.0, 0.0])
    parts = []
    knuckles, bases, fingers = [], [], []

    # Fingers: each closed round the grip, three phalanges with the
    # knuckles between them a little fuller, the pads on the grip side.
    for idx, (v, a0, lengths, r0, r1, meta) in enumerate(FINGERS):
        joints, radii = curl(grip, None, v, a0, lengths, r0, r1)
        fingers.append((joints, radii))
        blob = Blob(f"Finger{idx}")
        for k in range(3):
            a, b = joints[k], joints[k + 1]
            if k == 0:
                a = a + unit(a - b) * 1.2           # into the palm
            ra, rb = radii[k], radii[k + 1]
            blob.capsule(a, b, (ra + rb) / 2 * 0.97)
            mid = (a + b) / 2
            inward = unit(grip.centre(v) - mid)
            inward = unit(inward - np.dot(inward, unit(b - a)) * unit(b - a))
            blob.ellipsoid(mid + inward * ra * 0.28, (unit(b - a), np.array([0.0, 1.0, 0.0]), inward),
                           (np.linalg.norm(b - a) * 0.36, ra * 0.86, ra * 0.62), stiffness=1.0)
        for k in (1,):
            blob.ball(joints[k] - unit(grip.centre(v) - joints[k]) * radii[k] * 0.1, radii[k] * 0.8, stiffness=0.9)
        parts.append(blob.mesh())
        knuckles.append(joints[0])

    # The palm's frame: knuckle axis along v, the metacarpals back along
    # the curve's tangent from the middle knuckle, the back of the hand out.
    j0 = fingers[1][0]
    P, A = grip.curve(FINGERS[1][0], offset=fingers[1][1][0] + 0.06)
    i = int(np.argmin(np.linalg.norm(P - j0[0], axis=1)))
    tangent = unit(P[min(i + 6, len(P) - 1)] - P[i])       # round toward the driver
    m = unit(tangent + np.array([0.12, -0.10, 0.0]))        # wrist a little out and down
    dorsal = unit(np.cross(m, ev))
    if np.dot(dorsal, j0[0] - grip.centre(j0[0][1])) < 0:
        dorsal = -dorsal
    across = unit(np.cross(dorsal, m))                      # toward the index side
    if across[1] < 0:
        across = -across

    # The palm: a slab wide at the knuckles and narrowing to the wrist,
    # the knuckle heads proud of it on the back, the heel's two pads.
    palm = Blob("Palm")
    kc = np.mean(knuckles, axis=0)
    span = np.dot(knuckles[0] - knuckles[3], across)
    for idx, kn in enumerate(knuckles):
        palm.ball(kn + dorsal * 0.38, FINGERS[idx][3] * 0.98, stiffness=1.4)
        bases.append(kn + m * FINGERS[idx][5])
    meta = FINGERS[1][5]
    palm.ellipsoid(kc + m * (meta * 0.32) + dorsal * 0.15, (m, across, dorsal),
                   (meta * 0.42, span / 2 + 0.95, 1.32))
    palm.ellipsoid(kc + m * (meta * 0.78) + dorsal * 0.05, (m, across, dorsal),
                   (meta * 0.38, span / 2 + 0.3, 1.38))
    wrist = kc + m * (meta + 1.4)
    thenar = kc + m * (meta * 0.7) + across * (span / 2 - 0.2) - dorsal * 1.0
    palm.ellipsoid(thenar, (unit(m - across * 0.5), across, dorsal), (2.6, 1.45, 1.15))
    hypo = kc + m * (meta * 0.72) - across * (span / 2 + 0.1) - dorsal * 0.8
    palm.ellipsoid(hypo, (m, across, dorsal), (2.6, 1.1, 1.0))
    palm.ellipsoid(wrist, (m, across, dorsal), (1.7, 2.75, 1.85))

    # The thumb: its metacarpal from the heel (in the palm's family, so the
    # thenar blends into it) to a knuckle on the driver's face of the grip,
    # then two phalanges inward, laid down onto the spoke or the faceplate
    # as far as they go without passing through it.
    vt = FINGERS[1][0] + 0.1
    gc = grip.centre(vt)
    t_mcp = gc + np.array([-(grip.rx + 1.0) * 0.15, 0.0, grip.ry + 0.55])
    cmc = t_mcp + unit(thenar - t_mcp) * 4.4
    palm.capsule(cmc, t_mcp, 1.15)
    palm.ellipsoid((cmc + t_mcp) / 2 - dorsal * 0.5, (unit(t_mcp - cmc), across, dorsal), (2.4, 1.3, 1.05))
    tj = [t_mcp]
    # Each phalanx as far toward the nose as is clear, the tip bending down
    # past the knuckle's line but never more than a joint can.
    lo = -1.0
    for L, r in zip(THUMB, (0.98, 0.86)):
        p = tj[-1]
        best = None
        for dw in np.linspace(lo, lo + 1.4, 29):
            c = unit(np.array([-1.0, -0.18, dw]))
            if clear_of(bvh, p, p + c * L, r * 0.95) <= 0.0:
                best, lo = c, dw - 0.6
                break
        best = best if best is not None else unit(np.array([-1.0, -0.18, lo + 1.4]))
        tj.append(p + best * L)
    thumb = Blob("Thumb")
    thumb.capsule(tj[0] - unit(tj[1] - tj[0]) * 1.0, tj[1], 0.98)
    thumb.capsule(tj[1], tj[2] - unit(tj[2] - tj[1]) * 0.3, 0.82)
    # The pad of the thumb, flattened where it lies on the wheel.
    tip_dir = unit(tj[2] - tj[1])
    down = unit(np.array([0.0, 0.0, -1.0]) - tip_dir * tip_dir[2])
    thumb.ellipsoid(tj[1] + (tj[2] - tj[1]) * 0.55 + down * 0.25, (tip_dir, np.cross(tip_dir, down), down),
                    (1.3, 0.9, 0.55), stiffness=1.0)
    parts.append(thumb.mesh())
    parts.append(palm.mesh())

    hand = join(parts, "hand_r")
    for p in hand.data.polygons:
        p.material_index = 0
    hand.data.materials.append(mats["glove_leather"])
    hand.data.materials.append(mats["glove_palm"])
    hand.data.materials.append(mats["glove_cuff"])

    log(f"  finishing: {len(hand.data.polygons):,} faces")
    finish_hand(hand, grip, fingers, knuckles, dorsal, m, wrist)
    log("  seams")
    seams(hand, fingers, grip, dorsal)
    log("  gauntlet")

    # The gauntlet toward the elbow, out, down and back to the driver.
    d = unit(np.array([0.32, -0.42, 1.0]) * 0.7 + m * 0.3)
    gauntlet = cuff(mats, wrist - d * 1.2, d, across, dorsal)
    end = wrist + d * CUFF_CM
    hand = join([hand, gauntlet], "hand_r")
    return hand, end - d * 2.0


def join(objs, name):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    return ob


def finish_hand(hand, grip, fingers, knuckles, dorsal, m, wrist):
    """Leather and suede by which way the surface faces, the knuckle panel
    raised over the knuckles, the mesh thinned to a glove's budget."""
    bm = bmesh.new()
    bm.from_mesh(hand.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.00005)
    bm.normal_update()
    k0, k3 = knuckles[0] + dorsal * 0.5, knuckles[3] + dorsal * 0.5
    seg = k3 - k0
    wl = bm.verts.layers.float.new("w")
    for vtx in bm.verts:
        p = D(vtx.co)
        n = D(vtx.normal) * 0.01
        # Distance to the knuckle line, on the back of the hand only.
        t = np.clip(np.dot(p - k0, seg) / np.dot(seg, seg), -0.08, 1.08)
        dist = np.linalg.norm(p - (k0 + seg * t))
        w = (1.0 - smoothstep(1.35, 1.9, dist)) * smoothstep(0.1, 0.45, np.dot(unit(n), dorsal))
        if w > 0:
            vtx.co += vtx.normal * 0.0026 * w
        vtx[wl] = w
    # Suede where the hand faces the grip it holds: inside the curl of the
    # fingers and the palm, not their backs. Weighed per vertex on the
    # smooth normal, then each face takes its neighbours' majority twice, so
    # the panel's edge runs clean instead of in steps.
    sl = bm.verts.layers.float.new("suede")
    for vtx in bm.verts:
        p = D(vtx.co)
        to_grip = unit(grip.centre(p[1]) - p)
        near = 1.0 - smoothstep(4.6, 5.6, np.linalg.norm(p - grip.centre(p[1])))
        vtx[sl] = smoothstep(0.25, 0.55, np.dot(unit(D(vtx.normal)), to_grip)) * near
    for f in bm.faces:
        if np.mean([v[wl] for v in f.verts]) <= 0.55 and np.mean([v[sl] for v in f.verts]) > 0.5:
            f.material_index = 1
    for _ in range(2):
        flips = []
        for f in bm.faces:
            around = {g.material_index for e in f.edges for g in e.link_faces if g is not f}
            votes = [g.material_index for e in f.edges for g in e.link_faces if g is not f]
            if len(around) == 1 and votes and votes[0] != f.material_index:
                flips.append((f, votes[0]))
        for f, mi in flips:
            f.material_index = mi
    bm.to_mesh(hand.data)
    bm.free()
    mod = hand.modifiers.new("thin", 'DECIMATE')
    mod.ratio = 1.0
    faces = len(hand.data.polygons)
    mod.ratio = min(1.0, 11000.0 / max(faces * 2, 1))
    apply(hand, mod)
    for p in hand.data.polygons:
        p.use_smooth = True
    box_uv(hand, tile_cm=7.0)


def apply(ob, mod):
    bpy.context.view_layer.objects.active = ob
    with bpy.context.temp_override(object=ob, active_object=ob):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def box_uv(ob, tile_cm):
    """Box projection in the driver's frame at `tile_cm` a texture repeat:
    the scans' grain at its real size, the same on every face."""
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    uv = bm.loops.layers.uv.verify()
    for f in bm.faces:
        n = np.abs(D(f.normal))
        ax = int(np.argmax(n))
        keep = [i for i in range(3) if i != ax]
        for lp in f.loops:
            p = D(lp.vert.co) / tile_cm
            lp[uv].uv = (p[keep[0]], p[keep[1]])
    bm.to_mesh(ob.data)
    bm.free()


def project_path(bvh, pts, lift):
    """Points snapped onto a surface and lifted off it along its normal."""
    out = []
    for p in pts:
        hit = bvh.find_nearest(Vector(p))
        if hit[0] is None:
            continue
        out.append(np.array(hit[0]) + np.array(hit[1]) * lift)
    return out


def tube(name, pts, r, mat, seg=6):
    """A thin round tube along `pts` (driver's frame, cm), its ends closed."""
    cu = bpy.data.curves.new(name, 'CURVE')
    cu.dimensions = '3D'
    cu.bevel_depth = r * 0.01
    cu.bevel_resolution = 1
    cu.use_fill_caps = True
    sp = cu.splines.new('POLY')
    sp.points.add(len(pts) - 1)
    for i, p in enumerate(pts):
        sp.points[i].co = (*B(p), 1.0)
    ob = bpy.data.objects.new(name, cu)
    bpy.context.scene.collection.objects.link(ob)
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    bpy.data.objects.remove(ob)
    mo = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(mo)
    me.materials.append(mat)
    for p in me.polygons:
        p.use_smooth = True
    return mo


def seams(hand, fingers, grip, dorsal):
    """External seams: a welt down each side of every finger and over its
    tip, as a race glove's are sewn outside."""
    me = hand.data
    verts = [tuple(D(v.co)) for v in me.vertices]
    bvh = BVHTree.FromPolygons(verts, [list(p.vertices) for p in me.polygons])
    mat = hand.data.materials[0]
    objs = []
    for joints, radii in fingers:
        joints = [np.asarray(j) for j in joints]
        tipdir = unit(joints[3] - joints[2])
        line = []
        for side in (1.0, -1.0):
            run = []
            for k in range(3):
                a, b = joints[k], joints[k + 1]
                for t in np.linspace(0.0, 1.0, 7)[:-1] if k < 2 else np.linspace(0.0, 1.0, 7):
                    p = a + (b - a) * t
                    r = radii[k] + (radii[k + 1] - radii[k]) * t
                    out = unit(p - grip.centre(p[1]))
                    run.append(p + np.array([0.0, side * r * 0.97, 0.0]) + out * r * 0.25)
            line += run if side > 0 else run[::-1]
            if side > 0:
                # Over the tip.
                tip = joints[3] + tipdir * radii[3] * 0.9
                line += [tip + np.array([0.0, s * radii[3] * 0.6, 0.0]) for s in (0.5, 0.0, -0.5)]
        pts = project_path(bvh, line[2:-2], 0.035)
        if len(pts) > 3:
            objs.append(tube("seam", pts, 0.065, mat))
    if objs:
        joined = join([hand] + objs, hand.name)
        return joined
    return hand


def cuff(mats, start, d, across, dorsal):
    """The gauntlet: an open leather tube from inside the wrist flaring
    toward the elbow, its rim rolled in the panel leather, a strap round the
    wrist with its tab."""
    n_around, n_along = 40, 22
    L = CUFF_CM + 1.2
    ts = np.linspace(0.0, 1.0, n_along)
    side_ax = unit(across - np.dot(across, d) * d)
    up_ax = unit(np.cross(d, side_ax))
    if np.dot(up_ax, dorsal) < 0:
        up_ax = -up_ax
    th = np.linspace(0.0, 2 * math.pi, n_around, endpoint=False)
    bm = bmesh.new()
    rows_out, rows_in = [], []
    for t in ts:
        a_s = 2.75 + 1.05 * t ** 1.4             # half width across the hand
        a_u = 2.05 + 1.15 * t ** 1.4             # half thickness
        # A soft crease or two where the leather bunches at the wrist.
        wob = 1.0 + 0.025 * np.sin(th * 3 + 1.7 * t) * smoothstep(0.1, 0.4, t) * (1 - smoothstep(0.6, 0.9, t))
        c = start + d * (L * t)
        ring_o = [c + (side_ax * math.cos(a) * a_s + up_ax * math.sin(a) * a_u) * wob[i] for i, a in enumerate(th)]
        ring_i = [c + (side_ax * math.cos(a) * (a_s - 0.18) + up_ax * math.sin(a) * (a_u - 0.18)) for a in th]
        rows_out.append([bm.verts.new(B(p)) for p in ring_o])
        rows_in.append([bm.verts.new(B(p)) for p in ring_i])
    # Rolled rim: one ring proud of the end between outside and inside.
    tip = start + d * (L + 0.2)
    rim = [bm.verts.new(B(tip + side_ax * math.cos(a) * (3.8 - 0.09) + up_ax * math.sin(a) * (3.2 - 0.09)))
           for a in th]

    def band(r0, r1, mat, facing):
        """`facing`: +1 out from the axis, -1 into it, 0 out of the opening."""
        for i in range(n_around):
            j = (i + 1) % n_around
            f = bm.faces.new((r0[i], r0[j], r1[j], r1[i]))
            f.material_index = mat
            f.smooth = True
            f.normal_update()
            c = D(f.calc_center_median())
            radial = (c - start) - np.dot(c - start, d) * d
            want = d if facing == 0 else radial * facing
            if np.dot(D(f.normal) * 0.01, want) < 0:
                f.normal_flip()
    for k in range(n_along - 1):
        mat = 2 if k >= n_along - 3 else 0
        band(rows_out[k], rows_out[k + 1], mat, 1)
        band(rows_in[k], rows_in[k + 1], 1, -1)
    band(rows_out[-1], rim, 2, 0)
    band(rim, rows_in[-1], 2, 0)
    me = bpy.data.meshes.new("gauntlet")
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new("gauntlet", me)
    bpy.context.scene.collection.objects.link(ob)
    for k in ("glove_leather", "glove_palm", "glove_cuff"):
        me.materials.append(mats[k])
    parts = [ob]

    # The strap: a band round the wrist a little proud of the gauntlet,
    # rounded at its edges, and the tab where it fastens on the back.
    s0, s1 = 1.0, 3.4
    sb = bmesh.new()
    rings = []
    prof = [(0.0, 0.0), (0.12, 0.17), (0.5, 0.24), (0.88, 0.17), (1.0, 0.0)]
    for f, lift in prof:
        x = s0 + (s1 - s0) * f
        t = x / L
        a_s = 2.75 + 1.05 * t ** 1.4 + lift
        a_u = 2.05 + 1.15 * t ** 1.4 + lift
        c = start + d * x
        rings.append([sb.verts.new(B(c + side_ax * math.cos(a) * a_s + up_ax * math.sin(a) * a_u)) for a in th])
    for k in range(len(rings) - 1):
        for i in range(n_around):
            j = (i + 1) % n_around
            sb.faces.new((rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i])).smooth = True
    sme = bpy.data.meshes.new("strap")
    sb.to_mesh(sme)
    sb.free()
    strap = bpy.data.objects.new("strap", sme)
    bpy.context.scene.collection.objects.link(strap)
    sme.materials.append(mats["glove_cuff"])
    fix_normals(strap)
    parts.append(strap)

    # The tab: a padded rounded pad over the strap on the back of the wrist.
    bpy.ops.mesh.primitive_cube_add(size=1.0)
    tab = bpy.context.active_object
    tab.name = "tab"
    mid = start + d * ((s0 + s1) / 2 + 0.2)
    a_u = 2.05 + 1.15 * (((s0 + s1) / 2) / L) ** 1.4 + 0.3
    centre = mid + up_ax * a_u * 0.98
    bevel = tab.modifiers.new("bevel", 'BEVEL')
    bevel.width = 0.0012
    bevel.segments = 3
    bevel.limit_method = 'NONE'
    tab.scale = (1.0, 1.0, 1.0)
    # Unit cube -> the tab's size (cm) in its own frame: along d, across, out.
    bm2 = bmesh.new()
    bm2.from_mesh(tab.data)
    for v in bm2.verts:
        x, y, z = v.co
        p = centre + d * (x * 2.8) + side_ax * (y * 3.4) + up_ax * (z * 0.32)
        # Curve it round the wrist.
        off = (y * 3.4)
        p -= up_ax * (off * off) / (2 * (a_u + 0.5))
        v.co = B(p)
    bm2.to_mesh(tab.data)
    bm2.free()
    tab.data.materials.append(mats["glove_cuff"])
    apply(tab, bevel)
    sub = tab.modifiers.new("sub", 'SUBSURF')
    sub.levels = 1
    apply(tab, sub)
    for p in tab.data.polygons:
        p.use_smooth = True
    parts.append(tab)
    g = join(parts, "gauntlet")
    box_uv(g, tile_cm=7.0)
    return g


def fix_normals(ob):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()


def mirror_left(right):
    """The left glove: the right mirrored across the wheel's centre (u -> -u,
    Blender x), its faces turned back outward."""
    left = right.copy()
    left.data = right.data.copy()
    left.name = "hand_l"
    bpy.context.scene.collection.objects.link(left)
    bm = bmesh.new()
    bm.from_mesh(left.data)
    for v in bm.verts:
        v.co.x = -v.co.x
    bmesh.ops.reverse_faces(bm, faces=bm.faces)
    bm.to_mesh(left.data)
    bm.free()
    return left


def export(ob, path):
    for o in bpy.context.scene.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with bpy.context.temp_override(active_object=ob, object=ob, selected_objects=[ob],
                                   selected_editable_objects=[ob]):
        bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', use_selection=True, export_apply=True,
                                  export_yup=True, export_texcoords=True, export_normals=True,
                                  export_materials='EXPORT', export_cameras=False, export_lights=False,
                                  export_image_format='AUTO')
    return os.path.getsize(path)


def build_class(name, mats, preview=False):
    reset_keep(mats)
    log(f"{name}: the wheel")
    bvh, wheel = wheel_obstacles(name)
    log(f"{name}: the right hand")
    right, wrist = build_hand(name, mats, bvh)
    log(f"{name}: the left hand")
    left = mirror_left(right)
    hands = join([right, left], "hands")
    tris = sum(len(p.vertices) - 2 for p in hands.data.polygons)
    if preview:
        render_previews(name, hands, wheel)
    for o in wheel:
        bpy.data.objects.remove(o)
    size = export(hands, os.path.join(STEER_DIR, f"{name}_hands.glb"))
    # The wrist in the rig's wheel pivot frame: +X toward the nose (= -w),
    # +Y the driver's right (= u), +Z up (= v).
    meta_path = os.path.join(STEER_DIR, f"{name}.json")
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    meta["wrist_cm"] = [round(float(-wrist[2]), 3), round(float(wrist[0]), 3), round(float(wrist[1]), 3)]
    with open(meta_path, "w", newline="\n") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(f"{name}_hands: {tris:,} triangles, {size / 1024:.0f} KB, wrist {meta['wrist_cm']}")


def reset_keep(mats):
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    for me in list(bpy.data.meshes):
        if me.users == 0:
            bpy.data.meshes.remove(me)


# ------------------------------------------------------------- sleeves
def sleeve(name, mats, length, r0, r1, cuff_cm, elbow_end):
    """A race-suit sleeve along +X (Blender = glTF), hand end at 0,
    modelled at `length` cm and stretched to 1 m. `elbow_end`: 1 when the
    elbow is the far end (the forearm), 0 when it is the near end (the
    upper arm)."""
    n_th, n_x = 64, 72
    th = np.linspace(0.0, 2 * math.pi, n_th, endpoint=False)    # 0 = +Y, pi/2 = +Z (up)
    xs = np.linspace(0.0, 1.0, n_x)
    up = np.sin(th)                   # +1 on the inside of the bend
    rng = np.random.default_rng(7 if elbow_end else 11)
    # Folds: diagonal ridges bunched toward the elbow on the bend's inside,
    # a few long soft ones along the arm, a sag below.
    folds = []
    for _ in range(7):
        folds.append((rng.uniform(0.0, 2 * math.pi), rng.uniform(0.7, 1.3), rng.uniform(-0.5, 0.5)))
    soft = [(rng.uniform(0.0, 2 * math.pi), rng.uniform(2.0, 4.0), rng.uniform(0.0, 2 * math.pi)) for _ in range(5)]
    bm = bmesh.new()
    rows = []
    for x in xs:
        e = x if elbow_end else 1.0 - x                 # 1 at the elbow
        xc = x * length
        r = r0 + (r1 - r0) * x ** 0.7
        bunch = smoothstep(0.45, 1.0, e)
        off = np.zeros(n_th)
        # Compression folds: rings tilted round the arm, strongest inside.
        for ph, pitch, tilt in folds:
            wave = np.cos((xc + tilt * r * np.cos(th - ph)) / (pitch * 1.6) * 2 * math.pi + ph)
            off += 0.11 * bunch * np.clip(0.55 + 0.6 * up, 0.0, 1.2) * np.maximum(wave, -0.4)
        for ph, k, ph2 in soft:
            off += 0.045 * np.cos(th * k + ph) * np.cos(xc / length * 2 * math.pi * 0.8 + ph2)
        sag = 1.0 + 0.05 * np.clip(-up, 0.0, 1.0)
        # The seam under the arm: a groove with a welt each side.
        dth = np.angle(np.exp(1j * (th + math.pi / 2)))
        seam = -0.07 * np.exp(-(dth / 0.035) ** 2) + 0.05 * (np.exp(-((dth - 0.07) / 0.035) ** 2)
                                                              + np.exp(-((dth + 0.07) / 0.035) ** 2))
        rr = r * sag + off + seam
        # The knit band at the hand end: ribbed and drawn in.
        if x * length < cuff_cm:
            rr = r * 0.97 + 0.05 * np.cos(th * 48)
        # Domed elbow end.
        cap = 1.0
        if elbow_end and x > 0.0:
            z = (xc - (length - r1)) / r1
            cap = math.sqrt(max(1.0 - z * z, 0.0)) if z > 0 else 1.0
        elif not elbow_end:
            z = (r0 - xc) / r0
            cap = math.sqrt(max(1.0 - z * z, 0.0)) if z > 0 else 1.0
        cap = max(cap, 0.04)
        rr = rr * cap
        rows.append([bm.verts.new((xc * 0.01 * (100.0 / length), rr[i] * math.cos(t) * 0.01,
                                   rr[i] * math.sin(t) * 0.01)) for i, t in enumerate(th)])
    uv = bm.loops.layers.uv.verify()
    for k in range(n_x - 1):
        hand_band = xs[k] * length < cuff_cm
        for i in range(n_th):
            j = (i + 1) % n_th
            f = bm.faces.new((rows[k][i], rows[k][j], rows[k + 1][j], rows[k + 1][i]))
            f.smooth = True
            f.material_index = 1 if hand_band else 0
            for lp, (kk, ii) in zip(f.loops, ((k, i), (k, i + 1), (k + 1, i + 1), (k + 1, i))):
                circ = 2 * math.pi * r0
                lp[uv].uv = (ii / n_th * circ / 10.0, xs[kk] * length / 10.0)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    me.materials.append(mats["suit"])
    me.materials.append(mats["suit_cuff"])
    size = export(ob, os.path.join(HANDS_DIR, f"{name}.glb"))
    print(f"{name}: {len(me.polygons) * 2:,} triangles, {size / 1024:.0f} KB")
    return ob


# ------------------------------------------------------------- previews
def render_previews(name, hands, wheel):
    """The driver's view and a close one, Cycles, into out/hands/."""
    out = os.path.join(ROOT, "..", "apexsim", "out", "hands")
    os.makedirs(out, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    # CPU, a few threads: a GPU render from a headless run has hung the machine.
    sc.cycles.device = 'CPU'
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 6
    sc.cycles.samples = 32
    sc.cycles.use_denoising = True
    sc.render.resolution_x, sc.render.resolution_y = 960, 540
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    sky = world.node_tree.nodes.new("ShaderNodeTexSky")
    world.node_tree.links.new(sky.outputs["Color"], world.node_tree.nodes["Background"].inputs["Color"])
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.12
    sc.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", 'SUN'))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(30))
    sc.collection.objects.link(sun)
    sc.view_settings.view_transform = 'AgX'
    sc.view_settings.exposure = -0.7
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    views = {
        "eye": ((0.0, 14.0, 62.0), (0.0, -2.0, 0.0), 40.0),
        "eye_r": ((-2.0, 16.0, 60.0), (16.0, 0.0, 2.0), 18.0),
        "close": ((30.0, 10.0, 26.0), (15.0, 0.0, 0.0), 35.0),
        "front": ((10.0, 6.0, -40.0), (14.0, 0.0, 0.0), 40.0),
        "under": ((22.0, -24.0, 20.0), (15.0, 0.0, 2.0), 35.0),
    }
    for view, (eye, look, fov) in views.items():
        cam.location = B(eye)
        cam.rotation_euler = (B(look) - B(eye)).to_track_quat('-Z', 'Z').to_euler()
        cam.data.angle = math.radians(fov)
        sc.render.filepath = os.path.join(out, f"{name}_{view}.png")
        bpy.ops.render.render(write_still=True)


def sleeve_preview(objs):
    pass


def run():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    preview = "--preview" in argv
    wanted = [a for a in argv if not a.startswith("--")] or list(GRIPS)
    reset()
    mats = materials()
    for n in wanted:
        build_class(n, mats, preview)
    if not [a for a in argv if not a.startswith("--")]:
        reset_keep(mats)
        sleeve("forearm", mats, FOREARM_CM, 3.5, 4.8, 3.5, elbow_end=1)
        sleeve("upper_arm", mats, UPPER_ARM_CM, 4.9, 6.0, 0.0, elbow_end=0)


if bpy is not None:
    run()
