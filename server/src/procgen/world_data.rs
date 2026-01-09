/// Data structures for procedural world generation
use serde::{Deserialize, Serialize};

/// Compact heightmap representation for terrain
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TerrainHeightmap {
    /// Grid width in cells
    pub width: usize,
    /// Grid height in cells
    pub height: usize,
    /// Size of each grid cell in meters
    pub cell_size_m: f32,
    /// World-space origin X coordinate (bottom-left corner)
    pub origin_x: f32,
    /// World-space origin Y coordinate (bottom-left corner)
    pub origin_y: f32,
    /// Flattened height values (row-major order: heights[y * width + x])
    pub heights: Vec<f32>,
}

impl TerrainHeightmap {
    /// Create a new heightmap with given dimensions
    pub fn new(width: usize, height: usize, cell_size_m: f32, origin_x: f32, origin_y: f32) -> Self {
        Self {
            width,
            height,
            cell_size_m,
            origin_x,
            origin_y,
            heights: vec![0.0; width * height],
        }
    }

    /// Sample height at world coordinates using bilinear interpolation
    pub fn sample(&self, world_x: f32, world_y: f32) -> f32 {
        // Convert world coords to grid coords
        let grid_x = (world_x - self.origin_x) / self.cell_size_m;
        let grid_y = (world_y - self.origin_y) / self.cell_size_m;

        // Clamp to valid range
        if grid_x < 0.0 || grid_y < 0.0
            || grid_x >= (self.width - 1) as f32
            || grid_y >= (self.height - 1) as f32 {
            return 0.0;
        }

        // Get integer and fractional parts
        let x0 = grid_x.floor() as usize;
        let y0 = grid_y.floor() as usize;
        let x1 = (x0 + 1).min(self.width - 1);
        let y1 = (y0 + 1).min(self.height - 1);

        let fx = grid_x - x0 as f32;
        let fy = grid_y - y0 as f32;

        // Bilinear interpolation
        let h00 = self.get_height(x0, y0);
        let h10 = self.get_height(x1, y0);
        let h01 = self.get_height(x0, y1);
        let h11 = self.get_height(x1, y1);

        let h0 = h00 * (1.0 - fx) + h10 * fx;
        let h1 = h01 * (1.0 - fx) + h11 * fx;

        h0 * (1.0 - fy) + h1 * fy
    }

    /// Get height at grid coordinates (clamped to valid range)
    pub fn get_height(&self, x: usize, y: usize) -> f32 {
        if x >= self.width || y >= self.height {
            return 0.0;
        }
        self.heights[y * self.width + x]
    }

    /// Set height at grid coordinates
    pub fn set_height(&mut self, x: usize, y: usize, height: f32) {
        if x < self.width && y < self.height {
            self.heights[y * self.width + x] = height;
        }
    }
}

/// Environment preset configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EnvironmentPreset {
    /// Low-frequency noise for base terrain shape
    pub base_noise_freq: f32,
    /// High-frequency noise for detail
    pub detail_noise_freq: f32,
    /// Maximum terrain height in meters
    pub max_height: f32,
    /// Base object density (0-1)
    pub object_density: f32,
    /// Allowed object model IDs for this environment
    pub allowed_objects: Vec<String>,
    /// Ground color RGB (0-1 range)
    pub ground_color: [f32; 3],
}

impl EnvironmentPreset {
    /// Create desert preset
    pub fn desert() -> Self {
        Self {
            base_noise_freq: 0.02,
            detail_noise_freq: 0.1,
            max_height: 15.0,
            object_density: 0.3,
            allowed_objects: vec![
                "rock_small".to_string(),
                "rock_large".to_string(),
                "bush_dry".to_string(),
            ],
            ground_color: [0.8, 0.7, 0.5], // Sandy
        }
    }

    /// Create forest preset
    pub fn forest() -> Self {
        Self {
            base_noise_freq: 0.01,
            detail_noise_freq: 0.05,
            max_height: 40.0,
            object_density: 0.8,
            allowed_objects: vec![
                "tree_pine".to_string(),
                "tree_oak".to_string(),
                "bush_green".to_string(),
            ],
            ground_color: [0.3, 0.5, 0.2], // Forest green
        }
    }

    /// Create city preset
    pub fn city() -> Self {
        Self {
            base_noise_freq: 0.05,
            detail_noise_freq: 0.1,
            max_height: 5.0,
            object_density: 0.6,
            allowed_objects: vec![
                "building_small".to_string(),
                "building_large".to_string(),
                "lamp_post".to_string(),
                "fence".to_string(),
            ],
            ground_color: [0.4, 0.4, 0.4], // Concrete gray
        }
    }

    /// Create mountains preset
    pub fn mountains() -> Self {
        Self {
            base_noise_freq: 0.015,
            detail_noise_freq: 0.08,
            max_height: 60.0,
            object_density: 0.4,
            allowed_objects: vec![
                "rock_large".to_string(),
                "rock_small".to_string(),
                "tree_pine".to_string(),
            ],
            ground_color: [0.5, 0.5, 0.4], // Rocky gray
        }
    }

    /// Create plains preset
    pub fn plains() -> Self {
        Self {
            base_noise_freq: 0.025,
            detail_noise_freq: 0.06,
            max_height: 20.0,
            object_density: 0.7,
            allowed_objects: vec![
                "grass_tall".to_string(),
                "bush_green".to_string(),
                "tree_oak".to_string(),
            ],
            ground_color: [0.4, 0.6, 0.3], // Grassland
        }
    }

    /// Create country/countryside preset (rolling hills)
    pub fn country() -> Self {
        Self {
            base_noise_freq: 0.02,
            detail_noise_freq: 0.07,
            max_height: 25.0,
            object_density: 0.6,
            allowed_objects: vec![
                "tree_oak".to_string(),
                "bush_green".to_string(),
                "grass_tall".to_string(),
            ],
            ground_color: [0.45, 0.55, 0.3], // Countryside green
        }
    }

    /// Create park preset (manicured, mostly flat)
    pub fn park() -> Self {
        Self {
            base_noise_freq: 0.03,
            detail_noise_freq: 0.08,
            max_height: 8.0,
            object_density: 0.65,
            allowed_objects: vec![
                "tree_oak".to_string(),
                "bush_green".to_string(),
                "grass_tall".to_string(),
            ],
            ground_color: [0.35, 0.65, 0.3], // Park green
        }
    }
}

/// Complete procedural world data
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProceduralWorldData {
    /// Environment type identifier
    pub environment_type: String,
    /// Random seed for deterministic generation
    pub seed: u32,
    /// Terrain heightmap (None if generation failed)
    pub heightmap: Option<TerrainHeightmap>,
    /// Track corridor blend width in meters
    pub blend_width: f32,
    /// Object density multiplier (0-1)
    pub object_density: f32,
    /// Decal profile identifier
    pub decal_profile: String,
    /// Environment preset used for generation
    pub preset: EnvironmentPreset,
}
