use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{Read, Write};
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
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    #[serde(default)]
    pub metadata: Option<TrackMetadata>,
    #[serde(default)]
    pub kerbs: Vec<KerbSegment>,
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
    /// Kerb properties for this node (left and right sides)
    #[serde(default)]
    pub kerbs: Option<NodeKerbs>,
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

/// Kerb properties for a single track node
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct KerbProperties {
    /// Height of the kerb in meters (0 = no kerb)
    #[serde(default)]
    pub height: f32,
    /// Width of the kerb in meters
    #[serde(default)]
    pub width: f32,
}

/// Kerb data for both sides of the track at each node
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct NodeKerbs {
    #[serde(default)]
    pub left: KerbProperties,
    #[serde(default)]
    pub right: KerbProperties,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum KerbSide {
    Left,
    Right,
}

// Keep for backwards compatibility but mark as deprecated
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct KerbSegment {
    pub start_t: f32,
    pub end_t: f32,
    pub side: KerbSide,
    pub height: f32,
    pub width: f32,
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

    /// Sample terrain height at world coordinates with bilinear interpolation
    pub fn sample(&self, world_x: f32, world_y: f32) -> f32 {
        // Convert world coordinates to heightmap grid coordinates
        let grid_x = (world_x - self.origin_x) / self.cell_size_m;
        let grid_y = (world_y - self.origin_y) / self.cell_size_m;

        // Clamp to valid range
        let grid_x = grid_x.clamp(0.0, (self.width - 1) as f32);
        let grid_y = grid_y.clamp(0.0, (self.height - 1) as f32);

        // Bilinear interpolation
        let x0 = grid_x.floor() as usize;
        let y0 = grid_y.floor() as usize;
        let x1 = (x0 + 1).min(self.width - 1);
        let y1 = (y0 + 1).min(self.height - 1);

        let fx = grid_x - x0 as f32;
        let fy = grid_y - y0 as f32;

        let h00 = self.get_height(x0, y0);
        let h10 = self.get_height(x1, y0);
        let h01 = self.get_height(x0, y1);
        let h11 = self.get_height(x1, y1);

        let h0 = h00 * (1.0 - fx) + h10 * fx;
        let h1 = h01 * (1.0 - fx) + h11 * fx;

        h0 * (1.0 - fy) + h1 * fy
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

// Tree/vegetation data structures

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum CanopyShape {
    Cone,
    Sphere,
    MultiCone,
}

#[derive(Debug, Clone)]
pub struct TreeTypeDefinition {
    pub name: &'static str,
    pub trunk_height: f32,
    pub trunk_radius: f32,
    pub canopy_shape: CanopyShape,
    pub canopy_radius: f32,
    pub canopy_height: f32,
    pub trunk_color: [f32; 3],
    pub canopy_color: [f32; 3],
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlacedTree {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub tree_type: usize,
    pub scale: f32,
    pub rotation: f32,
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
    #[serde(default)]
    pub placed_trees: Vec<PlacedTree>,
}

// ── Editor overlay (saved as .apexedit ZIP archive) ──────────────────────────

/// A single terrain heightmap cell edit (sparse delta).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TerrainHeightEdit {
    pub x: usize,
    pub y: usize,
    pub height: f32,
}

/// Editor overlay manifest – the structured data stored inside the .apexedit ZIP.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrackEditorOverlay {
    /// Format version (currently 1).
    #[serde(default = "default_overlay_version")]
    pub version: u32,

    /// Per-node kerb overrides, keyed by node index.
    #[serde(default)]
    pub kerbs: BTreeMap<usize, NodeKerbs>,

    /// Sparse terrain heightmap edits (only cells that differ from the base).
    #[serde(default)]
    pub terrain_edits: Vec<TerrainHeightEdit>,

    /// All editor-placed trees (replaces the base terrain placed_trees).
    #[serde(default)]
    pub placed_trees: Vec<PlacedTree>,
}

fn default_overlay_version() -> u32 {
    1
}

impl TrackEditorOverlay {
    /// Save the overlay into a `.apexedit` ZIP archive.
    pub fn save(&self, path: &Path) -> Result<(), String> {
        let file = std::fs::File::create(path)
            .map_err(|e| format!("Failed to create overlay file: {e}"))?;
        let mut zip = zip::ZipWriter::new(file);

        let options = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);

        // Write manifest.yaml
        let yaml = serde_yaml::to_string(self)
            .map_err(|e| format!("Failed to serialize overlay: {e}"))?;

        zip.start_file("manifest.yaml", options)
            .map_err(|e| format!("Failed to write manifest entry: {e}"))?;
        zip.write_all(yaml.as_bytes())
            .map_err(|e| format!("Failed to write manifest data: {e}"))?;

        zip.finish()
            .map_err(|e| format!("Failed to finalize overlay archive: {e}"))?;

        Ok(())
    }

    /// Load an overlay from a `.apexedit` ZIP archive. Returns None if the file
    /// does not exist; returns Err on parse/IO failures.
    pub fn load(path: &Path) -> Result<Option<Self>, String> {
        if !path.exists() {
            return Ok(None);
        }

        let file = std::fs::File::open(path)
            .map_err(|e| format!("Failed to open overlay file: {e}"))?;
        let mut archive = zip::ZipArchive::new(file)
            .map_err(|e| format!("Failed to read overlay archive: {e}"))?;

        let mut manifest = archive.by_name("manifest.yaml")
            .map_err(|e| format!("Missing manifest.yaml in overlay: {e}"))?;

        let mut contents = String::new();
        manifest.read_to_string(&mut contents)
            .map_err(|e| format!("Failed to read manifest: {e}"))?;

        let overlay: TrackEditorOverlay = serde_yaml::from_str(&contents)
            .map_err(|e| format!("Failed to parse manifest: {e}"))?;

        Ok(Some(overlay))
    }

    /// Apply this overlay on top of loaded track + terrain data (mutates in place).
    pub fn apply(&self, track_data: &mut TrackFileFormat, terrain_data: &mut Option<ProceduralWorldData>) {
        // Apply kerb overrides
        for (&node_idx, kerbs) in &self.kerbs {
            if let Some(node) = track_data.nodes.get_mut(node_idx) {
                node.kerbs = Some(kerbs.clone());
            }
        }

        // Apply terrain height edits
        if let Some(terrain) = terrain_data.as_mut() {
            if let Some(heightmap) = terrain.heightmap.as_mut() {
                for edit in &self.terrain_edits {
                    if edit.x < heightmap.width && edit.y < heightmap.height {
                        heightmap.heights[edit.y * heightmap.width + edit.x] = edit.height;
                    }
                }
            }

            // Replace placed trees
            if !self.placed_trees.is_empty() {
                terrain.placed_trees = self.placed_trees.clone();
            }
        }
    }

    /// Build an overlay by diffing the current state against the original baseline.
    pub fn build_from_diff(
        track_data: &TrackFileFormat,
        terrain_data: &Option<ProceduralWorldData>,
        original_terrain_heights: &Option<Vec<f32>>,
    ) -> Self {
        let mut overlay = TrackEditorOverlay {
            version: 1,
            ..Default::default()
        };

        // Collect kerb overrides: any node with non-default kerbs
        for (i, node) in track_data.nodes.iter().enumerate() {
            if let Some(ref kerbs) = node.kerbs {
                let has_left = kerbs.left.height != 0.0 || kerbs.left.width != 0.0;
                let has_right = kerbs.right.height != 0.0 || kerbs.right.width != 0.0;
                if has_left || has_right {
                    overlay.kerbs.insert(i, kerbs.clone());
                }
            }
        }

        // Collect sparse terrain edits
        if let (Some(terrain), Some(original_heights)) = (terrain_data, original_terrain_heights) {
            if let Some(heightmap) = &terrain.heightmap {
                for y in 0..heightmap.height {
                    for x in 0..heightmap.width {
                        let idx = y * heightmap.width + x;
                        let current = heightmap.heights[idx];
                        let original = original_heights[idx];
                        if (current - original).abs() > 1e-6 {
                            overlay.terrain_edits.push(TerrainHeightEdit {
                                x,
                                y,
                                height: current,
                            });
                        }
                    }
                }
            }

            // Store placed trees
            overlay.placed_trees = terrain.placed_trees.clone();
        }

        overlay
    }
}
