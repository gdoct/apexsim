use crate::data::*;
use std::collections::HashMap;

/// Update car physics for one simulation tick
pub fn update_car_2d(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) {
    // 1. Longitudinal forces
    let throttle_force = input.throttle * config.max_engine_force_n;
    let brake_force = input.brake * config.max_brake_force_n;
    let drag_force = config.drag_coefficient * state.speed_mps.powi(2);
    let rolling_resistance = 100.0; // Constant N

    let net_force = throttle_force - brake_force - drag_force
        - rolling_resistance * state.speed_mps.signum();
    let accel = net_force / config.mass_kg;

    // 2. Update speed (clamp to prevent reversing)
    state.speed_mps = (state.speed_mps + accel * dt).max(0.0);

    // 3. Steering (bicycle model)
    let steering_angle = input.steering * config.max_steering_angle_rad;
    
    if steering_angle.abs() > 0.001 && state.speed_mps > 0.1 {
        let turn_radius = config.wheelbase_m / steering_angle.tan().abs().max(0.001);
        state.angular_vel_rad_s = state.speed_mps / turn_radius * steering_angle.signum();
    } else {
        state.angular_vel_rad_s = 0.0;
    }

    // 4. Apply grip limit (simplified: cap lateral accel)
    let max_lateral_accel = config.grip_coefficient * 9.81; // ~1g
    let actual_lateral_accel = state.speed_mps * state.angular_vel_rad_s.abs();
    if actual_lateral_accel > max_lateral_accel {
        state.angular_vel_rad_s *= max_lateral_accel / actual_lateral_accel;
    }

    // 5. Integrate position and orientation
    state.yaw_rad += state.angular_vel_rad_s * dt;
    state.vel_x = state.speed_mps * state.yaw_rad.cos();
    state.vel_y = state.speed_mps * state.yaw_rad.sin();
    state.pos_x += state.vel_x * dt;
    state.pos_y += state.vel_y * dt;

    // 6. Store inputs for telemetry
    state.throttle_input = input.throttle;
    state.brake_input = input.brake;
    state.steering_input = input.steering;
}

/// Check and resolve AABB collisions between cars
pub fn check_aabb_collisions(
    states: &mut [CarState],
    configs: &HashMap<CarConfigId, CarConfig>,
) {
    // Reset collision flags
    for state in states.iter_mut() {
        state.is_colliding = false;
    }

    // Check all pairs
    for i in 0..states.len() {
        for j in (i + 1)..states.len() {
            let config_i = configs.get(&states[i].car_config_id);
            let config_j = configs.get(&states[j].car_config_id);

            if let (Some(cfg_i), Some(cfg_j)) = (config_i, config_j) {
                if check_collision(&states[i], cfg_i, &states[j], cfg_j) {
                    // Mark as colliding
                    states[i].is_colliding = true;
                    states[j].is_colliding = true;

                    // Simple separation and speed reduction
                    let dx = states[j].pos_x - states[i].pos_x;
                    let dy = states[j].pos_y - states[i].pos_y;
                    let dist = (dx * dx + dy * dy).sqrt().max(0.1);

                    let separation = 0.5; // meters to push apart
                    let nx = dx / dist;
                    let ny = dy / dist;

                    states[i].pos_x -= nx * separation;
                    states[i].pos_y -= ny * separation;
                    states[j].pos_x += nx * separation;
                    states[j].pos_y += ny * separation;

                    // Reduce speed
                    states[i].speed_mps *= 0.8;
                    states[j].speed_mps *= 0.8;
                }
            }
        }
    }
}

/// Helper to check if two cars are colliding
fn check_collision(
    state_a: &CarState,
    config_a: &CarConfig,
    state_b: &CarState,
    config_b: &CarConfig,
) -> bool {
    // Simple AABB collision check (ignoring rotation for simplicity)
    let half_w_a = config_a.width_m / 2.0;
    let half_l_a = config_a.length_m / 2.0;
    let half_w_b = config_b.width_m / 2.0;
    let half_l_b = config_b.length_m / 2.0;

    let dx = (state_a.pos_x - state_b.pos_x).abs();
    let dy = (state_a.pos_y - state_b.pos_y).abs();

    dx < (half_l_a + half_l_b) && dy < (half_w_a + half_w_b)
}

/// Update track progress and detect lap completion
pub fn update_track_progress(
    state: &mut CarState,
    centerline: &[TrackPoint],
    track_length: f32,
    current_tick: u32,
) {
    if centerline.is_empty() {
        return;
    }

    // Find nearest centerline point
    let mut min_dist = f32::MAX;
    let mut nearest_idx = 0;

    for (idx, point) in centerline.iter().enumerate() {
        let dx = state.pos_x - point.x;
        let dy = state.pos_y - point.y;
        let dist = dx * dx + dy * dy;

        if dist < min_dist {
            min_dist = dist;
            nearest_idx = idx;
        }
    }

    let old_progress = state.track_progress;
    state.track_progress = centerline[nearest_idx].distance_from_start_m;

    // Detect lap completion (progress wraps)
    if state.current_lap > 0 && old_progress > track_length * 0.8 && state.track_progress < track_length * 0.2 {
        state.current_lap += 1;
        
        // Calculate lap time (at 240Hz, each tick is ~4.17ms)
        let lap_time_ms = (current_tick as f32 * 1000.0 / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);
        
        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }
    }

    // Start lap 1 when crossing the start line forward
    if state.current_lap == 0 && state.track_progress > track_length * 0.1 {
        state.current_lap = 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[test]
    fn test_update_car_acceleration() {
        let mut state = CarState {
            player_id: Uuid::new_v4(),
            car_config_id: Uuid::new_v4(),
            grid_position: 1,
            pos_x: 0.0,
            pos_y: 0.0,
            yaw_rad: 0.0,
            vel_x: 0.0,
            vel_y: 0.0,
            speed_mps: 0.0,
            angular_vel_rad_s: 0.0,
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            track_progress: 0.0,
            current_lap: 0,
            finish_position: None,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            is_colliding: false,
        };

        let config = CarConfig::default();
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
        };

        let dt = 1.0 / 240.0; // One tick at 240Hz

        update_car_2d(&mut state, &config, &input, dt);

        // Speed should increase
        assert!(state.speed_mps > 0.0);
        // Position should change
        assert!(state.pos_x > 0.0 || state.pos_y.abs() < 0.001);
    }

    #[test]
    fn test_update_car_braking() {
        let mut state = CarState {
            player_id: Uuid::new_v4(),
            car_config_id: Uuid::new_v4(),
            grid_position: 1,
            pos_x: 0.0,
            pos_y: 0.0,
            yaw_rad: 0.0,
            vel_x: 10.0,
            vel_y: 0.0,
            speed_mps: 10.0,
            angular_vel_rad_s: 0.0,
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            track_progress: 0.0,
            current_lap: 0,
            finish_position: None,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            is_colliding: false,
        };

        let config = CarConfig::default();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
        };

        let dt = 1.0 / 240.0;
        let initial_speed = state.speed_mps;

        update_car_2d(&mut state, &config, &input, dt);

        // Speed should decrease
        assert!(state.speed_mps < initial_speed);
    }

    #[test]
    fn test_update_car_steering() {
        let mut state = CarState {
            player_id: Uuid::new_v4(),
            car_config_id: Uuid::new_v4(),
            grid_position: 1,
            pos_x: 0.0,
            pos_y: 0.0,
            yaw_rad: 0.0,
            vel_x: 10.0,
            vel_y: 0.0,
            speed_mps: 10.0,
            angular_vel_rad_s: 0.0,
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            track_progress: 0.0,
            current_lap: 0,
            finish_position: None,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            is_colliding: false,
        };

        let config = CarConfig::default();
        let input = PlayerInputData {
            throttle: 0.5,
            brake: 0.0,
            steering: 0.5,
        };

        let dt = 1.0 / 240.0;

        update_car_2d(&mut state, &config, &input, dt);

        // Angular velocity should be non-zero
        assert!(state.angular_vel_rad_s != 0.0);
    }

    #[test]
    fn test_collision_detection() {
        let mut states = vec![
            CarState {
                player_id: Uuid::new_v4(),
                car_config_id: Uuid::new_v4(),
                grid_position: 1,
                pos_x: 0.0,
                pos_y: 0.0,
                yaw_rad: 0.0,
                vel_x: 0.0,
                vel_y: 0.0,
                speed_mps: 10.0,
                angular_vel_rad_s: 0.0,
                throttle_input: 0.0,
                brake_input: 0.0,
                steering_input: 0.0,
                track_progress: 0.0,
                current_lap: 0,
                finish_position: None,
                last_lap_time_ms: None,
                best_lap_time_ms: None,
                is_colliding: false,
            },
            CarState {
                player_id: Uuid::new_v4(),
                car_config_id: Uuid::new_v4(),
                grid_position: 2,
                pos_x: 1.0, // Close enough to collide
                pos_y: 0.0,
                yaw_rad: 0.0,
                vel_x: 0.0,
                vel_y: 0.0,
                speed_mps: 10.0,
                angular_vel_rad_s: 0.0,
                throttle_input: 0.0,
                brake_input: 0.0,
                steering_input: 0.0,
                track_progress: 0.0,
                current_lap: 0,
                finish_position: None,
                last_lap_time_ms: None,
                best_lap_time_ms: None,
                is_colliding: false,
            },
        ];

        let mut configs = HashMap::new();
        let config = CarConfig::default();
        configs.insert(states[0].car_config_id, config.clone());
        configs.insert(states[1].car_config_id, config);

        check_aabb_collisions(&mut states, &configs);

        // Both cars should be marked as colliding
        assert!(states[0].is_colliding);
        assert!(states[1].is_colliding);
        
        // Speed should be reduced
        assert!(states[0].speed_mps < 10.0);
        assert!(states[1].speed_mps < 10.0);
    }

    #[test]
    fn test_track_progress_update() {
        let mut state = CarState {
            player_id: Uuid::new_v4(),
            car_config_id: Uuid::new_v4(),
            grid_position: 1,
            pos_x: 10.0,
            pos_y: 0.0,
            yaw_rad: 0.0,
            vel_x: 0.0,
            vel_y: 0.0,
            speed_mps: 0.0,
            angular_vel_rad_s: 0.0,
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            track_progress: 0.0,
            current_lap: 0,
            finish_position: None,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            is_colliding: false,
        };

        let centerline = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                distance_from_start_m: 0.0,
            },
            TrackPoint {
                x: 10.0,
                y: 0.0,
                distance_from_start_m: 10.0,
            },
            TrackPoint {
                x: 20.0,
                y: 0.0,
                distance_from_start_m: 20.0,
            },
        ];

        update_track_progress(&mut state, &centerline, 20.0, 0);

        assert!(state.track_progress > 0.0);
    }
}
