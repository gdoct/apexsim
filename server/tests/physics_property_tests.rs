//! Property-based tests for the public physics API.
//!
//! These properties are deliberately invariant to tuning changes in the
//! physics internals (grip model, normal loads, etc.): they only assert
//! finiteness, non-negativity, collision-separation monotonicity, and
//! serialization roundtrips — never specific speeds, positions, or times.

use std::collections::HashMap;

use apexsim_server::data::{
    CarConfig, CarState, GridSlot, PlayerInputData, SuspensionConfig, TrackConfig,
};
use apexsim_server::network::{AuthSuccessData, ClientMessage, ServerMessage};
use apexsim_server::physics::{check_aabb_collisions_3d, update_car_3d};
use proptest::prelude::*;
use uuid::Uuid;

const DT: f32 = 1.0 / 240.0;

/// Build a car state at the given pose, referencing `config`.
fn make_car_state(config: &CarConfig, x: f32, y: f32, yaw: f32, speed: f32) -> CarState {
    let slot = GridSlot {
        position: 1,
        x,
        y,
        z: 0.0,
        yaw_rad: yaw,
    };
    let mut state = CarState::new(Uuid::new_v4(), config.id, &slot);
    state.vel_x = speed * yaw.cos();
    state.vel_y = speed * yaw.sin();
    state.speed_mps = speed;
    state
}

fn assert_all_finite(state: &CarState, context: &str) {
    let fields = [
        ("pos_x", state.pos_x),
        ("pos_y", state.pos_y),
        ("pos_z", state.pos_z),
        ("vel_x", state.vel_x),
        ("vel_y", state.vel_y),
        ("vel_z", state.vel_z),
        ("yaw_rad", state.yaw_rad),
        ("pitch_rad", state.pitch_rad),
        ("roll_rad", state.roll_rad),
        ("angular_vel_yaw", state.angular_vel_yaw),
        ("angular_vel_pitch", state.angular_vel_pitch),
        ("angular_vel_roll", state.angular_vel_roll),
        ("speed_mps", state.speed_mps),
    ];
    for (name, value) in fields {
        assert!(value.is_finite(), "{context}: {name} not finite ({value})");
    }
    assert!(
        state.speed_mps >= 0.0,
        "{context}: speed_mps negative ({})",
        state.speed_mps
    );
}

fn distance_2d(a: &CarState, b: &CarState) -> f32 {
    let dx = b.pos_x - a.pos_x;
    let dy = b.pos_y - a.pos_y;
    (dx * dx + dy * dy).sqrt()
}

/// Diagonal half-extent of a car footprint (max distance from center to a corner).
fn diagonal_half_extent(config: &CarConfig) -> f32 {
    let hl = config.length_m / 2.0;
    let hw = config.width_m / 2.0;
    (hl * hl + hw * hw).sqrt()
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    /// Property 1: after up to 100 ticks with arbitrary (clamped-range)
    /// inputs, all state fields stay finite and speed stays non-negative.
    #[test]
    fn update_car_3d_state_stays_finite(
        throttle in 0.0f32..=1.0,
        brake in 0.0f32..=1.0,
        steering in -1.0f32..=1.0,
        initial_speed in 0.0f32..=100.0,
        yaw in -std::f32::consts::PI..=std::f32::consts::PI,
        ticks in 1usize..=100,
        start_slot in 0usize..16,
    ) {
        let track = TrackConfig::default();
        let config = CarConfig::default();

        let slot = &track.start_positions[start_slot % track.start_positions.len()];
        let mut state = make_car_state(&config, slot.x, slot.y, yaw, initial_speed);
        state.pos_z = slot.z;

        let input = PlayerInputData {
            throttle,
            brake,
            steering,
            gear: None,
            clutch: None,
        };

        for tick in 0..ticks {
            update_car_3d(&mut state, &config, &input, &track, DT);
            assert_all_finite(&state, &format!("tick {tick}"));
        }
    }

    /// Property 2: overlapping cars get flagged and separation never pulls
    /// their centers closer together.
    #[test]
    fn collision_separation_never_decreases_distance(
        base_x in -50.0f32..=50.0,
        base_y in -50.0f32..=50.0,
        offset_x in -2.0f32..=2.0,
        offset_y in -2.0f32..=2.0,
        yaw_a in -std::f32::consts::PI..=std::f32::consts::PI,
        yaw_b in -std::f32::consts::PI..=std::f32::consts::PI,
    ) {
        let config = CarConfig::default();
        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        let state_a = make_car_state(&config, base_x, base_y, yaw_a, 0.0);
        let state_b = make_car_state(&config, base_x + offset_x, base_y + offset_y, yaw_b, 0.0);

        let mut states = vec![state_a, state_b];
        let distance_before = distance_2d(&states[0], &states[1]);

        check_aabb_collisions_3d(&mut states, &configs);

        // Cars whose centers are closer than twice the width half-extent
        // (the OBB inradius) must overlap regardless of yaw.
        let min_overlap_distance = config.width_m.min(config.length_m) - 0.1;
        if distance_before < min_overlap_distance {
            prop_assert!(states[0].is_colliding, "car A not flagged at distance {distance_before}");
            prop_assert!(states[1].is_colliding, "car B not flagged at distance {distance_before}");
        }

        if states[0].is_colliding {
            let distance_after = distance_2d(&states[0], &states[1]);
            prop_assert!(
                distance_after >= distance_before - 1e-4,
                "separation pulled cars together: before={distance_before}, after={distance_after}"
            );
        }
    }

    /// Property 3: cars farther apart than the sum of their diagonal
    /// half-extents are untouched by collision resolution.
    #[test]
    fn distant_cars_are_not_flagged_or_moved(
        base_x in -50.0f32..=50.0,
        base_y in -50.0f32..=50.0,
        direction in -std::f32::consts::PI..=std::f32::consts::PI,
        extra_distance in 0.1f32..=50.0,
        yaw_a in -std::f32::consts::PI..=std::f32::consts::PI,
        yaw_b in -std::f32::consts::PI..=std::f32::consts::PI,
        speed_a in 0.0f32..=100.0,
        speed_b in 0.0f32..=100.0,
    ) {
        let config = CarConfig::default();
        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        let separation = 2.0 * diagonal_half_extent(&config) + extra_distance;
        let bx = base_x + separation * direction.cos();
        let by = base_y + separation * direction.sin();

        let state_a = make_car_state(&config, base_x, base_y, yaw_a, speed_a);
        let state_b = make_car_state(&config, bx, by, yaw_b, speed_b);

        let before: Vec<[f32; 6]> = [&state_a, &state_b]
            .iter()
            .map(|s| [s.pos_x, s.pos_y, s.pos_z, s.vel_x, s.vel_y, s.vel_z])
            .collect();

        let mut states = vec![state_a, state_b];
        check_aabb_collisions_3d(&mut states, &configs);

        for (i, state) in states.iter().enumerate() {
            prop_assert!(!state.is_colliding, "distant car {i} flagged as colliding");
            let after = [
                state.pos_x, state.pos_y, state.pos_z,
                state.vel_x, state.vel_y, state.vel_z,
            ];
            // Bitwise equality: resolution must not have touched them at all.
            prop_assert_eq!(
                before[i].map(f32::to_bits),
                after.map(f32::to_bits),
                "distant car {} state changed: before={:?}, after={:?}",
                i, before[i], after
            );
        }
    }

    /// Property 4a: ClientMessage variants roundtrip through MessagePack.
    #[test]
    fn client_message_roundtrip(
        token in ".{0,32}",
        player_name in ".{0,32}",
        client_tick in any::<u32>(),
        server_tick_ack in any::<u32>(),
        throttle in 0.0f32..=1.0,
        brake in 0.0f32..=1.0,
        steering in -1.0f32..=1.0,
    ) {
        let messages = vec![
            ClientMessage::Authenticate { token, player_name },
            ClientMessage::Heartbeat { client_tick },
            ClientMessage::PlayerInput {
                server_tick_ack,
                throttle,
                brake,
                steering,
            },
        ];

        for msg in messages {
            let bytes = rmp_serde::to_vec_named(&msg).expect("serialize ClientMessage");
            let decoded: ClientMessage = rmp_serde::from_slice(&bytes).expect("deserialize ClientMessage");
            // The enums don't derive PartialEq; compare via serde_json values.
            prop_assert_eq!(
                serde_json::to_value(&msg).unwrap(),
                serde_json::to_value(&decoded).unwrap()
            );
        }
    }

    /// Property 4b: ServerMessage variants roundtrip through MessagePack.
    #[test]
    fn server_message_roundtrip(
        server_version in any::<u32>(),
        code in any::<u16>(),
        message in ".{0,64}",
        server_tick in any::<u32>(),
    ) {
        let messages = vec![
            ServerMessage::AuthSuccess(AuthSuccessData {
                player_id: Uuid::new_v4(),
                server_version,
            }),
            ServerMessage::Error { code, message },
            ServerMessage::HeartbeatAck { server_tick },
        ];

        for msg in messages {
            let bytes = rmp_serde::to_vec_named(&msg).expect("serialize ServerMessage");
            let decoded: ServerMessage = rmp_serde::from_slice(&bytes).expect("deserialize ServerMessage");
            prop_assert_eq!(
                serde_json::to_value(&msg).unwrap(),
                serde_json::to_value(&decoded).unwrap()
            );
        }
    }
}

/// Non-proptest sanity check: SuspensionConfig default is usable by tests
/// elsewhere and stays positive (guards against accidental zeroing of the
/// shared default while it is being made moddable).
#[test]
fn suspension_default_is_physical() {
    let s = SuspensionConfig::default();
    assert!(s.spring_rate_front_n_per_m > 0.0);
    assert!(s.spring_rate_rear_n_per_m > 0.0);
    assert!(s.max_travel_m > 0.0);
}
