"""Build the Yotota LMP2 car model in Blender and export it to
content/cars/yotota-lmp2/yotota_lmp2.glb.

Run inside Blender (5.x) with the apex helper on the path:

    exec(open(r"D:\\apexsim\\content\\cars\\yotota-lmp2\\build_yotota.py").read())

Frame: nose on -Y, ground at z=0, metres. That matches the other cars in
content/cars (nose on glTF +Z after export), which the Unreal car actor yaws
by -90 so the nose lands on world +X.

Stages: body loft (Catmull-Rom sections, subdivided) -> wheel arches
(boolean) -> parts (wheels, cockpit, wing, aero, lights, decals) -> join ->
export. The .blend is saved after each stage.
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

CAR_DIR = r"D:\apexsim\content\cars\yotota-lmp2"
TOOLS = r"D:\apexsim\content\props\_tools\apex_props.py"
spec = importlib.util.spec_from_file_location("apex", TOOLS)
apex = importlib.util.module_from_spec(spec); sys.modules["apex"] = apex; spec.loader.exec_module(apex)
from apex import Builder, material, reset_scene

def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, "yotota_lmp2.blend"))
    print("saved", tag)

def mat(name, color, metallic=0.0, roughness=0.5, coat=0.0, alpha=1.0, emission=None):
    m = material(name, color, metallic, roughness, emission)
    b = m.node_tree.nodes["Principled BSDF"]
    if coat:
        b.inputs["Coat Weight"].default_value = coat
    if alpha < 1.0:
        b.inputs["Alpha"].default_value = alpha
        try:
            m.surface_render_method = 'BLENDED'
        except Exception:
            pass
    return m

# ---------------------------------------------------------------- materials
reset_scene()
paint = mat("yotota_paint", (0.93, 0.93, 0.92), 0.05, 0.28, coat=1.0)
carbon = mat("yotota_carbon", (0.05, 0.05, 0.06), 0.3, 0.32, coat=0.6)
glass = mat("yotota_glass", (0.02, 0.03, 0.04), 0.0, 0.05, alpha=0.55)
liner = mat("yotota_liner", (0.03, 0.03, 0.03), 0.0, 0.9)
interior = mat("yotota_interior", (0.12, 0.12, 0.13), 0.1, 0.8)
seat_m = mat("yotota_seat", (0.08, 0.08, 0.30), 0.0, 0.85)
alc = mat("yotota_wheel_rim", (0.06, 0.06, 0.06), 0.0, 0.7)
metal = mat("yotota_metal", (0.6, 0.6, 0.62), 1.0, 0.35)
tyre = mat("yotota_tyre", (0.025, 0.025, 0.025), 0.0, 0.85)
rim = mat("yotota_rim", (0.12, 0.12, 0.13), 1.0, 0.35)
disc = mat("yotota_brake", (0.35, 0.33, 0.30), 1.0, 0.6)
caliper = mat("yotota_caliper", (0.85, 0.10, 0.05), 0.2, 0.4)
lamp = mat("yotota_headlight", (0.95, 0.95, 0.9), 0.0, 0.1, emission=(1.0, 0.98, 0.9))
lamp_h = mat("yotota_lamp_housing", (0.02, 0.02, 0.02), 0.6, 0.3)
tail = mat("yotota_taillight", (0.5, 0.03, 0.02), 0.0, 0.15, emission=(1.0, 0.08, 0.04))
display = mat("yotota_display", (0.02, 0.02, 0.03), 0.0, 0.2, emission=(0.1, 0.4, 0.2))
logo = apex.image_material("yotota_logo", os.path.join(CAR_DIR, "textures", "yotota_logo.png"), roughness=0.3, masked=True)

# ---------------------------------------------------------------- body loft
def catmull(pts, n):
    P = [Vector(p) for p in pts]; P = [P[0]] + P + [P[-1]]; out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(n):
            t = k / n
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(P[-2])
    return out

# Right-half section control points (x, z), bottom centre -> roof centre:
# floor centre, floor edge, lower side, fender top outer, fender top inner, valley, canopy side, canopy upper, roof.
KEYS = [
    (-2.45, [(0, .12), (.50, .12), (.56, .16), (.56, .24), (.44, .29), (.30, .30), (.18, .31), (.08, .32), (0, .32)]),
    (-2.25, [(0, .08), (.78, .08), (.86, .20), (.86, .40), (.70, .46), (.50, .42), (.30, .40), (.14, .41), (0, .41)]),
    (-1.85, [(0, .06), (.92, .06), (.97, .40), (.96, .80), (.78, .88), (.58, .76), (.36, .62), (.16, .58), (0, .57)]),
    (-1.50, [(0, .06), (.94, .06), (.98, .45), (.97, .86), (.78, .92), (.58, .80), (.36, .66), (.16, .62), (0, .61)]),
    (-1.10, [(0, .06), (.95, .06), (.98, .42), (.96, .76), (.78, .82), (.60, .72), (.38, .70), (.18, .72), (0, .72)]),
    (-0.60, [(0, .06), (.95, .06), (.97, .40), (.95, .64), (.78, .68), (.62, .66), (.42, .84), (.24, .96), (0, .99)]),
    (-0.10, [(0, .06), (.95, .06), (.97, .40), (.95, .63), (.78, .66), (.63, .65), (.44, .96), (.26, 1.05), (0, 1.07)]),
    ( 0.45, [(0, .06), (.95, .06), (.97, .40), (.95, .64), (.78, .68), (.64, .68), (.44, .95), (.26, 1.04), (0, 1.06)]),
    ( 1.00, [(0, .06), (.95, .06), (.98, .42), (.96, .78), (.78, .84), (.62, .78), (.40, .88), (.20, .94), (0, .96)]),
    ( 1.50, [(0, .06), (.94, .06), (.98, .45), (.97, .88), (.78, .93), (.60, .84), (.36, .84), (.18, .86), (0, .87)]),
    ( 1.90, [(0, .08), (.93, .08), (.97, .42), (.95, .82), (.76, .86), (.58, .80), (.34, .78), (.16, .78), (0, .79)]),
    ( 2.30, [(0, .22), (.84, .22), (.90, .45), (.88, .70), (.70, .74), (.52, .72), (.30, .70), (.14, .70), (0, .70)]),
]
NPC = len(KEYS[0][1]); NY = 90; SAMP = 4; key_ys = [k[0] for k in KEYS]
cp_curves = [catmull([(k[0], k[1][j][0], k[1][j][1]) for k in KEYS], 12) for j in range(NPC)]

def ctrl_at(y):
    out = []
    for c in cp_curves:
        for i in range(len(c) - 1):
            if c[i].x <= y <= c[i + 1].x:
                t = (y - c[i].x) / max(c[i + 1].x - c[i].x, 1e-6)
                v = c[i].lerp(c[i + 1], t); out.append((v.y, v.z)); break
        else:
            out.append((c[-1].y, c[-1].z) if y > c[-1].x else (c[0].y, c[0].z))
    return out

def ring(y):
    right = [(max(p.x, 0.0), p.y) for p in catmull(ctrl_at(y), SAMP)]
    right[0] = (0.0, right[0][1]); right[-1] = (0.0, right[-1][1])
    return [(x, y, z) for (x, z) in right + [(-x, z) for (x, z) in reversed(right[1:-1])]]

b = Builder("yotota_body"); bm = b.bm
ys = [key_ys[0] + (key_ys[-1] - key_ys[0]) * i / NY for i in range(NY + 1)]
rings = [[bm.verts.new(p) for p in ring(y)] for y in ys]
M = len(rings[0]); half = (M + 1) // 2
sp, sg = b.slot(paint), b.slot(glass); CAN = 6 * SAMP - 1
for i in range(NY):
    y = (ys[i] + ys[i + 1]) / 2
    for k in range(M):
        k2 = (k + 1) % M
        f = bm.faces.new((rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]))
        kk = k if k < half else M - k
        f.material_index = sg if (kk >= CAN and -0.95 < y < 0.75) else sp
        for l in f.loops:
            l[b.uv].uv = (l.vert.co.y, l.vert.co.x + l.vert.co.z)
        b.keep.add(f)
for i, order in ((0, -1), (NY, 1)):
    f = bm.faces.new(rings[i] if order > 0 else list(reversed(rings[i]))); f.material_index = sp
    for l in f.loops:
        l[b.uv].uv = (l.vert.co.x, l.vert.co.z)
    b.keep.add(f)
body = b.finish()
sub = body.modifiers.new("smooth", 'SUBSURF'); sub.levels = 1
with bpy.context.temp_override(object=body):
    bpy.ops.object.modifier_apply(modifier="smooth")
body.data.shade_smooth()
save("body")

# ---------------------------------------------------------------- wheel arches
def flip(ob):
    bm2 = bmesh.new(); bm2.from_mesh(ob.data); bmesh.ops.reverse_faces(bm2, faces=bm2.faces); bm2.to_mesh(ob.data); bm2.free()

part_objects = []
for (y, r) in ((-1.50, 0.43), (1.50, 0.44)):
    for sx in (-1, 1):
        cut = Builder("cut"); cut.cylinder(paint, (0.50 if sx > 0 else -1.20, y, r - 0.05), r, 0.70, segs=40, axis='X'); c = cut.finish()
        mod = body.modifiers.new("arch", 'BOOLEAN'); mod.operation = 'DIFFERENCE'; mod.object = c; mod.solver = 'EXACT'
        with bpy.context.temp_override(object=body):
            bpy.ops.object.modifier_apply(modifier=mod.name)
        bpy.data.objects.remove(c)
        x0 = 0.50 if sx > 0 else -0.985
        ln = Builder("liner"); ln.cylinder(liner, (x0, y, r - 0.05), r - 0.003, 0.485, segs=40, axis='X', caps=False); l_ob = ln.finish(); flip(l_ob)
        cap = Builder("linercap"); cap.cylinder(liner, (0.50 if sx > 0 else -0.50, y, r - 0.05), r - 0.003, 0.01 * sx, segs=40, axis='X'); c_ob = cap.finish()
        part_objects += [l_ob, c_ob]
save("arches")

# ---------------------------------------------------------------- parts
def sphere(b, m, center, r, seg=16, ring_=10, scale=(1, 1, 1)):
    s = b.slot(m); g = bmesh.ops.create_uvsphere(b.bm, u_segments=seg, v_segments=ring_, radius=r)
    vs = set(g['verts'])
    for v in vs:
        v.co = Vector((v.co.x * scale[0], v.co.y * scale[1], v.co.z * scale[2])) + Vector(center)
    for f in b.bm.faces:
        if f.verts[0] in vs:
            f.material_index = s

def torus(b, m, center, R, r, segs=32, rings_=12):
    """Torus with its axis along X."""
    s = b.slot(m); cx, cy, cz = center; grid = []
    for i in range(segs):
        a = 2 * math.pi * i / segs; row = []
        for j in range(rings_):
            t = 2 * math.pi * j / rings_
            row.append(b.bm.verts.new((cx + r * math.sin(t), cy + (R + r * math.cos(t)) * math.cos(a), cz + (R + r * math.cos(t)) * math.sin(a))))
        grid.append(row)
    for i in range(segs):
        for j in range(rings_):
            f = b.bm.faces.new((grid[i][j], grid[(i + 1) % segs][j], grid[(i + 1) % segs][(j + 1) % rings_], grid[i][(j + 1) % rings_]))
            f.material_index = s

def foil(b, m, x0, x1, chord, thick, camber, ly, lz, n=14, angle_deg=-8.0):
    """Cambered NACA-ish airfoil lofted along X; leading edge at (ly, lz), pitched by angle."""
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
    f = b.bm.faces.new(A); f.material_index = s
    f = b.bm.faces.new(list(reversed(B))); f.material_index = s

p = Builder("yotota_parts"); bm = p.bm
# wheels: not part of the body. The client draws the shared class wheel
# (content/wheels) at the [wheels] positions in car.toml, which are
# where the tyres used to be built here (x = +-0.78, hub at the tyre radius).
# cockpit
p.box(interior, (-0.62, -0.75, 0.10), (0.62, 0.95, 0.14))
p.box(interior, (-0.62, -0.75, 0.14), (-0.55, 0.95, 0.60)); p.box(interior, (0.55, -0.75, 0.14), (0.62, 0.95, 0.60))
p.box(seat_m, (-0.26, 0.20, 0.14), (0.26, 0.62, 0.30)); p.box(seat_m, (-0.28, 0.50, 0.30), (0.28, 0.66, 0.95))
p.box(seat_m, (-0.32, 0.18, 0.30), (-0.24, 0.62, 0.42)); p.box(seat_m, (0.24, 0.18, 0.30), (0.32, 0.62, 0.42))
p.box(interior, (-0.45, -0.55, 0.55), (0.45, -0.25, 0.80))
p.box(display, (-0.12, -0.26, 0.62), (0.12, -0.25, 0.74))
p.bar(metal, (0, -0.30, 0.68), (0, 0.02, 0.72), 0.02)
wheel_ring = [Vector((0.15 * math.cos(2 * math.pi * i / 24), 0.02, 0.72 + 0.15 * math.sin(2 * math.pi * i / 24))) for i in range(24)]
for i in range(24):
    p.bar(alc, wheel_ring[i], wheel_ring[(i + 1) % 24], 0.018, segs=6)
p.box(alc, (-0.14, 0.01, 0.70), (0.14, 0.035, 0.74)); p.box(alc, (-0.03, 0.01, 0.58), (0.03, 0.035, 0.72))
p.bar(metal, (-0.52, 0.62, 0.20), (-0.52, 0.62, 0.90), 0.02); p.bar(metal, (0.52, 0.62, 0.20), (0.52, 0.62, 0.90), 0.02)
p.bar(metal, (-0.52, 0.62, 0.90), (0.52, 0.62, 0.90), 0.02)
p.bar(metal, (-0.38, 0.62, 0.90), (-0.38, 1.25, 0.70), 0.02); p.bar(metal, (0.38, 0.62, 0.90), (0.38, 1.25, 0.70), 0.02)
for x in (-0.12, 0.0, 0.12):
    p.box(metal, (x - 0.03, -0.72, 0.16), (x + 0.03, -0.68, 0.30))
# rear wing, flap, endplates, swan necks, fin, scoop
foil(p, carbon, -0.94, 0.94, 0.36, 0.11, 0.06, 1.92, 1.05, angle_deg=-9.0)
foil(p, carbon, -0.94, 0.94, 0.14, 0.10, 0.05, 2.22, 1.13, angle_deg=-24.0)
for x in (-0.95, 0.95):
    s = p.slot(carbon); pts = [(x, 1.82, 0.86), (x, 2.38, 0.86), (x, 2.40, 1.32), (x, 1.98, 1.32), (x, 1.82, 1.15)]
    f = bm.faces.new([bm.verts.new(q) for q in (pts if x > 0 else list(reversed(pts)))]); f.material_index = s
    pts2 = [(x + (0.012 if x > 0 else -0.012), y, z) for (_, y, z) in pts]
    f = bm.faces.new([bm.verts.new(q) for q in (list(reversed(pts2)) if x > 0 else pts2)]); f.material_index = s
for x in (-0.44, 0.44):
    p.bar(carbon, (x, 1.60, 0.80), (x, 1.95, 1.25), 0.025); p.bar(carbon, (x, 1.95, 1.25), (x, 2.08, 1.10), 0.02)
s = p.slot(carbon); fin = [(0.62, 1.00), (0.95, 1.16), (2.05, 1.16), (2.05, 0.82), (0.62, 0.92)]
for sx, order in ((-0.012, 1), (0.012, -1)):
    f = bm.faces.new([bm.verts.new((sx, y, z)) for (y, z) in (fin if order > 0 else list(reversed(fin)))]); f.material_index = s
sphere(p, carbon, (0, 0.28, 1.06), 0.17, scale=(1.0, 1.9, 0.5))
p.box(lamp_h, (-0.13, -0.02, 1.05), (0.13, 0.02, 1.12))
# splitter, dive planes, diffuser + strakes
p.box(carbon, (-1.00, -2.62, 0.035), (1.00, -2.20, 0.065))
for sx in (-1, 1):
    p.box(carbon, (sx * 0.86 - 0.16, -2.35, 0.20), (sx * 0.86 + 0.16, -2.10, 0.215))
p.box(carbon, (-0.96, 1.70, 0.02), (0.96, 2.46, 0.09))
for x in (-0.66, -0.22, 0.22, 0.66):
    for k, h in enumerate((0.10, 0.14, 0.17, 0.19, 0.20, 0.20)):
        y0 = 1.72 + k * 0.12; p.box(carbon, (x - 0.01, y0, 0.09), (x + 0.01, y0 + 0.125, 0.09 + h))
# lights, intakes, mirrors, exhausts, antenna
for sx in (-1, 1):
    p.box(lamp_h, (sx * 0.64 - 0.22, -2.38, 0.30), (sx * 0.64 + 0.22, -2.10, 0.46))
    for k in range(3):
        p.cylinder(lamp, (sx * 0.64 - 0.13 + k * 0.13, -2.385, 0.38), 0.045, -0.01, segs=16, axis='Y')
    p.box(tail, (sx * 0.56 - 0.24, 2.325, 0.50), (sx * 0.56 + 0.24, 2.34, 0.54))
    p.box(lamp_h, (sx * 0.96 - 0.02, -0.95, 0.20), (sx * 0.96 + 0.02, -0.30, 0.52))
    p.bar(carbon, (sx * 0.93, -0.62, 0.72), (sx * 1.01, -0.60, 0.80), 0.015)
    sphere(p, carbon, (sx * 1.03, -0.60, 0.81), 0.06, scale=(0.5, 1.4, 1.0))
    p.cylinder(metal, (sx * 0.30, 2.30, 0.28), 0.04, 0.12, segs=12, axis='Y')
p.bar(carbon, (0.2, 0.9, 0.96), (0.2, 0.9, 1.20), 0.006)
# wordmark decals, reading forward on both flanks
s = p.slot(logo)
for sx in (-1, 1):
    x = sx * 0.985; y0, y1, z0, z1 = 0.62, 1.95, 0.42, 0.86
    pts = [(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)] if sx > 0 else [(x, y1, z0), (x, y0, z0), (x, y0, z1), (x, y1, z1)]
    f = bm.faces.new([bm.verts.new(q) for q in pts]); f.material_index = s
    for l, t in zip(f.loops, [(0, 0), (1, 0), (1, 1), (0, 1)]):
        l[p.uv].uv = t
    f.normal_update()
    if (f.normal.x > 0) != (sx > 0):
        f.normal_flip()
    p.keep.add(f)
parts = p.finish(); parts.data.shade_smooth()
part_objects.append(parts)
save("parts")

# ---------------------------------------------------------------- join + export
mesh_objs = [body] + part_objects
joined = bmesh.new(); mat_list = []
for o in mesh_objs:
    me = o.data
    remap = []
    for m in me.materials:
        if m not in mat_list:
            mat_list.append(m)
        remap.append(mat_list.index(m))
    tmp = bmesh.new(); tmp.from_mesh(me)
    for f in tmp.faces:
        f.material_index = remap[f.material_index] if remap else 0
    tmp_me = bpy.data.meshes.new("tmp"); tmp.to_mesh(tmp_me); tmp.free()
    joined.from_mesh(tmp_me); bpy.data.meshes.remove(tmp_me)
final = bpy.data.meshes.new("yotota_lmp2"); joined.to_mesh(final); joined.free()
for m in mat_list:
    final.materials.append(m)
car = bpy.data.objects.new("yotota_lmp2", final); bpy.context.scene.collection.objects.link(car)
final.shade_smooth()
for o in mesh_objs:
    o.hide_render = True; o.hide_viewport = True
save("joined")

for o in bpy.context.scene.objects:
    o.select_set(o is car)
bpy.context.view_layer.objects.active = car
glb = os.path.join(CAR_DIR, "yotota_lmp2.glb")
bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', use_selection=True, export_apply=True, export_yup=True,
                          export_texcoords=True, export_normals=True, export_materials='EXPORT',
                          export_cameras=False, export_lights=False)
save("exported")
print("GLB:", glb, os.path.getsize(glb))
