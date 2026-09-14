//! The `.layout.json` dossier: what the real circuit looks like.
//!
//! A `.ats` scene says where the props are; a *layout* says what the place
//! actually is — which side the pit lane runs on and where it goes, what
//! the grandstands are called and where they stand, the pit building and
//! the tower, the bridges over the road, the fairground wheel, and where
//! the woods really are. It sits beside the track's YAML
//! (`content/tracks/real/<Stem>.layout.json`), is written by
//! `scripts/osm_layout.py` from OpenStreetMap (public data, ODbL) and is
//! read here by [`crate::dress`], which turns it into props.
//!
//! Everything is in track space (metres, `+Y` left of the course) and
//! station-anchored where that is the natural way to say it, so a dossier
//! is readable and reviewable against a circuit map without running
//! anything. A missing dossier is not an error anywhere: a track without
//! one is simply groomed as before.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::ats::Side;

pub const LAYOUT_FORMAT: &str = "apex-track-layout";
pub const LAYOUT_VERSION: u32 = 1;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Layout {
    pub format: String,
    pub version: u32,
    /// File name of the source `track.yaml` this dossier describes.
    pub source_track: String,
    #[serde(default)]
    pub track_name: String,
    /// Where the facts come from; carried into the file so a scene built
    /// from it can always be traced back.
    #[serde(default)]
    pub attribution: String,
    #[serde(default)]
    pub corners: Vec<Corner>,
    #[serde(default)]
    pub pit_lane: Option<PitRoad>,
    #[serde(default)]
    pub stands: Vec<Stand>,
    #[serde(default)]
    pub structures: Vec<Structure>,
    #[serde(default)]
    pub crossings: Vec<Crossing>,
    #[serde(default)]
    pub landmarks: Vec<Landmark>,
    /// Outlines of real woodland near the circuit. When a dossier has any,
    /// the groomer plants its tree belts only inside them — which is what
    /// keeps the dunes at Zandvoort bare and the Ardennes at Spa dense.
    #[serde(default)]
    pub woods: Vec<Wood>,
}

/// A patch of real woodland, and what grows in it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Wood {
    /// `broadleaved`, `needleleaved` or `mixed` (OSM's `leaf_type`): the
    /// Ardennes are spruce plantations, the Parco di Monza is broadleaf,
    /// and the tree pass picks its species from this.
    #[serde(default)]
    pub leaf: String,
    pub ring: Vec<[f32; 2]>,
}

impl Wood {
    /// Even-odd point-in-ring test.
    pub fn contains(&self, x: f32, y: f32) -> bool {
        let ring = &self.ring;
        let mut inside = false;
        let mut j = ring.len().saturating_sub(1);
        for i in 0..ring.len() {
            let (xi, yi) = (ring[i][0], ring[i][1]);
            let (xj, yj) = (ring[j][0], ring[j][1]);
            if (yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi {
                inside = !inside;
            }
            j = i;
        }
        inside
    }
}

/// A named part of the course: "Tarzanbocht", "Eau Rouge", "Parabolica".
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Corner {
    pub name: String,
    pub station_m: f32,
    #[serde(default)]
    pub from_m: f32,
    #[serde(default)]
    pub to_m: f32,
}

/// The real pit lane's centerline, in course order.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PitRoad {
    pub side: Side,
    #[serde(default)]
    pub length_m: f32,
    pub nodes: Vec<[f32; 2]>,
}

/// One grandstand. `front` is the run of its outline that faces the road,
/// so a stand built round a bend keeps its curve instead of becoming a
/// box; the rest is what the stand's size and roof are decided from.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Stand {
    #[serde(default)]
    pub name: Option<String>,
    pub station_m: f32,
    pub side: Side,
    #[serde(default)]
    pub offset_m: f32,
    pub length_m: f32,
    pub depth_m: f32,
    #[serde(default)]
    pub covered: bool,
    #[serde(default)]
    pub front: Vec<[f32; 2]>,
    #[serde(default)]
    pub centre: [f32; 2],
    #[serde(default)]
    pub yaw_rad: f32,
}

/// A building beside the circuit: the pit building, a tower, hospitality.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Structure {
    #[serde(default)]
    pub name: Option<String>,
    pub station_m: f32,
    pub side: Side,
    #[serde(default)]
    pub offset_m: f32,
    pub length_m: f32,
    pub depth_m: f32,
    #[serde(default)]
    pub yaw_rad: f32,
    #[serde(default)]
    pub centre: [f32; 2],
    #[serde(default)]
    pub levels: u32,
    #[serde(default)]
    pub area_m2: f32,
    #[serde(default)]
    pub osm_building: Option<String>,
}

/// Something that crosses over the road: a footbridge, a service road,
/// or an arch like the tyre bridge at Le Mans.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Crossing {
    #[serde(default)]
    pub name: Option<String>,
    pub station_m: f32,
    #[serde(default)]
    pub kind: String,
    /// Which of the kit's brands it wears, if any. The real sponsor names
    /// are not the kit's (a circuit's signage is fictionalised), so this
    /// is a content choice the dossier makes rather than something read
    /// off the map.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub brand: Option<String>,
}

/// A point feature worth keeping: the fairground wheel at the Esses, a
/// floodlight mast, a tower.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Landmark {
    pub kind: String,
    #[serde(default)]
    pub name: Option<String>,
    pub station_m: f32,
    pub side: Side,
    pub centre: [f32; 2],
    /// Which of the kit's brands it wears; see [`Crossing::brand`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub brand: Option<String>,
    /// Heading of the landmark itself, when it matters which way it points
    /// (an airship is meant to be seen broadside). Absent: the course
    /// heading at its station.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yaw_rad: Option<f32>,
    /// Height above the road for something that flies. Absent: the kind's
    /// default.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub altitude_m: Option<f32>,
}

#[derive(Debug, thiserror::Error)]
pub enum LayoutError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON parse error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("not a {LAYOUT_FORMAT} file")]
    Format,
    #[error("layout version {0} is newer than this build understands ({LAYOUT_VERSION})")]
    Version(u32),
}

/// The dossier path belonging to a source track file:
/// `content/tracks/real/Monza.yaml` -> `content/tracks/real/Monza.layout.json`.
pub fn layout_path_for<P: AsRef<Path>>(track_path: P) -> PathBuf {
    let path = track_path.as_ref();
    let stem = path.file_stem().unwrap_or_default().to_string_lossy();
    path.with_file_name(format!("{stem}.layout.json"))
}

/// Load a dossier. `Ok(None)` when the track simply has none.
pub fn load_layout<P: AsRef<Path>>(path: P) -> Result<Option<Layout>, LayoutError> {
    let path = path.as_ref();
    if !path.exists() {
        return Ok(None);
    }
    let layout: Layout = serde_json::from_str(&std::fs::read_to_string(path)?)?;
    if layout.format != LAYOUT_FORMAT {
        return Err(LayoutError::Format);
    }
    if layout.version > LAYOUT_VERSION {
        return Err(LayoutError::Version(layout.version));
    }
    Ok(Some(layout))
}

impl Layout {
    /// The named corner a station falls in or just after, for messages.
    pub fn corner_at(&self, station_m: f32) -> Option<&Corner> {
        self.corners
            .iter()
            .rfind(|c| c.station_m <= station_m)
            .or_else(|| self.corners.last())
    }
}
