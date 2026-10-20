//! A seeded race started and ticked through the C API is the server's own
//! render of that seed, tick for tick: the race `apexsim-replay render`
//! starts and ticks (`seeded_race::start_race` and `tick_field`, which the
//! server's `replay_tools` re-exports and its render loop calls) gives the
//! same digest of every car's full state each second. The same seed twice
//! is the same race, and another seed another.

use std::ffi::CString;
use std::path::PathBuf;

use apexsim_physics::*;
use apexsim_sim::seeded_race::{start_race, tick_field, SimulateOptions};

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Zandervoort/Zandervoort.yaml";
const CARS: &str = "../apexsim-content/cars";
const HOST: &str = "posh-gt3rs";
const AI: u8 = 6;
const LAPS: u8 = 2;
const COUNTDOWN_S: u16 = 3;
const TICK_RATE: u16 = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;

fn c(path: PathBuf) -> CString {
    CString::new(path.to_string_lossy().into_owned()).unwrap()
}

fn start_through_the_api(seed: u64) -> *mut ApxRace {
    let (track, cars, host) = (c(repo(TRACK)), c(repo(CARS)), CString::new(HOST).unwrap());
    let mut race: *mut ApxRace = std::ptr::null_mut();
    let status = unsafe {
        apx_race_start(
            track.as_ptr(),
            cars.as_ptr(),
            host.as_ptr(),
            AI,
            LAPS,
            COUNTDOWN_S,
            TICK_RATE,
            seed,
            std::ptr::null(),
            0,
            &mut race,
        )
    };
    assert_eq!(status, APX_OK, "start: {:?}", unsafe {
        std::ffi::CStr::from_ptr(apx_last_error())
    });
    race
}

fn digest(race: *const ApxRace) -> u64 {
    let mut out = 0u64;
    assert_eq!(unsafe { apx_race_digest(race, &mut out) }, APX_OK);
    out
}

#[test]
fn a_seeded_race_through_the_api_is_the_servers_render_of_that_seed() {
    const SEED: u64 = 7;
    let api = start_through_the_api(SEED);
    let started = start_race(&SimulateOptions {
        track_path: repo(TRACK),
        cars_dir: repo(CARS),
        host_car: HOST.to_string(),
        ai_count: AI,
        laps: LAPS,
        countdown_seconds: COUNTDOWN_S,
        tick_rate: TICK_RATE,
        seed: Some(SEED),
        ..Default::default()
    })
    .expect("the render's race starts");
    let mut server = started.race;
    assert_eq!(
        digest(api),
        race_digest(&server),
        "the same grid, the same start"
    );

    // Through the countdown and 40 s of racing, compared every second.
    for second in 0..(COUNTDOWN_S as u32 + 40) {
        assert_eq!(unsafe { apx_race_tick(api, TICK_RATE as u32) }, APX_OK);
        for _ in 0..TICK_RATE {
            tick_field(&mut server);
        }
        assert_eq!(
            digest(api),
            race_digest(&server),
            "the races part at second {second}"
        );
    }

    // The cars as the API reads them out: the field, racing.
    let mut cars = vec![ApxRaceCar::default(); AI as usize];
    let mut written = 0usize;
    assert_eq!(
        unsafe { apx_race_cars(api, cars.as_mut_ptr(), cars.len(), &mut written) },
        APX_OK
    );
    assert_eq!(written, AI as usize);
    assert!(cars.iter().all(|car| car.speed_mps > 10.0), "{cars:?}");
    assert_eq!(cars, race_cars(&server));
    let mut tick = 0u32;
    assert_eq!(unsafe { apx_race_current_tick(api, &mut tick) }, APX_OK);
    assert_eq!(tick, server.session.current_tick);
    unsafe { apx_race_free(api) };
}

#[test]
fn a_seed_is_one_race_and_another_seed_another() {
    let a = start_through_the_api(11);
    let b = start_through_the_api(11);
    let c = start_through_the_api(12);
    for race in [a, b, c] {
        assert_eq!(
            unsafe { apx_race_tick(race, TICK_RATE as u32 * 10) },
            APX_OK
        );
    }
    assert_eq!(digest(a), digest(b));
    assert_ne!(digest(a), digest(c));
    // Too small a buffer says how big it has to be and writes nothing else.
    let mut written = 0usize;
    assert_eq!(
        unsafe { apx_race_cars(a, std::ptr::null_mut(), 0, &mut written) },
        APX_ERR_BUFFER
    );
    assert_eq!(written, AI as usize);
    for race in [a, b, c] {
        unsafe { apx_race_free(race) };
    }
}
