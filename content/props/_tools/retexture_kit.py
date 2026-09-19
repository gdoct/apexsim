r"""Retexture the high-frequency kit in place (docs/PROPS.md step 4): import
each GLB, swap its flat material slots for baked albedo / roughness / normal
versions (apex_tex), re-export to the same path. Geometry untouched.

    ASSETS = "all"    # or ["barrier/armco_4m", ...]
    exec(open(r"D:\apexsim\content\props\_tools\retexture_kit.py").read())

Slots not listed in SLOTS keep their flat material (glass, LEDs, brand and
crowd/fence masks stay the importer's business).
"""
import bpy, os, importlib.util, sys
from mathutils import Vector

_ROOT = "D:\\apexsim"
def _load(name, rel):
    s = importlib.util.spec_from_file_location(name, os.path.join(_ROOT, rel))
    m = importlib.util.module_from_spec(s); sys.modules[name] = m; s.loader.exec_module(m)
    return m
apex = _load("apex", "content\\props\\_tools\\apex_props.py")
tex = _load("apex_tex", "content\\props\\_tools\\apex_tex.py")

KIT = [
    "barrier/armco_4m", "barrier/armco_4m_fence", "barrier/armco_end", "barrier/concrete_4m",
    "barrier/concrete_4m_rail", "barrier/tecpro_2m",
    "tire_wall/tires_4m", "tire_wall/tires_corner",
    "grandstand/bay_10m", "grandstand/bay_10m_roof", "grandstand/bay_10m_large", "grandstand/bay_10m_large_roof",
    "grandstand/bay_10m_curve6", "grandstand/bay_10m_curve6_roof", "grandstand/bay_10m_curve12",
    "grandstand/bay_10m_curve12_roof", "grandstand/bay_10m_curve6_in", "grandstand/bay_10m_curve6_in_roof",
    "grandstand/bay_10m_stadium_roof", "grandstand/bay_10m_stadium_curve6_roof",
    "grandstand/end_cap", "grandstand/end_cap_large", "grandstand/end_cap_stadium",
    "grandstand/scaffold_10m", "grandstand/banking_seats",
    "pit/garage_6m", "pit/garage_6m_closed", "pit/garage_end", "pit/pit_wall_6m", "pit/pit_wall_plain_6m", "pit/box_kit",
    "misc/bull_statue",
]
# every `<bay>_crowd` variant that exists is retextured with its base
import glob as _glob
KIT += sorted("grandstand/" + os.path.basename(p)[:-4] for p in _glob.glob(os.path.join(_ROOT, "content", "props", "grandstand", "*_crowd.glb")))
try:
    ASSETS
except NameError:
    ASSETS = "all"

SLOTS = tex.KIT_SLOTS


def slot_material(name):
    return tex.kit_material(name)


def retexture(key):
    kind, asset = key.split("/")
    path = os.path.join(apex.PROPS_ROOT, kind, asset + ".glb")
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=path)
    ob = bpy.data.objects.get(asset)
    if ob is None:   # importer may have wrapped it
        ob = next(o for o in bpy.context.selected_objects if o.type == 'MESH')
        ob.name = asset
    swapped = []
    for i, m in enumerate(ob.data.materials):
        base = m.name.split(".")[0]
        if base in SLOTS:
            ob.data.materials[i] = slot_material(base)
            swapped.append(base)
        elif m.name != base:
            # the importer suffixed a name already in the file (from an
            # earlier asset); the slot name must survive the round trip
            other = bpy.data.materials.get(base)
            if other is not None and other is not m:
                other.name = base + "_prev"
            m.name = base
    # importer leaves the object rotated for Y-up; export handles the frame
    return {"swapped": swapped, "kept": [m.name for m in ob.data.materials if m.name.split(".")[0] not in SLOTS],
            "glb": apex.export_one(kind, asset)}


keys = KIT if ASSETS == "all" else ASSETS
apex.reset_scene()
tex.reset_cache()
result = {k: retexture(k) for k in keys if os.path.exists(os.path.join(apex.PROPS_ROOT, *k.split('/')) + '.glb')}
