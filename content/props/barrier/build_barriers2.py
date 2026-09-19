r"""Barrier kit, batch 2: sausage kerb, tecpro corner cap, concrete end ramp.

    ASSET = "all"        # or one of ASSETS
    exec(open(r"D:\apexsim\content\props\barrier\build_barriers2.py").read())

Frame per docs/PROPS.md: +X along the road, road on -Y, Z up, pivot on the
ground at the footprint centre (thin modules).
"""
import bpy, math, os, importlib.util, sys
from mathutils import Vector

_ROOT = "D:\\apexsim"
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, r"content\props\_tools\apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
_t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "content\\props\\_tools\\apex_tex.py"))
tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
B, M = apex.Builder, tex.kit_material   # baked slot where the kit has one, flat otherwise

ASSETS = ["sausage_kerb_2m", "tecpro_corner", "concrete_end"]
try:
    ASSET
except NameError:
    ASSET = "all"


def sausage_kerb_2m():
    """FIA-style sausage kerb: 0.5 m wide, 0.1 m high, rounded top, 2 m long.
    Yellow with a black-stencilled band pair at the ends (same slot, UVs)."""
    b = B("sausage_kerb_2m")
    yel = M("kerb_yellow", (0.95, 0.78, 0.05), roughness=0.55)
    # profile in (y, z): flat base, steep 20° side, rounded crown
    prof = [(-0.25, 0.0), (-0.23, 0.05)]
    for i in range(1, 8):
        a = math.pi * i / 8
        prof.append((-0.23 * math.cos(a), 0.05 + 0.05 * math.sin(a)))
    prof += [(0.23, 0.05), (0.25, 0.0)]
    b.extrude_profile(yel, prof, -1.0, 1.0, closed=True)
    return b.finish()


def tecpro_corner():
    """90° tecpro cap: a quarter annulus, inner R 0.3, outer R 1.3, centred at
    (-1, 1). The run arrives along +X, ends at x = -1 (its face y -0.3..0.7,
    like tecpro_2m), and this turns it away from the road to face +Y at x≈-0.2.
    Same convention as tires_corner. Red half then white half, black base."""
    b = B("tecpro_corner")
    red = M("tecpro_red", (0.75, 0.06, 0.05), roughness=0.45)
    white = M("tecpro_white", (0.9, 0.9, 0.88), roughness=0.45)
    base = M("tecpro_base", (0.08, 0.08, 0.08), roughness=0.8)
    cx, cy = -1.0, 1.0
    H, HB = 1.15, 0.28   # block height, base skirt height
    R0, R1 = 0.3, 1.3
    n = 8  # arc segments

    def ring(r, z, a0, a1, k):
        return [(cx + r * math.cos(a0 + (a1 - a0) * i / k), cy + r * math.sin(a0 + (a1 - a0) * i / k), z) for i in range(k + 1)]

    def block(mat, a0, a1, z0, z1, r0, r1, k):
        # 4 rings: inner-bottom, outer-bottom, outer-top, inner-top around the arc
        ib, ob = ring(r0, z0, a0, a1, k), ring(r1, z0, a0, a1, k)
        it, ot = ring(r0, z1, a0, a1, k), ring(r1, z1, a0, a1, k)
        secs = []
        for i in range(k + 1):
            secs.append([ib[i], ob[i], ot[i], it[i]])
        b.loft(mat, secs, closed=True, cap_ends=True)

    a0, a1 = -math.pi / 2, 0.0
    am = (a0 + a1) / 2
    # base skirt slightly wider than the blocks
    block(base, a0, a1, 0.0, HB, R0 - 0.03, R1 + 0.03, n)
    block(red, a0, am, HB, H, R0, R1, n // 2)
    block(white, am, a1, HB, H, R0, R1, n // 2)
    # top-corner chamfer look: a thin top ring cap in base colour
    block(base, a0, a1, H - 0.02, H + 0.02, R0 + 0.05, R1 - 0.05, n)
    return b.finish()


def concrete_end():
    """Ramped terminal for concrete_4m: same Jersey profile, full 1.0 m at
    x = -1 tapering to 0.15 m at x = +1. Tiles onto concrete_4m at x = -1."""
    b = B("concrete_end")
    con = M("barrier_concrete", (0.66, 0.65, 0.62), roughness=0.85)

    def prof(h):
        # jersey: 0.3 half-width base, kink at 0.25 (0.2), top half-width 0.12
        k = min(0.25, h * 0.4)
        return [(-0.3, 0.0), (-0.3, 0.05), (-0.2, k), (-0.12, h), (0.12, h), (0.2, k), (0.3, 0.05), (0.3, 0.0)]

    secs = []
    for i in range(6):
        x = -1.0 + 2.0 * i / 5
        h = 1.0 - 0.85 * (i / 5) ** 1.3
        secs.append([(x, y, z) for (y, z) in prof(h)])
    b.loft(con, secs, closed=True, cap_ends=True)
    return b.finish()


BUILD = {"sausage_kerb_2m": sausage_kerb_2m, "tecpro_corner": tecpro_corner, "concrete_end": concrete_end}
names = ASSETS if ASSET == "all" else [ASSET]
apex.reset_scene()
tex.reset_cache()
for n in names:
    BUILD[n]()
out = apex.export_all("barrier", names)
result = {"exported": out, "stats": apex.stats()}
