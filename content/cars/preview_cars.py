"""Render a consistent preview sheet for the generated cars.

Run inside Blender; set CARS (folder names) and optionally VIEWS first:

    CARS = ["posh-gt3rs"]
    exec(open(r"D:\\apexsim\\content\\cars\\preview_cars.py").read())

Loads each car's GLB plus the shared class wheel from content/wheels, places
the four wheels where car.toml's [wheels] table says (blender y = -axle_m,
x = +-track/2, z = radius), and renders to
content/props/_preview/cars/<folder>_<view>.png.

Views: "side" and "front" and "top" are orthographic (shape reading, no
perspective lies), "hero" and "rear" are 50 mm three-quarters. Scratch scene
only; nothing beside the PNGs is written.
"""
import bpy, math, os, sys
from mathutils import Vector

ROOT = r"D:\apexsim\content"
OUT = os.path.join(ROOT, "props", "_preview", "cars")
try:
    CARS
except NameError:
    CARS = ["posh-gt3rs", "limbotiti-caravan-gt3", "murcetes-amd-gt3",
            "yotota-lmp2", "posh-lmp2", "fugazzi-lmp2", "jeanetti-lmp2"]
try:
    VIEWS
except NameError:
    VIEWS = ["side", "front", "hero", "rear"]
try:
    RES
except NameError:
    RES = (1600, 900)
try:
    LIVERY          # 0: the model as authored; N: the car.toml's N-th [[livery]]
except NameError:
    LIVERY = 0

if sys.version_info >= (3, 11):
    import tomllib
else:
    import tomli as tomllib


def wipe():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for coll in list(bpy.data.collections):
        bpy.data.collections.remove(coll)


def studio():
    """Neutral grey studio: sky-ish world, key sun, soft fill, matte ground."""
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x, sc.render.resolution_y = RES
    sc.render.film_transparent = False
    sc.view_settings.view_transform = 'AgX'
    ee = sc.eevee
    for attr, val in (("use_gtao", True), ("use_shadows", True), ("use_raytracing", True),
                      ("taa_render_samples", 64), ("use_bloom", False)):
        try:
            setattr(ee, attr, val)
        except Exception:
            pass

    w = bpy.data.worlds.get("preview_world") or bpy.data.worlds.new("preview_world")
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    bg = nt.nodes.new("ShaderNodeBackground")
    sky = nt.nodes.new("ShaderNodeTexSky")
    for st in ('MULTIPLE_SCATTERING', 'NISHITA', 'SINGLE_SCATTERING'):
        try:
            sky.sky_type = st
            break
        except TypeError:
            continue
    for attr, val in (("sun_elevation", math.radians(28)), ("sun_rotation", math.radians(150))):
        try:
            setattr(sky, attr, val)
        except Exception:
            pass
    bg.inputs["Strength"].default_value = 0.30
    nt.links.new(sky.outputs[0], bg.inputs["Color"])
    nt.links.new(bg.outputs[0], nt.nodes.new("ShaderNodeOutputWorld").inputs["Surface"])
    bpy.context.scene.world = w

    key = bpy.data.objects.new("key", bpy.data.lights.new("keyL", 'SUN'))
    key.data.energy = 3.0
    key.data.angle = math.radians(2.5)
    key.rotation_euler = (math.radians(58), 0, math.radians(38))
    bpy.context.scene.collection.objects.link(key)

    fill = bpy.data.objects.new("fill", bpy.data.lights.new("fillL", 'AREA'))
    fill.data.energy = 900
    fill.data.size = 9.0
    fill.location = (-7, -5, 4.5)
    fill.rotation_euler = (math.radians(62), 0, math.radians(-40))
    bpy.context.scene.collection.objects.link(fill)

    m = bpy.data.materials.new("preview_ground")
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (0.13, 0.13, 0.14, 1)
    b.inputs["Roughness"].default_value = 0.55
    bpy.ops.mesh.primitive_plane_add(size=120, location=(0, 0, 0))
    bpy.context.object.data.materials.append(m)
    bpy.context.object.name = "ground"


def dusk():
    """Night-ish lighting for the lamp views: the emissives carry the shot."""
    for o in bpy.context.scene.objects:
        if o.type == 'LIGHT':
            o.data.energy *= 0.06
    w = bpy.context.scene.world
    if w and w.node_tree:
        for n in w.node_tree.nodes:
            if n.type == 'BACKGROUND':
                n.inputs["Strength"].default_value = 0.05


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


def bounds(objs):
    """World-space box from the vertices themselves. `bound_box` on a freshly
    imported object is not reliable until the depsgraph has run, and an eye
    derived from a stale box sat 10 cm low."""
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        if o.type != 'MESH':
            continue
        mw = o.matrix_world
        for v in o.data.vertices:
            p = mw @ v.co
            lo = Vector((min(lo[i], p[i]) for i in range(3)))
            hi = Vector((max(hi[i], p[i]) for i in range(3)))
    return lo, hi


def add_wheels(cfg):
    """Four wheels from content/wheels/<model>.glb at the car.toml positions."""
    w = cfg.get("wheels")
    if not w:
        return []
    src = os.path.join(ROOT, "wheels", w.get("model", "gt3") + ".glb")
    if not os.path.exists(src):
        print("no wheel model", src)
        return []
    out = []
    for (ax_key, tr_key, r_key, wd_key, sign) in (
            ("front_axle_m", "front_track_m", "front_radius_m", "front_width_m", -1),
            ("rear_axle_m", "rear_track_m", "rear_radius_m", "rear_width_m", -1)):
        axle = float(w[ax_key]); track = float(w[tr_key])
        R = float(w[r_key]); W = float(w[wd_key])
        for sx in (-1, 1):
            objs = import_glb(src)
            for o in objs:
                o.select_set(False)
            lo, hi = bounds(objs)
            # wheel frame: axle on X, radius in Y/Z
            src_w = max(hi.x - lo.x, 1e-6)
            src_r = max((hi.y - lo.y), (hi.z - lo.z)) / 2.0
            s = (W / src_w, R / src_r, R / src_r)
            for o in objs:
                if o.parent:
                    continue
                o.scale = (o.scale.x * s[0] * sx, o.scale.y * s[1], o.scale.z * s[2])
                o.location = (sx * track / 2.0, sign * axle, R)
            out += objs
    return out


OPEN_WHEEL = False   # set per car from its class: F1 cars get the open-wheel eye


def apply_livery(objs, car_dir, livery):
    """What the client does with a [[livery]] (ApexCarLivery.cpp): the paint
    and accent base colours, the paint's metallic, the logo image."""
    for o in objs:
        if o.type != 'MESH':
            continue
        for m in o.data.materials:
            if not m or not m.use_nodes:
                continue
            base = m.name.split(".")[0]
            bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
            if base == "car_paint" and bsdf:
                bsdf.inputs["Base Color"].default_value = (*livery["paint"], 1.0)
                if livery.get("metallic", -1) >= 0:
                    bsdf.inputs["Metallic"].default_value = livery["metallic"]
            elif base == "car_accent" and bsdf and "accent" in livery:
                bsdf.inputs["Base Color"].default_value = (*livery["accent"], 1.0)
            elif base == "car_logo" and livery.get("logo"):
                img = bpy.data.images.load(os.path.join(car_dir, livery["logo"]), check_existing=True)
                for n in m.node_tree.nodes:
                    if n.type == 'TEX_IMAGE':
                        n.image = img


def eye_from_box(lo, hi):
    """The driver's eye the client derives from the mesh box: 70% of the
    height, 5% of the length behind centre, 18% of the width to the left
    (ApexCockpit::DeriveLayout, closed style - docs/CAR_MODELS.md). An
    open-wheeler's is on the centreline, 8% behind centre, 82% up."""
    c = (lo + hi) / 2.0
    if OPEN_WHEEL:
        return Vector((0.0, c.y + 0.08 * (hi.y - lo.y), lo.z + 0.82 * (hi.z - lo.z)))
    return Vector((0.18 * (hi.x - lo.x),
                   c.y + 0.05 * (hi.y - lo.y),
                   lo.z + 0.70 * (hi.z - lo.z)))


def frame(view, lo, hi):
    c = (lo + hi) / 2.0
    L, Wd, H = hi.y - lo.y, hi.x - lo.x, hi.z - lo.z
    cam_d = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_d)
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam
    if view in ("side", "front", "top"):
        cam_d.type = 'ORTHO'
    if view == "side":
        cam_d.ortho_scale = L * 1.12
        cam.location = (14, c.y, c.z)
        cam.rotation_euler = (math.radians(90), 0, math.radians(90))
    elif view == "front":
        cam_d.ortho_scale = Wd * 1.35
        cam.location = (c.x, lo.y - 14, c.z + 0.05)
        cam.rotation_euler = (math.radians(90), 0, 0)
    elif view == "top":
        cam_d.ortho_scale = L * 1.12
        cam.location = (c.x, c.y, 14)
        cam.rotation_euler = (0, 0, 0)
    elif view == "cockpit":
        cam_d.lens = 32
        e = eye_from_box(lo, hi)
        cam.location = e
        _aim(cam, Vector((e.x, lo.y - 6.0, e.z - 0.55)))
    elif view == "mirror":
        cam_d.lens = 34
        e = eye_from_box(lo, hi)
        cam.location = Vector((0.0, e.y - 0.45, e.z + 0.12))
        _aim(cam, Vector((0.0, hi.y + 6.0, e.z - 0.10)))
    elif view == "lamps_front":
        # dusk, close and low on the nose corner: the lamps and what is in them
        cam_d.lens = 70
        cam.location = (c.x + 3.1, lo.y - 2.5, lo.z + 0.95)
        _aim(cam, Vector((c.x + 0.55, lo.y + 0.35, lo.z + 0.60)))
    elif view == "lamps_rear":
        cam_d.lens = 70
        cam.location = (c.x - 2.6, hi.y + 3.0, lo.z + 1.05)
        _aim(cam, Vector((c.x - 0.25, hi.y - 0.3, lo.z + 0.72)))
    elif view == "rear":
        cam_d.lens = 65
        cam.location = (c.x - 4.2, hi.y + 5.4, c.z + 1.9)
        _aim(cam, c)
    else:  # hero: front three-quarter, low
        cam_d.lens = 55
        cam.location = (c.x + 4.6, lo.y - 5.2, c.z + 1.15)
        _aim(cam, Vector((c.x, c.y - 0.3, c.z + 0.05)))
    return cam


def _aim(cam, target):
    d = (target - cam.location)
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()


for folder in CARS:
    car_dir = os.path.join(ROOT, "cars", folder)
    with open(os.path.join(car_dir, "car.toml"), "rb") as f:
        cfg = tomllib.load(f)
    OPEN_WHEEL = cfg.get("class", "").strip().upper() in ("F1", "FORMULA", "OPEN", "INDY")
    glb = os.path.join(car_dir, cfg["model"])
    for view in VIEWS:
        wipe()
        studio()
        if view.startswith("lamps"):
            dusk()
        body = import_glb(glb)
        if LIVERY and len(cfg.get("livery", [])) >= LIVERY:
            apply_livery(body, car_dir, cfg["livery"][LIVERY - 1])
        wheels = add_wheels(cfg)
        # the client derives the eye from the body mesh's own box (no wheels)
        lo, hi = bounds(body) if view in ("cockpit", "mirror") else bounds(body + wheels)
        frame(view, lo, hi)
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, "%s_%s%s.png" % (folder, view, "_L%d" % LIVERY if LIVERY else ""))
        bpy.context.scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        print("rendered", path)

result = {"cars": CARS, "views": VIEWS, "out": OUT}
