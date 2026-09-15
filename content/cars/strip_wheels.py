"""Take the wheels out of a car GLB so the client can draw and spin its own.

Run inside Blender (Blender 4.x/5.x), e.g. from its Python console:

    exec(open(r"D:\\apexsim\\content\\cars\\strip_wheels.py").read())
    print(measure(r"D:\\apexsim\\content\\cars\\posh-lmp2\\posh_lmp2.glb"))
    strip(r"D:\\apexsim\\content\\cars\\posh-lmp2\\posh_lmp2.glb", dry_run=True)

`measure` finds the four wheels from the faces whose material names a tyre
and returns what the `[wheels]` table of car.toml wants (docs/CAR_MODELS.md).
`strip` removes every loose part that lies wholly inside one of the four
wheel cylinders (tyre, rim, disc, caliper, nuts), which leaves the uprights
and suspension that reach out of the wheel, then writes the GLB back.
Both work in a scratch scene, so whatever is open in Blender is untouched.

Frame (as every car in this folder): nose on -Y, driver/left on +X, ground at
z = 0, metres.
"""
import bpy, bmesh, contextlib, io, json, struct
from mathutils import Vector

TYRE_WORDS = ("tyre", "tire")


def glb_json(path):
    """The JSON chunk of a GLB."""
    with open(path, "rb") as f:
        head = f.read(20)
        length = struct.unpack("<I", head[12:16])[0]
        return json.loads(f.read(length))


def material_names(path):
    return [m.get("name", "") for m in glb_json(path).get("materials", [])]


_DATA = ("objects", "meshes", "materials", "images", "textures", "node_groups", "cameras", "lights")


@contextlib.contextmanager
def _scene(glb):
    """A scratch scene holding `glb`, cleaned up afterwards.

    Blender suffixes a datablock name that is already taken ("car_paint.001"),
    and the exporter writes that suffix into the GLB, which would break the
    material slots the client looks up by name. So any existing material
    with one of the file's names is renamed out of the way for the duration,
    and everything the import created is deleted on the way out.
    """
    win = bpy.context.window
    orig = win.scene
    wanted = set(material_names(glb))
    held = []
    for m in bpy.data.materials:
        if m.name in wanted:
            held.append((m, m.name))
    for m, name in held:
        m.name = name + "__held_by_strip_wheels"
    before = {k: set(getattr(bpy.data, k)[:]) for k in _DATA}
    sc = bpy.data.scenes.new("strip_wheels_tmp")
    win.scene = sc
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            bpy.ops.import_scene.gltf(filepath=glb)
        got = {m.name for o in sc.objects if o.type == 'MESH' for m in o.data.materials if m}
        if got - wanted:
            raise RuntimeError("imported material names changed: %s" % sorted(got - wanted))
        yield sc
    finally:
        win.scene = orig
        bpy.data.scenes.remove(sc)
        for k in _DATA:
            coll = getattr(bpy.data, k)
            for idb in [i for i in coll[:] if i not in before[k]]:
                coll.remove(idb)
        for m, name in held:
            m.name = name


def _is_tyre(name):
    n = name.lower()
    return any(w in n for w in TYRE_WORDS)


def _tyre_boxes(sc, is_tyre=_is_tyre):
    """Bounding box of the tyre faces in each quadrant: {(left, rear): (min, max)}."""
    boxes = {}
    for o in sc.objects:
        if o.type != 'MESH':
            continue
        slots = {i for i, m in enumerate(o.data.materials) if m and is_tyre(m.name)}
        if not slots:
            continue
        verts = set()
        for p in o.data.polygons:
            if p.material_index in slots:
                verts.update(p.vertices)
        for i in verts:
            co = o.matrix_world @ o.data.vertices[i].co
            key = (co.x > 0.0, co.y > 0.0)
            box = boxes.setdefault(key, [Vector((1e9,) * 3), Vector((-1e9,) * 3)])
            for a in range(3):
                box[0][a] = min(box[0][a], co[a])
                box[1][a] = max(box[1][a], co[a])
    return boxes


def wheels_from_boxes(boxes):
    """Four quadrant boxes -> the car.toml `[wheels]` figures (front = -Y)."""
    if len(boxes) != 4:
        raise ValueError("expected tyres in all four quadrants, found %d" % len(boxes))
    out = {}
    for axle, rear in (("front", False), ("rear", True)):
        l0, l1 = boxes[(True, rear)]
        r0, r1 = boxes[(False, rear)]
        cl, cr = (l0 + l1) / 2, (r0 + r1) / 2
        radius = max(l1.y - l0.y, l1.z - l0.z, r1.y - r0.y, r1.z - r0.z) / 2
        width = max(l1.x - l0.x, r1.x - r0.x)
        out[axle] = dict(
            axle_y_m=round((cl.y + cr.y) / 2, 3),
            track_m=round(cl.x - cr.x, 3),
            centre_x_m=round((cl.x + cr.x) / 2, 3),
            centre_z_m=round((cl.z + cr.z) / 2, 3),
            radius_m=round(radius, 3),
            width_m=round(width, 3),
        )
    return out


def measure(glb, is_tyre=_is_tyre):
    with _scene(glb) as sc:
        return wheels_from_boxes(_tyre_boxes(sc, is_tyre))


def cylinders(wheels, radius_margin=0.03, inner_margin=0.10, outer_margin=0.03):
    """The four wheel volumes as (centre, radius, x_min, x_max)."""
    cyl = []
    for axle in ("front", "rear"):
        w = wheels[axle]
        for side in (1, -1):
            cx = w.get("centre_x_m", 0.0) + side * w["track_m"] / 2
            c = Vector((cx, w["axle_y_m"], w["centre_z_m"]))
            half = w["width_m"] / 2
            # The inner side gets more room: calipers and discs sit inboard.
            if side > 0:
                x0, x1 = cx - half - inner_margin, cx + half + outer_margin
            else:
                x0, x1 = cx - half - outer_margin, cx + half + inner_margin
            cyl.append((c, w["radius_m"] + radius_margin, x0, x1))
    return cyl


def _inside(co, cyl):
    c, r, x0, x1 = cyl
    if not (x0 <= co.x <= x1):
        return False
    return (co.y - c.y) ** 2 + (co.z - c.z) ** 2 <= r * r


def strip(glb, wheels=None, out=None, dry_run=False, is_tyre=_is_tyre, **margins):
    """Delete the loose parts inside the wheels. Returns a report."""
    report = {"removed_faces": 0, "by_material": {}, "tyre_faces_left": 0}
    source_names = set(material_names(glb))
    with _scene(glb) as sc:
        if wheels is None:
            wheels = wheels_from_boxes(_tyre_boxes(sc, is_tyre))
        cyl = cylinders(wheels, **margins)
        for o in [o for o in sc.objects if o.type == 'MESH']:
            bm = bmesh.new()
            bm.from_mesh(o.data)
            M = o.matrix_world
            world = {v: M @ v.co for v in bm.verts}
            seen = set()
            doomed = []
            for f in bm.faces:
                if f in seen:
                    continue
                part, stack = [], [f]
                seen.add(f)
                while stack:
                    g = stack.pop()
                    part.append(g)
                    for v in g.verts:
                        for h in v.link_faces:
                            if h not in seen:
                                seen.add(h)
                                stack.append(h)
                verts = {v for g in part for v in g.verts}
                for cy in cyl:
                    if all(_inside(world[v], cy) for v in verts):
                        doomed.extend(part)
                        break
            for g in doomed:
                m = o.data.materials[g.material_index].name if o.data.materials else "-"
                report["by_material"][m] = report["by_material"].get(m, 0) + 1
            report["removed_faces"] += len(doomed)
            if doomed:
                bmesh.ops.delete(bm, geom=doomed, context='FACES')
            for g in bm.faces:
                m = o.data.materials[g.material_index].name if o.data.materials else ""
                if is_tyre(m):
                    report["tyre_faces_left"] += 1
            if not dry_run:
                bm.to_mesh(o.data)
            bm.free()
        # Objects left with no faces go, so the export carries no empty nodes.
        if not dry_run:
            for o in [o for o in sc.objects if o.type == 'MESH' and len(o.data.polygons) == 0]:
                bpy.data.objects.remove(o, do_unlink=True)
            with contextlib.redirect_stdout(io.StringIO()):
                bpy.ops.export_scene.gltf(filepath=out or glb, use_active_scene=True, export_format='GLB')
            renamed = set(material_names(out or glb)) - source_names
            if renamed:
                raise RuntimeError("exported material names changed: %s" % sorted(renamed))
    report["wheels"] = wheels
    return report
