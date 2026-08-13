//! The `.ats` (Apex Track Scene) data model.
//!
//! A `.ats` file is the editable scene layer for one source `track.yaml`.
//! The YAML is **read-only input** — it belongs to the server and is never
//! written by the editor. Everything the 3D scene adds on top of the logical
//! track — ground surfaces, curbs, painted markings, the pit lane,
//! trackside props — lives here, and this is the file the Unreal importer
//! consumes (plain JSON, so `FJsonSerializer` can read it directly).
//!
//! Coordinates are track space (right-handed, `+X` course direction at
//! start/finish, `+Y` left, `+Z` up, meters). Track-anchored elements
//! (surfaces, curbs, markings) use *station* positions: distance in meters along the
//! sampled centerline, plus lateral offsets where positive is left of the
//! centerline. World-anchored elements (props, pit-lane nodes) use absolute
//! track-space coordinates.
//!
//! Determinism: element ids come from `next_id` (a monotonic counter
//! persisted in the file) — no UUIDs, no wall clock — so identical edits
//! produce byte-identical saves.

use serde::{Deserialize, Serialize};

use crate::track_data::TrackFile;

pub const ATS_FORMAT: &str = "apex-track-scene";
/// v2 added the `surfaces` layer (runoff, gravel, grass). v1 files still
/// load — `surfaces` defaults to empty — and are migrated in memory by
/// `ats_io::load_ats`.
pub const ATS_VERSION: u32 = 2;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AtsScene {
    /// Always [`ATS_FORMAT`]; rejects unrelated JSON files early.
    pub format: String,
    /// Format version, currently [`ATS_VERSION`].
    pub version: u32,
    /// File name (not path) of the source `track.yaml` this scene decorates.
    pub source_track: String,
    /// Track name copied from the source at creation time, for display.
    pub track_name: String,
    /// Ground beside the track: runoff, gravel traps, grass. Listed before
    /// the curbs because that is the order they stack on the ground.
    #[serde(default)]
    pub surfaces: Vec<Surface>,
    #[serde(default)]
    pub curbs: Vec<Curb>,
    #[serde(default)]
    pub markings: Vec<Marking>,
    #[serde(default)]
    pub pit_lane: Option<PitLane>,
    #[serde(default)]
    pub props: Vec<Prop>,
    /// Next element id to hand out. Monotonic, never reused.
    pub next_id: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    Left,
    Right,
}

impl Side {
    pub const ALL: [Side; 2] = [Side::Left, Side::Right];

    pub fn label(self) -> &'static str {
        match self {
            Side::Left => "left",
            Side::Right => "right",
        }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        match s.trim().to_ascii_lowercase().as_str() {
            "left" => Ok(Side::Left),
            "right" => Ok(Side::Right),
            other => Err(format!("unknown side {other:?} (expected left|right)")),
        }
    }
}

/// What a [`Surface`] patch is made of. The Unreal importer maps these to
/// materials and physics surfaces; the editor previews them as flat color.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SurfaceKind {
    /// Open ground: the default world surface either side of the circuit.
    Grass,
    /// Deceleration gravel trap.
    Gravel,
    /// Paved runoff — the modern alternative to gravel.
    AsphaltRunoff,
    /// Concrete apron, typically around pit entries and barriers.
    Concrete,
    /// Sand trap.
    Sand,
    /// The green synthetic strip bordering many curbs.
    Astroturf,
}

impl SurfaceKind {
    pub const ALL: [SurfaceKind; 6] = [
        SurfaceKind::Grass,
        SurfaceKind::Gravel,
        SurfaceKind::AsphaltRunoff,
        SurfaceKind::Concrete,
        SurfaceKind::Sand,
        SurfaceKind::Astroturf,
    ];

    pub fn label(self) -> &'static str {
        match self {
            SurfaceKind::Grass => "grass",
            SurfaceKind::Gravel => "gravel",
            SurfaceKind::AsphaltRunoff => "asphalt_runoff",
            SurfaceKind::Concrete => "concrete",
            SurfaceKind::Sand => "sand",
            SurfaceKind::Astroturf => "astroturf",
        }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        let needle = s.trim().to_ascii_lowercase();
        Self::ALL
            .into_iter()
            .find(|k| k.label() == needle)
            .ok_or_else(|| {
                let expected: Vec<&str> = Self::ALL.into_iter().map(|k| k.label()).collect();
                format!(
                    "unknown surface kind {s:?} (expected one of {})",
                    expected.join("|")
                )
            })
    }

    /// Draw order for coincident patches: lower renders underneath. Broad
    /// ground goes down first, then progressively more specific surfaces on
    /// top, so a gravel trap laid over the grass band actually shows.
    pub fn layer(self) -> u8 {
        match self {
            SurfaceKind::Grass => 0,
            SurfaceKind::Concrete => 1,
            SurfaceKind::AsphaltRunoff => 2,
            SurfaceKind::Sand => 3,
            SurfaceKind::Gravel => 4,
            SurfaceKind::Astroturf => 5,
        }
    }
}

/// A patch of ground beside the track — runoff, gravel trap, grass verge —
/// anchored to a station span like a curb, and measured *outward from the
/// track edge* so it follows the circuit as the track width changes.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Surface {
    pub id: u64,
    pub kind: SurfaceKind,
    /// Which side of the track the patch lies on.
    pub side: Side,
    pub start_m: f32,
    /// Station where it ends. On a closed loop `end_m < start_m` wraps
    /// through the start/finish line.
    pub end_m: f32,
    /// Gap between the track edge and the patch's inner border, meters
    /// (e.g. leave room for a curb).
    pub inner_m: f32,
    /// Extent outward from `inner_m` at `start_m`, meters.
    pub width_m: f32,
    /// Extent at `end_m`; `None` means no taper. Gravel traps and runoff
    /// areas are usually wedges, so the width is interpolated along the span.
    #[serde(default)]
    pub end_width_m: Option<f32>,
}

impl Surface {
    /// Width at `t` (0 at `start_m`, 1 at `end_m`).
    pub fn width_at(&self, t: f32) -> f32 {
        match self.end_width_m {
            Some(end) => self.width_m + (end - self.width_m) * t.clamp(0.0, 1.0),
            None => self.width_m,
        }
    }
}

/// A curb strip anchored to the track edge over a station span.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Curb {
    pub id: u64,
    pub side: Side,
    /// Station (meters along centerline) where the curb starts.
    pub start_m: f32,
    /// Station where it ends. On a closed loop `end_m < start_m` wraps
    /// through the start/finish line.
    pub end_m: f32,
    /// Curb width outward from the track edge, meters.
    pub width_m: f32,
    /// Visual style key the Unreal importer maps to a material,
    /// e.g. `red_white`, `yellow_black`, `green_white`.
    pub style: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MarkingKind {
    StartFinish,
    GridSlot,
    EdgeLine,
    PitEntry,
    PitExit,
    Custom,
}

impl MarkingKind {
    pub const ALL: [MarkingKind; 6] = [
        MarkingKind::StartFinish,
        MarkingKind::GridSlot,
        MarkingKind::EdgeLine,
        MarkingKind::PitEntry,
        MarkingKind::PitExit,
        MarkingKind::Custom,
    ];

    pub fn label(self) -> &'static str {
        match self {
            MarkingKind::StartFinish => "start_finish",
            MarkingKind::GridSlot => "grid_slot",
            MarkingKind::EdgeLine => "edge_line",
            MarkingKind::PitEntry => "pit_entry",
            MarkingKind::PitExit => "pit_exit",
            MarkingKind::Custom => "custom",
        }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        let needle = s.trim().to_ascii_lowercase();
        Self::ALL
            .into_iter()
            .find(|k| k.label() == needle)
            .ok_or_else(|| {
                let expected: Vec<&str> = Self::ALL.into_iter().map(|k| k.label()).collect();
                format!(
                    "unknown marking kind {s:?} (expected one of {})",
                    expected.join("|")
                )
            })
    }
}

/// A painted rectangle on the track surface, anchored in station/lateral
/// space so it follows the centerline through corners.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Marking {
    pub id: u64,
    pub kind: MarkingKind,
    pub start_m: f32,
    pub end_m: f32,
    /// Lateral extent, meters from the centerline; positive is left.
    pub lat_from_m: f32,
    pub lat_to_m: f32,
    /// Linear RGBA paint color.
    pub color: [f32; 4],
}

/// The pit lane: its own polyline in track space, rendered and exported as
/// a narrow road parallel to the main track.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PitLane {
    /// Centerline nodes of the pit lane, `[x, y, z]` in track space.
    pub nodes: Vec<[f32; 3]>,
    pub width_m: f32,
    /// Number of pit boxes, evenly distributed along the lane.
    pub box_count: u32,
    pub speed_limit_kmh: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PropKind {
    Tree,
    Sign,
    Barrier,
    TireWall,
    Building,
    Grandstand,
    Light,
    Cone,
    Misc,
}

impl PropKind {
    pub const ALL: [PropKind; 9] = [
        PropKind::Tree,
        PropKind::Sign,
        PropKind::Barrier,
        PropKind::TireWall,
        PropKind::Building,
        PropKind::Grandstand,
        PropKind::Light,
        PropKind::Cone,
        PropKind::Misc,
    ];

    pub fn label(self) -> &'static str {
        match self {
            PropKind::Tree => "tree",
            PropKind::Sign => "sign",
            PropKind::Barrier => "barrier",
            PropKind::TireWall => "tire_wall",
            PropKind::Building => "building",
            PropKind::Grandstand => "grandstand",
            PropKind::Light => "light",
            PropKind::Cone => "cone",
            PropKind::Misc => "misc",
        }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        let needle = s.trim().to_ascii_lowercase();
        Self::ALL
            .into_iter()
            .find(|k| k.label() == needle)
            .ok_or_else(|| {
                let expected: Vec<&str> = Self::ALL.into_iter().map(|k| k.label()).collect();
                format!(
                    "unknown prop kind {s:?} (expected one of {})",
                    expected.join("|")
                )
            })
    }
}

/// A world-anchored scene object: tree, sign, barrier, building, …
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Prop {
    pub id: u64,
    pub kind: PropKind,
    /// Asset key the Unreal importer maps to a mesh/blueprint,
    /// e.g. `tree_oak_large`, `board_100m`. The editor previews a stand-in.
    pub asset: String,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Counter-clockwise from +X, radians (server yaw convention).
    pub yaw_rad: f32,
    #[serde(default = "default_scale")]
    pub scale: f32,
    /// Optional text for signs/boards.
    #[serde(default)]
    pub text: Option<String>,
}

fn default_scale() -> f32 {
    1.0
}

impl AtsScene {
    /// Fresh scene for a just-opened track that has no `.ats` yet. Seeds a
    /// start/finish line marking spanning the track's width at station 0 so
    /// a new scene isn't invisibly empty.
    pub fn new_for_track(track: &TrackFile, source_file_name: &str) -> Self {
        let (width_left, width_right) = track
            .nodes
            .first()
            .map(|n| n.resolved_half_widths(track.default_width))
            .unwrap_or((5.0, 5.0));
        AtsScene {
            format: ATS_FORMAT.to_string(),
            version: ATS_VERSION,
            source_track: source_file_name.to_string(),
            track_name: track.name.clone(),
            surfaces: Vec::new(),
            curbs: Vec::new(),
            markings: vec![Marking {
                id: 1,
                kind: MarkingKind::StartFinish,
                start_m: 0.0,
                end_m: 0.6,
                lat_from_m: -width_right,
                lat_to_m: width_left,
                color: [1.0, 1.0, 1.0, 1.0],
            }],
            pit_lane: None,
            props: Vec::new(),
            next_id: 2,
        }
    }

    /// Hand out the next element id.
    pub fn alloc_id(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    pub fn surface(&self, id: u64) -> Option<&Surface> {
        self.surfaces.iter().find(|s| s.id == id)
    }

    pub fn surface_mut(&mut self, id: u64) -> Option<&mut Surface> {
        self.surfaces.iter_mut().find(|s| s.id == id)
    }

    pub fn curb(&self, id: u64) -> Option<&Curb> {
        self.curbs.iter().find(|c| c.id == id)
    }

    pub fn curb_mut(&mut self, id: u64) -> Option<&mut Curb> {
        self.curbs.iter_mut().find(|c| c.id == id)
    }

    pub fn marking(&self, id: u64) -> Option<&Marking> {
        self.markings.iter().find(|m| m.id == id)
    }

    pub fn marking_mut(&mut self, id: u64) -> Option<&mut Marking> {
        self.markings.iter_mut().find(|m| m.id == id)
    }

    pub fn prop(&self, id: u64) -> Option<&Prop> {
        self.props.iter().find(|p| p.id == id)
    }

    pub fn prop_mut(&mut self, id: u64) -> Option<&mut Prop> {
        self.props.iter_mut().find(|p| p.id == id)
    }

    /// Remove whichever element (surface, curb, marking, or prop) carries
    /// `id`. Returns `false` if no element has that id.
    pub fn remove_element(&mut self, id: u64) -> bool {
        let count = |s: &Self| s.surfaces.len() + s.curbs.len() + s.markings.len() + s.props.len();
        let before = count(self);
        self.surfaces.retain(|s| s.id != id);
        self.curbs.retain(|c| c.id != id);
        self.markings.retain(|m| m.id != id);
        self.props.retain(|p| p.id != id);
        count(self) != before
    }

    /// Structural sanity checks, run on every load and save.
    pub fn validate(&self) -> Result<(), String> {
        if self.format != ATS_FORMAT {
            return Err(format!(
                "not an Apex Track Scene file (format {:?}, expected {ATS_FORMAT:?})",
                self.format
            ));
        }
        if self.version == 0 || self.version > ATS_VERSION {
            return Err(format!(
                "unsupported .ats version {} (this editor supports up to {ATS_VERSION})",
                self.version
            ));
        }

        let mut ids: Vec<u64> = self
            .surfaces
            .iter()
            .map(|s| s.id)
            .chain(self.curbs.iter().map(|c| c.id))
            .chain(self.markings.iter().map(|m| m.id))
            .chain(self.props.iter().map(|p| p.id))
            .collect();
        ids.sort_unstable();
        if ids.windows(2).any(|w| w[0] == w[1]) {
            return Err("duplicate element id".to_string());
        }
        if let Some(&max) = ids.last() {
            if max >= self.next_id {
                return Err(format!(
                    "element id {max} is not below next_id {}",
                    self.next_id
                ));
            }
        }

        for surface in &self.surfaces {
            if !(surface.start_m.is_finite() && surface.end_m.is_finite()) {
                return Err(format!("surface {}: non-finite station", surface.id));
            }
            if !(surface.width_m.is_finite() && surface.width_m > 0.0) {
                return Err(format!("surface {}: width must be positive", surface.id));
            }
            if let Some(end_width) = surface.end_width_m {
                if !(end_width.is_finite() && end_width > 0.0) {
                    return Err(format!(
                        "surface {}: end width must be positive",
                        surface.id
                    ));
                }
            }
            if !(surface.inner_m.is_finite() && surface.inner_m >= 0.0) {
                return Err(format!(
                    "surface {}: inner offset must be zero or more",
                    surface.id
                ));
            }
        }
        for curb in &self.curbs {
            if !(curb.width_m.is_finite() && curb.width_m > 0.0) {
                return Err(format!("curb {}: width must be positive", curb.id));
            }
            if !(curb.start_m.is_finite() && curb.end_m.is_finite()) {
                return Err(format!("curb {}: non-finite station", curb.id));
            }
        }
        for marking in &self.markings {
            if !(marking.start_m.is_finite()
                && marking.end_m.is_finite()
                && marking.lat_from_m.is_finite()
                && marking.lat_to_m.is_finite())
            {
                return Err(format!("marking {}: non-finite extent", marking.id));
            }
            if marking.lat_from_m == marking.lat_to_m {
                return Err(format!("marking {}: zero lateral width", marking.id));
            }
        }
        for prop in &self.props {
            if !(prop.scale.is_finite() && prop.scale > 0.0) {
                return Err(format!("prop {}: scale must be positive", prop.id));
            }
            if !(prop.x.is_finite() && prop.y.is_finite() && prop.z.is_finite()) {
                return Err(format!("prop {}: non-finite position", prop.id));
            }
        }
        if let Some(pit) = &self.pit_lane {
            if pit.nodes.len() < 2 {
                return Err("pit lane needs at least 2 nodes".to_string());
            }
            if !(pit.width_m.is_finite() && pit.width_m > 0.0) {
                return Err("pit lane width must be positive".to_string());
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::TrackNode;

    fn test_track() -> TrackFile {
        TrackFile {
            name: "Test".to_string(),
            track_id: None,
            nodes: vec![
                TrackNode {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                    width: None,
                    width_left: Some(6.0),
                    width_right: Some(5.0),
                    banking: None,
                    friction: None,
                    surface_type: None,
                },
                TrackNode {
                    x: 100.0,
                    y: 0.0,
                    z: 0.0,
                    width: None,
                    width_left: None,
                    width_right: None,
                    banking: None,
                    friction: None,
                    surface_type: None,
                },
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: false,
            raceline: vec![],
            metadata: None,
        }
    }

    #[test]
    fn new_scene_seeds_a_full_width_start_finish_line() {
        let scene = AtsScene::new_for_track(&test_track(), "Test.yaml");
        assert!(scene.validate().is_ok());
        assert_eq!(scene.markings.len(), 1);
        let sf = &scene.markings[0];
        assert_eq!(sf.kind, MarkingKind::StartFinish);
        assert_eq!(sf.lat_to_m, 6.0);
        assert_eq!(sf.lat_from_m, -5.0);
        assert_eq!(scene.next_id, 2);
    }

    #[test]
    fn alloc_id_is_monotonic_and_remove_element_works_across_layers() {
        let mut scene = AtsScene::new_for_track(&test_track(), "Test.yaml");
        let curb_id = scene.alloc_id();
        scene.curbs.push(Curb {
            id: curb_id,
            side: Side::Left,
            start_m: 0.0,
            end_m: 50.0,
            width_m: 1.2,
            style: "red_white".to_string(),
        });
        let prop_id = scene.alloc_id();
        scene.props.push(Prop {
            id: prop_id,
            kind: PropKind::Tree,
            asset: "tree_generic".to_string(),
            x: 10.0,
            y: 20.0,
            z: 0.0,
            yaw_rad: 0.0,
            scale: 1.0,
            text: None,
        });
        assert!(prop_id > curb_id);
        assert!(scene.validate().is_ok());

        assert!(scene.remove_element(curb_id));
        assert!(scene.curbs.is_empty());
        assert!(!scene.remove_element(curb_id), "id must not be found twice");
        assert!(scene.remove_element(prop_id));
    }

    #[test]
    fn validate_rejects_duplicate_ids_and_bad_values() {
        let mut scene = AtsScene::new_for_track(&test_track(), "Test.yaml");
        scene.curbs.push(Curb {
            id: 1, // clashes with the seeded start/finish marking
            side: Side::Right,
            start_m: 0.0,
            end_m: 10.0,
            width_m: 1.0,
            style: "red_white".to_string(),
        });
        assert!(scene.validate().is_err());

        let mut scene = AtsScene::new_for_track(&test_track(), "Test.yaml");
        let id = scene.alloc_id();
        scene.curbs.push(Curb {
            id,
            side: Side::Right,
            start_m: 0.0,
            end_m: 10.0,
            width_m: 0.0,
            style: "red_white".to_string(),
        });
        assert!(scene.validate().is_err());
    }

    #[test]
    fn enum_parse_round_trips_labels() {
        for side in Side::ALL {
            assert_eq!(Side::parse(side.label()).unwrap(), side);
        }
        for kind in MarkingKind::ALL {
            assert_eq!(MarkingKind::parse(kind.label()).unwrap(), kind);
        }
        for kind in PropKind::ALL {
            assert_eq!(PropKind::parse(kind.label()).unwrap(), kind);
        }
        assert!(Side::parse("up").is_err());
    }
}
