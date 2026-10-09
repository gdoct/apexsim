r"""Marina Bay batch C1 - street furniture, crossings and trees the street circuit needs (MARINA_BAY.md s5).

    ASSET = "all"   # or one of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_c1.py").read())
Thin modules are centred on their footprint with the road on -Y (PROPS.md Conventions).
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())
ct = _load("card_trees", "card_trees.py")
try:
    ASSET
except NameError:
    ASSET = "all"

PROPS_DIR = os.path.join(_ROOT, "content", "props")


def cmat():
    m = dict(mb_mats())
    M = tex.kit_material
    m["galv"] = M("armco_galv", (0.62, 0.64, 0.66), metallic=0.7, roughness=0.45)
    m["post"] = M("armco_post", (0.55, 0.57, 0.58), metallic=0.7, roughness=0.5)
    m["sign_green"] = apex.material("mb_sign_green", (0.02, 0.28, 0.14), roughness=0.5)
    m["sign_blue"] = apex.material("mb_sign_blue", (0.03, 0.12, 0.45), roughness=0.5)
    m["sign_white"] = apex.material("mb_sign_white", (0.92, 0.93, 0.92), roughness=0.4)
    m["black"] = apex.material("mb_black", (0.03, 0.03, 0.035), roughness=0.5)
    m["yellow"] = apex.material("mb_yellow", (0.9, 0.7, 0.05), roughness=0.5)
    m["sig_red"] = apex.material("signal_red", (0.35, 0.02, 0.02), roughness=0.4, emission=(1.0, 0.05, 0.03))
    m["sig_amber"] = apex.material("signal_amber", (0.35, 0.2, 0.02), roughness=0.4, emission=(1.0, 0.55, 0.05))
    m["sig_green"] = apex.material("signal_green", (0.02, 0.3, 0.12), roughness=0.4, emission=(0.1, 1.0, 0.35))
    m["lamp"] = M("floodlight_lamp", (0.9, 0.9, 0.85), emission=(1, 0.95, 0.8))
    m["text"] = M("board_text", (0.96, 0.96, 0.94), roughness=0.4)
    return m


def tag_emissive():
    set_night(False)


# --------------------------------------------------------- barrier/quay_rail_4m
def quay_rail_4m():
    m = cmat(); b = B("quay_rail_4m")
    b.box(m["concrete"], (-2.0, -0.16, 0), (2.0, 0.16, 0.28))                       # kerb plinth
    for x in (-1.95, 0.0, 1.95):
        b.cylinder(m["galv"], (x, 0, 0.28), 0.045, 0.95, segs=10)
        b.cylinder(m["galv"], (x, 0, 1.2), 0.06, 0.03, segs=10)
    b.cylinder(m["galv"], (-2.0, 0, 1.18), 0.04, 4.0, segs=10, axis='X')              # top rail
    for z in (0.5, 0.82):
        b.cylinder(m["galv"], (-2.0, 0, z), 0.022, 4.0, segs=8, axis='X')
    for x in np.arange(-1.75, 1.8, 0.25):
        if abs(x) > 0.05 and abs(abs(x) - 1.95) > 0.05:
            b.cylinder(m["galv"], (x, 0, 0.28), 0.012, 0.9, segs=5)
    return b.finish()


# --------------------------------------------------------- fence/catch_fence_6m
def catch_fence_6m():
    m = cmat(); b = B("catch_fence_6m")
    mesh = bpy.data.materials.get("fence_mesh")
    if mesh is None:
        bpy.ops.import_scene.gltf(filepath=os.path.join(PROPS_DIR, "barrier", "armco_4m_fence.glb"))
        for ob in list(bpy.context.scene.objects):
            if ob.name != "PreviewGround":
                bpy.data.objects.remove(ob)
        mesh = bpy.data.materials["fence_mesh"]
    post = apex.material("fence_post", (0.4, 0.42, 0.43), metallic=0.8, roughness=0.5)
    H, y = 6.0, 0.0
    b.box(m["concrete"], (-2.0, -0.2, 0), (2.0, 0.2, 0.3))
    for x in (-2.0 + 0.1, 0.0, 2.0 - 0.1):
        b.cylinder(post, (x, y, 0.3), 0.075, H - 0.3, segs=8)
        # outrigger leaning towards the road, carrying two strands
        b.bar(post, (x, y, H - 1.2), (x, y - 0.8, H + 0.45), 0.04, segs=6)
    for z in (0.9, 3.0, H - 0.05):
        b.cylinder(post, (-2.0, y, z), 0.03, 4.0, segs=6, axis='X')
    for x in (-1.0, 1.0):
        b.bar(post, (x - 0.9, y, 0.3), (x + 0.0, y, H - 0.3), 0.02, segs=4) if False else None
    top = 0.3
    b.quad_uv(mesh, [(-2.0, y - 0.09, top), (2.0, y - 0.09, top), (2.0, y - 0.09, H - 0.1), (-2.0, y - 0.09, H - 0.1)],
              [(0, 0), (4, 0), (4, H - 0.1 - top), (0, H - 0.1 - top)])
    b.quad_uv(mesh, [(-2.0, y + 0.09, top), (-2.0, y + 0.09, H - 0.1), (2.0, y + 0.09, H - 0.1), (2.0, y + 0.09, top)],
              [(0, 0), (0, H - 0.1 - top), (4, H - 0.1 - top), (4, 0)])
    for sgn in (-1, 1):                                                              # two strands up the outrigger
        for z, dy in ((H + 0.0, -0.3), (H + 0.3, -0.6)):
            b.cylinder(post, (-2.0, y + dy, z), 0.012, 4.0, segs=4, axis='X')
    return b.finish()


# --------------------------------------------------------- sign/traffic_signal_pole
def signal_head(b, m, x, y, z, face):
    """Three-aspect head; face = +1/-1 along X (the way it looks)."""
    b.box(m["black"], (x - 0.18, y - 0.16, z), (x + 0.18, y + 0.16, z + 1.05))
    for i, key in enumerate(("sig_red", "sig_amber", "sig_green")):
        zz = z + 0.85 - i * 0.33
        xs = x + face * 0.18
        b.cylinder(m[key], (xs if face > 0 else xs - 0.03, y, zz), 0.1, 0.03, segs=14, axis='X')
        b.box(m["black"], (min(x + face * 0.18, x + face * 0.34), y - 0.13, zz + 0.09), (max(x + face * 0.18, x + face * 0.34), y + 0.13, zz + 0.12))
    # back plate
    b.box(m["black"], (x - 0.03 + face * 0.03, y - 0.3, z - 0.1), (x + 0.03 + face * 0.03, y + 0.3, z + 1.15)) if False else None


def traffic_signal_pole():
    m = cmat(); b = B("traffic_signal_pole")
    py = 3.0
    b.cylinder(m["galv"], (0, py, 0), 0.22, 0.5, segs=12)
    b.cone(m["galv"], (0, py, 0.5), 0.15, 0.1, 5.9, segs=12)
    b.bar(m["galv"], (0, py, 5.5), (0, py - 3.3, 6.05), 0.07, segs=8)                # mast arm over the carriageway
    b.bar(m["galv"], (0, py, 4.7), (0, py - 1.6, 5.75), 0.04, segs=6)                # arm brace
    for yh in (-0.4, -2.2):
        signal_head(b, m, 0.0, py + yh, 4.65, -1)
        signal_head(b, m, 0.0, py + yh, 4.65, 1)
    # pedestrian head + push button on the pole, facing the road
    b.box(m["black"], (-0.22, py - 0.35, 2.2), (0.22, py - 0.15, 2.9))
    b.box(m["sig_red"], (-0.12, py - 0.36, 2.62), (0.12, py - 0.34, 2.82))
    b.box(m["sig_green"], (-0.12, py - 0.36, 2.28), (0.12, py - 0.34, 2.48))
    b.box(m["yellow"], (-0.07, py - 0.3, 1.2), (0.07, py - 0.14, 1.4))
    b.box(m["sign_green"], (-0.28, py - 0.2, 3.2), (0.28, py - 0.12, 3.55))           # street-name blade
    b.box(m["sign_white"], (-0.25, py - 0.21, 3.23), (0.25, py - 0.19, 3.52))
    return b.finish()


# --------------------------------------------------------- sign/road_sign_post
def road_sign_post():
    m = cmat(); b = B("road_sign_post")
    for x in (-0.85, 0.85):
        b.cylinder(m["galv"], (x, 0.0, 0), 0.05, 3.55, segs=8)
    W, Hh, z0 = 2.5, 1.2, 2.35
    b.box(m["sign_green"], (-W / 2, -0.04, z0), (W / 2, 0.04, z0 + Hh))
    b.box(m["sign_white"], (-W / 2 + 0.05, -0.05, z0 + 0.05), (W / 2 - 0.05, -0.04, z0 + 0.09))      # border
    b.box(m["sign_white"], (-W / 2 + 0.05, -0.05, z0 + Hh - 0.09), (W / 2 - 0.05, -0.04, z0 + Hh - 0.05))
    b.box(m["sign_white"], (-W / 2 + 0.05, -0.05, z0 + 0.05), (-W / 2 + 0.09, -0.04, z0 + Hh - 0.05))
    b.box(m["sign_white"], (W / 2 - 0.09, -0.05, z0 + 0.05), (W / 2 - 0.05, -0.04, z0 + Hh - 0.05))
    # up-arrow and two text strips
    b.box(m["sign_white"], (-0.9, -0.05, z0 + 0.25), (-0.78, -0.04, z0 + 0.95))
    tri(b, m["sign_white"], (-1.05, -0.05, z0 + 0.7), (-0.63, -0.05, z0 + 0.7), (-0.84, -0.05, z0 + 1.02), out=(0, -1, 0))
    for zz in (0.35, 0.65):
        b.box(m["sign_white"], (-0.3, -0.05, z0 + zz), (0.95, -0.04, z0 + zz + 0.14))
    b.box(m["galv"], (-0.9, 0.04, z0 + 0.1), (0.9, 0.07, z0 + 0.16))                  # back rails
    b.box(m["galv"], (-0.9, 0.04, z0 + Hh - 0.16), (0.9, 0.07, z0 + Hh - 0.1))
    return b.finish()


# --------------------------------------------------------- bridge/sign_gantry
def sign_gantry():
    m = cmat(); b = B("sign_gantry")
    ZH = 6.9
    for s in (-1, 1):
        b.box(m["galv"], (-0.3, s * 9.0 - 0.3, 0), (0.3, s * 9.0 + 0.3, ZH + 0.2))
        b.box(m["concrete"], (-0.6, s * 9.0 - 0.6, 0), (0.6, s * 9.0 + 0.6, 0.5))
    # box-truss beam
    b.truss(m["galv"], (0, -9.0, ZH + 0.1), (0, 9.0, ZH + 0.1), 0.7, 0.7, pitch=1.5, r_chord=0.07, r_diag=0.04)
    b.box(m["steel_dark"], (-0.45, -9.0, ZH - 0.35), (0.45, 9.0, ZH - 0.27))             # catwalk
    for (yc, w) in ((-4.3, 6.2), (4.3, 6.2)):
        y0, y1 = yc - w / 2, yc + w / 2
        b.box(m["sign_green"], (-0.1, y0, ZH - 2.7), (0.1, y1, ZH - 0.35))
        b.box(m["sign_white"], (-0.11, y0 + 0.06, ZH - 2.64), (-0.1, y1 - 0.06, ZH - 2.6))
        b.box(m["sign_white"], (-0.11, y0 + 0.06, ZH - 0.45), (-0.1, y1 - 0.06, ZH - 0.41))
        for zz, a, c in ((ZH - 1.0, 0.6, 3.6), (ZH - 1.6, 0.6, 2.4), (ZH - 2.2, 0.6, 3.0)):
            b.box(m["sign_white"], (-0.11, y0 + a, zz), (-0.1, y0 + a + c, zz + 0.2))
        for yy in (y0 + 0.6, y1 - 0.6):
            b.box(m["galv"], (-0.12, yy - 0.05, ZH - 0.35), (0.12, yy + 0.05, ZH))
        for yy in (yc - 1.2, yc + 1.2):                                                   # sign lights
            b.bar(m["galv"], (0.0, yy, ZH - 0.35), (-0.7, yy, ZH - 0.1), 0.02, segs=4)
            b.box(m["lamp"], (-0.85, yy - 0.15, ZH - 0.16), (-0.6, yy + 0.15, ZH - 0.06))
    return b.finish()


# --------------------------------------------------------- misc/bus_shelter
def bus_shelter():
    m = cmat(); b = B("bus_shelter")
    L, D = 6.0, 2.2
    b.box(m["concrete"], (-L / 2 - 0.2, -D / 2 - 0.3, 0), (L / 2 + 0.2, D / 2 + 0.1, 0.12))           # pad
    for x in (-L / 2 + 0.15, L / 2 - 0.15):
        for y in (-D / 2 + 0.15, D / 2 - 0.15):
            b.cylinder(m["steel"], (x, y, 0.12), 0.06, 2.5, segs=8)
    # roof: slight fall to the rear, thick fascia
    quad(b, m["steel"], (-L / 2 - 0.3, -D / 2 - 0.3, 2.78), (L / 2 + 0.3, -D / 2 - 0.3, 2.78), (L / 2 + 0.3, D / 2 + 0.2, 2.62), (-L / 2 - 0.3, D / 2 + 0.2, 2.62), out=(0, 0, 1))
    quad(b, m["steel"], (-L / 2 - 0.3, -D / 2 - 0.3, 2.66), (L / 2 + 0.3, -D / 2 - 0.3, 2.66), (L / 2 + 0.3, D / 2 + 0.2, 2.5), (-L / 2 - 0.3, D / 2 + 0.2, 2.5), out=(0, 0, -1))
    b.box(m["steel"], (-L / 2 - 0.3, -D / 2 - 0.32, 2.5), (L / 2 + 0.3, -D / 2 - 0.2, 2.8))
    b.box(m["lit"], (-L / 2 + 0.2, -D / 2 - 0.325, 2.56), (L / 2 - 0.2, -D / 2 - 0.3, 2.74))            # lit route strip
    # glazing: back and both ends
    b.box(m["glass_pit"], (-L / 2 + 0.15, D / 2 - 0.08, 0.5), (L / 2 - 0.15, D / 2 - 0.04, 2.45))
    for s in (-1, 1):
        b.box(m["glass_pit"], (s * (L / 2 - 0.15) - 0.02, -D / 2 + 0.15, 0.5), (s * (L / 2 - 0.15) + 0.02, D / 2 - 0.15, 2.45))
    b.box(m["steel"], (-L / 2 + 0.15, D / 2 - 0.1, 2.4), (L / 2 - 0.15, D / 2 - 0.02, 2.5))
    # bench + perch, advert panel at one end, route pole
    b.box(m["steel_dark"], (-1.8, D / 2 - 0.65, 0.12), (1.8, D / 2 - 0.15, 0.2))
    b.box(m["plank"] if "plank" in m else m["steel"], (-1.8, D / 2 - 0.7, 0.5), (1.8, D / 2 - 0.15, 0.56)) if False else b.box(m["steel"], (-1.8, D / 2 - 0.7, 0.5), (1.8, D / 2 - 0.15, 0.56))
    for x in (-1.6, 0, 1.6):
        b.box(m["steel_dark"], (x - 0.04, D / 2 - 0.6, 0.12), (x + 0.04, D / 2 - 0.25, 0.5))
    b.box(m["steel_dark"], (L / 2 + 0.05, -0.6, 0.12), (L / 2 + 0.25, 0.6, 2.2))
    b.box(m["lit"], (L / 2 + 0.25, -0.5, 0.35), (L / 2 + 0.27, 0.5, 2.05))
    b.cylinder(m["galv"], (-L / 2 - 0.9, -D / 2 - 0.1, 0.12), 0.04, 3.3, segs=8)                 # bus-stop pole
    b.box(m["sign_blue"], (-L / 2 - 1.2, -D / 2 - 0.15, 2.5), (-L / 2 - 0.6, -D / 2 - 0.08, 3.4))
    return b.finish()


# --------------------------------------------------------- misc/station_entrance
def station_entrance():
    m = cmat(); b = B("station_entrance")
    L, D = 7.0, 5.0
    b.box(m["stone"], (-L / 2, -D / 2, 0), (L / 2, D / 2, 0.3))
    # stair well: dark opening with steps descending away from the road (+Y)
    b.box(m["black"], (-1.6, -D / 2 + 0.8, 0.31), (1.6, D / 2 - 0.5, 0.32))
    for i in range(6):
        b.box(m["concrete"], (-1.5, -D / 2 + 0.9 + i * 0.5, 0.3 - 0.0 - i * 0.0 + 0.0), (1.5, -D / 2 + 1.4 + i * 0.5, 0.33)) if False else None
    for s in (-1, 1):
        b.box(m["concrete_dark"], (s * 1.75 - 0.15, -D / 2 + 0.6, 0.3), (s * 1.75 + 0.15, D / 2 - 0.3, 1.05))     # parapet walls
        b.cylinder(m["galv"], (s * 1.5, -D / 2 + 0.7, 1.05), 0.035, 3.8, segs=8, axis='Y')                           # handrails
    b.box(m["concrete_dark"], (-1.9, D / 2 - 0.5, 0.3), (1.9, D / 2 - 0.3, 1.05))
    # glazed gable canopy on four posts
    for x in (-2.2, 2.2):
        for y in (-D / 2 + 0.4, D / 2 - 0.5):
            b.cylinder(m["steel"], (x, y, 0.3), 0.07, 3.0, segs=8)
    b.box(m["steel"], (-2.35, -D / 2 + 0.3, 3.3), (2.35, D / 2 - 0.4, 3.42))
    gable(b, m["glass_clear"], -2.4, 2.4, -D / 2 + 0.3, D / 2 - 0.4, 3.42, 0.9, ridge='y')
    b.box(m["steel"], (-0.06, -D / 2 + 0.3, 4.2), (0.06, D / 2 - 0.4, 4.35))
    # line-colour pylon with an arrow plate and a map case
    b.box(m["red"], (2.6, -1.8, 0.3), (3.1, -1.3, 3.6))
    b.box(m["sign_white"], (2.62, -1.82, 2.6), (3.08, -1.78, 3.4))
    b.box(m["black"], (2.7, -1.82, 3.4), (3.0, -1.78, 3.55))
    b.box(m["steel_dark"], (-3.3, -D / 2 + 0.2, 0.3), (-2.7, -D / 2 + 0.5, 2.0))
    b.box(m["lit"], (-3.25, -D / 2 + 0.18, 0.5), (-2.75, -D / 2 + 0.2, 1.8))
    for x in (-3.0, 3.0):                                                                           # planters
        b.box(m["concrete_dark"], (x - 0.4, D / 2 - 1.2, 0.3), (x + 0.4, D / 2 - 0.4, 0.9))
        b.ico(apex.material("mb_garden", (0.10, 0.26, 0.09), roughness=0.9), (x, D / 2 - 0.8, 1.2), 0.5, subdiv=1)
    return b.finish()


# --------------------------------------------------------- misc/carpark_entrance
def carpark_entrance():
    m = cmat(); b = B("carpark_entrance")
    L, D = 8.0, 9.0
    b.box(m["concrete"], (-L / 2, 0.0, 0), (L / 2, D / 2, 4.6))                          # building front with the portal
    b.box(m["black"], (-2.8, -0.05, 0), (2.8, 0.1, 3.3))                                 # dark ramp mouth
    # side retaining walls running out to the road, sloping down to zero
    for s in (-1, 1):
        prof = [(0.0, 0.0), (0.0, 3.6), (-D / 2 + 0.4, 0.0)]
        a = [b.bm.verts.new((s * 3.1, y, z)) for y, z in prof]
        c = [b.bm.verts.new((s * 3.5, y, z)) for y, z in prof]
        sl = b.slot(m["concrete"])
        for q in ((a[0], a[1], c[1], c[0]), (a[1], a[2], c[2], c[1]), (a[2], a[0], c[0], c[2])):
            f = b.bm.faces.new(q); f.material_index = sl
        for ring in (a, list(reversed(c))):
            f = b.bm.faces.new(ring); f.material_index = sl
    # ramp surface sinking into the portal (dark asphalt)
    quad(b, m["roof"], (-2.8, 0.1, -0.0), (2.8, 0.1, 0.0), (2.8, -D / 2 + 0.4, 0.02), (-2.8, -D / 2 + 0.4, 0.02), out=(0, 0, 1))
    # headroom bar, boom barrier, booth, P sign
    for s in (-1, 1):
        b.cylinder(m["steel"], (s * 3.0, -0.6, 0), 0.07, 3.0, segs=8)
    for i in range(10):
        c = m["yellow"] if i % 2 == 0 else m["black"]
        b.box(c, (-3.0 + i * 0.6, -0.68, 2.9), (-3.0 + (i + 1) * 0.6, -0.52, 3.05))
    b.box(m["white"], (3.4, -2.4, 0), (4.2, -1.6, 2.4))                                   # ticket booth
    b.box(m["glass_pit"], (3.4, -2.42, 1.1), (4.2, -2.38, 2.2))
    b.box(m["white"], (3.3, -2.5, 2.4), (4.3, -1.5, 2.55))
    b.box(m["steel_dark"], (2.55, -2.1, 0), (3.0, -1.7, 1.0))                              # barrier housing
    b.box(m["red"], (-2.0, -1.95, 0.95), (2.55, -1.85, 1.05))                              # boom arm
    b.box(m["white"], (-0.8, -1.96, 0.95), (-0.2, -1.84, 1.05)); b.box(m["white"], (0.8, -1.96, 0.95), (1.4, -1.84, 1.05))
    b.box(m["sign_blue"], (-0.9, -0.12, 3.7), (0.9, -0.04, 4.5))                           # P board (stem + bowl)
    b.box(m["sign_white"], (-0.45, -0.13, 3.8), (-0.25, -0.12, 4.4))
    b.box(m["sign_white"], (-0.25, -0.13, 4.2), (0.35, -0.12, 4.4)); b.box(m["sign_white"], (-0.25, -0.13, 3.9), (0.35, -0.12, 4.05))
    b.box(m["sign_white"], (0.25, -0.13, 3.95), (0.4, -0.12, 4.35))
    return b.finish()


# --------------------------------------------------------- bridge/linkbridge_covered
def linkbridge_covered():
    m = cmat(); b = B("linkbridge_covered")
    W, ZF = 3.6, 5.2
    b.box(m["concrete_dark"], (-W / 2, -9.8, ZF - 0.5), (W / 2, 9.8, ZF))                  # floor structure
    b.box(m["steel_dark"], (-W / 2 + 0.2, -9.8, ZF), (W / 2 - 0.2, 9.8, ZF + 0.04))
    for s in (-1, 1):
        b.box(m["glass_clear"], (s * (W / 2 - 0.1) - 0.04, -9.8, ZF + 0.9), (s * (W / 2 - 0.1) + 0.04, 9.8, ZF + 2.7))
        b.box(m["steel"], (s * (W / 2 - 0.1) - 0.08, -9.8, ZF), (s * (W / 2 - 0.1) + 0.08, 9.8, ZF + 0.9))   # spandrel
        b.box(m["steel"], (s * (W / 2 - 0.1) - 0.08, -9.8, ZF + 2.7), (s * (W / 2 - 0.1) + 0.08, 9.8, ZF + 2.95))
        for y in np.arange(-9.0, 9.1, 1.5):
            b.box(m["steel"], (s * (W / 2 - 0.1) - 0.06, y - 0.04, ZF + 0.9), (s * (W / 2 - 0.1) + 0.06, y + 0.04, ZF + 2.7))
    # curved roof: three-sided canopy over the glazing
    prof = [(-W / 2 - 0.3, ZF + 2.7), (-W / 2 + 0.3, ZF + 3.1), (0.0, ZF + 3.4), (W / 2 - 0.3, ZF + 3.1), (W / 2 + 0.3, ZF + 2.7)]
    for i in range(len(prof) - 1):
        x0, z0 = prof[i]; x1, z1 = prof[i + 1]
        quad(b, m["white"], (x0, -9.9, z0), (x1, -9.9, z1), (x1, 9.9, z1), (x0, 9.9, z0), out=(0, 0, 1))
        quad(b, m["white"], (x0, -9.9, z0 - 0.15), (x1, -9.9, z1 - 0.15), (x1, 9.9, z1 - 0.15), (x0, 9.9, z0 - 0.15), out=(0, 0, -1))
    b.box(m["lit"], (-0.4, -9.4, ZF + 3.22), (0.4, 9.4, ZF + 3.26))                         # light strip
    for s in (-1, 1):                                                                       # supporting columns off the verge
        b.cylinder(m["steel"], (0.0, s * 9.0, 0), 0.32, ZF - 0.5, segs=14)
        b.box(m["steel"], (-W / 2, s * 9.0 - 0.5, ZF - 0.9), (W / 2, s * 9.0 + 0.5, ZF - 0.5))
    return b.finish()


# --------------------------------------------------------- tree/raintree_l
def raintree_l():
    """Umbrella-crowned rain tree: a heavy clear trunk, a few spreading limbs, a wide flat crown."""
    rng = random.Random(7)
    mats = ct.materials()
    bark, leaf = mats["bark"], mats["broadleaf"]
    tm = ct.TreeMesh("raintree_l")
    H, Wd, card_m = 20.0, 26.0, 2.2
    base_z = 9.5
    margin = card_m * 0.45
    radii = Vector((Wd / 2 - margin, Wd / 2 * 0.94 - margin, (H - base_z) / 2 - margin * 0.5))
    centre = Vector((0, 0, base_z + radii.z + margin * 0.3))
    lobes = []
    n = 9
    for k in range(n):
        a = 2 * math.pi * k / n + rng.uniform(-0.25, 0.25)
        r = 0.5 if k else 0.0
        d = Vector((math.cos(a), math.sin(a), 0.15))
        c = centre + Vector((d.x * radii.x * (0.55 if k else 0.0), d.y * radii.y * (0.55 if k else 0.0), rng.uniform(-0.8, 0.6)))
        lr = Vector((radii.x * (0.5 if k else 0.62), radii.y * (0.5 if k else 0.62), radii.z * rng.uniform(0.7, 0.9)))
        lobes.append((c, d, lr))
    trunk_r = 0.62
    crotch = Vector((0.0, 0.0, base_z - 1.0))
    # trunk (slight lean) to the crotch, then a limb to each lobe
    tm.tube(bark, ct._bezier(Vector((0, 0, 0)), Vector((0.3, -0.2, base_z * 0.5)), crotch, 6),
            [trunk_r * (1.5 - 0.5 * k / 6) for k in range(7)], segs=10)
    for (c, d, lr) in lobes[1:]:
        start = crotch + Vector((0, 0, rng.uniform(0.0, 1.0)))
        mid = start + (c - start) * 0.45 + Vector((0, 0, 1.2))
        pts = ct._bezier(start, mid, c - Vector((0, 0, 0.7)), 4)
        tm.tube(bark, pts, [0.38 * (1 - 0.75 * i / 4) + 0.04 for i in range(5)], segs=7)
    ct._shell_cards(tm, leaf, rng, lobes, centre, radii, 700, card_m, rows=1, hang=0.3, floor=base_z - 0.5)
    return tm.finish()


# --------------------------------------------------------- light/lamp_arm_twin
def lamp_arm_twin():
    m = cmat(); b = B("lamp_arm_twin")
    H = 9.0
    b.cylinder(m["steel_dark"], (0, 0, 0), 0.32, 0.8, segs=12)
    b.cone(m["steel_dark"], (0, 0, 0.8), 0.22, 0.12, H - 1.2, segs=12)
    for s in (-1, 1):
        pts = [(0, 0, H - 0.9), (0, s * 0.8, H + 0.1), (0, s * 2.0, H + 0.45), (0, s * 3.0, H + 0.35)]
        for p, q in zip(pts, pts[1:]):
            b.bar(m["steel_dark"], p, q, 0.075, segs=6)
        b.box(m["steel_dark"], (-0.2, s * 3.0 - 0.45, H + 0.25), (0.2, s * 3.0 + 0.45, H + 0.4))     # luminaire housing
        b.box(m["lamp"], (-0.17, s * 3.0 - 0.4, H + 0.2), (0.17, s * 3.0 + 0.4, H + 0.25))           # lens, `floodlight_lamp` slot
        b.bar(m["steel_dark"], (0, s * 1.4, H - 0.2), (0, s * 2.0, H + 0.4), 0.03, segs=4)
    b.ico(m["steel_dark"], (0, 0, H + 0.05), 0.2, subdiv=1)
    return b.finish()


# --------------------------------------------------------- misc/monument_obelisk
def monument_obelisk():
    m = cmat(); b = B("monument_obelisk")
    for i, (h, w) in enumerate(((0.3, 7.0), (0.3, 6.2), (0.3, 5.4))):
        z = sum(x[0] for x in ((0.3, 0), (0.3, 0), (0.3, 0))[:i])
        b.box(m["stone"], (-w / 2, -w / 2, z), (w / 2, w / 2, z + h))
    b.box(m["stone"], (-2.2, -2.2, 0.9), (2.2, 2.2, 3.4))
    b.box(m["concrete"], (-2.45, -2.45, 3.4), (2.45, 2.45, 3.7))
    b.box(m["bronze"], (-1.2, -2.25, 1.6), (1.2, -2.2, 2.6))                                  # plaque
    secs = []
    for (z, w) in ((3.7, 3.0), (14.2, 1.55)):
        secs.append([(-w / 2, -w / 2, z), (w / 2, -w / 2, z), (w / 2, w / 2, z), (-w / 2, w / 2, z)])
    b.loft(m["stone"], secs, closed=True, cap_ends=True)
    frustum(b, m["stone"], (-0.775, 0.775, -0.775, 0.775), (-0.02, 0.02, -0.02, 0.02), 14.2, 15.5)
    return b.finish(recalc=True)


# --------------------------------------------------------- misc/monument_pagoda
def monument_pagoda():
    m = cmat(); b = B("monument_pagoda")
    def hexpts(r, z, rot=0.0):
        return [(r * math.cos(rot + math.pi / 3 * i), r * math.sin(rot + math.pi / 3 * i), z) for i in range(6)]
    prism_ngon(b, m["stone"], [(p[0], p[1]) for p in hexpts(5.2, 0)], 0, 0.5)
    prism_ngon(b, m["stone"], [(p[0], p[1]) for p in hexpts(4.4, 0)], 0.5, 0.95)
    prism_ngon(b, m["concrete"], [(p[0], p[1]) for p in hexpts(3.7, 0)], 0.95, 1.2)
    for p in hexpts(3.2, 1.2):
        b.cylinder(m["red"], (p[0], p[1], 1.2), 0.22, 4.4, segs=10)
        b.box(m["bronze"], (p[0] - 0.3, p[1] - 0.3, 5.6), (p[0] + 0.3, p[1] + 0.3, 5.75))
    ring = hexpts(3.55, 5.75)
    for i in range(6):
        j = (i + 1) % 6
        b.bar(m["red"], ring[i], ring[j], 0.14, segs=6)
        b.bar(m["red"], (ring[i][0], ring[i][1], 4.6), (ring[j][0], ring[j][1], 4.6), 0.1, segs=6)
    b.cone(m["tile"], (0, 0, 5.75), 5.2, 3.6, 1.1, segs=6, cap=True)
    b.cone(m["tile"], (0, 0, 6.85), 3.8, 1.2, 1.9, segs=6, cap=True)
    b.cone(m["tile"], (0, 0, 8.75), 1.5, 0.12, 0.9, segs=6, cap=True)
    b.ico(m["bronze"], (0, 0, 9.85), 0.3, subdiv=1)
    b.box(m["stone"], (-0.9, -0.2, 1.2), (0.9, 0.2, 3.6))                                     # stele inside
    b.box(m["bronze"], (-0.6, -0.22, 2.2), (0.6, -0.2, 3.0))
    return b.finish(recalc=True)


# --------------------------------------------------------- misc/monument_statue_plinth
def monument_statue_plinth():
    m = cmat(); b = B("monument_statue_plinth")
    for i, w in enumerate((6.0, 5.2, 4.4)):
        b.box(m["stone"], (-w / 2, -w / 2, i * 0.28), (w / 2, w / 2, (i + 1) * 0.28))
    b.box(m["stone"], (-1.7, -1.7, 0.84), (1.7, 1.7, 4.2))
    b.box(m["concrete"], (-1.95, -1.95, 4.2), (1.95, 1.95, 4.5))
    b.box(m["bronze"], (-0.8, -1.72, 2.3), (0.8, -1.7, 3.3))                                  # plaque
    z0 = 4.5
    for s in (-1, 1):                                                                          # boots + legs
        b.box(m["bronze"], (s * 0.3 - 0.17, -0.42, z0), (s * 0.3 + 0.17, 0.2, z0 + 0.2))
        b.cylinder(m["bronze"], (s * 0.3, -0.05, z0 + 0.2), 0.17, 1.2, segs=10)
    b.cone(m["bronze"], (0, 0, z0 + 0.35), 0.72, 0.56, 1.7, segs=14)                          # long coat
    b.cone(m["bronze"], (0, 0, z0 + 2.05), 0.56, 0.48, 1.05, segs=14)                         # torso
    b.box(m["bronze"], (-0.62, -0.27, z0 + 2.95), (0.62, 0.27, z0 + 3.2))                     # shoulders
    b.cylinder(m["bronze"], (0, 0, z0 + 3.15), 0.14, 0.3, segs=10)                            # neck
    b.ico(m["bronze"], (0, -0.02, z0 + 3.62), 0.3, subdiv=1, scale=(0.9, 1.0, 1.15))          # head
    b.cone(m["bronze"], (0, 0, z0 + 3.78), 0.34, 0.3, 0.12, segs=12)                          # cap brim/band
    b.bar(m["bronze"], (0.6, 0, z0 + 3.1), (0.66, -0.15, z0 + 1.95), 0.13, segs=6)            # arms: one down, one holding a scroll
    b.bar(m["bronze"], (-0.6, 0, z0 + 3.1), (-0.32, -0.45, z0 + 2.45), 0.13, segs=6)
    b.ico(m["bronze"], (0.66, -0.15, z0 + 1.9), 0.13, subdiv=1)
    b.cylinder(m["bronze"], (-0.32, -0.5, z0 + 2.3), 0.07, 0.5, segs=8, axis='X')
    return b.finish(recalc=True)


BUILD = {
    "quay_rail_4m": ("barrier", quay_rail_4m),
    "catch_fence_6m": ("fence", catch_fence_6m),
    "traffic_signal_pole": ("sign", traffic_signal_pole),
    "road_sign_post": ("sign", road_sign_post),
    "sign_gantry": ("bridge", sign_gantry),
    "bus_shelter": ("misc", bus_shelter),
    "station_entrance": ("misc", station_entrance),
    "carpark_entrance": ("misc", carpark_entrance),
    "linkbridge_covered": ("bridge", linkbridge_covered),
    "raintree_l": ("tree", raintree_l),
    "lamp_arm_twin": ("light", lamp_arm_twin),
    "monument_obelisk": ("misc", monument_obelisk),
    "monument_pagoda": ("misc", monument_pagoda),
    "monument_statue_plinth": ("misc", monument_statue_plinth),
}

def build(asset):
    clear_scene(); ground(60); kind, fn = BUILD[asset]; fn(); return kind

def export(asset):
    return export_tree(BUILD[asset][0], asset)

if ASSET != "all":
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
