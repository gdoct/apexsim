"""dem_fetch.bare_earth: a roof in the surface model is capped at the ground
around it, the road is never touched and a second pass changes nothing.

    python -m unittest discover -s tools/tests -v
"""

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import dem_fetch  # noqa: E402

SPEC = {"cell_m": 10.0, "cols": 61, "rows": 61, "origin_x": -300.0, "origin_y": -300.0}


def ground():
    """A slope: 0.05 up along x, 0.02 along y, 12 m at the origin."""
    xs = SPEC["origin_x"] + np.arange(SPEC["cols"]) * SPEC["cell_m"]
    ys = SPEC["origin_y"] + np.arange(SPEC["rows"]) * SPEC["cell_m"]
    gx, gy = np.meshgrid(xs, ys)
    return gx, gy, 12.0 + 0.05 * gx + 0.02 * gy


class BareEarth(unittest.TestCase):
    def test_a_roof_is_capped_at_the_slope_under_it(self):
        gx, gy, h0 = ground()
        roof = (np.abs(gx - 40.0) <= 50.0) & (np.abs(gy + 20.0) <= 30.0)
        h = h0 + np.where(roof, 25.0, 0.0)
        # The road runs along y = 200, far from the building.
        road = np.column_stack([np.linspace(-300, 300, 121), np.full(121, 200.0)])
        prints = [(40.0, -20.0, 50.0, 30.0, 0.0, False)]
        n, most = dem_fetch.bare_earth(h, SPEC, prints, road)
        assert n == int(roof.sum())
        assert abs(most - 25.0) < 0.5
        assert np.abs(h - h0).max() < 0.5

    def test_the_road_is_never_lowered(self):
        gx, gy, h0 = ground()
        roof = np.hypot(gx, gy) <= 60.0
        h = h0 + np.where(roof, 20.0, 0.0)
        road = np.column_stack([np.linspace(-300, 300, 121), np.zeros(121)])
        dem_fetch.bare_earth(h, SPEC, [(0.0, 0.0, 60.0, 60.0, 0.0, True)], road)
        near = np.abs(gy) <= dem_fetch.BARE_ROAD_KEEP_M
        assert np.allclose(h[near & roof], h0[near & roof] + 20.0)
        assert np.abs(h[~near] - h0[~near]).max() < 0.5

    def test_a_second_pass_changes_nothing(self):
        gx, gy, h0 = ground()
        roof = (np.abs(gx) <= 40.0) & (np.abs(gy) <= 40.0)
        h = (h0 + np.where(roof, 15.0, 0.0)).astype(np.float32).astype(float)
        road = np.array([[1000.0, 1000.0]])
        prints = [(0.0, 0.0, 40.0, 40.0, 0.3, False)]
        dem_fetch.bare_earth(h, SPEC, prints, road)
        once = h.astype(np.float32).astype(float)
        n, _ = dem_fetch.bare_earth(once, SPEC, prints, road)
        assert n == 0


if __name__ == "__main__":
    unittest.main()
