//! The pit lane sidecar: `<Stem>.pit.msgpack`.
//!
//! The lane's own centerline, where it leaves and rejoins the track, the
//! speed-limit stretch and every box, all from the same geometry the bake
//! paints the lines and stands the garages on, so a car stops where it sees
//! its box. Stations along the lane are metres from its first node; along
//! the track, metres from the start line. Without it a circuit has no pit
//! stops.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const PIT_SIDECAR_VERSION: u32 = 1;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PitSidecar {
    pub version: u32,
    pub width_m: f32,
    pub speed_limit_mps: f32,
    /// +1 when the lane lies on the track's left, -1 on its right.
    pub lane_side: i32,
    /// The lane's centerline, `[x, y, z]`, nodes a couple of metres apart.
    pub nodes: Vec<[f32; 3]>,
    pub length_m: f32,
    /// The speed limit holds between these lane stations.
    pub limit_start_m: f32,
    pub limit_end_m: f32,
    /// Track stations nearest the lane's first and last node.
    pub entry_station_m: f32,
    pub exit_station_m: f32,
    pub boxes: Vec<PitBoxSpot>,
}

/// Where a car stops for service: the middle of the box's working lane, in
/// front of its garage, facing down the lane.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct PitBoxSpot {
    pub lane_station_m: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
}

impl PitSidecar {
    /// `Monza.yaml` -> `Monza.pit.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        crate::sidecar_path(track_file, "pit")
    }
}
