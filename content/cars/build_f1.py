"""Build one of the generated F1 cars in Blender and export it to
content/cars/<folder>/<stem>.glb.

    VARIANT = "fugazzi"    # fugazzi | murcetes | mclarsen | ashton
    exec(open(r"D:\\apexsim\\content\\cars\\build_f1.py").read())

Same pipeline as build_gt3.py / build_lmp2.py / build_hypercar.py - shape and
livery data here, mechanics in carlib.py - for an open-wheeler. The loft is
the survival cell, sidepods and engine cover in one skin, nose cone to rear
crash structure; everything else is a part on top of it: the floor and
diffuser, front and rear wings, the halo, suspension, mirrors, brake ducts.
2026-style proportions: 3.4 m wheelbase, 1.9 m wide front wing, narrow
rear wing, no beam-wing bodywork around the gearbox.

Frame: nose on -Y, tail on +Y, ground z = 0, metres, driver on the
centreline. Exported with glTF +Y up, like every car in content/cars.

The client derives an open-wheel cockpit from the mesh box
(ApexCockpit::DeriveLayout, OpenWheel): the eye on the centreline, 8% of the
length behind centre and 82% of the height up; mirrors 70 cm ahead of the
eye, 8 cm below it and a quarter of the width out. The cockpit opening, the
halo and the mirror heads are built to those points (`open_wheel_points`).

The sidepods are undercut: the lower flank (j2) tucks in under the pod so
air can run along the floor to the coke-bottle, which is most of what makes
a modern F1 car look like one from the front three-quarter.

Section control points, floor centre outwards and up to the top centre:
    0 floor centre        3 upper flank (HARD)    6 inner top
    1 floor edge          4 shoulder              7 upper
    2 lower flank (HARD)  5 pod top inner         8 top centre
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
TRACK_F, TRACK_R = 1.580, 1.520
W_F, W_R = 0.305, 0.375
AX_F, AX_R = -1.650, 1.750        # wheelbase 3.4 m
SAMP = 5
FW_Y = (-3.02, -2.56)             # front wing, leading to trailing edge
FW_HW = 0.90
RW_Y = (2.16, 2.56)
RW_HW = 0.50
RW_Z = 0.79

# Brand data. Shape factors: nose_z (nose height), pod_w / pod_h (sidepod
# width and height), cover_h (airbox and engine cover), coke (how hard the
# pods pull in ahead of the rear wheels). inlet: the sidepod mouth.
# livery: boxes (y0, y1, j0, j1) of the loft painted in the accent colour.
VARIANTS = {
    "fugazzi": dict(folder="fugazzi-sf26", stem="fugazzi_sf26", logo="fugazzi_f1_logo.png",
                    paint=(0.56, 0.010, 0.015), accent=(0.93, 0.93, 0.92), paint_metallic=0.35,
                    number="16", nose_z=0.00, pod_w=1.00, pod_h=1.03, cover_h=1.00, coke=1.00,
                    inlet="tall", airbox="oval", tcam=(0.95, 0.80, 0.05),
                    # short, wide nose; pods that wash steeply down to the
                    # floor; swept wing endplates; no fin
                    nose_len=-0.14, nose_w=1.20, undercut=0.05, pod_slope=0.10,
                    fw="swept", rw="swept", beam=1, fin=False, rw_plan=("spoon", 0.035),
                    livery=[(-0.95, 1.40, 3.95, 4.25),            # white band along the pod shoulder
                            (-3.1, -2.45, 0.0, 9.0)]),            # white nose tip
    "murcetes": dict(folder="murcetes-amd-w17", stem="murcetes_w17", logo="murcetes_f1_logo.png",
                     paint=(0.60, 0.61, 0.64), accent=(0.015, 0.015, 0.018), paint_metallic=0.90,
                     number="63", nose_z=-0.03, pod_w=0.86, pod_h=0.94, cover_h=0.97, coke=0.92,
                     inlet="slot", airbox="tri", tcam=(0.02, 0.02, 0.02),
                     # needle nose; slim pods cut deep underneath; a shark
                     # fin to the wing; square endplates, double beam wing
                     nose_len=0.08, nose_w=0.78, undercut=0.10, pod_slope=0.02,
                     fw="classic", rw="square", beam=2, fin=True, rw_plan=("straight", 0.0),
                     livery=[(-1.3, 2.5, 0.0, 3.25),              # black below the flank crease
                             (-3.1, -2.3, 0.0, 9.0)]),
    "mclarsen": dict(folder="mclarsen-mcl40", stem="mclarsen_mcl40", logo="mclarsen_logo.png",
                     paint=(0.95, 0.32, 0.015), accent=(0.012, 0.016, 0.035), paint_metallic=0.30,
                     number="4", nose_z=0.02, pod_w=1.04, pod_h=1.00, cover_h=1.00, coke=1.04,
                     inlet="wide", airbox="oval", tcam=(0.02, 0.02, 0.02),
                     # the spoon: a broad flat nose over a low two-element
                     # wing; high pod shoulders ramping back; curled endplates
                     nose_len=0.0, nose_w=1.35, undercut=0.07, pod_slope=-0.03,
                     fw="low", rw="curl", beam=1, fin=False, rw_plan=("arch", 0.025),
                     livery=[(-1.3, 2.5, 0.0, 2.9),               # dark lower half
                             (0.45, 2.4, 6.6, 9.0),               # dark spine on the engine cover
                             (-2.2, -1.2, 6.8, 9.0)]),            # and down the nose
    "ashton": dict(folder="ashton-marvin-amr26", stem="ashton_amr26", logo="ashton_logo.png",
                   paint=(0.005, 0.15, 0.10), accent=(0.62, 0.95, 0.08), paint_metallic=0.65,
                   number="14", nose_z=0.01, pod_w=0.95, pod_h=1.06, cover_h=1.03, coke=0.97,
                   inlet="high", airbox="tri", tcam=(0.62, 0.95, 0.08),
                   # long pointed nose; pods that fall away early; tall swept
                   # front endplates, curled rear ones, a fin
                   nose_len=0.05, nose_w=0.92, undercut=0.03, pod_slope=0.06,
                   fw="swept", rw="curl", beam=2, fin=True, rw_plan=("swept", 0.07),
                   livery=[(-0.85, 1.60, 4.55, 4.85),             # lime pinstripe along the pod shoulder
                           (-3.1, -2.55, 0.0, 9.0)]),             # lime nose tip
}

INLETS = {  # sidepod mouth, before pod_w: x0, x1, z0, z1
    "tall": (0.33, 0.50, 0.28, 0.58),
    "slot": (0.38, 0.47, 0.26, 0.58),
    "wide": (0.30, 0.56, 0.40, 0.55),
    "high": (0.31, 0.53, 0.44, 0.60),
}

# Base hull. Right-half section control points (x, z).
KEYS = [
    (-2.78, [(0, .215), (.050, .215), (.075, .235), (.080, .265), (.070, .290), (.050, .300), (.030, .305), (.012, .307), (0, .307)]),
    (-2.45, [(0, .190), (.090, .190), (.120, .210), (.130, .280), (.120, .335), (.090, .355), (.055, .365), (.020, .370), (0, .370)]),
    (-2.00, [(0, .160), (.130, .160), (.170, .190), (.180, .330), (.170, .430), (.130, .465), (.080, .480), (.030, .485), (0, .485)]),
    (-1.55, [(0, .100), (.180, .100), (.220, .140), (.235, .380), (.220, .540), (.170, .585), (.100, .600), (.040, .605), (0, .605)]),
    (-1.10, [(0, .040), (.210, .040), (.250, .080), (.265, .400), (.255, .600), (.200, .645), (.120, .660), (.050, .665), (0, .665)]),
    (-0.80, [(0, .030), (.340, .030), (.440, .110), (.530, .420), (.500, .550), (.330, .630), (.200, .660), (.080, .670), (0, .670)]),
    (-0.35, [(0, .030), (.400, .030), (.520, .130), (.660, .440), (.600, .580), (.400, .630), (.260, .650), (.120, .655), (0, .655)]),
    (0.15, [(0, .030), (.380, .030), (.500, .130), (.640, .420), (.570, .550), (.360, .620), (.240, .640), (.120, .645), (0, .645)]),
    (0.62, [(0, .030), (.340, .030), (.450, .120), (.590, .400), (.520, .520), (.300, .620), (.160, .800), (.110, .950), (0, .980)]),
    (1.10, [(0, .030), (.290, .030), (.370, .100), (.460, .330), (.400, .430), (.240, .560), (.130, .700), (.080, .780), (0, .800)]),
    (1.55, [(0, .040), (.240, .040), (.270, .080), (.280, .260), (.250, .360), (.180, .450), (.110, .530), (.060, .570), (0, .580)]),
    (1.95, [(0, .080), (.150, .080), (.170, .110), (.170, .240), (.150, .320), (.110, .370), (.070, .410), (.030, .420), (0, .420)]),
    (2.30, [(0, .180), (.070, .180), (.085, .200), (.090, .260), (.080, .300), (.060, .320), (.035, .330), (.015, .335), (0, .335)]),
]


def apply_variant(keys, v):
    out = []
    for (y, pts) in keys:
        new = []
        nose = min(max((-1.60 - y) / 1.0, 0.0), 1.0)
        pod = -0.95 < y < 1.40
        slope = min(max((y - 0.15) / 0.95, 0.0), 1.0) if -0.95 < y < 1.40 else 0.0
        for j, (x, z) in enumerate(pts):
            z += v["nose_z"] * nose
            if y < -1.9:
                x *= 1.0 + (v.get("nose_w", 1.0) - 1.0) * min(1.0, (-1.9 - y) / 0.5)
            # the undercut: the floor edge and sill pulled in under the pod
            if -0.85 < y < 1.20 and j in (1, 2):
                x -= v.get("undercut", 0.0) * (1.0 if j == 1 else 0.6)
            # downwash: the pod's top falls towards the floor as it runs back
            if 3 <= j <= 5:
                z -= v.get("pod_slope", 0.0) * slope * (0.030 + (z - 0.03)) / 0.45
            if pod and 1 <= j <= 5:
                x *= v["pod_w"]
            if pod and 3 <= j <= 5:
                z = 0.03 + (z - 0.03) * v["pod_h"]
            if j >= 6 and y > 0.40:
                z = 0.60 + (z - 0.60) * v["cover_h"]
            if 1.00 <= y <= 1.95:
                x *= v["coke"]
            if v["airbox"] == "tri" and y == 0.62 and j in (6, 7):
                x *= 0.80                     # a narrow triangular intake
            new.append((x, z))
        if y < -2.6:
            y -= v.get("nose_len", 0.0)           # the tip, run out or pulled back
        out.append((y, new))
    return out


try:
    VARIANT
except NameError:
    VARIANT = "fugazzi"
V = VARIANTS[VARIANT]
CAR_DIR = os.path.join(carlib.CARS_ROOT, V["folder"])
os.makedirs(os.path.join(CAR_DIR, "textures"), exist_ok=True)


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


def open_wheel_points(lo_y, hi_y, lo_z, hi_z, half_w):
    """The eye and mirrors ApexCockpit::DeriveLayout puts in an open-wheeler,
    from the body's box (docs/CAR_MODELS.md, Cockpit)."""
    L, H = hi_y - lo_y, hi_z - lo_z
    eye = Vector((0.0, (lo_y + hi_y) / 2 + 0.08 * L, lo_z + 0.82 * H))
    return dict(eye=eye, mirror_y=eye.y - 0.70, mirror_z=eye.z - 0.08, mirror_x=0.25 * 2 * half_w)


# The box the client will see: front wing leading edge to rear wing trailing
# edge, plank to the top of the airbox / T-cam. Fixed by the class kit, so the
# cockpit can be cut before the parts exist.
TOP_Z = 0.98 * V["cover_h"] + (0.60 * (1 - V["cover_h"])) + 0.045   # T-cam on the airbox
OW = open_wheel_points(FW_Y[0] - 0.005, RW_Y[1] + 0.005, 0.004, TOP_Z, FW_HW + 0.012)
EYE = OW["eye"]

# ---------------------------------------------------------------- materials
carlib.reset_scene()
M = carlib.car_materials(V["paint"], V["accent"], (0.9, 0.9, 0.9),
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]),
                         seat_rgb=(0.05, 0.05, 0.06), paint_metallic=V["paint_metallic"],
                         paint_rough=0.24, accent_metallic=0.30)

# ---------------------------------------------------------------- body loft
VK = apply_variant(KEYS, V)
L = carlib.Loft(VK, samp=SAMP, ny=150, hard=(2, 3))
NOSE, TAIL = L.nose, L.tail


def face_mat(ym, kk, right):
    jc = kk / SAMP
    for (y0, y1, j0, j1) in V["livery"]:
        if y0 <= ym <= y1 and j0 <= jc <= j1:
            return M.accent
    return M.paint


body = L.build("body", face_mat, [M.paint, M.accent], subsurf=0)
save("body")

# --------------------------------------------------------------- apertures
# Cockpit: a lined tub opening round the derived eye, the seat pan at its
# bottom, the headrest behind; it stops short of the airbox.
CP_Y = (EYE.y - 0.56, EYE.y + 0.25)
carlib.aperture(body, M, (-0.235, CP_Y[0], 0.22), (0.235, CP_Y[1], 1.30))
# sidepod mouths, into the front face of each pod
ix0, ix1, iz0, iz1 = INLETS[V["inlet"]]
INLET_BACK = []
for sx in (-1, 1):
    a0, a1 = sorted((sx * ix0 * V["pod_w"], sx * ix1 * V["pod_w"]))
    carlib.aperture(body, M, (a0, -1.40, iz0 * V["pod_h"]), (a1, -0.62, iz1 * V["pod_h"]), mat=M.mesh)
    INLET_BACK.append((a0, a1, iz0 * V["pod_h"], iz1 * V["pod_h"]))
# the engine air intake over the driver's head
AB_Y = 0.46
ab_top = L.roof_z(0.60) - 0.035
carlib.aperture(body, M, (-0.070 if V["airbox"] == "oval" else -0.055, AB_Y - 0.10, ab_top - 0.13),
                (0.070 if V["airbox"] == "oval" else 0.055, AB_Y + 0.20, ab_top), mat=M.mesh)
carlib.sharpen(body, 32.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")
C = M.carbon

# ---- floor: a plank-thin plate in plan, raised edge wings, a diffuser
FLOOR_Z = (0.012, 0.028)
plan_r = [(0.26, -1.30), (0.52, -1.05), (0.76, -0.80), (0.80, -0.55), (0.80, 1.05),
          (0.70, 1.25), (0.56, 1.40), (0.50, 1.50)]
plan = plan_r + [(-x, y) for (x, y) in reversed(plan_r)]
carlib.panel_xy(p, C, plan, FLOOR_Z[0], FLOOR_Z[1])
for sx in (-1, 1):
    edge = [(-0.80, FLOOR_Z[1]), (1.05, FLOOR_Z[1]), (1.05, 0.085), (0.20, 0.110), (-0.55, 0.095)]
    carlib.plate(p, C, edge, sx * 0.795, 0.012, chamfer=0.004)
    # the floor's leading-edge fences and the bargeboard-ish board ahead of the pod
    for k, fx in enumerate((0.30, 0.42, 0.54)):
        fence = [(-1.28 + 0.12 * k, FLOOR_Z[1]), (-0.78, FLOOR_Z[1]), (-0.78, 0.16), (-1.10 + 0.12 * k, 0.20)]
        carlib.plate(p, C, fence, sx * fx, 0.008, chamfer=0.003)
carlib.diffuser(p, C, 1.48, 2.18, 0.48, FLOOR_Z[1], 0.235, thick=0.012,
                strakes=(-0.70, -0.35, 0.35, 0.70), strake_h=0.17)
# plank
p.box(M.metal, (-0.15, -1.25, 0.004), (0.15, 1.40, FLOOR_Z[0]))

# ---- front wing: main plane and two flaps between the endplates, on two
# pylons under the nose
fw_te = FW_Y[1]
FW = V["fw"]
carlib.foil(p, C, -FW_HW, FW_HW, 0.26, 0.030, 0.018, FW_Y[0], 0.085, angle_deg=-4.0)
carlib.foil(p, M.paint, -FW_HW + 0.02, -0.16, 0.15, 0.022, 0.014, FW_Y[0] + 0.21, 0.125, angle_deg=-18.0)
carlib.foil(p, M.paint, 0.16, FW_HW - 0.02, 0.15, 0.022, 0.014, FW_Y[0] + 0.21, 0.125, angle_deg=-18.0)
if FW != "low":
    carlib.foil(p, M.accent, -FW_HW + 0.02, -0.20, 0.12, 0.018, 0.012, FW_Y[0] + 0.33, 0.185, angle_deg=-30.0)
    carlib.foil(p, M.accent, 0.20, FW_HW - 0.02, 0.12, 0.018, 0.012, FW_Y[0] + 0.33, 0.185, angle_deg=-30.0)
if FW == "swept":
    # a third flap, short, curling up into the endplate
    carlib.foil(p, M.paint, -FW_HW + 0.02, -0.46, 0.09, 0.016, 0.010, FW_Y[0] + 0.40, 0.245, angle_deg=-42.0)
    carlib.foil(p, M.paint, 0.46, FW_HW - 0.02, 0.09, 0.016, 0.010, FW_Y[0] + 0.40, 0.245, angle_deg=-42.0)
FW_EP = {
    # classic: a plain upright plate
    "classic": [(FW_Y[0] - 0.005, 0.045), (fw_te, 0.050), (fw_te, 0.290), (FW_Y[0] + 0.30, 0.300),
                (FW_Y[0] + 0.02, 0.200)],
    # swept: taller, its leading edge raked back and the top running on past the flaps
    "swept": [(FW_Y[0] + 0.02, 0.045), (fw_te + 0.03, 0.050), (fw_te + 0.05, 0.330), (FW_Y[0] + 0.30, 0.345),
              (FW_Y[0] + 0.15, 0.260), (FW_Y[0] + 0.06, 0.140)],
    # low: a squat plate with a footplate, under a two-element wing
    "low": [(FW_Y[0] - 0.005, 0.040), (fw_te + 0.02, 0.040), (fw_te + 0.02, 0.200), (FW_Y[0] + 0.24, 0.215),
            (FW_Y[0] + 0.04, 0.130)],
}[FW]
for sx in (-1, 1):
    carlib.plate(p, C, FW_EP, sx * (FW_HW + 0.006), 0.012, chamfer=0.004)
    if FW == "low":
        # (inside the endplate's own box: the client's layout box ends there)
        p.box(C, (min(sx * (FW_HW - 0.10), sx * (FW_HW + 0.010)), FW_Y[0], 0.034),
              (max(sx * (FW_HW - 0.10), sx * (FW_HW + 0.010)), fw_te + 0.02, 0.046))
    # pylons from the wing up to the nose, wherever the nose now ends
    pz = L.floor_z(NOSE + 0.14)
    pyl = [(-2.86, 0.105), (-2.52, 0.105), (max(-2.45, NOSE + 0.30), pz + 0.01), (NOSE + 0.10, pz + 0.01)]
    carlib.plate(p, C, pyl, sx * (0.075 * V.get("nose_w", 1.0) ** 0.5), 0.012, chamfer=0.004)

# ---- rear wing: main plane and flap between simple endplates, one pylon
# from the gearbox, beam wing low down, rain light on the crash structure
# main plane and flap in the car's own plan (tips fixed, so the endplates
# meet them whatever the middle does)
RW_PLAN, RW_AMT = V["rw_plan"]
_, RW_TE = carlib.wing(p, C, RW_HW, 0.25, 0.038, 0.024, RW_Y[0], RW_Z, angle_deg=-6.0,
                       plan=RW_PLAN, amount=RW_AMT)
_, RW_TE2 = carlib.wing(p, M.paint, RW_HW, 0.14, 0.026, 0.016, RW_Y[0] + 0.24, RW_Z + 0.085,
                        angle_deg=-22.0, plan=RW_PLAN, amount=RW_AMT)
for (xa, xb, y, z) in carlib.spans(RW_TE2, -RW_HW, RW_HW, 10):
    carlib.gurney(p, C, xa, xb, y - 0.01, z, h=0.014, t=0.004, angle_deg=-22.0)
RW_EP = {
    "square": [(RW_Y[0] - 0.06, 0.60), (RW_Y[1], 0.64), (RW_Y[1] + 0.005, RW_Z + 0.16),
               (RW_Y[0] + 0.10, RW_Z + 0.17), (RW_Y[0] - 0.08, RW_Z + 0.06)],
    # swept: the leading edge raked forward as it climbs, the tip cut back
    "swept": [(RW_Y[0] + 0.02, 0.58), (RW_Y[1] + 0.005, 0.62), (RW_Y[1] - 0.04, RW_Z + 0.19),
              (RW_Y[0] - 0.02, RW_Z + 0.20), (RW_Y[0] - 0.12, RW_Z + 0.08)],
    # curl: a rounded top that rolls into the main plane
    # (nothing reaches past RW_Y[1] + 5 mm: the client's box, and so the
    # derived eye, ends there - see open_wheel_points)
    "curl": [(RW_Y[0] - 0.04, 0.60), (RW_Y[1], 0.62), (RW_Y[1] + 0.005, RW_Z + 0.12),
             (RW_Y[1] - 0.06, RW_Z + 0.17), (RW_Y[0] + 0.06, RW_Z + 0.16), (RW_Y[0] - 0.08, RW_Z + 0.10)],
}[V["rw"]]
for sx in (-1, 1):
    carlib.plate(p, M.paint, RW_EP, sx * (RW_HW + 0.008), 0.014, chamfer=0.005)
    if V["rw"] == "curl":
        # the tip rolls inboard over the flap
        p.box(M.paint, (min(sx * (RW_HW - 0.05), sx * (RW_HW + 0.015)), RW_Y[0] + 0.04, RW_Z + 0.150),
              (max(sx * (RW_HW - 0.05), sx * (RW_HW + 0.015)), RW_Y[1] - 0.03, RW_Z + 0.163))
rw_dz = RW_TE(0.0)[1] - RW_TE(RW_HW)[1]          # the plane's middle over its tips
pyl = [(1.98, L.roof_z(1.98) - 0.02), (2.10, L.roof_z(2.10) - 0.02), (2.36, RW_Z + 0.01 + rw_dz),
       (2.24, RW_Z + 0.01 + rw_dz)]
carlib.plate(p, C, pyl, 0.0, 0.020, chamfer=0.006)
carlib.foil(p, C, -0.42, 0.42, 0.20, 0.028, 0.016, 2.02, 0.300, angle_deg=-10.0)
if V.get("beam", 1) == 2:
    carlib.foil(p, C, -0.40, 0.40, 0.12, 0.022, 0.014, 2.19, 0.355, angle_deg=-24.0)
if V.get("fin"):
    # the shark fin: the engine cover's spine run up to the wing
    fin = [(0.70, L.roof_z(0.70) - 0.01), (1.20, L.roof_z(1.20) + 0.14), (RW_Y[0] + 0.02, RW_Z + 0.02 + rw_dz),
           (RW_Y[0] + 0.02, RW_Z - 0.10), (1.80, L.roof_z(1.80) - 0.01)]
    carlib.plate(p, M.paint, fin, 0.0, 0.012, chamfer=0.004)
carlib.led_grid(p, M, -0.036, 0.036, 0.205, 0.315, TAIL + 0.004, dir_y=1.0,
                cols=2, rows=5, glow=M.rain)
for sx in (-1, 1):          # the endplate rain lights of the current rules
    p.box(M.rain, (sx * (RW_HW + 0.016) - 0.004, RW_Y[1] - 0.02, 0.66), (sx * (RW_HW + 0.016) + 0.004, RW_Y[1], 0.76))
# exhaust over the crash structure
p.cylinder(M.metal, (0.0, 2.20, 0.405), 0.045, 0.20, segs=20, axis='Y')
p.cylinder(M.lamp_h, (0.0, 2.24, 0.405), 0.037, 0.14, segs=20, axis='Y')

# ---- halo: centre pillar off the tub ahead of the driver, a hoop round the
# cockpit at helmet height, rear legs down to the tub behind the shoulders
HZ = EYE.z + 0.03                  # the hoop sits just above the eye line
hy0 = CP_Y[0] - 0.02
pillar_foot = (0.0, hy0 - 0.20, L.roof_z(hy0 - 0.20) - 0.01)
carlib.guide_xyz(p, C, [pillar_foot, (0.0, hy0 - 0.06, HZ - 0.06), (0.0, hy0 + 0.04, HZ)], r=0.026, segs=12)
for sx in (-1, 1):
    # the hoop: from the pillar top round the side of the helmet, open at the back
    hoop = []
    for k in range(18):
        a = math.pi * 0.80 * k / 17
        hoop.append((sx * 0.25 * math.sin(a), hy0 + 0.04 + 0.44 * (1 - math.cos(a)) / 2, HZ))
    x_e, y_e, _ = hoop[-1]
    # rear leg down to the tub behind the shoulders
    hoop += [(sx * (abs(x_e) + 0.04), y_e + 0.06, HZ - 0.07),
             (sx * (abs(x_e) + 0.07), y_e + 0.10, L.z_at(y_e + 0.10, abs(x_e) + 0.07) - 0.01)]
    carlib.guide_xyz(p, C, hoop, r=0.022, segs=12)

# ---- cockpit: seat back and headrest, padded rim, the pan the cut left
seat_y = CP_Y[1] - 0.05
carlib.plate(p, M.seat, [(seat_y - 0.10, 0.23), (seat_y, 0.23), (seat_y + 0.02, 0.62), (seat_y - 0.04, 0.62)],
             0.0, 0.40, chamfer=0.02)
for sx in (-1, 1):
    rim_z = L.z_at(EYE.y, 0.25)
    p.box(M.alcantara, (sx * 0.235 - 0.03 if sx > 0 else -0.235, EYE.y - 0.10, rim_z - 0.05),
          (0.235 if sx > 0 else -0.235 + 0.03, CP_Y[1], rim_z + 0.035))
p.box(M.alcantara, (-0.12, CP_Y[1] - 0.06, L.z_at(CP_Y[1], 0.10) - 0.12), (0.12, CP_Y[1], L.z_at(CP_Y[1], 0.10) + 0.02))
p.box(M.interior, (-0.20, CP_Y[0], 0.22), (0.20, CP_Y[1], 0.24))

# ---- mirrors, on stalks from the pod shoulders, at the derived points
for sx in (-1, 1):
    mx, my, mz = OW["mirror_x"], OW["mirror_y"], OW["mirror_z"]
    base_z = L.z_at(my, mx - 0.05)
    stalk = [(my - 0.03, base_z - 0.01), (my + 0.05, base_z - 0.01), (my + 0.015, mz - 0.02), (my - 0.015, mz - 0.02)]
    carlib.plate(p, C, stalk, sx * (mx - 0.05), 0.016, chamfer=0.005)
    p.box(M.paint, (sx * mx - 0.075, my - 0.045, mz - 0.035), (sx * mx + 0.075, my + 0.035, mz + 0.035))
    p.box(M.glass, (sx * mx - 0.068, my + 0.035, mz - 0.028), (sx * mx + 0.068, my + 0.040, mz + 0.028))

# ---- intakes: dark backing in the sidepod mouths and the airbox, so the
# holes read as ducts rather than showing the cutter's lit back wall
for (a0, a1, z0, z1) in INLET_BACK:
    p.box(M.lamp_h, (a0 - 0.01, -0.67, z0 - 0.01), (a1 + 0.01, -0.64, z1 + 0.01))
    carlib.grille(p, M, a0 + 0.008, a1 - 0.008, -0.70, z0 + 0.01, z1 - 0.01, bars=4, depth=0.03, backing=False)
p.box(M.lamp_h, (-0.075, AB_Y + 0.16, ab_top - 0.14), (0.075, AB_Y + 0.19, ab_top + 0.005))
tc_y = 0.64
tc_z = L.roof_z(tc_y)
tcam = carlib.mat("car_tcam", V["tcam"], 0.2, 0.35)
p.box(tcam, (-0.035, tc_y - 0.06, tc_z - 0.005), (0.035, tc_y + 0.06, tc_z + 0.040))

# ---- suspension: wishbones, push/pull rods and track rods, carbon aero
# sections; brake ducts as drums just inboard of the wheels
for (ay, track, width, inner_x, zl, zu) in ((AX_F, TRACK_F, W_F, 0.18, 0.20, 0.44),
                                            (AX_R, TRACK_R, W_R, 0.16, 0.16, 0.36)):
    hub = track / 2.0
    ox = hub - width / 2.0 - 0.02                    # just inside the rim
    for sx in (-1, 1):
        for z_in, z_out, spread in ((zl, 0.20, 0.26), (zu, 0.50 if ay < 0 else 0.48, 0.20)):
            for dy in (-spread, spread):
                y_in = ay + dy
                x_in = max(L.x_at(y_in, z_in) - 0.01, inner_x) if ay < 0 else inner_x
                p.bar(C, (sx * x_in, y_in, z_in), (sx * ox, ay, z_out), 0.013, segs=6)
        # push rod (front) / pull rod (rear)
        if ay < 0:
            p.bar(C, (sx * (ox - 0.02), ay + 0.02, 0.22), (sx * inner_x, ay + 0.10, zu + 0.06), 0.011, segs=6)
        else:
            p.bar(C, (sx * (ox - 0.02), ay - 0.02, 0.46), (sx * inner_x, ay - 0.10, 0.15), 0.011, segs=6)
        p.bar(C, (sx * inner_x, ay + (0.12 if ay < 0 else -0.12), zl + 0.10), (sx * ox, ay + 0.12, 0.30), 0.010, segs=6)
        p.cylinder(C, (sx * (ox - 0.035) - 0.03, ay, 0.356), 0.19, 0.06, segs=24, axis='X')

# ---- nose: race number on a white plate, the camera pods either side
NUM = V["number"]
carlib.top_patch(p, M.decal, L, -2.36, -2.10, -0.085, 0.085, lift=0.003, nu=6, nv=4)
carlib.top_text(p, M.trim, L, NUM, 0.0, -2.23, size=0.14, thick=0.005, lift=0.004,
                face="front", squash=0.80)
for sx in (-1, 1):
    cy = -1.95
    cz = L.z_at(cy, 0.10)
    p.bar(C, (sx * 0.12, cy, cz - 0.02), (sx * 0.17, cy, cz + 0.01), 0.008, segs=6)
    p.box(M.accent, (sx * 0.17 - 0.016, cy - 0.045, cz), (sx * 0.17 + 0.016, cy + 0.045, cz + 0.028))

# ---- wordmark on each sidepod, and on the rear wing endplates
carlib.conform_decal(p, M.logo, L, -0.05, 0.75, 2.35, 3.85, sx=1, lift=0.004, nu=14, nv=6)
carlib.conform_decal(p, M.logo, L, -0.05, 0.75, 2.35, 3.85, sx=-1, lift=0.004, nu=14, nv=6, flip_u=True)

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0025, segments=2,
                     angle_deg=38.0)
save("parts")

# ------------------------------------------------------------ join + export
car, glb = carlib.join_and_export([body, parts], V["stem"], CAR_DIR,
                                  export=os.environ.get("APEX_EXPORT", "1") == "1")
save("joined")
st = carlib.mesh_stats(car)
print("stats:", st)
print("sightline:", carlib.sightline(car, open_wheel=True))
print("derived eye:", [round(c, 3) for c in EYE], "mirror y/z/x:",
      round(OW["mirror_y"], 3), round(OW["mirror_z"], 3), round(OW["mirror_x"], 3))
if glb:
    print("GLB:", glb, os.path.getsize(glb))
