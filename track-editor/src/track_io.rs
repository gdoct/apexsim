//! Load/save for the logical `track.yaml` (or `.json`) source layer.

use std::fs;
use std::path::Path;

use crate::track_data::TrackFile;

#[derive(Debug, thiserror::Error)]
pub enum TrackIoError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("YAML parse error: {0}")]
    Yaml(#[from] serde_yaml::Error),
    #[error("JSON parse error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("invalid track data: {0}")]
    Invalid(String),
}

/// Load a logical track from `path`. Format (YAML vs JSON) is detected the
/// same way `apexsim_server::track_loader::TrackLoader` detects it: content
/// starting with `{` is parsed as JSON, everything else as YAML.
pub fn load_track_file<P: AsRef<Path>>(path: P) -> Result<TrackFile, TrackIoError> {
    let content = fs::read_to_string(path.as_ref())?;
    let track = parse_track_file(&content)?;
    validate(&track)?;
    Ok(track)
}

pub fn parse_track_file(content: &str) -> Result<TrackFile, TrackIoError> {
    if content.trim_start().starts_with('{') {
        Ok(serde_json::from_str(content)?)
    } else {
        Ok(serde_yaml::from_str(content)?)
    }
}

/// Save a logical track to `path`. Format follows the file extension
/// (`.json` writes JSON, anything else writes YAML) so a `.yaml` source
/// round-trips as YAML and doesn't silently flip formats on save.
pub fn save_track_file<P: AsRef<Path>>(path: P, track: &TrackFile) -> Result<(), TrackIoError> {
    validate(track)?;
    let path = path.as_ref();
    let serialized = if path.extension().and_then(|e| e.to_str()) == Some("json") {
        serde_json::to_string_pretty(track)?
    } else {
        serde_yaml::to_string(track)?
    };
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    fs::write(path, serialized)?;
    Ok(())
}

fn validate(track: &TrackFile) -> Result<(), TrackIoError> {
    if track.nodes.len() < 2 {
        return Err(TrackIoError::Invalid(
            "track must have at least 2 nodes".to_string(),
        ));
    }
    if track.default_width <= 0.0 && track.nodes.iter().all(|n| n.width.is_none()) {
        return Err(TrackIoError::Invalid(
            "track must have a default_width or per-node width values".to_string(),
        ));
    }
    for cp in &track.checkpoints {
        if cp.index_start >= track.nodes.len() || cp.index_end >= track.nodes.len() {
            return Err(TrackIoError::Invalid(format!(
                "checkpoint indices out of bounds: start={}, end={}, nodes={}",
                cp.index_start,
                cp.index_end,
                track.nodes.len()
            )));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{Checkpoint, TrackNode};

    fn minimal_track() -> TrackFile {
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

    #[test]
    fn round_trips_yaml_through_a_temp_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.yaml");
        let track = minimal_track();

        save_track_file(&path, &track).unwrap();
        let loaded = load_track_file(&path).unwrap();

        assert_eq!(track, loaded);
    }

    #[test]
    fn round_trips_json_through_a_temp_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.json");
        let track = minimal_track();

        save_track_file(&path, &track).unwrap();
        let loaded = load_track_file(&path).unwrap();

        assert_eq!(track, loaded);
    }

    #[test]
    fn rejects_fewer_than_two_nodes() {
        let mut track = minimal_track();
        track.nodes.truncate(1);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.yaml");

        assert!(save_track_file(&path, &track).is_err());
    }

    #[test]
    fn rejects_out_of_bounds_checkpoint() {
        let mut track = minimal_track();
        track.checkpoints.push(Checkpoint {
            index_start: 5,
            index_end: 6,
        });
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.yaml");

        assert!(save_track_file(&path, &track).is_err());
    }
}
