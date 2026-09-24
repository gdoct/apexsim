//! Put each circuit's banking on the corner it belongs to, leaning the
//! right way. [`track_editor::track_bank`] explains what was wrong with the
//! banking the enrichment script laid and how this re-derives it.
//!
//! ```text
//! ats-bank --all                               # every real circuit
//! ats-bank content/tracks/real/Zandvoort.yaml  # one
//! ats-bank --all --dry-run                     # report, write nothing
//! ```
//!
//! This rewrites the track YAML: re-run `ats-dress` and `ats-export`
//! afterwards (the road, the verge and the ground are all baked from it).

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_editor::track_bank::{self, BankReport};
use track_editor::{track_io, ue_export_io};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

const USAGE: &str = "\
usage: ats-bank [--all] [--dry-run] [track.yaml ...]

  --all, -a        every track under content/tracks/real
  --dry-run, -n    report what would change without writing";

fn main() -> ExitCode {
    let mut all = false;
    let mut dry_run = false;
    let mut tracks: Vec<PathBuf> = Vec::new();
    for arg in std::env::args().skip(1) {
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
    if all {
        match ue_export_io::track_files_in(Path::new(DEFAULT_TRACK_DIR)) {
            Ok(found) => tracks.extend(found),
            Err(e) => {
                eprintln!("failed to list {DEFAULT_TRACK_DIR}: {e}");
                return ExitCode::FAILURE;
            }
        }
    }
    if tracks.is_empty() {
        eprintln!("{USAGE}");
        return ExitCode::FAILURE;
    }

    let mut failures = 0usize;
    let mut written = 0usize;
    for track_path in &tracks {
        let name = track_path.display().to_string();
        let mut track = match track_io::load_track_file(track_path) {
            Ok(track) => track,
            Err(e) => {
                eprintln!("{name}: {e}");
                failures += 1;
                continue;
            }
        };
        let Some(report) = track_bank::realign_banking(&mut track) else {
            eprintln!("{name}: too few nodes, skipping");
            continue;
        };
        print_report(&name, &report);
        if !report.changed() || dry_run {
            continue;
        }
        if let Err(e) = track_io::save_track_file(track_path, &track) {
            eprintln!("{name}: {e}");
            failures += 1;
            continue;
        }
        written += 1;
    }
    if !dry_run {
        println!(
            "wrote {written}/{} track(s); re-run ats-dress and ats-export",
            tracks.len()
        );
    }
    if failures > 0 {
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}

fn print_report(name: &str, report: &BankReport) {
    if report.spans == 0 {
        println!("{name}: no banking");
        return;
    }
    println!(
        "{name}: {} banked span(s) -> {} bend(s), largest change {:.1} deg",
        report.spans,
        report.bends.len(),
        report.max_change_rad.to_degrees()
    );
    for (from, to, angle) in &report.bends {
        let hand = if *angle > 0.0 {
            "right-hander"
        } else {
            "left-hander"
        };
        println!(
            "    {from:7.0}-{to:<7.0} m  {:+5.1} deg  ({hand})",
            angle.to_degrees()
        );
    }
    for (from, to) in &report.dropped {
        println!("    {from:7.0}-{to:<7.0} m  dropped: no bend within reach");
    }
}
