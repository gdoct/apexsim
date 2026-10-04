//! `apexsim-replay`: simulate AI races offline, find the moments worth
//! filming in them, and cut those into clips the client plays back.
//!
//! ```text
//! apexsim-replay simulate --track content/tracks/default/Zandvoort.yaml --car yotota-lmp2 \
//!     --ai 12 --laps 3 --weather sunny --time 18:30 --out out/zandvoort.bin
//! apexsim-replay info out/zandvoort.bin
//! apexsim-replay find out/zandvoort.bin --corner Hugenholtz --before 150 --after 120 --min-cars 3
//! apexsim-replay cut out/zandvoort.bin --from-tick 24000 --to-tick 26400 --out out/clip.clip.json
//! apexsim-replay pose --track content/tracks/default/Spa.yaml --corner "Eau Rouge" --offset -60 --lateral -25 --height 6
//! apexsim-replay render --track content/tracks/default/Zandvoort.yaml --class GT3 --cars 20 --laps 2 \
//!     --seed 7 --out build/showcase/Zandvoort.gt3.day.apxs
//! apexsim-replay info build/showcase/Zandvoort.gt3.day.apxs --check
//! apexsim-replay convert out/zandvoort.bin --rate 30 --out out/zandvoort.apxs
//! ```
//!
//! `render`, `convert` and a `cut` to `.apxs` write spectator streams
//! (docs/SPECTATOR.md): what the menu backdrop, the server's showcase and
//! `-ApexReplay=` play.
//!
//! Every command that reports prints JSON on stdout, so a script can drive
//! it; progress and warnings go to stderr.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use apexsim_server::data::SessionConditions;
use apexsim_server::replay::{read_replay_file, write_replay_file};
use apexsim_server::replay_tools::{
    car_of_class, check_stream, corner_station, cut, describe, describe_stream, find_track_yaml,
    find_windows, landmark_point, lateral_on, load_car_folder, parse_time_of_day, parse_weather,
    pose_at, pose_on_ground, render_best, render_stream, replay_to_stream, simulate_race, to_clip,
    RenderOptions, Side, SimulateOptions, Span,
};
use apexsim_server::spectator::StreamFile;
use apexsim_server::track_loader::TrackLoader;
use clap::{Parser, Subcommand};

#[derive(Parser, Debug)]
#[command(author, version, about = "Simulate, search and cut ApexSim race replays", long_about = None)]
struct Args {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Run a headless AI race and write it as a replay (.bin).
    Simulate {
        /// Track YAML.
        #[arg(long)]
        track: PathBuf,
        /// Host car (folder, id or name); the field is its class.
        #[arg(long, default_value = "yotota-lmp2")]
        car: String,
        /// Every AI drives the host car.
        #[arg(long)]
        same_car: bool,
        /// The `content/cars` folder.
        #[arg(long, default_value = "content/cars")]
        cars_dir: PathBuf,
        #[arg(long, default_value_t = 12)]
        ai: u8,
        #[arg(long, default_value_t = 3)]
        laps: u8,
        /// Stop after this many seconds of racing even without a finish.
        #[arg(long, default_value_t = 900.0)]
        max_seconds: f32,
        #[arg(long, default_value_t = 3)]
        countdown: u16,
        /// sunny | cloudy | overcast | lightrain | heavyrain
        #[arg(long, default_value = "sunny")]
        weather: String,
        /// Local time, hh:mm.
        #[arg(long, default_value = "13:00")]
        time: String,
        #[arg(long, default_value_t = 240)]
        tick_rate: u16,
        /// Frames recorded per second of race.
        #[arg(long, default_value_t = 60)]
        record_hz: u16,
        /// Fix the grid (the AI drivers' ids): the same seed, the same race.
        #[arg(long)]
        seed: Option<u64>,
        #[arg(long)]
        out: PathBuf,
    },
    /// Run a headless AI race and write it as a spectator stream (.apxs):
    /// the grid, the countdown, the laps and a tail past the winner's flag.
    Render {
        /// Track YAML.
        #[arg(long)]
        track: PathBuf,
        /// Car class (GT3, LMP2, F1...): the field is every car of it.
        #[arg(long)]
        class: Option<String>,
        /// Host car (folder, id or name) instead of --class; the field is
        /// its class.
        #[arg(long)]
        car: Option<String>,
        /// Every AI drives the host car.
        #[arg(long)]
        same_car: bool,
        /// The `content/cars` folder (`content/cars/default` keeps the
        /// player's own cars out).
        #[arg(long, default_value = "content/cars")]
        cars_dir: PathBuf,
        /// Grid size.
        #[arg(long, default_value_t = 20)]
        cars: u8,
        /// Laps from green.
        #[arg(long, default_value_t = 2)]
        laps: u8,
        /// Stop after this many seconds of racing even without a winner.
        #[arg(long, default_value_t = 1800.0)]
        max_seconds: f32,
        /// Seconds the grid stands before the lights go out.
        #[arg(long, default_value_t = 8)]
        countdown: u16,
        /// Seconds kept after the winner takes the flag.
        #[arg(long, default_value_t = 10.0)]
        tail: f32,
        /// sunny | cloudy | overcast | lightrain | heavyrain
        #[arg(long, default_value = "sunny")]
        weather: String,
        /// Local time, hh:mm.
        #[arg(long, default_value = "13:00")]
        time: String,
        /// Air temperature, °C (from the weather and the clock when absent).
        #[arg(long, allow_hyphen_values = true)]
        air: Option<i8>,
        /// Mean wind, km/h.
        #[arg(long)]
        wind: Option<u8>,
        /// Where the wind blows from, degrees from the start straight.
        #[arg(long)]
        wind_from: Option<u16>,
        #[arg(long, default_value_t = 240)]
        tick_rate: u16,
        /// Frames per second of the stream.
        #[arg(long, default_value_t = 30)]
        rate: u16,
        /// The grid's seed: the same seed, the same file.
        #[arg(long, default_value_t = 0)]
        seed: u64,
        /// Render this many seeds, from --seed up, and keep the best.
        #[arg(long, default_value_t = 1)]
        seeds: u32,
        /// How the kept seed is chosen; `best` (least trouble, closest
        /// racing) is the only rule.
        #[arg(long, default_value = "best")]
        pick: String,
        /// Keep only a window of the race, in ticks.
        #[arg(long)]
        from_tick: Option<u32>,
        #[arg(long)]
        to_tick: Option<u32>,
        #[arg(long)]
        out: PathBuf,
    },
    /// Turn a replay (.bin) into a spectator stream (.apxs).
    Convert {
        replay: PathBuf,
        /// Frames per second of the stream.
        #[arg(long, default_value_t = 30)]
        rate: u16,
        /// Track YAML (checksum, path, sectors); defaults from the stem.
        #[arg(long)]
        track: Option<PathBuf>,
        /// The `content/cars` folder, for the cars' checksums.
        #[arg(long, default_value = "content/cars")]
        cars_dir: PathBuf,
        #[arg(long)]
        from_tick: Option<u32>,
        #[arg(long)]
        to_tick: Option<u32>,
        #[arg(long)]
        out: PathBuf,
    },
    /// Print what a replay (.bin) or a stream (.apxs) holds, as JSON.
    Info {
        replay: PathBuf,
        /// A stream only: exit non-zero when the track or a car it was
        /// raced on no longer matches the content on disk.
        #[arg(long)]
        check: bool,
        /// The `content/tracks` folder --check compares against.
        #[arg(long, default_value = "content/tracks")]
        tracks_dir: PathBuf,
        /// The `content/cars` folder --check compares against.
        #[arg(long, default_value = "content/cars")]
        cars_dir: PathBuf,
    },
    /// Find where the field runs through a stretch of the lap together.
    Find {
        replay: PathBuf,
        /// Track YAML; defaults to content/tracks/default/<stem>.yaml.
        #[arg(long)]
        track: Option<PathBuf>,
        /// A corner named in the track's layout dossier (substring).
        #[arg(long)]
        corner: Option<String>,
        /// A station on the lap, metres, instead of a corner.
        #[arg(long)]
        station: Option<f32>,
        /// Metres before the station that count.
        #[arg(long, default_value_t = 150.0)]
        before: f32,
        /// Metres after the station that count.
        #[arg(long, default_value_t = 100.0)]
        after: f32,
        /// Cars that must be in the stretch at once.
        #[arg(long, default_value_t = 2)]
        min_cars: u8,
        /// Seconds added either side of each window.
        #[arg(long, default_value_t = 1.5)]
        pad: f32,
        /// Count the first lap too (the start queue).
        #[arg(long)]
        include_lap_1: bool,
        /// Only the last N laps of the leader (the finish).
        #[arg(long)]
        last_laps: Option<u16>,
        /// How many windows to print.
        #[arg(long, default_value_t = 5)]
        top: usize,
    },
    /// Cut a replay to a window: `.apxs` writes a stream the client plays
    /// (`-ApexReplay=`), `.bin` a replay, `.json` the older client clip.
    Cut {
        replay: PathBuf,
        #[arg(long)]
        from_tick: Option<u32>,
        #[arg(long)]
        to_tick: Option<u32>,
        /// Seconds from the start of the recording, instead of ticks.
        #[arg(long)]
        from_s: Option<f32>,
        #[arg(long)]
        to_s: Option<f32>,
        /// Track YAML (for the clip's centerline); defaults from the stem.
        #[arg(long)]
        track: Option<PathBuf>,
        /// Frames per second of an `.apxs` (the replay's own rate at most).
        #[arg(long, default_value_t = 60)]
        rate: u16,
        /// The `content/cars` folder, for an `.apxs`' car checksums.
        #[arg(long, default_value = "content/cars")]
        cars_dir: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Build the track guide of each track and class (docs/TRACK_GUIDE.md):
    /// `<Stem>.<Class>.guide.json` and its `.guide.apxs` recording.
    Guide {
        /// Track YAMLs (or `--all`).
        tracks: Vec<PathBuf>,
        /// Every track YAML in `<tracks-dir>/default`.
        #[arg(long)]
        all: bool,
        #[arg(long, default_value = "content/tracks")]
        tracks_dir: PathBuf,
        /// Car classes, comma separated, or `all` (every class under --cars-dir).
        #[arg(long, default_value = "all")]
        class: String,
        #[arg(long, default_value = "content/cars/default")]
        cars_dir: PathBuf,
        #[arg(long, default_value = "build/guide")]
        out: PathBuf,
        /// Print each track's detected corners (to author notes against) and
        /// simulate nothing.
        #[arg(long)]
        report: bool,
        /// Skip a guide whose file is newer than the track YAML, its notes,
        /// its dossier and every car.toml of the class.
        #[arg(long)]
        missing_only: bool,
        #[arg(long, default_value_t = 1)]
        seed: u64,
        /// Seeds tried per car before the least bad run is kept.
        #[arg(long, default_value_t = 6)]
        tries: u32,
        #[arg(long, default_value_t = 110)]
        skill: u8,
        /// Seconds between the cars.
        #[arg(long, default_value_t = 2.0)]
        gap: f32,
        #[arg(long, default_value_t = 3)]
        cars: u8,
        /// Frames per second of the recording.
        #[arg(long, default_value_t = 60)]
        rate: u16,
    },
    /// Where to stand a camera: a pose beside the road, as JSON and as the
    /// client's `-ApexCamera=` / `-ApexCameraLookAt=` switches.
    Pose {
        #[arg(long)]
        track: PathBuf,
        #[arg(long)]
        corner: Option<String>,
        #[arg(long)]
        station: Option<f32>,
        /// Metres along the lap from the corner/station (negative: before it).
        #[arg(long, default_value_t = 0.0, allow_hyphen_values = true)]
        offset: f32,
        /// Metres from the centerline: to the left, negative to the right,
        /// or on --side.
        #[arg(long, default_value_t = 20.0, allow_hyphen_values = true)]
        lateral: f32,
        /// left | right | outside | inside (of the bend at the eye), instead
        /// of the sign of --lateral.
        #[arg(long)]
        side: Option<String>,
        /// Metres above the road.
        #[arg(long, default_value_t = 4.0, allow_hyphen_values = true)]
        height: f32,
        /// Where the camera looks, metres along the lap from the corner.
        #[arg(long, default_value_t = 0.0, allow_hyphen_values = true)]
        look_offset: f32,
        #[arg(long, default_value_t = 0.0, allow_hyphen_values = true)]
        look_lateral: f32,
        /// Side for --look-lateral, as --side.
        #[arg(long)]
        look_side: Option<String>,
        #[arg(long, default_value_t = 1.0, allow_hyphen_values = true)]
        look_height: f32,
        /// Look at a dossier landmark, crossing, stand or structure (name or
        /// kind: `big_wheel`, `Tyre bridge`) instead of a station; lifted by
        /// --look-height.
        #[arg(long)]
        look_landmark: Option<String>,
    },
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("apexsim-replay: {e}");
            ExitCode::FAILURE
        }
    }
}

struct GuideArgs {
    tracks: Vec<PathBuf>,
    all: bool,
    tracks_dir: PathBuf,
    class: String,
    cars_dir: PathBuf,
    out: PathBuf,
    report: bool,
    missing_only: bool,
    seed: u64,
    tries: u32,
    skill: u8,
    gap: f32,
    cars: u8,
    rate: u16,
}

fn run_guide(args: GuideArgs) -> Result<(), String> {
    use apexsim_server::track_guide::{
        build_guide, detect_corners, dossier_corners, guide_name, lap_length_m, name_corners,
        notes_path, write_guide, GuideOptions,
    };
    let mut tracks = args.tracks;
    if args.all {
        let dir = args.tracks_dir.join("default");
        let mut found: Vec<PathBuf> = std::fs::read_dir(&dir)
            .map_err(|e| format!("{}: {e}", dir.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|x| x == "yaml"))
            .collect();
        found.sort();
        tracks.extend(found);
    }
    if tracks.is_empty() {
        return Err("name a track YAML or pass --all".to_string());
    }

    if args.report {
        let mut report = Vec::new();
        for path in &tracks {
            let track = TrackLoader::load_from_file(path)
                .map_err(|e| format!("{}: {e}", path.display()))?;
            let lap = lap_length_m(&track);
            let corners = detect_corners(&track);
            let names = name_corners(&corners, &dossier_corners(path), lap);
            let rows: Vec<serde_json::Value> = corners
                .iter()
                .zip(names)
                .enumerate()
                .map(|(i, (c, name))| {
                    serde_json::json!({
                        "number": i + 1,
                        "name": name,
                        "direction": c.direction(),
                        "entry_m": c.entry_m.round(),
                        "apex_m": c.apex_m.round(),
                        "exit_m": c.exit_m.round(),
                        "turn_deg": c.turn_rad().to_degrees().round(),
                        "min_radius_m": (1.0 / c.peak_kappa().max(1e-4)).round(),
                    })
                })
                .collect();
            report.push(serde_json::json!({
                "track": path.file_stem().map(|s| s.to_string_lossy().into_owned()),
                "length_m": lap.round(),
                "corners": rows,
            }));
        }
        return print_json(&report);
    }

    let (configs, _) = load_car_folder(&args.cars_dir)?;
    let mut classes: Vec<String> = if args.class.eq_ignore_ascii_case("all") {
        let mut all: Vec<String> = configs
            .values()
            .map(|c| c.class.trim().to_string())
            .collect();
        all.sort();
        all.dedup();
        all
    } else {
        args.class
            .split(',')
            .map(|c| c.trim().to_string())
            .collect()
    };
    classes.retain(|c| !c.is_empty());

    let newest_input = |track: &Path, class: &str| -> Option<std::time::SystemTime> {
        let mut inputs = vec![
            track.to_path_buf(),
            notes_path(track),
            track.with_extension("layout.json"),
        ];
        for toml in apexsim_server::car_loader::car_toml_paths(&args.cars_dir) {
            if let Ok(car) = apexsim_server::car_loader::CarLoader::load_from_file(&toml) {
                if car.class.trim().eq_ignore_ascii_case(class) {
                    inputs.push(toml);
                }
            }
        }
        inputs
            .iter()
            .filter_map(|p| std::fs::metadata(p).and_then(|m| m.modified()).ok())
            .max()
    };

    let mut failures = Vec::new();
    let mut written = Vec::new();
    for path in &tracks {
        let stem = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        for class in &classes {
            let target = args.out.join(guide_name(&stem, class));
            if args.missing_only {
                let built = std::fs::metadata(&target).and_then(|m| m.modified()).ok();
                if let (Some(built), Some(input)) = (built, newest_input(path, class)) {
                    if built >= input {
                        continue;
                    }
                }
            }
            let started = std::time::Instant::now();
            let opts = GuideOptions {
                track_path: path.clone(),
                cars_dir: args.cars_dir.clone(),
                class: class.clone(),
                seed: args.seed,
                tries: args.tries,
                skill: args.skill,
                car_gap_s: args.gap,
                cars: args.cars,
                record_hz: args.rate,
                ..GuideOptions::default()
            };
            let result = build_guide(&opts).and_then(|built| {
                let files = write_guide(&built, &args.out)?;
                Ok((built, files))
            });
            match result {
                Ok((built, (json, _))) => {
                    let off: Vec<String> = built
                        .runs
                        .iter()
                        .enumerate()
                        .filter(|(_, r)| !r.valid || r.off_track_s > 0.0)
                        .map(|(k, r)| format!("car {} {:.1} s off", k + 1, r.off_track_s))
                        .collect();
                    eprintln!(
                        "{stem} {class}: {} corners, lap {:.3} s{} in {:.1} s -> {}",
                        built.guide.corners.len(),
                        built.guide.track.lap_time_s,
                        if off.is_empty() {
                            String::new()
                        } else {
                            format!(" ({})", off.join(", "))
                        },
                        started.elapsed().as_secs_f32(),
                        json.display()
                    );
                    written.push(json.display().to_string());
                }
                Err(e) => {
                    eprintln!("{stem} {class}: {e}");
                    failures.push(format!("{stem} {class}: {e}"));
                }
            }
        }
    }
    print_json(&serde_json::json!({ "written": written, "failed": failures }))?;
    if failures.is_empty() {
        Ok(())
    } else {
        Err(format!("{} guide(s) failed", failures.len()))
    }
}

fn print_json<T: serde::Serialize>(value: &T) -> Result<(), String> {
    println!(
        "{}",
        serde_json::to_string_pretty(value).map_err(|e| e.to_string())?
    );
    Ok(())
}

/// The track YAML a replay was recorded on, when none is named.
fn default_track(explicit: Option<PathBuf>, stem: Option<&str>) -> Result<PathBuf, String> {
    if let Some(path) = explicit {
        return Ok(path);
    }
    let stem = stem.ok_or("the replay names no track; pass --track")?;
    // The shipped circuits, then the player's own.
    let candidates =
        ["default", "custom"].map(|dir| PathBuf::from(format!("content/tracks/{dir}/{stem}.yaml")));
    candidates
        .iter()
        .find(|path| path.is_file())
        .cloned()
        .ok_or_else(|| {
            format!(
                "{stem}.yaml not found in content/tracks/default or content/tracks/custom \
                 (run from the repo root or pass --track)"
            )
        })
}

/// Write a replay's frames as a spectator stream, with the track's and the
/// cars' checksums when the content is at hand (a stream without them still
/// plays; `info --check` just has less to compare).
fn write_stream(
    metadata: &apexsim_server::replay::ReplayMetadata,
    frames: &[apexsim_server::replay::ReplayFrame],
    rate: u16,
    track: Option<PathBuf>,
    cars_dir: &Path,
    out: &Path,
) -> Result<(), String> {
    let track = track
        .or_else(|| {
            metadata
                .track_stem
                .as_deref()
                .and_then(|stem| find_track_yaml(Path::new("content/tracks"), stem))
        })
        .and_then(|path| match TrackLoader::load_from_file(&path) {
            Ok(track) => Some(track),
            Err(e) => {
                eprintln!("track {}: {e}; the stream carries no path", path.display());
                None
            }
        });
    if track.is_none() {
        eprintln!("no track YAML at hand: the stream carries no checksum, path or sectors");
    }
    let cars = load_car_folder(cars_dir).ok().map(|(cars, _)| cars);
    let content = replay_to_stream(metadata, frames, rate, track.as_ref(), cars.as_ref())?;
    content.write_file(out).map_err(|e| e.to_string())
}

fn station_for(track: &Path, corner: Option<&str>, station: Option<f32>) -> Result<f32, String> {
    match (corner, station) {
        (Some(name), _) => {
            let (found, s) = corner_station(track, name)?;
            eprintln!("corner '{found}' at {s:.1} m");
            Ok(s)
        }
        (None, Some(s)) => Ok(s),
        (None, None) => Err("pass --corner or --station".to_string()),
    }
}

fn run(args: Args) -> Result<(), String> {
    match args.command {
        Command::Simulate {
            track,
            car,
            same_car,
            cars_dir,
            ai,
            laps,
            max_seconds,
            countdown,
            weather,
            time,
            tick_rate,
            record_hz,
            seed,
            out,
        } => {
            let opts = SimulateOptions {
                track_path: track,
                cars_dir,
                host_car: car,
                same_car,
                ai_count: ai,
                laps,
                max_seconds,
                countdown_seconds: countdown,
                conditions: SessionConditions {
                    weather: parse_weather(&weather)?,
                    time_of_day_minutes: parse_time_of_day(&time)?,
                    ..SessionConditions::DEFAULT
                },
                tick_rate,
                record_hz,
                seed,
            };
            let started = std::time::Instant::now();
            let (metadata, frames) = simulate_race(&opts)?;
            write_replay_file(&out, metadata.clone(), &frames).map_err(|e| e.to_string())?;
            eprintln!(
                "simulated {} cars on {} in {:.1} s -> {}",
                metadata.participants.len(),
                metadata.track_name,
                started.elapsed().as_secs_f32(),
                out.display()
            );
            print_json(&describe(&metadata, &frames))
        }
        Command::Render {
            track,
            class,
            car,
            same_car,
            cars_dir,
            cars,
            laps,
            max_seconds,
            countdown,
            tail,
            weather,
            time,
            air,
            wind,
            wind_from,
            tick_rate,
            rate,
            seed,
            seeds,
            pick,
            from_tick,
            to_tick,
            out,
        } => {
            if !pick.eq_ignore_ascii_case("best") {
                return Err(format!("unknown --pick '{pick}' (best)"));
            }
            let host_car = match (car, class) {
                (Some(car), _) => car,
                (None, Some(class)) => {
                    let (configs, folders) = load_car_folder(&cars_dir)?;
                    car_of_class(&configs, &folders, &class)
                        .map(|id| id.to_string())
                        .ok_or_else(|| {
                            format!("no car of class '{class}' under {}", cars_dir.display())
                        })?
                }
                (None, None) => return Err("pass --class or --car".to_string()),
            };
            let opts = RenderOptions {
                race: SimulateOptions {
                    track_path: track,
                    cars_dir,
                    host_car,
                    same_car,
                    ai_count: cars,
                    laps,
                    max_seconds,
                    countdown_seconds: countdown,
                    conditions: SessionConditions {
                        weather: parse_weather(&weather)?,
                        time_of_day_minutes: parse_time_of_day(&time)?,
                        air_temp_c: air,
                        humidity_pct: None,
                        wind_kph: wind,
                        wind_from_deg: wind_from,
                    }
                    .clamp(),
                    tick_rate,
                    record_hz: rate,
                    seed: Some(seed),
                },
                tail_seconds: tail,
                from_tick,
                to_tick,
            };
            let started = std::time::Instant::now();
            let rendered = if seeds > 1 {
                render_best(&opts, seeds)?
            } else {
                render_stream(&opts)?
            };
            rendered
                .content
                .write_file(&out)
                .map_err(|e| e.to_string())?;
            let file = StreamFile::open(&out).map_err(|e| e.to_string())?;
            eprintln!(
                "rendered {} cars on {} ({:.0} s of race) in {:.1} s -> {} ({} KB)",
                file.roster.entries.len(),
                file.header.track.display_name,
                file.header.duration_s(),
                started.elapsed().as_secs_f32(),
                out.display(),
                file.file_len() / 1024
            );
            print_json(&serde_json::json!({
                "out": out,
                "seed": rendered.seed,
                "score": rendered.score.score(),
                "race": rendered.score,
                "stream": describe_stream(&file)?,
            }))
        }
        Command::Convert {
            replay,
            rate,
            track,
            cars_dir,
            from_tick,
            to_tick,
            out,
        } => {
            let (metadata, frames) = read_replay_file(&replay).map_err(|e| e.to_string())?;
            let part = cut(&frames, from_tick.unwrap_or(0), to_tick.unwrap_or(u32::MAX));
            write_stream(&metadata, &part, rate, track, &cars_dir, &out)?;
            let file = StreamFile::open(&out).map_err(|e| e.to_string())?;
            print_json(&describe_stream(&file)?)
        }
        Command::Info {
            replay,
            check,
            tracks_dir,
            cars_dir,
        } => {
            if StreamFile::is_stream_file(&replay) {
                let file = StreamFile::open(&replay).map_err(|e| e.to_string())?;
                let info = describe_stream(&file)?;
                if !check {
                    return print_json(&info);
                }
                let stale = check_stream(&file.header, &file.roster, &tracks_dir, &cars_dir);
                print_json(&serde_json::json!({ "stream": info, "stale": stale }))?;
                return if stale.is_empty() {
                    Ok(())
                } else {
                    Err(format!(
                        "{} no longer matches the content: {}",
                        replay.display(),
                        stale.join("; ")
                    ))
                };
            }
            if check {
                return Err("--check reads a stream (.apxs), not a replay".to_string());
            }
            let (metadata, frames) = read_replay_file(&replay).map_err(|e| e.to_string())?;
            print_json(&describe(&metadata, &frames))
        }
        Command::Find {
            replay,
            track,
            corner,
            station,
            before,
            after,
            min_cars,
            pad,
            include_lap_1,
            last_laps,
            top,
        } => {
            let (metadata, frames) = read_replay_file(&replay).map_err(|e| e.to_string())?;
            let track = default_track(track, metadata.track_stem.as_deref())?;
            let at = station_for(&track, corner.as_deref(), station)?;
            let span = Span::around(at, before, after, metadata.track_length_m);
            let mut frames = frames;
            if let Some(n) = last_laps {
                // The leader's lap count at the end; keep frames where the
                // leader is on one of the last `n`.
                let final_lap = frames
                    .iter()
                    .flat_map(|f| f.telemetry.car_states.iter().map(|c| c.current_lap))
                    .max()
                    .unwrap_or(0);
                let first_lap = final_lap.saturating_sub(n.saturating_sub(1));
                frames.retain(|f| {
                    f.telemetry
                        .car_states
                        .iter()
                        .map(|c| c.current_lap)
                        .max()
                        .unwrap_or(0)
                        >= first_lap
                });
            }
            let mut windows = find_windows(&metadata, &frames, span, min_cars, pad, !include_lap_1);
            windows.truncate(top);
            print_json(&serde_json::json!({
                "station_m": at,
                "span": [span.from_m, span.to_m],
                "windows": windows,
            }))
        }
        Command::Cut {
            replay,
            from_tick,
            to_tick,
            from_s,
            to_s,
            track,
            rate: stream_rate,
            cars_dir,
            out,
        } => {
            let (metadata, frames) = read_replay_file(&replay).map_err(|e| e.to_string())?;
            let first = frames.first().map(|f| f.tick).unwrap_or(0);
            let last = frames.last().map(|f| f.tick).unwrap_or(0);
            let rate = metadata.tick_rate.max(1) as f32;
            let from = from_tick
                .or(from_s.map(|s| (s * rate) as u32))
                .unwrap_or(first);
            let to = to_tick.or(to_s.map(|s| (s * rate) as u32)).unwrap_or(last);
            if to <= from {
                return Err(format!("empty window {from}..{to}"));
            }
            let part = cut(&frames, from, to);
            if part.is_empty() {
                return Err(format!(
                    "no frames between ticks {from} and {to} (the recording runs {first}..{last})"
                ));
            }
            let is_json = out
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("json"));
            let is_stream = out
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("apxs"));
            if is_stream {
                write_stream(&metadata, &part, stream_rate, track, &cars_dir, &out)?;
            } else if is_json {
                let track = default_track(track, metadata.track_stem.as_deref())?;
                let track = TrackLoader::load_from_file(&track).map_err(|e| e.to_string())?;
                let clip = to_clip(&metadata, &part, &track.centerline, 10.0);
                if let Some(parent) = out.parent().filter(|p| !p.as_os_str().is_empty()) {
                    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
                }
                let text = serde_json::to_string(&clip).map_err(|e| e.to_string())?;
                std::fs::write(&out, text).map_err(|e| e.to_string())?;
            } else {
                write_replay_file(&out, metadata.clone(), &part).map_err(|e| e.to_string())?;
            }
            eprintln!(
                "cut {} frames ({:.1} s) -> {}",
                part.len(),
                (to - from) as f32 / rate,
                out.display()
            );
            print_json(&serde_json::json!({
                "out": out,
                "frames": part.len(),
                "from_tick": part.first().map(|f| f.tick),
                "to_tick": part.last().map(|f| f.tick),
                "duration_s": (part.last().unwrap().tick - part.first().unwrap().tick) as f32 / rate,
            }))
        }
        Command::Guide {
            tracks,
            all,
            tracks_dir,
            class,
            cars_dir,
            out,
            report,
            missing_only,
            seed,
            tries,
            skill,
            gap,
            cars,
            rate,
        } => run_guide(GuideArgs {
            tracks,
            all,
            tracks_dir,
            class,
            cars_dir,
            out,
            report,
            missing_only,
            seed,
            tries,
            skill,
            gap,
            cars,
            rate,
        }),
        Command::Pose {
            track,
            corner,
            station,
            offset,
            lateral,
            side,
            height,
            look_offset,
            look_lateral,
            look_side,
            look_height,
            look_landmark,
        } => {
            let at = station_for(&track, corner.as_deref(), station)?;
            let track_path = track;
            let track = TrackLoader::load_from_file(&track_path).map_err(|e| e.to_string())?;
            let signed =
                |station: f32, distance: f32, side: &Option<String>| -> Result<f32, String> {
                    Ok(match side {
                        Some(side) => lateral_on(&track, station, distance, Side::parse(side)?),
                        None => distance,
                    })
                };
            let eye_station = at + offset;
            let eye = pose_on_ground(
                &track,
                eye_station,
                signed(eye_station, lateral, &side)?,
                height,
            );
            let target = match look_landmark.as_deref() {
                Some(name) => {
                    let (label, p) = landmark_point(&track_path, &track, name, look_height)?;
                    eprintln!(
                        "looking at '{label}' ({:.1}, {:.1}, {:.1})",
                        p[0], p[1], p[2]
                    );
                    let mut t = pose_at(&track, at + look_offset, 0.0, 0.0);
                    (t.x, t.y, t.z) = (p[0], p[1], p[2]);
                    t
                }
                None => {
                    let look_station = at + look_offset;
                    pose_at(
                        &track,
                        look_station,
                        signed(look_station, look_lateral, &look_side)?,
                        look_height,
                    )
                }
            };
            let (dx, dy, dz) = (target.x - eye.x, target.y - eye.y, target.z - eye.z);
            let yaw = dy.atan2(dx).to_degrees();
            let pitch = dz.atan2((dx * dx + dy * dy).sqrt()).to_degrees();
            print_json(&serde_json::json!({
                "eye": eye,
                "target": target,
                "ground": if track.ground.is_some() { "heightfield" } else { "road" },
                "yaw_deg": yaw,
                "pitch_deg": pitch,
                "camera_arg": format!("-ApexCamera={:.2},{:.2},{:.2},{:.1},{:.1}", eye.x, eye.y, eye.z, yaw, pitch),
                "look_at_arg": format!(
                    "-ApexCameraLookAt={:.2},{:.2},{:.2},{:.2},{:.2},{:.2}",
                    eye.x, eye.y, eye.z, target.x, target.y, target.z
                ),
            }))
        }
    }
}
