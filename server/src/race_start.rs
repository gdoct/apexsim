//! How a race gets away (`data::RaceRules`): the formation lap, the
//! rolling start, and false starts.
//!
//! - **Formation lap** (`formation_lap`, a standing start): when the
//!   countdown runs out the field drives a lap held to
//!   [`FORMATION_SPEED_MPS`] (`CarState::speed_cap_mps`), no overtaking, and
//!   each car is driven onto its own slot over the last [`APPROACH_M`]
//!   ([`approach_input`]) and stopped there. With the field home the lights
//!   count [`LIGHTS_SECONDS`] down to the start.
//! - **Rolling start** (`StartType::Rolling`): the same lap, and the race goes
//!   green as the leader comes back to the line, the field rolling.
//! - **False start** (`jump_start`): through the countdown a driver's car in
//!   gear with the throttle open is let go of ([`releases`]), and one that
//!   has moved [`JUMP_START_M`] off its slot when the lights go out has
//!   jumped the start.
//!
//! Everything here is worked out from the tick's car states and the grid,
//! with no chance and no clock.

use std::collections::{BTreeMap, BTreeSet};

use crate::data::{CarConfig, CarState, GridSlot, PlayerId, PlayerInputData, TrackConfig};

/// The formation lap's pace, m/s (108 km/h).
pub const FORMATION_SPEED_MPS: f32 = 30.0;
/// Over this last stretch to its slot, m, the server drives each car onto
/// it and stops it there.
pub const APPROACH_M: f32 = 150.0;
/// How hard a car slows onto its slot, m/s².
pub const APPROACH_DECEL_MPS2: f32 = 3.0;
/// This close to its slot and stopped, m, a car is home and put on it.
pub const HOME_M: f32 = 1.5;
/// Below this it has stopped, m/s.
pub const HOME_SPEED_MPS: f32 = 1.0;
/// Each car keeps this far behind the car starting ahead of it on the
/// lap, m (its middle to the other's): the field runs single file in grid
/// order. On its approach it may close up to where its slot is behind
/// that car's.
pub const FORMATION_GAP_M: f32 = 14.0;
/// The lights' countdown once the field is back on the grid, s.
pub const LIGHTS_SECONDS: u16 = 5;
/// The formation lap is given this many times its length at its pace, s,
/// plus [`FORMATION_SPARE_S`]; a car not home by then is put on its slot.
pub const FORMATION_TIME_SHARE: f32 = 1.6;
pub const FORMATION_SPARE_S: f32 = 60.0;
/// A car this far off its slot when the lights go out has jumped the
/// start, m.
pub const JUMP_START_M: f32 = 0.5;
/// A driver's car in gear with more throttle than this through the
/// countdown is let go of, to move if it will.
pub const JUMP_THROTTLE: f32 = 0.05;
/// Steering toward the slot: m of lateral error to rad (a Stanley steer,
/// as the pit route's), and the yaw rate damped, s.
const PATH_GAIN: f32 = 1.4;
const YAW_DAMPING_S: f32 = 0.25;

/// How this race's start stands, made afresh each time the field lines up.
#[derive(Debug, Clone, Default)]
pub struct RaceStart {
    /// The formation (or pace) lap under way.
    pub formation: Option<Formation>,
    /// This start's formation lap has been driven.
    pub lap_run: bool,
    /// Drivers' cars let go of through the countdown ([`releases`]).
    pub released: BTreeSet<PlayerId>,
    /// Cars that jumped the start, as they did.
    pub jumped: BTreeSet<PlayerId>,
}

/// What the race is doing as telemetry tells it (`CompactTelemetry.race_phase`).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RacePhase {
    /// The formation lap of a standing start.
    Formation = 1,
    /// The pace lap of a rolling start.
    PaceLap = 2,
}

/// One car on the formation lap.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FormationCar {
    /// Its grid slot: the order the field runs in.
    pub grid: u8,
    /// Where its slot is along the lap, m from the line (behind it
    /// negative).
    pub slot_m: f32,
    /// The lap still to drive back to its slot, m.
    pub left_m: f32,
    /// Its place on the lap last tick, m.
    pub last_progress_m: f32,
    /// Where on the road its slot is, m left of the centerline.
    pub slot_left_m: f32,
    /// Back on its slot (a standing start), held there.
    pub home: bool,
    /// The car's own automatic-gearbox setting, kept while the approach
    /// drives it on the automatic box (as the pit route does), given back
    /// on its slot.
    pub own_auto_gearbox: Option<bool>,
}

/// A formation lap under way.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Formation {
    /// The pace lap of a rolling start: green as the leader reaches the
    /// line, nobody stops.
    pub rolling: bool,
    pub started_tick: u32,
    /// The tick by which every car still out is put on its slot.
    pub deadline_tick: u32,
    pub cars: BTreeMap<PlayerId, FormationCar>,
}

impl Formation {
    /// The lap begins with every car on its slot: each has a lap of
    /// `total_m` to drive back to it.
    pub fn begin<'a>(
        cars: impl Iterator<Item = &'a CarState>,
        track: &TrackConfig,
        total_m: f32,
        rolling: bool,
        tick: u32,
        tick_rate_hz: u16,
    ) -> Self {
        let lap_s = total_m / FORMATION_SPEED_MPS;
        let allowed_s = lap_s * FORMATION_TIME_SHARE + FORMATION_SPARE_S;
        Formation {
            rolling,
            started_tick: tick,
            deadline_tick: tick + (allowed_s * tick_rate_hz.max(1) as f32) as u32,
            cars: cars
                .map(|s| {
                    (
                        s.player_id,
                        FormationCar {
                            grid: s.grid_position,
                            slot_m: if s.track_progress > 0.5 * total_m {
                                s.track_progress - total_m
                            } else {
                                s.track_progress
                            },
                            left_m: total_m,
                            last_progress_m: s.track_progress,
                            slot_left_m: across_road(s, track).map_or(0.0, |(left, _)| left),
                            home: false,
                            own_auto_gearbox: None,
                        },
                    )
                })
                .collect(),
        }
    }

    /// Count each car's headway this tick off the lap it has left.
    pub fn advance(&mut self, state: &CarState, total_m: f32) {
        let Some(car) = self.cars.get_mut(&state.player_id) else {
            return;
        };
        if car.home || total_m <= 0.0 {
            return;
        }
        let moved = (state.track_progress - car.last_progress_m + 0.5 * total_m)
            .rem_euclid(total_m)
            - 0.5 * total_m;
        car.left_m -= moved;
        car.last_progress_m = state.track_progress;
    }

    /// The lap the car leading the field has left: the first in grid
    /// order still out.
    pub fn leader_left_m(&self) -> Option<f32> {
        self.cars
            .values()
            .filter(|c| !c.home)
            .min_by_key(|c| c.grid)
            .map(|c| c.left_m)
    }

    /// The pace each car still out is held to this tick, m/s: the formation
    /// pace (slowing onto its slot over the approach, in a standing start),
    /// and no closer than [`FORMATION_GAP_M`] behind the car starting ahead
    /// of it that is still in the lap (on the road, or home) at that car's
    /// speed: so the field gets away in grid order, one car after another,
    /// and runs single file. `field` is each car's speed and whether it is
    /// on the road and drivable.
    pub fn paces(
        &self,
        field: &BTreeMap<PlayerId, (f32, bool)>,
        total_m: f32,
    ) -> BTreeMap<PlayerId, f32> {
        let mut order: Vec<(&PlayerId, &FormationCar)> = self.cars.iter().collect();
        order.sort_by_key(|(_, c)| c.grid);
        let along = |c: &FormationCar| c.slot_m + total_m - c.left_m.max(0.0);
        let mut paces = BTreeMap::new();
        let mut ahead: Option<(&FormationCar, f32)> = None;
        for (id, car) in order {
            let (speed, running) = field.get(id).copied().unwrap_or((0.0, false));
            if car.home {
                ahead = Some((car, 0.0));
                continue;
            }
            let approaching = !self.rolling && car.left_m <= APPROACH_M;
            let mut pace = if self.rolling {
                FORMATION_SPEED_MPS
            } else {
                formation_cap_mps(car.left_m)
            };
            if let Some((front, front_speed)) = ahead {
                let gap = along(front) - along(car);
                let keep = if approaching {
                    (front.slot_m - car.slot_m).min(FORMATION_GAP_M)
                } else {
                    FORMATION_GAP_M
                };
                let room = (gap - keep).max(0.0);
                pace = pace.min(front_speed + (2.0 * APPROACH_DECEL_MPS2 * room).sqrt());
            }
            paces.insert(*id, pace);
            // A car off the road or out of it is no pace for the rest.
            if running {
                ahead = Some((car, speed));
            }
        }
        paces
    }

    /// Every car back on its slot.
    pub fn all_home(&self) -> bool {
        self.cars.values().all(|c| c.home)
    }
}

/// The pace a car on the formation lap with `left_m` to its slot is held
/// to: the formation pace, and over the approach slowing to a stop on it.
pub fn formation_cap_mps(left_m: f32) -> f32 {
    let stop = (2.0 * APPROACH_DECEL_MPS2 * (left_m - 0.5).max(0.0)).sqrt();
    FORMATION_SPEED_MPS.min(stop.max(0.0))
}

/// How the server drives a car over the last [`APPROACH_M`] onto its slot:
/// along the road at the slot's place across it (a Stanley steer on the
/// centerline's heading, as the pit route steers), at `pace`
/// ([`Formation::paces`]), stopping on the slot.
pub fn approach_input(
    state: &CarState,
    config: &CarConfig,
    track: &TrackConfig,
    car: &FormationCar,
    pace: f32,
) -> PlayerInputData {
    let v = state.speed_mps.max(0.0);
    let (left, heading) = across_road(state, track).unwrap_or((car.slot_left_m, state.yaw_rad));
    let heading_error = (heading - state.yaw_rad + std::f32::consts::PI)
        .rem_euclid(std::f32::consts::TAU)
        - std::f32::consts::PI;
    let steer_angle = heading_error + (-PATH_GAIN * (left - car.slot_left_m)).atan2(v + 2.0)
        - YAW_DAMPING_S * state.angular_vel_yaw;
    let steering = (steer_angle / config.max_steering_angle_rad.max(1e-3)).clamp(-1.0, 1.0);
    let target = pace.min(formation_cap_mps(car.left_m));
    let (throttle, brake) = if car.left_m < 0.5 || target < 0.3 {
        (0.0, 1.0)
    } else {
        crate::pit::pedals(target, v)
    };
    PlayerInputData {
        throttle,
        brake,
        steering,
        gear: None,
        clutch: Some(1.0),
        ..Default::default()
    }
}

/// Where a car is across the road, m left of the centerline, and the
/// road's heading there; `None` before it has been placed on the lap.
pub fn across_road(state: &CarState, track: &TrackConfig) -> Option<(f32, f32)> {
    let idx = state.nearest_centerline_idx? as usize;
    if idx >= track.centerline.len() {
        return None;
    }
    let (point, right) =
        crate::physics::centerline_at(&track.centerline, idx, state.pos_x, state.pos_y);
    Some((-right, point.heading_rad))
}

/// Whether a driver's car held through the countdown is let go of: in
/// gear, the throttle open.
pub fn releases(state: &CarState, input: &PlayerInputData) -> bool {
    state.gear != 0 && input.throttle > JUMP_THROTTLE
}

/// Whether a car let go of has moved off its `slot` far enough to have
/// jumped the start.
pub fn jumped(state: &CarState, slot: &GridSlot) -> bool {
    let (dx, dy) = (state.pos_x - slot.x, state.pos_y - slot.y);
    (dx * dx + dy * dy).sqrt() > JUMP_START_M
}

/// Put a car's race back to its start: lap 0, nothing timed, its place on
/// the lap found again. Its tyres, fuel, temperatures and damage stay as
/// the formation lap left them.
pub fn reset_laps(state: &mut CarState, track: &TrackConfig) {
    state.current_lap = 0;
    state.lap_start_tick = 0;
    state.current_lap_time_ms = 0;
    state.last_lap_time_ms = None;
    state.best_lap_time_ms = None;
    state.finish_position = None;
    state.next_checkpoint = 0;
    state.laps = Default::default();
    state.nearest_centerline_idx = None;
    crate::physics::seed_track_progress(state, track);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_cap_slows_a_car_to_a_stop_on_its_slot() {
        assert_eq!(formation_cap_mps(1000.0), FORMATION_SPEED_MPS);
        assert_eq!(formation_cap_mps(0.4), 0.0);
        let at_50 = formation_cap_mps(50.0);
        assert!(at_50 < FORMATION_SPEED_MPS && at_50 > 10.0, "{at_50}");
    }

    #[test]
    fn headway_is_counted_across_the_line() {
        let total = 5000.0;
        let mut state = CarState::new(
            uuid::Uuid::from_u128(1),
            uuid::Uuid::nil(),
            &GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        );
        state.track_progress = 4990.0;
        let track = TrackConfig::default();
        let mut f = Formation::begin(std::iter::once(&state), &track, total, false, 0, 100);
        state.track_progress = 10.0;
        f.advance(&state, total);
        let car = f.cars[&state.player_id];
        assert!((car.left_m - (total - 20.0)).abs() < 1e-3, "{car:?}");
        assert_eq!(f.leader_left_m(), Some(car.left_m));
        assert!(!f.all_home());
    }
}
