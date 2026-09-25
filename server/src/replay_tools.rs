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
            track_path: PathBuf::from("content/tracks/real/Zandvoort.yaml"),
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

/// Every `car.toml` under `cars_dir`, keyed by id, with the folder each came
/// from so a script can name a car the way the client's catalog does.
pub fn load_car_folder(cars_dir: &Path) -> Result<(CarMap, FolderMap), String> {
    let mut cars = HashMap::new();
    let mut folders = HashMap::new();
    let entries = std::fs::read_dir(cars_dir)
        .map_err(|e| format!("cannot read {}: {e}", cars_dir.display()))?;
    let mut paths: Vec<PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.join("car.toml").is_file())
        .collect();
    paths.sort();
    for dir in paths {
        let toml = dir.join("car.toml");
        match CarLoader::load_from_file(&toml) {
            Ok(car) => {
                folders.insert(
                    car.id,
                    dir.file_name()
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

/// Simulate an AI race and return it as a replay (metadata and frames). The
/// recording starts with the countdown, so a cut around
/// `metadata.race_start_tick` is the start.
pub fn simulate_race(opts: &SimulateOptions) -> Result<(ReplayMetadata, Vec<ReplayFrame>), String> {
    let mut track = TrackLoader::load_from_file(&opts.track_path)
        .map_err(|e| format!("track {}: {e}", opts.track_path.display()))?;
    opts.conditions.apply_to_track(&mut track);

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
    session.host_car_id = Some(host_car);
    session.conditions = opts.conditions.clamp();

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
        conditions: opts.conditions.clamp(),
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::network::{CarStateTelemetry, Telemetry};

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
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real/Spa.yaml"),
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
            &Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real/Spa.yaml"),
            "eau rouge",
        )
        .unwrap();
        assert_eq!(name, "Eau Rouge");
        assert!((station - 1056.7).abs() < 0.1);
    }

    #[test]
    fn sides_follow_the_bend() {
        let path =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real/Zandvoort.yaml");
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
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real/LeMans.yaml");
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
            track_path: root.join("content/tracks/real/Zandvoort.yaml"),
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
