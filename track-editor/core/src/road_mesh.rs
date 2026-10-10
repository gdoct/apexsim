//! The road as the *server* drives on it: `<Track>.road.msgpack`.
//!
//! A triangle mesh of every surface a wheel can stand on — the road, the
//! curbs, the run-off and ground bands, the pit lane — each triangle tagged
//! with a surface (its contact class, friction and whether it is inside
//! the track limits). The server's `road_mesh.rs` reads it and takes each
//! wheel's height, normal and surface from it instead of from the
//! centerline formula, so the road the sim drives on is the road the
//! client draws; without the sidecar the sim drives on the centerline as
//! before (`docs/content/road-mesh.md`).
//!
//! The bake writes the *rendered* triangles here, minus the render-only
//! lifts against z-fighting: the physics road and the drawn road share
//! their vertices. Paint (markings, decals, wear bands, grid boxes) is not
//! a surface and is left out.
//!
//! # Frame
//!
//! The server frame, not Unreal's: metres, +X along the start line's
//! heading, +Y left, +Z up, origin at the start/finish line. Triangles are
//! wound counter-clockwise seen from above (normal up).
//!
//! # Determinism
//!
//! The builder welds vertices by exact (rounded) equality and numbers them
//! in the order they are first seen, so the same bake writes the same
//! bytes; the weld map is only ever looked up, never iterated.

use std::collections::HashMap;

use serde::{Deserialize, Serialize};

pub const ROAD_MESH_VERSION: u32 = 1;

/// Contact classes, as the server's `RoadContact` reads them.
pub const CONTACT_ROAD: u8 = 0;
pub const CONTACT_CURB: u8 = 1;
/// Prepared tarmac past the curb: asphalt to drive on, off the track for
/// the lap.
pub const CONTACT_RUNOFF: u8 = 2;
/// Grass, gravel, sand, astroturf.
pub const CONTACT_OFF: u8 = 3;
/// Drives as road; off the track for the lap.
pub const CONTACT_PIT_LANE: u8 = 4;

/// Coordinates are rounded to this many decimals (0.1 mm) before welding,
/// which is also what keeps the file small.
const COORD_DECIMALS: i32 = 4;
/// A triangle whose footprint in the ground plane is smaller than this is
/// dropped: it is a curb's vertical outer face, or a sliver nothing can
/// stand on.
pub const MIN_FOOTPRINT_M2: f32 = 1e-4;

/// One kind of surface: what the sim does with a wheel on it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadSurface {
    /// `road`, `curb`, `runoff_asphalt`, `off_grass`, `pit_lane`, or an
    /// importer's own key.
    pub key: String,
    /// [`CONTACT_ROAD`] and friends.
    pub contact: u8,
    /// Multiplier on the grip the class has on this track (1.0 = the
    /// track's own figure for that class: the node's friction on the road,
    /// `curb_grip` on a curb, `off_track_grip` off it). A generated mesh
    /// writes 1.0 everywhere, so nothing changes against the centerline;
    /// an importer can scale a surface from its own table.
    pub friction: f32,
    /// Inside the track limits: a wheel here is not off the track.
    pub valid_track: bool,
    /// The pit lane. Nothing reads it yet.
    pub pit_lane: bool,
}

impl RoadSurface {
    pub fn road() -> Self {
        Self {
            key: "road".to_string(),
            contact: CONTACT_ROAD,
            friction: 1.0,
            valid_track: true,
            pit_lane: false,
        }
    }

    pub fn curb() -> Self {
        Self {
            key: "curb".to_string(),
            contact: CONTACT_CURB,
            friction: 1.0,
            valid_track: true,
            pit_lane: false,
        }
    }

    pub fn runoff(kind: &str) -> Self {
        Self {
            key: format!("runoff_{kind}"),
            contact: CONTACT_RUNOFF,
            friction: 1.0,
            valid_track: false,
            pit_lane: false,
        }
    }

    pub fn off(kind: &str) -> Self {
        Self {
            key: format!("off_{kind}"),
            contact: CONTACT_OFF,
            friction: 1.0,
            valid_track: false,
            pit_lane: false,
        }
    }

    pub fn pit_lane() -> Self {
        Self {
            key: "pit_lane".to_string(),
            contact: CONTACT_PIT_LANE,
            friction: 1.0,
            valid_track: false,
            pit_lane: true,
        }
    }
}

/// The sidecar as written (`rmp_serde::to_vec_named`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadMeshFile {
    pub version: u32,
    /// Server frame, metres.
    pub vertices: Vec<[f32; 3]>,
    /// Counter-clockwise seen from above. Indices into `vertices`.
    pub triangles: Vec<[u32; 3]>,
    /// One entry per triangle: an index into `surfaces`.
    pub triangle_surface: Vec<u16>,
    pub surfaces: Vec<RoadSurface>,
    /// Which generator wrote it and from what: `ats-export <crc>`,
    /// `ac-import ks_zandvoort`. Logged at load; not interpreted.
    pub source: String,
}

/// A facet the builder refused: where it was and what it was part of.
#[derive(Debug, Clone, PartialEq)]
pub struct DroppedFacet {
    pub station_m: f32,
    pub surface: String,
}

/// Accumulates triangles, welding shared vertices and keeping every
/// triangle wound counter-clockwise from above.
#[derive(Debug)]
pub struct RoadMeshBuilder {
    file: RoadMeshFile,
    weld: HashMap<[u32; 3], u32>,
    /// Facets with no footprint, kept so the bake can report them.
    pub dropped: Vec<DroppedFacet>,
}

impl RoadMeshBuilder {
    pub fn new(source: impl Into<String>) -> Self {
        Self {
            file: RoadMeshFile {
                version: ROAD_MESH_VERSION,
                vertices: Vec::new(),
                triangles: Vec::new(),
                triangle_surface: Vec::new(),
                surfaces: Vec::new(),
                source: source.into(),
            },
            weld: HashMap::new(),
            dropped: Vec::new(),
        }
    }

    /// Register a surface, or find the one already registered under its
    /// key. The first registration of a key wins.
    pub fn surface(&mut self, surface: RoadSurface) -> u16 {
        if let Some(i) = self.file.surfaces.iter().position(|s| s.key == surface.key) {
            return i as u16;
        }
        self.file.surfaces.push(surface);
        (self.file.surfaces.len() - 1) as u16
    }

    pub fn source_mut(&mut self) -> &mut String {
        &mut self.file.source
    }

    fn vertex(&mut self, p: (f32, f32, f32)) -> u32 {
        let rounded = [round(p.0), round(p.1), round(p.2)];
        let key = [
            rounded[0].to_bits(),
            rounded[1].to_bits(),
            rounded[2].to_bits(),
        ];
        if let Some(&i) = self.weld.get(&key) {
            return i;
        }
        let i = self.file.vertices.len() as u32;
        self.file.vertices.push(rounded);
        self.weld.insert(key, i);
        i
    }

    /// One triangle of `surface`. Wound counter-clockwise from above
    /// whichever way it comes in; dropped, and reported at `station_m`,
    /// when its footprint in the ground plane is under
    /// [`MIN_FOOTPRINT_M2`] (a vertical face, a sliver).
    pub fn triangle(
        &mut self,
        surface: u16,
        a: (f32, f32, f32),
        b: (f32, f32, f32),
        c: (f32, f32, f32),
        station_m: f32,
    ) {
        let (a, b, c) = (round3(a), round3(b), round3(c));
        // Twice the signed footprint, positive for counter-clockwise.
        let twice_area = (b.0 - a.0) * (c.1 - a.1) - (b.1 - a.1) * (c.0 - a.0);
        if !twice_area.is_finite() || twice_area.abs() < 2.0 * MIN_FOOTPRINT_M2 {
            self.dropped.push(DroppedFacet {
                station_m,
                surface: self
                    .file
                    .surfaces
                    .get(surface as usize)
                    .map(|s| s.key.clone())
                    .unwrap_or_default(),
            });
            return;
        }
        let (b, c) = if twice_area > 0.0 { (b, c) } else { (c, b) };
        let tri = [self.vertex(a), self.vertex(b), self.vertex(c)];
        self.file.triangles.push(tri);
        self.file.triangle_surface.push(surface);
    }

    /// A strip quad `[(i,k), (i,k+1), (i+1,k+1), (i+1,k)]`, as two
    /// triangles.
    pub fn quad(&mut self, surface: u16, corners: [(f32, f32, f32); 4], station_m: f32) {
        self.triangle(surface, corners[0], corners[1], corners[2], station_m);
        self.triangle(surface, corners[0], corners[2], corners[3], station_m);
    }

    pub fn triangle_count(&self) -> usize {
        self.file.triangles.len()
    }

    pub fn finish(self) -> RoadMeshFile {
        self.file
    }
}

fn round(v: f32) -> f32 {
    let scale = 10f32.powi(COORD_DECIMALS);
    let r = (v * scale).round() / scale;
    // Never write a negative zero: it welds apart from +0.
    if r == 0.0 {
        0.0
    } else {
        r
    }
}

fn round3(p: (f32, f32, f32)) -> (f32, f32, f32) {
    (round(p.0), round(p.1), round(p.2))
}

/// Sanity checks a reader applies before trusting a file: the version,
/// every index in range, one surface per triangle, finite coordinates.
pub fn validate(file: &RoadMeshFile) -> Result<(), String> {
    if file.version != ROAD_MESH_VERSION {
        return Err(format!("unsupported version {}", file.version));
    }
    if file.triangle_surface.len() != file.triangles.len() {
        return Err(format!(
            "{} surface tags for {} triangles",
            file.triangle_surface.len(),
            file.triangles.len()
        ));
    }
    if let Some(v) = file
        .vertices
        .iter()
        .find(|v| !v.iter().all(|c| c.is_finite()))
    {
        return Err(format!("non-finite vertex {v:?}"));
    }
    let n = file.vertices.len() as u32;
    if let Some(t) = file.triangles.iter().find(|t| t.iter().any(|&i| i >= n)) {
        return Err(format!("triangle {t:?} indexes past {n} vertices"));
    }
    let s = file.surfaces.len() as u16;
    if let Some(i) = file.triangle_surface.iter().find(|&&i| i >= s) {
        return Err(format!("surface tag {i} past {s} surfaces"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn welds_shared_vertices_and_winds_counter_clockwise() {
        let mut b = RoadMeshBuilder::new("test");
        let road = b.surface(RoadSurface::road());
        // Two quads along +x sharing their middle cross-section, one of
        // them handed in clockwise.
        b.quad(
            road,
            [
                (0.0, 1.0, 0.0),
                (0.0, -1.0, 0.0),
                (1.0, -1.0, 0.0),
                (1.0, 1.0, 0.0),
            ],
            0.0,
        );
        b.quad(
            road,
            [
                (1.0, -1.0, 0.0),
                (1.0, 1.0, 0.0),
                (2.0, 1.0, 0.0),
                (2.0, -1.0, 0.0),
            ],
            1.0,
        );
        let file = b.finish();
        assert_eq!(file.vertices.len(), 6, "{:?}", file.vertices);
        assert_eq!(file.triangles.len(), 4);
        for t in &file.triangles {
            let (a, b, c) = (
                file.vertices[t[0] as usize],
                file.vertices[t[1] as usize],
                file.vertices[t[2] as usize],
            );
            let twice = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            assert!(twice > 0.0, "clockwise triangle {t:?}");
        }
        assert!(validate(&file).is_ok());
    }

    #[test]
    fn vertical_faces_are_dropped_and_reported() {
        let mut b = RoadMeshBuilder::new("test");
        let curb = b.surface(RoadSurface::curb());
        // A curb's outer face: straight down, no footprint.
        b.quad(
            curb,
            [
                (0.0, 1.0, 0.05),
                (0.0, 1.0, 0.0),
                (1.0, 1.0, 0.0),
                (1.0, 1.0, 0.05),
            ],
            42.0,
        );
        assert_eq!(b.triangle_count(), 0);
        assert_eq!(b.dropped.len(), 2);
        assert_eq!(b.dropped[0].surface, "curb");
        assert_eq!(b.dropped[0].station_m, 42.0);
    }

    #[test]
    fn surfaces_are_registered_once_per_key() {
        let mut b = RoadMeshBuilder::new("test");
        let a = b.surface(RoadSurface::off("grass"));
        let again = b.surface(RoadSurface::off("grass"));
        let other = b.surface(RoadSurface::off("gravel"));
        assert_eq!(a, again);
        assert_ne!(a, other);
        assert_eq!(b.finish().surfaces.len(), 2);
    }

    #[test]
    fn validate_catches_bad_files() {
        let mut b = RoadMeshBuilder::new("test");
        let road = b.surface(RoadSurface::road());
        b.triangle(road, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.0);
        let good = b.finish();
        assert!(validate(&good).is_ok());
        let mut bad = good.clone();
        bad.triangles[0][1] = 9;
        assert!(validate(&bad).is_err());
        let mut bad = good.clone();
        bad.triangle_surface[0] = 3;
        assert!(validate(&bad).is_err());
        let mut bad = good.clone();
        bad.vertices[0][2] = f32::NAN;
        assert!(validate(&bad).is_err());
        let mut bad = good.clone();
        bad.version = 2;
        assert!(validate(&bad).is_err());
    }

    #[test]
    fn roundtrips_through_msgpack() {
        let mut b = RoadMeshBuilder::new("ats-export 1234");
        let road = b.surface(RoadSurface::road());
        b.triangle(road, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.5), 0.0);
        let file = b.finish();
        let bytes = rmp_serde::to_vec_named(&file).unwrap();
        let back: RoadMeshFile = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(back, file);
    }
}
