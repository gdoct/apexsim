//! The demo race behind the menu started with the whole field sliding into
//! each other off the grid: every AI aimed at the same point on the line, so
//! the side-by-side pairs steered into each other, leaned on each other nose
//! in, and came apart pointing half a radian away from where they were going.
//!
//! These run a full AI race start on real circuits and count the cars that
//! are out of shape a few seconds in.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::generate_ai_profiles;
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
/// (`docs/content/road-mesh.md`) instead of the centerline, so the two backends
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
    let custom = repo(&format!("content/tracks/custom/{stem}/{stem}.yaml"));
    let default = repo(&format!("content/tracks/default/{stem}/{stem}.yaml"));
    if !default.exists() && custom.exists() {
        custom
    } else {
        default
    }
}

/// `<car>/car.toml` in the shipped folder, else the player's own
/// (`content/cars/custom`, where `scripts/ac_car_import.py` writes).
fn car_file(car: &str) -> PathBuf {
    let custom = repo(&format!("content/cars/custom/{car}/car.toml"));
    let default = repo(&format!("content/cars/default/{car}/car.toml"));
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
    // `SURVEY_GRIP=0.55` scales the baked road grip by that, for finding
    // where the field stops coping.
    if let Some(grip) = std::env::var("SURVEY_GRIP")
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
    {
        for point in &mut track.centerline {
            point.grip_modifier *= grip;
        }
        track.track_surface.base_grip *= grip;
    }
    // `SURVEY_WEATHER=snow` (or rain, lightrain, heavyrain...) bakes that
    // sky onto the track, with the road state it leaves: the racing line
    // and the AI's speed profile are built for it.
    if let Ok(name) = std::env::var("SURVEY_WEATHER") {
        let weather = apexsim_server::data::Weather::ALL
            .into_iter()
            .find(|w| w.name().replace(' ', "") == name.to_lowercase().replace(' ', ""))
            .unwrap_or_else(|| panic!("SURVEY_WEATHER={name}: no such weather"));
        // `SURVEY_AIR_C=15` holds the air there, whatever the weather's own.
        let air = std::env::var("SURVEY_AIR_C")
            .ok()
            .and_then(|v| v.parse::<i8>().ok());
        let mut conditions = apexsim_server::data::SessionConditions {
            weather,
            air_temp_c: air,
            ..apexsim_server::data::SessionConditions::DEFAULT
        }
        .resolve(1);
        if air.is_some() {
            conditions.air_temp_c = air;
        }
        conditions.apply_to_track(&mut track);
    }
    let car = CarLoader::load_from_file(&car_file(car)).expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);

    // `SURVEY_AI_LEVEL=95` seats the field at that level, as a host's AI
    // level does (spread over 4 points round it); unset, the mixed field.
    let level: Option<u8> = std::env::var("SURVEY_AI_LEVEL")
        .ok()
        .and_then(|s| s.parse().ok());
    let mut profiles = generate_ai_profiles(ai_count, level);
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
    if std::env::var("DBG_NO_ROAD").is_ok() {
        race.track_config.road_state = None;
    }
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    assert_eq!(race.session.participants.len(), ai_count as usize);
    // `SURVEY_START=formation` (or `rolling`) gets the race away behind a
    // formation lap (a rolling start's pace lap): the race is handed over
    // at the green.
    let rules = match std::env::var("SURVEY_START").as_deref() {
        Ok("formation") => Some(RaceRules {
            formation_lap: true,
            ..Default::default()
        }),
        Ok("rolling") => Some(RaceRules {
            start: StartType::Rolling,
            ..Default::default()
        }),
        _ => None,
    };
    if let Some(rules) = rules {
        race.set_race_rules(rules);
        race.start_countdown_mode(0, GameMode::Race);
        for _ in 0..(TICK_RATE as u32 * 900) {
            if race.session.state == SessionState::Racing {
                break;
            }
            tick(&mut race);
        }
        assert_eq!(race.session.state, SessionState::Racing, "no green");
        return race;
    }
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
///
/// Time off the road is counted only round the seam: the first lap's
/// traffic elsewhere is the survey's business, and it differs between a
/// tree with the gitignored track sidecars and a fresh checkout (CI), which
/// race the same seed on different curbs and walls.
#[test]
fn ai_field_makes_the_first_lap_at_le_mans() {
    const SEAM_M: std::ops::Range<f32> = 3800.0..4600.0;
    let mut race = ai_race("LeMans", "yotota-lmp2", 4);
    let mut off_ticks: HashMap<u8, u32> = HashMap::new();
    for _ in 0..(TICK_RATE as u32 * 150) {
        tick(&mut race);
        for car in race.session.participants.values() {
            if !car.is_on_track && SEAM_M.contains(&car.track_progress) {
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
            // A track is its own folder: `<stem>/<stem>.yaml`.
            let path = entry.ok()?.path();
            let stem = path.file_name()?.to_string_lossy().into_owned();
            path.join(format!("{stem}.yaml")).is_file().then_some(stem)
        })
        .collect();
    tracks.sort();
    tracks.dedup();
    // `SURVEY_TRACKS=Spa,Monza` narrows the run to a few circuits.
    if let Ok(only) = std::env::var("SURVEY_TRACKS") {
        let only: Vec<&str> = only.split(',').map(str::trim).collect();
        tracks.retain(|t| only.contains(&t.as_str()));
    }
    // `SURVEY_CARS=AcFriendsFerrari499pLmh` races these cars instead (a
    // folder under content/cars/default or, for an import, custom).
    let cars: Vec<String> = std::env::var("SURVEY_CARS")
        .map(|v| v.split(',').map(|c| c.trim().to_string()).collect())
        .unwrap_or_else(|_| {
            ["yotota-lmp2", "fugazzi-sf26", "posh-gt3rs"]
                .map(String::from)
                .to_vec()
        });
    for track in tracks {
        for car in &cars {
            let mut race = ai_race(&track, car, DEMO_FIELD);
            let mut causes = DamageCauses::default();
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
                causes.sample(&race);
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
                "{track:>14} {car:>14}: out of shape at 3 s {at_3s}, car-seconds of contact {:5.1}, off {:5.1}, sliding {:5.1}, retired {retired}{}, passes {}",
                secs(contact),
                secs(off),
                secs(slide),
                causes.retired_by(&race),
                passes.count
            );
        }
    }
}

/// What damaged each car, by zone: each tick's rise in a zone is put down to
/// another car (in contact with one within 8 m), a wall (in contact, no car
/// near), or the car itself (kerbs, bottoming, a landing; for the engine its
/// revs, heat and wear). A retired car is reported by what did most to the
/// zone that went.
#[derive(Default)]
struct DamageCauses {
    last: HashMap<PlayerId, [f32; 5]>,
    /// Per car and zone: car, wall, own.
    by: HashMap<PlayerId, [[f32; 3]; 5]>,
}

impl DamageCauses {
    const NAMES: [&'static str; 3] = ["car", "wall", "own"];

    fn zones(c: &CarState) -> [f32; 5] {
        let d = &c.damage;
        [
            d.front_damage_percent,
            d.rear_damage_percent,
            d.left_damage_percent,
            d.right_damage_percent,
            d.engine_damage_percent,
        ]
    }

    fn sample(&mut self, race: &GameSession) {
        let cars: Vec<&CarState> = race.session.participants.values().collect();
        for c in &cars {
            let now = Self::zones(c);
            let before = self.last.insert(c.player_id, now).unwrap_or(now);
            let rise: Vec<(usize, f32)> = (0..5)
                .map(|z| (z, now[z] - before[z]))
                .filter(|(_, r)| *r > 0.0)
                .collect();
            if rise.is_empty() {
                continue;
            }
            let cause = if c.is_colliding {
                let near = cars.iter().any(|o| {
                    o.player_id != c.player_id && (o.pos_x - c.pos_x).hypot(o.pos_y - c.pos_y) < 8.0
                });
                if near {
                    0
                } else {
                    1
                }
            } else {
                2
            };
            let by = self.by.entry(c.player_id).or_default();
            for (z, r) in rise {
                by[z][cause] += r;
            }
        }
    }

    /// ` (car 2, wall 1)`: the retired cars by cause, or nothing.
    fn retired_by(&self, race: &GameSession) -> String {
        let mut count = [0u32; 3];
        for c in race.session.participants.values() {
            if c.damage.is_drivable {
                continue;
            }
            let zones = Self::zones(c);
            let Some(by) = self.by.get(&c.player_id) else {
                continue;
            };
            let zone = (0..5)
                .max_by(|a, b| zones[*a].total_cmp(&zones[*b]))
                .unwrap_or(0);
            let cause = (0..3)
                .max_by(|a, b| by[zone][*a].total_cmp(&by[zone][*b]))
                .unwrap_or(0);
            count[cause] += 1;
        }
        let parts: Vec<String> = (0..3)
            .filter(|i| count[*i] > 0)
            .map(|i| format!("{} {}", Self::NAMES[i], count[i]))
            .collect();
        if parts.is_empty() {
            String::new()
        } else {
            format!(" ({})", parts.join(", "))
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

/// A snow race at Spa (`SURVEY_WEATHER` as the survey reads it, snow when
/// unset) for `SNOW_SECONDS` (400): per car, the seconds it sat on the road
/// with its throttle open and getting nowhere, its hottest engine, its
/// engine damage, and whether it is out. A car that cannot climb Raidillon
/// used to sit at the limiter until its engine failed.
/// `cargo test --release --test ai_race_start_test snow_stall_survey -- --ignored --nocapture`
/// (`SURVEY_CAR=bugotti-chiffon-hypercar`, `SURVEY_TRACKS=Spa`).
#[test]
#[ignore]
fn snow_stall_survey() {
    if std::env::var("SURVEY_WEATHER").is_err() {
        // SAFETY: a test of its own process, before anything reads it.
        unsafe { std::env::set_var("SURVEY_WEATHER", "snow") };
    }
    let track = std::env::var("SURVEY_TRACKS").unwrap_or_else(|_| "Spa".into());
    let car = std::env::var("SURVEY_CAR").unwrap_or_else(|_| "bugotti-chiffon-hypercar".into());
    let seconds: u32 = std::env::var("SNOW_SECONDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(400);
    let mut race = ai_race(&track, &car, 8);
    let mut stalled: HashMap<PlayerId, u32> = HashMap::new();
    // The spell now and the longest, ticks.
    let mut spell: HashMap<PlayerId, (u32, u32)> = HashMap::new();
    let mut hottest: HashMap<PlayerId, f32> = HashMap::new();
    let mut hit: HashMap<PlayerId, bool> = HashMap::new();
    let mut where_stalled: HashMap<i32, u32> = HashMap::new();
    // Degrees of coolant gained, by the car's speed band (< 1, < 3, < 10,
    // < 25, faster m/s): where the heat comes from.
    let mut heat_by_speed = [0.0f32; 5];
    let mut last_c: HashMap<PlayerId, f32> = HashMap::new();
    for _ in 1..=(TICK_RATE as u32 * seconds) {
        tick(&mut race);
        let trace: Option<u8> = std::env::var("SNOW_TRACE")
            .ok()
            .and_then(|v| v.parse().ok());
        for c in race.session.participants.values() {
            if trace == Some(c.grid_position)
                && c.speed_mps < 3.0
                && race
                    .session
                    .current_tick
                    .is_multiple_of(TICK_RATE as u32 / 2)
            {
                println!(
                    "    trace t {:6.1} at {:7.1} v {:4.1} on {} hit {} gear {} thr {:.2} brk {:.2} str {:+.2} temp {:5.1} rev {:.1} reset {} off_s {:.1} stuck {:.1} recov {}",
                    race.session.current_tick as f32 / TICK_RATE as f32,
                    c.track_progress, c.speed_mps, c.is_on_track, c.is_colliding, c.gear,
                    c.throttle_input, c.brake_input, c.steering_input, c.water_temp_c,
                    c.racecraft.reverse_s, c.racecraft.wants_reset, c.racecraft.off_road_s,
                    c.racecraft.stuck_s, c.recovery.is_some()
                );
            }
            let before = last_c
                .insert(c.player_id, c.water_temp_c)
                .unwrap_or(c.water_temp_c);
            if c.water_temp_c > before {
                let band = [1.0, 3.0, 10.0, 25.0]
                    .iter()
                    .position(|v| c.speed_mps < *v)
                    .unwrap_or(4);
                heat_by_speed[band] += c.water_temp_c - before;
            }
            let h = hottest.entry(c.player_id).or_insert(0.0);
            *h = h.max(c.engine_temp_c);
            if c.is_colliding {
                hit.insert(c.player_id, true);
            }
            if c.is_on_track
                && !c.towed
                && !c.pit.driving
                && c.damage.is_drivable
                && c.speed_mps < 2.0
                && c.throttle_input > 0.5
            {
                let s = spell.entry(c.player_id).or_insert((0, 0));
                s.0 += 1;
                s.1 = s.1.max(s.0);
                *stalled.entry(c.player_id).or_insert(0) += 1;
                *where_stalled
                    .entry((c.track_progress / 50.0) as i32)
                    .or_insert(0) += 1;
            } else if let Some(s) = spell.get_mut(&c.player_id) {
                s.0 = 0;
            }
        }
    }
    let secs = |t: u32| t as f32 / TICK_RATE as f32;
    let (mut total_stall, mut out, mut cooked) = (0u32, 0, 0);
    for c in race.session.participants.values() {
        let s = stalled.get(&c.player_id).copied().unwrap_or(0);
        total_stall += s;
        let gone = !c.damage.is_drivable;
        out += gone as u32;
        let engine_out =
            gone && c.damage.engine_damage_percent >= 99.0 && !hit.contains_key(&c.player_id);
        cooked += engine_out as u32;
        println!(
            "  car {:2}: lap {:2} stalled {:6.1} s (longest {:5.1} s), hottest {:5.1} C, engine damage {:5.1}%, out {gone}{}",
            c.grid_position,
            c.current_lap,
            secs(s),
            secs(spell.get(&c.player_id).map_or(0, |s| s.1)),
            hottest[&c.player_id],
            c.damage.engine_damage_percent,
            if engine_out { " (engine, no hit)" } else { "" }
        );
    }
    let mut spots: Vec<_> = where_stalled.into_iter().collect();
    spots.sort_by_key(|s| std::cmp::Reverse(s.1));
    for (bucket, t) in spots.into_iter().take(4) {
        println!("    stalled {:6.1} car-s around {} m", secs(t), bucket * 50);
    }
    println!(
        "    coolant gained by speed (<1, <3, <10, <25, faster m/s): {:?}",
        heat_by_speed.map(|v| v.round())
    );
    println!(
        "{track} {car} snow: stalled {:.1} car-s, out {out}, engines cooked without a hit {cooked}",
        secs(total_stall)
    );
}

/// One AI alone (`SURVEY_AI_LEVEL`, an ace unless set) for `LONE_LAPS` (3)
/// laps of each `SURVEY_TRACKS` circuit in each `SURVEY_CARS` car: per lap
/// the time, the seconds off the road and sliding, and every corner (the
/// slowest point of a braking run of the car's plan) where it slid or left
/// the road, with the speed it planned there (times its pace) and the
/// slowest it went. Where a car cannot hold its own plan.
/// `SURVEY_CARS=AcFriendsFerrari499pLmh SURVEY_TRACKS=Spa cargo test --release --test ai_race_start_test lone_car_against_its_plan -- --ignored --nocapture`
#[test]
#[ignore]
fn lone_car_against_its_plan() {
    use apexsim_server::racing_line::{self, LinePhase};
    if std::env::var("SURVEY_AI_LEVEL").is_err() {
        // SAFETY: a test of its own process, before anything reads it.
        unsafe { std::env::set_var("SURVEY_AI_LEVEL", "110") };
    }
    let laps: u16 = std::env::var("LONE_LAPS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(3);
    let tracks = std::env::var("SURVEY_TRACKS").unwrap_or_else(|_| "Spa".into());
    let cars = std::env::var("SURVEY_CARS").unwrap_or_else(|_| "posh-gt3rs".into());
    let (mut all_off, mut all_slide) = (0.0f32, 0.0f32);
    for track in tracks.split(',').map(str::trim) {
        for car in cars.split(',').map(str::trim) {
            let mut race = ai_race(track, car, 1);
            let id = *race.session.participants.keys().next().unwrap();
            let config = race.car_configs.values().next().expect("the car").clone();
            let fuel = race.session.participants[&id].fuel_liters;
            let plan = racing_line::build_laden(&race.track_config, &config, fuel).unwrap();
            let skill = race.ai_profiles[&id].skill_level;
            let pace = apexsim_server::ai_driver::profile_pace(skill);
            let n = plan.points.len();
            // `LONE_PLAN=900,1000`: the plan between those metres, with the
            // line's radius (a 7.5 m half chord) and the lock it takes.
            if let Ok(span) = std::env::var("LONE_PLAN") {
                let v: Vec<f32> = span.split(',').filter_map(|x| x.parse().ok()).collect();
                let half = (7.5 / plan.spacing_m).round().max(1.0) as usize;
                for i in (0..n).filter(|i| {
                    let m = *i as f32 * plan.spacing_m;
                    v.len() == 2 && m >= v[0] && m <= v[1]
                }) {
                    let a = plan.points[(i + n - half) % n];
                    let b = plan.points[i];
                    let c = plan.points[(i + half) % n];
                    let ab = (b[0] - a[0]).hypot(b[1] - a[1]);
                    let bc = (c[0] - b[0]).hypot(c[1] - b[1]);
                    let ca = (a[0] - c[0]).hypot(a[1] - c[1]);
                    let cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                    let kappa = 2.0 * cross.abs() / (ab * bc * ca).max(1e-6);
                    println!(
                        "    plan {:6.0} m {:5.1} m/s radius {:6.1} m lock share {:.2} {:?}",
                        i as f32 * plan.spacing_m,
                        plan.speed_mps[i] * pace,
                        1.0 / kappa.max(1e-4),
                        config.wheelbase_m * kappa / config.max_steering_angle_rad,
                        plan.phases[i]
                    );
                }
            }
            // The corners: the slowest point of each braking run and what
            // follows it up to the next throttle.
            let mut corners = Vec::new();
            let mut i = 0;
            while i < n {
                if plan.phases[i] == LinePhase::Brake {
                    let mut j = i;
                    while j < n && plan.phases[j] != LinePhase::Throttle {
                        j += 1;
                    }
                    let slowest = (i..j.min(n))
                        .min_by(|a, b| plan.speed_mps[*a].total_cmp(&plan.speed_mps[*b]))
                        .unwrap_or(i);
                    corners.push(slowest);
                    i = j;
                } else {
                    i += 1;
                }
            }
            let nearest = |x: f32, y: f32| -> usize {
                (0..n)
                    .min_by(|a, b| {
                        let da = (plan.points[*a][0] - x).hypot(plan.points[*a][1] - y);
                        let db = (plan.points[*b][0] - x).hypot(plan.points[*b][1] - y);
                        da.total_cmp(&db)
                    })
                    .unwrap_or(0)
            };
            // Per plan point this lap: the car's slowest speed, worst slip,
            // and whether it was off the road there.
            let mut seen: Vec<Option<(f32, f32, bool)>> = vec![None; n];
            let (mut lap_off, mut lap_slide, mut lap_start) = (0u32, 0u32, 0u32);
            let mut lap = race.session.participants[&id].current_lap;
            let mut done = 0;
            let mut was = HashMap::new();
            for t in 1..=(TICK_RATE as u32 * 60 * 5 * laps as u32) {
                tick(&mut race);
                if std::env::var("SURVEY_EVENTS").is_ok() {
                    log_events(&race, t, &mut was);
                }
                let c = &race.session.participants[&id];
                if c.current_lap != lap {
                    if lap >= 1 {
                        let reach = (60.0 / plan.spacing_m) as i64;
                        let mut bad = Vec::new();
                        for &k in &corners {
                            let mut worst = (0.0f32, false, f32::INFINITY);
                            for d in -reach..=reach {
                                let p = (k as i64 + d).rem_euclid(n as i64) as usize;
                                if let Some((v, slip, off)) = seen[p] {
                                    worst.0 = worst.0.max(slip);
                                    worst.1 |= off;
                                    worst.2 = worst.2.min(v);
                                }
                            }
                            if worst.0 > 0.10 || worst.1 {
                                bad.push(format!(
                                    "{:.0} m plan {:.1} had {:.1} slip {:.2}{}",
                                    k as f32 * plan.spacing_m,
                                    plan.speed_mps[k] * pace,
                                    worst.2,
                                    worst.0,
                                    if worst.1 { " OFF" } else { "" }
                                ));
                            }
                        }
                        println!(
                            "{track:>12} {car:>24} lap {lap}: {:6.2} s, off {:4.1} s, sliding {:4.1} s{}{}",
                            (t - lap_start) as f32 / TICK_RATE as f32,
                            lap_off as f32 / TICK_RATE as f32,
                            lap_slide as f32 / TICK_RATE as f32,
                            if bad.is_empty() { "" } else { "\n      " },
                            bad.join("\n      ")
                        );
                        all_off += lap_off as f32 / TICK_RATE as f32;
                        all_slide += lap_slide as f32 / TICK_RATE as f32;
                        done += 1;
                    }
                    lap = c.current_lap;
                    lap_start = t;
                    lap_off = 0;
                    lap_slide = 0;
                    seen.iter_mut().for_each(|s| *s = None);
                    if done >= laps {
                        break;
                    }
                }
                let slip = body_slip(c).abs();
                lap_off += !c.is_on_track as u32;
                lap_slide += (c.is_on_track && slip > 0.10) as u32;
                let p = nearest(c.pos_x, c.pos_y);
                let s = seen[p].get_or_insert((c.speed_mps, 0.0, false));
                s.0 = s.0.min(c.speed_mps);
                s.1 = s.1.max(slip);
                s.2 |= !c.is_on_track;
                if !c.damage.is_drivable {
                    println!("{track:>12} {car:>24} retired on lap {lap}");
                    break;
                }
            }
        }
    }
    println!("lone total: off {all_off:.1} s, sliding {all_slide:.1} s");
}
