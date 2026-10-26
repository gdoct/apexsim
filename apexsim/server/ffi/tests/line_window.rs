//! The racing line aid's window through the C API (`apx_line_*`): a car on
//! the grid gets a window on the same points as the line the server sends.

use std::ffi::{CStr, CString};
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::SessionConditions;
use apexsim_sim::racing_line;
use apexsim_sim::track_loader::TrackLoader;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml";
const CAR: &str = "../apexsim-content/cars/default/fugazzi-sf26/car.toml";

fn ok(status: ApxStatus) {
    let why = unsafe { CStr::from_ptr(apx_last_error()) }.to_string_lossy();
    assert_eq!(status, APX_OK, "{why}");
}

#[test]
fn a_car_on_the_grid_gets_a_window_on_the_server_s_points() {
    let path = CString::new(repo(TRACK).to_str().unwrap()).unwrap();
    let cond = rmp_serde::to_vec_named(&SessionConditions::default()).unwrap();
    let toml = std::fs::read(repo(CAR)).unwrap();
    let (mut track, mut car, mut line) = (
        std::ptr::null_mut(),
        std::ptr::null_mut(),
        std::ptr::null_mut(),
    );
    unsafe {
        ok(apx_track_load(
            path.as_ptr(),
            0,
            cond.as_ptr(),
            cond.len(),
            &mut track,
        ));
        ok(apx_car_create(
            track,
            toml.as_ptr(),
            toml.len(),
            std::ptr::null(),
            0,
            1,
            &mut car,
        ));
        ok(apx_line_create(track, car, &mut line));

        // The server's line: the same track, the same points.
        let server_track = TrackLoader::load_from_file(repo(TRACK)).unwrap();
        let server_car = CarLoader::load_from_file(&repo(CAR)).unwrap();
        let server_line = racing_line::build(&server_track, &server_car).unwrap();
        let mut points = 0;
        ok(apx_line_point_count(line, &mut points));
        assert_eq!(points as usize, server_line.points.len());

        let mut phases = [9u8; 1024];
        let (mut start, mut count) = (0, 0);
        ok(apx_line_window(
            line,
            track,
            car,
            &mut start,
            phases.as_mut_ptr(),
            phases.len() as u32,
            &mut count,
        ));
        let expected = (racing_line::WINDOW_AHEAD_M / server_line.spacing_m).round() as u32;
        assert_eq!(count, expected, "a car on the grid is on the line");
        assert!(start < points);
        let window = &phases[..count as usize];
        assert!(window.iter().all(|&p| p <= 2));
        // From a standstill the first chicane is a braking zone ahead.
        assert!(window.contains(&2), "brakes for the chicane");

        // A short buffer gets as many as fit.
        let mut short = [9u8; 10];
        ok(apx_line_window(
            line,
            track,
            car,
            &mut start,
            short.as_mut_ptr(),
            10,
            &mut count,
        ));
        assert_eq!(count, 10);

        apx_line_free(line);
        apx_car_free(car);
        apx_track_free(track);
    }
}
