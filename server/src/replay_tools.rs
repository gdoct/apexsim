//! Offline replay tools: what the `apexsim-replay` binary is built on.
//!
//! A promotional clip wants a particular moment — the field through Eau
//! Rouge, three cars abreast onto Zandvoort's banking — from a race nobody
//! has to sit through. So a race is *simulated* here, headless and as fast
//! as the machine ticks it ([`simulate_race`]), written as an ordinary
//! replay file; the moment is *found* in it ([`find_windows`]: where the
//! most cars were inside a span of the lap at once); and the file is *cut*
//! to that window ([`cut`]) so the client has a few seconds to load rather
//! than a whole race. The client then plays the cut back from disk at a
//! fixed clock (`-ApexReplay=`), which is what makes a frame dump exact.
//!
//! Everything a script needs to place a camera comes from the same track
//! the sim ran on ([`pose_at`]), so a station quoted from the dossier lands
//! where the cars actually are.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::ai_driver::generate_default_ai_profiles;
use crate::car_loader::CarLoader;
use crate::data::*;
use crate::game_session::GameSession;
use crate::network::ServerMessage;
use crate::replay::{ReplayFrame, ReplayMetadata, ReplayParticipant};
use crate::track_loader::TrackLoader;

/// The race a clip is cut from.
#[derive(Debug, Clone)]
pub struct SimulateOptions {
    /// The track YAML.
    pub track_path: PathBuf,
    /// The `content/cars` folder to draw the field from.
    pub cars_dir: PathBuf,
    /// The host car: a folder name (`yotota-lmp2`), a car id or a name. The
    /// field is that car's class, dealt round-robin as the server deals it.
    pub host_car: String,
    /// Every AI drives the host car instead of the class's mix.
    pub same_car: bool,
    /// Grid size.
    pub ai_count: u8,
    /// Laps to the flag.
    pub laps: u8,
    /// Stop after this much race time even if nobody has finished.
    pub max_seconds: f32,
    /// Seconds of countdown before the lights go out (the grid, revving).
    pub countdown_seconds: u16,
    pub conditions: SessionConditions,
    pub tick_rate: u16,
    /// Frames written per second of race; the sim still ticks at `tick_rate`.
    pub record_hz: u16,
    /// Fixes the AI drivers' ids, and with them the grid order: the same
    /// options and seed give the same race, tick for tick. `None` deals a
    /// fresh grid each run, as the server does.
    pub seed: Option<u64>,
}

impl Default for SimulateOptions {
    fn default() -> Self {
        Self {
            track_path: PathBuf::from("content/tracks/default/Zandvoort.yaml"),
            cars_dir: PathBuf::from("content/cars"),
            host_car: "yotota-lmp2".to_string(),
            same_car: false,
            ai_count: 12,
            laps: 3,
            max_seconds: 600.0,
            countdown_seconds: 3,
            conditions: SessionConditions::DEFAULT,
            tick_rate: 240,
            record_hz: 60,
            seed: None,
        }
    }
}

/// Cars by id.
pub type CarMap = HashMap<CarConfigId, CarConfig>;
/// The `content/cars` folder each car came from, by id.
pub type FolderMap = HashMap<CarConfigId, String>;

/// Every `car.toml` under `cars_dir` (`default/` then `custom/`, the first to
/// claim an id kept, as the server loads them), keyed by id, with the folder
/// each came from so a script can name a car the way the client's catalog does.
pub fn load_car_folder(cars_dir: &Path) -> Result<(CarMap, FolderMap), String> {
    let mut cars = HashMap::new();
    let mut folders = HashMap::new();
    if !cars_dir.is_dir() {
        return Err(format!("cannot read {}", cars_dir.display()));
    }
    for toml in crate::car_loader::car_toml_paths(cars_dir) {
        match CarLoader::load_from_file(&toml) {
            Ok(car) => {
                if cars.contains_key(&car.id) {
                    eprintln!("skipping {}: its id is taken", toml.display());
                    continue;
                }
                folders.insert(
                    car.id,
                    toml.parent()
                        .and_then(|d| d.file_name())
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_default(),
                );
                cars.insert(car.id, car);
            }
            Err(e) => eprintln!("skipping {}: {e}", toml.display()),
        }
    }
    if cars.is_empty() {
        return Err(format!("no car.toml found under {}", cars_dir.display()));
    }
    Ok((cars, folders))
}

/// Find a car by folder, id or name (case-insensitive).
pub fn resolve_car(
    cars: &HashMap<CarConfigId, CarConfig>,
    folders: &HashMap<CarConfigId, String>,
    wanted: &str,
) -> Option<CarConfigId> {
    let wanted = wanted.trim();
    if let Ok(id) = wanted.parse::<CarConfigId>() {
        if cars.contains_key(&id) {
            return Some(id);
        }
    }
    folders
        .iter()
        .find(|(_, folder)| folder.eq_ignore_ascii_case(wanted))
        .map(|(id, _)| *id)
        .or_else(|| {
            cars.values()
                .find(|car| car.name.eq_ignore_ascii_case(wanted))
                .map(|car| car.id)
        })
}

/// A stable id for the `index`th driver of a seeded race (splitmix64).
fn seeded_id(seed: u64, index: u64) -> uuid::Uuid {
    let mut state = seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15);
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

/// Which content a track's YAML is beside: the file stem the client's level
/// and catalog are named by.
pub fn track_stem(track_path: &Path) -> Option<String> {
    track_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
}

/// A race lined up on the grid and counting down, with what a recorder
/// needs to know about it.
pub struct StartedRace {
    pub race: GameSession,
    pub track_name: String,
    pub track_id: TrackConfigId,
    pub track_length_m: f32,
}

/// Load the track and the cars, seat the AI field and start the countdown:
/// the race [`simulate_race`] and [`render_stream`] then tick.
pub fn start_race(opts: &SimulateOptions) -> Result<StartedRace, String> {
    let mut track = TrackLoader::load_from_file(&opts.track_path)
        .map_err(|e| format!("track {}: {e}", opts.track_path.display()))?;
    // The air named in full, the wind's direction from the seed, so a
    // seeded race replays with the same wind.
    let conditions = opts.conditions.resolve(opts.seed.unwrap_or(0));
    conditions.apply_to_track(&mut track);

    let (cars, folders) = load_car_folder(&opts.cars_dir)?;
    let host_car = resolve_car(&cars, &folders, &opts.host_car).ok_or_else(|| {
        format!(
            "no car named '{}' under {}",
            opts.host_car,
            opts.cars_dir.display()
        )
    })?;

    let ai_count = opts.ai_count.max(1);
    let mut profiles = generate_default_ai_profiles(ai_count);
    if let Some(seed) = opts.seed {
        for (i, profile) in profiles.iter_mut().enumerate() {
            profile.id = seeded_id(seed, i as u64);
        }
    }
    if opts.same_car {
        for profile in &mut profiles {
            profile.preferred_car_id = Some(host_car);
        }
    }

    let mut session = RaceSession::new(
        uuid::Uuid::new_v4(),
        track.id,
        SessionKind::Multiplayer,
        ai_count,
        ai_count,
        opts.laps.max(1),
    );
    // A seeded race is the same race down to its id: a stream rendered
    // twice is the same file.
    if let Some(seed) = opts.seed {
        session.id = seeded_id(seed, u64::MAX);
        session.host_player_id = seeded_id(seed, u64::MAX - 1);
    }
    session.host_car_id = Some(host_car);
    session.conditions = conditions;

    let track_length_m = track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0);
    let track_name = track.name.clone();
    let track_id = track.id;

    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(opts.tick_rate);
    race.spawn_ai_drivers();
    if race.session.participants.is_empty() {
        return Err("no AI could be seated: is the grid empty?".to_string());
    }
    race.start_countdown_mode(opts.countdown_seconds.max(1), GameMode::Race);
    Ok(StartedRace {
        race,
        track_name,
        track_id,
        track_length_m,
    })
}

/// Simulate an AI race and return it as a replay (metadata and frames). The
/// recording starts with the countdown, so a cut around
/// `metadata.race_start_tick` is the start.
pub fn simulate_race(opts: &SimulateOptions) -> Result<(ReplayMetadata, Vec<ReplayFrame>), String> {
    let StartedRace {
        mut race,
        track_name,
        track_id,
        track_length_m,
    } = start_race(opts)?;

    let participants: Vec<ReplayParticipant> = race
        .session
        .participants
        .iter()
        .map(|(pid, cs)| ReplayParticipant {
            player_id: *pid,
            player_name: race
                .get_ai_profile(pid)
                .map(|p| p.name.clone())
                .unwrap_or_else(|| "AI".to_string()),
            car_config_id: cs.car_config_id,
            finish_position: None,
            is_ai: true,
            livery: 0,
        })
        .collect();

    let record_every = (opts.tick_rate / opts.record_hz.clamp(1, opts.tick_rate)).max(1) as u32;
    let max_ticks = (opts.max_seconds.max(1.0) * opts.tick_rate as f32) as u32
        + opts.countdown_seconds as u32 * opts.tick_rate as u32;
    // A few seconds past the flag, so the last clip has the winner crossing it.
    let tail_ticks = opts.tick_rate as u32 * 5;

    let mut frames = Vec::new();
    let mut finished_at: Option<u32> = None;
    let mut race_start_tick = None;
    loop {
        let inputs: HashMap<PlayerId, PlayerInputData> = race
            .session
            .participants
            .keys()
            .map(|id| (*id, race.generate_ai_input(id)))
            .collect();
        race.tick(&inputs);
        let tick = race.session.current_tick;
        if race_start_tick.is_none() {
            race_start_tick = race.session.race_start_tick;
        }
        if tick.is_multiple_of(record_every) {
            if let ServerMessage::Telemetry(tel) = race.get_telemetry() {
                frames.push(ReplayFrame {
                    tick,
                    telemetry: tel,
                });
            }
        }
        if race.session.state == SessionState::Finished && finished_at.is_none() {
            finished_at = Some(tick);
        }
        if finished_at.is_some_and(|f| tick >= f + tail_ticks) || tick >= max_ticks {
            break;
        }
    }

    let mut participants = participants;
    for p in &mut participants {
        p.finish_position = race
            .session
            .participants
            .get(&p.player_id)
            .and_then(|c| c.finish_position);
    }

    let metadata = ReplayMetadata {
        session_id: race.session.id,
        track_config_id: track_id,
        track_name,
        recorded_at: std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0),
        duration_ticks: 0,
        tick_rate: opts.tick_rate,
        participants,
        conditions: race.session.conditions,
        race_start_tick,
        track_stem: track_stem(&opts.track_path),
        track_length_m,
    };
    Ok((metadata, frames))
}

/// A stretch of the recording worth pointing a camera at.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Window {
    pub from_tick: u32,
    pub to_tick: u32,
    pub from_s: f32,
    pub to_s: f32,
    /// Cars inside the span at once, at the busiest frame.
    pub peak_cars: u8,
    /// Car-seconds spent inside the span: the ranking.
    pub car_seconds: f32,
    /// The car leading through the span (index into the participants) and
    /// its lap as it entered.
    pub lead_car: u8,
    pub lead_lap: u16,
    /// Tick of the busiest frame: what to centre a short clip on.
    pub peak_tick: u32,
}

/// The span of the lap a window is looked for in, in centerline metres.
/// `from_m > to_m` wraps through the line.
#[derive(Debug, Clone, Copy)]
pub struct Span {
    pub from_m: f32,
    pub to_m: f32,
    pub lap_length_m: f32,
}

impl Span {
    pub fn contains(&self, station_m: f32) -> bool {
        let s = if self.lap_length_m > 0.0 {
            station_m.rem_euclid(self.lap_length_m)
        } else {
            station_m
        };
        if self.from_m <= self.to_m {
            s >= self.from_m && s <= self.to_m
        } else {
            s >= self.from_m || s <= self.to_m
        }
    }

    /// `station ± before/after`, wrapped onto the lap.
    pub fn around(station_m: f32, before_m: f32, after_m: f32, lap_length_m: f32) -> Self {
        let wrap = |s: f32| {
            if lap_length_m > 0.0 {
                s.rem_euclid(lap_length_m)
            } else {
                s
            }
        };
        Self {
            from_m: wrap(station_m - before_m),
            to_m: wrap(station_m + after_m),
            lap_length_m,
        }
    }
}

/// Where the field ran through `span` together. Every maximal run of frames
/// with at least `min_cars` cars inside the span is a window, extended by
/// `pad_s` either side and clipped to the recording; `skip_lap_1` leaves
/// out the first lap, where the grid is still a queue. Sorted busiest first.
pub fn find_windows(
    metadata: &ReplayMetadata,
    frames: &[ReplayFrame],
    span: Span,
    min_cars: u8,
    pad_s: f32,
    skip_lap_1: bool,
) -> Vec<Window> {
    let tick_rate = metadata.tick_rate.max(1) as f32;
    let secs = |tick: u32| tick as f32 / tick_rate;
    let index_of: HashMap<PlayerId, u8> = metadata
        .participants
        .iter()
        .enumerate()
        .map(|(i, p)| (p.player_id, i as u8))
        .collect();
    let first_tick = frames.first().map(|f| f.tick).unwrap_or(0);
    let last_tick = frames.last().map(|f| f.tick).unwrap_or(0);

    let mut windows = Vec::new();
    let mut open: Option<(usize, u8, u32, f32, u8, u16)> = None; // start idx, peak, peak tick, car-ticks, lead, lead lap
    let mut prev_tick = first_tick;
    let mut close = |open: &mut Option<(usize, u8, u32, f32, u8, u16)>, end_tick: u32| {
        if let Some((start_idx, peak, peak_tick, car_ticks, lead, lead_lap)) = open.take() {
            let start_tick = frames[start_idx].tick;
            let pad = (pad_s * tick_rate) as u32;
            let from_tick = start_tick.saturating_sub(pad).max(first_tick);
            let to_tick = (end_tick + pad).min(last_tick);
            windows.push(Window {
                from_tick,
                to_tick,
                from_s: secs(from_tick),
                to_s: secs(to_tick),
                peak_cars: peak,
                car_seconds: car_ticks / tick_rate,
                lead_car: lead,
                lead_lap,
                peak_tick,
            });
        }
    };

    for (i, frame) in frames.iter().enumerate() {
        if frame.telemetry.session_state != SessionState::Racing {
            close(&mut open, prev_tick);
            prev_tick = frame.tick;
            continue;
        }
        let inside: Vec<&crate::network::CarStateTelemetry> = frame
            .telemetry
            .car_states
            .iter()
            .filter(|c| c.finish_position.is_none())
            .filter(|c| !(skip_lap_1 && c.current_lap <= 1))
            .filter(|c| span.contains(c.track_progress))
            .collect();
        let count = inside.len() as u8;
        let dt_ticks = frame.tick.saturating_sub(prev_tick).max(1) as f32;
        if count >= min_cars.max(1) {
            // The lead is the car furthest along the span (wrapped).
            let lead = inside
                .iter()
                .max_by(|a, b| {
                    let key = |c: &crate::network::CarStateTelemetry| {
                        let s = if span.lap_length_m > 0.0 {
                            c.track_progress.rem_euclid(span.lap_length_m)
                        } else {
                            c.track_progress
                        };
                        if span.from_m > span.to_m && s < span.from_m {
                            s + span.lap_length_m
                        } else {
                            s
                        }
                    };
                    key(a)
                        .partial_cmp(&key(b))
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                .copied();
            match &mut open {
                None => {
                    let (lead_idx, lead_lap) = lead
                        .map(|c| {
                            (
                                index_of.get(&c.player_id).copied().unwrap_or(0),
                                c.current_lap,
                            )
                        })
                        .unwrap_or((0, 0));
                    open = Some((
                        i,
                        count,
                        frame.tick,
                        count as f32 * dt_ticks,
                        lead_idx,
                        lead_lap,
                    ));
                }
                Some((_, peak, peak_tick, car_ticks, _, _)) => {
                    *car_ticks += count as f32 * dt_ticks;
                    if count > *peak {
                        *peak = count;
                        *peak_tick = frame.tick;
                    }
                }
            }
        } else {
            close(&mut open, prev_tick);
        }
        prev_tick = frame.tick;
    }
    close(&mut open, last_tick);

    windows.sort_by(|a, b| {
        b.peak_cars.cmp(&a.peak_cars).then(
            b.car_seconds
                .partial_cmp(&a.car_seconds)
                .unwrap_or(std::cmp::Ordering::Equal),
        )
    });
    windows
}

/// The frames between two ticks, inclusive; the metadata unchanged but for
/// the duration [`crate::replay::write_replay_file`] recomputes.
pub fn cut(frames: &[ReplayFrame], from_tick: u32, to_tick: u32) -> Vec<ReplayFrame> {
    frames
        .iter()
        .filter(|f| f.tick >= from_tick && f.tick <= to_tick)
        .cloned()
        .collect()
}

/// What a script reads about a recording.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayInfo {
    pub track_name: String,
    pub track_stem: Option<String>,
    pub track_config_id: String,
    pub track_length_m: f32,
    pub tick_rate: u16,
    pub frames: usize,
    pub first_tick: u32,
    pub last_tick: u32,
    pub duration_s: f32,
    pub race_start_tick: Option<u32>,
    /// First tick the session was `Finished`, if it got there.
    pub finished_tick: Option<u32>,
    pub weather: String,
    pub time_of_day_minutes: u16,
    pub participants: Vec<ParticipantInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ParticipantInfo {
    pub index: u8,
    pub player_id: String,
    pub name: String,
    pub car_config_id: String,
    pub is_ai: bool,
    pub finish_position: Option<u8>,
    /// Tick of each lap counter change, so a script can cut a lap.
    pub lap_ticks: Vec<u32>,
    pub best_lap_ms: Option<u32>,
}

pub fn describe(metadata: &ReplayMetadata, frames: &[ReplayFrame]) -> ReplayInfo {
    let tick_rate = metadata.tick_rate.max(1) as f32;
    let first_tick = frames.first().map(|f| f.tick).unwrap_or(0);
    let last_tick = frames.last().map(|f| f.tick).unwrap_or(0);
    let finished_tick = frames
        .iter()
        .find(|f| f.telemetry.session_state == SessionState::Finished)
        .map(|f| f.tick);
    let participants = metadata
        .participants
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let mut lap_ticks = Vec::new();
            let mut last_lap = None;
            let mut best = None;
            for frame in frames {
                if let Some(c) = frame
                    .telemetry
                    .car_states
                    .iter()
                    .find(|c| c.player_id == p.player_id)
                {
                    if last_lap.is_some_and(|l| c.current_lap > l) {
                        lap_ticks.push(frame.tick);
                    }
                    last_lap = Some(c.current_lap);
                    if let Some(b) = c.best_lap_time_ms {
                        best = Some(best.map_or(b, |x: u32| x.min(b)));
                    }
                }
            }
            ParticipantInfo {
                index: i as u8,
                player_id: p.player_id.to_string(),
                name: p.player_name.clone(),
                car_config_id: p.car_config_id.to_string(),
                is_ai: p.is_ai,
                finish_position: p.finish_position,
                lap_ticks,
                best_lap_ms: best,
            }
        })
        .collect();
    ReplayInfo {
        track_name: metadata.track_name.clone(),
        track_stem: metadata.track_stem.clone(),
        track_config_id: metadata.track_config_id.to_string(),
        track_length_m: metadata.track_length_m,
        tick_rate: metadata.tick_rate,
        frames: frames.len(),
        first_tick,
        last_tick,
        duration_s: last_tick.saturating_sub(first_tick) as f32 / tick_rate,
        race_start_tick: metadata.race_start_tick,
        finished_tick,
        weather: metadata.conditions.weather.name().to_string(),
        time_of_day_minutes: metadata.conditions.time_of_day_minutes,
        participants,
    }
}

/// The clip file the client plays back (`-ApexReplay=<file>.clip.json`).
///
/// JSON rather than the replay's MessagePack because the client would
/// otherwise need a decoder for the named `Telemetry` map, and because a
/// script can read it too. Cars are positional arrays per frame, in the
/// order of `cars`, so the index a frame uses *is* the roster's car index:
///
/// `[x, y, z, yaw, pitch, roll, speed_mps, throttle, brake, steering, gear,
///   rpm, lap, station_m, on_track (0/1), finish_position (0 = none)]`
///
/// Everything is in the server frame (metres, +Y left, yaw counter-clockwise
/// from +X), as on the wire.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipFile {
    pub format: String,
    pub version: u32,
    pub track_name: String,
    pub track_stem: Option<String>,
    pub track_id: String,
    pub track_length_m: f32,
    pub weather: u8,
    pub time_of_day_minutes: u16,
    /// Ticks per second of the tick numbers below.
    pub tick_rate: u16,
    pub race_start_tick: Option<u32>,
    pub cars: Vec<ClipCar>,
    /// The centerline every ~10 m, `[x, y]`: where the TV director's
    /// trackside cameras stand.
    pub centerline: Vec<[f32; 2]>,
    pub frames: Vec<ClipFrame>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipCar {
    pub index: u8,
    pub name: String,
    pub car_config_id: String,
    pub livery: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipFrame {
    pub tick: u32,
    /// `SessionState` as its wire number (1 countdown, 2 racing, 3 finished).
    pub state: u8,
    pub countdown_ms: Option<u16>,
    pub cars: Vec<[f32; 16]>,
}

pub const CLIP_FORMAT: &str = "apexsim-clip";
pub const CLIP_VERSION: u32 = 1;

/// Turn a replay (or a cut of one) into the client's clip file. `centerline`
/// is the track's, decimated to about every `spacing_m`.
pub fn to_clip(
    metadata: &ReplayMetadata,
    frames: &[ReplayFrame],
    centerline: &[TrackPoint],
    spacing_m: f32,
) -> ClipFile {
    let index_of: HashMap<PlayerId, usize> = metadata
        .participants
        .iter()
        .enumerate()
        .map(|(i, p)| (p.player_id, i))
        .collect();
    let mut decimated = Vec::new();
    let mut last = f32::NEG_INFINITY;
    for p in centerline {
        if p.distance_from_start_m - last >= spacing_m {
            decimated.push([p.x, p.y]);
            last = p.distance_from_start_m;
        }
    }
    let clip_frames = frames
        .iter()
        .map(|f| {
            let mut cars = vec![[0.0f32; 16]; metadata.participants.len()];
            for c in &f.telemetry.car_states {
                let Some(&i) = index_of.get(&c.player_id) else {
                    continue;
                };
                cars[i] = [
                    c.pos_x,
                    c.pos_y,
                    c.pos_z,
                    c.yaw_rad,
                    c.pitch_rad,
                    c.roll_rad,
                    c.speed_mps,
                    c.throttle,
                    c.brake,
                    c.steering,
                    c.gear as f32,
                    c.engine_rpm,
                    c.current_lap as f32,
                    c.track_progress,
                    if c.is_on_track { 1.0 } else { 0.0 },
                    c.finish_position.unwrap_or(0) as f32,
                ];
            }
            ClipFrame {
                tick: f.tick,
                state: f.telemetry.session_state as u8,
                countdown_ms: f.telemetry.countdown_ms,
                cars,
            }
        })
        .collect();
    ClipFile {
        format: CLIP_FORMAT.to_string(),
        version: CLIP_VERSION,
        track_name: metadata.track_name.clone(),
        track_stem: metadata.track_stem.clone(),
        track_id: metadata.track_config_id.to_string(),
        track_length_m: metadata.track_length_m,
        weather: metadata.conditions.weather as u8,
        time_of_day_minutes: metadata.conditions.time_of_day_minutes,
        tick_rate: metadata.tick_rate,
        race_start_tick: metadata.race_start_tick,
        cars: metadata
            .participants
            .iter()
            .enumerate()
            .map(|(i, p)| ClipCar {
                index: i as u8,
                name: p.player_name.clone(),
                car_config_id: p.car_config_id.to_string(),
                livery: p.livery,
            })
            .collect(),
        centerline: decimated,
        frames: clip_frames,
    }
}

/// A point beside the road, in the server frame, for a camera.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPose {
    pub station_m: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Road heading at the station, radians counter-clockwise from +X.
    pub yaw_rad: f32,
    pub yaw_deg: f32,
    pub width_left_m: f32,
    pub width_right_m: f32,
    pub lap_length_m: f32,
}

/// Which side of the road a camera stands on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Left,
    Right,
    /// The outside of the bend at the station (left on a straight).
    Outside,
    /// The inside of the bend at the station (right on a straight).
    Inside,
}

impl Side {
    pub fn parse(text: &str) -> Result<Side, String> {
        match text.trim().to_lowercase().as_str() {
            "left" | "l" => Ok(Side::Left),
            "right" | "r" => Ok(Side::Right),
            "outside" | "out" => Ok(Side::Outside),
            "inside" | "in" => Ok(Side::Inside),
            other => Err(format!(
                "unknown side '{other}' (left, right, outside, inside)"
            )),
        }
    }
}

/// +1 when the road turns left through `station_m` (heading change over
/// ±`half_m`), -1 when it turns right, 0 on a straight.
pub fn turn_sign(track: &TrackConfig, station_m: f32, half_m: f32) -> f32 {
    let before = pose_at(track, station_m - half_m, 0.0, 0.0).yaw_rad;
    let after = pose_at(track, station_m + half_m, 0.0, 0.0).yaw_rad;
    let mut delta = after - before;
    while delta > std::f32::consts::PI {
        delta -= std::f32::consts::TAU;
    }
    while delta < -std::f32::consts::PI {
        delta += std::f32::consts::TAU;
    }
    if delta.abs() < 0.05 {
        0.0
    } else {
        delta.signum()
    }
}

/// A lateral distance (always positive) on a side, as a signed lateral
/// (positive left) for [`pose_at`].
pub fn lateral_on(track: &TrackConfig, station_m: f32, distance_m: f32, side: Side) -> f32 {
    let d = distance_m.abs();
    match side {
        Side::Left => d,
        Side::Right => -d,
        // A left-hander's outside is on the right.
        Side::Outside => {
            if turn_sign(track, station_m, 40.0) > 0.0 {
                -d
            } else {
                d
            }
        }
        Side::Inside => {
            if turn_sign(track, station_m, 40.0) > 0.0 {
                d
            } else {
                -d
            }
        }
    }
}

/// [`pose_at`], but lifted off the baked ground (`<Stem>.ground.msgpack`,
/// the terrain the client renders) rather than the road's height where the
/// track has one: a camera 20 m off the road at Eau Rouge would otherwise
/// stand inside the hill or float over the valley.
pub fn pose_on_ground(
    track: &TrackConfig,
    station_m: f32,
    lateral_m: f32,
    height_m: f32,
) -> TrackPose {
    let mut pose = pose_at(track, station_m, lateral_m, height_m);
    if let Some(ground) = &track.ground {
        pose.z = ground.sample(pose.x, pose.y) + height_m;
    }
    pose
}

/// The centerline pose at `station_m`, moved `lateral_m` to the left
/// (negative: right) and lifted `height_m`.
pub fn pose_at(track: &TrackConfig, station_m: f32, lateral_m: f32, height_m: f32) -> TrackPose {
    let lap = track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0);
    let s = if lap > 0.0 {
        station_m.rem_euclid(lap)
    } else {
        station_m
    };
    let (x, y, z, yaw) = crate::physics::pose_at_station(&track.centerline, s);
    let idx = track
        .centerline
        .partition_point(|p| p.distance_from_start_m < s)
        .min(track.centerline.len().saturating_sub(1));
    let p = &track.centerline[idx];
    TrackPose {
        station_m: s,
        x: x - yaw.sin() * lateral_m,
        y: y + yaw.cos() * lateral_m,
        z: z + height_m,
        yaw_rad: yaw,
        yaw_deg: yaw.to_degrees(),
        width_left_m: p.width_left_m,
        width_right_m: p.width_right_m,
        lap_length_m: lap,
    }
}

/// A corner named in the dossier beside the track YAML
/// (`<Stem>.layout.json`), by case-insensitive substring; the first match.
pub fn corner_station(track_path: &Path, name: &str) -> Result<(String, f32), String> {
    let dossier = track_path.with_extension("layout.json");
    let text = std::fs::read_to_string(&dossier)
        .map_err(|e| format!("no dossier {}: {e}", dossier.display()))?;
    let json: serde_json::Value =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", dossier.display()))?;
    let wanted = name.trim().to_lowercase();
    let corners = json
        .get("corners")
        .and_then(|c| c.as_array())
        .ok_or_else(|| format!("{} has no corners", dossier.display()))?;
    corners
        .iter()
        .find_map(|c| {
            let cname = c.get("name")?.as_str()?;
            let station = c.get("station_m")?.as_f64()? as f32;
            cname
                .to_lowercase()
                .contains(&wanted)
                .then(|| (cname.to_string(), station))
        })
        .ok_or_else(|| {
            let names: Vec<&str> = corners
                .iter()
                .filter_map(|c| c.get("name").and_then(|n| n.as_str()))
                .collect();
            format!(
                "no corner matching '{name}'; the dossier names {}",
                names.join(", ")
            )
        })
}

/// A landmark, crossing, stand or structure named in the dossier (by its
/// name or kind, case-insensitive substring), as a point in the server
/// frame: its centre where the dossier has one, else the centerline at its
/// station, lifted `height_m` above the road there (plus a landmark's own
/// altitude, for a blimp). The first of landmarks, crossings, stands and
/// structures to match wins.
pub fn landmark_point(
    track_path: &Path,
    track: &TrackConfig,
    name: &str,
    height_m: f32,
) -> Result<(String, [f32; 3]), String> {
    let dossier = track_path.with_extension("layout.json");
    let text = std::fs::read_to_string(&dossier)
        .map_err(|e| format!("no dossier {}: {e}", dossier.display()))?;
    let json: serde_json::Value =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", dossier.display()))?;
    let wanted = name.trim().to_lowercase();
    let mut seen = Vec::new();
    for layer in ["landmarks", "crossings", "stands", "structures"] {
        let Some(items) = json.get(layer).and_then(|v| v.as_array()) else {
            continue;
        };
        for item in items {
            let label = item
                .get("name")
                .and_then(|n| n.as_str())
                .map(str::to_string);
            let kind = item
                .get("kind")
                .and_then(|k| k.as_str())
                .map(str::to_string);
            let hit = [&label, &kind]
                .iter()
                .filter_map(|v| v.as_deref())
                .any(|v| v.to_lowercase().contains(&wanted));
            if let Some(l) = label.as_ref().or(kind.as_ref()) {
                seen.push(l.clone());
            }
            if !hit {
                continue;
            }
            let Some(station) = item.get("station_m").and_then(|v| v.as_f64()) else {
                continue;
            };
            let road = pose_at(track, station as f32, 0.0, 0.0);
            let (x, y) = match item.get("centre").and_then(|c| c.as_array()) {
                Some(c) if c.len() >= 2 => (
                    c[0].as_f64().unwrap_or(road.x as f64) as f32,
                    c[1].as_f64().unwrap_or(road.y as f64) as f32,
                ),
                _ => (road.x, road.y),
            };
            let altitude = item
                .get("altitude_m")
                .and_then(|v| v.as_f64())
                .unwrap_or(0.0) as f32;
            let label = label.or(kind).unwrap_or_else(|| layer.to_string());
            return Ok((label, [x, y, road.z + altitude + height_m]));
        }
    }
    seen.truncate(40);
    Err(format!(
        "no landmark matching '{name}' in {}; it names {}",
        dossier.display(),
        seen.join(", ")
    ))
}

/// Parse `hh:mm` into minutes after midnight.
pub fn parse_time_of_day(text: &str) -> Result<u16, String> {
    let (h, m) = text
        .split_once(':')
        .ok_or_else(|| format!("time of day '{text}' is not hh:mm"))?;
    let h: u16 = h
        .trim()
        .parse()
        .map_err(|_| format!("bad hour in '{text}'"))?;
    let m: u16 = m
        .trim()
        .parse()
        .map_err(|_| format!("bad minute in '{text}'"))?;
    if h > 23 || m > 59 {
        return Err(format!("time of day '{text}' is off the clock"));
    }
    Ok(h * 60 + m)
}

/// Parse a weather name as the create screen and the scripts spell it.
pub fn parse_weather(text: &str) -> Result<Weather, String> {
    let key: String = text
        .trim()
        .to_lowercase()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .collect();
    match key.as_str() {
        "sunny" | "clear" | "0" => Ok(Weather::Sunny),
        "cloudy" | "1" => Ok(Weather::Cloudy),
        "overcast" | "2" => Ok(Weather::Overcast),
        "lightrain" | "rain" | "3" => Ok(Weather::LightRain),
        "heavyrain" | "storm" | "4" => Ok(Weather::HeavyRain),
        _ => Err(format!(
            "unknown weather '{text}' (sunny, cloudy, overcast, lightrain, heavyrain)"
        )),
    }
}

// --- Spectator streams (`.apxs`) ----------------------------------------------------

use crate::spectator::{
    BroadcastEncoder, EventKind, RaceScore, ScoreKeeper, StreamContent, StreamEvent, StreamFile,
    StreamHeader, StreamPath, StreamRender, StreamRoster, StreamRosterEntry, StreamTrack,
};

/// A showcase race to render (`apexsim-replay render`).
#[derive(Debug, Clone)]
pub struct RenderOptions {
    /// The race itself. `record_hz` is the stream's frame rate, `laps` the
    /// laps from green, `countdown_seconds` how long the grid stands.
    pub race: SimulateOptions,
    /// Seconds kept after the winner takes the flag.
    pub tail_seconds: f32,
    /// Keep only this window of the race, in ticks.
    pub from_tick: Option<u32>,
    pub to_tick: Option<u32>,
}

impl Default for RenderOptions {
    fn default() -> Self {
        Self {
            race: SimulateOptions {
                ai_count: 20,
                laps: 2,
                countdown_seconds: 8,
                record_hz: 30,
                ..SimulateOptions::default()
            },
            tail_seconds: 10.0,
            from_tick: None,
            to_tick: None,
        }
    }
}

/// A rendered race and how it went.
#[derive(Debug, Clone)]
pub struct RenderedStream {
    pub content: StreamContent,
    pub score: RaceScore,
    pub seed: Option<u64>,
}

/// The first car of a class by folder name, for `render --class`.
pub fn car_of_class(cars: &CarMap, folders: &FolderMap, class: &str) -> Option<CarConfigId> {
    let mut matches: Vec<(&str, CarConfigId)> = cars
        .values()
        .filter(|car| car.class.trim().eq_ignore_ascii_case(class.trim()))
        .map(|car| (folders.get(&car.id).map_or("", |f| f.as_str()), car.id))
        .collect();
    matches.sort();
    matches.first().map(|(_, id)| *id)
}

/// Simulate an AI race and return it as a spectator stream: the grid and
/// its countdown, `laps` from green, and a tail after the winner's flag.
/// The same options and seed give the same bytes.
pub fn render_stream(opts: &RenderOptions) -> Result<RenderedStream, String> {
    let race_opts = &opts.race;
    let StartedRace {
        mut race,
        track_length_m,
        ..
    } = start_race(race_opts)?;
    let stem = track_stem(&race_opts.track_path).unwrap_or_default();

    let mut encoder = BroadcastEncoder::new(0);
    let mut header = encoder.header(&race, &stem);
    let roster = encoder.roster(&race);
    let path = encoder.path(&race);
    let sectors = encoder.sectors(&race);
    // The state the stream opens in is not an event.
    encoder.observe(&race, &[]);

    let tick_rate = race_opts.tick_rate.max(1);
    let frame_rate = race_opts.record_hz.clamp(1, tick_rate);
    let record_every = (tick_rate / frame_rate).max(1) as u32;
    let frame_dt = record_every as f32 / tick_rate as f32;
    let max_ticks = (race_opts.max_seconds.max(1.0) * tick_rate as f32) as u32
        + race_opts.countdown_seconds as u32 * tick_rate as u32;
    let tail_ticks = (opts.tail_seconds.max(0.0) * tick_rate as f32) as u32;

    let mut records: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut keeper = ScoreKeeper::default();
    let mut won_at: Option<u32> = None;
    loop {
        let inputs: HashMap<PlayerId, PlayerInputData> = race
            .session
            .participants
            .keys()
            .map(|id| (*id, race.generate_ai_input(id)))
            .collect();
        race.tick(&inputs);
        let tick = race.session.current_tick;
        let laps = race.take_lap_events();
        for event in encoder.observe(&race, &laps) {
            records.push((tick, event.encode()));
        }
        if tick.is_multiple_of(record_every) {
            let rows = BroadcastEncoder::rows(&race);
            keeper.frame(
                &rows,
                race.session.state == SessionState::Racing,
                frame_dt,
                track_length_m,
            );
            for body in encoder.frame_of(&race, &rows) {
                records.push((tick, body));
            }
        }
        if won_at.is_none()
            && (race.session.state == SessionState::Finished
                || race
                    .session
                    .participants
                    .values()
                    .any(|c| c.finish_position == Some(1)))
        {
            won_at = Some(tick);
        }
        if won_at.is_some_and(|w| tick >= w + tail_ticks) || tick >= max_ticks {
            break;
        }
    }

    if opts.from_tick.is_some() || opts.to_tick.is_some() {
        let from = opts.from_tick.unwrap_or(0);
        let to = opts.to_tick.unwrap_or(u32::MAX);
        records.retain(|(tick, _)| *tick >= from && *tick <= to);
    }
    let (Some(first), Some(last)) = (records.first(), records.last()) else {
        return Err("the window holds no frames".to_string());
    };
    header.start_tick = first.0;
    header.end_tick = last.0;
    header.frame_rate = (tick_rate as u32 / record_every) as u16;
    header.race_start_tick = race.session.race_start_tick;
    // What is being watched, not the countdown the stream opens in.
    header.game_mode = race.session.game_mode;
    header.render = StreamRender {
        seed: race_opts.seed,
        score: None,
    };
    Ok(RenderedStream {
        content: StreamContent {
            header,
            roster,
            path: Some(path),
            preamble: vec![sectors],
            records,
        },
        score: keeper.finish(),
        seed: race_opts.seed,
    })
}

/// Render `count` seeds from `opts.race.seed` (0 when unset) upward and
/// keep the race that scores best ([`RaceScore::score`]); its seed and
/// score go into the header, so the pick can be rendered again alone.
pub fn render_best(opts: &RenderOptions, count: u32) -> Result<RenderedStream, String> {
    let first = opts.race.seed.unwrap_or(0);
    let mut best: Option<RenderedStream> = None;
    for seed in first..first + count.max(1) as u64 {
        let mut one = opts.clone();
        one.race.seed = Some(seed);
        let rendered = render_stream(&one)?;
        eprintln!(
            "seed {seed}: score {:.1} ({} retired, {:.1} s contact, {:.1} s off the road, {:.1} s close)",
            rendered.score.score(),
            rendered.score.retirements,
            rendered.score.contact_s,
            rendered.score.off_road_s,
            rendered.score.close_s
        );
        if best
            .as_ref()
            .is_none_or(|b| rendered.score.score() > b.score.score())
        {
            best = Some(rendered);
        }
    }
    let mut best = best.expect("at least one seed is rendered");
    if count > 1 {
        best.content.header.render.score = Some(best.score.score());
    }
    Ok(best)
}

/// A replay (or a cut of one) as a spectator stream at `rate` frames a
/// second. A replay holds no pit state, compound, damage or hybrid, so the
/// rows carry none, and the only events are state changes and finishes.
/// `track` and `cars` fill in the checksums and the path when they are at
/// hand.
pub fn replay_to_stream(
    metadata: &ReplayMetadata,
    frames: &[ReplayFrame],
    rate: u16,
    track: Option<&TrackConfig>,
    cars: Option<&CarMap>,
) -> Result<StreamContent, String> {
    let (Some(first), Some(last)) = (frames.first(), frames.last()) else {
        return Err("the replay holds no frames".to_string());
    };
    let tick_rate = metadata.tick_rate.max(1);
    let every = (tick_rate / rate.clamp(1, tick_rate)).max(1) as u32;
    let index_of: HashMap<PlayerId, u8> = metadata
        .participants
        .iter()
        .enumerate()
        .map(|(i, p)| (p.player_id, i as u8))
        .collect();

    let header = StreamHeader {
        epoch: 0,
        version: crate::spectator::FORMAT_VERSION,
        stream_id: metadata.session_id,
        tick_rate,
        frame_rate: (tick_rate as u32 / every) as u16,
        row_size: crate::spectator::ROW_SIZE as u8,
        track: StreamTrack {
            track_id: metadata.track_config_id,
            stem: metadata.track_stem.clone().unwrap_or_default(),
            display_name: metadata.track_name.clone(),
            source_crc: track.map_or(0, |t| t.content_crc),
            length_m: metadata.track_length_m,
        },
        conditions: metadata.conditions,
        session_kind: SessionKind::Multiplayer,
        game_mode: first.telemetry.game_mode,
        lap_limit: 0,
        race_start_tick: metadata.race_start_tick,
        start_tick: first.tick,
        end_tick: last.tick,
        render: StreamRender::default(),
    };
    let roster = StreamRoster {
        epoch: 0,
        revision: 0,
        entries: metadata
            .participants
            .iter()
            .enumerate()
            .map(|(i, p)| StreamRosterEntry {
                car_index: i as u8,
                car_config_id: p.car_config_id,
                content_crc: cars
                    .and_then(|c| c.get(&p.car_config_id))
                    .map_or(0, |c| c.content_crc),
                livery: p.livery,
                name: p.player_name.clone(),
                is_ai: p.is_ai,
            })
            .collect(),
    };

    let mut records = Vec::new();
    let mut state = first.telemetry.session_state;
    let mut finished = vec![false; metadata.participants.len()];
    let mut next_frame_tick = first.tick;
    for frame in frames {
        let tick = frame.tick;
        if frame.telemetry.session_state != state {
            state = frame.telemetry.session_state;
            records.push((
                tick,
                StreamEvent {
                    epoch: 0,
                    tick,
                    kind: EventKind::SessionState(state),
                }
                .encode(),
            ));
        }
        let mut rows = Vec::with_capacity(frame.telemetry.car_states.len());
        for car in &frame.telemetry.car_states {
            let Some(&index) = index_of.get(&car.player_id) else {
                continue;
            };
            if let Some(position) = car.finish_position {
                if !std::mem::replace(&mut finished[index as usize], true) {
                    records.push((
                        tick,
                        StreamEvent {
                            epoch: 0,
                            tick,
                            kind: EventKind::Finish {
                                car_index: index,
                                position,
                            },
                        }
                        .encode(),
                    ));
                }
            }
            rows.push(crate::spectator::CarRow::from_replay(car, index));
        }
        if tick < next_frame_tick {
            continue;
        }
        next_frame_tick = tick + every;
        rows.sort_by_key(|r| r.car_index);
        for body in crate::spectator::encode_frames(
            0,
            tick,
            0,
            frame.telemetry.session_state,
            frame.telemetry.countdown_ms,
            &rows,
        ) {
            records.push((tick, body));
        }
    }

    Ok(StreamContent {
        header,
        roster,
        path: track.map(|t| StreamPath::from_centerline(&t.centerline, 0)),
        preamble: track
            .map(|t| {
                vec![StreamEvent {
                    epoch: 0,
                    tick: first.tick,
                    kind: EventKind::TrackSectors {
                        track_length_m: crate::laps::track_length_m(t),
                        boundaries_m: crate::laps::sector_boundaries_m(t),
                    },
                }]
            })
            .unwrap_or_default(),
        records,
    })
}

/// What `apexsim-replay info` prints about a stream file.
#[derive(Debug, Clone, Serialize)]
pub struct StreamInfo {
    pub format: &'static str,
    pub version: u8,
    pub stream_id: String,
    pub track_stem: String,
    pub track_name: String,
    pub track_id: String,
    pub track_source_crc: u32,
    pub track_length_m: f32,
    pub weather: String,
    pub time_of_day_minutes: u16,
    pub air_temp_c: Option<i8>,
    pub humidity_pct: Option<u8>,
    pub wind_kph: Option<u8>,
    pub wind_from_deg: Option<u16>,
    pub lap_limit: u8,
    pub tick_rate: u16,
    pub frame_rate: u16,
    pub row_size: u8,
    pub start_tick: u32,
    pub end_tick: u32,
    pub race_start_tick: Option<u32>,
    pub duration_s: f32,
    pub seed: Option<u64>,
    pub score: Option<f32>,
    pub cars: Vec<StreamCarInfo>,
    pub has_path: bool,
    pub blocks: usize,
    pub frames: usize,
    pub events: usize,
    pub file_bytes: usize,
    /// Bytes of the records once inflated.
    pub raw_bytes: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct StreamCarInfo {
    pub index: u8,
    pub name: String,
    pub car_config_id: String,
    pub content_crc: u32,
    pub livery: u8,
    pub is_ai: bool,
}

pub fn describe_stream(file: &StreamFile) -> Result<StreamInfo, String> {
    let records = file.records().map_err(|e| e.to_string())?;
    let kind = |k: u8| {
        records
            .iter()
            .filter(|b| crate::spectator::record_type(b) == Some(k))
            .count()
    };
    let h = &file.header;
    Ok(StreamInfo {
        format: "apxs",
        version: h.version,
        stream_id: h.stream_id.to_string(),
        track_stem: h.track.stem.clone(),
        track_name: h.track.display_name.clone(),
        track_id: h.track.track_id.to_string(),
        track_source_crc: h.track.source_crc,
        track_length_m: h.track.length_m,
        weather: h.conditions.weather.name().to_string(),
        time_of_day_minutes: h.conditions.time_of_day_minutes,
        air_temp_c: h.conditions.air_temp_c,
        humidity_pct: h.conditions.humidity_pct,
        wind_kph: h.conditions.wind_kph,
        wind_from_deg: h.conditions.wind_from_deg,
        lap_limit: h.lap_limit,
        tick_rate: h.tick_rate,
        frame_rate: h.frame_rate,
        row_size: h.row_size,
        start_tick: h.start_tick,
        end_tick: h.end_tick,
        race_start_tick: h.race_start_tick,
        duration_s: h.duration_s(),
        seed: h.render.seed,
        score: h.render.score,
        cars: file
            .roster
            .entries
            .iter()
            .map(|e| StreamCarInfo {
                index: e.car_index,
                name: e.name.clone(),
                car_config_id: e.car_config_id.to_string(),
                content_crc: e.content_crc,
                livery: e.livery,
                is_ai: e.is_ai,
            })
            .collect(),
        has_path: file.path.is_some(),
        blocks: file.block_count(),
        frames: kind(crate::spectator::RECORD_FRAME),
        events: kind(crate::spectator::RECORD_EVENT),
        file_bytes: file.file_len(),
        raw_bytes: records.iter().map(|b| b.len() + 4).sum(),
    })
}

/// The track YAML a stream's stem names, under `tracks_dir`'s `default/`
/// and `custom/` (or `tracks_dir` itself when it has neither).
pub fn find_track_yaml(tracks_dir: &Path, stem: &str) -> Option<PathBuf> {
    ["default", "custom", ""]
        .iter()
        .map(|sub| tracks_dir.join(sub).join(format!("{stem}.yaml")))
        .find(|path| path.is_file())
}

/// Where a stream no longer matches the content on disk: the track YAML's
/// checksum, and every car's. Empty when the file is fresh. A checksum of 0
/// in the file (a converted replay) is not compared.
pub fn check_stream(
    header: &StreamHeader,
    roster: &StreamRoster,
    tracks_dir: &Path,
    cars_dir: &Path,
) -> Vec<String> {
    let mut stale = Vec::new();
    // Rows from before a field was appended play, but without it (a stream
    // from before the tyres has no tyres on its timing tower).
    if (header.row_size as usize) < crate::spectator::ROW_SIZE {
        stale.push(format!(
            "rows of {} bytes, from before this build's {}: render it again for the fields since",
            header.row_size,
            crate::spectator::ROW_SIZE
        ));
    }
    match find_track_yaml(tracks_dir, &header.track.stem) {
        None => stale.push(format!(
            "track {}: no {}.yaml under {}",
            header.track.display_name,
            header.track.stem,
            tracks_dir.display()
        )),
        Some(path) => match std::fs::read(&path) {
            Ok(bytes) => {
                let crc = crate::content_crc::content_crc(&bytes);
                if header.track.source_crc != 0 && crc != header.track.source_crc {
                    stale.push(format!(
                        "track {}: the stream was raced on {:08x}, {} is {:08x}",
                        header.track.stem,
                        header.track.source_crc,
                        path.display(),
                        crc
                    ));
                }
            }
            Err(e) => stale.push(format!("track {}: {e}", path.display())),
        },
    }
    match load_car_folder(cars_dir) {
        Err(e) => stale.push(format!("cars: {e}")),
        Ok((cars, _)) => {
            let mut seen = std::collections::BTreeSet::new();
            for entry in &roster.entries {
                if !seen.insert(entry.car_config_id) {
                    continue;
                }
                match cars.get(&entry.car_config_id) {
                    None => stale.push(format!(
                        "car {}: not under {}",
                        entry.car_config_id,
                        cars_dir.display()
                    )),
                    Some(car) if entry.content_crc != 0 && car.content_crc != entry.content_crc => {
                        stale.push(format!(
                            "car {}: the stream was raced on {:08x}, its car.toml is {:08x}",
                            car.name, entry.content_crc, car.content_crc
                        ));
                    }
                    Some(_) => {}
                }
            }
        }
    }
    stale
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::network::{CarStateTelemetry, Telemetry};

    fn short_render() -> RenderOptions {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        RenderOptions {
            race: SimulateOptions {
                track_path: root.join("content/tracks/default/Zandvoort.yaml"),
                cars_dir: root.join("content/cars/default"),
                host_car: "yotota-lmp2".into(),
                same_car: false,
                ai_count: 3,
                laps: 1,
                max_seconds: 12.0,
                countdown_seconds: 1,
                conditions: SessionConditions {
                    weather: Weather::Overcast,
                    time_of_day_minutes: 19 * 60,
                    ..SessionConditions::DEFAULT
                },
                tick_rate: 240,
                record_hz: 30,
                seed: Some(11),
            },
            tail_seconds: 2.0,
            from_tick: None,
            to_tick: None,
        }
    }

    /// The determinism the pipeline's "only what is stale" rests on: the
    /// same arguments and seed write the same file, byte for byte.
    #[test]
    fn a_seeded_render_is_byte_identical() {
        use crate::spectator::{Record, StreamFile, RECORD_FRAME};
        let opts = short_render();
        let first = render_stream(&opts).expect("renders");
        let bytes = first.content.to_bytes().unwrap();
        let again = render_stream(&opts).expect("renders again");
        assert!(
            bytes == again.content.to_bytes().unwrap(),
            "a seeded render must write the same file twice"
        );
        // Another seed is another grid.
        let mut other = opts.clone();
        other.race.seed = Some(12);
        assert!(bytes != render_stream(&other).unwrap().content.to_bytes().unwrap());

        let file = StreamFile::from_bytes(bytes).unwrap();
        let h = &file.header;
        assert_eq!(h.track.stem, "Zandvoort");
        assert_ne!(h.track.source_crc, 0);
        assert!(h.track.length_m > 4000.0);
        assert_eq!((h.tick_rate, h.frame_rate), (240, 30));
        assert_eq!(h.conditions.weather, Weather::Overcast);
        // Resolved: every figure the AI raced in is named.
        assert!(h.conditions.air_temp_c.is_some() && h.conditions.wind_from_deg.is_some());
        assert_eq!(h.render.seed, Some(11));
        assert_eq!(h.game_mode, GameMode::Race);
        assert!(h.race_start_tick.is_some_and(|t| t > 200 && t < 300));
        assert_eq!(file.roster.entries.len(), 3);
        assert!(file
            .roster
            .entries
            .iter()
            .all(|e| e.is_ai && e.content_crc != 0));
        assert!(file.path.as_ref().is_some_and(|p| p.points.len() > 300));
        assert!(matches!(
            file.preamble.first().map(|e| &e.kind),
            Some(EventKind::TrackSectors { boundaries_m, .. }) if boundaries_m.len() == 2
        ));

        let records = file.records().unwrap();
        let frames: Vec<_> = records
            .iter()
            .filter(|b| crate::spectator::record_type(b) == Some(RECORD_FRAME))
            .map(|b| match Record::decode(b).unwrap() {
                Record::Frame(f) => f,
                _ => unreachable!(),
            })
            .collect();
        assert!(frames.len() > 350, "{}", frames.len());
        assert!(frames.windows(2).all(|w| w[1].tick == w[0].tick + 8));
        assert_eq!(frames[0].state, SessionState::Countdown);
        assert!(frames[0].countdown_ms < 1000);
        let last = frames.last().unwrap();
        assert_eq!(last.state, SessionState::Racing);
        assert_eq!(last.tick, h.end_tick);
        assert!(last
            .cars(h.row_size as usize)
            .all(|car| car.station_m() > 100.0 && car.speed_mps() > 10.0));
        // The lights going out is the first thing that happens.
        let first_event = records
            .iter()
            .find_map(|b| match Record::decode(b).unwrap() {
                Record::Event(e) => Some(e),
                _ => None,
            })
            .unwrap();
        assert_eq!(
            first_event.kind,
            EventKind::SessionState(SessionState::Racing)
        );
        assert_eq!(Some(first_event.tick), h.race_start_tick);

        // Fresh against the content it was rendered from, stale once that
        // content's checksum has moved.
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let (tracks, cars) = (
            root.join("content/tracks"),
            root.join("content/cars/default"),
        );
        assert_eq!(
            check_stream(&file.header, &file.roster, &tracks, &cars),
            Vec::<String>::new()
        );
        let mut moved = file.header.clone();
        moved.track.source_crc ^= 1;
        let mut roster = file.roster.clone();
        roster.entries[0].content_crc ^= 1;
        let stale = check_stream(&moved, &roster, &tracks, &cars);
        assert_eq!(stale.len(), 2, "{stale:?}");
        moved.track.stem = "Nowhere".into();
        assert!(check_stream(&moved, &file.roster, &tracks, &cars)[0].contains("no Nowhere.yaml"));
        // A stream from before the tyres were in a row is rendered again.
        let mut old = file.header.clone();
        old.row_size = crate::spectator::ROW_SIZE_V1 as u8;
        let stale = check_stream(&old, &file.roster, &tracks, &cars);
        assert!(
            stale.len() == 1 && stale[0].contains("rows of 44 bytes"),
            "{stale:?}"
        );

        let info = describe_stream(&file).unwrap();
        assert_eq!(info.frames, frames.len());
        assert_eq!(info.cars.len(), 3);
        assert!(info.file_bytes < info.raw_bytes);
    }

    #[test]
    fn a_window_and_the_best_seed_are_kept() {
        let mut opts = short_render();
        opts.race.max_seconds = 6.0;
        opts.from_tick = Some(480);
        opts.to_tick = Some(960);
        let cut = render_stream(&opts).unwrap();
        assert_eq!(cut.content.header.start_tick, 480);
        assert_eq!(cut.content.header.end_tick, 960);
        assert!(cut
            .content
            .records
            .iter()
            .all(|(tick, _)| (480..=960).contains(tick)));

        opts.from_tick = None;
        opts.to_tick = None;
        let best = render_best(&opts, 2).unwrap();
        assert!(best.seed == Some(11) || best.seed == Some(12));
        assert_eq!(best.content.header.render.seed, best.seed);
        assert_eq!(best.content.header.render.score, Some(best.score.score()));

        let (cars, folders) =
            load_car_folder(&Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/cars/default"))
                .unwrap();
        let gt3 = car_of_class(&cars, &folders, "gt3").expect("a GT3 car ships");
        assert_eq!(cars[&gt3].class, "GT3");
        assert!(car_of_class(&cars, &folders, "karts").is_none());
    }

    #[test]
    fn a_replay_converts_to_a_stream() {
        use crate::spectator::{Record, StreamFile};
        let ids = [uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2)];
        let mut frames = cut(&two_car_lap(&ids), 0, 960);
        // The second car takes the flag part way through.
        for f in frames.iter_mut().filter(|f| f.tick >= 480) {
            f.telemetry.car_states[1].finish_position = Some(1);
        }
        let content = replay_to_stream(&metadata(&ids), &frames, 30, None, None).unwrap();
        assert_eq!(content.header.frame_rate, 30);
        assert_eq!(
            (content.header.start_tick, content.header.end_tick),
            (0, 960)
        );
        assert!(content.path.is_none() && content.preamble.is_empty());
        let file = StreamFile::from_bytes(content.to_bytes().unwrap()).unwrap();
        assert_eq!(file.roster.entries[1].name, "AI 1");
        let records: Vec<Record> = file
            .records()
            .unwrap()
            .iter()
            .map(|b| Record::decode(b).unwrap())
            .collect();
        let frame_ticks: Vec<u32> = records
            .iter()
            .filter_map(|r| match r {
                Record::Frame(f) => Some(f.tick),
                _ => None,
            })
            .collect();
        // 60 Hz in, 30 Hz out.
        assert_eq!(frame_ticks.len(), 121);
        assert!(frame_ticks.windows(2).all(|w| w[1] == w[0] + 8));
        assert!(records.iter().any(|r| matches!(
            r,
            Record::Event(e) if e.tick == 480 && e.kind == EventKind::Finish { car_index: 1, position: 1 }
        )));
        let Some(Record::Frame(frame)) = records.iter().find(|r| matches!(r, Record::Frame(_)))
        else {
            panic!("no frame");
        };
        let cars: Vec<_> = frame.cars(file.header.row_size as usize).collect();
        assert_eq!(cars.len(), 2);
        assert_eq!(cars[0].compound, crate::network::COMPOUND_UNKNOWN);
        assert_eq!(cars[0].gear, 4);
    }

    fn car(id: PlayerId, progress: f32, lap: u16) -> CarStateTelemetry {
        CarStateTelemetry {
            player_id: id,
            pos_x: 0.0,
            pos_y: 0.0,
            pos_z: 0.0,
            yaw_rad: 0.0,
            pitch_rad: 0.0,
            roll_rad: 0.0,
            speed_mps: 50.0,
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: 4,
            engine_rpm: 7000.0,
            suspension: SuspensionTelemetry::default(),
            current_lap: lap,
            track_progress: progress,
            finish_position: None,
            current_lap_time_ms: 0,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            lap_flags: 0,
            is_on_track: true,
            is_colliding: false,
        }
    }

    fn metadata(ids: &[PlayerId]) -> ReplayMetadata {
        ReplayMetadata {
            session_id: uuid::Uuid::nil(),
            track_config_id: uuid::Uuid::nil(),
            track_name: "Ring".into(),
            recorded_at: 0,
            duration_ticks: 0,
            tick_rate: 240,
            participants: ids
                .iter()
                .enumerate()
                .map(|(i, id)| ReplayParticipant {
                    player_id: *id,
                    player_name: format!("AI {i}"),
                    car_config_id: uuid::Uuid::nil(),
                    finish_position: None,
                    is_ai: true,
                    livery: 0,
                })
                .collect(),
            conditions: SessionConditions::DEFAULT,
            race_start_tick: Some(0),
            track_stem: Some("Ring".into()),
            track_length_m: 1000.0,
        }
    }

    /// Two cars 20 m apart at 50 m/s round a 1 km lap, frames at 60 Hz.
    fn two_car_lap(ids: &[PlayerId]) -> Vec<ReplayFrame> {
        (0..(60 * 40))
            .map(|i| {
                let t = i as f32 / 60.0;
                let s0 = 50.0 * t;
                ReplayFrame {
                    tick: i * 4,
                    telemetry: Telemetry {
                        server_tick: i * 4,
                        session_state: SessionState::Racing,
                        game_mode: GameMode::Race,
                        countdown_ms: None,
                        car_states: vec![
                            car(ids[0], s0 % 1000.0, 1 + (s0 / 1000.0) as u16),
                            car(
                                ids[1],
                                (s0 - 20.0).max(0.0) % 1000.0,
                                1 + ((s0 - 20.0).max(0.0) / 1000.0) as u16,
                            ),
                        ],
                    },
                }
            })
            .collect()
    }

    #[test]
    fn span_wraps_through_the_line() {
        let span = Span {
            from_m: 900.0,
            to_m: 100.0,
            lap_length_m: 1000.0,
        };
        assert!(span.contains(950.0));
        assert!(span.contains(50.0));
        assert!(span.contains(1050.0));
        assert!(!span.contains(500.0));
        let plain = Span::around(500.0, 100.0, 50.0, 1000.0);
        assert_eq!((plain.from_m, plain.to_m), (400.0, 550.0));
        let wrapped = Span::around(30.0, 100.0, 50.0, 1000.0);
        assert_eq!((wrapped.from_m, wrapped.to_m), (930.0, 80.0));
    }

    #[test]
    fn windows_find_both_cars_in_the_span_each_lap() {
        let ids = [uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2)];
        let meta = metadata(&ids);
        let frames = two_car_lap(&ids);
        // Lap 2 only (skip lap 1): the cars pass 400..500 m once more at
        // t = 28 s .. 30.4 s (the second car 0.4 s behind).
        let windows = find_windows(
            &meta,
            &frames,
            Span {
                from_m: 400.0,
                to_m: 500.0,
                lap_length_m: 1000.0,
            },
            2,
            0.0,
            true,
        );
        assert_eq!(windows.len(), 1, "{windows:?}");
        let w = &windows[0];
        assert_eq!(w.peak_cars, 2);
        assert_eq!(w.lead_car, 0);
        assert_eq!(w.lead_lap, 2);
        assert!((w.from_s - 28.4).abs() < 0.1, "{w:?}");
        assert!((w.to_s - 30.0).abs() < 0.1, "{w:?}");
        // With lap 1 allowed there are two passes, busiest first, and the
        // padding stretches each.
        let all = find_windows(
            &meta,
            &frames,
            Span {
                from_m: 400.0,
                to_m: 500.0,
                lap_length_m: 1000.0,
            },
            1,
            1.0,
            false,
        );
        assert_eq!(all.len(), 2);
        assert!(all[0].to_s - all[0].from_s > 3.5);
        assert!(all.iter().all(|w| w.car_seconds > 0.0));
    }

    #[test]
    fn cut_keeps_the_ticks_inside_the_window() {
        let ids = [uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2)];
        let frames = two_car_lap(&ids);
        let part = cut(&frames, 100, 200);
        assert!(part.iter().all(|f| (100..=200).contains(&f.tick)));
        assert_eq!(part.len(), 26);
        let info = describe(&metadata(&ids), &frames);
        assert_eq!(info.participants.len(), 2);
        assert_eq!(
            info.participants[0].lap_ticks.len(),
            1,
            "one lap change in 40 s"
        );
        assert!((info.duration_s - 39.98).abs() < 0.1);
    }

    #[test]
    fn replay_round_trips_through_a_file() {
        let ids = [uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2)];
        let dir = tempfile::TempDir::new().unwrap();
        let path = dir.path().join("clip.bin");
        let frames = cut(&two_car_lap(&ids), 0, 400);
        crate::replay::write_replay_file(&path, metadata(&ids), &frames).unwrap();
        let (meta, back) = crate::replay::read_replay_file(&path).unwrap();
        assert_eq!(back.len(), frames.len());
        assert_eq!(meta.track_stem.as_deref(), Some("Ring"));
        assert_eq!(meta.duration_ticks, 400);
        assert_eq!(meta.participants[1].player_name, "AI 1");
        assert_eq!(
            back[3].telemetry.car_states[0].track_progress,
            frames[3].telemetry.car_states[0].track_progress
        );
    }

    #[test]
    fn clip_puts_each_car_at_its_roster_index() {
        let ids = [uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2)];
        let mut frames = cut(&two_car_lap(&ids), 0, 40);
        // The wire order need not be the roster's.
        for f in &mut frames {
            f.telemetry.car_states.reverse();
        }
        let centerline: Vec<TrackPoint> = (0..100)
            .map(|i| TrackPoint {
                x: i as f32 * 2.0,
                distance_from_start_m: i as f32 * 2.0,
                ..TrackPoint::default()
            })
            .collect();
        let clip = to_clip(&metadata(&ids), &frames, &centerline, 10.0);
        assert_eq!(clip.cars.len(), 2);
        assert_eq!(clip.centerline.len(), 20);
        assert_eq!(clip.frames.len(), frames.len());
        let f = &clip.frames[5];
        let wire = &frames[5].telemetry.car_states;
        assert_eq!(
            f.cars[0][13],
            wire.iter()
                .find(|c| c.player_id == ids[0])
                .unwrap()
                .track_progress
        );
        assert_eq!(
            f.cars[1][13],
            wire.iter()
                .find(|c| c.player_id == ids[1])
                .unwrap()
                .track_progress
        );
        assert_eq!(f.state, SessionState::Racing as u8);
        let json = serde_json::to_string(&clip).unwrap();
        let back: ClipFile = serde_json::from_str(&json).unwrap();
        assert_eq!(back.format, CLIP_FORMAT);
    }

    #[test]
    fn parsers_take_what_the_scripts_write() {
        assert_eq!(parse_time_of_day("21:30").unwrap(), 21 * 60 + 30);
        assert!(parse_time_of_day("25:00").is_err());
        assert_eq!(parse_weather("light rain").unwrap(), Weather::LightRain);
        assert_eq!(parse_weather("HeavyRain").unwrap(), Weather::HeavyRain);
        assert!(parse_weather("snow").is_err());
    }

    #[test]
    fn pose_stands_beside_the_road() {
        let track = TrackLoader::load_from_file(
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/default/Spa.yaml"),
        )
        .expect("Spa loads");
        let on = pose_at(&track, 1056.0, 0.0, 0.0);
        let left = pose_at(&track, 1056.0, 10.0, 3.0);
        let d = ((left.x - on.x).powi(2) + (left.y - on.y).powi(2)).sqrt();
        assert!((d - 10.0).abs() < 0.05, "{d}");
        assert!((left.z - on.z - 3.0).abs() < 1e-3);
        // A station past the lap wraps.
        let wrapped = pose_at(&track, on.lap_length_m + 1056.0, 0.0, 0.0);
        assert!((wrapped.x - on.x).abs() < 1e-2);
        let (name, station) = corner_station(
            &Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/default/Spa.yaml"),
            "eau rouge",
        )
        .unwrap();
        assert_eq!(name, "Eau Rouge");
        assert!((station - 1056.7).abs() < 0.1);
    }

    #[test]
    fn sides_follow_the_bend() {
        let path =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/default/Zandvoort.yaml");
        let track = TrackLoader::load_from_file(&path).expect("Zandvoort loads");
        // Tarzanbocht is a right-hander: its outside is on the left.
        let (_, tarzan) = corner_station(&path, "Tarzan").unwrap();
        assert!(turn_sign(&track, tarzan, 40.0) < 0.0);
        assert_eq!(lateral_on(&track, tarzan, 20.0, Side::Outside), 20.0);
        assert_eq!(lateral_on(&track, tarzan, 20.0, Side::Inside), -20.0);
        assert_eq!(lateral_on(&track, tarzan, -20.0, Side::Right), -20.0);
        assert!(Side::parse("sideways").is_err());
        // Without a baked ground the pose is the road's height.
        let a = pose_on_ground(&track, tarzan, 20.0, 3.0);
        let b = pose_at(&track, tarzan, 20.0, 3.0);
        if track.ground.is_none() {
            assert_eq!(a.z, b.z);
        }
    }

    #[test]
    fn landmarks_are_found_by_kind_or_name() {
        let path =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/default/LeMans.yaml");
        let track = TrackLoader::load_from_file(&path).expect("Le Mans loads");
        let (label, wheel) = landmark_point(&path, &track, "big_wheel", 20.0).unwrap();
        assert_eq!(label, "big_wheel");
        assert!((wheel[0] - 777.7).abs() < 0.1 && (wheel[1] + 382.13).abs() < 0.1);
        let road = pose_at(&track, 1010.0, 0.0, 0.0);
        assert!((wheel[2] - road.z - 20.0).abs() < 1e-3);
        let (label, bridge) = landmark_point(&path, &track, "tyre bridge", 5.0).unwrap();
        assert_eq!(label, "Tyre bridge");
        let at = pose_at(&track, 1043.0, 0.0, 5.0);
        assert!((bridge[0] - at.x).abs() < 1e-3 && (bridge[2] - at.z).abs() < 1e-3);
        assert!(landmark_point(&path, &track, "no such thing", 0.0).is_err());
    }

    /// The whole thing, on a real circuit: a short race, a window at a
    /// named corner, a cut that plays back. Two cars for a few laps.
    #[test]
    fn a_short_race_simulates_and_cuts() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let opts = SimulateOptions {
            track_path: root.join("content/tracks/default/Zandvoort.yaml"),
            cars_dir: root.join("content/cars"),
            host_car: "yotota-lmp2".into(),
            same_car: true,
            ai_count: 2,
            laps: 1,
            max_seconds: 20.0,
            countdown_seconds: 1,
            conditions: SessionConditions {
                weather: Weather::LightRain,
                time_of_day_minutes: 20 * 60,
                ..SessionConditions::DEFAULT
            },
            tick_rate: 240,
            record_hz: 30,
            seed: Some(7),
        };
        let (meta, frames) = simulate_race(&opts).expect("race simulates");
        // A seed makes the race repeat exactly.
        let (again_meta, again) = simulate_race(&opts).expect("race simulates again");
        assert_eq!(
            meta.participants
                .iter()
                .map(|p| p.player_id)
                .collect::<Vec<_>>(),
            again_meta
                .participants
                .iter()
                .map(|p| p.player_id)
                .collect::<Vec<_>>()
        );
        let last = |f: &[ReplayFrame]| {
            f.last()
                .unwrap()
                .telemetry
                .car_states
                .iter()
                .map(|c| (c.pos_x.to_bits(), c.pos_y.to_bits()))
                .collect::<Vec<_>>()
        };
        assert_eq!(
            last(&frames),
            last(&again),
            "a seeded race must replay bit for bit"
        );
        assert_eq!(meta.participants.len(), 2);
        assert!(meta.participants.iter().all(|p| p.is_ai));
        assert_eq!(meta.track_stem.as_deref(), Some("Zandvoort"));
        assert_eq!(meta.conditions.weather, Weather::LightRain);
        assert!(meta.race_start_tick.is_some());
        assert!(meta.track_length_m > 4000.0);
        // 30 Hz over ~21 s.
        assert!(frames.len() > 500 && frames.len() < 700, "{}", frames.len());
        assert!(frames.windows(2).all(|w| w[1].tick == w[0].tick + 8));
        let moved = frames
            .last()
            .unwrap()
            .telemetry
            .car_states
            .iter()
            .all(|c| c.track_progress > 200.0);
        assert!(moved, "the field should be well up the road after 20 s");
        let span = Span::around(300.0, 100.0, 100.0, meta.track_length_m);
        let windows = find_windows(&meta, &frames, span, 1, 0.5, false);
        assert!(!windows.is_empty());
        let w = &windows[0];
        let clip = cut(&frames, w.from_tick, w.to_tick);
        assert!(!clip.is_empty());
        assert!(clip.first().unwrap().tick >= w.from_tick);
        assert!(clip.last().unwrap().tick <= w.to_tick);
        let info = describe(&meta, &frames);
        assert_eq!(info.weather, "light rain");
        assert_eq!(info.time_of_day_minutes, 1200);
    }
}
