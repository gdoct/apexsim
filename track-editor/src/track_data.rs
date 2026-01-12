use serde::{Deserialize, Serialize};

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
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
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

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RacelinePoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrackMetadata {
    pub country: Option<String>,
    pub city: Option<String>,
    pub length_m: Option<f32>,
    pub description: Option<String>,
    pub year_built: Option<u32>,
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

// Terrain/Procedural data structures

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TerrainHeightmap {
    pub width: usize,
    pub height: usize,
    pub cell_size_m: f32,
    pub origin_x: f32,
    pub origin_y: f32,
    pub heights: Vec<f32>,
}

impl TerrainHeightmap {
    pub fn get_height(&self, x: usize, y: usize) -> f32 {
        if x >= self.width || y >= self.height {
            return 0.0;
        }
        self.heights[y * self.width + x]
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EnvironmentPreset {
    pub base_noise_freq: f32,
    pub detail_noise_freq: f32,
    pub max_height: f32,
    pub object_density: f32,
    pub allowed_objects: Vec<String>,
    pub ground_color: [f32; 3],
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProceduralWorldData {
    pub environment_type: String,
    pub seed: u32,
    pub heightmap: Option<TerrainHeightmap>,
    pub blend_width: f32,
    pub object_density: f32,
    pub decal_profile: String,
    pub preset: EnvironmentPreset,
}
