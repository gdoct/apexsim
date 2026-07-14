pub mod environment_presets;
pub mod noise;
pub mod terrain;
/// Procedural world generation module for ApexSim
///
/// This module provides terrain generation, environment object placement,
/// and trackside decal systems for racing tracks.
pub mod world_data;

// Re-export main types for convenience
pub use terrain::generate_procedural_world;
pub use world_data::{EnvironmentPreset, ProceduralWorldData, TerrainHeightmap};
