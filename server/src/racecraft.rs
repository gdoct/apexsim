//! Racecraft: what an AI driver does about the cars around it.
//!
//! The controller (`crate::ai_driver`) drives the racing line and, in
//! traffic, keeps a car's width from a car alongside and a following
//! distance behind a car ahead. That made a field that queued: a faster
//! car closed up, sat in the tow at the following distance and stayed
//! there, because nothing ever told it to leave the line. This module is
//! the part of the driver that does: every tick the session asks
//! each AI what it is doing about the cars near it ([`decide`]) and keeps
//! the answer on its car ([`CarState::racecraft`]), where the controller
//! reads it next tick. Kept on the car because it is a commitment: a
//! driver who has pulled out to pass does not change its mind every tick.
//!
//! - **Attack**: held up behind a car (slower through the corners than
//!   this driver would be there), the driver pulls out of the tow on the
//!   approach to a braking zone, to the inside of the corner, or round the
//!   outside when that is covered and it is brave enough, and brakes a
//!   shade later once alongside on the inside. Without half a car alongside
//!   at the end of the braking it lifts and tucks in behind; with it, it
//!   has the corner. Either way the lane ends at the apex. The corner is
//!   picked by its run in ([`pass_here`]): a driver held up waits for the
//!   end of the long straight, where the tow (and DRS) close it most.
//! - **Defend** (races only): a car behind that can get alongside by the
//!   braking point, at the speed it is closing, gets one move to the
//!   inside, made before the braking (or early on a straight against a
//!   car closing in the tow), held to the apex, and only one a corner. A
//!   car already alongside is too late to block.
//! - **Room**: with an attacker alongside, the driver being passed takes
//!   the lane beside it and holds it, rather than turning in on it.
//! - **Yield**: about to be lapped, the driver moves off the line toward
//!   the outside of the next corner and lifts a little.
//! - **Recovery**: stuck against a wall or off the road at a crawl, the
//!   driver backs out in reverse, swinging the nose toward the road, and
//!   drives on, steering for the leg of the road it left (followed through
//!   the spell off it, [`Racecraft::leg_idx`], not the lap position, which
//!   can jump to another leg nearby). It looks behind first: with a car
//!   coming on that stretch it waits for the gap, up to
//!   [`REJOIN_WAIT_MAX_S`], before backing out or setting off, and stops
//!   on the brake for one arriving while it backs. Before it a
//!   car pinned on a barrier sat there with its foot down until its engine
//!   cooked. A car that cannot get back (on
//!   snow it slides off every attempt) asks to be put back on the road
//!   once it has been off it [`RESET_AFTER_S`] without gaining
//!   [`RESET_HEADWAY_M`] of lap: the session recovers it as a driver's
//!   back-to-track would ([`Racecraft::wants_reset`]).
//! - **Mistakes**: under pressure (a car close behind), and less often on
//!   cold tyres, on a wet patch or late in a long stint ([`MistakeRisk`]),
//!   a driver now and then gets a corner wrong, by how inconsistent it is:
//!   it overcooks the braking, locks a wheel late in the braking zone,
//!   misses the apex wide or misses an upshift out of the corner
//!   ([`Mistake`]).
//!
//! Every chance is a hash of the driver, the lap and the corner, so a race
//! replays the same and the sim stays deterministic.

use serde::{Deserialize, Serialize};

use crate::ai_driver::AiDriverProfile;
use crate::data::{CarState, PlayerId, TrackConfig};
use crate::racing_line::{LinePhase, RacingLineProfile};

/// Below this a car is not racing anybody (the grid, a spin, a crawl).
const MIN_SPEED_MPS: f32 = 15.0;
/// How far ahead and behind a driver looks for a car to race, m.
const AHEAD_M: f32 = 80.0;
const BEHIND_M: f32 = 60.0;
/// Within this many seconds (plus [`CLOSE_EXTRA_M`]) of the car ahead a
/// driver counts as held up by it.
const HELD_GAP_S: f32 = 0.8;
const CLOSE_EXTRA_M: f32 = 5.0;
/// Slower than the plan by this share and this much, m/s, is being held
/// up: a car of the same pace ahead is not.
const HELD_MARGIN_SHARE: f32 = 0.01;
const HELD_MARGIN_MPS: f32 = 0.3;
/// Time held up before the boldest and the most timid driver try a pass:
/// time in corners and braking zones, where it is measured.
const HESITATE_BOLD_S: f32 = 0.3;
const HESITATE_TIMID_S: f32 = 1.5;
/// The approach to a braking zone, m before it, where a pass is started.
const ATTACK_FROM_M: f32 = 20.0;
const ATTACK_REACH_S: f32 = 3.5;
/// A car this much slower than the one behind it is passed on any straight.
const MUCH_SLOWER_MPS: f32 = 4.0;
/// Choosing where to pass: a corner whose run in is under this share of
/// the best of the next few is passed up, looking this many corners and
/// this far ahead, m; a run counts up to this long, m, and DRS open on it
/// adds this share. Held up this much longer than its hesitation, s, a
/// driver takes any corner.
const PLAN_SHARE: f32 = 0.6;
const PLAN_CORNERS: usize = 3;
const PLAN_AHEAD_M: f32 = 2500.0;
const PLAN_RUN_MAX_M: f32 = 1500.0;
const PLAN_DRS_WEIGHT: f32 = 1.0;
const PLAN_PATIENCE_S: f32 = 6.0;
/// Room left between the two cars when side by side, m.
const SIDE_ROOM_M: f32 = 1.2;
/// Margin kept from the road's edge when picking a lane, m.
const EDGE_MARGIN_M: f32 = 0.4;
/// An attack that has not come alongside in this long is given up.
const ATTACK_GIVE_UP_S: f32 = 10.0;
/// After a pass, or a pass given up, before the next.
const COOLDOWN_PASSED_S: f32 = 1.0;
const COOLDOWN_ABORT_S: f32 = 3.0;
/// The pace an attacker alongside in the braking zone takes on top of its
/// usual share of the profile: braking a little later.
const ATTACK_LATE_BRAKE: f32 = 0.02;
/// At the end of the braking an attacker whose middle is further behind
/// the other car's than this share of its length has not got the corner.
const TURN_IN_OVERLAP: f32 = 0.5;
/// Giving up a pass alongside: the lift, for this long, to drop in behind.
const TUCK_S: f32 = 1.5;
const TUCK_PACE: f32 = 0.93;
/// The car leaving room lifts this much while the other is level or ahead.
const ROOM_PACE: f32 = 0.98;
/// A defender sees a car this close behind (s, plus [`CLOSE_EXTRA_M`]).
const DEFEND_GAP_S: f32 = 0.5;
/// A defensive move is made between these distances before the braking.
const DEFEND_FROM_M: f32 = 40.0;
const DEFEND_REACH_S: f32 = 3.0;
/// The furthest a defensive move takes the car sideways, m, and how far
/// from the inside edge it stops.
const DEFEND_MOVE_M: f32 = 3.0;
const DEFEND_EDGE_M: f32 = 1.2;
/// A car behind is defended against only if it closes faster than this,
/// m/s, and at that rate gets alongside before the braking point, with
/// this much braking-zone gain on top, s.
const DEFEND_CLOSING_MPS: f32 = 0.5;
const DEFEND_BRAKING_GAIN_S: f32 = 1.0;
/// Covering the tow: a car in the wake within this gap, s (plus
/// [`CLOSE_EXTRA_M`]), closing faster than this, m/s, with the braking
/// point no further than this ahead, s, gets the move early; never when it
/// would arrive within this long of the move starting, s (the move takes
/// about a second to make).
const TOW_COVER_GAP_S: f32 = 0.8;
const TOW_COVER_CLOSING_MPS: f32 = 2.0;
const TOW_COVER_FROM_S: f32 = 8.0;
const TOW_COVER_LEAD_S: f32 = 1.2;
/// The share of drivers who defend at all: the most timid and the boldest.
const DEFEND_CHANCE_TIMID: f32 = 0.35;
const DEFEND_CHANCE_BOLD: f32 = 0.9;
/// A car being lapped moves this far toward the outside and lifts.
const YIELD_MOVE_M: f32 = 2.5;
const YIELD_PACE: f32 = 0.97;
/// Under pressure (a car within [`PRESSURE_GAP_S`] behind) a driver gets
/// a corner wrong with this chance per unit of inconsistency; which
/// [`Mistake`] is an even draw. An overcooked corner is taken this much of
/// its speed too fast and up to this much more.
const PRESSURE_GAP_S: f32 = 0.6;
const MISTAKE_CHANCE: f32 = 0.35;
const MISTAKE_OVERSPEED: f32 = 0.025;
const MISTAKE_OVERSPEED_SPREAD: f32 = 0.02;
/// A lock-up: the pedal held to the floor, past what the grip allows, for
/// this long and up to this much more, s, from this far before the apex, m
/// (late in the braking, where the downforce has gone and the turn-in
/// starts), or at the end of the braking if that comes first.
const LOCK_UP_S: f32 = 0.25;
const LOCK_UP_SPREAD_S: f32 = 0.35;
const LOCK_UP_APEX_M: f32 = 80.0;
/// A missed apex: the car runs this far wide of its line and up to this
/// much more, m, from the braking to the apex, moved on and off at this
/// rate, m/s.
const MISSED_APEX_M: f32 = 1.0;
const MISSED_APEX_SPREAD_M: f32 = 1.0;
const WIDE_RATE_MPS: f32 = 2.0;
/// A missed upshift out of the corner: no upshift for this long and up to
/// this much more, s (the engine sits on its limiter).
const MISSED_SHIFT_S: f32 = 0.4;
const MISSED_SHIFT_SPREAD_S: f32 = 0.6;
/// Away from pressure a driver gets a corner wrong too, at a share of the
/// chance under pressure for each thing that is against it
/// ([`MistakeRisk`], each 0..1): tyres under their window, a wet patch,
/// a long stint.
const COLD_MISTAKE: f32 = 0.15;
const WET_MISTAKE: f32 = 0.4;
const FATIGUE_MISTAKE: f32 = 0.15;
/// Under this much risk from them a corner is not rolled for at all.
const RISK_MIN: f32 = 0.01;
/// Tyres this far under their window's cold edge, °C, are as cold as the
/// risk counts.
const COLD_RISK_SPAN_C: f32 = 25.0;
/// Water under a tyre this much over the water the session's grip was
/// baked at (the weather's units) is as wet a patch as the risk counts.
const WET_RISK_SPAN: f32 = 0.3;
/// A stint starts to tire a driver after this long, s, and has tired it
/// all the risk counts after this.
const FATIGUE_FROM_S: f32 = 3600.0;
const FATIGUE_FULL_S: f32 = 4.0 * 3600.0;
/// How quickly the car moves onto a lane and back off it: the share of the
/// way per second (attack, defend, back to the line).
const BLEND_ATTACK_PER_S: f32 = 1.25;
const BLEND_DEFEND_PER_S: f32 = 0.9;
const BLEND_RETURN_PER_S: f32 = 0.5;
/// Below this a car not getting anywhere against a wall or off the road is
/// stuck, m/s, and after this long it backs out, s.
const STUCK_SPEED_MPS: f32 = 2.5;
const STUCK_AFTER_S: f32 = 1.5;
/// The longest a car backs out, s, and the wait before it may again.
const REVERSE_MAX_S: f32 = 2.5;
const RECOVER_COOLDOWN_S: f32 = 3.0;
/// Backing out stops early once the car is clear and pointing within this
/// of the road ahead, rad, having backed for at least this long, s.
const REVERSE_DONE_HEADING_RAD: f32 = 0.6;
const REVERSE_MIN_S: f32 = 0.8;
/// A car stopped off the road or against a wall waits before it backs out
/// or sets off while a racing car is coming on that stretch: within this
/// far behind along the lap, m (or alongside), and this soon, s. It waits
/// at most this long a spell, s, so a stream of traffic cannot hold it
/// there for ever.
const REJOIN_LOOK_M: f32 = 200.0;
const REJOIN_GAP_S: f32 = 3.0;
const REJOIN_WAIT_MAX_S: f32 = 6.0;
/// Off the road the leg the car left is followed by the nearest
/// centerline point within this many points of the last one.
const LEG_SEARCH_POINTS: usize = 32;
/// Off the road (or crawling against a wall) this long, s, without gaining
/// this much lap, m, a car asks to be put back on the road.
pub const RESET_AFTER_S: f32 = 15.0;
pub const RESET_HEADWAY_M: f32 = 20.0;

/// How far ahead a corner is looked for, m.
const CORNER_SCAN_M: f32 = 700.0;

/// What a driver is doing about the cars around it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum Tactic {
    /// Driving the line (and following whoever is ahead).
    #[default]
    Race,
    /// Passing [`Racecraft::rival`].
    Attack,
    /// Covering the inside against [`Racecraft::rival`].
    Defend,
    /// Leaving [`Racecraft::rival`], alongside to pass, a car's width.
    Room,
    /// Letting [`Racecraft::rival`], a lap up, through.
    Yield,
}

/// What a driver under pressure gets wrong in a corner.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum Mistake {
    #[default]
    None,
    /// Too fast into the braking zone ([`Racecraft::overspeed`]).
    Overcook,
    /// A wheel locked late in the braking ([`Racecraft::lock_up_s`]).
    LockUp,
    /// Wide of the apex ([`Racecraft::wide_m`]).
    MissedApex,
    /// An upshift missed out of the corner ([`Racecraft::missed_shift_s`]).
    MissedShift,
}

/// An AI driver's racecraft, kept on its car between ticks.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Racecraft {
    pub tactic: Tactic,
    /// The car the tactic is about.
    pub rival: Option<PlayerId>,
    /// Where the tactic wants the car across the road, m left of the
    /// centerline.
    pub lane_m: f32,
    /// How far the car has moved from its line onto `lane_m`, 0..1: the
    /// controller aims at `line + blend × (lane − line)`.
    pub blend: f32,
    /// The share of its usual pace the driver takes now: a late brake in
    /// an attack, an overcooked corner, a lift to let a car by.
    pub pace: f32,
    /// Seconds in the current tactic.
    pub since_s: f32,
    /// Seconds held up behind the car ahead.
    pub held_s: f32,
    /// Seconds before another pass is tried.
    pub cooldown_s: f32,
    /// The profile point of the corner the tactic is for.
    pub corner: Option<u32>,
    /// The corner a mistake has been rolled for (one roll a corner), and
    /// the overspeed rolled (0 for none).
    pub mistake_corner: Option<u32>,
    pub overspeed: f32,
    /// Seconds left lifting to tuck in behind after a pass given up.
    #[serde(default)]
    pub tuck_s: f32,
    /// Seconds stuck (crawling against a wall or off the road), seconds
    /// left backing out of it, and seconds before it is tried again.
    #[serde(default)]
    pub stuck_s: f32,
    #[serde(default)]
    pub reverse_s: f32,
    #[serde(default)]
    pub recover_cooldown_s: f32,
    /// The spell off the road: how long, s, and the lap position it began
    /// at, m; and whether it has gone on long enough to be put back.
    #[serde(default)]
    pub off_road_s: f32,
    #[serde(default)]
    pub off_road_from_m: f32,
    #[serde(default)]
    pub wants_reset: bool,
    /// The mistake rolled for `mistake_corner`, if any.
    #[serde(default)]
    pub mistake: Mistake,
    /// Seconds left of a lock-up: the brake to the floor whenever the plan
    /// brakes, past the friction circle.
    #[serde(default)]
    pub lock_up_s: f32,
    /// A missed apex: m left of its line the car runs (eased on and off).
    #[serde(default)]
    pub wide_m: f32,
    /// Seconds left of a missed upshift: the gear is held.
    #[serde(default)]
    pub missed_shift_s: f32,
    /// The centerline point of the leg of road the car is on: the car's
    /// own on the road, followed point by point off it, so a recovery
    /// steers for the road it left even where the lap position has jumped
    /// to another leg close by.
    #[serde(default)]
    pub leg_idx: Option<u32>,
    /// Stopped off the road or against a wall, waiting for a car coming
    /// to pass before backing out or setting off; and for how long, s.
    #[serde(default)]
    pub waiting: bool,
    #[serde(default)]
    pub wait_s: f32,
    /// The corner a defensive move has been made for: one move a corner,
    /// never a second one back across.
    #[serde(default)]
    pub defended_corner: Option<u32>,
}

impl Racecraft {
    /// A defensive move has been made for the corner at profile point
    /// `apex` already.
    fn defended(&self, apex: usize) -> bool {
        self.defended_corner == Some(apex as u32)
    }
}

impl Default for Racecraft {
    fn default() -> Self {
        Self {
            tactic: Tactic::Race,
            rival: None,
            lane_m: 0.0,
            blend: 0.0,
            pace: 1.0,
            since_s: 0.0,
            held_s: 0.0,
            cooldown_s: 0.0,
            corner: None,
            mistake_corner: None,
            overspeed: 0.0,
            tuck_s: 0.0,
            stuck_s: 0.0,
            reverse_s: 0.0,
            recover_cooldown_s: 0.0,
            off_road_s: 0.0,
            off_road_from_m: 0.0,
            wants_reset: false,
            mistake: Mistake::None,
            lock_up_s: 0.0,
            wide_m: 0.0,
            missed_shift_s: 0.0,
            leg_idx: None,
            waiting: false,
            wait_s: 0.0,
            defended_corner: None,
        }
    }
}

/// A car as the racecraft pass sees it: a snapshot of the field before any
/// driver decides, so no answer depends on the order they are asked in.
#[derive(Debug, Clone, Copy)]
pub struct Rival {
    pub id: PlayerId,
    pub progress_m: f32,
    /// Where it is, m.
    pub pos: (f32, f32),
    /// Laps × the lap length + progress: who is a lap up on whom.
    pub race_m: f32,
    /// Across the road, m left of the centerline.
    pub left_m: f32,
    pub speed_mps: f32,
    pub length_m: f32,
    pub width_m: f32,
    /// On the road and racing: not on the pit route, in a garage, towed or
    /// crawling.
    pub racing: bool,
    pub tactic: Tactic,
    pub rival: Option<PlayerId>,
    /// Its racecraft's lane and how far onto it the car is.
    pub lane_m: f32,
    pub blend: f32,
}

impl Rival {
    pub fn of(state: &CarState, length_m: f32, width_m: f32, total_m: f32) -> Self {
        Self {
            id: state.player_id,
            progress_m: state.track_progress,
            pos: (state.pos_x, state.pos_y),
            race_m: state.current_lap as f32 * total_m + state.track_progress,
            left_m: -state.lateral_offset_m,
            speed_mps: state.speed_mps,
            length_m,
            width_m,
            racing: racing(state),
            tactic: state.racecraft.tactic,
            rival: state.racecraft.rival,
            lane_m: state.racecraft.lane_m,
            blend: state.racecraft.blend,
        }
    }
}

/// Whether a car is out racing: on the road at speed, not being driven
/// down the pit lane, parked, towed or finished.
fn racing(state: &CarState) -> bool {
    state.is_on_track
        && state.speed_mps >= MIN_SPEED_MPS
        && !state.pit.driving
        && !state.in_garage
        && !state.is_ghost()
        && state.damage.is_drivable
        && state.finish_position.is_none()
}

/// What, besides a car close behind, puts a driver at risk of a mistake,
/// each 0..1 ([`MistakeRisk::of`]).
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct MistakeRisk {
    /// Tyres under their window: the colder axle's tread and carcass.
    pub cold: f32,
    /// Water under a tyre over what the session's grip was baked at: a
    /// puddle, a wet line in a drying race, rain the grip was not set for.
    pub wet: f32,
    /// The time in the car this stint.
    pub fatigue: f32,
}

impl MistakeRisk {
    /// `state`'s risk on `tire`s, on a road baked at `baked_water`, `stint_s`
    /// into its stint.
    pub fn of(
        state: &CarState,
        tire: &crate::data::TireConfig,
        baked_water: f32,
        stint_s: f32,
    ) -> Self {
        let cold = if state.tyres_fitted {
            let compound = tire.compound(state.tyre_compound);
            let edge =
                crate::tyre_thermal::optimum_c(tire, compound) - tire.temperature_window_c.max(0.0);
            let share = crate::tyre_thermal::TREAD_GRIP_SHARE;
            let at = |t: &crate::data::TireData| {
                share * t.temperature_c + (1.0 - share) * t.core_temperature_c
            };
            let t = &state.tires;
            let front = 0.5 * (at(&t.front_left) + at(&t.front_right));
            let rear = 0.5 * (at(&t.rear_left) + at(&t.rear_right));
            ((edge - front.min(rear)) / COLD_RISK_SPAN_C).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let water = state.tyre_water.iter().copied().fold(0.0, f32::max);
        Self {
            cold,
            wet: ((water - baked_water) / WET_RISK_SPAN).clamp(0.0, 1.0),
            fatigue: ((stint_s - FATIGUE_FROM_S) / (FATIGUE_FULL_S - FATIGUE_FROM_S))
                .clamp(0.0, 1.0),
        }
    }

    /// The chance of a mistake against the chance under pressure.
    fn weight(&self) -> f32 {
        COLD_MISTAKE * self.cold + WET_MISTAKE * self.wet + FATIGUE_MISTAKE * self.fatigue
    }
}

/// What the session tells the pass.
pub struct Context<'a> {
    pub track: &'a TrackConfig,
    pub total_m: f32,
    /// A race: defending and yielding are for races only, passing is for
    /// any session with cars on the road.
    pub race: bool,
    pub dt: f32,
}

/// The next corner a car brakes for, read off its speed profile.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Corner {
    /// The profile point of the corner's slowest point.
    pub apex: usize,
    /// Distance to where the braking starts, m (0 while braking).
    pub brake_in_m: f32,
    /// Distance to the apex, m.
    pub apex_in_m: f32,
    /// Which way it turns: +1 left, -1 right, 0 too straight to tell.
    pub inside: f32,
}

/// The profile point nearest the car: the profile is the line resampled
/// from its first point, so the share of the lap is close and the nearest
/// point around it settles the rest.
pub fn locate(speeds: &RacingLineProfile, state: &CarState, total_m: f32) -> Option<usize> {
    locate_at(
        speeds,
        (state.pos_x, state.pos_y),
        state.track_progress,
        total_m,
    )
}

/// [`locate`] for a point `pos` at `progress_m` along the lap.
pub fn locate_at(
    speeds: &RacingLineProfile,
    pos: (f32, f32),
    progress_m: f32,
    total_m: f32,
) -> Option<usize> {
    let n = speeds.points.len();
    if n == 0 || speeds.spacing_m <= 0.0 || total_m <= 0.0 {
        return None;
    }
    let guess = (progress_m / total_m * n as f32).round() as i64;
    let window = (100.0 / speeds.spacing_m).ceil() as i64;
    let mut here = guess.rem_euclid(n as i64) as usize;
    let mut best = f32::MAX;
    for offset in -window..=window {
        let i = (guess + offset).rem_euclid(n as i64) as usize;
        let [x, y, _] = speeds.points[i];
        let d2 = (x - pos.0).powi(2) + (y - pos.1).powi(2);
        if d2 < best {
            best = d2;
            here = i;
        }
    }
    Some(here)
}

/// The corner ahead of profile point `here`: the one the car is braking or
/// turning for now, else the next braking zone within [`CORNER_SCAN_M`].
/// Flat kinks are not corners here: nobody passes in one.
pub fn next_corner(speeds: &RacingLineProfile, here: usize) -> Option<Corner> {
    let n = speeds.points.len();
    let sp = speeds.spacing_m;
    if n < 8 || sp <= 0.0 {
        return None;
    }
    let at = |k: usize| (here + k) % n;
    let scan = ((CORNER_SCAN_M / sp) as usize).min(n - 1);

    // Braking or turning now, with the speed still to fall: this corner.
    let in_corner = speeds.phases[here] != LinePhase::Throttle;
    let brake_k = if in_corner {
        let near = ((150.0 / sp) as usize).min(scan);
        let falls = (1..=near).any(|k| speeds.speed_mps[at(k)] < speeds.speed_mps[here] - 0.5);
        if falls {
            Some(0)
        } else {
            None
        }
    } else {
        None
    };
    let brake_k = match brake_k {
        Some(k) => k,
        None => {
            // Out of this corner first, then the next braking zone.
            let mut k = 0;
            while k < scan && speeds.phases[at(k)] != LinePhase::Throttle {
                k += 1;
            }
            (k..=scan).find(|&k| speeds.phases[at(k)] == LinePhase::Brake)?
        }
    };

    // The slowest point after the braking starts, until the speed has
    // clearly picked up again.
    let mut apex_k = brake_k;
    let mut slowest = speeds.speed_mps[at(brake_k)];
    let reach = ((400.0 / sp) as usize).min(n - 1);
    for k in brake_k..=(brake_k + reach) {
        let v = speeds.speed_mps[at(k)];
        if v < slowest {
            slowest = v;
            apex_k = k;
        } else if v > slowest * 1.08 + 0.5 {
            break;
        }
    }

    // Which way it turns at the apex.
    let span = ((15.0 / sp).ceil() as usize).max(1);
    let a = speeds.points[(at(apex_k) + n - span) % n];
    let b = speeds.points[at(apex_k)];
    let c = speeds.points[(at(apex_k) + span) % n];
    let cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
    let norm = ((b[0] - a[0]).hypot(b[1] - a[1]) * (c[0] - b[0]).hypot(c[1] - b[1])).max(1e-3);
    // Under about 3° over the span is no side to take.
    let inside = if cross / norm > 0.05 {
        1.0
    } else if cross / norm < -0.05 {
        -1.0
    } else {
        0.0
    };
    Some(Corner {
        apex: at(apex_k),
        brake_in_m: brake_k as f32 * sp,
        apex_in_m: apex_k as f32 * sp,
        inside,
    })
}

/// A number in 0..1 for this driver, lap and corner, and a salt.
fn roll(driver: PlayerId, lap: u16, corner: usize, salt: u64) -> f32 {
    let (hi, lo) = driver.as_u64_pair();
    let seed = hi ^ lo.rotate_left(17) ^ ((lap as u64) << 40) ^ corner as u64;
    crate::wind::hash01(seed, salt)
}

/// The road at the car: m to its left and right edges from the centerline.
fn road_at(track: &TrackConfig, state: &CarState) -> (f32, f32) {
    state
        .nearest_centerline_idx
        .and_then(|i| track.centerline.get(i as usize))
        .map_or((5.0, 5.0), |p| (p.width_left_m, p.width_right_m))
}

/// Ahead of `me` along the lap, m, wrapped to half a lap either way.
fn ahead_of(me: f32, other: f32, total: f32) -> f32 {
    (other - me + 0.5 * total).rem_euclid(total) - 0.5 * total
}

/// What `state`'s driver does next about the cars in `field` (which may
/// include itself): its racecraft for the coming tick.
pub fn decide(
    state: &CarState,
    me: &Rival,
    profile: &AiDriverProfile,
    speeds: Option<&RacingLineProfile>,
    field: &[Rival],
    risk: &MistakeRisk,
    ctx: &Context<'_>,
) -> Racecraft {
    let mut rc = state.racecraft;
    let dt = ctx.dt;
    rc.cooldown_s = (rc.cooldown_s - dt).max(0.0);
    rc.since_s += dt;
    rc.pace = 1.0;
    rc.lock_up_s = (rc.lock_up_s - dt).max(0.0);
    rc.missed_shift_s = (rc.missed_shift_s - dt).max(0.0);
    recover(&mut rc, state, me, field, ctx);
    if rc.reverse_s > 0.0 {
        return settle(back_to_racing(rc, 0.0), dt);
    }

    let here = speeds.and_then(|s| locate(s, state, ctx.total_m));
    let corner = match (speeds, here) {
        (Some(s), Some(h)) => next_corner(s, h),
        _ => None,
    };
    let corner = match corner {
        Some(corner) if me.racing => corner,
        _ => return settle(back_to_racing(rc, 0.0), dt),
    };
    // Braking now, by the plan: where a late brake or an overcooked
    // corner happens (never past the turn-in, where it runs the car wide).
    let braking = match (speeds, here) {
        (Some(s), Some(h)) => s.phases[h] == LinePhase::Brake,
        _ => false,
    };
    let (road_left, road_right) = road_at(ctx.track, state);
    let half = 0.5 * me.width_m;
    let lane_lo = -road_right + half + EDGE_MARGIN_M;
    let lane_hi = road_left - half - EDGE_MARGIN_M;
    let clamp_lane = |lane: f32| {
        if lane_lo < lane_hi {
            lane.clamp(lane_lo, lane_hi)
        } else {
            0.0
        }
    };
    let speed = state.speed_mps.max(1.0);

    // The nearest racing car ahead and behind on the road.
    let mut ahead: Option<(&Rival, f32)> = None;
    let mut behind: Option<(&Rival, f32)> = None;
    for other in field.iter().filter(|o| o.id != me.id && o.racing) {
        let rel = ahead_of(me.progress_m, other.progress_m, ctx.total_m);
        if rel > 0.0 && rel < AHEAD_M && ahead.is_none_or(|(_, r)| rel < r) {
            ahead = Some((other, rel));
        } else if rel <= 0.0 && rel > -BEHIND_M && behind.is_none_or(|(_, r)| rel > r) {
            behind = Some((other, rel));
        }
    }
    let reach = |o: &Rival| 0.5 * (me.length_m + o.length_m);
    let overlapping = |o: &Rival, rel: f32| rel.abs() < reach(o) + 0.5;

    // Held up: close behind a car going slower than this driver's own
    // plan would take it here. Two drivers of a pace never are, so they
    // follow; a quicker one is, and the car it passed is not, so the pass
    // is not undone a corner later.
    // The plan as this driver can drive it now: its tyres, the air it is
    // in (a car in the dirty air of the one ahead is slower through the
    // corners, and should be), as the controller scales it.
    let grip = (state.tyre_grip_share() * state.aero_load_share).powf(0.75);
    let pace = crate::ai_driver::profile_pace(profile.skill_level) * grip;
    match ahead {
        Some((o, rel)) if rel - reach(o) < HELD_GAP_S * speed + CLOSE_EXTRA_M => {
            // Slower than this driver would be where that car is: read
            // the plan at its place, not this car's, or a car braking for
            // the corner ahead reads as slow to the one behind it still
            // on the straight.
            //
            // Only where the plan, not the engine, sets the speed: through
            // a corner or a braking zone. Out of one, every car is below
            // the speed it could hold, accelerating toward it.
            let at = speeds.and_then(|s| {
                locate_at(s, o.pos, o.progress_m, ctx.total_m)
                    .map(|h| (s.speed_mps[h], s.phases[h]))
            });
            match at {
                Some((plan, phase)) if phase != LinePhase::Throttle => {
                    if o.speed_mps < plan * pace * (1.0 - HELD_MARGIN_SHARE) - HELD_MARGIN_MPS {
                        rc.held_s += dt;
                    } else {
                        rc.held_s = (rc.held_s - 0.5 * dt).max(0.0);
                    }
                }
                _ => {}
            }
        }
        _ => rc.held_s = (rc.held_s - 2.0 * dt).max(0.0),
    }

    // Mistakes, under pressure or not, rolled once a corner.
    mistakes(
        &mut rc, state, me, profile, &corner, braking, behind, risk, speed, dt, ctx,
    );
    let mistake = rc.mistake_corner == Some(corner.apex as u32) && rc.overspeed > 0.0;

    // Carry on with what the driver is doing, or stop doing it.
    let rival = rc
        .rival
        .and_then(|id| field.iter().find(|o| o.id == id && o.racing))
        .map(|o| (o, ahead_of(me.progress_m, o.progress_m, ctx.total_m)));
    match (rc.tactic, rival) {
        (Tactic::Race, _) => {}
        (_, None) => rc = back_to_racing(rc, COOLDOWN_ABORT_S),
        (Tactic::Attack, Some((o, rel))) => {
            if rel < -(reach(o) + 3.0) {
                // Past: back onto the line, the cut back in front of it.
                rc = back_to_racing(rc, COOLDOWN_PASSED_S);
                rc.held_s = 0.0;
            } else if rc.corner != Some(corner.apex as u32) && overlapping(o, rel) {
                // Through the corner alongside: back to the lines, side by
                // side with the ordinary room between them, and another go
                // at the next corner. A lane held through a second corner
                // is the wrong line for it (a car ran off at Zandvoort
                // holding the hairpin's lane into the bend after it).
                rc = back_to_racing(rc, COOLDOWN_PASSED_S);
            } else if rel > AHEAD_M
                || rc.since_s > ATTACK_GIVE_UP_S
                || rc.corner != Some(corner.apex as u32)
            {
                // Dropped back, too long, or the corner went by without a
                // nose alongside.
                rc = back_to_racing(rc, COOLDOWN_ABORT_S);
                rc.held_s = 0.0;
            } else if !overlapping(o, rel) {
                // Still behind: keep the lane beside it, which follows it
                // across the road. If it has covered that side and there
                // is no room left, give up.
                let side = (rc.lane_m - o.left_m).signum();
                let lane =
                    clamp_lane(o.left_m + side * (0.5 * (me.width_m + o.width_m) + SIDE_ROOM_M));
                if (lane - o.left_m).abs() < 0.5 * (me.width_m + o.width_m) + 0.3
                    && corner.brake_in_m < 30.0
                {
                    rc = back_to_racing(rc, COOLDOWN_ABORT_S);
                    rc.held_s = 0.0;
                } else {
                    rc.lane_m = lane;
                }
            } else if !braking && corner.brake_in_m <= 0.0 && rel > TURN_IN_OVERLAP * o.length_m {
                // Turning in without half a car alongside: the corner is
                // the other car's. Lift, tuck in behind, and go again later.
                rc = back_to_racing(rc, COOLDOWN_ABORT_S);
                rc.held_s = 0.0;
                rc.tuck_s = TUCK_S;
            } else if braking && (rc.lane_m - o.left_m) * corner.inside > 0.0 {
                // Alongside on the inside into the braking zone: brake a
                // shade later.
                rc.pace = 1.0 + ATTACK_LATE_BRAKE;
            }
        }
        (Tactic::Defend, Some((_, rel))) => {
            // Held to the apex of the corner covered, and not when the car
            // has dropped away.
            if rc.corner != Some(corner.apex as u32) || rel < -BEHIND_M {
                rc = back_to_racing(rc, 0.0);
            }
        }
        (Tactic::Room, Some((o, rel))) => {
            // Held until the two are clear of each other, not only while
            // it attacks: through the corner and out of it a car's width
            // stays between them, beside wherever the other car is.
            if rel > reach(o) + 3.0 || rel < -(reach(o) + 5.0) {
                rc = back_to_racing(rc, 0.0);
            } else {
                // Beside the lane it holds, as when the room was first
                // given: beside where it is would follow it as its own
                // traffic dodge moves it away, and the two would walk each
                // other off the road (Le Mans' first corner, without the
                // road mesh). Once it stops attacking its blend runs out
                // and this is where it is.
                let theirs = o.left_m + o.blend.clamp(0.0, 1.0) * (o.lane_m - o.left_m);
                let side = if rc.lane_m >= theirs { 1.0 } else { -1.0 };
                rc.lane_m =
                    clamp_lane(theirs + side * (0.5 * (me.width_m + o.width_m) + SIDE_ROOM_M));
                // Level or behind it: the corner is its, so do not race it
                // out of the corner side by side.
                if rel > -0.5 * reach(o) {
                    rc.pace = ROOM_PACE;
                }
            }
        }
        (Tactic::Yield, Some((o, rel))) => {
            if rel > reach(o) + 3.0 || rel < -BEHIND_M {
                rc = back_to_racing(rc, 0.0);
            } else {
                rc.pace = YIELD_PACE;
            }
        }
    }

    // Something new.
    if rc.tactic == Tactic::Race {
        if let Some(next) = start_tactic(
            state,
            me,
            profile,
            &corner,
            ahead,
            behind,
            field,
            ctx,
            &clamp_lane,
            &rc,
            speeds.zip(here),
        ) {
            rc = next;
        }
    }

    if rc.tuck_s > 0.0 {
        rc.tuck_s = (rc.tuck_s - dt).max(0.0);
        rc.pace *= TUCK_PACE;
    }
    if mistake && braking {
        rc.pace *= 1.0 + rc.overspeed;
    }
    settle(rc, dt)
}

/// The mistakes a driver makes: one roll a corner, as it nears the braking
/// zone with a car close behind or on cold tyres, a wet patch or a long
/// stint ([`MistakeRisk`]), for whether it gets the corner wrong and how.
/// A corner's mistake ends once the corner is passed; a missed upshift is
/// made then, on the way out of it.
#[allow(clippy::too_many_arguments)]
fn mistakes(
    rc: &mut Racecraft,
    state: &CarState,
    me: &Rival,
    profile: &AiDriverProfile,
    corner: &Corner,
    braking: bool,
    behind: Option<(&Rival, f32)>,
    risk: &MistakeRisk,
    speed: f32,
    dt: f32,
    ctx: &Context<'_>,
) {
    let apex = corner.apex as u32;
    if rc.mistake_corner.is_some_and(|c| c != apex) {
        if rc.mistake == Mistake::MissedShift {
            let c = rc.mistake_corner.unwrap_or(0) as usize;
            rc.missed_shift_s =
                MISSED_SHIFT_S + MISSED_SHIFT_SPREAD_S * roll(me.id, state.current_lap, c, 6);
        }
        rc.mistake_corner = None;
        rc.mistake = Mistake::None;
        rc.overspeed = 0.0;
    }

    let reach = |o: &Rival| 0.5 * (me.length_m + o.length_m);
    let pressured =
        behind.is_some_and(|(o, rel)| -rel - reach(o) < PRESSURE_GAP_S * speed + CLOSE_EXTRA_M);
    let weight = risk.weight();
    let at_risk = pressured || weight >= RISK_MIN;
    if ctx.race && at_risk && corner.brake_in_m < 150.0 && rc.mistake_corner != Some(apex) {
        rc.mistake_corner = Some(apex);
        let inconsistency = (1.0 - profile.consistency).clamp(0.0, 1.0);
        let (lap, at) = (state.current_lap, corner.apex);
        let chance = MISTAKE_CHANCE * inconsistency * (if pressured { 1.0 } else { 0.0 } + weight);
        // Cold tyres and a wet patch go wrong in the braking and the
        // turn-in; a shift is missed under pressure or tired.
        let kinds = if pressured || risk.fatigue > 0.0 {
            4.0
        } else {
            3.0
        };
        rc.mistake = if roll(me.id, lap, at, 1) < chance {
            match (roll(me.id, lap, at, 5) * kinds) as u32 {
                0 => Mistake::Overcook,
                1 => Mistake::LockUp,
                2 => Mistake::MissedApex,
                _ => Mistake::MissedShift,
            }
        } else {
            Mistake::None
        };
        rc.overspeed = if rc.mistake == Mistake::Overcook {
            MISTAKE_OVERSPEED + MISTAKE_OVERSPEED_SPREAD * roll(me.id, lap, at, 2)
        } else {
            0.0
        };
    }

    let this_corner = rc.mistake_corner == Some(apex);
    // A lock-up, once, late in the braking or at its end.
    if this_corner
        && rc.mistake == Mistake::LockUp
        && corner.brake_in_m <= 0.0
        && (corner.apex_in_m < LOCK_UP_APEX_M || !braking)
    {
        rc.lock_up_s =
            LOCK_UP_S + LOCK_UP_SPREAD_S * roll(me.id, state.current_lap, corner.apex, 6);
        rc.mistake = Mistake::None;
    }
    // Wide of the apex from the braking to the apex, back after it.
    let wide = if this_corner
        && rc.mistake == Mistake::MissedApex
        && corner.brake_in_m <= 0.0
        && corner.inside != 0.0
    {
        -corner.inside
            * (MISSED_APEX_M
                + MISSED_APEX_SPREAD_M * roll(me.id, state.current_lap, corner.apex, 6))
    } else {
        0.0
    };
    let step = WIDE_RATE_MPS * dt;
    rc.wide_m += (wide - rc.wide_m).clamp(-step, step);
}

/// A new tactic, if one is called for.
#[allow(clippy::too_many_arguments)]
fn start_tactic(
    state: &CarState,
    me: &Rival,
    profile: &AiDriverProfile,
    corner: &Corner,
    ahead: Option<(&Rival, f32)>,
    behind: Option<(&Rival, f32)>,
    field: &[Rival],
    ctx: &Context<'_>,
    clamp_lane: &dyn Fn(f32) -> f32,
    rc: &Racecraft,
    plan: Option<(&RacingLineProfile, usize)>,
) -> Option<Racecraft> {
    let aggr = profile.aggressiveness.clamp(0.0, 1.0);
    let speed = state.speed_mps.max(1.0);
    let reach = |o: &Rival| 0.5 * (me.length_m + o.length_m);
    let fresh = |tactic: Tactic, rival: PlayerId, lane: f32| Racecraft {
        tactic,
        rival: Some(rival),
        lane_m: lane,
        since_s: 0.0,
        corner: Some(corner.apex as u32),
        ..*rc
    };

    // An attacker alongside: leave it a car's width, beside the lane it
    // holds (once alongside an attacker stops moving its lane).
    let alongside = field.iter().find(|o| {
        o.racing
            && o.tactic == Tactic::Attack
            && o.rival == Some(me.id)
            && ahead_of(me.progress_m, o.progress_m, ctx.total_m).abs() < reach(o) + 0.5
    });
    if let Some(o) = alongside {
        let theirs = o.left_m + o.blend.clamp(0.0, 1.0) * (o.lane_m - o.left_m);
        let side = if me.left_m >= theirs { 1.0 } else { -1.0 };
        let lane = theirs + side * (0.5 * (me.width_m + o.width_m) + SIDE_ROOM_M);
        return Some(fresh(Tactic::Room, o.id, clamp_lane(lane)));
    }

    // About to be lapped: let it by.
    if ctx.race {
        if let Some((o, rel)) = behind {
            if o.race_m > me.race_m + 0.5 * ctx.total_m
                && -rel - reach(o) < 1.0 * speed + CLOSE_EXTRA_M
            {
                let outside = if corner.inside != 0.0 {
                    -corner.inside
                } else {
                    1.0
                };
                let lane = clamp_lane(me.left_m + outside * YIELD_MOVE_M);
                return Some(fresh(Tactic::Yield, o.id, lane));
            }
        }
    }

    // Defend: one move to the inside before the braking, against a car
    // close behind that can get alongside by the end of the braking (or is
    // attacking us), not yet alongside, on the same lap. On a long
    // straight the same one move is made early against a car closing in
    // the tow, to cover the inside before it pulls out.
    if ctx.race && corner.inside != 0.0 && !rc.defended(corner.apex) {
        if let Some((o, rel)) = behind {
            let gap = -rel - reach(o);
            let attacking = o.tactic == Tactic::Attack && o.rival == Some(me.id);
            let closing = o.speed_mps - me.speed_mps;
            // It reaches us by the braking point at the speed it closes
            // now, with what a later brake gains it on top.
            let to_brake_s = corner.brake_in_m / speed;
            let reaches = closing > DEFEND_CLOSING_MPS
                && gap < closing * (to_brake_s + DEFEND_BRAKING_GAIN_S);
            let coming = attacking || reaches;
            let window = corner.brake_in_m >= DEFEND_FROM_M
                && corner.brake_in_m <= DEFEND_FROM_M + DEFEND_REACH_S * speed;
            // Covering the tow: further out on the straight, against a
            // car still in our wake (not pulled out yet) closing fast,
            // far enough behind that the move is made before it arrives.
            let in_tow = (o.left_m - me.left_m).abs() < 0.5 * me.width_m;
            let tow_cover = corner.brake_in_m > DEFEND_FROM_M + DEFEND_REACH_S * speed
                && corner.brake_in_m < TOW_COVER_FROM_S * speed
                && in_tow
                && closing > TOW_COVER_CLOSING_MPS
                && gap > closing * TOW_COVER_LEAD_S
                && gap < TOW_COVER_GAP_S * speed + CLOSE_EXTRA_M;
            let same_lap = (o.race_m - me.race_m).abs() < 0.5 * ctx.total_m;
            if gap > 0.5
                && ((gap < DEFEND_GAP_S * speed + CLOSE_EXTRA_M && coming && window)
                    || (tow_cover && reaches))
                && same_lap
            {
                let chance =
                    DEFEND_CHANCE_TIMID + (DEFEND_CHANCE_BOLD - DEFEND_CHANCE_TIMID) * aggr;
                let (road_left, road_right) = road_at(ctx.track, state);
                let inside_edge = if corner.inside > 0.0 {
                    road_left
                } else {
                    -road_right
                };
                let cover = inside_edge - corner.inside * (0.5 * me.width_m + DEFEND_EDGE_M);
                let lane = me.left_m + (cover - me.left_m).clamp(-DEFEND_MOVE_M, DEFEND_MOVE_M);
                // Only a move toward the inside, and only if the attacker
                // is not already there.
                let moves_in = (lane - me.left_m) * corner.inside > 0.5;
                let attacker_inside = (o.left_m - me.left_m) * corner.inside > 0.5 * me.width_m;
                if moves_in
                    && !attacker_inside
                    && roll(me.id, state.current_lap, corner.apex, 3) < chance
                {
                    let mut next = fresh(Tactic::Defend, o.id, clamp_lane(lane));
                    next.defended_corner = Some(corner.apex as u32);
                    return Some(next);
                }
            }
        }
    }

    // Attack: held up long enough, on the approach to a braking zone (or
    // anywhere on a straight behind a car much slower).
    let (o, rel) = ahead?;
    if rc.cooldown_s > 0.0 {
        return None;
    }
    let gap = rel - reach(o);
    let close = gap < 0.6 * speed + CLOSE_EXTRA_M;
    let hesitate = HESITATE_TIMID_S + (HESITATE_BOLD_S - HESITATE_TIMID_S) * aggr;
    let approach = corner.brake_in_m >= ATTACK_FROM_M
        && corner.brake_in_m <= ATTACK_FROM_M + ATTACK_REACH_S * speed;
    let crawling = o.speed_mps < me.speed_mps - MUCH_SLOWER_MPS && corner.brake_in_m > 60.0;
    // Where to pass: into the braking zone at the end of the run that
    // closes most (the longest straight, DRS open on it), unless held up
    // so long that any corner will do.
    let planned = rc.held_s >= hesitate + PLAN_PATIENCE_S
        || plan.is_none_or(|(s, h)| pass_here(s, h, corner, state.drs_allowed));
    if !close || !(crawling || (rc.held_s >= hesitate && approach && planned)) {
        return None;
    }

    let (road_left, road_right) = road_at(ctx.track, state);
    let beside = 0.5 * (me.width_m + o.width_m) + SIDE_ROOM_M;
    let room = |side: f32| {
        let edge = if side > 0.0 { road_left } else { road_right };
        edge - side * o.left_m - 0.5 * o.width_m - EDGE_MARGIN_M
    };
    let inside = if corner.inside != 0.0 {
        corner.inside
    } else if o.left_m > me.left_m {
        -1.0
    } else {
        1.0
    };
    let fits = |side: f32| room(side) >= me.width_m + SIDE_ROOM_M;
    let side = if fits(inside) {
        inside
    } else if fits(-inside) && (aggr > 0.5 || crawling) {
        // Round the outside: only the brave, or past a crawling car.
        -inside
    } else {
        return None;
    };
    Some(fresh(
        Tactic::Attack,
        o.id,
        clamp_lane(o.left_m + side * beside),
    ))
}

/// The straight run into the braking point at profile point `brake`, m:
/// the throttle stretch before it, up to [`PLAN_RUN_MAX_M`].
fn run_into(speeds: &RacingLineProfile, brake: usize) -> f32 {
    let n = speeds.points.len();
    let sp = speeds.spacing_m;
    let most = ((PLAN_RUN_MAX_M / sp) as usize).min(n.saturating_sub(1));
    let mut k = 1;
    while k <= most && speeds.phases[(brake + n - k) % n] == LinePhase::Throttle {
        k += 1;
    }
    (k - 1) as f32 * sp
}

/// Whether the corner ahead is the place to pass: its run in (the tow's
/// length, and twice that with DRS open on it now) at least
/// [`PLAN_SHARE`] of the best of the next [`PLAN_CORNERS`] corners'
/// within [`PLAN_AHEAD_M`]. A driver held up behind a car waits for the
/// long straight rather than diving into the first corner it reaches.
fn pass_here(speeds: &RacingLineProfile, here: usize, corner: &Corner, drs: bool) -> bool {
    let n = speeds.points.len();
    let sp = speeds.spacing_m;
    if n < 8 || sp <= 0.0 {
        return true;
    }
    let brake = (here + (corner.brake_in_m / sp).round() as usize) % n;
    let mut score = run_into(speeds, brake);
    if drs {
        score *= 1.0 + PLAN_DRS_WEIGHT;
    }
    let step = ((30.0 / sp).ceil() as usize).max(1);
    let mut from = corner.apex;
    let mut travelled = corner.apex_in_m;
    for _ in 0..PLAN_CORNERS {
        let Some(next) = next_corner(speeds, (from + step) % n) else {
            break;
        };
        travelled += step as f32 * sp + next.brake_in_m;
        if travelled > PLAN_AHEAD_M {
            break;
        }
        let brake = (from + step + (next.brake_in_m / sp).round() as usize) % n;
        if run_into(speeds, brake) * PLAN_SHARE > score {
            return false;
        }
        travelled += next.apex_in_m - next.brake_in_m;
        from = next.apex;
    }
    true
}

/// Getting unstuck: count the time crawling against a wall or off the road,
/// back out once it is long enough, and stop backing once clear and
/// pointing at the road (or out of time). Stopped with a car coming, wait
/// for it first.
fn recover(rc: &mut Racecraft, state: &CarState, me: &Rival, field: &[Rival], ctx: &Context<'_>) {
    let dt = ctx.dt;
    rc.recover_cooldown_s = (rc.recover_cooldown_s - dt).max(0.0);
    rc.waiting = false;
    follow_leg(rc, state, ctx.track);
    let out_of_it = state.pit.driving
        || state.in_garage
        || state.is_ghost()
        || !state.damage.is_drivable
        || state.finish_position.is_some();
    if out_of_it {
        rc.stuck_s = 0.0;
        rc.reverse_s = 0.0;
        rc.off_road_s = 0.0;
        rc.wants_reset = false;
        rc.wait_s = 0.0;
        return;
    }
    // The spell off the road, backing out included: one that gets nowhere
    // along the lap ends in a reset rather than backing out for ever.
    let off = !state.is_on_track || (state.is_colliding && state.speed_mps < STUCK_SPEED_MPS);
    if !off {
        rc.off_road_s = 0.0;
        // Rolling back onto the road the car is not off it, but its spell
        // of waiting goes on until it has backed out.
        if rc.reverse_s <= 0.0 {
            rc.wait_s = 0.0;
        }
    } else {
        if rc.off_road_s <= 0.0
            || ahead_of(rc.off_road_from_m, state.track_progress, ctx.total_m) > RESET_HEADWAY_M
        {
            rc.off_road_s = 0.0;
            rc.off_road_from_m = state.track_progress;
        }
        rc.off_road_s += dt;
        if rc.off_road_s >= RESET_AFTER_S {
            rc.wants_reset = true;
        }
    }
    if rc.reverse_s > 0.0 {
        // Still looking behind while it backs: a car arriving stops it on
        // the brake, in reverse, until it is by (the back-out's time is
        // held, not spent), as long as a spell's wait allows.
        if rc.wait_s < REJOIN_WAIT_MAX_S
            && traffic_coming(me, leg_m(rc, state, ctx.track), field, ctx.total_m)
        {
            rc.waiting = true;
            rc.wait_s += dt;
            return;
        }
        rc.reverse_s = (rc.reverse_s - dt).max(0.0);
        let backed = REVERSE_MAX_S - rc.reverse_s;
        let clear = !state.is_colliding
            && road_heading_error(state, ctx.track, leg_m(rc, state, ctx.track), ctx.total_m).abs()
                < REVERSE_DONE_HEADING_RAD;
        if backed >= REVERSE_MIN_S && clear {
            rc.reverse_s = 0.0;
        }
        if rc.reverse_s <= 0.0 {
            rc.recover_cooldown_s = RECOVER_COOLDOWN_S;
        }
        return;
    }
    let stuck = state.speed_mps < STUCK_SPEED_MPS && (state.is_colliding || !state.is_on_track);
    // Look behind before moving: backing out or setting off in front of a
    // car arriving puts the stopped car in its way.
    if stuck
        && rc.wait_s < REJOIN_WAIT_MAX_S
        && traffic_coming(me, leg_m(rc, state, ctx.track), field, ctx.total_m)
    {
        rc.waiting = true;
        rc.wait_s += dt;
        return;
    }
    rc.stuck_s = if stuck { rc.stuck_s + dt } else { 0.0 };
    if rc.stuck_s >= STUCK_AFTER_S && rc.recover_cooldown_s <= 0.0 {
        rc.stuck_s = 0.0;
        rc.reverse_s = REVERSE_MAX_S;
    }
}

/// Follow the leg of road the car is on ([`Racecraft::leg_idx`]): the
/// car's own nearest point on the road, and off it (or against a wall) the
/// nearest point within [`LEG_SEARCH_POINTS`] of the last, which never
/// jumps to another leg however close it runs.
fn follow_leg(rc: &mut Racecraft, state: &CarState, track: &TrackConfig) {
    let line = &track.centerline;
    let n = line.len();
    let last = rc.leg_idx.map(|i| i as usize).filter(|&i| i < n);
    let on_road = state.is_on_track && !state.is_colliding;
    rc.leg_idx = match last {
        Some(from) if !on_road && n > 2 * LEG_SEARCH_POINTS => {
            let d2 =
                |i: usize| (line[i].x - state.pos_x).powi(2) + (line[i].y - state.pos_y).powi(2);
            let mut best = (from, d2(from));
            for off in 1..=LEG_SEARCH_POINTS {
                for i in [(from + off) % n, (from + n - off) % n] {
                    let d = d2(i);
                    if d < best.1 {
                        best = (i, d);
                    }
                }
            }
            Some(best.0 as u32)
        }
        _ => state.nearest_centerline_idx,
    };
}

/// Where along the lap the leg the car is on puts it, m: its own lap
/// position until a leg has been followed.
pub fn leg_m(rc: &Racecraft, state: &CarState, track: &TrackConfig) -> f32 {
    rc.leg_idx
        .and_then(|i| track.centerline.get(i as usize))
        .map_or(state.track_progress, |p| p.distance_from_start_m)
}

/// A racing car coming on this stretch of road: behind `from_m` (or
/// alongside) within [`REJOIN_LOOK_M`] and arriving within
/// [`REJOIN_GAP_S`], and near in the world too (a lap position can be on
/// another leg).
fn traffic_coming(me: &Rival, from_m: f32, field: &[Rival], total_m: f32) -> bool {
    field.iter().filter(|o| o.id != me.id && o.racing).any(|o| {
        let behind = -ahead_of(from_m, o.progress_m, total_m);
        let near = (o.pos.0 - me.pos.0).hypot(o.pos.1 - me.pos.1) < REJOIN_LOOK_M + 50.0;
        behind > -10.0
            && behind < REJOIN_LOOK_M
            && near
            && behind.max(0.0) / o.speed_mps.max(1.0) < REJOIN_GAP_S
    })
}

/// How far the car points from the road 20 m ahead of `from_m` along the
/// lap, rad (positive: the road is to its left).
pub fn road_heading_error(state: &CarState, track: &TrackConfig, from_m: f32, total_m: f32) -> f32 {
    let ahead = (from_m + 20.0).rem_euclid(total_m.max(1.0));
    let i = track
        .centerline
        .partition_point(|p| p.distance_from_start_m < ahead)
        .min(track.centerline.len().saturating_sub(1));
    let Some(p) = track.centerline.get(i) else {
        return 0.0;
    };
    let bearing = (p.y - state.pos_y).atan2(p.x - state.pos_x);
    (bearing - state.yaw_rad + std::f32::consts::PI).rem_euclid(std::f32::consts::TAU)
        - std::f32::consts::PI
}

/// Back on the line, after a pass or one given up.
fn back_to_racing(rc: Racecraft, cooldown_s: f32) -> Racecraft {
    Racecraft {
        tactic: Tactic::Race,
        rival: None,
        since_s: 0.0,
        cooldown_s: rc.cooldown_s.max(cooldown_s),
        corner: None,
        ..rc
    }
}

/// Move the blend toward the tactic's lane, or back to the line.
fn settle(mut rc: Racecraft, dt: f32) -> Racecraft {
    let (to, rate) = match rc.tactic {
        Tactic::Race => (0.0, BLEND_RETURN_PER_S),
        Tactic::Attack | Tactic::Room => (1.0, BLEND_ATTACK_PER_S),
        Tactic::Defend | Tactic::Yield => (1.0, BLEND_DEFEND_PER_S),
    };
    let step = rate * dt;
    rc.blend = if rc.blend < to {
        (rc.blend + step).min(to)
    } else {
        (rc.blend - step).max(to)
    };
    rc
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A loop of straights and four left-hand corners: the profile
    /// brakes into each and the corner is found to the left.
    fn square_profile() -> RacingLineProfile {
        rectangle_profile(400.0, 400.0)
    }

    /// A rectangle `long` by `short` m, anticlockwise from the origin
    /// along x: braking over the last 100 m of each side, back up over the
    /// first 100 m of the next.
    fn rectangle_profile(long: f32, short: f32) -> RacingLineProfile {
        let sp = 2.5;
        let mut points = Vec::new();
        let mut speed = Vec::new();
        let mut phases = Vec::new();
        for s in 0..4 {
            let side = if s % 2 == 0 { long } else { short };
            let per_side = (side / sp) as usize;
            for i in 0..per_side {
                let t = i as f32 * sp;
                let (x, y) = match s {
                    0 => (t, 0.0),
                    1 => (long, t),
                    2 => (long - t, short),
                    _ => (0.0, short - t),
                };
                points.push([x, y, 0.0]);
                // Braking over the last 100 m of each side, slowest at the
                // corner, back up over the first 100 m of the next.
                let (v, ph) = if t > side - 100.0 {
                    (80.0 - (t - (side - 100.0)) * 0.5, LinePhase::Brake)
                } else if t < 100.0 {
                    (30.0 + t * 0.5, LinePhase::Throttle)
                } else {
                    (80.0, LinePhase::Throttle)
                };
                speed.push(v);
                phases.push(ph);
            }
        }
        RacingLineProfile {
            spacing_m: sp,
            points,
            speed_mps: speed,
            phases,
        }
    }

    #[test]
    fn the_next_corner_is_found_with_its_inside() {
        let p = square_profile();
        // 100 m into the first side: braking starts at 300 m.
        let c = next_corner(&p, 40).expect("a corner ahead");
        assert!((c.brake_in_m - 202.5).abs() < 5.0, "{c:?}");
        assert_eq!(c.inside, 1.0, "the square turns left: {c:?}");
        // Braking: the same corner, braking now.
        let c2 = next_corner(&p, 140).expect("a corner ahead");
        assert_eq!(c2.brake_in_m, 0.0);
        assert_eq!(c2.apex, c.apex);
    }

    /// A driver held up passes into the corner at the end of the long
    /// straight, not the short one before it.
    #[test]
    fn the_pass_is_planned_at_the_end_of_the_long_straight() {
        let p = rectangle_profile(800.0, 200.0);
        // 20 m into the short side: the corner ahead has a 100 m run, the
        // one after it 700 m.
        let here = (820.0 / p.spacing_m) as usize;
        let c = next_corner(&p, here).expect("a corner ahead");
        assert!(!pass_here(&p, here, &c, false));
        assert!(!pass_here(&p, here, &c, true), "DRS on a short run");
        // On the long side the corner ahead is the place.
        let here = (20.0 / p.spacing_m) as usize;
        let c = next_corner(&p, here).expect("a corner ahead");
        assert!(pass_here(&p, here, &c, false));
        // Every corner of a square is as good as the next.
        let sq = square_profile();
        let c = next_corner(&sq, 40).expect("a corner ahead");
        assert!(pass_here(&sq, 40, &c, false));
    }

    /// Me on the square at `t` m along its first side at 60 m/s; the
    /// road 5 m either side.
    fn on_the_square(id: u64, t: f32) -> CarState {
        let track = TrackConfig::default();
        let mut state = CarState::new(
            uuid::Uuid::from_u64_pair(id, 3),
            uuid::Uuid::new_v4(),
            &track.start_positions[0],
        );
        (state.pos_x, state.pos_y) = (t, 0.0);
        state.track_progress = t;
        state.speed_mps = 60.0;
        state.yaw_rad = 0.0;
        state.is_on_track = true;
        state.nearest_centerline_idx = None;
        state
    }

    /// A car `gap` m behind `me` (nose to tail), `closing` m/s faster,
    /// `left_m` across the road.
    fn chaser(me: &Rival, gap: f32, closing: f32, left_m: f32) -> Rival {
        let at = me.progress_m - gap - me.length_m;
        Rival {
            id: uuid::Uuid::from_u64_pair(99, 99),
            progress_m: at,
            pos: (at, 0.0),
            race_m: at,
            left_m,
            speed_mps: me.speed_mps + closing,
            racing: true,
            ..*me
        }
    }

    fn decide_on_square(state: &CarState, field: &[Rival]) -> Racecraft {
        let track = TrackConfig::default();
        let ctx = Context {
            track: &track,
            total_m: 1600.0,
            race: true,
            dt: 0.01,
        };
        let bold = AiDriverProfile::with_attributes("Bold", 100, 1.0, 0.8, 100, 0.8, 0.2, 0.9);
        let speeds = square_profile();
        let me = Rival::of(state, 4.5, 2.0, ctx.total_m);
        let mut all = vec![me];
        all.extend_from_slice(field);
        decide(state, &me, &bold, Some(&speeds), &all, &ctx)
    }

    /// A driver whose defence roll comes up for the square's first corner.
    fn defender_id() -> u64 {
        let speeds = square_profile();
        let apex = next_corner(&speeds, 40).expect("a corner").apex;
        (1..100)
            .find(|&id| roll(uuid::Uuid::from_u64_pair(id, 3), 0, apex, 3) < 0.85)
            .expect("a bold driver defends")
    }

    /// A defence is made only against a car that can get alongside by
    /// the braking: not one 20 m back closing at 1 m/s.
    #[test]
    fn a_defence_judges_how_fast_the_car_behind_is_coming() {
        let state = on_the_square(defender_id(), 150.0);
        let me = Rival::of(&state, 4.5, 2.0, 1600.0);
        let slow = decide_on_square(&state, &[chaser(&me, 20.0, 1.0, 0.0)]);
        assert_eq!(slow.tactic, Tactic::Race, "{slow:?}");
        let fast = decide_on_square(&state, &[chaser(&me, 20.0, 8.0, 0.0)]);
        assert_eq!(fast.tactic, Tactic::Defend, "{fast:?}");
        assert!(
            fast.lane_m > 2.0,
            "to the inside of the left-hander: {fast:?}"
        );

        // One move a corner: once made, never another for that corner.
        let mut again = state.clone();
        again.racecraft = Racecraft {
            tactic: Tactic::Race,
            rival: None,
            ..fast
        };
        let second = decide_on_square(&again, &[chaser(&me, 20.0, 8.0, 0.0)]);
        assert_eq!(second.tactic, Tactic::Race, "{second:?}");
    }

    /// Far out on the straight a car closing in the tow is covered early;
    /// one already pulled out of the wake, or arriving before the move is
    /// made, is not.
    #[test]
    fn the_tow_is_covered_on_the_straight() {
        let state = on_the_square(defender_id(), 20.0);
        let me = Rival::of(&state, 4.5, 2.0, 1600.0);
        let towing = decide_on_square(&state, &[chaser(&me, 15.0, 6.0, 0.0)]);
        assert_eq!(towing.tactic, Tactic::Defend, "{towing:?}");
        let out = decide_on_square(&state, &[chaser(&me, 15.0, 6.0, -2.0)]);
        assert_eq!(out.tactic, Tactic::Race, "{out:?}");
        let late = decide_on_square(&state, &[chaser(&me, 5.0, 6.0, 0.0)]);
        assert_eq!(late.tactic, Tactic::Race, "{late:?}");
    }

    #[test]
    fn a_roll_is_the_same_every_time() {
        let id = uuid::Uuid::from_u64_pair(1, 2);
        assert_eq!(roll(id, 3, 40, 1), roll(id, 3, 40, 1));
        assert_ne!(roll(id, 3, 40, 1), roll(id, 4, 40, 1));
    }

    #[test]
    fn the_blend_eases_on_and_off_a_lane() {
        let mut rc = Racecraft {
            tactic: Tactic::Attack,
            ..Racecraft::default()
        };
        for _ in 0..100 {
            rc = settle(rc, 0.01);
        }
        assert!(rc.blend > 0.99);
        rc.tactic = Tactic::Race;
        rc = settle(rc, 0.5);
        assert!((rc.blend - 0.75).abs() < 1e-4);
    }

    /// Under pressure an inconsistent driver gets corners wrong in every
    /// way, each the same every time it is rolled; each mistake plays out
    /// where it should and ends with its corner.
    #[test]
    fn mistakes_under_pressure_play_out_in_their_corner() {
        let track = TrackConfig::default();
        let ctx = Context {
            track: &track,
            total_m: 4000.0,
            race: true,
            dt: 0.01,
        };
        let profile = AiDriverProfile::with_attributes("Wild", 70, 0.5, 0.5, 200, 0.5, 1.0, 0.0);
        let mut state = CarState::new(
            uuid::Uuid::from_u64_pair(7, 9),
            uuid::Uuid::new_v4(),
            &track.start_positions[0],
        );
        state.track_progress = 1000.0;
        state.speed_mps = 60.0;
        state.is_on_track = true;
        let me = Rival::of(&state, 4.5, 2.0, ctx.total_m);
        let chaser = Rival {
            id: uuid::Uuid::from_u64_pair(1, 1),
            progress_m: 994.0,
            ..me
        };
        let behind = Some((&chaser, -6.0));
        let calm = MistakeRisk::default();
        let approach = |apex: usize| Corner {
            apex,
            brake_in_m: 100.0,
            apex_in_m: 200.0,
            inside: 1.0,
        };

        // Roll corners until each kind has come up.
        let mut seen = Vec::new();
        let mut by_kind = std::collections::BTreeMap::new();
        for apex in 0..400 {
            let mut rc = Racecraft::default();
            let mut again = Racecraft::default();
            let corner = approach(apex);
            mistakes(
                &mut rc, &state, &me, &profile, &corner, false, behind, &calm, 60.0, 0.01, &ctx,
            );
            mistakes(
                &mut again, &state, &me, &profile, &corner, false, behind, &calm, 60.0, 0.01, &ctx,
            );
            assert_eq!(rc, again, "a roll is the same every time");
            if !seen.contains(&rc.mistake) {
                seen.push(rc.mistake);
            }
            by_kind.entry(format!("{:?}", rc.mistake)).or_insert(apex);
        }
        assert_eq!(seen.len(), 5, "every kind and none: {seen:?}");

        let rolled = |kind: &str| {
            let apex = by_kind[kind];
            let mut rc = Racecraft::default();
            mistakes(
                &mut rc,
                &state,
                &me,
                &profile,
                &approach(apex),
                false,
                behind,
                &calm,
                60.0,
                0.01,
                &ctx,
            );
            (rc, apex)
        };

        // A lock-up waits for the late braking, then holds the pedal.
        let (mut rc, apex) = rolled("LockUp");
        let early = Corner {
            brake_in_m: 0.0,
            apex_in_m: 150.0,
            ..approach(apex)
        };
        mistakes(
            &mut rc, &state, &me, &profile, &early, true, None, &calm, 50.0, 0.01, &ctx,
        );
        assert_eq!(rc.lock_up_s, 0.0);
        let late = Corner {
            apex_in_m: 60.0,
            ..early
        };
        mistakes(
            &mut rc, &state, &me, &profile, &late, true, None, &calm, 40.0, 0.01, &ctx,
        );
        assert!(rc.lock_up_s >= LOCK_UP_S, "{rc:?}");
        assert_eq!(rc.mistake, Mistake::None, "once a corner");

        // A missed apex runs wide, to the outside of a left-hander, and
        // comes back after it.
        let (mut rc, apex) = rolled("MissedApex");
        let turning = Corner {
            brake_in_m: 0.0,
            apex_in_m: 40.0,
            ..approach(apex)
        };
        for _ in 0..100 {
            mistakes(
                &mut rc, &state, &me, &profile, &turning, true, None, &calm, 40.0, 0.01, &ctx,
            );
        }
        assert!(rc.wide_m < -MISSED_APEX_M * 0.99, "{rc:?}");
        let next = approach(apex + 500);
        for _ in 0..200 {
            mistakes(
                &mut rc, &state, &me, &profile, &next, false, None, &calm, 40.0, 0.01, &ctx,
            );
        }
        assert_eq!(rc.wide_m, 0.0);

        // A missed shift comes on the way out of the corner.
        let (mut rc, apex) = rolled("MissedShift");
        mistakes(
            &mut rc,
            &state,
            &me,
            &profile,
            &Corner {
                brake_in_m: 0.0,
                ..approach(apex)
            },
            true,
            None,
            &calm,
            40.0,
            0.01,
            &ctx,
        );
        assert_eq!(rc.missed_shift_s, 0.0);
        mistakes(
            &mut rc,
            &state,
            &me,
            &profile,
            &approach(apex + 500),
            false,
            None,
            &calm,
            40.0,
            0.01,
            &ctx,
        );
        assert!(rc.missed_shift_s >= MISSED_SHIFT_S, "{rc:?}");
        assert_eq!(rc.mistake_corner, None);

        // A consistent driver never errs.
        let ace = AiDriverProfile::with_attributes("Ace", 110, 0.5, 1.0, 50, 1.0, 0.0, 1.0);
        for apex in 0..400 {
            let mut rc = Racecraft::default();
            mistakes(
                &mut rc,
                &state,
                &me,
                &ace,
                &approach(apex),
                false,
                behind,
                &calm,
                60.0,
                0.01,
                &ctx,
            );
            assert_eq!(rc.mistake, Mistake::None);
        }
    }

    /// A hairpin: out along y = 0 for 500 m, round a 15 m radius, back
    /// along y = 30. The two legs run 30 m apart.
    fn hairpin() -> TrackConfig {
        let mut centerline = Vec::new();
        let mut d = 0.0;
        let mut push = |x: f32, y: f32, heading: f32, d: f32| {
            centerline.push(crate::data::TrackPoint {
                x,
                y,
                distance_from_start_m: d,
                heading_rad: heading,
                width_left_m: 6.0,
                width_right_m: 6.0,
                ..crate::data::TrackPoint::default()
            });
        };
        for i in 0..250 {
            push(i as f32 * 2.0, 0.0, 0.0, d);
            d += 2.0;
        }
        for i in 0..24 {
            let a = -std::f32::consts::FRAC_PI_2 + i as f32 * std::f32::consts::PI / 24.0;
            push(
                500.0 + 15.0 * a.cos(),
                15.0 + 15.0 * a.sin(),
                a + std::f32::consts::FRAC_PI_2,
                d,
            );
            d += 15.0 * std::f32::consts::PI / 24.0;
        }
        for i in 0..250 {
            push(500.0 - i as f32 * 2.0, 30.0, std::f32::consts::PI, d);
            d += 2.0;
        }
        TrackConfig {
            centerline,
            ..TrackConfig::default()
        }
    }

    /// Off the road a car keeps to the leg it left, though the other leg
    /// is nearer and its lap position has jumped to it; on the road it is
    /// its own nearest point again.
    #[test]
    fn off_the_road_the_leg_left_is_followed() {
        let track = hairpin();
        let mut state = CarState::new(
            uuid::Uuid::new_v4(),
            uuid::Uuid::new_v4(),
            &TrackConfig::default().start_positions[0],
        );
        let mut rc = Racecraft::default();
        (state.pos_x, state.pos_y) = (250.0, 0.0);
        state.is_on_track = true;
        state.nearest_centerline_idx = Some(125);
        follow_leg(&mut rc, &state, &track);
        assert_eq!(rc.leg_idx, Some(125));

        // Slides across toward the other leg, past halfway to it.
        state.is_on_track = false;
        for step in 1..=20 {
            state.pos_y = step as f32;
            state.pos_x = 250.0 + step as f32;
            follow_leg(&mut rc, &state, &track);
        }
        let back_leg = track
            .centerline
            .iter()
            .position(|p| p.y == 30.0 && p.x <= 270.0)
            .unwrap();
        state.nearest_centerline_idx = Some(back_leg as u32);
        state.track_progress = track.centerline[back_leg].distance_from_start_m;
        follow_leg(&mut rc, &state, &track);
        let leg = leg_m(&rc, &state, &track);
        assert!((leg - 270.0).abs() < 3.0, "on the outbound leg: {leg}");
        // Pointing back along the outbound leg, the road ahead is ahead.
        state.yaw_rad = 0.0;
        let error = road_heading_error(&state, &track, leg, 1500.0);
        assert!(error.abs() < 1.0, "{error}");
        let wrong = road_heading_error(&state, &track, state.track_progress, 1500.0);
        assert!(wrong.abs() > 2.0, "the other leg is behind: {wrong}");

        // Back on the road: its own nearest point.
        state.is_on_track = true;
        follow_leg(&mut rc, &state, &track);
        assert_eq!(rc.leg_idx, Some(back_leg as u32));
    }

    /// Alone, a driver gets corners wrong only with something against it
    /// (cold tyres, a wet patch, a long stint), less often than under
    /// pressure, and never misses a shift for a cold tyre or a puddle.
    #[test]
    fn mistakes_come_from_cold_tyres_and_wet_patches_too() {
        let track = TrackConfig::default();
        let ctx = Context {
            track: &track,
            total_m: 4000.0,
            race: true,
            dt: 0.01,
        };
        let profile = AiDriverProfile::with_attributes("Wild", 70, 0.5, 0.5, 200, 0.5, 1.0, 0.0);
        let mut state = CarState::new(
            uuid::Uuid::from_u64_pair(7, 9),
            uuid::Uuid::new_v4(),
            &track.start_positions[0],
        );
        state.track_progress = 1000.0;
        state.speed_mps = 60.0;
        state.is_on_track = true;
        let me = Rival::of(&state, 4.5, 2.0, ctx.total_m);
        let chaser = Rival {
            id: uuid::Uuid::from_u64_pair(1, 1),
            progress_m: 994.0,
            ..me
        };
        let count = |risk: &MistakeRisk, behind: Option<(&Rival, f32)>| {
            let mut kinds = std::collections::BTreeSet::new();
            let mut n = 0;
            for apex in 0..2000 {
                let mut rc = Racecraft::default();
                let corner = Corner {
                    apex,
                    brake_in_m: 100.0,
                    apex_in_m: 200.0,
                    inside: 1.0,
                };
                mistakes(
                    &mut rc, &state, &me, &profile, &corner, false, behind, risk, 60.0, 0.01, &ctx,
                );
                if rc.mistake != Mistake::None {
                    n += 1;
                    kinds.insert(format!("{:?}", rc.mistake));
                }
            }
            (n, kinds)
        };
        let calm = MistakeRisk::default();
        assert_eq!(count(&calm, None).0, 0, "nothing against it, no mistakes");
        let (pressured, _) = count(&calm, Some((&chaser, -6.0)));
        let cold = MistakeRisk { cold: 1.0, ..calm };
        let (n_cold, kinds) = count(&cold, None);
        assert!(n_cold > 0 && n_cold * 3 < pressured, "{n_cold} {pressured}");
        assert!(!kinds.contains("MissedShift"), "{kinds:?}");
        let wet = MistakeRisk { wet: 1.0, ..calm };
        let (n_wet, _) = count(&wet, None);
        assert!(n_wet > n_cold && n_wet < pressured, "{n_wet} {n_cold}");

        // The risk itself: tyres at the air on a cold morning, water over
        // what the road was baked at, four hours in the car.
        let tire = crate::data::TireConfig::default();
        state.tyres_fitted = true;
        for t in state.tires.each_mut() {
            t.temperature_c = 20.0;
            t.core_temperature_c = 20.0;
        }
        state.tyre_water = [0.0, 0.0, 0.4, 0.0];
        let risk = MistakeRisk::of(&state, &tire, 0.1, 5.0 * 3600.0);
        assert_eq!(risk.cold, 1.0);
        assert!((risk.wet - 1.0).abs() < 1e-5, "{risk:?}");
        assert_eq!(risk.fatigue, 1.0);
        let opt = crate::tyre_thermal::optimum_c(&tire, tire.compound(state.tyre_compound));
        for t in state.tires.each_mut() {
            t.temperature_c = opt;
            t.core_temperature_c = opt;
        }
        state.tyre_water = [0.1; 4];
        assert_eq!(
            MistakeRisk::of(&state, &tire, 0.1, 600.0),
            MistakeRisk::default(),
            "warm, on the road as baked, fresh"
        );
    }

    /// Stopped off the road with a car coming, a driver waits for it
    /// before backing out or setting off, but not for ever; with the road
    /// clear it gets on with it.
    #[test]
    fn a_stopped_car_waits_for_a_gap() {
        let track = hairpin();
        let ctx = Context {
            track: &track,
            total_m: 1100.0,
            race: true,
            dt: 0.1,
        };
        let mut state = CarState::new(
            uuid::Uuid::new_v4(),
            uuid::Uuid::new_v4(),
            &TrackConfig::default().start_positions[0],
        );
        (state.pos_x, state.pos_y) = (250.0, -8.0);
        state.track_progress = 250.0;
        state.nearest_centerline_idx = Some(125);
        state.is_on_track = false;
        state.speed_mps = 0.0;
        let me = Rival::of(&state, 4.5, 2.0, ctx.total_m);
        let coming = Rival {
            id: uuid::Uuid::from_u64_pair(5, 5),
            progress_m: 150.0,
            pos: (150.0, 0.0),
            speed_mps: 50.0,
            racing: true,
            ..me
        };
        let far = Rival {
            progress_m: 900.0,
            pos: (100.0, 30.0),
            ..coming
        };

        let mut rc = Racecraft::default();
        for _ in 0..20 {
            recover(&mut rc, &state, &me, &[me, coming], &ctx);
        }
        assert!(rc.waiting, "{rc:?}");
        assert_eq!(rc.stuck_s, 0.0, "not stuck while it waits");
        assert_eq!(rc.reverse_s, 0.0);
        for _ in 0..50 {
            recover(&mut rc, &state, &me, &[me, coming], &ctx);
        }
        assert!(!rc.waiting, "waited long enough: {rc:?}");

        let mut rc = Racecraft::default();
        for _ in 0..20 {
            recover(&mut rc, &state, &me, &[me, far], &ctx);
        }
        assert!(!rc.waiting, "the road behind is clear: {rc:?}");
        assert!(rc.reverse_s > 0.0, "and it backs out: {rc:?}");

        // Backing out, a car arriving stops it where it is, with its
        // back-out held; it carries on once the car has gone by.
        let backing = rc.reverse_s;
        state.gear = -1;
        state.is_on_track = true;
        let me = Rival::of(&state, 4.5, 2.0, ctx.total_m);
        recover(&mut rc, &state, &me, &[me, coming], &ctx);
        assert!(rc.waiting, "{rc:?}");
        assert_eq!(rc.reverse_s, backing, "the back-out is held");
        let gone = Rival {
            progress_m: 400.0,
            pos: (400.0, 0.0),
            ..coming
        };
        recover(&mut rc, &state, &me, &[me, gone], &ctx);
        assert!(!rc.waiting, "{rc:?}");
        assert!(rc.reverse_s < backing, "and it backs on");
    }
}
