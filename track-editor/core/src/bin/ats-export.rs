//! Batch-bake tracks into the `.uescene.json` manifest and `.uemesh` mesh
//! blob the Unreal client builds each circuit from at runtime, plus the
//! `.ground.msgpack` heightfield, `.curbs.msgpack` track limits,
//! `.walls.msgpack` barriers and `.road.msgpack` road mesh the server
//! reads from beside each YAML.
//!
//! ```text
//! ats-export --all                          # every track under content/tracks/real
//! ats-export content/tracks/real/Monza.yaml # one track
//! ats-export --all --out some/other/dir
//! ats-export --keep-sidecars road,walls content/tracks/real/X.yaml
//! ```
//!
//! A track whose `.ats` lists `external_sidecars` (an imported circuit whose
//! road, walls or ground were measured by its importer) keeps those files
//! on every run, `--all` included.
//!
//! Kept as a separate binary from the editor GUI so exporting can run in CI
//! and from a build script without opening a window.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_core::ats::Sidecar;
use track_core::ue_export_io::{self, ExportOptions, DEFAULT_EXPORT_DIR};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut all = false;
    let mut out_dir: Option<PathBuf> = None;
    let mut tracks: Vec<PathBuf> = Vec::new();
    let mut options = ExportOptions::default();
    let mut iter = args.iter();

    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--all" | "-a" => all = true,
            "--flat-curbs" => options.bake.flat_curbs = true,
            "--keep-sidecars" => {
                let Some(list) = iter.next() else {
                    eprintln!("--keep-sidecars needs a list (ground,curbs,walls,road or all)");
                    return ExitCode::FAILURE;
                };
                for key in list.split(',').map(str::trim).filter(|k| !k.is_empty()) {
                    if key == "all" {
                        options.keep_sidecars.extend(Sidecar::ALL);
                    } else if let Some(sidecar) = Sidecar::from_key(key) {
                        options.keep_sidecars.push(sidecar);
                    } else {
                        eprintln!("unknown sidecar {key:?} (ground, curbs, walls, road, all)");
                        return ExitCode::FAILURE;
                    }
                }
            }
            "--out" | "-o" => match iter.next() {
                Some(dir) => out_dir = Some(PathBuf::from(dir)),
                None => {
                    eprintln!("--out needs a directory");
                    return ExitCode::FAILURE;
                }
            },
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

    let out_dir = out_dir.unwrap_or_else(|| PathBuf::from(DEFAULT_EXPORT_DIR));
    let mut failures = 0usize;
    for track in &tracks {
        match ue_export_io::export_track_with_options(track, &out_dir, &options) {
            Ok(exported) => {
                let size_kb = |p: &Path| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0) / 1024;
                println!(
                    "{} -> {} ({} KB) + {} ({} KB)",
                    track.display(),
                    exported.scene_path.display(),
                    size_kb(&exported.scene_path),
                    exported
                        .mesh_blob_path
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy(),
                    size_kb(&exported.mesh_blob_path)
                );
                let sidecars = [
                    (Sidecar::Ground, exported.ground_path.as_ref()),
                    (Sidecar::Curbs, exported.curb_path.as_ref()),
                    (Sidecar::Walls, Some(&exported.walls_path)),
                    (Sidecar::Road, Some(&exported.road_path)),
                ];
                for (sidecar, path) in sidecars {
                    let Some(path) = path else { continue };
                    if exported.kept.contains(&sidecar) {
                        // The importer's file, reported as it is on disk.
                        if path.exists() {
                            println!(
                                "{} -> {} ({} KB, kept: written by another tool)",
                                track.display(),
                                path.display(),
                                size_kb(path)
                            );
                        } else {
                            println!(
                                "{} -> {} MISSING (kept for another tool, which has not written it; the server runs without it)",
                                track.display(),
                                path.display()
                            );
                        }
                    } else if sidecar != Sidecar::Road {
                        println!(
                            "{} -> {} ({} KB)",
                            track.display(),
                            path.display(),
                            size_kb(path)
                        );
                    }
                }
                if exported.kept.contains(&Sidecar::Road) {
                    continue;
                }
                println!(
                    "{} -> {} ({} KB, {} triangles{}{})",
                    track.display(),
                    exported.road_path.display(),
                    size_kb(&exported.road_path),
                    exported.road_triangles,
                    if exported.road_dropped > 0 {
                        format!(
                            ", {} facets without footprint dropped",
                            exported.road_dropped
                        )
                    } else {
                        String::new()
                    },
                    if options.bake.flat_curbs {
                        ", flat curbs"
                    } else {
                        ""
                    }
                );
            }
            Err(e) => {
                eprintln!("{}: {e}", track.display());
                failures += 1;
            }
        }
    }

    println!(
        "exported {}/{} track(s) to {}",
        tracks.len() - failures,
        tracks.len(),
        out_dir.display()
    );
    if failures > 0 {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

const USAGE: &str = "\
usage: ats-export [--all] [--out DIR] [--flat-curbs] [--keep-sidecars LIST] [TRACK.yaml ...]

  --all, -a      export every *.yaml under content/tracks/real
  --out, -o DIR  destination for the .uescene.json manifest and the .uemesh mesh blob
                 beside it (default: content/tracks/export); the .ground.msgpack,
                 .curbs.msgpack, .walls.msgpack and .road.msgpack sidecars always land
                 beside the YAML
  --flat-curbs   bake the curbs into the road mesh flat at the road edge's height
                 (the rendered curbs keep their profile), to compare the mesh sim
                 against the centerline one without the curbs' shape in the way
  --keep-sidecars LIST
                 leave these sidecars as they are on disk instead of baking them
                 (comma list of ground, curbs, walls, road, or all). A track's .ats
                 can declare the same for itself in \"external_sidecars\", which
                 every run honours
";
