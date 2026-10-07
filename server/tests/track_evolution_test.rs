//! The road and the sky through a session (`road_state.rs`,
//! `conditions.rs`): the line rubbering in under a race, rain arriving from
//! the forecast and the AI coming in for treaded tyres, the clock running
//! into the evening.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::conditions::WeatherChange;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::road_state::{CELL_M, RUBBER_REFERENCE};
use apexsim_server::track_loader::TrackLoader;
use apexsim_server::tyre_thermal::CompoundKind;
use uuid::Uuid;

const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads")
}

/// A race at Monza for `cars` AI GT3s under `conditions`, applied as the
/// server does on create.
fn race(conditions: SessionConditions, cars: usize) -> (GameSession, Vec<PlayerId>) {
    let mut track = monza();
    let mut session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        cars as u8 + 2,
        cars as u8,
        30,
    );
    // A fixed id: the forecast and the wind are laid out from it.
    session.id = Uuid::from_u128(0x5E55_1011);
    let conditions = conditions.resolve(session.id.as_u64_pair().0);
    conditions.apply_to_track(&mut track);
    session.conditions = conditions;
    let car = gt3();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let profiles: Vec<AiDriverProfile> = (0..cars)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("AI {i}"), 100);
            p.id = Uuid::from_u128(9000 + i as u128);
            p.preferred_car_id = Some(car.id);
            p
        })
        .collect();
    let ids = profiles.iter().map(|p| p.id).collect();
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, profiles);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    (gs, ids)
}

fn run(gs: &mut GameSession, ids: &[PlayerId], seconds: usize, mut each: impl FnMut(&GameSession)) {
    for _ in 0..seconds * HZ {
        let inputs: HashMap<PlayerId, PlayerInputData> = ids
            .iter()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        each(gs);
    }
}

/// Four cars for six minutes: the line they drive gains rubber, the road
/// beside it in the corners gathers marbles, and what the AI reads of the
/// road under it rises above the session's start.
#[test]
fn the_line_rubbers_in_and_the_marbles_gather_off_it() {
    let (mut gs, ids) = race(SessionConditions::DEFAULT, 4);
    let before = gs
        .track_config
        .road_state
        .as_ref()
        .unwrap()
        .mean_line_rubber();
    assert!((before - RUBBER_REFERENCE).abs() < 0.02, "{before}");
    run(&mut gs, &ids, 360, |_| {});
    let road = gs.track_config.road_state.as_ref().unwrap();
    let after = road.mean_line_rubber();
    let marbled = road.marbles.iter().filter(|m| **m > 0.005).count();
    let worst = road.marbles.iter().copied().fold(0.0, f32::max);
    println!(
        "Monza, 4 cars, 6 min: line rubber {before:.3} -> {after:.3}; {marbled} bins with marbles, worst {worst:.3}"
    );
    assert!(after > before + 0.005, "the line rubbers in");
    // A dozen passes each: the start of the marbles a race piles up.
    assert!(marbled > 50 && worst > 0.01, "marbles gather");
    // The marbles lie off the line: on it, the wheels sweep them.
    let on_line: f32 = road
        .cells
        .iter()
        .enumerate()
        .map(|(i, c)| {
            road.sample(i as f32 * CELL_M + 5.0, c.line_lateral_m + 0.8)
                .grip
        })
        .sum::<f32>()
        / road.cells.len() as f32;
    assert!(
        on_line > 1.0,
        "the line grips better than at the start: {on_line}"
    );
    // The sky held: the session's track is the start's.
    assert_eq!(gs.sky.weather, Weather::Sunny);
    assert_eq!(gs.track_config.track_surface.water, 0.0);
}

/// The forecast turns a dry race to heavy rain half a minute in: the rain
/// builds over minutes, the road takes the water, the track cools, and the
/// AI comes in for wet tyres.
#[test]
fn rain_from_the_forecast_wets_the_road_and_the_ai_stops_for_wets() {
    let conditions = SessionConditions {
        changeable: Some(1),
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race(conditions, 2);
    if gs.track_config.pit_lane.is_none() {
        return;
    }
    // The forecast laid out from the id would rain at some point; set one
    // that rains now.
    gs.sky.forecast = vec![WeatherChange {
        at_s: 30.0,
        weather: Weather::HeavyRain,
    }];
    let track_c0 = gs.track_config.track_surface.track_temperature_c;
    let mut wets = 0;
    run(&mut gs, &ids, 600, |_| {});
    for id in &ids {
        let s = &gs.session.participants[id];
        let tyre = &gs.car_configs[&s.car_config_id].tire_config;
        if tyre.compound(s.tyre_compound).kind != CompoundKind::Slick {
            wets += 1;
        }
    }
    let surface = &gs.track_config.track_surface;
    println!(
        "Monza, rain from 30 s: after 10 min rain {:.2}, road water {:.2}, track {:.1} -> {:.1} °C, {wets} of {} on treaded tyres",
        gs.sky.rain,
        surface.water,
        track_c0,
        surface.track_temperature_c,
        ids.len()
    );
    assert_eq!(gs.sky.weather, Weather::HeavyRain);
    assert!(gs.sky.rain > 0.9);
    assert!(surface.water > 0.5 && surface.wet);
    assert!(surface.track_temperature_c < track_c0 - 4.0);
    assert!(gs.sky.headlights_needed());
    assert_eq!(wets, ids.len(), "every AI came in for treaded tyres");
}

/// A clock at 60x from 19:00: ten minutes of session is ten hours of day.
/// The sun goes, the lights come on, the air and the asphalt cool, and
/// telemetry says so.
#[test]
fn a_fast_clock_runs_into_the_night() {
    let conditions = SessionConditions {
        time_of_day_minutes: 19 * 60,
        time_scale: Some(60),
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race(conditions, 1);
    let air0 = gs.track_config.track_surface.air_temperature_c;
    let track0 = gs.track_config.track_surface.track_temperature_c;
    run(&mut gs, &ids, 180, |_| {});
    // 19:00 + 3 min x 60 = 22:00.
    let sky = gs
        .get_compact_telemetry()
        .sky
        .expect("every frame carries the sky");
    println!(
        "19:00 -> {:02}:{:02}: air {air0:.1} -> {:.1}, track {track0:.1} -> {:.1}",
        sky.clock_s / 3600,
        sky.clock_s / 60 % 60,
        gs.track_config.track_surface.air_temperature_c,
        gs.track_config.track_surface.track_temperature_c
    );
    assert_eq!(sky.clock_s / 3600, 22);
    assert_eq!(sky.time_scale, 60);
    assert!(gs.sky.headlights_needed());
    let s = &gs.session.participants[&ids[0]];
    assert!(s.headlights, "the AI's lights are on at night");
    assert!(gs.track_config.track_surface.track_temperature_c < track0 - 3.0);
    assert!(gs.track_config.track_surface.air_temperature_c < air0);
}

/// Two sessions with the same id and the same conditions run the same:
/// the forecast, the rain and the road are functions of the session alone.
#[test]
fn a_changing_sky_is_deterministic() {
    let conditions = SessionConditions {
        changeable: Some(3),
        time_scale: Some(30),
        ..SessionConditions::DEFAULT
    };
    let (mut a, ids) = race(conditions, 2);
    let (mut b, _) = race(conditions, 2);
    assert_eq!(a.sky.forecast, b.sky.forecast);
    run(&mut a, &ids, 90, |_| {});
    run(&mut b, &ids, 90, |_| {});
    assert_eq!(a.sky, b.sky);
    assert_eq!(a.track_config.road_state, b.track_config.road_state);
    for id in &ids {
        let (sa, sb) = (&a.session.participants[id], &b.session.participants[id]);
        assert_eq!((sa.pos_x, sa.pos_y), (sb.pos_x, sb.pos_y));
    }
}
