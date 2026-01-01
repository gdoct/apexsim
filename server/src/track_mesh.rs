use crate::data::TrackPoint;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackMesh {
    pub vertices: Vec<Vertex3D>,
    pub indices: Vec<u32>,
    pub normals: Vec<Normal3D>,
    pub uvs: Vec<UV>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct Vertex3D {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct Normal3D {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct UV {
    pub u: f32,
    pub v: f32,
}

impl Vertex3D {
    pub fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    pub fn sub(&self, other: &Vertex3D) -> Self {
        Self {
            x: self.x - other.x,
            y: self.y - other.y,
            z: self.z - other.z,
        }
    }

    pub fn cross(&self, other: &Vertex3D) -> Self {
        Self {
            x: self.y * other.z - self.z * other.y,
            y: self.z * other.x - self.x * other.z,
            z: self.x * other.y - self.y * other.x,
        }
    }

    pub fn normalize(&self) -> Normal3D {
        let length = (self.x * self.x + self.y * self.y + self.z * self.z).sqrt();
        if length > 0.0001 {
            Normal3D {
                x: self.x / length,
                y: self.y / length,
                z: self.z / length,
            }
        } else {
            Normal3D {
                x: 0.0,
                y: 0.0,
                z: 1.0,
            }
        }
    }
}

pub struct TrackMeshGenerator;

impl TrackMeshGenerator {
    pub fn generate_mesh(centerline: &[TrackPoint], closed_loop: bool) -> TrackMesh {
        if centerline.is_empty() {
            return TrackMesh {
                vertices: vec![],
                indices: vec![],
                normals: vec![],
                uvs: vec![],
            };
        }

        let mut vertices = Vec::new();
        let mut indices = Vec::new();
        let mut normals = Vec::new();
        let mut uvs = Vec::new();

        let total_length = centerline
            .last()
            .map(|p| p.distance_from_start_m)
            .unwrap_or(0.0);

        for (i, point) in centerline.iter().enumerate() {
            let cos_heading = point.heading_rad.cos();
            let sin_heading = point.heading_rad.sin();

            let perpendicular_x = -sin_heading;
            let perpendicular_y = cos_heading;

            let banking_offset_z = if point.banking_rad.abs() > 0.001 {
                point.width_left_m * point.banking_rad.sin()
            } else {
                0.0
            };

            let left_vertex = Vertex3D::new(
                point.x + perpendicular_x * point.width_left_m,
                point.y + perpendicular_y * point.width_left_m,
                point.z + banking_offset_z,
            );

            let right_vertex = Vertex3D::new(
                point.x - perpendicular_x * point.width_right_m,
                point.y - perpendicular_y * point.width_right_m,
                point.z - banking_offset_z,
            );

            vertices.push(left_vertex);
            vertices.push(right_vertex);

            let u = if total_length > 0.0 {
                point.distance_from_start_m / total_length
            } else {
                i as f32 / centerline.len() as f32
            };

            uvs.push(UV { u, v: 0.0 });
            uvs.push(UV { u, v: 1.0 });

            let normal = Normal3D {
                x: 0.0,
                y: 0.0,
                z: 1.0,
            };
            normals.push(normal);
            normals.push(normal);
        }

        let num_segments = if closed_loop {
            centerline.len()
        } else {
            centerline.len() - 1
        };

        for i in 0..num_segments {
            let next_i = (i + 1) % centerline.len();

            let v0 = (i * 2) as u32;
            let v1 = (i * 2 + 1) as u32;
            let v2 = (next_i * 2) as u32;
            let v3 = (next_i * 2 + 1) as u32;

            indices.push(v0);
            indices.push(v1);
            indices.push(v2);

            indices.push(v2);
            indices.push(v1);
            indices.push(v3);
        }

        Self::compute_smooth_normals(&vertices, &indices, &mut normals);

        TrackMesh {
            vertices,
            indices,
            normals,
            uvs,
        }
    }

    fn compute_smooth_normals(vertices: &[Vertex3D], indices: &[u32], normals: &mut [Normal3D]) {
        let mut normal_accumulators: Vec<(f32, f32, f32)> = vec![(0.0, 0.0, 0.0); vertices.len()];

        for triangle_idx in (0..indices.len()).step_by(3) {
            let i0 = indices[triangle_idx] as usize;
            let i1 = indices[triangle_idx + 1] as usize;
            let i2 = indices[triangle_idx + 2] as usize;

            let v0 = &vertices[i0];
            let v1 = &vertices[i1];
            let v2 = &vertices[i2];

            let edge1 = v1.sub(v0);
            let edge2 = v2.sub(v0);
            let face_normal = edge1.cross(&edge2).normalize();

            normal_accumulators[i0].0 += face_normal.x;
            normal_accumulators[i0].1 += face_normal.y;
            normal_accumulators[i0].2 += face_normal.z;

            normal_accumulators[i1].0 += face_normal.x;
            normal_accumulators[i1].1 += face_normal.y;
            normal_accumulators[i1].2 += face_normal.z;

            normal_accumulators[i2].0 += face_normal.x;
            normal_accumulators[i2].1 += face_normal.y;
            normal_accumulators[i2].2 += face_normal.z;
        }

        for (i, accumulator) in normal_accumulators.iter().enumerate() {
            let vertex = Vertex3D::new(accumulator.0, accumulator.1, accumulator.2);
            normals[i] = vertex.normalize();
        }
    }

    pub fn export_obj(mesh: &TrackMesh) -> String {
        let mut obj = String::new();

        obj.push_str("# ApexSim Track Mesh\n");
        obj.push_str("# Generated by TrackMeshGenerator\n\n");

        for vertex in &mesh.vertices {
            obj.push_str(&format!("v {} {} {}\n", vertex.x, vertex.y, vertex.z));
        }

        obj.push_str("\n");

        for uv in &mesh.uvs {
            obj.push_str(&format!("vt {} {}\n", uv.u, uv.v));
        }

        obj.push_str("\n");

        for normal in &mesh.normals {
            obj.push_str(&format!("vn {} {} {}\n", normal.x, normal.y, normal.z));
        }

        obj.push_str("\n");

        for triangle_idx in (0..mesh.indices.len()).step_by(3) {
            let i0 = mesh.indices[triangle_idx] + 1;
            let i1 = mesh.indices[triangle_idx + 1] + 1;
            let i2 = mesh.indices[triangle_idx + 2] + 1;

            obj.push_str(&format!(
                "f {}/{}/{} {}/{}/{} {}/{}/{}\n",
                i0, i0, i0, i1, i1, i1, i2, i2, i2
            ));
        }

        obj
    }

    pub fn export_gltf_json(mesh: &TrackMesh, track_name: &str) -> String {
        serde_json::json!({
            "asset": {
                "version": "2.0",
                "generator": "ApexSim TrackMeshGenerator"
            },
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{
                "mesh": 0,
                "name": track_name
            }],
            "meshes": [{
                "primitives": [{
                    "attributes": {
                        "POSITION": 0,
                        "NORMAL": 1,
                        "TEXCOORD_0": 2
                    },
                    "indices": 3
                }]
            }],
            "accessors": [
                {
                    "bufferView": 0,
                    "componentType": 5126,
                    "count": mesh.vertices.len(),
                    "type": "VEC3",
                    "max": Self::compute_bounds_max(&mesh.vertices),
                    "min": Self::compute_bounds_min(&mesh.vertices)
                },
                {
                    "bufferView": 1,
                    "componentType": 5126,
                    "count": mesh.normals.len(),
                    "type": "VEC3"
                },
                {
                    "bufferView": 2,
                    "componentType": 5126,
                    "count": mesh.uvs.len(),
                    "type": "VEC2"
                },
                {
                    "bufferView": 3,
                    "componentType": 5125,
                    "count": mesh.indices.len(),
                    "type": "SCALAR"
                }
            ],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": mesh.vertices.len() * 12},
                {"buffer": 0, "byteOffset": mesh.vertices.len() * 12, "byteLength": mesh.normals.len() * 12},
                {"buffer": 0, "byteOffset": mesh.vertices.len() * 12 + mesh.normals.len() * 12, "byteLength": mesh.uvs.len() * 8},
                {"buffer": 0, "byteOffset": mesh.vertices.len() * 12 + mesh.normals.len() * 12 + mesh.uvs.len() * 8, "byteLength": mesh.indices.len() * 4}
            ],
            "buffers": [{
                "byteLength": mesh.vertices.len() * 12 + mesh.normals.len() * 12 + mesh.uvs.len() * 8 + mesh.indices.len() * 4
            }]
        })
        .to_string()
    }

    fn compute_bounds_max(vertices: &[Vertex3D]) -> Vec<f32> {
        let mut max_x = f32::MIN;
        let mut max_y = f32::MIN;
        let mut max_z = f32::MIN;

        for v in vertices {
            max_x = max_x.max(v.x);
            max_y = max_y.max(v.y);
            max_z = max_z.max(v.z);
        }

        vec![max_x, max_y, max_z]
    }

    fn compute_bounds_min(vertices: &[Vertex3D]) -> Vec<f32> {
        let mut min_x = f32::MAX;
        let mut min_y = f32::MAX;
        let mut min_z = f32::MAX;

        for v in vertices {
            min_x = min_x.min(v.x);
            min_y = min_y.min(v.y);
            min_z = min_z.min(v.z);
        }

        vec![min_x, min_y, min_z]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::SurfaceType;

    #[test]
    fn test_generate_simple_mesh() {
        let centerline = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 100.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ];

        let mesh = TrackMeshGenerator::generate_mesh(&centerline, false);

        assert_eq!(mesh.vertices.len(), 4);
        assert_eq!(mesh.indices.len(), 6);
        assert_eq!(mesh.normals.len(), 4);
        assert_eq!(mesh.uvs.len(), 4);
    }

    #[test]
    fn test_generate_closed_loop_mesh() {
        let centerline = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 100.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 100.0,
                z: 0.0,
                distance_from_start_m: 200.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: std::f32::consts::PI / 2.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ];

        let mesh = TrackMeshGenerator::generate_mesh(&centerline, true);

        assert_eq!(mesh.vertices.len(), 6);
        assert_eq!(mesh.indices.len(), 18);
    }

    #[test]
    fn test_export_obj() {
        let centerline = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 10.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ];

        let mesh = TrackMeshGenerator::generate_mesh(&centerline, false);
        let obj = TrackMeshGenerator::export_obj(&mesh);

        assert!(obj.contains("v "));
        assert!(obj.contains("vt "));
        assert!(obj.contains("vn "));
        assert!(obj.contains("f "));
    }

    #[test]
    fn test_mesh_with_banking() {
        let centerline = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.1,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 10.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.1,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ];

        let mesh = TrackMeshGenerator::generate_mesh(&centerline, false);

        assert!(mesh.vertices[0].z.abs() > 0.01);
    }
}
