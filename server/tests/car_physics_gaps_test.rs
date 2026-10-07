//! The car-physics and tyre gaps closed on 2026-10-07, end to end through
//! `GameSession`: a crest unloads the car, brake-by-wire takes the
//! recovered energy off the discs, a fall costs the car's ends, a low floor
//! porpoises at speed, the rear ducts cool the rears alone, the rain
//! stands in Spa's compressions and the cars dry a line at Monza, and a
//! hard hit sheds debris the next car picks up.
//!
//! Driven through `GameSession` directly, like `aero_test`.

use std::collections::HashMap;
use std::path::Path;

use apexsim_server::ai_driver::AiDriverProfile;
use apexsim_server::car_loader::CarLoader;
use apexsim_server::car_setup::CarSetup;
use apexsim_server::data::*;
use apexsim_server::game_session::GameSession;
use apexsim_server::physics;
use apexsim_server::road_state::RoadState;
use apexsim_server::track_loader::TrackLoader;
use uuid::Uuid;

const HZ: usize = apexsim_server::game_session::DEFAULT_TICK_RATE_HZ as usize;
const PLAYER: Uuid = Uuid::from_u128(100);
const OTHER: Uuid = Uuid::from_u128(101);

fn car(folder: &str) -> CarConfig {
    CarLoader::load_from_file(Path::new(&format!(
        "../content/cars/default/{folder}/car.toml"
    )))
    .expect("car loads")
}

fn monza() -> TrackConfig {
    TrackLoader::load_from_file("../content/tracks/default/Monza.yaml").expect("Monza")
}

/// A 2 km straight along +x, 10 m wide either side, with `z_at(station)`
/// for its elevation.
fn straight(z_at: impl Fn(f32) -> f32) -> TrackConfig {
    let n: usize = 500;
    let mut points: Vec<TrackPoint> = (0..n)
        .map(|i| {
            let s = i as f32 * 4.0;
            TrackPoint {
                x: s,
                y: 0.0,
                z: z_at(s),
                distance_from_start_m: s,
                width_left_m: 10.0,
                width_right_m: 10.0,
                ..Default::default()
            }
        })
        .collect();
    for i in 0..n {
        let (a, b) = (points[i.saturating_sub(1)].z, points[(i + 1).min(n - 1)].z);
        points[i].slope_rad = ((b - a) / 8.0).atan();
    }
    TrackConfig {
        centerline: points,
        start_positions: vec![GridSlot {
            position: 1,
            x: 20.0,
            y: 0.0,
            z: z_at(20.0),
            yaw_rad: 0.0,
        }],
        ..TrackConfig::default()
    }
}

/// One human in `config` on `track`, in free practice, placed at `station`
/// at `speed` with warm tyres.
fn seated(config: &CarConfig, track: TrackConfig, station: f32, speed: f32) -> GameSession {
    let mut car_configs = HashMap::new();
    car_configs.insert(config.id, config.clone());
    let session = RaceSession::new(
        Uuid::from_u128(1),
        track.id,
        SessionKind::Multiplayer,
        8,
        0,
        0,
    );
    let mut gs = GameSession::new(session, track, car_configs);
    gs.set_game_mode(GameMode::FreePractice);
    gs.add_player(PLAYER, config.id).expect("seat");
    let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, station);
    let state = gs.session.participants.get_mut(&PLAYER).expect("seated");
    state.pos_x = x;
    state.pos_y = y;
    state.pos_z = z;
    state.yaw_rad = yaw;
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state.gear = 5;
    state.auto_gearbox = true;
    state.tyres_fitted = false;
    physics::seed_track_progress(state, &gs.track_config);
    gs
}

fn drive(gs: &mut GameSession, input: PlayerInputData, ticks: usize) {
    let inputs: HashMap<PlayerId, PlayerInputData> = [(PLAYER, input)].into();
    for _ in 0..ticks {
        gs.tick(&inputs);
    }
}

fn total_load(s: &CarState) -> f32 {
    s.weight_front_left_n + s.weight_front_right_n + s.weight_rear_left_n + s.weight_rear_right_n
}

/// A hump 1.5 m high and 40 m wide (one sigma) at 600 m, taken at 50 m/s:
/// the car is light over its crest and heavy in the hollows either side;
/// on a flat road the crest model does nothing at all.
#[test]
fn a_crest_unloads_the_car_and_a_dip_loads_it() {
    let config = CarConfig::default();
    let hump = |s: f32| 1.5 * (-((s - 600.0) / 40.0).powi(2)).exp();
    let run = |track: TrackConfig| {
        let mut gs = seated(&config, track, 300.0, 50.0);
        let hold = PlayerInputData {
            throttle: 0.55,
            ..Default::default()
        };
        // Settle the filters and the loads before the hump.
        drive(&mut gs, hold, HZ / 2);
        let (mut min, mut max, mut flat_sum, mut flat_n) = (f32::MAX, f32::MIN, 0.0f32, 0.0f32);
        let mut accel = (f32::MAX, f32::MIN);
        for _ in 0..(HZ * 9) {
            drive(&mut gs, hold, 1);
            let s = &gs.session.participants[&PLAYER];
            if s.speed_mps < 5.0 {
                break;
            }
            let load = total_load(s);
            if (450.0..750.0).contains(&s.track_progress) {
                min = min.min(load);
                max = max.max(load);
            } else if s.track_progress < 450.0 {
                flat_sum += load;
                flat_n += 1.0;
            }
            accel.0 = accel.0.min(s.vertical_accel_mps2);
            accel.1 = accel.1.max(s.vertical_accel_mps2);
            if s.track_progress > 800.0 {
                break;
            }
        }
        (min, max, flat_sum / flat_n.max(1.0), accel)
    };
    let (min, max, flat, accel) = run(straight(hump));
    println!(
        "over the hump: loads {min:.0}..{max:.0} N against {flat:.0} N on the flat; vertical {:.2}..{:.2} m/s²",
        accel.0, accel.1
    );
    assert!(
        min < 0.85 * flat,
        "the crest unloads the car: {min:.0} vs {flat:.0}"
    );
    assert!(
        max > 1.05 * flat,
        "the hollow loads it: {max:.0} vs {flat:.0}"
    );
    assert!(accel.0 < -1.5 && accel.1 > 1.0, "{accel:?}");

    // A flat road: the model is bit-silent (the loads only move with the
    // downforce as the car gathers speed).
    let (min, max, flat, accel) = run(straight(|_| 0.0));
    assert_eq!(accel, (0.0, 0.0), "no curvature, no acceleration");
    assert!(
        (min - flat).abs() < 0.08 * flat && (max - flat).abs() < 0.08 * flat,
        "{min:.0}..{max:.0} against {flat:.0}"
    );
}

/// Brake-by-wire: the recovery stands in for the rear hydraulics, so the
/// rear discs run cooler for the same stop, and without it the recovery
/// brakes on top of the pedal.
#[test]
fn brake_by_wire_takes_the_recovered_energy_off_the_discs() {
    let stop = |bbw: bool| {
        let mut config = car("fugazzi-sf26");
        config.hybrid.brake_by_wire = bbw;
        let mut gs = seated(&config, monza(), 100.0, 80.0);
        {
            let s = gs.session.participants.get_mut(&PLAYER).unwrap();
            s.gear = 7;
            s.hybrid_battery_kwh = 0.4;
        }
        drive(&mut gs, PlayerInputData::default(), 2);
        let before = gs.session.participants[&PLAYER].brake_temp_c;
        let (x0, y0) = {
            let s = &gs.session.participants[&PLAYER];
            (s.pos_x, s.pos_y)
        };
        let brake = PlayerInputData {
            brake: 1.0,
            ..Default::default()
        };
        drive(&mut gs, brake, HZ * 3 / 2);
        let s = &gs.session.participants[&PLAYER];
        let distance = ((s.pos_x - x0).powi(2) + (s.pos_y - y0).powi(2)).sqrt();
        let rear_rise = 0.5 * (s.brake_temp_c[2] + s.brake_temp_c[3] - before[2] - before[3]);
        let front_rise = 0.5 * (s.brake_temp_c[0] + s.brake_temp_c[1] - before[0] - before[1]);
        (
            distance,
            s.speed_mps,
            front_rise,
            rear_rise,
            s.hybrid_battery_kwh,
        )
    };
    let with = stop(true);
    let without = stop(false);
    println!(
        "brake-by-wire: {:.1} m to {:.1} m/s, fronts +{:.0} °C, rears +{:.0} °C, battery {:.3} kWh;\n   without: {:.1} m to {:.1} m/s, fronts +{:.0} °C, rears +{:.0} °C, battery {:.3} kWh",
        with.0, with.1, with.2, with.3, with.4, without.0, without.1, without.2, without.3, without.4
    );
    assert!(with.4 > 0.4 && without.4 > 0.4, "both recover energy");
    assert!(
        with.3 < without.3 - 20.0,
        "the rear discs take less of the stop with the motor in the loop: {:.0} vs {:.0}",
        with.3,
        without.3
    );
    assert!(
        (with.2 - without.2).abs() < 0.15 * without.2.max(1.0),
        "the fronts do the same work either way"
    );
    // Without brake-by-wire the recovery brakes on top of the pedal: at
    // least as short a stop.
    assert!(
        without.0 <= with.0 + 1.0,
        "{:.1} vs {:.1}",
        without.0,
        with.0
    );
}

/// A fall onto the road costs the car's ends by the fall's speed, nothing
/// under the session's damage rule when that is off.
#[test]
fn a_landing_costs_the_ends_and_a_drop_is_free() {
    let fall = |height: f32, level: DamageLevel| {
        let config = car("posh-gt3rs");
        let mut gs = seated(&config, monza(), 100.0, 20.0);
        gs.set_damage(level);
        {
            let s = gs.session.participants.get_mut(&PLAYER).unwrap();
            s.pos_z += height;
        }
        drive(&mut gs, PlayerInputData::default(), HZ * 2);
        let d = &gs.session.participants[&PLAYER].damage;
        (
            d.front_damage_percent + d.rear_damage_percent,
            d.left_damage_percent,
        )
    };
    let (high, sides) = fall(1.5, DamageLevel::Full);
    let (low, _) = fall(0.1, DamageLevel::Full);
    let (off, _) = fall(1.5, DamageLevel::Off);
    println!(
        "a 1.5 m fall: {high:.2}% on the ends; a 10 cm drop: {low:.2}%; damage off: {off:.2}%"
    );
    assert!(high > 0.2, "{high}");
    assert_eq!(low, 0.0, "a drop the suspension takes");
    assert_eq!(off, 0.0);
    assert_eq!(sides, 0.0, "a landing is the ends' business");
}

/// An F1 run low at speed porpoises: the floor's oscillation builds; the
/// same car raised on its ride-height knobs does not.
#[test]
fn a_low_floor_porpoises_at_speed_and_a_raised_one_does_not() {
    let amp = |clicks: i8| {
        let config = car("fugazzi-sf26");
        assert!(config.aero.porpoising > 0.0, "the F1 is filed to porpoise");
        let mut gs = seated(&config, monza(), 100.0, 85.0);
        gs.set_car_setup(
            &PLAYER,
            CarSetup {
                ride_height_front: clicks,
                ride_height_rear: clicks,
                ..Default::default()
            },
        );
        gs.session.participants.get_mut(&PLAYER).unwrap().gear = 8;
        let flat = PlayerInputData {
            throttle: 1.0,
            ..Default::default()
        };
        drive(&mut gs, flat, HZ * 3);
        let s = &gs.session.participants[&PLAYER];
        (
            s.porpoise_amp,
            s.speed_mps,
            s.ride_height_front_m,
            s.ride_height_rear_m,
        )
    };
    let low = amp(-5);
    let high = amp(5);
    println!(
        "lowered: amplitude {:.3} at {:.0} m/s riding {:.0}/{:.0} mm; raised: {:.3} at {:.0} m/s riding {:.0}/{:.0} mm",
        low.0,
        low.1,
        low.2 * 1000.0,
        low.3 * 1000.0,
        high.0,
        high.1,
        high.2 * 1000.0,
        high.3 * 1000.0
    );
    assert!(low.0 > 0.04, "the lowered car porpoises: {}", low.0);
    assert!(
        high.0 < low.0 * 0.5,
        "the raised one hardly does: {}",
        high.0
    );
}

/// The rear ducts cool the rear brakes alone.
#[test]
fn the_rear_ducts_cool_the_rears_alone() {
    let stop = |rear_clicks: i8| {
        let config = car("posh-gt3rs");
        let mut gs = seated(&config, monza(), 100.0, 70.0);
        gs.set_car_setup(
            &PLAYER,
            CarSetup {
                brake_ducts_rear: rear_clicks,
                ..Default::default()
            },
        );
        {
            let s = gs.session.participants.get_mut(&PLAYER).unwrap();
            s.gear = 6;
            s.brake_temp_c = [500.0; 4];
            s.tyres_fitted = true;
        }
        // Four seconds at speed with the brakes off: the ducts' work.
        let hold = PlayerInputData {
            throttle: 0.8,
            ..Default::default()
        };
        drive(&mut gs, hold, HZ * 4);
        gs.session.participants[&PLAYER].brake_temp_c
    };
    let stock = stop(0);
    let open = stop(5);
    println!("stock {stock:?}; rear ducts +5: {open:?}");
    assert!(
        open[2] < stock[2] - 5.0 && open[3] < stock[3] - 5.0,
        "cooler rears"
    );
    assert!(
        (open[0] - stock[0]).abs() < 0.5 && (open[1] - stock[1]).abs() < 0.5,
        "the fronts untouched"
    );
}

/// Where the rain stands: Spa's compressions hold puddles, Monza's flat
/// lap holds none; and under a wet AI race at Monza the cars dry a line.
#[test]
fn the_rain_stands_in_spas_compressions_and_the_cars_dry_a_line_at_monza() {
    let wet = SessionConditions {
        weather: Weather::HeavyRain,
        ..SessionConditions::DEFAULT
    };
    let mut spa = TrackLoader::load_from_file("../content/tracks/default/Spa.yaml").expect("Spa");
    wet.apply_to_track(&mut spa);
    let field = RoadState::new(&spa, 0.5);
    println!(
        "Spa: {} puddle cells of {}",
        field.puddle_cells(),
        field.cells.len()
    );
    assert!(field.puddle_cells() > 0, "Eau Rouge holds water");
    assert!(
        field.puddle_cells() < field.cells.len() / 10,
        "most of the lap is not a puddle"
    );
    let deepest = field.cells.iter().map(|c| c.depth).fold(0.0, f32::max);
    assert!(
        deepest > 1.1,
        "a puddle stands deeper than the rain: {deepest}"
    );

    let mut monza = monza();
    wet.apply_to_track(&mut monza);
    let flat = RoadState::new(&monza, 0.5);
    assert_eq!(flat.puddle_cells(), 0, "Monza is flat");

    // Two AI laps at Monza in the rain: the line they drive dries.
    let config = car("posh-gt3rs");
    let mut car_configs = HashMap::new();
    car_configs.insert(config.id, config.clone());
    let mut profile = AiDriverProfile::new("Rain AI", 95);
    profile.id = Uuid::from_u128(7100);
    profile.preferred_car_id = Some(config.id);
    let driver = profile.id;
    let session = RaceSession::new(
        Uuid::from_u128(1),
        Uuid::nil(),
        SessionKind::Multiplayer,
        8,
        1,
        2,
    );
    let mut gs = GameSession::with_ai_profiles(session, monza, car_configs, vec![profile]);
    assert!(
        gs.track_config.road_state.is_some(),
        "a wet session builds its field"
    );
    gs.spawn_ai_drivers();
    gs.start_countdown_mode(1, GameMode::Race);
    let line_before = gs
        .track_config
        .road_state
        .as_ref()
        .unwrap()
        .water_at(1000.0, 0.0);
    let mut laps = 0;
    for _ in 0..(HZ * 60 * 6) {
        let inputs: HashMap<PlayerId, PlayerInputData> =
            [(driver, gs.generate_ai_input(&driver))].into();
        gs.tick(&inputs);
        for out in gs.take_lap_events() {
            if out.event.lap_time_ms.is_some() {
                laps += 1;
            }
        }
        if laps >= 2 {
            break;
        }
    }
    assert_eq!(laps, 2, "two wet laps");
    let field = gs.track_config.road_state.as_ref().unwrap();
    let cell = &field.cells[(1000.0 / apexsim_server::road_state::CELL_M) as usize];
    // The car dries its wheels' tracks, not the line's middle between
    // them: the driest spot within two metres of the line.
    let on_line = (-8..=8)
        .map(|k| field.water_at(1000.0, cell.line_lateral_m + 0.25 * k as f32))
        .fold(f32::MAX, f32::min);
    let off_line = field.water_at(1000.0, cell.line_lateral_m + 6.0);
    println!(
        "Monza 1000 m after two laps: on the line {on_line:.3}, 6 m off it {off_line:.3} (was {line_before:.3})"
    );
    assert!(
        on_line < line_before * 0.97,
        "the car wiped the line: {line_before} -> {on_line}"
    );
    assert!(
        on_line < off_line,
        "the line is drier than the rest of the road"
    );
    let s = &gs.session.participants[&driver];
    assert!(
        s.surface_grip_share > 0.9 && s.surface_grip_share < 1.3,
        "{}",
        s.surface_grip_share
    );
}

/// A hard hit sheds a piece onto the road; the next car over it picks it
/// up (and may puncture a tyre on it, by a deterministic hash).
#[test]
fn a_hard_hit_sheds_debris_the_next_car_picks_up() {
    let config = car("posh-gt3rs");
    let mut gs = seated(&config, monza(), 200.0, 0.0);
    gs.add_player(OTHER, config.id).expect("seat the other");
    // The first car took a hard hit on its left side.
    {
        let s = gs.session.participants.get_mut(&PLAYER).unwrap();
        s.last_hit_pct = 25.0;
        s.last_hit_angle = std::f32::consts::FRAC_PI_2;
    }
    let (hit_x, hit_y) = {
        let s = &gs.session.participants[&PLAYER];
        (s.pos_x, s.pos_y)
    };
    let idle: HashMap<PlayerId, PlayerInputData> = HashMap::new();
    gs.tick(&idle);
    assert_eq!(gs.debris.len(), 1, "one piece on the road");
    assert!(
        gs.session.participants[&PLAYER].last_hit_pct == 0.0,
        "the hit was read"
    );
    let piece = gs.debris[0];
    assert!((piece.x - hit_x).abs() < 1e-3 && (piece.y - hit_y).abs() < 1e-3);

    // The other car drives over it: its front-left wheel on the piece.
    {
        let (x, y, z, yaw) = physics::pose_at_station(&gs.track_config.centerline, 200.0);
        let s = gs.session.participants.get_mut(&OTHER).unwrap();
        let (c, sn) = (yaw.cos(), yaw.sin());
        let (lx, ly) = (config.wheelbase_m / 2.0, config.track_width_front_m / 2.0);
        s.pos_x = piece.x - (lx * c - ly * sn);
        s.pos_y = piece.y - (lx * sn + ly * c);
        s.pos_z = z;
        s.yaw_rad = yaw;
        s.vel_x = 20.0 * c;
        s.vel_y = 20.0 * sn;
        s.speed_mps = 20.0;
        s.gear = 3;
        let _ = (x, y);
    }
    // The first car's own wheels are not on the piece (it sits under the
    // car's centre), so only the other car can pick it up.
    gs.tick(&idle);
    assert!(gs.debris.is_empty(), "picked up: {:?}", gs.debris);
    let punctured = gs.session.participants[&OTHER]
        .tires
        .each()
        .iter()
        .any(|t| t.punctured || t.leak_kpa_per_s > 0.0);
    println!("the car over the debris punctured: {punctured}");
}
