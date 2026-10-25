//! Rebuild a circuit's scenery from its real-world layout dossier, then
//! groom what is left around it.
//!
//! The dossier (`tracks/default/<Stem>.layout.json`, written by
//! `tools/osm_layout.py` from OpenStreetMap) says what the place really
//! looks like: which side the pit lane runs and where, what the stands are
//! called and where they stand, the buildings, the bridges over the road,
//! the fairground wheel, the woods. [`track_core::dress`] lays those,
//! and [`track_core::groom`] then fills in barriers, boards and tree
//! belts around them — with the belts confined to the real woodland.
//!
//! ```text
//! ats-dress --all                           # every track that has a dossier (or a DEM)
//! ats-dress tracks/default/Monzarella/Monzarella.yaml  # one circuit
//! ats-dress --all --dry-run                 # report without writing
//! ats-dress --no-groom SpaFrankenchamps.yaml             # dress only
//! ats-dress --explain SpaFrankenchamps.yaml              # what each dossier entry became
//! ```
//!
//! Idempotent: both passes own what they lay, so a second run over a
//! dressed scene writes nothing. A circuit with an elevation model and no
//! dossier gets its far field alone (`dress::lay_far_field`).

use std::path::PathBuf;
use std::process::ExitCode;

use track_core::{ats_io, dress, groom, layout, project, ue_export_io};

const USAGE: &str = "\
usage: ats-dress [--all] [--dry-run] [--no-groom] [--verbose] [--explain] [track.yaml ...]

  --all, -a       every track under tracks/{default,custom} that has a dossier,
                  or a DEM sidecar (the far field alone)
  --dry-run, -n   report what would change without writing
  --no-groom      dress only; leave barriers, boards and trees alone
  --verbose, -v   list dossier entries that could not be placed
  --explain, -e   list what each dossier entry became (props by source stamp
                  and asset), what each lost on the way, and what no pass owns";

/// The scene's props by source stamp (`Prop::source`) and asset, one line
/// a source, the unstamped (hand-placed, or laid before the stamps) last.
fn explain_scene(scene: &track_core::ats::AtsScene) -> Vec<String> {
    use std::collections::BTreeMap;
    let mut by_source: BTreeMap<String, BTreeMap<String, usize>> = BTreeMap::new();
    let mut unstamped: BTreeMap<String, usize> = BTreeMap::new();
    for prop in &scene.props {
        match &prop.source {
            Some(source) => {
                *by_source
                    .entry(source.clone())
                    .or_default()
                    .entry(prop.asset.clone())
                    .or_default() += 1
            }
            None => *unstamped.entry(prop.asset.clone()).or_default() += 1,
        }
    }
    let assets = |m: &BTreeMap<String, usize>| {
        m.iter()
            .map(|(asset, n)| {
                if *n == 1 {
                    asset.clone()
                } else {
                    format!("{n} x {asset}")
                }
            })
            .collect::<Vec<_>>()
            .join(", ")
    };
    let mut lines = Vec::new();
    for (source, m) in &by_source {
        let n: usize = m.values().sum();
        lines.push(format!("{source}: {n} prop(s): {}", assets(m)));
    }
    if !unstamped.is_empty() {
        let n: usize = unstamped.values().sum();
        lines.push(format!(
            "no pass owns {n} prop(s) (placed by hand, or before the stamps): {}",
            assets(&unstamped)
        ));
    }
    lines
}

/// A circuit with an elevation model and no dossier: its far field alone
/// ([`dress::lay_far_field`]), the rest of its scene left as authored, then
/// groomed as `ats-groom` grooms it (its props re-seated on the ground the
/// model shapes).
fn far_field_only(
    track_path: &std::path::Path,
    dry_run: bool,
    no_groom: bool,
) -> Result<String, String> {
    let opened = project::open_project(track_path).map_err(|e| e.to_string())?;
    let mut scene = opened.scene.ok_or("scene failed to load")?;
    let dem = track_core::dem::load_dem(track_core::dem::dem_path_for(track_path))
        .map_err(|e| format!("elevation sidecar unusable ({e})"))?
        .ok_or("no elevation sidecar")?;
    let n = dress::lay_far_field(&opened.track, &mut scene, &dem).ok_or("degenerate centerline")?;
    let groomed = if no_groom {
        String::new()
    } else {
        let g = groom::groom_scene_with_dem(&opened.track, &mut scene, None, Some(&dem))
            .ok_or("grooming failed")?;
        format!(
            "; groom: {} wall(s), {} board(s), {} barrier(s), {} tree(s), {} pushed, {} reseated",
            g.walls, g.boards, g.barriers, g.trees, g.pushed, g.reseated
        )
    };
    if !dry_run {
        ats_io::save_ats(&opened.ats_path, &scene).map_err(|e| format!("failed to save: {e}"))?;
    }
    Ok(format!(
        "no dossier: the far field alone, {n} impostor(s){groomed}{}",
        if dry_run { " (dry run)" } else { "" }
    ))
}

fn main() -> ExitCode {
    let mut all = false;
    let mut dry_run = false;
    let mut no_groom = false;
    let mut verbose = false;
    let mut explain = false;
    let mut tracks: Vec<PathBuf> = Vec::new();

    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--dry-run" | "-n" => dry_run = true,
            "--no-groom" => no_groom = true,
            "--verbose" | "-v" => verbose = true,
            "--explain" | "-e" => explain = true,
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
        match ue_export_io::all_track_files() {
            Ok(found) => tracks.extend(found.into_iter().filter(|t| {
                layout::layout_path_for(t).exists() || track_core::dem::dem_path_for(t).exists()
            })),
            Err(e) => {
                eprintln!("failed to list the track folders: {e}");
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
        match ats_io::imported_marker(track_path) {
            Ok(Some(by)) => {
                println!("{name}: skipped (imported by {by}; see docs/ac-import.md)");
                continue;
            }
            Ok(None) => {}
            Err(e) => {
                eprintln!("{name}: {e}");
                failures += 1;
                continue;
            }
        }
        let layout_path = layout::layout_path_for(track_path);
        let layout = match layout::load_layout(&layout_path) {
            Ok(Some(layout)) => layout,
            Ok(None) if track_core::dem::dem_path_for(track_path).exists() => {
                // No dossier, but an elevation model: the far field alone.
                match far_field_only(track_path, dry_run, no_groom) {
                    Ok(line) => println!("{name}: {line}"),
                    Err(e) => {
                        eprintln!("{name}: {e}");
                        failures += 1;
                    }
                }
                continue;
            }
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
        // The elevation model plants the woodland on the slopes; a track
        // without one is dressed exactly as before.
        let dem = match track_core::dem::load_dem(track_core::dem::dem_path_for(track_path)) {
            Ok(dem) => dem,
            Err(e) => {
                eprintln!("{name}: elevation sidecar unusable ({e}); dressing without it");
                None
            }
        };
        let Some(report) =
            dress::dress_scene_with_dem(&opened.track, &mut scene, &layout, dem.as_ref())
        else {
            eprintln!("{name}: degenerate centerline, skipping");
            failures += 1;
            continue;
        };
        let groomed = if no_groom {
            None
        } else {
            match groom::groom_scene_with_dem(
                &opened.track,
                &mut scene,
                Some(&layout),
                dem.as_ref(),
            ) {
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
             {} surrounding(s), {} kerb piece(s), {} graffiti, pit lane {} ({} old prop(s) replaced){}",
            report.stands,
            report.stand_props,
            report.buildings,
            report.bridges,
            report.landmarks,
            report.surroundings,
            report.kerb_pieces,
            report.graffiti,
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
        if verbose || explain {
            for note in &report.skipped {
                println!("    skipped {note}");
            }
            for note in &report.checks {
                println!("    check {note}");
            }
        }
        if explain {
            for line in explain_scene(&scene) {
                println!("    {line}");
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
