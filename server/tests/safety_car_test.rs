//! The virtual safety car (`race_control::Course`): a car stranded on the
//! road neutralises the race, every car is held to its pace, and it ends
//! once the road is clear.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::generate_ai_profiles;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::race_control::{Course, VSC_SPEED_MPS};
use apexsim_server::race_start::RacePhase;
use apexsim_server::track_loader::TrackLoader;

const TICK_RATE: u16 = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

fn race(rules: RaceRules) -> GameSession {
    let track =
        TrackLoader::load_from_file(repo("content/tracks/default/Norisring/Norisring.yaml"))
            .expect("track loads");
    let car = CarLoader::load_from_file(&repo("content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let mut profiles = generate_ai_profiles(4, Some(95));
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = uuid::Uuid::from_u128(0x1000 + i as u128);
    }
    let session = RaceSession::new(
        uuid::Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        4,
        4,
        10,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    race.set_race_rules(rules);
    race.set_game_mode(GameMode::Race);
    race
}

fn tick(race: &mut GameSession) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .keys()
        .map(|id| (*id, race.generate_ai_input(id)))
        .collect();
    race.tick(&inputs);
}

/// Race 20 s, then one car breaks down where it is.
fn break_one_down(race: &mut GameSession) -> PlayerId {
    for _ in 0..(TICK_RATE as u32 * 20) {
        tick(race);
    }
    let id = *race.session.participants.keys().last().unwrap();
    race.session
        .participants
        .get_mut(&id)
        .unwrap()
        .damage
        .is_drivable = false;
    id
}

#[test]
fn a_stranded_car_brings_out_the_virtual_safety_car() {
    let mut race = race(RaceRules {
        safety_car: SafetyCar::Virtual,
        ..Default::default()
    });
    let broken = break_one_down(&mut race);
    let mut out_at = None;
    let mut ended_at = None;
    let mut fastest_held = 0.0f32;
    for t in 0..(TICK_RATE as u32 * 90) {
        tick(&mut race);
        match race.race_control.course {
            Course::Vsc => {
                out_at.get_or_insert(t);
                assert_eq!(race.race_phase(), Some(RacePhase::Vsc as u8));
                // Some seconds after it came out the field is at its pace.
                if t > out_at.unwrap() + TICK_RATE as u32 * 15 {
                    for s in race.session.participants.values() {
                        if s.player_id != broken && !s.pit.in_lane {
                            fastest_held = fastest_held.max(s.speed_mps);
                        }
                    }
                }
            }
            Course::VscEnding => {
                assert_eq!(race.race_phase(), Some(RacePhase::VscEnding as u8));
            }
            Course::Green => {
                if out_at.is_some() {
                    ended_at = Some(t);
                    break;
                }
            }
        }
    }
    assert!(out_at.is_some(), "the VSC came out");
    assert!(ended_at.is_some(), "and went in once the car was towed");
    assert!(
        fastest_held <= VSC_SPEED_MPS + 1.0,
        "held to the pace: {fastest_held:.1} m/s"
    );
    assert!(race
        .session
        .participants
        .values()
        .all(|s| s.speed_cap_mps == 0.0));
    assert_eq!(race.race_phase(), None);
}

#[test]
fn without_the_rule_a_stranded_car_brings_out_yellows_only() {
    let mut race = race(RaceRules::default());
    break_one_down(&mut race);
    for _ in 0..(TICK_RATE as u32 * 30) {
        tick(&mut race);
        assert_eq!(race.race_control.course, Course::Green);
    }
}
