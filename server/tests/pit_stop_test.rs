//! Pit stops end to end at Monza: the limiter holds a car to the lane's
//! limit, a car stopped at its box is serviced (the compound its driver
//! chose, fresh), and an AI on worn tyres plans a stop, drives the lane to
//! its box and back out, and races on.
//!
//! Driven through `GameSession` directly, like `fuel_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads")
}

/// One human on Monza in practice; skipped without the pit sidecar.
fn practice() -> Option<(GameSession, PlayerId)> {
    let track = monza();
    track.pit_lane.as_ref()?;
    let car = gt3();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
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
    gs.add_player(player, car.id).expect("seat");
    Some((gs, player))
}

/// Put `player` on the lane at `station` along it, heading down it.
fn on_the_lane(gs: &mut GameSession, player: &PlayerId, station: f32, speed: f32) {
    let lane = gs.track_config.pit_lane.clone().expect("lane");
    let (x, y) = lane.point_at(station);
    let (x2, y2) = lane.point_at(station + 2.0);
    let yaw = (y2 - y).atan2(x2 - x);
    let state = gs.session.participants.get_mut(player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = if speed > 20.0 { 3 } else { 1 };
    state.auto_gearbox = true;
    state.nearest_centerline_idx = None;
    physics::seed_track_progress(state, &gs.track_config);
}

#[test]
fn the_limiter_holds_a_car_to_the_lane_limit() {
    let Some((mut gs, player)) = practice() else {
        eprintln!("no pit sidecar for Monza: run ats-export");
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    on_the_lane(
        &mut gs,
        &player,
        lane.limit_start_m + 5.0,
        lane.speed_limit_mps + 8.0,
    );
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, flat_out)].into();
    let mut top = 0.0f32;
    for k in 0..(240 * 8) {
        gs.tick(&inputs);
        let s = &gs.session.participants[&player];
        if k > 240 * 3 && s.pit.limiter {
            top = top.max(s.speed_mps);
        }
    }
    println!(
        "flat out between the lines: {:.1} km/h against {:.0}",
        top * 3.6,
        lane.speed_limit_mps * 3.6
    );
    assert!(top > lane.speed_limit_mps - 2.0, "it drives at the limit");
    assert!(top < lane.speed_limit_mps + 0.5, "and no faster");
}

#[test]
fn a_car_stopped_at_its_box_gets_the_set_its_driver_chose() {
    let Some((mut gs, player)) = practice() else {
        return;
    };
    let lane = gs.track_config.pit_lane.clone().unwrap();
    gs.set_car_setup(
        &player,
        CarSetup {
            tyre_compound: 1,
            ..Default::default()
        },
    );
    let slot = gs.session.participants[&player].grid_position;
    let spot = *lane.box_for(slot);
    {
        let state = gs.session.participants.get_mut(&player).unwrap();
        state.pos_x = spot.x;
        state.pos_y = spot.y;
        state.yaw_rad = spot.yaw_rad;
        state.tires.front_left.wear_percent = 60.0;
        state.damage.front_damage_percent = 30.0;
    }
    let held = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, held)].into();
    gs.tick(&inputs);
    let s = &gs.session.participants[&player];
    assert!(s.pit.servicing, "stopped at its box, the crew start work");
    let seconds = s.pit.service_left_s;
    assert!(
        seconds > apexsim_server::pit::TYRE_CHANGE_S,
        "the repairs take time too: {seconds}"
    );
    for _ in 0..((seconds + 0.5) * 240.0) as usize {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&player];
    assert!(!s.pit.servicing && s.pit.serviced && s.pit.stops == 1);
    assert_eq!(s.tyre_compound, 0, "the softs the setup asked for");
    assert_eq!(s.tires.front_left.wear_percent, 0.0);
    assert_eq!(s.damage.front_damage_percent, 0.0);
}

#[test]
fn an_ai_on_worn_tyres_pits_and_races_on() {
    let track = monza();
    let Some(lane) = track.pit_lane.clone() else {
        return;
    };
    let car = gt3();
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);
    let mut profile = AiDriverProfile::new("Pit AI", 95);
    profile.id = Uuid::from_u128(7300);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        4,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    {
        let s = gs.session.participants.get_mut(&driver).unwrap();
        for t in s.tires.each_mut() {
            t.wear_percent = 72.0;
        }
    }

    let (mut laps, mut off_ticks, mut in_box_ticks) = (Vec::new(), 0u32, 0u32);
    let mut stopped_at = None;
    for _ in 0..(240 * 60 * 10) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        let s = &gs.session.participants[&driver];
        off_ticks += (!s.is_on_track && !s.pit.in_lane) as u32;
        if s.pit.servicing {
            in_box_ticks += 1;
            stopped_at.get_or_insert((s.pos_x, s.pos_y));
        }
        for out in gs.take_lap_events() {
            if let Some(ms) = out.event.lap_time_ms {
                laps.push(ms as f32 / 1000.0);
            }
        }
        if laps.len() >= 3 {
            break;
        }
    }
    let s = &gs.session.participants[&driver];
    println!(
        "laps {laps:?}; stops {}, {:.1} s in the box on {} ; {:.1} s off the road",
        s.pit.stops,
        in_box_ticks as f32 / 240.0,
        apexsim_server::tyre_thermal::COMPOUNDS[s.tyre_compound as usize].name,
        off_ticks as f32 / 240.0
    );
    assert_eq!(laps.len(), 3, "it races on");
    assert_eq!(s.pit.stops, 1, "one stop");
    let (x, y) = stopped_at.expect("it stopped");
    let spot = lane.box_for(s.grid_position);
    assert!(
        ((x - spot.x).powi(2) + (y - spot.y).powi(2)).sqrt() <= apexsim_server::pit::BOX_RADIUS_M
    );
    assert!(
        s.tires.each().iter().all(|t| t.wear_percent < 20.0),
        "fresh tyres"
    );
    assert!(
        off_ticks < 240 * 3,
        "{:.1} s off the road",
        off_ticks as f32 / 240.0
    );
    // The stop lap costs the lane and the service, not a disaster.
    let clean = laps[2];
    let stop_lap = laps[0].max(laps[1]);
    assert!(
        stop_lap > clean + 10.0 && stop_lap < clean + 60.0,
        "{laps:?}"
    );
}
