//! Fuel end to end: a race is fuelled for its distance from the session's
//! own estimate of a lap, the car then burns about what was planned, a
//! hotlap goes out light, and the fuel knob adds laps only where the car is
//! fuelled.
//!
//! Driven through `GameSession` directly (no network, no real-time waits),
//! like `lap_timing_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::{
    GameSession, HOTLAP_FUEL_LAPS, MIN_FUEL_LAPS, RACE_FUEL_MARGIN, RACE_FUEL_RESERVE_LAPS,
};
use apexsim_server::racing_line;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml")
        .expect("failed to load Monza")
}

fn car(folder: &str) -> CarConfig {
    CarLoader::load_from_file(Path::new(&format!(
        "../content/cars/default/{folder}/car.toml"
    )))
    .expect("car loads")
}

fn close(a: f32, b: f32, what: &str) {
    assert!((a - b).abs() < 1e-3 * b.max(1.0), "{what}: {a} vs {b}");
}

/// A three-lap AI race at Monza in `folder`: the session's estimate of a
/// lap and what the AI burnt on its second (flying) lap, litres.
fn race_burn(folder: &str) -> (f32, f32) {
    let car = car(folder);
    let car_id = car.id;
    let capacity = car.fuel.capacity_liters;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);

    let mut profile = AiDriverProfile::new("Fuel AI", 95);
    profile.id = Uuid::from_u128(7000);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let laps = 3;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        laps,
    );
    let mut gs = GameSession::with_ai_profiles(session, monza(), car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);

    let lap = gs.lap_fuel_liters(car_id).expect("a lap estimate");
    let fuelled = gs.session.participants[&driver].fuel_liters;
    close(
        fuelled,
        ((laps as f32 * (1.0 + RACE_FUEL_MARGIN) + RACE_FUEL_RESERVE_LAPS) * lap).min(capacity),
        "race fuel",
    );

    // Fuel at each crossing of the line.
    let mut at_line = Vec::new();
    for _ in 0..(240 * 60 * 10) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        for out in gs.take_lap_events() {
            if out.event.lap_time_ms.is_some() {
                at_line.push(gs.session.participants[&driver].fuel_liters);
            }
        }
        if at_line.len() >= 2 {
            break;
        }
    }
    assert_eq!(at_line.len(), 2, "{folder}: the AI completes two laps");
    assert!(at_line[1] > 0.0, "{folder}: not dry with a lap to go");
    (lap, at_line[0] - at_line[1])
}

#[test]
fn a_race_is_fuelled_for_its_distance_and_burns_what_was_planned() {
    // The estimate is a car at the limit of its plan: the AI, a little
    // slower, burns less; it must never burn more than the race margin
    // covers, or a long race runs dry.
    for (folder, plausible) in [
        ("fugazzi-sf26", 1.4..3.5),
        ("panini-zomba-hypercar", 1.8..4.5),
        ("yotota-lmp2", 1.8..4.5),
        ("posh-gt3rs", 1.8..4.5),
    ] {
        let (lap, burnt) = race_burn(folder);
        println!(
            "{folder}: estimated {lap:.2} L a lap of Monza, the AI burnt {burnt:.2} ({:.0}%)",
            100.0 * burnt / lap
        );
        assert!(
            plausible.contains(&lap),
            "{folder}: {lap:.2} L a lap of Monza is not a racing car's"
        );
        assert!(
            burnt > lap * 0.7 && burnt < lap * (1.0 + RACE_FUEL_MARGIN),
            "{folder}: a flying lap burns {burnt:.3} L against an estimate of {lap:.3}"
        );
    }
}

#[test]
fn a_full_tank_costs_lap_time_on_the_plan() {
    let track = monza();
    let car = car("fugazzi-sf26");
    let lap_time = |fuel: f32| {
        let profile = racing_line::build_laden(&track, &car, fuel).expect("line");
        let n = profile.speed_mps.len();
        (0..n)
            .map(|i| {
                let v = 0.5 * (profile.speed_mps[i] + profile.speed_mps[(i + 1) % n]);
                profile.spacing_m / v.max(1.0)
            })
            .sum::<f32>()
    };
    let dry = lap_time(0.0);
    let full = lap_time(car.fuel.capacity_liters);
    println!("Monza on the plan: {dry:.2} s dry, {full:.2} s full");
    // Real F1 rule of thumb: about 0.03 s a lap per kilogram.
    let per_kg = (full - dry) / car.fuel.mass_kg(car.fuel.capacity_liters);
    assert!(
        (0.01..0.06).contains(&per_kg),
        "{per_kg:.3} s a lap per kg of fuel"
    );
}

/// One human seated on Monza in the default car, switched to Hotlap.
fn hotlap_session() -> (GameSession, PlayerId, CarConfigId) {
    let track = monza();
    let car = CarConfig::default();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let player = Uuid::from_u128(100);
    gs.add_player(player, car_id).expect("seat");
    gs.set_game_mode(GameMode::Hotlap);
    (gs, player, car_id)
}

#[test]
fn a_hotlap_goes_out_light_and_the_knob_adds_laps_in_the_garage() {
    let (mut gs, player, car_id) = hotlap_session();
    let lap = gs.lap_fuel_liters(car_id).expect("a lap estimate");
    let fuel = |gs: &GameSession| gs.session.participants[&player].fuel_liters;
    close(fuel(&gs), HOTLAP_FUEL_LAPS * lap, "garage fill");

    gs.hotlap_relocate(&player, HotlapDestination::Track, false)
        .expect("out");
    close(fuel(&gs), HOTLAP_FUEL_LAPS * lap, "out on three laps");

    // On the track the knob waits for the garage.
    let two_more = CarSetup {
        fuel_load: 2,
        ..Default::default()
    };
    gs.set_car_setup(&player, two_more);
    close(
        fuel(&gs),
        HOTLAP_FUEL_LAPS * lap,
        "no refuelling on the track",
    );
    assert!(
        gs.simulated_config_for(&player).map(|c| c.id) == Some(car_id),
        "the fuel knob alone makes no tuned car"
    );

    gs.hotlap_relocate(&player, HotlapDestination::Garage, false)
        .expect("in");
    close(
        fuel(&gs),
        (HOTLAP_FUEL_LAPS + 2.0) * lap,
        "the garage adds two laps",
    );

    // In the garage the knob fills the car at once, and never under a lap.
    gs.set_car_setup(
        &player,
        CarSetup {
            fuel_load: -5,
            ..Default::default()
        },
    );
    close(fuel(&gs), MIN_FUEL_LAPS * lap, "never under a lap");
}

#[test]
fn practice_starts_full_less_the_knob() {
    let (mut gs, player, car_id) = hotlap_session();
    let capacity = gs.car_configs[&car_id].fuel.capacity_liters;
    let lap = gs.lap_fuel_liters(car_id).expect("a lap estimate");
    gs.set_game_mode(GameMode::FreePractice);
    close(
        gs.session.participants[&player].fuel_liters,
        capacity,
        "practice fill",
    );
    gs.set_game_mode(GameMode::Lobby);
    gs.set_car_setup(
        &player,
        CarSetup {
            fuel_load: -3,
            ..Default::default()
        },
    );
    gs.set_game_mode(GameMode::FreePractice);
    close(
        gs.session.participants[&player].fuel_liters,
        capacity - 3.0 * lap,
        "three laps short of full",
    );
}
