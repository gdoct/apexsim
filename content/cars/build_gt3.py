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
SAMP = 6
BELT, ROOF = 5.35, 6.60                  # control indices where glass starts / ends
TRIM_J = 0.22                            # black surround under the glass, just above the belt crease (j=5)
TRIM_Y = 0.045                           # and either side of every glass edge (pillars, cowl)
SUNSTRIP = 0.11                          # tinted band along the top of the screen

VARIANTS = {
    "posh": dict(
        folder="posh-gt3rs", stem="posh_gt3rs", logo="posh_logo.png",
        paint=(0.02, 0.10, 0.42), accent=(0.86, 0.87, 0.90), caliper=(0.95, 0.75, 0.05),
        paint_metallic=0.78, number="91", roof_factor=0.94, hips=(0.085, 0.110),
        rear_number_y=(1.30, 1.62), cam_x=0.0, bonnet_drop=(0.075, 0.13),
        axles=(-1.375, 1.375),
        glass_y=(-0.92, 0.92), screen_y=(-0.95, -0.40), rear_glass_y=(0.50, 0.94),
        wing="pylon", wing_z=1.19, wing_y=1.86, wing_hw=0.86, ducktail=True,
        ends=dict(nose_top=0.545, nose_w=0.90, tail_top=0.905, tail_bot=0.30, tail_w=0.94),
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
        paint=(0.95, 0.66, 0.02), accent=(0.04, 0.04, 0.045), caliper=(0.05, 0.05, 0.05),
        paint_metallic=0.55, number="63", roof_factor=0.97, hips=(0.085, 0.115),
        rear_number_y=None, cam_x=-0.28, bonnet_drop=(0.035, 0.09),
        axles=(-1.375, 1.375),
        glass_y=(-0.98, 0.50), screen_y=(-1.00, -0.46), rear_glass_y=(0.30, 0.74),
        wing="swan", wing_z=1.14, wing_y=1.80, wing_hw=0.88, ducktail=False,
        ends=dict(nose_top=0.47, nose_w=0.92, tail_top=0.80, tail_bot=0.30, tail_w=0.95),
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
        paint=(0.56, 0.57, 0.60), accent=(0.0, 0.62, 0.60), caliper=(0.85, 0.10, 0.05),
        paint_metallic=0.90, number="88", roof_factor=0.93, hips=(0.070, 0.115),
        rear_number_y=(0.86, 1.18), cam_x=0.0,
        axles=(-1.375, 1.375),
        glass_y=(-0.84, 0.62), screen_y=(-0.86, -0.26), rear_glass_y=(0.38, 0.62),
        cabin_shift=0.38, logo_y=(-0.74, 0.40), fender_vent=(0.28, 0.54), bonnet_drop=(0.14, 0.25),
        wing="pylon", wing_z=1.16, wing_y=1.90, wing_hw=0.86, ducktail=False,
        ends=dict(nose_top=0.54, nose_w=0.88, tail_top=0.92, tail_bot=0.32, tail_w=0.93),
        lights="slant", grille=True, exhaust="side", mirror="pod",
        shutlines=[(-1.86, "upper"), (-0.90, "upper"), (-0.82, "side"), (0.44, "side"), (1.30, "upper"), (1.92, "upper")],
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
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]),
                         paint_metallic=V.get("paint_metallic", 0.8), paint_rough=0.22)

# ---------------------------------------------------------------- body loft
# Stance and silhouette. The hips rise more than the front fenders, the
# greenhouse is pulled down towards the belt and leaned in a little, so the
# cabin reads as a low canopy on wide hips rather than a bubble of the same
# width as the sills.
KEYS = V["keys"]
if V.get("cabin_shift"):
    KEYS = carlib.shift_upper(KEYS, V["cabin_shift"], j_from=6, y_from=-1.7, y_to=-1.1)
if V.get("bonnet_drop"):
    KEYS = carlib.drop_bonnet(KEYS, V["bonnet_drop"][0], V["bonnet_drop"][1],
                              V["screen_y"][0], V["keys"][1][0])
KEYS = carlib.fender_bump(KEYS, (AX_F, AX_R),
                          amount=V.get("hips", (0.085, 0.110)), width=(0.62, 0.72), j0=2.2, j1=4.3)
KEYS = carlib.lower_roof(KEYS, factor=V.get("roof_factor", 0.93), j_from=6, pivot_j=5)
KEYS = carlib.tumblehome(KEYS, amount=0.028, j_from=6)
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
L = carlib.Loft(KEYS, samp=SAMP, ny=110, hard=(2, 3, 4, 5))
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

FV0, FV1 = V.get("fender_vent", (0.44, 0.72))
if "fender" in V["vents"]:
    L.recess(AX_F + FV0, AX_F + FV1, 2.15, 3.30, depth=0.042, rim=0.022)
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
# (the headlamps are cut into the corner as apertures below, not recessed:
# the corner's upper surface faces up and inward, so a pocket there was a
# shelf whose lamps were seen edge-on from the road)
LAMP_X = (0.50, 0.84)
LAMP_Z = (E["nose_top"] - 0.145, E["nose_top"] - 0.030)
# tail lamp bands wrap the rear corner the same way
TLAMP_J = (3.0, 3.75)
TLAMP_Y0, TLAMP_Y1 = TAILP_Y - 0.20, TAILP_Y - 0.02
L.recess(TLAMP_Y0, TLAMP_Y1, TLAMP_J[0], TLAMP_J[1], depth=0.030, rim=0.014)


def face_mat(ym, kk, right):
    """Paint, glass, or the satin-black surround. Real GT glass sits in a
    black frame - a trim band from the belt crease up to the glass, the
    pillars, the cowl and a sunstrip across the top of the screen - and that
    frame is most of what separates a race car from a toy at ten metres."""
    jc = kk / SAMP
    s0, s1 = V["screen_y"]
    r0, r1 = V["rear_glass_y"]
    g0, g1 = V["glass_y"]
    is_screen = s0 < ym < s1
    is_rear = r0 < ym < r1
    is_side = g0 < ym < g1
    if jc < BELT - TRIM_J:
        return M.paint
    if jc < BELT:
        return M.trim if (is_screen or is_rear or is_side) else M.paint
    if is_screen:
        return M.trim if ym > s1 - SUNSTRIP else M.glass
    if is_rear:
        return M.glass
    if is_side and jc < ROOF:
        return M.glass
    if jc < ROOF + 0.5 and any(abs(ym - e) < TRIM_Y for e in (s0, s1, r0, r1, g0, g1)):
        return M.trim
    return M.paint


body = L.build("body", face_mat, [M.paint, M.glass, M.trim], subsurf=0)
save("body")

# -------------------------------------------------------------- wheel arches
parts_objs = []
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    for sx in (-1, 1):
        bead = carlib.arch(body, L, M, y, tyre, sx=sx, gap=ARCH_GAP,
                           x_in=0.53, z_bottom=0.125, bead=V.get("arch_bead", True))
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
# headlamps: a box through the nose corner, deep enough to open on the flank
# too, so the lamp wraps the corner as a GT3's does
LAMP_ZC = (LAMP_Z[0] + LAMP_Z[1]) / 2
LAMP_YB = carlib.surface_station(L, LAMP_X[1], LAMP_ZC, NOSE, margin=0.02) + 0.06
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_YB, LAMP_Z[1]))
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
# the client measures the eye off the exported mesh box: its top is the wing
# endplates or the aerial, whichever stands taller (the ducktail/roof pod never do)
TOP_Z = max(V["wing_z"] + 0.150, L.roof_z(0.92) + 0.22)
CK = carlib.cockpit_points(L, V["wing_z"], liner_z=0.043, top_z=TOP_Z)
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
    # upper canard, shorter chord, a hand above the first
    dy2 = carlib.surface_station(L, 0.78, 0.43, NOSE, margin=0.02)
    dx2 = L.x_at(dy2 + 0.08, 0.43)
    dp2 = [(dy2, 0.415), (dy2 + 0.20, 0.440), (dy2 + 0.20, 0.460), (dy2, 0.435)]
    carlib.plate(p, M.carbon, dp2, sx * (dx2 + 0.040), 0.090, chamfer=0.008)
for sx in (-1, 1):
    skirt = [(-1.00, 0.085), (1.00, 0.085), (1.00, 0.175), (-1.00, 0.165)]
    carlib.plate(p, M.carbon, skirt, sx * (SILL_X + 0.012), 0.032, chamfer=0.008)
    # accent stripe along the sill: the slot the client paints per team, so
    # every car has to carry it (docs/CAR_MODELS.md)
    stripe = [(-0.96, 0.190), (0.96, 0.190), (0.96, 0.270), (-0.96, 0.265)]
    carlib.plate(p, M.accent, stripe, sx * (SILL_X + 0.004), 0.008)
    # a fin standing on the end of the skirt, ahead of the rear arch
    fin = [(0.78, 0.085), (1.00, 0.085), (1.00, 0.300), (0.86, 0.250)]
    carlib.plate(p, M.carbon, fin, sx * (SILL_X + 0.040), 0.010, chamfer=0.004)
# diffuser: tucked under the tail, ramping up into the bodywork
DIF_Y0, DIF_Y1 = AX_R + 0.22, TAIL - 0.02
DIF_HW = min(L.x_at(y, 0.19) for y in
             (DIF_Y0, (DIF_Y0 + DIF_Y1) / 2, DIF_Y1 - 0.05)) - 0.050
carlib.diffuser(p, M.carbon, DIF_Y0, DIF_Y1, DIF_HW, 0.070, VAL_Z[0] + 0.02,
                thick=0.014, strakes=(-0.70, -0.42, -0.14, 0.14, 0.42, 0.70), strake_h=0.190)

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

# ---- lights. Each headlamp is a pocket in the nose corner with a black
# floor, a trim bezel round its rim, projector modules (chrome bezel, dark
# bowl, lit ring, lit core) standing on the floor, the car's DRL signature
# as a lit light guide, and a clear lens flush with the paint over the lot.
# The tail is a wrap-around corner pocket with a light-guide pair, a brake
# block on the panel, and (Posh, Murcetes) a full-width light bar.
def nose_at(x, z, back=0.0):
    """The station where the skin passes (x, z) on the nose, `back` inside it."""
    return carlib.surface_station(L, abs(x), z, NOSE, step=0.008, margin=0.004) + back


LAMP_AX = lambda sx: Vector((sx * 0.22, -0.95, 0.10)).normalized()
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    xo, xi = (x1, x0) if sx > 0 else (x0, x1)          # outboard / inboard edge
    z0, z1 = LAMP_Z
    zc = LAMP_ZC
    p.box(M.lamp_h, (x0, LAMP_YB - 0.022, z0), (x1, LAMP_YB - 0.002, z1))      # back wall
    if V["lights"] == "round":
        # Posh: one large projector, four DRL points round it
        cx = xi + sx * 0.15
        carlib.projector(p, M, (cx, nose_at(cx + sx * 0.05, zc, 0.030), zc), LAMP_AX(sx), 0.046, depth=0.045)
        for (dx, dz) in ((0.075, 0.036), (0.075, -0.036), (-0.075, 0.036), (-0.075, -0.036)):
            px = cx + sx * dx
            carlib.projector(p, M, (px, nose_at(px, zc + dz, 0.024), zc + dz), LAMP_AX(sx), 0.011,
                             depth=0.018, chrome=False)
    elif V["lights"] == "ybar":
        # Limbotiti: two projectors with a Y-shaped guide meeting between them
        for k, dx in enumerate((0.07, 0.20)):
            px = xi + sx * dx
            carlib.projector(p, M, (px, nose_at(px + sx * 0.03, zc, 0.030), zc), LAMP_AX(sx), 0.030, depth=0.045)
        hub = (xi + sx * 0.135, nose_at(xi + sx * 0.135, zc, 0.020), zc)
        for (tx, tz) in ((xi + sx * 0.02, z1 - 0.018), (xo - sx * 0.02, z1 - 0.018), (xi + sx * 0.135, z0 + 0.018)):
            carlib.guide_xyz(p, M.lamp, [hub, (tx, nose_at(tx, tz, 0.020), tz)], r=0.0055, segs=8)
    else:
        # Murcetes: three projectors in a row under a hockey-stick DRL along the top
        for k in range(3):
            px = xi + sx * (0.06 + 0.11 * k)
            carlib.projector(p, M, (px, nose_at(px + sx * 0.03, zc - 0.012, 0.030), zc - 0.012),
                             LAMP_AX(sx), 0.027, depth=0.045)
        pts = []
        for t in (0.0, 0.25, 0.5, 0.75, 1.0):
            px = xi + sx * (0.02 + (abs(xo - xi) - 0.04) * t)
            pts.append((px, nose_at(px, z1 - 0.016, 0.020), z1 - 0.016))
        pts.append((xo - sx * 0.02, nose_at(xo - sx * 0.02, zc, 0.020), zc))
        carlib.guide_xyz(p, M.lamp, pts, r=0.0055, segs=8)
    carlib.front_lens(p, M.glass, L, x0 - 0.004, x1 + 0.004, z0 - 0.004, z1 + 0.004, NOSE,
                      lift=-0.0025, nu=10, nv=4, margin=0.0)

    # tail corner pocket: black floor, a pair of light guides wrapping the
    # corner, a smoked lens; the brake block sits on the tail panel below
    carlib.pocket_floor(p, M.lamp_h, L, TLAMP_Y0 + 0.006, TLAMP_Y1 - 0.006,
                        TLAMP_J[0] + 0.06, TLAMP_J[1] - 0.06, sx=sx, depth=0.030)
    carlib.pocket_bezel(p, M.trim, L, TLAMP_Y0 - 0.004, TLAMP_Y1 + 0.004,
                        TLAMP_J[0] - 0.04, TLAMP_J[1] + 0.04, sx=sx, w_y=0.010, w_j=0.08)
    TJM = (TLAMP_J[0] + TLAMP_J[1]) / 2
    for dj in (0.16, -0.16):
        carlib.light_guide(p, M.tail, L, [(TLAMP_Y0 + 0.016, TJM + dj), (TLAMP_Y1 - 0.016, TJM + dj)],
                           sx=sx, lift=-0.030 + 0.010, r=0.0055, per=6)
    carlib.conform_decal(p, M.lens_tint, L, TLAMP_Y0 + 0.010, TLAMP_Y1 - 0.010,
                         TLAMP_J[0] + 0.09, TLAMP_J[1] - 0.09, sx=sx, lift=-0.0025, nu=6, nv=3)
    bz = E["tail_top"] - 0.20
    bx = L.x_at(TAILP_Y, bz) - 0.05
    b0, b1 = sorted((sx * (bx - 0.22), sx * (bx - 0.02)))
    carlib.led_grid(p, M, b0, b1, bz - 0.032, bz + 0.004, TAIL - 0.004, dir_y=-1.0,
                    cols=5, rows=2, glow=M.brake)
if V["lights"] in ("round", "slant"):
    # full-width light bar across the tail panel, the running light
    bz = E["tail_top"] - 0.20
    bx = L.x_at(TAILP_Y, bz + 0.06) - 0.06
    # the tip station is pulled in behind the panel face, so the bar sits a
    # few millimetres proud of the tip - as a real light bar does
    carlib.tail_bar(p, M, -bx, bx, TAIL + 0.004, bz + 0.065, h=0.046, glow=M.tail, dir_y=-1.0)
else:
    for sx in (-1, 1):
        bz = E["tail_top"] - 0.20
        bx = L.x_at(TAILP_Y, bz) - 0.05
        b0, b1 = sorted((sx * 0.30, sx * (bx - 0.02)))
        carlib.tail_bar(p, M, b0, b1, TAIL + 0.004, bz + 0.065, h=0.040, glow=M.tail, dir_y=-1.0)
# FIA rain light: an LED matrix behind a clear lens in its aperture
carlib.led_grid(p, M, -0.10, 0.10, RAIN_Z[0] + 0.012, RAIN_Z[1] - 0.012, TAIL - 0.016, dir_y=-1.0,
                cols=6, rows=3, glow=M.rain)
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
        yv0, yv1 = AX_F + FV0 + 0.02, AX_F + FV1 - 0.02
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
wy = V["screen_y"][0] - 0.02
p.bar(M.trim, (DX - 0.44, wy, L.z_at(wy, 0.30) + 0.010),
      (DX + 0.26, wy + 0.03, L.z_at(wy + 0.03, 0.30) + 0.010), 0.009, segs=8)

# ---- small hardware: door pulls, bonnet pins, filler, roof camera pod
SIDE_SHUTS = sorted(y for (y, k) in V["shutlines"] if k == "side")
UPPER_SHUTS = sorted(y for (y, k) in V["shutlines"] if k == "upper")
if len(SIDE_SHUTS) >= 2:
    hy = SIDE_SHUTS[-1] - 0.17
    hz = L.point(hy, 4.6).z
    for sx in (-1, 1):
        hx = L.x_at(hy, hz)
        p.box(M.trim, (min(sx * (hx - 0.010), sx * (hx + 0.003)), hy - 0.06, hz - 0.016),
              (max(sx * (hx - 0.010), sx * (hx + 0.003)), hy + 0.06, hz + 0.016))
        p.box(M.metal, (min(sx * (hx - 0.004), sx * (hx + 0.004)), hy - 0.05, hz - 0.006),
              (max(sx * (hx - 0.004), sx * (hx + 0.004)), hy + 0.05, hz + 0.006))
for x in (-0.36, 0.36):
    py = UPPER_SHUTS[0] + 0.07
    pz = L.z_at(py, abs(x))
    p.cylinder(M.metal, (x, py, pz - 0.004), 0.016, 0.010, segs=12, axis='Z')
    p.cylinder(M.trim, (x, py, pz + 0.006), 0.007, 0.006, segs=8, axis='Z')
fy = AX_R - 0.50
fz = L.z_at(fy, 0.68)
p.cylinder(M.metal, (0.68, fy, fz - 0.008), 0.046, 0.014, segs=16, axis='Z')
p.cylinder(M.trim, (0.68, fy, fz + 0.006), 0.036, 0.004, segs=16, axis='Z')
cy = V["screen_y"][1] + 0.12
cx = V.get("cam_x", 0.0)
cz = L.z_at(cy, abs(cx))
p.box(M.trim, (cx - 0.045, cy - 0.06, cz - 0.01), (cx + 0.045, cy + 0.06, cz + 0.035))
p.cylinder(M.glass, (cx, cy + 0.06, cz + 0.018), 0.012, 0.006, segs=10, axis='Y')

# ---- race numbers: a white plate with the number on the bonnet, and on the
# deck where the deck is free (the Limbotiti's carries louvres)
NUM = V.get("number", "1")
carlib.top_patch(p, M.decal, L, NOSE + 0.34, NOSE + 0.76, -0.23, 0.23, lift=0.004, nu=8, nv=6)
carlib.top_text(p, M.trim, L, NUM, 0.0, NOSE + 0.55, size=0.30, thick=0.006, lift=0.005,
                face="front", squash=0.80)
if V.get("rear_number_y"):
    ry0, ry1 = V["rear_number_y"]
    carlib.top_patch(p, M.decal, L, ry0, ry1, -0.21, 0.21, lift=0.004, nu=6, nv=6)
    carlib.top_text(p, M.trim, L, NUM, 0.0, (ry0 + ry1) / 2, size=0.26, thick=0.006, lift=0.005,
                    face="rear", squash=0.80)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
# The client's cockpit rig draws the steering wheel and its display at the
# derived wheel point, so the mesh carries no wheel of its own (two wheels
# a few centimetres apart is what the first generation showed). Everything
# else the driver sees from the seat is here: cowl and cluster hood, a
# padded door either side, a bucket with its harness, the tunnel with the
# switch panel, a headliner under the cage and a bulkhead behind the seat.
XIN = min(L.x_at(y, DASH_Z) for y in (-0.66, -0.3, 0.0, 0.3, 0.64)) - 0.055   # inside face of the door cards
BELT_Z = L.point(0.0, 5.0).z                    # the belt crease at the B-pillar
HOOP_Y = 0.96                                   # main hoop: just behind the seat back
BULK_Y = HOOP_Y + 0.03
DOOR0, DOOR1 = SIDE_SHUTS[0] + 0.03, SIDE_SHUTS[-1] - 0.03
p.box(M.interior, (-XIN, -0.80, 0.115), (XIN, BULK_Y, 0.155))                 # floor
p.box(M.alcantara, (-XIN + 0.02, -0.60, 0.155), (XIN - 0.02, 0.10, 0.160))   # footwell mats
p.box(M.interior, (-0.17, -0.80, 0.155), (0.17, BULK_Y, 0.31))               # tunnel
p.box(M.trim, (-0.19, -0.80, 0.31), (0.19, BULK_Y, 0.325))                    # tunnel top
carlib.switch_panel(p, M, -0.15, 0.15, CK["wheel"].y - 0.20, CK["wheel"].y + 0.22, 0.335,
                    rows=3, cols=4, rotary=True)
p.box(M.interior, (-XIN, BULK_Y, 0.155), (XIN, BULK_Y + 0.03, 0.60))          # bulkhead
p.box(M.trim, (-XIN, BULK_Y - 0.01, 0.60), (XIN, BULK_Y + 0.04, 0.63))
# dash: cowl top across the cabin under the screen, a cluster hood ahead of
# the driver, a padded roll along the front edge, a flat carbon panel on
# the passenger side and a small data display on the centre
# The dash face sits a forearm beyond the wheel (the client's wheel point is
# 40 cm ahead of the eye; the cowl is ~70 cm), and its top a hand under the
# eye line: a dash any closer or taller fills the cockpit view with felt.
DASH_Z = EYE.z - 0.22
DASH_Y0, DASH_Y1 = -0.80, CK["wheel"].y - 0.28
p.box(M.interior, (-XIN, DASH_Y0, 0.56), (XIN, DASH_Y1, DASH_Z - 0.06))
p.box(M.alcantara, (-XIN, DASH_Y0, DASH_Z - 0.06), (XIN, DASH_Y1, DASH_Z))
p.bar(M.alcantara, (-XIN, DASH_Y1, DASH_Z - 0.016), (XIN, DASH_Y1, DASH_Z - 0.016), 0.016, segs=10)
hood = [(DASH_Y1 - 0.30, DASH_Z), (DASH_Y1 + 0.02, DASH_Z), (DASH_Y1 + 0.02, DASH_Z + 0.045),
        (DASH_Y1 - 0.24, DASH_Z + 0.045)]
carlib.plate(p, M.alcantara, hood, DX, 0.34, chamfer=0.010)
p.box(M.carbon, (-XIN + 0.06, DASH_Y1 - 0.02, 0.60), (-0.20, DASH_Y1 + 0.005, DASH_Z - 0.08))
p.box(M.trim, (-0.10, DASH_Y1 - 0.06, DASH_Z), (0.10, DASH_Y1 - 0.01, DASH_Z + 0.045))
p.box(M.display, (-0.085, DASH_Y1 - 0.012, DASH_Z + 0.008), (0.085, DASH_Y1 - 0.008, DASH_Z + 0.040))
carlib.switch_panel(p, M, DX - 0.42, DX - 0.20, DASH_Y1 - 0.12, DASH_Y1 - 0.02, DASH_Z + 0.001,
                    rows=1, cols=3, rotary=False)
# glovebox-side kick panels and the door cards
for sx in (-1, 1):
    carlib.door_card(p, M, sx, XIN, DOOR0, DOOR1, 0.20, BELT_Z - 0.035, pull=True)
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), DASH_Y0, 0.155),
          (max(sx * XIN, sx * (XIN + 0.02)), DOOR0, 0.56))                      # A-post kick
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), DOOR1, 0.155),
          (max(sx * XIN, sx * (XIN + 0.02)), BULK_Y + 0.03, 0.60))              # rear quarter
# seat and harness, pedals, footrest, extinguisher on the passenger floor
carlib.bucket_seat(p, M, DX, 0.12, 0.155, width=0.50, depth=0.52, back_h=0.64, rake_deg=22.0)
for x in (DX - 0.20, DX - 0.08, DX + 0.04):
    p.box(M.metal, (x - 0.032, -0.56, 0.165), (x + 0.032, -0.50, 0.30))           # pedals
p.box(M.metal, (DX + 0.14, -0.58, 0.165), (DX + 0.22, -0.48, 0.28))               # dead pedal
carlib.extinguisher(p, M, -0.48, -0.30, 0.225)
p.bar(M.alc, (DX - 0.36, DASH_Y1 - 0.22, DASH_Z - 0.10), (DX - 0.36, DASH_Y1 + 0.02, DASH_Z - 0.10),
      0.008, segs=6)                                                            # stalk
# headliner, hung under the roof between the screen and the rear glass
carlib.inner_skin(p, M.alcantara, L, V["screen_y"][1] + 0.03, V["rear_glass_y"][0] - 0.03, 0.56,
                  drop=0.032, nu=10, nv=6)
# roll cage: main hoop, roof rails, A-pillar bars, door bars, rear stays
# Every cage node is taken off the loft and pulled inboard, so the bars stay
# under the skin. Hard-coded heights put the hoop through the roof on a low
# car and on the floor on a tall one.
def inside(y, x, drop=0.055, pull=0.055):
    """A point just inside the upper surface at half-width `x`."""
    return (x, y, L.z_at(y, abs(x) + pull) - drop)


SCR_Y = V["screen_y"][1]
PY = V["screen_y"][0] + 0.06
for sx in (-1, 1):
    hoop_top = inside(HOOP_Y, sx * 0.54)
    # the A-pillar bar runs down the edge of the screen, not across the
    # driver's face: its top is at the roof's outer edge
    rail = inside(SCR_Y, sx * 0.60)
    foot = (sx * (L.x_at(PY, 0.72) - 0.075), PY, L.z_at(PY, 0.74) - 0.045)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.21), (sx * 0.70, HOOP_Y, hoop_top[2] - 0.10),
          0.023, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, hoop_top[2] - 0.10), hoop_top, 0.023, segs=8)
    p.bar(M.cage, hoop_top, rail, 0.021, segs=8)
    mid_y = (SCR_Y + PY) / 2
    mid = (sx * (L.x_at(mid_y, 0.86) - 0.070), mid_y, L.z_at(mid_y, 0.66) - 0.050)
    p.bar(M.cage, rail, mid, 0.019, segs=8)
    p.bar(M.cage, mid, foot, 0.019, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, hoop_top[2] - 0.16), (sx * 0.56, AX_R - 0.10, 0.54),
          0.020, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.56), (sx * 0.68, -0.52, 0.60), 0.020, segs=8)
    p.bar(M.cage, (sx * 0.70, HOOP_Y, 0.30), (sx * 0.68, -0.50, 0.34), 0.017, segs=8)
p.bar(M.cage, inside(HOOP_Y, -0.54), inside(HOOP_Y, 0.54), 0.023, segs=8)
p.bar(M.cage, (-0.70, HOOP_Y, 0.58), (0.70, HOOP_Y, 0.58), 0.019, segs=8)

# ---- wordmark, laid on the flank rather than floated off it
LG0, LG1 = V.get("logo_y", (-0.52, 0.62))
carlib.conform_decal(p, M.logo, L, LG0, LG1, 2.05, 3.05, sx=1, lift=0.005, nu=14, nv=6)
carlib.conform_decal(p, M.logo, L, LG0, LG1, 2.05, 3.05, sx=-1, lift=0.005, nu=14, nv=6,
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
print("sightline:", carlib.sightline(car))
if glb:
    print("GLB:", glb, os.path.getsize(glb))
