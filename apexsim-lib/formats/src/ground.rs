//! The ground sidecar: `<Stem>.ground.msgpack`.
//!
//! The height of whatever the client renders at a world position (road
//! surface, verge, run-off, terrain), sampled from the same ground model the
//! bake draws, so a car that leaves the asphalt sits on the grass the player
//! sees. The server reads it; without it the centerline elevation is used
//! everywhere.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const GROUND_SIDECAR_VERSION: u32 = 1;

/// Row-major grid of ground heights, `index = row * cols + col`, sample
/// `(col, row)` at `(origin_x + col * cell_m, origin_y + row * cell_m)`, in
/// the server frame (metres, +X forward at the start line, +Y left, +Z up:
/// the YAML's frame, not Unreal's). Each sample is the surface the client
/// renders at that XY: the road (with banking) inside the road, the pit
/// lane inside the pit lane, the verge, band or terrain ground elsewhere.
/// Heights in centimetres, clamped to the `i16` range.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GroundHeightfield {
    pub version: u32,
    pub origin_x: f32,
    pub origin_y: f32,
    pub cell_m: f32,
    pub cols: u32,
    pub rows: u32,
    pub heights_cm: Vec<i16>,
}

impl GroundHeightfield {
    /// `Monzarella.yaml` -> `Monzarella.ground.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        crate::sidecar_path(track_file, "ground")
    }

    /// Height at sample `(col, row)`, metres.
    pub fn height_m(&self, col: u32, row: u32) -> f32 {
        self.heights_cm[(row * self.cols + col) as usize] as f32 / 100.0
    }
}
