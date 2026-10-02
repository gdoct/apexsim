"""Build one of the LMP2 prototypes in Blender and export it to
content/cars/default/<folder>/<stem>.glb.

    VARIANT = "posh"       # yotota | posh | fugazzi | jeanetti
    exec(open(r"E:\\apexsim\\content\\cars\\build_lmp2.py").read())

The four cars share one hull generator; each VARIANT is a set of small shape
and livery parameters (nose width, fender peak, canopy height and position,
tail height, wing height, fin, lights, paint, logo). Everything below the
shape data comes from scripts/content/cars/carlib.py, shared with build_gt3.py.

Frame: nose on -Y, tail on +Y, ground z = 0, metres, left-hand drive (driver
on +X). Exported with glTF +Y up, so the nose lands on glTF +Z like the other
cars in content/cars, and AApexRaceCarActor's -90 deg yaw puts it on world +X.

Stance (class-wide, matched by each car.toml's [wheels] table): a 0.355/0.365 m
tyre on a 1.58 m track inside a ~1.96 m body, arch cut 22 mm clear of the tyre.

Section control points, floor centre outwards and up to the roof centre:
    0 floor centre        3 fender crown (HARD)   6 canopy side
    1 floor edge          4 fender inner          7 canopy upper
    2 sill (HARD)         5 valley (HARD)         8 roof centre
`hard` indices are creases - sampled either side, so they stay edges. On a
prototype they are what carries the shape: the fender crowns and the tunnel
between fender and cockpit are the whole look, and a smoothed loft loses both.
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

# ------------------------------------------------------------------ loading
_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")
for _n, _p in (("apex", os.path.join(_ROOT, "scripts", "content", "props", "apex_props.py")),
               ("carlib", os.path.join(_ROOT, "scripts", "content", "cars", "carlib.py"))):
    _s = importlib.util.spec_from_file_location(_n, _p)
    _m = importlib.util.module_from_spec(_s)
    sys.modules[_n] = _m
    _s.loader.exec_module(_m)
import apex, carlib                                        # noqa: E402
from carlib import Builder                                  # noqa: E402

# ------------------------------------------------------------------ class kit
TYRE_F, TYRE_R = 0.355, 0.365
TRACK = 1.580
HUB_X = TRACK / 2.0
ARCH_GAP = 0.022
SAMP = 6
AX_F, AX_R = -1.500, 1.500        # wheelbase 3.0 m, as [physics] says
CAN_LO, CAN_HI = 5.75, 7.45       # control indices where the canopy glass runs
TRIM_J = 0.28                     # satin-black surround under the glass, from the valley crease up
TRIM_Y = 0.045                    # and either side of every glass edge (pillars, cowl)
SUN_J = 0.45                      # tinted strip where the screen meets the roof panel

VARIANTS = {
    "yotota": dict(folder="yotota-lmp2", stem="yotota_lmp2", logo="yotota_logo.png",
                   paint=(0.90, 0.90, 0.88), accent=(0.80, 0.04, 0.04), caliper=(0.85, 0.10, 0.05),
                   paint_metallic=0.45, number="7", bonnet_drop=(0.02, 0.05), drl="T", tail="double",
                   nose_w=1.00, fender=1.00, roof=1.00, canopy_shift=0.00, tail_h=1.00,
                   side_w=1.00, wing_z=0.00, fin=True, lights="tri", mirror="pod",
                   scoop=(0.30, 0.62, 0.09), seat=(0.10, 0.10, 0.32),
                   # the plain one: square twin mouths, an upright intake
                   nose_z=0.0, valley=0.0, face="twin", side="upright",
                   # square endplates, a straight two-element wing on swan necks
                   wing_plan=("straight", 0.0), endplate="square", mount="swan", wing_led="trail",
                   sponsors=dict(fender=15, wing=2, ep=12)),
    "posh": dict(folder="posh-lmp2", stem="posh_lmp2", logo="posh_logo.png",
                 paint=(0.50, 0.51, 0.54), accent=(0.04, 0.04, 0.045), caliper=(0.95, 0.75, 0.05),
                 paint_metallic=0.90, number="22", bonnet_drop=(0.02, 0.05), drl="points", tail="bar",
                 nose_w=1.06, fender=1.03, roof=0.97, canopy_shift=-0.10, tail_h=0.94,
                 side_w=1.00, wing_z=-0.04, fin=True, lights="round", mirror="pod",
                 scoop=(0.34, 0.56, 0.085), seat=(0.10, 0.10, 0.12),
                 # smooth: filled-in valleys, a centre mouth between corner
                 # intakes, an intake that sweeps back under a waist line
                 nose_z=0.020, valley=0.035, face="tri", side="sweep",
                 lamp_x=(0.24, 0.56), lamp_h=0.130,
                 # an arched plane, endplates swept up to a tall trailing
                 # corner, swan necks set wide
                 wing_plan=("arch", 0.035), endplate="swoop", mount="swan_wide", wing_led="trail",
                 sponsors=dict(fender=14, wing=0, ep=8)),
    "fugazzi": dict(folder="fugazzi-lmp2", stem="fugazzi_lmp2", logo="fugazzi_logo.png",
                    paint=(0.62, 0.02, 0.03), accent=(0.95, 0.78, 0.05), caliper=(0.95, 0.80, 0.05),
                    paint_metallic=0.65, number="51", bonnet_drop=(0.02, 0.05), drl="blade", tail="rings",
                    nose_w=0.90, fender=1.05, roof=1.00, canopy_shift=0.12, tail_h=1.04,
                    side_w=0.99, wing_z=0.02, fin=True, lights="tri", mirror="stalk",
                    scoop=(0.26, 0.68, 0.095), seat=(0.16, 0.05, 0.05),
                    # sharp: drooped nose, deep valleys, one boomerang mouth,
                    # the long raked slash of the marque's hypercar
                    nose_z=-0.025, valley=-0.045, face="boomerang", side="slash",
                    lamp_x=(0.22, 0.66), lamp_h=0.070,
                    # a spoon-shaped plane hung under two pylons, raked
                    # endplates with the brake lights running up their backs
                    wing_plan=("spoon", 0.050), endplate="raked", mount="pylon", wing_led="endplate",
                    sponsors=dict(fender=14, wing=8, ep=4)),
    "jeanetti": dict(folder="jeanetti-lmp2", stem="jeanetti_lmp2", logo="jeanetti_logo.png",
                     paint=(0.02, 0.20, 0.10), accent=(0.95, 0.82, 0.18), caliper=(0.20, 0.20, 0.22),
                     paint_metallic=0.60, number="38", bonnet_drop=(0.02, 0.05), drl="claws", tail="claws",
                     nose_w=1.00, fender=0.98, roof=1.03, canopy_shift=0.05, tail_h=1.00,
                     side_w=1.02, wing_z=0.05, fin=False, lights="bar", mirror="pod",
                     scoop=(0.32, 0.52, 0.10), seat=(0.08, 0.10, 0.06),
                     # clawed: two tall mouths leaning in at the top, three
                     # gills behind the front wheel, an intake leaning forward
                     nose_z=0.012, valley=0.010, face="claw", side="gills",
                     lamp_x=(0.30, 0.66), lamp_h=0.090,
                     # a V-swept plane, louvred endplates, one central swan
                     # neck behind the fin, brake light across the middle
                     wing_plan=("swept", 0.12), endplate="louvred", mount="centre", wing_led="centre",
                     sponsors=dict(fender=14, wing=4, ep=10)),
}

# Base hull. Right-half section control points (x, z).
# Pass 5, measured against the imported prototypes (919, TS040): 1.90 m wide
# by rule (the first hull was 2.04 with the skirts), fender crowns at
# 0.76-0.81 rather than 0.95, a canopy ~0.9 m across at its base, and an
# engine deck that falls away behind the canopy to ~0.5 m at the tail with
# the fin standing out of it, instead of a hump the height of the fenders.
KEYS = [
    (-2.45, [(0, .105), (.465, .105), (.525, .155), (.535, .235), (.43, .285), (.29, .295), (.18, .305), (.08, .315), (0, .32)]),
    (-2.25, [(0, .065), (.725, .065), (.805, .195), (.815, .395), (.67, .455), (.475, .43), (.31, .43), (.14, .44), (0, .44)]),
    (-1.85, [(0, .055), (.85, .055), (.90, .395), (.905, .74), (.735, .78), (.545, .64), (.36, .60), (.16, .60), (0, .60)]),
    (-1.50, [(0, .055), (.87, .055), (.905, .445), (.915, .78), (.735, .81), (.545, .66), (.36, .65), (.16, .655), (0, .66)]),
    (-1.10, [(0, .055), (.88, .055), (.905, .415), (.90, .73), (.73, .77), (.56, .67), (.385, .695), (.18, .715), (0, .715)]),
    # the deck stays low to the foot of the screen, so the screen rises steeply
    # from it (a prototype's does) and the driver, lower now, still sees the road
    (-0.95, [(0, .055), (.88, .055), (.90, .405), (.895, .70), (.73, .74), (.57, .66), (.40, .70), (.19, .72), (0, .725)]),
    (-0.60, [(0, .055), (.88, .055), (.90, .395), (.89, .635), (.73, .665), (.58, .645), (.44, .835), (.25, .955), (0, .985)]),
    (-0.10, [(0, .055), (.88, .055), (.90, .395), (.89, .625), (.73, .645), (.59, .635), (.46, .955), (.27, 1.045), (0, 1.065)]),
    (0.45, [(0, .055), (.88, .055), (.90, .395), (.89, .635), (.73, .665), (.60, .655), (.46, .945), (.27, 1.035), (0, 1.055)]),
    (1.00, [(0, .055), (.88, .055), (.905, .415), (.90, .74), (.73, .765), (.58, .60), (.40, .66), (.21, .72), (0, .74)]),
    (1.50, [(0, .055), (.87, .055), (.905, .445), (.915, .76), (.73, .775), (.565, .55), (.37, .56), (.19, .59), (0, .61)]),
    (1.90, [(0, .075), (.86, .075), (.90, .415), (.89, .72), (.71, .735), (.545, .50), (.35, .49), (.17, .51), (0, .53)]),
    (2.30, [(0, .215), (.775, .215), (.83, .445), (.825, .63), (.655, .65), (.49, .46), (.31, .45), (.14, .46), (0, .47)]),
]


def apply_variant(keys, v):
    """The per-car shape factors: a prototype field is one silhouette with
    different noses, fender peaks, canopies and tails."""
    out = []
    for (y, pts) in keys:
        new = []
        for j, (x, z) in enumerate(pts):
            if y < -1.9:
                x *= v["nose_w"]
            if j in (3, 4) and abs(abs(y) - 1.5) < 0.45:
                z = 0.055 + (z - 0.055) * v["fender"]
            if j >= 6 and -0.7 < y < 1.1:
                z = 0.60 + (z - 0.60) * v["roof"]
            # The canopy: a prototype's is ~0.9 m across at its base and the
            # driver sits 0.15-0.2 m off centre (the imported cars' DRIVEREYES).
            # Pass 3 widened it 16% and raised it 4.5 cm to fit the client's
            # derived eye (18% of the width out, 70% of the box up); the build
            # now writes its own `[cockpit]` eye, so only a little of that stays.
            if j == 6 and -1.2 < y < 1.6:
                x *= 1.04
            if j == 7 and -1.2 < y < 1.6:
                x *= 1.03
            if j >= 7 and -0.9 < y < 1.3:
                z += 0.015 * (1.0 if -0.6 <= y <= 1.0 else 0.5)
            if j in (1, 2) and -1.9 < y < 2.0:
                x *= v["side_w"]
            if y > 1.7:
                z *= v["tail_h"]
            if y < -1.7 and j >= 2:
                z += v.get("nose_z", 0.0) * min(1.0, (-1.7 - y) / 0.6)
            if j in (4, 5) and -2.0 < y < 1.95:
                z += v.get("valley", 0.0) * (1.0 if j == 5 else 0.5)
            new.append((x, z))
        yy = y + (v["canopy_shift"] if -1.0 <= y <= 1.0 else 0.0)
        out.append((yy, new))
    return out


try:
    VARIANT
except NameError:
    VARIANT = "yotota"
V = VARIANTS[VARIANT]
CAR_DIR = os.path.join(carlib.CARS_ROOT, V["folder"])
CS = V["canopy_shift"]
SCREEN_Y = (-0.98 + CS, -0.30 + CS)
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
                         paint_rough=0.22,
                         carbon_weave=True, sponsors=True)

MIR = {k: carlib.mat("car_mirror_" + k, (0.55, 0.57, 0.60), 1.0, 0.05) for k in ("left", "right")}
SP = V["sponsors"]

# ---------------------------------------------------------------- body loft
VK = carlib.fender_bump(apply_variant(KEYS, V), (AX_F, AX_R),
                        amount=0.022, width=0.55, j0=2.4, j1=4.0)
if V.get("bonnet_drop"):
    # the nose deck between the fender crowns, a shade lower: the eye sits
    # 70% of the box up and every centimetre of cowl costs road (see sightline)
    VK = carlib.drop_bonnet(VK, V["bonnet_drop"][0], V["bonnet_drop"][1], SCREEN_Y[0], NOSE_KEY,
                            fade=0.20, j_full=7, partial=((6, 0.5),))
L = carlib.Loft(VK, samp=SAMP, ny=110, hard=(2, 3, 5))
NOSE, TAIL = L.nose, L.tail

ARCH_SPAN = [(AX_F - TYRE_F - ARCH_GAP, AX_F + TYRE_F + ARCH_GAP),
             (AX_R - TYRE_R - ARCH_GAP, AX_R + TYRE_R + ARCH_GAP)]


def in_arch(y):
    return any(a <= y <= b for (a, b) in ARCH_SPAN)


# Bodywork splits: the nose clam, the door, and the engine cover.
for (y, kind) in ((NOSE + 0.64, "upper"), (AX_F + 0.42, "side"),
                  (SIDE_Y[1] + 0.06, "side"), (SIDE_Y[1] + 0.10, "upper"),
                  (AX_R + 0.42, "upper")):
    if kind == "upper":
        L.groove(y, 4.2, 8.0, depth=0.010, width=0.015)
    elif not in_arch(y):
        L.groove(y, 1.4, 3.2, depth=0.010, width=0.015)

# Fender-top louvre panels: the signature prototype detail, cut in so the
# blades sit in a pocket rather than lying on the paint.
for ax in (AX_F, AX_R):
    L.recess(ax - 0.15, ax + 0.15, 3.30, 4.25, depth=0.022, rim=0.065)
# The flank, per car (swept vents and character lines, see build_gt3.py):
# the first cut gave all four the same box intake and brake exit.
S1 = SIDE_Y[1]
SIDES = {
    "upright": [
        ("recess", dict(y0=(S1 + 0.16, S1 + 0.13), y1=(S1 + 0.56, S1 + 0.54), j0=1.85, j1=3.15,
                        depth=0.060, rim=0.030, blades=3)),
        ("recess", dict(y0=AX_F + 0.46, y1=AX_F + 0.74, j0=1.90, j1=3.05, depth=0.040, rim=0.024,
                        blades=3, blade_r=0.006)),
    ],
    "sweep": [
        ("recess", dict(y0=(S1 + 0.12, S1 + 0.34), y1=(S1 + 0.52, S1 + 0.60), j0=1.85, j1=3.20,
                        depth=0.060, rim=0.030, bow=-0.05, blades=3, blade_r=0.007)),
        ("recess", dict(y0=(AX_F + 0.50, AX_F + 0.56), y1=(AX_F + 0.70, AX_F + 0.78), j0=1.95, j1=3.00,
                        depth=0.035, rim=0.020, blades=2, blade_r=0.005)),
        ("swage", dict(y0=AX_F + 0.45, y1=S1 + 0.36, j_a=3.10, j_mid=2.85, j_b=3.25, depth=0.008,
                       width=0.30, fade=0.25)),
    ],
    "slash": [
        ("recess", dict(y0=(S1 + 0.10, S1 + 0.44), y1=(S1 + 0.46, S1 + 0.64), j0=1.80, j1=3.30,
                        depth=0.065, rim=0.055, bow=-0.03, blades=3, blade_r=0.008)),
        ("recess", dict(y0=(AX_F + 0.62, AX_F + 0.46), y1=(AX_F + 0.72, AX_F + 0.58), j0=1.95, j1=3.05,
                        depth=0.035, rim=0.018)),
        ("swage", dict(y0=AX_F + 0.50, y1=S1 + 0.42, j_a=3.10, j_b=3.30, j_mid=3.00, depth=0.010,
                       width=0.30, fade=0.25)),
    ],
    "gills": [("recess", dict(y0=(AX_F + 0.44 + 0.08 * k, AX_F + 0.52 + 0.08 * k),
                              y1=(AX_F + 0.485 + 0.08 * k, AX_F + 0.565 + 0.08 * k), j0=1.95, j1=3.10,
                              depth=0.032, rim=0.012)) for k in range(3)] + [
        ("recess", dict(y0=(S1 + 0.30, S1 + 0.14), y1=(S1 + 0.60, S1 + 0.50), j0=1.85, j1=3.20,
                        depth=0.060, rim=0.030, blades=4, blade_r=0.006)),
    ],
}
SIDE_BLADES, SIDE_FLOORS = [], []
for (kind, kw) in SIDES[V["side"]]:
    kw = dict(kw)
    blades, blade_r = kw.pop("blades", 0), kw.pop("blade_r", 0.007)
    if kind == "swage":
        L.swage(**kw)
        continue
    L.recess(**kw)
    f = L.last_feature
    if f["swept"] and kw["depth"] > 0.022:
        SIDE_FLOORS.append(f)
    if blades:
        SIDE_BLADES.append((f, blades, blade_r))
# Engine-cover exit louvres behind the canopy.
L.recess(SIDE_Y[1] + 0.26, SIDE_Y[1] + 0.74, 5.80, 8.00, depth=0.030, rim=0.026)
# The tunnel between each front fender and the cockpit, deepened.
L.recess(AX_F + 0.30, -0.35 + CS, 4.70, 5.90, depth=0.030, rim=0.12)

# Lamp geometry, computed here (pre-build) so the pockets that follow can
# use it too - a lamp cut into an untouched curve is a box glued onto paint;
# cut into a shallow recessed panel, the same box reads as a housing.
LAMP_X = V.get("lamp_x") or {"round": (0.16, 0.58), "tri": (0.14, 0.62), "bar": (0.14, 0.62)}[V["lights"]]
# a hand lower than before: the fender fronts are lower now (0.45 m at the
# nose key) and a lamp whose top stood above the skin pushed its mask plate
# out of the bodywork
LAMP_Z = (0.300, 0.300 + min(0.120, V.get("lamp_h") or {"round": 0.120, "tri": 0.115, "bar": 0.100}[V["lights"]]))
LAMP_Y = carlib.surface_station(L, LAMP_X[1], 0.36, NOSE, margin=0.075)
TAILL_Y = carlib.surface_station(L, 0.74, 0.55, TAIL, margin=0.03)
TAILL_TOP = min(L.z_at(TAILL_Y, 0.55) - 0.060, 0.63)   # on the rear fender's face
RAIN_Z = (0.22, 0.42)                                    # under the deck's top at the tail
RAIN_Y = carlib.surface_station(L, 0.10, (RAIN_Z[0] + RAIN_Z[1]) / 2, TAIL, margin=0.05)

# Headlamp pocket: a shallow recessed panel so the cluster sits in a dent
# instead of a box glued onto the raw curve, and the per-cup surface-station
# drift has slack to land inside rather than tearing the skin at the edge.
L.recess(NOSE + 0.02, LAMP_Y + 0.34, 1.55, 4.60, depth=0.028, rim=0.15)
# Tail-lamp pocket and rain-light pocket, same idea at the back.
# (on the fender's face only, under the crown: run over the crown it dented
# the pass-5 rear fender, whose crown is 0.3 m lower than before)
L.recess(TAILL_Y - 0.30, TAIL - 0.02, 2.00, 3.05, depth=0.022, rim=0.11)
L.recess(RAIN_Y - 0.18, TAIL - 0.02, 3.40, 4.80, depth=0.018, rim=0.055)


def face_mat(ym, kk, right):
    """Paint, glass, or the satin-black surround. A prototype canopy is a
    bubble in a black frame: a trim band above the valley crease, the
    pillars either side of every glass edge, and a tinted strip where the
    screen runs into the roof panel."""
    jc = kk / SAMP
    is_screen = SCREEN_Y[0] < ym < SCREEN_Y[1]
    is_side = SIDE_Y[0] <= ym < SIDE_Y[1]
    if jc < CAN_LO - TRIM_J:
        if jc < 3.6 and L._offset(ym, kk, right, skip_swept=True) > 0.030:
            return M.mesh
        if jc < 1.70:
            return M.carbon               # pass 7: bare carbon below the sill line
        return M.paint
    if jc < CAN_LO:
        return M.trim if (is_screen or is_side) else M.paint
    if is_screen:
        return M.trim if jc > CAN_HI + SUN_J else M.glass
    if is_side and jc < CAN_HI:
        return M.glass
    if jc < CAN_HI + 0.4 and any(abs(ym - e) < TRIM_Y for e in (SCREEN_Y[0], SIDE_Y[1])):
        return M.trim
    return M.paint


body = L.build("body", face_mat, [M.paint, M.glass, M.trim, M.mesh, M.carbon], subsurf=0)
save("body")

# -------------------------------------------------------------- wheel arches
parts_objs = []
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    for sx in (-1, 1):
        carlib.arch(body, L, M, y, tyre, sx=sx, gap=ARCH_GAP,
                    x_in=0.50, z_bottom=0.105)
carlib.sharpen(body, 34.0)
save("arches")

# --------------------------------------------------------------- apertures
# Lamps, radiator mouths and the rain light are cut, so the cutter's surface
# lines the opening and the lens sits in a hole rather than on the paint - now
# inside the recessed pockets above, with extra margin so a curved cut edge
# stays inside the pocket wall instead of exposing a seam against the paint.
# The face: shaped radiator mouths under the lamps, per car (see build_gt3.py)
FACES = {
    "twin": [dict(poly=[(0.16, 0.125), (0.56, 0.125), (0.56, 0.290), (0.18, 0.290)], r=0.030,
                  bars=[(0.0, 0.034)], mirror=True)],
    "tri": [dict(poly=[(-0.13, 0.125), (0.13, 0.125), (0.16, 0.265), (-0.16, 0.265)], r=0.030,
                 bars=[(0.0, 0.030)]),
            dict(poly=[(0.26, 0.125), (0.60, 0.125), (0.62, 0.270), (0.30, 0.300)], r=0.035,
                 bars=[(0.0, 0.030)], mirror=True)],
    "boomerang": [dict(poly=[(-0.58, 0.120), (0.58, 0.120), (0.64, 0.240), (0.24, 0.260), (0.05, 0.200),
                             (-0.05, 0.200), (-0.24, 0.260), (-0.64, 0.240)], r=0.018,
                       bars=[(0.0, 0.028)], strut=True)],
    "claw": [dict(poly=[(0.12, 0.125), (0.36, 0.125), (0.52, 0.300), (0.30, 0.300)], r=0.025,
                  bars=[(90.0, 0.030)], mirror=True),
             dict(poly=[(0.42, 0.125), (0.62, 0.125), (0.64, 0.230), (0.52, 0.230)], r=0.020,
                  bars=[(0.0, 0.028)], mirror=True)],
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
    # forward of the nose tip, so the box cuts through at every x across a
    # curved front, not just at the widest point
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_Y + 0.135, LAMP_Z[1]))
    t0, t1 = sorted((sx * 0.30, sx * 0.76))
    carlib.aperture(body, M, (t0, TAILL_Y - 0.20, TAILL_TOP - 0.125),
                    (t1, TAILL_Y + 0.04, TAILL_TOP + 0.008))
# FIA rain light: a vertical bar on the centreline, inside its own pocket
carlib.aperture(body, M, (-0.065, RAIN_Y - 0.14, RAIN_Z[0]), (0.065, RAIN_Y + 0.05, RAIN_Z[1]))
carlib.sharpen(body, 34.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
WZ = 1.05 + V["wing_z"]
# The eye is the car's own (written to car.toml as [cockpit] at the end of
# the build): a prototype driver sits ahead of the middle of the wheelbase,
# a hand off the centreline, lower than a GT driver.
CK = carlib.authored_cockpit((0.18, -0.25 + CS, 0.82))
EYE, DASH_Z = CK["eye"], CK["dash_z"]
DX = EYE.x
SILL_X = L.x_at(0.0, 0.18)

# ---- floor aero: splitter, dive planes, skirts, diffuser
SPL_Z = 0.036
carlib.panel_xy(p, M.carbon, carlib.floor_plan(L, NOSE + 0.04, AX_F - 0.26, 0.125,
                                               steps=14, inset=-0.026),
                SPL_Z - 0.008, SPL_Z + 0.008)
for sx in (-1, 1):
    fy0, fy1 = NOSE + 0.10, AX_F - 0.30
    fence = [(fy0, SPL_Z), (fy1, SPL_Z), (fy1, SPL_Z + 0.050), (fy0, SPL_Z + 0.080)]
    carlib.plate(p, M.carbon, fence, sx * (L.x_at((fy0 + fy1) / 2, 0.135) - 0.028), 0.012,
                 chamfer=0.004)
    # canards on the fender flank, stacked
    dy = carlib.surface_station(L, 0.80, 0.42, NOSE, margin=0.02)
    dx = L.x_at(dy + 0.12, 0.42)
    for k, dz in enumerate((0.0, 0.085)):
        cp = [(dy, 0.395 + dz), (dy + 0.24 - k * 0.05, 0.425 + dz),
              (dy + 0.24 - k * 0.05, 0.445 + dz), (dy, 0.415 + dz)]
        carlib.plate(p, M.carbon, cp, sx * (dx + 0.050 - k * 0.010), 0.090, chamfer=0.007)
for sx in (-1, 1):
    skirt = [(AX_F + 0.30, 0.062), (AX_R - 0.30, 0.062), (AX_R - 0.30, 0.150),
             (AX_F + 0.30, 0.145)]
    carlib.plate(p, M.carbon, skirt, sx * (SILL_X + 0.010), 0.030, chamfer=0.008)
    # accent stripe along the sill, the slot the client paints per team
    stripe = [(AX_F + 0.34, 0.165), (AX_R - 0.34, 0.165), (AX_R - 0.34, 0.245),
              (AX_F + 0.34, 0.240)]
    carlib.plate(p, M.accent, stripe, sx * (SILL_X + 0.004), 0.008)
DIF_Y0, DIF_Y1 = AX_R + 0.20, TAIL - 0.02
DIF_HW = min(L.x_at(y, 0.17) for y in (DIF_Y0, (DIF_Y0 + DIF_Y1) / 2, DIF_Y1 - 0.05)) - 0.045
carlib.diffuser(p, M.carbon, DIF_Y0, DIF_Y1, DIF_HW, 0.055, 0.245,
                thick=0.014, strakes=(-0.72, -0.44, -0.16, 0.16, 0.44, 0.72), strake_h=0.20)

# ---- rear wing: two elements in the car's own plan, its own endplates,
# mounts and brake light. Every endplate keeps inside the first one's
# envelope (its top, WZ + 0.255, is the top of the mesh box the eye comes
# from; its back, WY + 0.515, the back of the car).
WY, WHW = 1.90, 0.925
PLAN, AMT = V["wing_plan"]
# (pass 7: inverted - leading edges low, trailing edges up, camber down;
# the first cut's sloped down to the rear, a lifting wing)
W_ANG, F_ANG = 7.0, 28.0
(te_y, te_z), TE = carlib.wing(p, M.carbon, WHW, 0.360, 0.100, -0.055, WY, WZ - 0.035, angle_deg=W_ANG,
                               plan=PLAN, amount=AMT)
(f2_y, f2_z), TE2 = carlib.wing(p, M.carbon, WHW, 0.145, 0.095, -0.050, te_y - 0.030, te_z + 0.012,
                                angle_deg=F_ANG, plan=PLAN, amount=AMT)
for (xa, xb, y, z) in carlib.spans(TE2, -WHW, WHW, 12):
    carlib.gurney(p, M.carbon, xa, xb, y, z, h=0.022, t=0.005, angle_deg=F_ANG)
_a = math.radians(W_ANG)
_dz = TE(0.0)[1] - TE(WHW)[1]
_dy = TE(0.0)[0] - TE(WHW)[0]
carlib.flat_decal(p, M.sponsor, (0.0, WY + 0.16 * math.cos(_a) - 0.020 * math.sin(_a) + _dy,
                                 WZ - 0.035 + 0.16 * math.sin(_a) + 0.020 * math.cos(_a) + _dz),
                  (-1, 0, 0), (0, -math.cos(_a), -math.sin(_a)), 0.76, 0.19, carlib.atlas_uv(SP["wing"]),
                  lift=0.004)
if V["wing_led"] in ("trail", "centre"):
    lx = WHW - 0.05 if V["wing_led"] == "trail" else 0.30
    for (xa, xb, y, z) in carlib.spans(TE2, -lx, lx, 12 if V["wing_led"] == "trail" else 4):
        carlib.led_strip(p, M, xa, xb, y - 0.038, z, h=0.024, t=0.012, glow=M.brake, dir_y=1.0)


def wing_dz(x):
    return TE(x)[1] - te_z


EPS = {
    # tops at WZ + 0.13: the imported prototypes' endplates stop 10-15 cm
    # over the plane (the class height limit), not 25
    "square": [(WY - 0.10, WZ - 0.185), (WY + 0.50, WZ - 0.135), (WY + 0.515, WZ + 0.110),
               (WY + 0.12, WZ + 0.130), (WY - 0.10, WZ + 0.060)],
    "swoop": [(WY - 0.08, WZ - 0.150), (WY + 0.42, WZ - 0.185), (WY + 0.515, WZ - 0.080),
              (WY + 0.515, WZ + 0.130), (WY + 0.36, WZ + 0.125), (WY + 0.08, WZ + 0.070),
              (WY - 0.10, WZ + 0.020)],
    "raked": [(WY - 0.02, WZ - 0.185), (WY + 0.44, WZ - 0.185), (WY + 0.515, WZ + 0.040),
              (WY + 0.46, WZ + 0.130), (WY + 0.14, WZ + 0.130), (WY - 0.10, WZ + 0.015)],
    "louvred": [(WY - 0.10, WZ - 0.160), (WY + 0.48, WZ - 0.185), (WY + 0.515, WZ + 0.105),
                (WY + 0.44, WZ + 0.130), (WY + 0.00, WZ + 0.130), (WY - 0.10, WZ + 0.080)],
}
for sx in (-1, 1):
    carlib.plate(p, M.carbon, EPS[V["endplate"]], sx * (WHW + 0.013), 0.016, chamfer=0.006)
    xo = WHW + 0.021
    if V["endplate"] == "louvred":
        # three slots down the endplate's rear half, dark with a carbon lip
        for k in range(3):
            zz = WZ - 0.14 + 0.06 * k
            p.box(M.lamp_h, (min(sx * xo, sx * (xo + 0.004)), WY + 0.28, zz),
                  (max(sx * xo, sx * (xo + 0.004)), WY + 0.44, zz + 0.022))
    if V["wing_led"] == "endplate":
        # a brake strip up the endplate's trailing edge
        p.box(M.lamp_h, (min(sx * xo, sx * (xo + 0.008)), WY + 0.455, WZ - 0.12),
              (max(sx * xo, sx * (xo + 0.008)), WY + 0.480, WZ + 0.08))
        p.box(M.brake, (min(sx * (xo + 0.006), sx * (xo + 0.010)), WY + 0.459, WZ - 0.112),
              (max(sx * (xo + 0.006), sx * (xo + 0.010)), WY + 0.476, WZ + 0.072))
MOUNT = V["mount"]
if MOUNT in ("swan", "swan_wide"):
    mx = 0.44 if MOUNT == "swan" else 0.62
    for sx in (-1, 1):
        carlib.swan_neck(p, M.carbon, sx * mx, (0, WY - 0.34, L.z_at(WY - 0.34, mx) - 0.03),
                         (0, WY + 0.09, WZ + 0.02 + wing_dz(mx)), r=0.024)
elif MOUNT == "centre":
    carlib.swan_neck(p, M.carbon, 0.0, (0, WY - 0.34, L.roof_z(WY - 0.34) - 0.03),
                     (0, WY + 0.09, WZ + 0.02 + wing_dz(0.0)), r=0.030)
else:
    for sx in (-1, 1):
        dz = wing_dz(0.34)
        py = [(WY - 0.04, L.roof_z(WY - 0.04) - 0.02), (WY + 0.22, L.roof_z(WY + 0.22) - 0.02),
              (WY + 0.26, WZ + dz - 0.02), (WY + 0.02, WZ + dz - 0.02)]
        carlib.plate(p, M.carbon, py, sx * 0.34, 0.026, chamfer=0.008)
if V["fin"]:
    fin = [(SIDE_Y[1] + 0.02, L.roof_z(SIDE_Y[1] + 0.02) - 0.01),
           (SIDE_Y[1] + 0.34, L.roof_z(SIDE_Y[1] + 0.34) + 0.125),
           (WY + 0.12, WZ - 0.055), (WY + 0.12, WZ - 0.165),
           (AX_R + 0.10, L.roof_z(AX_R + 0.10) - 0.02)]
    carlib.plate(p, M.carbon, fin, 0.0, 0.018, chamfer=0.006)

# ---- lights. The headlamp aperture is a lined hole in the nose; inside it
# a black back wall, projector modules set back from the skin (each at its
# own station, the nose being curved), an L-shaped DRL guide along the
# outboard and lower edges, and a clear lens flush with the skin over the
# hole. The tail aperture gets a back wall, an L light guide, a brake matrix
# and a smoked lens; the rain light is an LED matrix.
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    n = {"T": 3, "points": 2, "blade": 3, "claws": 2}[V["drl"]]
    cz = (LAMP_Z[0] + LAMP_Z[1]) / 2
    r = min((x1 - x0) / (2.3 * n), (LAMP_Z[1] - LAMP_Z[0]) * 0.36)
    p.box(M.lamp_h, (x0, LAMP_Y + 0.115, LAMP_Z[0]), (x1, LAMP_Y + 0.135, LAMP_Z[1]))
    for k in range(n):
        cx = x0 + 0.03 + r + ((x1 - x0) - 0.06 - 2 * r) * (k / (n - 1) if n > 1 else 0.5)
        cy = carlib.surface_station(L, abs(cx) + r, cz, NOSE, margin=0.010) + 0.030
        carlib.projector(p, M, (cx, cy, cz), (0.0, -1.0, 0.0), r, depth=0.080)
        # a black mask plate round each module, just behind its face, so the
        # hole shows a bezel and a lit face rather than the side of a can
        # (narrow: on the lower pass-5 nose the plate's outer corner stood
        # proud of the skin where the fender front curves away)
        p.box(M.lamp_h, (cx - r - 0.014, cy + 0.004, LAMP_Z[0] - 0.002), (cx + r + 0.014, cy + 0.014, LAMP_Z[1] + 0.002))
    # DRL signature, one per maker, a hand inside the skin
    xo = x1 - 0.022 if sx > 0 else x0 + 0.022
    xi = x0 + 0.022 if sx > 0 else x1 - 0.022
    zlo, zhi = LAMP_Z[0] + 0.016, LAMP_Z[1] - 0.016

    def skin(x, z, back=0.018):
        return carlib.surface_station(L, abs(x), z, NOSE, margin=0.005) + back

    if V["drl"] == "T":
        # Yotota: a vertical blade at the outboard edge, a short bar off its middle
        carlib.guide_xyz(p, M.lamp, [(xo, skin(xo, zhi), zhi), (xo, skin(xo, zlo), zlo)], r=0.0055, segs=8)
        xm = xo - sx * 0.16
        carlib.guide_xyz(p, M.lamp, [(xo, skin(xo, cz), cz), (xm, skin(xm, cz), cz)], r=0.0045, segs=8)
    elif V["drl"] == "points":
        # Posh: four DRL points round the projectors, the marque's face
        for (dx, dz) in ((0.06, 0.038), (0.06, -0.038), (-0.06, 0.038), (-0.06, -0.038)):
            for k in range(n):
                cx = x0 + 0.03 + r + ((x1 - x0) - 0.06 - 2 * r) * (k / (n - 1) if n > 1 else 0.5)
                px = cx + sx * dx * (r / 0.043)
                carlib.projector(p, M, (px, skin(px, cz + dz, 0.024), cz + dz), (0.0, -1.0, 0.0), 0.009,
                                 depth=0.016, chrome=False)
    elif V["drl"] == "blade":
        # Fugazzi: one thin horizontal blade along the top of the aperture
        carlib.guide_xyz(p, M.lamp, [(xi, skin(xi, zhi), zhi), (xo, skin(xo, zhi), zhi)], r=0.0050, segs=8)
    else:
        # Jeanetti: three vertical claws at the outboard end
        for k in range(3):
            cx = xo - sx * 0.032 * k
            carlib.guide_xyz(p, M.lamp, [(cx, skin(cx, zhi), zhi), (cx, skin(cx, zlo + 0.012 * k), zlo + 0.012 * k)],
                             r=0.0045, segs=8)
    carlib.front_lens(p, M.glass, L, x0 - 0.004, x1 + 0.004, LAMP_Z[0] - 0.004, LAMP_Z[1] + 0.004,
                      NOSE, lift=-0.0025, nu=8, nv=4)
    # tail
    t0, t1 = sorted((sx * 0.30, sx * 0.76))
    zt0, zt1 = TAILL_TOP - 0.125, TAILL_TOP + 0.008
    p.box(M.lamp_h, (t0, TAILL_Y - 0.20, zt0), (t1, TAILL_Y - 0.18, zt1))
    xo = t1 - 0.025 if sx > 0 else t0 + 0.025
    xi = t0 + 0.025 if sx > 0 else t1 - 0.025
    ty = TAILL_Y - 0.05
    if V["tail"] == "double":
        # Yotota: two horizontal stripes the width of the lamp, brake row under them
        for zz in (zt1 - 0.020, zt1 - 0.052):
            carlib.guide_xyz(p, M.tail, [(xi, ty, zz), (xo, ty, zz)], r=0.0060, segs=8)
        bb0, bb1 = sorted((xi, xo))
        carlib.led_grid(p, M, bb0, bb1, zt0 + 0.014, zt0 + 0.046, TAILL_Y - 0.062, dir_y=-1.0,
                        cols=8, rows=1, glow=M.brake)
    elif V["tail"] == "bar":
        # Posh: one stripe that runs on across the tail panel to the other lamp
        carlib.guide_xyz(p, M.tail, [(xi, ty, zt1 - 0.022), (xo, ty, zt1 - 0.022)], r=0.0070, segs=8)
        bb0, bb1 = sorted((xi, xo - sx * 0.10))
        carlib.led_grid(p, M, bb0, bb1, zt0 + 0.016, zt1 - 0.050, TAILL_Y - 0.062, dir_y=-1.0,
                        cols=6, rows=2, glow=M.brake)
    elif V["tail"] == "rings":
        # Fugazzi: twin round light-guide rings, brake strip along the bottom
        rr = (zt1 - zt0) * 0.36
        for cx in (xi + sx * (rr + 0.02), xo - sx * (rr + 0.02)):
            ring = [(cx + rr * math.cos(a), ty, (zt0 + zt1) / 2 + 0.012 + rr * math.sin(a))
                    for a in (2 * math.pi * k / 24 for k in range(25))]
            carlib.guide_xyz(p, M.tail, ring, r=0.0060, segs=8)
            carlib.projector(p, M, (cx, ty + 0.002, (zt0 + zt1) / 2 + 0.012), (0.0, 1.0, 0.0), rr * 0.55,
                             depth=0.04, glow=M.tail, chrome=False)
        bb0, bb1 = sorted((xi, xo))
        carlib.led_grid(p, M, bb0, bb1, zt0 + 0.010, zt0 + 0.030, TAILL_Y - 0.062, dir_y=-1.0,
                        cols=10, rows=1, glow=M.brake)
    else:
        # Jeanetti: three vertical claws at the outboard end, brake block inboard
        for k in range(3):
            cx = xo - sx * 0.055 * k
            carlib.guide_xyz(p, M.tail, [(cx, ty, zt1 - 0.018 - 0.010 * k), (cx, ty, zt0 + 0.018)], r=0.0065, segs=8)
        bb0, bb1 = sorted((xi, xo - sx * 0.16))
        carlib.led_grid(p, M, bb0, bb1, zt0 + 0.018, zt1 - 0.030, TAILL_Y - 0.062, dir_y=-1.0,
                        cols=4, rows=3, glow=M.brake)
    p.box(M.lens_tint, (t0 + 0.002, TAILL_Y - 0.008, zt0 + 0.002), (t1 - 0.002, TAILL_Y - 0.003, zt1 - 0.002))
carlib.led_grid(p, M, -0.048, 0.048, RAIN_Z[0] + 0.025, RAIN_Z[1] - 0.025, RAIN_Y - 0.020, dir_y=-1.0,
                cols=2, rows=6, glow=M.rain)
if V["tail"] == "bar":
    zb = min(TAILL_TOP - 0.014, L.roof_z(TAIL - 0.05) - 0.030)
    yb = max(carlib.surface_station(L, x, zb, TAIL, margin=0.0) for x in (0.0, 0.15, 0.30)) + 0.004
    carlib.tail_bar(p, M, -0.30, 0.30, yb, zb, h=0.036, glow=M.tail, dir_y=-1.0)

# ---- blades in the vents cut into the loft
for ax in (AX_F, AX_R):
    for sx in (-1, 1):
        z0, z1 = L.point(ax, 3.4).z, L.point(ax, 4.2).z
        for k in range(4):
            yv = ax - 0.13 + k * 0.070
            zt = L.point(yv, 3.75).z
            xv = L.point(yv, 3.75).x
            p.box(M.carbon, (sx * (xv - 0.20), yv, zt - 0.030),
                  (sx * (xv + 0.02), yv + 0.034, zt - 0.006))
# the flank's vents: blades leaning with each one, a dark floor under them
for (f, n, r) in SIDE_BLADES:
    for sx in (-1, 1):
        carlib.swept_blades(p, M.carbon, L, f, count=n, sx=sx, r=r)
for f in SIDE_FLOORS:
    for sx in (-1, 1):
        carlib.swept_floor(p, M.mesh, L, f, sx=sx)
# the face's mouths: mesh backing, the car's bars, the boomerang's strut
for (spec, poly, yb) in FACE:
    carlib.poly_fill(p, M.mesh, poly, yb, yb + 0.012)
    for (ang, pitch) in spec["bars"]:
        carlib.poly_bars(p, M.carbon, poly, yb - 0.030, angle_deg=ang, pitch=pitch,
                         t=0.008 if ang == 90.0 else 0.007, depth=0.028)
    if spec.get("strut"):
        zs = [z for (_, z) in poly]
        z0, z1 = min(zs) + 0.004, 0.196
        yf = carlib.surface_station(L, 0.02, z0, NOSE, step=0.004, margin=0.0) + 0.006
        carlib.plate(p, M.carbon, [(yf, z0), (yb, z0), (yb, z1), (yf + 0.02, z1)], 0.0, 0.060,
                     chamfer=0.008)
for k in range(4):
    yv = SIDE_Y[1] + 0.29 + k * 0.105
    zt = L.roof_z(yv) - 0.026
    p.box(M.carbon, (-0.34, yv, zt - 0.012), (0.34, yv + 0.050, zt + 0.012))

# ---- roof intake, mirrors, exhaust, antenna, tow hooks, wiper
sc_x, sc_y, sc_h = V["scoop"]
SC_Y = SIDE_Y[0] + sc_y
sc_z = L.roof_z(SC_Y) - 0.010
# a low snorkel that grows out of the roof and tapers away aft, rather than a
# box standing on the engine cover
sc_prof = [(SC_Y - 0.26, sc_z), (SC_Y - 0.13, sc_z + sc_h * 0.92),
           (SC_Y + 0.10, sc_z + sc_h), (SC_Y + 0.34, sc_z + sc_h * 0.35),
           (SC_Y + 0.40, sc_z)]
carlib.plate(p, M.carbon, sc_prof, 0.0, sc_x * 1.15, chamfer=0.035)
p.box(M.mesh, (-sc_x * 0.42, SC_Y - 0.272, sc_z + 0.020),
      (sc_x * 0.42, SC_Y - 0.252, sc_z + sc_h * 0.80))
MIR_Y = SCREEN_Y[0] + 0.24
# a deeper valley would drop the mirror onto the wider fender and push it
# out, widening the mesh box the client derives the driver's eye from
MIR_Z = L.point(MIR_Y, 5.10).z + 0.035 - min(V.get("valley", 0.0), 0.0)
MIRRORS = {}
for sx, key in ((1, "left"), (-1, "right")):
    MIRRORS[key] = carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.115, MIR_Y, MIR_Z, sx=sx,
                                 style=V["mirror"], glass=MIR[key], size=(0.18, 0.08, 0.12))
for x in (-0.22, 0.22):
    p.cylinder(M.lamp_h, (x, TAIL - 0.15, 0.300), 0.062, 0.075, segs=20, axis='Y')
    p.cylinder(M.metal, (x, TAIL - 0.16, 0.300), 0.050, 0.20, segs=20, axis='Y')
p.bar(M.carbon, (0.18, SIDE_Y[1] - 0.10, L.roof_z(SIDE_Y[1] - 0.10) - 0.01),
      (0.18, SIDE_Y[1] - 0.10, L.roof_z(SIDE_Y[1] - 0.10) + 0.18), 0.006)
for yy in (NOSE + 0.14, TAIL - 0.12):
    p.torus(M.towhook, (0.26, yy, 0.215), 0.052, 0.013, segs=16, rings=8)
wy = SCREEN_Y[0] - 0.02
p.bar(M.trim, (DX - 0.40, wy, L.z_at(wy, 0.34) + 0.010),
      (DX + 0.24, wy + 0.03, L.z_at(wy + 0.03, 0.34) + 0.010), 0.009, segs=8)

# ---- daytime-running blades under each headlamp cluster
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    dy = carlib.surface_station(L, max(abs(x0), abs(x1)), LAMP_Z[0] - 0.03, NOSE, margin=0.02)
    carlib.led_strip(p, M, x0 + 0.02, x1 - 0.02, dy + 0.006, LAMP_Z[0] - 0.030, h=0.016,
                     t=0.012, glow=M.lamp, dir_y=1.0)

# ---- small hardware: clam pins, door latch, filler, roof camera pod
for (x, yy) in ((-0.50, NOSE + 0.58), (0.50, NOSE + 0.58), (-0.82, NOSE + 0.70), (0.82, NOSE + 0.70)):
    pz = L.z_at(yy, abs(x))
    p.cylinder(M.metal, (x, yy, pz - 0.004), 0.015, 0.010, segs=12, axis='Z')
    p.cylinder(M.trim, (x, yy, pz + 0.006), 0.007, 0.006, segs=8, axis='Z')
for sx in (-1, 1):
    ly = SIDE_Y[1] - 0.14
    lz = L.point(ly, 2.6).z
    lx = L.x_at(ly, lz)
    p.box(M.trim, (min(sx * (lx - 0.008), sx * (lx + 0.003)), ly - 0.05, lz - 0.014),
          (max(sx * (lx - 0.008), sx * (lx + 0.003)), ly + 0.05, lz + 0.014))
fy = AX_R - 0.55
fz = L.z_at(fy, 0.62)
p.cylinder(M.metal, (0.62, fy, fz - 0.008), 0.042, 0.014, segs=16, axis='Z')
p.cylinder(M.trim, (0.62, fy, fz + 0.006), 0.033, 0.004, segs=16, axis='Z')
cy = SIDE_Y[1] - 0.02
cz = L.z_at(cy, 0.22)
p.box(M.trim, (0.22 - 0.04, cy - 0.05, cz - 0.01), (0.22 + 0.04, cy + 0.05, cz + 0.03))
p.cylinder(M.glass, (0.22, cy + 0.05, cz + 0.016), 0.010, 0.006, segs=10, axis='Y')

# ---- race number on the nose, on a white plate that follows the deck
NUM = V.get("number", "1")
carlib.top_decal(p, M.number, L, 0.0, NOSE + 0.48, 0.32, 0.32, carlib.number_uv(NUM), along_y=False,
                 lift=0.005, nu=8, nv=8)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
# No mesh steering wheel: the client's cockpit rig draws its own at the
# derived wheel point. The cabin is a prototype tub - narrow, the driver on
# the centreline-ish, a bulkhead behind him - built to the same points the
# GT3 cabin is, so the cockpit view reads the same in both classes.
DASH_Z = min(EYE.z - 0.22, L.roof_z(SCREEN_Y[0]) - 0.03)
DASH_Y0, DASH_Y1 = SCREEN_Y[0] - 0.02, CK["wheel"].y - 0.28
# inside the cabin, not the pods (pass 7: measured at the dash height the
# walls stood out at the sidepods' width, x 0.86)
XIN = min(L.x_at(y, max(DASH_Z, 0.78)) for y in (DASH_Y0 + 0.10, -0.3 + CS, 0.0 + CS, 0.3 + CS, SIDE_Y[1] - 0.05)) - 0.050
HOOP_Y = SIDE_Y[1] - 0.06
BULK_Y = HOOP_Y + 0.03
BELT_Z = L.point(0.0, CAN_LO).z
p.box(M.interior, (-XIN, DASH_Y0, 0.095), (XIN, BULK_Y, 0.135))                  # floor
p.box(M.alcantara, (-XIN + 0.02, DASH_Y0 + 0.02, 0.135), (XIN - 0.02, EYE.y - 0.50, 0.140))   # footwell mat
p.box(M.interior, (-0.14, DASH_Y0, 0.135), (0.14, BULK_Y, 0.30))                  # tunnel
p.box(M.trim, (-0.16, DASH_Y0, 0.30), (0.16, BULK_Y, 0.315))
carlib.switch_panel(p, M, -0.13, 0.13, CK["wheel"].y - 0.18, CK["wheel"].y + 0.20, 0.325,
                    rows=3, cols=3, rotary=True)
p.box(M.interior, (-XIN, BULK_Y, 0.135), (XIN, BULK_Y + 0.03, BELT_Z - 0.06))     # bulkhead
p.box(M.trim, (-XIN, BULK_Y - 0.01, BELT_Z - 0.06), (XIN, BULK_Y + 0.04, BELT_Z - 0.03))
p.box(M.interior, (-XIN, DASH_Y0, 0.54), (XIN, DASH_Y1, DASH_Z - 0.06))           # dash
p.box(M.alcantara, (-XIN, DASH_Y0, DASH_Z - 0.06), (XIN, DASH_Y1, DASH_Z))
p.bar(M.alcantara, (-XIN, DASH_Y1, DASH_Z - 0.016), (XIN, DASH_Y1, DASH_Z - 0.016), 0.016, segs=10)
hood = [(DASH_Y1 - 0.28, DASH_Z), (DASH_Y1 + 0.02, DASH_Z), (DASH_Y1 + 0.02, DASH_Z + 0.040),
        (DASH_Y1 - 0.22, DASH_Z + 0.040)]
carlib.plate(p, M.alcantara, hood, DX, 0.32, chamfer=0.010)
p.box(M.trim, (-0.09, DASH_Y1 - 0.06, DASH_Z), (0.09, DASH_Y1 - 0.01, DASH_Z + 0.040))
p.box(M.display, (-0.075, DASH_Y1 - 0.012, DASH_Z + 0.008), (0.075, DASH_Y1 - 0.008, DASH_Z + 0.035))
carlib.switch_panel(p, M, DX - 0.40, DX - 0.19, DASH_Y1 - 0.11, DASH_Y1 - 0.02, DASH_Z + 0.001,
                    rows=1, cols=3, rotary=False)
for sx in (-1, 1):
    carlib.door_card(p, M, sx, XIN, SIDE_Y[0] + 0.02, SIDE_Y[1] - 0.02, 0.18, BELT_Z - 0.035,
                     pull=True)
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), DASH_Y0, 0.135),
          (max(sx * XIN, sx * (XIN + 0.02)), SIDE_Y[0] + 0.02, 0.54))
    p.box(M.interior, (min(sx * XIN, sx * (XIN + 0.02)), SIDE_Y[1] - 0.02, 0.135),
          (max(sx * XIN, sx * (XIN + 0.02)), BULK_Y + 0.03, BELT_Z - 0.06))
SEAT_Y0 = EYE.y - 0.46
carlib.bucket_seat(p, M, DX, SEAT_Y0, 0.135, width=0.48, depth=0.52, back_h=0.62, rake_deg=24.0)
PED_Y = EYE.y - 1.10
for x in (DX - 0.19, DX - 0.07, DX + 0.05):
    p.box(M.metal, (x - 0.030, PED_Y - 0.06, 0.145), (x + 0.030, PED_Y, 0.275))
p.box(M.metal, (DX + 0.13, PED_Y - 0.08, 0.145), (DX + 0.20, PED_Y + 0.02, 0.26))
carlib.extinguisher(p, M, -0.40, SEAT_Y0 + 0.10, 0.20, r=0.048, length=0.32)
carlib.inner_skin(p, M.alcantara, L, SCREEN_Y[1] + 0.03, SIDE_Y[1] - 0.03, 0.40,
                  drop=0.030, nu=10, nv=6)
# roll structure inside the canopy: the A-pillar bar runs down the edge of
# the screen from the canopy's shoulder, not across the driver's face
for sx in (-1, 1):
    top = (sx * 0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050)
    carlib.inside_bar(p, M.cage, L, (sx * 0.56, HOOP_Y, 0.20), (sx * 0.56, HOOP_Y, top[2] - 0.12), 0.022, segs=8)
    carlib.inside_bar(p, M.cage, L, (sx * 0.56, HOOP_Y, top[2] - 0.12), top, 0.022, segs=8)
    # the screen's top corner is on the canopy's own edge (j7), ~0.3 m off
    # centre now that the canopy is prototype-narrow; at x = 0.50 the bar
    # ran down the middle of the driver's view
    sp = L.point(SCREEN_Y[1], 7.0)
    scr = (sx * (sp.x - 0.025), SCREEN_Y[1], sp.z - 0.035)
    carlib.inside_bar(p, M.cage, L, top, scr, 0.019, segs=8)
    # the bar's foot is at the canopy's own edge (the glass starts at CAN_LO),
    # not out on the fender: with the driver near the centreline a bar from
    # the fender crossed his view (pass-5 cockpit render)
    fx = L.point(wy + 0.02, CAN_LO).x - 0.03
    foot = (sx * fx, wy + 0.02, L.z_at(wy + 0.02, fx + 0.02) - 0.040)
    carlib.inside_bar(p, M.cage, L, scr, foot, 0.013, segs=8)
    carlib.inside_bar(p, M.cage, L, (sx * 0.56, HOOP_Y, top[2] - 0.18), (sx * 0.44, AX_R - 0.15, 0.50),
          0.019, segs=8)
    carlib.inside_bar(p, M.cage, L, (sx * 0.56, HOOP_Y, 0.52), (sx * 0.55, -0.62, 0.56), 0.019, segs=8)
carlib.inside_bar(p, M.cage, L, (-0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050),
      (0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050), 0.022, segs=8)

# ---- wordmark, laid on the flank rather than floated off it
# ends at the door shut: run on over the side intake it hid the intake
# Pass 7: the door's number panel behind the front wheel, the wordmark
# behind it; a sponsor on each front fender's crown and the endplates'
NUM_Y = (AX_F + 0.54, AX_F + 0.82)
DEC_Y = (AX_F + 0.90, SIDE_Y[1] + 0.02)
for sx in (-1, 1):
    carlib.conform_decal(p, M.number, L, NUM_Y[0], NUM_Y[1], 1.75, 3.00, sx=sx, lift=0.005, nu=8, nv=8,
                         flip_u=sx < 0, uv_rect=carlib.number_uv(NUM))
    carlib.conform_decal(p, M.logo, L, DEC_Y[0], DEC_Y[1], 1.90, 2.85, sx=sx,
                         lift=0.005, nu=14, nv=6, flip_u=sx < 0)
    _fx = L.point(AX_F - 0.48, 3.5).x
    carlib.top_decal(p, M.sponsor, L, sx * _fx, AX_F - 0.48, 0.28, 0.07, carlib.atlas_uv(SP["fender"]),
                     along_y=True, read_from=sx, lift=0.005, nu=8, nv=3)
    carlib.flat_decal(p, M.sponsor, (sx * (WHW + 0.0215), WY + 0.22, WZ - 0.03), (0, sx, 0), (0, 0, 1),
                      0.32, 0.08, carlib.atlas_uv(SP["ep"]), lift=0.0005)

# ---- the driver (pass 7): on the bucket, gloves on the rig's rim, feet on
# the pedals; exported apart from the body (carlib.export_driver)
drv = Builder("driver")
carlib.driver_figure(drv, M, EYE, CK["wheel"], (DX, SEAT_Y0 + 0.35, 0.135 + 0.21), (PED_Y, 0.18),
                     wheel_hw=0.16, suit=V["paint"])

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0040, segments=2,
                     angle_deg=38.0)
parts_objs.append(parts)
save("parts")

# ------------------------------------------------------------ join + export
car, glb = carlib.join_and_export([body] + parts_objs, V["stem"], CAR_DIR,
                                  export=os.environ.get("APEX_EXPORT", "1") == "1")
# the driver, in his own GLB (pass 7; see carlib.driver_figure)
DRIVER = carlib.export_driver(drv, V["stem"], CAR_DIR, export=bool(glb))
save("joined")
print("stats:", carlib.mesh_stats(car))
print("sightline:", carlib.sightline(car, eye=EYE))
if glb:
    carlib.write_cockpit_table(CAR_DIR, CK, 180.0, mirrors=MIRRORS,
                               note="LMP2: the seat ahead of the middle of the wheelbase, a hand off centre.")
    print("GLB:", glb, os.path.getsize(glb))
