"""What the client draws: every renderable kn5 mesh, classified into a kit
surface (drawn with ApexSim's ground texture sets) or an AC-textured
scenery material, merged by material into 250 m cells, in the track frame.

Classification, per mesh: what physics surface lies under its vertices
decides first (a mesh standing on AC's road is the road; on grass, grass),
the shader and the names second (a multi-layer terrain shader far from any
physics is terrain). Alpha-tested or blended materials are never a kit
surface: they are the painted lines, tree cards and fences that lie *on*
a surface. Kerbs keep AC's textures too, because the striping is authored
into the texture and the kit's stripes would need an along-kerb UV the
kn5 does not carry. `--textures ac` keeps AC's textures everywhere;
`flat` draws every AC-textured material in its texture's average colour.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

import numpy as np

from .frame import Frame
from .kn5 import Kn5File, Kn5Material, Kn5Mesh
from .physics import PhysicsWorld, ground_set_for
from .sidecars import CONTACT_CURB, CONTACT_OFF, CONTACT_PIT_LANE, CONTACT_ROAD, CONTACT_RUNOFF

CELL_M = 250.0
MAX_MERGED_VERTICES = 1_500_000
UNDER_SAMPLES = 64
UNDER_TOLERANCE_M = 0.35
UNDER_MIN_SHARE = 0.75

KIT_COLOURS = {
    "road": [0.24, 0.24, 0.26, 1.0],
    "pit_lane": [0.32, 0.32, 0.34, 1.0],
    "asphalt": [0.3, 0.3, 0.33, 1.0],
    "grass": [0.16, 0.42, 0.14, 1.0],
    "gravel": [0.62, 0.55, 0.4, 1.0],
    "sand": [0.76, 0.68, 0.42, 1.0],
    "concrete": [0.55, 0.55, 0.55, 1.0],
}

_SKIP_NAME = re.compile(r"(GROOVE|SKIDMARK|KSLAYER)", re.IGNORECASE)
_GRASS_HINTS = ("GRASS", "ERBA", "TERRAIN", "TERREN", "GROUND", "HILL", "COLLINA", "FIELD", "PRATO", "LAND")
_SAND_HINTS = ("SAND", "SABBIA", "DUNE", "DIRT", "TERRA")
_GRAVEL_HINTS = ("GRAVEL", "GHIAIA", "GRVL")
_GLASS_HINTS = ("GLASS", "VETRO", "WINDOW", "FINESTR")
_PERF_TRAP_TRIS = 150_000


@dataclass
class MaterialSpec:
    key: str
    family: str
    base_color: list[float]
    ground_set: str | None = None
    texture: str | None = None
    blend: str | None = None
    two_sided: bool = False
    roughness: float | None = None
    alpha_cutoff: float | None = None
    #: Source bookkeeping for the report.
    ac_material: str = ""
    shader: str = ""
    meshes: int = 0
    triangles: int = 0

    def manifest_entry(self) -> dict:
        entry = {"key": self.key, "family": self.family, "base_color": [round(c, 4) for c in self.base_color]}
        if self.ground_set:
            entry["ground_set"] = self.ground_set
        if self.texture:
            entry["texture"] = self.texture
        if self.blend:
            entry["blend"] = self.blend
        if self.two_sided:
            entry["two_sided"] = True
        if self.roughness is not None:
            entry["roughness"] = round(self.roughness, 3)
        if self.alpha_cutoff is not None:
            entry["alpha_cutoff"] = round(self.alpha_cutoff, 3)
        return entry


@dataclass
class SceneMesh:
    key: str
    positions: np.ndarray  # (n, 3) track frame, metres
    normals: np.ndarray
    uvs: np.ndarray        # (n, 2)
    triangles: np.ndarray  # (m, 3)
    draw_distance_m: float | None
    collision: bool
    source: str


@dataclass
class MergedMesh:
    name: str
    key: str
    positions: np.ndarray
    normals: np.ndarray
    uvs: np.ndarray
    triangles: np.ndarray
    draw_distance_m: float | None
    collision: bool


@dataclass
class SceneReport:
    kept: int = 0
    dropped_lod: int = 0
    dropped_logic: int = 0
    dropped_overlay: int = 0
    dropped_hidden: int = 0
    kept_triangles: int = 0
    kit_triangles: int = 0
    scenery_triangles: int = 0
    warnings: list[str] = field(default_factory=list)
    #: mesh name -> class, for the report's per-material table.
    classes: dict[str, int] = field(default_factory=dict)


def _sanitise(text: str) -> str:
    out = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_").lower()
    return out or "material"


def _blend_for(material: Kn5Material) -> str:
    shader = material.shader
    name = material.name.upper()
    if material.alpha_tested or "AT" in shader.split("_")[0][10:] or shader in ("ksTree", "ksGrass") \
            or shader.startswith("ksPerPixelAT") or material.alpha_blend == 2:
        return "masked"
    if material.alpha_blend == 1 or shader == "ksPerPixelAlpha":
        return "translucent"
    if any(h in name for h in _GLASS_HINTS) and "Refl" in shader:
        return "translucent"
    return "opaque"


def _roughness_for(material: Kn5Material) -> float:
    exp = float(material.props.get("ksSpecularEXP", 40.0) or 40.0)
    return float(np.clip(1.0 - 0.22 * np.log10(max(exp, 1.0) + 1.0), 0.35, 0.95))


def _kit_key(kind: str) -> tuple[str, str, str | None]:
    """`(key, family, ground_set)` for a kit classification."""
    if kind == "road":
        return "road_ac", "road", None
    if kind == "pit_lane":
        return "pit_lane_ac", "pit_lane", None
    return f"ac_{kind}", "surface", kind


def _hint_kit(names: str, shader: str) -> str | None:
    text = names.upper()
    if any(h in text for h in _GRAVEL_HINTS):
        return "gravel"
    if any(h in text for h in _SAND_HINTS):
        return "sand"
    if shader.startswith("ksMultilayer") or any(h in text for h in _GRASS_HINTS):
        return "grass"
    return None


def _under(mesh_positions: np.ndarray, world: PhysicsWorld) -> tuple[int | None, str | None]:
    """The physics contact class most of a mesh's vertices stand on, and the
    surface key, or `(None, None)`."""
    n = mesh_positions.shape[0]
    if n == 0 or world.index.count == 0:
        return None, None
    pick = np.linspace(0, n - 1, min(UNDER_SAMPLES, n)).astype(np.int64)
    p = mesh_positions[pick]
    z, contact, surf = world.contacts_at(p[:, 0], p[:, 1], p[:, 2] + 0.5)
    close = np.isfinite(z) & (np.abs(z - p[:, 2]) < UNDER_TOLERANCE_M)
    # Three quarters of the vertices, not half: a fence or a hoarding has
    # its whole bottom row on the grass and is not grass.
    if close.mean() < UNDER_MIN_SHARE:
        return None, None
    contacts = contact[close]
    counts = np.bincount(contacts, minlength=5)
    best = int(np.argmax(counts))
    if counts[best] < close.sum() * 0.6:
        return None, None
    surfs = surf[close][contacts == best]
    key = world.surfaces[int(np.bincount(surfs).argmax())].key if surfs.size else None
    return best, key


def collect_scene(kn5s: list[Kn5File], world: PhysicsWorld, frame: Frame, textures_mode: str,
                  report: SceneReport) -> tuple[list[SceneMesh], dict[str, MaterialSpec], dict[str, tuple[Kn5File, str]]]:
    """Every drawn mesh with its material key, the material table, and the
    AC textures the table needs (`key -> (kn5, texture name)`)."""
    meshes: list[SceneMesh] = []
    materials: dict[str, MaterialSpec] = {}
    needed: dict[str, tuple[Kn5File, str]] = {}
    scenery_keys: dict[tuple[str, str], str] = {}

    def kit_material(kind: str) -> MaterialSpec:
        key, family, ground_set = _kit_key(kind)
        spec = materials.get(key)
        if spec is None:
            colour = KIT_COLOURS["road" if kind == "road" else "pit_lane" if kind == "pit_lane" else kind]
            spec = MaterialSpec(key=key, family=family, base_color=list(colour), ground_set=ground_set,
                                ac_material="(kit)", shader="")
            materials[key] = spec
        return spec

    def scenery_material(kn: Kn5File, material: Kn5Material) -> MaterialSpec:
        ident = (kn.path.name, material.name)
        key = scenery_keys.get(ident)
        if key is None:
            base = "scenery_" + _sanitise(material.name)
            key = base
            n = 2
            while key in materials:
                key = f"{base}_{n}"
                n += 1
            scenery_keys[ident] = key
            blend = _blend_for(material)
            texture = material.texture("txDiffuse")
            spec = MaterialSpec(key=key, family="scenery", base_color=[1.0, 1.0, 1.0, 1.0],
                                blend=blend, two_sided=True, roughness=_roughness_for(material),
                                alpha_cutoff=None, ac_material=material.name, shader=material.shader)
            if blend == "masked":
                ref = float(material.props.get("ksAlphaRef", 0.5) or 0.5)
                spec.alpha_cutoff = ref if 0.0 < ref < 1.0 else 0.5
            if texture and texture in kn.textures:
                spec.texture = texture
                needed[texture] = (kn, texture)
            elif texture:
                report.warnings.append(f"material {material.name!r} names texture {texture!r}, which its kn5 lacks")
            materials[key] = spec
        return materials[key]

    for kn in kn5s:
        for mesh in kn.meshes:
            if not mesh.renderable or not mesh.active:
                report.dropped_hidden += 1
                continue
            if mesh.lod_in > 0.0:
                report.dropped_lod += 1
                continue
            name_u = mesh.name.upper()
            if name_u.startswith("AC_"):
                report.dropped_logic += 1
                continue
            if _SKIP_NAME.search(mesh.name):
                report.dropped_overlay += 1
                continue
            if mesh.triangle_count == 0:
                continue
            material = kn.materials[mesh.material]
            positions = frame.ac_points(mesh.world_positions())
            normals = frame.ac_vectors(mesh.world_normals())
            blend = _blend_for(material)
            under, under_key = _under(positions, world)
            kit: str | None = None
            if textures_mode == "kit" and blend == "opaque":
                if under in (CONTACT_ROAD,):
                    kit = "road"
                elif under == CONTACT_PIT_LANE:
                    kit = "pit_lane"
                elif under in (CONTACT_RUNOFF, CONTACT_OFF):
                    kit = ground_set_for(under_key or "", under)
                elif under is None:
                    kit = _hint_kit(f"{mesh.name} {material.name} {material.texture('txDiffuse') or ''}",
                                    material.shader)
            if mesh.triangle_count > _PERF_TRAP_TRIS or material.shader == "ksGrass":
                report.warnings.append(
                    f"mesh {mesh.name!r} ({mesh.triangle_count} triangles, {material.shader}) is a performance trap")
            if kit:
                spec = kit_material(kit)
                uvs = positions[:, :2].copy()
                collision = True
            else:
                spec = scenery_material(kn, material)
                uvs = mesh.vertices["uv"].astype(np.float64)
                collision = under is not None and blend == "opaque"
            spec.meshes += 1
            spec.triangles += mesh.triangle_count
            report.kept += 1
            report.kept_triangles += mesh.triangle_count
            if kit:
                report.kit_triangles += mesh.triangle_count
            else:
                report.scenery_triangles += mesh.triangle_count
            report.classes[f"{kn.path.name}:{mesh.name}"] = under if under is not None else -1
            meshes.append(SceneMesh(key=spec.key, positions=positions, normals=normals, uvs=uvs,
                                    triangles=mesh.triangles.astype(np.int64),
                                    draw_distance_m=float(mesh.lod_out) if mesh.lod_out > 0 else None,
                                    collision=collision, source=f"{kn.path.name}:{mesh.name}"))
    return meshes, materials, needed


def _distance_bucket(d: float | None) -> int:
    if d is None:
        return 0
    for edge in (50, 100, 150, 200, 300, 400, 600, 800, 1200, 2000):
        if d <= edge:
            return edge
    return 0


def _cell_tag(v: int) -> str:
    return f"{'m' if v < 0 else 'p'}{abs(v)}"


def merge_meshes(meshes: list[SceneMesh]) -> list[MergedMesh]:
    """Merge by (material, 250 m cell of the centroid, draw distance,
    collision); big groups are split so no mesh passes
    `MAX_MERGED_VERTICES`. Deterministic: groups and members are sorted."""
    groups: dict[tuple, list[SceneMesh]] = {}
    for m in meshes:
        c = m.positions.mean(axis=0)
        cx = int(np.floor(c[0] / CELL_M))
        cy = int(np.floor(c[1] / CELL_M))
        groups.setdefault((m.key, cx, cy, _distance_bucket(m.draw_distance_m), m.collision), []).append(m)
    out: list[MergedMesh] = []
    for (key, cx, cy, bucket, collision) in sorted(groups, key=lambda g: (g[0], g[1], g[2], g[3], g[4])):
        members = sorted(groups[(key, cx, cy, bucket, collision)], key=lambda m: m.source)
        base = f"{key}_{_cell_tag(cx)}_{_cell_tag(cy)}" + (f"_d{bucket}" if bucket else "") + ("" if collision else "_nc")
        part = 0
        chunk: list[SceneMesh] = []
        verts = 0
        for m in members:
            if chunk and verts + m.positions.shape[0] > MAX_MERGED_VERTICES:
                out.append(_merge(base if part == 0 else f"{base}_{part}", key, chunk, bucket, collision))
                part += 1
                chunk, verts = [], 0
            chunk.append(m)
            verts += m.positions.shape[0]
        if chunk:
            out.append(_merge(base if part == 0 else f"{base}_{part}", key, chunk, bucket, collision))
    # Names must be unique; a split at part 0 named `base` and a later `base_1` are.
    seen: set[str] = set()
    for m in out:
        assert m.name not in seen, m.name
        seen.add(m.name)
    return out


def _merge(name: str, key: str, members: list[SceneMesh], bucket: int, collision: bool) -> MergedMesh:
    offsets = np.cumsum([0] + [m.positions.shape[0] for m in members[:-1]])
    positions = np.concatenate([m.positions for m in members])
    normals = np.concatenate([m.normals for m in members])
    uvs = np.concatenate([m.uvs for m in members])
    triangles = np.concatenate([m.triangles + off for m, off in zip(members, offsets)])
    distance = max((m.draw_distance_m or 0.0) for m in members) if bucket else None
    return MergedMesh(name=name, key=key, positions=positions, normals=normals, uvs=uvs,
                      triangles=triangles, draw_distance_m=distance if distance else None,
                      collision=collision)
