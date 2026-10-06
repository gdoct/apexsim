//! Suspension geometry end to end on a GT3 at Monza (`crate::geometry`):
//! camber trades braking for cornering, rear toe-in costs speed on the
//! straight, and the filed setup is the car as it always drove.
//!
//! Driven through `GameSession` directly, like `damage_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

const PLAYER: Uuid = Uuid::from_u128(100);

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads")
}

/// The GT3 with `setup` at `speed` on Monza's straight, driven by `input`
/// for `seconds`; `read` sees the car and where it started.
fn drive<T>(
    setup: CarSetup,
    speed: f32,
    input: impl Fn(&CarState) -> PlayerInputData,
    seconds: f32,
    read: impl Fn(&CarState, (f32, f32)) -> T,
) -> T {
    let config = setup.apply(&gt3());
    let mut car_configs = HashMap::new();
    car_configs.insert(config.id, config.clone());
    let track = TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza");
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    gs.set_game_mode(GameMode::FreePractice);
    gs.add_player(PLAYER, config.id).expect("seat");
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, 60.0);
    {
        let state = gs.session.participants.get_mut(&PLAYER).expect("seated");
        state.pos_x = x;
        state.pos_y = y;
        state.pos_z = z;
        state.yaw_rad = yaw;
        state.vel_x = speed * yaw.cos();
        state.vel_y = speed * yaw.sin();
        state.speed_mps = speed;
        state.gear = 5;
        state.auto_gearbox = true;
        state.abs = Some(true);
        physics::seed_track_progress(state, &gs.track_config);
    }
    for _ in 0..(HZ as f32 * seconds) as usize {
        let now = input(&gs.session.participants[&PLAYER]);
        let inputs: HashMap<PlayerId, PlayerInputData> = [(PLAYER, now)].into();
        gs.tick(&inputs);
    }
    read(&gs.session.participants[&PLAYER], (x, y))
}

fn camber(clicks: i8) -> CarSetup {
    CarSetup {
        camber_front: clicks,
        camber_rear: clicks,
        ..Default::default()
    }
}

#[test]
fn more_camber_brakes_later_to_a_stop() {
    let brake = |_: &CarState| PlayerInputData {
        brake: 1.0,
        ..Default::default()
    };
    // Metres to a stop from 40 m/s under full braking with ABS.
    let travelled = |s: &CarState, (x0, y0): (f32, f32)| {
        ((s.pos_x - x0).powi(2) + (s.pos_y - y0).powi(2)).sqrt()
    };
    let filed = drive(CarSetup::default(), 40.0, brake, 4.0, travelled);
    let more = drive(camber(5), 40.0, brake, 4.0, travelled);
    println!("stopping from 144 km/h: {filed:.2} m filed, {more:.2} m with 1.25° more camber");
    assert!(more > filed + 0.2, "{more:.2} against {filed:.2}");
}

#[test]
#[ignore = "needs Monza's baked sidecars, which CI does not have (gitignored): on the bare centerline the circle leaves the road"]
fn camber_away_from_the_best_corners_worse() {
    // A steady circle at the grip limit: full lock at 30 m/s on the
    // throttle that holds the speed; the car's lateral acceleration after
    // three seconds is what the tyres will give it.
    let circle = |s: &CarState| PlayerInputData {
        steering: 0.35,
        throttle: if s.speed_mps < 30.0 { 0.6 } else { 0.2 },
        ..Default::default()
    };
    let lateral = |s: &CarState, _| s.g_forces.lateral_g.abs();
    let less = drive(camber(-5), 30.0, circle, 3.0, lateral);
    let filed = drive(CarSetup::default(), 30.0, circle, 3.0, lateral);
    let more = drive(camber(5), 30.0, circle, 3.0, lateral);
    println!("cornering at the limit: {less:.3} g at -2.25°, {filed:.3} g filed (-3.5°), {more:.3} g at -4.75°");
    assert!(
        more < filed,
        "past the best camber, more is worse: {more:.3} {filed:.3}"
    );
    assert!(less > more, "{less:.3} {more:.3}");
}

#[test]
fn rear_toe_in_costs_speed_on_the_straight() {
    let flat_out = |_: &CarState| PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let speed = |s: &CarState, _| s.speed_mps;
    let filed = drive(CarSetup::default(), 40.0, flat_out, 4.0, speed);
    let toed = drive(
        CarSetup {
            toe_rear: 5,
            ..Default::default()
        },
        40.0,
        flat_out,
        4.0,
        speed,
    );
    println!("4 s flat out from 144 km/h: {filed:.2} m/s filed (0.2° rear toe-in), {toed:.2} m/s at 0.45°");
    assert!(toed < filed, "{toed:.2} against {filed:.2}");
}
