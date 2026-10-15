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
//! - **The box**: a car stopped at its own box's spot (dealt by
//!   [`deal_box`]: the lowest box nobody holds) is serviced for [`service_seconds`], held still by
//!   the physics: a new set of the compound its driver chose (the setup's
//!   `tyre_compound`, or the AI's plan), fuel for the rest of the race where
//!   the rules allow refuelling (not an F1), and repairs.
//! - **The AI** decides in a race whether it needs a stop ([`plan_stop`]:
//!   worn tyres, a short tank, a damaged car, laps enough left to gain; a
//!   splash of fuel alone late on; an undercut or a cover of the cars
//!   around it),
//!   races the run-up on the road at a speed it can take the lane at
//!   ([`run_up_input`]), and from just short of the lane drives it itself
//!   ([`drive_input`]): to its box at the limit, stopped for the service, out
//!   at the limit and back onto the track past the exit.
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
/// Or a tyre carries a flat spot this deep (0..1): the shake costs lap
/// time and the driver fears for the carcass.
pub const AI_PIT_FLAT_SPOT: f32 = 0.6;
/// Or any body zone is this damaged, percent.
pub const AI_PIT_DAMAGE: f32 = 25.0;
/// Or its engine is, percent: power it would lose for the rest of the race.
pub const AI_PIT_ENGINE_DAMAGE: f32 = 20.0;
/// Or, when the tank will not reach the flag, once it holds less than this
/// many laps: the car turns in before the next lap it could not finish,
/// as a crew runs a stint to the end of the fuel. Asked anywhere on the
/// lap, so it reaches the lane with at least the margin over a lap left.
/// (Stopping as soon as the race needed more than the tank put a GT3 at Le
/// Mans in on lap 1 of 9 with 50 litres aboard.)
pub const AI_PIT_FUEL_LAPS: f32 = 1.6;
/// A car further than this from the lane's bounding box is not looked for
/// on it, m; within it, a hint further off the lane than this is stale.
pub const LANE_SEARCH_MARGIN_M: f32 = 30.0;
/// It turns onto the pit route this far before the lane leaves the track, m.
pub const AI_PIT_APPROACH_M: f32 = 250.0;
/// It races on the road up to this far before the lane leaves the track, m,
/// and only then hands the wheel to the lane ([`drive_input`]).
pub const AI_PIT_HANDOVER_M: f32 = 10.0;
/// How hard the AI slows on the pit route, m/s²: to the limit by the first
/// line, to a stop at its box, and on the run-up to the lane's own speed.
const LANE_DECEL: f32 = 4.0;
/// A human's car is taken over within this far past the pit entry line, m
/// (a car that crossed it pointing across the lane is taken once it
/// straightens).
pub const TAKEOVER_WINDOW_M: f32 = 40.0;
/// And handed back this far past the pit exit line, m: past it the lane is
/// the driver's again, as is the exit taper back onto the track.
pub const HANDBACK_PAST_LINE_M: f32 = 1.0;
/// The fastest it drives any of the pit route, m/s.
const LANE_TOP_SPEED_MPS: f32 = 45.0;

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
    /// The lane's bounding box, `[min x, min y, max x, max y]` (on load).
    #[serde(skip)]
    pub bounds: [f32; 4],
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
        self.bounds = self
            .nodes
            .iter()
            .fold([f32::MAX, f32::MAX, f32::MIN, f32::MIN], |b, n| {
                [
                    b[0].min(n[0]),
                    b[1].min(n[1]),
                    b[2].max(n[0]),
                    b[3].max(n[1]),
                ]
            });
        let mut at = 0.0f32;
        self.stations = Vec::with_capacity(self.nodes.len());
        self.stations.push(0.0);
        for pair in self.nodes.windows(2) {
            at += ((pair[1][0] - pair[0][0]).powi(2) + (pair[1][1] - pair[0][1]).powi(2)).sqrt();
            self.stations.push(at);
        }
        Ok(())
    }

    /// Whether `(x, y)` is within `margin` of the lane's bounding box: a car
    /// further out is nowhere near it, and is not looked for on it.
    pub fn near(&self, x: f32, y: f32, margin: f32) -> bool {
        let b = self.bounds;
        x >= b[0] - margin && y >= b[1] - margin && x <= b[2] + margin && y <= b[3] + margin
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
    /// The service under way repairs the car, and fits new brake pads.
    #[serde(default)]
    pub service_repair: bool,
    #[serde(default)]
    pub service_pads: bool,
    /// New discs (and pads) go on at this stop.
    #[serde(default)]
    pub service_discs: bool,
    /// The car's own box (0-based), dealt the first time the car is placed
    /// against the lane: the lowest box no other car holds.
    #[serde(default)]
    pub box_index: Option<u8>,
    /// The pit exit light is red this tick (the same for every car).
    #[serde(default)]
    pub exit_closed: bool,
    /// Held at the red exit light, and for how long, s.
    #[serde(default)]
    pub held: bool,
    #[serde(default)]
    pub held_s: f32,
    /// A human's automatic gearbox and steering aid as they were when the
    /// pit autopilot took the car, put back when it hands it back.
    #[serde(default)]
    pub restore_aids: Option<[bool; 2]>,
    /// How long the route has made no headway, s ([`AUTOPILOT_STUCK_S`]).
    #[serde(default)]
    pub stuck_s: f32,
    /// The AI's stop is a splash of fuel alone: no tyres, no repairs
    /// ([`StopPlan::fuel_only`]).
    #[serde(default)]
    pub fuel_only: bool,
    /// The laps the car had run when this set of tyres went on
    /// (`GameSession::laps_run`): how fast they wear.
    #[serde(default)]
    pub stint_start_laps: f32,
}

/// Less fuel than this is not worth stopping the crew for, L.
const MIN_REFUEL_L: f32 = 0.5;
/// Less damage than this (summed over the zones, percent) is left alone.
const MIN_REPAIR_PERCENT: f32 = 0.5;

/// What a stop does and how long each part takes, in the order the crew
/// does them: the tyres (an F1 crew is quicker), then the fuel (not at the
/// same time, as endurance rules have it), then the repairs, new brake
/// pads included when the old ones are past `brakes::PAD_CHANGE_PCT` and
/// new discs (with pads) when the old ones are past
/// `brakes::DISC_CHANGE_PCT` (their time is counted in `repair_s`: the
/// client's panel knows three parts). A part the stop does not need takes
/// 0 s.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ServicePlan {
    pub tyres_s: f32,
    pub fuel_s: f32,
    pub fuel_l: f32,
    pub repair_s: f32,
    pub repair_pct: f32,
    /// New brake pads go on.
    pub pads: bool,
    /// New discs go on, and pads with them.
    pub discs: bool,
}

impl ServicePlan {
    pub fn total_s(&self) -> f32 {
        self.tyres_s + self.fuel_s + self.repair_s
    }
}

/// Whether the rules let this car refuel at a stop: not an F1 car.
pub fn refuelling_allowed(config: &CarConfig) -> bool {
    config.class != "F1"
}

/// The stop for a car that wants `fuel_l` more fuel and carries
/// `damage_percent` of damage (summed over its zones), its most worn brake
/// `brake_wear_pct` gone and its most worn disc `disc_wear_pct`.
pub fn plan_service(
    config: &CarConfig,
    fuel_l: f32,
    damage_percent: f32,
    brake_wear_pct: f32,
    disc_wear_pct: f32,
) -> ServicePlan {
    let tyres_s = if config.class == "F1" {
        F1_TYRE_CHANGE_S
    } else {
        TYRE_CHANGE_S
    };
    let fuel_l = if fuel_l >= MIN_REFUEL_L { fuel_l } else { 0.0 };
    let repair_pct = if damage_percent >= MIN_REPAIR_PERCENT {
        damage_percent
    } else {
        0.0
    };
    let discs = disc_wear_pct >= crate::brakes::DISC_CHANGE_PCT;
    let pads = discs || brake_wear_pct >= crate::brakes::PAD_CHANGE_PCT;
    let brakes_s = if discs {
        crate::brakes::DISC_CHANGE_S
    } else if pads {
        crate::brakes::PAD_CHANGE_S
    } else {
        0.0
    };
    ServicePlan {
        tyres_s,
        fuel_s: fuel_l / REFUEL_LPS,
        fuel_l,
        repair_s: repair_pct * REPAIR_S_PER_PERCENT + brakes_s,
        repair_pct,
        pads,
        discs,
    }
}

/// How long a stop takes ([`plan_service`]), on brakes that need no pads.
pub fn service_seconds(config: &CarConfig, fuel_added_l: f32, damage_percent: f32) -> f32 {
    plan_service(config, fuel_added_l, damage_percent, 0.0, 0.0).total_s()
}

/// The fuel a car may still burn this stint, L: its tank, or what the
/// stint's energy left (`[fuel] stint_energy_mj`) holds where that is
/// less (the motor's share counted as fuel).
pub fn usable_fuel_liters(state: &CarState, config: &CarConfig) -> f32 {
    let left = config.fuel.stint_left_mj(state.stint_energy_used_mj);
    if left.is_finite() {
        state.fuel_liters.min(config.fuel.liters_for_mj(left))
    } else {
        state.fuel_liters
    }
}

/// The fuel worth carrying into a stint, L: `liters`, or no more than the
/// stint's energy (`[fuel] stint_energy_mj`) can burn.
pub fn stint_fuel_cap(config: &CarConfig, liters: f32) -> f32 {
    match config.fuel.stint_energy_mj {
        Some(mj) => liters.min(config.fuel.liters_for_mj(mj)),
        None => liters,
    }
}

/// The most worn of a car's brakes, percent.
pub fn brake_wear_percent(state: &CarState) -> f32 {
    state.brake_wear_pct.iter().copied().fold(0.0, f32::max)
}

/// The most worn of a car's discs, percent.
pub fn disc_wear_percent(state: &CarState) -> f32 {
    state
        .brake_disc_wear_pct
        .iter()
        .copied()
        .fold(0.0, f32::max)
}

/// The car's damage summed over its zones, percent: what the crew mends
/// (none when the damage is only drawn, `CarState::felt_damage`).
pub fn damage_percent(state: &CarState) -> f32 {
    let d = &state.felt_damage();
    d.front_damage_percent
        + d.rear_damage_percent
        + d.left_damage_percent
        + d.right_damage_percent
        + d.engine_damage_percent
}

/// How a driver saves fuel: seconds of coasting before each braking zone
/// (`AiDriverController::with_fuel_save`) and how hard it short-shifts
/// (`AiDriverController::with_short_shift`, 0..1).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct FuelSave {
    pub coast_s: f32,
    pub short_shift: f32,
}

impl FuelSave {
    pub const NONE: FuelSave = FuelSave {
        coast_s: 0.0,
        short_shift: 0.0,
    };
}

/// Seconds a driver coasts before each braking zone per share of the fuel
/// to the flag it is short: a coast of a second before each of a lap's
/// braking zones saves something like a tenth of its fuel, so 5% short is
/// a coast of about half a second, up to
/// [`crate::ai_driver::MAX_FUEL_SAVE_COAST_S`].
pub const FUEL_SAVE_COAST_S_PER_SHORT: f32 = 10.0;
/// Short-shifting starts at this share short and is full at the second:
/// coasting first, which costs the least time for the fuel it saves.
pub const SHORT_SHIFT_SHORT: (f32, f32) = (0.10, 0.25);
/// The most of the fuel to the flag a driver makes up by saving it, as a
/// share. A race that leaves a car short by more refuels it at its stops
/// (`GameSession::refuels`), whatever the class's rules.
pub const MAX_FUEL_SAVE_SHORT: f32 = 0.15;

/// How a driver `short` (a share of the fuel to the flag) saves it: coast
/// first, then short-shift as well.
pub fn fuel_save_for(short: f32) -> FuelSave {
    if short <= 0.0 {
        return FuelSave::NONE;
    }
    let (from, full) = SHORT_SHIFT_SHORT;
    FuelSave {
        coast_s: (short * FUEL_SAVE_COAST_S_PER_SHORT).min(crate::ai_driver::MAX_FUEL_SAVE_COAST_S),
        short_shift: ((short - from) / (full - from)).clamp(0.0, 1.0),
    }
}

/// The compound an AI picks for a stop, as a step off the car's reference:
/// the softest that lasts the rest of the race.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopPick {
    Hard,
    Reference,
    Soft,
}

impl StopPick {
    /// The setup knob's click for it (`TireConfig::compound_for_click`).
    pub fn click(self) -> i8 {
        match self {
            StopPick::Hard => -1,
            StopPick::Reference => 0,
            StopPick::Soft => 1,
        }
    }
}

/// A stop an AI wants: the compound, and whether it is a splash of fuel
/// alone (the tyres stay on: they reach the flag).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StopPlan {
    pub pick: StopPick,
    pub fuel_only: bool,
}

/// What an AI weighs a stop against besides its own car's state.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct StopContext {
    /// Laps left after the one it is on.
    pub laps_left: u32,
    /// A lap's fuel, for a car a stop refuels (`None` for one it does not).
    pub lap_fuel_l: Option<f32>,
    /// The fuel the car may still burn this stint, when less than its
    /// tank: a stint energy limit (`[fuel] stint_energy_mj`,
    /// [`usable_fuel_liters`]) runs out first. `None`: the tank.
    pub usable_fuel_l: Option<f32>,
    /// Saving fuel gets it to the flag ([`SAVE_TO_SKIP_SHORT`]): it makes
    /// no stop for fuel alone.
    pub save_to_flag: bool,
    /// Laps run on this set of tyres.
    pub stint_laps: f32,
    /// Seconds to the car ahead on the road.
    pub gap_ahead_s: Option<f32>,
    /// A car within [`AI_COVER_GAP_S`] behind has turned onto the pit
    /// route.
    pub rival_behind_stopping: bool,
    /// The driver's aggressiveness (0..1) and a number in 0..1 drawn for
    /// it this lap (`wind::hash01` of driver and lap).
    pub aggressiveness: f32,
    pub roll: f32,
}

/// A refuelled car short of the flag by no more than this share saves fuel
/// (`fuel_save_for`) rather than stopping for it: under
/// [`MAX_FUEL_SAVE_SHORT`], so the saving it starts reaches the flag.
pub const SAVE_TO_SKIP_SHORT: f32 = 0.10;
/// Worn tyres that will last the race stay on this many laps from the
/// flag: a stop would cost more than the grip gains.
pub const AI_STAY_OUT_LAPS: u32 = 3;
/// A tyre counts as lasting while it would reach the flag under this wear,
/// percent (it punctures worn through, at 100).
pub const AI_WEAR_TO_FLAG: f32 = 92.0;
/// A stop the car must make anyway is brought forward no closer to the flag
/// than this many laps.
pub const AI_STRATEGY_MIN_LAPS: u32 = 4;
/// Held up within this gap of the car ahead, s, a driver whose tyres are
/// [`AI_UNDERCUT_WEAR`] worn stops early to undercut it: new tyres and a
/// clear road while the car ahead runs on old ones. By a chance a lap from
/// the first (the timid) to their sum (the bold).
pub const AI_UNDERCUT_GAP_S: f32 = 1.2;
pub const AI_UNDERCUT_WEAR: f32 = 50.0;
pub const AI_UNDERCUT_CHANCE: (f32, f32) = (0.15, 0.45);
/// A car within this far behind turning in for its stop makes a driver
/// whose tyres are [`AI_COVER_WEAR`] worn cover it the next time by: or
/// the undercut puts the car behind ahead.
pub const AI_COVER_GAP_S: f32 = 2.5;
pub const AI_COVER_WEAR: f32 = 40.0;

/// The softest compound that lasts `laps_left`.
fn pick_for(laps_left: u32) -> StopPick {
    if laps_left >= 15 {
        StopPick::Hard
    } else if laps_left >= 6 {
        StopPick::Reference
    } else {
        StopPick::Soft
    }
}

/// The most worn tyre's wear, percent.
fn worst_wear(state: &CarState) -> f32 {
    state
        .tires
        .each()
        .iter()
        .map(|t| t.wear_percent)
        .fold(0.0, f32::max)
}

/// Whether the tyres reach the flag ([`AI_WEAR_TO_FLAG`]) at the rate they
/// have worn this stint; `false` before a lap of it says how fast that is.
pub fn tyres_reach_flag(state: &CarState, stint_laps: f32, laps_left: u32) -> bool {
    if stint_laps < 1.0 {
        return false;
    }
    let wear = worst_wear(state);
    let per_lap = wear / stint_laps;
    wear + per_lap * (laps_left as f32 + 1.0) < AI_WEAR_TO_FLAG
}

/// Whether an AI driver in a race wants to stop, on which compound, and
/// whether for fuel alone. A deep flat spot, a puncture (a leak counts,
/// before it is flat) or damage: always. Worn tyres: unless they reach the
/// flag within [`AI_STAY_OUT_LAPS`]. A short tank: a splash of fuel alone
/// when the tyres reach the flag, nothing when saving gets it there. And a
/// stop it must make anyway is brought forward to undercut a car it is
/// held up by, or to cover one behind that has turned in.
pub fn plan_stop(state: &CarState, ctx: &StopContext) -> Option<StopPlan> {
    let laps_left = ctx.laps_left;
    if laps_left == 0 {
        return None;
    }
    let pick = pick_for(laps_left);
    let full = Some(StopPlan {
        pick,
        fuel_only: false,
    });
    let tyre_fault = state
        .tires
        .each()
        .iter()
        .any(|t| t.flat_spot >= AI_PIT_FLAT_SPOT || t.punctured || t.leak_kpa_per_s > 0.0);
    let d = &state.felt_damage();
    let damaged = [
        d.front_damage_percent,
        d.rear_damage_percent,
        d.left_damage_percent,
        d.right_damage_percent,
    ]
    .iter()
    .any(|z| *z >= AI_PIT_DAMAGE)
        || d.engine_damage_percent >= AI_PIT_ENGINE_DAMAGE;
    if tyre_fault || damaged {
        return full;
    }
    let fuel_l = ctx.usable_fuel_l.unwrap_or(state.fuel_liters);
    let fuel_needed = |lap: f32| fuel_l < lap * (laps_left as f32 + 0.8);
    let lap_fuel = ctx.lap_fuel_l.filter(|_| !ctx.save_to_flag);
    let short = lap_fuel.is_some_and(|lap| fuel_needed(lap) && fuel_l < lap * AI_PIT_FUEL_LAPS);
    let tyres_last = tyres_reach_flag(state, ctx.stint_laps, laps_left);
    let wear = worst_wear(state);
    if wear >= AI_PIT_WEAR {
        if laps_left <= AI_STAY_OUT_LAPS && tyres_last && !short {
            return None;
        }
        return full;
    }
    if short {
        return Some(StopPlan {
            pick,
            fuel_only: tyres_last,
        });
    }
    // A stop it must make anyway, brought forward.
    let due = (ctx.stint_laps >= 2.0 && !tyres_last) || lap_fuel.is_some_and(fuel_needed);
    if !due || laps_left < AI_STRATEGY_MIN_LAPS {
        return None;
    }
    let cover = ctx.rival_behind_stopping && wear >= AI_COVER_WEAR;
    let (timid, bold) = AI_UNDERCUT_CHANCE;
    let undercut = ctx.gap_ahead_s.is_some_and(|g| g <= AI_UNDERCUT_GAP_S)
        && wear >= AI_UNDERCUT_WEAR
        && ctx.roll < timid + bold * ctx.aggressiveness.clamp(0.0, 1.0);
    if cover || undercut {
        return full;
    }
    None
}

/// A slick is the wrong tyre once the racing line holds this much water,
/// a treaded one once it holds less than [`TREADED_OFF_WATER`]: either way
/// the AI comes in for the track's tyre. The gap between is the
/// crossover's hysteresis, so a car does not stop again a lap later.
pub const SLICK_OFF_WATER: f32 = 0.25;
pub const TREADED_OFF_WATER: f32 = 0.06;
/// An intermediate gives way to a wet, and a wet to an intermediate, this
/// far either side of where the weather's tyre changes.
const TREADED_SWAP_MARGIN: f32 = 0.15;

/// What an AI weighs a change of tyres for the weather against, besides
/// the water on the line.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CrossoverContext {
    /// Laps left after the one it is on.
    pub laps_left: u32,
    /// Rain (or snow) falling now.
    pub raining: bool,
    /// Seconds until the forecast turns the sky wet, while it is dry, and
    /// dry, while it is wet; `None` when it does not within its reach.
    pub turn_in_s: Option<f32>,
    /// A lap, s.
    pub lap_s: f32,
    /// The driver's aggressiveness (0..1) and a number in 0..1 drawn for
    /// it this lap.
    pub aggressiveness: f32,
    pub roll: f32,
}

impl Default for CrossoverContext {
    /// A long race under a steady sky, a driver who never gambles.
    fn default() -> Self {
        Self {
            laps_left: u32::MAX,
            raining: false,
            turn_in_s: None,
            lap_s: 90.0,
            aggressiveness: 0.0,
            roll: 1.0,
        }
    }
}

/// This close to the flag a driver on the wrong tyres stays out (a stop
/// costs more than the laps), unless they are slicks in more than
/// [`SLICK_STAY_OUT_WATER`].
pub const CROSSOVER_STAY_OUT_LAPS: u32 = 2;
/// Slicks stay out in no more water than this, near the flag or with the
/// rain about to stop: past it they aquaplane.
pub const SLICK_STAY_OUT_WATER: f32 = 0.4;
/// A driver reads the forecast this many laps ahead: treaded tyres stay on
/// a drying line with rain due back within it, and a brave driver stays
/// out on slicks in the rain with the rain due to stop within one lap.
pub const FORECAST_LAPS: f32 = 2.0;
/// The gamble on slicks: on intermediates, with the rain stopped and none
/// due, the line under this much water, a driver takes slicks early by a
/// chance a lap of its aggressiveness times this.
pub const SLICK_GAMBLE_WATER: f32 = 0.14;
pub const SLICK_GAMBLE_CHANCE: f32 = 0.5;

/// The tyre an AI on `fitted` should stop for with `line_water` on the
/// racing line, if `fitted` is the wrong one for it: rain on slicks, a
/// drying line on treaded tyres, a downpour on intermediates. Read against
/// the race (`ctx`): no stop for it within [`CROSSOVER_STAY_OUT_LAPS`] of
/// the flag (slicks in a downpour aside), none off treaded tyres with rain
/// due back within [`FORECAST_LAPS`], a brave driver's slicks kept out in
/// the rain about to stop, and a brave driver's early slicks on a drying
/// line ([`SLICK_GAMBLE_WATER`]).
pub fn tyres_for_the_track(
    tire: &crate::data::TireConfig,
    fitted: u8,
    line_water: f32,
    ctx: &CrossoverContext,
) -> Option<u8> {
    use crate::tyre_thermal::{CompoundKind, WET_FROM_WATER};
    let kind = tire.compound(fitted).kind;
    let wrong = match kind {
        CompoundKind::Slick => line_water >= SLICK_OFF_WATER,
        CompoundKind::Intermediate => {
            !(TREADED_OFF_WATER..WET_FROM_WATER + TREADED_SWAP_MARGIN).contains(&line_water)
        }
        CompoundKind::Wet => {
            line_water < TREADED_OFF_WATER || line_water < WET_FROM_WATER - TREADED_SWAP_MARGIN
        }
    };
    let brave = ctx.roll < SLICK_GAMBLE_CHANCE * ctx.aggressiveness.clamp(0.0, 1.0);
    let forecast_s = FORECAST_LAPS * ctx.lap_s.max(1.0);
    if !wrong {
        // The gamble: slicks early on a line still damp.
        let gamble = kind == CompoundKind::Intermediate
            && line_water < SLICK_GAMBLE_WATER
            && !ctx.raining
            && ctx.turn_in_s.is_none_or(|s| s > forecast_s)
            && ctx.laps_left >= AI_STRATEGY_MIN_LAPS
            && brave;
        let slick = tire.reference_compound();
        return (gamble && tire.compound(slick).kind == CompoundKind::Slick).then_some(slick);
    }
    let slicks_hold = kind == CompoundKind::Slick && line_water < SLICK_STAY_OUT_WATER;
    // Nearly over: not worth the stop, unless slicks are aquaplaning.
    if ctx.laps_left <= CROSSOVER_STAY_OUT_LAPS && (kind != CompoundKind::Slick || slicks_hold) {
        return None;
    }
    // The forecast: rain due back keeps treaded tyres on a drying line ...
    let drying = kind != CompoundKind::Slick && line_water < TREADED_OFF_WATER;
    if drying && !ctx.raining && ctx.turn_in_s.is_some_and(|s| s <= forecast_s) {
        return None;
    }
    // ... and a brave driver keeps slicks out in rain about to stop.
    if slicks_hold && ctx.raining && brave && ctx.turn_in_s.is_some_and(|s| s <= ctx.lap_s) {
        return None;
    }
    let want = tire
        .weather_compound(line_water)
        .unwrap_or(tire.reference_compound());
    // A car with no treaded tyre stays out on its slicks.
    (tire.compound(want).kind != kind).then_some(want)
}

/// On the pit route but still short of the lane: how far along the track the
/// lane leaves it, m, while that is more than [`AI_PIT_HANDOVER_M`]. Until
/// then the car races on along the road ([`run_up_input`]); after it the
/// lane drives it ([`drive_input`]).
///
/// The lane used to drive the whole route, aiming from 250 m out at a point
/// just inside the lane's mouth: at Monza that chord cut the Parabolica so
/// gently that the car ran 20 m wide over the run-off, then swerved back
/// across the track into the lane at 37 m/s, across it, and into the armco
/// on its far side.
pub fn run_up_m(lane: &PitLane, state: &CarState, track_length_m: f32) -> Option<f32> {
    if !state.pit.driving || state.pit.serviced || track_length_m <= 0.0 {
        return None;
    }
    let to_lane = (lane.entry_station_m - state.track_progress).rem_euclid(track_length_m);
    (to_lane > AI_PIT_HANDOVER_M && to_lane <= AI_PIT_APPROACH_M + AI_PIT_HANDOVER_M)
        .then_some(to_lane)
}

/// The racing driver's own `input` on the run-up to the lane, `run_up_m`
/// short of it ([`run_up_m`]), held to a speed it can take the lane at: no
/// faster than slowing at the lane's rate from here would bring it to the
/// lane's entry speed by the handover. No DRS on the way into the pits.
pub fn run_up_input(
    lane: &PitLane,
    run_up_m: f32,
    speed_mps: f32,
    mut input: PlayerInputData,
) -> PlayerInputData {
    let entry = lane_speed_mps(lane, 0.0);
    let left = (run_up_m - AI_PIT_HANDOVER_M).max(0.0);
    let cap = (entry * entry + 2.0 * LANE_DECEL * left).sqrt();
    let (throttle, brake) = pedals(cap, speed_mps);
    input.throttle = input.throttle.min(throttle);
    input.brake = input.brake.max(brake);
    input.drs = false;
    input
}

/// How fast the lane has a car go at `s` along it on the way in: down to
/// the limit by the first line, held to it between, free past the last.
fn lane_speed_mps(lane: &PitLane, s: f32) -> f32 {
    let limit = lane.speed_limit_mps - 0.5;
    let zone = if s < lane.limit_start_m {
        (limit * limit + 2.0 * LANE_DECEL * (lane.limit_start_m - s)).sqrt()
    } else if s <= lane.limit_end_m {
        limit
    } else {
        f32::INFINITY
    };
    zone.min(LANE_TOP_SPEED_MPS)
}

/// Throttle and brake that bring `v` to `target`, m/s.
fn pedals(target: f32, v: f32) -> (f32, f32) {
    let error = target - v;
    if error > 0.3 {
        ((error * 0.25).clamp(0.0, 1.0), 0.0)
    } else if error < -0.3 {
        (0.0, (-error * 0.3).clamp(0.0, 1.0))
    } else {
        (0.1, 0.0)
    }
}

/// Another car on the lane, as the pit route sees it: where it is along
/// the lane and across it, and how fast it goes.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LaneCar {
    pub station_m: f32,
    pub lateral_m: f32,
    pub speed_mps: f32,
}

/// A car stays this far behind the one ahead of it on the route, m
/// (centre to centre).
const LANE_FOLLOW_GAP_M: f32 = 9.0;
/// Two cars this close across the lane share a path, m.
const LANE_SAME_PATH_M: f32 = 2.6;
/// The swing from the middle of the lane into the box, and back out, m of
/// lane.
const BOX_SWING_M: f32 = 20.0;
/// Speed through the swing into the box, m/s.
const BOX_SWING_SPEED_MPS: f32 = 8.0;
/// A held car stops this far short of the exit light, m.
const EXIT_STOP_SHORT_M: f32 = 2.0;
/// The exit light holds a car at most this long, s: a stream of traffic
/// never shuts a car in for good.
pub const EXIT_HOLD_MAX_S: f32 = 10.0;
/// The exit light goes red while a car on the track is this long, s (at
/// its speed), from where the lane rejoins it…
const EXIT_TRAFFIC_S: f32 = 3.0;
/// …or this close, m, whatever its speed.
const EXIT_TRAFFIC_M: f32 = 25.0;
/// Stanley gain on the distance off the path (per m/s).
const PATH_GAIN: f32 = 1.4;
/// The route's bends are taken at no more than this lateral acceleration,
/// m/s², looked for this far ahead, m.
const ROUTE_LATERAL_MPS2: f32 = 6.0;
const ROUTE_LOOKAHEAD_M: f32 = 60.0;
/// The most cornering the route asks of the tyres when it steers, m/s²,
/// and the slip angle on top of the kinematic lock, rad.
const ROUTE_GRIP_MPS2: f32 = 13.0;
const ROUTE_SLIP_RAD: f32 = 0.09;
/// Off the route by this much, m, or pointing this far across it, rad, the
/// car slows to half the limit, and to no less than ROUTE_WIDE_MPS, m/s,
/// further astray, while it steers back.
const ROUTE_WIDE_OFF_M: f32 = 4.0;
const ROUTE_WIDE_ANGLE_RAD: f32 = 0.4;
const ROUTE_WIDE_MPS: f32 = 9.0;
/// Steering taken off per rad/s of yaw rate the route does not ask for.
const YAW_DAMPING_S: f32 = 0.25;
/// A car on the pit route that has made no headway for this long, s (not
/// in service, not held at the light), is put back on its route
/// ([`recovery_pose`]).
pub const AUTOPILOT_STUCK_S: f32 = 4.0;
/// Short of its box by less than this, m, a stuck car is put on the box.
const RECOVER_TO_BOX_M: f32 = 8.0;

fn smoothstep(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

impl PitLane {
    /// The lane's middle and heading at `station`.
    pub fn frame_at(&self, station: f32) -> (f32, f32, f32) {
        let (x, y) = self.point_at(station);
        let (ax, ay) = self.point_at(station - 1.5);
        let (bx, by) = self.point_at(station + 1.5);
        (x, y, (by - ay).atan2(bx - ax))
    }

    /// `(x, y)` against the lane, and its distance across the lane from
    /// the middle, positive to the lane's left.
    pub fn lateral_of(&self, x: f32, y: f32, hint: Option<u32>) -> (LanePoint, f32) {
        let at = self.locate(x, y, hint);
        let (mx, my, heading) = self.frame_at(at.station_m);
        let lateral = -(x - mx) * heading.sin() + (y - my) * heading.cos();
        (at, lateral)
    }

    /// Box `index` (dealt by [`deal_box`]); shared round the row when a
    /// session holds more cars than the lane has boxes.
    pub fn box_at(&self, index: u8) -> &PitBox {
        &self.boxes[index as usize % self.boxes.len()]
    }

    /// How far across the lane a box's stop spot is from the middle, m,
    /// positive to the lane's left.
    pub fn box_lateral_m(&self, pit_box: &PitBox) -> f32 {
        let (mx, my, heading) = self.frame_at(pit_box.lane_station_m);
        -(pit_box.x - mx) * heading.sin() + (pit_box.y - my) * heading.cos()
    }
}

/// The box a car gets: the lowest one no car in `taken` holds; when every
/// box is held, the lowest of those held by the fewest.
pub fn deal_box(taken: &[u8], box_count: usize) -> u8 {
    let count = box_count.clamp(1, 255);
    let mut held = vec![0u32; count];
    for &b in taken {
        held[b as usize % count] += 1;
    }
    let least = held.iter().copied().min().unwrap_or(0);
    held.iter().position(|&n| n == least).unwrap_or(0) as u8
}

/// Whether the pit exit light is red: before the start of a race (the
/// lane is closed while the field lines up), or while a car on the track
/// is about to pass where the lane rejoins it, so nobody is sent out into
/// its path. `traffic` is every car on the track: its station along the
/// lap and its speed.
pub fn exit_closed(
    lane: &PitLane,
    before_the_start: bool,
    traffic: &[(f32, f32)],
    track_length_m: f32,
) -> bool {
    if before_the_start {
        return true;
    }
    if track_length_m <= 0.0 {
        return false;
    }
    traffic.iter().any(|&(station, speed)| {
        let to_exit = (lane.exit_station_m - station).rem_euclid(track_length_m);
        to_exit <= (speed * EXIT_TRAFFIC_S).max(EXIT_TRAFFIC_M)
    })
}

/// Whether a human's car has just crossed the pit entry line, so the pit
/// autopilot takes it over: on the lane, at or just past its first limit
/// line (the line painted across it, by the speed-limit board), past the
/// track's road edge on the lane's side by `off_road_m` (negative while it
/// is still on the road), and pointing down the lane. The entry taper up
/// to the line is the driver's: taken at the lane's mouth, the car was
/// driven over the road beside the racing cars.
pub fn takes_over(lane: &PitLane, at: &LanePoint, yaw_rad: f32, off_road_m: f32) -> bool {
    if at.distance_m > lane.width_m / 2.0 + 1.0 || off_road_m < 0.3 {
        return false;
    }
    let line = lane.limit_start_m.max(0.5);
    if at.station_m < line || at.station_m > line + TAKEOVER_WINDOW_M {
        return false;
    }
    let (_, _, heading) = lane.frame_at(at.station_m);
    (yaw_rad - heading).cos() > 0.3
}

/// Where the pit autopilot hands a car back, as a station along the lane:
/// a human's just past the exit line, the AI's at the lane's end (its
/// racing line knows nothing of the exit taper).
pub fn route_end_m(lane: &PitLane, is_ai: bool) -> f32 {
    let lane_end = lane.length_m - 3.0;
    if is_ai {
        lane_end
    } else {
        (lane.limit_end_m + HANDBACK_PAST_LINE_M).min(lane_end)
    }
}

/// Where the pit route runs across the lane at `s`, m from the middle
/// (positive left): down the middle, swinging over to the box's spot over
/// the last [`BOX_SWING_M`] before it, and back out over as much after.
fn route_lateral(s: f32, box_s: f32, box_lat: f32, serviced: bool) -> f32 {
    if !serviced {
        box_lat * smoothstep((s - (box_s - BOX_SWING_M)) / (BOX_SWING_M - 2.0))
    } else if s < box_s {
        box_lat
    } else {
        box_lat * (1.0 - smoothstep((s - box_s) / BOX_SWING_M))
    }
}

/// Where a car stuck on the pit route is put back: on its box's spot when
/// it is stuck short of it, else three metres on along its route, pointing
/// down the lane. A last resort: the route should never need it, but a
/// player's car must never be left shut in the pits.
pub fn recovery_pose(lane: &PitLane, pit_box: &PitBox, state: &CarState) -> (f32, f32, f32) {
    let s = state.pit.lane_station_m;
    if !state.pit.serviced && (pit_box.lane_station_m - s).abs() < RECOVER_TO_BOX_M {
        return (pit_box.x, pit_box.y, pit_box.yaw_rad);
    }
    let at = (s + 3.0).min(lane.length_m);
    let lat = route_lateral(
        at,
        pit_box.lane_station_m,
        lane.box_lateral_m(pit_box),
        state.pit.serviced,
    );
    let (x, y, heading) = lane.frame_at(at);
    (x - heading.sin() * lat, y + heading.cos() * lat, heading)
}

/// The route's curvature at `s`, 1/m: the lane's own, plus the swing.
fn route_curvature(lane: &PitLane, s: f32, box_s: f32, box_lat: f32, serviced: bool) -> f32 {
    let heading = |at: f32| {
        let (_, _, h) = lane.frame_at(at);
        let slope = (route_lateral(at + 1.0, box_s, box_lat, serviced)
            - route_lateral(at - 1.0, box_s, box_lat, serviced))
            / 2.0;
        h + slope.atan()
    };
    let turn = heading(s + 2.0) - heading(s - 2.0);
    let turn =
        (turn + std::f32::consts::PI).rem_euclid(std::f32::consts::TAU) - std::f32::consts::PI;
    turn / 4.0
}

/// How the server drives a car along the pit route, the AI's and, under
/// the pit autopilot, a human's: along the route ([`route_lateral`]) with a
/// Stanley steer (the route's heading, corrected for the distance off it
/// at the front axle), the limit between the lines, slowing through the
/// swing into the box and stopping on its spot, queueing behind a car
/// ahead on the same path, waiting at a red exit light, and free past the
/// last line. The automatic gearbox shifts and the steering aid is off for
/// the route (`GameSession`).
///
/// It used to be pure pursuit, aiming at the box's spot once it was near:
/// the target jumped sideways when it switched, and the car weaved ±5 m
/// down the lane.
pub fn drive_input(
    lane: &PitLane,
    pit_box: &PitBox,
    state: &CarState,
    config: &CarConfig,
    traffic: &[LaneCar],
) -> PlayerInputData {
    let v = state.speed_mps.max(0.0);
    let serviced = state.pit.serviced;
    let box_s = pit_box.lane_station_m;
    let box_lat = lane.box_lateral_m(pit_box);
    let (c, sn) = (state.yaw_rad.cos(), state.yaw_rad.sin());

    // The path at the front axle.
    let reach = config.wheelbase_m * 0.5;
    let (fx, fy) = (state.pos_x + c * reach, state.pos_y + sn * reach);
    let (front, lateral) = lane.lateral_of(fx, fy, state.pit.lane_node);
    let s = front.station_m;
    let want = route_lateral(s, box_s, box_lat, serviced);
    let slope = (route_lateral(s + 1.0, box_s, box_lat, serviced)
        - route_lateral(s - 1.0, box_s, box_lat, serviced))
        / 2.0;
    let (_, _, lane_heading) = lane.frame_at(s);
    let heading_error = (lane_heading + slope.atan() - state.yaw_rad + std::f32::consts::PI)
        .rem_euclid(std::f32::consts::TAU)
        - std::f32::consts::PI;
    // Damped by the yaw rate the route does not ask for: a car taken over
    // while it crosses into the lane's mouth at an angle otherwise swings
    // through the lane's middle onto its far edge.
    let path_yaw_rate = v * route_curvature(lane, s, box_s, box_lat, serviced);
    let steer_angle = heading_error + (-PATH_GAIN * (lateral - want)).atan2(v + 2.0)
        - YAW_DAMPING_S * (state.angular_vel_yaw - path_yaw_rate);
    // No more lock than the front tyres can use at this speed: past their
    // peak slip they slide and the car ploughs wide (the steering aid,
    // which would hold that, is off for the route).
    let usable = (config.wheelbase_m * ROUTE_GRIP_MPS2 / (v * v).max(1.0)).atan() + ROUTE_SLIP_RAD;
    let steer_angle = steer_angle.clamp(-usable, usable);
    let steering = (steer_angle / config.max_steering_angle_rad.max(1e-3)).clamp(-1.0, 1.0);

    // How fast: the limit between the lines, slow through the swing into
    // the box and stopped on its spot.
    let s_car = state.pit.lane_station_m;
    let mut target = lane_speed_mps(lane, s_car);
    // No faster than the route's own bends ahead allow: the entry and exit
    // tapers and the lane's curves, taken free past the last line, threw
    // a car wide onto the walls.
    let mut ahead = 0.0f32;
    while ahead <= ROUTE_LOOKAHEAD_M {
        let k = route_curvature(lane, s_car + ahead, box_s, box_lat, serviced).abs();
        if k > 1e-4 {
            let bend = (ROUTE_LATERAL_MPS2 / k).sqrt();
            target = target.min((bend * bend + 2.0 * LANE_DECEL * ahead).sqrt());
        }
        ahead += 3.0;
    }
    // Wide of the route, or pointing across it (a car taken over as it
    // cuts into the lane's mouth): slow down while the steer brings it
    // back.
    let astray =
        (lateral - want).abs() / ROUTE_WIDE_OFF_M + heading_error.abs() / ROUTE_WIDE_ANGLE_RAD;
    target = target.min((lane.speed_limit_mps * (1.0 - 0.5 * astray)).max(ROUTE_WIDE_MPS));
    let mut stop_here = false;
    if !serviced {
        let to_box = box_s - s_car;
        let to_stop = (2.0 * LANE_DECEL * (to_box - 0.3).max(0.0)).sqrt();
        if to_box < BOX_SWING_M + 10.0 {
            target = target.min(BOX_SWING_SPEED_MPS);
        }
        target = target.min(to_stop);
        stop_here = to_box < 0.6;
    }
    // A red exit light holds a car short of it, for a while.
    let to_light = lane.limit_end_m - EXIT_STOP_SHORT_M - s_car;
    if serviced
        && state.pit.exit_closed
        && state.pit.held_s < EXIT_HOLD_MAX_S
        && to_light > -EXIT_STOP_SHORT_M
    {
        target = target.min((2.0 * LANE_DECEL * to_light.max(0.0)).sqrt());
        stop_here |= to_light < 0.5;
    }
    // Queue behind a car ahead on the same path.
    let (_, my_lateral) = lane.lateral_of(state.pos_x, state.pos_y, state.pit.lane_node);
    for other in traffic {
        let ahead = other.station_m - s_car;
        if ahead > 0.5 && ahead < 40.0 && (other.lateral_m - my_lateral).abs() < LANE_SAME_PATH_M {
            let room = (ahead - LANE_FOLLOW_GAP_M).max(0.0);
            target = target.min(other.speed_mps + (2.0 * LANE_DECEL * room).sqrt());
            if room <= 0.0 && other.speed_mps < 1.0 {
                stop_here = true;
            }
        }
    }
    let (throttle, brake) = if stop_here {
        (0.0, 1.0)
    } else {
        pedals(target, v)
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
        ers_mode: None,
        ers_boost: false,
        handbrake: false,
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
            bounds: [0.0; 4],
        };
        lane.finish().unwrap();
        lane
    }

    #[test]
    fn the_autopilot_runs_from_the_entry_line_to_the_exit_line() {
        let lane = straight_lane();
        let take = |station: f32, off_road: f32| {
            let at = lane.locate(station, -20.0, None);
            takes_over(&lane, &at, 0.0, off_road)
        };
        assert!(!take(10.0, 3.0), "the entry taper is the driver's");
        assert!(!take(39.5, 3.0), "short of the line");
        assert!(take(40.2, 3.0), "over the line");
        assert!(
            take(70.0, 3.0),
            "a car that crossed it askew, once straight"
        );
        assert!(!take(40.2, -1.0), "not while on the road");
        assert!(!take(90.0, 3.0), "not deep in the lane");
        assert_eq!(
            route_end_m(&lane, false),
            161.0,
            "a human's: past the exit line"
        );
        assert_eq!(route_end_m(&lane, true), 197.0, "the AI's: the lane's end");
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
    fn every_car_gets_its_own_box_then_they_are_shared() {
        let lane = straight_lane();
        assert_eq!(deal_box(&[], 10), 0);
        assert_eq!(deal_box(&[0, 1, 3], 10), 2, "the lowest free box");
        let full: Vec<u8> = (0..10).collect();
        assert_eq!(deal_box(&full, 10), 0, "all held: shared from the first");
        assert_eq!(deal_box(&[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0], 10), 1);
        assert_eq!(lane.box_at(2).lane_station_m, 85.0);
        assert_eq!(lane.box_at(12).lane_station_m, 85.0, "round the row");
        assert!(
            (lane.box_lateral_m(lane.box_at(0)) + 4.4).abs() < 1e-3,
            "right of the lane"
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
        // Worn pads are changed with the repairs.
        let pads = plan_service(&gt, 0.0, 0.0, 70.0, 0.0);
        assert!(pads.pads && !pads.discs);
        assert!((pads.repair_s - crate::brakes::PAD_CHANGE_S).abs() < 1e-4);
        assert!(!plan_service(&gt, 0.0, 0.0, 30.0, 0.0).pads);
        // Worn discs go on with new pads, however worn the old pads are.
        let discs = plan_service(&gt, 0.0, 0.0, 10.0, 75.0);
        assert!(discs.discs && discs.pads);
        assert!((discs.repair_s - crate::brakes::DISC_CHANGE_S).abs() < 1e-4);
    }

    /// A race `laps_left` from the flag, refuelling at `lap_fuel_l` a lap,
    /// on tyres of unknown wear rate, nobody around.
    fn ctx(laps_left: u32, lap_fuel_l: Option<f32>) -> StopContext {
        StopContext {
            laps_left,
            lap_fuel_l,
            ..StopContext::default()
        }
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
        assert_eq!(
            plan_stop(&state, &ctx(10, Some(2.0))),
            None,
            "fresh and full"
        );
        state.tires.rear_left.wear_percent = 75.0;
        assert_eq!(
            plan_stop(&state, &ctx(20, Some(2.0))).map(|p| p.pick),
            Some(StopPick::Hard),
            "a long run: hards"
        );
        assert_eq!(
            plan_stop(&state, &ctx(8, Some(2.0))).map(|p| p.pick),
            Some(StopPick::Reference)
        );
        assert_eq!(
            plan_stop(&state, &ctx(3, Some(2.0))).map(|p| p.pick),
            Some(StopPick::Soft),
            "a sprint: softs"
        );
        assert_eq!(
            plan_stop(&state, &ctx(0, Some(2.0))),
            None,
            "not on the last lap"
        );
        state.tires.rear_left.wear_percent = 0.0;
        // A deep flat spot, a puncture or a leak each bring it in.
        state.tires.front_left.flat_spot = 0.7;
        assert!(
            plan_stop(&state, &ctx(10, Some(2.0))).is_some(),
            "a flat spot"
        );
        state.tires.front_left.flat_spot = 0.0;
        state.tires.front_right.leak_kpa_per_s = 2.0;
        assert!(
            plan_stop(&state, &ctx(10, Some(2.0))).is_some(),
            "a slow puncture"
        );
        state.tires.front_right.leak_kpa_per_s = 0.0;
        assert_eq!(plan_stop(&state, &ctx(10, Some(2.0))), None);
        assert_eq!(
            (
                StopPick::Hard.click(),
                StopPick::Reference.click(),
                StopPick::Soft.click()
            ),
            (-1, 0, 1)
        );
        state.fuel_liters = 5.0;
        assert_eq!(
            plan_stop(&state, &ctx(4, Some(2.0))),
            None,
            "short of the flag, but good for two more laps: a stint runs on"
        );
        state.fuel_liters = 3.0;
        assert!(
            plan_stop(&state, &ctx(4, Some(2.0))).is_some(),
            "short of fuel"
        );
        assert_eq!(
            plan_stop(&state, &ctx(1, Some(1.5))),
            None,
            "enough to the flag: no stop"
        );
    }

    fn fresh_car() -> CarState {
        CarState::new(
            uuid::Uuid::nil(),
            uuid::Uuid::nil(),
            &crate::data::GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        )
    }

    #[test]
    fn the_endgame_stays_out_on_tyres_that_last_and_splashes_fuel() {
        let mut state = fresh_car();
        // 72% worn after 12 laps: 6% a lap.
        state.tires.rear_left.wear_percent = 72.0;
        let mut c = ctx(2, None);
        c.stint_laps = 12.0;
        assert_eq!(plan_stop(&state, &c), None, "two laps on tyres that last");
        c.laps_left = 5;
        assert!(plan_stop(&state, &c).is_some(), "five: they would not");
        // Short of fuel late on, on tyres that last: a splash.
        state.tires.rear_left.wear_percent = 30.0;
        state.fuel_liters = 2.5;
        c.laps_left = 3;
        c.lap_fuel_l = Some(2.0);
        assert_eq!(
            plan_stop(&state, &c),
            Some(StopPlan {
                pick: StopPick::Soft,
                fuel_only: true
            }),
            "fuel alone"
        );
        // Unless saving gets it there.
        c.save_to_flag = true;
        assert_eq!(plan_stop(&state, &c), None, "it saves fuel instead");
        // On tyres that would not last, the stop is a full one.
        c.save_to_flag = false;
        state.tires.rear_left.wear_percent = 60.0;
        c.stint_laps = 6.0;
        assert_eq!(
            plan_stop(&state, &c).map(|p| p.fuel_only),
            Some(false),
            "tyres and fuel"
        );
    }

    #[test]
    fn the_crossover_reads_the_race_the_forecast_and_the_drivers_nerve() {
        use crate::tyre_thermal::{INTERMEDIATE, MEDIUM, WET};
        let tire = crate::data::TireConfig::default();
        let race = CrossoverContext {
            laps_left: 20,
            lap_s: 100.0,
            ..CrossoverContext::default()
        };
        // The thresholds as before.
        assert_eq!(
            tyres_for_the_track(&tire, MEDIUM, 0.3, &race),
            Some(INTERMEDIATE)
        );
        assert_eq!(
            tyres_for_the_track(&tire, INTERMEDIATE, 0.03, &race),
            Some(MEDIUM)
        );
        assert_eq!(tyres_for_the_track(&tire, INTERMEDIATE, 0.1, &race), None);
        // Nearly over: it stays out, unless slicks are aquaplaning.
        let last = CrossoverContext {
            laps_left: 1,
            ..race
        };
        assert_eq!(tyres_for_the_track(&tire, INTERMEDIATE, 0.03, &last), None);
        assert_eq!(tyres_for_the_track(&tire, MEDIUM, 0.3, &last), None);
        assert_eq!(
            tyres_for_the_track(&tire, MEDIUM, 0.9, &last),
            Some(WET),
            "a downpour"
        );
        // Rain due back within two laps keeps treaded tyres on.
        let returning = CrossoverContext {
            turn_in_s: Some(150.0),
            ..race
        };
        assert_eq!(
            tyres_for_the_track(&tire, INTERMEDIATE, 0.03, &returning),
            None
        );
        // A brave driver keeps slicks out in rain about to stop; a timid one
        // comes in.
        let stopping = CrossoverContext {
            raining: true,
            turn_in_s: Some(60.0),
            aggressiveness: 0.9,
            roll: 0.2,
            ..race
        };
        assert_eq!(tyres_for_the_track(&tire, MEDIUM, 0.3, &stopping), None);
        let timid = CrossoverContext {
            aggressiveness: 0.1,
            ..stopping
        };
        assert_eq!(
            tyres_for_the_track(&tire, MEDIUM, 0.3, &timid),
            Some(INTERMEDIATE)
        );
        // The gamble: slicks early on a drying line, rain stopped, none due.
        let gamble = CrossoverContext {
            aggressiveness: 0.9,
            roll: 0.2,
            ..race
        };
        assert_eq!(
            tyres_for_the_track(&tire, INTERMEDIATE, 0.1, &gamble),
            Some(MEDIUM)
        );
        let raining = CrossoverContext {
            raining: true,
            ..gamble
        };
        assert_eq!(
            tyres_for_the_track(&tire, INTERMEDIATE, 0.1, &raining),
            None
        );
        assert_eq!(
            tyres_for_the_track(
                &tire,
                INTERMEDIATE,
                0.1,
                &CrossoverContext {
                    aggressiveness: 0.1,
                    ..gamble
                }
            ),
            None,
            "a timid driver waits"
        );
    }

    #[test]
    fn a_stop_due_anyway_undercuts_the_car_ahead_or_covers_the_one_behind() {
        let mut state = fresh_car();
        // 55% after 11 laps of a race with 20 left: due a stop.
        state.tires.front_left.wear_percent = 55.0;
        let mut c = ctx(20, None);
        c.stint_laps = 11.0;
        assert_eq!(plan_stop(&state, &c), None, "running alone, it runs on");
        c.gap_ahead_s = Some(0.8);
        c.aggressiveness = 0.5;
        c.roll = 0.9;
        assert_eq!(plan_stop(&state, &c), None, "held up, not this lap");
        c.roll = 0.2;
        assert_eq!(
            plan_stop(&state, &c).map(|p| p.pick),
            Some(StopPick::Hard),
            "held up: the undercut"
        );
        c.gap_ahead_s = None;
        c.rival_behind_stopping = true;
        assert!(plan_stop(&state, &c).is_some(), "covers the car behind");
        // Tyres that reach the flag are no stop due: nothing to bring on.
        state.tires.front_left.wear_percent = 20.0;
        assert_eq!(plan_stop(&state, &c), None, "no stop due");
        // Nor so near the end.
        state.tires.front_left.wear_percent = 55.0;
        c.laps_left = 3;
        assert_eq!(plan_stop(&state, &c), None, "too late to gain");
    }

    #[test]
    fn the_ai_races_the_run_up_and_takes_the_lane_at_its_speed() {
        let lane = straight_lane();
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
        // The lane leaves a 1000 m lap at 900 m.
        state.pit.driving = true;
        state.track_progress = 700.0;
        assert_eq!(
            run_up_m(&lane, &state, 1000.0),
            Some(200.0),
            "on the run-up"
        );
        state.track_progress = 895.0;
        assert_eq!(run_up_m(&lane, &state, 1000.0), None, "handed to the lane");
        state.track_progress = 920.0;
        assert_eq!(run_up_m(&lane, &state, 1000.0), None, "past its mouth");
        state.track_progress = 700.0;
        state.pit.serviced = true;
        assert_eq!(run_up_m(&lane, &state, 1000.0), None, "on the way out");
        state.pit.serviced = false;
        state.pit.driving = false;
        assert_eq!(run_up_m(&lane, &state, 1000.0), None, "not stopping");

        let racing = PlayerInputData {
            throttle: 1.0,
            drs: true,
            ..Default::default()
        };
        let far = run_up_input(&lane, 200.0, 30.0, racing);
        assert_eq!(
            (far.throttle, far.brake, far.drs),
            (1.0, 0.0, false),
            "far out it races on, flap shut"
        );
        let corner = PlayerInputData {
            brake: 0.8,
            ..Default::default()
        };
        assert_eq!(
            run_up_input(&lane, 200.0, 30.0, corner).brake,
            0.8,
            "and brakes for its own corners"
        );
        let near = run_up_input(&lane, AI_PIT_HANDOVER_M + 2.0, 40.0, racing);
        assert!(
            near.throttle == 0.0 && near.brake > 0.5,
            "it slows for the lane: {near:?}"
        );
        let entry = lane_speed_mps(&lane, 0.0);
        let at = run_up_input(&lane, AI_PIT_HANDOVER_M + 1.0, entry - 2.0, racing);
        assert!(
            at.throttle > 0.0 && at.brake == 0.0,
            "but not below the lane's own speed: {at:?}"
        );
    }
}
