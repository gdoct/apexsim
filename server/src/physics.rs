//! 3D Physics Engine for Racing Simulation
//!
//! This module implements a realistic 3D vehicle physics simulation with:
//! - Full 3D position and orientation tracking
//! - Weight transfer (longitudinal and lateral)
//! - Pacejka-inspired tire model
//! - Suspension simulation with spring/damper dynamics
//! - Aerodynamic forces (drag and downforce)
//! - Track surface interaction (grip, elevation, banking)
//! - Engine and drivetrain simulation

use crate::data::*;
use std::collections::HashMap;
use std::f32::consts::PI;
use std::sync::Once;
use tracing::{debug};
/// Gravity constant (m/s²)
const GRAVITY: f32 = 9.81;

/// Air density at sea level (kg/m³)
const AIR_DENSITY: f32 = 1.225;

/// Minimum speed threshold for calculations (m/s)
const MIN_SPEED_THRESHOLD: f32 = 0.1;

/// Height above track surface before considered airborne (m)
const AIRBORNE_HEIGHT_THRESHOLD_M: f32 = 0.05;

/// Minimum upward speed to keep airborne state (m/s)
const AIRBORNE_VERTICAL_SPEED_THRESHOLD_MPS: f32 = 0.1;

/// Extra lateral margin beyond track width where centerline elevation remains valid for airborne checks
const SURFACE_QUERY_LATERAL_MARGIN_M: f32 = 5.0;

/// Per-wheel physics state for intermediate calculations
#[derive(Debug, Clone, Copy, Default)]
pub struct WheelState {
    pub load_n: f32,           // Vertical load on this wheel
    pub slip_ratio: f32,       // Longitudinal slip
    pub slip_angle_rad: f32,   // Lateral slip angle
    pub grip_force_x: f32,     // Longitudinal grip force
    pub grip_force_y: f32,     // Lateral grip force
    pub contact_z: f32,        // Ground contact elevation
    pub suspension_compression: f32, // Current compression (m)
    pub suspension_velocity_mps: f32,
    pub spring_force_n: f32,
    pub damper_force_n: f32,
}

/// Complete intermediate physics state for a vehicle
#[derive(Debug, Clone, Default)]
pub struct VehiclePhysicsState {
    pub front_left: WheelState,
    pub front_right: WheelState,
    pub rear_left: WheelState,
    pub rear_right: WheelState,
    
    // Forces in vehicle frame
    pub total_force_x: f32,      // Forward (+) / Backward (-)
    pub total_force_y: f32,      // Left (-) / Right (+)
    pub total_force_z: f32,      // Up (+) / Down (-)
    
    // Moments
    pub yaw_moment: f32,
    pub pitch_moment: f32,
    pub roll_moment: f32,
}

/// Track context information at the car's current position
#[derive(Debug, Clone, Default)]
pub struct TrackContext {
    pub nearest_point: usize,
    pub elevation: f32,
    pub banking_rad: f32,
    pub slope_rad: f32,
    pub heading_rad: f32,
    pub lateral_offset: f32,
    pub is_on_track: bool,
    pub surface_type: SurfaceType,
    pub grip_modifier: f32,
    pub width_left: f32,
    pub width_right: f32,
}

/// Ground sample returned by the active surface query backend.
#[derive(Debug, Clone, Copy)]
struct SurfaceQuerySample {
    nearest_point: usize,
    elevation: f32,
    banking_rad: f32,
    slope_rad: f32,
    heading_rad: f32,
    lateral_offset: f32,
    width_left: f32,
    width_right: f32,
    surface_type: SurfaceType,
    grip_modifier: f32,
}

/// Heightfield data carrier for surface queries.
struct HeightfieldSurfaceData<'a> {
    heightmap: &'a crate::procgen::TerrainHeightmap,
}

/// Interface for querying elevation from a heightfield source.
trait HeightfieldSurfaceProvider {
    fn elevation_at(&self, world_x: f32, world_y: f32) -> Option<f32>;
}

impl HeightfieldSurfaceProvider for HeightfieldSurfaceData<'_> {
    fn elevation_at(&self, world_x: f32, world_y: f32) -> Option<f32> {
        if self.heightmap.width < 2 || self.heightmap.height < 2 {
            return None;
        }

        let grid_x = (world_x - self.heightmap.origin_x) / self.heightmap.cell_size_m;
        let grid_y = (world_y - self.heightmap.origin_y) / self.heightmap.cell_size_m;

        let max_x = (self.heightmap.width - 1) as f32;
        let max_y = (self.heightmap.height - 1) as f32;

        if grid_x < 0.0 || grid_y < 0.0 || grid_x >= max_x || grid_y >= max_y {
            return None;
        }

        Some(self.heightmap.sample(world_x, world_y))
    }
}

/// Active runtime surface query backend.
///
/// Current implementation uses centerline-nearest samples.
/// This seam exists so we can switch to mesh/heightfield queries later
/// without rewriting the core physics update flow.
#[derive(Debug, Clone, Copy)]
enum SurfaceQueryBackend {
    Centerline,
    MeshHeightfield,
}

static MESH_BACKEND_STUB_LOG_ONCE: Once = Once::new();

fn active_surface_query_backend() -> SurfaceQueryBackend {
    match std::env::var("APEXSIM_SURFACE_QUERY_BACKEND") {
        Ok(value) if value.eq_ignore_ascii_case("mesh")
            || value.eq_ignore_ascii_case("heightfield")
            || value.eq_ignore_ascii_case("mesh_heightfield") =>
        {
            SurfaceQueryBackend::MeshHeightfield
        }
        _ => SurfaceQueryBackend::Centerline,
    }
}

/// Update car physics for one simulation tick - main 3D physics function
pub fn update_car_3d(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    track: &TrackConfig,
    dt: f32,
) {
    // Skip physics if car is undrivable
    if !state.damage.is_drivable {
        return;
    }

    // Keep fuel capacity in sync with config (for moddable cars)
    state.fuel_capacity_liters = config.fuel.capacity_liters;
    state.fuel_liters = state.fuel_liters.min(state.fuel_capacity_liters);
    
    // 1. Get track context at current position
    let track_ctx = get_track_context(state, track);
    state.is_on_track = track_ctx.is_on_track;
    state.current_surface = track_ctx.surface_type;
    state.surface_grip_modifier = track_ctx.grip_modifier;
    state.lateral_offset_m = track_ctx.lateral_offset;

    let lateral_query_limit = track_ctx.width_left.max(track_ctx.width_right) + SURFACE_QUERY_LATERAL_MARGIN_M;
    let can_query_surface = track_ctx.lateral_offset.abs() <= lateral_query_limit;
    let mut is_airborne = if can_query_surface {
        if state.pos_z <= track_ctx.elevation {
            false
        } else if state.is_airborne {
            true
        } else {
            state.pos_z > track_ctx.elevation + AIRBORNE_HEIGHT_THRESHOLD_M
                || state.vel_z > AIRBORNE_VERTICAL_SPEED_THRESHOLD_MPS
        }
    } else {
        false
    };
    state.is_airborne = is_airborne;
    
    // 2. Calculate static weight distribution
    let total_weight = config.mass_kg * GRAVITY;
    let (static_front_weight, static_rear_weight) = calculate_static_weight_distribution(config, total_weight);
    
    // 3. Calculate aerodynamic forces
    let (drag_force, downforce_front, downforce_rear) = calculate_aerodynamic_forces(state, config);
    state.drag_force_n = drag_force;
    state.downforce_front_n = downforce_front;
    state.downforce_rear_n = downforce_rear;
    
    // 4. Calculate engine torque and RPM
    let (engine_torque, engine_rpm) = calculate_engine_output(state, config, input);
    state.engine_rpm = engine_rpm;

    // 5. Calculate wheel torques from drivetrain
    let (drive_torque_front, drive_torque_rear) = calculate_drive_torques(
        engine_torque,
        config,
        state.gear,
    );

    
    // 6. Calculate brake forces
    let brake_force = input.brake * config.max_brake_force_n;
    let brake_front = brake_force * config.brake_bias_front;
    let brake_rear = brake_force * (1.0 - config.brake_bias_front);
    
    // 7. Calculate weight transfer
    let longitudinal_accel = state.g_forces.longitudinal_g * GRAVITY;
    let lateral_accel = state.g_forces.lateral_g * GRAVITY;
    
    let (weight_transfer_long, weight_transfer_lat_front, weight_transfer_lat_rear) = 
        calculate_weight_transfer(config, longitudinal_accel, lateral_accel, total_weight);

    let prev_fl_compression = state.suspension.front_left_travel_m;
    let prev_fr_compression = state.suspension.front_right_travel_m;
    let prev_rl_compression = state.suspension.rear_left_travel_m;
    let prev_rr_compression = state.suspension.rear_right_travel_m;
    
    // 8. Calculate individual wheel loads
    let front_weight = static_front_weight + downforce_front - weight_transfer_long;
    let rear_weight = static_rear_weight + downforce_rear + weight_transfer_long;

    let suspension_rest_length_m = (config.suspension.max_travel_m * 0.5)
        .clamp(0.0, config.suspension.max_travel_m);
    let hub_z = state.pos_z + config.wheel_radius_m;

    let cos_yaw = state.yaw_rad.cos();
    let sin_yaw = state.yaw_rad.sin();
    let sample_wheel_state = |
        local_x: f32,
        local_y: f32,
        spring_rate_n_per_m: f32,
        damper_compression: f32,
        damper_rebound: f32,
        previous_compression: f32,
    | -> WheelState {
        let world_x = state.pos_x + local_x * cos_yaw - local_y * sin_yaw;
        let world_y = state.pos_y + local_x * sin_yaw + local_y * cos_yaw;

        let contact_z = query_track_surface(track, world_x, world_y)
            .map(|sample| sample.elevation)
            .unwrap_or(track_ctx.elevation);

        let wheel_extension = (hub_z - contact_z - config.wheel_radius_m).max(0.0);
        let compression = (suspension_rest_length_m - wheel_extension)
            .clamp(0.0, config.suspension.max_travel_m);

        let compression_velocity = (compression - previous_compression) / dt.max(1e-4);
        let damping_coeff = if compression_velocity >= 0.0 {
            damper_compression
        } else {
            damper_rebound
        };

        let spring_force = spring_rate_n_per_m * compression;
        let damper_force = damping_coeff * compression_velocity;

        WheelState {
            load_n: (spring_force + damper_force).max(0.0),
            contact_z,
            suspension_compression: compression,
            suspension_velocity_mps: compression_velocity,
            spring_force_n: spring_force,
            damper_force_n: damper_force,
            ..Default::default()
        }
    };

    let mut wheel_front_left = sample_wheel_state(
        config.wheelbase_m / 2.0,
        -config.track_width_front_m / 2.0,
        config.suspension.spring_rate_front_n_per_m,
        config.suspension.damper_compression_front,
        config.suspension.damper_rebound_front,
        prev_fl_compression,
    );
    let mut wheel_front_right = sample_wheel_state(
        config.wheelbase_m / 2.0,
        config.track_width_front_m / 2.0,
        config.suspension.spring_rate_front_n_per_m,
        config.suspension.damper_compression_front,
        config.suspension.damper_rebound_front,
        prev_fr_compression,
    );
    let mut wheel_rear_left = sample_wheel_state(
        -config.wheelbase_m / 2.0,
        -config.track_width_rear_m / 2.0,
        config.suspension.spring_rate_rear_n_per_m,
        config.suspension.damper_compression_rear,
        config.suspension.damper_rebound_rear,
        prev_rl_compression,
    );
    let mut wheel_rear_right = sample_wheel_state(
        -config.wheelbase_m / 2.0,
        config.track_width_rear_m / 2.0,
        config.suspension.spring_rate_rear_n_per_m,
        config.suspension.damper_compression_rear,
        config.suspension.damper_rebound_rear,
        prev_rr_compression,
    );

    let any_wheel_contact = wheel_front_left.suspension_compression > 0.001
        || wheel_front_right.suspension_compression > 0.001
        || wheel_rear_left.suspension_compression > 0.001
        || wheel_rear_right.suspension_compression > 0.001;

    if is_airborne && any_wheel_contact {
        is_airborne = false;
    } else if !is_airborne && !any_wheel_contact {
        is_airborne = true;
    }

    if is_airborne {
        state.weight_front_left_n = 0.0;
        state.weight_front_right_n = 0.0;
        state.weight_rear_left_n = 0.0;
        state.weight_rear_right_n = 0.0;
        wheel_front_left.load_n = 0.0;
        wheel_front_right.load_n = 0.0;
        wheel_rear_left.load_n = 0.0;
        wheel_rear_right.load_n = 0.0;
    } else {
        state.weight_front_left_n =
            (front_weight / 2.0 + weight_transfer_lat_front + wheel_front_left.load_n).max(0.0);
        state.weight_front_right_n =
            (front_weight / 2.0 - weight_transfer_lat_front + wheel_front_right.load_n).max(0.0);
        state.weight_rear_left_n =
            (rear_weight / 2.0 + weight_transfer_lat_rear + wheel_rear_left.load_n).max(0.0);
        state.weight_rear_right_n =
            (rear_weight / 2.0 - weight_transfer_lat_rear + wheel_rear_right.load_n).max(0.0);

        let front_anti_roll_transfer =
            config.suspension.anti_roll_bar_front
                * (wheel_front_left.suspension_compression - wheel_front_right.suspension_compression)
                * 0.5;
        state.weight_front_left_n = (state.weight_front_left_n - front_anti_roll_transfer).max(0.0);
        state.weight_front_right_n = (state.weight_front_right_n + front_anti_roll_transfer).max(0.0);

        let rear_anti_roll_transfer =
            config.suspension.anti_roll_bar_rear
                * (wheel_rear_left.suspension_compression - wheel_rear_right.suspension_compression)
                * 0.5;
        state.weight_rear_left_n = (state.weight_rear_left_n - rear_anti_roll_transfer).max(0.0);
        state.weight_rear_right_n = (state.weight_rear_right_n + rear_anti_roll_transfer).max(0.0);

        wheel_front_left.load_n = state.weight_front_left_n;
        wheel_front_right.load_n = state.weight_front_right_n;
        wheel_rear_left.load_n = state.weight_rear_left_n;
        wheel_rear_right.load_n = state.weight_rear_right_n;
    }

    state.suspension.front_left_travel_m = wheel_front_left.suspension_compression;
    state.suspension.front_right_travel_m = wheel_front_right.suspension_compression;
    state.suspension.rear_left_travel_m = wheel_rear_left.suspension_compression;
    state.suspension.rear_right_travel_m = wheel_rear_right.suspension_compression;
    state.suspension.front_left_velocity_mps = wheel_front_left.suspension_velocity_mps;
    state.suspension.front_right_velocity_mps = wheel_front_right.suspension_velocity_mps;
    state.suspension.rear_left_velocity_mps = wheel_rear_left.suspension_velocity_mps;
    state.suspension.rear_right_velocity_mps = wheel_rear_right.suspension_velocity_mps;
    state.suspension.front_left_spring_force_n = wheel_front_left.spring_force_n;
    state.suspension.front_right_spring_force_n = wheel_front_right.spring_force_n;
    state.suspension.rear_left_spring_force_n = wheel_rear_left.spring_force_n;
    state.suspension.rear_right_spring_force_n = wheel_rear_right.spring_force_n;
    state.suspension.front_left_damper_force_n = wheel_front_left.damper_force_n;
    state.suspension.front_right_damper_force_n = wheel_front_right.damper_force_n;
    state.suspension.rear_left_damper_force_n = wheel_rear_left.damper_force_n;
    state.suspension.rear_right_damper_force_n = wheel_rear_right.damper_force_n;

    state.is_airborne = is_airborne;
    
    // 9. Calculate steering angle
    let steering_angle = input.steering * config.max_steering_angle_rad;
    
    // Apply Ackermann steering geometry (inner wheel turns more)
    let (steer_left, steer_right) = calculate_ackermann_steering(
        steering_angle,
        config.wheelbase_m,
        config.track_width_front_m,
    );
    
    // 10. Calculate tire forces using Pacejka-inspired model
    let effective_grip = config.tire_config.grip_coefficient * track_ctx.grip_modifier;
    
    // Calculate slip ratios, wheel loads and tire forces
    let mut fl_slip = (0.0, 0.0);
    let mut fr_slip = (0.0, 0.0);
    let mut rl_slip = (0.0, 0.0);
    let mut rr_slip = (0.0, 0.0);
    let mut fl_forces = (0.0, 0.0);
    let mut fr_forces = (0.0, 0.0);
    let mut rl_forces = (0.0, 0.0);
    let mut rr_forces = (0.0, 0.0);

    if is_airborne {
        state.weight_front_left_n = 0.0;
        state.weight_front_right_n = 0.0;
        state.weight_rear_left_n = 0.0;
        state.weight_rear_right_n = 0.0;
    } else {
        // Front left tire
        fl_slip = calculate_wheel_slip(
            state.speed_mps,
            state.angular_vel_yaw,
            steer_left,
            config.wheelbase_m / 2.0,
            -config.track_width_front_m / 2.0,
            drive_torque_front / 2.0,
            brake_front / 2.0,
            config.wheel_radius_m,
        );

        // Front right tire
        fr_slip = calculate_wheel_slip(
            state.speed_mps,
            state.angular_vel_yaw,
            steer_right,
            config.wheelbase_m / 2.0,
            config.track_width_front_m / 2.0,
            drive_torque_front / 2.0,
            brake_front / 2.0,
            config.wheel_radius_m,
        );

        // Rear left tire
        rl_slip = calculate_wheel_slip(
            state.speed_mps,
            state.angular_vel_yaw,
            0.0,
            -config.wheelbase_m / 2.0,
            -config.track_width_rear_m / 2.0,
            drive_torque_rear / 2.0,
            brake_rear / 2.0,
            config.wheel_radius_m,
        );

        // Rear right tire
        rr_slip = calculate_wheel_slip(
            state.speed_mps,
            state.angular_vel_yaw,
            0.0,
            -config.wheelbase_m / 2.0,
            config.track_width_rear_m / 2.0,
            drive_torque_rear / 2.0,
            brake_rear / 2.0,
            config.wheel_radius_m,
        );

        fl_forces = calculate_tire_forces(
            state.weight_front_left_n,
            fl_slip.0,
            fl_slip.1,
            effective_grip,
            &config.tire_config,
        );
        fr_forces = calculate_tire_forces(
            state.weight_front_right_n,
            fr_slip.0,
            fr_slip.1,
            effective_grip,
            &config.tire_config,
        );
        rl_forces = calculate_tire_forces(
            state.weight_rear_left_n,
            rl_slip.0,
            rl_slip.1,
            effective_grip,
            &config.tire_config,
        );
        rr_forces = calculate_tire_forces(
            state.weight_rear_right_n,
            rr_slip.0,
            rr_slip.1,
            effective_grip,
            &config.tire_config,
        );
    }
    
    // 11. Sum all forces
    // Rotate front tire forces by steering angle
    let fl_force_x = fl_forces.0 * steer_left.cos() - fl_forces.1 * steer_left.sin();
    let fl_force_y = fl_forces.0 * steer_left.sin() + fl_forces.1 * steer_left.cos();
    let fr_force_x = fr_forces.0 * steer_right.cos() - fr_forces.1 * steer_right.sin();
    let fr_force_y = fr_forces.0 * steer_right.sin() + fr_forces.1 * steer_right.cos();
    
    // Total forces in vehicle frame
    let total_force_x = fl_force_x + fr_force_x + rl_forces.0 + rr_forces.0 - drag_force;
    let total_force_y = fl_force_y + fr_force_y + rl_forces.1 + rr_forces.1;
    
    // Include gravity components on slopes
    let slope_force = if is_airborne {
        0.0
    } else {
        config.mass_kg * GRAVITY * track_ctx.slope_rad.sin()
    };
    let banking_force = if is_airborne {
        0.0
    } else {
        config.mass_kg * GRAVITY * track_ctx.banking_rad.sin()
    };
    
    // 12. Calculate yaw moment
    let yaw_moment = 
        // Front tire contributions
        (fl_force_y + fr_force_y) * (config.wheelbase_m / 2.0)
        // Rear tire contributions
        - (rl_forces.1 + rr_forces.1) * (config.wheelbase_m / 2.0)
        // Lateral force offset contributions
        + (fr_force_x - fl_force_x) * (config.track_width_front_m / 2.0)
        + (rr_forces.0 - rl_forces.0) * (config.track_width_rear_m / 2.0);
    
    // 13. Calculate accelerations
    let accel_x = (total_force_x - slope_force) / config.mass_kg;
    let accel_y = (total_force_y + banking_force) / config.mass_kg;

    
    // Yaw moment of inertia (simplified as rectangular body)
    let yaw_inertia = config.mass_kg * (config.length_m.powi(2) + config.width_m.powi(2)) / 12.0;
    let angular_accel_yaw = yaw_moment / yaw_inertia;
    
    // 14. Update G-forces
    state.g_forces.longitudinal_g = accel_x / GRAVITY;
    state.g_forces.lateral_g = accel_y / GRAVITY;
    state.g_forces.vertical_g = 1.0 + (downforce_front + downforce_rear) / (config.mass_kg * GRAVITY);
    
    // 15. Integrate velocities
    // Transform acceleration from vehicle frame to world frame
    let cos_yaw = state.yaw_rad.cos();
    let sin_yaw = state.yaw_rad.sin();
    
    let accel_world_x = accel_x * cos_yaw - accel_y * sin_yaw;
    let accel_world_y = accel_x * sin_yaw + accel_y * cos_yaw;
    
    state.vel_x += accel_world_x * dt;
    state.vel_y += accel_world_y * dt;
    
    // Apply off-track penalty
    if !is_airborne && !track_ctx.is_on_track {
        let penalty = 1.0 - track.track_surface.off_track_speed_penalty * dt;
        state.vel_x *= penalty;
        state.vel_y *= penalty;
    }
    
    state.speed_mps = (state.vel_x.powi(2) + state.vel_y.powi(2) + state.vel_z.powi(2)).sqrt();
    
    // Prevent negative speed
    if state.speed_mps < MIN_SPEED_THRESHOLD && input.throttle < 0.1 {
        state.vel_x = 0.0;
        state.vel_y = 0.0;
        state.speed_mps = 0.0;
    }
    
    // 16. Integrate angular velocity
    state.angular_vel_yaw += angular_accel_yaw * dt;
    
    // Apply angular damping
    state.angular_vel_yaw *= 0.995;
    
    // 17. Integrate position
    state.pos_x += state.vel_x * dt;
    state.pos_y += state.vel_y * dt;

    if is_airborne {
        state.vel_z -= GRAVITY * dt;
        state.pos_z += state.vel_z * dt;
    }

    let post_track_ctx = get_track_context(state, track);
    let post_lateral_query_limit = post_track_ctx.width_left.max(post_track_ctx.width_right) + SURFACE_QUERY_LATERAL_MARGIN_M;
    let can_query_post_surface = post_track_ctx.lateral_offset.abs() <= post_lateral_query_limit;

    if can_query_post_surface {
        if is_airborne {
            if state.pos_z <= post_track_ctx.elevation {
                state.pos_z = post_track_ctx.elevation;
                state.vel_z = 0.0;
                is_airborne = false;
            }
        } else {
            // Keep grounded cars above track plane while allowing suspension ride height.
            state.pos_z = state.pos_z.max(post_track_ctx.elevation);
            state.vel_z = 0.0;
        }
    } else {
        state.vel_z = 0.0;
        is_airborne = false;
    }
    state.is_airborne = is_airborne;

    state.is_on_track = post_track_ctx.is_on_track;
    state.current_surface = post_track_ctx.surface_type;
    state.surface_grip_modifier = post_track_ctx.grip_modifier;
    state.lateral_offset_m = post_track_ctx.lateral_offset;
    
    // 18. Integrate orientation
    state.yaw_rad += state.angular_vel_yaw * dt;
    state.yaw_rad = normalize_angle(state.yaw_rad);
    
    // Match track pitch and roll while grounded
    if state.is_airborne {
        state.pitch_rad += state.angular_vel_pitch * dt;
        state.roll_rad += state.angular_vel_roll * dt;
    } else {
        state.pitch_rad = post_track_ctx.slope_rad;
        state.roll_rad = -post_track_ctx.banking_rad;
    }
    
    // 19. Store inputs
    state.throttle_input = input.throttle;
    state.brake_input = input.brake;
    state.steering_input = input.steering;

    // Apply gear and clutch inputs if provided
    if let Some(gear) = input.gear {
        state.gear = gear;
    }
    if let Some(clutch) = input.clutch {
        state.clutch_input = clutch;
    }

    // 20. Update telemetry
    update_telemetry_3d(
        state,
        config,
        input,
        &post_track_ctx,
        fl_slip,
        fr_slip,
        rl_slip,
        rr_slip,
        dt,
    );
    
    // 21. Update fuel consumption
    update_fuel_consumption(state, config, input, dt);
}

/// Calculate static weight distribution based on CoG position
fn calculate_static_weight_distribution(config: &CarConfig, total_weight: f32) -> (f32, f32) {
    let front_weight = total_weight * config.weight_distribution_front;
    let rear_weight = total_weight * (1.0 - config.weight_distribution_front);
    (front_weight, rear_weight)
}

/// Calculate aerodynamic forces (drag and downforce)
fn calculate_aerodynamic_forces(state: &CarState, config: &CarConfig) -> (f32, f32, f32) {
    let speed_squared = state.speed_mps.powi(2);
    let dynamic_pressure = 0.5 * AIR_DENSITY * speed_squared;
    
    // Drag force
    let drag = dynamic_pressure * config.drag_coefficient * config.frontal_area_m2;
    
    // Downforce (lift coefficients are negative for downforce)
    let downforce_front = -dynamic_pressure * config.lift_coefficient_front * config.frontal_area_m2;
    let downforce_rear = -dynamic_pressure * config.lift_coefficient_rear * config.frontal_area_m2;
    
    (drag, downforce_front.max(0.0), downforce_rear.max(0.0))
}

/// Calculate engine output torque and RPM
fn calculate_engine_output(state: &CarState, config: &CarConfig, input: &PlayerInputData) -> (f32, f32) {
    // Calculate wheel speed based on current velocity
    let wheel_rpm = if state.speed_mps > MIN_SPEED_THRESHOLD {
        (state.speed_mps / (2.0 * PI * config.wheel_radius_m)) * 60.0
    } else {
        0.0
    };
    
    // Calculate engine RPM from wheel speed through drivetrain
    let gear_ratio = if state.gear > 0 && (state.gear as usize) < config.gear_ratios.len() {
        config.gear_ratios[state.gear as usize]
    } else if state.gear == 0 {
        0.0  // Neutral
    } else {
        config.gear_ratios[0]  // Reverse
    };
    
    let engine_rpm = if gear_ratio.abs() > 0.001 {
        (wheel_rpm * gear_ratio.abs() * config.final_drive_ratio).clamp(config.idle_rpm, config.max_engine_rpm)
    } else {
        config.idle_rpm + input.throttle * (config.redline_rpm - config.idle_rpm) * 0.3
    };
    
    let torque_at_rpm = if !config.engine.torque_curve.is_empty() {
        interpolate_torque_curve(&config.engine.torque_curve, engine_rpm)
    } else {
        // Legacy simple torque curve (peak at ~60% of redline)
        let rpm_normalized = (engine_rpm - config.idle_rpm) / (config.redline_rpm - config.idle_rpm);
        let torque_factor = 1.0 - (rpm_normalized - 0.6).powi(2);
        config.max_engine_torque_nm * torque_factor.clamp(0.3, 1.0)
    };

    // Rev limiter torque cut
    let limiter_rpm = config.engine.rev_limiter_rpm.max(config.redline_rpm);
    let limiter_cut = if engine_rpm >= limiter_rpm {
        // Hard cut near limiter
        0.2
    } else {
        1.0
    };

    // Engine braking & friction
    let rpm_frac = ((engine_rpm - config.idle_rpm) / (config.redline_rpm - config.idle_rpm).max(1.0)).clamp(0.0, 1.0);
    let engine_brake = if input.throttle < 0.01 {
        config.engine.engine_brake_torque_nm * rpm_frac
    } else {
        0.0
    };

    // Net torque produced by engine (positive = drive, negative = braking)
    let mut engine_torque = (input.throttle * torque_at_rpm * limiter_cut) - engine_brake;

    // Always apply a small friction torque opposing rotation
    engine_torque -= config.engine.friction_torque_nm * rpm_frac;
    
    (engine_torque, engine_rpm)
}

fn interpolate_torque_curve(curve: &[TorqueCurvePoint], rpm: f32) -> f32 {
    if curve.is_empty() {
        return 0.0;
    }

    // If curve isn't sorted, this still behaves reasonably for monotonic input, but
    // data authors should keep it ordered by RPM.
    if rpm <= curve[0].rpm {
        return curve[0].torque_nm;
    }
    if rpm >= curve[curve.len() - 1].rpm {
        return curve[curve.len() - 1].torque_nm;
    }

    for window in curve.windows(2) {
        let a = window[0];
        let b = window[1];
        if rpm >= a.rpm && rpm <= b.rpm {
            let t = (rpm - a.rpm) / (b.rpm - a.rpm).max(1.0);
            return a.torque_nm + (b.torque_nm - a.torque_nm) * t;
        }
    }

    curve[curve.len() - 1].torque_nm
}

/// Calculate drive torques for front and rear axles
fn calculate_drive_torques(engine_torque: f32, config: &CarConfig, gear: i8) -> (f32, f32) {
    if gear == 0 {
        return (0.0, 0.0);  // Neutral
    }
    
    let gear_ratio = if gear > 0 && (gear as usize) < config.gear_ratios.len() {
        config.gear_ratios[gear as usize]
    } else {
        config.gear_ratios[0]  // Reverse
    };
    
    let total_ratio = gear_ratio * config.final_drive_ratio;
    let wheel_torque = engine_torque * total_ratio * config.transmission.efficiency.clamp(0.0, 1.0);
    
    match config.drivetrain {
        Drivetrain::FWD => (wheel_torque, 0.0),
        Drivetrain::RWD => (0.0, wheel_torque),
        Drivetrain::AWD => (wheel_torque * 0.4, wheel_torque * 0.6),  // 40/60 split
    }
}

/// Calculate weight transfer from acceleration
fn calculate_weight_transfer(
    config: &CarConfig,
    longitudinal_accel: f32,
    lateral_accel: f32,
    _total_weight: f32,
) -> (f32, f32, f32) {
    // Longitudinal weight transfer
    let weight_transfer_long = (config.mass_kg * longitudinal_accel * config.cog_height_m) / config.wheelbase_m;
    
    // Lateral weight transfer (different for front and rear due to roll stiffness)
    let total_roll_stiffness = config.suspension.anti_roll_bar_front + config.suspension.anti_roll_bar_rear;
    let front_roll_ratio = config.suspension.anti_roll_bar_front / total_roll_stiffness.max(1.0);
    let rear_roll_ratio = config.suspension.anti_roll_bar_rear / total_roll_stiffness.max(1.0);
    
    let lateral_transfer_front = (config.mass_kg * lateral_accel * config.cog_height_m) / config.track_width_front_m * front_roll_ratio;
    let lateral_transfer_rear = (config.mass_kg * lateral_accel * config.cog_height_m) / config.track_width_rear_m * rear_roll_ratio;
    
    (weight_transfer_long, lateral_transfer_front, lateral_transfer_rear)
}

/// Calculate Ackermann steering geometry
fn calculate_ackermann_steering(steering_angle: f32, wheelbase: f32, track_width: f32) -> (f32, f32) {
    if steering_angle.abs() < 0.001 {
        return (0.0, 0.0);
    }
    
    // Calculate turn radius
    let turn_radius = wheelbase / steering_angle.tan().abs();
    
    // Inner and outer wheel angles
    let inner_radius = turn_radius - track_width / 2.0;
    let outer_radius = turn_radius + track_width / 2.0;
    
    let inner_angle = (wheelbase / inner_radius).atan();
    let outer_angle = (wheelbase / outer_radius).atan();
    
    if steering_angle > 0.0 {
        // Turning right: right wheel is inner
        (outer_angle, inner_angle)
    } else {
        // Turning left: left wheel is inner
        (-inner_angle, -outer_angle)
    }
}

/// Calculate slip ratio and slip angle for a wheel
fn calculate_wheel_slip(
    vehicle_speed: f32,
    yaw_rate: f32,
    steer_angle: f32,
    wheel_pos_x: f32,  // Distance from CoG (+ = front)
    _wheel_pos_y: f32,  // Distance from centerline (+ = right)
    drive_torque: f32,
    brake_force: f32,
    wheel_radius: f32,
) -> (f32, f32) {
    // Wheel velocity components due to vehicle motion and yaw
    let wheel_vel_x = vehicle_speed.max(MIN_SPEED_THRESHOLD);
    let wheel_vel_y = yaw_rate * wheel_pos_x;

    // Calculate wheel speed (assuming no longitudinal slip for now)
    let wheel_speed = wheel_vel_x / wheel_radius;

    // Slip ratio (longitudinal)
    // The driven wheel speed is increased by engine torque (through the drivetrain)
    // and decreased by brake force. The factor 100.0 is a simplified wheel inertia term.
    let driven_wheel_speed = wheel_speed + (drive_torque - brake_force * wheel_radius) / (100.0 * wheel_radius);

    let slip_ratio = if vehicle_speed > MIN_SPEED_THRESHOLD {
        // Normal driving: slip = (wheel_speed - ground_speed) / ground_speed
        (driven_wheel_speed * wheel_radius - wheel_vel_x) / wheel_vel_x
    } else {
        // Launch from standstill: use optimal slip ratio to maximize tire force
        // The Pacejka magic formula produces peak force at optimal slip (~0.1-0.15)
        // Beyond that, force drops off (simulating wheel spin)
        // So we target optimal slip when launching for maximum traction
        let optimal_slip = 0.12; // Near optimal slip for peak traction
        if drive_torque > 10.0 {
            optimal_slip  // Apply throttle = optimal slip for max force
        } else if brake_force > 10.0 {
            -optimal_slip  // Braking = negative slip
        } else {
            0.0  // No input = no slip
        }
    };

    // Slip angle (lateral)
    let slip_angle = if wheel_vel_x > MIN_SPEED_THRESHOLD {
        (wheel_vel_y / wheel_vel_x).atan() - steer_angle
    } else {
        0.0
    };

    (slip_ratio.clamp(-1.0, 1.0), slip_angle.clamp(-0.5, 0.5))
}

/// Calculate tire forces using simplified Pacejka magic formula
fn calculate_tire_forces(
    wheel_load: f32,
    slip_ratio: f32,
    slip_angle: f32,
    grip_coefficient: f32,
    tire_config: &TireConfig,
) -> (f32, f32) {
    if wheel_load < 1.0 {
        return (0.0, 0.0);
    }
    
    // Magic formula parameters (simplified)
    let b_long = 10.0;  // Stiffness factor
    let c_long = 1.9;   // Shape factor
    let d_long = grip_coefficient * wheel_load;  // Peak force
    
    let b_lat = 8.0;
    let c_lat = 1.3;
    let d_lat = grip_coefficient * wheel_load;
    
    // Longitudinal force (Fx)
    let slip_ratio_adjusted = slip_ratio / tire_config.optimal_slip_ratio;
    let fx = d_long * (c_long * (b_long * slip_ratio_adjusted).atan()).sin();
    
    // Lateral force (Fy)
    let slip_angle_adjusted = slip_angle / tire_config.optimal_slip_angle_rad;
    let fy = d_lat * (c_lat * (b_lat * slip_angle_adjusted).atan()).sin();
    
    // Combined slip (friction circle)
    let combined_force = (fx.powi(2) + fy.powi(2)).sqrt();
    let max_force = d_long.max(d_lat);
    
    if combined_force > max_force {
        let scale = max_force / combined_force;
        (fx * scale, fy * scale)
    } else {
        (fx, fy)
    }
}

/// Query ground/surface properties at a world-space position.
fn query_track_surface(track: &TrackConfig, world_x: f32, world_y: f32) -> Option<SurfaceQuerySample> {
    match active_surface_query_backend() {
        SurfaceQueryBackend::Centerline => query_track_surface_centerline(track, world_x, world_y),
        SurfaceQueryBackend::MeshHeightfield => query_track_surface_mesh_heightfield_stub(track, world_x, world_y),
    }
}

fn track_heightfield_provider(track: &TrackConfig) -> Option<HeightfieldSurfaceData<'_>> {
    let heightmap = track.procedural_world.as_ref()?.heightmap.as_ref()?;
    Some(HeightfieldSurfaceData { heightmap })
}

/// Mesh/heightfield surface query backend stub.
///
/// This keeps the seam stable while mesh-backed terrain sampling is implemented.
/// For now, it transparently falls back to centerline sampling.
fn query_track_surface_mesh_heightfield_stub(
    track: &TrackConfig,
    world_x: f32,
    world_y: f32,
) -> Option<SurfaceQuerySample> {
    let centerline_sample = query_track_surface_centerline(track, world_x, world_y)?;

    if let Some(provider) = track_heightfield_provider(track) {
        if let Some(elevation) = provider.elevation_at(world_x, world_y) {
            return Some(SurfaceQuerySample {
                elevation,
                ..centerline_sample
            });
        }
    }

    MESH_BACKEND_STUB_LOG_ONCE.call_once(|| {
        debug!(
            "Mesh/heightfield backend has no usable heightfield sample; using centerline fallback"
        );
    });

    Some(centerline_sample)
}

/// Centerline-backed surface query implementation.
fn query_track_surface_centerline(
    track: &TrackConfig,
    world_x: f32,
    world_y: f32,
) -> Option<SurfaceQuerySample> {
    if track.centerline.is_empty() {
        return None;
    }

    // Find nearest centerline point
    let mut min_dist_sq = f32::MAX;
    let mut nearest_idx = 0;

    for (idx, point) in track.centerline.iter().enumerate() {
        let dx = world_x - point.x;
        let dy = world_y - point.y;
        let dist_sq = dx * dx + dy * dy;

        if dist_sq < min_dist_sq {
            min_dist_sq = dist_sq;
            nearest_idx = idx;
        }
    }

    let nearest = &track.centerline[nearest_idx];

    // Calculate lateral offset (signed distance from centerline)
    let dx = world_x - nearest.x;
    let dy = world_y - nearest.y;
    let cross = dx * nearest.heading_rad.sin() - dy * nearest.heading_rad.cos();
    let lateral_offset = cross; // Positive = right of centerline

    Some(SurfaceQuerySample {
        nearest_point: nearest_idx,
        elevation: nearest.z,
        banking_rad: nearest.banking_rad,
        slope_rad: nearest.slope_rad,
        heading_rad: nearest.heading_rad,
        lateral_offset,
        width_left: nearest.width_left_m,
        width_right: nearest.width_right_m,
        surface_type: nearest.surface_type,
        grip_modifier: nearest.grip_modifier,
    })
}

/// Get track context at the car's current position
fn get_track_context(state: &CarState, track: &TrackConfig) -> TrackContext {
    let Some(surface) = query_track_surface(track, state.pos_x, state.pos_y) else {
        return TrackContext::default();
    };
    
    // Check if on track
    let is_on_track = surface.lateral_offset.abs() <= surface.width_right.max(surface.width_left);
    
    // Determine surface type and grip
    let (surface_type, grip_modifier) = if is_on_track {
        (surface.surface_type, surface.grip_modifier)
    } else {
        // Off track
        (SurfaceType::Grass, track.track_surface.off_track_grip)
    };
    
    TrackContext {
        nearest_point: surface.nearest_point,
        elevation: surface.elevation,
        banking_rad: surface.banking_rad,
        slope_rad: surface.slope_rad,
        heading_rad: surface.heading_rad,
        lateral_offset: surface.lateral_offset,
        is_on_track,
        surface_type,
        grip_modifier,
        width_left: surface.width_left,
        width_right: surface.width_right,
    }
}

/// Update telemetry data for 3D physics
fn update_telemetry_3d(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    _track_ctx: &TrackContext,
    fl_slip: (f32, f32),
    fr_slip: (f32, f32),
    rl_slip: (f32, f32),
    rr_slip: (f32, f32),
    dt: f32,
) {
    // Calculate tire temperatures based on slip and load
    let calculate_tire_temp = |slip_ratio: f32, slip_angle: f32, load: f32| -> f32 {
        let base_temp = 80.0;
        let slip_heat = (slip_ratio.abs() + slip_angle.abs()) * 100.0;
        let load_heat = load / 10000.0 * 10.0;
        let speed_cooling = state.speed_mps * 0.1;
        base_temp + slip_heat + load_heat - speed_cooling
    };
    
    // Front left tire
    state.tires.front_left.temperature_c = calculate_tire_temp(fl_slip.0, fl_slip.1, state.weight_front_left_n);
    state.tires.front_left.pressure_kpa = 200.0 + state.tires.front_left.temperature_c * 0.5;
    state.tires.front_left.slip_ratio = fl_slip.0;
    state.tires.front_left.slip_angle_rad = fl_slip.1;
    state.tires.front_left.wear_percent = (state.tires.front_left.wear_percent + 
        fl_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt).min(100.0);
    
    // Front right tire
    state.tires.front_right.temperature_c = calculate_tire_temp(fr_slip.0, fr_slip.1, state.weight_front_right_n);
    state.tires.front_right.pressure_kpa = 200.0 + state.tires.front_right.temperature_c * 0.5;
    state.tires.front_right.slip_ratio = fr_slip.0;
    state.tires.front_right.slip_angle_rad = fr_slip.1;
    state.tires.front_right.wear_percent = (state.tires.front_right.wear_percent + 
        fr_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt).min(100.0);
    
    // Rear left tire
    state.tires.rear_left.temperature_c = calculate_tire_temp(rl_slip.0, rl_slip.1, state.weight_rear_left_n);
    state.tires.rear_left.pressure_kpa = 200.0 + state.tires.rear_left.temperature_c * 0.5;
    state.tires.rear_left.slip_ratio = rl_slip.0;
    state.tires.rear_left.slip_angle_rad = rl_slip.1;
    state.tires.rear_left.wear_percent = (state.tires.rear_left.wear_percent + 
        rl_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt).min(100.0);
    
    // Rear right tire
    state.tires.rear_right.temperature_c = calculate_tire_temp(rr_slip.0, rr_slip.1, state.weight_rear_right_n);
    state.tires.rear_right.pressure_kpa = 200.0 + state.tires.rear_right.temperature_c * 0.5;
    state.tires.rear_right.slip_ratio = rr_slip.0;
    state.tires.rear_right.slip_angle_rad = rr_slip.1;
    state.tires.rear_right.wear_percent = (state.tires.rear_right.wear_percent + 
        rr_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt).min(100.0);
    
    // Engine temperature (increases with load, decreases with airflow)
    let engine_load = input.throttle * (state.engine_rpm / config.redline_rpm);
    let cooling = state.speed_mps * 0.2;
    state.engine_temp_c = 85.0 + engine_load * 15.0 - cooling;
    state.engine_temp_c = state.engine_temp_c.clamp(60.0, 120.0);
    
    // Oil temperature follows engine temp with lag
    state.oil_temp_c = state.oil_temp_c + (state.engine_temp_c + 5.0 - state.oil_temp_c) * 0.01;
    
    // Oil pressure (decreases at high temp)
    state.oil_pressure_kpa = 400.0 - (state.oil_temp_c - 80.0) * 2.0;
    state.oil_pressure_kpa = state.oil_pressure_kpa.clamp(100.0, 500.0);
    
    // Water temperature
    state.water_temp_c = state.water_temp_c + (state.engine_temp_c - state.water_temp_c) * 0.02;
}

/// Update fuel consumption
fn update_fuel_consumption(state: &mut CarState, config: &CarConfig, input: &PlayerInputData, dt: f32) {
    // Base consumption + load-based consumption
    let rpm_factor = state.engine_rpm / config.max_engine_rpm;
    let throttle_factor = input.throttle;
    
    state.fuel_consumption_lps = config.fuel.idle_consumption_lps
        + (throttle_factor * rpm_factor * config.fuel.load_consumption_scale);
    state.fuel_liters = (state.fuel_liters - state.fuel_consumption_lps * dt).max(0.0);
}

/// Check and resolve 3D AABB collisions between cars
pub fn check_aabb_collisions_3d(
    states: &mut [CarState],
    configs: &HashMap<CarConfigId, CarConfig>,
) {
    // Reset collision flags
    for state in states.iter_mut() {
        state.is_colliding = false;
        state.collision_normal_x = 0.0;
        state.collision_normal_y = 0.0;
        state.collision_normal_z = 0.0;
    }

    // Check all pairs
    for i in 0..states.len() {
        for j in (i + 1)..states.len() {
            let config_i = configs.get(&states[i].car_config_id);
            let config_j = configs.get(&states[j].car_config_id);

            if let (Some(cfg_i), Some(cfg_j)) = (config_i, config_j) {
                if check_collision_3d(&states[i], cfg_i, &states[j], cfg_j) {
                    // Mark as colliding
                    states[i].is_colliding = true;
                    states[j].is_colliding = true;

                    // Calculate collision normal
                    let dx = states[j].pos_x - states[i].pos_x;
                    let dy = states[j].pos_y - states[i].pos_y;
                    let dz = states[j].pos_z - states[i].pos_z;
                    let dist = (dx * dx + dy * dy + dz * dz).sqrt().max(0.1);
                    
                    let nx = dx / dist;
                    let ny = dy / dist;
                    let nz = dz / dist;
                    
                    states[i].collision_normal_x = -nx;
                    states[i].collision_normal_y = -ny;
                    states[i].collision_normal_z = -nz;
                    states[j].collision_normal_x = nx;
                    states[j].collision_normal_y = ny;
                    states[j].collision_normal_z = nz;

                    // Separate cars
                    let separation = 0.5;
                    states[i].pos_x -= nx * separation;
                    states[i].pos_y -= ny * separation;
                    states[j].pos_x += nx * separation;
                    states[j].pos_y += ny * separation;

                    // Calculate impact velocity and apply impulse
                    let rel_vel_x = states[j].vel_x - states[i].vel_x;
                    let rel_vel_y = states[j].vel_y - states[i].vel_y;
                    let rel_vel_normal = rel_vel_x * nx + rel_vel_y * ny;
                    
                    if rel_vel_normal < 0.0 {
                        // Collision impulse (elastic coefficient)
                        let restitution = 0.3;
                        let impulse = -(1.0 + restitution) * rel_vel_normal;
                        let impulse = impulse / (1.0 / cfg_i.mass_kg + 1.0 / cfg_j.mass_kg);
                        
                        states[i].vel_x -= impulse * nx / cfg_i.mass_kg;
                        states[i].vel_y -= impulse * ny / cfg_i.mass_kg;
                        states[j].vel_x += impulse * nx / cfg_j.mass_kg;
                        states[j].vel_y += impulse * ny / cfg_j.mass_kg;
                    }
                    
                    // Recalculate speeds
                    states[i].speed_mps = (states[i].vel_x.powi(2) + states[i].vel_y.powi(2) + states[i].vel_z.powi(2)).sqrt();
                    states[j].speed_mps = (states[j].vel_x.powi(2) + states[j].vel_y.powi(2) + states[j].vel_z.powi(2)).sqrt();
                    
                    // Apply damage
                    let impact_speed = ((states[i].speed_mps + states[j].speed_mps) / 2.0).min(50.0);
                    let damage_amount = (impact_speed / 50.0) * 5.0;
                    
                    let angle_i = (ny.atan2(nx) - states[i].yaw_rad).rem_euclid(2.0 * PI);
                    let angle_j = (ny.atan2(nx) - states[j].yaw_rad + PI).rem_euclid(2.0 * PI);
                    
                    apply_damage_to_car(&mut states[i], angle_i, damage_amount);
                    apply_damage_to_car(&mut states[j], angle_j, damage_amount);
                }
            }
        }
    }
}

/// Check if two cars are colliding using oriented bounding boxes (simplified to AABB)
fn check_collision_3d(
    state_a: &CarState,
    config_a: &CarConfig,
    state_b: &CarState,
    config_b: &CarConfig,
) -> bool {
    let half_l_a = config_a.length_m / 2.0;
    let half_w_a = config_a.width_m / 2.0;
    let half_h_a = config_a.height_m / 2.0;
    
    let half_l_b = config_b.length_m / 2.0;
    let half_w_b = config_b.width_m / 2.0;
    let half_h_b = config_b.height_m / 2.0;

    let dx = (state_a.pos_x - state_b.pos_x).abs();
    let dy = (state_a.pos_y - state_b.pos_y).abs();
    let dz = (state_a.pos_z - state_b.pos_z).abs();

    // Use larger dimension for rotated AABB approximation
    let size_a = half_l_a.max(half_w_a);
    let size_b = half_l_b.max(half_w_b);

    dx < (size_a + size_b) && dy < (size_a + size_b) && dz < (half_h_a + half_h_b)
}

/// Apply damage to a car based on collision angle
fn apply_damage_to_car(car: &mut CarState, angle: f32, damage_amount: f32) {
    if angle < PI / 4.0 || angle > 7.0 * PI / 4.0 {
        car.damage.front_damage_percent = (car.damage.front_damage_percent + damage_amount).min(100.0);
        car.damage.engine_damage_percent = (car.damage.engine_damage_percent + damage_amount * 0.5).min(100.0);
    } else if angle >= PI / 4.0 && angle < 3.0 * PI / 4.0 {
        car.damage.left_damage_percent = (car.damage.left_damage_percent + damage_amount).min(100.0);
    } else if angle >= 3.0 * PI / 4.0 && angle < 5.0 * PI / 4.0 {
        car.damage.rear_damage_percent = (car.damage.rear_damage_percent + damage_amount).min(100.0);
    } else {
        car.damage.right_damage_percent = (car.damage.right_damage_percent + damage_amount).min(100.0);
    }
    
    car.damage.is_drivable = car.damage.front_damage_percent < 80.0
        && car.damage.engine_damage_percent < 80.0;
}

/// Update track progress and detect lap completion
pub fn update_track_progress_3d(
    state: &mut CarState,
    track: &TrackConfig,
    current_tick: u32,
) {
    if track.centerline.is_empty() {
        return;
    }
    
    let track_length = track.centerline.last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(1000.0);

    // Find nearest centerline point
    let mut min_dist = f32::MAX;
    let mut nearest_idx = 0;

    for (idx, point) in track.centerline.iter().enumerate() {
        let dx = state.pos_x - point.x;
        let dy = state.pos_y - point.y;
        let dist = (dx * dx + dy * dy).sqrt();

        if dist < min_dist {
            min_dist = dist;
            nearest_idx = idx;
        }
    }

    let old_progress = state.track_progress;
    state.track_progress = track.centerline[nearest_idx].distance_from_start_m;

    // Debug: Log track progress every 240 ticks (once per second)
    if current_tick % 240 == 0 {
        debug!("[Lap Debug] Tick {}: current_lap={}, track_progress={:.1}m/{:.1}m, lap_time={}ms",
            current_tick, state.current_lap, state.track_progress, track_length, state.current_lap_time_ms);
    }

    // Update current lap time
    if state.current_lap > 0 {
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        state.current_lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / 240.0) as u32;
    }

    // Detect lap completion
    if state.current_lap > 0 && old_progress > track_length * 0.8 && state.track_progress < track_length * 0.2 {
        // Calculate lap time (time since lap started)
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        let lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);

        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }

        state.current_lap += 1;
        state.lap_start_tick = current_tick;  // Reset lap timer
        state.current_lap_time_ms = 0;
    }

    // Start lap 1 - only when crossing the start line going forward
    // This prevents false start detection when spawning near the finish line
    if state.current_lap == 0 && old_progress < track_length * 0.1 && state.track_progress > track_length * 0.1 {
        state.current_lap = 1;
        state.lap_start_tick = current_tick;  // Start timing first lap
        state.current_lap_time_ms = 0;
    }
}

/// Normalize angle to -PI to PI range
fn normalize_angle(angle: f32) -> f32 {
    let mut a = angle % (2.0 * PI);
    if a > PI {
        a -= 2.0 * PI;
    } else if a < -PI {
        a += 2.0 * PI;
    }
    a
}

// ============================================================================
// Legacy 2D API - Wrapper for backward compatibility
// ============================================================================

/// Legacy 2D physics update - wraps the 3D implementation
pub fn update_car_2d(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) {
    // Create a default track for legacy 2D mode
    let track = TrackConfig::default();
    update_car_3d(state, config, input, &track, dt);
}

/// Legacy track progress update
pub fn update_track_progress(
    state: &mut CarState,
    centerline: &[TrackPoint],
    track_length: f32,
    current_tick: u32,
) {
    if centerline.is_empty() {
        return;
    }

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

    // Update current lap time
    if state.current_lap > 0 {
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        state.current_lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / 240.0) as u32;
    }

    if state.current_lap > 0 && old_progress > track_length * 0.8 && state.track_progress < track_length * 0.2 {
        // Calculate lap time (time since lap started)
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        let lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);

        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }

        state.current_lap += 1;
        state.lap_start_tick = current_tick;  // Reset lap timer
        state.current_lap_time_ms = 0;
    }

    // Start lap 1 - only when crossing the start line going forward
    // This prevents false start detection when spawning near the finish line
    if state.current_lap == 0 && old_progress < track_length * 0.1 && state.track_progress > track_length * 0.1 {
        state.current_lap = 1;
        state.lap_start_tick = current_tick;  // Start timing first lap
        state.current_lap_time_ms = 0;
    }
}

/// Legacy collision check - wraps 3D version
pub fn check_aabb_collisions(
    states: &mut [CarState],
    configs: &HashMap<CarConfigId, CarConfig>,
) {
    check_aabb_collisions_3d(states, configs);
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn create_test_car_state() -> CarState {
        let grid_slot = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        CarState::new(Uuid::new_v4(), Uuid::new_v4(), &grid_slot)
    }

    fn create_test_config() -> CarConfig {
        CarConfig::default()
    }

    fn create_test_track() -> TrackConfig {
        TrackConfig::default()
    }

    #[test]
    fn test_update_car_acceleration() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_test_track();
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        let dt = 1.0 / 240.0;

        // Run multiple ticks to allow car to accelerate
        for _ in 0..100 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }

        // Speed should increase with throttle
        assert!(state.speed_mps > 0.0, "Speed should increase: {}", state.speed_mps);
        // RPM increases as the car gains speed in gear
        assert!(state.engine_rpm >= config.idle_rpm, "RPM should be at least idle: {}", state.engine_rpm);
    }

    #[test]
    fn test_update_car_braking() {
        let mut state = create_test_car_state();
        state.vel_x = 10.0;
        state.speed_mps = 10.0;
        
        let config = create_test_config();
        let track = create_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        let dt = 1.0 / 240.0;
        let initial_speed = state.speed_mps;

        update_car_3d(&mut state, &config, &input, &track, dt);

        assert!(state.speed_mps < initial_speed, "Speed should decrease from braking");
    }

    #[test]
    fn test_update_car_steering() {
        let mut state = create_test_car_state();
        state.vel_x = 10.0;
        state.speed_mps = 10.0;
        
        let config = create_test_config();
        let track = create_test_track();
        let input = PlayerInputData {
            throttle: 0.5,
            brake: 0.0,
            steering: 0.5,
            gear: None,
            clutch: None,
        };

        let dt = 1.0 / 240.0;

        update_car_3d(&mut state, &config, &input, &track, dt);

        // Angular velocity should change with steering
        assert!(state.angular_vel_yaw.abs() > 0.0 || state.yaw_rad.abs() > 0.0001,
            "Steering should cause yaw change");
    }

    #[test]
    fn test_collision_detection() {
        let config = create_test_config();
        let grid_slot1 = GridSlot { position: 1, x: 0.0, y: 0.0, z: 0.0, yaw_rad: 0.0 };
        let grid_slot2 = GridSlot { position: 2, x: 1.0, y: 0.0, z: 0.0, yaw_rad: 0.0 };
        
        let mut states = vec![
            CarState::new(Uuid::new_v4(), config.id, &grid_slot1),
            CarState::new(Uuid::new_v4(), config.id, &grid_slot2),
        ];
        states[0].speed_mps = 10.0;
        states[1].speed_mps = 10.0;

        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        check_aabb_collisions_3d(&mut states, &configs);

        assert!(states[0].is_colliding, "Car 1 should be colliding");
        assert!(states[1].is_colliding, "Car 2 should be colliding");
    }

    #[test]
    fn test_track_progress_update() {
        let track = create_test_track();
        let mut state = create_test_car_state();
        state.pos_x = track.centerline[1].x;
        state.pos_y = track.centerline[1].y;

        update_track_progress_3d(&mut state, &track, 0);

        assert!(state.track_progress > 0.0, "Track progress should be positive");
    }

    #[test]
    fn test_aerodynamic_forces() {
        let mut state = create_test_car_state();
        state.speed_mps = 50.0;  // 180 km/h
        let config = create_test_config();

        let (drag, df_front, df_rear) = calculate_aerodynamic_forces(&state, &config);

        assert!(drag > 0.0, "Drag should be positive at speed");
        assert!(df_front > 0.0, "Front downforce should be positive");
        assert!(df_rear > 0.0, "Rear downforce should be positive");
    }

    #[test]
    fn test_weight_transfer() {
        let config = create_test_config();
        let total_weight = config.mass_kg * GRAVITY;
        
        // Under braking (negative longitudinal accel)
        let (long_transfer, _, _) = calculate_weight_transfer(&config, -10.0, 0.0, total_weight);
        assert!(long_transfer < 0.0, "Weight should transfer forward under braking");
        
        // Under acceleration
        let (long_transfer, _, _) = calculate_weight_transfer(&config, 5.0, 0.0, total_weight);
        assert!(long_transfer > 0.0, "Weight should transfer rearward under acceleration");
    }

    #[test]
    fn test_tire_forces() {
        let tire_config = TireConfig::default();
        let load = 3000.0;  // 3000N wheel load
        
        // Test longitudinal force (acceleration)
        let (fx, fy) = calculate_tire_forces(load, 0.05, 0.0, 1.0, &tire_config);
        assert!(fx.abs() > 0.0, "Should produce longitudinal force");
        assert!(fy.abs() < 0.1, "Should produce minimal lateral force");
        
        // Test lateral force (cornering)
        let (fx, fy) = calculate_tire_forces(load, 0.0, 0.1, 1.0, &tire_config);
        assert!(fx.abs() < 0.1, "Should produce minimal longitudinal force");
        assert!(fy.abs() > 0.0, "Should produce lateral force");
    }

    #[test]
    fn test_ackermann_steering() {
        let wheelbase = 2.7;
        let track_width = 1.6;
        
        // Test right turn
        let (left, right) = calculate_ackermann_steering(0.3, wheelbase, track_width);
        assert!(left > 0.0 && right > 0.0, "Both wheels should turn");
        assert!(right > left, "Inner wheel (right) should turn more");
        
        // Test straight
        let (left, right) = calculate_ackermann_steering(0.0, wheelbase, track_width);
        assert!(left.abs() < 0.001 && right.abs() < 0.001, "No steering angle");
    }

    #[test]
    fn test_3d_position_and_orientation() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_test_track();
        
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        // Run several ticks
        for _ in 0..100 {
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        }

        // Car should have moved and adopted track elevation
        assert!(state.pos_x != 0.0 || state.pos_y != 0.0, "Car should have moved");
    }

    #[test]
    fn test_airborne_gravity_and_landing() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = TrackConfig {
            id: Uuid::new_v4(),
            name: "Flat Test Track".to_string(),
            centerline: vec![TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 20.0,
                width_right_m: 20.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            }],
            width_m: 40.0,
            source_path: None,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: None,
        };

        state.pos_x = 0.0;
        state.pos_y = 0.0;
        state.pos_z = 1.0;
        state.vel_z = 0.0;

        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        let dt = 1.0 / 240.0;

        update_car_3d(&mut state, &config, &input, &track, dt);

        assert!(state.is_airborne, "Car should be airborne when above track surface");
        assert!(state.vel_z < 0.0, "Gravity should accelerate car downward");
        assert_eq!(state.weight_front_left_n, 0.0, "Airborne car should have no tire load");

        for _ in 0..400 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }

        assert!(!state.is_airborne, "Car should land back on track surface");
        assert!(state.pos_z.abs() < 0.2, "Car should settle near track elevation");
        assert_eq!(state.vel_z, 0.0, "Vertical velocity should reset on landing");
    }

    #[test]
    fn test_mesh_backend_stub_overrides_elevation_from_heightfield() {
        let mut heightmap = crate::procgen::TerrainHeightmap::new(2, 2, 1.0, 0.0, 0.0);
        heightmap.set_height(0, 0, 5.0);
        heightmap.set_height(1, 0, 5.0);
        heightmap.set_height(0, 1, 5.0);
        heightmap.set_height(1, 1, 5.0);

        let track = TrackConfig {
            id: Uuid::new_v4(),
            name: "Heightfield Override Test".to_string(),
            centerline: vec![TrackPoint {
                x: 0.5,
                y: 0.5,
                z: 1.0,
                distance_from_start_m: 0.0,
                width_left_m: 20.0,
                width_right_m: 20.0,
                banking_rad: 0.1,
                camber_rad: 0.0,
                slope_rad: 0.05,
                heading_rad: 0.2,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            }],
            width_m: 40.0,
            source_path: None,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: Some(crate::procgen::ProceduralWorldData {
                environment_type: "test".to_string(),
                seed: 1,
                heightmap: Some(heightmap),
                blend_width: 20.0,
                object_density: 0.0,
                decal_profile: "default".to_string(),
                preset: crate::procgen::EnvironmentPreset::plains(),
            }),
        };

        let centerline_sample = query_track_surface_centerline(&track, 0.5, 0.5)
            .expect("centerline sample should exist");
        let mesh_sample = query_track_surface_mesh_heightfield_stub(&track, 0.5, 0.5)
            .expect("mesh stub sample should exist");

        assert_eq!(centerline_sample.elevation, 1.0, "centerline sample should use centerline z");
        assert!((mesh_sample.elevation - 5.0).abs() < 0.001, "mesh stub should use heightfield elevation");

        assert_eq!(mesh_sample.nearest_point, centerline_sample.nearest_point);
        assert!((mesh_sample.banking_rad - centerline_sample.banking_rad).abs() < 0.0001);
        assert!((mesh_sample.slope_rad - centerline_sample.slope_rad).abs() < 0.0001);
        assert!((mesh_sample.heading_rad - centerline_sample.heading_rad).abs() < 0.0001);
        assert!((mesh_sample.lateral_offset - centerline_sample.lateral_offset).abs() < 0.0001);
        assert_eq!(mesh_sample.surface_type, centerline_sample.surface_type);
        assert!((mesh_sample.grip_modifier - centerline_sample.grip_modifier).abs() < 0.0001);
    }

    fn create_split_height_track(left_z: f32, right_z: f32) -> TrackConfig {
        TrackConfig {
            id: Uuid::new_v4(),
            name: "Split Height Test".to_string(),
            centerline: vec![
                TrackPoint {
                    x: 0.0,
                    y: -1.0,
                    z: left_z,
                    distance_from_start_m: 0.0,
                    width_left_m: 20.0,
                    width_right_m: 20.0,
                    banking_rad: 0.0,
                    camber_rad: 0.0,
                    slope_rad: 0.0,
                    heading_rad: 0.0,
                    surface_type: SurfaceType::Asphalt,
                    grip_modifier: 1.0,
                },
                TrackPoint {
                    x: 0.0,
                    y: 1.0,
                    z: right_z,
                    distance_from_start_m: 1.0,
                    width_left_m: 20.0,
                    width_right_m: 20.0,
                    banking_rad: 0.0,
                    camber_rad: 0.0,
                    slope_rad: 0.0,
                    heading_rad: 0.0,
                    surface_type: SurfaceType::Asphalt,
                    grip_modifier: 1.0,
                },
            ],
            width_m: 40.0,
            source_path: None,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: None,
        }
    }

    #[test]
    fn test_per_wheel_contact_creates_asymmetric_suspension_travel() {
        let mut state = create_test_car_state();
        state.pos_x = 0.0;
        state.pos_y = 0.0;
        state.pos_z = 0.05;

        let config = create_test_config();
        let track = create_split_height_track(0.0, 0.2);
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);

        assert!(
            state.suspension.front_right_travel_m > state.suspension.front_left_travel_m,
            "Right front should compress more on higher right-side contact"
        );
        assert!(
            state.suspension.rear_right_travel_m > state.suspension.rear_left_travel_m,
            "Right rear should compress more on higher right-side contact"
        );
    }

    #[test]
    fn test_anti_roll_coupling_reduces_axle_load_split() {
        let mut state_no_arb = create_test_car_state();
        state_no_arb.pos_x = 0.0;
        state_no_arb.pos_y = 0.0;
        state_no_arb.pos_z = 0.05;

        let mut state_with_arb = state_no_arb.clone();

        let mut config_no_arb = create_test_config();
        config_no_arb.suspension.anti_roll_bar_front = 0.0;
        config_no_arb.suspension.anti_roll_bar_rear = 0.0;

        let mut config_with_arb = config_no_arb.clone();
        config_with_arb.suspension.anti_roll_bar_front = 20000.0;

        let track = create_split_height_track(0.0, 0.2);
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        let dt = 1.0 / 240.0;
        update_car_3d(&mut state_no_arb, &config_no_arb, &input, &track, dt);
        update_car_3d(&mut state_with_arb, &config_with_arb, &input, &track, dt);

        let front_split_no_arb = (state_no_arb.weight_front_right_n - state_no_arb.weight_front_left_n).abs();
        let front_split_with_arb = (state_with_arb.weight_front_right_n - state_with_arb.weight_front_left_n).abs();

        assert!(
            front_split_with_arb < front_split_no_arb,
            "Front anti-roll coupling should reduce left/right front axle load split under asymmetric contact"
        );
    }

    #[test]
    fn test_fuel_consumption() {
        let mut state = create_test_car_state();
        state.fuel_liters = 100.0;
        let config = create_test_config();
        let track = create_test_track();
        
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        let initial_fuel = state.fuel_liters;
        update_car_3d(&mut state, &config, &input, &track, 1.0);

        assert!(state.fuel_liters < initial_fuel, "Fuel should be consumed");
        assert!(state.fuel_consumption_lps > 0.0, "Fuel consumption rate should be positive");
    }

    #[test]
    fn test_damage_system() {
        let config = create_test_config();
        let grid_slot1 = GridSlot { position: 1, x: 0.0, y: 0.0, z: 0.0, yaw_rad: 0.0 };
        let grid_slot2 = GridSlot { position: 2, x: 1.0, y: 0.0, z: 0.0, yaw_rad: 0.0 };
        
        let mut states = vec![
            CarState::new(Uuid::new_v4(), config.id, &grid_slot1),
            CarState::new(Uuid::new_v4(), config.id, &grid_slot2),
        ];
        states[0].speed_mps = 30.0;
        states[0].vel_x = 30.0;
        states[1].speed_mps = 30.0;
        states[1].vel_x = -30.0;

        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        check_aabb_collisions_3d(&mut states, &configs);

        // Check that damage was applied
        let total_damage_0 = states[0].damage.front_damage_percent 
            + states[0].damage.rear_damage_percent
            + states[0].damage.left_damage_percent
            + states[0].damage.right_damage_percent;
        
        assert!(total_damage_0 > 0.0, "Collision should cause damage");
    }

    #[test]
    fn test_legacy_2d_api() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
        };

        // Test that legacy API still works
        update_car_2d(&mut state, &config, &input, 1.0 / 240.0);
        
        assert!(state.speed_mps > 0.0, "Legacy API should work");
    }
}
