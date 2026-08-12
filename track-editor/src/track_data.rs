//! Logical track data model.
//!
//! Field names, types, and declaration order deliberately mirror
//! `apexsim_server::track_loader::TrackFileFormat` and friends so that a
//! round-tripped file serializes with the same shape a hand-authored or
//! server-generated `track.yaml` uses. There is no compile-time link between
//! the two (the editor must build fast and stay decoupled from the server's
//! networking/runtime dependency stack), so `tests/compat.rs` pulls in
//! `apexsim-server` as a dev-dependency and asserts the two stay
//! interchangeable: any real track under `content/tracks` can be loaded here,
//! re-saved, and loaded again through the server's own `TrackLoader` with
//! identical physical values.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TrackFile {
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
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    #[serde(default)]
    pub metadata: Option<TrackMetadata>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Checkpoint {
    pub index_start: usize,
    pub index_end: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct SpawnPoint {
    pub position: usize,
    #[serde(default)]
    pub offset_x: f32,
    #[serde(default)]
    pub offset_y: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct RacelinePoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct TrackMetadata {
    #[serde(default)]
    pub country: Option<String>,
    #[serde(default)]
    pub city: Option<String>,
    #[serde(default)]
    pub length_m: Option<f32>,
    #[serde(default)]
    pub description: Option<String>,
    #[serde(default)]
    pub year_built: Option<u32>,
    #[serde(default)]
    pub category: Option<String>,
    #[serde(default)]
    pub environment_type: Option<String>,
    #[serde(default)]
    pub terrain_seed: Option<u32>,
    #[serde(default)]
    pub terrain_scale: Option<f32>,
    #[serde(default)]
    pub terrain_detail: Option<f32>,
    #[serde(default)]
    pub terrain_blend_width: Option<f32>,
    #[serde(default)]
    pub object_density: Option<f32>,
    #[serde(default)]
    pub decal_profile: Option<String>,
}
