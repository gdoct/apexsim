"""What every import is checked for before it is called done. Each check
is a row in the report: pass, warn or fail, with what it measured.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .centerline import Measured, Spine
from .physics import PhysicsWorld
from .sidecars import CONTACT_CURB, CONTACT_PIT_LANE, CONTACT_ROAD

WALL_REACH_M = 45.0
COVERAGE_FAIL_SHARE = 0.02
CENTERLINE_FAIL_SHARE = 0.02
BUDGET_TRIANGLES = 5_000_000
BUDGET_DRAW_CALLS = 1500
BUDGET_TEXTURE_MB = 600.0


@dataclass
class Check:
    name: str
    status: str  # pass | warn | fail
    detail: str
    data: dict = field(default_factory=dict)

    def row(self) -> dict:
        return {"name": self.name, "status": self.status, "detail": self.detail, **self.data}


def check_grid(world: PhysicsWorld, spawn_xy: list[tuple[float, float]], measured: Measured) -> Check:
    if not spawn_xy:
        return Check("grid", "warn", "no AC_START markers; the server lays its own 16-slot grid")
    xy = np.asarray(spawn_xy)
    z, contact, _ = world.contacts_at(xy[:, 0], xy[:, 1], np.full(len(xy), measured.centre[0, 2] + 20.0))
    ok = np.isfinite(z) & np.isin(contact, [CONTACT_ROAD, CONTACT_PIT_LANE, CONTACT_CURB])
    bad = int((~ok).sum())
    if bad:
        return Check("grid", "fail", f"{bad} of {len(xy)} grid slots have no road mesh under them",
                     {"slots": len(xy), "off_road": bad})
    return Check("grid", "pass", f"{len(xy)} grid slots on the road", {"slots": len(xy)})


def check_coverage(world: PhysicsWorld, spine: Spine, measured: Measured) -> Check:
    """Every metre of the lap, edge to edge at 0.25 m, has a physics triangle."""
    m = spine.positions.shape[0]
    lat = measured.laterals
    if measured.section_contact.size == 0:
        return Check("road_coverage", "fail", "no cross-sections were measured")
    inside = (lat[None, :] <= measured.width_left[:, None] + 1e-6) & (lat[None, :] >= -measured.width_right[:, None] - 1e-6)
    # The sections were sampled about the AI line; shift by the centre offset.
    offset = np.einsum("ij,ij->i", measured.centre[:, :2] - spine.positions[:, :2], spine.lefts)
    inside = (lat[None, :] <= measured.width_left[:, None] + offset[:, None] + 1e-6) & \
             (lat[None, :] >= -measured.width_right[:, None] + offset[:, None] - 1e-6)
    hole = inside & (measured.section_contact < 0)
    stations_with_holes = np.flatnonzero(hole.any(axis=1))
    share = len(stations_with_holes) / max(m, 1)
    worst = [float(spine.stations[i]) for i in stations_with_holes[:12]]
    detail = f"{len(stations_with_holes)} of {m} stations have a hole inside the road"
    status = "pass" if not len(stations_with_holes) else ("fail" if share > COVERAGE_FAIL_SHARE else "warn")
    return Check("road_coverage", status, detail, {"holes": int(len(stations_with_holes)), "stations": int(m),
                                                    "first_hole_stations_m": worst})


def check_centerline(measured: Measured, spine: Spine) -> Check:
    off = measured.spine_contact
    bad = np.flatnonzero(~np.isin(off, [CONTACT_ROAD, CONTACT_CURB, CONTACT_PIT_LANE]))
    share = len(bad) / max(len(off), 1)
    from_ai = int((~measured.from_mesh).sum())
    detail = f"the AI line leaves the road at {len(bad)} of {len(off)} stations; {from_ai} widths from the AI file"
    status = "pass" if len(bad) == 0 else ("fail" if share > CENTERLINE_FAIL_SHARE else "warn")
    return Check("centerline_on_road", status, detail,
                 {"off_stations": int(len(bad)), "first_off_stations_m": [float(spine.stations[i]) for i in bad[:12]],
                  "widths_from_ai": from_ai})


def check_walls(world: PhysicsWorld, spine: Spine, measured: Measured) -> Check:
    """Every 2 m, a ray from each road edge outward: does a wall stand
    within `WALL_REACH_M`? Reported, never failed: a track may have none."""
    segs = world.wall_segments
    if segs.shape[0] == 0:
        return Check("walls", "warn", "the track has no WALL physics meshes; nothing stops a car off the road",
                     {"segments": 0})
    step = max(int(round(2.0 / spine.step_m)), 1)
    idx = np.arange(0, spine.positions.shape[0], step)
    open_probes = 0
    total = 0
    open_stations: list[float] = []
    a = segs[:, 0:2]
    b = segs[:, 2:4]
    for i in idx:
        for side in (+1, -1):
            edge = measured.width_left[i] if side > 0 else measured.width_right[i]
            origin = measured.centre[i, :2] + side * edge * spine.lefts[i]
            direction = side * spine.lefts[i]
            total += 1
            if not _ray_hits_any(origin, direction, a, b, WALL_REACH_M):
                open_probes += 1
                if len(open_stations) < 12:
                    open_stations.append(float(spine.stations[i]))
    return Check("walls", "pass" if open_probes == 0 else "warn",
                 f"{open_probes} of {total} edge probes find no wall within {WALL_REACH_M:.0f} m "
                 f"({open_probes * 2} m of open edge); {segs.shape[0]} wall segments",
                 {"open_probes": open_probes, "probes": total, "segments": int(segs.shape[0]),
                  "first_open_stations_m": open_stations})


def _ray_hits_any(origin: np.ndarray, direction: np.ndarray, a: np.ndarray, b: np.ndarray, reach: float) -> bool:
    # Segment/ray intersection, vectorised over the segments.
    d = b - a
    denom = direction[0] * d[:, 1] - direction[1] * d[:, 0]
    ok = np.abs(denom) > 1e-9
    ao = a - origin
    t = np.where(ok, (ao[:, 0] * d[:, 1] - ao[:, 1] * d[:, 0]) / np.where(ok, denom, 1.0), np.inf)
    u = np.where(ok, (ao[:, 0] * direction[1] - ao[:, 1] * direction[0]) / np.where(ok, denom, 1.0), -1.0)
    hit = ok & (t >= 0.0) & (t <= reach) & (u >= 0.0) & (u <= 1.0)
    return bool(hit.any())


def check_budget(triangles: int, draw_calls: int, texture_bytes: int) -> Check:
    mb = texture_bytes / 1e6
    over = []
    if triangles > BUDGET_TRIANGLES:
        over.append(f"{triangles} triangles (over {BUDGET_TRIANGLES})")
    if draw_calls > BUDGET_DRAW_CALLS:
        over.append(f"{draw_calls} draw calls (over {BUDGET_DRAW_CALLS})")
    if mb > BUDGET_TEXTURE_MB:
        over.append(f"{mb:.0f} MB of textures (over {BUDGET_TEXTURE_MB:.0f})")
    build_s = triangles / 4.0e6 + mb / 400.0
    detail = f"{triangles} triangles, {draw_calls} draw calls, {mb:.0f} MB of textures, about {build_s:.0f} s to build"
    if over:
        detail += "; over budget: " + ", ".join(over)
    return Check("budget", "warn" if over else "pass", detail,
                 {"triangles": int(triangles), "draw_calls": int(draw_calls), "texture_mb": round(mb, 1),
                  "estimated_build_s": round(build_s, 1)})
