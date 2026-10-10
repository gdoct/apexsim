r"""Near-LOD card trees and ground scatter (docs/content/props.md step 4):

  tree/broadleaf_m_near          10 m broadleaf, ~440 once-bent leaf cards (~3.6 K tris)
  tree/broadleaf_m_near_autumn   the same tree in autumn leaves
  tree/conifer_m_near            12 m spruce, frond cards in whorls (~3.5 K tris)
  tree/grass_clump               3 crossed grass cards, 1 x 0.6 m
  tree/wildflower_clump          same with flowers

The trees come from card_trees.py, like the rest of the kit (build_trees.py):
build both scripts together, since the card and bark materials are shared
by name on import. Card slots `tree_card_*`, `scatter_grass`,
`scatter_flower` are alpha-MASKED and two-sided (the importer must treat
them like `fence_mesh`); the trunk is `tree_bark`.

    ASSET = "all"
    exec(open(r"E:\apexsim\scripts\content\props\build_near_trees.py").read())
"""
import bpy, math, os, importlib.util, sys
from mathutils import Vector

_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")
def _load(name, rel):
    s = importlib.util.spec_from_file_location(name, os.path.join(_ROOT, rel))
    m = importlib.util.module_from_spec(s); sys.modules[name] = m; s.loader.exec_module(m)
    return m
apex = _load("apex", "scripts\\content\\props\\apex_props.py")
tex = _load("apex_tex", "scripts\\content\\props\\apex_tex.py")
ct = _load("card_trees", "scripts\\content\\props\\card_trees.py")
B, M = apex.Builder, apex.material

try:
    ASSET
except NameError:
    ASSET = "all"


def broadleaf_m_near(foliage="broadleaf", name="broadleaf_m_near"):
    """broadleaf_m's near-view twin: wider, more and smaller cards, each
    bent once along its length."""
    return ct.broadleaf(name, 10.0, 9.0, 21, 440, 1.25, foliage=foliage, rows=2)


def conifer_m_near():
    return ct.conifer("conifer_m_near", 12.0, 3.9, 23, density=1.4, rows=2)


def clump(name, slot, flowers):
    b = B(name)
    card = tex.card_material(slot, tex.grass_card(seed=41 + flowers, flowers=flowers))
    for k in range(3):
        b.card(card, (0, 0, -0.1), 1.0, 0.7, yaw=k * math.pi / 3)   # sunk so the dense base is buried
    return b.finish()


BUILD = {
    "broadleaf_m_near": broadleaf_m_near,
    "broadleaf_m_near_autumn": lambda: broadleaf_m_near("autumn", "broadleaf_m_near_autumn"),
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
