//! Tests for the Unreal export bake.
//!
//! The expensive properties to get wrong here are the ones you cannot see
//! from Rust: a level that renders inside-out, geometry that drifts between
//! exports, or a track that silently bakes to nothing. Those get a test
//! each.

use std::fs;
use std::path::{Path, PathBuf};

use track_editor::ats::{
    AtsScene, Curb, Marking, MarkingKind, PitLane, Prop, PropKind, Side, Surface, SurfaceKind,
};
use track_editor::project;
use track_editor::track_data::{TrackFile, TrackNode};
use track_editor::ue_export::{self, UeMesh, UeScene, UE_SCENE_FORMAT, UE_SCENE_VERSION};
use track_editor::ue_export_io;

fn real_tracks_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real")
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
/// heading and banking all vary.
fn test_track() -> TrackFile {
    TrackFile {
        name: "Test Circuit".to_string(),
        track_id: Some("test".to_string()),
        nodes: vec![
            node(0.0, 0.0, 0.0, 0.0),
            node(200.0, 0.0, 4.0, 0.15),
            node(200.0, 200.0, 0.0, 0.15),
            node(0.0, 200.0, -3.0, 0.0),
        ],
        checkpoints: vec![],
        spawn_points: vec![],
        default_width: 12.0,
        closed_loop: true,
        raceline: vec![],
        metadata: None,
    }
}

/// A scene exercising every layer the exporter knows how to bake.
fn test_scene(track: &TrackFile) -> AtsScene {
    let mut scene = AtsScene::new_for_track(track, "Test.yaml");
    // `alloc_id` borrows the scene mutably and so does every `push` below,
    // so hand out ids from a plain counter and stamp the scene at the end.
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
    });
    scene.surfaces.push(Surface {
        id: id(),
        kind: SurfaceKind::Grass,
        side: Side::Left,
        // Full lap: start == end on a closed loop.
        start_m: 0.0,
        end_m: 0.0,
        inner_m: 0.0,
        width_m: 60.0,
        end_width_m: None,
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
        // Wraps through start/finish.
        start_m: 780.0,
        end_m: 25.0,
        width_m: 1.0,
        style: "yellow_black".to_string(),
    });
    scene.markings.push(Marking {
        id: id(),
        kind: MarkingKind::EdgeLine,
        start_m: 10.0,
        end_m: 60.0,
        lat_from_m: 5.5,
        lat_to_m: 6.0,
        color: [1.0, 1.0, 1.0, 1.0],
    });
    scene.props.push(Prop {
        id: id(),
        kind: PropKind::Tree,
        asset: "tree_oak_large".to_string(),
        x: 30.0,
        y: 25.0,
        z: 0.0,
        yaw_rad: std::f32::consts::FRAC_PI_2,
        scale: 1.4,
        text: None,
    });
    scene.pit_lane = Some(PitLane {
        nodes: vec![[0.0, -20.0, 0.0], [80.0, -20.0, 0.0], [160.0, -18.0, 1.0]],
        width_m: 12.0,
        box_count: 10,
        speed_limit_kmh: 60.0,
    });
    scene.next_id = next;
    scene
}

fn bake_test_scene() -> UeScene {
    let track = test_track();
    let scene = test_scene(&track);
    ue_export::bake(&track, &scene).expect("test track must bake")
}

fn vertex(mesh: &UeMesh, i: u32) -> [f64; 3] {
    let i = i as usize * 3;
    [
        mesh.positions[i] as f64,
        mesh.positions[i + 1] as f64,
        mesh.positions[i + 2] as f64,
    ]
}

fn normal(mesh: &UeMesh, i: u32) -> [f64; 3] {
    let i = i as usize * 3;
    [
        mesh.normals[i] as f64,
        mesh.normals[i + 1] as f64,
        mesh.normals[i + 2] as f64,
    ]
}

fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn length(v: [f64; 3]) -> f64 {
    dot(v, v).sqrt()
}

/// The normal Unreal would derive for a triangle, or `None` when the
/// triangle is too much of a sliver to carry an orientation at all.
///
/// `FStaticMeshOperations` takes a triangle's normal as
/// `-cross(p1 - p0, p2 - p0)` — Unreal's front face is clockwise seen from
/// the normal. Degeneracy is judged on the *relative* area (the sine of the
/// angle between the two edges), not the absolute cross product: at track
/// scale a 3-degree sliver still has a cross product thousands of units
/// long while being pure numerical noise.
fn unreal_face_normal(mesh: &UeMesh, a: u32, b: u32, c: u32) -> Option<[f64; 3]> {
    let e1 = sub(vertex(mesh, b), vertex(mesh, a));
    let e2 = sub(vertex(mesh, c), vertex(mesh, a));
    let n = cross(e1, e2);
    let scale = length(e1) * length(e2);
    // Slightly below the exporter's own MIN_FACET_QUALITY, so that rounding
    // positions to a tenth of a centimetre on the way out cannot nudge a
    // triangle the exporter deliberately kept out of the test's reach.
    if scale < 1e-9 || length(n) / scale < 0.005 {
        return None;
    }
    Some([-n[0], -n[1], -n[2]])
}

/// The single most expensive thing to get wrong: winding.
///
/// Unreal's front face is clockwise seen from the normal, which is to say
/// `FStaticMeshOperations` derives a triangle's normal as
/// `-cross(p1 - p0, p2 - p0)`. Assert that for every triangle in the bake
/// that derived normal agrees with the normal we shipped alongside it. If it
/// does not, the level loads with every surface visible only from below.
#[test]
fn winding_matches_unreals_front_face_convention() {
    let baked = bake_test_scene();
    assert!(!baked.meshes.is_empty());

    let mut checked = 0usize;
    for mesh in &baked.meshes {
        assert_eq!(
            mesh.indices.len() % 3,
            0,
            "{}: not a triangle list",
            mesh.name
        );
        for tri in mesh.indices.chunks_exact(3) {
            let (a, b, c) = (tri[0], tri[1], tri[2]);
            let Some(unreal_normal) = unreal_face_normal(mesh, a, b, c) else {
                continue;
            };
            assert!(
                dot(unreal_normal, normal(mesh, a)) > 0.0,
                "{}: triangle {a},{b},{c} is wound inside-out (Unreal would light it from below)",
                mesh.name
            );
            checked += 1;
        }
    }
    assert!(checked > 1000, "only checked {checked} triangles");
}

/// Curbs are the one element with a genuine 3D profile; a flat curb means
/// the profile builder regressed to the viewport's preview strip.
#[test]
fn curbs_are_raised_above_the_track() {
    let baked = bake_test_scene();
    let curb_meshes: Vec<&UeMesh> = baked
        .meshes
        .iter()
        .filter(|m| m.material_key.starts_with("curb_"))
        .collect();
    assert!(!curb_meshes.is_empty(), "no curb meshes were baked");

    for mesh in curb_meshes {
        let heights: Vec<f32> = mesh.positions.chunks_exact(3).map(|p| p[2]).collect();
        let lo = heights.iter().copied().fold(f32::INFINITY, f32::min);
        let hi = heights.iter().copied().fold(f32::NEG_INFINITY, f32::max);
        assert!(
            hi - lo > 3.0,
            "{}: curb spans only {:.2} cm vertically — profile is flat",
            mesh.name,
            hi - lo
        );
    }
}

/// Banked and sloped sections must tilt their normals; a bake that hardcodes
/// "up" (which is what the viewport preview does) would pass every other
/// test here.
#[test]
fn road_normals_follow_the_surface_rather_than_pointing_straight_up() {
    let baked = bake_test_scene();
    let road: Vec<&UeMesh> = baked
        .meshes
        .iter()
        .filter(|m| m.material_key == "road")
        .collect();
    assert!(!road.is_empty());

    let mut tilted = 0usize;
    for mesh in &road {
        for n in mesh.normals.chunks_exact(3) {
            let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
            assert!((len - 1.0).abs() < 0.01, "normal not unit length: {n:?}");
            assert!(n[2] > 0.0, "road normal points downward: {n:?}");
            if n[2] < 0.999 {
                tilted += 1;
            }
        }
    }
    assert!(
        tilted > 0,
        "no road normal tilts — the hill in the test track was ignored"
    );
}

#[test]
fn every_mesh_is_finite_non_empty_and_indexes_in_range() {
    let baked = bake_test_scene();
    for mesh in &baked.meshes {
        let vertex_count = mesh.positions.len() / 3;
        assert!(vertex_count > 0, "{}: no vertices", mesh.name);
        assert_eq!(mesh.positions.len() % 3, 0);
        assert_eq!(mesh.normals.len(), mesh.positions.len());
        assert_eq!(mesh.uvs.len(), vertex_count * 2);
        assert!(!mesh.indices.is_empty(), "{}: no triangles", mesh.name);
        assert!(
            mesh.positions.iter().all(|v| v.is_finite()),
            "{}: non-finite position",
            mesh.name
        );
        assert!(
            mesh.normals.iter().all(|v| v.is_finite()),
            "{}: non-finite normal",
            mesh.name
        );
        assert!(
            mesh.uvs.iter().all(|v| v.is_finite()),
            "{}: non-finite uv",
            mesh.name
        );
        assert!(
            mesh.indices.iter().all(|i| (*i as usize) < vertex_count),
            "{}: index out of range",
            mesh.name
        );
    }
}

#[test]
fn every_mesh_references_a_declared_material() {
    let baked = bake_test_scene();
    for mesh in &baked.meshes {
        assert!(
            baked.materials.iter().any(|m| m.key == mesh.material_key),
            "{} references undeclared material {}",
            mesh.name,
            mesh.material_key
        );
    }
    // Sorted and deduplicated, so the commandlet can bind them in order.
    let keys: Vec<&str> = baked.materials.iter().map(|m| m.key.as_str()).collect();
    let mut sorted = keys.clone();
    sorted.sort_unstable();
    sorted.dedup();
    assert_eq!(keys, sorted, "material table is unsorted or has duplicates");
}

#[test]
fn mesh_names_are_unique() {
    let baked = bake_test_scene();
    let mut names: Vec<&str> = baked.meshes.iter().map(|m| m.name.as_str()).collect();
    let before = names.len();
    names.sort_unstable();
    names.dedup();
    assert_eq!(
        before,
        names.len(),
        "duplicate mesh names would clash as assets"
    );
}

#[test]
fn scene_header_and_gameplay_anchors_are_populated() {
    let baked = bake_test_scene();
    assert_eq!(baked.format, UE_SCENE_FORMAT);
    assert_eq!(baked.version, UE_SCENE_VERSION);
    assert_eq!(baked.track_name, "Test Circuit");
    assert!(baked.closed_loop);
    assert!(baked.length_cm > 70_000.0, "length {}", baked.length_cm);

    // No authored spawn points: the exporter mirrors the server's 16-slot
    // fallback grid so client and server agree on where a car starts.
    assert_eq!(baked.grid.len(), 16);
    assert_eq!(baked.grid[0].position, 1);
    assert!(baked.centerline.len() > 100);
    assert_eq!(baked.centerline[0].s_cm, 0.0);
    assert!(baked.centerline[0].half_left_cm > 0.0);

    assert_eq!(baked.props.len(), 1);
    let prop = &baked.props[0];
    assert_eq!(prop.asset, "tree_oak_large");
    // Track (30, 25) m -> UE (3000, -2500) cm; yaw +90 deg CCW -> -90 deg.
    assert!((prop.location[0] - 3000.0).abs() < 0.5);
    assert!((prop.location[1] + 2500.0).abs() < 0.5);
    assert!((prop.yaw_deg + 90.0).abs() < 0.01);

    let pit = baked.pit_lane.expect("pit lane should export");
    assert_eq!(pit.box_count, 10);
    assert!(baked.meshes.iter().any(|m| m.material_key == "pit_lane"));
}

/// Authored spawn points win over the fallback grid, resolved exactly the
/// way `server/src/track_loader.rs` resolves them.
#[test]
fn authored_spawn_points_replace_the_fallback_grid() {
    let mut track = test_track();
    track.spawn_points = vec![
        track_editor::track_data::SpawnPoint {
            position: 0,
            offset_x: 2.0,
            offset_y: -3.0,
        },
        track_editor::track_data::SpawnPoint {
            position: 10,
            offset_x: 0.0,
            offset_y: 0.0,
        },
    ];
    let scene = test_scene(&track);
    let baked = ue_export::bake(&track, &scene).unwrap();
    assert_eq!(baked.grid.len(), 2);
    assert_eq!(baked.grid[0].position, 1);
    // Offsets are world-space in the server, so they land straight on the
    // sampled centerline point (with Y mirrored for Unreal).
    assert!((baked.grid[0].location[0] - 200.0).abs() < 1.0);
    assert!((baked.grid[0].location[1] - 300.0).abs() < 1.0);
}

#[test]
fn repeated_bakes_are_byte_identical() {
    let track = test_track();
    let scene = test_scene(&track);
    let first = serde_json::to_vec(&ue_export::bake(&track, &scene).unwrap()).unwrap();
    let second = serde_json::to_vec(&ue_export::bake(&track, &scene).unwrap()).unwrap();
    assert_eq!(first, second, "bake is not deterministic");
}

#[test]
fn export_roundtrips_through_disk() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("Test.uescene.json");
    let baked = bake_test_scene();
    ue_export_io::write_scene(&path, &baked).unwrap();
    let read_back = ue_export_io::read_scene(&path).unwrap();
    assert_eq!(baked, read_back);
}

#[test]
fn writing_an_export_twice_produces_the_same_bytes() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("Test.uescene.json");
    let baked = bake_test_scene();
    ue_export_io::write_scene(&path, &baked).unwrap();
    let first = fs::read(&path).unwrap();
    ue_export_io::write_scene(&path, &baked).unwrap();
    assert_eq!(first, fs::read(&path).unwrap());
}

/// Exporting must never touch the files it reads.
#[test]
fn exporting_leaves_the_source_track_and_scene_untouched() {
    let dir = tempfile::tempdir().unwrap();
    let yaml = dir.path().join("Mini.yaml");
    fs::write(
        &yaml,
        "name: Mini\nnodes:\n  - { x: 0.0, y: 0.0 }\n  - { x: 100.0, y: 0.0 }\n  - { x: 200.0, y: 40.0 }\ndefault_width: 10.0\nclosed_loop: false\n",
    )
    .unwrap();
    let yaml_before = fs::read(&yaml).unwrap();

    let out = dir.path().join("export");
    ue_export_io::export_track(&yaml, &out).unwrap();

    assert_eq!(
        fs::read(&yaml).unwrap(),
        yaml_before,
        "the YAML was written to"
    );
    assert!(
        !dir.path().join("Mini.ats").exists(),
        "exporting must not create an .ats"
    );
}

/// The real payload: every shipped circuit has to bake without a hand-hold.
#[test]
fn every_real_track_bakes() {
    let dir = real_tracks_dir();
    let tracks = ue_export_io::track_files_in(&dir).expect("content/tracks/real must be readable");
    assert!(
        tracks.len() >= 20,
        "expected the full track set, found {}",
        tracks.len()
    );

    for track_path in tracks {
        let opened = project::open_project(&track_path)
            .unwrap_or_else(|e| panic!("{}: {e}", track_path.display()));
        let scene = opened
            .scene
            .unwrap_or_else(|| panic!("{}: scene failed to load", track_path.display()));
        let baked = ue_export::bake(&opened.track, &scene)
            .unwrap_or_else(|| panic!("{}: produced no geometry", track_path.display()));

        assert!(
            baked.meshes.iter().any(|m| m.material_key == "road"),
            "{}: baked no road surface",
            track_path.display()
        );
        assert!(
            !baked.grid.is_empty(),
            "{}: baked no starting grid",
            track_path.display()
        );
        for mesh in &baked.meshes {
            let vertex_count = mesh.positions.len() / 3;
            assert!(
                mesh.positions.iter().all(|v| v.is_finite()),
                "{} / {}: non-finite geometry",
                track_path.display(),
                mesh.name
            );
            assert!(
                mesh.indices.iter().all(|i| (*i as usize) < vertex_count),
                "{} / {}: index out of range",
                track_path.display(),
                mesh.name
            );
            // Real circuits are where inversion actually bites: hairpins
            // make wide runoff patches fold back through the centre of
            // curvature. Check the winding on the shipped geometry, not
            // just on the synthetic track.
            for tri in mesh.indices.chunks_exact(3) {
                let (a, b, c) = (tri[0], tri[1], tri[2]);
                let Some(unreal_normal) = unreal_face_normal(mesh, a, b, c) else {
                    continue;
                };
                assert!(
                    dot(unreal_normal, normal(mesh, a)) > 0.0,
                    "{} / {}: triangle {a},{b},{c} is wound inside-out",
                    track_path.display(),
                    mesh.name
                );
            }
        }
    }
}
