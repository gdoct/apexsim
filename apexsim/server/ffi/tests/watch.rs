//! A watched hotlap through the C API: started from the client's
//! `CreateSession`, ticked, its stream records taken framed (the header
//! first), its corners as the `TrackCorners` message, and what it learnt
//! kept in its records folder when it ends. The same message and seed are
//! the same stream.

use std::ffi::{CStr, CString};
use std::path::{Path, PathBuf};

use apexsim_physics::*;
use apexsim_protocol::data::*;
use apexsim_protocol::network::{ClientMessage, HotlapWatchOptions, ServerMessage};

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Snorisring/Snorisring.yaml";
const CARS: &str = "../apexsim-content/cars";
const CAR: &str = "posh-gt3rs";
const HZ: u16 = apexsim_protocol::game_session::DEFAULT_TICK_RATE_HZ;

fn create_session() -> Vec<u8> {
    let message = ClientMessage::CreateSession {
        track_config_id: uuid::Uuid::nil(),
        max_players: 1,
        ai_count: 1,
        lap_limit: 0,
        session_kind: SessionKind::HotlapWatch,
        allowed_assists: Default::default(),
        conditions: SessionConditions {
            weather: Weather::Cloudy,
            time_of_day_minutes: 15 * 60,
            ..SessionConditions::DEFAULT
        },
        damage: Default::default(),
        ai_skill: None,
        race_seconds: None,
        grid_order: Vec::new(),
        qualifying_seconds: None,
        weekend: None,
        watch: Some(HotlapWatchOptions {
            stream: true,
            setup: None,
        }),
        qualifying_cutoff_pct: None,
        mandatory_stops: 0,
        compound_change: false,
        qualifying_format: Default::default(),
        qualifying_lap_allowance: 0,
        race_rules: Default::default(),
        classes: Vec::new(),
    };
    rmp_serde::to_vec_named(&message).unwrap()
}

fn cp(path: &Path) -> CString {
    CString::new(path.to_string_lossy().into_owned()).unwrap()
}

fn start(records: Option<&Path>) -> *mut ApxWatch {
    let message = create_session();
    let (track, cars, car) = (
        cp(&repo(TRACK)),
        cp(&repo(CARS)),
        CString::new(CAR).unwrap(),
    );
    let records = records.map(cp);
    let mut watch: *mut ApxWatch = std::ptr::null_mut();
    let status = unsafe {
        apx_watch_start(
            message.as_ptr(),
            message.len(),
            track.as_ptr(),
            cars.as_ptr(),
            car.as_ptr(),
            records.as_ref().map_or(std::ptr::null(), |r| r.as_ptr()),
            1,
            HZ,
            7,
            &mut watch,
        )
    };
    assert_eq!(status, APX_OK, "{:?}", unsafe {
        CStr::from_ptr(apx_last_error())
    });
    watch
}

fn take(watch: *mut ApxWatch) -> Vec<u8> {
    let mut size = 0usize;
    let status = unsafe { apx_watch_take(watch, std::ptr::null_mut(), 0, &mut size) };
    if size == 0 {
        assert_eq!(status, APX_OK);
        return Vec::new();
    }
    assert_eq!(status, APX_ERR_BUFFER);
    let mut buf = vec![0u8; size];
    assert_eq!(
        unsafe { apx_watch_take(watch, buf.as_mut_ptr(), buf.len(), &mut size) },
        APX_OK
    );
    buf
}

/// The record bodies of a framed run.
fn records(run: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut at = 0;
    while at + 4 <= run.len() {
        let len = u32::from_be_bytes(run[at..at + 4].try_into().unwrap()) as usize;
        out.push(&run[at + 4..at + 4 + len]);
        at += 4 + len;
    }
    assert_eq!(at, run.len(), "whole records");
    out
}

#[test]
fn a_watch_streams_its_laps_and_keeps_what_it_learnt() {
    if !repo(TRACK).is_file() {
        eprintln!("skipped: no content checkout beside the code");
        return;
    }
    let dir = std::env::temp_dir().join(format!("apx-watch-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let watch = start(Some(&dir));

    // The corners, as the message the codec decodes.
    let mut size = 0usize;
    unsafe { apx_watch_corners(watch, std::ptr::null_mut(), 0, &mut size) };
    let mut corners = vec![0u8; size];
    assert_eq!(
        unsafe { apx_watch_corners(watch, corners.as_mut_ptr(), corners.len(), &mut size) },
        APX_OK
    );
    match rmp_serde::from_slice::<ServerMessage>(&corners).unwrap() {
        ServerMessage::TrackCorners(data) => assert!(data.corners.len() >= 5),
        other => panic!("not TrackCorners: {other:?}"),
    }

    // Nothing before the first tick; then the catch-up, the header first.
    assert!(take(watch).is_empty());
    assert_eq!(unsafe { apx_watch_tick(watch, 1) }, APX_OK);
    let first = take(watch);
    let bodies = records(&first);
    assert!(bodies.len() >= 3, "header, roster, path at least");
    assert_eq!(bodies[0][1], 1, "the header first");

    // A minute and a half of laps streams frames.
    for _ in 0..90 {
        assert_eq!(unsafe { apx_watch_tick(watch, HZ as u32) }, APX_OK);
    }
    let laps = take(watch);
    assert!(
        records(&laps).len() > 90 * 25,
        "frames at about 30 a second"
    );
    unsafe { apx_watch_free(watch) };
    assert!(
        dir.join("watch_levels.json").is_file(),
        "the ladder level is kept"
    );

    // The same message and seed: the same stream.
    let (a, b) = (start(None), start(None));
    for watch in [a, b] {
        unsafe { apx_watch_tick(watch, HZ as u32 * 20) };
    }
    assert!(take(a) == take(b), "a seeded watch is the same laps");
    unsafe {
        apx_watch_free(a);
        apx_watch_free(b);
    }
    let _ = std::fs::remove_dir_all(&dir);
}
