"""`python scripts/ac_car_import.py <ac-car-folder> [options]`: one command
per AC car, writing `content/cars/custom/<Stem>/` (car.toml, the GLBs, the
skins and the report) and checking the result. See docs/AC_CAR_IMPORT.md.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import sys
import uuid
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

SCRIPTS = Path(__file__).resolve().parent.parent
REPO = SCRIPTS.parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from ac_import.kn5 import EncryptedKn5, Kn5Error, read_kn5  # noqa: E402

from . import TOOL_VERSION, liveries, model, physics  # noqa: E402
from .acd import AcdError  # noqa: E402
from .data import CarData, lod_kn5, main_kn5, read_car, vector  # noqa: E402
from .glb import read_glb_json  # noqa: E402
from .toml_out import render  # noqa: E402

CARS_DIR = REPO / "content" / "cars"
DEFAULT_DIR = CARS_DIR / "default"
CUSTOM_DIR = CARS_DIR / "custom"
NOTICE_MARKER = REPO / ".cache" / "ac_car_import" / "notice_shown"
#: UUID v5 namespace of imported cars' ids: two players importing the same
#: car get the same id, which is what lets the content checksum compare them.
CAR_ID_NAMESPACE = uuid.UUID("5f0c3f5e-2b7a-4f53-8d61-6c1e0a9b7d24")

TRIANGLE_BUDGET = 400_000
TEXTURE_BUDGET_MB = 300

NOTICE = """\
ac_car_import converts a car from your own Assetto Corsa install for your own
use on this machine. The result is written to content/cars/custom, which
ApexSim never packages or sends anywhere. It stays yours: do not redistribute
it, and note that some mod authors' terms forbid converting their work to
other games. Cars encrypted by Custom Shaders Patch are refused.
"""


class ImportError_(Exception):
    """A car that cannot be imported, with the reason."""


@dataclass
class Options:
    stem: str | None = None
    display_name: str | None = None
    car_class: str | None = None
    skin: str | None = None
    compound: int | None = None
    lod: str = "A"
    max_texture: int = 1024
    max_skin_texture: int = 2048
    cylinders: int | None = None
    keep_steering_wheel: bool = False
    force: bool = False
    dry_run: bool = False
    custom_dir: Path = CUSTOM_DIR
    default_dir: Path = DEFAULT_DIR


@dataclass
class Result:
    stem: str
    ok: bool
    summary: str
    checks: list[dict] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def camel(text: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", text)
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


def default_stem(car_dir: Path) -> str:
    return camel(car_dir.name) or "AcCar"


def car_id_for(folder: str) -> str:
    return str(uuid.uuid5(CAR_ID_NAMESPACE, f"ac-car:{folder.lower()}"))


def crc(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def show_notice_once() -> None:
    if NOTICE_MARKER.exists():
        return
    print(NOTICE)
    try:
        NOTICE_MARKER.parent.mkdir(parents=True, exist_ok=True)
        NOTICE_MARKER.write_text("shown\n", encoding="utf-8")
    except OSError:
        pass


# --- sound ----------------------------------------------------------------

ENGINE_WORDS = (
    (r"\bw16\b", 16, False), (r"\bv12\b|twelve.cylinder", 12, False), (r"\bv10\b|ten.cylinder", 10, False),
    (r"\bv8\b|eight.cylinder", 8, True), (r"flat.?six|boxer.?six|flat 6|straight.?six|inline.?six|\bi6\b|six.cylinder", 6, False),
    (r"\bv6\b", 6, False), (r"flat.?four|boxer|inline.?four|\bi4\b|four.cylinder|4.cylinder", 4, False),
    (r"\brotary\b|wankel", 2, False), (r"\bv4\b", 4, False), (r"inline.?five|five.cylinder|\bi5\b", 5, False),
    (r"three.cylinder|\bi3\b", 3, False),
)


def guess_engine(car: CarData) -> tuple[int | None, bool, str]:
    text = f"{car.ui.name} {car.ui.description} {' '.join(car.ui.tags)}".lower()
    for rx, cyl, crossplane in ENGINE_WORDS:
        if re.search(rx, text):
            return cyl, crossplane, re.search(rx, text).group(0)
    if re.search(r"\b911\b|cayman|boxster", text):
        return 6, False, "a Porsche flat-six"
    return None, True, ""


def sound_table(car: CarData, phys: physics.Physics, cylinders: int | None) -> dict:
    cyl, crossplane, why = guess_engine(car)
    if cylinders:
        cyl, why = cylinders, "--cylinders"
    race = phys.car_class not in ("Street",) and "street" not in car.ui.car_class.lower()
    if cyl is None:
        cyl = 6 if phys.car_class == "F1" else 8
        why = "not named: the class default"
    return {
        "cylinders": int(cyl),
        "crossplane": bool(crossplane and cyl == 8),
        "turbo": bool(phys.turbo),
        "exhaust_length_m": 1.0,
        "muffling": 0.25 if race else 0.6,
        "pops": 0.6 if race else 0.25,
        "gear_whine": 0.35 if race else 0.05,
        "intake_roar": 0.6,
    }, why


def ui_power_kw(car: CarData) -> float | None:
    """ui_car.json's own power figure (`"bhp": "500hp"`), for a sanity check."""
    text = str(car.ui.specs.get("bhp", ""))
    m = re.match(r"\s*([\d.,]+)\s*(kw|bhp|hp|ps|cv)?", text.lower())
    if not m:
        return None
    try:
        value = float(m.group(1).replace(",", ""))
    except ValueError:
        return None
    if value <= 0:
        return None
    return value if m.group(2) == "kw" else value * (0.7355 if m.group(2) in ("ps", "cv") else 0.7457)


# --- the import -----------------------------------------------------------

def import_car(car_dir: Path, opts: Options) -> Result:
    car_dir = Path(car_dir)
    warnings: list[str] = []
    stem = opts.stem or default_stem(car_dir)
    if not re.match(r"^[A-Za-z][A-Za-z0-9_\-]*$", stem):
        raise ImportError_(f"stem {stem!r} must be letters, digits, _ or -")
    out_dir = opts.custom_dir / stem
    if (opts.default_dir / stem).exists():
        raise ImportError_(f"{stem!r} is a shipped car's folder; pick another with --stem")
    car_id = car_id_for(car_dir.name)
    for toml in sorted(opts.default_dir.glob("*/car.toml")):
        if car_id in toml.read_text(encoding="utf-8", errors="replace"):
            raise ImportError_(f"id {car_id} is a shipped car's ({toml.parent.name})")
    if out_dir.exists() and not opts.force and not opts.dry_run:
        report = out_dir / f"{stem}.import.json"
        came_from = ""
        if report.is_file():
            try:
                came_from = json.loads(report.read_text(encoding="utf-8")).get("source", {}).get("folder", "")
            except (OSError, ValueError):
                pass
        where = f" (imported from {came_from})" if came_from else ""
        raise ImportError_(f"{out_dir} exists{where}; --force replaces it")

    # Read.
    try:
        car = read_car(car_dir)
    except AcdError as e:
        raise ImportError_(f"cannot read this car's data: {e}") from None
    kn5_path = lod_kn5(car_dir, opts.lod) if opts.lod.upper() != "A" else main_kn5(car_dir)
    if kn5_path is None:
        raise ImportError_(f"no {'LOD ' + opts.lod + ' ' if opts.lod.upper() != 'A' else ''}kn5 in {car_dir}")
    try:
        kn5 = read_kn5(kn5_path)
    except EncryptedKn5 as e:
        raise ImportError_(f"refused: {e}") from None
    except Kn5Error as e:
        raise ImportError_(f"cannot read {kn5_path.name}: {e}") from None
    files_read = {kn5_path.name: crc(kn5_path.read_bytes())}
    acd = car_dir / "data.acd"
    files_read["data.acd" if car.source == "data.acd" else "data/"] = crc(acd.read_bytes()) if acd.is_file() else ""
    ui_path = car_dir / "ui" / "ui_car.json"
    if ui_path.is_file():
        files_read["ui/ui_car.json"] = crc(ui_path.read_bytes())

    # Split and seat.
    split = model.split_meshes(kn5, keep_steering_wheel_in_body=opts.keep_steering_wheel)
    try:
        fits = model.fit_wheels(kn5, split)
    except model.ModelError as e:
        raise ImportError_(str(e)) from None
    seat = model.seat_for(fits)
    lights = model.read_lights(car, kn5, seat)
    colours = model.light_colours(car)
    carini = car.ini("car.ini")
    eye = vector(carini.get("GRAPHICS", {}).get("DRIVEREYES"))
    mirrors = model.read_mirrors(car, kn5, eye)
    # The glass is a slot of its own, like a lamp: the rig paints it.
    plan = model.plan_materials(kn5, split, lights, mirrors)
    if plan.skin_texture is None:
        warnings.append("the kn5 has no Skin_00.dds: the paint cannot be repainted by a livery")

    # Skins: the default is baked in; keep the kn5's own textures for the
    # liveries that do not override one.
    default_skin = None
    if car.skins:
        if opts.skin:
            default_skin = next((s for s in car.skins if s.name.lower() == opts.skin.lower()), None)
            if default_skin is None:
                raise ImportError_(f"no skin {opts.skin!r}; the car has {', '.join(s.name for s in car.skins)}")
        else:
            default_skin = car.skins[0]
    wanted = set(plan.alpha_textures)
    kn5_original = {name: kn5.textures[name].data for name in wanted if name in kn5.textures}
    baked = liveries.apply_default_skin(kn5, default_skin, wanted)

    texture_sizes = {name: (opts.max_skin_texture if name == plan.skin_texture else opts.max_texture) for name in wanted}
    cache: dict = {}

    def seat_pts(p):
        return seat.apply(p)

    def same(n):
        return n

    body = model.build_glb(kn5, split.body, plan, lights, colours, seat_pts, same, texture_sizes, cache, True,
                            mirrors)
    lo, hi = body.builder.bounds()
    top = model.body_top(np.concatenate([p.positions for p in body.builder.primitives]).astype(np.float64))
    length, width, height = float(hi[2] - lo[2]), float(hi[0] - lo[0]), top

    wheel_glbs = {}
    for corner, name in (("LF", "front"), ("LR", "rear")):
        frame = model.Frame.of(model.find_dummy(kn5, f"WHEEL_{corner}"))
        wheel_glbs[name] = model.build_glb(kn5, split.wheels[corner], plan, lights, colours, frame.to_local,
                                           frame.dir_to_local, texture_sizes, cache, False)

    steer_frame = model.steering_frame(kn5)
    steer_glb = None
    if split.steer and steer_frame is not None:
        steer_glb = model.build_glb(kn5, split.steer, plan, lights, colours, steer_frame.to_local,
                                    steer_frame.dir_to_local, texture_sizes, cache, False)
    elif split.steer:
        warnings.append("steering wheel meshes found but no STEER_HR dummy: left in the body")

    # Physics.
    try:
        phys = physics.map_physics(car, compound=opts.compound, bounds_m=(length, width, height),
                                   car_class=opts.car_class)
    except physics.PhysicsError as e:
        raise ImportError_(f"physics: {e}") from None
    warnings.extend(phys.warnings)
    if phys.has_drs:
        warnings.append("the DRS changes the aero (drs_* keys) but its flap is not split off the body yet: "
                        "the wing is drawn closed")

    # Liveries.
    liv = liveries.plan_liveries(kn5_original, kn5, car.skins, default_skin, plan,
                                 opts.max_skin_texture, opts.max_texture)

    # Cockpit, in the actor frame (+X nose, +Y right, +Z up, cm) from the
    # seated glTF frame (x left, y up, z nose).
    def actor_cm(p_model: np.ndarray) -> list[float]:
        g = seat.apply(np.asarray(p_model, dtype=np.float64))
        return [round(float(g[2]) * 100, 1), round(float(-g[0]) * 100, 1), round(float(g[1]) * 100, 1)]

    tags = {t.lower().lstrip("#") for t in car.ui.tags}
    open_cockpit = bool(tags & {"singleseater", "open wheel", "open-wheel", "formula"}) or phys.car_class in ("F1", "Formula")
    if eye is not None and not open_cockpit:
        # No roof over the eye (glass counts: a canopy is a roof): a
        # roadster, or a prototype without one.
        e = seat.apply(np.asarray(eye, dtype=np.float64))
        open_cockpit = not any(model.roof_over(seat.apply(m.world_positions()), m.triangles.astype(np.int64), e)
                               for m in split.body)
    cockpit: dict = {"style": "open" if open_cockpit else "closed"}
    if eye is not None:
        cockpit["eye_cm"] = actor_cm(eye)
    else:
        warnings.append("car.ini has no DRIVEREYES: the rig derives the eye from the bounds")
    if steer_frame is not None:
        cockpit["wheel_cm"] = actor_cm(steer_frame.origin)
        cockpit["wheel_rake_deg"] = round(model.column_rake_deg(steer_frame), 2) or 0.01
        lock = car.ini("driver3d.ini").get("STEER_ANIMATION", {}).get("LOCK") or carini.get("CONTROLS", {}).get("STEER_LOCK")
        if lock:
            cockpit["wheel_lock_deg"] = round(float(re.match(r"[\d.]+", lock.strip()).group(0)), 1)
        if steer_glb is not None:
            cockpit["steering_wheel_model"] = "steering_wheel.glb"
    for slot in model.MIRROR_SLOTS:
        glass = mirrors.get(slot)
        if glass is None:
            continue
        key = slot.removeprefix("car_")
        cockpit[f"{key}_cm"] = actor_cm(glass.centroid)
        # The glass as the driver faces it: the rig sizes its capture to it.
        cockpit[f"{key}_size_cm"] = [round(glass.size[0] * 100, 1), round(glass.size[1] * 100, 1)]
    has_display = model.find_dummy(kn5, "DISPLAY_DUMMY") is not None or "digital_instruments.ini" in car.files
    if has_display:
        cockpit["rig_dash"] = False
    if opts.keep_steering_wheel:
        cockpit["rig_wheel"] = False

    # Wheels, in the seated frame.
    def axle(c1, c2):
        a, b = seat.wheels[c1], seat.wheels[c2]
        return float(0.5 * (a.hub[2] + b.hub[2]) + seat.offset[2]), float(abs(a.hub[0] - b.hub[0]))

    fz, ft = axle("LF", "RF")
    rz, rt = axle("LR", "RR")
    wheels = {
        "model": "wheels/front.glb",
        "rear_model": "wheels/rear.glb",
        "front_axle_m": round(fz, 4), "rear_axle_m": round(rz, 4),
        "front_track_m": round(ft, 4), "rear_track_m": round(rt, 4),
        "front_radius_m": round(seat.wheels["LF"].radius, 4), "rear_radius_m": round(seat.wheels["LR"].radius, 4),
        "front_width_m": round(seat.wheels["LF"].width, 4), "rear_width_m": round(seat.wheels["LR"].width, 4),
    }

    sound, sound_why = sound_table(car, phys, opts.cylinders)
    name = opts.display_name or car.ui.name or car_dir.name
    year = car.ui.year
    if year is None:
        m = re.search(r"\b(19[0-9]{2}|20[0-9]{2})\b", f"{car.ui.name} {car_dir.name}")
        year = int(m.group(1)) if m else None
    country = car.ui.country
    if not country:
        countries = {"germany", "italy", "france", "japan", "usa", "uk", "great britain", "england", "sweden",
                     "austria", "spain", "netherlands", "south korea", "china", "australia"}
        country = next((t.title() for t in car.ui.tags if t.lower() in countries), "")
    header: dict = {
        "imported": "ac",
        "id": car_id,
        "name": name,
        "version": "1.0.0",
        "model": f"{stem}.glb",
        "brand": car.ui.brand or name.split(" ")[0],
        "class": phys.car_class,
    }
    if year:
        header["model_year"] = year
    if country:
        header["manufacturer_country"] = country
    source = {"folder": car_dir.name, "kn5": kn5_path.name, "data": car.source,
              "skin": default_skin.name if default_skin else "", "tool": TOOL_VERSION}
    notes = [f"Engine sound: {sound['cylinders']} cylinders ({sound_why})."] if sound_why else []
    toml_text = render(header=header, physics=phys, sound=sound, wheels=wheels, cockpit=cockpit,
                       drs_flap=None, source=source, liveries=liv.liveries, notes=notes)

    # Checks.
    checks: list[dict] = []

    def check(name: str, ok: bool, detail: str, fatal: bool = True) -> None:
        checks.append({"check": name, "ok": bool(ok), "fatal": fatal, "detail": detail})

    check("frame", length > width, f"body {length:.2f} m long (glTF Z) x {width:.2f} m wide (X)")
    below = float(lo[1])
    check("seat", below > -0.03, f"tyres on y = 0; the body's lowest point at {below * 100:.1f} cm")
    check("wheels", all(f.radius > 0.15 for f in seat.wheels.values()),
          ", ".join(f"{c} r {f.radius:.3f} w {f.width:.3f}" for c, f in seat.wheels.items()))
    required = ["mass_kg", "max_engine_force_n", "max_brake_force_n", "drag_coefficient", "grip_coefficient",
                "max_steering_angle_rad", "wheelbase_m"]
    bad = [k for k in required if not (isinstance(phys.get("physics", k), (int, float))
                                      and math.isfinite(phys.get("physics", k)) and phys.get("physics", k) > 0)]
    check("required keys", not bad, "all set and positive" if not bad else f"bad: {', '.join(bad)}")
    rpm = [r for r, _ in phys.torque_curve]
    check("torque curve", rpm == sorted(rpm) and len(rpm) >= 2, f"{len(rpm)} points, {rpm[0]:.0f}-{rpm[-1]:.0f} rpm")
    tris = body.builder.triangle_count + sum(g.builder.triangle_count for g in wheel_glbs.values()) + \
        (steer_glb.builder.triangle_count if steer_glb else 0)
    check("triangles", tris <= TRIANGLE_BUDGET, f"{tris:,} (budget {TRIANGLE_BUDGET:,})", fatal=False)
    decoded_mb = sum(t.width * t.height * 4 * 4 / 3 for t in cache.values() if t) / 1e6
    check("texture memory", decoded_mb <= TEXTURE_BUDGET_MB, f"{decoded_mb:.0f} MB decoded (budget {TEXTURE_BUDGET_MB})",
          fatal=False)
    ui_kw = ui_power_kw(car)
    if ui_kw:
        # The UI quotes a hybrid's system power: the engine's curve plus the motor.
        engine_kw = phys.get("engine", "max_power_w", 0) / 1000.0
        motor_kw = phys.get("hybrid", "motor_max_power_kw", 0.0) or 0.0
        ratio = (engine_kw + motor_kw) / ui_kw
        check("power", 0.75 <= ratio <= 1.33,
              f"{engine_kw:.0f} kW from the curve" + (f" + {motor_kw:.0f} kW motor" if motor_kw else "")
              + f" against ui_car.json's {ui_kw:.0f} kW", fatal=False)
    check("skin", default_skin is not None or not car.skins,
          f"livery 0 is {default_skin.name if default_skin else 'the kn5 as shipped'}; {len(liv.liveries)} more", fatal=False)
    ok = all(c["ok"] for c in checks if c["fatal"])

    report = {
        "tool": TOOL_VERSION,
        "stem": stem,
        "id": car_id,
        "source": {"folder": car_dir.name, "path": str(car_dir), "kn5": kn5_path.name, "data": car.source,
                   "files": files_read},
        "class": phys.car_class,
        "physics": {t: {k: v.value for k, v in e.items()} for t, e in phys.tables.items()},
        "fit": phys.fit,
        "seat": {"offset_m": [round(float(v), 4) for v in seat.offset]},
        "parts": {
            "body_meshes": len(split.body),
            "wheel_meshes": {c: len(v) for c, v in split.wheels.items()},
            "steering_wheel_meshes": len(split.steer),
            "dropped": {why: sorted(set(names)) for why, names in sorted(split.dropped.items())},
        },
        "materials": plan.report,
        "lights": dict(sorted(lights.items())),
        "mirrors": {s: g.node for s, g in mirrors.items()},
        "skins": {"default": default_skin.name if default_skin else None, "baked": baked,
                  "overridden_textures": liv.overridden,
                  "liveries": [{"name": l.name, "folder": l.folder} for l in liv.liveries]},
        "checks": checks,
        "warnings": warnings,
        "rebuild": "python scripts/ac_car_import.py " + _quote(str(car_dir)) +
                   (f" --stem {stem}" if opts.stem else "") +
                   (f" --skin {opts.skin}" if opts.skin else "") +
                   (f" --class {opts.car_class}" if opts.car_class else "") +
                   (f" --compound {opts.compound}" if opts.compound is not None else "") + " --force",
    }

    summary = (f"{stem}: {name} [{phys.car_class}], {tris:,} triangles, "
               f"{len(liv.liveries) + (1 if default_skin else 0)} liveries, "
               f"{phys.get('engine', 'max_power_w', 0) / 1000:.0f} kW, "
               f"grip {phys.get('physics', 'grip_coefficient')}")
    if opts.dry_run:
        return Result(stem, ok, summary + " (dry run, nothing written)", checks, warnings)

    # Write: into a fresh folder, so nothing of an earlier import lingers.
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    sizes = {}
    sizes[f"{stem}.glb"] = body.builder.write(out_dir / f"{stem}.glb")
    for name_, g in wheel_glbs.items():
        sizes[f"wheels/{name_}.glb"] = g.builder.write(out_dir / "wheels" / f"{name_}.glb")
    if steer_glb is not None:
        sizes["steering_wheel.glb"] = steer_glb.builder.write(out_dir / "steering_wheel.glb")
    liveries.write_files(out_dir, liv.files)
    for rel, data in liv.files.items():
        sizes[rel] = len(data)
    (out_dir / "car.toml").write_bytes(toml_text.encode("utf-8"))
    # Read back what the client will read.
    for rel in [f"{stem}.glb", "wheels/front.glb", "wheels/rear.glb"] + (["steering_wheel.glb"] if steer_glb else []):
        read_glb_json(out_dir / rel)
    report["files"] = {k: sizes[k] for k in sorted(sizes)}
    (out_dir / f"{stem}.import.json").write_text(json.dumps(report, indent=2, sort_keys=False, default=_json) + "\n",
                                                  encoding="utf-8")
    return Result(stem, ok, summary, checks, warnings)


def _json(o):
    if isinstance(o, np.generic):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    return str(o)


def _quote(s: str) -> str:
    return f'"{s}"' if " " in s else s


def list_car(car_dir: Path) -> None:
    car = read_car(car_dir)
    print(f"{car_dir.name}: {car.ui.name or '?'} ({car.source}), stem {default_stem(car_dir)}, id {car_id_for(car_dir.name)}")
    print(f"  class {physics.map_class(car, bool(car.ini('drs.ini')))}  tags {', '.join(car.ui.tags)}")
    print("  compounds: " + ", ".join(f"{i} {n}" + (" (default)" if i == physics.default_compound(car) else "")
                                      for i, n in physics.tyre_compounds(car)))
    print("  skins: " + ", ".join(f"{s.name} ({s.display_name})" for s in car.skins))
    kn5_path = main_kn5(car_dir)
    if kn5_path:
        kn5 = read_kn5(kn5_path, textures=False)
        split = model.split_meshes(kn5)
        print(f"  {kn5_path.name}: {kn5.triangle_count:,} triangles, {len(kn5.materials)} materials; body "
              f"{sum(m.triangle_count for m in split.body):,}, steering wheel {sum(m.triangle_count for m in split.steer):,}")
        for why, names in sorted(split.dropped.items()):
            print(f"  dropped ({why}): {len(names)} meshes")
    lods = [p.name for p in sorted(car_dir.glob('*_lod_*.kn5'))]
    if lods:
        print("  LODs: " + ", ".join(lods))


def print_result(r: Result) -> None:
    print(("OK   " if r.ok else "FAIL ") + r.summary)
    for c in r.checks:
        mark = "pass" if c["ok"] else ("FAIL" if c["fatal"] else "warn")
        print(f"  [{mark}] {c['check']}: {c['detail']}")
    for w in r.warnings:
        print(f"  warning: {w}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ac_car_import.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("car", nargs="?", help="an AC car folder (content/cars/<car>)")
    ap.add_argument("--all", metavar="CARS_DIR", help="import every car folder under CARS_DIR")
    ap.add_argument("--list", action="store_true", help="describe the car (skins, compounds, parts) and stop")
    ap.add_argument("--stem", help="the folder name under content/cars/custom (default: CamelCase of the AC folder)")
    ap.add_argument("--display-name", help="the name shown in the game (default: ui_car.json's)")
    ap.add_argument("--class", dest="car_class", help="the ApexSim class (default: from the tags; GT3, LMP2, Hypercar, F1...)")
    ap.add_argument("--skin", help="the skin baked in as livery 0 (default: the first)")
    ap.add_argument("--compound", type=int, help="tyres.ini compound index (default: COMPOUND_DEFAULT)")
    ap.add_argument("--lod", default="A", help="A (the full model) or B (a LOD, for AI fields)")
    ap.add_argument("--max-texture", type=int, default=1024, help="largest side of any texture but the skin (1024)")
    ap.add_argument("--max-skin-texture", type=int, default=2048, help="largest side of the skin texture (2048)")
    ap.add_argument("--cylinders", type=int, help="engine cylinders for the sound (default: guessed from the name)")
    ap.add_argument("--keep-steering-wheel", action="store_true",
                    help="leave the steering wheel in the body (static) and hide the rig's rim")
    ap.add_argument("--force", action="store_true", help="replace an existing import")
    ap.add_argument("--dry-run", action="store_true", help="read and check, write nothing")
    args = ap.parse_args(argv)

    if not args.car and not args.all:
        ap.print_help()
        return 2
    opts = Options(stem=args.stem, display_name=args.display_name, car_class=args.car_class, skin=args.skin,
                   compound=args.compound, lod=args.lod, max_texture=args.max_texture,
                   max_skin_texture=args.max_skin_texture, cylinders=args.cylinders,
                   keep_steering_wheel=args.keep_steering_wheel, force=args.force, dry_run=args.dry_run)
    if args.list:
        list_car(Path(args.car))
        return 0
    show_notice_once()
    folders = [Path(args.car)] if args.car else sorted(p for p in Path(args.all).iterdir() if p.is_dir())
    failed = 0
    for folder in folders:
        try:
            r = import_car(folder, opts)
            if args.all:
                print(("OK   " if r.ok else "FAIL ") + r.summary)
                for c in r.checks:
                    if not c["ok"] and c["fatal"]:
                        print(f"     {c['check']}: {c['detail']}")
            else:
                print_result(r)
            failed += 0 if r.ok else 1
        except (ImportError_, OSError) as e:
            print(f"FAIL {folder.name}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001 - one car's surprise must not end a collection
            if not args.all:
                raise
            print(f"FAIL {folder.name}: unexpected {type(e).__name__}: {e} (run it alone for the traceback)")
            failed += 1
        sys.stdout.flush()
    if not args.dry_run:
        print("Restart the server, and restart the game or run apexsim.car.Rescan.")
    return 1 if failed else 0
