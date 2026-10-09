r"""Marina Bay batch D1 - the start/finish straight (docs/content/MARINA_BAY_START_FINISH.md).

    ASSET = "all"   # or one key of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_d1.py").read())

"all" builds every asset into one scene (laid out in a row along X), exports each
GLB to content/props/<kind>/<asset>.glb and saves the scene as
content/props/_batches/marina_bay_d1.blend (a copy; the open file is untouched).

Conventions (PROPS.md): road on -Y, +X along the track, metres, pivot on the
ground. Deep road-facing kinds (grandstand, building, pit, the stage) have the
pivot on the road-facing edge and reach +Y; thin modules and free-standing
pieces are centred; the bridge is centred on its span (across Y).
Exceptions: `pit_light_truss_6m` and `led_ribbon_3m` have the pivot at their
bottom centre (place them at mounting height).

Emissive slots (the night pass drives these by name):
  stand_led_blue / stand_led_green   stand fascia + canopy edge LED lines
  globe_lamp                         lamp_globe_pole sphere
  balloon_lamp_white / _orange       lamp_balloon_tether(_orange)
  mb_lit_panel                       rims, lit rails, marquee panels, soffit strips
  floodlight_lamp                    pit_light_truss_6m, canopy lamp bars, stage lamps
  led_panel                          led_ribbon_3m face (brand from `text`), banner underside
  led_screen                         stage wall, pit-wall monitors and timing screen
  step_light_orange                  NEW: stair_zigzag_scaffold step lights
Texture slots: board_brand (banner_gantry, roof_wordmark_block, pit_wall_gantry
fascia), board_marker (garage_number_board, T_marker_<n>), crowd_cards (_crowd).
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())
try:
    ASSET
except NameError:
    ASSET = "all"

PROPS_DIR = os.path.join(_ROOT, "content", "props")


# ------------------------------------------------------------------ materials
def _mat_from_glb(name, glb):
    """Bring a slot in from an existing GLB (same material on every asset)."""
    m = bpy.data.materials.get(name)
    if m is not None:
        return m
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(PROPS_DIR, glb))
    for ob in set(bpy.data.objects) - before:
        bpy.data.objects.remove(ob, do_unlink=True)
    return bpy.data.materials[name]


def dmat():
    m = dict(mb_mats())
    M, A = tex.kit_material, apex.material
    m.update(
        gconc=M("grandstand_concrete", (0.6, 0.6, 0.6)),
        gdark=M("grandstand_concrete_dark", (0.42, 0.43, 0.44)),
        seat_a=M("grandstand_seat_a", (0.05, 0.12, 0.4)),
        seat_b=M("grandstand_seat_b", (0.75, 0.12, 0.08)),
        rail=M("grandstand_rail", (0.6, 0.62, 0.64)),
        gsteel=M("grandstand_steel", (0.25, 0.26, 0.28)),
        groof=M("grandstand_roof", (0.6, 0.62, 0.64)),
        plank=M("scaffold_plank", (0.55, 0.42, 0.25)),
        galv=M("armco_galv", (0.62, 0.64, 0.66), metallic=0.7, roughness=0.45),
        bpost=M("board_post", (0.4, 0.41, 0.43)),
        bback=M("board_back", (0.25, 0.26, 0.27)),
        bsteel=M("bridge_steel", (0.5, 0.51, 0.53)),
        bdark=M("bridge_dark", (0.12, 0.12, 0.13)),
        pconc=M("pit_concrete", (0.6, 0.6, 0.58)),
        tent=M("tent_white", (0.92, 0.92, 0.9), roughness=0.6),
        team=M("tent_colour", (0.8, 0.12, 0.1), roughness=0.6),
        flag=M("flag_cloth", (0.9, 0.9, 0.9), roughness=0.7),
        bark=M("tree_bark", (0.3, 0.22, 0.15), roughness=0.9),
        lamp=M("floodlight_lamp", (0.9, 0.9, 0.85), emission=(1, 0.95, 0.8)),
        led_blue=A("stand_led_blue", (0.1, 0.3, 1.0), roughness=0.4, emission=(0.1, 0.35, 1.0)),
        led_green=A("stand_led_green", (0.5, 1.0, 0.1), roughness=0.4, emission=(0.55, 1.0, 0.05)),
        globe=A("globe_lamp", (1.0, 0.95, 0.85), roughness=0.3, emission=(1.0, 0.85, 0.6)),
        bal_w=A("balloon_lamp_white", (0.95, 0.95, 0.92), roughness=0.5, emission=(1.0, 0.95, 0.85)),
        bal_o=A("balloon_lamp_orange", (1.0, 0.5, 0.1), roughness=0.5, emission=(1.0, 0.45, 0.05)),
        led_panel=A("led_panel", (0.05, 0.05, 0.06), roughness=0.3, emission=(1.0, 1.0, 1.0)),
        led_screen=A("led_screen", (0.03, 0.03, 0.04), roughness=0.25, emission=(0.6, 0.7, 1.0)),
        step_light=A("step_light_orange", (1.0, 0.45, 0.05), roughness=0.4, emission=(1.0, 0.4, 0.03)),
        roof_black=A("mb_roof_black", (0.05, 0.05, 0.055), roughness=0.8),
        sign_blue=A("mb_sign_blue", (0.03, 0.12, 0.45), roughness=0.5),
        sign_white=A("mb_sign_white", (0.92, 0.93, 0.92), roughness=0.4),
        black=A("mb_black", (0.03, 0.03, 0.035), roughness=0.5),
        garden=A("mb_garden", (0.10, 0.26, 0.09), roughness=0.9),
        soil=A("mb_soil", (0.16, 0.11, 0.07), roughness=1.0),
        brand=apex.image_material("board_brand", os.path.join(PR, "board", "brands", "apexsim.png")),
        marker=apex.image_material("board_marker", os.path.join(PR, "board", "markers", "1.png")),
        crowd=_mat_from_glb("crowd_cards", os.path.join("grandstand", "bay_10m_crowd.glb")),
        fmesh=_mat_from_glb("fence_mesh", os.path.join("barrier", "armco_4m_fence.glb")),
        fpost=A("fence_post", (0.4, 0.42, 0.43), metallic=0.8, roughness=0.5),
    )
    return m


# ------------------------------------------------------------------ helpers
def obox(b, mat, a, c, z0, z1, t):
    """Box between plan points a and c (its long axis), z0..z1, half-thickness t."""
    a, c = Vector((a[0], a[1], 0)), Vector((c[0], c[1], 0))
    d = (c - a).normalized()
    n = Vector((d.y, -d.x, 0))
    P = lambda p, s, z: (p.x + n.x * t * s, p.y + n.y * t * s, z)
    ctr = (a + c) / 2 + Vector((0, 0, (z0 + z1) / 2))
    def f(*pts):
        cen = sum((Vector(p) for p in pts), Vector()) / 4
        quad(b, mat, *pts, out=cen - ctr)
    f(P(a, -1, z1), P(c, -1, z1), P(c, 1, z1), P(a, 1, z1))
    f(P(a, -1, z0), P(c, -1, z0), P(c, 1, z0), P(a, 1, z0))
    f(P(a, 1, z0), P(c, 1, z0), P(c, 1, z1), P(a, 1, z1))
    f(P(a, -1, z0), P(c, -1, z0), P(c, -1, z1), P(a, -1, z1))
    f(P(a, -1, z0), P(a, 1, z0), P(a, 1, z1), P(a, -1, z1))
    f(P(c, -1, z0), P(c, 1, z0), P(c, 1, z1), P(c, -1, z1))
    return n


def face_mY(b, mat, x0, x1, z0, z1, y, ur=1.0, vr=1.0):
    """Explicit-UV quad facing -Y (u along +X)."""
    b.quad_uv(mat, [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)], [(0, 0), (ur, 0), (ur, vr), (0, vr)])


def face_pY(b, mat, x0, x1, z0, z1, y, ur=1.0, vr=1.0):
    """Explicit-UV quad facing +Y (u along -X, reads correctly from behind)."""
    b.quad_uv(mat, [(x1, y, z0), (x0, y, z0), (x0, y, z1), (x1, y, z1)], [(0, 0), (ur, 0), (ur, vr), (0, vr)])


def rail_run(b, mat, p0, p1, h=1.1, posts=2.0, r=0.025):
    """Handrail from p0 to p1 (ground points, may slope): posts, top and mid rail."""
    p0, p1 = Vector(p0), Vector(p1)
    n = max(1, int(round((p1 - p0).length / posts)))
    for i in range(n + 1):
        p = p0.lerp(p1, i / n)
        b.bar(mat, tuple(p), tuple(p + Vector((0, 0, h))), r, segs=6)
    b.bar(mat, tuple(p0 + Vector((0, 0, h))), tuple(p1 + Vector((0, 0, h))), r, segs=6)
    b.bar(mat, tuple(p0 + Vector((0, 0, h * 0.5))), tuple(p1 + Vector((0, 0, h * 0.5))), r * 0.75, segs=5)


# ================================================================== buildings
def pit_building_roofdeck():
    m = dmat(); b = B("pit_building_roofdeck")
    X0, X1 = -100.0, 100.0
    # block A: garages under a glazed hospitality band (lane side = -Y)
    b.box(m["pit_wall"], (X0, 0.5, 0), (X1, 34, 12.2))
    for i in range(33):
        x = -96.0 + i * 6.0
        b.box(m["door"], (x - 2.5, 0.45, 0.15), (x + 2.5, 0.5, 5.0))
        b.box(m["black"], (x - 0.6, 0.44, 5.25), (x + 0.6, 0.46, 5.75))             # number-board blank
    b.box(m["pit_floor"], (X0, -0.1, 0), (X1, 0.5, 0.15))
    b.box(m["white"], (X0, -0.6, 5.9), (X1, 0.5, 6.6))                                  # canopy slab over the lane
    b.box(m["glass_clear"], (X0, 0.0, 6.6), (X1, 0.5, 11.6))
    b.box(m["white"], (X0, -0.2, 11.6), (X1, 0.5, 12.2))
    # terrace 1 (z 12.2) and block B
    b.box(m["glass_pit"], (X0, 0.05, 12.2), (X1, 0.12, 13.3))
    b.cylinder(m["galv"], (X0, 0.09, 13.3), 0.035, X1 - X0, segs=6, axis='X')
    b.box(m["pit_wall"], (X0, 4.4, 12.2), (X1, 34, 17.6))
    b.box(m["glass_clear"], (X0 + 4, 4.0, 12.2), (X1 - 4, 4.4, 17.0))
    b.box(m["white"], (X0, 3.8, 17.0), (X1, 4.4, 17.6))
    # terrace 2 (z 17.6) and block C
    b.box(m["glass_pit"], (X0, 4.05, 17.6), (X1, 4.12, 18.7))
    b.cylinder(m["galv"], (X0, 4.09, 18.7), 0.035, X1 - X0, segs=6, axis='X')
    b.box(m["pit_wall"], (X0, 8.4, 17.6), (X1, 34, 22.6))
    b.box(m["glass_clear"], (X0 + 8, 8.0, 17.6), (X1 - 8, 8.4, 22.2))
    b.box(m["white"], (X0, 7.8, 22.2), (X1, 8.4, 22.6))
    # black roof deck with a raised parapet
    b.box(m["roof_black"], (X0, 8.4, 22.6), (X1, 34, 22.75))
    for lo, hi in (((X0, 8.0, 22.6), (X1, 8.4, 23.95)), ((X0, 33.6, 22.6), (X1, 34, 23.95)),
                   ((X0, 8.0, 22.6), (X0 + 0.4, 34, 23.95)), ((X1 - 0.4, 8.0, 22.6), (X1, 34, 23.95))):
        b.box(m["roof_black"], lo, hi)
    # stair towers at both ends, rising through the roof
    for s in (-1, 1):
        xa, xb = (X0 - 0.01, X0 + 5) if s < 0 else (X1 - 5, X1 + 0.01)
        b.box(m["concrete_dark"], (xa, 26, 0), (xb, 34.01, 25.2))
        b.box(m["glass_clear"], (xa + 1.2, 25.9, 1.0), (xb - 1.2, 26.0, 24.6))
    # roof masts
    for x in (-60, -20, 20, 60):
        b.cylinder(m["steel"], (x, 31, 22.75), 0.15, 3.25, segs=8)
        b.bar(m["steel"], (x, 31, 25.4), (x, 29.5, 22.75), 0.04, segs=4)
    return b.finish()


def roof_wordmark_block():
    m = dmat(); b = B("roof_wordmark_block")
    W, H, z0 = 40.0, 6.0, 1.5
    for i in range(9):
        x = -20 + 0.15 + i * (40 - 0.3) / 8
        b.box(m["bdark"], (x - 0.12, -0.12, 0), (x + 0.12, 0.12, z0))                # stilts
        b.bar(m["bdark"], (x, 0.28, 0), (x, 0.15, z0 + H * 0.7), 0.05, segs=5)       # back raker
        b.box(m["bdark"], (x - 0.25, -0.3, 0), (x + 0.25, 0.3, 0.06))                 # foot
    b.box(m["bback"], (-W / 2, -0.15, z0), (W / 2, 0.15, z0 + H))
    face_mY(b, m["brand"], -W / 2 + 0.3, W / 2 - 0.3, z0 + 0.3, z0 + H - 0.3, -0.152, ur=2.0)
    # lit red/white rim: alternating 2 m segments top and bottom, lit uprights at the ends
    for k in range(20):
        mat = m["lit"] if k % 2 == 0 else m["red"]
        xa = -W / 2 + k * 2.0
        b.box(mat, (xa, -0.2, z0 + H - 0.3), (xa + 2.0, -0.15, z0 + H))
        b.box(mat, (xa, -0.2, z0), (xa + 2.0, -0.15, z0 + 0.3))
    for s in (-1, 1):
        b.box(m["lit"], (s * W / 2 - 0.3 * (s > 0), -0.2, z0 + 0.3), (s * W / 2 + 0.3 * (s < 0), -0.15, z0 + H - 0.3))
    return b.finish()


def stair_zigzag_scaffold():
    m = dmat(); b = B("stair_zigzag_scaffold")
    n, run, rise = 20, 0.5, 0.175
    flights = ((-2.2, -0.2, -5.0, +1, 0.0), (0.2, 2.2, 5.0, -1, 3.5))           # y0, y1, x start, dir, z start
    for y0, y1, xs, dr, zs in flights:
        for i in range(n):
            xa = xs + dr * i * run
            z = zs + (i + 1) * rise
            lo, hi = sorted((xa, xa + dr * run))
            b.box(m["plank"], (lo, y0, z - 0.05), (hi, y1, z))
            b.box(m["step_light"], (xa - 0.02, y0 + 0.15, z - 0.07), (xa + 0.02, y1 - 0.15, z - 0.02))
        xe = xs + dr * n * run
        for y in (y0, y1):
            b.bar(m["gsteel"], (xs, y, zs), (xe, y, zs + n * rise), 0.06, segs=6)
            b.bar(m["galv"], (xs, y, zs + 1.0), (xe, y, zs + n * rise + 1.0), 0.025, segs=6)
            for i in range(0, n + 1, 5):
                x = xs + dr * i * run
                b.bar(m["galv"], (x, y, zs + i * rise), (x, y, zs + i * rise + 1.0), 0.022, segs=5)
    b.box(m["galv"], (5.0, -2.4, 3.42), (7.0, 2.4, 3.5))                                   # mid landing
    b.box(m["galv"], (-7.0, -2.4, 6.92), (-5.0, 2.4, 7.0))                                  # top landing
    rail_run(b, m["galv"], (7.0, -2.4, 3.5), (7.0, 2.4, 3.5), h=1.0)
    rail_run(b, m["galv"], (-7.0, 2.4, 7.0), (-5.0, 2.4, 7.0), h=1.0)
    rail_run(b, m["galv"], (-7.0, -2.4, 7.0), (-5.0, -2.4, 7.0), h=1.0)
    # scaffold: standards, ledgers, braces
    for x in (-7.0, -5.0, -2.5, 0.0, 2.5, 5.0, 7.0):
        for y in (-2.4, 0.0, 2.4):
            top = 7.0 if x <= -5 else (3.5 if x >= 5 else max(0.3, 5.25 - x * 0.35 if y > 0 else 1.75 + x * 0.35))
            b.cylinder(m["galv"], (x, y, 0), 0.045, top, segs=6)
    for y in (-2.4, 2.4):
        for z in (1.75, 3.45):
            b.cylinder(m["galv"], (-7.0, y, z), 0.035, 14.0, segs=6, axis='X')
        b.bar(m["galv"], (-7.0, y, 0.1), (-2.5, y, 3.45), 0.03, segs=5)
        b.bar(m["galv"], (2.5, y, 0.1), (7.0, y, 3.45), 0.03, segs=5)
    for x in (-7.0, -5.0, 5.0, 7.0):
        b.box(m["concrete_dark"], (x - 0.15, -2.55, 0), (x + 0.15, 2.55, 0.06))
    return b.finish()


# ================================================================== stands
ROWS, D, RISE, YF, Z0 = 18, 1.05, 0.72, 1.2, 2.2
ZDECK = Z0 + ROWS * RISE            # 15.16
YDECK = YF + ROWS * D               # 20.1


def _row(i):
    return YF + i * D, Z0 + i * RISE


def _stand_front(b, m, x0, x1):
    b.box(m["gdark"], (x0, 0.3, 0), (x1, YF, Z0))
    b.box(m["gconc"], (x0, 0.0, 0), (x1, 0.3, 3.0))
    b.box(m["led_blue"], (x0, -0.02, 2.40), (x1, 0.0, 2.55))
    b.box(m["led_green"], (x0, -0.02, 2.72), (x1, 0.0, 2.87))


def stand_bay(name, roof=False, crowd=False):
    m = dmat(); b = B(name)
    _stand_front(b, m, -5, 5)
    for i in range(ROWS):
        y, z = _row(i)
        b.box(m["gconc"], (-5, y, z - RISE - 0.1), (5, y + D, z))                     # stepped tread
        for xa, xb in ((-4.95, -0.6), (0.6, 4.95)):
            b.box(m["seat_a"], (xa, y + 0.18, z), (xb, y + 0.55, z + 0.42))
            b.box(m["seat_a"], (xa, y + 0.5, z + 0.42), (xb, y + 0.58, z + 0.8))
            if crowd:
                u0 = (i * 0.37 + (0.0 if xa < 0 else 0.55)) % 1.0
                b.quad_uv(m["crowd"], [(xa, y + 0.35, z), (xb, y + 0.35, z), (xb, y + 0.53, z + 1.15), (xa, y + 0.53, z + 1.15)],
                          [(u0, 0), (u0 + (xb - xa) / 8, 0), (u0 + (xb - xa) / 8, 1), (u0, 1)])
        b.box(m["gconc"], (-0.6, y, z), (0.6, y + D / 2, z + RISE / 2))                # aisle half-step
    # rear service deck, balustrade and wind screen
    b.box(m["gconc"], (-5, YDECK, ZDECK - 0.86), (5, 26, ZDECK))
    rail_run(b, m["rail"], (-5, 25.9, ZDECK), (5, 25.9, ZDECK), h=1.1)
    b.box(m["galv"], (-5, 25.88, ZDECK + 1.15), (5, 25.95, 17.0))
    # scaffold under the rake
    for x in (-2.5, 2.5):
        for i in range(0, ROWS, 3):
            y, z = _row(i)
            b.box(m["gsteel"], (x - 0.1, y + 0.4, 0), (x + 0.1, y + 0.6, z - RISE - 0.1))
        for y in (23.0, 25.7):
            b.box(m["gsteel"], (x - 0.1, y - 0.1, 0), (x + 0.1, y + 0.1, ZDECK - 0.86))
        for i in range(0, ROWS - 3, 3):
            ya, za = _row(i); yb, zb = _row(i + 3)
            b.bar(m["gsteel"], (x, ya + 0.5, 0.2), (x, yb + 0.5, zb - RISE - 0.2), 0.05, segs=5)
        b.bar(m["gsteel"], (x, 23.0, 0.2), (x, 25.7, ZDECK - 1.0), 0.05, segs=5)
    for i in range(0, ROWS, 3):
        y, z = _row(i)
        b.bar(m["gsteel"], (-2.5, y + 0.5, (z - RISE) * 0.5), (2.5, y + 0.5, (z - RISE) * 0.5), 0.04, segs=5)
    if roof:
        ZB, ZF, YC = 22.0, 20.6, 0.6                                  # roof top at the back / front, front edge
        zr = lambda y: ZF + (y - YC) * (ZB - ZF) / (26 - YC)
        for x in (-2.5, 2.5):
            b.box(m["gsteel"], (x - 0.15, 25.2, ZDECK), (x + 0.15, 25.5, ZB - 0.6))
            b.truss(m["gsteel"], (x, 25.6, ZB - 0.55), (x, YC + 0.5, ZF - 0.55), 0.35, 0.7, pitch=1.2, r_chord=0.05, r_diag=0.025)
            b.bar(m["gsteel"], (x, 25.35, ZDECK + 1.5), (x, 21.0, ZB - 0.8), 0.06, segs=5)
        quad(b, m["groof"], (-5, 26, ZB), (5, 26, ZB), (5, YC, ZF), (-5, YC, ZF), out=(0, 0, 1))
        quad(b, m["groof"], (-5, 26, ZB - 0.12), (5, 26, ZB - 0.12), (5, YC, ZF - 0.12), (-5, YC, ZF - 0.12), out=(0, 0, -1))
        for yc in (5.0, 11.0, 17.0, 23.0):                            # lit soffit strips
            ya, yb = yc - 0.4, yc + 0.4
            quad(b, m["lit"], (-4.8, ya, zr(ya) - 0.14), (4.8, ya, zr(ya) - 0.14), (4.8, yb, zr(yb) - 0.14), (-4.8, yb, zr(yb) - 0.14), out=(0, 0, -1))
        for yc in (3.0, 9.0, 15.0):                                   # lamp bars
            for x in (-3.75, -1.25, 1.25, 3.75):
                z = zr(yc) - 0.6
                b.box(m["bdark"], (x - 0.4, yc - 0.15, z), (x + 0.4, yc + 0.15, z + 0.25))
                b.box(m["lamp"], (x - 0.35, yc - 0.12, z - 0.03), (x + 0.35, yc + 0.12, z))
        b.box(m["white"], (-5, 0.4, ZF - 0.7), (5, 0.62, ZF + 0.12))                    # fascia
        b.box(m["led_blue"], (-5, 0.38, ZF - 0.55), (5, 0.4, ZF - 0.42))
        b.box(m["led_green"], (-5, 0.38, ZF - 0.27), (5, 0.4, ZF - 0.14))
    return b.finish()


def street_stand_tier_10m():
    return stand_bay("street_stand_tier_10m")


def street_stand_tier_10m_crowd():
    return stand_bay("street_stand_tier_10m_crowd", crowd=True)


def street_stand_tier_10m_roof():
    return stand_bay("street_stand_tier_10m_roof", roof=True)


def street_stand_tier_10m_roof_crowd():
    return stand_bay("street_stand_tier_10m_roof_crowd", roof=True, crowd=True)


def street_stand_tier_end():
    """4 m end cap, symmetric about x = 0: place at +-(L/2 + 2)."""
    m = dmat(); b = B("street_stand_tier_end")
    _stand_front(b, m, -2, 2)
    for i in range(ROWS):                                             # raked access stair, two steps a row
        y, z = _row(i)
        b.box(m["gconc"], (-1.7, y, z - RISE - 0.1), (1.7, y + D / 2, z - RISE / 2))
        b.box(m["gconc"], (-1.7, y + D / 2, z - RISE - 0.1), (1.7, y + D, z))
    for x in (-1.85, 1.85):                                           # open side: rake rails on scaffold
        rail_run(b, m["rail"], (x, YF, Z0), (x, YDECK, ZDECK), h=1.1, posts=3.15)
        for i in range(0, ROWS, 3):
            y, z = _row(i)
            b.box(m["gsteel"], (x - 0.1, y + 0.4, 0), (x + 0.1, y + 0.6, z - RISE - 0.1))
        for i in range(0, ROWS - 3, 3):
            ya, za = _row(i); yb, zb = _row(i + 3)
            b.bar(m["gsteel"], (x, ya + 0.5, 0.2), (x, yb + 0.5, zb - RISE - 0.2), 0.05, segs=5)
    # stair tower behind: five flights to the service deck, roofed at 17 m
    for x in (-1.9, 1.9):
        for y in (YDECK + 0.1, 25.9):
            b.box(m["gsteel"], (x - 0.1, y - 0.1, 0), (x + 0.1, y + 0.1, 17.0))
    for k in range(5):
        zs = k * 3.0
        xa, xb = (-1.8, -0.1) if k % 2 == 0 else (0.1, 1.8)
        for j in range(15):
            yy = (20.7 + j * 0.3) if k % 2 == 0 else (25.1 - j * 0.3)
            lo, hi = sorted((yy, yy + (0.3 if k % 2 == 0 else -0.3)))
            b.box(m["plank"], (xa, lo, zs + (j + 1) * 0.2 - 0.05), (xb, hi, zs + (j + 1) * 0.2))
        b.box(m["plank"], (-1.9, YDECK, zs + 2.95), (1.9, 20.7, zs + 3.0))
        b.box(m["plank"], (-1.9, 25.1, zs + 2.95), (1.9, 26, zs + 3.0))
        b.bar(m["galv"], (0, 20.7, zs + 1.0), (0, 25.1, zs + 4.0), 0.025, segs=5)
    b.box(m["gconc"], (-2, YDECK, ZDECK - 0.16), (2, 26, ZDECK))
    b.box(m["gsteel"], (-2.05, YDECK, 16.85), (2.05, 26.05, 17.0))
    for y in (YDECK + 0.1, 25.9):
        rail_run(b, m["galv"], (-1.9, y, 0), (1.9, y, 0), h=1.1)
    return b.finish()


def street_stand_deck_10m():
    m = dmat(); b = B("street_stand_deck_10m")
    b.box(m["glass_clear"], (-5, 2.0, 0), (5, 14, 3.9))
    b.box(m["white"], (-5, 0, 3.9), (5, 14, 4.3))                                       # balcony slab
    b.box(m["glass_pit"], (-5, 0.05, 4.3), (5, 0.13, 5.4))
    b.cylinder(m["galv"], (-5, 0.09, 5.4), 0.035, 10, segs=6, axis='X')
    for xa, xb in ((-4.9, -0.4), (0.4, 4.9)):
        b.box(m["seat_b"], (xa, 0.7, 4.3), (xb, 1.1, 4.75))
        b.box(m["seat_b"], (xa, 1.05, 4.75), (xb, 1.13, 5.15))
    b.box(m["glass_clear"], (-5, 3.0, 4.3), (5, 14, 8.2))
    b.box(m["white"], (-5, 1.5, 8.2), (5, 14, 8.6))
    b.box(m["led_blue"], (-5, 1.48, 8.3), (5, 1.5, 8.45))
    b.box(m["white"], (-5, 13.8, 8.6), (5, 14, 9.0))
    for x in (-4.85, 4.85):
        b.box(m["white"], (x - 0.15, 1.5, 4.3), (x + 0.15, 1.8, 8.2))
    return b.finish()


# ================================================================== overhead / furniture
def banner_gantry():
    """Bridge, centred on the span (local Y). Legs skewed 3 m along X; banner faces +-X."""
    m = dmat(); b = B("banner_gantry")
    A, C = (-1.5, -10.0), (1.5, 10.0)
    for (x, y) in (A, C):
        b.box(m["concrete"], (x - 0.8, y - 0.8, 0), (x + 0.8, y + 0.8, 0.6))
        b.truss(m["galv"], (x, y, 0.6), (x, y, 6.2), 0.8, 0.8, pitch=1.0, r_chord=0.06, r_diag=0.03)
    b.truss(m["bsteel"], (A[0], A[1], 6.6), (C[0], C[1], 6.6), 0.8, 0.8, pitch=1.2, r_chord=0.07, r_diag=0.035)
    P = lambda t: (A[0] + (C[0] - A[0]) * t, A[1] + (C[1] - A[1]) * t)
    a, c = P(0.1), P(0.9)
    zb, zt = 4.4, 6.1
    n = obox(b, m["bdark"], a, c, zb, zt, 0.12)
    obox(b, m["led_panel"], a, c, zb - 0.1, zb, 0.06)                                   # underside strip
    t = 0.125
    off = lambda p, s: (p[0] + n.x * t * s, p[1] + n.y * t * s)
    a1, c1 = off(a, 1), off(c, 1)
    b.quad_uv(m["brand"], [(a1[0], a1[1], zb + 0.1), (c1[0], c1[1], zb + 0.1), (c1[0], c1[1], zt - 0.1), (a1[0], a1[1], zt - 0.1)],
              [(0, 0), (2, 0), (2, 1), (0, 1)])
    a2, c2 = off(a, -1), off(c, -1)
    b.quad_uv(m["brand"], [(c2[0], c2[1], zb + 0.1), (a2[0], a2[1], zb + 0.1), (a2[0], a2[1], zt - 0.1), (c2[0], c2[1], zt - 0.1)],
              [(0, 0), (2, 0), (2, 1), (0, 1)])
    for tt in (0.2, 0.5, 0.8):
        p = P(tt)
        b.bar(m["galv"], (p[0], p[1], zt), (p[0], p[1], 6.25), 0.04, segs=5)
    return b.finish()


def finish_tower_scaffold():
    m = dmat(); b = B("finish_tower_scaffold")
    for x in (-1.95, 0.35):
        for y in (-1.95, 1.95):
            b.cylinder(m["galv"], (x, y, 0), 0.05, 3.5, segs=6)
            b.box(m["concrete_dark"], (x - 0.15, y - 0.15, 0), (x + 0.15, y + 0.15, 0.04))
    for z in (1.2, 2.4):
        b.cylinder(m["galv"], (-1.95, -1.95, z), 0.035, 3.9, segs=5, axis='Y')
        b.cylinder(m["galv"], (0.35, -1.95, z), 0.035, 3.9, segs=5, axis='Y')
    b.bar(m["galv"], (-1.95, -1.95, 0.1), (-1.95, 1.95, 3.4), 0.03, segs=5)
    b.bar(m["galv"], (0.35, 1.95, 0.1), (0.35, -1.95, 3.4), 0.03, segs=5)
    b.box(m["plank"], (-2.0, -2.0, 3.5), (0.4, 2.0, 3.6))
    b.box(m["white"], (-2.0, -2.0, 3.6), (0.4, 2.0, 4.4))
    b.box(m["glass_pit"], (-1.95, -1.95, 4.4), (0.35, 1.95, 5.9))
    for x in (-1.95, 0.35):
        for y in (-1.95, 1.95):
            b.box(m["white"], (x - 0.05, y - 0.05, 4.4), (x + 0.05, y + 0.05, 5.9))
    b.box(m["white"], (-2.1, -2.1, 5.9), (0.5, 2.1, 6.15))
    b.box(m["black"], (-1.4, -0.6, 6.4), (-0.8, 0.0, 6.75))                             # camera
    b.bar(m["black"], (-1.1, -0.6, 6.55), (-1.1, -0.85, 6.55), 0.09, segs=10)
    for dx, dy in ((-0.3, -0.3), (0.3, -0.3), (0.0, 0.3)):
        b.bar(m["galv"], (-1.1, -0.3, 6.4), (-1.1 + dx, -0.3 + dy, 6.15), 0.015, segs=4)
    b.cylinder(m["galv"], (0.2, 1.8, 6.15), 0.03, 0.85, segs=6)                         # flag mast
    b.quad_uv(m["flag"], [(0.2, 1.8, 6.5), (0.2, 1.2, 6.5), (0.2, 1.2, 6.9), (0.2, 1.8, 6.9)], [(0, 0), (1, 0), (1, 1), (0, 1)])
    b.quad_uv(m["flag"], [(0.2, 1.2, 6.5), (0.2, 1.8, 6.5), (0.2, 1.8, 6.9), (0.2, 1.2, 6.9)], [(1, 0), (0, 0), (0, 1), (1, 1)])
    for j in range(14):                                                                  # stair along +Y
        y, z = -1.9 + j * 0.25, (j + 1) * 0.25
        b.box(m["plank"], (0.55, y, z - 0.04), (1.85, y + 0.25, z))
    for x in (0.55, 1.85):
        b.bar(m["gsteel"], (x, -1.9, 0), (x, 1.6, 3.5), 0.05, segs=5)
        b.bar(m["galv"], (x, -1.9, 1.0), (x, 1.6, 4.5), 0.025, segs=5)
        b.cylinder(m["galv"], (x, -1.9, 0), 0.03, 1.0, segs=5)
        b.cylinder(m["galv"], (x, 1.6, 0), 0.04, 4.5, segs=5)
    b.box(m["plank"], (0.4, 1.6, 3.45), (1.9, 2.0, 3.5))
    rail_run(b, m["galv"], (1.9, 2.0, 3.5), (0.4, 2.0, 3.5), h=1.0)
    return b.finish()


def lamp_globe_pole():
    m = dmat(); b = B("lamp_globe_pole")
    b.box(m["steel_dark"], (-0.3, -0.3, 0), (0.3, 0.3, 0.03))
    b.cylinder(m["steel_dark"], (0, 0, 0.03), 0.22, 0.35, segs=16)
    b.cone(m["steel_dark"], (0, 0, 0.38), 0.075, 0.05, 5.32, segs=12)
    b.cylinder(m["steel_dark"], (0, 0, 5.62), 0.09, 0.1, segs=12)
    b.ico(m["globe"], (0, 0, 6.1), 0.4, subdiv=2)
    return b.finish()


def _balloon(name, key):
    m = dmat(); b = B(name)
    b.box(m["concrete_dark"], (-0.3, -0.3, 0), (0.3, 0.3, 0.35))
    b.torus(m["galv"], (0, 0, 0.38), 0.08, 0.02, segs=10, rings=4)
    b.bar(m["galv"], (0, 0, 0.38), (0, 0, 7.3), 0.012, segs=4)
    for k in range(3):
        a = 2 * math.pi * k / 3
        b.bar(m["galv"], (0, 0, 7.3), (0.45 * math.cos(a), 0.45 * math.sin(a), 7.62), 0.008, segs=3)
    b.cylinder(m["black"], (0, 0, 7.36), 0.2, 0.08, segs=12)
    b.ico(m[key], (0, 0, 8.2), 0.8, subdiv=2)
    return b.finish()


def lamp_balloon_tether():
    return _balloon("lamp_balloon_tether", "bal_w")


def lamp_balloon_tether_orange():
    return _balloon("lamp_balloon_tether_orange", "bal_o")


def pit_light_truss_6m():
    m = dmat(); b = B("pit_light_truss_6m")
    b.truss(m["galv"], (-3, 0, 0.78), (3, 0, 0.78), 1.0, 0.8, pitch=1.0, r_chord=0.045, r_diag=0.022)
    for x in (-2.25, -0.75, 0.75, 2.25):
        b.box(m["bdark"], (x - 0.35, -0.2, 0.08), (x + 0.35, 0.2, 0.28))
        b.box(m["lamp"], (x - 0.3, -0.17, 0.05), (x + 0.3, 0.17, 0.08))
        for s in (-1, 1):
            b.bar(m["bdark"], (x + s * 0.3, 0, 0.28), (x + s * 0.3, 0, 0.4), 0.02, segs=4)
    return b.finish()


def led_ribbon_3m():
    m = dmat(); b = B("led_ribbon_3m")
    b.box(m["bback"], (-1.5, -0.05, 0.05), (1.5, 0.075, 0.9))
    face_mY(b, m["led_panel"], -1.45, 1.45, 0.1, 0.85, -0.051)
    for x in (-1.2, 1.2):
        b.box(m["bpost"], (x - 0.06, -0.075, 0), (x + 0.06, 0.075, 0.05))
    return b.finish()


def catch_fence_post_lit_6m():
    m = dmat(); b = B("catch_fence_post_lit_6m")
    HV, HT, LEAN = 5.4, 6.4, -0.3
    b.box(m["concrete"], (-2.0, -0.2, 0), (2.0, 0.2, 0.3))
    for x in (-1.9, 0.0, 1.9):
        b.cylinder(m["fpost"], (x, 0, 0.3), 0.075, HV - 0.3, segs=8)
        b.bar(m["fpost"], (x, 0, HV), (x, LEAN, HT), 0.06, segs=8)
    for z in (0.9, 3.0, HV - 0.05):
        b.cylinder(m["fpost"], (-2.0, 0, z), 0.03, 4.0, segs=6, axis='X')
    b.quad_uv(m["fmesh"], [(-2.0, -0.08, 0.3), (2.0, -0.08, 0.3), (2.0, -0.08, HV), (-2.0, -0.08, HV)],
              [(0, 0), (4, 0), (4, HV - 0.3), (0, HV - 0.3)])
    hl = math.hypot(LEAN, HT - HV)
    b.quad_uv(m["fmesh"], [(-2.0, -0.08, HV), (2.0, -0.08, HV), (2.0, LEAN - 0.06, HT - 0.05), (-2.0, LEAN - 0.06, HT - 0.05)],
              [(0, HV - 0.3), (4, HV - 0.3), (4, HV - 0.3 + hl), (0, HV - 0.3 + hl)])
    b.box(m["steel_dark"], (-2.0, LEAN - 0.08, HT - 0.02), (2.0, LEAN + 0.08, HT + 0.06))
    b.box(m["lit"], (-2.0, LEAN - 0.09, HT - 0.12), (2.0, LEAN - 0.07, HT - 0.02))
    return b.finish()


def spectator_handrail_4m():
    m = dmat(); b = B("spectator_handrail_4m")
    for x in (-1.0, 1.0):
        b.box(m["rail"], (x - 0.05, -0.05, 0), (x + 0.05, 0.05, 0.01))
        b.cylinder(m["rail"], (x, 0, 0), 0.025, 1.08, segs=8)
    b.cylinder(m["rail"], (-2.0, 0, 1.075), 0.025, 4.0, segs=8, axis='X')
    b.cylinder(m["rail"], (-2.0, 0, 0.55), 0.018, 4.0, segs=6, axis='X')
    return b.finish()


def pit_entry_board():
    m = dmat(); b = B("pit_entry_board")
    for x in (-1.35, 1.35):
        b.cylinder(m["bpost"], (x, 0.07, 0), 0.08, 4.5, segs=10)
        for z in (3.4, 4.2):
            b.bar(m["bpost"], (x, 0.07, z), (x, -0.05, z), 0.03, segs=4)
    b.box(m["sign_blue"], (-1.5, -0.15, 3.2), (1.5, -0.05, 4.4))
    for lo, hi in (((-1.45, -0.16, 3.25), (1.45, -0.15, 3.3)), ((-1.45, -0.16, 4.3), (1.45, -0.15, 4.35)),
                   ((-1.45, -0.16, 3.25), (-1.4, -0.15, 4.35)), ((1.4, -0.16, 3.25), (1.45, -0.15, 4.35))):
        b.box(m["sign_white"], lo, hi)
    me = apex.text_mesh("pit_entry", "PIT", size=0.75, extrude=0.02,
                        font=bpy.data.fonts.load(r"C:\Windows\Fonts\arialbd.ttf", check_existing=True))
    apex.merge_mesh(b, m["sign_white"], me, offset=(-0.42, -0.165, 3.5))    # frees `me`
    b.box(m["sign_white"], (0.45, -0.17, 3.72), (1.0, -0.15, 3.88))
    tri(b, m["sign_white"], (1.0, -0.17, 3.52), (1.3, -0.17, 3.8), (1.0, -0.17, 4.08), out=(0, -1, 0))
    return b.finish()


def garage_number_board():
    m = dmat(); b = B("garage_number_board")
    b.box(m["bback"], (-0.75, 0.0, 0), (0.75, 0.08, 0.8))
    face_mY(b, m["marker"], -0.72, 0.72, 0.03, 0.77, -0.002)
    for x in (-0.6, 0.6):
        b.box(m["bpost"], (x - 0.03, 0.08, 0.35), (x + 0.03, 0.1, 0.45))
    return b.finish()


def pit_wall_gantry_6m():
    m = dmat(); b = B("pit_wall_gantry_6m")
    b.box(m["pconc"], (-3, 0, 0), (3, 0.4, 1.1))
    b.box(m["pit_floor"], (-3, 0.4, 0), (3, 2.0, 0.3))
    b.box(m["gsteel"], (-2.8, 0.42, 0.95), (2.8, 0.95, 1.05))
    for x in (-2.7, 2.7):
        b.box(m["gsteel"], (x - 0.04, 0.5, 0.3), (x + 0.04, 0.9, 0.95))
    for x in (-2.1, -0.7, 0.7, 2.1):
        b.box(m["black"], (x - 0.3, 0.44, 1.05), (x + 0.3, 0.5, 1.45))
        face_pY(b, m["led_screen"], x - 0.27, x + 0.27, 1.08, 1.42, 0.501)
        b.cylinder(m["black"], (x, 1.35, 0.3), 0.035, 0.62, segs=6)
        b.cylinder(m["black"], (x, 1.35, 0.92), 0.2, 0.06, segs=12)
    b.box(m["black"], (-1.0, 0.5, 2.15), (1.0, 0.62, 2.8))                               # timing screen
    face_pY(b, m["led_screen"], -0.95, 0.95, 2.2, 2.75, 0.621)
    b.bar(m["gsteel"], (-0.8, 0.56, 2.8), (-0.8, 0.56, 3.0), 0.02, segs=4)
    b.bar(m["gsteel"], (0.8, 0.56, 2.8), (0.8, 0.56, 3.0), 0.02, segs=4)
    b.box(m["gsteel"], (2.45, 1.7, 0.3), (2.9, 1.9, 1.7))                                # headset rack
    for z in (0.8, 1.15, 1.5):
        b.bar(m["galv"], (2.5, 1.7, z), (2.85, 1.7, z), 0.015, segs=4)
    for x in (-2.9, 2.9):
        b.cylinder(m["gsteel"], (x, 1.9, 0.3), 0.06, 2.7, segs=8)
        b.cylinder(m["gsteel"], (x, 0.2, 1.1), 0.05, 1.9, segs=8)
    b.box(m["team"], (-3, 0, 3.0), (3, 2.0, 3.2))
    face_mY(b, m["brand"], -3, 3, 3.0, 3.2, -0.002, ur=10.0, vr=0.2)
    return b.finish()


def marquee_peak_12m():
    m = dmat(); b = B("marquee_peak_12m")
    b.box(m["plank"], (-6, -4, 0), (6, 4, 0.15))
    for x in (-6, 0, 6):
        for y in (-4, 4):
            b.cylinder(m["alu"], (x - 0.06 * (x > 0) + 0.06 * (x < 0), y - 0.06 * (y > 0) + 0.06 * (y < 0), 0.15), 0.06, 2.85, segs=8)
    b.box(m["tent"], (-6, 3.95, 0.15), (6, 4.0, 3.0))
    for s in (-1, 1):
        b.box(m["tent"], (s * 6 - 0.05 * (s > 0), -4, 0.15), (s * 6 + 0.05 * (s < 0), 4, 3.0))
        b.box(m["lit"], (s * 6 + 0.01 * s - 0.01, -2.5, 1.6), (s * 6 + 0.01 * s + 0.01, 2.5, 2.4))
    for x in (-3, 3):
        b.box(m["lit"], (x - 1.6, 3.92, 1.5), (x + 1.6, 3.95, 2.5))
    b.box(m["tent"], (-6, -4.02, 2.6), (6, -3.97, 3.0))                                    # front valance
    for x0, x1 in ((-6, 0), (0, 6)):
        xc = (x0 + x1) / 2
        frustum(b, m["tent"], (x0, x1, -4, 4), (xc - 0.05, xc + 0.05, -0.05, 0.05), 3.0, 5.85)
        b.cylinder(m["alu"], (xc, 0, 5.85), 0.04, 0.15, segs=6)
    return b.finish()


def led_wall_stage_16m():
    m = dmat(); b = B("led_wall_stage_16m")
    b.box(m["black"], (-8, 0, 0), (8, 6, 1.2))
    b.box(m["plank"], (-8, 0, 1.2), (8, 6, 1.3))
    b.box(m["bdark"], (-7.5, 5.4, 1.3), (7.5, 5.8, 8.4))
    face_mY(b, m["led_screen"], -7.3, 7.3, 1.5, 8.2, 5.399)
    for x in (-7.75, 7.75):
        for y in (0.4, 5.6):
            b.truss(m["galv"], (x, y, 0), (x, y, 8.6), 0.4, 0.4, pitch=0.8, r_chord=0.035, r_diag=0.018)
        b.truss(m["galv"], (x, 0.4, 8.8), (x, 5.6, 8.8), 0.4, 0.4, pitch=0.8, r_chord=0.035, r_diag=0.018)
    for y in (0.4, 5.6):
        b.truss(m["galv"], (-7.75, y, 8.8), (7.75, y, 8.8), 0.4, 0.4, pitch=0.8, r_chord=0.035, r_diag=0.018)
    for x in (-5.0, -2.5, 0.0, 2.5, 5.0):
        b.box(m["bdark"], (x - 0.2, 0.2, 8.15), (x + 0.2, 0.6, 8.55))
        b.box(m["lamp"], (x - 0.17, 0.17, 8.12), (x + 0.17, 0.57, 8.15))
    for x in (-7.2, 7.2):
        b.box(m["black"], (x - 0.45, 0.3, 1.3), (x + 0.45, 1.2, 3.3))
    return b.finish()


def walkway_planter_4m():
    m = dmat(); b = B("walkway_planter_4m")
    b.box(m["stone"], (-2, -0.6, 0), (2, 0.6, 0.55))
    b.box(m["soil"], (-1.85, -0.45, 0.55), (1.85, 0.45, 0.57))
    for x in (-1.35, 1.35):
        b.ico(m["garden"], (x, 0, 0.72), 0.35, subdiv=1, scale=(1.4, 1.0, 0.7))
    b.cylinder(m["bark"], (0, 0, 0.55), 0.04, 0.42, segs=6)
    b.ico(m["garden"], (0, 0, 1.0), 0.36, subdiv=1, scale=(1.5, 1.3, 0.55))
    return b.finish()


BUILD = {
    "pit_building_roofdeck": ("building", pit_building_roofdeck),
    "roof_wordmark_block": ("board", roof_wordmark_block),
    "stair_zigzag_scaffold": ("misc", stair_zigzag_scaffold),
    "street_stand_tier_10m": ("grandstand", street_stand_tier_10m),
    "street_stand_tier_10m_crowd": ("grandstand", street_stand_tier_10m_crowd),
    "street_stand_tier_end": ("grandstand", street_stand_tier_end),
    "street_stand_tier_10m_roof": ("grandstand", street_stand_tier_10m_roof),
    "street_stand_tier_10m_roof_crowd": ("grandstand", street_stand_tier_10m_roof_crowd),
    "street_stand_deck_10m": ("grandstand", street_stand_deck_10m),
    "banner_gantry": ("bridge", banner_gantry),
    "finish_tower_scaffold": ("misc", finish_tower_scaffold),
    "lamp_globe_pole": ("light", lamp_globe_pole),
    "lamp_balloon_tether": ("light", lamp_balloon_tether),
    "lamp_balloon_tether_orange": ("light", lamp_balloon_tether_orange),
    "pit_light_truss_6m": ("light", pit_light_truss_6m),
    "led_ribbon_3m": ("board", led_ribbon_3m),
    "catch_fence_post_lit_6m": ("fence", catch_fence_post_lit_6m),
    "spectator_handrail_4m": ("barrier", spectator_handrail_4m),
    "pit_entry_board": ("sign", pit_entry_board),
    "garage_number_board": ("sign", garage_number_board),
    "pit_wall_gantry_6m": ("pit", pit_wall_gantry_6m),
    "marquee_peak_12m": ("attraction", marquee_peak_12m),
    "led_wall_stage_16m": ("attraction", led_wall_stage_16m),
    "walkway_planter_4m": ("misc", walkway_planter_4m),
}


def build(asset):
    clear_scene(); ground(60); kind, fn = BUILD[asset]; fn(); return kind


def export(asset):
    return export_tree(BUILD[asset][0], asset)


def build_all(save=True):
    """Every asset in one scene, in a row along X; export each and save the batch .blend."""
    clear_scene()
    out, x = {}, 0.0
    for name, (kind, fn) in BUILD.items():
        ob = fn()
        lo, hi = bounds(name)
        ob.location = (x - lo[0], 0, 0)
        x += (hi[0] - lo[0]) + 8.0
        glb, size = export_tree(kind, name)
        out[name] = {"kind": kind, "tris": tri_count(name), "bounds": [lo, hi], "kb": size // 1024}
    if save:
        p = os.path.join(PR, "_batches", "marina_bay_d1.blend")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=p, copy=True)
    return out


if ASSET == "all":
    RESULT = build_all()
else:
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
