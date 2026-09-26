//! The demo race behind the menu started with the whole field sliding into
//! each other off the grid: every AI aimed at the same point on the line, so
//! the side-by-side pairs steered into each other, leaned on each other nose
//! in, and came apart pointing half a radian away from where they were going.
//!
//! These run a full AI race start on real circuits and count the cars that
//! are out of shape a few seconds in.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::generate_default_ai_profiles;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;

const TICK_RATE: u16 = 240;
/// The menu's demo field (`apexsim.demo.AiCount`).
const DEMO_FIELD: u8 = 10;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

/// A race of `ai_count` AI drivers all in `car`, as a demo session sets it
/// up, with the lights already out.
fn ai_race(track: &str, car: &str, ai_count: u8) -> GameSession {
    let track = TrackLoader::load_from_file(repo(&format!("content/tracks/real/{track}.yaml")))
        .expect("track loads");
    let car = CarLoader::load_from_file(&repo(&format!("content/cars/{car}/car.toml")))
        .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);

    let mut profiles = generate_default_ai_profiles(ai_count);
    // The drivers' ids decide the grid (participants is ordered by id) and
    // seed the AI's noise, so random ids made every run a different race.
    // The Le Mans lap failed on CI about one run in four: in those fields
    // the last car on the grid came up alongside the third, on the outside,
    // into the Dunlop curve at 650 m, was held there by the side margin and
    // ran wide for 3-4 s. That is how the AI races side by side, not the
    // yaw-seam bug the test is about. Fixed ids make each test one race,
    // tick for tick; `AI_TEST_SEED=<n>` runs another field.
    let seed: u64 = std::env::var("AI_TEST_SEED")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(1);
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = seeded_id(seed, i as u64);
    }
    let session = RaceSession::new(
        seeded_id(seed, u64::MAX),
        track.id,
        SessionKind::Demo,
        ai_count,
        ai_count,
        3,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    assert_eq!(race.session.participants.len(), ai_count as usize);
    race.set_game_mode(GameMode::Race);
    race
}

/// A stable id for the `index`th driver of a seeded field (splitmix64, as
/// `replay_tools` seeds its races).
fn seeded_id(seed: u64, index: u64) -> uuid::Uuid {
    let mut state = seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    let mut next = || {
        state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    };
    uuid::Uuid::from_u64_pair(next(), next())
}

/// One server tick, with the inputs made the way the game loop makes them.
fn tick(race: &mut GameSession) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .keys()
        .map(|id| (*id, race.generate_ai_input(id)))
        .collect();
    race.tick(&inputs);
}

/// Angle between where the car points and where it is going (rad).
fn body_slip(car: &CarState) -> f32 {
    if car.speed_mps < 2.0 {
        return 0.0;
    }
    let (c, s) = (car.yaw_rad.cos(), car.yaw_rad.sin());
    let forward = car.vel_x * c + car.vel_y * s;
    let sideways = -car.vel_x * s + car.vel_y * c;
    sideways.atan2(forward.abs())
}

fn out_of_shape(car: &CarState) -> bool {
    body_slip(car).abs() > 0.10 || car.angular_vel_yaw.abs() > 0.6 || !car.is_on_track
}

fn describe(car: &CarState) -> String {
    format!(
        "grid {} at {:.1} m/s: slip {:.2} rad, yaw rate {:.2} rad/s, on track {}, lateral {:.1} m",
        car.grid_position,
        car.speed_mps,
        body_slip(car),
        car.angular_vel_yaw,
        car.is_on_track,
        car.lateral_offset_m
    )
}

fn assert_clean_start(track: &str, car: &str) {
    let mut race = ai_race(track, car, DEMO_FIELD);
    let mut contact_ticks = 0;
    for _ in 0..(TICK_RATE as u32 * 3) {
        tick(&mut race);
        contact_ticks += race
            .session
            .participants
            .values()
            .filter(|c| c.is_colliding)
            .count();
    }
    let unbalanced: Vec<String> = race
        .session
        .participants
        .values()
        .filter(|c| out_of_shape(c))
        .map(describe)
        .collect();
    assert!(
        unbalanced.is_empty(),
        "{track} / {car}: {} of {DEMO_FIELD} cars out of shape 3 s after the start:\n  {}",
        unbalanced.len(),
        unbalanced.join("\n  ")
    );
    assert_eq!(
        contact_ticks, 0,
        "{track} / {car}: cars touched off the grid ({contact_ticks} car-ticks of contact)"
    );
    assert!(
        race.session
            .participants
            .values()
            .all(|c| c.speed_mps > 10.0),
        "{track} / {car}: somebody never got going"
    );
}

#[test]
fn ai_field_stays_composed_off_the_grid() {
    // The demo's default circuit and car (the reported screenshot), plus an
    // F1 field, which has the power to spin its rears on the way out.
    assert_clean_start("LeMans", "yotota-lmp2");
    assert_clean_start("Spa", "redhorse-rb20");
}

/// Le Mans has a left-hander at 4.1 km where the road heads due west, across
/// the ±PI yaw seam. The AI's heading error wrapped the wrong way there and
/// every car turned right at full lock into the grass.
#[test]
fn ai_field_makes_the_first_lap_at_le_mans() {
    let mut race = ai_race("LeMans", "yotota-lmp2", 4);
    let mut off_ticks: HashMap<u8, u32> = HashMap::new();
    for _ in 0..(TICK_RATE as u32 * 150) {
        tick(&mut race);
        for car in race.session.participants.values() {
            if !car.is_on_track {
                *off_ticks.entry(car.grid_position).or_default() += 1;
            }
        }
    }
    let past_the_seam = race
        .session
        .participants
        .values()
        .filter(|c| c.track_progress > 4300.0)
        .count();
    assert_eq!(past_the_seam, 4, "cars stuck before 4.3 km: {off_ticks:?}");
    let worst = off_ticks.values().max().copied().unwrap_or(0);
    assert!(
        worst < TICK_RATE as u32,
        "a car spent {:.1} s off the road: {off_ticks:?}",
        worst as f32 / TICK_RATE as f32
    );
}

/// Every circuit with three kinds of car: prints how composed the field is.
/// `cargo test --release --test ai_race_start_test -- --ignored --nocapture`
#[test]
#[ignore]
fn survey_ai_races_on_every_circuit() {
    let mut tracks: Vec<String> = std::fs::read_dir(repo("content/tracks/real"))
        .expect("tracks")
        .filter_map(|entry| {
            let path = entry.ok()?.path();
            if path.extension()? != "yaml" {
                return None;
            }
            Some(path.file_stem()?.to_string_lossy().into_owned())
        })
        .collect();
    tracks.sort();
    // `SURVEY_TRACKS=Spa,Monza` narrows the run to a few circuits.
    if let Ok(only) = std::env::var("SURVEY_TRACKS") {
        let only: Vec<&str> = only.split(',').map(str::trim).collect();
        tracks.retain(|t| only.contains(&t.as_str()));
    }
    for track in tracks {
        for car in ["yotota-lmp2", "redhorse-rb20", "posh-911gt3"] {
            let mut race = ai_race(&track, car, DEMO_FIELD);
            let (mut contact, mut off, mut slide, mut at_3s) = (0u32, 0u32, 0u32, 0);
            let mut dbg: HashMap<(i32, u8), u32> = HashMap::new();
            for t in 1..=(TICK_RATE as u32 * 180) {
                tick(&mut race);
                if t == TICK_RATE as u32 * 3 {
                    at_3s = race
                        .session
                        .participants
                        .values()
                        .filter(|c| out_of_shape(c))
                        .count();
                }
                for c in race.session.participants.values() {
                    if c.is_colliding && std::env::var("SURVEY_DBG").is_ok() {
                        *dbg.entry(((c.track_progress / 20.0) as i32, c.grid_position))
                            .or_insert(0u32) += 1;
                    }
                    contact += c.is_colliding as u32;
                    off += !c.is_on_track as u32;
                    slide += (c.is_on_track && !c.is_colliding && body_slip(c).abs() > 0.10) as u32;
                }
            }
            let secs = |ticks: u32| ticks as f32 / TICK_RATE as f32;
            // `SURVEY_DBG=1`: where the contact was, so a car pinned against
            // a wall can be found on the map.
            let mut worst: Vec<_> = dbg.into_iter().collect();
            worst.sort_by_key(|w| std::cmp::Reverse(w.1));
            for ((bucket, grid), ticks) in worst.into_iter().take(4) {
                println!(
                    "    car {grid}: {:.1} s of contact around station {} m",
                    secs(ticks),
                    bucket * 20
                );
            }
            println!(
                "{track:>14} {car:>14}: out of shape at 3 s {at_3s}, car-seconds of contact {:5.1}, off {:5.1}, sliding {:5.1}",
                secs(contact),
                secs(off),
                secs(slide)
            );
        }
    }
}
