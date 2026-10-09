r"""Marina Bay batch B1 (Tier B) - three-tower resort with the SkyPark, and the lotus museum.

    ASSET = "all"   # or one of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_b1.py").read())
Original massing only: leaning-slab towers under a long ship-shaped sky deck, and a ten-petal
lotus; no signage or brand marks.
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())
try:
    ASSET
except NameError:
    ASSET = "all"


def cap(b, mat, ring, up=True):
    s = b.slot(mat)
    vs = [b.bm.verts.new(Vector(p)) for p in ring]
    f = b.bm.faces.new(vs); f.material_index = s
    _orient(f, Vector((0, 0, 1 if up else -1)))


def lens_ring(L, W, z, n=64, sk=0.0, xoff=0.0, tip=0.18):
    """Boat-like plan: pointed ends (tip), widest slightly east (sk)."""
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        x = (L / 2) * math.cos(a)
        w = (W / 2) * (abs(math.sin(a)) ** 0.8) * (1.0 - tip * max(0.0, math.cos(a)) ** 3 * 0.0)
        y = w * (1 if math.sin(a) >= 0 else -1) * (1.0 - 0.25 * (0.5 + 0.5 * math.cos(a) * sk))
        pts.append((x + xoff, y, z))
    return pts


# --------------------------------------------------------- attraction/landmark_three_towers_skypark
def three_towers_skypark():
    m = mb_mats(); b = B("landmark_three_towers_skypark")
    H, TH, LEAN = 194.0, 18.0, 15.0
    XS = (-105.0, 0.0, 105.0)
    # podium: low retail mall with canopy
    b.box(m["stone"], (-170, -48, 0), (170, 48, 1.0))
    b.box(m["glass_clear"], (-168, -46, 1.0), (168, 44, 13.0))
    b.box(m["white"], (-170, -48, 13.0), (170, 46, 14.5))
    for x in range(-164, 166, 12):
        b.cylinder(m["steel"], (x, -47.0, 1.0), 0.35, 12.0, segs=8)
    # three towers, each = two slabs leaning together; links high up
    for xc in XS:
        for sgn in (-1, 1):
            x0 = xc + (15.0 if sgn > 0 else -15.0 - TH)
            dx = -sgn * LEAN
            frustum(b, m["glass_grey"], (x0, x0 + TH, -26, 26), (x0 + dx, x0 + TH + dx, -26, 26), 14.5, H)
            for ex in (x0, x0 + TH):                                   # slim white corner fins on the end faces
                for ey in (-26.0, 26.0):
                    frustum(b, m["white"], (ex - 0.5, ex + 0.5, ey - 0.6, ey + 0.6), (ex + dx - 0.5, ex + dx + 0.5, ey - 0.6, ey + 0.6), 14.5, H)
        # sky-lobby links
        for zl, w in ((60.0, 9.0), (110.0, 6.0), (150.0, 4.0)):
            t = (zl - 14.5) / (H - 14.5)
            gap = 30.0 - 2 * LEAN * t
            if gap > 0.2:
                b.box(m["glass_clear"], (xc - gap / 2 - 0.2, -22, zl), (xc + gap / 2 + 0.2, 22, zl + w))
                b.box(m["white"], (xc - gap / 2 - 0.2, -22.5, zl + w), (xc + gap / 2 + 0.2, 22.5, zl + w + 0.5))
        pass
    # SkyPark: ship deck on the three tower tops
    L, W = 340.0, 42.0
    ZB, ZT = 184.0, 194.0
    top = lens_ring(L, W, ZT, xoff=18.0, sk=0.4)
    mid = lens_ring(L * 0.94, W * 0.7, ZT - 4.0, xoff=18.0, sk=0.4)
    bot = lens_ring(L * 0.7, W * 0.22, ZB, xoff=18.0, sk=0.4)
    ctr = (18.0, 0, ZT - 4)
    shell(b, m["white"], [bot, mid], ctr)
    shell(b, m["white"], [mid, top], ctr)
    cap(b, m["steel"], top, up=True)
    cap(b, m["steel"], bot, up=False)
    # deck features: infinity pool, gardens, bars, planter edges
    zd = ZT
    b.box(m["water"], (-10, 1.0, zd), (146, 8.8, zd + 0.25))                       # infinity pool strip
    b.box(m["concrete"], (-10.6, 0.5, zd), (146, 1.0, zd + 1.0))
    for (x0, x1, y0, y1) in ((-150, -20, -12, 12), (30, 150, -16, -3)):
        b.box(m["concrete_dark"], (x0, y0, zd), (x1, y1, zd + 0.5))
        for gx in np.arange(x0 + 4, x1 - 2, 9.0):
            b.ico(apex.material("mb_garden", (0.10, 0.26, 0.09), roughness=0.9), (gx, (y0 + y1) / 2, zd + 1.5), 2.6, subdiv=1, scale=(1, 1, 0.8), jitter=0.12, seed=int(gx))
    b.box(m["glass_clear"], (-165, -6, zd), (-150, 6, zd + 6.0))                     # west club pavilion
    b.box(m["white"], (-166, -7, zd + 6.0), (-149, 7, zd + 6.5))
    b.box(m["glass_clear"], (150, -8, zd), (172, 8, zd + 5.0))                       # east lookout / bow
    b.box(m["white"], (149, -9, zd + 5.0), (173, 9, zd + 5.5))
    return b.finish(recalc=False)


# --------------------------------------------------------- attraction/landmark_lotus_museum
def lotus_museum():
    m = mb_mats(); b = B("landmark_lotus_museum")
    # base: ring podium, glass band, central basin
    prism_ngon(b, m["stone"], ellipse_pts(0, 0, 27, 24, 56), 0, 1.0)
    prism_ngon(b, m["glass_clear"], ellipse_pts(0, 0, 25.5, 22.5, 56), 1.0, 7.0)
    prism_ngon(b, m["white"], ellipse_pts(0, 0, 26.5, 23.5, 56), 7.0, 8.0)
    prism_ngon(b, m["water"], ellipse_pts(0, 0, 9.0, 9.0, 36), 8.0, 8.4)
    def petal(phi, r0, z0, r1, z1, r2, z2, w0, w2, t0, t2):
        secs = []
        N = 7
        cph, sph = math.cos(phi), math.sin(phi)
        for i in range(N + 1):
            s = i / N
            r = r0 + (r1 - r0) * 2 * s * (1 - s) + (r2 - r0) * s * s       # quad bezier in (r, z)
            z = z0 + (z1 - z0) * 2 * s * (1 - s) + (z2 - z0) * s * s
            # derivative
            dr = 2 * (1 - s) * (r1 - r0) + 2 * s * (r2 - r1)
            dz = 2 * (1 - s) * (z1 - z0) + 2 * s * (z2 - z1)
            d = Vector((dr * cph, dr * sph, dz)).normalized()
            tn = Vector((-sph, cph, 0))
            nn = d.cross(tn).normalized()
            w = w0 + (w2 - w0) * s ** 1.3
            t = t0 + (t2 - t0) * s
            c = Vector((r * cph, r * sph, z))
            cup = w * 0.16
            o = lambda u: c + tn * (u * w / 2) + nn * (t / 2 + cup * (1 - u * u))
            i_ = lambda u: c + tn * (u * w / 2) + nn * (-t / 2 + cup * (1 - u * u))
            secs.append([o(-1), o(0), o(1), i_(1), i_(0), i_(-1)])
        b.loft(m["white"], secs, closed=True, cap_ends=True)
    for k in range(10):                                                              # outer petals
        petal(2 * math.pi * k / 10 + 0.15, 10.0, 8.0, 15.0, 24.0, 25.0, 40.0, 9.0, 1.6, 1.6, 0.4)
    for k in range(10):                                                              # inner, shorter, steeper petals
        petal(2 * math.pi * (k + 0.5) / 10 + 0.15, 8.0, 8.0, 10.0, 20.0, 14.5, 34.0, 6.5, 1.0, 1.8, 0.4)
    # skylight dish: inverted cone in the middle
    b.cone(m["glass_clear"], (0, 0, 8.4), 7.0, 7.0, 2.2, segs=24, cap=False)
    return b.finish(recalc=True)


BUILD = {
    "landmark_three_towers_skypark": ("attraction", three_towers_skypark),
    "landmark_lotus_museum": ("attraction", lotus_museum),
}

def build(asset):
    clear_scene(); ground(500); kind, fn = BUILD[asset]; fn(); return kind

def export(asset):
    return export_tree(BUILD[asset][0], asset)

if ASSET != "all":
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
