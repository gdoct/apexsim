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
//!   has the corner. Either way the lane ends at the apex.
//! - **Defend** (races only): a car closing behind on the approach to a
//!   braking zone gets one move to the inside, made before the braking,
//!   held to the apex. A car already alongside is too late to block.
//! - **Room**: with an attacker alongside, the driver being passed takes
//!   the lane beside it and holds it, rather than turning in on it.
//! - **Yield**: about to be lapped, the driver moves off the line toward
//!   the outside of the next corner and lifts a little.
//! - **Recovery**: stuck against a wall or off the road at a crawl, the
//!   driver backs out in reverse, swinging the nose toward the road, and
//!   drives on. Before it a car pinned on a barrier sat there with its
//!   foot down until its engine cooked.
//! - **Mistakes**: under pressure (a car close behind) a driver now and
//!   then overcooks a corner, by how inconsistent it is.
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
/// The share of drivers who defend at all: the most timid and the boldest.
const DEFEND_CHANCE_TIMID: f32 = 0.35;
const DEFEND_CHANCE_BOLD: f32 = 0.9;
/// A car being lapped moves this far toward the outside and lifts.
const YIELD_MOVE_M: f32 = 2.5;
const YIELD_PACE: f32 = 0.97;
/// Under pressure (a car within [`PRESSURE_GAP_S`] behind) a driver
/// overcooks a corner with this chance per unit of inconsistency, by this
/// much of the corner's speed and up to this much more.
const PRESSURE_GAP_S: f32 = 0.6;
const MISTAKE_CHANCE: f32 = 0.35;
const MISTAKE_OVERSPEED: f32 = 0.025;
const MISTAKE_OVERSPEED_SPREAD: f32 = 0.02;
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
        && !state.towed
        && state.damage.is_drivable
        && state.finish_position.is_none()
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
    ctx: &Context<'_>,
) -> Racecraft {
    let mut rc = state.racecraft;
    let dt = ctx.dt;
    rc.cooldown_s = (rc.cooldown_s - dt).max(0.0);
    rc.since_s += dt;
    rc.pace = 1.0;
    recover(&mut rc, state, ctx);
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

    // Mistakes under pressure, rolled once a corner.
    let pressured =
        behind.is_some_and(|(o, rel)| -rel - reach(o) < PRESSURE_GAP_S * speed + CLOSE_EXTRA_M);
    if ctx.race
        && pressured
        && corner.brake_in_m < 150.0
        && rc.mistake_corner != Some(corner.apex as u32)
    {
        rc.mistake_corner = Some(corner.apex as u32);
        let inconsistency = (1.0 - profile.consistency).clamp(0.0, 1.0);
        rc.overspeed =
            if roll(me.id, state.current_lap, corner.apex, 1) < MISTAKE_CHANCE * inconsistency {
                MISTAKE_OVERSPEED
                    + MISTAKE_OVERSPEED_SPREAD * roll(me.id, state.current_lap, corner.apex, 2)
            } else {
                0.0
            };
    }
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
    // close behind and coming (or attacking us), not yet alongside, on the
    // same lap.
    if ctx.race && corner.inside != 0.0 {
        if let Some((o, rel)) = behind {
            let gap = -rel - reach(o);
            let coming = o.speed_mps > me.speed_mps + 0.5
                || (o.tactic == Tactic::Attack && o.rival == Some(me.id));
            let window = corner.brake_in_m >= DEFEND_FROM_M
                && corner.brake_in_m <= DEFEND_FROM_M + DEFEND_REACH_S * speed;
            let same_lap = (o.race_m - me.race_m).abs() < 0.5 * ctx.total_m;
            if gap > 0.5
                && gap < DEFEND_GAP_S * speed + CLOSE_EXTRA_M
                && coming
                && window
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
                    return Some(fresh(Tactic::Defend, o.id, clamp_lane(lane)));
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
    if !close || !(crawling || (rc.held_s >= hesitate && approach)) {
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

/// Getting unstuck: count the time crawling against a wall or off the road,
/// back out once it is long enough, and stop backing once clear and
/// pointing at the road (or out of time).
fn recover(rc: &mut Racecraft, state: &CarState, ctx: &Context<'_>) {
    let dt = ctx.dt;
    rc.recover_cooldown_s = (rc.recover_cooldown_s - dt).max(0.0);
    let out_of_it = state.pit.driving
        || state.in_garage
        || state.towed
        || !state.damage.is_drivable
        || state.finish_position.is_some();
    if out_of_it {
        rc.stuck_s = 0.0;
        rc.reverse_s = 0.0;
        return;
    }
    if rc.reverse_s > 0.0 {
        rc.reverse_s = (rc.reverse_s - dt).max(0.0);
        let backed = REVERSE_MAX_S - rc.reverse_s;
        let clear = !state.is_colliding
            && road_heading_error(state, ctx.track, ctx.total_m).abs() < REVERSE_DONE_HEADING_RAD;
        if backed >= REVERSE_MIN_S && clear {
            rc.reverse_s = 0.0;
        }
        if rc.reverse_s <= 0.0 {
            rc.recover_cooldown_s = RECOVER_COOLDOWN_S;
        }
        return;
    }
    let stuck = state.speed_mps < STUCK_SPEED_MPS && (state.is_colliding || !state.is_on_track);
    rc.stuck_s = if stuck { rc.stuck_s + dt } else { 0.0 };
    if rc.stuck_s >= STUCK_AFTER_S && rc.recover_cooldown_s <= 0.0 {
        rc.stuck_s = 0.0;
        rc.reverse_s = REVERSE_MAX_S;
    }
}

/// How far the car points from the road 20 m ahead of it, rad (positive:
/// the road is to its left).
pub fn road_heading_error(state: &CarState, track: &TrackConfig, total_m: f32) -> f32 {
    let ahead = (state.track_progress + 20.0).rem_euclid(total_m.max(1.0));
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
        let sp = 2.5;
        let side = 400.0;
        let mut points = Vec::new();
        let mut speed = Vec::new();
        let mut phases = Vec::new();
        let per_side = (side / sp) as usize;
        for s in 0..4 {
            for i in 0..per_side {
                let t = i as f32 * sp;
                let (x, y) = match s {
                    0 => (t, 0.0),
                    1 => (side, t),
                    2 => (side - t, side),
                    _ => (0.0, side - t),
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
}
