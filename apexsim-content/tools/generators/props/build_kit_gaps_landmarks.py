"""Kit gaps 3, 5, 10, 11, 12 (docs/track-improvements.md "Kit gaps"): seven
landmark pieces that the circuit dossiers place and the kit had nothing for.

  building/wing_media_centre    a 40 m module of an elevated, wing-roofed media
                                and pit building; tiles seamlessly along X.
                                Road-facing deep kind: pivot on the road edge
                                (y = 0), the footprint reaching +Y.
  building/round_glass_tower    a ~30 m round glass control tower with a flared
                                glass observation head on a podium. Centred.
  building/arena_dome           a low elliptical domed ice arena, 120 x 90 m,
                                glazed facade ring under a panelled shell. Centred.
  misc/cauldron                 a ~50 m twisting, flaring steel plume tower with a
                                glowing bowl (`step_light_orange`). Centred.
  building/expo_pavilion        an expo-era pavilion: stacked, outward-sloping
                                chamfered floors behind a lattice of aluminium fins.
                                Centred.
  building/geodesic_dome        a 76 m, ~3/4 geodesic sphere: an outer triangle
                                layer, an inner hexagon layer and the diagonals
                                between them, glazed platforms inside. Centred.
  building/fuel_station_canopy  a roadside fuel station: canopy on four columns
                                over two pump islands, a shop behind, a price
                                pylon. Road-facing deep kind: pivot on the road
                                edge (y = 0), the footprint reaching +Y.

Headless:  blender -b --python tools/generators/props/build_kit_gaps_landmarks.py
(from apexsim-content/; APEX_ASSET=<key> builds one, default all). Writes
props/<kind>/<key>.{glb,blend} and previews props/_preview/<key>{,_b}.png.

Slots: the mb_* slots come from marina_common.mb_mats() so they are the same
materials the Marina Bay landmarks export under those names; KIT_SLOTS slots
(pit_wall, pit_door, pit_floor, pit_concrete, house_render, bridge_steel) come
from apex_tex.kit_material. `board_brand` is the brand slot the client fills:
the canopy fascia is UV'd one brand per ~3:1 rectangle (u 0..n along a face,
v 0..1 up it), reading left to right from outside. New slots: none.
Metres, Z up, +X along the track, the road on -Y.
"""
import importlib.util, math, os, sys

import bpy, bmesh
from mathutils import Vector

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or "tools/generators/props/x"))


def _load(name, fn):
    s = importlib.util.spec_from_file_location(name, os.path.join(_HERE, fn))
    m = importlib.util.module_from_spec(s)
    sys.modules[name] = m
    s.loader.exec_module(m)
    return m


mc = _load("marina_common", "marina_common.py")   # also loads apex + apex_tex
apex, tex = mc.apex, mc.tex
quad, tri = mc.quad, mc.tri

ASSET = os.environ.get("APEX_ASSET", "all")
PREVIEW = os.path.join(apex.PROPS_ROOT, "_preview")
TAU = 2 * math.pi


# ------------------------------------------------------------------ materials
def mats():
    """The slot set. mb_* exactly as the Marina Bay builders make them."""
    m = dict(mc.mb_mats())
    K = tex.kit_material
    m["pit_wall"] = K("pit_wall", (0.82, 0.82, 0.8))
    m["pit_door"] = K("pit_door", (0.7, 0.71, 0.72))
    m["pit_floor"] = K("pit_floor", (0.55, 0.55, 0.54))
    m["pit_concrete"] = K("pit_concrete", (0.6, 0.6, 0.58))
    m["render"] = K("house_render", (0.92, 0.9, 0.84))
    m["black"] = apex.material("mb_black", (0.03, 0.03, 0.035), roughness=0.6)
    m["brand"] = apex.image_material("board_brand", os.path.join(apex.PROPS_ROOT, "board", "brands", "apexsim.png"))
    m["flame"] = apex.material("step_light_orange", (1.0, 0.45, 0.05), roughness=0.4, emission=(1.0, 0.4, 0.03))
    return m


# ------------------------------------------------------------------ geometry
def revolve(b, mat, prof, segs, sx=1.0, sy=1.0, c=(0.0, 0.0), v_len=False, a0=0.0):
    """Surface of revolution of a (r, z) polyline about the vertical through c,
    x scaled by sx and y by sy (an ellipse when they differ). The outside is
    on the right of the walk (an outer wall walked upward faces out, a roof
    walked inward faces up, a soffit walked outward faces down). Explicit UVs:
    u = arc length, v = z (or length along the profile with v_len)."""
    vs = [0.0]
    for k in range(1, len(prof)):
        vs.append(vs[-1] + math.hypot(prof[k][0] - prof[k - 1][0], prof[k][1] - prof[k - 1][1]))
    kk = 0.5 * (sx + sy)

    def P(a, r, z):
        return (c[0] + sx * r * math.cos(a), c[1] + sy * r * math.sin(a), z)

    for k in range(len(prof) - 1):
        (r0, z0), (r1, z1) = prof[k], prof[k + 1]
        rm = max(0.5 * (r0 + r1), 1e-3) * kk
        v0, v1 = (vs[k], vs[k + 1]) if v_len else (z0, z1)
        for i in range(segs):
            aa, ab = a0 + TAU * i / segs, a0 + TAU * (i + 1) / segs
            ua, ub = aa * rm, ab * rm
            if r0 < 1e-6:
                b.quad_uv(mat, [P(aa, 0, z0), P(ab, r1, z1), P(aa, r1, z1)], [(ua, v0), (ub, v1), (ua, v1)])
            elif r1 < 1e-6:
                b.quad_uv(mat, [P(aa, r0, z0), P(ab, r0, z0), P(aa, 0, z1)], [(ua, v0), (ub, v0), (ua, v1)])
            else:
                b.quad_uv(mat, [P(aa, r0, z0), P(ab, r0, z0), P(ab, r1, z1), P(aa, r1, z1)],
                          [(ua, v0), (ub, v0), (ub, v1), (ua, v1)])


def hexa(b, mat, c8):
    """A convex hexahedron from 8 corners: c8[0:4] one face ring, c8[4:8] the
    opposite ring in the same order. Faces wound away from the centroid."""
    c8 = [Vector(p) for p in c8]
    cen = sum(c8, Vector()) / 8
    a, d = c8[:4], c8[4:]
    faces = [a, d] + [[a[i], a[(i + 1) % 4], d[(i + 1) % 4], d[i]] for i in range(4)]
    for f in faces:
        fc = sum(f, Vector()) / 4
        quad(b, mat, *f, out=fc - cen)


def plate(b, mat, p0, p1, p2, p3, n, t):
    """Quad p0..p3 thickened by t along n (both ways)."""
    n = Vector(n).normalized() * (t / 2)
    pts = [Vector(p) for p in (p0, p1, p2, p3)]
    hexa(b, mat, [p - n for p in pts] + [p + n for p in pts])


def octo(hx, hy, c, cx=0.0, cy=0.0):
    """Chamfered rectangle, counter-clockwise from above."""
    return [(cx + hx - c, cy - hy), (cx + hx, cy - hy + c), (cx + hx, cy + hy - c), (cx + hx - c, cy + hy),
            (cx - hx + c, cy + hy), (cx - hx, cy + hy - c), (cx - hx, cy - hy + c), (cx - hx + c, cy - hy)]


def ring_wall(b, mat, lo, hi, z0, z1, cen=(0.0, 0.0)):
    """Walls between two plan rings (same count, CCW) at z0 and z1, facing out."""
    n = len(lo)
    for i in range(n):
        j = (i + 1) % n
        p = [(lo[i][0], lo[i][1], z0), (lo[j][0], lo[j][1], z0), (hi[j][0], hi[j][1], z1), (hi[i][0], hi[i][1], z1)]
        mx = sum(q[0] for q in p) / 4 - cen[0]
        my = sum(q[1] for q in p) / 4 - cen[1]
        quad(b, mat, *p, out=(mx, my, 0))


def done(b):
    """Weld everything (UVs live on the loops) and finish without a normal
    recalc: every face here is wound explicitly."""
    bmesh.ops.remove_doubles(b.bm, verts=b.bm.verts[:], dist=1e-4)
    return b.finish(planar_uv=True, recalc=False)


# ======================================================== building/wing_media_centre
WING_L = 40.0


def wing_profile(n=24):
    """Aerofoil roof section in (y, z): NACA-style thickness on a cambered
    chord from the rounded back edge (y = 29) to a sharp edge at the road
    side (y = 0.3) that sweeps up. Returns (upper, lower), both back -> front."""
    back, front = Vector((29.0, 23.2)), Vector((0.3, 27.4))
    d = front - back
    c = d.length
    d.normalize()
    nrm = Vector((-d.y, d.x))
    if nrm.y < 0:
        nrm = -nrm
    t, m, p = 0.11, 0.07, 0.45
    up, lo = [], []
    for i in range(n + 1):
        x = 0.5 * (1 - math.cos(math.pi * i / n))
        yt = 5 * t * (0.2969 * math.sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x ** 3 - 0.1036 * x ** 4)
        yc = m / p ** 2 * (2 * p * x - x * x) if x < p else m / (1 - p) ** 2 * ((1 - 2 * p) + 2 * p * x - x * x)
        base = back + d * (x * c)
        up.append(tuple(base + nrm * ((yc + yt) * c)))
        lo.append(tuple(base + nrm * ((yc - yt) * c)))
    return up, lo


def wing_media_centre():
    m = mats()
    b = apex.Builder("wing_media_centre")
    X0, X1 = -WING_L / 2, WING_L / 2
    XS = [X0 + 2.0 + 4.0 * k for k in range(10)]   # 4 m bays, periodic over 40 m

    # Ground level behind the garages (they are 14.2 m deep): a clad core
    # with service doors, carrying the deck that cantilevers over them.
    b.box(m["pit_wall"], (X0, 15.0, 0.0), (X1, 27.5, 11.0))
    for x in (-16.5, 7.2):
        b.front_quad(m["pit_door"], x, x + 5.0, 0.0, 4.6, 14.98)
    b.box(m["concrete_dark"], (X0, 14.6, 0.0), (X1, 15.0, 0.4))
    for x in XS[::2]:   # pilasters every 8 m on the core face
        b.box(m["concrete"], (x - 0.4, 14.5, 0.0), (x + 0.4, 15.0, 11.0))
    # Deck: transfer slab + downstand edge beam at the road end.
    b.box(m["concrete"], (X0, 2.0, 11.0), (X1, 27.8, 12.0))
    b.box(m["white"], (X0, 1.6, 10.4), (X1, 2.4, 12.2))
    # Steel brackets under the cantilever, on the bay grid.
    for x in XS[::2]:
        plate(b, m["steel_dark"], (x, 15.0, 7.0), (x, 15.0, 11.0), (x, 2.6, 11.0), (x, 2.6, 10.4), n=(1, 0, 0), t=0.3)
    # Balcony rail on the deck's front.
    b.box(m["glass_pit"], (X0, 1.9, 12.2), (X1, 1.96, 13.2))
    b.box(m["steel"], (X0, 1.85, 13.2), (X1, 2.05, 13.32))

    # Two glazed media floors, the glass leaning out toward the road at the top.
    floors = [(12.0, 16.0, 4.6, 3.2), (16.6, 20.6, 4.0, 2.6)]   # z0, z1, y at z0, y at z1
    for z0, z1, yb, yt in floors:
        quad(b, m["glass_blue"], (X0, yb, z0), (X1, yb, z0), (X1, yt, z1), (X0, yt, z1), out=(0, -1, (yb - yt) / (z1 - z0)))
        # mullion fins on the bay grid, transoms at mid-height
        for x in XS + [X0 + 4.0 * k for k in range(10)]:
            b.bar(m["alu"], (x, yb - 0.12, z0), (x, yt - 0.12, z1), 0.07, segs=4)
        zm = (z0 + z1) / 2
        ym = (yb + yt) / 2 - 0.1
        b.bar(m["alu"], (X0, ym, zm), (X1, ym, zm), 0.05, segs=4)
        # end walls hidden inside a row, closing the end of one
        for x, s in ((X0 + 0.01, -1), (X1 - 0.01, 1)):
            quad(b, m["white"], (x, yb, z0), (x, 27.5, z0), (x, 27.5, z1), (x, yt, z1), out=(s, 0, 0))
    # floor slabs between and over the floors
    b.box(m["white"], (X0, 2.3, 16.0), (X1, 27.8, 16.6))
    b.box(m["white"], (X0, 1.7, 20.6), (X1, 27.8, 21.3))
    # back facade (away from the road)
    b.front_quad(m["glass_grey"], X0, X1, 12.0, 20.6, 27.5).normal_flip()   # faces +Y
    # Wing roof on raking struts.
    up, lo = wing_profile()
    lo_z = lambda y: min(lo, key=lambda q: abs(q[0] - y))[1]
    for x in XS[1::2]:
        for y in (5.0, 15.0, 25.0):
            zt = lo_z(y) + 0.1
            b.bar(m["steel"], (x - 0.8, y, 21.3), (x, y, zt), 0.18, segs=8)
            b.bar(m["steel"], (x + 0.8, y, 21.3), (x, y, zt), 0.18, segs=8)
    # roof skin: upper surface white, soffit aluminium, caps at the module ends
    for x0, x1 in ((X0, X1),):
        for i in range(len(up) - 1):
            (ya, za), (yb_, zb) = up[i], up[i + 1]
            quad(b, m["white"], (x0, ya, za), (x1, ya, za), (x1, yb_, zb), (x0, yb_, zb), out=(0, (zb - za), -(yb_ - ya)))
            (ya, za), (yb_, zb) = lo[i], lo[i + 1]
            quad(b, m["alu"], (x0, ya, za), (x1, ya, za), (x1, yb_, zb), (x0, yb_, zb), out=(0, -(zb - za), (yb_ - ya)))
    for x, s in ((X0, -1), (X1, 1)):
        loop = [(x, y, z) for (y, z) in up] + [(x, y, z) for (y, z) in reversed(lo[1:-1])]
        f = b.bm.faces.new([b.bm.verts.new(p) for p in loop])
        f.material_index = b.slot(m["white"])
        mc._orient(f, Vector((s, 0, 0)))
    # soffit ribs following the underside, on the bay grid
    for x in XS:
        for i in range(len(lo) - 1):
            p0, p1 = lo[i], lo[i + 1]
            b.bar(m["steel_dark"], (x, p0[0], p0[1] - 0.18), (x, p1[0], p1[1] - 0.18), 0.12, segs=4)
    # roof plant and a lift overrun on the deck roof, set back from the road
    b.box(m["steel"], (X0 + 6.0, 19.0, 21.3), (X0 + 12.0, 25.0, 22.4))
    b.box(m["concrete"], (X1 - 9.0, 22.0, 21.3), (X1 - 5.0, 26.0, 22.6))
    return done(b)


# ======================================================== building/round_glass_tower
def round_glass_tower():
    m = mats()
    b = apex.Builder("round_glass_tower")
    S = 48
    # Podium: chamfered plinth, glass band, cap slab, entrance canopy on -Y.
    mc.prism_ngon(b, m["concrete"], octo(12.5, 12.5, 4.0), 0.0, 0.5)
    mc.prism_ngon(b, m["glass_clear"], octo(11.6, 11.6, 3.7), 0.5, 3.9)
    mc.prism_ngon(b, m["white"], octo(12.4, 12.4, 4.0), 3.9, 4.6)
    for x in [-9.0 + 2.25 * k for k in range(9)]:
        b.box(m["alu"], (x - 0.08, -11.75, 0.5), (x + 0.08, -11.6, 3.9))
    b.box(m["white"], (-4.0, -15.5, 3.0), (4.0, -12.4, 3.4))
    for x in (-3.6, 3.6):
        b.cylinder(m["steel"], (x, -15.0, 0.5), 0.15, 2.5, segs=8)
    # Stalk: glazed shaft round a concrete core, floor rings.
    R = 3.6
    revolve(b, m["glass_blue"], [(R, 4.6), (R, 19.0)], S)
    for z in (8.0, 12.0, 16.0):
        revolve(b, m["alu"], [(R, z), (R + 0.15, z), (R + 0.15, z + 0.3), (R, z + 0.3)], S)
    for k in range(8):
        a = TAU * (k + 0.5) / 8
        b.bar(m["alu"], (math.cos(a) * (R + 0.08), math.sin(a) * (R + 0.08), 4.6),
              (math.cos(a) * (R + 0.08), math.sin(a) * (R + 0.08), 19.0), 0.12, segs=4)
    # Head: a glazed flare under two leaning observation floors.
    HR0, HR1 = 9.0, 9.6
    revolve(b, m["glass_blue"], [(R, 19.0), (HR0 - 0.3, 22.6)], S)
    revolve(b, m["white"], [(HR0 - 0.3, 22.6), (HR0 + 0.2, 22.6), (HR0 + 0.2, 23.0), (HR0, 23.0)], S)
    zmid = 26.0
    rmid = HR0 + (HR1 - HR0) * (zmid - 23.0) / 6.0
    revolve(b, m["glass_clear"], [(HR0, 23.0), (rmid, zmid)], S)
    revolve(b, m["white"], [(rmid, zmid), (rmid + 0.25, zmid), (rmid + 0.25, zmid + 0.35), (rmid + 0.03, zmid + 0.35)], S)
    revolve(b, m["glass_clear"], [(rmid + 0.03, zmid + 0.35), (HR1, 29.0)], S)
    # roof: overhanging canopy ring, a shallow cone, a mast
    revolve(b, m["white"], [(HR1, 29.0), (HR1 + 0.9, 29.0), (HR1 + 0.9, 29.7), (HR1 - 0.4, 29.9), (2.0, 30.6), (0.0, 30.7)], S)
    for k in range(36):
        a = TAU * k / 36
        p0 = (math.cos(a) * (HR0 + 0.06), math.sin(a) * (HR0 + 0.06), 23.0)
        p1 = (math.cos(a) * (HR1 + 0.06), math.sin(a) * (HR1 + 0.06), 29.0)
        b.bar(m["alu"], p0, p1, 0.06, segs=4)
    b.cylinder(m["steel"], (0, 0, 30.6), 0.6, 1.4, segs=12)
    b.cylinder(m["steel"], (0, 0, 32.0), 0.18, 4.5, segs=8)
    revolve(b, m["steel_dark"], [(1.6, 31.8), (1.7, 31.8), (1.7, 32.0), (1.6, 32.0)], 16)
    return done(b)


# ======================================================== building/arena_dome
def arena_dome():
    m = mats()
    b = apex.Builder("arena_dome")
    S = 128
    R = 45.0
    sx = 60.0 / R
    rev = lambda mat, prof, segs=S: revolve(b, mat, prof, segs, sx=sx, sy=1.0)
    # plinth / concourse, glazed facade ring leaning out, eave, shell roof
    rev(m["concrete"], [(47.6, 0.0), (47.6, 1.2), (44.0, 1.2)])
    rev(m["glass_blue"], [(44.0, 1.2), (45.0, 11.0)])
    rev(m["white"], [(45.0, 11.0), (46.6, 11.0), (46.6, 12.3), (45.0, 13.0)])
    prof = []
    for i in range(19):
        t = (math.pi / 2) * i / 18
        prof.append((R * math.cos(t) ** 0.9, 13.0 + 17.0 * math.sin(t)))
    prof[-1] = (0.0, 30.0)
    rev(m["alu"], prof, S)
    # panel ribs over the shell, a crown ring
    for k in range(32):
        a = TAU * k / 32
        pts = [(sx * (r + 0.18) * math.cos(a), (r + 0.18) * math.sin(a), z + 0.18) for (r, z) in prof[:-3]]
        for p0, p1 in zip(pts, pts[1:]):
            b.bar(m["white"], p0, p1, 0.22, segs=4)
    rc, zc = prof[-3]
    rev(m["white"], [(rc + 0.6, zc - 0.2), (rc + 0.6, zc + 0.5), (0.0, zc + 0.9)], 48)
    # facade fins
    for k in range(96):
        a = TAU * (k + 0.5) / 96
        p0 = (sx * 44.25 * math.cos(a), 44.25 * math.sin(a), 1.2)
        p1 = (sx * 45.25 * math.cos(a), 45.25 * math.sin(a), 11.0)
        b.bar(m["alu"], p0, p1, 0.14, segs=4)
    # four entrance canopies (the road side is -Y)
    for ang in (0.0, math.pi / 2, math.pi, 3 * math.pi / 2):
        ca, sa = math.cos(ang), math.sin(ang)
        rx, ry = sx * 45.0 * ca, 45.0 * sa
        ux, uy = ca, sa                # outward
        vx, vy = -sa, ca               # along the facade
        corner = lambda o, w, z: (rx + ux * o + vx * w, ry + uy * o + vy * w, z)
        hexa(b, m["white"], [corner(-1.5, -9, 5.2), corner(7, -9, 5.2), corner(7, 9, 5.2), corner(-1.5, 9, 5.2),
                              corner(-1.5, -9, 6.0), corner(7, -9, 6.0), corner(7, 9, 6.0), corner(-1.5, 9, 6.0)])
        for w in (-8.0, 8.0):
            p = corner(6.3, w, 1.2)
            b.cylinder(m["steel"], p, 0.2, 4.0, segs=8)
    return done(b)


# ======================================================== misc/cauldron
CAULDRON_H = 48.0


def cauldron_section(h, n=24, scale=1.0):
    """The plume's cross-section at height h above its base: a teardrop that
    widens, twists and sways with height."""
    s = h / CAULDRON_H
    wx = (2.2 + 7.8 * s ** 3.2) * scale
    wy = (1.6 + 3.0 * s ** 3.0) * scale
    tw = 1.9 * s + 0.6 * s * s
    cx = 2.2 * math.sin(math.pi * s)
    cy = -1.0 * math.sin(math.pi * s) ** 2
    pts = []
    for i in range(n):
        a = TAU * i / n
        x = wx * math.cos(a)
        y = wy * math.sin(a) * (1.0 + 0.4 * math.cos(a))
        pts.append((cx + x * math.cos(tw) - y * math.sin(tw), cy + x * math.sin(tw) + y * math.cos(tw)))
    return pts, (cx, cy)


def cauldron_tip(h, i, n=24):
    """Height added at section point i: the teardrop's tip side rises into a
    plume over the top fifth, so the lip slants up like a feather."""
    s = h / CAULDRON_H
    return 7.0 * max(0.0, (s - 0.8) / 0.2) ** 2 * max(0.0, math.cos(TAU * i / n)) ** 2


def skin(b, mat, rings):
    """Quads between consecutive 3D rings (same count, counter-clockwise from
    above), wound so a tube walked upward faces out."""
    n = len(rings[0])
    for ra, rb in zip(rings, rings[1:]):
        for i in range(n):
            j = (i + 1) % n
            f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in (ra[i], ra[j], rb[j], rb[i])])
            f.material_index = b.slot(mat)


def cauldron():
    m = mats()
    b = apex.Builder("cauldron")
    Z0 = 1.6
    # plinth: two chamfered steps
    mc.prism_ngon(b, m["stone"], octo(6.5, 6.5, 2.4), 0.0, 1.0)
    mc.prism_ngon(b, m["concrete"], octo(4.6, 4.6, 1.7), 1.0, Z0)
    # the plume: steel skin from the plinth to the lip
    N, RINGS = 24, 40
    rings = []
    for k in range(RINGS + 1):
        h = CAULDRON_H * k / RINGS
        pts, _ = cauldron_section(h, N)
        rings.append([(x, y, Z0 - 0.2 + h + cauldron_tip(h, i, N)) for i, (x, y) in enumerate(pts)])
    skin(b, m["steel"], rings)
    bot = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in reversed(rings[0])])
    bot.material_index = b.slot(m["steel"])
    # the bowl, dropping from the lip; its centre glows
    top = Z0 - 0.2 + CAULDRON_H
    _, cen = cauldron_section(CAULDRON_H, N)
    lip = rings[-1]
    bowl = [lip]
    for sc, dz in ((0.82, 0.9), (0.6, 1.6), (0.35, 2.0), (0.12, 2.2)):
        bowl.append([(cen[0] + (p[0] - cen[0]) * sc, cen[1] + (p[1] - cen[1]) * sc, top - dz) for p in lip])
    skin(b, m["steel_dark"], bowl[:3])          # walked down and in -> faces up into the bowl
    skin(b, m["flame"], bowl[2:])
    f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in bowl[-1]])
    f.material_index = b.slot(m["flame"])
    mc._orient(f, Vector((0, 0, 1)))
    # feather ribs: the teardrop's tip and tail run up the plume, plus two flanks
    for idx in (0, N // 2, N // 4, 3 * N // 4):
        prev = None
        for k in range(0, RINGS + 1, 2):
            h = CAULDRON_H * k / RINGS
            pts, (cx, cy) = cauldron_section(h, N, scale=1.0)
            x, y = pts[idx]
            d = Vector((x - cx, y - cy, 0))
            d = d.normalized() * 0.14 if d.length > 1e-6 else d
            p = (x + d.x, y + d.y, Z0 - 0.2 + h + cauldron_tip(h, idx, N))
            if prev:
                b.bar(m["steel_dark"], prev, p, 0.16 if idx in (0, N // 2) else 0.1, segs=6)
            prev = p
    # flame tongues rising out of the bowl
    for t in range(6):
        a = TAU * t / 6 + 0.3
        r0 = 0.9 + 0.5 * (t % 2)
        base = Vector((cen[0] + 1.6 * math.cos(a), cen[1] + 1.2 * math.sin(a), top - 1.9))
        hgt = 5.5 + 2.0 * ((t * 7) % 3) / 2
        rings = []
        for k in range(7):
            s = k / 6
            rr = r0 * (1 - s) ** 1.2 + 0.05
            off = Vector((0.6 * math.sin(2.2 * s + a), 0.6 * math.cos(1.7 * s + a), hgt * s))
            c = base + off
            rings.append([(c.x + rr * math.cos(TAU * i / 6), c.y + rr * 0.7 * math.sin(TAU * i / 6), c.z) for i in range(6)])
        skin(b, m["flame"], rings)
        f = b.bm.faces.new([b.bm.verts.new(Vector(p)) for p in rings[-1]])
        f.material_index = b.slot(m["flame"])
        mc._orient(f, Vector((0, 0, 1)))
    return done(b)


# ======================================================== building/expo_pavilion
def expo_pavilion():
    m = mats()
    b = apex.Builder("expo_pavilion")
    HX, HY, C = 22.0, 15.0, 7.0
    plan = lambda o: octo(HX + o, HY + o, C + o * (2 - math.sqrt(2)))
    FH, NF = 5.0, 5
    ob = lambda k: 2.4 * (k - 1)          # floor k's offset at its foot (k = 1..5)
    env = lambda z: 1.8 + 0.48 * (z - 10.0)   # the stepped facade's outer envelope
    # plinth and the recessed glazed ground floor
    mc.prism_ngon(b, m["concrete_dark"], plan(1.5), 0.0, 0.4)
    ring_wall(b, m["glass_grey"], plan(-2.5), plan(-2.5), 0.4, FH)
    for p in plan(-1.0)[::1]:
        b.cylinder(m["concrete"], (p[0], p[1], 0.4), 0.45, FH - 0.4, segs=10)
    # stacked floors: each leans out 1.8 m over its height, the next starts wider
    for k in range(1, NF + 1):
        z0 = k * FH
        mc.prism_ngon(b, m["concrete"], plan(ob(k) + 0.25), z0, z0 + 0.6)
        ring_wall(b, m["glass_grey"], plan(ob(k)), plan(ob(k) + 1.8), z0 + 0.6, z0 + FH)
    ztop = (NF + 1) * FH
    mc.prism_ngon(b, m["white"], plan(ob(NF) + 2.0), ztop, ztop + 0.8)
    mc.prism_ngon(b, m["white"], plan(ob(NF) + 1.6), ztop + 0.8, ztop + 1.4)
    # roof crown with a glass band
    mc.prism_ngon(b, m["concrete"], plan(-5.0), ztop + 1.4, ztop + 2.0)
    ring_wall(b, m["glass_grey"], plan(-6.0), plan(-6.0), ztop + 2.0, ztop + 4.2)
    mc.prism_ngon(b, m["white"], plan(-5.4), ztop + 4.2, ztop + 4.9)
    # the fin lattice: plates normal to each side, horizontals at the outer edge
    Z_A, Z_B = 6.2, ztop - 0.2
    for i in range(8):
        j = (i + 1) % 8

        def side_pt(z, o, t):
            pl = plan(env(z) + o)
            a, c = Vector(pl[i]), Vector(pl[j])
            return a + (c - a) * t

        a0, c0 = Vector(plan(0)[i]), Vector(plan(0)[j])
        L = (c0 - a0).length
        along = (c0 - a0).normalized()
        nfin = max(1, int(L / 2.5))
        for f in range(nfin + 1):
            t = (f + 0.5) / (nfin + 1) if i % 2 else f / nfin
            if i % 2 == 0 and f in (0, nfin):
                continue
            p = [side_pt(Z_A, 1.0, t), side_pt(Z_A, 2.3, t), side_pt(Z_B, 2.3, t), side_pt(Z_B, 1.0, t)]
            p = [Vector((q.x, q.y, z)) for q, z in zip(p, (Z_A, Z_A, Z_B, Z_B))]
            plate(b, m["alu"], *p, n=(along.x, along.y, 0), t=0.16)
        for z in [Z_A + 0.0] + [Z_A + 2.95 * (r + 1) for r in range(8)]:
            if z > Z_B + 0.01:
                continue
            p0, p1 = side_pt(z, 2.25, 0.0), side_pt(z, 2.25, 1.0)
            b.bar(m["alu"], (p0.x, p0.y, z), (p1.x, p1.y, z), 0.1, segs=4)
    # an entrance canopy toward the road
    hexa(b, m["white"], [(-9, -HY - 9.0, 4.0), (9, -HY - 9.0, 4.0), (9, -HY + 2.4, 4.0), (-9, -HY + 2.4, 4.0),
                          (-9, -HY - 9.0, 4.6), (9, -HY - 9.0, 4.6), (9, -HY + 2.4, 4.6), (-9, -HY + 2.4, 4.6)])
    for x in (-8.0, 8.0):
        b.cylinder(m["steel"], (x, -HY - 8.2, 0.4), 0.18, 3.6, segs=8)
    return done(b)


# ======================================================== building/geodesic_dome
def geodesic_dome():
    m = mats()
    b = apex.Builder("geodesic_dome")
    R, ZC, DEPTH = 38.0, 24.0, 2.0
    tmp = bmesh.new()
    bmesh.ops.create_icosphere(tmp, subdivisions=4, radius=R)   # 1280 faces
    tmp.verts.index_update()
    tmp.faces.index_update()
    up = Vector((0, 0, ZC))
    V = [v.co.copy() + up for v in tmp.verts]
    cen = {f.index: (sum((v.co for v in f.verts), Vector()) / 3).normalized() * (R - DEPTH) + up for f in tmp.faces}
    ZCUT = 0.6

    def seg(mat, p0, p1, r, segs):
        """A bar clipped to the ground."""
        p0, p1 = Vector(p0), Vector(p1)
        if p0.z < ZCUT and p1.z < ZCUT:
            return
        if p0.z < ZCUT or p1.z < ZCUT:
            lo, hi = (p0, p1) if p0.z < p1.z else (p1, p0)
            t = (ZCUT - lo.z) / (hi.z - lo.z)
            lo = lo + (hi - lo) * t
            p0, p1 = lo, hi
        b.bar(mat, p0, p1, r, segs=segs)

    steel = m["steel"]
    for e in tmp.edges:
        seg(steel, V[e.verts[0].index], V[e.verts[1].index], 0.22, 4)          # outer triangles
        if len(e.link_faces) == 2:
            seg(steel, cen[e.link_faces[0].index], cen[e.link_faces[1].index], 0.15, 3)   # inner hexagons
    for f in tmp.faces:
        for v in f.verts:
            seg(steel, cen[f.index], V[v.index], 0.08, 3)                       # diagonals
    tmp.free()
    # ring beam where the sphere meets the ground
    rg = math.sqrt(R * R - (ZC - ZCUT) ** 2)
    revolve(b, m["concrete"], [(rg + 0.9, 0.0), (rg + 0.9, ZCUT + 0.4), (rg - 2.6, ZCUT + 0.4), (rg - 2.6, 0.0), (rg + 0.9, 0.0)], 64)
    # inside: a lift core and stacked glazed exhibition platforms
    b.box(m["concrete"], (6.0, 2.0, 0.0), (10.0, 6.0, 44.0))
    plats = [(-4.0, -2.0, 12.0, 9.0, 0.0, 9.0), (2.0, 4.0, 10.0, 8.0, 12.0, 20.0), (-6.0, 6.0, 9.0, 7.0, 21.0, 28.0)]
    for cx, cy, hx, hy, z0, z1 in plats:
        mc.prism_ngon(b, m["glass_clear"], octo(hx - 1.0, hy - 1.0, 2.0, cx, cy), z0, z1 - 0.8)
        mc.prism_ngon(b, m["white"], octo(hx, hy, 2.5, cx, cy), z1 - 0.8, z1)
    # open terraces between them
    mc.prism_ngon(b, m["white"], octo(14.0, 10.0, 3.0, -2.0, 0.0), 32.0, 32.6)
    b.box(m["steel"], (-16.0, -10.05, 32.6), (12.0, -9.95, 33.7))
    # entrance pavilion toward the road
    b.box(m["glass_clear"], (-6.0, -rg - 6.0, 0.0), (6.0, -rg + 2.0, 4.0))
    b.box(m["white"], (-7.0, -rg - 7.0, 4.0), (7.0, -rg + 2.0, 4.6))
    return done(b)


# ======================================================== building/fuel_station_canopy
def brand_face(b, mat, p0, p1, h, repeats_hint=3.0):
    """A vertical fascia face from p0 to p1 (at its bottom, read left to right
    from outside: p0 is the viewer's left), height h; one 3:1 brand per repeat.
    Wound so it faces the viewer."""
    p0, p1 = Vector(p0), Vector(p1)
    L = (p1 - p0).length
    n = max(1, round(L / (repeats_hint * h)))
    up = Vector((0, 0, h))
    return b.quad_uv(mat, [p0, p1, p1 + up, p0 + up], [(0, 0), (n, 0), (n, 1), (0, 1)])


def fuel_station_canopy():
    m = mats()
    b = apex.Builder("fuel_station_canopy")
    # forecourt slab
    b.box(m["pit_floor"], (-15.0, 0.5, 0.0), (15.0, 20.0, 0.06))
    # canopy: roof, soffit with light panels, branded fascia on all four faces
    CX, Y0, Y1, Z0, Z1 = 12.0, 3.0, 17.0, 5.0, 6.2
    quad(b, m["roof"], (-CX, Y0, Z1), (CX, Y0, Z1), (CX, Y1, Z1), (-CX, Y1, Z1), out=(0, 0, 1))
    quad(b, m["white"], (-CX, Y0, Z0), (CX, Y0, Z0), (CX, Y1, Z0), (-CX, Y1, Z0), out=(0, 0, -1))
    brand_face(b, m["brand"], (-CX, Y0, Z0), (CX, Y0, Z0), Z1 - Z0)    # road face: viewer looks +Y, right is +X
    brand_face(b, m["brand"], (CX, Y1, Z0), (-CX, Y1, Z0), Z1 - Z0)    # back
    brand_face(b, m["brand"], (-CX, Y1, Z0), (-CX, Y0, Z0), Z1 - Z0)   # -X end
    brand_face(b, m["brand"], (CX, Y0, Z0), (CX, Y1, Z0), Z1 - Z0)     # +X end
    for x in (-8.0, -2.6, 2.6, 8.0):
        for y in (6.0, 10.0, 14.0):
            quad(b, m["lit"], (x - 1.2, y - 0.4, Z0 - 0.02), (x + 1.2, y - 0.4, Z0 - 0.02),
                 (x + 1.2, y + 0.4, Z0 - 0.02), (x - 1.2, y + 0.4, Z0 - 0.02), out=(0, 0, -1))
    # pump islands and the four columns on them
    for y in (7.5, 12.5):
        b.box(m["pit_concrete"], (-6.5, y - 0.7, 0.06), (6.5, y + 0.7, 0.26))
        b.box(m["white"], (-6.6, y - 0.75, 0.06), (-6.0, y + 0.75, 0.3))
        for x in (-5.0, 5.0):
            b.box(m["white"], (x - 0.35, y - 0.35, 0.26), (x + 0.35, y + 0.35, Z0))
            b.box(m["steel_dark"], (x - 0.45, y - 0.45, 0.26), (x + 0.45, y + 0.45, 1.0))
        for x in (-2.0, 2.0):
            b.box(m["white"], (x - 0.55, y - 0.3, 0.26), (x + 0.55, y + 0.3, 2.2))
            b.box(m["steel_dark"], (x - 0.6, y - 0.35, 2.2), (x + 0.6, y + 0.35, 2.45))
            for s in (-1, 1):
                b.box(m["glass_pit"], (x - 0.35, y + s * 0.3 - 0.01, 1.4), (x + 0.35, y + s * 0.3 + 0.01, 1.9))
                b.box(m["black"], (x + 0.4, y + s * 0.31 - 0.02, 0.9), (x + 0.5, y + s * 0.31 + 0.02, 1.3))
                b.bar(m["black"], (x + 0.45, y + s * 0.33, 1.3), (x + 0.6, y + s * 0.33, 0.6), 0.025, segs=4)
    # shop: rendered box, glazed shopfront facing the road, one brand panel
    SX0, SX1, SY0, SY1, SH = -10.0, 10.0, 21.0, 31.0, 4.2
    b.box(m["render"], (SX0, SY0 + 0.3, 0.0), (SX1, SY1, SH))
    b.keep.discard(b.front_quad(m["glass_clear"], SX0 + 1.0, SX1 - 5.0, 0.3, 3.0, SY0 + 0.28))   # planar UVs (metres)
    b.box(m["steel_dark"], (SX1 - 4.2, SY0 + 0.2, 0.0), (SX1 - 2.0, SY0 + 0.3, 2.6))   # door
    b.box(m["white"], (SX0 - 0.3, SY0, 3.2), (SX1 + 0.3, SY0 + 0.3, SH + 0.5))       # fascia band
    brand_face(b, m["brand"], (-2.4, SY0 - 0.02, 3.4), (2.4, SY0 - 0.02, 3.4), 1.6)
    b.box(m["white"], (SX0 - 0.3, SY0, SH), (SX1 + 0.3, SY1 + 0.3, SH + 0.5))        # parapet cap
    for x, y in ((-6.0, 26.0), (-3.0, 27.5), (5.0, 27.0)):
        b.box(m["steel"], (x - 0.8, y - 0.6, SH + 0.5), (x + 0.8, y + 0.6, SH + 1.5))
    b.box(m["white"], (SX0 - 0.3, SY0 - 1.6, 3.0), (SX1 + 0.3, SY0, 3.2))             # shopfront canopy
    # price pylon by the road
    b.box(m["white"], (12.0, 1.4, 0.0), (14.4, 2.0, 6.0))
    b.box(m["steel_dark"], (12.2, 1.36, 1.0), (14.2, 2.04, 4.6))
    brand_face(b, m["brand"], (11.9, 1.38, 6.0), (14.5, 1.38, 6.0), 0.86)
    brand_face(b, m["brand"], (14.5, 2.02, 6.0), (11.9, 2.02, 6.0), 0.86)
    b.box(m["white"], (11.9, 1.4, 6.86), (14.5, 2.0, 7.1))
    b.box(m["white"], (11.85, 1.4, 6.0), (11.9, 2.0, 6.86))
    b.box(m["white"], (14.5, 1.4, 6.0), (14.55, 2.0, 6.86))
    return done(b)


# ------------------------------------------------------------------ driver
BUILD = {
    # key: (kind, builder, ground half-size, shots [(look_from, look_at, fov)])
    "wing_media_centre": ("building", wing_media_centre, 200,
                          [((-70, -95, 6), (0, 12, 14), 50), ((-55, -45, 42), (0, 14, 12), 55)]),
    "round_glass_tower": ("building", round_glass_tower, 150,
                          [((-20, -110, 2), (0, 0, 18), 40), ((-48, -48, 32), (0, 0, 16), 55)]),
    "arena_dome": ("building", arena_dome, 400,
                   [((-80, -260, 4), (0, 0, 12), 35), ((-130, -130, 80), (0, 0, 8), 45)]),
    "cauldron": ("misc", cauldron, 150,
                 [((-30, -150, 3), (0, 0, 27), 45), ((-70, -70, 50), (0, 0, 27), 55)]),
    "expo_pavilion": ("building", expo_pavilion, 250,
                      [((-60, -170, 4), (0, 0, 15), 32), ((-75, -75, 50), (0, 0, 12), 45)]),
    "geodesic_dome": ("building", geodesic_dome, 300,
                      [((-60, -230, 4), (0, 0, 30), 40), ((-110, -110, 70), (0, 0, 25), 50)]),
    "fuel_station_canopy": ("building", fuel_station_canopy, 120,
                            [((-22, -45, 2.0), (0, 12, 3.5), 55), ((-30, -22, 18), (0, 14, 2), 50)]),
}


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for me in list(bpy.data.meshes):
        if me.users == 0:
            bpy.data.meshes.remove(me)


def asset_stats(ob):
    me = ob.data
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    lo = [min(v.co[i] for v in me.vertices) for i in range(3)]
    hi = [max(v.co[i] for v in me.vertices) for i in range(3)]
    return {"tris": tris, "bbox_min": [round(x, 2) for x in lo], "bbox_max": [round(x, 2) for x in hi],
            "size": [round(hi[i] - lo[i], 2) for i in range(3)], "slots": [mm.name for mm in me.materials]}


def preview_shots(key, ob, ground, shots):
    sc = bpy.context.scene
    cam = bpy.data.objects.get("PreviewCam")
    if not cam:
        cam = bpy.data.objects.new("PreviewCam", bpy.data.cameras.new("PreviewCam"))
        sc.collection.objects.link(cam)
    cam.data.clip_start, cam.data.clip_end = 0.5, 6000.0
    mc.ground(ground)
    extra = []
    if key == "wing_media_centre":       # three modules in a row: the seams must vanish
        for dx in (-WING_L, WING_L):
            o = bpy.data.objects.new(key + "_tile", ob.data)
            o.location = (dx, 0, 0)
            sc.collection.objects.link(o)
            extra.append(o)
    mc.set_night(False)
    # back faces culled in the previews, so a face wound the wrong way shows as a hole
    culled = [mm for mm in ob.data.materials if mm and not mm.use_backface_culling]
    for mm in culled:
        mm.use_backface_culling = True
    out = []
    for n, (lf, la, fov) in enumerate(shots):
        p = os.path.join(PREVIEW, key + ("" if n == 0 else "_b") + ".png")
        apex.preview(p, look_from=lf, look_at=la, fov=fov)
        out.append(p)
    for mm in culled:
        mm.use_backface_culling = False
    mc.set_night(True)
    for o in extra:
        bpy.data.objects.remove(o, do_unlink=True)
    return out


def run(key):
    kind, fn, ground, shots = BUILD[key]
    clear()
    ob = fn()
    st = asset_stats(ob)
    glb, size = apex.export_one(kind, key)
    shots_out = preview_shots(key, ob, ground, shots)
    apex.save_blend(kind, key)
    print("ASSET", key, kind, st, "GLB", glb, size, "PREVIEWS", shots_out)


if __name__ == "__main__":
    apex.reset_scene()
    tex.reset_cache()
    for key in (BUILD if ASSET == "all" else [k.strip() for k in ASSET.split(",")]):
        run(key)
