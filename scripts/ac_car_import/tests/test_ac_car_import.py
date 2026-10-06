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


class GeometryImportTest(unittest.TestCase):
    """suspensions.ini's points as the server's camber gain and toe."""

    @staticmethod
    def wishbones(upper_inner_x: float, upper_inner_y: float = 0.20) -> dict:
        return {"FRONT": {
            "TYPE": "DWB",
            "WBCAR_BOTTOM_FRONT": "0.30, -0.10, 0.20", "WBCAR_BOTTOM_REAR": "0.30, -0.10, -0.20",
            "WBTYRE_BOTTOM": "0.00, -0.10, 0.00",
            "WBCAR_TOP_FRONT": f"{upper_inner_x}, {upper_inner_y}, 0.20",
            "WBCAR_TOP_REAR": f"{upper_inner_x}, {upper_inner_y}, -0.20",
            "WBTYRE_TOP": "0.00, 0.20, 0.00",
            "WBTYRE_STEER": "0.00, 0.00, 0.10",
            "TOE_OUT": "0.001",
        }}

    def test_parallel_equal_arms_gain_nothing_and_a_short_upper_arm_does(self):
        parallel = physics.camber_gain(self.wishbones(0.30), "FRONT", 1.6)
        self.assertIsNotNone(parallel)
        self.assertAlmostEqual(parallel, 0.0, places=3)
        # A short upper arm inclined up to the upright: bump pulls its top in.
        short = physics.camber_gain(self.wishbones(0.15, 0.12), "FRONT", 1.6)
        self.assertTrue(0.1 < short <= 1.0, short)

    def test_a_strut_gains_some(self):
        strut = {"FRONT": {
            "TYPE": "STRUT",
            "WBCAR_BOTTOM_FRONT": "0.30, -0.10, 0.20", "WBCAR_BOTTOM_REAR": "0.30, -0.10, -0.20",
            "WBTYRE_BOTTOM": "0.00, -0.10, 0.00", "STRUT_CAR": "0.05, 0.45, 0.00",
        }}
        gain = physics.camber_gain(strut, "FRONT", 1.6)
        self.assertTrue(gain is not None and 0.0 < gain < 1.0, gain)
        self.assertIsNone(physics.camber_gain({"FRONT": {"TYPE": "DWB"}}, "FRONT", 1.6))

    def test_toe_out_over_the_steering_arm_is_toe_in_degrees(self):
        # 1 mm of TOE_OUT on a 10 cm arm: a third of a degree... of toe-out.
        toe = physics.toe_in_deg(self.wishbones(0.30), "FRONT")
        self.assertAlmostEqual(toe, -math.degrees(math.atan(0.001 / 0.1)), places=4)


class AeroPostureTest(unittest.TestCase):
    def test_the_heave_model_is_the_servers(self):
        # server/src/aero.rs the_bump_rubbers_hold_the_car_off_the_road:
        # 2 cm of free travel on a 4 cm car is 2 cm, and an unloaded axle
        # rises at most 5 cm.
        k = 80000.0
        self.assertAlmostEqual(physics.axle_travel(2 * k * 0.02, k, 0.04), 0.02, places=6)
        bumped = physics.axle_travel(2 * k * 0.06, k, 0.04)
        free = physics.FREE_TRAVEL_SHARE * 0.04
        self.assertAlmostEqual(bumped, free + (2 * k * 0.06 - 2 * k * free) / (2 * k * physics.BUMP_STIFFENING))
        self.assertEqual(physics.axle_travel(-1e6, k, 0.04), -physics.MAX_EXTENSION_M)

    def test_a_heave_spring_adds_half_its_rate_to_each_corner(self):
        susp = {"FRONT": {"SPRING_RATE": "40000"}, "HEAVE_FRONT": {"SPRING_RATE": "120000"},
                "REAR": {"SPRING_RATE": "30000"}}
        self.assertEqual(physics.wheel_spring_rate(susp, "FRONT")[0], 100000.0)
        self.assertEqual(physics.wheel_spring_rate(susp, "REAR")[0], 30000.0)
        self.assertEqual(physics.wheel_spring_rate({"FRONT": {}}, "FRONT")[0], None)

    def test_a_floor_table_gives_a_sensitivity_and_a_stall(self):
        floor = data.parse_lut("0|0\n0.01|0.3\n0.02|1.2\n0.04|1.1\n0.06|1.0\n0.1|0.8\n")
        wings = [physics.AeroWing(cl=lambda h: 0.6, z=0.0),
                 physics.AeroWing(cl=lambda h: 1.0 * floor(h), z=-1.0)]
        m = physics.aero_posture(wings, (0.06, 0.07), (80000.0, 70000.0), 1.4, -1.2, 2.6)
        front, rear = m["posture"]
        self.assertLess(front, 0.06)
        self.assertLess(rear, 0.07)
        self.assertGreater(m["ride_height_sensitivity"], 0.0, "lower is more downforce up here")
        self.assertGreater(m["stall_height_m"], 0.0)
        self.assertLess(m["stall_height_m"], 0.5 * (front + rear), "the stall is below where it runs")
        # A car of flat tables has neither.
        flat = physics.aero_posture([physics.AeroWing(cl=lambda h: 1.0, z=0.0)],
                                    (0.06, 0.07), (80000.0, 70000.0), 1.4, -1.2, 2.6)
        self.assertAlmostEqual(flat["ride_height_sensitivity"], 0.0)
        self.assertEqual(flat["stall_height_m"], 0.0)


class GroundEffectImportTest(unittest.TestCase):
    """The synthetic car with a floor, a heave spring and a tyre thermal
    curve: the server's `[aero]` and the tyre window come out of it."""

    @classmethod
    def setUpClass(cls):
        extra = {
            "aero.ini": DATA["aero.ini"] + """[WING_2]
NAME=DIFFUSER
CHORD=1
SPAN=2.0
POSITION=0,-0.2,-1.4
LUT_AOA_CL=body_cl.lut
LUT_AOA_CD=body_cd.lut
LUT_GH_CL=diffuser_gh.lut
ANGLE=0
CL_GAIN=5
""",
            "diffuser_gh.lut": "0|0\n0.01|0.3\n0.02|1.2\n0.04|1.1\n0.06|1.0\n0.1|0.8\n0.2|0.5\n",
            "suspensions.ini": DATA["suspensions.ini"] + "[HEAVE_FRONT]\nSPRING_RATE=60000\n",
            "tyres.ini": DATA["tyres.ini"] + "[THERMAL_FRONT]\nPERFORMANCE_CURVE=tcurve.lut\n",
            "tcurve.lut": "0|0.7\n20|0.8\n60|0.98\n70|1.0\n90|1.0\n120|0.96\n",
        }
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        from unittest import mock
        with mock.patch.dict(DATA, extra):
            car_dir = build_car(root / "ac", folder="test_floor")
        opts = cli.Options(custom_dir=root / "custom", default_dir=root / "default")
        (root / "default").mkdir()
        cls.result = cli.import_car(car_dir, opts)
        out = next((opts.custom_dir).iterdir())
        cls.toml = tomllib.loads((out / "car.toml").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_floor_becomes_the_servers_aero_map(self):
        a = self.toml["aero"]
        # Inside the server's validation ranges (car_loader.rs).
        self.assertTrue(0.005 <= a["ride_height_front_m"] <= 0.3)
        self.assertTrue(0.005 <= a["ride_height_rear_m"] <= 0.3)
        self.assertTrue(0.0 < a["ride_height_sensitivity"] <= 0.1)
        self.assertTrue(0.0 <= a["rake_sensitivity"] <= 0.05)
        self.assertTrue(0.0 < a["stall_height_m"] <= 0.1)

    def test_the_heave_spring_is_in_the_front_rate(self):
        self.assertEqual(self.toml["suspension"]["spring_rate_front_n_per_m"], 90000.0 + 30000.0)

    def test_the_thermal_curve_is_the_tyre_window(self):
        t = self.toml["tires"]
        self.assertEqual(t["optimal_temperature_c"], 80.0)
        self.assertEqual(t["temperature_window_c"], 10.0)
        self.assertNotIn("blanket_temperature_c", t, "a GT3 has no blankets")


class ErsImportTest(unittest.TestCase):
    """A synthetic hybrid laid out like Kunos' 2017 F1: the store from the
    discharge time, the lap budget from MAX_KJ_PER_LAP, the MGU-H share."""

    @classmethod
    def setUpClass(cls):
        extra = {
            "ers.ini": """[HEADER]
VERSION=1
[KINETIC]
TORQUE_CURVE=kers_torque.lut
COAST_CURVE=kers_torque_coast.lut
DISCHARGE_TIME=33330
HAS_BUTTON_OVERRIDE=1
MAX_KJ_PER_LAP=4000
[HEAT]
TORQUE_PERC=25
""",
            # 120 kW: 191 Nm at 6000 rpm.
            "kers_torque.lut": "1000|191\n6000|191\n12000|95.5\n15000|0\n",
            "kers_torque_coast.lut": "1000|100\n6000|100\n12000|50\n",
        }
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        from unittest import mock
        with mock.patch.dict(DATA, extra):
            car_dir = build_car(root / "ac", folder="test_ers")
        opts = cli.Options(custom_dir=root / "custom", default_dir=root / "default")
        (root / "default").mkdir()
        cli.import_car(car_dir, opts)
        out = next((opts.custom_dir).iterdir())
        cls.toml = tomllib.loads((out / "car.toml").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_store_the_budget_and_the_heat_recovery(self):
        h = self.toml["hybrid"]
        self.assertTrue(h["enabled"])
        self.assertAlmostEqual(h["motor_max_power_kw"], 120.0, delta=0.5)
        # 120 kW for 33.33 s: 4 MJ, 1.11 kWh.
        self.assertAlmostEqual(h["battery_capacity_kwh"], 1.111, delta=0.01)
        self.assertEqual(h["deploy_kj_per_lap"], 4000.0)
        self.assertAlmostEqual(h["heat_recovery_kw"], 30.0, delta=0.2)


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
        # The mirrors.ini glass: a slot of its own, chrome, no texture.
        self.assertEqual(mats["car_mirror_left"]["pbrMetallicRoughness"]["metallicFactor"], 1.0)
        self.assertNotIn("baseColorTexture", mats["car_mirror_left"]["pbrMetallicRoughness"])
        self.assertNotIn("MIRROR", mats)

    def test_the_mirror_glass_is_laid_out_for_the_rig(self):
        js = glb.read_glb_json(self.out / "TestCar.glb")
        mats = [m["name"] for m in js["materials"]]
        (prim,) = [p for p in js["meshes"][0]["primitives"] if mats[p["material"]] == "car_mirror_left"]
        pos = glb.read_glb_floats(self.out / "TestCar.glb", prim["attributes"]["POSITION"])
        uv = glb.read_glb_floats(self.out / "TestCar.glb", prim["attributes"]["TEXCOORD_0"])
        # The glass spans 0..1 both ways; u runs from the driver's right
        # (-x) to their left (+x), v from the top down.
        np.testing.assert_allclose(uv.min(axis=0), [0.0, 0.0], atol=1e-5)
        np.testing.assert_allclose(uv.max(axis=0), [1.0, 1.0], atol=1e-5)
        np.testing.assert_allclose(uv[:, 0], (pos[:, 0] - 0.95) / 0.1, atol=1e-4)
        np.testing.assert_allclose(uv[:, 1], (pos[:, 1].max() - pos[:, 1]) / 0.1, atol=1e-4)
        c = self.toml["cockpit"]
        self.assertEqual(c["mirror_left_size_cm"], [10.0, 10.0])
        self.assertNotIn("mirror_right_cm", c)

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


class MirrorPiecesTest(unittest.TestCase):
    """Kunos often model every mirror's glass as one mesh (the 787B's
    `MIRROR`): it is cut into pieces, each its own slot."""

    class Mesh:
        def __init__(self, name, geos):
            ps, ts, base = [], [], 0
            for p, t in geos:
                ps.append(p)
                ts.append(t + base)
                base += len(p)
            self.name, self.path = name, ("CAR",)
            self._p, self.triangles = np.concatenate(ps), np.concatenate(ts)

        def world_positions(self):
            return self._p

    class Car:
        def ini(self, name):
            return {"MIRROR_0": {"NAME": "MIRROR"}} if name == "mirrors.ini" else {}

    def test_one_mesh_of_three_glasses_is_three_mirrors(self):
        left = box((0.80, 0.8, 0.5), (1.00, 0.9, 0.52))
        right = box((-1.00, 0.8, 0.5), (-0.80, 0.9, 0.52))
        centre = box((-0.15, 1.0, 0.3), (0.15, 1.06, 0.31))
        mesh = self.Mesh("MIRROR", [left, right, centre])

        class Kn5:
            meshes = [mesh]

        mirrors = model.read_mirrors(self.Car(), Kn5(), np.array([0.3, 0.9, -0.3]))
        self.assertEqual(sorted(mirrors), ["car_mirror_centre", "car_mirror_left", "car_mirror_right"])
        np.testing.assert_allclose(mirrors["car_mirror_left"].size, (0.2, 0.1), atol=1e-6)
        np.testing.assert_allclose(mirrors["car_mirror_centre"].size, (0.3, 0.06), atol=1e-6)
        parts = model.mirror_parts(mirrors)[id(mesh)]
        self.assertEqual(sorted(len(rows) for _, rows in parts), [12, 12, 12])
        self.assertNotIn("", [s for s, _ in parts])


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
        # At the height it rides at 50 m/s (the server's reference), not at
        # rest: 2.42 m2 at rest, 2.45 at 55/49 mm on its springs.
        a = self.phys.fit["aero"]
        self.assertAlmostEqual(a["cl_area_m2"], 2.45, delta=0.02)
        self.assertAlmostEqual(a["cd_area_m2"], 0.99, delta=0.02)
        front, rear = a["posture_at_reference_m"]
        self.assertAlmostEqual(front, 0.055, delta=0.003)
        self.assertAlmostEqual(rear, 0.049, delta=0.003)

    def test_the_ride_height_map(self):
        # The front splitter's and the diffuser's LUT_GH_CL: ~2% a cm lower,
        # stalling around 25 mm (the diffuser's table falls off under 40).
        m = self.phys.fit["aero"]["map"]
        self.assertAlmostEqual(m["ride_height_sensitivity"], 0.018, delta=0.004)
        self.assertGreater(m["rake_sensitivity"], 0.0)
        self.assertAlmostEqual(m["stall_height_m"], 0.025, delta=0.004)
        self.assertEqual(self.phys.get("aero", "stall_height_m"), round(m["stall_height_m"], 4))

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
