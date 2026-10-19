//! Brake and engine heat end to end on Monza's main straight: an F1 car
//! on cold carbon brakes stops a long way later than on warm ones, a GT3 on
//! steel barely notices, a stop heats the discs hundreds of degrees, and
//! bigger ducts run them cooler.
//!
//! Driven through `GameSession` directly, like `aero_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_sim::brakes::BrakeMaterial;
use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::car_setup::CarSetup;
use apexsim_sim::data::*;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::physics;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn car(folder: &str) -> CarConfig {
    CarLoader::load_from_file(Path::new(&format!(
        "../../apexsim-content/cars/default/{folder}/car.toml"
    )))
    .expect("car loads")
}

/// A car at `speed` on Monza's straight with its brakes at `brake_c`, then
/// the brakes full on: how far it takes to get under 20 m/s, and the
/// hottest brake afterwards.
fn stop(folder: &str, speed: f32, brake_c: f32, setup: CarSetup) -> (f32, f32) {
    let (distance, s) = stop_car(car(folder), speed, brake_c, setup, [0.0; 4]);
    (
        distance,
        s.brake_temp_c.iter().copied().fold(f32::MIN, f32::max),
    )
}

/// [`stop`] for `config` on discs `disc_wear` worn: the distance and the
/// car after.
fn stop_car(
    config: CarConfig,
    speed: f32,
    brake_c: f32,
    setup: CarSetup,
    disc_wear: [f32; 4],
) -> (f32, CarState) {
    let mut car_configs = HashMap::new();
    car_configs.insert(config.id, config.clone());
    let track =
        TrackLoader::load_from_file("../../apexsim-content/tracks/default/Monza/Monza.yaml")
            .expect("Monza");
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
    gs.set_game_mode(GameMode::FreePractice);
    gs.add_player(player, config.id).expect("seat");
    gs.set_car_setup(&player, setup);
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, 60.0);
    let state = gs.session.participants.get_mut(&player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = 6;
    state.auto_gearbox = true;
    state.brake_temp_c = [brake_c; 4];
    state.brake_disc_wear_pct = disc_wear;
    physics::seed_track_progress(state, &gs.track_config);
    let (x0, y0) = (x, y);
    let brake = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, brake)].into();
    for _ in 0..(HZ * 10) {
        gs.tick(&inputs);
        if gs.session.participants[&player].speed_mps < 20.0 {
            break;
        }
    }
    let s = gs.session.participants[&player].clone();
    let distance = ((s.pos_x - x0).powi(2) + (s.pos_y - y0).powi(2)).sqrt();
    (distance, s)
}

#[test]
fn cold_carbon_stops_late_and_steel_does_not_care() {
    assert_eq!(car("fugazzi-sf26").brake_material, BrakeMaterial::Carbon);
    assert_eq!(car("posh-gt3rs").brake_material, BrakeMaterial::Steel);
    let (cold, _) = stop("fugazzi-sf26", 80.0, 20.0, CarSetup::default());
    let (warm, after) = stop("fugazzi-sf26", 80.0, 500.0, CarSetup::default());
    println!(
        "F1 from 288 km/h: {warm:.0} m on warm carbon ({after:.0} °C after), {cold:.0} m on cold"
    );
    assert!(
        cold > warm + 10.0,
        "cold carbon: {cold:.0} m against {warm:.0}"
    );
    assert!(after > 700.0, "a stop heats the discs: {after:.0} °C");

    let (steel_cold, _) = stop("posh-gt3rs", 70.0, 20.0, CarSetup::default());
    let (steel_warm, _) = stop("posh-gt3rs", 70.0, 300.0, CarSetup::default());
    println!("GT3 from 252 km/h: {steel_warm:.0} m warm, {steel_cold:.0} m cold");
    assert!(steel_cold < steel_warm * 1.08, "steel works from cold");
}

#[test]
fn bigger_ducts_run_the_brakes_cooler() {
    let open = CarSetup {
        brake_ducts: 5,
        ..Default::default()
    };
    let closed = CarSetup {
        brake_ducts: -5,
        ..Default::default()
    };
    let (_, hot) = stop("fugazzi-sf26", 80.0, 500.0, closed);
    let (_, cool) = stop("fugazzi-sf26", 80.0, 500.0, open);
    println!("after a stop: {hot:.0} °C on small ducts, {cool:.0} °C on big ones");
    assert!(cool < hot);
    let tuned = CarSetup {
        brake_ducts: 5,
        ..Default::default()
    }
    .apply(&car("fugazzi-sf26"));
    assert!(
        tuned.drag_coefficient > car("fugazzi-sf26").drag_coefficient,
        "air costs drag"
    );
}

/// `[brakes] disc_wear_per_mj`: the discs wear apart from the pads, and a
/// worn disc runs hotter for the same stop. Without the key they never
/// wear.
#[test]
fn discs_wear_apart_from_the_pads_and_a_thin_one_runs_hot() {
    let plain = car("fugazzi-sf26");
    assert_eq!(plain.brake_disc_wear_per_mj, 0.0);
    let (_, without) = stop_car(plain.clone(), 80.0, 500.0, CarSetup::default(), [0.0; 4]);
    assert_eq!(
        without.brake_disc_wear_pct, [0.0; 4],
        "no key, no disc wear"
    );
    let mut wearing = plain;
    wearing.brake_disc_wear_per_mj = 0.05;
    let (_, new) = stop_car(wearing.clone(), 80.0, 500.0, CarSetup::default(), [0.0; 4]);
    let (_, worn) = stop_car(wearing, 80.0, 500.0, CarSetup::default(), [80.0; 4]);
    let hottest = |s: &CarState| s.brake_temp_c.iter().copied().fold(f32::MIN, f32::max);
    println!(
        "disc wear from one stop: {:?} %, pads {:?} %; hottest {:.0} °C new, {:.0} °C on worn discs",
        new.brake_disc_wear_pct,
        new.brake_wear_pct,
        hottest(&new),
        hottest(&worn)
    );
    assert!(new.brake_disc_wear_pct.iter().all(|w| *w > 0.0));
    for (a, b) in new.brake_wear_pct.iter().zip(without.brake_wear_pct) {
        assert!(
            (a - b).abs() < 0.05 * b,
            "the pads near as before: {a} vs {b}"
        );
    }
    assert!(hottest(&worn) > hottest(&new) + 50.0);
}
