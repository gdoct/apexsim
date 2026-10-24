"""The car.toml text: everything the server and the client read, in the
shipped cars' order, with the AC key each figure came from as a comment.

Two readers constrain the layout. The server's is a real TOML parser, and
its `[tires]` and `[engine.turbo]` refuse unknown keys. The client's
(`ApexCarToml::Parse`) is line-based: no comment after a table header,
every array on one line, strings in double quotes.
"""

from __future__ import annotations

import math

from .physics import Physics

TABLE_ORDER = ("physics", "aero", "brakes", "tires", "engine", "engine.turbo", "transmission", "drivetrain",
               "differential", "fuel", "hybrid", "suspension")


def fmt(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"not a finite number: {value}")
        text = repr(round(value, 6))
        return text if ("." in text or "e" in text or "inf" in text) else text + ".0"
    if isinstance(value, str):
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(fmt(v) for v in value) + "]"
    raise TypeError(f"cannot write {value!r}")


def _float(v) -> float:
    return float(v)


def render(*, header: dict, physics: Physics, sound: dict, wheels: dict, cockpit: dict,
           drs_flap: dict | None, source: dict, liveries: list, notes: list[str]) -> str:
    lines: list[str] = []
    w = lines.append
    w("# Imported from Assetto Corsa by tools/ac_car_import.py for your own use on")
    w("# this machine: never shipped (cars/custom is not packaged). Re-run the")
    w("# import rather than editing this file; see the .import.json report beside it.")
    for n in notes:
        w(f"# {n}")
    w("")
    for key, value in header.items():
        w(f"{key} = {fmt(value)}")

    def table(name: str, entries: dict) -> None:
        w("")
        w(f"[{name}]")
        for key, v in entries.items():
            value = v.value
            if isinstance(value, float) or (isinstance(value, int) and not isinstance(value, bool)):
                value = _float(value)
            if isinstance(value, list):
                value = [_float(x) for x in value]
            comment = f"   # {v.source}" if v.source else ""
            w(f"{key} = {fmt(value)}{comment}")

    def compounds(rows: list[dict]) -> None:
        for r in rows:
            if r["index"] is None:
                source = "ApexSim's own: AC has no rain tyre"
            else:
                source = f"tyres.ini [FRONT_{r['index']}]" if r["index"] else "tyres.ini [FRONT]"
            w("")
            w("[[tires.compound]]")
            w(f"name = {fmt(r['name'])}   # {source}")
            w(f"kind = {fmt(r['kind'])}")
            w(f"grip = {fmt(float(r['grip']))}")
            w(f"wear = {fmt(float(r['wear']))}")
            w(f"window_shift_c = {fmt(float(r['window_shift_c']))}")
            if r.get("window_scale", 1.0) != 1.0:
                w(f"window_scale = {fmt(float(r['window_scale']))}")
            for key, ac_key in (("heat_scale", "FRICTION_K"), ("rolling_heat_scale", "ROLLING_K")):
                if r.get(key, 1.0) != 1.0:
                    w(f"{key} = {fmt(float(r[key]))}   # tyres.ini THERMAL_* {ac_key} over the reference's")
            if r.get("pressure_shift_kpa", 0.0) != 0.0:
                w(f"pressure_shift_kpa = {fmt(float(r['pressure_shift_kpa']))}   "
                  f"# tyres.ini PRESSURE_IDEAL less the reference's")
            for key, ac_key in (("load_sensitivity_front", "LS_EXPY"), ("load_sensitivity_rear", "LS_EXPY"),
                                ("reference_load_front_n", "FZ0"), ("reference_load_rear_n", "FZ0")):
                if key in r:
                    w(f"{key} = {fmt(float(r[key]))}   # tyres.ini {ac_key}: its own, not the reference's")
            if r["reference"]:
                w("reference = true   # the compound this import is calibrated on")

    for name in TABLE_ORDER:
        entries = physics.tables.get(name)
        if name == "aero" and (physics.aero_speed_map or physics.aero_input_map):
            if entries:
                table(name, entries)
            if physics.aero_speed_map:
                w("")
                w("# aero.ini DYNAMIC_CONTROLLER: the wings' angles by speed, as multipliers on the")
                w("# downforce and drag above (1 at 50 m/s, where they were taken)")
            for pt in physics.aero_speed_map:
                w("")
                w("[[aero.speed_point]]")
                for key in ("speed_mps", "downforce_front", "downforce_rear", "drag"):
                    w(f"{key} = {fmt(float(pt[key]))}")
            if physics.aero_input_map:
                w("")
                w("# aero.ini DYNAMIC_CONTROLLER: the wings' angles by the driver's inputs at 50 m/s,")
                w("# as multipliers (1 flat out in a straight line, where the figures were taken)")
            for pt in physics.aero_input_map:
                w("")
                w("[[aero.input_point]]")
                w(f'input = "{pt["input"]}"')
                for key in ("at", "downforce_front", "downforce_rear", "drag"):
                    w(f"{key} = {fmt(float(pt[key]))}")
            continue
        if not entries:
            continue
        table(name, entries)
        if name == "tires":
            compounds(physics.compounds)
        if name == "engine":
            for rpm, torque in physics.torque_curve:
                w("")
                w("[[engine.torque_curve]]")
                w(f"rpm = {fmt(float(rpm))}")
                w(f"torque_nm = {fmt(float(torque))}")
        if name == "hybrid":
            if physics.motor_curve:
                w("")
                w("# ers.ini TORQUE_CURVE: the motor's torque by its rpm")
            for rpm, torque in physics.motor_curve:
                w("")
                w("[[hybrid.torque_curve]]")
                w(f"rpm = {fmt(float(rpm))}")
                w(f"torque_nm = {fmt(float(torque))}")

    def plain(name: str, entries: dict, comments: dict | None = None) -> None:
        w("")
        w(f"[{name}]")
        for key, value in entries.items():
            c = (comments or {}).get(key)
            w(f"{key} = {fmt(value)}" + (f"   # {c}" if c else ""))

    plain("sound", sound, {"cylinders": "from the name and description; --cylinders overrides"})
    plain("wheels", wheels)
    if drs_flap:
        plain("drs_flap", drs_flap)
    plain("cockpit", cockpit)
    plain("source", source)

    if liveries:
        w("")
        w("# Livery 0 is the skin baked into the GLBs; these are AC's other skins.")
        for liv in liveries:
            w("")
            w("[[livery]]")
            w(f"name = {fmt(liv.name)}")
            if liv.skin:
                w(f"skin = {fmt(liv.skin)}")
            if liv.textures:
                w("textures = " + fmt([f"{slot}={path}" for slot, path in liv.textures]))
            if liv.preview:
                w(f"preview = {fmt(liv.preview)}")
    w("")
    return "\n".join(lines)
