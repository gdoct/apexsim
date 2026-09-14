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
use track_editor::ue_export::{
    self, UeMesh, UeScene, WallSegment, CURB_BANDS_VERSION, CURB_BAND_STEP_M, UE_SCENE_FORMAT,
    UE_SCENE_VERSION, WALLS_VERSION, WALL_KIND_ARMCO, WALL_KIND_CONCRETE, WALL_KIND_TIRES,
};
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
        length_m: None,
    });
    scene.pit_lane = Some(PitLane {
        nodes: vec![[0.0, -20.0, 0.0], [80.0, -20.0, 0.0], [160.0, -18.0, 1.0]],
        width_m: 12.0,
        box_count: 10,
        speed_limit_kmh: 60.0,
        authored: false,
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

    // The scene's own prop; the pit lane adds its generated complex on top.
    let authored: Vec<_> = baked.props.iter().filter(|p| p.kind != "pit").collect();
    assert_eq!(authored.len(), 1);
    let prop = authored[0];
    assert_eq!(prop.asset, "tree_oak_large");
    // Track (30, 25) m -> UE (3000, -2500) cm; yaw +90 deg CCW -> -90 deg.
    assert!((prop.location[0] - 3000.0).abs() < 0.5);
    assert!((prop.location[1] + 2500.0).abs() < 0.5);
    assert!((prop.yaw_deg + 90.0).abs() < 0.01);

    let pit = baked.pit_lane.expect("pit lane should export");
    assert_eq!(pit.box_count, 10);
    assert!(baked.meshes.iter().any(|m| m.material_key == "pit_lane"));

    // A scene that says nothing is dressed for a summer race day.
    assert_eq!(baked.dressing.season, "summer");
    assert!(baked.dressing.spectators);
}

/// The scene's dressing reaches the importer as words it can key on,
/// while the props keep their base asset keys: the importer swaps in the
/// `_autumn` and `_crowd` meshes itself.
#[test]
fn dressing_is_exported_without_renaming_the_props() {
    use track_editor::ats::{Dressing, Season};
    let track = test_track();
    let mut scene = test_scene(&track);
    scene.dressing = Dressing {
        season: Season::Autumn,
        spectators: false,
    };
    let id = scene.alloc_id();
    scene.props.push(track_editor::ats::Prop {
        id,
        kind: track_editor::ats::PropKind::Tree,
        asset: "broadleaf_m".to_string(),
        x: 60.0,
        y: 30.0,
        z: 0.0,
        yaw_rad: 0.0,
        scale: 1.0,
        text: None,
        length_m: None,
    });
    let baked = ue_export::bake(&track, &scene).unwrap();
    assert_eq!(baked.dressing.season, "autumn");
    assert!(!baked.dressing.spectators);
    assert_eq!(props_with(&baked, "tree", "broadleaf_m").len(), 1);
    assert!(props_with(&baked, "tree", "broadleaf_m_autumn").is_empty());
}

fn props_with<'a>(scene: &'a UeScene, kind: &str, asset: &str) -> Vec<&'a ue_export::UeProp> {
    scene
        .props
        .iter()
        .filter(|p| p.kind == kind && p.asset == asset)
        .collect()
}

/// A flat stadium, counter-clockwise: an 800 m straight along y = 0, a
/// 70 m hairpin, the return straight along y = 140 and a hairpin back.
/// Dense nodes keep the spline straight on the straights, which the
/// rounded square above is not.
fn stadium_track() -> TrackFile {
    const R: f32 = 70.0;
    let mut nodes = Vec::new();
    for i in 0..=32 {
        nodes.push(node(i as f32 * 25.0, 0.0, 0.0, 0.0));
    }
    for deg in (-80..=80).step_by(10) {
        let (sin, cos) = (deg as f32).to_radians().sin_cos();
        nodes.push(node(800.0 + R * cos, R + R * sin, 0.0, 0.0));
    }
    for i in (0..=32).rev() {
        nodes.push(node(i as f32 * 25.0, 2.0 * R, 0.0, 0.0));
    }
    for deg in (100..=260).step_by(10) {
        let (sin, cos) = (deg as f32).to_radians().sin_cos();
        nodes.push(node(R * cos, R + R * sin, 0.0, 0.0));
    }
    TrackFile {
        name: "Stadium".to_string(),
        track_id: Some("stadium".to_string()),
        nodes,
        checkpoints: vec![],
        spawn_points: vec![],
        default_width: 12.0,
        closed_loop: true,
        raceline: vec![],
        metadata: None,
    }
}

/// The stadium with a 10 m pit lane on the right of the start straight:
/// tapers off the road at both ends, 240 m of pit road parallel to it at
/// y = -20 in between.
fn stadium_scene() -> (TrackFile, AtsScene) {
    let track = stadium_track();
    let mut scene = AtsScene::new_for_track(&track, "Stadium.yaml");
    scene.pit_lane = Some(PitLane {
        nodes: {
            // Nodes every 30 m along the pit road, as `pit.rs` lays them,
            // so the spline stays straight rather than sagging between
            // two far-apart nodes.
            let mut nodes = vec![[100.0, -3.0, 0.0], [140.0, -12.0, 0.0]];
            nodes.extend((0..=8).map(|i| [160.0 + 30.0 * i as f32, -20.0, 0.0]));
            nodes.extend([[420.0, -12.0, 0.0], [460.0, -3.0, 0.0]]);
            nodes
        },
        width_m: 10.0,
        box_count: 10,
        speed_limit_kmh: 80.0,
        authored: false,
    });
    (track, scene)
}

/// The pit lane bakes its own complex: one garage per box on the far side
/// of the lane with an end block beyond each end of the row, and pit walls
/// on the road side, every module turned to face the track.
#[test]
fn the_pit_lane_bakes_a_garage_row_and_pit_walls() {
    let (track, scene) = stadium_scene();
    let baked = ue_export::bake(&track, &scene).unwrap();
    let garages = props_with(&baked, "pit", "garage_6m");
    assert_eq!(garages.len(), 10, "one garage per box");
    assert_eq!(props_with(&baked, "pit", "garage_end").len(), 2);
    // The crew's kit shares each garage's pivot and heading: authored to
    // reach out from the door onto the working lane.
    let kits = props_with(&baked, "pit", "box_kit");
    assert_eq!(kits.len(), 10, "a box kit per garage");
    for (garage, kit) in garages.iter().zip(&kits) {
        assert_eq!(garage.location, kit.location);
        assert_eq!(garage.yaw_deg, kit.yaw_deg);
    }
    let walls = props_with(&baked, "pit", "pit_wall_6m");
    assert_eq!(walls.len(), 10, "a wall module per box");
    let plain = props_with(&baked, "pit", "pit_wall_plain_6m");
    assert!(
        !plain.is_empty(),
        "plain walls fill the rest of the pit road"
    );

    // The lane runs at y = -20 m (UE +2000 cm) on the right of a road
    // along +X: garages beyond it (UE y 2500), walls between it and the
    // road (UE y 1400..1500), both looking toward the road, which is on
    // their left in the server frame — a module's front is its right-hand
    // side, so they are turned round (UE yaw ±180).
    for garage in &garages {
        assert!(
            (garage.location[1] - 2500.0).abs() < 1.0,
            "garage y {}",
            garage.location[1]
        );
        assert!(
            (garage.yaw_deg.abs() - 180.0).abs() < 0.5,
            "garage yaw {}",
            garage.yaw_deg
        );
        assert!(garage.location[0] > 16000.0 && garage.location[0] < 40000.0);
    }
    for wall in walls.iter().chain(&plain) {
        // The plain walls reach toward the tapers, where the lane already
        // bends away a little.
        let box_span = wall.asset == "pit_wall_6m";
        let range = if box_span {
            1399.0..=1501.0
        } else {
            // Toward the tapers the lane edge closes to 1.5 m off the
            // road edge, which is where a real pit wall runs.
            800.0..=1600.0
        };
        assert!(
            range.contains(&wall.location[1]),
            "wall y {}",
            wall.location[1]
        );
        if box_span {
            assert!(
                (wall.yaw_deg.abs() - 180.0).abs() < 0.5,
                "wall yaw {}",
                wall.yaw_deg
            );
        }
        // The relation counts the lane as parallel until it is within
        // 7.5 m of the road, a little way into each taper.
        let (lo, hi) = if box_span {
            (16000.0, 40000.0)
        } else {
            (14000.0, 42000.0)
        };
        assert!(
            wall.location[0] > lo && wall.location[0] < hi,
            "wall x {}",
            wall.location[0]
        );
    }
    // Boxes tile at 6 m along the lane.
    let mut xs: Vec<f32> = garages.iter().map(|g| g.location[0]).collect();
    xs.sort_by(|a, b| a.total_cmp(b));
    for pair in xs.windows(2) {
        assert!((pair[1] - pair[0] - 600.0).abs() < 1.0, "pitch {:?}", pair);
    }
}

/// The pre-kit scenes stood pit garages in as `building/pit_garage`; the
/// generated complex replaces them rather than doubling up.
#[test]
fn legacy_pit_garage_stand_ins_give_way_to_the_generated_complex() {
    let (track, mut scene) = stadium_scene();
    let id = scene.alloc_id();
    scene.props.push(Prop {
        id,
        kind: PropKind::Building,
        asset: "pit_garage".to_string(),
        x: 280.0,
        y: -34.0,
        z: 0.0,
        yaw_rad: 0.0,
        scale: 1.0,
        text: None,
        length_m: None,
    });
    let baked = ue_export::bake(&track, &scene).unwrap();
    assert!(props_with(&baked, "building", "pit_garage").is_empty());
    assert!(!props_with(&baked, "pit", "garage_6m").is_empty());

    // Without a pit lane the stand-in is exported as it always was.
    scene.pit_lane = None;
    let baked = ue_export::bake(&track, &scene).unwrap();
    assert_eq!(props_with(&baked, "building", "pit_garage").len(), 1);
    assert!(props_with(&baked, "pit", "garage_6m").is_empty());
}

/// Grandstands carry their length and the signed bend radius (positive
/// outside the corner), bridges the road width their span is scaled to.
#[test]
fn stands_and_bridges_carry_their_layout_hints() {
    let (track, mut scene) = stadium_scene();
    let mut add = |kind: PropKind, x: f32, y: f32, length_m: Option<f32>| {
        let id = scene.alloc_id();
        scene.props.push(Prop {
            id,
            kind,
            asset: "test".to_string(),
            x,
            y,
            z: 0.0,
            yaw_rad: 0.0,
            scale: 1.0,
            text: None,
            length_m,
        });
    };
    // The far hairpin turns left around (800, 70): a stand beyond it is on
    // the outside, one at its centre on the inside; the start straight
    // has no bend at all.
    add(PropKind::Grandstand, 900.0, 70.0, Some(50.0));
    add(PropKind::Grandstand, 830.0, 70.0, None);
    add(PropKind::Grandstand, 400.0, 40.0, None);
    add(PropKind::Bridge, 600.0, 0.0, None);
    let baked = ue_export::bake(&track, &scene).unwrap();

    let stands = props_with(&baked, "grandstand", "test");
    assert_eq!(stands.len(), 3);
    let outside = stands.iter().find(|p| p.location[0] > 89000.0).unwrap();
    let inside = stands
        .iter()
        .find(|p| (p.location[0] - 83000.0).abs() < 100.0)
        .unwrap();
    let straight = stands.iter().find(|p| p.location[0] < 41000.0).unwrap();
    assert_eq!(outside.length_m, Some(50.0));
    assert_eq!(inside.length_m, Some(30.0), "default length");
    assert!(
        outside.radius_m.is_some_and(|r| r > 50.0 && r < 90.0),
        "{:?}",
        outside.radius_m
    );
    assert!(
        inside.radius_m.is_some_and(|r| r < -50.0 && r > -90.0),
        "{:?}",
        inside.radius_m
    );
    assert_eq!(straight.radius_m, None);
    assert_eq!(straight.span_m, None);

    let bridge = props_with(&baked, "bridge", "test")[0];
    assert!(
        (bridge.span_m.unwrap() - 12.0).abs() < 0.05,
        "{:?}",
        bridge.span_m
    );
    assert_eq!(bridge.length_m, None);
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

/// The world must not be a void: every bake carries a terrain ground that
/// extends well past the track itself, so nothing in the level floats.
#[test]
fn ground_terrain_is_baked_under_the_scene() {
    let baked = bake_test_scene();
    let ground: Vec<&UeMesh> = baked
        .meshes
        .iter()
        .filter(|m| m.material_key == "ground")
        .collect();
    assert!(!ground.is_empty(), "no ground meshes baked");
    assert!(
        baked
            .materials
            .iter()
            .any(|m| m.key == "ground" && m.family == "surface"),
        "ground material missing"
    );

    // The test track spans 0..200 m in X; the ground has to reach beyond
    // that with its margin (positions are UE centimeters).
    let xs: Vec<f32> = ground
        .iter()
        .flat_map(|m| m.positions.chunks_exact(3).map(|p| p[0]))
        .collect();
    let min_x = xs.iter().copied().fold(f32::MAX, f32::min);
    let max_x = xs.iter().copied().fold(f32::MIN, f32::max);
    assert!(
        min_x < -10_000.0 && max_x > 30_000.0,
        "ground spans only {min_x}..{max_x} cm"
    );
}

/// Both road edges carry a painted line for the whole lap — the road
/// material cannot draw them itself (it does not know where the right edge
/// is), so they must come out of the bake as marking strips.
#[test]
fn edge_lines_run_the_length_of_both_road_edges() {
    let baked = bake_test_scene();
    let edge_material = baked
        .materials
        .iter()
        .find(|m| m.key.starts_with("marking_edge_line_"))
        .expect("edge line material missing");
    assert_eq!(edge_material.family, "marking");

    let lines: Vec<&UeMesh> = baked
        .meshes
        .iter()
        .filter(|m| m.material_key == edge_material.key)
        .collect();
    assert!(!lines.is_empty(), "no edge line meshes baked");

    // Both sides land in the same (section, material) meshes, so coverage
    // is checked as u-span for the lap and vertex count for the two sides:
    // each side emits one ~4-vertex quad per meter of station.
    let us: Vec<f32> = lines
        .iter()
        .flat_map(|m| m.uvs.chunks_exact(2).map(|uv| uv[0]))
        .collect();
    let min_u = us.iter().copied().fold(f32::MAX, f32::min);
    let max_u = us.iter().copied().fold(f32::MIN, f32::max);
    let lap_m = baked.length_cm / 100.0;
    assert!(
        max_u - min_u > 0.9 * lap_m,
        "edge lines span {min_u}..{max_u} m of a {lap_m} m lap"
    );
    let vertices: usize = lines.iter().map(|m| m.positions.len() / 3).sum();
    assert!(
        vertices as f32 > 1.6 * lap_m * 4.0,
        "{vertices} vertices is too few for lines on both edges"
    );
}

/// The start-light gantry anchor: on the line, on the road, facing the
/// direction of travel.
#[test]
fn start_finish_anchor_sits_on_the_line() {
    let baked = bake_test_scene();
    let sf = baked.start_finish.expect("start/finish anchor");
    // The seeded marking spans station 0..1 m, so the centre is 0.5 m down
    // the track from the first node at the origin, along the course — which
    // on this spline leaves the origin at 45 degrees to the right.
    let dist_cm = sf.location[0].hypot(sf.location[1]);
    assert!((dist_cm - 50.0).abs() < 5.0, "{:?}", sf.location);
    assert!(sf.location[2].abs() < 5.0, "{:?}", sf.location);
    assert!((sf.yaw_deg - 45.0).abs() < 5.0, "{}", sf.yaw_deg);
    assert!((sf.width_m - 12.0).abs() < 0.01, "{}", sf.width_m);
    // And it agrees with where the first centerline point heads.
    assert!((sf.yaw_deg - baked.centerline[0].yaw_deg).abs() < 5.0);
}

/// Grid boxes are laid out in station space: slot 16 is row 8, 56 m before
/// the line, so the outlines run from 58.5 m behind the line to the pole
/// stub 3.5 m ahead of it — on a closed loop, wrapped past the lap length.
#[test]
fn monza_grid_boxes_end_56_m_before_the_line() {
    let track_path = real_tracks_dir().join("Monza.yaml");
    let opened = project::open_project(&track_path).unwrap();
    let baked = ue_export::bake(&opened.track, &opened.scene.unwrap()).unwrap();
    let lap_m = baked.length_cm / 100.0;
    let us: Vec<f32> = baked
        .meshes
        .iter()
        .filter(|m| m.material_key.starts_with("marking_grid_slot_"))
        .flat_map(|m| m.uvs.chunks_exact(2).map(|uv| uv[0]))
        .collect();
    assert!(!us.is_empty(), "no grid box paint");
    // Spans behind the line wrap past the lap length; those ahead of it
    // don't. Fold both onto a signed station around the line.
    let around = |u: f32| if u > lap_m / 2.0 { u - lap_m } else { u };
    let rear = us.iter().map(|&u| around(u)).fold(f32::MAX, f32::min);
    let front = us.iter().map(|&u| around(u)).fold(f32::MIN, f32::max);
    assert!(
        (rear + 58.5).abs() < 0.6,
        "rear of box 16 at {rear} m, lap {lap_m} m"
    );
    assert!(
        (front - 3.5).abs() < 0.6,
        "front of the pole stub at {front} m"
    );
}

/// The verge at Spa: right of the road at station 0 the ground is the
/// road edge less the verge drop for 6 m, then eases into the terrain.
#[test]
fn spa_ground_hugs_the_road_edge_at_the_line() {
    use track_editor::terrain::{TerrainHeightfield, BLEND_END_M, VERGE_DROP_M};
    use track_editor::track_path::{offset_point, CenterlinePath};

    let track_path = real_tracks_dir().join("Spa.yaml");
    let opened = project::open_project(&track_path).unwrap();
    let path = CenterlinePath::from_track(&opened.track).unwrap();
    let field = TerrainHeightfield::from_path(&path).unwrap();
    let s = path.sample_at(0.0);
    let edge_z = offset_point(&s, -s.width_right_m).2;
    let at = |beyond: f32| {
        let p = offset_point(&s, -(s.width_right_m + beyond));
        field.ground_height_at(p.0, p.1)
    };
    let terrain_at = |beyond: f32| {
        let p = offset_point(&s, -(s.width_right_m + beyond));
        field.height_at(p.0, p.1)
    };
    let far = terrain_at(BLEND_END_M + 5.0);
    let profile: Vec<(f32, f32, f32)> = [1.0, 5.0, 10.0, 20.0, 40.0]
        .iter()
        .map(|&b| (b, at(b), terrain_at(b)))
        .collect();
    eprintln!(
        "Spa station 0, right of road: edge {edge_z:.3}; (beyond, ground, terrain) {profile:?}"
    );

    assert!((at(1.0) - (edge_z - VERGE_DROP_M)).abs() < 0.02);
    assert!((at(5.0) - (edge_z - VERGE_DROP_M)).abs() < 0.02);
    // Past the blend the ground *is* the terrain field.
    assert!(
        (at(40.0) - terrain_at(40.0)).abs() < 0.02,
        "40 m out: {} vs terrain {}",
        at(40.0),
        terrain_at(40.0)
    );
    // Stepless between the verge and the terrain.
    let mut prev = at(6.0);
    let span = (prev - far).abs().max(0.05);
    for b in 7..=40 {
        let z = at(b as f32);
        assert!(
            (z - prev).abs() <= span * 0.12 + 0.01,
            "step of {} m at {b} m out",
            z - prev
        );
        prev = z;
    }
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
    let exported = ue_export_io::export_track(&yaml, &out).unwrap();

    assert_eq!(
        fs::read(&yaml).unwrap(),
        yaml_before,
        "the YAML was written to"
    );
    assert_eq!(
        exported.walls_path,
        dir.path().join("Mini.walls.msgpack"),
        "the walls sidecar sits beside the YAML"
    );
    assert!(exported.walls_path.exists());
    assert!(
        !dir.path().join("Mini.ats").exists(),
        "exporting must not create an .ats"
    );
}

fn wall_prop(
    id: u64,
    kind: PropKind,
    asset: &str,
    x: f32,
    y: f32,
    yaw_rad: f32,
    scale: f32,
) -> Prop {
    Prop {
        id,
        kind,
        asset: asset.to_string(),
        x,
        y,
        z: 0.0,
        yaw_rad,
        scale,
        text: None,
        length_m: None,
    }
}

fn wall_ends(w: &WallSegment) -> [(f32, f32); 2] {
    [(w.x0, w.y0), (w.x1, w.y1)]
}

fn close(a: (f32, f32), b: (f32, f32)) -> bool {
    (a.0 - b.0).abs() < 1e-3 && (a.1 - b.1).abs() < 1e-3
}

/// The walls sidecar is the sim's only knowledge of the barriers: a
/// barrier it misses is one a car drives through, and a gap between two
/// modules of a run is a hole in the wall.
#[test]
fn walls_sidecar_follows_the_barriers_and_footprints() {
    use std::f32::consts::FRAC_PI_2;
    let track = test_track();
    let mut scene = test_scene(&track);
    let mut id = scene.next_id;
    let mut next = || {
        id += 1;
        id - 1
    };
    // Two armco modules laid half a metre apart, a tire wall run segment
    // at the groomer's scale, and a 40 m stand.
    scene.props.push(wall_prop(
        next(),
        PropKind::Barrier,
        "armco_generic",
        50.0,
        20.0,
        0.0,
        1.0,
    ));
    scene.props.push(wall_prop(
        next(),
        PropKind::Barrier,
        "armco_generic",
        54.5,
        20.0,
        0.0,
        1.0,
    ));
    scene.props.push(wall_prop(
        next(),
        PropKind::TireWall,
        "tire_wall_generic",
        80.0,
        -30.0,
        FRAC_PI_2,
        2.2,
    ));
    let mut stand = wall_prop(
        next(),
        PropKind::Grandstand,
        "grandstand_main",
        100.0,
        60.0,
        0.0,
        1.0,
    );
    stand.length_m = Some(40.0);
    scene.props.push(stand);
    scene.next_id = id;

    let baked = ue_export::bake_all(&track, &scene).expect("test track must bake");
    let walls = baked.walls;
    assert_eq!(walls.version, WALLS_VERSION);
    for w in &walls.segments {
        assert!(
            [w.x0, w.y0, w.x1, w.y1, w.z, w.height_m]
                .iter()
                .all(|v| v.is_finite()),
            "non-finite wall {w:?}"
        );
        assert!(w.height_m > 0.0, "flat wall {w:?}");
    }

    // The armco: one 4 m face per module, the gap between them closed.
    let mut armco: Vec<&WallSegment> = walls
        .segments
        .iter()
        .filter(|w| w.kind == WALL_KIND_ARMCO)
        .collect();
    armco.sort_by(|a, b| a.x0.total_cmp(&b.x0));
    assert_eq!(armco.len(), 2, "{armco:?}");
    assert!(
        close(wall_ends(armco[0])[0], (48.0, 20.0)),
        "{:?}",
        armco[0]
    );
    assert!(
        close(wall_ends(armco[0])[1], (52.25, 20.0)),
        "{:?}",
        armco[0]
    );
    assert!(
        close(wall_ends(armco[1])[0], (52.25, 20.0)),
        "{:?}",
        armco[1]
    );
    assert!(
        close(wall_ends(armco[1])[1], (56.5, 20.0)),
        "{:?}",
        armco[1]
    );

    // The tire wall: the kit's 4 m at scale 2.2, along its own heading.
    let tires: Vec<&WallSegment> = walls
        .segments
        .iter()
        .filter(|w| w.kind == WALL_KIND_TIRES)
        .collect();
    assert_eq!(tires.len(), 1, "{tires:?}");
    let ends = wall_ends(tires[0]);
    assert!(
        close(ends[0], (80.0, -34.4)) && close(ends[1], (80.0, -25.6)),
        "{:?}",
        tires[0]
    );

    // The stand: its four sides, 40 m long and the family's depth, its
    // front through the pivot and the rest behind it.
    let stand_sides: Vec<&WallSegment> = walls
        .segments
        .iter()
        .filter(|w| w.kind == WALL_KIND_CONCRETE)
        .filter(|w| {
            ((w.x0 + w.x1) / 2.0 - 100.0).abs() < 30.0 && ((w.y0 + w.y1) / 2.0 - 60.0).abs() < 30.0
        })
        .collect();
    assert_eq!(stand_sides.len(), 4, "{stand_sides:?}");
    let xs: Vec<f32> = stand_sides.iter().flat_map(|w| [w.x0, w.x1]).collect();
    let ys: Vec<f32> = stand_sides.iter().flat_map(|w| [w.y0, w.y1]).collect();
    let span = |v: &[f32]| {
        v.iter().cloned().fold(f32::NEG_INFINITY, f32::max)
            - v.iter().cloned().fold(f32::INFINITY, f32::min)
    };
    assert!(
        (span(&xs) - 40.0).abs() < 1e-3,
        "stand length {}",
        span(&xs)
    );
    assert!(
        (5.0..=16.0).contains(&span(&ys)),
        "stand depth {}",
        span(&ys)
    );
    assert!((xs.iter().sum::<f32>() / xs.len() as f32 - 100.0).abs() < 1e-3);
    let front = stand_sides
        .iter()
        .filter(|w| (w.y0 - 60.0).abs() < 1e-3 && (w.y1 - 60.0).abs() < 1e-3)
        .count();
    assert_eq!(front, 1, "one side runs through the pivot: {stand_sides:?}");
    let mean_y = ys.iter().sum::<f32>() / ys.len() as f32;
    assert!(
        ((mean_y - 60.0).abs() - span(&ys) / 2.0).abs() < 1e-3,
        "the stand reaches back from its front, not across it: mean y {mean_y}"
    );

    // A tree stops nothing.
    assert!(
        walls.segments.iter().all(|w| wall_ends(w)
            .iter()
            .all(|e| (e.0 - 30.0).hypot(e.1 - 25.0) > 3.0)),
        "a wall at the tree"
    );
}

/// The pit complex the lane implies is solid too: the pit wall between
/// the lane and the road as faces, the garages as footprints.
#[test]
fn walls_sidecar_carries_the_pit_complex() {
    let (track, scene) = stadium_scene();
    let walls = ue_export::bake_all(&track, &scene).unwrap().walls.segments;
    let concrete: Vec<&WallSegment> = walls
        .iter()
        .filter(|w| w.kind == WALL_KIND_CONCRETE)
        .collect();
    let length = |w: &WallSegment| (w.x1 - w.x0).hypot(w.y1 - w.y0);
    // Ten 6 m pit-wall modules plus the plain ones, laid end to end and
    // joined into a run: every face is at least a module long.
    let pit_wall_faces = concrete
        .iter()
        .filter(|w| length(w) >= 5.9 && length(w) <= 6.5 && (w.y0 - w.y1).abs() < 0.1)
        .filter(|w| (-16.0..=-13.0).contains(&w.y0))
        .count();
    assert!(pit_wall_faces >= 10, "pit wall faces: {pit_wall_faces}");
    // Ten garages and two end blocks, four sides each, beyond the lane.
    let garage_sides = concrete
        .iter()
        .filter(|w| (w.y0 + w.y1) / 2.0 < -17.0)
        .count();
    assert!(garage_sides >= 12 * 4, "garage sides: {garage_sides}");
    // The crew's kit is not a wall.
    assert!(
        !concrete
            .iter()
            .any(|w| (2.0..=3.0).contains(&length(w)) && w.height_m < 2.5),
        "a box kit became a wall"
    );
}

/// Written and read back the way the server does it.
#[test]
fn walls_sidecar_roundtrips_through_msgpack() {
    let track = test_track();
    let scene = test_scene(&track);
    let baked = ue_export::bake_all(&track, &scene).expect("test track must bake");
    let bytes = rmp_serde::to_vec_named(&baked.walls).unwrap();
    let back: ue_export::Walls = rmp_serde::from_slice(&bytes).unwrap();
    assert_eq!(back, baked.walls);
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

/// The curb sidecar is the sim's only knowledge of the curbs: get a span
/// wrong and the server counts a car on the curb as off the track.
#[test]
fn curb_sidecar_covers_each_curb_span() {
    let track = test_track();
    let scene = test_scene(&track);
    let baked = ue_export::bake_all(&track, &scene).expect("test track must bake");
    let curbs = baked.curbs.expect("a track with a length has curb bands");

    assert_eq!(curbs.version, CURB_BANDS_VERSION);
    assert_eq!(curbs.step_m, CURB_BAND_STEP_M);
    assert_eq!(curbs.left_cm.len(), curbs.right_cm.len());
    let at = |band: &[u16], station_m: f32| {
        band[(station_m / CURB_BAND_STEP_M).round() as usize % band.len()]
    };

    // Left curb: stations 30..90, 1.2 m wide.
    assert_eq!(at(&curbs.left_cm, 60.0), 120);
    assert_eq!(at(&curbs.left_cm, 30.0), 120);
    assert_eq!(at(&curbs.left_cm, 90.0), 120);
    assert_eq!(at(&curbs.left_cm, 29.0), 0);
    assert_eq!(at(&curbs.left_cm, 91.0), 0);
    // ...and it is a *left* curb, so the right edge stays bare there.
    assert_eq!(at(&curbs.right_cm, 60.0), 0);

    // Right curb: 780 through start/finish to 25, 1.0 m wide.
    assert_eq!(at(&curbs.right_cm, 790.0), 100);
    assert_eq!(at(&curbs.right_cm, 0.0), 100);
    assert_eq!(at(&curbs.right_cm, 20.0), 100);
    assert_eq!(at(&curbs.right_cm, 40.0), 0);
}

/// Every shipped circuit must bake curb bands that fit the track and stay
/// within the widths the editor allows, since the server trusts them as
/// track limits without re-checking against the `.ats`.
#[test]
fn real_tracks_bake_plausible_curb_bands() {
    for track_path in ue_export_io::track_files_in(&real_tracks_dir()).unwrap() {
        let opened = project::open_project(&track_path).unwrap();
        let Some(scene) = opened.scene else { continue };
        let baked = ue_export::bake_all(&opened.track, &scene).unwrap();
        let curbs = baked.curbs.expect("a real circuit has a length");
        let samples = curbs.left_cm.len();
        assert_eq!(curbs.right_cm.len(), samples);
        let length_m = opened
            .track
            .metadata
            .as_ref()
            .and_then(|m| m.length_m)
            .unwrap_or(0.0);
        if length_m > 0.0 {
            let spanned = samples as f32 * CURB_BAND_STEP_M;
            assert!(
                (spanned - length_m).abs() < 5.0,
                "{}: {spanned} m of curb bands for a {length_m} m lap",
                track_path.display()
            );
        }
        let widest = curbs
            .left_cm
            .iter()
            .chain(&curbs.right_cm)
            .copied()
            .max()
            .unwrap_or(0);
        assert!(
            widest <= 600,
            "{}: a {widest} cm curb is wider than the editor allows",
            track_path.display()
        );
        assert!(
            curbs.left_cm.iter().chain(&curbs.right_cm).any(|w| *w > 0),
            "{}: every circuit has curbs somewhere",
            track_path.display()
        );
    }
}
