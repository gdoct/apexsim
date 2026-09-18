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
TYRE_F, TYRE_R = 0.355, 0.365
TRACK = 1.580
HUB_X = TRACK / 2.0
ARCH_GAP = 0.022
SAMP = 5
AX_F, AX_R = -1.500, 1.500        # wheelbase 3.0 m, as [physics] says
CAN_LO, CAN_HI = 5.75, 7.45       # control indices where the canopy glass runs

VARIANTS = {
    "yotota": dict(folder="yotota-lmp2", stem="yotota_lmp2", logo="yotota_logo.png",
                   paint=(0.93, 0.93, 0.92), accent=(0.85, 0.05, 0.05), caliper=(0.85, 0.10, 0.05),
                   nose_w=1.00, fender=1.00, roof=1.00, canopy_shift=0.00, tail_h=1.00,
                   side_w=1.00, wing_z=0.00, fin=True, lights="tri", mirror="pod",
                   scoop=(0.30, 0.62, 0.16), seat=(0.10, 0.10, 0.32)),
    "posh": dict(folder="posh-lmp2", stem="posh_lmp2", logo="posh_logo.png",
                 paint=(0.75, 0.76, 0.78), accent=(0.05, 0.05, 0.05), caliper=(0.95, 0.75, 0.05),
                 nose_w=1.06, fender=1.03, roof=0.97, canopy_shift=-0.10, tail_h=0.94,
                 side_w=1.00, wing_z=-0.04, fin=True, lights="round", mirror="pod",
                 scoop=(0.34, 0.56, 0.15), seat=(0.10, 0.10, 0.12)),
    "fugazzi": dict(folder="fugazzi-lmp2", stem="fugazzi_lmp2", logo="fugazzi_logo.png",
                    paint=(0.80, 0.03, 0.03), accent=(0.95, 0.80, 0.05), caliper=(0.95, 0.80, 0.05),
                    nose_w=0.90, fender=1.05, roof=1.00, canopy_shift=0.12, tail_h=1.04,
                    side_w=0.99, wing_z=0.02, fin=True, lights="tri", mirror="stalk",
                    scoop=(0.26, 0.68, 0.17), seat=(0.16, 0.05, 0.05)),
    "jeanetti": dict(folder="jeanetti-lmp2", stem="jeanetti_lmp2", logo="jeanetti_logo.png",
                     paint=(0.04, 0.28, 0.14), accent=(0.95, 0.85, 0.20), caliper=(0.20, 0.20, 0.22),
                     nose_w=1.00, fender=0.98, roof=1.03, canopy_shift=0.05, tail_h=1.00,
                     side_w=1.02, wing_z=0.05, fin=False, lights="bar", mirror="pod",
                     scoop=(0.32, 0.52, 0.18), seat=(0.08, 0.10, 0.06)),
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
            if j in (1, 2) and -1.9 < y < 2.0:
                x *= v["side_w"]
            if y > 1.7:
                z *= v["tail_h"]
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


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


# ---------------------------------------------------------------- materials
carlib.reset_scene()
M = carlib.car_materials(V["paint"], V["accent"], V["caliper"],
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]),
                         seat_rgb=V["seat"])

# ---------------------------------------------------------------- body loft
VK = carlib.fender_bump(apply_variant(KEYS, V), (AX_F, AX_R),
                        amount=0.022, width=0.55, j0=2.4, j1=4.0)
L = carlib.Loft(VK, samp=SAMP, ny=84, hard=(2, 3, 5))
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
# Side radiator intake behind the door, and the brake exit ahead of the rear arch.
L.recess(SIDE_Y[1] + 0.16, SIDE_Y[1] + 0.56, 1.85, 3.15, depth=0.060, rim=0.030)
L.recess(AX_F + 0.46, AX_F + 0.74, 1.90, 3.05, depth=0.040, rim=0.024)
# Engine-cover exit louvres behind the canopy.
L.recess(SIDE_Y[1] + 0.26, SIDE_Y[1] + 0.74, 5.80, 8.00, depth=0.030, rim=0.026)
# The tunnel between each front fender and the cockpit, deepened.
L.recess(AX_F + 0.30, -0.35 + CS, 4.70, 5.90, depth=0.030, rim=0.12)

# Lamp geometry, computed here (pre-build) so the pockets that follow can
# use it too - a lamp cut into an untouched curve is a box glued onto paint;
# cut into a shallow recessed panel, the same box reads as a housing.
LAMP_X = {"round": (0.16, 0.58), "tri": (0.14, 0.62), "bar": (0.14, 0.62)}[V["lights"]]
LAMP_Z = (0.350, 0.350 + {"round": 0.125, "tri": 0.120, "bar": 0.100}[V["lights"]])
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
    jc = kk / SAMP
    if jc < CAN_LO:
        return M.paint
    if SCREEN_Y[0] < ym < SCREEN_Y[1]:
        return M.glass                      # windscreen: up to the roof
    if SIDE_Y[0] <= ym < SIDE_Y[1] and jc < CAN_HI:
        return M.glass                      # side glass, roof stays painted
    return M.paint


body = L.build("body", face_mat, [M.paint, M.glass], subsurf=0)
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
RAD_Y = carlib.surface_station(L, 0.56, 0.21, NOSE, margin=0.045)
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    # forward of the nose tip, so the box cuts through at every x across a
    # curved front, not just at the widest point
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_Y + 0.135, LAMP_Z[1]))
    t0, t1 = sorted((sx * 0.30, sx * 0.76))
    carlib.aperture(body, M, (t0, TAILL_Y - 0.20, TAILL_TOP - 0.125),
                    (t1, TAILL_Y + 0.04, TAILL_TOP + 0.008))
# the two radiator mouths either side of the nose centreline
for sx in (-1, 1):
    r0, r1 = sorted((sx * 0.16, sx * 0.56))
    carlib.aperture(body, M, (r0, RAD_Y - 0.03, 0.125), (r1, RAD_Y + 0.26, 0.290), mat=M.mesh)
# FIA rain light: a vertical bar on the centreline, inside its own pocket
carlib.aperture(body, M, (-0.065, RAIN_Y - 0.14, 0.340), (0.065, RAIN_Y + 0.05, 0.580))
carlib.sharpen(body, 34.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
CK = carlib.cockpit_points(L, 1.05 + V["wing_z"], liner_z=0.038)
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
                thick=0.014, strakes=(-0.64, -0.22, 0.22, 0.64), strake_h=0.185)

# ---- rear wing: two elements, endplates, swan necks, gurney, brake LED
WZ = 1.05 + V["wing_z"]
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

# ---- lights in their apertures
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    style = {"round": "round", "tri": "round", "bar": "bar"}[V["lights"]]
    n = {"round": 2, "tri": 3, "bar": 1}[V["lights"]]
    carlib.lamp_cluster(p, M, x0 + 0.010, x1 - 0.010, LAMP_Z[0] + 0.010, LAMP_Z[1] - 0.010,
                        LAMP_Y + 0.015, depth=0.10, style=style, count=n, dir_y=1.0,
                        housing=True, loft=L)
    t0, t1 = sorted((sx * 0.30, sx * 0.76))
    carlib.lamp_cluster(p, M, t0 + 0.008, t1 - 0.008, TAILL_TOP - 0.050, TAILL_TOP - 0.012,
                        TAILL_Y - 0.010, depth=0.10, style="bar", dir_y=-1.0,
                        glow=M.tail, housing=False)
    carlib.lamp_cluster(p, M, t0 + 0.008, t1 - 0.008, TAILL_TOP - 0.105, TAILL_TOP - 0.062,
                        TAILL_Y - 0.010, depth=0.10, style="bar", dir_y=-1.0,
                        glow=M.brake, housing=False)
    # radiator mouth: mesh backing and a raised surround
    r0, r1 = sorted((sx * 0.16, sx * 0.56))
    carlib.grille(p, M, r0 + 0.010, r1 - 0.010, RAD_Y + 0.010, 0.140, 0.275,
                  bars=4, depth=0.22, backing=True)
    carlib.duct_lip(p, M.carbon, r0, r1, RAD_Y - 0.010, 0.140, 0.275, out=0.018)
carlib.lamp_cluster(p, M, -0.050, 0.050, 0.355, 0.565, RAIN_Y - 0.008, depth=0.10,
                    style="bar", dir_y=-1.0, glow=M.rain, housing=False)

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
for sx in (-1, 1):
    iy0, iy1 = SIDE_Y[1] + 0.19, SIDE_Y[1] + 0.53
    z0, z1 = L.point((iy0 + iy1) / 2, 1.9).z, L.point((iy0 + iy1) / 2, 3.1).z
    carlib.louvre_bank(p, M.carbon, L.x_at((iy0 + iy1) / 2, (z0 + z1) / 2) - 0.020,
                       iy0, iy1, z0 + 0.03, z1 - 0.03, count=3, rake_deg=18, sx=sx)
    by0, by1 = AX_F + 0.48, AX_F + 0.72
    bz0, bz1 = L.point(by0, 1.95).z, L.point(by0, 3.0).z
    carlib.louvre_bank(p, M.carbon, L.x_at((by0 + by1) / 2, (bz0 + bz1) / 2) - 0.012,
                       by0, by1, bz0 + 0.02, bz1 - 0.02, count=3, rake_deg=24, sx=sx)
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
MIR_Z = L.point(MIR_Y, 5.10).z + 0.035
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
wy = SCREEN_Y[0] + 0.05
p.bar(M.carbon, (DX - 0.40, wy, L.z_at(wy, 0.34) + 0.013),
      (DX + 0.26, wy + 0.04, L.z_at(wy + 0.04, 0.34) + 0.013), 0.012, segs=8)

# ---- cockpit, built to the points the client derives (docs/CAR_MODELS.md)
p.box(M.interior, (-0.62, -0.78, 0.095), (0.62, 0.96, 0.135))
for sx in (-1, 1):
    p.box(M.interior, (sx * 0.585 - 0.035, -0.78, 0.135), (sx * 0.585 + 0.035, 0.96, 0.58))
p.box(M.seat, (DX - 0.245, 0.12, 0.135), (DX + 0.245, 0.60, 0.285))
p.box(M.seat, (DX - 0.265, 0.48, 0.285), (DX + 0.265, 0.645, 0.885))
for sx in (-1, 1):
    p.box(M.seat, (DX + sx * 0.275 - 0.04, 0.10, 0.285), (DX + sx * 0.275 + 0.04, 0.60, 0.425))
p.box(M.interior, (-0.56, -0.78, 0.545), (0.56, CK["wheel"].y - 0.12, DASH_Z))
p.box(M.display, (DX - 0.125, CK["wheel"].y - 0.125, DASH_Z - 0.165),
      (DX + 0.125, CK["wheel"].y - 0.115, DASH_Z - 0.045))
p.bar(M.metal, (DX, CK["wheel"].y - 0.16, CK["wheel"].z - 0.05),
      (DX, CK["wheel"].y, CK["wheel"].z), 0.021, segs=10)
carlib.steering_wheel(p, M, (DX, CK["wheel"].y, CK["wheel"].z), r=0.150, rim_r=0.019)
for x in (DX - 0.19, DX - 0.07, DX + 0.05):
    p.box(M.metal, (x - 0.030, -0.76, 0.145), (x + 0.030, -0.70, 0.275))
# roll structure inside the canopy
HOOP_Y = SIDE_Y[1] - 0.06
for sx in (-1, 1):
    top = (sx * 0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, 0.20), (sx * 0.56, HOOP_Y, top[2] - 0.12), 0.022, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, top[2] - 0.12), top, 0.022, segs=8)
    scr = (sx * 0.36, SCREEN_Y[1], L.z_at(SCREEN_Y[1], 0.42) - 0.045)
    p.bar(M.cage, top, scr, 0.019, segs=8)
    foot = (sx * (L.x_at(wy, 0.58) - 0.075), wy + 0.02, L.z_at(wy + 0.02, 0.60) - 0.040)
    p.bar(M.cage, scr, foot, 0.018, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, top[2] - 0.18), (sx * 0.44, AX_R - 0.15, 0.50),
          0.019, segs=8)
    p.bar(M.cage, (sx * 0.56, HOOP_Y, 0.52), (sx * 0.55, -0.62, 0.56), 0.019, segs=8)
p.bar(M.cage, (-0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050),
      (0.40, HOOP_Y, L.z_at(HOOP_Y, 0.46) - 0.050), 0.022, segs=8)

# ---- wordmark, laid on the flank rather than floated off it
DEC_Y = (AX_F + 0.82, AX_R - 0.42)
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
if glb:
    print("GLB:", glb, os.path.getsize(glb))
