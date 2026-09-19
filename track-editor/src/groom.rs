//! Deterministic clean-up and dressing of prop placement in a `.ats`
//! scene.
//!
//! The enrichment pass that populated the shipped scenes placed its props
//! against a flat world: tire walls ended up scattered 40–100 m into the
//! fields at road height, and buildings sit pinned to the road's elevation
//! even where the terrain has since fallen away. Grooming re-anchors all of
//! that against the same [`TerrainHeightfield`] the viewport and the bake
//! use, and then dresses the circuit with generated furniture:
//!
//! - **Tire walls and barriers** move to the outer edge of their station's
//!   prepared runoff (gravel / asphalt / sand / concrete patch when one is
//!   authored there, a fixed verge offset otherwise), aligned parallel to
//!   the track and seated on the blended ground.
//! - **Grandstands and buildings** are tested as the oriented slabs Unreal
//!   builds them as (30 m × 12 m and 15 m × 10 m, times scale) and pushed
//!   outward until every corner clears the road and the pit lane by 3 m;
//!   one that cannot be seated within 60 m is deleted and reported.
//! - **Everything else authored** keeps its position and yaw and is only
//!   re-seated vertically on the blended ground.
//! - **Distance boards** (`board_200m` / `board_100m` / `board_50m` signs)
//!   are laid on the outside of every braking corner's approach.
//! - **Armco** (`armco_generic` barriers) lines both sides of every
//!   straight the tire-wall pass leaves bare.
//! - **Tree belts** (`tree_generic`) fill the land 22–90 m out on both
//!   sides, thinned around buildings, grandstands and prepared runoff.
//!
//! Every generated pass *owns* the props it lays, recognised by their
//! `asset` key (the same convention as the enrichment's
//! `tire_wall_generic`): a pass deletes its own props and re-lays them from
//! scratch, and never touches authored props or another pass's output.
//! Layouts are keyed to cells on a station grid with hash-based jitter —
//! no RNG, no wall clock, no map-order dependence — so a re-groom lays
//! exactly the same props again.
//!
//! Grooming is idempotent: a second pass over a groomed scene changes
//! nothing, so it is safe to run over the whole track set at any time.
//! Every prop's height goes through one function, [`seat_z`], so a change
//! to the ground model reaches all of them.

use std::collections::BTreeMap;
use std::f32::consts::TAU;

use crate::ats::{AtsScene, Prop, PropKind, Side, SurfaceKind};
use crate::barriers;
use crate::dem::DemFile;
use crate::layout::{Layout, Wood};
use crate::props;
use crate::terrain::{self, TerrainHeightfield};
use crate::track_data::TrackFile;
use crate::track_mesh::surface_height;
use crate::track_path::{curvature_at, offset_point, CenterlinePath, PathSample};

/// Gap between the runoff's outer border and the wall placed behind it.
const WALL_GAP_M: f32 = 1.0;
/// Wall offset beyond the track edge when no runoff surface is authored.
const VERGE_OFFSET_M: f32 = 6.0;
/// Walls never move further out than this beyond the track edge, however
/// wide the authored surface is. The shipped scenes carry gravel wedges up
/// to 90 m deep; a wall at such a wedge's far edge reads as randomly
/// scattered, so past this cap the wall stands *in* the runoff instead.
const MAX_WALL_BEYOND_EDGE_M: f32 = 24.0;
/// Below these thresholds a prop counts as already in place and is left
/// byte-identical, which is what makes grooming idempotent.
const MIN_MOVE_M: f32 = 0.05;
const MIN_YAW_RAD: f32 = 1e-3;

/// A wall belongs at a corner. It survives grooming only when the course
/// bends at least this much (radius 250 m) somewhere within
/// [`CORNER_WINDOW_M`] of its station; the enrichment ringed entire
/// circuits with walls, and on the straights they read as clutter.
const CORNER_KAPPA: f32 = 1.0 / 250.0;
const CORNER_WINDOW_M: f32 = 60.0;

/// Corner walls are re-laid as continuous runs: anchors on one side within
/// …of one kit module per cell. The tire-wall module (`tires_4m`) is 4 m
/// long at scale 1, so 4 m cells give a continuous run, as the armco's
const WALL_SCALE: f32 = 1.0;

/// Free clearance between a prop's footprint and the road edge or the pit
/// lane's edge.
const PROP_CLEARANCE_M: f32 = 1.5;
/// Buildings this close to the pit lane are pit boxes: align them with the
/// lane so the pit complex reads as a row, not a scatter.
const PIT_BOX_ALIGN_RANGE_M: f32 = 40.0;

// ---- Boards and fences ----------------------------------------------------

/// A `board` snaps onto the nearest barrier or tire wall within this
/// distance; further than that it is groomed like a sign.
const BOARD_SNAP_RANGE_M: f32 = 40.0;
/// How far behind the barrier line (away from the road) a snapped board
/// stands, so its face clears the rail.
const BOARD_BEHIND_BARRIER_M: f32 = 0.4;
/// A `fence` panel stands this far behind where a wall would stand at its
/// station (the spectator fence runs behind the armco).
const FENCE_BEHIND_WALL_M: f32 = 1.5;

// ---- Distance boards ------------------------------------------------------

/// Asset keys of the three boards, with their distance before the corner
/// entry and the text painted on them.
pub const BOARDS: [(f32, &str, &str); 3] = [
    (200.0, "board_200m", "200"),
    (100.0, "board_100m", "100"),
    (50.0, "board_50m", "50"),
];
/// Station step of the curvature sweep that finds corner entries.
const CORNER_SWEEP_M: f32 = 5.0;
/// Corner runs closer than this along the course are one corner complex
/// (a chicane, an S, a double apex) with a single entry.
const CORNER_MERGE_GAP_M: f32 = 80.0;
/// A braking corner needs this much sub-threshold curvature before its
/// entry: boards belong on a straight, not between the bends of an S.
const BOARD_APPROACH_M: f32 = 150.0;
/// Cornering speed model, `v = sqrt(a_lat / kappa)`: with a lateral grip
/// of ~12 m/s² a 250 m radius (the corner threshold) still allows 55 m/s,
/// so a corner whose tightest point holds the car under
/// [`BOARD_MAX_CORNER_SPEED_MPS`] (a radius of 75 m or less) after a
/// clean approach costs a real speed drop and gets boards. Faster sweeps
/// — Monza's Curva Grande, Spa's Eau Rouge/Raidillon and Blanchimont, whose
/// digitised radii are 80–105 m — do not.
const BOARD_A_LAT: f32 = 12.0;
const BOARD_MAX_CORNER_SPEED_MPS: f32 = 30.0;
/// Boards stand this far beyond the road edge.
const BOARD_LATERAL_M: f32 = 3.0;
/// Free gap a board keeps from any other prop's footprint.
const BOARD_PROP_CLEAR_M: f32 = 0.5;

// ---- Trees ----------------------------------------------------------------

/// Asset key the tree pass used before the prop kit had species; still
/// owned by the pass, so an old scene's belt is replaced rather than kept
/// alongside the new one.
pub const TREE_ASSET: &str = "tree_generic";
/// The species the belts are planted from, by the leaf type of the wood
/// the tree stands in ([`Wood::leaf`]). Without a dossier the pass plants
/// the mixed set, which is what the generic key used to resolve to.
const BROADLEAF: [&str; 4] = ["broadleaf_m", "broadleaf_l", "broadleaf_s", "bush_cluster"];
const NEEDLELEAF: [&str; 3] = ["conifer_m", "conifer_l", "poplar"];
const MIXED: [&str; 5] = [
    "broadleaf_m",
    "broadleaf_l",
    "conifer_m",
    "poplar",
    "bush_cluster",
];

/// The near-view versions of the two commonest species: real foliage
/// cards rather than the solid blobs, for the band a driver actually
/// looks into. A 97-triangle conifer standing beside a 200 000-triangle
/// car is most of why the trackside read as a decade too old, and the
/// blobs are kept only for the belt behind, where they are all that makes
/// a belt affordable.
const NEAR_TREES: [(&str, &str); 2] = [
    ("broadleaf_m", "broadleaf_m_near"),
    ("conifer_m", "conifer_m_near"),
];

/// Ground cover along the verge: three crossed masked cards each, six
/// triangles, which is the cheapest thing in the kit and the one that
/// stops the grass reading as a flat green plane.
const SCATTER: [&str; 2] = ["grass_clump", "wildflower_clump"];

/// Every asset the tree pass may plant, so it recognises its own work.
fn is_tree_pass_asset(asset: &str) -> bool {
    asset == TREE_ASSET
        || BROADLEAF.contains(&asset)
        || NEEDLELEAF.contains(&asset)
        || MIXED.contains(&asset)
        || NEAR_TREES.iter().any(|(_, near)| *near == asset)
        || SCATTER.contains(&asset)
}

/// Ground cover, as opposed to a tree. Both are `tree` kind — the
/// importer instances them the same way — but a grass tuft has none of a
/// tree's spacing rules: clumps are meant to sit on top of each other.
pub fn is_ground_cover(asset: &str) -> bool {
    SCATTER.contains(&asset)
}

/// The near-view version of a species, where the kit has one.
fn near_variant(asset: &str) -> &str {
    NEAR_TREES
        .iter()
        .find(|(base, _)| *base == asset)
        .map_or(asset, |(_, near)| *near)
}

/// The species for a tree, from the wood it stands in and its own hash.
fn tree_asset(leaf: Option<&str>, roll: f32) -> &'static str {
    let set: &[&'static str] = match leaf {
        Some("broadleaved") => &BROADLEAF,
        Some("needleleaved") => &NEEDLELEAF,
        _ => &MIXED,
    };
    set[((roll * set.len() as f32) as usize).min(set.len() - 1)]
}
/// Station cell size of the tree belts.
const TREE_CELL_M: f32 = 12.0;
/// Depth range of the belt, measured beyond the road edge.
const TREE_BELT_NEAR_M: f32 = 22.0;
const TREE_BELT_FAR_M: f32 = 90.0;
/// Trees closer than this to the road are planted as the near set. It is
/// roughly the distance at which a card-foliage tree stops paying for
/// itself; past it the blobs read the same and cost a twentieth as much.
const TREE_NEAR_VIEW_M: f32 = 60.0;
/// Ground cover is scattered from the verge out to here.
const SCATTER_NEAR_M: f32 = 3.0;
const SCATTER_FAR_M: f32 = 40.0;
/// Clumps per station cell per side. They are six triangles each, so this
/// is a few thousand per circuit and still nothing next to one car.
const SCATTER_PER_CELL: u32 = 7;
const TREE_SCALE_MIN: f32 = 0.8;
const TREE_SCALE_MAX: f32 = 1.5;
/// Share of belt cells left empty so the belts have gaps.
const TREE_EMPTY_CELL_SHARE: f32 = 0.10;
/// Trees per non-empty cell, per side.
const TREE_MIN_PER_CELL: u32 = 3;
const TREE_MAX_PER_CELL: u32 = 6;
/// No trees this close to a building's or grandstand's footprint.
const TREE_STAND_CLEAR_M: f32 = 40.0;
/// No tree this close to any other prop.
const TREE_PROP_CLEAR_M: f32 = 6.0;
/// No tree this close to the pit lane's edge.
const TREE_LANE_CLEAR_M: f32 = 6.0;
/// A tree stays this far outside an authored runoff patch.
const TREE_RUNOFF_MARGIN_M: f32 = 2.0;

// ---- Armco on straights ---------------------------------------------------

/// Asset key every generated barrier carries; the barrier pass owns those.
pub const BARRIER_ASSET: &str = "armco_generic";
/// One barrier segment per cell: the stand-in (and Unreal's placeholder
/// cube, `PlaceholderPropSize`) is 4 m long at scale 1, so cells of 4 m
/// give a continuous rail.
const BARRIER_CELL_M: f32 = 4.0;
/// A cell is skipped when a tire wall, building, grandstand or the pit
/// lane is within this distance of the barrier's spot.
const BARRIER_CLEAR_M: f32 = 6.0;
/// Extra span either side of the pit lane on its own side of the track
/// where no barrier is laid, so a run never crosses the entry/exit tapers.
const BARRIER_PIT_MARGIN_M: f32 = 6.0;

/// Rough circumscribed footprint radius of a small prop stand-in at
/// scale 1 (see `scene::prop_pieces` for the shapes). The large kinds —
/// stands, buildings, walls, barriers — are oriented slabs instead
/// ([`footprint_half_extents`]); their radius only matters for the
/// "hugs the road" exemption in [`groom_props`].
fn prop_radius(kind: PropKind) -> f32 {
    match kind {
        PropKind::Building => 6.4,
        PropKind::Grandstand => 8.7,
        // The authored kit (docs/PROPS.md): a ferris wheel's 45 m footprint,
        // a transporter, a 6 m garage module.
        PropKind::Attraction => 25.0,
        PropKind::Vehicle => 3.0,
        PropKind::Pit => 3.0,
        PropKind::Tree => 1.5,
        PropKind::Sign | PropKind::Board => 1.0,
        PropKind::Light => 0.7,
        PropKind::Cone => 0.3,
        PropKind::Misc | PropKind::TireWall | PropKind::Barrier | PropKind::Fence => 0.6,
        // Never pushed: a bridge stands on the road by design and a sky prop
        // is not on the ground at all.
        PropKind::Bridge | PropKind::Sky => 0.0,
    }
}

/// Oriented footprint of the large stand-ins as half-extents (along the
/// prop's local +X, i.e. its yaw, and across it), matching Unreal's
/// `PlaceholderPropSize`: a grandstand is 30 m × 12 m, a building
/// 15 m × 10 m, a wall or barrier 6 m / 4 m long, all times scale. A
/// grandstand's seating faces local +Y.
fn footprint_half_extents(prop: &Prop) -> Option<(f32, f32)> {
    let scale = prop.scale;
    match prop.kind {
        // A stand is laid from its length; the bays' depth is the family's.
        PropKind::Grandstand => Some((
            prop.length_m.unwrap_or(STAND_DEFAULT_LENGTH_M) * scale / 2.0,
            props::resolve(prop.kind, &prop.asset).map_or(6.0, |a| a.depth_m / 2.0) * scale,
        )),
        PropKind::Building | PropKind::TireWall | PropKind::Barrier => {
            let (half_len, half_wid) = match props::resolve(prop.kind, &prop.asset) {
                Some(a) => (a.length_m / 2.0, a.depth_m / 2.0),
                None => match prop.kind {
                    PropKind::Building => (7.5, 5.0),
                    PropKind::TireWall => (3.0, 0.5),
                    _ => (2.0, 0.3),
                },
            };
            Some((half_len * scale, half_wid * scale))
        }
        _ => None,
    }
}

/// Default stand length when the scene does not say (the importer's too).
const STAND_DEFAULT_LENGTH_M: f32 = 30.0;

/// A prop's push-off radius: the circle round its authored footprint when
/// the kit knows the asset, else the kind's stand-in radius.
fn prop_radius_of(prop: &Prop) -> f32 {
    match props::resolve(prop.kind, &prop.asset) {
        Some(entry) if prop_radius(prop.kind) > 0.0 => entry.footprint_radius_m() * prop.scale,
        _ => prop_radius(prop.kind) * prop.scale,
    }
}

/// Free clearance every point of a building's footprint keeps from the
/// road edge and from the pit lane's edge. Pit garages belong right behind
/// the lane, so this stays small.
const FOOTPRINT_ROAD_CLEAR_M: f32 = 3.0;
/// A grandstand's front wall stays this far back from the road edge: a
/// 30 m-deep block 8 m from the asphalt reads as standing on the track.
const STAND_ROAD_CLEAR_M: f32 = 15.0;

fn footprint_road_clear_m(kind: PropKind) -> f32 {
    match kind {
        PropKind::Grandstand => STAND_ROAD_CLEAR_M,
        _ => FOOTPRINT_ROAD_CLEAR_M,
    }
}
const FOOTPRINT_LANE_CLEAR_M: f32 = 3.0;
/// How far a stand is pushed outward, in 1 m steps, before it is given
/// up and deleted.
const FOOTPRINT_MAX_PUSH_M: f32 = 60.0;

/// The nine probe points of an oriented footprint: corners, edge
/// midpoints and the centre.
fn footprint_samples(
    x: f32,
    y: f32,
    yaw: f32,
    (half_len, half_wid): (f32, f32),
) -> [(f32, f32); 9] {
    let (sin_h, cos_h) = yaw.sin_cos();
    let mut out = [(0.0, 0.0); 9];
    for (i, (a, b)) in [-1.0f32, 0.0, 1.0]
        .iter()
        .flat_map(|a| [-1.0f32, 0.0, 1.0].map(|b| (*a, b)))
        .enumerate()
    {
        let (lx, ly) = (a * half_len, b * half_wid);
        out[i] = (x + cos_h * lx - sin_h * ly, y + sin_h * lx + cos_h * ly);
    }
    out
}

/// The smallest road-edge clearance over a stand's or building's
/// footprint probes, against whichever section of the course is nearest
/// to each probe (so folded sections count). `None` for every other kind.
/// Negative means the footprint reaches inside the road's clearance band.
pub fn stand_road_clearance_m(path: &CenterlinePath, prop: &Prop) -> Option<f32> {
    if !matches!(prop.kind, PropKind::Grandstand | PropKind::Building) {
        return None;
    }
    let ext = footprint_half_extents(prop)?;
    footprint_samples(prop.x, prop.y, prop.yaw_rad, ext)
        .iter()
        .map(|&(sx, sy)| {
            let (s, lat, _) = nearest_cross_section(path, sx, sy);
            lat.abs() - half_width_on(&s, lat)
        })
        .reduce(f32::min)
}

/// Where a stand or building can stand with its whole footprint clear of
/// the road and the pit lane: its current spot when every probe already
/// is, otherwise pushed in 1 m steps along the outward normal of the road
/// section its deepest probe is nearest to — away from that section —
/// until it is. `None` when [`FOOTPRINT_MAX_PUSH_M`] is not enough.
fn seat_footprint(
    path: &CenterlinePath,
    lane: Option<&CenterlinePath>,
    x: f32,
    y: f32,
    yaw: f32,
    ext: (f32, f32),
    road_clear_m: f32,
) -> Option<(f32, f32)> {
    let (mut x, mut y) = (x, y);
    for _ in 0..=(FOOTPRINT_MAX_PUSH_M as usize) {
        let mut worst: Option<(f32, PathSample)> = None;
        let mut lane_blocked = false;
        for (sx, sy) in footprint_samples(x, y, yaw, ext) {
            let (s, lat, _) = nearest_cross_section(path, sx, sy);
            let gap = lat.abs() - half_width_on(&s, lat) - road_clear_m;
            if gap < 0.0 && worst.is_none_or(|(g, _)| gap < g) {
                worst = Some((gap, s));
            }
            if lane.is_some_and(|l| lane_edge_gap(l, sx, sy) < FOOTPRINT_LANE_CLEAR_M) {
                lane_blocked = true;
            }
        }
        if worst.is_none() && !lane_blocked {
            return Some((x, y));
        }
        // Only the lane in the way: it lies beside the road, so moving
        // away from the nearest road section walks past it.
        let section = worst
            .map(|(_, s)| s)
            .unwrap_or_else(|| nearest_cross_section(path, x, y).0);
        let (sin_h, cos_h) = section.heading_rad.sin_cos();
        let centre_lat = -sin_h * (x - section.pos.0) + cos_h * (y - section.pos.1);
        let (nx, ny) = if centre_lat >= 0.0 {
            (-sin_h, cos_h)
        } else {
            (sin_h, -cos_h)
        };
        x += nx;
        y += ny;
    }
    None
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct GroomReport {
    /// Props pushed clear of the road or pit lane (or squared up with it).
    pub pushed: usize,
    /// Props whose height alone was corrected.
    pub reseated: usize,
    /// Walls/barriers deleted for standing on a straight.
    pub removed: usize,
    /// Wall segments in the scene after grooming.
    pub walls: usize,
    /// The corner walls were re-laid (they didn't already match).
    pub walls_rebuilt: bool,
    /// Distance boards in the scene after grooming, and whether they were
    /// re-laid.
    pub boards: usize,
    pub boards_rebuilt: bool,
    /// Armco segments on straights after grooming, and whether re-laid.
    pub barriers: usize,
    pub barriers_rebuilt: bool,
    /// Belt trees after grooming, and whether re-laid.
    pub trees: usize,
    pub trees_rebuilt: bool,
    /// Prop count before grooming.
    pub total: usize,
    /// The pit lane was regenerated with a different shape.
    pub pit_rebuilt: bool,
    /// Ids of stands/buildings deleted because no spot within
    /// [`FOOTPRINT_MAX_PUSH_M`] of them clears the road and the pit lane.
    pub deleted: Vec<u64>,
}

impl GroomReport {
    pub fn changed(&self) -> bool {
        self.pushed > 0
            || self.reseated > 0
            || self.removed > 0
            || !self.deleted.is_empty()
            || self.walls_rebuilt
            || self.boards_rebuilt
            || self.barriers_rebuilt
            || self.trees_rebuilt
            || self.pit_rebuilt
    }
}

/// Lateral distance from the centerline a prop's center needs on the side
/// `lat` falls on, to keep its footprint off the road.
fn prop_lat_clearance(sample: &PathSample, lat: f32, radius: f32) -> f32 {
    half_width_on(sample, lat) + PROP_CLEARANCE_M + radius
}

/// The road's half width on the side of the centerline `lat` falls on.
fn half_width_on(sample: &PathSample, lat: f32) -> f32 {
    if lat >= 0.0 {
        sample.width_left_m
    } else {
        sample.width_right_m
    }
}

fn side_half_width(sample: &PathSample, side: Side) -> f32 {
    match side {
        Side::Left => sample.width_left_m,
        Side::Right => sample.width_right_m,
    }
}

fn signed(side: Side, lat: f32) -> f32 {
    match side {
        Side::Left => lat,
        Side::Right => -lat,
    }
}

/// The one place a prop gets its height: the road-edge / terrain blend
/// every ground band uses, evaluated at the prop's own footprint. `sample`
/// and `lat` locate the prop against the course; `(x, y)` is where it
/// actually stands.
fn seat_z(terrain: &TerrainHeightfield, sample: &PathSample, lat: f32, x: f32, y: f32) -> f32 {
    let road = offset_point(sample, lat);
    surface_height(Some(terrain), sample, lat, (x, y, road.2))
}

fn owned_by_board_pass(prop: &Prop) -> bool {
    prop.kind == PropKind::Sign && BOARDS.iter().any(|(_, asset, _)| *asset == prop.asset)
}

fn owned_by_tree_pass(prop: &Prop) -> bool {
    prop.kind == PropKind::Tree && is_tree_pass_asset(&prop.asset)
}

fn owned_by_barrier_pass(prop: &Prop) -> bool {
    // `BARRIER_ASSET` is the legacy key the scenes carried before the
    // pass decided anything; it is still recognised so an old scene is
    // adopted rather than doubled.
    prop.kind == PropKind::Barrier
        && (prop.asset == BARRIER_ASSET || barriers::is_barrier_asset(&prop.asset))
}

/// The ground props are seated on: the same field the exporter bakes,
/// pit lane included, over the elevation model when there is one. Built
/// the way `ue_export::bake_all_with_dem` builds it, which is the point —
/// a prop is only in the right place if it is seated on the ground that
/// is drawn under it.
pub fn seating_terrain(
    path: &CenterlinePath,
    scene: &AtsScene,
    dem: Option<&DemFile>,
) -> Option<TerrainHeightfield> {
    let lane = scene
        .pit_lane
        .as_ref()
        .and_then(|pit| CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0));
    let extra: Vec<&CenterlinePath> = lane.iter().collect();
    TerrainHeightfield::from_paths_with_dem(path, &extra, dem)
}

/// Groom the whole scene: the pit lane is rebuilt first (entry, pit road,
/// exit — [`crate::pit::generate_pit_lane`]) so that prop grooming can keep
/// props clear of the *final* lane, then every prop is placed by
/// [`groom_props`].
pub fn groom_scene(track: &TrackFile, scene: &mut AtsScene) -> Option<GroomReport> {
    groom_scene_with(track, scene, None)
}

/// Groom against the circuit's real-world dossier where it has one
/// ([`crate::layout`]): the pit lane [`crate::dress`] laid from it is kept
/// as it is, and the tree belts are planted only inside the woods that are
/// really there.
pub fn groom_scene_with(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: Option<&Layout>,
) -> Option<GroomReport> {
    groom_scene_with_dem(track, scene, layout, None)
}

/// [`groom_scene_with`] over the real land.
///
/// Every stage that seats a prop has to seat it on the ground the client
/// will draw, or the two disagree wherever they differ. Once the exporter
/// drew the elevation model and grooming still used the centerline
/// blanket, everything more than a few tens of metres from the road —
/// the villages, the car parks, the woodland on the slopes — would have
/// been placed at the blanket's height and rendered floating over, or
/// buried in, the real hillside.
pub fn groom_scene_with_dem(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: Option<&Layout>,
    dem: Option<&DemFile>,
) -> Option<GroomReport> {
    let path = CenterlinePath::from_track(track)?;
    // The circuit's own pit lane is a fact, not a shape to regenerate.
    if scene.pit_lane.as_ref().is_some_and(|pit| pit.authored) {
        let mut report = groom_props_with_dem(track, scene, layout, dem)?;
        report.pit_rebuilt = false;
        return Some(report);
    }
    let pit_rebuilt = match crate::pit::generate_pit_lane(&path, scene.pit_lane.as_ref()) {
        Some(pit) => {
            let rebuilt = scene.pit_lane.as_ref() != Some(&pit);
            scene.pit_lane = Some(pit);
            rebuilt
        }
        None => false,
    };

    let mut report = groom_props_with_dem(track, scene, layout, dem)?;
    report.pit_rebuilt = pit_rebuilt;
    Some(report)
}

/// Groom every prop in `scene` against `track`'s geometry. Returns `None`
/// for a degenerate track the centerline sampler rejects.
///
/// Passes run in dependency order — authored props, tire walls, boards,
/// armco, trees — each seeing everything laid before it, so a later pass
/// keeps clear of an earlier one and re-grooming reproduces the same
/// decisions.
pub fn groom_props(track: &TrackFile, scene: &mut AtsScene) -> Option<GroomReport> {
    groom_props_with(track, scene, None)
}

pub fn groom_props_with(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: Option<&Layout>,
) -> Option<GroomReport> {
    groom_props_with_dem(track, scene, layout, None)
}

/// [`groom_props_with`] over the real land; see [`groom_scene_with_dem`].
pub fn groom_props_with_dem(
    track: &TrackFile,
    scene: &mut AtsScene,
    layout: Option<&Layout>,
    dem: Option<&DemFile>,
) -> Option<GroomReport> {
    let path = CenterlinePath::from_track(track)?;
    let terrain = seating_terrain(&path, scene, dem)?;
    let dressed = layout.is_some();

    let mut report = GroomReport {
        total: scene.props.len(),
        ..Default::default()
    };

    // The surfaces are read while the props are mutated; snapshot them.
    let surfaces = scene.surfaces.clone();
    let lane = scene
        .pit_lane
        .as_ref()
        .and_then(|p| CenterlinePath::from_polyline(&p.nodes, p.width_m / 2.0));

    let mut kept = Vec::with_capacity(scene.props.len());
    let mut original_walls: Vec<Prop> = Vec::new();
    let mut original_boards: Vec<Prop> = Vec::new();
    let mut original_barriers: Vec<Prop> = Vec::new();
    let mut original_trees: Vec<Prop> = Vec::new();
    for mut prop in std::mem::take(&mut scene.props) {
        // Generated furniture is owned by its pass: set aside, re-laid
        // below, and adopted back unchanged when the layout still matches.
        if owned_by_board_pass(&prop) {
            original_boards.push(prop);
            continue;
        }
        if owned_by_barrier_pass(&prop) {
            original_barriers.push(prop);
            continue;
        }
        if owned_by_tree_pass(&prop) {
            original_trees.push(prop);
            continue;
        }

        match prop.kind {
            PropKind::TireWall | PropKind::Barrier => {
                // Walls are not groomed one by one: sparse dashes at a
                // uniform offset read as a phantom second circuit. They are
                // collected here and re-laid as continuous runs below.
                original_walls.push(prop);
            }
            // The exporter lays the pit complex from the pit lane's box
            // layout, so an authored pit module is left exactly where it
            // is; a sky prop's z is its altitude and never gets seated.
            PropKind::Pit | PropKind::Sky => kept.push(prop),
            PropKind::Bridge => {
                // A bridge spans the road on purpose: never pushed, its
                // pivot is the road centre, so it takes the road's height
                // at its station (banking included) rather than the ground.
                let (sample, lat, _) = nearest_cross_section(&path, prop.x, prop.y);
                let z = offset_point(&sample, lat).2;
                if (z - prop.z).abs() > MIN_MOVE_M {
                    prop.z = z;
                    report.reseated += 1;
                }
                kept.push(prop);
            }
            PropKind::Fence => {
                // Laid like a wall segment at its own station — the runoff
                // edge or the verge — but behind it, where the spectator
                // fence runs.
                let (sample, lat, along) = nearest_cross_section(&path, prop.x, prop.y);
                let side = if lat >= 0.0 { Side::Left } else { Side::Right };
                let station = path.sample_at(sample.station_m + along);
                let beyond = wall_offset(&path, &surfaces, &station, side) + FENCE_BEHIND_WALL_M;
                let target_lat = signed(side, side_half_width(&station, side) + beyond);
                let pos = offset_point(&station, target_lat);
                let z = seat_z(&terrain, &station, target_lat, pos.0, pos.1);
                if (pos.0 - prop.x).hypot(pos.1 - prop.y) > MIN_MOVE_M
                    || yaw_distance(station.heading_rad, prop.yaw_rad) > MIN_YAW_RAD
                {
                    (prop.x, prop.y, prop.yaw_rad) = (pos.0, pos.1, station.heading_rad);
                    prop.z = z;
                    report.pushed += 1;
                } else if (z - prop.z).abs() > MIN_MOVE_M {
                    prop.z = z;
                    report.reseated += 1;
                }
                kept.push(prop);
            }
            _ if (dressed && crate::dress::dressed_prop(&prop))
                || crate::dress::always_dress_owned(&prop) =>
            {
                // The dossier put this stand, building or landmark where
                // the real one is. Seat it on the ground and leave it
                // alone: a push would trade a fact for a guess.
                let (sample, lat, _) = nearest_cross_section(&path, prop.x, prop.y);
                let z = seat_z(&terrain, &sample, lat, prop.x, prop.y);
                if (z - prop.z).abs() > MIN_MOVE_M {
                    prop.z = z;
                    report.reseated += 1;
                }
                kept.push(prop);
            }
            _ => {
                let radius = prop_radius_of(&prop);
                let (mut x, mut y, mut yaw) = (prop.x, prop.y, prop.yaw_rad);

                if let Some(ext) = footprint_half_extents(&prop) {
                    // The whole slab has to clear the road — a 78 m stand
                    // whose centre is 20 m out still has a corner on the
                    // asphalt. Where the circuit folds, another section may
                    // be the one it fouls. A building or grandstand near
                    // the lane is pit furniture and is squared up with the
                    // lane first, since the yaw decides where its corners
                    // land; the two interact (a push can carry a stand
                    // into alignment range, or to a spot where the lane
                    // heads a little differently), so they are iterated to
                    // a fixed point here rather than left for the next run.
                    let mut seated = false;
                    for _ in 0..4 {
                        let yaw_now = match &lane {
                            Some(lane) => {
                                let (ls, llat, _) = nearest_cross_section(lane, x, y);
                                if llat.abs() < PIT_BOX_ALIGN_RANGE_M {
                                    ls.heading_rad
                                } else {
                                    yaw
                                }
                            }
                            None => yaw,
                        };
                        let Some((nx, ny)) = seat_footprint(
                            &path,
                            lane.as_ref(),
                            x,
                            y,
                            yaw_now,
                            ext,
                            footprint_road_clear_m(prop.kind),
                        ) else {
                            break;
                        };
                        let settled =
                            (nx, ny) == (x, y) && yaw_distance(yaw_now, yaw) < MIN_YAW_RAD;
                        (x, y, yaw) = (nx, ny, yaw_now);
                        if settled {
                            seated = true;
                            break;
                        }
                    }
                    if !seated {
                        report.deleted.push(prop.id);
                        continue;
                    }
                } else if radius >= 1.0 {
                    // Road clearance for the small kinds: the enrichment
                    // ringed props at a fixed offset from *their* section,
                    // and where the circuit folds that lands them on another
                    // one. Small furniture (cones, lights) may legitimately
                    // hug the road and is exempt.
                    //
                    // The road check and the pit-lane check each move the
                    // point clear of *their own* line, but a point cleared
                    // of the road can still land inside the lane's margin
                    // (or the reverse) -- and where a prop starts far from
                    // both (a floodlight mapped on the track itself), the
                    // nearest road section after the first move is not the
                    // one the original position was checked against, so
                    // one pass of each undersells the true clearance
                    // needed. Iterated to a fixed point for the same reason
                    // the footprint push above is: so a mis-seated prop
                    // (any distance out) settles in one groom, not a slow
                    // walk across several runs.
                    for _ in 0..4 {
                        let (before_x, before_y) = (x, y);
                        let (sample, lat, _) = nearest_cross_section(&path, x, y);
                        let needed = prop_lat_clearance(&sample, lat, radius);
                        if lat.abs() < needed {
                            let p = offset_point(&sample, needed.copysign(lat));
                            (x, y) = (p.0, p.1);
                        }

                        // Pit-lane clearance: of the two sides of the lane, take
                        // the one that also keeps the prop off the road — a prop
                        // squeezed into the road/lane gap belongs behind the
                        // lane, not between.
                        if let Some(lane) = &lane {
                            let (ls, llat, _) = nearest_cross_section(lane, x, y);
                            let needed_l = ls.width_left_m + PROP_CLEARANCE_M + radius;
                            if llat.abs() < needed_l {
                                let candidate = |lat_l: f32| {
                                    let p = offset_point(&ls, lat_l);
                                    let (rs, rlat, _) = nearest_cross_section(&path, p.0, p.1);
                                    let clear = rlat.abs() - prop_lat_clearance(&rs, rlat, radius);
                                    (p, clear)
                                };
                                let (a, b) = (candidate(needed_l), candidate(-needed_l));
                                let p = if a.1 >= b.1 { a.0 } else { b.0 };
                                (x, y) = (p.0, p.1);
                            }
                        }
                        if (x - before_x).hypot(y - before_y) <= MIN_MOVE_M {
                            break;
                        }
                    }
                }

                // Seat on the ground that is actually there: the same
                // road-edge / terrain blend every ground band uses.
                let (fs, flat, _) = nearest_cross_section(&path, x, y);
                let z = seat_z(&terrain, &fs, flat, x, y);

                if (x - prop.x).hypot(y - prop.y) > MIN_MOVE_M
                    || yaw_distance(yaw, prop.yaw_rad) > MIN_YAW_RAD
                {
                    (prop.x, prop.y, prop.yaw_rad) = (x, y, yaw);
                    prop.z = z;
                    report.pushed += 1;
                } else if (z - prop.z).abs() > MIN_MOVE_M {
                    prop.z = z;
                    report.reseated += 1;
                }
                kept.push(prop);
            }
        }
    }

    // One pass decides the whole barrier line: see `lay_all_barriers`.
    let (new_walls, new_rails) =
        lay_all_barriers(&path, &terrain, &surfaces, lane.as_ref(), layout, &kept);
    report.removed = 0;
    report.walls = new_walls.len();
    let mut props = kept;
    props.extend(adopt(
        scene,
        original_walls,
        new_walls,
        &mut report.walls_rebuilt,
    ));

    let corners = braking_corners(&path);
    let boards = lay_distance_boards(&path, &terrain, &corners, lane.as_ref(), &props);
    report.boards = boards.len();
    props.extend(adopt(
        scene,
        original_boards,
        boards,
        &mut report.boards_rebuilt,
    ));

    report.barriers = new_rails.len();
    props.extend(adopt(
        scene,
        original_barriers,
        new_rails,
        &mut report.barriers_rebuilt,
    ));

    // Boards go on the barriers, so they wait until every wall and armco
    // segment is in its final place.
    report.pushed += snap_boards_to_barriers(&path, &terrain, &mut props);

    let mut trees = lay_tree_belts(
        &path,
        &terrain,
        &surfaces,
        lane.as_ref(),
        &props,
        tree_density(track),
        layout.map(|l| l.woods.as_slice()).unwrap_or(&[]),
    );
    trees.extend(lay_ground_cover(
        &path,
        &terrain,
        &surfaces,
        lane.as_ref(),
        tree_density(track),
    ));
    report.trees = trees.len();
    props.extend(adopt(
        scene,
        original_trees,
        trees,
        &mut report.trees_rebuilt,
    ));

    scene.props = props;
    Some(report)
}

/// Snap every `board` prop onto the nearest barrier or tire wall within
/// [`BOARD_SNAP_RANGE_M`]: same station as the board, the barrier's
/// lateral plus [`BOARD_BEHIND_BARRIER_M`] further out, facing along the
/// course (the importer turns the face to the road). A board with no
/// barrier in reach keeps the place the generic pass gave it. Returns how
/// many boards moved.
fn snap_boards_to_barriers(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    props: &mut [Prop],
) -> usize {
    let rails: Vec<(f32, f32)> = props
        .iter()
        .filter(|p| matches!(p.kind, PropKind::Barrier | PropKind::TireWall))
        .map(|p| (p.x, p.y))
        .collect();
    let mut moved = 0;
    // A board the dressing pass laid — a corner's name — stays where that
    // pass put it. Snapped like a hoarding it lands on the nearest barrier
    // module, and where two barrier kinds meet the nearest module is a
    // different one each time the line is re-laid, so the board walked a
    // tenth of a metre back and forth on every groom.
    for board in props
        .iter_mut()
        .filter(|p| p.kind == PropKind::Board && !crate::dress::always_dress_owned(p))
    {
        let Some((rx, ry)) = rails
            .iter()
            .copied()
            .filter(|(rx, ry)| (rx - board.x).hypot(ry - board.y) <= BOARD_SNAP_RANGE_M)
            .min_by(|a, b| {
                let da = (a.0 - board.x).hypot(a.1 - board.y);
                let db = (b.0 - board.x).hypot(b.1 - board.y);
                da.total_cmp(&db)
            })
        else {
            continue;
        };
        let (_, rail_lat, _) = nearest_cross_section(path, rx, ry);
        let (sample, _, along) = nearest_cross_section(path, board.x, board.y);
        let station = path.sample_at(sample.station_m + along);
        let lat = rail_lat + BOARD_BEHIND_BARRIER_M.copysign(rail_lat);
        let pos = offset_point(&station, lat);
        let z = seat_z(terrain, &station, lat, pos.0, pos.1);
        if (pos.0 - board.x).hypot(pos.1 - board.y) > MIN_MOVE_M
            || yaw_distance(station.heading_rad, board.yaw_rad) > MIN_YAW_RAD
            || (z - board.z).abs() > MIN_MOVE_M
        {
            (board.x, board.y, board.z, board.yaw_rad) = (pos.0, pos.1, z, station.heading_rad);
            moved += 1;
        }
    }
    moved
}

/// Keep `original` (ids and all) when `fresh` reproduces it within the
/// placement tolerances — the signal that a groomed scene needs no
/// rewrite — otherwise hand out ids for `fresh` and flag the rebuild.
fn adopt(
    scene: &mut AtsScene,
    original: Vec<Prop>,
    fresh: Vec<Prop>,
    rebuilt: &mut bool,
) -> Vec<Prop> {
    if props_match(&original, &fresh) {
        original
    } else {
        *rebuilt = true;
        fresh
            .into_iter()
            .map(|prop| Prop {
                id: scene.alloc_id(),
                ..prop
            })
            .collect()
    }
}

/// Whether freshly laid props already match the existing ones, ids aside.
fn props_match(old: &[Prop], new: &[Prop]) -> bool {
    if old.len() != new.len() {
        return false;
    }
    let key = |p: &Prop| (p.x, p.y);
    let mut old_sorted: Vec<&Prop> = old.iter().collect();
    let mut new_sorted: Vec<&Prop> = new.iter().collect();
    old_sorted.sort_by(|a, b| key(a).partial_cmp(&key(b)).expect("finite prop positions"));
    new_sorted.sort_by(|a, b| key(a).partial_cmp(&key(b)).expect("finite prop positions"));
    old_sorted.iter().zip(&new_sorted).all(|(a, b)| {
        (a.x - b.x).abs() < MIN_MOVE_M
            && (a.y - b.y).abs() < MIN_MOVE_M
            && (a.z - b.z).abs() < MIN_MOVE_M
            && yaw_distance(a.yaw_rad, b.yaw_rad) < MIN_YAW_RAD
            && a.scale == b.scale
            && a.kind == b.kind
            && a.asset == b.asset
            && a.text == b.text
    })
}

// ---- Corner analysis and distance boards ---------------------------------

/// A stretch of the course bending at or beyond [`CORNER_KAPPA`], after
/// nearby runs have been merged into one complex. `end_m` may exceed the
/// lap length when the run wraps through start/finish.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CornerRun {
    pub start_m: f32,
    pub end_m: f32,
    /// Signed curvature at the entry station (positive turns left).
    pub entry_kappa: f32,
    /// Largest unsigned curvature anywhere in the run.
    pub peak_kappa: f32,
}

impl CornerRun {
    fn covers(&self, path: &CenterlinePath, station_m: f32) -> bool {
        let len = self.end_m - self.start_m;
        let along = if path.is_closed() {
            (station_m - self.start_m).rem_euclid(path.total_length_m())
        } else {
            station_m - self.start_m
        };
        (0.0..=len).contains(&along)
    }
}

/// A corner the driver has to brake for, with the stations of its boards.
#[derive(Debug, Clone, PartialEq)]
pub struct BrakingCorner {
    pub run: CornerRun,
    /// Cornering speed the model allows at the run's tightest point, m/s.
    pub corner_speed_mps: f32,
    /// Board asset key and station, for every board that could be placed.
    pub boards: Vec<(&'static str, f32)>,
}

impl BrakingCorner {
    pub fn entry_m(&self) -> f32 {
        self.run.start_m
    }

    /// The side of the road the boards stand on: the outside of the bend.
    pub fn outside(&self) -> Side {
        if self.run.entry_kappa > 0.0 {
            Side::Right
        } else {
            Side::Left
        }
    }
}

/// Every corner run on the course, in station order: a sweep at
/// [`CORNER_SWEEP_M`] over the curvature, contiguous over-threshold
/// stations grouped, runs within [`CORNER_MERGE_GAP_M`] merged (wrapping
/// through start/finish on closed loops).
pub fn corner_runs(path: &CenterlinePath) -> Vec<CornerRun> {
    let total = path.total_length_m();
    let steps = (total / CORNER_SWEEP_M).floor().max(1.0) as usize;
    let kappa: Vec<f32> = (0..steps)
        .map(|i| curvature_at(path, i as f32 * CORNER_SWEEP_M))
        .collect();

    let mut runs: Vec<CornerRun> = Vec::new();
    let mut i = 0;
    while i < steps {
        if kappa[i].abs() < CORNER_KAPPA {
            i += 1;
            continue;
        }
        let start = i;
        let mut peak = 0.0f32;
        while i < steps && kappa[i].abs() >= CORNER_KAPPA {
            peak = peak.max(kappa[i].abs());
            i += 1;
        }
        runs.push(CornerRun {
            start_m: start as f32 * CORNER_SWEEP_M,
            end_m: (i - 1) as f32 * CORNER_SWEEP_M,
            entry_kappa: kappa[start],
            peak_kappa: peak,
        });
    }

    // Fuse neighbours into complexes.
    let mut merged: Vec<CornerRun> = Vec::new();
    for run in runs {
        match merged.last_mut() {
            Some(prev) if run.start_m - prev.end_m < CORNER_MERGE_GAP_M => {
                prev.end_m = run.end_m;
                prev.peak_kappa = prev.peak_kappa.max(run.peak_kappa);
            }
            _ => merged.push(run),
        }
    }
    // …and across start/finish: the last run continues into the first.
    if path.is_closed() && merged.len() >= 2 {
        let first = merged[0];
        let last = merged.last().unwrap();
        if first.start_m + total - last.end_m < CORNER_MERGE_GAP_M {
            let last = merged.last_mut().unwrap();
            last.end_m = first.end_m + total;
            last.peak_kappa = last.peak_kappa.max(first.peak_kappa);
            merged.remove(0);
        }
    }
    merged
}

/// The corners that get distance boards: a clean [`BOARD_APPROACH_M`]
/// before the entry and a tightest point slower than
/// [`BOARD_MAX_CORNER_SPEED_MPS`] under the `v = sqrt(a_lat / kappa)`
/// model. Board stations that would land inside another corner run are
/// left out.
pub fn braking_corners(path: &CenterlinePath) -> Vec<BrakingCorner> {
    let runs = corner_runs(path);
    let total = path.total_length_m();
    let station = |s: f32| {
        if path.is_closed() {
            Some(s.rem_euclid(total))
        } else {
            (s >= 0.0).then_some(s)
        }
    };

    runs.iter()
        .filter_map(|run| {
            let approach_steps = (BOARD_APPROACH_M / CORNER_SWEEP_M) as i32;
            let clean_approach = (1..=approach_steps).all(|i| {
                match station(run.start_m - i as f32 * CORNER_SWEEP_M) {
                    Some(s) => curvature_at(path, s).abs() < CORNER_KAPPA,
                    None => false,
                }
            });
            if !clean_approach {
                return None;
            }
            let corner_speed_mps = (BOARD_A_LAT / run.peak_kappa).sqrt();
            if corner_speed_mps > BOARD_MAX_CORNER_SPEED_MPS {
                return None;
            }
            let boards = BOARDS
                .iter()
                .filter_map(|(distance, asset, _)| {
                    let s = station(run.start_m - distance)?;
                    (!runs.iter().any(|other| other.covers(path, s))).then_some((*asset, s))
                })
                .collect();
            Some(BrakingCorner {
                run: *run,
                corner_speed_mps,
                boards,
            })
        })
        .collect()
}

/// Lay the 200/100/50 boards of every braking corner on the outside of the
/// bend, [`BOARD_LATERAL_M`] beyond the road edge, facing along the course
/// (the board's local +X is the travel direction; the Unreal side puts the
/// readable face on −X so an approaching driver reads it). A board that
/// would overlap the pit lane or any prop already placed is skipped.
fn lay_distance_boards(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    corners: &[BrakingCorner],
    lane: Option<&CenterlinePath>,
    others: &[Prop],
) -> Vec<Prop> {
    let radius = prop_radius(PropKind::Sign);
    let mut boards = Vec::new();
    for corner in corners {
        let side = corner.outside();
        for (asset, station) in &corner.boards {
            let sample = path.sample_at(*station);
            let lat = signed(side, side_half_width(&sample, side) + BOARD_LATERAL_M);
            let pos = offset_point(&sample, lat);
            if lane
                .is_some_and(|lane| lane_edge_gap(lane, pos.0, pos.1) < PROP_CLEARANCE_M + radius)
            {
                continue;
            }
            if others
                .iter()
                .chain(boards.iter())
                .any(|p| footprint_gap(p, pos.0, pos.1) < radius + BOARD_PROP_CLEAR_M)
            {
                continue;
            }
            let text = BOARDS
                .iter()
                .find(|(_, a, _)| a == asset)
                .map(|(_, _, text)| text.to_string());
            boards.push(Prop {
                id: 0,
                kind: PropKind::Sign,
                asset: asset.to_string(),
                x: pos.0,
                y: pos.1,
                z: seat_z(terrain, &sample, lat, pos.0, pos.1),
                yaw_rad: sample.heading_rad,
                scale: 1.0,
                text,
                length_m: None,
            });
        }
    }
    boards
}

// ---- Armco on straights ---------------------------------------------------

/// The station span of the pit lane along the course and which side it
/// lies on, so the barrier pass can leave that whole stretch of that side
/// alone.
struct LaneSpan {
    side: Side,
    start_m: f32,
    len_m: f32,
}

impl LaneSpan {
    fn of(path: &CenterlinePath, lane: &CenterlinePath) -> Self {
        let samples = lane.samples();
        let first = samples.first().expect("lane has samples");
        let last = samples.last().expect("lane has samples");
        let mid = samples[samples.len() / 2];
        let (s0, _, a0) = nearest_cross_section(path, first.pos.0, first.pos.1);
        let (s1, _, a1) = nearest_cross_section(path, last.pos.0, last.pos.1);
        let (_, mid_lat, _) = nearest_cross_section(path, mid.pos.0, mid.pos.1);
        let start_m = s0.station_m + a0;
        let end_m = s1.station_m + a1;
        let len_m = if path.is_closed() {
            (end_m - start_m).rem_euclid(path.total_length_m())
        } else {
            (end_m - start_m).max(0.0)
        };
        LaneSpan {
            side: if mid_lat >= 0.0 {
                Side::Left
            } else {
                Side::Right
            },
            start_m,
            len_m,
        }
    }

    fn covers(&self, path: &CenterlinePath, side: Side, station_m: f32) -> bool {
        if side != self.side {
            return false;
        }
        let from = self.start_m - BARRIER_PIT_MARGIN_M;
        let along = if path.is_closed() {
            (station_m - from).rem_euclid(path.total_length_m())
        } else {
            station_m - from
        };
        (0.0..=self.len_m + 2.0 * BARRIER_PIT_MARGIN_M).contains(&along)
    }
}

// ---- The barrier line -----------------------------------------------------

/// Closest a corner's barrier stands to the road edge. Below the Tecpro
/// decision's `SHORT_RUNOFF_M` on purpose, so a tight corner with nothing
/// authored still reads as short of room and gets an absorbing barrier.
const CORNER_BARRIER_MIN_M: f32 = 14.0;
/// Closest a straight's rail stands to the road edge: where the old armco
/// pass put it, which the AI was always comfortable beside.
const STRAIGHT_BARRIER_MIN_M: f32 = 7.0;

/// Lay the whole barrier line, deciding what each 4 m cell gets.
///
/// This replaced two passes that between them never made a decision. One
/// hard-coded `armco_generic` along the straights; the other re-laid
/// corner walls by copying whichever asset the old procedural enrichment
/// had left within 40 m, which meant a corner with no legacy prop near it
/// got no barrier at all and every corner that did got the same tyre
/// wall. Walking every cell instead makes the coverage complete by
/// construction, and [`crate::barriers::decide`] gives each cell a kind
/// on the evidence: the circuit's own mapped barriers where the dossier
/// has them, and the shape of the road and its run-off everywhere else.
///
/// Returns the run split by prop kind, because the two are adopted
/// separately: tyres and Tecpro are laid as [`PropKind::TireWall`] (the
/// exporter bakes either as an absorbing wall) and everything else as
/// [`PropKind::Barrier`].
#[allow(clippy::too_many_arguments)]
fn lay_all_barriers(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    surfaces: &[crate::ats::Surface],
    lane: Option<&CenterlinePath>,
    layout: Option<&Layout>,
    others: &[Prop],
) -> (Vec<Prop>, Vec<Prop>) {
    let lane_span = lane.map(|l| LaneSpan::of(path, l));
    let cells = (path.total_length_m() / BARRIER_CELL_M).floor() as i64;
    let mapped: Vec<(&crate::layout::Line, barriers::MappedKind)> = layout
        .map(|l| {
            l.barriers
                .iter()
                .filter_map(|line| {
                    barriers::MappedKind::from_dossier(&line.kind).map(|kind| (line, kind))
                })
                .collect()
        })
        .unwrap_or_default();

    // Whether a barrier piece may stand at a point. Every piece is tested,
    // not just the cell it belongs to: a Tecpro block sits up to a metre
    // and a half from its cell's centre and an end cap three metres past
    // it, and at Albert Park's pit entry exactly those pieces — caps that
    // closed a run *because* the next cell was the pit lane's — stood in
    // the mouth of the lane where cars run wide at the last corner.
    let placeable = |x: f32, y: f32, road_z: f32| -> bool {
        if lane.is_some_and(|lane| lane_edge_gap(lane, x, y) < BARRIER_CLEAR_M) {
            return false;
        }
        // Clear of every part of the course, not only the one it was laid
        // for: see below.
        let (near_sample, near_lat, _) = nearest_cross_section(path, x, y);
        if near_lat.abs() - half_width_on(&near_sample, near_lat) < STRAIGHT_BARRIER_MIN_M - 1.0 {
            return false;
        }
        let at_underpass = terrain.wall_relation(x, y).is_some_and(|(past_wall, lower_z)| {
            (road_z - lower_z).abs() <= terrain::OVERHEAD_M || past_wall < BARRIER_CLEAR_M
        });
        !at_underpass && !barrier_blocked(others, x, y)
    };

    // Decide every cell first, then lay the modules, so a run knows where
    // it begins and ends and can be capped.
    let mut decided: BTreeMap<(bool, i64), (barriers::BarrierKind, PathSample, f32)> =
        BTreeMap::new();
    for idx in 0..cells {
        let center = (idx as f32 + 0.5) * BARRIER_CELL_M;
        let sample = path.sample_at(center);
        for side in Side::ALL {
            if lane_span
                .as_ref()
                .is_some_and(|span| span.covers(path, side, center))
            {
                // The pit side of the pit straight: the bake generates the
                // pit wall from the lane itself.
                continue;
            }
            // Where the barrier stands. Past the authored run-off as ever,
            // but never closer to the road than a car can reasonably get.
            //
            // The old passes laid a wall at a corner only where the 2026-09
            // enrichment had left a prop, so most corners had nothing to
            // hit and nobody noticed that `wall_offset` falls back to the
            // verge — 6 m — wherever a scene authors no run-off. Laying the
            // line at every corner made that distance real: the AI runs
            // wide on the outside and cuts the apex on the inside, and at
            // 6 m it found a barrier both ways. The all-circuit AI survey
            // went from 2 100 to 7 500 car-seconds off the road, and at
            // Zandvoort from twenty seconds to twenty minutes, most of it
            // pinned against a rail. Real circuits give a corner run-off
            // on both sides; so does this.
            let radius = tightest_radius(path, center);
            let authored = wall_offset(path, surfaces, &sample, side);
            let offset = if radius < barriers::STRAIGHT_RADIUS_M {
                authored.max(CORNER_BARRIER_MIN_M)
            } else {
                authored.max(STRAIGHT_BARRIER_MIN_M)
            };
            let lat = signed(side, side_half_width(&sample, side) + offset);
            let pos = offset_point(&sample, lat);
            // Where the circuit folds back near itself, a barrier laid out
            // from one section can land on another. The old passes never
            // met this because they barely laid anything at a corner; once
            // every cell had a barrier, pushed out to leave run-off, street
            // circuits grew rails across the neighbouring stretch of road.
            // `placeable` makes the same test the tree belts always have.
            if !placeable(pos.0, pos.1, sample.pos.2) {
                continue;
            }

            // Positive curvature is a left-hand bend, so the outside of
            // it is the right-hand side and the other way about.
            let bend = signed_curvature(path, center);
            let outside_of_bend = match side {
                Side::Left => bend < 0.0,
                Side::Right => bend > 0.0,
            };
            let cell = barriers::Cell {
                corner_radius_m: radius,
                runoff_m: offset,
                spectators_m: spectator_gap(others, pos.0, pos.1),
                mapped: nearest_mapped(&mapped, pos.0, pos.1),
                beside_pit_lane: false,
                outside_of_bend,
            };
            decided.insert(
                (side == Side::Left, idx),
                (barriers::decide(&cell), sample, lat),
            );
        }
    }

    let mut walls = Vec::new();
    let mut rails = Vec::new();
    for (&(left, idx), &(kind, _, lat)) in &decided {
        let previous = decided.get(&(left, idx - 1)).map(|d| d.0);
        let next = decided.get(&(left, idx + 1)).map(|d| d.0);
        let (prop_kind, asset) = kind.asset();
        let center = (idx as f32 + 0.5) * BARRIER_CELL_M;

        // A module shorter than the cell is repeated to fill it, so a
        // Tecpro run is continuous rather than a dashed line of blocks.
        let per_cell = (BARRIER_CELL_M / kind.module_m()).round().max(1.0) as i32;
        for step in 0..per_cell {
            let along = center - BARRIER_CELL_M * 0.5 + kind.module_m() * (step as f32 + 0.5);
            let at = path.sample_at(along);
            let here = offset_point(&at, lat);
            if !placeable(here.0, here.1, at.pos.2) {
                continue;
            }
            let prop = Prop {
                id: 0,
                kind: prop_kind,
                asset: asset.to_string(),
                x: here.0,
                y: here.1,
                z: seat_z(terrain, &at, lat, here.0, here.1),
                yaw_rad: at.heading_rad,
                scale: WALL_SCALE,
                text: None,
                length_m: None,
            };
            match prop_kind {
                PropKind::TireWall => walls.push(prop),
                _ => rails.push(prop),
            }
        }

        // Close the ends of the run. A run ends where the neighbouring
        // cell is missing (the pit lane, an underpass, a building) or
        // where the kind changes, because two kinds butted together need
        // the terminal that belongs to each.
        if let Some((cap_kind, cap_asset)) = kind.end_cap() {
            for (neighbour, direction) in [(previous, -1.0f32), (next, 1.0f32)] {
                if neighbour == Some(kind) {
                    continue;
                }
                let along = center + direction * (BARRIER_CELL_M * 0.5 + 1.0);
                let at = path.sample_at(along);
                let here = offset_point(&at, lat);
                if !placeable(here.0, here.1, at.pos.2) {
                    continue;
                }
                let prop = Prop {
                    id: 0,
                    kind: cap_kind,
                    asset: cap_asset.to_string(),
                    x: here.0,
                    y: here.1,
                    z: seat_z(terrain, &at, lat, here.0, here.1),
                    yaw_rad: at.heading_rad,
                    scale: WALL_SCALE,
                    text: None,
                    length_m: None,
                };
                match cap_kind {
                    PropKind::TireWall => walls.push(prop),
                    _ => rails.push(prop),
                }
            }
        }
    }
    (walls, rails)
}

/// Something already standing where a barrier would go.
fn barrier_blocked(others: &[Prop], x: f32, y: f32) -> bool {
    others.iter().any(|p| {
        let clear = match p.kind {
            // A grandstand stands *behind* the barrier that separates it
            // from the road — that is what the barrier is for — so only a
            // rail that would land inside the seating is dropped.
            PropKind::Grandstand => 0.5,
            PropKind::Building | PropKind::Pit | PropKind::Attraction => BARRIER_CLEAR_M,
            // A board stands on the barrier line by design, and a
            // bridge's footings are off the verge either side.
            PropKind::Board | PropKind::Bridge => return false,
            _ => BOARD_PROP_CLEAR_M,
        };
        footprint_gap(p, x, y) < clear
    })
}

/// How close the nearest thing with people in or around it is.
fn spectator_gap(others: &[Prop], x: f32, y: f32) -> f32 {
    others
        .iter()
        .filter(|p| {
            matches!(
                p.kind,
                PropKind::Grandstand | PropKind::Building | PropKind::Attraction
            )
        })
        .map(|p| footprint_gap(p, x, y))
        .fold(f32::MAX, f32::min)
}

/// The nearest mapped barrier run to a point, and what the map calls it.
fn nearest_mapped(
    mapped: &[(&crate::layout::Line, barriers::MappedKind)],
    x: f32,
    y: f32,
) -> Option<(f32, barriers::MappedKind)> {
    mapped
        .iter()
        .map(|(line, kind)| (line.distance_to(x, y), *kind))
        .min_by(|a, b| a.0.total_cmp(&b.0))
}

/// Which way the course bends around a station, averaged over the corner
/// window so that one noisy sample cannot flip a whole run of cells from
/// one side of the road to the other. Positive is a left-hand bend.
fn signed_curvature(path: &CenterlinePath, station_m: f32) -> f32 {
    let steps = (2.0 * CORNER_WINDOW_M / 10.0) as i32;
    let sum: f32 = (0..=steps)
        .map(|i| {
            let s = station_m - CORNER_WINDOW_M + i as f32 * 10.0;
            curvature_at(path, s)
        })
        .sum();
    sum / (steps + 1) as f32
}

/// The tightest radius the course reaches within [`CORNER_WINDOW_M`] of a
/// station: how hard the corner here is, in the terms the barrier
/// decision is stated in. `f32::MAX` on a straight.
fn tightest_radius(path: &CenterlinePath, station_m: f32) -> f32 {
    let steps = (2.0 * CORNER_WINDOW_M / 10.0) as i32;
    (0..=steps)
        .map(|i| {
            let s = station_m - CORNER_WINDOW_M + i as f32 * 10.0;
            let k = curvature_at(path, s).abs();
            if k < 1e-6 {
                f32::MAX
            } else {
                1.0 / k
            }
        })
        .fold(f32::MAX, f32::min)
}

/// Grass and wildflower clumps along the verge.
///
/// The ground a driver spends the lap looking at was a flat colour with a
/// noise pattern over it and nothing standing on it at all — no scatter,
/// no foliage, nothing. These are three crossed masked cards each, six
/// triangles, laid from the verge out to 40 m on the same deterministic
/// hash the belts use. They are planted on grass only: a clump on the
/// gravel or the asphalt run-off would be worse than none.
fn lay_ground_cover(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    surfaces: &[crate::ats::Surface],
    lane: Option<&CenterlinePath>,
    density: f32,
) -> Vec<Prop> {
    if density <= 0.0 {
        return Vec::new();
    }
    let cells = (path.total_length_m() / TREE_CELL_M).floor() as i64;
    let mut out = Vec::new();
    for idx in 0..cells {
        for side in Side::ALL {
            let side_key = match side {
                Side::Left => 0u64,
                Side::Right => 1,
            };
            let count = ((SCATTER_PER_CELL as f32) * density).round() as u32;
            for k in 0..count {
                let seed = [idx as u64, side_key, k as u64, 7];
                let station = idx as f32 * TREE_CELL_M + hash01(&seed, 1) * TREE_CELL_M;
                let sample = path.sample_at(station);
                let beyond = SCATTER_NEAR_M + hash01(&seed, 2) * (SCATTER_FAR_M - SCATTER_NEAR_M);

                // Prepared run-off is asphalt, gravel or astroturf; grass
                // is what is left, and grass is what this is for.
                let on_runoff = surfaces
                    .iter()
                    .filter(|s| s.side == side && is_prepared_runoff(s.kind))
                    .filter_map(|s| {
                        span_progress(path, s.start_m, s.end_m, station)
                            .map(|t| s.inner_m + s.width_at(t))
                    })
                    .any(|outer| beyond < outer);
                if on_runoff {
                    continue;
                }

                let lat = signed(side, side_half_width(&sample, side) + beyond);
                let pos = offset_point(&sample, lat);
                let (ns, nlat, _) = nearest_cross_section(path, pos.0, pos.1);
                if nlat.abs() < half_width_on(&ns, nlat) + SCATTER_NEAR_M - 0.5 {
                    continue;
                }
                if lane.is_some_and(|lane| lane_edge_gap(lane, pos.0, pos.1) < SCATTER_NEAR_M) {
                    continue;
                }
                let asset =
                    SCATTER[(hash01(&seed, 5) * SCATTER.len() as f32) as usize % SCATTER.len()];
                out.push(Prop {
                    id: 0,
                    kind: PropKind::Tree,
                    asset: asset.to_string(),
                    x: pos.0,
                    y: pos.1,
                    z: seat_z(terrain, &ns, nlat, pos.0, pos.1),
                    yaw_rad: hash01(&seed, 3) * TAU,
                    scale: 0.8 + hash01(&seed, 4) * 0.7,
                    text: None,
                    length_m: None,
                });
            }
        }
    }
    out
}

// ---- Trees ----------------------------------------------------------------

/// Belt density multiplier from the track's environment: desert circuits
/// get half the trees.
fn tree_density(track: &TrackFile) -> f32 {
    let desert = track
        .metadata
        .as_ref()
        .and_then(|m| m.environment_type.as_deref())
        .is_some_and(|env| env.trim().eq_ignore_ascii_case("desert"));
    if desert {
        0.5
    } else {
        1.0
    }
}

/// Plant tree belts on both sides of the course.
///
/// Every [`TREE_CELL_M`] cell and side draws its tree count from a hash
/// of the cell (some cells stay empty), and every tree hashes its own
/// station within the cell, its depth into the belt, scale and yaw. A tree
/// is dropped when it lands on or too near the road (also where the
/// circuit folds back on itself), the pit lane, an authored runoff patch,
/// a building or grandstand, or any other prop — including trees planted
/// before it, in cell order.
fn lay_tree_belts(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    surfaces: &[crate::ats::Surface],
    lane: Option<&CenterlinePath>,
    others: &[Prop],
    density: f32,
    woods: &[Wood],
) -> Vec<Prop> {
    const GRID_M: f32 = TREE_PROP_CLEAR_M;
    let grid_key = |x: f32, y: f32| ((x / GRID_M).floor() as i64, (y / GRID_M).floor() as i64);

    let cells = (path.total_length_m() / TREE_CELL_M).floor() as i64;
    let mut trees: Vec<Prop> = Vec::new();
    let mut planted: BTreeMap<(i64, i64), Vec<(f32, f32)>> = BTreeMap::new();

    for idx in 0..cells {
        for side in Side::ALL {
            let side_key = match side {
                Side::Left => 0,
                Side::Right => 1,
            };
            let cell = [idx as u64, side_key];
            let roll = hash01(&cell, 0);
            if roll < TREE_EMPTY_CELL_SHARE {
                continue;
            }
            let span = (TREE_MAX_PER_CELL - TREE_MIN_PER_CELL + 1) as f32;
            let spread = (roll - TREE_EMPTY_CELL_SHARE) / (1.0 - TREE_EMPTY_CELL_SHARE);
            let count = TREE_MIN_PER_CELL + ((spread * span) as u32).min(span as u32 - 1);
            let count =
                ((count as f32 * density).round() as u32).max(if density > 0.0 { 1 } else { 0 });

            for k in 0..count {
                let tree = [idx as u64, side_key, k as u64];
                let station = idx as f32 * TREE_CELL_M + hash01(&tree, 1) * TREE_CELL_M;
                let sample = path.sample_at(station);
                let beyond =
                    TREE_BELT_NEAR_M + hash01(&tree, 2) * (TREE_BELT_FAR_M - TREE_BELT_NEAR_M);

                // Authored runoff on this side at this station: the belt
                // starts beyond it.
                let inside_runoff = surfaces
                    .iter()
                    .filter(|s| s.side == side && is_prepared_runoff(s.kind))
                    .filter_map(|s| {
                        span_progress(path, s.start_m, s.end_m, station)
                            .map(|t| s.inner_m + s.width_at(t))
                    })
                    .any(|outer| beyond < outer + TREE_RUNOFF_MARGIN_M);
                if inside_runoff {
                    continue;
                }

                let lat = signed(side, side_half_width(&sample, side) + beyond);
                let pos = offset_point(&sample, lat);

                // The nearest section of the course — which, where the
                // circuit folds, may not be the one that planted it — must
                // still be a belt's distance away.
                let (ns, nlat, _) = nearest_cross_section(path, pos.0, pos.1);
                if nlat.abs() < half_width_on(&ns, nlat) + TREE_BELT_NEAR_M - 0.5 {
                    continue;
                }
                if lane.is_some_and(|lane| lane_edge_gap(lane, pos.0, pos.1) < TREE_LANE_CLEAR_M) {
                    continue;
                }
                let blocked = others.iter().any(|p| {
                    let clear = match p.kind {
                        PropKind::Building
                        | PropKind::Grandstand
                        | PropKind::Pit
                        | PropKind::Attraction => TREE_STAND_CLEAR_M,
                        PropKind::Sky => return false,
                        _ => TREE_PROP_CLEAR_M,
                    };
                    footprint_gap(p, pos.0, pos.1) < clear
                });
                if blocked {
                    continue;
                }
                let (gx, gy) = grid_key(pos.0, pos.1);
                let crowded = (gx - 1..=gx + 1).any(|cx| {
                    (gy - 1..=gy + 1).any(|cy| {
                        planted.get(&(cx, cy)).is_some_and(|near| {
                            near.iter()
                                .any(|(tx, ty)| (tx - pos.0).hypot(ty - pos.1) < TREE_PROP_CLEAR_M)
                        })
                    })
                });
                if crowded {
                    continue;
                }

                // With a dossier the belt is the real woodland and
                // nothing else: the dunes at Zandvoort stay bare, the
                // Ardennes stay dense, and both are right for once.
                let leaf = if woods.is_empty() {
                    None
                } else {
                    match woods.iter().find(|w| w.contains(pos.0, pos.1)) {
                        Some(wood) => Some(wood.leaf.as_str()),
                        None => continue,
                    }
                };

                planted.entry((gx, gy)).or_default().push((pos.0, pos.1));
                // Within the near band the species is the detailed one;
                // behind it, the blob.
                let species = tree_asset(leaf, hash01(&tree, 5));
                let asset = if beyond <= TREE_NEAR_VIEW_M {
                    near_variant(species)
                } else {
                    species
                };
                trees.push(Prop {
                    id: 0,
                    kind: PropKind::Tree,
                    asset: asset.to_string(),
                    x: pos.0,
                    y: pos.1,
                    z: seat_z(terrain, &ns, nlat, pos.0, pos.1),
                    yaw_rad: hash01(&tree, 3) * TAU,
                    scale: TREE_SCALE_MIN + hash01(&tree, 4) * (TREE_SCALE_MAX - TREE_SCALE_MIN),
                    text: None,
                    length_m: None,
                });
            }
        }
    }
    trees
}

fn is_prepared_runoff(kind: SurfaceKind) -> bool {
    matches!(
        kind,
        SurfaceKind::Gravel
            | SurfaceKind::AsphaltRunoff
            | SurfaceKind::Sand
            | SurfaceKind::Concrete
    )
}

/// Deterministic jitter in `[0, 1)` from integer keys and a salt
/// (FNV-1a over the words, then a splitmix finaliser so neighbouring cells
/// don't correlate).
fn hash01(keys: &[u64], salt: u64) -> f32 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for word in keys.iter().chain(std::iter::once(&salt)) {
        for byte in word.to_le_bytes() {
            h ^= byte as u64;
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    h ^= h >> 30;
    h = h.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    h ^= h >> 27;
    h = h.wrapping_mul(0x94d0_49bb_1331_11eb);
    h ^= h >> 31;
    (h >> 40) as f32 / (1u64 << 24) as f32
}

/// Planar distance from a point to the edge of a prop's footprint
/// (negative inside). Stands, buildings, walls and barriers are oriented
/// slabs ([`footprint_half_extents`]) — a 78 m grandstand or a 12 m wall
/// segment is nothing like a disc — everything else a disc of
/// [`prop_radius`].
fn footprint_gap(prop: &Prop, x: f32, y: f32) -> f32 {
    match footprint_half_extents(prop) {
        Some((half_len, half_thick)) => {
            let (sin_h, cos_h) = prop.yaw_rad.sin_cos();
            let (dx, dy) = (x - prop.x, y - prop.y);
            let along = (cos_h * dx + sin_h * dy).abs() - half_len;
            let across = (-sin_h * dx + cos_h * dy).abs() - half_thick;
            if along > 0.0 || across > 0.0 {
                along.max(0.0).hypot(across.max(0.0))
            } else {
                along.max(across)
            }
        }
        None => (x - prop.x).hypot(y - prop.y) - prop_radius_of(prop),
    }
}

/// Planar distance from a point to the pit lane's edge (negative inside
/// the lane). The lane is an open path: past either end the lateral
/// offset from the end sample is meaningless — the exit taper's heading
/// line runs across the road and would mark a strip on the *far* side of
/// the track as "beside the lane" — so beyond the ends the straight-line
/// distance to the end is used instead.
fn lane_edge_gap(lane: &CenterlinePath, x: f32, y: f32) -> f32 {
    let (sample, lat, along) = nearest_cross_section(lane, x, y);
    let samples = lane.samples();
    let at_start = sample.station_m <= samples[0].station_m && along < 0.0;
    let at_end = sample.station_m >= samples[samples.len() - 1].station_m && along > 0.0;
    let distance = if at_start || at_end {
        (x - sample.pos.0).hypot(y - sample.pos.1)
    } else {
        lat.abs()
    };
    distance - sample.width_left_m
}

/// The nearest sampled cross-section to a point, with the point's signed
/// lateral offset from it (positive = left of the course) and its
/// along-course offset from the sample (refining the station beyond the
/// ~2 m sample spacing, which matters for the wall-run station grid).
fn nearest_cross_section(path: &CenterlinePath, x: f32, y: f32) -> (PathSample, f32, f32) {
    let sample = *path
        .samples()
        .iter()
        .min_by(|a, b| {
            let da = (a.pos.0 - x).powi(2) + (a.pos.1 - y).powi(2);
            let db = (b.pos.0 - x).powi(2) + (b.pos.1 - y).powi(2);
            da.partial_cmp(&db).expect("finite sample distances")
        })
        .expect("CenterlinePath always has samples");
    let (sin_h, cos_h) = sample.heading_rad.sin_cos();
    let (dx, dy) = (x - sample.pos.0, y - sample.pos.1);
    // Left normal is the heading rotated +90°: (-sin, cos).
    let lat = -sin_h * dx + cos_h * dy;
    let along = cos_h * dx + sin_h * dy;
    (sample, lat, along)
}

/// How far beyond the track edge a wall belongs at this cross-section: past
/// the widest prepared runoff authored there, or on the verge when the only
/// thing beside the track is grass.
fn wall_offset(
    path: &CenterlinePath,
    surfaces: &[crate::ats::Surface],
    sample: &PathSample,
    side: Side,
) -> f32 {
    let runoff_outer = surfaces
        .iter()
        .filter(|s| s.side == side && is_prepared_runoff(s.kind))
        .filter_map(|s| {
            span_progress(path, s.start_m, s.end_m, sample.station_m)
                .map(|t| s.inner_m + s.width_at(t))
        })
        .fold(None::<f32>, |acc, outer| {
            Some(acc.map_or(outer, |a| a.max(outer)))
        });

    match runoff_outer {
        Some(outer) => (outer + WALL_GAP_M).min(MAX_WALL_BEYOND_EDGE_M),
        None => VERGE_OFFSET_M,
    }
}

/// Progress (0..=1) of `station` through the span, or `None` when the span
/// does not cover it. Wraps through start/finish on closed loops; a
/// zero-length span on a closed loop means the full lap, matching the mesh
/// builders.
fn span_progress(path: &CenterlinePath, start_m: f32, end_m: f32, station: f32) -> Option<f32> {
    let total = path.total_length_m();
    if path.is_closed() {
        let mut span = (end_m - start_m).rem_euclid(total);
        if span <= f32::EPSILON {
            span = total;
        }
        let along = (station - start_m).rem_euclid(total);
        (along <= span).then(|| along / span)
    } else {
        let span = end_m - start_m;
        if span <= 0.0 {
            return None;
        }
        let along = station - start_m;
        (0.0..=span).contains(&along).then(|| along / span)
    }
}

fn yaw_distance(a: f32, b: f32) -> f32 {
    use std::f32::consts::PI;
    ((a - b + PI).rem_euclid(TAU) - PI).abs()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ats::{Prop, Surface};
    use crate::track_data::TrackNode;

    fn node(x: f32, y: f32, z: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z,
            width: None,
            width_left: None,
            width_right: None,
            banking: None,
            friction: None,
            surface_type: None,
        }
    }

    /// A flat stadium run counter-clockwise: an 800 m straight along
    /// y = 0, a 70 m radius hairpin, the return straight along y = 140 and
    /// a second hairpin back to the line. Nodes are dense (25 m on the
    /// straights, 10° on the arcs) so the Catmull-Rom spline neither
    /// wiggles at the nodes nor rounds the corners off — the curvature
    /// sweep sees clean straights and two unambiguous corners.
    fn track() -> TrackFile {
        const R: f32 = 70.0;
        let mut nodes = Vec::new();
        for i in 0..=32 {
            nodes.push(node(i as f32 * 25.0, 0.0, 0.0));
        }
        for deg in (-80..=80).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(800.0 + R * cos, R + R * sin, 0.0));
        }
        for i in (0..=32).rev() {
            nodes.push(node(i as f32 * 25.0, 2.0 * R, 0.0));
        }
        for deg in (100..=260).step_by(10) {
            let (sin, cos) = (deg as f32).to_radians().sin_cos();
            nodes.push(node(R * cos, R + R * sin, 0.0));
        }
        TrackFile {
            name: "Groom".to_string(),
            track_id: None,
            nodes,
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 12.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        }
    }

    fn prop(kind: PropKind, x: f32, y: f32, z: f32) -> Prop {
        Prop {
            id: 0,
            kind,
            asset: "test".to_string(),
            x,
            y,
            z,
            yaw_rad: 1.0,
            scale: 1.0,
            text: None,
            length_m: None,
        }
    }

    fn scene_with(track: &TrackFile, props: Vec<Prop>) -> AtsScene {
        let mut scene = AtsScene::new_for_track(track, "Groom.yaml");
        for (i, mut p) in props.into_iter().enumerate() {
            p.id = scene.next_id + i as u64;
            scene.props.push(p);
        }
        scene.next_id += scene.props.len() as u64;
        scene
    }

    fn of_kind(scene: &AtsScene, kind: PropKind) -> Vec<&Prop> {
        scene.props.iter().filter(|p| p.kind == kind).collect()
    }

    #[test]
    fn no_absorbing_barrier_down_a_straight() {
        // The straights get a rail, not a tyre wall: absorbing barriers
        // belong where a car can arrive at one, and the old enrichment
        // ringed whole circuits with them.
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::TireWall, 350.0, 40.0, 0.0)]);
        groom_props(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        for wall in of_kind(&scene, PropKind::TireWall) {
            let (sample, _, _) = nearest_cross_section(&path, wall.x, wall.y);
            let radius = tightest_radius(&path, sample.station_m);
            assert!(
                radius < barriers::STRAIGHT_RADIUS_M,
                "absorbing barrier at station {} where the radius is {radius} m",
                sample.station_m
            );
        }
    }

    #[test]
    fn the_barrier_line_follows_the_runoff_edge() {
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        let gravel_id = scene.alloc_id();
        scene.surfaces.push(Surface {
            id: gravel_id,
            kind: SurfaceKind::Gravel,
            side: Side::Right,
            start_m: 0.0,
            end_m: 0.0, // full lap
            inner_m: 1.0,
            width_m: 14.0,
            end_width_m: None,
        });

        let report = groom_props(&track, &mut scene).unwrap();
        assert!(report.walls_rebuilt || report.barriers_rebuilt);

        // Half width 6 + gravel (1 + 14) + gap 1 = 22 m right of the
        // course, parallel to it.
        let path = CenterlinePath::from_track(&track).unwrap();
        let mut checked = 0;
        for barrier in scene
            .props
            .iter()
            .filter(|p| matches!(p.kind, PropKind::Barrier | PropKind::TireWall))
        {
            let (sample, lat, _) = nearest_cross_section(&path, barrier.x, barrier.y);
            if lat > 0.0 {
                continue; // the gravel is on the right
            }
            assert!((lat + 22.0).abs() < 2.5, "barrier sits at lateral {lat} m");
            assert!(
                yaw_distance(barrier.yaw_rad, sample.heading_rad) < 0.3,
                "yaw {} vs course {}",
                barrier.yaw_rad,
                sample.heading_rad
            );
            checked += 1;
        }
        assert!(checked > 10, "only {checked} barriers on the gravel side");
    }

    #[test]
    fn without_runoff_the_barrier_line_keeps_its_distance() {
        // With nothing authored beside the road, a straight's rail stands
        // where the old armco did and a corner's barrier leaves the run-off
        // a car needs: closer than that, the AI found a barrier every time
        // it ran wide or cut an apex.
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        groom_props(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let mut checked = 0;
        for barrier in scene
            .props
            .iter()
            .filter(|p| matches!(p.kind, PropKind::Barrier | PropKind::TireWall))
        {
            let (sample, lat, _) = nearest_cross_section(&path, barrier.x, barrier.y);
            let beyond = lat.abs() - half_width_on(&sample, lat);
            // Nothing anywhere closer than a straight's rail.
            assert!(
                beyond >= STRAIGHT_BARRIER_MIN_M - 2.5,
                "barrier {beyond:.1} m past the edge at station {:.0}",
                sample.station_m
            );
            // And well inside a corner, the corner's run-off. The decision
            // is taken per 4 m cell, so a module at the exact point where a
            // straight turns into a corner belongs to whichever cell laid
            // it; only a barrier with corner on both sides of it is judged
            // by the corner's rule.
            let corner_here = |s: f32| tightest_radius(&path, s) < barriers::STRAIGHT_RADIUS_M;
            let deep_in_corner = corner_here(sample.station_m - 2.0 * BARRIER_CELL_M)
                && corner_here(sample.station_m + 2.0 * BARRIER_CELL_M);
            if deep_in_corner {
                assert!(
                    beyond >= CORNER_BARRIER_MIN_M - 2.5,
                    "corner barrier only {beyond:.1} m past the edge at station {:.0}",
                    sample.station_m
                );
            }
            checked += 1;
        }
        assert!(checked > 50, "only {checked} barriers laid");
    }

    #[test]
    fn the_barrier_line_covers_the_lap_on_both_sides() {
        // The defect this replaced: a corner only got a wall when the old
        // enrichment happened to have left a prop within 40 m of it, so
        // coverage was wherever the scatter had been, not wherever a car
        // can leave the road.
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        groom_props(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let total = path.total_length_m();

        let mut covered_left = vec![false; (total / BARRIER_CELL_M) as usize + 1];
        let mut covered_right = covered_left.clone();
        for barrier in scene
            .props
            .iter()
            .filter(|p| matches!(p.kind, PropKind::Barrier | PropKind::TireWall))
        {
            let (sample, lat, _) = nearest_cross_section(&path, barrier.x, barrier.y);
            let cell = (sample.station_m / BARRIER_CELL_M) as usize;
            let side = if lat > 0.0 {
                &mut covered_left
            } else {
                &mut covered_right
            };
            if let Some(slot) = side.get_mut(cell) {
                *slot = true;
            }
        }
        let left = covered_left.iter().filter(|c| **c).count();
        let right = covered_right.iter().filter(|c| **c).count();
        let cells = covered_left.len();
        // The pit lane takes the left of the bottom straight, so the left
        // is guarded less than the right; both must still be most of the
        // lap rather than a scatter.
        assert!(
            right * 10 >= cells * 9,
            "only {right}/{cells} cells guarded on the right"
        );
        assert!(
            left * 10 >= cells * 9,
            "only {left}/{cells} cells guarded on the left"
        );
    }

    #[test]
    fn every_barrier_asset_is_one_the_kit_has() {
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        groom_props(&track, &mut scene).unwrap();
        let known: Vec<&str> = [
            barriers::BarrierKind::Armco,
            barriers::BarrierKind::ArmcoFence,
            barriers::BarrierKind::Tecpro,
            barriers::BarrierKind::Tyres,
            barriers::BarrierKind::Concrete,
        ]
        .iter()
        .flat_map(|k| {
            let (_, asset) = k.asset();
            let (_, cap) = k.end_cap().unwrap();
            [asset, cap]
        })
        .collect();
        for barrier in scene
            .props
            .iter()
            .filter(|p| matches!(p.kind, PropKind::Barrier | PropKind::TireWall))
        {
            assert!(
                known.contains(&barrier.asset.as_str()),
                "unknown barrier asset {}",
                barrier.asset
            );
            assert!(
                crate::props::KIT.iter().any(|a| a.asset == barrier.asset),
                "{} is not in the prop kit",
                barrier.asset
            );
        }
    }

    #[test]
    fn a_mapped_tyre_wall_is_laid_where_the_map_puts_it() {
        // The dossier's own barrier lines win over the geometry rule.
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        let mut layout = crate::layout::Layout {
            source_track: "Groom.yaml".to_string(),
            ..Default::default()
        };
        // A run down the middle of the bottom straight's right verge,
        // where the geometry alone would have chosen a plain rail.
        let line: Vec<[f32; 2]> = (0..20)
            .map(|i| {
                let station = 300.0 + i as f32 * 5.0;
                let sample = path.sample_at(station);
                let lat = -(side_half_width(&sample, Side::Right) + VERGE_OFFSET_M);
                let p = offset_point(&sample, lat);
                [p.0, p.1]
            })
            .collect();
        layout.barriers.push(crate::layout::Line {
            kind: "tyres".to_string(),
            name: None,
            line,
            closed: false,
            bridge: false,
            tunnel: false,
        });

        let mut scene = scene_with(&track, vec![]);
        groom_scene_with(&track, &mut scene, Some(&layout)).unwrap();
        let mut found = 0;
        for wall in of_kind(&scene, PropKind::TireWall) {
            let (sample, lat, _) = nearest_cross_section(&path, wall.x, wall.y);
            if lat < 0.0 && (300.0..=400.0).contains(&sample.station_m) {
                assert_eq!(wall.asset, "tires_4m");
                found += 1;
            }
        }
        assert!(found > 5, "only {found} mapped tyre modules laid");
    }

    #[test]
    fn buildings_keep_their_footprint_and_get_reseated() {
        let track = track();
        // 60 m out on a flat track, "floating" 10 m above the terrain.
        let mut scene = scene_with(&track, vec![prop(PropKind::Building, 350.0, 60.0, 10.0)]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.removed, 0);
        assert_eq!(report.reseated, 1);

        let building = &scene.props[0];
        assert_eq!(building.kind, PropKind::Building);
        assert_eq!((building.x, building.y), (350.0, 60.0));
        assert!(
            building.z < 2.0,
            "building still floats at z = {}",
            building.z
        );
    }

    #[test]
    fn props_on_the_road_are_pushed_clear() {
        let track = track();
        // A building dead on the bottom straight.
        let mut scene = scene_with(&track, vec![prop(PropKind::Building, 350.0, 0.0, 0.0)]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.pushed, 1);

        let path = CenterlinePath::from_track(&track).unwrap();
        let b = &scene.props[0];
        assert_eq!(b.kind, PropKind::Building);
        let (s, lat, _) = nearest_cross_section(&path, b.x, b.y);
        let needed = prop_lat_clearance(&s, lat, prop_radius(PropKind::Building));
        assert!(lat.abs() >= needed - 0.1, "building at lateral {lat} m");
    }

    #[test]
    fn a_grandstand_is_pushed_until_its_whole_footprint_clears_the_road() {
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        // A 60 m × 24 m stand (scale 2) lying along the bottom straight
        // with its centre 5 m left of the centerline: on the asphalt.
        let mut stand = prop(PropKind::Grandstand, 350.0, 5.0, 0.0);
        stand.scale = 2.0;
        stand.yaw_rad = 0.0;
        let mut scene = scene_with(&track, vec![stand]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.pushed, 1);
        assert!(report.deleted.is_empty());

        let stand = &scene.props[0];
        assert_eq!(stand.kind, PropKind::Grandstand);
        let clearance = stand_road_clearance_m(&path, stand).unwrap();
        assert!(
            clearance >= STAND_ROAD_CLEAR_M - 0.01,
            "footprint clears the road by only {clearance} m"
        );
        // Pushed straight out along the normal, not slid along the course.
        assert!(
            (stand.x - 350.0).abs() < 0.5,
            "stand slid to x = {}",
            stand.x
        );
        assert!(stand.y > 6.0 + 12.0 + STAND_ROAD_CLEAR_M - 1.0);
        // And it is stable: the second pass leaves it alone.
        let second = groom_props(&track, &mut scene).unwrap();
        assert!(!second.changed(), "{second:?}");
    }

    #[test]
    fn props_are_pushed_clear_of_the_pit_lane_and_aligned() {
        let track = track();
        // groom_scene builds the pit lane first (interior = left side of the
        // bottom straight); this building sits right where the lane runs.
        let mut scene = scene_with(&track, vec![prop(PropKind::Building, 100.0, 15.0, 0.0)]);
        groom_scene(&track, &mut scene).unwrap();

        let pit = scene.pit_lane.clone().expect("pit lane generated");
        let lane = CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0).unwrap();
        let b = &scene.props[0];
        assert_eq!(b.kind, PropKind::Building);
        let (ls, llat, _) = nearest_cross_section(&lane, b.x, b.y);
        let needed = ls.width_left_m + PROP_CLEARANCE_M + prop_radius(PropKind::Building);
        assert!(
            llat.abs() >= needed - 0.1,
            "building at lane lateral {llat} m"
        );
        // Pit furniture squares up with the lane.
        assert!(
            yaw_distance(b.yaw_rad, ls.heading_rad) < 0.3,
            "yaw {} vs lane {}",
            b.yaw_rad,
            ls.heading_rad
        );
    }

    #[test]
    fn both_hairpins_are_braking_corners_with_boards_on_the_outside() {
        let track = track();
        let path = CenterlinePath::from_track(&track).unwrap();
        let corners = braking_corners(&path);
        assert_eq!(corners.len(), 2, "{corners:?}");
        // The course runs +X along y = 0 and turns left (CCW) at x = 800:
        // the outside of each bend is the right-hand side.
        for corner in &corners {
            assert!(corner.run.entry_kappa > 0.0);
            assert_eq!(corner.outside(), Side::Right);
            assert_eq!(corner.boards.len(), 3);
            for (i, (asset, station)) in corner.boards.iter().enumerate() {
                assert_eq!(*asset, BOARDS[i].1);
                let expected = (corner.entry_m() - BOARDS[i].0).rem_euclid(path.total_length_m());
                assert!((station - expected).abs() < 1e-3);
            }
        }

        let mut scene = scene_with(&track, vec![]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.boards, 6);
        let boards = of_kind(&scene, PropKind::Sign);
        assert_eq!(boards.len(), 6);
        for board in boards {
            let (sample, lat, _) = nearest_cross_section(&path, board.x, board.y);
            assert!(
                (lat + sample.width_right_m + BOARD_LATERAL_M).abs() < 0.5,
                "board {} at lateral {lat}",
                board.asset
            );
            assert!(yaw_distance(board.yaw_rad, sample.heading_rad) < 0.05);
            assert_eq!(board.scale, 1.0);
            let text = board.text.as_deref().unwrap();
            assert!(board.asset == format!("board_{text}m"), "{board:?}");
        }
    }

    #[test]
    fn tree_belts_keep_their_distance() {
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::Grandstand, 350.0, 60.0, 0.0)]);
        groom_scene(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let stand = scene.props[0].clone();
        assert_eq!(stand.kind, PropKind::Grandstand);

        // Ground cover is `tree` kind too, and is meant to sit close
        // together, so the spacing rule below is about trees only.
        let trees: Vec<&Prop> = of_kind(&scene, PropKind::Tree)
            .into_iter()
            .filter(|t| !is_ground_cover(&t.asset))
            .collect();
        assert!(trees.len() > 200, "only {} trees", trees.len());
        for (i, t) in trees.iter().enumerate() {
            assert!(
                is_tree_pass_asset(&t.asset),
                "tree {i} planted as {}",
                t.asset
            );
            assert!((TREE_SCALE_MIN..=TREE_SCALE_MAX).contains(&t.scale));
            let (sample, lat, _) = nearest_cross_section(&path, t.x, t.y);
            let beyond = lat.abs() - half_width_on(&sample, lat);
            assert!(
                (TREE_BELT_NEAR_M - 1.0..=TREE_BELT_FAR_M + 1.0).contains(&beyond),
                "tree {beyond} m beyond the edge"
            );
            assert!(
                footprint_gap(&stand, t.x, t.y) >= TREE_STAND_CLEAR_M - 0.1,
                "tree {} m from the grandstand",
                footprint_gap(&stand, t.x, t.y)
            );
            for other in &trees[i + 1..] {
                assert!(
                    (t.x - other.x).hypot(t.y - other.y) >= TREE_PROP_CLEAR_M - 1e-3,
                    "trees {} and {} crowd each other",
                    t.id,
                    other.id
                );
            }
        }
    }

    #[test]
    fn desert_tracks_get_half_the_trees() {
        let mut track = track();
        let mut scene = scene_with(&track, vec![]);
        let full = groom_props(&track, &mut scene).unwrap().trees;

        track.metadata = Some(crate::track_data::TrackMetadata {
            environment_type: Some("desert".to_string()),
            ..Default::default()
        });
        let mut scene = scene_with(&track, vec![]);
        let half = groom_props(&track, &mut scene).unwrap().trees;
        assert!(
            half > full / 3 && half < full * 2 / 3,
            "desert {half} vs full {full}"
        );
    }

    #[test]
    fn generated_props_are_relaid_not_duplicated() {
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        let first = groom_props(&track, &mut scene).unwrap();
        // Nudge one tree and one barrier: their passes must lay them back
        // in place without growing the scene.
        let tree = scene
            .props
            .iter()
            .position(|p| p.kind == PropKind::Tree)
            .unwrap();
        scene.props[tree].x += 3.0;
        let barrier = scene
            .props
            .iter()
            .position(|p| p.kind == PropKind::Barrier)
            .unwrap();
        scene.props.remove(barrier);
        let second = groom_props(&track, &mut scene).unwrap();
        assert!(second.trees_rebuilt && second.barriers_rebuilt);
        assert!(!second.boards_rebuilt && !second.walls_rebuilt);
        assert_eq!(second.trees, first.trees);
        assert_eq!(second.barriers, first.barriers);
        assert!(scene.validate().is_ok());
    }

    #[test]
    fn grooming_is_idempotent() {
        let track = track();
        let mut scene = scene_with(
            &track,
            vec![
                prop(PropKind::TireWall, 350.0, 40.0, 0.0), // removed (straight)
                prop(PropKind::TireWall, 900.0, 100.0, 0.0), // pulled in (corner)
                prop(PropKind::Building, 350.0, 60.0, 10.0),
                prop(PropKind::Tree, 350.0, -40.0, 3.0),
            ],
        );
        assert!(groom_scene(&track, &mut scene).unwrap().changed());
        let after_first = scene.clone();
        let second = groom_scene(&track, &mut scene).unwrap();
        assert!(!second.changed(), "second pass changed: {second:?}");
        assert_eq!(scene, after_first);
        assert!(scene.validate().is_ok());
    }

    #[test]
    fn boards_snap_onto_the_nearest_barrier_run() {
        let track = track();
        // Mid-straight, where the armco pass lays a rail 7 m off the edge
        // (6 m half width): the board starts 20 m out and comes in behind it.
        let mut scene = scene_with(&track, vec![prop(PropKind::Board, 400.0, 20.0, 0.0)]);
        groom_props(&track, &mut scene).unwrap();
        let board = of_kind(&scene, PropKind::Board)[0];
        let rail = scene
            .props
            .iter()
            .filter(|p| p.kind == PropKind::Barrier)
            .min_by(|a, b| {
                (a.x - board.x)
                    .hypot(a.y - board.y)
                    .total_cmp(&(b.x - board.x).hypot(b.y - board.y))
            })
            .expect("armco was laid");
        assert!(
            (board.y - (rail.y + BOARD_BEHIND_BARRIER_M)).abs() < 0.05,
            "board y {} vs rail y {}",
            board.y,
            rail.y
        );
        assert!(
            (board.x - 400.0).abs() < 0.5,
            "board keeps its station: {}",
            board.x
        );
        assert!(yaw_distance(board.yaw_rad, 0.0) < 1e-3);

        let before = scene.clone();
        let again = groom_props(&track, &mut scene).unwrap();
        assert_eq!(scene, before);
        assert!(!again.changed(), "{again:?}");
    }

    #[test]
    fn fences_stand_behind_the_wall_line() {
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::Fence, 400.0, 30.0, 5.0)]);
        groom_props(&track, &mut scene).unwrap();
        let fence = of_kind(&scene, PropKind::Fence)[0];
        // No runoff authored: the wall line is the verge offset.
        let expected = 6.0 + VERGE_OFFSET_M + FENCE_BEHIND_WALL_M;
        assert!(
            (fence.y - expected).abs() < 0.05,
            "fence y {} vs {expected}",
            fence.y
        );
        assert!(
            fence.z.abs() < 0.5,
            "seated on the flat ground: {}",
            fence.z
        );
        let before = scene.clone();
        assert!(!groom_props(&track, &mut scene).unwrap().changed());
        assert_eq!(scene, before);
    }

    #[test]
    fn bridges_pits_and_sky_props_are_never_pushed() {
        let track = track();
        let mut bridge = prop(PropKind::Bridge, 400.0, 0.0, 3.0);
        bridge.yaw_rad = 0.0;
        let blimp = prop(PropKind::Sky, 400.0, 0.0, 150.0);
        let garage = prop(PropKind::Pit, 400.0, 2.0, 0.0);
        let mut scene = scene_with(&track, vec![bridge, blimp, garage]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.pushed, 0);
        assert!(report.deleted.is_empty());
        let bridge = of_kind(&scene, PropKind::Bridge)[0];
        assert_eq!(
            (bridge.x, bridge.y),
            (400.0, 0.0),
            "bridge stays on the road"
        );
        assert!(
            bridge.z.abs() < 0.05,
            "bridge takes the road height: {}",
            bridge.z
        );
        let blimp = of_kind(&scene, PropKind::Sky)[0];
        assert_eq!(
            (blimp.x, blimp.y, blimp.z),
            (400.0, 0.0, 150.0),
            "altitude kept"
        );
        let garage = of_kind(&scene, PropKind::Pit)[0];
        assert_eq!(
            (garage.x, garage.y, garage.z),
            (400.0, 2.0, 0.0),
            "pit left alone"
        );
    }

    #[test]
    fn attractions_are_pushed_clear_by_their_footprint() {
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::Attraction, 400.0, 10.0, 0.0)]);
        groom_props(&track, &mut scene).unwrap();
        let wheel = of_kind(&scene, PropKind::Attraction)[0];
        let path = CenterlinePath::from_track(&track).unwrap();
        let (s, lat, _) = nearest_cross_section(&path, wheel.x, wheel.y);
        let needed = prop_lat_clearance(&s, lat, prop_radius(PropKind::Attraction));
        assert!(lat.abs() >= needed - 0.05, "lat {lat} < needed {needed}");
    }
}
