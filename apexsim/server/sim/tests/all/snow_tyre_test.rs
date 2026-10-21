//! A studded snow tyre (`tyre_thermal::CompoundKind::Snow`) on a road baked
//! under a snow sky: the same car corners clearly harder on it than on the
//! tyre the weather gives a car without one.
use std::path::Path;

use apexsim_sim::car_loader::CarLoader;
use apexsim_sim::data::*;
use apexsim_sim::game_session::DEFAULT_TICK_RATE_HZ;
use apexsim_sim::physics::update_car_3d;
use apexsim_sim::tyre_thermal::{Compound, CompoundKind};

fn gt3() -> CarConfig {
    CarLoader::load_from_file(Path::new(
        "../../../apexsim-content/cars/default/posh-gt3rs/car.toml",
    ))
    .expect("car loads")
}

/// A flat, wide open pad baked as the server bakes a snow session.
fn snowy_pad() -> TrackConfig {
    let mut track = TrackConfig {
        centerline: (0..4000)
            .map(|i| TrackPoint {
                x: i as f32 * 2.0,
                distance_from_start_m: i as f32 * 2.0,
                width_left_m: 500.0,
                width_right_m: 500.0,
                ..Default::default()
            })
            .collect(),
        ..TrackConfig::default()
    };
    let conditions = SessionConditions {
        weather: Weather::Snow,
        ..SessionConditions::DEFAULT
    }
    .resolve(7);
    conditions.apply_to_track(&mut track);
    assert_eq!(track.track_surface.baked_snow, 1.0);
    track
}

/// The peak lateral g with the lock wound on slowly at a held 15 m/s, and
/// the compound the car was sent out on.
fn peak_lateral_g(config: &CarConfig, track: &TrackConfig) -> (f32, u8) {
    let dt = 1.0 / DEFAULT_TICK_RATE_HZ as f32;
    let speed = 15.0;
    let slot = GridSlot {
        position: 1,
        x: 0.0,
        y: 0.0,
        z: 0.0,
        yaw_rad: 0.0,
    };
    let mut state = CarState::new(uuid::Uuid::from_u128(1), config.id, &slot);
    state.vel_x = speed;
    state.speed_mps = speed;
    state.gear = 2;
    state.auto_gearbox = true;
    let ticks = (DEFAULT_TICK_RATE_HZ * 8) as usize;
    let mut peak = 0.0f32;
    for tick in 0..ticks {
        let input = PlayerInputData {
            throttle: ((speed - state.speed_mps) * 0.5 + 0.15).clamp(0.0, 1.0),
            steering: (tick as f32 / ticks as f32).min(1.0) * 0.6,
            ..Default::default()
        };
        update_car_3d(&mut state, config, &input, track, dt);
        peak = peak.max(state.g_forces.lateral_g.abs());
        if state.speed_mps < speed * 0.7 {
            break;
        }
    }
    (peak, state.tyre_compound)
}

#[test]
fn a_snow_tyre_corners_harder_on_a_covered_road() {
    let track = snowy_pad();
    let plain = gt3();
    let mut studded = plain.clone();
    let mut list = plain.tire_config.compounds().to_vec();
    list.push(Compound {
        name: "studs".into(),
        kind: CompoundKind::Snow,
        grip_scale: 1.0,
        wear_scale: 1.5,
        optimal_shift_c: -40.0,
        water_grip: [0.8, 0.9, 0.85],
        reference: false,
        snow_grip: 2.0,
        ice_grip: 1.0,
        bare_wear: 1.0,
    });
    let studs = list.len() as u8 - 1;
    studded.tire_config.compounds = list;

    let (plain_g, plain_on) = peak_lateral_g(&plain, &track);
    let (studded_g, studded_on) = peak_lateral_g(&studded, &track);
    println!(
        "covered road at 15 m/s: weather's tyre ({plain_on}) {plain_g:.3} g, studs ({studded_on}) {studded_g:.3} g"
    );
    assert_ne!(
        plain.tire_config.compound(plain_on).kind,
        CompoundKind::Snow
    );
    assert_eq!(studded_on, studs, "the snow sky's tyre for a car with one");
    assert!(
        studded_g > plain_g * 1.4,
        "studs {studded_g:.3} g against {plain_g:.3} g"
    );
}
