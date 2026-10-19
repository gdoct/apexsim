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

#: Overlays, matched on the mesh name and a see-through material's name:
#: Spa's rubber grooves are meshes called `Plane023` and `Loft286` wearing
#: a `groove3` material. They are painted to darken AC's own asphalt, and
#: laid over the kit's they drew a hard-edged dark sheet across the road.
_SKIP_NAME = re.compile(r"(GROOVE|SKIDMARK|KSLAYER)", re.IGNORECASE)
_KERB_HINTS = ("KERB", "CURB", "CORDOL")
_GRASS_HINTS = ("GRASS", "ERBA", "TERRAIN", "TERREN", "GROUND", "HILL", "COLLINA", "FIELD", "PRATO", "LAND")
_SAND_HINTS = ("SAND", "SABBIA", "DUNE")
#: Earth: sand, but only once grass has had its turn, since `TERRAIN` holds
#: `TERRA`.
_DIRT_HINTS = ("DIRT", "TERRA")
_GRAVEL_HINTS = ("GRAVEL", "GHIAIA", "GRVL")
_ROAD_HINTS = ("ASPH", "ASFALT", "TARMAC", "ROAD", "STRADA")
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


def _invisible(material: Kn5Material) -> bool:
    """A material AC draws fully transparent: its `alpha` property at zero.
    Mods hide physics meshes they leave renderable this way; Monza 2022's
    216 (`01WALL`..., the road and run-off) wear `physics`, `ksPerPixelAlpha`
    with `alpha = 0` on a flat normal map, and drawn they painted every
    wall and run-off in the normal map's lavender."""
    try:
        return float(material.props.get("alpha", 1.0)) <= 0.01
    except (TypeError, ValueError):
        return False


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


def _is_kerb(names: str) -> bool:
    """Kerbs keep AC's texture whatever lies under them: the stripes are in
    it. Spa's `CURB_B` stands on road physics in places and was drawn as
    kit asphalt there and as grass where nothing was under it."""
    text = names.upper()
    return any(h in text for h in _KERB_HINTS)


def _hint_kit(names: str, shader: str) -> str | None:
    """A kit surface from the names, then the shader, for a mesh with no
    physics under it. The names come first: Spa's terrain is
    `grass-ext-shad` on `grass-ext.dds` with Kunos' *tarmac* shader, and
    shader-first painted the whole valley as asphalt."""
    # A terrazzo is a terrace, not terra.
    text = names.upper().replace("TERRAZZ", "")
    if any(h in text for h in _GRAVEL_HINTS):
        return "gravel"
    if any(h in text for h in _SAND_HINTS):
        return "sand"
    if any(h in text for h in _GRASS_HINTS):
        return "grass"
    if any(h in text for h in _DIRT_HINTS):
        return "sand"
    if any(h in text for h in _ROAD_HINTS):
        return "road"
    # `ksMultilayer_objsp` is an object shader (railings, towers, stands)
    # and `ksMultilayer_fresnel*` is Kunos' tarmac: only the plain
    # multilayer family is terrain.
    if shader.startswith("ksMultilayer") and "objsp" not in shader:
        return "road" if "fresnel" in shader else "grass"
    return None


def _lies_flat(positions: np.ndarray, triangles: np.ndarray) -> bool:
    """Whether most of a mesh's area faces up. Only such a mesh can be a
    kit surface: a pit door whose material also paves the pit lane is not
    pit lane."""
    t = triangles.astype(np.int64)
    n = np.cross(positions[t[:, 1]] - positions[t[:, 0]], positions[t[:, 2]] - positions[t[:, 0]])
    area = np.linalg.norm(n, axis=1)
    if area.sum() <= 0.0:
        return False
    return float(area[n[:, 2] > 0.5 * area].sum()) >= 0.6 * float(area.sum())


@dataclass
class UnderVotes:
    """What physics lies under a mesh, sampled at its triangles' centres."""
    samples: int = 0
    close: int = 0
    #: Close samples per contact class.
    counts: np.ndarray = field(default_factory=lambda: np.zeros(5))
    #: Close samples per physics surface index.
    surfaces: dict[int, float] = field(default_factory=dict)


def _under_votes(positions: np.ndarray, triangles: np.ndarray, world: PhysicsWorld) -> UnderVotes:
    # Triangle centres, not vertices: a road ribbon's vertices all lie on
    # the road's edges, where the physics beneath is road or grass by a
    # coin toss, so a road chunk came out as grass.
    votes = UnderVotes()
    m = triangles.shape[0]
    if m == 0 or world.index.count == 0:
        return votes
    pick = np.linspace(0, m - 1, min(UNDER_SAMPLES, m)).astype(np.int64)
    p = positions[triangles[pick]].mean(axis=1)
    z, contact, surf = world.contacts_at(p[:, 0], p[:, 1], p[:, 2] + 0.5)
    close = np.isfinite(z) & (np.abs(z - p[:, 2]) < UNDER_TOLERANCE_M)
    votes.samples = int(len(pick))
    votes.close = int(close.sum())
    votes.counts = np.bincount(contact[close], minlength=5)[:5].astype(np.float64)
    for s in surf[close].tolist():
        votes.surfaces[int(s)] = votes.surfaces.get(int(s), 0.0) + 1.0
    return votes


def _decide(counts: np.ndarray, surfaces: dict[int, float],
            world: PhysicsWorld) -> tuple[int | None, str | None]:
    """The class holding 60% of the votes and its commonest surface key."""
    total = float(counts.sum())
    if total <= 0.0:
        return None, None
    best = int(np.argmax(counts))
    if counts[best] < total * 0.6:
        return None, None
    of_class = {s: n for s, n in surfaces.items() if world.surfaces[s].contact == best}
    key = world.surfaces[max(sorted(of_class), key=lambda s: of_class[s])].key if of_class else None
    return best, key


def _verdict(votes: UnderVotes, world: PhysicsWorld) -> tuple[int | None, str | None]:
    """A mesh's own verdict, or `(None, None)`. Three quarters must be on
    physics, not half: a fence or a hoarding has its whole bottom row on
    the grass and is not grass."""
    if votes.samples == 0 or votes.close < votes.samples * UNDER_MIN_SHARE:
        return None, None
    return _decide(votes.counts, votes.surfaces, world)


def _material_verdicts(entries: list[tuple[tuple[str, str], int, UnderVotes]],
                       world: PhysicsWorld) -> dict[tuple[str, str], tuple[int, str | None]]:
    """One verdict per AC material from those of its meshes that stand on
    physics. A material is one look, so a road chunk whose own samples were
    inconclusive (mostly off the physics mesh, say) is still road when the
    material's other chunks are, rather than falling to a name guess."""
    by_material: dict[tuple[str, str], list[tuple[int, UnderVotes]]] = {}
    for ident, tris, votes in entries:
        by_material.setdefault(ident, []).append((tris, votes))
    out: dict[tuple[str, str], tuple[int, str | None]] = {}
    for ident in sorted(by_material):
        members = by_material[ident]
        total = sum(t for t, _ in members)
        resolved = [(t, v) for t, v in members if _verdict(v, world)[0] is not None]
        if not resolved or sum(t for t, _ in resolved) < 0.25 * total:
            continue
        counts = np.zeros(5)
        surfaces: dict[int, float] = {}
        for t, v in resolved:
            # Weighted by the mesh's size, so a sliver cannot outvote a lap.
            w = t / max(v.close, 1)
            counts += v.counts * w
            for s, n in v.surfaces.items():
                surfaces[s] = surfaces.get(s, 0.0) + n * w
        best, key = _decide(counts, surfaces, world)
        if best is not None:
            out[ident] = (best, key)
    return out


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

    drawn = []
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
            if mesh.triangle_count == 0:
                continue
            material = kn.materials[mesh.material]
            if _invisible(material):
                report.dropped_hidden += 1
                continue
            # By material only when it is see-through: an opaque asphalt a
            # mod called `groove` is the road itself.
            if _SKIP_NAME.search(mesh.name) or (_SKIP_NAME.search(material.name)
                                                and _blend_for(material) != "opaque"):
                report.dropped_overlay += 1
                continue
            positions = frame.ac_points(mesh.world_positions())
            if not np.isfinite(positions).all():
                report.warnings.append(f"mesh {mesh.name!r} has non-finite vertices; left out")
                continue
            votes = _under_votes(positions, mesh.triangles.astype(np.int64), world)
            drawn.append((kn, mesh, material, positions, votes))
    by_material = _material_verdicts(
        [((kn.path.name, material.name), mesh.triangle_count, votes)
         for kn, mesh, material, _, votes in drawn if _blend_for(material) == "opaque"], world)

    for kn, mesh, material, positions, votes in drawn:
        normals = frame.ac_vectors(mesh.world_normals())
        blend = _blend_for(material)
        under, under_key = by_material.get((kn.path.name, material.name)) or _verdict(votes, world)
        kit: str | None = None
        names = f"{mesh.name} {material.name} {material.texture('txDiffuse') or ''}"
        if textures_mode == "kit" and blend == "opaque" and not _is_kerb(names) \
                and _lies_flat(positions, mesh.triangles):
            if under in (CONTACT_ROAD,):
                kit = "road"
            elif under == CONTACT_PIT_LANE:
                kit = "pit_lane"
            elif under in (CONTACT_RUNOFF, CONTACT_OFF):
                kit = ground_set_for(under_key or "", under)
            elif under is None:
                kit = _hint_kit(names, material.shader)
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
            collision = _verdict(votes, world)[0] is not None and blend == "opaque"
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
