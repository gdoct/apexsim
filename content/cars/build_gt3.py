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
L = carlib.Loft(KEYS, samp=SAMP, ny=80, hard=(2, 3, 4))
NOSE, TAIL = L.nose, L.tail

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
    L.recess(NOSE + 0.70, NOSE + 1.00, 6.05, 6.95, depth=0.022, rim=0.040)
if "deck_louvres" in V["vents"]:
    L.recess(V["rear_glass_y"][1] + 0.04, V["rear_glass_y"][1] + 0.50, 5.60, 8.00,
             depth=0.030, rim=0.026)
# brake-cooling exit ahead of each rear arch, and the radiator exit on the nose
L.recess(AX_R - 0.80, AX_R - 0.50, 2.10, 3.20, depth=0.040, rim=0.022)


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
LAMP_X = {"round": (0.44, 0.74), "ybar": (0.40, 0.78), "slant": (0.34, 0.82)}[V["lights"]]
LAMP_H = {"round": 0.155, "ybar": 0.135, "slant": 0.130}[V["lights"]]
# A lamp has to sit on the front face. Find the first station wide enough to
# hold it, then hang it off the nose height *there*: a fixed z cuts out
# through the top of a low nose and reads as a slot in the bonnet.
LAMP_Y = carlib.surface_station(L, LAMP_X[1], 0.50, NOSE, margin=0.03)
LAMP_TOP = L.roof_z(LAMP_Y) - 0.075
LAMP_Z = (LAMP_TOP - LAMP_H, LAMP_TOP)
TAILL_Y = carlib.surface_station(L, LAMP_X[1], 0.66, TAIL, margin=0.03)
TAILL_TOP = min(L.roof_z(TAILL_Y) - 0.12, 0.80)
GRILLE_Y = carlib.surface_station(L, 0.46 if V["grille"] else 0.36, 0.30, NOSE, margin=0.04)
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_Y + 0.16, LAMP_Z[1]))
    carlib.aperture(body, M, (x0, TAILL_Y - 0.17, TAILL_TOP - 0.195),
                    (x1, TAILL_Y + 0.02, TAILL_TOP))
GW = 0.52 if V["grille"] else 0.40
GZ = (LAMP_Z[0] - 0.235, LAMP_Z[0] - 0.045)
carlib.aperture(body, M, (-GW, GRILLE_Y - 0.03, GZ[0]), (GW, GRILLE_Y + 0.24, GZ[1]), mat=M.mesh)
# rain light, centre of the tail
RAIN_Y = carlib.surface_station(L, 0.13, 0.47, TAIL, margin=0.05)
carlib.aperture(body, M, (-0.13, RAIN_Y - 0.12, 0.415), (0.13, RAIN_Y + 0.03, 0.535))
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
spl_plan = carlib.floor_plan(L, NOSE + 0.06, AX_F - 0.20, 0.17, steps=12,
                             inset=-0.035, lead=0.055)
carlib.panel_xy(p, M.carbon, spl_plan, SPL_Z - 0.009, SPL_Z + 0.009)
for sx in (-1, 1):
    fy0, fy1 = NOSE + 0.10, AX_F - 0.24
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
carlib.diffuser(p, M.carbon, DIF_Y0, DIF_Y1, DIF_HW, 0.072, 0.225,
                thick=0.014, strakes=(-0.62, -0.21, 0.21, 0.62), strake_h=0.165)

# ---- rear wing: element, gurney, endplates, mounts, brake LED
WZ, WY, WHW = V["wing_z"], V["wing_y"], V["wing_hw"]
te_y, te_z = carlib.foil(p, M.carbon, -WHW, WHW, 0.345, 0.105, 0.055, WY, WZ, angle_deg=-9.0)
carlib.gurney(p, M.carbon, -WHW, WHW, te_y, te_z, h=0.024, t=0.005, angle_deg=-9.0)
carlib.led_strip(p, M, -WHW + 0.02, WHW - 0.02, te_y - 0.055, te_z, h=0.030, t=0.014,
                 glow=M.brake, dir_y=1.0)
for sx in (-1, 1):
    ep = [(WY - 0.10, WZ - 0.115), (WY + 0.40, WZ - 0.075), (WY + 0.415, WZ + 0.135),
          (WY + 0.10, WZ + 0.165), (WY - 0.10, WZ + 0.070)]
    carlib.plate(p, M.carbon, ep, sx * (WHW + 0.012), 0.015, chamfer=0.006)
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
    dt = [(TAIL - 0.36, L.z_at(TAIL - 0.36, 0.55) - 0.01), (TAIL - 0.04, 0.985),
          (TAIL - 0.04, 1.020), (TAIL - 0.36, L.z_at(TAIL - 0.36, 0.55) + 0.025)]
    carlib.plate(p, M.accent, dt, 0.0, 1.58, chamfer=0.012)

# ---- lights in their apertures
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    style = {"round": "round", "ybar": "ybar", "slant": "bar"}[V["lights"]]
    n = 2 if V["lights"] == "round" else 3
    carlib.lamp_cluster(p, M, x0 + 0.010, x1 - 0.010, LAMP_Z[0] + 0.010, LAMP_Z[1] - 0.010,
                        LAMP_Y + 0.015, depth=0.13, style=style, count=n, dir_y=1.0,
                        housing=False, loft=L)
    # tail: running strip over a brake strip, plus a wrap-around corner
    carlib.lamp_cluster(p, M, x0 + 0.008, x1 - 0.008, TAILL_TOP - 0.085, TAILL_TOP - 0.012,
                        TAILL_Y - 0.010, depth=0.10, style="bar", dir_y=-1.0,
                        glow=M.tail, housing=False)
    carlib.lamp_cluster(p, M, x0 + 0.008, x1 - 0.008, TAILL_TOP - 0.183, TAILL_TOP - 0.100,
                        TAILL_Y - 0.010, depth=0.10, style="bar", dir_y=-1.0,
                        glow=M.brake, housing=False)
carlib.lamp_cluster(p, M, -0.118, 0.118, 0.428, 0.522, RAIN_Y - 0.008, depth=0.10,
                    style="bar", dir_y=-1.0, glow=M.rain, housing=False)
# wrap-around corner element on each rear quarter
CORNER_X = max(L.x_at(y, TAILL_TOP - 0.10) for y in (TAIL - 0.30, TAIL - 0.15, TAIL)) - 0.055
for sx in (-1, 1):
    cy = carlib.surface_station(L, CORNER_X + 0.04, TAILL_TOP - 0.10, TAIL, margin=0.010)
    carlib.led_strip(p, M, sx * CORNER_X - 0.038, sx * CORNER_X + 0.038, cy - 0.014,
                     TAILL_TOP - 0.095, h=0.125, t=0.016, glow=M.brake, dir_y=-1.0)
carlib.grille(p, M, -GW + 0.012, GW - 0.012, GRILLE_Y + 0.01, GZ[0] + 0.012, GZ[1] - 0.012,
              bars=6 if V["grille"] else 4, depth=0.20, backing=True)
carlib.duct_lip(p, M.carbon, -GW, GW, GRILLE_Y - 0.010, GZ[0], GZ[1], out=0.020)

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
    for sx in (-1, 1):
        yv = NOSE + 0.74
        zt = L.point(yv, 6.5).z
        p.box(M.mesh, (sx * 0.16, yv, zt - 0.034), (sx * 0.40, yv + 0.22, zt - 0.012))

# ---- mirrors, exhausts, roof scoop, antenna, tow hooks, wiper
MIR_Y = V["screen_y"][0] + 0.30
MIR_Z = L.point(MIR_Y, 4.55).z + 0.045
for sx in (-1, 1):
    carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.115, MIR_Y, MIR_Z, sx=sx, style=V["mirror"])
if V["exhaust"] == "centre":
    for x in (-0.105, 0.105):
        p.cylinder(M.metal, (x, TAIL - 0.13, 0.30), 0.052, 0.15, segs=16, axis='Y')
        p.cylinder(M.lamp_h, (x, TAIL - 0.14, 0.30), 0.062, 0.05, segs=16, axis='Y')
elif V["exhaust"] == "hexquad":
    for x in (-0.325, -0.185, 0.185, 0.325):
        p.cylinder(M.metal, (x, TAIL - 0.11, 0.60), 0.047, 0.13, segs=6, axis='Y')
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
