"""Tests for the AC survey (`ac_import/features.py`) and the dossier
correction (`ac_layout.py`): what the survey finds on a synthetic track,
the fit and the rubber sheet on a synthetic pair of centerlines, and every
merge rule.

    python -m unittest discover -s scripts/ac_import/tests -v
"""

from __future__ import annotations

import copy
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import numpy as np

SCRIPTS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import ac_layout  # noqa: E402
from ac_import import features, kn5  # noqa: E402
from osm_layout import NearestGrid, densify  # noqa: E402
from test_ac_import import SyntheticOval, mat, mesh_node, write_ai  # noqa: E402


def box_mesh(oval, name, x0, y0, sx, sy, z0, z1, material):
    """An axis-aligned box in the world frame (x, y, z up)."""
    corners = np.array([oval.ac(x0 + dx, y0 + dy, dz) for dx in (0, sx) for dy in (0, sy) for dz in (z0, z1)])
    tris = np.array([[0, 2, 3], [0, 3, 1], [4, 5, 7], [4, 7, 6], [0, 1, 5], [0, 5, 4], [2, 6, 7], [2, 7, 3],
                     [0, 4, 6], [0, 6, 2], [1, 3, 7], [1, 7, 5]])
    return mesh_node(name, corners, tris, material)


def steps_mesh(oval, name, x0, y0, length, rows, material):
    """Tiered seating: `rows` steps rising 1 m per 2 m away from the road
    (the road is toward -x here)."""
    nodes = []
    for k in range(rows):
        nodes.append(box_mesh(oval, f"{name}_{k}", x0 + 2.0 * k, y0, 2.0, length, 0.0, 1.0 + k, material))
    return nodes


class SurveyedOval(SyntheticOval):
    """The importer's oval plus things to survey beside it: a building, a
    stand named in Italian, a stand that only its shape gives away, a
    footbridge over the road, a parked car, a patch of trees, a pit lane."""

    def build(self):
        super().build()
        root = self.root
        kn = kn5.read_kn5(root / "oval.kn5")
        r = self.RADIUS
        materials = [mat("Concrete", texture="stand.png"), mat("Seats", texture="stand.png"),
                     mat("tree_pine", shader="ksTree", texture="grass.png"), mat("Car", texture="asph.png")]
        nodes = []
        # A building 40 m outside the road at angle 0 (world +x).
        nodes.append(box_mesh(self, "Office", r + 40.0, -60.0, 20.0, 30.0, 0.0, 9.0, 0))
        # A stand by name, outside the road at angle 90 degrees (world +y).
        nodes.append(box_mesh(self, "TribunaCentrale", -30.0, r + 15.0, 60.0, 14.0, 0.0, 8.0, 1))
        # A stand by shape only: tiered rows outside at angle 180 (world -x),
        # rising away from the road (toward -x).
        for k in range(8):
            nodes.append(box_mesh(self, f"concrete_{k}", -(r + 12.0) - 2.0 * (k + 1), -20.0, 2.0, 40.0,
                                  0.0, 1.0 + k, 0))
        # A footbridge deck over the road at angle 270 (world -y): 3 m wide,
        # spanning the road 6 m up, on two supports.
        nodes.append(box_mesh(self, "Bridge_deck", -1.5, -(r + 12.0), 3.0, 24.0, 6.0, 7.0, 0))
        nodes.append(box_mesh(self, "Bridge_leg_a", -1.5, -(r + 12.0), 3.0, 3.0, 0.0, 6.0, 0))
        nodes.append(box_mesh(self, "Bridge_leg_b", -1.5, -(r - 15.0), 3.0, 3.0, 0.0, 6.0, 0))
        # A parked car: clutter, never a building.
        nodes.append(box_mesh(self, "parking_car_01", r + 50.0, 40.0, 4.5, 2.0, 0.0, 1.6, 3))
        # Tree cards: a patch of crossed quads at 30 m spacing outside angle 45.
        tri = np.array([[0, 1, 2], [0, 2, 3]])
        cx, cy = (r + 80.0) * math.cos(math.pi / 4), (r + 80.0) * math.sin(math.pi / 4)
        for i in range(6):
            for j in range(6):
                x, y = cx + 4.0 * i, cy + 4.0 * j
                quad = np.array([self.ac(x - 1, y, 0), self.ac(x + 1, y, 0), self.ac(x + 1, y, 6),
                                 self.ac(x - 1, y, 6)])
                nodes.append(mesh_node(f"tree_{i}_{j}", quad, tri, 2))
        kn5.write_kn5(root / "scenery.kn5", {}, materials, nodes)
        (root / "models.ini").write_text(
            "[MODEL_0]\nFILE=oval.kn5\nPOSITION=0,0,0\nROTATION=0,0,0\n"
            "[MODEL_1]\nFILE=scenery.kn5\nPOSITION=0,0,0\nROTATION=0,0,0\n", encoding="utf-8")
        del kn
        # A pit lane: on the road, then 15 m inside it for 200 m, then back.
        angles = np.linspace(-0.6, 0.6, 120)
        rad = np.where(np.abs(angles) < 0.45, r - 15.0, r)
        pts = np.array([self.ac(q * math.cos(a), q * math.sin(a), 0.3) for a, q in zip(angles, rad)])
        write_ai(root / "ai" / "pit_lane.ai", pts, side_left=3.0, side_right=3.0)


class SurveyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name) / "surveyed_oval"
        SurveyedOval(root)
        cls.survey = features.survey_layout(root, None)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def of(self, cls):
        return [o for o in self.survey.objects if o.cls == cls]

    def test_the_building_is_a_structure(self):
        offices = [o for o in self.of("structure") if "Office" in o.names]
        self.assertEqual(len(offices), 1)
        self.assertAlmostEqual(features.ring_area(offices[0].ring), 600.0, delta=5.0)
        self.assertAlmostEqual(offices[0].height_m, 9.0, delta=0.5)

    def test_stands_by_name_and_by_shape(self):
        stands = self.of("stand")
        self.assertTrue(any("TribunaCentrale" in o.names for o in stands), [o.reason for o in stands])
        shaped = [o for o in stands if "concrete" in o.names]
        self.assertEqual(len(shaped), 1, [o.reason for o in self.survey.objects if "concrete" in o.names])
        self.assertIn("rises", shaped[0].reason)

    def test_the_footbridge_is_a_crossing(self):
        crossings = self.of("crossing")
        self.assertEqual(len(crossings), 1, [o.reason for o in self.survey.objects])
        self.assertEqual(crossings[0].kind, "footbridge")

    def test_parked_cars_are_not_buildings(self):
        self.assertFalse(any("parking" in o.names for o in self.survey.objects))

    def test_trees_are_points(self):
        self.assertEqual(len(self.survey.trees), 36)
        self.assertTrue((self.survey.trees[:, 2] == 2).all(), "pine names make needleleaved trees")

    def test_the_pit_lane_is_trimmed_to_its_own_road(self):
        lane = self.survey.pit_lane
        self.assertIsNotNone(lane)
        # Kept: the 15 m inset stretch and one node either side, not the
        # 0.6 rad of road the AI spline starts and ends on.
        length = float(np.hypot(*np.diff(lane, axis=0).T).sum())
        self.assertGreater(length, 150.0)
        self.assertLess(length, 0.95 * 200.0 * 1.2)


def fake_track(pts: np.ndarray, half_width: float = 6.0):
    pts = densify(pts, 2.0, closed=True)[:-1]
    station = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
    total = float(station[-1] + np.hypot(*(pts[0] - pts[-1])))
    d = np.gradient(pts, axis=0)
    t = SimpleNamespace(pts=pts, station=station, total=total, width_left=np.full(len(pts), half_width),
                        width_right=np.full(len(pts), half_width), heading=np.arctan2(d[:, 1], d[:, 0]),
                        grid=NearestGrid(pts, cell=40.0))

    def locate(q):
        q = np.asarray(q, dtype=float).reshape(-1, 2)
        _, j = t.grid.query(q)
        h = t.heading[j]
        dd = q - t.pts[j]
        lat = -np.sin(h) * dd[:, 0] + np.cos(h) * dd[:, 1]
        along = np.cos(h) * dd[:, 0] + np.sin(h) * dd[:, 1]
        return (t.station[j] + along) % t.total, lat

    t.locate = locate
    t.index_at = lambda s: int(np.searchsorted(t.station, s % t.total).clip(0, len(t.pts) - 1))
    return t


def kidney(n=600):
    a = np.linspace(0, 2 * math.pi, n, endpoint=False)
    r = 500.0 + 140.0 * np.cos(2 * a) + 60.0 * np.sin(3 * a)
    return np.c_[r * np.cos(a), 0.7 * r * np.sin(a)]


class FitTests(unittest.TestCase):
    def setUp(self):
        self.native = kidney()
        # The AC trace: the native one with an 8 m smooth bend over a
        # stretch, then rotated 120 degrees and moved 300 m away.
        bent = self.native.copy()
        a = np.linspace(0, 2 * math.pi, len(bent), endpoint=False)
        bump = 8.0 * np.exp(-((a - 1.0) / 0.12) ** 2)
        normal = np.c_[np.cos(a), np.sin(a)]
        bent += bump[:, None] * normal
        th = math.radians(120.0)
        rot = np.array([[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]])
        self.rot, self.shift = rot, np.array([300.0, -120.0])
        self.ac = bent @ rot.T + self.shift
        self.track = fake_track(self.native)
        self.fit = ac_layout.Fit(self.track, densify(self.ac, 1.0, closed=True))

    def test_the_rigid_fit_finds_the_frame(self):
        self.assertAlmostEqual(abs(self.fit.scale - 1.0), 0.0, delta=0.003)
        self.assertAlmostEqual((math.degrees(self.fit.rotation) + 120.0 + 180.0) % 360.0 - 180.0, 0.0, delta=1.0)

    def test_the_field_takes_out_the_bend(self):
        self.assertLess(self.fit.rigid_cover, 0.999)
        self.assertGreater(self.fit.field_cover, 0.97)
        self.assertEqual(self.fit.gate(), [])
        self.assertTrue(self.fit.shift, "the bent stretch is reported as shifted")
        self.assertEqual(self.fit.reshape, [])

    def test_a_point_beside_the_bend_follows_the_native_road(self):
        a = 1.0
        i = int(round(a / (2 * math.pi) * len(self.native)))
        native_pt = self.native[i]
        out = native_pt / np.linalg.norm(native_pt)
        beside_native = native_pt + 30.0 * out
        # The same building in the AC frame: 30 m out from the *bent* road.
        bent_pt = native_pt + 8.0 * np.array([math.cos(a), math.sin(a)])
        beside_ac = (bent_pt + 30.0 * out) @ self.rot.T + self.shift
        moved = self.fit.apply(beside_ac[None, :])[0]
        self.assertLess(float(np.hypot(*(moved - beside_native))), 2.5)


def stand(name, centre, length=60.0, depth=15.0, yaw=0.0, source="osm"):
    return {"name": name, "source": source, "station_m": 10.0, "side": "left", "offset_m": 20.0,
            "length_m": length, "depth_m": depth, "yaw_rad": yaw, "centre": list(centre), "covered": False,
            "front": [[centre[0] - length / 2, centre[1] - depth / 2], [centre[0] + length / 2, centre[1] - depth / 2]]}


def building(name, centre, length=30.0, depth=20.0):
    return {"name": name, "station_m": 20.0, "side": "right", "offset_m": 30.0, "length_m": length,
            "depth_m": depth, "yaw_rad": 0.0, "centre": list(centre), "levels": 2, "osm_building": "yes",
            "area_m2": length * depth}


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.layout = {
            "format": "apex-track-layout", "version": 1, "source_track": "X.yaml", "attribution": "OSM",
            "pit_lane": {"side": "right", "length_m": 300.0, "nodes": [[0, -20], [300, -20]]},
            "stands": [stand("Main", (0, 50)), stand("Seating map stand", (500, 50), source="seating map")],
            "structures": [building("Office", (200, -60)), building("Kiosk", (800, -60), 8, 8),
                           building("Shed A", (1000, 0), 20, 20), building("Shed B", (1040, 0), 20, 20)],
            "crossings": [{"name": "Footbridge", "station_m": 400.0, "kind": "footbridge"},
                          {"name": "Tyre bridge", "station_m": 900.0, "kind": "arch", "source": "authored"}],
            "landmarks": [{"kind": "tower", "name": "Tower", "station_m": 50.0, "side": "left",
                           "centre": [100.0, 100.0]}],
            "woods": [{"leaf": "mixed", "ring": [[0, 0], [10, 0], [10, 10]]}],
        }
        ac = lambda e: {**e, "source": "ac", "name": None}  # noqa: E731
        self.overlay = {
            "ac": {"folder": "x", "layout": "", "origin": "kunos"},
            "entries": {
                # Main moved 6 m; the seating-map stand overlapped; a new stand.
                "stands": [ac(stand(None, (6, 52))), ac(stand(None, (505, 50))), ac(stand(None, (2000, 50)))],
                # The office matched as a building; one AC box over both sheds.
                "structures": [ac(building(None, (203, -58))), ac(building(None, (1020, 0), 120, 60))],
                "crossings": [{"name": None, "source": "ac", "station_m": 410.0, "kind": "road"},
                              {"name": None, "source": "ac", "station_m": 905.0, "kind": "road"}],
                "landmarks": [{"kind": "tower", "name": None, "source": "ac", "station_m": 55.0, "side": "left",
                               "centre": [120.0, 100.0]}],
                "woods": [{"leaf": "needleleaved", "source": "ac", "ring": [[50, 50], [60, 50], [60, 60]]}],
                "pit_lane": {"side": "right", "length_m": 280.0, "nodes": [[0, -15], [280, -15]], "source": "ac",
                             "box_count": 18},
            },
        }
        self.merged, self.rep = ac_layout.merge(self.layout, self.overlay, "NoSuchStem")

    def names(self, layer):
        return sorted((e.get("name") or "-", e.get("source")) for e in self.merged[layer])

    def test_a_matched_stand_takes_ac_geometry_and_keeps_its_name(self):
        main = [e for e in self.merged["stands"] if e.get("name") == "Main"]
        self.assertEqual(len(main), 1)
        self.assertEqual(main[0]["source"], "ac")
        self.assertEqual(main[0]["centre"], [6, 52])

    def test_manual_entries_are_never_replaced(self):
        self.assertIn(("Seating map stand", "seating map"), self.names("stands"))
        self.assertEqual(sum(1 for e in self.merged["stands"] if e["centre"] == [505, 50]), 0)
        self.assertIn(("Tyre bridge", "authored"), sorted((c["name"], c.get("source")) for c in self.merged["crossings"]))

    def test_new_ac_entries_are_added_and_unmatched_natives_kept(self):
        self.assertTrue(any(e["centre"] == [2000, 50] for e in self.merged["stands"]))
        self.assertIn(("Kiosk", None), self.names("structures"))

    def test_a_footprint_far_bigger_than_what_it_covers_keeps_the_natives(self):
        names = [n for n, _ in self.names("structures")]
        self.assertIn("Shed A", names)
        self.assertIn("Shed B", names)
        self.assertFalse(any(e["length_m"] == 120 for e in self.merged["structures"]))
        self.assertEqual(len(self.rep["stands_structures"]["size_mismatch_kept_native"]), 1)

    def test_crossings_and_landmarks_move_to_ac(self):
        fb = [c for c in self.merged["crossings"] if c.get("name") == "Footbridge"]
        self.assertEqual(fb[0]["station_m"], 410.0)
        self.assertEqual(fb[0]["kind"], "footbridge", "the dossier's kind is kept")
        self.assertEqual(len(self.merged["crossings"]), 2)
        tower = self.merged["landmarks"]
        self.assertEqual(len(tower), 1)
        self.assertEqual(tower[0]["centre"], [120.0, 100.0])
        self.assertEqual(tower[0]["name"], "Tower")

    def test_woods_are_a_union_and_the_pit_lane_is_replaced(self):
        self.assertEqual(len(self.merged["woods"]), 2)
        self.assertEqual(self.merged["pit_lane"]["box_count"], 18)

    def test_unapply_restores_the_dossier_and_reapplying_is_idempotent(self):
        back = ac_layout.unapply(self.merged)
        self.assertEqual(json.dumps(back, sort_keys=True), json.dumps(self.layout, sort_keys=True))
        again, _ = ac_layout.merge(ac_layout.unapply(self.merged), self.overlay, "NoSuchStem")
        self.assertEqual(json.dumps(again, sort_keys=True), json.dumps(self.merged, sort_keys=True))

    def test_a_manual_pit_lane_wins(self):
        stem = next(iter(ac_layout.MANUAL_PIT_LANE))
        merged, rep = ac_layout.merge(copy.deepcopy(self.layout), self.overlay, stem)
        self.assertEqual(merged["pit_lane"], self.layout["pit_lane"])
        self.assertTrue(rep["pit_lane"].startswith("kept"))


class PitSpanTests(unittest.TestCase):
    def setUp(self):
        self.track = fake_track(kidney())
        pts = self.track.pts
        self.layout = {"attribution": "OSM", "stands": [], "structures": [], "crossings": [], "landmarks": [],
                       "woods": [], "pit_lane": {"side": "right", "length_m": 600.0,
                                                 "nodes": pts[0:300:6].round(2).tolist()}}

    def overlay(self, i0, i1, mode="auto"):
        lane = {"side": "right", "length_m": 0.0, "nodes": self.track.pts[i0:i1:6].round(2).tolist(),
                "source": "ac", "box_count": 20}
        return {"ac": {"folder": "x", "layout": "", "origin": "kunos"}, "pit_mode": mode,
                "entries": {"stands": [], "structures": [], "crossings": [], "landmarks": [], "woods": [],
                            "pit_lane": lane}}

    def test_a_lane_that_spans_the_dossiers_replaces_it(self):
        merged, rep = ac_layout.merge(self.layout, self.overlay(5, 310), "NoSuchStem", self.track)
        self.assertEqual(merged["pit_lane"]["source"], "ac")

    def test_a_shorter_lane_only_brings_its_garage_count(self):
        merged, rep = ac_layout.merge(self.layout, self.overlay(5, 150), "NoSuchStem", self.track)
        self.assertNotIn("source", merged["pit_lane"])
        self.assertEqual(merged["pit_lane"]["box_count"], 20)
        self.assertIn("exit", rep["pit_lane"])
        self.assertEqual(ac_layout.unapply(merged)["pit_lane"], self.layout["pit_lane"])

    def test_the_pairs_file_can_insist_on_acs_lane(self):
        merged, _ = ac_layout.merge(self.layout, self.overlay(5, 150, "ac"), "NoSuchStem", self.track)
        self.assertEqual(merged["pit_lane"]["source"], "ac")


class GeometryTests(unittest.TestCase):
    def test_overlap_of_boxes(self):
        a = ac_layout.rect((0, 0), 10, 10, 0.0)
        b = ac_layout.rect((5, 0), 10, 10, 0.0)
        self.assertAlmostEqual(ac_layout.overlap_ratio(a, b), 0.5, places=6)
        c = ac_layout.rect((0, 0), 4, 4, 0.3)
        self.assertAlmostEqual(ac_layout.overlap_ratio(a, c), 1.0, places=6)

    def test_a_patch_outline_is_its_boundary(self):
        cells = np.zeros((6, 6), dtype=bool)
        cells[1:4, 1:5] = True
        ring = ac_layout._outline(cells)
        self.assertAlmostEqual(ac_layout.poly_area(ring), 12.0)

    def test_components_weld_split_vertices(self):
        p = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 0, 0.001], [1, 1, 0], [5, 5, 0], [6, 5, 0], [5, 6, 0.0]])
        t = np.array([[0, 1, 2], [3, 4, 2], [5, 6, 7]])
        comps = sorted(sorted(c.tolist()) for c in features.components(t, p))
        self.assertEqual(comps, [[0, 1, 2, 3, 4], [5, 6, 7]])


if __name__ == "__main__":
    unittest.main()
