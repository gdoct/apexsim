//! Simulation determinism: two runs with identical inputs must produce
//! bit-identical car state streams. This guards the ordered participant
//! iteration (BTreeMap), the deterministic AI noise, and the absence of
//! wall-clock/env reads in the sim — and is the safety harness for
//! hot-loop performance work.

use std::collections::HashMap;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
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

#[test]
fn test_simulation_is_bit_identical_across_runs() {
    let track_a = TrackLoader::load_from_file("../content/tracks/real/Monza.yaml")
        .expect("failed to load Monza");
    let track_b = TrackLoader::load_from_file("../content/tracks/real/Monza.yaml")
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
