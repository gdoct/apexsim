"""AC's physics files -> the figures ApexSim's car.toml takes
(docs/ac-import.md, "AC -> ApexSim: the mapping").

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

#: The server's ride-height aero (server/src/aero.rs): the speed its map is
#: referenced at, and its heave model. Kept equal to the Rust constants so
#: the posture worked out here is the one the server finds.
AERO_REFERENCE_SPEED_MPS = 50.0
FREE_TRAVEL_SHARE = 0.65
BUMP_STIFFENING = 6.0
MAX_EXTENSION_M = 0.05
#: The server's default springs, for a car whose suspensions.ini has none.
DEFAULT_SPRING_FRONT = 80000.0
DEFAULT_SPRING_REAR = 70000.0


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
    #: The hybrid motor's torque by its rpm (`[[hybrid.torque_curve]]`).
    motor_curve: list[tuple[float, float]] = field(default_factory=list)
    #: Figures for the report that are not car.toml keys.
    fit: dict = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    car_class: str = ""
    has_drs: bool = False
    cylinders: int | None = None
    turbo: bool = False
    #: The `[[tires.compound]]` tables, in order; empty keeps the defaults.
    compounds: list[dict] = field(default_factory=list)

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


def speed_factor(t: Tyre) -> float:
    """AC's speed sensitivity at the reference speed, as a grip multiplier."""
    return max(1.0 - t.speed_sensitivity * REFERENCE_SPEED_MPS, 0.5)


def mean_grip(front: Tyre, rear: Tyre) -> float:
    """The car's level of grip on this compound: the axles' `DY0` at the
    reference speed, averaged (what `grip_coefficient` is)."""
    return 0.5 * (front.dy0 * speed_factor(front) + rear.dy0 * speed_factor(rear))


def read_curve(car: CarData, text) -> Lut | None:
    """A curve key's value: a LUT file's name, or the inline form
    (`(|0=0.9|300=1.0|)`) a few cars write in its place."""
    text = str(text or "").strip()
    if not text:
        return None
    if text.startswith("("):
        return parse_inline_lut(text)
    return car.lut(text.split(";")[0].strip())


# --- hybrid (ERS) ----------------------------------------------------------


def _peak(lut: Lut | None) -> tuple[float, float]:
    """A motor curve's peak torque, Nm, and peak power, kW (x in rpm)."""
    if lut is None or not lut.x.size or float(lut.y.max()) <= 0.0:
        return 0.0, 0.0
    power = max(r * tq * 2 * math.pi / 60 for r, tq in lut.points) / 1000.0
    return float(lut.y.max()), power


def ers_min_speed_kph(car: CarData, front_motor: bool, controller: int) -> float | None:
    """The speed below which the default ERS delivery profile
    (`ctrl_ers_<n>.ini`, a front motor's `ctrl_ers_front_<n>.ini`) gives
    the motor nothing: the last speed its SPEED_KMH map is 0 at before it
    first opens, `None` when it opens from standstill (or there is no map)."""
    names = [f"ctrl_ers_{controller}.ini"]
    if front_motor:
        names.insert(0, f"ctrl_ers_front_{controller}.ini")
    name = next((n for n in names if n in car.files), None)
    if name is None:
        return None
    for c in read_controllers(car, name):
        if c.input not in ("SPEED_KMH", "SPEEDKMH"):
            continue
        closed = None
        for x, y in c.lut.points:
            if y > 0.0:
                break
            closed = x
        else:
            return None
        return closed if closed is not None and closed > 0.0 else None
    return None


def _motor_curve(lut, scale: float) -> list[tuple[float, float]]:
    """A motor's TORQUE_CURVE as `[[hybrid.torque_curve]]` points (its rpm,
    torque x `scale`), ordered by rpm; empty for none."""
    if lut is None or not lut.x.size:
        return []
    return [(round(float(r), 1), round(float(t) * scale, 1)) for r, t in sorted(lut.points)]


def map_ers(car: CarData, ers: dict, out, driven_radius: float, front_radius: float) -> None:
    """ers.ini's motors onto `[hybrid]`: the crank motor ([KINETIC], torque
    by engine rpm) or a front-axle one ([FRONT_MOTORS], torque at the axle
    by wheel rpm), whichever is the stronger; the store from its
    DISCHARGE_TIME, the lap budget from MAX_KJ_PER_LAP, the MGU-H share, and
    the default delivery profile's minimum speed."""
    k = ers["KINETIC"]
    crank_lut = read_curve(car, k.get("TORQUE_CURVE"))
    crank_t, crank_p = _peak(crank_lut)
    fm = ers.get("FRONT_MOTORS", {})
    front_lut = read_curve(car, fm.get("TORQUE_CURVE")) if fm else None
    front_t, front_p = _peak(front_lut) if fm else (0.0, 0.0)
    if crank_p <= 0.0 and front_p <= 0.0:
        out.warnings.append("ers.ini has no motor curve on either axle: no hybrid")
        return
    front_motor = front_p > crank_p
    if crank_p > 0.0 and front_p > 0.0:
        dropped = "the crank motor ([KINETIC])" if front_motor else "the front motor ([FRONT_MOTORS])"
        out.warnings.append(f"ers.ini has a crank motor ({crank_p:.0f} kW) and a front motor ({front_p:.0f} kW): "
                            f"the sim has one motor, so {dropped} is dropped")
    h = "hybrid"
    out.put(h, "enabled", True, "ers.ini [KINETIC]")
    if front_motor:
        # The sim turns the motor with the wheels at the driven radius: the
        # torque is scaled so the force at the front tyres and the power
        # come out as AC's.
        motor_t = front_t * driven_radius / max(front_radius, 0.05)
        motor_p = front_p
        regen_p = motor_p
        discharge_s = number(fm.get("DISCHARGE_TIME"), number(k.get("DISCHARGE_TIME"), 0.0)) / 1000.0
        out.put(h, "axle", "front", "ers.ini [FRONT_MOTORS]: the motor drives the front axle")
        out.put(h, "motor_max_torque_nm", round(motor_t, 1),
                "ers.ini [FRONT_MOTORS] TORQUE_CURVE peak, at the axle (x driven / front tyre radius)")
        out.put(h, "motor_max_power_kw", round(motor_p, 1), "ers.ini [FRONT_MOTORS] TORQUE_CURVE peak power (wheel rpm)")
        out.motor_curve = _motor_curve(front_lut, driven_radius / max(front_radius, 0.05))
        regen_source = "the front motor's peak power (it recovers through the fronts)"
        if fm.get("FRONT_TORQUE_VECTORING_BIAS") is not None:
            out.warnings.append("ers.ini FRONT_TORQUE_VECTORING_BIAS is not modelled: the front differential "
                                "splits the motor's torque")
    else:
        motor_t, motor_p = crank_t, crank_p
        regen_p = motor_p
        regen_source = "the motor's peak power (no COAST_CURVE)"
        coast_lut = read_curve(car, k.get("COAST_CURVE"))
        if coast_lut is not None and coast_lut.x.size:
            regen_p = max(abs(r * tq) * 2 * math.pi / 60 for r, tq in coast_lut.points) / 1000.0
            regen_source = "ers.ini COAST_CURVE peak power"
        discharge_s = number(k.get("DISCHARGE_TIME"), 0.0) / 1000.0
        out.put(h, "motor_max_torque_nm", round(motor_t, 1), "ers.ini TORQUE_CURVE peak")
        out.put(h, "motor_max_power_kw", round(motor_p, 1), "ers.ini TORQUE_CURVE peak power")
        out.motor_curve = _motor_curve(crank_lut, 1.0)
    # DISCHARGE_TIME is how long the store lasts deploying flat out, so the
    # store is the motor's power over that time; MAX_KJ_PER_LAP is what may
    # be deployed per lap (a regulation, not the store).
    kj = number(k.get("MAX_KJ_PER_LAP"), 0.0)
    store_kj = motor_p * discharge_s if discharge_s > 0.0 else (kj if 0.0 < kj < 1.0e5 else 4000.0)
    out.put(h, "battery_capacity_kwh", round(store_kj / 3600.0, 3),
            "ers.ini DISCHARGE_TIME x the motor's peak power" if discharge_s > 0.0
            else "ers.ini MAX_KJ_PER_LAP (no DISCHARGE_TIME)")
    out.put(h, "battery_max_discharge_kw", round(motor_p, 1), "the motor's peak power")
    out.put(h, "battery_max_charge_kw", round(regen_p, 1), regen_source)
    out.put(h, "regen_max_power_kw", round(regen_p, 1), regen_source)
    # Road cars file an absurd budget (the P1's 10 000 000 kJ): no limit.
    if 0.0 < kj < 1.0e5:
        out.put(h, "deploy_kj_per_lap", round(kj, 1), "ers.ini MAX_KJ_PER_LAP")
    heat = ers.get("HEAT", {})
    heat_perc = number(heat.get("TORQUE_PERC"), 0.0)
    if heat_perc > 0.0:
        out.put(h, "heat_recovery_kw", round(motor_p * heat_perc / 100.0, 1),
                "ers.ini [HEAT] TORQUE_PERC x the motor's peak power (the MGU-H feeding the motor)")
    controller = int(number(k.get("DEFAULT_CONTROLLER"), 0.0) or 0)
    min_kph = ers_min_speed_kph(car, front_motor, controller)
    if min_kph is not None:
        out.put(h, "deploy_min_speed_kph", round(min_kph, 1),
                f"ctrl_ers{'_front' if front_motor else ''}_{controller}.ini SPEED_KMH: nothing below")
    if any(n.startswith("ctrl_ers") for n in car.files):
        out.warnings.append("ERS delivery profiles (ctrl_ers_*.ini) are read for the default profile's minimum "
                            "speed only: their throttle maps, speed caps, oversteer / slip cuts and gear masks "
                            "are not carried, and the car gets the sim's Harvest / Balanced / Attack modes and "
                            "the overtake button in place of the other profiles")


# --- brakes ---------------------------------------------------------------

#: The classes the server races on carbon brakes (`BrakeMaterial::for_class`,
#: server/src/brakes.rs; a test keeps the two equal).
CARBON_CLASSES = ("F1", "Hypercar", "LMP2", "LMP1", "LMP")
#: The server's pad friction against disc temperature (`BrakeMaterial::
#: friction`; a test keeps them equal): a pad curve in brakes.ini is read as
#: whichever of the two it resembles.
BRAKE_FRICTION = {
    "carbon": [(0.0, 0.55), (150.0, 0.62), (300.0, 0.85), (400.0, 1.0), (900.0, 1.0), (1050.0, 0.85), (1200.0, 0.6)],
    "steel": [(0.0, 0.9), (100.0, 0.96), (200.0, 1.0), (600.0, 1.0), (700.0, 0.88), (850.0, 0.55)],
}
PAD_TEMPS = np.arange(0.0, 1001.0, 25.0)


def pad_material(car: CarData) -> tuple[str, dict[str, float]] | None:
    """Which of the server's two brake materials `brakes.ini`'s pad curves
    (`[TEMPS_FRONT]` / `[TEMPS_REAR]` `PERF_CURVE`, friction against disc
    temperature) resemble: ``("carbon" | "steel", {material: mean squared
    difference})`` over 0-1000 degrees, each curve scaled to its peak. A car
    whose brakes.ini has no curve (most of them) has nothing to say."""
    brakes = car.ini("brakes.ini")
    tables = {m: np.interp(PAD_TEMPS, [t for t, _ in pts], [f for _, f in pts]) for m, pts in BRAKE_FRICTION.items()}
    err = {m: 0.0 for m in tables}
    seen = 0
    for section in ("TEMPS_FRONT", "TEMPS_REAR"):
        curve = read_curve(car, brakes.get(section, {}).get("PERF_CURVE"))
        if curve is None or curve.x.size < 2 or float(curve.y.max()) <= 0.0:
            continue
        y = np.array([curve(t) for t in PAD_TEMPS]) / float(curve.y.max())
        for m, table in tables.items():
            err[m] += float(np.mean((y - table) ** 2))
        seen += 1
    if not seen:
        return None
    return ("carbon" if err["carbon"] < err["steel"] else "steel"), {m: round(v / seen, 4) for m, v in err.items()}


# --- tyre compounds -------------------------------------------------------

#: The server's limits on a compound (`car_loader.rs` validation).
GRIP_RANGE = (0.5, 1.5)
WEAR_RANGE = (0.2, 5.0)
SHIFT_RANGE_C = (-80.0, 80.0)
#: The share of peak grip a worn tyre has left when its life is counted over.
WORN_GRIP_SHARE = 0.95
#: The rain tyres an ApexSim car has by default (`tyre_thermal::default_compounds`),
#: given to an import whose AC compounds are all slicks (AC has no wet tyre).
RAIN_COMPOUNDS = (
    {"name": "intermediate", "kind": "intermediate", "grip": 1.0, "wear": 1.5, "window_shift_c": -25.0},
    {"name": "wet", "kind": "wet", "grip": 1.0, "wear": 2.0, "window_shift_c": -35.0},
)


def compound_kind(name: str) -> str:
    n = name.lower()
    if "wet" in n or "rain" in n:
        return "wet"
    if "inter" in n and "international" not in n:
        return "intermediate"
    return "slick"


def tyre_life_km(car: CarData, compound: int) -> float | None:
    """How far a compound runs before it has lost 5% of its best grip, from
    its `WEAR_CURVE` (virtual km against grip; the axles' mean). None when
    there is no curve."""
    t = car.ini("tyres.ini")
    lives = []
    for axle in ("FRONT", "REAR"):
        section = axle if compound == 0 else f"{axle}_{compound}"
        curve = car.lut(str(t.get(section, {}).get("WEAR_CURVE", "")).split(";")[0].strip())
        if curve is None or curve.x.size < 2:
            continue
        peak = float(curve.y.max())
        if peak <= 0.0:
            continue
        threshold = WORN_GRIP_SHARE * peak
        # Past the curve's end it is taken to last half as far again.
        life = float(curve.x.max()) * 1.5
        start = int(np.argmax(curve.y))
        for i in range(start + 1, curve.x.size):
            if curve.y[i] <= threshold:
                x0, y0 = float(curve.x[i - 1]), float(curve.y[i - 1])
                x1, y1 = float(curve.x[i]), float(curve.y[i])
                life = x1 if y0 == y1 else x0 + (x1 - x0) * (y0 - threshold) / (y0 - y1)
                break
        lives.append(max(life, 0.1))
    return sum(lives) / len(lives) if lives else None


def map_compounds(car: CarData, reference: int, warnings: list[str]) -> list[dict] | None:
    """The car's `[[tires.compound]]` list from tyres.ini's compounds, softest
    slick first (the setup knob's order), the reference (the compound the
    rest of the import is calibrated on) at grip 1, wear 1, shift 0.

    - `grip`: the compound's mean `DY0` (at the reference speed) over the
      reference's;
    - `wear`: the reference's life over its own (`tyre_life_km`);
    - `window_shift_c`: its thermal window's middle less the reference's;
    - `kind` from the name; a car with no wet or intermediate gets
      ApexSim's (AC has none), so the garage still has rain tyres.

    ``None`` for a car with a single compound: it keeps the default list."""
    compounds = tyre_compounds(car)
    if len(compounds) < 2:
        return None
    ref_front, ref_rear, _ = read_tyres(car, reference)
    ref_grip = mean_grip(ref_front, ref_rear)
    ref_window = read_thermal_window(car, reference)
    ref_life = tyre_life_km(car, reference)
    rows = []
    for index, name in compounds:
        try:
            front, rear, _ = read_tyres(car, index)
        except PhysicsError as e:
            warnings.append(f"compound {index} ({name}) skipped: {e}")
            continue
        window = read_thermal_window(car, index)
        life = tyre_life_km(car, index)
        grip = mean_grip(front, rear) / ref_grip
        shift = window[0] - ref_window[0] if window and ref_window else 0.0
        wear = ref_life / life if life and ref_life else 1.0
        rows.append({
            "index": index,
            "name": name,
            "kind": compound_kind(name),
            "grip": round(min(max(grip, GRIP_RANGE[0]), GRIP_RANGE[1]), 3),
            "wear": round(min(max(wear, WEAR_RANGE[0]), WEAR_RANGE[1]), 2),
            "window_shift_c": round(min(max(shift, SHIFT_RANGE_C[0]), SHIFT_RANGE_C[1]), 1),
            "reference": index == reference,
        })
    if len(rows) < 2:
        return None
    # Softer first: more grip, then a cooler window, then faster wear. The
    # treaded tyres come last, as the server's knob expects.
    rank = {"slick": 0, "intermediate": 1, "wet": 2}
    rows.sort(key=lambda r: (rank[r["kind"]], -round(r["grip"], 2), r["window_shift_c"], -r["wear"], r["index"]))
    for kind, extra in zip(("intermediate", "wet"), RAIN_COMPOUNDS):
        if not any(r["kind"] == kind for r in rows):
            rows.append(dict(extra, index=None, reference=False))
    rows.sort(key=lambda r: rank[r["kind"]])  # stable: the rain tyres to the end
    return rows


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


def axle_travel(delta_n: float, spring_n_per_m: float, static_height_m: float) -> float:
    """How far an axle moves down for a load `delta_n` over its static
    load: the server's `aero::axle_travel`, bump rubbers and all."""
    k = 2.0 * max(spring_n_per_m, 1.0)
    travel = delta_n / k
    free = FREE_TRAVEL_SHARE * max(static_height_m, 0.0)
    if travel <= free:
        return max(travel, -MAX_EXTENSION_M)
    return free + (delta_n - k * free) / (k * BUMP_STIFFENING)


def wing_height(z: float, front_m: float, rear_m: float, z_front: float, z_rear: float) -> float:
    """A wing's height above the road, from the two axles' ride heights by
    where it sits between them (held at the axle's past either end)."""
    span = z_front - z_rear
    t = 0.5 if abs(span) < 1e-6 else min(max((z - z_rear) / span, 0.0), 1.0)
    return rear_m + t * (front_m - rear_m)


@dataclass
class AeroWing:
    """One aero.ini wing for the ride-height map: `cl(h)` is its Cl x area,
    m2, at a ground height h (the angle and the gain already in), `z` its
    station in the physics frame."""
    cl: object
    z: float


def aero_posture(wings: list[AeroWing], static: tuple[float, float], springs: tuple[float, float],
                 z_front: float, z_rear: float, wheelbase: float) -> dict:
    """Where the car rides at the server's reference speed on its springs
    with the downforce its wings make there, and the ride-height map around
    that posture, as the server's `[aero]` keys want it:

    - `posture`: (front, rear) ride height, m, at `AERO_REFERENCE_SPEED_MPS`
      (the wings' heights feed back into their downforce: iterated);
    - `cla`: (front, rear) Cl x area there, split by station as the rest of
      the import does;
    - `ride_height_sensitivity`: downforce gained per cm lower (mean height,
      rake held), a share of the total there;
    - `rake_sensitivity`: the front share's gain per cm more rake;
    - `stall_height_m`: the mean height below the tables' peak where the
      total falls under 90% of it, 0 when it never does above the road.
    """

    def split(front_m: float, rear_m: float) -> tuple[float, float]:
        cla_f = cla = 0.0
        for w in wings:
            c = w.cl(wing_height(w.z, front_m, rear_m, z_front, z_rear))
            cla += c
            cla_f += c * (w.z - z_rear) / wheelbase
        return cla_f, cla - cla_f

    q = 0.5 * AIR_DENSITY * AERO_REFERENCE_SPEED_MPS ** 2
    front, rear = static
    for _ in range(20):
        cf, cr = split(front, rear)
        front = max(static[0] - axle_travel(q * cf, springs[0], static[0]), 0.0)
        rear = max(static[1] - axle_travel(q * cr, springs[1], static[1]), 0.0)
    cla = split(front, rear)

    def total(mean: float, rake: float) -> float:
        return sum(split(max(mean - rake / 2.0, 0.0), max(mean + rake / 2.0, 0.0)))

    def share(mean: float, rake: float) -> float:
        cf, cr = split(max(mean - rake / 2.0, 0.0), max(mean + rake / 2.0, 0.0))
        return cf / (cf + cr) if abs(cf + cr) > 1e-9 else 0.5

    mean0, rake0 = 0.5 * (front + rear), rear - front
    t0 = total(mean0, rake0)
    sensitivity = (total(mean0 - 0.005, rake0) - total(mean0 + 0.005, rake0)) / t0 if t0 > 1e-9 else 0.0
    rake_sensitivity = share(mean0, rake0 + 0.005) - share(mean0, rake0 - 0.005)

    # Down from the posture a millimetre at a time: the tables' peak, then
    # where the total has fallen under 90% of it.
    stall = 0.0
    best = t0
    h = mean0
    while h > 0.0:
        t = total(h, rake0)
        if t > best:
            best = t
        elif t < 0.9 * best:
            stall = h
            break
        h -= 0.001
    return {
        "posture": (front, rear),
        "cla": cla,
        "ride_height_sensitivity": sensitivity,
        "rake_sensitivity": rake_sensitivity,
        "stall_height_m": stall,
    }


def wheel_spring_rate(susp: dict, axle: str) -> tuple[float | None, str]:
    """The axle's spring rate at each wheel in heave: the corner spring plus
    half the axle's heave spring (`[HEAVE_FRONT]`), which an open-wheeler
    carries beside (or instead of) its corner springs. Heave is what the
    server's springs do most (the static sag, the ride height under
    downforce); roll has the anti-roll bars."""
    k = _opt(susp, axle, "SPRING_RATE") or 0.0
    heave = _opt(susp, f"HEAVE_{axle}", "SPRING_RATE") or 0.0
    if heave > 0:
        return k + heave / 2.0, (f"suspensions.ini [{axle}] SPRING_RATE {k:g} + "
                                  f"[HEAVE_{axle}] SPRING_RATE / 2")
    if k > 0:
        return k, f"suspensions.ini [{axle}] SPRING_RATE"
    return None, ""


# --- the mapping ----------------------------------------------------------


# ---- suspension geometry (server geometry.rs)

def _point(sections: dict, section: str, key: str):
    """A suspensions.ini point `x, y, z` (x toward the car, y up, z forward,
    from the wheel's hub), or None."""
    return vector(sections.get(section, {}).get(key), 3)


def _circle_meet(c1, r1, c2, r2, near):
    """Of the two points at `r1` from `c1` and `r2` from `c2` (2D), the one
    nearer `near`; None if the circles miss."""
    dx, dy = c2[0] - c1[0], c2[1] - c1[1]
    d = math.hypot(dx, dy)
    if d < 1e-9 or d > r1 + r2 or d < abs(r1 - r2):
        return None
    a = (r1 * r1 - r2 * r2 + d * d) / (2 * d)
    h = math.sqrt(max(r1 * r1 - a * a, 0.0))
    mx, my = c1[0] + a * dx / d, c1[1] + a * dy / d
    candidates = [(mx + h * dy / d, my - h * dx / d), (mx - h * dy / d, my + h * dx / d)]
    return min(candidates, key=lambda q: math.hypot(q[0] - near[0], q[1] - near[1]))


#: The highest roll centre taken, m: past it a linkage's points are
#: garbage (a solid axle drawn as wishbones) or the geometry is nearly all
#: of the load transfer, which the server's model cannot carry (it takes 0.5).
ROLL_CENTRE_MAX_M = 0.3

#: Wheel travel the camber gain is measured over, m.
GAIN_PROBE_M = 0.005


def camber_gain(sections: dict, axle: str, track_m: float):
    """How much of the body's roll the linkage gives back as camber, 0..1,
    from the front view of the suspension: the wheel lifted 5 mm in bump,
    the upright's lean change per metre of travel times half the track is
    the share of a roll angle it recovers. None when the points are not
    there or it is neither a double wishbone nor a strut."""
    kind = str(sections.get(axle, {}).get("TYPE", "")).split(";")[0].strip().upper()
    bottom_f = _point(sections, axle, "WBCAR_BOTTOM_FRONT")
    bottom_r = _point(sections, axle, "WBCAR_BOTTOM_REAR")
    ball = _point(sections, axle, "WBTYRE_BOTTOM")
    if not (bottom_f and bottom_r and ball):
        return None
    # Front view: across (x, toward the car) and up (y).
    lower = ((bottom_f[0] + bottom_r[0]) / 2, (bottom_f[1] + bottom_r[1]) / 2)
    b = (ball[0], ball[1])
    r_lower = math.hypot(b[0] - lower[0], b[1] - lower[1])
    if r_lower < 1e-6:
        return None
    # The lower ball joint lifted GAIN_PROBE_M on its arc.
    sin_new = (b[1] + GAIN_PROBE_M - lower[1]) / r_lower
    if abs(sin_new) > 1:
        return None
    ang = math.atan2(b[1] - lower[1], b[0] - lower[0])
    cands = [math.asin(sin_new), math.pi - math.asin(sin_new)]
    new_ang = min(cands, key=lambda a: abs(math.atan2(math.sin(a - ang), math.cos(a - ang))))
    b2 = (lower[0] + r_lower * math.cos(new_ang), lower[1] + r_lower * math.sin(new_ang))
    lean = lambda top, bot: math.atan2(top[0] - bot[0], top[1] - bot[1])
    if kind == "DWB":
        top_f = _point(sections, axle, "WBCAR_TOP_FRONT")
        top_r = _point(sections, axle, "WBCAR_TOP_REAR")
        top = _point(sections, axle, "WBTYRE_TOP")
        if not (top_f and top_r and top):
            return None
        upper = ((top_f[0] + top_r[0]) / 2, (top_f[1] + top_r[1]) / 2)
        t = (top[0], top[1])
        t2 = _circle_meet(upper, math.hypot(t[0] - upper[0], t[1] - upper[1]),
                          b2, math.hypot(t[0] - b[0], t[1] - b[1]), t)
        if t2 is None:
            return None
        change = lean(t2, b2) - lean(t, b)
    elif kind == "STRUT":
        mount = _point(sections, axle, "STRUT_CAR")
        if not mount:
            return None
        c = (mount[0], mount[1])
        change = lean(c, b2) - lean(c, b)
    else:
        return None
    # x points at the car: the top leaning further in is negative camber
    # gained in bump.
    return min(max(change / GAIN_PROBE_M * track_m / 2, 0.0), 1.0)


def toe_in_deg(sections: dict, axle: str):
    """AC's TOE_OUT (the steering arm's length change, m) as degrees of
    toe-in per wheel, over the arm from the kingpin to the tie rod."""
    toe_out = _opt(sections, axle, "TOE_OUT")
    steer = _point(sections, axle, "WBTYRE_STEER")
    low = _point(sections, axle, "WBTYRE_BOTTOM")
    high = _point(sections, axle, "WBTYRE_TOP") or _point(sections, axle, "STRUT_CAR")
    if toe_out is None or not (steer and low and high):
        return None
    # The kingpin's fore-aft position at the tie rod's height.
    span = high[1] - low[1]
    k = (steer[1] - low[1]) / span if abs(span) > 1e-6 else 0.0
    kingpin_z = low[2] + k * (high[2] - low[2])
    arm = abs(steer[2] - kingpin_z)
    if arm < 0.02:
        return None
    return -math.degrees(math.atan(toe_out / arm))


def _line_meet(p1, d1, p2, d2):
    """Where the lines `p1 + t d1` and `p2 + s d2` cross (2D); None if they
    are parallel."""
    cross = d1[0] * d2[1] - d1[1] * d2[0]
    if abs(cross) < 1e-9 * max(math.hypot(*d1) * math.hypot(*d2), 1e-12):
        return None
    t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / cross
    return (p1[0] + t * d1[0], p1[1] + t * d1[1])


def roll_centre_m(sections: dict, axle: str, radius_m: float, track_m: float):
    """The axle's roll centre, m above the road, from the front view of its
    linkage at the design position: the instant centre of the upright (the
    lower arm's line met by the upper arm's, or for a strut by the line at
    right angles to its axis through the top mount), joined to the tyre's
    contact patch, crossing the car's centreline. The points are from the
    hub (x toward the car, y up), so the contact patch is `radius_m` below
    it and the centreline `track_m / 2` inboard. None when the points are not
    there or the linkage is neither a double wishbone nor a strut. The value
    may fall outside what the server takes (a roll centre below the road):
    the caller holds it to the range."""
    kind = str(sections.get(axle, {}).get("TYPE", "")).split(";")[0].strip().upper()
    bottom_f = _point(sections, axle, "WBCAR_BOTTOM_FRONT")
    bottom_r = _point(sections, axle, "WBCAR_BOTTOM_REAR")
    ball = _point(sections, axle, "WBTYRE_BOTTOM")
    if not (bottom_f and bottom_r and ball):
        return None
    lower_in = ((bottom_f[0] + bottom_r[0]) / 2, (bottom_f[1] + bottom_r[1]) / 2)
    lower_dir = (ball[0] - lower_in[0], ball[1] - lower_in[1])
    if math.hypot(*lower_dir) < 1e-6:
        return None
    if kind == "DWB":
        top_f = _point(sections, axle, "WBCAR_TOP_FRONT")
        top_r = _point(sections, axle, "WBCAR_TOP_REAR")
        top = _point(sections, axle, "WBTYRE_TOP")
        if not (top_f and top_r and top):
            return None
        upper_in = ((top_f[0] + top_r[0]) / 2, (top_f[1] + top_r[1]) / 2)
        upper_dir = (top[0] - upper_in[0], top[1] - upper_in[1])
    elif kind == "STRUT":
        mount = _point(sections, axle, "STRUT_CAR")
        foot = _point(sections, axle, "STRUT_TYRE") or ball
        if not mount:
            return None
        axis = (mount[0] - foot[0], mount[1] - foot[1])
        if math.hypot(*axis) < 1e-6:
            return None
        upper_in, upper_dir = (mount[0], mount[1]), (-axis[1], axis[0])
    else:
        return None
    if math.hypot(*upper_dir) < 1e-6:
        return None
    patch = (0.0, -radius_m)
    centre = _line_meet(lower_in, lower_dir, upper_in, upper_dir)
    # Parallel arms: the instant centre is at infinity along them.
    direction = lower_dir if centre is None else (centre[0] - patch[0], centre[1] - patch[1])
    if abs(direction[0]) < 1e-6:
        return None
    t = (track_m / 2.0) / direction[0]
    return patch[1] + t * direction[1] + radius_m


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
    tyres_ini = car.ini("tyres.ini")
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
    k_f = springs["FRONT"][0] or DEFAULT_SPRING_FRONT
    k_r = springs["REAR"][0] or DEFAULT_SPRING_REAR
    load_f = mass * G * wf / 2.0
    load_r = mass * G * (1.0 - wf) / 2.0
    sag_f = load_f / k_f + (load_f / front.rate if front.rate else 0.0)
    sag_r = load_r / k_r + (load_r / rear.rate if rear.rate else 0.0)
    ride_f = cg_f + _opt(carini, "RIDE", "PICKUP_FRONT_HEIGHT", -cg_f + 0.08) + _opt(susp, "FRONT", "ROD_LENGTH", 0.0) - sag_f
    ride_r = cg_r + _opt(carini, "RIDE", "PICKUP_REAR_HEIGHT", -cg_r + 0.08) + _opt(susp, "REAR", "ROD_LENGTH", 0.0) - sag_r
    ride_f, ride_r = max(ride_f, 0.02), max(ride_r, 0.02)
    out.fit["static_ride_height_m"] = [round(ride_f, 4), round(ride_r, 4)]

    # Aero: every wing at its authored angle, at the height it runs at the
    # server's reference speed. A car whose wings have ground-height tables
    # (LUT_GH_CL) gets the server's ride-height map from them (`[aero]`):
    # its lift coefficients are the ones it makes at that posture, where
    # the server's map is zero, and the map's slopes are the tables'.
    wings: list[Wing] = []
    drs_ini = car.ini("drs.ini")
    drs_wings = {int(m.group(1)) for sec in drs_ini for m in [re.match(r"WING_(\d+)$", sec)] if m}
    z_front_axle = wheelbase * (1.0 - wf)
    z_rear_axle = -wheelbase * wf
    sections = []
    i = 0
    while f"WING_{i}" in aero:
        s = aero[f"WING_{i}"]
        pos = vector(s.get("POSITION")) or [0.0, 0.0, 0.0]
        sections.append((i, s, pos, number(s.get("ANGLE"), 0.0),
                         number(s.get("CHORD"), 1.0) * number(s.get("SPAN"), 1.0)))
        i += 1
    has_ground_tables = any(str(s.get("LUT_GH_CL", "")).strip() for _, s, _, _, _ in sections)
    aero_map = None
    if has_ground_tables:
        aero_map = aero_posture(
            [AeroWing(cl=(lambda h, s=s, a=angle, area=area: _wing_coefficients(car, s, a, h)[0] * area),
                      z=pos[2]) for _, s, pos, angle, area in sections],
            (ride_f, ride_r), (k_f, k_r), z_front_axle, z_rear_axle, wheelbase)
        post_f, post_r = aero_map["posture"]
    else:
        post_f, post_r = ride_f, ride_r
    for i, s, pos, angle, area in sections:
        height = (wing_height(pos[2], post_f, post_r, z_front_axle, z_rear_axle) if has_ground_tables
                  else (ride_f if pos[2] >= 0.0 else ride_r))
        cl, cd = _wing_coefficients(car, s, angle, height)
        wings.append(Wing(i, str(s.get("NAME", f"WING_{i}")).strip(), area, pos, angle, cl, cd, height))
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
    if aero_map:
        out.fit["aero"]["posture_at_reference_m"] = [round(post_f, 4), round(post_r, 4)]
        out.fit["aero"]["map"] = {k: round(v, 5) for k, v in aero_map.items()
                                  if k in ("ride_height_sensitivity", "rake_sensitivity", "stall_height_m")}

    # Yaw: each wing's YAW_CL_GAIN is the share of its lift it loses per
    # radian the car runs sideways to the air; the car's is theirs weighted
    # by the downforce each makes, per degree (the server's unit).
    yaw_num = yaw_den = 0.0
    for w, (_, s_, _, _, _) in zip(wings, sections):
        down = w.cl * w.area
        if down > 0.0:
            yaw_num += down * max(-number(s_.get("YAW_CL_GAIN"), 0.0), 0.0)
            yaw_den += down
    yaw_per_deg = math.radians(yaw_num / yaw_den) if yaw_den > 0.0 else 0.0

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

    # ---- aero posture: the server's ride-height map, from the wings' tables.
    if yaw_per_deg > 0.0:
        out.put("aero", "yaw_sensitivity", round(min(yaw_per_deg, 0.05), 5),
                "aero.ini YAW_CL_GAIN (per radian) of the wings, weighted by downforce, per degree")
    if aero_map:
        a = "aero"
        clamp = lambda v, lo, hi: min(max(v, lo), hi)  # noqa: E731
        out.put(a, "ride_height_front_m", round(clamp(ride_f, 0.005, 0.3), 4),
                "car.ini [RIDE] pickup, ROD_LENGTH and the static sag")
        out.put(a, "ride_height_rear_m", round(clamp(ride_r, 0.005, 0.3), 4),
                "car.ini [RIDE] pickup, ROD_LENGTH and the static sag")
        out.put(a, "ride_height_sensitivity", round(clamp(aero_map["ride_height_sensitivity"], 0.0, 0.1), 4),
                "aero.ini LUT_GH_CL: downforce gained per cm lower at the 50 m/s posture")
        out.put(a, "rake_sensitivity", round(clamp(aero_map["rake_sensitivity"], 0.0, 0.05), 4),
                "aero.ini LUT_GH_CL: front share gained per cm more rake")
        out.put(a, "stall_height_m", round(clamp(aero_map["stall_height_m"], 0.0, 0.1), 4),
                "aero.ini LUT_GH_CL: mean height where the tables fall under 90% of their peak")

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
    camber_gain_tyre = [v for v in (_opt(tyres_ini, "FRONT" if compound == 0 else f"FRONT_{compound}", "CAMBER_GAIN"),
                                    _opt(tyres_ini, "REAR" if compound == 0 else f"REAR_{compound}", "CAMBER_GAIN"))
                        if v is not None]
    compound_rows = map_compounds(car, compound, out.warnings)
    if compound_rows:
        out.compounds = compound_rows
        out.fit["compounds"] = [dict(r) for r in compound_rows]

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
    tank = vector(carini.get("FUELTANK", {}).get("POSITION"), 3)
    if tank:
        # The tank's z from the CG, between the axles: the lever rule gives
        # the share of the fuel's weight the front axle carries.
        share = min(max((tank[2] - z_rear) / wheelbase, 0.0), 1.0)
        out.put("fuel", "tank_front_share", round(share, 3), "car.ini [FUELTANK] POSITION z, by the lever rule between the axles")

    # ---- hybrid (ERS)
    ers = car.ini("ers.ini")
    if "KINETIC" in ers:
        map_ers(car, ers, out, driven_radius, front.radius)

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
    # Geometry: static camber, the linkage's camber gain, toe and bump stops.
    for axle, key in (("FRONT", "front"), ("REAR", "rear")):
        camber = _opt(susp, axle, "STATIC_CAMBER")
        if camber is not None:
            out.put(s, f"camber_{key}_deg", round(camber, 2), f"suspensions.ini [{axle}] STATIC_CAMBER")
        track = _opt(susp, axle, "TRACK") or 1.6
        gain = camber_gain(susp, axle, track)
        if gain is not None:
            out.put(s, f"camber_gain_{key}", round(gain, 3),
                    f"suspensions.ini [{axle}] points: camber per metre of bump x half the track")
        elif camber is not None:
            out.warnings.append(f"suspensions.ini [{axle}]: no double-wishbone or strut points, the sim's "
                                "default camber gain (0.5)")
        toe = toe_in_deg(susp, axle)
        if toe is not None:
            out.put(s, f"toe_{key}_deg", round(toe, 3),
                    f"suspensions.ini [{axle}] TOE_OUT over the steering arm (positive toe-in)")
    rates = [_opt(susp, a, "BUMP_STOP_RATE") for a in ("FRONT", "REAR")]
    gaps = []
    for a in ("FRONT", "REAR"):
        cands = [v for v in (_opt(susp, a, "BUMPSTOP_UP"), _opt(susp, a, "PACKER_RANGE")) if v and v > 0]
        if cands:
            gaps.append(min(cands))
    if gaps and all(rates):
        out.put(s, "bump_stop_gap_m", round(min(gaps), 4),
                "suspensions.ini BUMPSTOP_UP / PACKER_RANGE, the nearer (one gap for the car)")
        out.put(s, "bump_stop_rate_n_per_m", round(min(max(sum(rates) / 2, 0.0), 2.0e6), 1),
                "suspensions.ini BUMP_STOP_RATE, both axles averaged")

    # Roll centres from the front view of the linkage: both or neither (the
    # server reads a lone one as ground level on the other axle).
    centres = {}
    for axle, tyre_ in (("FRONT", front), ("REAR", rear)):
        centres[axle] = roll_centre_m(susp, axle, tyre_.radius, _opt(susp, axle, "TRACK") or 1.6)
    if all(v is not None for v in centres.values()):
        out.fit["roll_centre_m"] = {a: round(v, 4) for a, v in centres.items()}
        for axle, key in (("FRONT", "front"), ("REAR", "rear")):
            held = min(max(centres[axle], 0.0), ROLL_CENTRE_MAX_M)
            if abs(held - centres[axle]) > 0.005:
                out.warnings.append(f"suspensions.ini [{axle}]: the roll centre works out at {centres[axle]:.3f} m, "
                                    f"held to {held:g} m (the server takes none below the road)")
            out.put(s, f"roll_centre_{key}_m", round(held, 4),
                    f"suspensions.ini [{axle}] points: the instant centre of the linkage joined to the contact patch")
    elif any(susp.get(a, {}).get("WBCAR_BOTTOM_FRONT") for a in ("FRONT", "REAR")):
        out.warnings.append("suspensions.ini: no roll centre for an axle that is neither a double wishbone nor "
                            "a strut with its points: the sim's elastic-only load transfer")
    # Camber thrust: AC's CAMBER_GAIN is the same ratio (side force from
    # lean over the cornering stiffness, ~0.1 on a racing tyre).
    if camber_gain_tyre:
        out.put(s, "camber_thrust", round(min(max(sum(camber_gain_tyre) / len(camber_gain_tyre), 0.0), 0.5), 4),
                "tyres.ini CAMBER_GAIN, axles averaged")

    out.fit["reference_speed_mps"] = REFERENCE_SPEED_MPS
    out.fit["cog_height_m"] = round(cog, 4)
    out.car_class = car_class or map_class(car, out.has_drs)
    # Tyre blankets are a rule, not a figure AC carries: a modern F1 car
    # goes out on them as the shipped ones do (server tyre_thermal.rs).
    modern = car.ui.year >= 1990 if car.ui.year else out.has_drs
    if out.car_class == "F1" and modern:
        out.put("tires", "blanket_temperature_c", 70.0, "F1 rules: tyre blankets (AC has none)")
    # Brakes: the server takes the class's material unless the car.toml names
    # one. brakes.ini names none, but its pad curves say what the pads are
    # like. A curve that is a carbon pad's turns a class that races on steel
    # (a hybrid Formula car, a carbon-ceramic road car) to carbon; the classes
    # that race on carbon stay on it whatever the curve (an AC prototype's
    # curve is often a steel one).
    pads = pad_material(car)
    if pads:
        verdict, errors = pads
        out.fit["brake_pads"] = {"resembles": verdict, "mean_squared_difference": errors}
        if verdict == "carbon" and out.car_class not in CARBON_CLASSES:
            out.put("brakes", "material", "carbon",
                    "brakes.ini PERF_CURVE: the pads' friction against temperature is a carbon pad's")
    return out
