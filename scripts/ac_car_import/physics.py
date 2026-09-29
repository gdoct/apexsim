"""AC's physics files -> the figures ApexSim's car.toml takes
(docs/AC_CAR_IMPORT.md, "AC -> ApexSim: the mapping").

Frames. AC's physics origin is the car's centre of gravity, (x left, y up,
z forward); the aero wings' and the colliders' positions are given from it.
The 3D model sits at `physics - GRAPHICS_OFFSET` (so `DRIVEREYES`, which is
in the model's frame, needs no offset). Everything here stays in the
physics frame; `model.py` handles the mesh.

Every figure is returned with the AC key it came from, which the car.toml
writer turns into a comment and the report keeps.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

import numpy as np

from .data import CarData, Lut, number, vector

G = 9.81
AIR_DENSITY = 1.225
PSI_TO_KPA = 6.894757
#: AC's physics runs at 333 Hz; the turbo lags are per-step factors.
AC_PHYSICS_HZ = 333.0
#: Where a tyre's grip is read for the one `grip_coefficient` (the sim
#: applies the load sensitivity itself; the speed sensitivity it cannot).
REFERENCE_SPEED_MPS = 40.0


class PhysicsError(Exception):
    """A car whose data cannot give a figure the server requires."""


@dataclass
class Value:
    value: object
    source: str = ""


@dataclass
class Physics:
    #: table name -> key -> Value, in the order the writer emits them.
    tables: dict[str, dict[str, Value]] = field(default_factory=dict)
    torque_curve: list[tuple[float, float]] = field(default_factory=list)
    #: Figures for the report that are not car.toml keys.
    fit: dict = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    car_class: str = ""
    has_drs: bool = False
    cylinders: int | None = None
    turbo: bool = False

    def put(self, table: str, key: str, value, source: str = "") -> None:
        self.tables.setdefault(table, {})[key] = Value(value, source)

    def get(self, table: str, key: str, default=None):
        v = self.tables.get(table, {}).get(key)
        return default if v is None else v.value


def _req(sections: dict, section: str, key: str, what: str) -> float:
    v = number(sections.get(section, {}).get(key))
    if v is None:
        raise PhysicsError(f"{what}: [{section}] {key} is missing")
    return v


def _opt(sections: dict, section: str, key: str, default=None):
    return number(sections.get(section, {}).get(key), default)


@dataclass
class Tyre:
    section: str
    name: str
    radius: float
    width: float
    dy0: float
    dx0: float
    ls_expy: float
    ls_expx: float
    fz0: float
    speed_sensitivity: float
    pressure_ideal_psi: float | None
    rate: float | None


def _tyre(sections: dict, section: str) -> Tyre:
    s = sections.get(section)
    if not s:
        raise PhysicsError(f"tyres.ini has no [{section}]")

    def n(key, default=None):
        return number(s.get(key), default)

    radius = n("RADIUS")
    dy0 = n("DY0")
    if radius is None or dy0 is None:
        raise PhysicsError(f"tyres.ini [{section}] has no RADIUS or DY0")
    return Tyre(
        section=section,
        name=str(s.get("NAME", "")).strip(),
        radius=radius,
        width=n("WIDTH", 0.25),
        dy0=dy0,
        dx0=n("DX0", dy0),
        ls_expy=n("LS_EXPY", 1.0),
        ls_expx=n("LS_EXPX", 1.0),
        fz0=n("FZ0", 3000.0),
        speed_sensitivity=n("SPEED_SENSITIVITY", 0.0),
        pressure_ideal_psi=n("PRESSURE_IDEAL"),
        rate=n("RATE"),
    )


def thermal_window(curve: Lut) -> tuple[float, float, float] | None:
    """ApexSim's working window from AC's ``PERFORMANCE_CURVE`` (grip against
    temperature): ``(optimal_temperature_c, temperature_window_c,
    temperature_grip_falloff)``. The window is the curve's plateau (within
    half a percent of its peak), and the falloff the average slope over the
    30 degrees past each edge; ApexSim charges the cold side 0.6 of it (the
    pressure charges the rest), so the cold slope is divided by that.
    ``None`` for a curve with no plateau to speak of."""
    if curve.x.size < 2:
        return None
    peak = float(curve.y.max())
    if peak <= 0.0:
        return None
    top = curve.x[curve.y >= peak - 0.005]
    lo, hi = float(top.min()), float(top.max())
    optimum = 0.5 * (lo + hi)
    window = max(0.5 * (hi - lo), 2.0)
    span = 30.0
    cold = max(peak - curve(lo - span), 0.0) / peak / span / 0.6
    hot = max(peak - curve(hi + span), 0.0) / peak / span
    falloff = min(max(0.5 * (cold + hot), 0.0), 0.03)
    return round(optimum, 1), round(min(window, 50.0), 1), round(falloff, 5)


def read_thermal_window(car: CarData, compound: int) -> tuple[float, float, float] | None:
    """The front tyre's working window for ``compound`` (``[THERMAL_FRONT]``
    or ``[THERMAL_FRONT_n]``), or ``None`` when tyres.ini has no curve."""
    t = car.ini("tyres.ini")
    section = "THERMAL_FRONT" if compound == 0 else f"THERMAL_FRONT_{compound}"
    name = str(t.get(section, {}).get("PERFORMANCE_CURVE", "")).strip()
    curve = car.lut(name) if name else None
    return thermal_window(curve) if curve is not None else None


def tyre_compounds(car: CarData) -> list[tuple[int, str]]:
    """(index, name) of every compound in tyres.ini."""
    t = car.ini("tyres.ini")
    out = []
    i = 0
    while True:
        sec = "FRONT" if i == 0 else f"FRONT_{i}"
        if sec not in t:
            break
        out.append((i, str(t[sec].get("NAME", sec)).strip()))
        i += 1
    return out


def default_compound(car: CarData) -> int:
    t = car.ini("tyres.ini")
    return int(_opt(t, "COMPOUND_DEFAULT", "INDEX", 0) or 0)


def read_tyres(car: CarData, compound: int | None) -> tuple[Tyre, Tyre, int]:
    t = car.ini("tyres.ini")
    idx = default_compound(car) if compound is None else compound
    suffix = "" if idx == 0 else f"_{idx}"
    if f"FRONT{suffix}" not in t:
        raise PhysicsError(f"tyres.ini has no compound {idx}")
    return _tyre(t, f"FRONT{suffix}"), _tyre(t, f"REAR{suffix}"), idx


# --- class ----------------------------------------------------------------

def map_class(car: CarData, has_drs: bool) -> str:
    """The ApexSim class from ui_car.json's tags (docs: "Class and names")."""
    tags = {t.lower().lstrip("#").strip() for t in car.ui.tags}
    joined = " ".join(sorted(tags))
    name = f"{car.ui.name} {car.car_dir.name}".lower()
    if tags & {"gt3", "gte-gt3"} or re.search(r"\bgt3\b", name):
        return "GT3"
    if "lmp2" in tags:
        return "LMP2"
    if tags & {"lmp1", "lmh", "hypercars r", "hypercar", "lmdh", "gtp"}:
        return "Hypercar"
    if "singleseater" in tags or "single seater" in joined or "open wheel" in joined:
        if has_drs and (tags & {"gp", "f1", "formula 1"} or "formula 1" in joined):
            return "F1"
        return "Formula"
    if tags & {"gt4"}:
        return "GT4"
    if tags & {"gt2", "gte", "gtlm", "gt1"}:
        return "GT"
    if tags & {"vintage"}:
        return "Vintage"
    if tags & {"drift"}:
        return "Drift"
    if tags & {"touring", "tcr", "dtm"}:
        return "Touring"
    if car.ui.car_class.lower() == "street" or "street" in tags:
        return "Street"
    return "Race"


# --- engine ---------------------------------------------------------------

@dataclass
class Controller:
    """One `[CONTROLLER_n]` of a `ctrl_*.ini`: a LUT of one input, folded
    into the running value by its combinator, clamped to its limits."""
    input: str
    combinator: str
    lut: Lut
    up: float | None
    down: float | None


@dataclass
class Turbo:
    max_boost: float
    wastegate: float
    reference_rpm: float
    lag_up: float
    lag_dn: float
    #: `ctrl_turbo<n>.ini`: when present, it sets the wastegate.
    controllers: list[Controller] = field(default_factory=list)


def parse_inline_lut(text: str) -> Lut | None:
    """`(|0=0|1000=0.5|)`, the form controllers write their LUTs in."""
    pairs = re.findall(r"([-+\d.eE]+)\s*=\s*([-+\d.eE]+)", text)
    if not pairs:
        return None
    x = np.array([float(a) for a, _ in pairs])
    y = np.array([float(b) for _, b in pairs])
    order = np.argsort(x, kind="stable")
    return Lut(x[order], y[order])


def read_controllers(car: CarData, name: str) -> list[Controller]:
    out = []
    for sec, s in car.ini(name).items():
        if not sec.startswith("CONTROLLER"):
            continue
        lut_text = str(s.get("LUT", "")).strip()
        lut = parse_inline_lut(lut_text) if lut_text.startswith("(") else car.lut(lut_text)
        if lut is None:
            continue
        out.append(Controller(str(s.get("INPUT", "")).split()[0].upper() if s.get("INPUT") else "",
                              str(s.get("COMBINATOR", "ADD")).split()[0].upper(), lut,
                              number(s.get("UP_LIMIT")), number(s.get("DOWN_LIMIT"))))
    return out


#: Inputs a controller can be read at full throttle without a simulation.
KNOWN_INPUTS = {"RPMS", "GEAR", "GAS"}


def controller_value(controllers: list[Controller], rpm: float, gear: int) -> float:
    value = 0.0
    for c in controllers:
        x = {"RPMS": rpm, "GEAR": float(gear), "GAS": 1.0}[c.input]
        y = c.lut(x)
        if c.combinator == "MULT":
            value *= y
        elif c.combinator == "MIN":
            value = min(value, y)
        elif c.combinator == "MAX":
            value = max(value, y)
        else:
            value += y
        if c.up is not None:
            value = min(value, c.up)
        if c.down is not None:
            value = max(value, c.down)
    return value


def read_turbos(car: CarData, engine: dict) -> list[Turbo]:
    out = []
    i = 0
    while f"TURBO_{i}" in engine:
        s = engine[f"TURBO_{i}"]
        ctrls = read_controllers(car, f"ctrl_turbo{i}.ini")
        out.append(Turbo(
            max_boost=number(s.get("MAX_BOOST"), 0.0),
            wastegate=number(s.get("WASTEGATE"), 0.0),
            reference_rpm=max(number(s.get("REFERENCE_RPM"), 1.0), 1.0),
            lag_up=number(s.get("LAG_UP"), 0.99),
            lag_dn=number(s.get("LAG_DN"), 0.99),
            controllers=ctrls if all(c.input in KNOWN_INPUTS for c in ctrls) else [],
        ))
        i += 1
    return out


def boost_at(turbos: list[Turbo], rpm: float, gears: int = 1) -> float:
    """Full-throttle boost. AC multiplies the torque by `1 + boost`; each
    turbo builds `MAX_BOOST` linearly to `REFERENCE_RPM` (its GAMMA is the
    pedal's sensitivity, which is 1 at full throttle) and the wastegate caps
    it. A turbo controller sets the wastegate, often per gear (the 488 GTB
    is ten turbos, two lit in each gear); the sim has one curve, so it gets
    the gear with the most boost."""
    best = 0.0
    for gear in range(1, max(gears, 1) + 1):
        total = 0.0
        for t in turbos:
            b = t.max_boost * min(max(rpm, 0.0) / t.reference_rpm, 1.0)
            if t.controllers:
                b = min(b, max(controller_value(t.controllers, rpm, gear), 0.0))
            elif t.wastegate > 0.0:
                b = min(b, t.wastegate)
            total += b
        best = max(best, total)
    return best


def lag_seconds(factor: float) -> float:
    """AC's per-step lag factor at 333 Hz as a time constant."""
    if not (0.0 < factor < 1.0):
        return 0.0
    return min(max(-(1.0 / AC_PHYSICS_HZ) / math.log(factor), 0.0), 5.0)


# --- aero -----------------------------------------------------------------

@dataclass
class Wing:
    index: int
    name: str
    area: float
    position: list[float]
    angle: float
    cl: float
    cd: float
    ride_height: float | None


def _wing_coefficients(car: CarData, s: dict, angle: float, ride_height: float | None) -> tuple[float, float]:
    cl_lut = car.lut(s.get("LUT_AOA_CL"))
    cd_lut = car.lut(s.get("LUT_AOA_CD"))
    cl = cl_lut(angle) if cl_lut else 0.0
    cd = cd_lut(angle) if cd_lut else 0.0
    if ride_height is not None:
        gh_cl = car.lut(s.get("LUT_GH_CL"))
        gh_cd = car.lut(s.get("LUT_GH_CD"))
        if gh_cl is not None and gh_cl.x.size:
            cl *= gh_cl(ride_height)
        if gh_cd is not None and gh_cd.x.size:
            cd *= gh_cd(ride_height)
    cl *= number(s.get("CL_GAIN"), 1.0)
    cd *= number(s.get("CD_GAIN"), 1.0)
    return cl, cd


def wheel_spring_rate(susp: dict, axle: str) -> tuple[float | None, str]:
    """The axle's spring rate at each wheel. An open-wheeler may carry its
    springing on a heave spring (`[HEAVE_FRONT]`) with the corner springs at
    zero; in heave that is half its rate at each wheel."""
    k = _opt(susp, axle, "SPRING_RATE")
    if k and k > 0:
        return k, f"suspensions.ini [{axle}] SPRING_RATE"
    heave = _opt(susp, f"HEAVE_{axle}", "SPRING_RATE")
    if heave and heave > 0:
        return (k or 0.0) + heave / 2.0, f"suspensions.ini [HEAVE_{axle}] SPRING_RATE / 2 (the corner springs are {k or 0:g})"
    return None, ""


# --- the mapping ----------------------------------------------------------

def map_physics(car: CarData, *, compound: int | None = None, bounds_m: tuple[float, float, float] | None = None,
                car_class: str | None = None) -> Physics:
    """Every car.toml figure the server reads. `bounds_m` is the body
    mesh's (length, width, height); the physics box is the visual one."""
    out = Physics()
    carini = car.ini("car.ini")
    engine = car.ini("engine.ini")
    drivetrain = car.ini("drivetrain.ini")
    susp = car.ini("suspensions.ini")
    brakes = car.ini("brakes.ini")
    aero = car.ini("aero.ini")
    elec = car.ini("electronics.ini")
    if not carini or not engine or not drivetrain or not susp:
        raise PhysicsError("car.ini, engine.ini, drivetrain.ini or suspensions.ini is missing")

    front, rear, compound = read_tyres(car, compound)
    out.fit["compound"] = {"index": compound, "front": front.name, "rear": rear.name}

    # Mass, steering.
    mass = _req(carini, "BASIC", "TOTALMASS", "mass")
    lock = _req(carini, "CONTROLS", "STEER_LOCK", "steering")
    # A negative ratio turns the rim the other way in AC; the angle is the same.
    ratio = abs(_req(carini, "CONTROLS", "STEER_RATIO", "steering"))
    if lock <= 0 or ratio <= 0:
        raise PhysicsError(f"steering: STEER_LOCK {lock} / STEER_RATIO {ratio}")
    out.put("physics", "mass_kg", round(mass, 1), "car.ini TOTALMASS (with driver, no fuel)")

    # Geometry.
    wheelbase = _req(susp, "BASIC", "WHEELBASE", "wheelbase")
    wf = _req(susp, "BASIC", "CG_LOCATION", "weight distribution")
    track_f = _req(susp, "FRONT", "TRACK", "track")
    track_r = _req(susp, "REAR", "TRACK", "track")
    basey_f = _opt(susp, "FRONT", "BASEY", -0.1)
    basey_r = _opt(susp, "REAR", "BASEY", -0.1)
    cg_f = front.radius - basey_f
    cg_r = rear.radius - basey_r
    cog = wf * cg_f + (1.0 - wf) * cg_r
    # Axles in the physics frame (the CG at z = 0).
    z_front = wheelbase * (1.0 - wf)
    z_rear = -wheelbase * wf
    out.fit["axles_physics_z"] = [round(z_front, 4), round(z_rear, 4)]

    # Engine: power.lut is the naturally aspirated torque; a turbo
    # multiplies it by (1 + boost).
    power_lut = car.lut(engine.get("HEADER", {}).get("POWER_CURVE", "power.lut").split(";")[0].strip()) \
        or car.lut("power.lut")
    if power_lut is None or power_lut.x.size < 2:
        raise PhysicsError("engine.ini's power curve is missing")
    limiter = _opt(engine, "ENGINE_DATA", "LIMITER", 0.0) or float(power_lut.x.max())
    idle = _opt(engine, "ENGINE_DATA", "MINIMUM", 900.0)
    turbos = read_turbos(car, engine)
    gear_count = int(number(drivetrain.get("GEARS", {}).get("COUNT"), 1) or 1)
    out.turbo = bool(turbos)
    curve: list[tuple[float, float]] = []
    for rpm, torque in power_lut.points:
        if rpm < max(idle * 0.5, 1.0) or rpm > limiter + 500.0:
            continue
        boosted = torque * (1.0 + boost_at(turbos, rpm, gear_count))
        curve.append((rpm, max(boosted, 0.0)))
    # A LUT that stops short of the limiter or starts above idle: hold
    # its ends so the sim's curve covers the rev range.
    if curve and curve[0][0] > idle:
        curve.insert(0, (idle, curve[0][1]))
    if curve and curve[-1][0] < limiter:
        at_limiter = power_lut(limiter) * (1.0 + boost_at(turbos, limiter, gear_count))
        curve.append((limiter, max(at_limiter, 0.0)))
    seen: set[float] = set()
    curve = [(r, t) for r, t in curve if not (r in seen or seen.add(r))]
    if not curve or max(t for _, t in curve) <= 0.0:
        raise PhysicsError("the torque curve is empty")
    out.torque_curve = [(round(r, 1), round(t, 1)) for r, t in curve]
    peak_rpm, peak_torque = max(curve, key=lambda p: p[1])
    power = max(r * t * 2.0 * math.pi / 60.0 for r, t in curve)
    out.fit["turbo"] = [t.__dict__ for t in turbos]
    if turbos:
        out.fit["boost_at_peak_torque"] = round(boost_at(turbos, peak_rpm, gear_count), 3)
    unread = [i for i, t in enumerate(turbos) if not t.controllers and f"ctrl_turbo{i}.ini" in car.files]
    if unread:
        out.warnings.append(f"turbo controllers on inputs other than rpm, gear and throttle are not modelled "
                            f"(turbos {unread}): their boost is the WASTEGATE figure")
    elif any(t.controllers for t in turbos):
        out.warnings.append("turbo controllers read at full throttle, in the gear with the most boost")

    # Gears.
    gears = []
    count = int(_req(drivetrain, "GEARS", "COUNT", "gearbox"))
    for i in range(1, count + 1):
        gears.append(_req(drivetrain, "GEARS", f"GEAR_{i}", "gearbox"))
    reverse = -abs(_opt(drivetrain, "GEARS", "GEAR_R", gears[0]))
    final = _req(drivetrain, "GEARS", "FINAL", "gearbox")
    if any(b >= a for a, b in zip(gears, gears[1:])):
        raise PhysicsError(f"the forward gears do not count down: {gears}")
    drive_type = str(drivetrain.get("TRACTION", {}).get("TYPE", "RWD")).split(";")[0].strip().upper()
    layout = {"FWD": "FWD", "RWD": "RWD", "AWD": "AWD", "AWD2": "AWD"}.get(drive_type, "RWD")
    driven_radius = {"FWD": front.radius, "RWD": rear.radius}.get(layout, 0.5 * (front.radius + rear.radius))
    efficiency = 0.92
    max_force = peak_torque * gears[0] * final * efficiency / driven_radius

    # Brakes: MAX_TORQUE is the per-wheel torque at a full pedal, split by
    # FRONT_SHARE between the axles.
    brake_t = _opt(brakes, "DATA", "MAX_TORQUE", 2000.0)
    brake_share = _opt(brakes, "DATA", "FRONT_SHARE", 0.6)
    brake_force = 2.0 * brake_t * (brake_share / front.radius + (1.0 - brake_share) / rear.radius)

    # Static ride heights, for the aero's height tables: the design CG
    # height at the pickup point, the push rod, less the static sag of the
    # spring and the tyre under the axle's load.
    springs = {axle: wheel_spring_rate(susp, axle) for axle in ("FRONT", "REAR")}
    k_f = springs["FRONT"][0] or 80000.0
    k_r = springs["REAR"][0] or 70000.0
    load_f = mass * G * wf / 2.0
    load_r = mass * G * (1.0 - wf) / 2.0
    sag_f = load_f / k_f + (load_f / front.rate if front.rate else 0.0)
    sag_r = load_r / k_r + (load_r / rear.rate if rear.rate else 0.0)
    ride_f = cg_f + _opt(carini, "RIDE", "PICKUP_FRONT_HEIGHT", -cg_f + 0.08) + _opt(susp, "FRONT", "ROD_LENGTH", 0.0) - sag_f
    ride_r = cg_r + _opt(carini, "RIDE", "PICKUP_REAR_HEIGHT", -cg_r + 0.08) + _opt(susp, "REAR", "ROD_LENGTH", 0.0) - sag_r
    ride_f, ride_r = max(ride_f, 0.02), max(ride_r, 0.02)
    out.fit["static_ride_height_m"] = [round(ride_f, 4), round(ride_r, 4)]

    # Aero: every wing at its authored angle and the static ride height.
    wings: list[Wing] = []
    drs_ini = car.ini("drs.ini")
    drs_wings = {int(m.group(1)) for sec in drs_ini for m in [re.match(r"WING_(\d+)$", sec)] if m}
    i = 0
    while f"WING_{i}" in aero:
        s = aero[f"WING_{i}"]
        pos = vector(s.get("POSITION")) or [0.0, 0.0, 0.0]
        angle = number(s.get("ANGLE"), 0.0)
        height = ride_f if pos[2] >= 0.0 else ride_r
        cl, cd = _wing_coefficients(car, s, angle, height)
        area = number(s.get("CHORD"), 1.0) * number(s.get("SPAN"), 1.0)
        wings.append(Wing(i, str(s.get("NAME", f"WING_{i}")).strip(), area, pos, angle, cl, cd, height))
        i += 1
    if any(sec.startswith("DYNAMIC_CONTROLLER") for sec in aero):
        out.warnings.append("aero.ini's dynamic controllers (wings that move with speed or throttle) are not "
                            "modelled: every wing is taken at its authored ANGLE")
    body = next((w for w in wings if w.name.upper() == "BODY"), wings[0] if wings else None)
    area_ref = body.area if body else 2.0
    cla = sum(w.cl * w.area for w in wings)
    cda = sum(w.cd * w.area for w in wings)
    cla_front = sum(w.cl * w.area * (w.position[2] - z_rear) / wheelbase for w in wings)
    cla_rear = cla - cla_front
    drag_coefficient = cda / area_ref if area_ref > 0 else 0.35
    out.fit["aero"] = {
        "reference_area_m2": round(area_ref, 3),
        "cl_area_m2": round(cla, 3),
        "cd_area_m2": round(cda, 3),
        "front_downforce_share": round(cla_front / cla, 3) if abs(cla) > 1e-6 else None,
        "wings": [{"name": w.name, "angle": w.angle, "cl": round(w.cl, 4), "cd": round(w.cd, 4),
                   "area_m2": round(w.area, 3), "z_m": w.position[2]} for w in wings],
    }

    # DRS: the named wing's drag and rear downforce, closed against open.
    drs_drag = drs_rear = 0.0
    if drs_wings:
        anim = car.ini("wing_animations.ini")
        for w in wings:
            if w.index not in drs_wings:
                continue
            open_delta = 0.0
            for sec in anim.values():
                if int(number(sec.get("WING"), -1)) == w.index:
                    open_delta = abs(number(sec.get("MAX"), 0.0) - number(sec.get("MIN"), 0.0))
            open_angle = w.angle - open_delta
            cl_o, cd_o = _wing_coefficients(car, aero[f"WING_{w.index}"], open_angle, w.ride_height)
            if cda > 0:
                drs_drag += max((w.cd - cd_o) * w.area, 0.0) / cda
            if cla_rear > 0:
                drs_rear += max((w.cl - cl_o) * w.area * (1 - (w.position[2] - z_rear) / wheelbase), 0.0) / cla_rear
        out.has_drs = drs_drag > 0.0
        out.fit["drs"] = {"wings": sorted(drs_wings), "drag_reduction": round(drs_drag, 3),
                          "rear_downforce_reduction": round(drs_rear, 3)}

    # Tyres: the sim applies the load sensitivity (LS_EXPY about FZ0) and
    # the axle scales itself, so `grip_coefficient` is the car's level at
    # each tyre's reference load, with AC's speed sensitivity taken at the
    # reference speed. front/rear scales carry the axles' balance.
    speed_f = max(1.0 - front.speed_sensitivity * REFERENCE_SPEED_MPS, 0.5)
    speed_r = max(1.0 - rear.speed_sensitivity * REFERENCE_SPEED_MPS, 0.5)
    mu_f = front.dy0 * speed_f
    mu_r = rear.dy0 * speed_r
    grip = 0.5 * (mu_f + mu_r)
    # What that grip is at a loaded outside wheel in a 40 m/s corner, for
    # the report and the comparison with the shipped cars' linear tyres.
    q = 0.5 * AIR_DENSITY * REFERENCE_SPEED_MPS ** 2
    corner_f = (load_f + q * cla_front / 2.0) * 1.35
    corner_r = (load_r + q * cla_rear / 2.0) * 1.35
    mu_corner_f = mu_f * (corner_f / front.fz0) ** (front.ls_expy - 1.0)
    mu_corner_r = mu_r * (corner_r / rear.fz0) ** (rear.ls_expy - 1.0)
    out.fit["tyre"] = {
        "dy0": [front.dy0, rear.dy0], "dx0": [front.dx0, rear.dx0], "fz0_n": [front.fz0, rear.fz0],
        "ls_expy": [front.ls_expy, rear.ls_expy],
        "speed_factor_at_40mps": [round(speed_f, 4), round(speed_r, 4)],
        "corner_wheel_load_n": [round(corner_f), round(corner_r)],
        "mu_at_corner_load": [round(mu_corner_f, 3), round(mu_corner_r, 3)],
    }

    # ---- physics table
    p = "physics"
    length, width, height = bounds_m or (4.5, 1.9, 1.3)
    out.put(p, "length_m", round(length, 3), "the body mesh's bounds")
    out.put(p, "width_m", round(width, 3), "the body mesh's bounds")
    out.put(p, "height_m", round(height, 3), "the body mesh's bounds")
    out.put(p, "max_engine_force_n", round(max_force, 0), "peak torque x gear 1 x final x 0.92 / driven radius (a fallback only)")
    out.put(p, "max_brake_force_n", round(brake_force, 0), "brakes.ini 2 x MAX_TORQUE x (FRONT_SHARE / r_front + rear share / r_rear)")
    out.put(p, "drag_coefficient", round(drag_coefficient, 4), "aero.ini: sum(Cd x area) of every wing over the BODY area")
    out.put(p, "grip_coefficient", round(grip, 4), f"tyres.ini [{front.section}]/[{rear.section}] DY0 x (1 - SPEED_SENSITIVITY x 40 m/s), axle mean")
    out.put(p, "max_steering_angle_rad", round(math.radians(lock / ratio), 4), "car.ini STEER_LOCK / STEER_RATIO")
    out.put(p, "steering_ratio", round(ratio, 3), "car.ini STEER_RATIO (not simulated)")
    out.put(p, "wheelbase_m", round(wheelbase, 4), "suspensions.ini WHEELBASE")
    out.put(p, "track_width_front_m", round(track_f, 4), "suspensions.ini [FRONT] TRACK")
    out.put(p, "track_width_rear_m", round(track_r, 4), "suspensions.ini [REAR] TRACK")
    out.put(p, "wheel_radius_m", round(driven_radius, 4), f"tyres.ini RADIUS of the driven axle ({layout})")
    out.put(p, "cog_height_m", round(cog, 4), "tyres.ini RADIUS - suspensions.ini BASEY, weighted by axle")
    out.put(p, "weight_distribution_front", round(wf, 4), "suspensions.ini CG_LOCATION")
    out.put(p, "brake_bias_front", round(brake_share, 4), "brakes.ini FRONT_SHARE")
    out.put(p, "abs_enabled", bool(_opt(elec, "ABS", "PRESENT", 0)), "electronics.ini [ABS] PRESENT")
    out.put(p, "traction_control_enabled", bool(_opt(elec, "TRACTION_CONTROL", "PRESENT", 0)), "electronics.ini [TRACTION_CONTROL] PRESENT")
    out.put(p, "frontal_area_m2", round(area_ref, 4), "aero.ini BODY wing CHORD x SPAN")
    out.put(p, "lift_coefficient_front", round(-cla_front / area_ref, 4), "aero.ini: every wing's Cl x area on the front axle by its station")
    out.put(p, "lift_coefficient_rear", round(-cla_rear / area_ref, 4), "aero.ini: the rest on the rear axle")
    out.put(p, "drs_drag_reduction", round(drs_drag, 4), "drs.ini's wing, closed against open" if drs_wings else "no DRS wing")
    out.put(p, "drs_rear_downforce_reduction", round(drs_rear, 4), "drs.ini's wing, closed against open" if drs_wings else "no DRS wing")

    # ---- tires
    t = "tires"
    if front.pressure_ideal_psi:
        out.put(t, "optimal_pressure_kpa", round(front.pressure_ideal_psi * PSI_TO_KPA, 1), "tyres.ini PRESSURE_IDEAL (psi)")
    out.put(t, "load_sensitivity_front", round(min(max(front.ls_expy, 0.3), 1.2), 4), "tyres.ini LS_EXPY")
    out.put(t, "load_sensitivity_rear", round(min(max(rear.ls_expy, 0.3), 1.2), 4), "tyres.ini LS_EXPY")
    out.put(t, "reference_load_front_n", round(min(max(front.fz0, 100.0), 30000.0), 1), "tyres.ini FZ0")
    out.put(t, "reference_load_rear_n", round(min(max(rear.fz0, 100.0), 30000.0), 1), "tyres.ini FZ0")
    long_factor = 0.5 * (front.dx0 / front.dy0 + rear.dx0 / rear.dy0)
    out.put(t, "longitudinal_grip_factor", round(min(max(long_factor, 0.6), 1.6), 4), "tyres.ini DX0 / DY0")
    out.put(t, "front_grip_scale", round(min(max(mu_f / grip, 0.5), 1.5), 4), "the front axle's share of the grip")
    out.put(t, "rear_grip_scale", round(min(max(mu_r / grip, 0.5), 1.5), 4), "the rear axle's share of the grip")
    window = read_thermal_window(car, compound)
    if window:
        optimum, half_width, falloff = window
        out.put(t, "optimal_temperature_c", min(max(optimum, 30.0), 150.0), "tyres.ini PERFORMANCE_CURVE: the plateau's middle")
        out.put(t, "temperature_window_c", half_width, "tyres.ini PERFORMANCE_CURVE: half the plateau")
        out.put(t, "temperature_grip_falloff", falloff, "tyres.ini PERFORMANCE_CURVE: the slope past the plateau")

    # ---- engine
    e = "engine"
    coast_torque = _opt(engine, "COAST_REF", "TORQUE", 0.0)
    coast_rpm = _opt(engine, "COAST_REF", "RPM", limiter) or limiter
    redline = max(limiter - 200.0, idle + 500.0)
    out.put(e, "max_power_w", round(power, 0), "the torque curve's peak power")
    out.put(e, "max_torque_nm", round(peak_torque, 1), "the torque curve's peak")
    out.put(e, "idle_rpm", round(idle, 0), "engine.ini MINIMUM")
    out.put(e, "redline_rpm", round(redline, 0), "engine.ini LIMITER - 200")
    out.put(e, "max_rpm", round(limiter + 100.0, 0), "engine.ini LIMITER + 100")
    out.put(e, "rev_limiter_rpm", round(limiter, 0), "engine.ini LIMITER")
    out.put(e, "inertia_kg_m2", round(_opt(engine, "ENGINE_DATA", "INERTIA", 0.2), 4), "engine.ini INERTIA (not simulated)")
    if coast_torque:
        out.put(e, "engine_brake_torque_nm", round(coast_torque * redline / coast_rpm, 1),
                "engine.ini [COAST_REF] TORQUE, scaled to the redline")
    if turbos:
        share = out.fit["boost_at_peak_torque"] / (1.0 + out.fit["boost_at_peak_torque"])
        out.put("engine.turbo", "boosted_share", round(min(share, 0.9), 4), "boost / (1 + boost) at peak torque")
        out.put("engine.turbo", "lag_up_s", round(max(lag_seconds(t.lag_up) for t in turbos), 3), "engine.ini [TURBO_n] LAG_UP at 333 Hz")
        out.put("engine.turbo", "lag_down_s", round(max(lag_seconds(t.lag_dn) for t in turbos), 3), "engine.ini [TURBO_n] LAG_DN at 333 Hz")

    # ---- transmission
    tr = "transmission"
    shifter = bool(_opt(drivetrain, "GEARBOX", "SUPPORTS_SHIFTER", 0))
    out.put(tr, "transmission_type", "Manual" if shifter else "Sequential", "drivetrain.ini SUPPORTS_SHIFTER")
    out.put(tr, "gear_ratios", [round(reverse, 4)] + [round(g, 4) for g in gears], "drivetrain.ini GEAR_R, GEAR_1..n")
    out.put(tr, "final_drive_ratio", round(final, 4), "drivetrain.ini FINAL")
    up = _opt(drivetrain, "GEARBOX", "CHANGE_UP_TIME")
    if up:
        out.put(tr, "shift_time_s", round(up / 1000.0, 3), "drivetrain.ini CHANGE_UP_TIME (not simulated)")
    out.put(tr, "efficiency", efficiency, "AC has no figure; ApexSim's default")

    # ---- drivetrain, differential
    out.put("drivetrain", "layout", layout, f"drivetrain.ini TYPE={drive_type}")
    if layout == "AWD":
        share = _opt(drivetrain, "AWD", "FRONT_SHARE")
        if share is not None:
            out.put("drivetrain", "awd_front_share", round(min(max(share, 0.0), 1.0), 3), "drivetrain.ini [AWD] FRONT_SHARE")
    power_lock = _opt(drivetrain, "DIFFERENTIAL", "POWER", 0.0)
    coast_lock = _opt(drivetrain, "DIFFERENTIAL", "COAST", 0.0)
    preload = _opt(drivetrain, "DIFFERENTIAL", "PRELOAD", 0.0)
    diff_type = "ClutchLSD"
    if power_lock <= 0.0 and coast_lock <= 0.0 and preload <= 0.0:
        diff_type = "Open"
    elif power_lock >= 1.0 and coast_lock >= 1.0:
        diff_type = "Locked"
    out.put("differential", "simulated", True, "an import is balanced with the diff on")
    out.put("differential", "differential_type", diff_type, "drivetrain.ini [DIFFERENTIAL]")
    out.put("differential", "preload_nm", round(min(max(preload, 0.0), 5000.0), 1), "drivetrain.ini PRELOAD")
    out.put("differential", "lock_power", round(min(max(power_lock, 0.0), 1.0), 3), "drivetrain.ini POWER")
    out.put("differential", "lock_coast", round(min(max(coast_lock, 0.0), 1.0), 3), "drivetrain.ini COAST")

    # ---- fuel
    max_fuel = _opt(carini, "FUEL", "MAX_FUEL")
    if max_fuel:
        out.put("fuel", "capacity_liters", round(max_fuel, 1), "car.ini MAX_FUEL")

    # ---- hybrid (ERS)
    ers = car.ini("ers.ini")
    if "KINETIC" in ers:
        k = ers["KINETIC"]
        torque_lut = car.lut(str(k.get("TORQUE_CURVE", "")).split(";")[0].strip())
        coast_lut = car.lut(str(k.get("COAST_CURVE", "")).split(";")[0].strip())
        if torque_lut is None or not torque_lut.x.size or float(torque_lut.y.max()) <= 0.0:
            out.warnings.append("ers.ini's motor curve is empty (a front-axle motor, as on the 919, is not "
                                "modelled): no hybrid")
        else:
            motor_t = float(torque_lut.y.max())
            motor_p = max(r * tq * 2 * math.pi / 60 for r, tq in torque_lut.points) / 1000.0
            regen_p = motor_p
            if coast_lut is not None and coast_lut.x.size:
                regen_p = max(abs(r * tq) * 2 * math.pi / 60 for r, tq in coast_lut.points) / 1000.0
            kj = number(k.get("MAX_KJ_PER_LAP"), 2000.0)
            h = "hybrid"
            out.put(h, "enabled", True, "ers.ini [KINETIC]")
            out.put(h, "battery_capacity_kwh", round(kj / 3600.0, 3), "ers.ini MAX_KJ_PER_LAP (a lap's deployment, as the store)")
            out.put(h, "battery_max_discharge_kw", round(motor_p, 1), "the motor's peak power")
            out.put(h, "battery_max_charge_kw", round(regen_p, 1), "ers.ini COAST_CURVE peak power")
            out.put(h, "motor_max_torque_nm", round(motor_t, 1), "ers.ini TORQUE_CURVE peak")
            out.put(h, "motor_max_power_kw", round(motor_p, 1), "ers.ini TORQUE_CURVE peak power")
            out.put(h, "regen_max_power_kw", round(regen_p, 1), "ers.ini COAST_CURVE peak power")
            out.warnings.append("ERS deployment strategies (ctrl_ers_*.ini) are not modelled: the motor deploys "
                                "whenever the hybrid model allows")

    if "kers.ini" in car.files and "KINETIC" not in ers:
        out.warnings.append("kers.ini (AC's older KERS) is not modelled: the engine alone")

    # ---- suspension: AC's rates are at the wheel, as the sim's are.
    s = "suspension"
    for axle, key in (("FRONT", "front"), ("REAR", "rear")):
        k_, source = springs[axle]
        if k_:
            out.put(s, f"spring_rate_{key}_n_per_m", round(k_, 1), source)
        else:
            out.warnings.append(f"suspensions.ini [{axle}] has no spring rate: ApexSim's default spring")
    for axle, key in (("FRONT", "front"), ("REAR", "rear")):
        b = _opt(susp, axle, "DAMP_BUMP")
        if b:
            out.put(s, f"damper_compression_{key}", round(b, 1), f"suspensions.ini [{axle}] DAMP_BUMP")
    for axle, key in (("FRONT", "front"), ("REAR", "rear")):
        r_ = _opt(susp, axle, "DAMP_REBOUND")
        if r_:
            out.put(s, f"damper_rebound_{key}", round(r_, 1), f"suspensions.ini [{axle}] DAMP_REBOUND")
    for axle, key in (("FRONT", "front"), ("REAR", "rear")):
        arb = _opt(susp, "ARB", axle)
        if arb is not None:
            out.put(s, f"anti_roll_bar_{key}", round(arb, 1), f"suspensions.ini [ARB] {axle}")

    out.fit["reference_speed_mps"] = REFERENCE_SPEED_MPS
    out.fit["cog_height_m"] = round(cog, 4)
    out.car_class = car_class or map_class(car, out.has_drs)
    return out
