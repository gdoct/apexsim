//! Baked road mesh: the surface under a wheel as triangles.
//!
//! The track editor's `ats-export` writes `<Track>.road.msgpack` next to
//! the track YAML while it bakes the Unreal scene: the rendered road, curb,
//! run-off and ground bands and the pit lane as one indexed triangle mesh
//! in the server frame, without the render-only lifts, each triangle
//! tagged with a surface (its contact class, a friction multiplier and
//! whether it is inside the track limits). An importer can write the same
//! file from a track whose road was only ever triangles (Assetto Corsa's
//! physics kn5), which the centerline could not describe at all.
//!
//! With the sidecar loaded — and `[physics] road_contact = "mesh"` in
//! `server.toml` — each of the sim's six surface queries a tick (the car's
//! centre before and after integration, the four wheels) takes its height,
//! its normal and its surface class from here instead of from the
//! centerline formula. Everything about *where along the lap* a car is
//! (progress, laps, sectors, the AI, the racing line, the grid) stays on
//! the centerline; see `docs/ROAD_MESH.md`. Without the sidecar, or with
//! the setting at `"centerline"`, nothing changes.
//!
//! The query is a vertical ray: of the triangles under `(x, y)` it returns
//! the highest whose surface is at or below `z_ref + STEP_UP_M`, so a car
//! in the slot under Suzuka's crossover finds the slot floor and one on the
//! deck finds the deck. The triangles are bucketed into a uniform grid at
//! load, CSR-packed, so a query is a cell lookup and a handful of
//! point-in-triangle tests; it allocates nothing and iterates nothing in
//! hash order, which is what keeps the sim deterministic.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const ROAD_MESH_VERSION: u32 = 1;

/// Contact classes as the exporter writes them (`RoadSurface::contact`).
pub const CONTACT_ROAD: u8 = 0;
pub const CONTACT_CURB: u8 = 1;
pub const CONTACT_RUNOFF: u8 = 2;
pub const CONTACT_OFF: u8 = 3;
pub const CONTACT_PIT_LANE: u8 = 4;

/// How far above the reference height a surface may be and still be the
/// one under the wheel: a curb, a bump, the deck a car is about to land
/// on, but never the bridge three metres up.
pub const STEP_UP_M: f32 = 0.5;

/// A triangle with a smaller footprint in the ground plane is nothing a
/// wheel can stand on (a vertical face, a sliver) and is dropped at load.
pub const MIN_FOOTPRINT_M2: f32 = 1e-4;

/// Barycentric slack for the point-in-triangle test, so a point on a
/// shared edge belongs to both neighbours and none falls between them.
const EDGE_EPSILON: f32 = 1e-4;

/// Two surfaces this close under a point are one laid over the other — the
/// tarmac run-off across the grass band, a curb's inner edge on the road
/// edge — and the one written last wins, as it is drawn: the exporter
/// bakes the road, then the bands in layer order, then the curbs, and
/// keeps the pit lane off the road altogether.
pub const TIE_M: f32 = 0.02;

/// Grid cell size for a generated mesh (1 m road triangles, 4 m bands).
const CELL_M: f32 = 4.0;
/// A mesh whose bounding box would need more cells than this at
/// [`CELL_M`] gets coarser cells instead.
const MAX_CELLS: f32 = 2_000_000.0;

/// One kind of surface: what the sim does with a wheel on it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadSurface {
    /// `road`, `curb`, `runoff_asphalt`, `off_grass`, `pit_lane`, or an
    /// importer's own key. Logged, never interpreted.
    pub key: String,
    /// [`CONTACT_ROAD`] and friends.
    pub contact: u8,
    /// Multiplier on the grip the class has on this track (1.0 = the
    /// track's own figure for that class). A generated mesh writes 1.0
    /// everywhere.
    pub friction: f32,
    /// Inside the track limits: a wheel here is not off the track.
    pub valid_track: bool,
    /// The pit lane. Nothing reads it yet.
    pub pit_lane: bool,
}

/// The sidecar as written (`rmp_serde::to_vec_named`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadMeshFile {
    pub version: u32,
    /// Server frame: metres, +X along the start line's heading, +Y left,
    /// +Z up, origin at the start/finish line.
    pub vertices: Vec<[f32; 3]>,
    /// Counter-clockwise seen from above. Indices into `vertices`.
    pub triangles: Vec<[u32; 3]>,
    /// One entry per triangle: an index into `surfaces`.
    pub triangle_surface: Vec<u16>,
    pub surfaces: Vec<RoadSurface>,
    /// Which generator wrote it and from what.
    pub source: String,
}

#[derive(Debug, thiserror::Error)]
pub enum RoadMeshLoadError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("decode error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("invalid road mesh: {0}")]
    Invalid(String),
}

/// A triangle's plane, `n · p = d` with `n` unit and `n.z > 0`.
#[derive(Debug, Clone, Copy)]
struct Plane {
    nx: f32,
    ny: f32,
    nz: f32,
    d: f32,
}

/// What the mesh says is under a point.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MeshContact {
    pub z: f32,
    /// Unit surface normal, `z > 0`.
    pub normal: [f32; 3],
    /// Index into [`RoadMesh::surfaces`].
    pub surface: u16,
    pub triangle: u32,
}

/// The road mesh of a track with a grid over it.
#[derive(Debug, Clone)]
pub struct RoadMesh {
    vertices: Vec<[f32; 3]>,
    triangles: Vec<[u32; 3]>,
    triangle_surface: Vec<u16>,
    surfaces: Vec<RoadSurface>,
    planes: Vec<Plane>,
    source: String,
    /// Triangles the file held that had no footprint.
    dropped: usize,
    origin: (f32, f32),
    cell_m: f32,
    cols: usize,
    rows: usize,
    /// CSR: cell `c` owns `entries[starts[c]..starts[c + 1]]`, ascending.
    starts: Vec<u32>,
    entries: Vec<u32>,
}

impl RoadMesh {
    /// Sidecar path for a track file: `Monza.yaml` -> `Monza.road.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        let stem = track_file
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("track");
        track_file.with_file_name(format!("{}.road.msgpack", stem))
    }

    pub fn load(path: &Path) -> Result<Self, RoadMeshLoadError> {
        let bytes = std::fs::read(path)?;
        let file: RoadMeshFile = rmp_serde::from_slice(&bytes)?;
        Self::from_file(file)
    }

    /// Validate a file and build the index over it: the version, every
    /// index in range, one surface per triangle, finite coordinates; a
    /// triangle without a footprint is dropped and counted, one wound
    /// clockwise is turned round.
    pub fn from_file(file: RoadMeshFile) -> Result<Self, RoadMeshLoadError> {
        if file.version != ROAD_MESH_VERSION {
            return Err(RoadMeshLoadError::Invalid(format!(
                "unsupported version {}",
                file.version
            )));
        }
        if file.triangle_surface.len() != file.triangles.len() {
            return Err(RoadMeshLoadError::Invalid(format!(
                "{} surface tags for {} triangles",
                file.triangle_surface.len(),
                file.triangles.len()
            )));
        }
        if let Some(v) = file
            .vertices
            .iter()
            .find(|v| !v.iter().all(|c| c.is_finite()))
        {
            return Err(RoadMeshLoadError::Invalid(format!(
                "non-finite vertex {v:?}"
            )));
        }
        let n = file.vertices.len() as u32;
        if let Some(t) = file.triangles.iter().find(|t| t.iter().any(|&i| i >= n)) {
            return Err(RoadMeshLoadError::Invalid(format!(
                "triangle {t:?} indexes past {n} vertices"
            )));
        }
        let s = file.surfaces.len() as u16;
        if let Some(i) = file.triangle_surface.iter().find(|&&i| i >= s) {
            return Err(RoadMeshLoadError::Invalid(format!(
                "surface tag {i} past {s} surfaces"
            )));
        }
        if let Some(bad) = file
            .surfaces
            .iter()
            .find(|s| !(s.friction.is_finite() && s.friction >= 0.0))
        {
            return Err(RoadMeshLoadError::Invalid(format!(
                "surface {} has friction {}",
                bad.key, bad.friction
            )));
        }

        let RoadMeshFile {
            vertices,
            triangles,
            triangle_surface,
            surfaces,
            source,
            ..
        } = file;

        let mut kept: Vec<[u32; 3]> = Vec::with_capacity(triangles.len());
        let mut kept_surface: Vec<u16> = Vec::with_capacity(triangles.len());
        let mut planes: Vec<Plane> = Vec::with_capacity(triangles.len());
        let mut dropped = 0usize;
        for (tri, &surface) in triangles.iter().zip(&triangle_surface) {
            let (a, b, c) = (
                vertices[tri[0] as usize],
                vertices[tri[1] as usize],
                vertices[tri[2] as usize],
            );
            let twice = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if !twice.is_finite() || twice.abs() < 2.0 * MIN_FOOTPRINT_M2 {
                dropped += 1;
                continue;
            }
            let tri = if twice > 0.0 {
                *tri
            } else {
                [tri[0], tri[2], tri[1]]
            };
            let (b, c) = if twice > 0.0 { (b, c) } else { (c, b) };
            let (ux, uy, uz) = (b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            let (vx, vy, vz) = (c[0] - a[0], c[1] - a[1], c[2] - a[2]);
            let (nx, ny, nz) = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);
            let len = (nx * nx + ny * ny + nz * nz).sqrt();
            if !(len.is_finite() && len > 0.0) || nz <= 0.0 {
                dropped += 1;
                continue;
            }
            let (nx, ny, nz) = (nx / len, ny / len, nz / len);
            let d = nx * a[0] + ny * a[1] + nz * a[2];
            kept.push(tri);
            kept_surface.push(surface);
            planes.push(Plane { nx, ny, nz, d });
        }

        let mut mesh = Self {
            vertices,
            triangles: kept,
            triangle_surface: kept_surface,
            surfaces,
            planes,
            source,
            dropped,
            origin: (0.0, 0.0),
            cell_m: CELL_M,
            cols: 0,
            rows: 0,
            starts: vec![0],
            entries: Vec::new(),
        };
        mesh.build_grid();
        Ok(mesh)
    }

    fn build_grid(&mut self) {
        if self.triangles.is_empty() {
            return;
        }
        let (mut min_x, mut min_y, mut max_x, mut max_y) = (
            f32::INFINITY,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::NEG_INFINITY,
        );
        for tri in &self.triangles {
            for &i in tri {
                let v = self.vertices[i as usize];
                min_x = min_x.min(v[0]);
                min_y = min_y.min(v[1]);
                max_x = max_x.max(v[0]);
                max_y = max_y.max(v[1]);
            }
        }
        let origin = (min_x - 1.0, min_y - 1.0);
        let span_x = max_x + 1.0 - origin.0;
        let span_y = max_y + 1.0 - origin.1;
        let cell_m = CELL_M.max((span_x * span_y / MAX_CELLS).sqrt());
        let cols = (span_x / cell_m).ceil().max(1.0) as usize;
        let rows = (span_y / cell_m).ceil().max(1.0) as usize;

        let cell_range = |tri: &[u32; 3]| {
            let (mut lo_x, mut lo_y, mut hi_x, mut hi_y) = (
                f32::INFINITY,
                f32::INFINITY,
                f32::NEG_INFINITY,
                f32::NEG_INFINITY,
            );
            for &i in tri {
                let v = self.vertices[i as usize];
                lo_x = lo_x.min(v[0]);
                lo_y = lo_y.min(v[1]);
                hi_x = hi_x.max(v[0]);
                hi_y = hi_y.max(v[1]);
            }
            let c0 = ((lo_x - origin.0) / cell_m).floor().max(0.0) as usize;
            let c1 = (((hi_x - origin.0) / cell_m).floor().max(0.0) as usize).min(cols - 1);
            let r0 = ((lo_y - origin.1) / cell_m).floor().max(0.0) as usize;
            let r1 = (((hi_y - origin.1) / cell_m).floor().max(0.0) as usize).min(rows - 1);
            (c0, c1, r0, r1)
        };

        // Two passes: count per cell, then fill in triangle order, which
        // leaves every cell's entries ascending.
        let mut counts = vec![0u32; cols * rows];
        for tri in &self.triangles {
            let (c0, c1, r0, r1) = cell_range(tri);
            for r in r0..=r1 {
                for c in c0..=c1 {
                    counts[r * cols + c] += 1;
                }
            }
        }
        let mut starts = Vec::with_capacity(cols * rows + 1);
        let mut acc = 0u32;
        for n in &counts {
            starts.push(acc);
            acc += n;
        }
        starts.push(acc);
        let mut fill = starts.clone();
        let mut entries = vec![0u32; acc as usize];
        for (idx, tri) in self.triangles.iter().enumerate() {
            let (c0, c1, r0, r1) = cell_range(tri);
            for r in r0..=r1 {
                for c in c0..=c1 {
                    let cell = r * cols + c;
                    entries[fill[cell] as usize] = idx as u32;
                    fill[cell] += 1;
                }
            }
        }

        self.origin = origin;
        self.cell_m = cell_m;
        self.cols = cols;
        self.rows = rows;
        self.starts = starts;
        self.entries = entries;
    }

    pub fn surfaces(&self) -> &[RoadSurface] {
        &self.surfaces
    }

    pub fn surface(&self, idx: u16) -> &RoadSurface {
        &self.surfaces[idx as usize]
    }

    pub fn triangle_count(&self) -> usize {
        self.triangles.len()
    }

    pub fn vertex_count(&self) -> usize {
        self.vertices.len()
    }

    /// Triangles the file held that were dropped for having no footprint.
    pub fn dropped_count(&self) -> usize {
        self.dropped
    }

    pub fn source(&self) -> &str {
        &self.source
    }

    pub fn is_empty(&self) -> bool {
        self.triangles.is_empty()
    }

    /// The vertices of triangle `idx`.
    pub fn triangle(&self, idx: u32) -> [[f32; 3]; 3] {
        let t = self.triangles[idx as usize];
        [
            self.vertices[t[0] as usize],
            self.vertices[t[1] as usize],
            self.vertices[t[2] as usize],
        ]
    }

    /// The surface under `(x, y)`: the highest triangle containing the
    /// point whose height there is at or below `z_ref + STEP_UP_M`. Of two
    /// within [`TIE_M`] of each other (a shared edge, one surface laid over
    /// another) the higher triangle index wins. `None` where the mesh has
    /// nothing at or below that height.
    pub fn contact_at(&self, x: f32, y: f32, z_ref: f32) -> Option<MeshContact> {
        if self.triangles.is_empty() || !(x.is_finite() && y.is_finite() && z_ref.is_finite()) {
            return None;
        }
        let fc = (x - self.origin.0) / self.cell_m;
        let fr = (y - self.origin.1) / self.cell_m;
        if fc < 0.0 || fr < 0.0 || fc >= self.cols as f32 || fr >= self.rows as f32 {
            return None;
        }
        let cell = fr as usize * self.cols + fc as usize;
        let ceiling = z_ref + STEP_UP_M;
        let mut best: Option<MeshContact> = None;
        for &idx in &self.entries[self.starts[cell] as usize..self.starts[cell + 1] as usize] {
            let tri = self.triangles[idx as usize];
            let (a, b, c) = (
                self.vertices[tri[0] as usize],
                self.vertices[tri[1] as usize],
                self.vertices[tri[2] as usize],
            );
            if !contains(a, b, c, x, y) {
                continue;
            }
            let plane = self.planes[idx as usize];
            let z = (plane.d - plane.nx * x - plane.ny * y) / plane.nz;
            if z > ceiling {
                continue;
            }
            // Anything not clearly lower replaces, so the last (highest
            // index) of a near-tie is kept.
            if best.is_none_or(|b| z >= b.z - TIE_M) {
                best = Some(MeshContact {
                    z,
                    normal: [plane.nx, plane.ny, plane.nz],
                    surface: self.triangle_surface[idx as usize],
                    triangle: idx,
                });
            }
        }
        best
    }
}

/// Point-in-triangle by barycentric sign, with [`EDGE_EPSILON`] of slack.
/// The triangles are counter-clockwise, so every sub-area is positive
/// inside.
#[inline]
fn contains(a: [f32; 3], b: [f32; 3], c: [f32; 3], x: f32, y: f32) -> bool {
    let twice = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    let w0 = ((b[0] - x) * (c[1] - y) - (b[1] - y) * (c[0] - x)) / twice;
    let w1 = ((c[0] - x) * (a[1] - y) - (c[1] - y) * (a[0] - x)) / twice;
    let w2 = 1.0 - w0 - w1;
    w0 >= -EDGE_EPSILON && w1 >= -EDGE_EPSILON && w2 >= -EDGE_EPSILON
}

#[cfg(test)]
mod tests {
    use super::*;

    fn surfaces() -> Vec<RoadSurface> {
        vec![
            RoadSurface {
                key: "road".into(),
                contact: CONTACT_ROAD,
                friction: 1.0,
                valid_track: true,
                pit_lane: false,
            },
            RoadSurface {
                key: "off_grass".into(),
                contact: CONTACT_OFF,
                friction: 1.0,
                valid_track: false,
                pit_lane: false,
            },
        ]
    }

    /// A quad `x0..x1` by `y0..y1` at `z`, as two counter-clockwise
    /// triangles, appended to `file` with `surface`.
    fn push_quad(
        file: &mut RoadMeshFile,
        x0: f32,
        x1: f32,
        y0: f32,
        y1: f32,
        z: impl Fn(f32, f32) -> f32,
        surface: u16,
    ) {
        let base = file.vertices.len() as u32;
        file.vertices.push([x0, y0, z(x0, y0)]);
        file.vertices.push([x1, y0, z(x1, y0)]);
        file.vertices.push([x1, y1, z(x1, y1)]);
        file.vertices.push([x0, y1, z(x0, y1)]);
        file.triangles.push([base, base + 1, base + 2]);
        file.triangles.push([base, base + 2, base + 3]);
        file.triangle_surface.push(surface);
        file.triangle_surface.push(surface);
    }

    fn empty() -> RoadMeshFile {
        RoadMeshFile {
            version: ROAD_MESH_VERSION,
            vertices: Vec::new(),
            triangles: Vec::new(),
            triangle_surface: Vec::new(),
            surfaces: surfaces(),
            source: "test".into(),
        }
    }

    #[test]
    fn sidecar_sits_next_to_the_track_file() {
        let p = RoadMesh::sidecar_path(Path::new("content/tracks/default/Monza/Monza.yaml"));
        assert!(p.ends_with("Monza.road.msgpack"));
    }

    #[test]
    fn a_point_on_a_shared_edge_picks_the_higher_index() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 0);
        // Tag the second triangle as grass so the pick shows.
        file.triangle_surface[1] = 1;
        let mesh = RoadMesh::from_file(file).unwrap();
        // The diagonal runs (0,0)-(10,10); (5,5) is on it.
        let hit = mesh.contact_at(5.0, 5.0, 0.0).unwrap();
        assert_eq!(hit.triangle, 1);
        assert_eq!(hit.surface, 1);
        // Clearly inside the first triangle.
        let hit = mesh.contact_at(8.0, 2.0, 0.0).unwrap();
        assert_eq!(hit.triangle, 0);
        assert_eq!(hit.surface, 0);
    }

    #[test]
    fn a_surface_laid_over_another_within_a_tie_wins_as_the_later_one() {
        // Grass, then the tarmac run-off laid across it a centimetre lower
        // (the ground bends a little): the run-off is what is there.
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 1);
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| -0.01, 0);
        let mesh = RoadMesh::from_file(file).unwrap();
        let hit = mesh.contact_at(3.0, 3.0, 0.0).unwrap();
        assert_eq!(hit.surface, 0);
        assert!((hit.z + 0.01).abs() < 1e-6);
        // A real step still wins over what is written after it: a deck a
        // metre up is not a layer of the floor.
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 1.0, 0);
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 1);
        let mesh = RoadMesh::from_file(file).unwrap();
        let hit = mesh.contact_at(3.0, 3.0, 1.0).unwrap();
        assert_eq!(hit.surface, 0);
        assert!((hit.z - 1.0).abs() < 1e-6);
    }

    #[test]
    fn stacked_levels_pick_by_reference_height() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 0);
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 5.0, 0);
        let mesh = RoadMesh::from_file(file).unwrap();
        let low = mesh.contact_at(3.0, 3.0, 0.2).unwrap();
        assert!((low.z - 0.0).abs() < 1e-6, "in the slot: {low:?}");
        let high = mesh.contact_at(3.0, 3.0, 5.1).unwrap();
        assert!((high.z - 5.0).abs() < 1e-6, "on the deck: {high:?}");
        // Just under the deck by less than the step: still the deck.
        let step = mesh.contact_at(3.0, 3.0, 4.6).unwrap();
        assert!((step.z - 5.0).abs() < 1e-6);
        // Well below the floor: nothing at or below.
        assert!(mesh.contact_at(3.0, 3.0, -2.0).is_none());
    }

    #[test]
    fn a_hole_and_the_outside_return_none() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 0);
        push_quad(&mut file, 20.0, 30.0, 0.0, 10.0, |_, _| 0.0, 0);
        let mesh = RoadMesh::from_file(file).unwrap();
        assert!(mesh.contact_at(15.0, 5.0, 0.0).is_none(), "the hole");
        assert!(mesh.contact_at(-50.0, 5.0, 0.0).is_none(), "off the grid");
        assert!(mesh.contact_at(5.0, 500.0, 0.0).is_none());
        assert!(mesh.contact_at(f32::NAN, 5.0, 0.0).is_none());
    }

    #[test]
    fn the_normal_of_a_banked_quad_reproduces_its_bank() {
        // Rising to the left (+y) at sin(bank) per metre, like the
        // centerline's banking shear; sloping up along +x at 3%.
        let bank = 0.2f32;
        let grade = 0.03f32;
        let mut file = empty();
        push_quad(
            &mut file,
            0.0,
            10.0,
            -5.0,
            5.0,
            |x, y| x * grade + y * bank.sin(),
            0,
        );
        let mesh = RoadMesh::from_file(file).unwrap();
        let hit = mesh.contact_at(4.0, 1.0, 1.0).unwrap();
        let n = hit.normal;
        assert!(n[2] > 0.0);
        assert!(
            ((-n[1] / n[2]) - bank.sin()).abs() < 1e-5,
            "cross slope {}",
            -n[1] / n[2]
        );
        assert!(
            ((-n[0] / n[2]) - grade).abs() < 1e-5,
            "grade {}",
            -n[0] / n[2]
        );
        assert!((hit.z - (4.0 * grade + 1.0 * bank.sin())).abs() < 1e-5);
    }

    #[test]
    fn clockwise_triangles_are_turned_round_and_slivers_dropped() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 1.0, 0);
        for t in &mut file.triangles {
            t.swap(1, 2);
        }
        // A vertical face and a degenerate triangle.
        let base = file.vertices.len() as u32;
        file.vertices.push([0.0, 0.0, 0.0]);
        file.vertices.push([0.0, 0.0, 1.0]);
        file.vertices.push([5.0, 0.0, 1.0]);
        file.triangles.push([base, base + 1, base + 2]);
        file.triangle_surface.push(0);
        file.triangles.push([base, base, base + 2]);
        file.triangle_surface.push(0);
        let mesh = RoadMesh::from_file(file).unwrap();
        assert_eq!(mesh.triangle_count(), 2);
        assert_eq!(mesh.dropped_count(), 2);
        let hit = mesh.contact_at(5.0, 2.0, 1.0).unwrap();
        assert!((hit.z - 1.0).abs() < 1e-6);
        assert!(hit.normal[2] > 0.99);
    }

    #[test]
    fn bad_files_are_refused() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |_, _| 0.0, 0);
        let mut bad = file.clone();
        bad.version = 7;
        assert!(RoadMesh::from_file(bad).is_err());
        let mut bad = file.clone();
        bad.triangles[0][2] = 99;
        assert!(RoadMesh::from_file(bad).is_err());
        let mut bad = file.clone();
        bad.triangle_surface[0] = 9;
        assert!(RoadMesh::from_file(bad).is_err());
        let mut bad = file.clone();
        bad.triangle_surface.pop();
        assert!(RoadMesh::from_file(bad).is_err());
        let mut bad = file.clone();
        bad.vertices[1][0] = f32::INFINITY;
        assert!(RoadMesh::from_file(bad).is_err());
        let mut bad = file.clone();
        bad.surfaces[0].friction = -1.0;
        assert!(RoadMesh::from_file(bad).is_err());
        let empty_mesh = RoadMesh::from_file(empty()).unwrap();
        assert!(empty_mesh.is_empty());
        assert!(empty_mesh.contact_at(0.0, 0.0, 0.0).is_none());
    }

    #[test]
    fn roundtrips_through_msgpack() {
        let mut file = empty();
        push_quad(&mut file, 0.0, 10.0, 0.0, 10.0, |x, _| x * 0.1, 0);
        let bytes = rmp_serde::to_vec_named(&file).unwrap();
        let back: RoadMeshFile = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(back, file);
    }

    #[test]
    fn a_large_mesh_is_found_from_every_cell() {
        // A 200 m road of 1 m quads: every metre of it answers, and the
        // grid spans several cells.
        let mut file = empty();
        for i in 0..200 {
            let x = i as f32;
            push_quad(&mut file, x, x + 1.0, -6.0, 6.0, |x, _| x * 0.01, 0);
        }
        let mesh = RoadMesh::from_file(file).unwrap();
        for i in 0..2000 {
            let x = i as f32 * 0.1 + 0.05;
            let hit = mesh
                .contact_at(x, 2.5, 5.0)
                .unwrap_or_else(|| panic!("nothing under x = {x}"));
            assert!((hit.z - x * 0.01).abs() < 1e-4, "{x}: {}", hit.z);
        }
    }
}
