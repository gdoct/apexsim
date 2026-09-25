r"""misc/bull_statue -- "Der Bulle vom Spielberg" at its real size.

The first statue in the kit was a 2.5 m bronze bull on a plinth: in the
game it stood in the fan zone at the Red Bull Ring like a garden ornament.
The real one (Clemens Neugebauer and Martin Kolldorfer, 2012) is made to be
seen from the motorway two kilometres away. Its fabricator gives the figures
(3D Kunst GmbH, "Der Bulle aus Stahl"):

  * the bull is 14.6 m high, and the whole sculpture, arch included, 17.2 m
  * the horns are cast aluminium, 7 m from tip to tip
  * the skin is ~1,700 welded Corten plates over a steel skeleton
  * the arch is 83 lost-foam aluminium castings, weathered grey to copper

The arch's span is not published. 20 m at the ground is read off photos
against its height. The bull charges through the arch's plane with its
hind hooves at one foot and its head past the other, so the arch is
turned ARCH_YAW off the bull's line: one leg passes in front of the
hindquarters and the other behind the shoulders, as in the photos.

The bull is authored at roughly life size (x forward, z up, hooves on the
ground), pitched nose-up about its hind hooves, and then scaled so it is
BULL_HEIGHT_M high. Pivot on the ground at the middle of the bull. The bull
charges along +X, which is the course heading at the landmark
(`dress::lay_landmark`), so it is seen side-on from the road.

    exec(open(r"D:\apexsim\content\props\_batches\build_bull_statue.py").read())

or headless:
    blender -b --factory-startup -P content/props/_batches/build_bull_statue.py
"""
import bpy, math, os, importlib.util, sys
from mathutils import Vector, Matrix

_ROOT = "D:\\apexsim"
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "content\\props\\_tools\\apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
_t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "content\\props\\_tools\\apex_tex.py"))
tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
B, M = apex.Builder, tex.kit_material

BULL_HEIGHT_M = 14.6           # the bull alone, horn tips included
HORN_SPAN_M = 7.0              # tip to tip (checked, not imposed)
ARCH_HEIGHT_M = 17.2           # the whole sculpture
ARCH_SPAN_M = 20.0             # outside of the feet at the ground (from photos)
ARCH_YAW = math.radians(28.0)  # the arch's plane off the bull's line
PITCH = math.radians(14.0)     # nose-up: a charge, pushing off the hind legs
PLATE_M = 0.3                  # the concrete pads the hooves stand on

HOOF_PIVOT = Vector((-0.95, 0.0, 0.0))


def pitch(p):
    """Life-size body frame -> the charging pose (still life-size)."""
    return HOOF_PIVOT + Matrix.Rotation(-PITCH, 3, 'Y') @ (Vector(p) - HOOF_PIVOT)


def section(c, t, hw, top, bottom, n, sq=2.4):
    """A superellipse ring centred on c, square to the XZ tangent t,
    reaching `top` above the centre and `bottom` below it."""
    t = Vector((t.x, 0.0, t.z)).normalized()
    up, side = Vector((-t.z, 0.0, t.x)), Vector((0.0, 1.0, 0.0))
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        ca, sa = math.cos(a), math.sin(a)
        y = hw * math.copysign(abs(ca) ** (2 / sq), ca)
        v = math.copysign(abs(sa) ** (2 / sq), sa) * (top if sa > 0 else bottom)
        pts.append(c + side * y + up * v)
    return pts


def tube(b, mat, joints, radii, n=10, ref=(0.0, 1.0, 0.0), flat=1.0):
    """A tapered tube through `joints`, one ring per joint, capped."""
    pts = [Vector(j) for j in joints]
    ref = Vector(ref)
    rings = []
    for i, p in enumerate(pts):
        if i == 0:
            t = pts[1] - pts[0]
        elif i == len(pts) - 1:
            t = pts[-1] - pts[-2]
        else:
            t = (pts[i + 1] - p).normalized() + (p - pts[i - 1]).normalized()
        t.normalize()
        u = t.cross(ref).normalized()
        v = t.cross(u).normalized()
        r = radii[i]
        rings.append([p + (u * math.cos(2 * math.pi * k / n) + v * math.sin(2 * math.pi * k / n) * flat) * r
                      for k in range(n)])
    b.loft(mat, rings)


# Body, neck and head as one loft, life size: (x, z, half width, height
# above the spine, depth below it). The mass is in the shoulders and the
# hump; the belly tucks up at the flank; the head is carried low, forehead
# first, as a bull charges.
SPINE = [
    (-1.24, 1.08, 0.08, 0.10, 0.10),   # tail head
    (-1.16, 1.07, 0.26, 0.28, 0.34),
    (-1.00, 1.06, 0.36, 0.34, 0.44),   # rump
    (-0.74, 1.03, 0.37, 0.36, 0.44),
    (-0.42, 1.00, 0.34, 0.38, 0.36),   # flank: the waist
    (-0.05, 1.04, 0.42, 0.44, 0.46),   # ribs
    (0.30, 1.10, 0.48, 0.62, 0.54),    # shoulders
    (0.58, 1.10, 0.46, 0.62, 0.56),    # the hump
    (0.80, 1.02, 0.36, 0.48, 0.52),    # neck, over the dewlap
    (0.98, 0.96, 0.28, 0.36, 0.40),
    (1.12, 0.92, 0.25, 0.32, 0.30),    # forehead
    (1.26, 0.80, 0.22, 0.24, 0.24),
    (1.38, 0.68, 0.17, 0.18, 0.18),
    (1.46, 0.60, 0.14, 0.14, 0.14),    # muzzle
    (1.50, 0.57, 0.08, 0.07, 0.07),
]


def bull(b, m):
    """Everything but the arch, life size in the charging pose. Returns
    the hooves on the ground (for their pads)."""
    corten, gold = m["corten"], m["gold"]
    centres = [Vector((x, 0.0, z)) for x, z, _, _, _ in SPINE]
    rings = []
    for i, (x, z, hw, top, bottom) in enumerate(SPINE):
        a = centres[max(i - 1, 0)]
        c = centres[min(i + 1, len(centres) - 1)]
        rings.append([pitch(p) for p in section(centres[i], c - a, hw, top, bottom, 20)])
    b.loft(corten, rings)

    # Legs: hip and shoulder ride the body, the rest is posed in the
    # pitched frame so the planted hooves stand on the ground.
    grounded = []
    for s in (-1, 1):
        hip = pitch((-0.80, 0.25 * s, 0.95))
        back = 0.22 if s < 0 else 0.05       # one hind leg pushes off further back
        hoof = Vector((hip.x - back + 0.04, hip.y, 0.0))
        tube(b, corten, [hip, hip + Vector((0.12, 0, -0.40)), Vector((hip.x - back, hip.y, 0.36)),
                         Vector((hip.x - back + 0.02, hip.y, 0.10)), hoof],
             [0.21, 0.15, 0.085, 0.07, 0.085])
        grounded.append(hoof)
    # The road-side (-Y) foreleg reaches forward to the ground; the far one
    # is lifted and folded, mid-stride.
    sh = pitch((0.50, -0.26, 0.76))
    hoof = Vector((sh.x + 0.46, sh.y, 0.0))
    tube(b, corten, [sh, sh + Vector((0.05, 0, -0.32)), Vector((sh.x + 0.28, sh.y, 0.34)),
                     Vector((sh.x + 0.43, sh.y, 0.10)), hoof],
         [0.19, 0.13, 0.085, 0.07, 0.085])
    grounded.append(hoof)
    sh = pitch((0.50, 0.26, 0.76))
    tube(b, corten, [sh, sh + Vector((0.12, 0, -0.28)), sh + Vector((0.36, 0, -0.42)),
                     sh + Vector((0.26, 0, -0.68)), sh + Vector((0.20, 0, -0.76))],
         [0.19, 0.13, 0.085, 0.07, 0.085])

    # Horns: out from the poll, then forward and up to points aimed ahead.
    # Aluminium, gilded; tip to tip is HORN_SPAN_M once scaled.
    for s in (-1, 1):
        tube(b, gold, [pitch((1.08, 0.14 * s, 1.16)), pitch((1.09, 0.33 * s, 1.19)),
                       pitch((1.20, 0.44 * s, 1.25)), pitch((1.35, 0.45 * s, 1.36)),
                       pitch((1.47, 0.40 * s, 1.49))],
             [0.095, 0.078, 0.058, 0.036, 0.012], n=10, ref=(0.0, 0.0, 1.0))
        b.ico(corten, pitch((1.02, 0.29 * s, 1.08)), 1.0, subdiv=1, scale=(0.05, 0.11, 0.07))  # ears

    # Tail, flung out behind and falling.
    t0 = pitch((-1.22, 0.0, 1.11))
    tube(b, corten, [t0, t0 + Vector((-0.16, 0.0, -0.10)), t0 + Vector((-0.26, 0.03, -0.40)),
                     t0 + Vector((-0.24, 0.05, -0.68))], [0.05, 0.04, 0.03, 0.03], n=8)
    b.ico(corten, t0 + Vector((-0.24, 0.05, -0.76)), 1.0, subdiv=1, scale=(0.06, 0.06, 0.11))
    return grounded


def arch(b, mat, x0, steps=48):
    """A parabola in its own vertical plane, thicker toward the feet, the
    feet sunk a metre below the ground; turned ARCH_YAW about the pivot."""
    w_top, w_foot = 1.5, 2.6       # in the arch's plane
    d_top, d_foot = 1.2, 1.9       # across it
    H = ARCH_HEIGHT_M - w_top / 2
    a = ARCH_SPAN_M / 2 - w_foot / 2
    rot = Matrix.Rotation(ARCH_YAW, 3, 'Z')
    rings = []
    for i in range(steps + 1):
        u = -1.06 + 2.12 * i / steps
        c = Vector((a * u, 0.0, H * (1 - u * u)))
        t = Vector((a, 0.0, -2 * H * u)).normalized()
        n = Vector((-t.z, 0.0, t.x))
        f = min(u * u, 1.0)
        hw = (w_top + (w_foot - w_top) * f) / 2
        hd = (d_top + (d_foot - d_top) * f) / 2
        side = Vector((0.0, 1.0, 0.0))
        rings.append([Vector((x0, 0, 0)) + rot @ (c + n * sn * hw + side * sd * hd)
                      for sn, sd in ((1, 1), (1, -1), (-1, -1), (-1, 1))])
    b.loft(mat, rings)


def bull_statue():
    m = dict(
        corten=M("statue_corten", (0.36, 0.15, 0.07), roughness=0.8),
        gold=M("statue_gold", (0.83, 0.62, 0.25), metallic=1.0, roughness=0.3),
        arch=M("statue_arch", (0.56, 0.56, 0.55), metallic=0.8, roughness=0.45),
        plinth=M("statue_plinth", (0.5, 0.5, 0.5), roughness=0.85),
    )
    b = B("bull_statue")
    hooves = bull(b, m)
    # Life size -> the sculpture: scale about the ground, stand it on its
    # pads and centre it on the pivot.
    top = max(v.co.z for v in b.bm.verts)
    k = BULL_HEIGHT_M / top
    xs = [v.co.x * k for v in b.bm.verts]
    dx = -(min(xs) + max(xs)) / 2
    for v in b.bm.verts:
        v.co = Vector((v.co.x * k + dx, v.co.y * k, v.co.z * k + PLATE_M))
    for h in hooves:
        x, y = h.x * k + dx, h.y * k
        b.box(m["plinth"], (x - 0.9, y - 0.9, -0.2), (x + 0.9, y + 0.9, PLATE_M))
    arch(b, m["arch"], 0.0)
    return b.finish(), k


apex.reset_scene()
tex.reset_cache()
ob, scale = bull_statue()
exported = apex.export_one("misc", "bull_statue")
blend = apex.save_blend("misc", "bull_statue")
me = ob.data
gold = [me.vertices[i].co.y for p in me.polygons if me.materials[p.material_index].name == "statue_gold"
        for i in p.vertices]
result = {"exported": exported, "blend": blend, "life_size_scale": round(scale, 2),
          "horn_span_m": round(max(gold) - min(gold), 2), "stats": apex.stats()}
print("RESULT", result)
