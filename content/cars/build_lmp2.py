"""Build one of the LMP2 prototypes in Blender and export it to
content/cars/<folder>/<stem>.glb.

    VARIANT = "posh"       # yotota | posh | fugazzi | jeanetti
    exec(open(r"D:\\apexsim\\content\\cars\\build_lmp2.py").read())

The four cars share one hull generator; each VARIANT is a set of small shape
and livery parameters (nose width, fender peak, canopy height and position,
tail height, wing height, fin, lights, paint, logo). Everything below the
shape data comes from content/cars/carlib.py, shared with build_gt3.py.

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
                   scoop=(0.30, 0.62, 0.16), seat=(0.10, 0.10, 0.32),
                   # the plain one: square twin mouths, an upright intake
                   nose_z=0.0, valley=0.0, face="twin", side="upright"),
    "posh": dict(folder="posh-lmp2", stem="posh_lmp2", logo="posh_logo.png",
                 paint=(0.50, 0.51, 0.54), accent=(0.04, 0.04, 0.045), caliper=(0.95, 0.75, 0.05),
                 paint_metallic=0.90, number="22", bonnet_drop=(0.02, 0.05), drl="points", tail="bar",
                 nose_w=1.06, fender=1.03, roof=0.97, canopy_shift=-0.10, tail_h=0.94,
                 side_w=1.00, wing_z=-0.04, fin=True, lights="round", mirror="pod",
                 scoop=(0.34, 0.56, 0.15), seat=(0.10, 0.10, 0.12),
                 # smooth: filled-in valleys, a centre mouth between corner
                 # intakes, an intake that sweeps back under a waist line
                 nose_z=0.020, valley=0.035, face="tri", side="sweep",
                 lamp_x=(0.24, 0.56), lamp_h=0.130),
    "fugazzi": dict(folder="fugazzi-lmp2", stem="fugazzi_lmp2", logo="fugazzi_logo.png",
                    paint=(0.62, 0.02, 0.03), accent=(0.95, 0.78, 0.05), caliper=(0.95, 0.80, 0.05),
                    paint_metallic=0.65, number="51", bonnet_drop=(0.02, 0.05), drl="blade", tail="rings",
                    nose_w=0.90, fender=1.05, roof=1.00, canopy_shift=0.12, tail_h=1.04,
                    side_w=0.99, wing_z=0.02, fin=True, lights="tri", mirror="stalk",
                    scoop=(0.26, 0.68, 0.17), seat=(0.16, 0.05, 0.05),
                    # sharp: drooped nose, deep valleys, one boomerang mouth,
                    # the long raked slash of the marque's hypercar
                    nose_z=-0.025, valley=-0.045, face="boomerang", side="slash",
                    lamp_x=(0.22, 0.66), lamp_h=0.070),
    "jeanetti": dict(folder="jeanetti-lmp2", stem="jeanetti_lmp2", logo="jeanetti_logo.png",
                     paint=(0.02, 0.20, 0.10), accent=(0.95, 0.82, 0.18), caliper=(0.20, 0.20, 0.22),
                     paint_metallic=0.60, number="38", bonnet_drop=(0.02, 0.05), drl="claws", tail="claws",
                     nose_w=1.00, fender=0.98, roof=1.03, canopy_shift=0.05, tail_h=1.00,
                     side_w=1.02, wing_z=0.05, fin=False, lights="bar", mirror="pod",
                     scoop=(0.32, 0.52, 0.18), seat=(0.08, 0.10, 0.06),
                     # clawed: two tall mouths leaning in at the top, three
                     # gills behind the front wheel, an intake leaning forward
                     nose_z=0.012, valley=0.010, face="claw", side="gills",
                     lamp_x=(0.30, 0.66), lamp_h=0.090),
}

# Base hull. Right-half section control points (x, z).
KEYS = [
    (-2.45, [(0, .105), (.50, .105), (.565, .155), (.575, .235), (.46, .285), (.31, .295), (.18, .305), (.08, .315), (0, .32)]),
    (-2.25, [(0, .065), (.78, .065), (.865, .195), (.875, .395), (.72, .455), (.51, .415), (.31, .40), (.14, .41), (0, .41)]),
    (-1.85, [(0, .055), (.915, .055), (.965, .395), (.975, .805), (.79, .875), (.585, .755), (.36, .615), (.16, .575), (0, .565)]),
    (-1.50, [(0, .055), (.935, .055), (.975, .445), (.985, .865), (.79, .925), (.585, .795), (.36, .655), (.16, .615), (0, .605)]),
    (-1.10, [(0, .055), (.945, .055), (.975, .415), (.965, .755), (.785, .815), (.605, .715), (.385, .695), (.18, .715), (0, .715)]),
    (-0.60, [(0, .055), (.945, .055), (.965, .395), (.955, .635), (.785, .675), (.625, .655), (.425, .835), (.245, .955), (0, .985)]),
    (-0.10, [(0, .055), (.945, .055), (.965, .395), (.955, .625), (.785, .655), (.635, .645), (.445, .955), (.265, 1.045), (0, 1.065)]),
    (0.45, [(0, .055), (.945, .055), (.965, .395), (.955, .635), (.785, .675), (.645, .675), (.445, .945), (.265, 1.035), (0, 1.055)]),
    (1.00, [(0, .055), (.945, .055), (.975, .415), (.965, .775), (.785, .835), (.625, .775), (.405, .875), (.205, .935), (0, .955)]),
    (1.50, [(0, .055), (.935, .055), (.975, .445), (.985, .875), (.785, .925), (.605, .835), (.365, .835), (.185, .855), (0, .865)]),
    (1.90, [(0, .075), (.925, .075), (.965, .415), (.955, .815), (.765, .855), (.585, .795), (.345, .775), (.165, .775), (0, .785)]),
    (2.30, [(0, .215), (.835, .215), (.895, .445), (.885, .695), (.705, .735), (.525, .715), (.305, .695), (.14, .695), (0, .695)]),
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
            # The canopy is a broader, taller bubble than the first keys drew:
            # the client seats the driver 18% of the car's width off centre,
            # which on a 0.45 m half-width canopy is inside the side glass,
            # and 70% of the box up, which was 6 cm under the roof.
            if j == 6 and -1.2 < y < 1.6:
                x *= 1.16
            if j == 7 and -1.2 < y < 1.6:
                x *= 1.14
            if j >= 7 and -0.9 < y < 1.3:
                z += 0.045 * (1.0 if -0.6 <= y <= 1.0 else 0.5)
            if j in (1, 2) and -1.9 < y < 2.0:
                x *= v["side_w"]
            if y > 1.7:
                z *= v["tail_h"]
            if y < -1.7 and j >= 2:
                z += v.get("nose_z", 0.0) * min(1.0, (-1.7 - y) / 0.6)
            if j in (4, 5) and -2.0 < y < 1.95:
                z += v.get("valley", 0.0) * (1.0 if j == 5 else 0.5)
            new.append((x, z))
        yy = y + (v["canopy_shift"] if -0.7 <= y <= 1.0 else 0.0)
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
                         paint_rough=0.22)

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
LAMP_Z = (0.350, 0.350 + (V.get("lamp_h") or {"round": 0.125, "tri": 0.120, "bar": 0.100}[V["lights"]]))
LAMP_Y = carlib.surface_station(L, LAMP_X[1], 0.40, NOSE, margin=0.075)
TAILL_Y = carlib.surface_station(L, 0.74, 0.55, TAIL, margin=0.03)
TAILL_TOP = min(L.roof_z(TAILL_Y) - 0.085, 0.63)
RAIN_Y = carlib.surface_station(L, 0.10, 0.44, TAIL, margin=0.05)

# Headlamp pocket: a shallow recessed panel so the cluster sits in a dent
# instead of a box glued onto the raw curve, and the per-cup surface-station
# drift has slack to land inside rather than tearing the skin at the edge.
L.recess(NOSE + 0.02, LAMP_Y + 0.34, 1.55, 4.60, depth=0.028, rim=0.15)
# Tail-lamp pocket and rain-light pocket, same idea at the back.
L.recess(TAILL_Y - 0.30, TAIL - 0.02, 2.65, 4.30, depth=0.022, rim=0.11)
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


body = L.build("body", face_mat, [M.paint, M.glass, M.trim, M.mesh], subsurf=0)
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
carlib.aperture(body, M, (-0.065, RAIN_Y - 0.14, 0.340), (0.065, RAIN_Y + 0.05, 0.580))
carlib.sharpen(body, 34.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
WZ = 1.05 + V["wing_z"]
TOP_Z = WZ + 0.255                          # the endplates: the tallest thing on the car
CK = carlib.cockpit_points(L, WZ, liner_z=0.036, top_z=TOP_Z)
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

# ---- rear wing: two elements, endplates, swan necks, gurney, brake LED
WY, WHW = 1.90, 0.925
te_y, te_z = carlib.foil(p, M.carbon, -WHW, WHW, 0.360, 0.100, 0.055, WY, WZ, angle_deg=-9.0)
f2_y, f2_z = carlib.foil(p, M.carbon, -WHW, WHW, 0.145, 0.095, 0.050,
                         WY + 0.315, WZ + 0.085, angle_deg=-24.0)
carlib.gurney(p, M.carbon, -WHW, WHW, f2_y, f2_z, h=0.022, t=0.005, angle_deg=-24.0)
carlib.led_strip(p, M, -WHW + 0.05, WHW - 0.05, f2_y - 0.038, f2_z, h=0.024, t=0.012,
                 glow=M.brake, dir_y=1.0)
for sx in (-1, 1):
    ep = [(WY - 0.10, WZ - 0.185), (WY + 0.50, WZ - 0.135), (WY + 0.515, WZ + 0.225),
          (WY + 0.12, WZ + 0.255), (WY - 0.10, WZ + 0.095)]
    carlib.plate(p, M.carbon, ep, sx * (WHW + 0.013), 0.016, chamfer=0.006)
    carlib.swan_neck(p, M.carbon, sx * 0.44, (0, WY - 0.34, WZ - 0.30),
                     (0, WY + 0.09, WZ + 0.02), r=0.024)
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
        p.box(M.lamp_h, (cx - r - 0.035, cy + 0.004, LAMP_Z[0] - 0.002), (cx + r + 0.035, cy + 0.014, LAMP_Z[1] + 0.002))
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
carlib.led_grid(p, M, -0.048, 0.048, 0.365, 0.555, RAIN_Y - 0.020, dir_y=-1.0,
                cols=2, rows=6, glow=M.rain)
if V["tail"] == "bar":
    zb = TAILL_TOP + 0.008 - 0.022
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
for sx in (-1, 1):
    carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.105, MIR_Y, MIR_Z, sx=sx,
                  style=V["mirror"], head=(0.068, 0.125, 0.050))
for x in (-0.095, 0.095):
    p.cylinder(M.lamp_h, (x, TAIL - 0.15, 0.315), 0.062, 0.075, segs=20, axis='Y')
    p.cylinder(M.metal, (x, TAIL - 0.16, 0.315), 0.050, 0.20, segs=20, axis='Y')
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
carlib.top_patch(p, M.decal, L, NOSE + 0.30, NOSE + 0.66, -0.20, 0.20, lift=0.004, nu=8, nv=6)
carlib.top_text(p, M.trim, L, NUM, 0.0, NOSE + 0.48, size=0.26, thick=0.006, lift=0.005,
                face="front", squash=0.80)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
# No mesh steering wheel: the client's cockpit rig draws its own at the
# derived wheel point. The cabin is a prototype tub - narrow, the driver on
# the centreline-ish, a bulkhead behind him - built to the same points the
# GT3 cabin is, so the cockpit view reads the same in both classes.
DASH_Z = min(EYE.z - 0.22, L.roof_z(SCREEN_Y[0]) - 0.03)
DASH_Y0, DASH_Y1 = SCREEN_Y[0] - 0.02, CK["wheel"].y - 0.28
XIN = min(L.x_at(y, DASH_Z) for y in (DASH_Y0, -0.3, 0.0, 0.3, SIDE_Y[1])) - 0.050
HOOP_Y = SIDE_Y[1] - 0.06
BULK_Y = HOOP_Y + 0.03
BELT_Z = L.point(0.0, CAN_LO).z
p.box(M.interior, (-XIN, DASH_Y0, 0.095), (XIN, BULK_Y, 0.135))                  # floor
p.box(M.alcantara, (-XIN + 0.02, -0.72, 0.135), (XIN - 0.02, 0.08, 0.140))       # footwell mat
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
carlib.bucket_seat(p, M, DX, 0.10, 0.135, width=0.48, depth=0.52, back_h=0.62, rake_deg=24.0)
for x in (DX - 0.19, DX - 0.07, DX + 0.05):
    p.box(M.metal, (x - 0.030, -0.76, 0.145), (x + 0.030, -0.70, 0.275))
p.box(M.metal, (DX + 0.13, -0.78, 0.145), (DX + 0.20, -0.68, 0.26))
carlib.extinguisher(p, M, -0.40, -0.40, 0.20, r=0.048, length=0.32)
carlib.inner_skin(p, M.alcantara, L, SCREEN_Y[1] + 0.03, SIDE_Y[1] - 0.03, 0.40,
                  drop=0.030, nu=10, nv=6)
# roll structure inside the canopy: the A-pillar bar runs down the edge of
# the screen from the canopy's shoulder, not across the driver's face
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
    p.bar(M.cage, (sx * 0.56, HOOP_Y, 0.52), (sx * 0.55, -0.62, 0.56), 0.019, segs=8)
p.bar(M.cage, (-0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050),
      (0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050), 0.022, segs=8)

# ---- wordmark, laid on the flank rather than floated off it
# ends at the door shut: run on over the side intake it hid the intake
DEC_Y = (AX_F + 0.82, SIDE_Y[1] + 0.02)
carlib.conform_decal(p, M.logo, L, DEC_Y[0], DEC_Y[1], 1.90, 2.85, sx=1,
                     lift=0.005, nu=14, nv=6)
carlib.conform_decal(p, M.logo, L, DEC_Y[0], DEC_Y[1], 1.90, 2.85, sx=-1,
                     lift=0.005, nu=14, nv=6, flip_u=True)

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0040, segments=2,
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
