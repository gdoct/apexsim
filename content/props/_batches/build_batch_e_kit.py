r"""Batch E - kit rules for the real-layout dossiers (docs/PROPS.md step 2):

  board/corner_sign            named-corner board on two posts (`board_text` face)
  attraction/food_stall_6m     concession unit, serving hatch to the road, brand fascia
  attraction/ticket_gate       turnstile row under a WELCOME arch
  vehicle/camper_van           high-roof camper
  vehicle/coach                12 m bus
  vehicle/safety_car           estate with light bar (`safety_lightbar` emissive)
  vehicle/medical_car          same body, orange
  building/podium              pit-roof podium + backdrop (floor at 9.95 m, over garage_6m)
  misc/tyre_stack              loose tyre stacks
  misc/gate_4m                 steel field gate
  sign/hillside_letters        big standing letters, TEXT-driven (default "RED BULL RING")

    ASSET = "all"          # or one key of BUILD
    TEXT = "RED BULL RING" # hillside_letters
    exec(open(r"D:\apexsim\content\props\_batches\build_batch_e_kit.py").read())
"""
import bpy, math, os, importlib.util, sys, random
from mathutils import Vector

_ROOT = "D:\\apexsim"
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "content\\props\\_tools\\apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
_t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "content\\props\\_tools\\apex_tex.py"))
tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
B, M = apex.Builder, tex.kit_material   # baked slot where the kit has one, flat otherwise
PR = apex.PROPS_ROOT

try:
    ASSET
except NameError:
    ASSET = "all"
try:
    TEXT
except NameError:
    TEXT = "RED BULL RING"

# shared slots (names already known to the importer where they exist)
def mats():
    return dict(
        post=M("board_post", (0.35, 0.36, 0.38), metallic=0.6, roughness=0.5),
        back=M("board_back", (0.25, 0.26, 0.27), roughness=0.7),
        text=M("board_text", (0.96, 0.96, 0.94), roughness=0.4),
        brand=apex.image_material("board_brand", os.path.join(PR, "board", "brands", "apexsim.png")),
        steel=M("bridge_steel", (0.55, 0.56, 0.58), metallic=0.8, roughness=0.4),
        dark=M("bridge_dark", (0.12, 0.12, 0.13), roughness=0.6),
        galv=M("armco_galv", (0.62, 0.64, 0.66), metallic=0.7, roughness=0.45),
        glass=M("pit_glass", (0.05, 0.08, 0.1), metallic=0.2, roughness=0.15),
        tyre=M("vehicle_tyre", (0.05, 0.05, 0.05), roughness=0.9),
        rubber=M("tire_rubber", (0.06, 0.06, 0.06), roughness=0.85),
        concrete=M("pit_concrete", (0.6, 0.6, 0.58), roughness=0.85),
        plank=M("scaffold_plank", (0.55, 0.42, 0.25), roughness=0.8),
        white=M("tent_white", (0.92, 0.92, 0.9), roughness=0.6),
        colour=M("tent_colour", (0.8, 0.12, 0.1), roughness=0.6),
        lamp=M("floodlight_lamp", (0.9, 0.9, 0.85), emission=(1, 0.95, 0.8)),
    )


def wheels(b, m, xs, y_half, r=0.35, w=0.22, z=None):
    z = r if z is None else z
    for x in xs:
        for s in (-1, 1):
            b.cylinder(m["tyre"], (x, (y_half - w) if s > 0 else -y_half, z), r, w, segs=12, axis='Y')


# ------------------------------------------------------------ board/corner_sign
def corner_sign():
    m = mats()
    b = B("corner_sign")
    W, H, Z0 = 2.4, 0.8, 1.8   # board width, height, bottom edge height
    for x in (-0.9, 0.9):
        b.box(m["post"], (x - 0.04, -0.04, 0), (x + 0.04, 0.04, Z0 + H - 0.05))
    b.box(m["back"], (-W / 2, -0.02, Z0), (W / 2, 0.06, Z0 + H))
    b.front_quad(m["text"], -W / 2, W / 2, Z0, Z0 + H, -0.021)     # face to the road (-Y)
    # thin white frame band
    b.box(m["text"], (-W / 2 - 0.02, -0.03, Z0 - 0.02), (W / 2 + 0.02, -0.02, Z0))
    b.box(m["text"], (-W / 2 - 0.02, -0.03, Z0 + H), (W / 2 + 0.02, -0.02, Z0 + H + 0.02))
    return b.finish()


# ------------------------------------------------------- attraction/food_stall_6m
def food_stall_6m():
    """6 x 3 m trailer-style concession. Pivot on the road-facing edge (y = 0),
    body reaches +Y. Serving hatch and awning on -Y."""
    m = mats()
    b = B("food_stall_6m")
    L, D, H = 6.0, 3.0, 3.2
    z0 = 0.45   # chassis height
    b.box(m["white"], (-L / 2, 0.0, z0), (L / 2, D, H))                    # body
    b.box(m["dark"], (-L / 2 + 0.2, 0.3, 0.0), (L / 2 - 0.2, D - 0.3, z0))  # chassis skirt
    for x in (-1.8, 1.8):
        b.cylinder(m["tyre"], (x, 0.55, 0.32), 0.32, 0.25, segs=10, axis='Y')
        b.cylinder(m["tyre"], (x, D - 0.8, 0.32), 0.32, 0.25, segs=10, axis='Y')
    # serving hatch: dark recess + counter
    b.box(m["dark"], (-2.3, -0.02, z0 + 0.9), (2.3, 0.15, H - 0.7))
    b.box(m["plank"], (-2.4, -0.35, z0 + 0.85), (2.4, 0.02, z0 + 0.95))
    # awning: striped colour sheet propped on two arms
    b.sheet(m["colour"], (-3.1, -0.02, H - 0.1), (3.1, -0.02, H - 0.1), (3.1, -1.6, H - 0.6), (-3.1, -1.6, H - 0.6))
    b.sheet(m["colour"], (-3.1, -1.6, H - 0.6), (3.1, -1.6, H - 0.6), (3.1, -0.02, H - 0.1), (-3.1, -0.02, H - 0.1))
    for x in (-2.9, 2.9):
        b.bar(m["steel"], (x, -0.02, H - 1.1), (x, -1.5, H - 0.62), 0.02)
    # brand fascia above the hatch, 3:1 logo twice
    b.quad_uv(m["brand"], [(-2.7, -0.03, H - 0.05), (-0.1, -0.03, H - 0.05), (-0.1, -0.03, H - 0.9), (-2.7, -0.03, H - 0.9)],
              [(0, 1), (1, 1), (1, 0), (0, 0)])
    b.quad_uv(m["brand"], [(0.1, -0.03, H - 0.05), (2.7, -0.03, H - 0.05), (2.7, -0.03, H - 0.9), (0.1, -0.03, H - 0.9)],
              [(0, 1), (1, 1), (1, 0), (0, 0)])
    # roof kit: extractor + AC box
    b.box(m["steel"], (1.5, 1.0, H), (2.3, 1.8, H + 0.45))
    b.cylinder(m["steel"], (-1.8, 1.6, H), 0.18, 0.7, segs=8)
    # rear door
    b.box(m["dark"], (L / 2 - 0.02, 0.6, z0 + 0.05), (L / 2 + 0.01, 1.4, z0 + 2.0))
    return b.finish()


# ---------------------------------------------------------- attraction/ticket_gate
def ticket_gate():
    """Four tripod turnstiles between galvanised rails, under a 6 m arch
    carrying WELCOME. Pivot on the road-facing edge; people pass along Y."""
    m = mats()
    b = B("ticket_gate")
    W = 6.0
    n = 4
    pitch = W / (n + 1)
    # side rails and lane dividers
    for i in range(n + 1):
        x = -W / 2 + pitch * (i + 0.5)
        for y0, y1 in ((0.2, 1.8),):
            b.box(m["galv"], (x - 0.03, y0, 0.0), (x + 0.03, y1, 1.05))
            b.bar(m["galv"], (x, y0, 1.05), (x, y1, 1.05), 0.03)
            b.bar(m["galv"], (x, y0, 0.5), (x, y1, 0.5), 0.02)
    # turnstiles: cabinet + tripod arms
    for i in range(n):
        x = -W / 2 + pitch * (i + 1)
        b.box(m["steel"], (x - 0.18, 0.7, 0.0), (x + 0.18, 1.3, 0.95))
        b.box(m["dark"], (x - 0.15, 0.68, 0.9), (x + 0.15, 1.32, 1.0))
        hub = (x, 1.0, 0.98)
        for k in range(3):
            a = 2 * math.pi * k / 3
            b.bar(m["galv"], hub, (x, 1.0 + 0.45 * math.cos(a), 0.98 + 0.45 * math.sin(a)), 0.02)
    # arch: two box columns + header beam + fascia
    for x in (-W / 2 - 0.3, W / 2 + 0.3):
        b.box(m["steel"], (x - 0.2, 0.8, 0.0), (x + 0.2, 1.2, 4.2))
    b.box(m["steel"], (-W / 2 - 0.5, 0.8, 4.2), (W / 2 + 0.5, 1.2, 4.6))
    b.box(m["colour"], (-W / 2 - 0.5, 0.75, 3.4), (W / 2 + 0.5, 1.25, 4.2))
    me = apex.text_mesh("welcome", "WELCOME", size=0.55, extrude=0.08)
    apex.merge_mesh(b, m["white"], me, offset=(0, 0.72, 3.62))
    # brand boards on the columns
    for x in (-W / 2 - 0.3, W / 2 + 0.3):
        b.quad_uv(m["brand"], [(x - 0.2, 0.79, 3.3), (x + 0.2, 0.79, 3.3), (x + 0.2, 0.79, 2.1), (x - 0.2, 0.79, 2.1)],
                  [(0, 1), (0.33, 1), (0.33, 0), (0, 0)])
    return b.finish()


# ------------------------------------------------------------------- vehicles
def paint(name, rgb):
    return M(name, rgb, metallic=0.3, roughness=0.35)


def van_body(b, m, name, L, W, H, cab, roof_rgb=None, windows=True, wheel_xs=None):
    """Boxy van/coach shell centred on the footprint: lower body, glass band,
    wheels. `cab` is the length of the cab section (windscreen slope)."""
    pnt = paint(name, roof_rgb or (0.85, 0.85, 0.83))
    z0 = 0.45
    # lofted side profile along X (y half-width, z)
    secs = []
    xs = [-L / 2, -L / 2 + 0.15, L / 2 - cab, L / 2 - cab * 0.35, L / 2 - 0.1, L / 2]
    hs = [H - 0.1, H, H, H * 0.62, H * 0.45, z0 + 0.2]
    for x, h in zip(xs, hs):
        w = W / 2 - (0.06 if h < H else 0.0)
        secs.append([(x, -w, z0), (x, w, z0), (x, w * 0.92, h), (x, -w * 0.92, h)])
    b.loft(pnt, secs, closed=True, cap_ends=True)
    wheels(b, m, wheel_xs or (-L / 2 + 1.2, L / 2 - 1.6), W / 2, r=0.38 if not wheel_xs else 0.5, w=0.25 if not wheel_xs else 0.3)
    b.box(m["dark"], (-L / 2 + 0.6, -W / 2 + 0.1, 0.35), (L / 2 - 0.6, W / 2 - 0.1, z0 + 0.02))
    if windows:
        # side windows band + windscreen
        for s in (-1, 1):
            b.box(m["glass"], (-L / 2 + 0.4, s * (W / 2 * 0.925) - 0.005 * s, z0 + 1.0), (L / 2 - cab - 0.2, s * (W / 2 * 0.925) + 0.005 * s, H - 0.35))
        x0, x1 = L / 2 - cab * 0.35, L / 2 - 0.1
        b.sheet(m["glass"], (x0 + 0.01, -W * 0.44, H * 0.62 - 0.05), (x0 + 0.01, W * 0.44, H * 0.62 - 0.05),
                (x1 + 0.03, W * 0.44, H * 0.45 + 0.35), (x1 + 0.03, -W * 0.44, H * 0.45 + 0.35))
    return pnt


def camper_van():
    m = mats()
    b = B("camper_van")
    van_body(b, m, "vehicle_paint_a", 6.0, 2.1, 2.55, 1.6, roof_rgb=(0.88, 0.88, 0.86))
    # high roof pod + roof rack + awning rail
    b.box(M("vehicle_paint_a", (0.88, 0.88, 0.86)), (-2.9, -0.95, 2.5), (1.4, 0.95, 3.0))
    b.box(m["dark"], (-2.8, -0.9, 2.55), (1.3, 0.9, 2.62))
    b.box(m["white"], (-2.6, 1.02, 2.4), (1.2, 1.12, 2.55))          # rolled awning
    b.box(m["plank"], (-2.2, -0.3, 2.99), (0.8, 0.3, 3.05))          # roof box
    return b.finish()


def coach():
    m = mats()
    b = B("coach")
    van_body(b, m, "vehicle_paint_b", 12.0, 2.55, 3.7, 1.0, roof_rgb=(0.2, 0.28, 0.55), wheel_xs=(-3.6, -2.3, 4.2))
    # luggage doors + roof AC
    for x in (-3.0, 0.5):
        b.box(m["dark"], (x, -1.28, 0.55), (x + 2.2, -1.265, 1.2))
    b.box(m["steel"], (-1.5, -0.6, 3.68), (1.5, 0.6, 3.95))
    b.box(m["glass"], (-6.01, -0.9, 1.5), (-5.985, 0.9, 3.2))    # rear window
    return b.finish()


def car_body(b, m, name, rgb, L=4.8, W=1.85, H=1.45):
    """Estate-shaped saloon: lofted along X."""
    pnt = paint(name, rgb)
    z0 = 0.25
    secs = []
    prof = [(-L / 2, z0 + 0.55, 1.0), (-L / 2 + 0.3, H - 0.05, 0.95), (-L / 2 + 1.0, H, 0.85), (L / 2 - 2.0, H, 0.85),
            (L / 2 - 1.4, H - 0.4, 0.95), (L / 2 - 0.3, H - 0.55, 1.0), (L / 2, z0 + 0.45, 1.0)]
    for x, h, k in prof:
        w = W / 2
        secs.append([(x, -w, z0), (x, w, z0), (x, w * k, h), (x, -w * k, h)])
    b.loft(pnt, secs, closed=True, cap_ends=True)
    wheels(b, m, (-L / 2 + 0.9, L / 2 - 0.9), W / 2, r=0.33, w=0.22)
    b.box(m["dark"], (-L / 2 + 0.3, -W / 2 + 0.08, 0.2), (L / 2 - 0.3, W / 2 - 0.08, z0 + 0.02))
    # glass band
    for s in (-1, 1):
        b.box(m["glass"], (-L / 2 + 0.5, s * (W / 2 * 0.86) - 0.004 * s, H - 0.42), (L / 2 - 1.6, s * (W / 2 * 0.86) + 0.004 * s, H - 0.05))
    b.sheet(m["glass"], (L / 2 - 1.95, -W * 0.42, H - 0.02), (L / 2 - 1.95, W * 0.42, H - 0.02),
            (L / 2 - 1.35, W * 0.44, H - 0.4), (L / 2 - 1.35, -W * 0.44, H - 0.4))
    b.sheet(m["glass"], (-L / 2 + 0.28, W * 0.42, H - 0.06), (-L / 2 + 0.28, -W * 0.42, H - 0.06),
            (-L / 2 - 0.02, -W * 0.44, z0 + 0.58), (-L / 2 - 0.02, W * 0.44, z0 + 0.58))
    # light bar
    bar = M("safety_lightbar", (0.95, 0.55, 0.05), emission=(1.0, 0.45, 0.05))
    b.box(m["dark"], (-0.5, -0.55, H), (0.5, 0.55, H + 0.05))
    b.box(bar, (-0.45, -0.5, H + 0.05), (0.45, 0.5, H + 0.17))
    return pnt


def safety_car():
    m = mats()
    b = B("safety_car")
    car_body(b, m, "vehicle_paint_safety", (0.78, 0.79, 0.8))
    me = apex.text_mesh("sc", "SAFETY CAR", size=0.16, extrude=0.01)
    apex.merge_mesh(b, m["dark"], me, offset=(0.4, -0.94, 0.6))
    return b.finish()


def medical_car():
    m = mats()
    b = B("medical_car")
    car_body(b, m, "vehicle_paint_medical", (0.95, 0.42, 0.05))
    me = apex.text_mesh("mc", "MEDICAL CAR", size=0.16, extrude=0.01)
    apex.merge_mesh(b, m["white"], me, offset=(0.4, -0.94, 0.6))
    return b.finish()


# ------------------------------------------------------------- building/podium
def podium():
    """Three-step podium with a branded backdrop, floored at 9.95 m so it sits on
    a garage_6m roof (9.9 m). Pivot on the road-facing edge at ground level;
    four slim columns hide inside the garage."""
    m = mats()
    b = B("podium")
    Z = 9.95
    L, D = 12.0, 4.5
    for x, y in ((-5.5, 0.5), (5.5, 0.5), (-5.5, D - 0.5), (5.5, D - 0.5)):
        b.box(m["steel"], (x - 0.1, y - 0.1, 0), (x + 0.1, y + 0.1, Z))
    b.box(m["concrete"], (-L / 2, 0, Z - 0.05), (L / 2, D, Z))            # deck
    # steps: P2 left, P1 centre, P3 right
    for x0, x1, h in ((-2.7, -1.0, 0.6), (-0.9, 0.9, 0.9), (1.0, 2.7, 0.45)):
        b.box(m["colour"], (x0, 0.6, Z), (x1, 2.2, Z + h))
        b.box(m["white"], (x0 + 0.05, 0.62, Z + h), (x1 - 0.05, 2.18, Z + h + 0.02))
    # backdrop board 12 x 4, brand logo tiled
    b.box(m["dark"], (-L / 2 + 0.2, D - 0.5, Z), (L / 2 - 0.2, D - 0.3, Z + 4.0))
    for i in range(3):
        x0 = -L / 2 + 0.3 + i * 3.8
        b.quad_uv(m["brand"], [(x0, D - 0.51, Z + 3.7), (x0 + 3.6, D - 0.51, Z + 3.7), (x0 + 3.6, D - 0.51, Z + 2.5), (x0, D - 0.51, Z + 2.5)],
                  [(0, 1), (1, 1), (1, 0), (0, 0)])
    b.box(m["white"], (-L / 2 + 0.3, D - 0.52, Z + 0.3), (L / 2 - 0.3, D - 0.5, Z + 2.3))
    # front rail
    b.bar(m["galv"], (-L / 2, 0.1, Z + 1.1), (L / 2, 0.1, Z + 1.1), 0.03)
    for x in range(-6, 7, 2):
        b.bar(m["galv"], (x, 0.1, Z), (x, 0.1, Z + 1.1), 0.02)
    return b.finish()


# -------------------------------------------------------------- misc/tyre_stack
def tyre_stack():
    m = mats()
    b = B("tyre_stack")
    worn = M("tire_rubber_worn", (0.1, 0.1, 0.1), roughness=0.75)
    rng = random.Random(7)
    def tyre(c, mat, tilt=0.0):
        b.torus(mat, c, 0.24, 0.1, segs=10, rings=6)
    for k in range(4):
        tyre((0.0, 0.0, 0.1 + 0.2 * k), m["rubber"] if k % 2 else worn)
    for k in range(3):
        tyre((0.62, 0.15, 0.1 + 0.2 * k), worn if k % 2 else m["rubber"])
    for k in range(2):
        tyre((-0.55, -0.3, 0.1 + 0.2 * k), m["rubber"])
    # one leaning against the tall stack
    b.torus(m["rubber"], (0.3, -0.42, 0.24), 0.24, 0.1, segs=10, rings=6)
    return b.finish()


# ----------------------------------------------------------------- misc/gate_4m
def gate_4m():
    m = mats()
    b = B("gate_4m")
    steel = M("gate_steel", (0.5, 0.52, 0.5), metallic=0.7, roughness=0.5)
    for x in (-2.1, 2.1):
        b.cylinder(m["galv"], (x, 0, 0), 0.06, 1.4, segs=8)
        b.cone(m["galv"], (x, 0, 1.4), 0.07, 0.0, 0.06, segs=8, cap=False)
    # frame
    b.bar(steel, (-2.0, 0, 1.15), (2.0, 0, 1.15), 0.025)
    b.bar(steel, (-2.0, 0, 0.3), (2.0, 0, 0.3), 0.025)
    for x in (-2.0, 2.0):
        b.bar(steel, (x, 0, 0.3), (x, 0, 1.15), 0.025)
    for z in (0.5, 0.72, 0.94):
        b.bar(steel, (-2.0, 0, z), (2.0, 0, z), 0.015)
    b.bar(steel, (-2.0, 0, 0.3), (2.0, 0, 1.15), 0.015)
    return b.finish()


# ----------------------------------------------------------- sign/hillside_letters
def hillside_letters():
    """Free-standing letters (6 m tall, 0.8 m deep) on short steel legs, read
    from the road (-Y). Rebuild with TEXT = "..." for another circuit."""
    m = mats()
    b = B("hillside_letters")
    white = M("letters_white", (0.95, 0.95, 0.93), roughness=0.5)
    me = apex.text_mesh("hill", TEXT, size=6.0, extrude=0.8)
    apex.merge_mesh(b, white, me, offset=(0, 0, 0.6))
    # legs every ~3 m under the text span
    xmin = min(v.co.x for v in bpy.data.meshes[b.name].vertices) if b.name in bpy.data.meshes else None
    n = max(2, int(len(TEXT) * 6 * 0.6 / 3))
    span = len(TEXT) * 6 * 0.6
    for i in range(n + 1):
        x = -span / 2 + span * i / n
        b.box(m["galv"], (x - 0.08, -0.35, 0), (x + 0.08, 0.35, 0.62))
    return b.finish()


BUILD = {
    "board/corner_sign": corner_sign,
    "attraction/food_stall_6m": food_stall_6m,
    "attraction/ticket_gate": ticket_gate,
    "vehicle/camper_van": camper_van,
    "vehicle/coach": coach,
    "vehicle/safety_car": safety_car,
    "vehicle/medical_car": medical_car,
    "building/podium": podium,
    "misc/tyre_stack": tyre_stack,
    "misc/gate_4m": gate_4m,
    "sign/hillside_letters": hillside_letters,
}
keys = list(BUILD) if ASSET == "all" else [k for k in BUILD if k.endswith("/" + ASSET) or k == ASSET]
apex.reset_scene()
tex.reset_cache()
exported = {}
for k in keys:
    BUILD[k]()
    kind, asset = k.split("/")
    exported[k] = apex.export_one(kind, asset)
result = {"exported": exported, "stats": apex.stats()}
