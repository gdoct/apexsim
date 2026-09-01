//! Batch-groom prop placement in the shipped `.ats` scenes: tire walls and
//! barriers move to the runoff edge, everything else is seated on the
//! terrain. See `src/groom.rs` for the rules.
//!
//! ```text
//! ats-groom --all                          # every track under content/tracks/real
//! ats-groom content/tracks/real/Monza.yaml # one track
//! ats-groom --all --dry-run                # report without writing
//! ```
//!
//! Idempotent: re-running over groomed scenes writes nothing.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_editor::{ats_io, groom, project, ue_export_io};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut all = false;
    let mut dry_run = false;
    let mut tracks: Vec<PathBuf> = Vec::new();

    for arg in &args {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--dry-run" | "-n" => dry_run = true,
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
            "{name}: {} wall segment(s){}, {} wall(s) removed from straights, {} prop(s) pushed clear, {} reseated of {}, pit lane {}{}",
            report.walls,
            if report.walls_rebuilt {
                " re-laid"
            } else {
                " unchanged"
            },
            report.removed,
            report.pushed,
            report.reseated,
            report.total,
            if report.pit_rebuilt {
                "rebuilt"
            } else {
                "unchanged"
            },
            if dry_run { " (dry run)" } else { "" }
        );
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

const USAGE: &str = "\
usage: ats-groom [--all] [--dry-run] [TRACK.yaml ...]

  --all, -a      groom every *.yaml under content/tracks/real
  --dry-run, -n  report what would change without writing .ats files
";
