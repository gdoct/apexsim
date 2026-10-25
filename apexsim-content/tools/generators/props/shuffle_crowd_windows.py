r"""Give each seated crowd row its own window of the crowd strip, in place.

The stand modules' crowd cards (bay_10m_crowd and the `_crowd` twins that
copy it, build_stand_crowds.py) showed the strip (T_crowd.png, gen_crowd.py)
at a window one row further along each row up: row k's two 4.4 m cards
started at u = k/6 and k/6 + 0.55. The strip's one chequered flag then
stepped up the stand on a diagonal, plain to see from 30 m. This moves each
row's pair to a start hashed from k, the two cards still side by side.

    blender -b --python tools/generators/props/shuffle_crowd_windows.py    (from apexsim-content/)

Only GLBs whose crowd cards all sit on that layout are touched (the banking
seats, the scaffold and the street tiers lay their cards out otherwise), so
running it again finds nothing to do. Re-import the GLBs it names
(ApexPropImport -asset=grandstand/<name>).
"""
import bmesh, bpy, glob, importlib.util, math, os, sys

_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)

STEP = 1.0 / 6.0   # the old layout's step a row
RIGHT = 0.55       # the right card's start past the left's
TOL = 0.004


def row_start(k):
    """Row k's new window start in [0, 1): a hash, not a step."""
    return math.modf(abs(math.sin(k * 12.9898 + 4.1414) * 43758.5453))[0]


def old_row(u_min):
    """(row, is_right) of a card on the old layout, or None."""
    for right, base in ((False, 0.0), (True, RIGHT)):
        k = round((u_min - base) / STEP)
        if k >= 0 and abs(base + k * STEP - u_min) < TOL:
            return k, right
    return None


def shuffle(path):
    apex.reset_scene()
    with apex._ui_override():
        bpy.ops.import_scene.gltf(filepath=path)
    asset = os.path.basename(path)[:-4]
    ob = bpy.data.objects.get(asset) or next(o for o in bpy.data.objects if o.type == 'MESH')
    ob.name = asset
    mats = [m.name.split(".")[0] for m in ob.data.materials]
    if "crowd_cards" not in mats:
        return None
    slot = mats.index("crowd_cards")
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    uv = bm.loops.layers.uv.active
    cards = [f for f in bm.faces if f.material_index == slot]
    rows = [old_row(min(l[uv].uv.x for l in f.loops)) for f in cards]
    if not cards or any(r is None for r in rows):
        bm.free()
        return "skipped: not the bay layout"
    for f, (k, _) in zip(cards, rows):
        shift = row_start(k) - k * STEP
        for l in f.loops:
            l[uv].uv.x += shift
    bm.to_mesh(ob.data)
    bm.free()
    for m in ob.data.materials:   # keep slot names exactly as they were
        m.name = m.name.split(".")[0]
    apex.export_one("grandstand", asset)
    return f"{len(cards)} cards in {len({k for k, _ in rows})} rows"


result = {}
for p in sorted(glob.glob(os.path.join(apex.PROPS_ROOT, "grandstand", "*.glb"))):
    with open(p, "rb") as f:
        if b"crowd_cards" not in f.read():
            continue
    result[os.path.basename(p)] = shuffle(p)
print("SHUFFLED", result)
