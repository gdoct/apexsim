//! Aerodynamics that answer the car's posture: ride height and rake.
//!
//! The downforce of a car that works its floor depends on how close to the
//! road it runs and at what angle. Lower is more downforce (the floor
//! accelerates the air under it harder) until the floor **stalls** and gives
//! a chunk of it back; more **rake** (rear up against the front) moves the
//! balance forward. That is what makes an aero car change under braking (the
//! nose dives: more downforce, balance forward), at speed (the car squats on
//! its springs: more still) and with its springs (a softer car runs lower),
//! and it is what the springs and ride-height knobs of a real setup are for.
//!
//! The sim has no body heave of its own (the body follows the ground, the
//! loads are worked out analytically), so the ride height is too: each axle
//! sits at its static height less its load change over its springs
//! ([`ride_heights`]), stiffening [`BUMP_STIFFENING`] times on the bump
//! rubbers once it has used [`FREE_TRAVEL_SHARE`] of its height. The loads
//! are last tick's, which already carry the downforce, the braking and the
//! acceleration.
//!
//! The map ([`multipliers`]) is referenced to where the car rides at
//! [`REFERENCE_SPEED_MPS`] on its own (stock) springs: there it makes exactly
//! the downforce its car.toml says, so the class lap-time calibration holds,
//! and it makes more lower down and less higher up. A car without an
//! `[aero]` table has no sensitivity and makes exactly what it always did.
//! The racing line plans with the same map at steady state
//! ([`steady_downforce_factor`]).

use serde::{Deserialize, Serialize};

use crate::data::CarConfig;

/// Where the map is referenced: the speed at which the car makes exactly
/// its car.toml's downforce, m/s.
pub const REFERENCE_SPEED_MPS: f32 = 50.0;
/// An axle moves freely through this share of its static ride height, then
/// sits on its bump rubbers.
pub const FREE_TRAVEL_SHARE: f32 = 0.65;
/// How much stiffer the bump rubbers are than the springs.
pub const BUMP_STIFFENING: f32 = 6.0;
/// Most an unloaded axle rises over its static height, m.
const MAX_EXTENSION_M: f32 = 0.05;
/// Downforce a fully stalled floor gives back.
const STALL_LOSS: f32 = 0.4;
/// Share of a change in downforce that comes back as induced drag.
const INDUCED_DRAG_SHARE: f32 = 0.25;
/// Share of the front downforce a fully destroyed nose loses.
pub const FRONT_DAMAGE_AERO_LOSS: f32 = 0.5;
/// However the car sits, each axle keeps at least this share of its
/// downforce and gets at most this much more.
const MULTIPLIER_RANGE: (f32, f32) = (0.3, 1.8);

/// A car's ride-height aero (`[aero]` in car.toml). All sensitivities zero
/// is a car whose downforce is a constant, as every car's was.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct AeroConfig {
    /// Static ride height at rest, front and rear, m.
    pub ride_height_front_m: f32,
    pub ride_height_rear_m: f32,
    /// Share of the downforce gained per centimetre the car runs lower (its
    /// mean height) than at the reference.
    pub ride_height_sensitivity: f32,
    /// Shift of the front's share of the downforce per centimetre more rake
    /// (rear height less front) than at the reference.
    pub rake_sensitivity: f32,
    /// Mean height below which the floor stalls, m.
    pub stall_height_m: f32,
    /// Where the car rides at [`REFERENCE_SPEED_MPS`] on its stock springs
    /// ([`fit_reference`]); the map is zero there.
    #[serde(default)]
    pub reference_front_m: f32,
    #[serde(default)]
    pub reference_rear_m: f32,
}

impl Default for AeroConfig {
    fn default() -> Self {
        Self {
            ride_height_front_m: 0.06,
            ride_height_rear_m: 0.08,
            ride_height_sensitivity: 0.0,
            rake_sensitivity: 0.0,
            stall_height_m: 0.0,
            reference_front_m: 0.0,
            reference_rear_m: 0.0,
        }
    }
}

impl AeroConfig {
    /// Nothing about the car's posture changes its aero.
    pub fn is_constant(&self) -> bool {
        self.ride_height_sensitivity == 0.0
            && self.rake_sensitivity == 0.0
            && self.stall_height_m <= 0.0
    }
}

/// What the posture does to the car's aero, as multipliers on its car.toml
/// figures.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AeroMultipliers {
    pub downforce_front: f32,
    pub downforce_rear: f32,
    pub drag: f32,
}

impl AeroMultipliers {
    pub const ONE: AeroMultipliers = AeroMultipliers {
        downforce_front: 1.0,
        downforce_rear: 1.0,
        drag: 1.0,
    };
}

/// How far one axle has moved down from its static height for a load
/// `delta_n` above its static load (negative: lighter), m. Both wheels
/// share it; past the free travel the bump rubbers take over.
fn axle_travel(delta_n: f32, spring_n_per_m: f32, static_height_m: f32) -> f32 {
    let k = 2.0 * spring_n_per_m.max(1.0);
    let travel = delta_n / k;
    let free = FREE_TRAVEL_SHARE * static_height_m.max(0.0);
    if travel <= free {
        travel.max(-MAX_EXTENSION_M)
    } else {
        free + (delta_n - k * free) / (k * BUMP_STIFFENING)
    }
}

/// Front and rear ride height, m, for axle loads this far above their
/// static loads, N. Never below the road.
pub fn ride_heights(config: &CarConfig, front_delta_n: f32, rear_delta_n: f32) -> (f32, f32) {
    let aero = &config.aero;
    let s = &config.suspension;
    let front = aero.ride_height_front_m
        - axle_travel(
            front_delta_n,
            s.spring_rate_front_n_per_m,
            aero.ride_height_front_m,
        );
    let rear = aero.ride_height_rear_m
        - axle_travel(
            rear_delta_n,
            s.spring_rate_rear_n_per_m,
            aero.ride_height_rear_m,
        );
    (front.max(0.0), rear.max(0.0))
}

/// The front's share of the car's downforce by its car.toml figures.
fn front_share(config: &CarConfig) -> f32 {
    let (f, r) = (
        (-config.lift_coefficient_front).max(0.0),
        (-config.lift_coefficient_rear).max(0.0),
    );
    if f + r <= 0.0 {
        0.5
    } else {
        f / (f + r)
    }
}

/// The aero map: what a car riding at these heights makes of its car.toml
/// downforce and drag.
pub fn multipliers(config: &CarConfig, front_m: f32, rear_m: f32) -> AeroMultipliers {
    let aero = &config.aero;
    if aero.is_constant() {
        return AeroMultipliers::ONE;
    }
    let mean = 0.5 * (front_m + rear_m);
    let reference_mean = 0.5 * (aero.reference_front_m + aero.reference_rear_m);
    let mut total = 1.0 + aero.ride_height_sensitivity * 100.0 * (reference_mean - mean);
    if aero.stall_height_m > 0.0 && mean < aero.stall_height_m {
        let depth = ((aero.stall_height_m - mean) / aero.stall_height_m).min(1.0);
        total *= 1.0 - STALL_LOSS * depth;
    }
    let s0 = front_share(config);
    let rake = rear_m - front_m;
    let reference_rake = aero.reference_rear_m - aero.reference_front_m;
    let shift = (aero.rake_sensitivity * 100.0 * (rake - reference_rake))
        .clamp(-0.5 * s0, 0.5 * (1.0 - s0));
    let (lo, hi) = MULTIPLIER_RANGE;
    let front = if s0 > 0.0 {
        total * (s0 + shift) / s0
    } else {
        total
    };
    let rear = if s0 < 1.0 {
        total * (1.0 - s0 - shift) / (1.0 - s0)
    } else {
        total
    };
    AeroMultipliers {
        downforce_front: front.clamp(lo, hi),
        downforce_rear: rear.clamp(lo, hi),
        drag: 1.0 + INDUCED_DRAG_SHARE * (total.clamp(lo, hi) - 1.0),
    }
}

/// The car's clean-air downforce from its car.toml, front and rear, N, at
/// `speed`.
fn filed_downforce(config: &CarConfig, speed: f32) -> (f32, f32) {
    let q = 0.5 * crate::physics::AIR_DENSITY * speed * speed * config.frontal_area_m2;
    (
        (-config.lift_coefficient_front).max(0.0) * q,
        (-config.lift_coefficient_rear).max(0.0) * q,
    )
}

/// Where the car rides and what it makes of its downforce running straight
/// at `speed` with nothing but the downforce on its springs: the map solved
/// against itself (a handful of rounds; the loop's gain is well under one).
pub fn steady_state(config: &CarConfig, speed: f32) -> ((f32, f32), AeroMultipliers) {
    let (front0, rear0) = filed_downforce(config, speed);
    let mut m = AeroMultipliers::ONE;
    let mut heights = ride_heights(config, front0, rear0);
    if config.aero.is_constant() {
        return (heights, m);
    }
    for _ in 0..8 {
        m = multipliers(config, heights.0, heights.1);
        heights = ride_heights(config, front0 * m.downforce_front, rear0 * m.downforce_rear);
    }
    (heights, multipliers(config, heights.0, heights.1))
}

/// The share of its car.toml downforce the car makes at steady `speed`:
/// what the racing line plans with.
pub fn steady_downforce_factor(config: &CarConfig, speed: f32) -> f32 {
    if config.aero.is_constant() {
        return 1.0;
    }
    let (front0, rear0) = filed_downforce(config, speed.max(1.0));
    let (_, m) = steady_state(config, speed.max(1.0));
    let total = front0 + rear0;
    if total <= 0.0 {
        return 1.0;
    }
    (front0 * m.downforce_front + rear0 * m.downforce_rear) / total
}

/// Set the map's reference: where the car rides at [`REFERENCE_SPEED_MPS`]
/// with its car.toml downforce on its springs. Done once when the car is
/// loaded, on its stock springs, so a setup that changes the springs or the
/// ride height moves the car off it.
pub fn fit_reference(config: &mut CarConfig) {
    let (front, rear) = filed_downforce(config, REFERENCE_SPEED_MPS);
    let (hf, hr) = ride_heights(config, front, rear);
    config.aero.reference_front_m = hf;
    config.aero.reference_rear_m = hr;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn aero_car() -> CarConfig {
        let mut car = CarConfig {
            lift_coefficient_front: -0.8,
            lift_coefficient_rear: -1.2,
            frontal_area_m2: 2.0,
            ..CarConfig::default()
        };
        car.aero = AeroConfig {
            ride_height_front_m: 0.04,
            ride_height_rear_m: 0.08,
            ride_height_sensitivity: 0.03,
            rake_sensitivity: 0.01,
            stall_height_m: 0.012,
            ..AeroConfig::default()
        };
        fit_reference(&mut car);
        car
    }

    #[test]
    fn a_car_without_an_aero_table_makes_its_filed_downforce() {
        let car = CarConfig::default();
        assert!(car.aero.is_constant());
        assert_eq!(multipliers(&car, 0.0, 0.2), AeroMultipliers::ONE);
        assert_eq!(steady_downforce_factor(&car, 80.0), 1.0);
    }

    #[test]
    fn at_the_reference_the_map_is_the_file() {
        let car = aero_car();
        let m = multipliers(&car, car.aero.reference_front_m, car.aero.reference_rear_m);
        assert!((m.downforce_front - 1.0).abs() < 1e-6);
        assert!((m.downforce_rear - 1.0).abs() < 1e-6);
        assert!((m.drag - 1.0).abs() < 1e-6);
        // And at the reference speed the steady car is close to it (the
        // steady state also carries the map's own feedback).
        let f = steady_downforce_factor(&car, REFERENCE_SPEED_MPS);
        assert!((f - 1.0).abs() < 0.01, "{f}");
    }

    #[test]
    fn faster_is_lower_and_lower_is_more_downforce() {
        let car = aero_car();
        let slow = steady_state(&car, 30.0);
        let fast = steady_state(&car, 80.0);
        assert!(fast.0 .0 < slow.0 .0 && fast.0 .1 < slow.0 .1);
        assert!(steady_downforce_factor(&car, 80.0) > 1.0);
        assert!(steady_downforce_factor(&car, 30.0) < 1.0);
    }

    #[test]
    fn a_nose_dive_moves_the_balance_forward() {
        let car = aero_car();
        let (rf, rr) = (car.aero.reference_front_m, car.aero.reference_rear_m);
        let dive = multipliers(&car, rf - 0.01, rr + 0.005);
        assert!(dive.downforce_front > dive.downforce_rear);
        assert!(dive.downforce_front > 1.0);
    }

    #[test]
    fn a_floor_on_the_ground_stalls() {
        let car = aero_car();
        let near = multipliers(&car, 0.014, 0.014);
        let on = multipliers(&car, 0.002, 0.002);
        assert!(on.downforce_front < near.downforce_front);
    }

    #[test]
    fn the_bump_rubbers_hold_the_car_off_the_road() {
        let car = aero_car();
        let (free, _) = ride_heights(
            &car,
            2.0 * car.suspension.spring_rate_front_n_per_m * 0.02,
            0.0,
        );
        let (bumped, _) = ride_heights(
            &car,
            2.0 * car.suspension.spring_rate_front_n_per_m * 0.06,
            0.0,
        );
        assert!((free - 0.02).abs() < 1e-5, "{free}");
        assert!(
            bumped > 0.0,
            "three times the load does not ground it: {bumped}"
        );
        let (light, _) = ride_heights(&car, -1.0e6, 0.0);
        assert!((light - (0.04 + MAX_EXTENSION_M)).abs() < 1e-6);
    }
}
