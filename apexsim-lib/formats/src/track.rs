//! The track YAML: `<Stem>.yaml`, the authored source of a circuit.
//!
//! The server parses it into its runtime track (`TrackLoader`), the track
//! editor and the `ats-*` tools open it, and some of them write it back
//! (`ats-smooth`, `ats-bank`, the editor's save). A file may also be JSON:
//! content starting with `{` is JSON, anything else YAML.
//!
//! Frame: metres, +X along the start line's heading, +Y left, +Z up, origin
//! at the start/finish line (node 0).
//!
//! Field names, types and declaration order are the file's shape, and the
//! `skip_serializing_if`s are what a rewrite omits: a tool that rewrites a
//! YAML writes it through [`TrackFile`], so changing either changes the
//! bytes of every rewritten file and its content checksum.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TrackFile {
    /// The circuit's real name. Source data only: it is often the operator's
    /// trademark, so nothing a player sees shows it (see `display_name`).
    pub name: String,
    /// The name the game shows (a parody such as `Zandervoort`), sent as the
    /// track's name on the wire. Falls back to `name` when absent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_name: Option<String>,
    /// The track's UUID, the key every record, catalog row and showcase
    /// uses. A track without one gets a fresh id on every server load.
    #[serde(default)]
    pub track_id: Option<String>,
    pub nodes: Vec<TrackNode>,
    #[serde(default)]
    pub checkpoints: Vec<Checkpoint>,
    /// Node indices of the sector boundaries past the start line, in order.
    /// Two of them (three sectors); the server ignores anything else and
    /// splits the lap into even thirds. Kept through every tool that
    /// rewrites the YAML (none of them renumbers the nodes), and omitted
    /// when empty, so a file without them is rewritten unchanged.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub sectors: Vec<usize>,
    #[serde(default)]
    pub spawn_points: Vec<SpawnPoint>,
    #[serde(default)]
    pub default_width: f32,
    #[serde(default)]
    pub closed_loop: bool,
    /// The racing line, for the AI and the client's dots.
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    /// The DRS zones (`apexsim-content/tools/drs_zones.py` writes them):
    /// stations along the centerline in metres. Kept through every tool
    /// that rewrites the YAML, and what the bake paints the detection and
    /// activation lines from.
    #[serde(default)]
    pub drs_zones: Vec<DrsZone>,
    #[serde(default)]
    pub metadata: Option<TrackMetadata>,
}

impl TrackFile {
    /// Whether the road carries the crown and bumps of
    /// [`crate::road_profile`]: every circuit but an imported one, whose
    /// road its importer measured (`metadata.category: Imported`).
    pub fn has_road_profile(&self) -> bool {
        self.metadata
            .as_ref()
            .and_then(|m| m.category.as_deref())
            .is_none_or(|c| !c.eq_ignore_ascii_case("imported"))
    }
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

impl TrackNode {
    /// `(left, right)` half-widths in metres: explicit `width_left` and
    /// `width_right` win, then `width` split evenly, then the track's
    /// `default_width` split evenly.
    pub fn resolved_half_widths(&self, default_width: f32) -> (f32, f32) {
        if let (Some(wl), Some(wr)) = (self.width_left, self.width_right) {
            (wl, wr)
        } else if let Some(w) = self.width {
            (w / 2.0, w / 2.0)
        } else {
            (default_width / 2.0, default_width / 2.0)
        }
    }
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

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct RacelinePoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// One DRS zone along the centerline, stations in metres from the start
/// line: the gap to the car ahead is measured as a car crosses
/// `detection_m`, the flap may open from `start_m` and must be shut by
/// `end_m`. A zone may wrap through the start line.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct DrsZone {
    pub detection_m: f32,
    pub start_m: f32,
    pub end_m: f32,
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
    /// e.g. "F1", "DTM", "IndyCar".
    #[serde(default)]
    pub category: Option<String>,
    // Procedural generation parameters.
    /// "desert" | "forest" | "city" | "mountains" | "plains".
    #[serde(default)]
    pub environment_type: Option<String>,
    /// Deterministic seed (None = generate from the track id).
    #[serde(default)]
    pub terrain_seed: Option<u32>,
    /// Height multiplier (default 1.0).
    #[serde(default)]
    pub terrain_scale: Option<f32>,
    /// Noise frequency multiplier (default 0.5).
    #[serde(default)]
    pub terrain_detail: Option<f32>,
    /// Track corridor blend, metres (default 20.0).
    #[serde(default)]
    pub terrain_blend_width: Option<f32>,
    /// 0-1 density multiplier (default 0.8).
    #[serde(default)]
    pub object_density: Option<f32>,
    /// Decal set identifier (default "default").
    #[serde(default)]
    pub decal_profile: Option<String>,
    /// Where the circuit is (`apexsim-content/tools/track_location.py`, from
    /// the elevation sidecar's georeference): the start line's height above
    /// sea level, m, which the server thins the air by, and its latitude and
    /// longitude.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub altitude_m: Option<f32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latitude_deg: Option<f32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub longitude_deg: Option<f32>,
}
