//! Hot-loop benchmarks: the numbers that justify (or veto) physics and
//! game-loop changes. Run with `cargo bench`.
//!
//! Baselines to watch:
//! - `physics_step_monza`: one full car physics step on a real track, a car
//!   rolling down the start straight (cloned fresh for every step)
//! - `track_progress_monza`: the nearest-centerline scan in isolation
//! - `session_tick_8cars_monza`: a whole GameSession tick with eight AI in
//!   practice, the session rebuilt every 30 s of sim so the field stays healthy
//! - `*_mesh`: the same two on Monza's road mesh (`docs/content/road-mesh.md`),
//!   when `Monza.road.msgpack` has been baked; skipped otherwise

use criterion::{criterion_group, criterion_main, BatchSize, Criterion};
use std::collections::HashMap;
use std::hint::black_box;
use std::time::{Duration, Instant};

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::config::RoadContactMode;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

const MONZA: &str = "../content/tracks/default/Monza/Monza.yaml";

fn load_monza() -> TrackConfig {
    TrackLoader::load_from_file(MONZA).expect("failed to load Monza track")
}

/// Monza driving on its road mesh, or `None` when the sidecar is not baked.
fn load_monza_mesh() -> Option<TrackConfig> {
    let track = TrackLoader::load_from_file_with(MONZA, RoadContactMode::Mesh)
        .expect("failed to load Monza track");
    if track.road_mesh.is_none() {
        eprintln!("no Monza.road.msgpack: the *_mesh benches are skipped");
        return None;
    }
    Some(track)
}

fn make_car_state(track: &TrackConfig, config: &CarConfig, grid_index: usize) -> CarState {
    let slot = &track.start_positions[grid_index % track.start_positions.len()];
    CarState::new(Uuid::new_v4(), config.id, slot)
}

fn bench_physics_step(c: &mut Criterion) {
    bench_physics_step_on(c, "physics_step_monza", load_monza());
    if let Some(track) = load_monza_mesh() {
        bench_physics_step_on(c, "physics_step_monza_mesh", track);
    }
}

fn bench_physics_step_on(c: &mut Criterion, name: &str, track: TrackConfig) {
    let config = CarConfig::default();
    let input = PlayerInputData {
        throttle: 0.8,
        brake: 0.0,
        steering: 0.1,
        gear: None,
        clutch: None,
        drs: false,
        headlights: None,
        flash: false,
        ers_mode: None,
        ers_boost: false,
    };
    let dt = 1.0 / apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as f32;

    // A car rolling down the start straight, not one parked on the grid:
    // two seconds of throttle with the wheel straight.
    let mut warm = make_car_state(&track, &config, 0);
    let straight = PlayerInputData {
        steering: 0.0,
        ..input
    };
    for _ in 0..(2 * apexsim_server::game_session::DEFAULT_TICK_RATE_HZ) {
        physics::update_car_3d(&mut warm, &config, &straight, &track, dt);
    }
    assert!(warm.damage.is_drivable && warm.speed_mps > 10.0);

    // Every measured step starts from that same car. Reusing one state
    // across millions of iterations drives it until it is out
    // (`is_drivable` false), and from then on the step returns at once.
    c.bench_function(name, |b| {
        b.iter_batched_ref(
            || warm.clone(),
            |state| {
                physics::update_car_3d(
                    black_box(state),
                    black_box(&config),
                    black_box(&input),
                    black_box(&track),
                    dt,
                );
            },
            BatchSize::SmallInput,
        )
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
            physics::update_track_progress_3d(
                black_box(&mut state),
                black_box(&track),
                tick,
                apexsim_server::game_session::DEFAULT_TICK_RATE_HZ,
            );
        })
    });
}

fn bench_session_tick(c: &mut Criterion) {
    bench_session_tick_on(c, "session_tick_8cars_monza", load_monza());
    if let Some(track) = load_monza_mesh() {
        bench_session_tick_on(c, "session_tick_8cars_monza_mesh", track);
    }
}

/// Sim ticks a benched session runs before it is built again, so the
/// measurement stays on a field of healthy cars on fresh tyres and fuel
/// rather than one worn, wrecked or retired by minutes of running.
const SESSION_RESET_TICKS: u32 = 30 * apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as u32;

/// Eight AI in free practice, two seconds in, so the field is moving.
fn fresh_session(track: &TrackConfig) -> GameSession {
    let car = CarConfig::default();
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());

    let ai_profiles: Vec<AiDriverProfile> = (0..8)
        .map(|i| AiDriverProfile::new(format!("Bench AI {}", i), 80))
        .collect();

    let session = RaceSession::new(Uuid::new_v4(), track.id, SessionKind::Practice, 8, 8, 2);
    let mut game_session =
        GameSession::with_ai_profiles(session, track.clone(), car_configs, ai_profiles);
    game_session.spawn_ai_drivers();
    game_session.set_game_mode(GameMode::FreePractice);
    let inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();
    for _ in 0..(2 * apexsim_server::game_session::DEFAULT_TICK_RATE_HZ) {
        game_session.tick(&inputs);
    }
    game_session
}

fn bench_session_tick_on(c: &mut Criterion, name: &str, track: TrackConfig) {
    let inputs: HashMap<PlayerId, PlayerInputData> = HashMap::new();
    let mut game_session = fresh_session(&track);
    let mut ticks_run = 0u32;

    c.bench_function(name, |b| {
        b.iter_custom(|iters| {
            let mut elapsed = Duration::ZERO;
            for _ in 0..iters {
                if ticks_run >= SESSION_RESET_TICKS {
                    // Rebuilt outside the timed span.
                    game_session = fresh_session(&track);
                    ticks_run = 0;
                }
                let start = Instant::now();
                game_session.tick(black_box(&inputs));
                elapsed += start.elapsed();
                ticks_run += 1;
            }
            elapsed
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
