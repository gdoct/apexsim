use crate::ai_driver::{AiDriverController, AiDriverProfile, TrafficCar};
use crate::car_setup::CarSetup;
use crate::car_step;
use crate::data::*;
use crate::ghost::{GhostLap, GhostSample, GHOST_SAMPLE_HZ};
use crate::laps::{LapEvent, SECTOR_COUNT};
use crate::physics;
use crate::racing_line::{self, RacingLineProfile};
use crate::tyre_thermal;
use std::collections::HashMap;
use tracing::{debug, info};

/// Default simulation tick rate; used when no explicit rate is configured
/// (tests, benches, the offline tools). 240 Hz until 2026-10-06.
pub const DEFAULT_TICK_RATE_HZ: u16 = 420;

/// Longest countdown a client may ask for. The tick count is a `u16`, so
/// anything longer would wrap (at 420 Hz past 156 s).
pub const MAX_COUNTDOWN_SECONDS: u16 = 60;

/// How long `StartSession` counts down before the race goes green.
pub const START_SESSION_COUNTDOWN_SECONDS: u16 = 5;

/// Once the winner has finished, the rest of the field has this many of the
/// winner's average laps to complete the distance...
pub const FINISH_GRACE_LAPS: f32 = 2.0;
/// ...but never less than this. Past it the race ends with whoever has not
/// finished unclassified, so a car stuck in a gravel trap (or a player who
/// walked away) cannot hold the session open forever.
pub const FINISH_GRACE_MIN_SECONDS: u32 = 60;
/// A timed qualifying session ends at most this many laps (the slowest
/// car's estimate) after its chequered flag comes out...
pub const QUALIFYING_FLAG_GRACE_LAPS: f32 = 2.0;
/// ...but never sooner than this: the cars still out by then are put in
/// their garages and the session is over.
pub const QUALIFYING_FLAG_GRACE_MIN_SECONDS: u32 = 60;
/// A shoot-out run (outlap and flying lap) ends at this many of the car's
/// planned laps if it has not crossed the line at the end of its flying lap
/// by then: a car stuck or lost does not hold the field up.
pub const SHOOTOUT_RUN_LAPS: f32 = 3.0;
/// ... and never sooner than this, s.
pub const SHOOTOUT_RUN_MIN_SECONDS: u32 = 180;
/// Skill of the server driver that takes a human's car round on the
/// cool-down lap after they finish: unhurried, off the racing pace.
const COOLDOWN_SKILL: u8 = 75;

/// Laps of fuel a hotlap or qualifying run goes out with, before the
/// driver's fuel knob: light enough to be quick, enough for a warm-up lap,
/// a flying lap and the way in.
pub const HOTLAP_FUEL_LAPS: f32 = 3.0;
/// Share of a race's distance fuelled on top of it: what the estimate may
/// fall short of a car driven at the limit (`racing_line::lap_fuel_liters`
/// plans a little inside it).
pub const RACE_FUEL_MARGIN: f32 = 0.08;
/// Laps of fuel a race starts with beyond its distance and margin, before
/// the knob: the cool-down lap.
pub const RACE_FUEL_RESERVE_LAPS: f32 = 1.0;
/// Least fuel a car is ever sent out with, in laps, whatever the knob says.
pub const MIN_FUEL_LAPS: f32 = 1.0;
/// A lap's length in a timed race when nothing better is known (no lap
/// driven, no line to plan on), s: only the AI's pit planning reads it.
const FALLBACK_LAP_SECONDS: f32 = 120.0;

/// How far before the line a hotlap car is put out, so the first flying lap
/// starts at speed. Shortened on a track too small for it.
pub const HOTLAP_RUNUP_M: f32 = 300.0;

/// How long a car out of the race stands where it stopped before it is
/// towed away, s: long enough to be seen, as a marshal's yellow is.
pub const TOW_AFTER_S: f32 = 10.0;

/// A hit this hard, percent of damage, sheds a piece of debris onto the
/// road where it happened; one this hard may puncture the tyre nearest
/// it, with a chance that grows with the hit to this most
/// (`GameSession::update_debris`).
pub const DEBRIS_HIT_PCT: f32 = 8.0;
pub const PUNCTURE_HIT_PCT: f32 = 5.0;
pub const PUNCTURE_HIT_MAX_CHANCE: f32 = 0.6;
/// How long a piece of debris stays on the road, s, how many pieces at
/// most, how close a wheel has to pass to pick one up, m, and the chance
/// that it punctures the tyre that does.
pub const DEBRIS_LIFE_S: f32 = 90.0;
pub const MAX_DEBRIS: usize = 32;
pub const DEBRIS_REACH_M: f32 = 1.0;
/// A piece a wheel runs over strikes the bodywork over it: this much damage
/// at 50 m/s (less slower, a quarter of it at least), on the nose for a
/// front wheel and the tail for a rear one, and this share of the car's
/// speed scrubbed off.
pub const DEBRIS_STRIKE_PCT: f32 = 3.0;
pub const DEBRIS_STRIKE_SCRUB: f32 = 0.01;
pub const DEBRIS_PUNCTURE_CHANCE: f32 = 0.3;
/// Half the punctures are slow: a leak of this many kPa/s at least and at
/// most at the set pressure (`tyre_thermal::leak_rate_kpa_per_s` moves it
/// with the tyre's heat and pressure), by its cause. A piece of debris
/// stuck in the tread is a small hole (the tyre flat in one to several
/// laps); a hard hit cuts the sidewall or unseats the bead (flat in half
/// a lap to a lap or two).
const DEBRIS_LEAK_KPA_PER_S: (f32, f32) = (0.6, 3.5);
const HIT_LEAK_KPA_PER_S: (f32, f32) = (2.0, 8.0);

/// A piece of debris on the road.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Debris {
    pub x: f32,
    pub y: f32,
    pub until_tick: u32,
}

/// How a hashed puncture goes: a slow leak (half the time) of a rate in
/// `range` (its cause's), or at once.
fn slow_leak(seed: u64, range: (f32, f32)) -> Option<f32> {
    if crate::wind::hash01(seed, 0x5107) < 0.5 {
        let (lo, hi) = range;
        Some(lo + (hi - lo) * crate::wind::hash01(seed, 0x5108))
    } else {
        None
    }
}

/// A session's track with its road state (`crate::road_state`): the
/// water the baked weather leaves, the start's rubber on the line.
fn with_road(mut track: TrackConfig, conditions: &SessionConditions) -> TrackConfig {
    if track.road_state.is_none() {
        track.road_state = Some(crate::road_state::RoadState::new(
            &track,
            conditions.track_rubber(),
        ));
    }
    track
}

/// A session's own copy of `track`: `conditions` (resolved, as the session
/// echoes them) baked in (`SessionConditions::apply_to_track`) and its road
/// state laid, as the server makes it for a new session. What the client's
/// prediction loads (`server/ffi`).
pub fn session_track(mut track: TrackConfig, conditions: &SessionConditions) -> TrackConfig {
    conditions.apply_to_track(&mut track);
    with_road(track, conditions)
}

/// The live sky of `session` over `track` (`crate::conditions`).
/// Salt for an AI's strategy roll each lap (`pit::StopContext::roll`).
const STRATEGY_SALT: u64 = 0x5747_5241_5445_4759;
/// And for its nerve on the weather's tyres (`pit::CrossoverContext::roll`).
const CROSSOVER_SALT: u64 = 0x4352_4f53_534f_5652;

/// The [`AI_POWER_PLAN_STEP`]s of power a car's engine damage has cost it.
fn power_steps_lost(state: &CarState) -> u8 {
    let lost = 1.0 - crate::damage::engine_power_factor(&state.damage);
    (lost / AI_POWER_PLAN_STEP).round().clamp(0.0, 20.0) as u8
}

/// The laps a car has run in a race, the one it is on in part.
fn laps_run(state: &CarState, total_m: f32) -> f32 {
    (state.current_lap.max(1) - 1) as f32
        + (state.track_progress / total_m.max(1.0)).clamp(0.0, 1.0)
}

fn live_sky(session: &RaceSession, track: &TrackConfig) -> crate::conditions::LiveConditions {
    crate::conditions::LiveConditions::new(
        session.conditions,
        track.latitude_deg(),
        track.metadata.altitude_m.unwrap_or(0.0),
        session.conditions.sky_seed_or(session.id.as_u64_pair().0),
    )
}

/// The AI's speed plans per car: one for every this-many-th of its tank
/// (`GameSession::ai_speed_profile`). Each is a millisecond or so to build.
pub const AI_FUEL_PLAN_STEPS: usize = 10;
/// A damaged engine's plan is built for its power to this share
/// (`GameSession::plan_damaged_ai_speeds`): power lost in steps of 5%.
pub const AI_POWER_PLAN_STEP: f32 = 0.05;

/// Cars going out together are queued this far apart on the run-up.
pub const HOTLAP_SPACING_M: f32 = 30.0;
/// A qualifying car leaves this far past the line (or the pit exit), so its
/// first crossing of the line is a lap on.
pub const QUALIFYING_OUT_PAST_LINE_M: f32 = 30.0;
/// A run-up slot is taken while a car on the track is this close to it.
const HOTLAP_SLOT_CLEARANCE_M: f32 = 20.0;
/// How many slots back the queue reaches before cars double up.
const HOTLAP_SLOT_TRIES: usize = 16;

/// Skill of the AI that drives a watched hotlap: the top of the scale, on
/// `AiDriverProfile::exact_line`, so it shows the car at its best.
pub const HOTLAP_WATCH_SKILL: u8 = 110;
/// Seconds the car waits on the run-up before it sets off, so the watcher's
/// client has the track up (and the camera on the car) for the lap.
pub const HOTLAP_WATCH_HOLD_S: f32 = 6.0;
/// Share of the AI plan's speed the car is let go at when the hold ends, so
/// it is under the plan and the driver brings it up.
const HOTLAP_WATCH_ROLL_FRAC: f32 = 0.9;
/// A watched car that makes no headway for this long is put back on the
/// run-up (`GameSession::update_hotlap_watch`): the AI backs out of a wall
/// by itself, this is for the day it cannot.
pub const HOTLAP_WATCH_STUCK_S: f32 = 8.0;
/// Under this speed a watched car counts as standing still, m/s.
const HOTLAP_WATCH_STUCK_MPS: f32 = 1.5;

/// A watched hotlap (`SessionKind::HotlapWatch`): the session's one AI car
/// laps the track alone, every lap on fresh tyres and a fresh tank.
#[derive(Debug, Clone, Default)]
struct HotlapWatch {
    active: bool,
    /// The setup the car runs ([`GameSession::set_hotlap_watch_setup`]),
    /// as [`setup_key`]; 0 stock.
    setup_key: u64,
    /// The fastest legal lap watched of this circuit and car (an earlier
    /// watch's, or this one's), the ghost the stream carries, and a count
    /// that moves each time it is replaced.
    ghost: Option<GhostLap>,
    ghost_revision: u32,
    /// Tick until which the car is held on the run-up.
    hold_until_tick: u32,
    /// The car has been let go from the hold and put at speed
    /// ([`GameSession::roll_hotlap_watch_car`]).
    rolled: bool,
    /// Ticks the car has stood still (not held) so far.
    stuck_ticks: u32,
    /// The lap counter last tick: a change is the car crossing the line.
    last_lap: Option<u16>,
    /// How far the driver has been eased off since the lap began to go
    /// wrong ([`hotlap_watch_driver`]): 0 is the line itself at the top of
    /// the scale, which some circuits' rails and stands do not leave room
    /// for; each step after is a driver a little less exact.
    level: u8,
    /// Legal laps driven in a row at this level: [`HOTLAP_WATCH_CLIMB_LAPS`]
    /// of them take the driver a step back up.
    clean_laps: u8,
}

/// Clean laps in a row after which a watched hotlap's driver climbs a step
/// back up the ladder: a hiccup (a gust, a kerb taken badly) need not cost
/// the rest of the watch, and a remembered level is not a life sentence.
pub const HOTLAP_WATCH_CLIMB_LAPS: u8 = 3;

/// The sky a watched hotlap is driven under, folded into one number
/// (FNV-1a over the hour, the air and the wind): the ladder is kept per sky,
/// since a lap that held at noon may not in a crosswind at dusk.
fn sky_key(conditions: &SessionConditions) -> u64 {
    let c = conditions.clamp();
    let wind_from = c.wind_from_deg.unwrap_or(u16::MAX);
    [
        (c.time_of_day_minutes & 0xff) as u8,
        (c.time_of_day_minutes >> 8) as u8,
        c.air_temp_c.map_or(0x80, |t| t as u8),
        c.humidity_pct.unwrap_or(0xff),
        c.wind_kph.unwrap_or(0xff),
        (wind_from & 0xff) as u8,
        (wind_from >> 8) as u8,
    ]
    .iter()
    .fold(0xcbf2_9ce4_8422_2325u64, |hash, &byte| {
        (hash ^ byte as u64).wrapping_mul(0x0100_0000_01b3)
    })
}

/// A setup's clicks folded into one number (FNV-1a), so a watched hotlap's
/// ladder is kept per setup: a stock car's level says nothing of a softer
/// one's. 0 for the stock setup.
fn setup_key(setup: &CarSetup) -> u64 {
    if setup.is_stock() {
        return 0;
    }
    setup
        .clicks()
        .iter()
        .fold(0xcbf2_9ce4_8422_2325u64, |hash, &click| {
            (hash ^ click as u8 as u64).wrapping_mul(0x0100_0000_01b3)
        })
}

/// What a driver's racing line aid is planned for: their setup
/// ([`setup_key`]), the step of the tank, and the grip share in steps of
/// [`AID_GRIP_STEP`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct AidLineKey {
    setup: u64,
    fuel_step: usize,
    grip_steps: i32,
    /// The weekend day whose sky the line was planned under
    /// (`GameSession::sky_day`).
    day: u8,
}

impl AidLineKey {
    /// The line sent on joining: the stock car with a dry tank, on the
    /// grip the session was planned on, under the day's sky.
    const fn joined(day: u8) -> AidLineKey {
        AidLineKey {
            setup: 0,
            fuel_step: 0,
            grip_steps: (1.0 / AID_GRIP_STEP) as i32,
            day,
        }
    }
}

/// How far back down the lap an AI stuck on the road itself is put, m: a
/// run-up at the grade it could not climb from a standstill.
const AI_STALL_RUN_UP_M: f32 = 120.0;

/// Grip share a driver's tyres and road must move by before their racing
/// line aid is planned again: finer, and the heat of the tyres at the line
/// would send a new line most laps.
const AID_GRIP_STEP: f32 = 0.02;

/// The skill at the end of the ladder a watched hotlap's driver eases down.
const HOTLAP_WATCH_FLOOR_SKILL: u8 = 80;
/// Skill given up per step of the ladder.
const HOTLAP_WATCH_SKILL_STEP: u8 = 3;
/// Damage, percent in any zone, that makes a lap a crash.
const HOTLAP_WATCH_CRASH_PCT: f32 = 1.0;

/// The watched driver at `level` of the ladder: the exact line at the top
/// of the scale; then the same skill with the margin off the edge every
/// racing AI keeps; then that driver a few points slower each step.
fn hotlap_watch_driver(base: &AiDriverProfile, level: u8) -> AiDriverProfile {
    let skill = if level <= 1 {
        HOTLAP_WATCH_SKILL
    } else {
        HOTLAP_WATCH_SKILL
            .saturating_sub((level - 1).saturating_mul(HOTLAP_WATCH_SKILL_STEP))
            .max(HOTLAP_WATCH_FLOOR_SKILL)
    };
    let mut driver = AiDriverProfile::new(base.name.clone(), skill);
    driver.id = base.id;
    driver.preferred_car_id = base.preferred_car_id;
    driver.exact_line = level == 0;
    driver
}

/// Where a timed race's clock stands (`GameSession::race_clock`), as
/// `CompactTelemetry` carries it, positional: `[left_ms, final_lap]`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct RaceClock {
    /// Race time left, ms: the whole length until the green light, 0 once
    /// it has run out.
    pub left_ms: u32,
    /// The lap the race ends on: 0 while the clock runs, then the leader's
    /// lap when it ran out. The first car to complete it wins, and every
    /// car after it takes the flag at its next crossing of the line.
    pub final_lap: u16,
}

/// Where a race weekend stands (`GameSession::weekend_state`): what the
/// wire's `WeekendStateData` carries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WeekendState {
    /// 0 lobby (not started), 1 practice, 2 qualifying, 3 race.
    pub phase: u8,
    /// The qualifying round, 1-based; 0 outside qualifying.
    pub round: u8,
    /// How many qualifying rounds the weekend has.
    pub rounds: u8,
    /// The current phase has ended (the session is `Finished`).
    pub finished: bool,
    /// How many cars go through from this round; 0 in the final round and
    /// outside qualifying.
    pub advancing: u8,
    /// Every car knocked out so far, in grid order.
    pub eliminated: Vec<KnockedOut>,
    /// The phase's damage rule.
    pub damage: DamageLevel,
    /// The sky the current phase runs under, for a weekend whose practice
    /// or qualifying has its own; `None` for a weekend raced under one sky.
    pub conditions: Option<SessionConditions>,
}

/// A car knocked out of a weekend's qualifying ([`WeekendState`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct KnockedOut {
    /// The car's index, as the roster maps it.
    pub car_index: u8,
    /// The round it went out in, 1-based.
    pub round: u8,
    /// Its race grid slot, 1-based.
    pub grid_position: u8,
    /// Its best legal lap in that round, ms; 0 for none.
    pub lap_time_ms: u32,
    /// A late entry that ran no round (`weekend::Eliminated::late`).
    pub late: bool,
}

/// Where a qualifying shoot-out stands (`GameSession::shootout_state`):
/// what the wire's `ShootoutStateData` carries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShootoutState {
    /// The cars' indices (as the roster maps them) in run order.
    pub order: Vec<u8>,
    /// The 1-based place in `order` of the car on its run; 0 when none is.
    pub running: u8,
    /// How many cars of `order` have had their run.
    pub done: u8,
    /// The car on its run has started its flying lap.
    pub flying: bool,
}

/// A pit stop the crew started (`GameSession::take_pit_events`): the
/// wire's `PitServiceData` without the car's roster index. Each part's
/// seconds are 0 when the stop does not do it.
#[derive(Debug, Clone, PartialEq)]
pub struct PitService {
    /// The car's own box, 0-based from the lane's entry end.
    pub pit_box: u8,
    /// The tyre change, s, and the compound going on (an index into the
    /// car's `TireConfig::compounds()`).
    pub tyres_s: f32,
    pub compound: u8,
    /// Refuelling, s, and the litres going in.
    pub fuel_s: f32,
    pub fuel_l: f32,
    /// Repairs, s, and the damage repaired (percent, summed over the zones).
    pub repair_s: f32,
    pub repair_pct: f32,
    /// The whole stop, s.
    pub total_s: f32,
}

/// A driver's own pit box (`GameSession::take_pit_box_news`), in the
/// server's frame: metres, yaw in radians, +Y left. The wire's `PitBoxData`.
#[derive(Debug, Clone, PartialEq)]
pub struct PitBoxInfo {
    /// The box, 0-based from the lane's entry end.
    pub pit_box: u8,
    /// The stop spot, facing down the lane.
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    /// How far the spot is off the lane's middle, m, positive to the left.
    pub lateral_m: f32,
    pub lane_width_m: f32,
    /// The car's middle stops within this of the spot, m.
    pub radius_m: f32,
    /// The driver drives the pit lane themselves (the autopilot is off).
    pub manual: bool,
}

/// One corner of the lap, for a watcher's HUD
/// (`GameSession::set_track_corners`); the wire's `CornerData`.
#[derive(Debug, Clone, PartialEq)]
pub struct TrackCorner {
    /// From 1, in lap order from the start line.
    pub number: u8,
    /// The dossier's display name for it; empty when it has none.
    pub name: String,
    /// Stations in metres from the line, each in `[0, lap)`.
    pub entry_m: f32,
    pub apex_m: f32,
    pub exit_m: f32,
    /// The main turn goes left.
    pub left: bool,
}

/// One car of the session's roster (`GameSession::roster_rows`), in
/// `participants` order: the index compact telemetry identifies it by.
/// The wire's `RosterEntry`.
#[derive(Debug, Clone, PartialEq)]
pub struct RosterRow {
    pub car_index: u8,
    pub player_id: PlayerId,
    pub player_name: String,
    pub is_ai: bool,
    pub car_config_id: CarConfigId,
    /// The livery it wears, within the car's own list.
    pub livery: u8,
    /// The server drives the car until its driver rejoins (a held seat).
    pub away: bool,
    /// The car's starting place, 1 = pole; 0 when none is dealt.
    pub grid_slot: u8,
    /// On the grid: the driver said they are ready.
    pub ready: bool,
    /// Did not qualify (`park_non_qualifiers`).
    pub dnq: bool,
    /// Took the flag owing a stop under the pit rule.
    pub missed_stops: bool,
    /// The timed laps ended under a qualifying lap allowance.
    pub qualifying_laps: u8,
    /// Race control's penalties given to this car this race, in order.
    pub penalties: Vec<crate::race_control::Penalty>,
}

pub struct GameSession {
    pub session: RaceSession,
    pub track_config: TrackConfig,
    pub car_configs: HashMap<CarConfigId, CarConfig>,
    /// AI driver profiles indexed by their player ID
    pub ai_profiles: std::collections::BTreeMap<PlayerId, AiDriverProfile>,
    /// Speed profiles along the line per car the AI drives, one per
    /// [`AI_FUEL_PLAN_STEPS`]th of its tank from empty to full, built when
    /// the AI is seated: a car drives the plan for the next step up from
    /// what it carries ([`Self::ai_speed_profile`]), so it gets quicker as
    /// the tank drains and is never planned lighter than it is. Looked up by
    /// key only, never iterated.
    ai_speed_profiles: HashMap<CarConfigId, Vec<RacingLineProfile>>,
    /// The same for a car whose engine damage has cost it power, by car,
    /// [`AI_POWER_PLAN_STEP`]s of power lost and step of the tank: built as
    /// the damage comes (`plan_damaged_ai_speeds`), one plan at a time.
    ai_power_profiles: std::collections::BTreeMap<(CarConfigId, u8, usize), RacingLineProfile>,
    /// What each human's racing line aid was last planned for
    /// ([`Self::refreshed_racing_line`]); a driver missing here has the
    /// line sent on joining (stock car, dry tank, the planned grip).
    /// Looked up by key only, never iterated.
    aid_lines: HashMap<PlayerId, AidLineKey>,
    /// The race's flags (`crate::race_control`), from the tick.
    pub race_control: crate::race_control::RaceControl,
    /// How the race gets away (`crate::race_start`): its formation lap,
    /// and the cars that jumped the start. Made afresh as the field lines
    /// up.
    pub race_start: crate::race_start::RaceStart,
    /// Every penalty given this race (`crate::race_control::Penalty`), by
    /// car, in the order given. Emptied as the field lines up.
    pub penalties: std::collections::BTreeMap<PlayerId, Vec<crate::race_control::Penalty>>,
    /// Each pass under a yellow not yet judged, by pair (passer, passed),
    /// the tick it was made: penalised if the passer is still ahead
    /// [`crate::race_control::YELLOW_PASS_HANDBACK_S`] later, forgiven if
    /// it handed the place back before.
    yellow_passed: std::collections::BTreeMap<(PlayerId, PlayerId), u32>,
    /// How many times a weekend day has baked its own sky
    /// ([`Self::begin_day`]): a racing line planned under an earlier one is
    /// planned again.
    sky_day: u8,
    /// A day's own sky has just been baked: every human's racing line is
    /// planned again now, not at their next lap's end
    /// ([`Self::take_day_lines_due`]).
    day_lines_due: bool,
    /// The full-throttle straights of each car's plan (`hybrid::straights`,
    /// of its full-tank plan: every plan of a car shares its points), where
    /// a hybrid AI saves its overtake button for. Looked up by key only.
    ai_straights: HashMap<CarConfigId, crate::hybrid::Straights>,
    /// Simulation tick rate (Hz); the fixed timestep is `1 / tick_rate_hz`.
    tick_rate_hz: u16,
    /// The names of the humans seated, set as they join (the AI's are on
    /// their profiles): what the start order's name references match.
    driver_names: std::collections::BTreeMap<PlayerId, String>,
    /// Each driver's best legal qualifying lap, ms, and the tick it was set
    /// on (the earlier of two equal times starts ahead).
    qualifying: std::collections::BTreeMap<PlayerId, (u32, u32)>,
    /// The tick this session last entered `GameMode::Qualification` or
    /// `GameMode::Practice`: a timed session's clock runs from it
    /// (`timed_elapsed_ticks`).
    pub timed_start_tick: Option<u32>,
    /// A race weekend's progress (`crate::weekend`); unused outside one.
    weekend: crate::weekend::WeekendProgress,
    /// The `WeekendState` last handed out (`take_weekend_state`), so it
    /// goes out again only when a field changes.
    weekend_sent: Option<WeekendState>,
    /// A weekend phase began between ticks (`advance_weekend`): the game
    /// loop starts its replay (`take_phase_started`).
    phase_started: bool,
    /// The race day's damage rule, kept once a weekend's practice or
    /// qualifying has replaced `RaceSession::damage` with its own
    /// (`begin_phase_damage`).
    race_day_damage: Option<DamageLevel>,
    /// The one-lap shoot-out running in this qualifying session or round
    /// ([`Shootout`]); `None` outside one.
    shootout: Option<Shootout>,
    /// The `ShootoutState` last handed out (`take_shootout_state`).
    shootout_sent: Option<ShootoutState>,
    /// The flying laps each car has ended in this qualifying session or
    /// round, against the lap allowance
    /// (`RaceSession::qualifying_lap_allowance`).
    qualifying_laps_run: std::collections::BTreeMap<PlayerId, u8>,
    /// Set when session membership changed since the last roster broadcast;
    /// starts true so the roster goes out once when the session first ticks.
    roster_dirty: bool,
    /// The state the lobby was last told (`take_state_change`), so the
    /// session browser lists what the session is doing.
    listed_state: SessionState,
    /// Tick at which the race ends whether or not every car has finished,
    /// set when the winner crosses the line.
    finish_deadline_tick: Option<u32>,
    /// The server loop's tick on which this session was first seen
    /// finished: `game_loop::tick::cleanup_finished_sessions` times its
    /// removal from there (the session's own `current_tick` counts from its
    /// creation, not the server's start).
    pub finished_since_loop_tick: Option<u64>,
    /// The server loop's tick from which every human seated here has been
    /// away (`away`): `game_loop::tick` ends the session when none is back
    /// within `[server] reconnect_grace_seconds`.
    pub orphaned_since_loop_tick: Option<u64>,
    /// Run by the server from the dashboard (`crate::admin::hosted`), not
    /// by a player: it stays without a human in it, and the dashboard or
    /// its schedule starts and ends it. Nothing in the tick reads it.
    pub hosted: bool,
    /// Cars the AI field is not dealt (switched off on the dashboard);
    /// empty, the field is the host class's every car.
    pub ai_excluded_cars: std::collections::BTreeSet<CarConfigId>,
    /// Humans whose connection was lost: their seat is held for them to
    /// come back to (`hold_seat` / `resume_seat`) and the server drives
    /// their car meanwhile (`away_input`), or parks it in its garage in a
    /// hotlap or qualifying session.
    away: std::collections::BTreeSet<PlayerId>,
    /// The cars the qualifying cut-off left out of the race when it lined
    /// up (`park_non_qualifiers`): marked `RosterEntry::dnq`.
    not_qualified: std::collections::BTreeSet<PlayerId>,
    /// A finished human's steering aid, held while the server drives their
    /// car on the cool-down lap (the AI steers the rack directly) and given
    /// back when the grid is lined up again. Looked up by key only.
    held_steering_assist: HashMap<PlayerId, bool>,
    /// The car a driver with a garage setup is simulated with: their
    /// `CarSetup` baked into a copy of the shared config, remade on every
    /// `SetCarSetup` and dropped when the setup is stock or the player
    /// leaves. Looked up by key only, never iterated.
    tuned_configs: HashMap<PlayerId, CarConfig>,
    /// What each driver asked for, clamped.
    car_setups: HashMap<PlayerId, CarSetup>,
    /// Each driver's orders for their next stop (`SetPitPlan`), clamped;
    /// none for a driver who never sent one. Looked up by key only.
    pit_plans: HashMap<PlayerId, crate::pit::PitPlan>,
    /// Litres a lap of this track costs each car (`racing_line::lap_fuel_liters`),
    /// worked out the first time the car is fuelled. Looked up by key only.
    lap_fuel: HashMap<CarConfigId, f32>,
    /// Each car's ideal lap here, s (`racing_line::lap_time_s`), worked out
    /// with its lap fuel: how many laps a timed race is. Key lookups only.
    lap_seconds: HashMap<CarConfigId, f32>,
    /// How fast the racing line is drying (`LineDrying`), for the AI's
    /// crossover (`pit::CrossoverContext::drying_per_s`).
    line_drying: LineDrying,
    /// How the race's end stands: its final lap, who has the flag.
    race_end: RaceEnd,
    /// The livery each human driver picked (`SelectCar`); AI drivers are
    /// dealt one in `build_roster`. Looked up by key only.
    liveries: HashMap<PlayerId, u8>,
    /// The humans who said they are ready on the grid (`GameMode::Grid`),
    /// cleared each time the grid goes up and when the countdown begins.
    ready: std::collections::BTreeSet<PlayerId>,
    /// The countdown the grid hands over to once every driver is ready
    /// (`OpenGrid.countdown_seconds`), s.
    grid_countdown_seconds: u16,
    /// Timing lines crossed since the game loop last drained them.
    lap_events: Vec<SessionLapEvent>,
    /// Pit stops started since the clients were last told.
    pit_events: Vec<(PlayerId, PitService)>,
    /// The lap each car was last topped up at the line on, on a circuit
    /// with no pit lane (`update_line_refuel`).
    line_refuel_laps: std::collections::BTreeMap<PlayerId, u16>,
    /// The humans told where their pit box is (`take_pit_box_news`), and
    /// whether they drive the lane themselves as they were told it.
    pit_box_told: std::collections::BTreeMap<PlayerId, bool>,
    /// The lap each car is driving, sampled for a ghost. Human drivers only:
    /// a record is a driver's own, and the AI never sets one.
    lap_traces: HashMap<PlayerId, Vec<GhostSample>>,
    /// The fastest legal lap anyone has set in this session, and the fastest
    /// each sector has been driven — the purple times on a timing screen.
    session_best_lap_ms: Option<u32>,
    session_best_splits_ms: [Option<u32>; SECTOR_COUNT],
    /// Debris on the road ([`Debris`], `update_debris`), newest last.
    pub debris: Vec<Debris>,
    /// The sky as it is now: the clock, the forecast's weather, the air
    /// and the asphalt (`crate::conditions`, `update_sky`).
    pub sky: crate::conditions::LiveConditions,
    /// The tick the sky started on: 0, or the start of a race weekend's
    /// day with a sky of its own (`begin_day`). The clock and the forecast
    /// run from it.
    pub sky_epoch_tick: u32,
    /// The track as loaded, before any weather was baked into it (the
    /// server's shared copy), so a weekend's day can bake its own sky
    /// (`begin_day`); `None` for a session made without one.
    source_track: Option<std::sync::Arc<TrackConfig>>,
    /// The session's own conditions as created, the race day's sky, kept
    /// once a weekend's day has replaced `RaceSession::conditions`.
    race_day_conditions: Option<SessionConditions>,
    /// Debug-only hooks (`crate::debug_hooks`), off unless the server's
    /// `[debug]` table set them for the sessions it creates.
    debug: crate::debug_hooks::DebugHooks,
    /// Ticks of green so far, and the next debug event to fire.
    debug_green_ticks: u64,
    debug_next_event: usize,
    /// Green tick until which a stand-in holds the overtake button.
    debug_boost_until: std::collections::BTreeMap<PlayerId, u64>,
    /// The watched hotlap this session is, if it is one.
    hotlap_watch: HotlapWatch,
    /// The track's corners, for the watcher's HUD (`TrackCorners`).
    track_corners: Vec<TrackCorner>,
    /// Each driver's own-car feed for a predicting client
    /// (`crate::own_car`): the input sequence applied, the state blob being
    /// sent. Written by the game loop around the tick; never read by the
    /// simulation.
    pub own_car_feeds: std::collections::BTreeMap<PlayerId, crate::own_car::OwnCarFeed>,
}

/// A one-lap shoot-out (`QualifyingFormat::Shootout`): the cars run one at a
/// time in `order`, each sent out from the pit exit for an outlap and one
/// flying lap while the rest wait in their garages.
#[derive(Debug, Default, Clone)]
struct Shootout {
    /// Every car taking part, first out first.
    order: Vec<PlayerId>,
    /// How many of `order` have been called: run, running, or passed over
    /// (gone, or away when called).
    called: usize,
    /// The car on its run and the tick by which its run ends whatever it
    /// has done ([`SHOOTOUT_RUN_LAPS`]).
    runner: Option<(PlayerId, u32)>,
}

/// How a race's end stands, reset with every start.
#[derive(Debug, Default, Clone)]
struct RaceEnd {
    /// A timed race's last lap: the leader's lap when the clock ran out.
    final_lap: Option<u16>,
    /// Every car that has taken the flag, and how it took it.
    finishers: std::collections::BTreeMap<PlayerId, Finisher>,
    /// Once the winner is in, the lap each car still running was on: it
    /// takes the flag the next time it crosses the line, a lap down or not.
    /// Empty in a race over laps, where every car runs the distance.
    flag_laps: std::collections::BTreeMap<PlayerId, u16>,
}

/// The racing line's water as the race goes (`RoadState::mean_line_water`),
/// and how fast it is drying: the change a second, eased over
/// [`LINE_DRYING_EASE_S`] (positive drying, negative getting wetter).
#[derive(Debug, Clone, Copy, Default)]
struct LineDrying {
    last: Option<(u32, f32)>,
    per_s: f32,
}

/// The line's drying rate is eased over this long, s: a tick's change is
/// noise, a minute's is the weather.
const LINE_DRYING_EASE_S: f32 = 30.0;

impl LineDrying {
    /// Take the line's `water` at `tick`, at `rate_hz` ticks a second.
    fn sample(&mut self, tick: u32, water: f32, rate_hz: u32) {
        if let Some((last_tick, last_water)) = self.last {
            if tick > last_tick {
                let dt = (tick - last_tick) as f32 / rate_hz.max(1) as f32;
                let now = (last_water - water) / dt;
                self.per_s += (now - self.per_s) * (dt / LINE_DRYING_EASE_S).min(1.0);
            }
        }
        self.last = Some((tick, water));
    }
}

/// A car taking the flag.
#[derive(Debug, Clone, Copy)]
struct Finisher {
    /// Laps completed.
    laps: u16,
    tick: u32,
    /// How far past the line it was on that tick, m: who crossed first.
    progress: f32,
    /// It took the flag owing a stop or its second compound under the
    /// session's pit rule (`data::PitRule`): classified behind every
    /// finisher that met it.
    missed_stops: bool,
    /// Its penalties' seconds (`race_control::Penalty::added_s`, a
    /// drive-through or stop-go still owed at the flag added as time), in
    /// ticks: its race time is this much longer.
    penalty_ticks: u32,
}

/// Put a car down still at a pose: everything that moves (velocities,
/// spin, suspension, the body's lean, contact) as a fresh car on a grid
/// slot has it; fuel, tyres, damage, aids and timing stay as they are.
fn place_still(state: &mut CarState, x: f32, y: f32, z: f32, yaw_rad: f32) {
    let slot = GridSlot {
        position: state.grid_position,
        x,
        y,
        z,
        yaw_rad,
    };
    let fresh = CarState::new(state.player_id, state.car_config_id, &slot);
    state.pos_x = fresh.pos_x;
    state.pos_y = fresh.pos_y;
    state.pos_z = fresh.pos_z;
    state.yaw_rad = fresh.yaw_rad;
    state.pitch_rad = fresh.pitch_rad;
    state.roll_rad = fresh.roll_rad;
    state.vel_x = 0.0;
    state.vel_y = 0.0;
    state.vel_z = 0.0;
    state.speed_mps = 0.0;
    state.angular_vel_yaw = 0.0;
    state.angular_vel_pitch = 0.0;
    state.angular_vel_roll = 0.0;
    state.throttle_input = 0.0;
    state.brake_input = 0.0;
    state.steering_input = 0.0;
    state.wheel_angular_vel = [0.0; 4];
    state.suspension = fresh.suspension;
    state.g_forces = fresh.g_forces;
    state.is_colliding = false;
    state.collision_normal_x = 0.0;
    state.collision_normal_y = 0.0;
    state.collision_normal_z = 0.0;
    state.is_airborne = fresh.is_airborne;
    state.strike_mps = fresh.strike_mps;
    state.porpoise_phase = fresh.porpoise_phase;
    state.porpoise_amp = fresh.porpoise_amp;
    state.auto_shift_hold_ticks = 0;
    state.auto_reverse_ticks = 0;
    state.wheels_off_track = false;
    state.laps.off_track_ticks = 0;
}

/// A timing line crossed by one car, with what it meant for the session.
#[derive(Debug, Clone)]
pub struct SessionLapEvent {
    pub player_id: PlayerId,
    pub event: LapEvent,
    /// The fastest anyone has gone in this session.
    pub session_best_lap: bool,
    pub session_best_sector: bool,
    /// The lap's trace, taken when a human driver (or a watched hotlap's
    /// car) set a personal best on a legal lap. The store decides whether
    /// it beats a human's record.
    pub ghost: Option<GhostLap>,
}

/// Longest ghost trace held for one car: ten minutes at [`GHOST_SAMPLE_HZ`]
/// (a Nordschleife lap in a road car).
/// A car parked on the road must not grow a buffer without end.
const MAX_GHOST_SAMPLES: usize = (GHOST_SAMPLE_HZ as usize) * 600;

/// Take one car's timing-line crossing: update the session bests, take the
/// ghost trace when the lap earned one, and queue the event for the loop.
///
/// A free function, not a method: the tick loops hold `participants`
/// mutably, and this touches only the disjoint fields beside it.
#[allow(clippy::too_many_arguments)]
fn note_lap_event(
    out: &mut Vec<SessionLapEvent>,
    traces: &mut HashMap<PlayerId, Vec<GhostSample>>,
    best_lap_ms: &mut Option<u32>,
    best_splits_ms: &mut [Option<u32>; SECTOR_COUNT],
    player_id: PlayerId,
    is_ai: bool,
    event: LapEvent,
) {
    let sector = (event.sector as usize).min(SECTOR_COUNT - 1);
    let session_best_sector =
        event.valid && best_splits_ms[sector].is_none_or(|best| event.sector_time_ms < best);
    if session_best_sector {
        best_splits_ms[sector] = Some(event.sector_time_ms);
    }

    let mut session_best_lap = false;
    let mut ghost = None;
    if let Some(lap_time_ms) = event.lap_time_ms {
        session_best_lap = event.valid && best_lap_ms.is_none_or(|best| lap_time_ms < best);
        if session_best_lap {
            *best_lap_ms = Some(lap_time_ms);
        }
        // The lap is over either way: the trace of it goes with the event or
        // is thrown away, so the next lap records from empty.
        let samples = traces.remove(&player_id).unwrap_or_default();
        if !is_ai && event.valid && event.personal_best_lap && !samples.is_empty() {
            ghost = Some(GhostLap {
                lap_time_ms,
                sample_hz: GHOST_SAMPLE_HZ,
                samples,
            });
        }
    }

    out.push(SessionLapEvent {
        player_id,
        event,
        session_best_lap,
        session_best_sector,
        ghost,
    });
}

/// Append this tick's pose to a human driver's ghost trace (or a watched
/// hotlap's car's: `is_ai` false for it), at [`GHOST_SAMPLE_HZ`]. Called
/// before the progress update, so the sample's time is measured against the
/// lap the car is still on.
fn sample_ghost_trace(
    traces: &mut HashMap<PlayerId, Vec<GhostSample>>,
    state: &CarState,
    is_ai: bool,
    current_tick: u32,
    tick_rate_hz: u16,
) {
    if is_ai || state.current_lap == 0 {
        return;
    }
    let divisor = ((tick_rate_hz as f32 / GHOST_SAMPLE_HZ).round() as u32).max(1);
    if !current_tick.is_multiple_of(divisor) {
        return;
    }
    let trace = traces.entry(state.player_id).or_default();
    if trace.len() >= MAX_GHOST_SAMPLES {
        return;
    }
    trace.push(GhostSample {
        t_ms: crate::laps::ticks_to_ms(
            current_tick.saturating_sub(state.lap_start_tick),
            tick_rate_hz,
        ),
        x: state.pos_x,
        y: state.pos_y,
        z: state.pos_z,
        yaw_rad: state.yaw_rad,
        pitch_rad: state.pitch_rad,
        roll_rad: state.roll_rad,
        speed_mps: state.speed_mps,
        steering: state.steering_input,
        throttle: state.throttle_input,
        brake: state.brake_input,
        gear: state.gear,
        engine_rpm: state.engine_rpm,
    });
}

/// The config a car is simulated with: the driver's tuned copy when they
/// have one, else the shared config for the car. A free function so the
/// tick loops can hold `participants` mutably alongside it.
/// The centerline point nearest a car on the pit lane, looked for within
/// [`LANE_LAP_WINDOW_M`] of the lap station the lane's progress implies
/// (its entry and exit stations, in proportion along it).
fn anchor_on_lap(
    lane: &crate::pit::PitLane,
    centerline: &[crate::data::TrackPoint],
    total: f32,
    lane_station_m: f32,
    state: &CarState,
) -> Option<u32> {
    if centerline.is_empty() || total <= 0.0 || lane.length_m <= 0.0 {
        return None;
    }
    let span = (lane.exit_station_m - lane.entry_station_m).rem_euclid(total);
    let expect = lane.entry_station_m + span * (lane_station_m / lane.length_m).clamp(0.0, 1.0);
    let gap = |d: f32| {
        let g = (d - expect).rem_euclid(total);
        g.min(total - g)
    };
    centerline
        .iter()
        .enumerate()
        .filter(|(_, p)| gap(p.distance_from_start_m) <= LANE_LAP_WINDOW_M)
        .min_by(|a, b| {
            let da = (a.1.x - state.pos_x).powi(2) + (a.1.y - state.pos_y).powi(2);
            let db = (b.1.x - state.pos_x).powi(2) + (b.1.y - state.pos_y).powi(2);
            da.total_cmp(&db)
        })
        .map(|(i, _)| i as u32)
}

/// How far either way along the lap from where the pit lane's progress
/// puts a car its place on the lap is looked for, m.
const LANE_LAP_WINDOW_M: f32 = 150.0;

fn simulated_config<'a>(
    car_configs: &'a HashMap<CarConfigId, CarConfig>,
    tuned_configs: &'a HashMap<PlayerId, CarConfig>,
    state: &CarState,
) -> Option<&'a CarConfig> {
    tuned_configs
        .get(&state.player_id)
        .or_else(|| car_configs.get(&state.car_config_id))
}

/// What a set of tyres is at when a car is sent out on it.
#[derive(Clone, Copy)]
enum TyreStart {
    /// Out of the garage: its blankets, or the air.
    Garage,
    /// On a race grid: part of the way from there to the optimum, the
    /// warmth a formation lap would put in (`tyre_thermal::FORMATION_LAP_WARMTH`).
    Grid,
    /// At the optimum: a hotlap measures the car, not its warm-up.
    Warm,
}

/// Fit a car's tyres for a run (`tyre_thermal::fit`), with the car's
/// setup's pressures.
fn fit_tyres(
    state: &mut CarState,
    car_configs: &HashMap<CarConfigId, CarConfig>,
    tuned_configs: &HashMap<PlayerId, CarConfig>,
    track: &TrackConfig,
    start: TyreStart,
    car_setups: &HashMap<PlayerId, CarSetup>,
) {
    let Some(config) = simulated_config(car_configs, tuned_configs, state) else {
        return;
    };
    let tyre = &config.tire_config;
    // The compound the driver's setup chose for the next set (the weather's
    // tyre for a stock pick in the rain or on snow, and for the AI), from
    // the car's own list.
    let compound = car_setups
        .get(&state.player_id)
        .copied()
        .unwrap_or_default()
        .compound_index_for(
            tyre,
            track.track_surface.water,
            crate::road_state::RoadState::line_snow_of(track),
        );
    let surface = &track.track_surface;
    let temperature = match start {
        TyreStart::Garage => tyre_thermal::start_temperature_c(tyre, surface),
        TyreStart::Grid => tyre_thermal::grid_temperature_c(tyre, surface),
        // (A hotlap goes out at the chosen compound's own optimum: a soft
        // works 6 °C cooler than the medium.)
        TyreStart::Warm => tyre_thermal::optimum_c(tyre, tyre.compound(compound)),
    };
    tyre_thermal::fit(state, tyre, temperature, compound, surface.water);
    let brakes = crate::brakes::start_temperature_c(
        config.brake_material,
        track.track_surface.air_temperature_c,
        matches!(start, TyreStart::Grid),
        matches!(start, TyreStart::Warm),
    );
    crate::brakes::fit(state, brakes);
    crate::hybrid::charge_full(state, config);
    // The hybrid's mode at the start of the run: the driver's setup's (the
    // AI's and a stock setup's is Balanced).
    state.ers_mode = car_setups
        .get(&state.player_id)
        .copied()
        .unwrap_or_default()
        .start_mode();
}

impl GameSession {
    pub fn new(
        session: RaceSession,
        track_config: TrackConfig,
        car_configs: HashMap<CarConfigId, CarConfig>,
    ) -> Self {
        let track_config = with_road(track_config, &session.conditions);
        let sky = live_sky(&session, &track_config);
        Self {
            session,
            track_config,
            car_configs,
            ai_profiles: std::collections::BTreeMap::new(),
            tick_rate_hz: DEFAULT_TICK_RATE_HZ,
            driver_names: std::collections::BTreeMap::new(),
            qualifying: std::collections::BTreeMap::new(),
            timed_start_tick: None,
            weekend: Default::default(),
            weekend_sent: None,
            phase_started: false,
            race_day_damage: None,
            shootout: None,
            shootout_sent: None,
            qualifying_laps_run: Default::default(),
            roster_dirty: true,
            listed_state: SessionState::Lobby,
            ai_speed_profiles: HashMap::new(),
            ai_straights: HashMap::new(),
            ai_power_profiles: std::collections::BTreeMap::new(),
            aid_lines: HashMap::new(),
            race_control: Default::default(),
            race_start: Default::default(),
            penalties: Default::default(),
            yellow_passed: Default::default(),
            sky_day: 0,
            day_lines_due: false,
            finish_deadline_tick: None,
            finished_since_loop_tick: None,
            orphaned_since_loop_tick: None,
            hosted: false,
            ai_excluded_cars: std::collections::BTreeSet::new(),
            away: std::collections::BTreeSet::new(),
            not_qualified: std::collections::BTreeSet::new(),
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            ready: std::collections::BTreeSet::new(),
            grid_countdown_seconds: START_SESSION_COUNTDOWN_SECONDS,
            car_setups: HashMap::new(),
            pit_plans: HashMap::new(),
            lap_fuel: HashMap::new(),
            lap_seconds: HashMap::new(),
            line_drying: LineDrying::default(),
            race_end: RaceEnd::default(),
            lap_events: Vec::new(),
            pit_events: Vec::new(),
            line_refuel_laps: std::collections::BTreeMap::new(),
            pit_box_told: std::collections::BTreeMap::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
            debris: Vec::new(),
            sky,
            sky_epoch_tick: 0,
            source_track: None,
            race_day_conditions: None,
            debug: crate::debug_hooks::DebugHooks::default(),
            debug_green_ticks: 0,
            debug_next_event: 0,
            debug_boost_until: std::collections::BTreeMap::new(),
            hotlap_watch: HotlapWatch::default(),
            track_corners: Vec::new(),
            own_car_feeds: std::collections::BTreeMap::new(),
        }
    }

    /// Create a new game session with AI driver profiles.
    ///
    /// # Arguments
    /// * `session` - The race session configuration
    /// * `track_config` - Track configuration
    /// * `car_configs` - Available car configurations
    /// * `ai_profiles` - AI driver profiles to use for this session
    pub fn with_ai_profiles(
        session: RaceSession,
        track_config: TrackConfig,
        car_configs: HashMap<CarConfigId, CarConfig>,
        ai_profiles: Vec<AiDriverProfile>,
    ) -> Self {
        let ai_profiles_map: std::collections::BTreeMap<PlayerId, AiDriverProfile> =
            ai_profiles.into_iter().map(|p| (p.id, p)).collect();
        let track_config = with_road(track_config, &session.conditions);
        let sky = live_sky(&session, &track_config);

        Self {
            session,
            track_config,
            car_configs,
            ai_profiles: ai_profiles_map,
            tick_rate_hz: DEFAULT_TICK_RATE_HZ,
            driver_names: std::collections::BTreeMap::new(),
            qualifying: std::collections::BTreeMap::new(),
            timed_start_tick: None,
            weekend: Default::default(),
            weekend_sent: None,
            phase_started: false,
            race_day_damage: None,
            shootout: None,
            shootout_sent: None,
            qualifying_laps_run: Default::default(),
            roster_dirty: true,
            listed_state: SessionState::Lobby,
            ai_speed_profiles: HashMap::new(),
            ai_straights: HashMap::new(),
            ai_power_profiles: std::collections::BTreeMap::new(),
            aid_lines: HashMap::new(),
            race_control: Default::default(),
            race_start: Default::default(),
            penalties: Default::default(),
            yellow_passed: Default::default(),
            sky_day: 0,
            day_lines_due: false,
            finish_deadline_tick: None,
            finished_since_loop_tick: None,
            orphaned_since_loop_tick: None,
            hosted: false,
            ai_excluded_cars: std::collections::BTreeSet::new(),
            away: std::collections::BTreeSet::new(),
            not_qualified: std::collections::BTreeSet::new(),
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            ready: std::collections::BTreeSet::new(),
            grid_countdown_seconds: START_SESSION_COUNTDOWN_SECONDS,
            car_setups: HashMap::new(),
            pit_plans: HashMap::new(),
            lap_fuel: HashMap::new(),
            lap_seconds: HashMap::new(),
            line_drying: LineDrying::default(),
            race_end: RaceEnd::default(),
            lap_events: Vec::new(),
            pit_events: Vec::new(),
            line_refuel_laps: std::collections::BTreeMap::new(),
            pit_box_told: std::collections::BTreeMap::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
            debris: Vec::new(),
            sky,
            sky_epoch_tick: 0,
            source_track: None,
            race_day_conditions: None,
            debug: crate::debug_hooks::DebugHooks::default(),
            debug_green_ticks: 0,
            debug_next_event: 0,
            debug_boost_until: std::collections::BTreeMap::new(),
            hotlap_watch: HotlapWatch::default(),
            track_corners: Vec::new(),
            own_car_feeds: std::collections::BTreeMap::new(),
        }
    }

    /// The track as loaded, before the weather was baked in: what a race
    /// weekend's day with its own sky bakes again (`begin_day`).
    pub fn set_source_track(&mut self, track: std::sync::Arc<TrackConfig>) {
        self.source_track = Some(track);
    }

    /// A race weekend's day begins (practice, qualifying, the race) under
    /// its own sky, when the format names one for any day
    /// (`WeekendFormat::practice_conditions` / `qualifying_conditions`; the
    /// race day is the session's own `conditions`). The day's conditions
    /// are resolved (the day's own seed when the host gave none), baked
    /// into a fresh copy of the source track with a fresh road (the day's
    /// rubber, its water), and the sky starts over from this tick. The AI's
    /// speed plans and lap figures are worked out again. A weekend that
    /// names no day's sky keeps the one sky from its creation: nothing
    /// changes.
    fn begin_day(&mut self, phase: crate::weekend::WeekendPhase) {
        use crate::weekend::WeekendPhase;
        let Some(format) = self.session.weekend.as_ref() else {
            return;
        };
        if !format.has_day_conditions() {
            return;
        }
        let Some(source) = self.source_track.clone() else {
            return;
        };
        let picked = match phase {
            WeekendPhase::Practice => format.practice_conditions.as_deref().copied(),
            WeekendPhase::Qualifying => format.qualifying_conditions.as_deref().copied(),
            _ => None,
        };
        let race_day = *self
            .race_day_conditions
            .get_or_insert(self.session.conditions);
        let conditions = match picked {
            Some(day) => {
                // Each day its own forecast and auto air, from the
                // session's id and the day.
                let seed = self.session.id.as_u64_pair().0
                    ^ (phase as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
                day.at_site(&source)
                    .resolve_at(day.sky_seed_or(seed), source.latitude_deg())
            }
            None => race_day,
        };
        let mut track = (*source).clone();
        track.road_state = None;
        conditions.apply_to_track(&mut track);
        self.track_config = with_road(track, &conditions);
        self.session.conditions = conditions;
        self.sky = live_sky(&self.session, &self.track_config);
        self.sky_epoch_tick = self.session.current_tick;
        self.sky_day = self.sky_day.wrapping_add(1);
        self.day_lines_due = true;
        self.ai_speed_profiles.clear();
        self.ai_straights.clear();
        self.lap_fuel.clear();
        self.lap_seconds.clear();
        self.debris.clear();
    }

    /// A race weekend's phase begins under its own damage rule, when the
    /// format names one for practice or qualifying
    /// (`WeekendFormat::practice_damage` / `qualifying_damage`): every car
    /// and every later arrival takes it, and `RaceSession::damage` is it
    /// while the phase runs. The race, and a phase that names none, has the
    /// session's own. A format naming none changes nothing.
    fn begin_phase_damage(&mut self, phase: crate::weekend::WeekendPhase) {
        let Some(format) = self.session.weekend.as_ref() else {
            return;
        };
        if !format.has_phase_damage() {
            return;
        }
        let picked = format.phase_damage(phase);
        let race_day = *self.race_day_damage.get_or_insert(self.session.damage);
        self.set_damage(picked.unwrap_or(race_day));
    }

    /// Seconds of the sky's own clock (from `sky_epoch_tick`).
    pub fn sky_seconds(&self) -> f32 {
        self.session
            .current_tick
            .saturating_sub(self.sky_epoch_tick) as f32
            / self.tick_rate_hz.max(1) as f32
    }

    /// Set the simulation tick rate (Hz). Called with the configured server
    /// tick rate when the session is created by the server.
    pub fn set_tick_rate(&mut self, tick_rate_hz: u16) {
        self.tick_rate_hz = tick_rate_hz;
    }

    /// The simulation tick rate (Hz) this session runs at.
    pub fn tick_rate_hz(&self) -> u16 {
        self.tick_rate_hz
    }

    /// Fixed physics timestep in seconds, derived from the tick rate.
    fn dt(&self) -> f32 {
        1.0 / self.tick_rate_hz as f32
    }

    /// Advance the session by one tick
    pub fn tick(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.session.current_tick += 1;
        self.update_sky();
        self.hand_cars_to_stand_in();
        // A watched car waits on the run-up with its foot on the brake.
        let held;
        let inputs = if self.hotlap_watch.active
            && self.session.current_tick < self.hotlap_watch.hold_until_tick
        {
            held = self.hotlap_watch_hold(inputs);
            &held
        } else {
            inputs
        };
        crate::headlights::update(
            &mut self.session.participants,
            inputs,
            self.sky.headlights_needed(),
        );

        // Handle game mode specific logic
        match self.session.game_mode {
            GameMode::Lobby => {
                self.tick_lobby();
            }
            GameMode::Sandbox => {
                self.tick_sandbox();
            }
            GameMode::Countdown => {
                self.tick_countdown(inputs);
            }
            GameMode::Grid => {
                self.tick_grid(inputs);
            }
            GameMode::DemoLap => {
                self.tick_demolap(inputs);
            }
            GameMode::FreePractice => {
                self.tick_free_practice(inputs);
            }
            // Kept on the wire for old clients; replays are spectator
            // streams a client plays itself, so a session never runs one.
            GameMode::Replay => {}
            GameMode::Race => {
                self.tick_racing(inputs);
            }
            GameMode::Qualification | GameMode::Hotlap | GameMode::Practice => {
                // Qualifying and timed practice run like a hotlap: every
                // human starts in the garage, frozen and out of the
                // collision passes, until they go out; the AI drive on.
                self.tick_hotlap(inputs);
            }
        }
        if self.hotlap_watch.active {
            self.update_hotlap_watch();
        }
    }

    /// Lobby mode: Players selecting cars, no telemetry sent
    fn tick_lobby(&mut self) {
        // In lobby mode, no simulation occurs and no telemetry is sent
        // Players are selecting cars and waiting for session to start
    }

    /// Sandbox mode: No movement, no telemetry recording, camera exploration only
    fn tick_sandbox(&mut self) {
        // In sandbox mode, nothing moves and no physics updates occur
        // Players can freely move camera around the track
        // No telemetry is recorded or sent
    }

    /// Countdown mode: Players frozen in pit lane, countdown timer running
    fn tick_countdown(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        // A formation lap runs in the countdown's state, with no clock.
        if self.race_start.formation.is_some() {
            self.tick_formation(inputs);
            return;
        }
        // The pit lane is shut while the field lines up: the exit light
        // shows red until the start (`crate::pit::exit_closed`).
        self.update_pits();
        if let Some(ref mut countdown) = self.session.countdown_ticks_remaining {
            if *countdown > 0 {
                *countdown -= 1;
            } else {
                // Countdown finished: transition to the stored next mode
                // (set by start_countdown_mode). Without a stored next mode
                // we just clear the countdown, as before. A race whose
                // rules have a formation lap drives it first.
                self.session.countdown_ticks_remaining = None;
                if self.session.next_mode == Some(GameMode::Race) && self.formation_lap_due() {
                    self.begin_formation();
                    return;
                }
                if let Some(next_mode) = self.session.next_mode.take() {
                    self.transition_from_countdown(next_mode);
                }
            }
        }
        // Cars are held where they stand, no physics: the drivers can only
        // pick a gear and rev their engines. The countdown may have just
        // handed over to the next mode, which then owns this tick's cars.
        if self.session.game_mode != GameMode::Countdown {
            return;
        }
        self.hold_cars_on_grid(inputs);
    }

    /// The grid before the countdown: held as in the countdown, with no
    /// clock. Once every connected human is ready the countdown starts.
    fn tick_grid(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_pits();
        if self.grid_is_ready() {
            self.start_countdown_mode(self.grid_countdown_seconds, GameMode::Race);
            return;
        }
        self.hold_cars_on_grid(inputs);
    }

    /// Cars are held where they stand, no physics: the drivers can only
    /// pick a gear and rev their engines. Under the false-start rule
    /// (`RaceRules::jump_start`) a driver's car in gear with the throttle
    /// open is let go of through the countdown into the race
    /// (`crate::race_start::releases`), and one that moves off its slot has
    /// jumped the start.
    fn hold_cars_on_grid(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        let dt = self.dt();
        let watch = self.jump_watch_active();
        let drivers: std::collections::BTreeSet<PlayerId> = if watch {
            self.session
                .participants
                .keys()
                .filter(|id| {
                    !self.is_ai_player(id) && !self.debug.stand_in_driver && !self.away.contains(id)
                })
                .copied()
                .collect()
        } else {
            Default::default()
        };
        let tick = self.session.current_tick;
        let rate = self.tick_rate_hz;
        for state in self.session.participants.values_mut() {
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();
            let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state)
            else {
                continue;
            };
            let id = state.player_id;
            if watch && self.race_start.released.contains(&id) {
                physics::update_car_3d(state, config, &input, &self.track_config, dt);
                let _ = physics::update_track_progress_3d(state, &self.track_config, tick, rate);
                let slot = self
                    .track_config
                    .start_positions
                    .iter()
                    .find(|s| s.position == state.grid_position);
                if slot.is_some_and(|slot| crate::race_start::jumped(state, slot))
                    && self.race_start.jumped.insert(id)
                {
                    info!("Jump start: car on slot {}", state.grid_position);
                }
                continue;
            }
            physics::update_car_on_grid(state, config, &input, dt);
            if drivers.contains(&id) && crate::race_start::releases(state, &input) {
                self.race_start.released.insert(id);
            }
        }
    }

    /// Whether the countdown running now ends in the green light of a
    /// standing start under the false-start rule.
    fn jump_watch_active(&self) -> bool {
        self.session.race_rules.jump_start
            && self.session.game_mode == GameMode::Countdown
            && self.session.next_mode == Some(GameMode::Race)
            && self.session.countdown_ticks_remaining.is_some()
            && !self.formation_lap_due()
    }

    /// Whether the race about to start drives a formation (or pace) lap
    /// first: its rules ask for one and this start has not run it.
    fn formation_lap_due(&self) -> bool {
        self.session.race_rules.has_formation_lap()
            && !self.race_start.lap_run
            && !self.track_config.start_positions.is_empty()
            && crate::laps::track_length_m(&self.track_config) > 0.0
    }

    /// The countdown has run out into a formation lap: every car is let go
    /// of, held to the formation pace, the clock stopped until the field is
    /// back (`tick_formation`).
    fn begin_formation(&mut self) {
        let total = crate::laps::track_length_m(&self.track_config);
        let rolling = self.session.race_rules.start == crate::data::StartType::Rolling;
        self.race_start.formation = Some(crate::race_start::Formation::begin(
            self.session
                .participants
                .values()
                .filter(|s| s.damage.is_drivable && !s.towed && !s.in_garage && !s.is_ghost()),
            &self.track_config,
            total,
            rolling,
            self.session.current_tick,
            self.tick_rate_hz,
        ));
        for state in self.session.participants.values_mut() {
            state.speed_cap_mps = crate::race_start::FORMATION_SPEED_MPS;
            // The automatic box takes first as the field moves off.
            if state.auto_gearbox && state.gear == 0 {
                state.gear = 1;
                state.auto_shift_hold_ticks = 0;
            }
        }
        info!(
            "Session {}: {} lap begins",
            self.session.id,
            if rolling { "pace" } else { "formation" }
        );
    }

    /// One tick of the formation lap: the field driven as in a race, held
    /// to the formation pace, no lap timed; over the approach each car is
    /// driven onto its slot and stopped there (`crate::race_start`). A
    /// standing start counts the lights down once every car is home; a
    /// rolling start goes green as the leader reaches the line.
    fn tick_formation(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        use crate::race_start::{approach_input, APPROACH_M};
        let total = crate::laps::track_length_m(&self.track_config);
        let dt = self.dt();
        self.update_air();
        let Some(formation) = self.race_start.formation.as_mut() else {
            return;
        };
        let rolling = formation.rolling;

        // Each car's pace (single file, in grid order), and the server
        // drives each car over its approach to its slot.
        let field: std::collections::BTreeMap<PlayerId, (f32, bool)> = self
            .session
            .participants
            .values()
            .map(|s| {
                let running = s.is_on_track && s.damage.is_drivable && !s.towed && !s.in_garage;
                (s.player_id, (s.speed_mps, running))
            })
            .collect();
        let paces = formation.paces(&field, total);
        let mut inputs = inputs.clone();
        if !rolling {
            for state in self.session.participants.values() {
                let Some(car) = formation.cars.get(&state.player_id) else {
                    continue;
                };
                if car.home || car.left_m > APPROACH_M {
                    continue;
                }
                let Some(config) = self.car_configs.get(&state.car_config_id) else {
                    continue;
                };
                let pace = paces.get(&state.player_id).copied().unwrap_or(0.0);
                inputs.insert(
                    state.player_id,
                    approach_input(state, config, &self.track_config, car, pace),
                );
            }
        }

        let tick = self.session.current_tick;
        let rate = self.tick_rate_hz;
        for state in self.session.participants.values_mut() {
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();
            let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state)
            else {
                continue;
            };
            let Some(car) = formation.cars.get_mut(&state.player_id).filter(|c| !c.home) else {
                // Home on its slot (or never part of the lap): held.
                physics::update_car_on_grid(state, config, &input, dt);
                continue;
            };
            // Held to it as by a limiter; a car told to wait is held still
            // (a cap of 0 is none).
            state.speed_cap_mps = paces
                .get(&state.player_id)
                .copied()
                .unwrap_or(crate::race_start::FORMATION_SPEED_MPS)
                .max(0.05);
            // The approach drives on the automatic box, as the pit route
            // does: a car slowed to a stop in a high gear would stall.
            if !rolling && car.left_m <= APPROACH_M && car.own_auto_gearbox.is_none() {
                car.own_auto_gearbox = Some(state.auto_gearbox);
                state.auto_gearbox = true;
            }
            physics::update_car_3d(state, config, &input, &self.track_config, dt);
            // Nothing is timed on the formation lap.
            let _ = physics::update_track_progress_3d(state, &self.track_config, tick, rate);
        }
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);

        // Each car's headway, and who is home.
        let Some(formation) = self.race_start.formation.as_mut() else {
            return;
        };
        let timed_out = tick >= formation.deadline_tick;
        for state in self.session.participants.values_mut() {
            formation.advance(state, total);
            let Some(car) = formation.cars.get_mut(&state.player_id) else {
                continue;
            };
            if car.home || rolling {
                continue;
            }
            let out = !state.damage.is_drivable || state.towed || state.in_garage;
            let stopped = car.left_m <= crate::race_start::HOME_M
                && state.speed_mps < crate::race_start::HOME_SPEED_MPS;
            if !(out || stopped || car.left_m < -5.0 || timed_out) {
                continue;
            }
            car.home = true;
            state.speed_cap_mps = 0.0;
            if let Some(own) = car.own_auto_gearbox.take() {
                state.auto_gearbox = own;
            }
            if out {
                continue;
            }
            if let Some(slot) = self
                .track_config
                .start_positions
                .iter()
                .find(|s| s.position == state.grid_position)
            {
                place_still(state, slot.x, slot.y, slot.z, slot.yaw_rad);
            }
            crate::race_start::reset_laps(state, &self.track_config);
            state.gear = 0;
        }

        if rolling {
            let green = formation.leader_left_m().is_none_or(|left| left <= 0.0) || timed_out;
            if green {
                self.race_start.formation = None;
                self.race_start.lap_run = true;
                for state in self.session.participants.values_mut() {
                    state.speed_cap_mps = 0.0;
                    crate::race_start::reset_laps(state, &self.track_config);
                }
                info!("Session {}: rolling start, green", self.session.id);
                self.session.next_mode = None;
                self.transition_from_countdown(GameMode::Race);
            }
        } else if formation.all_home() {
            self.race_start.formation = None;
            self.race_start.lap_run = true;
            self.session.countdown_ticks_remaining = Some(
                (self.tick_rate_hz as u32 * crate::race_start::LIGHTS_SECONDS as u32)
                    .min(u16::MAX as u32) as u16,
            );
            info!(
                "Session {}: the field is back on the grid, the lights",
                self.session.id
            );
        }
    }

    /// Whether the grid may count down: at least one human is connected,
    /// and every connected one is ready. A driver away (a held seat) is not
    /// waited for.
    fn grid_is_ready(&self) -> bool {
        let mut connected = self
            .session
            .participants
            .keys()
            .filter(|id| !self.session.ai_player_ids.contains(id) && !self.away.contains(id))
            .peekable();
        connected.peek().is_some() && connected.all(|id| self.ready.contains(id))
    }

    /// Demo lap mode: AI driver demonstrates the track
    fn tick_demolap(&mut self, player_inputs: &HashMap<PlayerId, PlayerInputData>) {
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Initialize demo lap progress if not set
        if self.session.demo_lap_progress.is_none() {
            self.session.demo_lap_progress = Some(0.0);
        }

        // Use AI-driven demo lap if we have AI drivers
        if !self.session.ai_player_ids.is_empty() {
            // Merge player inputs with AI inputs
            let mut inputs = player_inputs.clone();
            for ai_id in &self.session.ai_player_ids {
                let ai_input = self.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }

            // Update physics for all cars (player + AI), each in the wake
            // of the cars ahead of it and the wind.
            self.update_wind();
            crate::slipstream::update(&mut self.session.participants, &self.car_configs);
            let ai_ids: std::collections::HashSet<PlayerId> =
                self.session.ai_player_ids.iter().copied().collect();
            let current_tick = self.session.current_tick;
            let tick_rate_hz = self.tick_rate_hz;
            let clock = car_step::StepClock {
                tick: current_tick,
                tick_rate_hz,
            };
            let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();
            for state in states.iter_mut() {
                let input = inputs.get(&state.player_id).copied().unwrap_or_default();

                if let Some(config) =
                    simulated_config(&self.car_configs, &self.tuned_configs, state)
                {
                    let is_ai = ai_ids.contains(&state.player_id);
                    let lap_traces = &mut self.lap_traces;
                    let ghost = is_ai && !self.hotlap_watch.active;
                    if let Some(event) = car_step::step_car(
                        state,
                        config,
                        &input,
                        &self.track_config,
                        clock,
                        |moved| {
                            sample_ghost_trace(lap_traces, moved, ghost, current_tick, tick_rate_hz)
                        },
                    ) {
                        note_lap_event(
                            &mut self.lap_events,
                            &mut self.lap_traces,
                            &mut self.session_best_lap_ms,
                            &mut self.session_best_splits_ms,
                            state.player_id,
                            is_ai && !self.hotlap_watch.active,
                            event,
                        );
                    }
                }
            }

            return;
        }

        // Fallback to old camera-following demo lap if no AI
        if self.track_config.raceline.is_empty() {
            // No racing line available, can't do demo lap
            return;
        }

        let raceline_len = self.track_config.raceline.len();

        if let Some(ref mut progress) = self.session.demo_lap_progress {
            // Calculate position on racing line
            let index = (*progress * raceline_len as f32).floor() as usize;
            let next_index = (index + 1) % raceline_len;
            let t = (*progress * raceline_len as f32) - index as f32;

            let p1 = &self.track_config.raceline[index];
            let p2 = &self.track_config.raceline[next_index];

            // Calculate curvature by looking ahead
            let lookahead_distance = 10; // Points to look ahead
            let ahead_index = (index + lookahead_distance) % raceline_len;
            let way_ahead_index = (index + lookahead_distance * 2) % raceline_len;

            let p_ahead = &self.track_config.raceline[ahead_index];
            let p_way_ahead = &self.track_config.raceline[way_ahead_index];

            // Calculate vectors for curvature estimation
            let v1_x = p_ahead.x - p1.x;
            let v1_y = p_ahead.y - p1.y;
            let v2_x = p_way_ahead.x - p_ahead.x;
            let v2_y = p_way_ahead.y - p_ahead.y;

            let len1 = (v1_x * v1_x + v1_y * v1_y).sqrt();
            let len2 = (v2_x * v2_x + v2_y * v2_y).sqrt();

            // Calculate angle change (curvature indicator)
            let mut curvature = 0.0;
            if len1 > 0.001 && len2 > 0.001 {
                // Dot product to find angle between vectors
                let dot = (v1_x * v2_x + v1_y * v2_y) / (len1 * len2);
                let angle_change = dot.clamp(-1.0, 1.0).acos();
                curvature = angle_change;
            }

            // Speed control based on curvature (adjusted for realistic lap times)
            // Max speed on straights: 60 m/s (216 km/h)
            // Min speed in tight corners: 25 m/s (90 km/h)
            let max_speed = 60.0;
            let min_speed = 25.0;

            // Map curvature (0 to ~PI) to speed range
            // High curvature (sharp corner) = low speed
            // Low curvature (straight) = high speed
            let curvature_factor = 1.0 - (curvature / std::f32::consts::PI).min(1.0);
            let target_speed = min_speed + (max_speed - min_speed) * curvature_factor;

            // Get current speed from demo car or use target speed
            let current_speed = self
                .session
                .participants
                .values()
                .next()
                .map(|car| car.speed_mps)
                .unwrap_or(target_speed);

            // Smooth acceleration/braking
            let accel_rate = 15.0; // m/s² acceleration
            let brake_rate = 25.0; // m/s² braking

            let demo_speed = if current_speed < target_speed {
                // Accelerate
                (current_speed + accel_rate * dt).min(target_speed)
            } else {
                // Brake
                (current_speed - brake_rate * dt).max(target_speed)
            };

            // Advance progress along the racing line based on current speed
            *progress += (demo_speed * dt) / raceline_len as f32;

            // Loop back when completing the lap
            if *progress >= 1.0 {
                *progress = 0.0;
            }

            // Interpolate position
            let x = p1.x + (p2.x - p1.x) * t;
            let y = p1.y + (p2.y - p1.y) * t;
            let z = p1.z + (p2.z - p1.z) * t;

            // Update demo car if there's one participant
            // In demo mode, we should have a single demo car
            if let Some(demo_car) = self.session.participants.values_mut().next() {
                demo_car.pos_x = x;
                demo_car.pos_y = y;
                demo_car.pos_z = z + 1.2; // Camera height 1.2m from surface

                // Calculate forward direction
                let dx = p2.x - p1.x;
                let dy = p2.y - p1.y;
                let dz = p2.z - p1.z;
                let len = (dx * dx + dy * dy + dz * dz).sqrt();

                if len > 0.001 {
                    // Set velocity to move forward along racing line
                    demo_car.vel_x = (dx / len) * demo_speed;
                    demo_car.vel_y = (dy / len) * demo_speed;
                    demo_car.vel_z = (dz / len) * demo_speed;
                    demo_car.speed_mps = demo_speed;

                    // Calculate yaw (heading) from velocity direction
                    demo_car.yaw_rad = (dy / len).atan2(dx / len);
                }
            }
        }
    }

    /// Free practice mode: Players drive freely with lap timing
    fn tick_free_practice(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_air();
        let autopilot = self.pit_autopilot_inputs(inputs);
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // Update each car
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;
        let tick_rate_hz = self.tick_rate_hz;
        let clock = car_step::StepClock {
            tick: current_tick,
            tick_rate_hz,
        };
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing);
            // the pit autopilot's while it holds the car.
            let input = autopilot
                .get(&state.player_id)
                .or_else(|| inputs.get(&state.player_id))
                .copied()
                .unwrap_or_default();

            // Get car config
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                // The car's own tick (`car_step`); a timing line crossed
                // here becomes a split on every driver's screen.
                let is_ai = ai_ids.contains(&state.player_id);
                let lap_traces = &mut self.lap_traces;
                let ghost = is_ai && !self.hotlap_watch.active;
                if let Some(event) =
                    car_step::step_car(state, config, &input, &self.track_config, clock, |moved| {
                        sample_ghost_trace(lap_traces, moved, ghost, current_tick, tick_rate_hz)
                    })
                {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai && !self.hotlap_watch.active,
                        event,
                    );
                }
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);
    }

    /// Hotlap and qualifying: free practice for the cars on the track, while the cars
    /// in the garage stand still — not simulated, not collided with, so a
    /// driver tuning in the garage is out of everyone's way.
    fn tick_hotlap(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.update_air();
        let autopilot = self.pit_autopilot_inputs(inputs);
        let dt = self.dt();
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;
        let tick_rate_hz = self.tick_rate_hz;
        let clock = car_step::StepClock {
            tick: current_tick,
            tick_rate_hz,
        };
        let track_length = crate::laps::track_length_m(&self.track_config);
        // Cars that crossed the start line this tick, and whether each was
        // on a timed lap (not its outlap): after a timed qualifying
        // session's clock, each of them takes the flag; a timed lap ended
        // counts against the lap allowance and ends a shoot-out run.
        let mut crossed: Vec<(PlayerId, bool)> = Vec::new();

        for state in self.session.participants.values_mut() {
            if state.in_garage {
                continue;
            }
            let input = autopilot
                .get(&state.player_id)
                .or_else(|| inputs.get(&state.player_id))
                .copied()
                .unwrap_or_default();
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                let old_progress = state.track_progress;
                let was_timed = state.current_lap > 0;
                let is_ai = ai_ids.contains(&state.player_id);
                let lap_traces = &mut self.lap_traces;
                let ghost = is_ai && !self.hotlap_watch.active;
                if let Some(event) =
                    car_step::step_car(state, config, &input, &self.track_config, clock, |moved| {
                        sample_ghost_trace(lap_traces, moved, ghost, current_tick, tick_rate_hz)
                    })
                {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai && !self.hotlap_watch.active,
                        event,
                    );
                }
                if track_length > 0.0
                    && old_progress > track_length * 0.8
                    && state.track_progress < track_length * 0.2
                {
                    crossed.push((state.player_id, was_timed));
                }
            }
        }

        // Only the cars on the track take part in the collision passes; the
        // order is still the BTreeMap's, so the solver stays deterministic.
        let mut state_refs: Vec<&mut CarState> = self
            .session
            .participants
            .values_mut()
            .filter(|s| !s.in_garage)
            .collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);

        if self.session.game_mode.has_outlap() {
            let crossers: Vec<PlayerId> = crossed.iter().map(|(id, _)| *id).collect();
            self.update_chequered_flag(&crossers);
        }
        if self.session.game_mode == GameMode::Qualification {
            if self.shootout.is_some() {
                self.update_shootout(&crossed);
            } else {
                self.update_lap_allowance(&crossed);
            }
        }
    }

    // --- Qualifying formats ------------------------------------------------

    /// How qualifying runs (`CreateSession.qualifying_format`).
    pub fn set_qualifying_format(&mut self, format: crate::data::QualifyingFormat) {
        self.session.qualifying_format = format;
    }

    /// The qualifying lap allowance (`CreateSession.qualifying_lap_allowance`,
    /// clamped); 0 for none.
    pub fn set_qualifying_lap_allowance(&mut self, laps: u8) {
        self.session.qualifying_lap_allowance = crate::data::clamp_qualifying_lap_allowance(laps);
    }

    /// Whether the qualifying running now is a one-lap shoot-out: a plain
    /// session's qualifying under `QualifyingFormat::Shootout`, or a
    /// weekend's final round under it.
    pub fn is_shootout_round(&self) -> bool {
        if self.session.game_mode != GameMode::Qualification
            || self.session.qualifying_format != crate::data::QualifyingFormat::Shootout
        {
            return false;
        }
        match self.session.weekend.as_ref() {
            Some(format) => self.weekend.round as usize >= format.rounds(),
            None => true,
        }
    }

    /// A shoot-out begins: every car taking part to its garage, AI too, the
    /// classification and the timing sheets empty, the run order fixed
    /// ([`Self::shootout_order`]), and the first car called out.
    fn begin_shootout(&mut self) {
        let order = self.shootout_order();
        self.qualifying.clear();
        self.clear_timing_sheets();
        let all: Vec<PlayerId> = self.session.participants.keys().copied().collect();
        for player_id in all {
            let _ = self.relocate(&player_id, HotlapDestination::Garage, false);
        }
        self.shootout = Some(Shootout {
            order,
            called: 0,
            runner: None,
        });
        self.shootout_sent = None;
        self.roster_dirty = true;
        self.call_next_runner();
    }

    /// The shoot-out's run order, first out first: the reverse of what the
    /// session stands on when it begins, so the car with most to defend
    /// runs last. A weekend's final runs in the reverse of the round before
    /// it (the slowest of those through first); a weekend whose only round
    /// is the shoot-out in the reverse of practice's best laps, cars with
    /// no time first; a plain session in the reverse of its start order
    /// (`starting_slots`: the host's order, an earlier qualifying's result,
    /// else the seating order). Knocked-out cars take no part.
    fn shootout_order(&self) -> Vec<PlayerId> {
        let taking_part: Vec<&CarState> = self
            .session
            .participants
            .values()
            .filter(|s| self.weekend.eliminated_in(&s.player_id).is_none())
            .collect();
        // Front first: a lower key starts further up the order it reverses.
        let mut keyed: Vec<((u32, u8, PlayerId), PlayerId)> = if self.is_weekend() {
            if !self.weekend.last_round.is_empty() {
                taking_part
                    .iter()
                    .map(|s| {
                        let place = self
                            .weekend
                            .last_round
                            .iter()
                            .position(|(id, _)| *id == s.player_id)
                            .map_or(u32::MAX, |i| i as u32);
                        ((place, s.grid_position, s.player_id), s.player_id)
                    })
                    .collect()
            } else {
                taking_part
                    .iter()
                    .map(|s| {
                        let best = s.best_lap_time_ms.filter(|ms| *ms > 0).unwrap_or(u32::MAX);
                        ((best, s.grid_position, s.player_id), s.player_id)
                    })
                    .collect()
            }
        } else {
            let slots = self.starting_slots();
            taking_part
                .iter()
                .map(|s| {
                    let slot = slots.get(&s.player_id).map_or(u32::MAX, |n| *n as u32);
                    ((slot, s.grid_position, s.player_id), s.player_id)
                })
                .collect()
        };
        keyed.sort();
        keyed.into_iter().rev().map(|(_, id)| id).collect()
    }

    /// The next car in the order goes out from the pit exit onto its
    /// outlap; a car that has left, or whose driver is away, is passed
    /// over. With the order run through, the shoot-out (and the session's
    /// qualifying) is over: `Finished`.
    fn call_next_runner(&mut self) {
        loop {
            let Some(shootout) = self.shootout.as_mut() else {
                return;
            };
            shootout.runner = None;
            let Some(&player_id) = shootout.order.get(shootout.called) else {
                self.session.state = SessionState::Finished;
                tracing::info!("Session {}: the shoot-out is over", self.session.id);
                return;
            };
            shootout.called += 1;
            if !self.session.participants.contains_key(&player_id) || self.is_away(&player_id) {
                continue;
            }
            if self
                .relocate(&player_id, HotlapDestination::Track, false)
                .is_err()
            {
                continue;
            }
            let rate = self.tick_rate_hz as u32;
            let lap_s = self
                .session
                .participants
                .get(&player_id)
                .and_then(|s| self.lap_seconds.get(&s.car_config_id).copied())
                .unwrap_or(FALLBACK_LAP_SECONDS);
            let run_s = ((lap_s * SHOOTOUT_RUN_LAPS) as u32).max(SHOOTOUT_RUN_MIN_SECONDS);
            let deadline = self.session.current_tick + run_s * rate;
            if let Some(shootout) = self.shootout.as_mut() {
                shootout.runner = Some((player_id, deadline));
            }
            return;
        }
    }

    /// A shoot-out after the tick: the car on its run is done when it ends
    /// its flying lap at the line (the lap is queued and classified as any
    /// other), goes to its garage, leaves, is wrecked, or runs out of time;
    /// it is garaged and the next car called. A driver who joins a plain
    /// session's shoot-out runs at the back of the order.
    fn update_shootout(&mut self, crossed: &[(PlayerId, bool)]) {
        if self.session.state == SessionState::Finished {
            return;
        }
        let newcomers: Vec<PlayerId> = match self.shootout.as_ref() {
            Some(shootout) => self
                .session
                .participants
                .keys()
                .filter(|id| !shootout.order.contains(id))
                .filter(|id| self.weekend.eliminated_in(id).is_none())
                .copied()
                .collect(),
            None => return,
        };
        if let Some(shootout) = self.shootout.as_mut() {
            shootout.order.extend(newcomers);
        }
        let Some((runner, deadline)) = self.shootout.as_ref().and_then(|s| s.runner) else {
            // Nobody out (every car before the newcomers had run): call one.
            self.call_next_runner();
            return;
        };
        let over = match self.session.participants.get(&runner) {
            None => true,
            Some(state) => {
                state.in_garage
                    || !state.damage.is_drivable
                    || self.session.current_tick >= deadline
                    || crossed.iter().any(|(id, timed)| *id == runner && *timed)
            }
        };
        if over {
            if self
                .session
                .participants
                .get(&runner)
                .is_some_and(|s| !s.in_garage)
            {
                let _ = self.relocate(&runner, HotlapDestination::Garage, false);
            }
            self.call_next_runner();
        }
    }

    /// The lap allowance after the tick: each car that ended a timed lap
    /// has it counted, and one that has used its allowance goes to its
    /// garage for good. Once every car taking part has used it and is in,
    /// the session (or round) is over.
    fn update_lap_allowance(&mut self, crossed: &[(PlayerId, bool)]) {
        let allowance = self.session.qualifying_lap_allowance;
        if allowance == 0 || self.session.state == SessionState::Finished {
            return;
        }
        for (player_id, timed) in crossed {
            if !*timed {
                continue;
            }
            let run = self.qualifying_laps_run.entry(*player_id).or_insert(0);
            *run = run.saturating_add(1);
            // The roster carries the count (`RosterEntry::qualifying_laps`).
            self.roster_dirty = true;
            if *run >= allowance {
                let _ = self.relocate(player_id, HotlapDestination::Garage, false);
            }
        }
        let all_done = self
            .session
            .participants
            .values()
            .filter(|s| self.weekend.eliminated_in(&s.player_id).is_none())
            .all(|s| {
                self.qualifying_laps_run
                    .get(&s.player_id)
                    .is_some_and(|n| *n >= allowance)
                    && (s.in_garage || !s.damage.is_drivable)
            });
        if all_done && !self.session.participants.is_empty() {
            self.session.state = SessionState::Finished;
        }
    }

    /// Where the shoot-out stands, for the wire; `None` outside one. A car
    /// that has left the session is dropped from the order.
    pub fn shootout_state(&self) -> Option<ShootoutState> {
        if self.session.game_mode != GameMode::Qualification {
            return None;
        }
        let shootout = self.shootout.as_ref()?;
        let runner = shootout.runner.map(|(id, _)| id);
        let mut order = Vec::new();
        let mut running = 0u8;
        let mut done = 0u8;
        for (i, id) in shootout.order.iter().enumerate() {
            let Some(car_index) = self.car_index_of(id) else {
                continue;
            };
            order.push(car_index);
            if Some(*id) == runner {
                running = order.len().min(u8::MAX as usize) as u8;
            } else if i < shootout.called {
                done = done.saturating_add(1);
            }
        }
        let flying = runner
            .and_then(|id| self.session.participants.get(&id))
            .is_some_and(|s| !s.outlap && s.current_lap > 0);
        Some(ShootoutState {
            order,
            running,
            done,
            flying,
        })
    }

    /// The shoot-out's state when it changed since the last call, for the
    /// game loop to send to the whole session.
    pub fn take_shootout_state(&mut self) -> Option<ShootoutState> {
        let state = self.shootout_state()?;
        if self.shootout_sent.as_ref() == Some(&state) {
            return None;
        }
        self.shootout_sent = Some(state.clone());
        Some(state)
    }

    /// A timed qualifying or practice session after its clock: each car
    /// crossing the
    /// line takes the chequered flag and goes to its garage (the lap it
    /// just completed is already timed and queued in `lap_events`, so it
    /// still classifies). The session is finished once every car is in its
    /// garage or undrivable, or at the deadline set when the flag came out
    /// (max([`QUALIFYING_FLAG_GRACE_MIN_SECONDS`],
    /// [`QUALIFYING_FLAG_GRACE_LAPS`] x the slowest car's lap)), when the
    /// cars still out are garaged.
    fn update_chequered_flag(&mut self, crossed: &[PlayerId]) {
        if !self.chequered_flag_out() || self.session.state == SessionState::Finished {
            return;
        }
        if self.finish_deadline_tick.is_none() {
            let rate = self.tick_rate_hz as f32;
            let slowest_lap_s = self
                .session
                .participants
                .values()
                .filter(|s| !s.in_garage)
                .map(|s| {
                    s.last_lap_time_ms
                        .or(s.best_lap_time_ms)
                        .filter(|ms| *ms > 0)
                        .map(|ms| ms as f32 / 1000.0)
                        .or_else(|| self.lap_seconds.get(&s.car_config_id).copied())
                        .unwrap_or(FALLBACK_LAP_SECONDS)
                })
                .fold(0.0f32, f32::max);
            let grace_ticks = ((slowest_lap_s * QUALIFYING_FLAG_GRACE_LAPS * rate) as u32)
                .max(QUALIFYING_FLAG_GRACE_MIN_SECONDS * self.tick_rate_hz as u32);
            self.finish_deadline_tick = Some(self.session.current_tick + grace_ticks);
            tracing::info!(
                "Session {}: the {:?} clock has run out; the chequered flag is out",
                self.session.id,
                self.session.game_mode
            );
        }
        for player_id in crossed {
            let _ = self.hotlap_relocate(player_id, HotlapDestination::Garage, false);
        }
        let at_deadline = self
            .finish_deadline_tick
            .is_some_and(|deadline| self.session.current_tick >= deadline);
        if at_deadline {
            let still_out: Vec<PlayerId> = self
                .session
                .participants
                .values()
                .filter(|s| !s.in_garage)
                .map(|s| s.player_id)
                .collect();
            for player_id in still_out {
                let _ = self.hotlap_relocate(&player_id, HotlapDestination::Garage, false);
            }
        }
        let all_in = self
            .session
            .participants
            .values()
            .all(|s| s.in_garage || !s.damage.is_drivable);
        if at_deadline || all_in {
            self.session.state = SessionState::Finished;
        }
    }

    /// Ticks since this session entered its timed mode (qualifying or
    /// practice); 0 outside them.
    fn timed_elapsed_ticks(&self) -> u64 {
        match self.timed_start_tick {
            Some(start) if self.session.game_mode.has_outlap() => {
                self.session.current_tick.saturating_sub(start) as u64
            }
            _ => 0,
        }
    }

    /// The length of the timed session running now, s: a weekend's
    /// practice or current qualifying round, or a plain session's
    /// `qualifying_seconds`. `None` for open-ended qualifying or practice
    /// and outside them.
    fn timed_session_seconds(&self) -> Option<u32> {
        let weekend = self.session.weekend.as_ref();
        match self.session.game_mode {
            GameMode::Practice => weekend
                .map(|w| w.practice_seconds)
                .filter(|seconds| *seconds > 0),
            // A shoot-out runs until every car has had its run: no clock.
            GameMode::Qualification if self.is_shootout_round() => None,
            GameMode::Qualification => match weekend {
                Some(w) => w
                    .qualifying_seconds
                    .get((self.weekend.round as usize).saturating_sub(1))
                    .copied(),
                None => self.session.qualifying_seconds,
            },
            _ => None,
        }
    }

    /// The timed session's length in ticks; `None` when it is open-ended.
    pub fn timed_length_ticks(&self) -> Option<u64> {
        self.timed_session_seconds()
            .map(|s| s as u64 * self.tick_rate_hz as u64)
    }

    /// The chequered flag is out: a timed qualifying or practice session
    /// whose clock has run out. Cars take it at their next crossing of the
    /// line, and none may go out again.
    pub fn chequered_flag_out(&self) -> bool {
        self.session.game_mode.has_outlap()
            && self
                .timed_length_ticks()
                .is_some_and(|length| self.timed_elapsed_ticks() >= length)
    }

    /// Set the length of a timed qualifying session
    /// (`CreateSession.qualifying_seconds`, clamped by
    /// `data::clamp_qualifying_seconds`); `None` for open-ended qualifying.
    /// A weekend's rounds carry their own lengths, so it is ignored there.
    /// The qualifying cut-off (`CreateSession.qualifying_cutoff_pct`,
    /// clamped): see [`Self::non_qualifiers`].
    pub fn set_qualifying_cutoff(&mut self, pct: Option<u16>) {
        self.session.qualifying_cutoff_pct = crate::data::clamp_qualifying_cutoff(pct);
    }

    /// The race-control rules (`CreateSession.race_rules`): see
    /// [`crate::data::RaceRules`].
    pub fn set_race_rules(&mut self, rules: crate::data::RaceRules) {
        self.session.race_rules = rules;
    }

    /// What the race is doing for telemetry (`CompactTelemetry.race_phase`):
    /// its formation or pace lap, the virtual safety car out or ending;
    /// `None` otherwise.
    pub fn race_phase(&self) -> Option<u8> {
        use crate::race_control::Course;
        use crate::race_start::RacePhase;
        if let Some(formation) = self.race_start.formation.as_ref() {
            return Some(if formation.rolling {
                RacePhase::PaceLap
            } else {
                RacePhase::Formation
            } as u8);
        }
        match self.race_control.course {
            Course::Green => None,
            Course::Vsc => Some(RacePhase::Vsc as u8),
            Course::VscEnding => Some(RacePhase::VscEnding as u8),
            Course::Sc => Some(RacePhase::SafetyCar as u8),
            Course::ScInThisLap => Some(RacePhase::ScInThisLap as u8),
        }
    }

    /// The race's pit-stop rule (`CreateSession.mandatory_stops` and
    /// `compound_change`, clamped): see [`crate::data::PitRule`].
    pub fn set_pit_rule(&mut self, mandatory_stops: u8, compound_change: bool) {
        self.session.pit_rule = crate::data::PitRule::clamped(mandatory_stops, compound_change);
    }

    /// What a car still owes the pit rule in a race: stops, and the second
    /// compound (`data::PitRule::owed`). Nothing outside a race.
    fn stops_owed(&self, state: &CarState) -> (u32, bool) {
        if self.session.game_mode != GameMode::Race {
            return (0, false);
        }
        self.session
            .pit_rule
            .owed(state.pit.stops, state.pit.compound_changed)
    }

    /// The drivers the qualifying cut-off keeps off the race's grid: every
    /// seated driver slower than `qualifying_cutoff_pct` of pole, or with no
    /// time. A weekend's cut-off is taken on Q1, which every car ran (a
    /// late entry ran none); a plain session's on its own qualifying. Empty
    /// without a cut-off, before qualifying has classified anyone, or with
    /// no time set at all.
    pub fn non_qualifiers(&self) -> std::collections::BTreeSet<PlayerId> {
        let mut out = std::collections::BTreeSet::new();
        let Some(pct) = self.session.qualifying_cutoff_pct else {
            return out;
        };
        let lap = |id: &PlayerId| -> u32 {
            if self.is_weekend() {
                self.weekend
                    .first_round
                    .iter()
                    .find(|(p, _)| p == id)
                    .map_or(0, |(_, ms)| *ms)
            } else {
                self.qualifying.get(id).map_or(0, |(ms, _)| *ms)
            }
        };
        let ran = if self.is_weekend() {
            !self.weekend.first_round.is_empty()
        } else {
            !self.qualifying.is_empty()
        };
        if !ran {
            return out;
        }
        let Some(pole) = self
            .session
            .participants
            .keys()
            .map(lap)
            .filter(|ms| *ms > 0)
            .min()
        else {
            return out;
        };
        let cutoff = pole as u64 * pct as u64 / 100;
        for id in self.session.participants.keys() {
            let ms = lap(id);
            if ms == 0 || ms as u64 > cutoff {
                out.insert(*id);
            }
        }
        out
    }

    pub fn set_qualifying_seconds(&mut self, seconds: Option<u32>) {
        self.session.qualifying_seconds = if self.session.weekend.is_some() {
            None
        } else {
            crate::data::clamp_qualifying_seconds(seconds)
        };
    }

    // --- A race weekend --------------------------------------------------

    /// Make this session a race weekend (`CreateSession.weekend`, clamped
    /// by `WeekendFormat::clamp`), or a plain session with `None`. A
    /// weekend's rounds carry their own lengths: `qualifying_seconds` goes.
    pub fn set_weekend(&mut self, format: Option<crate::weekend::WeekendFormat>) {
        self.session.weekend = format.map(crate::weekend::WeekendFormat::clamp);
        if self.session.weekend.is_some() {
            self.session.qualifying_seconds = None;
        }
        self.weekend = Default::default();
        self.weekend_sent = None;
    }

    /// Whether this session is a race weekend.
    pub fn is_weekend(&self) -> bool {
        self.session.weekend.is_some()
    }

    /// The host moves the weekend on (`ClientMessage::AdvanceWeekend`):
    /// from the lobby to practice (or Q1 when there is none), from finished
    /// practice to Q1, from a finished round to the next, and from the
    /// finished final round onto the race's grid ([`Self::open_grid`],
    /// [`START_SESSION_COUNTDOWN_SECONDS`] once every driver is ready),
    /// gridded by the weekend, so drivers change tyres, setup and livery
    /// after qualifying. With no driver connected there is nobody to say
    /// ready, and the race is counted straight in.
    /// Refused (the reason) outside a weekend, while a phase runs, and once
    /// the race has begun.
    pub fn advance_weekend(&mut self) -> Result<(), &'static str> {
        use crate::weekend::WeekendPhase;
        let Some(format) = self.session.weekend.clone() else {
            return Err("Not a race weekend");
        };
        self.settle_weekend();
        let finished = self.session.state == SessionState::Finished;
        match self.weekend.phase {
            WeekendPhase::Lobby if format.practice_seconds > 0 => self.begin_practice(),
            WeekendPhase::Lobby => self.begin_round(1),
            WeekendPhase::Practice if finished => self.begin_round(1),
            WeekendPhase::Practice => return Err("Practice is still running"),
            WeekendPhase::Qualifying if !finished => return Err("Qualifying is still running"),
            WeekendPhase::Qualifying if (self.weekend.round as usize) < format.rounds() => {
                self.begin_round(self.weekend.round + 1)
            }
            WeekendPhase::Qualifying if self.has_connected_driver() => {
                self.open_grid(START_SESSION_COUNTDOWN_SECONDS)
            }
            WeekendPhase::Qualifying => {
                self.start_countdown_mode(START_SESSION_COUNTDOWN_SECONDS, GameMode::Race)
            }
            WeekendPhase::Race => return Err("The race has begun"),
        }
        Ok(())
    }

    /// Timed practice begins: every human in the garage, the AI out.
    fn begin_practice(&mut self) {
        self.begin_day(crate::weekend::WeekendPhase::Practice);
        self.begin_phase_damage(crate::weekend::WeekendPhase::Practice);
        self.weekend.phase = crate::weekend::WeekendPhase::Practice;
        self.weekend.round = 0;
        self.set_game_mode(GameMode::Practice);
        self.send_ai_out();
        self.phase_started = true;
    }

    /// Qualifying round `round` (1-based) begins with an empty
    /// classification and its own clock: every human in the garage, the AI
    /// still taking part out, the knocked-out cars garaged for good. Q1
    /// sizes the knockout on the cars seated now.
    fn begin_round(&mut self, round: u8) {
        let rounds = self.session.weekend.as_ref().map_or(1, |w| w.rounds());
        if round == 1 {
            self.begin_day(crate::weekend::WeekendPhase::Qualifying);
            self.weekend.sizes =
                crate::weekend::knockout_sizes(self.session.participants.len(), rounds);
            self.weekend.eliminated.clear();
            self.weekend.final_order.clear();
            self.weekend.first_round.clear();
            self.weekend.last_round.clear();
            self.begin_phase_damage(crate::weekend::WeekendPhase::Qualifying);
        }
        self.weekend.phase = crate::weekend::WeekendPhase::Qualifying;
        self.weekend.round = round;
        self.weekend.round_closed = false;
        self.set_game_mode(GameMode::Qualification);
        self.garage_eliminated();
        self.send_ai_out();
        self.phase_started = true;
    }

    /// Every knocked-out car to its garage, AI too: it stays there.
    fn garage_eliminated(&mut self) {
        let out: Vec<PlayerId> = self
            .weekend
            .eliminated
            .iter()
            .map(|e| e.player_id)
            .collect();
        for player_id in out {
            let _ = self.hotlap_relocate(&player_id, HotlapDestination::Garage, false);
        }
    }

    /// Every AI car still taking part goes out of its garage onto its
    /// outlap, in seating order (the queue at the pit exit follows the
    /// participants' `BTreeMap` order).
    fn send_ai_out(&mut self) {
        // A shoot-out calls each car out in its turn.
        if self.shootout.is_some() {
            return;
        }
        let ai: Vec<PlayerId> = self
            .session
            .participants
            .keys()
            .filter(|id| self.session.ai_player_ids.contains(id))
            .filter(|id| self.weekend.eliminated_in(id).is_none())
            .copied()
            .collect();
        for player_id in ai {
            let _ = self.hotlap_relocate(&player_id, HotlapDestination::Track, false);
        }
    }

    /// Rank a weekend's qualifying round once it has finished: its cars by
    /// this round's best legal lap (no time behind every time, then seating
    /// order), the ones beyond the next round's size knocked out, or the
    /// final's order the front of the grid. Runs after the tick's laps are
    /// classified (`game_loop::tick`), and before any advance.
    pub fn settle_weekend(&mut self) {
        if self.session.weekend.is_none()
            || self.weekend.phase != crate::weekend::WeekendPhase::Qualifying
            || self.weekend.round_closed
            || self.session.state != SessionState::Finished
        {
            return;
        }
        let rounds = self.session.weekend.as_ref().map_or(1, |w| w.rounds());
        let taking_part: Vec<&CarState> = self
            .session
            .participants
            .values()
            .filter(|s| self.weekend.eliminated_in(&s.player_id).is_none())
            .collect();
        let mut ranking: Vec<(PlayerId, u32)> = self
            .qualifying_order()
            .into_iter()
            .filter(|id| taking_part.iter().any(|s| s.player_id == *id))
            .map(|id| (id, self.qualifying.get(&id).map_or(0, |(ms, _)| *ms)))
            .collect();
        let mut no_time: Vec<&CarState> = taking_part
            .into_iter()
            .filter(|s| !self.qualifying.contains_key(&s.player_id))
            .collect();
        no_time.sort_by_key(|s| (s.grid_position, s.player_id));
        ranking.extend(no_time.into_iter().map(|s| (s.player_id, 0)));
        self.weekend.close_round(&ranking, rounds);
        self.garage_eliminated();
        self.roster_dirty = true;
    }

    /// Whether this weekend waits for its host between phases: a practice
    /// or qualifying round has finished. Such a session is not removed by
    /// `[server] session_timeout_seconds` while a human is connected.
    pub fn awaits_weekend_advance(&self) -> bool {
        use crate::weekend::WeekendPhase;
        self.is_weekend()
            && matches!(
                self.weekend.phase,
                WeekendPhase::Practice | WeekendPhase::Qualifying
            )
            && self.session.state == SessionState::Finished
    }

    /// A weekend phase began since the last call (the game loop starts a
    /// replay for it).
    pub fn take_phase_started(&mut self) -> bool {
        std::mem::take(&mut self.phase_started)
    }

    /// The weekend's progress.
    pub fn weekend_progress(&self) -> &crate::weekend::WeekendProgress {
        &self.weekend
    }

    /// Where the weekend stands, for the wire; `None` outside a weekend.
    /// A knocked-out car that has left the session is left out.
    pub fn weekend_state(&self) -> Option<WeekendState> {
        use crate::weekend::WeekendPhase;
        let format = self.session.weekend.as_ref()?;
        let phase = self.weekend.phase;
        Some(WeekendState {
            phase: phase as u8,
            round: if phase == WeekendPhase::Qualifying {
                self.weekend.round
            } else {
                0
            },
            rounds: format.rounds() as u8,
            finished: phase != WeekendPhase::Lobby && self.session.state == SessionState::Finished,
            advancing: self.weekend.advancing().min(u8::MAX as usize) as u8,
            eliminated: self
                .weekend
                .eliminated
                .iter()
                .filter_map(|e| {
                    Some(KnockedOut {
                        car_index: self.car_index_of(&e.player_id)?,
                        round: e.round,
                        grid_position: e.grid_position,
                        lap_time_ms: e.lap_time_ms,
                        late: e.late,
                    })
                })
                .collect(),
            damage: self.session.damage,
            conditions: format
                .has_day_conditions()
                .then_some(self.session.conditions),
        })
    }

    /// The weekend's state when it changed since the last call (or was
    /// never handed out), for the game loop to send to the whole session.
    pub fn take_weekend_state(&mut self) -> Option<WeekendState> {
        let state = self.weekend_state()?;
        if self.weekend_sent.as_ref() == Some(&state) {
            return None;
        }
        self.weekend_sent = Some(state.clone());
        Some(state)
    }

    /// Race mode: full physics with lap counting, finish positions assigned
    /// as cars complete the race distance, session finished when all cars
    /// are classified.
    fn tick_racing(&mut self, inputs: &HashMap<PlayerId, PlayerInputData>) {
        self.fire_debug_events();
        self.update_air();
        let autopilot = self.pit_autopilot_inputs(inputs);
        let dt = self.dt(); // Fixed timestep derived from tick rate

        // A human who has finished is looking at the results and stops
        // sending input, while the last input received stays applied. The
        // server drives the car round instead.
        let cooldown_inputs: HashMap<PlayerId, PlayerInputData> = self
            .session
            .participants
            .values()
            .filter(|s| s.finish_position.is_some() && !self.is_ai_player(&s.player_id))
            .map(|s| (s.player_id, self.cooldown_input(&s.player_id)))
            .collect();

        // Update each car
        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        let current_tick = self.session.current_tick;
        let tick_rate_hz = self.tick_rate_hz;
        let clock = car_step::StepClock {
            tick: current_tick,
            tick_rate_hz,
        };
        let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();

        for state in states.iter_mut() {
            // Get input for this player (default to coasting if missing)
            let input = autopilot
                .get(&state.player_id)
                .or_else(|| cooldown_inputs.get(&state.player_id))
                .or_else(|| inputs.get(&state.player_id))
                .copied()
                .unwrap_or_default();

            // Get car config
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                // The car's own tick (`car_step`); a timing line crossed
                // here becomes a split on every driver's screen.
                let is_ai = ai_ids.contains(&state.player_id);
                let lap_traces = &mut self.lap_traces;
                let ghost = is_ai && !self.hotlap_watch.active;
                if let Some(event) =
                    car_step::step_car(state, config, &input, &self.track_config, clock, |moved| {
                        sample_ghost_trace(lap_traces, moved, ghost, current_tick, tick_rate_hz)
                    })
                {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai && !self.hotlap_watch.active,
                        event,
                    );
                }
            }
        }

        // Check collisions in place (BTreeMap iteration order makes the
        // order-dependent solver deterministic; no clone/rebuild needed)
        let mut state_refs: Vec<&mut CarState> = self.session.participants.values_mut().collect();
        physics::check_collisions_refs(&mut state_refs, &self.car_configs);
        physics::check_wall_collisions(&mut state_refs, &self.car_configs, &self.track_config, dt);
        self.update_flags();
        self.update_penalties();

        // A timed race whose clock just ran out learns its last lap; then
        // assign finish positions to cars that just took the flag, and
        // finish the session exactly once when every car is classified.
        self.update_race_clock();
        self.assign_finish_positions();
        if self.session.state != SessionState::Finished && self.is_race_complete() {
            self.session.state = SessionState::Finished;
        }
    }

    /// Race control's flags (`crate::race_control`) over this tick's field,
    /// each car told the yellow ahead of it and its blue flag. Clear before
    /// the green and once the race is over.
    fn update_flags(&mut self) {
        use crate::race_control::{Course, FlagCar, START_GRACE_S};
        let total = crate::laps::track_length_m(&self.track_config);
        if self.session.state != SessionState::Racing || total <= 0.0 {
            self.race_control.clear();
            for state in self.session.participants.values_mut() {
                state.flag_yellow = 0;
                state.flag_blue = false;
                state.speed_cap_mps = 0.0;
            }
            return;
        }
        let out_of_it = |s: &CarState| {
            s.in_garage
                || s.towed
                || s.pit.in_lane
                || s.finish_position.is_some()
                || !s.damage.is_drivable
                || s.is_ghost()
        };
        let launching = self.session.race_start_tick.is_some_and(|start| {
            (self.session.current_tick.saturating_sub(start) as f32)
                < START_GRACE_S * self.tick_rate_hz.max(1) as f32
        });
        let cars: Vec<FlagCar> = self
            .session
            .participants
            .values()
            .map(|s| FlagCar {
                laps: s.current_lap as f32 + s.track_progress / total,
                stranded: !launching
                    && s.is_on_track
                    && (s.speed_mps < crate::race_control::INCIDENT_SPEED_MPS
                        || !s.damage.is_drivable)
                    && !s.in_garage
                    && !s.towed
                    && !s.pit.in_lane
                    && s.finish_position.is_none()
                    && !s.is_ghost(),
                progress_m: s.track_progress,
                speed_mps: s.speed_mps,
                on_road: s.is_on_track,
                colliding: s.is_colliding,
                reversing: s.gear < 0 && s.speed_mps > 0.5,
                out_of_it: out_of_it(s),
                free: false,
            })
            .collect();
        let mut cars = cars;
        let dt = self.dt();
        let was = self.race_control.course;
        self.race_control
            .update_course(&cars, dt, self.session.race_rules.safety_car);
        if was == Course::Green && self.race_control.course == Course::Sc {
            // Called out: it drives the line planned for the first car of
            // the field.
            let car = self
                .session
                .participants
                .values()
                .next()
                .and_then(|s| self.car_configs.get(&s.car_config_id));
            self.race_control.safety_car = Some(crate::safety_car::SafetyCar::deploy(
                &self.track_config,
                car,
            ));
        }
        // The safety car on the road: it moves, and tells each car its pace.
        let mut sc_caps: Option<Vec<f32>> = None;
        let mut passed_sc: Vec<PlayerId> = Vec::new();
        let ramp = self.race_control.sc_ramp_mps();
        let road_clear = self.race_control.road_clear();
        let stranded_long = self.race_control.stranded_long();
        if let Some(sc) = self.race_control.safety_car.as_mut() {
            let queue: Vec<crate::safety_car::QueueCar> = self
                .session
                .participants
                .values()
                .map(|s| crate::safety_car::QueueCar {
                    laps: s.current_lap as f32 + s.track_progress / total,
                    progress_m: s.track_progress,
                    speed_mps: s.speed_mps,
                    running: !(s.in_garage
                        || s.towed
                        || (s.pit.in_lane && (s.pit.limiter || s.pit.servicing))
                        || s.finish_position.is_some()
                        || !s.damage.is_drivable
                        || s.is_ghost()),
                    moving: s.is_on_track,
                })
                .collect();
            let (orders, event) = sc.tick(
                &queue,
                &self.track_config,
                dt,
                ramp,
                road_clear,
                stranded_long,
            );
            for (car, free) in cars.iter_mut().zip(&orders.let_by) {
                car.free = *free;
            }
            let ids: Vec<PlayerId> = self.session.participants.keys().copied().collect();
            passed_sc = orders
                .passed_sc
                .iter()
                .filter_map(|i| ids.get(*i).copied())
                .collect();
            use crate::safety_car::ScEvent;
            match event {
                Some(ScEvent::Green) => {
                    self.race_control.set_course(Course::Green);
                    self.race_control.safety_car = None;
                }
                Some(ScEvent::InThisLap) => {
                    self.race_control.set_course(Course::ScInThisLap);
                    sc_caps = Some(orders.caps);
                }
                // Back out before it went in: the pace stays where it is.
                Some(ScEvent::BackOut) => {
                    self.race_control.course = Course::Sc;
                    sc_caps = Some(orders.caps);
                }
                None => sc_caps = Some(orders.caps),
            }
        }
        if self.race_control.course != was {
            info!(
                "Session {}: {:?}",
                self.session.id, self.race_control.course
            );
        }
        self.race_control
            .update(&cars, total, dt, self.session.current_tick, launching);
        for id in passed_sc {
            info!("Session {}: a car passed the safety car", self.session.id);
            self.penalise(id, crate::race_control::Offence::YellowPass);
        }
        let control = &self.race_control;
        let pace = control.vsc_pace_mps();
        for (i, state) in self.session.participants.values_mut().enumerate() {
            // The virtual safety car's pace, on every car on the track; the
            // safety car's queue, each car its own.
            state.speed_cap_mps = if let Some(caps) = sc_caps.as_ref() {
                caps.get(i).copied().unwrap_or(0.0)
            } else if out_of_it(state) {
                0.0
            } else {
                pace
            };
            state.flag_blue = control.blue(i);
            state.flag_yellow = if out_of_it(state) {
                0
            } else {
                control.yellow_ahead_for(i, state.track_progress, total) as u8
            };
        }
    }

    /// Race control's penalties this tick (`crate::race_control`): the jump
    /// starts the green found, the passes under a yellow the flags saw, and
    /// a drive-through or stop-go gone unserved too long added as time.
    /// Only the offences the session's rules name are penalised.
    fn update_penalties(&mut self) {
        use crate::race_control::Offence;
        let passes = self.race_control.take_yellow_passes();
        if self.session.state != SessionState::Racing {
            return;
        }
        let jumped = std::mem::take(&mut self.race_start.jumped);
        for id in jumped {
            self.penalise(id, Offence::JumpStart);
        }
        if !passes.is_empty() {
            let ids: Vec<PlayerId> = self.session.participants.keys().copied().collect();
            for pass in passes {
                let (Some(&passer), Some(&passed)) = (ids.get(pass.passer), ids.get(pass.passed))
                else {
                    continue;
                };
                self.yellow_passed
                    .entry((passer, passed))
                    .or_insert(pass.tick);
            }
        }
        // Each pass waits for the place to be handed back.
        if !self.yellow_passed.is_empty() {
            let total = crate::laps::track_length_m(&self.track_config);
            let handback_ticks =
                (crate::race_control::YELLOW_PASS_HANDBACK_S * self.tick_rate_hz as f32) as u32;
            let now = self.session.current_tick;
            let participants = &self.session.participants;
            let mut judged = Vec::new();
            self.yellow_passed.retain(|&(passer, passed), &mut tick| {
                let (Some(a), Some(b)) = (participants.get(&passer), participants.get(&passed))
                else {
                    return false;
                };
                if laps_run(b, total) > laps_run(a, total) {
                    return false;
                }
                if now.saturating_sub(tick) < handback_ticks {
                    return true;
                }
                judged.push(passer);
                false
            });
            for passer in judged {
                self.penalise(passer, Offence::YellowPass);
            }
        }
        // Too late to serve: added as time.
        let mut changed = false;
        for (id, list) in self.penalties.iter_mut() {
            let lap = self
                .session
                .participants
                .get(id)
                .map_or(0, |s| s.current_lap);
            for penalty in list.iter_mut().filter(|p| p.overdue(lap)) {
                penalty.convert();
                changed = true;
            }
        }
        if changed {
            self.roster_dirty = true;
        }
    }

    /// Give `player_id` the penalty for `offence`, if the session's rules
    /// penalise it: one to serve in the pit lane is added as time at once
    /// where it cannot be served (no pit lane, or too few laps left).
    pub fn penalise(&mut self, player_id: PlayerId, offence: crate::race_control::Offence) {
        if !self.session.race_rules.penalises(offence) {
            return;
        }
        let Some(state) = self.session.participants.get(&player_id) else {
            return;
        };
        if state.finish_position.is_some() {
            return;
        }
        let can_serve = self.track_config.pit_lane.is_some()
            && self.laps_left(state) >= crate::race_control::SERVE_WITHIN_LAPS as u32;
        let penalty = crate::race_control::Penalty::issue(
            offence,
            state.current_lap,
            self.session.current_tick,
            can_serve,
        );
        info!(
            "Session {}: car on slot {} penalised for {:?}: {:?} {} s",
            self.session.id, state.grid_position, offence, penalty.kind, penalty.seconds
        );
        self.penalties.entry(player_id).or_default().push(penalty);
        self.roster_dirty = true;
    }

    /// The first drive-through or stop-go `player_id` still has to serve.
    pub fn owed_penalty(&self, player_id: &PlayerId) -> Option<crate::race_control::PenaltyKind> {
        self.penalties
            .get(player_id)
            .and_then(|list| crate::race_control::owed(list))
            .map(|p| p.kind)
    }

    /// The first owed penalty of `kind` is served.
    fn serve_penalty(&mut self, player_id: &PlayerId, kind: crate::race_control::PenaltyKind) {
        use crate::race_control::PenaltyStatus;
        let Some(penalty) = self.penalties.get_mut(player_id).and_then(|list| {
            list.iter_mut()
                .find(|p| p.status == PenaltyStatus::Owed && p.kind == kind)
        }) else {
            return;
        };
        penalty.status = PenaltyStatus::Served;
        self.roster_dirty = true;
        info!("Session {}: {:?} served", self.session.id, kind);
    }

    /// The seconds `player_id`'s penalties add to its race time.
    pub fn penalty_seconds(&self, player_id: &PlayerId) -> u32 {
        self.penalties
            .get(player_id)
            .map_or(0, |list| list.iter().map(|p| p.added_s()).sum())
    }

    /// `StartSession`: count a session still in its lobby down into a race,
    /// [`START_SESSION_COUNTDOWN_SECONDS`] long, as `StartCountdown` with
    /// `next_mode: Race` does. Returns false (and changes nothing) for a
    /// session already under way.
    pub fn start_countdown(&mut self) -> bool {
        if self.session.state != SessionState::Lobby {
            return false;
        }
        self.start_countdown_mode(START_SESSION_COUNTDOWN_SECONDS, GameMode::Race);
        true
    }

    /// `OpenGrid`: the race's grid goes up (`GameMode::Grid`), from any
    /// state, as a countdown is taken from any: every car on its slot as
    /// for the start (`line_up_on_grid`), nobody ready yet. When everyone
    /// is, `countdown_seconds` (capped at [`MAX_COUNTDOWN_SECONDS`]) count
    /// into the race.
    pub fn open_grid(&mut self, countdown_seconds: u16) {
        self.enter_weekend_race();
        self.grid_countdown_seconds = countdown_seconds.min(MAX_COUNTDOWN_SECONDS);
        self.set_game_mode(GameMode::Grid);
    }

    /// A driver on the grid says they are ready, or takes it back. False
    /// (and nothing changes) outside `GameMode::Grid` or for anyone not
    /// driving a car here.
    pub fn set_ready(&mut self, player_id: PlayerId, ready: bool) -> bool {
        if self.session.game_mode != GameMode::Grid
            || self.is_ai_player(&player_id)
            || !self.session.participants.contains_key(&player_id)
        {
            return false;
        }
        let changed = if ready {
            self.ready.insert(player_id)
        } else {
            self.ready.remove(&player_id)
        };
        if changed {
            self.roster_dirty = true;
        }
        true
    }

    /// Whether this driver said they are ready on the grid.
    pub fn is_ready(&self, player_id: &PlayerId) -> bool {
        self.session.game_mode == GameMode::Grid && self.ready.contains(player_id)
    }

    /// Why a human may not join this session as a driver now, or `None`
    /// when they may. A running practice, hotlap or qualifying session takes
    /// drivers (a hotlap or qualifying driver arrives in the garage); a race
    /// takes them until the green light, and a finished session none. A
    /// race weekend takes none once Q1 has begun. A spectator
    /// (`JoinAsSpectator`) is never refused for this.
    pub fn refuses_drivers(&self) -> Option<&'static str> {
        // A weekend waiting for its host between phases takes drivers: the
        // next phase is theirs (after Q1 began, from the back of the grid).
        if self.session.state == SessionState::Finished && !self.awaits_weekend_advance() {
            return Some("Session has finished");
        }
        if self.session.game_mode == GameMode::Race {
            return Some("A race is under way: watch it instead");
        }
        if self.race_start.formation.is_some() {
            return Some("The formation lap is under way: watch it instead");
        }
        if self.is_weekend()
            && self.weekend.phase == crate::weekend::WeekendPhase::Qualifying
            && self.session.state != SessionState::Finished
        {
            return Some("A qualifying round is running: join after it or watch");
        }
        None
    }

    /// The session's state when it changed since the last call, for the
    /// lobby's listing (`LobbyManager::set_session_state`).
    pub fn take_state_change(&mut self) -> Option<SessionState> {
        if self.session.state == self.listed_state {
            return None;
        }
        self.listed_state = self.session.state;
        Some(self.session.state)
    }

    /// Set the game mode
    pub fn set_game_mode(&mut self, mode: GameMode) {
        self.session.game_mode = mode;
        // A formation lap belongs to the countdown it was driven in: a mode
        // set over it ends it, and its pace.
        if mode != GameMode::Countdown && self.race_start.formation.take().is_some() {
            for state in self.session.participants.values_mut() {
                state.speed_cap_mps = 0.0;
            }
        }
        // A shoot-out and the lap allowance's count belong to the
        // qualifying they were run in.
        self.shootout = None;
        if !self.qualifying_laps_run.is_empty() {
            self.qualifying_laps_run.clear();
            self.roster_dirty = true;
        }

        // Initialize mode-specific state
        match mode {
            GameMode::DemoLap => {
                debug!(
                    "[DemoLap] Setting demo lap mode. Participants: {}, AI profiles: {}",
                    self.session.participants.len(),
                    self.ai_profiles.len()
                );

                self.session.demo_lap_progress = Some(0.0);
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;

                // Remove human players from participants (they become spectators)
                // Only AI drivers should be in participants for DemoLap
                let human_player_ids: Vec<PlayerId> = self
                    .session
                    .participants
                    .keys()
                    .filter(|id| !self.session.ai_player_ids.contains(id))
                    .cloned()
                    .collect();
                for player_id in &human_player_ids {
                    self.session.participants.remove(player_id);
                    self.roster_dirty = true;
                    debug!(
                        "[DemoLap] Removed human player {} from participants (now spectator)",
                        player_id
                    );
                }

                // Ensure we have an AI driver for demo lap
                if self.session.ai_player_ids.is_empty() {
                    if !self.ai_profiles.is_empty() {
                        debug!("[DemoLap] Spawning AI from existing profiles");
                        // Temporarily increment ai_count and max_players to allow AI spawn
                        let original_ai_count = self.session.ai_count;
                        let original_max = self.session.max_players;
                        self.session.ai_count = 1;
                        self.session.max_players = 1;
                        self.spawn_ai_drivers();
                        self.session.ai_count = original_ai_count;
                        self.session.max_players = original_max;
                    } else {
                        // No AI profiles configured, create a default demo driver
                        use crate::ai_driver::AiDriverProfile;

                        debug!(
                            "[DemoLap] Creating default demo driver. Host car: {:?}",
                            self.session.host_car_id
                        );

                        let mut demo_profile = AiDriverProfile::new("Demo Driver", 95);
                        demo_profile.preferred_car_id = self.session.host_car_id;

                        let demo_player_id = demo_profile.id;
                        self.ai_profiles.insert(demo_player_id, demo_profile);

                        // Temporarily increment ai_count and max_players to allow AI spawn
                        let original_ai_count = self.session.ai_count;
                        let original_max = self.session.max_players;
                        self.session.ai_count = 1;
                        self.session.max_players = 1;
                        self.spawn_ai_drivers();
                        self.session.ai_count = original_ai_count;
                        self.session.max_players = original_max;

                        debug!(
                            "[DemoLap] After spawn: Participants: {}, AI IDs: {}",
                            self.session.participants.len(),
                            self.session.ai_player_ids.len()
                        );
                    }
                } else {
                    debug!(
                        "[DemoLap] Already have {} AI drivers",
                        self.session.ai_player_ids.len()
                    );
                }
            }
            GameMode::FreePractice => {
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;
                self.refuel_all(GameMode::FreePractice);
            }
            GameMode::Sandbox => {
                // Change session state to Racing so telemetry is sent
                self.session.state = SessionState::Racing;
            }
            GameMode::Countdown => {
                // Default 10 second countdown as per spec
                self.session.countdown_ticks_remaining = Some(self.tick_rate_hz * 10);
                self.session.state = SessionState::Countdown;
            }
            GameMode::Grid => {
                // The field lines up as for the start. The state is the
                // countdown one, so the cars are sent and drivers may still
                // join, but no clock runs until everyone is ready.
                self.line_up_on_grid();
                self.session.state = SessionState::Countdown;
                self.session.countdown_ticks_remaining = None;
                self.session.next_mode = Some(GameMode::Race);
                self.ready.clear();
                self.roster_dirty = true;
            }
            GameMode::Race => {
                // The race starts now: mark the session racing and remember
                // the start tick for race-time bookkeeping.
                self.session.state = SessionState::Racing;
                self.session.race_start_tick = Some(self.session.current_tick);
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
                self.race_end = RaceEnd::default();
                self.line_drying = LineDrying::default();
                // Pole sits on the line, so it never crosses it to start lap
                // 1: its lap starts with the green light.
                let tick = self.session.current_tick;
                for state in self.session.participants.values_mut() {
                    physics::start_lap_on_green(state, &self.track_config, tick);
                    // The pit rule counts from the green light: a car on a
                    // treaded tyre has its second compound already.
                    state.pit.stops = 0;
                    state.pit.compound_changed =
                        self.car_configs.get(&state.car_config_id).is_some_and(|c| {
                            c.tire_config.compound(state.tyre_compound).kind
                                != tyre_thermal::CompoundKind::Slick
                        });
                    // The grid waits in neutral; the automatic box takes
                    // first with the green light, never before.
                    if state.auto_gearbox && state.gear == 0 {
                        state.gear = 1;
                        state.auto_shift_hold_ticks = 0;
                    }
                }
            }
            GameMode::Qualification | GameMode::Hotlap | GameMode::Practice => {
                // Every driver starts in the garage, with the setup screen;
                // the AI (if any) is left where it stands and drives on.
                self.session.state = SessionState::Racing;
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
                if mode.has_outlap() {
                    // A timed session's clock starts now; entering
                    // qualifying again is a fresh session with a fresh
                    // classification.
                    self.timed_start_tick = Some(self.session.current_tick);
                    if mode == GameMode::Qualification && self.timed_session_seconds().is_some() {
                        self.qualifying.clear();
                    }
                    if self.timed_session_seconds().is_some() {
                        self.clear_timing_sheets();
                    }
                }
                let humans: Vec<PlayerId> = self
                    .session
                    .participants
                    .keys()
                    .filter(|id| !self.session.ai_player_ids.contains(id))
                    .copied()
                    .collect();
                for player_id in humans {
                    let _ = self.hotlap_relocate(&player_id, HotlapDestination::Garage, false);
                }
                if self.hotlap_watch.active {
                    self.send_hotlap_watch_out();
                }
                if self.is_shootout_round() {
                    self.begin_shootout();
                }
            }
            _ => {
                self.session.demo_lap_progress = None;
            }
        }
    }

    // --- A watched hotlap ------------------------------------------------

    /// Make this session a watched hotlap (`SessionKind::HotlapWatch`): its
    /// AI car goes onto the run-up on tyres at their optimum and, after
    /// [`HOTLAP_WATCH_HOLD_S`], laps the track alone for as long as anyone
    /// watches. Nobody presses start, so the hotlap begins now.
    pub fn start_hotlap_watch(&mut self) {
        self.hotlap_watch.active = true;
        self.set_game_mode(GameMode::Hotlap);
    }

    /// Whether this session is a watched hotlap.
    pub fn is_hotlap_watch(&self) -> bool {
        self.hotlap_watch.active
    }

    /// How far the watched driver has been eased off the exact line (0: not
    /// at all); see [`hotlap_watch_driver`].
    pub fn hotlap_watch_level(&self) -> u8 {
        self.hotlap_watch.level
    }

    /// What a watched hotlap's ladder level is kept under between watches:
    /// the track, the car that laps, the weather, the setup ([`setup_key`])
    /// and the rest of the sky ([`sky_key`]: hour, air, wind). The ghost is
    /// kept under the first two.
    pub fn hotlap_watch_key(&self) -> Option<(TrackConfigId, CarConfigId, u8, u64, u64)> {
        let car = self
            .session
            .ai_player_ids
            .first()
            .and_then(|id| self.session.participants.get(id))?
            .car_config_id;
        Some((
            self.session.track_config_id,
            car,
            self.session.conditions.weather as u8,
            self.hotlap_watch.setup_key,
            sky_key(&self.session.conditions),
        ))
    }

    /// The watched car runs `setup` (clamped as `SetCarSetup` is): its
    /// tuned config, its pressures on the next fit, its tank.
    pub fn set_hotlap_watch_setup(&mut self, setup: CarSetup) {
        let ids: Vec<PlayerId> = self.session.ai_player_ids.clone();
        let mut applied = setup.clamp();
        for id in ids {
            if let Some(set) = self.set_car_setup(&id, setup) {
                applied = set;
            }
            if let Some(state) = self.session.participants.get_mut(&id) {
                fit_tyres(
                    state,
                    &self.car_configs,
                    &self.tuned_configs,
                    &self.track_config,
                    TyreStart::Warm,
                    &self.car_setups,
                );
            }
        }
        self.hotlap_watch.setup_key = setup_key(&applied);
    }

    /// Start with `ghost` as the lap to beat (an earlier watch's best).
    pub fn set_hotlap_watch_ghost(&mut self, ghost: GhostLap) {
        self.hotlap_watch.ghost = Some(ghost);
        self.hotlap_watch.ghost_revision = self.hotlap_watch.ghost_revision.wrapping_add(1);
    }

    /// The watched hotlap's ghost and its revision (see [`HotlapWatch`]).
    pub fn hotlap_watch_ghost(&self) -> Option<(u32, &GhostLap)> {
        self.hotlap_watch
            .ghost
            .as_ref()
            .map(|g| (self.hotlap_watch.ghost_revision, g))
    }

    /// Start the watched driver `level` steps down the ladder, as an earlier
    /// watch of the same lap left it.
    pub fn set_hotlap_watch_level(&mut self, level: u8) {
        self.hotlap_watch.level = 0;
        self.hotlap_watch.clean_laps = 0;
        for profile in self.ai_profiles.values_mut() {
            *profile = hotlap_watch_driver(profile, 0);
        }
        for _ in 0..level {
            self.ease_hotlap_watch_driver();
        }
    }

    /// Give the watcher's HUD the track's corners.
    pub fn set_track_corners(&mut self, corners: Vec<TrackCorner>) {
        self.track_corners = corners;
    }

    /// The track's corners, as `set_track_corners` gave them.
    pub fn track_corners(&self) -> &[TrackCorner] {
        &self.track_corners
    }

    /// The AI's inputs while the car is held: the brake on, nothing else.
    fn hotlap_watch_hold(
        &self,
        inputs: &HashMap<PlayerId, PlayerInputData>,
    ) -> HashMap<PlayerId, PlayerInputData> {
        let mut held = inputs.clone();
        for id in &self.session.ai_player_ids {
            held.insert(
                *id,
                PlayerInputData {
                    brake: 1.0,
                    ..Default::default()
                },
            );
        }
        held
    }

    /// Put every AI car on the run-up, fresh, and hold it there.
    fn send_hotlap_watch_out(&mut self) {
        let ids: Vec<PlayerId> = self.session.ai_player_ids.clone();
        for id in ids {
            let _ = self.hotlap_relocate(&id, HotlapDestination::Track, false);
            // The lap before the trip is not this one's to answer for.
            if let Some(state) = self.session.participants.get_mut(&id) {
                state.laps.last_invalid = false;
            }
        }
        self.hotlap_watch.hold_until_tick =
            self.session.current_tick + (HOTLAP_WATCH_HOLD_S * self.tick_rate_hz as f32) as u32;
        self.hotlap_watch.stuck_ticks = 0;
        self.hotlap_watch.last_lap = None;
        self.hotlap_watch.rolled = false;
    }

    /// Let a held car go at the speed the AI's plan has for its place on the
    /// run-up: a hotlap's first timed lap starts at the line at speed, as
    /// every later one does, and not from a standstill 300 m before it.
    fn roll_hotlap_watch_car(&mut self) {
        let ids: Vec<PlayerId> = self.session.ai_player_ids.clone();
        for id in ids {
            let Some(state) = self.session.participants.get(&id) else {
                continue;
            };
            let Some(profile) = self.ai_speed_profile(state) else {
                continue;
            };
            if profile.speed_mps.is_empty() || profile.spacing_m <= 0.0 {
                continue;
            }
            // The profile runs along the racing line, not the centerline the
            // car's progress is measured on (at the Nordschleife 135 m
            // shorter): the car's place on it is found as the AI finds its
            // own, from the progress scaled to the line's length.
            let line_m = profile.spacing_m * profile.speed_mps.len() as f32;
            let lap_m = self.track_length_m();
            let guess = if lap_m > 0.0 {
                state.track_progress * line_m / lap_m
            } else {
                state.track_progress
            };
            let Some(at) = crate::ai_driver::profile_point(state, profile, guess) else {
                continue;
            };
            let speed = profile.speed_mps[at] * HOTLAP_WATCH_ROLL_FRAC;
            let Some(config) = self.car_configs.get(&state.car_config_id) else {
                continue;
            };
            let omega_rpm = speed / config.wheel_radius_m.max(0.1) * 60.0 / std::f32::consts::TAU;
            // The highest gear that keeps the engine under 85% of the
            // redline at this speed; the box shifts from there.
            let mut gear = 1;
            for (g, ratio) in config.gear_ratios.iter().enumerate().skip(1) {
                if omega_rpm * ratio.abs() * config.final_drive_ratio <= config.redline_rpm * 0.85 {
                    gear = g as i8;
                }
            }
            let (sin, cos) = state.yaw_rad.sin_cos();
            let omega = speed / config.wheel_radius_m.max(0.1);
            if let Some(state) = self.session.participants.get_mut(&id) {
                state.vel_x = speed * cos;
                state.vel_y = speed * sin;
                state.speed_mps = speed;
                state.wheel_angular_vel = [omega; 4];
                state.gear = gear;
            }
        }
    }

    /// After each tick of a watched hotlap: a car that has just crossed the
    /// line starts its next lap on a fresh tank and tyres (a long watch
    /// would otherwise watch the car wear out, and the lap times drift); a
    /// lap that went wrong - a crash, a car that has stopped for good, a lap
    /// struck for leaving the track - eases the driver a step down the
    /// ladder ([`hotlap_watch_driver`]), and a crash or a stop puts the car
    /// back on the run-up.
    fn update_hotlap_watch(&mut self) {
        if self.session.game_mode != GameMode::Hotlap {
            return;
        }
        // A legal lap faster than the ghost is the ghost now. (The events
        // wait for the loop to take them; a slower or equal one changes
        // nothing, so a second look at the same event does not either.)
        let faster = self
            .lap_events
            .iter()
            .filter_map(|e| e.ghost.as_ref())
            .filter(|g| {
                self.hotlap_watch
                    .ghost
                    .as_ref()
                    .is_none_or(|kept| g.lap_time_ms < kept.lap_time_ms)
            })
            .min_by_key(|g| g.lap_time_ms)
            .cloned();
        if let Some(ghost) = faster {
            self.set_hotlap_watch_ghost(ghost);
        }
        let ids: Vec<PlayerId> = self.session.ai_player_ids.clone();
        let stuck_limit = (HOTLAP_WATCH_STUCK_S * self.tick_rate_hz as f32) as u32;
        let held = self.session.current_tick < self.hotlap_watch.hold_until_tick;
        if !held && !self.hotlap_watch.rolled {
            self.hotlap_watch.rolled = true;
            self.roll_hotlap_watch_car();
        }
        let mut crashed = false;
        let mut struck = false;
        let mut clean_lap = false;
        for id in ids {
            let Some(state) = self.session.participants.get(&id) else {
                continue;
            };
            let car_id = state.car_config_id;
            let crossed_line = self
                .hotlap_watch
                .last_lap
                .is_some_and(|lap| lap != state.current_lap);
            self.hotlap_watch.last_lap = Some(state.current_lap);
            let d = &state.damage;
            let hurt = [
                d.front_damage_percent,
                d.rear_damage_percent,
                d.left_damage_percent,
                d.right_damage_percent,
            ]
            .into_iter()
            .fold(0.0, f32::max)
                >= HOTLAP_WATCH_CRASH_PCT;
            if !d.is_drivable || state.towed || hurt {
                crashed = true;
                continue;
            }
            if !held && state.speed_mps < HOTLAP_WATCH_STUCK_MPS {
                self.hotlap_watch.stuck_ticks += 1;
                if self.hotlap_watch.stuck_ticks >= stuck_limit {
                    crashed = true;
                    continue;
                }
            } else {
                self.hotlap_watch.stuck_ticks = 0;
            }
            if crossed_line {
                struck |= state.laps.last_invalid;
                // A timed lap closed (the first crossing ends the run-up).
                clean_lap |= !state.laps.last_invalid && state.current_lap >= 2;
                let fuel = self.start_fuel_liters(&id, car_id, GameMode::Hotlap);
                if let Some(state) = self.session.participants.get_mut(&id) {
                    state.fuel_liters = fuel;
                    state.damage = DamageState {
                        is_drivable: true,
                        ..Default::default()
                    };
                    fit_tyres(
                        state,
                        &self.car_configs,
                        &self.tuned_configs,
                        &self.track_config,
                        TyreStart::Warm,
                        &self.car_setups,
                    );
                }
            }
        }
        if crashed || struck {
            self.ease_hotlap_watch_driver();
        } else if clean_lap {
            self.hotlap_watch.clean_laps = self.hotlap_watch.clean_laps.saturating_add(1);
            if self.hotlap_watch.clean_laps >= HOTLAP_WATCH_CLIMB_LAPS
                && self.hotlap_watch.level > 0
            {
                self.set_hotlap_watch_level(self.hotlap_watch.level - 1);
            }
        }
        if crashed {
            self.send_hotlap_watch_out();
        }
    }

    /// One step down the ladder for the watched driver.
    fn ease_hotlap_watch_driver(&mut self) {
        self.hotlap_watch.clean_laps = 0;
        let level = self.hotlap_watch.level.saturating_add(1);
        let floor_level =
            2 + (HOTLAP_WATCH_SKILL - HOTLAP_WATCH_FLOOR_SKILL).div_ceil(HOTLAP_WATCH_SKILL_STEP);
        self.hotlap_watch.level = level.min(floor_level);
        for profile in self.ai_profiles.values_mut() {
            *profile = hotlap_watch_driver(profile, self.hotlap_watch.level);
        }
    }

    /// Move a hotlap driver's car: into the garage (parked on its grid slot,
    /// frozen, out of the collision passes) or out onto the run-up before
    /// the line, on the first free slot of a queue spaced [`HOTLAP_SPACING_M`]
    /// apart, so several drivers can go out together. The car comes back
    /// fresh — no damage, no half-driven lap — but keeps its aids and its
    /// bests. The lap starts, timed, as the car crosses the line. The
    /// tyres go on at the compound's optimum, so a hotlap measures the car
    /// and not its warm-up, unless the driver asks for `cold_tyres`: then
    /// as out of the garage (blankets or the air, cold brakes), and the
    /// first lap is the warm-up.
    pub fn hotlap_relocate(
        &mut self,
        player_id: &PlayerId,
        destination: HotlapDestination,
        cold_tyres: bool,
    ) -> Result<(), &'static str> {
        let mode = self.session.game_mode;
        if !mode.uses_garages() {
            return Err("Not a hotlap or qualifying session");
        }
        if destination == HotlapDestination::Track {
            if self.chequered_flag_out() {
                return Err(if mode == GameMode::Practice {
                    "Practice is over"
                } else {
                    "Qualifying is over"
                });
            }
            if mode == GameMode::Qualification {
                if let Some(entry) = self.weekend.eliminated_entry(player_id) {
                    return Err(crate::weekend::eliminated_reason(entry));
                }
                if self.shootout.is_some() {
                    return Err("A shoot-out: each car is sent out in its turn");
                }
                let allowance = self.session.qualifying_lap_allowance;
                if allowance > 0
                    && self
                        .qualifying_laps_run
                        .get(player_id)
                        .is_some_and(|n| *n >= allowance)
                {
                    return Err("Lap allowance used");
                }
            }
        }
        self.relocate(player_id, destination, cold_tyres)
    }

    /// Put a car in its garage or out on the track as `hotlap_relocate`
    /// does, without asking whether it may go: the session's own moves (a
    /// shoot-out's runs, the lap allowance).
    fn relocate(
        &mut self,
        player_id: &PlayerId,
        destination: HotlapDestination,
        cold_tyres: bool,
    ) -> Result<(), &'static str> {
        let mode = self.session.game_mode;
        if !mode.uses_garages() {
            return Err("Not a hotlap or qualifying session");
        }
        let Some(state) = self.session.participants.get(player_id) else {
            return Err("No car in this session");
        };
        // A qualifying car going out leaves from its box down the pit lane
        // when the lane lets out past the line.
        let from_box = (destination == HotlapDestination::Track && mode.has_outlap())
            .then(|| self.outlap_box_pose(state))
            .flatten();
        let pose = match (destination, from_box) {
            (_, Some(pose)) => pose,
            (HotlapDestination::Garage, None) => {
                let slot = self
                    .track_config
                    .start_positions
                    .iter()
                    .find(|s| s.position == state.grid_position)
                    .or_else(|| self.track_config.start_positions.first())
                    .ok_or("Track has no grid")?;
                (slot.x, slot.y, slot.z, slot.yaw_rad)
            }
            (HotlapDestination::Track, None) if mode.has_outlap() => self
                .qualifying_outlap_pose(player_id)
                .ok_or("Track has no centerline")?,
            (HotlapDestination::Track, None) => self
                .hotlap_runup_pose(player_id)
                .ok_or("Track has no centerline")?,
        };
        let in_garage = destination == HotlapDestination::Garage;
        // The garage fills the car to the driver's fuel knob, for the next
        // run or for this one.
        let car_id = state.car_config_id;
        let fuel = self.start_fuel_liters(player_id, car_id, mode);
        let state = self
            .session
            .participants
            .get_mut(player_id)
            .ok_or("No car in this session")?;
        let slot = GridSlot {
            position: state.grid_position,
            x: pose.0,
            y: pose.1,
            z: pose.2,
            yaw_rad: pose.3,
        };
        let mut fresh = CarState::new(state.player_id, state.car_config_id, &slot);
        fresh.auto_gearbox = state.auto_gearbox;
        fresh.abs = state.abs;
        fresh.traction_control = state.traction_control;
        fresh.damage_level = state.damage_level;
        fresh.steering_assist = state.steering_assist;
        fresh.manual_pits = state.manual_pits;
        fresh.pit.box_index = state.pit.box_index;
        // The timing sheet survives the trip: the panel and the delta are
        // measured against what the driver did before going in.
        fresh.best_lap_time_ms = state.best_lap_time_ms;
        fresh.last_lap_time_ms = state.last_lap_time_ms;
        fresh.laps.last_invalid = state.laps.last_invalid;
        fresh.laps.last_splits_ms = state.laps.last_splits_ms;
        fresh.laps.best_lap_ms = state.laps.best_lap_ms;
        fresh.laps.best_lap_splits_ms = state.laps.best_lap_splits_ms;
        fresh.laps.best_splits_ms = state.laps.best_splits_ms;
        fresh.in_garage = in_garage;
        // A qualifying car put on the track has its outlap to drive first.
        fresh.outlap = !in_garage && mode.has_outlap();
        fresh.fuel_liters = fuel;
        fit_tyres(
            &mut fresh,
            &self.car_configs,
            &self.tuned_configs,
            &self.track_config,
            // The outlap is a qualifying car's warm-up: it leaves cold.
            if cold_tyres || mode.has_outlap() {
                TyreStart::Garage
            } else {
                TyreStart::Warm
            },
            &self.car_setups,
        );
        // Parked cars wait in neutral; a car put on the run-up is in first.
        fresh.gear = if in_garage { 0 } else { 1 };
        // Out of the box: served already, the pit autopilot drives it to
        // the exit line and hands it back, unless the driver drives the
        // lane themselves.
        if from_box.is_some() {
            fresh.pit.serviced = true;
            if !fresh.manual_pits {
                fresh.pit.driving = true;
                fresh.pit.restore_aids = Some([fresh.auto_gearbox, fresh.steering_assist]);
                fresh.auto_gearbox = true;
                fresh.steering_assist = false;
            }
        }
        physics::seed_track_progress(&mut fresh, &self.track_config);
        *state = fresh;
        // The lap the car was on is abandoned with it.
        self.lap_traces.remove(player_id);
        Ok(())
    }

    /// A driver asks for their car back (`ClientMessage::RecoverCar`,
    /// `crate::recovery`): onto the track where it is, or to its pit box,
    /// held there for the recovery's time cost. Refused while the car is
    /// moving faster than `recovery::MAX_SPEED_MPS`, out of the race, in
    /// the garage or on the pit route, already recovering, before the
    /// start, or after the flag. In a hotlap or qualifying session the pits
    /// are the garage (`hotlap_relocate`).
    pub fn recover_car(
        &mut self,
        player_id: &PlayerId,
        destination: crate::recovery::RecoverDestination,
    ) -> Result<(), &'static str> {
        self.recover_car_back(player_id, destination, 0.0)
    }

    /// [`Self::recover_car`], put back onto the track `back_m` behind
    /// where the car is along the lap.
    fn recover_car_back(
        &mut self,
        player_id: &PlayerId,
        destination: crate::recovery::RecoverDestination,
        back_m: f32,
    ) -> Result<(), &'static str> {
        use crate::recovery::{RecoverDestination, Recovery};
        let mode = self.session.game_mode;
        let garage_mode = mode.uses_garages();
        match mode {
            GameMode::FreePractice
            | GameMode::Hotlap
            | GameMode::Qualification
            | GameMode::Practice => {}
            GameMode::Race if self.session.state == SessionState::Racing => {}
            _ => return Err("No recovery outside a running session"),
        }
        let state = self
            .session
            .participants
            .get(player_id)
            .ok_or("No car in this session")?;
        if state.in_garage {
            return Err("The car is in the garage");
        }
        if !state.damage.is_drivable || state.towed {
            return Err("The car is out");
        }
        if state.finish_position.is_some() {
            return Err("The car has finished");
        }
        if state.recovery.is_some() {
            return Err("Already recovering");
        }
        if state.pit.driving || state.pit.servicing {
            return Err("The car is on the pit route");
        }
        if state.speed_mps > crate::recovery::MAX_SPEED_MPS {
            return Err("Too fast to recover");
        }
        if destination == RecoverDestination::Pits && garage_mode {
            return self.hotlap_relocate(player_id, HotlapDestination::Garage, false);
        }
        let track = &self.track_config;
        if track.centerline.len() < 2 {
            return Err("Track has no centerline");
        }
        let (x, y, z, yaw) = match destination {
            RecoverDestination::Track => {
                let others: Vec<(f32, f32)> = self
                    .session
                    .participants
                    .values()
                    .filter(|o| o.player_id != *player_id && !o.in_garage && !o.is_ghost())
                    .map(|o| (o.pos_x, o.pos_y))
                    .collect();
                let total = crate::laps::track_length_m(track);
                let from = if back_m > 0.0 && total > 0.0 {
                    (state.track_progress - back_m).rem_euclid(total)
                } else {
                    state.track_progress
                };
                let station = crate::recovery::track_station(&track.centerline, from, &others);
                let (x, y, z, yaw) = physics::pose_at_station(&track.centerline, station);
                (x, y, physics::seat_height(track, x, y, z), yaw)
            }
            RecoverDestination::Pits => {
                let lane = track
                    .pit_lane
                    .as_ref()
                    .filter(|l| !l.boxes.is_empty())
                    .ok_or("Track has no pit lane")?;
                let spot = lane.box_at(state.pit.box_index.unwrap_or(0));
                (
                    spot.x,
                    spot.y,
                    physics::seat_height(track, spot.x, spot.y, spot.z),
                    spot.yaw_rad,
                )
            }
        };
        let nearest = physics::find_nearest_centerline_idx(&track.centerline, x, y, None);
        let progress = nearest.map(|i| track.centerline[i].distance_from_start_m);
        let state = self
            .session
            .participants
            .get_mut(player_id)
            .ok_or("No car in this session")?;
        place_still(state, x, y, z, yaw);
        if let (Some(idx), Some(progress)) = (nearest, progress) {
            state.nearest_centerline_idx = Some(idx as u32);
            state.track_progress = progress;
        }
        state.gear = 1;
        // The lap in progress is struck, and its ghost trace dropped.
        state.laps.invalid = true;
        state.pit.serviced = false;
        state.recovery = Some(Recovery::new(destination));
        self.lap_traces.remove(player_id);
        Ok(())
    }

    /// Each recovery's hold counted down (`crate::recovery`). A car towed
    /// to its box is handed to the pit autopilot, which services it and
    /// drives it out; a car put back on the track is let go once nothing
    /// is closing on it from behind, or after `RELEASE_WAIT_MAX_S`.
    fn update_recoveries(&mut self) {
        use crate::recovery::{RecoverDestination, RELEASE_WAIT_MAX_S};
        let dt = self.dt();
        let total = crate::laps::track_length_m(&self.track_config);
        let traffic: Vec<(PlayerId, f32, f32)> = self
            .session
            .participants
            .values()
            .filter(|s| !s.in_garage && !s.is_ghost() && !s.pit.in_lane && s.damage.is_drivable)
            .map(|s| (s.player_id, s.track_progress, s.speed_mps))
            .collect();
        for state in self.session.participants.values_mut() {
            let Some(mut recovery) = state.recovery else {
                continue;
            };
            if !state.damage.is_drivable || state.in_garage {
                state.recovery = None;
                continue;
            }
            recovery.left_s -= dt;
            let release = recovery.left_s <= 0.0
                && match recovery.destination {
                    RecoverDestination::Pits => true,
                    RecoverDestination::Track => {
                        let others: Vec<(f32, f32)> = traffic
                            .iter()
                            .filter(|(id, _, _)| *id != state.player_id)
                            .map(|&(_, progress, speed)| (progress, speed))
                            .collect();
                        recovery.waited_s >= RELEASE_WAIT_MAX_S
                            || crate::recovery::traffic_clear(state.track_progress, total, &others)
                    }
                };
            if recovery.left_s <= 0.0 {
                recovery.waited_s += dt;
            }
            if !release {
                state.recovery = Some(recovery);
                continue;
            }
            state.recovery = None;
            // A driver who drives the lane themselves is serviced where the
            // car stands, and drives it out.
            if recovery.destination == RecoverDestination::Pits && !state.manual_pits {
                // The crew's car now: serviced at the box and driven out.
                state.pit.driving = true;
                state.pit.restore_aids = Some([state.auto_gearbox, state.steering_assist]);
                state.auto_gearbox = true;
                state.steering_assist = false;
            }
        }
    }

    /// Where a qualifying car going out starts its outlap: its own box,
    /// when the track has a pit lane with boxes whose exit is past the
    /// line (in the lap's first half), so the outlap is the lane and then
    /// the lap; else `None`, and `qualifying_outlap_pose` puts it on the
    /// road. A car with no box dealt yet (the lane deals one on its first
    /// tick) is put on the road too.
    fn outlap_box_pose(&self, state: &CarState) -> Option<(f32, f32, f32, f32)> {
        let track = &self.track_config;
        let lane = track.pit_lane.as_ref().filter(|l| !l.boxes.is_empty())?;
        let length = crate::laps::track_length_m(track);
        if lane.exit_station_m >= length * 0.5 {
            return None;
        }
        let spot = lane.box_at(state.pit.box_index?);
        Some((
            spot.x,
            spot.y,
            physics::seat_height(track, spot.x, spot.y, spot.z),
            spot.yaw_rad,
        ))
    }

    /// Where a qualifying car goes out without a box to leave from
    /// (`outlap_box_pose`): the pit exit, the lane's last station on the
    /// centerline when that is past the line, else just past
    /// the line, so the car drives a whole lap (the outlap) before it
    /// crosses the line and starts the first timed one. Cars going out
    /// together are queued [`HOTLAP_SPACING_M`] apart along the lap, as on
    /// the hotlap run-up.
    fn qualifying_outlap_pose(&self, going_out: &PlayerId) -> Option<(f32, f32, f32, f32)> {
        let track = &self.track_config;
        let length = crate::laps::track_length_m(track);
        if track.centerline.len() < 2 || length <= 0.0 {
            return None;
        }
        let exit = track
            .pit_lane
            .as_ref()
            .map(|lane| lane.exit_station_m)
            .filter(|station| *station < length * 0.5);
        let first = exit.map_or(QUALIFYING_OUT_PAST_LINE_M, |station| {
            station + QUALIFYING_OUT_PAST_LINE_M
        });
        let mut fallback = None;
        for k in 0..HOTLAP_SLOT_TRIES {
            let station = first + k as f32 * HOTLAP_SPACING_M;
            if station > length * 0.5 {
                break;
            }
            let mut pose = physics::pose_at_station(&track.centerline, station);
            pose.2 = physics::seat_height(track, pose.0, pose.1, pose.2);
            fallback.get_or_insert(pose);
            let taken = self.session.participants.values().any(|other| {
                !other.in_garage
                    && other.player_id != *going_out
                    && (other.pos_x - pose.0).hypot(other.pos_y - pose.1) < HOTLAP_SLOT_CLEARANCE_M
            });
            if !taken {
                return Some(pose);
            }
        }
        fallback
    }

    /// Where the next car going out is put: on the centerline
    /// [`HOTLAP_RUNUP_M`] before the line, or the first slot behind it not
    /// already taken by a car on the track. `None` without a centerline.
    fn hotlap_runup_pose(&self, going_out: &PlayerId) -> Option<(f32, f32, f32, f32)> {
        let track = &self.track_config;
        let length = crate::laps::track_length_m(track);
        if track.centerline.len() < 2 || length <= 0.0 {
            return None;
        }
        let runup = HOTLAP_RUNUP_M.min(length * 0.2);
        let spacing = HOTLAP_SPACING_M.min(runup * 0.5);
        let mut fallback = None;
        for k in 0..HOTLAP_SLOT_TRIES {
            let station = length - runup - k as f32 * spacing;
            if station < length * 0.5 {
                break;
            }
            let mut pose = physics::pose_at_station(&track.centerline, station);
            pose.2 = physics::seat_height(track, pose.0, pose.1, pose.2);
            fallback.get_or_insert(pose);
            let taken = self.session.participants.values().any(|other| {
                !other.in_garage
                    && other.player_id != *going_out
                    && (other.pos_x - pose.0).hypot(other.pos_y - pose.1) < HOTLAP_SLOT_CLEARANCE_M
            });
            if !taken {
                return Some(pose);
            }
        }
        fallback
    }

    /// Start countdown mode with custom duration. The stored `next_mode` is
    /// transitioned to automatically when the countdown reaches zero.
    pub fn start_countdown_mode(&mut self, countdown_seconds: u16, next_mode: GameMode) {
        if next_mode == GameMode::Race {
            self.enter_weekend_race();
            // A race starts from the grid: after a finished race (or practice)
            // the cars are wherever they stopped, with their laps and finish
            // positions still set.
            self.line_up_on_grid();
        }
        if !self.ready.is_empty() {
            self.ready.clear();
            self.roster_dirty = true;
        }
        self.shootout = None;
        self.session.game_mode = GameMode::Countdown;
        self.session.state = SessionState::Countdown;
        self.session.countdown_ticks_remaining = Some(
            (self.tick_rate_hz as u32 * countdown_seconds.min(MAX_COUNTDOWN_SECONDS) as u32)
                .min(u16::MAX as u32) as u16,
        );
        self.session.next_mode = Some(next_mode);
    }

    /// However the host got to the race's grid or countdown, a weekend is
    /// at its race now. Nothing outside a weekend.
    fn enter_weekend_race(&mut self) {
        if self.is_weekend() {
            self.settle_weekend();
            if self.weekend.phase != crate::weekend::WeekendPhase::Race {
                self.begin_day(crate::weekend::WeekendPhase::Race);
                self.begin_phase_damage(crate::weekend::WeekendPhase::Race);
            }
            self.weekend.phase = crate::weekend::WeekendPhase::Race;
            self.weekend.round = 0;
        }
    }

    /// Transition from Countdown to another mode
    pub fn transition_from_countdown(&mut self, next_mode: GameMode) {
        self.session.countdown_ticks_remaining = None;
        self.session.next_mode = None;
        // set_game_mode performs the per-mode initialization (session state,
        // demo lap progress / demo driver spawn, race start bookkeeping).
        self.set_game_mode(next_mode);
    }

    /// Add a player to the session
    pub fn add_player(&mut self, player_id: PlayerId, car_config_id: CarConfigId) -> Option<u8> {
        self.pit_box_told.remove(&player_id);
        if self.session.participants.len() >= self.session.max_players as usize {
            return None;
        }

        // Find available grid position: the first free one, but on a grid
        // already lined up (the race's grid, its countdown) the slot behind
        // the last car, so a no-show's empty slot stays empty.
        let mut used_positions: Vec<u8> = self
            .session
            .participants
            .values()
            .map(|s| s.grid_position)
            .collect();
        used_positions.sort();

        let mut grid_position = 1;
        for pos in &used_positions {
            if *pos == grid_position {
                grid_position += 1;
            } else {
                break;
            }
        }
        let behind = used_positions.last().map_or(1, |p| p.saturating_add(1));
        if matches!(self.session.game_mode, GameMode::Grid | GameMode::Countdown)
            && self
                .track_config
                .start_positions
                .iter()
                .any(|s| s.position == behind)
        {
            grid_position = behind;
        }

        let fuel = self.start_fuel_liters(&player_id, car_config_id, self.planned_mode());
        // Seated on the grid for a race: its tyres are what the grid's are.
        let start = if self.planned_mode() == GameMode::Race {
            TyreStart::Grid
        } else {
            TyreStart::Garage
        };

        // Get grid slot
        if let Some(grid_slot) = self
            .track_config
            .start_positions
            .iter()
            .find(|s| s.position == grid_position)
        {
            let mut car_state = CarState::new(player_id, car_config_id, grid_slot);
            car_state.fuel_liters = fuel;
            fit_tyres(
                &mut car_state,
                &self.car_configs,
                &self.tuned_configs,
                &self.track_config,
                start,
                &self.car_setups,
            );
            // A client that never sends SetDriverAids would otherwise run the
            // car's own ABS and traction control in a session that forbids them.
            car_state.set_driver_aids(self.session.allowed_assists.clamp(car_state.driver_aids()));
            // The session's damage rule, the same for every car (AI included).
            car_state.damage_level = Some(self.session.damage);
            physics::seed_track_progress(&mut car_state, &self.track_config);
            self.session.participants.insert(player_id, car_state);
            // A weekend's newcomer after Q1 began takes no part in the
            // knockout and starts from the back.
            if self.is_weekend() && self.weekend.phase == crate::weekend::WeekendPhase::Qualifying {
                self.weekend.enter_late(player_id);
            }
            self.roster_dirty = true;
            Some(grid_position)
        } else {
            None
        }
    }

    /// The session's damage rule (`RaceSession::damage`), stamped on every
    /// car already seated; `add_player` stamps it on the rest. Set by the
    /// host on create, after the AI have been seated with the default.
    pub fn set_damage(&mut self, level: DamageLevel) {
        self.session.damage = level;
        for state in self.session.participants.values_mut() {
            state.damage_level = Some(level);
        }
    }

    /// Set a driver's aids to what they asked for, less what the session
    /// forbids; returns what was applied, or `None` for a player not here.
    pub fn set_driver_aids(
        &mut self,
        player_id: &PlayerId,
        asked: DriverAids,
    ) -> Option<DriverAids> {
        let applied = self.session.allowed_assists.clamp(asked);
        let car = self.session.participants.get_mut(player_id)?;
        car.set_driver_aids(applied);
        Some(applied)
    }

    /// Apply a driver's garage setup: every knob clamped into range, the
    /// result baked into the car they are simulated with from now on. A
    /// stock setup drops the tuned copy. `None` when the player has no car
    /// here. The setup takes effect at once, on the grid or mid-lap — but
    /// the fuel knob only where a car is fuelled: in a hotlap garage, or
    /// before the start (in the lobby or the countdown). Anywhere else it
    /// waits for the next time the car is put out.
    pub fn set_car_setup(&mut self, player_id: &PlayerId, asked: CarSetup) -> Option<CarSetup> {
        let applied = asked.clamp();
        let car = self.session.participants.get(player_id)?;
        let car_id = car.car_config_id;
        let refuel = car.in_garage
            || matches!(
                self.session.game_mode,
                GameMode::Lobby | GameMode::Countdown | GameMode::Grid
            );
        let base = self.car_configs.get(&car_id)?;
        match applied.simulated_config(base) {
            Some(tuned) => self.tuned_configs.insert(*player_id, tuned),
            None => self.tuned_configs.remove(player_id),
        };
        if applied.is_stock() {
            self.car_setups.remove(player_id);
        } else {
            self.car_setups.insert(*player_id, applied);
        }
        if refuel {
            let mode = if car.in_garage {
                self.session.game_mode
            } else {
                self.planned_mode()
            };
            let fuel = self.start_fuel_liters(player_id, car_id, mode);
            if let Some(car) = self.session.participants.get_mut(player_id) {
                car.fuel_liters = fuel;
            }
        }
        Some(applied)
    }

    /// Litres a lap of this track costs `car_id`, planned half full; `None`
    /// for an unknown car or a track with no line to plan on. Worked out
    /// once per car and kept.
    pub fn lap_fuel_liters(&mut self, car_id: CarConfigId) -> Option<f32> {
        if let Some(&liters) = self.lap_fuel.get(&car_id) {
            return Some(liters);
        }
        let car = self.car_configs.get(&car_id)?;
        let half = car.fuel.capacity_liters * 0.5;
        let profile = racing_line::build_laden(&self.track_config, car, half)?;
        let liters = racing_line::lap_fuel_liters(&profile, car, half);
        if !(liters.is_finite() && liters > 0.0) {
            return None;
        }
        let seconds = racing_line::lap_time_s(&profile);
        if seconds.is_finite() && seconds > 0.0 {
            self.lap_seconds.insert(car_id, seconds);
        }
        self.lap_fuel.insert(car_id, liters);
        Some(liters)
    }

    /// The race's distance in laps for `car_id`: the lap limit, or in a
    /// timed race as many of the car's ideal laps as the clock holds plus
    /// the one it runs out on. Call after `lap_fuel_liters`, which works the
    /// lap out.
    fn race_laps(&self, car_id: CarConfigId) -> f32 {
        match self.session.race_seconds {
            None => self.session.lap_limit as f32,
            Some(seconds) => {
                let lap = self
                    .lap_seconds
                    .get(&car_id)
                    .copied()
                    .unwrap_or(FALLBACK_LAP_SECONDS)
                    .max(1.0);
                (seconds as f32 / lap).ceil() + 1.0
            }
        }
    }

    /// What a driver's car is fuelled with for a run in `mode`: the race
    /// distance, [`RACE_FUEL_MARGIN`] and [`RACE_FUEL_RESERVE_LAPS`] for a race,
    /// [`HOTLAP_FUEL_LAPS`] for a hotlap or qualifying, a full tank for
    /// anything else; plus or minus the driver's fuel knob in laps, never
    /// under [`MIN_FUEL_LAPS`] and never over the tank. A car with no lap
    /// estimate goes out full.
    pub fn start_fuel_liters(
        &mut self,
        player_id: &PlayerId,
        car_id: CarConfigId,
        mode: GameMode,
    ) -> f32 {
        let Some(capacity) = self
            .car_configs
            .get(&car_id)
            .map(|car| car.fuel.capacity_liters.max(0.0))
        else {
            return 0.0;
        };
        let Some(lap) = self.lap_fuel_liters(car_id) else {
            return capacity;
        };
        let knob = self.car_setup(player_id).fuel_load as f32;
        let laps = match mode {
            GameMode::Race => {
                self.race_laps(car_id) * (1.0 + RACE_FUEL_MARGIN) + RACE_FUEL_RESERVE_LAPS
            }
            GameMode::Hotlap | GameMode::Qualification | GameMode::Practice => HOTLAP_FUEL_LAPS,
            _ => capacity / lap,
        };
        let car = &self.car_configs[&car_id];
        crate::pit::stint_fuel_cap(car, ((laps + knob).max(MIN_FUEL_LAPS) * lap).min(capacity))
    }

    /// The run a car put on the grid now is being fuelled for: the mode
    /// being counted into, or a race from the lobby.
    fn planned_mode(&self) -> GameMode {
        match self.session.game_mode {
            GameMode::Lobby => GameMode::Race,
            GameMode::Countdown => self.session.next_mode.unwrap_or(GameMode::Race),
            GameMode::Grid => GameMode::Race,
            mode => mode,
        }
    }

    /// Fill every car for a run in `mode`, as a session starting it does.
    fn refuel_all(&mut self, mode: GameMode) {
        let cars: Vec<(PlayerId, CarConfigId)> = self
            .session
            .participants
            .values()
            .map(|s| (s.player_id, s.car_config_id))
            .collect();
        for (player_id, car_id) in cars {
            let fuel = self.start_fuel_liters(&player_id, car_id, mode);
            if let Some(state) = self.session.participants.get_mut(&player_id) {
                state.fuel_liters = fuel;
            }
        }
    }

    /// The clamped setup a driver last sent; stock when they never did.
    pub fn car_setup(&self, player_id: &PlayerId) -> CarSetup {
        self.car_setups.get(player_id).copied().unwrap_or_default()
    }

    /// Keep a driver's orders for their next stop (clamped), and raise or
    /// lower their request to box this lap. Returns what was kept, or
    /// `None` for a player with no car here. A stop already under way
    /// keeps the plan it started with.
    pub fn set_pit_plan(
        &mut self,
        player_id: &PlayerId,
        asked: crate::pit::PitPlan,
    ) -> Option<crate::pit::PitPlan> {
        let plan = asked.clamp();
        let car = self.session.participants.get_mut(player_id)?;
        car.pit.requested = plan.box_this_lap && !car.pit.servicing;
        self.pit_plans.insert(*player_id, plan);
        Some(plan)
    }

    /// The orders a driver last sent for their next stop, if any.
    pub fn pit_plan(&self, player_id: &PlayerId) -> Option<crate::pit::PitPlan> {
        self.pit_plans.get(player_id).copied()
    }

    /// The config a driver's car is simulated with (their setup applied).
    pub fn simulated_config_for(&self, player_id: &PlayerId) -> Option<&CarConfig> {
        let state = self.session.participants.get(player_id)?;
        simulated_config(&self.car_configs, &self.tuned_configs, state)
    }

    /// Remove a player from the session
    pub fn remove_player(&mut self, player_id: &PlayerId) {
        if self.session.participants.remove(player_id).is_some() {
            self.roster_dirty = true;
        }
        self.away.remove(player_id);
        self.held_steering_assist.remove(player_id);
        self.aid_lines.remove(player_id);
        self.tuned_configs.remove(player_id);
        self.car_setups.remove(player_id);
        self.pit_plans.remove(player_id);
        self.lap_traces.remove(player_id);
        self.liveries.remove(player_id);
        if self.ready.remove(player_id) {
            self.roster_dirty = true;
        }
    }

    /// Take an AI car out of the session (the dashboard's kick): off the
    /// grid or off the track, out of the roster with the next tick. False
    /// when the id is not one of this session's AI drivers.
    pub fn remove_ai(&mut self, player_id: &PlayerId) -> bool {
        if !self.session.ai_player_ids.contains(player_id) {
            return false;
        }
        self.session.ai_player_ids.retain(|id| id != player_id);
        self.ai_profiles.remove(player_id);
        self.remove_player(player_id);
        true
    }

    /// Hold a human's seat after their connection was lost: the car stays
    /// in the session, driven by the server (`away_input`, steering the rack
    /// itself as the cool-down driver does) or, in a hotlap or qualifying
    /// session, parked in its garage. False when the player has no car here
    /// to hold (an AI, a demo, someone not seated).
    pub fn hold_seat(&mut self, player_id: &PlayerId) -> bool {
        if self.session.session_kind.is_watch_only()
            || self.session.ai_player_ids.contains(player_id)
        {
            return false;
        }
        let Some(state) = self.session.participants.get_mut(player_id) else {
            return false;
        };
        if self.away.insert(*player_id) {
            // The roster tells the other drivers the car is the server's.
            self.roster_dirty = true;
            if let std::collections::hash_map::Entry::Vacant(held) =
                self.held_steering_assist.entry(*player_id)
            {
                held.insert(state.steering_assist);
                state.steering_assist = false;
            }
            let car_id = state.car_config_id;
            self.plan_ai_speeds(car_id);
            let _ = self.hotlap_relocate(player_id, HotlapDestination::Garage, false);
        }
        true
    }

    /// Give a held seat back to its driver: the server stops driving the
    /// car and their steering aid comes back (after the flag the cool-down
    /// driver keeps the car, as for anyone finished). Returns the car's grid
    /// slot and model, or `None` when no seat is held for them here.
    pub fn resume_seat(&mut self, player_id: &PlayerId) -> Option<(u8, CarConfigId)> {
        if !self.away.remove(player_id) {
            return None;
        }
        // A new connection: it is told its box again.
        self.pit_box_told.remove(player_id);
        self.roster_dirty = true;
        self.orphaned_since_loop_tick = None;
        let state = self.session.participants.get_mut(player_id)?;
        if state.finish_position.is_none() {
            if let Some(assist) = self.held_steering_assist.remove(player_id) {
                state.steering_assist = assist;
            }
        }
        Some((state.grid_position, state.car_config_id))
    }

    /// Whether this human's seat is held while they are away.
    pub fn is_away(&self, player_id: &PlayerId) -> bool {
        self.away.contains(player_id)
    }

    /// Whether any human is seated here, connected or away.
    pub fn has_human_driver(&self) -> bool {
        self.session
            .participants
            .keys()
            .any(|id| !self.session.ai_player_ids.contains(id))
    }

    /// Whether a human seated here is connected (not away).
    pub fn has_connected_driver(&self) -> bool {
        self.session
            .participants
            .keys()
            .any(|id| !self.session.ai_player_ids.contains(id) && !self.away.contains(id))
    }

    /// The input the server drives an away driver's car with: the cool-down
    /// driver's, on the line at a gentle pace.
    pub fn away_input(&self, player_id: &PlayerId) -> PlayerInputData {
        self.cooldown_input(player_id)
    }

    /// The car index this player's telemetry carries, matching the roster.
    pub fn car_index_of(&self, player_id: &PlayerId) -> Option<u8> {
        self.session
            .participants
            .keys()
            .position(|id| id == player_id)
            .map(|idx| idx as u8)
    }

    /// Whether any car crossed a timing line this tick. The game loop drains
    /// the events once a tick, broadcasts them and measures the laps against
    /// the stored records; nothing in the sim reads them.
    pub fn has_lap_events(&self) -> bool {
        !self.lap_events.is_empty()
    }

    /// Timing lines crossed since the last call, drained.
    pub fn take_lap_events(&mut self) -> Vec<SessionLapEvent> {
        std::mem::take(&mut self.lap_events)
    }

    /// Where this session's sector lines are, for the joining client.
    pub fn sector_boundaries_m(&self) -> Vec<f32> {
        crate::laps::sector_boundaries_m(&self.track_config)
    }

    /// Lap length in metres.
    pub fn track_length_m(&self) -> f32 {
        crate::laps::track_length_m(&self.track_config)
    }

    /// The fastest legal lap set in this session so far.
    pub fn session_best_lap_ms(&self) -> Option<u32> {
        self.session_best_lap_ms
    }

    /// Remember a human driver's name, for the start order's references.
    pub fn set_driver_name(&mut self, player_id: PlayerId, name: &str) {
        self.driver_names.insert(player_id, name.to_string());
    }

    fn driver_name(&self, player_id: &PlayerId) -> String {
        self.ai_profiles
            .get(player_id)
            .map(|p| p.name.clone())
            .or_else(|| self.driver_names.get(player_id).cloned())
            .unwrap_or_default()
    }

    /// The start order the host asked for (see [`crate::grid_order`]),
    /// bounded; replaces any earlier one.
    pub fn set_grid_order(&mut self, order: Vec<String>) {
        self.session.grid_order = crate::grid_order::sanitize(order);
    }

    /// The drivers seated, with the slot each holds.
    fn seated_drivers(&self) -> Vec<crate::grid_order::Seated> {
        self.session
            .participants
            .values()
            .map(|s| crate::grid_order::Seated {
                id: s.player_id,
                name: self.driver_name(&s.player_id),
                grid_position: s.grid_position,
            })
            .collect()
    }

    /// Whom the next race grids by, slot by slot from pole: the weekend's
    /// grid, else the host's own order when there is one, else this
    /// session's qualifying result (drivers with no time behind the
    /// classified), else nobody (the seating order stands; under a reverse
    /// grid, `race_rules.reverse_grid`, that is the order turned round).
    /// A reverse grid turns the first N slots round. A driver the
    /// order names who has gone leaves their slot empty (`None`): the cars
    /// behind keep their places, as on a real grid. Held to the track's
    /// slots (`grid_order::fit`); `None` when even the drivers do not fit.
    fn grid_order_ids(&self) -> Option<Vec<Option<PlayerId>>> {
        let drivers = self.seated_drivers();
        let mut by_slot: Vec<&crate::grid_order::Seated> = drivers.iter().collect();
        by_slot.sort_by_key(|d| (d.grid_position, d.id));
        let here = |id: PlayerId| Some(id).filter(|id| drivers.iter().any(|d| d.id == *id));
        let mut order = if let Some(grid) = self.weekend.grid().filter(|_| self.is_weekend()) {
            crate::grid_order::complete(&by_slot, grid.into_iter().map(here).collect())
        } else if !self.session.grid_order.is_empty() {
            crate::grid_order::resolve(
                &drivers,
                self.session.host_player_id,
                &self.session.ai_player_ids,
                &self.session.grid_order,
            )
        } else if !self.qualifying.is_empty() {
            crate::grid_order::complete(
                &by_slot,
                self.qualifying_order().into_iter().map(here).collect(),
            )
        } else if self.session.race_rules.reverse_grid > 1 {
            by_slot.iter().map(|d| Some(d.id)).collect()
        } else {
            return None;
        };
        // A reverse grid turns the front of the order round.
        let reverse = (self.session.race_rules.reverse_grid as usize).min(order.len());
        order[..reverse].reverse();
        // The cut-off's non-qualifiers take no slot: the grid closes up
        // over them (they did not qualify, they are not no-shows), and they
        // are seated behind it, parked out of the race by the line-up.
        let out = self.non_qualifiers();
        if !out.is_empty() {
            let behind: Vec<Option<PlayerId>> = order
                .iter()
                .filter(|s| s.is_some_and(|id| out.contains(&id)))
                .copied()
                .collect();
            order.retain(|s| !s.is_some_and(|id| out.contains(&id)));
            while order.last() == Some(&None) {
                order.pop();
            }
            order.extend(behind);
        }
        crate::grid_order::fit(order, self.track_config.start_positions.len())
    }

    /// Each car's place on the grid the next start will use, 1 = pole: the
    /// order `apply_grid_order` will seat (host order, weekend grid or
    /// qualifying, empty slots kept), else the slots the cars hold. What
    /// the roster announces, so a lobby lists the grid before the cars are
    /// lined up on it.
    pub fn starting_slots(&self) -> HashMap<PlayerId, u8> {
        if let Some(order) = self.grid_order_ids() {
            return order
                .into_iter()
                .enumerate()
                .filter_map(|(i, id)| Some((id?, (i + 1) as u8)))
                .collect();
        }
        self.session
            .participants
            .iter()
            .map(|(id, s)| (*id, s.grid_position))
            .collect()
    }

    /// Seat the drivers on the grid slots in start order, a no-show's slot
    /// left empty. A session with no requested order and no qualifying
    /// keeps the slots it has.
    pub fn apply_grid_order(&mut self) {
        let Some(order) = self.grid_order_ids() else {
            return;
        };
        for (i, id) in order.iter().enumerate() {
            if let Some(state) = id.and_then(|id| self.session.participants.get_mut(&id)) {
                state.grid_position = (i + 1) as u8;
            }
        }
    }

    /// A legal qualifying lap. Returns this session's classification when
    /// the lap improved the driver's best, for the store to keep. The first
    /// time laps count in a session its own result replaces any order the
    /// host asked for.
    pub fn note_qualifying_lap(
        &mut self,
        player_id: PlayerId,
        lap_time_ms: u32,
    ) -> Option<crate::classification::QualifyingResult> {
        if self.session.game_mode != GameMode::Qualification || lap_time_ms == 0 {
            return None;
        }
        if self
            .qualifying
            .get(&player_id)
            .is_some_and(|(best, _)| *best <= lap_time_ms)
        {
            return None;
        }
        if self.qualifying.is_empty() {
            self.session.grid_order.clear();
        }
        self.qualifying
            .insert(player_id, (lap_time_ms, self.session.current_tick));
        // The grid the roster announces follows the classification.
        self.roster_dirty = true;
        Some(self.qualifying_result())
    }

    /// The drivers with a legal qualifying lap, fastest first.
    pub fn qualifying_order(&self) -> Vec<PlayerId> {
        let mut classified: Vec<(&PlayerId, &(u32, u32))> = self.qualifying.iter().collect();
        classified.sort_by_key(|(id, (ms, tick))| (*ms, *tick, **id));
        classified.into_iter().map(|(id, _)| *id).collect()
    }

    /// The qualifying classification, front first, each driver with the
    /// lap that placed them (0 for none). A plain session's is its order;
    /// a weekend's is combined: the round running (or the final, once
    /// ranked) by its laps, then every knocked-out car by grid slot with
    /// its round's lap.
    fn classified_laps(&self) -> Vec<(PlayerId, u32)> {
        let lap = |id: &PlayerId| self.qualifying.get(id).map_or(0, |(ms, _)| *ms);
        if !self.is_weekend() {
            return self
                .qualifying_order()
                .into_iter()
                .map(|id| (id, lap(&id)))
                .collect();
        }
        let front: Vec<(PlayerId, u32)> = if self.weekend.final_order.is_empty() {
            self.qualifying_order()
                .into_iter()
                .filter(|id| self.weekend.eliminated_in(id).is_none())
                .map(|id| (id, lap(&id)))
                .collect()
        } else {
            self.weekend.final_order.clone()
        };
        front
            .into_iter()
            .chain(
                self.weekend
                    .eliminated
                    .iter()
                    .map(|e| (e.player_id, e.lap_time_ms)),
            )
            .collect()
    }

    /// This session's classification as the store keeps it (a weekend's
    /// combined, `classified_laps`), drivers with a time only.
    pub fn qualifying_result(&self) -> crate::classification::QualifyingResult {
        let class = self
            .session
            .host_car_id
            .and_then(|id| self.car_configs.get(&id))
            .map(|c| c.class.clone())
            .unwrap_or_default();
        let entries = self
            .classified_laps()
            .into_iter()
            .filter(|(_, ms)| *ms > 0)
            .filter_map(|(id, lap_time_ms)| {
                let state = self.session.participants.get(&id)?;
                Some(crate::classification::QualifyingEntry {
                    name: self.driver_name(&id),
                    car_config_id: state.car_config_id,
                    lap_time_ms,
                    is_ai: self.session.ai_player_ids.contains(&id),
                })
            })
            .filter(|e| !e.name.is_empty())
            .collect();
        crate::classification::QualifyingResult {
            id: self.session.id,
            track_id: self.session.track_config_id,
            class,
            recorded_at: String::new(),
            entries,
        }
    }

    /// A fresh timed session (a qualifying round, a weekend's practice):
    /// every car's last and best laps and the session's bests are this
    /// session's, not the one before, so telemetry and `LapTiming` report
    /// what the round itself has seen.
    fn clear_timing_sheets(&mut self) {
        self.session_best_lap_ms = None;
        self.session_best_splits_ms = [None; SECTOR_COUNT];
        for state in self.session.participants.values_mut() {
            state.best_lap_time_ms = None;
            state.last_lap_time_ms = None;
            state.laps.last_invalid = false;
            state.laps.last_splits_ms = [0; SECTOR_COUNT];
            state.laps.best_lap_ms = None;
            state.laps.best_lap_splits_ms = [0; SECTOR_COUNT];
            state.laps.best_splits_ms = [None; SECTOR_COUNT];
        }
    }

    /// Put every car back on its grid slot with a clean race state (laps,
    /// times, finish position, damage), keeping the driver's aids.
    pub fn line_up_on_grid(&mut self) {
        self.apply_grid_order();
        self.finish_deadline_tick = None;
        self.race_end = RaceEnd::default();
        // A fresh start: its formation lap still to drive, nobody let go,
        // nobody penalised.
        self.race_start = Default::default();
        if !self.penalties.is_empty() {
            self.penalties.clear();
            self.roster_dirty = true;
        }
        self.yellow_passed.clear();
        // A fresh race, a fresh timing sheet: the session's bests and every
        // half-recorded ghost lap belong to the race that just ended.
        self.session_best_lap_ms = None;
        self.session_best_splits_ms = [None; SECTOR_COUNT];
        self.lap_traces.clear();
        self.lap_events.clear();
        let race_fuel: HashMap<PlayerId, f32> = self
            .session
            .participants
            .values()
            .map(|s| (s.player_id, s.car_config_id))
            .collect::<Vec<_>>()
            .into_iter()
            .map(|(player_id, car_id)| {
                (
                    player_id,
                    self.start_fuel_liters(&player_id, car_id, GameMode::Race),
                )
            })
            .collect();
        for state in self.session.participants.values_mut() {
            let Some(slot) = self
                .track_config
                .start_positions
                .iter()
                .find(|s| s.position == state.grid_position)
            else {
                continue;
            };
            let mut fresh = CarState::new(state.player_id, state.car_config_id, slot);
            // A race starts in neutral, so a driver can rev on the grid.
            fresh.gear = 0;
            if let Some(&fuel) = race_fuel.get(&state.player_id) {
                fresh.fuel_liters = fuel;
            }
            fit_tyres(
                &mut fresh,
                &self.car_configs,
                &self.tuned_configs,
                &self.track_config,
                TyreStart::Grid,
                &self.car_setups,
            );
            fresh.auto_gearbox = state.auto_gearbox;
            fresh.abs = state.abs;
            fresh.traction_control = state.traction_control;
            fresh.damage_level = state.damage_level;
            fresh.steering_assist = self
                .held_steering_assist
                .remove(&state.player_id)
                .unwrap_or(state.steering_assist);
            physics::seed_track_progress(&mut fresh, &self.track_config);
            *state = fresh;
        }
        self.held_steering_assist.clear();
        self.park_non_qualifiers();
    }

    /// The cut-off's non-qualifiers (`non_qualifiers`) do not start: each
    /// is out of the race from the line-up, parked in its pit box when the
    /// track has a lane (else on its slot at the back), as a retired car
    /// towed there is, so the race neither waits for it nor meets it.
    fn park_non_qualifiers(&mut self) {
        let out = self.non_qualifiers();
        if out != self.not_qualified {
            self.not_qualified = out.clone();
            self.roster_dirty = true;
        }
        let lane = self.track_config.pit_lane.as_ref();
        for id in out {
            let Some(state) = self.session.participants.get_mut(&id) else {
                continue;
            };
            state.damage.is_drivable = false;
            state.towed = true;
            state.retired_s = TOW_AFTER_S;
            if let Some(lane) = lane.filter(|l| !l.boxes.is_empty()) {
                let spot = lane.box_at(state.pit.box_index.unwrap_or(0));
                state.pos_x = spot.x;
                state.pos_y = spot.y;
                state.pos_z = spot.z;
                state.yaw_rad = spot.yaw_rad;
            }
        }
    }

    /// Switch on the debug-only hooks (`crate::debug_hooks`) for this
    /// session. The server does this only when its `[debug]` table asks.
    pub fn set_debug_hooks(&mut self, hooks: crate::debug_hooks::DebugHooks) {
        self.debug = hooks;
        self.debug_green_ticks = 0;
        self.debug_next_event = 0;
        self.debug_boost_until.clear();
    }

    /// The debug stand-in driver's input for a human's car, when the hooks
    /// ask for one: an AI profile at full skill drives it, its pit stops
    /// planned like any AI's (`plan_ai_stops`). `None` otherwise.
    pub fn stand_in_input(&self, player_id: &PlayerId) -> Option<PlayerInputData> {
        if !self.debug.stand_in_driver || self.is_ai_player(player_id) {
            return None;
        }
        let state = self.session.participants.get(player_id)?;
        let car_config = self.car_configs.get(&state.car_config_id)?;
        let mut profile = AiDriverProfile::new("Stand-in", 100);
        profile.id = *player_id;
        let mut input = self.ai_input_for(&profile, state, car_config);
        if self
            .debug_boost_until
            .get(player_id)
            .is_some_and(|until| self.debug_green_ticks < *until)
        {
            input.ers_boost = true;
        }
        Some(input)
    }

    /// A debug stand-in drives every human's car with the steering aid
    /// off, in every mode: the AI steers the wheel itself, and an aid
    /// between it and the car reshapes its every input (in qualifying it
    /// put the car into the first wall and left it there). Called every
    /// tick; the pit autopilot puts a driver's own aid back as it hands a
    /// car back, so it is taken off again.
    fn hand_cars_to_stand_in(&mut self) {
        if !self.debug.stand_in_driver {
            return;
        }
        let ai = &self.ai_profiles;
        for state in self.session.participants.values_mut() {
            if !ai.contains_key(&state.player_id) {
                state.steering_assist = false;
            }
        }
    }

    /// Fire the debug events (`crate::debug_hooks`) whose time of green has
    /// come. Called every racing tick.
    fn fire_debug_events(&mut self) {
        if self.debug.events.is_empty() || self.session.state != SessionState::Racing {
            return;
        }
        self.debug_green_ticks += 1;
        let now_s = self.debug_green_ticks as f32 / self.tick_rate_hz.max(1) as f32;
        while let Some(event) = self.debug.events.get(self.debug_next_event).copied() {
            if event.at_s > now_s {
                break;
            }
            self.debug_next_event += 1;
            let ids: Vec<PlayerId> = self
                .session
                .participants
                .keys()
                .enumerate()
                .filter(|(idx, id)| match event.target {
                    crate::debug_hooks::Target::All => true,
                    crate::debug_hooks::Target::Host => !self.is_ai_player(id),
                    crate::debug_hooks::Target::Car(n) => *idx == n as usize,
                })
                .map(|(_, id)| *id)
                .collect();
            for id in ids {
                if let crate::debug_hooks::Action::Boost(seconds) = event.action {
                    let ticks = (seconds.max(0.0) * self.tick_rate_hz as f32) as u64;
                    self.debug_boost_until
                        .insert(id, self.debug_green_ticks + ticks);
                }
                if let Some(state) = self.session.participants.get_mut(&id) {
                    tracing::info!(
                        "debug event at {:.1} s: {:?} on car {}",
                        now_s,
                        event.action,
                        id
                    );
                    crate::debug_hooks::apply(event.action, state);
                }
            }
        }
    }

    /// Input for a finished human's car: a gentle server driver on the line,
    /// seeded from the player's id so the sim stays deterministic. Public so
    /// tests can drive a human's car round without a controller.
    pub fn cooldown_input(&self, player_id: &PlayerId) -> PlayerInputData {
        let Some(state) = self.session.participants.get(player_id) else {
            return PlayerInputData::default();
        };
        let Some(car_config) = self.car_configs.get(&state.car_config_id) else {
            return PlayerInputData::default();
        };
        let mut profile = AiDriverProfile::new("Cool-down", COOLDOWN_SKILL);
        profile.id = *player_id;
        self.ai_input_for(&profile, state, car_config)
    }

    /// What the air does this tick, before the physics: the DRS rule and
    /// every car's wake (`crate::slipstream`).
    fn update_air(&mut self) {
        self.update_retirements();
        self.update_recoveries();
        self.update_pits();
        self.update_line_refuel();
        self.plan_damaged_ai_speeds();
        self.update_drs();
        self.update_wind();
        self.update_road();
        self.update_debris();
        crate::slipstream::update(&mut self.session.participants, &self.car_configs);
        self.update_racecraft();
    }

    /// Every AI driver's racecraft for the coming tick (`crate::racecraft`):
    /// whether it passes, defends or lets a car by. Decided from one
    /// snapshot of the field, so no driver's answer depends on whose was
    /// worked out first.
    fn update_racecraft(&mut self) {
        let total = crate::laps::track_length_m(&self.track_config);
        if total <= 0.0 || self.session.ai_player_ids.is_empty() {
            return;
        }
        let ctx = crate::racecraft::Context {
            track: &self.track_config,
            total_m: total,
            race: self.session.game_mode == GameMode::Race,
            dt: self.dt(),
        };
        let field: Vec<crate::racecraft::Rival> = self
            .session
            .participants
            .values()
            .filter_map(|state| {
                let config = self.car_configs.get(&state.car_config_id)?;
                Some(crate::racecraft::Rival {
                    human: !self.ai_profiles.contains_key(&state.player_id),
                    ..crate::racecraft::Rival::of(state, config.length_m, config.width_m, total)
                })
            })
            .collect();
        let decided: Vec<(PlayerId, crate::racecraft::Racecraft)> = field
            .iter()
            .filter_map(|me| {
                let profile = self.ai_profiles.get(&me.id)?;
                let state = self.session.participants.get(&me.id)?;
                let speeds = self.ai_speed_profile(state);
                let risk = self.mistake_risk(state, total);
                Some((
                    me.id,
                    crate::racecraft::decide(state, me, profile, speeds, &field, &risk, &ctx),
                ))
            })
            .collect();
        let mut resets = Vec::new();
        for (id, racecraft) in decided {
            if let Some(state) = self.session.participants.get_mut(&id) {
                state.racecraft = racecraft;
                if racecraft.wants_reset {
                    resets.push(id);
                }
            }
        }
        // A car that cannot get back on the road by itself is put back, as
        // a driver's back-to-track puts theirs (`recover_car`): on the
        // road where it is, held for the recovery's cost. One stuck on the
        // road itself (a grade it cannot climb) is put a run-up back down
        // the lap instead: where it stood it would stall again. Refused
        // (too fast, on the pit route), it asks again next tick.
        for id in resets {
            let back = self
                .session
                .participants
                .get(&id)
                .filter(|s| s.is_on_track && !s.is_colliding)
                .map_or(0.0, |_| AI_STALL_RUN_UP_M);
            if self
                .recover_car_back(&id, crate::recovery::RecoverDestination::Track, back)
                .is_ok()
            {
                if let Some(state) = self.session.participants.get_mut(&id) {
                    state.racecraft.off_road_s = 0.0;
                    state.racecraft.wants_reset = false;
                    state.racecraft.stuck_s = 0.0;
                    state.racecraft.reverse_s = 0.0;
                }
            }
        }
    }

    /// What puts an AI at risk of a mistake away from pressure
    /// (`racecraft::MistakeRisk`): its tyres, the water under them against
    /// the road's baked grip, and its time in the car this stint (the laps
    /// since its last stop at its lap time).
    fn mistake_risk(&self, state: &CarState, total_m: f32) -> crate::racecraft::MistakeRisk {
        let Some(config) = self.car_configs.get(&state.car_config_id) else {
            return crate::racecraft::MistakeRisk::default();
        };
        let lap_s = state
            .last_lap_time_ms
            .filter(|ms| *ms > 0)
            .map(|ms| ms as f32 / 1000.0)
            .or_else(|| self.lap_seconds.get(&state.car_config_id).copied())
            .unwrap_or(FALLBACK_LAP_SECONDS);
        let stint_laps = (laps_run(state, total_m) - state.pit.stint_start_laps).max(0.0);
        crate::racecraft::MistakeRisk::of(
            state,
            &config.tire_config,
            self.track_config.track_surface.baked_water,
            stint_laps * lap_s,
        )
    }

    /// The sky this tick (`crate::conditions`), once a second: the clock,
    /// the forecast's weather, the rain, the air and the asphalt, baked onto
    /// the session's track for the physics, and the rain handed to the road.
    /// A session whose sky holds only reads the road's water for telemetry.
    fn update_sky(&mut self) {
        let rate = self.tick_rate_hz.max(1) as u32;
        if !self.session.current_tick.is_multiple_of(rate) {
            return;
        }
        let session_s = self.sky_seconds();
        let (water, rubber, snow) = match self.track_config.road_state.as_ref() {
            Some(road) => (
                road.mean_water(),
                road.mean_line_rubber(),
                road.mean_line_snow(),
            ),
            None => (
                self.track_config.track_surface.water,
                self.sky.line_rubber,
                self.sky.line_snow,
            ),
        };
        self.sky.line_rubber = rubber;
        self.sky.line_snow = snow;
        self.sky.step(session_s, 1.0, water);
        if self.sky.is_static() {
            return;
        }
        // The wind's mean follows the weather (a front blows harder and
        // turns it); the bake's own figure, to the bit, while it has not
        // moved.
        let start_wind = self.sky.wind_target_kph(self.sky.start.weather);
        if self.sky.wind_kph != start_wind || self.sky.wind_veer_deg != 0.0 {
            self.track_config.track_surface.wind_mps =
                crate::data::wind_onto_track(&self.track_config, self.sky.wind_mps());
        }
        let surface = &mut self.track_config.track_surface;
        surface.air_temperature_c = self.sky.air_c;
        surface.track_temperature_c = self.sky.track_c;
        surface.air_density_ratio = self.sky.density_ratio;
        // The water the road holds is the tyre the track calls for.
        surface.water = water;
        surface.wet = water > crate::conditions::WET_FROM_WATER;
        if let Some(road) = self.track_config.road_state.as_mut() {
            road.rain = self.sky.rain;
            road.snowfall = self.sky.snowfall;
            road.track_c = self.sky.track_c;
        }
    }

    /// The road this tick (`crate::road_state`): every car that crosses
    /// into a new cell lays its wheels' passes there (rubber, a dried line,
    /// its marbles swept), and the road steps every few ticks.
    fn update_road(&mut self) {
        let Some(road) = self.track_config.road_state.as_mut() else {
            return;
        };
        for state in self.session.participants.values_mut() {
            if state.in_garage || state.towed || state.speed_mps < 3.0 {
                // Nothing laid standing, in the garage or on the hook.
                state.road_work.restart();
                continue;
            }
            let cell = (state.track_progress.max(0.0) / crate::road_state::CELL_M) as u32;
            if cell != state.road_cell {
                state.road_cell = cell;
                // Each tyre lays its pass where its patch runs, by its load,
                // its sliding and the tread it wore since the last cell.
                let weight_n = self
                    .car_configs
                    .get(&state.car_config_id)
                    .map_or(0.0, |c| c.mass_kg * crate::physics::GRAVITY);
                let work = &state.road_work;
                let passes = work.passes(weight_n);
                road.note_pass(state.track_progress, &passes, work.shed());
                if state.pit.in_lane {
                    // And in the pit lane, on the lane's own station.
                    let weight: f32 = passes.iter().map(|p| p.weight).sum();
                    road.note_lane_pass(state.pit.lane_station_m, weight);
                }
                state.road_work.restart();
            }
        }
        if self
            .session
            .current_tick
            .is_multiple_of(crate::road_state::STEP_TICKS)
        {
            let evaporation = self.sky.evaporation();
            road.step(
                crate::road_state::STEP_TICKS as f32 / self.tick_rate_hz.max(1) as f32,
                evaporation,
            );
        }
    }

    /// Debris and punctures (`crate::tyre_thermal::puncture`). A hard hit
    /// sheds a piece onto the road where it happened, kept for a while
    /// ([`DEBRIS_LIFE_S`], at most [`MAX_DEBRIS`] pieces), and may hole the
    /// tyre nearest it: at once, or as a slow leak. A car that drives over
    /// a piece picks it up, and may puncture a tyre on it. The chances are
    /// hashed from the tick and the car, so a replayed session loses the
    /// same tyres.
    fn update_debris(&mut self) {
        let tick = self.session.current_tick;
        let dt = self.dt();
        let mut shed: Vec<(f32, f32)> = Vec::new();
        for state in self.session.participants.values_mut() {
            let hit = std::mem::take(&mut state.last_hit_pct);
            if hit <= 0.0 {
                continue;
            }
            let seed = tick as u64 ^ (state.player_id.as_u128() as u64).rotate_left(17);
            if hit >= DEBRIS_HIT_PCT {
                shed.push((state.pos_x, state.pos_y));
            }
            if hit >= PUNCTURE_HIT_PCT {
                let chance = ((hit - PUNCTURE_HIT_PCT) / 40.0).clamp(0.0, PUNCTURE_HIT_MAX_CHANCE);
                if crate::wind::hash01(seed, 0xD1E) < chance {
                    // The tyre nearest the hit: the angle is in the car's
                    // frame, 0 the nose, π/2 the left side.
                    let a = state.last_hit_angle;
                    let front = a.cos() >= 0.0;
                    let left = a.sin() >= 0.0;
                    let index = match (front, left) {
                        (true, true) => 0,
                        (true, false) => 1,
                        (false, true) => 2,
                        (false, false) => 3,
                    };
                    let leak = slow_leak(seed, HIT_LEAK_KPA_PER_S);
                    tyre_thermal::puncture(state.tires.each_mut()[index], leak);
                }
            }
        }
        for (x, y) in shed {
            if self.debris.len() < MAX_DEBRIS {
                self.debris.push(Debris {
                    x,
                    y,
                    until_tick: tick + (DEBRIS_LIFE_S / dt.max(1e-4)) as u32,
                });
            }
        }
        if self.debris.is_empty() {
            return;
        }
        // Who drives over a piece: each wheel of every moving car.
        let mut picked = vec![false; self.debris.len()];
        for state in self.session.participants.values_mut() {
            if state.in_garage || state.towed || state.speed_mps < 5.0 {
                continue;
            }
            let Some(config) = self.car_configs.get(&state.car_config_id) else {
                continue;
            };
            let (c, s) = (state.yaw_rad.cos(), state.yaw_rad.sin());
            let half_l = config.wheelbase_m / 2.0;
            let wheels = [
                (half_l, config.track_width_front_m / 2.0),
                (half_l, -config.track_width_front_m / 2.0),
                (-half_l, config.track_width_rear_m / 2.0),
                (-half_l, -config.track_width_rear_m / 2.0),
            ];
            for (d, piece) in self.debris.iter().enumerate() {
                if picked[d] {
                    continue;
                }
                for (i, (lx, ly)) in wheels.iter().enumerate() {
                    let wx = state.pos_x + lx * c - ly * s;
                    let wy = state.pos_y + lx * s + ly * c;
                    let d2 = (wx - piece.x).powi(2) + (wy - piece.y).powi(2);
                    if d2 > DEBRIS_REACH_M * DEBRIS_REACH_M {
                        continue;
                    }
                    picked[d] = true;
                    let seed = tick as u64
                        ^ (state.player_id.as_u128() as u64).rotate_left(23)
                        ^ (d as u64).rotate_left(41);
                    if crate::wind::hash01(seed, 0xDEB) < DEBRIS_PUNCTURE_CHANCE {
                        let leak = slow_leak(seed, DEBRIS_LEAK_KPA_PER_S);
                        tyre_thermal::puncture(state.tires.each_mut()[i], leak);
                    }
                    // The piece strikes the bodywork over the wheel: a dent
                    // on the nose or the tail, a little on that side, and
                    // a jolt that scrubs some speed.
                    let strike = DEBRIS_STRIKE_PCT * (state.speed_mps / 50.0).clamp(0.25, 1.0);
                    let angle = match i {
                        0 => 0.3,
                        1 => -0.3,
                        2 => std::f32::consts::PI - 0.3,
                        _ => std::f32::consts::PI + 0.3,
                    };
                    crate::physics::apply_damage_to_car(state, angle, strike);
                    state.vel_x *= 1.0 - DEBRIS_STRIKE_SCRUB;
                    state.vel_y *= 1.0 - DEBRIS_STRIKE_SCRUB;
                    state.speed_mps *= 1.0 - DEBRIS_STRIKE_SCRUB;
                    break;
                }
            }
        }
        let mut d = 0;
        self.debris.retain(|piece| {
            let keep = !picked[d] && piece.until_tick > tick;
            d += 1;
            keep
        });
    }

    /// A car out of the race stands where it stopped for
    /// [`TOW_AFTER_S`], then is towed away: to its own pit box when the
    /// track has a pit lane (where the client goes on drawing it, out of
    /// everyone's way), else left where it is. Either way it leaves the
    /// collision passes, the wake and the DRS gaps; a race has never waited
    /// for it (`DamageState::is_drivable` counts as classified).
    fn update_retirements(&mut self) {
        let dt = self.dt();
        let lane = self.track_config.pit_lane.as_ref();
        for state in self.session.participants.values_mut() {
            if state.damage.is_drivable {
                // Repaired (a new grid, the hotlap garage): back in the race.
                state.retired_s = 0.0;
                state.towed = false;
                continue;
            }
            if state.towed {
                continue;
            }
            state.retired_s += dt;
            if state.retired_s < TOW_AFTER_S {
                continue;
            }
            state.towed = true;
            if let Some(lane) = lane.filter(|l| !l.boxes.is_empty()) {
                let spot = lane.box_at(state.pit.box_index.unwrap_or(0));
                state.pos_x = spot.x;
                state.pos_y = spot.y;
                state.pos_z = spot.z;
                state.yaw_rad = spot.yaw_rad;
            }
            state.vel_x = 0.0;
            state.vel_y = 0.0;
            state.vel_z = 0.0;
            state.speed_mps = 0.0;
            state.angular_vel_yaw = 0.0;
            state.throttle_input = 0.0;
            state.gear = 0;
            state.is_colliding = false;
            state.pit.driving = false;
            state.pit.wants_stop = false;
            state.pit.servicing = false;
            state.pit.in_lane = false;
            state.pit.limiter = false;
        }
    }

    /// The pit lane this tick (`crate::pit`): every car placed against it
    /// (its limiter, its box), the exit light, a human's car taken over by
    /// the pit autopilot as it drives into the lane and handed back at the
    /// lane's end, services started and finished, and in a race the AI's
    /// decision to stop and its turn onto the pit route.
    fn update_pits(&mut self) {
        let Some(lane) = self.track_config.pit_lane.as_ref() else {
            return;
        };
        let dt = self.dt();
        let total = crate::laps::track_length_m(&self.track_config);

        // The exit light: shut before a race's start, and while a car on
        // the track is about to pass where the lane rejoins it.
        let before_the_start =
            matches!(self.session.game_mode, GameMode::Countdown | GameMode::Grid)
                || (self.session.game_mode == GameMode::Race
                    && self.session.state != SessionState::Racing
                    && self.session.state != SessionState::Finished);
        let traffic: Vec<(f32, f32)> = self
            .session
            .participants
            .values()
            // Not "off the lane": the exit taper lies over the road, and a
            // car racing past the exit is on it.
            .filter(|s| !s.in_garage && !s.pit.driving && !s.pit.servicing && s.damage.is_drivable)
            .map(|s| (s.track_progress, s.speed_mps))
            .collect();
        let exit_closed = crate::pit::exit_closed(lane, before_the_start, &traffic, total);

        // Every car its own box: dealt the first time it is placed against
        // the lane, the lowest box nobody holds (BTreeMap order, so the
        // deal is deterministic).
        let mut taken: Vec<u8> = self
            .session
            .participants
            .values()
            .filter_map(|s| s.pit.box_index)
            .collect();
        for state in self.session.participants.values_mut() {
            if state.pit.box_index.is_none() {
                let b = crate::pit::deal_box(&taken, lane.boxes.len());
                state.pit.box_index = Some(b);
                taken.push(b);
            }
        }

        let ai_ids: std::collections::HashSet<PlayerId> =
            self.session.ai_player_ids.iter().copied().collect();
        // A debug stand-in, and the server driving an away driver's car,
        // drive their stops as the AI does, out to the lane's end.
        let stand_in = self.debug.stand_in_driver;
        let away = &self.away;
        let centerline = &self.track_config.centerline;
        let mut arrived: Vec<PlayerId> = Vec::new();
        let mut finished: Vec<PlayerId> = Vec::new();
        // Race control in the lane (a race under way): the drive-throughs
        // owed, the ones driven, and the cars out past a red light.
        let racing =
            self.session.game_mode == GameMode::Race && self.session.state == SessionState::Racing;
        let owes_drive_through: std::collections::BTreeSet<PlayerId> = if racing {
            self.penalties
                .iter()
                .filter(|(_, list)| {
                    crate::race_control::owed(list)
                        .is_some_and(|p| p.kind == crate::race_control::PenaltyKind::DriveThrough)
                })
                .map(|(id, _)| *id)
                .collect()
        } else {
            Default::default()
        };
        let mut drove_through: Vec<PlayerId> = Vec::new();
        let mut out_on_red: Vec<PlayerId> = Vec::new();
        // Closed while the safety car's queue forms: a car coming in is
        // not taken on (`PitState::entered_closed`), and the autopilot does
        // not take a car in.
        let lane_closed = racing && self.race_control.pit_closed();
        for state in self.session.participants.values_mut() {
            if state.in_garage || state.towed {
                continue;
            }
            // The windowed search is only good while the car is by the lane:
            // a hint carried round a lap drifts to whichever node is nearest
            // from wherever the car is, and at the lane's mouth the search
            // then looked at its far end (the AI at São Paulo never found
            // its way in). Away from the lane nothing is looked for; by it,
            // a hint that has lost the car is dropped for a full search.
            if !lane.near(state.pos_x, state.pos_y, crate::pit::LANE_SEARCH_MARGIN_M) {
                let pit = &mut state.pit;
                pit.lane_node = None;
                pit.in_lane = false;
                pit.entered_closed = false;
                pit.limiter = false;
                pit.exit_closed = exit_closed;
                if !pit.driving {
                    pit.serviced = false;
                }
                continue;
            }
            let mut at = lane.locate(state.pos_x, state.pos_y, state.pit.lane_node);
            if state.pit.lane_node.is_some() && at.distance_m > crate::pit::LANE_SEARCH_MARGIN_M {
                at = lane.locate(state.pos_x, state.pos_y, None);
            }
            let is_ai = ai_ids.contains(&state.player_id);
            let drives_as_ai = is_ai || stand_in || away.contains(&state.player_id);
            // How far past the road edge on the lane's side the car is.
            let off_road = state
                .nearest_centerline_idx
                .and_then(|i| centerline.get(i as usize))
                .map_or(f32::NEG_INFINITY, |p| {
                    let left = -state.lateral_offset_m;
                    if lane.lane_side > 0 {
                        left - p.width_left_m
                    } else {
                        -left - p.width_right_m
                    }
                });
            let in_lane = at.distance_m <= lane.width_m / 2.0 + 1.0;
            // On the lane, the car's place on the lap is looked for near
            // where the lane's own progress puts it. The physics' windowed
            // search follows whichever leg is nearest, and round the inside
            // of a hairpin that is the wrong one: at Spa a car leaving the
            // lane after La Source was still placed back on the pit
            // straight, and the AI it was handed to drove off the wrong way.
            if in_lane {
                if let Some(idx) = anchor_on_lap(lane, centerline, total, at.station_m, state) {
                    state.nearest_centerline_idx = Some(idx);
                }
            }
            let pit = &mut state.pit;
            let (was_in_lane, was_station, was_limiter) =
                (pit.in_lane, pit.lane_station_m, pit.limiter);
            pit.lane_node = Some(at.node as u32);
            pit.lane_station_m = at.station_m;
            pit.in_lane = in_lane;
            if !in_lane {
                pit.entered_closed = false;
            } else if !was_in_lane && lane_closed && !pit.driving {
                pit.entered_closed = true;
            }
            pit.limiter =
                pit.in_lane && (lane.limit_start_m..=lane.limit_end_m).contains(&at.station_m);
            pit.exit_closed = exit_closed;
            if racing {
                // A drive-through: in at the first line owing one, out at
                // the last without a stop between.
                if pit.limiter && !was_limiter && at.station_m < lane.limit_end_m {
                    pit.drive_through = owes_drive_through.contains(&state.player_id);
                    // The route (the AI's, the autopilot's) goes on past
                    // the box: as if served there.
                    if pit.drive_through && pit.driving {
                        pit.serviced = true;
                    }
                }
                let past_last_line = was_in_lane
                    && in_lane
                    && was_station < lane.limit_end_m
                    && at.station_m >= lane.limit_end_m;
                if past_last_line && std::mem::take(&mut pit.drive_through) {
                    drove_through.push(state.player_id);
                }
                // Out past the red light, driving the lane oneself.
                if past_last_line && exit_closed && !pit.driving {
                    out_on_red.push(state.player_id);
                }
            }
            if !pit.in_lane && !pit.driving {
                // Out of the lane: the next visit is a new stop.
                pit.serviced = false;
            }
            // A human's car that crosses the pit entry line is the
            // autopilot's until the exit line: to its own box, serviced,
            // and out.
            if !drives_as_ai
                && !state.manual_pits
                && !lane_closed
                && !pit.driving
                && !pit.serviced
                && state.finish_position.is_none()
                && crate::pit::takes_over(lane, &at, state.yaw_rad, off_road)
            {
                pit.driving = true;
                pit.restore_aids = Some([state.auto_gearbox, state.steering_assist]);
                state.auto_gearbox = true;
                state.steering_assist = false;
            }
            // A car towed to its box waits out its hold before the crew starts.
            let recovery_hold = state.recovery;
            let pit = &mut state.pit;
            if pit.servicing {
                pit.service_left_s -= dt;
                if pit.service_left_s <= 0.0 {
                    finished.push(state.player_id);
                }
            } else if pit.limiter
                && !pit.serviced
                && !pit.entered_closed
                && recovery_hold.is_none()
                && state.speed_mps < crate::pit::BOX_STOP_SPEED_MPS
            {
                let spot = lane.box_at(pit.box_index.unwrap_or(0));
                let d = ((state.pos_x - spot.x).powi(2) + (state.pos_y - spot.y).powi(2)).sqrt();
                if d <= crate::pit::BOX_RADIUS_M {
                    arrived.push(state.player_id);
                }
            }
            // Held at the red exit light (the count runs until the car is
            // past the light, so the hold's limit is not reset by a creep).
            pit.held = pit.driving
                && pit.serviced
                && exit_closed
                && pit.held_s < crate::pit::EXIT_HOLD_MAX_S
                && state.speed_mps < 1.0
                && at.station_m > lane.limit_end_m - 30.0
                && at.station_m < lane.limit_end_m;
            if pit.driving && pit.serviced && at.station_m < lane.limit_end_m && exit_closed {
                if pit.held || pit.held_s > 0.0 {
                    pit.held_s += dt;
                }
            } else if at.station_m >= lane.limit_end_m || !pit.driving {
                pit.held_s = 0.0;
            }
            // Headway on the route: a car that makes none for a while is
            // put back on it.
            if pit.driving && !pit.servicing && !pit.held && state.speed_mps < 0.5 {
                pit.stuck_s += dt;
            } else {
                pit.stuck_s = 0.0;
            }
            if pit.stuck_s > crate::pit::AUTOPILOT_STUCK_S {
                pit.stuck_s = 0.0;
                let spot = lane.box_at(pit.box_index.unwrap_or(0));
                let (x, y, yaw) = crate::pit::recovery_pose(lane, spot, state);
                state.pos_x = x;
                state.pos_y = y;
                state.yaw_rad = yaw;
                state.vel_x = 0.0;
                state.vel_y = 0.0;
                state.speed_mps = 0.0;
                state.angular_vel_yaw = 0.0;
            }
            let pit = &mut state.pit;
            // The route ends past the exit line (a human's) or the lane's
            // end (the AI's).
            if pit.driving
                && pit.serviced
                && at.station_m >= crate::pit::route_end_m(lane, drives_as_ai)
            {
                pit.driving = false;
                pit.held = false;
                match pit.restore_aids.take() {
                    Some([gearbox, steering]) => {
                        state.auto_gearbox = gearbox;
                        state.steering_assist = steering;
                    }
                    None => state.auto_gearbox = false,
                }
            }
        }
        for player_id in arrived {
            self.start_service(&player_id);
        }
        for player_id in finished {
            self.finish_service(&player_id);
        }
        for player_id in drove_through {
            self.serve_penalty(&player_id, crate::race_control::PenaltyKind::DriveThrough);
            if let Some(state) = self.session.participants.get_mut(&player_id) {
                state.pit.wants_stop = false;
            }
        }
        for player_id in out_on_red {
            self.penalise(player_id, crate::race_control::Offence::PitExitRed);
        }
        self.plan_ai_stops();
    }

    /// The input the pit autopilot drives each human's car with this tick,
    /// for the cars it holds (`crate::pit::drive_input`). The driver's own
    /// headlight switch and flash still count.
    fn pit_autopilot_inputs(
        &self,
        inputs: &HashMap<PlayerId, PlayerInputData>,
    ) -> HashMap<PlayerId, PlayerInputData> {
        let Some(lane) = self.track_config.pit_lane.as_ref() else {
            return HashMap::new();
        };
        self.session
            .participants
            .values()
            // A debug stand-in plans and drives its stops as the AI does,
            // run-up included (`stand_in_input`), and so does the server
            // driving an away driver's car.
            .filter(|s| {
                s.pit.driving
                    && !s.in_garage
                    && !self.is_ai_player(&s.player_id)
                    && !self.debug.stand_in_driver
                    && !self.away.contains(&s.player_id)
            })
            .filter_map(|state| {
                let config = self.car_configs.get(&state.car_config_id)?;
                let mut input = crate::pit::drive_input(
                    lane,
                    lane.box_at(state.pit.box_index.unwrap_or(0)),
                    state,
                    config,
                    &self.lane_traffic(lane, state),
                    crate::pit::route_end_m(lane, false),
                );
                if let Some(own) = inputs.get(&state.player_id) {
                    input.headlights = own.headlights;
                    input.flash = own.flash;
                }
                Some((state.player_id, input))
            })
            .collect()
    }

    /// The other cars on the pit lane, for `state`'s route.
    fn lane_traffic(
        &self,
        lane: &crate::pit::PitLane,
        state: &CarState,
    ) -> Vec<crate::pit::LaneCar> {
        self.session
            .participants
            .values()
            .filter(|o| o.player_id != state.player_id && o.pit.in_lane && !o.in_garage)
            .map(|o| {
                let (at, lateral) = lane.lateral_of(o.pos_x, o.pos_y, o.pit.lane_node);
                crate::pit::LaneCar {
                    station_m: at.station_m,
                    lateral_m: lateral,
                    speed_mps: o.speed_mps,
                }
            })
            .collect()
    }

    /// Pit stops started since the last call, for the clients.
    pub fn take_pit_events(&mut self) -> Vec<(PlayerId, PitService)> {
        std::mem::take(&mut self.pit_events)
    }

    /// Each human whose box has been dealt and who has not been told where
    /// it is (since joining, or rejoining), or whose pit autopilot has been
    /// switched since, with their box.
    pub fn take_pit_box_news(&mut self) -> Vec<(PlayerId, PitBoxInfo)> {
        let Some(lane) = self.track_config.pit_lane.as_ref() else {
            return Vec::new();
        };
        let mut news = Vec::new();
        for state in self.session.participants.values() {
            let Some(index) = state.pit.box_index else {
                continue;
            };
            if self.session.ai_player_ids.contains(&state.player_id)
                || self.pit_box_told.get(&state.player_id) == Some(&state.manual_pits)
            {
                continue;
            }
            let spot = lane.box_at(index);
            news.push((
                state.player_id,
                PitBoxInfo {
                    pit_box: (index as usize % lane.boxes.len()) as u8,
                    x: spot.x,
                    y: spot.y,
                    z: spot.z,
                    yaw_rad: spot.yaw_rad,
                    lateral_m: lane.box_lateral_m(spot),
                    lane_width_m: lane.width_m,
                    radius_m: crate::pit::BOX_RADIUS_M,
                    manual: state.manual_pits,
                },
            ));
        }
        for (player_id, pit_box) in &news {
            self.pit_box_told.insert(*player_id, pit_box.manual);
        }
        news
    }

    /// Laps a car has left after the one it is on, in a race. In a timed
    /// race: none once it is on its flag lap, to the final lap once the
    /// clock has run out, and before that as many of its laps (its last,
    /// else the plan's) as the clock still holds, rounded up.
    fn laps_left(&self, state: &CarState) -> u32 {
        let lap = state.current_lap.max(1);
        if self.session.race_seconds.is_none() {
            return (self.session.lap_limit as u32).saturating_sub(lap as u32);
        }
        if self.race_end.flag_laps.contains_key(&state.player_id) {
            return 0;
        }
        if let Some(final_lap) = self.race_end.final_lap {
            return final_lap.saturating_sub(lap) as u32;
        }
        let lap_s = state
            .last_lap_time_ms
            .filter(|ms| *ms > 0)
            .map(|ms| ms as f32 / 1000.0)
            .or_else(|| self.lap_seconds.get(&state.car_config_id).copied())
            .unwrap_or(FALLBACK_LAP_SECONDS)
            .max(1.0);
        (self.race_seconds_left().unwrap_or(0.0) / lap_s).ceil() as u32
    }

    /// A timed race's length in ticks; `None` in a race over laps.
    fn race_length_ticks(&self) -> Option<u64> {
        self.session
            .race_seconds
            .map(|s| s as u64 * self.tick_rate_hz as u64)
    }

    /// Ticks since the green light; 0 before it.
    fn race_elapsed_ticks(&self) -> u64 {
        self.session.race_start_tick.map_or(0, |start| {
            self.session.current_tick.saturating_sub(start) as u64
        })
    }

    /// Seconds left on a timed race's clock (all of it before the green
    /// light, 0 once it has run out); `None` in a race over laps.
    pub fn race_seconds_left(&self) -> Option<f32> {
        let length = self.race_length_ticks()?;
        let elapsed = match self.session.game_mode {
            GameMode::Race => self.race_elapsed_ticks(),
            _ => 0,
        };
        Some(length.saturating_sub(elapsed) as f32 / self.tick_rate_hz as f32)
    }

    /// Where a timed race's clock stands, for telemetry: `None` unless the
    /// session is a timed race counting into or running its race, or a
    /// timed qualifying session running (`final_lap` 0, `left_ms` 0 once
    /// the chequered flag is out).
    pub fn race_clock(&self) -> Option<RaceClock> {
        if self.session.game_mode.has_outlap() {
            let length = self.timed_length_ticks()?;
            let left_ticks = length.saturating_sub(self.timed_elapsed_ticks());
            return Some(RaceClock {
                left_ms: (left_ticks * 1000 / self.tick_rate_hz.max(1) as u64).min(u32::MAX as u64)
                    as u32,
                final_lap: 0,
            });
        }
        let length = self.race_length_ticks()?;
        let elapsed = match self.session.game_mode {
            GameMode::Race => self.race_elapsed_ticks(),
            GameMode::Countdown if self.session.next_mode == Some(GameMode::Race) => 0,
            GameMode::Grid => 0,
            _ => return None,
        };
        let left_ticks = length.saturating_sub(elapsed);
        Some(RaceClock {
            left_ms: (left_ticks * 1000 / self.tick_rate_hz.max(1) as u64).min(u32::MAX as u64)
                as u32,
            final_lap: self.race_end.final_lap.unwrap_or(0),
        })
    }

    /// Laps a car has completed: those it had at the flag once it has
    /// finished (the cool-down lap after it is not counted), else so far.
    /// The race's classification as it stands (`crate::results`): the
    /// finishers in their places, then the cars still running by laps and
    /// distance, each with its race time and gap. `names` are the humans';
    /// the AI's come from their profiles.
    pub fn classification(
        &self,
        names: &HashMap<PlayerId, String>,
    ) -> Vec<crate::classification::ClassifiedCar> {
        let rate = self.tick_rate_hz.max(1) as f64;
        let start = self.session.race_start_tick.unwrap_or(0);
        let total = crate::laps::track_length_m(&self.track_config).max(1.0);
        let mut order: Vec<(&PlayerId, &CarState)> = self.session.participants.iter().collect();
        order.sort_by(|(_, a), (_, b)| {
            let place = |s: &CarState| s.finish_position.unwrap_or(u8::MAX);
            place(a).cmp(&place(b)).then_with(|| {
                let run = |s: &CarState| s.current_lap as f32 * total + s.track_progress;
                run(b).total_cmp(&run(a))
            })
        });
        let winner = order
            .first()
            .and_then(|(id, _)| self.race_end.finishers.get(*id).copied());
        order
            .iter()
            .enumerate()
            .map(|(i, (id, state))| {
                let finisher = self.race_end.finishers.get(*id).copied();
                let laps = self.laps_completed(id);
                // A finisher's time is when it crossed the line, its
                // penalties added.
                let at = |f: &Finisher| f.tick + f.penalty_ticks;
                let race_time_ms = finisher
                    .map(|f| (at(&f).saturating_sub(start) as f64 / rate * 1000.0).round() as u32);
                let (gap_ms, laps_down) = match (finisher, winner) {
                    (Some(f), Some(w)) if f.laps == w.laps => (
                        (i > 0).then(|| {
                            (at(&f).saturating_sub(at(&w)) as f64 / rate * 1000.0).round() as u32
                        }),
                        0,
                    ),
                    (_, Some(w)) => (None, w.laps.saturating_sub(laps)),
                    _ => (None, 0),
                };
                crate::classification::ClassifiedCar {
                    position: (i + 1).min(u8::MAX as usize) as u8,
                    player_id: **id,
                    name: self
                        .get_ai_profile(id)
                        .map(|p| p.name.clone())
                        .or_else(|| names.get(*id).cloned())
                        .or_else(|| self.driver_names.get(*id).cloned())
                        .unwrap_or_else(|| format!("Player-{}", &id.to_string()[..8])),
                    is_ai: self.is_ai_player(id),
                    car_config_id: state.car_config_id,
                    status: if finisher.is_some() {
                        "finished"
                    } else if !state.damage.is_drivable {
                        "retired"
                    } else {
                        "running"
                    },
                    laps,
                    best_lap_ms: state.best_lap_time_ms.filter(|ms| *ms > 0),
                    race_time_ms,
                    gap_ms,
                    laps_down,
                    missed_stops: finisher.is_some_and(|f| f.missed_stops),
                    penalty_s: self.penalty_seconds(id),
                }
            })
            .collect()
    }

    pub fn laps_completed(&self, player_id: &PlayerId) -> u16 {
        if let Some((_, finisher)) = self
            .race_end
            .finishers
            .iter()
            .find(|entry| entry.0 == player_id)
        {
            return finisher.laps;
        }
        self.session
            .participants
            .get(player_id)
            .map_or(0, |s| s.current_lap.saturating_sub(1))
    }

    /// The lap whose completion wins the race: the distance in a race over
    /// laps, and in a timed race the final lap once its clock has run out.
    fn finish_lap(&self) -> Option<u16> {
        if self.session.race_seconds.is_some() {
            self.race_end.final_lap
        } else {
            Some(self.session.lap_limit as u16)
        }
    }

    /// A timed race whose clock has just run out: the leader's lap is the
    /// last (the first car to complete it wins). Should nobody on that lap
    /// manage it, the race still ends: the leader's lap and the usual grace
    /// after the clock.
    fn update_race_clock(&mut self) {
        let Some(length) = self.race_length_ticks() else {
            return;
        };
        if self.race_end.final_lap.is_some()
            || self.session.race_start_tick.is_none()
            || self.race_elapsed_ticks() < length
        {
            return;
        }
        let running = self
            .session
            .participants
            .values()
            .filter(|s| s.finish_position.is_none() && s.damage.is_drivable);
        let leader = running.max_by(|a, b| {
            a.current_lap.cmp(&b.current_lap).then(
                a.track_progress
                    .partial_cmp(&b.track_progress)
                    .unwrap_or(std::cmp::Ordering::Equal),
            )
        });
        let final_lap = leader.map_or(1, |s| s.current_lap).max(1);
        self.race_end.final_lap = Some(final_lap);

        let laps_done = final_lap.saturating_sub(1).max(1) as u64;
        let average_lap_ticks = self.race_elapsed_ticks() / laps_done;
        let grace_ticks = ((average_lap_ticks as f32 * (FINISH_GRACE_LAPS + 1.0)) as u32)
            .max(FINISH_GRACE_MIN_SECONDS * self.tick_rate_hz as u32);
        self.finish_deadline_tick
            .get_or_insert(self.session.current_tick + grace_ticks);
        tracing::info!(
            "Session {}: the race clock has run out; lap {} is the last",
            self.session.id,
            final_lap
        );
    }

    /// A car has stopped at its box: what the service will do, and how long
    /// each part takes. Tyres always: the compound is the driver's setup's
    /// choice (the AI's plan for an AI). Fuel where the rules let this car
    /// refuel and it needs some: what the rest of the run needs (a race's
    /// distance with its margin, else what the session fills a car with).
    /// Repairs when anything is damaged. An AI's splash (`PitState::
    /// fuel_only`) is the fuel alone. The plan goes to every client
    /// (`ServerMessage::PitService`) for the pit-stop panel.
    fn start_service(&mut self, player_id: &PlayerId) {
        // A stop owing a stop-go is the stop-go: held in the box, no work.
        let racing = self.session.game_mode == GameMode::Race;
        if racing && self.owed_penalty(player_id) == Some(crate::race_control::PenaltyKind::StopGo)
        {
            let hold = self
                .penalties
                .get(player_id)
                .and_then(|list| crate::race_control::owed(list))
                .map_or(0.0, |p| p.seconds as f32);
            let Some(state) = self.session.participants.get_mut(player_id) else {
                return;
            };
            let pit_box = state.pit.box_index.unwrap_or(0);
            let compound = state.tyre_compound;
            let pit = &mut state.pit;
            pit.servicing = true;
            pit.stop_go = true;
            pit.drive_through = false;
            pit.service_left_s = hold;
            pit.service_compound = compound;
            pit.service_fuel_l = 0.0;
            pit.service_repair = false;
            pit.service_pads = false;
            pit.service_discs = false;
            pit.service_tyres = false;
            pit.service_pad_set = None;
            self.pit_events.push((
                *player_id,
                PitService {
                    pit_box,
                    tyres_s: 0.0,
                    compound,
                    fuel_s: 0.0,
                    fuel_l: 0.0,
                    repair_s: 0.0,
                    repair_pct: 0.0,
                    total_s: hold,
                },
            ));
            return;
        }
        if let Some(state) = self.session.participants.get_mut(player_id) {
            // A stop is no drive-through.
            state.pit.drive_through = false;
        }
        let Some(state) = self.session.participants.get(player_id) else {
            return;
        };
        let car_id = state.car_config_id;
        let is_ai = self.is_ai_player(player_id);
        let fuel_only = state.pit.fuel_only;
        // The driver's own orders; none (the crew's stop) for the AI.
        let orders = if is_ai {
            crate::pit::PitPlan::default()
        } else {
            self.pit_plan(player_id).unwrap_or_default()
        };
        // A stop the server planned (an AI's, or a car it drives for a
        // driver: a debug stand-in, an away driver) fits the compound it was
        // planned for, unless the driver ordered one: the setup's pick there
        // brought the car back in every lap to change off it.
        let planned_by_server =
            is_ai || self.debug.stand_in_driver || self.away.contains(player_id);
        let compound = if fuel_only || orders.keep_tyres {
            state.tyre_compound
        } else if planned_by_server && state.pit.wants_stop && orders.compound.is_none() {
            state.pit.next_compound
        } else {
            let tyre = self
                .simulated_config_for(player_id)
                .map(|c| c.tire_config.clone())
                .unwrap_or_default();
            match orders.compound {
                Some(index) => index.min(tyre.compounds().len().saturating_sub(1) as u8),
                None => self.car_setup(player_id).compound_index_for(
                    &tyre,
                    self.track_config.track_surface.water,
                    crate::road_state::RoadState::line_snow_of(&self.track_config),
                ),
            }
        };
        let laps_left = self.laps_left(state) as f32 + 1.0;
        let fuel_now = state.fuel_liters;
        let damage = crate::pit::damage_percent(state);
        let brake_wear = crate::pit::brake_wear_percent(state);
        let disc_wear = crate::pit::disc_wear_percent(state);
        let pit_box = state.pit.box_index.unwrap_or(0);
        let mode = self.session.game_mode;
        let Some(config) = self.car_configs.get(&car_id).cloned() else {
            return;
        };
        let fuel_target = if !self.refuels(car_id) {
            fuel_now
        } else if let Some(litres) = orders.fuel_l {
            (fuel_now + litres).min(config.fuel.capacity_liters)
        } else if mode == GameMode::Race {
            match self.lap_fuel_liters(car_id) {
                Some(lap) => (laps_left * (1.0 + RACE_FUEL_MARGIN) * lap + 0.5 * lap)
                    .min(config.fuel.capacity_liters),
                None => config.fuel.capacity_liters,
            }
        } else {
            self.start_fuel_liters(player_id, car_id, mode)
        };
        // A stint's energy limit: no more than the next stint can burn.
        let fuel_target = crate::pit::stint_fuel_cap(&config, fuel_target).max(fuel_now);
        let mut plan = crate::pit::plan_service_for(
            &config,
            fuel_target - fuel_now,
            damage,
            brake_wear,
            disc_wear,
            &orders,
        );
        if fuel_only {
            plan = crate::pit::ServicePlan {
                tyres_s: 0.0,
                repair_s: 0.0,
                repair_pct: 0.0,
                pads: false,
                discs: false,
                ..plan
            };
        }
        if let Some(state) = self.session.participants.get_mut(player_id) {
            let pit = &mut state.pit;
            pit.servicing = true;
            pit.service_left_s = plan.total_s();
            pit.service_compound = compound;
            pit.service_fuel_l = plan.fuel_l;
            pit.service_repair = plan.repair_pct > 0.0;
            pit.service_pads = plan.pads;
            pit.service_discs = plan.discs;
            pit.service_tyres = plan.tyres_s > 0.0;
            pit.service_pad_set = orders.pad_set.filter(|_| plan.pads);
        }
        self.pit_events.push((
            *player_id,
            PitService {
                pit_box,
                tyres_s: plan.tyres_s,
                compound,
                fuel_s: plan.fuel_s,
                fuel_l: plan.fuel_l,
                repair_s: plan.repair_s,
                repair_pct: plan.repair_pct,
                total_s: plan.total_s(),
            },
        ));
    }

    /// The service is done: the new set on (at what the car's tyres go out
    /// at: its blankets, or the air), the fuel in, the car repaired if that
    /// was part of the stop.
    fn finish_service(&mut self, player_id: &PlayerId) {
        let total_m = crate::laps::track_length_m(&self.track_config);
        // A stop-go's hold is over: served, and no stop for the pit rule.
        if self
            .session
            .participants
            .get(player_id)
            .is_some_and(|s| s.pit.stop_go)
        {
            if let Some(state) = self.session.participants.get_mut(player_id) {
                let pit = &mut state.pit;
                pit.stop_go = false;
                pit.servicing = false;
                pit.service_left_s = 0.0;
                pit.serviced = true;
                pit.wants_stop = false;
                pit.requested = false;
            }
            self.serve_penalty(player_id, crate::race_control::PenaltyKind::StopGo);
            return;
        }
        let Some(state) = self.session.participants.get_mut(player_id) else {
            return;
        };
        let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state).cloned()
        else {
            return;
        };
        if state.pit.service_tyres {
            let tyre = &config.tire_config;
            let temperature =
                tyre_thermal::start_temperature_c(tyre, &self.track_config.track_surface);
            let compound = state.pit.service_compound;
            let water = self.track_config.track_surface.water;
            // The pit rule's second compound (`data::PitRule`): a dry
            // compound other than the one coming off, or a treaded tyre.
            if compound != state.tyre_compound
                || tyre.compound(compound).kind != tyre_thermal::CompoundKind::Slick
            {
                state.pit.compound_changed = true;
            }
            tyre_thermal::fit(state, tyre, temperature, compound, water);
            state.pit.stint_start_laps = laps_run(state, total_m);
        }
        state.fuel_liters =
            (state.fuel_liters + state.pit.service_fuel_l).min(config.fuel.capacity_liters);
        if state.pit.service_repair {
            // The hits, the heat and the missed shifts: not the wear.
            state.damage = state.damage.repaired();
        }
        if state.pit.service_discs {
            crate::brakes::fit_discs(state);
        } else if state.pit.service_pads {
            crate::brakes::fit_pads(state);
        }
        // A new stint's energy (`crate::hybrid`).
        crate::hybrid::new_stint(state, &config);
        let pit = &mut state.pit;
        let pad_set = pit.service_pad_set.take();
        pit.service_pads = false;
        pit.service_discs = false;
        pit.service_tyres = false;
        pit.servicing = false;
        pit.service_left_s = 0.0;
        pit.service_repair = false;
        pit.serviced = true;
        pit.wants_stop = false;
        pit.fuel_only = false;
        pit.requested = false;
        pit.stops += 1;
        // New pads of another set: the setup's pad knob moves to it, and
        // the car is simulated with it from here.
        if let Some(click) = pad_set {
            let mut setup = self.car_setup(player_id);
            if setup.brake_pads != click {
                setup.brake_pads = click;
                self.set_car_setup(player_id, setup);
            }
        }
    }

    /// In a race, each AI driver's decision to stop (`pit::plan_stop`), and
    /// its turn onto the pit route once the lane's entry is near: the
    /// automatic gearbox on for the route, `pit::drive_input` driving.
    fn plan_ai_stops(&mut self) {
        if self.session.game_mode != GameMode::Race {
            return;
        }
        let line_water = self
            .track_config
            .road_state
            .as_ref()
            .map_or(self.track_config.track_surface.water, |r| {
                r.mean_line_water()
            });
        self.line_drying.sample(
            self.session.current_tick,
            line_water,
            self.tick_rate_hz as u32,
        );
        let line_snow = crate::road_state::RoadState::line_snow_of(&self.track_config);
        let Some((entry, approach)) = self
            .track_config
            .pit_lane
            .as_ref()
            .map(|lane| (lane.entry_station_m, crate::pit::AI_PIT_APPROACH_M))
        else {
            return;
        };
        let total = crate::laps::track_length_m(&self.track_config);
        let mut ai_ids = self.session.ai_player_ids.clone();
        if self.debug.stand_in_driver {
            // A stand-in driver plans its stops like any AI.
            ai_ids.extend(
                self.session
                    .participants
                    .keys()
                    .filter(|id| !self.is_ai_player(id))
                    .copied(),
            );
        } else {
            // So does the server driving an away driver's car: a long race
            // does not run its tank dry while they are gone.
            ai_ids.extend(self.away.iter().copied());
        }
        for ai_id in ai_ids {
            let Some(state) = self.session.participants.get(&ai_id) else {
                continue;
            };
            // A retired car is towed, not driven in (`update_retirements`).
            if state.finish_position.is_some()
                || state.pit.driving
                || state.pit.servicing
                || !state.damage.is_drivable
            {
                continue;
            }
            // A penalty to serve: in at the next pass of the lane.
            let penalty = self.owed_penalty(&ai_id);
            if penalty.is_some() && !state.pit.wants_stop {
                if let Some(state) = self.session.participants.get_mut(&ai_id) {
                    state.pit.wants_stop = true;
                    state.pit.fuel_only = false;
                    state.pit.next_compound = state.tyre_compound;
                }
                continue;
            }
            if !state.pit.wants_stop {
                let laps_left = self.laps_left(state);
                let car_id = state.car_config_id;
                // Short of fuel is no reason to stop in a car the rules do
                // not refuel (an F1): the stop would add none, and the car
                // would come in again every lap.
                let refuels = self.refuels(car_id);
                let lap_fuel = self.lap_fuel_liters(car_id).filter(|_| refuels);
                let Some(state) = self.session.participants.get(&ai_id) else {
                    continue;
                };
                // The weather first: rain on slicks, a dry line on treaded
                // tyres (`pit::tyres_for_the_track`).
                let crossover = self.crossover_context(state, laps_left, line_snow);
                let for_the_track = self.car_configs.get(&car_id).and_then(|c| {
                    crate::pit::tyres_for_the_track(
                        &c.tire_config,
                        state.tyre_compound,
                        line_water,
                        &crossover,
                    )
                });
                if let Some(compound) = for_the_track.filter(|_| laps_left > 0) {
                    if let Some(state) = self.session.participants.get_mut(&ai_id) {
                        state.pit.wants_stop = true;
                        state.pit.fuel_only = false;
                        state.pit.next_compound = compound;
                    }
                    continue;
                }
                let usable = self
                    .car_configs
                    .get(&car_id)
                    .map_or(state.fuel_liters, |c| {
                        crate::pit::usable_fuel_liters(state, c)
                    });
                let ctx = crate::pit::StopContext {
                    usable_fuel_l: Some(usable),
                    ..self.stop_context(state, laps_left, lap_fuel, total)
                };
                if let Some(plan) = crate::pit::plan_stop(state, &ctx) {
                    // In the rain or on snow the tyre is the weather's;
                    // either way one of the car's own.
                    let tyre = self
                        .car_configs
                        .get(&car_id)
                        .map(|c| c.tire_config.clone())
                        .unwrap_or_default();
                    let weather = crate::pit::snow_tyres(&tyre, state.tyre_compound, line_snow)
                        .or_else(|| tyre.weather_compound(line_water));
                    let compound = weather.unwrap_or_else(|| {
                        let want = tyre.compound_for_click(plan.pick.click());
                        if ctx.compound_owed {
                            crate::pit::other_dry_compound(&tyre, state.tyre_compound, want)
                        } else {
                            want
                        }
                    });
                    if let Some(state) = self.session.participants.get_mut(&ai_id) {
                        state.pit.wants_stop = true;
                        state.pit.fuel_only = plan.fuel_only;
                        state.pit.next_compound = compound;
                    }
                }
                continue;
            }
            let to_entry = (entry - state.track_progress).rem_euclid(total.max(1.0));
            // Not into a lane closed under the safety car: the next lap.
            if to_entry <= approach && !self.race_control.pit_closed() {
                if let Some(state) = self.session.participants.get_mut(&ai_id) {
                    state.pit.driving = true;
                    state.pit.serviced = false;
                    state.auto_gearbox = true;
                }
            }
        }
    }

    /// The wind this tick, gusting about the session's mean
    /// (`crate::wind::gusting`), on the session's own track.
    fn update_wind(&mut self) {
        let clock = car_step::StepClock {
            tick: self.session.current_tick,
            tick_rate_hz: self.tick_rate_hz,
        };
        car_step::air_for_tick(&mut self.track_config, clock);
    }

    /// The DRS rule for this tick (`crate::drs`): which cars may open the
    /// flap where they are now. Before the physics, so the input the AI
    /// generated against last tick's `drs_allowed` and the human's button
    /// both meet an up-to-date answer.
    fn update_drs(&mut self) {
        crate::drs::update(
            &mut self.session.participants,
            &self.car_configs,
            &self.track_config,
            self.session.game_mode,
            self.session.conditions.weather,
        );
    }

    /// Input from `profile` driving `state` among the rest of the field.
    fn ai_input_for(
        &self,
        profile: &AiDriverProfile,
        state: &CarState,
        car_config: &CarConfig,
    ) -> PlayerInputData {
        // On the pit route the lane drives it (`crate::pit`), from just short
        // of where the lane leaves the track; on the run-up to there the car
        // races on along the road, held to a speed it can take the lane at.
        let mut run_up = None;
        if state.pit.driving {
            if let Some(lane) = &self.track_config.pit_lane {
                let total = crate::laps::track_length_m(&self.track_config);
                match crate::pit::run_up_m(lane, state, total) {
                    Some(to_lane) => run_up = Some((lane, to_lane)),
                    None => {
                        return crate::pit::drive_input(
                            lane,
                            lane.box_at(state.pit.box_index.unwrap_or(0)),
                            state,
                            car_config,
                            &self.lane_traffic(lane, state),
                            // The AI's racing line knows nothing of the
                            // exit taper: it slows for every bend ahead.
                            f32::INFINITY,
                        );
                    }
                }
            }
        }
        // Saving fuel, and nursing a hot or damaged engine: the more of
        // the two.
        let fuel_save = self.fuel_save(state).max(self.engine_nurse(state));
        let controller = AiDriverController::new(profile, &self.track_config, car_config)
            .with_speed_profile(self.ai_speed_profile(state))
            .with_fuel_save(fuel_save.coast_s)
            .with_short_shift(fuel_save.short_shift);
        // BTreeMap order: the traffic list, and so the input, is
        // deterministic.
        let traffic: Vec<TrafficCar> = self
            .session
            .participants
            .values()
            .filter(|other| other.player_id != state.player_id)
            .filter_map(|other| {
                let config = self.car_configs.get(&other.car_config_id)?;
                Some(TrafficCar {
                    state: other,
                    length_m: config.length_m,
                    width_m: config.width_m,
                })
            })
            .collect();
        let mut input = controller.generate_input_in_traffic(
            state,
            &traffic,
            self.session.current_tick,
            self.tick_rate_hz,
        );
        // The hybrid: the overtake button to attack or defend, harvesting
        // when low and alone (`hybrid::ai_ers`).
        if car_config.hybrid.enabled {
            let (mode, boost) = self.ai_ers(state, car_config);
            input.ers_mode = Some(mode as u8);
            input.ers_boost = boost;
        }
        if let Some((lane, to_lane)) = run_up {
            input = crate::pit::run_up_input(lane, to_lane, state.speed_mps, input);
        }
        input
    }

    /// What an AI weighs a change of tyres for the weather against
    /// (`pit::tyres_for_the_track`): the race left, the rain falling, the
    /// forecast (`conditions::LiveConditions::changes_after`), the snow on
    /// the line (`line_snow`) and its nerve.
    fn crossover_context(
        &self,
        state: &CarState,
        laps_left: u32,
        line_snow: f32,
    ) -> crate::pit::CrossoverContext {
        let rate = self.tick_rate_hz.max(1) as f32;
        let now_s = self.session.current_tick as f32 / rate;
        let wet = |w: Weather| w.is_wet() || w.is_snow();
        let raining = self.sky.rain > 0.05 || self.sky.snowfall > 0.05;
        let turn_in_s = self
            .sky
            .changes_after(now_s, 16)
            .find(|c| wet(c.weather) != raining)
            .map(|c| c.at_s - now_s);
        let lap_s = self.own_lap_s(state);
        // How hard it will rain: the sky now, and the forecast's within
        // its reach.
        let horizon_s = crate::pit::FORECAST_LAPS * lap_s;
        let rain_ahead = self
            .sky
            .changes_after(now_s, 16)
            .take_while(|c| c.at_s - now_s <= horizon_s)
            .map(|c| c.weather.rainfall())
            .fold(self.sky.rain, f32::max);
        // What the rest of the running field is on.
        let treaded = |s: &CarState| {
            self.car_configs.get(&s.car_config_id).map(|c| {
                c.tire_config.compound(s.tyre_compound).kind != tyre_thermal::CompoundKind::Slick
            })
        };
        let (on_treaded, running) = self
            .session
            .participants
            .values()
            .filter(|s| {
                s.player_id != state.player_id
                    && !s.in_garage
                    && s.damage.is_drivable
                    && s.finish_position.is_none()
            })
            .filter_map(treaded)
            .fold((0u32, 0u32), |(t, n), on| (t + on as u32, n + 1));
        let field_treaded = (running > 0).then(|| on_treaded as f32 / running as f32);
        let (aggressiveness, roll) = match self.ai_profiles.get(&state.player_id) {
            Some(profile) => {
                let (hi, lo) = state.player_id.as_u64_pair();
                (
                    profile.aggressiveness,
                    crate::wind::hash01(
                        hi ^ lo.rotate_left(13),
                        CROSSOVER_SALT ^ state.current_lap as u64,
                    ),
                )
            }
            None => (0.0, 1.0),
        };
        crate::pit::CrossoverContext {
            laps_left,
            raining,
            turn_in_s,
            lap_s,
            aggressiveness,
            roll,
            rain_ahead,
            drying_per_s: self.line_drying.per_s,
            field_treaded,
            line_snow,
        }
    }

    /// A lap of `state`'s car, s: its last, or its car's planned lap.
    fn own_lap_s(&self, state: &CarState) -> f32 {
        state
            .last_lap_time_ms
            .filter(|ms| *ms > 0)
            .map(|ms| ms as f32 / 1000.0)
            .or_else(|| self.lap_seconds.get(&state.car_config_id).copied())
            .unwrap_or(FALLBACK_LAP_SECONDS)
    }

    /// Whether a stop now would put `state`'s car back on the road in
    /// traffic (`pit::rejoins_in_traffic`): the stop's cost
    /// (`pit::pit_loss_s`, with the fuel to the flag a refuelled car takes)
    /// against how far behind it on the road, at its pace, each other car
    /// running out of the lane is.
    fn rejoins_in_traffic(
        &self,
        state: &CarState,
        laps_left: u32,
        lap_fuel_l: Option<f32>,
        total_m: f32,
    ) -> bool {
        let (Some(lane), Some(config)) = (
            self.track_config.pit_lane.as_ref(),
            self.car_configs.get(&state.car_config_id),
        ) else {
            return false;
        };
        if total_m <= 0.0 {
            return false;
        }
        let lap_s = self.own_lap_s(state).max(1.0);
        let room = (config.fuel.capacity_liters - state.fuel_liters).max(0.0);
        let fuel_l = lap_fuel_l.map_or(0.0, |lap| {
            (lap * (laps_left as f32 + 1.0) - state.fuel_liters).clamp(0.0, room)
        });
        let service_s = crate::pit::service_seconds(config, fuel_l, 0.0);
        let loss_s = crate::pit::pit_loss_s(lane, total_m, lap_s, service_s);
        let pace = total_m / lap_s;
        let behind = self
            .session
            .participants
            .values()
            .filter(|o| {
                o.player_id != state.player_id
                    && !o.in_garage
                    && !o.pit.in_lane
                    && o.damage.is_drivable
                    && o.finish_position.is_none()
            })
            .map(|o| (state.track_progress - o.track_progress).rem_euclid(total_m) / pace);
        crate::pit::rejoins_in_traffic(loss_s, behind)
    }

    /// What an AI weighs a stop against (`pit::plan_stop`): the race left,
    /// its fuel, how long its tyres have run, and the cars around it.
    fn stop_context(
        &self,
        state: &CarState,
        laps_left: u32,
        lap_fuel_l: Option<f32>,
        total_m: f32,
    ) -> crate::pit::StopContext {
        let field: Vec<(PlayerId, f32)> = self
            .session
            .participants
            .values()
            .filter(|s| !s.in_garage && s.damage.is_drivable && s.finish_position.is_none())
            .map(|s| (s.player_id, s.track_progress))
            .collect();
        let gap_ahead_s = if total_m > 0.0 {
            crate::drs::gap_ahead_s(state, &field, total_m)
        } else {
            None
        };
        // A car close behind on the road that has turned in for its stop.
        let rival_behind_stopping = total_m > 0.0
            && self.session.participants.values().any(|other| {
                other.player_id != state.player_id && other.pit.driving && !other.pit.serviced && {
                    let behind = (state.track_progress - other.track_progress).rem_euclid(total_m);
                    behind > 0.0 && behind <= crate::pit::AI_COVER_GAP_S * other.speed_mps.max(1.0)
                }
            });
        let (aggressiveness, roll) = match self.ai_profiles.get(&state.player_id) {
            Some(profile) => {
                let (hi, lo) = state.player_id.as_u64_pair();
                (
                    profile.aggressiveness,
                    crate::wind::hash01(
                        hi ^ lo.rotate_left(29),
                        STRATEGY_SALT ^ state.current_lap as u64,
                    ),
                )
            }
            None => (0.0, 1.0),
        };
        let (stops_owed, compound_owed) = self.stops_owed(state);
        crate::pit::StopContext {
            laps_left,
            lap_fuel_l,
            usable_fuel_l: None,
            save_to_flag: lap_fuel_l.is_some() && self.saves_to_flag(state),
            stops_owed,
            compound_owed,
            rejoin_traffic: self.rejoins_in_traffic(state, laps_left, lap_fuel_l, total_m),
            neutralised: self.race_control.course != crate::race_control::Course::Green,
            stint_laps: laps_run(state, total_m) - state.pit.stint_start_laps,
            gap_ahead_s,
            rival_behind_stopping,
            aggressiveness,
            roll,
        }
    }

    /// How an AI in a race saves fuel to reach the flag on what it carries
    /// (`crate::pit::fuel_save_for`): nothing while the tank will do, or
    /// when the car will be refuelled at a stop (the pit plan's business);
    /// past that, coasting into the braking zones and then short-shifting.
    fn fuel_save(&self, state: &CarState) -> crate::pit::FuelSave {
        if self.session.game_mode != GameMode::Race
            || self.session.state != SessionState::Racing
            || state.finish_position.is_some()
        {
            return crate::pit::FuelSave::NONE;
        }
        // Refuelled at a stop, or at the line where there is no lane
        // (`update_line_refuel`): nothing to save.
        if self.refuels(state.car_config_id)
            && (self.track_config.pit_lane.is_none()
                || state.pit.wants_stop
                || !self.saves_to_flag(state))
        {
            return crate::pit::FuelSave::NONE;
        }
        crate::pit::fuel_save_for(self.fuel_short_share(state))
    }

    /// Without a pit lane nothing can refuel a car the race outruns (the
    /// Nordschleife): a car a stop would refuel (`refuels`) is topped up
    /// as it starts each lap instead, to what reaches the flag with the
    /// race margin and reserve (`RACE_FUEL_MARGIN`,
    /// `RACE_FUEL_RESERVE_LAPS`), never over its tank, and loses no time
    /// for it: the stop it could not make. A car that reaches the flag on
    /// its start fill is never touched, and one the rules do not refuel
    /// saves fuel as it would anywhere (`fuel_save`).
    fn update_line_refuel(&mut self) {
        if self.track_config.pit_lane.is_some()
            || self.session.game_mode != GameMode::Race
            || self.session.state != SessionState::Racing
        {
            return;
        }
        let due: Vec<(PlayerId, CarConfigId, u16, f32)> = self
            .session
            .participants
            .values()
            .filter(|s| s.finish_position.is_none() && s.current_lap >= 1)
            .filter(|s| self.line_refuel_laps.get(&s.player_id) != Some(&s.current_lap))
            .filter(|s| self.refuels(s.car_config_id))
            .filter_map(|s| {
                let lap = *self.lap_fuel.get(&s.car_config_id)?;
                let laps = self.laps_left(s) as f32 + 1.0;
                let needed = (laps * (1.0 + RACE_FUEL_MARGIN) + RACE_FUEL_RESERVE_LAPS) * lap;
                Some((s.player_id, s.car_config_id, s.current_lap, needed))
            })
            .collect();
        for (player_id, car_id, lap, needed) in due {
            let capacity = self
                .car_configs
                .get(&car_id)
                .map_or(0.0, |c| c.fuel.capacity_liters.max(0.0));
            if let Some(state) = self.session.participants.get_mut(&player_id) {
                state.fuel_liters = state.fuel_liters.max(needed.min(capacity));
            }
            self.line_refuel_laps.insert(player_id, lap);
        }
    }

    /// How an AI nurses its engine (`crate::pit::engine_nurse_for`): by
    /// its coolant and the damage it feels (the hits, the heat and the
    /// over-revs; its wear is how every engine ages). Racing or not, but
    /// not once it has finished.
    fn engine_nurse(&self, state: &CarState) -> crate::pit::FuelSave {
        if state.finish_position.is_some() {
            return crate::pit::FuelSave::NONE;
        }
        let felt = state.felt_damage();
        crate::pit::engine_nurse_for(
            state.water_temp_c,
            felt.engine_damage_percent - felt.engine_wear_percent,
        )
    }

    /// Whether a car a stop would refuel saves fuel to the flag instead:
    /// no more than `pit::SAVE_TO_SKIP_SHORT` short, on tyres that reach
    /// it (`pit::tyres_reach_flag`), so the stop is saved.
    fn saves_to_flag(&self, state: &CarState) -> bool {
        let total_m = crate::laps::track_length_m(&self.track_config);
        let short = self.fuel_short_share(state);
        // A car the pit rule stops anyway takes its fuel there.
        self.stops_owed(state) == (0, false)
            && short > 0.0
            && short <= crate::pit::SAVE_TO_SKIP_SHORT
            && crate::pit::tyres_reach_flag(
                state,
                laps_run(state, total_m) - state.pit.stint_start_laps,
                self.laps_left(state),
            )
    }

    /// The share of the fuel to the flag a car in a race is short of, 0
    /// while the tank will do (or with no lap estimate).
    fn fuel_short_share(&self, state: &CarState) -> f32 {
        let Some(&lap) = self.lap_fuel.get(&state.car_config_id) else {
            return 0.0;
        };
        let total = crate::laps::track_length_m(&self.track_config).max(1.0);
        let this_lap = 1.0 - (state.track_progress / total).clamp(0.0, 1.0);
        let needed = lap * (self.laps_left(state) as f32 + this_lap);
        if needed <= 0.0 || state.fuel_liters >= needed {
            return 0.0;
        }
        1.0 - state.fuel_liters / needed
    }

    /// Whether a stop refuels `car_id` in this session: by its rules
    /// (`crate::pit::refuelling_allowed`), and in a race its tank cannot
    /// reach the flag on even saving all a driver can
    /// (`crate::pit::MAX_FUEL_SAVE_SHORT`), as a series racing that far
    /// would have it: an F1 in a three-hour race. Read from the lap estimate
    /// the start's fill worked out; without one, the rules alone.
    pub fn refuels(&self, car_id: CarConfigId) -> bool {
        let Some(car) = self.car_configs.get(&car_id) else {
            return false;
        };
        if crate::pit::refuelling_allowed(car) {
            return true;
        }
        if self.planned_mode() != GameMode::Race {
            return false;
        }
        let Some(&lap) = self.lap_fuel.get(&car_id) else {
            return false;
        };
        let needed = self.race_laps(car_id) * lap;
        let capacity = car.fuel.capacity_liters.max(0.0);
        needed > 0.0 && 1.0 - capacity / needed > crate::pit::MAX_FUEL_SAVE_SHORT
    }

    /// An AI's hybrid mode and overtake button: Balanced and the button
    /// off before its second lap of a race, outside a race and on the pit
    /// route; past that `hybrid::ai_ers` on its charge and the gaps to the
    /// cars ahead and behind.
    fn ai_ers(&self, state: &CarState, config: &CarConfig) -> (crate::hybrid::ErsMode, bool) {
        let balanced = (crate::hybrid::ErsMode::Balanced, false);
        if !crate::drs::race_rules(self.session.game_mode)
            || state.current_lap < crate::drs::DRS_RACE_FROM_LAP
            || state.pit.driving
        {
            return balanced;
        }
        let total = crate::laps::track_length_m(&self.track_config);
        if total <= 0.0 {
            return balanced;
        }
        let field: Vec<(PlayerId, f32, f32)> = self
            .session
            .participants
            .values()
            .filter(|s| !s.in_garage && s.damage.is_drivable)
            .map(|s| (s.player_id, s.track_progress, s.speed_mps))
            .collect();
        let ahead: Vec<(PlayerId, f32)> = field.iter().map(|(id, p, _)| (*id, *p)).collect();
        let charge = state.hybrid_battery_kwh / config.hybrid.battery_capacity_kwh.max(1e-6);
        // The straight it is on, or coming onto, against the lap's longest.
        let straight_share = self.ai_speed_profile(state).and_then(|plan| {
            let here = crate::racecraft::locate(plan, state, total)?;
            self.ai_straights.get(&state.car_config_id)?.share_at(here)
        });
        crate::hybrid::ai_ers(
            charge,
            crate::drs::gap_ahead_s(state, &ahead, total),
            crate::drs::gap_behind_s(state, &field, total),
            straight_share,
        )
    }

    /// Generate AI input for a player using their AI profile.
    ///
    /// Returns default input if the player is not an AI or has no profile.
    pub fn generate_ai_input(&self, player_id: &PlayerId) -> PlayerInputData {
        // Check if this player has an AI profile
        if let Some(profile) = self.ai_profiles.get(player_id) {
            if let Some(state) = self.session.participants.get(player_id) {
                // Get the car config for this AI player
                if let Some(car_config) = self.car_configs.get(&state.car_config_id) {
                    return self.ai_input_for(profile, state, car_config);
                }
            }
        }

        // Fallback: no AI profile found, return default (coasting)
        PlayerInputData::default()
    }

    /// Check if a player is an AI driver.
    pub fn is_ai_player(&self, player_id: &PlayerId) -> bool {
        self.ai_profiles.contains_key(player_id)
    }

    /// Get the AI profile for a player, if they are an AI.
    pub fn get_ai_profile(&self, player_id: &PlayerId) -> Option<&AiDriverProfile> {
        self.ai_profiles.get(player_id)
    }

    /// The livery a driver wears, as they picked it. Clamped to the car's
    /// own list when the roster is built, so a stale pick is harmless.
    pub fn set_livery(&mut self, player_id: PlayerId, livery: u8) {
        let old = self.liveries.insert(player_id, livery).unwrap_or(0);
        if old != livery {
            self.roster_dirty = true;
        }
    }

    /// Whether session membership changed since the last roster broadcast.
    pub fn roster_is_dirty(&self) -> bool {
        self.roster_dirty
    }

    /// Send the roster again on the next tick, to everyone in the session:
    /// a spectator who joins a session already under way has never had it,
    /// and its telemetry means nothing without it.
    pub fn mark_roster_dirty(&mut self) {
        self.roster_dirty = true;
    }

    /// True once per membership change: returns whether a fresh roster needs
    /// broadcasting and clears the flag.
    pub fn take_roster_dirty(&mut self) -> bool {
        std::mem::take(&mut self.roster_dirty)
    }

    /// The car-index → player mapping for compact telemetry, a row a car
    /// in `participants` order (the wire's `SessionRoster`). Human player
    /// names are looked up in `names` (lobby data); AI names come from
    /// their profiles.
    pub fn roster_rows(&self, names: &HashMap<PlayerId, String>) -> Vec<RosterRow> {
        // AI drivers are dealt liveries in grid order, each model's cars
        // taking the next one of its list, so a field of the same car is not
        // a row of clones.
        let mut dealt: HashMap<CarConfigId, u8> = HashMap::new();
        let slots = self.starting_slots();
        self.session
            .participants
            .iter()
            .enumerate()
            .map(|(idx, (player_id, state))| {
                let livery_count = self
                    .car_configs
                    .get(&state.car_config_id)
                    .map_or(0, |c| c.livery_names.len().min(u8::MAX as usize - 1) as u8);
                let is_ai = self.ai_profiles.contains_key(player_id);
                let player_name = self
                    .ai_profiles
                    .get(player_id)
                    .map(|p| p.name.clone())
                    .or_else(|| names.get(player_id).cloned())
                    .unwrap_or_else(|| format!("Player-{}", &player_id.to_string()[..8]));
                let livery = if is_ai {
                    let next = dealt.entry(state.car_config_id).or_insert(0);
                    let pick = *next % (livery_count + 1);
                    *next = next.wrapping_add(1);
                    pick
                } else {
                    let asked = self.liveries.get(player_id).copied().unwrap_or(0);
                    if asked <= livery_count {
                        asked
                    } else {
                        0
                    }
                };
                RosterRow {
                    car_index: idx as u8,
                    player_id: *player_id,
                    player_name,
                    is_ai,
                    car_config_id: state.car_config_id,
                    livery,
                    away: self.away.contains(player_id),
                    grid_slot: slots.get(player_id).copied().unwrap_or(state.grid_position),
                    ready: self.is_ready(player_id),
                    dnq: self.not_qualified.contains(player_id),
                    missed_stops: self
                        .race_end
                        .finishers
                        .get(player_id)
                        .is_some_and(|f| f.missed_stops),
                    qualifying_laps: self
                        .qualifying_laps_run
                        .get(player_id)
                        .copied()
                        .unwrap_or(0),
                    penalties: self.penalties.get(player_id).cloned().unwrap_or_default(),
                }
            })
            .collect()
    }

    /// Race is complete once every car has been classified with a finish
    /// position (i.e. completed the race distance).
    fn is_race_complete(&self) -> bool {
        if self.session.participants.is_empty() {
            return false;
        }
        // The winner's finish started the clock on everyone else.
        if self
            .finish_deadline_tick
            .is_some_and(|deadline| self.session.current_tick >= deadline)
        {
            return true;
        }

        // A car counts as done when it is classified (finished the race
        // distance) OR is a DNF (undrivable — it can never finish, and must
        // not keep the race running forever).
        self.session
            .participants
            .values()
            .all(|s| s.finish_position.is_some() || !s.damage.is_drivable)
    }

    /// Classify the cars that just took the flag: those completing the
    /// winning lap (`finish_lap`) and, in a timed race once the winner is
    /// in, every car crossing the line after it, a lap down or not. The
    /// order is laps completed, then who crossed first (the same tick: who
    /// was further past the line; then deterministic BTreeMap order), so
    /// in a race over laps a car's place never changes once it has one,
    /// while in a timed race a lapped car that crossed early drops behind a
    /// car on the lead lap that crosses after it. Under a pit rule
    /// (`data::PitRule`) a car that takes the flag owing a stop or its
    /// second compound is classified behind every finisher that met it,
    /// however many laps it ran (`RosterEntry::missed_stops`).
    fn assign_finish_positions(&mut self) {
        let Some(finish_lap) = self.finish_lap() else {
            return;
        };
        let tick = self.session.current_tick;
        // A car taking the flag owing a drive-through or stop-go has it
        // added as time.
        let mut converted = false;
        for (id, list) in self.penalties.iter_mut() {
            let crossing = self.session.participants.get(id).is_some_and(|s| {
                s.finish_position.is_none()
                    && (s.current_lap > finish_lap
                        || self
                            .race_end
                            .flag_laps
                            .get(id)
                            .is_some_and(|lap| s.current_lap > *lap))
            });
            if crossing {
                for penalty in list.iter_mut() {
                    if penalty.status == crate::race_control::PenaltyStatus::Owed {
                        penalty.convert();
                        converted = true;
                    }
                }
            }
        }
        if converted {
            self.roster_dirty = true;
        }
        let winner_was_in = !self.race_end.finishers.is_empty();
        let flag_laps = &self.race_end.flag_laps;
        let rate = self.tick_rate_hz as u32;
        let new_finishers: Vec<(PlayerId, Finisher)> = self
            .session
            .participants
            .iter()
            .filter(|(id, s)| {
                s.finish_position.is_none()
                    && (s.current_lap > finish_lap
                        || flag_laps.get(id).is_some_and(|lap| s.current_lap > *lap))
            })
            .map(|(id, s)| {
                let (stops_owed, compound_owed) = self.stops_owed(s);
                let finisher = Finisher {
                    laps: s.current_lap.saturating_sub(1),
                    tick,
                    progress: s.track_progress,
                    missed_stops: stops_owed > 0 || compound_owed,
                    penalty_ticks: self.penalty_seconds(id) * rate,
                };
                (*id, finisher)
            })
            .collect();

        if new_finishers.is_empty() {
            return;
        }
        if new_finishers.iter().any(|(_, f)| f.missed_stops) {
            self.roster_dirty = true;
        }
        self.race_end
            .finishers
            .extend(new_finishers.iter().copied());

        let mut order: Vec<(PlayerId, Finisher)> = self
            .race_end
            .finishers
            .iter()
            .filter(|(id, _)| self.session.participants.contains_key(id))
            .map(|(id, f)| (*id, *f))
            .collect();
        // Stable: exact ties keep BTreeMap order. A car that missed the
        // pit rule goes behind every car that met it; a penalty's time is
        // added to when a car crossed the line.
        order.sort_by(|(_, a), (_, b)| {
            a.missed_stops
                .cmp(&b.missed_stops)
                .then(b.laps.cmp(&a.laps))
                .then((a.tick + a.penalty_ticks).cmp(&(b.tick + b.penalty_ticks)))
                .then(
                    b.progress
                        .partial_cmp(&a.progress)
                        .unwrap_or(std::cmp::Ordering::Equal),
                )
        });
        for (place, (player_id, _)) in order.iter().enumerate() {
            if let Some(state) = self.session.participants.get_mut(player_id) {
                state.finish_position = Some((place + 1).min(u8::MAX as usize) as u8);
            }
        }

        if !winner_was_in {
            // The winner is in: the rest of the field is on the clock.
            let race_ticks = tick.saturating_sub(self.session.race_start_tick.unwrap_or(0));
            let average_lap_ticks = race_ticks as f32 / finish_lap.max(1) as f32;
            let grace_ticks = ((average_lap_ticks * FINISH_GRACE_LAPS) as u32)
                .max(FINISH_GRACE_MIN_SECONDS * self.tick_rate_hz as u32);
            self.finish_deadline_tick = Some(tick + grace_ticks);
            // A timed race flags everyone at their next crossing.
            if self.session.race_seconds.is_some() {
                self.race_end.flag_laps = self
                    .session
                    .participants
                    .iter()
                    .filter(|(_, s)| s.finish_position.is_none())
                    .map(|(id, s)| (*id, s.current_lap))
                    .collect();
            }
        }

        // Humans who just finished hand their car to the cool-down driver,
        // which steers the rack directly and plans on the car's own speeds.
        for (player_id, _) in &new_finishers {
            if self.is_ai_player(player_id) {
                continue;
            }
            let Some(state) = self.session.participants.get_mut(player_id) else {
                continue;
            };
            self.held_steering_assist
                .insert(*player_id, state.steering_assist);
            state.steering_assist = false;
            let car_id = state.car_config_id;
            self.plan_ai_speeds(car_id);
        }
    }

    /// Build the AI's speed profiles for `car_id`, one per step of its tank
    /// (see `ai_speed_profiles`), unless they are built.
    fn plan_ai_speeds(&mut self, car_id: CarConfigId) {
        if self.ai_speed_profiles.contains_key(&car_id) {
            return;
        }
        let Some(car) = self.car_configs.get(&car_id) else {
            return;
        };
        let capacity = car.fuel.capacity_liters.max(0.0);
        let plans: Option<Vec<RacingLineProfile>> = (0..=AI_FUEL_PLAN_STEPS)
            .map(|step| {
                let fuel = capacity * step as f32 / AI_FUEL_PLAN_STEPS as f32;
                racing_line::build_laden(&self.track_config, car, fuel)
            })
            .collect();
        if let Some(plans) = plans {
            if let Some(full) = plans.last() {
                self.ai_straights
                    .insert(car_id, crate::hybrid::straights(full));
            }
            self.ai_speed_profiles.insert(car_id, plans);
        }
    }

    /// A human driver's racing line aid planned again for their car as it
    /// is at the line: their setup, the next step of the tank above what
    /// they carry, and what their tyres and the road under them are worth
    /// (`CarState::tyre_grip_share`). `None` when nothing has moved enough
    /// since the line they have, or the session does not allow the aid.
    /// Called at each lap's end, so it costs one plan a lap at most.
    pub fn refreshed_racing_line(&mut self, player_id: &PlayerId) -> Option<RacingLineProfile> {
        if !self.session.allowed_assists.racing_line || self.is_ai_player(player_id) {
            return None;
        }
        let state = self.session.participants.get(player_id)?;
        let car_id = state.car_config_id;
        let grip = state.tyre_grip_share();
        let held = self
            .aid_lines
            .get(player_id)
            .copied()
            .unwrap_or(AidLineKey::joined(self.sky_day));
        // Hysteresis: the grip moves the key only once it has left the
        // held step by a whole step.
        let grip_steps = if (grip / AID_GRIP_STEP - held.grip_steps as f32).abs() >= 1.0 {
            (grip / AID_GRIP_STEP).round() as i32
        } else {
            held.grip_steps
        };
        let key = AidLineKey {
            setup: setup_key(&self.car_setup(player_id)),
            fuel_step: self.fuel_plan_step(car_id, state.fuel_liters),
            grip_steps,
            day: self.sky_day,
        };
        if key == held {
            return None;
        }
        let car = self.simulated_config_for(player_id)?;
        let fuel =
            car.fuel.capacity_liters.max(0.0) * key.fuel_step as f32 / AI_FUEL_PLAN_STEPS as f32;
        let profile = racing_line::build_driven(
            &self.track_config,
            car,
            fuel,
            key.grip_steps as f32 * AID_GRIP_STEP,
        )?;
        self.aid_lines.insert(*player_id, key);
        Some(profile)
    }

    /// Whether a weekend day has baked its own sky since the last call: the
    /// game loop then plans every human's racing line again
    /// ([`Self::refreshed_racing_line`]) instead of waiting for their lap.
    pub fn take_day_lines_due(&mut self) -> bool {
        std::mem::take(&mut self.day_lines_due)
    }

    /// The joining line was sent to `player_id` (stock car, dry tank): the
    /// next lap's end plans theirs again from there.
    pub fn note_joining_racing_line(&mut self, player_id: &PlayerId) {
        self.aid_lines.remove(player_id);
    }

    /// The step of `car_id`'s tank a car carrying `fuel_liters` is planned
    /// at: the next one at or above it.
    fn fuel_plan_step(&self, car_id: CarConfigId, fuel_liters: f32) -> usize {
        let capacity = self
            .car_configs
            .get(&car_id)
            .map_or(0.0, |c| c.fuel.capacity_liters);
        let step = if capacity > 0.0 {
            (fuel_liters / capacity * AI_FUEL_PLAN_STEPS as f32).ceil() as usize
        } else {
            0
        };
        step.min(AI_FUEL_PLAN_STEPS)
    }

    /// The AI's speed profile for `state`: the one planned at the next step
    /// of the tank at or above what it carries, and for the power its
    /// engine has left (`plan_damaged_ai_speeds`) once damage has cost it
    /// any.
    fn ai_speed_profile(&self, state: &CarState) -> Option<&RacingLineProfile> {
        let car_id = state.car_config_id;
        let plans = self.ai_speed_profiles.get(&car_id)?;
        let step = self.fuel_plan_step(car_id, state.fuel_liters);
        let lost = power_steps_lost(state);
        if lost > 0 {
            if let Some(plan) = self.ai_power_profiles.get(&(car_id, lost, step)) {
                return Some(plan);
            }
        }
        plans.get(step.min(plans.len().saturating_sub(1)))
    }

    /// Build the plans the AI needs for engines that damage has cost power
    /// ([`AI_POWER_PLAN_STEP`]s of it, `damage::engine_power_factor`), the
    /// car's own with its power cut to what is left: the straights, the
    /// braking points and the coasting it plans from them are the car's as
    /// it is. Each is built once, the tick after the damage.
    fn plan_damaged_ai_speeds(&mut self) {
        let wanted: std::collections::BTreeSet<(CarConfigId, u8, usize)> = self
            .session
            .participants
            .values()
            .filter(|s| self.ai_speed_profiles.contains_key(&s.car_config_id))
            .filter_map(|s| {
                let lost = power_steps_lost(s);
                (lost > 0).then(|| {
                    (
                        s.car_config_id,
                        lost,
                        self.fuel_plan_step(s.car_config_id, s.fuel_liters),
                    )
                })
            })
            .filter(|key| !self.ai_power_profiles.contains_key(key))
            .collect();
        for (car_id, lost, step) in wanted {
            let Some(car) = self.car_configs.get(&car_id) else {
                continue;
            };
            let mut weak = car.clone();
            weak.max_engine_power_w *= (1.0 - lost as f32 * AI_POWER_PLAN_STEP).max(0.0);
            let fuel = car.fuel.capacity_liters.max(0.0) * step as f32 / AI_FUEL_PLAN_STEPS as f32;
            if let Some(plan) = racing_line::build_laden(&self.track_config, &weak, fuel) {
                self.ai_power_profiles.insert((car_id, lost, step), plan);
            }
        }
    }

    /// Spawn AI drivers using the provided profiles.
    ///
    /// AI drivers will be added to the session up to the configured ai_count.
    /// Each AI uses their preferred car (if set) or the default car.
    pub fn spawn_ai_drivers(&mut self) {
        let current_ai_count = self.session.ai_player_ids.len() as u8;
        let ai_to_spawn = self.session.ai_count.saturating_sub(current_ai_count);

        if ai_to_spawn == 0 {
            return;
        }

        // Collect profile data we need before mutating self
        let profiles_to_spawn: Vec<(PlayerId, Option<CarConfigId>)> = self
            .ai_profiles
            .values()
            .filter(|p| !self.session.ai_player_ids.contains(&p.id))
            .take(ai_to_spawn as usize)
            .map(|p| (p.id, p.preferred_car_id))
            .collect();

        // The field races in the host car's class; without a host car, the
        // smallest id anchors it (HashMap order varies per run).
        let anchor = self
            .session
            .host_car_id
            .filter(|id| self.car_configs.contains_key(id))
            .or_else(|| self.car_configs.keys().min().copied());
        let Some(anchor) = anchor else {
            tracing::error!("Cannot spawn AI drivers: no car configurations loaded");
            return;
        };
        let mut field = class_field(&self.car_configs, anchor);
        field.retain(|id| *id == anchor || !self.ai_excluded_cars.contains(id));
        // Carry on the rotation from the AI already on the grid, so a later
        // spawn does not stack the same car again.
        let mut next_car = self.session.ai_player_ids.len();

        for (ai_id, preferred_car) in profiles_to_spawn {
            if self.session.participants.len() >= self.session.max_players as usize {
                break;
            }

            let car_id = preferred_car.unwrap_or_else(|| {
                let car = field[next_car % field.len()];
                next_car += 1;
                car
            });

            if self.add_player(ai_id, car_id).is_some() {
                self.session.ai_player_ids.push(ai_id);
                self.plan_ai_speeds(car_id);
            }
        }
    }

    /// Add AI profiles to the session.
    ///
    /// This should be called when setting up the session in the lobby.
    pub fn set_ai_profiles(&mut self, profiles: Vec<AiDriverProfile>) {
        self.ai_profiles = profiles.into_iter().map(|p| (p.id, p)).collect();
    }
}

/// The cars an AI field is dealt from, in dealing order: every car of
/// `anchor`'s class, ordered by id and starting just after `anchor`, so a
/// GT3 host races a mixed GT3 field with its own model coming round last.
/// A car with no class races only against itself. Never empty: `anchor` is
/// always in it.
pub fn class_field(
    car_configs: &HashMap<CarConfigId, CarConfig>,
    anchor: CarConfigId,
) -> Vec<CarConfigId> {
    let class = car_configs
        .get(&anchor)
        .map(|car| car.class.trim())
        .unwrap_or_default();
    let mut pool: Vec<CarConfigId> = if class.is_empty() {
        vec![anchor]
    } else {
        car_configs
            .values()
            .filter(|car| car.class.trim().eq_ignore_ascii_case(class))
            .map(|car| car.id)
            .collect()
    };
    if !pool.contains(&anchor) {
        pool.push(anchor);
    }
    pool.sort();
    let at = pool.iter().position(|id| *id == anchor).unwrap_or(0);
    let len = pool.len();
    pool.rotate_left((at + 1) % len);
    pool
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn create_test_session() -> GameSession {
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 0, 3);

        GameSession::new(session, track, car_configs)
    }

    /// A car whose engine damage has cost it power drives a plan built for
    /// what it has left: slower on the straights, the same through the
    /// corners.
    #[test]
    fn a_damaged_engine_is_planned_at_the_power_it_has_left() {
        let track = crate::track_loader::TrackLoader::load_from_file(
            "../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
        )
        .expect("Monzarella");
        let car = crate::car_loader::CarLoader::load_from_file(std::path::Path::new(
            "../../apexsim-content/cars/default/posh-gt3rs/car.toml",
        ))
        .expect("car");
        let car_id = car.id;
        let mut car_configs = HashMap::new();
        car_configs.insert(car_id, car);
        let mut profile = AiDriverProfile::new("Plan AI", 95);
        profile.id = Uuid::from_u128(7400);
        profile.preferred_car_id = Some(car_id);
        let driver = profile.id;
        let session = RaceSession::new(
            Uuid::from_u128(1),
            Uuid::nil(),
            SessionKind::Multiplayer,
            8,
            1,
            3,
        );
        let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
        gs.spawn_ai_drivers();
        let top = |gs: &GameSession| {
            let state = &gs.session.participants[&driver];
            gs.ai_speed_profile(state)
                .expect("a plan")
                .speed_mps
                .iter()
                .copied()
                .fold(0.0, f32::max)
        };
        let healthy = top(&gs);
        gs.session
            .participants
            .get_mut(&driver)
            .unwrap()
            .damage
            .engine_damage_percent = 80.0;
        assert_eq!(top(&gs), healthy, "planned at full power until rebuilt");
        gs.plan_damaged_ai_speeds();
        let damaged = top(&gs);
        assert!(
            damaged < healthy - 2.0,
            "a damaged engine's top speed: {damaged:.1} against {healthy:.1} m/s"
        );
        assert_eq!(gs.ai_power_profiles.len(), 1);
        gs.plan_damaged_ai_speeds();
        assert_eq!(gs.ai_power_profiles.len(), 1, "built once");
    }

    #[test]
    fn test_game_session_creation() {
        let game_session = create_test_session();
        assert_eq!(game_session.session.state, SessionState::Lobby);
        assert_eq!(game_session.session.current_tick, 0);
    }

    #[test]
    fn test_add_player() {
        let mut game_session = create_test_session();
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;

        let position = game_session.add_player(player_id, car_id);
        assert!(position.is_some());
        assert_eq!(position.unwrap(), 1);
        assert_eq!(game_session.session.participants.len(), 1);
    }

    #[test]
    fn test_start_countdown() {
        let mut game_session = create_test_session();
        assert!(game_session.start_countdown());

        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(game_session.session.next_mode, Some(GameMode::Race));
        assert!(game_session.session.countdown_ticks_remaining.is_some());
        // A second press changes nothing.
        assert!(!game_session.start_countdown());
    }

    /// `StartSession` counts into a race: after its countdown the session is
    /// racing, not frozen in a countdown with no mode to go to.
    #[test]
    fn start_session_counts_down_into_a_race() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(Uuid::new_v4(), car_id).unwrap();
        let ticks = START_SESSION_COUNTDOWN_SECONDS as u32 * DEFAULT_TICK_RATE_HZ as u32 + 2;
        assert!(game_session.start_countdown());
        let inputs = HashMap::new();
        for _ in 0..ticks {
            game_session.tick(&inputs);
        }
        assert_eq!(game_session.session.game_mode, GameMode::Race);
        assert_eq!(game_session.session.state, SessionState::Racing);
    }

    #[test]
    fn a_running_race_refuses_drivers_but_practice_takes_them() {
        let mut game_session = create_test_session();
        assert_eq!(game_session.refuses_drivers(), None);
        game_session.start_countdown_mode(5, GameMode::Race);
        assert_eq!(game_session.refuses_drivers(), None, "the grid is open");
        game_session.set_game_mode(GameMode::Race);
        assert!(game_session.refuses_drivers().is_some());
        game_session.set_game_mode(GameMode::FreePractice);
        assert_eq!(game_session.refuses_drivers(), None);
        game_session.set_game_mode(GameMode::Hotlap);
        assert_eq!(game_session.refuses_drivers(), None);
        game_session.session.state = SessionState::Finished;
        assert!(game_session.refuses_drivers().is_some());
    }

    #[test]
    fn the_lobby_hears_each_state_change_once() {
        let mut game_session = create_test_session();
        assert_eq!(game_session.take_state_change(), None);
        game_session.set_game_mode(GameMode::FreePractice);
        assert_eq!(game_session.take_state_change(), Some(SessionState::Racing));
        assert_eq!(game_session.take_state_change(), None);
    }

    #[test]
    fn test_tick_countdown() {
        let mut game_session = create_test_session();
        // Use the new game mode system
        game_session.set_game_mode(GameMode::Countdown);

        let initial_countdown = game_session.session.countdown_ticks_remaining.unwrap();

        let inputs = HashMap::new();
        game_session.tick(&inputs);

        assert_eq!(
            game_session.session.countdown_ticks_remaining.unwrap(),
            initial_countdown - 1
        );
    }

    #[test]
    fn test_car_setup_tunes_only_that_driver() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let tuned_driver = Uuid::new_v4();
        let stock_driver = Uuid::new_v4();
        game_session.add_player(tuned_driver, car_id);
        game_session.add_player(stock_driver, car_id);
        let base_spring = game_session.car_configs[&car_id]
            .suspension
            .spring_rate_front_n_per_m;

        // Out of range on purpose: the applied setup is the clamped one.
        let asked = CarSetup {
            spring_front: 9,
            rev_limiter: 2,
            ..Default::default()
        };
        let applied = game_session.set_car_setup(&tuned_driver, asked).unwrap();
        assert_eq!(applied.spring_front, 5);
        assert_eq!(applied.rev_limiter, 0);
        assert_eq!(game_session.car_setup(&tuned_driver), applied);

        let tuned = game_session.simulated_config_for(&tuned_driver).unwrap();
        assert!((tuned.suspension.spring_rate_front_n_per_m - base_spring * 1.2).abs() < 1e-2);
        let stock = game_session.simulated_config_for(&stock_driver).unwrap();
        assert_eq!(stock.suspension.spring_rate_front_n_per_m, base_spring);
        assert_eq!(
            game_session.car_configs[&car_id]
                .suspension
                .spring_rate_front_n_per_m,
            base_spring
        );

        // A stock setup drops the tuned copy; so does leaving.
        game_session.set_car_setup(&tuned_driver, CarSetup::default());
        assert!(game_session.tuned_configs.is_empty());
        assert!(game_session.car_setup(&tuned_driver).is_stock());
        game_session.set_car_setup(&tuned_driver, applied);
        game_session.remove_player(&tuned_driver);
        assert!(game_session.tuned_configs.is_empty());

        // Nobody's car: nothing applied.
        assert!(game_session
            .set_car_setup(&Uuid::new_v4(), applied)
            .is_none());
    }

    /// A human's racing line aid is planned again at the line for the car
    /// as it is: a full tank and a slippery road make a slower line, a car
    /// that has not changed gets none, and a few hundredths of grip from
    /// the tyres' heat do not send one.
    #[test]
    fn the_racing_line_aid_follows_the_fuel_and_the_grip() {
        let track = crate::track_loader::TrackLoader::load_from_file(
            "../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
        )
        .expect("Monzarella");
        let car = crate::car_loader::CarLoader::load_from_file(std::path::Path::new(
            "../../apexsim-content/cars/default/posh-gt3rs/car.toml",
        ))
        .expect("car");
        let car_id = car.id;
        let mut car_configs = HashMap::new();
        car_configs.insert(car_id, car.clone());
        let session = RaceSession::new(
            Uuid::from_u128(1),
            Uuid::nil(),
            SessionKind::Multiplayer,
            8,
            0,
            3,
        );
        let mut gs = GameSession::new(session, track, car_configs);
        let driver = Uuid::from_u128(7401);
        gs.add_player(driver, car_id);
        let joined =
            racing_line::lap_time_s(&racing_line::build(&gs.track_config, &car).expect("a line"));

        // Warm tyres in the window, as at the end of a lap.
        let state = gs.session.participants.get_mut(&driver).unwrap();
        state.tyres_fitted = true;
        for tyre in state.tires.each_mut() {
            tyre.grip_factor = 1.0;
        }
        state.surface_grip_share = 1.0;
        state.fuel_liters = car.fuel.capacity_liters;
        let full = racing_line::lap_time_s(
            &gs.refreshed_racing_line(&driver)
                .expect("a full tank plans again"),
        );
        assert!(
            full > joined,
            "full tank {full:.2} s against dry {joined:.2} s"
        );
        assert!(gs.refreshed_racing_line(&driver).is_none(), "nothing moved");

        gs.session
            .participants
            .get_mut(&driver)
            .unwrap()
            .surface_grip_share = 0.99;
        assert!(
            gs.refreshed_racing_line(&driver).is_none(),
            "a hundredth of grip is the tyres' heat"
        );
        gs.session
            .participants
            .get_mut(&driver)
            .unwrap()
            .surface_grip_share = 0.8;
        let wet = racing_line::lap_time_s(
            &gs.refreshed_racing_line(&driver)
                .expect("a fifth of the grip gone plans again"),
        );
        assert!(wet > full + 1.0, "slippery {wet:.2} s against {full:.2} s");
    }

    #[test]
    fn test_car_setup_changes_the_simulated_car() {
        // Two identical drivers, one with a shorter final drive: after a
        // second at full throttle from the grid the tuned car has pulled a
        // different speed, so the tick loop really reads the tuned copy.
        let mut a = create_test_session();
        let car_id = a.car_configs.values().next().unwrap().id;
        let driver = Uuid::new_v4();
        a.add_player(driver, car_id);
        let mut b = create_test_session();
        b.add_player(driver, car_id);
        b.set_car_setup(
            &driver,
            CarSetup {
                torque_map: -5,
                ..Default::default()
            },
        );
        for session in [&mut a, &mut b] {
            session.set_game_mode(GameMode::FreePractice);
        }
        let mut inputs = HashMap::new();
        inputs.insert(
            driver,
            PlayerInputData {
                throttle: 1.0,
                gear: Some(1),
                ..Default::default()
            },
        );
        for _ in 0..DEFAULT_TICK_RATE_HZ {
            a.tick(&inputs);
            b.tick(&inputs);
        }
        let speed = |s: &GameSession| s.session.participants[&driver].speed_mps;
        assert!(speed(&a) > 1.0, "the stock car moved: {}", speed(&a));
        assert!(
            speed(&b) < speed(&a),
            "80% torque should be slower: tuned {} vs stock {}",
            speed(&b),
            speed(&a)
        );
    }

    #[test]
    fn test_ai_input_generation() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;

        // Create an AI profile
        let ai_profile = AiDriverProfile::new("Test AI", 90);
        let ai_player_id = ai_profile.id;

        // Add AI profile to the session
        game_session.set_ai_profiles(vec![ai_profile]);

        // Add the AI player to the session
        game_session.add_player(ai_player_id, car_id);

        let ai_input = game_session.generate_ai_input(&ai_player_id);

        // AI should generate valid inputs
        assert!(ai_input.throttle >= 0.0 && ai_input.throttle <= 1.0);
        assert!(ai_input.brake >= 0.0 && ai_input.brake <= 1.0);
        assert!(ai_input.steering >= -1.0 && ai_input.steering <= 1.0);
    }

    #[test]
    fn test_ai_spawn_with_profiles() {
        use crate::ai_driver::generate_default_ai_profiles;

        let track = TrackConfig::default();
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        // Create session with 2 AI drivers
        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 2, 3);
        let ai_profiles = generate_default_ai_profiles(2);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);

        // Spawn AI drivers
        game_session.spawn_ai_drivers();

        // Should have 2 AI participants
        assert_eq!(game_session.session.participants.len(), 2);
        assert_eq!(game_session.session.ai_player_ids.len(), 2);

        // All should be recognized as AI players
        for ai_id in &game_session.session.ai_player_ids {
            assert!(game_session.is_ai_player(ai_id));
        }
    }

    #[test]
    fn roster_carries_picked_liveries_and_deals_the_ai_theirs() {
        use crate::ai_driver::generate_default_ai_profiles;

        let track = TrackConfig::default();
        let car = CarConfig {
            livery_names: vec!["Blue".to_string(), "Gold".to_string()],
            ..CarConfig::default()
        };
        let car_id = car.id;
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car);
        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 4, 3);
        let mut game_session = GameSession::with_ai_profiles(
            session,
            track,
            car_configs,
            generate_default_ai_profiles(4),
        );
        game_session.spawn_ai_drivers();
        let (picked, stale, stock) = (Uuid::new_v4(), Uuid::new_v4(), Uuid::new_v4());
        for p in [picked, stale, stock] {
            game_session.add_player(p, car_id);
        }
        game_session.set_livery(picked, 2);
        game_session.set_livery(stale, 7);

        let roster = game_session.roster_rows(&HashMap::new());
        let livery_of = |id: PlayerId| roster.iter().find(|e| e.player_id == id).unwrap().livery;
        assert_eq!(livery_of(picked), 2, "a pick inside the car's list is kept");
        assert_eq!(
            livery_of(stale),
            0,
            "a pick past the end falls back to the car as authored"
        );
        assert_eq!(livery_of(stock), 0);
        let mut ai: Vec<u8> = roster
            .iter()
            .filter(|e| e.is_ai)
            .map(|e| e.livery)
            .collect();
        ai.sort();
        assert_eq!(
            ai,
            vec![0, 0, 1, 2],
            "four AI in one model wear its three liveries in turn"
        );

        game_session.remove_player(&picked);
        game_session.add_player(picked, car_id);
        let roster = game_session.roster_rows(&HashMap::new());
        assert_eq!(
            roster
                .iter()
                .find(|e| e.player_id == picked)
                .unwrap()
                .livery,
            0,
            "leaving the session forgets the pick"
        );
    }

    // --- Game Mode Tests ---

    #[test]
    fn test_default_game_mode_is_lobby() {
        let game_session = create_test_session();
        assert_eq!(game_session.session.game_mode, GameMode::Lobby);
    }

    #[test]
    fn test_lobby_mode_tick() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Lobby);

        let initial_tick = game_session.session.current_tick;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Tick counter should increment
        assert_eq!(game_session.session.current_tick, initial_tick + 1);

        // No participants should move in lobby mode
        assert_eq!(game_session.session.participants.len(), 0);
    }

    #[test]
    fn test_sandbox_mode_tick() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Sandbox);

        // Add a player
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        let initial_pos = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Position should not change in sandbox mode
        let final_pos = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        assert_eq!(initial_pos, final_pos);
    }

    #[test]
    fn test_countdown_mode_decrements() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        let initial_countdown = game_session.session.countdown_ticks_remaining.unwrap();
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        assert_eq!(
            game_session.session.countdown_ticks_remaining.unwrap(),
            initial_countdown - 1
        );
    }

    #[test]
    fn test_countdown_mode_finishes() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);
        game_session.session.countdown_ticks_remaining = Some(1);

        let inputs = HashMap::new();
        game_session.tick(&inputs); // Decrements to 0

        // Should be 0 now
        assert_eq!(game_session.session.countdown_ticks_remaining, Some(0));

        game_session.tick(&inputs); // Clears to None

        // Countdown should be None after finishing
        assert_eq!(game_session.session.countdown_ticks_remaining, None);
    }

    #[test]
    fn test_demolap_mode_initializes_progress() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        game_session.set_game_mode(GameMode::DemoLap);

        assert!(game_session.session.demo_lap_progress.is_some());
        assert_eq!(game_session.session.demo_lap_progress.unwrap(), 0.0);
    }

    #[test]
    fn test_demolap_mode_advances_progress() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Add racing line to track
        game_session.track_config.raceline = vec![
            RacelinePoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 100.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 0.0,
                y: 100.0,
                z: 0.0,
            },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        // With the AI-driven demo lap, we check that the AI car moves
        // (demo_lap_progress is only used in the camera-following fallback)
        let ai_player_id = *game_session
            .session
            .ai_player_ids
            .first()
            .expect("DemoLap should spawn an AI driver");

        let initial_pos = game_session
            .session
            .participants
            .get(&ai_player_id)
            .map(|s| (s.pos_x, s.pos_y))
            .unwrap();

        let inputs = HashMap::new();

        // Run enough ticks for the car to launch from standstill and move
        // One second of simulation time.
        for _ in 0..DEFAULT_TICK_RATE_HZ {
            game_session.tick(&inputs);
        }

        // AI car should have moved
        let final_pos = game_session
            .session
            .participants
            .get(&ai_player_id)
            .map(|s| (s.pos_x, s.pos_y))
            .unwrap();

        let distance_moved =
            ((final_pos.0 - initial_pos.0).powi(2) + (final_pos.1 - initial_pos.1).powi(2)).sqrt();

        assert!(
            distance_moved > 0.1,
            "AI car should have moved in DemoLap mode, moved: {}m",
            distance_moved
        );
    }

    #[test]
    fn test_demolap_mode_loops() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Add racing line
        game_session.track_config.raceline = vec![
            RacelinePoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            RacelinePoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
            },
        ];

        game_session.set_game_mode(GameMode::DemoLap);

        // With AI-driven demo lap, verify the AI driver exists and is moving
        assert!(
            !game_session.session.ai_player_ids.is_empty(),
            "DemoLap should create an AI driver"
        );

        let ai_player_id = *game_session.session.ai_player_ids.first().unwrap();

        let inputs = HashMap::new();

        // Run simulation for enough ticks to allow the car to launch from standstill
        // (physics now supports generating tire force from rest via slip ratio)
        for _ in 0..100 {
            game_session.tick(&inputs);
        }

        // AI should be driving (speed > 0) after launching from standstill
        let ai_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();
        assert!(
            ai_state.speed_mps > 0.0,
            "AI should gain speed during demo lap (launched from standstill)"
        );
    }

    #[test]
    fn test_free_practice_mode_updates_physics() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::FreePractice);

        // Add a player
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        let initial_pos_x = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;

        // Apply throttle input
        let mut inputs = HashMap::new();
        inputs.insert(
            player_id,
            PlayerInputData {
                throttle: 1.0,
                brake: 0.0,
                steering: 0.0,
                gear: None,
                clutch: None,
                drs: false,
                headlights: None,
                flash: false,
                ers_mode: None,
                ers_boost: false,
                handbrake: false,
            },
        );

        // Run several ticks to allow physics to update
        for _ in 0..DEFAULT_TICK_RATE_HZ {
            game_session.tick(&inputs);
        }

        // Car should have moved after applying throttle for 1 second
        let final_pos_x = game_session
            .session
            .participants
            .get(&player_id)
            .unwrap()
            .pos_x;
        assert_ne!(initial_pos_x, final_pos_x);
    }

    #[test]
    fn test_set_game_mode() {
        let mut game_session = create_test_session();

        game_session.set_game_mode(GameMode::Sandbox);
        assert_eq!(game_session.session.game_mode, GameMode::Sandbox);

        game_session.set_game_mode(GameMode::Countdown);
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert!(game_session.session.countdown_ticks_remaining.is_some());

        game_session.set_game_mode(GameMode::DemoLap);
        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
        assert!(game_session.session.demo_lap_progress.is_some());
    }

    #[test]
    fn test_start_countdown_mode() {
        let mut game_session = create_test_session();

        game_session.start_countdown_mode(10, GameMode::FreePractice);

        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(
            game_session.session.countdown_ticks_remaining,
            Some(DEFAULT_TICK_RATE_HZ * 10)
        );
    }

    #[test]
    fn test_grid_waits_for_every_connected_driver_then_counts_down() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        // Nobody can be ready before the grid is up.
        assert!(!game_session.set_ready(a, true));

        game_session.open_grid(3);
        assert_eq!(game_session.session.game_mode, GameMode::Grid);
        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert_eq!(game_session.session.countdown_ticks_remaining, None);
        let none = HashMap::new();
        for _ in 0..DEFAULT_TICK_RATE_HZ * 5 {
            game_session.tick(&none);
        }
        assert_eq!(
            game_session.session.game_mode,
            GameMode::Grid,
            "no clock on the grid"
        );

        // One ready is not enough; the roster says who is.
        assert!(game_session.set_ready(a, true));
        game_session.tick(&none);
        assert_eq!(game_session.session.game_mode, GameMode::Grid);
        let roster = game_session.roster_rows(&HashMap::new());
        let ready = |id: PlayerId| roster.iter().find(|e| e.player_id == id).unwrap().ready;
        assert!(ready(a) && !ready(b));

        // A driver away is not waited for.
        game_session.away.insert(b);
        game_session.tick(&none);
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(
            game_session.session.countdown_ticks_remaining,
            Some(DEFAULT_TICK_RATE_HZ * 3)
        );
        assert_eq!(game_session.session.next_mode, Some(GameMode::Race));
        let roster = game_session.roster_rows(&HashMap::new());
        assert!(roster.iter().all(|e| !e.ready), "ready ends with the grid");
        for _ in 0..DEFAULT_TICK_RATE_HZ * 4 {
            game_session.tick(&none);
        }
        assert_eq!(game_session.session.game_mode, GameMode::Race);
    }

    #[test]
    fn test_grid_with_nobody_ready_starts_when_the_host_counts_down() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let a = Uuid::from_u128(1);
        game_session.add_player(a, car_id).unwrap();
        game_session.open_grid(10);
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.game_mode, GameMode::Grid);
        game_session.start_countdown_mode(5, GameMode::Race);
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert!(!game_session.set_ready(a, true), "the grid is gone");
    }

    #[test]
    fn test_grid_ready_leaves_with_the_driver() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        game_session.open_grid(5);
        assert!(game_session.set_ready(b, true));
        // The one driver not ready leaves: the rest are, so it counts down.
        game_session.remove_player(&a);
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
    }

    fn racing_session_with_two_humans() -> (GameSession, PlayerId, PlayerId) {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        game_session.set_game_mode(GameMode::Race);
        (game_session, a, b)
    }

    /// A time penalty is added to when a car took the flag: the car first
    /// over the line with 5 s to add drops behind one crossing 2 s later,
    /// and its race time and gap carry the 5 s.
    #[test]
    fn a_time_penalty_is_added_to_the_race_time() {
        use crate::race_control::{Offence, YELLOW_PASS_PENALTY_S};
        let (mut gs, first, second) = racing_session_with_two_humans();
        gs.session.race_rules.yellow_penalty = true;
        gs.penalise(first, Offence::YellowPass);
        assert_eq!(gs.penalty_seconds(&first), YELLOW_PASS_PENALTY_S as u32);
        gs.session.participants.get_mut(&first).unwrap().current_lap = 4;
        gs.tick(&HashMap::new());
        assert_eq!(gs.session.participants[&first].finish_position, Some(1));
        for _ in 0..2 * DEFAULT_TICK_RATE_HZ {
            gs.tick(&HashMap::new());
        }
        gs.session
            .participants
            .get_mut(&second)
            .unwrap()
            .current_lap = 4;
        gs.tick(&HashMap::new());
        assert_eq!(gs.session.participants[&second].finish_position, Some(1));
        assert_eq!(gs.session.participants[&first].finish_position, Some(2));
        let table = gs.classification(&HashMap::new());
        assert_eq!(table[0].player_id, second);
        assert_eq!(table[1].penalty_s, YELLOW_PASS_PENALTY_S as u32);
        let gap = table[1].gap_ms.unwrap();
        assert!((2_900..=3_100).contains(&gap), "5 s less the 2 s: {gap} ms");
    }

    /// A pass under a yellow is judged [`YELLOW_PASS_HANDBACK_S`] after it:
    /// the passer still ahead is penalised, one that handed the place back
    /// is not.
    #[test]
    fn a_yellow_pass_handed_back_is_forgiven() {
        use crate::race_control::{YELLOW_PASS_HANDBACK_S, YELLOW_PASS_PENALTY_S};
        let window = (YELLOW_PASS_HANDBACK_S * DEFAULT_TICK_RATE_HZ as f32) as u32 + 2;
        for handed_back in [false, true] {
            let (mut gs, passer, passed) = racing_session_with_two_humans();
            gs.session.race_rules.yellow_penalty = true;
            gs.session
                .participants
                .get_mut(&passer)
                .unwrap()
                .current_lap = 2;
            let tick = gs.session.current_tick;
            gs.yellow_passed.insert((passer, passed), tick);
            for _ in 0..window / 2 {
                gs.tick(&HashMap::new());
            }
            assert_eq!(gs.penalty_seconds(&passer), 0, "not judged yet");
            if handed_back {
                gs.session
                    .participants
                    .get_mut(&passed)
                    .unwrap()
                    .current_lap = 3;
            }
            for _ in 0..window {
                gs.tick(&HashMap::new());
            }
            let owed = if handed_back {
                0
            } else {
                YELLOW_PASS_PENALTY_S as u32
            };
            assert_eq!(
                gs.penalty_seconds(&passer),
                owed,
                "handed back: {handed_back}"
            );
            assert!(gs.yellow_passed.is_empty());
        }
    }

    #[test]
    fn test_race_grid_waits_in_neutral_and_revs_until_the_green_light() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (auto, manual) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(auto, car_id).unwrap();
        game_session.add_player(manual, car_id).unwrap();
        game_session
            .session
            .participants
            .get_mut(&auto)
            .unwrap()
            .auto_gearbox = true;
        game_session.start_countdown_mode(2, GameMode::Race);
        let config = game_session.car_configs[&car_id].clone();
        let start = |s: &GameSession, id: &PlayerId| {
            let car = &s.session.participants[id];
            (car.pos_x, car.pos_y, car.gear)
        };
        assert_eq!(start(&game_session, &auto).2, 0);
        assert_eq!(start(&game_session, &manual).2, 0);
        let grid = (start(&game_session, &auto), start(&game_session, &manual));

        // Flat out on the grid for a second: the engines rev, nothing moves,
        // and the automatic box stays in neutral.
        let full = PlayerInputData {
            throttle: 1.0,
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(auto, full), (manual, full)].into();
        for _ in 0..DEFAULT_TICK_RATE_HZ {
            game_session.tick(&inputs);
        }
        let car = &game_session.session.participants[&auto];
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(car.gear, 0);
        assert!(
            car.engine_rpm > config.redline_rpm * 0.8,
            "revving on the grid, rpm={}",
            car.engine_rpm
        );
        assert!((car.throttle_input - 1.0).abs() < 1e-6);
        assert_eq!(
            (start(&game_session, &auto), start(&game_session, &manual)),
            grid
        );

        // The manual driver selects first on the grid, then waits.
        let first = PlayerInputData {
            gear: Some(1),
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(auto, PlayerInputData::default()), (manual, first)].into();
        game_session.tick(&inputs);
        assert_eq!(game_session.session.participants[&manual].gear, 1);
        let coast: HashMap<PlayerId, PlayerInputData> = HashMap::new();
        while game_session.session.game_mode == GameMode::Countdown {
            game_session.tick(&coast);
        }

        // Green: the automatic box takes first by itself.
        assert_eq!(game_session.session.game_mode, GameMode::Race);
        assert_eq!(game_session.session.participants[&auto].gear, 1);
        assert_eq!(game_session.session.participants[&manual].gear, 1);
    }

    #[test]
    fn test_winner_finishing_starts_the_clock_on_the_rest() {
        let (mut game_session, winner, _) = racing_session_with_two_humans();
        game_session
            .session
            .participants
            .get_mut(&winner)
            .unwrap()
            .current_lap = 4;

        game_session.tick(&HashMap::new());
        assert_eq!(
            game_session.session.participants[&winner].finish_position,
            Some(1)
        );
        assert_eq!(game_session.session.state, SessionState::Racing);

        // A quick race: the floor applies.
        let deadline = game_session.finish_deadline_tick.expect("deadline set");
        assert!(
            deadline
                >= game_session.session.current_tick
                    + FINISH_GRACE_MIN_SECONDS * DEFAULT_TICK_RATE_HZ as u32
        );

        game_session.session.current_tick = deadline - 1;
        game_session.tick(&HashMap::new());
        assert_eq!(
            game_session.session.state,
            SessionState::Finished,
            "the race ends at the deadline with the second car unclassified"
        );
    }

    /// A timed race: the clock runs to the end, the leader's lap is the
    /// last, the winner takes the flag on completing it, and every car
    /// after it at its next crossing, a lap down or not; a lapped car that
    /// crossed first is classified behind the car on the lead lap.
    #[test]
    fn test_timed_race_ends_on_the_leaders_lap_after_the_clock() {
        let mut game_session = create_test_session();
        game_session.session.race_seconds = Some(60);
        game_session.session.lap_limit = 0;
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (leader, second, lapped) = (Uuid::from_u128(1), Uuid::from_u128(2), Uuid::from_u128(3));
        for id in [leader, second, lapped] {
            game_session.add_player(id, car_id).unwrap();
        }
        game_session.set_game_mode(GameMode::Race);
        let start = game_session.session.race_start_tick.unwrap();
        let set_lap = |gs: &mut GameSession, id: &PlayerId, lap: u16| {
            gs.session.participants.get_mut(id).unwrap().current_lap = lap;
        };
        set_lap(&mut game_session, &leader, 6);
        set_lap(&mut game_session, &second, 5);
        set_lap(&mut game_session, &lapped, 4);
        let none: HashMap<PlayerId, PlayerInputData> = HashMap::new();

        // Laps past nothing while the clock runs: no lap is the last.
        game_session.session.current_tick = start + 60 * DEFAULT_TICK_RATE_HZ as u32 - 10;
        game_session.tick(&none);
        let clock = game_session.race_clock().expect("a timed race has a clock");
        assert!(clock.left_ms > 0 && clock.left_ms < 100, "{clock:?}");
        assert_eq!(clock.final_lap, 0);
        assert!(game_session
            .session
            .participants
            .values()
            .all(|s| s.finish_position.is_none()));

        // Out of time: the leader's lap is the last, and the race has a
        // deadline in case nobody can complete it.
        game_session.session.current_tick = start + 60 * DEFAULT_TICK_RATE_HZ as u32;
        game_session.tick(&none);
        let clock = game_session.race_clock().unwrap();
        assert_eq!(
            clock,
            RaceClock {
                left_ms: 0,
                final_lap: 6
            }
        );
        assert!(game_session.finish_deadline_tick.is_some());

        // The leader completes it: the winner.
        set_lap(&mut game_session, &leader, 7);
        game_session.tick(&none);
        let place = |gs: &GameSession, id: &PlayerId| gs.session.participants[id].finish_position;
        assert_eq!(place(&game_session, &leader), Some(1));
        assert_eq!(place(&game_session, &second), None);

        // The lapped car crosses first: it has the flag at once...
        set_lap(&mut game_session, &lapped, 5);
        game_session.tick(&none);
        assert_eq!(place(&game_session, &lapped), Some(2));
        assert_eq!(game_session.session.state, SessionState::Racing);

        // ...and the car on the lead lap that crosses after it goes ahead.
        set_lap(&mut game_session, &second, 6);
        game_session.tick(&none);
        assert_eq!(place(&game_session, &second), Some(2));
        assert_eq!(place(&game_session, &lapped), Some(3));
        assert_eq!(game_session.session.state, SessionState::Finished);
    }

    /// The pit rule: a car that takes the flag owing its stop, or the second
    /// compound, is classified behind every finisher that met the rule, and
    /// the roster says so; the stops count from the green light.
    #[test]
    fn a_car_that_misses_the_pit_rule_is_classified_behind_those_that_met_it() {
        let mut game_session = create_test_session();
        game_session.set_pit_rule(9, true);
        assert_eq!(
            game_session.session.pit_rule,
            crate::data::PitRule {
                mandatory_stops: crate::data::MAX_MANDATORY_STOPS,
                compound_change: true
            }
        );
        game_session.set_pit_rule(1, true);
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (none, same, met) = (Uuid::from_u128(1), Uuid::from_u128(2), Uuid::from_u128(3));
        for id in [none, same, met] {
            game_session.add_player(id, car_id).unwrap();
        }
        // A stop in practice does not count.
        game_session
            .session
            .participants
            .get_mut(&met)
            .unwrap()
            .pit
            .stops = 1;
        game_session.set_game_mode(GameMode::Race);
        assert_eq!(game_session.session.participants[&met].pit.stops, 0);
        for (id, stops, changed) in [(same, 1, false), (met, 1, true)] {
            let pit = &mut game_session.session.participants.get_mut(&id).unwrap().pit;
            pit.stops = stops;
            pit.compound_changed = changed;
        }
        let set_lap = |gs: &mut GameSession, id: &PlayerId, lap: u16| {
            gs.session.participants.get_mut(id).unwrap().current_lap = lap;
        };
        let ticks: HashMap<PlayerId, PlayerInputData> = HashMap::new();
        let place = |gs: &GameSession, id: &PlayerId| gs.session.participants[id].finish_position;
        let finish = game_session.session.lap_limit as u16 + 1;

        // The car with no stop takes the flag first: the winner, for now.
        set_lap(&mut game_session, &none, finish);
        game_session.tick(&ticks);
        assert_eq!(place(&game_session, &none), Some(1));
        // The one that stopped onto the same compound is behind nobody that
        // met the rule yet; the one that met it goes ahead of both.
        set_lap(&mut game_session, &same, finish);
        game_session.tick(&ticks);
        set_lap(&mut game_session, &met, finish);
        game_session.tick(&ticks);
        assert_eq!(place(&game_session, &met), Some(1));
        assert_eq!(place(&game_session, &none), Some(2));
        assert_eq!(place(&game_session, &same), Some(3));
        let roster = game_session.roster_rows(&HashMap::new());
        let missed = |id: PlayerId| {
            roster
                .iter()
                .find(|e| e.player_id == id)
                .unwrap()
                .missed_stops
        };
        assert!(missed(none) && missed(same) && !missed(met));

        // The classification written for the race (`crate::results`): the
        // places as the flag gave them, the penalty, the gaps on the lap.
        let names: HashMap<PlayerId, String> =
            [(met, "Met".to_string()), (none, "None".to_string())].into();
        let table = game_session.classification(&names);
        let ids: Vec<PlayerId> = table.iter().map(|c| c.player_id).collect();
        assert_eq!(ids, vec![met, none, same]);
        assert_eq!(table[0].name, "Met");
        assert_eq!(table[0].status, "finished");
        assert!(!table[0].missed_stops && table[1].missed_stops);
        assert_eq!(table[0].gap_ms, None, "the winner has no gap");
        assert!(
            table[2].gap_ms.is_some(),
            "a car on the winner's lap has one"
        );
        assert!(table
            .iter()
            .all(|c| c.laps_down == 0 && c.race_time_ms.is_some()));
        assert_eq!(
            table.iter().map(|c| c.position).collect::<Vec<_>>(),
            vec![1, 2, 3]
        );
    }

    /// A timed race is as many laps as the clock holds plus the one it runs
    /// out on, and a car's laps to go count down by its own lap time.
    #[test]
    fn test_timed_race_laps_come_from_the_clock() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.lap_seconds.insert(car_id, 90.0);
        assert_eq!(game_session.race_laps(car_id), 3.0, "a race over laps");

        game_session.session.race_seconds = Some(3600);
        game_session.session.lap_limit = 0;
        assert_eq!(game_session.race_laps(car_id), 41.0);

        let player = Uuid::from_u128(1);
        game_session.add_player(player, car_id).unwrap();
        // Seating fuels the car, which plans its ideal lap on this track.
        game_session.lap_seconds.insert(car_id, 90.0);
        game_session.set_game_mode(GameMode::Race);
        let start = game_session.session.race_start_tick.unwrap();
        game_session.session.current_tick = start + 1800 * DEFAULT_TICK_RATE_HZ as u32;
        let state = game_session.session.participants[&player].clone();
        assert_eq!(
            game_session.laps_left(&state),
            20,
            "half an hour of 90 s laps"
        );
        let mut quicker = state.clone();
        quicker.last_lap_time_ms = Some(60_000);
        assert_eq!(game_session.laps_left(&quicker), 30, "its own pace wins");
        assert_eq!(game_session.race_seconds_left(), Some(1800.0));
    }

    /// A timed qualifying session with two humans, both in their garages
    /// as it begins: `seconds` long, entered now.
    fn timed_qualifying(seconds: u32) -> (GameSession, PlayerId, PlayerId) {
        let mut game_session = create_test_session();
        game_session.set_qualifying_seconds(Some(seconds));
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        game_session.set_game_mode(GameMode::Qualification);
        assert!(game_session.session.participants[&a].in_garage);
        assert!(game_session.session.participants[&b].in_garage);
        (game_session, a, b)
    }

    /// Put a car just past the line at speed, as if it had been short of
    /// it the tick before, on a clean flying lap 2 begun `lap_s` ago, and
    /// drive it on until the crossing counts (or it is put in its garage).
    /// Returns the ticks it took.
    fn drive_over_the_line(game_session: &mut GameSession, id: PlayerId, lap_s: u32) -> u32 {
        let length = game_session.track_length_m();
        let point = game_session
            .track_config
            .centerline
            .iter()
            .find(|p| p.distance_from_start_m >= 2.0)
            .cloned()
            .unwrap();
        let tick = game_session.session.current_tick;
        let rate = game_session.tick_rate_hz as u32;
        let state = game_session.session.participants.get_mut(&id).unwrap();
        place_still(state, point.x, point.y, point.z, point.heading_rad);
        let speed = 30.0;
        state.vel_x = speed * point.heading_rad.cos();
        state.vel_y = speed * point.heading_rad.sin();
        state.speed_mps = speed;
        state.track_progress = length - 1.0;
        state.outlap = false;
        state.current_lap = 2;
        state.next_checkpoint = u8::MAX;
        state.lap_start_tick = tick.saturating_sub(lap_s * rate);
        state.laps.invalid = false;
        state.laps.splits_ms = [20_000, 20_000, 0];
        state.laps.sector_start_tick = tick.saturating_sub(20 * rate);
        let input = PlayerInputData {
            throttle: 0.3,
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(id, input)].into_iter().collect();
        for n in 1..=rate {
            game_session.tick(&inputs);
            let state = &game_session.session.participants[&id];
            if state.in_garage || state.current_lap > 2 {
                return n;
            }
        }
        panic!("the car never reached the line");
    }

    /// What the game loop does with the timing lines crossed: a valid lap
    /// goes through `note_qualifying_lap` (`game_loop::tick`).
    fn classify_laps(game_session: &mut GameSession) -> usize {
        let mut laps = 0;
        for out in game_session.take_lap_events() {
            if let (Some(ms), true) = (out.event.lap_time_ms, out.event.valid) {
                game_session.note_qualifying_lap(out.player_id, ms);
                laps += 1;
            }
        }
        laps
    }

    #[test]
    fn open_ended_qualifying_has_no_clock_and_never_ends() {
        let mut game_session = create_test_session();
        game_session.set_qualifying_seconds(Some(0));
        assert_eq!(game_session.session.qualifying_seconds, None);
        game_session.set_game_mode(GameMode::Qualification);
        game_session.session.current_tick += 10 * 3600 * DEFAULT_TICK_RATE_HZ as u32;
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.race_clock(), None);
        assert!(!game_session.chequered_flag_out());
        assert_eq!(game_session.session.state, SessionState::Racing);
    }

    #[test]
    fn qualifying_seconds_are_clamped() {
        assert_eq!(crate::data::clamp_qualifying_seconds(None), None);
        assert_eq!(crate::data::clamp_qualifying_seconds(Some(0)), None);
        assert_eq!(crate::data::clamp_qualifying_seconds(Some(5)), Some(60));
        assert_eq!(crate::data::clamp_qualifying_seconds(Some(900)), Some(900));
        assert_eq!(
            crate::data::clamp_qualifying_seconds(Some(24 * 3600)),
            Some(crate::data::MAX_QUALIFYING_SECONDS)
        );
    }

    /// The qualifying clock counts down from entering the mode; at zero the
    /// flag is out: a car on track takes it at its next crossing, the lap
    /// it completes there still classifies, it goes to its garage, nobody
    /// goes out again, and the session ends with every car in.
    #[test]
    fn the_flag_garages_a_car_at_its_next_crossing_and_that_lap_counts() {
        let (mut game_session, a, b) = timed_qualifying(600);
        let start = game_session.session.current_tick;
        let length_ticks = 600 * DEFAULT_TICK_RATE_HZ as u32;
        assert_eq!(
            game_session.race_clock(),
            Some(RaceClock {
                left_ms: 600_000,
                final_lap: 0
            })
        );
        game_session
            .hotlap_relocate(&a, HotlapDestination::Track, false)
            .unwrap();
        game_session.tick(&HashMap::new());
        let clock = game_session.race_clock().unwrap();
        assert!(
            clock.left_ms < 600_000 && clock.left_ms > 599_000,
            "{clock:?}"
        );
        assert_eq!(clock.final_lap, 0);
        // The race's own clock and laps are untouched by qualifying.
        assert_eq!(game_session.race_seconds_left(), None);

        // Before the flag a crossing is just a lap.
        game_session.session.current_tick += 100 * DEFAULT_TICK_RATE_HZ as u32;
        drive_over_the_line(&mut game_session, a, 61);
        assert!(!game_session.session.participants[&a].in_garage);
        assert_eq!(classify_laps(&mut game_session), 1);

        // The clock runs out: the flag is out, the car still on track runs on.
        game_session.session.current_tick = start + length_ticks - 1;
        game_session.tick(&HashMap::new());
        assert!(game_session.chequered_flag_out());
        assert_eq!(
            game_session.race_clock(),
            Some(RaceClock {
                left_ms: 0,
                final_lap: 0
            })
        );
        assert!(!game_session.session.participants[&a].in_garage);
        assert_eq!(game_session.session.state, SessionState::Racing);
        assert_eq!(
            game_session.hotlap_relocate(&b, HotlapDestination::Track, false),
            Err("Qualifying is over")
        );

        // Its next crossing: the flag, a faster lap, and the garage.
        game_session.note_qualifying_lap(b, 59_000);
        assert_eq!(game_session.qualifying_order(), vec![b, a]);
        drive_over_the_line(&mut game_session, a, 58);
        assert!(game_session.session.participants[&a].in_garage);
        assert_eq!(classify_laps(&mut game_session), 1);
        assert!(game_session.qualifying[&a].0 < 59_000);
        assert_eq!(game_session.qualifying_order(), vec![a, b]);
        assert_eq!(game_session.session.state, SessionState::Finished);
        // The clock stays at zero and nobody goes out.
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.race_clock().unwrap().left_ms, 0);
        assert!(game_session
            .hotlap_relocate(&a, HotlapDestination::Track, false)
            .is_err());
    }

    /// A car that never comes back to the line is brought in at the
    /// deadline after the flag, and the session ends with it.
    #[test]
    fn timed_qualifying_ends_at_the_deadline_after_the_flag() {
        let (mut game_session, a, b) = timed_qualifying(60);
        let start = game_session.session.current_tick;
        for id in [a, b] {
            game_session
                .hotlap_relocate(&id, HotlapDestination::Track, false)
                .unwrap();
        }
        game_session.session.current_tick = start + 60 * DEFAULT_TICK_RATE_HZ as u32 - 1;
        game_session.tick(&HashMap::new());
        let flag_tick = game_session.session.current_tick;
        let deadline = game_session.finish_deadline_tick.expect("a deadline");
        assert!(
            deadline >= flag_tick + QUALIFYING_FLAG_GRACE_MIN_SECONDS * DEFAULT_TICK_RATE_HZ as u32
        );
        assert_eq!(game_session.session.state, SessionState::Racing);

        game_session.session.current_tick = deadline - 1;
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.state, SessionState::Finished);
        assert!(game_session
            .session
            .participants
            .values()
            .all(|s| s.in_garage));
        assert_eq!(
            game_session.take_state_change(),
            Some(SessionState::Finished)
        );
    }

    /// Qualifying entered again is a fresh session: the clock from full
    /// and an empty classification.
    #[test]
    fn qualifying_again_restarts_the_clock_and_the_classification() {
        let (mut game_session, a, b) = timed_qualifying(60);
        game_session.note_qualifying_lap(a, 90_000);
        // The round's own timing sheet, as telemetry would carry it.
        if let Some(state) = game_session.session.participants.get_mut(&a) {
            state.best_lap_time_ms = Some(90_000);
            state.laps.best_lap_ms = Some(90_000);
            state.laps.best_splits_ms = [Some(30_000); SECTOR_COUNT];
        }
        game_session.session_best_lap_ms = Some(90_000);
        game_session.session.current_tick += 60 * DEFAULT_TICK_RATE_HZ as u32;
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.state, SessionState::Finished);

        game_session.set_game_mode(GameMode::Qualification);
        // A new round's bests are its own, not the last round's.
        let car = &game_session.session.participants[&a];
        assert_eq!(car.best_lap_time_ms, None);
        assert_eq!(car.laps.best_lap_ms, None);
        assert_eq!(car.laps.best_splits_ms, [None; SECTOR_COUNT]);
        assert_eq!(game_session.session_best_lap_ms, None);
        assert_eq!(game_session.session.state, SessionState::Racing);
        assert!(!game_session.chequered_flag_out());
        assert_eq!(game_session.race_clock().unwrap().left_ms, 60_000);
        assert!(game_session.qualifying_order().is_empty());
        assert!(game_session.finish_deadline_tick.is_none());
        assert!(game_session
            .hotlap_relocate(&b, HotlapDestination::Track, false)
            .is_ok());
    }

    /// After a finished qualifying session the host counts into the race:
    /// the cars leave their garages for the grid in qualifying order.
    #[test]
    fn a_race_after_finished_qualifying_grids_from_the_result() {
        let (mut game_session, a, b) = timed_qualifying(60);
        game_session.note_qualifying_lap(a, 91_000);
        game_session.note_qualifying_lap(b, 89_000);
        game_session.session.current_tick += 60 * DEFAULT_TICK_RATE_HZ as u32;
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.state, SessionState::Finished);

        game_session.start_countdown_mode(3, GameMode::Race);
        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert_eq!(game_session.race_clock(), None, "a race over laps");
        let car = |gs: &GameSession, id: &PlayerId| gs.session.participants[id].clone();
        assert_eq!(car(&game_session, &b).grid_position, 1);
        assert_eq!(car(&game_session, &a).grid_position, 2);
        assert!(!car(&game_session, &a).in_garage && !car(&game_session, &b).in_garage);
        for _ in 0..(3 * DEFAULT_TICK_RATE_HZ as u32 + 1) {
            game_session.tick(&HashMap::new());
        }
        assert_eq!(game_session.session.game_mode, GameMode::Race);
        assert_eq!(game_session.session.state, SessionState::Racing);
    }

    // --- A race weekend --------------------------------------------------

    /// A weekend of `n` humans (ids 1..=n, seated in that order, named
    /// A, B, C...): `practice` s of practice (0 for none), then rounds of
    /// `rounds` s each.
    fn weekend_of(n: u128, practice: u32, rounds: &[u32]) -> (GameSession, Vec<PlayerId>) {
        let mut game_session = create_test_session();
        game_session.set_weekend(Some(crate::weekend::WeekendFormat {
            practice_seconds: practice,
            qualifying_seconds: rounds.to_vec(),
            practice_conditions: None,
            qualifying_conditions: None,
            ..Default::default()
        }));
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let ids: Vec<PlayerId> = (1..=n).map(Uuid::from_u128).collect();
        for (i, id) in ids.iter().enumerate() {
            let name = ((b'A' + i as u8) as char).to_string();
            game_session.set_driver_name(*id, &name);
            assert!(game_session.add_player(*id, car_id).is_some());
        }
        game_session.session.host_player_id = ids[0];
        (game_session, ids)
    }

    /// Run the clock of the timed phase out with every car in its garage:
    /// the flag is out and the phase is over on that tick.
    fn run_out_the_clock(game_session: &mut GameSession) {
        let length = game_session.timed_length_ticks().expect("a timed phase") as u32;
        game_session.session.current_tick = game_session.timed_start_tick.unwrap() + length - 1;
        game_session.tick(&HashMap::new());
        assert!(game_session.chequered_flag_out());
    }

    /// The phase's laps are in and the round is over, as the game loop
    /// leaves it after the tick.
    fn end_phase(game_session: &mut GameSession) {
        run_out_the_clock(game_session);
        assert_eq!(game_session.session.state, SessionState::Finished);
        game_session.settle_weekend();
    }

    #[test]
    fn a_weekend_format_is_clamped_and_replaces_the_qualifying_length() {
        let mut game_session = create_test_session();
        game_session.set_weekend(Some(crate::weekend::WeekendFormat {
            practice_seconds: 30,
            qualifying_seconds: vec![600, 0, 9_000, 600],
            practice_conditions: None,
            qualifying_conditions: None,
            ..Default::default()
        }));
        game_session.set_qualifying_seconds(Some(300));
        assert_eq!(game_session.session.qualifying_seconds, None);
        assert_eq!(
            game_session.session.weekend,
            Some(crate::weekend::WeekendFormat {
                practice_seconds: 60,
                qualifying_seconds: vec![600, 60, 7200],
                practice_conditions: None,
                qualifying_conditions: None,
                ..Default::default()
            })
        );
        let state = game_session.weekend_state().unwrap();
        assert_eq!(
            (state.phase, state.round, state.rounds, state.finished),
            (0, 0, 3, false)
        );
        assert_eq!(state.advancing, 0);
        assert!(state.eliminated.is_empty());
        assert!(create_test_session().weekend_state().is_none());
    }

    /// Practice has qualifying's clock and flag, but nothing it does
    /// classifies for the grid or is stored as qualifying.
    #[test]
    fn practice_has_a_clock_and_a_flag_and_classifies_nothing() {
        let (mut game_session, ids) = weekend_of(2, 600, &[600]);
        let (a, b) = (ids[0], ids[1]);
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.game_mode, GameMode::Practice);
        assert_eq!(game_session.session.state, SessionState::Racing);
        assert!(game_session.take_phase_started());
        assert!(!game_session.take_phase_started());
        assert_eq!(
            game_session.race_clock(),
            Some(RaceClock {
                left_ms: 600_000,
                final_lap: 0
            })
        );
        assert!(game_session.session.participants[&a].in_garage);
        let state = game_session.weekend_state().unwrap();
        assert_eq!((state.phase, state.round, state.finished), (1, 0, false));

        // Out of the garage as in qualifying: the outlap, on cold tyres.
        game_session
            .hotlap_relocate(&a, HotlapDestination::Track, false)
            .unwrap();
        assert!(game_session.session.participants[&a].outlap);
        assert!(game_session.refuses_drivers().is_none());

        // A lap before the flag is timed but classifies nobody.
        game_session.session.current_tick += 100 * DEFAULT_TICK_RATE_HZ as u32;
        drive_over_the_line(&mut game_session, a, 61);
        assert_eq!(classify_laps(&mut game_session), 1);
        assert!(game_session.qualifying_order().is_empty());
        assert!(game_session.qualifying_result().entries.is_empty());

        // The flag: the car on track takes it at the line, nobody goes out.
        let start = game_session.timed_start_tick.unwrap();
        game_session.session.current_tick = start + 600 * DEFAULT_TICK_RATE_HZ as u32 - 1;
        game_session.tick(&HashMap::new());
        assert!(game_session.chequered_flag_out());
        assert_eq!(game_session.race_clock().unwrap().left_ms, 0);
        assert_eq!(game_session.session.state, SessionState::Racing);
        assert_eq!(
            game_session.hotlap_relocate(&b, HotlapDestination::Track, false),
            Err("Practice is over")
        );
        drive_over_the_line(&mut game_session, a, 58);
        assert!(game_session.session.participants[&a].in_garage);
        assert_eq!(classify_laps(&mut game_session), 1);
        assert!(game_session.qualifying_order().is_empty());
        assert_eq!(game_session.session.state, SessionState::Finished);
        let state = game_session.weekend_state().unwrap();
        assert_eq!((state.phase, state.finished), (1, true));
        assert!(game_session.awaits_weekend_advance());
    }

    #[test]
    fn advancing_is_refused_outside_a_weekend_while_a_phase_runs_and_after_the_race() {
        assert_eq!(
            create_test_session().advance_weekend(),
            Err("Not a race weekend")
        );
        let (mut game_session, _) = weekend_of(2, 600, &[600]);
        game_session.advance_weekend().unwrap();
        assert_eq!(
            game_session.advance_weekend(),
            Err("Practice is still running")
        );
        end_phase(&mut game_session);
        game_session.advance_weekend().unwrap();
        assert_eq!(
            game_session.advance_weekend(),
            Err("Qualifying is still running")
        );
        end_phase(&mut game_session);
        game_session.advance_weekend().unwrap();
        // The race's grid card: tyres, setup and livery after qualifying.
        assert_eq!(game_session.session.game_mode, GameMode::Grid);
        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert_eq!(game_session.advance_weekend(), Err("The race has begun"));
        let humans: Vec<PlayerId> = game_session.session.participants.keys().copied().collect();
        for id in &humans {
            assert!(game_session.set_ready(*id, true));
        }
        for _ in 0..=START_SESSION_COUNTDOWN_SECONDS as u32 * DEFAULT_TICK_RATE_HZ as u32 + 1 {
            game_session.tick(&HashMap::new());
        }
        assert_eq!(game_session.session.game_mode, GameMode::Race);
        assert_eq!(game_session.advance_weekend(), Err("The race has begun"));
    }

    /// With no driver connected to say ready, the weekend's race is counted
    /// straight in rather than waiting on an empty grid.
    #[test]
    fn a_weekend_with_nobody_connected_counts_its_race_straight_in() {
        let (mut game_session, ids) = weekend_of(2, 0, &[600]);
        game_session.advance_weekend().unwrap();
        end_phase(&mut game_session);
        for id in &ids {
            assert!(game_session.hold_seat(id));
        }
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.game_mode, GameMode::Countdown);
        assert_eq!(game_session.session.next_mode, Some(GameMode::Race));
        assert_eq!(game_session.weekend_state().unwrap().phase, 3);
    }

    #[test]
    fn a_weekend_without_practice_starts_at_q1() {
        let (mut game_session, _) = weekend_of(2, 0, &[600]);
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.game_mode, GameMode::Qualification);
        let state = game_session.weekend_state().unwrap();
        assert_eq!(
            (state.phase, state.round, state.rounds, state.advancing),
            (2, 1, 1, 0)
        );
        assert_eq!(game_session.race_clock().unwrap().left_ms, 600_000);
    }

    /// Lobby, practice, Q1 (two knocked out, one of them with no time),
    /// Q2, the race: each phase's clock, the knockout's grid slots, the
    /// eliminated cars kept in, the race gridded by the weekend and the
    /// combined classification stored.
    #[test]
    fn a_weekend_runs_practice_a_knockout_and_grids_the_race() {
        let (mut game_session, ids) = weekend_of(4, 300, &[600, 480]);
        let (a, b, c, d) = (ids[0], ids[1], ids[2], ids[3]);
        // Asked-for orders give way to the weekend's grid.
        game_session.set_grid_order(vec!["D".into(), "C".into()]);

        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.race_clock().unwrap().left_ms, 300_000);
        end_phase(&mut game_session);

        // Q1: four cars, two go through.
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.game_mode, GameMode::Qualification);
        assert_eq!(game_session.session.state, SessionState::Racing);
        assert_eq!(game_session.weekend_progress().sizes, vec![4, 2]);
        assert_eq!(game_session.race_clock().unwrap().left_ms, 600_000);
        assert!(game_session.refuses_drivers().is_some());
        let state = game_session.weekend_state().unwrap();
        assert_eq!(
            (state.phase, state.round, state.rounds, state.advancing),
            (2, 1, 2, 2)
        );
        game_session.note_qualifying_lap(a, 80_000);
        game_session.note_qualifying_lap(c, 81_000);
        game_session.note_qualifying_lap(b, 82_000);
        end_phase(&mut game_session);
        let state = game_session.weekend_state().unwrap();
        assert!(state.finished);
        assert_eq!(
            state.eliminated,
            vec![
                KnockedOut {
                    car_index: 1,
                    round: 1,
                    grid_position: 3,
                    lap_time_ms: 82_000,
                    late: false,
                },
                KnockedOut {
                    car_index: 3,
                    round: 1,
                    grid_position: 4,
                    lap_time_ms: 0,
                    late: false,
                },
            ]
        );

        // Q2: a fresh clock and classification; the knocked out stay in.
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.race_clock().unwrap().left_ms, 480_000);
        assert!(game_session.qualifying_order().is_empty());
        let state = game_session.weekend_state().unwrap();
        assert_eq!(
            (state.round, state.advancing, state.finished),
            (2, 0, false)
        );
        assert_eq!(state.eliminated.len(), 2);
        assert_eq!(
            game_session.hotlap_relocate(&b, HotlapDestination::Track, false),
            Err("Eliminated in Q1")
        );
        assert_eq!(
            game_session.hotlap_relocate(&d, HotlapDestination::Track, false),
            Err("Eliminated in Q1")
        );
        assert!(game_session.session.participants[&b].in_garage);
        game_session
            .hotlap_relocate(&a, HotlapDestination::Track, false)
            .unwrap();
        game_session
            .hotlap_relocate(&a, HotlapDestination::Garage, false)
            .unwrap();
        game_session.note_qualifying_lap(c, 79_000);
        game_session.note_qualifying_lap(a, 79_500);
        end_phase(&mut game_session);
        assert_eq!(
            game_session.weekend_progress().grid(),
            Some(vec![c, a, b, d])
        );

        // The stored result: the grid, each with the lap that placed them,
        // the car with no time left out.
        let stored: Vec<(String, u32)> = game_session
            .qualifying_result()
            .entries
            .into_iter()
            .map(|e| (e.name, e.lap_time_ms))
            .collect();
        assert_eq!(
            stored,
            vec![
                ("C".to_string(), 79_000),
                ("A".to_string(), 79_500),
                ("B".to_string(), 82_000)
            ]
        );

        // The race grids by the weekend, over the asked-for order.
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.state, SessionState::Countdown);
        assert_eq!(game_session.session.next_mode, Some(GameMode::Race));
        let state = game_session.weekend_state().unwrap();
        assert_eq!((state.phase, state.round, state.finished), (3, 0, false));
        assert_eq!(state.eliminated.len(), 2);
        let slots: Vec<u8> = [c, a, b, d]
            .iter()
            .map(|id| slot(&game_session, id))
            .collect();
        assert_eq!(slots, vec![1, 2, 3, 4]);
        assert!(game_session
            .session
            .participants
            .values()
            .all(|s| !s.in_garage));
    }

    /// A weekend between phases takes drivers: after practice they are in
    /// Q1; after a round they take no part and start from the back. A
    /// running round and a finished race refuse them.
    #[test]
    fn a_weekend_takes_drivers_between_phases() {
        let (mut game_session, ids) = weekend_of(3, 600, &[600, 600]);
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.advance_weekend().unwrap();
        assert!(game_session.refuses_drivers().is_none(), "running practice");
        end_phase(&mut game_session);
        assert!(game_session.refuses_drivers().is_none(), "practice over");
        let early = Uuid::from_u128(10);
        game_session.add_player(early, car_id).unwrap();
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.weekend_progress().sizes, vec![4, 2]);
        assert!(game_session.refuses_drivers().is_some(), "Q1 running");
        for (id, ms) in ids
            .iter()
            .chain([&early])
            .zip([80_000, 81_000, 82_000, 83_000])
        {
            game_session.note_qualifying_lap(*id, ms);
        }
        end_phase(&mut game_session);
        assert!(game_session.refuses_drivers().is_none(), "between rounds");
        let late = Uuid::from_u128(11);
        game_session.add_player(late, car_id).unwrap();
        let entry = *game_session
            .weekend_progress()
            .eliminated_entry(&late)
            .unwrap();
        assert!(entry.late);
        assert_eq!(
            (entry.round, entry.grid_position, entry.lap_time_ms),
            (1, 5, 0)
        );
        game_session.advance_weekend().unwrap();
        assert_eq!(
            game_session.hotlap_relocate(&late, HotlapDestination::Track, false),
            Err("Joined after qualifying began: starts from the back")
        );
        game_session.note_qualifying_lap(ids[1], 79_000);
        game_session.note_qualifying_lap(ids[0], 79_500);
        end_phase(&mut game_session);
        assert_eq!(
            game_session.weekend_progress().grid(),
            Some(vec![ids[1], ids[0], ids[2], early, late])
        );
        game_session.advance_weekend().unwrap();
        assert!(game_session.refuses_drivers().is_none(), "the race's grid");
        game_session.session.state = SessionState::Finished;
        assert!(game_session.refuses_drivers().is_some(), "the race is over");
    }

    /// The 107% rule: a car slower than the cut-off of pole, or with no
    /// time, takes no grid slot and is parked out of the race; without a
    /// cut-off every car starts.
    #[test]
    fn the_qualifying_cutoff_keeps_slow_cars_off_the_grid() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_qualifying_cutoff(Some(107));
        assert!(
            game_session.non_qualifiers().is_empty(),
            "no qualifying yet"
        );
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(c, 86_000);
        game_session.note_qualifying_lap(a, 80_000);
        game_session.note_qualifying_lap(b, 85_000);
        let out: Vec<PlayerId> = game_session.non_qualifiers().into_iter().collect();
        assert_eq!(out, vec![c], "85.0 is inside 107% of 80.0, 86.0 is not");
        game_session.line_up_on_grid();
        assert_eq!((slot(&game_session, &a), slot(&game_session, &b)), (1, 2));
        let parked = &game_session.session.participants[&c];
        assert!(!parked.damage.is_drivable && parked.towed);
        assert!(game_session.session.participants[&a].damage.is_drivable);
        // The roster marks it, so a client shows DNQ rather than a retirement.
        assert!(game_session.take_roster_dirty());
        let roster = game_session.roster_rows(&HashMap::new());
        let dnq: Vec<PlayerId> = roster
            .iter()
            .filter(|e| e.dnq)
            .map(|e| e.player_id)
            .collect();
        assert_eq!(dnq, vec![c]);

        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(a, 80_000);
        game_session.note_qualifying_lap(b, 99_000);
        game_session.line_up_on_grid();
        assert!(game_session.non_qualifiers().is_empty());
        assert!(game_session
            .session
            .participants
            .values()
            .all(|s| s.damage.is_drivable));
        let _ = c;

        // A weekend measures it on Q1: no time is out too.
        let (mut game_session, ids) = weekend_of(4, 0, &[600]);
        game_session.set_qualifying_cutoff(Some(0));
        assert_eq!(game_session.session.qualifying_cutoff_pct, None);
        game_session.set_qualifying_cutoff(Some(300));
        assert_eq!(game_session.session.qualifying_cutoff_pct, Some(150));
        game_session.set_qualifying_cutoff(Some(107));
        game_session.advance_weekend().unwrap();
        for (id, ms) in ids.iter().zip([80_000, 90_000, 81_000, 0]) {
            game_session.note_qualifying_lap(*id, ms);
        }
        end_phase(&mut game_session);
        let out: Vec<PlayerId> = game_session.non_qualifiers().into_iter().collect();
        assert_eq!(out, vec![ids[1], ids[3]]);
        game_session.advance_weekend().unwrap();
        assert_eq!(
            (slot(&game_session, &ids[0]), slot(&game_session, &ids[2])),
            (1, 2)
        );
        assert!(out
            .iter()
            .all(|id| !game_session.session.participants[id].damage.is_drivable));
    }

    /// Two cars with no time are knocked out in seating order.
    #[test]
    fn cars_with_no_time_go_out_in_seating_order() {
        let (mut game_session, ids) = weekend_of(5, 0, &[600, 600, 600]);
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.weekend_progress().sizes, vec![5, 4, 3]);
        for (id, ms) in ids.iter().zip([83_000, 82_000, 0, 81_000, 0]) {
            game_session.note_qualifying_lap(*id, ms);
        }
        end_phase(&mut game_session);
        let out = &game_session.weekend_progress().eliminated;
        assert_eq!(out.len(), 1);
        assert_eq!((out[0].player_id, out[0].grid_position), (ids[4], 5));
        assert_eq!(out[0].lap_time_ms, 0);
        // Q2: of the four left, the slowest (the other with no time) goes.
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.weekend_state().unwrap().advancing, 3);
        for (id, ms) in ids.iter().zip([80_000, 80_500, 0, 79_000]) {
            game_session.note_qualifying_lap(*id, ms);
        }
        end_phase(&mut game_session);
        let out: Vec<(PlayerId, u8, u8)> = game_session
            .weekend_progress()
            .eliminated
            .iter()
            .map(|e| (e.player_id, e.round, e.grid_position))
            .collect();
        assert_eq!(out, vec![(ids[2], 2, 4), (ids[4], 1, 5)]);
    }

    /// The AI go out of their garages at each round's start; a knocked-out
    /// AI car stays in its garage.
    #[test]
    fn the_ai_go_out_each_round_and_a_knocked_out_ai_stays_in() {
        use crate::ai_driver::generate_default_ai_profiles;
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let car_id = car.id;
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car);
        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 3, 3);
        let mut game_session = GameSession::with_ai_profiles(
            session,
            track,
            car_configs,
            generate_default_ai_profiles(3),
        );
        game_session.set_weekend(Some(crate::weekend::WeekendFormat {
            practice_seconds: 0,
            qualifying_seconds: vec![600, 600],
            practice_conditions: None,
            qualifying_conditions: None,
            ..Default::default()
        }));
        game_session.spawn_ai_drivers();
        let human = Uuid::from_u128(1);
        game_session.add_player(human, car_id).unwrap();

        game_session.advance_weekend().unwrap();
        let ai = game_session.session.ai_player_ids.clone();
        for id in &ai {
            let car = &game_session.session.participants[id];
            assert!(!car.in_garage && car.outlap, "AI out on its outlap");
        }
        assert!(game_session.session.participants[&human].in_garage);
        // The cars still out take the flag at the deadline.
        game_session.note_qualifying_lap(human, 80_000);
        game_session.note_qualifying_lap(ai[0], 81_000);
        let start = game_session.timed_start_tick.unwrap();
        game_session.session.current_tick = start + 600 * DEFAULT_TICK_RATE_HZ as u32 - 1;
        game_session.tick(&HashMap::new());
        let deadline = game_session.finish_deadline_tick.unwrap();
        game_session.session.current_tick = deadline - 1;
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.state, SessionState::Finished);
        game_session.settle_weekend();
        let out: Vec<PlayerId> = game_session
            .weekend_progress()
            .eliminated
            .iter()
            .map(|e| e.player_id)
            .collect();
        assert_eq!(out.len(), 2);
        assert!(!out.contains(&human) && !out.contains(&ai[0]));

        game_session.advance_weekend().unwrap();
        assert!(!game_session.session.participants[&ai[0]].in_garage);
        for id in &out {
            assert!(game_session.session.participants[id].in_garage);
            assert_eq!(
                game_session.hotlap_relocate(id, HotlapDestination::Track, false),
                Err("Eliminated in Q1")
            );
        }
        for _ in 0..10 {
            game_session.tick(&HashMap::new());
        }
        for id in &out {
            assert!(game_session.session.participants[id].in_garage);
        }
    }

    /// The weekend's state is handed out once per change.
    #[test]
    fn a_lap_allowance_garages_a_car_after_its_laps_and_ends_qualifying() {
        let (mut game_session, a, b) = timed_qualifying(600);
        game_session.set_qualifying_lap_allowance(2);
        assert_eq!(game_session.session.qualifying_lap_allowance, 2);
        game_session.set_qualifying_lap_allowance(200);
        assert_eq!(
            game_session.session.qualifying_lap_allowance,
            crate::data::MAX_QUALIFYING_LAP_ALLOWANCE
        );
        game_session.set_qualifying_lap_allowance(2);
        game_session.session.current_tick += 200 * DEFAULT_TICK_RATE_HZ as u32;
        for id in [a, b] {
            game_session
                .hotlap_relocate(&id, HotlapDestination::Track, false)
                .unwrap();
        }
        let laps_in_roster = |gs: &GameSession, id: PlayerId| {
            gs.roster_rows(&HashMap::new())
                .iter()
                .find(|e| e.player_id == id)
                .map(|e| e.qualifying_laps)
        };
        assert_eq!(laps_in_roster(&game_session, a), Some(0));
        game_session.take_roster_dirty();
        drive_over_the_line(&mut game_session, a, 80);
        assert!(!game_session.session.participants[&a].in_garage);
        assert!(
            game_session.take_roster_dirty(),
            "a counted lap goes out in the roster"
        );
        assert_eq!(laps_in_roster(&game_session, a), Some(1));
        drive_over_the_line(&mut game_session, a, 80);
        assert_eq!(laps_in_roster(&game_session, a), Some(2));
        assert!(
            game_session.session.participants[&a].in_garage,
            "in after the second lap"
        );
        assert_eq!(classify_laps(&mut game_session), 2, "both laps count");
        assert_eq!(
            game_session.hotlap_relocate(&a, HotlapDestination::Track, false),
            Err("Lap allowance used")
        );
        assert_eq!(game_session.session.state, SessionState::Racing);
        drive_over_the_line(&mut game_session, b, 80);
        drive_over_the_line(&mut game_session, b, 80);
        assert!(game_session.session.participants[&b].in_garage);
        assert_eq!(
            game_session.session.state,
            SessionState::Finished,
            "every car has run its laps"
        );
        // A fresh qualifying session counts afresh.
        game_session.set_game_mode(GameMode::Qualification);
        assert_eq!(laps_in_roster(&game_session, a), Some(0));
        game_session
            .hotlap_relocate(&a, HotlapDestination::Track, false)
            .unwrap();
    }

    #[test]
    fn a_weekend_phase_runs_under_its_own_damage_rule() {
        let mut game_session = create_test_session();
        game_session.set_weekend(Some(crate::weekend::WeekendFormat {
            practice_seconds: 600,
            qualifying_seconds: vec![600],
            practice_damage: Some(DamageLevel::Off),
            qualifying_damage: Some(DamageLevel::VisualOnly),
            ..Default::default()
        }));
        game_session.set_damage(DamageLevel::Reduced);
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.session.host_player_id = a;
        let rule = |gs: &GameSession| {
            (
                gs.session.damage,
                gs.weekend_state().unwrap().damage,
                gs.session.participants[&a].damage_level,
            )
        };

        game_session.advance_weekend().unwrap();
        assert_eq!(
            rule(&game_session),
            (DamageLevel::Off, DamageLevel::Off, Some(DamageLevel::Off))
        );
        // A late arrival takes the phase's rule (and its garage, as the
        // join does).
        game_session.add_player(b, car_id).unwrap();
        game_session
            .hotlap_relocate(&b, HotlapDestination::Garage, false)
            .unwrap();
        assert_eq!(
            game_session.session.participants[&b].damage_level,
            Some(DamageLevel::Off)
        );
        end_phase(&mut game_session);

        game_session.advance_weekend().unwrap();
        let visual = Some(DamageLevel::VisualOnly);
        assert_eq!(
            rule(&game_session),
            (DamageLevel::VisualOnly, DamageLevel::VisualOnly, visual)
        );
        assert_eq!(game_session.session.participants[&b].damage_level, visual);
        end_phase(&mut game_session);

        // The race is the session's own rule.
        game_session.start_countdown_mode(1, GameMode::Race);
        let reduced = Some(DamageLevel::Reduced);
        assert_eq!(
            rule(&game_session),
            (DamageLevel::Reduced, DamageLevel::Reduced, reduced)
        );
        assert_eq!(game_session.session.participants[&b].damage_level, reduced);
    }

    #[test]
    fn a_weekend_without_phase_damage_keeps_the_sessions_rule() {
        let (mut game_session, ids) = weekend_of(2, 600, &[600]);
        game_session.set_damage(DamageLevel::Reduced);
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.session.damage, DamageLevel::Reduced);
        assert_eq!(
            game_session.session.participants[&ids[1]].damage_level,
            Some(DamageLevel::Reduced)
        );
        assert_eq!(
            game_session.weekend_state().unwrap().damage,
            DamageLevel::Reduced
        );
    }

    #[test]
    fn a_weekend_final_shootout_runs_the_cars_through_in_reverse() {
        let (mut game_session, ids) = weekend_of(4, 0, &[600, 600]);
        game_session.set_qualifying_format(crate::data::QualifyingFormat::Shootout);
        let (a, b, c, d) = (ids[0], ids[1], ids[2], ids[3]);

        // Q1 is the timed knockout as ever.
        game_session.advance_weekend().unwrap();
        assert!(!game_session.is_shootout_round());
        assert!(game_session.shootout_state().is_none());
        assert_eq!(game_session.race_clock().unwrap().left_ms, 600_000);
        game_session.note_qualifying_lap(c, 80_000);
        game_session.note_qualifying_lap(a, 81_000);
        game_session.note_qualifying_lap(b, 82_000);
        end_phase(&mut game_session);

        // Q2, the final, is the shoot-out: no clock, the slower of the two
        // through runs first, the other waits in its garage.
        game_session.advance_weekend().unwrap();
        assert!(game_session.is_shootout_round());
        assert_eq!(game_session.race_clock(), None);
        let state = game_session.shootout_state().unwrap();
        let index = |id: &PlayerId| game_session.car_index_of(id).unwrap();
        assert_eq!(state.order, vec![index(&a), index(&c)]);
        assert_eq!((state.running, state.done, state.flying), (1, 0, false));
        assert!(!game_session.session.participants[&a].in_garage);
        assert!(game_session.session.participants[&a].outlap);
        for id in [b, c, d] {
            assert!(game_session.session.participants[&id].in_garage);
        }
        assert_eq!(
            game_session.hotlap_relocate(&c, HotlapDestination::Track, false),
            Err("A shoot-out: each car is sent out in its turn")
        );
        assert_eq!(
            game_session.hotlap_relocate(&b, HotlapDestination::Track, false),
            Err("Eliminated in Q1")
        );

        // a ends its flying lap: in, and c is called.
        drive_over_the_line(&mut game_session, a, 79);
        assert_eq!(classify_laps(&mut game_session), 1);
        assert!(game_session.session.participants[&a].in_garage);
        assert!(!game_session.session.participants[&c].in_garage);
        let state = game_session.shootout_state().unwrap();
        assert_eq!((state.running, state.done), (2, 1));

        // c gives up its run: the shoot-out is over, and a's lap is pole.
        game_session
            .hotlap_relocate(&c, HotlapDestination::Garage, false)
            .unwrap();
        game_session.tick(&HashMap::new());
        assert_eq!(game_session.session.state, SessionState::Finished);
        let state = game_session.shootout_state().unwrap();
        assert_eq!((state.running, state.done), (0, 2));
        game_session.settle_weekend();
        assert_eq!(
            game_session.weekend_progress().grid(),
            Some(vec![a, c, b, d])
        );
    }

    #[test]
    fn a_shootout_runs_in_reverse_of_the_start_order_and_takes_a_newcomer_last() {
        let (mut game_session, ids) = three_drivers();
        game_session.set_qualifying_format(crate::data::QualifyingFormat::Shootout);
        game_session.set_qualifying_seconds(Some(600));
        let [a, b, c] = ids;
        game_session.set_grid_order(vec!["B".into(), "C".into(), "A".into()]);
        game_session.set_game_mode(GameMode::Qualification);
        assert_eq!(game_session.race_clock(), None, "a shoot-out has no clock");
        let index = |gs: &GameSession, id: &PlayerId| gs.car_index_of(id).unwrap();
        assert_eq!(
            game_session.shootout_state().unwrap().order,
            vec![
                index(&game_session, &a),
                index(&game_session, &c),
                index(&game_session, &b)
            ]
        );
        assert!(!game_session.session.participants[&a].in_garage);
        assert!(game_session.take_shootout_state().is_some());
        assert!(game_session.take_shootout_state().is_none(), "sent once");

        let car_id = game_session.car_configs.values().next().unwrap().id;
        let late = Uuid::from_u128(9);
        game_session.add_player(late, car_id).unwrap();
        game_session
            .hotlap_relocate(&late, HotlapDestination::Garage, false)
            .unwrap();
        game_session.tick(&HashMap::new());
        let state = game_session.shootout_state().unwrap();
        assert_eq!(state.order.len(), 4);
        assert_eq!(state.order[3], index(&game_session, &late));
        assert!(game_session.session.participants[&late].in_garage);

        // Leaving qualifying ends the shoot-out.
        game_session.start_countdown_mode(1, GameMode::Race);
        assert!(game_session.shootout_state().is_none());
    }

    #[test]
    fn the_weekend_state_goes_out_when_it_changes() {
        let (mut game_session, _) = weekend_of(2, 0, &[600]);
        assert_eq!(game_session.take_weekend_state().unwrap().phase, 0);
        assert!(game_session.take_weekend_state().is_none());
        game_session.advance_weekend().unwrap();
        assert_eq!(game_session.take_weekend_state().unwrap().phase, 2);
        assert!(game_session.take_weekend_state().is_none());
        run_out_the_clock(&mut game_session);
        assert!(game_session.take_weekend_state().unwrap().finished);
    }

    #[test]
    fn test_finished_human_is_driven_by_the_server_and_gets_aids_back() {
        let (mut game_session, winner, _) = racing_session_with_two_humans();
        let car = game_session.session.participants.get_mut(&winner).unwrap();
        car.current_lap = 4;
        car.steering_assist = true;

        // The client's last input stays in the server's map after it stops
        // sending: here, standing on the brake. Only the cool-down driver can
        // get the car moving.
        let stale = PlayerInputData {
            brake: 1.0,
            ..Default::default()
        };
        let inputs: HashMap<PlayerId, PlayerInputData> = [(winner, stale)].into();
        for _ in 0..(2 * DEFAULT_TICK_RATE_HZ) {
            game_session.tick(&inputs);
        }
        let car = &game_session.session.participants[&winner];
        assert!(car.finish_position.is_some());
        assert!(
            car.speed_mps > 2.0,
            "the stale input must not be driving the car, speed={}",
            car.speed_mps
        );
        assert!(
            !car.steering_assist,
            "the cool-down driver steers without the aid"
        );

        game_session.start_countdown_mode(5, GameMode::Race);
        let car = &game_session.session.participants[&winner];
        assert!(car.steering_assist, "lining up again restores the aid");
        assert_eq!(car.current_lap, 0);
        assert_eq!(car.finish_position, None);
        assert_eq!(game_session.finish_deadline_tick, None);
    }

    #[test]
    fn test_transition_from_countdown() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        game_session.transition_from_countdown(GameMode::FreePractice);

        assert_eq!(game_session.session.game_mode, GameMode::FreePractice);
        assert_eq!(game_session.session.countdown_ticks_remaining, None);
    }

    #[test]
    fn test_transition_from_countdown_to_demolap() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Countdown);

        game_session.transition_from_countdown(GameMode::DemoLap);

        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
        assert!(game_session.session.demo_lap_progress.is_some());
    }

    #[test]
    fn test_replay_mode_does_nothing() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Replay);

        let initial_tick = game_session.session.current_tick;
        let inputs = HashMap::new();

        game_session.tick(&inputs);

        // Tick should increment but nothing else happens
        assert_eq!(game_session.session.current_tick, initial_tick + 1);
    }

    #[test]
    fn test_demolap_without_raceline() {
        let mut game_session = create_test_session();

        // Add a demo car
        let player_id = Uuid::new_v4();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.add_player(player_id, car_id);

        // Clear raceline
        game_session.track_config.raceline.clear();

        game_session.set_game_mode(GameMode::DemoLap);

        let inputs = HashMap::new();
        game_session.tick(&inputs);

        // Should not crash when raceline is empty
        assert_eq!(game_session.session.game_mode, GameMode::DemoLap);
    }

    #[test]
    fn test_mode_persists_across_ticks() {
        let mut game_session = create_test_session();
        game_session.set_game_mode(GameMode::Sandbox);

        let inputs = HashMap::new();
        for _ in 0..10 {
            game_session.tick(&inputs);
        }

        // Mode should still be Sandbox
        assert_eq!(game_session.session.game_mode, GameMode::Sandbox);
    }

    #[test]
    fn test_ai_driver_integration_demo_lap() {
        use crate::ai_driver::generate_default_ai_profiles;

        // Create a track with a simple racing line
        let mut track = TrackConfig::default();

        // Create a simple circular track layout for testing
        // 100m radius circle, 628m total length
        let num_points = 32;
        let radius = 100.0;
        let mut raceline = Vec::new();
        let mut centerline = Vec::new();

        for i in 0..num_points {
            let angle = (i as f32 / num_points as f32) * 2.0 * std::f32::consts::PI;
            let x = angle.cos() * radius;
            let y = angle.sin() * radius;
            let distance = (i as f32 / num_points as f32) * 2.0 * std::f32::consts::PI * radius;

            raceline.push(RacelinePoint { x, y, z: 0.0 });
            centerline.push(crate::data::TrackPoint {
                x,
                y,
                z: 0.0,
                distance_from_start_m: distance,
                width_left_m: 10.0,
                width_right_m: 10.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: angle + std::f32::consts::FRAC_PI_2,
                grip_modifier: 1.0,
                surface_type: SurfaceType::Asphalt,
            });
        }

        track.raceline = raceline;
        track.centerline = centerline;
        track.rebuild_raceline_distances();

        // Create session with one AI driver
        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 1, 1);
        let ai_profiles = generate_default_ai_profiles(1);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);

        // Spawn the AI driver
        game_session.spawn_ai_drivers();
        assert_eq!(game_session.session.participants.len(), 1);

        // Get the AI player ID
        let ai_player_id = *game_session.session.ai_player_ids.first().unwrap();

        // Set to FreePractice mode to test AI driving
        game_session.set_game_mode(GameMode::FreePractice);

        // Give the AI car initial speed to avoid stall (physics limitation)
        // In a real sim, clutch modulation would handle launch
        if let Some(ai_state) = game_session.session.participants.get_mut(&ai_player_id) {
            ai_state.speed_mps = 5.0; // Start with 5 m/s (18 km/h)
            ai_state.vel_x = 5.0;
            ai_state.engine_rpm = 2000.0; // Start engine above idle
                                          // And on warm tyres: this is about the AI driving, not about
                                          // cold tyres (tyre_temperature_test). Unfitted tyres are put
                                          // at their optimum on the first tick.
            ai_state.tyres_fitted = false;
        }

        // Run simulation for 2 seconds.
        for _ in 0..(2 * DEFAULT_TICK_RATE_HZ) {
            // Generate AI inputs for all AI players
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);
        }

        // Verify AI driver state
        let ai_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();

        // AI should have moved from starting position
        let start_pos = &game_session.track_config.start_positions[0];
        let distance_moved = ((ai_state.pos_x - start_pos.x).powi(2)
            + (ai_state.pos_y - start_pos.y).powi(2))
        .sqrt();

        assert!(
            distance_moved > 10.0,
            "AI should have moved at least 10m from start position (started at 5m/s), moved: {}m",
            distance_moved
        );

        // AI should have positive speed
        assert!(
            ai_state.speed_mps > 0.0,
            "AI should be moving, speed: {} m/s",
            ai_state.speed_mps
        );

        // AI position should be reasonably close to the track centerline
        // Find nearest track point
        let nearest_track_point = game_session
            .track_config
            .centerline
            .iter()
            .min_by_key(|p| {
                let dx = p.x - ai_state.pos_x;
                let dy = p.y - ai_state.pos_y;
                ((dx * dx + dy * dy) * 1000.0) as i32
            })
            .unwrap();

        let distance_from_centerline = ((ai_state.pos_x - nearest_track_point.x).powi(2)
            + (ai_state.pos_y - nearest_track_point.y).powi(2))
        .sqrt();

        // AI should stay within 50m of centerline (generous tolerance for test)
        assert!(
            distance_from_centerline < 50.0,
            "AI should stay close to track centerline, distance: {}m",
            distance_from_centerline
        );

        // AI should be generating valid inputs
        let ai_input = game_session.generate_ai_input(&ai_player_id);
        assert!(ai_input.throttle >= 0.0 && ai_input.throttle <= 1.0);
        assert!(ai_input.brake >= 0.0 && ai_input.brake <= 1.0);
        assert!(ai_input.steering >= -1.0 && ai_input.steering <= 1.0);
        assert!(ai_input.gear.is_some());

        // AI should be in a reasonable gear
        if let Some(gear) = ai_input.gear {
            assert!(
                (1..=6).contains(&gear),
                "AI gear should be between 1 and 6, got: {}",
                gear
            );
        }
    }

    #[test]
    fn test_ai_driver_follows_racing_line() {
        use crate::ai_driver::AiDriverProfile;

        // Create a straight track for easier validation
        let mut track = TrackConfig::default();

        // Create a 500m straight track
        let num_points = 50;
        let mut raceline = Vec::new();
        let mut centerline = Vec::new();

        for i in 0..num_points {
            let x = i as f32 * 10.0; // 10m spacing
            let y = 0.0;
            let distance = i as f32 * 10.0;

            raceline.push(RacelinePoint { x, y, z: 0.0 });
            centerline.push(crate::data::TrackPoint {
                x,
                y,
                z: 0.0,
                distance_from_start_m: distance,
                width_left_m: 10.0,
                width_right_m: 10.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0, // Straight track, heading east
                grip_modifier: 1.0,
                surface_type: SurfaceType::Asphalt,
            });
        }

        track.raceline = raceline;
        track.centerline = centerline;
        track.rebuild_raceline_distances();

        // Create high-skill AI (should be very precise)
        let ai_profile = AiDriverProfile::new("Test AI", 105);
        let ai_player_id = ai_profile.id;

        let car = CarConfig::default();
        let mut car_configs = HashMap::new();
        car_configs.insert(car.id, car.clone());

        let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 1, 1);

        let mut game_session =
            GameSession::with_ai_profiles(session, track, car_configs, vec![ai_profile]);

        // Spawn AI and add to session
        game_session.spawn_ai_drivers();

        // Start in free practice mode
        game_session.set_game_mode(GameMode::FreePractice);

        // Give the AI car initial speed to avoid stall
        if let Some(ai_state) = game_session.session.participants.get_mut(&ai_player_id) {
            ai_state.speed_mps = 10.0; // Start with 10 m/s
            ai_state.vel_x = 10.0;
            ai_state.engine_rpm = 3000.0;
        }

        // Run for 3 seconds to let AI stabilize
        for _ in 0..720 {
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);
        }

        // Check AI position over next 1 second, verifying it stays on line
        let mut max_lateral_deviation = 0.0f32;

        for _ in 0..DEFAULT_TICK_RATE_HZ {
            let mut inputs = HashMap::new();
            for ai_id in &game_session.session.ai_player_ids {
                let ai_input = game_session.generate_ai_input(ai_id);
                inputs.insert(*ai_id, ai_input);
            }
            game_session.tick(&inputs);

            if let Some(ai_state) = game_session.session.participants.get(&ai_player_id) {
                // Y should be close to 0 for straight track
                let lateral_deviation = ai_state.pos_y.abs();
                max_lateral_deviation = max_lateral_deviation.max(lateral_deviation);
            }
        }

        // High-skill AI should stay within 20m of the racing line on a straight
        assert!(
            max_lateral_deviation < 20.0,
            "High-skill AI should stay close to racing line, max deviation: {}m",
            max_lateral_deviation
        );

        // Verify AI is making forward progress
        let final_state = game_session
            .session
            .participants
            .get(&ai_player_id)
            .unwrap();
        assert!(
            final_state.pos_x > 50.0,
            "AI should have made significant forward progress, x position: {}m",
            final_state.pos_x
        );
    }

    fn classed_car(class: &str, id: u128) -> CarConfig {
        CarConfig {
            id: Uuid::from_u128(id),
            class: class.to_string(),
            ..CarConfig::default()
        }
    }

    #[test]
    fn class_field_deals_the_host_class_starting_after_the_host() {
        let cars: HashMap<CarConfigId, CarConfig> = [
            classed_car("GT3", 1),
            classed_car("LMP2", 2),
            classed_car("gt3", 3),
            classed_car("GT3", 4),
            classed_car("", 5),
        ]
        .into_iter()
        .map(|c| (c.id, c))
        .collect();

        let gt3 = class_field(&cars, Uuid::from_u128(3));
        assert_eq!(
            gt3,
            vec![Uuid::from_u128(4), Uuid::from_u128(1), Uuid::from_u128(3)],
            "every GT3 (class compared case-blind), host's model last"
        );
        assert_eq!(
            class_field(&cars, Uuid::from_u128(2)),
            vec![Uuid::from_u128(2)]
        );
        assert_eq!(
            class_field(&cars, Uuid::from_u128(5)),
            vec![Uuid::from_u128(5)],
            "an unclassed car races only against itself"
        );
    }

    #[test]
    fn ai_field_is_a_mix_of_the_host_class() {
        let cars = [
            classed_car("GT3", 1),
            classed_car("GT3", 2),
            classed_car("LMP2", 3),
            classed_car("GT3", 4),
        ];
        let car_configs: HashMap<CarConfigId, CarConfig> =
            cars.iter().map(|c| (c.id, c.clone())).collect();
        let track = TrackConfig::default();
        let mut session =
            RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 5, 3);
        session.host_car_id = Some(Uuid::from_u128(2));
        let mut game_session = GameSession::with_ai_profiles(
            session,
            track,
            car_configs,
            crate::ai_driver::generate_default_ai_profiles(5),
        );
        game_session.spawn_ai_drivers();

        let driven: Vec<CarConfigId> = game_session
            .session
            .ai_player_ids
            .iter()
            .map(|id| game_session.session.participants[id].car_config_id)
            .collect();
        assert_eq!(driven.len(), 5);
        assert!(
            !driven.contains(&Uuid::from_u128(3)),
            "no LMP2 in a GT3 field"
        );
        for gt3 in [1, 2, 4] {
            assert!(
                driven.contains(&Uuid::from_u128(gt3)),
                "GT3 {gt3} is on the grid"
            );
        }

        let roster = game_session.roster_rows(&HashMap::new());
        for entry in &roster {
            assert_eq!(
                entry.car_config_id,
                game_session.session.participants[&entry.player_id].car_config_id
            );
        }
    }

    #[test]
    fn test_demo_driver_uses_host_car() {
        // Create a session with a specific host car
        let track = TrackConfig::default();
        let car1 = CarConfig::default();
        let car2 = CarConfig {
            id: Uuid::new_v4(), // Different ID
            name: "Test Car 2".to_string(),
            ..CarConfig::default()
        };

        let mut car_configs = HashMap::new();
        car_configs.insert(car1.id, car1.clone());
        car_configs.insert(car2.id, car2.clone());

        let host_id = Uuid::new_v4();
        let mut session = RaceSession::new(host_id, track.id, SessionKind::Multiplayer, 8, 0, 3);
        session.host_car_id = Some(car2.id); // Host selected car2

        let mut game_session = GameSession::new(session, track, car_configs);

        // Set to DemoLap mode, which should create a demo driver with the host's car
        game_session.set_game_mode(GameMode::DemoLap);

        // Verify demo driver was created
        assert_eq!(game_session.session.ai_player_ids.len(), 1);

        // Verify the demo driver has a car state
        let demo_driver_id = game_session.session.ai_player_ids[0];
        let car_state = game_session
            .session
            .participants
            .get(&demo_driver_id)
            .expect("Demo driver should have a car state");

        // Verify the demo driver is using the host's selected car (car2)
        assert_eq!(
            car_state.car_config_id, car2.id,
            "Demo driver should use the host's selected car"
        );
    }

    /// A session that forbids an aid pins it off for every driver, whatever
    /// they ask for and whatever the car's own file says; one that allows
    /// them passes the request through.
    #[test]
    fn a_session_forbidding_aids_pins_them_off() {
        let everything = DriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(true),
            traction_control: Some(TractionControl::High),
            pit_autopilot: Some(true),
        };

        let mut strict = create_test_session();
        strict.session.allowed_assists = AllowedAssists::NONE;
        let player = Uuid::new_v4();
        let car_id = strict.car_configs.values().next().unwrap().id;
        strict.add_player(player, car_id).unwrap();
        let seated = strict.session.participants[&player].driver_aids();
        assert_eq!(
            seated.abs,
            Some(false),
            "a client that never sends its aids must not get the car's ABS"
        );
        assert_eq!(seated.traction_control, Some(TractionControl::Off));

        let applied = strict.set_driver_aids(&player, everything).unwrap();
        assert_eq!(
            applied,
            DriverAids {
                auto_gearbox: false,
                steering_assist: false,
                abs: Some(false),
                traction_control: Some(TractionControl::Off),
                pit_autopilot: Some(true),
            }
        );
        assert_eq!(strict.session.participants[&player].driver_aids(), applied);

        strict.line_up_on_grid();
        assert_eq!(
            strict.session.participants[&player].driver_aids(),
            applied,
            "lining up again keeps the clamped aids"
        );

        let mut open = create_test_session();
        open.add_player(player, car_id).unwrap();
        let seated = open.session.participants[&player].driver_aids();
        assert_eq!(
            seated.abs, None,
            "allowed: the car's own until the client says"
        );
        assert_eq!(open.set_driver_aids(&player, everything), Some(everything));

        let mut no_tc = create_test_session();
        no_tc.session.allowed_assists = AllowedAssists {
            traction_control: false,
            ..AllowedAssists::ALL
        };
        no_tc.add_player(player, car_id).unwrap();
        let applied = no_tc.set_driver_aids(&player, everything).unwrap();
        assert_eq!(applied.traction_control, Some(TractionControl::Off));
        assert_eq!(applied.abs, Some(true));
        assert!(applied.auto_gearbox && applied.steering_assist);
        assert_eq!(no_tc.set_driver_aids(&Uuid::new_v4(), everything), None);
    }

    /// Damage is the session's rule, not a driver's: every car takes the
    /// same, those seated before the host set it (the AI) and after, and
    /// nothing a driver sends changes it.
    #[test]
    fn every_car_takes_the_sessions_damage() {
        let mut session = create_test_session();
        let car_id = session.car_configs.values().next().unwrap().id;
        let early = Uuid::new_v4();
        session.add_player(early, car_id).unwrap();
        assert_eq!(
            session.session.participants[&early].damage_level,
            Some(DamageLevel::Full),
            "full damage unless the host says otherwise"
        );

        session.set_damage(DamageLevel::Off);
        let late = Uuid::new_v4();
        session.add_player(late, car_id).unwrap();
        for player in [early, late] {
            let car = &session.session.participants[&player];
            assert_eq!(car.damage_level, Some(DamageLevel::Off));
            assert_eq!(car.damage_scale(), 0.0);
        }

        session.set_driver_aids(
            &late,
            DriverAids {
                auto_gearbox: true,
                steering_assist: true,
                abs: Some(true),
                traction_control: Some(TractionControl::High),
                pit_autopilot: Some(true),
            },
        );
        session.line_up_on_grid();
        assert_eq!(
            session.session.participants[&late].damage_level,
            Some(DamageLevel::Off),
            "the aids and the grid leave the session's damage alone"
        );

        session.set_damage(DamageLevel::Reduced);
        assert_eq!(
            session.session.participants[&early].damage_scale(),
            DamageLevel::REDUCED_SHARE
        );
    }

    /// Three humans seated in join order, named "A", "B" and "C".
    fn three_drivers() -> (GameSession, [PlayerId; 3]) {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let ids = [Uuid::new_v4(), Uuid::new_v4(), Uuid::new_v4()];
        for (id, name) in ids.iter().zip(["A", "B", "C"]) {
            game_session.set_driver_name(*id, name);
            assert!(game_session.add_player(*id, car_id).is_some());
        }
        (game_session, ids)
    }

    fn slot(game_session: &GameSession, id: &PlayerId) -> u8 {
        game_session.session.participants[id].grid_position
    }

    #[test]
    fn a_requested_grid_order_seats_the_race_grid() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_grid_order(vec!["C".into(), "A".into()]);
        game_session.line_up_on_grid();
        // C and A as asked, the unlisted B behind them.
        assert_eq!(
            (
                slot(&game_session, &c),
                slot(&game_session, &a),
                slot(&game_session, &b)
            ),
            (1, 2, 3)
        );
    }

    #[test]
    fn a_reverse_grid_turns_the_front_of_the_order_round() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_race_rules(crate::data::RaceRules {
            reverse_grid: 2,
            ..Default::default()
        });
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(a, 90_000);
        game_session.note_qualifying_lap(b, 91_000);
        game_session.note_qualifying_lap(c, 92_000);
        assert_eq!(game_session.starting_slots()[&b], 1);
        game_session.line_up_on_grid();
        // The two fastest swap; the third keeps the slot it earned.
        assert_eq!(
            (
                slot(&game_session, &b),
                slot(&game_session, &a),
                slot(&game_session, &c)
            ),
            (1, 2, 3)
        );
        // Longer than the field: the whole order reversed, the host's
        // order and the seating order alike.
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_race_rules(crate::data::RaceRules {
            reverse_grid: 10,
            ..Default::default()
        });
        game_session.set_grid_order(vec!["B".into(), "A".into(), "C".into()]);
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &c),
                slot(&game_session, &a),
                slot(&game_session, &b)
            ),
            (1, 2, 3)
        );
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_race_rules(crate::data::RaceRules {
            reverse_grid: 3,
            ..Default::default()
        });
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &c),
                slot(&game_session, &b),
                slot(&game_session, &a)
            ),
            (1, 2, 3)
        );
    }

    #[test]
    fn without_an_order_or_qualifying_the_seating_stands() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &a),
                slot(&game_session, &b),
                slot(&game_session, &c)
            ),
            (1, 2, 3)
        );
    }

    #[test]
    fn a_driver_who_is_not_there_leaves_the_slot_empty() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_grid_order(vec!["Ghost".into(), "B".into(), "C".into()]);
        assert_eq!(game_session.starting_slots()[&b], 2);
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &b),
                slot(&game_session, &c),
                slot(&game_session, &a)
            ),
            (2, 3, 4)
        );
        // A qualifier who left: the slot they earned stays empty.
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(a, 90_000);
        game_session.note_qualifying_lap(b, 91_000);
        game_session.note_qualifying_lap(c, 92_000);
        game_session.remove_player(&a);
        game_session.line_up_on_grid();
        assert_eq!((slot(&game_session, &b), slot(&game_session, &c)), (2, 3));
        // A driver joining once the grid is up starts behind the last car,
        // not in the empty slot.
        let (mut game_session, [_, _, _]) = three_drivers();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        game_session.set_grid_order(vec!["Ghost".into(), "B".into(), "C".into()]);
        game_session.open_grid(5);
        assert_eq!(game_session.add_player(Uuid::new_v4(), car_id), Some(5));
        // A grid with no slot to spare closes up instead.
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.track_config.start_positions.truncate(3);
        game_session.set_grid_order(vec!["Ghost".into(), "B".into(), "C".into()]);
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &b),
                slot(&game_session, &c),
                slot(&game_session, &a)
            ),
            (1, 2, 3)
        );
    }

    #[test]
    fn qualifying_classifies_by_best_legal_lap_and_grids_the_race() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_game_mode(GameMode::Qualification);
        // Laps outside a qualifying session count for nothing.
        assert!(game_session.note_qualifying_lap(a, 90_000).is_some());
        assert!(game_session.note_qualifying_lap(a, 91_000).is_none());
        assert!(game_session.note_qualifying_lap(c, 88_000).is_some());
        // The same time a tick later starts behind.
        game_session.session.current_tick += 1;
        let result = game_session.note_qualifying_lap(b, 90_000).unwrap();
        assert_eq!(game_session.qualifying_order(), vec![c, a, b]);
        let names: Vec<_> = result.entries.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["C", "A", "B"]);
        game_session.line_up_on_grid();
        assert_eq!(
            (
                slot(&game_session, &c),
                slot(&game_session, &a),
                slot(&game_session, &b)
            ),
            (1, 2, 3)
        );
    }

    #[test]
    fn a_driver_with_no_time_starts_behind_the_classified() {
        let (mut game_session, [a, b, c]) = three_drivers();
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(c, 88_000);
        game_session.line_up_on_grid();
        assert_eq!(slot(&game_session, &c), 1);
        assert_eq!((slot(&game_session, &a), slot(&game_session, &b)), (2, 3));
    }

    #[test]
    fn the_sessions_own_qualifying_replaces_a_requested_order() {
        let (mut game_session, [a, _b, c]) = three_drivers();
        game_session.set_grid_order(vec!["A".into()]);
        game_session.set_game_mode(GameMode::Qualification);
        game_session.note_qualifying_lap(c, 88_000);
        assert!(game_session.session.grid_order.is_empty());
        game_session.line_up_on_grid();
        assert_eq!(slot(&game_session, &c), 1);
        assert_eq!(slot(&game_session, &a), 2);
    }

    #[test]
    fn an_order_can_name_the_host_and_the_ai_by_seat() {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let host = game_session.session.host_player_id;
        game_session.set_driver_name(host, "Me");
        game_session.add_player(host, car_id);
        let ai: Vec<PlayerId> = (0..2).map(|_| Uuid::new_v4()).collect();
        for id in &ai {
            game_session.add_player(*id, car_id);
            game_session.session.ai_player_ids.push(*id);
        }
        game_session.set_grid_order(vec!["@ai:2".into(), "@host".into(), "@ai:1".into()]);
        game_session.line_up_on_grid();
        assert_eq!(slot(&game_session, &ai[1]), 1);
        assert_eq!(slot(&game_session, &host), 2);
        assert_eq!(slot(&game_session, &ai[0]), 3);
    }
}
