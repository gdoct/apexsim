"""The drivable world: AC's physics meshes into the server's road, walls,
ground and (with `centerline.py`) curb sidecars.

AC finds its physics surfaces by name, across every kn5 the layout lists:
a mesh called `NNKEY...` (digits, then a `surfaces.ini` KEY as a
prefix) is a physics surface, whether or not it is drawn; `NNWALL...` is a
wall. Kunos keep them in a separate, non-renderable kn5; mods often draw
them as well. This does the same.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

import numpy as np

from .frame import Frame, ac_to_world
from .ini import Surface
from .kn5 import Kn5File, Kn5Mesh
from .sidecars import (CONTACT_CURB, CONTACT_OFF, CONTACT_PIT_LANE, CONTACT_ROAD, CONTACT_RUNOFF,
                       WALL_ARMCO, WALL_CONCRETE, WALL_TIRES)
from .trigrid import TriangleIndex

# Any number of digits: the Nordschleife numbers its last 2 km `100TRM-NRM`
# to `111TRM-NRM`.
PHYSICS_NAME = re.compile(r"^\d+([A-Za-z].*)$")

#: The grip each contact class already carries on the server
#: (`TrackSurface` defaults and `physics::RUNOFF_GRIP_FACTOR`); AC's friction
#: is expressed relative to the track's own asphalt and divided by these, so
#: the sim ends up at AC's number.
CLASS_GRIP = {CONTACT_ROAD: 1.0, CONTACT_CURB: 0.85, CONTACT_RUNOFF: 0.95, CONTACT_OFF: 0.6,
              CONTACT_PIT_LANE: 1.0}

_CURB_HINTS = ("KERB", "CURB", "KURB", "CRB")
_OFF_HINTS = ("GRASS", "GRS", "SAND", "GRAVEL", "GRVL", "DIRT", "EARTH", "MUD", "SNOW", "CARPET",
              "GRILL", "ASTRO", "TURF")
_TIRE_HINTS = ("TYRE", "TIRE", "TECPRO", "GOMME", "TWALL")
_CONCRETE_HINTS = ("CONC", "CEMENT", "JERSEY", "BLOCK", "STONE", "BRICK", "MURO")


def surface_key_for(mesh_name: str, surfaces: dict[str, Surface]) -> tuple[str | None, bool]:
    """`(key, is_wall)` for a mesh name, or `(None, False)` for a mesh that
    is not physics. The longest KEY the name's remainder starts with wins."""
    m = PHYSICS_NAME.match(mesh_name)
    if not m:
        return None, False
    rem = m.group(1).upper()
    if rem.startswith("WALL"):
        return "WALL", True
    # Some tracks write the digit into the key itself (`KEY=1ASPHALT`), so
    # a key may prefix the whole name as well as what follows the digits.
    full = mesh_name.upper()
    best = None
    for key in surfaces:
        if (rem.startswith(key) or full.startswith(key)) and (best is None or len(key) > len(best)):
            best = key
    return best, False


def contact_for(surface: Surface) -> int:
    k = surface.key.lstrip("0123456789")
    if surface.pit_lane or k.startswith("PIT"):
        return CONTACT_PIT_LANE
    if any(h in k for h in _CURB_HINTS):
        return CONTACT_CURB
    if any(h in k for h in _OFF_HINTS):
        return CONTACT_OFF
    if not surface.valid_track:
        return CONTACT_RUNOFF if surface.friction >= 0.85 else CONTACT_OFF
    return CONTACT_ROAD


def ground_set_for(key: str, contact: int) -> str:
    """Which of ApexSim's ground texture sets a surface key is drawn with."""
    k = key.upper()
    if any(h in k for h in ("GRASS", "GRS", "TURF", "ASTRO", "CARPET")):
        return "grass"
    if any(h in k for h in ("SAND", "DIRT", "EARTH", "MUD", "DUNE")):
        return "sand"
    if any(h in k for h in ("GRAVEL", "GRVL", "GHIAIA", "STONES")):
        return "gravel"
    if any(h in k for h in ("CONC", "CNC", "CEMENT")):
        return "concrete"
    if contact in (CONTACT_ROAD, CONTACT_PIT_LANE, CONTACT_RUNOFF):
        return "asphalt"
    return "grass"


def wall_kind_for(mesh_name: str, material_name: str) -> int:
    text = (mesh_name + " " + material_name).upper()
    if any(h in text for h in _TIRE_HINTS):
        return WALL_TIRES
    if any(h in text for h in _CONCRETE_HINTS):
        return WALL_CONCRETE
    return WALL_ARMCO


@dataclass
class RoadSurfaceSpec:
    key: str
    contact: int
    friction: float
    valid_track: bool
    pit_lane: bool
    ac_friction: float
    triangles: int = 0


@dataclass
class PhysicsWorld:
    """Every physics triangle of the layout in the track frame."""
    #: (n, 3) metres.
    vertices: np.ndarray
    #: (m, 3) int64.
    triangles: np.ndarray
    #: (m,) index into `surfaces`.
    triangle_surface: np.ndarray
    surfaces: list[RoadSurfaceSpec]
    #: (m,) contact class per triangle.
    triangle_contact: np.ndarray
    index: TriangleIndex
    #: (w, 6) wall segments x0 y0 x1 y1 z height and (w,) kinds.
    wall_segments: np.ndarray
    wall_kinds: np.ndarray
    #: Mesh names that matched the physics pattern but no surface key.
    unknown_keys: dict[str, int] = field(default_factory=dict)
    physics_meshes: int = 0
    wall_meshes: int = 0
    road_friction: float = 1.0

    def contacts_at(self, xs, ys, ceiling):
        """`(z, contact, surface_index)` under each point; contact -1 where nothing."""
        z, t = self.index.query(xs, ys, ceiling)
        contact = np.where(t >= 0, self.triangle_contact[np.maximum(t, 0)], -1)
        surf = np.where(t >= 0, self.triangle_surface[np.maximum(t, 0)], -1)
        return z, contact, surf


def collect_physics(kn5s: list[Kn5File], surfaces: dict[str, Surface], frame: Frame,
                    warnings: list[str]) -> PhysicsWorld:
    """Walk every kn5's meshes for physics names, convert them into the track
    frame and index them."""
    used: dict[str, RoadSurfaceSpec] = {}
    vert_chunks: list[np.ndarray] = []
    tri_chunks: list[np.ndarray] = []
    surf_chunks: list[np.ndarray] = []
    walls: list[np.ndarray] = []
    wall_kinds: list[np.ndarray] = []
    unknown: dict[str, int] = {}
    offset = 0
    physics_meshes = 0
    wall_meshes = 0
    area_by_key: dict[str, float] = {}

    for kn in kn5s:
        for mesh in kn.meshes:
            key, is_wall = surface_key_for(mesh.name, surfaces)
            if is_wall:
                wall_meshes += 1
                seg, kinds = _wall_segments(mesh, kn, frame)
                if seg.size:
                    walls.append(seg)
                    wall_kinds.append(kinds)
                continue
            if key is None:
                if PHYSICS_NAME.match(mesh.name):
                    rem = PHYSICS_NAME.match(mesh.name).group(1).upper()
                    short = re.split(r"[^A-Z\-]", rem, 1)[0] or rem[:12]
                    unknown[short] = unknown.get(short, 0) + mesh.triangle_count
                continue
            if mesh.triangle_count == 0:
                continue
            physics_meshes += 1
            spec = used.get(key)
            if spec is None:
                s = surfaces[key]
                contact = contact_for(s)
                spec = RoadSurfaceSpec(key=key, contact=contact, friction=1.0, valid_track=s.valid_track,
                                       pit_lane=s.pit_lane or contact == CONTACT_PIT_LANE,
                                       ac_friction=s.friction)
                used[key] = spec
            p = frame.ac_points(mesh.world_positions())
            if not np.isfinite(p).all():
                warnings.append(f"physics mesh {mesh.name!r} has non-finite vertices; left out")
                continue
            t = mesh.triangles.astype(np.int64)
            # Drop degenerate corners (a repeated index) up front.
            keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
            t = t[keep]
            if t.shape[0] == 0:
                continue
            spec.triangles += int(t.shape[0])
            a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]
            area_by_key[key] = area_by_key.get(key, 0.0) + float(
                np.abs((b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])).sum() * 0.5)
            vert_chunks.append(p)
            tri_chunks.append(t + offset)
            surf_chunks.append(np.full(t.shape[0], list(used).index(key), dtype=np.int64))
            offset += p.shape[0]

    surfaces_list = list(used.values())
    # AC's friction is absolute; the server's is a multiplier on the class's
    # own grip. The track's asphalt (the road surface with the most area) is
    # the reference: it ends up at exactly 1.0 x the road's grip.
    road_keys = [s.key for s in surfaces_list if s.contact == CONTACT_ROAD]
    if road_keys:
        ref_key = max(road_keys, key=lambda k: area_by_key.get(k, 0.0))
        road_friction = used[ref_key].ac_friction or 1.0
    else:
        road_friction = 1.0
        warnings.append("no road-class physics surface was found; friction is relative to 1.0")
    for s in surfaces_list:
        s.friction = round((s.ac_friction / road_friction) / CLASS_GRIP[s.contact], 4)

    if vert_chunks:
        vertices = np.concatenate(vert_chunks)
        triangles = np.concatenate(tri_chunks)
        triangle_surface = np.concatenate(surf_chunks)
    else:
        vertices = np.zeros((0, 3))
        triangles = np.zeros((0, 3), dtype=np.int64)
        triangle_surface = np.zeros(0, dtype=np.int64)
    contact_table = np.array([s.contact for s in surfaces_list] or [CONTACT_ROAD], dtype=np.int64)
    triangle_contact = contact_table[triangle_surface] if triangle_surface.size else np.zeros(0, dtype=np.int64)
    index = TriangleIndex(vertices, triangles)
    if walls:
        segs = np.concatenate(walls)
        kinds = np.concatenate(wall_kinds)
        segs, kinds = _dedupe_segments(segs, kinds)
    else:
        segs = np.zeros((0, 6))
        kinds = np.zeros(0, dtype=np.int64)
    if unknown:
        listed = ", ".join(f"{k} ({v} tris)" for k, v in sorted(unknown.items(), key=lambda kv: -kv[1])[:8])
        warnings.append(f"physics-named meshes with no surfaces.ini key were ignored: {listed}")
    return PhysicsWorld(vertices=vertices, triangles=triangles, triangle_surface=triangle_surface,
                        surfaces=surfaces_list, triangle_contact=triangle_contact, index=index,
                        wall_segments=segs, wall_kinds=kinds, unknown_keys=unknown,
                        physics_meshes=physics_meshes, wall_meshes=wall_meshes,
                        road_friction=road_friction)


def _wall_segments(mesh: Kn5Mesh, kn: Kn5File, frame: Frame) -> tuple[np.ndarray, np.ndarray]:
    """A wall mesh's steep faces as ground-plane segments with a base height
    and a height: each triangle's footprint is the span of its corners along
    its longest horizontal edge; near-horizontal faces (a wall's cap, a tyre
    stack's top) are left out."""
    p = frame.ac_points(mesh.world_positions())
    t = mesh.triangles.astype(np.int64)
    a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]
    n = np.cross(b - a, c - a)
    length = np.linalg.norm(n, axis=1)
    ok = length > 1e-9
    steep = ok & (np.abs(n[:, 2]) / np.where(ok, length, 1.0) < 0.7)
    if not steep.any():
        return np.zeros((0, 6)), np.zeros(0, dtype=np.int64)
    a, b, c = a[steep], b[steep], c[steep]
    corners = np.stack([a, b, c], axis=1)  # (k, 3, 3)
    edges = np.stack([b - a, c - b, a - c], axis=1)[:, :, :2]
    lengths = np.linalg.norm(edges, axis=2)
    longest = np.argmax(lengths, axis=1)
    d = edges[np.arange(len(edges)), longest]
    dl = np.linalg.norm(d, axis=1)
    keep = dl > 0.01
    if not keep.any():
        return np.zeros((0, 6)), np.zeros(0, dtype=np.int64)
    corners, d, dl = corners[keep], d[keep], dl[keep]
    d = d / dl[:, None]
    along = (corners[:, :, :2] * d[:, None, :]).sum(axis=2)  # (k, 3)
    lo = np.argmin(along, axis=1)
    hi = np.argmax(along, axis=1)
    rows = np.arange(len(corners))
    p0 = corners[rows, lo, :2]
    p1 = corners[rows, hi, :2]
    z0 = corners[:, :, 2].min(axis=1)
    z1 = corners[:, :, 2].max(axis=1)
    height = z1 - z0
    seg = np.column_stack([p0, p1, z0, height])
    seg = seg[height > 0.05]
    kind = wall_kind_for(mesh.name, kn.materials[mesh.material].name)
    return seg, np.full(seg.shape[0], kind, dtype=np.int64)


def _dedupe_segments(segs: np.ndarray, kinds: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Two triangles of one quad give the same footprint; keep one per
    (endpoints to the centimetre), the tallest, lowest base, in a stable order."""
    if segs.shape[0] == 0:
        return segs, kinds
    a = np.rint(segs[:, 0:2] * 100).astype(np.int64)
    b = np.rint(segs[:, 2:4] * 100).astype(np.int64)
    swap = (a[:, 0] > b[:, 0]) | ((a[:, 0] == b[:, 0]) & (a[:, 1] > b[:, 1]))
    lo = np.where(swap[:, None], b, a)
    hi = np.where(swap[:, None], a, b)
    keyed = np.column_stack([lo, hi])
    order = np.lexsort((-segs[:, 5], segs[:, 4], keyed[:, 3], keyed[:, 2], keyed[:, 1], keyed[:, 0]))
    keyed = keyed[order]
    first = np.ones(len(order), dtype=bool)
    first[1:] = np.any(np.diff(keyed, axis=0) != 0, axis=1)
    picked = order[first]
    out = segs[picked].copy()
    out[:, 0:2] = keyed[first][:, 0:2] / 100.0
    out[:, 2:4] = keyed[first][:, 2:4] / 100.0
    return out, kinds[picked]


def ground_heightfield(world: PhysicsWorld, cell_m: float = 4.0, margin_m: float = 60.0
                       ) -> tuple[float, float, np.ndarray]:
    """The highest physics surface on a `cell_m` grid over the physics
    bounds plus `margin_m`, holes filled from their neighbours: `(origin_x,
    origin_y, heights (rows, cols))`."""
    lo, hi = world.index.bounds()
    origin = lo[:2] - margin_m
    span = hi[:2] + margin_m - origin
    cols = int(np.ceil(span[0] / cell_m)) + 1
    rows = int(np.ceil(span[1] / cell_m)) + 1
    xs = origin[0] + np.arange(cols) * cell_m
    ys = origin[1] + np.arange(rows) * cell_m
    gx, gy = np.meshgrid(xs, ys)
    z, _, _ = world.contacts_at(gx.reshape(-1), gy.reshape(-1), hi[2] + 10.0)
    h = z.reshape(rows, cols)
    h = _fill_holes(h)
    return float(origin[0]), float(origin[1]), h


def _fill_holes(h: np.ndarray) -> np.ndarray:
    """Fill NaN cells from the mean of their filled neighbours, outward, so
    the grass past the last physics mesh carries the nearest height on."""
    h = h.copy()
    if np.all(np.isnan(h)):
        return np.zeros_like(h)
    for _ in range(max(h.shape) + 2):
        missing = np.isnan(h)
        if not missing.any():
            break
        padded = np.pad(h, 1, mode="edge")
        acc = np.zeros_like(h)
        cnt = np.zeros_like(h)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nb = padded[1 + dy:1 + dy + h.shape[0], 1 + dx:1 + dx + h.shape[1]]
                ok = ~np.isnan(nb)
                acc[ok] += nb[ok]
                cnt[ok] += 1
        fill = missing & (cnt > 0)
        h[fill] = acc[fill] / cnt[fill]
    h[np.isnan(h)] = 0.0
    return h


def triangle_contact_name(contact: int) -> str:
    from .sidecars import CONTACT_NAMES
    return CONTACT_NAMES.get(int(contact), "none")
