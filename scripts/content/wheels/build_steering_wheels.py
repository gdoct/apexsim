"""Build the class steering wheels the cockpit rig draws: content/wheels/steering/<class>.glb.

    python scripts/content/wheels/build_steering_wheels.py          # every class
    python scripts/content/wheels/build_steering_wheels.py gt3 f1   # a few

Plain Python and numpy (no Blender), so it runs anywhere the other content
scripts do and writes byte-identical files every run.

One wheel per `[wheels] model` class (f1, lmp2, hypercar, gt3). A car whose
car.toml names no `[cockpit] steering_wheel_model` of its own is given the
wheel of its class by the client (`UApexCarContentSubsystem`), which the rig
draws and turns in place of its primitive rim.

Frame (docs/content/CAR_MODELS.md, `steering_wheel_model`): metres, hub at
the origin, the wheel straight and upright, the rim in glTF XY (+Y up, +X the
car's left) and the column along +Z toward the nose, so the driver looks at
the face from -Z. It is built here in centimetres in a driver's frame (u to
the driver's right, v up, w toward the driver) and turned into glTF at the
end: X = -u, Y = v, Z = -w (a half turn about Y, so no winding flips).

Beside each GLB a `<class>.json` says where the rig's hub display goes:
`dash_cm`, the display's centre in the rig's wheel pivot frame (cm, +X along
the column toward the nose, +Y the driver's right, +Z up) just proud of the
wheel's screen glass, and `dash_width_cm`, the glass width (the display is
512 x 224, so the glass is drawn at that aspect).

Materials (slot names): `sw_carbon` (a generated twill texture),
`sw_carbon_back`, `sw_grip` (alcantara / rubber), `sw_metal`, `sw_metal_dark`,
`sw_screen`, `sw_bezel`, `sw_mark` (printed white), `sw_btn_<colour>`,
`sw_knob_<colour>`, `sw_led_<colour>`, `sw_marker`.
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys
import zlib

import numpy as np

ROOT = os.environ.get("APEXSIM_ROOT") or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "content", "wheels", "steering")

DASH_ASPECT = 512.0 / 224.0      # UApexCockpitDashWidget::DrawWidth / DrawHeight
DASH_PROUD_CM = 0.12             # the display floats this far in front of the glass
CARBON_TILE_CM = 2.4             # one repeat of the twill texture


# --------------------------------------------------------------------------
# Geometry
# --------------------------------------------------------------------------

def _norm(v):
    v = np.asarray(v, dtype=np.float64)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.where(n < 1e-12, 1.0, n)


def outline_normals(pts):
    """Outward vertex normals of a closed CCW polygon (u, v)."""
    p = np.asarray(pts, dtype=np.float64)
    e = np.roll(p, -1, axis=0) - p
    en = _norm(np.stack([e[:, 1], -e[:, 0]], axis=1))
    return _norm(en + np.roll(en, 1, axis=0))


def signed_area(pts):
    p = np.asarray(pts)
    return 0.5 * float(np.sum(p[:, 0] * np.roll(p[:, 1], -1) - np.roll(p[:, 0], -1) * p[:, 1]))


def ccw(pts):
    p = np.asarray(pts, dtype=np.float64)
    return p if signed_area(p) > 0 else p[::-1].copy()


def ear_clip(pts):
    """Triangles (index triples, CCW) of a simple CCW polygon."""
    p = np.asarray(pts, dtype=np.float64)
    idx = list(range(len(p)))
    tris = []

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    guard = 0
    while len(idx) > 3 and guard < 10000:
        guard += 1
        n = len(idx)
        found = False
        for k in range(n):
            i0, i1, i2 = idx[(k - 1) % n], idx[k], idx[(k + 1) % n]
            a, b, c = p[i0], p[i1], p[i2]
            if cross(a, b, c) <= 1e-12:
                continue
            inside = False
            for j in idx:
                if j in (i0, i1, i2):
                    continue
                q = p[j]
                if cross(a, b, q) >= 0 and cross(b, c, q) >= 0 and cross(c, a, q) >= 0:
                    inside = True
                    break
            if not inside:
                tris.append((i0, i1, i2))
                idx.pop(k)
                found = True
                break
        if not found:
            # Degenerate leftovers: fan them.
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
            return tris
    if len(idx) == 3:
        tris.append(tuple(idx))
    return tris


def rrect(cx, cy, w, h, r, seg=6):
    """A rounded rectangle, CCW."""
    r = min(r, w / 2 - 1e-3, h / 2 - 1e-3)
    pts = []
    for (ox, oy, a0) in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90),
                         (-w / 2 + r, -h / 2 + r, 180), (w / 2 - r, -h / 2 + r, 270)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90 * i / seg)
            pts.append((cx + ox + r * math.cos(a), cy + oy + r * math.sin(a)))
    return np.array(pts)


def circle(cx, cy, r, seg=24, phase=0.0):
    a = np.linspace(0, 2 * math.pi, seg, endpoint=False) + phase
    return np.stack([cx + r * np.cos(a), cy + r * np.sin(a)], axis=1)


def rounded_poly(pts, r, seg=4):
    """A CCW polygon with each corner rounded by r (clamped to half the shorter edge)."""
    p = ccw(pts)
    n = len(p)
    out = []
    for i in range(n):
        a, b, c = p[i - 1], p[i], p[(i + 1) % n]
        d1, d2 = _norm(a - b), _norm(c - b)
        l1, l2 = np.linalg.norm(a - b), np.linalg.norm(c - b)
        ang = math.acos(max(-1.0, min(1.0, float(np.dot(d1, d2)))))
        if ang > math.pi - 1e-3:
            out.append(b)
            continue
        t = r / math.tan(ang / 2)
        t = min(t, 0.45 * l1, 0.45 * l2)
        rr = t * math.tan(ang / 2)
        p1, p2 = b + d1 * t, b + d2 * t
        centre = b + _norm(d1 + d2) * (rr / math.sin(ang / 2))
        a1 = math.atan2(*(p1 - centre)[::-1])
        a2 = math.atan2(*(p2 - centre)[::-1])
        da = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi
        for k in range(seg + 1):
            ak = a1 + da * k / seg
            out.append(centre + rr * np.array([math.cos(ak), math.sin(ak)]))
    return np.array(out)


class Frame:
    """An origin and three axes; local (x, y, z) -> origin + x e1 + y e2 + z e3."""

    def __init__(self, origin=(0, 0, 0), e1=(1, 0, 0), e2=(0, 1, 0), e3=(0, 0, 1)):
        self.o = np.array(origin, dtype=np.float64)
        self.m = np.stack([_norm(e1), _norm(e2), _norm(e3)], axis=0)

    def p(self, local):
        return self.o + np.asarray(local, dtype=np.float64) @ self.m

    def n(self, local):
        return _norm(np.asarray(local, dtype=np.float64) @ self.m)


FACE = Frame()  # the face plane: x = u, y = v, z = w


def at(u, v, w=0.0):
    return Frame((u, v, w))


def axis_frame(origin, axis):
    """A frame whose z is `axis`."""
    z = _norm(axis)
    helper = np.array([0, 0, 1.0]) if abs(z[2]) < 0.9 else np.array([1.0, 0, 0])
    x = _norm(np.cross(helper, z))
    y = np.cross(z, x)
    return Frame(origin, x, y, z)


class Mesh:
    def __init__(self):
        self.parts: dict[str, list] = {}

    def add(self, mat, P, N, I, UV=None):
        P = np.asarray(P, dtype=np.float64)
        N = _norm(N)
        I = np.asarray(I, dtype=np.int64).reshape(-1, 3)
        if len(I) == 0:
            return
        # Every generator's winding is checked against its own normals.
        a, b, c = P[I[:, 0]], P[I[:, 1]], P[I[:, 2]]
        fn = np.cross(b - a, c - a)
        vn = N[I[:, 0]] + N[I[:, 1]] + N[I[:, 2]]
        flip = np.einsum("ij,ij->i", fn, vn) < 0
        I = I.copy()
        I[flip] = I[flip][:, [0, 2, 1]]
        if UV is None:
            UV = P[:, :2] / CARBON_TILE_CM
        self.parts.setdefault(mat, []).append((P, N, np.asarray(UV, dtype=np.float64), I))

    def triangles(self):
        return sum(len(i) for parts in self.parts.values() for (_, _, _, i) in parts)

    # --- primitives -------------------------------------------------------

    def extrude(self, mat, outline, z0, z1, frame=FACE, chamfer=0.0, chamfer_back=0.0, back=True, uv_scale=None):
        """A prism of a closed outline from z0 to z1 in `frame`, its front edge chamfered."""
        o = ccw(outline)
        nrm = outline_normals(o)
        ch = min(chamfer, (z1 - z0) * 0.45)
        chb = min(chamfer_back, (z1 - z0) * 0.45)
        P, N, I = [], [], []

        def ring(pts, z):
            return [np.array([x, y, z]) for x, y in pts]

        def band(r_hi, r_lo, n_hi, n_lo):
            base = len(P)
            k = len(r_hi)
            for j in range(k):
                P.append(r_hi[j]); N.append(n_hi[j])
            for j in range(k):
                P.append(r_lo[j]); N.append(n_lo[j])
            for j in range(k):
                j1 = (j + 1) % k
                I.append((base + j, base + k + j, base + k + j1))
                I.append((base + j, base + k + j1, base + j1))

        side_n = [np.array([nx, ny, 0.0]) for nx, ny in nrm]
        front_in = o - nrm * ch
        back_in = o - nrm * chb
        if ch > 0:
            cn = [_norm(np.array([nx, ny, 1.0])) for nx, ny in nrm]
            band(ring(front_in, z1), ring(o, z1 - ch), cn, cn)
        band(ring(o, z1 - ch), ring(o, z0 + chb), side_n, side_n)
        if chb > 0 and back:
            bn = [_norm(np.array([nx, ny, -1.0])) for nx, ny in nrm]
            band(ring(o, z0 + chb), ring(back_in, z0), bn, bn)
        tris = ear_clip(front_in)
        base = len(P)
        for x, y in front_in:
            P.append(np.array([x, y, z1])); N.append(np.array([0, 0, 1.0]))
        I += [(base + a, base + b, base + c) for a, b, c in tris]
        if back:
            tris = ear_clip(back_in)
            base = len(P)
            for x, y in back_in:
                P.append(np.array([x, y, z0])); N.append(np.array([0, 0, -1.0]))
            I += [(base + a, base + c, base + b) for a, b, c in tris]
        P, N = np.array(P), np.array(N)
        uv = P[:, :2] / (uv_scale or CARBON_TILE_CM)
        self.add(mat, frame.p(P), frame.n(N), I, uv)

    def box(self, mat, cu, cv, w0, w1, su, sv, frame=FACE, chamfer=0.0, angle=0.0):
        pts = np.array([(-su / 2, -sv / 2), (su / 2, -sv / 2), (su / 2, sv / 2), (-su / 2, sv / 2)])
        c, s = math.cos(angle), math.sin(angle)
        pts = pts @ np.array([[c, s], [-s, c]]) + (cu, cv)
        self.extrude(mat, pts, w0, w1, frame, chamfer=chamfer)

    def cylinder(self, mat, frame, r, z0, z1, seg=24, chamfer=0.0, chamfer_back=0.0):
        self.extrude(mat, circle(0, 0, r, seg), z0, z1, frame, chamfer=chamfer, chamfer_back=chamfer_back)

    def sweep(self, mat, path, section, up=(0, 0, 1), closed=False, scales=None, caps=True):
        """A section (closed CCW outline in (across, up)) swept along a polyline."""
        path = np.asarray(path, dtype=np.float64)
        sec = ccw(section)
        sn = outline_normals(sec)
        m, k = len(path), len(sec)
        if closed:
            t = path[(np.arange(m) + 1) % m] - path[(np.arange(m) - 1) % m]
        else:
            t = np.gradient(path, axis=0)
        t = _norm(t)
        upv = np.array(up, dtype=np.float64)
        side = _norm(np.cross(t, upv))
        upp = np.cross(side, t)
        sc = np.ones((m, 2)) if scales is None else np.asarray(scales, dtype=np.float64).reshape(m, -1) * np.ones((m, 2))
        P = (path[:, None, :] + side[:, None, :] * (sec[None, :, 0:1] * sc[:, None, 0:1])
             + upp[:, None, :] * (sec[None, :, 1:2] * sc[:, None, 1:2]))
        # Normals of a scaled section: scale the 2D normal by the inverse scale.
        nx = sn[None, :, 0:1] / sc[:, None, 0:1]
        ny = sn[None, :, 1:2] / sc[:, None, 1:2]
        N = side[:, None, :] * nx + upp[:, None, :] * ny
        P = P.reshape(-1, 3)
        N = N.reshape(-1, 3)
        I = []
        rows = m if closed else m - 1
        for j in range(rows):
            j1 = (j + 1) % m
            for i in range(k):
                i1 = (i + 1) % k
                a, b, c, d = j * k + i, j * k + i1, j1 * k + i1, j1 * k + i
                I += [(a, b, c), (a, c, d)]
        # Along the path for u, around the section for v.
        seglen = np.r_[0, np.cumsum(np.linalg.norm(np.diff(path, axis=0), axis=1))]
        uv = np.stack([np.repeat(seglen, k) / CARBON_TILE_CM, np.tile(np.arange(k) / k * 4.0, m)], axis=1)
        self.add(mat, P, N, I, uv)
        if caps and not closed:
            for end, sign in ((0, -1.0), (m - 1, 1.0)):
                ring = P.reshape(m, k, 3)[end]
                tris = ear_clip(sec)
                n = np.repeat((t[end] * sign)[None, :], k, axis=0)
                self.add(mat, ring, n, tris)

    # --- parts ------------------------------------------------------------

    def button(self, u, v, w, r, colour, frame=FACE):
        self.cylinder("sw_metal_dark", Frame(frame.p((u, v, w))), r + 0.2, 0.0, 0.22, seg=20, chamfer=0.06)
        self.cylinder(f"sw_btn_{colour}", Frame(frame.p((u, v, w))), r, 0.0, 0.5, seg=20, chamfer=0.14)

    def rotary(self, u, v, w, r, colour, frame=FACE, ticks=10, pointer_deg=0.0, height=0.95):
        f = Frame(frame.p((u, v, w)))
        # Printed scale round the knob.
        for k in range(ticks):
            a = math.radians(-135 + 270 * k / max(1, ticks - 1))
            ru = r + 0.55
            self.box("sw_mark", ru * -math.sin(a), ru * math.cos(a), 0.0, 0.04, 0.09, 0.38 if k % 3 == 0 else 0.24,
                     frame=f, angle=a)
        self.cylinder("sw_metal_dark", f, r + 0.25, 0.0, 0.25, seg=24, chamfer=0.06)
        teeth = 28
        knurl = []
        for i in range(teeth * 2):
            a = math.pi * 2 * i / (teeth * 2)
            rr = r if i % 2 == 0 else r - 0.08
            knurl.append((rr * math.cos(a), rr * math.sin(a)))
        self.extrude(f"sw_knob_{colour}", np.array(knurl), 0.2, height, f, chamfer=0.16)
        a = math.radians(pointer_deg)
        self.box("sw_mark", -math.sin(a) * r * 0.5, math.cos(a) * r * 0.5, height - 0.02, height + 0.03, 0.14, r * 0.8,
                 frame=f, angle=a)

    def joystick(self, u, v, w, frame=FACE):
        f = Frame(frame.p((u, v, w)))
        self.cylinder("sw_metal_dark", f, 0.95, 0.0, 0.25, seg=24, chamfer=0.06)
        self.cylinder("sw_metal", f, 0.55, 0.2, 0.8, seg=20, chamfer=0.1)
        self.cylinder("sw_btn_black", f, 0.62, 0.8, 1.1, seg=20, chamfer=0.12)

    def screen(self, cu, cv, w_face, glass_w, frame=FACE):
        """A bezel and glass; returns the glass's front w."""
        glass_h = glass_w / DASH_ASPECT
        bz = 0.55
        self.extrude("sw_bezel", rrect(cu, cv, glass_w + 2 * bz, glass_h + 2 * bz, 0.6),
                     w_face - 0.05, w_face + 0.45, frame, chamfer=0.18)
        glass_front = w_face + 0.47
        self.extrude("sw_screen", rrect(cu, cv, glass_w, glass_h, 0.15), w_face + 0.3, glass_front, frame)
        return glass_front, glass_h

    def leds(self, cu, cv, w, count, pitch, colours, frame=FACE):
        x0 = cu - pitch * (count - 1) / 2
        for i in range(count):
            col = colours[min(len(colours) - 1, i * len(colours) // count)]
            self.extrude("sw_bezel", rrect(x0 + i * pitch, cv, pitch * 0.82, 0.62, 0.12), w - 0.05, w + 0.18, frame)
            self.extrude(f"sw_led_{col}", rrect(x0 + i * pitch, cv, pitch * 0.6, 0.4, 0.1), w + 0.1, w + 0.26, frame)

    def column(self, r_hub, w_back):
        """The hub behind the faceplate and the quick release, along the column toward the nose."""
        f = axis_frame((0, 0, 0), (0, 0, -1))      # local z = -w: toward the nose
        z0 = w_back - 0.4                          # starts inside the back of the plate
        self.cylinder("sw_metal_dark", f, r_hub, z0, z0 + 2.6, seg=32, chamfer=0.3, chamfer_back=0.3)
        self.cylinder("sw_metal", f, r_hub * 0.82, z0 + 2.6, z0 + 3.8, seg=32, chamfer=0.15, chamfer_back=0.15)
        self.cylinder("sw_metal_dark", f, r_hub * 0.62, z0 + 3.8, z0 + 8.0, seg=24, chamfer=0.1)

    def paddle(self, mat, outline, w0, w1):
        self.extrude(mat, rounded_poly(outline, 0.6), w0, w1, chamfer=0.15, chamfer_back=0.15)


def arc(cu, cv, r, a0, a1, n):
    a = np.radians(np.linspace(a0, a1, n))
    return np.stack([cu + r * np.cos(a), cv + r * np.sin(a)], axis=1)


def smooth_path(ctrl, n=40):
    """A Catmull-Rom curve through 2D control points (open), as (u, v)."""
    c = np.asarray(ctrl, dtype=np.float64)
    c = np.vstack([2 * c[0] - c[1], c, 2 * c[-1] - c[-2]])
    out = []
    segs = len(c) - 3
    per = max(2, n // segs)
    for s in range(segs):
        p0, p1, p2, p3 = c[s:s + 4]
        for i in range(per):
            t = i / per
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(c[-2])
    return np.array(out)


def lift(path2d, w):
    p = np.asarray(path2d, dtype=np.float64)
    return np.column_stack([p, np.full(len(p), w)])


def ellipse(rx, ry, seg=18):
    a = np.linspace(0, 2 * math.pi, seg, endpoint=False)
    return np.stack([rx * np.cos(a), ry * np.sin(a)], axis=1)


def grip_section(rx, ry, seg=20):
    """A grip's section: a rounded, slightly flattened oval (across the rim x, along the column y)."""
    a = np.linspace(0, 2 * math.pi, seg, endpoint=False)
    sq = np.sign(np.cos(a)) * np.abs(np.cos(a)) ** 0.8, np.sign(np.sin(a)) * np.abs(np.sin(a)) ** 0.8
    return np.stack([rx * sq[0], ry * sq[1]], axis=1)


def mirror_u(pts):
    p = np.asarray(pts, dtype=np.float64).copy()
    p[:, 0] = -p[:, 0]
    return p[::-1]


# --------------------------------------------------------------------------
# The classes
# --------------------------------------------------------------------------

def build_formula():
    """A modern formula wheel: an open-topped carbon faceplate between two
    moulded rubber grips, a small screen high in the middle, a rev-light
    strip over it and rotaries in the lower corners."""
    m = Mesh()
    # Faceplate: wide at the grips, a chin under the screen.
    half = [(0, -6.6), (4.0, -6.6), (7.2, -5.6), (10.2, -4.4), (11.2, -1.5), (11.0, 4.6),
            (9.4, 6.4), (6.6, 6.7)]
    plate = rounded_poly(np.vstack([np.array(half[1:]), mirror_u(half)[:-1]]), 1.2)
    m.extrude("sw_carbon", plate, -1.3, 0.35, chamfer=0.3, chamfer_back=0.2)
    m.extrude("sw_carbon_back", rounded_poly([(-8.5, -5.8), (8.5, -5.8), (8.5, 5.4), (-8.5, 5.4)], 2.0),
              -2.4, -1.25, chamfer_back=0.4, back=True)
    # Grips: moulded, thick, swept forward at the top.
    for s in (-1, 1):
        ctrl = [(s * 11.0, -7.2), (s * 12.4, -4.0), (s * 12.9, 0.0), (s * 12.6, 3.6), (s * 11.6, 6.0)]
        path = lift(smooth_path(ctrl, 36), -0.55)
        n = len(path)
        thick = 1.0 + 0.12 * np.sin(np.linspace(0, math.pi, n))
        m.sweep("sw_grip", path, grip_section(1.75, 2.15), up=(0, 0, 1), scales=np.stack([thick, thick], 1))
    glass_front, glass_h = m.screen(0.0, 2.3, 0.35, 10.8)
    m.leds(0.0, 5.55, 0.35, 15, 0.72, ["green", "red", "blue"])
    # Buttons beside the screen.
    for s, cols in ((-1, ["red", "yellow", "white"]), (1, ["blue", "green", "white"])):
        for i, c in enumerate(cols):
            m.button(s * 8.1, 4.0 - i * 2.05, 0.35, 0.62, c)
        m.button(s * 9.75, 1.0, 0.35, 0.5, "black")
    # The lower corners: two rotaries a side, small buttons between.
    m.rotary(-7.4, -3.0, 0.35, 1.2, "red", pointer_deg=40)
    m.rotary(7.4, -3.0, 0.35, 1.2, "blue", pointer_deg=-30)
    m.rotary(-3.7, -4.6, 0.35, 0.95, "orange", pointer_deg=0, ticks=8)
    m.rotary(3.7, -4.6, 0.35, 0.95, "yellow", pointer_deg=60, ticks=8)
    m.joystick(0.0, -3.9, 0.35)
    for i, c in enumerate(["orange", "white", "white", "purple"]):
        m.button(-4.2 + i * 2.8, -1.35, 0.35, 0.48, c)
    # Paddles: shift high and wide, clutch below.
    for s in (-1, 1):
        shift = [(s * 6.0, 4.6), (s * 13.5, 3.8), (s * 16.8, 1.8), (s * 16.4, 0.2), (s * 12.0, 0.6), (s * 6.0, 1.0)]
        m.paddle("sw_carbon", shift, -3.3, -2.7)
        clutch = [(s * 4.0, -2.2), (s * 11.5, -3.0), (s * 14.4, -5.0), (s * 13.6, -6.2), (s * 9.0, -5.4), (s * 4.0, -4.6)]
        m.paddle("sw_metal", clutch, -4.0, -3.55)
    m.column(2.4, 2.4)
    return m, (glass_front, 2.3, 10.8)


def build_endurance(hyper):
    """A prototype's wheel: a big closed carbon frame, grips on both sides
    joined by a bar over the top, a wide screen and banks of coloured
    buttons for the long night (a hypercar's a size up and more ornate)."""
    m = Mesh()
    hw = 13.4 if hyper else 13.0          # grip centreline
    top = 7.6 if hyper else 7.2
    bot = -7.6 if hyper else -7.4
    glass_w = 13.6 if hyper else 12.6
    screen_v = 2.6
    # Faceplate.
    if hyper:
        half = [(0, bot + 0.3), (5.0, bot + 0.3), (9.4, bot + 1.6), (11.6, -4.0), (11.8, 4.2), (10.4, 6.0), (6.0, 6.2)]
    else:
        half = [(0, bot + 0.4), (8.2, bot + 0.4), (11.4, -5.0), (11.6, 4.8), (9.0, 6.0)]
    plate = rounded_poly(np.vstack([np.array(half[1:]), mirror_u(half)[:-1]]), 1.4)
    m.extrude("sw_carbon", plate, -1.4, 0.4, chamfer=0.3, chamfer_back=0.2)
    m.extrude("sw_carbon_back", rrect(0, -0.8, 18.0, 11.0, 2.5), -2.6, -1.3, chamfer_back=0.4)
    # Grips and the top bar, one closed loop with the faceplate's chin.
    for s in (-1, 1):
        ctrl = [(s * (hw - 1.2), bot + 0.2), (s * hw, -4.5), (s * (hw + 0.3), 0.0), (s * hw, 4.2),
                (s * (hw - 1.0), top - 0.6)]
        path = lift(smooth_path(ctrl, 36), -0.5)
        n = len(path)
        thick = 1.0 + 0.1 * np.sin(np.linspace(0, math.pi, n))
        m.sweep("sw_grip", path, grip_section(1.8, 2.2), scales=np.stack([thick, thick], 1))
    bar = lift(smooth_path([(-(hw - 1.2), top - 0.9), (-(hw - 3.4), top), (0, top + 0.15), (hw - 3.4, top),
                            (hw - 1.2, top - 0.9)], 30), -0.5)
    m.sweep("sw_carbon", bar, rrect(0, 0, 1.6, 2.0, 0.6, 3))
    # Struts from the bar down to the faceplate.
    for s in (-1, 1):
        m.box("sw_carbon", s * 6.8, 6.6, -1.2, 0.2, 1.6, 1.6, chamfer=0.25)
    glass_front, glass_h = m.screen(0.0, screen_v, 0.4, glass_w)
    m.leds(0.0, top, 0.55, 15 if hyper else 13, 0.78, ["green", "red", "blue"])
    # Button banks: two columns a side beside the screen.
    if hyper:
        banks = {-1: [["yellow", "purple", "white"], ["red", "blue", "green"]],
                 1: [["blue", "green", "orange"], ["white", "yellow", "purple"]]}
    else:
        banks = {-1: [["red", "white", "yellow"], ["blue", "black", "green"]],
                 1: [["green", "black", "blue"], ["yellow", "white", "orange"]]}
    for s in (-1, 1):
        for c, col in enumerate(banks[s]):
            for r, colour in enumerate(col):
                m.button(s * (8.75 + c * 1.85), 4.6 - r * 1.85, 0.4, 0.66, colour)
    # Lower field: three rotaries a side, a pair of funky switches, a row of small buttons.
    knobs = ["orange", "blue", "red"] if hyper else ["red", "blue", "yellow"]
    for s in (-1, 1):
        m.rotary(s * 9.3, -2.4, 0.4, 1.15, knobs[0], pointer_deg=s * 35)
        m.rotary(s * 6.6, -4.9, 0.4, 1.05, knobs[1], pointer_deg=-s * 20)
        m.rotary(s * 3.4, -2.9, 0.4, 0.9, knobs[2], pointer_deg=s * 60, ticks=8)
        m.joystick(s * 9.8, -5.6, 0.4)
    m.rotary(0.0, -4.8, 0.4, 1.25, "silver" if hyper else "red", pointer_deg=0, ticks=12)
    for i, c in enumerate(["white", "orange", "green", "orange", "white"]):
        m.button(-4.4 + i * 2.2, -0.75, 0.4, 0.5, c)
    # Paddles.
    for s in (-1, 1):
        shift = [(s * 7.0, 4.4), (s * 14.0, 4.0), (s * 17.2, 2.2), (s * 16.8, 0.4), (s * 12.0, 0.6), (s * 7.0, 1.2)]
        m.paddle("sw_carbon", shift, -3.6, -3.0)
        clutch = [(s * 5.0, -2.6), (s * 12.0, -3.2), (s * 15.0, -5.2), (s * 14.0, -6.4), (s * 9.0, -5.8), (s * 5.0, -5.0)]
        m.paddle("sw_metal", clutch, -4.3, -3.85)
    m.column(2.6, 2.6)
    return m, (glass_front, screen_v, glass_w)


def build_gt():
    """A GT3 wheel: a round rim with a flat bottom in alcantara with a top
    marker, three carbon spokes into a central button box with the screen
    in it, and big aluminium shift paddles."""
    m = Mesh()
    R = 16.4
    flat = -13.6
    # The rim's centreline: a circle cut by a chord at `flat`.
    a_flat = math.degrees(math.asin(flat / R))     # negative
    a1 = 180 - a_flat          # the left end of the flat; a_flat is the right end
    # From the right end of the flat up over the top and down to the left end.
    upper = arc(0, 0, R, a_flat, a1, 120)
    chord_u = R * math.cos(math.radians(a_flat))
    lower = np.stack([np.linspace(-chord_u, chord_u, 24)[1:-1], np.full(22, flat)], axis=1)
    loop = np.vstack([upper, lower])
    path = lift(loop, -0.6)
    n = len(path)
    # Thicker at the hands (3 and 9 o'clock).
    ang = np.arctan2(loop[:, 1], loop[:, 0])
    hand = np.exp(-((np.abs(np.cos(ang)) - 1.0) ** 2) / 0.05) * (loop[:, 1] > flat + 0.5)
    sc = 1.0 + 0.16 * hand
    m.sweep("sw_grip", path, grip_section(1.55, 1.95), closed=True, scales=np.stack([sc, sc * 1.05], 1))
    # Top dead centre marker.
    marker = lift(arc(0, 0, R, 85, 95, 8), -0.6)
    m.sweep("sw_marker", marker, grip_section(1.6, 2.0), scales=np.full((8, 2), 1.07))
    # Spokes.
    for s in (-1, 1):
        spoke = [(s * 9.6, 2.6), (s * (R - 0.4), 1.6), (s * (R - 0.4), -2.6), (s * 9.6, -3.6)]
        m.extrude("sw_carbon", rounded_poly(spoke, 0.5), -1.6, -0.3, chamfer=0.2)
    m.extrude("sw_carbon", rounded_poly([(-3.4, -5.0), (3.4, -5.0), (2.8, flat + 0.4), (-2.8, flat + 0.4)], 0.6),
              -1.6, -0.3, chamfer=0.2)
    # The button box.
    box = rounded_poly([(-10.4, -5.6), (10.4, -5.6), (10.8, 4.8), (7.6, 6.4), (-7.6, 6.4), (-10.8, 4.8)], 1.4)
    m.extrude("sw_carbon", box, -1.5, 0.5, chamfer=0.3, chamfer_back=0.2)
    m.extrude("sw_carbon_back", rrect(0, -0.4, 16.0, 9.0, 2.0), -2.7, -1.4, chamfer_back=0.4)
    glass_w = 10.0
    glass_front, glass_h = m.screen(0.0, 2.4, 0.5, glass_w)
    m.leds(0.0, 5.55, 0.5, 11, 0.74, ["green", "red", "blue"])
    for s, cols in ((-1, [["red", "yellow"], ["white", "green"]]), (1, [["blue", "white"], ["orange", "black"]])):
        for c, col in enumerate(cols):
            for r, colour in enumerate(col):
                m.button(s * (6.9 + c * 1.9), 3.4 - r * 1.95, 0.5, 0.68, colour)
    m.rotary(-5.6, -3.0, 0.5, 1.2, "orange", pointer_deg=30)
    m.rotary(5.6, -3.0, 0.5, 1.2, "orange", pointer_deg=-45)
    m.rotary(0.0, -3.5, 0.5, 1.0, "silver", pointer_deg=0, ticks=8)
    for i, c in enumerate(["white", "red", "white"]):
        m.button(-2.6 + i * 2.6, -0.8, 0.5, 0.5, c)
    m.button(-8.8, -3.6, 0.5, 0.55, "black")
    m.button(8.8, -3.6, 0.5, 0.55, "black")
    # Big paddles behind the spokes.
    for s in (-1, 1):
        shift = [(s * 8.0, 3.6), (s * 15.0, 4.6), (s * 17.6, 3.4), (s * 17.9, 1.0), (s * 15.0, -0.4), (s * 8.0, 0.4)]
        m.paddle("sw_metal", shift, -3.6, -3.0)
    m.column(2.9, 2.7)
    return m, (glass_front, 2.4, glass_w)


CLASSES = {
    "f1": build_formula,
    "lmp2": lambda: build_endurance(hyper=False),
    "hypercar": lambda: build_endurance(hyper=True),
    "gt3": build_gt,
}

# --------------------------------------------------------------------------
# Materials
# --------------------------------------------------------------------------

BUTTON = {
    "red": (0.55, 0.02, 0.015), "yellow": (0.75, 0.48, 0.02), "blue": (0.02, 0.12, 0.6),
    "green": (0.03, 0.45, 0.06), "white": (0.75, 0.75, 0.75), "black": (0.015, 0.015, 0.016),
    "orange": (0.8, 0.18, 0.01), "purple": (0.25, 0.03, 0.5),
}
KNOB = {
    "red": (0.6, 0.03, 0.02), "blue": (0.04, 0.16, 0.65), "yellow": (0.8, 0.55, 0.05),
    "orange": (0.85, 0.22, 0.02), "silver": (0.7, 0.7, 0.72),
}
LED = {"green": (0.05, 0.9, 0.12), "red": (0.95, 0.04, 0.03), "blue": (0.08, 0.25, 1.0)}

# KHR_materials_emissive_strength, in the client's units (a brake light is
# 3000): the lenses glow dimly so they read as lights, not paint; the rev
# lights proper are the hub display's.
LED_STRENGTH = 60.0


def material(name):
    def pbr(col, metal=0.0, rough=0.5):
        return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*col, 1.0],
                                                        "metallicFactor": metal, "roughnessFactor": rough}}
    if name == "sw_carbon":
        m = pbr((1.0, 1.0, 1.0), 0.0, 0.32)
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        return m
    if name == "sw_carbon_back":
        return pbr((0.022, 0.022, 0.024), 0.0, 0.6)
    if name == "sw_grip":
        return pbr((0.028, 0.028, 0.03), 0.0, 0.95)
    if name == "sw_metal":
        return pbr((0.62, 0.62, 0.64), 1.0, 0.32)
    if name == "sw_metal_dark":
        return pbr((0.06, 0.06, 0.065), 1.0, 0.4)
    if name == "sw_screen":
        return pbr((0.004, 0.004, 0.005), 0.0, 0.06)
    if name == "sw_bezel":
        return pbr((0.012, 0.012, 0.013), 0.0, 0.55)
    if name == "sw_mark":
        return pbr((0.8, 0.8, 0.8), 0.0, 0.5)
    if name == "sw_marker":
        return pbr((0.85, 0.3, 0.02), 0.0, 0.85)
    kind, _, colour = name[3:].partition("_")
    if kind == "btn":
        return pbr(BUTTON[colour], 0.0, 0.42)
    if kind == "knob":
        return pbr(KNOB[colour], 0.9, 0.3)
    if kind == "led":
        m = pbr(tuple(c * 0.4 for c in LED[colour]), 0.0, 0.15)
        m["emissiveFactor"] = list(LED[colour])
        m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": LED_STRENGTH}}
        return m
    raise KeyError(name)


def png(rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def carbon_texture(size=256, tows=8):
    """A 2x2 twill: tows over two, under two, stepped a tow per row; each
    tow shaded across its width so the weave catches the light."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float64)
    t = size / tows
    cx, cy = np.floor(x / t), np.floor(y / t)
    warp = ((cx + cy) % 4) < 2
    fx, fy = (x % t) / t, (y % t) / t
    across = np.where(warp, fx, fy)
    along = np.where(warp, fy, fx)
    shade = 0.55 + 0.45 * np.sin(math.pi * across) ** 0.6
    shade *= 0.9 + 0.1 * np.cos(2 * math.pi * along)
    base = np.where(warp, 0.060, 0.038)
    v = base * shade
    srgb = np.where(v <= 0.0031308, 12.92 * v, 1.055 * np.power(v, 1 / 2.4) - 0.055)
    g = np.clip(np.round(srgb * 255), 0, 255).astype(np.uint8)
    rgb = np.stack([g, g, np.clip(g.astype(int) + 2, 0, 255).astype(np.uint8)], axis=2)
    return png(rgb)


# --------------------------------------------------------------------------
# GLB
# --------------------------------------------------------------------------

def to_gltf(P, N):
    """Driver's frame (cm) -> glTF (m): X = -u, Y = v, Z = -w."""
    P = np.asarray(P) * 0.01
    return (np.stack([-P[:, 0], P[:, 1], -P[:, 2]], axis=1),
            np.stack([-N[:, 0], N[:, 1], -N[:, 2]], axis=1))


def write_glb(mesh: Mesh, path):
    blob = bytearray()
    views, accessors, prims, mats = [], [], [], []

    def add_view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    def add_acc(arr, ctype, typ, target, minmax=False):
        view = add_view(arr.tobytes(), target)
        a = {"bufferView": view, "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a["min"] = [float(x) for x in arr.min(axis=0)]
            a["max"] = [float(x) for x in arr.max(axis=0)]
        accessors.append(a)
        return len(accessors) - 1

    names = sorted(mesh.parts)
    if "sw_carbon" in names:
        names.remove("sw_carbon")
        names.insert(0, "sw_carbon")
    for name in names:
        Ps, Ns, UVs, Is = [], [], [], []
        base = 0
        for (P, N, UV, I) in mesh.parts[name]:
            Ps.append(P); Ns.append(N); UVs.append(UV); Is.append(I + base)
            base += len(P)
        P, N = to_gltf(np.vstack(Ps), np.vstack(Ns))
        UV = np.vstack(UVs)
        I = np.vstack(Is)
        # A half turn keeps the winding; glTF wants CCW from outside, as built.
        pa = add_acc(P.astype(np.float32), 5126, "VEC3", 34962, minmax=True)
        na = add_acc(N.astype(np.float32), 5126, "VEC3", 34962)
        ta = add_acc(UV.astype(np.float32), 5126, "VEC2", 34962)
        ia = add_acc(I.astype(np.uint32).reshape(-1), 5125, "SCALAR", 34963)
        mats.append(material(name))
        prims.append({"attributes": {"POSITION": pa, "NORMAL": na, "TEXCOORD_0": ta}, "indices": ia,
                      "material": len(mats) - 1})
    img_view = add_view(carbon_texture())
    gltf = {
        "asset": {"version": "2.0", "generator": "ApexSim build_steering_wheels.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "steering_wheel", "mesh": 0}],
        "meshes": [{"name": "steering_wheel", "primitives": prims}],
        "materials": mats,
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": img_view, "mimeType": "image/png"}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    while len(blob) % 4:
        blob.append(0)
    out = (struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
           + struct.pack("<II", len(js), 0x4E4F534A) + js
           + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def build(name):
    mesh, (glass_front_w, screen_v, glass_w) = CLASSES[name]()
    os.makedirs(OUT_DIR, exist_ok=True)
    size = write_glb(mesh, os.path.join(OUT_DIR, f"{name}.glb"))
    # The rig's pivot: +X along the column toward the nose (= -w), +Y the
    # driver's right (= u), +Z up (= v).
    dash = {
        "_comment": "Written by scripts/content/wheels/build_steering_wheels.py: where the cockpit rig "
                    "puts its hub display on this wheel (wheel pivot frame, cm). Do not edit.",
        "dash_cm": [round(-(glass_front_w + DASH_PROUD_CM), 3), 0.0, round(screen_v, 3)],
        "dash_width_cm": round(glass_w, 3),
    }
    with open(os.path.join(OUT_DIR, f"{name}.json"), "w", newline="\n") as f:
        json.dump(dash, f, indent=2)
        f.write("\n")
    print(f"{name}: {mesh.triangles():,} triangles, {len(mesh.parts)} materials, {size / 1024:.0f} KB")
    return mesh


if __name__ == "__main__":
    wanted = sys.argv[1:] or list(CLASSES)
    for n in wanted:
        if n not in CLASSES:
            sys.exit(f"unknown class {n}; one of {', '.join(CLASSES)}")
        build(n)
