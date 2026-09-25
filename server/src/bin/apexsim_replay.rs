//! `apexsim-replay`: simulate AI races offline, find the moments worth
//! filming in them, and cut those into clips the client plays back.
//!
//! ```text
//! apexsim-replay simulate --track content/tracks/real/Zandvoort.yaml --car yotota-lmp2 \
//!     --ai 12 --laps 3 --weather sunny --time 18:30 --out out/zandvoort.bin
//! apexsim-replay info out/zandvoort.bin
//! apexsim-replay find out/zandvoort.bin --corner Hugenholtz --before 150 --after 120 --min-cars 3
//! apexsim-replay cut out/zandvoort.bin --from-tick 24000 --to-tick 26400 --out out/clip.clip.json
//! apexsim-replay pose --track content/tracks/real/Spa.yaml --corner "Eau Rouge" --offset -60 --lateral -25 --height 6
//! ```
//!
//! Every command that reports prints JSON on stdout, so a script can drive
//! it; progress and warnings go to stderr.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use apexsim_server::data::SessionConditions;
use apexsim_server::replay::{read_replay_file, write_replay_file};
use apexsim_server::replay_tools::{
    corner_station, cut, describe, find_windows, landmark_point, lateral_on, parse_time_of_day,
    parse_weather, pose_at, pose_on_ground, simulate_race, to_clip, Side, SimulateOptions, Span,
};
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
    /// Print what a replay holds, as JSON.
    Info { replay: PathBuf },
    /// Find where the field runs through a stretch of the lap together.
    Find {
        replay: PathBuf,
        /// Track YAML; defaults to content/tracks/real/<stem>.yaml.
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
    /// Cut a replay to a window: `.bin` writes a replay, `.json` a client clip.
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
        #[arg(long)]
        out: PathBuf,
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
    let path = PathBuf::from(format!("content/tracks/real/{stem}.yaml"));
    if path.is_file() {
        Ok(path)
    } else {
        Err(format!(
            "{} not found (run from the repo root or pass --track)",
            path.display()
        ))
    }
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
        Command::Info { replay } => {
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
            if is_json {
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
