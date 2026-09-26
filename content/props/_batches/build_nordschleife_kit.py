r"""Nordschleife kit: German guard rail, German signs, Burg Nürburg.

Run inside Blender (Text Editor or console):

    ASSET = "all"        # or one of ASSETS
    exec(open(r"D:\apexsim\content\props\_batches\build_nordschleife_kit.py").read())

or headless with the `bpy` module (APEXSIM_ROOT = the repo):

    APEXSIM_ROOT=/path/to/apexsim python -c "exec(open('content/props/_batches/build_nordschleife_kit.py').read())"

Writes content/props/<kind>/<asset>.glb for every asset below, the sign
textures they carry to content/props/_textures/, the km board faces to
content/props/board/markers/km<N>.png (the marker slot's `text`), and saves
this batch's scene as content/props/_batches/nordschleife_kit.blend.

Frame per docs/PROPS.md: +X along the road, road on -Y, Z up, pivot on the
ground (thin modules and the castle centred on their footprint; signs on
their post).

- **vangrail** ("Schutzplanke", what the Dutch call vangrail): the German
  road-style guard rail that lines the Nordschleife. Unlike the UK-style
  triple-rail `armco_4m` it is separate A-profile W-beams bolted to spacer
  blocks on C-posts every 2 m, with daylight between the beams.
  `vangrail_4m` is the Nordschleife's usual double rail, `vangrail_4m_triple`
  the three-high rail of the fast sections, `vangrail_4m_fence` the double
  rail under a catch fence where people stand, `vangrail_end` the rounded
  end piece. Material slots are the armco's (`armco_galv`, `armco_post`,
  `armco_bolt`) and the fence's (`fence_post`, `fence_mesh`) so the
  importer shares them with the kit.
- **signs** (`board` kind, all read by a driver coming up the road, so the
  importer turns them to face up the course like a braking marker):
  `chevron_left` / `chevron_right` (Zeichen 625 Richtungstafel, red on
  white, on the outside of a bend, pointing the way it turns),
  `km_marker` (the Ring's kilometre boards, number = `text` on the
  `board_marker` slot: T_marker_km<N>), `de_curve_left` / `de_curve_right`
  (Zeichen 103 / 105), `de_danger` (Zeichen 101), `de_overtake_left`
  ("Überholen nur links", the Touristenfahrten board).
- **castle_ruin**: Burg Nürburg on its basalt cone inside the loop: the
  round keep (Bergfried) with its crenellated viewing top, the ring of the
  inner ward, the broken outer curtain wall with a gate tower. Centred on
  its footprint; a 6 m plinth below the pivot hides the hillside slope.
"""
import bpy, bmesh, math, os, importlib.util, sys
from mathutils import Vector

_ROOT = os.environ.get("APEXSIM_ROOT", r"D:\apexsim")
os.environ.setdefault("APEXSIM_ROOT", _ROOT)
_s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "content", "props", "_tools", "apex_props.py"))
apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
_t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "content", "props", "_tools", "apex_tex.py"))
tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
B, M = apex.Builder, tex.kit_material

PROPS = os.path.join(_ROOT, "content", "props")
TEX_DIR = os.path.join(PROPS, "_textures")

ASSETS = [
    ("barrier", "vangrail_4m"),
    ("barrier", "vangrail_4m_triple"),
    ("barrier", "vangrail_4m_fence"),
    ("barrier", "vangrail_end"),
    ("board", "chevron_left"),
    ("board", "chevron_right"),
    ("board", "km_marker"),
    ("board", "de_curve_left"),
    ("board", "de_curve_right"),
    ("board", "de_danger"),
    ("board", "de_overtake_left"),
    ("building", "castle_ruin"),
]
try:
    ASSET
except NameError:
    ASSET = "all"

# ------------------------------------------------------------------ textures


def _font(size):
    from PIL import ImageFont
    for f in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "arialbd.ttf", "C:/Windows/Fonts/arialbd.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def _save(img, name):
    os.makedirs(TEX_DIR, exist_ok=True)
    p = os.path.join(TEX_DIR, name + ".png")
    img.save(p)
    return p


RED = (200, 16, 30)
WHITE = (245, 245, 240)
BLUE = (0, 72, 150)
BLACK = (18, 18, 18)
YELLOW = (250, 200, 20)


def chevron_texture():
    """Zeichen 625: white board, red chevrons pointing right (+u)."""
    from PIL import Image, ImageDraw
    w, h = 1024, 256
    img = Image.new("RGB", (w, h), WHITE)
    d = ImageDraw.Draw(img)
    for i in range(3):
        x0 = 120 + i * 280
        d.polygon([(x0, 20), (x0 + 110, 20), (x0 + 230, h // 2), (x0 + 110, h - 20),
                   (x0, h - 20), (x0 + 120, h // 2)], fill=RED)
    d.rectangle([0, 0, w - 1, h - 1], outline=RED, width=10)
    return _save(img, "sign_chevron_col")


def triangle_sign(name, draw_inside):
    """A German danger sign (Zeichen 1xx): white triangle, red border."""
    from PIL import Image, ImageDraw
    s = 512
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top, bl, br = (s / 2, 18), (14, s - 50), (s - 14, s - 50)
    d.polygon([top, bl, br], fill=RED)
    inset = 52
    d.polygon([(s / 2, 18 + inset * 1.6), (14 + inset * 1.35, s - 50 - inset * 0.72),
               (s - 14 - inset * 1.35, s - 50 - inset * 0.72)], fill=WHITE)
    draw_inside(d, s)
    return _save(img, name)


def _curve(d, s, left):
    """The bend arrow of Zeichen 103/105: a thick line up, bending left or right."""
    sign = -1 if left else 1
    x = s / 2 - sign * 30
    pts = [(x, s - 150), (x, s - 250), (x + sign * 70, s - 320)]
    d.line(pts, fill=BLACK, width=34, joint="curve")
    tip = (x + sign * 105, s - 355)
    d.polygon([tip, (x + sign * 40, s - 360), (x + sign * 95, s - 290)], fill=BLACK)


def _danger(d, s):
    d.rectangle([s / 2 - 17, 190, s / 2 + 17, 350], fill=BLACK)
    d.ellipse([s / 2 - 20, 370, s / 2 + 20, 410], fill=BLACK)


def overtake_texture():
    """The Touristenfahrten board: blue, white text, arrow to the left."""
    from PIL import Image, ImageDraw
    w, h = 768, 512
    img = Image.new("RGB", (w, h), BLUE)
    d = ImageDraw.Draw(img)
    d.rectangle([12, 12, w - 13, h - 13], outline=WHITE, width=10)
    f = _font(92)
    for i, line in enumerate(("ÜBERHOLEN", "NUR LINKS")):
        tw = d.textlength(line, font=f)
        d.text(((w - tw) / 2, 60 + i * 120), line, fill=WHITE, font=f)
    d.line([(230, 400), (560, 400)], fill=WHITE, width=34)
    d.polygon([(170, 400), (270, 340), (270, 460)], fill=WHITE)
    return _save(img, "sign_de_overtake_col")


def km_textures():
    """The kilometre boards' faces, one per km: black number on yellow,
    `km` under it. Written where the marker set lives, so ApexPropImport
    brings them in as T_marker_km<N> (the prop's text is `km<N>`)."""
    from PIL import Image, ImageDraw
    out = os.path.join(PROPS, "board", "markers")
    for n in range(1, 21):
        img = Image.new("RGB", (768, 1024), YELLOW)
        d = ImageDraw.Draw(img)
        d.rectangle([20, 20, 747, 1003], outline=BLACK, width=24)
        f = _font(430 if n < 10 else 360)
        t = str(n)
        tw = d.textlength(t, font=f)
        d.text(((768 - tw) / 2, 150), t, fill=BLACK, font=f)
        f2 = _font(170)
        tw = d.textlength("km", font=f2)
        d.text(((768 - tw) / 2, 700), "km", fill=BLACK, font=f2)
        img.save(os.path.join(out, f"km{n}.png"))
    return out


# ------------------------------------------------------------------ vangrail

BEAM_H = 0.31      # A-profile height
BEAM_D = 0.085     # its bulge toward the road
BEAM_Y = -0.19     # back face of the beam (road side of the spacers)
POST_Y = (0.0, 0.055)
SPACER_Y = (BEAM_Y + 0.004, 0.0)
POSTS_X = (-1.0, 1.0)


def w_profile(z0, y_back=BEAM_Y):
    """The W-beam's cross-section, (y, z), road-facing front then back."""
    f = [(0.0, 0.0), (-BEAM_D, 0.075), (-0.02, 0.155), (-BEAM_D, 0.235), (0.0, BEAM_H)]
    front = [(y_back + dy, z0 + dz) for dy, dz in f]
    back = [(y_back + dy + 0.004, z0 + dz) for dy, dz in reversed(f)]
    return front + back


def beam(b, galv, bolt, z0, x0=-2.0, x1=2.0):
    b.extrude_profile(galv, w_profile(z0), x0, x1, closed=True)
    for px in POSTS_X:
        if x0 <= px <= x1:
            for dz in (0.1, 0.21):
                b.cylinder(bolt, (px, BEAM_Y - 0.03, z0 + dz), 0.014, 0.03, segs=6, axis='Y')


def post(b, mat, x, height):
    """A C-post, open toward the field, 100 x 55 mm."""
    t = 0.006
    b.box(mat, (x - 0.05, POST_Y[0], -0.05), (x + 0.05, POST_Y[0] + t, height))
    b.box(mat, (x - 0.05, POST_Y[0], -0.05), (x - 0.05 + t, POST_Y[1], height))
    b.box(mat, (x + 0.05 - t, POST_Y[0], -0.05), (x + 0.05, POST_Y[1], height))


def spacer(b, mat, x, z0):
    b.box(mat, (x - 0.04, SPACER_Y[0], z0 + 0.04), (x + 0.04, SPACER_Y[1], z0 + BEAM_H - 0.04))


def vangrail(name, beams, fence=False):
    b = B(name)
    galv, post_m, bolt = M("armco_galv"), M("armco_post"), M("armco_bolt", (0.35, 0.35, 0.36), metallic=0.9, roughness=0.4)
    top = beams[-1] + BEAM_H + 0.04
    for x in POSTS_X:
        post(b, post_m, x, top)
        for z0 in beams:
            spacer(b, post_m, x, z0)
    for z0 in beams:
        beam(b, galv, bolt, z0)
    if fence:
        fp = M("fence_post", (0.4, 0.42, 0.43), metallic=0.8, roughness=0.5)
        # The kit's own masked mesh material, borrowed from armco_4m_fence (see run()).
        mesh = bpy.data.materials["fence_mesh"]
        for x in (-2.0, 0.0, 2.0):
            b.cylinder(fp, (x, 0.19, 0.0), 0.04, 3.5, segs=8)
        for z in (top + 0.05, 3.45):
            b.cylinder(fp, (-2.0, 0.19, z), 0.025, 4.0, segs=6, axis='X')
        b.quad_uv(mesh, [(-2.0, 0.19, top), (2.0, 0.19, top), (2.0, 0.19, 3.48), (-2.0, 0.19, 3.48)],
                  [(0, 0), (4, 0), (4, 3.48 - top), (0, 3.48 - top)])
        # And from the far side, so the mesh reads from the forest too.
        b.quad_uv(mesh, [(-2.0, 0.195, top), (-2.0, 0.195, 3.48), (2.0, 0.195, 3.48), (2.0, 0.195, top)],
                  [(0, 0), (0, 3.48 - top), (4, 3.48 - top), (4, 0)])
    return b.finish()


def vangrail_end():
    """The rounded end piece: the double rail's two beams curl back away
    from the road at both ends of a 2 m module (laid overlapping a run's
    last module, whichever end)."""
    b = B("vangrail_end")
    galv, post_m, bolt = M("armco_galv"), M("armco_post"), M("armco_bolt", (0.35, 0.35, 0.36), metallic=0.9, roughness=0.4)
    beams = (0.40, 0.85)
    post(b, post_m, 0.0, beams[-1] + BEAM_H + 0.04)
    for z0 in beams:
        spacer(b, post_m, 0.0, z0)
        beam(b, galv, bolt, z0, -0.7, 0.7)
        # Curls: the profile swept round a quarter circle at each end.
        for end in (-1, 1):
            r, n = 0.3, 6
            prev = None
            for i in range(n + 1):
                a = math.pi / 2 * i / n
                cx, cy = end * (0.7 + r * math.sin(a)), BEAM_Y + r * (1 - math.cos(a))
                ring = [(cx, cy + dy, z) for dy, z in [(p[0] - BEAM_Y, p[1]) for p in w_profile(z0)]]
                ring = [b.bm.verts.new(v) for v in ring]
                if prev is not None:
                    s = b.slot(galv)
                    for k in range(len(ring)):
                        k2 = (k + 1) % len(ring)
                        q = (prev[k], ring[k], ring[k2], prev[k2]) if end > 0 else (prev[k], prev[k2], ring[k2], ring[k])
                        b.bm.faces.new(q).material_index = s
                prev = ring
    return b.finish()


# ------------------------------------------------------------------- signs


def board_on_posts(name, face_mat, w, h, bottom, posts, uv=((0, 0), (1, 0), (1, 1), (0, 1)), back="board_back"):
    """A flat sign facing the road (-Y) on round posts, its face `face_mat`."""
    b = B(name)
    pm = M("board_post")
    bk = M(back)
    for x in posts:
        b.cylinder(pm, (x, 0.04, 0.0), 0.03, bottom + h, segs=8)
    y = -0.005
    b.quad_uv(face_mat, [(-w / 2, y, bottom), (w / 2, y, bottom), (w / 2, y, bottom + h), (-w / 2, y, bottom + h)], list(uv))
    b.box(bk, (-w / 2, 0.0, bottom), (w / 2, 0.02, bottom + h))
    return b


def chevron(name, left):
    col = chevron_texture()
    mat = apex.image_material("sign_chevron", col, roughness=0.35)
    # The texture points right; a left bend mirrors it across u.
    uv = ((1, 0), (0, 0), (0, 1), (1, 1)) if left else ((0, 0), (1, 0), (1, 1), (0, 1))
    return board_on_posts(name, mat, 1.6, 0.4, 0.9, (-0.6, 0.6), uv).finish()


def km_marker():
    out = km_textures()
    mat = apex.image_material("board_marker", os.path.join(out, "km1.png"), roughness=0.4)
    return board_on_posts("km_marker", mat, 0.6, 0.8, 1.0, (0.0,)).finish()


def triangle(name, draw):
    col = triangle_sign("sign_" + name + "_col", draw)
    mat = apex.image_material("sign_" + name, col, roughness=0.35, masked=True)
    b = B(name)
    pm = M("board_post")
    b.cylinder(pm, (0.0, 0.04, 0.0), 0.035, 2.6, segs=8)
    s, bottom = 0.9, 1.75
    b.quad_uv(mat, [(-s / 2, -0.005, bottom), (s / 2, -0.005, bottom), (s / 2, -0.005, bottom + s), (-s / 2, -0.005, bottom + s)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    # The grey back of the plate, a triangle.
    bk = M("board_back")
    top = bottom + s * (1 - 18 / 512)
    b.sheet(bk, (-s / 2 + 0.02, 0.006, bottom + s * 50 / 512), (s / 2 - 0.02, 0.006, bottom + s * 50 / 512), (0.0, 0.006, top), (0.0, 0.006, top))
    return b.finish()


def overtake():
    col = overtake_texture()
    mat = apex.image_material("sign_de_overtake", col, roughness=0.35)
    return board_on_posts("de_overtake_left", mat, 1.5, 1.0, 1.4, (-0.55, 0.55)).finish()


# ------------------------------------------------------------------ castle


def castle_ruin():
    """Burg Nürburg: keep, inner ward, broken outer wall, gate tower.

    Centred on its footprint (the keep is at the pivot). About 70 x 56 m,
    the keep 26 m with its battlements."""
    b = B("castle_ruin")
    stone = M("house_stone")
    dark = M("castle_basalt", (0.22, 0.21, 0.2), roughness=0.9)
    roof = M("house_roof_dark")
    base = -6.0   # hides the hill's slope under the walls

    # The basalt knoll the walls stand on.
    b.cylinder(dark, (0, 0, base), 30.0, -base + 1.5, segs=20)

    def ring_wall(rx, ry, height, thick, gaps, mat, segs=28, crenel=True, cx=0.0, cy=0.0):
        for i in range(segs):
            if i in gaps:
                continue
            a0, a1 = 2 * math.pi * i / segs, 2 * math.pi * (i + 1) / segs
            p0 = Vector((cx + rx * math.cos(a0), cy + ry * math.sin(a0), 0))
            p1 = Vector((cx + rx * math.cos(a1), cy + ry * math.sin(a1), 0))
            # A ruin: the wall height wanders, lower where it has fallen.
            h = height * (0.55 + 0.45 * abs(math.sin(i * 1.7 + 0.4)))
            d = (p1 - p0)
            n = Vector((-d.y, d.x, 0)).normalized() * thick / 2
            q = [p0 - n, p1 - n, p1 + n, p0 + n]
            s = b.slot(mat)
            lo = [b.bm.verts.new((v.x, v.y, base)) for v in q]
            hi = [b.bm.verts.new((v.x, v.y, h)) for v in q]
            for k in range(4):
                k2 = (k + 1) % 4
                b.bm.faces.new((lo[k], lo[k2], hi[k2], hi[k])).material_index = s
            b.bm.faces.new(list(reversed(hi))).material_index = s
            if crenel and h > height * 0.8:
                # Merlons along the top.
                for t in (0.15, 0.55):
                    c0 = p0 + d * t
                    c1 = p0 + d * (t + 0.25)
                    b.box(mat, (min(c0.x, c1.x) - thick / 3, min(c0.y, c1.y) - thick / 3, h),
                          (max(c0.x, c1.x) + thick / 3, max(c0.y, c1.y) + thick / 3, h + 1.0))

    # Outer curtain wall, broken in three places, and its gate.
    ring_wall(34.0, 27.0, 7.0, 1.6, gaps={3, 11, 19}, mat=stone)
    # Inner ward.
    ring_wall(17.0, 14.0, 10.0, 1.8, gaps={7}, mat=stone, segs=20, cx=-3.0, cy=2.0)
    # Gate tower on the outer wall.
    gx, gy = 34.0 * math.cos(2 * math.pi * 3.5 / 28), 27.0 * math.sin(2 * math.pi * 3.5 / 28)
    b.box(stone, (gx - 4.0, gy - 4.0, base), (gx + 4.0, gy + 4.0, 13.0))
    b.cylinder(roof, (gx, gy, 13.0), 0.1, 0.1, segs=4)
    # The keep: a round tower with a battlement ring and a viewing platform.
    b.cylinder(stone, (0, 0, base), 6.0, 24.0 - base, segs=24)
    b.cylinder(stone, (0, 0, 24.0), 6.6, 0.8, segs=24)
    for i in range(12):
        a = 2 * math.pi * i / 12
        x, y = 6.2 * math.cos(a), 6.2 * math.sin(a)
        b.box(stone, (x - 0.6, y - 0.6, 24.8), (x + 0.6, y + 0.6, 26.0))
    # Slit windows (dark insets) round the keep.
    for i in range(6):
        a = 2 * math.pi * i / 6 + 0.3
        for z in (8.0, 14.0, 19.0):
            x, y = 6.02 * math.cos(a), 6.02 * math.sin(a)
            b.box(dark, (x - 0.25, y - 0.25, z), (x + 0.25, y + 0.25, z + 1.4))
    # A hall's gable wall in the inner ward.
    b.box(stone, (-12.0, 6.0, base), (-2.0, 7.6, 12.0))
    return b.finish()


# ------------------------------------------------------------------ driver

BUILD = {
    "vangrail_4m": lambda: vangrail("vangrail_4m", (0.40, 0.85)),
    "vangrail_4m_triple": lambda: vangrail("vangrail_4m_triple", (0.40, 0.80, 1.20)),
    "vangrail_4m_fence": lambda: vangrail("vangrail_4m_fence", (0.40, 0.85), fence=True),
    "vangrail_end": vangrail_end,
    "chevron_left": lambda: chevron("chevron_left", True),
    "chevron_right": lambda: chevron("chevron_right", False),
    "km_marker": km_marker,
    "de_curve_left": lambda: triangle("de_curve_left", lambda d, s: _curve(d, s, True)),
    "de_curve_right": lambda: triangle("de_curve_right", lambda d, s: _curve(d, s, False)),
    "de_danger": lambda: triangle("de_danger", _danger),
    "de_overtake_left": overtake,
    "castle_ruin": castle_ruin,
}


def run(which="all"):
    apex.reset_scene()
    tex.reset_cache()
    # Borrow the catch fence's masked mesh material from the kit's own
    # armco_4m_fence, so the two fences look alike standing side by side.
    bpy.ops.import_scene.gltf(filepath=os.path.join(PROPS, "barrier", "armco_4m_fence.glb"))
    for ob in list(bpy.context.scene.objects):
        bpy.data.objects.remove(ob)
    for name in ("armco_galv", "armco_post", "armco_bolt", "fence_post"):
        m = bpy.data.materials.get(name)
        if m is not None:
            m.name = name + "_borrowed"
    done = []
    for kind, asset in ASSETS:
        if which not in ("all", asset):
            continue
        ob = BUILD[asset]()
        # Spread them out in the saved scene; export puts each at the origin.
        ob.location = (len(done) * 12.0, 0, 0)
        done.append((kind, asset, apex.export_one(kind, asset)))
    apex.save_blend("_batches", "nordschleife_kit")
    for kind, asset, (path, size) in done:
        print(f"  {kind}/{asset}.glb  {size // 1024} KiB")
    return done


run(ASSET)
