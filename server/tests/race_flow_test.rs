//! Full race flow driven directly through GameSession (no network, no
//! real-time waits): countdown → automatic transition to Race → 4 AI drivers
//! complete a 2-lap race at Monza → finish positions assigned → session
//! Finished.
//!
//! This is the key acceptance test for the AI corner-speed planning: an AI
//! that cannot brake for corners never completes a Monza lap.

use std::collections::HashMap;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn fixed_uuid(n: u128) -> Uuid {
    Uuid::from_u128(n)
}

fn format_lap_time(ms: u32) -> String {
    format!("{}:{:06.3}", ms / 60_000, (ms % 60_000) as f32 / 1000.0)
}

/// Race distance the way the client ranks cars (`ApexRace::RaceDistanceM`):
/// a lap-0 car in the back half of the lap is still short of the line.
fn race_distance(state: &CarState, track_length: f32) -> f32 {
    if state.current_lap == 0 && state.track_progress > track_length * 0.5 {
        return state.track_progress - track_length;
    }
    (state.current_lap.max(1) - 1) as f32 * track_length + state.track_progress
}

/// The grid sits behind the start line. At the green light the order by race
/// distance must be the grid order, and nobody behind the line may be a lap
/// up: a car used to start lap 1 on the first racing tick, which ranked the
/// back of the grid ahead of everyone who had already crossed the line.
#[test]
fn test_grid_order_holds_at_the_green_light_monza() {
    let track = TrackLoader::load_from_file("../content/tracks/real/Monza.yaml")
        .expect("failed to load Monza");
    let track_length = track.centerline.last().unwrap().distance_from_start_m;

    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let ai_profiles: Vec<AiDriverProfile> = (0..6)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Grid AI {}", i), 90);
            p.id = fixed_uuid(3000 + i as u128);
            p
        })
        .collect();
    let ai_ids: Vec<PlayerId> = ai_profiles.iter().map(|p| p.id).collect();
    let session = RaceSession::new(fixed_uuid(1), track.id, SessionKind::Multiplayer, 8, 6, 3);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);

    let ranked = |gs: &GameSession| {
        let mut cars: Vec<&CarState> = gs.session.participants.values().collect();
        cars.sort_by(|a, b| {
            race_distance(b, track_length)
                .partial_cmp(&race_distance(a, track_length))
                .unwrap()
        });
        cars.iter().map(|c| c.grid_position).collect::<Vec<u8>>()
    };

    let mut green = false;
    for _ in 0..(240 * 20) {
        let inputs: HashMap<PlayerId, PlayerInputData> = ai_ids
            .iter()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        if gs.session.game_mode != GameMode::Race {
            continue;
        }
        if !green {
            green = true;
            // Only the front row may differ (side by side on the line).
            let order = ranked(&gs);
            assert!(
                order[2..].windows(2).all(|w| w[0] < w[1]),
                "grid order must hold at the green light, got {:?}",
                order
            );
        }
        for state in gs.session.participants.values() {
            if state.current_lap >= 1 {
                assert!(
                    state.track_progress < track_length * 0.5,
                    "car P{} is on lap 1 at station {:.0} m of {:.0} m: still behind the line",
                    state.grid_position,
                    state.track_progress,
                    track_length
                );
            }
        }
        if gs.session.participants.values().all(|s| s.current_lap >= 1)
            && gs
                .session
                .participants
                .values()
                .all(|s| s.track_progress > 100.0 && s.track_progress < track_length * 0.5)
        {
            return;
        }
    }
    panic!("every car should have crossed the line within 20 s");
}

#[test]
fn test_race_flow_countdown_to_finish_monza() {
    let track = TrackLoader::load_from_file("../content/tracks/real/Monza.yaml")
        .expect("failed to load Monza");

    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    // Four AI drivers, fastest on pole (grid slots are assigned in profile
    // ID order), so the race spreads out instead of bunching up.
    let skills = [105u8, 96, 88, 80];
    let ai_profiles: Vec<AiDriverProfile> = skills
        .iter()
        .enumerate()
        .map(|(i, &skill)| {
            let mut p = AiDriverProfile::new(format!("Race AI {}", i), skill);
            p.id = fixed_uuid(2000 + i as u128);
            p
        })
        .collect();
    let ai_ids: Vec<PlayerId> = ai_profiles.iter().map(|p| p.id).collect();

    let lap_limit = 2u8;
    let session = RaceSession::new(
        fixed_uuid(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        4,
        lap_limit,
    );

    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    gs.spawn_ai_drivers();
    assert_eq!(gs.session.participants.len(), 4, "all 4 AI should spawn");

    // Countdown → Race, stored on the session and executed by tick()
    gs.start_countdown_mode(3, GameMode::Race);
    assert_eq!(gs.session.game_mode, GameMode::Countdown);
    assert_eq!(gs.session.state, SessionState::Countdown);
    assert_eq!(gs.session.next_mode, Some(GameMode::Race));

    // Cap at 30 minutes of simulated time
    let max_ticks = 240u32 * 1800;
    let mut transitioned_to_race = false;
    let mut race_ticks = 0u64;
    let mut off_track_ticks: HashMap<PlayerId, u64> = HashMap::new();

    for _ in 0..max_ticks {
        let mut inputs = HashMap::new();
        for ai_id in &ai_ids {
            inputs.insert(*ai_id, gs.generate_ai_input(ai_id));
        }
        gs.tick(&inputs);

        if gs.session.game_mode == GameMode::Race {
            transitioned_to_race = true;
            race_ticks += 1;
            for (id, state) in &gs.session.participants {
                if !state.is_on_track {
                    *off_track_ticks.entry(*id).or_default() += 1;
                }
            }
        }
        if gs.session.state == SessionState::Finished {
            break;
        }
    }

    assert!(
        transitioned_to_race,
        "countdown must auto-transition to Race"
    );
    assert_eq!(gs.session.game_mode, GameMode::Race);
    assert!(
        gs.session.race_start_tick.is_some(),
        "race start tick must be recorded"
    );
    assert_eq!(
        gs.session.state,
        SessionState::Finished,
        "race must reach Finished within the tick budget"
    );

    // Every AI must have completed the full race distance
    for ai_id in &ai_ids {
        let state = &gs.session.participants[ai_id];
        assert!(
            state.current_lap > lap_limit as u16,
            "AI {} must complete {} laps, got current_lap={}",
            ai_id,
            lap_limit,
            state.current_lap
        );
    }

    // Finish positions 1..=4, unique
    let mut positions: Vec<u8> = gs
        .session
        .participants
        .values()
        .map(|s| s.finish_position.expect("every car must be classified"))
        .collect();
    positions.sort_unstable();
    assert_eq!(
        positions,
        vec![1, 2, 3, 4],
        "positions must be unique 1..=4"
    );

    // Lap times must be plausible for Monza (~5.8km) at these AI speeds
    println!("Race result (finish position: laps, last lap, best lap):");
    for (id, state) in &gs.session.participants {
        let last = state.last_lap_time_ms.expect("finisher has a last lap");
        let best = state.best_lap_time_ms.expect("finisher has a best lap");
        let off_pct = *off_track_ticks.get(id).unwrap_or(&0) as f64 / race_ticks.max(1) as f64;
        println!(
            "  P{} {}: laps={} last={} best={} off_track={:.1}%",
            state.finish_position.unwrap(),
            id,
            state.current_lap - 1,
            format_lap_time(last),
            format_lap_time(best),
            off_pct * 100.0
        );
        for lap_ms in [last, best] {
            // Plausibility bounds for the current AI pace envelope
            // (~2:45-3:10 after the pace-tuning pass, down from ~3:50-4:30;
            // real-world reference is ~1:21, still out of reach for the
            // default road car + AI).
            assert!(
                (80_000..=230_000).contains(&lap_ms),
                "Monza lap time should be 80s-230s, got {}ms",
                lap_ms
            );
        }
        // Excursions cost time but must stay bounded. Racing in a pack
        // without a tactical avoidance layer (still missing) causes
        // contact-induced excursions on top of the ~solo baseline (fastest
        // car ~16-17%); 40% is the regression guard for the whole field.
        //
        // The guard is deliberately loose because it is bounding the wrong
        // thing: the trailing car in the pack absorbs everyone else's contact,
        // and currently sits near 32%. Tightening this is a job for the
        // avoidance layer (see REMAINING_TASKS.md), not for the threshold —
        // until then this only catches gross regressions such as a car that
        // has stopped following the racing line at all.
        assert!(
            off_pct < 0.40,
            "AI {} spent {:.1}% of the race off track (must be <40%)",
            id,
            off_pct * 100.0
        );
    }
}
