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

TABLE_ORDER = ("physics", "tires", "engine", "engine.turbo", "transmission", "drivetrain",
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
    w("# Imported from Assetto Corsa by scripts/ac_car_import.py for your own use on")
    w("# this machine: never shipped (content/cars/custom is not packaged). Re-run the")
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

    for name in TABLE_ORDER:
        entries = physics.tables.get(name)
        if not entries:
            continue
        table(name, entries)
        if name == "engine":
            for rpm, torque in physics.torque_curve:
                w("")
                w("[[engine.torque_curve]]")
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
