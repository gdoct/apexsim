r"""Warm-climate trees for the kit (track-improvements.md kit gap 13):

  tree/eucalypt_l     ~24 m gum tree: a tall pale trunk forking into crooked
                      limbs, an open, clumpy grey-green crown of hanging
                      leaf sprays (AlbertParkour)
  tree/stone_pine_l   ~17 m umbrella pine: a bare, leaning trunk forking
                      high into a candelabra of limbs under a broad, flat,
                      dense dark-green crown (Barcelunatic)

Both are leaf-card trees from card_trees.py (TreeMesh, _shell_cards), like
the rest of the kit. Slots: `tree_bark` (the kit's, on the pine),
`tree_bark_pale` (gum bark), `tree_card_eucalypt`, `tree_card_stone_pine`;
the three new ones come from card_trees.warm_materials() and appear in no
other GLB, so this script builds on its own: rebuilding the other tree
scripts does not need it, and it does not need them.

Pivot on the ground at the trunk base, centred. Run headless from
apexsim-content:

    "C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" -b --python tools/generators/props/build_trees_med.py
    blender -b --python-expr "ASSET='stone_pine_l'; exec(open(r'E:\apexsim\apexsim-content\tools\generators\props\build_trees_med.py').read())"

Exports props/tree/<key>.glb, previews props/_preview/<key>.png (beside
broadleaf_l and conifer_l for scale) plus _trees_med_far.png (the lineup
from 150 m), and saves props/tree/trees_med.blend.
"""
import bpy, math, os, random, importlib.util, sys
from mathutils import Vector

_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))


def _load(name, rel):
    s = importlib.util.spec_from_file_location(name, os.path.join(_ROOT, rel))
    m = importlib.util.module_from_spec(s)
    sys.modules[name] = m
    s.loader.exec_module(m)
    return m


apex = _load("apex", r"tools\generators\props\apex_props.py")
ct = _load("card_trees", r"tools\generators\props\card_trees.py")
UP = Vector((0.0, 0.0, 1.0))
PREVIEW_DIR = os.path.join(_ROOT, "props", "_preview")

try:
    ASSET
except NameError:
    ASSET = "all"


def _crooked(rng, a, b, c, n, jitter):
    """A bezier through a, b, c with each inner point kicked sideways: a
    gum's limbs zig-zag where a pine's run smooth."""
    pts = ct._bezier(a, b, c, n)
    for i in range(1, n):
        pts[i] = pts[i] + Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-0.3, 0.3))) * jitter
    return pts


def _limb(tm, mat, pts, r0, r1, segs):
    n = len(pts) - 1
    tm.tube(mat, pts, [r0 + (r1 - r0) * (i / n) for i in range(n + 1)], segs=segs, tile=1.5)


# ------------------------------------------------------------------ eucalypt
def eucalypt_l(name="eucalypt_l"):
    rng = random.Random(ct.seed_of(name))
    mats = ct.warm_materials()
    bark, leaf = mats["bark_pale"], mats["eucalypt"]
    tm = ct.TreeMesh(name)
    card = 2.0
    # the clumps: separated, high, irregular; a crown that is all gaps
    lobes = [(Vector((0.4, 0.3, 20.8)), UP, Vector((2.6, 2.4, 1.9)))]
    tries = 0
    while len(lobes) < 12 and tries < 4000:
        tries += 1
        z = rng.uniform(12.5, 20.0)
        rmax = 6.2 - 0.25 * max(0.0, z - 16.0)
        r = rng.uniform(1.5, rmax)
        a = rng.uniform(0, 2 * math.pi)
        c = Vector((math.cos(a) * r, math.sin(a) * r, z))
        if any((c - L[0]).length < 3.4 for L in lobes):
            continue
        s = rng.uniform(0.85, 1.15)
        lobes.append((c, (c - Vector((0, 0, 10))).normalized(),
                      Vector((2.4 * s, 2.2 * s, rng.uniform(1.3, 1.8)))))
    # trunk: tall, nearly straight, a little flare at the foot
    fork = Vector((0.9, -0.4, 9.0))
    tpts = ct._bezier(Vector((0, 0, 0)), Vector((-0.1, 0.1, 5.0)), fork, 7)
    tm.tube(bark, tpts, [0.62, 0.5, 0.47, 0.45, 0.43, 0.41, 0.39, 0.37], segs=10, tile=1.5)
    # three leaders, each taking the clumps on its side of the crown
    az0 = rng.uniform(0, 2 * math.pi)
    groups = {0: [], 1: [], 2: []}
    for L in lobes:
        a = math.atan2(L[0].y - fork.y, L[0].x - fork.x)
        k = int(((a - az0) % (2 * math.pi)) / (2 * math.pi / 3)) % 3
        groups[k].append(L)
    for k, members in groups.items():
        if not members:
            continue
        mean = sum((L[0] for L in members), Vector()) / len(members)
        end = fork + (mean - fork) * rng.uniform(0.45, 0.55)
        end.z = max(end.z, fork.z + 3.0)
        mid = fork + (end - fork) * 0.5 + UP * 0.8
        lpts = _crooked(rng, fork - UP * 0.4, mid, end, 4, 0.35)
        _limb(tm, bark, lpts, 0.3, 0.2, 8)
        for (c, d, lr) in members:
            tgt = c - UP * lr.z * 0.3
            m2 = end + (tgt - end) * 0.5 + UP * 0.6
            bpts = _crooked(rng, end, m2, tgt, 4, 0.4)
            _limb(tm, bark, bpts, 0.16, 0.05, 6)
            for _ in range(2):                            # twigs into the clump
                tip = c + Vector((rng.uniform(-1, 1) * lr.x, rng.uniform(-1, 1) * lr.y, rng.uniform(-0.3, 0.6) * lr.z)) * 0.7
                tm.tube(bark, [tgt, (tgt + tip) / 2 + UP * 0.2, tip], [0.05, 0.035, 0.015], segs=4, tile=1.5)
    centre = Vector((0, 0, 17.0))
    radii = Vector((7.0, 7.0, 5.5))
    ct._shell_cards(tm, leaf, rng, lobes, centre, radii, 560, card, rows=1, hang=1.1, floor=11.0)
    return tm.finish()


# ------------------------------------------------------------------ stone pine
def stone_pine_l(name="stone_pine_l"):
    rng = random.Random(ct.seed_of(name))
    mats = ct.warm_materials()
    bark, leaf = mats["bark"], mats["stone_pine"]
    tm = ct.TreeMesh(name)
    card = 1.9
    # trunk: bare, leaning a few degrees, forking at about half the height
    lean = Vector((1.0, 0.35, 0.0))
    fork = Vector((0, 0, 7.6)) + lean
    tpts = ct._bezier(Vector((0, 0, 0)), Vector((0.15, 0.05, 4.0)), fork, 6)
    tm.tube(bark, tpts, [0.62, 0.48, 0.45, 0.42, 0.4, 0.38, 0.36], segs=10)
    crown_c = Vector((0.3, 0.1, 13.6))
    lobes = [(crown_c + Vector((0, 0, 0.35)), UP, Vector((3.1, 3.1, 1.5)))]
    n = 6
    a0 = rng.uniform(0, 2 * math.pi)
    for k in range(n):
        a = a0 + 2 * math.pi * k / n + rng.uniform(-0.2, 0.2)
        r = rng.uniform(3.4, 4.0)
        c = crown_c + Vector((math.cos(a) * r, math.sin(a) * r, rng.uniform(-0.35, 0.15)))
        lobes.append((c, Vector((math.cos(a), math.sin(a), 0.2)).normalized(),
                      Vector((2.7, 2.7, rng.uniform(1.4, 1.7)))))
    for k in range(4):                                   # an uneven rim
        a = a0 + math.pi / n + 2 * math.pi * k / 4 + rng.uniform(-0.3, 0.3)
        r = rng.uniform(4.6, 5.2)
        c = crown_c + Vector((math.cos(a) * r, math.sin(a) * r, rng.uniform(-0.7, -0.3)))
        lobes.append((c, Vector((math.cos(a), math.sin(a), 0.0)), Vector((1.9, 1.9, 1.2))))
    # the candelabra: limbs up and out from the fork, each forking under the crown
    m = 5
    b0 = rng.uniform(0, 2 * math.pi)
    for k in range(m):
        a = b0 + 2 * math.pi * k / m + rng.uniform(-0.25, 0.25)
        out = Vector((math.cos(a), math.sin(a), 0))
        start = fork - UP * rng.uniform(0.0, 0.8)
        mid = crown_c + out * rng.uniform(2.0, 2.8)
        mid.z = rng.uniform(10.2, 10.9)
        pts = ct._bezier(start, start + (mid - start) * 0.5 + UP * 0.9, mid, 4)
        _limb(tm, bark, pts, 0.24, 0.15, 7)
        for s in (-1, 1):
            aa = a + s * rng.uniform(0.3, 0.5)
            end = crown_c + Vector((math.cos(aa), math.sin(aa), 0)) * rng.uniform(3.6, 4.8)
            end.z = rng.uniform(12.0, 12.6)
            pts2 = ct._bezier(mid, (mid + end) / 2 + UP * 0.7, end, 3)
            _limb(tm, bark, pts2, 0.13, 0.05, 6)
    radii = Vector((6.8, 6.8, 2.4))
    ct._shell_cards(tm, leaf, rng, lobes, crown_c, radii, 780, card, rows=1, upright=0.25, floor=11.4)
    return tm.finish()


# ------------------------------------------------------------------ date palm
def date_palm_l(name="date_palm_l"):
    """A date palm: a straight, slightly leaning trunk of cut leaf bases and
    a round head of stiff, arching pinnate fronds: young ones upright, old
    ones hanging brown against the trunk."""
    rng = random.Random(ct.seed_of(name))
    mats = ct.warm_materials()
    bark, frond = mats["bark_palm"], mats["date_palm"]
    tm = ct.TreeMesh(name)
    Ht = 10.2
    lean = Vector((0.55, -0.25, 0.0))
    top = Vector((0, 0, Ht)) + lean
    tpts = ct._bezier(Vector((0, 0, 0)), Vector((0, 0, Ht * 0.5)) + lean * 0.15, top, 8)
    tm.tube(bark, tpts, [0.55, 0.43, 0.41, 0.40, 0.40, 0.41, 0.43, 0.47, 0.52], segs=10, tile=1.0)
    head = top - UP * 0.15

    def normal_of(p):
        d = p - head
        d.z *= 0.6
        return ((d.normalized() if d.length > 1e-4 else UP) + UP * 0.4).normalized()

    # (count, elevation range deg, length range m, width m, cells, drop below the head m)
    tiers = ((5, (74, 86), (2.6, 3.2), 1.2, (ct.CELL_SUN,), -0.3),
             (14, (48, 66), (3.8, 4.4), 2.0, (ct.CELL_SUN, ct.CELL_WARM), -0.1),
             (16, (20, 40), (4.4, 5.0), 2.1, (ct.CELL_WARM, ct.CELL_COOL), 0.05),
             (13, (-12, 14), (4.2, 4.8), 2.0, (ct.CELL_COOL, ct.CELL_WARM), 0.2),
             (8, (-74, -58), (3.4, 4.0), 1.6, (ct.CELL_DARK,), 0.45))
    k_all = 0
    for (n, (e0, e1), (l0, l1), w, cells, drop) in tiers:
        for k in range(n):
            a = k_all * 2.39996 + rng.uniform(-0.15, 0.15)   # the phyllotaxis spiral
            k_all += 1
            el = math.radians(rng.uniform(e0, e1))
            L = rng.uniform(l0, l1)
            out = Vector((math.cos(a), math.sin(a), 0.0))
            U = (out * math.cos(el) + UP * math.sin(el)).normalized()
            side = Vector((-math.sin(a), math.cos(a), 0.0))
            N = side.cross(U)
            if N.z < 0:
                N = -N
            roll = rng.uniform(-0.25, 0.25)
            N = (N * math.cos(roll) + side * math.sin(roll)).normalized()
            base = head + out * 0.2 - UP * drop
            # the tip arches over: more on a level frond than on an upright one
            arch = -UP * L * (0.12 + 0.3 * max(0.0, math.cos(el))) if el > 0 else -UP * L * 0.05
            tm.card(frond, base, U, N, w, L, cells[rng.randrange(len(cells))], normal_of,
                    fold=0.35, rows=4, droop=arch)
    return tm.finish()


BUILD = {
    "eucalypt_l": eucalypt_l,
    "stone_pine_l": stone_pine_l,
    "date_palm_l": date_palm_l,
}


def _tris(ob):
    return sum(len(p.vertices) - 2 for p in ob.data.polygons)


def _bbox(ob):
    vs = [v.co for v in ob.data.vertices]
    lo = [min(v[i] for v in vs) for i in range(3)]
    hi = [max(v[i] for v in vs) for i in range(3)]
    return lo, hi


def _ground(size=600.0):
    g = apex.Builder("PreviewGround")
    g.box(apex.material("preview_ground", (0.25, 0.32, 0.18), roughness=0.9),
          (-size / 2, -size / 2, -0.05), (size / 2, size / 2, 0))
    g.finish()


apex.reset_scene()
keys = list(BUILD) if ASSET == "all" else [ASSET]
report = {}
for k in keys:
    ob = BUILD[k](k)
    apex.export_one("tree", k)
    lo, hi = _bbox(ob)
    report[k] = {"tris": _tris(ob), "bbox_min": [round(x, 2) for x in lo], "bbox_max": [round(x, 2) for x in hi],
                 "size": [round(hi[i] - lo[i], 2) for i in range(3)], "slots": [m.name for m in ob.data.materials]}

blend = apex.save_blend("tree", "trees_med") if ASSET == "all" else None

# previews: each new tree between the kit's broadleaf_l and conifer_l
refs = [ct.broadleaf("_ref_broadleaf_l", 16.0, 13.0, ct.seed_of("broadleaf_l"), 620, 2.0),
        ct.conifer("_ref_conifer_l", 20.0, 6.8, ct.seed_of("conifer_l"), rows=1)]
report["_ref_tris"] = {r.name: _tris(r) for r in refs}
_ground()
os.makedirs(PREVIEW_DIR, exist_ok=True)
objs = {k: bpy.data.objects[k] for k in keys}
for k, ob in objs.items():
    for o in objs.values():
        o.hide_render = o is not ob
    refs[0].location = (-17.0, 3.0, 0.0)
    refs[1].location = (14.0, 3.0, 0.0)
    ob.location = (0, 0, 0)
    apex.preview(os.path.join(PREVIEW_DIR, k + ".png"), look_from=(4, -48, 7), look_at=(0, 0, 10),
                 res=(1600, 900), fov=50)
    apex.preview(os.path.join(PREVIEW_DIR, "_" + k + "_close.png"), look_from=(14, -22, 4), look_at=(0, 0, 11),
                 res=(1200, 900), fov=60)
# the lineup from 150 m: silhouettes at race distance
xs = [-36.0, -18.0, 0.0, 18.0, 36.0, 54.0]
lineup = [refs[0]] + list(objs.values()) + [refs[1]]
for o, x in zip(lineup, xs[:len(lineup)]):
    o.hide_render = False
    o.location = (x, 0, 0)
apex.preview(os.path.join(PREVIEW_DIR, "_trees_med_far.png"), look_from=(0, -150, 4), look_at=(0, 0, 9),
             res=(1600, 700), fov=30)
for o in objs.values():
    o.location = (0, 0, 0)
result = {"report": report, "blend": blend}
print("TREES_MED", result)
