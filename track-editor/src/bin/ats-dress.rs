//! Rebuild a circuit's scenery from its real-world layout dossier, then
//! groom what is left around it.
//!
//! The dossier (`content/tracks/real/<Stem>.layout.json`, written by
//! `scripts/osm_layout.py` from OpenStreetMap) says what the place really
//! looks like: which side the pit lane runs and where, what the stands are
//! called and where they stand, the buildings, the bridges over the road,
//! the fairground wheel, the woods. [`track_editor::dress`] lays those,
//! and [`track_editor::groom`] then fills in barriers, boards and tree
//! belts around them — with the belts confined to the real woodland.
//!
//! ```text
//! ats-dress --all                           # every track that has a dossier
//! ats-dress content/tracks/real/Monza.yaml  # one circuit
//! ats-dress --all --dry-run                 # report without writing
//! ats-dress --no-groom Spa.yaml             # dress only
//! ```
//!
//! Idempotent: both passes own what they lay, so a second run over a
//! dressed scene writes nothing.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_editor::{ats_io, dress, groom, layout, project, ue_export_io};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

const USAGE: &str = "\
usage: ats-dress [--all] [--dry-run] [--no-groom] [--verbose] [track.yaml ...]

  --all, -a       every track under content/tracks/real that has a dossier
  --dry-run, -n   report what would change without writing
  --no-groom      dress only; leave barriers, boards and trees alone
  --verbose, -v   list dossier entries that could not be placed";

fn main() -> ExitCode {
    let mut all = false;
    let mut dry_run = false;
    let mut no_groom = false;
    let mut verbose = false;
    let mut tracks: Vec<PathBuf> = Vec::new();

    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--dry-run" | "-n" => dry_run = true,
            "--no-groom" => no_groom = true,
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

    if all {
        match ue_export_io::track_files_in(Path::new(DEFAULT_TRACK_DIR)) {
            Ok(found) => tracks.extend(
                found
                    .into_iter()
                    .filter(|t| layout::layout_path_for(t).exists()),
            ),
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
    for track_path in &tracks {
        let name = track_path.display();
        let layout_path = layout::layout_path_for(track_path);
        let layout = match layout::load_layout(&layout_path) {
            Ok(Some(layout)) => layout,
            Ok(None) => {
                eprintln!(
                    "{name}: no {} — nothing to dress from",
                    layout_path.display()
                );
                failures += 1;
                continue;
            }
            Err(e) => {
                eprintln!("{name}: {} is unusable: {e}", layout_path.display());
                failures += 1;
                continue;
            }
        };
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
        let Some(report) = dress::dress_scene(&opened.track, &mut scene, &layout) else {
            eprintln!("{name}: degenerate centerline, skipping");
            failures += 1;
            continue;
        };
        let groomed = if no_groom {
            None
        } else {
            match groom::groom_scene_with(&opened.track, &mut scene, Some(&layout)) {
                Some(g) => Some(g),
                None => {
                    eprintln!("{name}: grooming failed, skipping");
                    failures += 1;
                    continue;
                }
            }
        };

        println!(
            "{name}: {} stand(s) as {} prop(s), {} building(s), {} bridge(s), {} landmark(s), \
             pit lane {} ({} old prop(s) replaced){}",
            report.stands,
            report.stand_props,
            report.buildings,
            report.bridges,
            report.landmarks,
            if report.pit_lane {
                layout
                    .pit_lane
                    .as_ref()
                    .map(|p| p.side.label())
                    .unwrap_or("kept")
            } else {
                "kept"
            },
            report.removed,
            if dry_run { " (dry run)" } else { "" }
        );
        if let Some(g) = &groomed {
            println!(
                "    groom: {} wall(s), {} board(s), {} barrier(s), {} tree(s), {} pushed, {} reseated",
                g.walls, g.boards, g.barriers, g.trees, g.pushed, g.reseated
            );
        }
        if verbose {
            for note in &report.skipped {
                println!("    skipped {note}");
            }
        }

        if !dry_run {
            if let Err(e) = ats_io::save_ats(&opened.ats_path, &scene) {
                eprintln!("{name}: failed to save: {e}");
                failures += 1;
            }
        }
    }

    if failures > 0 {
        eprintln!("{failures} track(s) failed");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}
