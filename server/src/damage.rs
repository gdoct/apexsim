//! Progressive damage: what a hit, an overheating engine or a missed shift
//! costs a car long before it is out.
//!
//! A car's damage is five percentages (`DamageState`): the front, the rear,
//! each side and the engine. They grow from
//!
//! - **impacts**, car against car or car against wall, with the closing
//!   speed along the contact normal to a power ([`impact_damage`]), so a
//!   brush costs nothing and a real hit costs a lot; a hit to the nose dents
//!   the engine too;
//! - **heat**: an engine past `engine_heat::OVERHEAT_C` wears itself out
//!   ([`OVERHEAT_DAMAGE_PER_C_S`] a second per degree over);
//! - **over-revving**: a downshift that spins the engine past its limit
//!   ([`over_rev_damage`]);
//! - **wear**: every hour at the top of the rev range costs a few percent
//!   ([`engine_wear`]), so a 24-hour race ends on a tired engine and one
//!   driven on the limiter tires faster.
//!
//! and cost, progressively:
//!
//! - the **front** its downforce (`aero::FRONT_DAMAGE_AERO_LOSS`) and its
//!   radiator's cooling ([`radiator_factor`]), so a damaged nose runs hot;
//! - the **rear** its rear downforce ([`rear_downforce_factor`]);
//! - a **side** its suspension: that side's tyres grip less and the car
//!   pulls toward it ([`toe_offset_rad`], [`side_grip_factor`]);
//! - the **engine** its power ([`engine_power_factor`]).
//!
//! A car is out when a zone reaches 100% ([`DamageState::refresh`]). A pit
//! stop repairs it all (`pit::service_seconds` counts the time).

use crate::data::DamageState;

/// Closing speed under which contact does no damage, m/s.
pub const IMPACT_THRESHOLD_MPS: f32 = 2.5;
/// Damage per (m/s over the threshold) to [`IMPACT_EXPONENT`], percent.
const IMPACT_SCALE: f32 = 0.2;
const IMPACT_EXPONENT: f32 = 1.6;
/// Share of a hit to the nose the engine takes (the radiators, the
/// cooling behind them).
pub const NOSE_TO_ENGINE: f32 = 0.3;
/// Engine damage per second per degree over `engine_heat::OVERHEAT_C`,
/// percent: at 120 °C an engine is gone in under three minutes.
pub const OVERHEAT_DAMAGE_PER_C_S: f32 = 0.08;
/// Engine damage per second per share over the rev limit, percent, above
/// a small tolerance ([`OVER_REV_TOLERANCE`]).
const OVER_REV_DAMAGE_PER_S: f32 = 400.0;
const OVER_REV_TOLERANCE: f32 = 1.02;
/// Engine wear, percent per hour, at the rev limit, and how steeply it
/// falls with the revs: at 85% of the limit (a race's running) it is a
/// third of that, about a quarter of the engine over 24 hours; at idle,
/// nothing to speak of.
const ENGINE_WEAR_PER_HOUR: f32 = 3.0;
const ENGINE_WEAR_EXPONENT: i32 = 6;

/// Most toe a fully damaged side bends into its front wheel, rad.
const MAX_TOE_RAD: f32 = 0.025;
/// Grip a fully damaged side's tyres lose.
const SIDE_GRIP_LOSS: f32 = 0.12;
/// Rear downforce a fully damaged rear loses (wing, diffuser).
const REAR_DOWNFORCE_LOSS: f32 = 0.4;
/// Cooling a fully damaged nose loses (its radiator inlets).
const RADIATOR_LOSS: f32 = 0.6;
/// Power a fully damaged engine has lost, and the curve's shape.
const ENGINE_POWER_LOSS: f32 = 0.4;
const ENGINE_LOSS_EXPONENT: f32 = 1.2;

/// Damage a contact at `closing_mps` does to the zone it hits, percent.
pub fn impact_damage(closing_mps: f32) -> f32 {
    let over = (closing_mps.min(60.0) - IMPACT_THRESHOLD_MPS).max(0.0);
    IMPACT_SCALE * over.powf(IMPACT_EXPONENT)
}

/// Engine damage this tick from revs the gearbox forced past the limit:
/// `ratio` is the revs the wheels ask for over the engine's limit.
pub fn over_rev_damage(ratio: f32, dt: f32) -> f32 {
    (ratio - OVER_REV_TOLERANCE).max(0.0) * OVER_REV_DAMAGE_PER_S * dt
}

/// Engine wear this tick, percent, turning at `rpm_share` of its limit.
pub fn engine_wear(rpm_share: f32, dt: f32) -> f32 {
    ENGINE_WEAR_PER_HOUR * rpm_share.clamp(0.0, 1.1).powi(ENGINE_WEAR_EXPONENT) * dt / 3600.0
}

/// Engine damage this tick from running at `coolant_c`.
pub fn overheat_damage(coolant_c: f32, dt: f32) -> f32 {
    (coolant_c - crate::engine_heat::OVERHEAT_C).max(0.0) * OVERHEAT_DAMAGE_PER_C_S * dt
}

fn share(percent: f32) -> f32 {
    (percent / 100.0).clamp(0.0, 1.0)
}

/// Toe the side damage bends into the front wheels, rad, positive to the
/// left: the car pulls toward its damaged side.
pub fn toe_offset_rad(damage: &DamageState) -> f32 {
    MAX_TOE_RAD * (share(damage.left_damage_percent) - share(damage.right_damage_percent))
}

/// Grip a side's tyres keep: `left` for the left wheels.
pub fn side_grip_factor(damage: &DamageState, left: bool) -> f32 {
    let d = if left {
        damage.left_damage_percent
    } else {
        damage.right_damage_percent
    };
    1.0 - SIDE_GRIP_LOSS * share(d)
}

/// The rear downforce a damaged rear keeps.
pub fn rear_downforce_factor(damage: &DamageState) -> f32 {
    1.0 - REAR_DOWNFORCE_LOSS * share(damage.rear_damage_percent)
}

/// The cooling a damaged nose's radiators keep.
pub fn radiator_factor(damage: &DamageState) -> f32 {
    1.0 - RADIATOR_LOSS * share(damage.front_damage_percent)
}

/// The power a damaged engine keeps.
pub fn engine_power_factor(damage: &DamageState) -> f32 {
    1.0 - ENGINE_POWER_LOSS * share(damage.engine_damage_percent).powf(ENGINE_LOSS_EXPONENT)
}

impl DamageState {
    /// Out when a zone is gone: a wrecked car or a blown engine.
    pub fn refresh(&mut self) {
        self.is_drivable = [
            self.front_damage_percent,
            self.rear_damage_percent,
            self.left_damage_percent,
            self.right_damage_percent,
            self.engine_damage_percent,
        ]
        .iter()
        .all(|d| *d < 100.0);
    }

    /// Add engine damage and re-judge whether the car can go on.
    pub fn hurt_engine(&mut self, percent: f32) {
        if percent > 0.0 {
            self.engine_damage_percent = (self.engine_damage_percent + percent).min(100.0);
            self.refresh();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_brush_is_free_and_a_real_hit_is_not() {
        assert_eq!(impact_damage(2.0), 0.0);
        assert!(impact_damage(5.0) < 1.0);
        let ten = impact_damage(10.0);
        let thirty = impact_damage(30.0);
        assert!((3.0..6.0).contains(&ten), "{ten}");
        assert!((30.0..50.0).contains(&thirty), "{thirty}");
        assert!(impact_damage(50.0) > 90.0);
    }

    #[test]
    fn each_zone_costs_its_own_thing() {
        let clean = DamageState::default();
        assert_eq!(toe_offset_rad(&clean), 0.0);
        assert_eq!(side_grip_factor(&clean, true), 1.0);
        assert_eq!(engine_power_factor(&clean), 1.0);
        assert_eq!(radiator_factor(&clean), 1.0);
        let left = DamageState {
            left_damage_percent: 50.0,
            ..Default::default()
        };
        assert!(toe_offset_rad(&left) > 0.0, "pulls to its damaged side");
        assert!(side_grip_factor(&left, true) < 1.0 && side_grip_factor(&left, false) == 1.0);
        let engine = DamageState {
            engine_damage_percent: 50.0,
            ..Default::default()
        };
        let power = engine_power_factor(&engine);
        assert!((0.8..0.9).contains(&power), "{power}");
    }

    #[test]
    fn heat_and_a_missed_shift_hurt_the_engine_and_a_zone_at_100_is_out() {
        assert_eq!(overheat_damage(100.0, 1.0), 0.0);
        assert!((overheat_damage(120.0, 1.0) - 0.64).abs() < 1e-4);
        assert_eq!(over_rev_damage(1.0, 1.0), 0.0);
        assert!(
            over_rev_damage(1.5, 1.0 / 240.0) > 0.5,
            "a money shift in one tick"
        );
        let mut d = DamageState {
            is_drivable: true,
            engine_damage_percent: 99.5,
            ..Default::default()
        };
        d.hurt_engine(0.4);
        assert!(d.is_drivable);
        d.hurt_engine(1.0);
        assert!(!d.is_drivable);
    }

    #[test]
    fn an_engine_wears_by_the_hour_and_faster_on_the_limiter() {
        let day = |share: f32| engine_wear(share, 24.0 * 3600.0);
        let racing = day(0.85);
        assert!((20.0..35.0).contains(&racing), "{racing}");
        assert!(day(1.0) > 2.0 * racing);
        assert!(engine_wear(0.85, 3600.0) < 1.5, "a sprint barely notices");
    }
}
