"""ApexSim prop-building helpers for Blender (5.x).

Load inside Blender's Python console or an MCP session:

    import importlib.util, sys
    spec = importlib.util.spec_from_file_location("apex", r"D:\\apexsim\\content\\props\\_tools\\apex_props.py")
    apex = importlib.util.module_from_spec(spec); sys.modules["apex"] = apex; spec.loader.exec_module(apex)

Conventions (see docs/PROPS.md):
  * metres; pivot on the ground at the footprint centre (sky props: hull centre)
  * +X along the track, the road is on -Y in Blender (glTF/Unreal import maps this
    to the importer's +Y "faces road" side), Z up
  * planar UVs at 1 UV unit = 1 m, except faces made with Builder.quad_uv
  * one material slot per surface type, named <kind>_<surface>
  * export: <kind>/<asset>.glb next to the source <kind>/<name>.blend
"""
import bpy, bmesh, math, os
from mathutils import Vector

PROPS_ROOT = r"D:\apexsim\content\props"


def reset_scene():
    bpy.ops.wm.read_homefile(use_empty=True)
    sc = bpy.context.scene
    sc.unit_settings.system = 'METRIC'
    sc.unit_settings.scale_length = 1.0


def material(name, color, metallic=0.0, roughness=0.5, emission=None):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 5.0
    return m


def image_material(name, path, roughness=0.6, masked=False):
    """Base-colour texture material. masked=True wires alpha through a
    Greater-Than node so the glTF exporter writes alphaMode MASK."""
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL'):
            nt.nodes.remove(n)
    bsdf = nt.nodes["Principled BSDF"]
    img = bpy.data.images.load(path, check_existing=True)
    img.pack()
    ti = nt.nodes.new("ShaderNodeTexImage")
    ti.image = img
    nt.links.new(ti.outputs["Color"], bsdf.inputs["Base Color"])
    if masked:
        gt = nt.nodes.new("ShaderNodeMath")
        gt.operation = 'GREATER_THAN'
        gt.inputs[1].default_value = 0.5
        nt.links.new(ti.outputs["Alpha"], gt.inputs[0])
        nt.links.new(gt.outputs[0], bsdf.inputs["Alpha"])
    bsdf.inputs["Roughness"].default_value = roughness
    return m


class Builder:
    """Accumulates geometry into one bmesh with material slots."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.mats = []
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.keep = set()  # faces whose UVs are explicit

    def slot(self, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        return self.mats.index(mat)

    def box(self, mat, lo, hi):
        s = self.slot(mat)
        lo, hi = Vector(lo), Vector(hi)
        v = [self.bm.verts.new((x, y, z)) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
        for q in [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]:
            f = self.bm.faces.new([v[i] for i in q])
            f.material_index = s
        return v

    def cylinder(self, mat, center, radius, height, segs=12, axis='Z', caps=True):
        s = self.slot(mat)
        c = Vector(center)
        rings = []
        for k in (0, 1):
            ring = []
            for i in range(segs):
                a = 2 * math.pi * i / segs
                p = Vector((math.cos(a) * radius, math.sin(a) * radius, k * height))
                if axis == 'X':
                    p = Vector((p.z, p.x, p.y))
                elif axis == 'Y':
                    p = Vector((p.x, p.z, p.y))
                ring.append(self.bm.verts.new(c + p))
            rings.append(ring)
        for i in range(segs):
            j = (i + 1) % segs
            f = self.bm.faces.new((rings[0][i], rings[0][j], rings[1][j], rings[1][i]))
            f.material_index = s
        if caps:
            f = self.bm.faces.new(list(reversed(rings[0]))); f.material_index = s
            f = self.bm.faces.new(rings[1]); f.material_index = s

    def torus(self, mat, center, R, r, segs=12, rings=6):
        s = self.slot(mat)
        cx, cy, cz = center
        grid = []
        for i in range(segs):
            a = 2 * math.pi * i / segs
            row = []
            for j in range(rings):
                t = 2 * math.pi * j / rings
                row.append(self.bm.verts.new((cx + (R + r * math.cos(t)) * math.cos(a),
                                              cy + (R + r * math.cos(t)) * math.sin(a),
                                              cz + r * math.sin(t))))
            grid.append(row)
        for i in range(segs):
            for j in range(rings):
                f = self.bm.faces.new((grid[i][j], grid[(i + 1) % segs][j],
                                       grid[(i + 1) % segs][(j + 1) % rings], grid[i][(j + 1) % rings]))
                f.material_index = s

    def extrude_profile(self, mat, profile_yz, x0, x1, closed=False, flip=False):
        """Sweep a 2D (y, z) polyline along X from x0 to x1."""
        s = self.slot(mat)
        a = [self.bm.verts.new((x0, y, z)) for (y, z) in profile_yz]
        b = [self.bm.verts.new((x1, y, z)) for (y, z) in profile_yz]
        n = len(profile_yz)
        for i in range(n if closed else n - 1):
            j = (i + 1) % n
            q = (a[i], b[i], b[j], a[j]) if not flip else (a[i], a[j], b[j], b[i])
            f = self.bm.faces.new(q)
            f.material_index = s
        return a, b

    def sheet(self, mat, p0, p1, p2, p3):
        s = self.slot(mat)
        f = self.bm.faces.new([self.bm.verts.new(Vector(p)) for p in (p0, p1, p2, p3)])
        f.material_index = s
        return f

    def quad_uv(self, mat, pts, uvs):
        """A face with explicit UVs (skipped by the planar pass)."""
        s = self.slot(mat)
        f = self.bm.faces.new([self.bm.verts.new(Vector(p)) for p in pts])
        f.material_index = s
        for l, uv in zip(f.loops, uvs):
            l[self.uv].uv = uv
        self.keep.add(f)
        return f

    def front_quad(self, mat, x0, x1, z0, z1, y, repeats=1):
        """Textured quad facing -Y (the road), UV 0..repeats along X."""
        return self.quad_uv(mat, [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)],
                            [(0, 0), (repeats, 0), (repeats, 1), (0, 1)])

    def finish(self, planar_uv=True, uv_scale=1.0, recalc=True):
        bm, keep = self.bm, self.keep
        bmesh.ops.remove_doubles(bm, verts=[v for v in bm.verts if not any(f in keep for f in v.link_faces)], dist=1e-5)
        if recalc:
            bmesh.ops.recalc_face_normals(bm, faces=[f for f in bm.faces if f not in keep])
        if planar_uv:
            for f in bm.faces:
                if f in keep:
                    continue
                n = f.normal
                ax = max(range(3), key=lambda i: abs(n[i]))
                for l in f.loops:
                    p = l.vert.co
                    uv = (p.y, p.z) if ax == 0 else (p.x, p.z) if ax == 1 else (p.x, p.y)
                    l[self.uv].uv = (uv[0] * uv_scale, uv[1] * uv_scale)
        me = bpy.data.meshes.new(self.name)
        bm.to_mesh(me)
        bm.free()
        for m in self.mats:
            me.materials.append(m)
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        me.shade_smooth()
        with bpy.context.temp_override(object=ob, selected_editable_objects=[ob]):
            try:
                bpy.ops.object.shade_auto_smooth(angle=math.radians(35))
            except Exception:
                pass
        return ob


def export_one(kind, asset):
    """Export the object named `asset` to <PROPS_ROOT>/<kind>/<asset>.glb at the origin."""
    ob = bpy.data.objects[asset]
    loc = ob.location.copy()
    ob.location = (0, 0, 0)
    d = os.path.join(PROPS_ROOT, kind)
    os.makedirs(d, exist_ok=True)
    for o in bpy.context.scene.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    glb = os.path.join(d, asset + ".glb")
    bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', use_selection=True, export_apply=True,
                              export_yup=True, export_texcoords=True, export_normals=True,
                              export_materials='EXPORT', export_cameras=False, export_lights=False)
    ob.location = loc
    return glb, os.path.getsize(glb)


def save_blend(kind, name):
    d = os.path.join(PROPS_ROOT, kind)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name + ".blend")
    bpy.ops.wm.save_as_mainfile(filepath=p)
    return p


def stats():
    out = {}
    for o in bpy.context.scene.objects:
        if o.type == 'MESH':
            d = o.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
            out[o.name] = {"verts": len(d.vertices), "tris": sum(len(p.vertices) - 2 for p in d.polygons),
                           "mats": [m.name for m in o.data.materials],
                           "bbox_min": [round(min(v.co[i] for v in o.data.vertices), 3) for i in range(3)],
                           "bbox_max": [round(max(v.co[i] for v in o.data.vertices), 3) for i in range(3)]}
    return out


def preview_ground():
    if "PreviewGround" not in bpy.data.objects:
        g = Builder("PreviewGround")
        g.box(material("preview_ground", (0.25, 0.32, 0.18), roughness=0.9), (-40, -40, -0.05), (40, 40, 0))
        g.finish()


def preview(path, look_from=(6, -7, 3), look_at=(0, 0, 0.6), res=(1280, 720), fov=45, ortho=None):
    sc = bpy.context.scene
    cam = bpy.data.objects.get("PreviewCam")
    if not cam:
        cd = bpy.data.cameras.new("PreviewCam")
        cam = bpy.data.objects.new("PreviewCam", cd)
        sc.collection.objects.link(cam)
    cam.location = look_from
    cam.rotation_euler = (Vector(look_at) - Vector(look_from)).to_track_quat('-Z', 'Y').to_euler()
    cam.data.angle = math.radians(fov)
    cam.data.type = 'ORTHO' if ortho else 'PERSP'
    if ortho:
        cam.data.ortho_scale = ortho
    sc.camera = cam
    sun = bpy.data.objects.get("PreviewSun")
    if not sun:
        ld = bpy.data.lights.new("PreviewSun", 'SUN')
        sun = bpy.data.objects.new("PreviewSun", ld)
        sc.collection.objects.link(sun)
        ld.energy = 4.0
        sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(-40))
    w = sc.world or bpy.data.worlds.new("World")
    sc.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (0.55, 0.65, 0.8, 1)
        bg.inputs[1].default_value = 1.0
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.filepath = path
    sc.render.image_settings.file_format = 'PNG'
    bpy.ops.render.render(write_still=True)
    return path
