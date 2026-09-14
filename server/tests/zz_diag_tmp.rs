use apexsim_server::ai_driver::generate_default_ai_profiles;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::track_loader::TrackLoader;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
fn repo(p: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(p)
}
fn body_slip(s: &CarState) -> f32 {
    if s.speed_mps < 2.0 {
        return 0.0;
    }
    let (c, sn) = (s.yaw_rad.cos(), s.yaw_rad.sin());
    (-s.vel_x * sn + s.vel_y * c).atan2((s.vel_x * c + s.vel_y * sn).abs())
}
#[test]
#[ignore]
fn diag() {
    let track_name = std::env::var("T").unwrap_or("Norisring".into());
    let car_name = std::env::var("C").unwrap_or("posh-911gt3".into());
    let n: u8 = std::env::var("N")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(2);
    let track =
        TrackLoader::load_from_file(repo(&format!("content/tracks/real/{track_name}.yaml")))
            .unwrap();
    let car =
        CarLoader::load_from_file(&repo(&format!("content/cars/{car_name}/car.toml"))).unwrap();
    let cid = car.id;
    let mut cars = HashMap::new();
    cars.insert(cid, car);
    let mut profiles = generate_default_ai_profiles(n);
    for p in &mut profiles {
        p.preferred_car_id = Some(cid);
    }
    let session = RaceSession::new(uuid::Uuid::new_v4(), track.id, SessionKind::Demo, n, n, 3);
    let mut gs = GameSession::with_ai_profiles(session, track, cars, profiles);
    gs.set_tick_rate(240);
    gs.spawn_ai_drivers();
    gs.set_game_mode(GameMode::Race);
    let prof = apexsim_server::racing_line::build(
        &gs.track_config,
        gs.car_configs.values().next().unwrap(),
    )
    .unwrap();
    let prof_at = |x: f32, y: f32| -> (f32, f32) {
        let mut best = (f32::MAX, 0.0, 0.0);
        for (i, p) in prof.points.iter().enumerate() {
            let d = (p[0] - x).powi(2) + (p[1] - y).powi(2);
            if d < best.0 {
                let n = prof.points.len();
                let a = prof.points[(i + n - 3) % n];
                let c = prof.points[(i + 3) % n];
                let h0 = (p[1] - a[1]).atan2(p[0] - a[0]);
                let h1 = (c[1] - p[1]).atan2(c[0] - p[0]);
                let dh = (h1 - h0 + 3.14159).rem_euclid(6.28318) - 3.14159;
                best = (d, prof.speed_mps[i], dh / (3.0 * prof.spacing_m));
            }
        }
        (best.1, best.2)
    };
    let mut hist: HashMap<PlayerId, std::collections::VecDeque<String>> = HashMap::new();
    let mut sliding: HashMap<PlayerId, bool> = HashMap::new();
    let mut events = 0;
    for t in 0..240 * 180 {
        let inputs: HashMap<PlayerId, PlayerInputData> = gs
            .session
            .participants
            .keys()
            .map(|id| (*id, gs.generate_ai_input(id)))
            .collect();
        gs.tick(&inputs);
        for s in gs.session.participants.values() {
            let h = hist.entry(s.player_id).or_default();
            if t % 24 == 0 {
                h.push_back(format!("    prof{:5.1} k{:7.4} rear{:6.3} t{:6.1} st{:6.0} v{:5.1} slip{:6.3} yawr{:5.2} steer{:5.2} th{:4.2} br{:4.2} g{} lat{:5.1} latg{:5.2} lng{:5.2} sr[{:5.2} {:5.2} {:5.2} {:5.2}] on{}",
                    prof_at(s.pos_x, s.pos_y).0, prof_at(s.pos_x, s.pos_y).1, { let cfg=&gs.car_configs[&s.car_config_id]; let b=cfg.wheelbase_m*cfg.weight_distribution_front; let (c,sn)=(s.yaw_rad.cos(),s.yaw_rad.sin()); if s.speed_mps>3.0 {(-s.vel_x*sn+s.vel_y*c - s.angular_vel_yaw*b).atan2((s.vel_x*c+s.vel_y*sn).abs())} else {0.0} }, t as f32 / 240.0, s.track_progress, s.speed_mps, body_slip(s), s.angular_vel_yaw, s.steering_input, s.throttle_input, s.brake_input, s.gear, s.lateral_offset_m, s.g_forces.lateral_g, s.g_forces.longitudinal_g,
                    s.tires.front_left.slip_ratio, s.tires.front_right.slip_ratio, s.tires.rear_left.slip_ratio, s.tires.rear_right.slip_ratio, s.is_on_track as u8));
                if h.len() > 14 {
                    h.pop_front();
                }
            }
            let cfg = &gs.car_configs[&s.car_config_id];
            let b = cfg.wheelbase_m * cfg.weight_distribution_front;
            let (c, sn) = (s.yaw_rad.cos(), s.yaw_rad.sin());
            let fwd = s.vel_x * c + s.vel_y * sn;
            let lat = -s.vel_x * sn + s.vel_y * c;
            let rear = if s.speed_mps > 3.0 {
                (lat - s.angular_vel_yaw * b).atan2(fwd.abs())
            } else {
                0.0
            };
            let sl = rear.abs() > cfg.tire_config.optimal_slip_angle_rad && s.is_on_track;
            let was = sliding.insert(s.player_id, sl).unwrap_or(false);
            if sl && !was && events < 6 && t > 240 * 5 {
                events += 1;
                println!(
                    "  skill {} slide at t {:.1}",
                    gs.ai_profiles[&s.player_id].skill_level,
                    t as f32 / 240.0
                );
                for l in h.iter() {
                    println!("{l}");
                }
            }
        }
    }
}

#[test]
#[ignore]
fn skidpad_limits() {
    use apexsim_server::physics::{update_car_3d, AIR_DENSITY};
    let track = TrackConfig {
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
    };
    let dir = repo("content/cars");
    let mut names: Vec<_> = std::fs::read_dir(dir)
        .unwrap()
        .filter_map(|e| {
            let p = e.ok()?.path().join("car.toml");
            p.exists().then_some(p)
        })
        .collect();
    names.sort();
    for path in names {
        let cfg = CarLoader::load_from_file(&path).unwrap();
        let mut row = format!(
            "{:>28} mu {:.2} wd {:.2} ",
            cfg.name, cfg.tire_config.grip_coefficient, cfg.weight_distribution_front
        );
        for speed in [15.0f32, 30.0, 50.0, 70.0] {
            let lift = cfg.lift_coefficient_front + cfg.lift_coefficient_rear;
            let ideal = cfg.tire_config.grip_coefficient
                * (9.81
                    + 0.5 * AIR_DENSITY * cfg.frontal_area_m2 * (-lift).max(0.0) * speed * speed
                        / cfg.mass_kg);
            // Slowly ramp the steering; record best steady lateral accel before the rear drifts past peak.
            let slot = GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            };
            let mut s = CarState::new(uuid::Uuid::new_v4(), cfg.id, &slot);
            s.vel_x = speed;
            s.speed_mps = speed;
            s.gear = 3;
            s.auto_gearbox = true;
            let mut best = 0.0f32;
            let mut why = "lock";
            let b = cfg.wheelbase_m * cfg.weight_distribution_front;
            for tick in 0..(240 * 30) {
                let steer = (tick as f32 / (240.0 * 30.0)).min(1.0);
                let input = PlayerInputData {
                    throttle: ((speed - s.speed_mps) * 0.5 + 0.3).clamp(0.0, 1.0),
                    steering: steer,
                    ..Default::default()
                };
                update_car_3d(&mut s, &cfg, &input, &track, 1.0 / 240.0);
                let (c, sn) = (s.yaw_rad.cos(), s.yaw_rad.sin());
                let fwd = s.vel_x * c + s.vel_y * sn;
                let lat = -s.vel_x * sn + s.vel_y * c;
                let rear = (lat - s.angular_vel_yaw * b).atan2(fwd.abs());
                if rear.abs() > cfg.tire_config.optimal_slip_angle_rad {
                    why = "rear";
                    break;
                }
                if s.speed_mps < speed - 3.0 {
                    why = "speed";
                    break;
                }
                best = best.max(s.speed_mps * s.angular_vel_yaw.abs());
            }
            row += &format!(
                "| {speed:.0}: {:.1}/{:.1} ({:.0}%) {why} ",
                best,
                ideal,
                100.0 * best / ideal
            );
        }
        println!("{row}");
    }
}

#[test]
#[ignore]
fn sweep() {
    let combos = [
        ("IMS", "posh-911gt3"),
        ("Norisring", "posh-911gt3"),
        ("Hockenheim", "posh-911gt3"),
        ("Suzuka", "redhorse-rb20"),
        ("Spa", "redhorse-rb20"),
        ("YasMarina", "yotota-lmp2"),
        ("Suzuka", "posh-911gt3"),
        ("Monza", "redhorse-rb20"),
    ];
    let (mut toff, mut tsl, mut tw) = (0.0, 0.0, 0.0);
    for (tn, cn) in combos {
        let track =
            TrackLoader::load_from_file(repo(&format!("content/tracks/real/{tn}.yaml"))).unwrap();
        let car = CarLoader::load_from_file(&repo(&format!("content/cars/{cn}/car.toml"))).unwrap();
        let cid = car.id;
        let mut cars = HashMap::new();
        cars.insert(cid, car);
        let mut profiles = generate_default_ai_profiles(6);
        for p in &mut profiles {
            p.preferred_car_id = Some(cid);
        }
        let session = RaceSession::new(uuid::Uuid::new_v4(), track.id, SessionKind::Demo, 6, 6, 3);
        let mut gs = GameSession::with_ai_profiles(session, track, cars, profiles);
        gs.set_tick_rate(240);
        gs.spawn_ai_drivers();
        gs.set_game_mode(GameMode::Race);
        let (mut off, mut sl, mut weave, mut laps) = (0u32, 0u32, 0.0f64, 0.0);
        for _ in 0..240 * 150 {
            let inputs: HashMap<PlayerId, PlayerInputData> = gs
                .session
                .participants
                .keys()
                .map(|id| (*id, gs.generate_ai_input(id)))
                .collect();
            gs.tick(&inputs);
            for s in gs.session.participants.values() {
                let cfg = &gs.car_configs[&s.car_config_id];
                let b = cfg.wheelbase_m * cfg.weight_distribution_front;
                let (c, sn) = (s.yaw_rad.cos(), s.yaw_rad.sin());
                let fwd = s.vel_x * c + s.vel_y * sn;
                let lat = -s.vel_x * sn + s.vel_y * c;
                let rear = if s.speed_mps > 3.0 {
                    (lat - s.angular_vel_yaw * b).atan2(fwd.abs())
                } else {
                    0.0
                };
                off += !s.is_on_track as u32;
                sl += (s.is_on_track && rear.abs() > cfg.tire_config.optimal_slip_angle_rad) as u32;
                weave += (s.steering_input as f64 - 0.0).abs() * 0.0;
            }
        }
        for s in gs.session.participants.values() {
            laps += s.track_progress / 1000.0 + s.current_lap as f32 * 0.0;
        }
        let _ = weave;
        println!(
            "{tn:>12} {cn:>14}: off {:6.1} slide {:6.1} km {:.1}",
            off as f32 / 240.0,
            sl as f32 / 240.0,
            laps
        );
        toff += off as f32 / 240.0;
        tsl += sl as f32 / 240.0;
        tw += laps;
    }
    println!("TOTAL off {toff:.0} slide {tsl:.0} km {tw:.0}");
}
