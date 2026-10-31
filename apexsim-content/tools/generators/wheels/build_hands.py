"""Build the driver's gloves and sleeves the cockpit rig draws on the wheel.

    python tools/generators/wheels/build_hands.py          # every class, and the sleeves
    python tools/generators/wheels/build_hands.py gt3 f1   # a few classes

Plain Python and numpy like build_steering_wheels.py, whose mesh, frame and
GLB writer it shares, and as deterministic.

Per wheel class, `wheels/steering/<class>_hands.glb`: both gloves closed round
that wheel's grips at 9 and 3 o'clock, in the wheel GLB's own frame, so the rig
hangs it on the wheel and it turns with the rim. Each glove ends in a gauntlet
cuff running back toward the driver's elbow; the cuff's open end is the wrist
the rig's arms start from, written into the class's `<class>.json` as
`wrist_cm` (the right hand, wheel pivot frame, cm; the left mirrors it).

Class-free, `wheels/hands/forearm.glb` and `wheels/hands/upper_arm.glb`: a
race-suit sleeve each, 1 m along glTF +X (the rig stretches it from joint to
joint), tapering toward the hand.

Materials (slot names): `glove_leather` (the back of the hand), `glove_palm`
(suede, inside the grip), `glove_cuff` (the gauntlet's band), `suit` (the
sleeves; the rig tints it with the car's paint) and `suit_cuff` (the knit
band at the wrist). Brand-free.
"""
from __future__ import annotations

import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_steering_wheels as sw  # noqa: E402

HANDS_DIR = os.path.join(sw.ROOT, "wheels", "hands")

# Each class wheel's right grip as build_steering_wheels.py lays it: its
# centre (u right, v up, w toward the driver, cm) at 3 o'clock, and the grip
# section's half widths across the rim (u) and along the column (w).
GRIPS = {
    "f1": ((12.9, 0.0, -0.55), 1.96, 2.40),
    "lmp2": ((13.3, 0.0, -0.5), 1.98, 2.42),
    "hypercar": ((13.7, 0.0, -0.5), 1.98, 2.42),
    "gt3": ((16.4, 0.0, -0.6), 1.80, 2.37),
    "rally": ((17.5, 0.0, 4.2), 1.45, 1.75),
}

HAND_HALF_V = 4.4          # the hand across the four fingers, half, cm
PHI_FINGER_TIP = -212.0    # the fingers wrap from the outside round the nose side
PHI_KNUCKLE = -12.0
PHI_HEEL = 118.0           # the heel of the hand on the driver's side
CUFF_CM = 9.0              # the gauntlet's length back toward the elbow


def _surface(m: sw.Mesh, mat, P, N, closed_u=False):
    """A (rows x cols) grid of points and normals as triangles."""
    rows, cols = P.shape[:2]
    I = []
    for i in range(rows - 1):
        for j in range(cols - (0 if closed_u else 1)):
            j1 = (j + 1) % cols
            a, b, c, d = i * cols + j, i * cols + j1, (i + 1) * cols + j1, (i + 1) * cols + j
            I += [(a, b, c), (a, c, d)]
    uv = np.stack(np.meshgrid(np.linspace(0, 1, cols), np.linspace(0, 1, rows)), axis=-1).reshape(-1, 2) * 2.0
    m.add(mat, P.reshape(-1, 3), N.reshape(-1, 3), I, uv)


def glove(m: sw.Mesh, grip, rx, ry, side):
    """One glove closed round a vertical grip at `grip`; `side` +1 is the right
    hand. Returns the wrist (the cuff's open end), driver's frame, cm."""
    g = np.array(grip, dtype=np.float64) * np.array([side, 1.0, 1.0])
    ex = np.array([side, 0.0, 0.0])       # outward
    ez = np.array([0.0, 0.0, 1.0])        # toward the driver
    ev = np.array([0.0, 1.0, 0.0])

    phis = np.radians(np.linspace(PHI_FINGER_TIP, PHI_HEEL, 64))
    vs = np.linspace(-HAND_HALF_V, HAND_HALF_V, 17)
    deg = np.degrees(phis)

    # Shell thickness round the grip: thin fingers, a knuckle ridge, a fuller
    # back of the hand; four finger ridges across the band where it wraps.
    finger = deg < PHI_KNUCKLE
    thick = np.where(finger, 1.55, 2.25 + 0.35 * np.sin(np.radians(np.clip(deg, 0, 110)) * 1.6))
    thick += 0.45 * np.exp(-((deg - PHI_KNUCKLE) / 14.0) ** 2)
    ridge = 0.32 * np.cos(np.pi * 4 * (vs / (2 * HAND_HALF_V) + 0.5) - np.pi) ** 2
    # The little finger shorter, the band rounding off at the hand's edges.
    edge = 1.0 - 0.55 * (np.abs(vs) / HAND_HALF_V) ** 6

    def ring(r_off, v_scale):
        cu = np.cos(phis)[:, None, None]
        cw = np.sin(phis)[:, None, None]
        rr = r_off[:, :, None]
        P = (g[None, None, :] + (rx + rr) * cu * ex + (ry + rr) * cw * ez
             + (vs[None, :, None] * v_scale) * ev)
        return P

    out_off = (thick[:, None] + np.where(finger[:, None], ridge[None, :], 0.0)) * edge[None, :] + 0.12
    in_off = np.full_like(out_off, 0.12)
    Po, Pi = ring(out_off, 1.0), ring(in_off, 0.96)
    radial = np.cos(phis)[:, None, None] * ex + np.sin(phis)[:, None, None] * ez
    No = np.repeat(radial, len(vs), axis=1)
    _surface(m, "glove_leather", Po, No)
    _surface(m, "glove_palm", Pi, -No)
    # The band's two edges (top and bottom of the fist) and its ends.
    for jj, sgn in ((0, -1.0), (len(vs) - 1, 1.0)):
        P = np.stack([Pi[:, jj], Po[:, jj]], axis=1)
        _surface(m, "glove_leather", P, np.repeat((ev * sgn)[None, None, :], P.shape[0], 0).repeat(2, 1))
    for ii, sgn in ((0, -1.0), (len(phis) - 1, 1.0)):
        tangent = (-np.sin(phis[ii]) * ex + np.cos(phis[ii]) * ez) * sgn
        P = np.stack([Pi[ii], Po[ii]], axis=0)
        _surface(m, "glove_leather", P, np.repeat(np.repeat(tangent[None, None, :], 2, 0), len(vs), 1))

    # The thumb: from the heel of the hand across the driver's face of the
    # grip toward the spoke, above the fingers.
    def at(phi_deg, v, r):
        p = math.radians(phi_deg)
        return g + (rx + r) * math.cos(p) * ex + (ry + r) * math.sin(p) * ez + v * ev
    thumb = np.array([at(a, v, r) for a, v, r in ((95, 0.9, 2.0), (112, 1.3, 1.6), (130, 1.6, 1.25),
                                                   (148, 1.7, 1.0), (165, 1.6, 0.9), (178, 1.4, 0.85))])
    n = len(thumb)
    taper = np.linspace(1.15, 0.8, n)
    m.sweep("glove_leather", thumb, sw.ellipse(1.05, 0.95, 14), up=(0, 1, 0), scales=np.stack([taper, taper], 1))

    # The gauntlet: from the back of the hand toward the elbow, out, down and
    # back to the driver.
    back = at(52, -0.6, 1.2)
    d = np.array([side * 0.32, -0.42, 1.0])
    d /= np.linalg.norm(d)
    wrist = back + CUFF_CM * d
    cuff = np.array([back + t * CUFF_CM * d for t in np.linspace(0, 1, 8)])
    grow = np.linspace(1.0, 1.18, len(cuff))
    m.sweep("glove_leather", cuff[:6], sw.ellipse(3.0, 2.5, 20), up=(0, 1, 0), scales=np.stack([grow[:6], grow[:6]], 1))
    m.sweep("glove_cuff", cuff[5:], sw.ellipse(3.0, 2.5, 20), up=(0, 1, 0),
            scales=np.stack([grow[5:] * 1.02, grow[5:] * 1.02], 1))
    return wrist


def material(name):
    def pbr(col, rough, metal=0.0):
        return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*col, 1.0],
                                                        "metallicFactor": metal, "roughnessFactor": rough}}
    if name == "glove_leather":
        m = pbr((1.0, 1.0, 1.0), 0.62)
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        return m
    if name == "glove_palm":
        return pbr((0.16, 0.16, 0.17), 0.95)
    if name == "glove_cuff":
        return pbr((0.22, 0.22, 0.24), 0.8)
    if name == "suit":
        m = pbr((1.0, 1.0, 1.0), 0.85)
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        return m
    if name == "suit_cuff":
        return pbr((0.03, 0.03, 0.035), 0.95)
    raise KeyError(name)


def grain_texture(size=256, base=0.040, seed=7):
    """Leather or fabric grain: soft value noise round a base grey (linear),
    as an sRGB PNG; the rig's tint multiplies it."""
    rng = np.random.default_rng(seed)
    acc = np.zeros((size, size))
    for octave, amp in ((8, 0.5), (32, 0.3), (128, 0.2)):
        cells = rng.random((octave, octave))
        y, x = np.mgrid[0:size, 0:size] * (octave / size)
        x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - x0, y - y0
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        c = lambda a, b: cells[b % octave, a % octave]  # noqa: E731
        acc += amp * ((c(x0, y0) * (1 - fx) + c(x0 + 1, y0) * fx) * (1 - fy)
                      + (c(x0, y0 + 1) * (1 - fx) + c(x0 + 1, y0 + 1) * fx) * fy)
    v = base * (0.8 + 0.4 * acc)
    srgb = np.where(v <= 0.0031308, 12.92 * v, 1.055 * np.power(v, 1 / 2.4) - 0.055)
    g = np.clip(np.round(srgb * 255), 0, 255).astype(np.uint8)
    return sw.png(np.stack([g, g, g], axis=2))


def build_class(name):
    grip, rx, ry = GRIPS[name]
    m = sw.Mesh()
    wrist = None
    for side in (1, -1):
        w = glove(m, grip, rx, ry, side)
        wrist = w if side == 1 else wrist
    os.makedirs(sw.OUT_DIR, exist_ok=True)
    path = os.path.join(sw.OUT_DIR, f"{name}_hands.glb")
    size = sw.write_glb(m, path, material_of=material, images=[grain_texture(base=0.035)],
                        first="glove_leather", node="hands")
    # The wrist in the rig's wheel pivot frame: +X toward the nose (= -w),
    # +Y the driver's right (= u), +Z up (= v).
    meta_path = os.path.join(sw.OUT_DIR, f"{name}.json")
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    meta["wrist_cm"] = [round(-wrist[2], 3), round(wrist[0], 3), round(wrist[1], 3)]
    with open(meta_path, "w", newline="\n") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(f"{name}_hands: {m.triangles():,} triangles, {size / 1024:.0f} KB, wrist {meta['wrist_cm']}")


def sleeve(name, r0, r1, cuff_cm):
    """A race-suit sleeve 1 m along +X (driver's frame u, so glTF -X: built
    reversed), radius r0 at the hand end tapering out to r1, a knit band of
    `cuff_cm` at the hand end."""
    m = sw.Mesh()
    xs = np.linspace(0.0, 100.0, 24)
    r = r0 + (r1 - r0) * (xs / 100.0) ** 0.7
    path = np.stack([-xs, np.zeros_like(xs), np.zeros_like(xs)], axis=1)   # glTF +X after to_gltf
    sec = sw.ellipse(1.0, 0.92, 22)
    body = xs >= cuff_cm
    m.sweep("suit", path[body], sec, up=(0, 1, 0), scales=np.stack([r[body], r[body]], 1))
    if cuff_cm > 0:
        k = xs <= cuff_cm + 1e-6
        k[np.argmax(~k)] = True
        m.sweep("suit_cuff", path[k], sec, up=(0, 1, 0), scales=np.stack([r[k] * 1.03, r[k] * 1.03], 1))
    os.makedirs(HANDS_DIR, exist_ok=True)
    size = sw.write_glb(m, os.path.join(HANDS_DIR, f"{name}.glb"), material_of=material,
                        images=[grain_texture(base=0.5, seed=11)], first="suit", node=name)
    print(f"{name}: {m.triangles():,} triangles, {size / 1024:.0f} KB")


if __name__ == "__main__":
    wanted = sys.argv[1:] or list(GRIPS)
    for n in wanted:
        if n not in GRIPS:
            sys.exit(f"unknown class {n}; one of {', '.join(GRIPS)}")
        build_class(n)
    if not sys.argv[1:]:
        sleeve("forearm", 3.6, 4.9, 4.0)
        sleeve("upper_arm", 4.9, 6.0, 0.0)
