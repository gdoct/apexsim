//! Simulation determinism: two runs with identical inputs must produce
//! bit-identical car state streams. This guards the ordered participant
//! iteration (BTreeMap), the deterministic AI noise, and the absence of
//! wall-clock/env reads in the sim — and is the safety harness for
//! hot-loop performance work.

use std::collections::HashMap;

use apexsim_sim::ai_driver::AiDriverProfile;
use apexsim_sim::data::*;
use apexsim_sim::game_session::GameSession;
use apexsim_sim::track_loader::RoadContactMode;
use apexsim_sim::track_loader::TrackLoader;
use uuid::Uuid;

const TICKS: u32 = 2000;

/// Fixed IDs so both runs build identical sessions.
fn fixed_uuid(n: u128) -> Uuid {
    Uuid::from_u128(n)
}

fn build_session(track: TrackConfig) -> (GameSession, Vec<PlayerId>) {
    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    // Deterministic AI profiles: fixed IDs and identical parameters
    let ai_profiles: Vec<AiDriverProfile> = (0..4)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Det AI {}", i), 70 + i * 5);
            p.id = fixed_uuid(1000 + i as u128);
            p
        })
        .collect();
    let ai_ids: Vec<PlayerId> = ai_profiles.iter().map(|p| p.id).collect();

    let mut session = RaceSession::new(fixed_uuid(1), track.id, SessionKind::Practice, 8, 4, 2);
    session.id = fixed_uuid(2);

    let human_a = fixed_uuid(10);
    let human_b = fixed_uuid(11);

    let mut game_session = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    game_session.add_player(human_a, car.id);
    game_session.add_player(human_b, car.id);
    game_session.spawn_ai_drivers();
    game_session.set_game_mode(GameMode::FreePractice);

    let mut players = vec![human_a, human_b];
    players.extend(ai_ids);
    (game_session, players)
}

/// Scripted human input, varying per tick but fully reproducible.
fn scripted_input(tick: u32, player_index: u32) -> PlayerInputData {
    let phase = (tick + player_index * 37) as f32 * 0.013;
    PlayerInputData {
        throttle: 0.55 + 0.4 * phase.sin().abs(),
        brake: if (tick / 300) % 5 == 4 { 0.6 } else { 0.0 },
        steering: 0.25 * (phase * 0.7).sin(),
        gear: None,
        clutch: None,
        drs: false,
        headlights: None,
        flash: false,
        ers_mode: None,
        ers_boost: false,
        handbrake: false,
    }
}

/// Run the sim and fingerprint every car's full kinematic state per tick
/// as exact bit patterns.
fn run_and_fingerprint(track: TrackConfig) -> Vec<u32> {
    let (mut session, players) = build_session(track);
    let humans = &players[..2];

    let mut fingerprint = Vec::new();
    for tick in 0..TICKS {
        let mut inputs = HashMap::new();
        for (idx, player) in humans.iter().enumerate() {
            inputs.insert(*player, scripted_input(tick, idx as u32));
        }
        // AI inputs generated inside the session tick path in production go
        // through game_loop::tick; here we mimic it the same way both runs.
        for ai_id in session.session.ai_player_ids.clone() {
            let ai_input = session.generate_ai_input(&ai_id);
            inputs.insert(ai_id, ai_input);
        }
        session.tick(&inputs);

        if tick % 10 == 0 {
            // BTreeMap iteration: deterministic order
            for state in session.session.participants.values() {
                fingerprint.push(state.pos_x.to_bits());
                fingerprint.push(state.pos_y.to_bits());
                fingerprint.push(state.pos_z.to_bits());
                fingerprint.push(state.yaw_rad.to_bits());
                fingerprint.push(state.vel_x.to_bits());
                fingerprint.push(state.vel_y.to_bits());
                fingerprint.push(state.speed_mps.to_bits());
                fingerprint.push(state.angular_vel_yaw.to_bits());
                fingerprint.push(state.track_progress.to_bits());
                fingerprint.push(state.current_lap as u32);
            }
        }
    }
    fingerprint
}

/// The same, driving on Monza's road mesh: the triangle query must be as
/// pure as the centerline one. Skipped (loudly) until the sidecar has
/// been baked (`ats-export`), since it is generated, not checked in.
#[test]
fn test_simulation_is_bit_identical_across_runs_on_the_road_mesh() {
    let load = || {
        TrackLoader::load_from_file_with(
            "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
            RoadContactMode::Mesh,
        )
        .expect("failed to load Monza")
    };
    let track_a = load();
    if track_a.road_mesh.is_none() {
        eprintln!("no Monzarella.road.msgpack: bake it with ats-export to run this test");
        return;
    }
    let track_b = load();
    assert!(track_b.road_mesh.is_some());

    let run1 = run_and_fingerprint(track_a);
    let run2 = run_and_fingerprint(track_b);

    assert_eq!(run1.len(), run2.len(), "fingerprint lengths differ");
    let first_divergence = run1.iter().zip(run2.iter()).position(|(a, b)| a != b);
    assert!(
        first_divergence.is_none(),
        "mesh simulation diverged at fingerprint index {:?} of {}",
        first_divergence,
        run1.len()
    );
}

#[test]
fn test_simulation_is_bit_identical_across_runs() {
    let track_a = TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .expect("failed to load Monza");
    let track_b = TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .expect("failed to load Monza");

    let run1 = run_and_fingerprint(track_a);
    let run2 = run_and_fingerprint(track_b);

    assert_eq!(run1.len(), run2.len(), "fingerprint lengths differ");
    let first_divergence = run1.iter().zip(run2.iter()).position(|(a, b)| a != b);
    assert!(
        first_divergence.is_none(),
        "simulation diverged at fingerprint index {:?} of {}",
        first_divergence,
        run1.len()
    );
}

/// Every car's whole state, the road's and the sky's, as text: `Debug`
/// prints each float to the shortest string that reads back to its bits,
/// so two equal strings are two bit-identical states.
fn whole_state(session: &GameSession) -> String {
    format!(
        "{:?}|{:?}|{:?}|{:?}",
        session.session.participants,
        session.track_config.road_state,
        session.track_config.track_surface,
        session.sky
    )
}

/// One tick of the scripted drive, the humans' input keyed to the
/// session's own tick so a rewound session is fed what it was fed before.
fn scripted_tick(session: &mut GameSession, humans: &[PlayerId]) {
    let tick = session.session.current_tick;
    let mut inputs = HashMap::new();
    for (idx, player) in humans.iter().enumerate() {
        inputs.insert(*player, scripted_input(tick, idx as u32));
    }
    for ai_id in session.session.ai_player_ids.clone() {
        let ai_input = session.generate_ai_input(&ai_id);
        inputs.insert(ai_id, ai_input);
    }
    session.tick(&inputs);
}

/// A rewind (`GameSession::snapshot` / `restore`): run, take a snapshot,
/// run on, restore it and run the same ticks again; every tick of the
/// second run is the first's, to the bit, and a second restore of the same
/// snapshot replays them once more (running on never touched it).
#[test]
fn a_restored_snapshot_replays_the_same_ticks() {
    // On the road mesh and the walls where they are baked (the live
    // server's surface), on the centerline where they are not (CI).
    let track = TrackLoader::load_from_file_with(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
        RoadContactMode::Mesh,
    )
    .expect("failed to load Monza");
    if track.road_mesh.is_none() {
        eprintln!("no Monzarella.road.msgpack: the rewind runs on the centerline");
    }
    let (mut session, players) = build_session(track);
    let humans = players[..2].to_vec();
    for _ in 0..600 {
        scripted_tick(&mut session, &humans);
    }
    let snapshot = session.snapshot();
    assert_eq!(snapshot.tick(), session.session.current_tick);
    let at_snapshot = whole_state(&session);

    let run = |session: &mut GameSession| -> Vec<String> {
        (0..900)
            .map(|_| {
                scripted_tick(session, &humans);
                whole_state(session)
            })
            .collect()
    };
    let first = run(&mut session);
    assert_ne!(first[899], at_snapshot, "the cars moved");

    for pass in 0..2 {
        session.restore(&snapshot);
        assert_eq!(session.session.current_tick, snapshot.tick());
        assert_eq!(whole_state(&session), at_snapshot, "restored, pass {pass}");
        let again = run(&mut session);
        let diverged = first.iter().zip(&again).position(|(a, b)| a != b);
        assert!(
            diverged.is_none(),
            "pass {pass}: the rewound run diverged at tick {:?} of 900",
            diverged
        );
    }
}

/// The driver's rewind strikes every lap in progress at the point
/// rewound to, and keeps the track's lines (left out of the snapshot).
#[test]
fn a_rewind_strikes_the_laps_in_progress() {
    let track = TrackLoader::load_from_file(
        "../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
    )
    .expect("failed to load Monza");
    let points = track.centerline.len();
    let (mut session, players) = build_session(track);
    let humans = players[..2].to_vec();
    for _ in 0..200 {
        scripted_tick(&mut session, &humans);
    }
    let snapshot = session.snapshot();
    assert_eq!(session.track_config.centerline.len(), points);
    for _ in 0..200 {
        scripted_tick(&mut session, &humans);
    }
    session.rewind_to(&snapshot);
    assert_eq!(session.track_config.centerline.len(), points);
    assert!(!session.car_configs.is_empty());
    assert!(session
        .session
        .participants
        .values()
        .all(|car| car.laps.invalid));
    scripted_tick(&mut session, &humans);
}
