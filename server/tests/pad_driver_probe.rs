//! What a pad does to the tyres: the AI's line driven with a pad's coarse
//! stick on top of it, with the speed-sensitive steering aid on or locked
//! off. `#[ignore]`d: a harness, not a pin.
//!
//! The report that built it (2026-10-02): a hotlap at Zandvoort in the
//! 994P on a pad, "170 °C for just trying to stay inside the white lines",
//! on a model whose AI peaks at 108 there. The player's profile had the
//! steering aid forbidden on the create screen; without it a pad's stick
//! is the car's whole 23° of lock at any speed, and at 150 km/h anything
//! past a third of the travel is beyond the tyre's peak slip and sliding.
//!
//! ```text
//! PAD_AID=0 PAD_NOISE=0.12 cargo test --release --test pad_driver_probe -- --ignored --nocapture
//! PAD_AID=1 PAD_NOISE=0.12 ...            # PAD_CAR, PAD_TRACK, PAD_HOLD_S, PAD_LAPS, PAD_SKILL
//! ```
//!
//! `PAD_FULL=0.3` leans the stick on its stop whenever more than 30% is
//! meant, latched per corner, which is how a pad driver takes a slow
//! corner. The AI cannot drive like that (it plans for the wheel angle it
//! asked for, and on the stop the car leaves its line and never completes
//! a lap, with the aid as it was or as it is), so that mode measures
//! nothing; the aid's slow-corner scrub is pinned by
//! `physics::tests::steering_assist_never_scrubs_the_fronts_in_a_slow_corner`
//! instead, with the stick held on the stop at 12, 20 and 30 m/s.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics::assisted_steering;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

/// The session ticks at the server default; every duration here is seconds of it.
const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;

fn env_or<T: std::str::FromStr>(key: &str, default: T) -> T {
    std::env::var(key)
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(default)
}

fn car(folder: &str) -> CarConfig {
    let shipped = format!("../content/cars/default/{folder}/car.toml");
    let custom = format!("../content/cars/custom/{folder}/car.toml");
    let path = if Path::new(&shipped).exists() {
        shipped
    } else {
        custom
    };
    CarLoader::load_from_file(Path::new(&path)).expect("car loads")
}

/// A pad's stick: the input the driver means plus an error held for a
/// reaction time, hash-based so a run repeats.
fn stick_error(tick: u64, hold_ticks: u64, amplitude: f32) -> f32 {
    let slot = tick / hold_ticks.max(1);
    let mut h = slot.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    h ^= h >> 29;
    h = h.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    h ^= h >> 32;
    let unit = (h & 0xFFFF) as f32 / 65535.0;
    (2.0 * unit - 1.0) * amplitude
}

#[test]
#[ignore]
fn pad_driver_probe() {
    let folder: String = env_or("PAD_CAR", "fugazzi-994p-hypercar".to_string());
    let stem: String = env_or("PAD_TRACK", "Zandvoort".to_string());
    let aid: u8 = env_or("PAD_AID", 0);
    let noise: f32 = env_or("PAD_NOISE", 0.12);
    let hold_s: f32 = env_or("PAD_HOLD_S", 0.2);
    let laps: u8 = env_or("PAD_LAPS", 2);
    // A pad driver who leans the stick on its stop in every corner, as pad
    // drivers do: the stick goes to ±1 whenever more than this much of it
    // is meant (0 = never).
    let full_from: f32 = env_or("PAD_FULL", 0.0);
    // A driver with margin in hand, not the AI at its limit.
    let skill: u8 = env_or("PAD_SKILL", 60);

    let car = car(&folder);
    let car_id = car.id;
    let mut car_configs = HashMap::new();
    car_configs.insert(car.id, car.clone());
    let mut profile = AiDriverProfile::new("Pad", skill);
    profile.id = Uuid::from_u128(7200);
    profile.preferred_car_id = Some(car_id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(3),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        laps,
    );
    let mut track = TrackLoader::load_from_file(format!("../content/tracks/default/{stem}.yaml"))
        .expect("track loads");
    // The report's sky: overcast, 12:15, 18 °C.
    let conditions = SessionConditions {
        weather: Weather::Overcast,
        time_of_day_minutes: 12 * 60 + 15,
        air_temp_c: Some(18),
        ..SessionConditions::DEFAULT
    };
    conditions.apply_to_track(&mut track);
    let mut gs = GameSession::with_ai_profiles(session, track, car_configs, vec![profile]);
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    gs.session
        .participants
        .get_mut(&driver)
        .unwrap()
        .steering_assist = aid != 0;

    let tire = &car.tire_config;
    let peak = tire.optimal_slip_angle_rad;
    let hold_ticks = (hold_s * HZ as f32) as u64;
    let (mut lap_count, mut tick) = (0usize, 0u64);
    // Which stop the stick is leaning on (0: none): it stays there until
    // the corner is done, as a hand does; the AI's own flicker between
    // the stops would spin any car.
    let mut on_stop = 0.0f32;
    let (mut hottest, mut past_peak_ticks, mut off_ticks, mut racing_ticks) =
        (f32::MIN, 0u32, 0u32, 0u32);
    let mut lap_times = Vec::new();
    println!(
        "{folder} at {stem}, skill {skill}, aid {}, stick error ±{noise:.2} held {hold_s} s, on the stop from {full_from}; window {:.0} ± {:.0}",
        if aid != 0 { "on" } else { "locked off" },
        tire.optimal_temperature_c,
        tire.temperature_window_c
    );
    for _ in 0..(HZ * 60 * 4 * laps as usize) {
        let mut input = gs.generate_ai_input(&driver);
        {
            let s = &gs.session.participants[&driver];
            if gs.session.game_mode == GameMode::Race {
                let meant = if aid != 0 {
                    // The AI asks for a wheel angle; through the aid the
                    // stick asks for a share of the lock the aid allows at
                    // this speed (downforce from the file's coefficients,
                    // the front axle's travel as the physics sees it).
                    let v = s.speed_mps;
                    let downforce = 0.5
                        * 1.225
                        * v
                        * v
                        * car.frontal_area_m2
                        * (car.lift_coefficient_front.abs() + car.lift_coefficient_rear.abs());
                    let (cos_yaw, sin_yaw) = (s.yaw_rad.cos(), s.yaw_rad.sin());
                    let v_long = s.vel_x * cos_yaw + s.vel_y * sin_yaw;
                    let v_lat = -s.vel_x * sin_yaw + s.vel_y * cos_yaw;
                    let front_axle_x = car.wheelbase_m * (1.0 - car.weight_distribution_front);
                    let travel = (v_lat + s.angular_vel_yaw * front_axle_x).atan2(v_long.max(0.1));
                    let share = assisted_steering(
                        &car,
                        car.mass_kg,
                        input.steering.signum().max(0.0) * 2.0 - 1.0,
                        v,
                        downforce,
                        travel,
                    )
                    .abs();
                    if share > 1e-3 {
                        input.steering / share
                    } else {
                        input.steering
                    }
                } else {
                    input.steering
                };
                if full_from > 0.0 && s.speed_mps > 8.0 {
                    if meant.abs() > full_from {
                        on_stop = meant.signum();
                    } else if meant.abs() < 0.35 * full_from || meant.signum() != on_stop {
                        on_stop = 0.0;
                    }
                } else {
                    on_stop = 0.0;
                }
                let meant = if on_stop != 0.0 { on_stop } else { meant };
                input.steering = (meant + stick_error(tick, hold_ticks, noise)).clamp(-1.0, 1.0);
            }
        }
        gs.tick(&[(driver, input)].into());
        tick += 1;
        let s = &gs.session.participants[&driver];
        if gs.session.game_mode == GameMode::Race {
            racing_ticks += 1;
            for t in s.tires.each() {
                hottest = hottest.max(t.temperature_c);
            }
            let fronts = [&s.tires.front_left, &s.tires.front_right];
            if fronts.iter().any(|t| t.slip_angle_rad.abs() > peak) {
                past_peak_ticks += 1;
            }
            off_ticks += (!s.is_on_track) as u32;
        }
        let events = gs.take_lap_events();
        let s = &gs.session.participants[&driver];
        for out in events {
            if let Some(ms) = out.event.lap_time_ms {
                lap_count += 1;
                lap_times.push(ms as f32 / 1000.0);
                let t = &s.tires;
                println!(
                    "  lap {lap_count}: {:.3} s  tread FL {:.0} FR {:.0} RL {:.0} RR {:.0}  hottest so far {hottest:.0}  fronts past peak slip {:.1} s  off track {:.1} s",
                    ms as f32 / 1000.0,
                    t.front_left.temperature_c,
                    t.front_right.temperature_c,
                    t.rear_left.temperature_c,
                    t.rear_right.temperature_c,
                    past_peak_ticks as f32 / HZ as f32,
                    off_ticks as f32 / HZ as f32
                );
            }
        }
        if lap_count >= laps as usize {
            break;
        }
    }
    println!(
        "  {:.0} s racing: hottest tread {hottest:.0} °C, fronts past peak slip {:.1} s ({:.0}%), off track {:.1} s, laps {lap_times:?}",
        racing_ticks as f32 / HZ as f32,
        past_peak_ticks as f32 / HZ as f32,
        100.0 * past_peak_ticks as f32 / racing_ticks.max(1) as f32,
        off_ticks as f32 / HZ as f32
    );
}
