//! The road mesh sidecar: `<Stem>.road.msgpack`.
//!
//! A triangle mesh of every surface a wheel can stand on (the road, the
//! curbs, the run-off and ground bands, the pit lane), each triangle tagged
//! with a surface: its contact class, a friction multiplier and whether it
//! is inside the track limits. `ats-export` writes the rendered triangles
//! minus the render-only lifts; an importer can write the same file from a
//! track whose road was only ever triangles. The server takes each wheel's
//! height, normal and surface from it (`apexsim-content/docs/road-mesh.md`).
//!
//! Frame: the server's, not Unreal's: metres, +X along the start line's
//! heading, +Y left, +Z up, origin at the start/finish line. Triangles are
//! wound counter-clockwise seen from above (normal up).

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const ROAD_MESH_VERSION: u32 = 1;

/// Contact classes (`RoadSurface::contact`).
pub const CONTACT_ROAD: u8 = 0;
pub const CONTACT_CURB: u8 = 1;
/// Prepared tarmac past the curb: asphalt to drive on, off the track for
/// the lap.
pub const CONTACT_RUNOFF: u8 = 2;
/// Grass, gravel, sand, astroturf.
pub const CONTACT_OFF: u8 = 3;
/// Drives as road; off the track for the lap.
pub const CONTACT_PIT_LANE: u8 = 4;

/// A triangle whose footprint in the ground plane is smaller than this,
/// m2, is nothing a wheel can stand on (a curb's vertical outer face, a
/// sliver): the bake drops it and a reader drops it too.
pub const MIN_FOOTPRINT_M2: f32 = 1e-4;

/// One kind of surface: what the sim does with a wheel on it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadSurface {
    /// `road`, `curb`, `runoff_asphalt`, `off_grass`, `off_gravel`,
    /// `pit_lane`, or an importer's own key. An `off_` key names the
    /// ground.
    pub key: String,
    /// [`CONTACT_ROAD`] and friends.
    pub contact: u8,
    /// Multiplier on the grip the class has on this track (1.0 = the
    /// track's own figure for that class: the node's friction on the road,
    /// `curb_grip` on a curb, `off_track_grip` off it). A generated mesh
    /// writes 1.0 everywhere, so nothing changes against the centerline;
    /// an importer can scale a surface from its own table.
    pub friction: f32,
    /// Inside the track limits: a wheel here is not off the track.
    pub valid_track: bool,
    /// The pit lane: the server drives it as one whatever `contact` says.
    pub pit_lane: bool,
}

impl RoadSurface {
    pub fn road() -> Self {
        Self {
            key: "road".to_string(),
            contact: CONTACT_ROAD,
            friction: 1.0,
            valid_track: true,
            pit_lane: false,
        }
    }

    pub fn curb() -> Self {
        Self {
            key: "curb".to_string(),
            contact: CONTACT_CURB,
            friction: 1.0,
            valid_track: true,
            pit_lane: false,
        }
    }

    pub fn runoff(kind: &str) -> Self {
        Self {
            key: format!("runoff_{kind}"),
            contact: CONTACT_RUNOFF,
            friction: 1.0,
            valid_track: false,
            pit_lane: false,
        }
    }

    pub fn off(kind: &str) -> Self {
        Self {
            key: format!("off_{kind}"),
            contact: CONTACT_OFF,
            friction: 1.0,
            valid_track: false,
            pit_lane: false,
        }
    }

    pub fn pit_lane() -> Self {
        Self {
            key: "pit_lane".to_string(),
            contact: CONTACT_PIT_LANE,
            friction: 1.0,
            valid_track: false,
            pit_lane: true,
        }
    }
}

/// The sidecar as written (`rmp_serde::to_vec_named`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadMeshFile {
    pub version: u32,
    /// Server frame, metres.
    pub vertices: Vec<[f32; 3]>,
    /// Counter-clockwise seen from above. Indices into `vertices`.
    pub triangles: Vec<[u32; 3]>,
    /// One entry per triangle: an index into `surfaces`.
    pub triangle_surface: Vec<u16>,
    pub surfaces: Vec<RoadSurface>,
    /// Which generator wrote it and from what: `ats-export <crc>`,
    /// `ac-import ks_zandvoort`. Logged at load; not interpreted.
    pub source: String,
}

impl RoadMeshFile {
    /// `Monza.yaml` -> `Monza.road.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        crate::sidecar_path(track_file, "road")
    }
}
