r"""Procedural PBR texture baking for the prop kit (numpy inside Blender).

Everything is tileable, generated from seeds, and saved beside the assets
under content/props/_textures/<slot>_{col,rough,nrm}.png (plus _mask for
masked card textures) so it can be inspected and regenerated. Textures are
authored at 1 tile = 1 m to match the kit's planar UVs (Builder.finish),
unless a material passes `tile_m`.

    apex_tex.pbr_material("barrier_concrete", apex_tex.concrete(seed=3))
    apex_tex.card_material("tree_card_broadleaf", apex_tex.leaf_cluster_card())
"""
import math, os
import numpy as np

# Everything from `blur` down to the generators is plain numpy, and the ground
# baker (scripts/bake_ground_textures.py) runs it outside Blender. Only the
# node-wiring half needs bpy, so a missing bpy is not an error until one of
# those functions is called.
try:
    import bpy
except ImportError:                                              # not in Blender
    bpy = None

TEX_DIR = r"D:\apexsim\content\props\_textures"
os.makedirs(TEX_DIR, exist_ok=True)
SIZE = 512


# ----------------------------------------------------------------- basics
def blur(a, sigma):
    if sigma <= 0.01:
        return a
    n = a.shape[0]
    f = np.fft.fftfreq(n)
    fx, fy = np.meshgrid(f, f)
    k = np.exp(-2.0 * (np.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * k))


def fbm(seed, scales=(2, 4, 8, 16, 32), weights=(0.45, 0.25, 0.16, 0.09, 0.05), size=SIZE):
    rng = np.random.default_rng(seed)
    t = np.zeros((size, size))
    for sc, w in zip(scales, weights):
        t += w * blur(rng.random((size, size)), size / (2.2 * sc))
    t -= t.min()
    if t.max() > 1e-9:
        t /= t.max()
    return t


def grid(size=SIZE):
    u = (np.arange(size) + 0.5) / size
    return np.meshgrid(u, u)   # X (cols), Y (rows), both 0..1


def normal_from_height(h, strength=2.0):
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5 * h.shape[0]
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5 * h.shape[0]
    nx, ny, nz = -dx * strength * 0.02, -dy * strength * 0.02, np.ones_like(h)
    L = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx / L * 0.5 + 0.5, ny / L * 0.5 + 0.5, nz / L * 0.5 + 0.5], -1)


def to_image(name, rgb, alpha=None, noncolor=False):
    size = rgb.shape[0]
    img = bpy.data.images.get(name)
    if img is None or img.size[0] != size:
        if img is not None:
            bpy.data.images.remove(img)
        img = bpy.data.images.new(name, width=size, height=size, alpha=alpha is not None)
    # colourspace first: changing it later reloads the (empty) buffer
    img.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
    pix = np.ones((size, size, 4), dtype=np.float32)
    pix[:, :, :3] = np.clip(rgb, 0, 1)
    if alpha is not None:
        pix[:, :, 3] = np.clip(alpha, 0, 1)
    img.pixels.foreach_set(pix.ravel())
    img.filepath_raw = os.path.join(TEX_DIR, name + ".png")
    img.file_format = 'PNG'
    img.save()
    img.pack()
    return img


def _wire(m, maps, tile_m=1.0, metallic=0.0, masked=False, emission=None):
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL'):
            nt.nodes.remove(n)
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = metallic
    coord = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.0 / tile_m, 1.0 / tile_m, 1.0)
    nt.links.new(coord.outputs["UV"], mp.inputs["Vector"])

    def tex(img):
        t = nt.nodes.new("ShaderNodeTexImage")
        t.image = img
        nt.links.new(mp.outputs["Vector"], t.inputs["Vector"])
        return t

    tc = tex(maps["col"])
    nt.links.new(tc.outputs["Color"], bsdf.inputs["Base Color"])
    if "rough" in maps:
        tr = tex(maps["rough"])
        nt.links.new(tr.outputs["Color"], bsdf.inputs["Roughness"])
    if "nrm" in maps:
        tn = tex(maps["nrm"])
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if masked:
        gt = nt.nodes.new("ShaderNodeMath")
        gt.operation = 'GREATER_THAN'
        gt.inputs[1].default_value = 0.5
        nt.links.new(tc.outputs["Alpha"], gt.inputs[0])
        nt.links.new(gt.outputs[0], bsdf.inputs["Alpha"])
        m.use_backface_culling = False
        if hasattr(m, "surface_render_method"):
            m.surface_render_method = 'DITHERED'
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 5.0
    return m


def pbr_material(name, gen, tile_m=1.0, metallic=0.0):
    """gen -> (color HxWx3, rough HxW, height HxW, normal_strength)."""
    color, rough, height, ns = gen
    maps = {"col": to_image(name + "_col", color),
            "rough": to_image(name + "_rough", np.stack([rough] * 3, -1), noncolor=True),
            "nrm": to_image(name + "_nrm", normal_from_height(height, ns), noncolor=True)}
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    return _wire(m, maps, tile_m=tile_m, metallic=metallic)


def card_material(name, gen):
    """gen -> (color HxWx3, alpha HxW). Masked, two-sided."""
    color, alpha = gen
    maps = {"col": to_image(name + "_col", color, alpha=alpha)}
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    _wire(m, maps, masked=True)
    m.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.8
    return m


# ------------------------------------------------------------- surfaces
def _col(rgb):
    return np.array(rgb).reshape(1, 1, 3)


def concrete(seed=1, rgb=(0.64, 0.63, 0.60), stains=0.25, form_lines=True):
    X, Y = grid()
    pores = fbm(seed, scales=(32, 64, 128), weights=(0.4, 0.35, 0.25))
    blotch = fbm(seed + 3, scales=(2, 4, 8), weights=(0.55, 0.3, 0.15))
    height = 0.5 + 0.18 * (pores - 0.5) - 0.12 * (pores < 0.12)      # pits
    if form_lines:
        seam = (np.abs(((Y * 2.0) % 1.0) - 0.5) < 0.006).astype(float)
        height -= 0.4 * seam
    shade = 0.85 + 0.3 * (blotch - 0.5)
    streak = fbm(seed + 9, scales=(1, 3), weights=(0.6, 0.4), size=SIZE)
    streak = np.clip((streak - 0.55) * 2.5, 0, 1) * stains
    color = _col(rgb) * shade[..., None] * (1 - 0.35 * streak[..., None]) * (0.95 + 0.1 * pores[..., None])
    rough = np.clip(0.78 + 0.18 * pores - 0.1 * blotch, 0.6, 0.98)
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 0.7


def galvanised(seed=2, rgb=(0.66, 0.68, 0.70), spangle=True):
    X, Y = grid()
    # spangle: cell-noise look from blurred thresholded noise
    cells = fbm(seed, scales=(20, 40), weights=(0.6, 0.4))
    edges = np.abs(cells - blur(cells, 3)) * 6
    spang = np.clip(edges, 0, 1) if spangle else np.zeros_like(cells)
    smudge = fbm(seed + 4, scales=(2, 5), weights=(0.6, 0.4))
    color = _col(rgb) * (0.85 + 0.25 * cells[..., None]) * (0.9 + 0.2 * smudge[..., None]) * (1 - 0.15 * spang[..., None])
    rough = np.clip(0.38 + 0.25 * smudge + 0.2 * spang, 0.3, 0.75)
    height = 0.5 + 0.1 * (cells - 0.5)
    return np.clip(color, 0, 1), rough, height, 0.6


def painted_steel(seed=5, rgb=(0.35, 0.36, 0.38), chips=0.08):
    smudge = fbm(seed, scales=(3, 8, 24), weights=(0.5, 0.3, 0.2))
    chip = (fbm(seed + 1, scales=(48, 96), weights=(0.5, 0.5)) > (1 - chips)).astype(float)
    color = _col(rgb) * (0.9 + 0.2 * smudge[..., None])
    color = color * (1 - chip[..., None]) + _col((0.5, 0.45, 0.4)) * chip[..., None]
    rough = np.clip(0.35 + 0.3 * smudge + 0.4 * chip, 0.25, 0.9)
    height = 0.5 - 0.3 * chip + 0.05 * (smudge - 0.5)
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.0


def rubber(seed=7, rgb=(0.07, 0.07, 0.07), dust=0.3, tread=True):
    """Tyre sidewall/tread: fine grain, dusty patches, ring grooves along U."""
    X, Y = grid()
    grain = fbm(seed, scales=(64, 128), weights=(0.5, 0.5))
    dusty = np.clip((fbm(seed + 2, scales=(2, 4, 8), weights=(0.5, 0.3, 0.2)) - 0.5) * 2.2, 0, 1) * dust
    height = 0.5 + 0.12 * (grain - 0.5)
    if tread:
        groove = (np.abs(((Y * 6.0) % 1.0) - 0.5) < 0.08).astype(float)
        height -= 0.35 * groove
    color = _col(rgb) * (0.8 + 0.5 * grain[..., None]) + _col((0.45, 0.42, 0.36)) * dusty[..., None]
    rough = np.clip(0.75 + 0.2 * grain - 0.2 * dusty, 0.5, 0.98)
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.2


def vinyl(seed=11, rgb=(0.75, 0.06, 0.05), scuff=0.35):
    X, Y = grid()
    weave = fbm(seed, scales=(96, 160), weights=(0.5, 0.5))
    scuffs = np.clip((fbm(seed + 3, scales=(2, 5, 12), weights=(0.45, 0.35, 0.2)) - 0.55) * 3, 0, 1) * scuff
    seam = ((np.abs(((X * 1.0) % 1.0) - 0.5) < 0.006) | (np.abs(((Y * 2.0) % 1.0) - 0.5) < 0.006)).astype(float)
    color = _col(rgb) * (0.92 + 0.16 * weave[..., None]) * (1 - 0.35 * scuffs[..., None]) * (1 - 0.3 * seam[..., None])
    rough = np.clip(0.4 + 0.15 * weave + 0.4 * scuffs, 0.3, 0.9)
    height = 0.5 + 0.04 * (weave - 0.5) - 0.2 * seam - 0.05 * scuffs
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 0.8


def plastic(seed=13, rgb=(0.05, 0.12, 0.40)):
    speck = fbm(seed, scales=(64, 128), weights=(0.5, 0.5))
    fade = fbm(seed + 1, scales=(2, 4), weights=(0.6, 0.4))
    color = _col(rgb) * (0.85 + 0.25 * fade[..., None]) * (0.95 + 0.1 * speck[..., None])
    rough = np.clip(0.42 + 0.2 * speck, 0.35, 0.7)
    height = 0.5 + 0.03 * (speck - 0.5)
    return np.clip(color, 0, 1), rough, height, 0.4


def corrugated(seed=17, rgb=(0.6, 0.62, 0.64), period=0.075):
    X, Y = grid()
    wave = 0.5 + 0.5 * np.sin(2 * np.pi * X / period)
    smudge = fbm(seed, scales=(2, 5, 12), weights=(0.5, 0.3, 0.2))
    rust = np.clip((fbm(seed + 1, scales=(3, 8), weights=(0.6, 0.4)) - 0.62) * 4, 0, 1)
    color = _col(rgb) * (0.85 + 0.25 * smudge[..., None]) * (0.9 + 0.2 * wave[..., None])
    color = color * (1 - rust[..., None] * 0.6) + _col((0.45, 0.22, 0.1)) * rust[..., None] * 0.6
    rough = np.clip(0.4 + 0.25 * smudge + 0.4 * rust, 0.3, 0.9)
    return np.clip(color, 0, 1), rough, wave, 2.5


def cladding(seed=19, rgb=(0.82, 0.82, 0.8), panel=(1.0, 0.5)):
    """Composite wall panels: subtle grain, panel seams every panel[0] x panel[1] m."""
    X, Y = grid()
    grain = fbm(seed, scales=(32, 96), weights=(0.6, 0.4))
    seam = ((np.abs(((X / panel[0]) % 1.0) - 0.5) < 0.004 / panel[0]) | (np.abs(((Y / panel[1]) % 1.0) - 0.5) < 0.004 / panel[1])).astype(float)
    grime = np.clip((fbm(seed + 2, scales=(1, 3), weights=(0.5, 0.5)) - 0.5) * 1.5, 0, 1)
    color = _col(rgb) * (0.94 + 0.1 * grain[..., None]) * (1 - 0.4 * seam[..., None]) * (1 - 0.12 * grime[..., None])
    rough = np.clip(0.45 + 0.15 * grain + 0.2 * grime, 0.35, 0.85)
    height = 0.5 - 0.3 * seam + 0.02 * (grain - 0.5)
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.2


def roller_door(seed=23, rgb=(0.7, 0.71, 0.72), slat=0.08):
    X, Y = grid()
    s = ((Y / slat) % 1.0)
    prof = np.where(s < 0.85, 0.5 + 0.2 * np.sin(np.pi * s / 0.85), 0.15)
    smudge = fbm(seed, scales=(2, 6), weights=(0.6, 0.4))
    color = _col(rgb) * (0.85 + 0.25 * smudge[..., None]) * (0.8 + 0.3 * prof[..., None])
    rough = np.clip(0.35 + 0.25 * smudge, 0.3, 0.7)
    return np.clip(color, 0, 1), rough, prof, 2.0


# ----------------------------------------------------------- card textures
def _ellipse(X, Y, cx, cy, rx, ry, ang):
    c, s = math.cos(ang), math.sin(ang)
    dx, dy = X - cx, Y - cy
    u = (dx * c + dy * s) / rx
    v = (-dx * s + dy * c) / ry
    return u * u + v * v <= 1.0


def _blade(X, Y, x0, y0, x1, y1, w0, w1):
    """Tapered line segment mask (width w0 at start to w1 at end)."""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy + 1e-9
    t = np.clip(((X - x0) * dx + (Y - y0) * dy) / L2, 0, 1)
    px, py = x0 + t * dx, y0 + t * dy
    d = np.hypot(X - px, Y - py)
    return d <= (w0 + (w1 - w0) * t) * 0.5


def leaf_cluster_card(seed=31, greens=((0.10, 0.30, 0.06), (0.16, 0.38, 0.09), (0.22, 0.42, 0.12)), n=42):
    """A clump of broadleaf leaves on twigs. Alpha = leaves, colour shaded
    by a light-from-top gradient plus per-leaf tint."""
    rng = np.random.default_rng(seed)
    X, Y = grid()
    alpha = np.zeros_like(X)
    color = np.zeros(X.shape + (3,))
    # twigs
    for k in range(5):
        a = rng.uniform(-0.6, 0.6) + (math.pi / 2)
        x1, y1 = 0.5 + 0.42 * math.cos(a), 0.15 + 0.7 * math.sin(a)
        m = _blade(X, Y, 0.5, 0.05, x1, y1, 0.012, 0.004)
        alpha[m] = 1
        color[m] = (0.25, 0.18, 0.1)
    for k in range(n):
        r = rng.uniform(0.15, 1.0) ** 0.6
        a = rng.uniform(0, 2 * math.pi)
        cx, cy = 0.5 + 0.42 * r * math.cos(a), 0.5 + 0.42 * r * math.sin(a) * 0.95
        rx, ry = rng.uniform(0.06, 0.1), rng.uniform(0.035, 0.06)
        ang = rng.uniform(0, math.pi)
        m = _ellipse(X, Y, cx, cy, rx, ry, ang)
        # leaf shape: pinch tip and add a midrib darkening
        g = np.array(greens[rng.integers(len(greens))]) * rng.uniform(0.8, 1.15)
        alpha[m] = 1
        light = 0.75 + 0.5 * (cy - 0.2)
        color[m] = np.clip(g * light, 0, 1)
        rib = _blade(X, Y, cx - rx * math.cos(ang), cy - rx * math.sin(ang), cx + rx * math.cos(ang), cy + rx * math.sin(ang), 0.006, 0.002)
        color[m & rib] *= 0.7
    # fringe darkening for depth
    return color, alpha


def spruce_frond_card(seed=37, greens=((0.06, 0.20, 0.08), (0.09, 0.26, 0.10), (0.12, 0.30, 0.12), (0.16, 0.34, 0.14))):
    """A spruce branch seen flat: stem up the middle, side twigs, needles
    drawn as short thick strokes until the frond reads solid from a metre
    away (about 70 % coverage inside the triangle)."""
    rng = np.random.default_rng(seed)
    X, Y = grid()
    alpha = np.zeros_like(X)
    color = np.zeros(X.shape + (3,))
    stem = _blade(X, Y, 0.5, 0.02, 0.5, 0.97, 0.03, 0.008)
    alpha[stem] = 1
    color[stem] = (0.28, 0.2, 0.12)
    for k in range(16):
        t = 0.06 + 0.86 * k / 16
        L = 0.46 * (1 - 0.75 * t) + 0.04
        for s in (-1, 1):
            x1, y1 = 0.5 + s * L, t + 0.05
            tw = _blade(X, Y, 0.5, t, x1, y1, 0.014, 0.004)
            alpha[tw] = 1
            color[tw] = (0.26, 0.19, 0.11)
            for j in range(12):
                u = (j + 0.5) / 12
                nx, ny = 0.5 + s * L * u, t + 0.05 * u
                g = np.array(greens[rng.integers(len(greens))])
                for d in (-1, 1):
                    ang = math.atan2(0.05, s * L) + d * rng.uniform(0.7, 1.1)
                    nl = rng.uniform(0.035, 0.06) * (1 - 0.3 * t)
                    nd = _blade(X, Y, nx, ny, nx + nl * math.cos(ang), ny + nl * math.sin(ang), 0.012, 0.004)
                    alpha[nd] = 1
                    color[nd] = np.clip(g * (0.75 + 0.5 * ny) * rng.uniform(0.85, 1.1), 0, 1)
    # soft fill behind the needles so the frond reads dense at distance
    fill = blur(alpha, 3.0) > 0.45
    color[fill & (alpha == 0)] = np.array(greens[0]) * 0.8
    alpha[fill] = 1
    return color, alpha


def grass_card(seed=41, flowers=0):
    rng = np.random.default_rng(seed)
    X, Y = grid()
    alpha = np.zeros_like(X)
    color = np.zeros(X.shape + (3,))
    greens = ((0.28, 0.45, 0.12), (0.36, 0.52, 0.16), (0.45, 0.55, 0.2), (0.55, 0.5, 0.22))
    for k in range(70):
        x0 = rng.uniform(0.1, 0.9)
        lean = rng.uniform(-0.3, 0.3)
        h = rng.uniform(0.35, 0.95)
        # blade as 3 segments bending in the lean direction
        px, py = x0, 0.0
        g = np.array(greens[rng.integers(len(greens))]) * rng.uniform(0.85, 1.1)
        for seg in range(3):
            nx, ny = px + lean * h / 3 * (seg + 1) * 0.5, py + h / 3
            m = _blade(X, Y, px, py, nx, ny, 0.02 * (1 - seg / 3), 0.02 * (1 - (seg + 1) / 3) + 0.002)
            alpha[m] = 1
            color[m] = np.clip(g * (0.7 + 0.5 * ny), 0, 1)
            px, py = nx, ny
    pal = ((0.95, 0.9, 0.2), (0.95, 0.95, 0.95), (0.8, 0.3, 0.55), (0.9, 0.35, 0.2), (0.5, 0.4, 0.85))
    for k in range(flowers):
        x0, y0 = rng.uniform(0.15, 0.85), rng.uniform(0.45, 0.85)
        st = _blade(X, Y, x0 + rng.uniform(-0.05, 0.05), 0.02, x0, y0, 0.01, 0.005)
        alpha[st] = 1
        color[st] = (0.3, 0.45, 0.15)
        c = np.array(pal[rng.integers(len(pal))])
        for p in range(5):
            a = 2 * math.pi * p / 5
            m = _ellipse(X, Y, x0 + 0.02 * math.cos(a), y0 + 0.02 * math.sin(a), 0.016, 0.011, a)
            alpha[m] = 1
            color[m] = c
        m = _ellipse(X, Y, x0, y0, 0.01, 0.01, 0)
        alpha[m] = 1
        color[m] = (0.95, 0.75, 0.1)
    return color, alpha


def wood(seed=43, rgb=(0.45, 0.30, 0.16), plank_m=0.15, weathered=0.3):
    """Planks along U: grain streaks, a seam every plank_m, grey weathering."""
    X, Y = grid()
    grain = fbm(seed, scales=(2, 6), weights=(0.5, 0.5))
    streak = 0.5 + 0.5 * np.sin(2 * np.pi * (Y * 40 + 6 * (grain - 0.5)))
    fine = fbm(seed + 1, scales=(48, 96), weights=(0.5, 0.5))
    seam = (np.abs(((Y / plank_m) % 1.0) - 0.5) < 0.006 / plank_m).astype(float)
    grey = np.clip((fbm(seed + 2, scales=(1, 3), weights=(0.6, 0.4)) - 0.45) * 2, 0, 1) * weathered
    color = _col(rgb) * (0.8 + 0.3 * streak[..., None]) * (0.9 + 0.2 * fine[..., None])
    color = color * (1 - grey[..., None]) + _col((0.5, 0.48, 0.44)) * grey[..., None] * (0.8 + 0.3 * streak[..., None])
    color = color * (1 - 0.45 * seam[..., None])
    rough = np.clip(0.6 + 0.2 * fine + 0.2 * grey, 0.5, 0.95)
    height = 0.5 + 0.08 * (streak - 0.5) - 0.3 * seam + 0.04 * (fine - 0.5)
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.2


def roof_tiles(seed=47, rgb=(0.55, 0.25, 0.15), tile_w=0.25, tile_h=0.35):
    """Overlapping clay tiles: courses along V (staggered), a rounded profile
    per tile, moss and dirt in the low-frequency blotch."""
    X, Y = grid()
    row = np.floor(Y / tile_h)
    xs = (X / tile_w + 0.5 * (row % 2)) % 1.0
    ys = (Y / tile_h) % 1.0
    prof = 0.5 + 0.35 * np.cos(np.pi * (xs - 0.5)) * (0.6 + 0.4 * ys) - 0.35 * (ys < 0.08)
    blotch = fbm(seed, scales=(2, 5, 12), weights=(0.5, 0.3, 0.2))
    moss = np.clip((fbm(seed + 1, scales=(3, 8), weights=(0.6, 0.4)) - 0.62) * 4, 0, 1)
    color = _col(rgb) * (0.75 + 0.45 * blotch[..., None]) * (0.7 + 0.4 * prof[..., None])
    color = color * (1 - 0.7 * moss[..., None]) + _col((0.3, 0.38, 0.15)) * moss[..., None] * 0.7
    rough = np.clip(0.7 + 0.2 * blotch, 0.55, 0.95)
    return np.clip(color, 0, 1), rough, np.clip(prof, 0, 1), 2.0


def plaster(seed=53, rgb=(0.92, 0.9, 0.84), stains=0.2):
    return concrete(seed, rgb=rgb, stains=stains, form_lines=False)


def _voronoi_edges(seed, cells, size=SIZE):
    """Tileable distance to the nearest Voronoi cell border (in texels
    of a unit tile), for `cells` random sites: small along the seams."""
    rng = np.random.default_rng(seed)
    sites = rng.random((cells, 2))
    X, Y = grid(size)
    d1 = np.full((size, size), 9.0)
    d2 = np.full((size, size), 9.0)
    for sx, sy in sites:
        dx = np.abs(X - sx); dx = np.minimum(dx, 1 - dx)
        dy = np.abs(Y - sy); dy = np.minimum(dy, 1 - dy)
        d = np.sqrt(dx * dx + dy * dy)
        d2 = np.where(d < d1, d1, np.minimum(d2, d))
        d1 = np.minimum(d1, d)
    return d2 - d1


def corten(seed=57, rgb=(0.36, 0.15, 0.07), cells=14):
    """Weathering steel welded from polygonal plates: a rust skin with
    orange blooms and dark streaks, and dark gaps along the plate seams,
    which is how the Spielberg bull's open lattice reads from the stands."""
    edge = _voronoi_edges(seed, cells)
    gap = np.clip(1.0 - edge / 0.012, 0, 1)                 # ~6 cm seams at a 2 m tile
    bloom = fbm(seed + 1, scales=(4, 12, 32), weights=(0.5, 0.3, 0.2))
    streak = fbm(seed + 2, scales=(2, 6), weights=(0.6, 0.4))
    pits = fbm(seed + 3, scales=(64, 128), weights=(0.5, 0.5))
    color = _col(rgb) * (0.8 + 0.45 * bloom[..., None]) * (0.92 + 0.12 * pits[..., None])
    color = color * (1 - 0.25 * np.clip((streak - 0.55) * 3, 0, 1)[..., None])
    color = color * (1 - gap[..., None]) + _col((0.05, 0.03, 0.02)) * gap[..., None]
    rough = np.clip(0.72 + 0.2 * pits - 0.1 * bloom + 0.1 * gap, 0.55, 0.98)
    height = 0.55 + 0.08 * (pits - 0.5) - 0.5 * gap
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.6


def cast_alu(seed=59, rgb=(0.56, 0.56, 0.55)):
    """Lost-foam cast aluminium segments: grey, a seam every half tile,
    and copper-brown tarnish rising from the feet (the arch at Spielberg)."""
    X, Y = grid()
    grain = fbm(seed, scales=(24, 64, 128), weights=(0.4, 0.35, 0.25))
    seam = (np.abs(((Y * 2.0) % 1.0) - 0.5) < 0.005).astype(float)
    tarnish = np.clip((fbm(seed + 1, scales=(2, 5, 12), weights=(0.5, 0.3, 0.2)) - 0.45) * 2.2, 0, 1)
    color = _col(rgb) * (0.9 + 0.18 * grain[..., None])
    color = color * (1 - 0.6 * tarnish[..., None]) + _col((0.45, 0.28, 0.17)) * 0.6 * tarnish[..., None]
    color = color * (1 - 0.35 * seam[..., None])
    rough = np.clip(0.4 + 0.2 * grain + 0.15 * tarnish, 0.3, 0.8)
    height = 0.5 + 0.05 * (grain - 0.5) - 0.3 * seam
    return np.clip(color, 0, 1), rough, np.clip(height, 0, 1), 1.0


# ----------------------------------------------------- the kit's baked slots
# slot -> (generator, tile_m, metallic). Shared by retexture_kit.py and the
# batch builders (via kit_material), so a slot looks the same on every asset.
KIT_SLOTS = {
    "armco_galv": (lambda: galvanised(seed=2), 1.0, 0.85),
    "armco_post": (lambda: galvanised(seed=4, rgb=(0.55, 0.57, 0.58), spangle=False), 0.5, 0.8),
    "gate_steel": (lambda: galvanised(seed=6, rgb=(0.5, 0.52, 0.5), spangle=False), 0.5, 0.7),
    "tire_rubber": (lambda: rubber(seed=7, dust=0.25), 0.5, 0.0),
    "tire_rubber_worn": (lambda: rubber(seed=8, rgb=(0.1, 0.1, 0.1), dust=0.5), 0.5, 0.0),
    "tire_belt": (lambda: rubber(seed=9, rgb=(0.05, 0.05, 0.06), dust=0.15, tread=False), 1.0, 0.0),
    "vehicle_tyre": (lambda: rubber(seed=10, rgb=(0.05, 0.05, 0.05), dust=0.1), 0.5, 0.0),
    "tecpro_red": (lambda: vinyl(seed=11, rgb=(0.75, 0.06, 0.05)), 1.0, 0.0),
    "tecpro_white": (lambda: vinyl(seed=12, rgb=(0.9, 0.9, 0.88)), 1.0, 0.0),
    "tecpro_base": (lambda: rubber(seed=13, rgb=(0.08, 0.08, 0.08), dust=0.3, tread=False), 1.0, 0.0),
    "kerb_yellow": (lambda: concrete(seed=14, rgb=(0.95, 0.78, 0.05), stains=0.3, form_lines=False), 1.0, 0.0),
    "barrier_concrete": (lambda: concrete(seed=1), 2.0, 0.0),
    "pit_concrete": (lambda: concrete(seed=5, rgb=(0.6, 0.6, 0.58), stains=0.15), 2.0, 0.0),
    "pit_floor": (lambda: concrete(seed=6, rgb=(0.55, 0.55, 0.54), form_lines=False), 2.0, 0.0),
    "pit_wall": (lambda: cladding(seed=19), 1.0, 0.1),
    "pit_wall_dark": (lambda: cladding(seed=20, rgb=(0.28, 0.29, 0.31)), 1.0, 0.2),
    "pit_frame": (lambda: painted_steel(seed=21, rgb=(0.32, 0.33, 0.35), chips=0.03), 1.0, 0.5),
    "pit_door": (lambda: roller_door(seed=23), 1.0, 0.6),
    "grandstand_concrete": (lambda: concrete(seed=31, rgb=(0.6, 0.6, 0.6)), 2.0, 0.0),
    "grandstand_concrete_dark": (lambda: concrete(seed=32, rgb=(0.42, 0.43, 0.44), stains=0.35), 2.0, 0.0),
    "grandstand_seat_a": (lambda: plastic(seed=33, rgb=(0.05, 0.12, 0.40)), 0.5, 0.0),
    "grandstand_seat_b": (lambda: plastic(seed=34, rgb=(0.75, 0.12, 0.08)), 0.5, 0.0),
    "grandstand_rail": (lambda: galvanised(seed=35, rgb=(0.6, 0.62, 0.64), spangle=False), 0.5, 0.8),
    "grandstand_steel": (lambda: painted_steel(seed=36, rgb=(0.25, 0.26, 0.28), chips=0.05), 1.0, 0.5),
    "grandstand_roof": (lambda: corrugated(seed=37), 1.0, 0.6),
    "bridge_steel": (lambda: painted_steel(seed=38, rgb=(0.5, 0.51, 0.53), chips=0.02), 1.0, 0.7),
    "bridge_dark": (lambda: painted_steel(seed=39, rgb=(0.12, 0.12, 0.13), chips=0.02), 1.0, 0.4),
    "board_post": (lambda: galvanised(seed=40, rgb=(0.4, 0.41, 0.43), spangle=False), 0.5, 0.7),
    "board_back": (lambda: painted_steel(seed=41, rgb=(0.25, 0.26, 0.27), chips=0.04), 1.0, 0.4),
    "scaffold_plank": (lambda: wood(seed=43, rgb=(0.55, 0.42, 0.25), plank_m=0.2, weathered=0.4), 1.0, 0.0),
    "house_timber": (lambda: wood(seed=44, rgb=(0.42, 0.28, 0.15), plank_m=0.15, weathered=0.25), 1.0, 0.0),
    "house_timber_dark": (lambda: wood(seed=45, rgb=(0.25, 0.17, 0.10), plank_m=0.12, weathered=0.2), 1.0, 0.0),
    "house_render": (lambda: plaster(seed=46, rgb=(0.92, 0.9, 0.84)), 2.0, 0.0),
    "house_render_b": (lambda: plaster(seed=48, rgb=(0.85, 0.8, 0.62), stains=0.3), 2.0, 0.0),
    "house_roof_tile": (lambda: roof_tiles(seed=47), 1.0, 0.0),
    "house_roof_dark": (lambda: roof_tiles(seed=49, rgb=(0.25, 0.24, 0.23)), 1.0, 0.0),
    "house_stone": (lambda: concrete(seed=50, rgb=(0.62, 0.6, 0.55), stains=0.3), 1.0, 0.0),
    "statue_plinth": (lambda: concrete(seed=51, rgb=(0.5, 0.5, 0.5), stains=0.2), 1.0, 0.0),
    "statue_corten": (lambda: corten(seed=57), 2.0, 0.3),
    "statue_arch": (lambda: cast_alu(seed=59), 2.0, 0.8),
    "tent_white": (lambda: cladding(seed=52, rgb=(0.92, 0.92, 0.9), panel=(2.0, 1.0)), 1.0, 0.0),
    "letters_white": (lambda: cladding(seed=54, rgb=(0.95, 0.95, 0.93), panel=(3.0, 3.0)), 1.0, 0.0),
    "forest_floor": (lambda: concrete(seed=55, rgb=(0.12, 0.14, 0.07), stains=0.4, form_lines=False), 4.0, 0.0),
}
_kit_cache = {}


def kit_material(name, rgb=None, **flat):
    """The baked material for a kit slot, or a flat Principled one (via
    apex_props.material) when the slot has no bake. Cached per session."""
    if name in KIT_SLOTS:
        if name not in _kit_cache:
            old = bpy.data.materials.get(name)
            if old is not None and old.node_tree and not any(n.type == 'TEX_IMAGE' for n in old.node_tree.nodes):
                old.name = name + "_flat"
            gen, tile, met = KIT_SLOTS[name]
            _kit_cache[name] = pbr_material(name, gen(), tile_m=tile, metallic=met)
        return _kit_cache[name]
    import sys
    return sys.modules["apex"].material(name, rgb, **flat)


def reset_cache():
    _kit_cache.clear()


# ------------------------------------------------------- the ground surfaces
# The circuit's own surfaces — the thing a driver looks at for an hour — baked
# bigger than the kit's slots and tileable over GROUND_TILE_M of world.
# scripts/bake_ground_textures.py writes them out; the `ApexGroundTexImport`
# commandlet brings them in; M_ApexTrackBase samples them.
#
# Colour maps are normalised to a per-channel mean of 0.5 on purpose. The
# track material multiplies them by the exporter's own per-key colour, which
# is what still tells a red kerb from a yellow one, Monza's asphalt from its
# pit lane and a gravel trap from a sand one. A map carrying its own hue
# would tint twice and every surface would come out over-saturated; relative
# hue *variation* survives the normalisation, which is where the straw
# patches in the grass and the blue-grey aggregate in the asphalt live.
#
# GROUND_TILE_M is mirrored by ApexGround::TextureTileM on the Unreal side.
# The two have to agree or every surface is tiled at the wrong size.
GROUND_SIZE = 1024
GROUND_TILE_M = 2.0


def _gn(seed, scales, weights):
    return fbm(seed, scales=scales, weights=weights, size=GROUND_SIZE)


def _ground_grid():
    return grid(GROUND_SIZE)


def _mean_half(color):
    """Each channel to a mean of 0.5. Clipping moves the mean back, so the
    scaling is iterated to a fixed point rather than applied once — three
    passes is well inside a tenth of a percent for everything here."""
    out = np.clip(color, 0, 1)
    for _ in range(3):
        m = out.reshape(-1, 3).mean(axis=0)
        out = np.clip(out * (0.5 / np.maximum(m, 1e-6)), 0, 1)
    return out


def _stone_field(count, r_min, r_max, seed):
    """Rounded stones stamped into a height field with wrapping, so the bed
    tiles. Returns the domed tops and a tone per stone. Each stone is drawn
    into its own window: a whole-image pass per stone would be a thousand
    megapixel operations for one gravel trap."""
    size = GROUND_SIZE
    rng = np.random.default_rng(seed)
    height = np.zeros((size, size))
    tone = np.full((size, size), 0.5)
    for _ in range(count):
        r = rng.uniform(r_min, r_max) * size
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        shade = rng.uniform(0.5, 1.0)
        rad = int(math.ceil(r)) + 1
        ix, iy = int(cx), int(cy)
        xs = (np.arange(-rad, rad + 1) + ix) % size
        ys = (np.arange(-rad, rad + 1) + iy) % size
        dx = (np.arange(-rad, rad + 1) + ix - cx)[None, :]
        dy = (np.arange(-rad, rad + 1) + iy - cy)[:, None]
        d2 = (dx * dx + dy * dy) / (r * r)
        dome = np.where(d2 < 1.0, np.sqrt(np.maximum(1.0 - d2, 0.0)), 0.0)
        win = np.ix_(ys, xs)
        above = dome > height[win]
        height[win] = np.where(above, dome, height[win])
        tone[win] = np.where(above, shade, tone[win])
    return height, tone


def asphalt(seed=61, coarse=1.0, polish=0.3):
    """Bitmacadam from a metre up: aggregate showing through the binder, a
    few darker repair patches, and the polished sheen a racing line leaves.
    The pale specks are the stone, not dirt — asphalt that reads as one flat
    grey is the single biggest reason a circuit looks painted on."""
    stone = _gn(seed, (72, 144, 288), (0.45, 0.35, 0.20))
    chip = _gn(seed + 1, (192, 384), (0.55, 0.45))
    patch = _gn(seed + 2, (2, 5, 11), (0.5, 0.3, 0.2))
    pit = (chip > 0.80).astype(float)
    height = 0.5 + 0.20 * coarse * (stone - 0.5) - 0.18 * coarse * pit
    shade = 0.80 + 0.45 * stone - 0.25 * patch
    color = np.stack(
        [shade * (0.99 + 0.04 * chip), shade, shade * (1.01 + 0.05 * (1.0 - stone))], -1)
    sheen = np.clip((patch - 0.55) * 2.4, 0, 1) * polish
    rough = np.clip(0.92 - 0.30 * sheen + 0.10 * (stone - 0.5), 0.45, 0.99)
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.4


def turf(seed=67, dry=0.3):
    """Mown grass from above: blades too small to model, clumps that are
    not, and the straw patches every verge has by race weekend."""
    blade = _gn(seed, (220, 440), (0.55, 0.45))
    clump = _gn(seed + 1, (6, 14, 30), (0.5, 0.3, 0.2))
    straw = np.clip((_gn(seed + 2, (3, 7), (0.6, 0.4)) - 0.58) * 3.0, 0, 1) * dry
    lit = 0.70 + 0.55 * blade + 0.35 * (clump - 0.5)
    green = np.stack([lit * 0.75, lit, lit * 0.55], -1)
    hay = np.stack([lit * 1.15, lit * 1.05, lit * 0.60], -1)
    color = green * (1 - straw[..., None]) + hay * straw[..., None]
    rough = np.clip(0.93 + 0.06 * (blade - 0.5) - 0.05 * straw, 0.75, 0.99)
    height = 0.5 + 0.30 * (blade - 0.5) + 0.12 * (clump - 0.5)
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.8


def gravel_bed(seed=71, count=1600):
    """A deceleration trap: rounded pea shingle over its own fines."""
    dome, tone = _stone_field(count, 0.008, 0.020, seed)
    fines = _gn(seed + 1, (160, 320), (0.5, 0.5))
    dust = _gn(seed + 2, (3, 8), (0.6, 0.4))
    height = 0.35 * fines + 0.65 * dome
    lit = 0.55 + 0.75 * dome * tone + 0.25 * fines
    color = np.stack([lit * 1.08, lit, lit * 0.82], -1) * (0.9 + 0.2 * dust[..., None])
    rough = np.clip(0.88 + 0.08 * fines - 0.05 * dome, 0.70, 0.99)
    return _mean_half(color), rough, np.clip(height, 0, 1), 2.2


def sand_trap(seed=73):
    """Raked sand: ripples at a third of a metre, grain under them, and the
    scuffs where a car has been dug out."""
    _, Y = _ground_grid()
    warp = _gn(seed, (2, 5), (0.6, 0.4))
    ripple = 0.5 + 0.5 * np.sin(2 * np.pi * (Y * 6.0 + 1.4 * (warp - 0.5)))
    grain = _gn(seed + 1, (300, 600), (0.5, 0.5))
    dug = np.clip((_gn(seed + 2, (4, 9), (0.6, 0.4)) - 0.60) * 3.0, 0, 1)
    height = 0.5 + 0.16 * (ripple - 0.5) + 0.10 * (grain - 0.5) - 0.18 * dug
    lit = 0.78 + 0.30 * ripple + 0.20 * grain - 0.15 * dug
    color = np.stack([lit * 1.10, lit, lit * 0.78], -1)
    rough = np.clip(0.95 - 0.05 * ripple, 0.80, 0.99)
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.6


def concrete_slab(seed=89):
    """An apron: float-finished slabs with a saw-cut joint at each tile edge,
    which puts the joints on a 2 m grid — what a real apron has, so here the
    repeat is a feature rather than the usual giveaway."""
    X, Y = _ground_grid()
    pores = _gn(seed, (40, 110, 260), (0.4, 0.35, 0.25))
    blotch = _gn(seed + 1, (2, 4, 9), (0.55, 0.30, 0.15))
    stain = np.clip((_gn(seed + 2, (1, 3), (0.6, 0.4)) - 0.55) * 2.5, 0, 1)
    joint = ((np.minimum(X, 1.0 - X) < 0.004) | (np.minimum(Y, 1.0 - Y) < 0.004)).astype(float)
    height = 0.5 + 0.14 * (pores - 0.5) - 0.12 * (pores < 0.12) - 0.45 * joint
    lit = 0.88 + 0.22 * (blotch - 0.5) + 0.10 * pores - 0.30 * stain - 0.30 * joint
    color = np.stack([lit, lit * 0.995, lit * 0.97], -1)
    rough = np.clip(0.78 + 0.18 * pores - 0.10 * blotch, 0.60, 0.98)
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.0


def astroturf(seed=79):
    """The synthetic strip beside a kerb: fibres all one length, tufted in
    rows about three centimetres apart, backing showing where it is worn."""
    X, _ = _ground_grid()
    strands = _gn(seed, (260, 520), (0.5, 0.5))
    rows = 0.5 + 0.5 * np.sin(2 * np.pi * X * 64.0)
    wear = np.clip((_gn(seed + 1, (3, 8), (0.55, 0.45)) - 0.60) * 3.0, 0, 1)
    lit = 0.72 + 0.40 * strands + 0.12 * rows - 0.25 * wear
    color = np.stack([lit * 0.68, lit, lit * 0.52], -1)
    rough = np.clip(0.80 + 0.12 * strands + 0.10 * wear, 0.65, 0.97)
    height = 0.5 + 0.22 * (strands - 0.5) + 0.06 * (rows - 0.5)
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.2


def kerb_paint(seed=83):
    """Painted kerb concrete. The paint itself is flat, so what shows is the
    trowel grain under it, the chips a hundred cars put in the nose and the
    rubber ground into it — the red/white alternation is the material's
    stripe, not the texture's, so this map is colourless by design."""
    grain = _gn(seed, (48, 128, 256), (0.40, 0.35, 0.25))
    chip = (_gn(seed + 1, (96, 220), (0.5, 0.5)) > 0.83).astype(float)
    scuff = np.clip((_gn(seed + 2, (2, 6), (0.6, 0.4)) - 0.50) * 2.2, 0, 1)
    lit = 0.92 + 0.16 * (grain - 0.5) - 0.35 * chip - 0.22 * scuff
    color = np.stack([lit, lit * (1 - 0.03 * scuff), lit * (1 - 0.05 * scuff)], -1)
    rough = np.clip(0.42 + 0.35 * chip + 0.25 * scuff + 0.10 * grain, 0.30, 0.95)
    height = 0.5 + 0.06 * (grain - 0.5) - 0.25 * chip
    return _mean_half(color), rough, np.clip(height, 0, 1), 1.0


# Set name -> generator. The names are the file stems under
# content/textures/ground and the `T_ground_<set>_*` assets they import as;
# ApexGround::LookFor picks one of them per material family.
GROUND_SLOTS = {
    "asphalt": asphalt,
    "grass": turf,
    "gravel": gravel_bed,
    "sand": sand_trap,
    "concrete": concrete_slab,
    "astroturf": astroturf,
    "kerb": kerb_paint,
}
