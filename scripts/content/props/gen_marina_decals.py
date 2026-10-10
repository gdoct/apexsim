r"""Marina Bay start/finish decals + garage number plates (docs/content/circuits.md).

Runs inside Blender (fonts are rasterised from Blender text meshes, no PIL):
    exec(open(r"E:\apexsim\scripts\content\props\gen_marina_decals.py").read())

Writes content/props/decal/marina/*.png (decal convention as the graffiti set:
image width = across the road, height = along it, top = far end, left = left
of the road; RGBA, paint where alpha > 0) and content/props/board/markers/1..24.png
(garage_number_board plates, 1536 x 768 = the 1.5 x 0.8 m face).

  edge_yellow_blue.png     512 x 1024  4 m across x 8 m along, tiles along: blue/white
                                       blocks (1 m) on the outer edge, wide yellow band inside
  edge_yellow_blue_r.png   mirrored copy for the right-hand edge
  pit_exit_blue_green.png  512 x 768   8 x 12 m: solid blue exit line, green hatched
                                       zone, dashed white lane line
  wall_base_brand.png      2048 x 308  4 x 0.6 m: "APEXSIM" in white with a cyan rule
  finish_chequer_wide.png  1536 x 256  12 x 2 m: 0.5 m chequer, 24 x 4 squares
  pit_lane_digits.png      1024 x 1024 4 x 4 atlas, row-major from the top-left:
                                       0..9, roundels 60 / 80 / 100, PIT, arrow, blank
"""
import bpy, bmesh, os, math
import numpy as np

_ROOT = os.environ.get("APEXSIM_ROOT", r"E:\apexsim")
OUT = os.path.join(_ROOT, "content", "props", "decal", "marina")
MARK = os.path.join(_ROOT, "content", "props", "board", "markers")
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"
FONT_B = r"C:\Windows\Fonts\arialbd.ttf"
SS = 3  # supersampling for text edges
rng = np.random.default_rng(11)


def save_png(path, rgba):
    h, w = rgba.shape[:2]
    img = bpy.data.images.new("_tmp_decal", w, h, alpha=True)
    img.pixels.foreach_set(np.flipud(np.clip(rgba, 0, 1)).astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def _font(path):
    for f in bpy.data.fonts:
        if f.filepath == path:
            return f
    return bpy.data.fonts.load(path)


def text_tris(text, font):
    cu = bpy.data.curves.new("_tm", 'FONT')
    cu.body = text
    cu.font = _font(font)
    cu.size = 1.0
    cu.align_x = 'CENTER'
    try:
        cu.fill_mode = 'FRONT'
    except (AttributeError, TypeError):
        pass
    ob = bpy.data.objects.new("_tm", cu)
    bpy.context.scene.collection.objects.link(ob)
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    bpy.data.objects.remove(ob)
    bpy.data.curves.remove(cu)
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    t = np.array([[(v.co.x, v.co.y) for v in f.verts] for f in bm.faces], np.float64)
    bm.free()
    bpy.data.meshes.remove(me)
    return t


def raster(tris, w, h):
    m = np.zeros((h, w), bool)
    for t in tris:
        x0, x1 = max(int(np.floor(t[:, 0].min())), 0), min(int(np.ceil(t[:, 0].max())), w - 1)
        y0, y1 = max(int(np.floor(t[:, 1].min())), 0), min(int(np.ceil(t[:, 1].max())), h - 1)
        if x1 < x0 or y1 < y0:
            continue
        X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        (ax, ay), (bx, by), (cx, cy) = t
        d1 = (X - bx) * (ay - by) - (ax - bx) * (Y - by)
        d2 = (X - cx) * (by - cy) - (bx - cx) * (Y - cy)
        d3 = (X - ax) * (cy - ay) - (cx - ax) * (Y - ay)
        neg = (d1 < 0) | (d2 < 0) | (d3 < 0)
        pos = (d1 > 0) | (d2 > 0) | (d3 > 0)
        m[y0:y1 + 1, x0:x1 + 1] |= ~(neg & pos)
    return m


def text_cov(text, w, h, font=FONT, fill=0.8, cx=0.5, cy=0.5):
    """Coverage (h x w, 0..1) of `text` fitted into fill*w x fill*h, centred at (cx, cy)."""
    t = text_tris(text, font)
    lo, hi = t.reshape(-1, 2).min(0), t.reshape(-1, 2).max(0)
    W, H = w * SS, h * SS
    s = min(W * fill / (hi[0] - lo[0]), H * fill / (hi[1] - lo[1]))
    mid = (lo + hi) / 2
    px = np.empty_like(t)
    px[..., 0] = (t[..., 0] - mid[0]) * s + W * cx
    px[..., 1] = H * cy - (t[..., 1] - mid[1]) * s
    m = raster(px, W, H).astype(np.float32)
    return m.reshape(h, SS, w, SS).mean(axis=(1, 3))


def wear(h, w, amount=0.12, scale=24):
    """Paint wear: a soft noise mask in 0..1 (1 = full paint)."""
    n = rng.random((h // scale + 2, w // scale + 2)).astype(np.float32)
    n = np.kron(n, np.ones((scale, scale), np.float32))[:h, :w]
    fine = rng.random((h, w)).astype(np.float32)
    return np.clip(1.0 - amount * (n * 0.7 + fine * 0.3) * 1.6, 0, 1)


def layer(rgba, cov, rgb):
    a = cov[..., None]
    rgba[..., :3] = rgba[..., :3] * (1 - a) + np.array(rgb, np.float32) * a
    rgba[..., 3] = np.maximum(rgba[..., 3], cov)


WHITE, YELLOW, BLUE, GREEN = (0.95, 0.95, 0.93), (0.98, 0.80, 0.05), (0.05, 0.25, 0.75), (0.15, 0.70, 0.25)


def edge_yellow_blue():
    w, h = 512, 1024                       # 4 m across x 8 m along: 128 px/m
    ppm = 128
    img = np.zeros((h, w, 4), np.float32)
    def band(m0, m1, rgb, rows=None):
        c = np.zeros((h, w), np.float32)
        c[:, int(m0 * ppm):int(m1 * ppm)] = 1.0
        if rows is not None:
            c *= rows[:, None]
        layer(img, c * wear(h, w), rgb)
    blocks = ((np.arange(h) // ppm) % 2).astype(np.float32)          # 1 m blocks along the road
    band(0.00, 0.95, BLUE, rows=blocks)
    band(0.00, 0.95, WHITE, rows=1 - blocks)
    band(0.95, 1.10, WHITE)
    band(1.25, 3.75, YELLOW)                                          # the wide yellow band
    save_png(os.path.join(OUT, "edge_yellow_blue.png"), img)
    save_png(os.path.join(OUT, "edge_yellow_blue_r.png"), img[:, ::-1].copy())


def pit_exit_blue_green():
    w, h, ppm = 512, 768, 64               # 8 x 12 m
    img = np.zeros((h, w, 4), np.float32)
    X, Y = np.meshgrid(np.arange(w) / ppm, np.arange(h) / ppm)
    blue = ((X > 0.4) & (X < 0.9)).astype(np.float32)
    zone = (X > 1.3) & (X < 6.2)
    hatch = (((X + Y) % 1.6) < 0.45) & zone
    edge = zone & ((X < 1.45) | (X > 6.05))
    green = (hatch | edge).astype(np.float32)
    dash = ((X > 7.0) & (X < 7.15) & ((Y % 3.0) < 1.5)).astype(np.float32)
    layer(img, blue * wear(h, w), BLUE)
    layer(img, green * wear(h, w), GREEN)
    layer(img, dash * wear(h, w), WHITE)
    save_png(os.path.join(OUT, "pit_exit_blue_green.png"), img)


def wall_base_brand():
    w, h = 2048, 308
    img = np.zeros((h, w, 4), np.float32)
    layer(img, text_cov("APEXSIM", w, h, FONT_B, fill=0.62, cy=0.46) * wear(h, w, 0.08), WHITE)
    rule = np.zeros((h, w), np.float32)
    rule[int(h * 0.86):int(h * 0.92), int(w * 0.08):int(w * 0.92)] = 1.0
    layer(img, rule * wear(h, w, 0.08), (0.0, 0.78, 1.0))
    save_png(os.path.join(OUT, "wall_base_brand.png"), img)


def finish_chequer_wide():
    w, h = 1536, 256                       # 12 x 2 m, 0.5 m squares (64 px)
    X, Y = np.meshgrid(np.arange(w) // 64, np.arange(h) // 64)
    white = ((X + Y) % 2 == 0).astype(np.float32)
    img = np.zeros((h, w, 4), np.float32)
    img[..., :3] = 0.03
    layer(img, white * wear(h, w, 0.06), WHITE)
    img[..., 3] = 1.0
    save_png(os.path.join(OUT, "finish_chequer_wide.png"), img)


def pit_lane_digits():
    C, N = 256, 4
    img = np.zeros((C * N, C * N, 4), np.float32)
    cells = [str(i) for i in range(10)] + ["r60", "r80", "r100", "PIT", "arrow", ""]
    Y, X = np.mgrid[0:C, 0:C] + 0.5
    r = np.hypot(X - C / 2, Y - C / 2)
    for k, c in enumerate(cells):
        cell = np.zeros((C, C, 4), np.float32)
        if c.isdigit():
            layer(cell, text_cov(c, C, C, FONT, fill=0.78), WHITE)
        elif c.startswith("r"):
            layer(cell, (r < C * 0.47).astype(np.float32), WHITE)
            layer(cell, ((r < C * 0.47) & (r > C * 0.37)).astype(np.float32), (0.8, 0.05, 0.05))
            layer(cell, text_cov(c[1:], C, C, FONT, fill=0.42), (0.02, 0.02, 0.02))
        elif c == "PIT":
            layer(cell, text_cov(c, C, C, FONT, fill=0.8), WHITE)
        elif c == "arrow":
            shaft = (abs(X - C / 2) < C * 0.08) & (Y > C * 0.42) & (Y < C * 0.92)
            head = (Y > C * 0.08) & (Y <= C * 0.46) & (abs(X - C / 2) < (Y - C * 0.08) * 0.75)
            layer(cell, (shaft | head).astype(np.float32), WHITE)
        cell[..., 3] *= wear(C, C, 0.06)
        ry, rx = divmod(k, N)
        img[ry * C:(ry + 1) * C, rx * C:(rx + 1) * C] = cell
    save_png(os.path.join(OUT, "pit_lane_digits.png"), img)


def garage_numbers(n=24):
    w, h = 1536, 768
    for i in range(1, n + 1):
        img = np.zeros((h, w, 4), np.float32)
        img[..., :3] = (0.06, 0.07, 0.09)
        img[..., 3] = 1.0
        bar = np.zeros((h, w), np.float32)
        bar[:int(h * 0.06), :] = 1.0
        bar[-int(h * 0.06):, :] = 1.0
        layer(img, bar, (0.0, 0.78, 1.0))
        layer(img, text_cov(str(i), w, h, FONT, fill=0.66), WHITE)
        save_png(os.path.join(MARK, f"{i}.png"), img)


os.makedirs(OUT, exist_ok=True)
os.makedirs(MARK, exist_ok=True)
edge_yellow_blue()
pit_exit_blue_green()
wall_base_brand()
finish_chequer_wide()
pit_lane_digits()
garage_numbers()
RESULT = sorted(os.listdir(OUT))
