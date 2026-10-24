//! The road mesh sidecar: the road the server drives on is the road the
//! client draws.
//!
//! Every probe here reads the file through the bytes on disk and asks it
//! the server's question (`contact_at` below is the server's
//! `RoadMesh::contact_at` without its grid: of the triangles under a point,
//! the highest at or below the reference height plus a step, the later one
//! of a near tie), so what is checked is what the sim will see, not what
//! the exporter meant. The file's type is the formats crate's, which the
//! server decodes it with.

use std::path::{Path, PathBuf};

use track_core::ats::{AtsScene, Curb, PitLane, Side, Surface, SurfaceKind};
use track_core::project;
use track_core::road_mesh::{
    self, RoadMeshFile, RoadSurface, CONTACT_CURB, CONTACT_OFF, CONTACT_PIT_LANE, CONTACT_ROAD,
    CONTACT_RUNOFF, MIN_FOOTPRINT_M2,
};
use track_core::track_data::{TrackFile, TrackNode};
use track_core::track_path::{curvature_at, offset_point, CenterlinePath};
use track_core::ue_export::{self, BakeOptions, Baked};
use track_core::ue_export_io;

fn real_tracks_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tracks/default")
}

fn node(x: f32, y: f32, z: f32, banking: f32) -> TrackNode {
    TrackNode {
        x,
        y,
        z,
        width: Some(12.0),
        width_left: None,
        width_right: None,
        banking: (banking != 0.0).then_some(banking),
        friction: None,
        surface_type: None,
    }
}

/// A closed square with a hill on one side and a banked corner, so slope,
/// heading and banking all vary (the export tests' track).
fn test_track() -> TrackFile {
    TrackFile {
        name: "Test Circuit".to_string(),
        display_name: None,
        track_id: Some("test".to_string()),
        nodes: vec![
            node(0.0, 0.0, 0.0, 0.0),
            node(200.0, 0.0, 4.0, 0.15),
            node(200.0, 200.0, 0.0, 0.15),
            node(0.0, 200.0, -3.0, 0.0),
        ],
        checkpoints: vec![],
        sectors: vec![],
        spawn_points: vec![],
        default_width: 12.0,
        closed_loop: true,
        raceline: vec![],
        drs_zones: Vec::new(),
        metadata: None,
    }
}

/// Curbs on both sides, a gravel wedge on the right, grass the whole lap
/// on the left, tarmac run-off at the far end and a pit lane.
fn test_scene(track: &TrackFile) -> AtsScene {
    let mut scene = AtsScene::new_for_track(track, "Test.yaml");
    let mut next = scene.next_id;
    let mut id = || {
        next += 1;
        next - 1
    };
    scene.surfaces.push(Surface {
        id: id(),
        kind: SurfaceKind::Gravel,
        side: Side::Right,
        start_m: 40.0,
        end_m: 120.0,
        inner_m: 1.5,
        width_m: 8.0,
        end_width_m: Some(20.0),
        paint: None,
    });
    scene.surfaces.push(Surface {
        id: id(),
        kind: SurfaceKind::Grass,
        side: Side::Left,
        start_m: 0.0,
        end_m: 0.0,
        inner_m: 0.0,
        width_m: 60.0,
        end_width_m: None,
        paint: None,
    });
    scene.surfaces.push(Surface {
        id: id(),
        kind: SurfaceKind::AsphaltRunoff,
        side: Side::Right,
        start_m: 300.0,
        end_m: 360.0,
        inner_m: 0.0,
        width_m: 10.0,
        end_width_m: None,
        paint: Some("red_white".to_string()),
    });
    scene.curbs.push(Curb {
        id: id(),
        side: Side::Left,
        start_m: 30.0,
        end_m: 90.0,
        width_m: 1.2,
        style: "red_white".to_string(),
    });
    scene.curbs.push(Curb {
        id: id(),
        side: Side::Right,
        start_m: 780.0,
        end_m: 25.0,
        width_m: 1.0,
        style: "yellow_black".to_string(),
    });
    scene.pit_lane = Some(PitLane {
        nodes: vec![[0.0, -20.0, 0.0], [80.0, -20.0, 0.0], [160.0, -18.0, 1.0]],
        width_m: 12.0,
        box_count: 10,
        speed_limit_kmh: 60.0,
        authored: false,
        podium_m: None,
    });
    scene.next_id = next;
    scene
}

fn bake(options: &BakeOptions) -> (Baked, CenterlinePath) {
    let track = test_track();
    let scene = test_scene(&track);
    let baked = ue_export::bake_all_with_options(&track, &scene, None, options)
        .expect("test track must bake");
    let path = CenterlinePath::from_track(&track).unwrap();
    (baked, path)
}

/// The server's `STEP_UP_M`, `TIE_M` and point-in-triangle slack.
const STEP_UP_M: f32 = 0.5;
const TIE_M: f32 = 0.02;
const EDGE_EPSILON: f32 = 1e-4;

/// What the mesh says is under a point.
#[derive(Debug, Clone, Copy)]
struct MeshContact {
    z: f32,
    normal: [f32; 3],
    surface: u16,
}

/// The file as the server reads it, with its question, by brute force.
/// A kept triangle: its corners, unit normal, plane offset and surface.
type KeptTriangle = ([[f32; 3]; 3], [f32; 3], f32, u16);

struct RoadMesh {
    file: RoadMeshFile,
    /// Triangles kept (with a footprint), wound counter-clockwise, with
    /// their unit normal and plane offset.
    kept: Vec<KeptTriangle>,
    dropped: usize,
}

impl RoadMesh {
    fn from_file(file: RoadMeshFile) -> Self {
        assert_eq!(file.version, road_mesh::ROAD_MESH_VERSION);
        assert_eq!(file.triangle_surface.len(), file.triangles.len());
        let mut kept = Vec::new();
        let mut dropped = 0;
        for (tri, &surface) in file.triangles.iter().zip(&file.triangle_surface) {
            let v = tri.map(|i| file.vertices[i as usize]);
            let (a, mut b, mut c) = (v[0], v[1], v[2]);
            let twice = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if !twice.is_finite() || twice.abs() < 2.0 * MIN_FOOTPRINT_M2 {
                dropped += 1;
                continue;
            }
            if twice < 0.0 {
                std::mem::swap(&mut b, &mut c);
            }
            let (u, w) = (
                [b[0] - a[0], b[1] - a[1], b[2] - a[2]],
                [c[0] - a[0], c[1] - a[1], c[2] - a[2]],
            );
            let n = [
                u[1] * w[2] - u[2] * w[1],
                u[2] * w[0] - u[0] * w[2],
                u[0] * w[1] - u[1] * w[0],
            ];
            let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
            if len.is_nan() || len <= 0.0 || n[2] <= 0.0 {
                dropped += 1;
                continue;
            }
            let n = [n[0] / len, n[1] / len, n[2] / len];
            let d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
            kept.push(([a, b, c], n, d, surface));
        }
        Self {
            file,
            kept,
            dropped,
        }
    }

    fn triangle_count(&self) -> usize {
        self.kept.len()
    }

    fn dropped_count(&self) -> usize {
        self.dropped
    }

    fn surfaces(&self) -> &[RoadSurface] {
        &self.file.surfaces
    }

    fn surface(&self, idx: u16) -> &RoadSurface {
        &self.file.surfaces[idx as usize]
    }

    fn contact_at(&self, x: f32, y: f32, z_ref: f32) -> Option<MeshContact> {
        let ceiling = z_ref + STEP_UP_M;
        let mut best: Option<MeshContact> = None;
        for &([a, b, c], n, d, surface) in &self.kept {
            let twice = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            let w0 = ((b[0] - x) * (c[1] - y) - (b[1] - y) * (c[0] - x)) / twice;
            let w1 = ((c[0] - x) * (a[1] - y) - (c[1] - y) * (a[0] - x)) / twice;
            let w2 = 1.0 - w0 - w1;
            if w0 < -EDGE_EPSILON || w1 < -EDGE_EPSILON || w2 < -EDGE_EPSILON {
                continue;
            }
            let z = (d - n[0] * x - n[1] * y) / n[2];
            if z > ceiling {
                continue;
            }
            if best.is_none_or(|b| z >= b.z - TIE_M) {
                best = Some(MeshContact {
                    z,
                    normal: n,
                    surface,
                });
            }
        }
        best
    }
}

/// The file as the server reads it: through the bytes it is written as.
fn server_mesh(file: &RoadMeshFile) -> RoadMesh {
    let bytes = rmp_serde::to_vec_named(file).unwrap();
    RoadMesh::from_file(rmp_serde::from_slice(&bytes).unwrap())
}

fn key_of<'a>(mesh: &'a RoadMesh, hit: &MeshContact) -> &'a str {
    &mesh.surface(hit.surface).key
}

/// Probe laterals across the road at `station`, inset from both edges,
/// and short of the exporter's curvature clamp (`0.85 / κ`), which pulls
/// the inside edge of a tight bend in.
fn road_laterals(path: &CenterlinePath, station: f32, step: f32, inset: f32) -> Vec<f32> {
    let sample = path.sample_at(station);
    let kappa = curvature_at(path, station);
    let mut lo = -sample.width_right_m + inset;
    let mut hi = sample.width_left_m - inset;
    if kappa.abs() > 1e-6 {
        let limit = 0.85 / kappa.abs() - 0.1;
        if kappa > 0.0 {
            hi = hi.min(limit);
        } else {
            lo = lo.max(-limit);
        }
    }
    let mut out = Vec::new();
    let mut lat = lo;
    while lat <= hi {
        out.push(lat);
        lat += step;
    }
    out
}

#[test]
fn the_road_mesh_covers_the_road_and_matches_the_rendered_road() {
    let (baked, path) = bake(&BakeOptions::default());
    assert!(road_mesh::validate(&baked.road).is_ok());
    assert_eq!(baked.road.version, road_mesh::ROAD_MESH_VERSION);
    // What the physics bake drops is a curb's outer face or a sliver of a
    // ground band swept round a bend; nothing of the road itself.
    let road_drops: Vec<_> = baked
        .road_dropped
        .iter()
        .filter(|d| d.surface == "road" || d.surface == "pit_lane")
        .collect();
    assert!(road_drops.is_empty(), "dropped: {road_drops:?}");
    assert!(
        baked.road_dropped.iter().any(|d| d.surface == "curb"),
        "the curbs' outer faces are reported"
    );
    let mesh = server_mesh(&baked.road);
    assert!(
        mesh.triangle_count() > 1000,
        "{} triangles",
        mesh.triangle_count()
    );
    assert_eq!(
        mesh.dropped_count(),
        0,
        "the file holds nothing without a footprint"
    );

    let total = path.total_length_m();
    let mut probes = 0;
    let mut misses = Vec::new();
    let mut worst = 0.0f32;
    let mut station = 0.0f32;
    while station < total {
        for lat in road_laterals(&path, station, 0.25, 0.05) {
            let sample = path.sample_at(station);
            let p = offset_point(&sample, lat);
            probes += 1;
            match mesh.contact_at(p.0, p.1, p.2 + 1.0) {
                None => misses.push((station, lat)),
                Some(hit) => {
                    assert_eq!(key_of(&mesh, &hit), "road", "station {station} lat {lat}");
                    assert_eq!(mesh.surface(hit.surface).contact, CONTACT_ROAD);
                    assert!(mesh.surface(hit.surface).valid_track);
                    worst = worst.max((hit.z - p.2).abs());
                    assert!(hit.normal[2] > 0.9, "normal {:?}", hit.normal);
                }
            }
        }
        station += 1.0;
    }
    assert!(probes > 10_000);
    assert!(
        misses.is_empty(),
        "{} of {probes} road probes hit nothing, first {:?}",
        misses.len(),
        &misses[..misses.len().min(10)]
    );
    // Rounded to 0.1 mm; the rest is a twisting banked quad drawn as two
    // flat triangles.
    assert!(worst < 5e-3, "road height off by up to {worst} m");
}

#[test]
fn curbs_carry_their_profile_unless_flattened() {
    let (profiled, path) = bake(&BakeOptions::default());
    let (flat, _) = bake(&BakeOptions { flat_curbs: true });
    let profiled = server_mesh(&profiled.road);
    let flat = server_mesh(&flat.road);
    // The left curb runs 30..90 m, 1.2 m wide; probe its top.
    let sample = path.sample_at(60.0);
    let edge = offset_point(&sample, sample.width_left_m);
    let top = offset_point(&sample, sample.width_left_m + 1.2 * 0.95);
    let raised = profiled
        .contact_at(top.0, top.1, edge.2 + 1.0)
        .expect("curb top");
    assert_eq!(key_of(&profiled, &raised), "curb");
    assert_eq!(profiled.surface(raised.surface).contact, CONTACT_CURB);
    assert!(
        (raised.z - (edge.2 + 0.05)).abs() < 4e-3,
        "curb top {} against edge {}",
        raised.z,
        edge.2
    );
    let flattened = flat
        .contact_at(top.0, top.1, edge.2 + 1.0)
        .expect("flat curb");
    assert_eq!(key_of(&flat, &flattened), "curb");
    assert!(
        (flattened.z - edge.2).abs() < 2e-3,
        "flat curb {} against edge {}",
        flattened.z,
        edge.2
    );
    // The inner edge of the curb meets the road's height either way.
    let inner = offset_point(&sample, sample.width_left_m + 0.02);
    let at_edge = profiled.contact_at(inner.0, inner.1, edge.2 + 1.0).unwrap();
    assert!(
        (at_edge.z - edge.2).abs() < 1e-2,
        "{} vs {}",
        at_edge.z,
        edge.2
    );
}

#[test]
fn bands_and_the_pit_lane_are_surfaces_with_their_class() {
    let (baked, path) = bake(&BakeOptions::default());
    let mesh = server_mesh(&baked.road);
    let keys: Vec<&str> = mesh.surfaces().iter().map(|s| s.key.as_str()).collect();
    for key in [
        "road",
        "curb",
        "off_gravel",
        "off_grass",
        "runoff_asphalt",
        "pit_lane",
    ] {
        assert!(keys.contains(&key), "no {key} in {keys:?}");
    }
    for s in mesh.surfaces() {
        assert_eq!(
            s.friction, 1.0,
            "{}: a generated mesh changes no grip",
            s.key
        );
        assert_eq!(
            s.valid_track,
            matches!(s.contact, CONTACT_ROAD | CONTACT_CURB),
            "{}",
            s.key
        );
    }

    // Gravel on the right at station 80 (1.5 m in from the edge).
    let sample = path.sample_at(80.0);
    let edge = offset_point(&sample, -sample.width_right_m);
    let p = offset_point(&sample, -(sample.width_right_m + 1.5 + 2.0));
    let hit = mesh.contact_at(p.0, p.1, edge.2 + 1.0).expect("gravel");
    assert_eq!(key_of(&mesh, &hit), "off_gravel");
    assert_eq!(mesh.surface(hit.surface).contact, CONTACT_OFF);
    assert!(
        hit.z <= edge.2 + 1e-3,
        "the gravel sits at or below the road edge"
    );

    // Grass on the left the whole lap (station 350 is on the second leg,
    // clear of the pit lane).
    let sample = path.sample_at(350.0);
    let edge = offset_point(&sample, sample.width_left_m);
    let p = offset_point(&sample, sample.width_left_m + 5.0);
    let hit = mesh.contact_at(p.0, p.1, edge.2 + 1.0).expect("grass");
    assert_eq!(key_of(&mesh, &hit), "off_grass");

    // Tarmac run-off, painted or not, is run-off.
    let sample = path.sample_at(330.0);
    let edge = offset_point(&sample, -sample.width_right_m);
    let p = offset_point(&sample, -(sample.width_right_m + 4.0));
    let hit = mesh.contact_at(p.0, p.1, edge.2 + 1.0).expect("run-off");
    assert_eq!(key_of(&mesh, &hit), "runoff_asphalt");
    assert_eq!(mesh.surface(hit.surface).contact, CONTACT_RUNOFF);
    assert!(!mesh.surface(hit.surface).valid_track);

    // The pit lane, at its own height, along its polyline: where the
    // lane's tapers lie on the road the road is what is there (the lane's
    // facets on it are clipped out of the physics mesh), everywhere else
    // the lane.
    let mut lane_hits = 0;
    for i in 0..=32 {
        let x = i as f32 * 5.0;
        let y = if x <= 80.0 {
            -20.0
        } else {
            -20.0 + (x - 80.0) / 80.0 * 2.0
        };
        let hit = mesh
            .contact_at(x, y, 10.0)
            .unwrap_or_else(|| panic!("nothing under the pit lane at ({x}, {y})"));
        match key_of(&mesh, &hit) {
            "pit_lane" => {
                lane_hits += 1;
                assert_eq!(mesh.surface(hit.surface).contact, CONTACT_PIT_LANE);
                assert!(mesh.surface(hit.surface).pit_lane);
                assert!(hit.z.abs() < 1.5, "pit lane at ({x}, {y}) is at {}", hit.z);
            }
            "road" => {}
            other => panic!("{other} under the pit lane at ({x}, {y})"),
        }
    }
    assert!(
        lane_hits >= 3,
        "only {lane_hits} probes along the lane read pit lane"
    );
}

#[test]
fn the_road_sidecar_is_deterministic_and_roundtrips_through_disk() {
    let (first, _) = bake(&BakeOptions::default());
    let (second, _) = bake(&BakeOptions::default());
    let a = rmp_serde::to_vec_named(&first.road).unwrap();
    let b = rmp_serde::to_vec_named(&second.road).unwrap();
    assert_eq!(a, b, "the road mesh bake is not deterministic");

    let dir = tempfile::tempdir().unwrap();
    let path = ue_export_io::road_sidecar_path_for(&dir.path().join("Test.yaml"));
    assert!(path.ends_with("Test.road.msgpack"));
    ue_export_io::write_road_sidecar(&path, &first.road).unwrap();
    let back: RoadMeshFile = rmp_serde::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(back, first.road);
    // And a reader of the very same bytes keeps every triangle.
    let mesh = RoadMesh::from_file(back);
    assert_eq!(mesh.triangle_count(), first.road.triangles.len());
    assert_eq!(mesh.file.source, "ats-export");
}

/// Every real circuit's road mesh covers its road, short of the curvature
/// clamp, and sits on the rendered road's height. Bakes every circuit:
/// run with `cargo test -p track-core --test road_mesh -- --ignored`.
#[test]
#[ignore]
fn every_real_track_road_mesh_covers_the_road() {
    let dir = real_tracks_dir();
    let tracks = ue_export_io::track_files_in(&dir).expect("tracks/default must be readable");
    assert!(tracks.len() >= 20);
    let mut report: Vec<(String, usize, usize, f32, usize)> = Vec::new();
    for track_path in tracks {
        let opened = project::open_project(&track_path)
            .unwrap_or_else(|e| panic!("{}: {e}", track_path.display()));
        let scene = opened.scene.unwrap_or_else(|| {
            AtsScene::new_for_track(
                &opened.track,
                &track_path.file_name().unwrap_or_default().to_string_lossy(),
            )
        });
        let dem = track_core::dem::load_dem(track_core::dem::dem_path_for(&track_path))
            .unwrap_or_else(|e| panic!("{}: {e}", track_path.display()));
        let baked = ue_export::bake_all_with_dem(&opened.track, &scene, dem.as_ref())
            .expect("real track must bake");
        let path = CenterlinePath::from_track(&opened.track).unwrap();
        let mesh = server_mesh(&baked.road);
        let total = path.total_length_m();
        let (mut probes, mut misses, mut worst) = (0usize, 0usize, 0.0f32);
        let mut station = 0.0f32;
        while station < total {
            let sample = path.sample_at(station);
            for lat in road_laterals(&path, station, 0.5, 0.1) {
                let p = offset_point(&sample, lat);
                probes += 1;
                match mesh.contact_at(p.0, p.1, p.2 + 1.0) {
                    None => {
                        misses += 1;
                        if misses <= 5 {
                            eprintln!(
                                "  {}: nothing under station {station:.0} lat {lat:.2}",
                                track_path.display()
                            );
                        }
                    }
                    Some(hit) => worst = worst.max((hit.z - p.2).abs()),
                }
            }
            station += 2.0;
        }
        let name = track_path
            .file_stem()
            .unwrap()
            .to_string_lossy()
            .into_owned();
        eprintln!(
            "{name}: {} triangles, {probes} probes, {misses} misses, worst height {worst:.4} m, {} facets dropped",
            mesh.triangle_count(),
            baked.road_dropped.len()
        );
        report.push((name, probes, misses, worst, mesh.triangle_count()));
    }
    let offenders: Vec<_> = report
        .iter()
        .filter(|(_, _, misses, worst, _)| *misses > 0 || *worst > 5e-3)
        .collect();
    assert!(
        offenders.is_empty(),
        "road mesh holes or drift: {offenders:?}"
    );
}
