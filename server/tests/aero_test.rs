//! Ride-height aero end to end: an F1 car flat out down Monza's main
//! straight squats on its springs and gains downforce, dives under braking
//! with the balance moving forward, holds its ride height steady at a
//! steady speed, and its setup's wings and ride heights do what they say.
//!
//! Driven through `GameSession` directly, like `slipstream_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::aero;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn f1() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/fugazzi-sf26/car.toml"))
        .expect("car loads")
}

/// One driver on Monza's straight 100 m past the line at `speed`, warm
/// tyres, the box doing the shifting.
fn on_the_straight(speed: f32) -> (GameSession, PlayerId) {
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
    let player = Uuid::from_u128(100);
    gs.add_player(player, car.id).expect("seat");
    gs.set_game_mode(GameMode::FreePractice);
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, 100.0);
    let state = gs.session.participants.get_mut(&player).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = 7;
    state.auto_gearbox = true;
    state.tyres_fitted = false;
    physics::seed_track_progress(state, &gs.track_config);
    (gs, player)
}

fn drive(gs: &mut GameSession, player: PlayerId, input: PlayerInputData, ticks: usize) {
    let inputs: HashMap<PlayerId, PlayerInputData> = [(player, input)].into();
    for _ in 0..ticks {
        gs.tick(&inputs);
    }
}

#[test]
fn the_car_squats_at_speed_and_dives_under_braking() {
    let car = f1();
    let (mut gs, player) = on_the_straight(45.0);
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    drive(&mut gs, player, flat_out, 60);
    let slow = gs.session.participants[&player].clone();
    // Flat out until near the end of the straight.
    let mut heights = Vec::new();
    for _ in 0..(240 * 5) {
        drive(&mut gs, player, flat_out, 1);
        let s = &gs.session.participants[&player];
        heights.push(s.ride_height_front_m);
    }
    let fast = gs.session.participants[&player].clone();
    println!(
        "{:.0} km/h: front {:.1} mm rear {:.1} mm; {:.0} km/h: front {:.1} mm rear {:.1} mm",
        slow.speed_mps * 3.6,
        slow.ride_height_front_m * 1000.0,
        slow.ride_height_rear_m * 1000.0,
        fast.speed_mps * 3.6,
        fast.ride_height_front_m * 1000.0,
        fast.ride_height_rear_m * 1000.0
    );
    assert!(fast.speed_mps > slow.speed_mps + 20.0);
    assert!(
        fast.ride_height_front_m < slow.ride_height_front_m
            && fast.ride_height_rear_m < slow.ride_height_rear_m,
        "the downforce squats the car"
    );
    assert!(
        fast.ride_height_front_m > 0.0,
        "the bump rubbers hold it off the road"
    );
    // No ride-height oscillation: over the last second, the front moves
    // only as the speed does.
    let last = &heights[heights.len() - 240..];
    let wobble = last
        .windows(2)
        .map(|w| (w[1] - w[0]).abs())
        .fold(0.0f32, f32::max);
    assert!(wobble < 5e-4, "a tick's step in ride height: {wobble}");
    // More downforce per unit of dynamic pressure than the file down low.
    let q = 0.5 * physics::AIR_DENSITY * fast.speed_mps.powi(2) * car.frontal_area_m2;
    let filed = q * -(car.lift_coefficient_front + car.lift_coefficient_rear);
    let made = fast.downforce_front_n + fast.downforce_rear_n;
    assert!(
        made > filed * 1.02,
        "{made:.0} N against {filed:.0} N filed"
    );

    // Stand on the brakes: the nose goes down, the rear up.
    let brake = PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    drive(&mut gs, player, brake, 60);
    let braking = &gs.session.participants[&player];
    println!(
        "braking at {:.0} km/h: front {:.1} mm rear {:.1} mm",
        braking.speed_mps * 3.6,
        braking.ride_height_front_m * 1000.0,
        braking.ride_height_rear_m * 1000.0
    );
    let rake = |s: &CarState| s.ride_height_rear_m - s.ride_height_front_m;
    assert!(rake(braking) > rake(&fast), "the dive adds rake");
}

#[test]
fn the_setup_moves_the_car_on_its_map() {
    let car = f1();
    let at = |c: &CarConfig| aero::steady_downforce_factor(c, 70.0);
    let lower = CarSetup {
        ride_height_front: -3,
        ride_height_rear: -3,
        ..Default::default()
    }
    .apply(&car);
    let stiffer = CarSetup {
        spring_front: 5,
        spring_rear: 5,
        ..Default::default()
    }
    .apply(&car);
    println!(
        "steady downforce at 252 km/h: stock x{:.3}, 6 mm lower x{:.3}, stiff springs x{:.3}",
        at(&car),
        at(&lower),
        at(&stiffer)
    );
    assert!(at(&lower) > at(&car), "lower is more downforce");
    assert!(at(&stiffer) < at(&car), "a stiffer car squats less");
    let wings = CarSetup {
        rear_wing: 3,
        ..Default::default()
    }
    .apply(&car);
    assert!(wings.lift_coefficient_rear < car.lift_coefficient_rear);
    assert!(wings.drag_coefficient > car.drag_coefficient);
}
