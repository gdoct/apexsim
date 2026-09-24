//! Iron the kinks out of a circuit's traced centerline, in place.
//!
//! The real circuits' YAML centerlines are GPS traces resampled to 5 m
//! nodes, and the metre-scale noise in a trace reads, over a 5 m chord, as
//! curvature. The Red Bull Ring's Remus comes out at an 8 m radius on a
//! 10.6 m wide road; the road loft there has no room to draw an inside
//! edge at all, so the exporter clamps, breaks and drops facets, and the
//! corner ships with holes in it. [`track_editor::track_smooth`] explains
//! what the filter does and why it is this one.
//!
//! ```text
//! ats-smooth --all                              # every real circuit
//! ats-smooth content/tracks/real/Spielberg.yaml # one
//! ats-smooth --all --dry-run                    # report, write nothing
//! ats-smooth --report content/tracks/real/*.yaml  # what is tight today
//! ```
//!
//! This rewrites the track YAML, which every other stage reads: re-run
//! `ats-dress` and `ats-export` afterwards, and expect the content
//! checksum — and so every client's copy of the track — to change.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_editor::track_data::TrackFile;
use track_editor::track_smooth::{
    self, node_stations, tight_nodes, SmoothConfig, SmoothReport, DEFAULT_ITERATIONS,
    DEFAULT_TOLERANCE_M, DEFAULT_WINDOW,
};
use track_editor::{track_io, ue_export_io};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

/// A corner tighter than this has less room than the road is wide, which
/// is where the exporter's loft starts deleting geometry. Reported before
/// and after so a circuit that still has one is obvious.
const TIGHT_RADIUS_M: f32 = 25.0;

const USAGE: &str = "\
usage: ats-smooth [--all] [--dry-run] [--report] [--tolerance M] [--passes N]
                  [--window N] [track.yaml ...]

  --all, -a        every track under content/tracks/real
  --dry-run, -n    report what would change without writing
  --report, -r     report the centerline as it is now and stop
  --tolerance M    how far a node may move from its traced position
  --tight-tol M    the same, inside a corner the trace pinched. Raising
                   this is what lets a badly traced apex be opened all
                   the way out to the minimum radius in one run, at the
                   cost of moving the line further from the survey
  --min-radius M   the tightest centerline radius to leave
  --passes N       filter passes
  --window N       half-width of the fitting window, in nodes";

fn main() -> ExitCode {
    let mut all = false;
    let mut dry_run = false;
    let mut report_only = false;
    let mut config = SmoothConfig::default();
    let mut tracks: Vec<PathBuf> = Vec::new();

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--dry-run" | "-n" => dry_run = true,
            "--report" | "-r" => report_only = true,
            "--help" | "-h" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            "--tolerance" | "--tight-tol" | "--min-radius" | "--passes" | "--window" => {
                let Some(value) = args.next() else {
                    eprintln!("{arg} needs a value\n\n{USAGE}");
                    return ExitCode::FAILURE;
                };
                let ok = match arg.as_str() {
                    "--tolerance" => value.parse().map(|v| config.tolerance_m = v).is_ok(),
                    "--tight-tol" => value.parse().map(|v| config.tight_tolerance_m = v).is_ok(),
                    "--min-radius" => value.parse().map(|v| config.min_radius_m = v).is_ok(),
                    "--passes" => value.parse().map(|v| config.iterations = v).is_ok(),
                    _ => value.parse().map(|v| config.window = v).is_ok(),
                };
                if !ok {
                    eprintln!("{arg}: {value} is not a number\n\n{USAGE}");
                    return ExitCode::FAILURE;
                }
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

    if !report_only {
        println!(
            "tolerance {:.2} m, {} pass(es), window +/-{} node(s)",
            config.tolerance_m,
            if config.iterations == DEFAULT_ITERATIONS {
                DEFAULT_ITERATIONS
            } else {
                config.iterations
            },
            if config.window == DEFAULT_WINDOW {
                DEFAULT_WINDOW
            } else {
                config.window
            },
        );
        if config.tolerance_m > DEFAULT_TOLERANCE_M * 3.0 {
            println!("  note: a tolerance this wide can move the course off its survey");
        }
    }

    let mut failures = 0usize;
    let mut written = 0usize;
    for track_path in &tracks {
        let name = track_path.display();
        let mut track: TrackFile = match track_io::load_track_file(track_path) {
            Ok(track) => track,
            Err(e) => {
                eprintln!("{name}: {e}");
                failures += 1;
                continue;
            }
        };

        if report_only {
            report_track(&name.to_string(), &track);
            continue;
        }

        let before = tight_nodes(&track, TIGHT_RADIUS_M).len();
        let Some(report) = track_smooth::smooth_track(&mut track, config) else {
            eprintln!("{name}: too few nodes to smooth, skipping");
            failures += 1;
            continue;
        };
        let after = tight_nodes(&track, TIGHT_RADIUS_M).len();
        print_report(&name.to_string(), &report, before, after);
        if report.min_radius_after_m < config.min_radius_m {
            // The default allowance is deliberately tight: a surveyed
            // line should not be moved metres without someone deciding
            // to. Say so, and say what the decision would look like.
            println!(
                "    still under the {:.0} m floor; run again to take it further, or                  --tight-tol 10 to open these fully in one run (which moves the                  worst apex several metres off the trace)",
                config.min_radius_m,
            );
        }

        if !report.changed() {
            println!("    already smooth; leaving the file alone");
            continue;
        }
        if dry_run {
            continue;
        }
        if let Err(e) = track_io::save_track_file(track_path, &track) {
            eprintln!("{name}: {e}");
            failures += 1;
            continue;
        }
        written += 1;
    }

    if !report_only && !dry_run {
        println!(
            "wrote {written}/{} track(s); re-run ats-dress and ats-export",
            tracks.len()
        );
    }
    if failures > 0 {
        eprintln!("{failures} track(s) failed");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}

fn print_report(name: &str, report: &SmoothReport, tight_before: usize, tight_after: usize) {
    println!(
        "{name}: {} node(s), moved {:.2} m at most ({:.3} m mean), z by {:.2} m",
        report.nodes, report.max_shift_m, report.mean_shift_m, report.max_z_shift_m
    );
    println!(
        "    tightest radius {:.1} m -> {:.1} m; worst curvature jump {:.4} -> {:.4} per m",
        report.min_radius_before_m,
        report.min_radius_after_m,
        report.max_curvature_jump_before,
        report.max_curvature_jump_after,
    );
    println!(
        "    under {TIGHT_RADIUS_M:.0} m radius: {tight_before} node(s) -> {tight_after}; \
         lap {:.1} m -> {:.1} m",
        report.length_before_m, report.length_after_m,
    );
    if report.raceline_worst_before > 0.0 || report.raceline_worst_after > 0.0 {
        let flag = if report.raceline_worst_after > 1.0 {
            "  <-- OFF THE ROAD"
        } else {
            ""
        };
        println!(
            "    raceline reaches {:.2} -> {:.2} of the half-width{flag}",
            report.raceline_worst_before, report.raceline_worst_after,
        );
    }
}

/// What the centerline looks like as it stands: where it is too tight for
/// the road to be drawn, by station, so the list reads like a lap.
fn report_track(name: &str, track: &TrackFile) {
    let stations = node_stations(track);
    let tight = tight_nodes(track, TIGHT_RADIUS_M);
    println!("{name}: {} node(s)", track.nodes.len());
    if tight.is_empty() {
        println!("    nothing under {TIGHT_RADIUS_M:.0} m radius");
        return;
    }
    println!(
        "    {} node(s) under {TIGHT_RADIUS_M:.0} m radius:",
        tight.len()
    );
    for (index, radius) in tight.iter().take(20) {
        let station = stations.get(*index).copied().unwrap_or(0.0);
        let node = &track.nodes[*index];
        let (left, right) = node.resolved_half_widths(track.default_width);
        println!(
            "      station {station:7.0} m  radius {radius:6.1} m  road {:.1} m wide",
            left + right
        );
    }
    if tight.len() > 20 {
        println!("      ... and {} more", tight.len() - 20);
    }
}
