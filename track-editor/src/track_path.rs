//! Arc-length ("station") parameterization of a track centerline.
//!
//! Scene elements in a `.ats` file anchor to the track by *station*:
//! distance in meters along the sampled centerline, plus a lateral offset
//! (positive = left). [`CenterlinePath`] samples a [`TrackFile`] once — the
//! same Catmull-Rom spline and adaptive spacing as
//! `server/src/track_loader.rs::SplineInterpolator` — and then answers
//! `station -> pose` queries by interpolating between samples. Everything
//! stays in track space (right-handed, `+Z` up, meters).

use crate::track_data::TrackFile;

/// One sampled cross-section of a centerline.
#[derive(Debug, Clone, Copy)]
pub struct PathSample {
    /// Cumulative distance from the start of the path, meters (2D, in the
    /// track XY plane).
    pub station_m: f32,
    /// Centerline position, track space.
    pub pos: (f32, f32, f32),
    /// Course heading, CCW from +X, radians.
    pub heading_rad: f32,
    pub width_left_m: f32,
    pub width_right_m: f32,
    pub banking_rad: f32,
    /// Preview color for the surface type at this sample (linear RGBA).
    pub surface_color: [f32; 4],
}

pub struct CenterlinePath {
    samples: Vec<PathSample>,
    total_len_m: f32,
    closed: bool,
}

const TARGET_POINT_SPACING_M: f32 = 2.0;
const MIN_POINTS_PER_SEGMENT: usize = 2;
const MAX_POINTS_PER_SEGMENT: usize = 30;

impl CenterlinePath {
    /// Sample a logical track's centerline. Returns `None` for degenerate
    /// tracks (fewer than 2 nodes or zero length).
    pub fn from_track(track: &TrackFile) -> Option<Self> {
        let nodes = &track.nodes;
        if nodes.len() < 2 {
            return None;
        }

        let positions: Vec<(f32, f32, f32)> = nodes.iter().map(|n| (n.x, n.y, n.z)).collect();
        let mut samples = Vec::new();
        let last_index = if track.closed_loop {
            nodes.len()
        } else {
            nodes.len() - 1
        };

        for i in 0..last_index {
            let p0 = control_point(&positions, i, -1, track.closed_loop);
            let p1 = positions[i];
            let p2 = control_point(&positions, i, 1, track.closed_loop);
            let p3 = control_point(&positions, i, 2, track.closed_loop);

            let chord_len = ((p2.0 - p1.0).powi(2) + (p2.1 - p1.1).powi(2)).sqrt();
            let points_per_segment = ((chord_len / TARGET_POINT_SPACING_M).ceil() as usize)
                .clamp(MIN_POINTS_PER_SEGMENT, MAX_POINTS_PER_SEGMENT);

            let node = &nodes[i];
            let (width_left_m, width_right_m) = node.resolved_half_widths(track.default_width);
            let banking_rad = node.banking.unwrap_or(0.0);
            let color = surface_color(node.surface_type.as_deref());

            for j in 0..points_per_segment {
                let t = j as f32 / points_per_segment as f32;
                samples.push(PathSample {
                    station_m: 0.0, // filled in below
                    pos: catmull_rom(p0, p1, p2, p3, t),
                    heading_rad: 0.0, // filled in below
                    width_left_m,
                    width_right_m,
                    banking_rad,
                    surface_color: color,
                });
            }
        }

        if !track.closed_loop {
            // Close the sampled centerline with the final node itself so the
            // last segment doesn't fall a hair short of it.
            let last = nodes.last().unwrap();
            let (width_left_m, width_right_m) = last.resolved_half_widths(track.default_width);
            samples.push(PathSample {
                station_m: 0.0,
                pos: (last.x, last.y, last.z),
                heading_rad: 0.0,
                width_left_m,
                width_right_m,
                banking_rad: last.banking.unwrap_or(0.0),
                surface_color: surface_color(last.surface_type.as_deref()),
            });
        }

        Self::finish(samples, track.closed_loop)
    }

    /// Sample a free-standing polyline (e.g. the pit lane) with a constant
    /// width and no banking, through the same Catmull-Rom spline.
    pub fn from_polyline(points: &[[f32; 3]], half_width_m: f32) -> Option<Self> {
        if points.len() < 2 {
            return None;
        }
        let positions: Vec<(f32, f32, f32)> = points.iter().map(|p| (p[0], p[1], p[2])).collect();
        let mut samples = Vec::new();
        let color = [0.32, 0.32, 0.34, 1.0];

        for i in 0..positions.len() - 1 {
            let p0 = control_point(&positions, i, -1, false);
            let p1 = positions[i];
            let p2 = control_point(&positions, i, 1, false);
            let p3 = control_point(&positions, i, 2, false);

            let chord_len = ((p2.0 - p1.0).powi(2) + (p2.1 - p1.1).powi(2)).sqrt();
            let points_per_segment = ((chord_len / TARGET_POINT_SPACING_M).ceil() as usize)
                .clamp(MIN_POINTS_PER_SEGMENT, MAX_POINTS_PER_SEGMENT);

            for j in 0..points_per_segment {
                let t = j as f32 / points_per_segment as f32;
                samples.push(PathSample {
                    station_m: 0.0,
                    pos: catmull_rom(p0, p1, p2, p3, t),
                    heading_rad: 0.0,
                    width_left_m: half_width_m,
                    width_right_m: half_width_m,
                    banking_rad: 0.0,
                    surface_color: color,
                });
            }
        }
        let last = positions.last().unwrap();
        samples.push(PathSample {
            station_m: 0.0,
            pos: *last,
            heading_rad: 0.0,
            width_left_m: half_width_m,
            width_right_m: half_width_m,
            banking_rad: 0.0,
            surface_color: color,
        });

        Self::finish(samples, false)
    }

    fn finish(mut samples: Vec<PathSample>, closed: bool) -> Option<Self> {
        if samples.len() < 2 {
            return None;
        }
        compute_headings(&mut samples, closed);

        let mut station = 0.0;
        for i in 0..samples.len() {
            samples[i].station_m = station;
            if i + 1 < samples.len() {
                station += horizontal_distance(samples[i].pos, samples[i + 1].pos);
            }
        }
        let total_len_m = if closed {
            station + horizontal_distance(samples[samples.len() - 1].pos, samples[0].pos)
        } else {
            station
        };
        if total_len_m <= 0.0 {
            return None;
        }

        Some(Self {
            samples,
            total_len_m,
            closed,
        })
    }

    pub fn total_length_m(&self) -> f32 {
        self.total_len_m
    }

    pub fn is_closed(&self) -> bool {
        self.closed
    }

    pub fn samples(&self) -> &[PathSample] {
        &self.samples
    }

    /// The interpolated cross-section at `station_m`. Wraps on closed loops;
    /// clamps to the ends on open paths.
    pub fn sample_at(&self, station_m: f32) -> PathSample {
        let s = if self.closed {
            station_m.rem_euclid(self.total_len_m)
        } else {
            station_m.clamp(0.0, self.total_len_m)
        };

        // Index of the last sample whose station is <= s.
        let idx = match self
            .samples
            .binary_search_by(|sample| sample.station_m.partial_cmp(&s).unwrap())
        {
            Ok(i) => i,
            Err(0) => 0,
            Err(i) => i - 1,
        };

        let a = self.samples[idx];
        let (b, seg_len) = if idx + 1 < self.samples.len() {
            let b = self.samples[idx + 1];
            (b, b.station_m - a.station_m)
        } else if self.closed {
            (self.samples[0], self.total_len_m - a.station_m)
        } else {
            return a;
        };
        if seg_len <= f32::EPSILON {
            return a;
        }
        let t = ((s - a.station_m) / seg_len).clamp(0.0, 1.0);

        let lerp = |x: f32, y: f32| x + (y - x) * t;
        // Interpolate heading via its direction vector so the wrap at ±pi
        // doesn't produce a bogus intermediate angle.
        let (sin_a, cos_a) = a.heading_rad.sin_cos();
        let (sin_b, cos_b) = b.heading_rad.sin_cos();
        let heading_rad = lerp(sin_a, sin_b).atan2(lerp(cos_a, cos_b));

        PathSample {
            station_m: s,
            pos: (
                lerp(a.pos.0, b.pos.0),
                lerp(a.pos.1, b.pos.1),
                lerp(a.pos.2, b.pos.2),
            ),
            heading_rad,
            width_left_m: lerp(a.width_left_m, b.width_left_m),
            width_right_m: lerp(a.width_right_m, b.width_right_m),
            banking_rad: lerp(a.banking_rad, b.banking_rad),
            surface_color: a.surface_color,
        }
    }
}

/// Signed curvature at a station, 1/m. Positive means the course turns
/// left. Taken as the rate of change of heading over arc length; on an open
/// path `sample_at` clamps at the ends, which only flattens the estimate.
pub fn curvature_at(path: &CenterlinePath, station_m: f32) -> f32 {
    use std::f32::consts::{PI, TAU};
    // Wide enough to average out the heading jitter of digitized nodes,
    // which sit about 2 m apart. A short window turns that jitter into
    // spikes of false curvature and clamps perfectly straight sections.
    const DELTA_M: f32 = 8.0;
    let behind = path.sample_at(station_m - DELTA_M);
    let ahead = path.sample_at(station_m + DELTA_M);
    let mut delta = ahead.heading_rad - behind.heading_rad;
    // Unwrap across ±pi, or a corner straddling the branch cut reads as an
    // enormous curvature.
    while delta > PI {
        delta -= TAU;
    }
    while delta < -PI {
        delta += TAU;
    }
    delta / (2.0 * DELTA_M)
}

/// Point at `lateral_m` (positive = left) from a cross-section's centerline,
/// banking applied as the same vertical shear the track ribbon uses.
pub fn offset_point(sample: &PathSample, lateral_m: f32) -> (f32, f32, f32) {
    let (sin_h, cos_h) = sample.heading_rad.sin_cos();
    // Left-normal in the track XY plane: heading rotated +90 degrees.
    let (nx, ny) = (-sin_h, cos_h);
    let bank_sin = sample.banking_rad.sin();
    (
        sample.pos.0 + nx * lateral_m,
        sample.pos.1 + ny * lateral_m,
        sample.pos.2 + lateral_m * bank_sin,
    )
}

pub fn surface_color(surface_type: Option<&str>) -> [f32; 4] {
    match surface_type.map(str::to_ascii_lowercase).as_deref() {
        Some("curb") => [0.75, 0.15, 0.1, 1.0],
        Some("grass") => [0.16, 0.45, 0.14, 1.0],
        Some("gravel") => [0.62, 0.55, 0.4, 1.0],
        Some("sand") => [0.76, 0.68, 0.42, 1.0],
        Some("wet") => [0.3, 0.34, 0.4, 1.0],
        Some("concrete") => [0.55, 0.55, 0.55, 1.0],
        _ => [0.24, 0.24, 0.26, 1.0], // asphalt (default)
    }
}

fn horizontal_distance(a: (f32, f32, f32), b: (f32, f32, f32)) -> f32 {
    (b.0 - a.0).hypot(b.1 - a.1)
}

fn control_point(
    positions: &[(f32, f32, f32)],
    i: usize,
    offset: i32,
    closed_loop: bool,
) -> (f32, f32, f32) {
    let len = positions.len() as i32;
    let idx = i as i32 + offset;
    let idx = if closed_loop {
        idx.rem_euclid(len)
    } else {
        idx.clamp(0, len - 1)
    };
    positions[idx as usize]
}

fn catmull_rom(
    p0: (f32, f32, f32),
    p1: (f32, f32, f32),
    p2: (f32, f32, f32),
    p3: (f32, f32, f32),
    t: f32,
) -> (f32, f32, f32) {
    let t2 = t * t;
    let t3 = t2 * t;
    let blend = |a: f32, b: f32, c: f32, d: f32| -> f32 {
        0.5 * ((2.0 * b)
            + (-a + c) * t
            + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2
            + (-a + 3.0 * b - 3.0 * c + d) * t3)
    };
    (
        blend(p0.0, p1.0, p2.0, p3.0),
        blend(p0.1, p1.1, p2.1, p3.1),
        blend(p0.2, p1.2, p2.2, p3.2),
    )
}

fn compute_headings(samples: &mut [PathSample], closed_loop: bool) {
    let len = samples.len();
    if len < 2 {
        return;
    }
    for i in 0..len {
        let next = if i + 1 < len {
            Some(i + 1)
        } else if closed_loop {
            Some(0)
        } else {
            None
        };
        samples[i].heading_rad = match next {
            Some(next_i) => {
                let dx = samples[next_i].pos.0 - samples[i].pos.0;
                let dy = samples[next_i].pos.1 - samples[i].pos.1;
                dy.atan2(dx)
            }
            None => samples[i - 1].heading_rad,
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{TrackFile, TrackNode};

    fn node(x: f32, y: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z: 0.0,
            width: None,
            width_left: None,
            width_right: None,
            banking: None,
            friction: None,
            surface_type: None,
        }
    }

    fn square_loop() -> TrackFile {
        TrackFile {
            name: "Square".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0),
                node(100.0, 0.0),
                node(100.0, 100.0),
                node(0.0, 100.0),
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        }
    }

    #[test]
    fn square_loop_length_is_plausible() {
        let path = CenterlinePath::from_track(&square_loop()).unwrap();
        // A Catmull-Rom through a 100 m square bulges outward a bit; the
        // total must be at least the polygon perimeter and not wildly more.
        assert!(path.total_length_m() > 390.0, "{}", path.total_length_m());
        assert!(path.total_length_m() < 500.0, "{}", path.total_length_m());
    }

    #[test]
    fn sample_at_wraps_on_closed_loops() {
        let path = CenterlinePath::from_track(&square_loop()).unwrap();
        let total = path.total_length_m();
        let a = path.sample_at(10.0);
        let b = path.sample_at(10.0 + total);
        assert!((a.pos.0 - b.pos.0).abs() < 1e-3);
        assert!((a.pos.1 - b.pos.1).abs() < 1e-3);
    }

    #[test]
    fn stations_are_monotonic() {
        let path = CenterlinePath::from_track(&square_loop()).unwrap();
        for w in path.samples().windows(2) {
            assert!(w[1].station_m > w[0].station_m);
        }
    }

    #[test]
    fn offset_point_moves_left_of_heading() {
        let sample = PathSample {
            station_m: 0.0,
            pos: (0.0, 0.0, 0.0),
            heading_rad: 0.0, // heading +X, so left is +Y
            width_left_m: 5.0,
            width_right_m: 5.0,
            banking_rad: 0.0,
            surface_color: [0.0; 4],
        };
        let (x, y, z) = offset_point(&sample, 3.0);
        assert!((x - 0.0).abs() < 1e-6);
        assert!((y - 3.0).abs() < 1e-6);
        assert!(z.abs() < 1e-6);
    }

    #[test]
    fn polyline_path_has_constant_width() {
        let path =
            CenterlinePath::from_polyline(&[[0.0, 0.0, 0.0], [50.0, 0.0, 0.0]], 4.0).unwrap();
        let s = path.sample_at(path.total_length_m() / 2.0);
        assert_eq!(s.width_left_m, 4.0);
        assert_eq!(s.width_right_m, 4.0);
        assert!(!path.is_closed());
    }
}
