"""The server's pit sidecar (`<Stem>.pit.msgpack`, `PitSidecar` in
`track-editor/core/src/ue_export.rs`, loaded as `server/sim/src/pit.rs`
`PitLane`) from what AC draws for its pits: `ai/pit_lane.ai` (the line a
car takes from the track, through the lane, and back) and the `AC_PIT_n`
dummies (where each team's car stops).

AC's pit line starts and ends on the racing line, usually a long way
before the lane proper begins, and the server wants a lane that begins
where it leaves the track (the AI hands over to it there, and a car near
it is placed along it), so the line is cut to the stretch that is off the
track, plus a short lead-in each end. The speed limit holds over the lane's
pit-surface triangles (`IS_PITLANE` / a `PIT` key), else over the stretch
that is clear of the track by more than a car's width.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .ai import AiSpline
from .centerline import AI_HEIGHT_FALLBACK_M, Measured, Spine
from .frame import Frame
from .physics import PhysicsWorld
from .sidecars import CONTACT_PIT_LANE

VERSION = 1
#: The lane's centerline every this many metres, as the exporter writes it.
STEP_M = 2.0
#: A lane node this far past the road's edge is off the track.
LEAVES_TRACK_M = 0.5
#: Past the edge by this much the lane is clear of the track (a car's width).
CLEAR_OF_TRACK_M = 3.0
#: The speed-limit lines lie where the lane is this far past the road's edge.
PAST_EDGE_M = 1.0
#: The lane begins this far before it first leaves the track, and ends this
#: far after it last does (the taper the server's AI and takeover expect).
LEAD_M = 30.0
#: A box's marker further than this from the lane is another lane's (or a
#: marker that does not belong to the line).
BOX_REACH_M = 20.0
#: A pit-surface run broken by less than this is one run.
RUN_GAP_M = 8.0
#: The lane has to be at least this long and hold at least one box.
MIN_LANE_M = 40.0
#: AC's pit line has parted from the racing line (the AI line) once it is
#: this far off it.
PARTS_FROM_LINE_M = 1.5
#: The lane's mouth moves back to where the pit line parts from the racing
#: line at most this far before it leaves the track.
MAX_EARLY_M = 100.0
#: A wall whose foot is more than this above the pit line does not part it
#: from the track (a bridge's parapet).
WALL_LEVEL_M = 2.0
#: Pit lane speed limit, km/h: AC's default (a session rule in AC, not a file).
SPEED_LIMIT_KMH = 80.0


@dataclass
class PitLaneResult:
    payload: dict | None
    report: dict
    warnings: list[str] = field(default_factory=list)


def _resample(points: np.ndarray, step: float) -> np.ndarray:
    seg = np.linalg.norm(np.diff(points[:, :2], axis=0), axis=1)
    keep = np.concatenate([[True], seg > 1e-6])
    points = points[keep]
    seg = np.linalg.norm(np.diff(points[:, :2], axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    n = max(int(np.ceil(cum[-1] / step)), 1)
    at = np.linspace(0.0, cum[-1], n + 1)
    return np.column_stack([np.interp(at, cum, points[:, k]) for k in range(3)])


def _beyond_edge(centre: np.ndarray, spine: Spine, width_left: np.ndarray, width_right: np.ndarray,
                 xy: np.ndarray, chunk: int = 256) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """For each point: how far past the road's edge it is (negative on the
    road), the index of the nearest road-middle sample, and which side it is
    on (+1 left)."""
    count = len(xy)
    beyond = np.empty(count)
    index = np.empty(count, dtype=np.int64)
    side = np.empty(count, dtype=np.int64)
    middle = centre[:, :2]
    for a in range(0, count, chunk):
        d = np.linalg.norm(xy[a:a + chunk, None, :] - middle[None, :, :], axis=2)
        index[a:a + chunk] = d.argmin(axis=1)
    t = spine.tangents[index]
    rel = xy - middle[index]
    lateral = t[:, 0] * rel[:, 1] - t[:, 1] * rel[:, 0]
    side[:] = np.where(lateral >= 0.0, 1, -1)
    edge = np.where(lateral >= 0.0, width_left[index], width_right[index])
    beyond[:] = np.abs(lateral) - edge
    return beyond, index, side


def _distance_to(points: np.ndarray, xy: np.ndarray, chunk: int = 256) -> np.ndarray:
    """For each of `xy`: the distance to the nearest of `points`."""
    out = np.empty(len(xy))
    for a in range(0, len(xy), chunk):
        out[a:a + chunk] = np.linalg.norm(xy[a:a + chunk, None, :] - points[None, :, :], axis=2).min(axis=1)
    return out


def _walled_off(xy: np.ndarray, z: np.ndarray, middle: np.ndarray, walls: np.ndarray,
                chunk: int = 128) -> np.ndarray:
    """For each point: whether a wall stands between it and the road's
    middle (`middle`, one point each), a wall the line's height crosses.

    The road's edge is where the physics' road ends, and where the pit lane
    is asphalt joined to the track's (Red Bull Ring's pit straight, Laguna
    Seca's start line) the edge takes the lane in: the lane is then told
    from the track only by the pit wall between them."""
    count = len(xy)
    out = np.zeros(count, dtype=bool)
    if walls is None or len(walls) == 0 or count == 0:
        return out
    lo = np.minimum(xy, middle).min(axis=0) - 1.0
    hi = np.maximum(xy, middle).max(axis=0) + 1.0
    wx0, wy0, wx1, wy1 = walls[:, 0], walls[:, 1], walls[:, 2], walls[:, 3]
    keep = ((np.maximum(wx0, wx1) >= lo[0]) & (np.minimum(wx0, wx1) <= hi[0])
            & (np.maximum(wy0, wy1) >= lo[1]) & (np.minimum(wy0, wy1) <= hi[1]))
    w = walls[keep]
    if len(w) == 0:
        return out
    ax, ay, bx, by, base, height = (w[:, k][None, :] for k in range(6))
    for a in range(0, count, chunk):
        px, py = xy[a:a + chunk, 0:1], xy[a:a + chunk, 1:2]
        qx, qy = middle[a:a + chunk, 0:1], middle[a:a + chunk, 1:2]
        zz = z[a:a + chunk, None]
        d1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        d2 = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax)
        d3 = (qx - px) * (ay - py) - (qy - py) * (ax - px)
        d4 = (qx - px) * (by - py) - (qy - py) * (bx - px)
        # A wall at the line's height: not a bridge's parapet overhead or
        # a wall down a bank below.
        level = (base <= zz + WALL_LEVEL_M) & (base + height >= zz + 0.3)
        out[a:a + chunk] = ((d1 * d2 < 0.0) & (d3 * d4 < 0.0) & level).any(axis=1)
    return out


def _longest_run(mask: np.ndarray, stations: np.ndarray, gap_m: float) -> tuple[int, int] | None:
    """The longest stretch of True (first and last index), runs less than
    `gap_m` apart counted as one."""
    idx = np.flatnonzero(mask)
    if idx.size == 0:
        return None
    runs: list[list[int]] = [[int(idx[0]), int(idx[0])]]
    for i in idx[1:]:
        if stations[i] - stations[runs[-1][1]] <= gap_m:
            runs[-1][1] = int(i)
        else:
            runs.append([int(i), int(i)])
    best = max(runs, key=lambda r: stations[r[1]] - stations[r[0]])
    return best[0], best[1]


def track_stations(centre: np.ndarray, node_step: int) -> np.ndarray:
    """Each spine sample's station along the lap as the *server* measures it:
    the arc length of the track YAML's nodes (every `node_step`-th sample of
    the measured centre, closed), not the AI line's. The road's middle is
    longer than the AI line round a lap (Red Bull Ring: 1.3%), and a station
    taken from the AI line puts the pit entry 50 m off by the last corner."""
    count = centre.shape[0]
    idx = np.arange(0, count, node_step)
    nodes = centre[idx, :2]
    seg = np.linalg.norm(np.diff(np.vstack([nodes, nodes[:1]]), axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    return np.interp(np.arange(count), np.concatenate([idx, [count]]), cum)


def build(pit_ai: AiSpline, boxes_ac: list[np.ndarray], frame: Frame, spine: Spine, measured: Measured,
          world: PhysicsWorld, *, node_step: int, min_width_m: float,
          speed_limit_kmh: float = SPEED_LIMIT_KMH) -> PitLaneResult:
    """The sidecar payload, or ``None`` with a warning saying why not."""
    warnings: list[str] = []
    report: dict = {"boxes_in_file": len(boxes_ac)}
    if pit_ai.count < 2:
        return PitLaneResult(None, report, ["pit_lane.ai has under two points: no pit lane"])
    if not boxes_ac:
        return PitLaneResult(None, report, ["no AC_PIT_n markers: no pit boxes, so no pit lane"])

    line = _resample(frame.ac_points(pit_ai.positions), STEP_M)
    stations = np.arange(len(line)) * (np.linalg.norm(np.diff(line[:, :2], axis=0), axis=1).sum() / max(len(line) - 1, 1))
    centre = measured.centre
    dist, near, side_of = _beyond_edge(centre, spine, measured.width_left, measured.width_right, line[:, :2])

    # The part off the track (past the road's edge, or behind a wall from
    # it), and a lead-in each end.
    walled = _walled_off(line[:, :2], line[:, 2], centre[near, :2], world.wall_segments)
    off = _longest_run((dist > LEAVES_TRACK_M) | walled, stations, RUN_GAP_M)
    if off is None:
        return PitLaneResult(None, report, ["the pit line never leaves the track: no pit lane"])
    # The lane's mouth: before the line peels off the racing line, not only
    # before it leaves the road, so a car coming in on the racing line is on
    # the lane before it turns off (Spa's turns off through the bus stop
    # at 40 degrees, 20 m before the road's edge).
    entry = off[0]
    parted = _distance_to(spine.positions[:, :2], line[:, :2]) > PARTS_FROM_LINE_M
    while entry > 0 and parted[entry - 1] and stations[off[0]] - stations[entry - 1] <= MAX_EARLY_M:
        entry -= 1
    report["parts_from_racing_line_m"] = round(float(stations[off[0]] - stations[entry]), 1)
    lo = int(np.searchsorted(stations, stations[entry] - LEAD_M, side="left"))
    hi = int(np.searchsorted(stations, stations[off[1]] + LEAD_M, side="right"))
    line, stations, dist, near, side_of = (line[lo:hi], stations[lo:hi] - stations[lo], dist[lo:hi], near[lo:hi],
                                           side_of[lo:hi])
    if len(line) < 3 or stations[-1] < MIN_LANE_M:
        return PitLaneResult(None, report, [f"the pit line is only {stations[-1]:.0f} m off the track: no pit lane"])

    # z from the physics under the line (the AI line floats a little above it).
    z, contact, _ = world.contacts_at(line[:, 0], line[:, 1], line[:, 2] + 1.5)
    line = line.copy()
    line[:, 2] = np.where(np.isfinite(z), z, line[:, 2] - AI_HEIGHT_FALLBACK_M)

    # The speed limit holds over the pit surface; failing that, where the
    # line is clear of the track by a car's width and more.
    how = "pit surface"
    limit = _longest_run(contact == CONTACT_PIT_LANE, stations, RUN_GAP_M)
    if limit is None or stations[limit[1]] - stations[limit[0]] < MIN_LANE_M * 0.5:
        how = "clear of the track"
        limit = _longest_run(dist > CLEAR_OF_TRACK_M, stations, RUN_GAP_M)
    if limit is None:
        return PitLaneResult(None, report, ["the pit line is never clear of the track: no pit lane"])
    # The server takes a human's car over at the first line only once it is
    # past the road's edge, and hands it back at the last: the lines are not
    # on the road. (AC's pit surface can begin on the road's edge.)
    past_edge = np.flatnonzero(dist > PAST_EDGE_M)
    if past_edge.size:
        limit = (max(limit[0], int(past_edge[0])), min(limit[1], int(past_edge[-1])))
    if limit[1] - limit[0] < 2:
        return PitLaneResult(None, report, ["the pit line's speed-limit stretch is under 4 m long: no pit lane"])
    limit_start, limit_end = float(stations[limit[0]]), float(stations[limit[1]])

    # Boxes: each marker to its nearest lane node, in order along the lane.
    spots = []
    for p_ac in boxes_ac:
        p = frame.ac_points(p_ac)
        d = np.linalg.norm(line[:, :2] - p[:2], axis=1)
        i = int(d.argmin())
        if d[i] > BOX_REACH_M:
            continue
        zz, _, _ = world.contacts_at(np.array([p[0]]), np.array([p[1]]), np.array([p[2] + 5.0]))
        j = min(max(i, 1), len(line) - 2)
        heading = float(np.arctan2(line[j + 1, 1] - line[j - 1, 1], line[j + 1, 0] - line[j - 1, 0]))
        spots.append((float(stations[i]), float(p[0]), float(p[1]),
                      float(zz[0]) if np.isfinite(zz[0]) else float(line[i, 2]), heading, float(d[i])))
    spots.sort()
    report.update({"lane_m": round(float(stations[-1]), 1), "limit": how,
                   "limit_start_m": round(limit_start, 1), "limit_end_m": round(limit_end, 1),
                   "boxes": len(spots)})
    if not spots:
        return PitLaneResult(None, report, [f"none of the {len(boxes_ac)} AC_PIT markers is within "
                                            f"{BOX_REACH_M:.0f} m of the pit line's speed-limit stretch: no pit lane"],)
    if len(spots) < len(boxes_ac):
        warnings.append(f"{len(boxes_ac) - len(spots)} of {len(boxes_ac)} AC_PIT markers are not by the pit line "
                        "and were left out")

    # Which side of the track the lane is on, where it is furthest off it.
    side = int(side_of[int(np.argmax(dist))])

    # AC's line is the through lane and the boxes stand up to 11 m off it, on
    # the working lane beside it. The server serves a car only on the lane
    # (within half its width and a metre), so the width takes in the boxes.
    reach = max(off for *_, off in spots)
    width_m = max(float(min_width_m), 2.0 * reach)
    report["width_m"] = round(width_m, 1)
    stations_of = track_stations(centre, node_step)
    payload = {
        "version": VERSION,
        "width_m": width_m,
        "speed_limit_mps": float(speed_limit_kmh / 3.6),
        "lane_side": side,
        "nodes": [[float(x), float(y), float(zz)] for x, y, zz in line],
        "length_m": float(stations[-1]),
        "limit_start_m": limit_start,
        "limit_end_m": limit_end,
        "entry_station_m": float(stations_of[near[0]]),
        "exit_station_m": float(stations_of[near[-1]]),
        "boxes": [{"lane_station_m": s, "x": x, "y": y, "z": zz, "yaw_rad": h} for s, x, y, zz, h, _ in spots],
    }
    report["lane_side"] = side
    return PitLaneResult(payload, report, warnings)
