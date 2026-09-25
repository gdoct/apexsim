"""Build one of the Hypercar (LMH) prototypes in Blender and export it to
content/cars/<folder>/<stem>.glb.

    VARIANT = "panini"     # panini | fugazzi | bugotti
    exec(open(r"D:\\apexsim\\content\\cars\\build_hypercar.py").read())

Same pipeline as build_lmp2.py (shape data here, mechanics in carlib.py), a
different hull. A hypercar is longer, wider and lower than an LMP2 and the
look is the opposite of the LMP2's pods: a low, pointed nose between two
tall floating fenders, a deep valley either side of a narrow teardrop
canopy, a long tail and a high wing that stands on its endplates off the
rear fenders instead of on swan necks. Lamps are slits, not boxes.

Frame: nose on -Y, tail on +Y, ground z = 0, metres, left-hand drive (driver
on +X). Exported with glTF +Y up, like every car in content/cars.

Stance (class-wide, matched by each car.toml's [wheels] table): a 0.360 m
tyre all round on a 1.62 m track inside a ~2.0 m body, arch cut 20 mm clear
of the tyre, wheelbase 3.1 m.

Section control points, floor centre outwards and up to the roof centre:
    0 floor centre        3 fender crown (HARD)   6 canopy side
    1 floor edge          4 fender inner          7 canopy upper
    2 sill (HARD)         5 valley (HARD)         8 roof centre
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

# ------------------------------------------------------------------ loading
_ROOT = os.environ.get("APEXSIM_ROOT", r"D:\apexsim")
for _n, _p in (("apex", os.path.join(_ROOT, "content", "props", "_tools", "apex_props.py")),
               ("carlib", os.path.join(_ROOT, "content", "cars", "carlib.py"))):
    _s = importlib.util.spec_from_file_location(_n, _p)
    _m = importlib.util.module_from_spec(_s)
    sys.modules[_n] = _m
    _s.loader.exec_module(_m)
import apex, carlib                                        # noqa: E402
from carlib import Builder                                  # noqa: E402

# ------------------------------------------------------------------ class kit
TYRE_F, TYRE_R = 0.360, 0.360
TRACK = 1.620
HUB_X = TRACK / 2.0
ARCH_GAP = 0.020
SAMP = 6
AX_F, AX_R = -1.550, 1.550        # wheelbase 3.1 m, as [physics] says
CAN_LO, CAN_HI = 5.75, 7.45
TRIM_J = 0.24
TRIM_Y = 0.040
SUN_J = 0.40

# Shape factors, as build_lmp2.py: nose_w (tip width), fender (crown height
# over the axles), roof (canopy height), canopy_shift (cabin along the car),
# tail_h, wing_z. Livery: paint/accent/caliper/seat. Signatures: lights
# (headlamp slit), drl, tail, exhaust, scoop, grille.
VARIANTS = {
    # Panini Zomba: the carbon one. Four round tail lamps, the quad exhaust
    # in the middle of the tail, a roof snorkel, stacked round headlamps.
    "panini": dict(folder="panini-zomba-hypercar", stem="panini_zomba", logo="panini_logo.png",
                   paint=(0.020, 0.045, 0.110), accent=(0.78, 0.79, 0.82), caliper=(0.85, 0.86, 0.88),
                   paint_metallic=0.85, number="9", bonnet_drop=(0.015, 0.035),
                   lights="round", drl="points", tail="rings", exhaust="quad", grille="twin",
                   nose_w=0.94, fender=1.02, roof=1.00, canopy_shift=0.04, tail_h=1.00, wing_z=0.02,
                   mirror="stalk", scoop=(0.22, 0.60, 0.13), seat=(0.36, 0.17, 0.06), rain_z=(0.420, 0.550),
                   # upright and square: a raised nose, shallow valleys, a wide
                   # canopy, fenders that stand like boxes over the wheels
                   nose_z=0.060, valley=0.065, canopy_w=1.07, fender_r=1.00, face="twin", side="upright",
                   # a straight plane on square endplates, brake strip along the flap
                   wing_plan=("straight", 0.0), endplate="square", wing_led="trail"),
    # Fugazzi 994P: rosso with a yellow sill, a single thin blade over a slit
    # headlamp, a thin double stripe at the back. The lowest and sharpest.
    "fugazzi": dict(folder="fugazzi-994p-hypercar", stem="fugazzi_994p", logo="fugazzi_hyper_logo.png",
                    paint=(0.60, 0.015, 0.025), accent=(0.96, 0.78, 0.04), caliper=(0.96, 0.80, 0.05),
                    paint_metallic=0.60, number="50", bonnet_drop=(0.015, 0.035),
                    lights="slit", drl="blade", tail="double", exhaust="twin", grille="twin",
                    nose_w=0.88, fender=1.05, roof=0.96, canopy_shift=0.10, tail_h=0.96, wing_z=0.00,
                    mirror="stalk", scoop=None, seat=(0.16, 0.04, 0.04),
                    # low and pointed: the nose drooped, the valleys cut deep
                    # either side of a narrow canopy, rear fenders swept up
                    nose_z=-0.030, valley=-0.055, canopy_w=0.95, fender_r=1.07, face="boomerang",
                    side="slash",
                    # a spoon-shaped plane between endplates that rise to a
                    # swept-back horn, the brake light up their trailing edges
                    wing_plan=("spoon", 0.055), endplate="horn", wing_led="endplate"),
    # Bugotti Chiffon: two-tone French blue, the horseshoe grille, the C-line
    # round the side intake, a quad-lamp bar and one light bar across the tail.
    "bugotti": dict(folder="bugotti-chiffon-hypercar", stem="bugotti_chiffon", logo="bugotti_logo.png",
                    paint=(0.015, 0.085, 0.42), accent=(0.004, 0.006, 0.018), accent_metallic=0.25, caliper=(0.05, 0.25, 0.80),
                    paint_metallic=0.80, number="16", bonnet_drop=(0.015, 0.035),
                    lights="quad", drl="under", tail="fullbar", exhaust="twin", grille="horseshoe",
                    nose_w=1.00, fender=1.00, roof=1.02, canopy_shift=0.00, tail_h=1.02, wing_z=0.03,
                    mirror="pod", scoop=None, seat=(0.04, 0.10, 0.30), cline=True, two_tone=True, rain_z=(0.285, 0.430),
                    # round and full: bulbous fenders, a filled-in valley, the
                    # C-shaped intake the polished line wraps
                    nose_z=0.010, valley=0.020, canopy_w=1.00, fender_r=0.98, fender_x=0.025,
                    face="horseshoe", side="c",
                    # an arched plane, endplates with a rounded top rolling
                    # over it, brake light across the middle
                    wing_plan=("arch", 0.040), endplate="roll", wing_led="centre"),
}

# Base hull. Right-half section control points (x, z).
KEYS = [
    (-2.52, [(0, .100), (.46, .100), (.53, .140), (.55, .200), (.44, .235), (.30, .245), (.18, .250), (.08, .255), (0, .260)]),
    (-2.30, [(0, .060), (.80, .060), (.885, .180), (.90, .420), (.76, .490), (.53, .370), (.31, .330), (.14, .330), (0, .330)]),
    (-1.90, [(0, .050), (.95, .050), (.99, .380), (1.00, .720), (.83, .800), (.62, .640), (.37, .490), (.16, .465), (0, .460)]),
    (-1.55, [(0, .050), (.965, .050), (1.00, .420), (1.005, .830), (.82, .890), (.63, .770), (.38, .580), (.17, .530), (0, .515)]),
    (-1.12, [(0, .050), (.975, .050), (1.00, .400), (.99, .720), (.81, .770), (.62, .640), (.42, .630), (.21, .650), (0, .655)]),
    (-0.62, [(0, .050), (.975, .050), (.99, .380), (.975, .600), (.80, .630), (.65, .580), (.47, .810), (.27, .935), (0, .965)]),
    (-0.10, [(0, .050), (.975, .050), (.99, .380), (.975, .590), (.80, .615), (.66, .585), (.48, .925), (.29, 1.020), (0, 1.040)]),
    (0.45, [(0, .050), (.975, .050), (.99, .380), (.975, .600), (.80, .630), (.66, .610), (.47, .910), (.28, 1.000), (0, 1.020)]),
    (1.02, [(0, .050), (.975, .050), (1.00, .400), (.99, .740), (.81, .800), (.63, .720), (.41, .800), (.21, .860), (0, .875)]),
    (1.55, [(0, .050), (.965, .050), (1.00, .430), (1.005, .850), (.81, .900), (.62, .800), (.36, .760), (.18, .770), (0, .780)]),
    (1.98, [(0, .070), (.95, .070), (.985, .400), (.975, .760), (.78, .800), (.59, .720), (.34, .690), (.16, .685), (0, .690)]),
    (2.46, [(0, .230), (.84, .230), (.90, .400), (.89, .600), (.72, .635), (.54, .610), (.31, .590), (.14, .585), (0, .585)]),
]


def apply_variant(keys, v):
    out = []
    for (y, pts) in keys:
        new = []
        for j, (x, z) in enumerate(pts):
            if y < -1.9:
                x *= v["nose_w"]
            if j in (3, 4) and abs(abs(y) - 1.55) < 0.45:
                z = 0.050 + (z - 0.050) * v["fender"]
            if j >= 6 and -0.7 < y < 1.1:
                z = 0.60 + (z - 0.60) * v["roof"]
            if y > 1.7:
                z *= v["tail_h"]
            # the variant's own sculpture on top of the shared proportions
            if y < -1.7 and j >= 2:
                z += v.get("nose_z", 0.0) * min(1.0, (-1.7 - y) / 0.6)
            if j in (4, 5) and -2.0 < y < 1.95:
                z += v.get("valley", 0.0) * (1.0 if j == 5 else 0.5)
            if j in (6, 7) and -0.7 < y < 1.1:
                x *= v.get("canopy_w", 1.0)
            if j in (3, 4) and abs(y - 1.55) < 0.45:
                z = 0.050 + (z - 0.050) * v.get("fender_r", 1.0)
            if j in (2, 3) and abs(abs(y) - 1.55) < 0.50:
                x += v.get("fender_x", 0.0)
            new.append((x, z))
        yy = y + (v["canopy_shift"] if -0.7 <= y <= 1.0 else 0.0)
        out.append((yy, new))
    return out


try:
    VARIANT
except NameError:
    VARIANT = "fugazzi"
V = VARIANTS[VARIANT]
CAR_DIR = os.path.join(carlib.CARS_ROOT, V["folder"])
os.makedirs(os.path.join(CAR_DIR, "textures"), exist_ok=True)
CS = V["canopy_shift"]
SCREEN_Y = (-1.00 + CS, -0.30 + CS)
SIDE_Y = (-0.30 + CS, 0.58 + CS)
CANOPY_Y = (SCREEN_Y[0], SIDE_Y[1])
NOSE_KEY = KEYS[1][0]


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


# ---------------------------------------------------------------- materials
carlib.reset_scene()
M = carlib.car_materials(V["paint"], V["accent"], V["caliper"],
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]),
                         seat_rgb=V["seat"], paint_metallic=V.get("paint_metallic", 0.8),
                         paint_rough=0.20, accent_metallic=V.get("accent_metallic", 0.55))

# ---------------------------------------------------------------- body loft
VK = carlib.fender_bump(apply_variant(KEYS, V), (AX_F, AX_R),
                        amount=0.020, width=0.58, j0=2.4, j1=4.0)
if V.get("bonnet_drop"):
    VK = carlib.drop_bonnet(VK, V["bonnet_drop"][0], V["bonnet_drop"][1], SCREEN_Y[0], NOSE_KEY,
                            fade=0.20, j_full=7, partial=((6, 0.5),))
L = carlib.Loft(VK, samp=SAMP, ny=124, hard=(2, 3, 5))
NOSE, TAIL = L.nose, L.tail

ARCH_SPAN = [(AX_F - TYRE_F - ARCH_GAP, AX_F + TYRE_F + ARCH_GAP),
             (AX_R - TYRE_R - ARCH_GAP, AX_R + TYRE_R + ARCH_GAP)]


def in_arch(y):
    return any(a <= y <= b for (a, b) in ARCH_SPAN)


# Shut lines: nose clam, door, engine cover. Fewer than the LMP2: the
# surfaces are what sells a hypercar, so they are kept long and unbroken.
for (y, kind) in ((NOSE + 0.70, "upper"), (AX_F + 0.44, "side"),
                  (SIDE_Y[1] + 0.06, "side"), (AX_R + 0.46, "upper")):
    if kind == "upper":
        L.groove(y, 4.2, 8.0, depth=0.008, width=0.012)
    elif not in_arch(y):
        L.groove(y, 1.4, 3.2, depth=0.008, width=0.012)

# The flank, per car (swept vents, character lines, see build_gt3.py): the
# first cut gave all three the same upright box intake behind the door and
# the same brake exit behind the front wheel.
S1 = SIDE_Y[1]
SIDES = {
    # upright: a tall intake leaning slightly forward, a square brake exit,
    # louvres on top of the front fenders
    "upright": [
        ("recess", dict(y0=(S1 + 0.14, S1 + 0.10), y1=(S1 + 0.52, S1 + 0.50), j0=1.80, j1=3.30,
                        depth=0.070, rim=0.035, blades=4)),
        ("recess", dict(y0=AX_F + 0.46, y1=AX_F + 0.70, j0=1.95, j1=3.00, depth=0.035, rim=0.024,
                        blades=3, blade_r=0.006)),
        ("recess", dict(y0=AX_F - 0.16, y1=AX_F + 0.16, j0=3.30, j1=4.25, depth=0.020, rim=0.065,
                        louvres=True)),
    ],
    # slash: one long intake raked hard back from the sill to the hip, a thin
    # brake slot leaning the other way, a crease running back into the intake
    "slash": [
        ("recess", dict(y0=(S1 + 0.08, S1 + 0.42), y1=(S1 + 0.46, S1 + 0.62), j0=1.75, j1=3.35,
                        depth=0.075, rim=0.055, bow=-0.03, blades=3, blade_r=0.008)),
        ("recess", dict(y0=(AX_F + 0.62, AX_F + 0.46), y1=(AX_F + 0.72, AX_F + 0.58), j0=1.95, j1=3.10,
                        depth=0.035, rim=0.018)),
        ("swage", dict(y0=AX_F + 0.50, y1=S1 + 0.40, j_a=3.15, j_b=3.35, j_mid=3.05, depth=0.010,
                       width=0.30, fade=0.25)),
    ],
    # c: an intake whose edges bow forward at mid-height, the polished C-line
    # following its leading edge; a small upright vent behind the front wheel
    "c": [
        ("recess", dict(y0=S1 + 0.12, y1=S1 + 0.50, j0=1.70, j1=3.40, depth=0.070, rim=0.035,
                        bow=-0.09, blades=4, blade_r=0.006)),
        ("recess", dict(y0=(AX_F + 0.46, AX_F + 0.50), y1=(AX_F + 0.60, AX_F + 0.64), j0=2.05, j1=2.95,
                        depth=0.030, rim=0.016)),
    ],
}
SIDE_BLADES, SIDE_FLOORS, LOUVRES = [], [], []
for (kind, kw) in SIDES[V["side"]]:
    kw = dict(kw)
    blades, blade_r = kw.pop("blades", 0), kw.pop("blade_r", 0.007)
    louvres = kw.pop("louvres", False)
    if kind == "swage":
        L.swage(**kw)
        continue
    L.recess(**kw)
    f = L.last_feature
    if louvres:
        LOUVRES.append(f)
    if f["swept"] and kw["depth"] > 0.022:
        SIDE_FLOORS.append(f)
    if blades:
        SIDE_BLADES.append((f, blades, blade_r))
# The valley between each front fender and the cockpit, carved deeper.
L.recess(AX_F + 0.30, -0.35 + CS, 4.70, 5.90, depth=0.035, rim=0.14)

# Headlamp: a slit across the front face of each fender, above the mouths;
# lower and further inboard it would break out through the low nose deck.
LIGHTS = {  # x span, z0, height
    "round": ((0.56, 0.84), 0.290, 0.075),
    "slit": ((0.54, 0.86), 0.315, 0.045),
    "quad": ((0.54, 0.86), 0.300, 0.055),
}[V["lights"]]
LAMP_X = LIGHTS[0]
LAMP_Z = (LIGHTS[1], LIGHTS[1] + LIGHTS[2])
LAMP_Y = carlib.surface_station(L, LAMP_X[1], (LAMP_Z[0] + LAMP_Z[1]) / 2, NOSE, margin=0.070)
TAILL_Y = carlib.surface_station(L, 0.74, 0.55, TAIL, margin=0.03)
TAILL_TOP = min(L.roof_z(TAILL_Y) - 0.070, 0.62)
TAILL_H = {"rings": 0.110, "double": 0.060, "fullbar": 0.060}[V["tail"]]
RAIN_Z = V.get("rain_z", (0.400, 0.545))
RAIN_Y = carlib.surface_station(L, 0.10, (RAIN_Z[0] + RAIN_Z[1]) / 2, TAIL, margin=0.05)

L.recess(NOSE + 0.02, LAMP_Y + 0.30, 1.55, 4.60, depth=0.024, rim=0.15)
if V["tail"] != "fullbar":
    L.recess(TAILL_Y - 0.30, TAIL - 0.02, 2.65, 4.30, depth=0.020, rim=0.11)
L.recess(RAIN_Y - 0.18, TAIL - 0.02, 3.40, 4.80, depth=0.016, rim=0.055)


# Two-tone (Bugotti): everything above the fender's inner shoulder - the
# valleys, the canopy frame, the decks - in the dark accent, the fenders and
# flanks in the body colour.
TWO_TONE_J = 4.35 if V.get("two_tone") else 99.0
TOP = M.accent if V.get("two_tone") else M.paint


def face_mat(ym, kk, right):
    jc = kk / SAMP
    is_screen = SCREEN_Y[0] < ym < SCREEN_Y[1]
    is_side = SIDE_Y[0] <= ym < SIDE_Y[1]
    if jc < CAN_LO - TRIM_J:
        if jc < 3.6 and L._offset(ym, kk, right, skip_swept=True) > 0.030:
            return M.mesh
        return TOP if jc > TWO_TONE_J else M.paint
    if jc < CAN_LO:
        return M.trim if (is_screen or is_side) else TOP
    if is_screen:
        return M.trim if jc > CAN_HI + SUN_J else M.glass
    if is_side and jc < CAN_HI:
        return M.glass
    if jc < CAN_HI + 0.4 and any(abs(ym - e) < TRIM_Y for e in (SCREEN_Y[0], SIDE_Y[1])):
        return M.trim
    return TOP


body = L.build("body", face_mat, [M.paint, M.glass, M.trim, M.mesh] + ([M.accent] if V.get("two_tone") else []),
               subsurf=0)
save("body")

# -------------------------------------------------------------- wheel arches
parts_objs = []
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    for sx in (-1, 1):
        carlib.arch(body, L, M, y, tyre, sx=sx, gap=ARCH_GAP,
                    x_in=0.60, z_bottom=0.100)
carlib.sharpen(body, 34.0)
save("arches")

# --------------------------------------------------------------- apertures
# The face: shaped radiator mouths under the lamps, per car (see build_gt3.py)
FACES = {
    # twin: two big rounded mouths, their outer ends lifting, slats across
    "twin": [dict(poly=[(0.15, 0.115), (0.58, 0.115), (0.62, 0.245), (0.19, 0.235)], r=0.040,
                  bars=[(0.0, 0.030)], mirror=True)],
    # boomerang: one wide low mouth notched up either side of a centre
    # strut, the ends swept up under the lamps
    "boomerang": [dict(poly=[(-0.60, 0.110), (0.60, 0.110), (0.66, 0.215), (0.24, 0.225), (0.05, 0.180),
                             (-0.05, 0.180), (-0.24, 0.225), (-0.66, 0.215)], r=0.018,
                       bars=[(0.0, 0.026)], strut=True)],
    # horseshoe: the centre shoe (below) and two parallelogram mouths
    # leaning outboard, vertical bars
    "horseshoe": [dict(poly=[(0.25, 0.115), (0.56, 0.115), (0.62, 0.240), (0.30, 0.240)], r=0.025,
                       bars=[(90.0, 0.030)], mirror=True)],
}
FACE = []
for spec in FACES[V["face"]]:
    polys = [carlib.rounded(spec["poly"], spec["r"])]
    if spec.get("mirror"):
        polys.append(carlib.flip_x(polys[0]))
    for poly in polys:
        yb = carlib.poly_depth(L, poly, NOSE, margin=0.035)
        carlib.aperture_poly(body, M, poly, NOSE - 0.06, yb + 0.03, mat=M.mesh)
        FACE.append((spec, poly, yb))
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_Y + 0.13, LAMP_Z[1]))
    if V["tail"] != "fullbar":
        t0, t1 = sorted((sx * 0.34, sx * 0.80))
        carlib.aperture(body, M, (t0, TAILL_Y - 0.20, TAILL_TOP - TAILL_H),
                        (t1, TAILL_Y + 0.04, TAILL_TOP + 0.006))
if V["grille"] == "horseshoe":
    HS_Y = carlib.surface_station(L, 0.13, 0.20, NOSE, margin=0.03)
    carlib.aperture(body, M, (-0.13, HS_Y - 0.08, 0.105), (0.13, HS_Y + 0.22, 0.285), mat=M.mesh)
if V["tail"] == "fullbar":
    # one lined slot across the whole tail for the bar
    carlib.aperture(body, M, (-0.84, TAIL - 0.12, TAILL_TOP - 0.052), (0.84, TAIL + 0.05, TAILL_TOP + 0.006))
carlib.aperture(body, M, (-0.060, RAIN_Y - 0.14, RAIN_Z[0]), (0.060, RAIN_Y + 0.05, RAIN_Z[1]))
carlib.sharpen(body, 34.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
WZ = 1.04 + V["wing_z"]
TOP_Z = WZ + 0.235                          # endplate tops
CK = carlib.cockpit_points(L, WZ, liner_z=0.036, top_z=TOP_Z)
EYE, DASH_Z = CK["eye"], CK["dash_z"]
DX = EYE.x
SILL_X = L.x_at(0.0, 0.18)

# ---- floor aero: splitter, skirts, diffuser. No canards: clean flanks.
SPL_Z = 0.036
carlib.panel_xy(p, M.carbon, carlib.floor_plan(L, NOSE + 0.03, AX_F - 0.28, 0.120,
                                               steps=16, inset=-0.030),
                SPL_Z - 0.007, SPL_Z + 0.007)
for sx in (-1, 1):
    fy0, fy1 = NOSE + 0.12, AX_F - 0.32
    fence = [(fy0, SPL_Z), (fy1, SPL_Z), (fy1, SPL_Z + 0.045), (fy0, SPL_Z + 0.060)]
    carlib.plate(p, M.carbon, fence, sx * (L.x_at((fy0 + fy1) / 2, 0.130) - 0.030), 0.010,
                 chamfer=0.004)
    skirt = [(AX_F + 0.32, 0.058), (AX_R - 0.32, 0.058), (AX_R - 0.32, 0.135),
             (AX_F + 0.32, 0.130)]
    carlib.plate(p, M.carbon, skirt, sx * (SILL_X + 0.010), 0.026, chamfer=0.008)
    stripe = [(AX_F + 0.36, 0.150), (AX_R - 0.36, 0.150), (AX_R - 0.36, 0.180),
              (AX_F + 0.36, 0.178)]
    carlib.plate(p, M.accent, stripe, sx * (SILL_X + 0.004), 0.008)
DIF_Y0, DIF_Y1 = AX_R + 0.22, TAIL - 0.02
DIF_HW = min(L.x_at(y, 0.17) for y in (DIF_Y0, (DIF_Y0 + DIF_Y1) / 2, DIF_Y1 - 0.05)) - 0.045
carlib.diffuser(p, M.carbon, DIF_Y0, DIF_Y1, DIF_HW, 0.055, 0.235,
                thick=0.014, strakes=(-0.70, -0.40, -0.13, 0.13, 0.40, 0.70), strake_h=0.19)

# ---- rear wing: one wide main plane and a flap, carried on tall endplates
# that grow out of the rear fenders (no swan necks), a fin into its middle.
WY, WHW = 2.04, 0.935
PLAN, AMT = V["wing_plan"]
(te_y, te_z), TE = carlib.wing(p, M.carbon, WHW, 0.380, 0.090, 0.050, WY, WZ, angle_deg=-8.0,
                               plan=PLAN, amount=AMT)
(f2_y, f2_z), TE2 = carlib.wing(p, M.carbon, WHW, 0.140, 0.085, 0.045, WY + 0.325, WZ + 0.075,
                                angle_deg=-22.0, plan=PLAN, amount=AMT)
for (xa, xb, y, z) in carlib.spans(TE2, -WHW, WHW, 12):
    carlib.gurney(p, M.carbon, xa, xb, y, z, h=0.018, t=0.005, angle_deg=-22.0)
if V["wing_led"] in ("trail", "centre"):
    lx = WHW - 0.05 if V["wing_led"] == "trail" else 0.32
    for (xa, xb, y, z) in carlib.spans(TE2, -lx, lx, 12 if V["wing_led"] == "trail" else 4):
        carlib.led_strip(p, M, xa, xb, y - 0.036, z, h=0.020, t=0.010, glow=M.brake, dir_y=1.0)
EP_Y0 = WY - 0.10
# The endplates grow out of the rear fenders; above the fender each car has
# its own outline. The top stays at WZ + 0.235 (the mesh box the eye is
# derived from) and the back at WY + 0.53.
for sx in (-1, 1):
    zb0 = L.z_at(EP_Y0, WHW - 0.05) - 0.015
    zb1 = L.z_at(min(WY + 0.40, TAIL - 0.03), WHW - 0.05) - 0.015
    ep = {
        "square": [(EP_Y0, zb0), (WY + 0.40, zb1), (WY + 0.53, WZ - 0.02), (WY + 0.52, WZ + 0.215),
                   (WY + 0.14, WZ + 0.235), (WY - 0.08, WZ + 0.13)],
        "horn": [(EP_Y0, zb0), (WY + 0.40, zb1), (WY + 0.46, WZ - 0.04), (WY + 0.53, WZ + 0.235),
                 (WY + 0.40, WZ + 0.180), (WY + 0.10, WZ + 0.120), (WY - 0.10, WZ + 0.02)],
        "roll": [(EP_Y0, zb0), (WY + 0.40, zb1), (WY + 0.53, WZ + 0.00), (WY + 0.50, WZ + 0.180),
                 (WY + 0.38, WZ + 0.235), (WY + 0.06, WZ + 0.225), (WY - 0.08, WZ + 0.16)],
    }[V["endplate"]]
    carlib.plate(p, M.carbon, ep, sx * (WHW + 0.012), 0.014, chamfer=0.005)
    xo = WHW + 0.019
    if V["endplate"] == "roll":
        # the top rolls inboard over the flap
        p.box(M.carbon, (min(sx * (WHW - 0.05), sx * (WHW + 0.012)), WY + 0.10, WZ + 0.205),
              (max(sx * (WHW - 0.05), sx * (WHW + 0.012)), WY + 0.40, WZ + 0.219))
    if V["wing_led"] == "endplate":
        p.box(M.lamp_h, (min(sx * xo, sx * (xo + 0.008)), WY + 0.40, WZ - 0.02),
              (max(sx * xo, sx * (xo + 0.008)), WY + 0.425, WZ + 0.16))
        p.box(M.brake, (min(sx * (xo + 0.006), sx * (xo + 0.010)), WY + 0.404, WZ - 0.012),
              (max(sx * (xo + 0.006), sx * (xo + 0.010)), WY + 0.421, WZ + 0.152))
fin = [(SIDE_Y[1] - 0.02, L.roof_z(SIDE_Y[1] - 0.02) - 0.01),
       (SIDE_Y[1] + 0.40, L.roof_z(SIDE_Y[1] + 0.40) + 0.10),
       (WY + 0.16, WZ - 0.02), (WY + 0.16, WZ - 0.12),
       (AX_R + 0.20, L.roof_z(AX_R + 0.20) - 0.02)]
carlib.plate(p, M.paint, fin, 0.0, 0.016, chamfer=0.006)

# ---- headlamps: back wall, projectors a few cm in, the maker's DRL, lens.
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    n = {"round": 3, "slit": 2, "quad": 4}[V["lights"]]
    cz = (LAMP_Z[0] + LAMP_Z[1]) / 2
    r = min((x1 - x0) / (2.4 * n), (LAMP_Z[1] - LAMP_Z[0]) * 0.36)
    p.box(M.lamp_h, (x0, LAMP_Y + 0.110, LAMP_Z[0]), (x1, LAMP_Y + 0.130, LAMP_Z[1]))
    for k in range(n):
        cx = x0 + 0.03 + r + ((x1 - x0) - 0.06 - 2 * r) * (k / (n - 1) if n > 1 else 0.5)
        cy = carlib.surface_station(L, abs(cx) + r, cz, NOSE, margin=0.010) + 0.026
        carlib.projector(p, M, (cx, cy, cz), (0.0, -1.0, 0.0), r, depth=0.070)
    xo = x1 - 0.020 if sx > 0 else x0 + 0.020
    xi = x0 + 0.020 if sx > 0 else x1 - 0.020
    zlo, zhi = LAMP_Z[0] + 0.012, LAMP_Z[1] - 0.012

    def skin(x, z, back=0.016):
        return carlib.surface_station(L, abs(x), z, NOSE, margin=0.005) + back

    if V["drl"] == "points":
        # Panini: a small lit point beside each round lamp
        for k in range(n):
            cx = x0 + 0.03 + r + ((x1 - x0) - 0.06 - 2 * r) * (k / (n - 1) if n > 1 else 0.5)
            px = cx + sx * (r + 0.012)
            carlib.projector(p, M, (px, skin(px, cz + r * 0.6, 0.022), cz + r * 0.6), (0.0, -1.0, 0.0),
                             0.008, depth=0.014, chrome=False)
    elif V["drl"] == "blade":
        # Fugazzi: one thin blade the length of the slit, along its top edge
        carlib.guide_xyz(p, M.lamp, [(xi, skin(xi, zhi), zhi), (xo, skin(xo, zhi), zhi)], r=0.0045, segs=8)
    else:
        # Bugotti: a thin lit line under the quad bar, turning up at the outboard end
        carlib.guide_xyz(p, M.lamp, [(xi, skin(xi, zlo), zlo), (xo, skin(xo, zlo), zlo),
                                     (xo, skin(xo, zhi), zhi)], r=0.0040, segs=8)
    carlib.front_lens(p, M.glass, L, x0 - 0.004, x1 + 0.004, LAMP_Z[0] - 0.004, LAMP_Z[1] + 0.004,
                      NOSE, lift=-0.0025, nu=10, nv=3)


    # ---- tail lamps in their own apertures
    if V["tail"] == "fullbar":
        continue
    t0, t1 = sorted((sx * 0.34, sx * 0.80))
    zt0, zt1 = TAILL_TOP - TAILL_H, TAILL_TOP + 0.006
    p.box(M.lamp_h, (t0, TAILL_Y - 0.20, zt0), (t1, TAILL_Y - 0.18, zt1))
    xo = t1 - 0.025 if sx > 0 else t0 + 0.025
    xi = t0 + 0.025 if sx > 0 else t1 - 0.025
    ty = TAILL_Y - 0.05
    if V["tail"] == "rings":
        # Panini: two round lamps a side, each a lit ring round a projector
        rr = (zt1 - zt0) * 0.38
        for cx in (xi + sx * (rr + 0.03), xo - sx * (rr + 0.03)):
            ccz = (zt0 + zt1) / 2 + 0.008
            ring = [(cx + rr * math.cos(a), ty, ccz + rr * math.sin(a))
                    for a in (2 * math.pi * k / 28 for k in range(29))]
            carlib.guide_xyz(p, M.tail, ring, r=0.0065, segs=8)
            carlib.projector(p, M, (cx, ty + 0.002, ccz), (0.0, 1.0, 0.0), rr * 0.55,
                             depth=0.04, glow=M.brake, chrome=True)
    else:
        # Fugazzi: two long thin stripes, brake row between them
        for zz in (zt1 - 0.012, zt0 + 0.012):
            carlib.guide_xyz(p, M.tail, [(xi, ty, zz), (xo, ty, zz)], r=0.0050, segs=8)
        bb0, bb1 = sorted((xi, xo - sx * 0.08))
        carlib.led_grid(p, M, bb0, bb1, zt0 + 0.022, zt1 - 0.022, TAILL_Y - 0.062, dir_y=-1.0,
                        cols=10, rows=1, glow=M.brake)
    p.box(M.lens_tint, (t0 + 0.002, TAILL_Y - 0.008, zt0 + 0.002), (t1 - 0.002, TAILL_Y - 0.003, zt1 - 0.002))

if V["tail"] == "fullbar":
    # Bugotti: one light bar edge to edge, brake blocks at each end
    zb = TAILL_TOP - 0.023
    yb = max(carlib.surface_station(L, x, zb, TAIL, margin=0.0) for x in (0.0, 0.3, 0.6)) + 0.004
    carlib.tail_bar(p, M, -0.82, 0.82, yb, zb, h=0.046, glow=M.tail, dir_y=-1.0,
                    brake_x=((-0.80, -0.50), (0.50, 0.80)))
elif V["tail"] == "double":
    # Fugazzi: the upper stripe runs on across the tail between the lamps
    zb = TAILL_TOP - 0.006
    yb = max(carlib.surface_station(L, x, zb, TAIL, margin=0.0) for x in (0.0, 0.17, 0.34)) + 0.004
    for (bx0, bx1) in ((-0.34, -0.08), (0.08, 0.34)):   # either side of the rain light
        carlib.tail_bar(p, M, bx0, bx1, yb, zb, h=0.022, glow=M.tail, dir_y=-1.0)
carlib.led_grid(p, M, -0.044, 0.044, RAIN_Z[0] + 0.015, RAIN_Z[1] - 0.015, RAIN_Y - 0.020, dir_y=-1.0,
                cols=2, rows=6, glow=M.rain)

# ---- the face's mouths: mesh backing, the car's bars, the boomerang's strut
for (spec, poly, yb) in FACE:
    carlib.poly_fill(p, M.mesh, poly, yb, yb + 0.012)
    for (ang, pitch) in spec["bars"]:
        carlib.poly_bars(p, M.carbon, poly, yb - 0.030, angle_deg=ang, pitch=pitch,
                         t=0.008 if ang == 90.0 else 0.007, depth=0.028)
    if spec.get("strut"):
        zs = [z for (_, z) in poly]
        z0, z1 = min(zs) + 0.004, 0.176
        yf = carlib.surface_station(L, 0.02, z0, NOSE, step=0.004, margin=0.0) + 0.006
        carlib.plate(p, M.carbon, [(yf, z0), (yb, z0), (yb, z1), (yf + 0.02, z1)], 0.0, 0.060,
                     chamfer=0.008)

# ---- horseshoe grille (Bugotti)
if V["grille"] == "horseshoe":
    hy = HS_Y - 0.02
    carlib.grille(p, M, -0.115, 0.115, hy + 0.02, 0.115, 0.270, bars=5, depth=0.20, backing=True)
    shoe = []
    for k in range(25):
        a = math.pi * k / 24                        # a half circle over the top
        shoe.append((-0.105 * math.cos(a), 0.195 + 0.075 * math.sin(a)))
    pts = [(-0.095, 0.110)] + shoe + [(0.095, 0.110)]
    carlib.guide_xyz(p, M.chrome, [(x, carlib.surface_station(L, abs(x) + 0.01, z, NOSE, margin=0.0) - 0.004, z)
                                   for (x, z) in pts], r=0.011, segs=10)

# ---- C-line (Bugotti): a polished bar round the side intake
if V.get("cline"):
    # over the top of the intake, down its bowed leading edge, under it
    f = next(ff for ff in SIDE_BLADES if ff[0]["kind"] == "recess")[0]
    _, yz = L.recess_edges(f, f["j1"])
    cpts = [(yz + 0.02, f["j1"] + 0.18)]
    for k in range(9):
        jc = f["j1"] + 0.15 - (f["j1"] - f["j0"] + 0.30) * k / 8
        cpts.append((L.recess_edges(f, jc)[0] - 0.05, jc))
    cpts.append((yz + 0.02, f["j0"] - 0.18))
    for sx in (-1, 1):
        carlib.light_guide(p, M.chrome, L, cpts, sx=sx, lift=0.006, r=0.010, segs=10, per=5)

# ---- blades in the flank's vents, leaning with each vent; louvres across
# the front fender tops where the car has them
for (f, n, r) in SIDE_BLADES:
    for sx in (-1, 1):
        carlib.swept_blades(p, M.carbon, L, f, count=n, sx=sx, r=r)
for f in SIDE_FLOORS:
    for sx in (-1, 1):
        carlib.swept_floor(p, M.mesh, L, f, sx=sx)
for f in LOUVRES:
    for sx in (-1, 1):
        for k in range(5):
            yv = f["y0"] + 0.02 + k * 0.058
            zt = L.point(yv, 3.75).z
            xv = L.point(yv, 3.75).x
            p.box(M.carbon, (min(sx * (xv - 0.20), sx * (xv + 0.02)), yv, zt - 0.026),
                  (max(sx * (xv - 0.20), sx * (xv + 0.02)), yv + 0.026, zt - 0.006))

# ---- exhausts
if V["exhaust"] == "quad":
    # Panini: four pipes in a square in the middle of the tail
    for x in (-0.055, 0.055):
        for z in (0.270, 0.360):
            p.cylinder(M.chrome, (x, TAIL - 0.10, z), 0.040, 0.14, segs=24, axis='Y')
            p.cylinder(M.lamp_h, (x, TAIL - 0.09, z), 0.032, 0.14, segs=24, axis='Y')
else:
    for x in (-0.22, 0.22):
        p.cylinder(M.lamp_h, (x, TAIL - 0.14, 0.300), 0.056, 0.070, segs=20, axis='Y')
        p.cylinder(M.metal, (x, TAIL - 0.15, 0.300), 0.045, 0.19, segs=20, axis='Y')

# ---- roof snorkel (Panini only), mirrors, antenna, tow hooks, wiper
if V["scoop"]:
    sc_x, sc_y, sc_h = V["scoop"]
    SC_Y = SIDE_Y[0] + sc_y
    sc_z = L.roof_z(SC_Y) - 0.010
    sc_prof = [(SC_Y - 0.26, sc_z), (SC_Y - 0.12, sc_z + sc_h * 0.92),
               (SC_Y + 0.12, sc_z + sc_h), (SC_Y + 0.40, sc_z + sc_h * 0.30),
               (SC_Y + 0.48, sc_z)]
    carlib.plate(p, M.carbon, sc_prof, 0.0, sc_x * 1.10, chamfer=0.035)
    p.box(M.mesh, (-sc_x * 0.42, SC_Y - 0.262, sc_z + 0.020),
          (sc_x * 0.42, SC_Y - 0.242, sc_z + sc_h * 0.80))
MIR_Y = SCREEN_Y[0] + 0.26
# a deeper valley would drop the mirror onto the wider fender and push it
# out, widening the mesh box the client derives the driver's eye from
MIR_Z = L.point(MIR_Y, 5.10).z + 0.035 - min(V.get("valley", 0.0), 0.0)
for sx in (-1, 1):
    carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.100, MIR_Y, MIR_Z, sx=sx,
                  style=V["mirror"], head=(0.064, 0.120, 0.046))
p.bar(M.carbon, (0.16, SIDE_Y[1] - 0.10, L.roof_z(SIDE_Y[1] - 0.10) - 0.01),
      (0.16, SIDE_Y[1] - 0.10, L.roof_z(SIDE_Y[1] - 0.10) + 0.16), 0.005)
for yy in (NOSE + 0.16, TAIL - 0.12):
    p.torus(M.towhook, (0.28, yy, 0.200), 0.048, 0.012, segs=16, rings=8)
wy = SCREEN_Y[0] - 0.02
p.bar(M.trim, (DX - 0.40, wy, L.z_at(wy, 0.34) + 0.010),
      (DX + 0.24, wy + 0.03, L.z_at(wy + 0.03, 0.34) + 0.010), 0.008, segs=8)

# ---- small hardware: clam pins, door latch, filler, roof camera pod
for (x, yy) in ((-0.50, NOSE + 0.62), (0.50, NOSE + 0.62)):
    pz = L.z_at(yy, abs(x))
    p.cylinder(M.metal, (x, yy, pz - 0.004), 0.014, 0.010, segs=12, axis='Z')
    p.cylinder(M.trim, (x, yy, pz + 0.006), 0.007, 0.006, segs=8, axis='Z')
for sx in (-1, 1):
    ly = SIDE_Y[1] - 0.14
    lz = L.point(ly, 2.6).z
    lx = L.x_at(ly, lz)
    p.box(M.trim, (min(sx * (lx - 0.008), sx * (lx + 0.003)), ly - 0.045, lz - 0.012),
          (max(sx * (lx - 0.008), sx * (lx + 0.003)), ly + 0.045, lz + 0.012))
fy = AX_R - 0.58
fz = L.z_at(fy, 0.62)
p.cylinder(M.metal, (0.62, fy, fz - 0.008), 0.040, 0.014, segs=16, axis='Z')
p.cylinder(M.trim, (0.62, fy, fz + 0.006), 0.031, 0.004, segs=16, axis='Z')
cy = SIDE_Y[1] - 0.02
cz = L.z_at(cy, 0.22)
p.box(M.trim, (0.22 - 0.04, cy - 0.05, cz - 0.01), (0.22 + 0.04, cy + 0.05, cz + 0.028))
p.cylinder(M.glass, (0.22, cy + 0.05, cz + 0.014), 0.010, 0.006, segs=10, axis='Y')

# ---- race number on the nose
NUM = V.get("number", "1")
carlib.top_patch(p, M.decal, L, NOSE + 0.30, NOSE + 0.64, -0.19, 0.19, lift=0.004, nu=8, nv=6)
carlib.top_text(p, M.trim, L, NUM, 0.0, NOSE + 0.47, size=0.24, thick=0.006, lift=0.005,
                face="front", squash=0.80)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
DASH_Z = min(EYE.z - 0.22, L.roof_z(SCREEN_Y[0]) - 0.03)
DASH_Y0, DASH_Y1 = SCREEN_Y[0] - 0.02, CK["wheel"].y - 0.28
XIN = min(L.x_at(y, DASH_Z) for y in (DASH_Y0, -0.3, 0.0, 0.3, SIDE_Y[1])) - 0.050
HOOP_Y = SIDE_Y[1] - 0.06
BULK_Y = HOOP_Y + 0.03
BELT_Z = L.point(0.0, CAN_LO).z
p.box(M.interior, (-XIN, DASH_Y0, 0.090), (XIN, BULK_Y, 0.130))                  # floor
p.box(M.alcantara, (-XIN + 0.02, -0.74, 0.130), (XIN - 0.02, 0.08, 0.135))       # footwell mat
p.box(M.interior, (-0.14, DASH_Y0, 0.130), (0.14, BULK_Y, 0.29))                  # tunnel
p.box(M.trim, (-0.16, DASH_Y0, 0.29), (0.16, BULK_Y, 0.305))
carlib.switch_panel(p, M, -0.13, 0.13, CK["wheel"].y - 0.18, CK["wheel"].y + 0.20, 0.315,
                    rows=3, cols=3, rotary=True)
p.box(M.interior, (-XIN, BULK_Y, 0.130), (XIN, BULK_Y + 0.03, BELT_Z - 0.06))     # bulkhead
p.box(M.trim, (-XIN, BULK_Y - 0.01, BELT_Z - 0.06), (XIN, BULK_Y + 0.04, BELT_Z - 0.03))
p.box(M.interior, (-XIN, DASH_Y0, 0.52), (XIN, DASH_Y1, DASH_Z - 0.06))           # dash
p.box(M.alcantara, (-XIN, DASH_Y0, DASH_Z - 0.06), (XIN, DASH_Y1, DASH_Z))
p.bar(M.alcantara, (-XIN, DASH_Y1, DASH_Z - 0.016), (XIN, DASH_Y1, DASH_Z - 0.016), 0.016, segs=10)
hood = [(DASH_Y1 - 0.28, DASH_Z), (DASH_Y1 + 0.02, DASH_Z), (DASH_Y1 + 0.02, DASH_Z + 0.036),
        (DASH_Y1 - 0.22, DASH_Z + 0.036)]
carlib.plate(p, M.alcantara, hood, DX, 0.30, chamfer=0.010)
p.box(M.trim, (-0.09, DASH_Y1 - 0.06, DASH_Z), (0.09, DASH_Y1 - 0.01, DASH_Z + 0.036))
p.box(M.display, (-0.075, DASH_Y1 - 0.012, DASH_Z + 0.008), (0.075, DASH_Y1 - 0.008, DASH_Z + 0.032))
carlib.switch_panel(p, M, DX - 0.40, DX - 0.19, DASH_Y1 - 0.11, DASH_Y1 - 0.02, DASH_Z + 0.001,
                    rows=1, cols=3, rotary=False)
for sx in (-1, 1):
    carlib.door_card(p, M, sx, XIN, SIDE_Y[0] + 0.02, SIDE_Y[1] - 0.02, 0.17, BELT_Z - 0.035,
                     pull=True)
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), DASH_Y0, 0.130),
          (max(sx * XIN, sx * (XIN + 0.02)), SIDE_Y[0] + 0.02, 0.52))
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), SIDE_Y[1] - 0.02, 0.130),
          (max(sx * XIN, sx * (XIN + 0.02)), BULK_Y + 0.03, BELT_Z - 0.06))
carlib.bucket_seat(p, M, DX, 0.10, 0.130, width=0.48, depth=0.52, back_h=0.60, rake_deg=26.0)
for x in (DX - 0.19, DX - 0.07, DX + 0.05):
    p.box(M.metal, (x - 0.030, -0.78, 0.140), (x + 0.030, -0.72, 0.265))
p.box(M.metal, (DX + 0.13, -0.80, 0.140), (DX + 0.20, -0.70, 0.25))
carlib.extinguisher(p, M, -0.40, -0.40, 0.19, r=0.046, length=0.32)
carlib.inner_skin(p, M.alcantara, L, SCREEN_Y[1] + 0.03, SIDE_Y[1] - 0.03, 0.40,
                  drop=0.030, nu=10, nv=6)
for sx in (-1, 1):
    top = (sx * 0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, 0.20), (sx * 0.56, HOOP_Y, top[2] - 0.12), 0.022, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, top[2] - 0.12), top, 0.022, segs=8)
    scr = (sx * 0.50, SCREEN_Y[1], L.point(SCREEN_Y[1], CAN_LO + 0.25).z - 0.030)
    p.bar(M.cage, top, scr, 0.019, segs=8)
    foot = (sx * (L.x_at(wy, 0.58) - 0.075), wy + 0.02, L.z_at(wy + 0.02, 0.60) - 0.040)
    p.bar(M.cage, scr, foot, 0.018, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, top[2] - 0.18), (sx * 0.44, AX_R - 0.15, 0.50),
          0.019, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, 0.50), (sx * 0.55, -0.64, 0.54), 0.019, segs=8)
p.bar(M.cage, (-0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050),
      (0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050), 0.022, segs=8)

# ---- wordmark on the door, ahead of the side intake
DEC_Y = (AX_F + 0.80, SIDE_Y[1] + 0.00)
carlib.conform_decal(p, M.logo, L, DEC_Y[0], DEC_Y[1], 1.95, 2.95, sx=1,
                     lift=0.005, nu=14, nv=6)
carlib.conform_decal(p, M.logo, L, DEC_Y[0], DEC_Y[1], 1.95, 2.95, sx=-1,
                     lift=0.005, nu=14, nv=6, flip_u=True)

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0035, segments=2,
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
