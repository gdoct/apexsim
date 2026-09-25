//! Baking a track project into the JSON the Unreal `ApexTrackImport`
//! commandlet turns into a level.
//!
//! The `.ats` scene alone is not enough for Unreal: every track-anchored
//! element (surface, curb, marking) is a *station span* measured along a
//! centerline that lives in the read-only `track.yaml`, and Unreal has no
//! YAML parser. So the export resolves all of that here — it samples the
//! centerline through the same [`CenterlinePath`] the viewport and the
//! server use, extrudes every element into triangles, and writes one
//! self-contained `.uescene.json` per track. The commandlet stays a dumb
//! asset writer: buffers in, `UStaticMesh` out.
//!
//! These are *not* the editor's preview meshes. The editor's `preview_mesh`
//! builds flat strips with a hardcoded up-normal, which is fine for a
//! viewport and wrong for a lit level: here normals follow the banked surface frame and
//! curbs get a raised profile with a real outer face.
//!
//! # The ground
//!
//! Everything that touches the ground — the ground mesh, the authored
//! surface bands, a curb's outer face, the painted strip beyond it — takes
//! its height from one function, [`TerrainHeightfield::ground_height_at`]:
//! the verge beside every road (track and pit lane), blending into the
//! terrain further out. That is what keeps the world from stepping at the
//! road edge, and it is also what the server's `.ground.msgpack` sidecar
//! samples, so the sim's idea of the ground is the one the client renders.
//!
//! # Road paint
//!
//! Paint is geometry too — the parent material can only vary a base color
//! per key — so every color is its own `marking_*` material: edge lines,
//! the chequered start/finish line, grid boxes, pit-lane lines and the
//! painted strip beside each curb. The rubbered racing line is the
//! exception: `wear_core` / `wear_edge` are dark bands of family `road`,
//! not paint.
//!
//! # Conventions at the boundary
//!
//! Everything in the output is already in Unreal's frame, so the commandlet
//! never converts anything:
//!
//! - **Units**: centimeters, degrees.
//! - **Position**: track `(x, y, z)` m -> UE `(x·100, −y·100, z·100)` cm.
//! - **Yaw**: track `θ` rad CCW -> UE `−θ·180/π` degrees.
//!
//! That mapping negates one axis, so its determinant is −1: it is a mirror,
//! not a rotation, and it flips triangle handedness for free. Geometry is
//! therefore emitted with ordinary right-handed CCW winding and *not*
//! reversed again — see [`Builder::emit_quad`], which is the one place the
//! convention lives.
//!
//! # Determinism
//!
//! Same inputs must give a byte-identical file: no wall clock, no unordered
//! iteration, and every float is rounded to a fixed number of decimals
//! before serialization (which also keeps the files a good deal smaller).

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::ats::{
    AtsScene, Curb, Decal, Dressing, Marking, MarkingKind, Prop, PropKind, Side, Surface,
};
use crate::dem::DemFile;
use crate::props;
use crate::strip_layout::{
    surface_kind_color, surface_lateral_fractions, surface_lift, CURB_LIFT_M, MARKING_LIFT_M,
};
use crate::terrain::{self, GroundHeightfield, TerrainHeightfield, Underpass};
use crate::track_data::TrackFile;
use crate::track_path::{curvature_at, offset_point, CenterlinePath, PathSample};

pub const UE_SCENE_FORMAT: &str = "apex-ue-scene";
pub const UE_SCENE_VERSION: u32 = 1;

/// Meters -> Unreal centimeters.
const M_TO_CM: f32 = 100.0;

/// Cross-section spacing when baking the track ribbon, curbs, markings and
/// the pit lane, meters.
const STEP_M: f32 = 1.0;
/// Ground patches are big and flat; they do not need road tessellation.
const SURFACE_STEP_M: f32 = 4.0;

/// Strips are cut at multiples of this station, and chunks that share a
/// section and a material are merged into one mesh. Without it a circuit
/// becomes either one 6 km mesh (no culling) or one mesh per element
/// (hundreds of actors).
const SECTION_LEN_M: f32 = 250.0;

/// Minimum `facet_quality` a triangle needs to be worth exporting: the sine
/// of the angle between its edges, so roughly half a degree.
const MIN_FACET_QUALITY: f32 = 0.01;

/// Grid ground within this of an underpass wall line is left out of the
/// grid and drawn along the road instead, meters: a 4 m ground cell's
/// diagonal, so no grid facet can straddle the wall.
const UNDERPASS_CUT_M: f32 = 5.8;
/// Diagonal of the fine ground grid, meters.
const GRID_DIAGONAL_M: f32 = 5.7;
/// Lift of the ground drawn along an underpass over the grid it overlaps.
const COVER_LIFT_M: f32 = 0.03;
/// Walls lower than this are not drawn; the ground meets the slot floor.
const WALL_MIN_HEIGHT_M: f32 = 0.25;
/// Wall coping: how far it stands above the embankment and how thick it is.
const WALL_PARAPET_M: f32 = 0.5;
const WALL_COPING_M: f32 = 0.6;
/// Bridge parapets along the deck edges.
const PARAPET_HEIGHT_M: f32 = 1.1;
const PARAPET_WIDTH_M: f32 = 0.35;
/// The fascia leans out this much over the deck's depth: a profile segment
/// needs some lateral extent to be extruded at all.
const FASCIA_LEAN_M: f32 = 0.05;
const STRUCTURE_KEY: &str = "structure_concrete";
const STRUCTURE_COLOR: [f32; 4] = [0.62, 0.61, 0.58, 1.0];
/// Suzuka's crossover wears a yellow sponsor board; so do most.
const FASCIA_KEY: &str = "structure_fascia";

/// Material key for the distant land. Its own key rather than the
/// ground's, so the client can give the skyline a coarser, lower-contrast
/// treatment than the grass a driver is looking at.
const HORIZON_KEY: &str = "horizon";
const FASCIA_COLOR: [f32; 4] = [0.95, 0.76, 0.05, 1.0];

/// Height of a curb's outer lip above the track surface, meters.
const CURB_HEIGHT_M: f32 = 0.05;
/// Where the curb's slope breaks, as a fraction of its width / height.
const CURB_LIP_FRAC: f32 = 0.15;
/// Painted flat strip beyond a curb, meters wide, so the curb reads as part
/// of the road rather than a wall standing on the verge.
const CURB_STRIP_M: f32 = 0.3;
/// The strip's lift over the ground: above every surface band (which top
/// out at `surface_lift(Astroturf)`), still under the curb's lip.
const CURB_STRIP_LIFT_M: f32 = 0.09;

/// The pit lane's entry and exit deliberately overlap the road so the
/// surfaces connect; this lift keeps that overlap from z-fighting.
const PIT_LIFT_M: f32 = 0.02;

// Paint. Each layer gets its own lift so overlapping layers never fight.
const EDGE_LINE_WIDTH_M: f32 = 0.20;
const LINE_WIDTH_M: f32 = 0.15;
const LINE_COLOR: [f32; 4] = [0.85, 0.85, 0.82, 1.0];
/// DRS detection and activation lines: painted across the road, white.
const DRS_LINE_COLOR: [f32; 4] = [0.9, 0.9, 0.88, 1.0];
const DRS_LINE_WIDTH_M: f32 = 0.4;
/// A DRS board stands this far past the road edge.
const DRS_BOARD_OFF_EDGE_M: f32 = 3.5;
/// The pit lane's speed-limit lines across the lane, and the depth of a
/// pit box's outline into the lane from the garage side.
const PIT_LIMIT_LINE_M: f32 = 0.3;
const PIT_BOX_DEPTH_M: f32 = 3.2;
/// The pit exit blend line: how far it runs past the exit and how far in
/// from the road edge.
const PIT_EXIT_LINE_M: f32 = 120.0;
const PIT_EXIT_LINE_IN_M: f32 = 2.2;
/// Painted run-off: the width of each stripe across the band, and the
/// paint's lift over the tarmac it is on.
const RUNOFF_STRIPE_M: f32 = 1.5;
const RUNOFF_PAINT_LIFT_M: f32 = 0.012;
/// Start/finish chequer and grid boxes sit a hair over the edge lines.
const GRID_PAINT_LIFT_M: f32 = MARKING_LIFT_M + 0.005;
/// Road decals (graffiti) are painted over everything else on the road,
/// edge lines included.
const DECAL_LIFT_M: f32 = GRID_PAINT_LIFT_M + 0.005;
/// A decal is cut into columns this wide across the road and rows this
/// long along it, so it follows the crown and the camber instead of
/// bridging them as one flat quad.
const DECAL_COLUMN_M: f32 = 1.0;
const DECAL_ROW_M: f32 = 0.5;
/// Pit-lane lines ride on the lifted lane deck.
const PIT_LINE_LIFT_M: f32 = PIT_LIFT_M + MARKING_LIFT_M;
const CHEQUER_CHECK_M: f32 = 0.5;
const CHEQUER_DARK: [f32; 4] = [0.08, 0.08, 0.09, 1.0];
const GRID_BOX_LEN_M: f32 = 5.0;
const GRID_BOX_WIDTH_M: f32 = 2.6;
const GRID_POLE_STUB_M: f32 = 1.0;
const PIT_DASH_M: f32 = 3.0;
const PIT_GAP_M: f32 = 3.0;
/// A pit lane counts as running parallel to the road (dashed line, no
/// merge) once its near edge is this far off the road edge.
const PIT_PARALLEL_GAP_M: f32 = 1.5;

/// Rubbered racing line: the worn core and the softer edge strips.
const WEAR_CORE_HALF_M: f32 = 1.2;
const WEAR_EDGE_M: f32 = 0.8;
const WEAR_LIFT_M: f32 = 0.02;
/// The worn band is soft-edged and smooth; road tessellation would double
/// its vertex count for nothing.
const WEAR_STEP_M: f32 = 2.0;
const WEAR_CORE_COLOR: [f32; 4] = [0.12, 0.12, 0.13, 1.0];
const WEAR_EDGE_COLOR: [f32; 4] = [0.17, 0.17, 0.18, 1.0];

// Fallback starting grid, mirroring
// `server/src/track_loader.rs::generate_start_positions` so the client puts
// its `PlayerStart`s exactly where the server will put the cars.
/// The fallback grid, matching the server's: rows of two 8 m apart, the
/// second car of each row staggered half a row back, the columns 4 m
/// apart. A full Formula 1 stagger (8 m per position) reaches 120 m back,
/// which at Le Mans is into the Ford chicane.
const GRID_SLOTS: u32 = 16;
const GRID_SPACING_M: f32 = 8.0;
const GRID_STAGGER_M: f32 = 4.0;
const GRID_LATERAL_M: f32 = 4.0;

// ---------------------------------------------------------------------------
// Output model
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeScene {
    pub format: String,
    pub version: u32,
    #[serde(default)]
    pub track_id: Option<String>,
    pub track_name: String,
    /// File name of the logical YAML this was baked from.
    pub source_track: String,
    pub closed_loop: bool,
    pub length_cm: f32,
    pub metadata: UeMetadata,
    /// Season and spectators: which kit variants the importer picks
    /// (`_autumn` trees, `_crowd` stands). The props keep their base keys.
    pub dressing: UeDressing,
    /// Every material key referenced by `meshes`, with what the commandlet
    /// needs to generate a material for it. Sorted by key.
    pub materials: Vec<UeMaterial>,
    pub meshes: Vec<UeMesh>,
    pub props: Vec<UeProp>,
    /// Starting grid, in finishing-order slots (slot 1 is pole).
    pub grid: Vec<UeGridSlot>,
    /// The sampled centerline, for spline-following, minimaps and AI.
    pub centerline: Vec<UeCenterlinePoint>,
    #[serde(default)]
    pub pit_lane: Option<UePitLane>,
    /// The start/finish line, for the start-light gantry: the centre of the
    /// line on the road surface, the direction of travel, and the road
    /// width there.
    #[serde(default)]
    pub start_finish: Option<UeStartFinish>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeStartFinish {
    /// UE centimeters, on the road surface at the line's centre.
    pub location: [f32; 3],
    /// Direction of travel, same convention as props.
    pub yaw_deg: f32,
    /// Full road width at the line, meters.
    pub width_m: f32,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct UeMetadata {
    #[serde(default)]
    pub country: Option<String>,
    #[serde(default)]
    pub city: Option<String>,
    #[serde(default)]
    pub category: Option<String>,
    #[serde(default)]
    pub environment_type: Option<String>,
}

/// How a material key should look. The commandlet generates one material
/// instance per key; `base_color` is what the editor previewed, which is at
/// least a legible stand-in until real materials exist in the project.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeMaterial {
    pub key: String,
    /// `road`, `curb`, `surface`, `marking`, `decal` or `pit_lane` — lets the
    /// commandlet pick a parent material per family.
    pub family: String,
    /// Linear RGBA.
    pub base_color: [f32; 4],
}

/// One bakeable static mesh. Buffers are flattened (`[x, y, z, x, y, z, …]`)
/// because nesting them triples the JSON size for no gain.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeMesh {
    /// `{material_key}_{section:03}`, unique within the scene.
    pub name: String,
    pub material_key: String,
    /// Vertex positions, UE centimeters.
    pub positions: Vec<f32>,
    pub normals: Vec<f32>,
    /// `u` runs along the track in meters, `v` across it in meters. Left in
    /// world scale so one material tiles identically on every track.
    pub uvs: Vec<f32>,
    /// Triangle list, already wound for Unreal's left-handed frame.
    pub indices: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct UeDressing {
    /// `summer` or `autumn`.
    pub season: String,
    pub spectators: bool,
}

impl From<Dressing> for UeDressing {
    fn from(d: Dressing) -> Self {
        UeDressing {
            season: d.season.label().to_string(),
            spectators: d.spectators,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeProp {
    pub kind: String,
    /// Asset key the commandlet maps to a mesh or blueprint.
    pub asset: String,
    /// UE centimeters.
    pub location: [f32; 3],
    pub yaw_deg: f32,
    pub scale: f32,
    #[serde(default)]
    pub text: Option<String>,
    /// Grandstands: the stand's length along its heading, metres; the
    /// importer lays bays from it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub length_m: Option<f32>,
    /// Grandstands: the signed centerline radius at the stand's station,
    /// metres — positive when the stand is on the outside of the bend,
    /// negative on the inside, absent on a straight. Picks the wedge bay.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radius_m: Option<f32>,
    /// Bridges: the road width at the prop's station, metres, which the
    /// importer scales the 15 m span to.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub span_m: Option<f32>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeGridSlot {
    /// 1-based; slot 1 is pole.
    pub position: u32,
    pub location: [f32; 3],
    pub yaw_deg: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UeCenterlinePoint {
    /// Station along the centerline, UE centimeters.
    pub s_cm: f32,
    pub location: [f32; 3],
    pub yaw_deg: f32,
    pub half_left_cm: f32,
    pub half_right_cm: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct UePitLane {
    pub width_cm: f32,
    pub box_count: u32,
    pub speed_limit_kmh: f32,
}

/// Everything one bake produces: the Unreal scene and the server's
/// sidecars — the ground heightfield (absent only for a track too
/// degenerate to have a terrain), the curb bands and the walls.
pub struct Baked {
    pub scene: UeScene,
    pub ground: Option<GroundHeightfield>,
    pub curbs: Option<CurbBands>,
    pub walls: Walls,
}

/// Station spacing of the curb sidecar, meters. Curbs run for tens of
/// meters, so a metre of granularity at their ends costs nothing.
pub const CURB_BAND_STEP_M: f32 = 1.0;
pub const CURB_BANDS_VERSION: u32 = 2;

/// The curbs as the *server* needs them (`<Track>.curbs.msgpack`, written
/// with `rmp_serde::to_vec_named`): not geometry, just how far the curb
/// reaches out from each road edge, so the sim can count a car on the curb
/// as on the track instead of off it.
///
/// Sample `i` is the cross-section at station `i * step_m` along the
/// centerline, the value the curb's width outward from that side's road
/// edge in centimeters (0 where the edge has no curb). Left and right are
/// the track's own sides — the same `width_left_m` / `width_right_m` the
/// YAML centerline carries.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CurbBands {
    pub version: u32,
    pub step_m: f32,
    pub left_cm: Vec<u16>,
    pub right_cm: Vec<u16>,
    /// Version 2: how far the prepared *tarmac* run-off (`asphalt_runoff`,
    /// `concrete` bands) reaches past each road edge, centimetres, 0 where
    /// there is none. A car past the curb but within this is on tarmac —
    /// off the track for the lap, but on a surface with grip and no
    /// grass drag.
    #[serde(default)]
    pub runoff_left_cm: Vec<u16>,
    #[serde(default)]
    pub runoff_right_cm: Vec<u16>,
}

/// Flatten the scene's curb spans into the per-station bands the server
/// reads.
///
/// Overlapping curbs on a side keep the widest, and a curb shorter than a
/// step still claims the sample nearest its middle rather than vanishing.
fn bake_curb_bands(
    path: &CenterlinePath,
    curbs: &[Curb],
    surfaces: &[Surface],
) -> Option<CurbBands> {
    let total = path.total_length_m();
    if !(total.is_finite() && total > 0.0) {
        return None;
    }
    // A closed loop wraps at `count`, so the last sample must be one step
    // short of the line; an open path ends *on* it.
    let count = if path.is_closed() {
        (total / CURB_BAND_STEP_M).ceil().max(1.0) as usize
    } else {
        (total / CURB_BAND_STEP_M).floor() as usize + 1
    };
    let mut left = vec![0u16; count];
    let mut right = vec![0u16; count];

    for curb in curbs {
        let Some((start, end)) = resolve_span(path, curb.start_m, curb.end_m) else {
            continue;
        };
        // A NaN width lands here as 0 (`f32::max` prefers the number) and
        // is dropped with the rest of the sub-centimeter curbs.
        let width_cm = (curb.width_m.max(0.0) * 100.0).round();
        if width_cm < 1.0 {
            continue;
        }
        let width_cm = width_cm.min(u16::MAX as f32) as u16;
        let band = match curb.side {
            Side::Left => &mut left,
            Side::Right => &mut right,
        };
        let first = (start / CURB_BAND_STEP_M).ceil() as i64;
        let last = (end / CURB_BAND_STEP_M).floor() as i64;
        let (first, last) = if last < first {
            let mid = ((start + end) * 0.5 / CURB_BAND_STEP_M).round() as i64;
            (mid, mid)
        } else {
            (first, last)
        };
        for i in first..=last {
            let idx = i.rem_euclid(count as i64) as usize;
            band[idx] = band[idx].max(width_cm);
        }
    }

    // The tarmac run-off: the outer reach of every asphalt or concrete
    // band, sampled at the same stations.
    let mut runoff_left = vec![0u16; count];
    let mut runoff_right = vec![0u16; count];
    for surface in surfaces {
        if !matches!(
            surface.kind,
            crate::ats::SurfaceKind::AsphaltRunoff | crate::ats::SurfaceKind::Concrete
        ) {
            continue;
        }
        let Some((start, end)) = resolve_span(path, surface.start_m, surface.end_m) else {
            continue;
        };
        let band = match surface.side {
            Side::Left => &mut runoff_left,
            Side::Right => &mut runoff_right,
        };
        let first = (start / CURB_BAND_STEP_M).ceil() as i64;
        let last = (end / CURB_BAND_STEP_M).floor() as i64;
        if last < first {
            continue;
        }
        for i in first..=last {
            let station = i as f32 * CURB_BAND_STEP_M;
            let t = ((station - start) / (end - start).max(1e-3)).clamp(0.0, 1.0);
            let reach_cm = ((surface.inner_m + surface.width_at(t)).max(0.0) * 100.0).round();
            if reach_cm < 1.0 {
                continue;
            }
            let idx = i.rem_euclid(count as i64) as usize;
            band[idx] = band[idx].max(reach_cm.min(u16::MAX as f32) as u16);
        }
    }

    Some(CurbBands {
        version: CURB_BANDS_VERSION,
        runoff_left_cm: runoff_left,
        runoff_right_cm: runoff_right,
        step_m: CURB_BAND_STEP_M,
        left_cm: left,
        right_cm: right,
    })
}

// ---------------------------------------------------------------------------
// Walls for the server
// ---------------------------------------------------------------------------

pub const WALLS_VERSION: u32 = 1;
/// Steel armco or a fence: springy.
pub const WALL_KIND_ARMCO: u8 = 0;
/// Tires or TecPro: absorbs the hit.
pub const WALL_KIND_TIRES: u8 = 1;
/// Concrete, a building, a stand, a parapet: hard.
pub const WALL_KIND_CONCRETE: u8 = 2;
/// A sausage kerb: driven over, jolts the car, never stops it.
pub const WALL_KIND_KERB: u8 = 3;

/// Two thin walls whose ends are this close along the run are joined, so
/// a run of 4 m armco modules or tire-wall blocks laid a little apart
/// reads as one continuous barrier and a car cannot slip between them.
const WALL_JOIN_GAP_M: f32 = 4.5;
/// …provided the ends are within this much of each other sideways…
const WALL_JOIN_SIDESTEP_M: f32 = 1.0;
/// …and the two run within about 35° of each other.
const WALL_JOIN_ALIGN_COS: f32 = 0.82;
/// A pit module deeper than this is a garage (a footprint), not a wall.
const PIT_WALL_MAX_DEPTH_M: f32 = 2.5;
/// Station step the underpass walls and parapets are sampled at.
const UNDERPASS_WALL_STEP_M: f32 = 2.0;
/// An underpass wall is written short of the upper ground by this much,
/// so a car on the embankment above — its wheels at the wall's top — is
/// not level with it; and dropped where less than a bumper's height is
/// left.
const UNDERPASS_WALL_TRIM_M: f32 = 1.0;
const UNDERPASS_WALL_MIN_M: f32 = 0.3;
/// Footprint of a building the kit does not know, metres.
const UNKNOWN_BUILDING_FOOTPRINT: (f32, f32, f32) = (15.0, 10.0, 8.0);

/// One solid face in the ground plane, track frame (metres, +Y left):
/// the ground height at its base, how tall it is, and what it is made of
/// (`WALL_KIND_*`).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct WallSegment {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    pub z: f32,
    pub height_m: f32,
    pub kind: u8,
}

/// The walls as the *server* needs them (`<Track>.walls.msgpack`, written
/// with `rmp_serde::to_vec_named`): not meshes, just where the solid faces
/// are, so the sim can stop a car at the armco the player sees instead of
/// letting it drive through.
///
/// A barrier, tire wall, fence or pit wall is one segment along its
/// heading; a grandstand, building, garage or fairground piece is the four
/// sides of its footprint, laid behind the pivot where the mesh stands
/// behind it; an underpass contributes the abutment walls
/// beside the lower road and the deck's parapets. Heights let the server
/// tell a wall the car is level with from one on the road above or below.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Walls {
    pub version: u32,
    pub segments: Vec<WallSegment>,
}

/// Unreal centimetres back to the track frame.
fn from_ue(p: [f32; 3]) -> (f32, f32, f32) {
    (p[0] / M_TO_CM, -p[1] / M_TO_CM, p[2] / M_TO_CM)
}

/// A wall of `length_m` centred on `at`, running along `yaw`.
fn wall_face(
    at: (f32, f32),
    yaw: f32,
    length_m: f32,
    z: f32,
    height_m: f32,
    kind: u8,
) -> WallSegment {
    let (sin, cos) = yaw.sin_cos();
    let (hx, hy) = (cos * length_m / 2.0, sin * length_m / 2.0);
    WallSegment {
        x0: at.0 - hx,
        y0: at.1 - hy,
        x1: at.0 + hx,
        y1: at.1 + hy,
        z,
        height_m,
        kind,
    }
}

/// The deep kit meshes stand with their pivot on the road-facing edge and
/// reach away from the road; these few are centred on it instead.
pub(crate) fn footprint_is_centred(kind: PropKind, asset: &str) -> bool {
    matches!(
        (kind, asset),
        (PropKind::Building, "control_tower" | "castle_ruin")
            | (
                PropKind::Attraction,
                "camera_tower" | "ferris_wheel" | "tent_6m"
            )
    )
}

/// Where a `length_m` × `depth_m` footprint pivoted at `at` is centred:
/// on the pivot for a centred asset, else half its depth behind it, away
/// from the road — the side of the prop's own normal the nearest
/// centerline point is not on.
fn footprint_centre(
    at: (f32, f32),
    yaw: f32,
    depth_m: f32,
    centred: bool,
    path: &CenterlinePath,
) -> (f32, f32) {
    if centred {
        return at;
    }
    let (sin, cos) = yaw.sin_cos();
    let normal = (-sin, cos);
    let (sample, _) = nearest_sample(path, at.0, at.1);
    let toward = (at.0 - sample.pos.0) * normal.0 + (at.1 - sample.pos.1) * normal.1;
    let away = if toward >= 0.0 { 1.0 } else { -1.0 };
    (
        at.0 + away * normal.0 * depth_m / 2.0,
        at.1 + away * normal.1 * depth_m / 2.0,
    )
}

/// The four sides of a `length_m` × `depth_m` footprint centred on `at`,
/// its length along `yaw`.
fn wall_footprint(
    at: (f32, f32),
    yaw: f32,
    length_m: f32,
    depth_m: f32,
    z: f32,
    height_m: f32,
    kind: u8,
) -> [WallSegment; 4] {
    let (sin, cos) = yaw.sin_cos();
    let (ux, uy) = (cos * length_m / 2.0, sin * length_m / 2.0);
    let (vx, vy) = (-sin * depth_m / 2.0, cos * depth_m / 2.0);
    let corner = |su: f32, sv: f32| (at.0 + su * ux + sv * vx, at.1 + su * uy + sv * vy);
    let corners = [
        corner(-1.0, -1.0),
        corner(1.0, -1.0),
        corner(1.0, 1.0),
        corner(-1.0, 1.0),
    ];
    let side = |a: (f32, f32), b: (f32, f32)| WallSegment {
        x0: a.0,
        y0: a.1,
        x1: b.0,
        y1: b.1,
        z,
        height_m,
        kind,
    };
    [
        side(corners[0], corners[1]),
        side(corners[1], corners[2]),
        side(corners[2], corners[3]),
        side(corners[3], corners[0]),
    ]
}

/// What a barrier asset is made of, by its key.
fn barrier_material(kind: PropKind, asset: &str) -> u8 {
    match kind {
        PropKind::TireWall => WALL_KIND_TIRES,
        PropKind::Barrier if asset.starts_with("sausage_kerb") => WALL_KIND_KERB,
        PropKind::Barrier if asset.starts_with("tecpro") => WALL_KIND_TIRES,
        PropKind::Barrier if asset.starts_with("concrete") => WALL_KIND_CONCRETE,
        _ => WALL_KIND_ARMCO,
    }
}

/// The walls the exported props imply (see [`Walls`]), plus an underpass's
/// structure when the terrain has one.
fn bake_walls(
    props: &[UeProp],
    path: &CenterlinePath,
    terrain: Option<&TerrainHeightfield>,
) -> Walls {
    let mut thin: Vec<WallSegment> = Vec::new();
    let mut solid: Vec<WallSegment> = Vec::new();
    for prop in props {
        let Some(kind) = PropKind::ALL
            .iter()
            .copied()
            .find(|k| k.label() == prop.kind)
        else {
            continue;
        };
        let (x, y, z) = from_ue(prop.location);
        let yaw = -prop.yaw_deg.to_radians();
        let kit = props::resolve(kind, &prop.asset);
        let scale = if prop.scale.is_finite() && prop.scale > 0.0 {
            prop.scale
        } else {
            1.0
        };
        match kind {
            PropKind::Barrier | PropKind::Fence | PropKind::TireWall => {
                let (length, height) = kit.map_or((4.0, 1.0), |a| (a.length_m, a.height_m));
                thin.push(wall_face(
                    (x, y),
                    yaw,
                    length * scale,
                    z,
                    height,
                    barrier_material(kind, &prop.asset),
                ));
            }
            PropKind::Pit => {
                // The crew's kit stands on the working lane; a car may
                // drive over it rather than crash into it.
                let Some(a) = kit.filter(|_| prop.asset != "box_kit") else {
                    continue;
                };
                if a.depth_m <= PIT_WALL_MAX_DEPTH_M {
                    thin.push(wall_face(
                        (x, y),
                        yaw,
                        a.length_m * scale,
                        z,
                        a.height_m,
                        WALL_KIND_CONCRETE,
                    ));
                } else {
                    let depth = a.depth_m * scale;
                    solid.extend(wall_footprint(
                        footprint_centre((x, y), yaw, depth, false, path),
                        yaw,
                        a.length_m * scale,
                        depth,
                        z,
                        a.height_m,
                        WALL_KIND_CONCRETE,
                    ));
                }
            }
            PropKind::Grandstand => {
                // Scale is a length multiplier for a stand; the bays'
                // depth is the family's.
                let length = prop.length_m.unwrap_or(STAND_DEFAULT_LENGTH_M) * scale;
                let (depth, height) = kit.map_or((12.0, 8.0), |a| (a.depth_m, a.height_m));
                solid.extend(wall_footprint(
                    footprint_centre((x, y), yaw, depth, false, path),
                    yaw,
                    length,
                    depth,
                    z,
                    height,
                    WALL_KIND_CONCRETE,
                ));
            }
            PropKind::Building | PropKind::Attraction => {
                let (length, depth, height) = match kit {
                    Some(a) => (a.length_m, a.depth_m, a.height_m),
                    None if kind == PropKind::Building => UNKNOWN_BUILDING_FOOTPRINT,
                    None => continue,
                };
                let depth = depth * scale;
                let centred = footprint_is_centred(kind, &prop.asset);
                solid.extend(wall_footprint(
                    footprint_centre((x, y), yaw, depth, centred, path),
                    yaw,
                    length * scale,
                    depth,
                    z,
                    height,
                    WALL_KIND_CONCRETE,
                ));
            }
            // Trees, signs, lights, cones, boards (on the barrier line
            // already), bridges (their footings are off the verge),
            // vehicles and sky props stop nothing.
            _ => {}
        }
    }
    join_wall_runs(&mut thin);
    if let Some(field) = terrain {
        underpass_walls(field, &mut solid);
    }
    let mut segments = thin;
    segments.extend(solid);
    Walls {
        version: WALLS_VERSION,
        segments,
    }
}

/// Close the small gaps in runs of thin walls: an end that has another
/// wall's end within [`WALL_JOIN_GAP_M`] straight ahead of it, roughly in
/// line, is moved to halfway between them (and that end is moved to the
/// same place), so the run is continuous.
fn join_wall_runs(walls: &mut [WallSegment]) {
    let ends: Vec<[(f32, f32); 2]> = walls.iter().map(|w| [(w.x0, w.y0), (w.x1, w.y1)]).collect();
    let dirs: Vec<(f32, f32)> = walls
        .iter()
        .map(|w| {
            let len = (w.x1 - w.x0).hypot(w.y1 - w.y0).max(1e-6);
            ((w.x1 - w.x0) / len, (w.y1 - w.y0) / len)
        })
        .collect();
    let mut moved: Vec<[Option<(f32, f32)>; 2]> = vec![[None, None]; walls.len()];
    for i in 0..walls.len() {
        for end in 0..2 {
            let p = ends[i][end];
            // Which way is "beyond" this end.
            let out = if end == 1 {
                dirs[i]
            } else {
                (-dirs[i].0, -dirs[i].1)
            };
            let mut best: Option<(f32, (f32, f32))> = None;
            for j in 0..walls.len() {
                // A kerb is not a barrier: it never seals a gap in a run.
                if j == i
                    || walls[i].kind == WALL_KIND_KERB
                    || walls[j].kind == WALL_KIND_KERB
                    || (dirs[i].0 * dirs[j].0 + dirs[i].1 * dirs[j].1).abs() < WALL_JOIN_ALIGN_COS
                {
                    continue;
                }
                for q in ends[j] {
                    let (dx, dy) = (q.0 - p.0, q.1 - p.1);
                    let along = dx * out.0 + dy * out.1;
                    let aside = (dx * out.1 - dy * out.0).abs();
                    let gap = dx.hypot(dy);
                    if along <= 0.0 || gap > WALL_JOIN_GAP_M || aside > WALL_JOIN_SIDESTEP_M {
                        continue;
                    }
                    if best.is_none_or(|(g, _)| gap < g) {
                        best = Some((gap, q));
                    }
                }
            }
            if let Some((_, q)) = best {
                moved[i][end] = Some(((p.0 + q.0) / 2.0, (p.1 + q.1) / 2.0));
            }
        }
    }
    for (wall, m) in walls.iter_mut().zip(moved) {
        if let Some((x, y)) = m[0] {
            (wall.x0, wall.y0) = (x, y);
        }
        if let Some((x, y)) = m[1] {
            (wall.x1, wall.y1) = (x, y);
        }
    }
}

/// A polyline sampled along `path` at `step`, as segments into `out`.
fn wall_polyline(
    path: &CenterlinePath,
    from: f32,
    to: f32,
    step: f32,
    mut point: impl FnMut(&PathSample) -> Option<(f32, f32, f32, f32)>,
    kind: u8,
    out: &mut Vec<WallSegment>,
) {
    let mut prev: Option<(f32, f32, f32, f32)> = None;
    let mut station = from;
    loop {
        let here = point(&path.sample_at(station));
        if let (Some(a), Some(b)) = (prev, here) {
            let height = a.3.max(b.3);
            if height >= UNDERPASS_WALL_MIN_M {
                out.push(WallSegment {
                    x0: a.0,
                    y0: a.1,
                    x1: b.0,
                    y1: b.1,
                    z: a.2.min(b.2),
                    height_m: height,
                    kind,
                });
            }
        }
        prev = here;
        if station >= to {
            break;
        }
        station = (station + step).min(to);
    }
}

/// The abutment walls beside the lower road (where the export draws them)
/// and the parapets along the deck.
fn underpass_walls(field: &TerrainHeightfield, out: &mut Vec<WallSegment>) {
    let gap = terrain::UNDERPASS_WALL_GAP_M;
    let over = terrain::DECK_OVERHANG_M;
    for u in field.underpasses() {
        let lower = field.road_path(u.lower_road);
        let upper = field.road_path(u.upper_road);
        for outward in [1.0f32, -1.0] {
            let edge = move |s: &PathSample| {
                if outward > 0.0 {
                    s.width_left_m
                } else {
                    -s.width_right_m
                }
            };
            for (from, to) in wall_runs(field, lower, u, outward) {
                wall_polyline(
                    lower,
                    from,
                    to,
                    UNDERPASS_WALL_STEP_M,
                    |s| {
                        let face = offset_point(s, edge(s) + outward * gap);
                        let floor = offset_point(s, edge(s) + outward * (gap - 0.1));
                        let bank = offset_point(s, edge(s) + outward * (gap + 0.1));
                        let z = field.ground_height_at(floor.0, floor.1);
                        let top = field.ground_height_at(bank.0, bank.1);
                        Some((face.0, face.1, z, top - z - UNDERPASS_WALL_TRIM_M))
                    },
                    WALL_KIND_CONCRETE,
                    out,
                );
            }
            let (start, end) = u.deck_span_m;
            wall_polyline(
                upper,
                start,
                end,
                UNDERPASS_WALL_STEP_M,
                |s| {
                    let p = offset_point(s, edge(s) + outward * over);
                    Some((p.0, p.1, p.2, PARAPET_HEIGHT_M))
                },
                WALL_KIND_CONCRETE,
                out,
            );
        }
    }
}

// ---------------------------------------------------------------------------
// Baking
// ---------------------------------------------------------------------------

/// Bake a loaded track and its scene into the Unreal export.
///
/// Returns `None` only for a degenerate track the centerline sampler
/// rejects (fewer than two nodes, or zero length).
pub fn bake(track: &TrackFile, scene: &AtsScene) -> Option<UeScene> {
    bake_all(track, scene).map(|b| b.scene)
}

/// [`bake`], plus the sidecars for the server.
pub fn bake_all(track: &TrackFile, scene: &AtsScene) -> Option<Baked> {
    bake_all_with_dem(track, scene, None)
}

/// [`bake_all`] over the real land.
///
/// With a track's elevation sidecar the ground is the land the circuit is
/// actually built on and the skyline is the real one; without it the
/// ground is the centerline blanket it always was and there is no
/// skyline at all.
pub fn bake_all_with_dem(
    track: &TrackFile,
    scene: &AtsScene,
    dem: Option<&DemFile>,
) -> Option<Baked> {
    let path = CenterlinePath::from_track(track)?;
    let lane = scene
        .pit_lane
        .as_ref()
        .and_then(|pit| CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0));
    let extra: Vec<&CenterlinePath> = lane.iter().collect();
    let terrain = TerrainHeightfield::from_paths_with_dem(&path, &extra, dem);

    let mut bake = Bake {
        ground: terrain.as_ref(),
        chunks: Vec::new(),
        materials: BTreeMap::new(),
    };

    // The pit lane's relation to the road decides where the road's edge
    // line breaks for the entry and exit, so it is resolved first.
    let lane_relation = lane
        .as_ref()
        .map(|lane| LaneRelation::resolve(&path, lane, terrain.as_ref()))
        .unwrap_or_default();

    bake.road(&path);
    bake.edge_lines(&path, &lane_relation.edge_gaps);
    // Ground first, then the track, then what sits on it — the same layering
    // the viewport uses, so the export reads the way the editor looked.
    if let Some(field) = &terrain {
        bake.ground(field);
        // The skyline goes in before anything that stands on the ground,
        // and only when the circuit has an elevation model to draw it
        // from; without one there is nothing out there to draw.
        if let Some(dem) = dem {
            bake.horizon(field, dem);
        }
        for underpass in field.underpasses() {
            bake.underpass(field, underpass);
        }
    }
    for surface in &scene.surfaces {
        bake.surface(&path, surface);
    }
    for curb in &scene.curbs {
        bake.curb(&path, curb);
    }
    for marking in &scene.markings {
        bake.marking(&path, marking);
    }
    for decal in &scene.decals {
        bake.decal(&path, decal);
    }
    bake.grid_boxes(track, &path);
    bake.drs_lines(track, &path);
    bake.wear(track, &path);
    let pit_lane = scene.pit_lane.as_ref().and_then(|pit| {
        let lane = lane.as_ref()?;
        bake.pit_lane(lane, pit.width_m);
        bake.pit_markings(lane, pit.width_m, pit.box_count, &lane_relation);
        bake.pit_exit_line(&path, &lane_relation);
        Some(UePitLane {
            width_cm: round(pit.width_m * M_TO_CM, 1),
            box_count: pit.box_count,
            speed_limit_kmh: round(pit.speed_limit_kmh, 2),
        })
    });

    let metadata = track
        .metadata
        .as_ref()
        .map(|m| UeMetadata {
            country: m.country.clone(),
            city: m.city.clone(),
            category: m.category.clone(),
            environment_type: m.environment_type.clone(),
        })
        .unwrap_or_default();

    let ground = terrain
        .as_ref()
        .map(|field| field.bake_ground_sidecar(terrain::GROUND_SIDECAR_CELL_M));
    let curbs = bake_curb_bands(&path, &scene.curbs, &scene.surfaces);
    let props = bake_props(
        track,
        scene,
        &path,
        lane.as_ref(),
        &lane_relation,
        terrain.as_ref(),
    );
    let walls = bake_walls(&props, &path, terrain.as_ref());

    let scene = UeScene {
        format: UE_SCENE_FORMAT.to_string(),
        version: UE_SCENE_VERSION,
        track_id: track.track_id.clone(),
        track_name: track.name.clone(),
        source_track: scene.source_track.clone(),
        closed_loop: track.closed_loop,
        length_cm: round(path.total_length_m() * M_TO_CM, 1),
        metadata,
        dressing: scene.dressing.into(),
        materials: bake.materials.into_values().collect(),
        meshes: merge_chunks(bake.chunks),
        props,
        grid: bake_grid(track, &path),
        centerline: bake_centerline(&path),
        pit_lane,
        start_finish: Some(bake_start_finish(scene, &path)),
    };
    Some(Baked {
        scene,
        ground,
        curbs,
        walls,
    })
}

// ---------------------------------------------------------------------------
// Strip extrusion
// ---------------------------------------------------------------------------

/// What a profile point's height is measured from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Anchor {
    /// The road surface at the point's lateral (centerline + banking
    /// shear), as mapped by the strip's `height` function.
    Road,
    /// The ground that is actually there: [`TerrainHeightfield::ground_height_at`].
    /// Near an underpass this is seated on the level the element belongs
    /// to (see [`seat_on_ground`]).
    Ground,
    /// The ground field as it is, whatever level the element is on, but
    /// never above the underside of a bridge deck: for the underpass's own
    /// walls and slot.
    Terrain,
}

/// One point of a strip's cross-section: lateral offset from the centerline
/// (positive = left) and height above its anchor, both meters.
#[derive(Clone, Copy, Debug)]
struct ProfilePoint {
    lat_m: f32,
    lift_m: f32,
    anchor: Anchor,
}

impl ProfilePoint {
    fn lifted(lat_m: f32, lift_m: f32) -> Self {
        Self {
            lat_m,
            lift_m,
            anchor: Anchor::Road,
        }
    }

    fn grounded(lat_m: f32, lift_m: f32) -> Self {
        Self {
            lat_m,
            lift_m,
            anchor: Anchor::Ground,
        }
    }

    fn terrain(lat_m: f32, lift_m: f32) -> Self {
        Self {
            lat_m,
            lift_m,
            anchor: Anchor::Terrain,
        }
    }
}

/// How a strip point sits against an underpass.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Seat {
    /// An element of the level above, draped down into the slot: its facets
    /// are not drawn.
    hanging: bool,
    /// `Some(inside the slot)` near an underpass wall, `None` elsewhere.
    slot: Option<bool>,
}

/// Where a ground-anchored point of an element on `sample`'s stretch of road
/// goes. Away from an underpass that is simply the ground. Near one, the
/// ground can belong to the other level: an element of the upper road over
/// the deck sits on the deck, and one over the slot beyond the deck hangs.
fn seat_on_ground(
    field: &TerrainHeightfield,
    sample: &PathSample,
    pos: (f32, f32, f32),
) -> (f32, Seat) {
    let ground = field.ground_height_at(pos.0, pos.1);
    let own = sample.pos.2;
    if let Some(top) = field.deck_top_at(pos.0, pos.1) {
        if (own - top).abs() <= terrain::OVERHEAD_M && ground < top - terrain::OVERHEAD_M {
            return (top - terrain::VERGE_DROP_M, Seat::default());
        }
    }
    let Some((past_wall, _)) = field.wall_relation(pos.0, pos.1) else {
        return (ground, Seat::default());
    };
    let inside = past_wall < 0.0;
    let seat = Seat {
        hanging: inside && ground < own - terrain::OVERHEAD_M,
        slot: Some(inside),
    };
    (ground, seat)
}

/// [`band_reach`] at every cross-section a band's strip will have (the
/// same stations `extrude` walks), then limited so it never grows faster
/// than [`BAND_REACH_SLOPE`] per metre of course, either way. A band's
/// columns are fractions of its width, so a width that jumped from one
/// cross-section to the next strung quads diagonally across whatever lay
/// between — the road the cap was there to keep clear of. Taking the
/// lower envelope only ever narrows the band.
fn band_reach_profile(
    field: &TerrainHeightfield,
    path: &CenterlinePath,
    surface: &Surface,
) -> Option<Vec<f32>> {
    let (start, end) = resolve_span(path, surface.start_m, surface.end_m)?;
    let span = end - start;
    let steps = ((span / SURFACE_STEP_M).ceil() as usize).max(1);
    let step_len = span / steps as f32;
    let mut reach: Vec<f32> = (0..=steps)
        .map(|i| {
            let progress = i as f32 / steps as f32;
            let sample = path.sample_at(start + span * progress);
            let (edge, outward) = match surface.side {
                Side::Left => (sample.width_left_m, 1.0),
                Side::Right => (-sample.width_right_m, -1.0),
            };
            band_reach(
                field,
                &sample,
                edge,
                outward,
                surface.inner_m,
                surface.width_at(progress),
            )
        })
        .collect();
    let grow = BAND_REACH_SLOPE * step_len;
    // A full lap wraps: two sweeps each way carry a narrow spot across
    // start/finish.
    let lap = path.is_closed() && (span - path.total_length_m()).abs() < 1.0;
    for _ in 0..if lap { 2 } else { 1 } {
        for i in 1..=steps {
            reach[i] = reach[i].min(reach[i - 1] + grow);
        }
        if lap {
            reach[0] = reach[0].min(reach[steps] + grow);
        }
        for i in (0..steps).rev() {
            reach[i] = reach[i].min(reach[i + 1] + grow);
        }
        if lap {
            reach[steps] = reach[steps].min(reach[0] + grow);
        }
    }
    Some(reach)
}

/// How fast a band's width may change along the course, metres of width
/// per metre of course: see [`band_reach_profile`].
const BAND_REACH_SLOPE: f32 = 0.5;

/// How far a ground band laid beside `sample` may reach before it would
/// cover another section of the course, metres of band width (at most
/// `width`).
///
/// A grass apron is 130 m wide and drawn with columns 10 m apart past the
/// first 40 m. Where another leg of the circuit runs within that — the
/// far side of a hairpin, a straight alongside — a single quad spanned
/// the other road from verge to verge, a plane at verge height that the
/// road's own crown, camber or banking rose through and dipped under: the
/// "terrain over the track" seen from the car. The band now stops at the
/// line halfway between its own road edge and the other road's, so the two
/// sections' bands meet there instead of stacking. The pit lane counts as
/// another road: a run-off band laid across its merge covered it.
fn band_reach(
    field: &TerrainHeightfield,
    sample: &PathSample,
    edge: f32,
    outward: f32,
    inner: f32,
    width: f32,
) -> f32 {
    /// Probe spacing across the band.
    const PROBE_M: f32 = 2.0;
    /// How far a probe looks for another section.
    const SEARCH_M: f32 = 160.0;
    /// Around a crossover the underpass owns the ground — the slot, the
    /// embankment, the deck and the bands dropped over them — and it is
    /// built on the band's columns where they always were: squeezed, a
    /// quad hung over the lower road (Suzuka). Bands this close to one
    /// keep their full width.
    const UNDERPASS_KEEP_M: f32 = 200.0;
    if field
        .underpasses()
        .iter()
        .any(|u| (sample.pos.0 - u.at.0).hypot(sample.pos.1 - u.at.1) < UNDERPASS_KEEP_M)
    {
        return width;
    }
    let mut reached = 0.0f32;
    let mut d = 0.0f32;
    while d <= width {
        let beyond = inner + d;
        let lat = edge + outward * beyond;
        let (x, y, _) = offset_point(sample, lat);
        if let Some((road, station, other_lat, other_half)) =
            field.nearest_road_point(x, y, SEARCH_M)
        {
            // Its own cross-section stays nearest out to the fold of a
            // bend; anything else is another stretch of the course, or the
            // pit lane.
            let other =
                road != 0 || station_gap_m(field, station, sample.station_m) > 5.0 + 0.5 * beyond;
            if other {
                let to_other_edge = other_lat.abs() - other_half;
                if to_other_edge <= beyond {
                    return reached.min(width);
                }
            }
        }
        reached = d;
        d += PROBE_M;
    }
    width
}

/// Distance between two stations along the track, the short way round on
/// a closed course.
fn station_gap_m(field: &TerrainHeightfield, a: f32, b: f32) -> f32 {
    let path = field.road_path(0);
    let d = (a - b).abs();
    if path.is_closed() {
        d.min(path.total_length_m() - d)
    } else {
        d
    }
}

/// One drawable cross-section, in track space.
struct CrossSection {
    points: Vec<(f32, f32, f32)>,
    /// Per point, how it sits against an underpass.
    seats: Vec<Seat>,
    /// Lateral arc length at each point, for the `v` texture coordinate.
    vs: Vec<f32>,
    /// Unwrapped station, so it stays monotonic across start/finish.
    station: f32,
    /// Unit course direction from the centerline heading. Unlike a
    /// direction derived from the strip's own vertices, this stays correct
    /// where the strip itself has folded, which is what makes it usable as
    /// the reference for detecting that fold.
    course: (f32, f32, f32),
}

/// Raw geometry for one (section, material) bucket, in UE units already.
struct Chunk {
    section: i32,
    material_key: String,
    positions: Vec<f32>,
    normals: Vec<f32>,
    uvs: Vec<f32>,
    indices: Vec<u32>,
}

/// The state of one bake: the ground everything is seated on, and the
/// geometry and materials accumulated so far.
struct Bake<'a> {
    ground: Option<&'a TerrainHeightfield>,
    chunks: Vec<Chunk>,
    materials: BTreeMap<String, UeMaterial>,
}

impl Bake<'_> {
    fn register(&mut self, key: &str, family: &str, base_color: [f32; 4]) {
        self.materials
            .entry(key.to_string())
            .or_insert_with(|| UeMaterial {
                key: key.to_string(),
                family: family.to_string(),
                base_color: base_color.map(|c| round(c, 4)),
            });
    }

    /// Extrude a cross-section profile along `path` from `start_m` to
    /// `end_m`.
    ///
    /// `profile` fills the cross-section at each station; it is handed the
    /// sample and the progress through the span (0 at `start_m`, 1 at
    /// `end_m`) so tapering elements can vary their width. The profile
    /// **must be ordered by non-increasing lateral offset** (left to
    /// right): outward normals are derived from that ordering, and a
    /// profile authored the other way round comes out lit from
    /// underneath. [`normalize_profile`] enforces it.
    ///
    /// `height` maps a road-anchored vertex's road-following position to
    /// its final height (before the point's own lift): the identity for
    /// road-hugging strips, the road-surface probe for the racing line.
    /// Ground-anchored points take the ground height at their XY instead.
    /// Both run *after* curvature clamping, on the point's final lateral —
    /// a height computed for a pre-clamp lateral would sit inconsistently
    /// over the clamped position and fold facets.
    ///
    /// On a closed path `end_m <= start_m` wraps through the start/finish
    /// line.
    #[allow(clippy::too_many_arguments)]
    fn strip<F>(
        &mut self,
        path: &CenterlinePath,
        start_m: f32,
        end_m: f32,
        step_m: f32,
        material_key: &str,
        profile: F,
        height: impl Fn(&PathSample, f32, (f32, f32, f32)) -> f32,
    ) where
        F: FnMut(&PathSample, f32, &mut Vec<ProfilePoint>),
    {
        self.extrude(
            path,
            start_m,
            end_m,
            step_m,
            material_key,
            profile,
            height,
            true,
        );
    }

    /// [`Self::strip`] for a closed or folded shape — a wall, a deck — whose
    /// profile is a path around the cross-section rather than a surface
    /// left to right. The order is taken as given: each segment faces the
    /// way a left-to-right surface segment would, so going up faces left,
    /// going down faces right, and going right to left faces down.
    #[allow(clippy::too_many_arguments)]
    fn shape<F>(
        &mut self,
        path: &CenterlinePath,
        start_m: f32,
        end_m: f32,
        step_m: f32,
        material_key: &str,
        profile: F,
    ) where
        F: FnMut(&PathSample, f32, &mut Vec<ProfilePoint>),
    {
        self.extrude(
            path,
            start_m,
            end_m,
            step_m,
            material_key,
            profile,
            |_, _, p| p.2,
            false,
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn extrude<F>(
        &mut self,
        path: &CenterlinePath,
        start_m: f32,
        end_m: f32,
        step_m: f32,
        material_key: &str,
        mut profile: F,
        height: impl Fn(&PathSample, f32, (f32, f32, f32)) -> f32,
        normalize: bool,
    ) where
        F: FnMut(&PathSample, f32, &mut Vec<ProfilePoint>),
    {
        let Some((start, end)) = resolve_span(path, start_m, end_m) else {
            return;
        };
        let span = end - start;
        let steps = ((span / step_m).ceil() as usize).max(1);

        // Every cross-section, in track space, before any UE conversion.
        // `None` marks a station the element cannot be drawn at — see
        // `clamp_profile` — which breaks the strip rather than distorting it.
        let mut sections: Vec<Option<CrossSection>> = Vec::with_capacity(steps + 1);
        let mut buf: Vec<ProfilePoint> = Vec::new();
        let mut profile_len = 0usize;

        for i in 0..=steps {
            // Unwrapped on purpose: a span that wraps keeps counting past the
            // total length, which is what the section index and the `u`
            // coordinate both want — each must stay monotonic across
            // start/finish.
            let station = start + span * (i as f32 / steps as f32);
            let sample = path.sample_at(station);
            buf.clear();
            profile(&sample, i as f32 / steps as f32, &mut buf);
            if buf.len() < 2 {
                return;
            }
            // Order first, then clamp: clamping is monotonic, so it cannot
            // disturb the left-to-right ordering the normals rely on, whereas
            // ordering a clamped profile could flip a cross-section whose
            // points collapsed onto the limit.
            if normalize {
                normalize_profile(&mut buf);
            }
            if profile_len == 0 {
                profile_len = buf.len();
            } else if buf.len() != profile_len {
                // A profile that changes point count mid-span cannot be
                // extruded as one strip.
                return;
            }
            if !clamp_profile(&mut buf, curvature_at(path, station)) {
                sections.push(None);
                continue;
            }

            let mut points = Vec::with_capacity(buf.len());
            let mut seats = Vec::with_capacity(buf.len());
            let mut v_coords = Vec::with_capacity(buf.len());
            let mut v_acc = 0.0f32;
            for (k, p) in buf.iter().enumerate() {
                let mut pos = offset_point(&sample, p.lat_m);
                let mut seat = Seat::default();
                pos.2 = match (p.anchor, self.ground) {
                    (Anchor::Ground, Some(field)) => {
                        let (z, s) = seat_on_ground(field, &sample, pos);
                        seat = s;
                        z + p.lift_m
                    }
                    (Anchor::Terrain, Some(field)) => {
                        let z = field.ground_height_at(pos.0, pos.1) + p.lift_m;
                        match field.deck_top_at(pos.0, pos.1) {
                            Some(top) => z.min(top - terrain::DECK_DEPTH_M),
                            None => z,
                        }
                    }
                    _ => height(&sample, p.lat_m, pos) + p.lift_m,
                };
                if k > 0 {
                    v_acc += distance(points[k - 1], pos);
                }
                points.push(pos);
                seats.push(seat);
                v_coords.push(v_acc);
            }
            let (sin_h, cos_h) = sample.heading_rad.sin_cos();
            sections.push(Some(CrossSection {
                points,
                seats,
                vs: v_coords,
                station,
                course: (cos_h, sin_h, 0.0),
            }));
        }

        // Emit each maximal run of drawable cross-sections, cut again wherever
        // it crosses a section boundary (the shared cross-section is repeated on
        // both sides, so the seam has no gap).
        let mut builder = Builder::default();
        let mut open_section: Option<i32> = None;
        let chunks = &mut self.chunks;
        let mut flush = |builder: &mut Builder, open: &mut Option<i32>| {
            if let Some(section) = open.take() {
                let chunk = std::mem::take(builder).finish(section, material_key);
                if !chunk.indices.is_empty() {
                    chunks.push(chunk);
                }
            }
        };

        for i in 0..steps {
            let (Some(a), Some(b)) = (&sections[i], &sections[i + 1]) else {
                // The strip is interrupted here; close whatever is open so the
                // two sides never get stitched across the gap.
                flush(&mut builder, &mut open_section);
                continue;
            };

            let section = section_index(a.station);
            if open_section != Some(section) {
                flush(&mut builder, &mut open_section);
                open_section = Some(section);
            }

            let forward_a = forward_at(&sections, i);
            let forward_b = forward_at(&sections, i + 1);
            for k in 0..profile_len - 1 {
                if underpass_hides([
                    (a.points[k], a.seats[k]),
                    (a.points[k + 1], a.seats[k + 1]),
                    (b.points[k + 1], b.seats[k + 1]),
                    (b.points[k], b.seats[k]),
                ]) {
                    continue;
                }
                let quad = [
                    (a.points[k], a.vs[k], a.station),
                    (a.points[k + 1], a.vs[k + 1], a.station),
                    (b.points[k + 1], b.vs[k + 1], b.station),
                    (b.points[k], b.vs[k], b.station),
                ];
                builder.emit_quad(quad, forward_a, forward_b, a.course);
            }
        }
        flush(&mut builder, &mut open_section);
    }

    /// A flat road-anchored quad between two laterals over a station span:
    /// the workhorse for paint.
    #[allow(clippy::too_many_arguments)]
    fn paint(
        &mut self,
        path: &CenterlinePath,
        start_m: f32,
        end_m: f32,
        step_m: f32,
        key: &str,
        lat_a: f32,
        lat_b: f32,
        lift_m: f32,
    ) {
        self.strip(
            path,
            start_m,
            end_m,
            step_m,
            key,
            move |_, _, out| {
                out.push(ProfilePoint::lifted(lat_a, lift_m));
                out.push(ProfilePoint::lifted(lat_b, lift_m));
            },
            |_, _, p| p.2,
        );
    }
}

/// Whether a strip facet is one an underpass cannot show: part of it hangs
/// into the slot from the level above, or it spans the wall line from the
/// slot floor to the embankment — a ramp through the wall.
fn underpass_hides(corners: [((f32, f32, f32), Seat); 4]) -> bool {
    if corners.iter().any(|(_, seat)| seat.hanging) {
        return true;
    }
    let inside = corners.iter().any(|(_, s)| s.slot == Some(true));
    let outside = corners.iter().any(|(_, s)| s.slot == Some(false));
    if !(inside && outside) {
        return false;
    }
    let (lo, hi) = corners
        .iter()
        .fold((f32::INFINITY, f32::NEG_INFINITY), |(lo, hi), (p, _)| {
            (lo.min(p.2), hi.max(p.2))
        });
    hi - lo > terrain::OVERHEAD_M
}

/// Keep lateral offsets short of the centre of curvature, and report
/// whether anything drawable is left.
///
/// A station-anchored element measures straight out from the centerline, so
/// on the inside of a corner its borders converge. Past a radius of `1/κ`
/// the offset curve folds through itself: consecutive cross-sections run
/// backwards relative to each other and the strip turns inside out. Austin
/// has apexes down to an 8 m radius on a 15 m wide track, so this is not a
/// corner case — the road's own inside edge folds there.
///
/// Clamping fixes the borders that reach too far. What it cannot fix is an
/// element that lies *entirely* beyond the limit — a curb on the inside of
/// such an apex would have to wrap around a sub-metre radius, and squeezing
/// it into the available room just yields a knot of degenerate facets.
/// Those cross-sections return `false` and the caller breaks the strip:
/// a curb interrupted for a few metres reads far better than one tied in a
/// knot.
fn clamp_profile(profile: &mut [ProfilePoint], kappa: f32) -> bool {
    /// Fraction of the turn radius a border may reach.
    const MARGIN: f32 = 0.85;
    /// Below this much lateral extent there is no facet worth emitting.
    const MIN_EXTENT_M: f32 = 0.05;
    /// Room a one-sided element needs between its inner border and the
    /// curvature limit to be worth drawing at all.
    const MIN_ROOM_M: f32 = 0.5;

    if kappa.abs() >= 1e-6 {
        let limit = MARGIN / kappa.abs();

        // An element straddling the centerline — the road, a marking across
        // it — always has room: only its far edge is clamped, and it keeps
        // the rest of its width. One lying wholly on the inside of the turn
        // is different, and needs actual room between its inner border and
        // the limit, not merely to start inside it. An element whose inner
        // border sits a hair inside gets compressed into a few centimetres
        // of facets sweeping around the centre of curvature: adjacent
        // cross-sections then barely advance while the profile writhes, and
        // the result is a knot, not a curb. Report those undrawable and let
        // the caller break the strip.
        let wholly_inside = if kappa > 0.0 {
            profile.iter().all(|p| p.lat_m > 0.0)
        } else {
            profile.iter().all(|p| p.lat_m < 0.0)
        };
        if wholly_inside {
            let innermost = profile
                .iter()
                .map(|p| p.lat_m.abs())
                .fold(f32::INFINITY, f32::min);
            if limit < innermost + MIN_ROOM_M {
                return false;
            }
        }

        for p in profile.iter_mut() {
            p.lat_m = if kappa > 0.0 {
                p.lat_m.min(limit)
            } else {
                p.lat_m.max(-limit)
            };
        }
    }
    let lo = profile
        .iter()
        .map(|p| p.lat_m)
        .fold(f32::INFINITY, f32::min);
    let hi = profile
        .iter()
        .map(|p| p.lat_m)
        .fold(f32::NEG_INFINITY, f32::max);
    hi - lo >= MIN_EXTENT_M
}

/// Enforce the "left to right" profile ordering the normals depend on.
fn normalize_profile(profile: &mut [ProfilePoint]) {
    let (Some(first), Some(last)) = (profile.first(), profile.last()) else {
        return;
    };
    if first.lat_m < last.lat_m {
        profile.reverse();
    }
}

/// Course direction at cross-section `i`, from the centers of the
/// neighbouring cross-sections. Taken from the emitted geometry rather than
/// the sample heading so it picks up elevation change as well as bearing.
///
/// Neighbours that were dropped as undrawable are skipped over; the
/// cross-section itself stands in when it has no usable neighbour on a side.
fn forward_at(sections: &[Option<CrossSection>], i: usize) -> (f32, f32, f32) {
    let mid = |s: &CrossSection| {
        let a = s.points[0];
        let b = s.points[s.points.len() - 1];
        ((a.0 + b.0) * 0.5, (a.1 + b.1) * 0.5, (a.2 + b.2) * 0.5)
    };
    let here = sections[i].as_ref().map(&mid);
    let before = (0..i).rev().find_map(|j| sections[j].as_ref().map(&mid));
    let after = (i + 1..sections.len()).find_map(|j| sections[j].as_ref().map(&mid));

    let (lo, hi) = match (before, after) {
        (Some(lo), Some(hi)) => (lo, hi),
        (Some(lo), None) => (lo, here.unwrap_or(lo)),
        (None, Some(hi)) => (here.unwrap_or(hi), hi),
        (None, None) => return (1.0, 0.0, 0.0),
    };
    let forward = normalize((hi.0 - lo.0, hi.1 - lo.1, hi.2 - lo.2));

    // Where a strip doubles back — a curb on the inside of an apex tighter
    // than the curb's own offset — successive cross-sections march
    // backwards and this difference points against the course. Its
    // magnitude is still the surface's true slope, so keep it and just fix
    // the sign; left alone, it flips the normal of every facet it touches
    // while the geometry around it stays put.
    let course = sections[i]
        .as_ref()
        .map(|s| s.course)
        .unwrap_or((1.0, 0.0, 0.0));
    if dot(forward, course) < 0.0 {
        (-forward.0, -forward.1, -forward.2)
    } else {
        forward
    }
}

#[derive(Default)]
struct Builder {
    positions: Vec<f32>,
    normals: Vec<f32>,
    uvs: Vec<f32>,
    indices: Vec<u32>,
}

impl Builder {
    /// One quad of a strip, given as
    /// `[(i,k), (i,k+1), (i+1,k+1), (i+1,k)]` in track space with each
    /// vertex's `v` coordinate and station.
    ///
    /// Vertices are not shared between quads. That costs some duplication,
    /// but it means each facet of a curb keeps its own normal without any
    /// smoothing-group machinery, while normals still vary smoothly *along*
    /// the strip because they are derived per cross-section.
    fn emit_quad(
        &mut self,
        quad: [((f32, f32, f32), f32, f32); 4],
        forward_a: (f32, f32, f32),
        forward_b: (f32, f32, f32),
        course: (f32, f32, f32),
    ) {
        // Reject a facet that has folded back on itself.
        //
        // Clamping keeps most offsets inside the turn radius, but it works
        // from an estimated curvature, and an estimate wide enough to ignore
        // the jitter of digitized nodes also flattens a genuine hairpin. So
        // the fold is caught here instead, after the fact and locally: if
        // either edge of this facet advances *against* the course, the strip
        // has turned back on itself and the facet is inside-out by
        // construction. Better to leave a hole than to ship one.
        if dot(sub(quad[3].0, quad[0].0), course) <= 0.0
            || dot(sub(quad[2].0, quad[1].0), course) <= 0.0
        {
            return;
        }

        // Lateral direction of this facet, per cross-section. Ordered left
        // to right, so `cross(lateral, forward)` points out of the surface.
        //
        // A facet can pinch to nothing where a clamped patch converges, and
        // a zero-length edge has no direction to take a normal from. Borrow
        // the other cross-section's rather than inventing one; if both have
        // pinched there is no facet left to draw.
        let raw_a = sub(quad[1].0, quad[0].0);
        let raw_b = sub(quad[2].0, quad[3].0);
        let (ok_a, ok_b) = (length(raw_a) > 1e-5, length(raw_b) > 1e-5);
        let (lat_a, lat_b) = match (ok_a, ok_b) {
            (true, true) => (normalize(raw_a), normalize(raw_b)),
            (true, false) => (normalize(raw_a), normalize(raw_a)),
            (false, true) => (normalize(raw_b), normalize(raw_b)),
            (false, false) => return,
        };

        // Slivers get dropped rather than shipped. A 130 m wide ground band
        // swept round a corner degenerates into facets a fraction of a
        // degree wide: they cover no pixels at any viewing angle, their
        // orientation is numerical noise, and they are exactly the input
        // Nanite and lightmap packing handle worst.
        let keep_first = facet_quality(quad[0].0, quad[1].0, quad[2].0) >= MIN_FACET_QUALITY;
        let keep_second = facet_quality(quad[0].0, quad[2].0, quad[3].0) >= MIN_FACET_QUALITY;
        if !keep_first && !keep_second {
            return;
        }

        let n_a = normalize(cross(lat_a, forward_a));
        let n_b = normalize(cross(lat_b, forward_b));

        // The two ends of this facet disagree about which way the surface
        // faces, so it is twisted through more than a right angle between
        // one cross-section and the next. No winding is correct for such a
        // quad — one of its triangles would be inside-out whichever way it
        // is wound — so there is nothing to emit. This is what is left of a
        // strip tangled around an apex tighter than its own offset, once
        // clamping and fold rejection have taken their share.
        if dot(n_a, n_b) <= 0.0 {
            return;
        }

        let base = (self.positions.len() / 3) as u32;

        for (vertex, normal) in [
            (quad[0], n_a),
            (quad[1], n_a),
            (quad[2], n_b),
            (quad[3], n_b),
        ] {
            let (pos, v, station) = vertex;
            push_position(&mut self.positions, pos);
            push_normal(&mut self.normals, normal);
            self.uvs.push(round(station, 3));
            self.uvs.push(round(v, 3));
        }

        // Wind each triangle to agree with the normal it ships with.
        //
        // The base ordering is right-handed CCW, deliberately *not*
        // reversed: track -> UE mirrors Y, and that mirror is itself the
        // handedness change. Unreal's front face is clockwise seen from the
        // normal — `MeshDescription` takes a triangle's normal as
        // `-cross(p1 - p0, p2 - p0)` — so negating Y turns RH CCW-front
        // into exactly that. Reversing as well would light every surface in
        // the level from underneath.
        //
        // Rather than trust that argument per triangle, each one is checked
        // against the normal derived from the surface frame and flipped if
        // it disagrees. For well-formed geometry this never fires. It
        // exists for the residue at hairpins whose radius is smaller than
        // the width of the element wrapped around them: clamping and fold
        // rejection remove the bulk, but what survives can still be
        // degenerate enough that its vertex order says one thing and its
        // frame another. There the frame wins — it is the one that knows
        // which side is up.
        let mut push = |a: usize, b: usize, c: usize| {
            let geometric = cross(sub(quad[b].0, quad[a].0), sub(quad[c].0, quad[a].0));
            let (a, b, c) = (base + a as u32, base + b as u32, base + c as u32);
            if dot(geometric, n_a) >= 0.0 {
                self.indices.extend_from_slice(&[a, b, c]);
            } else {
                self.indices.extend_from_slice(&[a, c, b]);
            }
        };
        if keep_first {
            push(0, 1, 2);
        }
        if keep_second {
            push(0, 2, 3);
        }
    }

    fn finish(self, section: i32, material_key: &str) -> Chunk {
        Chunk {
            section,
            material_key: material_key.to_string(),
            positions: self.positions,
            normals: self.normals,
            uvs: self.uvs,
            indices: self.indices,
        }
    }
}

/// Station runs along the lower road where the wall on one side
/// (`outward` +1 left, -1 right) stands at least [`WALL_MIN_HEIGHT_M`]
/// above the slot floor.
fn wall_runs(
    field: &TerrainHeightfield,
    lower: &CenterlinePath,
    u: &Underpass,
    outward: f32,
) -> Vec<(f32, f32)> {
    let gap = terrain::UNDERPASS_WALL_GAP_M;
    let (start, end) = u.wall_span_m;
    let mut runs: Vec<(f32, f32)> = Vec::new();
    let mut open: Option<f32> = None;
    let mut station = start;
    while station <= end {
        let s = lower.sample_at(station);
        let edge = if outward > 0.0 {
            s.width_left_m
        } else {
            -s.width_right_m
        };
        let floor = offset_point(&s, edge + outward * (gap - 0.1));
        let bank = offset_point(&s, edge + outward * (gap + 0.1));
        let height =
            field.ground_height_at(bank.0, bank.1) - field.ground_height_at(floor.0, floor.1);
        match (height >= WALL_MIN_HEIGHT_M, open) {
            (true, None) => open = Some(station),
            (false, Some(from)) => {
                runs.push((from, station));
                open = None;
            }
            _ => {}
        }
        station += STEP_M;
    }
    if let Some(from) = open {
        runs.push((from, end));
    }
    runs
}

/// Merge chunks that share a section and a material into one mesh each.
fn merge_chunks(mut chunks: Vec<Chunk>) -> Vec<UeMesh> {
    // Stable sort on a total key: the merge order — and so the vertex order
    // inside every merged mesh — must not depend on how the scene happened
    // to be laid out in memory.
    chunks.sort_by(|a, b| {
        a.material_key
            .cmp(&b.material_key)
            .then(a.section.cmp(&b.section))
    });

    let mut meshes: Vec<UeMesh> = Vec::new();
    let mut open: Option<(String, i32)> = None;
    for chunk in chunks {
        if chunk.indices.is_empty() {
            continue;
        }
        let bucket = (chunk.material_key.clone(), chunk.section);
        if open.as_ref() != Some(&bucket) {
            meshes.push(UeMesh {
                name: mesh_name(&chunk.material_key, chunk.section),
                material_key: chunk.material_key.clone(),
                positions: Vec::new(),
                normals: Vec::new(),
                uvs: Vec::new(),
                indices: Vec::new(),
            });
            open = Some(bucket);
        }
        let mesh = meshes.last_mut().unwrap();
        let offset = (mesh.positions.len() / 3) as u32;
        mesh.positions.extend_from_slice(&chunk.positions);
        mesh.normals.extend_from_slice(&chunk.normals);
        mesh.uvs.extend_from_slice(&chunk.uvs);
        mesh.indices
            .extend(chunk.indices.iter().map(|i| i + offset));
    }
    meshes
}

fn mesh_name(material_key: &str, section: i32) -> String {
    format!("{material_key}_{section:03}")
}

fn section_index(station_m: f32) -> i32 {
    (station_m / SECTION_LEN_M).floor() as i32
}

/// Resolve a station span the way the viewport does: wrapping through
/// start/finish on a closed loop, clamped on an open path.
fn resolve_span(path: &CenterlinePath, start_m: f32, end_m: f32) -> Option<(f32, f32)> {
    let total = path.total_length_m();
    if !(total.is_finite() && total > 0.0) {
        return None;
    }
    if path.is_closed() {
        let start = start_m.rem_euclid(total);
        let mut end = end_m.rem_euclid(total);
        if end <= start {
            end += total;
        }
        Some((start, end))
    } else {
        let start = start_m.clamp(0.0, total);
        let end = end_m.clamp(0.0, total);
        (end > start).then_some((start, end))
    }
}

// ---------------------------------------------------------------------------
// Per-element profiles
// ---------------------------------------------------------------------------

impl Bake<'_> {
    fn road(&mut self, path: &CenterlinePath) {
        let key = "road";
        self.register(key, "road", [0.24, 0.24, 0.26, 1.0]);
        self.strip(
            path,
            0.0,
            path.total_length_m(),
            STEP_M,
            key,
            |sample, _, out| {
                out.push(ProfilePoint::lifted(sample.width_left_m, 0.0));
                out.push(ProfilePoint::lifted(-sample.width_right_m, 0.0));
            },
            |_, _, p| p.2,
        );
    }

    /// White lines along both road edges, the full length of the circuit,
    /// broken where the pit lane joins the road so the lane reads as
    /// connected rather than painted over.
    ///
    /// These are their own strips rather than paint in the road material:
    /// the road's `v` coordinate counts meters from the *left* edge, so a
    /// material has no way to know where the right edge is on a track of
    /// varying width.
    fn edge_lines(&mut self, path: &CenterlinePath, gaps: &[(Side, f32, f32)]) {
        let key = format!("marking_edge_line_{}", color_hex(LINE_COLOR));
        self.register(&key, "marking", LINE_COLOR);
        let total = path.total_length_m();
        for side in [Side::Left, Side::Right] {
            let mut side_gaps: Vec<(f32, f32)> = gaps
                .iter()
                .filter(|(s, _, _)| *s == side)
                .map(|(_, a, b)| (*a, *b))
                .collect();
            side_gaps.sort_by(|a, b| a.0.total_cmp(&b.0));

            // The complement of the gaps over one lap. On a closed loop the
            // last piece runs up to `total`, which the span resolver folds
            // back onto station 0 — so the lap closes without a seam gap.
            let mut pieces: Vec<(f32, f32)> = Vec::new();
            let mut cursor = 0.0f32;
            for (a, b) in side_gaps {
                if a > cursor + 0.01 {
                    pieces.push((cursor, a));
                }
                cursor = cursor.max(b);
            }
            if cursor < total - 0.01 {
                pieces.push((cursor, total));
            }

            for (start, end) in pieces {
                self.strip(
                    path,
                    start,
                    end,
                    STEP_M,
                    &key,
                    move |sample, _, out| {
                        let (edge, outward) = match side {
                            Side::Left => (sample.width_left_m, 1.0),
                            Side::Right => (-sample.width_right_m, -1.0),
                        };
                        out.push(ProfilePoint::lifted(edge, MARKING_LIFT_M));
                        out.push(ProfilePoint::lifted(
                            edge - outward * EDGE_LINE_WIDTH_M,
                            MARKING_LIFT_M,
                        ));
                    },
                    |_, _, p| p.2,
                );
            }
        }
    }

    fn pit_lane(&mut self, lane: &CenterlinePath, width_m: f32) {
        let key = "pit_lane";
        self.register(key, "pit_lane", [0.32, 0.32, 0.34, 1.0]);
        let half = width_m / 2.0;
        self.strip(
            lane,
            0.0,
            lane.total_length_m(),
            STEP_M,
            key,
            move |_, _, out| {
                out.push(ProfilePoint::lifted(half, PIT_LIFT_M));
                out.push(ProfilePoint::lifted(-half, PIT_LIFT_M));
            },
            |_, _, p| p.2,
        );
    }

    /// Pit-lane paint: a solid line along the pit-box side, a dashed line
    /// along the road side where the lane runs parallel to the road, and
    /// solid lines on both edges of the entry and exit tapers.
    fn pit_markings(
        &mut self,
        lane: &CenterlinePath,
        width_m: f32,
        box_count: u32,
        relation: &LaneRelation,
    ) {
        let key = format!("marking_pit_line_{}", color_hex(LINE_COLOR));
        self.register(&key, "marking", LINE_COLOR);
        let half = width_m / 2.0;
        // The lane runs the track's way, so with the lane on the track's
        // left the road is to the lane's right.
        let side = relation.lane_side as f32;
        let box_edge = side * half;
        let road_edge = -side * half;
        let inward = |edge: f32| edge - edge.signum() * LINE_WIDTH_M;

        // Pit-box side: solid, the whole lane.
        self.paint(
            lane,
            0.0,
            lane.total_length_m(),
            STEP_M,
            &key,
            box_edge,
            inward(box_edge),
            PIT_LINE_LIFT_M,
        );

        // Road side: dashed where parallel, solid through the tapers.
        let spans = if relation.spans.is_empty() {
            vec![(0.0, lane.total_length_m(), true)]
        } else {
            relation.spans.clone()
        };
        for (start, end, parallel) in spans {
            if !parallel {
                self.paint(
                    lane,
                    start,
                    end,
                    STEP_M,
                    &key,
                    road_edge,
                    inward(road_edge),
                    PIT_LINE_LIFT_M,
                );
                continue;
            }
            let mut at = start;
            while at < end - 0.3 {
                let dash_end = (at + PIT_DASH_M).min(end);
                self.paint(
                    lane,
                    at,
                    dash_end,
                    STEP_M,
                    &key,
                    road_edge,
                    inward(road_edge),
                    PIT_LINE_LIFT_M,
                );
                at += PIT_DASH_M + PIT_GAP_M;
            }
        }

        // The speed limit: a line across the lane where it starts and where
        // it ends — the ends of the longest parallel stretch, which is where
        // the bake stands the garages and where a real limit line is.
        let total = lane.total_length_m();
        let (limit_start, limit_end) = relation
            .spans
            .iter()
            .filter(|(_, _, parallel)| *parallel)
            .map(|(s, e, _)| (*s, *e))
            .max_by(|a, b| (a.1 - a.0).total_cmp(&(b.1 - b.0)))
            .unwrap_or((0.0, total));
        for at in [limit_start, limit_end] {
            if at <= PIT_LIMIT_LINE_M || at >= total - PIT_LIMIT_LINE_M {
                continue;
            }
            self.paint(
                lane,
                at - PIT_LIMIT_LINE_M / 2.0,
                at + PIT_LIMIT_LINE_M / 2.0,
                STEP_M,
                &key,
                half,
                -half,
                PIT_LINE_LIFT_M,
            );
        }

        // The pit boxes: a working-lane outline in front of each garage,
        // the same pitch and row the garages stand on (`bake_pit_complex`).
        let span = limit_end - limit_start;
        let boxes = (box_count as f32).min((span / PIT_MODULE_M).floor()) as u32;
        if boxes > 0 {
            let row = boxes as f32 * PIT_MODULE_M;
            let row_start = (limit_start + limit_end) / 2.0 - row / 2.0;
            let outer = box_edge;
            let inner = box_edge - side * PIT_BOX_DEPTH_M;
            for i in 0..boxes {
                let s0 = row_start + i as f32 * PIT_MODULE_M;
                let s1 = s0 + PIT_MODULE_M;
                // The two sides of the box, across the lane.
                for at in [s0, s1] {
                    self.paint(
                        lane,
                        at - LINE_WIDTH_M / 2.0,
                        at + LINE_WIDTH_M / 2.0,
                        STEP_M,
                        &key,
                        outer,
                        inner,
                        PIT_LINE_LIFT_M,
                    );
                }
                // Its lane-side edge.
                self.paint(
                    lane,
                    s0,
                    s1,
                    STEP_M,
                    &key,
                    inner + side * LINE_WIDTH_M,
                    inner,
                    PIT_LINE_LIFT_M,
                );
            }
        }
    }

    /// The pit exit blend line: from where the exit taper leaves the road
    /// edge, a solid line along the road on the lane's side for
    /// [`PIT_EXIT_LINE_M`], a car's width in, which a car leaving the pits
    /// stays inside of until it ends.
    fn pit_exit_line(&mut self, path: &CenterlinePath, relation: &LaneRelation) {
        let Some((side, _, end)) = relation
            .edge_gaps
            .iter()
            .max_by(|a, b| a.2.total_cmp(&b.2))
            .copied()
        else {
            return;
        };
        let key = format!("marking_pit_exit_{}", color_hex(LINE_COLOR));
        self.register(&key, "marking", LINE_COLOR);
        let total = path.total_length_m();
        let from = end;
        let to = if path.is_closed() {
            end + PIT_EXIT_LINE_M
        } else {
            (end + PIT_EXIT_LINE_M).min(total)
        };
        self.strip(
            path,
            from,
            to,
            STEP_M,
            &key,
            move |sample, _, out| {
                let (edge, outward) = match side {
                    Side::Left => (sample.width_left_m, 1.0),
                    Side::Right => (-sample.width_right_m, -1.0),
                };
                let at = edge - outward * PIT_EXIT_LINE_IN_M;
                out.push(ProfilePoint::lifted(at, MARKING_LIFT_M));
                out.push(ProfilePoint::lifted(
                    at - outward * EDGE_LINE_WIDTH_M,
                    MARKING_LIFT_M,
                ));
            },
            |_, _, p| p.2,
        );
    }

    /// A curb: a ramp from the track edge up to a lip, then a vertical face
    /// dropping to the verge on the outside, and a painted flat strip on
    /// the ground beyond it. The editor previews this as a flat painted
    /// strip; in a lit level it needs the actual profile.
    ///
    /// The profile stands on the road *edge* height, not on the banking
    /// plane extended past the edge: the verge is flat from the edge too,
    /// so the outer face is the same [`CURB_HEIGHT_M`] + lift +
    /// [`terrain::VERGE_DROP_M`] everywhere — on the high side of a banked
    /// corner the extended plane would turn it into a 40 cm wall.
    fn curb(&mut self, path: &CenterlinePath, curb: &Curb) {
        let key = format!("curb_{}", sanitize(&curb.style));
        let (base, alternate) = curb_style_colors(&curb.style);
        self.register(&key, "curb", base);

        let side = curb.side;
        let width = curb.width_m.max(0.01);
        let edge_of = move |sample: &PathSample| match side {
            Side::Left => (sample.width_left_m, 1.0f32),
            Side::Right => (-sample.width_right_m, -1.0f32),
        };
        self.strip(
            path,
            curb.start_m,
            curb.end_m,
            STEP_M,
            &key,
            move |sample, _, out| {
                // Signed so the same arithmetic works on both sides:
                // `outward` grows away from the centerline.
                let (edge, outward) = edge_of(sample);
                let at = |d: f32, lift: f32| {
                    ProfilePoint::lifted(edge + outward * d, lift + CURB_LIFT_M)
                };
                out.push(at(0.0, 0.0));
                out.push(at(width * CURB_LIP_FRAC, CURB_HEIGHT_M * 0.6));
                out.push(at(width, CURB_HEIGHT_M));
                // Outer face, straight down to the verge that is actually
                // there — not to some ground metres below.
                out.push(ProfilePoint::grounded(edge + outward * width, 0.0));
            },
            move |sample, _, _| offset_point(sample, edge_of(sample).0).2,
        );

        // The painted strip beyond the curb, in the curb's alternate color,
        // flat on the ground.
        let strip_key = format!("marking_curb_strip_{}", color_hex(alternate));
        self.register(&strip_key, "marking", alternate);
        self.strip(
            path,
            curb.start_m,
            curb.end_m,
            STEP_M,
            &strip_key,
            move |sample, _, out| {
                let (edge, outward) = edge_of(sample);
                out.push(ProfilePoint::grounded(
                    edge + outward * width,
                    CURB_STRIP_LIFT_M,
                ));
                out.push(ProfilePoint::grounded(
                    edge + outward * (width + CURB_STRIP_M),
                    CURB_STRIP_LIFT_M,
                ));
            },
            |_, _, p| p.2,
        );
    }

    fn surface(&mut self, path: &CenterlinePath, surface: &Surface) {
        let key = format!("surface_{}", surface.kind.label());
        self.register(&key, "surface", surface_kind_color(surface.kind));

        let side = surface.side;
        let inner = surface.inner_m;
        let lift = surface_lift(surface.kind);
        let spec = surface.clone();
        // The ground profile bends across the band, so the profile needs
        // columns across it — two border points would just span a plane
        // over whatever lies between them. The count must be constant along
        // the strip (`strip` requires it), so it comes from the band's
        // widest cross-section.
        let fractions = surface_lateral_fractions(surface);
        let reach = self
            .ground
            .and_then(|field| band_reach_profile(field, path, surface));
        let steps = reach.as_ref().map_or(1, |r| r.len().max(2) - 1);
        let reach_for_paint = reach.clone();
        let spec_for_paint = spec.clone();
        let width_at = std::rc::Rc::new(move |progress: f32| -> f32 {
            let mut width = spec_for_paint.width_at(progress);
            if let Some(reach) = &reach_for_paint {
                let i = ((progress * steps as f32).round() as usize).min(steps);
                width = width.min(reach[i]).max(0.01);
            }
            width
        });
        self.strip(
            path,
            surface.start_m,
            surface.end_m,
            SURFACE_STEP_M,
            &key,
            move |sample, progress, out| {
                let (edge, outward) = match side {
                    Side::Left => (sample.width_left_m, 1.0),
                    Side::Right => (-sample.width_right_m, -1.0),
                };
                let mut width = spec.width_at(progress);
                if let Some(reach) = &reach {
                    let i = ((progress * steps as f32).round() as usize).min(steps);
                    width = width.min(reach[i]).max(0.01);
                }
                for f in &fractions {
                    let lat_m = edge + outward * (inner + width * f);
                    out.push(ProfilePoint::grounded(lat_m, lift));
                }
            },
            |_, _, p| p.2,
        );

        // Painted run-off: stripes parallel to the road across the band,
        // each its own marking strip a hair above the tarmac, like the
        // painted strip beside a curb.
        let painted = matches!(
            surface.kind,
            crate::ats::SurfaceKind::AsphaltRunoff | crate::ats::SurfaceKind::Concrete
        );
        if let (true, Some(style)) = (painted, surface.paint.as_deref()) {
            let Some(colours) = runoff_paint_colours(style) else {
                return;
            };
            let widest = surface
                .width_m
                .max(surface.end_width_m.unwrap_or(surface.width_m));
            let stripes = (widest / RUNOFF_STRIPE_M).ceil().max(1.0) as usize;
            for k in 0..stripes {
                let colour = colours[k % 2];
                let stripe_key = format!("marking_runoff_{}", color_hex(colour));
                self.register(&stripe_key, "marking", colour);
                let width_at = std::rc::Rc::clone(&width_at);
                self.strip(
                    path,
                    surface.start_m,
                    surface.end_m,
                    SURFACE_STEP_M,
                    &stripe_key,
                    move |sample, progress, out| {
                        let (edge, outward) = match side {
                            Side::Left => (sample.width_left_m, 1.0),
                            Side::Right => (-sample.width_right_m, -1.0),
                        };
                        let width = width_at(progress);
                        let from = (k as f32 * RUNOFF_STRIPE_M).min(width);
                        let to = ((k + 1) as f32 * RUNOFF_STRIPE_M).min(width);
                        out.push(ProfilePoint::grounded(
                            edge + outward * (inner + from),
                            lift + RUNOFF_PAINT_LIFT_M,
                        ));
                        out.push(ProfilePoint::grounded(
                            edge + outward * (inner + to),
                            lift + RUNOFF_PAINT_LIFT_M,
                        ));
                    },
                    |_, _, p| p.2,
                );
            }
        }
    }

    /// The world ground: the terrain heightfield as meshes, tiled so Unreal
    /// can cull them, seated on [`TerrainHeightfield::ground_height_at`].
    /// Cells within reach of a road are subdivided so the verge profile is
    /// actually followed there, and again less finely out to a couple of
    /// hundred metres; further out the coarse field is exact.
    fn ground(&mut self, field: &TerrainHeightfield) {
        /// Coarse cells per tile side.
        const TILE_CELLS: usize = 32;
        /// Fine cells per coarse cell side: 12 m -> 2 m.
        const SUB: usize = 6;
        /// Fine cells per quad beside the road (2 m) and in the ring past
        /// it (6 m). Both must divide `SUB`, or a cell would not fill.
        const NEAR_STEP: usize = 1;
        const MID_STEP: usize = 3;
        /// A coarse cell is drawn at `NEAR_STEP` when any corner is this
        /// close to a road centerline, at `MID_STEP` out to `MID_RADIUS_M`,
        /// and whole beyond. Past the near radius the ground is the coarse
        /// field itself (the verge blend ends at `BLEND_END_M` + half
        /// width), so a subdivided cell's boundary vertices lie on the
        /// coarse edge and resolutions meet without cracks. The near radius
        /// is what has to clear the blend; the middle ring exists because
        /// 12 m quads a hundred metres out are visibly faceted against the
        /// horizon, while 2 m quads out there would be most of the
        /// circuit's triangles for nothing.
        const FINE_RADIUS_M: f32 = 60.0;
        const MID_RADIUS_M: f32 = 200.0;

        let key = "ground";
        self.register(key, "surface", terrain::GROUND_COLOR);

        let (cols, rows) = (field.cols(), field.rows());
        if cols < 2 || rows < 2 {
            return;
        }
        let coarse_m = field.cell_m();
        let fine_m = coarse_m / SUB as f32;
        let (origin_x, origin_y, _) = field.vertex(0, 0);

        // Band per coarse vertex: 0 beside the road, 1 in the middle ring,
        // 2 out in the coarse field.
        let band: Vec<u8> = (0..rows * cols)
            .map(|i| {
                let (x, y, _) = field.vertex(i % cols, i / cols);
                if field.road_distance_at(x, y, FINE_RADIUS_M).is_some() {
                    0
                } else if field.road_distance_at(x, y, MID_RADIUS_M).is_some() {
                    1
                } else {
                    2
                }
            })
            .collect();
        // A cell is drawn at its nearest corner's band, so the finer side
        // always wins wherever two bands meet.
        let cell_step = |c: usize, r: usize| {
            let nearest = band[r * cols + c]
                .min(band[r * cols + c + 1])
                .min(band[(r + 1) * cols + c])
                .min(band[(r + 1) * cols + c + 1]);
            match nearest {
                0 => NEAR_STEP,
                1 => MID_STEP,
                _ => SUB,
            }
        };

        let mut tile = 0i32;
        let mut r0 = 0;
        while r0 + 1 < rows {
            let r1 = (r0 + TILE_CELLS).min(rows - 1);
            let mut c0 = 0;
            while c0 + 1 < cols {
                let c1 = (c0 + TILE_CELLS).min(cols - 1);

                let mut chunk = Chunk {
                    section: tile,
                    material_key: key.to_string(),
                    positions: Vec::new(),
                    normals: Vec::new(),
                    uvs: Vec::new(),
                    indices: Vec::new(),
                };
                // Vertices keyed by fine grid coordinates, shared between
                // every facet of the tile that touches them.
                let mut vertices: BTreeMap<(usize, usize), u32> = BTreeMap::new();
                let mut vertex = |fc: usize, fr: usize, chunk: &mut Chunk| -> u32 {
                    *vertices.entry((fc, fr)).or_insert_with(|| {
                        let x = origin_x + fc as f32 * fine_m;
                        let y = origin_y + fr as f32 * fine_m;
                        let z = field.ground_height_at(x, y);
                        push_position(&mut chunk.positions, (x, y, z));
                        push_normal(
                            &mut chunk.normals,
                            field.ground_normal_at(x, y, fine_m * 0.5),
                        );
                        chunk.uvs.push(round(x, 3));
                        chunk.uvs.push(round(y, 3));
                        (chunk.positions.len() / 3 - 1) as u32
                    })
                };
                // Near an underpass wall the ground steps straight up, and
                // a grid facet across the step would be a slope reaching
                // metres into the slot — over the road itself. Those facets
                // are left out; `underpass` draws that ground along the road.
                let cut = |fc: usize, fr: usize| {
                    let x = origin_x + fc as f32 * fine_m;
                    let y = origin_y + fr as f32 * fine_m;
                    field
                        .wall_relation(x, y)
                        .is_some_and(|(past_wall, _)| past_wall.abs() < UNDERPASS_CUT_M)
                };
                let mut quad = |fc: usize, fr: usize, step: usize, chunk: &mut Chunk| {
                    if [(0, 0), (step, 0), (0, step), (step, step)]
                        .iter()
                        .any(|&(dc, dr)| cut(fc + dc, fr + dr))
                    {
                        return;
                    }
                    let v00 = vertex(fc, fr, chunk);
                    let v10 = vertex(fc + step, fr, chunk);
                    let v01 = vertex(fc, fr + step, chunk);
                    let v11 = vertex(fc + step, fr + step, chunk);
                    // Right-handed CCW facing up in track space; the
                    // track -> UE mirror turns that into Unreal's clockwise
                    // front face, same as every strip (see `emit_quad`).
                    chunk.indices.extend_from_slice(&[v00, v10, v01]);
                    chunk.indices.extend_from_slice(&[v10, v11, v01]);
                };

                for r in r0..r1 {
                    for c in c0..c1 {
                        let step = cell_step(c, r);
                        let per_side = SUB / step;
                        for l in 0..per_side {
                            for k in 0..per_side {
                                quad(c * SUB + k * step, r * SUB + l * step, step, &mut chunk);
                            }
                        }
                    }
                }
                self.chunks.push(chunk);
                tile += 1;
                c0 = c1;
            }
            r0 = r1;
        }
    }

    /// The skyline: the land from where the ground mesh stops out to the
    /// far hills, drawn from the elevation model's coarse grid.
    ///
    /// The ground mesh reaches 800 m past the circuit and then simply
    /// ends, which is why every track faded into height fog and then into
    /// a bare atmosphere gradient. That reads as a green pancake however
    /// good the road looks. This is the rest of the view: 90 m posts out
    /// to eight kilometres, which at the Red Bull Ring is the wooded
    /// slopes of the Murtal rising four hundred metres within a kilometre
    /// and the Seetaler Alpen behind them.
    ///
    /// It is geometry rather than a painted backdrop on purpose. The sun
    /// lights it, it takes the weather's fog, it goes dark at dusk with
    /// everything else, and the TV director can point a long lens at it.
    /// The cost is one material and about thirty thousand triangles for a
    /// 16 km square, which is less than the circuit's own kerbs.
    ///
    /// The near field is cut out of it: where the detailed ground mesh
    /// already covers the land, the horizon has a hole so the two do not
    /// fight for the same depth. The seam sits in the fog by construction,
    /// because it is where the old mesh used to end.
    fn horizon(&mut self, field: &TerrainHeightfield, dem: &DemFile) {
        /// Coarse cells per tile side: the far field is culled in big
        /// pieces because it is all visible at once or not at all.
        const TILE_CELLS: usize = 24;
        /// How far past the ground mesh's own edge the hole reaches. One
        /// coarse cell of overlap, so the two meshes abut under the fog
        /// rather than leaving a gap of sky at the join.
        const OVERLAP_M: f32 = 90.0;

        let key = HORIZON_KEY;
        self.register(key, "surface", terrain::GROUND_COLOR);

        let grid = &dem.outer;
        if grid.cols < 2 || grid.rows < 2 {
            return;
        }
        // The hole: the ground mesh's own extent, less an overlap.
        let (near_min_x, near_min_y, _) = field.vertex(0, 0);
        let (near_max_x, near_max_y, _) = field.vertex(field.cols() - 1, field.rows() - 1);
        let hole = (
            near_min_x + OVERLAP_M,
            near_min_y + OVERLAP_M,
            near_max_x - OVERLAP_M,
            near_max_y - OVERLAP_M,
        );
        let inside_hole = |x: f32, y: f32| x > hole.0 && x < hole.2 && y > hole.1 && y < hole.3;

        let cell = grid.cell_m;
        let mut tile = 0i32;
        let mut r0 = 0usize;
        while r0 + 1 < grid.rows {
            let r1 = (r0 + TILE_CELLS).min(grid.rows - 1);
            let mut c0 = 0usize;
            while c0 + 1 < grid.cols {
                let c1 = (c0 + TILE_CELLS).min(grid.cols - 1);
                let mut chunk = Chunk {
                    section: tile,
                    material_key: key.to_string(),
                    positions: Vec::new(),
                    normals: Vec::new(),
                    uvs: Vec::new(),
                    indices: Vec::new(),
                };
                let mut vertices: BTreeMap<(usize, usize), u32> = BTreeMap::new();
                let mut vertex = |c: usize, r: usize, chunk: &mut Chunk| -> u32 {
                    *vertices.entry((c, r)).or_insert_with(|| {
                        let x = grid.origin_x + c as f32 * cell;
                        let y = grid.origin_y + r as f32 * cell;
                        let z = grid.height_at(x, y);
                        push_position(&mut chunk.positions, (x, y, z));
                        // A normal from the neighbouring posts: at this
                        // scale the slope is the whole of the shading.
                        let dzdx = (grid.height_at(x + cell, y) - grid.height_at(x - cell, y))
                            / (2.0 * cell);
                        let dzdy = (grid.height_at(x, y + cell) - grid.height_at(x, y - cell))
                            / (2.0 * cell);
                        let len = (dzdx * dzdx + dzdy * dzdy + 1.0).sqrt();
                        push_normal(&mut chunk.normals, (-dzdx / len, -dzdy / len, 1.0 / len));
                        chunk.uvs.push(round(x, 3));
                        chunk.uvs.push(round(y, 3));
                        (chunk.positions.len() / 3 - 1) as u32
                    })
                };

                for r in r0..r1 {
                    for c in c0..c1 {
                        let x0 = grid.origin_x + c as f32 * cell;
                        let y0 = grid.origin_y + r as f32 * cell;
                        // A quad wholly inside the hole is the detailed
                        // ground's job; one straddling the edge is drawn,
                        // so the two overlap rather than leave a gap.
                        if inside_hole(x0, y0)
                            && inside_hole(x0 + cell, y0)
                            && inside_hole(x0, y0 + cell)
                            && inside_hole(x0 + cell, y0 + cell)
                        {
                            continue;
                        }
                        let v00 = vertex(c, r, &mut chunk);
                        let v10 = vertex(c + 1, r, &mut chunk);
                        let v01 = vertex(c, r + 1, &mut chunk);
                        let v11 = vertex(c + 1, r + 1, &mut chunk);
                        chunk.indices.extend_from_slice(&[v00, v10, v01]);
                        chunk.indices.extend_from_slice(&[v10, v11, v01]);
                    }
                }
                if !chunk.indices.is_empty() {
                    self.chunks.push(chunk);
                    tile += 1;
                }
                c0 = c1;
            }
            r0 = r1;
        }
    }

    /// A road passing under another: the ground along the slot that the
    /// grid leaves out, the abutment walls, and the deck carrying the upper
    /// road, with its parapets and fascia.
    fn underpass(&mut self, field: &TerrainHeightfield, u: &Underpass) {
        let lower = field.road_path(u.lower_road).clone();
        let upper = field.road_path(u.upper_road).clone();
        let gap = terrain::UNDERPASS_WALL_GAP_M;
        self.register("ground", "surface", terrain::GROUND_COLOR);
        self.register(STRUCTURE_KEY, "structure", STRUCTURE_COLOR);
        self.register(FASCIA_KEY, "structure", FASCIA_COLOR);

        // The ground either side of the wall line, over everything the grid
        // cut (a grid cell reaches a diagonal past the cut) and a margin
        // past the ends of the walls.
        let (start, end) = u.wall_span_m;
        let reach = UNDERPASS_CUT_M + GRID_DIAGONAL_M;
        for outward in [1.0f32, -1.0] {
            let edge = move |s: &PathSample| {
                if outward > 0.0 {
                    s.width_left_m
                } else {
                    -s.width_right_m
                }
            };
            let floor_from = gap - reach;
            let floor_cols = ((gap - floor_from) / 1.0).ceil() as usize;
            self.strip(
                &lower,
                start - GRID_DIAGONAL_M,
                end + GRID_DIAGONAL_M,
                2.0,
                "ground",
                move |s, _, out| {
                    for c in 0..=floor_cols {
                        let b =
                            floor_from + (gap - 0.1 - floor_from) * (c as f32 / floor_cols as f32);
                        out.push(ProfilePoint::terrain(edge(s) + outward * b, COVER_LIFT_M));
                    }
                },
                |_, _, p| p.2,
            );
            let ridge_cols = (reach / 1.5).ceil() as usize;
            self.strip(
                &lower,
                start - GRID_DIAGONAL_M,
                end + GRID_DIAGONAL_M,
                2.0,
                "ground",
                move |s, _, out| {
                    for c in 0..=ridge_cols {
                        let b = gap + 0.1 + reach * (c as f32 / ridge_cols as f32);
                        out.push(ProfilePoint::terrain(edge(s) + outward * b, COVER_LIFT_M));
                    }
                },
                |_, _, p| p.2,
            );

            // The wall itself, where there is a wall's worth of height:
            // its face, a coping, and the back of the coping down onto the
            // embankment.
            for (from, to) in wall_runs(field, &lower, u, outward) {
                self.shape(&lower, from, to, STEP_M, STRUCTURE_KEY, move |s, _, out| {
                    let face = edge(s) + outward * gap;
                    let back = face + outward * WALL_COPING_M;
                    let inner = face - outward * 0.05;
                    let outer = face + outward * 0.05;
                    let points = [
                        ProfilePoint::terrain(back, 0.0),
                        ProfilePoint::terrain(back, WALL_PARAPET_M),
                        ProfilePoint::terrain(outer, WALL_PARAPET_M),
                        ProfilePoint::terrain(inner, 0.0),
                    ];
                    // Left of the road the path runs outside-in; right of
                    // it, mirrored, so every face still points out of the
                    // wall.
                    if outward > 0.0 {
                        out.extend(points);
                    } else {
                        out.extend(points.into_iter().rev());
                    }
                });
            }
        }

        // The deck: the upper road's own ribbon is its top, so this is the
        // parapets, the underside and the fascia down each side.
        let (start, end) = u.deck_span_m;
        let top = -0.1;
        let depth = terrain::DECK_DEPTH_M;
        let over = terrain::DECK_OVERHANG_M;
        self.shape(
            &upper,
            start,
            end,
            STEP_M,
            STRUCTURE_KEY,
            move |s, _, out| {
                let left = s.width_left_m + over;
                let right = -(s.width_right_m + over);
                out.extend([
                    ProfilePoint::lifted(left, PARAPET_HEIGHT_M),
                    ProfilePoint::lifted(left - PARAPET_WIDTH_M, PARAPET_HEIGHT_M),
                    ProfilePoint::lifted(left - PARAPET_WIDTH_M, top),
                    ProfilePoint::lifted(right + PARAPET_WIDTH_M, top),
                    ProfilePoint::lifted(right + PARAPET_WIDTH_M, PARAPET_HEIGHT_M),
                    ProfilePoint::lifted(right, PARAPET_HEIGHT_M),
                ]);
            },
        );
        self.shape(
            &upper,
            start,
            end,
            STEP_M,
            STRUCTURE_KEY,
            move |s, _, out| {
                out.extend([
                    ProfilePoint::lifted(-(s.width_right_m + over + FASCIA_LEAN_M), -depth),
                    ProfilePoint::lifted(s.width_left_m + over + FASCIA_LEAN_M, -depth),
                ]);
            },
        );
        self.shape(&upper, start, end, STEP_M, FASCIA_KEY, move |s, _, out| {
            let left = s.width_left_m + over;
            out.extend([
                ProfilePoint::lifted(left + FASCIA_LEAN_M, -depth),
                ProfilePoint::lifted(left, PARAPET_HEIGHT_M),
            ]);
        });
        self.shape(&upper, start, end, STEP_M, FASCIA_KEY, move |s, _, out| {
            let right = -(s.width_right_m + over);
            out.extend([
                ProfilePoint::lifted(right, PARAPET_HEIGHT_M),
                ProfilePoint::lifted(right - FASCIA_LEAN_M, -depth),
            ]);
        });
    }

    fn marking(&mut self, path: &CenterlinePath, marking: &Marking) {
        let (lo, hi) = if marking.lat_from_m <= marking.lat_to_m {
            (marking.lat_from_m, marking.lat_to_m)
        } else {
            (marking.lat_to_m, marking.lat_from_m)
        };

        if marking.kind == MarkingKind::StartFinish {
            self.chequer(path, marking, lo, hi);
            return;
        }

        // Markings of the same kind can be painted different colors, so the
        // key carries the color too — otherwise a yellow pit-exit line and
        // a white one would collapse onto one material.
        let key = format!(
            "marking_{}_{}",
            marking.kind.label(),
            color_hex(marking.color)
        );
        self.register(&key, "marking", marking.color);
        self.paint(
            path,
            marking.start_m,
            marking.end_m,
            STEP_M,
            &key,
            hi,
            lo,
            MARKING_LIFT_M,
        );
    }

    /// A picture on the road ([`Decal`]): a road-hugging grid under the
    /// decal's own material key (family `decal`), with UVs normalised over
    /// the picture — `u` 0 at its left edge to 1 at its right, `v` 0 at
    /// the far end to 1 at the near one — so the texture's top reads
    /// furthest down the road. A reversed decal is turned half round.
    fn decal(&mut self, path: &CenterlinePath, decal: &Decal) {
        let Some(key) = decal.material_key() else {
            return;
        };
        let total = path.total_length_m();
        if total.is_nan() || total <= 0.0 || decal.length_m >= total {
            return;
        }
        self.register(&key, "decal", [1.0, 1.0, 1.0, 1.0]);
        let left = decal.lat_m + decal.width_m / 2.0;
        let right = decal.lat_m - decal.width_m / 2.0;
        let columns = ((decal.width_m / DECAL_COLUMN_M).ceil() as usize).max(1);
        let first = self.chunks.len();
        self.strip(
            path,
            decal.start_m,
            decal.start_m + decal.length_m,
            DECAL_ROW_M,
            &key,
            move |_, _, out| {
                for c in 0..=columns {
                    let lat = left - (left - right) * c as f32 / columns as f32;
                    out.push(ProfilePoint::lifted(lat, DECAL_LIFT_M));
                }
            },
            |_, _, p| p.2,
        );
        // `strip` wrote u = station (unwrapped from the span's start) and
        // v = distance across from the left edge, both in metres.
        let start = decal.start_m.rem_euclid(total);
        for chunk in &mut self.chunks[first..] {
            for uv in chunk.uvs.as_chunks_mut::<2>().0 {
                let along = ((uv[0] - start).rem_euclid(total) / decal.length_m).clamp(0.0, 1.0);
                let across = (uv[1] / decal.width_m).clamp(0.0, 1.0);
                let (u, v) = if decal.reversed {
                    (1.0 - across, along)
                } else {
                    (across, 1.0 - along)
                };
                uv[0] = round(u, 4);
                uv[1] = round(v, 4);
            }
        }
    }

    /// The start/finish line as a chequer: the marking's span along the
    /// track, its lateral extent across, in [`CHEQUER_CHECK_M`] checks
    /// alternating the marking's color with a dark one.
    fn chequer(&mut self, path: &CenterlinePath, marking: &Marking, lo: f32, hi: f32) {
        let light_key = format!("marking_start_finish_{}", color_hex(marking.color));
        let dark_key = format!("marking_start_finish_{}", color_hex(CHEQUER_DARK));
        self.register(&light_key, "marking", marking.color);
        self.register(&dark_key, "marking", CHEQUER_DARK);

        let total = path.total_length_m();
        let length = if path.is_closed() {
            let l = (marking.end_m - marking.start_m).rem_euclid(total);
            if l <= f32::EPSILON {
                total
            } else {
                l
            }
        } else {
            marking.end_m - marking.start_m
        };
        if length <= 0.0 || !length.is_finite() || hi - lo <= 0.0 {
            return;
        }
        let rows = (length / CHEQUER_CHECK_M).ceil() as usize;
        let cols = ((hi - lo) / CHEQUER_CHECK_M).ceil() as usize;
        for row in 0..rows {
            let s0 = marking.start_m + row as f32 * CHEQUER_CHECK_M;
            let s1 = (s0 + CHEQUER_CHECK_M).min(marking.start_m + length);
            for col in 0..cols {
                let l0 = lo + col as f32 * CHEQUER_CHECK_M;
                let l1 = (l0 + CHEQUER_CHECK_M).min(hi);
                let key = if (row + col) % 2 == 0 {
                    &light_key
                } else {
                    &dark_key
                };
                self.paint(
                    path,
                    s0,
                    s1,
                    CHEQUER_CHECK_M,
                    key,
                    l1,
                    l0,
                    GRID_PAINT_LIFT_M,
                );
            }
        }
    }

    /// White outlines for the starting grid, one per slot, in station /
    /// lateral space so the back rows bend with the track, each with the
    /// short "pole" stub ahead of its left side.
    fn grid_boxes(&mut self, track: &TrackFile, path: &CenterlinePath) {
        let key = format!("marking_grid_slot_{}", color_hex([1.0, 1.0, 1.0, 1.0]));
        self.register(&key, "marking", [1.0, 1.0, 1.0, 1.0]);

        let (half_len, half_w) = (GRID_BOX_LEN_M / 2.0, GRID_BOX_WIDTH_M / 2.0);
        for (station, lateral) in grid_slot_frames(track, path) {
            let (back, front) = (station - half_len, station + half_len);
            let (right, left) = (lateral - half_w, lateral + half_w);
            // Front and back bars, full width.
            self.paint(
                path,
                front - LINE_WIDTH_M,
                front,
                STEP_M,
                &key,
                left,
                right,
                GRID_PAINT_LIFT_M,
            );
            self.paint(
                path,
                back,
                back + LINE_WIDTH_M,
                STEP_M,
                &key,
                left,
                right,
                GRID_PAINT_LIFT_M,
            );
            // Sides.
            self.paint(
                path,
                back,
                front,
                STEP_M,
                &key,
                left,
                left - LINE_WIDTH_M,
                GRID_PAINT_LIFT_M,
            );
            self.paint(
                path,
                back,
                front,
                STEP_M,
                &key,
                right + LINE_WIDTH_M,
                right,
                GRID_PAINT_LIFT_M,
            );
            // Pole stub ahead of the left side.
            self.paint(
                path,
                front,
                front + GRID_POLE_STUB_M,
                STEP_M,
                &key,
                left,
                left - LINE_WIDTH_M,
                GRID_PAINT_LIFT_M,
            );
        }
    }

    /// The rubbered racing line: a dark worn core with softer edge strips
    /// either side, along the track's raceline when it has one — and along
    /// the centerline eased toward the inside of each corner when it does
    /// not. Family `road`, not `marking`: this is grime, not paint.
    fn wear(&mut self, track: &TrackFile, path: &CenterlinePath) {
        self.register("wear_core", "road", WEAR_CORE_COLOR);
        self.register("wear_edge", "road", WEAR_EDGE_COLOR);

        let ground = self.ground;
        // Seated on the road surface wherever the line crosses it, so the
        // band follows the banking rather than the raceline's own heights.
        let seat = move |_: &PathSample, _: f32, p: (f32, f32, f32)| {
            ground.map_or(p.2, |field| field.surface_height_near(p.0, p.1, p.2))
        };

        let raceline: Vec<[f32; 3]> = track.raceline.iter().map(|p| [p.x, p.y, p.z]).collect();
        let line = if raceline.len() >= 4 {
            CenterlinePath::from_polyline_with(&raceline, WEAR_CORE_HALF_M, track.closed_loop)
        } else {
            None
        };

        let (outer, inner) = (WEAR_CORE_HALF_M + WEAR_EDGE_M, WEAR_CORE_HALF_M);
        let bands: [(&str, f32, f32); 3] = [
            ("wear_edge", outer, inner),
            ("wear_core", inner, -inner),
            ("wear_edge", -inner, -outer),
        ];
        match &line {
            Some(line) => {
                for (key, a, b) in bands {
                    self.strip(
                        line,
                        0.0,
                        line.total_length_m(),
                        WEAR_STEP_M,
                        key,
                        move |_, _, out| {
                            out.push(ProfilePoint::lifted(a, WEAR_LIFT_M));
                            out.push(ProfilePoint::lifted(b, WEAR_LIFT_M));
                        },
                        seat,
                    );
                }
            }
            None => {
                // Ease a metre toward the inside of each corner: curvature
                // scaled so a 200 m radius already puts the line fully
                // inside, without the jump a bare sign would make.
                let centre = |sample: &PathSample| {
                    (curvature_at(path, sample.station_m) * 200.0).clamp(-1.0, 1.0)
                };
                for (key, a, b) in bands {
                    self.strip(
                        path,
                        0.0,
                        path.total_length_m(),
                        WEAR_STEP_M,
                        key,
                        move |sample, _, out| {
                            let c = centre(sample);
                            out.push(ProfilePoint::lifted(c + a, WEAR_LIFT_M));
                            out.push(ProfilePoint::lifted(c + b, WEAR_LIFT_M));
                        },
                        seat,
                    );
                }
            }
        }
    }
}

/// How the pit lane sits against the road, resolved once per bake.
#[derive(Default)]
struct LaneRelation {
    /// +1 when the lane lies on the track's left, −1 on its right.
    lane_side: i32,
    /// Lane station spans `(start, end, parallel)`: parallel where the lane
    /// runs clear of the road, tapering (entry / exit) otherwise.
    spans: Vec<(f32, f32, bool)>,
    /// Road station spans, per side, where the lane overlaps the road edge
    /// and the edge line must break.
    edge_gaps: Vec<(Side, f32, f32)>,
}

impl LaneRelation {
    fn resolve(
        path: &CenterlinePath,
        lane: &CenterlinePath,
        field: Option<&TerrainHeightfield>,
    ) -> Self {
        /// Widest search for the road from a lane point.
        const REACH_M: f32 = 80.0;
        /// Overlap spans closer than this are one break in the edge line.
        const MERGE_M: f32 = 3.0;
        /// Slack either side of an overlap span.
        const SLACK_M: f32 = 1.0;

        let Some(field) = field else {
            return Self::default();
        };
        let half_lane = lane.sample_at(0.0).width_left_m;
        let total = lane.total_length_m();
        let steps = (total / STEP_M).ceil().max(1.0) as usize;

        // Walk the lane, relating each point to the road.
        let mut votes = 0i32;
        let mut parallel_flags: Vec<bool> = Vec::with_capacity(steps + 1);
        let mut overlaps: Vec<(Side, f32)> = Vec::new();
        for i in 0..=steps {
            let s = total * (i as f32 / steps as f32);
            let p = lane.sample_at(s).pos;
            match field.nearest_track_point(p.0, p.1, REACH_M) {
                Some((road_s, lat, half)) => {
                    votes += if lat >= 0.0 { 1 } else { -1 };
                    let beyond = lat.abs() - half;
                    parallel_flags.push(beyond >= half_lane + PIT_PARALLEL_GAP_M);
                    if beyond < half_lane {
                        let side = if lat >= 0.0 { Side::Left } else { Side::Right };
                        overlaps.push((side, road_s));
                    }
                }
                None => parallel_flags.push(true),
            }
        }
        let lane_side = if votes >= 0 { 1 } else { -1 };

        // Runs of equal flags -> spans.
        let mut spans: Vec<(f32, f32, bool)> = Vec::new();
        let station = |i: usize| total * (i as f32 / steps as f32);
        let mut run_start = 0usize;
        for i in 1..=parallel_flags.len() {
            if i == parallel_flags.len() || parallel_flags[i] != parallel_flags[run_start] {
                spans.push((
                    station(run_start),
                    station(i.min(steps)),
                    parallel_flags[run_start],
                ));
                run_start = i;
            }
        }

        // Cluster the overlapping road stations per side into edge gaps.
        let mut edge_gaps: Vec<(Side, f32, f32)> = Vec::new();
        for side in [Side::Left, Side::Right] {
            let mut stations: Vec<f32> = overlaps
                .iter()
                .filter(|(s, _)| *s == side)
                .map(|(_, st)| *st)
                .collect();
            stations.sort_by(f32::total_cmp);
            let mut open: Option<(f32, f32)> = None;
            for s in stations {
                open = match open {
                    Some((a, b)) if s - b <= MERGE_M => Some((a, s)),
                    Some((a, b)) => {
                        edge_gaps.push((side, a - SLACK_M, b + SLACK_M));
                        let _ = (a, b);
                        Some((s, s))
                    }
                    None => Some((s, s)),
                };
            }
            if let Some((a, b)) = open {
                edge_gaps.push((side, a - SLACK_M, b + SLACK_M));
            }
        }
        let road_total = path.total_length_m();
        for gap in &mut edge_gaps {
            gap.1 = gap.1.max(0.0);
            gap.2 = gap.2.min(road_total);
        }

        Self {
            lane_side,
            spans,
            edge_gaps,
        }
    }
}

/// Every starting slot as `(station, lateral)` on the track, in slot order:
/// the authored spawn points projected onto their centerline sample's
/// frame, or the server's 16-slot fallback laid out along the track.
fn grid_slot_frames(track: &TrackFile, path: &CenterlinePath) -> Vec<(f32, f32)> {
    let samples = path.samples();
    if samples.is_empty() {
        return Vec::new();
    }
    if !track.spawn_points.is_empty() {
        return track
            .spawn_points
            .iter()
            .filter_map(|spawn| {
                let sample = samples.get(spawn.position)?;
                let (sin_h, cos_h) = sample.heading_rad.sin_cos();
                let along = cos_h * spawn.offset_x + sin_h * spawn.offset_y;
                let lateral = -sin_h * spawn.offset_x + cos_h * spawn.offset_y;
                Some((sample.station_m + along, lateral))
            })
            .collect();
    }
    (0..GRID_SLOTS)
        .map(|i| {
            let row = (i / 2) as f32;
            let column = (i % 2) as f32;
            (
                -(row * GRID_SPACING_M + column * GRID_STAGGER_M),
                (column - 0.5) * GRID_LATERAL_M,
            )
        })
        .collect()
}

fn curb_style_colors(style: &str) -> ([f32; 4], [f32; 4]) {
    let white = [0.92, 0.92, 0.92, 1.0];
    match style {
        "yellow_black" => ([0.9, 0.75, 0.05, 1.0], [0.08, 0.08, 0.08, 1.0]),
        "green_white" => ([0.1, 0.5, 0.15, 1.0], white),
        "blue_white" => ([0.1, 0.25, 0.7, 1.0], white),
        _ => ([0.75, 0.12, 0.1, 1.0], white),
    }
}

// ---------------------------------------------------------------------------
// Non-geometry payload
// ---------------------------------------------------------------------------

/// Curvature below which a grandstand's station counts as straight and no
/// `radius_m` is emitted (radius over 1 km).
const STAND_STRAIGHT_KAPPA: f32 = 1.0 / 1000.0;
/// Default stand length when the scene does not say.
const STAND_DEFAULT_LENGTH_M: f32 = 30.0;
/// Pit modules tile at this pitch along the lane (`garage_6m`,
/// `pit_wall_6m`).
const PIT_MODULE_M: f32 = 6.0;
/// Asset the pre-kit scenes used for pit garages; superseded by the
/// generated complex whenever the scene has a pit lane.
const LEGACY_PIT_GARAGE_ASSET: &str = "pit_garage";
/// The pit wall stands in the apron between the lane and the road, at most
/// this far off the lane's edge (its team stand reaches 2.3 m back over
/// the lane).
const PIT_WALL_MAX_OFFSET_M: f32 = 1.0;
/// Least apron between the lane's road-side edge and the track edge for a
/// wall to stand in it outside the box span: the wall sits half way across
/// (capped at [`PIT_WALL_MAX_OFFSET_M`] from the lane), so this keeps it two
/// metres off the road.
const PIT_TAPER_WALL_MIN_M: f32 = 3.0;
/// How far from the lane the road is looked for when sizing the apron.
const PIT_APRON_REACH_M: f32 = 80.0;

/// The nearest centerline sample to a point and the point's signed
/// lateral offset from it (positive = left of the course).
fn nearest_sample(path: &CenterlinePath, x: f32, y: f32) -> (PathSample, f32) {
    let sample = *path
        .samples()
        .iter()
        .min_by(|a, b| {
            let da = (a.pos.0 - x).powi(2) + (a.pos.1 - y).powi(2);
            let db = (b.pos.0 - x).powi(2) + (b.pos.1 - y).powi(2);
            da.total_cmp(&db)
        })
        .expect("a centerline path always has samples");
    let (sin_h, cos_h) = sample.heading_rad.sin_cos();
    let lat = -sin_h * (x - sample.pos.0) + cos_h * (y - sample.pos.1);
    (sample, lat)
}

/// Signed centerline radius at a stand: positive when the stand is on the
/// outside of the bend, `None` on a straight.
fn stand_radius_m(path: &CenterlinePath, x: f32, y: f32) -> Option<f32> {
    let (sample, lat) = nearest_sample(path, x, y);
    let kappa = curvature_at(path, sample.station_m);
    if kappa.abs() < STAND_STRAIGHT_KAPPA {
        return None;
    }
    // Curvature is positive turning left, whose outside is the right-hand
    // side (negative lateral): outside means the two signs disagree.
    let outside = kappa * lat < 0.0;
    let radius = 1.0 / kappa.abs();
    Some(round(if outside { radius } else { -radius }, 2))
}

fn bake_prop(p: &Prop, path: &CenterlinePath) -> UeProp {
    let (length_m, radius_m, span_m) = match p.kind {
        PropKind::Grandstand => (
            Some(round(p.length_m.unwrap_or(STAND_DEFAULT_LENGTH_M), 2)),
            stand_radius_m(path, p.x, p.y),
            None,
        ),
        PropKind::Bridge => {
            let (sample, _) = nearest_sample(path, p.x, p.y);
            (
                None,
                None,
                Some(round(sample.width_left_m + sample.width_right_m, 2)),
            )
        }
        _ => (None, None, None),
    };
    UeProp {
        kind: p.kind.label().to_string(),
        asset: p.asset.clone(),
        location: to_ue((p.x, p.y, p.z)),
        yaw_deg: round(-p.yaw_rad.to_degrees(), 3),
        scale: round(p.scale, 4),
        text: p.text.clone(),
        length_m,
        radius_m,
        span_m,
    }
}

/// The two colours of a painted run-off style, or `None` for a style the
/// bake does not know (which is then left bare).
pub fn runoff_paint_colours(style: &str) -> Option<[[f32; 4]; 2]> {
    const RED: [f32; 4] = [0.72, 0.08, 0.06, 1.0];
    const YELLOW: [f32; 4] = [0.9, 0.72, 0.05, 1.0];
    const WHITE: [f32; 4] = [0.85, 0.85, 0.82, 1.0];
    const BLUE: [f32; 4] = [0.05, 0.25, 0.7, 1.0];
    const GREEN: [f32; 4] = [0.1, 0.5, 0.18, 1.0];
    Some(match style {
        "red_yellow" => [RED, YELLOW],
        "red_white" => [RED, WHITE],
        "blue_red" => [BLUE, RED],
        "blue_white" => [BLUE, WHITE],
        "green_white" => [GREEN, WHITE],
        _ => return None,
    })
}

/// DRS detection and activation lines across the road (`marking` family:
/// paint), one per zone in the track file.
impl Bake<'_> {
    fn drs_lines(&mut self, track: &TrackFile, path: &CenterlinePath) {
        if track.drs_zones.is_empty() {
            return;
        }
        let key = format!("marking_drs_line_{}", color_hex(DRS_LINE_COLOR));
        self.register(&key, "marking", DRS_LINE_COLOR);
        for zone in &track.drs_zones {
            for station in [zone.detection_m, zone.start_m] {
                let sample = path.sample_at(station);
                self.paint(
                    path,
                    station - DRS_LINE_WIDTH_M / 2.0,
                    station + DRS_LINE_WIDTH_M / 2.0,
                    STEP_M,
                    &key,
                    sample.width_left_m,
                    -sample.width_right_m,
                    GRID_PAINT_LIFT_M,
                );
            }
        }
    }
}

/// The boards that mark the DRS zones: a `DRS` sign on both sides at each
/// activation line and a `DRS DETECTION` sign at each detection line,
/// [`DRS_BOARD_OFF_EDGE_M`] past the road edge, facing along the course
/// like a corner sign (the Unreal builder turns the face to the road).
fn bake_drs_boards(
    track: &TrackFile,
    path: &CenterlinePath,
    terrain: Option<&TerrainHeightfield>,
) -> Vec<UeProp> {
    let mut out = Vec::new();
    for zone in &track.drs_zones {
        for (station, text) in [(zone.detection_m, "DRS DETECTION"), (zone.start_m, "DRS")] {
            let sample = path.sample_at(station);
            for sign in [1.0f32, -1.0] {
                let half = if sign > 0.0 {
                    sample.width_left_m
                } else {
                    sample.width_right_m
                };
                let lat = sign * (half + DRS_BOARD_OFF_EDGE_M);
                let p = offset_point(&sample, lat);
                let z = terrain.map_or(p.2, |field| field.ground_height_at(p.0, p.1));
                out.push(UeProp {
                    kind: "board".to_string(),
                    asset: "corner_sign".to_string(),
                    location: to_ue((p.0, p.1, z)),
                    yaw_deg: round(-sample.heading_rad.to_degrees(), 3),
                    scale: 1.0,
                    text: Some(text.to_string()),
                    length_m: None,
                    radius_m: None,
                    span_m: None,
                });
            }
        }
    }
    out
}

/// The scene's props, plus the pit complex the pit lane implies.
///
/// A `grandstand` carries its length and the signed bend radius at its
/// station so the importer can lay straight or wedge bays; a `bridge`
/// carries the road width its span is scaled to. When the scene has a pit
/// lane the garages, end blocks and pit walls are generated from its box
/// layout ([`bake_pit_complex`]) and the pre-kit `building/pit_garage`
/// stand-ins are dropped, since they stood in for exactly that.
fn bake_props(
    track: &TrackFile,
    scene: &AtsScene,
    path: &CenterlinePath,
    lane: Option<&CenterlinePath>,
    relation: &LaneRelation,
    terrain: Option<&TerrainHeightfield>,
) -> Vec<UeProp> {
    let pit = scene
        .pit_lane
        .as_ref()
        .zip(lane)
        .map(|(pit, lane)| bake_pit_complex(lane, pit.width_m, pit.box_count, relation, terrain))
        .unwrap_or_default();
    let complex_present = !pit.is_empty();
    scene
        .props
        .iter()
        .filter(|p| {
            !(complex_present && p.kind == PropKind::Building && p.asset == LEGACY_PIT_GARAGE_ASSET)
        })
        .map(|p| bake_prop(p, path))
        .chain(pit)
        .chain(bake_drs_boards(track, path, terrain))
        .collect()
}

/// A pit module in the lane's frame: station, signed lateral (positive =
/// left of the lane), and whether it faces the lane's left.
fn pit_module(
    lane: &CenterlinePath,
    kind_asset: (&str, &str),
    station_m: f32,
    lat_m: f32,
    faces_left: bool,
) -> UeProp {
    use std::f32::consts::PI;
    let sample = lane.sample_at(station_m);
    let pos = offset_point(&sample, lat_m);
    // The authored modules open toward local +Y in Unreal's frame, which
    // is the right-hand side of the heading in the server frame; facing
    // left is the same module turned round.
    let yaw = if faces_left {
        sample.heading_rad + PI
    } else {
        sample.heading_rad
    };
    UeProp {
        kind: kind_asset.0.to_string(),
        asset: kind_asset.1.to_string(),
        location: to_ue((pos.0, pos.1, sample.pos.2)),
        yaw_deg: round(-yaw.to_degrees(), 3),
        scale: 1.0,
        text: None,
        length_m: None,
        radius_m: None,
        span_m: None,
    }
}

/// The pit complex implied by the lane: one `pit/garage_6m` per box (with a
/// `pit/box_kit` in front of it) on the
/// garage side of the parallel pit road (6 m pitch, centred on the road,
/// door on the lane), a `pit/garage_end` beyond the first and last box,
/// `pit/pit_wall_6m` along the road side of the lane over the box span and
/// `pit/pit_wall_plain_6m` over the rest of the parallel road. Nothing on
/// the entry and exit tapers, where the lane merges with the road.
fn bake_pit_complex(
    lane: &CenterlinePath,
    width_m: f32,
    box_count: u32,
    relation: &LaneRelation,
    terrain: Option<&TerrainHeightfield>,
) -> Vec<UeProp> {
    let total = lane.total_length_m();
    // The longest stretch of pit road that runs clear of the track; a lane
    // whose relation could not be resolved is taken as parallel throughout.
    let (start, end) = relation
        .spans
        .iter()
        .filter(|(_, _, parallel)| *parallel)
        .map(|(s, e, _)| (*s, *e))
        .max_by(|a, b| (a.1 - a.0).total_cmp(&(b.1 - b.0)))
        .unwrap_or((0.0, total));
    let span = end - start;
    let boxes = (box_count as f32).min((span / PIT_MODULE_M).floor()) as u32;
    if boxes == 0 {
        return Vec::new();
    }

    let half = width_m / 2.0;
    // The lane runs the track's way: with the lane on the track's left the
    // garages are further left (+), the track to the right (-). Every
    // module looks toward the track — the garages across the lane, the
    // walls straight at it — so with the lane on the left they all face
    // right, and the other way round.
    let side = relation.lane_side as f32;
    let face_left = relation.lane_side < 0;
    let box_edge = side * half;
    let mid = (start + end) / 2.0;
    let row = boxes as f32 * PIT_MODULE_M;
    let row_start = mid - row / 2.0;

    let mut out = Vec::new();
    for i in 0..boxes {
        let s = row_start + (i as f32 + 0.5) * PIT_MODULE_M;
        out.push(pit_module(
            lane,
            ("pit", "garage_6m"),
            s,
            box_edge,
            face_left,
        ));
        // The crew's kit on the working lane in front of the door, on the
        // same pivot: it is authored to reach 2.6 m out from the garage.
        out.push(pit_module(lane, ("pit", "box_kit"), s, box_edge, face_left));
    }
    for s in [
        row_start - PIT_MODULE_M / 2.0,
        row_start + row + PIT_MODULE_M / 2.0,
    ] {
        if s - PIT_MODULE_M / 2.0 >= start && s + PIT_MODULE_M / 2.0 <= end {
            out.push(pit_module(
                lane,
                ("pit", "garage_end"),
                s,
                box_edge,
                face_left,
            ));
        }
    }

    // The pit wall stands in the apron between the lane and the road: half
    // way across it, capped so the wall face never reaches the road edge
    // and the team stand behind it never reaches far into the lane.
    let wall_lat = |s: f32| {
        let sample = lane.sample_at(s);
        let shift = terrain
            .and_then(|field| {
                let edge = offset_point(&sample, -side * half);
                let (_, lat, road_half) =
                    field.nearest_track_point(edge.0, edge.1, PIT_APRON_REACH_M)?;
                Some(((lat.abs() - road_half) * 0.5).clamp(0.0, PIT_WALL_MAX_OFFSET_M))
            })
            .unwrap_or(0.0);
        -side * (half + shift)
    };
    let mut s = row_start + PIT_MODULE_M / 2.0;
    while s < row_start + row {
        out.push(pit_module(
            lane,
            ("pit", "pit_wall_6m"),
            s,
            wall_lat(s),
            face_left,
        ));
        s += PIT_MODULE_M;
    }
    // Plain walls over the rest of the pit road, whole modules only.
    let mut s = row_start - PIT_MODULE_M / 2.0;
    while s - PIT_MODULE_M / 2.0 >= start {
        out.push(pit_module(
            lane,
            ("pit", "pit_wall_plain_6m"),
            s,
            wall_lat(s),
            face_left,
        ));
        s -= PIT_MODULE_M;
    }
    let mut s = row_start + row + PIT_MODULE_M / 2.0;
    while s + PIT_MODULE_M / 2.0 <= end {
        out.push(pit_module(
            lane,
            ("pit", "pit_wall_plain_6m"),
            s,
            wall_lat(s),
            face_left,
        ));
        s += PIT_MODULE_M;
    }
    // The rest of the lane — the other parallel stretches, and the entry
    // and exit as far as there is an apron between the two roads to
    // stand a wall in. The walls used to stop at the box span's ends,
    // which left the whole of a long pit entry (Hockenheim's, Shanghai's)
    // open between the lane and the track.
    let apron = |s: f32| -> Option<f32> {
        let field = terrain?;
        let sample = lane.sample_at(s);
        let edge = offset_point(&sample, -side * half);
        let (_, lat, road_half) = field.nearest_track_point(edge.0, edge.1, PIT_APRON_REACH_M)?;
        Some(lat.abs() - road_half)
    };
    let mut s = PIT_MODULE_M / 2.0;
    while s + PIT_MODULE_M / 2.0 <= total {
        let walled = s > start && s < end;
        if !walled && apron(s).is_some_and(|a| a >= PIT_TAPER_WALL_MIN_M) {
            out.push(pit_module(
                lane,
                ("pit", "pit_wall_plain_6m"),
                s,
                wall_lat(s),
                face_left,
            ));
        }
        s += PIT_MODULE_M;
    }
    out
}

/// The starting grid, resolved exactly the way the server resolves it, so a
/// car spawned by the client sits where the server thinks it is.
fn bake_grid(track: &TrackFile, path: &CenterlinePath) -> Vec<UeGridSlot> {
    let samples = path.samples();
    if samples.is_empty() {
        return Vec::new();
    }

    if !track.spawn_points.is_empty() {
        return track
            .spawn_points
            .iter()
            .enumerate()
            .filter_map(|(idx, spawn)| {
                let sample = samples.get(spawn.position)?;
                Some(UeGridSlot {
                    position: idx as u32 + 1,
                    location: to_ue((
                        sample.pos.0 + spawn.offset_x,
                        sample.pos.1 + spawn.offset_y,
                        sample.pos.2,
                    )),
                    yaw_deg: round(-sample.heading_rad.to_degrees(), 3),
                })
            })
            .collect();
    }

    // No authored grid: two columns stepping back from the line, matching
    // the server's fallback.
    let start = samples[0];
    let (sin_h, cos_h) = start.heading_rad.sin_cos();
    (0..GRID_SLOTS)
        .map(|i| {
            let row = (i / 2) as f32;
            let column = (i % 2) as f32;
            let forward = -(row * GRID_SPACING_M + column * GRID_STAGGER_M);
            let lateral = (column - 0.5) * GRID_LATERAL_M;
            UeGridSlot {
                position: i + 1,
                location: to_ue((
                    start.pos.0 + forward * cos_h - lateral * sin_h,
                    start.pos.1 + forward * sin_h + lateral * cos_h,
                    start.pos.2,
                )),
                yaw_deg: round(-start.heading_rad.to_degrees(), 3),
            }
        })
        .collect()
}

/// The start/finish line, anchored on the scene's `start_finish` marking
/// (its centre, across and along) — or station 0 across the full road when
/// the scene has none.
fn bake_start_finish(scene: &AtsScene, path: &CenterlinePath) -> UeStartFinish {
    let marking = scene
        .markings
        .iter()
        .find(|m| m.kind == MarkingKind::StartFinish);
    let (station, lateral) = match marking {
        Some(m) => {
            let total = path.total_length_m();
            let length = if path.is_closed() {
                (m.end_m - m.start_m).rem_euclid(total)
            } else {
                (m.end_m - m.start_m).max(0.0)
            };
            (m.start_m + length / 2.0, (m.lat_from_m + m.lat_to_m) / 2.0)
        }
        None => (0.0, 0.0),
    };
    let sample = path.sample_at(station);
    UeStartFinish {
        location: to_ue(offset_point(&sample, lateral)),
        yaw_deg: round(-sample.heading_rad.to_degrees(), 3),
        width_m: round(sample.width_left_m + sample.width_right_m, 3),
    }
}

fn bake_centerline(path: &CenterlinePath) -> Vec<UeCenterlinePoint> {
    path.samples()
        .iter()
        .map(|s| UeCenterlinePoint {
            s_cm: round(s.station_m * M_TO_CM, 1),
            location: to_ue(s.pos),
            yaw_deg: round(-s.heading_rad.to_degrees(), 3),
            half_left_cm: round(s.width_left_m * M_TO_CM, 1),
            half_right_cm: round(s.width_right_m * M_TO_CM, 1),
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Track meters -> Unreal centimeters, mirroring Y.
fn to_ue(p: (f32, f32, f32)) -> [f32; 3] {
    [
        round(p.0 * M_TO_CM, 1),
        round(-p.1 * M_TO_CM, 1),
        round(p.2 * M_TO_CM, 1),
    ]
}

fn push_position(out: &mut Vec<f32>, p: (f32, f32, f32)) {
    out.extend_from_slice(&to_ue(p));
}

fn push_normal(out: &mut Vec<f32>, n: (f32, f32, f32)) {
    out.push(round(n.0, 4));
    out.push(round(-n.1, 4));
    out.push(round(n.2, 4));
}

/// Fixed-precision rounding. Keeps repeated exports byte-identical and cuts
/// the file size roughly in half; `-0.0` is collapsed to `0.0` so the sign
/// of a zero can never flip between runs.
fn round(v: f32, places: i32) -> f32 {
    if !v.is_finite() {
        return 0.0;
    }
    let f = 10f32.powi(places);
    let r = (v * f).round() / f;
    if r == 0.0 {
        0.0
    } else {
        r
    }
}

fn sub(a: (f32, f32, f32), b: (f32, f32, f32)) -> (f32, f32, f32) {
    (a.0 - b.0, a.1 - b.1, a.2 - b.2)
}

fn cross(a: (f32, f32, f32), b: (f32, f32, f32)) -> (f32, f32, f32) {
    (
        a.1 * b.2 - a.2 * b.1,
        a.2 * b.0 - a.0 * b.2,
        a.0 * b.1 - a.1 * b.0,
    )
}

/// How far a triangle is from degenerate: the sine of the angle between two
/// of its edges, so it is scale-free. An absolute area test cannot work at
/// track scale, where a 0.2-degree sliver spanning 100 m still has a large
/// cross product.
fn facet_quality(a: (f32, f32, f32), b: (f32, f32, f32), c: (f32, f32, f32)) -> f32 {
    let e1 = sub(b, a);
    let e2 = sub(c, a);
    let scale = length(e1) * length(e2);
    if scale < 1e-9 {
        return 0.0;
    }
    length(cross(e1, e2)) / scale
}

fn dot(a: (f32, f32, f32), b: (f32, f32, f32)) -> f32 {
    a.0 * b.0 + a.1 * b.1 + a.2 * b.2
}

fn length(v: (f32, f32, f32)) -> f32 {
    (v.0 * v.0 + v.1 * v.1 + v.2 * v.2).sqrt()
}

fn normalize(v: (f32, f32, f32)) -> (f32, f32, f32) {
    let len = length(v);
    if len > 1e-6 {
        (v.0 / len, v.1 / len, v.2 / len)
    } else {
        (0.0, 0.0, 1.0)
    }
}

fn distance(a: (f32, f32, f32), b: (f32, f32, f32)) -> f32 {
    let d = sub(b, a);
    (d.0 * d.0 + d.1 * d.1 + d.2 * d.2).sqrt()
}

/// Lowercase hex of the RGB channels, for disambiguating material keys.
fn color_hex(c: [f32; 4]) -> String {
    let byte = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    format!("{:02x}{:02x}{:02x}", byte(c[0]), byte(c[1]), byte(c[2]))
}

/// Reduce a free-form style key to something safe in an asset name.
fn sanitize(s: &str) -> String {
    let cleaned: String = s
        .trim()
        .to_ascii_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    if cleaned.is_empty() {
        "default".to_string()
    } else {
        cleaned
    }
}
