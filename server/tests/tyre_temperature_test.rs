//! Tyre temperature end to end: cars go out cold (or on blankets), come up
//! to their window within a lap or two at racing pace, and the AI drives to
//! the grip its tyres have; a hotlap goes out warm; a slide overheats.
//!
//! Driven through `GameSession` directly (no network, no real-time waits),
//! like `fuel_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml")
        .expect("failed to load Monza")
}

/// A shipped car, or the player's own (an imported one) by its folder.
fn car(folder: &str) -> CarConfig {
    let shipped = format!("../content/cars/default/{folder}/car.toml");
    let custom = format!("../content/cars/custom/{folder}/car.toml");
    let path = if Path::new(&shipped).exists() {
        shipped
    } else {
        custom
    };
    CarLoader::load_from_file(Path::new(&path)).expect("car loads")
}

/// One tyre set's state, averaged per axle: (front tread, front core, rear
/// tread, rear core, grip share).
fn axles(state: &CarState) -> (f32, f32, f32, f32, f32) {
    let t = &state.tires;
    (
        0.5 * (t.front_left.temperature_c + t.front_right.temperature_c),
        0.5 * (t.front_left.core_temperature_c + t.front_right.core_temperature_c),
        0.5 * (t.rear_left.temperature_c + t.rear_right.temperature_c),
        0.5 * (t.rear_left.core_temperature_c + t.rear_right.core_temperature_c),
        state.tyre_grip_share(),
    )
}

/// Per completed lap of an AI race at Monza: the lap time, s, and the
/// tyres at the line.
struct LapLog {
    time_s: f32,
    tyres: (f32, f32, f32, f32, f32),
    /// Lowest grip share seen during the lap, and the hottest tread.
    min_grip: f32,
    max_tread: f32,
    /// The most worn tyre at the line, percent.
    wear: f32,
}

fn ai_race(folder: &str, laps: u8, conditions: SessionConditions) -> (CarConfig, Vec<LapLog>) {
    let car = car(folder);
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let mut profile = AiDriverProfile::new("Tyre AI", 95);
    profile.id = Uuid::from_u128(7100);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        laps,
    );
    let mut track = monza();
    conditions.apply_to_track(&mut track);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);

    let mut logs = Vec::new();
    let (mut min_grip, mut max_tread) = (1.0f32, f32::MIN);
    for _ in 0..(240 * 60 * 3 * laps as usize) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let state = &gs.session.participants[&driver];
        if gs.session.game_mode == GameMode::Race {
            min_grip = min_grip.min(state.tyre_grip_share());
            for t in state.tires.each() {
                max_tread = max_tread.max(t.temperature_c);
            }
        }
        for out in gs.take_lap_events() {
            if let Some(ms) = out.event.lap_time_ms {
                let s = &gs.session.participants[&driver];
                logs.push(LapLog {
                    time_s: ms as f32 / 1000.0,
                    tyres: axles(s),
                    min_grip,
                    max_tread,
                    wear: s
                        .tires
                        .each()
                        .iter()
                        .map(|t| t.wear_percent)
                        .fold(0.0, f32::max),
                });
                min_grip = 1.0;
                max_tread = f32::MIN;
            }
        }
        if logs.len() >= laps as usize {
            break;
        }
    }
    (car, logs)
}

fn print_race(folder: &str, car: &CarConfig, logs: &[LapLog]) {
    let t = &car.tire_config;
    println!(
        "{folder}: window {:.0} ± {:.0} °C, blankets {:?}",
        t.optimal_temperature_c, t.temperature_window_c, t.blanket_temperature_c
    );
    for (i, lap) in logs.iter().enumerate() {
        let (ft, fc, rt, rc, grip) = lap.tyres;
        println!(
            "  lap {}: {:7.3} s  front {ft:5.1}/{fc:5.1}  rear {rt:5.1}/{rc:5.1}  grip {grip:.3}  (lap min {:.3}, hottest tread {:.0}, wear {:.1}%)",
            i + 1,
            lap.time_s,
            lap.min_grip,
            lap.max_tread,
            lap.wear
        );
    }
}

/// The calibration harness: where each class's tyres sit over a race at
/// Monza. `TYRE_PROBE_CARS=a,b` narrows it, `TYRE_PROBE_LAPS=N` lengthens it.
#[test]
#[ignore]
fn tyre_temperature_probe() {
    let cars = std::env::var("TYRE_PROBE_CARS").unwrap_or_else(|_| {
        "fugazzi-sf26,panini-zomba-hypercar,yotota-lmp2,posh-gt3rs".to_string()
    });
    let laps = std::env::var("TYRE_PROBE_LAPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4);
    for folder in cars.split(',') {
        let (car, logs) = ai_race(folder, laps, SessionConditions::DEFAULT);
        print_race(folder, &car, &logs);
    }
}

/// The temperature the grip reads, per axle (front, rear), from the
/// logged tread and core.
fn grip_temperatures(tyres: (f32, f32, f32, f32, f32)) -> (f32, f32) {
    let share = apexsim_server::tyre_thermal::TREAD_GRIP_SHARE;
    let (ft, fc, rt, rc, _) = tyres;
    (
        share * ft + (1.0 - share) * fc,
        share * rt + (1.0 - share) * rc,
    )
}

#[test]
fn every_class_warms_into_its_window_and_stays_there() {
    for folder in [
        "fugazzi-sf26",
        "panini-zomba-hypercar",
        "yotota-lmp2",
        "posh-gt3rs",
    ] {
        let (car, logs) = ai_race(folder, 3, SessionConditions::DEFAULT);
        print_race(folder, &car, &logs);
        assert_eq!(logs.len(), 3, "{folder}: three laps");
        let t = &car.tire_config;
        let (lo, hi) = (
            t.optimal_temperature_c - t.temperature_window_c,
            t.optimal_temperature_c + t.temperature_window_c,
        );
        let (front, rear) = grip_temperatures(logs[2].tyres);
        for (axle, temp) in [("front", front), ("rear", rear)] {
            assert!(
                (lo..=hi).contains(&temp),
                "{folder}: the {axle} tyres run at {temp:.1} °C on lap 3, outside {lo}..{hi}"
            );
        }
        assert!(
            logs[2].tyres.4 > 0.97,
            "{folder}: warm tyres keep their grip ({:.3})",
            logs[2].tyres.4
        );
        // The start costs something (cool tyres, and the standing start),
        // not a whole lap of crawling. The standing start alone is ~5 s in
        // the LMP2 on warm tyres; the cool ones add about 3.
        let cost = logs[0].time_s - logs[2].time_s;
        assert!(
            (0.0..10.0).contains(&cost),
            "{folder}: lap 1 is {cost:.2} s slower than lap 3"
        );
    }
}

#[test]
fn a_wet_track_runs_the_tyres_cooler() {
    let (_, dry) = ai_race("posh-gt3rs", 2, SessionConditions::DEFAULT);
    let wet = SessionConditions {
        weather: Weather::HeavyRain,
        ..SessionConditions::DEFAULT
    };
    let (_, rain) = ai_race("posh-gt3rs", 2, wet);
    let (dry_front, dry_rear) = grip_temperatures(dry[1].tyres);
    let (wet_front, wet_rear) = grip_temperatures(rain[1].tyres);
    println!("dry {dry_front:.1}/{dry_rear:.1}, heavy rain {wet_front:.1}/{wet_rear:.1}");
    assert!(wet_front < dry_front - 10.0 && wet_rear < dry_rear - 10.0);
}

/// One human seated on Monza in `folder`, the session switched to `mode`.
fn seated(folder: &str, mode: GameMode) -> (GameSession, PlayerId, CarConfig) {
    let car = car(folder);
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        0,
        3,
    );
    let mut gs = GameSession::new(session, monza(), car_configs);
    let player = Uuid::from_u128(100);
    // Seated into the mode, as a driver joining it is: a seat taken in the
    // lobby is one on a race grid.
    gs.set_game_mode(mode);
    gs.add_player(player, car.id).expect("seat");
    (gs, player, car)
}

fn tread(gs: &GameSession, player: &PlayerId) -> f32 {
    gs.session.participants[player]
        .tires
        .front_left
        .temperature_c
}

#[test]
fn cars_go_out_at_the_air_or_on_their_blankets() {
    let (gs, player, _) = seated("posh-gt3rs", GameMode::FreePractice);
    let air = gs.track_config.track_surface.air_temperature_c;
    assert!(gs.session.participants[&player].tyres_fitted);
    assert_eq!(tread(&gs, &player), air, "no blankets: out at the air");
    let pressure = gs.session.participants[&player]
        .tires
        .front_left
        .pressure_kpa;
    assert!(pressure < 140.0, "a cold tyre is soft: {pressure:.0} kPa");

    let (gs, player, car) = seated("fugazzi-sf26", GameMode::FreePractice);
    assert_eq!(
        tread(&gs, &player),
        car.tire_config.blanket_temperature_c.expect("F1 blankets")
    );

    // A seat taken in the lobby is on a race grid.
    let (gs, player, car) = seated("posh-gt3rs", GameMode::Lobby);
    assert_eq!(
        tread(&gs, &player),
        apexsim_server::tyre_thermal::grid_temperature_c(
            &car.tire_config,
            &gs.track_config.track_surface
        )
    );
}

#[test]
fn a_race_grid_is_given_its_formation_lap() {
    let (mut gs, player, car) = seated("posh-gt3rs", GameMode::Lobby);
    gs.start_countdown_mode(1, GameMode::Race);
    let expected = apexsim_server::tyre_thermal::grid_temperature_c(
        &car.tire_config,
        &gs.track_config.track_surface,
    );
    assert_eq!(tread(&gs, &player), expected);
    assert!(expected > gs.track_config.track_surface.air_temperature_c + 10.0);
}

#[test]
fn a_hotlap_goes_out_on_warm_tyres() {
    let (mut gs, player, car) = seated("posh-gt3rs", GameMode::Hotlap);
    gs.hotlap_relocate(&player, HotlapDestination::Track)
        .expect("out");
    let state = &gs.session.participants[&player];
    assert_eq!(tread(&gs, &player), car.tire_config.optimal_temperature_c);
    assert_eq!(state.tyre_grip_share(), 1.0);
    assert_eq!(
        state.tires.front_left.pressure_kpa,
        car.tire_config.pressure_front_kpa
    );
}
