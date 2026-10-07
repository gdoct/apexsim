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
//!   driven on the limiter tires faster;
//! - **kerb strikes, bottoming and landings** ([`kerb_strike_damage`],
//!   [`bottoming_damage`], [`landing_damage`]): the suspension speed of a
//!   wheel on a kerb, or of one driven past its bump stop, filtered over a
//!   few ticks ([`STRIKE_FILTER_S`]: the road mesh's steps are one-tick
//!   spikes, a strike lasts) costs that side or that end of the car above a
//!   threshold, quadratically; a landing costs the ends by the fall's speed,
//!   nose first or tail first by the attitude.
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
//! stop repairs the hits, the heat and the missed shifts (`pit::service_seconds`
//! counts the time), but not the wear: that part of the engine's damage is
//! kept apart ([`DamageState::engine_wear_percent`], [`DamageState::wear_engine`])
//! and a repair takes the engine back to it, since no crew rebuilds an
//! engine in a pit box.

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

/// The strike measure's filter, s: a few ticks at 420 Hz, so a one-tick
/// step where two road-mesh triangles meet (18 m/s of shaft speed for a
/// tick at Suzuka) reads as a fraction of a metre per second and a real
/// strike, which lasts tens of milliseconds, nearly whole.
pub const STRIKE_FILTER_S: f32 = 0.012;
/// A wheel on a kerb with a filtered suspension speed over this, m/s, is
/// striking it; damage per second per (m/s over)^2 to that side.
pub const KERB_STRIKE_MPS: f32 = 1.5;
const KERB_DAMAGE_PER_S: f32 = 8.0;
/// A wheel this far past its bump stop, m, at a filtered speed over this,
/// m/s, is bottoming; damage per second per (m/s over)^2 to that end.
pub const BOTTOMING_PAST_M: f32 = 0.008;
pub const BOTTOMING_STRIKE_MPS: f32 = 0.6;
const BOTTOMING_DAMAGE_PER_S: f32 = 6.0;
/// A landing slower than this, m/s of fall, is a bump; faster, it costs
/// this share of a contact's damage at that speed.
pub const LANDING_MIN_MPS: f32 = 2.0;
const LANDING_SHARE: f32 = 0.5;

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

/// Damage this tick to the side of a wheel on a kerb whose filtered
/// suspension speed is `strike_mps`, percent.
pub fn kerb_strike_damage(strike_mps: f32, dt: f32) -> f32 {
    let over = (strike_mps - KERB_STRIKE_MPS).max(0.0);
    KERB_DAMAGE_PER_S * over * over * dt
}

/// Damage this tick to the end of a wheel `past_m` past its bump stop at a
/// filtered suspension speed of `strike_mps`, percent.
pub fn bottoming_damage(past_m: f32, strike_mps: f32, dt: f32) -> f32 {
    if past_m < BOTTOMING_PAST_M {
        return 0.0;
    }
    let over = (strike_mps - BOTTOMING_STRIKE_MPS).max(0.0);
    BOTTOMING_DAMAGE_PER_S * over * over * dt
}

/// Damage a landing at `fall_mps` does, percent, to be shared between the
/// car's ends.
pub fn landing_damage(fall_mps: f32) -> f32 {
    if fall_mps < LANDING_MIN_MPS {
        return 0.0;
    }
    LANDING_SHARE * impact_damage(fall_mps)
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

    /// Add damage to a side (`left`) and re-judge the car.
    pub fn hurt_side(&mut self, left: bool, percent: f32) {
        if percent <= 0.0 {
            return;
        }
        let zone = if left {
            &mut self.left_damage_percent
        } else {
            &mut self.right_damage_percent
        };
        *zone = (*zone + percent).min(100.0);
        self.refresh();
    }

    /// Add damage to an end (`front`) and re-judge the car.
    pub fn hurt_end(&mut self, front: bool, percent: f32) {
        if percent <= 0.0 {
            return;
        }
        let zone = if front {
            &mut self.front_damage_percent
        } else {
            &mut self.rear_damage_percent
        };
        *zone = (*zone + percent).min(100.0);
        self.refresh();
    }

    /// Wear the engine: damage a repair does not undo.
    pub fn wear_engine(&mut self, percent: f32) {
        if percent > 0.0 {
            self.engine_wear_percent = (self.engine_wear_percent + percent).min(100.0);
            self.hurt_engine(percent);
        }
    }

    /// The car after a pit crew's repairs: every zone mended but the
    /// engine's wear, which stays.
    pub fn repaired(&self) -> DamageState {
        let wear = self.engine_wear_percent.clamp(0.0, 100.0);
        let mut d = DamageState {
            engine_damage_percent: wear,
            engine_wear_percent: wear,
            is_drivable: true,
            ..Default::default()
        };
        d.refresh();
        d
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
    fn strikes_cost_above_a_threshold_and_a_mesh_step_does_not() {
        let dt = 1.0 / 420.0;
        assert_eq!(kerb_strike_damage(1.0, dt), 0.0);
        // A hard kerb strike at 2.5 m/s of filtered shaft speed for 50 ms.
        let hard: f32 = (0..21).map(|_| kerb_strike_damage(2.5, dt)).sum();
        assert!((0.2..0.8).contains(&hard), "{hard}");
        // Short of the stop nothing; past it, over the speed, something.
        assert_eq!(bottoming_damage(0.0, 3.0, dt), 0.0);
        assert_eq!(bottoming_damage(0.02, 0.5, dt), 0.0);
        assert!(bottoming_damage(0.02, 2.0, dt) > 0.0);
        // The filter: an 18 m/s one-tick spike reads as a fraction of a
        // metre per second.
        let mut strike = 0.0f32;
        strike += (18.0 - strike) * (dt / STRIKE_FILTER_S).min(1.0);
        assert!(strike < 4.0, "{strike}");
        for _ in 0..10 {
            strike += (0.0 - strike) * (dt / STRIKE_FILTER_S).min(1.0);
        }
        assert!(strike < 0.5, "{strike}");
        // A landing: a drop is free, a fall is not.
        assert_eq!(landing_damage(1.0), 0.0);
        assert!(landing_damage(6.0) > 0.5 && landing_damage(6.0) < impact_damage(6.0));
        let mut d = DamageState {
            is_drivable: true,
            ..Default::default()
        };
        d.hurt_side(true, 3.0);
        d.hurt_end(false, 2.0);
        assert_eq!((d.left_damage_percent, d.rear_damage_percent), (3.0, 2.0));
    }

    #[test]
    fn a_repair_undoes_the_hits_but_not_the_wear() {
        let mut d = DamageState {
            is_drivable: true,
            front_damage_percent: 40.0,
            engine_damage_percent: 10.0,
            ..Default::default()
        };
        d.wear_engine(5.0);
        assert!((d.engine_damage_percent - 15.0).abs() < 1e-6);
        assert!((d.engine_wear_percent - 5.0).abs() < 1e-6);
        let r = d.repaired();
        assert_eq!(r.front_damage_percent, 0.0);
        assert!((r.engine_damage_percent - 5.0).abs() < 1e-6, "{r:?}");
        assert!((r.engine_wear_percent - 5.0).abs() < 1e-6);
        assert!(r.is_drivable);
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
