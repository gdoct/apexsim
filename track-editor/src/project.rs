//! Opening a track "project": the read-only source `track.yaml` plus its
//! editable `.ats` scene sidecar. Shared by the GUI file dialog and the MCP
//! `open_track` tool so both follow the same rules:
//!
//! - the YAML is loaded read-only, never written;
//! - an existing sibling `.ats` is loaded;
//! - a missing `.ats` gets a fresh in-memory scene (saved on first save);
//! - a corrupt `.ats` is reported and left untouched — the track still
//!   opens, but with no scene, so the broken file can't be clobbered.

use std::path::{Path, PathBuf};

use crate::ats::AtsScene;
use crate::ats_io;
use crate::track_data::TrackFile;
use crate::track_io;

pub struct OpenedProject {
    pub track: TrackFile,
    pub track_path: PathBuf,
    pub ats_path: PathBuf,
    /// `None` if an existing `.ats` failed to load (see `message`).
    pub scene: Option<AtsScene>,
    /// True when the scene was freshly created and isn't on disk yet.
    pub dirty: bool,
    /// Human-readable summary of what happened, for the status line.
    pub message: String,
}

pub fn open_project(path: &Path) -> Result<OpenedProject, String> {
    let track =
        track_io::load_track_file(path).map_err(|e| format!("failed to load track: {e}"))?;

    let ats_path = ats_io::ats_path_for(path);
    let source_name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();

    let (scene, scene_status, dirty) = if ats_path.exists() {
        match ats_io::load_ats(&ats_path) {
            Ok(scene) => {
                let note = if scene.source_track != source_name {
                    format!(" (warning: scene was created for {:?})", scene.source_track)
                } else {
                    String::new()
                };
                (
                    Some(scene),
                    format!("Loaded scene {}{note}", ats_path.display()),
                    false,
                )
            }
            Err(e) => (
                None,
                format!(
                    "Track loaded, but {} is unreadable: {e}. Fix or delete it, then reopen.",
                    ats_path.display()
                ),
                false,
            ),
        }
    } else {
        (
            Some(AtsScene::new_for_track(&track, &source_name)),
            format!("New scene (unsaved) → {}", ats_path.display()),
            true,
        )
    };

    Ok(OpenedProject {
        message: format!(
            "Loaded \"{}\" ({} nodes). {scene_status}",
            track.name,
            track.nodes.len()
        ),
        track,
        track_path: path.to_path_buf(),
        ats_path,
        scene,
        dirty,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    const MINIMAL_YAML: &str = "\
name: Mini
nodes:
  - { x: 0.0, y: 0.0 }
  - { x: 100.0, y: 0.0 }
default_width: 10.0
closed_loop: false
";

    #[test]
    fn missing_ats_yields_a_fresh_unsaved_scene() {
        let dir = tempfile::tempdir().unwrap();
        let yaml = dir.path().join("Mini.yaml");
        fs::write(&yaml, MINIMAL_YAML).unwrap();

        let project = open_project(&yaml).unwrap();
        assert!(project.dirty);
        assert!(project.scene.is_some());
        assert_eq!(project.ats_path, dir.path().join("Mini.ats"));
        assert!(!project.ats_path.exists(), "opening must not write files");
    }

    #[test]
    fn existing_ats_is_loaded_not_recreated() {
        let dir = tempfile::tempdir().unwrap();
        let yaml = dir.path().join("Mini.yaml");
        fs::write(&yaml, MINIMAL_YAML).unwrap();

        let first = open_project(&yaml).unwrap();
        let mut scene = first.scene.unwrap();
        let id = scene.alloc_id();
        scene.props.push(crate::ats::Prop {
            id,
            kind: crate::ats::PropKind::Tree,
            asset: "tree_generic".to_string(),
            x: 5.0,
            y: 5.0,
            z: 0.0,
            yaw_rad: 0.0,
            scale: 1.0,
            text: None,
        });
        ats_io::save_ats(&first.ats_path, &scene).unwrap();

        let second = open_project(&yaml).unwrap();
        assert!(!second.dirty);
        assert_eq!(second.scene.unwrap().props.len(), 1);
    }

    #[test]
    fn corrupt_ats_opens_the_track_without_a_scene() {
        let dir = tempfile::tempdir().unwrap();
        let yaml = dir.path().join("Mini.yaml");
        fs::write(&yaml, MINIMAL_YAML).unwrap();
        let ats = dir.path().join("Mini.ats");
        fs::write(&ats, "not json at all").unwrap();
        let bytes_before = fs::read(&ats).unwrap();

        let project = open_project(&yaml).unwrap();
        assert!(project.scene.is_none());
        assert!(project.message.contains("unreadable"));
        assert_eq!(fs::read(&ats).unwrap(), bytes_before);
    }
}
