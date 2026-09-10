//! Batch-groom prop placement in the shipped `.ats` scenes: tire walls and
//! barriers move to the runoff edge, everything else is seated on the
//! terrain, and distance boards, armco and tree belts are laid. See
//! `src/groom.rs` for the rules.
//!
//! ```text
//! ats-groom --all                          # every track under content/tracks/real
//! ats-groom content/tracks/real/Monza.yaml # one track
//! ats-groom --all --dry-run                # report without writing
//! ats-groom --verbose Monza.yaml           # also list braking corners + board stations
//! ```
//!
//! Idempotent: re-running over groomed scenes writes nothing.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_editor::track_data::TrackFile;
use track_editor::track_path::CenterlinePath;
use track_editor::{ats_io, groom, project, ue_export_io};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut all = false;
    let mut dry_run = false;
    let mut verbose = false;
    let mut tracks: Vec<PathBuf> = Vec::new();

    for arg in &args {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--dry-run" | "-n" => dry_run = true,
            "--verbose" | "-v" => verbose = true,
            "--help" | "-h" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            other if other.starts_with('-') => {
                eprintln!("unknown option {other}\n\n{USAGE}");
                return ExitCode::FAILURE;
            }
            path => tracks.push(PathBuf::from(path)),
        }
    }

    if !all && tracks.is_empty() {
        eprintln!("{USAGE}");
        return ExitCode::FAILURE;
    }
    if all {
        let dir = Path::new(DEFAULT_TRACK_DIR);
        match ue_export_io::track_files_in(dir) {
            Ok(found) => tracks.extend(found),
            Err(e) => {
                eprintln!("failed to list {}: {e}", dir.display());
                return ExitCode::FAILURE;
            }
        }
    }

    let mut failures = 0usize;
    let mut written = 0usize;
    for track_path in &tracks {
        let name = track_path.display();
        let opened = match project::open_project(track_path) {
            Ok(opened) => opened,
            Err(e) => {
                eprintln!("{name}: {e}");
                failures += 1;
                continue;
            }
        };
        let Some(mut scene) = opened.scene else {
            eprintln!("{name}: scene failed to load, skipping");
            failures += 1;
            continue;
        };
        let Some(report) = groom::groom_scene(&opened.track, &mut scene) else {
            eprintln!("{name}: degenerate centerline, skipping");
            failures += 1;
            continue;
        };

        if report.changed() && !dry_run {
            if let Err(e) = ats_io::save_ats(&opened.ats_path, &scene) {
                eprintln!("{name}: failed to save: {e}");
                failures += 1;
                continue;
            }
            written += 1;
        }
        println!(
            "{name}: {} wall segment(s){}, {} wall(s) removed from straights, {} board(s){}, {} armco segment(s){}, {} tree(s){}, {} prop(s) pushed clear, {} reseated, {} deleted{} of {}, pit lane {}{}",
            report.walls,
            relaid(report.walls_rebuilt),
            report.removed,
            report.boards,
            relaid(report.boards_rebuilt),
            report.barriers,
            relaid(report.barriers_rebuilt),
            report.trees,
            relaid(report.trees_rebuilt),
            report.pushed,
            report.reseated,
            report.deleted.len(),
            if report.deleted.is_empty() {
                String::new()
            } else {
                format!(" (ids {:?})", report.deleted)
            },
            report.total,
            if report.pit_rebuilt {
                "rebuilt"
            } else {
                "unchanged"
            },
            if dry_run { " (dry run)" } else { "" }
        );
        if verbose {
            print_corners(&opened.track);
        }
    }

    println!(
        "groomed {}/{} track(s), {written} file(s) written",
        tracks.len() - failures,
        tracks.len()
    );
    if failures > 0 {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

fn relaid(rebuilt: bool) -> &'static str {
    if rebuilt {
        " re-laid"
    } else {
        " unchanged"
    }
}

/// List every braking corner with the stations its boards were laid at.
fn print_corners(track: &TrackFile) {
    let Some(path) = CenterlinePath::from_track(track) else {
        return;
    };
    let corners = groom::braking_corners(&path);
    println!(
        "  {} braking corner(s) on {:.0} m (entry, end, outside, corner speed, boards):",
        corners.len(),
        path.total_length_m()
    );
    for corner in &corners {
        let boards: Vec<String> = corner
            .boards
            .iter()
            .map(|(asset, station)| format!("{asset}@{station:.0}"))
            .collect();
        println!(
            "    entry {:>6.0} m  end {:>6.0} m  {:<5}  {:>3.0} km/h  {}",
            corner.entry_m(),
            corner.run.end_m,
            corner.outside().label(),
            corner.corner_speed_mps * 3.6,
            boards.join(" ")
        );
    }
}

const USAGE: &str = "\
usage: ats-groom [--all] [--dry-run] [--verbose] [TRACK.yaml ...]

  --all, -a      groom every *.yaml under content/tracks/real
  --dry-run, -n  report what would change without writing .ats files
  --verbose, -v  also list braking corners and their board stations
";
