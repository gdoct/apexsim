"""The lap's spine: AC's `fast_lane.ai` moved onto the middle of the road
the physics mesh describes, resampled, measured, and written as the track
YAML the server keys everything on (laps, sectors, the AI, the racing line,
the grid).

Widths, heights and banking are measured on the physics mesh rather than
trusted from the AI file: at every metre a cross-section is sampled across
the road and the run of road-class triangles around the spine gives the
two edges; the middle of that run is the node, the kerb and run-off runs
beyond the edges are the curb sidecar. The AI file's own side distances
are only a fallback where no road lies under the line (a mod whose spine
leaves its own asphalt).
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field

import numpy as np

from .frame import Frame, ac_to_world
from .physics import PhysicsWorld
from .sidecars import CONTACT_CURB, CONTACT_PIT_LANE, CONTACT_ROAD, CONTACT_RUNOFF

TRACK_ID_NAMESPACE = uuid.UUID("7b1d1a8e-0d6f-4b7a-9a6c-3c8f1e2d5a10")
NODE_STEP_M = 5.0
LATERAL_REACH_M = 40.0
LATERAL_STEP_M = 0.25
#: The AI line rides this much above AC's physics road on the Kunos circuits.
AI_HEIGHT_FALLBACK_M = 0.3
SANE_SIDE_M = (0.5, 60.0)
#: Smoothing of the road middle's offset from the AI line, in metres of lap.
OFFSET_MEDIAN_M = 9
OFFSET_SIGMA_M = 6.0


def track_id_for(folder: str, layout: str) -> str:
    return str(uuid.uuid5(TRACK_ID_NAMESPACE, f"ac:{folder.lower()}/{layout.lower()}"))


@dataclass
class Spine:
    """The AI line in the track frame, rolled to start at the start line and
    resampled every `step_m` (a whole number of nodes of `NODE_STEP_M`)."""
    positions: np.ndarray  # (m, 3)
    tangents: np.ndarray   # (m, 2) unit
    lefts: np.ndarray      # (m, 2) unit, +90 deg from the tangent
    stations: np.ndarray   # (m,)
    step_m: float
    lap_m: float
    #: Station along the *AI* polyline (as AC measures it) of the start line.
    ai_start_station: float
    closed: bool


def make_frame(fl_positions_ac: np.ndarray, closed: bool, time0_ac: tuple[np.ndarray, np.ndarray] | None,
               warnings: list[str]) -> tuple[Frame, np.ndarray]:
    """The track frame from the AI line and the start-line markers, and the
    Z-up world origin it used. Without markers the line's own first point
    stands in."""
    world = ac_to_world(fl_positions_ac)
    if time0_ac is not None:
        left = ac_to_world(time0_ac[0])
        right = ac_to_world(time0_ac[1])
        origin = (left + right) / 2.0
    else:
        origin = world[0].copy()
        warnings.append("no AC_TIME_0_L/R markers: the start line is the AI line's first point")
    # Heading: the AI tangent at the point nearest the origin.
    d = np.linalg.norm(world[:, :2] - origin[:2], axis=1)
    i = int(np.argmin(d))
    j = (i + 1) % len(world) if closed else min(i + 1, len(world) - 1)
    k = (i - 1) % len(world) if closed else max(i - 1, 0)
    t = world[j, :2] - world[k, :2]
    heading = float(np.arctan2(t[1], t[0]))
    if d[i] > 30.0:
        warnings.append(f"the start line is {d[i]:.0f} m from the AI line; check the markers")
    return Frame(origin=origin, heading=heading), origin


def build_spine(fl_positions_ac: np.ndarray, frame: Frame, closed: bool) -> Spine:
    p = frame.ac_points(fl_positions_ac)
    if closed:
        # Close the polyline exactly; AC leaves a gap of a point or two.
        p = np.vstack([p, p[:1]])
    seg = np.linalg.norm(np.diff(p[:, :2], axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    total = float(cum[-1])
    # Station of the origin (the start line) on the polyline.
    a = p[:-1, :2]
    b = p[1:, :2]
    ab = b - a
    ab_len2 = np.maximum((ab * ab).sum(axis=1), 1e-9)
    t = np.clip(((-a) * ab).sum(axis=1) / ab_len2, 0.0, 1.0)
    foot = a + t[:, None] * ab
    dist = np.linalg.norm(foot, axis=1)
    s_index = int(np.argmin(dist))
    s0 = float(cum[s_index] + t[s_index] * seg[s_index])

    nodes = max(int(round(total / NODE_STEP_M)), 4)
    samples = nodes * int(NODE_STEP_M)
    step = total / samples
    stations = np.arange(samples) * step
    ai_stations = (stations + s0) % total if closed else np.minimum(stations + s0, total)

    def interp(col: np.ndarray) -> np.ndarray:
        return np.interp(ai_stations, cum, col)

    x = interp(p[:, 0])
    y = interp(p[:, 1])
    z = interp(p[:, 2])
    # Station 0 is the foot of the origin on the line, which is not the
    # origin: that is the middle of the timing gate, and Spa's gate is
    # 5.4 m off its AI line. Pinning the first point to the origin put a
    # 5 m-radius kink in the raceline at the seam, the speed profile
    # braked for it, and every braking point after it was planned for
    # the wrong entry speed. The grid is measured from the measured node 0.
    positions = np.column_stack([x, y, z])
    nxt = np.roll(positions, -1, axis=0)
    prv = np.roll(positions, 1, axis=0)
    if not closed:
        nxt[-1] = positions[-1] + (positions[-1] - positions[-2])
        prv[0] = positions[0] - (positions[1] - positions[0])
    tangents = (nxt - prv)[:, :2]
    length = np.linalg.norm(tangents, axis=1, keepdims=True)
    length[length == 0] = 1.0
    tangents = tangents / length
    lefts = np.column_stack([-tangents[:, 1], tangents[:, 0]])
    return Spine(positions=positions, tangents=tangents, lefts=lefts, stations=stations,
                 step_m=step, lap_m=total, ai_start_station=s0, closed=closed)


@dataclass
class Measured:
    """Everything the cross-sections gave, one row per spine sample."""
    centre: np.ndarray       # (m, 3) node positions (spine shifted to the road's middle)
    width_left: np.ndarray
    width_right: np.ndarray
    banking: np.ndarray
    curb_left: np.ndarray
    curb_right: np.ndarray
    runoff_left: np.ndarray
    runoff_right: np.ndarray
    #: True where the road run came from the mesh, False where the AI side
    #: distances (or the previous node) stood in.
    from_mesh: np.ndarray
    #: Contact class under the spine point itself (-1 where nothing).
    spine_contact: np.ndarray
    laterals: np.ndarray = field(default_factory=lambda: np.zeros(0))
    #: (m, k) contact classes across each section.
    section_contact: np.ndarray = field(default_factory=lambda: np.zeros((0, 0), dtype=np.int64))
    section_z: np.ndarray = field(default_factory=lambda: np.zeros((0, 0)))


def measure(spine: Spine, world: PhysicsWorld, ai_side_left: np.ndarray, ai_side_right: np.ndarray,
            warnings: list[str]) -> Measured:
    m = spine.positions.shape[0]
    laterals = np.arange(-LATERAL_REACH_M, LATERAL_REACH_M + LATERAL_STEP_M / 2, LATERAL_STEP_M)
    k = laterals.shape[0]
    px = spine.positions[:, None, 0] + laterals[None, :] * spine.lefts[:, None, 0]
    py = spine.positions[:, None, 1] + laterals[None, :] * spine.lefts[:, None, 1]
    ceiling = np.repeat(spine.positions[:, 2] + 1.5, k)
    z, contact, _ = world.contacts_at(px.reshape(-1), py.reshape(-1), ceiling)
    z = z.reshape(m, k)
    contact = contact.reshape(m, k)
    zero = int(np.argmin(np.abs(laterals)))

    is_road = contact == CONTACT_ROAD
    centre = spine.positions.copy()
    width_left = np.zeros(m)
    width_right = np.zeros(m)
    curb_left = np.zeros(m)
    curb_right = np.zeros(m)
    runoff_left = np.zeros(m)
    runoff_right = np.zeros(m)
    banking = np.zeros(m)
    from_mesh = np.zeros(m, dtype=bool)
    spine_contact = contact[:, zero].copy()

    # AI side distances, resampled onto the spine's stations, as the fallback.
    side_ok = ((ai_side_left > SANE_SIDE_M[0]) & (ai_side_left < SANE_SIDE_M[1])
               & (ai_side_right > SANE_SIDE_M[0]) & (ai_side_right < SANE_SIDE_M[1]))
    fallback_used = 0
    last_wl, last_wr = 5.0, 5.0
    edge_left = np.zeros(m)
    edge_right = np.zeros(m)
    for i in range(m):
        row = is_road[i]
        seed = zero if row[zero] else _nearest_true(row, zero, int(4.0 / LATERAL_STEP_M))
        if seed is None:
            wl, wr = _ai_widths(ai_side_left, ai_side_right, side_ok, i, m, last_wl, last_wr)
            fallback_used += 1
            width_left[i], width_right[i] = wl, wr
            last_wl, last_wr = wl, wr
            continue
        lo = seed
        while lo - 1 >= 0 and row[lo - 1]:
            lo -= 1
        hi = seed
        while hi + 1 < k and row[hi + 1]:
            hi += 1
        # Edges half a sample past the last road sample.
        d_right = laterals[lo] - LATERAL_STEP_M / 2
        d_left = laterals[hi] + LATERAL_STEP_M / 2
        if lo == 0 or hi == k - 1:
            # The run reaches the sampling window's end: a paddock or a
            # parallel road merged into it. Trust the AI widths on that side.
            wl_ai, wr_ai = _ai_widths(ai_side_left, ai_side_right, side_ok, i, m, last_wl, last_wr)
            if hi == k - 1:
                d_left = min(d_left, wl_ai)
            if lo == 0:
                d_right = max(d_right, -wr_ai)
        c = (d_left + d_right) / 2.0
        edge_left[i], edge_right[i] = d_left, d_right
        width_left[i] = d_left - c
        width_right[i] = c - d_right
        from_mesh[i] = True
        last_wl, last_wr = width_left[i], width_right[i]
        # Kerbs and run-off beyond each edge.
        curb_left[i], runoff_left[i] = _bands_beyond(contact[i], hi + 1, +1, k)
        curb_right[i], runoff_right[i] = _bands_beyond(contact[i], lo - 1, -1, k)
    if fallback_used:
        warnings.append(f"{fallback_used} of {m} cross-sections found no road under the AI line; "
                        "the AI file's side distances were used there")

    # The middle of each section is quantised to the lateral step and follows
    # every notch in the mesh's road edge: at Spa it zig-zagged 0.3 m node to
    # node, a 100 m radius on every straight. The offset from the (smooth)
    # AI line is smoothed along the lap instead; the edges stay where they
    # were measured.
    offset = np.where(from_mesh, (edge_left + edge_right) / 2.0, 0.0)
    offset = _smooth_along(offset, spine.closed)
    centre[:, :2] = spine.positions[:, :2] + offset[:, None] * spine.lefts
    width_left = np.where(from_mesh, np.maximum(edge_left - offset, 0.5), width_left)
    width_right = np.where(from_mesh, np.maximum(offset - edge_right, 0.5), width_right)

    # Height and banking at the node: the mesh under the node and at both edges.
    ceiling_n = centre[:, 2] + 1.5
    zc, _, _ = world.contacts_at(centre[:, 0], centre[:, 1], ceiling_n)
    inner_l = np.maximum(width_left - 0.5, 0.2)
    inner_r = np.maximum(width_right - 0.5, 0.2)
    zl, _, _ = world.contacts_at(centre[:, 0] + inner_l * spine.lefts[:, 0],
                                 centre[:, 1] + inner_l * spine.lefts[:, 1], ceiling_n)
    zr, _, _ = world.contacts_at(centre[:, 0] - inner_r * spine.lefts[:, 0],
                                 centre[:, 1] - inner_r * spine.lefts[:, 1], ceiling_n)
    ai_z = spine.positions[:, 2] - AI_HEIGHT_FALLBACK_M
    zc = np.where(np.isfinite(zc), zc, ai_z)
    centre[:, 2] = zc
    both = np.isfinite(zl) & np.isfinite(zr)
    banking = np.where(both, np.arctan2(np.where(both, zl, 0) - np.where(both, zr, 0),
                                        np.maximum(inner_l + inner_r, 0.5)), 0.0)
    banking = np.clip(banking, -0.6, 0.6)
    return Measured(centre=centre, width_left=width_left, width_right=width_right, banking=banking,
                    curb_left=curb_left, curb_right=curb_right, runoff_left=runoff_left,
                    runoff_right=runoff_right, from_mesh=from_mesh, spine_contact=spine_contact,
                    laterals=laterals, section_contact=contact, section_z=z)


def _smooth_along(values: np.ndarray, closed: bool) -> np.ndarray:
    """A running median over `OFFSET_MEDIAN_M` (a section that caught a
    paddock is one sample, not a bump) then a Gaussian of `OFFSET_SIGMA_M`,
    wrapping round a closed lap. The samples are a metre apart."""
    n = values.shape[0]
    half = OFFSET_MEDIAN_M // 2
    pad = int(4 * OFFSET_SIGMA_M) + half
    if n <= 2 * pad:
        return values.copy()
    padded = np.concatenate([values[-pad:], values, values[:pad]]) if closed         else np.pad(values, pad, mode="edge")
    windows = np.lib.stride_tricks.sliding_window_view(padded, 2 * half + 1)
    median = np.median(windows, axis=1)  # length n + 2 * (pad - half)
    x = np.arange(-(pad - half), pad - half + 1)
    kernel = np.exp(-0.5 * (x / OFFSET_SIGMA_M) ** 2)
    kernel /= kernel.sum()
    return np.convolve(median, kernel, mode="valid")


def _nearest_true(row: np.ndarray, at: int, reach: int) -> int | None:
    for d in range(1, reach + 1):
        if at + d < row.shape[0] and row[at + d]:
            return at + d
        if at - d >= 0 and row[at - d]:
            return at - d
    return None


def _ai_widths(side_left, side_right, side_ok, i, m, last_wl, last_wr) -> tuple[float, float]:
    j = int(round(i * (len(side_left) / max(m, 1)))) % max(len(side_left), 1)
    if len(side_left) and side_ok[j]:
        return float(side_left[j]), float(side_right[j])
    return last_wl, last_wr


def _bands_beyond(row: np.ndarray, start: int, direction: int, k: int) -> tuple[float, float]:
    """`(curb_m, runoff_m)` beyond a road edge: the contiguous curb run right
    at the edge (one-sample gaps forgiven), then the tarmac run-off beyond
    it, both measured from the road edge."""
    curb = 0
    j = start
    gap = 0
    while 0 <= j < k:
        c = row[j]
        if c == CONTACT_CURB:
            curb = abs(j - start) + 1
            gap = 0
        elif gap < 1 and c not in (CONTACT_ROAD, CONTACT_PIT_LANE) and curb > 0:
            gap += 1
        else:
            break
        j += direction
    runoff = 0
    j = start + direction * curb
    gap = 0
    while 0 <= j < k:
        c = row[j]
        if c == CONTACT_RUNOFF:
            runoff = abs(j - start) + 1
            gap = 0
        elif gap < 1 and runoff > 0 and c not in (CONTACT_ROAD, CONTACT_PIT_LANE, CONTACT_CURB):
            gap += 1
        else:
            break
        j += direction
    return curb * LATERAL_STEP_M, runoff * LATERAL_STEP_M


def nearest_station_index(spine: Spine, point_xy: np.ndarray) -> int:
    d = np.linalg.norm(spine.positions[:, :2] - np.asarray(point_xy)[:2], axis=1)
    return int(np.argmin(d))


def track_yaml(spine: Spine, measured: Measured, raceline: np.ndarray, *, name: str, display_name: str,
               track_id: str, sectors: list[int], spawn_xy: list[tuple[float, float]],
               drs_zones: list[tuple[float, float, float]], metadata: dict, per_node: int = int(NODE_STEP_M)
               ) -> dict:
    """The YAML document, in the key order the shipped circuits use."""
    idx = np.arange(0, spine.positions.shape[0], per_node)
    nodes = []
    for i in idx:
        nodes.append({
            "x": round(float(measured.centre[i, 0]), 3),
            "y": round(float(measured.centre[i, 1]), 3),
            "z": round(float(measured.centre[i, 2]), 3),
            "width": None,
            "width_left": round(float(measured.width_left[i]), 3),
            "width_right": round(float(measured.width_right[i]), 3),
            "banking": round(float(measured.banking[i]), 4),
            "friction": 1.0,
            "surface_type": "Asphalt",
        })
    default_width = float(np.mean(measured.width_left[idx] + measured.width_right[idx]))
    doc = {
        "name": name,
        "display_name": display_name,
        "track_id": track_id,
        "nodes": nodes,
        "checkpoints": [],
        "sectors": [int(s) for s in sectors],
        "spawn_points": [{"position": 0, "offset_x": round(float(x), 3), "offset_y": round(float(y), 3)}
                         for x, y in spawn_xy],
        "default_width": round(default_width, 3),
        "closed_loop": bool(spine.closed),
        "raceline": [{"x": round(float(p[0]), 3), "y": round(float(p[1]), 3), "z": round(float(p[2]), 3)}
                     for p in raceline],
        "drs_zones": [{"detection_m": round(float(d), 1), "start_m": round(float(s), 1), "end_m": round(float(e), 1)}
                      for d, s, e in drs_zones],
        "metadata": metadata,
    }
    return doc


def dump_yaml(doc: dict) -> str:
    import yaml

    class _Dumper(yaml.SafeDumper):
        pass

    def _none(dumper, _value):
        return dumper.represent_scalar("tag:yaml.org,2002:null", "null")

    _Dumper.add_representer(type(None), _none)
    return yaml.dump(doc, Dumper=_Dumper, sort_keys=False, allow_unicode=True, default_flow_style=False,
                     width=1000)
