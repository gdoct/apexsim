//! Tyre temperature: two thermal masses per tyre, and what their heat does
//! to the grip and the pressure.
//!
//! Each tyre is a **tread** (the rubber on the road: light, heated by the
//! power the contact patch dissipates sliding, cooled by the air and by the
//! road it rolls onto) and a **core** (carcass, gas and some of the wheel:
//! heavy, heated by the tyre flexing as it rolls and by the tread above it,
//! cooled by the air inside the wheel arch). The tread answers a slide in a
//! corner or two and cools down a straight; the core takes laps, and the
//! pressure follows it.
//!
//! - **Grip** reads a blend of the two ([`TREAD_GRIP_SHARE`]): full inside
//!   `optimal_temperature_c ± temperature_window_c`, falling off by
//!   `temperature_grip_falloff` per degree outside it, eased in over the
//!   first degrees so the edge of the window is not a step the car can
//!   feel. Cold is as bad as hot per degree, and much further away.
//! - **Pressure** is the gas law on the core: the setup's pressures are
//!   what the tyre runs at *at its optimum*, so a cold tyre is soft and an
//!   overheated one is over-inflated, and the pressure grip curve
//!   (`TireConfig::pressure_grip_factor`) charges for both. A tyre in its
//!   window at the set pressure grips exactly what it did before
//!   temperatures existed.
//!
//! Heat in, per tyre: the slip power, `|Fx| · slip speed along + |Fy| ·
//! slip speed across`, is what the tyre dissipates to make its force.
//! Up to the tyre's peak slip it is the rubber's hysteresis as the patch
//! is sheared and let go every revolution, and it heats the rubber right
//! through: [`TREAD_HYSTERESIS_SHARE`] of it the tread layer, by mass,
//! the rest the bulk (the core here). Past the peak it is the rubber
//! sliding over the road, and friction at an interface heats both sides
//! by their thermal effusivities: only [`FRICTION_HEAT_TO_TYRE`] of that
//! goes into the tread, the rest into a road that is fresh under every
//! pass. [`CARCASS_HEAT_SHARE`] of `rolling_resistance · load · speed`
//! (the carcass flexing) heats the core too. Heat out: convection to the
//! air, growing with speed to the 0.8 as a turbulent boundary layer does,
//! and conduction into the road through the patch, growing with the root
//! of speed as fresh road keeps arriving under it; a wet road takes
//! several times as much.
//!
//! The first version put 85% of the sub-peak slip power into the tread.
//! That is the whole of the power into a kilogram and a half of rubber,
//! and a careful driver felt it as "every slight turn is 10 °C": an LMP2
//! at 2° of slip at 70 m/s dissipates 22 kW per front tyre, which is 9 °C
//! a second in that layer, and Eau Rouge took its fronts from 85 to 150 °C
//! while the AI, smooth and at the peak, saw 109 on the same lap. The
//! tyre keeps the same energy now; it lands in the bulk, and the surface
//! follows it over a lap instead of leading it through every corner.
//!
//! Everything is a pure function of the tick's inputs (no clock, no RNG),
//! so the sim stays deterministic.

use crate::data::{CarState, TireConfig, TireData, TrackSurface};

/// Where a compound grips best, °C, for a car that does not say.
pub const DEFAULT_OPTIMAL_C: f32 = 90.0;
/// Half-width of the flat top of the grip curve, °C.
pub const DEFAULT_WINDOW_C: f32 = 10.0;
/// Grip lost per degree outside the window.
pub const DEFAULT_GRIP_FALLOFF: f32 = 0.004;

/// The falloff reaches its full slope over about this many degrees past
/// the window's edge (`off² / (off + EASE)`).
const FALLOFF_EASE_C: f32 = 4.0;
/// However cold or hot, a tyre keeps this much of its grip from temperature
/// alone (the pressure can take more).
const MAX_THERMAL_GRIP_LOSS: f32 = 0.25;
/// A cold tyre is charged this share of the falloff per degree: it is also
/// under-inflated, and the pressure curve charges for that.
const COLD_FALLOFF_SHARE: f32 = 0.6;
/// How much of the temperature the grip reads is the tread's; the rest is
/// the core's, which is what keeps a surface flash from being a grip spike.
pub const TREAD_GRIP_SHARE: f32 = 0.7;

const ATMOSPHERE_KPA: f32 = 101.325;
const KELVIN: f32 = 273.15;

/// Heat capacities, J/K: a tread of about a kilogram and a half of rubber
/// on a carcass, gas and hub of ten times that.
const TREAD_HEAT_CAPACITY: f32 = 2_000.0;
const CORE_HEAT_CAPACITY: f32 = 7_000.0;
/// Conduction between tread and core, W/K (a millimetre or two of rubber
/// under the surface).
const TREAD_CORE_CONDUCTANCE: f32 = 350.0;
/// Convection from the tread to the air: still air, and the forced part
/// per (m/s)^0.8, W/K.
const TREAD_AIR_STILL: f32 = 5.0;
const TREAD_AIR_FORCED: f32 = 1.2;
/// The core sheds heat to the air inside the wheel at this share of the
/// tread's rate.
const CORE_AIR_SHARE: f32 = 0.8;
/// Conduction into the road per root of speed (m/s), W/K, and how many
/// times that a wet road takes.
const TREAD_ROAD_CONDUCTANCE: f32 = 3.5;
const WET_ROAD_FACTOR: f32 = 4.0;
/// Share of the slip power below the peak slip (the rubber's hysteresis,
/// spread through it) that the tread layer takes, by mass: the surface
/// layer and the tread blocks under it, which flex the most. The bulk
/// takes the rest. Handing part of that power to the road instead was
/// tried (a quarter of it in the tyre: the LMP2's fronts ran at 66 °C at
/// Monza against a window of 82-102; 0.55: 72; 0.9: 78): the AI at its
/// peak slip sets every class's window, and the tyre has to keep the
/// energy it kept before. Only where it lands changed.
const TREAD_HYSTERESIS_SHARE: f32 = 0.3;
/// Share of the friction power at the patch (the rubber sliding over the
/// road: everything past the peak slip) that goes into the tyre at all.
/// Friction heats the interface, and
/// the heat splits between rubber and asphalt by their thermal
/// effusivities (about 0.7 and 1.5 kJ/m²K√s): roughly a third to the tyre,
/// the rest into a road that is fresh under every pass. With all of it in
/// the tread, a few seconds of understeer took a GT3's fronts from 85 to
/// 160 °C, the lost grip made it slide more, and a locked wheel cooked its
/// tread to 370 °C in six seconds.
const FRICTION_HEAT_TO_TYRE: f32 = 0.3;
/// Share of the rolling-resistance power that stays in the carcass as
/// heat; the rest goes into the tread and the road at the patch.
const CARCASS_HEAT_SHARE: f32 = 0.8;

/// A tyre compound, as a change to the car's own tyre: its grip, how fast
/// it wears and where its working window sits. The medium is the car's
/// tyre exactly, so everything calibrated on it holds.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Compound {
    pub name: &'static str,
    /// Multiplier on the tyre's grip.
    pub grip_scale: f32,
    /// Multiplier on its wear.
    pub wear_scale: f32,
    /// Shift of its working window, °C (a soft works cooler).
    pub optimal_shift_c: f32,
    /// What it keeps of the road's grip on a dry road, in light rain and in
    /// heavy rain (`TrackSurface::water` 0, 0.5, 1; linear between). The
    /// road's own grip is baked for the tyre the weather calls for, so
    /// that tyre reads 1.0 there: a slick in heavy rain aquaplanes, a wet
    /// on a dry road is soft, treaded rubber that squirms.
    pub water_grip: [f32; 3],
}

/// The compounds every car has, softest slick first, then the intermediate
/// and the full wet: `CarState::tyre_compound` and the setup's
/// `tyre_compound` knob index them.
pub const COMPOUNDS: [Compound; 5] = [
    Compound {
        name: "soft",
        grip_scale: 1.03,
        wear_scale: 1.8,
        optimal_shift_c: -6.0,
        water_grip: SLICK_WATER_GRIP,
    },
    Compound {
        name: "medium",
        grip_scale: 1.0,
        wear_scale: 1.0,
        optimal_shift_c: 0.0,
        water_grip: SLICK_WATER_GRIP,
    },
    Compound {
        name: "hard",
        grip_scale: 0.97,
        wear_scale: 0.55,
        optimal_shift_c: 6.0,
        water_grip: SLICK_WATER_GRIP,
    },
    Compound {
        name: "intermediate",
        grip_scale: 1.0,
        wear_scale: 1.5,
        optimal_shift_c: -25.0,
        water_grip: [0.88, 1.0, 0.88],
    },
    Compound {
        name: "wet",
        grip_scale: 1.0,
        wear_scale: 2.0,
        optimal_shift_c: -35.0,
        water_grip: [0.78, 0.95, 1.0],
    },
];
/// A slick on a wet road: damp is a few seconds a lap, standing water is
/// aquaplaning.
const SLICK_WATER_GRIP: [f32; 3] = [1.0, 0.85, 0.6];
/// The medium: what a car is fitted with unless someone chose otherwise.
pub const MEDIUM: u8 = 1;
pub const INTERMEDIATE: u8 = 3;
pub const WET: u8 = 4;

/// The compound at `index`, the medium for an index out of range.
pub fn compound(index: u8) -> &'static Compound {
    COMPOUNDS
        .get(index as usize)
        .unwrap_or(&COMPOUNDS[MEDIUM as usize])
}

/// The tyre the weather calls for on a road with this much `water`: the
/// intermediate in light rain, the full wet past it; `None` when dry.
pub fn weather_compound(water: f32) -> Option<u8> {
    if water <= 0.0 {
        None
    } else if water < 0.75 {
        Some(INTERMEDIATE)
    } else {
        Some(WET)
    }
}

impl Compound {
    /// Its grip on a road with this much water (0 dry .. 1 heavy rain), as
    /// a multiplier on the car's tyre.
    pub fn grip_on(&self, water: f32) -> f32 {
        let w = water.clamp(0.0, 1.0) * 2.0;
        let [dry, light, heavy] = self.water_grip;
        let wet = if w <= 1.0 {
            dry + (light - dry) * w
        } else {
            light + (heavy - light) * (w - 1.0)
        };
        self.grip_scale * wet
    }
}

/// Wear, percent of the tread, per megajoule the patch dissipates sliding
/// (the same friction power that heats it), on a medium at its window.
pub const WEAR_PERCENT_PER_MJ: f32 = 7.0;
/// Each degree the tread runs over its window wears it this much faster.
const HOT_WEAR_PER_C: f32 = 0.04;
/// Grip a tyre loses as it wears: this share across its life, and on top
/// of it a cliff of [`WEAR_CLIFF_LOSS`] from [`WEAR_CLIFF_START`] to worn
/// through (quadratic, so it comes on slowly and then all at once).
const WEAR_LINEAR_LOSS: f32 = 0.06;
const WEAR_CLIFF_START: f32 = 0.7;
const WEAR_CLIFF_LOSS: f32 = 0.24;

/// A wheel turning this much slower than the road (slip ratio, braking) is
/// locked: one patch of its tread scrubs the road and the rest rests.
pub const LOCK_SLIP_RATIO: f32 = 0.9;
/// Flat spot depth (0..1) per megajoule a locked wheel's patch dissipates,
/// on a medium: a second locked from 50 m/s on a loaded front (some
/// 0.2 MJ) leaves a third of the way to the worst.
const FLAT_SPOT_PER_MJ: f32 = 1.5;
/// Below this the wheel is crawling, m/s: nothing to grind.
const FLAT_SPOT_MIN_SPEED_MPS: f32 = 3.0;
/// Grip the deepest flat spot costs: the patch is not round any more and
/// rides off the road once a turn.
const FLAT_SPOT_GRIP_LOSS: f32 = 0.04;
/// What a punctured tyre keeps of its grip, on its carcass.
pub const PUNCTURE_GRIP: f32 = 0.35;
/// The pressure a punctured tyre reads, kPa.
pub const PUNCTURED_KPA: f32 = 15.0;
/// Rolling resistance of a punctured tyre, as a share of its load: a flat
/// tyre's carcass and rim drag on the road.
pub const PUNCTURE_ROLLING_RESISTANCE: f32 = 0.08;

/// Grip left in a tyre `wear_percent` worn.
pub fn wear_grip_factor(wear_percent: f32) -> f32 {
    let w = (wear_percent / 100.0).clamp(0.0, 1.0);
    let cliff = ((w - WEAR_CLIFF_START) / (1.0 - WEAR_CLIFF_START)).max(0.0);
    1.0 - WEAR_LINEAR_LOSS * w - WEAR_CLIFF_LOSS * cliff * cliff
}

/// What one tyre did this tick, as the heat model needs it.
#[derive(Debug, Clone, Copy, Default)]
pub struct TyreWork {
    /// Longitudinal and lateral force at the patch, N.
    pub fx: f32,
    pub fy: f32,
    pub slip_ratio: f32,
    pub slip_angle_rad: f32,
    /// Vertical load, N.
    pub load_n: f32,
    /// Speed of the wheel along the road, m/s.
    pub speed_mps: f32,
    /// Speed of the air over the tyre, m/s: the airflow through the wind
    /// and the wake of a car ahead, which is what cools it (the road under
    /// it arrives at `speed_mps`).
    pub air_mps: f32,
}

impl TyreWork {
    /// Power the contact patch dissipates, W: each force times the speed
    /// the rubber slides at in its direction.
    pub fn slide_power_w(&self) -> f32 {
        let v = self.speed_mps.abs();
        self.fx.abs() * self.slip_ratio.abs().min(1.0) * v
            + self.fy.abs() * self.slip_angle_rad.sin().abs() * v
    }

    /// The patch's friction power split at the tyre's peak slip, W: the
    /// part up to it (the rubber gripping and deforming) and the part past
    /// it (the rubber sliding). They add up to [`Self::slide_power_w`].
    pub fn power_split_w(&self, tire: &TireConfig) -> (f32, f32) {
        let v = self.speed_mps.abs();
        let along = self.slip_ratio.abs().min(1.0);
        let across = self.slip_angle_rad.sin().abs();
        let along_peak = along.min(tire.optimal_slip_ratio.max(0.0));
        let across_peak = across.min(tire.optimal_slip_angle_rad.max(0.0).sin());
        let fx = self.fx.abs() * v;
        let fy = self.fy.abs() * v;
        let gripping = fx * along_peak + fy * across_peak;
        let sliding = fx * (along - along_peak) + fy * (across - across_peak);
        (gripping, sliding)
    }
}

/// Grip the temperature leaves a tyre whose tread and core are at these,
/// as a share of its peak (1.0 inside the window).
pub fn thermal_grip_factor(
    tire: &TireConfig,
    compound: &Compound,
    tread_c: f32,
    core_c: f32,
) -> f32 {
    let t = TREAD_GRIP_SHARE * tread_c + (1.0 - TREAD_GRIP_SHARE) * core_c;
    let from_optimum = t - optimum_c(tire, compound);
    let off = from_optimum.abs() - tire.temperature_window_c.max(0.0);
    if off <= 0.0 {
        return 1.0;
    }
    let eased = off * off / (off + FALLOFF_EASE_C);
    let falloff = tire.temperature_grip_falloff.max(0.0)
        * if from_optimum < 0.0 {
            COLD_FALLOFF_SHARE
        } else {
            1.0
        };
    (1.0 - falloff * eased).max(1.0 - MAX_THERMAL_GRIP_LOSS)
}

/// The middle of a compound's window on this car's tyre, °C.
pub fn optimum_c(tire: &TireConfig, compound: &Compound) -> f32 {
    tire.optimal_temperature_c + compound.optimal_shift_c
}

/// Gauge pressure, kPa, of a tyre set to `set_kpa` at its optimum whose gas
/// is at `core_c`: the gas law at constant volume, on absolute pressures.
pub fn pressure_kpa(tire: &TireConfig, compound: &Compound, set_kpa: f32, core_c: f32) -> f32 {
    let optimum = optimum_c(tire, compound);
    if core_c == optimum {
        return set_kpa;
    }
    let ratio = (core_c + KELVIN).max(1.0) / (optimum + KELVIN).max(1.0);
    (set_kpa + ATMOSPHERE_KPA) * ratio - ATMOSPHERE_KPA
}

/// The multiplier a tyre's grip takes from its compound, its wear, its
/// temperature and the pressure that puts it at. A new medium at its
/// optimum with the set pressure on it is the set pressure's own factor,
/// exactly.
pub fn grip_multiplier(
    tire: &TireConfig,
    compound: &Compound,
    set_kpa: f32,
    tyre: &TireData,
    water: f32,
) -> f32 {
    let optimum = optimum_c(tire, compound);
    let used = compound.grip_on(water)
        * wear_grip_factor(tyre.wear_percent)
        * (1.0 - FLAT_SPOT_GRIP_LOSS * tyre.flat_spot.clamp(0.0, 1.0));
    if tyre.punctured {
        return PUNCTURE_GRIP * tire.pressure_grip_factor(set_kpa) * used;
    }
    if tyre.core_temperature_c == optimum && tyre.temperature_c == optimum {
        return tire.pressure_grip_factor(set_kpa) * used;
    }
    thermal_grip_factor(tire, compound, tyre.temperature_c, tyre.core_temperature_c)
        * tire.pressure_grip_factor(pressure_kpa(
            tire,
            compound,
            set_kpa,
            tyre.core_temperature_c,
        ))
        * used
}

/// The set pressure of each tyre, FL FR RL RR.
fn set_pressures(tire: &TireConfig) -> [f32; 4] {
    [
        tire.pressure_front_kpa,
        tire.pressure_front_kpa,
        tire.pressure_rear_kpa,
        tire.pressure_rear_kpa,
    ]
}

/// Refresh the pressure and the grip share a tyre reports from its
/// temperatures and wear. The grip share is against a new medium in its
/// window at its set pressure, so 1.0 is "as good as the setup makes it"
/// (a fresh soft is a little over).
fn refresh(tyre: &mut TireData, tire: &TireConfig, compound: &Compound, set_kpa: f32, water: f32) {
    tyre.pressure_kpa = if tyre.punctured {
        PUNCTURED_KPA
    } else {
        pressure_kpa(tire, compound, set_kpa, tyre.core_temperature_c)
    };
    let best = tire.pressure_grip_factor(set_kpa).max(1e-3);
    tyre.grip_factor = grip_multiplier(tire, compound, set_kpa, tyre, water) / best;
}

/// Put a new set of `compound` (a [`COMPOUNDS`] index) on a car, at
/// `temperature_c` right through, as it is sent out on them, onto a road
/// with this much `water`.
pub fn fit(
    state: &mut CarState,
    tire: &TireConfig,
    temperature_c: f32,
    compound_index: u8,
    water: f32,
) {
    let index = if (compound_index as usize) < COMPOUNDS.len() {
        compound_index
    } else {
        MEDIUM
    };
    let c = compound(index);
    for (tyre, set) in state.tires.each_mut().into_iter().zip(set_pressures(tire)) {
        tyre.temperature_c = temperature_c;
        tyre.core_temperature_c = temperature_c;
        tyre.wear_percent = 0.0;
        tyre.flat_spot = 0.0;
        tyre.punctured = false;
        refresh(tyre, tire, c, set, water);
    }
    state.tyre_compound = index;
    state.tyres_fitted = true;
}

/// What a car's tyres are at when it is sent out: its blankets, or the air
/// when it has none (a blanket is never colder than the air).
pub fn start_temperature_c(tire: &TireConfig, surface: &TrackSurface) -> f32 {
    let air = surface.air_temperature_c;
    tire.blanket_temperature_c.map_or(air, |b| b.max(air))
}

/// Share of the way from [`start_temperature_c`] to the optimum a race
/// grid's tyres are at. There is no formation lap to drive, so the grid is
/// given the warmth one puts in: a car without blankets starts the race on
/// tyres that are cool rather than stone cold.
pub const FORMATION_LAP_WARMTH: f32 = 0.5;

/// What a race grid's tyres are at when the lights go out.
pub fn grid_temperature_c(tire: &TireConfig, surface: &TrackSurface) -> f32 {
    let start = start_temperature_c(tire, surface);
    let optimum = tire.optimal_temperature_c;
    // (The medium's optimum: a grid warms the tyres a formation lap's worth
    // whatever they are.)
    if start >= optimum {
        start
    } else {
        start + FORMATION_LAP_WARMTH * (optimum - start)
    }
}

/// Advance one tyre by `dt`: its heat, and the tread its sliding wears
/// away (faster the hotter it runs over its window).
pub fn step(
    tyre: &mut TireData,
    work: &TyreWork,
    tire: &TireConfig,
    compound: &Compound,
    set_kpa: f32,
    surface: &TrackSurface,
    dt: f32,
) {
    let v = work.speed_mps.abs();
    let (gripping, sliding) = work.power_split_w(tire);
    let slide = gripping + sliding;
    let rolling = tire.rolling_resistance.max(0.0) * work.load_n.max(0.0) * v * CARCASS_HEAT_SHARE;

    let air = surface.air_temperature_c;
    let road = surface.track_temperature_c;
    let tread = tyre.temperature_c;
    let core = tyre.core_temperature_c;

    let to_air = TREAD_AIR_STILL + TREAD_AIR_FORCED * work.air_mps.abs().powf(0.8);
    let wet = if surface.wet { WET_ROAD_FACTOR } else { 1.0 };
    // The patch only touches the road with a load on it.
    let to_road = if work.load_n > 1.0 {
        TREAD_ROAD_CONDUCTANCE * wet * v.sqrt()
    } else {
        0.0
    };
    let across = TREAD_CORE_CONDUCTANCE * (tread - core);

    // Sliding friction heats the tread's surface (the tyre's share of it);
    // the rubber's hysteresis heats the rubber by mass, and the carcass's
    // flexing the bulk.
    let tread_in = FRICTION_HEAT_TO_TYRE * sliding + TREAD_HYSTERESIS_SHARE * gripping
        - to_air * (tread - air)
        - to_road * (tread - road);
    let core_in = (1.0 - TREAD_HYSTERESIS_SHARE) * gripping + rolling
        - CORE_AIR_SHARE * to_air * (core - air);

    tyre.temperature_c = tread + (tread_in - across) * dt / TREAD_HEAT_CAPACITY;
    tyre.core_temperature_c = core + (core_in + across) * dt / CORE_HEAT_CAPACITY;

    let over = (tread - optimum_c(tire, compound) - tire.temperature_window_c).max(0.0);
    let wear = WEAR_PERCENT_PER_MJ
        * slide
        * dt
        * 1e-6
        * compound.wear_scale
        * tire.wear_rate.max(0.0)
        * (1.0 + HOT_WEAR_PER_C * over);
    tyre.wear_percent = (tyre.wear_percent + wear).min(100.0);
    // Worn through, it lets go of its air.
    if tyre.wear_percent >= 100.0 {
        tyre.punctured = true;
    }
    // A locked wheel grinds one patch of its tread flat (softer rubber
    // faster).
    if work.slip_ratio <= -LOCK_SLIP_RATIO && v > FLAT_SPOT_MIN_SPEED_MPS {
        let grind = work.fx.abs() * work.slip_ratio.abs().min(1.0) * v;
        tyre.flat_spot =
            (tyre.flat_spot + FLAT_SPOT_PER_MJ * grind * dt * 1e-6 * compound.wear_scale).min(1.0);
    }
    refresh(tyre, tire, compound, set_kpa, surface.water);
}

/// Advance all four tyres, FL FR RL RR.
pub fn step_all(
    state: &mut CarState,
    work: &[TyreWork; 4],
    tire: &TireConfig,
    surface: &TrackSurface,
    dt: f32,
) {
    let c = compound(state.tyre_compound);
    for ((tyre, w), set) in state
        .tires
        .each_mut()
        .into_iter()
        .zip(work)
        .zip(set_pressures(tire))
    {
        step(tyre, w, tire, c, set, surface, dt);
    }
}

impl CarState {
    /// How much of its grip the car's weaker axle has from its tyres'
    /// compound, wear, temperatures and pressures (1.0 a new medium in the
    /// window at the set pressure):
    /// what a driver who can feel the tyres drives to. 1.0 before the
    /// tyres are fitted.
    pub fn tyre_grip_share(&self) -> f32 {
        if !self.tyres_fitted {
            return 1.0;
        }
        let t = &self.tires;
        let front = 0.5 * (t.front_left.grip_factor + t.front_right.grip_factor);
        let rear = 0.5 * (t.rear_left.grip_factor + t.rear_right.grip_factor);
        // A fresh soft is a little over the plan's tyre, and a driver uses it.
        front.min(rear).clamp(0.0, COMPOUNDS[0].grip_scale)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const M: Compound = COMPOUNDS[MEDIUM as usize];

    fn tyre() -> TireConfig {
        TireConfig::default()
    }

    #[test]
    fn the_window_is_flat_and_the_edges_ease_in() {
        let t = tyre();
        let opt = t.optimal_temperature_c;
        for d in [-10.0, -5.0, 0.0, 5.0, 10.0] {
            assert_eq!(thermal_grip_factor(&t, &M, opt + d, opt + d), 1.0);
        }
        // Just past the edge: almost nothing, then the full slope.
        let edge = 1.0 - thermal_grip_factor(&t, &M, opt + 11.0, opt + 11.0);
        assert!(
            edge > 0.0 && edge < t.temperature_grip_falloff * 0.3,
            "{edge}"
        );
        let far = |d: f32| thermal_grip_factor(&t, &M, opt + d, opt + d);
        let slope = far(40.0) - far(41.0);
        assert!((slope - t.temperature_grip_falloff).abs() < 2e-4, "{slope}");
        // Cold is charged less per degree: the pressure charges the rest.
        let cold = 1.0 - far(-30.0);
        let hot = 1.0 - far(30.0);
        assert!(
            (cold - COLD_FALLOFF_SHARE * hot).abs() < 1e-5,
            "{cold} {hot}"
        );
        // And never below the floor.
        assert_eq!(far(-400.0), 1.0 - MAX_THERMAL_GRIP_LOSS);
    }

    #[test]
    fn the_tread_counts_more_than_the_core() {
        let t = tyre();
        let opt = t.optimal_temperature_c;
        // 30 °C over is 9 over when it is the core, 21 when it is the tread.
        let hot_tread = thermal_grip_factor(&t, &M, opt + 30.0, opt);
        let hot_core = thermal_grip_factor(&t, &M, opt, opt + 30.0);
        assert!(hot_tread < hot_core && hot_core == 1.0);
    }

    #[test]
    fn the_set_pressure_is_the_pressure_at_the_optimum() {
        let t = tyre();
        let opt = t.optimal_temperature_c;
        assert!((pressure_kpa(&t, &M, 175.0, opt) - 175.0).abs() < 1e-3);
        // A slick set to 180 hot is at about 125 kPa at 20 °C, as a real
        // tyre that gains half a bar on the way up to temperature.
        let cold = pressure_kpa(&t, &M, 180.0, 20.0);
        assert!((120.0..132.0).contains(&cold), "{cold}");
        assert!(pressure_kpa(&t, &M, 180.0, opt + 20.0) > 195.0);
    }

    #[test]
    fn a_tyre_at_its_optimum_grips_as_it_did_before_temperatures() {
        let t = tyre();
        let mut data = TireData {
            temperature_c: t.optimal_temperature_c,
            core_temperature_c: t.optimal_temperature_c,
            ..Default::default()
        };
        assert_eq!(grip_multiplier(&t, &M, 180.0, &data, 0.0), 1.0);
        assert_eq!(
            grip_multiplier(&t, &M, 190.0, &data, 0.0),
            t.pressure_grip_factor(190.0)
        );
        refresh(&mut data, &t, &M, 190.0, 0.0);
        assert_eq!(data.grip_factor, 1.0, "as good as the setup makes it");
        assert_eq!(data.pressure_kpa, 190.0);
    }

    #[test]
    fn a_slide_past_the_peak_puts_most_of_its_heat_into_the_road() {
        let t = tyre();
        let surface = TrackSurface::default();
        let heat = |work: &TyreWork| {
            let mut data = TireData {
                temperature_c: 85.0,
                core_temperature_c: 70.0,
                ..Default::default()
            };
            for _ in 0..(5 * 240) {
                step(&mut data, work, &t, &M, 180.0, &surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        // The split adds up to the whole.
        let understeer = TyreWork {
            fy: 4000.0,
            slip_angle_rad: 0.5,
            load_n: 3300.0,
            speed_mps: 25.0,
            air_mps: 25.0,
            ..Default::default()
        };
        let (gripping, sliding) = understeer.power_split_w(&t);
        assert!((gripping + sliding - understeer.slide_power_w()).abs() < 1.0);
        assert!(sliding > 2.0 * gripping);
        // Five seconds of heavy understeer warms the fronts, not cooks them.
        let hot = heat(&understeer);
        assert!((95.0..135.0).contains(&hot), "understeer: {hot:.1}");
        // Nor does five seconds of a locked wheel at 40 m/s.
        let locked = TyreWork {
            fx: 4000.0,
            slip_ratio: -1.0,
            load_n: 3300.0,
            speed_mps: 40.0,
            air_mps: 40.0,
            ..Default::default()
        };
        let hot = heat(&locked);
        assert!(hot < 220.0, "locked: {hot:.1}");
        // At the peak nothing changes: all of it is gripping power.
        let at_peak = TyreWork {
            fy: 4000.0,
            slip_angle_rad: t.optimal_slip_angle_rad,
            load_n: 3300.0,
            speed_mps: 25.0,
            air_mps: 25.0,
            ..Default::default()
        };
        assert!(at_peak.power_split_w(&t).1.abs() < 1e-3);
    }

    #[test]
    fn sliding_heats_the_tread_and_a_straight_cools_it() {
        let t = tyre();
        let surface = TrackSurface::default();
        let mut data = TireData {
            temperature_c: 80.0,
            core_temperature_c: 80.0,
            ..Default::default()
        };
        // Twice the peak slip angle: a proper slide.
        let slide = TyreWork {
            fy: 6_000.0,
            slip_angle_rad: 0.25,
            load_n: 5_000.0,
            speed_mps: 40.0,
            air_mps: 40.0,
            ..Default::default()
        };
        for _ in 0..(240 * 2) {
            step(&mut data, &slide, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(
            data.temperature_c > data.core_temperature_c + 2.0,
            "the tread runs ahead: {data:?}"
        );
        for _ in 0..(240 * 4) {
            step(&mut data, &slide, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c > 95.0, "{data:?}");
        let peak = data.temperature_c;
        let straight = TyreWork {
            load_n: 4_000.0,
            speed_mps: 80.0,
            air_mps: 80.0,
            ..Default::default()
        };
        for _ in 0..(240 * 5) {
            step(&mut data, &straight, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c < peak - 8.0, "{peak} -> {data:?}");
    }

    /// The report that found the old split: an LMP2 driven carefully on a
    /// pad, 2° of slip on a loaded front at 70 m/s (a third of the peak;
    /// 22 kW of slip power), gained 10 °C per slight turn and 65 through
    /// Eau Rouge. A fast corner now warms the surface by a few degrees,
    /// and a lap of them does not run away.
    #[test]
    fn a_careful_corner_warms_the_tread_a_few_degrees() {
        let t = tyre();
        let surface = TrackSurface::default();
        let mut data = TireData {
            temperature_c: 92.0,
            core_temperature_c: 92.0,
            ..Default::default()
        };
        let corner = TyreWork {
            fy: 9_000.0,
            slip_angle_rad: 2.0f32.to_radians(),
            load_n: 6_500.0,
            speed_mps: 70.0,
            air_mps: 70.0,
            ..Default::default()
        };
        assert!((corner.slide_power_w() - 22_000.0).abs() < 500.0);
        for _ in 0..(240 * 4) {
            step(&mut data, &corner, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        let rise = data.temperature_c - 92.0;
        assert!((1.0..12.0).contains(&rise), "four seconds of it: {data:?}");
        // The bulk takes most of the energy, and shows it slowly.
        let core_rise = data.core_temperature_c - 92.0;
        assert!((2.0..12.0).contains(&core_rise), "{data:?}");
        assert!(
            rise < 2.0 * core_rise,
            "the surface leads a little: {data:?}"
        );
        // The straight after it takes the surface back toward the bulk.
        let peak = data.temperature_c;
        let straight = TyreWork {
            load_n: 5_000.0,
            speed_mps: 75.0,
            air_mps: 75.0,
            ..Default::default()
        };
        for _ in 0..(240 * 5) {
            step(&mut data, &straight, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c < peak - 2.0, "{peak} -> {data:?}");
        // (A lap of such corners is pinned on the AI at Monza and Spa in
        // `tests/tyre_temperature_test.rs`.)
    }

    #[test]
    fn a_parked_tyre_settles_toward_the_air_and_road() {
        let t = tyre();
        let surface = TrackSurface::default();
        let mut data = TireData {
            temperature_c: 100.0,
            core_temperature_c: 100.0,
            ..Default::default()
        };
        let idle = TyreWork::default();
        let mut last = data.temperature_c;
        for _ in 0..30 {
            for _ in 0..(240 * 60) {
                step(&mut data, &idle, &t, &M, 180.0, &surface, 1.0 / 240.0);
            }
            assert!(data.temperature_c < last, "cooling every minute: {data:?}");
            last = data.temperature_c;
        }
        // Half an hour in still air: most of the way down, the core behind.
        let air = surface.air_temperature_c;
        assert!(data.temperature_c - air < 0.2 * (100.0 - air), "{data:?}");
        assert!(data.core_temperature_c > data.temperature_c);
    }

    #[test]
    fn a_wet_road_takes_more_heat() {
        let t = tyre();
        let dry = TrackSurface::default();
        let wet = TrackSurface {
            wet: true,
            ..TrackSurface::default()
        };
        let roll = TyreWork {
            load_n: 4_000.0,
            speed_mps: 50.0,
            air_mps: 50.0,
            ..Default::default()
        };
        let run = |surface: &TrackSurface| {
            let mut data = TireData {
                temperature_c: 90.0,
                core_temperature_c: 90.0,
                ..Default::default()
            };
            for _ in 0..(240 * 10) {
                step(&mut data, &roll, &t, &M, 180.0, surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        assert!(run(&wet) < run(&dry) - 5.0);
    }

    #[test]
    fn blankets_send_a_car_out_warm() {
        let surface = TrackSurface::default();
        let mut t = tyre();
        assert_eq!(start_temperature_c(&t, &surface), surface.air_temperature_c);
        t.blanket_temperature_c = Some(70.0);
        assert_eq!(start_temperature_c(&t, &surface), 70.0);
        // The grid is half way from there to the optimum.
        assert_eq!(
            grid_temperature_c(&t, &surface),
            70.0 + FORMATION_LAP_WARMTH * (t.optimal_temperature_c - 70.0)
        );
    }

    #[test]
    fn slower_air_cools_less_so_a_tow_or_a_tailwind_runs_hotter() {
        let t = tyre();
        let surface = TrackSurface::default();
        let run = |air_mps: f32| {
            let mut data = TireData {
                temperature_c: 95.0,
                core_temperature_c: 95.0,
                ..Default::default()
            };
            let roll = TyreWork {
                fy: 3_000.0,
                slip_angle_rad: 0.03,
                load_n: 4_000.0,
                speed_mps: 60.0,
                air_mps,
                ..Default::default()
            };
            for _ in 0..(240 * 30) {
                step(&mut data, &roll, &t, &M, 180.0, &surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        // Clean air at the car's speed, then 70% of it (a close tow).
        let clean = run(60.0);
        let towed = run(42.0);
        assert!(towed > clean + 1.0, "{clean:.1} -> {towed:.1}");
    }

    #[test]
    fn a_locked_wheel_grinds_a_flat_spot_and_a_worn_tyre_lets_go() {
        let t = tyre();
        let surface = TrackSurface::default();
        let opt = t.optimal_temperature_c;
        let mut data = TireData {
            temperature_c: opt,
            core_temperature_c: opt,
            ..Default::default()
        };
        refresh(&mut data, &t, &M, 180.0, 0.0);
        let fresh = data.grip_factor;
        // Braking hard at the peak: no flat spot.
        let braking = TyreWork {
            fx: -6_000.0,
            slip_ratio: -t.optimal_slip_ratio,
            load_n: 6_000.0,
            speed_mps: 50.0,
            air_mps: 50.0,
            ..Default::default()
        };
        for _ in 0..240 {
            step(&mut data, &braking, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert_eq!(data.flat_spot, 0.0);
        // A second locked from 50 m/s.
        let locked = TyreWork {
            slip_ratio: -1.0,
            ..braking
        };
        for _ in 0..240 {
            step(&mut data, &locked, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(
            (0.2..0.7).contains(&data.flat_spot),
            "a second's lock: {}",
            data.flat_spot
        );
        let spotted = TireData {
            temperature_c: opt,
            core_temperature_c: opt,
            flat_spot: 1.0,
            ..Default::default()
        };
        let loss = 1.0 - grip_multiplier(&t, &M, 180.0, &spotted, 0.0) / fresh;
        assert!((loss - FLAT_SPOT_GRIP_LOSS).abs() < 1e-4, "{loss}");

        // Worn through, the tyre punctures: most of its grip and its air go.
        data.wear_percent = 99.999;
        for _ in 0..240 {
            step(&mut data, &braking, &t, &M, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.punctured);
        assert_eq!(data.pressure_kpa, PUNCTURED_KPA);
        assert!(data.grip_factor < 0.4, "{}", data.grip_factor);

        // A new set clears both.
        let mut car = CarState::new(
            uuid::Uuid::nil(),
            uuid::Uuid::nil(),
            &crate::data::GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        );
        car.tires.front_left = data;
        fit(&mut car, &t, opt, MEDIUM, 0.0);
        assert!(!car.tires.front_left.punctured);
        assert_eq!(car.tires.front_left.flat_spot, 0.0);
    }

    #[test]
    fn the_weathers_tyre_grips_as_the_road_was_baked_and_slicks_aquaplane() {
        let medium = compound(MEDIUM);
        let inter = compound(INTERMEDIATE);
        let wet = compound(WET);
        // Dry: the medium is the car's tyre; the treaded tyres squirm.
        assert_eq!(medium.grip_on(0.0), 1.0);
        assert!(inter.grip_on(0.0) < 0.9 && wet.grip_on(0.0) < inter.grip_on(0.0));
        // Light rain is the intermediate's, heavy the wet's: exactly what
        // the road was baked for.
        assert_eq!(inter.grip_on(0.5), 1.0);
        assert_eq!(wet.grip_on(1.0), 1.0);
        assert!(inter.grip_on(0.5) > wet.grip_on(0.5));
        assert!(inter.grip_on(0.5) > medium.grip_on(0.5));
        // A slick in standing water is little better than a sledge.
        assert!(medium.grip_on(1.0) <= 0.6);
        assert_eq!(weather_compound(0.0), None);
        assert_eq!(weather_compound(0.5), Some(INTERMEDIATE));
        assert_eq!(weather_compound(1.0), Some(WET));
    }
}
