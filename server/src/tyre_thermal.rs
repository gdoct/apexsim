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
//! Heat in, per tyre: `|Fx| · slip speed along + |Fy| · slip speed across`
//! (the friction power of the patch, [`SLIDE_HEAT_TO_TREAD`] of it into the
//! tread), and [`CARCASS_HEAT_SHARE`] of `rolling_resistance · load ·
//! speed` (the carcass's hysteresis) into the core. Heat out: convection to the air, growing with
//! speed to the 0.8 as a turbulent boundary layer does, and conduction into
//! the road through the patch, growing with the root of speed as fresh road
//! keeps arriving under it; a wet road takes several times as much.
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
/// Conduction between tread and core, W/K.
const TREAD_CORE_CONDUCTANCE: f32 = 150.0;
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
/// Share of the patch's friction power that stays in the tread; the rest
/// is the rubber's own deformation, which heats the carcass.
const SLIDE_HEAT_TO_TREAD: f32 = 0.85;
/// Share of the rolling-resistance power that stays in the carcass as
/// heat; the rest goes into the tread and the road at the patch.
const CARCASS_HEAT_SHARE: f32 = 0.8;

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
}

impl TyreWork {
    /// Power the contact patch dissipates, W: each force times the speed
    /// the rubber slides at in its direction.
    pub fn slide_power_w(&self) -> f32 {
        let v = self.speed_mps.abs();
        self.fx.abs() * self.slip_ratio.abs().min(1.0) * v
            + self.fy.abs() * self.slip_angle_rad.sin().abs() * v
    }
}

/// Grip the temperature leaves a tyre whose tread and core are at these,
/// as a share of its peak (1.0 inside the window).
pub fn thermal_grip_factor(tire: &TireConfig, tread_c: f32, core_c: f32) -> f32 {
    let t = TREAD_GRIP_SHARE * tread_c + (1.0 - TREAD_GRIP_SHARE) * core_c;
    let from_optimum = t - tire.optimal_temperature_c;
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

/// Gauge pressure, kPa, of a tyre set to `set_kpa` at its optimum whose gas
/// is at `core_c`: the gas law at constant volume, on absolute pressures.
pub fn pressure_kpa(tire: &TireConfig, set_kpa: f32, core_c: f32) -> f32 {
    if core_c == tire.optimal_temperature_c {
        return set_kpa;
    }
    let ratio = (core_c + KELVIN).max(1.0) / (tire.optimal_temperature_c + KELVIN).max(1.0);
    (set_kpa + ATMOSPHERE_KPA) * ratio - ATMOSPHERE_KPA
}

/// The multiplier a tyre's grip takes from its temperature and the
/// pressure that puts it at: thermal factor times the pressure grip curve.
/// At the optimum with the set pressure on it, this is the set pressure's
/// own factor, exactly.
pub fn grip_multiplier(tire: &TireConfig, set_kpa: f32, tyre: &TireData) -> f32 {
    if tyre.core_temperature_c == tire.optimal_temperature_c
        && tyre.temperature_c == tire.optimal_temperature_c
    {
        return tire.pressure_grip_factor(set_kpa);
    }
    thermal_grip_factor(tire, tyre.temperature_c, tyre.core_temperature_c)
        * tire.pressure_grip_factor(pressure_kpa(tire, set_kpa, tyre.core_temperature_c))
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
/// temperatures. The grip share is against the same tyre in its window at
/// its set pressure, so 1.0 is "as good as the setup makes it".
fn refresh(tyre: &mut TireData, tire: &TireConfig, set_kpa: f32) {
    tyre.pressure_kpa = pressure_kpa(tire, set_kpa, tyre.core_temperature_c);
    let best = tire.pressure_grip_factor(set_kpa).max(1e-3);
    tyre.grip_factor = grip_multiplier(tire, set_kpa, tyre) / best;
}

/// Put a set of tyres at `temperature_c` right through, as a car is sent
/// out on them.
pub fn fit(state: &mut CarState, tire: &TireConfig, temperature_c: f32) {
    for (tyre, set) in state.tires.each_mut().into_iter().zip(set_pressures(tire)) {
        tyre.temperature_c = temperature_c;
        tyre.core_temperature_c = temperature_c;
        refresh(tyre, tire, set);
    }
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
    if start >= optimum {
        start
    } else {
        start + FORMATION_LAP_WARMTH * (optimum - start)
    }
}

/// Advance one tyre by `dt`.
pub fn step(
    tyre: &mut TireData,
    work: &TyreWork,
    tire: &TireConfig,
    set_kpa: f32,
    surface: &TrackSurface,
    dt: f32,
) {
    let v = work.speed_mps.abs();
    let slide = work.slide_power_w();
    let rolling = tire.rolling_resistance.max(0.0) * work.load_n.max(0.0) * v * CARCASS_HEAT_SHARE;

    let air = surface.air_temperature_c;
    let road = surface.track_temperature_c;
    let tread = tyre.temperature_c;
    let core = tyre.core_temperature_c;

    let to_air = TREAD_AIR_STILL + TREAD_AIR_FORCED * v.powf(0.8);
    let wet = if surface.wet { WET_ROAD_FACTOR } else { 1.0 };
    // The patch only touches the road with a load on it.
    let to_road = if work.load_n > 1.0 {
        TREAD_ROAD_CONDUCTANCE * wet * v.sqrt()
    } else {
        0.0
    };
    let across = TREAD_CORE_CONDUCTANCE * (tread - core);

    let tread_in = SLIDE_HEAT_TO_TREAD * slide - to_air * (tread - air) - to_road * (tread - road);
    let core_in =
        (1.0 - SLIDE_HEAT_TO_TREAD) * slide + rolling - CORE_AIR_SHARE * to_air * (core - air);

    tyre.temperature_c = tread + (tread_in - across) * dt / TREAD_HEAT_CAPACITY;
    tyre.core_temperature_c = core + (core_in + across) * dt / CORE_HEAT_CAPACITY;
    refresh(tyre, tire, set_kpa);
}

/// Advance all four tyres, FL FR RL RR.
pub fn step_all(
    state: &mut CarState,
    work: &[TyreWork; 4],
    tire: &TireConfig,
    surface: &TrackSurface,
    dt: f32,
) {
    for ((tyre, w), set) in state
        .tires
        .each_mut()
        .into_iter()
        .zip(work)
        .zip(set_pressures(tire))
    {
        step(tyre, w, tire, set, surface, dt);
    }
}

impl CarState {
    /// How much of its grip the car's weaker axle has from its tyres'
    /// temperatures and pressures (1.0 in the window at the set pressure):
    /// what a driver who can feel the tyres drives to. 1.0 before the
    /// tyres are fitted.
    pub fn tyre_grip_share(&self) -> f32 {
        if !self.tyres_fitted {
            return 1.0;
        }
        let t = &self.tires;
        let front = 0.5 * (t.front_left.grip_factor + t.front_right.grip_factor);
        let rear = 0.5 * (t.rear_left.grip_factor + t.rear_right.grip_factor);
        front.min(rear).clamp(0.0, 1.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tyre() -> TireConfig {
        TireConfig::default()
    }

    #[test]
    fn the_window_is_flat_and_the_edges_ease_in() {
        let t = tyre();
        let opt = t.optimal_temperature_c;
        for d in [-10.0, -5.0, 0.0, 5.0, 10.0] {
            assert_eq!(thermal_grip_factor(&t, opt + d, opt + d), 1.0);
        }
        // Just past the edge: almost nothing, then the full slope.
        let edge = 1.0 - thermal_grip_factor(&t, opt + 11.0, opt + 11.0);
        assert!(
            edge > 0.0 && edge < t.temperature_grip_falloff * 0.3,
            "{edge}"
        );
        let far = |d: f32| thermal_grip_factor(&t, opt + d, opt + d);
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
        let hot_tread = thermal_grip_factor(&t, opt + 30.0, opt);
        let hot_core = thermal_grip_factor(&t, opt, opt + 30.0);
        assert!(hot_tread < hot_core && hot_core == 1.0);
    }

    #[test]
    fn the_set_pressure_is_the_pressure_at_the_optimum() {
        let t = tyre();
        let opt = t.optimal_temperature_c;
        assert!((pressure_kpa(&t, 175.0, opt) - 175.0).abs() < 1e-3);
        // A slick set to 180 hot is at about 125 kPa at 20 °C, as a real
        // tyre that gains half a bar on the way up to temperature.
        let cold = pressure_kpa(&t, 180.0, 20.0);
        assert!((120.0..132.0).contains(&cold), "{cold}");
        assert!(pressure_kpa(&t, 180.0, opt + 20.0) > 195.0);
    }

    #[test]
    fn a_tyre_at_its_optimum_grips_as_it_did_before_temperatures() {
        let t = tyre();
        let mut data = TireData {
            temperature_c: t.optimal_temperature_c,
            core_temperature_c: t.optimal_temperature_c,
            ..Default::default()
        };
        assert_eq!(grip_multiplier(&t, 180.0, &data), 1.0);
        assert_eq!(
            grip_multiplier(&t, 190.0, &data),
            t.pressure_grip_factor(190.0)
        );
        refresh(&mut data, &t, 190.0);
        assert_eq!(data.grip_factor, 1.0, "as good as the setup makes it");
        assert_eq!(data.pressure_kpa, 190.0);
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
        let slide = TyreWork {
            fy: 6_000.0,
            slip_angle_rad: 0.15,
            load_n: 5_000.0,
            speed_mps: 40.0,
            ..Default::default()
        };
        for _ in 0..(240 * 3) {
            step(&mut data, &slide, &t, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(
            data.temperature_c > data.core_temperature_c + 10.0,
            "the tread runs ahead: {data:?}"
        );
        assert!(data.temperature_c > 100.0, "{data:?}");
        let peak = data.temperature_c;
        let straight = TyreWork {
            load_n: 4_000.0,
            speed_mps: 80.0,
            ..Default::default()
        };
        for _ in 0..(240 * 5) {
            step(&mut data, &straight, &t, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c < peak - 15.0, "{peak} -> {data:?}");
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
                step(&mut data, &idle, &t, 180.0, &surface, 1.0 / 240.0);
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
            ..Default::default()
        };
        let run = |surface: &TrackSurface| {
            let mut data = TireData {
                temperature_c: 90.0,
                core_temperature_c: 90.0,
                ..Default::default()
            };
            for _ in 0..(240 * 10) {
                step(&mut data, &roll, &t, 180.0, surface, 1.0 / 240.0);
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
}
