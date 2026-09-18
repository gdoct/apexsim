"""Shared car-building toolkit for content/cars/build_*.py (Blender 5.x).

Load from a build script:

    import importlib.util, sys, os
    for _n, _p in (("apex", r"D:\\apexsim\\content\\props\\_tools\\apex_props.py"),
                   ("carlib", r"D:\\apexsim\\content\\cars\\carlib.py")):
        _s = importlib.util.spec_from_file_location(_n, _p)
        _m = importlib.util.module_from_spec(_s); sys.modules[_n] = _m; _s.loader.exec_module(_m)
    import carlib

Frame: nose on -Y, tail on +Y, ground z = 0, metres, left-hand drive (driver
on +X). See docs/CAR_MODELS.md.

What this module exists for
---------------------------
The first generation of these cars read as toys for five reasons, and each
has a counterpart here:

* every surface was either fully inflated (section loft + SUBSURF, which
  smooths any feature away) or knife-thin -> `Loft` takes *hard* control
  points, creased so a shoulder line or fender peak survives subdivision,
  and `bevel()` gives the bolt-on parts a small radius instead of a sharp
  arris;
* detail was pasted on as coplanar strips floating off the surface ->
  `Loft.groove()` and `Loft.recess()` cut shut lines, ducts and vents *into*
  the loft by displacing inserted stations along the true surface normal,
  and `Loft.conform()` lays a decal on the curved flank;
* stance was wrong -> `arch()` cuts an opening that follows the tyre with a
  rolled lip and a closed liner, instead of a bare cylinder boolean;
* aero parts were paper -> `plate()`, `foil()`, `endplate()`, `gurney()`,
  `diffuser()` and `splitter()` all carry real thickness;
* lights were emissive boxes -> `lamp_cluster()` sets a lens and reflectors
  into a housing recessed in the body.
"""
import bpy, bmesh, math, os
from mathutils import Vector

import apex
from apex import Builder, material, reset_scene   # noqa: F401  (re-exported)

CARS_ROOT = r"D:\apexsim\content\cars"


# --------------------------------------------------------------- materials
def mat(name, color, metallic=0.0, roughness=0.5, coat=0.0, alpha=1.0, emission=None):
    m = material(name, color, metallic, roughness, emission)
    b = m.node_tree.nodes["Principled BSDF"]
    if coat:
        b.inputs["Coat Weight"].default_value = coat
    if alpha < 1.0:
        b.inputs["Alpha"].default_value = alpha
        try:
            m.surface_render_method = 'BLENDED'
        except Exception:
            pass
    return m


class Mats(dict):
    """Attribute access over the standard slot set."""

    def __getattr__(self, k):
        try:
            return self[k]
        except KeyError:
            raise AttributeError(k)


def car_materials(paint_rgb, accent_rgb, caliper_rgb, logo_path=None, seat_rgb=(0.10, 0.10, 0.12)):
    """The slot set every car GLB carries; names are what the client drives
    (docs/CAR_MODELS.md - do not rename)."""
    m = Mats(
        paint=mat("car_paint", paint_rgb, 0.10, 0.25, coat=1.0),
        accent=mat("car_accent", accent_rgb, 0.05, 0.30, coat=1.0),
        carbon=mat("car_carbon", (0.045, 0.045, 0.055), 0.30, 0.32, coat=0.6),
        glass=mat("car_glass", (0.02, 0.03, 0.04), 0.0, 0.05, alpha=0.5),
        liner=mat("car_liner", (0.028, 0.028, 0.030), 0.0, 0.92),
        interior=mat("car_interior", (0.11, 0.11, 0.12), 0.10, 0.80),
        seat=mat("car_seat", seat_rgb, 0.0, 0.85),
        alc=mat("car_wheel_rim", (0.055, 0.055, 0.058), 0.0, 0.70),
        metal=mat("car_metal", (0.58, 0.58, 0.60), 1.0, 0.35),
        cage=mat("car_cage", (0.74, 0.74, 0.77), 0.90, 0.40),
        rim=mat("car_rim", (0.12, 0.12, 0.13), 1.0, 0.35),
        brake_disc=mat("car_brake", (0.34, 0.32, 0.30), 1.0, 0.60),
        caliper=mat("car_caliper", caliper_rgb, 0.20, 0.40),
        lamp=mat("car_headlight", (0.95, 0.95, 0.90), 0.0, 0.08, emission=(1.0, 0.98, 0.90)),
        lamp_h=mat("car_lamp_housing", (0.018, 0.018, 0.020), 0.60, 0.30),
        tail=mat("car_taillight", (0.50, 0.03, 0.02), 0.0, 0.15, emission=(1.0, 0.08, 0.04)),
        brake=mat("car_brakelight", (0.60, 0.02, 0.02), 0.0, 0.15, emission=(1.0, 0.02, 0.0)),
        rain=mat("car_rainlight", (0.60, 0.02, 0.02), 0.0, 0.15, emission=(1.0, 0.02, 0.0)),
        display=mat("car_display", (0.02, 0.02, 0.03), 0.0, 0.20, emission=(0.10, 0.40, 0.20)),
        towhook=mat("car_towhook", (0.88, 0.10, 0.05), 0.30, 0.50),
        rubber=mat("car_rubber", (0.03, 0.03, 0.03), 0.0, 0.90),
        mesh=mat("car_mesh", (0.035, 0.035, 0.040), 0.40, 0.55),
    )
    if logo_path:
        m["logo"] = apex.image_material("car_logo", logo_path, roughness=0.30, masked=True)
    return m


# ------------------------------------------------------------------ curves
def catmull(pts, n):
    """Uniform Catmull-Rom through `pts`, `n` samples per span, endpoints kept."""
    P = [Vector(p) for p in pts]
    P = [P[0]] + P + [P[-1]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(n):
            t = k / n
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t
                              + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(P[-2])
    return out


def _smoothstep(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def fender_bump(keys, axles, amount=0.085, width=0.62, j0=2.2, j1=4.3, fade=0.8):
    """Raise the flank over each axle so the fender crowns stand above the
    bonnet and deck.

    Proportion, not decoration: if the arch opening tops out above the fender
    peak the section has no outer crossing at that height, the flank and the
    upper surface swap places, and anything that samples the skin there (the
    arch liner, a decal, a vent) tears. Every GT3 and prototype has the
    fender crown clearly above the arch lip; this puts it there."""
    out = []
    for (y, pts) in keys:
        f = 0.0
        for ax in axles:
            f = max(f, math.exp(-((y - ax) / width) ** 2))
        new = []
        for j, (x, z) in enumerate(pts):
            if amount and j0 <= j <= j1:
                g = min(_smoothstep((j - j0) / fade), _smoothstep((j1 - j) / fade))
                z = z + amount * f * g
            new.append((x, z))
        out.append((y, new))
    return out


def insert_stations(keys, ys, key_samp=12):
    """Add interpolated stations at `ys` to a key list.

    The hand-authored keys are 40-60 cm apart, which is fine along the flank
    and hopeless at the ends: a nose or a tail lofted from two keys is one big
    blend with nothing to shape. Extra stations there give the bumper face,
    the corner and the leading edge of the bonnet each their own section."""
    tmp = Loft(keys, samp=2, ny=4, key_samp=key_samp)
    out = list(keys)
    for y in ys:
        if any(abs(y - k[0]) < 1e-6 for k in out):
            continue
        out.append((y, tmp.ctrl_at(y)))
    return sorted(out, key=lambda k: k[0])


def remap_station(keys, y, x_scale=1.0, z_lo=None, z_hi=None, j_from=0):
    """Reshape the station at `y`: scale its half-widths and stretch its
    heights so the section runs from `z_lo` to `z_hi` (either may be None to
    keep that end). `j_from` limits the height remap to control indices from
    there up, so the floor and sill can be left alone."""
    out = []
    for (ky, pts) in keys:
        if abs(ky - y) > 1e-6:
            out.append((ky, pts))
            continue
        zs = [z for (_, z) in pts[j_from:]]
        lo, hi = min(zs), max(zs)
        nlo = lo if z_lo is None else z_lo
        nhi = hi if z_hi is None else z_hi
        new = []
        for j, (x, z) in enumerate(pts):
            if j >= j_from and hi > lo:
                z = nlo + (z - lo) / (hi - lo) * (nhi - nlo)
            new.append((x * x_scale, z))
        out.append((ky, new))
    return out


def tip_station(keys, y_from, y_new, x_scale=0.80, z_shrink=0.75):
    """Add a station at `y_new` that is the section at `y_from` pulled in
    towards its middle - a rounded end instead of a flat cap the full size of
    the bumper."""
    src = next(pts for (ky, pts) in keys if abs(ky - y_from) < 1e-6)
    zs = [z for (_, z) in src]
    zc = (min(zs) + max(zs)) / 2.0
    new = [(x * x_scale, zc + (z - zc) * z_shrink) for (x, z) in src]
    return sorted(keys + [(y_new, new)], key=lambda k: k[0])


# -------------------------------------------------------------------- loft
class Loft:
    """A body shell lofted through cross-section keys, with feature lines.

    `keys` is a list of `(y, [(x, z), ...])` right-half control points, listed
    from the floor centre outwards and up to the roof centre; every key needs
    the same number of points. Between keys each control point follows a
    Catmull-Rom curve along Y, and each station's points are swept into a
    section the same way, so the shell is smooth in both directions.

    Two things the old generator could not express:

    `hard=(j, ...)` marks control indices as *creases*. The section is
    sampled as separate runs either side of a hard point, so it is a genuine
    corner rather than a smoothed bulge, and the edge loop it produces is
    creased so `SUBSURF` keeps it. That is how a shoulder line, a fender peak
    or a bonnet-to-fender transition reads as an edge instead of melting.

    `groove()` and `recess()` cut features *into* the shell. Both insert their
    own stations and displace them along the true surface normal, so a shut
    line is a V-groove in the bodywork and a duct has a floor and walls -
    rather than a strip of dark geometry hovering over an untouched surface.
    """

    def __init__(self, keys, samp=4, ny=96, hard=(), hard_y=(), key_samp=12):
        self.keys = list(keys)
        self.npc = len(keys[0][1])
        self.samp = samp
        self.ny = ny
        self.nr = (self.npc - 1) * samp + 1           # samples across the right half
        self.m = 2 * self.nr - 2                      # samples around the closed ring
        self.hard = set(hard)
        self.hard_y = list(hard_y)
        self.key_ys = [k[0] for k in self.keys]
        self.nose, self.tail = self.key_ys[0], self.key_ys[-1]
        self.cp = [catmull([(k[0], k[1][j][0], k[1][j][1]) for k in self.keys], key_samp)
                   for j in range(self.npc)]
        self._feat = []          # feature list, see groove()/recess()
        self._crease_y = []      # (y, j0, j1) transverse creases from features
        self._cache = {}

    # ---- sampling -----------------------------------------------------
    def ctrl_at(self, y):
        """The right-half control points at station `y`."""
        out = []
        for c in self.cp:
            for i in range(len(c) - 1):
                if c[i].x <= y <= c[i + 1].x:
                    t = (y - c[i].x) / max(c[i + 1].x - c[i].x, 1e-9)
                    v = c[i].lerp(c[i + 1], t)
                    out.append((v.y, v.z))
                    break
            else:
                out.append((c[-1].y, c[-1].z) if y > c[-1].x else (c[0].y, c[0].z))
        return out

    def half(self, y):
        """Right-half section samples at `y` as (x, z), honouring hard corners."""
        key = round(y, 6)
        if key in self._cache:
            return self._cache[key]
        ctrl = self.ctrl_at(y)
        bounds = sorted({0, self.npc - 1} | {j for j in self.hard if 0 < j < self.npc - 1})
        right = []
        for n, (a, b) in enumerate(zip(bounds, bounds[1:])):
            seg = [(p.x, p.y) for p in catmull(ctrl[a:b + 1], self.samp)]
            right += seg if n == len(bounds) - 2 else seg[:-1]
        right[0] = (0.0, right[0][1])
        right[-1] = (0.0, right[-1][1])
        right = [(max(x, 0.0), z) for (x, z) in right]
        self._cache[key] = right
        return right

    def point(self, y, j):
        """Point on the right flank at station `y`, control index `j` (may be fractional)."""
        h = self.half(y)
        k = j * self.samp
        i0 = int(math.floor(k))
        i1 = min(i0 + 1, len(h) - 1)
        t = k - i0
        x = h[i0][0] * (1 - t) + h[i1][0] * t
        z = h[i0][1] * (1 - t) + h[i1][1] * t
        return Vector((x, y, z))

    def normal(self, y, j, dy=0.012, dj=0.25):
        """Outward unit normal on the right flank at (`y`, control index `j`)."""
        a = self.point(y, max(j - dj, 0.0))
        b = self.point(y, min(j + dj, self.npc - 1.0))
        c = self.point(max(y - dy, self.nose), j)
        d = self.point(min(y + dy, self.tail), j)
        n = (b - a).cross(d - c)
        if n.length < 1e-9:
            return Vector((1, 0, 0))
        n.normalize()
        p = self.point(y, j)
        mid = Vector((0.0, p.y, (self.half(y)[0][1] + self.half(y)[-1][1]) / 2))
        if n.dot(p - mid) < 0:
            n = -n
        return n

    def x_at(self, y, z):
        """Half-width of the outer skin at height `z` on station `y`.

        A section is a closed loop, so several samples share a height - the
        floor, the flank and the roof. Taking the nearest one by z picks the
        roofline as happily as the flank, so this walks the section and
        returns the *outermost* crossing of `z`, falling back to the widest
        sample when `z` is above or below the whole section."""
        h = self.half(y)
        best = None
        for i in range(len(h) - 1):
            z0, z1 = h[i][1], h[i + 1][1]
            if (z0 - z) * (z1 - z) <= 0.0 and abs(z1 - z0) > 1e-9:
                t = (z - z0) / (z1 - z0)
                x = h[i][0] * (1 - t) + h[i + 1][0] * t
                best = x if best is None else max(best, x)
        if best is None:
            best = max(q[0] for q in h)
        return best

    def z_at(self, y, x):
        """Height of the *upper* surface at half-width `x` on station `y`.

        Deliberately not "the nearest sample by x": the floor centre and the
        roof centre are both at x = 0, and taking the nearest one put the roll
        cage on the floor. Walks down from the roof centre and returns the
        first crossing, so this is always the top of the car."""
        h = self.half(y)
        for i in range(len(h) - 1, 0, -1):
            x0, x1 = h[i][0], h[i - 1][0]
            if (x0 - x) * (x1 - x) <= 0.0 and abs(x1 - x0) > 1e-9:
                t = (x - x0) / (x1 - x0)
                return h[i][1] * (1 - t) + h[i - 1][1] * t
        return h[-1][1]

    def lower_z(self, y, x):
        """Height of the underside at half-width `x` (walking up from the floor)."""
        h = self.half(y)
        for i in range(len(h) - 1):
            x0, x1 = h[i][0], h[i + 1][0]
            if (x0 - x) * (x1 - x) <= 0.0 and abs(x1 - x0) > 1e-9:
                t = (x - x0) / (x1 - x0)
                return h[i][1] * (1 - t) + h[i + 1][1] * t
        return h[0][1]

    def max_half_width(self, steps=48):
        """The widest half-width anywhere on the shell - i.e. half the X extent
        of the exported mesh box, which is what the client measures off."""
        best = 0.0
        for i in range(steps + 1):
            y = self.nose + (self.tail - self.nose) * i / steps
            best = max(best, max(q[0] for q in self.half(y)))
        return best

    def roof_z(self, y):
        return self.half(y)[-1][1]

    def floor_z(self, y):
        return self.half(y)[0][1]

    # ---- features -----------------------------------------------------
    def groove(self, y, j0, j1, depth=0.010, width=0.014, jfade=0.35, sx=0, crease=True):
        """A shut line: a V-groove cut across the flank at station `y`,
        between control indices `j0` and `j1`. `sx` = -1/+1 for one side only."""
        self._feat.append(dict(kind="groove", y=y, j0=j0, j1=j1, depth=depth,
                               w=width / 2.0, jfade=jfade, sx=sx))
        if crease:
            self._crease_y += [(y - width / 2.0, j0, j1), (y + width / 2.0, j0, j1)]
        return self

    def recess(self, y0, y1, j0, j1, depth=0.030, rim=0.016, jfade=0.30, sx=0, crease=True):
        """A duct, intake or vent: a panel of the flank pushed in along the
        surface normal, with sloped walls `rim` long at each end."""
        self._feat.append(dict(kind="recess", y0=y0, y1=y1, j0=j0, j1=j1,
                               depth=depth, rim=rim, jfade=jfade, sx=sx))
        if crease:
            self._crease_y += [(y0, j0, j1), (y1, j0, j1)]
        return self

    def _depth_y(self, f, y):
        if f["kind"] == "groove":
            d = abs(y - f["y"])
            return 0.0 if d >= f["w"] else f["depth"] * (1.0 - d / f["w"])
        y0, y1, rim = f["y0"], f["y1"], f["rim"]
        if y <= y0 or y >= y1:
            return 0.0
        if y < y0 + rim:
            return f["depth"] * _smoothstep((y - y0) / rim)
        if y > y1 - rim:
            return f["depth"] * _smoothstep((y1 - y) / rim)
        return f["depth"]

    def _offset(self, y, kk, right):
        """Inward displacement at station `y`, half-ring index `kk`."""
        if not self._feat:
            return 0.0
        jc = kk / self.samp
        tot = 0.0
        for f in self._feat:
            if f["sx"] and (f["sx"] > 0) != right:
                continue
            dz = self._depth_y(f, y)
            if dz <= 0.0:
                continue
            fd = f["jfade"]
            g = _smoothstep((jc - f["j0"]) / fd) * _smoothstep((f["j1"] - jc) / fd)
            if g > 0.0:
                tot += dz * g
        return tot

    def _stations(self):
        ys = [self.nose + (self.tail - self.nose) * i / self.ny for i in range(self.ny + 1)]
        for f in self._feat:
            if f["kind"] == "groove":
                ys += [f["y"] - f["w"], f["y"] - f["w"] * 0.5, f["y"],
                       f["y"] + f["w"] * 0.5, f["y"] + f["w"]]
            else:
                y0, y1, rim = f["y0"], f["y1"], f["rim"]
                ys += [y0, y0 + rim * 0.5, y0 + rim, y1 - rim, y1 - rim * 0.5, y1]
        ys = [y for y in ys if self.nose <= y <= self.tail]
        ys = sorted(set(round(y, 6) for y in ys))
        out = [ys[0]]
        for y in ys[1:]:
            if y - out[-1] > 1.5e-3:      # keep stations apart enough to mesh
                out.append(y)
        out[-1] = round(self.tail, 6)
        return out

    # ---- meshing ------------------------------------------------------
    def build(self, name, face_mat, mats_order, subsurf=1, uv=lambda co: (co.y, co.x + co.z)):
        """Mesh the shell. `face_mat(y_mid, kk, right) -> material` picks a slot
        per face; `mats_order` is the material list the slots come from."""
        b = Builder(name)
        bm = b.bm
        slots = {m: b.slot(m) for m in mats_order}
        ys = self._stations()
        rings = []
        for y in ys:
            h = self.half(y)
            row = []
            for k in range(self.m):
                right = k < self.nr
                kk = k if right else self.m - k
                x, z = h[kk]
                p = Vector((x if right else -x, y, z))
                off = self._offset(y, kk, right)
                if off and kk not in (0, self.nr - 1):
                    n = self.normal(y, kk / self.samp)
                    if not right:
                        n = Vector((-n.x, n.y, n.z))
                    p = p - n * off
                row.append(bm.verts.new(p))
            rings.append(row)

        for i in range(len(ys) - 1):
            ym = (ys[i] + ys[i + 1]) / 2
            for k in range(self.m):
                k2 = (k + 1) % self.m
                # wound so the normal points out of the shell (the first
                # generation of these bodies was uniformly inside-out, which
                # is half of why the paint read as flat)
                f = bm.faces.new((rings[i][k], rings[i + 1][k], rings[i + 1][k2], rings[i][k2]))
                right = k < self.nr
                kk = k if right else self.m - k
                f.material_index = slots[face_mat(ym, kk, right)]
                for l in f.loops:
                    l[b.uv].uv = uv(l.vert.co)
                b.keep.add(f)
        for i, fwd in ((0, True), (len(ys) - 1, False)):
            verts = rings[i] if fwd else list(reversed(rings[i]))
            f = bm.faces.new(verts)
            f.material_index = slots[face_mat(ys[i], 0, True)]
            for l in f.loops:
                l[b.uv].uv = (l.vert.co.x, l.vert.co.z)
            b.keep.add(f)

        # creases: longitudinal at hard control points, transverse at feature rims
        cl = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
        ring_idx = {id(v): (i, k) for i, row in enumerate(rings) for k, v in enumerate(row)}
        hard_k = set()
        for j in self.hard:
            hard_k.add(j * self.samp)
            hard_k.add((self.m - j * self.samp) % self.m)
        crease_rows = []
        for (yc, j0, j1) in self._crease_y:
            best = min(range(len(ys)), key=lambda i: abs(ys[i] - yc))
            if abs(ys[best] - yc) < 4e-3:
                crease_rows.append((best, j0 * self.samp, j1 * self.samp))
        for e in bm.edges:
            a = ring_idx.get(id(e.verts[0]))
            c = ring_idx.get(id(e.verts[1]))
            if not a or not c:
                continue
            if a[1] == c[1] and a[1] in hard_k and abs(a[0] - c[0]) == 1:
                e[cl] = 1.0
            elif a[0] == c[0]:
                kk0 = a[1] if a[1] < self.nr else self.m - a[1]
                kk1 = c[1] if c[1] < self.nr else self.m - c[1]
                for (row, k0, k1) in crease_rows:
                    if a[0] == row and k0 - self.samp <= min(kk0, kk1) and max(kk0, kk1) <= k1 + self.samp:
                        e[cl] = 1.0
                        break

        ob = b.finish(planar_uv=False)
        if subsurf:
            sub = ob.modifiers.new("smooth", 'SUBSURF')
            sub.levels = subsurf
            sub.render_levels = subsurf
            sub.use_creases = True
            with bpy.context.temp_override(object=ob, selected_editable_objects=[ob]):
                bpy.ops.object.modifier_apply(modifier=sub.name)
        sharpen(ob, 34.0)
        return ob


# ------------------------------------------------------------------ bevel
def bevel(ob, width=0.006, segments=2, angle_deg=40.0, harden=True):
    """Put a small radius on every hard edge of `ob`.

    Racing bodywork has small radii, not arrises. The bolt-on parts are built
    from boxes and plates, so one pass of this is what stops them reading as
    cardboard. Angle-limited, so flat panels are left alone."""
    m = ob.modifiers.new("bevel", 'BEVEL')
    m.width = width
    m.segments = segments
    m.limit_method = 'ANGLE'
    m.angle_limit = math.radians(angle_deg)
    for attr, val in (("miter_outer", 'MITER_ARC'), ("use_clamp_overlap", True),
                      ("clamp_overlap", True), ("harden_normals", harden)):
        try:
            setattr(m, attr, val)
        except Exception:
            pass
    with bpy.context.temp_override(object=ob, selected_editable_objects=[ob]):
        bpy.ops.object.modifier_apply(modifier=m.name)
    return sharpen(ob, 42.0)


def sharpen(ob, angle_deg=38.0):
    """Split shading normals on edges sharper than `angle_deg`.

    Blender's `shade_auto_smooth` operator needs the Smooth-by-Angle geometry
    nodes asset, which is not present on every install; marking edges sharp
    does the same job with no dependency, and is what makes a crease read as
    an edge rather than a soft bulge."""
    me = ob.data
    me.shade_smooth()
    bm = bmesh.new()
    bm.from_mesh(me)
    lim = math.radians(angle_deg)
    for e in bm.edges:
        if len(e.link_faces) == 2:
            e.smooth = e.calc_face_angle() < lim
        else:
            e.smooth = True
    bm.to_mesh(me)
    bm.free()
    return ob


def flip_faces(ob):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.reverse_faces(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()
    return ob


def outward_normals(ob):
    """Make the mesh's normals point out of the solid."""
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()
    return ob


# ------------------------------------------------------------- wheel arches
def _prism(mat, profile_yz, x0, x1, name="prism"):
    """A closed solid: the closed polygon `profile_yz` swept from x0 to x1."""
    b = Builder(name)
    bm = b.bm
    s = b.slot(mat)
    A = [bm.verts.new((x0, y, z)) for (y, z) in profile_yz]
    B = [bm.verts.new((x1, y, z)) for (y, z) in profile_yz]
    n = len(profile_yz)
    for i in range(n):
        j = (i + 1) % n
        f = bm.faces.new((A[i], A[j], B[j], B[i]))
        f.material_index = s
    for verts in (list(reversed(A)), B):
        f = bm.faces.new(verts)
        f.material_index = s
    ob = b.finish(planar_uv=True)
    return outward_normals(ob)


def arch_outline(y_axle, r_open, tyre_r, z_bottom, segs=40):
    """The opening a wheel arch cuts, as a (y, z) polyline: straight up the
    trailing edge, over the tyre at `r_open`, straight down the leading edge."""
    pts = [(y_axle - r_open, z_bottom)]
    for i in range(segs + 1):
        a = math.pi * (1.0 - i / segs)
        pts.append((y_axle + r_open * math.cos(a), tyre_r + r_open * math.sin(a)))
    pts.append((y_axle + r_open, z_bottom))
    return pts


def arch(body, loft, mats, y_axle, tyre_r, sx=1, gap=0.022, x_in=0.50,
         z_bottom=0.11, segs=40, bead=False, lip=0.024, flare=0.016):
    """Cut one wheel arch in `body`, following the tyre at `gap` clearance.

    The body shell is closed, so it is a valid solid and the boolean gives a
    watertight wheel well for free: the cutter's own surface becomes the
    well's walls. Handing the cutter the liner slot is therefore all it takes
    - an extra liner object on top of those walls is coincident geometry, and
    renders as z-fighting stripes.

    The old generator cut a plain cylinder of a radius well clear of the tyre,
    which read as a circular porthole with a 7 cm gap and a zero-thickness
    edge. Returns the bead object, or None."""
    r_open = tyre_r + gap
    out = arch_outline(y_axle, r_open, tyre_r, z_bottom, segs)
    cut_poly = out + [(y_axle + r_open, -0.5), (y_axle - r_open, -0.5)]
    c = _prism(mats.liner, cut_poly, min(sx * x_in, sx * 1.6), max(sx * x_in, sx * 1.6),
               "arch_cut")
    mod = body.modifiers.new("arch", 'BOOLEAN')
    mod.operation = 'DIFFERENCE'
    mod.object = c
    mod.solver = 'EXACT'
    with bpy.context.temp_override(object=body, selected_editable_objects=[body]):
        bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(c)
    if not bead:
        return None
    # an arch extension standing proud of the fender, as a solid ring
    b = Builder("arch_bead")
    bm = b.bm
    sc = b.slot(mats.carbon)
    ax = Vector((0.0, y_axle, tyre_r))
    xsamp = [loft.x_at(y, z) for (y, z) in out]
    xsm = [sum(xsamp[max(i - 2, 0):i + 3]) / len(xsamp[max(i - 2, 0):i + 3])
           for i in range(len(xsamp))]
    rows = [[], [], []]
    for i, (y, z) in enumerate(out):
        rad = Vector((0.0, y, z)) - ax
        rad = rad.normalized() if rad.length > 1e-6 else Vector((0, 0, 1))
        for r, (dx, dr) in enumerate(((-0.004, 0.0), (lip, flare * 0.5), (lip * 0.3, flare))):
            rows[r].append(bm.verts.new((sx * (xsm[i] + dx),
                                         y + rad.y * dr, z + rad.z * dr)))
    for i in range(len(out) - 1):
        for r in range(len(rows) - 1):
            A, B = rows[r], rows[r + 1]
            q = (A[i], A[i + 1], B[i + 1], B[i]) if sx > 0 else (A[i], B[i], B[i + 1], A[i + 1])
            f = bm.faces.new(q)
            f.material_index = sc
    return sharpen(b.finish(planar_uv=True, recalc=False), 40.0)


def box_solid(mat, lo, hi, name="box_cut"):
    """A closed box, for use as a boolean cutter."""
    lo, hi = Vector(lo), Vector(hi)
    prof = [(lo.y, lo.z), (hi.y, lo.z), (hi.y, hi.z), (lo.y, hi.z)]
    return _prism(mat, prof, lo.x, hi.x, name)


def cut_solid(body, solid, keep=False):
    """Boolean-difference `solid` out of `body`.

    The body shell is closed, so the cutter's surface becomes the walls of
    the hole: give the cutter the slot you want those walls to have (a lamp
    housing, a duct throat) and the aperture comes out lined."""
    mod = body.modifiers.new("cut", 'BOOLEAN')
    mod.operation = 'DIFFERENCE'
    mod.object = solid
    mod.solver = 'EXACT'
    with bpy.context.temp_override(object=body, selected_editable_objects=[body]):
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if not keep:
        bpy.data.objects.remove(solid)
    return body


def aperture(body, mats, lo, hi, mat=None):
    """Cut a rectangular opening (lamp, grille, duct) with lined walls."""
    return cut_solid(body, box_solid(mat or mats.lamp_h, lo, hi))


# -------------------------------------------------------------------- aero
def plate(b, mat, profile_yz, x, thick, name=None, chamfer=0.0):
    """A solid plate: the closed (y, z) outline given thickness across X.
    Endplates, fins and dive planes were two coplanar faces with open sides
    before, which is why they read as paper."""
    s = b.slot(mat)
    bm = b.bm
    h = thick / 2.0
    ins = []
    if chamfer > 0.0:
        cy = sum(p[0] for p in profile_yz) / len(profile_yz)
        cz = sum(p[1] for p in profile_yz) / len(profile_yz)
        for (y, z) in profile_yz:
            d = Vector((cy - y, cz - z))
            d = d.normalized() * chamfer if d.length > 1e-6 else Vector((0, 0))
            ins.append((y + d.x, z + d.y))
    else:
        ins = list(profile_yz)
    A = [bm.verts.new((x - h, y, z)) for (y, z) in ins]
    B = [bm.verts.new((x + h, y, z)) for (y, z) in ins]
    n = len(ins)
    if chamfer > 0.0:
        Am = [bm.verts.new((x - h * 0.45, y, z)) for (y, z) in profile_yz]
        Bm = [bm.verts.new((x + h * 0.45, y, z)) for (y, z) in profile_yz]
        rows = [A, Am, Bm, B]
    else:
        rows = [A, B]
    for r in range(len(rows) - 1):
        for i in range(n):
            j = (i + 1) % n
            f = bm.faces.new((rows[r][i], rows[r][j], rows[r + 1][j], rows[r + 1][i]))
            f.material_index = s
    for verts in (list(reversed(A)), B):
        f = bm.faces.new(verts)
        f.material_index = s
    return A, B


def panel_xy(b, mat, plan, z0, z1, name=None):
    """A flat panel from a plan-view outline, given thickness in Z.

    Floor aero is shaped in plan, not in section: a splitter or a diffuser
    floor has to follow the outline of the bodywork above it, or its corners
    hang out in mid-air beside the nose."""
    s = b.slot(mat)
    bm = b.bm
    A = [bm.verts.new((x, y, z0)) for (x, y) in plan]
    B = [bm.verts.new((x, y, z1)) for (x, y) in plan]
    n = len(plan)
    for i in range(n):
        j = (i + 1) % n
        f = bm.faces.new((A[i], A[j], B[j], B[i]))
        f.material_index = s
    for verts in (list(reversed(A)), B):
        f = bm.faces.new(verts)
        f.material_index = s
    return A, B


def floor_plan(loft, y0, y1, z, steps=14, inset=0.0, lead=0.0):
    """A plan outline following the body's half-width at height `z`.

    `inset` pulls it inboard of the skin, `lead` pushes the leading station
    forward (a splitter oversails the bodywork by a few centimetres)."""
    ys = [y0 + (y1 - y0) * i / steps for i in range(steps + 1)]
    right = [(max(loft.x_at(y, z) - inset, 0.02), y) for y in ys]
    if lead:
        right[0] = (right[0][0], right[0][1] - lead)
    left = [(-x, y) for (x, y) in reversed(right)]
    return right + left


def foil(b, mat, x0, x1, chord, thick, camber, ly, lz, n=16, angle_deg=-8.0, tip_scale=1.0):
    """A cambered wing element: NACA-ish section, leading edge at (ly, lz),
    pitched `angle_deg`, lofted along X."""
    s = b.slot(mat)
    bm = b.bm
    a = math.radians(angle_deg)

    def yt(t):
        return 5 * thick * (0.2969 * math.sqrt(max(t, 0.0)) - 0.126 * t
                            - 0.3516 * t ** 2 + 0.2843 * t ** 3 - 0.1015 * t ** 4)

    def yc(t):
        return camber * (2 * t - t * t)

    pts = [(t * chord, (yc(t) + yt(t)) * chord) for t in (i / n for i in range(n + 1))]
    pts += [(t * chord, (yc(t) - yt(t)) * chord) for t in ((n - i) / n for i in range(n + 1))]

    def prof(scale):
        return [(ly + (py * math.cos(a) - pz * math.sin(a)) * scale,
                 lz + (py * math.sin(a) + pz * math.cos(a)) * scale) for (py, pz) in pts]

    P0, P1 = prof(tip_scale), prof(1.0)
    A = [bm.verts.new((x0, y, z)) for (y, z) in P0]
    B = [bm.verts.new((x1, y, z)) for (y, z) in P1]
    L = len(P0)
    for i in range(L):
        j = (i + 1) % L
        f = bm.faces.new((A[i], B[i], B[j], A[j]))
        f.material_index = s
    f = bm.faces.new(list(reversed(A))); f.material_index = s
    f = bm.faces.new(B); f.material_index = s
    te_y = ly + chord * math.cos(a)
    te_z = lz + chord * math.sin(a)
    return te_y, te_z


def gurney(b, mat, x0, x1, y, z, h=0.022, t=0.005, angle_deg=-8.0):
    """The lip along a wing's trailing edge."""
    a = math.radians(angle_deg) + math.pi / 2
    dy, dz = math.cos(a) * h, math.sin(a) * h
    ny, nz = -math.sin(a) * t, math.cos(a) * t
    prof = [(y, z), (y + dy, z + dz), (y + dy + ny, z + dz + nz), (y + ny, z + nz)]
    return plate(b, mat, prof, (x0 + x1) / 2, abs(x1 - x0))


def swan_neck(b, mat, x, root, tip, r=0.020, segs=8):
    """A wing stay that goes over the element: root -> knee -> tip."""
    root, tip = Vector(root), Vector(tip)
    knee = Vector((x, tip.y - 0.10, tip.z + 0.06))
    b.bar(mat, (x, root.y, root.z), knee, r, segs=segs)
    b.bar(mat, knee, (x, tip.y, tip.z), r * 0.85, segs=segs)


def splitter(b, mat, y0, y1, half_w, z, thick=0.016, fence_h=0.07, nose_r=0.010, fences=True):
    """A front splitter with a rolled leading edge and end fences, tucked
    under the nose rather than hanging in front of it."""
    prof = [(y0 + nose_r, z - thick / 2), (y1, z - thick / 2), (y1, z + thick / 2),
            (y0 + nose_r, z + thick / 2), (y0, z + thick / 2 - nose_r * 0.6),
            (y0, z - thick / 2 + nose_r * 0.6)]
    A = [b.bm.verts.new((-half_w, y, zz)) for (y, zz) in prof]
    B = [b.bm.verts.new((half_w, y, zz)) for (y, zz) in prof]
    s = b.slot(mat)
    n = len(prof)
    for i in range(n):
        j = (i + 1) % n
        f = b.bm.faces.new((A[i], B[i], B[j], A[j]))
        f.material_index = s
    f = b.bm.faces.new(list(reversed(A))); f.material_index = s
    f = b.bm.faces.new(B); f.material_index = s
    if fences:
        for sx in (-1, 1):
            fp = [(y0 + 0.02, z), (y1 - 0.02, z), (y1 - 0.02, z + fence_h * 0.6), (y0 + 0.06, z + fence_h)]
            plate(b, mat, fp, sx * (half_w - 0.012), 0.010)
    return A, B


def diffuser(b, mat, y0, y1, half_w, z0, z1, thick=0.014, strakes=(-0.66, -0.24, 0.24, 0.66),
             strake_h=0.20, fence=True):
    """A ramped diffuser with thickness, strakes that grow with the ramp and
    side fences - instead of a flat plank with slivers hanging under it."""
    prof = [(y0, z0 - thick), (y1, z1 - thick), (y1, z1), (y0, z0)]
    A = [b.bm.verts.new((-half_w, y, z)) for (y, z) in prof]
    B = [b.bm.verts.new((half_w, y, z)) for (y, z) in prof]
    s = b.slot(mat)
    for i in range(len(prof)):
        j = (i + 1) % len(prof)
        f = b.bm.faces.new((A[i], B[i], B[j], A[j]))
        f.material_index = s
    f = b.bm.faces.new(list(reversed(A))); f.material_index = s
    f = b.bm.faces.new(B); f.material_index = s
    for x in strakes:
        sp = [(y0 + 0.03, z0), (y1 - 0.02, z1), (y1 - 0.02, z1 + strake_h),
              (y0 + 0.03, z0 + strake_h * 0.35)]
        plate(b, mat, sp, x * half_w, 0.012, chamfer=0.006)
    if fence:
        for sx in (-1, 1):
            fp = [(y0, z0), (y1, z1), (y1, z1 + strake_h * 1.15), (y0, z0 + strake_h * 0.5)]
            plate(b, mat, fp, sx * (half_w - 0.010), 0.014)


def surface_station(loft, x, z, start, step=0.015, limit=1.2, margin=0.025):
    """The first station from `start` (searching towards the middle of the car)
    where the skin is at least `x` + `margin` half-wide at height `z`.

    Anything mounted on a corner - a lamp, a dive plane, a bumper duct - has
    to be put where the body is actually wide enough to hold it. Guessing a y
    near the nose leaves the part hanging in mid-air, because the nose station
    is the narrowest part of the car."""
    sign = 1.0 if start < (loft.nose + loft.tail) / 2 else -1.0
    y = start
    for _ in range(int(limit / step)):
        if loft.x_at(y, z) >= abs(x) + margin:
            return y
        y += sign * step
    return start + sign * limit


# ------------------------------------------------------------------ lights
def lamp_cluster(b, mats, x0, x1, z0, z1, y_face, depth=0.060, style="round", count=3,
                 dir_y=1.0, lens=True, glow=None, cup_r=None, housing=True, loft=None):
    """A lamp set into the bodywork: dark housing, emissive projector cups or
    bar behind a clear lens standing just proud of the surface.

    `y_face` is the outer face, `dir_y` points into the car (+1 at the nose,
    -1 at the tail). `glow` defaults to the headlight slot; pass
    `mats.brake`/`mats.tail`/`mats.rain` for the rear clusters."""
    glow = glow or mats.lamp
    yi = y_face + depth * dir_y
    if housing:
        b.box(mats.lamp_h, (x0, min(y_face, yi), z0), (x1, max(y_face, yi), z1))
    cz = (z0 + z1) / 2.0
    if style in ("round", "tri"):
        n = max(1, count)
        r = cup_r or min((x1 - x0) / (2.2 * n), (z1 - z0) * 0.40)
        span = (x1 - x0) - 2 * r - 0.012
        for k in range(n):
            cx = x0 + r + 0.006 + (span * (k / (n - 1)) if n > 1 else span / 2)
            # A nose is curved, so the skin is at a different station for each
            # cup. One shared y_face leaves the inboard cups poking out of the
            # bodywork like little tubes.
            cy = y_face
            if loft is not None:
                st = surface_station(loft, abs(cx) + r, cz,
                                     loft.nose if dir_y > 0 else loft.tail, margin=0.010)
                cy = st + 0.012 * dir_y
            b.cylinder(mats.lamp_h, (cx, cy + 0.004 * dir_y, cz), r,
                       (depth - 0.010) * dir_y, segs=20, axis='Y', caps=False)
            b.cylinder(glow, (cx, cy + (depth - 0.012) * dir_y, cz), r * 0.86,
                       0.010 * dir_y, segs=20, axis='Y')
    elif style == "bar":
        b.box(glow, (x0 + 0.010, min(y_face + 0.030 * dir_y, y_face + 0.040 * dir_y),
                     cz - (z1 - z0) * 0.22),
              (x1 - 0.010, max(y_face + 0.030 * dir_y, y_face + 0.040 * dir_y),
               cz + (z1 - z0) * 0.22))
    elif style == "ybar":
        b.box(glow, (x0 + 0.010, y_face + 0.026 * dir_y, cz + (z1 - z0) * 0.10),
              (x1 - 0.010, y_face + 0.034 * dir_y, cz + (z1 - z0) * 0.26))
        b.box(glow, (x0 + (x1 - x0) * 0.44, y_face + 0.026 * dir_y, z0 + 0.010),
              (x0 + (x1 - x0) * 0.56, y_face + 0.034 * dir_y, cz + (z1 - z0) * 0.12))
    if lens:
        b.box(mats.glass, (x0 + 0.004, min(y_face - 0.004 * dir_y, y_face + 0.008 * dir_y),
                           z0 + 0.004),
              (x1 - 0.004, max(y_face - 0.004 * dir_y, y_face + 0.008 * dir_y), z1 - 0.004))


def led_strip(b, mats, x0, x1, y, z, h=0.028, t=0.012, glow=None, dir_y=1.0):
    """A recessed LED bar: housing, emissive face, clear cover."""
    glow = glow or mats.brake
    b.box(mats.lamp_h, (x0, y, z - h / 2), (x1, y + t * dir_y, z + h / 2))
    b.box(glow, (x0 + 0.006, y - 0.002 * dir_y, z - h / 2 + 0.005),
          (x1 - 0.006, y + 0.004 * dir_y, z + h / 2 - 0.005))
    b.box(mats.glass, (x0 + 0.004, y - 0.006 * dir_y, z - h / 2 + 0.002),
          (x1 - 0.004, y - 0.001 * dir_y, z + h / 2 - 0.002))


# ------------------------------------------------------------------- vents
def louvre_bank(b, mat, x, y0, y1, z0, z1, count=5, blade_t=0.006, rake_deg=22.0,
                depth=0.030, sx=1):
    """Angled blades set into a vent on the flank (x is the flank surface)."""
    n = max(1, count)
    for k in range(n):
        t = (k + 0.5) / n
        z = z0 + (z1 - z0) * t
        dz = math.tan(math.radians(rake_deg)) * depth
        prof = [(y0, z), (y1, z), (y1, z - dz - blade_t), (y0, z - dz - blade_t)]
        plate(b, mat, prof, sx * (x - depth * 0.5), depth, chamfer=0.002)


def grille(b, mats, x0, x1, y, z0, z1, bars=7, t=0.007, depth=0.05, dir_y=1.0, backing=True):
    """A radiator aperture: recessed dark box with horizontal bars in it."""
    if backing:
        b.box(mats.mesh, (x0, min(y, y + depth * dir_y), z0),
              (x1, max(y, y + depth * dir_y), z1))
    n = max(1, bars)
    for k in range(n):
        z = z0 + (z1 - z0) * (k + 0.5) / n
        b.box(mats.carbon, (x0 + 0.004, min(y, y + 0.014 * dir_y), z - t / 2),
              (x1 - 0.004, max(y, y + 0.014 * dir_y), z + t / 2))


def duct_lip(b, mat, x0, x1, y, z0, z1, out=0.018, dir_y=1.0):
    """A raised surround around an intake, so the hole has an edge."""
    for (a, c) in (((x0 - out, z0 - out), (x0, z1 + out)),
                   ((x1, z0 - out), (x1 + out, z1 + out)),
                   ((x0, z0 - out), (x1, z0)),
                   ((x0, z1), (x1, z1 + out))):
        b.box(mat, (a[0], min(y, y + out * dir_y), a[1]),
              (c[0], max(y, y + out * dir_y), c[1]))


# ----------------------------------------------------------------- mirrors
def mirror(b, mats, x, y, z, sx=1, style="pod", head=(0.075, 0.140, 0.055)):
    """A GT mirror: aerofoil stalk and a teardrop head with a glass face -
    not a sphere on a stick."""
    hx, hy, hz = head
    stalk = [(y - 0.035, z - 0.10), (y + 0.035, z - 0.10), (y + 0.022, z), (y - 0.022, z)]
    plate(b, mats.carbon, stalk, sx * (x - 0.055), 0.024, chamfer=0.006)
    if style == "stalk":
        b.bar(mats.carbon, (sx * (x - 0.16), y, z), (sx * x, y - 0.01, z + 0.015), 0.012, segs=8)
    b.box(mats.carbon, (sx * x - hx / 2, y - hy / 2, z - hz / 2),
          (sx * x + hx / 2, y + hy / 2, z + hz / 2))
    b.box(mats.glass, (sx * x - hx / 2 + 0.008, y - hy / 2 + 0.006, z - hz / 2 + 0.008),
          (sx * x + hx / 2 - 0.008, y - hy / 2 + 0.012, z + hz / 2 - 0.008))


# ------------------------------------------------------------------ decals
def conform_decal(b, mat, loft, y0, y1, j0, j1, sx=1, lift=0.004, nu=10, nv=5, flip_u=False):
    """Lay a wordmark on the flank so it follows the surface, rather than
    standing a flat quad off a curved panel."""
    s = b.slot(mat)
    bm = b.bm
    grid = []
    for iu in range(nu + 1):
        u = iu / nu
        y = y0 + (y1 - y0) * u
        row = []
        for iv in range(nv + 1):
            v = iv / nv
            j = j0 + (j1 - j0) * v
            p = loft.point(y, j)
            n = loft.normal(y, j)
            q = p + n * lift
            row.append((bm.verts.new((sx * q.x, q.y, q.z)), (1 - u if flip_u else u, v)))
        grid.append(row)
    for iu in range(nu):
        for iv in range(nv):
            a, c = grid[iu][iv], grid[iu][iv + 1]
            d, e = grid[iu + 1][iv + 1], grid[iu + 1][iv]
            quad = (a, c, d, e) if sx > 0 else (a, e, d, c)
            f = bm.faces.new([q[0] for q in quad])
            f.material_index = s
            for l, q in zip(f.loops, quad):
                l[b.uv].uv = q[1]
            f.normal_update()
            if (f.normal.x > 0) != (sx > 0):
                f.normal_flip()
            b.keep.add(f)


# ----------------------------------------------------------------- cockpit
def cockpit_points(loft, wing_z, liner_z=-0.057, tail_pad=0.12, nose_pad=0.14):
    """Where the client will put the driver's eye, wheel and mirror.

    Mirrors `ApexCockpit::DeriveLayout` (closed style, docs/CAR_MODELS.md) so
    the interior can be built around the same points instead of by eye."""
    lo_z, hi_z = liner_z, wing_z + 0.22
    y0, y1 = loft.nose - nose_pad, loft.tail + tail_pad
    eye_z = lo_z + 0.70 * (hi_z - lo_z)
    eye_y = (y0 + y1) / 2 + 0.05 * (y1 - y0)
    # 18% of the car's WIDTH, not of the section's local width at eye height:
    # up by the roof the shell is barely a metre across, and measuring there
    # put the driver 18 cm inboard of where the client will actually sit him.
    eye_x = 0.18 * (2.0 * loft.max_half_width())
    return dict(eye=Vector((eye_x, eye_y, eye_z)),
                wheel=Vector((eye_x, eye_y - 0.40, eye_z - 0.20)),
                mirror=Vector((0.0, eye_y - 0.45, eye_z + 0.12)),
                dash_z=eye_z - 0.16)


def steering_wheel(b, mats, centre, r=0.160, rim_r=0.019, segs=28, flat_bottom=True):
    """A rim with a flat bottom, three spokes and a hub."""
    cx, cy, cz = centre
    ring = []
    for i in range(segs):
        a = 2 * math.pi * i / segs
        z = math.sin(a)
        if flat_bottom and z < -0.55:
            z = -0.55
        ring.append(Vector((cx + r * math.cos(a), cy, cz + r * z)))
    for i in range(segs):
        b.bar(mats.alc, ring[i], ring[(i + 1) % segs], rim_r, segs=8)
    for a in (math.radians(160), math.radians(20), math.radians(270)):
        b.bar(mats.alc, (cx, cy + 0.004, cz),
              (cx + r * 0.92 * math.cos(a), cy + 0.004, cz + r * 0.92 * math.sin(a)), 0.016, segs=6)
    b.cylinder(mats.alc, (cx, cy - 0.010, cz), 0.052, 0.036, segs=18, axis='Y')
    b.box(mats.display, (cx - 0.055, cy - 0.013, cz - 0.026), (cx + 0.055, cy - 0.011, cz + 0.026))


# ------------------------------------------------------------- join, export
def ui_context():
    """A context override with a real window/screen/area.

    Driven over the MCP bridge there is no UI context, and the glTF exporter
    reads `bpy.context.active_object`, so it raises before it writes anything.
    Handing it a window from the window manager makes the export work the same
    way it does from Blender's own console."""
    wm = bpy.context.window_manager
    if not wm or not wm.windows:
        return None
    win = wm.windows[0]
    scr = win.screen
    area = next((a for a in scr.areas if a.type == 'VIEW_3D'), None) or (scr.areas[0] if scr.areas else None)
    ctx = dict(window=win, screen=scr, scene=bpy.context.scene,
               view_layer=bpy.context.view_layer)
    if area:
        ctx["area"] = area
        region = next((r for r in area.regions if r.type == 'WINDOW'), None)
        if region:
            ctx["region"] = region
        if area.spaces and area.spaces.active:
            ctx["space_data"] = area.spaces.active
    return ctx


def export_glb(ob, path):
    """Select `ob` alone and write it as a GLB."""
    for o in bpy.context.scene.objects:
        o.select_set(o is ob)
    ctx = ui_context() or {}
    kw = dict(filepath=path, export_format='GLB', use_selection=True, export_apply=True,
              export_yup=True, export_texcoords=True, export_normals=True,
              export_materials='EXPORT', export_cameras=False, export_lights=False)
    with bpy.context.temp_override(active_object=ob, object=ob,
                                   selected_objects=[ob], selected_editable_objects=[ob],
                                   **ctx):
        bpy.context.view_layer.objects.active = ob
        bpy.ops.export_scene.gltf(**kw)
    return path


def join_and_export(objs, stem, car_dir, export=True, hide=True):
    """Merge the build's objects into one mesh named `stem` and write
    <car_dir>/<stem>.glb. Material slots are unified, keeping their names."""
    joined = bmesh.new()
    mat_list = []
    for o in objs:
        me = o.data
        remap = []
        for m in me.materials:
            if m not in mat_list:
                mat_list.append(m)
            remap.append(mat_list.index(m))
        tmp = bmesh.new()
        tmp.from_mesh(me)
        for f in tmp.faces:
            f.material_index = remap[f.material_index] if remap else 0
        tmp_me = bpy.data.meshes.new("tmp")
        tmp.to_mesh(tmp_me)
        tmp.free()
        joined.from_mesh(tmp_me)
        bpy.data.meshes.remove(tmp_me)
    final = bpy.data.meshes.new(stem)
    joined.to_mesh(final)
    joined.free()
    for m in mat_list:
        final.materials.append(m)
    car = bpy.data.objects.new(stem, final)
    bpy.context.scene.collection.objects.link(car)
    sharpen(car, 38.0)
    if hide:
        for o in objs:
            o.hide_render = True
            o.hide_viewport = True
    glb = None
    if export:
        glb = export_glb(car, os.path.join(car_dir, stem + ".glb"))
    return car, glb


def mesh_stats(ob):
    d = ob.data
    return dict(verts=len(d.vertices), tris=sum(len(p.vertices) - 2 for p in d.polygons),
                mats=[m.name for m in d.materials],
                bbox_min=[round(min(v.co[i] for v in d.vertices), 3) for i in range(3)],
                bbox_max=[round(max(v.co[i] for v in d.vertices), 3) for i in range(3)])
