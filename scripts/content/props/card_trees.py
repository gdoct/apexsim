r"""Leaf-card trees: the generator behind every tree in the kit but the palms
and the forest impostors (build_trees.py, build_near_trees.py).

A tree is a curved, tapered trunk and branches in `tree_bark`, and a crown
of alpha-MASKED, two-sided cards: broadleaf crowns are a handful of lobes
(one per main branch) shelled with leaf-cluster cards whose twig points
back at the branch, conifers are whorls of drooping frond cards, a poplar is
a spindle of upright clusters and a bush a low mound of them. Three things
make the cards read as foliage rather than as paper:

* **Atlases.** Each card material is a 2x2 atlas of four clusters of
  different brightness and hue; the shell takes the bright and warm ones,
  the inside of the crown the dark one, so a crown has depth before any
  light reaches it. The colour bleeds into the transparent texels, so the
  mips carry no black fringe.
* **Crown normals.** Every vertex's normal is the crown's (the lobe's and
  the overall envelope's outward direction, a little of the card's own and
  a little of the sky), exported as custom normals; a crown is shaded as one
  soft volume and not as two hundred flat quads.
* **Outward fronts.** A card's front always faces out of the crown, so the
  two-sided material's back-face flip agrees with the normals.

The slots are `tree_bark`, `tree_card_broadleaf`, `tree_card_autumn` and
`tree_card_conifer`. Materials are shared by name across the kind when the
kit is imported, so every GLB carrying one carries the same texture: build
all the trees together (both scripts, ASSET = "all") after changing one.
"""
import bpy, bmesh, math, os, random, sys, zlib, importlib.util
import numpy as np
from mathutils import Vector

_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")


def _load(name, rel):
    if name in sys.modules:
        return sys.modules[name]
    s = importlib.util.spec_from_file_location(name, os.path.join(_ROOT, rel))
    m = importlib.util.module_from_spec(s)
    sys.modules[name] = m
    s.loader.exec_module(m)
    return m


tex = _load("apex_tex", r"scripts\content\props\apex_tex.py")

CELL = 512            # one atlas cell; the atlas is 2 x 2 cells
UP = Vector((0.0, 0.0, 1.0))


def seed_of(key):
    """A seed that is the same in every process (str hash is not)."""
    return zlib.crc32(key.encode()) & 0xFFFF


# ------------------------------------------------------------------ painting
class Canvas:
    """One atlas cell: colour and coverage, painted shape by shape in its
    own bounding box. v = 0 is row 0, as Blender stores an image."""

    def __init__(self, n=CELL):
        self.n = n
        self.col = np.zeros((n, n, 3))
        self.a = np.zeros((n, n))
        u = (np.arange(n) + 0.5) / n
        self.X, self.Y = np.meshgrid(u, u)

    def _box(self, xs, ys, pad):
        n = self.n
        c0 = max(0, int((min(xs) - pad) * n))
        c1 = min(n, int((max(xs) + pad) * n) + 1)
        r0 = max(0, int((min(ys) - pad) * n))
        r1 = min(n, int((max(ys) + pad) * n) + 1)
        if c1 <= c0 or r1 <= r0:
            return None
        return (slice(r0, r1), slice(c0, c1))

    def leaf(self, bx, by, ang, length, width, rgb, light=1.0, rib=0.8):
        """A pointed leaf from its stalk end (bx, by) along `ang`: widest a
        little before the middle, a lit and a shaded half either side of a
        midrib, darker toward the rim."""
        ex, ey = math.cos(ang), math.sin(ang)
        tipx, tipy = bx + ex * length, by + ey * length
        box = self._box((bx, tipx), (by, tipy), width)
        if box is None:
            return
        X, Y = self.X[box], self.Y[box]
        dx, dy = X - bx, Y - by
        s = (dx * ex + dy * ey) / length
        t = (-dx * ey + dy * ex) / (0.5 * width)
        sc = np.clip(s, 0.0, 1.0)
        half = np.sin(np.pi * sc ** 0.85) ** 0.8
        m = (s >= 0.0) & (s <= 1.0) & (np.abs(t) <= half)
        if not m.any():
            return
        rim = np.clip(np.abs(t) / np.maximum(half, 1e-3), 0.0, 1.0)
        side = np.where(t * ex > 0.0, 1.07, 0.88)          # one half toward the light
        shade = light * side * (1.0 - 0.28 * rim ** 3) * (0.9 + 0.12 * sc)
        shade = np.where(np.abs(t) < 0.07, shade * rib, shade)
        c = np.clip(np.asarray(rgb)[None, None, :] * shade[..., None], 0.0, 1.0)
        self.col[box][m] = c[m]
        self.a[box][m] = 1.0

    def twig(self, x0, y0, x1, y1, w0, w1, rgb):
        box = self._box((x0, x1), (y0, y1), max(w0, w1))
        if box is None:
            return
        m = tex._blade(self.X[box], self.Y[box], x0, y0, x1, y1, w0, w1)
        self.col[box][m] = rgb
        self.a[box][m] = 1.0

    def finish(self):
        """Bleed the leaf colour into the transparent texels (normalised
        blur), so a mip mixes leaf with leaf and never with black."""
        a = self.a
        num = np.stack([tex.blur(self.col[..., i] * a, 6.0) for i in range(3)], -1)
        den = tex.blur(a, 6.0)[..., None]
        mean = (self.col * a[..., None]).sum((0, 1)) / max(a.sum(), 1.0)
        fill = np.where(den > 1e-3, num / np.maximum(den, 1e-3), mean[None, None, :])
        col = np.where(a[..., None] > 0.5, self.col, np.clip(fill, 0.0, 1.0))
        return col, a


def _atlas(cells):
    n = cells[0][0].shape[0]
    col = np.zeros((2 * n, 2 * n, 3))
    a = np.zeros((2 * n, 2 * n))
    for k, (c, al) in enumerate(cells):
        r0, c0 = (k // 2) * n, (k % 2) * n
        col[r0:r0 + n, c0:c0 + n] = c
        a[r0:r0 + n, c0:c0 + n] = al
    return col, a


def leaf_cluster(seed, palette, light=1.0, warm=0.0, leaf_len=(0.065, 0.10)):
    """A spray of broadleaf leaves, twig end at the bottom middle: a stem
    forking into side shoots, leaves alternating along every shoot and
    crowding at its tip, a layer of shaded leaves behind them. `palette`
    is sRGB leaf colours; the light falls from the top of the card."""
    rng = np.random.default_rng(seed)
    cv = Canvas()
    pal = [np.array(p) for p in palette]
    warm_rgb = np.array((1.12, 1.05, 0.72))

    def colour(k_light):
        g = pal[rng.integers(len(pal))] * rng.uniform(0.86, 1.12)
        if rng.random() < 0.15:                     # a leaf catching the sun
            g = g * 1.22 * warm_rgb
        g = g * (1.0 - warm) + g * warm_rgb * warm
        return np.clip(g * k_light * light, 0.0, 1.0)

    def leaf_at(bx, by, a, k_light, rib=0.8):
        L = rng.uniform(*leaf_len)
        cv.leaf(bx, by, a, L, L * rng.uniform(0.48, 0.62), colour(k_light), rib=rib)

    # the shoots: a stem up the middle, side shoots off it, each forking once
    bark = np.array((0.26, 0.20, 0.13)) * light
    shoots = []                                     # (x0, y0, x1, y1)
    x, y = 0.5 + rng.uniform(-0.02, 0.02), 0.02
    ang = math.pi / 2 + rng.uniform(-0.15, 0.15)
    for k in range(5):
        L = 0.17 - 0.016 * k
        nx, ny = x + L * math.cos(ang), y + L * math.sin(ang)
        cv.twig(x, y, nx, ny, 0.014 - 0.0018 * k, 0.012 - 0.0018 * k, bark)
        shoots.append((x, y, nx, ny))
        side = 1 if k % 2 else -1
        for s in ((side, -side) if k > 0 else (side,)):
            if k > 0 and s == -side and rng.random() < 0.25:
                continue
            sa = ang + s * rng.uniform(0.7, 1.15)
            sl = rng.uniform(0.26, 0.38) * (1.0 - 0.13 * k)
            sx, sy = nx + sl * math.cos(sa), ny + sl * math.sin(sa)
            cv.twig(nx, ny, sx, sy, 0.007, 0.003, bark)
            shoots.append((nx, ny, sx, sy))
            fa = sa - s * rng.uniform(0.4, 0.7)
            fl = sl * rng.uniform(0.4, 0.6)
            mx, my = (nx + sx) / 2, (ny + sy) / 2
            shoots.append((mx, my, mx + fl * math.cos(fa), my + fl * math.sin(fa)))
        x, y = nx, ny
        ang += rng.uniform(-0.2, 0.2) - 0.4 * (ang - math.pi / 2)
    # shaded leaves behind, along the shoots
    for (x0, y0, x1, y1) in shoots:
        sa = math.atan2(y1 - y0, x1 - x0)
        for t in np.linspace(0.3, 1.0, 4):
            leaf_at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, sa + rng.uniform(-1.6, 1.6),
                    0.6 + 0.25 * y0, rib=0.9)
    # front leaves: alternating along each shoot, a rosette at its tip
    for (x0, y0, x1, y1) in shoots:
        sa = math.atan2(y1 - y0, x1 - x0)
        n = 6
        for i in range(n):
            t = (i + 0.5) / n
            px, py = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            leaf_at(px, py, sa + (1 if i % 2 else -1) * rng.uniform(0.6, 1.1), 0.8 + 0.3 * py)
        for _ in range(3):
            leaf_at(x1, y1, sa + rng.uniform(-0.7, 0.7), 0.85 + 0.3 * y1)
    return cv.finish()


GREEN = ((0.20, 0.34, 0.09), (0.26, 0.40, 0.11), (0.31, 0.44, 0.14), (0.18, 0.31, 0.10))
AUTUMN = ((0.84, 0.60, 0.12), (0.82, 0.40, 0.08), (0.62, 0.18, 0.07), (0.50, 0.32, 0.12), (0.40, 0.42, 0.13))
SPRUCE = ((0.09, 0.22, 0.10), (0.11, 0.26, 0.12), (0.14, 0.30, 0.13), (0.10, 0.24, 0.12))
SPRUCE_TIP = (0.30, 0.46, 0.16)                     # the year's new growth

# atlas cells: 0 sunlit, 1 warm, 2 cool, 3 inside the crown
CELL_SUN, CELL_WARM, CELL_COOL, CELL_DARK = range(4)


def broadleaf_atlas(seed=31):
    return _atlas([
        leaf_cluster(seed, GREEN, light=1.12, warm=0.15),
        leaf_cluster(seed + 1, GREEN, light=1.02, warm=0.35),
        leaf_cluster(seed + 2, GREEN[:2] + GREEN[3:], light=0.96),
        leaf_cluster(seed + 3, GREEN, light=0.7),
    ])


def autumn_atlas(seed=61):
    return _atlas([
        leaf_cluster(seed, AUTUMN[:2], light=1.05),
        leaf_cluster(seed + 1, AUTUMN[1:3] + AUTUMN[:1], light=0.98),
        leaf_cluster(seed + 2, AUTUMN, light=0.95),
        leaf_cluster(seed + 3, AUTUMN[2:], light=0.7),
    ])


def spruce_frond(seed, greens, light, tips=0.35):
    """A spruce branch from above, trunk end at the bottom: a stem, side
    shoots angled forward (short at the trunk, longest two thirds out,
    short again at the tip), each shoot and its forks furred with needles,
    the light green of the new growth at the ends."""
    rng = np.random.default_rng(seed)
    cv = Canvas()
    gs = [np.array(g) * light for g in greens]
    tip_rgb = np.array(SPRUCE_TIP) * light
    bark = np.array((0.26, 0.19, 0.12)) * light

    def needles(x0, y0, x1, y1, dense=0.011, nl=(0.022, 0.036)):
        L = math.hypot(x1 - x0, y1 - y0)
        sa = math.atan2(y1 - y0, x1 - x0)
        n = max(2, int(L / dense))
        for i in range(n):
            t = (i + 0.5) / n
            px, py = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            g = gs[rng.integers(len(gs))] * rng.uniform(0.85, 1.12) * (0.82 + 0.3 * t)
            if t > 1.0 - tips * rng.uniform(0.6, 1.2):
                g = g * 0.4 + tip_rgb * 0.6 * rng.uniform(0.9, 1.1)
            for s in (-1, 1):
                a = sa + s * rng.uniform(0.5, 1.0)
                l = rng.uniform(*nl) * (1.0 - 0.35 * t)
                cv.twig(px, py, px + l * math.cos(a), py + l * math.sin(a), 0.010, 0.003,
                        np.clip(g * rng.uniform(0.9, 1.1), 0, 1))

    stem_x = 0.5 + rng.uniform(-0.02, 0.02)
    cv.twig(stem_x, 0.02, stem_x + rng.uniform(-0.03, 0.03), 0.97, 0.022, 0.007, bark)
    shoots = []
    n = 13
    for k in range(n):
        t = 0.05 + 0.9 * (k + rng.uniform(0.0, 0.6)) / n
        prof = math.sin(math.pi * min(1.0, (t + 0.08) / 1.02) ** 0.8)
        for s in (-1, 1):
            L = (0.40 * prof + 0.04) * rng.uniform(0.85, 1.05)
            a = math.pi / 2 - s * rng.uniform(0.75, 1.0)
            x0, y0 = stem_x, t
            x1, y1 = x0 + L * math.cos(a), min(0.98, y0 + L * math.sin(a))
            shoots.append((x0, y0, x1, y1))
            if L > 0.15:
                for f in (0.45, 0.7):
                    fx, fy = x0 + (x1 - x0) * f, y0 + (y1 - y0) * f
                    fa = a + rng.uniform(0.3, 0.6) * (1 if rng.random() < 0.5 else -1)
                    fl = L * rng.uniform(0.25, 0.4)
                    shoots.append((fx, fy, fx + fl * math.cos(fa), fy + fl * math.sin(fa)))
    for (x0, y0, x1, y1) in shoots:
        cv.twig(x0, y0, x1, y1, 0.006, 0.003, bark)
    for (x0, y0, x1, y1) in shoots:
        needles(x0, y0, x1, y1)
    needles(stem_x, 0.5, stem_x, 0.98, nl=(0.03, 0.045))
    # a dense core so the frond reads solid from a distance
    core = (tex.blur(cv.a, 3.0) > 0.55) & (cv.a < 0.5)
    cv.col[core] = gs[0] * 0.55
    cv.a[core] = 1.0
    return cv.finish()


def conifer_atlas(seed=37):
    return _atlas([
        spruce_frond(seed, SPRUCE, 1.12, tips=0.4),
        spruce_frond(seed + 1, SPRUCE, 1.0),
        spruce_frond(seed + 2, SPRUCE[:3], 0.92, tips=0.2),
        spruce_frond(seed + 3, SPRUCE, 0.68, tips=0.1),
    ])


def bark_texture(seed=3, rgb=(0.26, 0.20, 0.15)):
    """Furrowed bark, furrows along v (up the trunk), broken into plates,
    with lichen in the furrows. Tileable."""
    X, Y = tex.grid()
    warp = tex.fbm(seed, scales=(3, 6), weights=(0.6, 0.4))
    fine = tex.fbm(seed + 1, scales=(24, 48, 96), weights=(0.45, 0.35, 0.2))
    ridges = 0.5 + 0.5 * np.sin(2 * math.pi * (X * 7 + 0.9 * (warp - 0.5)))
    cracks = np.clip((np.sin(2 * math.pi * (Y * 9 + 2.0 * warp)) - 0.85) / 0.15, 0, 1) * (ridges > 0.45)
    height = np.clip(ridges ** 0.7 * 0.6 + fine * 0.4 - 0.35 * cracks, 0, 1)
    blotch = tex.fbm(seed + 2, scales=(2, 5), weights=(0.6, 0.4))
    base = np.array(rgb)[None, None, :]
    color = base * (0.5 + 0.75 * height[..., None]) * (0.85 + 0.3 * blotch[..., None])
    lichen = np.clip((tex.fbm(seed + 5, scales=(6, 12), weights=(0.6, 0.4)) - 0.62) * 4, 0, 1) * (1 - height)
    color = color * (1 - 0.6 * lichen[..., None]) + np.array((0.36, 0.40, 0.27)) * 0.6 * lichen[..., None]
    rough = np.clip(0.82 + 0.15 * (1 - height), 0, 1)
    return np.clip(color, 0, 1), rough, height, 3.0


_MATS = {}


def materials():
    """The four slots, made once per Blender session."""
    if not _MATS:
        _MATS["bark"] = tex.pbr_material("tree_bark", bark_texture(), tile_m=1.2)
        _MATS["broadleaf"] = tex.card_material("tree_card_broadleaf", broadleaf_atlas())
        _MATS["autumn"] = tex.card_material("tree_card_autumn", autumn_atlas())
        _MATS["conifer"] = tex.card_material("tree_card_conifer", conifer_atlas())
    return _MATS


# ------------------------------------------------------------------ geometry
def _cell_uv(cell, u, v):
    inset = 0.004
    u0, v0 = (cell % 2) * 0.5, (cell // 2) * 0.5
    return (u0 + inset + u * (0.5 - 2 * inset), v0 + inset + v * (0.5 - 2 * inset))


class TreeMesh:
    """One bmesh with named slots, UVs and a normal per face corner (set
    as custom normals when the mesh is made)."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.mats, self._slot = [], {}
        self.corner_normals = []          # per face, in creation order

    def slot(self, mat):
        if mat not in self._slot:
            self._slot[mat] = len(self.mats)
            self.mats.append(mat)
        return self._slot[mat]

    def face(self, mat, verts, uvs, normals):
        f = self.bm.faces.new(verts)
        f.material_index = self.slot(mat)
        f.smooth = True
        for l, uv in zip(f.loops, uvs):
            l[self.uv].uv = uv
        self.corner_normals.append([Vector(n).normalized() for n in normals])

    # -- wood
    def tube(self, mat, pts, radii, segs=7, tile=1.2):
        """A tapered tube through `pts` (parallel-transported frame),
        UVs in metres round and along it, normals radial."""
        pts = [Vector(p) for p in pts]
        tang = []
        for i in range(len(pts)):
            a = pts[max(i - 1, 0)]
            b = pts[min(i + 1, len(pts) - 1)]
            tang.append((b - a).normalized())
        ref = Vector((1, 0, 0)) if abs(tang[0].z) > 0.9 else UP
        side = tang[0].cross(ref).normalized()
        rings, along = [], 0.0
        for i, (p, t, r) in enumerate(zip(pts, tang, radii)):
            if i:
                along += (p - pts[i - 1]).length
                side = (side - t * side.dot(t)).normalized()
            fwd = t.cross(side).normalized()
            circ = 2 * math.pi * max(radii[0], 0.05)
            ring = []
            for k in range(segs + 1):                      # seam duplicated for the UV
                a = 2 * math.pi * k / segs
                n = side * math.cos(a) + fwd * math.sin(a)
                ring.append((self.bm.verts.new(p + n * r), n, (circ * k / segs / tile, along / tile)))
            rings.append(ring)
        for r0, r1 in zip(rings, rings[1:]):
            for k in range(segs):
                q = (r0[k], r0[k + 1], r1[k + 1], r1[k])
                self.face(mat, [x[0] for x in q], [x[2] for x in q], [x[1] for x in q])

    # -- foliage
    def card(self, mat, base, U, N, w, h, cell, normal_of, fold=0.18, rows=1, droop=None):
        """A card from its twig end `base` along U (length h, width w),
        front N, the sides folded back by `fold` of the width; `rows` = 2
        bends it once along its length by `droop` (a vector at the tip).
        `normal_of(p)` gives the crown's normal at a point."""
        U, N = Vector(U).normalized(), Vector(N).normalized()
        R = U.cross(N).normalized()
        N = R.cross(U).normalized()
        grid = []
        for j in range(rows + 1):
            v = j / rows
            bend = droop * (v * v) if droop is not None else Vector()
            row = []
            for i in range(3):
                u = i / 2
                p = Vector(base) + U * (h * v) + R * (w * (u - 0.5)) + bend
                if i != 1:
                    p -= N * (fold * w * 0.5)
                n = normal_of(p) + N * 0.3
                row.append((self.bm.verts.new(p), n, _cell_uv(cell, u, v)))
            grid.append(row)
        for j in range(rows):
            for i in range(2):
                q = (grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i])
                self.face(mat, [x[0] for x in q], [x[2] for x in q], [x[1] for x in q])

    def finish(self):
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.mats:
            me.materials.append(m)
        me.shade_smooth()
        loop_normals = [None] * len(me.loops)
        for poly, ns in zip(me.polygons, self.corner_normals):
            for k, li in enumerate(range(poly.loop_start, poly.loop_start + poly.loop_total)):
                loop_normals[li] = ns[k]
        me.normals_split_custom_set([tuple(n) for n in loop_normals])
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob


def _unit(rng):
    while True:
        v = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
        if 0.05 < v.length <= 1.0:
            return v.normalized()


def _bezier(a, b, c, n):
    return [a * (1 - t) ** 2 + b * 2 * t * (1 - t) + c * t * t for t in (k / n for k in range(n + 1))]


def _crown_normal(p, lobes, centre, radii, sky=0.25):
    """Outward from the crown's envelope and from the nearest lobe."""
    d = p - centre
    env = Vector((d.x / radii.x ** 2, d.y / radii.y ** 2, d.z / radii.z ** 2))
    env = env.normalized() if env.length > 1e-6 else UP
    lc, lr = min(lobes, key=lambda L: (p - L[0]).length - L[1])
    lob = (p - lc).normalized() if (p - lc).length > 1e-6 else env
    return (env * 0.6 + lob * 0.4 + UP * sky).normalized()


def _shell_cards(tm, mat, rng, lobes, centre, radii, count, size, rows=1, upright=0.0, hang=0.0,
                 floor=0.05):
    """Spread `count` cards over the lobes by surface area: most on the
    shell, a dark few inside; twig toward the lobe centre, front outward."""
    areas = [r.x * r.z for (_, _, r) in lobes]
    total = sum(areas)
    simple = [(c, max(r.x, r.y, r.z)) for (c, _, r) in lobes]

    def normal_of(p):
        return _crown_normal(p, simple, centre, radii)

    for (lc, _, lr), area in zip(lobes, areas):
        for _ in range(max(1, round(count * area / total))):
            d = _unit(rng)
            if d.z < -0.4 and rng.random() < 0.5:         # fewer underneath
                d.z = -d.z * 0.5
                d.normalize()
            inner = rng.random() < 0.18
            rr = rng.uniform(0.25, 0.6) if inner else rng.uniform(0.78, 1.0)
            p = lc + Vector((d.x * lr.x, d.y * lr.y, d.z * lr.z)) * rr
            p.z = max(p.z, floor + size * 0.3)
            out = normal_of(p)
            N = (out * 0.85 + _unit(rng) * 0.5).normalized()
            if N.dot(out) < 0.35:
                N = (N + out).normalized()
            U = (out - N * out.dot(N))
            U = (U.normalized() if U.length > 1e-3 else UP) + UP * upright - UP * hang
            U = (U - N * U.dot(N)).normalized()
            s = size * rng.uniform(0.8, 1.15)
            if inner:
                cell = CELL_DARK
            elif out.z > 0.45:
                cell = CELL_SUN if rng.random() < 0.65 else CELL_WARM
            else:
                cell = (CELL_WARM, CELL_COOL, CELL_COOL, CELL_DARK)[rng.randrange(4)]
            base = p - U * (s * 0.45)
            tm.card(mat, base, U, N, s, s, cell, normal_of, fold=rng.uniform(0.1, 0.3), rows=rows,
                    droop=(-UP * s * 0.12) if rows > 1 else None)


# ------------------------------------------------------------------ species
def broadleaf(name, height, width, seed, cards, card_m, foliage="broadleaf", rows=1, lobes_n=None):
    """A deciduous tree: trunk to the crown's base, a main branch to each
    lobe of the crown, a smaller one to a second lobe off most of them."""
    rng = random.Random(seed)
    mats = materials()
    bark, leaf = mats["bark"], mats[foliage]
    tm = TreeMesh(name)
    margin = card_m * 0.45
    base_z = height * rng.uniform(0.18, 0.25)
    radii = Vector((width / 2 - margin, width / 2 * 0.92 - margin, (height - base_z) / 2 - margin))
    centre = Vector((0, 0, base_z + margin + radii.z))
    trunk_r = width * 0.022 + 0.05
    lean = Vector((rng.uniform(-0.25, 0.25), rng.uniform(-0.25, 0.25), 0)) * width * 0.04
    crotch = Vector((0, 0, base_z)) + lean
    n_l = lobes_n or (5 if width < 6 else (7 if width < 10 else 9))
    lobes = []
    # a top lobe, then lobes round the crown, alternating high and low
    for k in range(n_l):
        if k == 0:
            d = Vector((rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), 1.0)).normalized()
        else:
            a = 2 * math.pi * k / (n_l - 1) + rng.uniform(-0.35, 0.35)
            el = rng.uniform(-0.6, -0.05) if k % 2 else rng.uniform(0.1, 0.6)
            d = Vector((math.cos(a) * math.cos(el), math.sin(a) * math.cos(el), math.sin(el)))
        reach = rng.uniform(0.5, 0.65)
        c = centre + Vector((d.x * radii.x, d.y * radii.y, d.z * radii.z)) * reach
        s = rng.uniform(0.45, 0.6)
        lr = Vector((radii.x * s, radii.y * s, radii.z * s * rng.uniform(0.75, 0.95)))
        lobes.append((c, d, lr))
    # trunk: curved, into the top lobe as a leader
    top = lobes[0][0]
    tpts = _bezier(Vector((0, 0, 0)), crotch, top, 6)
    tm.tube(bark, tpts, [trunk_r * (1.3 if k == 0 else 1.0) * (1 - 0.8 * k / 6) + 0.03 for k in range(7)], segs=8)
    for k, (c, d, lr) in enumerate(lobes[1:]):
        # off the trunk at the height of the lobe it feeds, low lobes first
        h = min(0.75, max(0.0, (c.z - crotch.z) / max(top.z - crotch.z, 0.1)) * 0.8 + rng.uniform(-0.05, 0.1))
        start = crotch + (top - crotch) * h
        mid = start + (c - start) * 0.5 + UP * (c - start).length * 0.15
        pts = _bezier(start, mid, c, 4)
        r0 = trunk_r * 0.55
        tm.tube(bark, pts, [r0 * (1 - 0.75 * i / 4) + 0.015 for i in range(5)], segs=6)
        if rng.random() < 0.7:                            # a fork toward the next lobe
            nxt = lobes[1 + (k + 1) % (len(lobes) - 1)][0]
            f0 = pts[2]
            f1 = f0 + (nxt - f0) * 0.45 + UP * 0.3
            tm.tube(bark, _bezier(f0, (f0 + f1) / 2 + UP * 0.2, f1, 2), [r0 * 0.35, r0 * 0.25, 0.012], segs=5)
    _shell_cards(tm, leaf, rng, lobes, centre, radii, cards, card_m, rows=rows, hang=0.15)
    return tm.finish()


def poplar(name, height, width, seed, cards, card_m, foliage="broadleaf"):
    """A Lombardy poplar: a straight trunk nearly to the top and a narrow
    spindle of upright clusters, widest a third of the way up."""
    rng = random.Random(seed)
    mats = materials()
    bark, leaf = mats["bark"], mats[foliage]
    tm = TreeMesh(name)
    base_z = height * 0.10
    top_z = height - card_m * 0.5
    tm.tube(bark, [Vector((0, 0, z)) + Vector((rng.uniform(-0.05, 0.05), rng.uniform(-0.05, 0.05), 0)) * (z / height)
                   for z in np.linspace(0, height * 0.9, 6)],
            [0.26 * (1 - 0.85 * k / 5) + 0.03 for k in range(6)], segs=7)

    def radius_at(z):
        t = (z - base_z) / (top_z - base_z)
        return max(0.4, (width / 2 - card_m * 0.4) * math.sin(math.pi * min(1.0, max(0.0, t)) ** 0.75))

    lobes = []
    n = 11
    for k in range(n):
        z = base_z + (top_z - base_z) * (k + 0.5) / n
        r = radius_at(z)
        off = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0)) * r * 0.25
        c = Vector((0, 0, z)) + off
        lobes.append((c, UP, Vector((r * 0.85, r * 0.85, (top_z - base_z) / n * 1.8))))
        if k % 2 == 0 and z < height * 0.85:
            tm.tube(bark, _bezier(Vector((0, 0, z - 1.0)), Vector((0, 0, z - 0.3)) + off * 0.5, c + UP * 0.6, 2),
                    [0.06, 0.04, 0.012], segs=5)
    centre = Vector((0, 0, (base_z + top_z) / 2))
    radii = Vector((width / 2, width / 2, (top_z - base_z) / 2))
    _shell_cards(tm, leaf, rng, lobes, centre, radii, cards, card_m, upright=0.9)
    return tm.finish()


def bush(name, width, height, seed, cards, card_m, foliage="broadleaf"):
    """A shrub: a low mound of four or five lobes on a few stems."""
    rng = random.Random(seed)
    mats = materials()
    bark, leaf = mats["bark"], mats[foliage]
    tm = TreeMesh(name)
    m = card_m * 0.4
    lobes = []
    for k in range(5):
        a = 2 * math.pi * k / 5 + rng.uniform(-0.4, 0.4)
        r = 0.0 if k == 0 else rng.uniform(0.15, 0.25) * width
        lr = Vector((rng.uniform(0.28, 0.36) * width - m, rng.uniform(0.26, 0.34) * width - m,
                     (height / 2 - m) * rng.uniform(0.75, 1.0)))
        c = Vector((math.cos(a) * r, math.sin(a) * r, lr.z + m * 0.5))
        lobes.append((c, UP, lr))
        tm.tube(bark, _bezier(Vector((c.x * 0.2, c.y * 0.2, 0)), Vector((c.x * 0.6, c.y * 0.6, c.z * 0.5)), c, 2),
                [0.05, 0.035, 0.01], segs=5)
    centre = Vector((0, 0, height * 0.45))
    radii = Vector((width / 2, width / 2 * 0.9, height / 2))
    _shell_cards(tm, leaf, rng, lobes, centre, radii, cards, card_m, floor=0.0)
    return tm.finish()


def conifer(name, height, width, seed, density=1.0, rows=2):
    """A spruce: whorls of frond cards from the trunk, rising near the top
    and drooping lower down, tips turned up; shorter toward the leader."""
    rng = random.Random(seed)
    mats = materials()
    bark, frond = mats["bark"], mats["conifer"]
    tm = TreeMesh(name)
    tm.tube(bark, [Vector((0, 0, z)) for z in np.linspace(0, height * 0.97, 6)],
            [width * 0.045 * (1 - 0.85 * k / 5) + 0.03 for k in range(6)], segs=8)
    z0 = height * rng.uniform(0.06, 0.10)
    dz = max(0.28, height / 40.0)
    R0 = width / 2

    def normal_of(p):
        rad = Vector((p.x, p.y, 0))
        rad = rad.normalized() if rad.length > 1e-4 else Vector((1, 0, 0))
        return (rad + UP * 0.55).normalized()

    z, tier = z0, 0
    while z < height - 0.7:
        t = (z - z0) / (height - z0)
        R = R0 * (1 - t) ** 0.95 * rng.uniform(0.9, 1.05) + 0.2
        n = max(5, int(round(R * 6.5 * density)))
        rise = math.radians(-25 + 50 * t)           # droop low down, rise near the top
        for k in range(n):
            a = 2 * math.pi * k / n + rng.uniform(-0.2, 0.2) + tier * 0.7
            out = Vector((math.cos(a), math.sin(a), 0))
            el = rise + rng.uniform(-0.12, 0.12)
            L = R * rng.uniform(0.88, 1.05)
            fz = z + dz * rng.uniform(-0.4, 0.4)
            el = max(el, math.asin(max(-1.0, min(1.0, (0.3 + 0.2 * L - fz) / L))))   # the low whorl clears the ground
            U = (out * math.cos(el) + UP * math.sin(el)).normalized()
            side = Vector((-math.sin(a), math.cos(a), 0))
            N = side.cross(U)
            if N.z < 0:
                N = -N
            roll = rng.uniform(-0.5, 0.5)            # not every frond flat to the sky
            N = (N * math.cos(roll) + side * math.sin(roll) + out * 0.25).normalized()
            base = Vector((0, 0, fz)) + out * 0.08
            cell = CELL_DARK if (t < 0.25 and rng.random() < 0.4) or rng.random() < 0.15 else \
                (CELL_SUN if t > 0.5 and rng.random() < 0.5 else (CELL_WARM, CELL_COOL)[rng.randrange(2)])
            droop = (UP * (0.12 * L) - U * 0.0) if rows > 1 else None
            tm.card(frond, base, U, N, L * 0.95, L, cell, normal_of, fold=0.35, rows=rows, droop=droop)
        z += dz * rng.uniform(0.9, 1.1)
        tier += 1
    for a in (0.0, math.pi / 2):                    # the leader
        N = Vector((math.cos(a), math.sin(a), 0))
        tm.card(frond, Vector((0, 0, height - 1.4)), UP, N, 0.55, 1.4, CELL_SUN, normal_of, fold=0.0)
    return tm.finish()


# ------------------------------------------------------------------ far woods
# The forest impostors stand in for a 40 m patch of wood out to 8 km, so a
# tree there can afford a few triangles, not a crown of cards: three
# crossed billboards of a sprite rendered from a real card tree. The
# sprites are lit by an even white sky only (albedo times occlusion), so
# the sun in the game lights them like everything else.
SPRITES = (                     # cell: kind and builder
    ("broadleaf", lambda: broadleaf("_sprite_b0", 14.0, 9.0, 101, 560, 1.8)),
    ("broadleaf", lambda: broadleaf("_sprite_b1", 12.0, 9.5, 102, 500, 1.6)),
    ("conifer", lambda: conifer("_sprite_c0", 16.0, 5.0, 103, density=1.2, rows=1)),
    ("conifer", lambda: conifer("_sprite_c1", 13.0, 4.6, 104, density=1.3, rows=1)),
)


def _render_sprite(ob, path, px=1024):
    """Render `ob` from the side, orthographic, base on the frame's bottom
    edge, on a transparent film; returns the frame's world size."""
    sc = bpy.context.scene
    hidden = [o for o in sc.objects if o is not ob and not o.hide_render]
    for o in hidden:
        o.hide_render = True
    pts = [ob.matrix_world @ v.co for v in ob.data.vertices]
    top = max(p.z for p in pts)
    span = max(max(abs(p.x) for p in pts), max(abs(p.y) for p in pts)) * 2
    size = max(top, span) * 1.02
    cd = bpy.data.cameras.new("_sprite_cam")
    cd.type = 'ORTHO'
    cd.ortho_scale = size
    cd.clip_end = 500.0
    cam = bpy.data.objects.new("_sprite_cam", cd)
    sc.collection.objects.link(cam)
    cam.location = (0.0, -200.0, size / 2)
    cam.rotation_euler = (math.pi / 2, 0.0, 0.0)
    sc.camera = cam
    world = sc.world or bpy.data.worlds.new("World")
    sc.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs[0].default_value = (1.0, 1.0, 1.0, 1.0)
    bg.inputs[1].default_value = 1.0
    r = sc.render
    r.engine = 'BLENDER_EEVEE'
    r.film_transparent = True
    r.resolution_x = r.resolution_y = px
    r.resolution_percentage = 100
    r.image_settings.file_format = 'PNG'
    r.image_settings.color_mode = 'RGBA'
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    for o in hidden:
        o.hide_render = False
    bpy.data.objects.remove(cam)
    bpy.data.cameras.remove(cd)
    return size


def _sprite_cell(path):
    """The rendered frame as an atlas cell: halved with premultiplied
    averaging, colour bled into the clear texels."""
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float64).reshape(h, w, 4)
    bpy.data.images.remove(img)
    a = px[..., 3]
    pre = px[..., :3] * a[..., None]
    a2 = a.reshape(h // 2, 2, w // 2, 2).mean((1, 3))
    c2 = pre.reshape(h // 2, 2, w // 2, 2, 3).mean((1, 3)) / np.maximum(a2, 1e-4)[..., None]
    cv = Canvas(h // 2)
    cv.col, cv.a = np.clip(c2, 0, 1), np.clip(a2, 0, 1)
    col, _ = cv.finish()
    return col, cv.a


def forest_material(tmp_dir):
    """`forest_billboard`: the four sprites, rendered here and now (the
    frames land in `tmp_dir`); also returns each cell's (frame size, kind)."""
    if "forest" not in _MATS:
        os.makedirs(tmp_dir, exist_ok=True)
        cells, info = [], []
        for k, (kind, build) in enumerate(SPRITES):
            ob = build()
            path = os.path.join(tmp_dir, "_sprite_%d.png" % k)
            size = _render_sprite(ob, path)
            me = ob.data
            bpy.data.objects.remove(ob)
            bpy.data.meshes.remove(me)
            cells.append(_sprite_cell(path))
            info.append((size, kind))
        col, alpha = _atlas(cells)
        # one-sided (see billboard_tree), so not a `tree_card_*` slot, which
        # the importer forces two-sided
        m = tex.card_material("forest_billboard", (col, alpha))
        m.use_backface_culling = True
        _MATS["forest"] = m
        _MATS["forest_cells"] = info
    return _MATS["forest"], _MATS["forest_cells"]


def billboard_tree(tm, mat, cell, x, y, z, height_m, width_scale, yaw, sink_m=0.0):
    """Three crossed billboards of atlas cell `cell`, the (square) frame
    scaled to `height_m`, its bottom edge at z, stretched `sink_m` further
    down (a patch is planted level on a slope). Each is two one-sided
    cards back to back, both with a normal mostly toward the sky: a two-
    sided card flips its normal on the back face, which on a billboard
    turns that half of every far wood black."""
    s = height_m
    w = s * width_scale
    c = Vector((x, y, z))
    for k in range(3):
        a = yaw + k * math.pi / 3
        R = Vector((math.cos(a), math.sin(a), 0.0))
        G = R.cross(UP)                                   # the front face's own normal
        lo = c - UP * sink_m
        quad = [lo - R * (w / 2), lo + R * (w / 2), c + R * (w / 2) + UP * s, c - R * (w / 2) + UP * s]
        uvs = [_cell_uv(cell, 0, 0), _cell_uv(cell, 1, 0), _cell_uv(cell, 1, 1), _cell_uv(cell, 0, 1)]
        for side in (1, -1):
            n = (G * (0.75 * side) + UP * 0.45).normalized()
            pts = [p - G * (0.01 * (1 - side)) for p in quad]
            order = (0, 1, 2, 3) if side > 0 else (1, 0, 3, 2)
            tm.face(mat, [tm.bm.verts.new(pts[i]) for i in order], [uvs[i] for i in order], [n] * 4)
