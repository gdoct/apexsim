//! Tests for grade separations: a circuit that passes over itself.
//!
//! A heightfield has one height per XY, and the bake used to resolve the
//! crossing the only way that allows — the ground dropped into a valley
//! around the lower road and the upper road floated over it, with its curbs
//! hanging down in curtains and the server's ground putting the bridge's
//! surface beside the road underneath. These pin the underpass instead: an
//! embankment, a clear slot, a deck.

use std::path::Path;

use track_editor::ats::{AtsScene, Curb, Side, Surface, SurfaceKind};
use track_editor::ats_io;
use track_editor::terrain::{
    TerrainHeightfield, Underpass, DECK_DEPTH_M, UNDERPASS_WALL_GAP_M, VERGE_DROP_M,
};
use track_editor::track_data::{TrackFile, TrackNode};
use track_editor::track_io;
use track_editor::track_path::CenterlinePath;
use track_editor::ue_export::{self, UeMesh};

const HALF_WIDTH_M: f32 = 6.0;
const HIGH_M: f32 = 12.0;

fn node(x: f32, y: f32, z: f32) -> TrackNode {
    TrackNode {
        x,
        y,
        z,
        width: Some(2.0 * HALF_WIDTH_M),
        width_left: None,
        width_right: None,
        banking: None,
        friction: None,
        surface_type: None,
    }
}

/// A figure of eight: a flat straight east along y = 0, and the loop's
/// return leg crossing it southbound along x = 0, 12 m up.
fn figure_eight() -> TrackFile {
    TrackFile {
        name: "Figure Eight".to_string(),
        track_id: Some("figure-eight".to_string()),
        nodes: vec![
            node(-100.0, 0.0, 0.0),
            node(100.0, 0.0, 0.0),
            node(300.0, 0.0, 0.0),
            node(380.0, 150.0, 3.0),
            node(200.0, 260.0, 9.0),
            node(0.0, 150.0, HIGH_M),
            node(0.0, -150.0, HIGH_M),
            node(-200.0, -260.0, 9.0),
            node(-380.0, -150.0, 3.0),
            node(-300.0, 0.0, 0.0),
        ],
        checkpoints: vec![],
        spawn_points: vec![],
        default_width: 2.0 * HALF_WIDTH_M,
        closed_loop: true,
        raceline: vec![],
        metadata: None,
    }
}

fn field(track: &TrackFile) -> TerrainHeightfield {
    let path = CenterlinePath::from_track(track).unwrap();
    TerrainHeightfield::from_path(&path).unwrap()
}

fn the_underpass(field: &TerrainHeightfield) -> Underpass {
    let found = field.underpasses();
    assert_eq!(found.len(), 1, "expected one crossing, found {found:?}");
    found[0].clone()
}

/// Curbs and wide bands on both levels through the crossing: everything
/// ground-anchored that could hang into the slot or climb its walls.
fn scene_with_dressing(track: &TrackFile, u: &Underpass) -> AtsScene {
    let mut scene = AtsScene::new_for_track(track, "FigureEight.yaml");
    let mut id = scene.next_id;
    for station in [u.lower_station_m, u.upper_station_m] {
        for side in [Side::Left, Side::Right] {
            scene.curbs.push(Curb {
                id,
                side,
                start_m: station - 60.0,
                end_m: station + 60.0,
                width_m: 1.2,
                style: "red_white".to_string(),
            });
            id += 1;
            scene.surfaces.push(Surface {
                id,
                kind: SurfaceKind::Gravel,
                side,
                start_m: station - 80.0,
                end_m: station + 80.0,
                inner_m: 1.5,
                width_m: 40.0,
                end_width_m: None,
            });
            id += 1;
        }
    }
    scene.next_id = id;
    scene
}

/// Track-space positions of every triangle's corners, centroid and edge
/// midpoints.
fn sample_points(mesh: &UeMesh) -> Vec<(f32, f32, f32)> {
    let at = |i: u32| {
        let i = i as usize * 3;
        (
            mesh.positions[i] / 100.0,
            -mesh.positions[i + 1] / 100.0,
            mesh.positions[i + 2] / 100.0,
        )
    };
    let mix = |a: (f32, f32, f32), b: (f32, f32, f32)| {
        ((a.0 + b.0) / 2.0, (a.1 + b.1) / 2.0, (a.2 + b.2) / 2.0)
    };
    let mut out = Vec::new();
    for tri in mesh.indices.chunks_exact(3) {
        let (a, b, c) = (at(tri[0]), at(tri[1]), at(tri[2]));
        let centroid = (
            (a.0 + b.0 + c.0) / 3.0,
            (a.1 + b.1 + c.1) / 3.0,
            (a.2 + b.2 + c.2) / 3.0,
        );
        out.extend([a, b, c, centroid, mix(a, b), mix(b, c), mix(c, a)]);
    }
    out
}

#[test]
fn a_crossing_with_clearance_is_an_underpass() {
    let track = figure_eight();
    let f = field(&track);
    let u = the_underpass(&f);

    assert!(
        u.at.0.abs() < 1.0 && u.at.1.abs() < 1.0,
        "crossing at {:?}",
        u.at
    );
    assert!(
        (u.clearance_m - HIGH_M).abs() < 1.0,
        "clearance {}",
        u.clearance_m
    );

    let path = CenterlinePath::from_track(&track).unwrap();
    assert!(
        path.sample_at(u.lower_station_m).pos.2 < path.sample_at(u.upper_station_m).pos.2,
        "lower and upper are the wrong way round"
    );
    // Square crossing: the deck spans the slot (two half-widths plus the
    // wall gaps) and a little bearing, not the whole approach.
    let deck = u.deck_span_m.1 - u.deck_span_m.0;
    let slot = 2.0 * (HALF_WIDTH_M + UNDERPASS_WALL_GAP_M);
    assert!(
        deck > slot && deck < slot + 20.0,
        "deck {deck} m over a {slot} m slot"
    );
    let walls = u.wall_span_m.1 - u.wall_span_m.0;
    assert!(walls > deck, "walls {walls} m");

    // Roads that merely meet at a level are not underpasses.
    let mut flat = figure_eight();
    for n in &mut flat.nodes {
        n.z = 0.0;
    }
    assert!(field(&flat).underpasses().is_empty());
}

#[test]
fn the_upper_road_keeps_its_embankment_and_the_lower_road_its_slot() {
    let f = field(&figure_eight());
    // The spline through the nodes overshoots 12 m a little.
    let high = the_underpass(&f).clearance_m;
    let wall = HALF_WIDTH_M + UNDERPASS_WALL_GAP_M;

    // Beside the upper road, clear of the slot: the verge, not a valley.
    for y in [wall + 3.0, wall + 12.0, -(wall + 3.0), -(wall + 12.0)] {
        let g = f.ground_height_at(HALF_WIDTH_M + 2.0, y);
        assert!(
            (g - (high - VERGE_DROP_M)).abs() < 0.6,
            "ground {g} beside the bridge at y = {y}"
        );
    }
    // In the slot, under and beside the bridge: the lower road's verge.
    for (x, y) in [
        (0.0, HALF_WIDTH_M + 1.0),
        (HALF_WIDTH_M + 2.0, -(wall - 0.5)),
        (30.0, 8.0),
    ] {
        let g = f.ground_height_at(x, y);
        assert!(g.abs() < 0.3, "ground {g} in the slot at ({x}, {y})");
    }
    // The surface under the bridge is the road below it, not the deck.
    assert!(f.surface_height_at(0.0, 2.0).abs() < 0.3);
    assert!(f.surface_height_at(0.0, HALF_WIDTH_M + 1.0).abs() < 0.3);
    assert!((f.surface_height_near(0.0, 2.0, high) - high).abs() < 0.3);
    assert!(
        f.deck_top_at(0.0, 2.0)
            .is_some_and(|z| (z - high).abs() < 0.3),
        "the deck covers the crossing"
    );
    assert!(
        f.deck_top_at(0.0, 60.0).is_none(),
        "the deck stops at the slot"
    );
}

#[test]
fn nothing_but_air_is_in_the_slot_under_the_bridge() {
    let track = figure_eight();
    let f = field(&track);
    let u = the_underpass(&f);
    let baked = ue_export::bake(&track, &scene_with_dressing(&track, &u)).unwrap();

    let slot = HALF_WIDTH_M + UNDERPASS_WALL_GAP_M - 0.2;
    let ceiling = u.clearance_m - DECK_DEPTH_M - 0.1;
    let mut offenders: Vec<String> = Vec::new();
    for mesh in &baked.meshes {
        for p in sample_points(mesh) {
            // The lower road is the straight y = 0 through here.
            if p.0.abs() < 60.0 && p.1.abs() < slot && p.2 > 0.3 && p.2 < ceiling {
                offenders.push(format!(
                    "{} at ({:.1}, {:.1}, {:.1})",
                    mesh.material_key, p.0, p.1, p.2
                ));
            }
        }
    }
    offenders.dedup();
    assert!(
        offenders.is_empty(),
        "{} points in the slot, e.g. {:?}",
        offenders.len(),
        &offenders[..offenders.len().min(8)]
    );

    // And there is a bridge: a deck with its fascia, and walls.
    let has = |key: &str| baked.meshes.iter().any(|m| m.material_key == key);
    assert!(has("structure_concrete") && has("structure_fascia"));
}

#[test]
fn the_deck_underside_faces_down_at_the_road_below() {
    let track = figure_eight();
    let f = field(&track);
    let u = the_underpass(&f);
    let baked = ue_export::bake(&track, &scene_with_dressing(&track, &u)).unwrap();

    let mut underside = 0;
    for mesh in baked
        .meshes
        .iter()
        .filter(|m| m.material_key == "structure_concrete")
    {
        for tri in mesh.indices.chunks_exact(3) {
            let n = tri[0] as usize * 3;
            // A deck quad is the deck's full width, so test its centroid.
            let centroid = |axis: usize| {
                tri.iter()
                    .map(|&i| mesh.positions[i as usize * 3 + axis])
                    .sum::<f32>()
                    / 300.0
            };
            let (x, y, z) = (centroid(0), -centroid(1), centroid(2));
            if x.abs() < 4.0 && y.abs() < 4.0 && (z - (u.clearance_m - DECK_DEPTH_M)).abs() < 0.3 {
                assert!(
                    mesh.normals[n + 2] < -0.9,
                    "deck underside normal {:?}",
                    &mesh.normals[n..n + 3]
                );
                underside += 1;
            }
        }
    }
    assert!(underside > 0, "no deck underside over the road");
}

#[test]
fn the_server_ground_under_the_bridge_is_the_lower_level() {
    let track = figure_eight();
    let scene = AtsScene::new_for_track(&track, "FigureEight.yaml");
    let ground = ue_export::bake_all(&track, &scene).unwrap().ground.unwrap();
    let sample = |x: f32, y: f32| {
        let col = ((x - ground.origin_x) / ground.cell_m).round() as u32;
        let row = ((y - ground.origin_y) / ground.cell_m).round() as u32;
        ground.height_m(col, row)
    };
    // Off the lower road's edge, under the upper road: not 12 m up.
    for y in [HALF_WIDTH_M + 1.0, -(HALF_WIDTH_M + 3.0)] {
        assert!(
            sample(0.0, y).abs() < 0.5,
            "sidecar {} at (0, {y})",
            sample(0.0, y)
        );
    }
    // On the embankment beside the upper road, clear of the slot.
    assert!((sample(HALF_WIDTH_M + 2.0, 30.0) - HIGH_M).abs() < 1.0);
}

/// The real thing: Suzuka's crossover, the reason this exists.
#[test]
fn suzuka_has_its_crossover() {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/tracks/real");
    let track = track_io::load_track_file(dir.join("Suzuka.yaml")).unwrap();
    let scene = ats_io::load_ats(dir.join("Suzuka.ats")).unwrap();
    let path = CenterlinePath::from_track(&track).unwrap();
    let lane = scene
        .pit_lane
        .as_ref()
        .and_then(|pit| CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0))
        .unwrap();
    let f = TerrainHeightfield::from_paths(&path, &[&lane]).unwrap();
    let found = f.underpasses();
    assert_eq!(found.len(), 1, "{found:?}");
    let u = &found[0];
    assert_eq!((u.lower_road, u.upper_road), (0, 0));
    // Degner-to-hairpin passes under the back straight before 130R.
    assert!(u.lower_station_m < u.upper_station_m);
    assert!(
        u.clearance_m > 10.0 && u.clearance_m < 15.0,
        "{}",
        u.clearance_m
    );
}

/// Armco lines straights, but not at an underpass: the lower road has its
/// walls there, and the upper road's armco would stand in the slot below.
#[test]
fn groomed_armco_stays_out_of_the_underpass() {
    use track_editor::ats::PropKind;
    use track_editor::groom;

    let track = figure_eight();
    let mut scene = AtsScene::new_for_track(&track, "FigureEight.yaml");
    groom::groom_scene(&track, &mut scene).unwrap();
    let barriers: Vec<_> = scene
        .props
        .iter()
        .filter(|p| p.kind == PropKind::Barrier)
        .collect();

    let wall = HALF_WIDTH_M + UNDERPASS_WALL_GAP_M;
    let in_the_way: Vec<_> = barriers
        .iter()
        .filter(|p| {
            // The lower road's own armco; the upper road's may stand on the
            // embankment beside it, 12 m up.
            let beside_lower = p.x.abs() < 40.0 && p.y.abs() < 20.0 && p.z < HIGH_M / 2.0;
            let over_slot = p.y.abs() < wall && p.x.abs() > HALF_WIDTH_M && p.x.abs() < 20.0;
            beside_lower || over_slot
        })
        .map(|p| (p.x, p.y, p.z))
        .collect();
    assert!(
        in_the_way.is_empty(),
        "armco at the underpass: {in_the_way:?}"
    );

    // The same straight at a level crossing — no underpass — does get armco
    // there, so the check above is not passing on an empty straight.
    let mut flat = figure_eight();
    for n in &mut flat.nodes {
        n.z = 0.0;
    }
    let mut flat_scene = AtsScene::new_for_track(&flat, "FigureEight.yaml");
    groom::groom_scene(&flat, &mut flat_scene).unwrap();
    assert!(
        flat_scene.props.iter().any(|p| p.kind == PropKind::Barrier
            && p.x.abs() < 40.0
            && p.y.abs() > HALF_WIDTH_M
            && p.y.abs() < 20.0),
        "no armco on the level straight either"
    );
}
