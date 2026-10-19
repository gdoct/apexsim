//! The wall sidecar: `<Stem>.walls.msgpack`.
//!
//! Not meshes: where the solid faces are, so the sim stops a car at the
//! armco the player sees. A barrier, tire wall, fence or pit wall is one
//! segment along its heading; a grandstand, building, garage or fairground
//! piece is the four sides of its footprint; an underpass contributes the
//! abutment walls beside the lower road and the deck's parapets. Heights let
//! the server tell a wall the car is level with from one on the road above
//! or below. Without it nothing stops a car off the road.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const WALLS_VERSION: u32 = 1;

/// Steel armco or a fence: springy.
pub const WALL_KIND_ARMCO: u8 = 0;
/// Tires or TecPro: absorbs the hit.
pub const WALL_KIND_TIRES: u8 = 1;
/// Concrete, a building, a stand, a parapet: hard.
pub const WALL_KIND_CONCRETE: u8 = 2;
/// A sausage kerb: driven over, jolts the car, never stops it.
pub const WALL_KIND_KERB: u8 = 3;

/// One solid face in the ground plane, track frame (metres, +Y left): the
/// ground height at its base, how tall it is, and what it is made of
/// (`WALL_KIND_*`).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct WallSegment {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    /// Ground height at the base of the wall.
    pub z: f32,
    /// How far the wall reaches above its base.
    pub height_m: f32,
    /// `WALL_KIND_*`.
    pub kind: u8,
}

impl WallSegment {
    /// Length in the ground plane, metres.
    pub fn length(&self) -> f32 {
        (self.x1 - self.x0).hypot(self.y1 - self.y0)
    }
}

/// The sidecar as written (`rmp_serde::to_vec_named`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct WallFile {
    pub version: u32,
    pub segments: Vec<WallSegment>,
}

impl WallFile {
    /// `Monzarella.yaml` -> `Monzarella.walls.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        crate::sidecar_path(track_file, "walls")
    }
}
