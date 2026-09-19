//! Writing `.uescene.json` exports (and the server's sidecars) to disk.
//!
//! Exports are *generated* files — the `.ats` and the `.yaml` remain the
//! only sources of truth — so they land in their own directory rather than
//! as siblings of the content they were baked from, and that directory is
//! gitignored. A full circuit bakes to a few megabytes of vertex data;
//! committing 26 of those would dwarf the content it came from.
//!
//! The exceptions are the server's sidecars, `<Track>.ground.msgpack` and
//! `<Track>.curbs.msgpack` and `<Track>.walls.msgpack`: the server reads them from beside the YAML
//! (they are the sim's ground and track limits, not client content), so
//! they are written there — and gitignored there, since they are generated
//! all the same.

use std::fs;
use std::path::{Path, PathBuf};

use crate::project;
use crate::terrain::GroundHeightfield;
use crate::ue_export::{self, CurbBands, UeScene, Walls};

/// Where exports go when no destination is given, relative to the repo root.
pub const DEFAULT_EXPORT_DIR: &str = "content/tracks/export";

#[derive(Debug, thiserror::Error)]
pub enum UeExportError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("MessagePack error: {0}")]
    MsgPack(#[from] rmp_serde::encode::Error),
    #[error("{0}")]
    Project(String),
    #[error("track {0} has no usable centerline")]
    Degenerate(String),
    #[error("elevation sidecar: {0}")]
    Dem(#[from] crate::dem::DemError),
}

/// `Monza.yaml` -> `<dir>/Monza.uescene.json`.
pub fn export_path_for(dir: &Path, track_path: &Path) -> PathBuf {
    let stem = track_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "Track".to_string());
    dir.join(format!("{stem}.uescene.json"))
}

/// `Monza.yaml` -> `<same dir>/Monza.ground.msgpack`.
pub fn ground_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("ground.msgpack")
}

/// `Monza.yaml` -> `<same dir>/Monza.curbs.msgpack`.
pub fn curb_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("curbs.msgpack")
}

/// `Monza.yaml` -> `<same dir>/Monza.walls.msgpack`.
pub fn walls_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("walls.msgpack")
}

/// What one track's export wrote.
pub struct Exported {
    pub scene_path: PathBuf,
    /// `None` for a track too degenerate to have a terrain.
    pub ground_path: Option<PathBuf>,
    /// `None` for a track with no usable centerline length.
    pub curb_path: Option<PathBuf>,
    /// The walls, always written (a track with nothing to hit gets an
    /// empty file, which the server reads as such).
    pub walls_path: PathBuf,
}

/// Bake the track at `track_path` (plus its sibling `.ats`) into `dir`, and
/// its server sidecars next to the YAML.
///
/// A track whose `.ats` is missing or unreadable still exports: the scene
/// layers are simply empty, and you get the bare road ribbon. That is worth
/// having — it is the difference between "the circuit is drivable in Unreal"
/// and "nothing loads".
pub fn export_track(track_path: &Path, dir: &Path) -> Result<Exported, UeExportError> {
    let opened = project::open_project(track_path).map_err(UeExportError::Project)?;
    let scene = opened.scene.unwrap_or_else(|| {
        crate::ats::AtsScene::new_for_track(
            &opened.track,
            &track_path.file_name().unwrap_or_default().to_string_lossy(),
        )
    });

    // The elevation sidecar is what gives the circuit its real ground and
    // its skyline. A track without one is baked exactly as before, and an
    // unreadable one is reported rather than silently ignored: a horizon
    // that quietly stops appearing is the kind of regression nobody
    // notices until a screenshot.
    let dem_path = crate::dem::dem_path_for(track_path);
    let dem = crate::dem::load_dem(&dem_path).map_err(UeExportError::Dem)?;

    let baked = ue_export::bake_all_with_dem(&opened.track, &scene, dem.as_ref())
        .ok_or_else(|| UeExportError::Degenerate(opened.track.name.clone()))?;
    let scene_path = export_path_for(dir, track_path);
    write_scene(&scene_path, &baked.scene)?;
    let ground_path = match &baked.ground {
        Some(ground) => {
            let path = ground_sidecar_path_for(track_path);
            write_ground_sidecar(&path, ground)?;
            Some(path)
        }
        None => None,
    };
    let curb_path = match &baked.curbs {
        Some(curbs) => {
            let path = curb_sidecar_path_for(track_path);
            write_curb_sidecar(&path, curbs)?;
            Some(path)
        }
        None => None,
    };
    let walls_path = walls_sidecar_path_for(track_path);
    write_walls_sidecar(&walls_path, &baked.walls)?;
    Ok(Exported {
        scene_path,
        ground_path,
        curb_path,
        walls_path,
    })
}

/// The server's ground heightfield, `rmp_serde::to_vec_named`, written
/// temp-then-rename like everything else.
pub fn write_ground_sidecar(path: &Path, ground: &GroundHeightfield) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(ground)?)
}

fn write_msgpack(path: &Path, serialized: &[u8]) -> Result<(), UeExportError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    let tmp = path.with_extension("msgpack.tmp");
    fs::write(&tmp, serialized)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

/// The server's curb bands, written the same way as the ground sidecar.
pub fn write_curb_sidecar(path: &Path, curbs: &CurbBands) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(curbs)?)
}

/// The server's walls, written the same way.
pub fn write_walls_sidecar(path: &Path, walls: &Walls) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(walls)?)
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
