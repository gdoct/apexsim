r"""The `_crowd` twins of the stand modules that copy bay_10m's seating
section (tent_roof_10m, palm_canopy_10m from build_kit_gaps_stands.py):
bay_10m_crowd's crowd cards, unchanged, joined onto each module. Run after
either module or the crowd strip changes.

    blender -b --python tools/generators/props/build_stand_crowds.py    (from apexsim-content/)
"""
import bpy, bmesh, os, importlib.util, sys

_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)

MODULES = ["tent_roof_10m", "palm_canopy_10m"]
SOURCE = "bay_10m_crowd"


def load(asset):
    before = set(bpy.data.objects)
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=os.path.join(apex.PROPS_ROOT, "grandstand", asset + ".glb"))
    return next(o for o in set(bpy.data.objects) - before if o.type == 'MESH')


def crowd_only(ob):
    """Strip every face but the crowd cards."""
    keep = {i for i, m in enumerate(ob.data.materials) if m.name.split(".")[0] == "crowd_cards"}
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index not in keep], context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(ob.data)
    bm.free()


def merge_into(base, crowd):
    """Append crowd's faces (UVs kept) to base under a new crowd_cards slot,
    on the data: the join operator needs a 3D view, which a headless run
    does not have."""
    mat = next(m for m in crowd.data.materials if m.name.split(".")[0] == "crowd_cards")
    base.data.materials.append(mat)
    slot = len(base.data.materials) - 1
    bm = bmesh.new()
    bm.from_mesh(base.data)
    uv = bm.loops.layers.uv.active
    src = bmesh.new()
    src.from_mesh(crowd.data)
    suv = src.loops.layers.uv.active
    vmap = {v: bm.verts.new(crowd.matrix_world @ v.co) for v in src.verts}
    for f in src.faces:
        nf = bm.faces.new([vmap[v] for v in f.verts])
        nf.material_index = slot
        nf.smooth = f.smooth
        for la, lb in zip(f.loops, nf.loops):
            lb[uv].uv = la[suv].uv
    src.free()
    bm.to_mesh(base.data)
    bm.free()
    bpy.data.objects.remove(crowd, do_unlink=True)


def twin(module):
    apex.reset_scene()
    base = load(module)
    crowd = load(SOURCE)
    crowd_only(crowd)
    merge_into(base, crowd)
    for m in base.data.materials:   # the slot names exactly as the kind has them
        m.name = m.name.split(".")[0]
    base.name = module + "_crowd"
    return apex.export_one("grandstand", base.name)


result = {m: twin(m)[1] for m in MODULES}
print("TWINS", result)
