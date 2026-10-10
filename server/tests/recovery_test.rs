//! Recovering a stuck car at Monza (`crate::recovery`): back to the track
//! where it is, or to its pit box, held for the time cost, then let go.
//!
//! Driven through `GameSession` directly, like `pit_stop_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::recovery::{self, RecoverDestination};
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza/Monza.yaml").expect("Monza")
}

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new("../content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads")
}

/// Humans on Monza in practice.
fn practice(players: u128) -> (GameSession, Vec<PlayerId>) {
    let track = monza();
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
    gs.set_game_mode(GameMode::FreePractice);
    let ids: Vec<PlayerId> = (0..players).map(|i| Uuid::from_u128(100 + i)).collect();
    for id in &ids {
        gs.add_player(*id, car.id).expect("seat");
    }
    (gs, ids)
}

/// Park `player` beside the road at `station`, nose pointing off it, as a
/// car stuck against the barrier is.
fn stuck_at(gs: &mut GameSession, player: &PlayerId, station: f32) {
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, station);
    let off = 14.0;
    let state = gs.session.participants.get_mut(player).expect("seated");
    state.pos_x = x - off * yaw.sin();
    state.pos_y = y + off * yaw.cos();
    state.pos_z = z;
    state.yaw_rad = yaw + std::f32::consts::FRAC_PI_2;
    state.vel_x = 0.0;
    state.vel_y = 0.0;
    state.speed_mps = 0.0;
    state.gear = 1;
    state.nearest_centerline_idx = None;
    physics::seed_track_progress(state, &gs.track_config);
}

/// Flat out, for every player.
fn throttle(ids: &[PlayerId]) -> HashMap<PlayerId, PlayerInputData> {
    ids.iter()
        .map(|id| {
            (
                *id,
                PlayerInputData {
                    throttle: 1.0,
                    ..Default::default()
                },
            )
        })
        .collect()
}

fn ticks(seconds: f32) -> usize {
    (seconds * HZ as f32) as usize
}

#[test]
fn a_stuck_car_is_put_back_on_the_track_where_it_is_and_held() {
    let (mut gs, ids) = practice(1);
    let me = ids[0];
    stuck_at(&mut gs, &me, 2000.0);
    let before = gs.session.participants[&me].track_progress;

    gs.recover_car(&me, RecoverDestination::Track)
        .expect("recovered");
    let s = &gs.session.participants[&me];
    let (x, y, _, yaw) = physics::pose_at_station(&gs.track_config.centerline, before);
    assert!(
        (s.pos_x - x).hypot(s.pos_y - y) < 1.0,
        "on the centerline at its own station"
    );
    assert!(
        (s.yaw_rad - yaw).sin().abs() < 0.05,
        "pointing along the lap"
    );
    assert!((s.track_progress - before).abs() < 10.0);
    assert!(s.laps.invalid, "the lap in progress is struck");
    assert!(s.recovery.is_some() && s.is_ghost());

    // Flat out through the hold: it does not move.
    let inputs = throttle(&ids);
    let (x0, y0) = (s.pos_x, s.pos_y);
    for _ in 0..ticks(recovery::TRACK_HOLD_S - 0.5) {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&me];
    assert!(s.recovery.is_some(), "still held");
    assert!((s.pos_x - x0).hypot(s.pos_y - y0) < 0.05, "held still");

    // Released with nobody about, and away it goes.
    for _ in 0..ticks(4.0) {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&me];
    assert!(s.recovery.is_none(), "released");
    assert!(s.speed_mps > 5.0, "driving off: {:.1} m/s", s.speed_mps);
    assert!(s.track_progress > before, "forwards along the lap");
}

#[test]
fn a_moving_car_cannot_be_recovered() {
    let (mut gs, ids) = practice(1);
    let me = ids[0];
    stuck_at(&mut gs, &me, 2000.0);
    gs.session.participants.get_mut(&me).unwrap().speed_mps = 20.0;
    assert!(gs.recover_car(&me, RecoverDestination::Track).is_err());
    assert!(gs.session.participants[&me].recovery.is_none());
}

#[test]
fn a_car_already_on_the_spot_is_not_landed_on() {
    let (mut gs, ids) = practice(2);
    let (me, other) = (ids[0], ids[1]);
    stuck_at(&mut gs, &me, 2000.0);
    let station = gs.session.participants[&me].track_progress;
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, station);
    {
        let o = gs.session.participants.get_mut(&other).unwrap();
        o.pos_x = x;
        o.pos_y = y;
        o.pos_z = z;
        o.yaw_rad = yaw;
    }
    gs.recover_car(&me, RecoverDestination::Track)
        .expect("recovered");
    let s = &gs.session.participants[&me];
    assert!(
        (s.pos_x - x).hypot(s.pos_y - y) >= recovery::CLEARANCE_M,
        "put down behind the car on its spot"
    );
    assert!(s.track_progress < station, "behind, never ahead");
}

#[test]
fn a_car_towed_to_its_box_is_serviced_and_driven_out() {
    let (mut gs, ids) = practice(1);
    if gs.track_config.pit_lane.is_none() {
        eprintln!("no pit sidecar for Monza: run ats-export");
        return;
    }
    let me = ids[0];
    let inputs: HashMap<PlayerId, PlayerInputData> = [(me, PlayerInputData::default())].into();
    // A tick against the lane deals the car its box.
    gs.tick(&inputs);
    stuck_at(&mut gs, &me, 3000.0);

    gs.recover_car(&me, RecoverDestination::Pits)
        .expect("towed");
    let lane = gs.track_config.pit_lane.clone().unwrap();
    let s = &gs.session.participants[&me];
    let spot = lane.box_at(s.pit.box_index.unwrap_or(0));
    assert!(
        (s.pos_x - spot.x).hypot(s.pos_y - spot.y) < 0.5,
        "at its box"
    );

    for _ in 0..ticks(recovery::PITS_HOLD_S - 1.0) {
        gs.tick(&inputs);
    }
    let s = &gs.session.participants[&me];
    assert!(!s.pit.servicing, "the crew waits for the tow's time");

    let mut serviced = false;
    for _ in 0..ticks(120.0) {
        gs.tick(&inputs);
        let s = &gs.session.participants[&me];
        serviced |= s.pit.servicing;
        if serviced && !s.pit.driving {
            break;
        }
    }
    let s = &gs.session.participants[&me];
    assert!(serviced, "serviced at the box");
    assert!(!s.pit.driving, "handed back at the lane's end");
    assert_eq!(s.pit.stops, 1);
}

#[test]
fn back_to_pits_in_qualifying_is_the_garage() {
    let (mut gs, ids) = practice(1);
    let me = ids[0];
    gs.set_game_mode(GameMode::Qualification);
    gs.hotlap_relocate(&me, HotlapDestination::Track, false)
        .expect("out");
    gs.recover_car(&me, RecoverDestination::Pits)
        .expect("garaged");
    assert!(gs.session.participants[&me].in_garage);
}
