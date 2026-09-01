//! Deterministic clean-up of auto-generated prop placement in a `.ats`
//! scene.
//!
//! The enrichment pass that populated the shipped scenes placed its props
//! against a flat world: tire walls ended up scattered 40–100 m into the
//! fields at road height, and buildings sit pinned to the road's elevation
//! even where the terrain has since fallen away. Grooming re-anchors all of
//! that against the same [`TerrainHeightfield`] the viewport and the bake
//! use:
//!
//! - **Tire walls and barriers** move to the outer edge of their station's
//!   prepared runoff (gravel / asphalt / sand / concrete patch when one is
//!   authored there, a fixed verge offset otherwise), aligned parallel to
//!   the track and seated on the blended ground.
//! - **Everything else** keeps its position and yaw and is only re-seated
//!   vertically on the blended ground.
//!
//! Grooming is idempotent: a second pass over a groomed scene changes
//! nothing, so it is safe to run over the whole track set at any time.

use crate::ats::{AtsScene, Prop, PropKind, Side, SurfaceKind};
use crate::terrain::TerrainHeightfield;
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

/// Rough circumscribed footprint radius of a prop stand-in at scale 1
/// (see `scene::prop_pieces` for the shapes). Walls and barriers are
/// exempt — they are deliberately laid tight against the runoff.
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
    /// Prop count before grooming.
    pub total: usize,
    /// The pit lane was regenerated with a different shape.
    pub pit_rebuilt: bool,
}

impl GroomReport {
    pub fn changed(&self) -> bool {
        self.pushed > 0
            || self.reseated > 0
            || self.removed > 0
            || self.walls_rebuilt
            || self.pit_rebuilt
    }
}

/// Lateral distance from the centerline a prop's center needs on the side
/// `lat` falls on, to keep its footprint off the road.
fn prop_lat_clearance(sample: &PathSample, lat: f32, radius: f32) -> f32 {
    let half_width = if lat >= 0.0 {
        sample.width_left_m
    } else {
        sample.width_right_m
    };
    half_width + PROP_CLEARANCE_M + radius
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
    let mut anchors: Vec<WallAnchor> = Vec::new();
    for mut prop in std::mem::take(&mut scene.props) {
        let (sample, lat, along) = nearest_cross_section(&path, prop.x, prop.y);

        match prop.kind {
            PropKind::TireWall | PropKind::Barrier => {
                // Walls are not groomed one by one: sparse dashes at a
                // uniform offset read as a phantom second circuit. They are
                // collected here and re-laid as continuous runs below.
                anchors.push(WallAnchor {
                    station_m: sample.station_m + along,
                    left: lat >= 0.0,
                    asset: prop.asset.clone(),
                });
                original_walls.push(prop);
            }
            _ => {
                let radius = prop_radius(prop.kind) * prop.scale;
                let (mut x, mut y, mut yaw) = (prop.x, prop.y, prop.yaw_rad);

                // Road clearance: the enrichment ringed props at a fixed
                // offset from *their* section, and where the circuit folds
                // that lands them on another one. Small furniture (cones,
                // lights) may legitimately hug the road and is exempt.
                if radius >= 1.0 {
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
                        // A building or grandstand near the lane is pit
                        // furniture: square it up with the lane.
                        if matches!(prop.kind, PropKind::Building | PropKind::Grandstand) {
                            let (ls, llat, _) = nearest_cross_section(lane, x, y);
                            if llat.abs() < PIT_BOX_ALIGN_RANGE_M {
                                yaw = ls.heading_rad;
                            }
                        }
                    }
                }

                // Seat on the ground that is actually there: the same
                // road-edge / terrain blend every ground band uses.
                let (fs, flat, _) = nearest_cross_section(&path, x, y);
                let pos = offset_point(&fs, flat);
                let z = surface_height(Some(&terrain), &fs, flat, (x, y, pos.2));

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
    if walls_match(&original_walls, &new_walls) {
        // Byte-identical placement: keep the original props (and their ids)
        // so re-grooming a groomed scene writes nothing.
        kept.extend(original_walls);
    } else {
        report.walls_rebuilt = true;
        scene.props = kept;
        for wall in new_walls {
            let id = scene.alloc_id();
            scene.props.push(Prop { id, ..wall });
        }
        return Some(report);
    }
    scene.props = kept;

    Some(report)
}

/// One kept wall's place along the course, before runs are laid.
struct WallAnchor {
    station_m: f32,
    left: bool,
    asset: String,
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
    use std::collections::BTreeMap;

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
            let side = if left { Side::Left } else { Side::Right };
            let sample = path.sample_at(cell_center(idx));
            let beyond_edge = wall_offset(path, surfaces, &sample, side);
            let half_width = match side {
                Side::Left => sample.width_left_m,
                Side::Right => sample.width_right_m,
            };
            let target_lat = match side {
                Side::Left => half_width + beyond_edge,
                Side::Right => -(half_width + beyond_edge),
            };
            let pos = offset_point(&sample, target_lat);
            let z = surface_height(Some(terrain), &sample, target_lat, pos);
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

/// Whether the freshly laid walls already match the existing ones, ids
/// aside — the signal that a groomed scene needs no rewrite.
fn walls_match(old: &[Prop], new: &[Prop]) -> bool {
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
            && a.asset == b.asset
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
        .filter(|s| {
            s.side == side
                && matches!(
                    s.kind,
                    SurfaceKind::Gravel
                        | SurfaceKind::AsphaltRunoff
                        | SurfaceKind::Sand
                        | SurfaceKind::Concrete
                )
        })
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
    use std::f32::consts::{PI, TAU};
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

    /// A flat rounded rectangle with genuinely straight straights (dense
    /// collinear nodes) and tight corners, so the corner test in
    /// `near_corner` has something unambiguous to measure.
    fn track() -> TrackFile {
        let mut nodes = Vec::new();
        for i in 0..=7 {
            nodes.push(node(i as f32 * 100.0, 0.0, 0.0));
        }
        nodes.push(node(800.0, 100.0, 0.0));
        for i in (0..=7).rev() {
            nodes.push(node(i as f32 * 100.0, 200.0, 0.0));
        }
        nodes.push(node(-100.0, 100.0, 0.0));
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

    #[test]
    fn walls_on_straights_are_removed() {
        let track = track();
        // Mid-straight, 40 m out: exactly the ring clutter the enrichment
        // left everywhere.
        let mut scene = scene_with(&track, vec![prop(PropKind::TireWall, 350.0, 40.0, 0.0)]);
        let report = groom_props(&track, &mut scene).unwrap();
        assert_eq!(report.removed, 1);
        assert!(scene.props.is_empty(), "straight wall survived");
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
        // course, parallel to it.
        let path = CenterlinePath::from_track(&track).unwrap();
        let wall = &scene.props[0];
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
        let (s, lat, _) = nearest_cross_section(&path, b.x, b.y);
        let needed = prop_lat_clearance(&s, lat, prop_radius(PropKind::Building));
        assert!(lat.abs() >= needed - 0.1, "building at lateral {lat} m");
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
        assert!(groom_props(&track, &mut scene).unwrap().changed());
        let after_first = scene.clone();
        let second = groom_props(&track, &mut scene).unwrap();
        assert!(!second.changed(), "second pass changed: {second:?}");
        assert_eq!(scene, after_first);
    }
}
