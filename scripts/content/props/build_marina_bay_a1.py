r"""Marina Bay batch A1 - Esplanade domes, XL observation wheel (+terminal), pit building.

    ASSET = "all"   # or one of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_a1.py").read())
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())

try:
    ASSET
except NameError:
    ASSET = "all"


# --------------------------------------------------------- building/landmark_twin_domes
def dome_tris(b, glass, alu, c, rx, ry, rz, nseg=40, nring=10, spike=1.3):
    """Triangulated dome: glass facets + an aluminium sunshade pyramid on each."""
    P = ellipsoid_pts(c, rx, ry, rz, nseg, nring)
    c = Vector(c)

    def out_of(p):
        return Vector(((p[0] - c.x) / rx ** 2, (p[1] - c.y) / ry ** 2, (p[2] - c.z) / rz ** 2))

    def facet(p0, p1, p2):
        p0, p1, p2 = Vector(p0), Vector(p1), Vector(p2)
        n = (p1 - p0).cross(p2 - p0)
        if n.length < 1e-3:
            return
        cen = (p0 + p1 + p2) / 3
        o = out_of(cen)
        tri(b, glass, p0, p1, p2, out=o)
        nn = n.normalized()
        if nn.dot(o) < 0:
            nn = -nn
        apex = cen + nn * spike
        q0, q1, q2 = (cen + (p - cen) * 0.88 for p in (p0, p1, p2))
        for a, bb in ((q0, q1), (q1, q2), (q2, q0)):
            tri(b, alu, a + nn * 0.05, bb + nn * 0.05, apex, out=(a + bb) / 2 + nn * 0.1 - c)

    for i in range(nring):
        for j in range(nseg):
            k = (j + 1) % nseg
            a, bb, cc, d = P[i][j], P[i][k], P[i + 1][k], P[i + 1][j]
            if (i + j) % 2 == 0:
                facet(a, bb, cc); facet(a, cc, d)
            else:
                facet(a, bb, d); facet(bb, cc, d)


def twin_domes():
    m = mb_mats()
    dg = apex.material("mb_dome_glass", (0.03, 0.09, 0.14), metallic=0.5, roughness=0.05)
    b = B("landmark_twin_domes")
    Z0 = 5.2
    domes = [(-48.0, 46.0, 39.5, 29.8), (47.0, 42.0, 37.0, 27.6)]    # cx, rx, ry, rz  (35 m / 32.8 m crowns)
    for cx, rx, ry, rz in domes:
        # drum: footing, glazed band, cornice
        prism_ngon(b, m["concrete"], ellipse_pts(cx, 0, rx + 1.4, ry + 1.4, 64), 0, 1.2)
        prism_ngon(b, m["glass_clear"], ellipse_pts(cx, 0, rx + 0.9, ry + 0.9, 64), 1.2, 4.4)
        prism_ngon(b, m["stone"], ellipse_pts(cx, 0, rx + 1.6, ry + 1.6, 64), 4.4, Z0)
        dome_tris(b, dg, m["alu"], (cx, 0, Z0), rx, ry, rz, nseg=44, nring=10, spike=1.4)
        # mullion ring at the dome springing
        for k in range(44):
            a = 2 * math.pi * k / 44
            b.bar(m["steel_dark"], (cx + (rx + 0.9) * math.cos(a), (ry + 0.9) * math.sin(a), 1.2),
                  (cx + (rx + 0.9) * math.cos(a), (ry + 0.9) * math.sin(a), 4.4), 0.12, segs=4)
    # link foyer between the domes + front arcade towards the road (-Y)
    b.box(m["stone"], (-6, -26, 0), (8, 26, 9))
    b.box(m["glass_clear"], (-5.5, -26.4, 0.8), (7.5, -25.9, 8.2))
    b.box(m["stone"], (-96, -44, 0), (92, -34, 0.5))          # entrance terrace
    for x in range(-90, 92, 9):
        b.box(m["steel"], (x - 0.2, -37.2, 0.5), (x + 0.2, -36.8, 5.6))       # canopy posts
    b.box(m["white"], (-92, -41, 5.6), (90, -35.4, 5.95))                      # canopy plate
    return b.finish(recalc=False, planar_uv=True)


# --------------------------------------------------------- attraction/landmark_big_wheel_xl
def ring_pt(R, a, y):
    return (R * math.cos(a), y, R * math.sin(a))


def big_wheel_xl():
    m = mb_mats()
    HUB, YW = 90.0, -8.0          # hub height, wheel-plane offset (towards the road)
    RIM, RC = 70.8, 72.4          # rim radius, capsule centre radius (outer edge = 75.0)
    # ---------------- rotor (hub at its origin)
    r = B("rotor")
    N = 96
    for side in (-1, 1):
        torus_y(r, m["steel"], (0, side * 2.2, 0), RIM, 0.9, segs=N, rings=8)
        torus_y(r, m["lights_rim"], (0, side * 2.2, 0), 0.35 + RIM - 1.2, 0.3, segs=N, rings=4)
    for k in range(N):
        a0, a1 = 2 * math.pi * k / N, 2 * math.pi * (k + 1) / N
        r.bar(m["steel"], ring_pt(RIM, a0, -2.2), ring_pt(RIM, a1, 2.2), 0.13, segs=5)
        r.bar(m["steel"], ring_pt(RIM, a0, 2.2), ring_pt(RIM, a1, -2.2), 0.13, segs=5)
        if k % 4 == 0:
            r.bar(m["steel"], ring_pt(RIM, a0, -2.2), ring_pt(RIM, a0, 2.2), 0.16, segs=6)
    # spokes: tangent "bicycle" lacing, two planes
    NS = 36
    for side in (-1, 1):
        for k in range(NS):
            a = 2 * math.pi * k / NS
            for sgn in (1, -1):
                ah = a + sgn * 0.55
                r.bar(m["lights_warm"], ring_pt(3.6, ah, side * 4.6), ring_pt(RIM, a + sgn * 0.0, side * 2.2), 0.06, segs=4)
    r.cylinder(m["steel_dark"], (0, -6.0, 0), 3.6, 12.0, segs=24, axis='Y')
    for s in (-1, 1):
        r.cylinder(m["steel"], (0, s * 4.6 - 0.35, 0), 4.7, 0.7, segs=32, axis='Y')
        r.cylinder(m["lights_hub"], (0, s * 6.1 - 0.15 * s - (0.3 if s < 0 else 0.0), 0), 2.3, 0.3, segs=24, axis='Y')
    # capsules, rigid to the rim
    NC = 28
    for k in range(NC):
        a = 2 * math.pi * k / NC
        rad = Vector((math.cos(a), 0, math.sin(a)))
        tan = Vector((-math.sin(a), 0, math.cos(a)))
        yv = Vector((0, 1, 0))
        c = rad * RC
        ax_t, ax_y, ax_r = 4.6, 2.5, 2.6
        nth, nps = 8, 14
        P = []
        for i in range(nth + 1):
            th = -math.pi / 2 + math.pi * i / nth
            ring = []
            for j in range(nps):
                ps = 2 * math.pi * j / nps
                ring.append(c + tan * (ax_t * math.sin(th)) + (yv * math.cos(ps) * ax_y + rad * math.sin(ps) * ax_r) * math.cos(th))
            P.append(ring)
        shell(r, m["glass_cap"], P, c)
        # white base pan (lower hemisphere, slightly proud)
        Pb = []
        for i in range(nth + 1):
            th = -math.pi / 2 + math.pi * i / nth
            ring = []
            for j in range(nps // 2 + 1):
                ps = math.pi + math.pi * j / (nps // 2)   # lower half (towards hub)
                ring.append(c + tan * (ax_t * 1.02 * math.sin(th)) + (yv * math.cos(ps) * ax_y * 1.03 + rad * math.sin(ps) * ax_r * 1.03) * math.cos(th))
            Pb.append(ring)
        shell(r, m["white"], Pb, c, closed_j=False)
        for s in (-1, 1):
            r.bar(m["steel_dark"], rad * (RIM + 0.4) + yv * s * 2.2, rad * (RC - 1.4) + yv * s * 1.1 + tan * 0.0, 0.14, segs=5)
    # ---------------- static: A-frame on the far side + rear stay + footings
    s = B("landmark_big_wheel_xl")
    for sx in (-1, 1):
        s.bar(m["steel"], (sx * 29, 16, 0.5), (sx * 3.2, 2.0, HUB - 3), 1.35, segs=10)
        s.box(m["concrete"], (sx * 29 - 3, 13, 0), (sx * 29 + 3, 19, 1.2))
        for z in (28, 52, 72):
            t = z / (HUB - 3)
            xa = sx * (29 + (3.2 - 29) * t); ya = 16 + (2.0 - 16) * t
    for z in (28, 52, 72):
        t = z / (HUB - 3)
        x = 29 + (3.2 - 29) * t; y = 16 + (2.0 - 16) * t
        s.bar(m["steel"], (-x, y, z), (x, y, z), 0.55, segs=6)
    s.bar(m["steel"], (0, 20, 0.5), (0, 3.5, HUB - 6), 0.9, segs=8)
    s.box(m["concrete"], (-3, 17.5, 0), (3, 22.5, 1.2))
    s.box(m["steel_dark"], (-4.5, -1.6, HUB - 5), (4.5, 4.5, HUB + 5))          # bearing housing
    s.cylinder(m["steel_dark"], (0, -3.5, HUB), 2.0, 2.5, segs=16, axis='Y')    # axle stub
    # boarding platform under the wheel's lowest point
    s.box(m["concrete"], (-14, YW - 6, 0), (14, YW + 6, 3.4))
    s.box(m["steel_dark"], (-14.2, YW - 6.2, 3.4), (14.2, YW + 6.2, 3.7))
    so = s.finish(recalc=True)
    ro = r.finish(recalc=False)
    ro.parent = so
    ro.location = (0, YW, HUB)
    return so


# --------------------------------------------------------- building/building_wheel_terminal
def wheel_terminal():
    m = mb_mats()
    b = B("building_wheel_terminal")
    L, D = 64.0, 30.0
    b.box(m["stone"], (-L / 2, -D / 2, 0), (L / 2, D / 2, 0.9))                         # podium
    # ground-floor lobby: recessed glass between columns
    b.box(m["glass_clear"], (-28, -11.5, 0.9), (28, -11.3, 8.6))
    b.box(m["stone"], (-28, -11.5, 0.9), (28, 11.5, 1.0))
    b.box(m["stone"], (-28, 11.3, 0.9), (28, 11.5, 8.6))                                 # rear wall
    for sx in (-28, 28):
        b.box(m["stone"], (sx - 0.15 + (0 if sx < 0 else 0), -11.5, 0.9), (sx + 0.15, 11.5, 8.6))
    for x in range(-26, 27, 6):
        b.cylinder(m["steel"], (x, -13.6, 0.9), 0.3, 8.0, segs=10)                       # colonnade
    # canopy
    b.box(m["white"], (-30, -15.0, 8.6), (30, -10.5, 9.1))
    # upper volume, cantilevered towards the road
    b.box(m["glass_teal"], (-26, -13.5, 9.1), (30, 11.5, 19.0))
    for x in range(-26, 31, 4):
        b.box(m["white"], (x - 0.12, -14.2, 9.1), (x + 0.12, -13.4, 19.0))               # vertical fins
    b.box(m["white"], (-26.4, -14.2, 9.1), (30.4, -13.4, 9.6))                           # sill band
    b.box(m["white"], (-26.4, -14.2, 18.5), (30.4, -13.4, 19.2))                         # head band
    # sloped roof sheet (rises 2.5 m towards the rear) + overhanging fascia
    prof = [(-16.0, 19.0), (-16.0, 19.6), (13.0, 22.0), (13.0, 21.4)]
    b.extrude_profile(m["roof"], prof, -28.5, 31.5, closed=True)
    b.box(m["white"], (-28.5, 13.0, 19.0), (31.5, 13.4, 22.0))
    # plant room + louvres on the roof
    b.box(m["steel"], (6, 3, 21.2), (20, 10, 24.0))
    b.box(m["steel_dark"], (6.2, 2.9, 22.0), (19.8, 3.1, 23.5))
    # entrance steps and signage band
    b.box(m["concrete"], (-8, -D / 2, 0), (8, -13.0, 0.9)); b.box(m["concrete"], (-9, -14.6, 0), (9, -D / 2 + 0.01, 0.45))
    # tail building towards the wheel (+X): single-storey ticketing hall
    b.box(m["stone"], (30.5, -9, 0.9), (L / 2, 9, 7.0))
    b.box(m["glass_clear"], (30.5, -9.2, 1.0), (L / 2 - 1, -8.9, 6.4))
    b.box(m["white"], (30.0, -10, 7.0), (L / 2 + 0.5, 10, 7.5))
    return b.finish()


# --------------------------------------------------------- building/building_pit_street
def pit_street():
    m = mb_mats()
    b = B("building_pit_street")
    L, D, H = 120.0, 32.0, 15.0
    y0, y1 = -D / 2, D / 2
    # ground floor: service bays recessed under the paddock-club cantilever
    b.box(m["pit_floor"], (-L / 2, y0, 0), (L / 2, y1, 0.2))
    b.box(m["concrete_dark"], (-L / 2, -9.0, 0.2), (L / 2, y1, 5.5))                    # rear mass
    n = 20
    pitch = L / n
    for i in range(n):
        xc = -L / 2 + pitch * (i + 0.5)
        b.box(m["door"], (xc - 2.6, -9.1, 0.2), (xc + 2.6, -8.95, 4.4))                    # roller door
        b.box(m["glass_pit"], (xc - 2.6, -9.12, 4.5), (xc + 2.6, -9.0, 5.3))               # transom
        b.box(m["white"], (xc + pitch / 2 - 0.35, -9.4, 0.2), (xc + pitch / 2 + 0.35, -9.0, 5.5))   # pier
    for x in np.arange(-L / 2, L / 2 + 0.1, pitch):
        b.cylinder(m["steel"], (x, y0 + 1.2, 0.2), 0.3, 5.4, segs=10)                    # arcade columns
    b.box(m["pit_floor"], (-L / 2, y0, 0.2), (L / 2, -9.0, 0.26))                      # lane apron
    # level 1: glazed hospitality band, fins
    b.box(m["white"], (-L / 2 - 0.4, y0, 5.5), (L / 2 + 0.4, y1, 6.1))                  # slab edge
    b.box(m["glass_blue"], (-L / 2, y0 + 1.0, 6.1), (L / 2, y1, 10.8))
    for x in np.arange(-L / 2, L / 2 + 0.1, 3.0):
        b.box(m["white"], (x - 0.1, y0 + 0.4, 6.1), (x + 0.1, y0 + 1.1, 10.8))
    b.box(m["white"], (-L / 2 - 0.4, y0, 10.8), (L / 2 + 0.4, y1, 11.4))
    # level 2: set-back terrace club with glass balustrade
    b.box(m["glass_teal"], (-52, -4.0, 11.4), (52, y1 - 3, 14.2))
    for x in np.arange(-L / 2, L / 2 + 0.1, 2.0):
        b.box(m["steel"], (x - 0.03, y0 + 0.4, 11.4), (x + 0.03, y0 + 0.43, 12.5))
    b.box(m["glass_clear"], (-L / 2, y0 + 0.38, 11.8), (L / 2, y0 + 0.42, 12.5))
    b.box(m["steel"], (-L / 2, y0 + 0.3, 12.5), (L / 2, y0 + 0.5, 12.6))
    # roof plate with front cantilever
    b.box(m["white"], (-54, -8.0, 14.2), (54, y1 - 1.5, 14.9))
    b.box(m["steel_dark"], (-54, -8.0, 14.9), (54, y1 - 1.5, 15.0))
    # stair/lift cores at both ends + rear service
    for sx in (-1, 1):
        b.box(m["concrete"], (sx * (L / 2 - 4) - 4, y1 - 8, 0.2), (sx * (L / 2 - 4) + 4, y1 + 1.5, 15.2))
        b.box(m["glass_clear"], (sx * (L / 2 - 4) - 2.5, y1 - 8.1, 3), (sx * (L / 2 - 4) + 2.5, y1 - 7.9, 14))
    for x in np.arange(-L / 2 + 8, L / 2 - 8, 6):
        b.box(m["steel_dark"], (x, y1 - 1.3, 14.9), (x + 3, y1 - 0.2, 16.2))               # roof plant
    # lighting masts + antenna
    for x in (-40, -13, 13, 40):
        b.bar(m["steel"], (x, -2.0, 15.0), (x, -2.0, 24.0), 0.14, segs=6)
        b.box(m["lights_hub"], (x - 1.0, -2.6, 24.0), (x + 1.0, -1.4, 24.6))
    return b.finish()


BUILD = {
    "landmark_twin_domes": ("building", twin_domes),
    "landmark_big_wheel_xl": ("attraction", big_wheel_xl),
    "building_wheel_terminal": ("building", wheel_terminal),
    "building_pit_street": ("building", pit_street),
}

def build(asset):
    clear_scene()
    ground()
    kind, fn = BUILD[asset]
    fn()
    return kind

def export(asset):
    kind = BUILD[asset][0]
    return export_tree(kind, asset)

if ASSET != "all":
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
