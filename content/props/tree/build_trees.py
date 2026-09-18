"""Build the trackside tree/foliage kit and export to content/props/tree/<asset>.glb.

    ASSET = "broadleaf_m"      # any key in ASSETS, or "all"
    exec(open(r"D:\apexsim\content\props\tree\build_trees.py").read())

v2: on top of the v1 shape work (blob-cluster broadleaf, tiered/drooping
conifers, tapered/drooping palms) this adds the two things a shape pass alone
can't fix - actual surface material and organic irregularity:

* every canopy blob and trunk ring gets a Perlin bump baked straight into its
  vertex positions (radial on blobs, per-ring on trunks/branches), so nothing
  is a bare primitive anymore - no perfect sphere, no perfect cylinder.
* every material slot (tree_bark, tree_foliage_a-c, tree_conifer_a-b,
  tree_autumn_a-c) gets a procedurally generated, tileable base-colour +
  roughness + normal map instead of one flat Principled BSDF colour - bark
  gets vertical ridge-and-groove detail, foliage/conifer/autumn get mottled
  leaf-cluster blotching plus fine speckle, all baked with numpy directly in
  Blender (no textures fetched or hand-painted) and saved alongside the GLBs
  under content/props/tree/textures/ so they're inspectable and regenerable.

Still the same "solid, no alpha" contract - textures only drive colour,
roughness and normal, never alpha - and the same material-slot names the
importer and existing tracks already reference.
"""
import bpy, bmesh, math, os, random, importlib.util, sys
import numpy as np
from mathutils import Vector, Matrix, noise as mnoise

_ROOT = r"D:\apexsim"
_s = importlib.util.spec_from_file_location(
    "apex", os.path.join(_ROOT, r"content\props\_tools\apex_props.py"))
apex = importlib.util.module_from_spec(_s)
sys.modules["apex"] = apex
_s.loader.exec_module(apex)

OUT_DIR = os.path.join(_ROOT, r"content\props\tree")
TEX_DIR = os.path.join(OUT_DIR, "textures")
os.makedirs(TEX_DIR, exist_ok=True)

BARK_RGB = (0.20, 0.14, 0.09)
GREEN = [(0.10, 0.30, 0.06), (0.14, 0.36, 0.09), (0.07, 0.24, 0.05)]
AUTUMN = [(0.75, 0.32, 0.05), (0.85, 0.58, 0.08), (0.55, 0.14, 0.05)]
CONIFER = [(0.06, 0.20, 0.08), (0.08, 0.25, 0.10)]

TEX_SIZE = 256
UV_SCALE = 0.9   # texture tiles per metre, box-mapped


# ------------------------------------------------------------- pixel helpers
def _gauss_blur(a, sigma):
    """Seamless Gaussian blur via FFT (inherently periodic, so this tiles
    perfectly with no grid or seam artifact regardless of blob size)."""
    if sigma <= 0.01:
        return a
    size = a.shape[0]
    f = np.fft.fftfreq(size)
    fx, fy = np.meshgrid(f, f)
    kernel = np.exp(-2.0 * (np.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * kernel))


def _fbm(size, seed, scales=(2, 4, 8, 16, 32), weights=(0.45, 0.25, 0.16, 0.09, 0.05)):
    """Multi-octave tileable noise in 0..1: full-resolution white noise
    Gaussian-blurred to each target feature scale (`scales[i]` ~ how many
    blotches span the image), then combined. FFT blur means no upsample-grid
    artifact, unlike a kron-then-box-blur approach."""
    rng = np.random.default_rng(seed)
    total = np.zeros((size, size), dtype=np.float64)
    for sc, w in zip(scales, weights):
        n = rng.random((size, size))
        sigma = size / (2.2 * sc)
        n = _gauss_blur(n, sigma)
        total += w * n
    total -= total.min()
    if total.max() > 1e-9:
        total /= total.max()
    return total


def _normal_from_height(h, strength=2.2):
    """Tangent-space normal map (0..1 RGB) from a heightfield, wrap-around
    central differences so it stays seamless."""
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(h)
    length = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
    nx, ny, nz = nx / length, ny / length, nz / length
    rgb = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5], axis=-1)
    return rgb


def _to_image(name, rgb01):
    """rgb01: HxWx3 float array in 0..1 -> a saved+packed bpy.data.images entry."""
    size = rgb01.shape[0]
    img = bpy.data.images.get(name)
    if img is None:
        img = bpy.data.images.new(name, width=size, height=size, alpha=False)
    pix = np.ones((size, size, 4), dtype=np.float32)
    pix[:, :, 0:3] = np.clip(rgb01, 0.0, 1.0)
    img.pixels.foreach_set(pix.ravel())
    path = os.path.join(TEX_DIR, name + ".png")
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    img.pack()
    return img


def _leaf_color_and_bump(base_rgb, size, seed, warm=False):
    """Mottled leaf-cluster colour: low-freq blotches (different leaves
    catching light differently) + high-freq speckle (leaf edges/shadow gaps),
    all colour - never alpha. Returns (color 0..1 HxWx3, height 0..1 HxW)."""
    blotch = _fbm(size, seed, scales=(3, 6, 12), weights=(0.5, 0.3, 0.2))
    speckle = _fbm(size, seed + 101, scales=(24, 48), weights=(0.6, 0.4))
    height = np.clip(blotch * 0.7 + speckle * 0.3, 0.0, 1.0)
    base = np.array(base_rgb).reshape(1, 1, 3)
    shade = 0.62 + 0.65 * blotch[..., None]
    speck_dark = 1.0 - 0.30 * (speckle[..., None] ** 2)
    color = base * shade * speck_dark
    if warm:
        # a second warm tone bleeds through the blotches (real autumn
        # canopies are patchy, not one flat colour)
        alt = np.array(base_rgb).reshape(1, 1, 3) * np.array([1.15, 0.85, 0.85])
        mix = np.clip((blotch - 0.55) * 2.2, 0.0, 1.0)[..., None]
        color = color * (1 - mix) + alt * shade * mix
    return np.clip(color, 0.0, 1.0), height


def make_organic_material(name, base_rgb, seed=0, kind="foliage"):
    """Bark, foliage, conifer and autumn all share this: a base-colour map,
    a roughness map and a normal map, baked with numpy and wired into the
    Principled BSDF. Never touches Alpha - stays solid per docs/PROPS.md."""
    size = TEX_SIZE
    if kind == "bark":
        x = np.linspace(0, 2 * math.pi * 5, size, endpoint=False)
        y = np.linspace(0, 1, size, endpoint=False)
        X, Y = np.meshgrid(x, y)
        warp = _fbm(size, seed + 7, scales=(4, 8), weights=(0.6, 0.4))
        ridges = 0.5 + 0.5 * np.sin(X + 3.0 * (warp - 0.5))
        fine = _fbm(size, seed, scales=(16, 32, 64), weights=(0.5, 0.3, 0.2))
        height = np.clip(ridges * 0.65 + fine * 0.35, 0.0, 1.0)
        base = np.array(base_rgb).reshape(1, 1, 3)
        shade = 0.55 + 0.75 * height[..., None]
        blotch = _fbm(size, seed + 55, scales=(3, 6), weights=(0.6, 0.4))
        color = base * shade * (0.85 + 0.3 * blotch[..., None])
        rough = np.clip(0.72 + 0.22 * fine - 0.10 * ridges, 0.55, 0.98)
        normal_strength = 2.6
    else:
        warm = (kind == "autumn")
        color, height = _leaf_color_and_bump(base_rgb, size, seed, warm=warm)
        rough = np.clip(0.55 + 0.35 * height - 0.10 * (kind == "conifer"), 0.35, 0.95)
        normal_strength = 1.6 if kind != "conifer" else 2.0

    color_img = _to_image(name + "_col", np.clip(color, 0, 1))
    rough_img = _to_image(name + "_rough", np.stack([rough] * 3, axis=-1))
    rough_img.colorspace_settings.name = 'Non-Color'
    normal_img = _to_image(name + "_nrm", _normal_from_height(height, strength=normal_strength))
    normal_img.colorspace_settings.name = 'Non-Color'

    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL'):
            nt.nodes.remove(n)
    bsdf = nt.nodes["Principled BSDF"]

    tex_c = nt.nodes.new("ShaderNodeTexImage")
    tex_c.image = color_img
    nt.links.new(tex_c.outputs["Color"], bsdf.inputs["Base Color"])

    tex_r = nt.nodes.new("ShaderNodeTexImage")
    tex_r.image = rough_img
    nt.links.new(tex_r.outputs["Color"], bsdf.inputs["Roughness"])

    tex_n = nt.nodes.new("ShaderNodeTexImage")
    tex_n.image = normal_img
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(tex_n.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return m


# ---------------------------------------------------------------------- kit
class TB:
    """Tiny tree builder: one bmesh, named material slots, box-mapped UVs,
    per-vertex organic noise, done."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.mats = []
        self._slot = {}

    def slot(self, mat):
        if mat not in self._slot:
            self._slot[mat] = len(self.mats)
            self.mats.append(mat)
        return self._slot[mat]

    def blob(self, mat, center, radius, scale=(1.0, 1.0, 1.0), subdiv=1,
             rot=(0.0, 0.0, 0.0), smooth=True, bump=0.16, seed=0):
        """A subdivided, anisotropically-scaled icosphere, radially perturbed
        by Perlin noise so it reads as an organic lump, not a bare
        icosahedron or a smooth billiard ball."""
        idx = self.slot(mat)
        c = Vector(center)
        M = (Matrix.Translation(c)
             @ Matrix.Rotation(rot[2], 4, 'Z')
             @ Matrix.Rotation(rot[1], 4, 'Y')
             @ Matrix.Rotation(rot[0], 4, 'X')
             @ Matrix.Diagonal((scale[0] * radius, scale[1] * radius,
                                 scale[2] * radius, 1.0)))
        before_f = len(self.bm.faces)
        before_v = len(self.bm.verts)
        bmesh.ops.create_icosphere(self.bm, subdivisions=subdiv, radius=1.0,
                                    matrix=M, calc_uvs=False)
        for v in self.bm.verts[before_v:]:
            d = v.co - c
            if d.length > 1e-6:
                nrm = d.normalized()
                n = mnoise.noise(v.co * (2.2 / max(radius, 0.05)) + Vector((seed, seed * 1.7, seed * 0.6)))
                v.co += nrm * (n * bump * radius)
        for f in self.bm.faces[before_f:]:
            f.material_index = idx
            f.smooth = smooth
        return self.bm.faces[before_f:]

    def taper(self, mat, p0, p1, r0, r1, segs=8, smooth=False, bark_bump=0.07, seed=0):
        """A tapered trunk/branch segment from p0 (radius r0) to p1 (radius
        r1), each ring nudged in/out by noise so the trunk isn't a perfect
        cylinder - real bark bulges and furrows along its length."""
        idx = self.slot(mat)
        p0, p1 = Vector(p0), Vector(p1)
        axis = p1 - p0
        L = axis.length
        up = axis.normalized() if L > 1e-6 else Vector((0, 0, 1))
        ref = Vector((0, 0, 1)) if abs(up.z) < 0.9 else Vector((1, 0, 0))
        rgt = up.cross(ref).normalized()
        fwd = rgt.cross(up).normalized()
        ring0, ring1 = [], []
        for i in range(segs):
            a = 2 * math.pi * i / segs
            off = rgt * math.cos(a) + fwd * math.sin(a)
            p_a, p_b = p0 + off * r0, p1 + off * r1
            if bark_bump:
                n_a = mnoise.noise(p_a * 6.0 + Vector((seed, seed * 2, 0)))
                n_b = mnoise.noise(p_b * 6.0 + Vector((seed, seed * 2, 0)))
                p_a += off * (n_a * bark_bump * max(r0, 0.02))
                p_b += off * (n_b * bark_bump * max(r1, 0.02))
            ring0.append(self.bm.verts.new(p_a))
            ring1.append(self.bm.verts.new(p_b))
        for i in range(segs):
            j = (i + 1) % segs
            f = self.bm.faces.new((ring0[i], ring0[j], ring1[j], ring1[i]))
            f.material_index = idx
            f.smooth = smooth
        return ring0, ring1

    def cap(self, mat, ring, apex_pt, smooth=False):
        """Close a ring with a fan up to a single apex point."""
        idx = self.slot(mat)
        av = self.bm.verts.new(apex_pt)
        n = len(ring)
        for i in range(n):
            f = self.bm.faces.new((ring[i], ring[(i + 1) % n], av))
            f.material_index = idx
            f.smooth = smooth

    def cone_tier(self, mat, apex_pt, base_z, base_center_xy, base_r, segs=10,
                  droop=0.10, notch=0.16, seed=0, smooth=True):
        """One conifer tier: apex up top, a scalloped, drooping base rim
        (branch tips sagging down and out) instead of a flat circular cut."""
        idx = self.slot(mat)
        rnd = random.Random(seed)
        apex_v = self.bm.verts.new(apex_pt)
        ring = []
        for i in range(segs):
            a = 2 * math.pi * i / segs + rnd.uniform(-0.05, 0.05)
            r = base_r * (1.0 + notch * math.sin(a * 3.0 + seed) * 0.5
                           + notch * rnd.uniform(-0.35, 0.35))
            sag = droop * (0.55 + 0.45 * math.sin(a * 5.0 + seed * 1.7) ** 2
                            + 0.3 * rnd.random())
            x = base_center_xy[0] + math.cos(a) * r
            y = base_center_xy[1] + math.sin(a) * r
            z = base_z - sag
            ring.append(self.bm.verts.new((x, y, z)))
        for i in range(segs):
            f = self.bm.faces.new((apex_v, ring[i], ring[(i + 1) % segs]))
            f.material_index = idx
            f.smooth = smooth
        return ring

    def frond(self, mat, root, tip, width0, width1, droop, side_tilt=0.0,
              segs=6, smooth=True):
        """A tapering, drooping palm-frond blade: a thin curved strip rather
        than one flat triangular spike."""
        idx = self.slot(mat)
        root, tip = Vector(root), Vector(tip)
        span = tip - root
        L = span.length
        fwd = span.normalized() if L > 1e-6 else Vector((1, 0, 0))
        upz = Vector((0, 0, 1))
        side = fwd.cross(upz)
        if side.length < 1e-6:
            side = Vector((0, 1, 0))
        side.normalize()
        rows = []
        for i in range(segs + 1):
            t = i / segs
            w = width0 * (1 - t) + width1 * t
            sag = droop * (t ** 1.6)
            lateral = side_tilt * t
            centre = root + span * t - upz * sag + side * lateral
            rows.append((centre, side * (w * 0.5)))
        verts = [(self.bm.verts.new(c - h), self.bm.verts.new(c + h)) for c, h in rows]
        for i in range(segs):
            a0, b0 = verts[i]
            a1, b1 = verts[i + 1]
            f = self.bm.faces.new((a0, b0, b1, a1))
            f.material_index = idx
            f.smooth = smooth
        return verts

    def _box_uv(self, scale=UV_SCALE):
        for f in self.bm.faces:
            n = f.normal
            ax, ay, az = abs(n.x), abs(n.y), abs(n.z)
            for loop in f.loops:
                co = loop.vert.co
                if az >= ax and az >= ay:
                    u, v = co.x, co.y
                elif ay >= ax:
                    u, v = co.x, co.z
                else:
                    u, v = co.y, co.z
                loop[self.uv].uv = (u * scale, v * scale)

    def finish(self):
        self._box_uv()
        mesh = bpy.data.meshes.new(self.name)
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=1e-6)
        self.bm.normal_update()
        self.bm.to_mesh(mesh)
        self.bm.free()
        for m in self.mats:
            mesh.materials.append(m)
        ob = bpy.data.objects.new(self.name, mesh)
        bpy.context.scene.collection.objects.link(ob)
        return ob


def export_glb(ob, path):
    for o in bpy.data.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    win = bpy.context.window_manager.windows[0]
    with bpy.context.temp_override(window=win, screen=win.screen,
                                    area=win.screen.areas[0] if win.screen.areas else None):
        bpy.ops.export_scene.gltf(filepath=path, use_selection=True,
                                   export_apply=True, export_yup=True,
                                   export_materials='EXPORT')
    return path


def mats_for(rgb_list, prefix, suffix=""):
    return [apex.material("%s_%s%s" % (prefix, k, suffix), c, roughness=0.9)
            for k, c in zip("abc"[:len(rgb_list)], rgb_list)]


result_log = []


def base_materials():
    bark = make_organic_material("tree_bark", BARK_RGB, seed=1, kind="bark")
    foliage = [make_organic_material("tree_foliage_%s" % k, c, seed=10 + i, kind="foliage")
               for i, (k, c) in enumerate(zip("abc", GREEN))]
    conifer = [make_organic_material("tree_conifer_%s" % k, c, seed=20 + i, kind="conifer")
               for i, (k, c) in enumerate(zip("ab", CONIFER))]
    autumn = [make_organic_material("tree_autumn_%s" % k, c, seed=30 + i, kind="autumn")
              for i, (k, c) in enumerate(zip("abc", AUTUMN))]
    return bark, foliage, conifer, autumn


# --------------------------------------------------------------- per-asset builds
def build_broadleaf(name, height, width, bark_mat, foliage_mats, seed=0):
    rnd = random.Random(seed)
    tb = TB(name)
    trunk_h = height * rnd.uniform(0.38, 0.46)
    top_r = width * 0.10
    base_r = width * 0.045
    segs_n = 3
    pts = [Vector((0.0, 0.0, 0.0))]
    for i in range(1, segs_n + 1):
        t = i / segs_n
        jitter = Vector((rnd.uniform(-0.04, 0.04), rnd.uniform(-0.04, 0.04), 0.0)) * width
        pts.append(Vector((0.0, 0.0, trunk_h * t)) + jitter * t)
    radii = [base_r + (top_r - base_r) * (i / segs_n) for i in range(segs_n + 1)]
    for i in range(segs_n):
        tb.taper(bark_mat, pts[i], pts[i + 1], radii[i], radii[i + 1], segs=7, smooth=True)
    canopy_h = height - trunk_h
    canopy_center = pts[-1] + Vector((0.0, 0.0, canopy_h * 0.30))
    n_hero = 3 if height < 8 else (4 if height < 13 else 5)
    n_fill = 4 if height < 8 else (6 if height < 13 else 8)
    for i in range(n_hero):
        a = rnd.uniform(0, 2 * math.pi)
        r_xy = rnd.uniform(0.0, width * 0.26)
        r_z = rnd.uniform(-canopy_h * 0.22, canopy_h * 0.38)
        c = canopy_center + Vector((math.cos(a) * r_xy, math.sin(a) * r_xy, r_z))
        rad = rnd.uniform(canopy_h * 0.34, canopy_h * 0.48)
        sc = (rnd.uniform(0.9, 1.2), rnd.uniform(0.9, 1.2), rnd.uniform(0.8, 1.05))
        mat = foliage_mats[i % len(foliage_mats)]
        tb.blob(mat, c, rad, scale=sc, subdiv=1,
                rot=(rnd.uniform(0, math.pi), rnd.uniform(0, math.pi), rnd.uniform(0, math.pi)))
    for i in range(n_fill):
        a = rnd.uniform(0, 2 * math.pi)
        r_xy = rnd.uniform(width * 0.18, width * 0.38)
        r_z = rnd.uniform(-canopy_h * 0.32, canopy_h * 0.46)
        c = canopy_center + Vector((math.cos(a) * r_xy, math.sin(a) * r_xy, r_z))
        rad = rnd.uniform(canopy_h * 0.16, canopy_h * 0.26)
        sc = (rnd.uniform(0.85, 1.2), rnd.uniform(0.85, 1.2), rnd.uniform(0.8, 1.1))
        mat = foliage_mats[i % len(foliage_mats)]
        tb.blob(mat, c, rad, scale=sc, subdiv=0,
                rot=(rnd.uniform(0, math.pi), rnd.uniform(0, math.pi), rnd.uniform(0, math.pi)))
    tb.blob(foliage_mats[0], pts[-1] + Vector((0.0, 0.0, canopy_h * 0.05)),
            canopy_h * 0.24, scale=(1.1, 1.1, 0.7), subdiv=0)
    return tb.finish()


def build_conifer(name, height, width, bark_mat, conifer_mats, seed=0):
    rnd = random.Random(seed)
    tb = TB(name)
    n_tiers = 9
    trunk_top = height * 0.96
    tb.taper(bark_mat, (0, 0, 0), (0, 0, trunk_top), width * 0.045, width * 0.014, segs=8, smooth=True)
    tier_h = (trunk_top - height * 0.04) / (n_tiers - 1) if n_tiers > 1 else trunk_top
    for i in range(n_tiers):
        frac = i / (n_tiers - 1)
        base_z = height * 0.04 + frac * (trunk_top - height * 0.04)
        apex_z = min(base_z + tier_h * 1.55, height * 0.998)
        r = width * 0.52 * (1.0 - frac * 0.86) + width * 0.03
        tb.cone_tier(conifer_mats[i % 2], (0.0, 0.0, apex_z), base_z, (0.0, 0.0), r,
                     segs=9, droop=tier_h * 0.55, notch=0.34, seed=seed * 13 + i, smooth=False)
    return tb.finish()


def build_poplar(name, height, width, bark_mat, foliage_mats, seed=0):
    rnd = random.Random(seed)
    tb = TB(name)
    trunk_h = height * 0.32
    tb.taper(bark_mat, (0, 0, 0), (0, 0, trunk_h), width * 0.05, width * 0.035, segs=7, smooth=True)
    tb.taper(bark_mat, (0, 0, trunk_h), (0, 0, trunk_h * 1.15), width * 0.035, width * 0.03, segs=7, smooth=True)
    canopy_h = height - trunk_h
    n = 8
    for i in range(n):
        t = i / (n - 1)
        z = trunk_h * 0.85 + canopy_h * t * 0.92
        wobble = rnd.uniform(-width * 0.10, width * 0.10)
        rad = width * (0.30 - 0.10 * abs(t - 0.5)) * rnd.uniform(0.85, 1.05)
        c = (wobble, rnd.uniform(-width * 0.06, width * 0.06), z)
        sc = (1.0, 1.0, rnd.uniform(1.5, 2.0))
        mat = foliage_mats[i % len(foliage_mats)]
        tb.blob(mat, c, rad, scale=sc, subdiv=1, rot=(0.0, 0.0, rnd.uniform(0, math.pi)))
    return tb.finish()


def build_bush(name, foliage_mats, seed=0):
    rnd = random.Random(seed)
    tb = TB(name)
    width, height = 4.32, 2.41
    n = 6
    for i in range(n):
        a = rnd.uniform(0, 2 * math.pi)
        r_xy = rnd.uniform(0.0, width * 0.28)
        c = (math.cos(a) * r_xy, math.sin(a) * r_xy, rnd.uniform(height * 0.25, height * 0.55))
        rad = rnd.uniform(height * 0.35, height * 0.55)
        sc = (rnd.uniform(0.9, 1.2), rnd.uniform(0.9, 1.2), rnd.uniform(0.7, 0.9))
        rot = (rnd.uniform(0, math.pi), rnd.uniform(0, math.pi), rnd.uniform(0, math.pi))
        tb.blob(foliage_mats[i % len(foliage_mats)], c, rad, scale=sc, subdiv=1, rot=rot)
    return tb.finish()


def build_palm(name, kind, bark_mat, foliage_mats, seed=0):
    rnd = random.Random(seed)
    tb = TB(name)
    if kind == "ornamental":
        height, width = 13.15, 3.77
        trunk_top_frac = 0.90
        n_fronds = 9
        frond_len = width * 1.35
        droop = frond_len * 0.55
    else:
        height, width = 8.35, 4.37
        trunk_top_frac = 0.82
        n_fronds = 13
        frond_len = width * 1.05
        droop = frond_len * 0.30
    trunk_top = height * trunk_top_frac
    n_seg = 5
    lean = Vector((rnd.uniform(-0.3, 0.3), rnd.uniform(-0.3, 0.3), 0.0))
    pts = [Vector((0.0, 0.0, 0.0))]
    for i in range(1, n_seg + 1):
        t = i / n_seg
        pts.append(Vector((0.0, 0.0, trunk_top * t)) + lean * (t ** 1.5))
    for i in range(n_seg):
        t0, t1 = i / n_seg, (i + 1) / n_seg
        r0 = width * 0.075 * (1 - 0.35 * t0) * (1.0 + 0.06 * math.sin(t0 * math.pi * n_seg))
        r1 = width * 0.075 * (1 - 0.35 * t1) * (1.0 + 0.06 * math.sin(t1 * math.pi * n_seg))
        tb.taper(bark_mat, pts[i], pts[i + 1], r0, r1, segs=9, smooth=False)
    crown = pts[-1]
    for i in range(n_fronds):
        a = 2 * math.pi * i / n_fronds + rnd.uniform(-0.15, 0.15)
        tilt_up = rnd.uniform(0.15, 0.45) if kind == "ornamental" else rnd.uniform(0.35, 0.6)
        dirv = Vector((math.cos(a), math.sin(a), tilt_up)).normalized()
        tip = crown + dirv * frond_len
        root = crown + Vector((math.cos(a), math.sin(a), 0.0)) * (width * 0.06)
        tb.frond(foliage_mats[i % len(foliage_mats)], root, tip, width * 0.22, width * 0.03,
                 droop=droop, side_tilt=rnd.uniform(-0.15, 0.15), segs=6)
    tb.blob(bark_mat, crown, width * 0.09, subdiv=0)
    return tb.finish()


# ------------------------------------------------------------------- dispatch
ASSETS = {
    "broadleaf_s": dict(kind="broadleaf", height=5.46, width=3.96),
    "broadleaf_m": dict(kind="broadleaf", height=9.48, width=7.26),
    "broadleaf_l": dict(kind="broadleaf", height=15.44, width=11.99),
    "conifer_m": dict(kind="conifer", height=13.48, width=5.25),
    "conifer_l": dict(kind="conifer", height=22.46, width=8.40),
    "poplar": dict(kind="poplar", height=17.68, width=3.67),
    "bush_cluster": dict(kind="bush"),
    "palm_ornamental": dict(kind="palm", palm_kind="ornamental"),
    "palm_oil": dict(kind="palm", palm_kind="oil"),
}
AUTUMN_CAPABLE = {"broadleaf_s", "broadleaf_m", "broadleaf_l", "poplar", "bush_cluster"}


def build_asset(key, autumn=False, seed=0):
    spec = ASSETS[key]
    bark, foliage, conifer_m, autumn_m = base_materials()
    fol = autumn_m if autumn else foliage
    kind = spec["kind"]
    nm = key + ("_autumn" if autumn else "")
    if kind == "broadleaf":
        ob = build_broadleaf(nm, spec["height"], spec["width"], bark, fol, seed=seed)
    elif kind == "conifer":
        ob = build_conifer(nm, spec["height"], spec["width"], bark, conifer_m, seed=seed)
    elif kind == "poplar":
        ob = build_poplar(nm, spec["height"], spec["width"], bark, fol, seed=seed)
    elif kind == "bush":
        ob = build_bush(nm, fol, seed=seed)
    elif kind == "palm":
        ob = build_palm(nm, spec["palm_kind"], bark, fol, seed=seed)
    else:
        raise ValueError(key)
    return ob


try:
    ASSET
except NameError:
    ASSET = "broadleaf_m"

apex.reset_scene()
built = []
keys = list(ASSETS.keys()) if ASSET == "all" else [ASSET]

for key in keys:
    ob = build_asset(key, autumn=False, seed=(hash(key) & 0xffff))
    out_path = os.path.join(OUT_DIR, key + ".glb")
    export_glb(ob, out_path)
    built.append((key, len(ob.data.polygons), out_path))
    if key in AUTUMN_CAPABLE:
        ob2 = build_asset(key, autumn=True, seed=(hash(key) & 0xffff) + 1)
        out_path2 = os.path.join(OUT_DIR, key + "_autumn.glb")
        export_glb(ob2, out_path2)
        built.append((key + "_autumn", len(ob2.data.polygons), out_path2))

result = {"built": built}

