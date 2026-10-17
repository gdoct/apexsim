"""Build one of the Group B rally cars in Blender and export it to
content/cars/default/<folder>/<stem>.glb.

    VARIANT = "oudi"      # oudi | pugeot | lancha
    exec(open(r"E:\\apexsim\\scripts\\content\\cars\\build_rally.py").read())

Headless: `blender -b --factory-startup --python-expr "VARIANT='oudi';
exec(open(r'...build_rally.py').read())"`.

The shape data is in rally_cars.py (shared with the livery painter); this
script is the GT3 build's method (scripts/content/cars/build_gt3.py, on
carlib.py) with a rally car's parts: a higher floor, box flares, period
lamps, mud flaps, a roof scoop, and each car's own aero (a shovel splitter
and boot wing, a roof wing, a ducktail).

The painted shell is not `car_paint` but `car_skin`: its faces are projected
onto skin_layout.py's regions, and the works skin
(`textures/skin_works.png`, drawn by `liveries.py` from rally_skins.py) is
the GLB's own texture. A livery is another skin on the same projection
(docs/content/cars.md, Liveries). Run `python scripts/content/cars/liveries.py
<folder>` before the build so the works skin exists.
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

# ------------------------------------------------------------------ loading
_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")
_HERE = os.path.join(_ROOT, "scripts", "content", "cars")
for _n, _p in (("apex", os.path.join(_ROOT, "scripts", "content", "props", "apex_props.py")),
               ("carlib", os.path.join(_HERE, "carlib.py")),
               ("skin_layout", os.path.join(_HERE, "skin_layout.py")),
               ("rally_cars", os.path.join(_HERE, "rally_cars.py"))):
    _s = importlib.util.spec_from_file_location(_n, _p)
    _m = importlib.util.module_from_spec(_s)
    sys.modules[_n] = _m
    _s.loader.exec_module(_m)
import apex, carlib, skin_layout, rally_cars                # noqa: E402
from carlib import Builder                                  # noqa: E402

try:
    VARIANT
except NameError:
    VARIANT = "oudi"
V = rally_cars.VARIANTS[VARIANT]
CAR_DIR = os.path.join(carlib.CARS_ROOT, V["folder"])
AX_F, AX_R = V["axles"]
TYRE_F, TYRE_R = V["tyres"]
HALF_W = V["half_w"]
HUB_X = V["track"] / 2.0
XS = HALF_W                       # the GT3 build's lateral figures were for a 1.0 m half-width
ARCH_GAP = 0.030
ARCH_WRAP = 24.0
SAMP = 6
BELT, ROOF = 5.35, 6.60
TRIM_J = 0.20
TRIM_Y = 0.040
SUNSTRIP = 0.13
SKIN_BOX = V["skin_box"]


def save(tag):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CAR_DIR, V["stem"] + ".blend"))
    print("saved", tag)


# ---------------------------------------------------------------- materials
carlib.reset_scene()
WORKS = os.path.join(CAR_DIR, "textures", "skin_works.png")
if not os.path.exists(WORKS):
    raise SystemExit("no %s: run scripts/content/cars/liveries.py %s first" % (WORKS, V["folder"]))
M = carlib.car_materials((0.80, 0.80, 0.78), (0.03, 0.03, 0.035), (0.80, 0.55, 0.05),
                         paint_metallic=0.10, paint_rough=0.30, accent_metallic=0.10, accent_rough=0.35,
                         carbon_weave=True, sponsors=False)
# the shell: the works skin under a clear coat; a livery swaps the image
M["skin"] = carlib.textured_mat("car_skin", WORKS, metallic=0.05, roughness=0.32, coat=1.0, coat_roughness=0.04)
MIR = {k: carlib.mat("car_mirror_" + k, (0.55, 0.57, 0.60), 1.0, 0.05) for k in ("left", "right")}

# ---------------------------------------------------------------- body loft
KEYS = V["keys"]
KEYS = carlib.scale_width(KEYS, HALF_W / max(x for (_, pts) in KEYS for (x, _) in pts))
if V.get("roof"):
    KEYS = carlib.roofline(KEYS, V["roof"], j_from=6, pivot_j=5)
KEYS = carlib.fender_bump(KEYS, (AX_F, AX_R), amount=V["hips"], width=(0.50, 0.55), j0=2.2, j1=4.3)
KEYS = carlib.tumblehome(KEYS, amount=0.015, j_from=6)
E = V["ends"]
K0, K1 = V["keys"][0][0], V["keys"][-1][0]
KEYS = carlib.insert_stations(KEYS, [K0 + 0.10, K0 + 0.24, K1 - 0.10, K1 - 0.24])
KEYS = carlib.remap_station(KEYS, K0, x_scale=E["nose_w"], z_hi=E["nose_top"], j_from=2)
KEYS = carlib.remap_station(KEYS, K0 + 0.10, x_scale=0.975, z_hi=E["nose_top"] + 0.04, j_from=3)
KEYS = carlib.remap_station(KEYS, K1, x_scale=E["tail_w"], z_lo=E["tail_bot"], z_hi=E["tail_top"])
KEYS = carlib.remap_station(KEYS, K1 - 0.10, x_scale=0.985, z_hi=E["tail_top"] + 0.02, j_from=4)
KEYS = carlib.tip_station(KEYS, K0, K0 - 0.05, x_scale=0.82, z_shrink=0.74)
KEYS = carlib.tip_station(KEYS, K1, K1 + 0.04, x_scale=0.84, z_shrink=0.76)
L = carlib.Loft(KEYS, samp=SAMP, ny=110, hard=(2, 3, 4, 5))
NOSE, TAIL = L.nose, L.tail
BUMPER_Y, TAILP_Y = K0, K1

ARCH_SPAN = [(AX_F - TYRE_F - ARCH_GAP, AX_F + TYRE_F + ARCH_GAP),
             (AX_R - TYRE_R - ARCH_GAP, AX_R + TYRE_R + ARCH_GAP)]


def in_arch(y):
    return any(a - 0.03 <= y <= b + 0.03 for (a, b) in ARCH_SPAN)


for (y, kind) in V["shutlines"]:
    if kind == "upper":
        L.groove(y, 4.2, 8.0, depth=0.010, width=0.014)
    elif not in_arch(y):
        L.groove(y, 1.9, 4.9, depth=0.010, width=0.014)

SIDE_FLOORS = []
for (kind, kw) in V["side"]:
    L.recess(**kw)
    if L.last_feature["swept"] and kw["depth"] > 0.022:
        SIDE_FLOORS.append(L.last_feature)
if "bonnet" in V["vents"]:
    # the E2's bonnet: two exits over the radiator, ahead of the screen
    for x0, x1 in ((0.12, 0.44),):
        L.recess(V["screen_y"][0] - 0.62, V["screen_y"][0] - 0.30, 6.2, 7.6, depth=0.026, rim=0.020)
if "deck" in V["vents"]:
    L.recess(V["rear_glass_y"][1] + 0.12, V["rear_glass_y"][1] + 0.78, 6.0, 8.0, depth=0.030, rim=0.026)


def face_mat(ym, kk, right):
    """The skin, glass, or the black surround round the glass; the sun
    strip across the top of the screen is skin (the livery's banner)."""
    jc = kk / SAMP
    s0, s1 = V["screen_y"]
    r0, r1 = V["rear_glass_y"]
    g0, g1 = V["glass_y"]
    is_screen = s0 < ym < s1
    is_rear = r0 < ym < r1
    is_side = g0 < ym < g1
    if jc < BELT - TRIM_J:
        if L._offset(ym, kk, right, skip_swept=True) > 0.022:
            return M.mesh
        return M.skin
    if jc < BELT:
        return M.trim if (is_screen or is_rear or is_side) else M.skin
    if is_screen:
        return M.skin if ym > s1 - SUNSTRIP else M.glass
    if is_rear:
        return M.glass
    if is_side and jc < ROOF:
        return M.glass
    if jc < ROOF + 0.5 and any(abs(ym - e) < TRIM_Y for e in (s0, r0, r1, g0, g1)):
        return M.trim
    return M.skin


body = L.build("body", face_mat, [M.skin, M.glass, M.trim, M.mesh], subsurf=0)

# -------------------------------------------------------------- wheel arches
X_IN = HUB_X - 0.25                    # the arch liner's inner wall
parts_objs = []
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    for sx in (-1, 1):
        bead = carlib.arch(body, L, M, y, tyre, sx=sx, gap=ARCH_GAP, x_in=X_IN, z_bottom=0.14,
                           bead=True, wrap_deg=ARCH_WRAP, lip=0.024, flare=0.020)
        if bead is not None:
            parts_objs.append(bead)
carlib.sharpen(body, 34.0)

# --------------------------------------------------------------- apertures
NT = E["nose_top"]
FACE = []
if V["grille"] == "slats":
    # a wide slatted grille between the lamps over a deep intake in the dam
    FACE.append(dict(poly=[(-0.36 * XS, NT - 0.150), (0.36 * XS, NT - 0.150), (0.36 * XS, NT - 0.030),
                           (-0.36 * XS, NT - 0.030)], r=0.012, bars=[(0.0, 0.026)]))
    FACE.append(dict(poly=[(-0.55 * XS, NT - 0.390), (0.55 * XS, NT - 0.390), (0.60 * XS, NT - 0.250),
                           (-0.60 * XS, NT - 0.250)], r=0.025, bars=[(0.0, 0.040)]))
else:
    # the 037's mouth: one broad meshed opening in the nose under the lamps
    FACE.append(dict(poly=[(-0.50 * XS, NT - 0.330), (0.50 * XS, NT - 0.330), (0.56 * XS, NT - 0.180),
                           (-0.56 * XS, NT - 0.180)], r=0.030, bars=[(35.0, 0.030), (-35.0, 0.030)]))
for spec in FACE:
    poly = carlib.rounded(spec["poly"], spec["r"])
    yb = carlib.poly_depth(L, poly, NOSE, margin=0.035)
    carlib.aperture_poly(body, M, poly, NOSE - 0.06, yb + 0.03, mat=M.mesh)
    spec["cut"] = (poly, yb)
LAMP_X = (V["lamp_x"][0] * XS, V["lamp_x"][1] * XS)
LAMP_Z = (NT - 0.025 - V["lamp_h"], NT - 0.025)
LAMP_ZC = (LAMP_Z[0] + LAMP_Z[1]) / 2
LAMP_YB = carlib.surface_station(L, LAMP_X[1], LAMP_ZC, NOSE, margin=0.02) + 0.05
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    carlib.aperture(body, M, (x0, NOSE - 0.06, LAMP_Z[0]), (x1, LAMP_YB, LAMP_Z[1]))
VAL_Z = (E["tail_bot"] + 0.02, E["tail_bot"] + 0.14)
VAL_HW = L.x_at(TAILP_Y, VAL_Z[1]) - 0.16
VAL_POLY = carlib.rounded([(-VAL_HW, VAL_Z[0]), (VAL_HW, VAL_Z[0]), (VAL_HW, VAL_Z[1]), (-VAL_HW, VAL_Z[1])], 0.02)
carlib.aperture_poly(body, M, VAL_POLY, TAILP_Y - 0.20, TAIL + 0.05, mat=M.mesh)
carlib.sharpen(body, 34.0)
save("body")

# -------------------------------------------------------------------- parts
p = Builder("parts")
CK = carlib.authored_cockpit(V["eye"])
EYE, DASH_Z = CK["eye"], CK["dash_z"]
DX = EYE.x
SILL_X = L.x_at(0.0, 0.22)

# ---- the face: mesh behind each opening, slats or a diamond mesh
for spec in FACE:
    poly, yb = spec["cut"]
    carlib.poly_fill(p, M.mesh, poly, yb, yb + 0.012)
    for (ang, pitch) in spec["bars"]:
        carlib.poly_bars(p, M.trim, poly, yb - 0.030, angle_deg=ang, pitch=pitch, t=0.008, depth=0.026)
carlib.poly_fill(p, M.mesh, VAL_POLY, TAILP_Y - 0.15, TAILP_Y - 0.13)


def nose_at(x, z, back=0.0):
    return carlib.surface_station(L, abs(x), z, NOSE, step=0.008, margin=0.004) + back


LAMP_AX = lambda sx: Vector((sx * 0.10, -0.99, 0.0)).normalized()
for sx in (-1, 1):
    x0, x1 = sorted((sx * LAMP_X[0], sx * LAMP_X[1]))
    xo, xi = (x1, x0) if sx > 0 else (x0, x1)
    z0, z1 = LAMP_Z
    p.box(M.lamp_h, (x0, LAMP_YB - 0.022, z0), (x1, LAMP_YB - 0.002, z1))
    if V["lamps"] == "round":
        # twin round lamps a side, the inner pair the driving lamps
        w = abs(x1 - x0)
        for k, f in enumerate((0.27, 0.73)):
            cx = xi + sx * w * f
            carlib.projector(p, M, (cx, nose_at(cx, LAMP_ZC, 0.030), LAMP_ZC), LAMP_AX(sx),
                             min(0.044, (z1 - z0) / 2 - 0.006), depth=0.045)
    else:
        # a rectangular reflector lamp: a main projector and a smaller one
        w = abs(x1 - x0)
        carlib.projector(p, M, (xo - sx * w * 0.30, nose_at(xo - sx * w * 0.3, LAMP_ZC, 0.030), LAMP_ZC),
                         LAMP_AX(sx), min(0.050, (z1 - z0) / 2 - 0.008), depth=0.045)
        carlib.projector(p, M, (xi + sx * w * 0.22, nose_at(xi + sx * w * 0.22, LAMP_ZC, 0.030), LAMP_ZC),
                         LAMP_AX(sx), min(0.034, (z1 - z0) / 2 - 0.012), depth=0.040)
    carlib.front_lens(p, M.glass, L, x0 - 0.004, x1 + 0.004, z0 - 0.004, z1 + 0.004, NOSE,
                      lift=-0.0025, nu=10, nv=4, margin=0.0)

# ---- splitter / chin
SPL_Z = V["floor"] - 0.035
if V["splitter"] == "shovel":
    # the E2's shovel: a broad plate well ahead of the nose, carried on
    # fences, and a lip under it
    plan = carlib.floor_plan(L, NOSE + 0.01, AX_F - 0.30, 0.20, steps=12, inset=-0.025)
    carlib.panel_xy(p, M.trim, plan, SPL_Z - 0.010, SPL_Z + 0.010)
    for sx in (-1, 1):
        fy0, fy1 = NOSE + 0.02, NOSE + 0.36
        fence = [(fy0, SPL_Z), (fy1, SPL_Z), (fy1, SPL_Z + 0.12), (fy0 + 0.06, SPL_Z + 0.08)]
        carlib.plate(p, M.trim, fence, sx * (L.x_at(NOSE + 0.20, 0.22) - 0.08), 0.012, chamfer=0.004)
else:
    # a slim chin lip tucked under the nose, proud of it by a finger
    plan = carlib.floor_plan(L, NOSE + 0.06, NOSE + 0.40, 0.18, steps=8, inset=0.010)
    carlib.panel_xy(p, M.trim, plan, SPL_Z + 0.010, SPL_Z + 0.024)

# ---- rear aero
WZ, WY, WHW = V["wing_z"], V["wing_y"], V["wing_hw"] * XS
if V["wing"] in ("boot", "roof"):
    (te_y, te_z), TE = carlib.wing(p, M.accent, WHW, V["wing_chord"], 0.075, -0.035, WY, WZ - 0.03,
                                   angle_deg=8.0, plan="straight", amount=0.0)
    ep = [(WY - 0.05, WZ - 0.11), (WY + V["wing_chord"] + 0.06, WZ - 0.12),
          (WY + V["wing_chord"] + 0.06, WZ + 0.09), (WY + 0.02, WZ + 0.07)]
    for sx in (-1, 1):
        carlib.plate(p, M.accent, ep, sx * (WHW + 0.010), 0.012, chamfer=0.004)
    if V["wing"] == "boot":
        # pillars down onto the boot lid
        for sx in (-1, 1):
            bx = WHW * 0.78
            bz = L.z_at(WY + 0.12, bx)
            post = [(WY + 0.02, bz - 0.02), (WY + 0.24, bz - 0.02), (WY + 0.20, WZ - 0.02), (WY + 0.06, WZ - 0.02)]
            carlib.plate(p, M.accent, post, sx * bx, 0.030, chamfer=0.008)
    else:
        # the 205's roof wing: the plane off the back of the roof, its
        # endplates dropping onto the hatch's flanks
        for sx in (-1, 1):
            bx = WHW * 0.86
            post = [(WY - 0.20, L.z_at(WY - 0.20, bx) - 0.02), (WY + 0.05, L.z_at(WY - 0.20, bx) - 0.02),
                    (WY + 0.10, WZ - 0.02), (WY - 0.05, WZ - 0.02)]
            carlib.plate(p, M.accent, post, sx * bx, 0.024, chamfer=0.006)
else:
    # the 037's ducktail: a lip standing up off the tail of the clamshell
    d0, d1 = TAILP_Y - 0.28, TAILP_Y + 0.02
    dt = [(d0, L.roof_z(d0) - 0.012), (d1, L.roof_z(d1) + 0.040), (d1, L.roof_z(d1) + 0.075),
          (d0 + 0.02, L.roof_z(d0) + 0.016)]
    carlib.plate(p, M.accent, dt, 0.0, 2.0 * WHW * 0.96, chamfer=0.012)

# ---- tail lights
LAMP_OUT = TAIL + 0.010
TL = V["tail"]
tz = E["tail_top"] - 0.20
if TL == "bar":
    tx = L.x_at(TAILP_Y, tz) - 0.06
    carlib.tail_bar(p, M, -tx, tx, TAIL + 0.004, tz, h=0.11, glow=M.tail, dir_y=-1.0, guide_r=0.016)
    for sx in (-1, 1):
        b0, b1 = sorted((sx * (tx - 0.24), sx * (tx - 0.04)))
        carlib.led_grid(p, M, b0, b1, tz - 0.040, tz + 0.040, TAIL + 0.016, dir_y=-1.0,
                        cols=3, rows=1, glow=M.brake, pitch_gap=0.30)
elif TL == "blocks":
    for sx in (-1, 1):
        tx = L.x_at(TAILP_Y, tz) - 0.05
        b0, b1 = sorted((sx * (tx - 0.20), sx * tx))
        grid = [(b0, tz - 0.12), (b1, tz - 0.12), (b1, tz + 0.10), (b0, tz + 0.10)]
        carlib.poly_fill(p, M.lamp_h, grid, LAMP_OUT - 0.040, LAMP_OUT)
        carlib.led_grid(p, M, b0, b1, tz + 0.01, tz + 0.09, LAMP_OUT + 0.002, dir_y=-1.0,
                        cols=2, rows=1, glow=M.tail)
        carlib.led_grid(p, M, b0, b1, tz - 0.11, tz - 0.01, LAMP_OUT + 0.002, dir_y=-1.0,
                        cols=2, rows=1, glow=M.brake)
else:
    # four round lamps, two a side
    for sx in (-1, 1):
        tx = L.x_at(TAILP_Y, tz) - 0.10
        for k, (dx, mat) in enumerate(((0.0, M.brake), (0.15, M.tail))):
            cx = sx * (tx - dx)
            ax = Vector((0.0, 1.0, 0.0))
            p.cylinder(M.lamp_h, (cx, TAIL - 0.01, tz), 0.062, 0.03, segs=24, axis='Y')
            p.cylinder(mat, (cx, TAIL + 0.012, tz), 0.052, 0.012, segs=24, axis='Y')
# FIA rain light
carlib.led_grid(p, M, -0.08, 0.08, VAL_Z[1] + 0.04, VAL_Z[1] + 0.10, TAIL + 0.004, dir_y=-1.0,
                cols=4, rows=2, glow=M.rain)

# ---- mud flaps: rubber behind each wheel, the rally car's signature
for (y, tyre) in ((AX_F, TYRE_F), (AX_R, TYRE_R)):
    fy = y + tyre + 0.07
    for sx in (-1, 1):
        x0, x1 = sorted((sx * (HUB_X - 0.15), sx * (HUB_X + 0.11)))
        p.box(M.accent, (x0, fy, 0.045), (x1, fy + 0.010, max(0.30, V["floor"] + 0.16)))

# ---- side intake floors
for f in SIDE_FLOORS:
    for sx in (-1, 1):
        carlib.swept_floor(p, M.mesh, L, f, sx=sx)
if "deck" in V["vents"]:
    y0 = V["rear_glass_y"][1] + 0.16
    for k in range(7):
        yv = y0 + k * 0.088
        zt = L.roof_z(yv) - 0.026
        p.box(M.trim, (-0.36 * XS, yv, zt - 0.010), (0.36 * XS, yv + 0.040, zt + 0.012))
if "bonnet" in V["vents"]:
    y0 = V["screen_y"][0] - 0.60
    for k in range(4):
        yv = y0 + k * 0.070
        zt = L.roof_z(yv) - 0.022
        p.box(M.trim, (-0.30 * XS, yv, zt - 0.010), (0.30 * XS, yv + 0.032, zt + 0.010))

# ---- mirrors, scoop, exhausts, aerial, wiper, hooks, bonnet pins
MIR_Y = V["screen_y"][0] + 0.30
MIR_Z = L.point(MIR_Y, 5.0).z - 0.010
MIRRORS = {}
for sx, key in ((1, "left"), (-1, "right")):
    MIRRORS[key] = carlib.mirror(p, M, L.x_at(MIR_Y, MIR_Z) + 0.115, MIR_Y, MIR_Z, sx=sx, style="pod",
                                 glass=MIR[key], size=(0.17, 0.080, 0.11))
if V["scoop"]:
    sx_, sy_, sr_ = V["scoop"]
    zt = L.roof_z(sy_)
    sc_prof = [(sy_ - 0.26, zt - 0.02), (sy_ + 0.22, zt - 0.02), (sy_ + 0.18, zt + sr_),
               (sy_ - 0.16, zt + sr_ * 0.92)]
    carlib.plate(p, M.accent, sc_prof, sx_, 0.34, chamfer=0.025)
    p.box(M.mesh, (sx_ - 0.135, sy_ - 0.275, zt + 0.006), (sx_ + 0.135, sy_ - 0.25, zt + sr_ - 0.008))
EXH_Z = (VAL_Z[0] + VAL_Z[1]) / 2
if V["exhaust"] == "centre":
    p.cylinder(M.metal, (0.0, TAILP_Y - 0.12, EXH_Z), 0.050, 0.15, segs=16, axis='Y')
elif V["exhaust"] == "twin":
    for x in (-0.14, 0.14):
        p.cylinder(M.metal, (x, TAILP_Y - 0.12, EXH_Z), 0.040, 0.15, segs=16, axis='Y')
else:
    # the Oudi's exhaust exits the flank ahead of the rear wheel
    p.cylinder(M.metal, (-(SILL_X + 0.005), AX_R - TYRE_R - 0.16, 0.20), 0.045, -0.07, segs=16, axis='X')
AER_Y = V["screen_y"][1] + 0.45
p.bar(M.trim, (0.0, AER_Y, L.roof_z(AER_Y) - 0.01), (0.0, AER_Y, L.roof_z(AER_Y) + 0.16), 0.004)
for yy in (NOSE + 0.08, TAIL - 0.08):
    p.torus(M.towhook, (0.30 * XS, yy, 0.26), 0.050, 0.013, segs=16, rings=8)
wy = V["screen_y"][0] - 0.02
p.bar(M.trim, (DX - 0.42, wy, L.z_at(wy, 0.30) + 0.010),
      (DX + 0.24, wy + 0.03, L.z_at(wy + 0.03, 0.30) + 0.010), 0.008, segs=8)
UPPER_SHUTS = sorted(y for (y, k) in V["shutlines"] if k == "upper")
for x in (-0.34 * XS, 0.34 * XS):
    py = UPPER_SHUTS[0] + 0.06
    pz = L.z_at(py, abs(x))
    p.cylinder(M.metal, (x, py, pz - 0.004), 0.016, 0.010, segs=12, axis='Z')
    p.cylinder(M.trim, (x, py, pz + 0.006), 0.007, 0.006, segs=8, axis='Z')

# ---- cockpit (the GT3 build's, at this car's width)
XIN = min(L.x_at(y, z) for y in (-0.40, 0.0, 0.30) for z in (DASH_Z, L.point(0.0, 5.0).z - 0.05)) - 0.055
ARCH_END = AX_F + TYRE_F + ARCH_GAP + 0.04
X_WELL = X_IN - 0.07


def ibox(mat, lo, hi):
    if lo[1] < ARCH_END:
        y1 = min(hi[1], ARCH_END)
        p.box(mat, (max(lo[0], -X_WELL), lo[1], lo[2]), (min(hi[0], X_WELL), y1, hi[2]))
        if hi[1] > ARCH_END:
            p.box(mat, (lo[0], ARCH_END, lo[2]), hi)
    else:
        p.box(mat, lo, hi)


FL = V["floor"]
BELT_Z = L.point(0.0, 5.0).z
SEAT_Y0 = EYE.y - 0.48
HOOP_Y = SEAT_Y0 + 0.84
BULK_Y = HOOP_Y + 0.03
SIDE_SHUTS = sorted(y for (y, k) in V["shutlines"] if k == "side")
DOOR0, DOOR1 = SIDE_SHUTS[0] + 0.03, SIDE_SHUTS[-1] - 0.03
FLOOR_Y0 = min(DOOR0 - 0.25, EYE.y - 1.40)
ibox(M.interior, (-XIN, FLOOR_Y0, FL - 0.005), (XIN, BULK_Y, FL + 0.035))
ibox(M.alcantara, (-XIN + 0.02, FLOOR_Y0 + 0.20, FL + 0.035), (XIN - 0.02, SEAT_Y0 - 0.02, FL + 0.040))
p.box(M.interior, (-0.16, FLOOR_Y0, FL + 0.035), (0.16, BULK_Y, FL + 0.20))
p.box(M.trim, (-0.18, FLOOR_Y0, FL + 0.20), (0.18, BULK_Y, FL + 0.215))
carlib.switch_panel(p, M, -0.14, 0.14, CK["wheel"].y - 0.20, CK["wheel"].y + 0.22, FL + 0.225,
                    rows=2, cols=4, rotary=True)
p.box(M.interior, (-XIN, BULK_Y, FL + 0.035), (XIN, BULK_Y + 0.03, BELT_Z - 0.20))
DASH_Z = EYE.z - 0.24
DASH_Y0, DASH_Y1 = max(FLOOR_Y0, V["screen_y"][0] + 0.06), CK["wheel"].y - 0.28
ibox(M.interior, (-XIN, DASH_Y0, DASH_Z - 0.30), (XIN, DASH_Y1, DASH_Z - 0.05))
ibox(M.alcantara, (-XIN, DASH_Y0, DASH_Z - 0.05), (XIN, DASH_Y1, DASH_Z))
p.bar(M.alcantara, (-XIN, DASH_Y1, DASH_Z - 0.016), (XIN, DASH_Y1, DASH_Z - 0.016), 0.016, segs=10)
# a binnacle of round gauges ahead of the driver, the co-driver's trip
# meter on the other side
hood = [(DASH_Y1 - 0.26, DASH_Z), (DASH_Y1 + 0.02, DASH_Z), (DASH_Y1 + 0.02, DASH_Z + 0.07),
        (DASH_Y1 - 0.20, DASH_Z + 0.07)]
carlib.plate(p, M.alcantara, hood, DX, 0.30, chamfer=0.012)
for k, gx in enumerate((-0.09, 0.0, 0.09)):
    p.cylinder(M.display, (DX + gx, DASH_Y1 + 0.004, DASH_Z + 0.035), 0.030, 0.006, segs=20, axis='Y')
p.box(M.trim, (-XIN + 0.10, DASH_Y1 - 0.04, DASH_Z), (-XIN + 0.32, DASH_Y1 + 0.005, DASH_Z + 0.06))
p.box(M.display, (-XIN + 0.12, DASH_Y1 + 0.004, DASH_Z + 0.012), (-XIN + 0.30, DASH_Y1 + 0.008, DASH_Z + 0.048))
CARD_Y0 = DOOR0
while CARD_Y0 < DOOR0 + 0.6 and (L.z_at(CARD_Y0, XIN + 0.02) < BELT_Z
                                  or L.x_at(CARD_Y0, BELT_Z - 0.02) - 0.025 < XIN):
    CARD_Y0 += 0.02
for sx in (-1, 1):
    carlib.door_card(p, M, sx, XIN, CARD_Y0, DOOR1, FL + 0.08, BELT_Z - 0.035, pull=True)
# both buckets: the co-driver rides in a rally car
carlib.bucket_seat(p, M, DX, SEAT_Y0, FL + 0.035, width=0.48, depth=0.50, back_h=0.64, rake_deg=20.0)
carlib.bucket_seat(p, M, -DX, SEAT_Y0 + 0.08, FL + 0.035, width=0.48, depth=0.50, back_h=0.64, rake_deg=20.0)
PED_Y = EYE.y - 1.12
for x in (DX - 0.20, DX - 0.08, DX + 0.04):
    p.box(M.metal, (x - 0.032, PED_Y - 0.06, FL + 0.045), (x + 0.032, PED_Y, FL + 0.18))
carlib.extinguisher(p, M, -(XIN - 0.10), SEAT_Y0 - 0.38, FL + 0.10)
# the handbrake lever by the driver's right hand
p.bar(M.metal, (DX - 0.27, SEAT_Y0 + 0.05, FL + 0.22), (DX - 0.27, SEAT_Y0 - 0.12, FL + 0.46), 0.012, segs=8)
carlib.inner_skin(p, M.alcantara, L, V["screen_y"][1] + 0.03, V["rear_glass_y"][0] - 0.03, 0.52 * XS,
                  drop=0.030, nu=10, nv=6)


def inside(y, x, drop=0.055, pull=0.055):
    return (x, y, L.z_at(y, abs(x) + pull) - drop)


SCR_Y = V["screen_y"][1]
PY = V["screen_y"][0] + 0.06
CX = XIN - 0.06
for sx in (-1, 1):
    hoop_top = inside(HOOP_Y, sx * 0.52 * XS)
    rail = inside(SCR_Y, sx * 0.58 * XS)
    foot = (sx * (L.x_at(PY, 0.72) - 0.075), PY, L.z_at(PY, 0.70 * XS) - 0.045)
    carlib.inside_bar(p, M.cage, L, (sx * CX, HOOP_Y, FL + 0.06), (sx * CX, HOOP_Y, hoop_top[2] - 0.10), 0.022, segs=8)
    carlib.inside_bar(p, M.cage, L, (sx * CX, HOOP_Y, hoop_top[2] - 0.10), hoop_top, 0.022, segs=8)
    carlib.inside_bar(p, M.cage, L, hoop_top, rail, 0.020, segs=8)
    mid_y = (SCR_Y + PY) / 2
    mid = (sx * (L.x_at(mid_y, 0.86) - 0.070), mid_y, L.z_at(mid_y, 0.62 * XS) - 0.050)
    carlib.inside_bar(p, M.cage, L, rail, mid, 0.018, segs=8)
    carlib.inside_bar(p, M.cage, L, mid, foot, 0.018, segs=8)
    carlib.inside_bar(p, M.cage, L, (sx * CX, HOOP_Y, FL + 0.42), (sx * (CX - 0.02), DOOR0 + 0.05, FL + 0.46),
                      0.019, segs=8)
carlib.inside_bar(p, M.cage, L, inside(HOOP_Y, -0.52 * XS), inside(HOOP_Y, 0.52 * XS), 0.022, segs=8)

# ---- the driver (and his helmet in the works colour)
drv = Builder("driver")
carlib.driver_figure(drv, M, EYE, CK["wheel"], (DX, SEAT_Y0 + 0.35, FL + 0.035 + 0.22), (PED_Y, FL + 0.08),
                     wheel_hw=0.17, suit=(0.80, 0.80, 0.78))

parts = carlib.bevel(p.finish(planar_uv=True, recalc=False), width=0.004, segments=2, angle_deg=38.0)
parts_objs.append(parts)
save("parts")


# ------------------------------------------------------------ skin + export
def project_skin(ob):
    """Lay every `car_skin` face on its region of the skin (skin_layout)."""
    me = ob.data
    idx = {i for i, m in enumerate(me.materials) if m and m.name.split(".")[0] == "car_skin"}
    uvl = me.uv_layers.active or me.uv_layers.new(name="UVMap")
    n = 0
    for poly in me.polygons:
        if poly.material_index not in idx:
            continue
        nrm, ctr = tuple(poly.normal), tuple(poly.center)
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = skin_layout.uv(nrm, ctr, tuple(co), SKIN_BOX)
        n += 1
    return n


EXPORT = os.environ.get("APEX_EXPORT", "1") == "1"
car, _ = carlib.join_and_export([body] + parts_objs, V["stem"], CAR_DIR, export=False)
print("skin faces:", project_skin(car))
glb = carlib.export_glb(car, os.path.join(CAR_DIR, V["stem"] + ".glb")) if EXPORT else None
DRIVER = carlib.export_driver(drv, V["stem"], CAR_DIR, export=bool(glb))
save("joined")
print("stats:", carlib.mesh_stats(car))
print("sightline:", carlib.sightline(car, eye=EYE))
if glb:
    carlib.write_cockpit_table(CAR_DIR, CK, 270.0, mirrors=MIRRORS,
                               note="Rally: the seat where the car put it, a co-driver beside it.")
    print("GLB:", glb, os.path.getsize(glb))
