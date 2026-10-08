//! The session's air end to end: the reference day is the car as filed,
//! altitude and heat thin the air (a naturally aspirated engine loses its
//! breath with it, a turbo mostly does not), and the wind is the air a car
//! drives through: a tailwind down Monza's straight is worth speed, a
//! headwind costs it, and the AI still gets round in a gale.
//!
//! Driven through `GameSession` directly, like `slipstream_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::racing_line;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn car(folder: &str) -> CarConfig {
    CarLoader::load_from_file(Path::new(&format!(
        "../content/cars/default/{folder}/car.toml"
    )))
    .expect("car loads")
}

fn track(stem: &str) -> TrackConfig {
    TrackLoader::load_from_file(format!("../content/tracks/default/{stem}/{stem}.yaml"))
        .expect("track loads")
}

#[test]
fn the_reference_day_is_the_car_as_filed() {
    let default = SessionConditions::DEFAULT;
    assert_eq!(default.air_density_ratio(0.0), 1.0);
    // Resolved (every figure named) it is the same day.
    assert_eq!(default.resolve(99).air_density_ratio(0.0), 1.0);
    let rho = default.air_density(0.0);
    assert!((1.18..1.21).contains(&rho), "{rho} kg/m3 on a mild day");
}

#[test]
fn altitude_and_heat_thin_the_air() {
    let mexico = track("MexicoCity");
    let altitude = mexico.metadata.altitude_m.expect("Mexico City's altitude");
    assert!(altitude > 2000.0);
    let thin = SessionConditions::DEFAULT.air_density_ratio(altitude);
    assert!((0.74..0.82).contains(&thin), "Mexico City: {thin}");

    let spielberg = track("Spielberg").metadata.altitude_m.expect("altitude");
    let styria = SessionConditions::DEFAULT.air_density_ratio(spielberg);
    assert!((0.9..0.95).contains(&styria), "Spielberg: {styria}");

    let frost = SessionConditions {
        air_temp_c: Some(-3),
        ..SessionConditions::DEFAULT
    };
    let heat = SessionConditions {
        air_temp_c: Some(40),
        humidity_pct: Some(80),
        ..SessionConditions::DEFAULT
    };
    assert!(frost.air_density_ratio(0.0) > 1.07);
    assert!(heat.air_density_ratio(0.0) < 0.95);

    // Baked into a session's track.
    let mut session_track = mexico.clone();
    SessionConditions::DEFAULT.apply_to_track(&mut session_track);
    assert_eq!(session_track.track_surface.air_density_ratio, thin);
}

#[test]
fn a_turbo_keeps_its_breath_where_an_engine_without_one_loses_it() {
    let turbo = car("fugazzi-sf26");
    let na = car("yotota-lmp2");
    assert!(turbo.engine.forced_induction && !na.engine.forced_induction);
    let thin = 0.78;
    let turbo_loss = 1.0 - physics::engine_density_factor(&turbo, thin);
    let na_loss = 1.0 - physics::engine_density_factor(&na, thin);
    assert!((na_loss - 0.22).abs() < 1e-6);
    assert!(turbo_loss < na_loss / 3.0);

    // On the plan: at Mexico City's altitude the naturally aspirated car
    // loses its downforce and its power together, so its lap is slower;
    // the turbo car keeps its power against less drag and is quicker on
    // the straights than at sea level.
    let lap = |car: &CarConfig, altitude: Option<f32>| {
        let mut t = track("Monza");
        t.metadata.altitude_m = altitude;
        SessionConditions::DEFAULT.apply_to_track(&mut t);
        let profile = racing_line::build(&t, car).expect("line");
        let n = profile.speed_mps.len();
        let time: f32 = (0..n)
            .map(|i| {
                let v = 0.5 * (profile.speed_mps[i] + profile.speed_mps[(i + 1) % n]);
                profile.spacing_m / v.max(1.0)
            })
            .sum();
        let top = profile.speed_mps.iter().copied().fold(0.0, f32::max);
        (time, top)
    };
    let (na_sea, _) = lap(&na, None);
    let (na_high, _) = lap(&na, Some(2240.0));
    let (_, turbo_top_sea) = lap(&turbo, None);
    let (_, turbo_top_high) = lap(&turbo, Some(2240.0));
    println!(
        "LMP2 at Monza: {na_sea:.2} s at sea level, {na_high:.2} s at 2240 m; F1 top speed {:.0} -> {:.0} km/h",
        turbo_top_sea * 3.6,
        turbo_top_high * 3.6
    );
    assert!(na_high > na_sea + 0.5);
    assert!(turbo_top_high >= turbo_top_sea);
}

/// An F1 car flat out down Monza's straight for four seconds in the wind
/// from `from_deg`: its speed at the end.
fn down_the_straight(wind_kph: u8, from_deg: u16) -> f32 {
    let car = car("fugazzi-sf26");
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let mut t = track("Monza");
    SessionConditions {
        wind_kph: Some(wind_kph),
        wind_from_deg: Some(from_deg),
        ..SessionConditions::DEFAULT
    }
    .apply_to_track(&mut t);
    let session = RaceSession::new(Uuid::from_u128(1), t.id, SessionKind::Multiplayer, 8, 0, 0);
    let mut gs = GameSession::new(session, t, car_configs);
    let player = Uuid::from_u128(100);
    gs.add_player(player, car.id).expect("seat");
    gs.set_game_mode(GameMode::FreePractice);
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, 100.0);
    let state = gs.session.participants.get_mut(&player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = 60.0 * yaw.cos();
    state.vel_y = 60.0 * yaw.sin();
    state.speed_mps = 60.0;
    state.gear = 6;
    state.auto_gearbox = true;
    state.tyres_fitted = false;
    physics::seed_track_progress(state, &gs.track_config);
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, flat_out)].into();
    for k in 0..(HZ * 4) {
        gs.tick(&inputs);
        if std::env::var("AIR_DBG").is_ok() && k % HZ == 0 {
            let s = &gs.session.participants[&player];
            eprintln!(
                "  wind {:?} speed {:.1} drag {:.0} gear {} rpm {:.0}",
                gs.track_config.track_surface.wind_now_mps,
                s.speed_mps,
                s.drag_force_n,
                s.gear,
                s.engine_rpm
            );
        }
    }
    gs.session.participants[&player].speed_mps
}

#[test]
fn a_tailwind_is_worth_speed_and_a_headwind_costs_it() {
    let still = down_the_straight(0, 0);
    let head = down_the_straight(35, 0);
    let tail = down_the_straight(35, 180);
    println!(
        "Monza straight: still {:.1}, 35 km/h headwind {:.1}, tailwind {:.1} km/h",
        still * 3.6,
        head * 3.6,
        tail * 3.6
    );
    assert!(head < still - 1.0 && tail > still + 1.0);
}

#[test]
fn the_ai_gets_round_in_a_gale() {
    let car = car("posh-gt3rs");
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let mut profile = AiDriverProfile::new("Gale AI", 95);
    profile.id = Uuid::from_u128(7200);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let mut t = track("Monza");
    SessionConditions {
        wind_kph: Some(50),
        wind_from_deg: Some(135),
        ..SessionConditions::DEFAULT
    }
    .apply_to_track(&mut t);
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        2,
    );
    let mut gs = GameSession::with_ai_profiles(session, t, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    let (mut laps, mut off_ticks, mut min_share) = (0, 0u32, 1.0f32);
    for _ in 0..(HZ * 60 * 6) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        off_ticks += !s.is_on_track as u32;
        if gs.session.game_mode == GameMode::Race && s.speed_mps > 30.0 {
            min_share = min_share.min(s.aero_load_share);
        }
        laps += gs
            .take_lap_events()
            .iter()
            .filter(|e| e.event.lap_time_ms.is_some())
            .count();
        if laps >= 2 {
            break;
        }
    }
    println!(
        "a gale at Monza: {laps} laps, {:.1} s off the road, the lowest aero load share {min_share:.3}",
        off_ticks as f32 / HZ as f32
    );
    assert_eq!(laps, 2);
    assert!(min_share < 1.0, "a tailwind somewhere took load away");
    assert!(
        off_ticks < HZ as u32 * 5,
        "{} s off",
        off_ticks as f32 / HZ as f32
    );
}
