"""Build one of the GT3 cars in Blender and export it to content/cars/<folder>/<stem>.glb.

    VARIANT = "limbotiti"      # posh | limbotiti | murcetes
    exec(open(r"D:\\apexsim\\content\\cars\\build_gt3.py").read())

Unlike the LMP2s (one hull, parameter tweaks) each GT3 has its own set of
cross-section keys, so the silhouettes differ: a rounded rear-engined coupe
with a fastback and wide hips, a low sharp wedge, and a long-bonnet GT.
Frame: nose on -Y, ground z=0, metres; left-hand drive (driver on +X).
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

VARIANTS = {
    "posh": dict(
        folder="posh-gt3rs", stem="posh_gt3rs", logo="posh_logo.png",
        paint=(0.10, 0.55, 0.75), accent=(0.05, 0.05, 0.05), caliper=(0.95, 0.75, 0.05),
        wheels=((-1.35, 0.265), (1.40, 0.275)), arch_r=0.42, glass_y=(-0.95, 1.15), screen_y=(-0.95, -0.25), rear_glass_y=(0.55, 1.15),
        wing="pylon", wing_z=1.20, wing_y=1.85, ducktail=True, lights="round", grille=False, louvres=False,
        exhaust="centre", mirror="pod",
        panel_lines=[(-1.62, "upper"), (-0.98, "side"), (0.62, "side"), (1.25, "upper"), (1.75, "upper")],
        vents=["fender", "naca_front"], scoop=None,
        keys=[
            (-2.25, [(0, .20), (.55, .20), (.72, .32), (.78, .50), (.60, .62), (.45, .66), (.30, .68), (.14, .70), (0, .70)]),
            (-1.90, [(0, .10), (.85, .10), (.95, .35), (.97, .62), (.80, .78), (.60, .76), (.40, .74), (.18, .74), (0, .74)]),
            (-1.35, [(0, .08), (.90, .08), (.99, .40), (1.0, .68), (.82, .86), (.62, .80), (.42, .78), (.18, .78), (0, .78)]),
            (-0.95, [(0, .08), (.90, .08), (.98, .40), (.98, .66), (.84, .82), (.66, .80), (.46, .82), (.20, .82), (0, .82)]),
            (-0.60, [(0, .08), (.90, .08), (.98, .40), (.98, .66), (.85, .78), (.70, .90), (.52, 1.04), (.30, 1.10), (0, 1.12)]),
            (-0.20, [(0, .08), (.90, .08), (.98, .40), (.98, .66), (.86, .78), (.72, .92), (.56, 1.14), (.32, 1.27), (0, 1.30)]),
            ( 0.40, [(0, .08), (.92, .08), (1.00, .40), (1.00, .68), (.88, .80), (.74, .92), (.58, 1.10), (.32, 1.24), (0, 1.28)]),
            ( 1.00, [(0, .08), (.94, .08), (1.02, .42), (1.03, .72), (.90, .84), (.74, .90), (.52, 1.00), (.28, 1.10), (0, 1.13)]),
            ( 1.40, [(0, .08), (.94, .08), (1.02, .42), (1.04, .74), (.90, .86), (.72, .90), (.48, .96), (.24, 1.02), (0, 1.04)]),
            ( 1.90, [(0, .14), (.90, .14), (.98, .42), (1.00, .72), (.86, .84), (.66, .90), (.44, .94), (.20, .98), (0, 1.00)]),
            ( 2.30, [(0, .30), (.78, .30), (.86, .48), (.88, .70), (.76, .82), (.58, .88), (.38, .92), (.16, .94), (0, .95)]),
        ]),
    "limbotiti": dict(
        folder="limbotiti-caravan-gt3", stem="limbotiti_caravan", logo="limbotiti_logo.png",
        paint=(0.95, 0.72, 0.02), accent=(0.05, 0.05, 0.05), caliper=(0.05, 0.05, 0.05),
        wheels=((-1.35, 0.265), (1.35, 0.275)), arch_r=0.42, glass_y=(-1.00, 0.55), screen_y=(-1.00, -0.32), rear_glass_y=(0.38, 0.58),
        wing="swan", wing_z=1.05, wing_y=1.80, ducktail=False, lights="ybar", grille=False, louvres=True, smooth=False,
        exhaust="hexquad", mirror="stalk",
        panel_lines=[(-1.70, "upper"), (-1.00, "side"), (0.58, "side"), (0.60, "upper"), (1.80, "upper")],
        vents=["fender", "side_intake"], scoop=(0.0, 0.28, 0.14),
        keys=[
            (-2.25, [(0, .18), (.70, .18), (.86, .28), (.92, .44), (.70, .52), (.50, .54), (.30, .56), (.14, .57), (0, .58)]),
            (-1.85, [(0, .10), (.90, .10), (.98, .32), (1.00, .58), (.82, .70), (.62, .74), (.42, .76), (.20, .78), (0, .79)]),
            (-1.35, [(0, .08), (.92, .08), (1.00, .36), (1.01, .64), (.84, .74), (.64, .74), (.44, .74), (.20, .75), (0, .76)]),
            (-1.00, [(0, .08), (.92, .08), (1.00, .36), (1.00, .66), (.88, .78), (.76, .78), (.60, .78), (.30, .78), (0, .78)]),
            (-0.65, [(0, .08), (.92, .08), (1.00, .36), (1.00, .66), (.88, .80), (.76, .84), (.60, .96), (.30, 1.00), (0, 1.01)]),
            (-0.30, [(0, .08), (.92, .08), (1.00, .36), (1.00, .68), (.90, .82), (.78, .86), (.62, 1.08), (.32, 1.12), (0, 1.13)]),
            ( 0.30, [(0, .08), (.94, .08), (1.02, .38), (1.02, .70), (.92, .84), (.80, .88), (.62, 1.06), (.32, 1.10), (0, 1.11)]),
            ( 0.85, [(0, .08), (.96, .08), (1.04, .40), (1.04, .72), (.92, .84), (.76, .90), (.54, .98), (.26, 1.02), (0, 1.03)]),
            ( 1.35, [(0, .08), (.96, .08), (1.04, .40), (1.05, .74), (.92, .86), (.74, .92), (.50, .96), (.24, .98), (0, .99)]),
            ( 1.85, [(0, .14), (.94, .14), (1.02, .40), (1.02, .72), (.90, .84), (.70, .90), (.46, .94), (.22, .96), (0, .97)]),
            ( 2.25, [(0, .32), (.84, .32), (.92, .50), (.94, .70), (.80, .80), (.60, .86), (.38, .90), (.16, .92), (0, .93)]),
        ]),
    "murcetes": dict(
        folder="murcetes-amd-gt3", stem="murcetes_amd_gt3", logo="murcetes_logo.png",
        paint=(0.80, 0.80, 0.82), accent=(0.0, 0.65, 0.62), caliper=(0.85, 0.10, 0.05),
        wheels=((-1.45, 0.265), (1.35, 0.275)), arch_r=0.42, glass_y=(-0.45, 1.20), screen_y=(-0.45, 0.35), rear_glass_y=(0.85, 1.20),
        wing="pylon", wing_z=1.16, wing_y=1.90, ducktail=False, lights="slant", grille=True, louvres=False,
        exhaust="side", mirror="pod",
        panel_lines=[(-1.75, "upper"), (-0.45, "upper"), (-0.42, "side"), (0.75, "side"), (1.45, "upper"), (1.90, "upper")],
        vents=["fender", "bonnet_louvres"], scoop=None,
        keys=[
            (-2.35, [(0, .22), (.60, .22), (.76, .34), (.82, .54), (.64, .64), (.46, .68), (.28, .70), (.12, .72), (0, .72)]),
            (-1.95, [(0, .10), (.88, .10), (.97, .36), (.99, .64), (.82, .78), (.62, .84), (.42, .86), (.18, .88), (0, .89)]),
            (-1.45, [(0, .08), (.92, .08), (1.00, .40), (1.01, .70), (.84, .84), (.64, .84), (.44, .85), (.18, .86), (0, .86)]),
            (-0.80, [(0, .08), (.92, .08), (1.00, .40), (1.00, .70), (.86, .82), (.66, .84), (.46, .86), (.20, .87), (0, .87)]),
            (-0.45, [(0, .08), (.92, .08), (1.00, .40), (1.00, .70), (.86, .82), (.68, .86), (.48, .88), (.22, .88), (0, .88)]),
            (-0.10, [(0, .08), (.92, .08), (1.00, .40), (1.00, .70), (.86, .82), (.70, .92), (.52, 1.06), (.28, 1.14), (0, 1.16)]),
            ( 0.40, [(0, .08), (.94, .08), (1.02, .42), (1.02, .72), (.88, .84), (.72, .94), (.56, 1.14), (.30, 1.25), (0, 1.28)]),
            ( 0.95, [(0, .08), (.96, .08), (1.04, .44), (1.05, .76), (.92, .88), (.74, .96), (.52, 1.08), (.26, 1.16), (0, 1.18)]),
            ( 1.35, [(0, .08), (.96, .08), (1.04, .44), (1.06, .78), (.92, .90), (.72, .96), (.48, 1.02), (.24, 1.06), (0, 1.08)]),
            ( 1.90, [(0, .16), (.92, .16), (1.00, .44), (1.02, .76), (.88, .88), (.66, .94), (.42, .98), (.20, 1.00), (0, 1.01)]),
            ( 2.35, [(0, .34), (.80, .34), (.88, .50), (.90, .72), (.76, .84), (.56, .90), (.36, .94), (.16, .96), (0, .97)]),
        ]),
}
try:
    VARIANT
except NameError:
    VARIANT = "posh"
V = VARIANTS[VARIANT]
CAR_DIR = os.path.join(r"D:\apexsim\content\cars", V["folder"])
TOOLS = r"D:\apexsim\content\props\_tools\apex_props.py"
spec = importlib.util.spec_from_file_location("apex", TOOLS)
apex = importlib.util.module_from_spec(spec); sys.modules["apex"] = apex; spec.loader.exec_module(apex)
from apex import Builder, material, reset_scene

def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend")); print("saved", tag)

def mat(name, color, metallic=0.0, roughness=0.5, coat=0.0, alpha=1.0, emission=None):
    m = material(name, color, metallic, roughness, emission); b = m.node_tree.nodes["Principled BSDF"]
    if coat: b.inputs["Coat Weight"].default_value = coat
    if alpha < 1.0:
        b.inputs["Alpha"].default_value = alpha
        try: m.surface_render_method = 'BLENDED'
        except Exception: pass
    return m

reset_scene()
paint = mat("car_paint", V["paint"], 0.1, 0.25, coat=1.0)
accent = mat("car_accent", V["accent"], 0.05, 0.3, coat=1.0)
carbon = mat("car_carbon", (0.05, 0.05, 0.06), 0.3, 0.32, coat=0.6)
glass = mat("car_glass", (0.02, 0.03, 0.04), 0.0, 0.05, alpha=0.5)
liner = mat("car_liner", (0.03, 0.03, 0.03), 0.0, 0.9)
interior = mat("car_interior", (0.12, 0.12, 0.13), 0.1, 0.8)
seat_m = mat("car_seat", (0.10, 0.10, 0.12), 0.0, 0.85)
alc = mat("car_wheel_rim", (0.06, 0.06, 0.06), 0.0, 0.7)
metal = mat("car_metal", (0.6, 0.6, 0.62), 1.0, 0.35)
cage = mat("car_cage", (0.75, 0.75, 0.78), 0.9, 0.4)
tyre = mat("car_tyre", (0.025, 0.025, 0.025), 0.0, 0.85)
rim = mat("car_rim", (0.12, 0.12, 0.13), 1.0, 0.35)
disc = mat("car_brake", (0.35, 0.33, 0.30), 1.0, 0.6)
caliper = mat("car_caliper", V["caliper"], 0.2, 0.4)
lamp = mat("car_headlight", (0.95, 0.95, 0.9), 0.0, 0.1, emission=(1.0, 0.98, 0.9))
lamp_h = mat("car_lamp_housing", (0.02, 0.02, 0.02), 0.6, 0.3)
tail = mat("car_taillight", (0.5, 0.03, 0.02), 0.0, 0.15, emission=(1.0, 0.08, 0.04))
display = mat("car_display", (0.02, 0.02, 0.03), 0.0, 0.2, emission=(0.1, 0.4, 0.2))
logo = apex.image_material("car_logo", os.path.join(CAR_DIR, "textures", V["logo"]), roughness=0.3, masked=True)

# ---------------------------------------------------------------- body loft
def catmull(pts, n):
    P = [Vector(p) for p in pts]; P = [P[0]] + P + [P[-1]]; out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(n):
            t = k / n
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(P[-2]); return out
KEYS = V["keys"]; NPC = len(KEYS[0][1]); NY = 96; SAMP = 4; key_ys = [k[0] for k in KEYS]
cp_curves = [catmull([(k[0], k[1][j][0], k[1][j][1]) for k in KEYS], 12) for j in range(NPC)]
def ctrl_at(y):
    out = []
    for c in cp_curves:
        for i in range(len(c) - 1):
            if c[i].x <= y <= c[i + 1].x:
                t = (y - c[i].x) / max(c[i + 1].x - c[i].x, 1e-6); v = c[i].lerp(c[i + 1], t); out.append((v.y, v.z)); break
        else:
            out.append((c[-1].y, c[-1].z) if y > c[-1].x else (c[0].y, c[0].z))
    return out
def ring(y):
    right = [(max(p.x, 0.0), p.y) for p in catmull(ctrl_at(y), SAMP)]
    right[0] = (0.0, right[0][1]); right[-1] = (0.0, right[-1][1])
    return [(x, y, z) for (x, z) in right + [(-x, z) for (x, z) in reversed(right[1:-1])]]
b = Builder("body"); bm = b.bm
ys = [key_ys[0] + (key_ys[-1] - key_ys[0]) * i / NY for i in range(NY + 1)]
rings = [[bm.verts.new(p) for p in ring(y)] for y in ys]; M = len(rings[0]); half = (M + 1) // 2
sp, sg = b.slot(paint), b.slot(glass); WIN0, WIN1 = 4 * SAMP + 1, 7 * SAMP - 1   # window band: belt line up to the roof edge
g0, g1 = V["glass_y"]; s0, s1 = V["screen_y"]; r0, r1 = V["rear_glass_y"]
for i in range(NY):
    y = (ys[i] + ys[i + 1]) / 2
    for k in range(M):
        k2 = (k + 1) % M; f = bm.faces.new((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]))
        kk = k if k < half else M - k
        side = WIN0 <= kk < WIN1 and g0 < y < g1                 # side windows: window line to roof edge
        full = kk >= WIN0 and (s0 < y < s1 or r0 < y < r1)      # windscreen / rear window: everything above the window line
        f.material_index = sg if (side or full) else sp
        for l in f.loops: l[b.uv].uv = (l.vert.co.y, l.vert.co.x + l.vert.co.z)
        b.keep.add(f)
for i, order in ((0, -1), (NY, 1)):
    f = bm.faces.new(rings[i] if order > 0 else list(reversed(rings[i]))); f.material_index = sp
    for l in f.loops: l[b.uv].uv = (l.vert.co.x, l.vert.co.z)
    b.keep.add(f)
body = b.finish()
if V.get("smooth", True):
    sub = body.modifiers.new("smooth", 'SUBSURF'); sub.levels = 1
    with bpy.context.temp_override(object=body): bpy.ops.object.modifier_apply(modifier="smooth")
body.data.shade_smooth()
save("body")

# ---------------------------------------------------------------- wheel arches
def flip(ob):
    bm2 = bmesh.new(); bm2.from_mesh(ob.data); bmesh.ops.reverse_faces(bm2, faces=bm2.faces); bm2.to_mesh(ob.data); bm2.free()
part_objects = []
R_ARCH = V["arch_r"]
for (y, _) in V["wheels"]:
    for sx in (-1, 1):
        cut = Builder("cut"); cut.cylinder(paint, (0.52 if sx > 0 else -1.25, y, R_ARCH - 0.06), R_ARCH, 0.73, segs=40, axis='X'); c = cut.finish()
        mod = body.modifiers.new("arch", 'BOOLEAN'); mod.operation = 'DIFFERENCE'; mod.object = c; mod.solver = 'EXACT'
        with bpy.context.temp_override(object=body): bpy.ops.object.modifier_apply(modifier=mod.name)
        bpy.data.objects.remove(c)
        x0 = 0.52 if sx > 0 else -1.05
        ln = Builder("liner"); ln.cylinder(liner, (x0, y, R_ARCH - 0.06), R_ARCH - 0.003, 0.53, segs=40, axis='X', caps=False); l_ob = ln.finish(); flip(l_ob)
        cap = Builder("linercap"); cap.cylinder(liner, (0.52 if sx > 0 else -0.52, y, R_ARCH - 0.06), R_ARCH - 0.003, 0.01 * sx, segs=40, axis='X'); c_ob = cap.finish()
        part_objects += [l_ob, c_ob]
save("arches")

# ---------------------------------------------------------------- parts
def sphere(b, m, center, r, seg=16, ring_=10, scale=(1, 1, 1)):
    s = b.slot(m); g = bmesh.ops.create_uvsphere(b.bm, u_segments=seg, v_segments=ring_, radius=r); vs = set(g['verts'])
    for v in vs: v.co = Vector((v.co.x * scale[0], v.co.y * scale[1], v.co.z * scale[2])) + Vector(center)
    for f in b.bm.faces:
        if f.verts[0] in vs: f.material_index = s
def torus(b, m, center, R, r, segs=32, rings_=12):
    s = b.slot(m); cx, cy, cz = center; grid = []
    for i in range(segs):
        a = 2 * math.pi * i / segs; row = []
        for j in range(rings_):
            t = 2 * math.pi * j / rings_
            row.append(b.bm.verts.new((cx + r * math.sin(t), cy + (R + r * math.cos(t)) * math.cos(a), cz + (R + r * math.cos(t)) * math.sin(a))))
        grid.append(row)
    for i in range(segs):
        for j in range(rings_):
            f = b.bm.faces.new((grid[i][j], grid[(i + 1) % segs][j], grid[(i + 1) % segs][(j + 1) % rings_], grid[i][(j + 1) % rings_])); f.material_index = s
def foil(b, m, x0, x1, chord, thick, camber, ly, lz, n=14, angle_deg=-8.0):
    s = b.slot(m); a = math.radians(angle_deg); pts = []
    def yt(t): return 5 * thick * (0.2969 * math.sqrt(t) - 0.126 * t - 0.3516 * t ** 2 + 0.2843 * t ** 3 - 0.1015 * t ** 4)
    def yc(t): return camber * (2 * t - t * t)
    for i in range(n + 1):
        t = i / n; pts.append((t * chord, (yc(t) + yt(t)) * chord))
    for i in range(n, -1, -1):
        t = i / n; pts.append((t * chord, (yc(t) - yt(t)) * chord))
    prof = [(ly + py * math.cos(a) - pz * math.sin(a), lz + py * math.sin(a) + pz * math.cos(a)) for (py, pz) in pts]
    A = [b.bm.verts.new((x0, y, z)) for (y, z) in prof]; B = [b.bm.verts.new((x1, y, z)) for (y, z) in prof]; L = len(prof)
    for i in range(L):
        j = (i + 1) % L; f = b.bm.faces.new((A[i], A[j], B[j], B[i])); f.material_index = s
    f = b.bm.faces.new(A); f.material_index = s; f = b.bm.faces.new(list(reversed(B))); f.material_index = s

p = Builder("parts"); bm = p.bm
NOSE, TAIL = key_ys[0], key_ys[-1]
# wheels
for (y, R) in V["wheels"]:
    w = 0.30 if y < 0 else 0.33
    for sx in (-1, 1):
        x = sx * 0.80; zc = R + 0.085
        torus(p, tyre, (x, y, zc), R, 0.085, segs=40, rings_=14)
        p.cylinder(tyre, (x - w / 2, y, zc), R + 0.01, w, segs=40, axis='X', caps=False)
        p.cylinder(rim, (x - w / 2 + 0.03, y, zc), R - 0.01, w - 0.06, segs=32, axis='X', caps=False)
        xo = x + sx * (w / 2 - 0.04); p.cylinder(rim, (xo - 0.01, y, zc), 0.09, 0.02, segs=16, axis='X')
        for k in range(5):
            a = 2 * math.pi * k / 5; c, s_ = math.cos(a), math.sin(a)
            for da in (-0.12, 0.12):
                c2, s2 = math.cos(a + da), math.sin(a + da)
                p.bar(rim, (xo, y + 0.08 * c2, zc + 0.08 * s2), (xo - sx * 0.03, y + (R - 0.03) * c2, zc + (R - 0.03) * s2), 0.014, segs=6)
        p.cylinder(disc, (x - 0.02 * sx - 0.012, y, zc), 0.17, 0.024, segs=32, axis='X')
        p.box(caliper, (x - 0.045, y + 0.06, zc + 0.04), (x + 0.045, y + 0.19, zc + 0.16))
# cockpit (LHD: driver on +X), cage, dash
def roof_z(y): return ctrl_at(y)[8][1]
DX = 0.38
EYE_Z = 0.70 * (V["wing_z"] + 0.22)        # the client's closed-cockpit eye: 70% of the box height (the wing endplates top it)
DASH_Z = min(EYE_Z - 0.16, roof_z(-0.45) - 0.06); WHEEL_Z = EYE_Z - 0.20
p.box(interior, (-0.80, -0.55, 0.12), (0.80, 1.20, 0.16))
p.box(interior, (-0.85, -0.55, 0.16), (-0.78, 1.20, 0.60)); p.box(interior, (0.78, -0.55, 0.16), (0.85, 1.20, 0.60))
p.box(seat_m, (DX - 0.26, 0.15, 0.16), (DX + 0.26, 0.62, 0.32)); p.box(seat_m, (DX - 0.28, 0.50, 0.32), (DX + 0.28, 0.66, 1.00))
p.box(seat_m, (DX - 0.32, 0.13, 0.32), (DX - 0.24, 0.62, 0.45)); p.box(seat_m, (DX + 0.24, 0.13, 0.32), (DX + 0.32, 0.62, 0.45))
p.box(interior, (-0.80, -0.55, 0.55), (0.80, -0.20, DASH_Z))
p.box(display, (DX - 0.12, -0.21, DASH_Z - 0.16), (DX + 0.12, -0.20, DASH_Z - 0.04))
p.bar(metal, (DX, -0.25, WHEEL_Z - 0.04), (DX, 0.05, WHEEL_Z), 0.02)
wr = [Vector((DX + 0.16 * math.cos(2 * math.pi * i / 24), 0.05, WHEEL_Z + 0.16 * math.sin(2 * math.pi * i / 24))) for i in range(24)]
for i in range(24): p.bar(alc, wr[i], wr[(i + 1) % 24], 0.018, segs=6)
p.box(alc, (DX - 0.15, 0.04, WHEEL_Z - 0.02), (DX + 0.15, 0.065, WHEEL_Z + 0.02)); p.box(alc, (DX - 0.03, 0.04, WHEEL_Z - 0.16), (DX + 0.03, 0.065, WHEEL_Z))
# roll cage: main hoop, A-pillar bars, door bars, rear stays; sized from the roof line
HZ = roof_z(0.70) - 0.10; AZ = roof_z(-0.40) - 0.10
for x in (-0.72, 0.72):
    p.bar(cage, (x, 0.70, 0.20), (x, 0.70, HZ), 0.022); p.bar(cage, (x, 0.70, HZ), (x * 0.9, -0.40, AZ), 0.022)
    p.bar(cage, (x * 0.9, -0.40, AZ), (x * 0.95, -0.55, 0.60), 0.022); p.bar(cage, (x, 0.70, HZ - 0.07), (x * 0.8, 1.30, 0.55), 0.02)
    p.bar(cage, (x, 0.70, 0.55), (x * 0.95, -0.50, 0.60), 0.02)
p.bar(cage, (-0.72, 0.70, HZ), (0.72, 0.70, HZ), 0.022); p.bar(cage, (-0.72, 0.70, HZ), (0.72, 0.70, 0.55), 0.018)
# splitter, dive planes, diffuser, side skirts
p.box(carbon, (-1.02, NOSE - 0.14, 0.04), (1.02, NOSE + 0.35, 0.07))
for sx in (-1, 1): p.box(carbon, (sx * 0.90 - 0.14, NOSE + 0.05, 0.30), (sx * 0.90 + 0.14, NOSE + 0.30, 0.315))
p.box(carbon, (-0.98, TAIL - 0.65, 0.02), (0.98, TAIL + 0.12, 0.10))
for x in (-0.60, -0.20, 0.20, 0.60):
    for k, h in enumerate((0.08, 0.12, 0.16, 0.19, 0.20)):
        y0 = TAIL - 0.62 + k * 0.12; p.box(carbon, (x - 0.01, y0, 0.10), (x + 0.01, y0 + 0.125, 0.10 + h))
for sx in (-1, 1): p.box(carbon, (sx * 1.00 - 0.05, -0.95, 0.06), (sx * 1.00 + 0.05, 0.95, 0.12))
# rear wing
WZ, WY = V["wing_z"], V["wing_y"]
foil(p, carbon, -0.95, 0.95, 0.34, 0.11, 0.06, WY, WZ, angle_deg=-8.0)
for x in (-0.96, 0.96):
    s = p.slot(carbon); pts = [(x, WY - 0.10, WZ - 0.14), (x, WY + 0.44, WZ - 0.14), (x, WY + 0.46, WZ + 0.22), (x, WY + 0.06, WZ + 0.22), (x, WY - 0.10, WZ + 0.10)]
    f = bm.faces.new([bm.verts.new(q) for q in (pts if x > 0 else list(reversed(pts)))]); f.material_index = s
    pts2 = [(x + (0.012 if x > 0 else -0.012), y, z) for (_, y, z) in pts]
    f = bm.faces.new([bm.verts.new(q) for q in (list(reversed(pts2)) if x > 0 else pts2)]); f.material_index = s
if V["wing"] == "pylon":
    for x in (-0.55, 0.55): p.box(carbon, (x - 0.02, WY + 0.02, WZ - 0.32), (x + 0.02, WY + 0.24, WZ + 0.02))
else:
    for x in (-0.45, 0.45): p.bar(carbon, (x, WY - 0.30, WZ - 0.22), (x, WY + 0.05, WZ + 0.16), 0.022); p.bar(carbon, (x, WY + 0.05, WZ + 0.16), (x, WY + 0.22, WZ + 0.05), 0.018)
if V["ducktail"]: p.box(accent, (-0.80, TAIL - 0.35, 0.94), (0.80, TAIL - 0.05, 0.99))
# lights
for sx in (-1, 1):
    x = sx * 0.66; yN = NOSE + 0.02
    if V["lights"] == "round":
        p.cylinder(lamp_h, (x, yN - 0.005, 0.62), 0.14, 0.10, segs=24, axis='Y'); p.cylinder(lamp, (x, yN - 0.008, 0.62), 0.11, -0.012, segs=24, axis='Y')
    elif V["lights"] == "ybar":
        p.box(lamp_h, (x - 0.24, yN - 0.01, 0.48), (x + 0.24, yN + 0.14, 0.60))
        p.box(lamp, (x - 0.22, yN - 0.015, 0.545), (x + 0.22, yN - 0.005, 0.565)); p.box(lamp, (x - 0.03, yN - 0.015, 0.50), (x + 0.03, yN - 0.005, 0.55))
    else:
        p.box(lamp_h, (x - 0.26, yN - 0.01, 0.56), (x + 0.26, yN + 0.16, 0.70))
        p.box(lamp, (x - 0.24, yN - 0.015, 0.60 + sx * 0.0), (x + 0.24, yN - 0.005, 0.66))
    p.box(tail, (sx * 0.55 - 0.28, TAIL - 0.015, 0.66), (sx * 0.55 + 0.28, TAIL + 0.002, 0.72))
if V["grille"]:
    p.box(lamp_h, (-0.55, NOSE - 0.02, 0.28), (0.55, NOSE + 0.06, 0.62))
    for k in range(5): p.box(metal, (-0.53, NOSE - 0.03, 0.31 + k * 0.065), (0.53, NOSE + 0.0, 0.325 + k * 0.065))
if V["louvres"]:
    for k in range(7):
        y0 = 0.62 + k * 0.10; zt = roof_z(y0 + 0.02)
        p.box(carbon, (-0.42, y0, zt - 0.02), (0.42, y0 + 0.04, zt + 0.015))
# mirrors, exhausts, antenna
for sx in (-1, 1):
    if V["mirror"] == "pod":
        p.bar(carbon, (sx * 0.95, -0.55, 0.90), (sx * 1.08, -0.55, 0.95), 0.015); sphere(p, carbon, (sx * 1.12, -0.55, 0.96), 0.07, scale=(0.6, 1.5, 1.0))
    else:
        p.bar(carbon, (sx * 0.80, -0.55, 0.98), (sx * 1.10, -0.50, 1.02), 0.012); p.box(carbon, (sx * 1.10 - 0.04, -0.60, 0.97), (sx * 1.10 + 0.04, -0.42, 1.06))
if V["exhaust"] == "centre":
    for x in (-0.10, 0.10): p.cylinder(metal, (x, TAIL - 0.12, 0.30), 0.05, 0.14, segs=14, axis='Y')
elif V["exhaust"] == "hexquad":
    for x in (-0.32, -0.18, 0.18, 0.32): p.cylinder(metal, (x, TAIL - 0.10, 0.62), 0.045, 0.12, segs=6, axis='Y')
else:
    for sx in (-1, 1):
        for k in range(2): p.cylinder(metal, (sx * 0.98, 0.15 + k * 0.12, 0.22), 0.035, 0.06 * sx, segs=12, axis='X')
p.bar(carbon, (0.0, 0.9, roof_z(0.9) - 0.02), (0.0, 0.9, roof_z(0.9) + 0.22), 0.006)
# panel shut lines: thin dark strips hugging the loft at a station
def panel_line(y, part):
    pts = ring(y); n = len(pts); s = p.slot(lamp_h)
    lo, hi = (3 * SAMP, n // 2 + 1) if part == "upper" else (SAMP + 1, 5 * SAMP + 1)
    idx = list(range(lo, hi))
    idx += [n - i for i in idx if 0 < n - i < n]          # mirror to the other half
    for i in idx:
        j = i + 1 if i < n // 2 + 1 else i - 1
        if j < 0 or j >= n: continue
        a, b_ = Vector(pts[i]), Vector(pts[j])
        for q in (a, b_):
            d = Vector((q.x, 0, q.z - 0.5)).normalized(); q += d * 0.004
        f = bm.faces.new([bm.verts.new(v) for v in ((a.x, y - 0.006, a.z), (b_.x, y - 0.006, b_.z), (b_.x, y + 0.006, b_.z), (a.x, y + 0.006, a.z))])
        f.material_index = s
for (yy, part) in V["panel_lines"]:
    panel_line(yy, part)
def surf_x(y, z):
    """x of the loft surface at (y, z) on the right side."""
    pts = ring(y); best = min(pts[:len(pts) // 2 + 1], key=lambda q: abs(q[2] - z)); return best[0]
def surf_z(y, x):
    pts = ring(y)[:len(ring(y)) // 2 + 1]; best = min(pts, key=lambda q: abs(q[0] - x)); return best[2]
for vent in V["vents"]:
    if vent == "fender":            # louvred vent behind each front wheel
        yv = V["wheels"][0][0] + 0.55
        for sx in (-1, 1):
            xs = surf_x(yv, 0.62)
            p.box(lamp_h, (sx * (xs - 0.005), yv - 0.02, 0.50), (sx * (xs + 0.006), yv + 0.26, 0.74))
            for k in range(4): p.box(carbon, (sx * (xs + 0.006), yv - 0.01, 0.52 + k * 0.055), (sx * (xs + 0.012), yv + 0.25, 0.53 + k * 0.055))
    if vent == "naca_front":        # two NACA ducts on the front lid
        for sx in (-1, 1):
            yv = NOSE + 0.75; zt = surf_z(yv, sx * 0.35)
            p.box(lamp_h, (sx * 0.35 - 0.10, yv, zt - 0.02), (sx * 0.35 + 0.10, yv + 0.32, zt + 0.004))
    if vent == "side_intake":       # big intakes behind the doors, mid-engined
        for sx in (-1, 1):
            xs = surf_x(0.85, 0.65)
            p.box(lamp_h, (sx * (xs - 0.02), 0.72, 0.40), (sx * (xs + 0.008), 1.05, 0.82))
            for k in range(3): p.box(carbon, (sx * (xs + 0.008), 0.73, 0.44 + k * 0.12), (sx * (xs + 0.014), 1.04, 0.46 + k * 0.12))
    if vent == "bonnet_louvres":    # rows of slits down the long bonnet
        for k in range(6):
            yv = NOSE + 0.95 + k * 0.10; zt = surf_z(yv, 0.0)
            p.box(lamp_h, (-0.45, yv, zt - 0.02), (0.45, yv + 0.05, zt + 0.006))
if V["scoop"]:
    sx_, sy_, sr_ = V["scoop"]; zt = roof_z(sy_)
    sphere(p, carbon, (sx_, sy_, zt - 0.02), sr_, scale=(1.0, 2.0, 0.45)); p.box(lamp_h, (sx_ - 0.10, sy_ - 0.30, zt - 0.01), (sx_ + 0.10, sy_ - 0.26, zt + 0.05))
# tow hooks (red loops) and a windscreen wiper
hook = mat("car_towhook", (0.9, 0.1, 0.05), 0.3, 0.5)
for yy in (NOSE + 0.08, TAIL - 0.08):
    zt = surf_z(yy, 0.30) if yy < 0 else surf_z(yy, 0.30)
    p.cylinder(hook, (0.30, yy - 0.06 if yy < 0 else yy + 0.06, zt - 0.10), 0.035, 0.03, segs=10, axis='Y')
wy = g0 + 0.10; wz = roof_z(wy) - 0.16
p.bar(carbon, (DX - 0.10, wy, wz), (DX + 0.35, wy - 0.02, wz + 0.42), 0.012)
# door logos
s = p.slot(logo)
for sx in (-1, 1):
    x = sx * 1.045; y0, y1, z0, z1 = -0.55, 0.65, 0.34, 0.72
    pts = [(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)] if sx > 0 else [(x, y1, z0), (x, y0, z0), (x, y0, z1), (x, y1, z1)]
    f = bm.faces.new([bm.verts.new(q) for q in pts]); f.material_index = s
    for l, t in zip(f.loops, [(0, 0), (1, 0), (1, 1), (0, 1)]): l[p.uv].uv = t
    f.normal_update()
    if (f.normal.x > 0) != (sx > 0): f.normal_flip()
    p.keep.add(f)
parts = p.finish(); parts.data.shade_smooth(); part_objects.append(parts)
save("parts")

# ---------------------------------------------------------------- join
mesh_objs = [body] + part_objects
joined = bmesh.new(); mat_list = []
for o in mesh_objs:
    me = o.data; remap = []
    for m in me.materials:
        if m not in mat_list: mat_list.append(m)
        remap.append(mat_list.index(m))
    tmp = bmesh.new(); tmp.from_mesh(me)
    for f in tmp.faces: f.material_index = remap[f.material_index] if remap else 0
    tmp_me = bpy.data.meshes.new("tmp"); tmp.to_mesh(tmp_me); tmp.free(); joined.from_mesh(tmp_me); bpy.data.meshes.remove(tmp_me)
final = bpy.data.meshes.new(V["stem"]); joined.to_mesh(final); joined.free()
for m in mat_list: final.materials.append(m)
car = bpy.data.objects.new(V["stem"], final); bpy.context.scene.collection.objects.link(car); final.shade_smooth()
for o in mesh_objs: o.hide_render = True; o.hide_viewport = True
save("joined")
# export (run separately when driven over the MCP bridge: the exporter needs a fresh context after reset_scene)
if os.environ.get("APEX_EXPORT", "1") == "1":
    for o in bpy.context.scene.objects: o.select_set(o is car)
    bpy.context.view_layer.objects.active = car
    glb = os.path.join(CAR_DIR, V["stem"] + ".glb")
    bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', use_selection=True, export_apply=True, export_yup=True,
                              export_texcoords=True, export_normals=True, export_materials='EXPORT', export_cameras=False, export_lights=False)
    print("GLB:", glb, os.path.getsize(glb))
