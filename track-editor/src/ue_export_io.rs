//! Writing `.uescene.json` exports to disk.
//!
//! Exports are *generated* files — the `.ats` and the `.yaml` remain the
//! only sources of truth — so they land in their own directory rather than
//! as siblings of the content they were baked from, and that directory is
//! gitignored. A full circuit bakes to a few megabytes of vertex data;
//! committing 26 of those would dwarf the content it came from.

use std::fs;
use std::path::{Path, PathBuf};

use crate::project;
use crate::ue_export::{self, UeScene};

/// Where exports go when no destination is given, relative to the repo root.
pub const DEFAULT_EXPORT_DIR: &str = "content/tracks/export";

#[derive(Debug, thiserror::Error)]
pub enum UeExportError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("{0}")]
    Project(String),
    #[error("track {0} has no usable centerline")]
    Degenerate(String),
}

/// `Monza.yaml` -> `<dir>/Monza.uescene.json`.
pub fn export_path_for(dir: &Path, track_path: &Path) -> PathBuf {
    let stem = track_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "Track".to_string());
    dir.join(format!("{stem}.uescene.json"))
}

/// Bake the track at `track_path` (plus its sibling `.ats`) into `dir`.
///
/// A track whose `.ats` is missing or unreadable still exports: the scene
/// layers are simply empty, and you get the bare road ribbon. That is worth
/// having — it is the difference between "the circuit is drivable in Unreal"
/// and "nothing loads".
pub fn export_track(track_path: &Path, dir: &Path) -> Result<PathBuf, UeExportError> {
    let opened = project::open_project(track_path).map_err(UeExportError::Project)?;
    let scene = opened.scene.unwrap_or_else(|| {
        crate::ats::AtsScene::new_for_track(
            &opened.track,
            &track_path.file_name().unwrap_or_default().to_string_lossy(),
        )
    });

    let baked = ue_export::bake(&opened.track, &scene)
        .ok_or_else(|| UeExportError::Degenerate(opened.track.name.clone()))?;
    let out = export_path_for(dir, track_path);
    write_scene(&out, &baked)?;
    Ok(out)
}

/// Compact JSON, written temp-then-rename like the `.ats` saver, so an
/// interrupted export never leaves a half-written file for the commandlet
/// to choke on. Compact rather than pretty: this file is machine-read, and
/// pretty-printing a few hundred thousand floats triples it for nothing.
pub fn write_scene(path: &Path, scene: &UeScene) -> Result<(), UeExportError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    let serialized = serde_json::to_vec(scene)?;
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, &serialized)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

pub fn read_scene(path: &Path) -> Result<UeScene, UeExportError> {
    let bytes = fs::read(path)?;
    Ok(serde_json::from_slice(&bytes)?)
}

/// Every `*.yaml` in `dir`, sorted, so a batch export is reproducible.
pub fn track_files_in(dir: &Path) -> Result<Vec<PathBuf>, UeExportError> {
    let mut found: Vec<PathBuf> = fs::read_dir(dir)?
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| {
            p.extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("yaml"))
        })
        .collect();
    found.sort();
    Ok(found)
}
