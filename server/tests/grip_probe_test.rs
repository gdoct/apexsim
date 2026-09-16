//! Exploratory harness (ignored): how much cornering grip a car really has
//! in the sim, and whether it holds Silverstone's first corner at the
//! racing line's own speed.
use std::path::Path;

use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::physics::{seed_track_progress, update_car_3d, AIR_DENSITY};
use apexsim_server::racing_line;
use apexsim_server::track_loader::TrackLoader;

const DT: f32 = 1.0 / 240.0;
const G: f32 = 9.81;

fn root() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}

fn car(name: &str) -> CarConfig {
    CarLoader::load_from_file(&root().join("content/cars").join(name).join("car.toml")).unwrap()
}

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

fn fresh(config: &CarConfig, x: f32, y: f32, yaw: f32, speed: f32, gear: i8) -> CarState {
    let slot = GridSlot {
        position: 1,
        x,
        y,
        z: 0.0,
        yaw_rad: yaw,
    };
    let mut s = CarState::new(uuid::Uuid::new_v4(), config.id, &slot);
    s.vel_x = speed * yaw.cos();
    s.vel_y = speed * yaw.sin();
    s.speed_mps = speed;
    s.gear = gear;
    s.auto_gearbox = true;
    s
}

/// Slowly wind on lock at a held speed and report the peak lateral g the
/// car reached against the point-mass ideal the racing line plans with.
#[test]
#[ignore]
fn skidpad_sweep() {
    let car_name = std::env::var("PROBE_CAR").unwrap_or("murcetes-amd-gt3".into());
    let config = car(&car_name);
    let track = skidpad();
    let lift = config.lift_coefficient_front + config.lift_coefficient_rear;
    let k = 0.5 * AIR_DENSITY * config.frontal_area_m2 * (-lift).max(0.0) / config.mass_kg;
    println!(
        "{} mu={} ClA={:.2}",
        config.name,
        config.tire_config.grip_coefficient,
        -lift * config.frontal_area_m2
    );
    for tc in [TractionControl::Off, TractionControl::High] {
        for speed in [20.0f32, 35.0, 50.0, 65.0] {
            let mut state = fresh(&config, 0.0, 0.0, 0.0, speed, 4);
            state.traction_control = Some(tc);
            let mut peak = 0.0f32;
            let mut peak_steer = 0.0;
            let mut at_peak_slip = (0.0, 0.0);
            for tick in 0..(240 * 12) {
                let steer = (tick as f32 / (240.0 * 12.0)).min(1.0);
                let input = PlayerInputData {
                    throttle: ((speed - state.speed_mps) * 0.5 + 0.2).clamp(0.0, 1.0),
                    steering: steer,
                    ..Default::default()
                };
                update_car_3d(&mut state, &config, &input, &track, DT);
                let g = state.g_forces.lateral_g.abs();
                if g > peak {
                    peak = g;
                    peak_steer = steer;
                    at_peak_slip = (
                        state.tires.front_left.slip_angle_rad,
                        state.tires.rear_left.slip_angle_rad,
                    );
                }
                if state.speed_mps < speed * 0.8 {
                    break;
                }
            }
            let ideal = config.tire_config.grip_coefficient * (G + k * speed * speed) / G;
            println!(
                "tc={:?} v={:>3.0} m/s  peak {:.2} g (ideal {:.2} g, {:.0}%) at steer {:.2} slip f/r {:.3}/{:.3} rad; end speed {:.1}",
                tc, speed, peak, ideal, 100.0 * peak / ideal, peak_steer, at_peak_slip.0, at_peak_slip.1, state.speed_mps
            );
        }
    }
}

fn nearest_on(points: &[[f32; 3]], x: f32, y: f32, hint: usize) -> (usize, f32) {
    let n = points.len();
    let mut best = (hint, f32::MAX);
    for d in 0..40 {
        let i = (hint + d) % n;
        let dd = (points[i][0] - x).hypot(points[i][1] - y);
        if dd < best.1 {
            best = (i, dd);
        }
    }
    best
}

/// Drive the raceline from before the first corner through it, holding the
/// racing line's speed profile times `scale`, and report the worst lateral
/// offset from the line and whether the car stayed on the road.
fn run_first_corner(
    config: &CarConfig,
    track: &TrackConfig,
    scale: f32,
    tc: TractionControl,
    from_m: f32,
    to_m: f32,
) -> String {
    let profile = racing_line::build(track, config).expect("profile");
    let pts = &profile.points;
    let sp = profile.spacing_m;
    let start_i = (from_m / sp) as usize;
    let end_i = (to_m / sp) as usize;
    let yaw0 = (pts[start_i + 1][1] - pts[start_i][1]).atan2(pts[start_i + 1][0] - pts[start_i][0]);
    let v0 = profile.speed_mps[start_i] * scale;
    let mut state = fresh(config, pts[start_i][0], pts[start_i][1], yaw0, v0, 4);
    state.pos_z = pts[start_i][2];
    state.traction_control = Some(tc);
    state.abs = Some(true);
    state.steering_assist = std::env::var("PROBE_ASSIST").is_ok();
    seed_track_progress(&mut state, track);
    let mut i = start_i;
    let mut worst_off = 0.0f32;
    let mut off_track_ticks = 0;
    let mut max_g = 0.0f32;
    let mut min_speed = f32::MAX;
    let mut log = String::new();
    let mut prev_steer = 0.0f32;
    for tick in 0..(240 * 40) {
        let (ni, dist) = nearest_on(pts, state.pos_x, state.pos_y, i);
        i = ni;
        if i >= end_i {
            break;
        }
        // Pure pursuit on a look-ahead scaled with speed.
        let look = ((state.speed_mps * 0.55).max(8.0) / sp) as usize;
        let tgt = pts[(i + look) % pts.len()];
        let dx = tgt[0] - state.pos_x;
        let dy = tgt[1] - state.pos_y;
        let heading_to = dy.atan2(dx);
        let mut err = heading_to - state.yaw_rad;
        while err > std::f32::consts::PI {
            err -= std::f32::consts::TAU;
        }
        while err < -std::f32::consts::PI {
            err += std::f32::consts::TAU;
        }
        let l = (dx * dx + dy * dy).sqrt();
        let curvature = 2.0 * err.sin() / l.max(1.0);
        let wheel = (curvature * config.wheelbase_m).atan();
        let steer_raw = if state.steering_assist {
            // The aid maps a full stick to the grip-limit turn, so the stick
            // is the share of the car's grip the corner asks for.
            let v2 = state.speed_mps * state.speed_mps;
            let lift = config.lift_coefficient_front + config.lift_coefficient_rear;
            let k = 0.5 * AIR_DENSITY * config.frontal_area_m2 * (-lift).max(0.0) / config.mass_kg;
            let available = config.tire_config.grip_coefficient * (G + k * v2);
            (curvature * v2 / available).clamp(-1.0, 1.0)
        } else {
            (wheel / config.max_steering_angle_rad).clamp(-1.0, 1.0)
        };
        // Rate-limit like a wheel in hand (full lock in 0.35 s).
        let steer = prev_steer + (steer_raw - prev_steer).clamp(-DT / 0.35, DT / 0.35);
        prev_steer = steer;
        let target_v = profile.speed_mps[(i + 2) % pts.len()] * scale;
        let dv = target_v - state.speed_mps;
        let (throttle, brake) = if dv > 0.0 {
            ((dv * 0.6 + 0.15).min(1.0), 0.0)
        } else {
            (0.0, (-dv * 0.4).min(1.0))
        };
        let input = PlayerInputData {
            throttle,
            brake,
            steering: steer,
            ..Default::default()
        };
        update_car_3d(&mut state, config, &input, track, DT);
        let ahead = pts[(i + 1) % pts.len()];
        let tx = ahead[0] - pts[i][0];
        let ty = ahead[1] - pts[i][1];
        let side = (tx * (state.pos_y - pts[i][1]) - ty * (state.pos_x - pts[i][0])).signum();
        let off = dist * side;
        if off.abs() > worst_off.abs() {
            worst_off = off;
        }
        if !state.is_on_track {
            off_track_ticks += 1;
        }
        max_g = max_g.max(state.g_forces.lateral_g.abs());
        min_speed = min_speed.min(state.speed_mps);
        if tick % 48 == 0 {
            log.push_str(&format!(
                "  s={:5.0} v={:5.1} tgt={:5.1} thr={:.2} brk={:.2} steer={:+.2} latg={:+.2} off={:+5.1} slip f/r {:+.3}/{:+.3} {}\n",
                i as f32 * sp, state.speed_mps, target_v, throttle, brake, steer, state.g_forces.lateral_g, off,
                state.tires.front_right.slip_angle_rad, state.tires.rear_right.slip_angle_rad, if state.is_on_track { "" } else { "OFF" }
            ));
        }
        if state.speed_mps < 5.0 {
            break;
        }
    }
    format!(
        "scale {:.2} tc={:?}: worst offset {:+.1} m, off-track ticks {}, max lat {:.2} g, min speed {:.1}\n{}",
        scale,
        tc,
        worst_off,
        off_track_ticks,
        max_g,
        min_speed,
        if std::env::var("PROBE_VERBOSE").is_ok() {
            log
        } else {
            String::new()
        }
    )
}

#[test]
#[ignore]
fn silverstone_first_corner() {
    let car_name = std::env::var("PROBE_CAR").unwrap_or("murcetes-amd-gt3".into());
    let config = car(&car_name);
    let track =
        TrackLoader::load_from_file(root().join("content/tracks/real/Silverstone.yaml")).unwrap();
    let profile = racing_line::build(&track, &config).unwrap();
    println!(
        "{} on {}: profile km/h s=250..600:",
        config.name, track.name
    );
    for i in (100..240).step_by(8) {
        print!(
            " {:.0}:{:.0}{:?}",
            i as f32 * profile.spacing_m,
            profile.speed_mps[i] * 3.6,
            profile.phases[i]
        );
    }
    println!();
    for tc in [
        TractionControl::Low,
        TractionControl::High,
        TractionControl::Off,
    ] {
        for scale in [1.0f32, 1.05, 1.1] {
            println!(
                "{}",
                run_first_corner(&config, &track, scale, tc, 200.0, 700.0)
            );
        }
    }
}

/// The racing line's own lap time for every shipped car on Silverstone: the
/// sim's ideal lap, to set grip levels against real lap times.
#[test]
#[ignore]
fn silverstone_profile_lap_times() {
    let track =
        TrackLoader::load_from_file(root().join("content/tracks/real/Silverstone.yaml")).unwrap();
    let dir = root().join("content/cars");
    let mut names: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().join("car.toml").exists())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    names.sort();
    let mu_scale: f32 = std::env::var("PROBE_MU_SCALE")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(1.0);
    let cla_scale: f32 = std::env::var("PROBE_CLA_SCALE")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(1.0);
    let only = std::env::var("PROBE_CLASS").ok();
    for name in names {
        let mut config = car(&name);
        if let Some(class) = &only {
            if &config.class != class {
                continue;
            }
        }
        config.tire_config.grip_coefficient *= mu_scale;
        config.lift_coefficient_front *= cla_scale;
        config.lift_coefficient_rear *= cla_scale;
        let profile = racing_line::build(&track, &config).unwrap();
        let time: f32 = profile
            .speed_mps
            .iter()
            .map(|v| profile.spacing_m / v.max(1.0))
            .sum();
        let vmax = profile.speed_mps.iter().cloned().fold(0.0, f32::max);
        let vmin = profile.speed_mps.iter().cloned().fold(f32::MAX, f32::min);
        println!(
            "{:<28} {:>2}:{:05.2}  vmax {:.0} km/h  vmin {:.0} km/h  mu {:.2} ClA {:.2}",
            config.name,
            (time / 60.0) as u32,
            time % 60.0,
            vmax * 3.6,
            vmin * 3.6,
            config.tire_config.grip_coefficient,
            -(config.lift_coefficient_front + config.lift_coefficient_rear)
                * config.frontal_area_m2
        );
    }
}
