"""Survey an AC track for what stands beside its road: the stands, the
buildings, what crosses over the road, masts and towers, where the trees
are, and the pit lane. `scripts/ac_layout.py` turns the survey into
corrections for a native circuit's layout dossier.

Nothing here keeps AC geometry: an object comes out as its footprint (a
convex ring in the track frame), its height and what it was classified as,
and a tree as a point. The frame is the importer's own (origin on the
start line, +X along the AI line there), so a survey lines up with an
`ac_import.py` import of the same layout.

How objects are found:

- Every drawn kn5 mesh (the importer's selection: renderable, LOD 0, not an
  `AC_*` logic object, not an overlay, not invisible) is split into its
  connected pieces, with vertices welded at 1 cm so a box whose faces were
  split at the UV seams stays one piece.
- Tree cards (`ksTree`, or a masked material named like a tree) become
  points; crowd cards mark the stand they sit in; other masked or
  translucent materials (fences, nets, glass, signs) are left out.
- A piece that lies flat and low (relief under 1.5 m, not lifted off the
  ground) is ground: road, paint, terrain. Terrain sheets are also left out
  by size and by the importer's own ground-name hints.
- Pieces whose footprints overlap are merged into one object (a roof, its
  walls and its seats), and each object is classified from its size, its
  height above AC's physics ground and the names of its meshes and
  materials.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import ai, centerline, ini, kn5, physics
from .frame import Frame
from .scene import _SKIP_NAME, _blend_for, _hint_kit, _invisible

SURVEY_VERSION = 1

#: Pieces flatter than this, and not lifted off the ground, are ground.
FLAT_RELIEF_M = 1.5
#: Underside this far above the ground: a deck, a roof or a gantry, kept
#: however flat it is.
LIFTED_M = 2.5
#: A single piece bigger than this is a terrain sheet, not a building.
TERRAIN_AREA_M2 = 50_000.0
#: Pieces smaller than this footprint only count when tall (a mast).
MIN_PIECE_AREA_M2 = 6.0
MAST_MIN_HEIGHT_M = 10.0
#: Pieces whose outlines come within this distance are one object.
MERGE_GAP_M = 0.5
#: Classification thresholds (see `classify`).
MIN_STRUCTURE_HEIGHT_M = 2.5
MIN_STRUCTURE_AREA_M2 = 250.0
MIN_STAND_LENGTH_M = 12.0
MAX_OBJECT_LENGTH_M = 350.0
CROSSING_CLEARANCE_M = 4.0
FOOTBRIDGE_MAX_DECK_M = 6.0
MAST_MAX_SIDE_M = 4.0
FLOODLIGHT_MIN_HEIGHT_M = 15.0
PIT_COMPLEX_M = 30.0

#: Seating by name. Not a bare `STAND`: Kunos' standing spectators are
#: `ppl-stand` on `people_stand.dds`, and a bare match turned every group of
#: them round Zandvoort's paddock into a grandstand.
_STAND_HINTS = re.compile(r"(TRIBUN|GRANDST|GSTAND|G_STAND|BLEACH|GRADIN|SEDUT|SEATING|SEATS|SITZPL|POSTI_A_SEDERE)",
                          re.I)
#: Parked cars, vehicles, tents and site clutter: a car park's hundreds of
#: cars touch each other and the stand beside them, and merged they made
#: Spa's main grandstand one 340 m "building". The kit dresses car parks
#: and camp sites from the dossier's areas instead.
_CLUTTER_HINTS = re.compile(r"(PARKING|PRK|VEHICLE|TRUCK|CAMPER|CARAVAN|TRACTOR|GRU_|CRANE|GAZEBO|TENT|"
                            r"AMBULANCE|CARRATT|TOILET|PORTALOO|BUS_|CAR_\d|CARS|SCOOTER|BIKE|MARSHAL|"
                            r"KERB|CURB|CORDOL|KRB|CONE|BALE|POLE_CAMERA)", re.I)
_CROWD_HINTS = re.compile(r"(CROWD|PEOPLE|PPL|PUBLIC|PUBBLICO|SPECT|FANS|AUDIENCE|ZUSCH|PERSON)", re.I)
#: Spectators standing on a bank or in the paddock: no seating under them.
_STANDING_HINTS = re.compile(r"(PPL.?STAND|PEOPLE.?STAND|STANDING)", re.I)
#: A stand needs this many seated spectator cards inside its outline when
#: nothing in its names says it is one.
MIN_CROWD_CARDS = 20
#: Terrain sculpted as objects (Kunos' Red Bull Ring hills are `mount`).
_TERRAIN_HINTS = re.compile(r"(MOUNT|ROCK|STONES|CLIFF|TRAVERT|EMBANK|SLOPE|HILL|BANK_|DUNE|RUMBLE)", re.I)
PROFILE_SAMPLES = 400
#: An object whose pieces cover less of its hull than this is a loose
#: cluster of things that touch, and is regrouped by real overlap.
MIN_FILL = 0.45
STRICT_OVERLAP = 0.25
#: The seating test (`_seating_profile`).
SEAT_MIN_DEPTH_M = 8.0
SEAT_MAX_DEPTH_M = 70.0
SEAT_MIN_RISE_M = 4.0
SEAT_MIN_TREND = 0.7
SEAT_ROAD_RANGE_M = 120.0
_TREE_HINTS = re.compile(r"(TREE|ALBER|PINE|PINO|FIR|SPRUCE|OAK|BIRCH|POPLAR|PIOPP|CYPRES|CIPRESS|PALM|"
                         r"BUSH|CESPUG|FOLIAG|LEAF|LEAVES|FOREST|BOSCO|WOOD|CONIFER|BAUM|BOOM)", re.I)
_NEEDLE_HINTS = re.compile(r"(PINE|PINO|FIR|SPRUCE|CONIFER|CYPRES|CIPRESS|LARCH|TANNE|FICHTE|ABETE)", re.I)
_BROAD_HINTS = re.compile(r"(OAK|BIRCH|POPLAR|PIOPP|BEECH|MAPLE|PLANE|ELM|LINDEN|PALM|BROAD|QUERC)", re.I)
_BARRIER_HINTS = re.compile(r"(ARMCO|GUARD|RAIL|FENCE|NET|TYRE|TIRE|GOMME|TECPRO|BARRIER|WALL|MURO|JERSEY|"
                            r"CABLE|POLE_FENCE|RECINZ)", re.I)
_ROOF_HINTS = re.compile(r"(ROOF|TETTO|CANOPY|DACH|COVER)", re.I)
_LIGHT_HINTS = re.compile(r"(LIGHT|LAMP|FLOOD|MAST|LUCE|FARO|PYLON|TOWERLIGHT)", re.I)
_WHEEL_HINTS = re.compile(r"(FERRIS|BIGWHEEL|BIG_WHEEL|RIESENRAD|RUOTA_PANOR|WHEEL_PARK)", re.I)
_SCREEN_HINTS = re.compile(r"(SCREEN|MAXISCH|VIDEOWALL|LEDWALL|JUMBO)", re.I)
_TOWER_HINTS = re.compile(r"(TOWER|TORRE|TURM|CONTROL)", re.I)
_CAMERA_HINTS = re.compile(r"(CAMERA|CAM_TOWER|TV_TOWER)", re.I)


class SurveyError(Exception):
    pass


@dataclass
class Piece:
    ring: np.ndarray     # (k, 2) convex hull, counter-clockwise, not closed
    area: float
    zmin: float
    zmax: float
    ground: float
    names: str
    #: Up to `PROFILE_SAMPLES` of its vertices (x, y, z), for the seating test.
    sample: np.ndarray | None = None


@dataclass
class SurveyObject:
    cls: str             # stand | structure | crossing | landmark | pit_complex | ignored
    ring: np.ndarray
    height_m: float
    zmin: float
    ground: float
    reason: str
    kind: str | None = None      # landmark / crossing kind
    covered: bool = False
    station_m: float | None = None   # crossings: where on the AC lap
    deck_m: float | None = None
    names: str = ""

    def to_json(self) -> dict:
        out = {
            "cls": self.cls,
            "ring": [[round(float(x), 2), round(float(y), 2)] for x, y in self.ring],
            "height_m": round(self.height_m, 1),
            "reason": self.reason,
        }
        for k in ("kind", "station_m", "deck_m"):
            v = getattr(self, k)
            if v is not None:
                out[k] = round(v, 1) if isinstance(v, float) else v
        if self.covered:
            out["covered"] = True
        if self.names:
            out["names"] = self.names[:160]
        return out


@dataclass
class Survey:
    folder: str
    layout: str
    centre: np.ndarray            # (m, 3) the road's middle every spine step, track frame
    width_left: np.ndarray        # (m,) road half widths there
    width_right: np.ndarray
    lap_m: float
    objects: list[SurveyObject]
    trees: np.ndarray             # (n, 3): x, y, leaf (0 mixed, 1 broad, 2 needle)
    pit_lane: np.ndarray | None   # (k, 2) in course order
    pit_boxes: int
    files: dict[str, int]
    warnings: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {
            "version": SURVEY_VERSION,
            "folder": self.folder,
            "layout": self.layout,
            "lap_m": round(self.lap_m, 1),
            "centre": [[round(float(x), 2), round(float(y), 2), round(float(z), 2)] for x, y, z in self.centre],
            "width_left": [round(float(w), 2) for w in self.width_left],
            "width_right": [round(float(w), 2) for w in self.width_right],
            "objects": [o.to_json() for o in self.objects],
            "trees": [[round(float(x), 1), round(float(y), 1), int(l)] for x, y, l in self.trees],
            "pit_lane": None if self.pit_lane is None else
            [[round(float(x), 2), round(float(y), 2)] for x, y in self.pit_lane],
            "pit_boxes": self.pit_boxes,
            "files": self.files,
            "warnings": self.warnings,
        }


# ---------------------------------------------------------------- geometry


def convex_hull(pts: np.ndarray) -> np.ndarray:
    """Andrew's monotone chain; counter-clockwise, not closed."""
    p = np.asarray(pts, dtype=np.float64)[:, :2]
    if len(p) > 256:
        # The extreme points in 180 directions: the hull of those is the
        # hull to within millimetres on anything we survey, and keeps the
        # Python loop below short on a 50k-vertex building.
        ang = np.linspace(0.0, 2 * math.pi, 180, endpoint=False)
        dirs = np.stack([np.cos(ang), np.sin(ang)], axis=1)
        p = p[np.unique((p @ dirs.T).argmax(axis=0))]
    p = np.unique(np.round(p, 3), axis=0)
    if len(p) <= 2:
        return p
    p = p[np.lexsort((p[:, 1], p[:, 0]))]

    def half(points):
        out: list[np.ndarray] = []
        for q in points:
            while len(out) >= 2 and _cross(out[-2], out[-1], q) <= 0:
                out.pop()
            out.append(q)
        return out

    lower = half(p)
    upper = half(p[::-1])
    return np.array(lower[:-1] + upper[:-1])


def _cross(o, a, b) -> float:
    return float((a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]))


def ring_area(ring: np.ndarray) -> float:
    if len(ring) < 3:
        return 0.0
    x, y = ring[:, 0], ring[:, 1]
    return float(abs(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))) / 2)


def convex_overlap(a: np.ndarray, b: np.ndarray, gap: float) -> bool:
    """Whether two convex rings come within `gap` of each other
    (separating-axis test with the gap as slack)."""
    for ring in (a, b):
        n = len(ring)
        if n < 2:
            continue
        for i in range(n):
            e = ring[(i + 1) % n] - ring[i]
            ln = math.hypot(e[0], e[1])
            if ln < 1e-6:
                continue
            axis = np.array([-e[1], e[0]]) / ln
            pa, pb = a @ axis, b @ axis
            if pa.max() + gap < pb.min() or pb.max() + gap < pa.min():
                return False
    if len(a) < 3 or len(b) < 3:
        # Points and segments have no edges for the test above to separate
        # on in every direction; fall back to their bounding boxes.
        return not ((a.max(0) + gap < b.min(0)).any() or (b.max(0) + gap < a.min(0)).any())
    return True


def points_in_ring(pts: np.ndarray, ring: np.ndarray) -> np.ndarray:
    """Even-odd test for many points."""
    x, y = pts[:, 0], pts[:, 1]
    inside = np.zeros(len(pts), dtype=bool)
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i]
        xj, yj = ring[j]
        cross = ((yi > y) != (yj > y)) & (x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-12) + xi)
        inside ^= cross
        j = i
    return inside


def components(triangles: np.ndarray, positions: np.ndarray) -> list[np.ndarray]:
    """Vertex index sets of the mesh's connected pieces, with vertices
    welded at 1 cm (a box split at its UV seams stays one piece)."""
    if len(triangles) == 0:
        return []
    key = np.round(positions * 100.0).astype(np.int64)
    _, weld = np.unique(key, axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    t = weld[triangles]
    parent = np.arange(int(weld.max()) + 1)
    edges = np.concatenate([t[:, [0, 1]], t[:, [1, 2]]])
    while True:
        # Hook every edge's larger root onto its smaller, then compress.
        a, b = parent[edges[:, 0]], parent[edges[:, 1]]
        diff = a != b
        if not diff.any():
            break
        lo, hi = np.minimum(a[diff], b[diff]), np.maximum(a[diff], b[diff])
        np.minimum.at(parent, hi, lo)
        while True:
            q = parent[parent]
            if np.array_equal(q, parent):
                break
            parent = q
    used = np.unique(triangles.reshape(-1))
    labels = parent[weld[used]]
    order = np.argsort(labels, kind="stable")
    cuts = np.flatnonzero(np.diff(labels[order])) + 1
    return [used[g] for g in np.split(order, cuts) if len(g)]


# ----------------------------------------------------------------- survey


def survey_layout(track_dir: Path, layout_name: str | None, warnings: list[str] | None = None) -> Survey:
    """Read one AC layout and survey it. `layout_name` is the layout's
    subfolder (`layout_gp`), or None / "" for a single-layout track."""
    from .cli import _markers, find_system_surfaces, source_crc_of

    warnings = warnings if warnings is not None else []
    layouts = ini.find_layouts(track_dir)
    want = layout_name or ""
    layout = next((l for l in layouts if l.name == want), None)
    if layout is None:
        raise SurveyError(f"{track_dir.name}: no layout {want!r} (has {[l.name for l in layouts]})")

    files: dict[str, int] = {}
    kn5s: list[kn5.Kn5File] = []
    for fn in ini.model_files(layout):
        path = layout.track_dir / fn
        if not path.is_file():
            warnings.append(f"{fn} is listed but missing")
            continue
        kn5s.append(kn5.read_kn5(path, textures=False))
        files[fn] = source_crc_of(path)
    fast_lane_path = layout.ai_dir / "fast_lane.ai"
    fast_lane = ai.read_ai(fast_lane_path)
    files[str(fast_lane_path.relative_to(layout.track_dir)).replace("\\", "/")] = source_crc_of(fast_lane_path)

    system_surfaces = find_system_surfaces(track_dir)
    surfaces_ini = layout.find_data("surfaces.ini")
    surfaces = ini.read_surfaces([p for p in (surfaces_ini, system_surfaces) if p])
    for key, s in ini.SYSTEM_SURFACES.items():
        surfaces.setdefault(key, s)

    markers = _markers(kn5s)
    frame, _ = centerline.make_frame(fast_lane.positions, True, markers.get("AC_TIME_0"), warnings)
    world = physics.collect_physics(kn5s, surfaces, frame, [])
    z0, _, _ = world.contacts_at(np.zeros(1), np.zeros(1), np.array([5.0]))
    if np.isfinite(z0[0]):
        frame = frame.with_origin_z(frame.origin[2] + float(z0[0]))
    world = physics.collect_physics(kn5s, surfaces, frame, warnings)
    spine = centerline.build_spine(fast_lane.positions, frame, True)
    measured = centerline.measure(spine, world, fast_lane.side_left, fast_lane.side_right, warnings)
    centre = measured.centre

    pit_lane = None
    pit_path = layout.ai_dir / "pit_lane.ai"
    if pit_path.is_file():
        try:
            pit = ai.read_ai(pit_path)
            pl = frame.ac_points(pit.positions)[:, :2]
            files[str(pit_path.relative_to(layout.track_dir)).replace("\\", "/")] = source_crc_of(pit_path)
            pit_lane = _resample(pl, 6.0)
        except ai.AiError as e:
            warnings.append(f"pit_lane.ai unreadable: {e}")
    else:
        warnings.append("no ai/pit_lane.ai")
    pit_boxes = sum(1 for k in markers if k.startswith("AC_PIT_"))
    if pit_lane is not None:
        trimmed = trim_pit_lane(pit_lane, centre[:, :2], measured.width_left, measured.width_right)
        if trimmed is None:
            warnings.append("pit_lane.ai never leaves the road; no pit lane")
        pit_lane = trimmed

    pieces, trees, crowd = _collect_pieces(kn5s, frame, world, warnings)
    objects = []
    for group in _merge_pieces(pieces):
        if len(group) > 1 and _fill(group) < MIN_FILL:
            objects.extend(_merge_pieces(group, strict=True))
        else:
            objects.append(group)
    classified = [classify(o, centre, spine.lap_m, pit_lane, crowd) for o in objects]
    return Survey(folder=track_dir.name, layout=want, centre=centre,
                  width_left=np.asarray(measured.width_left, dtype=np.float64),
                  width_right=np.asarray(measured.width_right, dtype=np.float64), lap_m=float(spine.lap_m),
                  objects=classified, trees=trees, pit_lane=pit_lane, pit_boxes=pit_boxes,
                  files=files, warnings=warnings)


def _resample(pts: np.ndarray, step: float) -> np.ndarray:
    seg = np.hypot(*np.diff(pts, axis=0).T)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    if s[-1] <= step:
        return pts
    q = np.arange(0.0, s[-1], step)
    q = np.append(q, s[-1])
    return np.c_[np.interp(q, s, pts[:, 0]), np.interp(q, s, pts[:, 1])]


def trim_pit_lane(lane: np.ndarray, centre: np.ndarray, wl: np.ndarray, wr: np.ndarray) -> np.ndarray | None:
    """AC's pit spline is the AI's whole route through the pits: it starts
    and ends on the race track itself (Zandvoort's runs from the last
    corner, round Tarzan and back). Keep the longest run of it that is off
    the road, plus the node either side where it leaves and rejoins."""
    tangent = np.gradient(centre, axis=0)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-9)
    off = np.zeros(len(lane), dtype=bool)
    for i, p in enumerate(lane):
        d2 = ((centre - p) ** 2).sum(1)
        j = int(d2.argmin())
        lat = float(-tangent[j, 1] * (p[0] - centre[j, 0]) + tangent[j, 0] * (p[1] - centre[j, 1]))
        half = float(wl[j] if lat > 0 else wr[j])
        off[i] = abs(lat) > half + 0.5
    best = None
    i = 0
    while i < len(off):
        if not off[i]:
            i += 1
            continue
        j = i
        while j + 1 < len(off) and off[j + 1]:
            j += 1
        seg = lane[i:j + 1]
        length = float(np.hypot(*np.diff(seg, axis=0).T).sum()) if j > i else 0.0
        if best is None or length > best[0]:
            best = (length, i, j)
        i = j + 1
    if best is None or best[0] < 60.0:
        return None
    _, i, j = best
    return lane[max(i - 1, 0):min(j + 2, len(lane))]


def _leaf_of(names: str) -> int:
    if _NEEDLE_HINTS.search(names):
        return 2
    if _BROAD_HINTS.search(names):
        return 1
    return 0


def _ground_under(world, xy: np.ndarray, ceiling: float) -> float:
    z, _, _ = world.contacts_at(xy[:, 0], xy[:, 1], np.full(len(xy), ceiling))
    z = z[np.isfinite(z)]
    return float(np.median(z)) if len(z) else float("nan")


def _collect_pieces(kn5s, frame: Frame, world, warnings: list[str]) -> tuple[list[Piece], np.ndarray, np.ndarray]:
    """The solid pieces, the trees (x, y, leaf) and the seated spectator
    cards (x, y). Cards are evidence, not geometry: merged in, a crowd
    spread over a bank stitched trucks, tents and trees into one object."""
    pieces: list[Piece] = []
    trees: list[tuple[float, float, int]] = []
    crowd_pts: list[tuple[float, float]] = []
    for kn in kn5s:
        for mesh in kn.meshes:
            if not mesh.renderable or not mesh.active or mesh.lod_in > 0.0:
                continue
            if mesh.name.upper().startswith("AC_") or mesh.triangle_count == 0:
                continue
            material = kn.materials[mesh.material]
            if _invisible(material) or _SKIP_NAME.search(mesh.name) or _SKIP_NAME.search(material.name):
                continue
            blend = _blend_for(material)
            tex = material.texture("txDiffuse") or ""
            names = " ".join([*mesh.path[-3:], mesh.name, material.name, tex])
            positions = frame.ac_points(mesh.world_positions())
            if not np.isfinite(positions).all():
                continue
            tris = mesh.triangles.astype(np.int64)
            is_tree = material.shader == "ksTree" or (blend != "opaque" and _TREE_HINTS.search(names)
                                                      and not _CROWD_HINTS.search(names))
            if is_tree:
                leaf = _leaf_of(names)
                for comp in components(tris, positions):
                    c = positions[comp]
                    trees.append((float(c[:, 0].mean()), float(c[:, 1].mean()), leaf))
                continue
            if blend != "opaque":
                if _CROWD_HINTS.search(names) and not _STANDING_HINTS.search(names):
                    for comp in components(tris, positions):
                        c = positions[comp]
                        crowd_pts.append((float(c[:, 0].mean()), float(c[:, 1].mean())))
                continue  # fences, nets, glass, signs, decals, spectators
            if _BARRIER_HINTS.search(names) and not _STAND_HINTS.search(names):
                continue  # the barrier pass lays our own
            if _CLUTTER_HINTS.search(names) and not _STAND_HINTS.search(names):
                continue
            if _TERRAIN_HINTS.search(names) and not _STAND_HINTS.search(names):
                continue
            kit = _hint_kit(names, material.shader)
            for comp in components(tris, positions):
                p = positions[comp]
                ring = convex_hull(p[:, :2])
                area = ring_area(ring)
                if area > TERRAIN_AREA_M2:
                    continue
                zmin, zmax = float(p[:, 2].min()), float(p[:, 2].max())
                probe = ring if len(ring) else p[:1, :2]
                probe = np.vstack([probe, probe.mean(0)[None, :]])
                ground = _ground_under(world, probe, zmin + 0.5)
                if not math.isfinite(ground):
                    ground = zmin
                relief = zmax - zmin
                lifted = zmin - ground >= LIFTED_M
                if relief < FLAT_RELIEF_M and not lifted:
                    continue
                if kit in ("grass", "sand", "gravel", "road") and not lifted:
                    continue
                tall = zmax - ground >= MAST_MIN_HEIGHT_M
                if area < MIN_PIECE_AREA_M2 and not tall:
                    continue
                step = max(len(p) // PROFILE_SAMPLES, 1)
                pieces.append(Piece(ring=ring, area=area, zmin=zmin, zmax=zmax, ground=ground, names=names,
                                    sample=p[::step][:PROFILE_SAMPLES].copy()))
    tree_arr = np.array(trees, dtype=np.float64).reshape(-1, 3)
    return pieces, tree_arr, np.array(crowd_pts, dtype=np.float64).reshape(-1, 2)


def _fill(group: list[Piece]) -> float:
    """How much of a group's hull its pieces cover, on a 1 m raster."""
    ring = convex_hull(np.vstack([p.ring for p in group if len(p.ring)]))
    area = ring_area(ring)
    if area <= 0.0 or len(ring) < 3:
        return 1.0
    step = max(1.0, math.sqrt(area / 20000.0))
    lo, hi = ring.min(0), ring.max(0)
    xs, ys = np.meshgrid(np.arange(lo[0], hi[0], step), np.arange(lo[1], hi[1], step))
    grid = np.c_[xs.ravel(), ys.ravel()]
    grid = grid[points_in_ring(grid, ring)]
    if len(grid) == 0:
        return 1.0
    covered = np.zeros(len(grid), dtype=bool)
    for p in group:
        if len(p.ring) < 3:
            continue
        plo, phi = p.ring.min(0), p.ring.max(0)
        box = ((grid >= plo) & (grid <= phi)).all(axis=1) & ~covered
        if box.any():
            idx = np.flatnonzero(box)
            covered[idx[points_in_ring(grid[idx], p.ring)]] = True
    return float(covered.mean())


def _overlap_share(a: np.ndarray, b: np.ndarray) -> float:
    """Overlap over the smaller outline, on a raster of the smaller one."""
    small, big = (a, b) if ring_area(a) <= ring_area(b) else (b, a)
    area = ring_area(small)
    if area <= 0.0:
        return 0.0
    step = max(0.5, math.sqrt(area / 400.0))
    lo, hi = small.min(0), small.max(0)
    xs, ys = np.meshgrid(np.arange(lo[0], hi[0] + step, step), np.arange(lo[1], hi[1] + step, step))
    grid = np.c_[xs.ravel(), ys.ravel()]
    grid = grid[points_in_ring(grid, small)]
    if len(grid) == 0:
        return 0.0
    return float(points_in_ring(grid, big).mean())


def _merge_pieces(pieces: list[Piece], strict: bool = False) -> list[list[Piece]]:
    """Union pieces whose outlines touch (or, `strict`, overlap by a
    quarter of the smaller), with a uniform-grid broad phase."""
    n = len(pieces)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    cell = 20.0
    boxes = [(p.ring.min(0) - MERGE_GAP_M, p.ring.max(0) + MERGE_GAP_M) if len(p.ring) else None for p in pieces]
    grid: dict[tuple[int, int], list[int]] = {}
    for i, b in enumerate(boxes):
        if b is None:
            continue
        lo, hi = (b[0] // cell).astype(int), (b[1] // cell).astype(int)
        # A piece spanning many cells is registered in each; huge ones are
        # rare (terrain is gone) so this stays linear in practice.
        for cx in range(lo[0], hi[0] + 1):
            for cy in range(lo[1], hi[1] + 1):
                grid.setdefault((cx, cy), []).append(i)
    tested: set[tuple[int, int]] = set()
    for members in grid.values():
        for ai_ in range(len(members)):
            i = members[ai_]
            for j in members[ai_ + 1:]:
                if (i, j) in tested:
                    continue
                tested.add((i, j))
                bi, bj = boxes[i], boxes[j]
                if (bi[1] < bj[0]).any() or (bj[1] < bi[0]).any():
                    continue
                if find(i) == find(j):
                    continue
                if not convex_overlap(pieces[i].ring, pieces[j].ring, MERGE_GAP_M):
                    continue
                if strict and (len(pieces[i].ring) < 3 or len(pieces[j].ring) < 3 or
                               _overlap_share(pieces[i].ring, pieces[j].ring) < STRICT_OVERLAP):
                    continue
                parent[find(i)] = find(j)
    groups: dict[int, list[Piece]] = {}
    for i in range(n):
        if boxes[i] is not None:
            groups.setdefault(find(i), []).append(pieces[i])
    return list(groups.values())


def _oriented_extent(ring: np.ndarray) -> tuple[float, float]:
    best = None
    m = len(ring)
    for i in range(m):
        e = ring[(i + 1) % m] - ring[i]
        ln = math.hypot(*e)
        if ln < 1e-6:
            continue
        u = e / ln
        p = ring @ np.array([[u[0], -u[1]], [u[1], u[0]]])
        ext = np.ptp(p, axis=0)
        if best is None or ext[0] * ext[1] < best[0] * best[1]:
            best = ext
    if best is None:
        ext = np.ptp(ring, axis=0) if len(ring) else np.zeros(2)
        best = ext
    return float(max(best)), float(min(best))


def classify(group: list[Piece], centre: np.ndarray, lap_m: float, pit_lane: np.ndarray | None,
             crowd: np.ndarray | None = None) -> SurveyObject:
    pts = np.vstack([p.ring for p in group if len(p.ring)])
    ring = convex_hull(pts)
    area = ring_area(ring)
    length, depth = _oriented_extent(ring) if len(ring) >= 3 else (float(np.ptp(pts, axis=0).max()), 0.0)
    zmin = min(p.zmin for p in group)
    zmax = max(p.zmax for p in group)
    ground = float(np.median([p.ground for p in group]))
    height = zmax - ground
    names = " ".join(sorted({p.names for p in group}))[:2000]
    seated = 0
    if crowd is not None and len(crowd) and len(ring) >= 3:
        lo, hi = ring.min(0), ring.max(0)
        box = crowd[((crowd >= lo) & (crowd <= hi)).all(axis=1)]
        seated = int(points_in_ring(box, ring).sum()) if len(box) else 0
    obj = SurveyObject(cls="ignored", ring=ring, height_m=height, zmin=zmin, ground=ground, reason="",
                       names=names)

    # Over the road? The spine points inside the outline, and how high the
    # outline's underside is above them.
    if len(ring) >= 3:
        inside = points_in_ring(centre[:, :2], ring)
        if inside.any():
            road_z = float(centre[inside, 2].max())
            # The pieces over the road (not the supports beside it) say how
            # high the underside is.
            over = [p for p in group if len(p.ring) >= 3 and points_in_ring(centre[inside, :2], p.ring).any()]
            under = min(p.zmin for p in over) if over else zmin
            idx = np.flatnonzero(inside)
            if under - road_z >= CROSSING_CLEARANCE_M:
                step = lap_m / max(len(centre), 1)
                obj.cls = "crossing"
                obj.deck_m = float(len(idx) * step)
                obj.station_m = float(idx[len(idx) // 2] * step)
                obj.kind = "footbridge" if obj.deck_m <= FOOTBRIDGE_MAX_DECK_M else "road"
                obj.reason = f"spans the road {under - road_z:.1f} m up, deck {obj.deck_m:.0f} m"
                return obj
            obj.reason = f"stands on the road (underside {under - road_z:.1f} m up)"
            return obj

    if length > MAX_OBJECT_LENGTH_M:
        obj.reason = f"{length:.0f} m long: merged scenery, not one object"
        return obj
    if depth <= MAST_MAX_SIDE_M and length <= MAST_MAX_SIDE_M:
        if height >= FLOODLIGHT_MIN_HEIGHT_M and _LIGHT_HINTS.search(names):
            obj.cls, obj.kind, obj.reason = "landmark", "floodlight", f"{height:.0f} m mast, light names"
            return obj
        obj.reason = f"slender {height:.0f} m object"
        return obj
    for pattern, kind in ((_WHEEL_HINTS, "big_wheel"), (_SCREEN_HINTS, "screen"), (_CAMERA_HINTS, "camera_tower")):
        if pattern.search(names) and height >= 6.0:
            obj.cls, obj.kind, obj.reason = "landmark", kind, f"{kind} by name"
            return obj
    if pit_lane is not None and len(pit_lane) > 1:
        near_pit = min(_distance_to_polyline(q, pit_lane) for q in (ring if len(ring) else pts)) < PIT_COMPLEX_M / 2 \
            or _distance_to_polyline(ring.mean(0) if len(ring) else pts.mean(0), pit_lane) < PIT_COMPLEX_M
    else:
        near_pit = False
    stand_named = bool(_STAND_HINTS.search(names))
    big_enough = height >= MIN_STRUCTURE_HEIGHT_M and area >= MIN_STRUCTURE_AREA_M2
    if near_pit and (big_enough or stand_named):
        obj.cls, obj.reason = "pit_complex", "beside the pit lane (the bake builds the pit complex)"
        return obj
    if (stand_named or seated >= MIN_CROWD_CARDS) and length >= MIN_STAND_LENGTH_M and height >= 2.0:
        obj.cls = "stand"
        obj.covered = bool(_ROOF_HINTS.search(names))
        obj.reason = ("named like a stand" if stand_named else f"{seated} seated spectators") + \
            f", {length:.0f} x {depth:.0f} m"
        return obj
    if length >= MIN_STAND_LENGTH_M and len(ring) >= 3:
        seat = _seating_profile(group, ring, centre[:, :2])
        if seat is not None:
            obj.cls = "stand"
            obj.covered = seat[1]
            obj.reason = f"rises {seat[0]:.0f} m away from the road, {length:.0f} x {depth:.0f} m"
            return obj
    if big_enough:
        if _TOWER_HINTS.search(names) and area <= 600.0 and height >= 15.0:
            obj.cls, obj.kind, obj.reason = "landmark", "tower", f"{height:.0f} m tower by name"
            return obj
        obj.cls = "structure"
        obj.reason = f"{length:.0f} x {depth:.0f} m, {height:.0f} m high"
        return obj
    obj.reason = f"too small ({area:.0f} m2, {height:.1f} m)"
    return obj


def _seating_profile(group: list[Piece], ring: np.ndarray, road: np.ndarray) -> tuple[float, bool] | None:
    """Whether an object is tiered seating facing the road: across its depth,
    from the side nearer the road, the height of its vertices climbs
    steadily (a building's is flat, a roof's level). Returns (rise, covered:
    something stands well over the front row) or None. Kunos name the Red
    Bull Ring's stands `concrete` and `Metal_new`, so names alone miss them."""
    pts = np.vstack([p.sample for p in group if p.sample is not None and len(p.sample)])
    if len(pts) < 40:
        return None
    # The box's axes: the long side, and across it.
    best = None
    m = len(ring)
    for i in range(m):
        e = ring[(i + 1) % m] - ring[i]
        ln = math.hypot(*e)
        if ln < 1e-6:
            continue
        u = e / ln
        proj = ring @ np.array([[u[0], -u[1]], [u[1], u[0]]])
        ext = np.ptp(proj, axis=0)
        if best is None or ext[0] * ext[1] < best[0]:
            best = (ext[0] * ext[1], u, ext)
    _, u, ext = best
    if ext[1] > ext[0]:
        u = np.array([-u[1], u[0]])
        ext = ext[::-1]
    depth = float(ext[1])
    if not SEAT_MIN_DEPTH_M <= depth <= SEAT_MAX_DEPTH_M:
        return None
    across = np.array([-u[1], u[0]])
    c = ring.mean(0)
    d_road = np.hypot(*(road - c).T)
    k = int(d_road.argmin())
    if d_road[k] > SEAT_ROAD_RANGE_M + depth:
        return None
    if float((road[k] - c) @ across) > 0:
        across = -across        # +across points away from the road
    t = (pts[:, :2] - c) @ across
    t0, t1 = t.min(), t.max()
    if t1 - t0 < SEAT_MIN_DEPTH_M:
        return None
    bins = np.clip(((t - t0) / (t1 - t0) * 6).astype(int), 0, 5)
    ground = float(np.median([p.ground for p in group]))
    med = []
    for b in range(6):
        z = pts[bins == b, 2]
        if len(z) < 3:
            return None
        # The upper quartile: a seating mesh's vertices sit at both the
        # foot and the top of every step, so the median reads the feet.
        med.append(float(np.percentile(z, 75)) - ground)
    med = np.array(med)
    trend = float(np.corrcoef(np.arange(6), med)[0, 1]) if np.ptp(med) > 0 else 0.0
    rise = float(med[-1] - med[0])
    if trend < SEAT_MIN_TREND or rise < SEAT_MIN_RISE_M:
        return None
    front = pts[bins == 0, 2] - ground
    covered = bool(len(front) and float(front.max()) > med[-1] + 1.0)
    return rise, covered


def _distance_to_polyline(p: np.ndarray, line: np.ndarray) -> float:
    a, b = line[:-1], line[1:]
    ab = b - a
    t = np.clip(((p - a) * ab).sum(1) / np.maximum((ab * ab).sum(1), 1e-9), 0.0, 1.0)
    foot = a + t[:, None] * ab
    return float(np.hypot(*(foot - p).T).min())
