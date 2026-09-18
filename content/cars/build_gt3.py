"""Build one of the GT3 cars in Blender and export it to content/cars/<folder>/<stem>.glb.

    VARIANT = "limbotiti"      # posh | limbotiti | murcetes
    exec(open(r"D:\\apexsim\\content\\cars\\build_gt3.py").read())

Each GT3 has its own set of cross-section keys, so the silhouettes differ: a
rounded rear-engined coupe with a fastback and wide hips, a low sharp wedge,
and a long-bonnet GT. Everything below the shape data comes from
content/cars/carlib.py, which is shared with build_lmp2.py.

Frame: nose on -Y, ground z=0, metres; left-hand drive (driver on +X).

Stance (class-wide, and matched by each car.toml's [wheels] table): a
0.345/0.355 m tyre on a 1.70 m track inside a ~2.04 m body, with the arch
cut 24 mm clear of the tyre - so the wheel fills the arch instead of sitting
20 cm inboard of an oversized porthole.

Section control points, floor centre outwards and up to the roof centre:
    0 floor centre      3 fender peak (HARD)   6 upper surface
    1 floor edge        4 shoulder (HARD)      7 roof shoulder
    2 sill (HARD)       5 belt line            8 roof centre
`hard` indices are creases: the section is sampled either side of them, so
they stay edges instead of melting into the surrounding surface.
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

# ------------------------------------------------------------------ loading
_ROOT = r"D:\apexsim"
for _n, _p in (("apex", os.path.join(_ROOT, r"content\props\_tools\apex_props.py")),
               ("carlib", os.path.join(_ROOT, r"content\cars\carlib.py"))):
    _s = importlib.util.spec_from_file_location(_n, _p)
    _m = importlib.util.module_from_spec(_s)
    sys.modules[_n] = _m
    _s.loader.exec_module(_m)
import apex, carlib                                        # noqa: E402
from carlib import Builder                                  # noqa: E402

# ------------------------------------------------------------------ class kit
TYRE_F, TYRE_R = 0.345, 0.355          # tyre radii; car.toml [wheels] must agree
TRACK = 1.700                          # hub to hub, both axles
HUB_X = TRACK / 2.0
ARCH_GAP = 0.024
SAMP = 5
BELT, ROOF = 5.35, 6.60                  # control indices where glass starts / ends

VARIANTS = {
    "posh": dict(
        folder="posh-gt3rs", stem="posh_gt3rs", logo="posh_logo.png",
        paint=(0.10, 0.55, 0.75), accent=(0.05, 0.05, 0.05), caliper=(0.95, 0.75, 0.05),
        axles=(-1.375, 1.375),
        glass_y=(-0.92, 0.92), screen_y=(-0.95, -0.40), rear_glass_y=(0.50, 0.94),
        wing="pylon", wing_z=1.19, wing_y=1.86, wing_hw=0.86, ducktail=True,
        ends=dict(nose_top=0.585, nose_w=0.90, tail_top=0.905, tail_bot=0.30, tail_w=0.94),
        lights="round", grille=False, exhaust="centre", mirror="pod",
        shutlines=[(-1.82, "upper"), (-0.94, "side"), (0.58, "side"), (1.24, "upper"), (1.84, "upper")],
        vents=["fender", "naca_front"], scoop=None,
        keys=[
            (-2.25, [(0, .195), (.55, .195), (.72, .31), (.79, .50), (.66, .60), (.46, .655), (.30, .68), (.14, .695), (0, .70)]),
            (-1.90, [(0, .095), (.86, .095), (.955, .35), (.985, .625), (.845, .735), (.60, .755), (.40, .74), (.18, .74), (0, .74)]),
            (-1.375, [(0, .075), (.90, .075), (.995, .40), (1.015, .685), (.875, .795), (.63, .80), (.42, .78), (.18, .78), (0, .78)]),
            (-0.95, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.855, .775), (.67, .795), (.46, .82), (.20, .82), (0, .82)]),
            (-0.60, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.86, .765), (.71, .885), (.52, 1.035), (.30, 1.095), (0, 1.115)]),
            (-0.20, [(0, .075), (.90, .075), (.985, .40), (.99, .665), (.865, .765), (.725, .905), (.56, 1.135), (.32, 1.265), (0, 1.30)]),
            (0.40, [(0, .075), (.925, .075), (1.005, .40), (1.015, .685), (.885, .79), (.745, .915), (.58, 1.10), (.32, 1.235), (0, 1.275)]),
            (1.00, [(0, .075), (.945, .075), (1.025, .42), (1.045, .725), (.905, .835), (.745, .895), (.52, 1.00), (.28, 1.095), (0, 1.13)]),
            (1.375, [(0, .075), (.945, .075), (1.025, .42), (1.05, .745), (.905, .855), (.72, .895), (.48, .955), (.24, 1.015), (0, 1.04)]),
            (1.90, [(0, .135), (.90, .135), (.985, .42), (1.005, .725), (.865, .835), (.66, .895), (.44, .935), (.20, .975), (0, 1.00)]),
            (2.30, [(0, .295), (.78, .295), (.865, .48), (.885, .695), (.765, .815), (.58, .875), (.38, .915), (.16, .945), (0, .95)]),
        ]),
    "limbotiti": dict(
        folder="limbotiti-caravan-gt3", stem="limbotiti_caravan", logo="limbotiti_logo.png",
        paint=(0.95, 0.72, 0.02), accent=(0.05, 0.05, 0.05), caliper=(0.05, 0.05, 0.05),
        axles=(-1.375, 1.375),
        glass_y=(-0.98, 0.50), screen_y=(-1.00, -0.46), rear_glass_y=(0.30, 0.74),
        wing="swan", wing_z=1.14, wing_y=1.80, wing_hw=0.88, ducktail=False,
        ends=dict(nose_top=0.50, nose_w=0.92, tail_top=0.80, tail_bot=0.30, tail_w=0.95),
        lights="ybar", grille=False, exhaust="hexquad", mirror="stalk",
        shutlines=[(-1.84, "upper"), (-0.94, "side"), (0.56, "side"), (0.86, "upper"), (1.84, "upper")],
        vents=["fender", "side_intake", "deck_louvres"], scoop=(0.0, 0.26, 0.13),
        keys=[
            (-2.25, [(0, .155), (.70, .155), (.855, .255), (.915, .415), (.755, .485), (.52, .515), (.31, .535), (.14, .545), (0, .55)]),
            (-1.85, [(0, .095), (.905, .095), (.975, .315), (1.00, .555), (.845, .665), (.63, .685), (.43, .66), (.20, .65), (0, .65)]),
            (-1.375, [(0, .075), (.925, .075), (1.00, .355), (1.015, .635), (.865, .755), (.655, .765), (.445, .705), (.20, .68), (0, .67)]),
            (-1.00, [(0, .075), (.925, .075), (1.00, .355), (1.005, .655), (.885, .765), (.765, .765), (.60, .725), (.30, .705), (0, .70)]),
            (-0.65, [(0, .075), (.925, .075), (1.00, .355), (1.005, .655), (.885, .785), (.77, .845), (.61, .895), (.31, .935), (0, .95)]),
            (-0.30, [(0, .075), (.925, .075), (1.00, .355), (1.005, .675), (.895, .805), (.785, .865), (.625, 1.095), (.32, 1.145), (0, 1.16)]),
            (0.30, [(0, .075), (.945, .075), (1.02, .375), (1.025, .695), (.925, .825), (.805, .885), (.625, 1.075), (.32, 1.125), (0, 1.14)]),
            (0.85, [(0, .075), (.965, .075), (1.04, .395), (1.045, .715), (.925, .835), (.765, .885), (.545, .905), (.26, .925), (0, .93)]),
            (1.375, [(0, .075), (.965, .075), (1.04, .395), (1.055, .735), (.925, .835), (.745, .865), (.505, .875), (.24, .875), (0, .88)]),
            (1.85, [(0, .135), (.945, .135), (1.02, .395), (1.025, .715), (.905, .815), (.705, .845), (.465, .855), (.22, .855), (0, .86)]),
            (2.25, [(0, .315), (.845, .315), (.925, .495), (.945, .675), (.805, .775), (.605, .815), (.385, .835), (.16, .835), (0, .84)]),
        ]),
    "murcetes": dict(
        folder="murcetes-amd-gt3", stem="murcetes_amd_gt3", logo="murcetes_logo.png",
        paint=(0.80, 0.80, 0.82), accent=(0.0, 0.65, 0.62), caliper=(0.85, 0.10, 0.05),
        axles=(-1.375, 1.375),
        glass_y=(-0.46, 0.98), screen_y=(-0.48, 0.12), rear_glass_y=(0.74, 1.00),
        wing="pylon", wing_z=1.16, wing_y=1.90, wing_hw=0.86, ducktail=False,
        ends=dict(nose_top=0.64, nose_w=0.88, tail_top=0.92, tail_bot=0.32, tail_w=0.93),
        lights="slant", grille=True, exhaust="side", mirror="pod",
        shutlines=[(-1.86, "upper"), (-0.52, "upper"), (-0.46, "side"), (0.74, "side"), (1.44, "upper"), (1.92, "upper")],
        vents=["fender", "bonnet_louvres"], scoop=None,
        keys=[
            (-2.35, [(0, .215), (.60, .215), (.775, .335), (.835, .535), (.685, .625), (.47, .675), (.28, .70), (.12, .715), (0, .72)]),
            (-1.95, [(0, .095), (.885, .095), (.975, .355), (1.00, .635), (.855, .765), (.63, .835), (.42, .855), (.18, .875), (0, .89)]),
            (-1.375, [(0, .075), (.925, .075), (1.005, .395), (1.02, .695), (.875, .825), (.655, .84), (.44, .845), (.18, .855), (0, .86)]),
            (-0.80, [(0, .075), (.925, .075), (1.005, .395), (1.01, .695), (.885, .815), (.675, .84), (.46, .855), (.20, .865), (0, .87)]),
            (-0.45, [(0, .075), (.925, .075), (1.005, .395), (1.01, .695), (.885, .815), (.695, .855), (.48, .875), (.22, .875), (0, .88)]),
            (-0.10, [(0, .075), (.925, .075), (1.005, .395), (1.01, .695), (.885, .815), (.715, .915), (.52, 1.055), (.28, 1.135), (0, 1.16)]),
            (0.40, [(0, .075), (.945, .075), (1.025, .415), (1.03, .715), (.905, .835), (.735, .935), (.56, 1.135), (.30, 1.245), (0, 1.28)]),
            (0.95, [(0, .075), (.965, .075), (1.045, .435), (1.055, .755), (.925, .875), (.755, .955), (.52, 1.075), (.26, 1.155), (0, 1.18)]),
            (1.375, [(0, .075), (.965, .075), (1.045, .435), (1.065, .775), (.925, .895), (.735, .955), (.48, 1.015), (.24, 1.055), (0, 1.08)]),
            (1.90, [(0, .155), (.925, .155), (1.005, .435), (1.025, .755), (.885, .875), (.675, .935), (.42, .975), (.20, .995), (0, 1.01)]),
            (2.35, [(0, .335), (.805, .335), (.885, .495), (.905, .715), (.765, .835), (.56, .895), (.36, .935), (.16, .955), (0, .97)]),
        ]),
}

try:
    VARIANT
except NameError:
    VARIANT = "posh"
V = VARIANTS[VARIANT]
CAR_DIR = os.path.join(carlib.CARS_ROOT, V["folder"])
AX_F, AX_R = V["axles"]


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


# ---------------------------------------------------------------- materials
carlib.reset_scene()
M = carlib.car_materials(V["paint"], V["accent"], V["caliper"],
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]))

# ---------------------------------------------------------------- body loft
KEYS = carlib.fender_bump(V["keys"], (AX_F, AX_R),
                          amount=V.get("fender_rise", 0.085), width=0.62, j0=2.2, j1=4.3)
# The ends. Two keys 35 cm apart loft into one blunt blend, so the nose and
# tail get their own stations: a bumper face at the tip, a corner station
# behind it, the bonnet leading edge, and a pulled-in tip so the end cap is a
# small rounded panel rather than the full silhouette stamped flat.
E = V["ends"]
K0, K1 = V["keys"][0][0], V["keys"][-1][0]
KEYS = carlib.insert_stations(KEYS, [K0 + 0.10, K0 + 0.26, K1 - 0.10, K1 - 0.28])
KEYS = carlib.remap_station(KEYS, K0, x_scale=E["nose_w"], z_hi=E["nose_top"], j_from=2)
KEYS = carlib.remap_station(KEYS, K0 + 0.10, x_scale=0.975, z_hi=E["nose_top"] + 0.045, j_from=3)
KEYS = carlib.remap_station(KEYS, K0 + 0.26, z_hi=E["nose_top"] + 0.095, j_from=4)
KEYS = carlib.remap_station(KEYS, K1, x_scale=E["tail_w"], z_lo=E["tail_bot"], z_hi=E["tail_top"])
KEYS = carlib.remap_station(KEYS, K1 - 0.10, x_scale=0.985, z_hi=E["tail_top"] + 0.03, j_from=4)
KEYS = carlib.tip_station(KEYS, K0, K0 - 0.06, x_scale=0.80, z_shrink=0.72)
KEYS = carlib.tip_station(KEYS, K1, K1 + 0.05, x_scale=0.82, z_shrink=0.74)
L = carlib.Loft(KEYS, samp=SAMP, ny=84, hard=(2, 3, 4))
NOSE, TAIL = L.nose, L.tail
BUMPER_Y, TAILP_Y = K0, K1          # the flat faces sit here; the tips are rounded

# Shut lines. `upper` runs over the bonnet or deck, `side` down the flank;
# both stay clear of the arch openings (a groove cut where the arch boolean
# later removes material leaves its walls hanging in the wheel well).
ARCH_SPAN = [(AX_F - TYRE_F - ARCH_GAP, AX_F + TYRE_F + ARCH_GAP),
             (AX_R - TYRE_R - ARCH_GAP, AX_R + TYRE_R + ARCH_GAP)]


def in_arch(y):
    return any(a <= y <= b for (a, b) in ARCH_SPAN)


for (y, kind) in V["shutlines"]:
    if kind == "upper":
        L.groove(y, 4.2, 8.0, depth=0.011, width=0.016)
    elif not in_arch(y):
        L.groove(y, 1.9, 4.1, depth=0.011, width=0.016)
    else:
        print("skipped side shutline inside an arch at y =", y)

if "fender" in V["vents"]:
    L.recess(AX_F + 0.44, AX_F + 0.72, 2.15, 3.30, depth=0.042, rim=0.022)
if "side_intake" in V["vents"]:
    L.recess(0.70, 1.06, 1.95, 3.35, depth=0.055, rim=0.026)
if "bonnet_louvres" in V["vents"]:
    L.recess(NOSE + 0.95, NOSE + 1.45, 5.40, 8.00, depth=0.032, rim=0.030)
if "naca_front" in V["vents"]:
    L.recess(K0 + 0.72, K0 + 1.02, 6.00, 8.00, depth=0.030, rim=0.016)
if "deck_louvres" in V["vents"]:
    L.recess(V["rear_glass_y"][1] + 0.04, V["rear_glass_y"][1] + 0.50, 5.60, 8.00,
             depth=0.030, rim=0.026)
# brake-cooling exit ahead of each rear arch
L.recess(AX_R - 0.80, AX_R - 0.50, 2.10, 3.20, depth=0.040, rim=0.022)
# headlamp pockets: a recess that wraps the nose corner between the bumper
# shoulder (j3) and the bonnet edge, so the lamp is set into the corner
# rather than punched through it as a box
LAMP_J = {"round": (2.75, 4.25), "ybar": (2.55, 3.65), "slant": (2.85, 4.55)}[V["lights"]]
LAMP_Y0, LAMP_Y1 = BUMPER_Y + 0.02, BUMPER_Y + 0.24
L.recess(LAMP_Y0, LAMP_Y1, LAMP_J[0], LAMP_J[1], depth=0.055, rim=0.018)
# tail lamp bands wrap the rear corner the same way
TLAMP_J = (3.0, 3.75)
TLAMP_Y0, TLAMP_Y1 = TAILP_Y - 0.20, TAILP_Y - 0.02
L.recess(TLAMP_Y0, TLAMP_Y1, TLAMP_J[0], TLAMP_J[1], depth=0.030, rim=0.014)


def face_mat(ym, kk, right):
    jc = kk / SAMP
    if jc < BELT:
        return M.paint
    s0, s1 = V["screen_y"]
    r0, r1 = V["rear_glass_y"]
    if s0 < ym < s1 or r0 < ym < r1:
        return M.glass
    g0, g1 = V["glass_y"]
    if g0 < ym < g1 and jc < ROOF:
        return M.glass
    return M.paint


body = L.build("body", face_mat, [M.paint, M.glass], subsurf=0)
save("body")

# -------------------------------------------------------------- wheel arches
parts_objs = []
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    for sx in (-1, 1):
        bead = carlib.arch(body, L, M, y, tyre, sx=sx, gap=ARCH_GAP,
                           x_in=0.53, z_bottom=0.125, bead=V.get("arch_bead", False))
        if bead is not None:
            parts_objs.append(bead)
carlib.sharpen(body, 34.0)
save("arches")

# --------------------------------------------------------------- apertures
# Lamps, grille and exhaust exits are cut, not drawn on: the cutter's surface
# becomes the walls of the opening, so the aperture comes out lined and the
# lens sits in a hole instead of floating on the paint.
# bumper: one wide mouth across the nose with a brake duct outboard of each
# end, and the tail's lower valance where the exhausts and diffuser exit
MOUTH_HW = 0.50 if V["grille"] else 0.44
MOUTH_Z = (E["nose_top"] - 0.36, E["nose_top"] - 0.16)
carlib.aperture(body, M, (-MOUTH_HW, NOSE - 0.05, MOUTH_Z[0]),
                (MOUTH_HW, BUMPER_Y + 0.22, MOUTH_Z[1]), mat=M.mesh)
for sx in (-1, 1):
    d0, d1 = sorted((sx * (MOUTH_HW + 0.09), sx * (MOUTH_HW + 0.26)))
    dy = carlib.surface_station(L, MOUTH_HW + 0.26, MOUTH_Z[0] + 0.05, NOSE, margin=0.02)
    carlib.aperture(body, M, (d0, NOSE - 0.05, MOUTH_Z[0] + 0.01),
                    (d1, dy + 0.16, MOUTH_Z[1] - 0.03), mat=M.mesh)
VAL_Z = (E["tail_bot"] + 0.02, E["tail_bot"] + 0.19)
VAL_HW = L.x_at(TAILP_Y, VAL_Z[1]) - 0.11
carlib.aperture(body, M, (-VAL_HW, TAILP_Y - 0.22, VAL_Z[0]),
                (VAL_HW, TAIL + 0.05, VAL_Z[1]), mat=M.mesh)
# rain light, centre of the tail panel
RAIN_Z = (VAL_Z[1] + 0.05, VAL_Z[1] + 0.15)
carlib.aperture(body, M, (-0.12, TAILP_Y - 0.10, RAIN_Z[0]), (0.12, TAIL + 0.05, RAIN_Z[1]))
carlib.sharpen(body, 34.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
# lo_z is the lowest thing the build makes (the splitter), which is what
# the client will measure off the exported mesh box
CK = carlib.cockpit_points(L, V["wing_z"], liner_z=0.046)
EYE, DASH_Z = CK["eye"], CK["dash_z"]
DX = EYE.x                                       # driver centreline (LHD: +X)
SILL_X = L.x_at(0.0, 0.20)
HALF = max(L.x_at(y, 0.30) for y in (AX_F, 0.0, AX_R))

# ---- floor aero: splitter, dive planes, side skirts, diffuser
# splitter: a plan panel that follows the nose, oversailing it by 55 mm
SPL_Z = 0.052
spl_plan = carlib.floor_plan(L, NOSE + 0.05, AX_F - 0.20, 0.16, steps=14, inset=-0.028)
carlib.panel_xy(p, M.carbon, spl_plan, SPL_Z - 0.009, SPL_Z + 0.009)
for sx in (-1, 1):
    fy0, fy1 = NOSE + 0.12, NOSE + 0.62
    fence = [(fy0, SPL_Z), (fy1, SPL_Z), (fy1, SPL_Z + 0.055), (fy0, SPL_Z + 0.085)]
    carlib.plate(p, M.carbon, fence, sx * (L.x_at((fy0 + fy1) / 2, 0.17) - 0.030), 0.012,
                 chamfer=0.004)
for sx in (-1, 1):
    dy = carlib.surface_station(L, 0.78, 0.34, NOSE, margin=0.02)
    dx = L.x_at(dy + 0.10, 0.34)
    dp = [(dy, 0.315), (dy + 0.26, 0.345), (dy + 0.26, 0.367), (dy, 0.337)]
    carlib.plate(p, M.carbon, dp, sx * (dx + 0.045), 0.105, chamfer=0.008)
for sx in (-1, 1):
    skirt = [(-1.00, 0.085), (1.00, 0.085), (1.00, 0.175), (-1.00, 0.165)]
    carlib.plate(p, M.carbon, skirt, sx * (SILL_X + 0.012), 0.032, chamfer=0.008)
    # accent stripe along the sill: the slot the client paints per team, so
    # every car has to carry it (docs/CAR_MODELS.md)
    stripe = [(-0.96, 0.190), (0.96, 0.190), (0.96, 0.270), (-0.96, 0.265)]
    carlib.plate(p, M.accent, stripe, sx * (SILL_X + 0.004), 0.008)
# diffuser: tucked under the tail, ramping up into the bodywork
DIF_Y0, DIF_Y1 = AX_R + 0.22, TAIL - 0.02
DIF_HW = min(L.x_at(y, 0.19) for y in
             (DIF_Y0, (DIF_Y0 + DIF_Y1) / 2, DIF_Y1 - 0.05)) - 0.050
carlib.diffuser(p, M.carbon, DIF_Y0, DIF_Y1, DIF_HW, 0.070, VAL_Z[0] + 0.02,
                thick=0.014, strakes=(-0.62, -0.21, 0.21, 0.62), strake_h=0.150)

# ---- rear wing: element, gurney, endplates, mounts, brake LED
WZ, WY, WHW = V["wing_z"], V["wing_y"], V["wing_hw"]
te_y, te_z = carlib.foil(p, M.carbon, -WHW, WHW, 0.345, 0.105, 0.055, WY, WZ, angle_deg=-9.0)
carlib.gurney(p, M.carbon, -WHW, WHW, te_y, te_z, h=0.024, t=0.005, angle_deg=-9.0)
carlib.led_strip(p, M, -WHW + 0.02, WHW - 0.02, te_y - 0.055, te_z, h=0.030, t=0.014,
                 glow=M.brake, dir_y=1.0)
for sx in (-1, 1):
    ep = [(WY - 0.06, WZ - 0.060), (WY + 0.10, WZ - 0.105), (WY + 0.38, WZ - 0.085),
          (WY + 0.405, WZ + 0.030), (WY + 0.36, WZ + 0.120), (WY + 0.16, WZ + 0.150),
          (WY - 0.02, WZ + 0.105), (WY - 0.08, WZ + 0.030)]
    carlib.plate(p, M.carbon, ep, sx * (WHW + 0.012), 0.014, chamfer=0.005)
if V["wing"] == "pylon":
    for sx in (-1, 1):
        py = [(WY + 0.01, WZ - 0.34), (WY + 0.26, WZ - 0.32), (WY + 0.22, WZ + 0.02),
              (WY + 0.02, WZ + 0.02)]
        carlib.plate(p, M.carbon, py, sx * 0.54, 0.028, chamfer=0.008)
else:
    for sx in (-1, 1):
        carlib.swan_neck(p, M.carbon, sx * 0.46, (0, WY - 0.30, WZ - 0.26),
                         (0, WY + 0.10, WZ + 0.02), r=0.022)
if V["ducktail"]:
    d0, d1 = TAILP_Y - 0.34, TAILP_Y - 0.03
    dt = [(d0, L.roof_z(d0) - 0.012), (d1, L.roof_z(d1) + 0.030), (d1, L.roof_z(d1) + 0.062),
          (d0 + 0.02, L.roof_z(d0) + 0.020)]
    carlib.plate(p, M.accent, dt, 0.0, 1.50, chamfer=0.012)

# ---- lights: projector cups in the corner pockets, under a lens that
# follows the bodywork; tail bands wrapping the rear corners
for sx in (-1, 1):
    jm = (LAMP_J[0] + LAMP_J[1]) / 2
    n = 2 if V["lights"] == "round" else 3
    for k in range(n):
        t = (k + 0.5) / n
        yk = LAMP_Y0 + 0.03 + (LAMP_Y1 - LAMP_Y0 - 0.06) * t
        pt = L.point(yk, jm)
        nm = L.normal(yk, jm)
        r = 0.034 if n == 3 else 0.046
        for (rr, dd, mm) in ((r, 0.045, M.lamp_h), (r * 0.86, 0.010, M.lamp)):
            a = pt - nm * (0.050 - 0.004)
            b = pt - nm * (0.050 - 0.004 - dd) if mm is M.lamp else pt - nm * 0.006
            p.bar(mm, (sx * a.x, a.y, a.z), (sx * b.x, b.y, b.z), rr, segs=18)
    carlib.conform_decal(p, M.glass, L, LAMP_Y0 + 0.012, LAMP_Y1 - 0.012,
                         LAMP_J[0] + 0.12, LAMP_J[1] - 0.12, sx=sx, lift=-0.003, nu=6, nv=4)
    # tail: running band in the corner pocket, brake band below it on the panel
    carlib.conform_decal(p, M.tail, L, TLAMP_Y0 + 0.012, TLAMP_Y1 - 0.012,
                         TLAMP_J[0] + 0.15, TLAMP_J[1] - 0.15, sx=sx, lift=-0.026, nu=6, nv=3)
    carlib.conform_decal(p, M.glass, L, TLAMP_Y0 + 0.010, TLAMP_Y1 - 0.010,
                         TLAMP_J[0] + 0.10, TLAMP_J[1] - 0.10, sx=sx, lift=-0.003, nu=6, nv=3)
    bz = E["tail_top"] - 0.20
    bx = L.x_at(TAILP_Y, bz) - 0.05
    carlib.led_strip(p, M, sx * 0.30, sx * bx, TAIL - 0.012, bz, h=0.055, t=0.030,
                     glow=M.brake, dir_y=-1.0)
    carlib.led_strip(p, M, sx * 0.30, sx * bx, TAIL - 0.012, bz + 0.075, h=0.040, t=0.030,
                     glow=M.tail, dir_y=-1.0)
carlib.lamp_cluster(p, M, -0.11, 0.11, RAIN_Z[0] + 0.01, RAIN_Z[1] - 0.01, TAIL - 0.008,
                    depth=0.09, style="bar", dir_y=-1.0, glow=M.rain, housing=False)
# bumper mouth: mesh backing, bars and a raised surround
carlib.grille(p, M, -MOUTH_HW + 0.012, MOUTH_HW - 0.012, BUMPER_Y + 0.02,
              MOUTH_Z[0] + 0.012, MOUTH_Z[1] - 0.012, bars=3, depth=0.24, backing=True)
carlib.duct_lip(p, M.carbon, -MOUTH_HW, MOUTH_HW, BUMPER_Y - 0.008, MOUTH_Z[0], MOUTH_Z[1],
                out=0.018)
for sx in (-1, 1):
    d0, d1 = sorted((sx * (MOUTH_HW + 0.09), sx * (MOUTH_HW + 0.26)))
    p.box(M.mesh, (d0 + 0.01, BUMPER_Y + 0.13, MOUTH_Z[0] + 0.02),
          (d1 - 0.01, BUMPER_Y + 0.15, MOUTH_Z[1] - 0.04))
# rear valance: mesh, exhausts inside it
p.box(M.mesh, (-VAL_HW + 0.01, TAILP_Y - 0.16, VAL_Z[0] + 0.01),
      (VAL_HW - 0.01, TAILP_Y - 0.14, VAL_Z[1] - 0.01))

# ---- blades in the vents cut into the loft
if "fender" in V["vents"]:
    for sx in (-1, 1):
        yv0, yv1 = AX_F + 0.46, AX_F + 0.70
        zv0, zv1 = L.point(yv0, 2.2).z, L.point(yv0, 3.3).z
        carlib.louvre_bank(p, M.carbon, L.x_at((yv0 + yv1) / 2, (zv0 + zv1) / 2) - 0.012,
                           yv0, yv1, zv0 + 0.02, zv1 - 0.02, count=4, rake_deg=24, sx=sx)
if "side_intake" in V["vents"]:
    for sx in (-1, 1):
        zv0, zv1 = L.point(0.88, 2.0).z, L.point(0.88, 3.3).z
        carlib.louvre_bank(p, M.carbon, L.x_at(0.88, (zv0 + zv1) / 2) - 0.018,
                           0.73, 1.03, zv0 + 0.03, zv1 - 0.03, count=3, rake_deg=18, sx=sx)
if "bonnet_louvres" in V["vents"] or "deck_louvres" in V["vents"]:
    y0 = NOSE + 0.98 if "bonnet_louvres" in V["vents"] else V["rear_glass_y"][1] + 0.07
    for k in range(4):
        yv = y0 + k * 0.105
        zt = L.roof_z(yv) - 0.028
        p.box(M.carbon, (-0.40, yv, zt - 0.012), (0.40, yv + 0.052, zt + 0.014))
if "naca_front" in V["vents"]:
    yv = K0 + 0.75
    zt = L.roof_z(yv + 0.12)
    p.box(M.mesh, (-0.34, yv, zt - 0.040), (0.34, yv + 0.24, zt - 0.024))

# ---- mirrors, exhausts, roof scoop, antenna, tow hooks, wiper
MIR_Y = V["screen_y"][0] + 0.30
MIR_Z = L.point(MIR_Y, 4.55).z + 0.045
for sx in (-1, 1):
    carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.115, MIR_Y, MIR_Z, sx=sx, style=V["mirror"])
EXH_Z = (VAL_Z[0] + VAL_Z[1]) / 2
if V["exhaust"] == "centre":
    for x in (-0.105, 0.105):
        p.cylinder(M.metal, (x, TAILP_Y - 0.16, EXH_Z), 0.050, 0.17, segs=16, axis='Y')
        p.cylinder(M.lamp_h, (x, TAILP_Y - 0.17, EXH_Z), 0.060, 0.05, segs=16, axis='Y')
elif V["exhaust"] == "hexquad":
    for x in (-0.325, -0.185, 0.185, 0.325):
        p.cylinder(M.metal, (x, TAILP_Y - 0.16, EXH_Z), 0.045, 0.17, segs=6, axis='Y')
else:
    for sx in (-1, 1):
        for k in range(2):
            p.cylinder(M.metal, (sx * (SILL_X + 0.01), 0.16 + k * 0.13, 0.225),
                       0.037, 0.07 * sx, segs=14, axis='X')
if V["scoop"]:
    sx_, sy_, sr_ = V["scoop"]
    zt = L.roof_z(sy_)
    sc_prof = [(sy_ - 0.34, zt - 0.02), (sy_ + 0.28, zt - 0.02), (sy_ + 0.24, zt + sr_),
               (sy_ - 0.20, zt + sr_ * 0.92)]
    carlib.plate(p, M.carbon, sc_prof, sx_, 0.30, chamfer=0.030)
    p.box(M.mesh, (sx_ - 0.115, sy_ - 0.355, zt + 0.01), (sx_ + 0.115, sy_ - 0.33, zt + sr_ - 0.01))
p.bar(M.carbon, (0.0, 0.92, L.roof_z(0.92) - 0.01), (0.0, 0.92, L.roof_z(0.92) + 0.22), 0.007)
for yy in (NOSE + 0.10, TAIL - 0.10):
    p.torus(M.towhook, (0.30, yy, 0.245), 0.055, 0.014, segs=16, rings=8)
wy = V["screen_y"][0] + 0.06
p.bar(M.carbon, (DX - 0.46, wy, L.z_at(wy, 0.30) + 0.014),
      (DX + 0.30, wy + 0.04, L.z_at(wy + 0.04, 0.30) + 0.014), 0.013, segs=8)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
p.box(M.interior, (-0.80, -0.58, 0.115), (0.80, 1.20, 0.155))                     # floor
for sx in (-1, 1):
    p.box(M.interior, (sx * 0.845 - 0.035, -0.58, 0.155), (sx * 0.845 + 0.035, 1.20, 0.62))
p.box(M.seat, (DX - 0.255, 0.14, 0.155), (DX + 0.255, 0.62, 0.315))               # base
p.box(M.seat, (DX - 0.275, 0.50, 0.315), (DX + 0.275, 0.665, 0.905))              # back
for sx in (-1, 1):
    p.box(M.seat, (DX + sx * 0.285 - 0.04, 0.12, 0.315), (DX + sx * 0.285 + 0.04, 0.62, 0.455))
p.box(M.interior, (-0.80, -0.58, 0.58), (0.80, CK["wheel"].y - 0.12, DASH_Z))     # dash
p.box(M.display, (DX - 0.13, CK["wheel"].y - 0.125, DASH_Z - 0.17),
      (DX + 0.13, CK["wheel"].y - 0.115, DASH_Z - 0.045))
p.bar(M.metal, (DX, CK["wheel"].y - 0.16, CK["wheel"].z - 0.05),
      (DX, CK["wheel"].y, CK["wheel"].z), 0.022, segs=10)
carlib.steering_wheel(p, M, (DX, CK["wheel"].y, CK["wheel"].z), r=0.158, rim_r=0.020)
for x in (DX - 0.20, DX - 0.08, DX + 0.04):
    p.box(M.metal, (x - 0.032, -0.56, 0.165), (x + 0.032, -0.50, 0.30))           # pedals
# roll cage: main hoop, roof rails, A-pillar bars, door bars, rear stays
# Every cage node is taken off the loft and pulled inboard, so the bars stay
# under the skin. Hard-coded heights put the hoop through the roof on a low
# car and on the floor on a tall one.
def inside(y, x, drop=0.055, pull=0.055):
    """A point just inside the upper surface at half-width `x`."""
    return (x, y, L.z_at(y, abs(x) + pull) - drop)


HOOP_Y = min(V["rear_glass_y"][0] + 0.10, 0.85)
SCR_Y = V["screen_y"][1]
PY = V["screen_y"][0] + 0.06
for sx in (-1, 1):
    hoop_top = inside(HOOP_Y, sx * 0.54)
    rail = inside(SCR_Y, sx * 0.46)
    foot = (sx * (L.x_at(PY, 0.72) - 0.075), PY, L.z_at(PY, 0.74) - 0.045)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.21), (sx * 0.70, HOOP_Y, hoop_top[2] - 0.10),
          0.023, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, hoop_top[2] - 0.10), hoop_top, 0.023, segs=8)
    p.bar(M.cage, hoop_top, rail, 0.021, segs=8)
    mid_y = (SCR_Y + PY) / 2
    mid = (sx * (L.x_at(mid_y, 0.86) - 0.085), mid_y, L.z_at(mid_y, 0.60) - 0.050)
    p.bar(M.cage, rail, mid, 0.019, segs=8)
    p.bar(M.cage, mid, foot, 0.019, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, hoop_top[2] - 0.16), (sx * 0.56, AX_R - 0.10, 0.54),
          0.020, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.56), (sx * 0.68, -0.52, 0.60), 0.020, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.30), (sx * 0.68, -0.50, 0.34), 0.017, segs=8)
p.bar(M.cage, inside(HOOP_Y, -0.54), inside(HOOP_Y, 0.54), 0.023, segs=8)
p.bar(M.cage, (-0.70, HOOP_Y, 0.58), (0.70, HOOP_Y, 0.58), 0.019, segs=8)

# ---- wordmark, laid on the flank rather than floated off it
carlib.conform_decal(p, M.logo, L, -0.52, 0.62, 2.05, 3.05, sx=1, lift=0.005, nu=14, nv=6)
carlib.conform_decal(p, M.logo, L, -0.52, 0.62, 2.05, 3.05, sx=-1, lift=0.005, nu=14, nv=6,
                     flip_u=True)

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0045, segments=2,
                     angle_deg=38.0)
parts_objs.append(parts)
save("parts")

# ------------------------------------------------------------ join + export
car, glb = carlib.join_and_export([body] + parts_objs, V["stem"], CAR_DIR,
                                  export=os.environ.get("APEX_EXPORT", "1") == "1")
save("joined")
print("stats:", carlib.mesh_stats(car))
if glb:
    print("GLB:", glb, os.path.getsize(glb))
