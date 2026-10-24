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
use apexsim_server::session_wire::SessionWire;
use apexsim_server::track_loader::TrackLoader;
use apexsim_server::tyre_thermal::CompoundKind;
use uuid::Uuid;

const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .expect("Monzarella")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new(
        "../../../apexsim-content/cars/default/posh-gt3rs/car.toml",
    ))
    .expect("car loads")
}

/// A race at Monza for `cars` AI GT3s under `conditions`, applied as the
/// server does on create.
fn race(conditions: SessionConditions, cars: usize) -> (GameSession, Vec<PlayerId>) {
    race_in(conditions, cars, gt3())
}

/// [`race`] in `car`.
fn race_in(
    conditions: SessionConditions,
    cars: usize,
    car: CarConfig,
) -> (GameSession, Vec<PlayerId>) {
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
    if !gs
        .track_config
        .has_sidecars(&[Sidecar::Pit], "a wet stop's tyres")
    {
        return;
    }
    // The forecast laid out from the id would rain at some point; set one
    // that rains now.
    gs.sky.forecast = vec![WeatherChange {
        at_s: 30.0,
        weather: Weather::HeavyRain,
    }];
    let track_c0 = gs.track_config.track_surface.track_temperature_c;
    let speed = |w: [f32; 2]| (w[0] * w[0] + w[1] * w[1]).sqrt() * 3.6;
    let wind0 = speed(gs.track_config.track_surface.wind_mps);
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
    // The front's wind came with it: the sun's 8 km/h breeze to a storm's.
    let wind = speed(surface.wind_mps);
    println!("wind {wind0:.1} -> {wind:.1} km/h");
    assert!(
        (wind0 - 8.0).abs() < 0.01 && wind > 26.0,
        "{wind0} -> {wind}"
    );
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

/// A snow session: the road starts covered and grips as baked, six cars
/// pack a line that grips a third more than the untouched snow beside it,
/// telemetry carries the snow, and a replay of the same session is the same.
#[test]
fn snow_covers_the_road_and_the_field_packs_a_line() {
    let conditions = SessionConditions {
        weather: Weather::Snow,
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race(conditions, 6);
    let surface = &gs.track_config.track_surface;
    assert_eq!(surface.baked_snow, 1.0);
    assert!(surface.wet && surface.air_temperature_c <= 2.0);
    assert!((surface.base_grip - Weather::Snow.road_grip_factor()).abs() < 1e-6);
    let road = gs.track_config.road_state.as_ref().unwrap();
    assert!((road.mean_line_snow() - 1.0).abs() < 1e-6);
    assert!(
        (road.sample(300.0, 0.0).grip - 1.0).abs() < 1e-3,
        "as baked"
    );
    run(&mut gs, &ids, 240, |_| {});
    let road = gs.track_config.road_state.as_ref().unwrap();
    println!(
        "Monza in snow after 4 min: line cover {:.2}, air {:.1} °C",
        road.mean_line_snow(),
        gs.track_config.track_surface.air_temperature_c
    );
    assert!(road.mean_line_snow() < 0.99, "the line is packed");
    assert!(road.mean_line_snow() > apexsim_server::road_state::SNOW_FLOOR - 0.01);
    let sky = gs.get_compact_telemetry().sky.expect("a sky");
    assert_eq!(sky.weather, Weather::Snow as u8);
    assert!(sky.line_snow_pct > 60 && sky.snowfall_pct == 100, "{sky:?}");
    assert_eq!(sky.rain_pct, 0, "snow is not rain");
    assert!(gs.sky.headlights_needed());
    // The same session again is the same.
    let (mut again, ids2) = race(conditions, 6);
    run(&mut again, &ids2, 240, |_| {});
    assert_eq!(gs.track_config.road_state, again.track_config.road_state);
}

/// A winter sky that changes: it starts overcast and cold, the forecast
/// brings snow, the road whitens, and the flakes stop with the weather.
#[test]
fn a_winter_forecast_brings_snow_onto_a_bare_road() {
    let conditions = SessionConditions {
        weather: Weather::Overcast,
        air_temp_c: Some(-3),
        changeable: Some(2),
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race(conditions, 2);
    gs.sky.forecast = vec![
        WeatherChange {
            at_s: 20.0,
            weather: Weather::Snow,
        },
        WeatherChange {
            at_s: 700.0,
            weather: Weather::Overcast,
        },
    ];
    assert_eq!(gs.track_config.road_state.as_ref().unwrap().snowfall, 0.0);
    let grip0 = gs
        .track_config
        .road_state
        .as_ref()
        .unwrap()
        .sample(300.0, 0.0)
        .grip;
    run(&mut gs, &ids, 600, |_| {});
    let road = gs.track_config.road_state.as_ref().unwrap();
    println!(
        "overcast -> snow: snowfall {:.2}, cover on the line {:.2}, grip {grip0:.2} -> {:.2}",
        road.snowfall,
        road.mean_line_snow(),
        road.sample(300.0, 0.0).grip
    );
    assert_eq!(gs.sky.weather, Weather::Snow);
    assert!(road.snowfall > 0.9 && road.has_snow());
    assert!(
        road.sample(300.0, 4.0).grip < grip0 * 0.7,
        "the road whitens"
    );
    assert!(
        gs.sky.air_c < 3.0,
        "the winter air stays cold: {}",
        gs.sky.air_c
    );
}

fn rally() -> CarConfig {
    CarLoader::load_from_file(Path::new(
        "../../../apexsim-content/cars/default/lancha-037-rally/car.toml",
    ))
    .expect("car loads")
}

/// The line's mean snow cover now.
fn line_snow(gs: &GameSession) -> f32 {
    gs.track_config
        .road_state
        .as_ref()
        .unwrap()
        .mean_line_snow()
}

/// Each AI's call for a change of tyres that `wanted` picks out: when it
/// asked (s) and the line's cover then, the first time each asks.
fn calls(
    gs: &mut GameSession,
    ids: &[PlayerId],
    seconds: usize,
    wanted: impl Fn(CompoundKind) -> bool,
) -> HashMap<PlayerId, (f32, f32)> {
    let mut asked = HashMap::new();
    let mut tick = 0usize;
    run(gs, ids, seconds, |gs| {
        tick += 1;
        if !tick.is_multiple_of(HZ) {
            return;
        }
        for id in ids {
            let s = &gs.session.participants[id];
            let tyre = &gs.car_configs[&s.car_config_id].tire_config;
            if s.pit.wants_stop
                && s.tyre_compound != s.pit.next_compound
                && wanted(tyre.compound(s.pit.next_compound).kind)
            {
                asked
                    .entry(*id)
                    .or_insert((tick as f32 / HZ as f32, line_snow(gs)));
            }
        }
    });
    asked
}

/// A dry winter race with snow forecast six minutes in: the rally cars
/// call for their studs before the snow lies on the line (most before it
/// starts to fall, a gambler as it does), and every one is on them once
/// the line is covered.
#[test]
fn the_ai_fits_studs_before_the_forecast_snow() {
    let conditions = SessionConditions {
        weather: Weather::Overcast,
        air_temp_c: Some(-3),
        changeable: Some(2),
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race_in(conditions, 4, rally());
    if !gs
        .track_config
        .has_sidecars(&[Sidecar::Pit], "a stop for studs")
    {
        return;
    }
    const SNOW_AT_S: f32 = 360.0;
    gs.sky.forecast = vec![WeatherChange {
        at_s: SNOW_AT_S,
        weather: Weather::Snow,
    }];
    let asked = calls(&mut gs, &ids, 840, |k| k == CompoundKind::Snow);
    let mut on_studs = 0;
    for id in &ids {
        let s = &gs.session.participants[id];
        let tyre = &gs.car_configs[&s.car_config_id].tire_config;
        on_studs += (tyre.compound(s.tyre_compound).kind == CompoundKind::Snow) as usize;
        println!(
            "snow at {SNOW_AT_S} s: asked for studs at {:?}",
            asked.get(id)
        );
    }
    println!(
        "after 14 min: line cover {:.2}, {on_studs} of {} on studs",
        line_snow(&gs),
        ids.len()
    );
    assert_eq!(asked.len(), ids.len(), "every car asked for studs");
    for (at_s, cover) in asked.values() {
        assert!(
            *cover < apexsim_server::pit::SNOW_ON_COVER,
            "asked at {at_s} s on a line already covered {cover}"
        );
    }
    assert!(
        asked.values().any(|(at_s, _)| *at_s < SNOW_AT_S),
        "ahead of the snow"
    );
    assert!(line_snow(&gs) > apexsim_server::pit::SNOW_ON_COVER);
    assert_eq!(on_studs, ids.len(), "all on studs on the covered line");
}

/// A snow race whose snow stops and whose sun comes out on a track a few
/// degrees over freezing: the line melts, and the rally cars call for
/// their tarmac tyres while it still holds a snow tyre's cover, not after
/// it is bare.
#[test]
fn the_ai_comes_off_studs_ahead_of_the_melt() {
    let conditions = SessionConditions {
        weather: Weather::Snow,
        air_temp_c: Some(2),
        changeable: Some(2),
        ..SessionConditions::DEFAULT
    };
    let (mut gs, ids) = race_in(conditions, 4, rally());
    if !gs
        .track_config
        .has_sidecars(&[Sidecar::Pit], "a stop off studs")
    {
        return;
    }
    gs.sky.forecast = vec![WeatherChange {
        at_s: 30.0,
        weather: Weather::Sunny,
    }];
    let start_on = {
        let s = &gs.session.participants[&ids[0]];
        gs.car_configs[&s.car_config_id]
            .tire_config
            .compound(s.tyre_compound)
            .kind
    };
    assert_eq!(start_on, CompoundKind::Snow, "out on studs on the snow");
    let asked = calls(&mut gs, &ids, 900, |k| k != CompoundKind::Snow);
    for id in &ids {
        println!("melt: asked off studs at {:?}", asked.get(id));
    }
    println!(
        "after 15 min: track {:.1} °C, line cover {:.2}",
        gs.sky.track_c,
        line_snow(&gs)
    );
    assert!(gs.sky.track_c > 0.0, "the track warmed over freezing");
    assert!(
        line_snow(&gs) < apexsim_server::pit::SNOW_OFF_COVER,
        "the line melted"
    );
    assert_eq!(asked.len(), ids.len(), "every car asked off studs");
    for id in &ids {
        let s = &gs.session.participants[id];
        let tyre = &gs.car_configs[&s.car_config_id].tire_config;
        assert_ne!(
            tyre.compound(s.tyre_compound).kind,
            CompoundKind::Snow,
            "off studs on the bare line"
        );
    }
    for (at_s, cover) in asked.values() {
        assert!(
            *cover >= apexsim_server::pit::SNOW_OFF_COVER,
            "asked at {at_s} s on a line already bare {cover}"
        );
    }
}
