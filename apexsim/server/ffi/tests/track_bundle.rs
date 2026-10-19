//! A track from its physics bundle (`apx_track_load_bundle`, phase 4 of
//! docs/proposals/client-prediction.md) drives a car exactly as the same
//! files loaded from disk; the bundle's checksum is the files' own
//! (`apx_track_files_crc`), and a bundle that is not the one asked for is
//! refused.
//!
//! Monza from the checkout, with its sidecars where they have been baked
//! and from the YAML alone in CI.

use std::ffi::{CStr, CString};
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;

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

/// The bundle the server would build of a YAML and its sidecars.
fn build(yaml: &std::path::Path, road_contact: u8) -> (Vec<u8>, u32) {
    let path = CString::new(yaml.to_str().unwrap()).unwrap();
    let (mut size, mut crc) = (0usize, 0u32);
    unsafe {
        assert_eq!(
            apx_track_bundle_build(
                path.as_ptr(),
                road_contact,
                std::ptr::null_mut(),
                0,
                &mut size,
                &mut crc
            ),
            APX_ERR_BUFFER
        );
        let mut bytes = vec![0u8; size];
        ok(apx_track_bundle_build(
            path.as_ptr(),
            road_contact,
            bytes.as_mut_ptr(),
            bytes.len(),
            &mut size,
            &mut crc,
        ));
        (bytes, crc)
    }
}

fn input(tick: u32) -> ApxInput {
    let t = tick as f32;
    ApxInput {
        throttle: (t / 150.0).min(1.0),
        steering: 0.05 * (t * 0.01).sin(),
        headlights: -1,
        ers_mode: -1,
        ..Default::default()
    }
}

#[test]
fn a_track_from_its_bundle_drives_as_from_its_files() {
    let yaml = repo(TRACK);
    let path = CString::new(yaml.to_str().unwrap()).unwrap();
    let toml = std::fs::read(repo(CAR)).unwrap();
    for road_contact in [0u8, 1] {
        let (bundle, crc) = build(&yaml, road_contact);
        let mut files_crc = 0u32;
        unsafe {
            ok(apx_track_files_crc(
                path.as_ptr(),
                road_contact,
                &mut files_crc,
            ))
        };
        assert_eq!(files_crc, crc, "the files' checksum is the bundle's");

        let none = std::ptr::null();
        let (mut from_disk, mut from_bundle) = (std::ptr::null_mut(), std::ptr::null_mut());
        unsafe {
            ok(apx_track_load(
                path.as_ptr(),
                road_contact,
                none,
                0,
                &mut from_disk,
            ));
            ok(apx_track_load_bundle(
                bundle.as_ptr(),
                bundle.len(),
                crc,
                road_contact,
                none,
                0,
                &mut from_bundle,
            ));
        }
        let cars: Vec<*mut ApxCar> = [from_disk, from_bundle]
            .iter()
            .map(|&track| {
                let mut car = std::ptr::null_mut();
                unsafe {
                    ok(apx_car_create(
                        track,
                        toml.as_ptr(),
                        toml.len(),
                        none.cast(),
                        0,
                        1,
                        &mut car,
                    ))
                };
                car
            })
            .collect();
        for tick in 1..=900u32 {
            let i = input(tick);
            unsafe {
                ok(apx_car_step(
                    cars[0],
                    from_disk,
                    &i,
                    tick,
                    DEFAULT_TICK_RATE_HZ,
                ));
                ok(apx_car_step(
                    cars[1],
                    from_bundle,
                    &i,
                    tick,
                    DEFAULT_TICK_RATE_HZ,
                ));
            }
            let (mut a, mut b) = (ApxCarPose::default(), ApxCarPose::default());
            unsafe {
                ok(apx_car_pose(cars[0], &mut a));
                ok(apx_car_pose(cars[1], &mut b));
            }
            assert_eq!(a, b, "tick {tick} on road_contact {road_contact}");
        }
        let mut last = ApxCarPose::default();
        unsafe { ok(apx_car_pose(cars[1], &mut last)) };
        assert!(
            last.speed_mps > 20.0,
            "the car drove ({} m/s)",
            last.speed_mps
        );
        eprintln!(
            "road_contact {road_contact}: bundle {:08X}, {} bytes, the car the same for 900 ticks",
            crc,
            bundle.len()
        );
        unsafe {
            for car in cars {
                apx_car_free(car);
            }
            apx_track_free(from_disk);
            apx_track_free(from_bundle);
        }
    }
}

#[test]
fn a_bundle_that_is_not_the_one_asked_for_is_refused() {
    let (bundle, crc) = build(&repo(TRACK), 0);
    let mut track = std::ptr::null_mut();
    let none = std::ptr::null();
    unsafe {
        assert_eq!(
            apx_track_load_bundle(
                bundle.as_ptr(),
                bundle.len(),
                crc ^ 1,
                0,
                none,
                0,
                &mut track
            ),
            APX_ERR_TRACK
        );
        assert!(track.is_null());
        let why = CStr::from_ptr(apx_last_error())
            .to_string_lossy()
            .into_owned();
        assert!(why.contains("checksum"), "{why}");

        let mut damaged = bundle.clone();
        let middle = damaged.len() / 2;
        damaged[middle] ^= 0x55;
        assert_ne!(
            apx_track_load_bundle(damaged.as_ptr(), damaged.len(), crc, 0, none, 0, &mut track),
            APX_OK
        );
        assert!(track.is_null());
        let junk = b"not a bundle";
        assert_eq!(
            apx_track_load_bundle(junk.as_ptr(), junk.len(), crc, 0, none, 0, &mut track),
            APX_ERR_TRACK
        );
        let missing = CString::new("no/such/track.yaml").unwrap();
        let mut out = 0u32;
        assert_eq!(
            apx_track_files_crc(missing.as_ptr(), 0, &mut out),
            APX_ERR_TRACK
        );
    }
}

/// The bundle each circuit would be, from the folder `BUNDLE_TRACKS_DIR`
/// names (`apexsim-content/tracks/default` by default):
/// `cargo test --release --test track_bundle -- --ignored --nocapture`.
#[test]
#[ignore = "measures the bundles of the circuits whose sidecars are baked"]
fn print_bundle_sizes() {
    let dir = std::env::var("BUNDLE_TRACKS_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo("../apexsim-content/tracks/default"));
    let names: Vec<String> = std::env::var("BUNDLE_TRACKS")
        .unwrap_or_else(|_| "Monzarella,SpaFrankenchamps,Mordschleife,Lemons,Sudoku,Snorisring".into())
        .split(',')
        .map(|s| s.trim().to_string())
        .collect();
    for name in names {
        let yaml = dir.join(&name).join(format!("{name}.yaml"));
        if !yaml.exists() {
            eprintln!("{name}: no {}", yaml.display());
            continue;
        }
        let files = apexsim_sim::track_bundle::TrackFiles::read(
            &yaml,
            apexsim_sim::track_loader::RoadContactMode::Mesh,
        )
        .unwrap();
        let started = std::time::Instant::now();
        let bundle = apexsim_sim::track_bundle::TrackBundle::of(&files);
        let built_ms = started.elapsed().as_millis();
        let started = std::time::Instant::now();
        apexsim_sim::track_bundle::TrackFiles::decode(&bundle.bytes, Some(bundle.crc)).unwrap();
        let read_ms = started.elapsed().as_millis();
        let mb = |n: usize| n as f64 / 1e6;
        eprintln!(
            "{name:>14}: files {:.2} MB (road mesh {:.2} MB), bundle {:.2} MB in {} chunks; \
             built in {built_ms} ms, read in {read_ms} ms",
            mb(bundle.raw_len),
            mb(files.road.as_ref().map_or(0, Vec::len)),
            mb(bundle.bytes.len()),
            bundle.chunk_count(),
        );
    }
}
