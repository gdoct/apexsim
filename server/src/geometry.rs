//! Suspension geometry: camber, toe and bump stops.
//!
//! The suspension is four vertical spring/damper units (`physics`), and the
//! body has no roll of its own: the loads are analytic. This module adds
//! what the wheels' angles do to the tyres, on the same terms.
//!
//! **Camber.** The body's roll is worked out from the lateral load and the
//! roll stiffness the load transfer already uses ([`body_roll_rad`]). Each
//! wheel's camber against the road is its static camber, plus the roll
//! less what the linkage gains back (`camber_gain`: 0 a beam, ~0.5 a
//! double wishbone), less the carcass's own lean under side load
//! ([`CARCASS_LEAN_DEG`] at the car's reference cornering). What the tyre
//! feels is its *lean into the corner*: the outside wheel's negative
//! camber leans it in, the inside wheel's leans it out. Lateral grip peaks
//! at a lean of [`OPTIMAL_LEAN_DEG`] and falls away quadratically either
//! side ([`LATERAL_LOSS_PER_DEG2`]); braking and traction lose a little
//! with any camber at all ([`LONGITUDINAL_LOSS_PER_DEG2`]), which is the
//! trade a setup makes.
//!
//! Both are **normalised to the car as filed**: the lateral grip to the
//! axle's load-weighted grip at the car's reference cornering with its
//! filed camber, the longitudinal to the filed camber upright, so a car
//! at its filed camber grips at those two points exactly as its car.toml
//! says and the class calibration holds; a camber setup away from the file
//! gains or loses from there. A car.toml without camber keys has no
//! camber model at all ([`Geometry::camber_modelled`]).
//!
//! **Toe** (degrees per wheel, positive toe-in) steers each wheel by its
//! toe: the fronts on top of the steering, the rears on their own. It
//! costs drag and tyre heat straight away (the two tyres push against each
//! other) and toe-in at the rear steadies the car; nothing is scaled.
//!
//! **Bump stops** (`bump_stop_gap_m` past the static laden compression,
//! `bump_stop_rate_n_per_m`): a wheel pushed past its stop, over a kerb or
//! bottoming out, carries the stop's force on top of its spring's.

use crate::data::{CarConfig, SuspensionConfig};

/// The lean into the corner at which a tyre grips best, degrees.
pub const OPTIMAL_LEAN_DEG: f32 = 2.0;
/// Lateral grip lost per square degree away from [`OPTIMAL_LEAN_DEG`].
pub const LATERAL_LOSS_PER_DEG2: f32 = 0.004;
/// Braking and traction grip lost per square degree of camber.
pub const LONGITUDINAL_LOSS_PER_DEG2: f32 = 0.0025;
/// The carcass leans out of the corner this far at the car's reference
/// cornering, degrees: what makes a car want static negative camber.
pub const CARCASS_LEAN_DEG: f32 = 2.0;
/// Share of an axle's load the outside wheel carries at the reference, for
/// the normalisation. With the lean and the carcass above, it puts the
/// best static camber near -(0.5 x (roll + 4)) degrees: -2.5 to -3 for
/// most cars.
const REFERENCE_OUTSIDE_SHARE: f32 = 0.75;
/// Speed of the reference cornering, m/s (the aero's own reference).
const REFERENCE_SPEED_MPS: f32 = 50.0;
/// Lateral acceleration over which the grip blends from one hand of
/// corner to the other, m/s² (straight ahead both wheels are neither).
const HAND_BLEND_MPS2: f32 = 1.5;
const GRAVITY: f32 = 9.81;
const AIR_DENSITY: f32 = 1.225;

/// The two grip multipliers camber gives a tyre: sideways and along.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CamberGrip {
    pub lateral: f32,
    pub longitudinal: f32,
}

impl CamberGrip {
    pub const NEUTRAL: CamberGrip = CamberGrip {
        lateral: 1.0,
        longitudinal: 1.0,
    };
}

/// The body's roll, rad, toward the outside of the corner: positive when
/// the car accelerates left (turning left, the body leaning right).
pub fn body_roll_rad(config: &CarConfig, mass_kg: f32, lateral_accel: f32) -> f32 {
    let s = &config.suspension;
    let stiffness = (s.spring_rate_front_n_per_m + s.anti_roll_bar_front)
        * config.track_width_front_m.powi(2)
        / 2.0
        + (s.spring_rate_rear_n_per_m + s.anti_roll_bar_rear) * config.track_width_rear_m.powi(2)
            / 2.0;
    (mass_kg * lateral_accel * config.cog_height_m / stiffness.max(1.0)).clamp(-0.15, 0.15)
}

/// The car's reference cornering, m/s²: its tyres' grip with the downforce
/// it makes at the reference speed.
pub fn reference_lateral_accel(config: &CarConfig, mass_kg: f32) -> f32 {
    let lift = -(config.lift_coefficient_front + config.lift_coefficient_rear);
    let downforce =
        0.5 * AIR_DENSITY * config.frontal_area_m2 * lift.max(0.0) * REFERENCE_SPEED_MPS.powi(2);
    config.tire_config.grip_coefficient * GRAVITY * (1.0 + downforce / (mass_kg.max(1.0) * GRAVITY))
}

fn lateral_grip(lean_deg: f32) -> f32 {
    (1.0 - LATERAL_LOSS_PER_DEG2 * (lean_deg - OPTIMAL_LEAN_DEG).powi(2)).max(0.5)
}

fn longitudinal_grip(camber_deg: f32) -> f32 {
    (1.0 - LONGITUDINAL_LOSS_PER_DEG2 * camber_deg.powi(2)).max(0.5)
}

/// One axle's geometry.
#[derive(Debug, Clone, Copy)]
struct Axle {
    camber_deg: f32,
    filed_camber_deg: f32,
    gain: f32,
}

impl Axle {
    fn of(s: &SuspensionConfig, front: bool) -> Self {
        if front {
            Axle {
                camber_deg: s.camber_front_deg,
                filed_camber_deg: s.camber_filed_front_deg,
                gain: s.camber_gain_front,
            }
        } else {
            Axle {
                camber_deg: s.camber_rear_deg,
                filed_camber_deg: s.camber_filed_rear_deg,
                gain: s.camber_gain_rear,
            }
        }
    }

    /// The outside and inside wheels' leans into a corner at `roll_deg`
    /// (the magnitude) and `usage` of the reference cornering, with static
    /// camber `camber_deg`.
    fn leans(&self, camber_deg: f32, roll_deg: f32, usage: f32) -> (f32, f32) {
        let roll = roll_deg * (1.0 - self.gain);
        let carcass = CARCASS_LEAN_DEG * usage;
        // Outside: its camber against the road is the static plus the
        // roll; negative leans it in. Inside: the roll makes its camber
        // more negative, which leans it out of the corner.
        let outside = -(camber_deg + roll) - carcass;
        let inside = (camber_deg - roll) - carcass;
        (outside, inside)
    }

    /// The axle's grip at the reference with its filed camber: what the
    /// lateral multiplier is measured against.
    fn reference_grip(&self, roll_ref_deg: f32) -> f32 {
        let (out, inn) = self.leans(self.filed_camber_deg, roll_ref_deg, 1.0);
        REFERENCE_OUTSIDE_SHARE * lateral_grip(out)
            + (1.0 - REFERENCE_OUTSIDE_SHARE) * lateral_grip(inn)
    }
}

/// Each wheel's camber grip this tick, FL FR RL RR, from the lateral
/// acceleration the car had last tick (+ = left).
pub fn camber_grip(config: &CarConfig, mass_kg: f32, lateral_accel: f32) -> [CamberGrip; 4] {
    let s = &config.suspension;
    if !s.camber_modelled {
        return [CamberGrip::NEUTRAL; 4];
    }
    let a_ref = reference_lateral_accel(config, mass_kg).max(1.0);
    let roll_ref_deg = body_roll_rad(config, mass_kg, a_ref).to_degrees();
    let roll_deg = body_roll_rad(config, mass_kg, lateral_accel)
        .to_degrees()
        .abs();
    let usage = (lateral_accel.abs() / a_ref).min(1.5);
    // How much the car is turning left (1) or right (0): the left wheels
    // are the inside of a left-hander.
    let left_hand = 0.5 + 0.5 * (lateral_accel / HAND_BLEND_MPS2).tanh();

    let wheel = |axle: &Axle, left: bool| {
        let (out, inn) = axle.leans(axle.camber_deg, roll_deg, usage);
        let (as_outside, as_inside) = (lateral_grip(out), lateral_grip(inn));
        let inside_share = if left { left_hand } else { 1.0 - left_hand };
        let lateral = inside_share * as_inside + (1.0 - inside_share) * as_outside;
        CamberGrip {
            lateral: lateral / axle.reference_grip(roll_ref_deg),
            longitudinal: longitudinal_grip(axle.camber_deg)
                / longitudinal_grip(axle.filed_camber_deg),
        }
    };
    let front = Axle::of(s, true);
    let rear = Axle::of(s, false);
    [
        wheel(&front, true),
        wheel(&front, false),
        wheel(&rear, true),
        wheel(&rear, false),
    ]
}

/// Each wheel's toe as a steer angle, rad, FL FR RL RR (positive turns a
/// wheel left): toe-in turns the left wheels right and the right wheels
/// left.
pub fn toe_steer_rad(s: &SuspensionConfig) -> [f32; 4] {
    let front = s.toe_front_deg.to_radians();
    let rear = s.toe_rear_deg.to_radians();
    [-front, front, -rear, rear]
}

/// The bump stop's force on a wheel at `compression`, N, given the
/// compression the car's static laden weight puts on that corner.
pub fn bump_stop_force_n(
    s: &SuspensionConfig,
    compression_m: f32,
    static_compression_m: f32,
) -> f32 {
    match s.bump_stop_gap_m {
        Some(gap) if s.bump_stop_rate_n_per_m > 0.0 => {
            s.bump_stop_rate_n_per_m * (compression_m - static_compression_m - gap).max(0.0)
        }
        _ => 0.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn car(camber: f32, gain: f32) -> CarConfig {
        let mut config = CarConfig::default();
        let s = &mut config.suspension;
        s.camber_modelled = true;
        s.camber_front_deg = camber;
        s.camber_rear_deg = camber;
        s.camber_filed_front_deg = camber;
        s.camber_filed_rear_deg = camber;
        s.camber_gain_front = gain;
        s.camber_gain_rear = gain;
        config
    }

    fn mass(config: &CarConfig) -> f32 {
        config.mass_kg
    }

    #[test]
    fn a_car_without_camber_keys_is_untouched() {
        let config = CarConfig::default();
        assert!(!config.suspension.camber_modelled);
        assert_eq!(camber_grip(&config, 1200.0, 12.0), [CamberGrip::NEUTRAL; 4]);
        assert_eq!(toe_steer_rad(&config.suspension), [0.0; 4]);
    }

    #[test]
    fn the_filed_camber_grips_as_filed_at_the_reference() {
        let config = car(-3.0, 0.5);
        let m = mass(&config);
        let a = reference_lateral_accel(&config, m);
        // A left-hander at the reference: the right wheels are outside.
        let g = camber_grip(&config, m, a);
        let axle =
            REFERENCE_OUTSIDE_SHARE * g[1].lateral + (1.0 - REFERENCE_OUTSIDE_SHARE) * g[0].lateral;
        assert!((axle - 1.0).abs() < 0.01, "{axle}");
        assert!(
            g[1].lateral > g[0].lateral,
            "the outside leans in, the inside out"
        );
        assert_eq!(g[0].longitudinal, 1.0);
    }

    #[test]
    fn more_camber_corners_better_and_brakes_worse_up_to_a_point() {
        let filed = car(-1.0, 0.5);
        let m = mass(&filed);
        let a = reference_lateral_accel(&filed, m);
        let with = |camber: f32| {
            let mut c = filed.clone();
            c.suspension.camber_front_deg = camber;
            c.suspension.camber_rear_deg = camber;
            let g = camber_grip(&c, m, a);
            (
                REFERENCE_OUTSIDE_SHARE * g[1].lateral
                    + (1.0 - REFERENCE_OUTSIDE_SHARE) * g[0].lateral,
                camber_grip(&c, m, 0.0)[0].longitudinal,
            )
        };
        let (lat_1, long_1) = with(-1.0);
        let (lat_3, long_3) = with(-3.0);
        let (lat_6, _) = with(-6.0);
        assert!(lat_3 > lat_1, "{lat_3} {lat_1}");
        assert!(long_3 < long_1);
        assert!(lat_6 < lat_3, "too much is worse again: {lat_6} {lat_3}");
    }

    #[test]
    fn the_hands_are_mirror_images_and_straight_ahead_is_even() {
        let config = car(-3.0, 0.5);
        let m = mass(&config);
        let left = camber_grip(&config, m, 15.0);
        let right = camber_grip(&config, m, -15.0);
        assert!((left[0].lateral - right[1].lateral).abs() < 1e-6);
        let straight = camber_grip(&config, m, 0.0);
        assert!((straight[0].lateral - straight[1].lateral).abs() < 1e-6);
    }

    #[test]
    fn toe_in_points_the_wheels_at_each_other_and_stops_push_back() {
        let mut s = SuspensionConfig {
            toe_front_deg: 0.1,
            toe_rear_deg: 0.2,
            ..Default::default()
        };
        let toe = toe_steer_rad(&s);
        assert!(toe[0] < 0.0 && toe[1] > 0.0 && toe[2] < 0.0 && toe[3] > 0.0);
        assert_eq!(bump_stop_force_n(&s, 0.1, 0.02), 0.0);
        s.bump_stop_gap_m = Some(0.03);
        s.bump_stop_rate_n_per_m = 200_000.0;
        assert_eq!(bump_stop_force_n(&s, 0.04, 0.02), 0.0);
        assert!((bump_stop_force_n(&s, 0.06, 0.02) - 2000.0).abs() < 1e-2);
    }
}
