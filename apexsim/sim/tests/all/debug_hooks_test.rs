//! The debug hooks (`apexsim_sim::debug_hooks`) end to end on Monza:
//! the stand-in drives a human's car, and an event punctures it, which the
//! stand-in pits for like any AI.

use std::collections::HashMap;
use std::path::Path;

use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::*;
use apexsim_sim::debug_hooks::DebugHooks;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

const HZ: usize = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn race_with_events(car_folder: &str, events: &str) -> Option<(GameSession, PlayerId)> {
    let track = TrackLoader::load_from_file(
        "../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .ok()?;
    track.pit_lane.as_ref()?;
    let car = CarLoader::load_from_file(Path::new(&format!(
        "../../apexsim-content/cars/default/{car_folder}/car.toml"
    )))
    .ok()?;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        4,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.add_player(player, car.id).expect("seat");
    gs.set_debug_hooks(DebugHooks::from_settings(true, events));
    gs.start_countdown_mode(1, GameMode::Race);
    Some((gs, player))
}

/// Drive until `done` or `limit_s`; the human's input is whatever a client
/// would send while nobody touches it: nothing.
fn run(gs: &mut GameSession, player: PlayerId, limit_s: f32, done: impl Fn(&CarState) -> bool) {
    for _ in 0..(limit_s * HZ as f32) as usize {
        let input = gs.stand_in_input(&player).unwrap_or_default();
        gs.tick(&[(player, input)].into());
        let s = &gs.session.participants[&player];
        if done(s) {
            return;
        }
    }
}

#[test]
fn the_stand_in_pits_a_punctured_car_and_races_on() {
    let Some((mut gs, player)) = race_with_events("posh-gt3rs", "8:host:puncture=RR") else {
        return;
    };
    run(&mut gs, player, 400.0, |s| {
        s.pit.stops >= 1 && !s.pit.driving
    });
    let s = &gs.session.participants[&player];
    assert_eq!(s.pit.stops, 1, "stopped once (lap {})", s.current_lap);
    assert!(!s.tires.rear_right.punctured, "a new set fitted");
}

#[test]
fn a_full_damage_event_retires_the_car() {
    let Some((mut gs, player)) = race_with_events("posh-gt3rs", "5:host:damage=front:100") else {
        return;
    };
    run(&mut gs, player, 20.0, |s| s.towed);
    let s = &gs.session.participants[&player];
    assert!(!s.damage.is_drivable);
    assert!(s.towed, "towed to its box");
    // Front damage past the AI's threshold, but a towed car is not then
    // planned onto the pit route (the HUD read PIT LANE - AUTOPILOT).
    run(&mut gs, player, 5.0, |_| false);
    let s = &gs.session.participants[&player];
    assert!(
        !s.pit.driving && !s.pit.wants_stop,
        "a towed car is left in its box"
    );
}

#[test]
fn a_boost_event_holds_the_overtake_button() {
    let Some((mut gs, player)) = race_with_events("fugazzi-sf26", "1:host:boost=2") else {
        return;
    };
    // Through the countdown and a second and a half of green.
    run(&mut gs, player, 30.0, |s| s.current_lap >= 1);
    run(&mut gs, player, 1.5, |_| false);
    assert!(gs.stand_in_input(&player).unwrap().ers_boost, "held");
    run(&mut gs, player, 2.0, |_| false);
    assert!(!gs.stand_in_input(&player).unwrap().ers_boost, "let go");
}

/// A heat event cooks the fronts: their grip drops at once and the tyre
/// model carries on from there (cools), rather than snapping back.
#[test]
fn a_heat_event_cooks_the_fronts() {
    let Some((mut gs, player)) = race_with_events("posh-gt3rs", "3:host:heat=front:140") else {
        return;
    };
    let before = std::cell::Cell::new(None);
    run(&mut gs, player, 60.0, |s| {
        if s.tires.front_left.temperature_c >= 130.0 {
            return true;
        }
        before.set(Some(s.tires.front_left.grip_factor));
        false
    });
    let before = before.get();
    let s = &gs.session.participants[&player];
    assert!(s.tires.front_left.core_temperature_c > 130.0, "core heated");
    assert!(s.tires.rear_left.temperature_c < 130.0, "rears untouched");
    run(&mut gs, player, 0.5, |_| false);
    let s = &gs.session.participants[&player];
    let grip = s.tires.front_left.grip_factor;
    assert!(
        grip < before.unwrap_or(1.0) - 0.02,
        "greasy fronts lose grip: {grip} after {before:?}"
    );
    assert!(
        s.tires.front_left.core_temperature_c > 100.0,
        "still hot half a second on"
    );
}

/// The stand-in drives a human's car in qualifying too, with the steering
/// aid their profile has on (as a client's `SetDriverAids` leaves it): a
/// shoot-out's run is driven to a flying lap and the shoot-out ends. With
/// the aid between the AI and the car, the run went into the first wall
/// and stood there until its time ran out.
#[test]
fn the_stand_in_drives_a_qualifying_run() {
    let Ok(track) = TrackLoader::load_from_file(
        "../../apexsim-content/tracks/default/Snorisring/Snorisring.yaml",
    ) else {
        return;
    };
    let car = CarLoader::load_from_file(Path::new(
        "../../apexsim-content/cars/default/limbotiti-caravan-gt3/car.toml",
    ))
    .expect("the Caravan");
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let host = Uuid::from_u128(1);
    let session = RaceSession::new(host, track.id, SessionKind::Multiplayer, 8, 0, 3);
    let mut gs = GameSession::new(session, track, car_configs);
    gs.add_player(host, car_id).expect("seat");
    gs.set_driver_name(host, "Host");
    gs.set_debug_hooks(DebugHooks::from_settings(true, ""));
    gs.session
        .participants
        .get_mut(&host)
        .unwrap()
        .steering_assist = true;
    gs.set_qualifying_format(QualifyingFormat::Shootout);
    gs.set_game_mode(GameMode::Qualification);
    let mut flying = None;
    for _ in 0..(HZ * 300) {
        let input = gs.stand_in_input(&host).unwrap_or_default();
        gs.tick(&[(host, input)].into());
        for lap in gs.take_lap_events() {
            if lap.player_id == host && lap.event.lap_time_ms.is_some() {
                flying = lap.event.lap_time_ms;
            }
        }
        if gs.session.state == SessionState::Finished {
            break;
        }
    }
    let ms = flying.expect("a flying lap driven");
    assert!(ms < 120_000, "a lap at pace: {ms} ms");
    assert_eq!(
        gs.session.state,
        SessionState::Finished,
        "the shoot-out ended"
    );
}
