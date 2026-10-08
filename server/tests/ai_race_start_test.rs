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

const TICK_RATE: u16 = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ;
/// The menu's demo field (`apexsim.demo.AiCount`).
const DEMO_FIELD: u8 = 10;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

/// A race of `ai_count` AI drivers all in `car`, as a demo session sets it
/// up, with the lights already out.
/// `SURVEY_ROAD_CONTACT=mesh` runs the survey on each circuit's road mesh
/// (`docs/ROAD_MESH.md`) instead of the centerline, so the two backends
/// can be compared line by line.
fn road_contact() -> apexsim_server::config::RoadContactMode {
    std::env::var("SURVEY_ROAD_CONTACT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(apexsim_server::config::RoadContactMode::Centerline)
}

/// `<stem>.yaml` in the shipped folder, else the player's own
/// (`content/tracks/custom`, where `scripts/ac_import.py` writes).
fn track_file(stem: &str) -> PathBuf {
    let custom = repo(&format!("content/tracks/custom/{stem}.yaml"));
    let default = repo(&format!("content/tracks/default/{stem}.yaml"));
    if !default.exists() && custom.exists() {
        custom
    } else {
        default
    }
}

fn ai_race(track: &str, car: &str, ai_count: u8) -> GameSession {
    let mut track =
        TrackLoader::load_from_file_with(track_file(track), road_contact()).expect("track loads");
    // `SURVEY_WIND_KPH=25` races the field in that wind, from 45° off the
    // start straight's nose (every straight meets it differently); still
    // air otherwise, the reference day's density either way.
    if let Some(kph) = std::env::var("SURVEY_WIND_KPH")
        .ok()
        .and_then(|v| v.parse::<u8>().ok())
    {
        apexsim_server::data::SessionConditions {
            wind_kph: Some(kph),
            wind_from_deg: Some(45),
            ..apexsim_server::data::SessionConditions::DEFAULT
        }
        .apply_to_track(&mut track);
        track.track_surface.air_density_ratio = 1.0;
    }
    let car = CarLoader::load_from_file(&repo(&format!("content/cars/default/{car}/car.toml")))
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
    assert_clean_start("Spa", "fugazzi-sf26");
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
    // The shipped circuits and the player's own (an imported AC track is
    // named by its stem too: `SURVEY_TRACKS=KsZandvoort`).
    let mut tracks: Vec<String> = ["content/tracks/default", "content/tracks/custom"]
        .iter()
        .filter_map(|dir| std::fs::read_dir(repo(dir)).ok())
        .flatten()
        .filter_map(|entry| {
            let path = entry.ok()?.path();
            if path.extension()? != "yaml" {
                return None;
            }
            Some(path.file_stem()?.to_string_lossy().into_owned())
        })
        .collect();
    tracks.sort();
    tracks.dedup();
    // `SURVEY_TRACKS=Spa,Monza` narrows the run to a few circuits.
    if let Ok(only) = std::env::var("SURVEY_TRACKS") {
        let only: Vec<&str> = only.split(',').map(str::trim).collect();
        tracks.retain(|t| only.contains(&t.as_str()));
    }
    for track in tracks {
        for car in ["yotota-lmp2", "fugazzi-sf26", "posh-gt3rs"] {
            let mut race = ai_race(&track, car, DEMO_FIELD);
            let (mut contact, mut off, mut slide, mut at_3s) = (0u32, 0u32, 0u32, 0);
            let mut dbg: HashMap<(i32, u8), u32> = HashMap::new();
            let mut passes = Passes::default();
            let mut tactics: HashMap<String, u32> = HashMap::new();
            let mut was: HashMap<PlayerId, (bool, bool, u32)> = HashMap::new();
            for t in 1..=(TICK_RATE as u32 * 180) {
                tick(&mut race);
                if t.is_multiple_of(TICK_RATE as u32 / 4) {
                    passes.sample(&race);
                }
                if std::env::var("SURVEY_EVENTS").is_ok() {
                    log_events(&race, t, &mut was);
                }
                if t == TICK_RATE as u32 * 3 {
                    at_3s = race
                        .session
                        .participants
                        .values()
                        .filter(|c| out_of_shape(c))
                        .count();
                }
                // A car out of the race is parked in its box (towed), not
                // racing: it is counted as retired, not as time off the
                // road against the garage walls.
                for c in race
                    .session
                    .participants
                    .values()
                    .filter(|c| !c.towed && c.damage.is_drivable)
                {
                    if c.is_colliding && std::env::var("SURVEY_DBG").is_ok() {
                        *tactics
                            .entry(format!("{:?}", c.racecraft.tactic))
                            .or_insert(0u32) += 1;
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
            if !tactics.is_empty() {
                println!("    contact by tactic: {tactics:?}");
            }
            for ((bucket, grid), ticks) in worst.into_iter().take(4) {
                println!(
                    "    car {grid}: {:.1} s of contact around station {} m",
                    secs(ticks),
                    bucket * 20
                );
            }
            // Cars progressive damage put out (`crate::damage`).
            let retired = race
                .session
                .participants
                .values()
                .filter(|c| !c.damage.is_drivable)
                .count();
            println!(
                "{track:>14} {car:>14}: out of shape at 3 s {at_3s}, car-seconds of contact {:5.1}, off {:5.1}, sliding {:5.1}, retired {retired}, passes {}",
                secs(contact),
                secs(off),
                secs(slide),
                passes.count
            );
        }
    }
}

/// `SURVEY_EVENTS=1`: each contact as it starts, with the nearest car,
/// and every spell off the road longer than 2 s as it ends.
fn log_events(race: &GameSession, t: u32, was: &mut HashMap<PlayerId, (bool, bool, u32)>) {
    let total = apexsim_server::laps::track_length_m(&race.track_config).max(1.0);
    let cars: Vec<&CarState> = race.session.participants.values().collect();
    for c in &cars {
        let (hit, off, off_since) = was.get(&c.player_id).copied().unwrap_or((false, false, 0));
        if c.is_colliding && !hit {
            let near = cars
                .iter()
                .filter(|o| o.player_id != c.player_id)
                .map(|o| {
                    let rel = (o.track_progress - c.track_progress + 0.5 * total).rem_euclid(total)
                        - 0.5 * total;
                    (o, rel)
                })
                .min_by(|a, b| a.1.abs().total_cmp(&b.1.abs()));
            if let Some((o, rel)) = near {
                println!(
                    "    t {:6.1} car {:2} {:?}/{:.2} at {:6.0} m lat {:5.1} v {:4.1} | car {:2} {:?}/{:.2} {:+5.1} m ahead lat {:5.1} v {:4.1}",
                    t as f32 / TICK_RATE as f32,
                    c.grid_position,
                    c.racecraft.tactic,
                    c.racecraft.blend,
                    c.track_progress,
                    -c.lateral_offset_m,
                    c.speed_mps,
                    o.grid_position,
                    o.racecraft.tactic,
                    o.racecraft.blend,
                    rel,
                    -o.lateral_offset_m,
                    o.speed_mps
                );
            }
        }
        // `SURVEY_TRACE=grid,from_s,to_s`: one car every tenth of a second.
        if let Ok(spec) = std::env::var("SURVEY_TRACE") {
            let v: Vec<f32> = spec.split(',').filter_map(|x| x.parse().ok()).collect();
            let secs = t as f32 / TICK_RATE as f32;
            if v.len() == 3
                && c.grid_position as f32 == v[0]
                && secs >= v[1]
                && secs <= v[2]
                && t.is_multiple_of(TICK_RATE as u32 / 10)
            {
                println!(
                    "    trace {secs:6.1} at {:6.1} m lat {:6.1} v {:4.1} on {} {:?} lane {:5.1} blend {:.2} pace {:.3} thr {:.2} brk {:.2} str {:+.2} hit {}",
                    c.track_progress,
                    -c.lateral_offset_m,
                    c.speed_mps,
                    c.is_on_track,
                    c.racecraft.tactic,
                    c.racecraft.lane_m,
                    c.racecraft.blend,
                    c.racecraft.pace,
                    c.throttle_input,
                    c.brake_input,
                    c.steering_input,
                    c.is_colliding
                );
            }
        }
        let d = &c.damage;
        let zones = [
            d.front_damage_percent,
            d.rear_damage_percent,
            d.left_damage_percent,
            d.right_damage_percent,
            d.engine_damage_percent,
        ];
        let worst: f32 = zones.iter().copied().fold(0.0, f32::max);
        let key = uuid::Uuid::from_u64_pair(c.player_id.as_u64_pair().0, !0);
        let before = was.get(&key).map_or(0.0, |w| w.2 as f32 / 10.0);
        if worst >= before + 10.0 || (!d.is_drivable && before < 100.0) {
            println!(
                "    t {:6.1} car {:2} {:?} at {:6.0} m v {:4.1} damage F/R/L/R/E {:?}{}",
                t as f32 / TICK_RATE as f32,
                c.grid_position,
                c.racecraft.tactic,
                c.track_progress,
                c.speed_mps,
                zones.map(|z| z.round() as i32),
                if d.is_drivable { "" } else { " RETIRED" }
            );
            let mark = if d.is_drivable { worst } else { 100.0 };
            was.insert(key, (false, false, (mark * 10.0) as u32));
        }
        let off_now = !c.is_on_track;
        let since = if off_now && !off { t } else { off_since };
        if !off_now && off && t - off_since > 2 * TICK_RATE as u32 {
            println!(
                "    car {:2} off the road {:.1} s from {:.1} s",
                c.grid_position,
                (t - off_since) as f32 / TICK_RATE as f32,
                off_since as f32 / TICK_RATE as f32
            );
        }
        was.insert(c.player_id, (c.is_colliding, off_now, since));
    }
}

/// Passes on the road: two cars within [`Passes::NEAR_M`] of each other
/// that swap order, counted once the new leader is [`Passes::CLEAR_M`]
/// ahead (so a dice side by side is not a pass a tick). A car on the pit
/// route, in a garage or out of the race is not passed and passes nobody.
#[derive(Default)]
struct Passes {
    leader: HashMap<(PlayerId, PlayerId), PlayerId>,
    count: u32,
}

impl Passes {
    const NEAR_M: f32 = 30.0;
    const CLEAR_M: f32 = 2.0;

    fn sample(&mut self, race: &GameSession) {
        let total = apexsim_server::laps::track_length_m(&race.track_config).max(1.0);
        let cars: Vec<&CarState> = race
            .session
            .participants
            .values()
            .filter(|c| !c.pit.driving && !c.in_garage && c.damage.is_drivable && !c.towed)
            .collect();
        for (i, a) in cars.iter().enumerate() {
            for b in &cars[i + 1..] {
                let rel = (a.track_progress - b.track_progress + 0.5 * total).rem_euclid(total)
                    - 0.5 * total;
                let key = (a.player_id, b.player_id);
                if rel.abs() > Self::NEAR_M {
                    self.leader.remove(&key);
                    continue;
                }
                if rel.abs() < Self::CLEAR_M {
                    continue;
                }
                let ahead = if rel > 0.0 { a.player_id } else { b.player_id };
                if let Some(before) = self.leader.insert(key, ahead) {
                    self.count += (before != ahead) as u32;
                }
            }
        }
    }
}
