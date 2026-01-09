/// Procedural world generation module for ApexSim
///
/// This module provides terrain generation, environment object placement,
/// and trackside decal systems for racing tracks.

pub mod world_data;
pub mod noise;
pub mod environment_presets;
pub mod terrain;

// Re-export main types for convenience
pub use world_data::{ProceduralWorldData, TerrainHeightmap, EnvironmentPreset};
pub use terrain::generate_procedural_world;
