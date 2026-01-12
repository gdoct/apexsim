use bevy::prelude::*;
use bevy::render::mesh::{Indices, PrimitiveTopology};
use crate::track_data::TrackNode;

/// Generate a Bevy mesh from track nodes using Catmull-Rom spline interpolation
pub fn generate_track_mesh(nodes: &[TrackNode], closed_loop: bool) -> Mesh {
    if nodes.len() < 2 {
        return Mesh::new(PrimitiveTopology::TriangleList, default());
    }

    let default_width = 12.0;
    let points_per_segment = 20;

    // Interpolate the track centerline
    let mut track_points: Vec<InterpolatedPoint> = Vec::new();

    for i in 0..nodes.len() {
        let p0_idx = if i == 0 && closed_loop {
            nodes.len() - 1
        } else if i == 0 {
            0
        } else {
            i - 1
        };

        let p1_idx = i;
        let p2_idx = (i + 1) % nodes.len();
        let p3_idx = if closed_loop {
            (i + 2) % nodes.len()
        } else {
            (i + 2).min(nodes.len() - 1)
        };

        if i == nodes.len() - 1 && !closed_loop {
            break;
        }

        let p0 = &nodes[p0_idx];
        let p1 = &nodes[p1_idx];
        let p2 = &nodes[p2_idx];
        let p3 = &nodes[p3_idx];

        for j in 0..points_per_segment {
            let t = j as f32 / points_per_segment as f32;
            let (x, y, z) = catmull_rom_point(p0, p1, p2, p3, t);

            let (width_left, width_right) = if let (Some(wl), Some(wr)) = (p1.width_left, p1.width_right) {
                (wl, wr)
            } else if let Some(w) = p1.width {
                (w / 2.0, w / 2.0)
            } else {
                (default_width / 2.0, default_width / 2.0)
            };

            let banking = p1.banking.unwrap_or(0.0);

            track_points.push(InterpolatedPoint {
                x,
                y,
                z,
                width_left,
                width_right,
                banking,
                heading: 0.0, // Will be computed
            });
        }
    }

    // Compute headings
    compute_headings(&mut track_points, closed_loop);

    // Generate mesh vertices
    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    let total_length = track_points.len() as f32;

    for (i, point) in track_points.iter().enumerate() {
        let cos_heading = point.heading.cos();
        let sin_heading = point.heading.sin();

        // Perpendicular direction (left)
        let perpendicular_x = -sin_heading;
        let perpendicular_y = cos_heading;

        // Banking offset
        let banking_offset_z = if point.banking.abs() > 0.001 {
            point.width_left * point.banking.sin()
        } else {
            0.0
        };

        // Left vertex
        let left_x = point.x + perpendicular_x * point.width_left;
        let left_y = point.y + perpendicular_y * point.width_left;
        let left_z = point.z + banking_offset_z;

        // Right vertex
        let right_x = point.x - perpendicular_x * point.width_right;
        let right_y = point.y - perpendicular_y * point.width_right;
        let right_z = point.z - banking_offset_z;

        positions.push([left_x, left_y, left_z]);
        positions.push([right_x, right_y, right_z]);

        // UVs
        let u = i as f32 / total_length;
        uvs.push([u, 0.0]);
        uvs.push([u, 1.0]);

        // Normals (will be computed properly later)
        normals.push([0.0, 0.0, 1.0]);
        normals.push([0.0, 0.0, 1.0]);
    }

    // Generate indices
    let num_segments = if closed_loop {
        track_points.len()
    } else {
        track_points.len() - 1
    };

    for i in 0..num_segments {
        let next_i = (i + 1) % track_points.len();

        let v0 = (i * 2) as u32;
        let v1 = (i * 2 + 1) as u32;
        let v2 = (next_i * 2) as u32;
        let v3 = (next_i * 2 + 1) as u32;

        // First triangle
        indices.push(v0);
        indices.push(v1);
        indices.push(v2);

        // Second triangle
        indices.push(v2);
        indices.push(v1);
        indices.push(v3);
    }

    // Compute smooth normals
    compute_smooth_normals(&positions, &indices, &mut normals);

    let mut mesh = Mesh::new(PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_UV_0, uvs);
    mesh.insert_indices(Indices::U32(indices));

    mesh
}

struct InterpolatedPoint {
    x: f32,
    y: f32,
    z: f32,
    width_left: f32,
    width_right: f32,
    banking: f32,
    heading: f32,
}

fn catmull_rom_point(
    p0: &TrackNode,
    p1: &TrackNode,
    p2: &TrackNode,
    p3: &TrackNode,
    t: f32,
) -> (f32, f32, f32) {
    let t2 = t * t;
    let t3 = t2 * t;

    let x = 0.5
        * ((2.0 * p1.x)
            + (-p0.x + p2.x) * t
            + (2.0 * p0.x - 5.0 * p1.x + 4.0 * p2.x - p3.x) * t2
            + (-p0.x + 3.0 * p1.x - 3.0 * p2.x + p3.x) * t3);

    let y = 0.5
        * ((2.0 * p1.y)
            + (-p0.y + p2.y) * t
            + (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2
            + (-p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3);

    let z = 0.5
        * ((2.0 * p1.z)
            + (-p0.z + p2.z) * t
            + (2.0 * p0.z - 5.0 * p1.z + 4.0 * p2.z - p3.z) * t2
            + (-p0.z + 3.0 * p1.z - 3.0 * p2.z + p3.z) * t3);

    (x, y, z)
}

fn compute_headings(points: &mut [InterpolatedPoint], closed_loop: bool) {
    for i in 0..points.len() {
        let next_idx = if i == points.len() - 1 {
            if closed_loop { 0 } else { i }
        } else {
            i + 1
        };

        if next_idx != i {
            let dx = points[next_idx].x - points[i].x;
            let dy = points[next_idx].y - points[i].y;
            points[i].heading = dy.atan2(dx);
        } else if i > 0 {
            points[i].heading = points[i - 1].heading;
        }
    }
}

fn compute_smooth_normals(positions: &[[f32; 3]], indices: &[u32], normals: &mut [[f32; 3]]) {
    let mut normal_accumulators: Vec<Vec3> = vec![Vec3::ZERO; positions.len()];

    for triangle_idx in (0..indices.len()).step_by(3) {
        let i0 = indices[triangle_idx] as usize;
        let i1 = indices[triangle_idx + 1] as usize;
        let i2 = indices[triangle_idx + 2] as usize;

        let v0 = Vec3::from_array(positions[i0]);
        let v1 = Vec3::from_array(positions[i1]);
        let v2 = Vec3::from_array(positions[i2]);

        let edge1 = v1 - v0;
        let edge2 = v2 - v0;
        let face_normal = edge1.cross(edge2).normalize_or_zero();

        normal_accumulators[i0] += face_normal;
        normal_accumulators[i1] += face_normal;
        normal_accumulators[i2] += face_normal;
    }

    for (i, accumulator) in normal_accumulators.iter().enumerate() {
        let n = accumulator.normalize_or_zero();
        normals[i] = [n.x, n.y, n.z];
    }
}
