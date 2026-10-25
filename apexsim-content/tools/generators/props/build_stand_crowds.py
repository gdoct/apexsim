r"""The `_crowd` twins of the stand modules whose seating is not bay_10m's.

Two ways, by module:

- `MODULES` copy bay_10m's seating section (tent_roof_10m, palm_canopy_10m,
  lotus_canopy_10m from build_kit_gaps_stands.py): bay_10m_crowd's crowd
  cards, unchanged, joined onto each module.
- `FITTED` have rows of their own (the stadium family: three tiers, forty
  rows): their bench tops are found in the mesh (`seat_rows`), and each row
  gets one of bay_10m_crowd's card rows, in turn, at the height and depth
  the cards sit over a bay_10m bench. Any seated module can be added here.

Run after a module or the crowd strip changes.

    blender -b --python tools/generators/props/build_stand_crowds.py    (from apexsim-content/)
"""
import bpy, bmesh, os, importlib.util, sys

_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)

MODULES = ["tent_roof_10m", "palm_canopy_10m", "lotus_canopy_10m"]
FITTED = ["bay_10m_stadium_roof", "bay_10m_stadium_curve6_roof"]
SOURCE = "bay_10m_crowd"
# A row of cards is the cards whose bottoms are within this of each other, m.
ROW_TOL_M = 0.15
# Where a fitted card's foot stands against its bench's centre, m along the
# stand (negative: toward the road).
FITTED_CARD_Y_M = -0.12


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


def seat_rows(ob, min_area):
    """The bench tops of a stand, front to back: (z, y) of each row's
    upward-facing seat faces of at least `min_area` m2."""
    mw = ob.matrix_world
    names = [m.name.split(".")[0] for m in ob.data.materials]
    tops = []
    for p in ob.data.polygons:
        n = (mw.to_3x3() @ p.normal).normalized()
        if names[p.material_index].startswith("grandstand_seat") and n.z > 0.9 and p.area >= min_area:
            c = mw @ p.center
            tops.append((c.z, c.y))
    rows = []
    for z, y in sorted(tops):
        if rows and z - rows[-1][0] < ROW_TOL_M:
            continue
        rows.append((z, y))
    return rows


def card_rows(crowd):
    """bay_10m_crowd's cards grouped into rows by their bottom edge: each
    row's faces as lists of (position, uv), and the row's (bottom z, front y)."""
    bm = bmesh.new()
    bm.from_mesh(crowd.data)
    uv = bm.loops.layers.uv.active
    mw = crowd.matrix_world
    cards = {i for i, m in enumerate(crowd.data.materials) if m.name.split(".")[0] == "crowd_cards"}
    faces = []
    for f in bm.faces:
        if f.material_index not in cards:
            continue
        pts = [(mw @ l.vert.co, l[uv].uv.copy()) for l in f.loops]
        faces.append((min(p.z for p, _ in pts), min(p.y for p, _ in pts), pts))
    bm.free()
    faces.sort(key=lambda f: f[0])
    rows = []
    for bottom, front, pts in faces:
        if rows and bottom - rows[-1]["z"] < ROW_TOL_M:
            rows[-1]["faces"].append(pts)
            rows[-1]["y"] = min(rows[-1]["y"], front)
        else:
            rows.append({"z": bottom, "y": front, "faces": [pts]})
    return rows


def fitted_crowd(base, template):
    """An object of crowd cards for `base`'s own rows: each of its bench
    rows takes the template's card rows in turn, moved so the cards sit
    over that bench as they sit over the template's."""
    benches = seat_rows(template, 0.0)
    cards = card_rows(template)

    # How a template card row stands against the bench it seats on: the
    # highest bench under its bottom edge (plus a little: the cards start
    # just under the bench top, where a seated figure's legs are).
    def seat_of(row):
        under = [b for b in benches if b[0] <= row["z"] + 0.3]
        return max(under) if under else benches[0]

    offsets = []
    for row in cards:
        z, y = seat_of(row)
        offsets.append((row["z"] - z, row["y"] - y))
    # Deeper, steeper rows than bay_10m's (the stadium's rise 0.62 m in
    # 0.85): the template's depth put the card inside the next row's
    # riser, with only a flag's tip showing. On a fitted module the card
    # stands at its own bench's front, over the seat.
    offsets = [(dz, FITTED_CARD_Y_M) for dz, _ in offsets]

    target = seat_rows(base, 1.0)
    me = bpy.data.meshes.new("fitted_crowd")
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    for i, (z, y) in enumerate(target):
        k = i % len(cards)
        row, (dz, dy) = cards[k], offsets[k]
        shift_z = (z + dz) - row["z"]
        shift_y = (y + dy) - row["y"]
        for pts in row["faces"]:
            verts = [bm.verts.new((p.x, p.y + shift_y, p.z + shift_z)) for p, _ in pts]
            face = bm.faces.new(verts)
            for loop, (_, t) in zip(face.loops, pts):
                loop[uv].uv = t
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new("fitted_crowd", me)
    bpy.context.scene.collection.objects.link(ob)
    me.materials.append(next(m for m in template.data.materials if m.name.split(".")[0] == "crowd_cards"))
    print("FITTED", base.name, len(target), "rows,", len(me.polygons), "cards")
    return ob


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


def fitted_twin(module):
    apex.reset_scene()
    base = load(module)
    template = load(SOURCE)
    crowd = fitted_crowd(base, template)
    bpy.data.objects.remove(template, do_unlink=True)
    merge_into(base, crowd)
    for m in base.data.materials:
        m.name = m.name.split(".")[0]
    base.name = module + "_crowd"
    return apex.export_one("grandstand", base.name)


only = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
result = {m: twin(m)[1] for m in MODULES if not only or m in only}
result.update({m: fitted_twin(m)[1] for m in FITTED if not only or m in only})
print("TWINS", result)
