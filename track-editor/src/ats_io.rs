//! Load/save for `.ats` (Apex Track Scene) files.
//!
//! The `.ats` sits next to its source `track.yaml` (same stem, `.ats`
//! extension) and is the only file the editor ever writes. Saves are
//! write-temp-then-rename so a failed save leaves the previous file
//! untouched, and serialization is plain `serde_json` pretty output over
//! `Vec`-backed data — repeated saves of unchanged data are byte-identical.

use std::fs;
use std::path::{Path, PathBuf};

use crate::ats::AtsScene;

#[derive(Debug, thiserror::Error)]
pub enum AtsIoError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON parse error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("invalid scene data: {0}")]
    Invalid(String),
}

/// The `.ats` path belonging to a source track file:
/// `content/tracks/real/Silverstone.yaml` -> `content/tracks/real/Silverstone.ats`.
pub fn ats_path_for<P: AsRef<Path>>(track_path: P) -> PathBuf {
    track_path.as_ref().with_extension("ats")
}

pub fn load_ats<P: AsRef<Path>>(path: P) -> Result<AtsScene, AtsIoError> {
    let content = fs::read_to_string(path.as_ref())?;
    let scene: AtsScene = serde_json::from_str(&content)?;
    scene.validate().map_err(AtsIoError::Invalid)?;
    Ok(scene)
}

pub fn save_ats<P: AsRef<Path>>(path: P, scene: &AtsScene) -> Result<(), AtsIoError> {
    scene.validate().map_err(AtsIoError::Invalid)?;
    let path = path.as_ref();
    let mut serialized = serde_json::to_string_pretty(scene)?;
    serialized.push('\n');

    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    // Write to a sibling temp file first: if serialization or the write
    // fails midway, the existing .ats stays intact. `fs::rename` replaces
    // the destination on both Unix and Windows.
    let tmp = path.with_extension("ats.tmp");
    fs::write(&tmp, &serialized)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ats::{Curb, PitLane, Prop, PropKind, Side};
    use crate::track_data::{TrackFile, TrackNode};

    fn test_track() -> TrackFile {
        TrackFile {
            name: "Test".to_string(),
            track_id: None,
            nodes: vec![
                TrackNode {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                    width: None,
                    width_left: None,
                    width_right: None,
                    banking: None,
                    friction: None,
                    surface_type: None,
                },
                TrackNode {
                    x: 100.0,
                    y: 0.0,
                    z: 0.0,
                    width: None,
                    width_left: None,
                    width_right: None,
                    banking: None,
                    friction: None,
                    surface_type: None,
                },
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: false,
            raceline: vec![],
            metadata: None,
        }
    }

    fn populated_scene() -> AtsScene {
        let mut scene = AtsScene::new_for_track(&test_track(), "Test.yaml");
        let id = scene.alloc_id();
        scene.curbs.push(Curb {
            id,
            side: Side::Left,
            start_m: 20.0,
            end_m: 60.0,
            width_m: 1.2,
            style: "red_white".to_string(),
        });
        let id = scene.alloc_id();
        scene.props.push(Prop {
            id,
            kind: PropKind::Sign,
            asset: "board_100m".to_string(),
            x: 50.0,
            y: 8.0,
            z: 0.0,
            yaw_rad: 1.2,
            scale: 1.0,
            text: Some("100".to_string()),
        });
        scene.pit_lane = Some(PitLane {
            nodes: vec![[0.0, -10.0, 0.0], [80.0, -10.0, 0.0]],
            width_m: 8.0,
            box_count: 10,
            speed_limit_kmh: 80.0,
        });
        scene
    }

    #[test]
    fn ats_path_sits_next_to_the_yaml() {
        assert_eq!(
            ats_path_for("content/tracks/real/Silverstone.yaml"),
            PathBuf::from("content/tracks/real/Silverstone.ats")
        );
    }

    #[test]
    fn round_trips_through_a_temp_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Test.ats");
        let scene = populated_scene();

        save_ats(&path, &scene).unwrap();
        let loaded = load_ats(&path).unwrap();
        assert_eq!(scene, loaded);
    }

    #[test]
    fn repeated_saves_are_byte_identical() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Test.ats");
        let scene = populated_scene();

        save_ats(&path, &scene).unwrap();
        let first = fs::read(&path).unwrap();
        save_ats(&path, &scene).unwrap();
        let second = fs::read(&path).unwrap();
        assert_eq!(first, second);
    }

    #[test]
    fn load_rejects_foreign_json() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Test.ats");
        fs::write(&path, "{\"hello\": 1}").unwrap();
        assert!(load_ats(&path).is_err());
    }

    #[test]
    fn failed_save_leaves_the_previous_file_untouched() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Test.ats");
        let scene = populated_scene();
        save_ats(&path, &scene).unwrap();
        let before = fs::read(&path).unwrap();

        let mut broken = scene.clone();
        broken.curbs[0].width_m = -1.0; // fails validation
        assert!(save_ats(&path, &broken).is_err());
        assert_eq!(fs::read(&path).unwrap(), before);
    }
}
