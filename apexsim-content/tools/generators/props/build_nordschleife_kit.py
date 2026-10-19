r"""Nordschleife kit: German guard rail, German signs, Burg Nürburg.

Run inside Blender (Text Editor or console):

    ASSET = "all"        # or one of ASSETS, or several: "a,b,c"
    exec(open(r"E:\apexsim\apexsim-content\tools\generators\props\build_nordschleife_kit.py").read())

or headless (the user may have a file of their own open in Blender):

    "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" -b --factory-startup ^
        --python-expr "ASSET='all'; exec(open(r'E:\apexsim\apexsim-content\tools\generators\props\build_nordschleife_kit.py', encoding='utf-8').read())"

Blender's own Python has no PIL, so the sign faces and number plates are
painted first by the system Python, which has no `bpy`:

    python tools/generators/props/build_nordschleife_kit.py      # -> _textures/, board/markers/

and the Blender run loads the PNGs it finds.

Writes props/<kind>/<asset>.glb for every asset below, the sign
textures they carry to props/_textures/, the km board faces to
props/board/markers/km<N>.png (the marker slot's `text`), and saves
this batch's scene as props/_batches/nordschleife_kit.blend.

Frame per docs/props.md: +X along the road, road on -Y, Z up, pivot on the
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

The dressing pieces (docs/circuits.md, Nordschleife):

- **marshal_post_numbered** (`sign`): the kit's `marshal_post` with the
  post's number board in front of it; the number is the prop's `text` on
  the `board_marker` slot (`post12` -> T_marker_post12, written to
  board/markers/ here).
- **de_descent / de_climb** (Zeichen 108 / 110) with the grade on a plate
  under the triangle (`board_marker`, `grade12` -> T_marker_grade12), and
  **de_double_curve_left / _right** (Zeichen 105).
- **rail_tag_<name>** (`board`): the crowd's tags painted on the double
  rail, a masked sheet wrapped round both W-beams of a `vangrail_4m`
  module (laid on one by the groomer). Textures from gen_graffiti.py.
- **track_bridge_10m** (`bridge`): the parapets and fascias of a bridge
  carrying the lap over a road (the B 257 at Quiddelbach and Breidscheid),
  tiled along the span; authored for a 15 m road, scaled to the real one.
- **overpass_concrete** (`bridge`): a forest track's concrete beam bridge
  over the lap, its abutments set back behind the rails.
- **toll_gate_lane** (`misc`): a Touristenfahrten toll lane, booth and
  lift arm, the arm along local X; centred.
- **stone_lookout_tower** (`building`): the 16 m stone lookout tower on
  the Hohe Acht; centred, a plinth below the pivot.
- **spectator_bank_10m** (`misc`): a low rounded turf bank (the ground
  set's grass) with a hedge of bush cards on its crest, where the crowd
  stands behind the rail; symmetric and centred.
- **camp_scaffold_tower**, **camp_gazebo**, **camp_flag_pole** (`misc`):
  the 24-hour camps' home-built scaffold towers, pop-up gazebos and flag
  poles (`flag_cloth`, the flag code as `text`).
"""
import math, os, importlib.util, sys

try:
    import bpy, bmesh
    from mathutils import Vector
except ImportError:  # the system Python: paint the textures only
    bpy = None
try:
    import PIL  # noqa: F401
    HAVE_PIL = True
except ImportError:  # Blender's Python: load what the system Python painted
    HAVE_PIL = False

# The content root (apexsim-content): three folders above this script
# (tools/generators/<group>/), else $APEXSIM_CONTENT (a script exec'd in Blender
# has no __file__), else the checkout on E:.
_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
_ROOT = (os.path.abspath(os.path.join(_SELF, "..", "..", ".."))
         if os.path.isdir(os.path.join(_SELF, "..", "..", "generators"))
         else os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"))
os.environ.setdefault("APEXSIM_CONTENT", _ROOT)
if bpy is not None:
    _s = importlib.util.spec_from_file_location("apex", os.path.join(_ROOT, "tools", "generators", "props", "apex_props.py"))
    apex = importlib.util.module_from_spec(_s); sys.modules["apex"] = apex; _s.loader.exec_module(apex)
    _t = importlib.util.spec_from_file_location("apex_tex", os.path.join(_ROOT, "tools", "generators", "props", "apex_tex.py"))
    tex = importlib.util.module_from_spec(_t); sys.modules["apex_tex"] = tex; _t.loader.exec_module(tex)
    B, M = apex.Builder, tex.kit_material

PROPS = os.path.join(_ROOT, "props")
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
    ("sign", "marshal_post_numbered"),
    ("board", "de_descent"),
    ("board", "de_climb"),
    ("board", "de_double_curve_left"),
    ("board", "de_double_curve_right"),
    ("board", "rail_tag_kalle"),
    ("board", "rail_tag_eifel"),
    ("board", "rail_tag_vollgas"),
    ("board", "rail_tag_hup"),
    ("board", "rail_tag_moppel"),
    ("board", "rail_tag_hearts"),
    ("bridge", "track_bridge_10m"),
    ("bridge", "overpass_concrete"),
    ("misc", "toll_gate_lane"),
    ("building", "stone_lookout_tower"),
    ("misc", "spectator_bank_10m"),
    ("misc", "camp_scaffold_tower"),
    ("misc", "camp_gazebo"),
    ("misc", "camp_flag_pole"),
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


def _painted(path):
    """In Blender (no PIL): the PNG the system Python painted, which has to
    be there."""
    if not os.path.exists(path):
        raise SystemExit(f"{path} is missing: run this script with the system Python first")
    return path


RED = (200, 16, 30)
WHITE = (245, 245, 240)
BLUE = (0, 72, 150)
BLACK = (18, 18, 18)
YELLOW = (250, 200, 20)


def chevron_texture():
    """Zeichen 625: white board, red chevrons pointing right (+u)."""
    if not HAVE_PIL:
        return _painted(os.path.join(TEX_DIR, "sign_chevron_col.png"))
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
    if not HAVE_PIL:
        return _painted(os.path.join(TEX_DIR, name + ".png"))
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
    if not HAVE_PIL:
        return _painted(os.path.join(TEX_DIR, "sign_de_overtake_col.png"))
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
    out = os.path.join(PROPS, "board", "markers")
    if not HAVE_PIL:
        _painted(os.path.join(out, "km1.png"))
        return out
    from PIL import Image, ImageDraw
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


# ------------------------------------------------------- the dressing pieces


def import_glb(kind, asset):
    """Import one of the kit's own GLBs; returns (every new object, the mesh objects)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(PROPS, kind, asset + ".glb"))
    new = [o for o in bpy.data.objects if o not in before]
    return new, [o for o in new if o.type == 'MESH']


def absorb(b, objects, transform=None):
    """Copy mesh objects into Builder `b` in world space (then `transform`,
    a 4x4 Matrix, if given), materials and UVs kept."""
    for ob in objects:
        me = ob.data
        uv = me.uv_layers.active
        mw = ob.matrix_world if transform is None else transform @ ob.matrix_world
        verts = [b.bm.verts.new(mw @ v.co) for v in me.vertices]
        for poly in me.polygons:
            mat = ob.material_slots[poly.material_index].material if ob.material_slots else None
            try:
                f = b.bm.faces.new([verts[i] for i in poly.vertices])
            except ValueError:
                continue
            f.material_index = b.slot(mat)
            if uv is not None:
                for loop, li in zip(f.loops, poly.loop_indices):
                    loop[b.uv].uv = uv.data[li].uv
            b.keep.add(f)


def world_bounds(objects):
    pts = [o.matrix_world @ Vector(c) for o in objects for c in o.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return lo, hi


def borrow_material(kind, asset, name):
    """A material of an existing kit GLB, by slot name, so a new piece of the
    same kind looks like its neighbours (the importer shares slots by name
    across a kind, so it has to be the same material anyway)."""
    m = bpy.data.materials.get(name)
    if m is not None and m.get("apex_borrowed"):
        return m
    new, _ = import_glb(kind, asset)
    got = bpy.data.materials.get(name)
    for ob in new:
        bpy.data.objects.remove(ob)
    if got is not None:
        got["apex_borrowed"] = True
    return got


def plate_texture(path, text, size=(512, 256)):
    """A white plate with a black border and black text."""
    if not HAVE_PIL:
        return _painted(path)
    from PIL import Image, ImageDraw
    w, h = size
    img = Image.new("RGB", size, WHITE)
    d = ImageDraw.Draw(img)
    d.rectangle([6, 6, w - 7, h - 7], outline=BLACK, width=12)
    f = _font(int(h * 0.62))
    tw = d.textlength(text, font=f)
    d.text(((w - tw) / 2, h * 0.13), text, fill=BLACK, font=f)
    img.save(path)
    return path


def post_textures():
    """The marshal posts' number boards, post1..post120, in the marker set
    (imported as T_marker_post<N>)."""
    out = os.path.join(PROPS, "board", "markers")
    for n in range(1, 121):
        plate_texture(os.path.join(out, f"post{n}.png"), str(n))
    return os.path.join(out, "post1.png")


def grade_textures():
    """The grade plates under the hill signs, grade5..grade25 (T_marker_grade<N>)."""
    out = os.path.join(PROPS, "board", "markers")
    for n in range(5, 26):
        plate_texture(os.path.join(out, f"grade{n}.png"), f"{n} %")
    return os.path.join(out, "grade10.png")


def marshal_post_numbered():
    """The kit's marshal post with its number board on two posts in front
    of it, read from the road."""
    new, meshes = import_glb("sign", "marshal_post")
    lo, hi = world_bounds(meshes)
    b = B("marshal_post_numbered")
    absorb(b, meshes)
    for ob in new:
        bpy.data.objects.remove(ob)
    face = apex.image_material("board_marker", post_textures(), roughness=0.4)
    pm, bk = M("board_post"), M("board_back")
    w, h, bottom = 1.3, 0.65, 1.9
    cx = (lo.x + hi.x) / 2
    y = lo.y - 0.35
    for x in (cx - 0.5, cx + 0.5):
        b.cylinder(pm, (x, y + 0.05, 0.0), 0.03, bottom + h, segs=8)
    b.quad_uv(face, [(cx - w / 2, y - 0.005, bottom), (cx + w / 2, y - 0.005, bottom),
                     (cx + w / 2, y - 0.005, bottom + h), (cx - w / 2, y - 0.005, bottom + h)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    b.box(bk, (cx - w / 2, y, bottom), (cx + w / 2, y + 0.02, bottom + h))
    return b.finish()


def _slope_symbol(d, s, down):
    """Zeichen 108 / 110: a black wedge, falling (108) or rising (110) to the right."""
    left, right, base, top = 185, s - 160, s - 125, s - 235
    if down:
        d.polygon([(left, top), (right, base), (left, base)], fill=BLACK)
    else:
        d.polygon([(left, base), (right, top), (right, base)], fill=BLACK)


def _double_curve(d, s, left_first):
    """Zeichen 105: the arrow bends one way, then back."""
    sign = -1 if left_first else 1
    x = s / 2
    pts = [(x - sign * 20, s - 140), (x - sign * 20, s - 185), (x + sign * 38, s - 240),
           (x + sign * 38, s - 275), (x - sign * 28, s - 325)]
    d.line(pts, fill=BLACK, width=30, joint="curve")
    tip = (x - sign * 52, s - 360)
    d.polygon([tip, (x - sign * 2, s - 350), (x - sign * 50, s - 298)], fill=BLACK)


TRIANGLES = {
    "de_curve_left": lambda d, s: _curve(d, s, True),
    "de_curve_right": lambda d, s: _curve(d, s, False),
    "de_danger": lambda d, s: _danger(d, s),
    "de_double_curve_left": lambda d, s: _double_curve(d, s, True),
    "de_double_curve_right": lambda d, s: _double_curve(d, s, False),
    "de_descent": lambda d, s: _slope_symbol(d, s, True),
    "de_climb": lambda d, s: _slope_symbol(d, s, False),
}


def paint_textures():
    """Every PNG this kit carries, painted with PIL (the system Python)."""
    chevron_texture()
    overtake_texture()
    km_textures()
    post_textures()
    grade_textures()
    for name, draw in TRIANGLES.items():
        triangle_sign("sign_" + name + "_col", draw)
    print("painted the Nordschleife kit's textures")


def hill_sign(name, down):
    """A gradient warning: the triangle (masked, its own slot) and the grade
    on a plate under it (`board_marker`, the prop's `text`)."""
    col = triangle_sign("sign_" + name + "_col", lambda d, s: _slope_symbol(d, s, down))
    mat = apex.image_material("sign_" + name, col, roughness=0.35, masked=True)
    plate = apex.image_material("board_marker", grade_textures(), roughness=0.4)
    b = B(name)
    pm, bk = M("board_post"), M("board_back")
    b.cylinder(pm, (0.0, 0.04, 0.0), 0.035, 2.6, segs=8)
    s, bottom = 0.9, 1.75
    b.quad_uv(mat, [(-s / 2, -0.005, bottom), (s / 2, -0.005, bottom), (s / 2, -0.005, bottom + s), (-s / 2, -0.005, bottom + s)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    top = bottom + s * (1 - 18 / 512)
    b.sheet(bk, (-s / 2 + 0.02, 0.006, bottom + s * 50 / 512), (s / 2 - 0.02, 0.006, bottom + s * 50 / 512), (0.0, 0.006, top), (0.0, 0.006, top))
    pw, ph, pb = 0.6, 0.3, 1.38
    b.quad_uv(plate, [(-pw / 2, -0.005, pb), (pw / 2, -0.005, pb), (pw / 2, -0.005, pb + ph), (-pw / 2, -0.005, pb + ph)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    b.box(bk, (-pw / 2, 0.0, pb), (pw / 2, 0.02, pb + ph))
    return b.finish()


def rail_tag(name):
    """A tag painted on a double rail module: the road faces of both
    W-beams (z 0.40 and 0.85), 4 mm proud of the steel, the picture running
    over the pair as if the gap between them were not there."""
    mat = apex.image_material(name, os.path.join(TEX_DIR, name + ".png"), roughness=0.55, masked=True)
    b = B(name)
    f = [(0.0, 0.0), (-BEAM_D, 0.075), (-0.02, 0.155), (-BEAM_D, 0.235), (0.0, BEAM_H)]
    z_lo, z_hi = 0.40, 0.85 + BEAM_H
    x0, x1 = -1.97, 1.97
    for z0 in (0.40, 0.85):
        for (dy0, dz0), (dy1, dz1) in zip(f[:-1], f[1:]):
            ya, za = BEAM_Y + dy0 - 0.004, z0 + dz0
            yb, zb = BEAM_Y + dy1 - 0.004, z0 + dz1
            va, vb = (za - z_lo) / (z_hi - z_lo), (zb - z_lo) / (z_hi - z_lo)
            b.quad_uv(mat, [(x0, ya, za), (x1, ya, za), (x1, yb, zb), (x0, yb, zb)],
                      [(0, va), (1, va), (1, vb), (0, vb)])
    return b.finish(recalc=False)


def track_bridge_10m():
    """A 10 m module of a bridge carrying the lap: on each side a concrete
    parapet with a steel railing, its fascia beam dropping below the deck.
    Authored for a 15 m road (`span_m / 15` scales local Y), the parapets
    13.6-14.2 m out, so on the Nordschleife's 9 m they stand just behind
    the rail."""
    b = B("track_bridge_10m")
    conc, dark, steel = M("bridge_concrete"), M("bridge_dark"), M("bridge_steel")
    x0, x1 = -5.0, 5.0
    for side in (-1, 1):
        yi, yo, co = side * 13.6, side * 14.2, side * 14.35
        b.box(conc, (x0, min(yi, yo), -1.6), (x1, max(yi, yo), 0.95))
        b.box(conc, (x0, min(yo, co), 0.75), (x1, max(yo, co), 0.95))
        b.box(dark, (x0, min(yo, co), 0.70), (x1, max(yo, co), 0.75))
        yr = side * 13.9
        for i in range(5):
            x = x0 + 1.0 + i * 2.0
            b.box(steel, (x - 0.03, yr - 0.03, 0.95), (x + 0.03, yr + 0.03, 1.55))
        for z in (1.25, 1.52):
            b.box(steel, (x0, yr - 0.025, z), (x1, yr + 0.025, z + 0.05))
    b.box(dark, (x0, -13.6, -1.6), (x1, 13.6, -1.45))
    return b.finish()


def overpass_concrete():
    """A forest track's concrete beam bridge over the lap: a 5 m deck 5.4 m
    over the road with railings, on abutments 14-16.5 m out (for a 15 m
    road; scaled like every bridge), with wing walls."""
    b = B("overpass_concrete")
    conc, dark, steel = M("bridge_concrete"), M("bridge_dark"), M("bridge_steel")
    dw = 2.5
    deck_lo, deck_hi = 5.4, 6.3
    b.box(conc, (-dw, -16.5, deck_lo), (dw, 16.5, deck_hi))
    b.box(dark, (-dw + 0.3, -14.0, deck_lo - 0.05), (dw - 0.3, 14.0, deck_lo))
    for side in (-1, 1):
        lo_y, hi_y = min(side * 14.0, side * 16.5), max(side * 14.0, side * 16.5)
        b.box(conc, (-dw - 0.6, lo_y, -2.0), (dw + 0.6, hi_y, deck_lo))
        b.box(conc, (-dw - 3.6, lo_y, -2.0), (-dw - 0.6, hi_y, deck_lo - 1.5))
        b.box(conc, (dw + 0.6, lo_y, -2.0), (dw + 3.6, hi_y, deck_lo - 1.5))
    for x in (-dw + 0.05, dw - 0.05):
        for i in range(17):
            y = -16.0 + i * 2.0
            b.box(steel, (x - 0.03, y - 0.03, deck_hi), (x + 0.03, y + 0.03, deck_hi + 1.1))
        for z in (deck_hi + 0.55, deck_hi + 1.05):
            b.box(steel, (x - 0.025, -16.5, z), (x + 0.025, 16.5, z + 0.05))
    return b.finish()


def toll_gate_lane():
    """A toll lane at the Touristenfahrten entrance: a booth, a pedestal and
    a red and white lift arm along +X, down. Centred on its 5.4 m footprint."""
    b = B("toll_gate_lane")
    white = M("toll_white", (0.9, 0.9, 0.88), roughness=0.5)
    red = M("toll_red", (0.75, 0.08, 0.06), roughness=0.5)
    glass = M("toll_glass", (0.18, 0.22, 0.27), roughness=0.1)
    dark = M("bridge_dark")
    b.box(white, (-2.7, -0.7, 0.0), (-1.3, 0.7, 1.1))
    b.box(glass, (-2.7, -0.7, 1.1), (-1.3, 0.7, 2.2))
    b.box(white, (-2.7, -0.7, 2.2), (-1.3, 0.7, 2.4))
    b.box(dark, (-2.8, -0.8, 2.4), (-1.2, 0.8, 2.6))
    b.box(white, (-1.0, -0.2, 0.0), (-0.6, 0.2, 1.05))
    b.box(dark, (-0.95, -0.15, 1.05), (-0.65, 0.15, 1.15))
    n = 7
    for i in range(n):
        xa = -0.7 + i * 3.4 / n
        b.box(red if i % 2 == 0 else white, (xa, -0.05, 0.98), (xa + 3.4 / n, 0.05, 1.08))
    return b.finish()


def stone_lookout_tower():
    """The Kaiser-Wilhelm-Turm on the Hohe Acht (1909, 16.3 m): a square
    stone tower with a battlemented viewing top and a stair turret on one
    corner. Centred; 4 m of plinth below the pivot for the summit's slope."""
    b = B("stone_lookout_tower")
    stone, dark = M("house_stone"), M("castle_basalt", (0.22, 0.21, 0.2), roughness=0.9)
    base, w = -4.0, 2.6
    b.box(stone, (-w, -w, base), (w, w, 14.6))
    b.box(stone, (-w - 0.25, -w - 0.25, 14.6), (w + 0.25, w + 0.25, 15.1))
    for i in range(5):
        t = -w - 0.25 + i * (2 * w + 0.5 - 0.55) / 4
        for a0, a1, b0, b1 in ((t, t + 0.55, -w - 0.25, -w + 0.15), (t, t + 0.55, w - 0.15, w + 0.25),
                               (-w - 0.25, -w + 0.15, t, t + 0.55), (w - 0.15, w + 0.25, t, t + 0.55)):
            b.box(stone, (a0, b0, 15.1), (a1, b1, 16.3))
    b.cylinder(stone, (w, w, base), 1.1, 16.0 - base, segs=12)
    b.cylinder(dark, (w, w, 16.0), 1.2, 0.3, segs=12)
    b.box(dark, (-0.6, -w - 0.02, 0.0), (0.6, -w + 0.1, 2.4))
    for z in (5.0, 9.0, 12.5):
        for x in (-1.2, 1.2):
            b.box(dark, (x - 0.25, -w - 0.02, z), (x + 0.25, -w + 0.1, z + 1.1))
    return b.finish()


def bank_grass():
    """The verge's own mown turf (apex_tex.turf, the ground set's grass),
    taken from its mean-0.5 form to a grass albedo near what the terrain
    draws, tiled over 2 m."""
    import numpy as np
    color, rough, height, ns = tex.turf()
    # sRGB values (to_image writes them as they are): about 0.06, 0.11,
    # 0.02 linear, a third of the flat green that read as too bright.
    albedo = np.clip(color * 2.0 * np.array([0.26, 0.35, 0.14]), 0, 1)
    return tex.pbr_material("bank_grass", (albedo, rough, height, ns), tile_m=2.0)


def spectator_bank_10m():
    """An earth bank where the crowd stands: a low, rounded mound of turf
    10 m long, 5 m across and 0.6 m high, its ends easing down so a row of
    them reads as one bank, with a low hedge of the kit's own bush cards
    along the crest. Symmetric about its long axis, so it reads the same
    whichever way it is turned; skirts run 0.8 m below the pivot to meet a
    sloping verge."""
    b = B("spectator_bank_10m")
    grass = bank_grass()
    half, crest = 2.5, 0.6
    ys = [half * (2 * j / 12 - 1) for j in range(13)]
    xs = [-5.0 + 0.5 * i for i in range(21)]

    def z(x, y):
        across = 0.5 + 0.5 * math.cos(math.pi * y / half)
        end = min(1.0, (5.0 - abs(x)) / 1.4)
        end = 0.35 + 0.65 * end * end * (3 - 2 * end)
        return crest * across * end

    s = b.slot(grass)
    grid_v = [[b.bm.verts.new((x, y, z(x, y))) for y in ys] for x in xs]
    for i in range(len(xs) - 1):
        for j in range(len(ys) - 1):
            f = b.bm.faces.new((grid_v[i][j], grid_v[i + 1][j], grid_v[i + 1][j + 1], grid_v[i][j + 1]))
            f.material_index = s
    # Skirts down into the ground on the four sides.
    def skirt(edge, out):
        low = [b.bm.verts.new((v.co.x + out[0], v.co.y + out[1], -0.8)) for v in edge]
        for k in range(len(edge) - 1):
            f = b.bm.faces.new((edge[k], edge[k + 1], low[k + 1], low[k]))
            f.material_index = s
    skirt([row[0] for row in grid_v], (0.0, -0.3))
    skirt([row[-1] for row in grid_v], (0.0, 0.3))
    skirt(grid_v[0], (-0.3, 0.0))
    skirt(grid_v[-1], (0.3, 0.0))
    # The hedge: bush clusters at a third of their size along the crest.
    # One import, five copies: importing it again would bring its
    # materials in again as `.001` slots.
    from mathutils import Matrix
    new, meshes = import_glb("tree", "bush_cluster")
    for k, x in enumerate((-4.0, -2.0, 0.0, 2.0, 4.0)):
        sc = 0.30 + 0.04 * ((k * 7) % 3) / 2
        m = Matrix.Translation((x + 0.15 * ((k * 5) % 3 - 1), 0.0, crest * 0.85)) @ Matrix.Rotation(k * 1.3, 4, 'Z') @ Matrix.Scale(sc, 4)
        absorb(b, meshes, m)
    for ob in new:
        bpy.data.objects.remove(ob)
    return b.finish()


def camp_scaffold_tower():
    """A camp's home-built viewing tower: scaffold tubes, plank decks at
    2.2 m and 4.4 m, a tarpaulin over the top and a banner. 3 x 2.4 m, centred."""
    b = B("camp_scaffold_tower")
    tube, plank = M("armco_galv"), M("scaffold_plank")
    tarp = M("camp_tarp", (0.12, 0.30, 0.62), roughness=0.8)
    banner = M("camp_banner", (0.9, 0.85, 0.2), roughness=0.8)
    xs, ys = (-1.5, 1.5), (-1.2, 1.2)
    for x in xs:
        for y in ys:
            b.cylinder(tube, (x, y, 0.0), 0.024, 6.4, segs=6)
    for z in (2.2, 4.4):
        b.box(plank, (-1.55, -1.25, z), (1.55, 1.25, z + 0.06))
        for y in ys:
            b.cylinder(tube, (-1.5, y, z + 1.0), 0.02, 3.0, segs=6, axis='X')
        for x in xs:
            b.cylinder(tube, (x, -1.2, z + 1.0), 0.02, 2.4, segs=6, axis='Y')
    for y in ys:
        b.bar(tube, (-1.5, y, 0.1), (1.5, y, 2.2), 0.02)
        b.bar(tube, (1.5, y, 2.3), (-1.5, y, 4.4), 0.02)
    b.box(tarp, (-1.6, -1.3, 6.3), (1.6, 1.3, 6.36))
    b.box(tarp, (-1.6, -0.05, 6.36), (1.6, 0.05, 6.7))
    b.box(banner, (-1.4, -1.24, 4.55), (1.4, -1.22, 5.35))
    return b.finish()


def camp_gazebo():
    """A 3 x 3 m pop-up gazebo: four legs, a peaked canvas roof, a valance."""
    b = B("camp_gazebo")
    leg = M("armco_galv")
    canvas = M("camp_canvas", (0.85, 0.85, 0.82), roughness=0.85)
    for x in (-1.5, 1.5):
        for y in (-1.5, 1.5):
            b.cylinder(leg, (x, y, 0.0), 0.025, 2.2, segs=6)
    corners = [(-1.55, -1.55), (1.55, -1.55), (1.55, 1.55), (-1.55, 1.55)]
    top = b.bm.verts.new((0.0, 0.0, 2.95))
    s = b.slot(canvas)
    for (ax, ay), (bx, by) in zip(corners, corners[1:] + corners[:1]):
        va = b.bm.verts.new((ax, ay, 2.2))
        vb = b.bm.verts.new((bx, by, 2.2))
        b.bm.faces.new((va, vb, top)).material_index = s
        b.box(canvas, (min(ax, bx) - 0.01, min(ay, by) - 0.01, 1.95), (max(ax, bx) + 0.01, max(ay, by) + 0.01, 2.2))
    return b.finish()


def camp_flag_pole():
    """A fan's flag on a 6 m pole; the cloth is the `flag_cloth` slot, so the
    flag code is the prop's `text` (T_flag_<cc>)."""
    b = B("camp_flag_pole")
    pole = M("armco_galv")
    cloth = borrow_material("sign", "flag_pole", "flag_cloth") or M("flag_cloth", (0.8, 0.8, 0.8))
    b.cylinder(pole, (0.0, 0.0, 0.0), 0.03, 6.0, segs=8)
    b.quad_uv(cloth, [(0.05, 0.0, 4.4), (1.85, 0.0, 4.4), (1.85, 0.0, 5.6), (0.05, 0.0, 5.6)],
              [(0, 0), (1, 0), (1, 1), (0, 1)])
    b.quad_uv(cloth, [(1.85, 0.003, 4.4), (0.05, 0.003, 4.4), (0.05, 0.003, 5.6), (1.85, 0.003, 5.6)],
              [(1, 0), (0, 0), (0, 1), (1, 1)])
    return b.finish(recalc=False)


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
    "marshal_post_numbered": marshal_post_numbered,
    "de_descent": lambda: hill_sign("de_descent", True),
    "de_climb": lambda: hill_sign("de_climb", False),
    "de_double_curve_left": lambda: triangle("de_double_curve_left", TRIANGLES["de_double_curve_left"]),
    "de_double_curve_right": lambda: triangle("de_double_curve_right", TRIANGLES["de_double_curve_right"]),
    **{f"rail_tag_{n}": (lambda n=n: rail_tag(f"rail_tag_{n}"))
       for n in ("kalle", "eifel", "vollgas", "hup", "moppel", "hearts")},
    "track_bridge_10m": track_bridge_10m,
    "overpass_concrete": overpass_concrete,
    "toll_gate_lane": toll_gate_lane,
    "stone_lookout_tower": stone_lookout_tower,
    "spectator_bank_10m": spectator_bank_10m,
    "camp_scaffold_tower": camp_scaffold_tower,
    "camp_gazebo": camp_gazebo,
    "camp_flag_pole": camp_flag_pole,
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
    wanted = {w.strip() for w in which.split(",")}
    for kind, asset in ASSETS:
        if "all" not in wanted and asset not in wanted:
            continue
        ob = BUILD[asset]()
        # Spread them out in the saved scene; export puts each at the origin.
        ob.location = (len(done) * 12.0, 0, 0)
        done.append((kind, asset, apex.export_one(kind, asset)))
    apex.save_blend("_batches", "nordschleife_kit")
    for kind, asset, (path, size) in done:
        print(f"  {kind}/{asset}.glb  {size // 1024} KiB")
    return done


if bpy is None:
    paint_textures()
else:
    run(ASSET)
