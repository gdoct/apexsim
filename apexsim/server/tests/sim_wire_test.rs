//! Simulation tests that read the result through the wire: the sky the
//! client is sent (`network::SkyNow`), a car's lap flags
//! (`network::lap_flags_of`) and the session's telemetry and sky
//! (`session_wire::SessionWire`). The simulation crate (`apexsim-sim`) does
//! not know the wire, so these live with the server.

use apexsim_server::conditions::{LiveConditions, WeatherChange};
use apexsim_server::data::*;
use apexsim_server::game_session::{GameSession, DEFAULT_TICK_RATE_HZ};
use apexsim_server::headlights::{update, LAP_FLAG_HEADLIGHTS, LAP_FLAG_HEADLIGHT_FLASH};
use apexsim_server::physics;
use std::collections::{BTreeMap, HashMap};
use uuid::Uuid;

// --- conditions ------------------------------------------------------------

fn picked(weather: Weather, hh: u16, scale: u8, changeable: u8) -> SessionConditions {
    SessionConditions {
        weather,
        time_of_day_minutes: hh * 60,
        time_scale: Some(scale),
        changeable: Some(changeable),
        ..SessionConditions::DEFAULT
    }
    .resolve(7)
}

#[test]
fn the_sky_ahead_names_the_next_three_changes_with_their_etas() {
    let mut live = LiveConditions::new(picked(Weather::Sunny, 13, 1, 2), 50.0, 0.0, 3);
    live.forecast = vec![
        WeatherChange {
            at_s: 600.0,
            weather: Weather::Cloudy,
        },
        WeatherChange {
            at_s: 1800.0,
            weather: Weather::LightRain,
        },
        WeatherChange {
            at_s: 2400.0,
            weather: Weather::HeavyRain,
        },
        WeatherChange {
            at_s: 4000.0,
            weather: Weather::Overcast,
        },
    ];
    let sky = apexsim_server::network::SkyNow::of(&live, [0.0, 0.0], 100.0);
    assert_eq!(
        (sky.next_weather, sky.next_in_s),
        (Weather::Cloudy as u8, 500)
    );
    assert_eq!(
        (sky.next2_weather, sky.next2_in_s),
        (Weather::LightRain as u8, 1700)
    );
    assert_eq!(
        (sky.next3_weather, sky.next3_in_s),
        (Weather::HeavyRain as u8, 2300)
    );
    // Past the third: only the last change is left, and none after it.
    let late = apexsim_server::network::SkyNow::of(&live, [0.0, 0.0], 2500.0);
    assert_eq!(
        (late.next_weather, late.next_in_s),
        (Weather::Overcast as u8, 1500)
    );
    assert_eq!((late.next2_in_s, late.next3_in_s), (0, 0));
}

// --- headlights ------------------------------------------------------------

fn field() -> (BTreeMap<PlayerId, CarState>, PlayerId) {
    let id = Uuid::new_v4();
    let slot = GridSlot {
        position: 1,
        x: 0.0,
        y: 0.0,
        z: 0.0,
        yaw_rad: 0.0,
    };
    let mut field = BTreeMap::new();
    field.insert(id, CarState::new(id, Uuid::new_v4(), &slot));
    (field, id)
}

fn at(weather: Weather, hh: u16, mm: u16) -> SessionConditions {
    SessionConditions {
        weather,
        time_of_day_minutes: hh * 60 + mm,
        ..SessionConditions::DEFAULT
    }
}

#[test]
fn the_switch_beats_the_sky_and_the_flash_is_held() {
    let (mut cars, id) = field();
    let mut inputs = HashMap::new();
    inputs.insert(
        id,
        PlayerInputData {
            headlights: Some(false),
            ..Default::default()
        },
    );
    update(
        &mut cars,
        &inputs,
        at(Weather::HeavyRain, 23, 0).headlights_needed(),
    );
    assert!(!cars[&id].headlights, "switched off at night");

    inputs.get_mut(&id).unwrap().headlights = Some(true);
    update(
        &mut cars,
        &inputs,
        SessionConditions::DEFAULT.headlights_needed(),
    );
    assert!(cars[&id].headlights, "switched on by day");

    let input = inputs.get_mut(&id).unwrap();
    input.headlights = None;
    input.flash = true;
    update(
        &mut cars,
        &inputs,
        SessionConditions::DEFAULT.headlights_needed(),
    );
    assert!(!cars[&id].headlights);
    assert!(cars[&id].headlight_flash);
    let flags = apexsim_server::network::lap_flags_of(&cars[&id]);
    assert_eq!(
        flags & (LAP_FLAG_HEADLIGHTS | LAP_FLAG_HEADLIGHT_FLASH),
        LAP_FLAG_HEADLIGHT_FLASH
    );

    inputs.get_mut(&id).unwrap().flash = false;
    update(
        &mut cars,
        &inputs,
        SessionConditions::DEFAULT.headlights_needed(),
    );
    assert!(!cars[&id].headlight_flash, "let go, the flash ends");
}

// --- the session -----------------------------------------------------------

fn create_test_session() -> GameSession {
    let track = TrackConfig::default();
    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 0, 3);

    GameSession::new(session, track, car_configs)
}

/// A weekend of `n` humans (ids 1..=n, seated in that order, named
/// A, B, C...): `practice` s of practice (0 for none), then rounds of
/// `rounds` s each.
fn weekend_of(n: u128, practice: u32, rounds: &[u32]) -> (GameSession, Vec<PlayerId>) {
    let mut game_session = create_test_session();
    game_session.set_weekend(Some(apexsim_server::weekend::WeekendFormat {
        practice_seconds: practice,
        qualifying_seconds: rounds.to_vec(),
        practice_conditions: None,
        qualifying_conditions: None,
        ..Default::default()
    }));
    let car_id = game_session.car_configs.values().next().unwrap().id;
    let ids: Vec<PlayerId> = (1..=n).map(Uuid::from_u128).collect();
    for (i, id) in ids.iter().enumerate() {
        let name = ((b'A' + i as u8) as char).to_string();
        game_session.set_driver_name(*id, &name);
        assert!(game_session.add_player(*id, car_id).is_some());
    }
    game_session.session.host_player_id = ids[0];
    (game_session, ids)
}

/// Run the clock of the timed phase out with every car in its garage:
/// the flag is out and the phase is over on that tick.
fn run_out_the_clock(game_session: &mut GameSession) {
    let length = game_session.timed_length_ticks().expect("a timed phase") as u32;
    game_session.session.current_tick = game_session.timed_start_tick.unwrap() + length - 1;
    game_session.tick(&HashMap::new());
    assert!(game_session.chequered_flag_out());
}

/// The phase's laps are in and the round is over, as the game loop
/// leaves it after the tick.
fn end_phase(game_session: &mut GameSession) {
    run_out_the_clock(game_session);
    assert_eq!(game_session.session.state, SessionState::Finished);
    game_session.settle_weekend();
}

/// Two humans racing on the default oval, lap limit 3.
/// A car stopped on the road brings out a double yellow over its
/// marshal sector and a single over the one before: the car arriving
/// behind is told, telemetry carries both, and the flags go once the
/// stopped car has gone and the hold has run out.
#[test]
fn a_car_stopped_on_the_road_brings_out_the_yellows() {
    // The wire's view of the session (runtime side).
    use apexsim_server::session_wire::SessionWire;
    let track = apexsim_server::track_loader::TrackLoader::load_from_file(
        "../../apexsim-content/tracks/default/Monza/Monza.yaml",
    )
    .expect("Monza");
    let car = CarConfig::default();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car_id, car);
    let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Multiplayer, 8, 0, 3);
    let mut gs = GameSession::new(session, track, car_configs);
    let (arriving, stopped) = (Uuid::from_u128(1), Uuid::from_u128(2));
    gs.add_player(arriving, car_id).unwrap();
    gs.add_player(stopped, car_id).unwrap();
    gs.set_game_mode(GameMode::Race);
    gs.session.state = SessionState::Racing;
    gs.session.race_start_tick = Some(0);
    gs.session.current_tick = 60 * DEFAULT_TICK_RATE_HZ as u32;
    let place = |gs: &mut GameSession, id: PlayerId, station: f32, speed: f32| {
        let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, station);
        let s = gs.session.participants.get_mut(&id).unwrap();
        s.pos_x = x;
        s.pos_y = y;
        s.pos_z = z;
        s.yaw_rad = yaw;
        s.vel_x = speed * yaw.cos();
        s.vel_y = speed * yaw.sin();
        s.speed_mps = speed;
        s.gear = 4;
        s.current_lap = 2;
        s.nearest_centerline_idx = None;
        physics::seed_track_progress(s, &gs.track_config);
    };
    place(&mut gs, stopped, 1125.0, 0.0);
    place(&mut gs, arriving, 850.0, 50.0);
    // A stop is an incident once it has lasted `INCIDENT_CONFIRM_S`.
    let confirm = (apexsim_server::race_control::INCIDENT_CONFIRM_S * DEFAULT_TICK_RATE_HZ as f32)
        .ceil() as usize;
    for _ in 0..confirm - 1 {
        gs.tick(&HashMap::new());
    }
    assert!(gs.yellow_flags().is_none(), "not out at once");
    for _ in 0..2 {
        gs.tick(&HashMap::new());
    }
    let a = &gs.session.participants[&arriving];
    assert_eq!(
        a.flag_yellow, 2,
        "the double ahead, at {:.0} m",
        a.track_progress
    );
    let flags = gs.yellow_flags().expect("yellows out");
    assert_eq!(flags.sector_m, 250);
    assert!(flags.sectors.contains(&[4, 2]), "{:?}", flags.sectors);
    assert!(flags.sectors.contains(&[3, 1]), "{:?}", flags.sectors);
    let telemetry = gs.get_compact_telemetry();
    let row = telemetry
        .car_states
        .iter()
        .find(|c| Some(c.car_index) == gs.car_index_of(&arriving))
        .unwrap();
    assert_eq!(
        row.race_flags >> apexsim_server::network::RACE_FLAG_YELLOW_SHIFT,
        2
    );
    assert!(telemetry.yellow_flags.is_some());

    // The stopped car is taken away (to the pits): the yellows hold, then go.
    gs.session.participants.get_mut(&stopped).unwrap().in_garage = true;
    for _ in 0..((apexsim_server::race_control::YELLOW_HOLD_S + 0.5) * DEFAULT_TICK_RATE_HZ as f32)
        as usize
    {
        place(&mut gs, arriving, 600.0, 50.0);
        gs.tick(&HashMap::new());
    }
    assert!(gs.yellow_flags().is_none(), "cleared");
    assert_eq!(gs.session.participants[&arriving].flag_yellow, 0);
}

/// Each day of a weekend under its own sky: practice in the rain at
/// dawn, qualifying overcast, the race under the session's own sky,
/// each baked from the source track afresh with the sky's clock
/// started at the day. A weekend naming no day's sky keeps its one.
#[test]
fn a_weekend_day_bakes_its_own_sky() {
    // The wire's view of the session (runtime side).
    use apexsim_server::session_wire::SessionWire;
    let (mut game_session, ids) = weekend_of(2, 600, &[600]);
    let dry = game_session.track_config.track_surface.base_grip;
    let source = std::sync::Arc::new(game_session.track_config.clone());
    game_session.set_source_track(source.clone());
    // No day named: practice keeps the session's sky and track.
    game_session.advance_weekend().unwrap();
    assert_eq!(game_session.session.conditions, SessionConditions::DEFAULT);
    assert_eq!(game_session.sky_epoch_tick, 0);
    assert_eq!(game_session.weekend_state().unwrap().conditions, None);
    assert!(!game_session.take_day_lines_due());
    let _ = ids;

    let (mut game_session, ids) = weekend_of(2, 600, &[600]);
    let driver = ids[1];
    game_session.set_source_track(source);
    let base = game_session.session.conditions;
    let rain = SessionConditions {
        weather: Weather::HeavyRain,
        time_of_day_minutes: 7 * 60,
        ..SessionConditions::DEFAULT
    };
    let overcast = SessionConditions {
        weather: Weather::Overcast,
        ..SessionConditions::DEFAULT
    };
    game_session.set_weekend(Some(apexsim_server::weekend::WeekendFormat {
        practice_seconds: 600,
        qualifying_seconds: vec![600],
        practice_conditions: Some(Box::new(rain)),
        qualifying_conditions: Some(Box::new(overcast)),
        ..Default::default()
    }));
    game_session.session.current_tick = 1000;
    game_session.advance_weekend().unwrap();
    assert_eq!(game_session.session.conditions.weather, Weather::HeavyRain);
    assert!(game_session.track_config.track_surface.wet);
    assert!(game_session.track_config.track_surface.base_grip < dry);
    assert_eq!(game_session.sky_epoch_tick, 1000);
    assert_eq!(game_session.sky_now().clock_s, 7 * 3600);
    // Everyone is told the day's sky, and every driver's racing line is
    // planned again under it, once.
    assert_eq!(
        game_session.weekend_state().unwrap().conditions,
        Some(game_session.session.conditions)
    );
    assert!(game_session.take_day_lines_due());
    assert!(!game_session.take_day_lines_due());
    assert!(game_session.refreshed_racing_line(&driver).is_some());
    assert!(game_session.refreshed_racing_line(&driver).is_none());
    end_phase(&mut game_session);

    game_session.advance_weekend().unwrap();
    assert_eq!(game_session.session.conditions.weather, Weather::Overcast);
    assert!(!game_session.track_config.track_surface.wet);
    let overcast_grip = game_session.track_config.track_surface.base_grip;
    assert!(overcast_grip < dry && overcast_grip > 0.9 * dry);
    end_phase(&mut game_session);

    // The race day: the session's own sky, the track as filed.
    game_session.advance_weekend().unwrap();
    assert_eq!(game_session.session.conditions, base);
    assert_eq!(game_session.track_config.track_surface.base_grip, dry);
    assert!(!game_session.track_config.track_surface.wet);
}
