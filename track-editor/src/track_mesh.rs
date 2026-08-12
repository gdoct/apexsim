//! Builds a renderable ribbon mesh from a [`TrackFile`]'s centerline.
//!
//! This is a preview mesh for the editor viewport, not the authoritative
//! server geometry — but the sampling (Catmull-Rom spline, adaptive point
//! spacing, per-point width/banking/surface inherited from the segment's
//! start node) mirrors `server/src/track_loader.rs::SplineInterpolator` so
//! what you see here matches what the server will actually load.

use bevy::asset::RenderAssetUsages;
use bevy::mesh::{Indices, Mesh, PrimitiveTopology};
use bevy::prelude::Vec3;

use crate::coords;
use crate::track_data::{TrackFile, TrackNode};

/// A single sampled cross-section of the centerline.
struct Sample {
    pos: (f32, f32, f32),
    width_left: f32,
    width_right: f32,
    banking_rad: f32,
    heading_rad: f32,
    surface_color: [f32; 4],
}

const TARGET_POINT_SPACING_M: f32 = 2.0;
const MIN_POINTS_PER_SEGMENT: usize = 2;
const MAX_POINTS_PER_SEGMENT: usize = 30;

/// Build a triangle-strip-shaped ribbon mesh spanning the track's width,
/// with vertex colors keyed by each segment's surface type.
///
/// Returns `None` if there are too few nodes to form a segment.
pub fn build_ribbon_mesh(track: &TrackFile) -> Option<Mesh> {
    let samples = sample_centerline(track);
    if samples.len() < 2 {
        return None;
    }

    let mut positions: Vec<[f32; 3]> = Vec::with_capacity(samples.len() * 2);
    let mut normals: Vec<[f32; 3]> = Vec::with_capacity(samples.len() * 2);
    let mut colors: Vec<[f32; 4]> = Vec::with_capacity(samples.len() * 2);
    let mut uvs: Vec<[f32; 2]> = Vec::with_capacity(samples.len() * 2);

    for sample in &samples {
        let (left, right) = edge_points(sample);
        positions.push(coords::to_bevy(left.0, left.1, left.2).to_array());
        positions.push(coords::to_bevy(right.0, right.1, right.2).to_array());
        normals.push([0.0, 1.0, 0.0]);
        normals.push([0.0, 1.0, 0.0]);
        colors.push(sample.surface_color);
        colors.push(sample.surface_color);
        let u = sample.pos.0.hypot(sample.pos.1); // cheap, not arc length; fine for a preview
        uvs.push([u, 0.0]);
        uvs.push([u, 1.0]);
    }

    let mut indices: Vec<u32> = Vec::with_capacity((samples.len() - 1) * 6);
    let segment_count = if track.closed_loop {
        samples.len()
    } else {
        samples.len() - 1
    };
    for i in 0..segment_count {
        let i0 = (i * 2) as u32;
        let i1 = i0 + 1;
        let next = (i + 1) % samples.len();
        let i2 = (next * 2) as u32;
        let i3 = i2 + 1;
        // Two triangles per quad, wound so the top face (+Y normal) is visible.
        indices.extend_from_slice(&[i0, i1, i2, i1, i3, i2]);
    }

    let mesh = Mesh::new(
        PrimitiveTopology::TriangleList,
        RenderAssetUsages::RENDER_WORLD | RenderAssetUsages::MAIN_WORLD,
    )
    .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, positions)
    .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, normals)
    .with_inserted_attribute(Mesh::ATTRIBUTE_COLOR, colors)
    .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, uvs)
    .with_inserted_indices(Indices::U32(indices));

    Some(mesh)
}

/// Left/right edge positions (track space) for a cross-section, banking
/// applied as a vertical offset of the outer/inner edges around the
/// centerline (positive banking lifts the left edge, per the server's
/// "positive = banked towards inside" convention with `+Y` = left).
fn edge_points(sample: &Sample) -> ((f32, f32, f32), (f32, f32, f32)) {
    let (sin_h, cos_h) = sample.heading_rad.sin_cos();
    // Left-normal in the track XY plane: heading rotated +90 degrees.
    let (nx, ny) = (-sin_h, cos_h);
    let bank_sin = sample.banking_rad.sin();

    let left = (
        sample.pos.0 + nx * sample.width_left,
        sample.pos.1 + ny * sample.width_left,
        sample.pos.2 + sample.width_left * bank_sin,
    );
    let right = (
        sample.pos.0 - nx * sample.width_right,
        sample.pos.1 - ny * sample.width_right,
        sample.pos.2 - sample.width_right * bank_sin,
    );
    (left, right)
}

fn surface_color(surface_type: Option<&str>) -> [f32; 4] {
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

fn sample_centerline(track: &TrackFile) -> Vec<Sample> {
    let nodes = &track.nodes;
    if nodes.len() < 2 {
        return Vec::new();
    }

    let mut samples = Vec::new();
    let node_count = nodes.len();
    let last_index = if track.closed_loop {
        node_count
    } else {
        node_count - 1
    };

    for i in 0..last_index {
        let p0 = control_point(nodes, i, -1, track.closed_loop);
        let p1 = &nodes[i];
        let p2 = control_point(nodes, i, 1, track.closed_loop);
        let p3 = control_point(nodes, i, 2, track.closed_loop);

        let chord_len = ((p2.x - p1.x).powi(2) + (p2.y - p1.y).powi(2)).sqrt();
        let points_per_segment = ((chord_len / TARGET_POINT_SPACING_M).ceil() as usize)
            .clamp(MIN_POINTS_PER_SEGMENT, MAX_POINTS_PER_SEGMENT);

        let (width_left, width_right) = resolve_width(p1, track.default_width);
        let color = surface_color(p1.surface_type.as_deref());
        let banking_rad = p1.banking.unwrap_or(0.0);

        for j in 0..points_per_segment {
            let t = j as f32 / points_per_segment as f32;
            let pos = catmull_rom(p0, p1, p2, p3, t);
            samples.push(Sample {
                pos,
                width_left,
                width_right,
                banking_rad,
                heading_rad: 0.0, // filled in below, once neighbors are known
                surface_color: color,
            });
        }
    }

    if !track.closed_loop {
        // Close the sampled centerline with the final node itself so the
        // last segment doesn't fall a hair short of it.
        let last = nodes.last().unwrap();
        let (width_left, width_right) = resolve_width(last, track.default_width);
        samples.push(Sample {
            pos: (last.x, last.y, last.z),
            width_left,
            width_right,
            banking_rad: last.banking.unwrap_or(0.0),
            heading_rad: 0.0,
            surface_color: surface_color(last.surface_type.as_deref()),
        });
    }

    compute_headings(&mut samples, track.closed_loop);
    samples
}

fn control_point(nodes: &[TrackNode], i: usize, offset: i32, closed_loop: bool) -> &TrackNode {
    let len = nodes.len() as i32;
    let idx = i as i32 + offset;
    let idx = if closed_loop {
        idx.rem_euclid(len)
    } else {
        idx.clamp(0, len - 1)
    };
    &nodes[idx as usize]
}

fn resolve_width(node: &TrackNode, default_width: f32) -> (f32, f32) {
    if let (Some(wl), Some(wr)) = (node.width_left, node.width_right) {
        (wl, wr)
    } else if let Some(w) = node.width {
        (w / 2.0, w / 2.0)
    } else {
        (default_width / 2.0, default_width / 2.0)
    }
}

fn catmull_rom(
    p0: &TrackNode,
    p1: &TrackNode,
    p2: &TrackNode,
    p3: &TrackNode,
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
        blend(p0.x, p1.x, p2.x, p3.x),
        blend(p0.y, p1.y, p2.y, p3.y),
        blend(p0.z, p1.z, p2.z, p3.z),
    )
}

fn compute_headings(samples: &mut [Sample], closed_loop: bool) {
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

/// Position of a single node in Bevy space, for spawning/updating the
/// draggable node handle entities (see `scene.rs`).
pub fn node_bevy_position(node: &TrackNode) -> Vec3 {
    coords::to_bevy(node.x, node.y, node.z)
}
