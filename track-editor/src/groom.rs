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
/// this gap along the course fuse into one chain…
const WALL_GROUP_GAP_M: f32 = 45.0;
/// …of segments spaced this far apart. The tire-wall stand-in is 6 m at
/// scale 1; the scale is set slightly above `WALL_SEGMENT_M / 6` so
/// neighbouring segments overlap ~10 % and a run stays visually continuous
/// through a curve instead of opening chinks on its outside.
const WALL_SEGMENT_M: f32 = 12.0;
const WALL_SCALE: f32 = 2.2;

/// Free clearance between a prop's footprint and the road edge or the pit
/// lane's edge.
const PROP_CLEARANCE_M: f32 = 1.5;
/// Buildings this close to the pit lane are pit boxes: align them with the
/// lane so the pit complex reads as a row, not a scatter.
const PIT_BOX_ALIGN_RANGE_M: f32 = 40.0;

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

/// Asset key every generated tree carries; the tree pass owns those.
pub const TREE_ASSET: &str = "tree_generic";
/// Station cell size of the tree belts.
const TREE_CELL_M: f32 = 12.0;
/// Depth range of the belt, measured beyond the road edge.
const TREE_BELT_NEAR_M: f32 = 22.0;
const TREE_BELT_FAR_M: f32 = 90.0;
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
/// Barriers stand this far beyond the road edge.
const BARRIER_OFFSET_M: f32 = 7.0;
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
        PropKind::Tree => 1.5,
        PropKind::Sign => 1.0,
        PropKind::Light => 0.7,
        PropKind::Cone => 0.3,
        PropKind::Misc | PropKind::TireWall | PropKind::Barrier => 0.6,
    }
}

/// Oriented footprint of the large stand-ins as half-extents (along the
/// prop's local +X, i.e. its yaw, and across it), matching Unreal's
/// `PlaceholderPropSize`: a grandstand is 30 m × 12 m, a building
/// 15 m × 10 m, a wall or barrier 6 m / 4 m long, all times scale. A
/// grandstand's seating faces local +Y.
fn footprint_half_extents(kind: PropKind, scale: f32) -> Option<(f32, f32)> {
    match kind {
        PropKind::Grandstand => Some((15.0 * scale, 6.0 * scale)),
        PropKind::Building => Some((7.5 * scale, 5.0 * scale)),
        PropKind::TireWall => Some((3.0 * scale, 0.5 * scale)),
        PropKind::Barrier => Some((2.0 * scale, 0.3 * scale)),
        _ => None,
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
    let ext = footprint_half_extents(prop.kind, prop.scale)?;
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
    prop.kind == PropKind::Tree && prop.asset == TREE_ASSET
}

fn owned_by_barrier_pass(prop: &Prop) -> bool {
    prop.kind == PropKind::Barrier && prop.asset == BARRIER_ASSET
}

/// Groom the whole scene: the pit lane is rebuilt first (entry, pit road,
/// exit — [`crate::pit::generate_pit_lane`]) so that prop grooming can keep
/// props clear of the *final* lane, then every prop is placed by
/// [`groom_props`].
pub fn groom_scene(track: &TrackFile, scene: &mut AtsScene) -> Option<GroomReport> {
    let path = CenterlinePath::from_track(track)?;
    let pit_rebuilt = match crate::pit::generate_pit_lane(&path, scene.pit_lane.as_ref()) {
        Some(pit) => {
            let rebuilt = scene.pit_lane.as_ref() != Some(&pit);
            scene.pit_lane = Some(pit);
            rebuilt
        }
        None => false,
    };

    let mut report = groom_props(track, scene)?;
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
    let path = CenterlinePath::from_track(track)?;
    let terrain = TerrainHeightfield::from_path(&path)?;

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
    let mut anchors: Vec<WallAnchor> = Vec::new();
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
                anchors.push(wall_anchor_of(&path, &surfaces, &prop));
                original_walls.push(prop);
            }
            _ => {
                let (sample, lat, _) = nearest_cross_section(&path, prop.x, prop.y);
                let radius = prop_radius(prop.kind) * prop.scale;
                let (mut x, mut y, mut yaw) = (prop.x, prop.y, prop.yaw_rad);

                if let Some(ext) = footprint_half_extents(prop.kind, prop.scale) {
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

    let (new_walls, dropped) = lay_wall_runs(&path, &terrain, &surfaces, anchors);
    report.removed = dropped;
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

    let barriers = lay_straight_barriers(&path, &terrain, lane.as_ref(), &props);
    report.barriers = barriers.len();
    props.extend(adopt(
        scene,
        original_barriers,
        barriers,
        &mut report.barriers_rebuilt,
    ));

    let trees = lay_tree_belts(
        &path,
        &terrain,
        &surfaces,
        lane.as_ref(),
        &props,
        tree_density(track),
    );
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

/// One kept wall's place along the course, before runs are laid.
struct WallAnchor {
    station_m: f32,
    left: bool,
    asset: String,
}

/// Course sections within this reach of a wall are tried when recovering
/// the cell that laid it.
const WALL_RECOGNISE_RANGE_M: f32 = 40.0;

/// The cell an existing wall belongs to. A wall this pass laid stands
/// exactly where its cell puts it, so the cell is recovered by trying every
/// course section within reach and keeping the one whose cell reproduces
/// the wall's position. The nearest section alone is not enough: where
/// the circuit folds, a wall 24 m out at a hairpin can lie nearer the
/// other leg, and re-grooming would bounce it between two cells forever.
/// Anything else (the enrichment's scattered walls) anchors to the nearest
/// section.
fn wall_anchor_of(
    path: &CenterlinePath,
    surfaces: &[crate::ats::Surface],
    prop: &Prop,
) -> WallAnchor {
    let total = path.total_length_m();
    let normalise = |station: f32| {
        if path.is_closed() {
            station.rem_euclid(total)
        } else {
            station
        }
    };
    let mut best: Option<(f32, bool, f32)> = None;
    for s in path.samples() {
        if (s.pos.0 - prop.x).hypot(s.pos.1 - prop.y) > WALL_RECOGNISE_RANGE_M {
            continue;
        }
        let (sin_h, cos_h) = s.heading_rad.sin_cos();
        let (dx, dy) = (prop.x - s.pos.0, prop.y - s.pos.1);
        let lat = -sin_h * dx + cos_h * dy;
        let station = normalise(s.station_m + cos_h * dx + sin_h * dy);
        let idx = (station / WALL_SEGMENT_M).floor() as i64;
        let (_, _, pos) = wall_pose(path, surfaces, lat >= 0.0, idx);
        let miss = (pos.0 - prop.x).hypot(pos.1 - prop.y);
        if miss < MIN_MOVE_M && best.is_none_or(|(m, _, _)| miss < m) {
            best = Some((miss, lat >= 0.0, station));
        }
    }
    let (station_m, left) = match best {
        Some((_, left, station)) => (station, left),
        None => {
            let (s, lat, along) = nearest_cross_section(path, prop.x, prop.y);
            (normalise(s.station_m + along), lat >= 0.0)
        }
    };
    WallAnchor {
        station_m,
        left,
        asset: prop.asset.clone(),
    }
}

/// Where cell `idx`'s wall segment stands on one side: the cross-section
/// at the cell centre, the signed lateral offset (runoff edge or verge)
/// and the resulting position.
fn wall_pose(
    path: &CenterlinePath,
    surfaces: &[crate::ats::Surface],
    left: bool,
    idx: i64,
) -> (PathSample, f32, (f32, f32, f32)) {
    let side = if left { Side::Left } else { Side::Right };
    let sample = path.sample_at((idx as f32 + 0.5) * WALL_SEGMENT_M);
    let beyond_edge = wall_offset(path, surfaces, &sample, side);
    let target_lat = signed(side, side_half_width(&sample, side) + beyond_edge);
    let pos = offset_point(&sample, target_lat);
    (sample, target_lat, pos)
}

/// Re-lay walls as continuous runs on a fixed station grid.
///
/// Every anchor claims the [`WALL_SEGMENT_M`] cell its station falls in; a
/// cell survives iff *its center* is near a corner — a pure function of the
/// cell index, which is what makes re-grooming stable: the segments this
/// lays claim exactly the same cells when they come back as anchors. Gaps
/// up to [`WALL_GROUP_GAP_M`] between surviving cells on one side are
/// filled, and one segment is laid per cell, seated at the runoff edge.
///
/// Returns the segments and how many anchors were dropped on straights.
fn lay_wall_runs(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    surfaces: &[crate::ats::Surface],
    mut anchors: Vec<WallAnchor>,
) -> (Vec<Prop>, usize) {
    let cell_center = |idx: i64| (idx as f32 + 0.5) * WALL_SEGMENT_M;

    // Earliest anchor wins the cell's asset key, deterministically.
    anchors.sort_by(|a, b| {
        a.left
            .cmp(&b.left)
            .then(a.station_m.total_cmp(&b.station_m))
    });
    let mut cells: BTreeMap<(bool, i64), String> = BTreeMap::new();
    let mut anchor_cells: Vec<(bool, i64)> = Vec::new();
    for anchor in anchors {
        let idx = (anchor.station_m / WALL_SEGMENT_M).floor() as i64;
        anchor_cells.push((anchor.left, idx));
        if near_corner(path, cell_center(idx)) {
            cells.entry((anchor.left, idx)).or_insert(anchor.asset);
        }
    }

    // Fill the gaps inside each side's runs so the wall reads as one
    // barrier, not a dashed ring.
    let keys: Vec<(bool, i64)> = cells.keys().copied().collect();
    for pair in keys.windows(2) {
        let ((left_a, a), (left_b, b)) = (pair[0], pair[1]);
        if left_a == left_b
            && ((b - a) as f32) * WALL_SEGMENT_M <= WALL_GROUP_GAP_M + WALL_SEGMENT_M
        {
            let asset = cells[&(left_a, a)].clone();
            for idx in a + 1..b {
                cells.entry((left_a, idx)).or_insert_with(|| asset.clone());
            }
        }
    }

    // Straight-wall drops are judged against the final set: an anchor whose
    // cell came back via gap filling was not removed, or a groomed scene
    // would report removals forever.
    let dropped = anchor_cells
        .iter()
        .filter(|cell| !cells.contains_key(cell))
        .count();

    let walls = cells
        .into_iter()
        .map(|((left, idx), asset)| {
            let (sample, target_lat, pos) = wall_pose(path, surfaces, left, idx);
            let z = seat_z(terrain, &sample, target_lat, pos.0, pos.1);
            Prop {
                id: 0, // assigned by the caller
                kind: PropKind::TireWall,
                asset,
                x: pos.0,
                y: pos.1,
                z,
                yaw_rad: sample.heading_rad,
                scale: WALL_SCALE,
                text: None,
            }
        })
        .collect();
    (walls, dropped)
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

/// True when the course bends meaningfully somewhere within
/// [`CORNER_WINDOW_M`] of the station.
fn near_corner(path: &CenterlinePath, station_m: f32) -> bool {
    let steps = (2.0 * CORNER_WINDOW_M / 10.0) as i32;
    (0..=steps).any(|i| {
        let s = station_m - CORNER_WINDOW_M + i as f32 * 10.0;
        curvature_at(path, s).abs() >= CORNER_KAPPA
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

/// Line the straights with armco: one [`BARRIER_CELL_M`] segment per cell
/// and side, [`BARRIER_OFFSET_M`] beyond the road edge, wherever the cell
/// centre is not near a corner (the tire-wall pass covers those) and no
/// tire wall, building, grandstand or the pit lane comes within
/// [`BARRIER_CLEAR_M`]. The pit side of the pit straight, tapers included,
/// gets none.
fn lay_straight_barriers(
    path: &CenterlinePath,
    terrain: &TerrainHeightfield,
    lane: Option<&CenterlinePath>,
    others: &[Prop],
) -> Vec<Prop> {
    let lane_span = lane.map(|l| LaneSpan::of(path, l));
    let cells = (path.total_length_m() / BARRIER_CELL_M).floor() as i64;
    let mut barriers = Vec::new();
    for idx in 0..cells {
        let center = (idx as f32 + 0.5) * BARRIER_CELL_M;
        if near_corner(path, center) {
            continue;
        }
        let sample = path.sample_at(center);
        for side in Side::ALL {
            if lane_span
                .as_ref()
                .is_some_and(|span| span.covers(path, side, center))
            {
                continue;
            }
            let lat = signed(side, side_half_width(&sample, side) + BARRIER_OFFSET_M);
            let pos = offset_point(&sample, lat);
            if lane.is_some_and(|lane| lane_edge_gap(lane, pos.0, pos.1) < BARRIER_CLEAR_M) {
                continue;
            }
            // At an underpass the lower road has its walls instead, and the
            // upper road's armco would stand in the slot beneath it.
            let at_underpass =
                terrain
                    .wall_relation(pos.0, pos.1)
                    .is_some_and(|(past_wall, lower_z)| {
                        (sample.pos.2 - lower_z).abs() <= terrain::OVERHEAD_M
                            || past_wall < BARRIER_CLEAR_M
                    });
            if at_underpass {
                continue;
            }
            let blocked = others.iter().any(|p| {
                let clear = match p.kind {
                    PropKind::TireWall | PropKind::Building | PropKind::Grandstand => {
                        BARRIER_CLEAR_M
                    }
                    _ => BOARD_PROP_CLEAR_M,
                };
                footprint_gap(p, pos.0, pos.1) < clear
            });
            if blocked {
                continue;
            }
            barriers.push(Prop {
                id: 0,
                kind: PropKind::Barrier,
                asset: BARRIER_ASSET.to_string(),
                x: pos.0,
                y: pos.1,
                z: seat_z(terrain, &sample, lat, pos.0, pos.1),
                yaw_rad: sample.heading_rad,
                scale: 1.0,
                text: None,
            });
        }
    }
    barriers
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
                        PropKind::Building | PropKind::Grandstand => TREE_STAND_CLEAR_M,
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

                planted.entry((gx, gy)).or_default().push((pos.0, pos.1));
                trees.push(Prop {
                    id: 0,
                    kind: PropKind::Tree,
                    asset: TREE_ASSET.to_string(),
                    x: pos.0,
                    y: pos.1,
                    z: seat_z(terrain, &ns, nlat, pos.0, pos.1),
                    yaw_rad: hash01(&tree, 3) * TAU,
                    scale: TREE_SCALE_MIN + hash01(&tree, 4) * (TREE_SCALE_MAX - TREE_SCALE_MIN),
                    text: None,
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
    match footprint_half_extents(prop.kind, prop.scale) {
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
        None => (x - prop.x).hypot(y - prop.y) - prop_radius(prop.kind) * prop.scale,
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
    fn walls_on_straights_are_removed() {
        let track = track();
        // Mid-straight, 40 m out: exactly the ring clutter the enrichment
        // left everywhere.
        let mut scene = scene_with(&track, vec![prop(PropKind::TireWall, 350.0, 40.0, 0.0)]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.removed, 1);
        assert!(
            of_kind(&scene, PropKind::TireWall).is_empty(),
            "straight wall survived"
        );
    }

    #[test]
    fn stranded_corner_wall_is_pulled_to_the_runoff_edge() {
        let track = track();
        // Far outside the east corner.
        let mut scene = scene_with(&track, vec![prop(PropKind::TireWall, 900.0, 100.0, 0.0)]);
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
        assert!(report.walls_rebuilt);
        assert!(report.walls >= 1);
        assert_eq!(report.removed, 0);

        // Half width 6 + gravel (1 + 14) + gap 1 = 22 m right of the
        // course (the outside of a counter-clockwise lap), parallel to it.
        let path = CenterlinePath::from_track(&track).unwrap();
        let wall = &scene.props[0];
        assert_eq!(wall.kind, PropKind::TireWall);
        let (sample, lat, _) = nearest_cross_section(&path, wall.x, wall.y);
        assert!((lat + 22.0).abs() < 2.5, "wall sits at lateral {lat} m");
        assert!(
            yaw_distance(wall.yaw_rad, sample.heading_rad) < 0.25,
            "yaw {} vs course {}",
            wall.yaw_rad,
            sample.heading_rad
        );
    }

    #[test]
    fn corner_wall_without_runoff_lands_on_the_verge() {
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::TireWall, 900.0, 100.0, 5.0)]);
        groom_props(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let wall = &scene.props[0];
        assert_eq!(wall.kind, PropKind::TireWall);
        let (_, lat, _) = nearest_cross_section(&path, wall.x, wall.y);
        assert!(
            (lat + 6.0 + VERGE_OFFSET_M).abs() < 2.5,
            "wall at lateral {lat} m"
        );
    }

    #[test]
    fn buildings_keep_their_footprint_and_get_reseated() {
        let track = track();
        // 60 m out on a flat track, "floating" 10 m above the terrain.
        let mut scene = scene_with(&track, vec![prop(PropKind::Building, 350.0, 60.0, 10.0)]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.removed, 0);
        assert_eq!(report.walls, 0);
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
    fn armco_lines_the_straights_but_not_the_pit_side_or_corners() {
        let track = track();
        let mut scene = scene_with(&track, vec![]);
        groom_scene(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let pit = scene.pit_lane.clone().expect("pit lane generated");
        let lane = CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0).unwrap();

        let barriers = of_kind(&scene, PropKind::Barrier);
        assert!(barriers.len() > 100, "only {} barriers", barriers.len());
        let (mut left, mut right) = (0, 0);
        for b in &barriers {
            assert_eq!(b.asset, BARRIER_ASSET);
            assert_eq!(b.scale, 1.0);
            let (sample, lat, _) = nearest_cross_section(&path, b.x, b.y);
            assert!(
                (lat.abs() - half_width_on(&sample, lat) - BARRIER_OFFSET_M).abs() < 0.5,
                "barrier at lateral {lat}"
            );
            assert!(yaw_distance(b.yaw_rad, sample.heading_rad) < 0.05);
            assert!(
                !near_corner(&path, sample.station_m),
                "barrier in a corner at station {}",
                sample.station_m
            );
            let gap = lane_edge_gap(&lane, b.x, b.y);
            assert!(
                gap >= BARRIER_CLEAR_M - 0.1,
                "barrier {gap} m from the pit lane"
            );
            if lat > 0.0 {
                left += 1
            } else {
                right += 1
            }
        }
        // The pit lane takes the interior (left) side of the bottom
        // straight, so the left is guarded less than the right.
        assert!(left < right, "left {left}, right {right}");
    }

    #[test]
    fn tree_belts_keep_their_distance() {
        let track = track();
        let mut scene = scene_with(&track, vec![prop(PropKind::Grandstand, 350.0, 60.0, 0.0)]);
        groom_scene(&track, &mut scene).unwrap();
        let path = CenterlinePath::from_track(&track).unwrap();
        let stand = scene.props[0].clone();
        assert_eq!(stand.kind, PropKind::Grandstand);

        let trees = of_kind(&scene, PropKind::Tree);
        assert!(trees.len() > 200, "only {} trees", trees.len());
        for (i, t) in trees.iter().enumerate() {
            assert_eq!(t.asset, TREE_ASSET);
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
}
