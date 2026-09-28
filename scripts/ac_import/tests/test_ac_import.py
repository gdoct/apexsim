"""Tests for the AC importer: the readers on synthetic files, the frame's
handedness, the texture encoders against Pillow's decoder, and one whole
import of a synthetic oval through the same code path a real track takes.

    python -m unittest discover -s scripts/ac_import/tests -v
"""

from __future__ import annotations

import io
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

SCRIPTS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SCRIPTS))

from ac_import import ai, centerline, cli, export, frame, ini, kn5, physics, scene, textures, trigrid  # noqa: E402
from ac_import.sidecars import CONTACT_CURB, CONTACT_OFF, CONTACT_ROAD  # noqa: E402


def mat(name="m", shader="ksPerPixel", texture=None, **props):
    return kn5.Kn5Material(name=name, shader=shader, alpha_blend=0, alpha_tested=False, depth_mode=0,
                           props=dict(props), slots={"txDiffuse": texture} if texture else {})


def mesh_node(name, positions, triangles, material=0, uvs=None, lod_in=0.0, lod_out=0.0, renderable=True):
    """A kn5 mesh node in AC's frame, normals from the triangles (up for a
    floor wound as AC winds it)."""
    p = np.asarray(positions, dtype=np.float64)
    t = np.asarray(triangles, dtype=np.int64)
    n = np.zeros_like(p)
    a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]
    fn = np.cross(b - a, c - a)
    for i in range(3):
        np.add.at(n, t[:, i], fn)
    length = np.linalg.norm(n, axis=1, keepdims=True)
    length[length == 0] = 1
    n = n / length
    v = np.zeros(len(p), dtype=kn5.VERTEX_DTYPE)
    v["pos"] = p
    v["nrm"] = n
    v["uv"] = uvs if uvs is not None else p[:, [0, 2]] * 0.1
    v["tan"] = [1, 0, 0]
    return ("mesh", name, v, t, material, lod_in, lod_out, renderable)


def dummy(name, position, children=()):
    m = np.eye(4)
    m[3, :3] = position
    return ("dummy", name, m, list(children))


def write_ai(path: Path, positions_ac: np.ndarray, side_left: float, side_right: float) -> None:
    n = len(positions_ac)
    out = bytearray(struct.pack("<iiii", 7, n, 0, 0))
    length = 0.0
    for i, p in enumerate(positions_ac):
        if i:
            length += float(np.linalg.norm(positions_ac[i] - positions_ac[i - 1]))
        out += struct.pack("<ffffi", p[0], p[1], p[2], length, i)
    out += struct.pack("<i", n)
    for _ in range(n):
        extra = [40.0, 1, 0, 0, 200.0, side_left, side_right, 0, 1, 0, 1, 0, 1.5, 1, 0, 0, 0, 0]
        out += struct.pack("<18f", *extra)
    out += struct.pack("<i", 0)
    path.write_bytes(bytes(out))


class SyntheticOval:
    """An AC track folder: a 12 m road ring of radius 200 m, kerbs on the
    inside for a span, grass either side, a wall ring outside, the timing
    and grid markers, an AI line on the ring, and a small textured
    grandstand box. Built in AC's frame (y up, the world's -Y as z)."""

    RADIUS = 200.0
    WIDTH = 12.0

    def __init__(self, root: Path):
        self.root = root
        self.build()

    @staticmethod
    def ac(world_x, world_y, world_z):
        return np.array([world_x, world_z, -world_y], dtype=np.float64)

    def ring(self, r0, r1, z, segments=240):
        """A ring between radii as quads, wound with the normal up in AC's frame."""
        angles = np.linspace(0, 2 * np.pi, segments, endpoint=False)
        inner = [self.ac(r0 * np.cos(a), r0 * np.sin(a), z) for a in angles]
        outer = [self.ac(r1 * np.cos(a), r1 * np.sin(a), z) for a in angles]
        positions = np.array(inner + outer)
        tris = []
        for i in range(segments):
            j = (i + 1) % segments
            tris.append([i, j, segments + i])
            tris.append([j, segments + j, segments + i])
        tris = np.array(tris)
        # Wind so cross(b-a, c-a).y > 0 (up in AC's frame).
        a, b, c = positions[tris[:, 0]], positions[tris[:, 1]], positions[tris[:, 2]]
        flip = np.cross(b - a, c - a)[:, 1] < 0
        tris[flip] = tris[flip][:, [0, 2, 1]]
        return positions, tris

    def build(self):
        root = self.root
        (root / "ai").mkdir(parents=True)
        (root / "data").mkdir()
        (root / "ui").mkdir()
        r, w = self.RADIUS, self.WIDTH
        materials = [mat("Fisica"), mat("Asphalt", texture="asph.png"), mat("StandWall", texture="stand.png"),
                     mat("Erba", shader="ksMultilayer", texture="grass.png")]
        nodes = []
        # Physics: road, inside kerb over the first quarter, grass, walls.
        p, t = self.ring(r - w / 2, r + w / 2, 0.0)
        nodes.append(mesh_node("1ROAD_main", p, t, 0, renderable=False))
        p, t = self.ring(r - w / 2 - 1.0, r - w / 2, 0.02, segments=240)
        keep = np.arange(len(t)) < 120  # the first quarter's quads (60 segments x 2)
        nodes.append(mesh_node("1KERB_in", p, t[keep], 0, renderable=False))
        p, t = self.ring(r - 40.0, r - w / 2 - 1.0, -0.08)
        nodes.append(mesh_node("1GRASS_in", p, t, 0, renderable=False))
        p, t = self.ring(r + w / 2, r + 40.0, -0.08)
        nodes.append(mesh_node("1GRASS_out", p, t, 0, renderable=False))
        # A wall ring 20 m outside the road: vertical quads 1 m high.
        segments = 120
        angles = np.linspace(0, 2 * np.pi, segments, endpoint=False)
        base = [self.ac((r + 20) * np.cos(a), (r + 20) * np.sin(a), 0.0) for a in angles]
        top = [self.ac((r + 20) * np.cos(a), (r + 20) * np.sin(a), 1.0) for a in angles]
        wp = np.array(base + top)
        wt = []
        for i in range(segments):
            j = (i + 1) % segments
            wt.append([i, j, segments + i])
            wt.append([j, segments + j, segments + i])
        nodes.append(mesh_node("1WALL_ring", wp, np.array(wt), 0, renderable=False))
        # Visuals: the road drawn with a texture (becomes the kit road), a
        # grass plane (kit grass by shader), a stand box (scenery).
        p, t = self.ring(r - w / 2, r + w / 2, 0.01)
        nodes.append(mesh_node("asphalt_vis", p, t, 1, lod_out=0.0))
        p, t = self.ring(r + w / 2, r + 40.0, -0.07)
        nodes.append(mesh_node("erba_vis", p, t, 3))
        bx, by = r + 30.0, 0.0
        box = np.array([self.ac(bx + dx, by + dy, dz) for dx in (0, 6) for dy in (0, 10) for dz in (0, 4)])
        bt = np.array([[0, 2, 3], [0, 3, 1], [4, 5, 7], [4, 7, 6], [0, 1, 5], [0, 5, 4], [2, 6, 7], [2, 7, 3],
                       [0, 4, 6], [0, 6, 2], [1, 3, 7], [1, 7, 5]])
        nodes.append(mesh_node("Stand_01", box, bt, 2, lod_out=600.0))
        nodes.append(mesh_node("Stand_01_LOD", box, bt, 2, lod_in=600.0, lod_out=2000.0))
        # Markers: the start line across the road at angle 0 (the AI runs
        # counter-clockwise seen from above, so left is toward the centre).
        cube = np.array([self.ac(x, y, z) for x in (-0.5, 0.5) for y in (-0.5, 0.5) for z in (-0.5, 0.5)])
        ct = bt
        def marker(name, wx, wy, wz):
            return dummy(name, self.ac(wx, wy, wz), [mesh_node(name, cube, ct, 0)])
        nodes.append(marker("AC_TIME_0_L", r - w / 2, 0.0, 1.5))
        nodes.append(marker("AC_TIME_0_R", r + w / 2, 0.0, 1.5))
        a1 = 2 * np.pi / 3
        nodes.append(marker("AC_TIME_1_L", (r - w / 2) * np.cos(a1), (r - w / 2) * np.sin(a1), 1.5))
        nodes.append(marker("AC_TIME_1_R", (r + w / 2) * np.cos(a1), (r + w / 2) * np.sin(a1), 1.5))
        a2 = 4 * np.pi / 3
        nodes.append(marker("AC_TIME_2_L", (r - w / 2) * np.cos(a2), (r - w / 2) * np.sin(a2), 1.5))
        nodes.append(marker("AC_TIME_2_R", (r + w / 2) * np.cos(a2), (r + w / 2) * np.sin(a2), 1.5))
        for i in range(4):
            a = -(10.0 + 8.0 * i) / r
            side = 2.5 if i % 2 == 0 else -2.5
            nodes.append(marker(f"AC_START_{i}", (r + side) * np.cos(a), (r + side) * np.sin(a), 1.5))
        nodes.append(marker("AC_PIT_0", (r + 20.0), -50.0, 1.5))
        from PIL import Image
        def png(colour):
            im = Image.new("RGBA", (16, 16), colour)
            buf = io.BytesIO()
            im.save(buf, format="PNG")
            return buf.getvalue()
        texs = {"asph.png": png((60, 60, 60, 255)), "stand.png": png((200, 40, 40, 255)),
                "grass.png": png((40, 120, 30, 255))}
        kn5.write_kn5(root / "oval.kn5", texs, materials, nodes)
        (root / "models.ini").write_text("[MODEL_0]\nFILE=oval.kn5\nPOSITION=0,0,0\nROTATION=0,0,0\n", encoding="utf-8")
        (root / "data" / "surfaces.ini").write_text(
            "[SURFACE_0]\nKEY=ROAD\nFRICTION=0.98\nIS_VALID_TRACK=1\nIS_PITLANE=0\n"
            "[SURFACE_1]\nKEY=KERB\nFRICTION=0.95\nIS_VALID_TRACK=1\n"
            "[SURFACE_2]\nKEY=GRASS\nFRICTION=0.6\nIS_VALID_TRACK=0\n", encoding="utf-8")
        (root / "data" / "drs_zones.ini").write_text("[ZONE_0]\nDETECTION=0.10\nSTART=0.20\nEND=0.40\n", encoding="utf-8")
        (root / "ui" / "ui_track.json").write_text(
            '{\n\t"name": "Synthetic Oval",\n\t"country": "Nowhere",\n\t"city": "Test",\n\t"length": "1257",\n\t"pitboxes": "1",\n\t"run": "anticlockwise",\n}',
            encoding="utf-8")
        # The AI line on the ring, 1.5 m apart, offset 1 m to the outside so
        # the road-centre shift is exercised, starting 20 m before the line.
        n = int(round(2 * np.pi * r / 1.5))
        angles = np.linspace(0, 2 * np.pi, n, endpoint=False) - 20.0 / r
        pts = np.array([self.ac((r + 1.0) * np.cos(a), (r + 1.0) * np.sin(a), 0.3) for a in angles])
        write_ai(root / "ai" / "fast_lane.ai", pts, side_left=7.0, side_right=5.0)


class FrameTests(unittest.TestCase):
    def test_ac_to_world_is_a_rotation_about_x(self):
        np.testing.assert_allclose(frame.ac_to_world(np.array([1.0, 0, 0])), [1, 0, 0])
        np.testing.assert_allclose(frame.ac_to_world(np.array([0, 1.0, 0])), [0, 0, 1])
        np.testing.assert_allclose(frame.ac_to_world(np.array([0, 0, 1.0])), [0, -1, 0])
        m = frame.ac_to_world(np.eye(3))
        self.assertAlmostEqual(np.linalg.det(m), 1.0)

    def test_a_clockwise_ac_lap_is_clockwise_from_above(self):
        # Positive signed area in AC's raw (x, z) plane is what every Kunos
        # clockwise circuit shows; from above (Z up) that must be clockwise,
        # i.e. a negative signed area in (X, Y).
        a = np.linspace(0, 2 * np.pi, 100, endpoint=False)
        ac = np.column_stack([np.cos(a), np.zeros_like(a), np.sin(a)])
        area_xz = 0.5 * np.sum(ac[:, 0] * np.roll(ac[:, 2], -1) - np.roll(ac[:, 0], -1) * ac[:, 2])
        self.assertGreater(area_xz, 0)
        w = frame.ac_to_world(ac)
        area_xy = 0.5 * np.sum(w[:, 0] * np.roll(w[:, 1], -1) - np.roll(w[:, 0], -1) * w[:, 1])
        self.assertLess(area_xy, 0)

    def test_track_frame_puts_the_heading_on_x_and_left_on_y(self):
        f = frame.Frame(origin=np.array([10.0, 5.0, 1.0]), heading=np.pi / 2)
        np.testing.assert_allclose(f.points(np.array([10.0, 15.0, 1.0])), [10, 0, 0], atol=1e-9)
        np.testing.assert_allclose(f.points(np.array([0.0, 5.0, 1.0])), [0, 10, 0], atol=1e-9)
        self.assertAlmostEqual(f.yaw(np.pi / 2), 0.0)


class ReaderTests(unittest.TestCase):
    def test_kn5_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "t.kn5"
            m = np.eye(4)
            m[3, :3] = [10, 20, 30]
            verts = np.zeros(3, dtype=kn5.VERTEX_DTYPE)
            verts["pos"] = [[0, 0, 0], [1, 0, 0], [0, 0, 1]]
            verts["nrm"] = [0, 1, 0]
            nodes = [dummy("D", [10, 20, 30], [("mesh", "1ROAD_x", verts, np.array([[0, 2, 1]]), 0, 0.0, 400.0, True)])]
            kn5.write_kn5(path, {"tex.dds": b"DDS \x00\x00\x00\x00"}, [mat("Road", texture="tex.dds", ksSpecularEXP=40.0)], nodes)
            f = kn5.read_kn5(path)
            self.assertEqual([x.name for x in f.meshes], ["1ROAD_x"])
            self.assertEqual(f.meshes[0].lod_out, 400.0)
            np.testing.assert_allclose(f.meshes[0].world_positions()[1], [11, 20, 30])
            self.assertEqual(f.materials[0].texture(), "tex.dds")
            self.assertEqual([x.name for x in f.dummies], ["root", "D"])
            with self.assertRaises(kn5.EncryptedKn5):
                kn5.write_kn5(path, {"tex.dds": b"ENCRYPTED!!"}, [mat()], nodes)
                kn5.read_kn5(path)

    def test_ai_reader_offsets(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "fast_lane.ai"
            pts = np.array([[0, 0, 0], [1.5, 0, 0], [3.0, 0, 0]], dtype=np.float64)
            write_ai(path, pts, 4.0, 6.0)
            s = ai.read_ai(path)
            self.assertEqual(s.count, 3)
            self.assertAlmostEqual(float(s.side_left[1]), 4.0)
            self.assertAlmostEqual(float(s.side_right[2]), 6.0)
            self.assertAlmostEqual(float(s.length[-1]), 3.0)

    def test_ini_and_ui_json(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "models_gp.ini").write_text("[MODEL_1]\nFILE=b.kn5\n[MODEL_0]\nFILE=a.kn5 ; comment\n", encoding="utf-8")
            (root / "ui" / "gp").mkdir(parents=True)
            (root / "ui" / "gp" / "ui_track.json").write_bytes(b'\xef\xbb\xbf{"name": "T\xe9st",\t"length": "5.2 km", "run": "clockwise",}')
            layouts = ini.find_layouts(root)
            self.assertEqual([lay.name for lay in layouts], ["gp"])
            self.assertEqual(ini.model_files(layouts[0]), ["a.kn5", "b.kn5"])
            ui = ini.read_ui_track(layouts[0].ui_dir / "ui_track.json")
            self.assertEqual(ui.name, "Tést")
            self.assertEqual(ui.length_m, 5200.0)
            surfaces = ini.read_surfaces([])
            self.assertEqual(surfaces, {})

    def test_surface_keys_and_classes(self):
        surfaces = {"ROAD": ini.Surface("ROAD", 1.0, True, False), "GRASS": ini.Surface("GRASS", 0.6, False, False),
                    "KERB": ini.Surface("KERB", 0.92, True, False), "PITS": ini.Surface("PITS", 0.96, True, True),
                    "OUT": ini.Surface("OUT", 0.9, False, False), "TRM-ZNDV": ini.Surface("TRM-ZNDV", 0.98, True, False)}
        self.assertEqual(physics.surface_key_for("01TRM-ZNDV_03", surfaces), ("TRM-ZNDV", False))
        self.assertEqual(physics.surface_key_for("7KERB12", surfaces), ("KERB", False))
        self.assertEqual(physics.surface_key_for("05WALL01", surfaces), ("WALL", True))
        self.assertEqual(physics.surface_key_for("Rete_campo", surfaces), (None, False))
        self.assertEqual(physics.surface_key_for("3NOPE", surfaces), (None, False))
        self.assertEqual(physics.contact_for(surfaces["KERB"]), CONTACT_CURB)
        self.assertEqual(physics.contact_for(surfaces["GRASS"]), CONTACT_OFF)
        self.assertEqual(physics.contact_for(surfaces["OUT"]), 2)
        self.assertEqual(physics.contact_for(surfaces["PITS"]), 4)
        self.assertEqual(physics.contact_for(surfaces["ROAD"]), CONTACT_ROAD)


class TriangleIndexTests(unittest.TestCase):
    def test_highest_below_ceiling_wins(self):
        v = np.array([[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0],
                      [0, 0, 5], [10, 0, 5], [10, 10, 5], [0, 10, 5]], dtype=np.float64)
        t = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
        idx = trigrid.TriangleIndex(v, t)
        z, hit = idx.query(np.array([3.0, 3.0, 50.0]), np.array([3.0, 3.0, 3.0]), np.array([0.2, 6.0, 6.0]))
        self.assertAlmostEqual(z[0], 0.0)
        self.assertAlmostEqual(z[1], 5.0)
        self.assertTrue(np.isnan(z[2]) and hit[2] == -1)


class TextureTests(unittest.TestCase):
    def test_bc1_and_bc3_decode_with_pillow(self):
        from PIL import Image
        rgba = np.zeros((8, 8, 4), dtype=np.uint8)
        rgba[:, :4] = [200, 30, 30, 255]
        rgba[:, 4:] = [30, 30, 200, 128]
        dds1 = textures.write_dds_bytes(b"DXT1", 8, 8, [textures.encode_mip(rgba, b"DXT1")])
        im = Image.open(io.BytesIO(dds1)).convert("RGBA")
        px = np.asarray(im)
        self.assertLess(abs(int(px[0, 0, 0]) - 200), 12)
        self.assertLess(abs(int(px[0, 7, 2]) - 200), 12)
        dds5 = textures.write_dds_bytes(b"DXT5", 8, 8, [textures.encode_mip(rgba, b"DXT5")])
        im = Image.open(io.BytesIO(dds5)).convert("RGBA")
        px = np.asarray(im)
        self.assertEqual(int(px[0, 0, 3]), 255)
        self.assertLess(abs(int(px[0, 7, 3]) - 128), 4)

    def test_convert_makes_a_full_mip_chain(self):
        from PIL import Image
        im = Image.new("RGBA", (64, 32), (10, 200, 10, 255))
        buf = io.BytesIO()
        im.save(buf, format="PNG")
        conv = textures.convert_texture(buf.getvalue(), max_size=32)
        self.assertEqual((conv.width, conv.height), (32, 16))
        self.assertEqual(conv.fourcc, b"DXT1")
        self.assertEqual(conv.mips, textures.full_chain(32, 16))
        info = textures.dds_info(conv.data)
        self.assertEqual(info.mips, conv.mips)
        self.assertGreater(conv.average_linear[1], conv.average_linear[0])
        # A DXT5 with its mips is copied as it is.
        again = textures.convert_texture(conv.data, max_size=2048)
        self.assertTrue(again.how.startswith("copied"))
        self.assertEqual(again.data, conv.data)


class ImportTests(unittest.TestCase):
    def test_the_synthetic_oval_imports_and_is_deterministic(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            SyntheticOval(root / "synth_oval")
            custom = root / "custom"
            out = root / "build"
            layout = ini.find_layouts(root / "synth_oval")[0]
            opts = cli.Options(out_dir=out, custom_dir=custom, stem="SynthOval")
            result = cli.import_layout(layout, opts, None)
            self.assertTrue(result.ok, result.summary)
            for name in ("SynthOval.yaml", "SynthOval.ats", "SynthOval.road.msgpack", "SynthOval.walls.msgpack",
                         "SynthOval.ground.msgpack", "SynthOval.curbs.msgpack", "SynthOval.import.json"):
                self.assertTrue((custom / name).is_file(), name)
            for name in ("SynthOval.uescene.json", "SynthOval.uemesh", "previews/SynthOval.png"):
                self.assertTrue((out / name).is_file(), name)
            self.assertTrue((out / "SynthOval.textures" / "stand.dds").is_file())

            import yaml
            doc = yaml.safe_load((custom / "SynthOval.yaml").read_text(encoding="utf-8"))
            nodes = doc["nodes"]
            self.assertEqual(nodes[0]["x"], 0.0, "node 0 is on the start line")
            self.assertLessEqual(abs(nodes[0]["y"]), 0.13, "node 0 is the road's centre, within a sampling step")
            self.assertLess(abs(nodes[0]["z"]), 0.05, "the origin is on the road")
            widths = [n["width_left"] + n["width_right"] for n in nodes]
            self.assertLess(abs(np.median(widths) - 12.0), 0.6, "the road is 12 m wide on the mesh")
            self.assertLess(abs(nodes[0]["width_left"] - 6.0), 0.5, "the AI line's 1 m outside offset was removed")
            self.assertEqual(len(doc["sectors"]), 2)
            self.assertEqual(len(doc["spawn_points"]), 4)
            self.assertEqual(doc["closed_loop"], True)
            self.assertEqual(len(doc["drs_zones"]), 1)
            self.assertAlmostEqual(doc["metadata"]["length_m"], 2 * np.pi * 201.0, delta=3.0)
            # Counter-clockwise from above: the left edge (inside) is at +y.
            i = len(nodes) // 4
            self.assertGreater(nodes[i]["x"] * 0 + nodes[i]["y"], 0)

            report = json.loads((custom / "SynthOval.import.json").read_text(encoding="utf-8"))
            statuses = {c["name"]: c["status"] for c in report["checks"]}
            self.assertEqual(statuses["grid"], "pass")
            self.assertEqual(statuses["road_coverage"], "pass")
            self.assertEqual(statuses["centerline_on_road"], "pass")
            # Only the outside has a wall: every inside probe is open.
            walls_check = next(c for c in report["checks"] if c["name"] == "walls")
            self.assertEqual(walls_check["status"], "warn")
            self.assertEqual(walls_check["open_probes"] * 2, walls_check["probes"])
            keys = {m["key"]: m for m in report["materials"]}
            self.assertEqual(keys["road_ac"]["family"], "road")
            self.assertEqual(keys["ac_grass"]["ground_set"], "grass")
            self.assertTrue(any(k.startswith("scenery_standwall") for k in keys))
            surfaces = {s["key"]: s for s in report["physics"]["surfaces"]}
            self.assertEqual(surfaces["KERB"]["contact"], "curb")
            self.assertAlmostEqual(surfaces["KERB"]["friction_multiplier"], (0.95 / 0.98) / 0.85, places=3)
            self.assertEqual(report["scene"]["dropped_lower_lods"], 1)

            import msgpack
            curbs = msgpack.unpackb((custom / "SynthOval.curbs.msgpack").read_bytes(), raw=False)
            self.assertEqual(curbs["version"], 2)
            self.assertGreater(max(curbs["left_cm"][:200]), 60, "the inside kerb is on the left over the first quarter")
            self.assertEqual(max(curbs["right_cm"]), 0)
            walls = msgpack.unpackb((custom / "SynthOval.walls.msgpack").read_bytes(), raw=False)
            self.assertGreater(len(walls["segments"]), 100)
            road = msgpack.unpackb((custom / "SynthOval.road.msgpack").read_bytes(), raw=False)
            self.assertEqual(road["version"], 1)
            self.assertEqual(len(road["triangles"]), len(road["triangle_surface"]))
            manifest = json.loads((out / "SynthOval.uescene.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["version"], 3)
            self.assertEqual(manifest["imported"], "ac")
            self.assertEqual(len(manifest["grid"]), 4)
            stand = [m for m in manifest["meshes"] if m["material_key"].startswith("scenery_standwall")]
            self.assertTrue(stand and stand[0]["draw_distance_m"] == 600.0 and stand[0]["collision"] is False)

            # Byte-identical on a re-run.
            before = {p.name: p.read_bytes() for p in list(custom.iterdir()) + list(out.iterdir()) if p.is_file()}
            opts.force = True
            cli.import_layout(layout, opts, None)
            after = {p.name: p.read_bytes() for p in list(custom.iterdir()) + list(out.iterdir()) if p.is_file()}
            self.assertEqual(before.keys(), after.keys())
            for name in before:
                self.assertEqual(before[name], after[name], f"{name} changed between runs")

            # A second import of the same stem is refused without --force,
            # and the export manifest is refused nowhere.
            opts.force = False
            with self.assertRaises(cli.ImportError_):
                cli.import_layout(layout, opts, None)


if __name__ == "__main__":
    unittest.main()
