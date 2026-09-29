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
    if let Some(&idx) = track.sectors.iter().find(|&&i| i >= track.nodes.len()) {
        return Err(TrackIoError::Invalid(format!(
            "sector index out of bounds: {idx}, nodes={}",
            track.nodes.len()
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{Checkpoint, TrackNode};

    /// `scripts/track_location.py` writes the location at the end of the
    /// metadata block in an f32's shortest form; a tool that rewrites the
    /// YAML (ats-smooth, ats-bank) must write the same lines back.
    #[test]
    fn the_location_survives_a_rewrite_as_written() {
        let dir =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../content/tracks/default");
        let mut seen = 0;
        for entry in std::fs::read_dir(&dir).expect("tracks") {
            let path = entry.expect("entry").path();
            if path.extension().and_then(|e| e.to_str()) != Some("yaml") {
                continue;
            }
            let text = std::fs::read_to_string(&path).expect("yaml");
            let lines: Vec<&str> = text
                .lines()
                .filter(|l| {
                    l.starts_with("  altitude_m:")
                        || l.starts_with("  latitude_deg:")
                        || l.starts_with("  longitude_deg:")
                })
                .collect();
            if lines.is_empty() {
                continue;
            }
            let track = parse_track_file(&text).expect("parses");
            let written = serde_yaml::to_string(&track).expect("writes");
            for line in &lines {
                assert!(
                    written.lines().any(|w| w == *line),
                    "{}: {line} comes back as something else",
                    path.display()
                );
            }
            seen += 1;
        }
        assert!(seen > 20, "only {seen} tracks carry a location");
    }

    fn minimal_track() -> TrackFile {
        TrackFile {
            name: "Test".to_string(),
            display_name: None,
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
            sectors: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: false,
            raceline: vec![],
            drs_zones: Vec::new(),
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
    fn sectors_survive_a_rewrite_and_stay_out_of_a_file_without_them() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.yaml");

        let plain = minimal_track();
        save_track_file(&path, &plain).unwrap();
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(!text.contains("sectors"), "no empty key written:\n{text}");

        let mut timed = minimal_track();
        timed.sectors = vec![0, 1];
        save_track_file(&path, &timed).unwrap();
        assert_eq!(load_track_file(&path).unwrap().sectors, vec![0, 1]);
    }

    #[test]
    fn rejects_out_of_bounds_sector() {
        let mut track = minimal_track();
        track.sectors = vec![1, 7];
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.yaml");

        assert!(save_track_file(&path, &track).is_err());
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
