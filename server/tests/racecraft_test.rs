//! Racecraft (`apexsim_server::racecraft`): a quicker AI gets past a slower
//! one instead of queueing behind it, and a car stuck nose-first in a
//! barrier reverses out of it and rejoins.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use apexsim_server::walls::{WallSegment, Walls};

const TICK_RATE: u16 = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ;

fn repo(path: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path)
}

/// A race at Monza of these drivers, all in the GT3, gridded in the order
/// given (participants are ordered by id).
fn race(skills: &[u8], prepare: impl FnOnce(&mut TrackConfig)) -> GameSession {
    let mut track = TrackLoader::load_from_file_with(
        repo("content/tracks/default/Monza.yaml"),
        apexsim_server::config::RoadContactMode::Centerline,
    )
    .expect("track loads");
    prepare(&mut track);
    let car = CarLoader::load_from_file(&repo("content/cars/default/posh-gt3rs/car.toml"))
        .expect("car loads");
    let car_id = car.id;
    let mut cars = HashMap::new();
    cars.insert(car.id, car);
    let profiles: Vec<AiDriverProfile> = skills
        .iter()
        .enumerate()
        .map(|(i, &skill)| {
            let mut p = AiDriverProfile::new(format!("Driver {i}"), skill).with_car(car_id);
            p.id = uuid::Uuid::from_u64_pair(1, i as u64 + 1);
            p
        })
        .collect();
    let count = skills.len() as u8;
    let session = RaceSession::new(
        uuid::Uuid::from_u64_pair(9, 9),
        track.id,
        SessionKind::Demo,
        count,
        count,
        5,
    );
    let mut race = GameSession::with_ai_profiles(session, track, cars, profiles);
    race.set_tick_rate(TICK_RATE);
    race.spawn_ai_drivers();
    race.set_game_mode(GameMode::Race);
    race
}

fn tick(race: &mut GameSession) {
    let inputs: HashMap<PlayerId, PlayerInputData> = race
        .session
        .participants
        .keys()
        .map(|id| (*id, race.generate_ai_input(id)))
        .collect();
    race.tick(&inputs);
}

fn car(race: &GameSession, index: u64) -> &CarState {
    &race.session.participants[&uuid::Uuid::from_u64_pair(1, index + 1)]
}

/// How far car `a` is ahead of car `b` along the lap, m (within half a lap).
fn lead(race: &GameSession, a: u64, b: u64) -> f32 {
    let total = apexsim_server::laps::track_length_m(&race.track_config);
    let rel = car(race, a).track_progress - car(race, b).track_progress;
    (rel + 0.5 * total).rem_euclid(total) - 0.5 * total
}

/// An ace gridded behind a novice: the old traffic layer held it at a
/// following distance for as long as the novice stayed ahead. It pulls out
/// and passes, on the road and without leaning on the other car.
#[test]
fn a_quicker_driver_gets_past_a_slower_one() {
    let mut race = race(&[75, 105], |_| {});
    // Rolling, nose to tail on the middle of the road down the main
    // straight, the novice 25 m ahead: on the grid's staggered slots the
    // ace simply drove past on the run to the first corner.
    for (index, station) in [(0u64, 400.0f32), (1, 375.0)] {
        let id = uuid::Uuid::from_u64_pair(1, index + 1);
        let (x, y, z, yaw) =
            apexsim_server::physics::pose_at_station(&race.track_config.centerline, station);
        let c = race.session.participants.get_mut(&id).expect("the car");
        (c.pos_x, c.pos_y, c.pos_z, c.yaw_rad) = (x, y, z, yaw);
        (c.vel_x, c.vel_y, c.speed_mps) = (yaw.cos() * 40.0, yaw.sin() * 40.0, 40.0);
        c.gear = 4;
        c.nearest_centerline_idx = None;
        apexsim_server::physics::seed_track_progress(c, &race.track_config);
    }
    assert!(lead(&race, 0, 1) > 20.0, "the novice starts ahead");
    let mut contact = 0u32;
    let mut attacked = false;
    let mut passed_at = None;
    for t in 1..=(TICK_RATE as u32 * 60) {
        tick(&mut race);
        contact += car(&race, 1).is_colliding as u32;
        attacked |= car(&race, 1).racecraft.tactic == apexsim_server::racecraft::Tactic::Attack;
        if std::env::var("RC_DBG").is_ok() && t % (TICK_RATE as u32 / 2) == 0 {
            let (a, b) = (car(&race, 0), car(&race, 1));
            println!(
                "{:5.1} novice {:7.1} v {:4.1} | ace {:7.1} v {:4.1} {:?} held {:.1} | state {:?}",
                t as f32 / TICK_RATE as f32,
                a.track_progress,
                a.speed_mps,
                b.track_progress,
                b.speed_mps,
                b.racecraft.tactic,
                b.racecraft.held_s,
                race.session.state
            );
        }
        if passed_at.is_none() && lead(&race, 1, 0) > 10.0 {
            passed_at = Some(t);
        }
    }
    let passed_at = passed_at.expect("the ace never got past the novice");
    assert!(attacked, "the pass was made without an attack");
    assert!(
        lead(&race, 1, 0) > 0.0,
        "the novice is back ahead at the end"
    );
    let contact_s = contact as f32 / TICK_RATE as f32;
    assert!(contact_s < 1.0, "{contact_s:.1} s of contact passing");
    println!(
        "passed after {:.1} s, {contact_s:.2} s of contact",
        passed_at as f32 / TICK_RATE as f32
    );
}

/// A car parked off the road with its nose against a barrier: driving on,
/// it only pushed into it (a car pinned on a wall sat there until its
/// engine cooked). It reverses out and rejoins the race.
#[test]
fn a_car_stuck_in_a_barrier_reverses_out() {
    // A barrier along the right of Monza's main straight, 13 m off the
    // middle, and the car stood just short of it, pointing straight at it.
    const STATION_M: f32 = 600.0;
    let mut wall_at = (0.0, 0.0, 0.0, 0.0);
    let mut race = race(&[100], |track| {
        let (x, y, z, yaw) = apexsim_server::physics::pose_at_station(&track.centerline, STATION_M);
        let (rx, ry) = (yaw.sin(), -yaw.cos());
        let (fx, fy) = (yaw.cos(), yaw.sin());
        let (wx, wy) = (x + rx * 13.0, y + ry * 13.0);
        wall_at = (x, y, z, yaw);
        track.walls = Some(Walls::from_segments(vec![WallSegment {
            x0: wx - fx * 40.0,
            y0: wy - fy * 40.0,
            x1: wx + fx * 40.0,
            y1: wy + fy * 40.0,
            z: z - 1.0,
            height_m: 3.0,
            kind: 0,
        }]));
    });
    let (x, y, z, yaw) = wall_at;
    let id = uuid::Uuid::from_u64_pair(1, 1);
    {
        let c = race.session.participants.get_mut(&id).expect("the car");
        let (rx, ry) = (yaw.sin(), -yaw.cos());
        c.pos_x = x + rx * 10.5;
        c.pos_y = y + ry * 10.5;
        c.pos_z = z;
        c.yaw_rad = yaw - std::f32::consts::FRAC_PI_2;
        (c.vel_x, c.vel_y, c.speed_mps) = (0.0, 0.0, 0.0);
        c.gear = 1;
        c.nearest_centerline_idx = None;
        apexsim_server::physics::seed_track_progress(c, &race.track_config);
    }
    let mut reversed = false;
    let mut rejoined = None;
    for t in 1..=(TICK_RATE as u32 * 60) {
        tick(&mut race);
        let c = &race.session.participants[&id];
        reversed |= c.gear < 0;
        if std::env::var("RC_DBG").is_ok() && t % (TICK_RATE as u32 / 4) == 0 {
            println!(
                "{:5.1} v {:4.1} gear {} on {} hit {} stuck {:.1} rev {:.1} lat {:.1} yaw {:.2}",
                t as f32 / TICK_RATE as f32,
                c.speed_mps,
                c.gear,
                c.is_on_track,
                c.is_colliding,
                c.racecraft.stuck_s,
                c.racecraft.reverse_s,
                c.lateral_offset_m,
                c.yaw_rad
            );
        }
        if reversed && rejoined.is_none() && c.is_on_track && c.speed_mps > 15.0 {
            rejoined = Some(t);
        }
    }
    assert!(reversed, "the car never selected reverse");
    let rejoined = rejoined.expect("the car never got back on the road at speed");
    println!(
        "back racing after {:.1} s",
        rejoined as f32 / TICK_RATE as f32
    );
    assert!(race.session.participants[&id].damage.is_drivable);
}
