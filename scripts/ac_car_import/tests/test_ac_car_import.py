"""Tests for the AC car importer: data.acd on a synthetic file, the LUTs,
the turbo, and one whole import of a synthetic car through the same code
path a real one takes. With the Assetto Corsa install this machine has, the
911 GT3 R's figures from docs/AC_CAR_IMPORT.md are pinned too.

    python -m unittest discover -s scripts/ac_car_import/tests -v
"""

from __future__ import annotations

import io
import json
import math
import re
import struct
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

SCRIPTS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SCRIPTS))

from ac_import import kn5  # noqa: E402
from ac_car_import import acd, cli, data, glb, model, physics  # noqa: E402

AC_CARS = Path(r"E:\SteamLibrary\steamapps\common\assettocorsa\content\cars")
GT3R = AC_CARS / "ks_porsche_911_gt3_r_2016"


def png(colour, size=8, alpha=255) -> bytes:
    buf = io.BytesIO()
    Image.new("RGBA", (size, size), tuple(colour) + (alpha,)).save(buf, format="PNG")
    return buf.getvalue()


def encrypt_acd(files: dict[str, bytes], folder: str) -> bytes:
    """The inverse of `acd.decrypt`, for a synthetic data.acd."""
    key = acd.acd_key(folder).encode("ascii")
    out = bytearray(struct.pack("<ii", -1111, 1))
    for name, plain in files.items():
        out += struct.pack("<i", len(name)) + name.encode("latin-1") + struct.pack("<i", len(plain))
        for i, b in enumerate(plain):
            out += bytes([(b + key[i % len(key)]) % 256, 0, 0, 0])
    return bytes(out)


def mat(name, shader="ksPerPixel", texture=None, blend=0, tested=False, **props):
    return kn5.Kn5Material(name=name, shader=shader, alpha_blend=blend, alpha_tested=tested, depth_mode=0,
                           props=dict(props), slots={"txDiffuse": texture} if texture else {})


def verts(p: np.ndarray) -> np.ndarray:
    v = np.zeros(len(p), dtype=kn5.VERTEX_DTYPE)
    v["pos"] = p
    n = p - p.mean(axis=0)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    v["nrm"] = n
    v["uv"] = p[:, [0, 2]]
    v["tan"] = [1, 0, 0]
    return v


def box(lo, hi):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    p = np.array([[x, y, z] for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)], dtype=np.float64)
    t = np.array([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1],
                  [2, 3, 7], [2, 7, 6], [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])
    return p, t


def tyre(radius=0.33, width=0.3, n=24):
    """A tyre in its wheel dummy's frame: axle along x, centred."""
    a = np.linspace(0, 2 * np.pi, n, endpoint=False)
    ring = np.stack([np.zeros(n), radius * np.cos(a), radius * np.sin(a)], axis=1)
    p = np.concatenate([ring + [-width / 2, 0, 0], ring + [width / 2, 0, 0]])
    t = []
    for i in range(n):
        j = (i + 1) % n
        t += [[i, j, n + j], [i, n + j, n + i]]
    return p, np.array(t)


def node_mesh(name, geo, material, lod_in=0.0):
    p, t = geo
    return ("mesh", name, verts(p), t, material, lod_in, 0.0, True)


def dummy(name, position, children=(), axes=None):
    m = np.eye(4)
    if axes is not None:
        m[:3, :3] = axes
    m[3, :3] = position
    return ("dummy", name, m, list(children))


RAKE_DEG = 15.0

DATA = {
    # A negative STEER_RATIO (the rim turning the other way in AC) is the Audi S1's.
    "car.ini": """[BASIC]
TOTALMASS=1200
[GRAPHICS]
DRIVEREYES=0.35,0.85,-0.3
[CONTROLS]
STEER_LOCK=270
STEER_RATIO=-13.5
[FUEL]
MAX_FUEL=100
[RIDE]
PICKUP_FRONT_HEIGHT=-0.30
PICKUP_REAR_HEIGHT=-0.30
""",
    "engine.ini": """[HEADER]
POWER_CURVE=power.lut
[ENGINE_DATA]
LIMITER=8000
MINIMUM=1000
INERTIA=0.1
[COAST_REF]
RPM=8000
TORQUE=80
[TURBO_0]
MAX_BOOST=1.0
WASTEGATE=0.8
REFERENCE_RPM=4000
GAMMA=2
LAG_UP=0.99
LAG_DN=0.995
""",
    "power.lut": "0|100\n1000|200\n4000|300\n6000|320\n8000|280\n",
    "drivetrain.ini": """[TRACTION]
TYPE=AWD2
[AWD]
FRONT_SHARE=0.35
[GEARS]
COUNT=4
GEAR_R=-3.0
GEAR_1=3.0
GEAR_2=2.0
GEAR_3=1.4
GEAR_4=1.0
FINAL=4.0
[DIFFERENTIAL]
POWER=0.3
COAST=0.5
PRELOAD=50
[GEARBOX]
CHANGE_UP_TIME=80
""",
    "suspensions.ini": """[BASIC]
WHEELBASE=2.6
CG_LOCATION=0.45
[ARB]
FRONT=30000
REAR=20000
[FRONT]
BASEY=-0.1
TRACK=1.6
SPRING_RATE=90000
DAMP_BUMP=5000
DAMP_REBOUND=6000
[REAR]
BASEY=-0.1
TRACK=1.6
SPRING_RATE=80000
DAMP_BUMP=5000
DAMP_REBOUND=6000
""",
    "tyres.ini": """[COMPOUND_DEFAULT]
INDEX=0
[FRONT]
NAME=Race
RADIUS=0.33
WIDTH=0.28
DY0=1.5
DX0=1.45
LS_EXPY=0.8
FZ0=3000
SPEED_SENSITIVITY=0.002
PRESSURE_IDEAL=27
RATE=250000
[REAR]
NAME=Race
RADIUS=0.33
WIDTH=0.30
DY0=1.6
DX0=1.5
LS_EXPY=0.85
FZ0=3300
SPEED_SENSITIVITY=0.002
PRESSURE_IDEAL=27
RATE=250000
""",
    "brakes.ini": "[DATA]\nMAX_TORQUE=2000\nFRONT_SHARE=0.6\n",
    "electronics.ini": "[ABS]\nPRESENT=1\n[TRACTION_CONTROL]\nPRESENT=0\n",
    "aero.ini": """[WING_0]
NAME=BODY
CHORD=1
SPAN=2.0
POSITION=0,0,0
LUT_AOA_CL=body_cl.lut
LUT_AOA_CD=body_cd.lut
ANGLE=0
[WING_1]
NAME=REAR
CHORD=1
SPAN=1.0
POSITION=0,0.5,-1.17
LUT_AOA_CL=rear_cl.lut
LUT_AOA_CD=rear_cd.lut
ANGLE=10
""",
    "body_cl.lut": "0|0.1\n",
    "body_cd.lut": "0|0.35\n",
    "rear_cl.lut": "0|0.2\n20|1.0\n",
    "rear_cd.lut": "0|0.02\n20|0.12\n",
    "lights.ini": "[BRAKE_0]\nNAME=RearLight_0\nCOLOR=300,6,0\n[LIGHT_0]\nNAME=FrontLight_0\nCOLOR=300,300,320\n",
    "mirrors.ini": "[MIRROR_0]\nNAME=GEO_Mirror_L\n",
}


def build_car(root: Path, folder: str = "test_car", packed: bool = False) -> Path:
    car = root / folder
    car.mkdir(parents=True)
    files = {k: v.encode("latin-1") for k, v in DATA.items()}
    if packed:
        (car / "data.acd").write_bytes(encrypt_acd(files, folder))
    else:
        (car / "data").mkdir()
        for name, blob in files.items():
            (car / "data" / name).write_bytes(blob)
    (car / "ui").mkdir()
    (car / "ui" / "ui_car.json").write_text(json.dumps({
        "name": "Testa GT3 2020", "brand": "Testa", "class": "race", "tags": ["gt3", "rwd", "italy"],
        "description": "A 4.0 litre V8 test car.",
    }))
    for skin, colour in (("a_red", (200, 20, 20)), ("b_blue", (20, 20, 200))):
        d = car / "skins" / skin
        d.mkdir(parents=True)
        (d / "Skin_00.dds").write_bytes(png(colour, 16))
        (d / "ui_skin.json").write_text(json.dumps({"skinname": f"Skin {skin}"}))
        if skin == "b_blue":
            (d / "preview.jpg").write_bytes(png((1, 2, 3)))
            (d / "Banner.dds").write_bytes(png((0, 200, 0), 8, 128))

    materials = [
        mat("EXT_Carpaint", "ksPerPixelMultiMap", "Skin_00.dds", fresnelMaxLevel=0.6, ksSpecularEXP=50),
        mat("EXT_Flat_Light", "ksPerPixel", "lights.dds"),
        mat("EXT_Tyre", "ksTyres", "tyre.dds"),
        mat("EXT_Rim", "ksPerPixel", "rim.dds"),
        mat("INT_Steer", "ksPerPixel", "steer.dds"),
        mat("EXT_Banner", "ksPerPixel", "Banner.dds", blend=1),
        mat("MIRROR", "ksPerPixelReflection", None, fresnelMaxLevel=1.0, fresnelC=1.0),
    ]
    textures = {"Skin_00.dds": png((128, 128, 128), 16), "lights.dds": png((255, 255, 255)),
                "tyre.dds": png((20, 20, 20)), "rim.dds": png((180, 180, 180)), "steer.dds": png((40, 40, 40)),
                "Banner.dds": png((255, 255, 0), 8, 100)}
    s, c = math.sin(math.radians(RAKE_DEG)), math.cos(math.radians(RAKE_DEG))
    steer_axes = np.array([[1, 0, 0], [0, c, s], [0, -s, c]])  # column (z) forward and down
    wheels = []
    for corner, x, z in (("LF", 0.8, 1.3), ("RF", -0.8, 1.3), ("LR", 0.8, -1.3), ("RR", -0.8, -1.3)):
        wheels.append(dummy(f"WHEEL_{corner}", (x, 0.23, z), [
            node_mesh(f"GEO_TYRE_{corner}", tyre(), 2),
            node_mesh(f"GEO_RIM_{corner}", tyre(0.25, 0.2), 3),
            dummy(f"RIM_BLUR_{corner}", (x, 0.23, z), [node_mesh(f"GEO_BLUR_{corner}", tyre(0.25, 0.1), 3)]),
        ]))
    nodes = [
        node_mesh("GEO_Body", box((-0.95, 0.0, -2.2), (0.95, 1.1, 2.2)), 0),
        node_mesh("GEO_Antenna", box((-0.005, 1.1, -0.5), (0.005, 1.5, -0.45)), 0),
        node_mesh("GEO_Banner", box((-0.5, 1.0, 0.6), (0.5, 1.05, 0.7)), 5),
        dummy("REAR_LIGHT", (0, 0, 0), [node_mesh("RearLight_0", box((-0.8, 0.6, -2.25), (-0.5, 0.7, -2.2)), 1)]),
        dummy("FRONT_LIGHT", (0, 0, 0), [node_mesh("FrontLight_0", box((0.5, 0.5, 2.2), (0.8, 0.6, 2.25)), 1)]),
        node_mesh("GEO_Mirror_L", box((0.95, 0.8, 0.5), (1.05, 0.9, 0.55)), 6),
        *wheels,
        dummy("COCKPIT_HR", (0, 0, 0), [
            dummy("STEER_HR", (0.35, 0.65, 0.2), [node_mesh("GEO_Steer", box((-0.15, -0.15, -0.02), (0.15, 0.15, 0.02)), 4)],
                  axes=steer_axes),
        ]),
        dummy("COCKPIT_LR", (0, 0, 0), [node_mesh("GEO_COCKPIT_LR", box((-0.5, 0.2, -0.5), (0.5, 0.8, 0.5)), 4)]),
        dummy("DAMAGE_GLASS_1", (0, 0, 0), [node_mesh("DAMAGE_GLASS_MESH", box((-0.5, 0.9, 0.5), (0.5, 1.0, 0.6)), 5)]),
        node_mesh("GEO_Far_Lod", box((-1, 0, -1), (1, 1, 1)), 0, lod_in=50.0),
    ]
    kn5.write_kn5(car / "test_car.kn5", textures, materials, nodes)
    return car


class AcdTest(unittest.TestCase):
    def test_the_key_is_the_one_recovered_from_the_911s_ciphertext(self):
        self.assertEqual(acd.acd_key("ks_porsche_911_gt3_r_2016"), "145-191-144-93-26-0-15-55")
        self.assertEqual(acd.acd_key("KS_PORSCHE_911_GT3_R_2016"), acd.acd_key("ks_porsche_911_gt3_r_2016"))

    def test_a_packed_car_reads_and_a_renamed_one_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = {"car.ini": b"[BASIC]\nTOTALMASS=1000\n", "power.lut": b"0|100\n"}
            path = Path(tmp) / "some_car" / "data.acd"
            path.parent.mkdir()
            path.write_bytes(encrypt_acd(files, "some_car"))
            self.assertEqual(acd.read_acd(path), files)
            with self.assertRaises(acd.AcdError):
                acd.read_acd(path, "renamed_car")


class DataTest(unittest.TestCase):
    def test_a_lut_interpolates_and_holds_its_ends(self):
        lut = data.parse_lut("; comment\n0|0\n10|1.0 ; trailing\n\n20\t|\t3\n")
        self.assertEqual(lut(5), 0.5)
        self.assertEqual(lut(-5), 0.0)
        self.assertEqual(lut(30), 3.0)

    def test_a_key_on_the_header_line_is_read(self):
        from ac_import.ini import parse_ini
        self.assertEqual(parse_ini("[REAR]NAME=Road\nRADIUS=0.33\n")["REAR"], {"NAME": "Road", "RADIUS": "0.33"})

    def test_a_heave_spring_stands_in_for_zero_corner_springs(self):
        susp = {"FRONT": {"SPRING_RATE": "0"}, "HEAVE_FRONT": {"SPRING_RATE": "80000"},
                "REAR": {"SPRING_RATE": "94000"}}
        self.assertEqual(physics.wheel_spring_rate(susp, "FRONT")[0], 40000.0)
        self.assertEqual(physics.wheel_spring_rate(susp, "REAR")[0], 94000.0)

    def test_boost_builds_to_the_reference_rpm_and_the_wastegate_caps_it(self):
        t = [physics.Turbo(max_boost=1.0, wastegate=0.8, reference_rpm=4000, lag_up=0.99, lag_dn=0.99)]
        self.assertAlmostEqual(physics.boost_at(t, 2000), 0.5)
        self.assertAlmostEqual(physics.boost_at(t, 6000), 0.8)
        self.assertAlmostEqual(physics.lag_seconds(0.99), -(1 / 333) / math.log(0.99))

    def test_turbos_a_controller_lights_in_one_gear_each_are_not_summed(self):
        # The 488 GTB's scheme: a turbo per gear, its wastegate an rpm LUT
        # times a gear LUT that is 0.5 in its own gear and 0 elsewhere.
        def turbo(gear):
            rpm = physics.Controller("RPMS", "ADD", physics.parse_inline_lut("(|0=0|2000=1.2|)"), None, None)
            gears = physics.Controller("GEAR", "MULT", physics.parse_inline_lut(
                "(|" + "|".join(f"{g}={0.5 if g == gear else 0}" for g in range(4)) + "|)"), None, None)
            return physics.Turbo(1.4, 0.7, 1000, 0.99, 0.99, [rpm, gears])
        self.assertAlmostEqual(physics.boost_at([turbo(1), turbo(2), turbo(3)], 3000, gears=3), 0.6)


class ThermalWindowTest(unittest.TestCase):
    def test_the_plateau_is_the_window_and_the_shoulders_the_falloff(self):
        # The 911 GT3 R's tcurve_slicksGT3s.lut.
        curve = data.parse_lut(
            "0|0.7\n20|0.8\n45|0.9\n60|0.98\n70|1.0\n80|1.0\n85|1.0\n100|0.97\n"
            "140|0.95\n160|0.95\n180|0.95\n240|0.8\n300|0.6\n")
        optimum, window, falloff = physics.thermal_window(curve)
        self.assertEqual((optimum, window), (77.5, 7.5))
        self.assertTrue(0.002 < falloff < 0.006, falloff)

    def test_a_flat_curve_has_a_wide_window_and_no_falloff(self):
        optimum, window, falloff = physics.thermal_window(data.parse_lut("0|1\n200|1\n"))
        self.assertEqual((optimum, window, falloff), (100.0, 50.0, 0.0))


class SyntheticImportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.car_dir = build_car(root / "ac")
        cls.opts = cli.Options(custom_dir=root / "custom", default_dir=root / "default")
        (root / "default").mkdir()
        cls.result = cli.import_car(cls.car_dir, cls.opts)
        cls.out = cls.opts.custom_dir / "TestCar"
        cls.toml_text = (cls.out / "car.toml").read_text(encoding="utf-8")
        cls.toml = tomllib.loads(cls.toml_text)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_import_passes_its_checks(self):
        failed = [c for c in self.result.checks if not c["ok"]]
        self.assertTrue(self.result.ok, failed)

    def test_car_toml_has_every_key_the_server_requires(self):
        t = self.toml
        self.assertEqual(t["imported"], "ac")
        for key in ("id", "name", "version", "model"):
            self.assertIn(key, t)
        for key in ("mass_kg", "max_engine_force_n", "max_brake_force_n", "drag_coefficient", "grip_coefficient",
                    "max_steering_angle_rad", "wheelbase_m"):
            self.assertGreater(t["physics"][key], 0, key)
        self.assertEqual(t["id"], cli.car_id_for("test_car"))
        self.assertEqual(t["class"], "GT3")
        self.assertEqual(t["model_year"], 2020)

    def test_car_toml_is_laid_out_for_the_clients_line_reader(self):
        for line in self.toml_text.splitlines():
            if line.startswith("["):
                self.assertRegex(line, r"^\[\[?[a-z_.]+\]\]?$", "a comment after a header breaks the client")
            self.assertFalse(line.rstrip().endswith(("[", ",")), f"an array must stay on one line: {line}")

    def test_the_physics_mapping(self):
        p = self.toml["physics"]
        self.assertAlmostEqual(p["max_steering_angle_rad"], round(math.radians(270 / 13.5), 4))
        # 2 x T x (share / r + rest / r)
        self.assertAlmostEqual(p["max_brake_force_n"], round(2 * 2000 * (0.6 / 0.33 + 0.4 / 0.33)), delta=1)
        # grip at the reference: DY0 x (1 - 0.002 x 40), axle mean; scales keep the balance.
        mu_f, mu_r = 1.5 * 0.92, 1.6 * 0.92
        self.assertAlmostEqual(p["grip_coefficient"], round((mu_f + mu_r) / 2, 4))
        tires = self.toml["tires"]
        self.assertAlmostEqual(tires["front_grip_scale"] * p["grip_coefficient"], mu_f, places=3)
        self.assertAlmostEqual(tires["optimal_pressure_kpa"], round(27 * physics.PSI_TO_KPA, 1))
        self.assertEqual(tires["reference_load_rear_n"], 3300.0)
        # The turbo is baked into the curve: 300 Nm x (1 + 0.8) at 4000 rpm.
        curve = {c["rpm"]: c["torque_nm"] for c in self.toml["engine"]["torque_curve"]}
        self.assertAlmostEqual(curve[4000.0], 540.0)
        self.assertIn("turbo", self.toml["engine"])
        self.assertEqual(self.toml["drivetrain"], {"layout": "AWD", "awd_front_share": 0.35})
        self.assertEqual(self.toml["transmission"]["gear_ratios"], [-3.0, 3.0, 2.0, 1.4, 1.0])
        self.assertTrue(self.toml["differential"]["simulated"])
        # Aero: body Cl 0.1 x 2 m2 at the CG + rear wing Cl 0.6 x 1 m2 on the
        # rear axle (CG_LOCATION 0.45 of 2.6 m: 1.17 m behind the CG).
        cla = -(p["lift_coefficient_front"] + p["lift_coefficient_rear"]) * p["frontal_area_m2"]
        self.assertAlmostEqual(cla, 0.8, places=3)
        self.assertAlmostEqual(p["lift_coefficient_rear"], round(-(0.1 * 2 * 0.55 + 0.6) / 2, 4), places=3)
        self.assertEqual(p["drs_drag_reduction"], 0.0)

    def test_the_body_is_seated_and_the_aerial_does_not_set_its_height(self):
        lo, hi = self._bounds("TestCar.glb")
        # Hubs at 0.23 on a 0.33 tyre: lifted 0.10.
        self.assertAlmostEqual(lo[1], 0.10, places=3)
        self.assertAlmostEqual(self.toml["physics"]["height_m"], 1.2, delta=0.03)
        self.assertGreater(hi[1], 1.55)  # the aerial is still drawn
        self.assertAlmostEqual(lo[2], -2.25, places=3)

    def test_the_wheels_are_split_off_in_their_own_frame(self):
        lo, hi = self._bounds("wheels/front.glb")
        np.testing.assert_allclose(lo, [-0.15, -0.33, -0.33], atol=2e-3)
        np.testing.assert_allclose(hi, [0.15, 0.33, 0.33], atol=2e-3)
        w = self.toml["wheels"]
        self.assertEqual((w["model"], w["rear_model"]), ("wheels/front.glb", "wheels/rear.glb"))
        self.assertAlmostEqual(w["front_axle_m"], 1.3, places=3)
        self.assertAlmostEqual(w["rear_axle_m"], -1.3, places=3)
        self.assertAlmostEqual(w["front_track_m"], 1.6, places=3)
        self.assertAlmostEqual(w["front_radius_m"], 0.33, places=3)
        self.assertAlmostEqual(w["front_width_m"], 0.3, places=3)
        names = [m["name"] for m in glb.read_glb_json(self.out / "wheels/front.glb")["materials"]]
        self.assertEqual(sorted(names), ["wheel_rim", "wheel_tyre"])

    def test_dropped_parts_are_not_drawn(self):
        js = glb.read_glb_json(self.out / "TestCar.glb")
        tris = sum(js["accessors"][p["indices"]]["count"] // 3 for p in js["meshes"][0]["primitives"])
        # body, aerial, banner, two lamps, the mirror: six boxes, nothing else.
        self.assertEqual(tris, 6 * 12)
        dropped = json.loads((self.out / "TestCar.import.json").read_text())["parts"]["dropped"]
        self.assertEqual(set(dropped), {"the pre-blurred spinning rim", "the low-detail cockpit",
                                        "broken glass (damage)", "a far LOD stand-in (lodIn > 0)"})

    def test_slots_the_game_drives_are_named_for_it(self):
        mats = {m["name"]: m for m in glb.read_glb_json(self.out / "TestCar.glb")["materials"]}
        self.assertIn("KHR_materials_clearcoat", mats["car_skin"]["extensions"])
        self.assertEqual(mats["car_brakelight"]["emissiveFactor"][0], 1.0)
        self.assertIn("car_headlight", mats)
        self.assertEqual(mats["EXT_Banner"]["alphaMode"], "BLEND")
        self.assertEqual(mats["MIRROR"]["pbrMetallicRoughness"]["metallicFactor"], 1.0)

    def test_the_steering_wheel_is_upright_in_its_file_and_raked_by_the_cockpit_table(self):
        lo, hi = self._bounds("steering_wheel.glb")
        np.testing.assert_allclose(lo, [-0.15, -0.15, -0.02], atol=1e-4)
        c = self.toml["cockpit"]
        self.assertAlmostEqual(c["wheel_rake_deg"], -RAKE_DEG, places=2)
        self.assertEqual(c["wheel_lock_deg"], 270.0)
        self.assertEqual(c["steering_wheel_model"], "steering_wheel.glb")
        # The actor frame: +X nose, +Y right, +Z up, cm; the wheel dummy at
        # x 0.35 (left), y 0.65 + the 0.10 seat, z 0.2.
        self.assertEqual(c["wheel_cm"], [20.0, -35.0, 75.0])
        self.assertEqual(c["eye_cm"], [-30.0, -35.0, 95.0])
        self.assertEqual(c["style"], "closed")
        self.assertIn("mirror_left_cm", c)

    def test_the_other_skin_is_a_texture_livery(self):
        (liv,) = self.toml["livery"]
        self.assertEqual(liv["name"], "Skin b_blue")
        self.assertEqual(liv["skin"], "skins/b_blue/Skin_00.jpg")
        self.assertEqual(liv["preview"], "skins/b_blue/preview.jpg")
        self.assertEqual(liv["textures"], ["EXT_Banner=skins/b_blue/Banner.png"])
        for rel in (liv["skin"], liv["preview"], "skins/b_blue/Banner.png"):
            self.assertTrue((self.out / rel).is_file(), rel)
        # Livery 0 is the first skin, baked in: red.
        self.assertEqual(json.loads((self.out / "TestCar.import.json").read_text())["skins"]["default"], "a_red")

    def test_rerunning_writes_the_same_bytes_and_needs_force(self):
        before = {p.relative_to(self.out): p.read_bytes() for p in self.out.rglob("*") if p.is_file()}
        with self.assertRaises(cli.ImportError_):
            cli.import_car(self.car_dir, self.opts)
        opts = cli.Options(custom_dir=self.opts.custom_dir, default_dir=self.opts.default_dir, force=True)
        cli.import_car(self.car_dir, opts)
        after = {p.relative_to(self.out): p.read_bytes() for p in self.out.rglob("*") if p.is_file()}
        self.assertEqual(sorted(before), sorted(after))
        for rel in before:
            self.assertEqual(before[rel], after[rel], str(rel))

    def test_a_packed_car_imports_like_an_unpacked_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            car = build_car(Path(tmp) / "ac", packed=True)
            opts = cli.Options(custom_dir=Path(tmp) / "custom", default_dir=Path(tmp) / "default", dry_run=True)
            r = cli.import_car(car, opts)
            self.assertTrue(r.ok)

    def test_a_shipped_folder_name_is_refused(self):
        (self.opts.default_dir / "TestCar").mkdir(exist_ok=True)
        try:
            with self.assertRaises(cli.ImportError_):
                cli.import_car(self.car_dir, cli.Options(custom_dir=self.opts.custom_dir,
                                                         default_dir=self.opts.default_dir, force=True))
        finally:
            (self.opts.default_dir / "TestCar").rmdir()

    def _bounds(self, rel):
        js = glb.read_glb_json(self.out / rel)
        acc = js["accessors"]
        prims = js["meshes"][0]["primitives"]
        lo = np.min([acc[p["attributes"]["POSITION"]]["min"] for p in prims], axis=0)
        hi = np.max([acc[p["attributes"]["POSITION"]]["max"] for p in prims], axis=0)
        return lo, hi


class Kn5PathTest(unittest.TestCase):
    def test_a_mesh_knows_every_dummy_above_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            car = build_car(Path(tmp))
            k = kn5.read_kn5(car / "test_car.kn5", textures=False)
            blur = next(m for m in k.meshes if m.name == "GEO_BLUR_LF")
            self.assertEqual(blur.path, ("root", "WHEEL_LF", "RIM_BLUR_LF"))


@unittest.skipUnless(GT3R.is_dir(), "no Assetto Corsa install with the 911 GT3 R on this machine")
class Porsche911Gt3RTest(unittest.TestCase):
    """The figures docs/AC_CAR_IMPORT.md quotes for the Kunos 911 GT3 R."""

    @classmethod
    def setUpClass(cls):
        cls.phys = physics.map_physics(data.read_car(GT3R))

    def test_aero(self):
        a = self.phys.fit["aero"]
        self.assertAlmostEqual(a["cl_area_m2"], 2.42, delta=0.02)
        self.assertAlmostEqual(a["cd_area_m2"], 0.99, delta=0.01)

    def test_brakes_steering_and_grip(self):
        self.assertAlmostEqual(self.phys.get("physics", "max_brake_force_n"), 22921, delta=2)
        self.assertAlmostEqual(self.phys.get("physics", "max_steering_angle_rad"), 0.5027, places=4)
        self.assertAlmostEqual(self.phys.get("physics", "grip_coefficient"), 1.4943, places=3)
        self.assertEqual(self.phys.car_class, "GT3")

    def test_the_steering_column_runs_forward_and_down(self):
        k = kn5.read_kn5(GT3R / "porsche_911_gt3_r.kn5", textures=False)
        self.assertAlmostEqual(model.column_rake_deg(model.steering_frame(k)), -15.58, places=1)


if __name__ == "__main__":
    unittest.main()
