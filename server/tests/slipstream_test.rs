//! Slipstream end to end: a car running flat out behind another down
//! Monza's main straight gains on one running alone, feels the dirty air on
//! its front wing, and the leader feels nothing.
//!
//! Driven through `GameSession` directly, like `fuel_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::slipstream::Wake;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn f1() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/fugazzi-sf26/car.toml"))
        .expect("car loads")
}

/// Put `player` on the centerline `station_m` from the line at `speed`, in
/// top gear's neighbourhood with the box doing the rest and warm tyres.
fn place(gs: &mut GameSession, player: &PlayerId, station_m: f32, speed: f32) {
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, station_m);
    let state = gs.session.participants.get_mut(player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = 6;
    state.auto_gearbox = true;
    state.tyres_fitted = false;
    physics::seed_track_progress(state, &gs.track_config);
}

/// Four seconds flat out down the straight: the follower's speed at the
/// end, the lowest drag and front downforce factors it saw, and the
/// leader's.
fn run(with_leader: bool) -> (f32, f32, f32, Wake) {
    let car = f1();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let track = TrackLoader::load_from_file("../content/tracks/default/Monza.yaml")
        .expect("failed to load Monza");
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    let follower = Uuid::from_u128(100);
    let leader = Uuid::from_u128(101);
    gs.add_player(follower, car.id).expect("seat");
    if with_leader {
        gs.add_player(leader, car.id).expect("seat");
    }
    gs.set_game_mode(GameMode::FreePractice);
    place(&mut gs, &follower, 100.0, 60.0);
    if with_leader {
        place(&mut gs, &leader, 100.0 + car.length_m + 10.0, 60.0);
    }

    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let inputs: HashMap<PlayerId, PlayerInputData> =
        [(follower, flat_out), (leader, flat_out)].into();
    let (mut min_drag, mut min_front) = (1.0f32, 1.0f32);
    let mut leader_wake = Wake::CLEAN;
    for _ in 0..(240 * 4) {
        gs.tick(&inputs);
        let me = &gs.session.participants[&follower];
        min_drag = min_drag.min(me.wake.drag);
        min_front = min_front.min(me.wake.downforce_front);
        if let Some(l) = gs.session.participants.get(&leader) {
            leader_wake = l.wake;
        }
    }
    (
        gs.session.participants[&follower].speed_mps,
        min_drag,
        min_front,
        leader_wake,
    )
}

#[test]
fn a_car_in_the_tow_gains_and_the_leader_is_in_clean_air() {
    let (alone, clean_drag, clean_front, _) = run(false);
    let (towed, drag, front, leader) = run(true);
    println!(
        "alone {:.1} km/h, in the tow {:.1} km/h (drag x{drag:.2}, front downforce x{front:.2})",
        alone * 3.6,
        towed * 3.6
    );
    assert_eq!(
        (clean_drag, clean_front),
        (1.0, 1.0),
        "nobody ahead, no wake"
    );
    assert_eq!(leader, Wake::CLEAN, "nothing ahead of the leader");
    assert!(drag < 0.85, "10 m behind is a real tow: x{drag:.2}");
    assert!(front < 0.95, "and dirty air on the front wing: x{front:.2}");
    assert!(
        towed > alone + 1.0,
        "the tow is worth speed: {:.1} vs {:.1} km/h",
        towed * 3.6,
        alone * 3.6
    );
}
