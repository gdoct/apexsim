use crate::data::{TrackConfig, TrackPoint, SurfaceType, TrackSurface, GridSlot, RacelinePoint, TrackMetadata};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackFileFormat {
    pub name: String,
    #[serde(default)]
    pub track_id: Option<String>,
    pub nodes: Vec<TrackNode>,
    #[serde(default)]
    pub checkpoints: Vec<Checkpoint>,
    #[serde(default)]
    pub spawn_points: Vec<SpawnPoint>,
    #[serde(default)]
    pub default_width: f32,
    #[serde(default)]
    pub closed_loop: bool,
    /// Optional raceline (optimal racing line) for AI and visualization
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    /// Track metadata
    #[serde(default)]
    pub metadata: Option<TrackMetadata>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackNode {
    pub x: f32,
    pub y: f32,
    #[serde(default)]
    pub z: f32,
    #[serde(default)]
    pub width: Option<f32>,
    #[serde(default)]
    pub width_left: Option<f32>,
    #[serde(default)]
    pub width_right: Option<f32>,
    #[serde(default)]
    pub banking: Option<f32>,
    #[serde(default)]
    pub friction: Option<f32>,
    #[serde(default)]
    pub surface_type: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Checkpoint {
    pub index_start: usize,
    pub index_end: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpawnPoint {
    pub position: usize,
    #[serde(default)]
    pub offset_x: f32,
    #[serde(default)]
    pub offset_y: f32,
}

#[derive(Debug)]
pub enum TrackLoadError {
    IoError(std::io::Error),
    ParseError(String),
    InvalidData(String),
}

impl From<std::io::Error> for TrackLoadError {
    fn from(err: std::io::Error) -> Self {
        TrackLoadError::IoError(err)
    }
}

impl std::fmt::Display for TrackLoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TrackLoadError::IoError(e) => write!(f, "IO error: {}", e),
            TrackLoadError::ParseError(e) => write!(f, "Parse error: {}", e),
            TrackLoadError::InvalidData(e) => write!(f, "Invalid data: {}", e),
        }
    }
}

impl std::error::Error for TrackLoadError {}

pub struct TrackLoader;

impl TrackLoader {
    pub fn load_from_file<P: AsRef<Path>>(path: P) -> Result<TrackConfig, TrackLoadError> {
        let content = fs::read_to_string(path)?;
        Self::load_from_string(&content)
    }

    pub fn load_from_string(content: &str) -> Result<TrackConfig, TrackLoadError> {
        let track_file: TrackFileFormat = if content.trim_start().starts_with('{') {
            serde_json::from_str(content)
                .map_err(|e| TrackLoadError::ParseError(format!("JSON parse error: {}", e)))?
        } else {
            serde_yaml::from_str(content)
                .map_err(|e| TrackLoadError::ParseError(format!("YAML parse error: {}", e)))?
        };

        Self::validate(&track_file)?;
        Self::build_track_config(track_file)
    }

    fn validate(track: &TrackFileFormat) -> Result<(), TrackLoadError> {
        if track.nodes.len() < 2 {
            return Err(TrackLoadError::InvalidData(
                "Track must have at least 2 nodes".to_string(),
            ));
        }

        if track.default_width <= 0.0 && track.nodes.iter().all(|n| n.width.is_none()) {
            return Err(TrackLoadError::InvalidData(
                "Track must have a default_width or per-node width values".to_string(),
            ));
        }

        for checkpoint in &track.checkpoints {
            if checkpoint.index_start >= track.nodes.len()
                || checkpoint.index_end >= track.nodes.len()
            {
                return Err(TrackLoadError::InvalidData(format!(
                    "Checkpoint indices out of bounds: start={}, end={}, nodes={}",
                    checkpoint.index_start,
                    checkpoint.index_end,
                    track.nodes.len()
                )));
            }
        }

        Ok(())
    }

    fn build_track_config(track_file: TrackFileFormat) -> Result<TrackConfig, TrackLoadError> {
        let default_width = if track_file.default_width > 0.0 {
            track_file.default_width
        } else {
            12.0
        };

        let centerline_points = SplineInterpolator::interpolate_spline(
            &track_file.nodes,
            track_file.closed_loop,
            default_width,
        )?;

        let start_positions = Self::generate_start_positions(&track_file, &centerline_points);

        // Use track_id from file if provided, otherwise generate new UUID
        let track_id = if let Some(track_id_str) = &track_file.track_id {
            uuid::Uuid::parse_str(track_id_str)
                .map_err(|e| TrackLoadError::InvalidData(format!("Invalid track_id format: {}", e)))?
        } else {
            uuid::Uuid::new_v4()
        };

        // Convert raceline points to the data structure
        let raceline = track_file.raceline.into_iter().map(|rl| {
            crate::data::RacelinePoint {
                x: rl.x,
                y: rl.y,
                z: rl.z,
            }
        }).collect();

        let metadata = track_file.metadata.unwrap_or_default();

        Ok(TrackConfig {
            id: track_id,
            name: track_file.name,
            centerline: centerline_points,
            width_m: default_width,
            source_path: None,
            start_positions,
            track_surface: TrackSurface {
                base_grip: 1.0,
                curb_grip: 0.85,
                off_track_grip: 0.6,
                off_track_speed_penalty: 0.8,
            },
            pit_lane: None,
            raceline,
            metadata,
        })
    }

    fn generate_start_positions(
        track_file: &TrackFileFormat,
        centerline: &[TrackPoint],
    ) -> Vec<GridSlot> {
        if !track_file.spawn_points.is_empty() {
            track_file
                .spawn_points
                .iter()
                .enumerate()
                .filter_map(|(idx, spawn)| {
                    if spawn.position < centerline.len() {
                        let point = &centerline[spawn.position];
                        Some(GridSlot {
                            position: idx as u8 + 1,
                            x: point.x + spawn.offset_x,
                            y: point.y + spawn.offset_y,
                            z: point.z,
                            yaw_rad: point.heading_rad,
                        })
                    } else {
                        None
                    }
                })
                .collect()
        } else {
            if centerline.is_empty() {
                return vec![];
            }

            let start_point = &centerline[0];
            let grid_spacing = 8.0;
            let lateral_spacing = 3.0;

            (0..16)
                .map(|i| {
                    let row = i / 2;
                    let column = i % 2;
                    let offset_forward = -(row as f32) * grid_spacing;
                    let offset_lateral = (column as f32 - 0.5) * lateral_spacing;

                    let cos_h = start_point.heading_rad.cos();
                    let sin_h = start_point.heading_rad.sin();

                    GridSlot {
                        position: i + 1,
                        x: start_point.x + offset_forward * cos_h - offset_lateral * sin_h,
                        y: start_point.y + offset_forward * sin_h + offset_lateral * cos_h,
                        z: start_point.z,
                        yaw_rad: start_point.heading_rad,
                    }
                })
                .collect()
        }
    }
}

pub struct SplineInterpolator;

impl SplineInterpolator {
    pub fn interpolate_spline(
        nodes: &[TrackNode],
        closed_loop: bool,
        default_width: f32,
    ) -> Result<Vec<TrackPoint>, TrackLoadError> {
        if nodes.len() < 2 {
            return Err(TrackLoadError::InvalidData(
                "Need at least 2 nodes for interpolation".to_string(),
            ));
        }

        let mut track_points = Vec::new();
        let points_per_segment = 20;

        for i in 0..nodes.len() {
            let p0_idx = if i == 0 && closed_loop {
                nodes.len() - 1
            } else if i == 0 {
                0
            } else {
                i - 1
            };

            let p1_idx = i;
            let p2_idx = (i + 1) % nodes.len();
            let p3_idx = if closed_loop {
                (i + 2) % nodes.len()
            } else {
                (i + 2).min(nodes.len() - 1)
            };

            if i == nodes.len() - 1 && !closed_loop {
                break;
            }

            let p0 = &nodes[p0_idx];
            let p1 = &nodes[p1_idx];
            let p2 = &nodes[p2_idx];
            let p3 = &nodes[p3_idx];

            for j in 0..points_per_segment {
                let t = j as f32 / points_per_segment as f32;
                let point = Self::catmull_rom_point(p0, p1, p2, p3, t);

                let (width_left, width_right) = if let (Some(wl), Some(wr)) = (p1.width_left, p1.width_right) {
                    (wl, wr)
                } else if let Some(w) = p1.width {
                    (w / 2.0, w / 2.0)
                } else {
                    (default_width / 2.0, default_width / 2.0)
                };

                let banking = p1.banking.unwrap_or(0.0);
                let friction = p1.friction.unwrap_or(1.0);

                let surface_type = Self::parse_surface_type(p1.surface_type.as_deref());

                track_points.push(TrackPoint {
                    x: point.0,
                    y: point.1,
                    z: point.2,
                    distance_from_start_m: 0.0,
                    width_left_m: width_left,
                    width_right_m: width_right,
                    banking_rad: banking,
                    camber_rad: 0.0,
                    slope_rad: 0.0,
                    heading_rad: 0.0,
                    surface_type,
                    grip_modifier: friction,
                });
            }
        }

        Self::compute_derived_properties(&mut track_points, closed_loop);

        Ok(track_points)
    }

    fn catmull_rom_point(
        p0: &TrackNode,
        p1: &TrackNode,
        p2: &TrackNode,
        p3: &TrackNode,
        t: f32,
    ) -> (f32, f32, f32) {
        let t2 = t * t;
        let t3 = t2 * t;

        let x = 0.5
            * ((2.0 * p1.x)
                + (-p0.x + p2.x) * t
                + (2.0 * p0.x - 5.0 * p1.x + 4.0 * p2.x - p3.x) * t2
                + (-p0.x + 3.0 * p1.x - 3.0 * p2.x + p3.x) * t3);

        let y = 0.5
            * ((2.0 * p1.y)
                + (-p0.y + p2.y) * t
                + (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2
                + (-p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3);

        let z = 0.5
            * ((2.0 * p1.z)
                + (-p0.z + p2.z) * t
                + (2.0 * p0.z - 5.0 * p1.z + 4.0 * p2.z - p3.z) * t2
                + (-p0.z + 3.0 * p1.z - 3.0 * p2.z + p3.z) * t3);

        (x, y, z)
    }

    fn compute_derived_properties(points: &mut [TrackPoint], closed_loop: bool) {
        if points.is_empty() {
            return;
        }

        let mut cumulative_distance = 0.0;
        points[0].distance_from_start_m = 0.0;

        for i in 1..points.len() {
            let dx = points[i].x - points[i - 1].x;
            let dy = points[i].y - points[i - 1].y;
            let dz = points[i].z - points[i - 1].z;
            let segment_length = (dx * dx + dy * dy + dz * dz).sqrt();
            cumulative_distance += segment_length;
            points[i].distance_from_start_m = cumulative_distance;
        }

        for i in 0..points.len() {
            let next_idx = if i == points.len() - 1 {
                if closed_loop {
                    0
                } else {
                    i
                }
            } else {
                i + 1
            };

            if next_idx != i {
                let dx = points[next_idx].x - points[i].x;
                let dy = points[next_idx].y - points[i].y;
                let dz = points[next_idx].z - points[i].z;
                let dist_2d = (dx * dx + dy * dy).sqrt();

                points[i].heading_rad = dy.atan2(dx);

                if dist_2d > 0.001 {
                    points[i].slope_rad = dz.atan2(dist_2d);
                }
            } else {
                if i > 0 {
                    points[i].heading_rad = points[i - 1].heading_rad;
                    points[i].slope_rad = points[i - 1].slope_rad;
                }
            }
        }
    }

    fn parse_surface_type(s: Option<&str>) -> SurfaceType {
        match s {
            Some("Asphalt") | Some("asphalt") => SurfaceType::Asphalt,
            Some("Curb") | Some("curb") => SurfaceType::Curb,
            Some("Grass") | Some("grass") => SurfaceType::Grass,
            Some("Gravel") | Some("gravel") => SurfaceType::Gravel,
            Some("Wet") | Some("wet") => SurfaceType::Wet,
            Some("Sand") | Some("sand") => SurfaceType::Sand,
            Some("Concrete") | Some("concrete") => SurfaceType::Concrete,
            _ => SurfaceType::Asphalt,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_simple_track_json() {
        let json = r#"{
            "name": "Test Track",
            "default_width": 10.0,
            "closed_loop": true,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0},
                {"x": 100.0, "y": 0.0, "z": 0.0},
                {"x": 100.0, "y": 100.0, "z": 0.0},
                {"x": 0.0, "y": 100.0, "z": 0.0}
            ]
        }"#;

        let track = TrackLoader::load_from_string(json).unwrap();
        assert_eq!(track.name, "Test Track");
        assert!(track.centerline.len() > 4);
        assert_eq!(track.width_m, 10.0);
    }

    #[test]
    fn test_load_simple_track_yaml() {
        let yaml = r#"
name: "Test Track"
default_width: 10.0
closed_loop: true
nodes:
  - x: 0.0
    y: 0.0
    z: 0.0
  - x: 100.0
    y: 0.0
    z: 0.0
  - x: 100.0
    y: 100.0
    z: 0.0
  - x: 0.0
    y: 100.0
    z: 0.0
"#;

        let track = TrackLoader::load_from_string(yaml).unwrap();
        assert_eq!(track.name, "Test Track");
        assert!(track.centerline.len() > 4);
    }

    #[test]
    fn test_track_with_banking_and_friction() {
        let json = r#"{
            "name": "Banked Track",
            "default_width": 12.0,
            "closed_loop": false,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0, "banking": 0.0, "friction": 1.0},
                {"x": 100.0, "y": 0.0, "z": 5.0, "banking": 0.1, "friction": 0.9}
            ]
        }"#;

        let track = TrackLoader::load_from_string(json).unwrap();
        assert!(track.centerline.len() >= 2);
    }

    #[test]
    fn test_invalid_track_too_few_nodes() {
        let json = r#"{
            "name": "Invalid",
            "default_width": 10.0,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0}
            ]
        }"#;

        let result = TrackLoader::load_from_string(json);
        assert!(result.is_err());
    }

    #[test]
    fn test_catmull_rom_interpolation() {
        let nodes = vec![
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
                x: 10.0,
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
                x: 20.0,
                y: 10.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
        ];

        let points = SplineInterpolator::interpolate_spline(&nodes, false, 10.0).unwrap();
        assert!(points.len() > 3);
        assert!(points[0].distance_from_start_m == 0.0);
        assert!(points.last().unwrap().distance_from_start_m > 0.0);
    }

    #[test]
    fn test_heading_calculation() {
        let nodes = vec![
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
                x: 10.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
        ];

        let points = SplineInterpolator::interpolate_spline(&nodes, false, 10.0).unwrap();
        assert!(points[0].heading_rad.abs() < 0.1);
    }
}
