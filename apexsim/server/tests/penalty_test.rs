//! Race control's penalties served in the pit lane (`race_control`): the
//! AI drives through the lane for a drive-through and stops in its box for
//! a stop-go, and neither counts as a stop for the pit rule.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::generate_ai_profiles;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::race_control::{Offence, PenaltyKind, PenaltyStatus};
use apexsim_server::track_loader::TrackLoader;

const TICK_RATE: u16 = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

/// Two AI cars racing ten laps of the Norisring under every penalty rule.
fn race() -> GameSession {
    let track = TrackLoader::load_from_file(repo(
        "../apexsim-content/tracks/default/Norisring/Norisring.yaml",
    ))
    .expect("track loads");
    assert!(track.pit_lane.is_some(), "the Norisring's pit lane sidecar");
    let car =
        CarLoader::load_from_file(&repo("../apexsim-content/cars/default/posh-gt3rs/car.toml"))
            .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let mut profiles = generate_ai_profiles(2, Some(95));
    for (i, profile) in profiles.iter_mut().enumerate() {
        profile.preferred_car_id = Some(car_id);
        profile.id = uuid::Uuid::from_u128(0x1000 + i as u128);
    }
    let session = RaceSession::new(
        uuid::Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        2,
        2,
        10,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    race.set_race_rules(RaceRules {
        jump_start: true,
        yellow_penalty: true,
        pit_exit_penalty: true,
        ..Default::default()
    });
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

/// Race until `id`'s first penalty is no longer owed (or `seconds` pass);
/// whether it stopped in its box on the way.
fn race_until_served(race: &mut GameSession, id: PlayerId, seconds: u32) -> bool {
    let mut stopped = false;
    for _ in 0..(TICK_RATE as u32 * seconds) {
        tick(race);
        stopped |= race.session.participants[&id].pit.servicing;
        if race.penalties[&id][0].status != PenaltyStatus::Owed {
            break;
        }
    }
    stopped
}

#[test]
fn the_ai_drives_through_the_lane_for_a_drive_through() {
    let mut race = race();
    for _ in 0..(TICK_RATE as u32 * 5) {
        tick(&mut race);
    }
    let id = *race.session.participants.keys().next().unwrap();
    race.penalise(id, Offence::JumpStart);
    assert_eq!(race.owed_penalty(&id), Some(PenaltyKind::DriveThrough));
    let stopped = race_until_served(&mut race, id, 300);
    let penalty = race.penalties[&id][0];
    assert_eq!(penalty.status, PenaltyStatus::Served, "{penalty:?}");
    assert!(!stopped, "a drive-through does not stop");
    let state = &race.session.participants[&id];
    assert_eq!(state.pit.stops, 0, "and is no stop for the pit rule");
    assert!(!state.pit.wants_stop);
    assert_eq!(race.penalty_seconds(&id), 0, "served: nothing added");
}

#[test]
fn the_ai_stops_for_a_stop_go_and_no_work_is_done() {
    let mut race = race();
    for _ in 0..(TICK_RATE as u32 * 5) {
        tick(&mut race);
    }
    let id = *race.session.participants.keys().next().unwrap();
    let compound = race.session.participants[&id].tyre_compound;
    race.penalise(id, Offence::PitExitRed);
    assert_eq!(race.owed_penalty(&id), Some(PenaltyKind::StopGo));
    let stopped = race_until_served(&mut race, id, 300);
    let penalty = race.penalties[&id][0];
    assert_eq!(penalty.status, PenaltyStatus::Served, "{penalty:?}");
    assert!(stopped, "a stop-go stops in the box");
    let state = &race.session.participants[&id];
    assert_eq!(state.pit.stops, 0, "no stop for the pit rule");
    assert_eq!(state.tyre_compound, compound);
    assert!(
        state.tires.each().iter().all(|t| t.wear_percent > 0.0),
        "the same tyres"
    );
}

#[test]
fn a_penalty_the_rules_do_not_name_is_not_given() {
    let mut race = race();
    race.set_race_rules(RaceRules::default());
    let id = *race.session.participants.keys().next().unwrap();
    race.penalise(id, Offence::YellowPass);
    assert!(!race.penalties.contains_key(&id));
}
