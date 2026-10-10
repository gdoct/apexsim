"""The kn5 -> the GLBs the client draws (docs/content/ac-import.md, "Visuals").

AC stores a car as (x left, y up, z forward) in metres with
counter-clockwise front faces: glTF's frame, so positions are copied. Only
the seat changes: the body is lifted so its tyres stand on y = 0 and
shifted so the origin is midway between the axles, which is where the
client's `[wheels]` figures are measured from.

Parts, by the dummy a mesh hangs under (its `path`):

- `WHEEL_LF` / `WHEEL_LR` (with `DISC_*`) become `wheels/front.glb` and
  `wheels/rear.glb` in the wheel dummy's own frame: hub at the origin,
  axle on X, face on +X, the class wheels' frame. The other two corners
  are drawn from those by the client. `RIM_BLUR_*` is dropped.
- `STEER_HR` becomes `steering_wheel.glb` in its own frame, which is the
  column's: the rig tilts it by `wheel_rake_deg` and turns it.
- `COCKPIT_LR`, `STEER_LR`, `DAMAGE_GLASS*`, `CINTURE_ON` (belts over a
  driver ApexSim does not draw), `lod_in > 0`, hidden and non-renderable
  meshes are dropped.
- Everything else is the body.
"""

from __future__ import annotations

import io
import re
from dataclasses import dataclass, field

import numpy as np
from PIL import Image

from ac_import.kn5 import Kn5Dummy, Kn5File, Kn5Material, Kn5Mesh
from ac_import.textures import decode_image

from .data import CarData, number, vector
from .glb import GlbBuilder, GlbMaterial

CORNERS = ("LF", "RF", "LR", "RR")

DROP_PARTS = (
    (re.compile(r"^RIM_BLUR_", re.I), "the pre-blurred spinning rim"),
    (re.compile(r"^DAMAGE_GLASS", re.I), "broken glass (damage)"),
    (re.compile(r"^COCKPIT_LR$", re.I), "the low-detail cockpit"),
    (re.compile(r"^STEER_LR$", re.I), "the low-detail steering wheel"),
    (re.compile(r"^CINTURE_ON$", re.I), "the belts drawn over a driver"),
)

JPEG_QUALITY = 90


class ModelError(Exception):
    pass


@dataclass
class Frame:
    """A dummy's world transform as origin and axes (rows: local x, y, z in
    world), with the scale taken out."""
    origin: np.ndarray
    axes: np.ndarray

    @classmethod
    def of(cls, dummy: Kn5Dummy) -> "Frame":
        m = dummy.transform.astype(np.float64)
        axes = m[:3, :3].copy()
        axes /= np.linalg.norm(axes, axis=1, keepdims=True)
        return cls(m[3, :3].copy(), axes)

    def to_local(self, points: np.ndarray) -> np.ndarray:
        return (points - self.origin) @ self.axes.T

    def dir_to_local(self, dirs: np.ndarray) -> np.ndarray:
        return dirs @ self.axes.T


@dataclass
class Split:
    body: list[Kn5Mesh] = field(default_factory=list)
    wheels: dict[str, list[Kn5Mesh]] = field(default_factory=dict)
    steer: list[Kn5Mesh] = field(default_factory=list)
    #: reason -> [mesh names]
    dropped: dict[str, list[str]] = field(default_factory=dict)


def _chain(m: Kn5Mesh) -> tuple[str, ...]:
    return m.path + (m.name,)


def split_meshes(kn5: Kn5File, *, keep_steering_wheel_in_body: bool = False) -> Split:
    out = Split(wheels={c: [] for c in CORNERS})

    def drop(reason: str, m: Kn5Mesh) -> None:
        out.dropped.setdefault(reason, []).append(m.name)

    for m in kn5.meshes:
        chain = _chain(m)
        if not m.renderable:
            drop("not renderable", m)
            continue
        if not m.active:
            drop("hidden in the kn5", m)
            continue
        if m.lod_in > 0.0:
            drop("a far LOD stand-in (lodIn > 0)", m)
            continue
        if m.triangle_count == 0:
            continue
        reason = next((why for rx, why in DROP_PARTS for n in chain if rx.match(n)), None)
        if reason:
            drop(reason, m)
            continue
        corner = next((n[-2:].upper() for n in chain
                       if re.match(r"^(WHEEL|DISC)_(LF|RF|LR|RR)$", n, re.I)), None)
        if corner:
            out.wheels[corner].append(m)
            continue
        if any(n.upper() == "STEER_HR" for n in chain) and not keep_steering_wheel_in_body:
            out.steer.append(m)
            continue
        out.body.append(m)
    return out


def find_dummy(kn5: Kn5File, name: str) -> Kn5Dummy | None:
    for d in kn5.dummies:
        if d.name.upper() == name.upper():
            return d
    return None


# --- seating --------------------------------------------------------------

@dataclass
class WheelFit:
    corner: str
    hub: np.ndarray  # model frame
    radius: float
    width: float


@dataclass
class Seat:
    """model frame -> glTF frame: p + offset."""
    offset: np.ndarray
    wheels: dict[str, WheelFit]

    def apply(self, p: np.ndarray) -> np.ndarray:
        return p + self.offset


def fit_wheels(kn5: Kn5File, split: Split) -> dict[str, WheelFit]:
    fits: dict[str, WheelFit] = {}
    for corner in CORNERS:
        dummy = find_dummy(kn5, f"WHEEL_{corner}")
        if dummy is None:
            raise ModelError(f"the kn5 has no WHEEL_{corner} dummy")
        meshes = split.wheels[corner]
        if not meshes:
            raise ModelError(f"WHEEL_{corner} has no meshes")
        frame = Frame.of(dummy)
        # The tyre sets the size; a rim or disc never reaches past it.
        tyres = [m for m in meshes if re.search(r"tyre|tire", " ".join(_chain(m)), re.I)] or meshes
        local = np.concatenate([frame.to_local(m.world_positions()) for m in tyres])
        radius = float(np.sqrt(local[:, 1] ** 2 + local[:, 2] ** 2).max())
        width = float(local[:, 0].max() - local[:, 0].min())
        fits[corner] = WheelFit(corner, frame.origin, radius, width)
    return fits


def seat_for(fits: dict[str, WheelFit]) -> Seat:
    lift = float(np.mean([f.radius - f.hub[1] for f in fits.values()]))
    front_z = 0.5 * (fits["LF"].hub[2] + fits["RF"].hub[2])
    rear_z = 0.5 * (fits["LR"].hub[2] + fits["RR"].hub[2])
    if front_z <= rear_z:
        raise ModelError("the front wheels are not ahead of the rear ones: not a car in AC's frame")
    shift = -0.5 * (front_z + rear_z)
    return Seat(np.array([0.0, lift, shift]), fits)


# --- materials ------------------------------------------------------------

def _roughness(mat: Kn5Material) -> float:
    exp = float(mat.props.get("ksSpecularEXP", 40.0) or 0.0)
    return float(np.clip(1.1 - 0.3 * np.log10(max(exp, 0.0) + 1.0), 0.05, 0.95))


def alpha_mode(mat: Kn5Material) -> str:
    if mat.alpha_blend:
        return "BLEND"
    if mat.alpha_tested:
        return "MASK"
    return "OPAQUE"


@dataclass
class LightSpec:
    slot: str
    colour: tuple[float, float, float]
    nodes: list[str]


def read_lights(car: CarData, kn5: Kn5File, seat: Seat) -> dict[str, str]:
    """mesh node name -> `car_brakelight` / `car_taillight` /
    `car_headlight` / `car_rainlight`, from lights.ini. A lamp counts as
    front or rear by where its mesh is; a lit display LED between the
    axles is neither and keeps its material, and so does glass AC lights
    from behind (a blended material stays glass)."""
    lights = car.ini("lights.ini")
    front_z = 0.5 * (seat.wheels["LF"].hub[2] + seat.wheels["RF"].hub[2])
    rear_z = 0.5 * (seat.wheels["LR"].hub[2] + seat.wheels["RR"].hub[2])
    centroids: dict[str, float] = {}
    for m in kn5.meshes:
        if alpha_mode(kn5.materials[m.material]) == "BLEND":
            continue
        for n in _chain(m)[-2:]:
            centroids.setdefault(n.upper(), float(m.world_positions()[:, 2].mean()))
    out: dict[str, str] = {}
    for section, s in lights.items():
        names = [x.strip() for x in str(s.get("NAME", "")).split(",") if x.strip()]
        colour = vector(s.get("COLOR")) or [0.0, 0.0, 0.0]
        if max(colour) <= 0.0:
            continue
        for node in names:
            z = centroids.get(node.upper())
            if z is None:
                continue
            front = z > front_z - 0.25
            rear = z < rear_z + 0.25
            if section.startswith("BRAKE_"):
                slot = "car_brakelight" if rear else None
            elif number(s.get("SPECIAL"), 0) and rear:
                slot = "car_rainlight"
            elif rear:
                slot = "car_taillight"
            elif front:
                slot = "car_headlight"
            else:
                slot = None
            if slot:
                out.setdefault(node.upper(), slot)
    return out


def light_colours(car: CarData) -> dict[str, tuple[float, float, float]]:
    """The colour each light slot takes, normalised, from the brightest
    lamp lights.ini gives it."""
    lights = car.ini("lights.ini")
    best: dict[str, tuple[float, list[float]]] = {}
    for section, s in lights.items():
        colour = vector(s.get("COLOR")) or [0.0, 0.0, 0.0]
        peak = max(colour)
        if peak <= 0:
            continue
        key = "BRAKE" if section.startswith("BRAKE_") else ("SPECIAL" if number(s.get("SPECIAL"), 0) else "LIGHT")
        if key not in best or peak > best[key][0]:
            best[key] = (peak, colour)
    return {k: tuple(c / p for c in col) for k, (p, col) in best.items()}


# --- textures -------------------------------------------------------------

# --- mirrors --------------------------------------------------------------

#: The glass slots the cockpit rig paints its mirror captures onto, one
#: material each (the game finds them by name, like the lamps).
MIRROR_SLOTS = ("car_mirror_centre", "car_mirror_left", "car_mirror_right")


@dataclass
class MirrorGlass:
    """One mirror's glass as the cockpit rig paints it (model frame, m).

    AC draws a mirror by rendering into the glass mesh's own UVs; here the
    glass gets new ones, laid flat across it as the driver faces it, so the
    rig's rear-facing capture fills it edge to edge: `u` runs from the
    driver's right to their left and `v` from top to bottom. That is the
    capture's own image read the way a mirror reads, so the client samples
    it unflipped.

    AC renders one rear view for every mirror, and Kunos often model all
    the glass as one mesh (the 787B's `MIRROR` is both door mirrors and the
    interior one), each piece's UVs picking its part of that view. So the
    glass is found by piece, not by mesh: `parts` are the triangles of it
    in each mesh."""
    slot: str
    node: str
    parts: list[tuple[Kn5Mesh, np.ndarray]]
    centroid: np.ndarray
    #: in the glass, toward the driver's right
    across: np.ndarray
    up: np.ndarray
    across_max: float
    up_max: float
    #: width, height
    size: tuple[float, float]

    def uv(self, p: np.ndarray) -> np.ndarray:
        p = np.asarray(p, dtype=np.float64)
        u = (self.across_max - p @ self.across) / self.size[0]
        v = (self.up_max - p @ self.up) / self.size[1]
        return np.clip(np.stack([u, v], axis=1), 0.0, 1.0).astype(np.float32)


#: Pieces of glass closer than this (m) are one mirror.
MIRROR_JOIN_M = 0.03


def _islands(m: Kn5Mesh) -> list[np.ndarray]:
    """The triangle rows of each connected piece of a mesh, joined through
    vertices that share a position (a seam splits them in the buffer)."""
    tris = m.triangles.astype(np.int64)
    if len(tris) == 0:
        return []
    key = np.round(m.world_positions() / 5e-4).astype(np.int64)
    _, vid = np.unique(key, axis=0, return_inverse=True)
    vid = np.asarray(vid).ravel()[tris]
    parent = list(range(int(vid.max()) + 1))

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for a, b, c in vid.tolist():
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    roots = np.array([find(int(a)) for a in vid[:, 0]])
    return [np.flatnonzero(roots == r) for r in dict.fromkeys(roots.tolist())]


def read_mirrors(car: CarData, kn5: Kn5File, eye: np.ndarray | None) -> dict[str, MirrorGlass]:
    """slot -> glass, from mirrors.ini. The named meshes are cut into
    pieces (connected triangles, joined within `MIRROR_JOIN_M`); a piece is
    the centre mirror within 20 cm of the centreline, or when it is the only
    piece of an interior (`INT...`) mesh, else left or right by its side;
    the largest of each wins and the rest stay chrome. The glass's plane is
    the thinnest axis of its points, facing the eye (or the tail, without
    one)."""
    pieces: list[tuple[str, Kn5Mesh, np.ndarray]] = []
    seen: set[int] = set()
    for sec in car.ini("mirrors.ini").values():
        node = str(sec.get("NAME", "")).strip()
        if not node:
            continue
        for m in kn5.meshes:
            if id(m) in seen or node.upper() not in (n.upper() for n in _chain(m)):
                continue
            seen.add(id(m))
            pieces += [(node, m, rows) for rows in _islands(m)]
    if not pieces:
        return {}

    def points(m: Kn5Mesh, rows: np.ndarray) -> np.ndarray:
        return m.world_positions()[np.unique(m.triangles[rows])].astype(np.float64)

    boxes = [(points(m, r).min(axis=0), points(m, r).max(axis=0)) for _, m, r in pieces]
    group = list(range(len(pieces)))

    def root(i: int) -> int:
        while group[i] != i:
            i = group[i]
        return i

    for i in range(len(pieces)):
        for j in range(i + 1, len(pieces)):
            gap = np.maximum(boxes[i][0] - boxes[j][1], boxes[j][0] - boxes[i][1]).max()
            if gap < MIRROR_JOIN_M:
                group[root(j)] = root(i)
    clusters: dict[int, list[int]] = {}
    for i in range(len(pieces)):
        clusters.setdefault(root(i), []).append(i)
    per_node: dict[str, int] = {}
    for members in clusters.values():
        node = pieces[members[0]][0]
        per_node[node] = per_node.get(node, 0) + 1

    candidates: list[tuple[float, str, str, list[tuple[Kn5Mesh, np.ndarray]]]] = []
    for members in clusters.values():
        node = pieces[members[0]][0]
        parts = [(pieces[i][1], pieces[i][2]) for i in members]
        tri = np.concatenate([m.world_positions()[m.triangles[r]] for m, r in parts]).astype(np.float64)
        area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
        c = (tri.mean(axis=1) * area[:, None]).sum(axis=0) / max(float(area.sum()), 1e-12)
        if abs(c[0]) < 0.2 or (node.upper().startswith("INT") and per_node[node] == 1):
            slot = "car_mirror_centre"
        elif c[0] > 0:
            slot = "car_mirror_left"
        else:
            slot = "car_mirror_right"
        candidates.append((float(area.sum()), slot, node, parts))

    out: dict[str, MirrorGlass] = {}
    for _, slot, node, parts in sorted(candidates, key=lambda k: -k[0]):
        if slot in out:
            continue
        pts = np.concatenate([points(m, r) for m, r in parts])
        c = pts.mean(axis=0)
        _, _, axes = np.linalg.svd(pts - c, full_matrices=False)
        normal = axes[-1]
        toward = (np.asarray(eye, dtype=np.float64) - c) if eye is not None else np.array([0.0, 0.0, -1.0])
        if normal @ toward < 0:
            normal = -normal
        # (x left, y up, z nose): up x normal is the driver's right.
        across = np.cross([0.0, 1.0, 0.0], normal)
        if np.linalg.norm(across) < 1e-3:
            across = np.array([-1.0, 0.0, 0.0]) - normal * -normal[0]
        across /= np.linalg.norm(across)
        up = np.cross(normal, across)
        a, u = pts @ across, pts @ up
        size = (max(float(a.max() - a.min()), 1e-3), max(float(u.max() - u.min()), 1e-3))
        out[slot] = MirrorGlass(slot, node, parts, c, across, up, float(a.max()), float(u.max()), size)
    return out


def mirror_parts(mirrors: dict[str, MirrorGlass] | None) -> dict[int, list[tuple[str, np.ndarray]]]:
    """id(mesh) -> [(slot, triangle rows)] for every mesh holding glass,
    the rows no mirror claimed last with slot ""."""
    by_mesh: dict[int, tuple[Kn5Mesh, list[tuple[str, np.ndarray]]]] = {}
    for g in (mirrors or {}).values():
        for m, rows in g.parts:
            by_mesh.setdefault(id(m), (m, []))[1].append((g.slot, rows))
    out: dict[int, list[tuple[str, np.ndarray]]] = {}
    for key, (m, parts) in by_mesh.items():
        rest = np.setdiff1d(np.arange(len(m.triangles)), np.concatenate([r for _, r in parts]))
        if len(rest):
            parts.append(("", rest))
        out[key] = parts
    return out


@dataclass
class TextureOut:
    data: bytes
    mime: str
    ext: str
    width: int
    height: int


def encode_texture(blob: bytes, max_size: int, with_alpha: bool) -> TextureOut:
    rgba = decode_image(blob)
    h, w = rgba.shape[:2]
    scale = 1.0
    while max(w, h) * scale > max_size:
        scale /= 2.0
    if scale < 1.0:
        im = Image.fromarray(rgba, "RGBA").resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    else:
        im = Image.fromarray(rgba, "RGBA")
    buf = io.BytesIO()
    if with_alpha:
        im.save(buf, format="PNG", optimize=False, compress_level=6)
        return TextureOut(buf.getvalue(), "image/png", "png", im.width, im.height)
    im.convert("RGB").save(buf, format="JPEG", quality=JPEG_QUALITY, subsampling=0, optimize=False)
    return TextureOut(buf.getvalue(), "image/jpeg", "jpg", im.width, im.height)


# --- the materials plan ---------------------------------------------------

@dataclass
class MaterialPlan:
    """How every kept AC material becomes a glTF material, and which
    textures are written at which size."""
    skin_texture: str | None
    #: (AC material index, light slot or "") -> output name
    names: dict[tuple[int, str], str] = field(default_factory=dict)
    #: texture name -> needs alpha (a blended or masked material samples it)
    alpha_textures: dict[str, bool] = field(default_factory=dict)
    #: output name -> AC material name, for the report
    report: dict[str, dict] = field(default_factory=dict)


def find_skin_texture(kn5: Kn5File) -> str | None:
    for name in kn5.textures:
        if name.lower() == "skin_00.dds":
            return name
    return None


def plan_materials(kn5: Kn5File, split: Split, lights: dict[str, str],
                   mirrors: dict[str, MirrorGlass] | None = None) -> MaterialPlan:
    plan = MaterialPlan(skin_texture=find_skin_texture(kn5))
    skin_count = 0
    used: set[str] = set()

    def unique(name: str) -> str:
        base, n = name, 1
        while name.lower() in used:
            name = f"{base}_{n}"
            n += 1
        used.add(name.lower())
        return name

    def light_of(m: Kn5Mesh) -> str:
        for n in reversed(_chain(m)):
            slot = lights.get(n.upper())
            if slot:
                return slot
        return ""

    parts: list[tuple[str, list[Kn5Mesh]]] = [("body", split.body), ("steer", split.steer)]
    parts += [(f"wheel_{c}", split.wheels[c]) for c in ("LF", "LR")]
    rim_for: dict[str, int] = {}
    for part, meshes in parts:
        if part.startswith("wheel_"):
            tris: dict[int, int] = {}
            for m in meshes:
                shader = kn5.materials[m.material].shader
                if shader not in ("ksTyres", "ksBrakeDisc") and alpha_mode(kn5.materials[m.material]) == "OPAQUE":
                    tris[m.material] = tris.get(m.material, 0) + m.triangle_count
            if tris:
                rim_for[part] = max(sorted(tris), key=lambda k: tris[k])

    # Names are decided in a fixed order (material index) so re-running
    # names every slot the same.
    wanted: set[tuple[int, str, str]] = set()
    glass = mirror_parts(mirrors)
    for part, meshes in parts:
        for m in meshes:
            slot = light_of(m) if part == "body" else ""
            pieces = glass.get(id(m)) if part == "body" else None
            for glass_slot, _ in pieces or [("", None)]:
                wanted.add((m.material, glass_slot or slot, part))
    light_taken: set[str] = set()
    for mat_index, slot, part in sorted(wanted, key=lambda k: (k[0], k[1], k[2])):
        if (mat_index, slot) in plan.names:
            continue
        mat = kn5.materials[mat_index]
        diffuse = mat.texture("txDiffuse")
        if slot:
            if slot in light_taken:
                # One material per light slot: the game finds it by name.
                plan.names[(mat_index, slot)] = next(v for (i, s), v in plan.names.items() if s == slot)
                continue
            light_taken.add(slot)
            name = slot
            used.add(slot)
        elif plan.skin_texture and diffuse == plan.skin_texture:
            name = unique("car_skin" if skin_count == 0 else f"car_skin_{skin_count}")
            skin_count += 1
        elif mat.shader == "ksTyres":
            name = unique("wheel_tyre")
        elif mat.shader == "ksBrakeDisc":
            name = unique("wheel_brake")
        elif part.startswith("wheel_") and rim_for.get(part) == mat_index:
            name = unique("wheel_rim")
        else:
            name = unique(re.sub(r"[^A-Za-z0-9_\-]", "_", mat.name) or f"material_{mat_index}")
        plan.names[(mat_index, slot)] = name
        plan.report[name] = {"ac_material": mat.name, "shader": mat.shader, "texture": diffuse,
                             "alpha": alpha_mode(mat)}
        if diffuse and diffuse in kn5.textures and slot not in MIRROR_SLOTS:
            plan.alpha_textures[diffuse] = plan.alpha_textures.get(diffuse, False) or alpha_mode(mat) != "OPAQUE"
    # Lit rim/disc materials share the rim's name across both wheel GLBs;
    # that is intended (a skin's rim texture reaches both).
    return plan


def material_for(kn5: Kn5File, plan: MaterialPlan, mat_index: int, slot: str,
                 image: int | None, colours: dict[str, tuple[float, float, float]]) -> GlbMaterial:
    mat = kn5.materials[mat_index]
    name = plan.names[(mat_index, slot)]
    ambient = float(mat.props.get("ksAmbient", 0.4))
    diffuse = float(mat.props.get("ksDiffuse", 0.4))
    # AC lights a surface as texture x (ambient + diffuse x N.L); a PBR
    # base colour is the texture alone, so the pair scales it down for
    # the surfaces Kunos darkened (seats, trim), never up.
    level = float(np.clip((ambient + diffuse) / 0.9, 0.2, 1.0))
    fresnel_max = float(mat.props.get("fresnelMaxLevel", 0.0))
    fresnel_c = float(mat.props.get("fresnelC", 0.0))
    g = GlbMaterial(
        name=name,
        base_color=(level, level, level, 1.0),
        texture=image,
        roughness=_roughness(mat),
        alpha_mode=alpha_mode(mat),
    )
    if slot in MIRROR_SLOTS:
        # Chrome outside the cockpit; the rig paints its capture over it
        # there. The glass's AC texture would not fit its new UVs.
        g.texture, g.metallic, g.roughness, g.base_color = None, 1.0, 0.05, (1.0, 1.0, 1.0, 1.0)
        g.alpha_mode = "OPAQUE"
        return g
    if "mirror" in mat.name.lower() or (fresnel_max >= 0.95 and fresnel_c >= 0.9):
        g.metallic, g.roughness, g.base_color = 1.0, 0.05, (1.0, 1.0, 1.0, 1.0)
    elif fresnel_max >= 0.8 and "Reflection" in mat.shader:
        g.metallic = 1.0
    if g.alpha_mode == "BLEND" and float(mat.props.get("ksSpecularEXP", 0)) >= 200:
        g.roughness = 0.05
    if plan.skin_texture and mat.texture("txDiffuse") == plan.skin_texture and fresnel_max >= 0.3 and not slot:
        g.clearcoat, g.clearcoat_roughness = 1.0, 0.08
    if slot:
        key = {"car_brakelight": "BRAKE", "car_rainlight": "SPECIAL"}.get(slot, "LIGHT")
        colour = colours.get(key, (1.0, 0.1, 0.05) if key != "LIGHT" else (1.0, 0.95, 0.9))
        if slot == "car_rainlight":
            # Nothing switches it yet; lit, it would glow all race.
            colour = (0.0, 0.0, 0.0)
        g.emissive = colour
        g.alpha_mode = "OPAQUE"
    return g


# --- building the GLBs ----------------------------------------------------

@dataclass
class BuiltGlb:
    builder: GlbBuilder
    #: texture name -> image index
    images: dict[str, int]


def build_glb(kn5: Kn5File, meshes: list[Kn5Mesh], plan: MaterialPlan, lights: dict[str, str],
              colours: dict[str, tuple[float, float, float]], to_glb, normals_to_glb,
              texture_sizes: dict[str, int], texture_cache: dict[tuple[str, int, bool], TextureOut],
              light_meshes: bool, mirrors: dict[str, MirrorGlass] | None = None) -> BuiltGlb:
    """One GLB of `meshes`, positions through `to_glb` (a function of an
    (n, 3) array), every AC material to its planned glTF material; the
    glass of `mirrors` gets its UVs from the glass (`MirrorGlass.uv`)."""
    b = GlbBuilder()
    images: dict[str, int] = {}
    mat_out: dict[str, int] = {}
    groups: dict[str, list[tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]]] = {}
    order: list[tuple[str, int, str]] = []

    def light_of(m: Kn5Mesh) -> str:
        if not light_meshes:
            return ""
        for n in reversed(_chain(m)):
            slot = lights.get(n.upper())
            if slot:
                return slot
        return ""

    glass = mirror_parts(mirrors)
    for m in meshes:
        parts = glass.get(id(m)) or [("", None)]
        for glass_slot, rows in parts:
            slot = glass_slot or light_of(m)
            name = plan.names[(m.material, slot)]
            if name not in groups:
                groups[name] = []
                order.append((name, m.material, slot))
            pos, nrm, tris = m.world_positions(), m.world_normals(), m.triangles
            uv = m.vertices["uv"].astype(np.float32)
            if rows is not None:
                # Only this piece's triangles, and only the vertices they use.
                used, tris = np.unique(tris[rows], return_inverse=True)
                tris = tris.reshape(-1, 3).astype(m.triangles.dtype)
                pos, nrm, uv = pos[used], nrm[used], uv[used]
            if glass_slot:
                uv = mirrors[glass_slot].uv(pos)
            groups[name].append((to_glb(pos), normals_to_glb(nrm), uv, tris))

    for name, mat_index, slot in order:
        mat = kn5.materials[mat_index]
        tex_name = None if slot in MIRROR_SLOTS else mat.texture("txDiffuse")
        image = None
        if tex_name and tex_name in kn5.textures:
            alpha = plan.alpha_textures.get(tex_name, False)
            size = texture_sizes.get(tex_name, 1024)
            key = (tex_name, size, alpha)
            if key not in texture_cache:
                try:
                    texture_cache[key] = encode_texture(kn5.textures[tex_name].data, size, alpha)
                except Exception:  # noqa: BLE001 - a texture Pillow cannot read draws flat
                    texture_cache[key] = None
            t = texture_cache[key]
            if t is not None:
                image = b.add_image(tex_name, t.data, t.mime)
                images[tex_name] = image
        mat_out[name] = b.add_material(material_for(kn5, plan, mat_index, slot, image, colours))
        ps, ns, uvs, tris = [], [], [], []
        base = 0
        for p, n, uv, t in groups[name]:
            ps.append(p)
            ns.append(n)
            uvs.append(uv)
            tris.append(t + base)
            base += len(p)
        b.add_primitive(np.concatenate(ps), np.concatenate(ns), np.concatenate(uvs),
                        np.concatenate(tris), mat_out[name])
    return BuiltGlb(b, images)


def steering_frame(kn5: Kn5File) -> Frame | None:
    d = find_dummy(kn5, "STEER_HR")
    return Frame.of(d) if d is not None else None


def column_rake_deg(frame: Frame) -> float:
    """The rig's `wheel_rake_deg`: the pitch that turns the car's nose axis
    onto the column (the steering dummy's local +Z). The rig pitches its
    wheel pivot the Unreal way, positive lifting the column's forward end
    (the top of the rim toward the driver); a real column runs forward and
    down, so a real car's figure is negative."""
    c = frame.axes[2]
    return float(np.degrees(np.arctan2(c[1], c[2])))


def body_top(points: np.ndarray, min_width: float = 0.3, slice_m: float = 0.02) -> float:
    """The top of the bodywork: the highest slice of `slice_m` still at
    least `min_width` across, so an aerial or a wiper arm standing proud of
    the roof does not set the car's height (it sets the collision box)."""
    y = points[:, 1]
    top = float(y.max())
    floor = float(y.min())
    order = np.argsort(-y, kind="stable")
    ys, xs = y[order], points[order, 0]
    h = top
    while h > floor:
        band = (ys <= h) & (ys > h - slice_m)
        if band.any() and float(xs[band].max() - xs[band].min()) >= min_width:
            return h
        h -= slice_m
    return top


def roof_over(points: np.ndarray, triangles: np.ndarray, at: np.ndarray, clearance: float = 0.05) -> bool:
    """Whether any triangle lies straight above `at` (a vertical ray, tested
    in plan): a closed cabin has a roof over the driver's eye."""
    tri = points[triangles]
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
    # Barycentric coordinates of `at` in each triangle's plan (x, z).
    v0, v1 = b[:, [0, 2]] - a[:, [0, 2]], c[:, [0, 2]] - a[:, [0, 2]]
    v2 = at[[0, 2]] - a[:, [0, 2]]
    den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
    ok = np.abs(den) > 1e-12
    den = np.where(ok, den, 1.0)
    u = (v2[:, 0] * v1[:, 1] - v1[:, 0] * v2[:, 1]) / den
    v = (v0[:, 0] * v2[:, 1] - v2[:, 0] * v0[:, 1]) / den
    inside = ok & (u >= 0) & (v >= 0) & (u + v <= 1)
    y = a[:, 1] + u * (b[:, 1] - a[:, 1]) + v * (c[:, 1] - a[:, 1])
    return bool((inside & (y > at[1] + clearance)).any())


def mesh_centroid(kn5: Kn5File, node: str) -> np.ndarray | None:
    pts = [m.world_positions() for m in kn5.meshes if node.upper() in (n.upper() for n in _chain(m))]
    if not pts:
        return None
    return np.concatenate(pts).mean(axis=0)
