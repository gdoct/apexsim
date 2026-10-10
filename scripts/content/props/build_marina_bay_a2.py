r"""Marina Bay batch A2 - bridges: steel through-arch, wide deck parapets, viaduct span, double-helix footbridge.

    ASSET = "all"   # or one of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_a2.py").read())
Bridge convention (docs/content/props.md section 2): road along X, span across Y (authored for a 15 m road,
supports 1.5 m off each edge; the importer scales local Y only).
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())
try:
    ASSET
except NameError:
    ASSET = "all"


def lamp_twin(b, m, x, y, h=9.0, reach=3.2, dirn=-1):
    """Cast pole with a swan-neck arm over the carriageway (dirn: -1 arm towards -Y)."""
    b.cylinder(m["steel_dark"], (x, y, 0), 0.22, h, segs=8)
    b.cylinder(m["steel_dark"], (x, y, 0), 0.35, 0.9, segs=8)
    b.bar(m["steel_dark"], (x, y, h - 0.4), (x, y + dirn * reach, h + 0.35), 0.1, segs=6)
    b.box(m["lights_hub"], (x - 0.45, y + dirn * reach - 0.3, h + 0.2), (x + 0.45, y + dirn * reach + 0.3, h + 0.35))


def pylon(b, m, x, y, h=3.4, w=1.3):
    b.box(m["stone"], (x - w / 2, y - w / 2, 0), (x + w / 2, y + w / 2, h))
    b.box(m["stone"], (x - w / 2 - 0.12, y - w / 2 - 0.12, h), (x + w / 2 + 0.12, y + w / 2 + 0.12, h + 0.22))
    b.ico(m["lights_hub"], (x, y, h + 0.65), 0.38, subdiv=1)


# --------------------------------------------------------- bridge/bridge_arch_steel
def bridge_arch_steel():
    m = mb_mats()
    b = B("bridge_arch_steel")
    L, YS, RISE, Z0 = 30.0, 9.6, 11.0, 1.0
    zc = lambda x: Z0 + RISE * (1 - (x / (L / 2)) ** 2)
    N = 20
    for s in (-1, 1):
        # deck edge plate girder with rivet band and kerb
        b.box(m["steel"], (-L / 2, s * 9.7 - 0.5, -1.4), (L / 2, s * 9.7 + 0.5, 0.9))
        b.box(m["concrete"], (-L / 2, s * 9.7 - 0.65, 0.9), (L / 2, s * 9.7 + 0.65, 1.05))
        # arch: two chords + lattice, hangers
        for i in range(N):
            x0, x1 = -L / 2 + L * i / N, -L / 2 + L * (i + 1) / N
            for dz in (0.0, -1.1):
                b.bar(m["white"], (x0, s * YS, zc(x0) + dz + 0.55), (x1, s * YS, zc(x1) + dz + 0.55), 0.3, segs=10)
            if i % 2 == 0:
                b.bar(m["white"], (x0, s * YS, zc(x0) - 0.55), (x1, s * YS, zc(x1) + 0.55 - 1.1), 0.09, segs=5)
            else:
                b.bar(m["white"], (x0, s * YS, zc(x0) + 0.55 - 1.1), (x1, s * YS, zc(x1) - 0.55), 0.09, segs=5)
        for i in range(0, N + 1):
            x = -L / 2 + L * i / N
            if 0 < i < N:
                b.bar(m["steel"], (x, s * YS, 0.9), (x, s * YS, zc(x) - 0.45), 0.065, segs=6)
        # parapet rail
        b.box(m["steel_dark"], (-L / 2, s * 10.15 - 0.04, 1.05), (L / 2, s * 10.15 + 0.04, 1.15))
        b.box(m["steel_dark"], (-L / 2, s * 10.15 - 0.04, 2.0), (L / 2, s * 10.15 + 0.04, 2.1))
        for i in range(0, 31):
            x = -L / 2 + i
            b.box(m["steel_dark"], (x - 0.03, s * 10.15 - 0.03, 1.05), (x + 0.03, s * 10.15 + 0.03, 2.1))
    # top bracing struts where the arches are high enough (clearance > 6 m)
    for x in (-8.0, 0.0, 8.0):
        zz = zc(x) - 0.3
        b.bar(m["white"], (x, -YS, zz), (x, YS, zz), 0.16, segs=8)
        b.bar(m["white"], (x, -YS, zz - 0.9), (x, YS, zz - 0.9), 0.1, segs=6)
    for sx in (-L / 2, L / 2):
        for s in (-1, 1):
            b.box(m["white"], (sx - 0.3, s * YS - 0.35, 0.9), (sx + 0.3, s * YS + 0.35, Z0 + 1.2))   # skewback
    for sx in (-L / 2 + 0.7, L / 2 - 0.7):
        for s in (-1, 1):
            pylon(b, m, sx, s * 10.25, h=3.2, w=0.9)
    return b.finish()


# --------------------------------------------------------- bridge/bridge_deck_wide
def bridge_deck_wide():
    m = mb_mats()
    b = B("bridge_deck_wide")
    L = 40.0
    for s in (-1, 1):
        fas = [(s * 8.0, -2.4), (s * 11.4, -1.6), (s * 11.4, 0.15), (s * 8.0, 0.15)]
        b.extrude_profile(m["concrete_dark"], fas, -L / 2, L / 2, closed=True, flip=(s > 0))
        b.box(m["concrete"], (-L / 2, s * 10.4 - 1.2 if s > 0 else s * 10.4 - 1.2, 0.0), (L / 2, s * 10.4 + 1.2, 0.2))   # footway slab
        b.box(m["concrete"], (-L / 2, s * 11.0 - 0.25, 0.2), (L / 2, s * 11.0 + 0.25, 1.15))                         # parapet wall
        b.box(m["steel"], (-L / 2, s * 11.0 - 0.18, 1.15), (L / 2, s * 11.0 + 0.18, 1.35))                           # coping rail
        b.box(m["concrete"], (-L / 2, s * 8.3 - 0.15, 0.15), (L / 2, s * 8.3 + 0.15, 0.4))                           # kerb
        for x in (-L / 4, L / 4):
            lamp_twin(b, m, x, s * 10.2, h=9.0, reach=3.8, dirn=-s)
        for x in range(-int(L / 2), int(L / 2) + 1, 5):
            b.box(m["concrete"], (x - 0.1, s * 11.0 - 0.28, 0.2), (x + 0.1, s * 11.0 + 0.28, 1.15))                  # panel joint
        pylon(b, m, -L / 2 + 0.7, s * 11.0, h=2.4, w=1.2)
        pylon(b, m, L / 2 - 0.7, s * 11.0, h=2.4, w=1.2)
    # expansion joints across the deck
    b.box(m["steel_dark"], (-0.2, -8.0, 0.0), (0.2, 8.0, 0.02))
    return b.finish()


# --------------------------------------------------------- bridge/viaduct_deck
def viaduct_deck():
    m = mb_mats()
    b = B("viaduct_deck")
    L, ZB, ZT = 26.0, 9.6, 12.4
    # box-girder section (y, z), top cantilevers wider than the soffit
    sec = [(-10.2, ZT), (10.2, ZT), (10.2, ZT - 0.5), (7.6, ZB), (-7.6, ZB), (-10.2, ZT - 0.5)]
    b.extrude_profile(m["concrete"], sec, -L / 2, L / 2, closed=True)
    for s in (-1, 1):
        b.box(m["concrete"], (-L / 2, s * 9.9 - 0.3, ZT), (L / 2, s * 9.9 + 0.3, ZT + 1.1))        # jersey/parapet
        b.box(m["steel"], (-L / 2, s * 9.9 - 0.07, ZT + 1.1), (L / 2, s * 9.9 + 0.07, ZT + 1.35))
        for x in np.arange(-L / 2 + 1.5, L / 2, 3.0):
            b.box(m["concrete_dark"], (x - 0.1, s * 10.2 - 0.02, ZB + 0.9), (x + 0.1, s * 10.2 + 0.08, ZT - 0.5))   # fascia ribs
        # piers + cap beam
        b.box(m["concrete"], (-L / 2 + 2.5, s * 8.2 - 1.3, ZB - 1.0), (L / 2 - 2.5, s * 8.2 + 1.3, ZB))
        for x in (-6.5, 6.5):
            b.cylinder(m["concrete"], (x, s * 8.2, 0.0), 1.05, ZB - 1.0, segs=20)
            b.cylinder(m["concrete_dark"], (x, s * 8.2, 0.0), 1.2, 0.35, segs=20)
    for x in (-L / 2, L / 2):
        b.box(m["steel_dark"], (x - 0.02, -10.2, ZT), (x + 0.02, 10.2, ZT + 0.03))                    # joint
    return b.finish()


# --------------------------------------------------------- attraction/bridge_double_helix
def bridge_double_helix():
    m = mb_mats()
    b = B("bridge_double_helix")
    L, PITCH, ZC = 112.0, 14.0, 4.3
    NSEG = 24
    n_turns = L / PITCH
    steps = int(n_turns * NSEG)
    def hel(R, phase, sign):
        pts = []
        for i in range(steps + 1):
            s = L * i / steps
            th = sign * 2 * math.pi * s / PITCH + phase
            pts.append((-L / 2 + s, R * math.cos(th), ZC + R * math.sin(th)))
        return pts
    outer = [hel(4.1, 0.0, 1), hel(4.1, math.pi, 1)]
    inner = [hel(3.2, math.pi / 2, -1), hel(3.2, 3 * math.pi / 2, -1)]
    for pts in outer:
        for p, q in zip(pts, pts[1:]):
            b.bar(m["steel"], p, q, 0.36, segs=8)
    for pts in inner:
        for p, q in zip(pts, pts[1:]):
            b.bar(m["steel_dark"], p, q, 0.24, segs=6)
    # struts between the strands every half-pitch/6
    for i in range(0, steps + 1, 2):
        for oi in outer:
            ai = oi[i]
            for ii in inner:
                bi = ii[i]
                if (Vector(ai) - Vector(bi)).length < 2.2:
                    b.bar(m["steel"], ai, bi, 0.06, segs=4)
    # deck: dark steel plate + LED edge strips + handrail posts
    b.box(m["steel_dark"], (-L / 2, -2.4, 0.15), (L / 2, 2.4, 0.5))
    for s in (-1, 1):
        b.box(m["lit"], (-L / 2, s * 2.3 - 0.06, 0.5), (L / 2, s * 2.3 + 0.06, 0.58))
        b.box(m["steel"], (-L / 2, s * 2.45 - 0.04, 0.5), (L / 2, s * 2.45 + 0.04, 1.4))
    for i in range(0, 8):   # viewing-pod lamps
        pass
    # landings
    for sx in (-1, 1):
        b.box(m["concrete"], (sx * (L / 2) - 2.5 + (2.5 if sx < 0 else 0) - (2.5 if sx > 0 else 0) * 0, -3.4, 0), (sx * (L / 2) + (2.5 if sx > 0 else -2.5) * 0 + 2.5 * sx, 3.4, 0.5)) if False else None
    for sx in (-1, 1):
        x0, x1 = (L / 2, L / 2 + 3.0) if sx > 0 else (-L / 2 - 3.0, -L / 2)
        b.box(m["concrete"], (x0, -3.4, 0), (x1, 3.4, 0.55))
    return b.finish()


BUILD = {
    "bridge_arch_steel": ("bridge", bridge_arch_steel),
    "bridge_deck_wide": ("bridge", bridge_deck_wide),
    "viaduct_deck": ("bridge", viaduct_deck),
    "bridge_double_helix": ("attraction", bridge_double_helix),
}

def build(asset):
    clear_scene(); ground()
    kind, fn = BUILD[asset]; fn(); return kind

def export(asset):
    return export_tree(BUILD[asset][0], asset)

if ASSET != "all":
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
