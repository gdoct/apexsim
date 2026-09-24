//! The editor viewport: Bevy scene, preview meshes, UI state and the MCP
//! endpoint. The pipeline lives in `track_core`; its modules are re-exported
//! here so editor code can keep naming them `crate::ats` and friends.

pub use track_core::{
    ats, ats_io, barriers, dem, dress, groom, layout, pit, project, props, strip_layout, terrain,
    track_bank, track_data, track_io, track_path, track_smooth, ue_export, ue_export_io,
};

pub mod coords;
pub mod mcp;
pub mod preview_mesh;
pub mod scene;
pub mod state;
