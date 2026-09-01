//! Deterministic pit-lane generation from the track's own geometry.
//!
//! The shipped scenes carry pit lanes that run beside the start/finish
//! straight but never actually join the road: the polyline ends hang a few
//! meters off the track edge, so there is no entry, no exit, and the lane
//! reads as a stray ribbon. This rebuilds the whole lane from scratch:
//!
//! ```text
//!   track ══════════════════════════════════════════════════ ▶ course
//!            ╲ entry                                exit ╱
//!             ╲__ pit road, parallel to the straight ___╱
//! ```
//!
//! - The **pit road** runs parallel to the longest low-curvature stretch
//!   around the start/finish line, offset one apron past the track edge.
//! - The **entry** peels off the road edge before that stretch, the
//!   **exit** merges back onto it after — both ends overlap the road
//!   slightly so the surfaces visibly connect.
//! - Every node takes the track's height at its station, so the lane
//!   follows the road over crests instead of its own flat plane.
//!
//! Generation is a pure function of the centerline (plus the lane's
//! existing width/boxes/speed limit, which are preserved), so re-running
//! it is idempotent.

use crate::ats::PitLane;
use crate::track_path::{curvature_at, offset_point, CenterlinePath};

/// Curvature below which the course still counts as "straight", 1/m.
const STRAIGHT_KAPPA: f32 = 1.0 / 300.0;
/// How far the straight search extends either side of start/finish.
const SEARCH_M: f32 = 400.0;
/// The parallel pit road keeps this much clearance from the straight's
/// ends, so the tapers happen on the straight, not in the corners.
const END_CLEARANCE_M: f32 = 30.0;
/// Shortest / longest parallel pit road worth building.
const MIN_ROAD_M: f32 = 120.0;
const MAX_ROAD_M: f32 = 400.0;
/// Length of the entry / exit tapers along the course.
const TAPER_M: f32 = 80.0;
/// Gap between the track edge and the pit road's inner edge.
const APRON_M: f32 = 2.0;
/// How far the entry/exit ends reach onto the road, so the lane and the
/// road surfaces overlap instead of showing a seam.
const MERGE_OVERLAP_M: f32 = 1.5;
/// Node spacing along the parallel section.
const NODE_SPACING_M: f32 = 30.0;

const DEFAULT_WIDTH_M: f32 = 10.0;
const DEFAULT_SPEED_LIMIT_KMH: f32 = 80.0;

/// Rebuild the pit lane for `track`'s centerline. `existing` donates its
/// width, box count, speed limit and — importantly — its side of the
/// track; a track without a pit lane gets defaults and the interior side.
///
/// Returns `None` only when the path is degenerate.
pub fn generate_pit_lane(path: &CenterlinePath, existing: Option<&PitLane>) -> Option<PitLane> {
    let total = path.total_length_m();
    if !(total.is_finite() && total > 2.0 * (MIN_ROAD_M + TAPER_M)) {
        return None;
    }

    // The straight around start/finish: expand both ways while the course
    // stays straight enough, clamped to the search window.
    let mut behind = 0.0f32;
    while behind < SEARCH_M && curvature_at(path, -(behind + 10.0)).abs() < STRAIGHT_KAPPA {
        behind += 10.0;
    }
    let mut ahead = 0.0f32;
    while ahead < SEARCH_M && curvature_at(path, ahead + 10.0).abs() < STRAIGHT_KAPPA {
        ahead += 10.0;
    }

    // The parallel road within it. The tapers must fit inside the straight
    // too — an entry placed beyond it lands in the corner before
    // start/finish and the lane ends up cutting across the road. When the
    // straight is too short for road + tapers (street circuits), take a
    // fixed span and let the lane follow the gentle curve.
    let (mut road_start, mut road_end) = (
        -(behind - END_CLEARANCE_M - TAPER_M).max(0.0),
        (ahead - END_CLEARANCE_M - TAPER_M).max(0.0),
    );
    if road_end - road_start < MIN_ROAD_M {
        road_start = -MIN_ROAD_M * 0.75;
        road_end = MIN_ROAD_M * 0.75;
    }
    road_start = road_start.max(-MAX_ROAD_M * 0.6);
    road_end = road_end.min(MAX_ROAD_M * 0.6);

    let side = existing.map_or_else(|| interior_side(path), |pit| lane_side(path, pit));
    let width = existing.map_or(DEFAULT_WIDTH_M, |p| p.width_m.clamp(8.0, 14.0));

    // Lateral offsets, signed (positive = left).
    let road_lat = |s: f32| {
        let sample = path.sample_at(s);
        let half = match side {
            1 => sample.width_left_m,
            _ => sample.width_right_m,
        };
        side as f32 * (half + APRON_M + width / 2.0)
    };
    let merge_lat = |s: f32| {
        let sample = path.sample_at(s);
        let half = match side {
            1 => sample.width_left_m,
            _ => sample.width_right_m,
        };
        side as f32 * (half - MERGE_OVERLAP_M)
    };

    let node_at = |s: f32, lat: f32| {
        let sample = path.sample_at(s);
        let p = offset_point(&sample, lat);
        [p.0, p.1, p.2]
    };

    let mut nodes: Vec<[f32; 3]> = Vec::new();
    // Entry: on the road, then halfway out, then on the pit road.
    nodes.push(node_at(
        road_start - TAPER_M,
        merge_lat(road_start - TAPER_M),
    ));
    let entry_mid = road_start - TAPER_M / 2.0;
    nodes.push(node_at(
        entry_mid,
        (merge_lat(entry_mid) + road_lat(entry_mid)) / 2.0,
    ));

    // The parallel pit road.
    let span = road_end - road_start;
    let steps = ((span / NODE_SPACING_M).ceil() as usize).max(1);
    for i in 0..=steps {
        let s = road_start + span * (i as f32 / steps as f32);
        nodes.push(node_at(s, road_lat(s)));
    }

    // Exit: mirror of the entry.
    let exit_mid = road_end + TAPER_M / 2.0;
    nodes.push(node_at(
        exit_mid,
        (merge_lat(exit_mid) + road_lat(exit_mid)) / 2.0,
    ));
    nodes.push(node_at(road_end + TAPER_M, merge_lat(road_end + TAPER_M)));

    let box_count = existing.map_or_else(
        || ((span / 12.0) as u32).clamp(8, 32),
        |p| p.box_count.clamp(4, 40),
    );
    let speed_limit_kmh = existing.map_or(DEFAULT_SPEED_LIMIT_KMH, |p| {
        p.speed_limit_kmh.clamp(30.0, 120.0)
    });

    Some(PitLane {
        nodes,
        width_m: width,
        box_count,
        speed_limit_kmh,
    })
}

/// Which side of the track an existing lane sits on: +1 left, -1 right,
/// decided by majority vote of its nodes.
fn lane_side(path: &CenterlinePath, pit: &PitLane) -> i32 {
    let mut left = 0i32;
    for n in &pit.nodes {
        let (sample, _) = nearest(path, n[0], n[1]);
        let (sin_h, cos_h) = sample.heading_rad.sin_cos();
        let lat = -sin_h * (n[0] - sample.pos.0) + cos_h * (n[1] - sample.pos.1);
        left += if lat >= 0.0 { 1 } else { -1 };
    }
    if left >= 0 {
        1
    } else {
        -1
    }
}

/// The circuit's interior side at start/finish: +1 when the lap runs
/// counter-clockwise (interior on the left), -1 clockwise. Open point-to-
/// point tracks default to the right.
fn interior_side(path: &CenterlinePath) -> i32 {
    if !path.is_closed() {
        return -1;
    }
    let samples = path.samples();
    let mut doubled_area = 0.0f32;
    for i in 0..samples.len() {
        let a = samples[i].pos;
        let b = samples[(i + 1) % samples.len()].pos;
        doubled_area += a.0 * b.1 - b.0 * a.1;
    }
    if doubled_area >= 0.0 {
        1
    } else {
        -1
    }
}

fn nearest(path: &CenterlinePath, x: f32, y: f32) -> (crate::track_path::PathSample, f32) {
    let sample = path
        .samples()
        .iter()
        .min_by(|a, b| {
            let da = (a.pos.0 - x).powi(2) + (a.pos.1 - y).powi(2);
            let db = (b.pos.0 - x).powi(2) + (b.pos.1 - y).powi(2);
            da.partial_cmp(&db).expect("finite sample distances")
        })
        .copied()
        .expect("CenterlinePath always has samples");
    let d = (sample.pos.0 - x).hypot(sample.pos.1 - y);
    (sample, d)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{TrackFile, TrackNode};

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

    /// A rounded-rectangle circuit with a ~600 m bottom straight through
    /// start/finish and a hill on the far side. Dense nodes keep the spline
    /// close to the intended shape.
    fn circuit() -> TrackFile {
        let mut nodes = Vec::new();
        for i in 0..=6 {
            nodes.push(node(i as f32 * 100.0, 0.0, 0.0)); // bottom straight +X
        }
        nodes.push(node(700.0, 100.0, 5.0));
        for i in (0..=6).rev() {
            nodes.push(node(i as f32 * 100.0, 200.0, 10.0)); // top straight -X
        }
        nodes.push(node(-100.0, 100.0, 5.0));
        TrackFile {
            name: "Pit".to_string(),
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

    fn path() -> CenterlinePath {
        CenterlinePath::from_track(&circuit()).unwrap()
    }

    #[test]
    fn lane_ends_reach_onto_the_road_and_middle_stays_clear() {
        let path = path();
        let pit = generate_pit_lane(&path, None).unwrap();
        assert!(pit.nodes.len() >= 6);

        let lateral = |n: &[f32; 3]| {
            let (sample, _) = nearest(&path, n[0], n[1]);
            let (sin_h, cos_h) = sample.heading_rad.sin_cos();
            (
                (-sin_h * (n[0] - sample.pos.0) + cos_h * (n[1] - sample.pos.1)).abs(),
                sample.width_left_m.max(sample.width_right_m),
            )
        };

        let (first, half) = lateral(pit.nodes.first().unwrap());
        let (last, _) = lateral(pit.nodes.last().unwrap());
        assert!(
            first < half,
            "entry hangs {first} m out (half width {half})"
        );
        assert!(last < half, "exit hangs {last} m out");

        // Middle of the lane: a full lane width clear of the road edge.
        let mid = &pit.nodes[pit.nodes.len() / 2];
        let (mid_lat, half) = lateral(mid);
        assert!(
            mid_lat > half + 2.0,
            "pit road at lateral {mid_lat}, half width {half}"
        );
    }

    #[test]
    fn lane_keeps_the_existing_side_width_and_boxes() {
        let path = path();
        let generated = generate_pit_lane(&path, None).unwrap();

        // Flip the donor lane to the other side of the track.
        let mut flipped = generated.clone();
        for n in &mut flipped.nodes {
            n[1] = -n[1];
        }
        flipped.width_m = 11.0;
        flipped.box_count = 20;

        let regenerated = generate_pit_lane(&path, Some(&flipped)).unwrap();
        assert_eq!(regenerated.width_m, 11.0);
        assert_eq!(regenerated.box_count, 20);
        // The bottom straight runs along y = 0; a right-side lane sits at
        // negative y, matching the flipped donor.
        let mid = regenerated.nodes[regenerated.nodes.len() / 2];
        assert!(mid[1] < 0.0, "lane did not keep the donor's side: {mid:?}");
    }

    #[test]
    fn lane_follows_the_road_height() {
        let path = path();
        let pit = generate_pit_lane(&path, None).unwrap();
        for n in &pit.nodes {
            let (sample, _) = nearest(&path, n[0], n[1]);
            assert!(
                (n[2] - sample.pos.2).abs() < 1.0,
                "node {n:?} vs road z {}",
                sample.pos.2
            );
        }
    }

    #[test]
    fn generation_is_deterministic_and_idempotent() {
        let path = path();
        let first = generate_pit_lane(&path, None).unwrap();
        let second = generate_pit_lane(&path, Some(&first)).unwrap();
        assert_eq!(first, second);
    }
}
