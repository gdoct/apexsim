//! The track guide: a corner-by-corner walk round a circuit, made offline
//! and played on the client from two files (docs/TRACK_GUIDE.md).
//!
//! Three lone AI cars of a class lap the empty track ([`solo_run`]); each
//! one's first flying lap is kept and the three are merged into one
//! spectator stream, the second and third car following the first by a
//! fixed gap, so the loop at a corner shows three clean passes and no car
//! is ever in another's wake. The corners are found from the centerline's
//! curvature ([`detect_corners`]), named from the dossier
//! (`<Stem>.layout.json`'s `display_name`, never the real name), measured
//! from the first car's run (minimum speed, gear, where it braked), and
//! described: what the geometry says about each ([`gotchas`]) and what a
//! person wrote about it (`<Stem>.guide.yml`, [`GuideNotes`]).
//!
//! Every position in the guide is the server frame, as `-ApexCamera=`
//! takes it; every time is seconds from the stream's first frame.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::ai_driver::AiDriverProfile;
use crate::data::*;
use crate::game_session::GameSession;
use crate::replay_tools::{
    load_car_folder, pose_at, pose_on_ground, track_stem, CarMap, FolderMap,
};
use crate::spectator::{
    encode_frames, BroadcastEncoder, CarRow, StreamContent, StreamRender, StreamRoster,
    StreamRosterEntry,
};
use crate::track_loader::TrackLoader;

/// The format of `guide.json`.
pub const GUIDE_VERSION: u32 = 1;

/// What a guide is made with (`apexsim-replay guide`).
#[derive(Debug, Clone)]
pub struct GuideOptions {
    pub track_path: PathBuf,
    /// The `content/cars` folder (`content/cars/default` keeps the player's
    /// own cars out).
    pub cars_dir: PathBuf,
    /// car.toml `class`: F1, GT3, Hypercar, LMP2.
    pub class: String,
    /// The first seed tried; a run that leaves the track or strikes its lap
    /// is tried again with the next.
    pub seed: u64,
    /// Seeds tried per car before the least bad run is kept.
    pub tries: u32,
    pub skill: u8,
    /// How far each following car runs behind the one before it.
    pub car_gap_s: f32,
    pub cars: u8,
    pub tick_rate: u16,
    pub record_hz: u16,
}

impl Default for GuideOptions {
    fn default() -> Self {
        Self {
            track_path: PathBuf::from("content/tracks/default/Zandvoort.yaml"),
            cars_dir: PathBuf::from("content/cars/default"),
            class: "GT3".to_string(),
            seed: 1,
            tries: 6,
            skill: 110,
            car_gap_s: 2.0,
            cars: 3,
            tick_rate: 240,
            record_hz: 60,
        }
    }
}

// --- The guide file -----------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TrackGuide {
    pub version: u32,
    pub class: String,
    pub display_class: String,
    /// The stream's file name, beside this file.
    pub recording: String,
    pub source_crc: u32,
    pub car: GuideCar,
    pub car_gap_s: f32,
    pub track: GuideTrack,
    pub overview: GuideOverview,
    pub corners: Vec<GuideCorner>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideCar {
    pub id: String,
    pub folder: String,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideTrack {
    pub stem: String,
    pub track_id: String,
    pub display_name: String,
    pub description: Option<String>,
    pub country: Option<String>,
    pub city: Option<String>,
    pub category: Option<String>,
    pub year_built: Option<u32>,
    pub length_m: f32,
    pub altitude_m: Option<f32>,
    pub latitude_deg: Option<f32>,
    pub longitude_deg: Option<f32>,
    /// Lowest and highest point of the lap, relative to the start line.
    pub elevation_min_m: f32,
    pub elevation_max_m: f32,
    /// Total ascent over a lap.
    pub climb_m: f32,
    pub corners: u32,
    pub left: u32,
    pub right: u32,
    /// `clockwise` or `anticlockwise`, seen from above.
    pub direction: String,
    pub longest_straight_m: f32,
    pub drs_zones: u32,
    /// The guide car's flying lap.
    pub lap_time_s: f32,
    pub top_speed_kph: f32,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideOverview {
    /// The frozen moment the guide opens on: the first car short of the line.
    pub time_s: f32,
    pub cameras: Vec<GuideCamera>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideCamera {
    pub name: String,
    /// `fixed`: a tripod at `eye` looking at `look`.
    pub kind: String,
    pub eye: [f32; 3],
    pub look: [f32; 3],
    pub fov_deg: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideClip {
    pub from_s: f32,
    pub to_s: f32,
    pub slow_from_s: f32,
    pub slow_to_s: f32,
    pub slow_rate: f32,
    pub apex_s: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GuideCorner {
    pub number: u32,
    /// The turns it covers, counted one per change of hand round the lap
    /// (a chicane is two): `turn_from == turn_to` for a single corner.
    pub turn_from: u32,
    pub turn_to: u32,
    pub name: String,
    pub direction: String,
    pub entry_m: f32,
    pub apex_m: f32,
    pub exit_m: f32,
    pub turn_deg: f32,
    pub min_radius_m: f32,
    pub clip: GuideClip,
    pub min_speed_kph: f32,
    pub apex_gear: i8,
    pub entry_speed_kph: f32,
    pub exit_speed_kph: f32,
    /// Metres before the apex the guide car started braking; `None` when it
    /// did not brake.
    pub brake_m: Option<f32>,
    pub flat_out: bool,
    pub elevation_change_m: f32,
    /// Signed: positive leans into the corner.
    pub banking_deg: f32,
    pub gotchas: Vec<String>,
    pub notes: Vec<String>,
    pub cameras: Vec<GuideCamera>,
}

// --- Hand-written notes (`<Stem>.guide.yml`) ------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct GuideNotes {
    #[serde(default)]
    pub overview: Vec<String>,
    #[serde(default)]
    pub corners: Vec<CornerNote>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CornerNote {
    /// A station inside the corner: the detected corner nearest it (by its
    /// span, within [`NOTE_REACH_M`]) takes the note.
    pub at_m: f32,
    /// Overrides the display name. A sound-alike, never the real name.
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub notes: Vec<String>,
}

/// Gotchas and notes a corner card shows at most.
const MAX_CARD_LINES: usize = 6;

/// How far from a corner's span a note's `at_m` may be and still find it.
pub const NOTE_REACH_M: f32 = 150.0;

pub fn notes_path(track_path: &Path) -> PathBuf {
    track_path.with_extension("guide.yml")
}

pub fn load_notes(track_path: &Path) -> Result<GuideNotes, String> {
    let path = notes_path(track_path);
    if !path.is_file() {
        return Ok(GuideNotes::default());
    }
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    serde_yaml::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))
}

// --- Corners ------------------------------------------------------------------

/// Sample spacing of the curvature sweep.
const STEP_M: f32 = 2.0;
/// Curvature is a heading change over the box this wide: what a GPS
/// trace's wobble cannot fake and a chicane's flick does not hide in.
const SMOOTH_M: f32 = 24.0;
/// A corner is where the road turns tighter than this radius...
const CORNER_KAPPA: f32 = 1.0 / 450.0;
/// ...for at least this far...
const MIN_RUN_M: f32 = 30.0;
/// ...and through at least this much heading.
const MIN_TURN_RAD: f32 = 0.26;
/// Two runs of the same hand this close are one corner (a double apex).
const SAME_HAND_GAP_M: f32 = 30.0;
/// Runs this close, whatever their hand, are one stop of the guide (a
/// chicane, an esses), as long as the stop stays under [`MAX_STOP_SPAN_M`].
const STOP_GAP_M: f32 = 30.0;
const MAX_STOP_SPAN_M: f32 = 300.0;

/// One run of the road turning one way.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TurnRun {
    /// Stations, unrolled: `end > start`, both may lie past the lap.
    pub start_m: f32,
    pub end_m: f32,
    /// +1 left, -1 right.
    pub sign: f32,
    /// Signed heading change, radians.
    pub turn_rad: f32,
    pub peak_kappa: f32,
    pub peak_m: f32,
}

/// A stop of the guide: one corner, or a chicane of several runs.
#[derive(Debug, Clone, PartialEq)]
pub struct Corner {
    pub runs: Vec<TurnRun>,
    /// Stations, unrolled so `entry <= apex <= exit`; the apex lies in
    /// `[0, lap)`, the entry may be negative and the exit past the lap.
    pub entry_m: f32,
    pub apex_m: f32,
    pub exit_m: f32,
}

impl Corner {
    pub fn direction(&self) -> String {
        let mut hands: Vec<&str> = Vec::new();
        for run in &self.runs {
            let hand = if run.sign > 0.0 { "left" } else { "right" };
            if hands.last() != Some(&hand) {
                hands.push(hand);
            }
        }
        hands.join("-")
    }

    /// Turns as a driver counts them: one per change of hand.
    pub fn turn_count(&self) -> u32 {
        1 + self
            .runs
            .windows(2)
            .filter(|w| w[0].sign != w[1].sign)
            .count() as u32
    }

    pub fn turn_rad(&self) -> f32 {
        self.runs.iter().map(|r| r.turn_rad.abs()).sum()
    }

    pub fn peak_kappa(&self) -> f32 {
        self.runs.iter().map(|r| r.peak_kappa).fold(0.0, f32::max)
    }

    /// +1 when the corner's main run turns left.
    pub fn main_sign(&self) -> f32 {
        self.runs
            .iter()
            .max_by(|a, b| a.turn_rad.abs().total_cmp(&b.turn_rad.abs()))
            .map_or(1.0, |r| r.sign)
    }
}

fn wrap_angle(mut a: f32) -> f32 {
    while a > std::f32::consts::PI {
        a -= std::f32::consts::TAU;
    }
    while a < -std::f32::consts::PI {
        a += std::f32::consts::TAU;
    }
    a
}

pub fn lap_length_m(track: &TrackConfig) -> f32 {
    track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0)
}

/// Signed curvature (positive turning left) every [`STEP_M`] round the lap,
/// smoothed over [`SMOOTH_M`].
pub fn curvature_profile(track: &TrackConfig) -> Vec<f32> {
    let lap = lap_length_m(track);
    let n = ((lap / STEP_M).floor() as usize).max(3);
    let yaw: Vec<f32> = (0..n)
        .map(|i| pose_at(track, i as f32 * STEP_M, 0.0, 0.0).yaw_rad)
        .collect();
    let raw: Vec<f32> = (0..n)
        .map(|i| wrap_angle(yaw[(i + 1) % n] - yaw[(i + n - 1) % n]) / (2.0 * STEP_M))
        .collect();
    let half = ((SMOOTH_M / STEP_M) / 2.0).round() as isize;
    (0..n as isize)
        .map(|i| {
            let mut sum = 0.0;
            for k in -half..=half {
                sum += raw[(i + k).rem_euclid(n as isize) as usize];
            }
            sum / (2 * half + 1) as f32
        })
        .collect()
}

/// Every run of the road turning one way, in lap order (unrolled stations,
/// starting from the straightest point of the lap).
pub fn turn_runs(track: &TrackConfig) -> Vec<TurnRun> {
    let kappa = curvature_profile(track);
    let n = kappa.len();
    // Start the sweep where the road is straightest, so no run straddles it.
    let i0 = (0..n)
        .min_by(|&a, &b| kappa[a].abs().total_cmp(&kappa[b].abs()))
        .unwrap_or(0);
    let mut runs: Vec<TurnRun> = Vec::new();
    let mut current: Option<TurnRun> = None;
    for j in 0..=n {
        let i = (i0 + j) % n;
        let s = (i0 + j) as f32 * STEP_M;
        let k = if j == n { 0.0 } else { kappa[i] };
        let sign = if k.abs() >= CORNER_KAPPA {
            k.signum()
        } else {
            0.0
        };
        if let Some(run) = current.as_mut() {
            if sign == run.sign {
                run.end_m = s;
                run.turn_rad += k * STEP_M;
                if k.abs() > run.peak_kappa {
                    run.peak_kappa = k.abs();
                    run.peak_m = s;
                }
                continue;
            }
            runs.push(*run);
            current = None;
        }
        if sign != 0.0 {
            current = Some(TurnRun {
                start_m: s,
                end_m: s,
                sign,
                turn_rad: k * STEP_M,
                peak_kappa: k.abs(),
                peak_m: s,
            });
        }
    }
    // A double apex is one corner.
    let mut merged: Vec<TurnRun> = Vec::new();
    for run in runs {
        if let Some(last) = merged.last_mut() {
            if last.sign == run.sign && run.start_m - last.end_m <= SAME_HAND_GAP_M {
                last.end_m = run.end_m;
                last.turn_rad += run.turn_rad;
                if run.peak_kappa > last.peak_kappa {
                    last.peak_kappa = run.peak_kappa;
                    last.peak_m = run.peak_m;
                }
                continue;
            }
        }
        merged.push(run);
    }
    merged
        .into_iter()
        .filter(|r| r.end_m - r.start_m >= MIN_RUN_M && r.turn_rad.abs() >= MIN_TURN_RAD)
        .collect()
}

/// The guide's stops, in lap order from the start line.
pub fn detect_corners(track: &TrackConfig) -> Vec<Corner> {
    let lap = lap_length_m(track);
    let runs = turn_runs(track);
    let mut groups: Vec<Vec<TurnRun>> = Vec::new();
    for run in runs {
        if let Some(group) = groups.last_mut() {
            let start = group[0].start_m;
            let end = group.last().map_or(start, |r| r.end_m);
            if run.start_m - end <= STOP_GAP_M
                && run.end_m - start <= MAX_STOP_SPAN_M
                && group.len() < 3
                && (group.len() < 2 || run.sign != group[group.len() - 1].sign)
            {
                group.push(run);
                continue;
            }
        }
        groups.push(vec![run]);
    }
    let mut corners: Vec<Corner> = groups
        .into_iter()
        .map(|runs| {
            let entry = runs[0].start_m;
            let exit = runs.last().map_or(entry, |r| r.end_m);
            let peak = runs
                .iter()
                .max_by(|a, b| a.peak_kappa.total_cmp(&b.peak_kappa))
                .map_or(entry, |r| r.peak_m);
            // Roll the apex into the lap and carry the span with it.
            let apex = if lap > 0.0 {
                peak.rem_euclid(lap)
            } else {
                peak
            };
            let shift = apex - peak;
            Corner {
                runs,
                entry_m: entry + shift,
                apex_m: apex,
                exit_m: exit + shift,
            }
        })
        .collect();
    corners.sort_by(|a, b| a.apex_m.total_cmp(&b.apex_m));
    corners
}

/// The dossier's corners that have a name a screen may show.
#[derive(Debug, Clone, PartialEq)]
pub struct NamedCorner {
    pub display_name: String,
    pub station_m: f32,
    pub from_m: f32,
    pub to_m: f32,
}

pub fn dossier_corners(track_path: &Path) -> Vec<NamedCorner> {
    let path = track_path.with_extension("layout.json");
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Vec::new();
    };
    let Ok(json) = serde_json::from_str::<serde_json::Value>(&text) else {
        return Vec::new();
    };
    let Some(corners) = json.get("corners").and_then(|c| c.as_array()) else {
        return Vec::new();
    };
    corners
        .iter()
        .filter_map(|c| {
            let display_name = c.get("display_name")?.as_str()?.trim().to_string();
            if display_name.is_empty() || names_a_straight(&display_name) {
                return None;
            }
            let station_m = c.get("station_m")?.as_f64()? as f32;
            let from_m = c
                .get("from_m")
                .and_then(|v| v.as_f64())
                .unwrap_or(station_m as f64) as f32;
            let to_m = c
                .get("to_m")
                .and_then(|v| v.as_f64())
                .unwrap_or(station_m as f64) as f32;
            Some(NamedCorner {
                display_name,
                station_m,
                from_m: from_m.min(to_m),
                to_m: from_m.max(to_m),
            })
        })
        .collect()
}

/// A named way that is a straight, not a corner.
fn names_a_straight(name: &str) -> bool {
    let low = name.to_lowercase();
    [
        "straight",
        "gerade",
        "ligne droite",
        "reta ",
        "recta",
        "hangar",
    ]
    .iter()
    .any(|w| low.contains(w))
}

/// Distance from `s` to the span `[from, to]` round a lap of `lap` metres.
fn distance_to_span(s: f32, from: f32, to: f32, lap: f32) -> f32 {
    let mut best = f32::MAX;
    for k in [-1.0, 0.0, 1.0] {
        let x = s + k * lap;
        let d = if x < from {
            from - x
        } else if x > to {
            x - to
        } else {
            0.0
        };
        best = best.min(d);
    }
    best
}

/// A name for each corner from the dossier: the named way that overlaps it
/// most (its middle nearest the apex on a tie), each name used once.
pub fn name_corners(corners: &[Corner], named: &[NamedCorner], lap: f32) -> Vec<Option<String>> {
    // Every (corner, name) pair that is close enough, best first.
    let mut pairs: Vec<(f32, usize, usize)> = Vec::new();
    for (ci, c) in corners.iter().enumerate() {
        for (ni, n) in named.iter().enumerate() {
            let middle = distance_to_span(n.station_m, c.entry_m - 30.0, c.exit_m + 30.0, lap);
            if middle > 0.0 {
                continue;
            }
            let to_apex = distance_to_span(n.station_m, c.apex_m, c.apex_m, lap);
            pairs.push((to_apex, ci, ni));
        }
    }
    pairs.sort_by(|a, b| a.0.total_cmp(&b.0));
    let mut out = vec![None; corners.len()];
    let mut used = vec![false; named.len()];
    for (_, ci, ni) in pairs {
        if out[ci].is_none() && !used[ni] {
            out[ci] = Some(named[ni].display_name.clone());
            used[ni] = true;
        }
    }
    out
}

// --- A solo run ---------------------------------------------------------------

/// One lone AI car's laps, every frame from green.
#[derive(Debug, Clone)]
pub struct SoloRun {
    pub car_id: CarConfigId,
    pub driver: String,
    pub rows: Vec<CarRow>,
    pub ticks: Vec<u32>,
    /// Frame where the first flying lap begins (the car on the line).
    pub lap_start: usize,
    /// Frame where it ends.
    pub lap_end: usize,
    pub lap_time_s: f32,
    /// Inside track limits all the way round.
    pub valid: bool,
    /// Seconds of the flying lap with all four wheels off the track.
    pub off_track_s: f32,
    pub seed: u64,
}

/// A session of one AI car, on the grid and counting down.
fn solo_session(
    track: &TrackConfig,
    cars: &CarMap,
    car: CarConfigId,
    skill: u8,
    seed: u64,
    driver: &str,
    tick_rate: u16,
) -> GameSession {
    let mut profile = AiDriverProfile::new(driver, skill);
    profile.id = seeded_id(seed, 0);
    profile.preferred_car_id = Some(car);
    let mut session = RaceSession::new(
        seeded_id(seed, u64::MAX),
        track.id,
        SessionKind::Multiplayer,
        1,
        1,
        9,
    );
    session.host_player_id = seeded_id(seed, u64::MAX - 1);
    session.host_car_id = Some(car);
    session.conditions = track_conditions(track);
    let mut race =
        GameSession::with_ai_profiles(session, track.clone(), cars.clone(), vec![profile]);
    race.set_tick_rate(tick_rate);
    race.spawn_ai_drivers();
    race.start_countdown_mode(1, GameMode::Race);
    race
}

/// The conditions every guide is driven in: a dry, still noon, so a figure
/// on the card is the car's and not the weather's.
pub fn guide_conditions() -> SessionConditions {
    SessionConditions {
        wind_kph: Some(0),
        ..SessionConditions::DEFAULT
    }
}

fn track_conditions(_track: &TrackConfig) -> SessionConditions {
    guide_conditions().resolve(0)
}

fn seeded_id(seed: u64, index: u64) -> uuid::Uuid {
    let mut state = seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0x6775_6964_6500_0000;
    let mut next = || {
        state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    };
    let high = next() as u128;
    let low = next() as u128;
    uuid::Builder::from_random_bytes(((high << 64) | low).to_be_bytes()).into_uuid()
}

/// Run one car from the grid through its first flying lap and `tail_s`
/// beyond, recording a row every `record_every` ticks.
#[allow(clippy::too_many_arguments)]
pub fn solo_run(
    track: &TrackConfig,
    cars: &CarMap,
    car: CarConfigId,
    skill: u8,
    seed: u64,
    driver: &str,
    tick_rate: u16,
    record_every: u32,
    tail_s: f32,
) -> Result<SoloRun, String> {
    let mut race = solo_session(track, cars, car, skill, seed, driver, tick_rate);
    let Some(pid) = race.session.participants.keys().next().copied() else {
        return Err("the car could not be seated".to_string());
    };
    let lap = lap_length_m(track).max(1.0);
    let tick_rate_f = tick_rate as f32;
    // Generous: a lap at 20 m/s on average, twice over, and the countdown.
    let max_ticks = ((lap / 20.0) * 3.0 * tick_rate_f) as u32 + 10 * tick_rate as u32;
    let tail_ticks = (tail_s * tick_rate_f) as u32;

    let mut rows = Vec::new();
    let mut ticks = Vec::new();
    let mut lap_changes: Vec<usize> = Vec::new();
    let mut last_lap: Option<u16> = None;
    let mut flying: Option<(u32, bool)> = None; // lap time ms, valid
    let mut off_frames = 0u32;
    let mut done_at: Option<u32> = None;
    loop {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(pid, race.generate_ai_input(&pid))].into_iter().collect();
        race.tick(&inputs);
        let tick = race.session.current_tick;
        let events = race.take_lap_events();
        let Some(state) = race.session.participants.get(&pid) else {
            return Err("the car left the session".to_string());
        };
        if race.session.state == SessionState::Racing && tick.is_multiple_of(record_every) {
            if last_lap.is_some_and(|l| l != state.current_lap) {
                lap_changes.push(rows.len());
            }
            last_lap = Some(state.current_lap);
            if lap_changes.len() == 1 && state.wheels_off_track {
                off_frames += 1;
            }
            rows.push(CarRow::from_car_state(state, 0));
            ticks.push(tick);
        }
        if lap_changes.len() >= 2 && flying.is_none() {
            // The lap end that closed the flying lap.
            if let Some(end) = events
                .iter()
                .find_map(|e| e.event.lap_time_ms.map(|ms| (ms, e.event.valid)))
            {
                flying = Some(end);
            }
        }
        if lap_changes.len() >= 2 && done_at.is_none() {
            done_at = Some(tick + tail_ticks);
        }
        if done_at.is_some_and(|d| tick >= d) {
            break;
        }
        if !state.damage.is_drivable {
            let d = &state.damage;
            return Err(format!(
                "the car retired at {:.0} m on lap {} (damage front {:.0} rear {:.0} left {:.0} right {:.0} engine {:.0}, water {:.0} °C)",
                state.track_progress,
                state.current_lap,
                d.front_damage_percent,
                d.rear_damage_percent,
                d.left_damage_percent,
                d.right_damage_percent,
                d.engine_damage_percent,
                state.water_temp_c,
            ));
        }
        if tick >= max_ticks {
            return Err(format!(
                "no flying lap within {:.0} s",
                max_ticks as f32 / tick_rate_f
            ));
        }
    }
    let (lap_start, lap_end) = (lap_changes[0], lap_changes[1]);
    let frame_s = record_every as f32 / tick_rate_f;
    let (lap_ms, valid) = flying.unwrap_or((
        ((lap_end - lap_start) as f32 * frame_s * 1000.0) as u32,
        off_frames == 0,
    ));
    Ok(SoloRun {
        car_id: car,
        driver: driver.to_string(),
        rows,
        ticks,
        lap_start,
        lap_end,
        lap_time_s: lap_ms as f32 / 1000.0,
        valid,
        off_track_s: off_frames as f32 * frame_s,
        seed,
    })
}

// --- Building a guide -----------------------------------------------------------------

/// A built guide: the file and the stream it plays.
#[derive(Debug, Clone)]
pub struct BuiltGuide {
    pub guide: TrackGuide,
    pub stream: StreamContent,
    pub runs: Vec<SoloRun>,
}

pub fn display_class(raw: &str) -> String {
    match raw.trim().to_ascii_lowercase().as_str() {
        "f1" => "Formula".to_string(),
        "wec" | "endurance" => "Endurance".to_string(),
        "dtm" | "gt3" => "GT3".to_string(),
        "indycar" | "indy" => "Independent".to_string(),
        _ => raw.trim().to_string(),
    }
}

/// The class's cars, by folder name.
pub fn class_cars(cars: &CarMap, folders: &FolderMap, class: &str) -> Vec<CarConfigId> {
    let mut found: Vec<(String, CarConfigId)> = cars
        .values()
        .filter(|c| c.class.trim().eq_ignore_ascii_case(class.trim()))
        .map(|c| (folders.get(&c.id).cloned().unwrap_or_default(), c.id))
        .collect();
    found.sort();
    found.into_iter().map(|(_, id)| id).collect()
}

const DRIVERS: [&str; 3] = ["Guide", "Second", "Third"];

/// A run of `car` that keeps inside the track limits, trying seeds from
/// `first_seed`; the least bad when none does.
#[allow(clippy::too_many_arguments)]
fn best_run(
    track: &TrackConfig,
    cars: &CarMap,
    car: CarConfigId,
    opts: &GuideOptions,
    first_seed: u64,
    driver: &str,
    record_every: u32,
    tail_s: f32,
) -> Result<SoloRun, String> {
    let mut best: Option<SoloRun> = None;
    let mut last_err = String::new();
    // At the top of the scale the AI has no randomness, so another seed is
    // the same lap: each retry steps the skill down instead, which moves
    // its braking points and its line.
    for attempt in 0..opts.tries.max(1) {
        let seed = first_seed + attempt as u64;
        let skill = opts.skill.saturating_sub(2 * attempt as u8).max(90);
        match solo_run(
            track,
            cars,
            car,
            skill,
            seed,
            driver,
            opts.tick_rate,
            record_every,
            tail_s,
        ) {
            Ok(run) => {
                if run.valid && run.off_track_s == 0.0 {
                    return Ok(run);
                }
                eprintln!(
                    "  skill {skill}: {} ({:.1} s off the track), trying another",
                    if run.valid { "legal" } else { "lap struck" },
                    run.off_track_s
                );
                let better = best
                    .as_ref()
                    .is_none_or(|b| (run.valid, -run.off_track_s) > (b.valid, -b.off_track_s));
                if better {
                    best = Some(run);
                }
            }
            Err(e) => {
                eprintln!("  skill {skill}: {e}");
                last_err = e;
            }
        }
    }
    best.ok_or(last_err)
}

/// Seconds of the stream kept before the guide car reaches the line.
const LEAD_IN_S: f32 = 5.0;
/// The overview freezes the guide car this far short of the line.
const OVERVIEW_SHORT_M: f32 = 25.0;
/// Seconds kept after the last car finishes its lap.
const TAIL_S: f32 = 9.0;

pub fn build_guide(opts: &GuideOptions) -> Result<BuiltGuide, String> {
    let stem = track_stem(&opts.track_path).ok_or("the track path has no stem")?;
    let mut track = TrackLoader::load_from_file(&opts.track_path)
        .map_err(|e| format!("track {}: {e}", opts.track_path.display()))?;
    guide_conditions().resolve(0).apply_to_track(&mut track);
    let lap = lap_length_m(&track);
    if lap <= 0.0 {
        return Err("the track has no centerline".to_string());
    }

    let (cars, folders) = load_car_folder(&opts.cars_dir)?;
    let field = class_cars(&cars, &folders, &opts.class);
    if field.is_empty() {
        return Err(format!(
            "no car of class '{}' under {}",
            opts.class,
            opts.cars_dir.display()
        ));
    }

    let tick_rate = opts.tick_rate.max(1);
    let record_every = (tick_rate / opts.record_hz.clamp(1, tick_rate)).max(1) as u32;
    let fps = tick_rate as f32 / record_every as f32;
    let count = opts.cars.clamp(1, 3) as usize;
    let gap_frames = (opts.car_gap_s.max(0.0) * fps).round() as usize;
    let tail_s = TAIL_S + opts.car_gap_s * (count - 1) as f32;

    // Car k drives the class's k-th car; one that cannot make a flying lap
    // (an engine that cooks on a long lap) hands its place to the next.
    let mut runs: Vec<SoloRun> = Vec::with_capacity(count);
    let mut broken: Vec<CarConfigId> = Vec::new();
    for k in 0..count {
        let mut last_err = String::new();
        let mut done = false;
        for step in 0..field.len() {
            let car = field[(k + step) % field.len()];
            if broken.contains(&car) {
                continue;
            }
            eprintln!(
                "{stem} {}: car {} ({})",
                opts.class,
                k + 1,
                folders.get(&car).map_or("?", |f| f.as_str())
            );
            match best_run(
                &track,
                &cars,
                car,
                opts,
                opts.seed + 1000 * k as u64,
                DRIVERS[k],
                record_every,
                tail_s,
            ) {
                Ok(run) => {
                    runs.push(run);
                    done = true;
                    break;
                }
                Err(e) => {
                    eprintln!("  {e}: trying another car");
                    broken.push(car);
                    last_err = e;
                }
            }
        }
        if !done {
            return Err(last_err);
        }
    }

    // The merged timeline: output frame i shows run k at its frame
    // lap_start_k - lead + i - k * gap.
    let lead = (LEAD_IN_S * fps).round() as usize;
    let length = runs[0].lap_end - runs[0].lap_start + lead + (TAIL_S * fps) as usize;
    let frame_of = |k: usize, i: usize| -> usize {
        let run = &runs[k];
        let j = run.lap_start as isize - lead as isize + i as isize - (k * gap_frames) as isize;
        j.clamp(0, run.rows.len() as isize - 1) as usize
    };
    for (k, run) in runs.iter().enumerate() {
        if run.lap_start < lead + k * gap_frames {
            return Err("the first lap is too short to lead into the second".to_string());
        }
    }

    // --- The stream.
    let race = solo_session(
        &track,
        &cars,
        runs[0].car_id,
        opts.skill,
        0,
        DRIVERS[0],
        tick_rate,
    );
    let encoder = BroadcastEncoder::new(0);
    let mut header = encoder.header(&race, &stem);
    let path = encoder.path(&race);
    let sectors = encoder.sectors(&race);
    let roster = StreamRoster {
        epoch: 0,
        revision: 0,
        entries: runs
            .iter()
            .enumerate()
            .map(|(k, run)| StreamRosterEntry {
                car_index: k as u8,
                car_config_id: run.car_id,
                content_crc: cars.get(&run.car_id).map_or(0, |c| c.content_crc),
                // The same car twice wears its next livery.
                livery: runs[..k].iter().filter(|r| r.car_id == run.car_id).count() as u8,
                name: run.driver.clone(),
                is_ai: true,
            })
            .collect(),
    };
    let base_tick = 1u32;
    let mut records = Vec::with_capacity(length);
    for i in 0..length {
        let rows: Vec<CarRow> = (0..runs.len())
            .map(|k| {
                let mut row = runs[k].rows[frame_of(k, i)];
                row.car_index = k as u8;
                row
            })
            .collect();
        let tick = base_tick + i as u32 * record_every;
        for body in encode_frames(0, tick, 0, SessionState::Racing, None, &rows) {
            records.push((tick, body));
        }
    }
    header.start_tick = base_tick;
    header.end_tick = base_tick + (length as u32 - 1) * record_every;
    header.frame_rate = fps.round() as u16;
    header.race_start_tick = Some(base_tick);
    header.game_mode = GameMode::Race;
    header.lap_limit = 1;
    header.render = StreamRender {
        seed: Some(opts.seed),
        score: None,
    };
    let stream = StreamContent {
        header,
        roster,
        path: Some(path),
        preamble: vec![sectors],
        records,
    };

    // --- The guide car's lap, for the measurements.
    let lead_run = &runs[0];
    let first = lead_run.lap_start - lead; // run-0 frame of output frame 0
    let lap_number = lead_run.rows[lead_run.lap_start].lap;
    let samples: Vec<Sample> = (first..lead_run.rows.len())
        .map(|j| {
            let row = &lead_run.rows[j];
            Sample {
                time_s: (j - first) as f32 / fps,
                distance_m: (row.lap as f32 - lap_number as f32) * lap + row.station_m(),
                speed_mps: row.speed_mps(),
                gear: row.gear,
                throttle: row.throttle as f32 / 255.0,
                brake: row.brake as f32 / 255.0,
            }
        })
        .collect();
    let lap_samples =
        &samples[lead..(lead + lead_run.lap_end - lead_run.lap_start).min(samples.len())];
    let top_speed_kph = lap_samples
        .iter()
        .map(|s| s.speed_mps * 3.6)
        .fold(0.0, f32::max);

    // --- The corners.
    let corners = detect_corners(&track);
    let names = name_corners(&corners, &dossier_corners(&opts.track_path), lap);
    let notes = load_notes(&opts.track_path)?;
    let mut corner_notes: Vec<Vec<String>> = vec![Vec::new(); corners.len()];
    let mut corner_names: Vec<Option<String>> = names;
    for note in &notes.corners {
        let nearest = corners
            .iter()
            .enumerate()
            .map(|(i, c)| (distance_to_span(note.at_m, c.entry_m, c.exit_m, lap), i))
            .min_by(|a, b| a.0.total_cmp(&b.0));
        match nearest {
            Some((d, i)) if d <= NOTE_REACH_M => {
                corner_notes[i].extend(note.notes.iter().cloned());
                if let Some(name) = &note.name {
                    corner_names[i] = Some(name.clone());
                }
            }
            _ => eprintln!(
                "{}: no corner near the note at {:.0} m",
                notes_path(&opts.track_path).display(),
                note.at_m
            ),
        }
    }

    let class_name = display_class(&opts.class);
    let mut guide_corners = Vec::with_capacity(corners.len());
    let mut turns_before = 0u32;
    for (i, corner) in corners.iter().enumerate() {
        let next_entry = corners
            .get(i + 1)
            .map_or(corners[0].entry_m + lap, |c| c.entry_m);
        let straight_after_m = (next_entry - corner.exit_m).max(0.0);
        let prev_apex = match i {
            0 => corners.last().map_or(-lap, |c| c.apex_m - lap),
            _ => guide_corners
                .last()
                .map_or(-lap, |c: &GuideCorner| c.apex_m),
        };
        let measured = measure(
            corner,
            &samples,
            opts.car_gap_s * (count - 1) as f32,
            prev_apex,
        );
        let geometry = corner_geometry(&track, corner, &measured);
        let gotchas = gotchas(
            &track,
            corner,
            &measured,
            &geometry,
            straight_after_m,
            &class_name,
        );
        // The card holds six lines and does not scroll: the hand-written
        // notes keep their place, the lowest-ranked gotchas give way.
        let mut gotchas = gotchas;
        gotchas.truncate(MAX_CARD_LINES.saturating_sub(corner_notes[i].len()).max(2));
        let turn_from = turns_before + 1;
        let turn_to = turns_before + corner.turn_count();
        turns_before = turn_to;
        guide_corners.push(GuideCorner {
            number: (i + 1) as u32,
            turn_from,
            turn_to,
            name: corner_names[i].clone().unwrap_or_else(|| {
                if turn_to > turn_from {
                    format!("Turns {turn_from}-{turn_to}")
                } else {
                    format!("Turn {turn_from}")
                }
            }),
            direction: corner.direction(),
            entry_m: round1(corner.entry_m),
            apex_m: round1(measured.apex_m),
            exit_m: round1(corner.exit_m),
            turn_deg: round1(corner.turn_rad().to_degrees()),
            min_radius_m: round1(1.0 / corner.peak_kappa().max(1e-4)),
            clip: measured.clip.clone(),
            min_speed_kph: round1(measured.min_speed_mps * 3.6),
            apex_gear: measured.apex_gear,
            entry_speed_kph: round1(measured.entry_speed_mps * 3.6),
            exit_speed_kph: round1(measured.exit_speed_mps * 3.6),
            brake_m: measured.brake_m.map(round1),
            flat_out: measured.flat_out,
            elevation_change_m: round1(geometry.elevation_change_m),
            banking_deg: round1(geometry.banking_into_deg),
            gotchas,
            notes: corner_notes[i].clone(),
            cameras: corner_cameras(&track, corner, &measured),
        });
    }

    // The overview freezes the guide car just short of the line, where
    // both overview cameras (behind the line, looking down it) see it.
    let overview_time_s = samples
        .iter()
        .find(|s| s.distance_m >= -OVERVIEW_SHORT_M)
        .map_or(LEAD_IN_S - 1.0, |s| s.time_s);
    let facts = track_facts(&track, &corners);
    let meta = track.metadata.clone();
    let car0 = cars.get(&runs[0].car_id);
    let guide = TrackGuide {
        version: GUIDE_VERSION,
        class: opts.class.clone(),
        display_class: class_name,
        recording: recording_name(&stem, &opts.class),
        source_crc: track.content_crc,
        car: GuideCar {
            id: runs[0].car_id.to_string(),
            folder: folders.get(&runs[0].car_id).cloned().unwrap_or_default(),
            name: car0.map_or(String::new(), |c| c.name.clone()),
        },
        car_gap_s: opts.car_gap_s,
        track: GuideTrack {
            stem: stem.clone(),
            track_id: track.id.to_string(),
            display_name: track.name.clone(),
            description: meta.description.clone(),
            country: meta.country.clone(),
            city: meta.city.clone(),
            category: meta.category.clone(),
            year_built: meta.year_built,
            length_m: round1(lap),
            altitude_m: meta.altitude_m.map(round1),
            latitude_deg: meta.latitude_deg,
            longitude_deg: meta.longitude_deg,
            elevation_min_m: round1(facts.elevation_min_m),
            elevation_max_m: round1(facts.elevation_max_m),
            climb_m: round1(facts.climb_m),
            corners: corners.iter().map(Corner::turn_count).sum(),
            left: hands(&corners, 1.0),
            right: hands(&corners, -1.0),
            direction: facts.direction,
            longest_straight_m: round1(facts.longest_straight_m),
            drs_zones: track.drs_zones.len() as u32,
            lap_time_s: (lead_run.lap_time_s * 1000.0).round() / 1000.0,
            top_speed_kph: round1(top_speed_kph),
            notes: notes.overview.clone(),
        },
        overview: GuideOverview {
            time_s: round2(overview_time_s),
            cameras: overview_cameras(&track),
        },
        corners: guide_corners,
    };
    Ok(BuiltGuide {
        guide,
        stream,
        runs,
    })
}

/// Turns of one hand, counted as [`Corner::turn_count`] counts them.
fn hands(corners: &[Corner], sign: f32) -> u32 {
    corners
        .iter()
        .map(|c| {
            let mut n = 0;
            let mut last = 0.0;
            for run in &c.runs {
                if run.sign != last && run.sign == sign {
                    n += 1;
                }
                last = run.sign;
            }
            n
        })
        .sum()
}

pub fn guide_name(stem: &str, class: &str) -> String {
    format!("{stem}.{class}.guide.json")
}

pub fn recording_name(stem: &str, class: &str) -> String {
    format!("{stem}.{class}.guide.apxs")
}

/// Write the guide and its stream into `out_dir`.
pub fn write_guide(built: &BuiltGuide, out_dir: &Path) -> Result<(PathBuf, PathBuf), String> {
    std::fs::create_dir_all(out_dir).map_err(|e| format!("{}: {e}", out_dir.display()))?;
    let json_path = out_dir.join(guide_name(&built.guide.track.stem, &built.guide.class));
    let stream_path = out_dir.join(&built.guide.recording);
    built
        .stream
        .write_file(&stream_path)
        .map_err(|e| format!("{}: {e}", stream_path.display()))?;
    let mut text = serde_json::to_string_pretty(&built.guide).map_err(|e| e.to_string())?;
    text.push('\n');
    std::fs::write(&json_path, text).map_err(|e| format!("{}: {e}", json_path.display()))?;
    Ok((json_path, stream_path))
}

fn round1(v: f32) -> f32 {
    (v * 10.0).round() / 10.0
}

fn round2(v: f32) -> f32 {
    (v * 100.0).round() / 100.0
}

// --- Measuring a corner -----------------------------------------------------------------

/// The guide car at one frame of the stream.
#[derive(Debug, Clone, Copy)]
pub struct Sample {
    pub time_s: f32,
    /// Distance from the start of the flying lap (negative before it).
    pub distance_m: f32,
    pub speed_mps: f32,
    pub gear: i8,
    pub throttle: f32,
    pub brake: f32,
}

/// What the guide car did through a corner.
#[derive(Debug, Clone, PartialEq)]
pub struct Measured {
    pub apex_m: f32,
    pub min_speed_mps: f32,
    pub apex_gear: i8,
    pub entry_speed_mps: f32,
    pub exit_speed_mps: f32,
    /// Station the braking began at.
    pub brake_at_m: Option<f32>,
    pub brake_m: Option<f32>,
    pub flat_out: bool,
    /// A lift without the brake.
    pub lift: bool,
    pub clip: GuideClip,
}

/// The first sample at or past `distance`.
fn sample_at(samples: &[Sample], distance: f32) -> usize {
    samples
        .iter()
        .position(|s| s.distance_m >= distance)
        .unwrap_or(samples.len().saturating_sub(1))
}

/// Measure the guide car through `corner`; `followers_s` is how long after
/// it the last following car passes, which the loop must wait for.
///
/// `prev_apex_m` is where the corner before it was taken: nothing before it
/// (its braking, its slowest point) belongs to this one.
pub fn measure(
    corner: &Corner,
    samples: &[Sample],
    followers_s: f32,
    prev_apex_m: f32,
) -> Measured {
    let window_m = (corner.entry_m - 400.0)
        .max(prev_apex_m + 20.0)
        .min(corner.entry_m);
    let entry = sample_at(samples, corner.entry_m);
    let exit = sample_at(samples, corner.exit_m).max(entry);
    let (min_i, _) = samples[entry..=exit]
        .iter()
        .enumerate()
        .min_by(|a, b| a.1.speed_mps.total_cmp(&b.1.speed_mps))
        .map(|(i, s)| (entry + i, s.speed_mps))
        .unwrap_or((entry, 0.0));
    let window = sample_at(samples, window_m).min(entry);
    let touched_brake = samples[window..=exit].iter().any(|s| s.brake > 0.15);
    // The car slowing for the corner without the brake: the most it gave
    // up from the 200 m before the entry to the slowest point.
    let approach_top = samples[sample_at(samples, corner.entry_m - 200.0).max(window)..=min_i]
        .iter()
        .map(|s| s.speed_mps)
        .fold(0.0, f32::max);
    let slowed = (approach_top - samples[min_i].speed_mps) * 3.6 >= 12.0;

    // The braking point: the start of the stretch on the brake (gaps of
    // under a third of a second bridged: the AI trails and re-applies) that
    // takes the most speed off between the window's start and the slowest
    // point. A dab mid-corner is a stretch too, but a small one.
    let mut brake_i = None;
    if touched_brake {
        let mut best_drop = 0.0;
        let mut i = window;
        while i <= min_i {
            if samples[i].brake <= 0.02 {
                i += 1;
                continue;
            }
            let start = i;
            let mut end = i;
            let mut j = i + 1;
            while j <= min_i {
                if samples[j].brake > 0.02 {
                    end = j;
                } else if samples[j].time_s - samples[end].time_s > 0.33 {
                    break;
                }
                j += 1;
            }
            let peak = samples[start..=end]
                .iter()
                .map(|s| s.brake)
                .fold(0.0, f32::max);
            let drop = samples[start].speed_mps - samples[end].speed_mps;
            // The stop is this corner's: it ends at most 150 m before the
            // entry and starts faster than the corner is taken. A run that
            // ended in the corner before only looks like one.
            let ends_near = samples[end].distance_m >= corner.entry_m - 150.0;
            let from_above = samples[start].speed_mps > samples[min_i].speed_mps;
            if peak > 0.15 && drop > 3.0 && ends_near && from_above && drop > best_drop {
                best_drop = drop;
                brake_i = Some(start);
            }
            i = end + 1;
        }
    }
    let braked = brake_i.is_some();
    let flat_out = !braked && !slowed;
    let lift = !braked && slowed;
    // A flat corner's apex is where the road is tightest; a braked one's
    // where the car is slowest.
    let apex_i = if flat_out {
        sample_at(samples, corner.apex_m)
    } else {
        min_i
    };
    let (brake_at_m, brake_m) = match brake_i {
        Some(b) if braked => (
            Some(samples[b].distance_m),
            Some(samples[min_i].distance_m - samples[b].distance_m),
        ),
        _ => (None, None),
    };
    let entry_speed = match brake_i {
        Some(b) if braked => samples[b].speed_mps,
        _ => approach_top.max(samples[entry].speed_mps),
    };

    // The loop: from a little before the braking point (or the entry) to
    // when the last follower is past the apex and on its way out.
    let apex_t = samples[apex_i].time_s;
    let lead_from = brake_i
        .map(|b| samples[b].time_s)
        .unwrap_or(samples[entry].time_s)
        - 1.5;
    let from_s = lead_from.clamp(apex_t - 7.0, apex_t - 3.0).max(0.0);
    let to_s = (apex_t + followers_s + 2.5).max(from_s + 6.0);
    // Slow motion: the guide car from turn-in to the exit, at most 4.5 s.
    let turn_in_t = samples[entry].time_s - 0.5;
    let slow_from_s = turn_in_t.clamp(apex_t - 3.0, apex_t - 0.5).max(from_s);
    let slow_to_s = samples[exit]
        .time_s
        .clamp(apex_t + 1.0, slow_from_s + 4.5)
        .min(to_s);
    Measured {
        apex_m: samples[apex_i].distance_m,
        min_speed_mps: samples[min_i].speed_mps,
        apex_gear: samples[min_i].gear,
        entry_speed_mps: entry_speed,
        exit_speed_mps: samples[exit].speed_mps,
        brake_at_m,
        brake_m,
        flat_out,
        lift,
        clip: GuideClip {
            from_s: round2(from_s),
            to_s: round2(to_s),
            slow_from_s: round2(slow_from_s),
            slow_to_s: round2(slow_to_s),
            slow_rate: 0.25,
            apex_s: round2(apex_t),
        },
    }
}

/// What the road does through a corner.
#[derive(Debug, Clone, PartialEq)]
pub struct Geometry {
    pub elevation_change_m: f32,
    /// Gradient over the braking zone (rise over run; negative downhill).
    pub braking_slope: f32,
    pub banking_into_deg: f32,
    /// The car's vertical acceleration over the most convex point near the
    /// turn-in, m/s² (negative: it goes light).
    pub crest_accel: f32,
    /// ...and over the most concave point (positive: it is pressed down).
    pub dip_accel: f32,
    /// Where along the corner the road is tightest, 0 (entry) to 1 (exit).
    pub tightest_at: f32,
    /// Nearest wall beyond the road edge on the outside of the exit, m.
    pub exit_wall_m: Option<f32>,
    /// Tarmac run-off on the outside of the exit, m.
    pub exit_runoff_m: f32,
}

fn height_at(track: &TrackConfig, station: f32) -> f32 {
    pose_at(track, station, 0.0, 0.0).z
}

pub fn corner_geometry(track: &TrackConfig, corner: &Corner, measured: &Measured) -> Geometry {
    let lap = lap_length_m(track);
    let z_entry = height_at(track, corner.entry_m);
    let z_exit = height_at(track, corner.exit_m);
    let braking_slope = match (measured.brake_at_m, measured.brake_m) {
        (Some(at), Some(len)) if len > 20.0 => {
            (height_at(track, at + len) - height_at(track, at)) / len
        }
        _ => 0.0,
    };

    let apex = measured.apex_m.rem_euclid(lap.max(1.0));
    let idx = track
        .centerline
        .partition_point(|p| p.distance_from_start_m < apex)
        .min(track.centerline.len().saturating_sub(1));
    let banking = track.centerline.get(idx).map_or(0.0, |p| p.banking_rad);
    // Positive banking lifts the left edge, which leans a right-hander in.
    let banking_into_deg = (banking * -corner.main_sign()).to_degrees();

    // Vertical curvature over ±20 m, from 80 m before the entry to the apex,
    // times the car's speed there.
    let speed = measured.entry_speed_mps.max(measured.min_speed_mps);
    let mut crest: f32 = 0.0;
    let mut dip: f32 = 0.0;
    let mut s = corner.entry_m - 80.0;
    while s <= measured.apex_m.max(corner.entry_m) {
        let d2 = (height_at(track, s + 20.0) - 2.0 * height_at(track, s)
            + height_at(track, s - 20.0))
            / 400.0;
        let v = if s < corner.entry_m {
            speed
        } else {
            measured.min_speed_mps.max(speed * 0.7)
        };
        crest = crest.min(d2 * v * v);
        dip = dip.max(d2 * v * v);
        s += 10.0;
    }

    // Where the tightest point falls along the corner.
    let span = (corner.exit_m - corner.entry_m).max(1.0);
    // The geometric apex is the tightest point (`detect_corners`), in the
    // same unrolled frame as the entry.
    let tightest_at = ((corner.apex_m - corner.entry_m) / span).clamp(0.0, 1.0);

    // The exit's outside: walls and run-off.
    let outside_left = corner.runs.last().map_or(1.0, |r| r.sign) < 0.0;
    let mut exit_wall_m: Option<f32> = None;
    let mut exit_runoff_m: f32 = 0.0;
    let mut s = measured.apex_m;
    while s <= corner.exit_m + 60.0 {
        let pose = pose_at(track, s, 0.0, 0.0);
        let (edge, sign) = if outside_left {
            (pose.width_left_m, 1.0)
        } else {
            (pose.width_right_m, -1.0)
        };
        if let Some(curbs) = &track.curbs {
            // The curbs speak in "positive = right".
            exit_runoff_m = exit_runoff_m.max(curbs.runoff_at(s.rem_euclid(lap.max(1.0)), -sign));
        }
        if let Some(walls) = &track.walls {
            let edge_pose = pose_at(track, s, sign * edge, 0.0);
            let mut near = Vec::new();
            walls.candidates(edge_pose.x, edge_pose.y, 30.0, &mut near);
            for &w in &near {
                let seg = &walls.segments()[w as usize];
                let d = point_segment_distance(edge_pose.x, edge_pose.y, seg);
                // Only what stands beyond the edge, not across the road.
                let beyond = pose_at(track, s, sign * (edge + d), 0.0);
                if point_segment_distance(beyond.x, beyond.y, seg) < 1.0 {
                    exit_wall_m = Some(exit_wall_m.map_or(d, |m: f32| m.min(d)));
                }
            }
        }
        s += 10.0;
    }

    Geometry {
        elevation_change_m: z_exit - z_entry,
        braking_slope,
        banking_into_deg,
        crest_accel: crest,
        dip_accel: dip,
        tightest_at,
        exit_wall_m,
        exit_runoff_m,
    }
}

fn point_segment_distance(x: f32, y: f32, seg: &crate::walls::WallSegment) -> f32 {
    let (dx, dy) = (seg.x1 - seg.x0, seg.y1 - seg.y0);
    let len2 = dx * dx + dy * dy;
    let t = if len2 > 0.0 {
        (((x - seg.x0) * dx + (y - seg.y0) * dy) / len2).clamp(0.0, 1.0)
    } else {
        0.0
    };
    (x - (seg.x0 + t * dx)).hypot(y - (seg.y0 + t * dy))
}

/// What a driver should know about a corner that the numbers on the card do
/// not already say, most important first, at most four.
pub fn gotchas(
    track: &TrackConfig,
    corner: &Corner,
    m: &Measured,
    g: &Geometry,
    straight_after_m: f32,
    class_name: &str,
) -> Vec<String> {
    let lap = lap_length_m(track);
    let mut out: Vec<(u8, String)> = Vec::new();
    let kph = |v: f32| (v * 3.6).round() as i32;
    let turn_deg = corner.turn_rad().to_degrees();
    let radius = 1.0 / corner.peak_kappa().max(1e-4);
    let chicane = corner.runs.len() > 1 && corner.direction().contains('-');

    if m.flat_out {
        out.push((
            9,
            format!("Flat out in a {class_name} car: commit, keep the steering smooth and the line tidy."),
        ));
    } else if m.lift {
        out.push((
            8,
            "No brakes: a lift settles the nose. Back on the power early.".to_string(),
        ));
    } else if let Some(len) = m.brake_m {
        let drop = m.entry_speed_mps - m.min_speed_mps;
        if drop * 3.6 > 100.0 {
            out.push((
                9,
                format!(
                    "Big stop: {} to {} km/h. Brake hard in a straight line, then ease off as you turn in.",
                    kph(m.entry_speed_mps),
                    kph(m.min_speed_mps)
                ),
            ));
        } else if len < 60.0 {
            out.push((
                6,
                "A short, sharp brake: a dab to set the car up, not a big stop.".to_string(),
            ));
        }
    }

    if g.braking_slope < -0.025 {
        out.push((
            8,
            "Downhill braking: the car is light and the stop is longer. Brake a little earlier."
                .to_string(),
        ));
    } else if g.braking_slope > 0.025 {
        out.push((
            5,
            "Uphill braking helps the stop: you can brake later than it looks.".to_string(),
        ));
    }

    if g.crest_accel < -3.0 {
        out.push((
            9,
            "Crest on the way in: the car goes light. Turn in gently and wait for the grip."
                .to_string(),
        ));
    }
    if g.dip_accel > 4.0 {
        out.push((
            5,
            "Compression: the car is pressed into the road and has extra grip. You can lean on it."
                .to_string(),
        ));
    }

    if g.banking_into_deg >= 3.0 {
        out.push((
            8,
            format!(
                "Banked {:.0}°: there is more grip than it looks. Carry speed.",
                g.banking_into_deg
            ),
        ));
    } else if g.banking_into_deg <= -1.5 {
        out.push((
            8,
            "Off-camber: the road falls away from the corner, so there is less grip than it looks."
                .to_string(),
        ));
    }

    if chicane {
        out.push((
            7,
            "Change of direction: give up some of the first part so the second is straighter, and keep the car settled between them."
                .to_string(),
        ));
    } else if turn_deg >= 140.0 && radius < 35.0 && m.brake_m.is_some() {
        out.push((
            7,
            "Hairpin: brake in a straight line, rotate the car on the way in, and straighten the exit before full power."
                .to_string(),
        ));
    } else if turn_deg >= 45.0 && g.tightest_at > 0.62 {
        out.push((
            7,
            "It tightens on the way out: apex late and be patient with the throttle.".to_string(),
        ));
    } else if turn_deg >= 45.0 && g.tightest_at < 0.38 && !m.flat_out {
        out.push((
            5,
            "It opens up on the exit: get the car turned early and the power comes early too."
                .to_string(),
        ));
    }

    if !m.flat_out && m.min_speed_mps * 3.6 > 190.0 {
        out.push((
            6,
            "Fast corner: trust the downforce and keep your steering inputs small.".to_string(),
        ));
    }
    if !m.flat_out && m.apex_gear > 0 && m.apex_gear <= 2 {
        out.push((
            4,
            format!(
                "Slow, in gear {}: feed the throttle in on exit or the rear will step out.",
                m.apex_gear
            ),
        ));
    }

    if let Some(wall) = g.exit_wall_m {
        if wall < 4.0 {
            out.push((
                8,
                "A wall is close on the outside of the exit: there is no room for running wide."
                    .to_string(),
            ));
        }
    }
    if g.exit_wall_m.is_none_or(|w| w > 8.0) && g.exit_runoff_m > 8.0 {
        out.push((
            5,
            "Tarmac run-off on the exit forgives a mistake, but four wheels past the kerb strikes the lap."
                .to_string(),
        ));
    }

    if straight_after_m > 700.0 {
        out.push((
            6,
            format!(
                "It leads onto a {:.1} km straight: exit speed matters more than entry speed.",
                straight_after_m / 1000.0
            ),
        ));
    }
    if track.drs_zones.iter().any(|z| {
        let d = (z.detection_m - corner.exit_m).rem_euclid(lap.max(1.0));
        d < 350.0
    }) {
        out.push((
            3,
            "DRS detection is just after the exit: a clean exit sets up the next straight."
                .to_string(),
        ));
    }

    out.sort_by_key(|o| std::cmp::Reverse(o.0));
    out.into_iter().take(4).map(|(_, s)| s).collect()
}

// --- Cameras ----------------------------------------------------------------------------

fn camera(name: &str, eye: [f32; 3], look: [f32; 3], frame_width_m: f32) -> GuideCamera {
    let d = ((eye[0] - look[0]).powi(2) + (eye[1] - look[1]).powi(2) + (eye[2] - look[2]).powi(2))
        .sqrt()
        .max(1.0);
    let fov = (2.0 * (frame_width_m * 0.5 / d).atan())
        .to_degrees()
        .clamp(14.0, 70.0);
    GuideCamera {
        name: name.to_string(),
        kind: "fixed".to_string(),
        eye: eye.map(round2),
        look: look.map(round2),
        fov_deg: round1(fov),
    }
}

fn point(track: &TrackConfig, station: f32, lateral: f32, height: f32) -> [f32; 3] {
    let p = pose_on_ground(track, station, lateral, height);
    [p.x, p.y, p.z]
}

fn road_point(track: &TrackConfig, station: f32, height: f32) -> [f32; 3] {
    let p = pose_at(track, station, 0.0, height);
    [p.x, p.y, p.z]
}

/// The lateral that puts a point `beyond` metres past the road edge on the
/// outside (`outside_left`) or inside.
fn off_edge(track: &TrackConfig, station: f32, left: bool, beyond: f32) -> f32 {
    let p = pose_at(track, station, 0.0, 0.0);
    if left {
        p.width_left_m + beyond
    } else {
        -(p.width_right_m + beyond)
    }
}

pub fn corner_cameras(track: &TrackConfig, corner: &Corner, m: &Measured) -> Vec<GuideCamera> {
    let apex = m.apex_m;
    let sign = corner.main_sign();
    let outside_left = sign < 0.0;
    let apex_look = road_point(track, apex, 1.0);
    let mut cams = Vec::new();

    // Outside the apex, looking across at it.
    cams.push(camera(
        "Trackside",
        point(track, apex, off_edge(track, apex, outside_left, 20.0), 6.5),
        apex_look,
        70.0,
    ));

    // On the inside of the braking zone, looking down the road to the apex.
    let approach = m
        .brake_at_m
        .map(|b| b - 25.0)
        .unwrap_or(corner.entry_m - 80.0);
    cams.push(camera(
        if m.brake_at_m.is_some() {
            "Braking zone"
        } else {
            "Approach"
        },
        point(
            track,
            approach,
            off_edge(track, approach, !outside_left, 10.0),
            5.0,
        ),
        road_point(track, (approach + apex) * 0.5 + 20.0, 1.0),
        90.0,
    ));

    // Past the exit on the outside, looking back at the cars coming through.
    let exit = corner.exit_m + 35.0;
    cams.push(camera(
        "Exit",
        point(track, exit, off_edge(track, exit, outside_left, 12.0), 4.0),
        road_point(track, (corner.entry_m + apex) * 0.5, 1.0),
        80.0,
    ));

    // High above the corner.
    let span = (corner.exit_m - corner.entry_m).max(60.0);
    let middle = (corner.entry_m + corner.exit_m) * 0.5;
    let height = (span * 0.55).clamp(60.0, 180.0);
    let ground = road_point(track, middle, 0.0);
    let back = pose_at(track, middle - span * 0.35, 0.0, 0.0);
    cams.push(camera(
        "Overhead",
        [back.x, back.y, ground[2] + height],
        ground,
        span * 1.3,
    ));
    cams
}

pub fn overview_cameras(track: &TrackConfig) -> Vec<GuideCamera> {
    let left = off_edge(track, -90.0, true, 14.0);
    vec![
        camera(
            "Start line",
            point(track, -90.0, left, 9.0),
            road_point(track, 15.0, 1.0),
            60.0,
        ),
        {
            let ground = road_point(track, 0.0, 0.0);
            let back = pose_at(track, -140.0, 0.0, 0.0);
            camera(
                "Overhead",
                [back.x, back.y, ground[2] + 110.0],
                road_point(track, 60.0, 0.0),
                220.0,
            )
        },
    ]
}

// --- Track facts --------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct TrackFacts {
    pub elevation_min_m: f32,
    pub elevation_max_m: f32,
    pub climb_m: f32,
    pub direction: String,
    pub longest_straight_m: f32,
}

pub fn track_facts(track: &TrackConfig, corners: &[Corner]) -> TrackFacts {
    let lap = lap_length_m(track);
    let z0 = height_at(track, 0.0);
    let mut min: f32 = 0.0;
    let mut max: f32 = 0.0;
    let mut climb: f32 = 0.0;
    // Heights every 20 m, so a few centimetres of trace noise is not climb.
    let mut last = z0;
    let mut s = 20.0;
    while s <= lap {
        let z = height_at(track, s);
        min = min.min(z - z0);
        max = max.max(z - z0);
        if z > last {
            climb += z - last;
        }
        last = z;
        s += 20.0;
    }
    // Shoelace over the centerline: positive is anticlockwise (+Y left).
    let area: f32 = track
        .centerline
        .iter()
        .zip(track.centerline.iter().cycle().skip(1))
        .map(|(a, b)| a.x * b.y - b.x * a.y)
        .sum();
    let longest = corners
        .iter()
        .enumerate()
        .map(|(i, c)| {
            let next = corners
                .get(i + 1)
                .map_or(corners[0].entry_m + lap, |n| n.entry_m);
            next - c.exit_m
        })
        .fold(0.0, f32::max);
    TrackFacts {
        elevation_min_m: min,
        elevation_max_m: max,
        climb_m: climb,
        direction: if area >= 0.0 {
            "anticlockwise"
        } else {
            "clockwise"
        }
        .to_string(),
        longest_straight_m: if corners.is_empty() { lap } else { longest },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn zandvoort() -> Option<PathBuf> {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../content/tracks/default/Zandvoort.yaml");
        path.is_file().then_some(path)
    }

    #[test]
    fn a_chicane_is_one_stop_with_both_hands() {
        let runs = vec![
            TurnRun {
                start_m: 100.0,
                end_m: 150.0,
                sign: 1.0,
                turn_rad: 0.8,
                peak_kappa: 0.03,
                peak_m: 120.0,
            },
            TurnRun {
                start_m: 170.0,
                end_m: 220.0,
                sign: -1.0,
                turn_rad: -0.8,
                peak_kappa: 0.04,
                peak_m: 200.0,
            },
        ];
        let corner = Corner {
            runs,
            entry_m: 100.0,
            apex_m: 200.0,
            exit_m: 220.0,
        };
        assert_eq!(corner.direction(), "left-right");
        assert!((corner.turn_rad() - 1.6).abs() < 1e-6);
    }

    #[test]
    fn names_go_to_the_nearest_corner_once() {
        let corner = |apex: f32| Corner {
            runs: vec![],
            entry_m: apex - 40.0,
            apex_m: apex,
            exit_m: apex + 40.0,
        };
        let corners = vec![corner(100.0), corner(400.0), corner(990.0)];
        let named = vec![
            NamedCorner {
                display_name: "First".into(),
                station_m: 110.0,
                from_m: 60.0,
                to_m: 160.0,
            },
            NamedCorner {
                display_name: "Across the line".into(),
                station_m: 5.0,
                from_m: 0.0,
                to_m: 20.0,
            },
        ];
        let names = name_corners(&corners, &named, 1000.0);
        assert_eq!(names[0].as_deref(), Some("First"));
        assert_eq!(names[1], None);
        // 5 m is 15 m past the corner at 990 round a 1000 m lap.
        assert_eq!(names[2].as_deref(), Some("Across the line"));
    }

    #[test]
    fn notes_parse_from_yaml() {
        let notes: GuideNotes = serde_yaml::from_str(
            "overview:\n  - A fast one.\ncorners:\n  - at_m: 410\n    name: Somewhere\n    notes:\n      - Brake early.\n",
        )
        .unwrap();
        assert_eq!(notes.overview, vec!["A fast one."]);
        assert_eq!(notes.corners[0].at_m, 410.0);
        assert_eq!(notes.corners[0].name.as_deref(), Some("Somewhere"));
    }

    #[test]
    fn zandvoort_has_a_plausible_corner_count() {
        let Some(path) = zandvoort() else {
            return;
        };
        let track = TrackLoader::load_from_file(&path).unwrap();
        let corners = detect_corners(&track);
        // The real circuit has 14 numbered turns; chicanes fold into one stop.
        assert!(
            (9..=16).contains(&corners.len()),
            "{} corners: {:?}",
            corners.len(),
            corners
                .iter()
                .map(|c| (c.apex_m as i32, c.direction()))
                .collect::<Vec<_>>()
        );
        let lap = lap_length_m(&track);
        for c in &corners {
            assert!((0.0..lap).contains(&c.apex_m));
            assert!(c.entry_m <= c.apex_m && c.apex_m <= c.exit_m);
        }
    }
}
