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

/// Gravity constant (m/s²)
const GRAVITY: f32 = 9.81;

/// Air density at sea level (kg/m³)
const AIR_DENSITY: f32 = 1.225;

/// Minimum speed threshold for calculations (m/s)
const MIN_SPEED_THRESHOLD: f32 = 0.1;

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
    
    // 8. Calculate individual wheel loads
    let front_weight = static_front_weight + downforce_front - weight_transfer_long;
    let rear_weight = static_rear_weight + downforce_rear + weight_transfer_long;
    
    state.weight_front_left_n = (front_weight / 2.0 + weight_transfer_lat_front).max(0.0);
    state.weight_front_right_n = (front_weight / 2.0 - weight_transfer_lat_front).max(0.0);
    state.weight_rear_left_n = (rear_weight / 2.0 + weight_transfer_lat_rear).max(0.0);
    state.weight_rear_right_n = (rear_weight / 2.0 - weight_transfer_lat_rear).max(0.0);
    
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
    
    // Calculate slip ratios and angles for each wheel
    let _wheel_speed_front = state.speed_mps * (1.0 + state.angular_vel_yaw * config.track_width_front_m / 2.0 / state.speed_mps.max(0.1));
    let _wheel_speed_rear = state.speed_mps * (1.0 + state.angular_vel_yaw * config.track_width_rear_m / 2.0 / state.speed_mps.max(0.1));
    
    // Front left tire
    let fl_slip = calculate_wheel_slip(
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
    let fr_slip = calculate_wheel_slip(
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
    let rl_slip = calculate_wheel_slip(
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
    let rr_slip = calculate_wheel_slip(
        state.speed_mps,
        state.angular_vel_yaw,
        0.0,
        -config.wheelbase_m / 2.0,
        config.track_width_rear_m / 2.0,
        drive_torque_rear / 2.0,
        brake_rear / 2.0,
        config.wheel_radius_m,
    );
    
    // Calculate tire forces
    let fl_forces = calculate_tire_forces(
        state.weight_front_left_n,
        fl_slip.0,
        fl_slip.1,
        effective_grip,
        &config.tire_config,
    );
    let fr_forces = calculate_tire_forces(
        state.weight_front_right_n,
        fr_slip.0,
        fr_slip.1,
        effective_grip,
        &config.tire_config,
    );
    let rl_forces = calculate_tire_forces(
        state.weight_rear_left_n,
        rl_slip.0,
        rl_slip.1,
        effective_grip,
        &config.tire_config,
    );
    let rr_forces = calculate_tire_forces(
        state.weight_rear_right_n,
        rr_slip.0,
        rr_slip.1,
        effective_grip,
        &config.tire_config,
    );
    
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
    let slope_force = config.mass_kg * GRAVITY * track_ctx.slope_rad.sin();
    let banking_force = config.mass_kg * GRAVITY * track_ctx.banking_rad.sin();
    
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
    if !track_ctx.is_on_track {
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
    state.pos_z = track_ctx.elevation; // Snap to track elevation
    
    // 18. Integrate orientation
    state.yaw_rad += state.angular_vel_yaw * dt;
    state.yaw_rad = normalize_angle(state.yaw_rad);
    
    // Match track pitch and roll
    state.pitch_rad = track_ctx.slope_rad;
    state.roll_rad = -track_ctx.banking_rad;
    
    // 19. Store inputs
    state.throttle_input = input.throttle;
    state.brake_input = input.brake;
    state.steering_input = input.steering;
    
    // 20. Update telemetry
    update_telemetry_3d(state, config, input, &track_ctx, fl_slip, fr_slip, rl_slip, rr_slip, dt);
    
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
    let driven_wheel_speed = wheel_speed + (drive_torque - brake_force * wheel_radius) / (100.0 * wheel_radius);
    let slip_ratio = if wheel_vel_x > MIN_SPEED_THRESHOLD {
        (driven_wheel_speed * wheel_radius - wheel_vel_x) / wheel_vel_x
    } else {
        0.0
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

/// Get track context at the car's current position
fn get_track_context(state: &CarState, track: &TrackConfig) -> TrackContext {
    if track.centerline.is_empty() {
        return TrackContext::default();
    }
    
    // Find nearest centerline point
    let mut min_dist_sq = f32::MAX;
    let mut nearest_idx = 0;
    
    for (idx, point) in track.centerline.iter().enumerate() {
        let dx = state.pos_x - point.x;
        let dy = state.pos_y - point.y;
        let dist_sq = dx * dx + dy * dy;
        
        if dist_sq < min_dist_sq {
            min_dist_sq = dist_sq;
            nearest_idx = idx;
        }
    }
    
    let nearest = &track.centerline[nearest_idx];
    
    // Calculate lateral offset (signed distance from centerline)
    let dx = state.pos_x - nearest.x;
    let dy = state.pos_y - nearest.y;
    let cross = dx * nearest.heading_rad.sin() - dy * nearest.heading_rad.cos();
    let lateral_offset = cross;  // Positive = right of centerline
    
    // Check if on track
    let is_on_track = lateral_offset.abs() <= nearest.width_right_m.max(nearest.width_left_m);
    
    // Determine surface type and grip
    let (surface_type, grip_modifier) = if is_on_track {
        (nearest.surface_type, nearest.grip_modifier)
    } else {
        // Off track
        (SurfaceType::Grass, track.track_surface.off_track_grip)
    };
    
    TrackContext {
        nearest_point: nearest_idx,
        elevation: nearest.z,
        banking_rad: nearest.banking_rad,
        slope_rad: nearest.slope_rad,
        heading_rad: nearest.heading_rad,
        lateral_offset,
        is_on_track,
        surface_type,
        grip_modifier,
        width_left: nearest.width_left_m,
        width_right: nearest.width_right_m,
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
    
    // Suspension travel (based on weight and spring rates)
    let calculate_suspension_travel = |weight: f32, spring_rate: f32| -> f32 {
        (weight / spring_rate).clamp(0.0, config.suspension.max_travel_m)
    };
    
    state.suspension.front_left_travel_m = calculate_suspension_travel(
        state.weight_front_left_n,
        config.suspension.spring_rate_front_n_per_m,
    );
    state.suspension.front_right_travel_m = calculate_suspension_travel(
        state.weight_front_right_n,
        config.suspension.spring_rate_front_n_per_m,
    );
    state.suspension.rear_left_travel_m = calculate_suspension_travel(
        state.weight_rear_left_n,
        config.suspension.spring_rate_rear_n_per_m,
    );
    state.suspension.rear_right_travel_m = calculate_suspension_travel(
        state.weight_rear_right_n,
        config.suspension.spring_rate_rear_n_per_m,
    );
    
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

    // Detect lap completion
    if state.current_lap > 0 && old_progress > track_length * 0.8 && state.track_progress < track_length * 0.2 {
        state.current_lap += 1;
        
        let lap_time_ms = (current_tick as f32 * 1000.0 / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);
        
        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }
    }

    // Start lap 1
    if state.current_lap == 0 && state.track_progress > track_length * 0.1 {
        state.current_lap = 1;
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

    if state.current_lap > 0 && old_progress > track_length * 0.8 && state.track_progress < track_length * 0.2 {
        state.current_lap += 1;
        
        let lap_time_ms = (current_tick as f32 * 1000.0 / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);
        
        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }
    }

    if state.current_lap == 0 && state.track_progress > track_length * 0.1 {
        state.current_lap = 1;
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
        };

        // Run several ticks
        for _ in 0..100 {
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        }

        // Car should have moved and adopted track elevation
        assert!(state.pos_x != 0.0 || state.pos_y != 0.0, "Car should have moved");
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
        };

        // Test that legacy API still works
        update_car_2d(&mut state, &config, &input, 1.0 / 240.0);
        
        assert!(state.speed_mps > 0.0, "Legacy API should work");
    }
}
