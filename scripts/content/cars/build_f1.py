"""Build one of the generated F1 cars in Blender and export it to
content/cars/default/<folder>/<stem>.glb.

    VARIANT = "fugazzi"    # fugazzi | murcetes | mclarsen | ashton
    exec(open(r"E:\\apexsim\\content\\cars\\build_f1.py").read())

Same pipeline as build_gt3.py / build_lmp2.py / build_hypercar.py - shape and
livery data here, mechanics in carlib.py - for an open-wheeler. The loft is
the survival cell, sidepods and engine cover in one skin, nose cone to rear
crash structure; everything else is a part on top of it: the floor and
diffuser, front and rear wings, the halo, suspension, mirrors, brake ducts.
2026-style proportions: 3.4 m wheelbase, 1.8 m front wing, a narrow rear
wing over a beam wing.

Frame: nose on -Y, tail on +Y, ground z = 0, metres, driver on the
centreline. Exported with glTF +Y up, like every car in content/cars.

Pass 6 (measured against the imported SF70H and Formula Hybrid 2021):

* the eye is authored, not derived. The client's open-wheel derivation (8%
  of the box behind centre, 82% up) put the driver 1.87 m behind the front
  axle and 0.84 m up; the imports sit 1.43-1.57 m behind it and 0.67-0.78
  up. `[cockpit]` now carries eye, wheel and the mirrors as built, and the
  cockpit, halo and mirrors are laid off EYE.
* the hull is a tub with sidepods on it, not one blob: the pod top sits
  ~10 cm under the cockpit rim with a crease where they meet, the pod's
  flank is the widest line, and under it the undercut tucks in to the floor
  edge - the void you see between pod and floor from the front three-quarter.
  Everything below that crease (j < 2) is bare carbon.
* the wings are inverted wings: elements rise to the trailing edge (the
  old ones sloped down, a lifting wing) and the front wing's flaps rise and
  steepen as they run out to the endplate (`carlib.foil_path`).
* bare carbon has a weave, there is a sponsor atlas, the halo is a 4 cm
  section in the body colour, the mirrors are aero heads whose glass is the
  `car_mirror_*` slot the cockpit rig paints its capture onto.

Section control points, floor centre outwards and up to the top centre:
    0 floor centre        3 pod flank (HARD)      6 tub side (cockpit rim)
    1 floor edge          4 pod shoulder          7 tub upper
    2 undercut (HARD)     5 pod top / tub (HARD)  8 top centre
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
TYRE_F, TYRE_R = 0.360, 0.360
TRACK_F, TRACK_R = 1.580, 1.520
W_F, W_R = 0.305, 0.375
AX_F, AX_R = -1.650, 1.750        # wheelbase 3.4 m
SAMP = 5
FW_Y = (-3.02, -2.60)             # front wing, leading edge to the last flap
FW_HW = 0.90
RW_Y = (2.14, 2.56)
RW_HW = 0.50
RW_Z = 0.780                      # main plane leading edge
FLOOR_Z = (0.028, 0.040)          # plank-to-floor: ~45 mm of ride height under the plank's face

# The driver (pass 6, authored): 1.52 m behind the front axle, 0.79 m up -
# between the SF70H (1.57 / 0.78) and the 2021 car (1.43 / 0.67).
EYE = Vector((0.0, AX_F + 1.52, 0.79))
CK = carlib.authored_cockpit(EYE, wheel_ahead=0.40, wheel_below=0.17)

# Brand data. Shape factors: nose_z (nose height), pod_w / pod_h (sidepod
# width and height), cover_h (airbox and engine cover), coke (how hard the
# pods pull in ahead of the rear wheels). inlet: the sidepod mouth.
# livery: boxes (y0, y1, j0, j1) of the loft painted in the accent colour.
# sponsors: atlas cells (textures.py) for the pod, the engine cover, the
# nose, the front and rear endplates and the DRS flap.
VARIANTS = {
    "fugazzi": dict(folder="fugazzi-sf26", stem="fugazzi_sf26", logo="fugazzi_f1_logo.png",
                    paint=(0.50, 0.006, 0.010), accent=(0.93, 0.93, 0.92), paint_metallic=0.30,
                    number="16", nose_z=0.00, pod_w=1.00, pod_h=1.03, cover_h=1.00, coke=1.00,
                    inlet="tall", airbox="oval", tcam=(0.95, 0.80, 0.05), halo="paint",
                    # short, wide nose; pods that wash steeply down to the
                    # floor; swept wing endplates; no fin
                    nose_len=-0.10, nose_w=1.20, undercut=0.05, pod_slope=0.10,
                    fw="swept", rw="swept", beam=1, fin=False, rw_plan=("spoon", 0.030),
                    livery=[(-0.70, 1.40, 4.10, 4.40),            # white band along the pod shoulder
                            (-3.1, -2.45, 0.0, 9.0)],             # white nose tip
                    sponsors=dict(pod=8, cover=4, nose=14, fw_ep=2, rw_ep=13, flap=0)),
    "murcetes": dict(folder="murcetes-amd-w17", stem="murcetes_w17", logo="murcetes_f1_logo.png",
                     paint=(0.60, 0.61, 0.64), accent=(0.015, 0.015, 0.018), paint_metallic=0.90,
                     number="63", nose_z=-0.03, pod_w=0.86, pod_h=0.94, cover_h=0.97, coke=0.92,
                     inlet="slot", airbox="tri", tcam=(0.02, 0.02, 0.02), halo="carbon",
                     # needle nose; slim pods cut deep underneath; a shark
                     # fin to the wing; square endplates, double beam wing
                     nose_len=0.08, nose_w=0.78, undercut=0.10, pod_slope=0.02,
                     fw="classic", rw="square", beam=2, fin=True, rw_plan=("straight", 0.0),
                     livery=[(-1.3, 2.5, 0.0, 3.25),              # black below the flank crease
                             (-3.1, -2.3, 0.0, 9.0)],
                     sponsors=dict(pod=6, cover=7, nose=15, fw_ep=10, rw_ep=6, flap=2)),
    "mclarsen": dict(folder="mclarsen-mcl40", stem="mclarsen_mcl40", logo="mclarsen_logo.png",
                     paint=(0.95, 0.32, 0.015), accent=(0.012, 0.016, 0.035), paint_metallic=0.30,
                     number="4", nose_z=0.02, pod_w=1.04, pod_h=1.00, cover_h=1.00, coke=1.04,
                     inlet="wide", airbox="oval", tcam=(0.02, 0.02, 0.02), halo="paint",
                     # the spoon: a broad flat nose over a low two-element
                     # wing; high pod shoulders ramping back; curled endplates
                     nose_len=0.0, nose_w=1.35, undercut=0.07, pod_slope=-0.03,
                     fw="low", rw="curl", beam=1, fin=False, rw_plan=("arch", 0.025),
                     livery=[(-1.3, 2.5, 0.0, 2.9),               # dark lower half
                             (0.45, 2.4, 6.6, 9.0),               # dark spine on the engine cover
                             (-2.2, -1.2, 6.8, 9.0)],             # and down the nose
                     sponsors=dict(pod=0, cover=11, nose=15, fw_ep=3, rw_ep=10, flap=6)),
    "ashton": dict(folder="ashton-marvin-amr26", stem="ashton_amr26", logo="ashton_logo.png",
                   paint=(0.005, 0.15, 0.10), accent=(0.62, 0.95, 0.08), paint_metallic=0.65,
                   number="14", nose_z=0.01, pod_w=0.95, pod_h=1.06, cover_h=1.03, coke=0.97,
                   inlet="high", airbox="tri", tcam=(0.62, 0.95, 0.08), halo="paint",
                   # long pointed nose; pods that fall away early; tall swept
                   # front endplates, curled rear ones, a fin
                   nose_len=0.05, nose_w=0.92, undercut=0.03, pod_slope=0.06,
                   fw="swept", rw="curl", beam=2, fin=True, rw_plan=("swept", 0.07),
                   livery=[(-0.60, 1.60, 4.55, 4.80),             # lime pinstripe along the pod shoulder
                           (-3.1, -2.55, 0.0, 9.0)],              # lime nose tip
                   sponsors=dict(pod=4, cover=8, nose=14, fw_ep=6, rw_ep=4, flap=10)),
}

INLETS = {  # sidepod mouth, before pod_w: x0, x1, z0, z1
    "tall": (0.29, 0.50, 0.31, 0.56),
    "slot": (0.31, 0.47, 0.30, 0.56),
    "wide": (0.28, 0.51, 0.38, 0.56),
    "high": (0.29, 0.50, 0.40, 0.57),
}

# Base hull. Right-half section control points (x, z), j0..j8 as above.
# The tub: ~0.48 m wide at the cockpit, its rim at ~0.70-0.73. The pods: the
# front face at y -0.86 (behind the front tyre's trailing edge at -1.29 with
# room for the floor's leading-edge fences), the flank at ~0.62, the top
# ~0.58, ten centimetres under the rim, the undercut from the flank crease
# (j3, z ~0.37) in to the floor edge at the tub's own width.
KEYS = [
    (-2.78, [(0, .150), (.045, .150), (.070, .168), (.078, .205), (.071, .236), (.052, .250), (.031, .256), (.012, .258), (0, .258)]),
    (-2.45, [(0, .145), (.080, .145), (.108, .165), (.121, .240), (.115, .300), (.092, .328), (.056, .340), (.020, .345), (0, .345)]),
    (-2.00, [(0, .140), (.112, .140), (.146, .172), (.162, .320), (.156, .425), (.126, .462), (.079, .477), (.030, .483), (0, .484)]),
    (-1.55, [(0, .120), (.148, .120), (.182, .162), (.198, .380), (.192, .545), (.162, .588), (.102, .606), (.041, .612), (0, .613)]),
    (-1.10, [(0, .060), (.198, .060), (.232, .100), (.247, .400), (.243, .615), (.206, .667), (.132, .688), (.051, .695), (0, .696)]),
    (-0.92, [(0, .052), (.210, .052), (.248, .130), (.268, .380), (.266, .600), (.250, .642), (.214, .695), (.118, .714), (0, .717)]),
    (-0.80, [(0, .050), (.214, .050), (.300, .205), (.520, .362), (.512, .556), (.300, .600), (.236, .700), (.122, .722), (0, .726)]),
    (-0.35, [(0, .050), (.215, .050), (.340, .210), (.620, .378), (.598, .552), (.318, .590), (.240, .698), (.125, .733), (0, .738)]),
    (0.15, [(0, .050), (.215, .050), (.350, .205), (.610, .365), (.570, .520), (.300, .575), (.224, .702), (.120, .762), (0, .772)]),
    (0.62, [(0, .050), (.210, .050), (.330, .180), (.540, .320), (.480, .458), (.250, .545), (.166, .800), (.100, .962), (0, .986)]),
    (1.10, [(0, .050), (.200, .050), (.240, .130), (.390, .250), (.340, .370), (.200, .470), (.130, .690), (.080, .800), (0, .815)]),
    (1.55, [(0, .060), (.170, .060), (.190, .110), (.250, .200), (.230, .310), (.165, .400), (.110, .520), (.060, .585), (0, .600)]),
    (1.95, [(0, .090), (.140, .090), (.155, .120), (.165, .230), (.150, .310), (.110, .360), (.070, .400), (.030, .415), (0, .420)]),
    (2.30, [(0, .180), (.070, .180), (.085, .200), (.090, .260), (.080, .300), (.060, .320), (.035, .330), (.015, .335), (0, .335)]),
]
POD_Y = (-0.86, 1.40)             # where the pod factors apply


def apply_variant(keys, v):
    out = []
    for (y, pts) in keys:
        new = []
        nose = min(max((-1.60 - y) / 1.0, 0.0), 1.0)
        pod = POD_Y[0] < y < POD_Y[1]
        slope = min(max((y - 0.15) / 0.95, 0.0), 1.0) if pod else 0.0
        for j, (x, z) in enumerate(pts):
            z += v["nose_z"] * nose
            if y < -1.9:
                x *= 1.0 + (v.get("nose_w", 1.0) - 1.0) * min(1.0, (-1.9 - y) / 0.5)
            # the undercut: the turn-under pulled further in under the pod
            if pod and y > -0.85 and j == 2:
                x -= v.get("undercut", 0.0)
            # downwash: the pod's top falls towards the floor as it runs back
            if 3 <= j <= 5 and pod:
                z -= v.get("pod_slope", 0.0) * slope * (0.030 + (z - 0.03)) / 0.45
            if pod and 2 <= j <= 5 and y > -0.85:
                x *= v["pod_w"]
            if pod and 3 <= j <= 5 and y > -0.85:
                z = 0.05 + (z - 0.05) * v["pod_h"]
            if j >= 6 and y > 0.40:
                z = 0.60 + (z - 0.60) * v["cover_h"]
            if 1.00 <= y <= 1.95 and 1 <= j <= 5:
                x *= v["coke"]
            if v["airbox"] == "tri" and j in (6, 7):
                # a narrow triangular intake, eased into the cover behind it
                x *= {0.62: 0.86, 1.10: 0.94}.get(y, 1.0)
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
SP = V["sponsors"]


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


# ---------------------------------------------------------------- materials
carlib.reset_scene()
M = carlib.car_materials(V["paint"], V["accent"], (0.9, 0.9, 0.9),
                         logo_path=os.path.join(CAR_DIR, "textures", V["logo"]),
                         seat_rgb=(0.05, 0.05, 0.06), paint_metallic=V["paint_metallic"],
                         paint_rough=0.26, accent_metallic=0.30, carbon_weave=True, sponsors=True)
MIR = {k: carlib.mat("car_mirror_" + k, (0.55, 0.57, 0.60), 1.0, 0.05) for k in ("left", "right")}
C = M.carbon

# ---------------------------------------------------------------- body loft
VK = apply_variant(KEYS, V)
L = carlib.Loft(VK, samp=SAMP, ny=170, hard=(2, 3, 5))
NOSE, TAIL = L.nose, L.tail


def face_mat(ym, kk, right):
    jc = kk / SAMP
    for (y0, y1, j0, j1) in V["livery"]:
        if y0 <= ym <= y1 and j0 <= jc <= j1:
            return M.accent
    # bare carbon under the undercut's crease, and under the nose
    if jc < 2.0 and ym > -1.25:
        return C
    if jc < 1.3:
        return C
    return M.paint


body = L.build("body", face_mat, [M.paint, M.accent, C], subsurf=0)
save("body")

# --------------------------------------------------------------- apertures
# Cockpit: a lined tub opening round the eye, the seat pan at its bottom,
# the headrest behind; it stops short of the airbox.
CP_Y = (EYE.y - 0.62, EYE.y + 0.22)
CP_HW = 0.205
carlib.aperture(body, M, (-CP_HW, CP_Y[0], 0.20), (CP_HW, CP_Y[1], 1.30))
# sidepod mouths, into the front face of each pod
ix0, ix1, iz0, iz1 = INLETS[V["inlet"]]
INLET = []
for sx in (-1, 1):
    a0, a1 = sorted((sx * ix0 * V["pod_w"], sx * ix1 * V["pod_w"]))
    z0, z1 = 0.05 + (iz0 - 0.05) * V["pod_h"], 0.05 + (iz1 - 0.05) * V["pod_h"]
    # a rounded mouth, its outboard lower corner cut back the way the
    # undercut runs under it
    poly = carlib.rounded([(a0, z0 + (0.05 if sx < 0 else 0.0)), (a1, z0 + (0.05 if sx > 0 else 0.0)),
                           (a1, z1), (a0, z1)], 0.035)
    carlib.aperture_poly(body, M, poly, -1.10, -0.58, mat=M.mesh)
    INLET.append((a0, a1, z0, z1, poly))
# the engine air intake over the driver's head
AB_Y = EYE.y + 0.52
ab_top = L.roof_z(AB_Y + 0.02) - 0.030
AB_HW = 0.072 if V["airbox"] == "oval" else 0.056
carlib.aperture(body, M, (-AB_HW, AB_Y - 0.10, ab_top - 0.15), (AB_HW, AB_Y + 0.22, ab_top), mat=M.mesh)
carlib.sharpen(body, 32.0)
save("apertures")

# -------------------------------------------------------------------- parts
p = Builder("parts")

# ---- floor: a plate in plan, the edge wing along its sides, fences at its
# leading edge, the plank, and the diffuser
plan_r = [(0.24, -1.20), (0.46, -1.02), (0.70, -0.86), (0.78, -0.62), (0.80, 0.95),
          (0.74, 1.20), (0.60, 1.40), (0.52, 1.50)]
plan = plan_r + [(-x, y) for (x, y) in reversed(plan_r)]
carlib.panel_xy(p, C, plan, FLOOR_Z[0], FLOOR_Z[1])
for sx in (-1, 1):
    # the edge wing: a lip that rises off the floor edge and rolls outboard
    edge = [(-0.62, FLOOR_Z[1]), (0.95, FLOOR_Z[1]), (0.95, 0.070), (0.40, 0.095), (-0.40, 0.085)]
    carlib.plate(p, C, edge, sx * 0.792, 0.010, chamfer=0.003)
    carlib.foil_path(p, C, [sx * 0.70, sx * 0.80] if sx > 0 else [-0.80, -0.70],
                     lambda x: (-0.20, 0.090, 12.0, 0.36), 0.05, -0.03, n=8)
    # floor fences: curved blades under the leading edge, leaning outboard
    for k, fx in enumerate((0.26, 0.36, 0.46, 0.56)):
        y0 = -1.16 + 0.07 * k
        fence = [(y0, FLOOR_Z[1]), (-0.66, FLOOR_Z[1]), (-0.66, 0.16 - 0.02 * k), (y0 + 0.10, 0.20 - 0.02 * k),
                 (y0 + 0.02, 0.12)]
        carlib.plate(p, C, fence, sx * fx, 0.007, chamfer=0.002)
carlib.diffuser(p, C, 1.46, 2.18, 0.50, FLOOR_Z[1], 0.240, thick=0.012,
                strakes=(-0.72, -0.40, -0.12, 0.12, 0.40, 0.72), strake_h=0.17)
p.box(M.metal, (-0.15, -1.15, 0.008), (0.15, 1.40, FLOOR_Z[0]))           # plank and skids
for yy in (-0.70, 0.10, 0.90):
    p.box(M.metal, (-0.10, yy - 0.06, 0.006), (0.10, yy + 0.06, 0.009))

# ---- front wing: main plane and two or three flaps; flat across the
# middle under the nose, rising and steepening towards the endplates
FW = V["fw"]
NFLAP = {"classic": 2, "swept": 3, "low": 2}[FW]
X_IN = 0.20


def fw_main(x):
    t = min(abs(x) / FW_HW, 1.0)
    return (FW_Y[0] + 0.025 * (1 - t), 0.078 + 0.012 * t ** 2, 3.0 + 3.0 * t, 0.29 - 0.03 * t)


def fw_flap(k, prev):
    """Flap k sits over the trailing edge of the element before it (a slot
    of ~1 cm), rising a few centimetres and steepening as it runs out to
    the endplate. The rise is the element's own, not compounded."""
    rise = {"classic": 0.012, "swept": 0.020, "low": 0.008}[FW]
    chords = (0.140, 0.112, 0.088)

    def fn(x):
        t = min(max((abs(x) - X_IN) / (FW_HW - X_IN), 0.0), 1.0)
        ly, lz, ang, ch = prev(x)
        a = math.radians(ang)
        te_y, te_z = ly + ch * math.cos(a), lz + ch * math.sin(a)
        return (te_y - 0.030, te_z + 0.006 + rise * t * t,
                12.0 + 8.0 * k + (3.0 + 2.0 * k) * t,
                chords[k] * (1.0 - 0.15 * t))
    return fn


xs_full = [FW_HW * (2 * i / 24 - 1) for i in range(25)]
carlib.foil_path(p, C, xs_full, fw_main, 0.055, -0.035, n=14)
FLAP_MATS = [M.paint, M.accent, C]
CHAIN = [fw_main]
for k in range(NFLAP):
    CHAIN.append(fw_flap(k, CHAIN[-1]))
for sx in (-1, 1):
    xs = [sx * (X_IN + (FW_HW - X_IN - 0.012) * i / 14) for i in range(15)]
    if sx < 0:
        xs = list(reversed(xs))
    for k in range(NFLAP):
        carlib.foil_path(p, FLAP_MATS[k], xs, CHAIN[k + 1], 0.06, -0.04, n=12)
_ly, _lz, _a, _ch = CHAIN[-1](FW_HW - 0.012)
FW_TOP = _lz + _ch * math.sin(math.radians(_a))
FW_TE = max(_ly + _ch * math.cos(math.radians(_a)) + 0.02, FW_Y[1])
FW_EP = {
    # classic: a plain board, square top
    "classic": [(FW_Y[0] - 0.005, 0.045), (FW_TE, 0.050), (FW_TE + 0.01, FW_TOP + 0.03), (FW_Y[0] + 0.24, FW_TOP + 0.05),
                (FW_Y[0] + 0.02, 0.200)],
    # swept: its leading edge raked back, the top running on past the flaps
    "swept": [(FW_Y[0] + 0.02, 0.045), (FW_TE + 0.02, 0.050), (FW_TE + 0.05, FW_TOP + 0.02),
              (FW_Y[0] + 0.32, FW_TOP + 0.04), (FW_Y[0] + 0.16, 0.250), (FW_Y[0] + 0.05, 0.130)],
    # low: a squat plate with a footplate, under a two-element wing
    "low": [(FW_Y[0] - 0.005, 0.040), (FW_TE + 0.02, 0.040), (FW_TE + 0.02, FW_TOP + 0.02),
            (FW_Y[0] + 0.24, FW_TOP + 0.03), (FW_Y[0] + 0.04, 0.130)],
}[FW]
for sx in (-1, 1):
    carlib.plate(p, C, FW_EP, sx * (FW_HW + 0.006), 0.010, chamfer=0.003)
    # the footplate and the endplate's diveplane, outboard
    p.box(C, (min(sx * (FW_HW - 0.06), sx * (FW_HW + 0.012)), FW_Y[0] - 0.005, 0.036),
          (max(sx * (FW_HW - 0.06), sx * (FW_HW + 0.012)), FW_TE, 0.046))
    # nose supports: the nose sits on the main plane through two short pylons
    pz = L.floor_z(NOSE + 0.12)
    pyl = [(NOSE + 0.02, 0.105), (NOSE + 0.30, 0.105), (NOSE + 0.30, pz + 0.01), (NOSE + 0.06, pz + 0.01)]
    carlib.plate(p, C, pyl, sx * (0.055 * V.get("nose_w", 1.0) ** 0.5), 0.010, chamfer=0.003)

# ---- rear wing: main plane and DRS flap in the car's own plan, endplates,
# the beam wing over the diffuser, the pylon from the gearbox
RW_PLAN, RW_AMT = V["rw_plan"]
_, RW_TE = carlib.wing(p, C, RW_HW, 0.27, 0.10, -0.050, RW_Y[0], RW_Z, angle_deg=9.0,
                       plan=RW_PLAN, amount=RW_AMT)
# The upper flap is the DRS flap: built as its own object and exported as
# <stem>_drs.glb with its origin on the hinge, so the client can open it.
# The hinge runs across the car along the flap's trailing edge at the tips;
# opening lifts the leading edge DRS_OPEN_DEG, which is the slot a real
# flap opens between itself and the main plane.
DRS_OPEN_DEG = 25.0
fl = Builder("drs_flap")
te0 = RW_TE(RW_HW)
_, RW_TE2 = carlib.wing(fl, M.paint, RW_HW, 0.17, 0.09, -0.045, te0[0] - 0.035, te0[1] + 0.016,
                        angle_deg=38.0, plan=RW_PLAN, amount=RW_AMT)
for (xa, xb, y, z) in carlib.spans(RW_TE2, -RW_HW, RW_HW, 10):
    carlib.gurney(fl, C, xa, xb, y - 0.004, z, h=0.012, t=0.004, angle_deg=38.0)
DRS_HINGE = RW_TE2(RW_HW)                          # (y, z), Blender frame
# the flap's sponsor, along its upper face
a_fl = math.radians(38.0)
_dy0 = RW_TE2(0.0)[0] - RW_TE2(RW_HW)[0]          # the plan's offset at the middle
_dz0 = RW_TE2(0.0)[1] - RW_TE2(RW_HW)[1]
fc_y = te0[0] - 0.035 + 0.085 * math.cos(a_fl) - 0.016 * math.sin(a_fl) + _dy0
fc_z = te0[1] + 0.016 + 0.085 * math.sin(a_fl) + 0.016 * math.cos(a_fl) + _dz0
carlib.flat_decal(fl, M.sponsor, (0.0, fc_y, fc_z), (1, 0, 0), (0, math.cos(a_fl), math.sin(a_fl)),
                  0.52, 0.13, carlib.atlas_uv(SP["flap"]), lift=0.002)
EP_TOP = DRS_HINGE[1] + 0.035
EP_BACK = DRS_HINGE[0] + 0.02
RW_EP = {
    "square": [(RW_Y[0] - 0.06, 0.58), (EP_BACK, 0.62), (EP_BACK + 0.005, EP_TOP),
               (RW_Y[0] + 0.08, EP_TOP + 0.01), (RW_Y[0] - 0.08, RW_Z + 0.06)],
    # swept: the leading edge raked forward as it climbs, the tip cut back
    "swept": [(RW_Y[0] + 0.02, 0.56), (EP_BACK + 0.005, 0.60), (EP_BACK - 0.03, EP_TOP + 0.02),
              (RW_Y[0] - 0.02, EP_TOP + 0.03), (RW_Y[0] - 0.12, RW_Z + 0.07)],
    # curl: a rounded top that rolls into the main plane
    "curl": [(RW_Y[0] - 0.04, 0.58), (EP_BACK, 0.60), (EP_BACK + 0.005, EP_TOP - 0.04),
             (EP_BACK - 0.06, EP_TOP + 0.01), (RW_Y[0] + 0.06, EP_TOP), (RW_Y[0] - 0.08, RW_Z + 0.08)],
}[V["rw"]]
for sx in (-1, 1):
    carlib.plate(p, C, RW_EP, sx * (RW_HW + 0.008), 0.012, chamfer=0.004)
    if V["rw"] == "curl":
        # the tip rolls inboard over the flap
        p.box(M.paint, (min(sx * (RW_HW - 0.05), sx * (RW_HW + 0.014)), RW_Y[0] + 0.04, EP_TOP - 0.010),
              (max(sx * (RW_HW - 0.05), sx * (RW_HW + 0.014)), EP_BACK - 0.03, EP_TOP + 0.003))
    # the endplate's sponsor, on its outer face
    ec_y = (RW_Y[0] + EP_BACK) / 2
    carlib.flat_decal(p, M.sponsor, (sx * (RW_HW + 0.0145), ec_y, (0.62 + EP_TOP) / 2 + 0.02),
                      (0, sx, 0), (0, 0, 1), 0.34, 0.085, carlib.atlas_uv(SP["rw_ep"]), lift=0.0005)
    # the endplate rain lights of the current rules, up the trailing edge
    p.box(M.rain, (sx * (RW_HW + 0.016) - 0.004, EP_BACK - 0.022, 0.66), (sx * (RW_HW + 0.016) + 0.004, EP_BACK - 0.004, 0.80))
rw_dz = RW_TE(0.0)[1] - RW_TE(RW_HW)[1]          # the plane's middle over its tips
pyl = [(1.98, L.roof_z(1.98) - 0.02), (2.10, L.roof_z(2.10) - 0.02), (RW_Y[0] + 0.20, RW_Z + 0.015 + rw_dz),
       (RW_Y[0] + 0.06, RW_Z + 0.005 + rw_dz)]
carlib.plate(p, C, pyl, 0.0, 0.018, chamfer=0.005)
# beam wing: one or two elements low over the diffuser exit, on their own endplates
carlib.foil(p, C, -0.44, 0.44, 0.19, 0.08, -0.04, 2.03, 0.290, angle_deg=10.0)
if V.get("beam", 1) == 2:
    carlib.foil(p, C, -0.42, 0.42, 0.11, 0.08, -0.04, 2.19, 0.330, angle_deg=30.0)
for sx in (-1, 1):
    carlib.plate(p, C, [(2.00, 0.24), (2.33, 0.25), (2.34, 0.40), (2.02, 0.36)], sx * 0.445, 0.008, chamfer=0.002)
if V.get("fin"):
    # the shark fin: the engine cover's spine run up towards the wing
    fin = [(0.70, L.roof_z(0.70) - 0.01), (1.20, L.roof_z(1.20) + 0.12), (RW_Y[0] - 0.02, RW_Z - 0.02 + rw_dz),
           (RW_Y[0] - 0.02, RW_Z - 0.12), (1.80, L.roof_z(1.80) - 0.01)]
    carlib.plate(p, M.paint, fin, 0.0, 0.010, chamfer=0.003)
carlib.led_grid(p, M, -0.036, 0.036, 0.205, 0.315, TAIL + 0.004, dir_y=1.0,
                cols=2, rows=5, glow=M.rain)
# exhaust over the crash structure
p.cylinder(M.metal, (0.0, 2.20, 0.405), 0.045, 0.20, segs=24, axis='Y')
p.cylinder(M.lamp_h, (0.0, 2.24, 0.405), 0.037, 0.14, segs=24, axis='Y')
p.torus(M.metal, (0.0, 2.30, 0.405), 0.041, 0.005, segs=24, rings=6)

# ---- halo: a 4 cm section. Centre pillar off the tub ahead of the driver,
# the hoop round the cockpit just over the eye line, the rear legs down to
# the tub behind the shoulders. In the body colour unless the car says carbon.
HALO = M.paint if V["halo"] == "paint" else C
HZ = EYE.z + 0.060
hy0 = CP_Y[0] + 0.02
pillar_foot_y = hy0 - 0.24
pf = [(pillar_foot_y, L.roof_z(pillar_foot_y) - 0.015), (hy0 + 0.02, L.roof_z(hy0) - 0.01),
      (hy0 + 0.05, HZ - 0.02), (hy0 - 0.02, HZ + 0.03), (pillar_foot_y + 0.06, L.roof_z(pillar_foot_y + 0.06) + 0.03)]
carlib.plate(p, HALO, pf, 0.0, 0.048, chamfer=0.012)
for sx in (-1, 1):
    hoop = []
    for k in range(48):
        a = math.pi * 0.84 * k / 47
        hoop.append((sx * 0.255 * math.sin(a), hy0 + 0.04 + 0.48 * (1 - math.cos(a)) / 2, HZ + 0.012 * math.cos(a)))
    x_e, y_e, _ = hoop[-1]
    hoop += [(sx * (abs(x_e) + 0.03), y_e + 0.06, HZ - 0.06),
             (sx * (abs(x_e) + 0.05), y_e + 0.10, L.z_at(y_e + 0.10, abs(x_e) + 0.05) - 0.02)]
    carlib.guide_xyz(p, HALO, hoop, r=0.024, segs=16)

# ---- cockpit: seat back and headrest pads, the padded rim, the pan the cut
# left, the front bulkhead with a display, a harness
seat_y = CP_Y[1] - 0.05
carlib.plate(p, M.seat, [(seat_y - 0.10, 0.22), (seat_y, 0.22), (seat_y + 0.02, 0.62), (seat_y - 0.04, 0.62)],
             0.0, 0.36, chamfer=0.02)
rim_z = L.z_at(EYE.y, CP_HW + 0.01)
for sx in (-1, 1):
    # head protection pads along the cockpit sides, beside the helmet
    pad = [(EYE.y - 0.16, rim_z - 0.08), (CP_Y[1], rim_z - 0.08), (CP_Y[1], rim_z + 0.05), (EYE.y - 0.10, rim_z + 0.03)]
    carlib.plate(p, M.alcantara, pad, sx * (CP_HW - 0.035), 0.07, chamfer=0.012)
p.box(M.alcantara, (-0.12, CP_Y[1] - 0.07, L.z_at(CP_Y[1], 0.10) - 0.14), (0.12, CP_Y[1], L.z_at(CP_Y[1], 0.10) + 0.03))
p.box(M.interior, (-CP_HW + 0.01, CP_Y[0], 0.20), (CP_HW - 0.01, CP_Y[1], 0.22))
bh_y = CP_Y[0] + 0.02
# the front bulkhead stays under the wheel (the rig draws the wheel and its
# display); the tub's inside walls in the interior finish, not the paint
p.box(M.interior, (-CP_HW + 0.01, bh_y - 0.02, 0.30), (CP_HW - 0.01, bh_y, 0.50))
for sx in (-1, 1):
    p.box(M.interior, (min(sx * (CP_HW - 0.012), sx * (CP_HW - 0.002)), CP_Y[0], 0.22),
          (max(sx * (CP_HW - 0.012), sx * (CP_HW - 0.002)), CP_Y[1], rim_z - 0.012))
p.box(M.interior, (-CP_HW + 0.01, CP_Y[0] + 0.002, 0.22), (CP_HW - 0.01, CP_Y[0] + 0.012, rim_z - 0.015))
for sx in (-1, 1):
    p.bar(M.harness, (sx * 0.10, seat_y - 0.06, 0.60), (sx * 0.06, EYE.y - 0.22, 0.40), 0.020, segs=4)

# ---- mirrors: aero heads on stalks off the pod tops, the glass in the
# car_mirror_* slots the cockpit rig paints its capture onto; written into
# car.toml's [cockpit] as built
MIR_Y = EYE.y - 0.62
MIR_X = 0.50
MIR_Z = EYE.z - 0.06
MIRRORS = {}
for sx, key in ((1, "left"), (-1, "right")):
    gc, gsz = carlib.aero_mirror(p, M, MIR[key], (sx * MIR_X, MIR_Y, MIR_Z), w=0.17, h=0.070, d=0.10,
                                 housing=M.paint)
    MIRRORS[key] = (gc, gsz)
    base_z = L.z_at(MIR_Y + 0.04, MIR_X - 0.08)
    stalk = [(MIR_Y - 0.04, base_z - 0.02), (MIR_Y + 0.06, base_z - 0.02), (MIR_Y + 0.03, MIR_Z - 0.02),
             (MIR_Y - 0.02, MIR_Z - 0.02)]
    carlib.plate(p, C, stalk, sx * (MIR_X - 0.07), 0.012, chamfer=0.004)
    # the second stay to the halo's leg, as the cars carry them
    p.bar(C, (sx * (MIR_X - 0.08), MIR_Y + 0.02, MIR_Z + 0.02), (sx * 0.24, hy0 + 0.20, HZ - 0.01), 0.007, segs=6)

# ---- intakes: dark backing in the sidepod mouths and the airbox, a lip
# round each so the hole has an edge, and the cooling gills on the pod tops
for (a0, a1, z0, z1, poly) in INLET:
    carlib.poly_fill(p, M.lamp_h, poly, -0.60, -0.58)
    carlib.poly_bars(p, C, poly, -0.66, angle_deg=0.0, pitch=0.045, t=0.006, depth=0.02)
    yf = carlib.surface_station(L, a1 if a1 > 0 else -a0, (z0 + z1) / 2, -1.2, step=0.004, margin=0.0)
    carlib.poly_rim(p, C, poly, yf - 0.004, w=0.012, depth=0.020, dir_y=1.0)
for sx in (-1, 1):
    for k in range(6):
        yy = 0.12 + 0.065 * k
        carlib.conform_decal(p, M.mesh, L, yy, yy + 0.028, 4.35, 4.95, sx=sx, lift=0.0025, nu=2, nv=4)
p.box(M.lamp_h, (-AB_HW - 0.005, AB_Y + 0.17, ab_top - 0.16), (AB_HW + 0.005, AB_Y + 0.20, ab_top + 0.005))
for sx in (-1, 1):
    p.bar(C, (sx * AB_HW, AB_Y - 0.10, ab_top - 0.15), (sx * AB_HW, AB_Y - 0.10, ab_top), 0.008, segs=8)
p.bar(C, (-AB_HW, AB_Y - 0.10, ab_top), (AB_HW, AB_Y - 0.10, ab_top), 0.008, segs=8)
tc_y = AB_Y + 0.02
tc_z = L.roof_z(tc_y)
tcam = carlib.mat("car_tcam", V["tcam"], 0.2, 0.35)
p.box(tcam, (-0.040, tc_y - 0.07, tc_z - 0.004), (0.040, tc_y + 0.07, tc_z + 0.042))
p.box(M.glass, (-0.030, tc_y - 0.072, tc_z + 0.010), (0.030, tc_y - 0.068, tc_z + 0.032))
# pitot and antennae
p.bar(M.metal, (0.0, -2.05, L.roof_z(-2.05)), (0.0, -2.30, L.roof_z(-2.05) + 0.03), 0.004, segs=6)
p.bar(C, (0.0, 1.30, L.roof_z(1.30)), (0.0, 1.36, L.roof_z(1.30) + 0.16), 0.003, segs=6)

# ---- suspension: wishbones, push/pull rods and track rods, carbon aero
# sections; brake ducts with the wheel-wake deflector over the front ones
for (ay, track, width, inner_x, zl, zu) in ((AX_F, TRACK_F, W_F, 0.17, 0.20, 0.44),
                                            (AX_R, TRACK_R, W_R, 0.16, 0.16, 0.36)):
    hub = track / 2.0
    ox = hub - width / 2.0 - 0.02                    # just inside the rim
    for sx in (-1, 1):
        for z_in, z_out, spread in ((zl, 0.20, 0.26), (zu, 0.50 if ay < 0 else 0.48, 0.20)):
            for dy in (-spread, spread):
                y_in = ay + dy
                x_in = max(L.x_at(y_in, z_in) - 0.01, inner_x) if ay < 0 else inner_x
                p.bar(C, (sx * x_in, y_in, z_in), (sx * ox, ay, z_out), 0.013, segs=8)
        if ay < 0:
            p.bar(C, (sx * (ox - 0.02), ay + 0.02, 0.22), (sx * inner_x, ay + 0.10, zu + 0.06), 0.011, segs=8)
        else:
            p.bar(C, (sx * (ox - 0.02), ay - 0.02, 0.46), (sx * inner_x, ay - 0.10, 0.15), 0.011, segs=8)
        p.bar(C, (sx * inner_x, ay + (0.12 if ay < 0 else -0.12), zl + 0.10), (sx * ox, ay + 0.12, 0.30), 0.010, segs=8)
        p.cylinder(C, (sx * (ox - 0.035) - 0.03, ay, 0.356), 0.19, 0.06, segs=28, axis='X')
        p.cylinder(M.lamp_h, (sx * (ox - 0.045) - 0.03, ay - 0.13, 0.250), 0.045, 0.05, segs=16, axis='X')
        if ay < 0:
            # the wheel-wake deflector: a small wing on the duct, ahead of the tyre's top
            xd = sx * (ox - 0.06)
            carlib.foil_path(p, C, sorted((xd - 0.06 * sx, xd + 0.02 * sx)),
                             lambda x: (ay - 0.36, 0.40, 18.0, 0.10), 0.07, -0.04, n=8)

# ---- numbers and wordmarks: the race number on the nose, the team's
# wordmark (the livery's logo) on the engine cover, sponsors from the atlas
NUM = V["number"]
carlib.top_patch(p, M.decal, L, -2.36, -2.10, -0.085, 0.085, lift=0.003, nu=6, nv=4)
carlib.top_text(p, M.trim, L, NUM, 0.0, -2.23, size=0.14, thick=0.005, lift=0.004,
                face="front", squash=0.80)
for sx in (-1, 1):
    cy = -1.95
    cz = L.z_at(cy, 0.10)
    p.bar(C, (sx * 0.12, cy, cz - 0.02), (sx * 0.17, cy, cz + 0.01), 0.008, segs=6)
    p.box(M.accent, (sx * 0.17 - 0.016, cy - 0.045, cz), (sx * 0.17 + 0.016, cy + 0.045, cz + 0.028))
    # the team's wordmark high on the engine cover, the main sponsor on the pod
    carlib.conform_decal(p, M.logo, L, 0.62, 1.30, 5.85, 6.95, sx=sx, lift=0.004, nu=14, nv=6,
                         flip_u=sx < 0)
    carlib.conform_decal(p, M.sponsor, L, -0.55, 0.45, 2.55, 3.55, sx=sx, lift=0.004, nu=16, nv=6,
                         flip_u=sx < 0, uv_rect=carlib.atlas_uv(SP["pod"]))
    carlib.conform_decal(p, M.sponsor, L, -0.05, 0.55, 4.55, 5.05, sx=sx, lift=0.004, nu=10, nv=4,
                         flip_u=sx < 0, uv_rect=carlib.atlas_uv(SP["cover"]))
    carlib.conform_decal(p, M.sponsor, L, -2.05, -1.68, 3.0, 4.6, sx=sx, lift=0.003, nu=8, nv=4,
                         flip_u=sx < 0, uv_rect=carlib.atlas_uv(SP["nose"]))
    # the front endplates' outer faces
    carlib.flat_decal(p, M.sponsor, (sx * (FW_HW + 0.0115), (FW_Y[0] + FW_TE) / 2 + 0.03, 0.13),
                      (0, sx, 0), (0, 0, 1), 0.30, 0.075, carlib.atlas_uv(SP["fw_ep"]), lift=0.0005)

# ---- the driver (pass 7): lying back in the tub, gloves on the rig's rim,
# feet up on the pedals behind the front axle; exported apart from the body
drv = Builder("driver")
carlib.driver_figure(drv, M, EYE, CK["wheel"], (0.0, EYE.y - 0.42, 0.20), (AX_F + 0.14, 0.30),
                     wheel_hw=0.13, suit=V["paint"], style="open")

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.0025, segments=2,
                     angle_deg=38.0)
flap = carlib.bevel(fl.finish(planar_uv=True, recalc=False), width=0.0025, segments=2, angle_deg=38.0)
for v in flap.data.vertices:
    v.co.y -= DRS_HINGE[0]
    v.co.z -= DRS_HINGE[1]
flap.name = V["stem"] + "_drs"
save("parts")


def write_drs_table(toml_path, stem, hinge_yz, open_deg):
    """Put the flap's `[drs_flap]` table in car.toml (docs/CAR_MODELS.md): the
    GLB, the hinge in the wheels' convention (forward of the body origin,
    up from the floor, metres) and how far it opens. Replaced on every run,
    kept above the liveries' marker (`carlib.write_table`)."""
    carlib.write_table(toml_path, "drs_flap", [
        "[drs_flap]",
        "# the rear wing's upper element, cut out of the body so the client can open it",
        'model = "%s_drs.glb"' % stem,
        "hinge_forward_m = %.4f" % -hinge_yz[0],
        "hinge_up_m = %.4f" % hinge_yz[1],
        "open_deg = %.1f" % open_deg])

# ------------------------------------------------------------ join + export
car, glb = carlib.join_and_export([body, parts], V["stem"], CAR_DIR,
                                  export=os.environ.get("APEX_EXPORT", "1") == "1")
# the driver, in his own GLB (pass 7; see carlib.driver_figure)
DRIVER = carlib.export_driver(drv, V["stem"], CAR_DIR, export=bool(glb))
if glb:
    carlib.export_glb(flap, os.path.join(CAR_DIR, V["stem"] + "_drs.glb"))
    write_drs_table(os.path.join(CAR_DIR, "car.toml"), V["stem"], DRS_HINGE, DRS_OPEN_DEG)
    carlib.write_cockpit_table(CAR_DIR, CK, 180.0, style="open", rake_deg=12.0, mirrors=MIRRORS,
                               note="Pass 6: authored against the imported F1s (1.43-1.57 m behind the front axle).")
flap.hide_render = flap.hide_viewport = True
print("drs flap: hinge y/z", [round(c, 4) for c in DRS_HINGE], "open", DRS_OPEN_DEG)
save("joined")
st = carlib.mesh_stats(car)
print("stats:", st)
print("sightline:", carlib.sightline(car, eye=EYE, ray_x=0.032))
print("eye:", [round(c, 3) for c in EYE], "mirrors:", {k: [round(c, 3) for c in v[0]] for k, v in MIRRORS.items()})
if glb:
    print("GLB:", glb, os.path.getsize(glb))
