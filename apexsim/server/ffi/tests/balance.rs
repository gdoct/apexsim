//! A balanced car through the C API (`apx_car_create_balanced`): what the
//! server's `[[balance]]` makes of the car.toml, so a predicting client
//! steps the car the server steps. Less power is a slower car up the
//! straight, the neutral figures the car as loaded, bit for bit, and figures
//! out of the server's bounds are refused.

use std::ffi::CString;
use std::path::PathBuf;

use apexsim_physics::*;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml";
const CAR: &str = "../apexsim-content/cars/default/fugazzi-sf26/car.toml";
const HZ: u16 = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;

fn load_track() -> *mut ApxTrack {
    let path = CString::new(repo(TRACK).to_string_lossy().into_owned()).unwrap();
    let mut track: *mut ApxTrack = std::ptr::null_mut();
    assert_eq!(
        unsafe { apx_track_load(path.as_ptr(), 0, std::ptr::null(), 0, &mut track) },
        APX_OK
    );
    track
}

fn create(track: *mut ApxTrack, balance: Option<ApxBalance>) -> (ApxStatus, *mut ApxCar) {
    let toml = std::fs::read(repo(CAR)).unwrap();
    let mut car: *mut ApxCar = std::ptr::null_mut();
    let status = unsafe {
        match balance {
            None => apx_car_create(
                track,
                toml.as_ptr(),
                toml.len(),
                std::ptr::null(),
                0,
                1,
                &mut car,
            ),
            Some(b) => apx_car_create_balanced(
                track,
                toml.as_ptr(),
                toml.len(),
                std::ptr::null(),
                0,
                1,
                &b,
                &mut car,
            ),
        }
    };
    (status, car)
}

/// Full throttle for four seconds from the grid: the speed reached.
fn launch(track: *mut ApxTrack, car: *mut ApxCar) -> f32 {
    let input = ApxInput {
        throttle: 1.0,
        brake: 0.0,
        steering: 0.0,
        clutch: 0.0,
        gear: 0,
        has_gear: 0,
        has_clutch: 0,
        drs: 0,
        headlights: -1,
        flash: 0,
        ers_mode: -1,
        ers_boost: 0,
        handbrake: 0,
    };
    for tick in 0..(HZ as u32 * 4) {
        assert_eq!(
            unsafe { apx_car_step(car, track, &input, tick, HZ) },
            APX_OK
        );
    }
    let mut pose = unsafe { std::mem::zeroed::<ApxCarPose>() };
    assert_eq!(unsafe { apx_car_pose(car, &mut pose) }, APX_OK);
    pose.speed_mps
}

#[test]
fn a_balanced_car_steps_as_the_server_balances_it() {
    if !repo(TRACK).is_file() {
        eprintln!("skipped: no content checkout beside the code");
        return;
    }
    let track = load_track();
    let (_, plain) = create(track, None);
    let neutral = ApxBalance {
        ballast_kg: 0.0,
        power: 1.0,
        downforce: 1.0,
    };
    let (_, same) = create(track, Some(neutral));
    let (status, slower) = create(
        track,
        Some(ApxBalance {
            ballast_kg: 40.0,
            power: 0.8,
            downforce: 1.0,
        }),
    );
    assert_eq!(status, APX_OK);

    let (v_plain, v_same, v_slower) = (
        launch(track, plain),
        launch(track, same),
        launch(track, slower),
    );
    assert_eq!(
        v_plain.to_bits(),
        v_same.to_bits(),
        "neutral figures: the car as loaded"
    );
    assert!(
        v_slower < v_plain - 0.5,
        "less power and more mass: {v_slower} m/s against {v_plain}"
    );

    let (refused, none) = create(
        track,
        Some(ApxBalance {
            ballast_kg: 0.0,
            power: 3.0,
            downforce: 1.0,
        }),
    );
    assert_eq!(refused, APX_ERR_INVALID);
    assert!(none.is_null());
    unsafe {
        apx_car_free(plain);
        apx_car_free(same);
        apx_car_free(slower);
        apx_track_free(track);
    }
}
