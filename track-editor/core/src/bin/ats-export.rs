//! Batch-bake tracks into the `.uescene.json` manifest and `.uemesh` mesh
//! blob the Unreal `ApexTrackImport` commandlet consumes, plus the
//! `.ground.msgpack` heightfield and
//! `.curbs.msgpack` track limits and `.walls.msgpack` barriers the server
//! reads from beside each YAML.
//!
//! ```text
//! ats-export --all                          # every track under content/tracks/real
//! ats-export content/tracks/real/Monza.yaml # one track
//! ats-export --all --out some/other/dir
//! ```
//!
//! Kept as a separate binary from the editor GUI so exporting can run in CI
//! and from a build script without opening a window.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use track_core::ue_export_io::{self, DEFAULT_EXPORT_DIR};

const DEFAULT_TRACK_DIR: &str = "content/tracks/real";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut all = false;
    let mut out_dir: Option<PathBuf> = None;
    let mut tracks: Vec<PathBuf> = Vec::new();
    let mut iter = args.iter();

    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--all" | "-a" => all = true,
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
        match ue_export_io::export_track(track, &out_dir) {
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
                for sidecar in exported
                    .ground_path
                    .iter()
                    .chain(exported.curb_path.iter())
                    .chain(std::iter::once(&exported.walls_path))
                {
                    println!(
                        "{} -> {} ({} KB)",
                        track.display(),
                        sidecar.display(),
                        size_kb(sidecar)
                    );
                }
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
usage: ats-export [--all] [--out DIR] [TRACK.yaml ...]

  --all, -a      export every *.yaml under content/tracks/real
  --out, -o DIR  destination for the .uescene.json manifest and the .uemesh mesh blob
                 beside it (default: content/tracks/export); the .ground.msgpack,
                 .curbs.msgpack and .walls.msgpack sidecars always land beside the YAML
";
