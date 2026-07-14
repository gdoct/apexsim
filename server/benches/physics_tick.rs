//! Hot-loop benchmarks: the numbers that justify (or veto) physics and
//! game-loop changes. Run with `cargo bench`.
//!
//! Baselines to watch:
//! - `physics_step_monza`: one full car physics step on a real track
//! - `track_progress_monza`: the nearest-centerline scan in isolation
//! - `session_tick_8cars_monza`: a whole GameSession tick with AI drivers

use criterion::{criterion_group, criterion_main, Criterion};
use std::collections::HashMap;
use std::hint::black_box;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

const MONZA: &str = "../content/tracks/real/Monza.yaml";

fn load_monza() -> TrackConfig {
    TrackLoader::load_from_file(MONZA).expect("failed to load Monza track")
}

fn make_car_state(track: &TrackConfig, config: &CarConfig, grid_index: usize) -> CarState {
    let slot = &track.start_positions[grid_index % track.start_positions.len()];
    CarState::new(Uuid::new_v4(), config.id, slot)
}

fn bench_physics_step(c: &mut Criterion) {
    let track = load_monza();
    let config = CarConfig::default();
    let mut state = make_car_state(&track, &config, 0);
    let input = PlayerInputData {
        throttle: 0.8,
        brake: 0.0,
        steering: 0.1,
        gear: None,
        clutch: None,
    };
    let dt = 1.0 / 240.0;

    c.bench_function("physics_step_monza", |b| {
        b.iter(|| {
            physics::update_car_3d(
                black_box(&mut state),
                black_box(&config),
                black_box(&input),
                black_box(&track),
                dt,
            );
        })
    });
}

fn bench_track_progress(c: &mut Criterion) {
    let track = load_monza();
    let config = CarConfig::default();
    let mut state = make_car_state(&track, &config, 0);

    c.bench_function("track_progress_monza", |b| {
        let mut tick = 0u32;
        b.iter(|| {
            tick = tick.wrapping_add(1);
            physics::update_track_progress_3d(black_box(&mut state), black_box(&track), tick, 240);
        })
    });
}

fn bench_session_tick(c: &mut Criterion) {
    let track = load_monza();
    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let ai_profiles: Vec<AiDriverProfile> = (0..8)
        .map(|i| AiDriverProfile::new(format!("Bench AI {}", i), 80))
        .collect();

    let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 8, 2);
    let mut game_session = GameSession::with_ai_profiles(session, track, car_configs, ai_profiles);
    game_session.spawn_ai_drivers();
    game_session.set_game_mode(GameMode::FreePractice);

    let inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();

    c.bench_function("session_tick_8cars_monza", |b| {
        b.iter(|| {
            game_session.tick(black_box(&inputs));
        })
    });
}

criterion_group!(
    benches,
    bench_physics_step,
    bench_track_progress,
    bench_session_tick
);
criterion_main!(benches);
