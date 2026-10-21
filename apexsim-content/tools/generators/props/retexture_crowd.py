r"""Put the current crowd strip (props/grandstand/T_crowd.png, painted by
gen_crowd.py) into every grandstand GLB with a `crowd_cards` slot, in place:
import, swap the slot's image, re-export to the same path. Geometry and slot
names untouched.

    blender -b --python tools/generators/props/retexture_crowd.py    (from apexsim-content/)
    or exec(open(r"...\retexture_crowd.py").read()) in a Blender session
"""
import bpy, glob, os, importlib.util, sys

_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)

STRIP = os.path.join(apex.PROPS_ROOT, "grandstand", "T_crowd.png")


def retexture(path):
    apex.reset_scene()
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=path)
    asset = os.path.basename(path)[:-4]
    ob = bpy.data.objects.get(asset) or next(o for o in bpy.data.objects if o.type == 'MESH')
    ob.name = asset
    img = bpy.data.images.load(STRIP, check_existing=False)
    img.name = "T_crowd"
    img.pack()
    hit = False
    for m in ob.data.materials:
        if m.name.split(".")[0] != "crowd_cards":
            continue
        for n in m.node_tree.nodes:
            if n.type == 'TEX_IMAGE':
                n.image = img
                hit = True
    if not hit:
        return None
    for m in ob.data.materials:   # keep slot names exactly as they were
        m.name = m.name.split(".")[0]
    return apex.export_one("grandstand", asset)[1]


result = {}
for p in sorted(glob.glob(os.path.join(apex.PROPS_ROOT, "grandstand", "*.glb"))):
    with open(p, "rb") as f:
        if b"crowd_cards" not in f.read():
            continue
    result[os.path.basename(p)] = retexture(p)
print("RETEXTURED", result)
