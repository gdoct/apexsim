"""Readers for AC's `.ai` spline files (`ai/fast_lane.ai`, `ai/pit_lane.ai`).

    i32 version (7), i32 count, i32 lap_time, i32 sample_count
    count x (f32 x, y, z, length, i32 id)              the points
    i32 extra_count                                     (== count)
    extra_count x 18 f32: speed, gas, brake, obsolete_lat_g, radius,
        side_left, side_right, camber, direction, normal xyz, length,
        forward xyz, tag, grade
    i32 has_grid, then a spatial index nobody needs

`side_left` / `side_right` are the distances from the point to the road's
edges; a few mods carry garbage there (Zandvoort's fast lane has 4e6 on
one point), so callers clamp them. Positions are in AC's raw frame
(`frame.py` converts).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

POINT_DTYPE = np.dtype([("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("len", "<f4"), ("id", "<i4")])
EXTRA_FIELDS = (
    "speed", "gas", "brake", "obsolete_lat_g", "radius", "side_left", "side_right",
    "camber", "direction", "nx", "ny", "nz", "length", "fx", "fy", "fz", "tag", "grade",
)


class AiError(Exception):
    pass


@dataclass
class AiSpline:
    path: Path
    version: int
    #: (n, 3) float64, AC frame.
    positions: np.ndarray
    #: (n,) cumulative length as AC stored it.
    length: np.ndarray
    #: (n, 18) float64, see EXTRA_FIELDS; zero rows when the file has none.
    extra: np.ndarray

    @property
    def count(self) -> int:
        return int(self.positions.shape[0])

    @property
    def side_left(self) -> np.ndarray:
        return self.extra[:, 5]

    @property
    def side_right(self) -> np.ndarray:
        return self.extra[:, 6]

    def is_closed(self, tolerance_m: float = 50.0) -> bool:
        if self.count < 3:
            return False
        return float(np.linalg.norm(self.positions[0] - self.positions[-1])) < tolerance_m


def read_ai(path: Path) -> AiSpline:
    path = Path(path)
    data = path.read_bytes()
    if len(data) < 16:
        raise AiError(f"{path.name}: too short to be an .ai file")
    version, count, _lap_time, _samples = struct.unpack("<iiii", data[:16])
    if version not in (7,) or count <= 0 or count > 1_000_000:
        raise AiError(f"{path.name}: unsupported .ai file (version {version}, {count} points)")
    at = 16
    need = count * POINT_DTYPE.itemsize
    if len(data) < at + need:
        raise AiError(f"{path.name}: truncated after {count} points were promised")
    pts = np.frombuffer(data[at:at + need], dtype=POINT_DTYPE)
    at += need
    extra = np.zeros((count, 18), dtype=np.float64)
    if len(data) >= at + 4:
        extra_count = struct.unpack("<i", data[at:at + 4])[0]
        at += 4
        need = extra_count * 18 * 4
        if 0 < extra_count <= count and len(data) >= at + need:
            extra[:extra_count] = np.frombuffer(data[at:at + need], dtype="<f4").reshape(extra_count, 18)
    positions = np.stack([pts["x"], pts["y"], pts["z"]], axis=1).astype(np.float64)
    if not np.all(np.isfinite(positions)):
        raise AiError(f"{path.name}: non-finite point positions")
    return AiSpline(path=path, version=version, positions=positions,
                    length=pts["len"].astype(np.float64), extra=extra)
