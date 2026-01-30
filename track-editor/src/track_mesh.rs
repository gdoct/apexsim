use bevy::prelude::*;
use bevy::render::mesh::{Indices, PrimitiveTopology};
use crate::track_data::TrackNode;

/// Precomputed track edge polylines - the smooth left and right outlines of the track.
/// Each entry corresponds to an interpolated point along the spline.
/// `source_node[i]` gives the original TrackNode index that this point was interpolated from.
pub struct TrackEdges {
    pub left: Vec<Vec3>,
    pub right: Vec<Vec3>,
    pub centerline: Vec<Vec3>,
    pub perpendiculars: Vec<Vec3>,
    /// For each interpolated point, which source TrackNode index it came from (segment start)
    pub source_node: Vec<usize>,
}

/// Compute interpolated track edge polylines from the raw nodes.
pub fn compute_track_edges(nodes: &[TrackNode], closed_loop: bool) -> TrackEdges {
    let default_width = 12.0;
    let points_per_segment = 20;

    let mut centerline: Vec<Vec3> = Vec::new();
    let mut widths_left: Vec<f32> = Vec::new();
    let mut widths_right: Vec<f32> = Vec::new();
    let mut bankings: Vec<f32> = Vec::new();
    let mut source_node: Vec<usize> = Vec::new();

    if nodes.len() < 2 {
        return TrackEdges {
            left: Vec::new(),
            right: Vec::new(),
            centerline: Vec::new(),
            perpendiculars: Vec::new(),
            source_node: Vec::new(),
        };
    }

    let num_segments = if closed_loop { nodes.len() } else { nodes.len() - 1 };

    for i in 0..num_segments {
        let p0_idx = if i == 0 {
            if closed_loop { nodes.len() - 1 } else { 0 }
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

        let p0 = &nodes[p0_idx];
        let p1 = &nodes[p1_idx];
        let p2 = &nodes[p2_idx];
        let p3 = &nodes[p3_idx];

        let (w1_left, w1_right) = get_node_widths(p1, default_width);
        let (w2_left, w2_right) = get_node_widths(p2, default_width);
        let b1 = p1.banking.unwrap_or(0.0);
        let b2 = p2.banking.unwrap_or(0.0);

        for j in 0..points_per_segment {
            let t = j as f32 / points_per_segment as f32;
            let pos = catmull_rom_point(p0, p1, p2, p3, t);
            centerline.push(pos);
            widths_left.push(w1_left + (w2_left - w1_left) * t);
            widths_right.push(w1_right + (w2_right - w1_right) * t);
            bankings.push(b1 + (b2 - b1) * t);
            source_node.push(p1_idx);
        }
    }

    let n = centerline.len();

    // Compute tangents
    let mut tangents: Vec<Vec3> = Vec::with_capacity(n);
    for i in 0..n {
        let prev_idx = if i == 0 {
            if closed_loop { n - 1 } else { 0 }
        } else {
            i - 1
        };
        let next_idx = if i == n - 1 {
            if closed_loop { 0 } else { n - 1 }
        } else {
            i + 1
        };
        tangents.push((centerline[next_idx] - centerline[prev_idx]).normalize_or_zero());
    }

    // Compute perpendiculars
    let mut perpendiculars: Vec<Vec3> = Vec::with_capacity(n);
    for i in 0..n {
        let tangent_2d = Vec2::new(tangents[i].x, tangents[i].y);
        let tangent_2d_len = tangent_2d.length();
        let perp = if tangent_2d_len > 0.001 {
            let perp_2d = Vec2::new(-tangent_2d.y, tangent_2d.x) / tangent_2d_len;
            Vec3::new(perp_2d.x, perp_2d.y, 0.0)
        } else {
            Vec3::X
        };
        perpendiculars.push(perp);
    }

    // Compute left and right edge positions
    let mut left_edges: Vec<Vec3> = Vec::with_capacity(n);
    let mut right_edges: Vec<Vec3> = Vec::with_capacity(n);
    for i in 0..n {
        let center = centerline[i];
        let perp = perpendiculars[i];
        let banking_rad = bankings[i].to_radians();
        let cos_bank = banking_rad.cos();
        let sin_bank = banking_rad.sin();
        let banked_perp = perp * cos_bank + Vec3::Z * sin_bank;

        left_edges.push(center + banked_perp * widths_left[i]);
        right_edges.push(center - banked_perp * widths_right[i]);
    }

    TrackEdges {
        left: left_edges,
        right: right_edges,
        centerline,
        perpendiculars,
        source_node,
    }
}

/// Generate a Bevy mesh from track nodes using Catmull-Rom spline interpolation
/// Creates a thick slab mesh to prevent terrain poke-through
pub fn generate_track_mesh(nodes: &[TrackNode], closed_loop: bool) -> Mesh {
    if nodes.len() < 2 {
        return Mesh::new(PrimitiveTopology::TriangleList, default());
    }

    let edges = compute_track_edges(nodes, closed_loop);
    let n = edges.centerline.len();

    if n == 0 {
        return Mesh::new(PrimitiveTopology::TriangleList, default());
    }

    let track_thickness = 2.0;

    // Step 4: Generate mesh vertices
    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    // Calculate total length for UV mapping
    let mut total_length = 0.0f32;
    for i in 1..n {
        total_length += (edges.centerline[i] - edges.centerline[i - 1]).length();
    }
    if closed_loop {
        total_length += (edges.centerline[0] - edges.centerline[n - 1]).length();
    }

    let mut accumulated_length = 0.0f32;

    // Generate vertices for top and bottom surfaces
    // Layout: for each cross-section i, we have 4 vertices:
    //   i*4 + 0: top left
    //   i*4 + 1: top right
    //   i*4 + 2: bottom left
    //   i*4 + 3: bottom right

    for i in 0..n {
        let top_left = edges.left[i];
        let top_right = edges.right[i];

        // Bottom surface vertices (track_thickness below)
        let bottom_left = top_left - Vec3::Z * track_thickness;
        let bottom_right = top_right - Vec3::Z * track_thickness;

        // Approximate banked_up from cross product of edge direction and tangent
        let cross_dir = (top_right - top_left).normalize_or_zero();
        let banked_up = edges.perpendiculars[i].cross(cross_dir).normalize_or_zero();
        let banked_up = if banked_up.z < 0.0 { -banked_up } else { banked_up };
        // Fallback if degenerate
        let banked_up = if banked_up.length_squared() < 0.5 { Vec3::Z } else { banked_up };

        // UVs
        let u = if total_length > 0.0 {
            accumulated_length / total_length
        } else {
            i as f32 / n as f32
        };

        // Top left
        positions.push([top_left.x, top_left.y, top_left.z]);
        normals.push([banked_up.x, banked_up.y, banked_up.z]);
        uvs.push([u, 0.0]);

        // Top right
        positions.push([top_right.x, top_right.y, top_right.z]);
        normals.push([banked_up.x, banked_up.y, banked_up.z]);
        uvs.push([u, 1.0]);

        // Bottom left
        positions.push([bottom_left.x, bottom_left.y, bottom_left.z]);
        normals.push([-banked_up.x, -banked_up.y, -banked_up.z]);
        uvs.push([u, 0.0]);

        // Bottom right
        positions.push([bottom_right.x, bottom_right.y, bottom_right.z]);
        normals.push([-banked_up.x, -banked_up.y, -banked_up.z]);
        uvs.push([u, 1.0]);

        // Update accumulated length
        if i > 0 {
            accumulated_length += (edges.centerline[i] - edges.centerline[i - 1]).length();
        }
    }

    // Step 5: Generate indices for top, bottom, left side, and right side surfaces
    // For closed loops, we need to connect the last segment back to vertex 0
    let num_quads = if closed_loop { n } else { n - 1 };

    for i in 0..num_quads {
        // Vertex indices for current and next cross-section
        // Each cross-section has 4 vertices: top_left, top_right, bottom_left, bottom_right
        let curr_tl = (i * 4) as u32;         // current top left
        let curr_tr = (i * 4 + 1) as u32;     // current top right
        let curr_bl = (i * 4 + 2) as u32;     // current bottom left
        let curr_br = (i * 4 + 3) as u32;     // current bottom right

        // For closed loops, wrap back to vertex 0 on the last segment
        let next_i = if closed_loop { (i + 1) % n } else { i + 1 };
        let next_tl = (next_i * 4) as u32;       // next top left
        let next_tr = (next_i * 4 + 1) as u32;   // next top right
        let next_bl = (next_i * 4 + 2) as u32;   // next bottom left
        let next_br = (next_i * 4 + 3) as u32;   // next bottom right

        // Top surface (facing up)
        indices.push(curr_tl);
        indices.push(curr_tr);
        indices.push(next_tl);
        indices.push(next_tl);
        indices.push(curr_tr);
        indices.push(next_tr);

        // Bottom surface (facing down)
        indices.push(curr_bl);
        indices.push(next_bl);
        indices.push(curr_br);
        indices.push(curr_br);
        indices.push(next_bl);
        indices.push(next_br);

        // Left side (facing left)
        indices.push(curr_tl);
        indices.push(next_tl);
        indices.push(curr_bl);
        indices.push(curr_bl);
        indices.push(next_tl);
        indices.push(next_bl);

        // Right side (facing right)
        indices.push(curr_tr);
        indices.push(curr_br);
        indices.push(next_tr);
        indices.push(next_tr);
        indices.push(curr_br);
        indices.push(next_br);
    }

    // Note: We skip smooth normal computation since we have explicit normals for each surface

    let mut mesh = Mesh::new(PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_UV_0, uvs);
    mesh.insert_indices(Indices::U32(indices));

    mesh
}

fn get_node_widths(node: &TrackNode, default_width: f32) -> (f32, f32) {
    if let (Some(wl), Some(wr)) = (node.width_left, node.width_right) {
        (wl, wr)
    } else if let Some(w) = node.width {
        (w / 2.0, w / 2.0)
    } else {
        (default_width / 2.0, default_width / 2.0)
    }
}

/// Catmull-Rom spline position at parameter t
fn catmull_rom_point(
    p0: &TrackNode,
    p1: &TrackNode,
    p2: &TrackNode,
    p3: &TrackNode,
    t: f32,
) -> Vec3 {
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

    Vec3::new(x, y, z)
}
