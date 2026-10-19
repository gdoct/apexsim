//! The curb sidecar: `<Stem>.curbs.msgpack`.
//!
//! Not geometry: how far the curbs, the tarmac run-off and the gravel and
//! sand traps reach past each road edge, per station, so the sim counts a
//! car with two wheels on a curb as on the track and drives a trap as the
//! player sees it. Without it the road edge is the track limit.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// The version a bake writes. A reader takes 1 to this: each later version
/// only adds `#[serde(default)]` bands.
pub const CURB_BANDS_VERSION: u32 = 3;

/// Sample `i` is the cross-section at station `i * step_m` along the
/// centerline, each value centimetres outward from that side's road edge (0
/// where there is nothing). Left and right are the track's own sides, the
/// same `width_left_m` / `width_right_m` the centerline carries.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct CurbBands {
    pub version: u32,
    pub step_m: f32,
    pub left_cm: Vec<u16>,
    pub right_cm: Vec<u16>,
    /// Version 2: how far the prepared *tarmac* run-off (`asphalt_runoff`,
    /// `concrete` bands) reaches past each road edge, centimetres, 0 where
    /// there is none. A car past the curb but within this is on tarmac:
    /// off the track for the lap, but on a surface with grip and no grass
    /// drag. Empty in a version 1 file, which reads as no tarmac anywhere.
    #[serde(default)]
    pub runoff_left_cm: Vec<u16>,
    #[serde(default)]
    pub runoff_right_cm: Vec<u16>,
    /// Version 3: the gravel traps past each road edge, per sample
    /// `[inner, outer]` centimetres from the edge, `[0, 0]` where there is
    /// none (overlapping traps keep their hull). Empty in an older file,
    /// which reads as no gravel anywhere.
    #[serde(default)]
    pub gravel_left_cm: Vec<[u16; 2]>,
    #[serde(default)]
    pub gravel_right_cm: Vec<[u16; 2]>,
    /// Version 3: the sand traps, as the gravel.
    #[serde(default)]
    pub sand_left_cm: Vec<[u16; 2]>,
    #[serde(default)]
    pub sand_right_cm: Vec<[u16; 2]>,
    /// Version 3: the ground past the bands, as a road mesh surface's kind
    /// (`desert` where the grass is drawn as sand); empty is grass.
    #[serde(default)]
    pub ground: String,
}

impl CurbBands {
    /// `Monza.yaml` -> `Monza.curbs.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        crate::sidecar_path(track_file, "curbs")
    }
}
