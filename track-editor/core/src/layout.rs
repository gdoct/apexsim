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

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
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
    /// The real barrier lines: armco, walls and tyre stacks as OSM has
    /// them. The barrier pass lays its runs along these where a circuit
    /// has them and falls back to the runoff edge where it does not.
    #[serde(default)]
    pub barriers: Vec<Line>,
    /// Service roads, access tracks, lanes and footpaths around the
    /// circuit. `kind` is one of `major`, `minor`, `service`, `track`,
    /// `path`.
    #[serde(default)]
    pub roads: Vec<Line>,
    /// Streams and rivers. `kind` is `river`, `stream` or `ditch`.
    #[serde(default)]
    pub waterways: Vec<Line>,
    /// Ground cover and enclosures: car parks, camp sites, meadow,
    /// farmland, villages, scrub, water. What the land around the circuit
    /// actually is, rather than the single grass band it used to be.
    #[serde(default)]
    pub areas: Vec<Area>,
    /// Point features worth a prop: the statue, chapels, pylons, gates,
    /// food stalls, camp sites mapped as a node.
    #[serde(default)]
    pub poi: Vec<Poi>,
    /// Tag census of everything within range of the road that no rule
    /// claimed, so the next circuit's gaps are visible in the dossier
    /// instead of in a screenshot. Data for humans; nothing reads it.
    #[serde(default)]
    pub unclassified: std::collections::BTreeMap<String, u32>,
    /// Pictures the fans paint on the road (the Nordschleife's graffiti),
    /// laid by `ats-dress` as `.ats` `decals`. OSM has none of this; the
    /// entries come from `MANUAL_GRAFFITI` in `scripts/osm_layout.py`.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub graffiti: Vec<Graffiti>,
}

/// One painted picture on the road, in station / lateral space.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Graffiti {
    /// `graffiti/<name>`: `content/props/decal/graffiti/<name>.png`.
    pub image: String,
    /// Where the picture starts (the edge a driver reaches first).
    pub station_m: f32,
    /// Centre across the road, metres, positive left.
    #[serde(default)]
    pub lat_m: f32,
    pub length_m: f32,
    pub width_m: f32,
    /// Painted for the other direction.
    #[serde(default)]
    pub reversed: bool,
    /// The spot it is painted at ("Brünnchen"), for a reader of the dossier.
    #[serde(default)]
    pub name: Option<String>,
}

/// A line feature on the ground: a barrier run, a road, a stream. What it
/// is comes from `kind`, whose vocabulary is per layer (see [`Layout`]).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Line {
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub name: Option<String>,
    pub line: Vec<[f32; 2]>,
    /// The way closed on itself: an enclosure rather than a run.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub closed: bool,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub bridge: bool,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub tunnel: bool,
}

impl Line {
    /// Distance from a point to the nearest segment of the line.
    pub fn distance_to(&self, x: f32, y: f32) -> f32 {
        let mut best = f32::MAX;
        for pair in self.line.windows(2) {
            let (a, b) = (pair[0], pair[1]);
            let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
            let len2 = dx * dx + dy * dy;
            let t = if len2 > 1e-6 {
                (((x - a[0]) * dx + (y - a[1]) * dy) / len2).clamp(0.0, 1.0)
            } else {
                0.0
            };
            let (px, py) = (a[0] + t * dx, a[1] + t * dy);
            best = best.min((x - px).hypot(y - py));
        }
        best
    }

    /// Total length of the run.
    pub fn length_m(&self) -> f32 {
        self.line
            .windows(2)
            .map(|p| (p[1][0] - p[0][0]).hypot(p[1][1] - p[0][1]))
            .sum()
    }
}

/// A patch of ground with a kind: a car park, a meadow, a village.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Area {
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub name: Option<String>,
    pub ring: Vec<[f32; 2]>,
}

impl Area {
    /// Even-odd point-in-ring test.
    pub fn contains(&self, x: f32, y: f32) -> bool {
        ring_contains(&self.ring, x, y)
    }

    /// Centroid of the ring's vertices.
    pub fn centre(&self) -> (f32, f32) {
        let n = self.ring.len().max(1) as f32;
        let sx: f32 = self.ring.iter().map(|p| p[0]).sum();
        let sy: f32 = self.ring.iter().map(|p| p[1]).sum();
        (sx / n, sy / n)
    }

    /// Shoelace area, always positive.
    pub fn area_m2(&self) -> f32 {
        let mut acc = 0.0;
        let mut j = self.ring.len().saturating_sub(1);
        for i in 0..self.ring.len() {
            acc += (self.ring[j][0] + self.ring[i][0]) * (self.ring[j][1] - self.ring[i][1]);
            j = i;
        }
        (acc / 2.0).abs()
    }
}

/// A point feature: the statue, a chapel, a pylon, a camp site.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Poi {
    pub kind: String,
    #[serde(default)]
    pub name: Option<String>,
    pub station_m: f32,
    pub side: Side,
    pub centre: [f32; 2],
}

/// Even-odd point-in-ring test, shared by [`Wood`] and [`Area`].
fn ring_contains(ring: &[[f32; 2]], x: f32, y: f32) -> bool {
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
        ring_contains(&self.ring, x, y)
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
