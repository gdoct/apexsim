r"""Near-LOD card trees and ground scatter (docs/PROPS.md step 4):

  tree/broadleaf_m_near   10 m broadleaf, trunk + branches + ~250 bent leaf cards (~2.5 K tris)
  tree/conifer_m_near     12 m spruce, trunk + ~220 drooping frond cards (~2 K tris)
  tree/grass_clump        3 crossed grass cards, 1 x 0.6 m
  tree/wildflower_clump   same with flowers

Card slots `tree_card_broadleaf`, `tree_card_conifer`, `scatter_grass`,
`scatter_flower` are alpha-MASKED and two-sided (the importer must treat
them like `fence_mesh`); the trunk reuses `tree_bark`.

    ASSET = "all"
    exec(open(r"D:\apexsim\content\props\tree\build_near_trees.py").read())
"""
import bpy, math, os, importlib.util, sys, random
from mathutils import Vector

_ROOT = "D:\\apexsim"
def _load(name, rel):
    s = importlib.util.spec_from_file_location(name, os.path.join(_ROOT, rel))
    m = importlib.util.module_from_spec(s); sys.modules[name] = m; s.loader.exec_module(m)
    return m
apex = _load("apex", "content\\props\\_tools\\apex_props.py")
tex = _load("apex_tex", "content\\props\\_tools\\apex_tex.py")
B, M = apex.Builder, apex.material

try:
    ASSET
except NameError:
    ASSET = "all"


def bark():
    return tex.pbr_material("tree_bark", tex.rubber(seed=3, rgb=(0.22, 0.15, 0.09), dust=0.15, tread=False), tile_m=0.5)


def bent_card(b, mat, center, w, h, yaw, tilt, bend=0.15):
    """2x2-subdivided card whose middle bows outward (8 tris)."""
    cy, sy = math.cos(yaw), math.sin(yaw)
    ct, st = math.cos(tilt), math.sin(tilt)
    s = b.slot(mat)
    vs = []
    for j in range(3):
        row = []
        for i in range(3):
            u, v = (i - 1) * 0.5 * w, j * 0.5 * h
            bow = bend * h * (1 - (i - 1) ** 2) * (1 - (j - 1) ** 2)
            x, y, z = u, -v * st + bow, v * ct
            p = (center[0] + x * cy - y * sy, center[1] + x * sy + y * cy, center[2] + z)
            vert = b.bm.verts.new(p)
            row.append((vert, (i * 0.5, j * 0.5)))
        vs.append(row)
    for j in range(2):
        for i in range(2):
            quad = [vs[j][i], vs[j][i + 1], vs[j + 1][i + 1], vs[j + 1][i]]
            f = b.bm.faces.new([q[0] for q in quad])
            f.material_index = s
            for l, (_, uv) in zip(f.loops, quad):
                l[b.uv].uv = uv
            b.keep.add(f)


def trunk(b, mat, base, top, r0, r1, segs=10, bends=3, rng=None):
    pts = [Vector(base) + (Vector(top) - Vector(base)) * (k / bends) + Vector((rng.uniform(-0.15, 0.15), rng.uniform(-0.15, 0.15), 0)) * (k > 0) for k in range(bends + 1)]
    secs = []
    for k, p in enumerate(pts):
        r = r0 + (r1 - r0) * k / bends
        secs.append([(p.x + r * math.cos(2 * math.pi * i / segs), p.y + r * math.sin(2 * math.pi * i / segs), p.z) for i in range(segs)])
    b.loft(mat, secs, closed=True, cap_ends=True)
    return pts[-1]


def broadleaf_m_near():
    rng = random.Random(11)
    b = B("broadleaf_m_near")
    bk = bark()
    card = tex.card_material("tree_card_broadleaf", tex.leaf_cluster_card(seed=31))
    top = trunk(b, bk, (0, 0, 0), (0, 0, 4.2), 0.32, 0.2, rng=rng)
    # branches into the canopy
    tips = []
    for k in range(6):
        a = 2 * math.pi * k / 6 + rng.uniform(-0.3, 0.3)
        L = rng.uniform(2.2, 3.2)
        tip = Vector((top.x + L * math.cos(a), top.y + L * math.sin(a), top.z + rng.uniform(1.4, 2.6)))
        b.bar(bk, top, tip, 0.1, segs=6)
        tips.append(tip)
        mid = (top + tip) / 2
        sub = Vector((mid.x + 1.2 * math.cos(a + 0.9), mid.y + 1.2 * math.sin(a + 0.9), mid.z + 1.2))
        b.bar(bk, mid, sub, 0.05, segs=5)
    # cards fill an ellipsoid canopy centred at 6.6 m, radii 4 x 4 x 3.2
    cz, rx, rz = 6.6, 4.0, 3.2
    for k in range(250):
        while True:
            u, v, w = rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)
            if u * u + v * v + w * w <= 1.0:
                break
        rad = (u * u + v * v + w * w) ** 0.5
        # bias toward the shell so the silhouette reads, keep some inside
        shell = 0.55 + 0.45 * rad
        p = (u * rx * shell, v * rx * shell, cz + w * rz * shell - 0.7)
        yaw = math.atan2(v, u) + rng.uniform(-0.5, 0.5) + math.pi / 2
        tilt = rng.uniform(-0.8, 0.8) + (0.6 if w > 0.6 else 0.0)
        size = rng.uniform(1.4, 2.1)
        bent_card(b, card, (p[0], p[1], p[2] - size * 0.4), size, size, yaw, tilt)
    return b.finish()


def conifer_m_near():
    rng = random.Random(23)
    b = B("conifer_m_near")
    bk = bark()
    card = tex.card_material("tree_card_conifer", tex.spruce_frond_card(seed=37))
    trunk(b, bk, (0, 0, 0), (0, 0, 11.4), 0.25, 0.04, segs=8, bends=4, rng=rng)
    H = 12.0
    z = 1.6
    tier = 0
    while z < H - 0.6:
        rad = 2.6 * (1 - (z / H)) + 0.25
        n = max(4, int(rad * 5.0))
        for k in range(n):
            a = 2 * math.pi * k / n + rng.uniform(-0.25, 0.25) + tier * 0.4
            L = rad * rng.uniform(0.85, 1.05)
            base = (0.1 * math.cos(a), 0.1 * math.sin(a), z)
            # frond leans out and droops: yaw faces outward, tilt lays it down
            yaw = a + math.pi / 2 + rng.uniform(-0.15, 0.15)
            tilt = -(math.pi / 2 - 0.35 + rng.uniform(-0.15, 0.15))
            bent_card(b, card, base, L * 0.7, L, yaw, tilt, bend=0.05)
        z += 0.38 + 0.02 * tier
        tier += 1
    # leader
    bent_card(b, card, (0, 0, H - 1.2), 0.5, 1.3, 0, 0)
    bent_card(b, card, (0, 0, H - 1.2), 0.5, 1.3, math.pi / 2, 0)
    return b.finish()


def clump(name, slot, flowers):
    b = B(name)
    card = tex.card_material(slot, tex.grass_card(seed=41 + flowers, flowers=flowers))
    for k in range(3):
        b.card(card, (0, 0, -0.1), 1.0, 0.7, yaw=k * math.pi / 3)   # sunk so the dense base is buried
    return b.finish()


BUILD = {
    "broadleaf_m_near": broadleaf_m_near,
    "conifer_m_near": conifer_m_near,
    "grass_clump": lambda: clump("grass_clump", "scatter_grass", 0),
    "wildflower_clump": lambda: clump("wildflower_clump", "scatter_flower", 9),
}
keys = list(BUILD) if ASSET == "all" else [ASSET]
apex.reset_scene()
exported = {}
for k in keys:
    BUILD[k]()
    exported[k] = apex.export_one("tree", k)
result = {"exported": exported, "stats": apex.stats()}
