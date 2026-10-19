"""AC's frame to ApexSim's.

AC positions are Y-up. Checked on every Kunos circuit against the
direction `ui_track.json` records: a clockwise lap traces a positive
signed area in AC's raw `(x, z)` plane, so the frame is right-handed once
`y` is read as up and `-z` as the second horizontal axis. The conversion
is therefore a rotation about X, no mirror, and triangles keep their
authored index order (the physics road's cross product already points up):

    ApexSim (X, Y, Z) = (x, -z, y)          metres, +Z up

after which the track frame puts the origin on the start line (the
midpoint of `AC_TIME_0_L` / `AC_TIME_0_R`) with +X along the direction of
travel there and +Y to the left, like every shipped circuit. One
`Frame` applies both steps to everything.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


def ac_to_world(p: np.ndarray) -> np.ndarray:
    """AC (x, y, z) -> Z-up (x, -z, y), any shape (..., 3)."""
    p = np.asarray(p, dtype=np.float64)
    out = np.empty_like(p)
    out[..., 0] = p[..., 0]
    out[..., 1] = -p[..., 2]
    out[..., 2] = p[..., 1]
    return out


@dataclass(frozen=True)
class Frame:
    """The Z-up world -> track frame: translate `origin` to 0, then yaw by
    `-heading` so the start line's direction of travel is +X."""
    origin: np.ndarray
    heading: float

    @property
    def cos(self) -> float:
        return float(np.cos(self.heading))

    @property
    def sin(self) -> float:
        return float(np.sin(self.heading))

    def points(self, p: np.ndarray) -> np.ndarray:
        """(..., 3) Z-up world points -> track frame."""
        p = np.asarray(p, dtype=np.float64) - self.origin
        out = np.empty_like(p)
        c, s = self.cos, self.sin
        out[..., 0] = c * p[..., 0] + s * p[..., 1]
        out[..., 1] = -s * p[..., 0] + c * p[..., 1]
        out[..., 2] = p[..., 2]
        return out

    def vectors(self, v: np.ndarray) -> np.ndarray:
        v = np.asarray(v, dtype=np.float64)
        out = np.empty_like(v)
        c, s = self.cos, self.sin
        out[..., 0] = c * v[..., 0] + s * v[..., 1]
        out[..., 1] = -s * v[..., 0] + c * v[..., 1]
        out[..., 2] = v[..., 2]
        return out

    def ac_points(self, p: np.ndarray) -> np.ndarray:
        return self.points(ac_to_world(p))

    def ac_vectors(self, v: np.ndarray) -> np.ndarray:
        return self.vectors(ac_to_world(v))

    def with_origin_z(self, z: float) -> "Frame":
        origin = self.origin.copy()
        origin[2] = z
        return Frame(origin=origin, heading=self.heading)

    def yaw(self, world_yaw: float) -> float:
        """A Z-up world yaw (CCW from +X) in the track frame."""
        return float(np.arctan2(np.sin(world_yaw - self.heading), np.cos(world_yaw - self.heading)))
