//! The formats' shapes, held by small inline fixtures.
//!
//! The YAML fixtures are in the form `serde_yaml` writes, so a parse and a
//! rewrite must give the same bytes back: that is what keeps a tool's
//! rewrite of a track from changing its content checksum. The sidecar
//! goldens are the bytes `rmp_serde::to_vec_named` writes; a changed byte
//! is a changed format (`cargo test --test round_trip -- --nocapture`
//! prints them).

use apexsim_formats::curbs::CurbBands;
use apexsim_formats::ground::GroundHeightfield;
use apexsim_formats::pit::{PitBoxSpot, PitSidecar};
use apexsim_formats::road_mesh::{RoadMeshFile, RoadSurface};
use apexsim_formats::track::TrackFile;
use apexsim_formats::walls::{WallFile, WallSegment, WALL_KIND_TIRES};

/// Every key the track YAML has, as a rewrite writes them.
const FULL_YAML: &str = "\
name: Test Circuit
display_name: Testervoort
track_id: 6f9619ff-8b86-d011-b42d-00c04fc964ff
nodes:
- x: 0.0
  y: 0.0
  z: 0.5
  width: 12.0
  width_left: null
  width_right: null
  banking: null
  friction: null
  surface_type: null
- x: 200.0
  y: 0.0
  z: 4.0
  width: null
  width_left: 6.5
  width_right: 5.5
  banking: 0.15
  friction: 0.95
  surface_type: Asphalt
checkpoints:
- index_start: 1
  index_end: 1
sectors:
- 1
- 1
spawn_points:
- position: 0
  offset_x: 1.0
  offset_y: -2.0
default_width: 12.0
closed_loop: true
raceline:
- x: 1.0
  y: 2.0
  z: 3.0
drs_zones:
- detection_m: 100.0
  start_m: 150.0
  end_m: 20.0
metadata:
  country: Netherlands
  city: Zandvoort
  length_m: 4259.0
  description: A seaside circuit
  year_built: 1948
  category: F1
  environment_type: plains
  terrain_seed: 7
  terrain_scale: 1.0
  terrain_detail: 0.5
  terrain_blend_width: 20.0
  object_density: 0.8
  decal_profile: default
  altitude_m: 5.0
  latitude_deg: 52.388
  longitude_deg: 4.541
";

/// What a rewrite of the barest file writes: the optional keys that are
/// omitted when empty stay omitted.
const BARE_YAML: &str = "\
name: Bare
nodes:
- x: 0.0
  y: 0.0
- x: 10.0
  y: 0.0
default_width: 10.0
";

const BARE_REWRITTEN: &str = "\
name: Bare
track_id: null
nodes:
- x: 0.0
  y: 0.0
  z: 0.0
  width: null
  width_left: null
  width_right: null
  banking: null
  friction: null
  surface_type: null
- x: 10.0
  y: 0.0
  z: 0.0
  width: null
  width_left: null
  width_right: null
  banking: null
  friction: null
  surface_type: null
checkpoints: []
spawn_points: []
default_width: 10.0
closed_loop: false
raceline: []
drs_zones: []
metadata: null
";

#[test]
fn a_full_track_yaml_rewrites_to_the_same_bytes() {
    let track: TrackFile = serde_yaml::from_str(FULL_YAML).unwrap();
    assert_eq!(track.display_name.as_deref(), Some("Testervoort"));
    assert_eq!(track.sectors, vec![1, 1]);
    assert_eq!(track.nodes[1].resolved_half_widths(12.0), (6.5, 5.5));
    assert_eq!(track.nodes[0].resolved_half_widths(12.0), (6.0, 6.0));
    let meta = track.metadata.as_ref().unwrap();
    assert_eq!(meta.altitude_m, Some(5.0));
    assert_eq!(serde_yaml::to_string(&track).unwrap(), FULL_YAML);
}

#[test]
fn a_bare_track_yaml_rewrites_without_the_omitted_keys() {
    let track: TrackFile = serde_yaml::from_str(BARE_YAML).unwrap();
    assert!(track.sectors.is_empty() && track.display_name.is_none());
    assert_eq!(track.nodes[1].resolved_half_widths(10.0), (5.0, 5.0));
    let rewritten = serde_yaml::to_string(&track).unwrap();
    assert_eq!(rewritten, BARE_REWRITTEN);
    let again: TrackFile = serde_yaml::from_str(&rewritten).unwrap();
    assert_eq!(again, track);
    assert_eq!(serde_yaml::to_string(&again).unwrap(), BARE_REWRITTEN);
}

#[test]
fn metadata_without_a_location_leaves_it_off() {
    let yaml = format!("{BARE_YAML}metadata:\n  country: Italy\n");
    let track: TrackFile = serde_yaml::from_str(&yaml).unwrap();
    let out = serde_yaml::to_string(&track).unwrap();
    assert!(out.contains("country: Italy"));
    assert!(!out.contains("altitude_m") && !out.contains("latitude_deg"));
    assert!(out.contains("city: null"), "the older keys are written");
}

#[test]
fn a_json_track_reads_as_the_yaml_does() {
    let track: TrackFile = serde_yaml::from_str(FULL_YAML).unwrap();
    let json = serde_json::to_string_pretty(&track).unwrap();
    let back: TrackFile = serde_json::from_str(&json).unwrap();
    assert_eq!(back, track);
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// The bytes a sidecar is written as, against its golden.
fn golden<T>(name: &str, value: &T, expected: &str)
where
    T: serde::Serialize + serde::de::DeserializeOwned + PartialEq + std::fmt::Debug,
{
    let bytes = rmp_serde::to_vec_named(value).unwrap();
    println!("{name}: {}", hex(&bytes));
    assert_eq!(hex(&bytes), expected, "{name}: the sidecar's bytes moved");
    let back: T = rmp_serde::from_slice(&bytes).unwrap();
    assert_eq!(&back, value, "{name}: round trip");
}

#[test]
fn the_ground_sidecar_golden() {
    let field = GroundHeightfield {
        version: 1,
        origin_x: -10.0,
        origin_y: -10.0,
        cell_m: 10.0,
        cols: 3,
        rows: 2,
        heights_cm: vec![0, 100, 200, 1000, 1100, -1200],
    };
    assert_eq!(field.height_m(2, 1), -12.0);
    golden("ground", &field, GROUND_GOLDEN);
}

#[test]
fn the_curb_sidecar_golden_and_an_old_version() {
    let bands = CurbBands {
        version: 3,
        step_m: 1.0,
        left_cm: vec![0, 150],
        right_cm: vec![100, 0],
        runoff_left_cm: vec![0, 800],
        runoff_right_cm: vec![0, 0],
        gravel_left_cm: vec![[0, 0], [200, 900]],
        gravel_right_cm: vec![],
        sand_left_cm: vec![],
        sand_right_cm: vec![[100, 500], [0, 0]],
        ground: "desert".to_string(),
    };
    golden("curbs", &bands, CURBS_GOLDEN);

    // A version 1 file has only the curbs; the later bands read as empty.
    #[derive(serde::Serialize)]
    struct V1 {
        version: u32,
        step_m: f32,
        left_cm: Vec<u16>,
        right_cm: Vec<u16>,
    }
    let v1 = rmp_serde::to_vec_named(&V1 {
        version: 1,
        step_m: 2.0,
        left_cm: vec![5],
        right_cm: vec![6],
    })
    .unwrap();
    let read: CurbBands = rmp_serde::from_slice(&v1).unwrap();
    assert_eq!(read.version, 1);
    assert!(read.runoff_left_cm.is_empty() && read.gravel_left_cm.is_empty());
    assert!(read.ground.is_empty());
}

#[test]
fn the_wall_sidecar_golden() {
    let walls = WallFile {
        version: 1,
        segments: vec![WallSegment {
            x0: 0.0,
            y0: 0.0,
            x1: 3.0,
            y1: 4.0,
            z: 1.5,
            height_m: 1.0,
            kind: WALL_KIND_TIRES,
        }],
    };
    assert_eq!(walls.segments[0].length(), 5.0);
    golden("walls", &walls, WALLS_GOLDEN);
}

#[test]
fn the_road_mesh_sidecar_golden() {
    let mesh = RoadMeshFile {
        version: 1,
        vertices: vec![[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.25]],
        triangles: vec![[0, 1, 2]],
        triangle_surface: vec![1],
        surfaces: vec![RoadSurface::road(), RoadSurface::off("gravel")],
        source: "ats-export cbf43926".to_string(),
    };
    golden("road", &mesh, ROAD_GOLDEN);
}

#[test]
fn the_pit_sidecar_golden() {
    let pit = PitSidecar {
        version: 1,
        width_m: 12.0,
        speed_limit_mps: 22.5,
        lane_side: -1,
        nodes: vec![[0.0, -20.0, 0.0], [2.0, -20.0, 0.0]],
        length_m: 2.0,
        limit_start_m: 0.5,
        limit_end_m: 1.5,
        entry_station_m: 4000.0,
        exit_station_m: 10.0,
        boxes: vec![PitBoxSpot {
            lane_station_m: 1.0,
            x: 1.0,
            y: -22.0,
            z: 0.0,
            yaw_rad: 0.0,
        }],
    };
    golden("pit", &pit, PIT_GOLDEN);
}

const GROUND_GOLDEN: &str = concat!(
    "87a776657273696f6e01a86f726967696e5f78cac1200000a86f726967696e5f79cac1200000a663",
    "656c6c5f6dca41200000a4636f6c7303a4726f777302aa686569676874735f636d960064ccc8cd03",
    "e8cd044cd1fb50",
);
const CURBS_GOLDEN: &str = concat!(
    "8ba776657273696f6e03a6737465705f6dca3f800000a76c6566745f636d9200cc96a87269676874",
    "5f636d926400ae72756e6f66665f6c6566745f636d9200cd0320af72756e6f66665f72696768745f",
    "636d920000ae67726176656c5f6c6566745f636d9292000092ccc8cd0384af67726176656c5f7269",
    "6768745f636d90ac73616e645f6c6566745f636d90ad73616e645f72696768745f636d929264cd01",
    "f4920000a667726f756e64a6646573657274",
);
const WALLS_GOLDEN: &str = concat!(
    "82a776657273696f6e01a87365676d656e74739187a27830ca00000000a27930ca00000000a27831",
    "ca40400000a27931ca40800000a17aca3fc00000a86865696768745f6dca3f800000a46b696e6401",
);
const ROAD_GOLDEN: &str = concat!(
    "86a776657273696f6e01a876657274696365739393ca00000000ca00000000ca0000000093ca3f80",
    "0000ca00000000ca0000000093ca00000000ca3f800000ca3e800000a9747269616e676c65739193",
    "000102b0747269616e676c655f737572666163659101a873757266616365739285a36b6579a4726f",
    "6164a7636f6e7461637400a86672696374696f6eca3f800000ab76616c69645f747261636bc3a870",
    "69745f6c616e65c285a36b6579aa6f66665f67726176656ca7636f6e7461637403a8667269637469",
    "6f6eca3f800000ab76616c69645f747261636bc2a87069745f6c616e65c2a6736f75726365b36174",
    "732d6578706f7274206362663433393236",
);
const PIT_GOLDEN: &str = concat!(
    "8ba776657273696f6e01a777696474685f6dca41400000af73706565645f6c696d69745f6d7073ca",
    "41b40000a96c616e655f73696465ffa56e6f6465739293ca00000000cac1a00000ca0000000093ca",
    "40000000cac1a00000ca00000000a86c656e6774685f6dca40000000ad6c696d69745f7374617274",
    "5f6dca3f000000ab6c696d69745f656e645f6dca3fc00000af656e7472795f73746174696f6e5f6d",
    "ca457a0000ae657869745f73746174696f6e5f6dca41200000a5626f7865739185ae6c616e655f73",
    "746174696f6e5f6dca3f800000a178ca3f800000a179cac1b00000a17aca00000000a77961775f72",
    "6164ca00000000",
);
