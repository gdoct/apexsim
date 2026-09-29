//! Pit stops: the lane, the limiter, the box and the service.
//!
//! The track editor bakes each circuit's pit lane into `<Stem>.pit.msgpack`
//! beside its YAML (`ats-export`): the lane's centerline, where it leaves and
//! rejoins the track, the speed-limit stretch between the painted lines and
//! a stop spot in front of every garage. The server loads it onto the track
//! (`TrackConfig::pit_lane`); a circuit without one has no pit stops.
//!
//! Once a tick, before the physics (`GameSession::update_pits`), every car is
//! placed on the lane ([`PitState`]):
//!
//! - **The limiter**: in the lane between the lines, the physics holds the
//!   car to the lane's limit (`physics::update_car_3d` cuts the throttle).
//! - **The box**: a car stopped at its own box's spot (its grid position's
//!   box, teams share) is serviced for [`service_seconds`], held still by
//!   the physics: a new set of the compound its driver chose (the setup's
//!   `tyre_compound`, or the AI's plan), fuel for the rest of the race where
//!   the rules allow refuelling (not an F1), and repairs.
//! - **The AI** decides in a race whether it needs a stop ([`plan_stop`]:
//!   worn tyres, a short tank, a damaged car, laps enough left to gain) and
//!   drives the lane itself ([`drive_input`]): onto the lane before the
//!   entry, to its box at the limit, stopped for the service, out at the
//!   limit and back onto the track past the exit.
//!
//! Everything is a pure function of the positions and the session clock, so
//! the sim stays deterministic.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::data::{CarConfig, CarState, PlayerInputData};

/// A car stops "at its box" within this distance of the spot, m.
pub const BOX_RADIUS_M: f32 = 2.5;
/// And below this speed, m/s.
pub const BOX_STOP_SPEED_MPS: f32 = 1.0;
/// Tyre change, s: an F1 crew, and everyone else's.
pub const F1_TYRE_CHANGE_S: f32 = 2.5;
pub const TYRE_CHANGE_S: f32 = 9.0;
/// Refuelling rate, L/s (an endurance rig).
pub const REFUEL_LPS: f32 = 2.0;
/// Repairs, s per percent of damage summed over the car's zones.
pub const REPAIR_S_PER_PERCENT: f32 = 0.04;

/// A car wants a stop when a tyre is this worn, percent.
pub const AI_PIT_WEAR: f32 = 70.0;
/// Or any body zone is this damaged, percent.
pub const AI_PIT_DAMAGE: f32 = 25.0;
/// Or its engine is, percent: power it would lose for the rest of the race.
pub const AI_PIT_ENGINE_DAMAGE: f32 = 20.0;
/// It turns onto the pit route this far before the lane leaves the track, m.
pub const AI_PIT_APPROACH_M: f32 = 250.0;

/// One box: where a car stops for service.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct PitBox {
    pub lane_station_m: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
}

/// The pit lane, as `ats-export` bakes it (`PitSidecar` in track-core).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PitLane {
    pub version: u32,
    pub width_m: f32,
    pub speed_limit_mps: f32,
    /// +1 when the lane lies on the track's left, -1 on its right.
    pub lane_side: i32,
    pub nodes: Vec<[f32; 3]>,
    pub length_m: f32,
    pub limit_start_m: f32,
    pub limit_end_m: f32,
    /// Track stations nearest the lane's first and last node.
    pub entry_station_m: f32,
    pub exit_station_m: f32,
    pub boxes: Vec<PitBox>,
    /// Each node's station along the lane, m (worked out on load).
    #[serde(skip)]
    pub stations: Vec<f32>,
}

/// Where a car is against the lane.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LanePoint {
    pub node: usize,
    /// Station along the lane, m (clamped to it).
    pub station_m: f32,
    /// Distance from the lane's middle, m.
    pub distance_m: f32,
}

impl PitLane {
    /// `Monza.yaml` -> `Monza.pit.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        let stem = track_file
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        track_file.with_file_name(format!("{stem}.pit.msgpack"))
    }

    pub fn load(path: &Path) -> Result<PitLane, String> {
        let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
        let mut lane: PitLane = rmp_serde::from_slice(&bytes).map_err(|e| e.to_string())?;
        lane.finish()?;
        Ok(lane)
    }

    /// Work out the stations, and refuse a lane nothing can drive.
    pub fn finish(&mut self) -> Result<(), String> {
        if self.nodes.len() < 2 || self.boxes.is_empty() {
            return Err("a pit lane needs a line and a box".into());
        }
        let mut at = 0.0f32;
        self.stations = Vec::with_capacity(self.nodes.len());
        self.stations.push(0.0);
        for pair in self.nodes.windows(2) {
            at += ((pair[1][0] - pair[0][0]).powi(2) + (pair[1][1] - pair[0][1]).powi(2)).sqrt();
            self.stations.push(at);
        }
        Ok(())
    }

    /// The car at `(x, y)` against the lane, searching near `hint` (the
    /// last tick's node) when there is one.
    pub fn locate(&self, x: f32, y: f32, hint: Option<u32>) -> LanePoint {
        let n = self.nodes.len();
        let (lo, hi) = match hint {
            Some(h) => {
                let h = (h as usize).min(n - 1);
                (h.saturating_sub(12), (h + 12).min(n - 1))
            }
            None => (0, n - 1),
        };
        let mut best = (0usize, f32::MAX, 0.0f32);
        for i in lo..hi.max(lo + 1).min(n - 1) {
            let a = self.nodes[i];
            let b = self.nodes[i + 1];
            let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
            let len2 = (dx * dx + dy * dy).max(1e-6);
            let t = (((x - a[0]) * dx + (y - a[1]) * dy) / len2).clamp(0.0, 1.0);
            let (px, py) = (a[0] + dx * t, a[1] + dy * t);
            let d2 = (x - px).powi(2) + (y - py).powi(2);
            if d2 < best.1 {
                let station = self.stations[i] + t * (self.stations[i + 1] - self.stations[i]);
                best = (i, d2, station);
            }
        }
        LanePoint {
            node: best.0,
            station_m: best.2,
            distance_m: best.1.sqrt(),
        }
    }

    /// The lane's middle at `station`, m.
    pub fn point_at(&self, station: f32) -> (f32, f32) {
        let s = station.clamp(0.0, *self.stations.last().unwrap_or(&0.0));
        let i = self
            .stations
            .partition_point(|&v| v <= s)
            .clamp(1, self.nodes.len() - 1);
        let (a, b) = (self.nodes[i - 1], self.nodes[i]);
        let span = (self.stations[i] - self.stations[i - 1]).max(1e-6);
        let t = ((s - self.stations[i - 1]) / span).clamp(0.0, 1.0);
        (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    }

    /// The box a car on grid slot `grid_position` stops at: one each while
    /// there are enough, shared beyond that.
    pub fn box_for(&self, grid_position: u8) -> &PitBox {
        &self.boxes[(grid_position.max(1) as usize - 1) % self.boxes.len()]
    }
}

/// A car against the pit lane, and its stop.
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct PitState {
    /// The lane node nearest the car last tick (the search's hint).
    #[serde(default)]
    pub lane_node: Option<u32>,
    /// Station along the lane, m.
    #[serde(default)]
    pub lane_station_m: f32,
    /// On the lane (within its width).
    #[serde(default)]
    pub in_lane: bool,
    /// Between the limit lines: the physics holds the car to the limit.
    #[serde(default)]
    pub limiter: bool,
    /// Stopped at its box and being worked on.
    #[serde(default)]
    pub servicing: bool,
    #[serde(default)]
    pub service_left_s: f32,
    /// This visit's service is done (until the car leaves the lane).
    #[serde(default)]
    pub serviced: bool,
    /// The AI wants a stop, and the compound it will take.
    #[serde(default)]
    pub wants_stop: bool,
    #[serde(default)]
    pub next_compound: u8,
    /// The AI is driving the pit route.
    #[serde(default)]
    pub driving: bool,
    /// Stops made this session.
    #[serde(default)]
    pub stops: u16,
    /// The service under way: the compound going on and the fuel going in.
    #[serde(default)]
    pub service_compound: u8,
    #[serde(default)]
    pub service_fuel_l: f32,
}

/// What a stop will do: the compound fitted, the fuel added, and the damage
/// repaired, and how long it takes.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Service {
    pub compound: u8,
    pub fuel_added_l: f32,
    pub repair: bool,
    pub seconds: f32,
}

/// Whether the rules let this car refuel at a stop: not an F1 car.
pub fn refuelling_allowed(config: &CarConfig) -> bool {
    config.class != "F1"
}

/// How long a stop takes: the tyre change (an F1 crew is quicker), then the
/// fuel (not at the same time, as endurance rules have it), then the
/// repairs.
pub fn service_seconds(config: &CarConfig, fuel_added_l: f32, damage_percent: f32) -> f32 {
    let tyres = if config.class == "F1" {
        F1_TYRE_CHANGE_S
    } else {
        TYRE_CHANGE_S
    };
    tyres + fuel_added_l.max(0.0) / REFUEL_LPS + damage_percent.max(0.0) * REPAIR_S_PER_PERCENT
}

/// The car's damage summed over its zones, percent.
pub fn damage_percent(state: &CarState) -> f32 {
    let d = &state.damage;
    d.front_damage_percent
        + d.rear_damage_percent
        + d.left_damage_percent
        + d.right_damage_percent
        + d.engine_damage_percent
}

/// Whether an AI driver in a race wants to stop, and on which compound:
/// `laps_left` after the one it is on, `lap_fuel_l` a lap's fuel.
pub fn plan_stop(state: &CarState, laps_left: u32, lap_fuel_l: Option<f32>) -> Option<u8> {
    if laps_left == 0 {
        return None;
    }
    let worn = state
        .tires
        .each()
        .iter()
        .any(|t| t.wear_percent >= AI_PIT_WEAR);
    let d = &state.damage;
    let damaged = [
        d.front_damage_percent,
        d.rear_damage_percent,
        d.left_damage_percent,
        d.right_damage_percent,
    ]
    .iter()
    .any(|z| *z >= AI_PIT_DAMAGE)
        || d.engine_damage_percent >= AI_PIT_ENGINE_DAMAGE;
    let short = lap_fuel_l.is_some_and(|lap| state.fuel_liters < lap * (laps_left as f32 + 0.8));
    if !(worn || damaged || short) {
        return None;
    }
    // The softest that lasts the rest.
    Some(if laps_left >= 15 {
        2
    } else if laps_left >= 6 {
        crate::tyre_thermal::MEDIUM
    } else {
        0
    })
}

/// Where the AI steers and how fast it goes on the pit route: pure pursuit
/// along the lane (onto its box's spot at the end), the limit between the
/// lines, a stop at the box, out at the limit. The automatic gearbox does
/// the shifting (`GameSession` switches it on for the route).
pub fn drive_input(
    lane: &PitLane,
    pit_box: &PitBox,
    state: &CarState,
    config: &CarConfig,
) -> PlayerInputData {
    const STOP_DECEL: f32 = 4.0;
    let v = state.speed_mps;
    let s = state.pit.lane_station_m;
    let look = (4.0 + 0.35 * v).clamp(6.0, 25.0);
    let to_box = pit_box.lane_station_m - s;
    let heading_to_box = !state.pit.serviced;

    // Where to aim: along the lane, then the box's spot once it is near.
    let (mut tx, mut ty) = lane.point_at(s + look);
    if heading_to_box && to_box < look + 6.0 {
        tx = pit_box.x;
        ty = pit_box.y;
    }
    let (c, sn) = (state.yaw_rad.cos(), state.yaw_rad.sin());
    let (dx, dy) = (tx - state.pos_x, ty - state.pos_y);
    let local_x = dx * c + dy * sn;
    let local_y = -dx * sn + dy * c;
    let dist = (local_x * local_x + local_y * local_y).sqrt().max(1.0);
    let alpha = local_y.atan2(local_x.max(0.1));
    let steer_angle = (2.0 * config.wheelbase_m * alpha.sin()).atan2(dist);
    let steering = (steer_angle / config.max_steering_angle_rad.max(1e-3)).clamp(-1.0, 1.0);

    // How fast: down to the limit by the first line, held to it between,
    // stopping at the box; free past the last line on the way out.
    let limit = lane.speed_limit_mps - 0.5;
    let zone = if s < lane.limit_start_m {
        (limit * limit + 2.0 * STOP_DECEL * (lane.limit_start_m - s)).sqrt()
    } else if s <= lane.limit_end_m {
        limit
    } else {
        f32::INFINITY
    };
    let mut target = zone.min(45.0);
    if heading_to_box {
        let along = to_box.max(0.0);
        target = target.min((2.0 * STOP_DECEL * (along - 0.3).max(0.0)).sqrt());
    }
    let error = target - v;
    let (throttle, brake) = if heading_to_box && to_box < 0.8 {
        (0.0, 1.0)
    } else if error > 0.3 {
        ((error * 0.25).clamp(0.0, 1.0), 0.0)
    } else if error < -0.3 {
        (0.0, (-error * 0.3).clamp(0.0, 1.0))
    } else {
        (0.1, 0.0)
    };
    PlayerInputData {
        throttle,
        brake,
        steering,
        gear: None,
        clutch: Some(1.0),
        drs: false,
        headlights: None,
        flash: false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A straight lane along +X from 0 to 200 m, the limit between 40 and
    /// 160, boxes every 6 m from 70.
    pub(crate) fn straight_lane() -> PitLane {
        let mut lane = PitLane {
            version: 1,
            width_m: 12.0,
            speed_limit_mps: 80.0 / 3.6,
            lane_side: -1,
            nodes: (0..=100).map(|i| [i as f32 * 2.0, -20.0, 0.0]).collect(),
            length_m: 200.0,
            limit_start_m: 40.0,
            limit_end_m: 160.0,
            entry_station_m: 900.0,
            exit_station_m: 150.0,
            boxes: (0..10)
                .map(|i| PitBox {
                    lane_station_m: 73.0 + 6.0 * i as f32,
                    x: 73.0 + 6.0 * i as f32,
                    y: -24.4,
                    z: 0.0,
                    yaw_rad: 0.0,
                })
                .collect(),
            stations: Vec::new(),
        };
        lane.finish().unwrap();
        lane
    }

    #[test]
    fn a_car_is_placed_on_the_lane() {
        let lane = straight_lane();
        let at = lane.locate(51.0, -21.0, None);
        assert!((at.station_m - 51.0).abs() < 1e-3);
        assert!((at.distance_m - 1.0).abs() < 1e-3);
        // The hint finds the same place.
        assert_eq!(lane.locate(51.0, -21.0, Some(25)).station_m, at.station_m);
        assert_eq!(lane.point_at(51.0), (51.0, -20.0));
        assert_eq!(
            lane.point_at(900.0),
            (200.0, -20.0),
            "held to the lane's end"
        );
    }

    #[test]
    fn boxes_go_one_each_then_are_shared() {
        let lane = straight_lane();
        assert_eq!(lane.box_for(1).lane_station_m, 73.0);
        assert_eq!(lane.box_for(3).lane_station_m, 85.0);
        assert_eq!(
            lane.box_for(11).lane_station_m,
            73.0,
            "slot 11 shares box 1"
        );
    }

    #[test]
    fn a_stop_takes_the_tyres_then_the_fuel_then_the_repairs() {
        let gt = CarConfig {
            class: "GT3".into(),
            ..CarConfig::default()
        };
        let f1 = CarConfig {
            class: "F1".into(),
            ..CarConfig::default()
        };
        assert!(refuelling_allowed(&gt) && !refuelling_allowed(&f1));
        assert_eq!(service_seconds(&f1, 0.0, 0.0), F1_TYRE_CHANGE_S);
        assert!((service_seconds(&gt, 60.0, 50.0) - (TYRE_CHANGE_S + 30.0 + 2.0)).abs() < 1e-4);
    }

    #[test]
    fn the_ai_stops_for_worn_tyres_on_a_compound_that_lasts() {
        let mut state = CarState::new(
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
        assert_eq!(plan_stop(&state, 10, Some(2.0)), None, "fresh and full");
        state.tires.rear_left.wear_percent = 75.0;
        assert_eq!(
            plan_stop(&state, 20, Some(2.0)),
            Some(2),
            "a long run: hards"
        );
        assert_eq!(
            plan_stop(&state, 8, Some(2.0)),
            Some(crate::tyre_thermal::MEDIUM)
        );
        assert_eq!(plan_stop(&state, 3, Some(2.0)), Some(0), "a sprint: softs");
        assert_eq!(plan_stop(&state, 0, Some(2.0)), None, "not on the last lap");
        state.tires.rear_left.wear_percent = 0.0;
        state.fuel_liters = 5.0;
        assert!(plan_stop(&state, 4, Some(2.0)).is_some(), "short of fuel");
    }
}
