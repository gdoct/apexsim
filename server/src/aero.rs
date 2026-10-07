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
//! The car's **attitude** to the air counts too ([`Posture`]): a car
//! running sideways (a body slip angle) or rolled over in a corner has
//! its floor and wings at an angle the designers did not want, and loses
//! `yaw_sensitivity` / `roll_sensitivity` of its downforce per degree; and
//! a floor run near its stall height at speed **porpoises** (`porpoising`,
//! 0..1): the floor stalls, the car rises, the floor works again, the car
//! squats, at a few hertz ([`PORPOISE_HZ`]), which the physics carries as
//! an oscillation of the downforce and the ride height
//! (`CarState::porpoise_phase` / `porpoise_amp`, [`step_porpoising`]). A
//! car.toml without those keys has none of it.
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
    /// Share of the downforce lost per degree of body slip angle (the car
    /// running sideways to the air), and per degree of body roll.
    #[serde(default)]
    pub yaw_sensitivity: f32,
    #[serde(default)]
    pub roll_sensitivity: f32,
    /// How much the floor porpoises when run near its stall height at
    /// speed, 0 (never) to 1.
    #[serde(default)]
    pub porpoising: f32,
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
            yaw_sensitivity: 0.0,
            roll_sensitivity: 0.0,
            porpoising: 0.0,
            reference_front_m: 0.0,
            reference_rear_m: 0.0,
        }
    }
}

/// The car's attitude to the air this tick, for [`multipliers_at`].
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Posture {
    /// The body's slip angle to the airflow, degrees (either way).
    pub yaw_deg: f32,
    /// The body's roll, degrees (either way).
    pub roll_deg: f32,
    /// The porpoising this tick, as a signed share of the downforce
    /// (`CarState::porpoise_amp` times the phase).
    pub porpoise: f32,
}

impl Posture {
    pub const STRAIGHT: Posture = Posture {
        yaw_deg: 0.0,
        roll_deg: 0.0,
        porpoise: 0.0,
    };
}

/// Porpoising's frequency, Hz, and the most of the downforce it swings
/// at `porpoising = 1`.
pub const PORPOISE_HZ: f32 = 5.0;
pub const PORPOISE_MAX_SWING: f32 = 0.25;
/// The floor porpoises from this many stall heights up, and from this
/// speed, m/s (fully [`PORPOISE_FULL_SPEED_MPS`] above it).
const PORPOISE_ONSET_HEIGHTS: f32 = 1.6;
const PORPOISE_MIN_SPEED_MPS: f32 = 50.0;
const PORPOISE_FULL_SPEED_MPS: f32 = 25.0;
/// How quickly the oscillation builds, and dies, s.
const PORPOISE_RISE_S: f32 = 0.6;
const PORPOISE_DECAY_S: f32 = 0.3;
/// Ride height the oscillation swings the car through at full amplitude, m.
pub const PORPOISE_HEIGHT_SWING_M: f32 = 0.015;

/// Advance a car's porpoising by `dt` at `mean_height_m` and `speed_mps`:
/// returns the phase and the amplitude, and the signed share of the
/// downforce this tick. Nothing for a car without `porpoising`.
pub fn step_porpoising(
    aero: &AeroConfig,
    phase: f32,
    amp: f32,
    mean_height_m: f32,
    speed_mps: f32,
    dt: f32,
) -> (f32, f32, f32) {
    if aero.porpoising <= 0.0 || aero.stall_height_m <= 0.0 {
        return (0.0, 0.0, 0.0);
    }
    let onset = aero.stall_height_m * PORPOISE_ONSET_HEIGHTS;
    let depth = ((onset - mean_height_m) / (onset - aero.stall_height_m)).clamp(0.0, 1.0);
    let speed = ((speed_mps - PORPOISE_MIN_SPEED_MPS) / PORPOISE_FULL_SPEED_MPS).clamp(0.0, 1.0);
    let target = aero.porpoising.min(1.0) * PORPOISE_MAX_SWING * depth * speed;
    let tau = if target > amp {
        PORPOISE_RISE_S
    } else {
        PORPOISE_DECAY_S
    };
    let amp = amp + (target - amp) * (dt / tau).min(1.0);
    let phase = (phase + std::f32::consts::TAU * PORPOISE_HZ * dt) % std::f32::consts::TAU;
    (phase, amp, amp * phase.sin())
}

impl AeroConfig {
    /// Nothing about the car's posture changes its aero.
    pub fn is_constant(&self) -> bool {
        self.ride_height_sensitivity == 0.0
            && self.rake_sensitivity == 0.0
            && self.stall_height_m <= 0.0
            && self.yaw_sensitivity == 0.0
            && self.roll_sensitivity == 0.0
            && self.porpoising == 0.0
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
/// downforce and drag, running straight and level.
pub fn multipliers(config: &CarConfig, front_m: f32, rear_m: f32) -> AeroMultipliers {
    multipliers_at(config, front_m, rear_m, Posture::STRAIGHT)
}

/// Least a sideways or rolled car keeps of its downforce from its attitude.
const ATTITUDE_FLOOR: f32 = 0.5;

/// [`multipliers`] for a car at `posture` to the air.
pub fn multipliers_at(
    config: &CarConfig,
    front_m: f32,
    rear_m: f32,
    posture: Posture,
) -> AeroMultipliers {
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
    // Sideways or rolled, the floor and the wings are off their angles.
    let attitude = 1.0
        - aero.yaw_sensitivity * posture.yaw_deg.abs()
        - aero.roll_sensitivity * posture.roll_deg.abs();
    total *= attitude.max(ATTITUDE_FLOOR);
    // And the floor's porpoising swings it.
    total *= 1.0 + posture.porpoise.clamp(-0.9, 0.9);
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
    fn a_sideways_or_rolled_car_loses_downforce_and_a_straight_one_does_not() {
        let mut car = aero_car();
        let (rf, rr) = (car.aero.reference_front_m, car.aero.reference_rear_m);
        // Without the keys the attitude changes nothing.
        let yawed = multipliers_at(
            &car,
            rf,
            rr,
            Posture {
                yaw_deg: 10.0,
                roll_deg: 3.0,
                porpoise: 0.0,
            },
        );
        assert_eq!(yawed, multipliers(&car, rf, rr));
        car.aero.yaw_sensitivity = 0.01;
        car.aero.roll_sensitivity = 0.02;
        let straight = multipliers_at(&car, rf, rr, Posture::STRAIGHT);
        assert!((straight.downforce_front - 1.0).abs() < 1e-6);
        let yawed = multipliers_at(
            &car,
            rf,
            rr,
            Posture {
                yaw_deg: 10.0,
                roll_deg: 0.0,
                porpoise: 0.0,
            },
        );
        assert!((yawed.downforce_front - 0.9).abs() < 1e-5, "{yawed:?}");
        let rolled = multipliers_at(
            &car,
            rf,
            rr,
            Posture {
                yaw_deg: 0.0,
                roll_deg: 2.5,
                porpoise: 0.0,
            },
        );
        assert!((rolled.downforce_rear - 0.95).abs() < 1e-5, "{rolled:?}");
        // Never below the floor, however sideways.
        let spun = multipliers_at(
            &car,
            rf,
            rr,
            Posture {
                yaw_deg: 90.0,
                roll_deg: 0.0,
                porpoise: 0.0,
            },
        );
        assert!((spun.downforce_front - ATTITUDE_FLOOR).abs() < 1e-5);
        // The racing line's steady state is still the straight car.
        assert!((steady_downforce_factor(&car, REFERENCE_SPEED_MPS) - 1.0).abs() < 0.01);
    }

    #[test]
    fn a_low_floor_at_speed_porpoises_and_a_raised_one_does_not() {
        let mut car = aero_car();
        assert_eq!(
            step_porpoising(&car.aero, 0.0, 0.0, 0.01, 80.0, 0.01),
            (0.0, 0.0, 0.0),
            "no key, no porpoising"
        );
        car.aero.porpoising = 0.8;
        let dt = 1.0 / 420.0;
        let (mut phase, mut amp) = (0.0, 0.0);
        let (mut lo, mut hi) = (f32::MAX, f32::MIN);
        for _ in 0..840 {
            let (p, a, swing) = step_porpoising(&car.aero, phase, amp, 0.013, 85.0, dt);
            phase = p;
            amp = a;
            lo = lo.min(swing);
            hi = hi.max(swing);
        }
        assert!(amp > 0.1, "builds up near the stall height: {amp}");
        assert!(hi > 0.08 && lo < -0.08, "swings both ways: {lo} {hi}");
        // Raise the car and it dies away.
        for _ in 0..420 {
            let (p, a, _) = step_porpoising(&car.aero, phase, amp, 0.04, 85.0, dt);
            phase = p;
            amp = a;
        }
        assert!(amp < 0.01, "{amp}");
        // Slow, it never starts.
        let (_, slow, _) = step_porpoising(&car.aero, 0.0, 0.0, 0.013, 30.0, 1.0);
        assert_eq!(slow, 0.0);
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
