//! Lap timing end to end: sector splits that add up to the lap the driver
//! sees, track limits that strike a lap, and the sector lines a joining
//! client is told about.
//!
//! Driven through `GameSession` directly (no network, no real-time waits),
//! like `race_flow_test`.

use std::collections::HashMap;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::laps::{self, SECTOR_COUNT};
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/real/Monza.yaml").expect("failed to load Monza")
}

/// One AI on track, counted straight into a race.
fn ai_session(track: TrackConfig, drivers: usize) -> (GameSession, Vec<PlayerId>) {
    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car);

    let profiles: Vec<AiDriverProfile> = (0..drivers)
        .map(|i| {
            let mut p = AiDriverProfile::new(format!("Timing AI {i}"), 95);
            p.id = Uuid::from_u128(5000 + i as u128);
            p
        })
        .collect();
    let ids: Vec<PlayerId> = profiles.iter().map(|p| p.id).collect();
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        drivers as u8,
        10,
    );
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, profiles);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    (gs, ids)
}

#[test]
fn sectors_are_crossed_in_order_and_add_up_to_the_lap() {
    let (mut gs, ids) = ai_session(monza(), 1);
    let driver = ids[0];

    // Collect every timing line the car crosses over two laps.
    let mut crossings = Vec::new();
    for _ in 0..(240 * 60 * 8) {
        let inputs: HashMap<PlayerId, PlayerInputData> = ids
            .iter()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        for out in gs.take_lap_events() {
            assert_eq!(out.player_id, driver);
            crossings.push(out.event);
        }
        if crossings.iter().filter(|e| e.lap_time_ms.is_some()).count() >= 2 {
            break;
        }
    }

    let laps_done = crossings.iter().filter(|e| e.lap_time_ms.is_some()).count();
    assert!(
        laps_done >= 2,
        "the AI should have completed two Monza laps, got {laps_done} ({} crossings)",
        crossings.len()
    );

    // Every lap is three sectors, crossed 0, 1, 2 — and the splits it
    // carries at the line add up to the lap time the driver was shown.
    let mut expect = 0u8;
    let mut laps_checked = 0;
    for event in &crossings {
        assert_eq!(
            event.sector, expect,
            "sectors must be crossed in order, got {event:?}"
        );
        expect = (expect + 1) % SECTOR_COUNT as u8;
        assert!(event.sector_time_ms > 0, "a sector takes time: {event:?}");
        if let Some(lap_time_ms) = event.lap_time_ms {
            assert_eq!(
                event.splits_ms.iter().sum::<u32>(),
                lap_time_ms,
                "splits must add up to the lap: {event:?}"
            );
            assert_eq!(event.sector, SECTOR_COUNT as u8 - 1);
            laps_checked += 1;
        } else {
            assert!(event.sector < SECTOR_COUNT as u8 - 1);
        }
    }
    assert!(laps_checked >= 2);

    // The second lap is the first one this car set a time on from the line,
    // so it is the personal best unless it was struck.
    let best = gs.session.participants[&driver].laps.best_lap_ms;
    let first_legal = crossings
        .iter()
        .find(|e| e.lap_time_ms.is_some() && e.valid)
        .and_then(|e| e.lap_time_ms);
    assert_eq!(best, first_legal.map(|t| best.unwrap_or(t).min(t)));
}

#[test]
fn a_car_dragged_off_the_road_loses_the_lap() {
    let (mut gs, ids) = ai_session(monza(), 1);
    let driver = ids[0];

    // Get the car racing and cleanly on track.
    for _ in 0..(240 * 30) {
        let inputs: HashMap<PlayerId, PlayerInputData> = ids
            .iter()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        let state = &gs.session.participants[&driver];
        if state.current_lap >= 1 && state.track_progress > 200.0 {
            break;
        }
    }
    let state = gs.session.participants.get_mut(&driver).unwrap();
    assert!(state.current_lap >= 1, "the car should be on a lap");
    state.laps.invalid = false;

    // Well beyond the widest Monza verge: every wheel is on the grass.
    let track = monza();
    for _ in 0..(240 * 2) {
        let state = gs.session.participants.get_mut(&driver).unwrap();
        state.pos_y += 60.0;
        state.wheels_off_track = true;
        laps::note_track_limits(state, 240);
    }
    assert!(
        gs.session.participants[&driver].laps.invalid,
        "a car two seconds off the road has lost the lap"
    );

    // And a struck lap never becomes a best, however quick it was.
    let state = gs.session.participants.get_mut(&driver).unwrap();
    let before = state.best_lap_time_ms;
    state.laps.splits_ms = [20_000, 20_000, 0];
    state.laps.sector = 2;
    state.lap_start_tick = 0;
    state.track_progress = laps::track_length_m(&track) * 0.95;
    physics::update_track_progress_3d(state, &track, 240 * 60, 240);
    assert_eq!(
        state.best_lap_time_ms, before,
        "a lap outside track limits cannot be a best"
    );
}

#[test]
fn sector_lines_are_even_thirds_unless_the_track_says_otherwise() {
    let track = monza();
    let length = laps::track_length_m(&track);
    assert!(length > 5000.0, "Monza is 5.8 km, got {length}");

    let (gs, _) = ai_session(monza(), 1);
    let boundaries = gs.sector_boundaries_m();
    assert_eq!(boundaries.len(), SECTOR_COUNT - 1);
    assert!((boundaries[0] - length / 3.0).abs() < 1.0);
    assert!((boundaries[1] - length * 2.0 / 3.0).abs() < 1.0);
    assert!(
        boundaries.windows(2).all(|w| w[0] < w[1]),
        "boundaries must be ascending"
    );
    assert!((gs.track_length_m() - length).abs() < 0.001);
}
