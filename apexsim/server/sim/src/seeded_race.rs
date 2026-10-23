//! A seeded AI race, set up the way the server's offline tools (and the
//! client's own simulation) start one: the track loaded with the session's
//! air baked in, the car folder read, the field dealt and seeded, the
//! countdown running. The same options and seed give the same race, tick
//! for tick, wherever it runs: `apexsim-replay render` (the showcases),
//! the track guide, and the client through the physics library's C API.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::ai_driver::generate_ai_profiles;
use crate::car_loader::CarLoader;
use crate::data::*;
use crate::game_session::GameSession;
use crate::track_loader::TrackLoader;

/// The race a clip is cut from.
#[derive(Debug, Clone)]
pub struct SimulateOptions {
    /// The track YAML.
    pub track_path: PathBuf,
    /// The content's `cars` folder to draw the field from.
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
    /// The AI field's level, as a host's `CreateSession.ai_skill`: the field
    /// spread round it (`ai_driver::field_skills`). `None` is the mixed
    /// field, novice to ace, which every showcase and guide was rendered
    /// with, so they replay as before.
    pub ai_level: Option<u8>,
}

impl Default for SimulateOptions {
    fn default() -> Self {
        Self {
            track_path: PathBuf::from(
                "../apexsim-content/tracks/default/Zandervoort/Zandervoort.yaml",
            ),
            cars_dir: PathBuf::from("../apexsim-content/cars"),
            host_car: "yotota-lmp2".to_string(),
            same_car: false,
            ai_count: 12,
            laps: 3,
            max_seconds: 600.0,
            countdown_seconds: 3,
            conditions: SessionConditions::DEFAULT,
            tick_rate: crate::game_session::DEFAULT_TICK_RATE_HZ,
            record_hz: 60,
            seed: None,
            ai_level: None,
        }
    }
}

/// Cars by id.
pub type CarMap = HashMap<CarConfigId, CarConfig>;
/// The content's `cars` folder each car came from, by id.
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
pub fn seeded_id(seed: u64, index: u64) -> uuid::Uuid {
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
    let conditions = opts.conditions.at_site(&track).resolve_at(
        opts.conditions.sky_seed_or(opts.seed.unwrap_or(0)),
        track.climate_site(),
    );
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
    let mut profiles = generate_ai_profiles(ai_count, opts.ai_level);
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

/// One tick of a race nobody drives: every car's input from its own AI
/// (`GameSession::generate_ai_input`, in the participants' order), then the
/// session's tick. What a showcase render, a guide's field and the
/// client's own simulation each step, so a seed gives one race.
pub fn tick_field(race: &mut GameSession) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .keys()
        .map(|id| (*id, race.generate_ai_input(id)))
        .collect();
    race.tick(&inputs);
}
