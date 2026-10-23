//! How a race gets away (`race_start`): a formation lap back to the grid,
//! a rolling start's green at the line, and a false start.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_sim::ai_driver::generate_ai_profiles;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::*;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::race_start;
use apexsim_sim::track_loader::TrackLoader;

const TICK_RATE: u16 = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(path)
}

/// `ai_count` AI cars and `humans` drivers' cars at `track`, lined up and
/// counting one second into a race under `rules`.
fn grid(track: &str, ai_count: u8, humans: u8, rules: RaceRules) -> (GameSession, Vec<PlayerId>) {
    let track = TrackLoader::load_from_file(repo(&format!(
        "../apexsim-content/tracks/default/{track}/{track}.yaml"
    )))
    .expect("track loads");
    let car =
        CarLoader::load_from_file(&repo("../apexsim-content/cars/default/posh-gt3rs/car.toml"))
            .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let mut profiles = generate_ai_profiles(ai_count, Some(95));
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = uuid::Uuid::from_u128(0x1000 + i as u128);
    }
    let session = RaceSession::new(
        uuid::Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        ai_count + humans,
        ai_count,
        3,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    let humans: Vec<PlayerId> = (0..humans)
        .map(|i| {
            let id = uuid::Uuid::from_u128(0x9000 + i as u128);
            race.add_player(id, car_id).expect("seated");
            id
        })
        .collect();
    race.set_race_rules(rules);
    race.start_countdown_mode(1, GameMode::Race);
    (race, humans)
}

/// One tick: the AI's input made as the game loop makes it, the humans'
/// from `human`.
fn tick(race: &mut GameSession, human: impl Fn(&CarState) -> PlayerInputData) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .values()
        .map(|s| {
            let input = if race.is_ai_player(&s.player_id) {
                race.generate_ai_input(&s.player_id)
            } else {
                human(s)
            };
            (s.player_id, input)
        })
        .collect();
    race.tick(&inputs);
}

/// The rows of a rolling start's field that are formed up at the green on
/// the Norisring (of eight, the circuits checked by hand form one at Spa,
/// its chicane just before the line, to seven at Zandvoort).
const FORMED_ROWS: usize = 4;

fn slot_of(race: &GameSession, state: &CarState) -> GridSlot {
    race.track_config
        .start_positions
        .iter()
        .find(|s| s.position == state.grid_position)
        .expect("a slot")
        .clone()
}

/// A standing start with a formation lap: the field drives a lap at the
/// formation pace with no pass, comes back to stop on its own slots, and
/// the lights count down from there; nothing is timed on the lap, and the
/// lap warms tyres fitted cold.
#[test]
fn a_formation_lap_brings_the_field_back_to_the_grid() {
    let rules = RaceRules {
        formation_lap: true,
        ..Default::default()
    };
    let (mut race, _) = grid("Snorisring", 6, 0, rules);
    let start_temp: f32 = race
        .session
        .participants
        .values()
        .map(|s| s.tires.front_left.temperature_c)
        .sum::<f32>()
        / 6.0;
    let mut formation_ticks = 0u32;
    let mut top_speed = 0.0f32;
    let mut contact = 0u32;
    let mut attacks = 0u32;
    let mut lights_seen = false;
    for _ in 0..(TICK_RATE as u32 * 240) {
        tick(&mut race, |_| PlayerInputData::default());
        if race.race_phase().is_some() {
            formation_ticks += 1;
            assert_eq!(race.session.state, SessionState::Countdown);
            assert_eq!(race.session.countdown_ticks_remaining, None);
            for s in race.session.participants.values() {
                top_speed = top_speed.max(s.speed_mps);
                contact += s.is_colliding as u32;
            }
        } else if formation_ticks > 0
            && race.session.state == SessionState::Countdown
            && !lights_seen
        {
            lights_seen = true;
            let ticks = race.session.countdown_ticks_remaining.unwrap() as u32;
            assert_eq!(ticks, race_start::LIGHTS_SECONDS as u32 * TICK_RATE as u32);
            // Every car stands on its slot, lap 0, nothing timed.
            for s in race.session.participants.values() {
                let slot = slot_of(&race, s);
                let off = ((s.pos_x - slot.x).powi(2) + (s.pos_y - slot.y).powi(2)).sqrt();
                assert!(
                    off < 0.01,
                    "car {} {off:.2} m off its slot",
                    s.grid_position
                );
                assert!(
                    s.speed_mps < 0.05,
                    "car {} at {} m/s",
                    s.grid_position,
                    s.speed_mps
                );
                assert_eq!(s.current_lap, 0);
                assert_eq!(s.speed_cap_mps, 0.0);
                assert!(s.best_lap_time_ms.is_none());
            }
            let warm: f32 = race
                .session
                .participants
                .values()
                .map(|s| s.tires.front_left.temperature_c)
                .sum::<f32>()
                / 6.0;
            println!(
                "formation lap: {:.0} s, tyres {start_temp:.1} -> {warm:.1} °C",
                formation_ticks as f32 / TICK_RATE as f32
            );
            // The grid went out on garage-cold tyres, and the lap (its
            // weave on the straights) warmed them.
            let air = race.track_config.track_surface.air_temperature_c;
            assert!(
                (start_temp - air).abs() < 0.5,
                "fitted at {start_temp:.1} °C, air {air:.1}"
            );
            assert!(warm > start_temp + 12.0, "{start_temp:.1} -> {warm:.1} °C");
        }
        if race.session.state == SessionState::Racing {
            break;
        }
        // No pass is begun on the formation lap.
        if race.race_phase().is_some() {
            attacks += race
                .session
                .participants
                .values()
                .filter(|s| format!("{:?}", s.racecraft.tactic) == "Attack")
                .count() as u32;
        }
    }
    assert!(formation_ticks > 0, "no formation lap");
    assert!(lights_seen, "never back on the grid");
    assert_eq!(race.session.state, SessionState::Racing, "the race began");
    assert!(
        top_speed <= race_start::FORMATION_SPEED_MPS + 1.0,
        "{top_speed:.1} m/s on the formation lap"
    );
    assert_eq!(contact, 0, "car-ticks of contact on the formation lap");
    assert_eq!(attacks, 0, "car-ticks spent attacking on the formation lap");
}

/// A rolling start: the pace lap, then green as the leader comes back to
/// the line, the field already moving and formed up two by two behind it,
/// with no contact; pole starts lap 1 on the green.
#[test]
fn a_rolling_start_goes_green_at_the_line() {
    let rules = RaceRules {
        start: StartType::Rolling,
        ..Default::default()
    };
    let (mut race, _) = grid("Snorisring", 12, 0, rules);
    let mut saw_pace_lap = false;
    let mut contact = 0u32;
    for _ in 0..(TICK_RATE as u32 * 200) {
        tick(&mut race, |_| PlayerInputData::default());
        if race.race_phase() == Some(race_start::RacePhase::PaceLap as u8) {
            saw_pace_lap = true;
            contact += race
                .session
                .participants
                .values()
                .filter(|s| s.is_colliding)
                .count() as u32;
        }
        if race.session.state == SessionState::Racing {
            break;
        }
    }
    assert!(saw_pace_lap);
    assert_eq!(race.session.state, SessionState::Racing);
    assert_eq!(contact, 0, "car-ticks of contact on the pace lap");
    // Two by two: the front rows roll on their slots' sides of the road,
    // each car its slot's distance behind the car ahead as the grid stands,
    // spread by `ROLLING_ROW_SPREAD` (the back of a field may still be
    // closing up at the green).
    let mut field: Vec<&CarState> = race.session.participants.values().collect();
    field.sort_by_key(|s| s.grid_position);
    let mut ahead: Option<(f32, GridSlot)> = None;
    for s in field.into_iter().take(FORMED_ROWS * 2) {
        let slot = slot_of(&race, s);
        let (heading_x, heading_y) = (slot.yaw_rad.cos(), slot.yaw_rad.sin());
        let (dx, dy) = (s.pos_x - slot.x, s.pos_y - slot.y);
        let (along, across) = (
            dx * heading_x + dy * heading_y,
            -dx * heading_y + dy * heading_x,
        );
        println!(
            "car {}: {along:+.1} m along, {across:+.1} m across its slot",
            s.grid_position
        );
        if let Some((front, front_slot)) = ahead {
            let spacing = (front_slot.x - slot.x) * heading_x + (front_slot.y - slot.y) * heading_y;
            let off = along - front + (race_start::ROLLING_ROW_SPREAD - 1.0) * spacing;
            assert!(
                off.abs() < 2.5,
                "car {} {off:+.1} m off its place behind the car ahead",
                s.grid_position
            );
        }
        assert!(
            across.abs() < 0.8,
            "car {} {across:+.1} m across its slot",
            s.grid_position
        );
        ahead = Some((along, slot));
    }
    let slowest = race
        .session
        .participants
        .values()
        .map(|s| s.speed_mps)
        .fold(f32::MAX, f32::min);
    assert!(slowest > 10.0, "the field rolls: slowest {slowest:.1} m/s");
    assert!(race
        .session
        .participants
        .values()
        .all(|s| s.current_lap <= 1 && s.best_lap_time_ms.is_none()));
    // Pole is on lap 1 within a second of the green, at the line.
    for _ in 0..TICK_RATE {
        tick(&mut race, |_| PlayerInputData::default());
    }
    let pole = race
        .session
        .participants
        .values()
        .min_by_key(|s| s.grid_position)
        .unwrap();
    assert_eq!(pole.current_lap, 1, "pole's lap starts at the line");
    assert!(race
        .session
        .participants
        .values()
        .all(|s| s.speed_cap_mps == 0.0));
}

/// The false-start rule: a driver in gear on the throttle is let go of
/// through the countdown, and moving off the slot is a jump start; one
/// waiting on the brake is not.
#[test]
fn a_car_off_its_slot_before_the_lights_jumped_the_start() {
    let rules = RaceRules {
        jump_start: true,
        ..Default::default()
    };
    let (mut race, humans) = grid("Snorisring", 2, 2, rules);
    race.start_countdown_mode(3, GameMode::Race);
    let (eager, patient) = (humans[0], humans[1]);
    for _ in 0..(TICK_RATE as u32 * 4) {
        tick(&mut race, |s| {
            if s.player_id == eager {
                PlayerInputData {
                    throttle: 0.4,
                    gear: Some(1),
                    clutch: Some(1.0),
                    ..Default::default()
                }
            } else {
                PlayerInputData {
                    brake: 1.0,
                    gear: Some(1),
                    ..Default::default()
                }
            }
        });
        if race.session.state == SessionState::Racing {
            break;
        }
    }
    assert_eq!(race.session.state, SessionState::Racing);
    assert!(
        race.race_start.jumped.contains(&eager),
        "the eager car jumped"
    );
    assert!(!race.race_start.jumped.contains(&patient));
    assert!(
        race.session
            .participants
            .keys()
            .filter(|id| race.is_ai_player(id))
            .all(|id| !race.race_start.jumped.contains(id)),
        "the AI waits for the lights"
    );
}

/// A car let go of before the lights meets the held field: the eager car
/// in slot 3, flat out through a long countdown, runs into the car held in
/// slot 1 ahead of it and does not drive through it.
#[test]
fn a_jumped_car_hits_the_held_car_ahead() {
    let rules = RaceRules {
        jump_start: true,
        ..Default::default()
    };
    let (mut race, humans) = grid("Snorisring", 2, 1, rules);
    race.start_countdown_mode(10, GameMode::Race);
    let eager = humans[0];
    let ahead = *race
        .session
        .participants
        .values()
        .find(|s| s.grid_position == 1)
        .map(|s| &s.player_id)
        .expect("a car on pole");
    assert_eq!(race.session.participants[&eager].grid_position, 3);
    let mut hit = false;
    while race.session.state != SessionState::Racing {
        tick(&mut race, |s| {
            if s.player_id == eager {
                PlayerInputData {
                    throttle: 0.6,
                    gear: Some(1),
                    clutch: Some(1.0),
                    ..Default::default()
                }
            } else {
                PlayerInputData {
                    brake: 1.0,
                    gear: Some(1),
                    ..Default::default()
                }
            }
        });
        hit |= race.session.participants[&eager].is_colliding;
        // Along the pole car's heading the eager car stays behind it.
        let (e, a) = (
            &race.session.participants[&eager],
            &race.session.participants[&ahead],
        );
        let ahead_m = (e.pos_x - a.pos_x) * a.yaw_rad.cos() + (e.pos_y - a.pos_y) * a.yaw_rad.sin();
        assert!(
            ahead_m < 0.0,
            "drove through the car ahead: {ahead_m} m past it"
        );
    }
    assert!(race.race_start.jumped.contains(&eager));
    assert!(hit, "the jumped car reached the car ahead");
}

/// Without the rule the grid is held as ever: nobody moves before the
/// lights, whatever the pedals say.
#[test]
fn without_the_rule_a_car_is_held_on_its_slot() {
    let (mut race, humans) = grid("Snorisring", 0, 1, RaceRules::default());
    race.start_countdown_mode(2, GameMode::Race);
    let id = humans[0];
    let slot = slot_of(&race, &race.session.participants[&id]);
    while race.session.state != SessionState::Racing {
        tick(&mut race, |_| PlayerInputData {
            throttle: 1.0,
            gear: Some(1),
            ..Default::default()
        });
    }
    let s = &race.session.participants[&id];
    assert!(((s.pos_x - slot.x).powi(2) + (s.pos_y - slot.y).powi(2)).sqrt() < 0.01);
    assert!(race.race_start.jumped.is_empty());
}
