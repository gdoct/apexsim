//! Slipstream and dirty air: what the cars ahead do to the air a car
//! drives into.
//!
//! A car moving through still air leaves a wake behind it: slower air, so a
//! car in it has less **drag** (the tow, the overtaking mechanic on every
//! straight), and turbulent air, so its wings make less **downforce** (the
//! dirty air that makes following through a fast corner hard). Both are
//! strongest right behind the car and fade with the gap, the downforce loss
//! much faster than the tow; both fade sideways across a wake that widens
//! as it trails. The front wing sits lowest in the turbulence and loses the
//! most, so a car following closely also understeers.
//!
//! The game session runs [`update`] once per tick before the physics, like
//! the DRS rule: from a snapshot of every car's position and velocity it
//! sets each car's `CarState::wake` (1.0 in clean air), which
//! `physics::calculate_aerodynamic_forces` multiplies into the drag and the
//! downforce. A car in several wakes takes the strongest of each. The snapshot
//! is taken before any car is written, so the answer does not depend on the
//! order the cars are visited in, and it is a pure function of the positions,
//! so the sim stays deterministic.
//!
//! How strong a wake is scales with the leader's drag area against the
//! follower's ([`size_ratio`]): a hypercar punches a bigger hole than a GT3,
//! and an F1 car behind a GT3 feels less of it.

use std::collections::{BTreeMap, HashMap};

use serde::{Deserialize, Serialize};

use crate::data::{CarConfig, CarConfigId, CarState, PlayerId};

/// Drag saved right behind a car of the same size, bumper to bumper.
pub const TOW_MAX: f32 = 0.35;
/// The tow falls by a factor e over this much gap, m.
pub const TOW_DECAY_M: f32 = 30.0;
/// Downforce lost right behind a car of the same size.
pub const DIRTY_AIR_MAX: f32 = 0.25;
/// The downforce loss falls by a factor e over this much gap, m.
pub const DIRTY_AIR_DECAY_M: f32 = 15.0;
/// The front wing's share of the loss and the rear's: the front loses more.
const FRONT_LOSS_SHARE: f32 = 1.3;
const REAR_LOSS_SHARE: f32 = 0.7;
/// No wake counts past this gap, m.
const WAKE_REACH_M: f32 = 100.0;
/// A wake's lateral spread: half the leader's width at its tail, widening
/// by this much per metre behind it (one sigma of a Gaussian).
const WAKE_SPREAD_PER_M: f32 = 0.04;
/// A leader slower than this leaves no wake worth the name; it grows to
/// full strength by [`WAKE_FULL_SPEED_MPS`].
const WAKE_MIN_SPEED_MPS: f32 = 10.0;
const WAKE_FULL_SPEED_MPS: f32 = 25.0;
/// Range the size ratio is held to.
const SIZE_RATIO_RANGE: (f32, f32) = (0.6, 1.4);
/// A tow under this share is not worth a light on the HUD.
pub const TOW_SHOWN: f32 = 0.03;

/// The air a car is driving into, as multipliers on its clean-air figures.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Wake {
    pub drag: f32,
    pub downforce_front: f32,
    pub downforce_rear: f32,
}

impl Wake {
    pub const CLEAN: Wake = Wake {
        drag: 1.0,
        downforce_front: 1.0,
        downforce_rear: 1.0,
    };

    /// Share of the drag the tow saves (0 in clean air).
    pub fn tow(&self) -> f32 {
        1.0 - self.drag
    }
}

impl Default for Wake {
    fn default() -> Self {
        Self::CLEAN
    }
}

/// One car as the wake model sees it.
#[derive(Debug, Clone, Copy)]
struct Body {
    x: f32,
    y: f32,
    vx: f32,
    vy: f32,
    yaw: f32,
    length: f32,
    width: f32,
    /// Drag area, m² (Cd·A).
    drag_area: f32,
}

/// How much stronger (or weaker) a wake feels than one from a car the
/// follower's own size: the root of the leader's drag area over the
/// follower's, held to [`SIZE_RATIO_RANGE`].
fn size_ratio(leader_drag_area: f32, follower_drag_area: f32) -> f32 {
    let (lo, hi) = SIZE_RATIO_RANGE;
    (leader_drag_area.max(0.01) / follower_drag_area.max(0.01))
        .sqrt()
        .clamp(lo, hi)
}

/// The wake of `leader` where `follower` is: `(tow, dirty)`, the share of
/// drag saved and of downforce lost (before the axle split).
fn wake_of(leader: &Body, follower: &Body) -> (f32, f32) {
    let speed = (leader.vx * leader.vx + leader.vy * leader.vy).sqrt();
    if speed < WAKE_MIN_SPEED_MPS {
        return (0.0, 0.0);
    }
    // The wake trails behind the leader along its velocity.
    let (dx, dy) = (leader.vx / speed, leader.vy / speed);
    let (rx, ry) = (follower.x - leader.x, follower.y - leader.y);
    let behind = -(rx * dx + ry * dy);
    let gap = behind - 0.5 * (leader.length + follower.length);
    if behind <= 0.0 || gap > WAKE_REACH_M {
        return (0.0, 0.0);
    }
    let gap = gap.max(0.0);
    let lateral = rx * -dy + ry * dx;
    let sigma = 0.5 * leader.width + WAKE_SPREAD_PER_M * gap;
    let across = (-0.5 * (lateral / sigma).powi(2)).exp();
    // A car pointing across the wake (spinning, crossing it at an angle)
    // meets it with less of its front.
    let facing = (follower.yaw.cos() * dx + follower.yaw.sin() * dy).max(0.0);
    let strength = ((speed - WAKE_MIN_SPEED_MPS) / (WAKE_FULL_SPEED_MPS - WAKE_MIN_SPEED_MPS))
        .clamp(0.0, 1.0)
        * across
        * facing
        * size_ratio(leader.drag_area, follower.drag_area);
    (
        TOW_MAX * (-gap / TOW_DECAY_M).exp() * strength,
        DIRTY_AIR_MAX * (-gap / DIRTY_AIR_DECAY_M).exp() * strength,
    )
}

/// Set every car's wake for this tick from where the field is now. Cars in
/// a hotlap garage neither leave nor feel one.
pub fn update(
    participants: &mut BTreeMap<PlayerId, CarState>,
    car_configs: &HashMap<CarConfigId, CarConfig>,
) {
    let bodies: Vec<(PlayerId, Body)> = participants
        .values()
        .filter(|s| !s.in_garage)
        .filter_map(|s| {
            let config = car_configs.get(&s.car_config_id)?;
            Some((
                s.player_id,
                Body {
                    x: s.pos_x,
                    y: s.pos_y,
                    vx: s.vel_x,
                    vy: s.vel_y,
                    yaw: s.yaw_rad,
                    length: config.length_m,
                    width: config.width_m,
                    drag_area: config.drag_coefficient * config.frontal_area_m2,
                },
            ))
        })
        .collect();

    for state in participants.values_mut() {
        let Some((_, me)) = bodies.iter().find(|(id, _)| *id == state.player_id) else {
            state.wake = Wake::CLEAN;
            continue;
        };
        let (mut tow, mut dirty) = (0.0f32, 0.0f32);
        for (id, other) in &bodies {
            if *id == state.player_id {
                continue;
            }
            let (dx, dy) = (other.x - me.x, other.y - me.y);
            let reach = WAKE_REACH_M + other.length + me.length;
            if dx * dx + dy * dy > reach * reach {
                continue;
            }
            let (t, d) = wake_of(other, me);
            tow = tow.max(t);
            dirty = dirty.max(d);
        }
        state.wake = Wake {
            drag: 1.0 - tow.min(0.9),
            downforce_front: 1.0 - (dirty * FRONT_LOSS_SHARE).min(0.9),
            downforce_rear: 1.0 - (dirty * REAR_LOSS_SHARE).min(0.9),
        };
    }
}

impl CarState {
    /// The share of its clean-air tyre load a car has in the wake it is in:
    /// its weight and downforce now over its weight and the downforce it
    /// would have in clean air. What a driver feels as the car going light
    /// behind another; 1.0 in clean air.
    pub fn wake_load_share(&self, mass_kg: f32) -> f32 {
        let weight = mass_kg.max(1.0) * 9.81;
        let now = self.downforce_front_n + self.downforce_rear_n;
        let clean = self.downforce_front_n / self.wake.downforce_front.max(0.1)
            + self.downforce_rear_n / self.wake.downforce_rear.max(0.1);
        ((weight + now) / (weight + clean)).clamp(0.0, 1.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn body(x: f32, y: f32, speed: f32) -> Body {
        Body {
            x,
            y,
            vx: speed,
            vy: 0.0,
            yaw: 0.0,
            length: 5.0,
            width: 2.0,
            drag_area: 1.4,
        }
    }

    #[test]
    fn right_behind_is_the_strongest_and_it_fades_with_the_gap() {
        let leader = body(100.0, 0.0, 60.0);
        let at = |gap: f32| wake_of(&leader, &body(100.0 - 5.0 - gap, 0.0, 60.0));
        let (tow0, dirty0) = at(0.0);
        assert!((tow0 - TOW_MAX).abs() < 1e-5 && (dirty0 - DIRTY_AIR_MAX).abs() < 1e-5);
        let (tow20, dirty20) = at(20.0);
        let (tow60, dirty60) = at(60.0);
        assert!(tow0 > tow20 && tow20 > tow60 && tow60 > 0.0);
        // The dirty air goes much sooner than the tow.
        assert!(dirty20 / dirty0 < tow20 / tow0);
        assert!(dirty60 < 0.01, "{dirty60}");
        assert_eq!(at(WAKE_REACH_M + 1.0), (0.0, 0.0));
    }

    #[test]
    fn nothing_ahead_of_the_leader_or_beside_it() {
        let leader = body(100.0, 0.0, 60.0);
        assert_eq!(wake_of(&leader, &body(120.0, 0.0, 60.0)), (0.0, 0.0));
        let (beside, _) = wake_of(&leader, &body(80.0, 6.0, 60.0));
        let (behind, _) = wake_of(&leader, &body(80.0, 0.0, 60.0));
        assert!(beside < 0.1 * behind, "{beside} vs {behind}");
    }

    #[test]
    fn a_slow_car_leaves_no_wake() {
        let leader = body(100.0, 0.0, 5.0);
        assert_eq!(wake_of(&leader, &body(90.0, 0.0, 5.0)), (0.0, 0.0));
    }

    #[test]
    fn a_bigger_car_punches_a_bigger_hole() {
        let small = body(100.0, 0.0, 60.0);
        let big = Body {
            drag_area: 2.8,
            ..small
        };
        let follower = body(80.0, 0.0, 60.0);
        assert!(wake_of(&big, &follower).0 > wake_of(&small, &follower).0);
    }

    #[test]
    fn the_field_is_read_before_it_is_written() {
        use crate::data::GridSlot;
        let config = CarConfig::default();
        let configs: HashMap<CarConfigId, CarConfig> = [(config.id, config.clone())].into();
        let mut cars = BTreeMap::new();
        for (i, x) in [(1u128, 100.0f32), (2, 90.0), (3, 80.0)] {
            let id = uuid::Uuid::from_u128(i);
            let mut s = CarState::new(
                id,
                config.id,
                &GridSlot {
                    position: i as u8,
                    x,
                    y: 0.0,
                    z: 0.0,
                    yaw_rad: 0.0,
                },
            );
            s.vel_x = 60.0;
            cars.insert(id, s);
        }
        update(&mut cars, &configs);
        let wake = |i: u128| cars[&uuid::Uuid::from_u128(i)].wake;
        assert_eq!(wake(1), Wake::CLEAN, "the leader is in clean air");
        assert!(wake(2).tow() > 0.2 && wake(3).tow() > 0.2);
        assert!(wake(2).downforce_front < wake(2).downforce_rear);
        // Nothing of last tick's wake leaks into this one: the same field
        // from clean air gives the same answer.
        let mut fresh = cars.clone();
        for s in fresh.values_mut() {
            s.wake = Wake::CLEAN;
        }
        update(&mut fresh, &configs);
        for (id, s) in &cars {
            assert_eq!(s.wake, fresh[id].wake);
        }
    }

    #[test]
    fn the_load_share_is_what_the_wake_took_off_the_downforce() {
        let mut s = CarState::new(
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
        s.downforce_front_n = 4000.0;
        s.downforce_rear_n = 4000.0;
        assert_eq!(s.wake_load_share(800.0), 1.0);
        s.wake = Wake {
            drag: 0.8,
            downforce_front: 0.5,
            downforce_rear: 1.0,
        };
        // 4000 N of front downforce would be 8000 in clean air.
        let weight = 800.0 * 9.81;
        let expected = (weight + 8000.0) / (weight + 12000.0);
        assert!((s.wake_load_share(800.0) - expected).abs() < 1e-5);
    }
}
