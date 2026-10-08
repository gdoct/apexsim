//! The generated road mesh held against the centerline it was generated
//! from, on Monza (`docs/ROAD_MESH.md`, "Agreement with the centerline").
//!
//! The two backends sample two different splines through the same nodes
//! (the exporter's 2 m Catmull-Rom, the server's adaptive one), so this
//! is the first measurement of how far apart the road they each describe
//! is. Needs `Monza.road.msgpack`, which `ats-export` writes and which is
//! generated rather than checked in: without it these tests say so and
//! pass, like the `*_mesh` benches.

use apexsim_server::config::RoadContactMode;
use apexsim_server::data::{TrackConfig, TrackPoint};
use apexsim_server::physics::{probe_surface, seat_height, RoadContact};
use apexsim_server::track_loader::TrackLoader;

const MONZA: &str = "../content/tracks/default/Monza/Monza.yaml";

/// Monza on the centerline and Monza on its mesh, or `None` without the
/// sidecar.
fn load_both() -> Option<(TrackConfig, TrackConfig)> {
    let line = TrackLoader::load_from_file(MONZA).expect("Monza loads");
    let mesh = TrackLoader::load_from_file_with(MONZA, RoadContactMode::Mesh).expect("Monza loads");
    if mesh.road_mesh.is_none() {
        eprintln!("no Monza.road.msgpack: bake it with ats-export to run this test");
        return None;
    }
    assert!(
        line.road_mesh.is_none(),
        "the default load leaves the mesh alone"
    );
    Some((line, mesh))
}

/// The point `lateral_right` metres to the right of centerline point `p`.
fn beside(p: &TrackPoint, lateral_right: f32) -> (f32, f32) {
    let (sin_h, cos_h) = p.heading_rad.sin_cos();
    (p.x + lateral_right * sin_h, p.y - lateral_right * cos_h)
}

#[test]
fn the_road_mesh_agrees_with_the_centerline_on_monza() {
    let Some((line, mesh)) = load_both() else {
        return;
    };
    let curbs = line.curbs.as_ref().expect("Monza has its curb bands");

    let mut probes = 0usize;
    let mut fell_back = 0usize;
    let mut road_holes: Vec<(f32, f32)> = Vec::new();
    let (mut max_dz, mut sum_dz) = (0.0f32, 0.0f64);
    let (mut max_dslope, mut max_dbank) = (0.0f32, 0.0f32);
    let (mut at_dz, mut at_dslope, mut at_dbank) =
        ((0.0f32, 0.0f32), (0.0f32, 0.0f32), (0.0f32, 0.0f32));
    let mut mismatches: Vec<(f32, f32, RoadContact, RoadContact)> = Vec::new();

    for (i, p) in line.centerline.iter().enumerate().step_by(5) {
        let station = p.distance_from_start_m;
        // Across the road, inset from both edges.
        let mut lats: Vec<(f32, &str)> = Vec::new();
        let mut lat = -p.width_left_m + 0.3;
        while lat <= p.width_right_m - 0.3 {
            lats.push((lat, "road"));
            lat += 0.5;
        }
        // Across each curb band, inset from its edges and clear of its
        // ends (the bands are sampled per metre of station, the mesh's
        // curbs start and end where the scene says).
        for sign in [1.0f32, -1.0] {
            let half = if sign > 0.0 {
                p.width_right_m
            } else {
                p.width_left_m
            };
            let curb = curbs.width_at(station, sign);
            let steady = curb > 0.4
                && curbs.width_at(station - 2.0, sign) >= curb - 1e-3
                && curbs.width_at(station + 2.0, sign) >= curb - 1e-3;
            if !steady {
                continue;
            }
            let mut d = 0.15;
            while d <= curb - 0.15 {
                lats.push((sign * (half + d), "curb"));
                d += 0.25;
            }
        }

        for (lat, zone) in lats {
            let (x, y) = beside(p, lat);
            let a = probe_surface(&line, x, y, p.z, Some(i)).expect("centerline sample");
            let b = probe_surface(&mesh, x, y, a.context.elevation, Some(i)).expect("mesh sample");
            probes += 1;
            if !b.from_mesh {
                fell_back += 1;
                if zone == "road" {
                    road_holes.push((station, lat));
                }
                continue;
            }
            let dz = (b.context.elevation - a.context.elevation).abs();
            if zone == "curb" {
                // The mesh curb has its 5 cm profile where the centerline
                // blends the curb down toward the verge: the one change
                // the mesh brings on purpose. Only the class is held to
                // agree, and the height to within that.
                if a.contact != b.contact {
                    mismatches.push((station, lat, a.contact, b.contact));
                }
                assert!(
                    dz < 0.15,
                    "curb at station {station} lat {lat} off by {dz} m"
                );
                continue;
            }
            if dz > max_dz {
                (max_dz, at_dz) = (dz, (station, lat));
            }
            sum_dz += dz as f64;
            let dslope = (b.context.slope_rad - a.context.slope_rad).abs();
            if dslope > max_dslope {
                (max_dslope, at_dslope) = (dslope, (station, lat));
            }
            let dbank = (b.context.banking_rad - a.context.banking_rad).abs();
            if dbank > max_dbank {
                (max_dbank, at_dbank) = (dbank, (station, lat));
            }
            if a.contact != b.contact {
                mismatches.push((station, lat, a.contact, b.contact));
            }
            assert_eq!(a.context.nearest_point, b.context.nearest_point);
            assert_eq!(a.context.heading_rad, b.context.heading_rad);
            assert_eq!(a.context.lateral_offset, b.context.lateral_offset);
        }
    }

    eprintln!(
        "Monza, {probes} probes: {fell_back} fell back to the centerline ({} inside the road), \
         height off by {:.1} mm at most (station {:.0} lat {:.1}) and {:.2} mm on average, \
         slope by {:.4} rad (station {:.0} lat {:.1}), banking by {:.4} rad (station {:.0} lat {:.1}), \
         {} contact classes differ",
        road_holes.len(),
        max_dz * 1000.0,
        at_dz.0,
        at_dz.1,
        sum_dz / (probes - fell_back).max(1) as f64 * 1000.0,
        max_dslope,
        at_dslope.0,
        at_dslope.1,
        max_dbank,
        at_dbank.0,
        at_dbank.1,
        mismatches.len()
    );
    assert!(
        road_holes.is_empty(),
        "the mesh has nothing under {} road probes, first {:?}",
        road_holes.len(),
        &road_holes[..road_holes.len().min(10)]
    );
    assert!(
        mismatches.is_empty(),
        "{} probes classed differently, first {:?}",
        mismatches.len(),
        &mismatches[..mismatches.len().min(10)]
    );
    // On the road, the two splines' disagreement; the design expected
    // centimetres.
    assert!(max_dz < 0.05, "height differs by {max_dz} m");
    assert!(max_dslope < 0.03, "slope differs by {max_dslope} rad");
    assert!(max_dbank < 0.03, "banking differs by {max_dbank} rad");
}

/// The grid is seated on the mesh where the mesh is loaded, and the mesh
/// road is where the centerline formula put the slots before.
#[test]
fn grid_slots_are_seated_on_the_road_mesh() {
    let Some((line, mesh)) = load_both() else {
        return;
    };
    assert_eq!(line.start_positions.len(), mesh.start_positions.len());
    for (a, b) in line.start_positions.iter().zip(&mesh.start_positions) {
        assert_eq!((a.x, a.y, a.yaw_rad), (b.x, b.y, b.yaw_rad));
        let seated = seat_height(&mesh, b.x, b.y, a.z);
        assert!(
            (seated - b.z).abs() < 1e-4,
            "slot {}: seated at {seated}, loaded with {}",
            a.position,
            b.z
        );
        assert!(
            (b.z - a.z).abs() < 0.1,
            "slot {} sits {:.3} m off the centerline formula",
            a.position,
            b.z - a.z
        );
    }
}
