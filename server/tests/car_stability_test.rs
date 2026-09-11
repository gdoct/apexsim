//! Every shipped car must be yaw-stable at speed: a steering input inside
//! the grip limit has to die away once the wheel is centred.
//!
//! Before the cars carried their own aero they all ran on the loader's
//! default downforce, and the F1s spun from a 1 s input at ~75 m/s asking
//! for 70% of the grip they had there: the rear tyres were spending most of
//! theirs on drive alone, so the rear let go first and the slide fed itself
//! (1.5 rad/s of yaw after the wheel was back in the middle). The golf cart
//! did the same until its weight went over the rear axle.

use std::path::Path;

use apexsim_server::car_loader::{drag_limited_speed_mps, CarLoader};
use apexsim_server::data::*;
use apexsim_server::physics::{update_car_3d, AIR_DENSITY};

const GRAVITY: f32 = 9.81;
const DT: f32 = 1.0 / 240.0;
/// Share of the car's grip at speed the steering step asks for.
const GRIP_DEMAND: f32 = 0.7;

/// A straight 40 km long and 4 km wide: room to wander, all asphalt.
fn skidpad() -> TrackConfig {
    TrackConfig {
        centerline: (0..20000)
            .map(|i| TrackPoint {
                x: i as f32 * 2.0,
                distance_from_start_m: i as f32 * 2.0,
                width_left_m: 2000.0,
                width_right_m: 2000.0,
                ..Default::default()
            })
            .collect(),
        ..TrackConfig::default()
    }
}

fn shipped_cars() -> Vec<CarConfig> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/cars");
    let mut cars: Vec<CarConfig> = std::fs::read_dir(dir)
        .expect("content/cars")
        .filter_map(|entry| {
            let path = entry.ok()?.path().join("car.toml");
            path.exists()
                .then(|| CarLoader::load_from_file(&path).expect("shipped car loads"))
        })
        .collect();
    cars.sort_by(|a, b| a.name.cmp(&b.name));
    assert!(!cars.is_empty(), "no cars found in content/cars");
    cars
}

/// The speed each car is tested at: fast, but short of its top speed.
fn test_speed(config: &CarConfig) -> f32 {
    (0.85 * drag_limited_speed_mps(config)).min(80.0)
}

/// Hold `steering` for `hold_s` at `speed` (throttle keeping the speed up),
/// centre the wheel, and let the car run until 3 s have passed. `Err` says
/// how it failed to settle.
fn steering_step(
    config: &CarConfig,
    speed: f32,
    steering: f32,
    hold_s: f32,
    steering_assist: bool,
) -> Result<(), String> {
    let track = skidpad();
    let slot = GridSlot {
        position: 1,
        x: 0.0,
        y: 0.0,
        z: 0.0,
        yaw_rad: 0.0,
    };
    let mut state = CarState::new(uuid::Uuid::new_v4(), config.id, &slot);
    state.vel_x = speed;
    state.speed_mps = speed;
    state.gear = 5;
    state.auto_gearbox = true;
    state.steering_assist = steering_assist;

    let hold_ticks = (hold_s / DT) as u32;
    let mut peak_in_step: f32 = 0.0;
    let mut peak_after: f32 = 0.0;
    for tick in 0..(240 * 3) {
        let held = tick < hold_ticks;
        let input = PlayerInputData {
            throttle: ((speed - state.speed_mps) * 0.5 + 0.3).clamp(0.0, 1.0),
            steering: if held { steering } else { 0.0 },
            ..Default::default()
        };
        update_car_3d(&mut state, config, &input, &track, DT);
        let rate = state.angular_vel_yaw.abs();
        if held {
            peak_in_step = peak_in_step.max(rate);
        } else {
            peak_after = peak_after.max(rate);
        }
    }

    let v_lat = -state.vel_x * state.yaw_rad.sin() + state.vel_y * state.yaw_rad.cos();
    if peak_after > peak_in_step * 1.05 {
        Err(format!(
            "{} kept rotating after the wheel was centred at {speed:.0} m/s: \
             {peak_after:.2} rad/s vs {peak_in_step:.2} while steering",
            config.name
        ))
    } else if state.angular_vel_yaw.abs() > 0.02 || v_lat.abs() > 0.5 {
        Err(format!(
            "{} did not settle after a steering step at {speed:.0} m/s: \
             yaw rate {:.3} rad/s, sliding {v_lat:.2} m/s",
            config.name, state.angular_vel_yaw
        ))
    } else {
        Ok(())
    }
}

#[test]
fn shipped_cars_settle_after_a_steering_step_at_speed() {
    let failures: Vec<String> = shipped_cars()
        .iter()
        .filter_map(|config| {
            let speed = test_speed(config);
            // A step asking for most of the grip the car has at this speed,
            // downforce included, in steady state (neutral-steer estimate:
            // yaw rate v*delta/L, lateral accel v*r).
            let lift = config.lift_coefficient_front + config.lift_coefficient_rear;
            let downforce_accel =
                0.5 * AIR_DENSITY * config.frontal_area_m2 * (-lift).max(0.0) * speed * speed
                    / config.mass_kg;
            let demand =
                GRIP_DEMAND * config.tire_config.grip_coefficient * (GRAVITY + downforce_accel);
            let wheel_angle = demand * config.wheelbase_m / (speed * speed);
            let steering = wheel_angle / config.max_steering_angle_rad;
            steering_step(config, speed, steering, 1.0, false).err()
        })
        .collect();
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn shipped_cars_survive_a_full_lock_flick_with_the_steering_aid() {
    // A pad stick slammed to the stop at speed. The aid turns it into the
    // most lock the front tyres can use, so the car turns in hard and has to
    // come back once the stick is let go, not spin.
    let failures: Vec<String> = shipped_cars()
        .iter()
        .filter_map(|config| steering_step(config, test_speed(config), 1.0, 0.5, true).err())
        .collect();
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
