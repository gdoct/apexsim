//! Progressive damage end to end on Monza's main straight: a bent left side
//! pulls the car left, a damaged engine accelerates it less, and an engine
//! run past its heat limit wears itself out.
//!
//! Driven through `GameSession` directly, like `brake_heat_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

const PLAYER: Uuid = Uuid::from_u128(100);

/// A GT3 on Monza's straight at `speed` with `damage`, driven by `input`
/// for `seconds`, then handed to `read`.
fn drive<T>(
    damage: DamageState,
    water_c: f32,
    speed: f32,
    input: PlayerInputData,
    seconds: f32,
    read: impl Fn(&CarState, (f32, f32, f32)) -> T,
) -> T {
    let config =
        CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
            .expect("car loads");
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
    let state = gs.session.participants.get_mut(&PLAYER).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = 4;
    state.auto_gearbox = true;
    state.damage = damage;
    state.water_temp_c = water_c;
    physics::seed_track_progress(state, &gs.track_config);
    let inputs: HashMap<PlayerId, PlayerInputData> = [(PLAYER, input)].into();
    for _ in 0..(240.0 * seconds) as usize {
        gs.tick(&inputs);
    }
    read(&gs.session.participants[&PLAYER], (x, y, yaw))
}

fn clean() -> DamageState {
    DamageState {
        is_drivable: true,
        ..Default::default()
    }
}

/// How far left of its starting line the car has gone, m.
fn drift(s: &CarState, (x0, y0, yaw): (f32, f32, f32)) -> f32 {
    -(s.pos_x - x0) * yaw.sin() + (s.pos_y - y0) * yaw.cos()
}

#[test]
fn a_bent_left_side_pulls_the_car_left() {
    let cruise = PlayerInputData {
        throttle: 0.4,
        ..Default::default()
    };
    let straight = drive(clean(), 90.0, 40.0, cruise, 2.0, drift);
    let bent = drive(
        DamageState {
            left_damage_percent: 60.0,
            ..clean()
        },
        90.0,
        40.0,
        cruise,
        2.0,
        drift,
    );
    println!("after 2 s hands off: {straight:.2} m clean, {bent:.2} m with the left bent");
    assert!(
        bent > straight + 2.0,
        "the car pulls toward its damaged side: {bent:.2} m against {straight:.2}"
    );
}

#[test]
fn a_damaged_engine_accelerates_less() {
    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let speed = |s: &CarState, _| s.speed_mps;
    let whole = drive(clean(), 90.0, 30.0, flat_out, 4.0, speed);
    let hurt = drive(
        DamageState {
            engine_damage_percent: 70.0,
            ..clean()
        },
        90.0,
        30.0,
        flat_out,
        4.0,
        speed,
    );
    println!(
        "4 s flat out from 108 km/h: {whole:.1} m/s whole, {hurt:.1} m/s at 70% engine damage"
    );
    assert!(
        whole - 30.0 > (hurt - 30.0) * 1.1,
        "{whole:.1} against {hurt:.1}"
    );
    assert!(hurt > 35.0, "a damaged engine still drives the car");
}

#[test]
fn an_engine_past_its_heat_limit_wears_out_and_a_blown_one_stops() {
    let idle = PlayerInputData::default();
    let (engine, drivable) = drive(clean(), 125.0, 0.0, idle, 3.0, |s, _| {
        (s.damage.engine_damage_percent, s.damage.is_drivable)
    });
    println!("3 s at 125 °C: {engine:.2}% engine damage");
    assert!(engine > 0.5, "{engine}");
    assert!(drivable);

    let flat_out = PlayerInputData {
        throttle: 1.0,
        ..Default::default()
    };
    let blown = DamageState {
        engine_damage_percent: 100.0,
        is_drivable: false,
        ..clean()
    };
    let speed = drive(blown, 90.0, 30.0, flat_out, 1.0, |s, _| s.speed_mps);
    assert!(speed <= 30.0, "a blown engine drives nothing: {speed}");
}

/// A car out of the race stands where it stopped for a few seconds, then is
/// towed to its pit box: out of the way, out of the collision passes.
#[test]
fn a_retired_car_is_towed_to_its_box() {
    let out = DamageState {
        is_drivable: false,
        engine_damage_percent: 100.0,
        ..Default::default()
    };
    let idle = PlayerInputData::default();
    let (still, towed_early) = drive(out, 90.0, 0.0, idle, 5.0, |s, (x, y, _)| {
        (
            ((s.pos_x - x).powi(2) + (s.pos_y - y).powi(2)).sqrt(),
            s.towed,
        )
    });
    assert!(
        still < 0.01 && !towed_early,
        "stands where it stopped at first"
    );
    let track = TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza");
    let Some(lane) = track.pit_lane.as_ref() else {
        return;
    };
    let (towed, at_box) = drive(
        out,
        90.0,
        0.0,
        idle,
        apexsim_server::game_session::TOW_AFTER_S + 1.0,
        |s, _| {
            let spot = lane.box_at(s.pit.box_index.unwrap_or(0));
            (
                s.towed,
                ((s.pos_x - spot.x).powi(2) + (s.pos_y - spot.y).powi(2)).sqrt(),
            )
        },
    );
    assert!(
        towed,
        "towed after {} s",
        apexsim_server::game_session::TOW_AFTER_S
    );
    assert!(at_box < 0.01, "{at_box} m from its box");
}
