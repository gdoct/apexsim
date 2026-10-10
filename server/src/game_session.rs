use crate::ai_driver::{AiDriverController, AiDriverProfile, TrafficCar};
use crate::car_setup::CarSetup;
use crate::data::*;
use crate::laps::{LapEvent, SECTOR_COUNT};
use crate::network::*;
use crate::physics;
use crate::racing_line::{self, RacingLineProfile};
use crate::records::{GhostLap, GhostSample, GHOST_SAMPLE_HZ};
use crate::tyre_thermal;
use std::collections::HashMap;
use tracing::debug;

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
pub const DEBRIS_PUNCTURE_CHANCE: f32 = 0.3;
/// Half the punctures are slow: a leak of this many kPa/s at most and at
/// least, which has the tyre flat in half a lap to a few laps.
const SLOW_LEAK_KPA_PER_S: (f32, f32) = (1.0, 6.0);

/// A piece of debris on the road.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Debris {
    pub x: f32,
    pub y: f32,
    pub until_tick: u32,
}

/// How a hashed puncture goes: a slow leak (half the time), or at once.
fn slow_leak(seed: u64) -> Option<f32> {
    if crate::wind::hash01(seed, 0x5107) < 0.5 {
        let (lo, hi) = SLOW_LEAK_KPA_PER_S;
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

/// The live sky of `session` over `track` (`crate::conditions`).
fn live_sky(session: &RaceSession, track: &TrackConfig) -> crate::conditions::LiveConditions {
    crate::conditions::LiveConditions::new(
        session.conditions,
        track.latitude_deg(),
        track.metadata.altitude_m.unwrap_or(0.0),
        session.id.as_u64_pair().0,
    )
}

/// The AI's speed plans per car: one for every this-many-th of its tank
/// (`GameSession::ai_speed_profile`). Each is a millisecond or so to build.
pub const AI_FUEL_PLAN_STEPS: usize = 10;

/// Seconds an AI coasts before each braking zone per share of the race's
/// fuel it is short (`GameSession::fuel_save_coast_s`): a coast of a
/// second before each of a lap's braking zones saves something like a
/// tenth of its fuel, so 5% short is a coast of about half a second.
pub const FUEL_SAVE_COAST_S_PER_SHORT: f32 = 10.0;
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
/// A watched car that makes no headway for this long is put back on the
/// run-up (`GameSession::update_hotlap_watch`): the AI backs out of a wall
/// by itself, this is for the day it cannot.
pub const HOTLAP_WATCH_STUCK_S: f32 = 8.0;
/// Under this speed a watched car counts as standing still, m/s.
const HOTLAP_WATCH_STUCK_MPS: f32 = 1.5;

/// A watched hotlap (`SessionKind::HotlapWatch`): the session's one AI car
/// laps the track alone, every lap on fresh tyres and a fresh tank.
#[derive(Debug, Clone, Copy, Default)]
struct HotlapWatch {
    active: bool,
    /// Tick until which the car is held on the run-up.
    hold_until_tick: u32,
    /// Ticks the car has stood still (not held) so far.
    stuck_ticks: u32,
    /// The lap counter last tick: a change is the car crossing the line.
    last_lap: Option<u16>,
    /// How far the driver has been eased off since the lap began to go
    /// wrong ([`hotlap_watch_driver`]): 0 is the line itself at the top of
    /// the scale, which some circuits' rails and stands do not leave room
    /// for; each step after is a driver a little less exact.
    level: u8,
}

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
    /// Simulation tick rate (Hz); the fixed timestep is `1 / tick_rate_hz`.
    tick_rate_hz: u16,
    /// The names of the humans seated, set as they join (the AI's are on
    /// their profiles): what the start order's name references match.
    driver_names: std::collections::BTreeMap<PlayerId, String>,
    /// Each driver's best legal qualifying lap, ms, and the tick it was set
    /// on (the earlier of two equal times starts ahead).
    qualifying: std::collections::BTreeMap<PlayerId, (u32, u32)>,
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
    pub(crate) finished_since_loop_tick: Option<u64>,
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
    /// Litres a lap of this track costs each car (`racing_line::lap_fuel_liters`),
    /// worked out the first time the car is fuelled. Looked up by key only.
    lap_fuel: HashMap<CarConfigId, f32>,
    /// Each car's ideal lap here, s (`racing_line::lap_time_s`), worked out
    /// with its lap fuel: how many laps a timed race is. Key lookups only.
    lap_seconds: HashMap<CarConfigId, f32>,
    /// How the race's end stands: its final lap, who has the flag.
    race_end: RaceEnd,
    /// The livery each human driver picked (`SelectCar`); AI drivers are
    /// dealt one in `build_roster`. Looked up by key only.
    liveries: HashMap<PlayerId, u8>,
    /// Timing lines crossed since the game loop last drained them.
    lap_events: Vec<SessionLapEvent>,
    /// Pit stops started since the clients were last told.
    pit_events: Vec<(PlayerId, crate::network::PitServiceData)>,
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
    track_corners: Vec<crate::network::CornerData>,
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

/// A car taking the flag.
#[derive(Debug, Clone, Copy)]
struct Finisher {
    /// Laps completed.
    laps: u16,
    tick: u32,
    /// How far past the line it was on that tick, m: who crossed first.
    progress: f32,
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
    /// The lap's trace, taken when a human driver set a personal best on a
    /// legal lap. The store decides whether it beats their record.
    pub ghost: Option<GhostLap>,
}

/// Longest ghost trace held for one car: five minutes at [`GHOST_SAMPLE_HZ`].
/// A car parked on the road must not grow a buffer without end.
const MAX_GHOST_SAMPLES: usize = (GHOST_SAMPLE_HZ as usize) * 300;

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

/// Append this tick's pose to a human driver's ghost trace, at
/// [`GHOST_SAMPLE_HZ`]. Called before the progress update, so the sample's
/// time is measured against the lap the car is still on.
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
    /// formation lap there is none of.
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
    // tyre for a stock pick in the rain, and for the AI), from the car's
    // own list.
    let compound = car_setups
        .get(&state.player_id)
        .copied()
        .unwrap_or_default()
        .compound_index_for(tyre, track.track_surface.water);
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
            roster_dirty: true,
            listed_state: SessionState::Lobby,
            ai_speed_profiles: HashMap::new(),
            finish_deadline_tick: None,
            finished_since_loop_tick: None,
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            car_setups: HashMap::new(),
            lap_fuel: HashMap::new(),
            lap_seconds: HashMap::new(),
            race_end: RaceEnd::default(),
            lap_events: Vec::new(),
            pit_events: Vec::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
            debris: Vec::new(),
            sky,
            debug: crate::debug_hooks::DebugHooks::default(),
            debug_green_ticks: 0,
            debug_next_event: 0,
            debug_boost_until: std::collections::BTreeMap::new(),
            hotlap_watch: HotlapWatch::default(),
            track_corners: Vec::new(),
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
            roster_dirty: true,
            listed_state: SessionState::Lobby,
            ai_speed_profiles: HashMap::new(),
            finish_deadline_tick: None,
            finished_since_loop_tick: None,
            held_steering_assist: HashMap::new(),
            tuned_configs: HashMap::new(),
            liveries: HashMap::new(),
            car_setups: HashMap::new(),
            lap_fuel: HashMap::new(),
            lap_seconds: HashMap::new(),
            race_end: RaceEnd::default(),
            lap_events: Vec::new(),
            pit_events: Vec::new(),
            lap_traces: HashMap::new(),
            session_best_lap_ms: None,
            session_best_splits_ms: [None; SECTOR_COUNT],
            debris: Vec::new(),
            sky,
            debug: crate::debug_hooks::DebugHooks::default(),
            debug_green_ticks: 0,
            debug_next_event: 0,
            debug_boost_until: std::collections::BTreeMap::new(),
            hotlap_watch: HotlapWatch::default(),
            track_corners: Vec::new(),
        }
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
            GameMode::DemoLap => {
                self.tick_demolap(inputs);
            }
            GameMode::FreePractice => {
                self.tick_free_practice(inputs);
            }
            GameMode::Replay => {
                self.tick_replay();
            }
            GameMode::Race => {
                self.tick_racing(inputs);
            }
            GameMode::Qualification | GameMode::Hotlap => {
                // Qualifying runs like a hotlap: every human starts in the
                // garage, frozen and out of the collision passes, until
                // they go out; the AI drive on.
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
        // The pit lane is shut while the field lines up: the exit light
        // shows red until the start (`crate::pit::exit_closed`).
        self.update_pits();
        if let Some(ref mut countdown) = self.session.countdown_ticks_remaining {
            if *countdown > 0 {
                *countdown -= 1;
            } else {
                // Countdown finished: transition to the stored next mode
                // (set by start_countdown_mode). Without a stored next mode
                // we just clear the countdown, as before.
                self.session.countdown_ticks_remaining = None;
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
        let dt = self.dt();
        for state in self.session.participants.values_mut() {
            let input = inputs.get(&state.player_id).copied().unwrap_or_default();
            if let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state) {
                physics::update_car_on_grid(state, config, &input, dt);
            }
        }
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
            let mut states: Vec<&mut CarState> = self.session.participants.values_mut().collect();
            for state in states.iter_mut() {
                let input = inputs.get(&state.player_id).copied().unwrap_or_default();

                if let Some(config) =
                    simulated_config(&self.car_configs, &self.tuned_configs, state)
                {
                    physics::update_car_3d(state, config, &input, &self.track_config, dt);
                    let is_ai = ai_ids.contains(&state.player_id);
                    sample_ghost_trace(
                        &mut self.lap_traces,
                        state,
                        is_ai,
                        current_tick,
                        self.tick_rate_hz,
                    );
                    if let Some(event) = physics::update_track_progress_3d(
                        state,
                        &self.track_config,
                        current_tick,
                        self.tick_rate_hz,
                    ) {
                        note_lap_event(
                            &mut self.lap_events,
                            &mut self.lap_traces,
                            &mut self.session_best_lap_ms,
                            &mut self.session_best_splits_ms,
                            state.player_id,
                            is_ai,
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
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );

                // Update track progress; a timing line crossed here becomes
                // a split on every driver's screen.
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
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
                physics::update_car_3d(state, config, &input, &self.track_config, dt);
                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
                        event,
                    );
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
    }

    /// Replay mode: Send telemetry from recorded data (view-only)
    fn tick_replay(&mut self) {
        // Replay mode is not yet implemented
        // This would play back previously recorded telemetry data
        // For now, do nothing
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
                // Update 3D physics with track context
                physics::update_car_3d(state, config, &input, &self.track_config, dt);

                let is_ai = ai_ids.contains(&state.player_id);
                sample_ghost_trace(
                    &mut self.lap_traces,
                    state,
                    is_ai,
                    current_tick,
                    self.tick_rate_hz,
                );

                // Update track progress; a timing line crossed here becomes
                // a split on every driver's screen.
                if let Some(event) = physics::update_track_progress_3d(
                    state,
                    &self.track_config,
                    current_tick,
                    self.tick_rate_hz,
                ) {
                    note_lap_event(
                        &mut self.lap_events,
                        &mut self.lap_traces,
                        &mut self.session_best_lap_ms,
                        &mut self.session_best_splits_ms,
                        state.player_id,
                        is_ai,
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

        // A timed race whose clock just ran out learns its last lap; then
        // assign finish positions to cars that just took the flag, and
        // finish the session exactly once when every car is classified.
        self.update_race_clock();
        self.assign_finish_positions();
        if self.session.state != SessionState::Finished && self.is_race_complete() {
            self.session.state = SessionState::Finished;
        }
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

    /// Why a human may not join this session as a driver now, or `None`
    /// when they may. A running practice, hotlap or qualifying session takes
    /// drivers (a hotlap or qualifying driver arrives in the garage); a race
    /// takes them until the green light, and a finished session none. A
    /// spectator (`JoinAsSpectator`) is never refused for this.
    pub fn refuses_drivers(&self) -> Option<&'static str> {
        if self.session.state == SessionState::Finished {
            return Some("Session has finished");
        }
        if self.session.game_mode == GameMode::Race {
            return Some("A race is under way: watch it instead");
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
            GameMode::Race => {
                // The race starts now: mark the session racing and remember
                // the start tick for race-time bookkeeping.
                self.session.state = SessionState::Racing;
                self.session.race_start_tick = Some(self.session.current_tick);
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
                self.race_end = RaceEnd::default();
                // Pole sits on the line, so it never crosses it to start lap
                // 1: its lap starts with the green light.
                let tick = self.session.current_tick;
                for state in self.session.participants.values_mut() {
                    physics::start_lap_on_green(state, &self.track_config, tick);
                    // The grid waits in neutral; the automatic box takes
                    // first with the green light, never before.
                    if state.auto_gearbox && state.gear == 0 {
                        state.gear = 1;
                        state.auto_shift_hold_ticks = 0;
                    }
                }
            }
            GameMode::Qualification | GameMode::Hotlap => {
                // Every driver starts in the garage, with the setup screen;
                // the AI (if any) is left where it stands and drives on.
                self.session.state = SessionState::Racing;
                self.session.demo_lap_progress = None;
                self.finish_deadline_tick = None;
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

    /// Give the watcher's HUD the track's corners.
    pub fn set_track_corners(&mut self, corners: Vec<crate::network::CornerData>) {
        self.track_corners = corners;
    }

    /// The `TrackCorners` message for a joining watcher; `None` when the
    /// track has no corners.
    pub fn track_corners_message(&self, session_id: SessionId) -> Option<ServerMessage> {
        if self.track_corners.is_empty() {
            return None;
        }
        Some(ServerMessage::TrackCorners(
            crate::network::TrackCornersData {
                session_id,
                track_length_m: self.track_length_m(),
                corners: self.track_corners.clone(),
            },
        ))
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
        let ids: Vec<PlayerId> = self.session.ai_player_ids.clone();
        let stuck_limit = (HOTLAP_WATCH_STUCK_S * self.tick_rate_hz as f32) as u32;
        let held = self.session.current_tick < self.hotlap_watch.hold_until_tick;
        let mut crashed = false;
        let mut struck = false;
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
        }
        if crashed {
            self.send_hotlap_watch_out();
        }
    }

    /// One step down the ladder for the watched driver.
    fn ease_hotlap_watch_driver(&mut self) {
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
        if !matches!(mode, GameMode::Hotlap | GameMode::Qualification) {
            return Err("Not a hotlap or qualifying session");
        }
        let Some(state) = self.session.participants.get(player_id) else {
            return Err("No car in this session");
        };
        let pose = match destination {
            HotlapDestination::Garage => {
                let slot = self
                    .track_config
                    .start_positions
                    .iter()
                    .find(|s| s.position == state.grid_position)
                    .or_else(|| self.track_config.start_positions.first())
                    .ok_or("Track has no grid")?;
                (slot.x, slot.y, slot.z, slot.yaw_rad)
            }
            HotlapDestination::Track if mode == GameMode::Qualification => self
                .qualifying_outlap_pose(player_id)
                .ok_or("Track has no centerline")?,
            HotlapDestination::Track => self
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
        fresh.outlap = !in_garage && mode == GameMode::Qualification;
        fresh.fuel_liters = fuel;
        fit_tyres(
            &mut fresh,
            &self.car_configs,
            &self.tuned_configs,
            &self.track_config,
            // The outlap is a qualifying car's warm-up: it leaves cold.
            if cold_tyres || mode == GameMode::Qualification {
                TyreStart::Garage
            } else {
                TyreStart::Warm
            },
            &self.car_setups,
        );
        // Parked cars wait in neutral; a car put on the run-up is in first.
        fresh.gear = if in_garage { 0 } else { 1 };
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
        use crate::recovery::{RecoverDestination, Recovery};
        let mode = self.session.game_mode;
        let garage_mode = matches!(mode, GameMode::Hotlap | GameMode::Qualification);
        match mode {
            GameMode::FreePractice | GameMode::Hotlap | GameMode::Qualification => {}
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
                let station = crate::recovery::track_station(
                    &track.centerline,
                    state.track_progress,
                    &others,
                );
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
            if recovery.destination == RecoverDestination::Pits {
                // The crew's car now: serviced at the box and driven out.
                state.pit.driving = true;
                state.pit.restore_aids = Some([state.auto_gearbox, state.steering_assist]);
                state.auto_gearbox = true;
                state.steering_assist = false;
            }
        }
    }

    /// Where a qualifying car goes out: the pit exit, the lane's last
    /// station on the centerline when that is past the line, else just past
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
            // A race starts from the grid: after a finished race (or practice)
            // the cars are wherever they stopped, with their laps and finish
            // positions still set.
            self.line_up_on_grid();
        }
        self.session.game_mode = GameMode::Countdown;
        self.session.state = SessionState::Countdown;
        self.session.countdown_ticks_remaining = Some(
            (self.tick_rate_hz as u32 * countdown_seconds.min(MAX_COUNTDOWN_SECONDS) as u32)
                .min(u16::MAX as u32) as u16,
        );
        self.session.next_mode = Some(next_mode);
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
        if self.session.participants.len() >= self.session.max_players as usize {
            return None;
        }

        // Find available grid position
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
                GameMode::Lobby | GameMode::Countdown
            );
        let base = self.car_configs.get(&car_id)?;
        if applied.changes_car() {
            self.tuned_configs.insert(*player_id, applied.apply(base));
        } else {
            self.tuned_configs.remove(player_id);
        }
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
            GameMode::Hotlap | GameMode::Qualification => HOTLAP_FUEL_LAPS,
            _ => capacity / lap,
        };
        ((laps + knob).max(MIN_FUEL_LAPS) * lap).min(capacity)
    }

    /// The run a car put on the grid now is being fuelled for: the mode
    /// being counted into, or a race from the lobby.
    fn planned_mode(&self) -> GameMode {
        match self.session.game_mode {
            GameMode::Lobby => GameMode::Race,
            GameMode::Countdown => self.session.next_mode.unwrap_or(GameMode::Race),
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
        self.tuned_configs.remove(player_id);
        self.car_setups.remove(player_id);
        self.lap_traces.remove(player_id);
        self.liveries.remove(player_id);
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

    /// Whom the next race grids by, first to last: the host's own order
    /// when there is one, else this session's qualifying result (drivers
    /// with no time behind the classified), else nobody (the seating order
    /// stands).
    fn grid_order_ids(&self) -> Option<Vec<PlayerId>> {
        let drivers = self.seated_drivers();
        if !self.session.grid_order.is_empty() {
            return Some(crate::grid_order::resolve(
                &drivers,
                self.session.host_player_id,
                &self.session.ai_player_ids,
                &self.session.grid_order,
            ));
        }
        if self.qualifying.is_empty() {
            return None;
        }
        let mut by_slot: Vec<&crate::grid_order::Seated> = drivers.iter().collect();
        by_slot.sort_by_key(|d| (d.grid_position, d.id));
        let classified: Vec<PlayerId> = self
            .qualifying_order()
            .into_iter()
            .filter(|id| drivers.iter().any(|d| d.id == *id))
            .collect();
        Some(crate::grid_order::complete(&by_slot, classified))
    }

    /// Seat the drivers on the grid slots in start order. A session with no
    /// requested order and no qualifying keeps the slots it has.
    pub fn apply_grid_order(&mut self) {
        let Some(order) = self.grid_order_ids() else {
            return;
        };
        if order.len() > self.track_config.start_positions.len() {
            return;
        }
        for (i, id) in order.iter().enumerate() {
            if let Some(state) = self.session.participants.get_mut(id) {
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
    ) -> Option<crate::records::QualifyingResult> {
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
        Some(self.qualifying_result())
    }

    /// The drivers with a legal qualifying lap, fastest first.
    pub fn qualifying_order(&self) -> Vec<PlayerId> {
        let mut classified: Vec<(&PlayerId, &(u32, u32))> = self.qualifying.iter().collect();
        classified.sort_by_key(|(id, (ms, tick))| (*ms, *tick, **id));
        classified.into_iter().map(|(id, _)| *id).collect()
    }

    /// This session's classification as the store keeps it.
    pub fn qualifying_result(&self) -> crate::records::QualifyingResult {
        let class = self
            .session
            .host_car_id
            .and_then(|id| self.car_configs.get(&id))
            .map(|c| c.class.clone())
            .unwrap_or_default();
        let entries = self
            .qualifying_order()
            .into_iter()
            .filter_map(|id| {
                let state = self.session.participants.get(&id)?;
                Some(crate::records::QualifyingEntry {
                    name: self.driver_name(&id),
                    car_config_id: state.car_config_id,
                    lap_time_ms: self.qualifying.get(&id)?.0,
                    is_ai: self.session.ai_player_ids.contains(&id),
                })
            })
            .filter(|e| !e.name.is_empty())
            .collect();
        crate::records::QualifyingResult {
            id: self.session.id,
            track_id: self.session.track_config_id,
            class,
            recorded_at: String::new(),
            entries,
        }
    }

    /// Put every car back on its grid slot with a clean race state (laps,
    /// times, finish position, damage), keeping the driver's aids.
    pub fn line_up_on_grid(&mut self) {
        self.apply_grid_order();
        self.finish_deadline_tick = None;
        self.race_end = RaceEnd::default();
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

    /// Fire the debug events (`crate::debug_hooks`) whose time of green has
    /// come. Called every racing tick.
    fn fire_debug_events(&mut self) {
        if self.debug.stand_in_driver {
            // The AI steers the wheel itself: an aid between it and the car
            // would reshape its every input.
            let ai = &self.ai_profiles;
            for state in self.session.participants.values_mut() {
                if !ai.contains_key(&state.player_id) {
                    state.steering_assist = false;
                }
            }
        }
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
                Some(crate::racecraft::Rival::of(
                    state,
                    config.length_m,
                    config.width_m,
                    total,
                ))
            })
            .collect();
        let decided: Vec<(PlayerId, crate::racecraft::Racecraft)> = field
            .iter()
            .filter_map(|me| {
                let profile = self.ai_profiles.get(&me.id)?;
                let state = self.session.participants.get(&me.id)?;
                let speeds = self.ai_speed_profile(state.car_config_id, state.fuel_liters);
                Some((
                    me.id,
                    crate::racecraft::decide(state, me, profile, speeds, &field, &ctx),
                ))
            })
            .collect();
        for (id, racecraft) in decided {
            if let Some(state) = self.session.participants.get_mut(&id) {
                state.racecraft = racecraft;
            }
        }
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
        let session_s = self.session.current_tick as f32 / rate as f32;
        let (water, rubber) = match self.track_config.road_state.as_ref() {
            Some(road) => (road.mean_water(), road.mean_line_rubber()),
            None => (self.track_config.track_surface.water, self.sky.line_rubber),
        };
        self.sky.line_rubber = rubber;
        self.sky.step(session_s, 1.0, water);
        if self.sky.is_static() {
            return;
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
                continue;
            }
            let cell = (state.track_progress.max(0.0) / crate::road_state::CELL_M) as u32;
            if cell != state.road_cell {
                state.road_cell = cell;
                // The wheels' paths either side of the car's middle.
                let half = self.car_configs.get(&state.car_config_id).map_or(0.8, |c| {
                    0.25 * (c.track_width_front_m + c.track_width_rear_m)
                });
                let lateral = road.lateral_of(state.track_progress, state.pos_x, state.pos_y);
                road.note_pass(
                    state.track_progress,
                    &[lateral - half, lateral + half],
                    crate::road_state::shed_for_lateral_g(state.g_forces.lateral_g),
                );
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
                    let leak = slow_leak(seed);
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
                        let leak = slow_leak(seed);
                        tyre_thermal::puncture(state.tires.each_mut()[i], leak);
                    }
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
        let before_the_start = self.session.game_mode == GameMode::Countdown
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
        let centerline = &self.track_config.centerline;
        let mut arrived: Vec<PlayerId> = Vec::new();
        let mut finished: Vec<PlayerId> = Vec::new();
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
            pit.lane_node = Some(at.node as u32);
            pit.lane_station_m = at.station_m;
            pit.in_lane = in_lane;
            pit.limiter =
                pit.in_lane && (lane.limit_start_m..=lane.limit_end_m).contains(&at.station_m);
            pit.exit_closed = exit_closed;
            if !pit.in_lane && !pit.driving {
                // Out of the lane: the next visit is a new stop.
                pit.serviced = false;
            }
            // A human's car that drives into the lane is the autopilot's
            // until the lane's end: to its own box, serviced, and out.
            if !is_ai
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
            // The route ends past the lane's last line.
            if pit.driving && pit.serviced && at.station_m >= lane.length_m - 3.0 {
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
            // run-up included (`stand_in_input`).
            .filter(|s| {
                s.pit.driving
                    && !s.in_garage
                    && !self.is_ai_player(&s.player_id)
                    && !self.debug.stand_in_driver
            })
            .filter_map(|state| {
                let config = self.car_configs.get(&state.car_config_id)?;
                let mut input = crate::pit::drive_input(
                    lane,
                    lane.box_at(state.pit.box_index.unwrap_or(0)),
                    state,
                    config,
                    &self.lane_traffic(lane, state),
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
    pub fn take_pit_events(&mut self) -> Vec<(PlayerId, crate::network::PitServiceData)> {
        std::mem::take(&mut self.pit_events)
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
    /// session is a timed race counting into or running its race.
    pub fn race_clock(&self) -> Option<RaceClock> {
        let length = self.race_length_ticks()?;
        let elapsed = match self.session.game_mode {
            GameMode::Race => self.race_elapsed_ticks(),
            GameMode::Countdown if self.session.next_mode == Some(GameMode::Race) => 0,
            _ => return None,
        };
        let left_ticks = length.saturating_sub(elapsed);
        Some(RaceClock {
            left_ms: (left_ticks * 1000 / self.tick_rate_hz.max(1) as u64).min(u32::MAX as u64)
                as u32,
            final_lap: self.race_end.final_lap.unwrap_or(0),
        })
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
    /// Repairs when anything is damaged. The plan goes to every client
    /// (`ServerMessage::PitService`) for the pit-stop panel.
    fn start_service(&mut self, player_id: &PlayerId) {
        let Some(state) = self.session.participants.get(player_id) else {
            return;
        };
        let car_id = state.car_config_id;
        let is_ai = self.is_ai_player(player_id);
        let compound = if is_ai {
            state.pit.next_compound
        } else {
            let tyre = self
                .simulated_config_for(player_id)
                .map(|c| c.tire_config.clone())
                .unwrap_or_default();
            self.car_setup(player_id)
                .compound_index_for(&tyre, self.track_config.track_surface.water)
        };
        let laps_left = self.laps_left(state) as f32 + 1.0;
        let fuel_now = state.fuel_liters;
        let damage = crate::pit::damage_percent(state);
        let brake_wear = crate::pit::brake_wear_percent(state);
        let pit_box = state.pit.box_index.unwrap_or(0);
        let mode = self.session.game_mode;
        let Some(config) = self.car_configs.get(&car_id).cloned() else {
            return;
        };
        let fuel_target = if !crate::pit::refuelling_allowed(&config) {
            fuel_now
        } else if mode == GameMode::Race {
            match self.lap_fuel_liters(car_id) {
                Some(lap) => (laps_left * (1.0 + RACE_FUEL_MARGIN) * lap + 0.5 * lap)
                    .min(config.fuel.capacity_liters),
                None => config.fuel.capacity_liters,
            }
        } else {
            self.start_fuel_liters(player_id, car_id, mode)
        };
        let plan = crate::pit::plan_service(&config, fuel_target - fuel_now, damage, brake_wear);
        if let Some(state) = self.session.participants.get_mut(player_id) {
            let pit = &mut state.pit;
            pit.servicing = true;
            pit.service_left_s = plan.total_s();
            pit.service_compound = compound;
            pit.service_fuel_l = plan.fuel_l;
            pit.service_repair = plan.repair_pct > 0.0;
            pit.service_pads = plan.pads;
        }
        self.pit_events.push((
            *player_id,
            crate::network::PitServiceData {
                car_index: 0,
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
        let Some(state) = self.session.participants.get_mut(player_id) else {
            return;
        };
        let Some(config) = simulated_config(&self.car_configs, &self.tuned_configs, state).cloned()
        else {
            return;
        };
        let tyre = &config.tire_config;
        let temperature = tyre_thermal::start_temperature_c(tyre, &self.track_config.track_surface);
        let compound = state.pit.service_compound;
        let water = self.track_config.track_surface.water;
        tyre_thermal::fit(state, tyre, temperature, compound, water);
        state.fuel_liters =
            (state.fuel_liters + state.pit.service_fuel_l).min(config.fuel.capacity_liters);
        if state.pit.service_repair {
            // The hits, the heat and the missed shifts: not the wear.
            state.damage = state.damage.repaired();
        }
        if state.pit.service_pads {
            crate::brakes::fit_pads(state);
        }
        // A new stint's energy (`crate::hybrid`).
        crate::hybrid::new_stint(state, &config);
        let pit = &mut state.pit;
        pit.service_pads = false;
        pit.servicing = false;
        pit.service_left_s = 0.0;
        pit.service_repair = false;
        pit.serviced = true;
        pit.wants_stop = false;
        pit.stops += 1;
    }

    /// In a race, each AI driver's decision to stop (`pit::plan_stop`), and
    /// its turn onto the pit route once the lane's entry is near: the
    /// automatic gearbox on for the route, `pit::drive_input` driving.
    fn plan_ai_stops(&mut self) {
        if self.session.game_mode != GameMode::Race {
            return;
        }
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
            if !state.pit.wants_stop {
                let laps_left = self.laps_left(state);
                let car_id = state.car_config_id;
                // Short of fuel is no reason to stop in a car the rules do
                // not refuel (an F1): the stop would add none, and the car
                // would come in again every lap.
                let refuels = self
                    .car_configs
                    .get(&car_id)
                    .is_some_and(crate::pit::refuelling_allowed);
                let lap_fuel = self.lap_fuel_liters(car_id).filter(|_| refuels);
                let Some(state) = self.session.participants.get(&ai_id) else {
                    continue;
                };
                // The weather first: rain on slicks, a dry line on treaded
                // tyres (`pit::tyres_for_the_track`).
                let line_water = self
                    .track_config
                    .road_state
                    .as_ref()
                    .map_or(self.track_config.track_surface.water, |r| {
                        r.mean_line_water()
                    });
                let for_the_track = self.car_configs.get(&car_id).and_then(|c| {
                    crate::pit::tyres_for_the_track(&c.tire_config, state.tyre_compound, line_water)
                });
                if let Some(compound) = for_the_track.filter(|_| laps_left > 0) {
                    if let Some(state) = self.session.participants.get_mut(&ai_id) {
                        state.pit.wants_stop = true;
                        state.pit.next_compound = compound;
                    }
                    continue;
                }
                if let Some(pick) = crate::pit::plan_stop(state, laps_left, lap_fuel) {
                    // In the rain the tyre is the weather's; either way one
                    // of the car's own.
                    let tyre = self
                        .car_configs
                        .get(&car_id)
                        .map(|c| c.tire_config.clone())
                        .unwrap_or_default();
                    let compound = tyre
                        .weather_compound(line_water)
                        .unwrap_or_else(|| tyre.compound_for_click(pick.click()));
                    if let Some(state) = self.session.participants.get_mut(&ai_id) {
                        state.pit.wants_stop = true;
                        state.pit.next_compound = compound;
                    }
                }
                continue;
            }
            let to_entry = (entry - state.track_progress).rem_euclid(total.max(1.0));
            if to_entry <= approach {
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
        let surface = &mut self.track_config.track_surface;
        let seconds = self.session.current_tick as f32 / self.tick_rate_hz.max(1) as f32;
        surface.wind_now_mps = crate::wind::gusting(surface.wind_mps, seconds);
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
                        );
                    }
                }
            }
        }
        let controller = AiDriverController::new(profile, &self.track_config, car_config)
            .with_speed_profile(self.ai_speed_profile(state.car_config_id, state.fuel_liters))
            .with_fuel_save(self.fuel_save_coast_s(state, car_config));
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
        // The hybrid: balanced, and the overtake button when chasing.
        if car_config.hybrid.enabled {
            input.ers_mode = Some(crate::hybrid::DEFAULT_MODE);
            input.ers_boost = self.ai_wants_boost(state);
        }
        if let Some((lane, to_lane)) = run_up {
            input = crate::pit::run_up_input(lane, to_lane, state.speed_mps, input);
        }
        input
    }

    /// How long an AI in a race coasts before each braking zone to reach the
    /// flag on what it carries: nothing while the tank will do, or when the
    /// car will be refuelled at a stop (the pit plan's business); past that,
    /// [`FUEL_SAVE_COAST_S_PER_SHORT`] per share it is short, capped by the
    /// driver ([`crate::ai_driver::MAX_FUEL_SAVE_COAST_S`]).
    fn fuel_save_coast_s(&self, state: &CarState, config: &CarConfig) -> f32 {
        if self.session.game_mode != GameMode::Race
            || self.session.state != SessionState::Racing
            || state.finish_position.is_some()
        {
            return 0.0;
        }
        if crate::pit::refuelling_allowed(config) && self.track_config.pit_lane.is_some() {
            return 0.0;
        }
        let Some(&lap) = self.lap_fuel.get(&state.car_config_id) else {
            return 0.0;
        };
        let total = crate::laps::track_length_m(&self.track_config).max(1.0);
        let this_lap = 1.0 - (state.track_progress / total).clamp(0.0, 1.0);
        let needed = lap * (self.laps_left(state) as f32 + this_lap);
        if needed <= 0.0 || state.fuel_liters >= needed {
            return 0.0;
        }
        (1.0 - state.fuel_liters / needed) * FUEL_SAVE_COAST_S_PER_SHORT
    }

    /// An AI in a race presses the overtake button within
    /// [`crate::hybrid::AI_BOOST_GAP_S`] of the car ahead, from the second
    /// lap, on the throttle.
    fn ai_wants_boost(&self, state: &CarState) -> bool {
        if !crate::drs::race_rules(self.session.game_mode)
            || state.current_lap < crate::drs::DRS_RACE_FROM_LAP
            || state.pit.driving
        {
            return false;
        }
        let total = crate::laps::track_length_m(&self.track_config);
        if total <= 0.0 {
            return false;
        }
        let field: Vec<(PlayerId, f32)> = self
            .session
            .participants
            .values()
            .filter(|s| !s.in_garage && s.damage.is_drivable)
            .map(|s| (s.player_id, s.track_progress))
            .collect();
        crate::drs::gap_ahead_s(state, &field, total)
            .is_some_and(|g| g <= crate::hybrid::AI_BOOST_GAP_S)
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

    /// Get telemetry for broadcast
    pub fn get_telemetry(&self) -> ServerMessage {
        let car_states: Vec<CarStateTelemetry> = self
            .session
            .participants
            .values()
            .map(CarStateTelemetry::from)
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz as f32) * 1000.0) as u16);

        let telemetry = crate::network::Telemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
        };

        ServerMessage::Telemetry(telemetry)
    }

    /// Compact wire telemetry (protocol v2): cars are identified by their
    /// position in the deterministic `participants` iteration order, matching
    /// the indices announced in the most recent `SessionRoster`.
    pub fn get_compact_telemetry(&self) -> CompactTelemetry {
        let car_states: Vec<CompactCarState> = self
            .session
            .participants
            .values()
            .enumerate()
            .map(|(idx, s)| CompactCarState::from_car_state(s, idx as u8))
            .collect();

        let countdown_ms = self
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / self.tick_rate_hz as f32) * 1000.0) as u16);

        CompactTelemetry {
            server_tick: self.session.current_tick,
            session_state: self.session.state,
            game_mode: self.session.game_mode,
            countdown_ms,
            car_states,
            race_clock: self.race_clock(),
            sky: Some(crate::network::SkyNow::of(
                &self.sky,
                self.track_config.track_surface.wind_now_mps,
                self.session.current_tick as f32 / self.tick_rate_hz.max(1) as f32,
            )),
        }
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

    /// Build the car-index → player mapping for compact telemetry. Human
    /// player names are looked up in `names` (lobby data); AI names come from
    /// their profiles.
    pub fn build_roster(&self, names: &HashMap<PlayerId, String>) -> SessionRosterData {
        // AI drivers are dealt liveries in grid order, each model's cars
        // taking the next one of its list, so a field of the same car is not
        // a row of clones.
        let mut dealt: HashMap<CarConfigId, u8> = HashMap::new();
        let entries = self
            .session
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
                RosterEntry {
                    car_index: idx as u8,
                    player_id: *player_id,
                    player_name,
                    is_ai,
                    car_config_id: state.car_config_id,
                    livery,
                }
            })
            .collect();

        SessionRosterData {
            session_id: self.session.id,
            entries,
        }
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
    /// car on the lead lap that crosses after it.
    fn assign_finish_positions(&mut self) {
        let Some(finish_lap) = self.finish_lap() else {
            return;
        };
        let tick = self.session.current_tick;
        let winner_was_in = !self.race_end.finishers.is_empty();
        let flag_laps = &self.race_end.flag_laps;
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
                let finisher = Finisher {
                    laps: s.current_lap.saturating_sub(1),
                    tick,
                    progress: s.track_progress,
                };
                (*id, finisher)
            })
            .collect();

        if new_finishers.is_empty() {
            return;
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
        // Stable: exact ties keep BTreeMap order.
        order.sort_by(|(_, a), (_, b)| {
            b.laps.cmp(&a.laps).then(a.tick.cmp(&b.tick)).then(
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
            self.ai_speed_profiles.insert(car_id, plans);
        }
    }

    /// The AI's speed profile for `car_id` carrying `fuel_liters`: the one
    /// planned at the next step of the tank at or above it.
    fn ai_speed_profile(
        &self,
        car_id: CarConfigId,
        fuel_liters: f32,
    ) -> Option<&RacingLineProfile> {
        let plans = self.ai_speed_profiles.get(&car_id)?;
        let capacity = self
            .car_configs
            .get(&car_id)
            .map_or(0.0, |c| c.fuel.capacity_liters);
        let step = if capacity > 0.0 {
            (fuel_liters / capacity * AI_FUEL_PLAN_STEPS as f32).ceil() as usize
        } else {
            0
        };
        plans.get(step.min(plans.len().saturating_sub(1)))
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
        let field = class_field(&self.car_configs, anchor);
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

        let roster = game_session.build_roster(&HashMap::new());
        let livery_of = |id: PlayerId| {
            roster
                .entries
                .iter()
                .find(|e| e.player_id == id)
                .unwrap()
                .livery
        };
        assert_eq!(livery_of(picked), 2, "a pick inside the car's list is kept");
        assert_eq!(
            livery_of(stale),
            0,
            "a pick past the end falls back to the car as authored"
        );
        assert_eq!(livery_of(stock), 0);
        let mut ai: Vec<u8> = roster
            .entries
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
        let roster = game_session.build_roster(&HashMap::new());
        assert_eq!(
            roster
                .entries
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

    /// Two humans racing on the default oval, lap limit 3.
    fn racing_session_with_two_humans() -> (GameSession, PlayerId, PlayerId) {
        let mut game_session = create_test_session();
        let car_id = game_session.car_configs.values().next().unwrap().id;
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        game_session.add_player(a, car_id).unwrap();
        game_session.add_player(b, car_id).unwrap();
        game_session.set_game_mode(GameMode::Race);
        (game_session, a, b)
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

        let roster = game_session.build_roster(&HashMap::new());
        for entry in &roster.entries {
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
    fn a_driver_who_is_not_there_closes_the_grid_up() {
        let (mut game_session, [a, b, c]) = three_drivers();
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
