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
use crate::feedback::{ContactSurface, FeedbackTick};
use crate::walls::{WallKind, Walls};
use std::collections::HashMap;
use std::f32::consts::PI;
use std::sync::Once;
use tracing::debug;
/// Gravity constant (m/s²)
const GRAVITY: f32 = 9.81;

/// Air density at sea level (kg/m³)
pub const AIR_DENSITY: f32 = 1.225;

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
    pub load_n: f32,                 // Vertical load on this wheel
    pub slip_ratio: f32,             // Longitudinal slip
    pub slip_angle_rad: f32,         // Lateral slip angle
    pub grip_force_x: f32,           // Longitudinal grip force
    pub grip_force_y: f32,           // Lateral grip force
    pub contact_z: f32,              // Ground contact elevation
    pub suspension_compression: f32, // Current compression (m)
    pub suspension_velocity_mps: f32,
    pub spring_force_n: f32,
    pub damper_force_n: f32,
    /// Road, curb or off track under this tyre (driver feedback only;
    /// tarmac run-off reads as road there — it is smooth).
    pub surface: ContactSurface,
    /// Past the curb band: off the track for the lap, whatever it is
    /// made of. What the track limits count.
    pub off_track: bool,
}

/// Complete intermediate physics state for a vehicle
#[derive(Debug, Clone, Default)]
pub struct VehiclePhysicsState {
    pub front_left: WheelState,
    pub front_right: WheelState,
    pub rear_left: WheelState,
    pub rear_right: WheelState,

    // Forces in vehicle frame
    pub total_force_x: f32, // Forward (+) / Backward (-)
    pub total_force_y: f32, // Left (-) / Right (+)
    pub total_force_z: f32, // Up (+) / Down (-)

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

static SURFACE_QUERY_BACKEND: std::sync::OnceLock<SurfaceQueryBackend> = std::sync::OnceLock::new();

fn active_surface_query_backend() -> SurfaceQueryBackend {
    // Read the env var once; this runs on every surface query (several times
    // per car per tick), so it must not hit the environment each call.
    *SURFACE_QUERY_BACKEND.get_or_init(|| match std::env::var("APEXSIM_SURFACE_QUERY_BACKEND") {
        Ok(value)
            if value.eq_ignore_ascii_case("mesh")
                || value.eq_ignore_ascii_case("heightfield")
                || value.eq_ignore_ascii_case("mesh_heightfield") =>
        {
            SurfaceQueryBackend::MeshHeightfield
        }
        _ => SurfaceQueryBackend::Centerline,
    })
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

    // Automatic gearbox: a manual shift from the driver always wins, the
    // box only fills in when none was sent this tick.
    let auto_gear = if state.auto_gearbox && input.gear.is_none() {
        auto_gear_selection(state, config, input, dt)
    } else {
        None
    };
    let mut auto_input = PlayerInputData {
        gear: input.gear.or(auto_gear),
        ..*input
    };
    // In reverse the automatic box drives on the brake pedal and brakes on
    // the throttle: the throttle always means forwards, the brake always
    // means stop or back up. Keyed on the gear driving this tick, since a
    // requested shift only lands at the end of it.
    if state.auto_gearbox && state.gear < 0 {
        std::mem::swap(&mut auto_input.throttle, &mut auto_input.brake);
    }
    let input = &auto_input;

    // The DRS flap: open while the driver holds the button where the rules
    // allow it (`crate::drs`), shut the moment the brake goes on.
    state.drs_open = state.drs_allowed && input.drs && input.brake < crate::drs::DRS_BRAKE_CLOSE;

    // Keep fuel capacity in sync with config (for moddable cars)
    state.fuel_capacity_liters = config.fuel.capacity_liters;
    state.fuel_liters = state.fuel_liters.min(state.fuel_capacity_liters);

    // 1. Get track context at current position (windowed search seeded by
    // the previous tick's cached index; cache updated for wheel queries)
    let track_ctx = get_track_context(state, track);
    state.nearest_centerline_idx = Some(track_ctx.nearest_point as u32);
    state.is_on_track = track_ctx.is_on_track;
    state.current_surface = track_ctx.surface_type;
    state.surface_grip_modifier = track_ctx.grip_modifier;
    state.lateral_offset_m = track_ctx.lateral_offset;

    let lateral_query_limit =
        track_ctx.width_left.max(track_ctx.width_right) + SURFACE_QUERY_LATERAL_MARGIN_M;
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
    let (static_front_weight, static_rear_weight) =
        calculate_static_weight_distribution(config, total_weight);
    let (front_axle_x, rear_axle_x) = axle_positions(config);

    // 3. Calculate aerodynamic forces
    let (drag_force, downforce_front, downforce_rear) = calculate_aerodynamic_forces(state, config);
    state.drag_force_n = drag_force;
    state.downforce_front_n = downforce_front;
    state.downforce_rear_n = downforce_rear;

    // 4. Calculate engine torque and RPM
    let (engine_torque, engine_rpm) = calculate_engine_output(state, config, input, dt);
    state.engine_rpm = engine_rpm;

    // 4b. Hybrid system: electric motor assist + brake regeneration
    let motor_torque = update_hybrid_system(state, config, input, engine_rpm, dt);

    // 5. Calculate wheel torques from drivetrain. A partially released
    // clutch transmits proportionally less torque.
    let clutch = input.clutch.unwrap_or(state.clutch_input).clamp(0.0, 1.0);
    let (drive_torque_front, drive_torque_rear) =
        calculate_drive_torques((engine_torque + motor_torque) * clutch, config, state.gear);

    // 6. Calculate brake forces
    let brake_force = input.brake * config.max_brake_force_n;
    let brake_front = brake_force * config.brake_bias_front;
    let brake_rear = brake_force * (1.0 - config.brake_bias_front);

    // 7. Calculate weight transfer.
    // Deliberately uses the PREVIOUS tick's accelerations (g_forces are
    // written at the end of each tick): loads depend on forces which depend
    // on loads, so a same-tick solution would need fixed-point iteration.
    // At 240Hz the one-tick (4.2ms) lag is a stable, standard approximation.
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

    let suspension_rest_length_m =
        (config.suspension.max_travel_m * 0.5).clamp(0.0, config.suspension.max_travel_m);
    let hub_z = state.pos_z + config.wheel_radius_m;

    let cos_yaw = state.yaw_rad.cos();
    let sin_yaw = state.yaw_rad.sin();
    // The body sits parallel to the road under it (grounded pitch and roll
    // follow the track's slope and banking), so each hub rides on that plane
    // rather than all four at the centre's height. Without this a 3% grade
    // parks one axle on its bump stop and the other at full droop, and on a
    // banked road the lower wheels carry kilonewtons more than the upper ones
    // on a dead-straight line.
    let (cos_track, sin_track) = (track_ctx.heading_rad.cos(), track_ctx.heading_rad.sin());
    let (grade, cross_fall) = (track_ctx.slope_rad.tan(), track_ctx.banking_rad.sin());
    let sample_wheel_state = |local_x: f32,
                              local_y: f32,
                              spring_rate_n_per_m: f32,
                              damper_compression: f32,
                              damper_rebound: f32,
                              previous_compression: f32|
     -> WheelState {
        let offset_x = local_x * cos_yaw - local_y * sin_yaw;
        let offset_y = local_x * sin_yaw + local_y * cos_yaw;
        let world_x = state.pos_x + offset_x;
        let world_y = state.pos_y + offset_y;

        // Wheels sit within a couple meters of the car: the car's own
        // nearest index is an excellent hint.
        let sample = query_track_surface(track, world_x, world_y, Some(track_ctx.nearest_point));
        let contact_z = sample.map_or(track_ctx.elevation, |s| s.elevation);
        let contact = sample.map_or(RoadContact::Road, |s| road_contact(track, &s));
        let surface = contact.feedback();
        let off_track = contact.off_track();

        // Along-track and leftward components of the wheel's offset.
        let along = offset_x * cos_track + offset_y * sin_track;
        let left = -offset_x * sin_track + offset_y * cos_track;
        let wheel_hub_z = hub_z + along * grade + left * cross_fall;

        let wheel_extension = (wheel_hub_z - contact_z - config.wheel_radius_m).max(0.0);
        let compression =
            (suspension_rest_length_m - wheel_extension).clamp(0.0, config.suspension.max_travel_m);

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
            surface,
            off_track,
            ..Default::default()
        }
    };

    // Body frame: +y is LEFT, so the left wheels sit at +track/2.
    let mut wheel_front_left = sample_wheel_state(
        front_axle_x,
        config.track_width_front_m / 2.0,
        config.suspension.spring_rate_front_n_per_m,
        config.suspension.damper_compression_front,
        config.suspension.damper_rebound_front,
        prev_fl_compression,
    );
    let mut wheel_front_right = sample_wheel_state(
        front_axle_x,
        -config.track_width_front_m / 2.0,
        config.suspension.spring_rate_front_n_per_m,
        config.suspension.damper_compression_front,
        config.suspension.damper_rebound_front,
        prev_fr_compression,
    );
    let mut wheel_rear_left = sample_wheel_state(
        rear_axle_x,
        config.track_width_rear_m / 2.0,
        config.suspension.spring_rate_rear_n_per_m,
        config.suspension.damper_compression_rear,
        config.suspension.damper_rebound_rear,
        prev_rl_compression,
    );
    let mut wheel_rear_right = sample_wheel_state(
        rear_axle_x,
        -config.track_width_rear_m / 2.0,
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
        // Tire normal load = analytic model (static weight + aero downforce
        // + longitudinal/lateral transfer) plus a ZERO-SUM suspension term.
        // The raw spring/damper forces carry the car's static weight, so
        // adding them outright would double-count it (~2x grip). Only the
        // deviation from the axle mean transfers load — this preserves
        // terrain-asymmetry response (bumps, kerbs) without inflating the
        // total normal force.
        let front_mean_load = (wheel_front_left.load_n + wheel_front_right.load_n) / 2.0;
        let rear_mean_load = (wheel_rear_left.load_n + wheel_rear_right.load_n) / 2.0;
        let susp_delta_fl = wheel_front_left.load_n - front_mean_load;
        let susp_delta_fr = wheel_front_right.load_n - front_mean_load;
        let susp_delta_rl = wheel_rear_left.load_n - rear_mean_load;
        let susp_delta_rr = wheel_rear_right.load_n - rear_mean_load;

        // Lateral transfer is signed with the acceleration (+ = left). Load
        // moves to the OUTSIDE of the turn, i.e. away from the acceleration:
        // accelerating left loads the right wheels. With the sign the other
        // way the inside wheels carry the load, and under ABS braking (every
        // wheel at its load limit) the inside brakes harder, yaws the nose
        // further off line and the car spins from a straight-line stop.
        state.weight_front_left_n =
            (front_weight / 2.0 - weight_transfer_lat_front + susp_delta_fl).max(0.0);
        state.weight_front_right_n =
            (front_weight / 2.0 + weight_transfer_lat_front + susp_delta_fr).max(0.0);
        state.weight_rear_left_n =
            (rear_weight / 2.0 - weight_transfer_lat_rear + susp_delta_rl).max(0.0);
        state.weight_rear_right_n =
            (rear_weight / 2.0 + weight_transfer_lat_rear + susp_delta_rr).max(0.0);

        // The anti-roll bar resists one wheel of an axle compressing more
        // than the other: twisted, it pushes harder on the more compressed
        // wheel and eases off the other, so that wheel carries more load.
        // The body has no roll of its own here, so the difference comes from
        // the ground under each wheel, as when one wheel runs over a bump.
        let front_anti_roll_transfer = config.suspension.anti_roll_bar_front
            * (wheel_front_left.suspension_compression - wheel_front_right.suspension_compression)
            * 0.5;
        state.weight_front_left_n = (state.weight_front_left_n + front_anti_roll_transfer).max(0.0);
        state.weight_front_right_n =
            (state.weight_front_right_n - front_anti_roll_transfer).max(0.0);

        let rear_anti_roll_transfer = config.suspension.anti_roll_bar_rear
            * (wheel_rear_left.suspension_compression - wheel_rear_right.suspension_compression)
            * 0.5;
        state.weight_rear_left_n = (state.weight_rear_left_n + rear_anti_roll_transfer).max(0.0);
        state.weight_rear_right_n = (state.weight_rear_right_n - rear_anti_roll_transfer).max(0.0);

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

    // Body-frame velocities: forward (+x) and leftward (+y) components
    let v_long = state.vel_x * cos_yaw + state.vel_y * sin_yaw;
    let v_lat = -state.vel_x * sin_yaw + state.vel_y * cos_yaw;

    // 9. Calculate steering angle, through the speed-sensitive aid when the
    // driver has it on.
    let steering = if state.steering_assist {
        let front_axle_travel_rad =
            (v_lat + state.angular_vel_yaw * front_axle_x).atan2(v_long.max(MIN_SPEED_THRESHOLD));
        assisted_steering(
            config,
            input.steering,
            state.speed_mps,
            downforce_front + downforce_rear,
            front_axle_travel_rad,
        )
    } else {
        input.steering
    };
    let steering_angle = steering * config.max_steering_angle_rad;

    // Apply Ackermann steering geometry (inner wheel turns more)
    let (steer_left, steer_right) = calculate_ackermann_steering(
        steering_angle,
        config.wheelbase_m,
        config.track_width_front_m,
    );

    // 10. Calculate tire forces using Pacejka-inspired model
    let effective_grip = config.tire_config.grip_coefficient * track_ctx.grip_modifier;
    // Tyre pressure per axle (the garage setup): exactly 1.0 at the
    // optimum, so a stock car is unchanged to the bit.
    let effective_grip_front = effective_grip * config.tire_config.front_grip_factor();
    let effective_grip_rear = effective_grip * config.tire_config.rear_grip_factor();

    // Solve per-wheel tire forces (quasi-static torque balance with
    // wheelspin / lockup / ABS behavior and friction-ellipse coupling)
    let mut fl = WheelForces::default();
    let mut fr = WheelForces::default();
    let mut rl = WheelForces::default();
    let mut rr = WheelForces::default();

    // The driver's own aids win over the car's; an AI or an old client has
    // neither set and drives the car as its file describes it.
    let abs_enabled = state.abs.unwrap_or(config.abs_enabled);
    let traction_control = state
        .traction_control
        .unwrap_or(if config.traction_control_enabled {
            TractionControl::Low
        } else {
            TractionControl::Off
        });

    if is_airborne {
        state.weight_front_left_n = 0.0;
        state.weight_front_right_n = 0.0;
        state.weight_rear_left_n = 0.0;
        state.weight_rear_right_n = 0.0;
    } else {
        fl = solve_wheel_forces(
            v_long,
            v_lat,
            state.angular_vel_yaw,
            steer_left,
            front_axle_x,
            drive_torque_front / 2.0,
            brake_front / 2.0,
            config.wheel_radius_m,
            state.weight_front_left_n,
            effective_grip_front,
            &config.tire_config,
            abs_enabled,
            traction_control,
        );
        fr = solve_wheel_forces(
            v_long,
            v_lat,
            state.angular_vel_yaw,
            steer_right,
            front_axle_x,
            drive_torque_front / 2.0,
            brake_front / 2.0,
            config.wheel_radius_m,
            state.weight_front_right_n,
            effective_grip_front,
            &config.tire_config,
            abs_enabled,
            traction_control,
        );
        rl = solve_wheel_forces(
            v_long,
            v_lat,
            state.angular_vel_yaw,
            0.0,
            rear_axle_x,
            drive_torque_rear / 2.0,
            brake_rear / 2.0,
            config.wheel_radius_m,
            state.weight_rear_left_n,
            effective_grip_rear,
            &config.tire_config,
            abs_enabled,
            traction_control,
        );
        rr = solve_wheel_forces(
            v_long,
            v_lat,
            state.angular_vel_yaw,
            0.0,
            rear_axle_x,
            drive_torque_rear / 2.0,
            brake_rear / 2.0,
            config.wheel_radius_m,
            state.weight_rear_right_n,
            effective_grip_rear,
            &config.tire_config,
            abs_enabled,
            traction_control,
        );
    }

    let fl_slip = (fl.slip_ratio, fl.slip_angle);
    let fr_slip = (fr.slip_ratio, fr.slip_angle);
    let rl_slip = (rl.slip_ratio, rl.slip_angle);
    let rr_slip = (rr.slip_ratio, rr.slip_angle);
    let fl_forces = (fl.fx, fl.fy);
    let fr_forces = (fr.fx, fr.fy);
    let rl_forces = (rl.fx, rl.fy);
    let rr_forces = (rr.fx, rr.fy);
    state.wheel_angular_vel = [fl.omega, fr.omega, rl.omega, rr.omega];

    // 10b. What the driver feels, for the DriverFeedback message. Output
    // only: nothing below reads it back.
    let per_slip_ratio =
        |w: &WheelForces| w.slip_ratio / config.tire_config.optimal_slip_ratio.max(1e-4);
    let per_slip_angle =
        |w: &WheelForces| w.slip_angle / config.tire_config.optimal_slip_angle_rad.max(1e-4);
    let wheels = [&fl, &fr, &rl, &rr];
    let wheel_states = [
        &wheel_front_left,
        &wheel_front_right,
        &wheel_rear_left,
        &wheel_rear_right,
    ];
    let front_tyres = [
        FrontTyre::of(
            &fl,
            state.weight_front_left_n,
            steer_left,
            effective_grip_front,
            &config.tire_config,
        ),
        FrontTyre::of(
            &fr,
            state.weight_front_right_n,
            steer_right,
            effective_grip_front,
            &config.tire_config,
        ),
    ];
    // What one more unit of the driver's input turns the wheels by: full
    // lock, or the aid's slope where the driver is (nothing where it is
    // holding the wheels at the grip limit).
    let aid_gain = if state.steering_assist {
        let front_axle_travel_rad =
            (v_lat + state.angular_vel_yaw * front_axle_x).atan2(v_long.max(MIN_SPEED_THRESHOLD));
        let aided = |input: f32| {
            assisted_steering(
                config,
                input,
                state.speed_mps,
                downforce_front + downforce_rear,
                front_axle_travel_rad,
            )
        };
        let probe = STIFFNESS_PROBE_INPUT;
        (aided(input.steering + probe) - aided(input.steering - probe)) / (2.0 * probe)
    } else {
        1.0
    };
    state.feedback.record_tick(&FeedbackTick {
        steer_torque: steering_column_torque(front_tyres, config, static_front_weight),
        steer_input: input.steering,
        steer_stiffness: steering_column_stiffness(
            front_tyres,
            config,
            static_front_weight,
            steering_angle,
            config.max_steering_angle_rad * aid_gain,
        ),
        front_load: (state.weight_front_left_n + state.weight_front_right_n)
            / static_front_weight.max(1.0),
        slip_ratio: wheels.map(per_slip_ratio),
        slip_angle: wheels.map(per_slip_angle),
        surface: if is_airborne {
            [ContactSurface::Road; 4]
        } else {
            wheel_states.map(|w| w.surface)
        },
        suspension_mps: wheel_states.map(|w| w.suspension_velocity_mps),
        abs_active: wheels.iter().any(|w| w.abs_active),
        tc_active: wheels.iter().any(|w| w.tc_active),
    });

    // Track limits are judged on the same per-wheel surfaces the force
    // feedback uses: all four past the curb band is off the track. A car in
    // the air is not counted — it left from somewhere, and that tick already
    // counted.
    state.wheels_off_track = !is_airborne && wheel_states.iter().all(|w| w.off_track);

    // 11. Sum all forces
    // Rotate front tire forces by steering angle
    let fl_force_x = fl_forces.0 * steer_left.cos() - fl_forces.1 * steer_left.sin();
    let fl_force_y = fl_forces.0 * steer_left.sin() + fl_forces.1 * steer_left.cos();
    let fr_force_x = fr_forces.0 * steer_right.cos() - fr_forces.1 * steer_right.sin();
    let fr_force_y = fr_forces.0 * steer_right.sin() + fr_forces.1 * steer_right.cos();

    // Total forces in vehicle frame. Drag opposes the motion, which is
    // rearwards only while the car is going forwards.
    let drag_force_x = if v_long < 0.0 {
        -drag_force
    } else {
        drag_force
    };
    let total_force_x = fl_force_x + fr_force_x + rl_forces.0 + rr_forces.0 - drag_force_x;
    let total_force_y = fl_force_y + fr_force_y + rl_forces.1 + rr_forces.1;

    // Include gravity components on slopes
    let slope_force = if is_airborne {
        0.0
    } else {
        config.mass_kg * GRAVITY * track_ctx.slope_rad.sin()
    };
    // Banking: the share of gravity along a cambered road pulls the car
    // down the slope, toward the low edge. Positive banking lifts the left
    // edge (the convention the road mesh and `surface_elevation` share), so
    // the pull points at the road's right, (sin h, -cos h) in the world.
    // It used to be added to the car's lateral axis as +m g sin(bank) —
    // pushing up the bank — which made every banked right-hander shed the
    // car outward; and it ignored which way the car pointed.
    let (banking_force_x, banking_force_y) = if is_airborne {
        (0.0, 0.0)
    } else {
        let pull = config.mass_kg * GRAVITY * track_ctx.banking_rad.sin();
        let rel = state.yaw_rad - track_ctx.heading_rad;
        (-pull * rel.sin(), -pull * rel.cos())
    };

    // 12. Calculate yaw moment: front tires ahead of CoG, rear tires behind,
    // plus lateral offsets of left/right wheels.
    let yaw_moment = (fl_force_y + fr_force_y) * front_axle_x
        + (rl_forces.1 + rr_forces.1) * rear_axle_x
        + (fr_force_x - fl_force_x) * (config.track_width_front_m / 2.0)
        + (rr_forces.0 - rl_forces.0) * (config.track_width_rear_m / 2.0);

    // 13. Calculate accelerations
    let accel_x = (total_force_x - slope_force + banking_force_x) / config.mass_kg;
    let accel_y = (total_force_y + banking_force_y) / config.mass_kg;

    // Yaw moment of inertia (simplified as rectangular body)
    let yaw_inertia = config.mass_kg * (config.length_m.powi(2) + config.width_m.powi(2)) / 12.0;
    let angular_accel_yaw = yaw_moment / yaw_inertia;

    // 14. Update G-forces
    state.g_forces.longitudinal_g = accel_x / GRAVITY;
    state.g_forces.lateral_g = accel_y / GRAVITY;
    state.g_forces.vertical_g =
        1.0 + (downforce_front + downforce_rear) / (config.mass_kg * GRAVITY);

    // 15. Integrate velocities
    // Transform acceleration from vehicle frame to world frame
    let cos_yaw = state.yaw_rad.cos();
    let sin_yaw = state.yaw_rad.sin();

    let accel_world_x = accel_x * cos_yaw - accel_y * sin_yaw;
    let accel_world_y = accel_x * sin_yaw + accel_y * cos_yaw;

    state.vel_x += accel_world_x * dt;
    state.vel_y += accel_world_y * dt;

    // Off-track rolling drag: a fixed deceleration against the planar
    // velocity, never enough to reverse it. The grass grip already makes a
    // shortcut slow; this only adds the soft ground's resistance, so a car
    // that went off can drive back at a sensible speed.
    if !is_airborne && !track_ctx.is_on_track {
        let planar = (state.vel_x.powi(2) + state.vel_y.powi(2)).sqrt();
        if planar > 0.0 {
            let scale = (1.0 - track.track_surface.off_track_drag_mps2 * dt / planar).max(0.0);
            state.vel_x *= scale;
            state.vel_y *= scale;
        }
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

    // Apply angular damping, scaled by dt so behavior is tick-rate
    // independent (0.995/tick at 240Hz ⇒ decay rate ln(0.995)·240 ≈ 1.2/s).
    const YAW_DAMPING_RATE_PER_SEC: f32 = 1.2029;
    state.angular_vel_yaw *= (-YAW_DAMPING_RATE_PER_SEC * dt).exp();

    // Low-speed kinematic constraint: below walking-to-jogging speeds real
    // tires roll instead of sliding, so yaw follows bicycle-model kinematics
    // (v/L·tan(δ)). The pure force-based yaw dynamics are singular near
    // standstill (huge moments from clamped slip angles at ~zero speed) and
    // make the car pirouette. Blend from kinematic to dynamic with speed.
    const KINEMATIC_BLEND_SPEED_MPS: f32 = 8.0;
    if !is_airborne && state.speed_mps < KINEMATIC_BLEND_SPEED_MPS {
        // Signed with the direction of travel: backing up with the wheels
        // turned left swings the nose right.
        let reversing = state.vel_x * cos_yaw + state.vel_y * sin_yaw < 0.0;
        let signed_speed = if reversing {
            -state.speed_mps
        } else {
            state.speed_mps
        };
        let kinematic_yaw_rate = signed_speed / config.wheelbase_m.max(0.1) * steering_angle.tan();
        let blend = (state.speed_mps / KINEMATIC_BLEND_SPEED_MPS).clamp(0.0, 1.0);
        state.angular_vel_yaw = kinematic_yaw_rate * (1.0 - blend) + state.angular_vel_yaw * blend;
    }

    // 17. Integrate position
    state.pos_x += state.vel_x * dt;
    state.pos_y += state.vel_y * dt;

    if is_airborne {
        state.vel_z -= GRAVITY * dt;
        state.pos_z += state.vel_z * dt;
    }

    let post_track_ctx = get_track_context(state, track);
    state.nearest_centerline_idx = Some(post_track_ctx.nearest_point as u32);
    // With a baked ground heightfield the surface is defined everywhere and
    // the car follows it wherever it goes. Without one, the centerline height
    // is only meaningful near the road, so beyond a margin z is held.
    let post_lateral_query_limit =
        post_track_ctx.width_left.max(post_track_ctx.width_right) + SURFACE_QUERY_LATERAL_MARGIN_M;
    let can_query_post_surface =
        track.ground.is_some() || post_track_ctx.lateral_offset.abs() <= post_lateral_query_limit;

    // Height a grounded car will follow the surface DOWN in one tick before
    // being considered launched (crest handling). Suspension keeps wheels in
    // contact over ordinary elevation changes; only a genuine drop-off (or
    // enough upward velocity) makes the car airborne.
    const GROUND_FOLLOW_MAX_DROP_M: f32 = 0.35;

    if can_query_post_surface {
        if is_airborne {
            if state.pos_z <= post_track_ctx.elevation {
                state.pos_z = post_track_ctx.elevation;
                state.vel_z = 0.0;
                is_airborne = false;
            }
        } else {
            let gap = state.pos_z - post_track_ctx.elevation;
            if gap <= GROUND_FOLLOW_MAX_DROP_M {
                // Stay glued to the surface: snap down as well as up, so
                // cresting a hill doesn't flag the car airborne and bounce
                // the wheel loads every tick.
                state.pos_z = post_track_ctx.elevation;
                state.vel_z = 0.0;
            } else {
                // The surface dropped away faster than suspension can
                // follow: the car is genuinely launched.
                is_airborne = true;
            }
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
    // What the front wheels were given, after the steering aid, so the
    // cockpit wheel turns as far as the road wheels do.
    state.steering_input = steering;

    // Apply gear and clutch inputs if provided. The box refuses to change
    // direction while the car is still rolling the other way.
    if let Some(gear) = input.gear {
        let v_long_now = state.vel_x * cos_yaw + state.vel_y * sin_yaw;
        if gear_engages(state.gear, gear, v_long_now) {
            state.gear = gear;
        }
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

/// Where the axles sit along the body relative to the centre of gravity
/// (+x forward): the front axle `(1 - w_f)·L` ahead of it and the rear
/// `w_f·L` behind, from the static weight split `w_f`. These are the lever
/// arms of the tyre forces about the CoG. With both axles at `L/2` the
/// steady-state yaw balance demanded equal lateral force from each axle,
/// so the lighter axle (the rear, on every car with a forward weight bias)
/// hit its limit first and every car oversteered at the limit; with the
/// arms in the right place the axles reach their limits together, and the
/// balance is set by aero and suspension as it should be.
fn axle_positions(config: &CarConfig) -> (f32, f32) {
    let w_f = config.weight_distribution_front.clamp(0.05, 0.95);
    (config.wheelbase_m * (1.0 - w_f), -config.wheelbase_m * w_f)
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

    // The open DRS flap takes its share off the drag and the rear wing.
    let (drag_scale, rear_scale) = match (state.drs_open, config.drs) {
        (true, Some(drs)) => (1.0 - drs.drag_reduction, 1.0 - drs.rear_downforce_reduction),
        _ => (1.0, 1.0),
    };

    // Drag force
    let drag = dynamic_pressure * config.drag_coefficient * config.frontal_area_m2 * drag_scale;

    // Downforce (lift coefficients are negative for downforce)
    let downforce_front =
        -dynamic_pressure * config.lift_coefficient_front * config.frontal_area_m2;
    let downforce_rear =
        -dynamic_pressure * config.lift_coefficient_rear * config.frontal_area_m2 * rear_scale;

    (drag, downforce_front.max(0.0), downforce_rear.max(0.0))
}

/// Time constant of a free-revving engine climbing toward the revs the
/// throttle asks for (nothing but its own inertia to turn).
const FREE_REV_RISE_S: f32 = 0.25;
/// Time constant of a free-revving engine falling back toward idle.
const FREE_REV_FALL_S: f32 = 0.5;
/// How far the limiter's ignition cut drops the revs before they climb
/// again, so a flat-out engine in neutral bounces off the limiter.
const FREE_REV_LIMITER_DROP_RPM: f32 = 250.0;

/// Engine rpm one tick on from `rpm` with nothing connected to the wheels
/// (neutral): the throttle opening sets a target between idle and just past
/// the limiter, which the revs chase with the engine's inertia and the
/// limiter cuts.
pub fn free_rev_rpm(rpm: f32, config: &CarConfig, throttle: f32, dt: f32) -> f32 {
    let limiter = config
        .engine
        .rev_limiter_rpm
        .max(config.redline_rpm)
        .min(config.max_engine_rpm.max(config.idle_rpm));
    let span = (limiter - config.idle_rpm).max(0.0);
    // Aim past the limiter so full throttle reaches it rather than creeping
    // up on it forever.
    let target = config.idle_rpm + throttle.clamp(0.0, 1.0) * span * 1.1;
    let rpm = rpm.max(config.idle_rpm);
    let tau = if target > rpm {
        FREE_REV_RISE_S
    } else {
        FREE_REV_FALL_S
    };
    let next = rpm + (target - rpm) * (1.0 - (-dt / tau).exp());
    if next >= limiter {
        (limiter - FREE_REV_LIMITER_DROP_RPM).max(config.idle_rpm)
    } else {
        next
    }
}

/// A car held on the grid during the countdown: it cannot move, but the
/// driver can pick a gear and blip the engine against the limiter. The
/// automatic box does nothing here; it takes first when the lights go out.
pub fn update_car_on_grid(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) {
    if let Some(gear) = input.gear {
        if gear_engages(state.gear, gear, 0.0) {
            state.gear = gear;
        }
    }
    // With the car held, the clutch is in whatever gear is selected.
    state.engine_rpm = free_rev_rpm(state.engine_rpm, config, input.throttle, dt);
    state.throttle_input = input.throttle;
    state.brake_input = input.brake;
    state.steering_input = input.steering;
    state.auto_reverse_ticks = 0;
}

/// Calculate engine output torque and RPM
fn calculate_engine_output(
    state: &CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) -> (f32, f32) {
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
        0.0 // Neutral
    } else {
        config.gear_ratios[0] // Reverse
    };

    let engine_rpm = if gear_ratio.abs() > 0.001 {
        let geared_rpm = (wheel_rpm * gear_ratio.abs() * config.final_drive_ratio)
            .clamp(config.idle_rpm, config.max_engine_rpm);
        // Clutch-slip launch: at low road speed in the launch gears the
        // clutch slips, letting the engine rev toward a throttle-dependent
        // launch RPM and deliver its torque there instead of being pinned
        // to idle by the (near-zero) wheel speed.
        const CLUTCH_SLIP_MAX_SPEED_MPS: f32 = 10.0;
        const CLUTCH_SLIP_MAX_GEAR: i8 = 2;
        if state.speed_mps < CLUTCH_SLIP_MAX_SPEED_MPS
            && (state.gear < 0 || (1..=CLUTCH_SLIP_MAX_GEAR).contains(&state.gear))
            && input.throttle > 0.05
        {
            let launch_rpm =
                config.idle_rpm + input.throttle * (config.redline_rpm - config.idle_rpm) * 0.45;
            geared_rpm.max(launch_rpm)
        } else {
            geared_rpm
        }
    } else {
        free_rev_rpm(state.engine_rpm, config, input.throttle, dt)
    };

    let torque_at_rpm = engine_curve_torque_nm(config, engine_rpm);

    // Rev limiter torque cut
    let limiter_rpm = config.engine.rev_limiter_rpm.max(config.redline_rpm);
    let limiter_cut = if engine_rpm >= limiter_rpm {
        // Hard cut near limiter
        0.2
    } else {
        1.0
    };

    // Engine braking & friction
    let rpm_frac = ((engine_rpm - config.idle_rpm)
        / (config.redline_rpm - config.idle_rpm).max(1.0))
    .clamp(0.0, 1.0);
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

/// Torque the engine makes at `rpm` with the throttle wide open, before the
/// limiter cut: the car's torque curve, or the legacy parabola (peak at ~60%
/// of the redline) for a car that has none.
fn engine_curve_torque_nm(config: &CarConfig, rpm: f32) -> f32 {
    if !config.engine.torque_curve.is_empty() {
        interpolate_torque_curve(&config.engine.torque_curve, rpm)
    } else {
        let rpm_normalized = (rpm - config.idle_rpm) / (config.redline_rpm - config.idle_rpm);
        let torque_factor = 1.0 - (rpm_normalized - 0.6).powi(2);
        config.max_engine_torque_nm * torque_factor.clamp(0.3, 1.0)
    }
}

/// What reaches the gearbox at full throttle: the curve less the engine's
/// internal friction, which grows with revs exactly as it does on the road.
fn full_throttle_net_torque_nm(config: &CarConfig, rpm: f32) -> f32 {
    let rpm_frac =
        ((rpm - config.idle_rpm) / (config.redline_rpm - config.idle_rpm).max(1.0)).clamp(0.0, 1.0);
    engine_curve_torque_nm(config, rpm) - config.engine.friction_torque_nm * rpm_frac
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

/// Hybrid system update: returns the electric motor's assist torque (Nm at
/// the crank) and updates the battery state of charge on the car. Under
/// braking the motor regenerates instead of assisting.
fn update_hybrid_system(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    engine_rpm: f32,
    dt: f32,
) -> f32 {
    let hybrid = &config.hybrid;
    if !hybrid.enabled {
        return 0.0;
    }

    // Seed the battery from config on first use (serde default is -1.0).
    if state.hybrid_battery_kwh < 0.0 {
        state.hybrid_battery_kwh = hybrid.battery_capacity_kwh;
    }

    const KWH_PER_JOULE: f32 = 1.0 / 3.6e6;

    // Regeneration under braking: charge the battery, no assist.
    if input.brake > 0.1 && state.speed_mps > 3.0 {
        let regen_kw = hybrid
            .regen_max_power_kw
            .min(hybrid.battery_max_charge_kw)
            .max(0.0)
            * input.brake;
        state.hybrid_battery_kwh = (state.hybrid_battery_kwh
            + regen_kw * 1000.0 * dt * KWH_PER_JOULE)
            .min(hybrid.battery_capacity_kwh);
        return 0.0;
    }

    // Assist under throttle, limited by motor torque, motor/battery power
    // and remaining charge.
    if input.throttle > 0.05 && state.hybrid_battery_kwh > 0.0 {
        let omega = (engine_rpm / 60.0) * 2.0 * PI; // rad/s
        let power_limit_w = hybrid
            .motor_max_power_kw
            .min(hybrid.battery_max_discharge_kw)
            .max(0.0)
            * 1000.0;
        let torque_from_power = if omega > 1.0 {
            power_limit_w / omega
        } else {
            hybrid.motor_max_torque_nm
        };
        let motor_torque = (input.throttle * hybrid.motor_max_torque_nm)
            .min(torque_from_power)
            .max(0.0);
        let drawn_w = motor_torque * omega.max(1.0);
        state.hybrid_battery_kwh =
            (state.hybrid_battery_kwh - drawn_w * dt * KWH_PER_JOULE).max(0.0);
        return motor_torque;
    }

    0.0
}

/// Calculate drive torques for front and rear axles
fn calculate_drive_torques(engine_torque: f32, config: &CarConfig, gear: i8) -> (f32, f32) {
    if gear == 0 {
        return (0.0, 0.0); // Neutral
    }

    let gear_ratio = if gear > 0 && (gear as usize) < config.gear_ratios.len() {
        config.gear_ratios[gear as usize]
    } else {
        config.gear_ratios[0] // Reverse
    };

    let total_ratio = gear_ratio * config.final_drive_ratio;
    let wheel_torque = engine_torque * total_ratio * config.transmission.efficiency.clamp(0.0, 1.0);

    match config.drivetrain {
        Drivetrain::FWD => (wheel_torque, 0.0),
        Drivetrain::RWD => (0.0, wheel_torque),
        Drivetrain::AWD => (wheel_torque * 0.4, wheel_torque * 0.6), // 40/60 split
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
    let weight_transfer_long =
        (config.mass_kg * longitudinal_accel * config.cog_height_m) / config.wheelbase_m;

    // Lateral weight transfer, split front/rear by roll stiffness. Each
    // axle's roll stiffness comes from its springs acting across the track
    // width, with the anti-roll bar as an additional term — with zero ARBs
    // the springs still resist roll, so lateral transfer never vanishes.
    let front_roll_stiffness = (config.suspension.spring_rate_front_n_per_m
        + config.suspension.anti_roll_bar_front)
        * config.track_width_front_m.powi(2)
        / 2.0;
    let rear_roll_stiffness = (config.suspension.spring_rate_rear_n_per_m
        + config.suspension.anti_roll_bar_rear)
        * config.track_width_rear_m.powi(2)
        / 2.0;
    let total_roll_stiffness = front_roll_stiffness + rear_roll_stiffness;
    let front_roll_ratio = front_roll_stiffness / total_roll_stiffness.max(1.0);
    let rear_roll_ratio = rear_roll_stiffness / total_roll_stiffness.max(1.0);

    let lateral_transfer_front = (config.mass_kg * lateral_accel * config.cog_height_m)
        / config.track_width_front_m
        * front_roll_ratio;
    let lateral_transfer_rear = (config.mass_kg * lateral_accel * config.cog_height_m)
        / config.track_width_rear_m
        * rear_roll_ratio;

    (
        weight_transfer_long,
        lateral_transfer_front,
        lateral_transfer_rear,
    )
}

/// Wheel angle of the tightest turn the car can hold at `speed_mps`: the
/// radius at which cornering takes all its grip (downforce included), as a
/// steering angle `L·a/v²`.
pub fn grip_limit_lock_rad(config: &CarConfig, speed_mps: f32, downforce_n: f32) -> f32 {
    let grip_accel = config.tire_config.grip_coefficient
        * (GRAVITY + downforce_n.max(0.0) / config.mass_kg.max(1.0));
    config.wheelbase_m * grip_accel / (speed_mps * speed_mps).max(1e-3)
}

/// Lock the steering aid allows past the kinematic turn angle, as a share of
/// the tyre's peak slip angle. The front tyres only make their peak force at
/// that slip, so with no allowance a stick at the stop held the car short of
/// its grip once the axles were balanced (`axle_positions`). Half the peak
/// is inside the flat top of the curve, where extra lock is force, not scrub.
const STEERING_AID_SLIP_ALLOWANCE: f32 = 0.5;

/// The speed-sensitive steering aid: the driver's input (-1..1, + = left)
/// turned into the share of the rack's lock the front wheels get.
///
/// Full input asks for the tightest turn the car can hold at this speed and
/// no more, so a pad's stick at the stop never overdrives the tyres: at
/// walking pace that is full lock, at 300 km/h about 2° for an F1 and under
/// half a degree for a road car with little downforce.
/// Any lock past it would only make the car rotate faster than it can
/// corner, which is how a flick of the stick at speed became a spin.
///
/// The lock towards whichever side the front axle is already travelling
/// grows by that angle, so the stick can still point the wheels where the
/// car is going and catch a slide. Centred, the wheels stay straight: the
/// aid never countersteers by itself.
pub fn assisted_steering(
    config: &CarConfig,
    input: f32,
    speed_mps: f32,
    downforce_n: f32,
    front_axle_travel_rad: f32,
) -> f32 {
    let full_lock = config.max_steering_angle_rad.max(1e-3);
    let toward_travel = (input.signum() * front_axle_travel_rad).max(0.0);
    let slip_allowance = STEERING_AID_SLIP_ALLOWANCE * config.tire_config.optimal_slip_angle_rad;
    let lock =
        (grip_limit_lock_rad(config, speed_mps, downforce_n) + slip_allowance + toward_travel)
            .min(full_lock);
    (input.clamp(-1.0, 1.0) * lock / full_lock).clamp(-1.0, 1.0)
}

/// Ackermann steering geometry: (left, right) wheel angles for a centre
/// angle, positive to the LEFT as everywhere in the sim. The inner wheel runs
/// the tighter circle, so it turns further.
fn calculate_ackermann_steering(
    steering_angle: f32,
    wheelbase: f32,
    track_width: f32,
) -> (f32, f32) {
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
        // Turning left: the left wheel is inner
        (inner_angle, outer_angle)
    } else {
        // Turning right: the right wheel is inner
        (-outer_angle, -inner_angle)
    }
}

/// Magic-formula shape factors. Slip inputs are NORMALIZED (1.0 = the
/// tire's optimal slip), so the stiffness factor B places the curve's peak
/// at exactly 1.0: sin(C·atan(B·x)) peaks at B·x = tan(π/(2C)).
const PACEJKA_C_LONG: f32 = 1.9;
const PACEJKA_C_LAT: f32 = 1.3;

/// Absolute slip-ratio ceiling for wheelspin (deep in the magic formula's
/// falloff region). Reached when drive torque exceeds traction ~3x.
const WHEELSPIN_MAX_SLIP_RATIO: f32 = 0.35;
/// High traction control caps drive at this share of the longitudinal grip
/// left beside the lateral force, so a corner exit keeps a little in hand.
const TC_HIGH_LATERAL_MARGIN: f32 = 0.9;

/// Body-frame speed below which brakes produce no tire force (the low-speed
/// stop snap handles the final halt) and the launch model applies.
const BRAKE_DEADZONE_SPEED_MPS: f32 = 0.15;

fn pacejka(d: f32, c: f32, normalized_slip: f32) -> f32 {
    let b = (std::f32::consts::PI / (2.0 * c)).tan();
    d * (c * (b * normalized_slip).atan()).sin()
}

/// Invert the magic formula in its stable region: the normalized slip at
/// which the tire produces `force` (|force| <= d).
fn pacejka_inverse(d: f32, c: f32, force: f32) -> f32 {
    let b = (std::f32::consts::PI / (2.0 * c)).tan();
    (((force / d).clamp(-1.0, 1.0)).asin() / c).tan() / b
}

/// Per-wheel longitudinal/lateral force solution.
#[derive(Clone, Copy, Default)]
struct WheelForces {
    fx: f32,
    fy: f32,
    slip_ratio: f32,
    slip_angle: f32,
    /// Wheel angular velocity (rad/s) consistent with the slip solution.
    omega: f32,
    /// ABS held the wheel at peak slip instead of letting it lock.
    abs_active: bool,
    /// Traction control cut drive torque to the traction limit.
    tc_active: bool,
}

/// Where the zero-slip pneumatic trail has shrunk to nothing, in multiples of
/// the tyre's peak slip angle. As the rear of the contact patch starts to
/// slide, its centre of pressure walks forward toward the steering axis. The
/// aligning torque therefore peaks at about half the peak slip, well before
/// the lateral force does, and falls away through the limit: the wheel going
/// light is how a driver feels the fronts letting go. At 2.0 the fall was
/// 14% by the peak and nobody could feel it; at 1.4 it is about a third.
const PNEUMATIC_TRAIL_ZERO_AT_SLIP: f32 = 1.4;

/// How far past zero the pneumatic trail goes once the whole patch slides, as
/// a share of its zero-slip length. A real tyre's centre of pressure ends up
/// just ahead of the contact centre, so a washed-out front is lighter than
/// the caster alone would make it.
const PNEUMATIC_TRAIL_FLOOR: f32 = -0.1;

/// Caster (mechanical) trail as a share of the zero-slip pneumatic trail.
/// It never shrinks, so a sliding front still pulls the wheel toward where
/// the car is going, which is what winds the rim into a countersteer.
const MECHANICAL_TRAIL_SHARE: f32 = 0.35;

/// Scrub radius as a share of the zero-slip pneumatic trail: how far outboard
/// of the kingpin axis the contact patch sits. A longitudinal force on one
/// front tyre turns the wheel toward that side, so a tyre braking harder than
/// its partner (loaded on the outside of a corner, or on a different surface)
/// tugs the rim. Equal forces cancel.
const SCRUB_RADIUS_SHARE: f32 = 0.25;

/// Kingpin-inclination jacking arm, in zero-slip-trail units: steering lifts
/// the front of the car, so the axle's weight pulls the wheels back to the
/// centre. Small at racing lock; it is what centres the wheel at a crawl,
/// where the tyres have no slip to align with, and it grows with the load
/// braking puts on the front.
const JACKING_ARM_SHARE: f32 = 0.6;

/// Longest the contact patch grows over its static length, however much load
/// is on the tyre.
const MAX_CONTACT_PATCH_GROWTH: f32 = 2.0;

/// One front tyre as the steering column sees it.
#[derive(Clone, Copy, Debug, Default)]
struct FrontTyre {
    /// Lateral force, wheel frame, + = left.
    fy: f32,
    /// Longitudinal force, wheel frame, + = driving.
    fx: f32,
    slip_angle: f32,
    load_n: f32,
    steer_rad: f32,
    /// Radius of the tyre's friction circle, N (`d` in the magic formula).
    grip_n: f32,
    /// How much of the magic formula's lateral force the friction ellipse
    /// left the tyre (1 unless braking or drive took some of it), so the
    /// force can be solved again at a slightly different slip angle.
    lateral_scale: f32,
}

impl FrontTyre {
    fn of(
        forces: &WheelForces,
        load_n: f32,
        steer_rad: f32,
        grip_coefficient: f32,
        tire: &TireConfig,
    ) -> Self {
        let grip_n = grip_coefficient * load_n.max(0.0);
        let unscaled = -pacejka(
            grip_n,
            PACEJKA_C_LAT,
            forces.slip_angle / tire.optimal_slip_angle_rad.max(1e-4),
        );
        let lateral_scale = if unscaled.abs() > 1e-3 * grip_n.max(1.0) {
            (forces.fy / unscaled).clamp(0.0, 1.0)
        } else {
            1.0
        };
        Self {
            fy: forces.fy,
            fx: forces.fx,
            slip_angle: forces.slip_angle,
            load_n,
            steer_rad,
            grip_n,
            lateral_scale,
        }
    }

    /// The same tyre with its wheel turned `delta_rad` further left: the slip
    /// angle falls by as much, and the lateral force follows the magic
    /// formula under the same share of the friction ellipse.
    fn steered_by(self, delta_rad: f32, tire: &TireConfig) -> Self {
        let slip_angle = (self.slip_angle - delta_rad).clamp(-0.5, 0.5);
        let fy = -pacejka(
            self.grip_n,
            PACEJKA_C_LAT,
            slip_angle / tire.optimal_slip_angle_rad.max(1e-4),
        ) * self.lateral_scale;
        Self {
            fy,
            slip_angle,
            steer_rad: self.steer_rad + delta_rad,
            ..self
        }
    }
}

/// Steering input the column stiffness is measured over, either side of the
/// driver's own: a hundredth of full lock, a couple of degrees of rim.
const STIFFNESS_PROBE_INPUT: f32 = 0.01;

/// How the column torque changes with the steering input, per unit of input
/// (full lock is 1), around where the driver is holding it, in the torque's
/// units and sign. Negative while the tyres are gripping: turn the wheel
/// further left and the rim pushes harder to the right. It falls to nothing
/// at the aligning torque's crest and turns positive past it, where the
/// fronts are letting go.
///
/// The torque reaches the driver a network round trip after the rim moved,
/// which on its own is a spring that answers late: let go of the wheel in a
/// corner and it holds where it was for a few hundredths of a second, then
/// jumps. With this slope a wheel can work out the torque at the rim's
/// position *now* from the newest sample, and only what the slope does not
/// explain (the car's own motion) arrives late.
///
/// `steering_angle` is the centre angle the wheels are at; each wheel is
/// moved to where the Ackermann geometry puts it for the nudged angle, since
/// the inner wheel turns further than the outer.
fn steering_column_stiffness(
    front: [FrontTyre; 2],
    config: &CarConfig,
    static_front_load_n: f32,
    steering_angle: f32,
    rad_per_input: f32,
) -> f32 {
    let delta = STIFFNESS_PROBE_INPUT * rad_per_input;
    let at = |angle: f32| {
        let (left, right) =
            calculate_ackermann_steering(angle, config.wheelbase_m, config.track_width_front_m);
        let [fl, fr] = front;
        steering_column_torque(
            [
                fl.steered_by(left - fl.steer_rad, &config.tire_config),
                fr.steered_by(right - fr.steer_rad, &config.tire_config),
            ],
            config,
            static_front_load_n,
        )
    };
    (at(steering_angle + delta) - at(steering_angle - delta)) / (2.0 * STIFFNESS_PROBE_INPUT)
}

/// Torque the two front tyres put into the steering column, `[left, right]`.
/// Positive turns the wheel left, the steering sign, so in a corner the
/// aligning part is against the lock: the self-centring a driver steers
/// against. Per tyre it is
///
/// - **aligning**: `-Fy · (pneumatic + caster trail)`. The pneumatic trail
///   is the contact patch's length, which grows with the square root of the
///   tyre's load, times a shape that shrinks with slip (see
///   [`PNEUMATIC_TRAIL_ZERO_AT_SLIP`]). The load is the tyre's own, so the
///   outside front loaded in a corner, the fronts loaded under braking and
///   the downforce at speed all make the rim heavier, and a front unloaded
///   over a crest or under power makes it light;
/// - **scrub**: `-side · scrub · Fx`, the pull toward a tyre braking harder
///   than its partner;
/// - **jacking**: `-load · arm · sin(steer)`, the axle's weight centring
///   the wheel.
///
/// 1.0 is the car's reference: the front axle cornering at its static grip
/// limit on zero-slip trail. Downforce and load transfer take it past 1. The
/// value is not clamped, because a device with more headroom can use it.
///
/// Shape of the aligning part, for an evenly and statically loaded axle at
/// slip `n` times the tyre's peak (C_LAT = 1.3): 0.53 at n = 0.2, a crest of
/// 0.69 around 0.45, 0.47 at the peak, 0.21 at 1.5 and about 0.18 from 2 on.
fn steering_column_torque(
    front: [FrontTyre; 2],
    config: &CarConfig,
    static_front_load_n: f32,
) -> f32 {
    let reference =
        config.tire_config.grip_coefficient * static_front_load_n * (1.0 + MECHANICAL_TRAIL_SHARE);
    if reference <= 1.0 {
        return 0.0;
    }
    let peak_slip = config.tire_config.optimal_slip_angle_rad.max(1e-4);
    let static_tyre_load = (static_front_load_n / 2.0).max(1.0);
    let moment: f32 = front
        .iter()
        .zip([1.0f32, -1.0])
        .map(|(tyre, side)| {
            let n = (tyre.slip_angle / peak_slip).abs();
            let patch = (tyre.load_n.max(0.0) / static_tyre_load)
                .sqrt()
                .min(MAX_CONTACT_PATCH_GROWTH);
            let pneumatic =
                patch * (1.0 - n / PNEUMATIC_TRAIL_ZERO_AT_SLIP).max(PNEUMATIC_TRAIL_FLOOR);
            let aligning = -tyre.fy * (pneumatic + MECHANICAL_TRAIL_SHARE);
            let scrub = -side * SCRUB_RADIUS_SHARE * tyre.fx;
            let jacking = -tyre.load_n.max(0.0) * JACKING_ARM_SHARE * tyre.steer_rad.sin();
            aligning + scrub + jacking
        })
        .sum();
    moment / reference
}

/// Solve one wheel's tire forces from the applied torques using a
/// quasi-static torque balance (real slip-ratio behavior without the stiff
/// wheel ODE):
///
/// - While the requested longitudinal force fits inside the grip circle the
///   tire is in its stable region: it transmits exactly the request, at the
///   slip ratio the magic formula prescribes for that force.
/// - Excess drive torque spins the wheel up (`WHEELSPIN_SLIP_RATIO`, force
///   drops into the falloff region).
/// - Excess brake torque locks the wheel (slip −1, sliding-friction force)
///   — unless ABS is enabled, which holds the wheel at peak slip.
///
/// The friction ellipse then couples longitudinal and lateral force so a
/// braking or spinning tire loses cornering force and vice versa.
#[allow(clippy::too_many_arguments)]
fn solve_wheel_forces(
    v_long: f32, // Body-frame longitudinal velocity (+ = forward)
    v_lat: f32,  // Body-frame lateral velocity (+ = left)
    yaw_rate: f32,
    steer_angle: f32,
    wheel_pos_x: f32, // Distance from CoG (+ = front)
    drive_torque: f32,
    brake_force: f32,
    wheel_radius: f32,
    wheel_load: f32,
    grip_coefficient: f32,
    tire_config: &TireConfig,
    abs_enabled: bool,
    traction_control: TractionControl,
) -> WheelForces {
    // Wheel contact-patch velocity in the body frame: the body's own
    // velocity plus the rotational contribution at this wheel's position.
    // The lateral term is what lets tires resist sideways sliding — without
    // it the car is on ice.
    //
    // The slip solution is worked in the direction of travel (`dir`), so a
    // car backing up has the same grip as one going forwards. At a
    // standstill the direction is the one the drive is pushing.
    let dir = if v_long < -BRAKE_DEADZONE_SPEED_MPS
        || (v_long <= BRAKE_DEADZONE_SPEED_MPS && drive_torque < 0.0)
    {
        -1.0
    } else {
        1.0
    };
    let wheel_vel_x = v_long.abs().max(MIN_SPEED_THRESHOLD);
    let wheel_vel_y = v_lat + yaw_rate * wheel_pos_x;
    let free_rolling_omega = dir * wheel_vel_x / wheel_radius;

    if wheel_load < 1.0 {
        return WheelForces {
            omega: free_rolling_omega,
            ..Default::default()
        };
    }

    // Peak available force (friction circle radius)
    let d = grip_coefficient * wheel_load;

    // Slip angle: angle between where the wheel points and where its
    // contact patch actually travels. Backing up, the steered wheel's
    // heading enters with the opposite sign.
    let slip_angle = if wheel_vel_x > MIN_SPEED_THRESHOLD {
        ((wheel_vel_y / wheel_vel_x).atan() - dir * steer_angle).clamp(-0.5, 0.5)
    } else {
        0.0
    };

    // Lateral force (Fy): RESTORING — a positive slip angle means the
    // contact patch travels left of where the wheel points, so the tire
    // pushes right (negative). With the sign the other way the car turns
    // against the steering input and spins.
    let fy = -pacejka(
        d,
        PACEJKA_C_LAT,
        slip_angle / tire_config.optimal_slip_angle_rad,
    );

    // Net longitudinal torque on the wheel, along the direction of travel
    // (positive drives the car the way it is going, negative slows it).
    // Brakes produce nothing in the deadzone around standstill.
    let braking = if v_long.abs() > BRAKE_DEADZONE_SPEED_MPS {
        brake_force
    } else {
        0.0
    };
    let requested_force = (drive_torque * dir - braking * wheel_radius) / wheel_radius;

    // High traction control leaves the tyre a lateral budget: drive is capped
    // at what the friction circle has left beside the cornering force, less a
    // margin, so the ellipse below never has to scale the lateral force down.
    let tc_limit = match traction_control {
        TractionControl::High => {
            ((d * d - fy * fy).max(0.0).sqrt() * TC_HIGH_LATERAL_MARGIN).min(d)
        }
        TractionControl::Low | TractionControl::Off => d,
    };
    let tc_enabled = traction_control != TractionControl::Off;

    let (fx, slip_ratio) = if tc_enabled && requested_force > tc_limit {
        // Traction control cuts drive torque to what the tyre can carry: at
        // Low the wheel is held at peak slip and delivers peak force, at High
        // it stays short of the peak with the lateral share kept.
        let normalized = pacejka_inverse(d, PACEJKA_C_LONG, tc_limit);
        (
            tc_limit,
            (normalized * tire_config.optimal_slip_ratio).clamp(-1.0, 1.0),
        )
    } else if requested_force.abs() <= d {
        // Stable region: the tire transmits exactly what is asked of it.
        let normalized = pacejka_inverse(d, PACEJKA_C_LONG, requested_force);
        (
            requested_force,
            (normalized * tire_config.optimal_slip_ratio).clamp(-1.0, 1.0),
        )
    } else if requested_force < 0.0 {
        // Brake torque exceeds traction.
        if abs_enabled {
            // ABS holds the wheel at peak slip → peak braking force.
            (-d, -tire_config.optimal_slip_ratio)
        } else {
            // Locked wheel: full negative slip, sliding-friction force from
            // the falloff region of the magic formula.
            let normalized = -1.0 / tire_config.optimal_slip_ratio;
            (pacejka(d, PACEJKA_C_LONG, normalized), -1.0)
        }
    } else {
        // Drive torque exceeds traction: wheelspin. The spin depth grades
        // with the torque oversupply — a slight excess hovers just past the
        // peak (little force lost), a large excess spins deep into the
        // falloff region.
        let overshoot = (requested_force / d).min(3.0);
        let slip = (tire_config.optimal_slip_ratio * (1.25 + (overshoot - 1.0) * 1.5))
            .min(WHEELSPIN_MAX_SLIP_RATIO);
        let normalized = slip / tire_config.optimal_slip_ratio;
        (pacejka(d, PACEJKA_C_LONG, normalized), slip)
    };

    // Combined slip: friction ellipse. Longitudinal and lateral demands
    // share one grip budget; scale both down proportionally when the
    // combined demand exceeds it.
    let usage = ((fx / d).powi(2) + (fy / d).powi(2)).sqrt();
    let (fx, fy) = if usage > 1.0 {
        (fx / usage, fy / usage)
    } else {
        (fx, fy)
    };
    // Back from the direction of travel to the body frame.
    let fx = fx * dir;

    // Wheel angular velocity consistent with the slip solution.
    let omega = free_rolling_omega * (1.0 + slip_ratio);

    WheelForces {
        fx,
        fy,
        slip_ratio,
        slip_angle,
        omega,
        abs_active: abs_enabled && requested_force < -d,
        tc_active: tc_enabled && requested_force > tc_limit,
    }
}

/// Automatic gearbox. Where it shifts comes from the engine's torque curve,
/// not a fixed fraction of the redline: a gear is worth being in while it
/// puts more torque on the road than its neighbour would at the same road
/// speed. A peaky engine therefore shifts at the limiter and a torquey one
/// well short of it, each at the point that accelerates the car hardest.
///
/// The limiter is still a hard backstop for an engine that pulls all the way
/// to it (a torque curve flat to the redline has no crossover at all).
const AUTO_UPSHIFT_CEILING_FRAC: f32 = 0.98;
/// On throttle, a lower gear must pull this much harder than the current one
/// before the box kicks down. The margin is the hysteresis: right after an
/// upshift the gear below pulls about as hard as the new one, and without it
/// the box would hunt between the two.
const AUTO_KICKDOWN_TORQUE_GAIN: f32 = 1.05;
/// Throttle at or above which the driver is asking to accelerate, so the box
/// picks gears for torque; below it the box only keeps the revs up.
const AUTO_KICKDOWN_THROTTLE: f32 = 0.5;
/// Off throttle, drop a gear once revs have fallen out of the power band,
/// so there is drive waiting when the throttle comes back.
const AUTO_DOWNSHIFT_FRAC: f32 = 0.60;
/// Never downshift into a gear that would put the engine above this.
const AUTO_DOWNSHIFT_CEILING_FRAC: f32 = 0.92;
/// Minimum time between automatic shifts: a sequential box under braking
/// steps down one gear at a time rather than dumping four at once.
const AUTO_SHIFT_HOLD_S: f32 = 0.3;

/// Torque at the wheels, bar the constant final drive and efficiency, in
/// gear ratio `ratio` with the engine at `rpm` and the throttle wide open.
fn wheel_torque_potential(config: &CarConfig, ratio: f32, rpm: f32) -> f32 {
    full_throttle_net_torque_nm(config, rpm) * ratio
}

/// Whether the next gear up would put more torque on the road than this one
/// does now, at the same road speed.
fn next_gear_pulls_harder(config: &CarConfig, ratio_now: f32, ratio_up: f32, rpm: f32) -> bool {
    let rpm_up = rpm * ratio_up / ratio_now;
    rpm_up >= config.idle_rpm
        && wheel_torque_potential(config, ratio_up, rpm_up)
            > wheel_torque_potential(config, ratio_now, rpm)
}

/// Engine rpm at which the box leaves `gear` for the next one up on full
/// throttle: the torque crossover, or the limiter backstop if the engine
/// pulls all the way there. `None` in top gear.
pub fn auto_upshift_rpm(config: &CarConfig, gear: i8) -> Option<f32> {
    let g = usize::try_from(gear).ok().filter(|g| *g >= 1)?;
    let ratio_now = config.gear_ratios.get(g)?.abs();
    let ratio_up = config.gear_ratios.get(g + 1)?.abs();
    let ceiling = config.redline_rpm * AUTO_UPSHIFT_CEILING_FRAC;
    // Walk up from idle in 10 rpm steps; the curve is piecewise linear, so
    // this finds the crossover to well inside a tick's worth of revs.
    let mut rpm = config.idle_rpm;
    while rpm < ceiling {
        if next_gear_pulls_harder(config, ratio_now, ratio_up, rpm) {
            return Some(rpm);
        }
        rpm += 10.0;
    }
    Some(ceiling)
}

/// Speed below which the car counts as standing still: the automatic box
/// may change direction, and a manual shift into or out of reverse engages.
pub const DIRECTION_CHANGE_MAX_SPEED_MPS: f32 = 1.0;
/// How long the brake must be held at a standstill before the automatic box
/// selects reverse, so stopping at the end of a braking zone (or on the grid)
/// does not start the car backing up.
pub const AUTO_REVERSE_HOLD_S: f32 = 0.4;
/// Pedal travel that counts as "held" for the automatic box's direction
/// changes.
const AUTO_DIRECTION_PEDAL: f32 = 0.3;

/// Whether a shift from `from` to `to` engages with the car rolling at
/// `v_long` (body frame, + forwards). Reverse only goes in once the car is
/// no longer rolling forwards, and a forward gear once it is no longer
/// rolling backwards; neutral always engages.
pub fn gear_engages(from: i8, to: i8, v_long: f32) -> bool {
    match (from.signum(), to.signum()) {
        (_, -1) if from >= 0 => v_long < DIRECTION_CHANGE_MAX_SPEED_MPS,
        (-1, 1) => v_long > -DIRECTION_CHANGE_MAX_SPEED_MPS,
        _ => true,
    }
}

/// Gear the automatic box wants this tick, or `None` to keep the current
/// one. Holding the brake at a standstill selects reverse; the throttle at a
/// standstill in reverse or neutral selects first. Runs on the server
/// because it needs the car's torque curve and gear ratios, which the client
/// never learns from the wire protocol.
///
/// `input` is the driver's pedals as sent, not the swapped pair the car
/// drives on in reverse.
pub fn auto_gear_selection(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) -> Option<i8> {
    // Direction changes come first and ignore the shift hold: the hold
    // paces a sequential box through its ratios, not a car at rest.
    let standstill = state.speed_mps < DIRECTION_CHANGE_MAX_SPEED_MPS;
    let braking_to_reverse = standstill
        && state.gear >= 0
        && input.brake >= AUTO_DIRECTION_PEDAL
        && input.throttle < AUTO_DIRECTION_PEDAL;
    if braking_to_reverse {
        state.auto_reverse_ticks = state.auto_reverse_ticks.saturating_add(1);
        if f32::from(state.auto_reverse_ticks) * dt >= AUTO_REVERSE_HOLD_S {
            state.auto_reverse_ticks = 0;
            state.auto_shift_hold_ticks = 0;
            return Some(-1);
        }
    } else {
        state.auto_reverse_ticks = 0;
    }
    if standstill && state.gear <= 0 && input.throttle >= AUTO_DIRECTION_PEDAL {
        state.auto_shift_hold_ticks = 0;
        return Some(1);
    }

    if state.auto_shift_hold_ticks > 0 {
        state.auto_shift_hold_ticks -= 1;
        return None;
    }
    let max_gear = (config.gear_ratios.len() as i8 - 1).max(1);
    let redline = config.redline_rpm.max(config.idle_rpm + 1.0);
    let rpm = state.engine_rpm;
    let ratio = |g: i8| {
        config
            .gear_ratios
            .get(g as usize)
            .map(|r| r.abs().max(1e-3))
    };
    let wanted = match state.gear {
        g if g < 0 => None,
        0 => (input.throttle > 0.05).then_some(1),
        g => {
            let upshift = match (ratio(g), ratio(g + 1)) {
                (Some(now), Some(up)) if g < max_gear => {
                    rpm >= redline * AUTO_UPSHIFT_CEILING_FRAC
                        || next_gear_pulls_harder(config, now, up, rpm)
                }
                _ => false,
            };
            let downshift = match (ratio(g), ratio(g - 1)) {
                (Some(now), Some(down)) if g > 1 => {
                    let rpm_down = rpm * down / now;
                    let fits = rpm_down <= redline * AUTO_DOWNSHIFT_CEILING_FRAC;
                    let wants = if input.throttle >= AUTO_KICKDOWN_THROTTLE {
                        wheel_torque_potential(config, down, rpm_down)
                            > wheel_torque_potential(config, now, rpm) * AUTO_KICKDOWN_TORQUE_GAIN
                    } else {
                        rpm < redline * AUTO_DOWNSHIFT_FRAC
                    };
                    fits && wants
                }
                _ => false,
            };
            if upshift {
                Some(g + 1)
            } else if downshift {
                Some(g - 1)
            } else {
                None
            }
        }
    };
    if wanted.is_some() {
        state.auto_shift_hold_ticks = (AUTO_SHIFT_HOLD_S / dt.max(1e-4)).round() as u16;
    }
    wanted
}

/// Half-width of the windowed nearest-point search around a previous tick's
/// index. At 240Hz a car at 100 m/s moves ~0.4m per tick, a small fraction
/// of the window at typical centerline point spacing.
const CENTERLINE_SEARCH_WINDOW: usize = 32;

/// The pose on the centerline at `station_m` from the line: (x, y, z, yaw),
/// interpolated between the two samples either side, the yaw along the
/// segment. A station past the end lands on the last point, one before the
/// start on the first. Used to put a car down at a chosen distance along
/// the lap (the hotlap run-up); at least two points are expected.
pub fn pose_at_station(centerline: &[TrackPoint], station_m: f32) -> (f32, f32, f32, f32) {
    let last = centerline.len() - 1;
    let idx = centerline
        .partition_point(|p| p.distance_from_start_m < station_m)
        .clamp(1, last);
    let (a, b) = (&centerline[idx - 1], &centerline[idx]);
    let span = (b.distance_from_start_m - a.distance_from_start_m).max(1e-3);
    let t = ((station_m - a.distance_from_start_m) / span).clamp(0.0, 1.0);
    let yaw = (b.y - a.y).atan2(b.x - a.x);
    (
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        yaw,
    )
}

/// Find the index of the centerline point nearest to (x, y).
///
/// With a `hint` (the previous tick's index) only a small window around it
/// is searched — O(1) instead of O(track points). If the best match sits at
/// the window edge (car teleported/reset, or moved further than the window),
/// the result is distrusted and a full scan runs instead.
pub fn find_nearest_centerline_idx(
    centerline: &[TrackPoint],
    x: f32,
    y: f32,
    hint: Option<usize>,
) -> Option<usize> {
    let n = centerline.len();
    if n == 0 {
        return None;
    }

    let dist_sq = |idx: usize| {
        let dx = x - centerline[idx].x;
        let dy = y - centerline[idx].y;
        dx * dx + dy * dy
    };

    if let Some(hint) = hint {
        if hint < n && n > 2 * CENTERLINE_SEARCH_WINDOW {
            let mut best_idx = hint;
            let mut best_d = dist_sq(hint);
            let mut best_off: usize = 0;
            for off in 1..=CENTERLINE_SEARCH_WINDOW {
                // Window with wraparound (tracks are typically closed loops)
                let fwd = (hint + off) % n;
                let back = (hint + n - off) % n;
                for idx in [fwd, back] {
                    let d = dist_sq(idx);
                    if d < best_d {
                        best_d = d;
                        best_idx = idx;
                        best_off = off;
                    }
                }
            }
            if best_off < CENTERLINE_SEARCH_WINDOW {
                return Some(best_idx);
            }
            // Best at window edge: hint unreliable, fall through to full scan
        }
    }

    let mut best_idx = 0;
    let mut best_d = f32::MAX;
    for idx in 0..n {
        let d = dist_sq(idx);
        if d < best_d {
            best_d = d;
            best_idx = idx;
        }
    }
    Some(best_idx)
}

/// Query ground/surface properties at a world-space position. `hint` is the
/// car's cached nearest-centerline index from the previous query, enabling a
/// windowed (near-constant-time) search.
fn query_track_surface(
    track: &TrackConfig,
    world_x: f32,
    world_y: f32,
    hint: Option<usize>,
) -> Option<SurfaceQuerySample> {
    match active_surface_query_backend() {
        SurfaceQueryBackend::Centerline => {
            query_track_surface_centerline(track, world_x, world_y, hint)
        }
        SurfaceQueryBackend::MeshHeightfield => {
            query_track_surface_mesh_heightfield_stub(track, world_x, world_y, hint)
        }
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
    hint: Option<usize>,
) -> Option<SurfaceQuerySample> {
    let centerline_sample = query_track_surface_centerline(track, world_x, world_y, hint)?;

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

/// Lateral distance past the road edge over which the surface blends from
/// the asphalt onto the baked ground, so the verge is a small ramp rather
/// than a step for the wheel that crosses the white line first.
const ROAD_EDGE_BLEND_M: f32 = 1.5;

/// Within this far past the road edge, baked ground more than
/// [`OTHER_LEVEL_M`] from the edge's height belongs to another level of the
/// circuit, so the road edge height is held instead. The exporter keeps the
/// verge within centimetres of the edge everywhere else, so only a grade
/// separation trips it: the lower road seen from a car on the bridge (the
/// deck reaches `DECK_OVERHANG_M`, 1.6 m, past the edge, the parapet a
/// little further), or the upper road seen from the lower one.
const DECK_REACH_M: f32 = 2.5;
/// See [`DECK_REACH_M`]; the exporter's `OVERHEAD_M`.
const OTHER_LEVEL_M: f32 = 3.0;

/// Height of the surface under (world_x, world_y) given the nearest
/// centerline point and the signed lateral offset (positive = right).
///
/// On the asphalt it is the centerline elevation sheared by the banking, the
/// formula the track editor bakes the road ribbon with. Off the asphalt it is
/// the baked ground heightfield when the track ships one (the grass the
/// client actually draws), blended in over [`ROAD_EDGE_BLEND_M`]. Without a
/// heightfield the road-edge height is held, as before.
fn surface_elevation(
    track: &TrackConfig,
    nearest: &TrackPoint,
    lateral_offset: f32,
    world_x: f32,
    world_y: f32,
) -> f32 {
    let (half_width, edge_lateral) = if lateral_offset >= 0.0 {
        (nearest.width_right_m, nearest.width_right_m)
    } else {
        (nearest.width_left_m, -nearest.width_left_m)
    };
    let shear = nearest.banking_rad.sin();
    let overhang = lateral_offset.abs() - half_width;
    if overhang <= 0.0 {
        return nearest.z - lateral_offset * shear;
    }
    let edge_z = nearest.z - edge_lateral * shear;
    match track.ground.as_ref() {
        Some(ground) => {
            let ground_z = ground.sample(world_x, world_y);
            if overhang <= DECK_REACH_M && (ground_z - edge_z).abs() > OTHER_LEVEL_M {
                return edge_z;
            }
            let t = (overhang / ROAD_EDGE_BLEND_M).clamp(0.0, 1.0);
            edge_z + (ground_z - edge_z) * t
        }
        None => edge_z,
    }
}

/// Longest gap between consecutive centerline points that still counts as a
/// segment of road. It separates a closed loop's last-to-first segment (a
/// normal point spacing) from an open track's two far-apart ends.
const MAX_CENTERLINE_SEGMENT_M: f32 = 50.0;

/// The centerline between the points either side of (x, y): the nearest
/// point on the polyline through `nearest` and its neighbours, as a
/// centerline point interpolated along that segment, plus the signed lateral
/// offset from it (positive = right).
///
/// Interpolating is what keeps the ground continuous under a wheel. Reading
/// the nearest point's height outright turns every slope into a staircase
/// (a centimetre or two per point at ~1 m spacing), and a wheel that drops
/// down one step a tick before its partner on the same axle reads it as a
/// 4 m/s suspension stroke: the damper puts the whole axle load on one wheel
/// and none on the other. At speed on Le Mans that happened several times a
/// second, with the unloaded rear wheel spinning up under drive, and the car
/// wobbled and stepped out on the straights.
fn centerline_at(centerline: &[TrackPoint], nearest: usize, x: f32, y: f32) -> (TrackPoint, f32) {
    let n = centerline.len();
    let p = &centerline[nearest];
    let neighbour = |idx: usize| {
        let q = &centerline[idx];
        let gap_sq = (q.x - p.x).powi(2) + (q.y - p.y).powi(2);
        (idx != nearest && gap_sq <= MAX_CENTERLINE_SEGMENT_M.powi(2)).then_some(idx)
    };
    let next = neighbour((nearest + 1) % n);
    let prev = neighbour((nearest + n - 1) % n);

    // Closest point on each segment touching `nearest`; the nearer one wins.
    let project = |a: usize, b: usize| {
        let (pa, pb) = (&centerline[a], &centerline[b]);
        let (sx, sy) = (pb.x - pa.x, pb.y - pa.y);
        let len_sq = (sx * sx + sy * sy).max(1e-9);
        let t = (((x - pa.x) * sx + (y - pa.y) * sy) / len_sq).clamp(0.0, 1.0);
        let dist_sq = (x - pa.x - sx * t).powi(2) + (y - pa.y - sy * t).powi(2);
        (a, b, t, dist_sq)
    };
    let segment = match (
        prev.map(|a| project(a, nearest)),
        next.map(|b| project(nearest, b)),
    ) {
        (Some(back), Some(fwd)) => Some(if back.3 < fwd.3 { back } else { fwd }),
        (back, fwd) => back.or(fwd),
    };
    let Some((a, b, t, _)) = segment else {
        // A single point: nothing to interpolate.
        let cross = (x - p.x) * p.heading_rad.sin() - (y - p.y) * p.heading_rad.cos();
        return (p.clone(), cross);
    };

    let (pa, pb) = (&centerline[a], &centerline[b]);
    let lerp = |from: f32, to: f32| from + (to - from) * t;
    let (sx, sy) = (pb.x - pa.x, pb.y - pa.y);
    let len = (sx * sx + sy * sy).sqrt().max(1e-6);
    let foot_x = lerp(pa.x, pb.x);
    let foot_y = lerp(pa.y, pb.y);
    // Positive = right of the direction of travel along the segment.
    let lateral_offset = ((x - foot_x) * sy - (y - foot_y) * sx) / len;
    let point = TrackPoint {
        x: foot_x,
        y: foot_y,
        z: lerp(pa.z, pb.z),
        distance_from_start_m: lerp(pa.distance_from_start_m, pb.distance_from_start_m),
        width_left_m: lerp(pa.width_left_m, pb.width_left_m),
        width_right_m: lerp(pa.width_right_m, pb.width_right_m),
        banking_rad: lerp(pa.banking_rad, pb.banking_rad),
        camber_rad: lerp(pa.camber_rad, pb.camber_rad),
        // A point's slope and heading describe the segment leaving it.
        slope_rad: pa.slope_rad,
        heading_rad: sy.atan2(sx),
        surface_type: p.surface_type,
        grip_modifier: p.grip_modifier,
    };
    (point, lateral_offset)
}

/// Centerline-backed surface query implementation.
fn query_track_surface_centerline(
    track: &TrackConfig,
    world_x: f32,
    world_y: f32,
    hint: Option<usize>,
) -> Option<SurfaceQuerySample> {
    let nearest_idx = find_nearest_centerline_idx(&track.centerline, world_x, world_y, hint)?;
    let (line, lateral_offset) = centerline_at(&track.centerline, nearest_idx, world_x, world_y);

    Some(SurfaceQuerySample {
        nearest_point: nearest_idx,
        elevation: surface_elevation(track, &line, lateral_offset, world_x, world_y),
        banking_rad: line.banking_rad,
        slope_rad: line.slope_rad,
        heading_rad: line.heading_rad,
        lateral_offset,
        width_left: line.width_left_m,
        width_right: line.width_right_m,
        surface_type: line.surface_type,
        grip_modifier: line.grip_modifier,
    })
}

/// Where a surface sample sits against the track limits: on the road, on the
/// curb band past its edge, or beyond both.
///
/// The curbs reach a little past the road edge: a driver who puts two wheels
/// on a curb is using the track, not cutting it, so the band counts as on it
/// (with the curb's own grip, and without the off-track speed penalty).
/// What is under a point of the car, as the sim distinguishes it: the
/// road, the curb band (still track), the prepared tarmac run-off past it
/// (off the track for the lap, but asphalt to drive on), or the grass.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RoadContact {
    Road,
    Curb,
    Runoff,
    Off,
}

impl RoadContact {
    /// Off the track for the lap: past the curbs.
    pub fn off_track(self) -> bool {
        matches!(self, RoadContact::Runoff | RoadContact::Off)
    }

    /// What the driver feels: the tarmac run-off is as smooth as the road.
    pub fn feedback(self) -> ContactSurface {
        match self {
            RoadContact::Road | RoadContact::Runoff => ContactSurface::Road,
            RoadContact::Curb => ContactSurface::Curb,
            RoadContact::Off => ContactSurface::Off,
        }
    }
}

/// Grip of the tarmac run-off relative to the road: swept less often, no
/// rubber laid on it.
pub const RUNOFF_GRIP_FACTOR: f32 = 0.95;

fn road_contact(track: &TrackConfig, surface: &SurfaceQuerySample) -> RoadContact {
    let half_width = if surface.lateral_offset >= 0.0 {
        surface.width_right
    } else {
        surface.width_left
    };
    let overhang = surface.lateral_offset.abs() - half_width;
    if overhang <= 0.0 {
        return RoadContact::Road;
    }
    let station_m = track
        .centerline
        .get(surface.nearest_point)
        .map_or(0.0, |p| p.distance_from_start_m);
    let (curb_width, runoff_reach) = track.curbs.as_ref().map_or((0.0, 0.0), |bands| {
        (
            bands.width_at(station_m, surface.lateral_offset),
            bands.runoff_at(station_m, surface.lateral_offset),
        )
    });
    if overhang <= curb_width {
        RoadContact::Curb
    } else if overhang <= runoff_reach {
        RoadContact::Runoff
    } else {
        RoadContact::Off
    }
}

/// Get track context at the car's current position
fn get_track_context(state: &CarState, track: &TrackConfig) -> TrackContext {
    let hint = state.nearest_centerline_idx.map(|i| i as usize);
    let Some(surface) = query_track_surface(track, state.pos_x, state.pos_y, hint) else {
        return TrackContext::default();
    };

    let contact = road_contact(track, &surface);
    // On the track as the physics means it — a surface with grip and no
    // grass drag. The tarmac run-off counts: it is asphalt. The lap's track
    // limits are judged per wheel (`WheelState::off_track`), where the
    // run-off is off.
    let is_on_track = contact != RoadContact::Off;

    // Determine surface type and grip
    let (surface_type, grip_modifier) = match contact {
        RoadContact::Curb => (SurfaceType::Curb, track.track_surface.curb_grip),
        RoadContact::Road => (surface.surface_type, surface.grip_modifier),
        RoadContact::Runoff => (
            SurfaceType::Asphalt,
            surface.grip_modifier * RUNOFF_GRIP_FACTOR,
        ),
        RoadContact::Off => (SurfaceType::Grass, track.track_surface.off_track_grip),
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
#[allow(clippy::too_many_arguments)]
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
    state.tires.front_left.temperature_c =
        calculate_tire_temp(fl_slip.0, fl_slip.1, state.weight_front_left_n);
    state.tires.front_left.pressure_kpa = 200.0 + state.tires.front_left.temperature_c * 0.5;
    state.tires.front_left.slip_ratio = fl_slip.0;
    state.tires.front_left.slip_angle_rad = fl_slip.1;
    state.tires.front_left.wear_percent = (state.tires.front_left.wear_percent
        + fl_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt)
        .min(100.0);

    // Front right tire
    state.tires.front_right.temperature_c =
        calculate_tire_temp(fr_slip.0, fr_slip.1, state.weight_front_right_n);
    state.tires.front_right.pressure_kpa = 200.0 + state.tires.front_right.temperature_c * 0.5;
    state.tires.front_right.slip_ratio = fr_slip.0;
    state.tires.front_right.slip_angle_rad = fr_slip.1;
    state.tires.front_right.wear_percent = (state.tires.front_right.wear_percent
        + fr_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt)
        .min(100.0);

    // Rear left tire
    state.tires.rear_left.temperature_c =
        calculate_tire_temp(rl_slip.0, rl_slip.1, state.weight_rear_left_n);
    state.tires.rear_left.pressure_kpa = 200.0 + state.tires.rear_left.temperature_c * 0.5;
    state.tires.rear_left.slip_ratio = rl_slip.0;
    state.tires.rear_left.slip_angle_rad = rl_slip.1;
    state.tires.rear_left.wear_percent = (state.tires.rear_left.wear_percent
        + rl_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt)
        .min(100.0);

    // Rear right tire
    state.tires.rear_right.temperature_c =
        calculate_tire_temp(rr_slip.0, rr_slip.1, state.weight_rear_right_n);
    state.tires.rear_right.pressure_kpa = 200.0 + state.tires.rear_right.temperature_c * 0.5;
    state.tires.rear_right.slip_ratio = rr_slip.0;
    state.tires.rear_right.slip_angle_rad = rr_slip.1;
    state.tires.rear_right.wear_percent = (state.tires.rear_right.wear_percent
        + rr_slip.0.abs() * 0.0001 * config.tire_config.wear_rate * dt)
        .min(100.0);

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
fn update_fuel_consumption(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    dt: f32,
) {
    // Base consumption + load-based consumption
    let rpm_factor = state.engine_rpm / config.max_engine_rpm;
    let throttle_factor = input.throttle;

    state.fuel_consumption_lps = config.fuel.idle_consumption_lps
        + (throttle_factor * rpm_factor * config.fuel.load_consumption_scale);
    state.fuel_liters = (state.fuel_liters - state.fuel_consumption_lps * dt).max(0.0);
}

/// Check and resolve 3D OBB collisions between cars (owned-slice wrapper).
pub fn check_obb_collisions_3d(states: &mut [CarState], configs: &HashMap<CarConfigId, CarConfig>) {
    let mut refs: Vec<&mut CarState> = states.iter_mut().collect();
    check_collisions_refs(&mut refs, configs);
}

/// Check and resolve collisions on a slice of mutable references — lets the
/// caller pass `participants.values_mut().collect()` directly, avoiding a
/// full clone + map rebuild of every CarState per tick.
pub fn check_collisions_refs(
    states: &mut [&mut CarState],
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
                if let Some(overlap) = check_obb_overlap(states[i], cfg_i, states[j], cfg_j) {
                    // Mark as colliding
                    states[i].is_colliding = true;
                    states[j].is_colliding = true;

                    // SAT gives the minimum-translation normal (a → b)
                    let nx = overlap.normal_x;
                    let ny = overlap.normal_y;
                    let nz = 0.0;

                    states[i].collision_normal_x = -nx;
                    states[i].collision_normal_y = -ny;
                    states[i].collision_normal_z = -nz;
                    states[j].collision_normal_x = nx;
                    states[j].collision_normal_y = ny;
                    states[j].collision_normal_z = nz;

                    // Separate exactly by penetration depth, split inversely
                    // proportional to mass (light car moves further).
                    let inv_mass_i = 1.0 / cfg_i.mass_kg.max(1.0);
                    let inv_mass_j = 1.0 / cfg_j.mass_kg.max(1.0);
                    let total_inv_mass = inv_mass_i + inv_mass_j;
                    let sep_i = overlap.penetration * inv_mass_i / total_inv_mass;
                    let sep_j = overlap.penetration * inv_mass_j / total_inv_mass;
                    states[i].pos_x -= nx * sep_i;
                    states[i].pos_y -= ny * sep_i;
                    states[j].pos_x += nx * sep_j;
                    states[j].pos_y += ny * sep_j;

                    // Calculate impact velocity and apply impulse
                    let rel_vel_x = states[j].vel_x - states[i].vel_x;
                    let rel_vel_y = states[j].vel_y - states[i].vel_y;
                    let rel_vel_normal = rel_vel_x * nx + rel_vel_y * ny;

                    // Both drivers feel the hit by how fast they were closing.
                    let closing_mps = (-rel_vel_normal).max(0.0);
                    states[i].feedback.record_impact(closing_mps);
                    states[j].feedback.record_impact(closing_mps);

                    if rel_vel_normal < 0.0 {
                        // Collision impulse (elastic coefficient)
                        let restitution = 0.3;
                        let impulse = -(1.0 + restitution) * rel_vel_normal;
                        let impulse = impulse / (1.0 / cfg_i.mass_kg + 1.0 / cfg_j.mass_kg);

                        let dv_i = (-impulse * nx / cfg_i.mass_kg, -impulse * ny / cfg_i.mass_kg);
                        let dv_j = (impulse * nx / cfg_j.mass_kg, impulse * ny / cfg_j.mass_kg);
                        states[i].vel_x += dv_i.0;
                        states[i].vel_y += dv_i.1;
                        states[j].vel_x += dv_j.0;
                        states[j].vel_y += dv_j.1;
                        let kick_i = impact_steer_kick(states[i], cfg_i, dv_i, 0.0, None);
                        let kick_j = impact_steer_kick(states[j], cfg_j, dv_j, 0.0, None);
                        states[i].feedback.record_steer_kick(kick_i);
                        states[j].feedback.record_steer_kick(kick_j);
                    }

                    // Recalculate speeds
                    states[i].speed_mps = (states[i].vel_x.powi(2)
                        + states[i].vel_y.powi(2)
                        + states[i].vel_z.powi(2))
                    .sqrt();
                    states[j].speed_mps = (states[j].vel_x.powi(2)
                        + states[j].vel_y.powi(2)
                        + states[j].vel_z.powi(2))
                    .sqrt();

                    // Apply damage from the CLOSING speed along the contact
                    // normal — not the cars' travel speed. Two cars racing
                    // side by side at 250 km/h that touch lightly have a
                    // closing speed near zero; using travel speed bricked
                    // whole packs from incidental contact.
                    let impact_speed = rel_vel_normal.abs().min(50.0);
                    if impact_speed > 1.0 {
                        let damage_amount = (impact_speed / 50.0) * 5.0;

                        let angle_i = (ny.atan2(nx) - states[i].yaw_rad).rem_euclid(2.0 * PI);
                        let angle_j = (ny.atan2(nx) - states[j].yaw_rad + PI).rem_euclid(2.0 * PI);

                        apply_damage_to_car(states[i], angle_i, damage_amount);
                        apply_damage_to_car(states[j], angle_j, damage_amount);
                    }
                }
            }
        }
    }
}

/// Result of an OBB separating-axis test: the minimum-translation vector.
struct ObbOverlap {
    /// Unit normal pointing from car A toward car B
    normal_x: f32,
    normal_y: f32,
    /// Overlap depth along the normal (meters)
    penetration: f32,
}

/// Yaw-aware oriented-bounding-box overlap test (SAT on the two cars'
/// rectangles in the ground plane, plus a height gate). Returns the
/// minimum-translation vector when the boxes overlap.
fn check_obb_overlap(
    state_a: &CarState,
    config_a: &CarConfig,
    state_b: &CarState,
    config_b: &CarConfig,
) -> Option<ObbOverlap> {
    // Height gate first (cars at different elevations don't collide)
    let dz = (state_a.pos_z - state_b.pos_z).abs();
    if dz >= (config_a.height_m + config_b.height_m) / 2.0 {
        return None;
    }
    obb_overlap(
        (state_a.pos_x, state_a.pos_y),
        (state_a.yaw_rad.cos(), state_a.yaw_rad.sin()),
        config_a.length_m / 2.0,
        config_a.width_m / 2.0,
        (state_b.pos_x, state_b.pos_y),
        (state_b.yaw_rad.cos(), state_b.yaw_rad.sin()),
        config_b.length_m / 2.0,
        config_b.width_m / 2.0,
    )
}

/// Separating-axis test between two rectangles in the ground plane, each
/// given by its centre, unit forward axis and half extents along and
/// across it. The normal points from A toward B.
#[allow(clippy::too_many_arguments)]
fn obb_overlap(
    center_a: (f32, f32),
    fwd_a: (f32, f32),
    half_l_a: f32,
    half_w_a: f32,
    center_b: (f32, f32),
    fwd_b: (f32, f32),
    half_l_b: f32,
    half_w_b: f32,
) -> Option<ObbOverlap> {
    // Local axes of each box (forward, left)
    let ax_a = fwd_a;
    let ay_a = (-fwd_a.1, fwd_a.0);
    let ax_b = fwd_b;
    let ay_b = (-fwd_b.1, fwd_b.0);

    let dx = center_b.0 - center_a.0;
    let dy = center_b.1 - center_a.1;

    // Projected extent of a box onto a unit axis
    let extent = |axis: (f32, f32), fwd: (f32, f32), left: (f32, f32), hl: f32, hw: f32| {
        hl * (axis.0 * fwd.0 + axis.1 * fwd.1).abs()
            + hw * (axis.0 * left.0 + axis.1 * left.1).abs()
    };

    let mut min_penetration = f32::INFINITY;
    let mut best_axis = (0.0f32, 0.0f32);

    for axis in [ax_a, ay_a, ax_b, ay_b] {
        let center_dist = dx * axis.0 + dy * axis.1;
        let overlap = extent(axis, ax_a, ay_a, half_l_a, half_w_a)
            + extent(axis, ax_b, ay_b, half_l_b, half_w_b)
            - center_dist.abs();
        if overlap <= 0.0 {
            return None; // Separating axis found
        }
        if overlap < min_penetration {
            min_penetration = overlap;
            // Orient the axis from A toward B
            best_axis = if center_dist >= 0.0 {
                axis
            } else {
                (-axis.0, -axis.1)
            };
        }
    }

    Some(ObbOverlap {
        normal_x: best_axis.0,
        normal_y: best_axis.1,
        penetration: min_penetration,
    })
}

/// Apply damage to a car based on collision angle
fn apply_damage_to_car(car: &mut CarState, angle: f32, damage_amount: f32) {
    if !(PI / 4.0..=7.0 * PI / 4.0).contains(&angle) {
        car.damage.front_damage_percent =
            (car.damage.front_damage_percent + damage_amount).min(100.0);
        car.damage.engine_damage_percent =
            (car.damage.engine_damage_percent + damage_amount * 0.5).min(100.0);
    } else if (PI / 4.0..3.0 * PI / 4.0).contains(&angle) {
        car.damage.left_damage_percent =
            (car.damage.left_damage_percent + damage_amount).min(100.0);
    } else if (3.0 * PI / 4.0..5.0 * PI / 4.0).contains(&angle) {
        car.damage.rear_damage_percent =
            (car.damage.rear_damage_percent + damage_amount).min(100.0);
    } else {
        car.damage.right_damage_percent =
            (car.damage.right_damage_percent + damage_amount).min(100.0);
    }

    car.damage.is_drivable =
        car.damage.front_damage_percent < 80.0 && car.damage.engine_damage_percent < 80.0;
}

// ---------------------------------------------------------------------------
// Walls
// ---------------------------------------------------------------------------

/// A wall is solid this far below its footing too, so a car on a verge a
/// little lower than the barrier's base still meets it.
const WALL_BELOW_M: f32 = 0.3;
/// Half-thickness a wall face is given for the overlap test.
const WALL_HALF_THICKNESS_M: f32 = 0.05;
/// Fraction of the tangential speed lost per second while the flank grinds
/// along a wall.
const WALL_SCRAPE_RATE_PER_SEC: f32 = 0.4;
/// Fraction of speed a car loses per second while riding a sausage kerb.
const KERB_SCRUB_RATE_PER_SEC: f32 = 0.6;
/// Closing speed reported to the feedback while on a kerb, so it rumbles.
const KERB_JOLT_MPS: f32 = 1.5;
/// Cap on the yaw-rate change one wall contact may apply.
const WALL_MAX_YAW_KICK_RAD_S: f32 = 4.0;
/// Steering-column kick, in reference torques, per m/s the front axle is
/// shoved sideways (or a front corner stopped) by a hit. A 5 m/s side swipe
/// is well past anything the tyres can put into the rim.
const STEER_KICK_PER_MPS: f32 = 0.35;
/// A front corner stopped short kicks this much per m/s against a sideways
/// shove: the scrub radius is a shorter lever than the trail.
const STEER_KICK_CORNER_SHARE: f32 = 0.5;
/// Largest kick one hit reports; the client soft-limits it anyway.
const MAX_STEER_KICK: f32 = 3.0;

/// The jolt a hit puts through the steering column, from the velocity change
/// it gave the car (`dv`, world frame, m/s), the yaw-rate change and, when
/// known, where it landed (`contact`, world offset from the car's centre).
/// Positive turns the wheel left, as the steering torque does.
///
/// Either way the rim is yanked toward the side that was hit. A front tyre
/// shoved sideways at its contact patch, which trails the steering axis,
/// turns the wheel toward the push's source, as the aligning torque would;
/// and a front corner stopped short turns it toward that corner, as a tyre
/// braking alone does through the scrub radius.
fn impact_steer_kick(
    state: &CarState,
    config: &CarConfig,
    dv: (f32, f32),
    dyaw: f32,
    contact: Option<(f32, f32)>,
) -> f32 {
    let (sin_yaw, cos_yaw) = state.yaw_rad.sin_cos();
    let (front_axle_x, _) = axle_positions(config);
    let dv_long = dv.0 * cos_yaw + dv.1 * sin_yaw;
    let dv_lat = -dv.0 * sin_yaw + dv.1 * cos_yaw + dyaw * front_axle_x;
    let mut kick = -dv_lat;
    if let Some((cx, cy)) = contact {
        let ahead = cx * cos_yaw + cy * sin_yaw;
        let left = -cx * sin_yaw + cy * cos_yaw;
        if ahead > 0.0 {
            // Only a rearward jolt stops a corner; a push from behind is
            // taken by the body, not the wheels.
            kick += left.signum() * (-dv_long).max(0.0) * STEER_KICK_CORNER_SHARE;
        }
    }
    (kick * STEER_KICK_PER_MPS).clamp(-MAX_STEER_KICK, MAX_STEER_KICK)
}

/// Stop every car at the track's walls (`TrackConfig::walls`, the baked
/// barriers). Runs after the car-car pass, so the contact flags it sets
/// survive the tick; a track without a walls sidecar has nothing to hit.
pub fn check_wall_collisions(
    states: &mut [&mut CarState],
    configs: &HashMap<CarConfigId, CarConfig>,
    track: &TrackConfig,
    dt: f32,
) {
    let Some(walls) = track.walls.as_ref() else {
        return;
    };
    let mut nearby = Vec::new();
    for state in states.iter_mut() {
        if let Some(config) = configs.get(&state.car_config_id) {
            resolve_wall_contacts(state, config, walls, dt, &mut nearby);
        }
    }
}

/// Push one car out of every wall it overlaps and take the hit out of its
/// velocity. `nearby` is scratch for the wall query, kept by the caller so
/// the tick does not allocate per car.
///
/// Each wall is a thin rectangle; the same separating-axis test as the
/// car-car pass gives the push. A closing hit bounces by the wall's
/// restitution, loses tangential speed to its friction, and spins the car
/// by the moment of that impulse about the deepest corner, so a nose-in
/// hit swings the car round rather than stopping it dead. A car already
/// resting against the wall just scrapes along it.
pub fn resolve_wall_contacts(
    state: &mut CarState,
    config: &CarConfig,
    walls: &Walls,
    dt: f32,
    nearby: &mut Vec<u32>,
) {
    let half_l = config.length_m / 2.0;
    let half_w = config.width_m / 2.0;
    walls.candidates(state.pos_x, state.pos_y, half_l.hypot(half_w) + 0.5, nearby);
    for &idx in nearby.iter() {
        let wall = walls.segments()[idx as usize];

        // Height gate: a car on a bridge deck is above the abutment walls
        // of the road beneath, and a car below is under the parapets.
        let car_low = state.pos_z - WALL_BELOW_M;
        let car_high = state.pos_z + config.height_m;
        if car_high <= wall.z || car_low >= wall.z + wall.height_m {
            continue;
        }

        let len = wall.length();
        let dir = ((wall.x1 - wall.x0) / len, (wall.y1 - wall.y0) / len);
        let car_fwd = (state.yaw_rad.cos(), state.yaw_rad.sin());
        let Some(overlap) = obb_overlap(
            (state.pos_x, state.pos_y),
            car_fwd,
            half_l,
            half_w,
            ((wall.x0 + wall.x1) / 2.0, (wall.y0 + wall.y1) / 2.0),
            dir,
            len / 2.0,
            WALL_HALF_THICKNESS_M,
        ) else {
            continue;
        };

        let kind = wall.kind();
        if kind == WallKind::Kerb {
            // A sausage kerb is driven over, not into: no push-out, no
            // bounce, just a scrub of speed and a rumble for the wheel.
            let keep = (-KERB_SCRUB_RATE_PER_SEC * dt).exp();
            state.vel_x *= keep;
            state.vel_y *= keep;
            state.feedback.record_impact(KERB_JOLT_MPS);
            state.speed_mps =
                (state.vel_x.powi(2) + state.vel_y.powi(2) + state.vel_z.powi(2)).sqrt();
            continue;
        }

        // Normal from the wall into the car.
        let (nx, ny) = (-overlap.normal_x, -overlap.normal_y);
        state.pos_x += nx * overlap.penetration;
        state.pos_y += ny * overlap.penetration;
        state.is_colliding = true;
        state.collision_normal_x = nx;
        state.collision_normal_y = ny;
        state.collision_normal_z = 0.0;

        let vn = state.vel_x * nx + state.vel_y * ny;
        let (tx, ty) = (-ny, nx);
        let vt = state.vel_x * tx + state.vel_y * ty;
        if vn < 0.0 {
            let closing = -vn;
            let dvn = (1.0 + kind.restitution()) * closing;
            let dvt = -vt.signum() * (kind.friction() * dvn).min(vt.abs());
            let (jx, jy) = (dvn * nx + dvt * tx, dvn * ny + dvt * ty);
            state.vel_x += jx;
            state.vel_y += jy;

            // The impulse lands on the corner deepest in the wall; its
            // moment about the centre is the yaw kick (impulse and inertia
            // both per unit mass).
            let (rx, ry) = deepest_corner(car_fwd, half_l, half_w, (nx, ny));
            let inertia = (config.length_m.powi(2) + config.width_m.powi(2)) / 12.0;
            let kick = ((rx * jy - ry * jx) / inertia.max(0.1))
                .clamp(-WALL_MAX_YAW_KICK_RAD_S, WALL_MAX_YAW_KICK_RAD_S);
            state.angular_vel_yaw += kick;

            state.feedback.record_impact(closing);
            let steer_kick = impact_steer_kick(state, config, (jx, jy), kick, Some((rx, ry)));
            state.feedback.record_steer_kick(steer_kick);
            let impact_speed = closing.min(50.0);
            if impact_speed > 1.0 {
                let damage_amount = (impact_speed / 50.0) * 5.0;
                // Where the wall is, seen from the car (0 = dead ahead),
                // as the car-car pass measures it.
                let angle = ((-ny).atan2(-nx) - state.yaw_rad).rem_euclid(2.0 * PI);
                apply_damage_to_car(state, angle, damage_amount);
            }
        } else {
            // Resting against the wall: the flank grinds along it.
            let scraped = vt * (-WALL_SCRAPE_RATE_PER_SEC * dt).exp();
            state.vel_x += (scraped - vt) * tx;
            state.vel_y += (scraped - vt) * ty;
        }
        state.speed_mps = (state.vel_x.powi(2) + state.vel_y.powi(2) + state.vel_z.powi(2)).sqrt();
    }
}

/// The car corner furthest along `-normal`, relative to the car's centre.
fn deepest_corner(fwd: (f32, f32), half_l: f32, half_w: f32, normal: (f32, f32)) -> (f32, f32) {
    let left = (-fwd.1, fwd.0);
    let mut best = (0.0, 0.0);
    let mut best_depth = f32::INFINITY;
    for (sl, sw) in [(1.0, 1.0), (1.0, -1.0), (-1.0, 1.0), (-1.0, -1.0)] {
        let corner = (
            fwd.0 * half_l * sl + left.0 * half_w * sw,
            fwd.1 * half_l * sl + left.1 * half_w * sw,
        );
        let depth = corner.0 * normal.0 + corner.1 * normal.1;
        if depth < best_depth {
            best_depth = depth;
            best = corner;
        }
    }
    best
}

/// Number of virtual checkpoints synthesized (at 25%/50%/75% of track
/// length) when a track defines no explicit checkpoints, so anti-shortcut
/// lap validation always applies.
pub const VIRTUAL_CHECKPOINT_COUNT: usize = 3;

/// Maximum forward track-progress delta (m) in a single tick that still
/// advances checkpoints. Larger jumps are treated as teleports/nearest-point
/// discontinuities (e.g. corner cutting across a hairpin) and do not credit
/// checkpoints. Physically a car moves <0.5 m/tick at 240Hz; the windowed
/// nearest-point search moves at most ~32 m/tick.
const MAX_CHECKPOINT_ADVANCE_PER_TICK_M: f32 = 50.0;

/// Distance from start (m) of checkpoint `idx` for this track. Uses the
/// track's explicit checkpoints when defined, otherwise virtual checkpoints
/// at even fractions of the lap.
fn checkpoint_distance_m(track: &TrackConfig, track_length: f32, idx: usize) -> f32 {
    if track.checkpoints.is_empty() {
        track_length * (idx as f32 + 1.0) / (VIRTUAL_CHECKPOINT_COUNT as f32 + 1.0)
    } else {
        track.checkpoints[idx]
    }
}

/// Place a car on the centerline without counting anything: the station and
/// the search hint are set from where the car stands. Called when a car is
/// put on the grid, so the first racing tick sees no movement. Otherwise the
/// jump from the default station 0 to a grid slot just short of the line
/// reads as crossing the 10% mark and starts lap 1 almost a lap early.
pub fn seed_track_progress(state: &mut CarState, track: &TrackConfig) {
    let Some(idx) = find_nearest_centerline_idx(&track.centerline, state.pos_x, state.pos_y, None)
    else {
        return;
    };
    state.nearest_centerline_idx = Some(idx as u32);
    state.track_progress = track.centerline[idx].distance_from_start_m;
    // The stopwatch starts again with the car: splits and any strike against
    // the lap in progress belong to the race that just ended. Session bests
    // are kept, like `best_lap_time_ms`.
    state.laps.start_lap(0);
    state.wheels_off_track = false;
}

/// Update track progress and detect lap completion.
///
/// Laps are validated with checkpoints: a lap only counts when every
/// checkpoint was passed in order (driving forward) before the start/finish
/// wrap. This rejects driving backwards across the line and shortcut/teleport
/// exploits. Tracks without explicit checkpoints get virtual ones at
/// 25/50/75% of the lap.
///
/// Returns the timing line the car crossed on this tick, if any: a sector
/// boundary or the lap itself (`crate::laps`). At most one per tick — the
/// boundaries are hundreds of metres apart and a tick is a few centimetres.
pub fn update_track_progress_3d(
    state: &mut CarState,
    track: &TrackConfig,
    current_tick: u32,
    tick_rate_hz: u16,
) -> Option<crate::laps::LapEvent> {
    if track.centerline.is_empty() {
        return None;
    }

    let track_length = track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(1000.0);

    // Find nearest centerline point (windowed, seeded by the cached index)
    let hint = state.nearest_centerline_idx.map(|i| i as usize);
    let nearest_idx =
        find_nearest_centerline_idx(&track.centerline, state.pos_x, state.pos_y, hint)?;
    state.nearest_centerline_idx = Some(nearest_idx as u32);

    let old_progress = state.track_progress;
    state.track_progress = track.centerline[nearest_idx].distance_from_start_m;

    // Debug: Log track progress once per second
    if current_tick.is_multiple_of(tick_rate_hz as u32) {
        debug!(
            "[Lap Debug] Tick {}: current_lap={}, track_progress={:.1}m/{:.1}m, lap_time={}ms",
            current_tick,
            state.current_lap,
            state.track_progress,
            track_length,
            state.current_lap_time_ms
        );
    }

    // Update current lap time
    if state.current_lap > 0 {
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        state.current_lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / tick_rate_hz as f32) as u32;
        crate::laps::note_track_limits(state, tick_rate_hz);
    }

    let mut lap_event = None;

    // Sector boundaries. Only a step to the next sector counts: the wrap back
    // to sector 1 over the line is the lap's business, below, and a car
    // rejoining several sectors along never gets the splits it did not drive.
    if state.current_lap > 0 && track_length > 0.0 {
        let forward = (state.track_progress - old_progress).rem_euclid(track_length);
        let new_sector = crate::laps::sector_of(track, state.track_progress);
        if forward > 0.0
            && forward <= MAX_CHECKPOINT_ADVANCE_PER_TICK_M
            && new_sector == state.laps.sector + 1
            && (new_sector as usize) < crate::laps::SECTOR_COUNT
        {
            lap_event = Some(crate::laps::close_sector(
                state,
                new_sector,
                current_tick,
                tick_rate_hz,
            ));
        }
    }

    // Advance checkpoints passed by this tick's forward movement.
    // `forward` is the wrap-aware forward delta; backwards movement wraps to
    // a large positive value and teleports exceed the per-tick cap, so
    // neither credits checkpoints.
    let num_checkpoints = if track.checkpoints.is_empty() {
        VIRTUAL_CHECKPOINT_COUNT
    } else {
        track.checkpoints.len()
    };
    if track_length > 0.0 {
        let forward = (state.track_progress - old_progress).rem_euclid(track_length);
        if forward > 0.0 && forward <= MAX_CHECKPOINT_ADVANCE_PER_TICK_M {
            while (state.next_checkpoint as usize) < num_checkpoints {
                let cp = checkpoint_distance_m(track, track_length, state.next_checkpoint as usize);
                let to_cp = (cp - old_progress).rem_euclid(track_length);
                if to_cp > 0.0 && to_cp <= forward {
                    state.next_checkpoint += 1;
                } else {
                    break;
                }
            }
        }
    }

    // Detect lap completion (start/finish wrap). The lap only counts when
    // every checkpoint was hit in order.
    if state.current_lap > 0
        && old_progress > track_length * 0.8
        && state.track_progress < track_length * 0.2
    {
        if (state.next_checkpoint as usize) >= num_checkpoints {
            // Calculate lap time (time since lap started)
            let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
            let lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / tick_rate_hz as f32) as u32;
            state.last_lap_time_ms = Some(lap_time_ms);

            // The lap closes the final sector. The time is always kept, but
            // only a lap inside track limits can become a best.
            let completed = state.current_lap;
            let event =
                crate::laps::close_lap(state, completed, lap_time_ms, current_tick, tick_rate_hz);
            if event.valid
                && (state.best_lap_time_ms.is_none()
                    || lap_time_ms < state.best_lap_time_ms.unwrap())
            {
                state.best_lap_time_ms = Some(lap_time_ms);
            }
            lap_event = Some(event);

            state.current_lap += 1;
            state.lap_start_tick = current_tick; // Reset lap timer
            state.current_lap_time_ms = 0;
        } else {
            // The car reached the line without passing every checkpoint: it
            // cut the course, so the lap it is on is struck.
            state.laps.invalid = true;
        }
        // Counted or not, the car is back at the start of a lap: it must hit
        // every checkpoint again before the next wrap can count.
        state.next_checkpoint = 0;
    }

    // Start lap 1. A car waiting behind the line (the grid) starts it as it
    // crosses the line, so the first lap is timed from the line like every
    // other. Checkpoints are credited from the line on: any behind the car's
    // new station were passed on this lap.
    if state.current_lap == 0
        && old_progress > track_length * 0.8
        && state.track_progress < track_length * 0.2
    {
        state.current_lap = 1;
        state.lap_start_tick = current_tick;
        state.current_lap_time_ms = 0;
        state.next_checkpoint = checkpoints_before(track, track_length, state.track_progress);
        state
            .laps
            .start_lap_at(current_tick, track, state.track_progress);
    }

    // A car that starts past the line (placed there, not on a grid behind
    // it) starts lap 1 on clearing 10% of the track, so spawning near the
    // finish cannot trigger it.
    // (next_checkpoint is NOT reset here: checkpoint tracking began when the
    // car crossed the start line, so any checkpoint inside the first 10% of
    // the track has already been credited for this lap.)
    if state.current_lap == 0
        && old_progress < track_length * 0.1
        && state.track_progress > track_length * 0.1
    {
        state.current_lap = 1;
        state.lap_start_tick = current_tick; // Start timing first lap
        state.current_lap_time_ms = 0;
        state
            .laps
            .start_lap_at(current_tick, track, state.track_progress);
    }

    lap_event
}

/// How many checkpoints lie at or before `station_m` on the lap: the value
/// of `next_checkpoint` for a car that starts a lap standing there.
fn checkpoints_before(track: &TrackConfig, track_length: f32, station_m: f32) -> u8 {
    let count = if track.checkpoints.is_empty() {
        VIRTUAL_CHECKPOINT_COUNT
    } else {
        track.checkpoints.len()
    };
    (0..count)
        .take_while(|&idx| checkpoint_distance_m(track, track_length, idx) <= station_m)
        .count() as u8
}

/// Start lap 1 for a car standing at or past the start line when the race
/// goes green (pole sits on the line). Cars behind the line are left on lap 0
/// and start it as they cross. Returns whether the lap was started.
pub fn start_lap_on_green(state: &mut CarState, track: &TrackConfig, current_tick: u32) -> bool {
    let track_length = track
        .centerline
        .last()
        .map(|p| p.distance_from_start_m)
        .unwrap_or(0.0);
    if state.current_lap != 0 || track_length <= 0.0 || state.track_progress >= track_length * 0.5 {
        return false;
    }
    state.current_lap = 1;
    state.lap_start_tick = current_tick;
    state.current_lap_time_ms = 0;
    state.next_checkpoint = checkpoints_before(track, track_length, state.track_progress);
    state
        .laps
        .start_lap_at(current_tick, track, state.track_progress);
    true
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
pub fn update_car_2d(state: &mut CarState, config: &CarConfig, input: &PlayerInputData, dt: f32) {
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

    if state.current_lap > 0
        && old_progress > track_length * 0.8
        && state.track_progress < track_length * 0.2
    {
        // Calculate lap time (time since lap started)
        let ticks_elapsed = current_tick.saturating_sub(state.lap_start_tick);
        let lap_time_ms = ((ticks_elapsed as f32 * 1000.0) / 240.0) as u32;
        state.last_lap_time_ms = Some(lap_time_ms);

        if state.best_lap_time_ms.is_none() || lap_time_ms < state.best_lap_time_ms.unwrap() {
            state.best_lap_time_ms = Some(lap_time_ms);
        }

        state.current_lap += 1;
        state.lap_start_tick = current_tick; // Reset lap timer
        state.current_lap_time_ms = 0;
    }

    // Start lap 1 - only when crossing the start line going forward
    // This prevents false start detection when spawning near the finish line
    if state.current_lap == 0
        && old_progress < track_length * 0.1
        && state.track_progress > track_length * 0.1
    {
        state.current_lap = 1;
        state.lap_start_tick = current_tick; // Start timing first lap
        state.current_lap_time_ms = 0;
    }
}

/// Legacy collision check - wraps 3D version
pub fn check_obb_collisions(states: &mut [CarState], configs: &HashMap<CarConfigId, CarConfig>) {
    check_obb_collisions_3d(states, configs);
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::feedback::DriverFeedback;
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

    /// A 2km straight along +x through the origin, so a car spawned at
    /// (0,0) is ON the track (the default oval's centerline is 100m away,
    /// which silently puts dynamics tests on 0.4-grip grass).
    fn create_straight_test_track() -> TrackConfig {
        TrackConfig {
            centerline: (0..500)
                .map(|i| TrackPoint {
                    x: i as f32 * 4.0,
                    distance_from_start_m: i as f32 * 4.0,
                    width_left_m: 10.0,
                    width_right_m: 10.0,
                    ..Default::default()
                })
                .collect(),
            ..TrackConfig::default()
        }
    }

    /// The straight above, with a curb of `curb_m` along its right edge for
    /// the whole length.
    fn straight_track_with_right_curb(curb_m: f32) -> TrackConfig {
        let mut track = create_straight_test_track();
        let stations = (track.centerline.len() as f32 * 4.0) as usize;
        track.curbs = Some(crate::curbs::CurbBands {
            version: 1,
            step_m: 1.0,
            left_cm: vec![0; stations],
            right_cm: vec![(curb_m * 100.0) as u16; stations],
            runoff_left_cm: Vec::new(),
            runoff_right_cm: Vec::new(),
        });
        track
    }

    /// The straight with a curb and, past it, `runoff_m` of tarmac along
    /// its right edge.
    fn straight_track_with_right_runoff(curb_m: f32, runoff_m: f32) -> TrackConfig {
        let mut track = straight_track_with_right_curb(curb_m);
        let stations = track.centerline.len() * 4;
        if let Some(bands) = track.curbs.as_mut() {
            bands.version = 2;
            bands.runoff_left_cm = vec![0; stations];
            bands.runoff_right_cm = vec![(runoff_m * 100.0) as u16; stations];
        }
        track
    }

    fn context_at(track: &TrackConfig, y: f32) -> TrackContext {
        let mut state = create_test_car_state();
        state.pos_x = 100.0;
        state.pos_y = y;
        get_track_context(&state, track)
    }

    /// Off the edge of a bridge the baked ground is the road far below it.
    /// A wheel over the deck's overhang stays at road height; past the deck
    /// it is over the drop.
    #[test]
    fn test_bridge_deck_holds_road_height_past_the_edge() {
        let mut track = create_straight_test_track();
        for p in &mut track.centerline {
            p.z = 12.0;
        }
        // 10 m of asphalt each side; everything around is 12 m below.
        track.ground = Some(crate::ground::GroundHeightfield {
            version: 1,
            origin_x: -50.0,
            origin_y: -50.0,
            cell_m: 4.0,
            cols: 100,
            rows: 26,
            heights_cm: vec![0; 2600],
        });

        let on_the_deck = context_at(&track, -11.5);
        assert!(
            (on_the_deck.elevation - 12.0).abs() < 1e-3,
            "1.5 m past the edge is deck, got {}",
            on_the_deck.elevation
        );
        let over_the_drop = context_at(&track, -14.0);
        assert!(
            over_the_drop.elevation < 0.5,
            "4 m past the edge is over the drop, got {}",
            over_the_drop.elevation
        );

        // An ordinary verge, a little below the edge, still blends in.
        let mut verge = track.clone();
        verge.ground.as_mut().unwrap().heights_cm = vec![1150; 2600];
        let blended = context_at(&verge, -10.75);
        assert!(blended.elevation < 12.0 && blended.elevation > 11.5);
    }

    /// Driving over a curb is using the track, not leaving it: the sim has
    /// to agree, or a driver taking the normal line through a chicane is
    /// slowed as if they had put two wheels on the grass.
    #[test]
    fn test_curb_counts_as_on_track() {
        // 10 m of asphalt each side of the centerline, then 1.5 m of curb
        // on the right.
        let track = straight_track_with_right_curb(1.5);

        // Negative y is to the right of a car heading along +x.
        let asphalt = context_at(&track, -9.0);
        assert!(asphalt.is_on_track);
        assert_eq!(asphalt.surface_type, SurfaceType::Asphalt);

        let curb = context_at(&track, -11.0);
        assert!(curb.is_on_track, "1 m past the edge is still curb");
        assert_eq!(curb.surface_type, SurfaceType::Curb);
        assert_eq!(curb.grip_modifier, track.track_surface.curb_grip);

        let past_the_curb = context_at(&track, -12.0);
        assert!(!past_the_curb.is_on_track, "2 m past the edge is grass");
        assert_eq!(past_the_curb.surface_type, SurfaceType::Grass);

        // The curb is on the right edge only.
        let other_side = context_at(&track, 11.0);
        assert!(!other_side.is_on_track);
        assert_eq!(other_side.surface_type, SurfaceType::Grass);
    }

    /// The tarmac run-off past the curb is asphalt to drive on — grip and
    /// no grass drag — but off the track for the lap: a car with all four
    /// wheels on it is struck like one on the grass.
    #[test]
    fn test_tarmac_runoff_is_asphalt_but_off_the_track() {
        // 1.5 m of curb, then tarmac out to 8 m past the edge.
        let track = straight_track_with_right_runoff(1.5, 8.0);

        let runoff = context_at(&track, -15.0);
        assert!(runoff.is_on_track, "5 m past the edge is tarmac run-off");
        assert_eq!(runoff.surface_type, SurfaceType::Asphalt);
        assert!(
            (runoff.grip_modifier - RUNOFF_GRIP_FACTOR).abs() < 1e-4,
            "run-off grip {}",
            runoff.grip_modifier
        );
        let grass = context_at(&track, -19.0);
        assert!(!grass.is_on_track, "9 m past the edge is grass");

        // Track limits: a car wholly on the run-off is off the track.
        let mut state = create_test_car_state();
        state.pos_x = 100.0;
        state.pos_y = -15.0;
        state.vel_x = 30.0;
        state.speed_mps = 30.0;
        let config = CarConfig::default();
        let input = PlayerInputData {
            throttle: 0.5,
            ..Default::default()
        };
        update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        assert!(
            state.wheels_off_track,
            "four wheels on the run-off is off the track"
        );
        assert!(state.is_on_track, "but the physics is on asphalt");
        // And the run-off car coasts as a car on the road does: no grass
        // drag on it.
        let mut on_road = create_test_car_state();
        on_road.pos_x = 100.0;
        on_road.pos_y = -5.0;
        on_road.vel_x = 30.0;
        on_road.speed_mps = 30.0;
        let mut coasting = create_test_car_state();
        coasting.pos_x = 100.0;
        coasting.pos_y = -15.0;
        coasting.vel_x = 30.0;
        coasting.speed_mps = 30.0;
        let coast = PlayerInputData::default();
        for _ in 0..240 {
            update_car_3d(&mut on_road, &config, &coast, &track, 1.0 / 240.0);
            update_car_3d(&mut coasting, &config, &coast, &track, 1.0 / 240.0);
        }
        assert!(
            (coasting.vel_x - on_road.vel_x).abs() < 0.25,
            "run-off {} vs road {} after a second",
            coasting.vel_x,
            on_road.vel_x
        );
    }

    /// Without the sidecar the road edge is the limit, exactly as it was
    /// before curbs were baked for the sim.
    #[test]
    fn test_without_curb_bands_the_road_edge_is_the_limit() {
        let track = create_straight_test_track();
        assert!(track.curbs.is_none());
        let just_off = context_at(&track, -11.0);
        assert!(!just_off.is_on_track);
        assert_eq!(just_off.surface_type, SurfaceType::Grass);
    }

    /// The off-track penalty is what the driver actually feels, so check it
    /// through the real tick rather than the surface query alone.
    #[test]
    fn test_curb_does_not_trigger_the_off_track_penalty() {
        let config = create_test_config();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        let dt = 1.0 / 240.0;

        let run = |track: &TrackConfig, y: f32| {
            let mut state = create_test_car_state();
            state.pos_x = 100.0;
            state.pos_y = y;
            state.vel_x = 50.0;
            state.speed_mps = 50.0;
            state.gear = 4;
            for _ in 0..120 {
                update_car_3d(&mut state, &config, &input, track, dt);
            }
            state.speed_mps
        };

        let curbed = straight_track_with_right_curb(1.5);
        let on_curb = run(&curbed, -11.0);
        let on_asphalt = run(&curbed, -9.0);
        let on_grass = run(&curbed, -12.0);

        assert!(
            (on_curb - on_asphalt).abs() < 0.5,
            "a lap over the curb should coast like the asphalt: {on_curb} vs {on_asphalt}"
        );
        assert!(
            on_grass < on_curb - 0.2,
            "grass should still cost speed: {on_grass} vs {on_curb}"
        );
    }

    /// A car that went off must be able to drive back at a useful speed:
    /// the drag used to be a fraction of speed per second (0.8/s on loaded
    /// tracks), which held a car on full throttle near 8 m/s.
    #[test]
    fn test_car_accelerates_on_grass() {
        let config = create_test_config();
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        let dt = 1.0 / 240.0;
        let mut track = straight_track_with_right_curb(1.5);
        // What `track_loader` gives every real circuit.
        track.track_surface.off_track_grip = 0.6;

        let mut state = create_test_car_state();
        state.pos_x = 100.0;
        state.pos_y = -14.0;
        state.vel_x = 5.0;
        state.speed_mps = 5.0;
        state.gear = 1;
        state.auto_gearbox = true;
        for _ in 0..(240 * 5) {
            update_car_3d(&mut state, &config, &input, &track, dt);
            // Hold the car on the grass line; only the speed matters here.
            state.pos_y = -14.0;
        }
        assert!(!state.is_on_track);
        assert!(
            state.speed_mps > 14.0,
            "five seconds of full throttle on grass should be well past a crawl: {}",
            state.speed_mps
        );
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
            drs: false,
        };

        let dt = 1.0 / 240.0;

        // Run multiple ticks to allow car to accelerate
        for _ in 0..100 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }

        // Speed should increase with throttle
        assert!(
            state.speed_mps > 0.0,
            "Speed should increase: {}",
            state.speed_mps
        );
        // RPM increases as the car gains speed in gear
        assert!(
            state.engine_rpm >= config.idle_rpm,
            "RPM should be at least idle: {}",
            state.engine_rpm
        );
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
            drs: false,
        };

        let dt = 1.0 / 240.0;
        let initial_speed = state.speed_mps;

        update_car_3d(&mut state, &config, &input, &track, dt);

        assert!(
            state.speed_mps < initial_speed,
            "Speed should decrease from braking"
        );
    }

    #[test]
    fn test_full_braking_with_abs_achieves_near_peak_grip_decel() {
        // With ABS the tires must brake near the friction limit (~1g for
        // grip 1.0), not the ~0.5g of a locked wheel deep in the falloff.
        let mut state = create_test_car_state();
        state.vel_x = 60.0;
        state.speed_mps = 60.0;
        state.gear = 5;

        let config = create_test_config();
        assert!(config.abs_enabled);
        let track = create_straight_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };

        // Measure the average decel between 55 and 25 m/s
        let dt = 1.0 / 240.0;
        let mut ticks_in_window = 0u32;
        for _ in 0..240 * 6 {
            update_car_3d(&mut state, &config, &input, &track, dt);
            if (25.0..55.0).contains(&state.speed_mps) {
                ticks_in_window += 1;
            }
            if state.speed_mps < 25.0 {
                break;
            }
        }
        assert!(state.speed_mps < 25.0, "car should slow substantially");
        let decel = (55.0 - 25.0) / (ticks_in_window as f32 * dt);
        assert!(
            decel > 8.0,
            "ABS braking should reach near-peak decel (~1g), got {:.1} m/s² over {} ticks",
            decel,
            ticks_in_window
        );
    }

    #[test]
    fn auto_gearbox_shifts_from_the_cars_own_rev_range() {
        let config = create_test_config();
        let redline = config.redline_rpm;
        let top = config.gear_ratios.len() as i8 - 1;
        let dt = 1.0 / 240.0;
        let coast = PlayerInputData::default();
        let full = PlayerInputData {
            throttle: 1.0,
            ..PlayerInputData::default()
        };

        // Near the limiter: up one.
        let mut state = create_test_car_state();
        state.auto_gearbox = true;
        state.gear = 2;
        state.engine_rpm = redline * 0.99;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), Some(3));
        // ...but not past the last gear.
        state.auto_shift_hold_ticks = 0;
        state.gear = top;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), None);

        // Revs fallen out of the band off throttle: down one.
        state.auto_shift_hold_ticks = 0;
        state.gear = 4;
        state.engine_rpm = redline * 0.5;
        assert_eq!(
            auto_gear_selection(&mut state, &config, &coast, dt),
            Some(3)
        );

        // Right after an upshift the revs sit where the shift left them: on
        // throttle that must not bounce straight back down (hunting).
        state.auto_shift_hold_ticks = 0;
        state.gear = 2;
        let shifted_at = auto_upshift_rpm(&config, 1).unwrap();
        state.engine_rpm = shifted_at * config.gear_ratios[2] / config.gear_ratios[1];
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), None);

        // Neutral goes to first on throttle, never to reverse.
        state.auto_shift_hold_ticks = 0;
        state.gear = 0;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), Some(1));
        state.auto_shift_hold_ticks = 0;
        state.gear = -1;
        assert_eq!(auto_gear_selection(&mut state, &config, &coast, dt), None);
    }

    /// An engine whose torque falls away hard after a mid-range peak, on a
    /// close-ratio box: the torque crossover sits well short of the redline.
    fn peaky_config() -> CarConfig {
        let mut config = create_test_config();
        config.idle_rpm = 1000.0;
        config.redline_rpm = 9000.0;
        config.engine.friction_torque_nm = 0.0;
        config.gear_ratios = vec![-3.0, 3.0, 2.5, 2.1, 1.8];
        config.engine.torque_curve = [(1000.0, 200.0), (5000.0, 400.0), (9000.0, 150.0)]
            .into_iter()
            .map(|(rpm, torque_nm)| TorqueCurvePoint { rpm, torque_nm })
            .collect();
        config
    }

    #[test]
    fn auto_gearbox_upshifts_where_the_next_gear_pulls_harder() {
        let config = peaky_config();
        let dt = 1.0 / 240.0;
        let full = PlayerInputData {
            throttle: 1.0,
            ..PlayerInputData::default()
        };
        let crossover = auto_upshift_rpm(&config, 1).unwrap();
        assert!(
            crossover < config.redline_rpm * 0.9,
            "a peaky engine on close ratios should shift well short of the              redline, not at it: {crossover}"
        );
        // It really is the crossover: second pulls harder above it, first
        // below it.
        let (first, second) = (config.gear_ratios[1], config.gear_ratios[2]);
        let at = |rpm: f32, ratio: f32| full_throttle_net_torque_nm(&config, rpm) * ratio;
        let above = crossover + 50.0;
        let below = crossover - 50.0;
        assert!(at(above * second / first, second) > at(above, first));
        assert!(at(below * second / first, second) <= at(below, first));

        let mut state = create_test_car_state();
        state.auto_gearbox = true;
        state.gear = 1;
        state.engine_rpm = below;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), None);
        state.engine_rpm = above;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), Some(2));
    }

    #[test]
    fn auto_gearbox_kicks_down_on_throttle_but_not_when_cruising() {
        let config = peaky_config();
        let dt = 1.0 / 240.0;
        let full = PlayerInputData {
            throttle: 1.0,
            ..PlayerInputData::default()
        };
        let cruise = PlayerInputData {
            throttle: 0.3,
            ..PlayerInputData::default()
        };
        // Third gear, well below the peak: second would land near the peak
        // and put far more torque down.
        let mut state = create_test_car_state();
        state.auto_gearbox = true;
        state.gear = 3;
        state.engine_rpm = 3500.0;
        assert_eq!(auto_gear_selection(&mut state, &config, &full, dt), Some(2));

        // Easing along at the same revs the box leaves the gear alone: the
        // revs are still above the band it drops out of off throttle.
        state.auto_shift_hold_ticks = 0;
        state.gear = 3;
        state.engine_rpm = config.redline_rpm * AUTO_DOWNSHIFT_FRAC + 100.0;
        assert_eq!(auto_gear_selection(&mut state, &config, &cruise, dt), None);

        // A kickdown that would throw the engine at the limiter is refused,
        // however hard the lower gear would pull. (Torque rising to the
        // redline, so the gear below always pulls harder and upshifting is
        // never the better answer at these revs.)
        let mut rising = config.clone();
        rising.engine.torque_curve = [(1000.0, 200.0), (9000.0, 400.0)]
            .into_iter()
            .map(|(rpm, torque_nm)| TorqueCurvePoint { rpm, torque_nm })
            .collect();
        state.auto_shift_hold_ticks = 0;
        state.gear = 3;
        state.engine_rpm = rising.redline_rpm * 0.85;
        assert_eq!(auto_gear_selection(&mut state, &rising, &full, dt), None);
    }

    /// Flat torque to the redline has no crossover: the box must still
    /// shift, at the limiter backstop, rather than sit on the limiter.
    #[test]
    fn auto_gearbox_shifts_at_the_backstop_when_torque_never_falls() {
        let mut config = peaky_config();
        config.engine.torque_curve = [(1000.0, 300.0), (9000.0, 300.0)]
            .into_iter()
            .map(|(rpm, torque_nm)| TorqueCurvePoint { rpm, torque_nm })
            .collect();
        let backstop = config.redline_rpm * AUTO_UPSHIFT_CEILING_FRAC;
        assert_eq!(auto_upshift_rpm(&config, 1), Some(backstop));
        assert_eq!(auto_upshift_rpm(&config, 4), None, "no gear above top");
    }

    #[test]
    fn auto_gearbox_downshifts_while_braking_to_a_stop() {
        let mut state = create_test_car_state();
        state.auto_gearbox = true;
        state.vel_x = 45.0;
        state.speed_mps = 45.0;
        state.gear = 4;
        let config = create_test_config();
        let track = create_straight_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        let dt = 1.0 / 240.0;
        let mut lowest = state.gear;
        // Up to the moment it stops: held on, the brake then selects reverse.
        for _ in 0..(240 * 6) {
            update_car_3d(&mut state, &config, &input, &track, dt);
            lowest = lowest.min(state.gear);
            if state.speed_mps < 0.5 {
                break;
            }
        }
        assert!(
            state.speed_mps < 2.0,
            "should have stopped, still doing {:.1} m/s",
            state.speed_mps
        );
        assert_eq!(
            lowest, 1,
            "the box should have worked its way down to first"
        );
        assert_eq!(state.gear, 1);
    }

    fn forward_speed(state: &CarState) -> f32 {
        state.vel_x * state.yaw_rad.cos() + state.vel_y * state.yaw_rad.sin()
    }

    #[test]
    fn auto_gearbox_reverses_on_the_brake_at_a_standstill() {
        let mut state = create_test_car_state();
        state.auto_gearbox = true;
        state.gear = 1;
        let config = create_test_config();
        let track = create_straight_test_track();
        let dt = 1.0 / 240.0;
        let brake = PlayerInputData {
            brake: 1.0,
            ..Default::default()
        };

        // A short press at rest (stopping on the grid) stays in first.
        for _ in 0..(AUTO_REVERSE_HOLD_S * 0.5 / dt) as usize {
            update_car_3d(&mut state, &config, &brake, &track, dt);
        }
        assert_eq!(state.gear, 1);
        assert_eq!(state.speed_mps, 0.0);

        // Held on, it engages reverse and the brake pedal drives backwards.
        for _ in 0..(240 * 2) {
            update_car_3d(&mut state, &config, &brake, &track, dt);
        }
        assert_eq!(state.gear, -1);
        assert!(
            forward_speed(&state) < -3.0,
            "should be backing up, v_long={:.2}",
            forward_speed(&state)
        );

        // The throttle now brakes the car to a stop, then takes it forwards.
        let throttle = PlayerInputData {
            throttle: 1.0,
            ..Default::default()
        };
        let mut stopped_in_reverse = false;
        for _ in 0..(240 * 3) {
            update_car_3d(&mut state, &config, &throttle, &track, dt);
            if state.gear == -1 && state.speed_mps < DIRECTION_CHANGE_MAX_SPEED_MPS {
                stopped_in_reverse = true;
            }
        }
        assert!(stopped_in_reverse, "the throttle should brake in reverse");
        assert!(state.gear >= 1);
        assert!(
            forward_speed(&state) > 3.0,
            "should be driving forwards again, v_long={:.2}",
            forward_speed(&state)
        );
    }

    #[test]
    fn reverse_gear_backs_up_and_steers_the_right_way() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_straight_test_track();
        let dt = 1.0 / 240.0;
        let input = PlayerInputData {
            throttle: 0.6,
            steering: 0.5,
            gear: Some(-1),
            ..Default::default()
        };
        // Manual box: the throttle drives, the shift lands at the end of the
        // first tick.
        for _ in 0..240 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }
        assert_eq!(state.gear, -1);
        assert!(
            forward_speed(&state) < -1.0,
            "v_long={}",
            forward_speed(&state)
        );
        // Backing up with the wheels turned left swings the nose right.
        assert!(
            state.angular_vel_yaw < 0.0,
            "yaw rate {} should be clockwise",
            state.angular_vel_yaw
        );
        assert!(state.yaw_rad < 0.0);
    }

    #[test]
    fn a_car_rolling_backwards_is_slowed_by_drag_and_holds_its_line() {
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_straight_test_track();
        let dt = 1.0 / 240.0;
        state.pos_x = 500.0;
        state.gear = 0;
        state.vel_x = -20.0;
        state.speed_mps = 20.0;
        // A small sideways drift that the tyres must take out.
        state.vel_y = 0.5;
        for _ in 0..240 {
            update_car_3d(&mut state, &config, &PlayerInputData::default(), &track, dt);
        }
        assert!(
            state.vel_x > -20.0 && state.vel_x < 0.0,
            "drag should slow a reversing car, vel_x={}",
            state.vel_x
        );
        assert!(state.vel_y.abs() < 0.5, "vel_y={}", state.vel_y);
    }

    #[test]
    fn a_free_revving_engine_climbs_bounces_off_the_limiter_and_falls_back() {
        let config = create_test_config();
        let limiter = config.engine.rev_limiter_rpm.max(config.redline_rpm);
        let dt = 1.0 / 240.0;
        let mut rpm = config.idle_rpm;
        let mut peak = rpm;
        let mut cuts = 0;
        for _ in 0..(240 * 2) {
            let next = free_rev_rpm(rpm, &config, 1.0, dt);
            if next < rpm {
                cuts += 1;
            }
            rpm = next;
            peak = peak.max(rpm);
        }
        assert!(peak < limiter, "never past the limiter, peak={peak}");
        assert!(peak > limiter - FREE_REV_LIMITER_DROP_RPM);
        assert!(cuts >= 5, "bouncing off the limiter, {cuts} cuts");

        // Half throttle holds part way; letting go returns to idle.
        let mut half = config.idle_rpm;
        for _ in 0..(240 * 3) {
            half = free_rev_rpm(half, &config, 0.5, dt);
        }
        assert!(half > config.idle_rpm + 1000.0 && half < limiter - 500.0);
        for _ in 0..(240 * 4) {
            half = free_rev_rpm(half, &config, 0.0, dt);
        }
        assert!(half < config.idle_rpm + 50.0, "back to idle, rpm={half}");
    }

    #[test]
    fn reverse_only_engages_once_the_car_stops_rolling_forwards() {
        assert!(!gear_engages(1, -1, 10.0));
        assert!(!gear_engages(0, -1, 10.0));
        assert!(gear_engages(1, 0, 10.0));
        assert!(gear_engages(1, -1, 0.2));
        assert!(!gear_engages(-1, 1, -5.0));
        assert!(gear_engages(-1, 1, -0.2));
        assert!(gear_engages(-1, 0, -5.0));
        assert!(gear_engages(3, 2, 40.0));

        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_straight_test_track();
        state.pos_x = 500.0;
        state.gear = 3;
        state.vel_x = 30.0;
        state.speed_mps = 30.0;
        let input = PlayerInputData {
            gear: Some(-1),
            ..Default::default()
        };
        update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        assert_eq!(state.gear, 3, "reverse must not go in at 30 m/s");
    }

    /// A straight along +x with a point every metre, climbing `grade` and
    /// banked `banking_rad`, like the real circuits' adaptive centerlines.
    fn graded_straight(grade: f32, banking_rad: f32) -> TrackConfig {
        let slope_rad = grade.atan();
        TrackConfig {
            centerline: (0..3000)
                .map(|i| TrackPoint {
                    x: i as f32,
                    z: i as f32 * grade,
                    distance_from_start_m: i as f32,
                    width_left_m: 10.0,
                    width_right_m: 10.0,
                    slope_rad,
                    banking_rad,
                    ..Default::default()
                })
                .collect(),
            ..TrackConfig::default()
        }
    }

    #[test]
    fn surface_height_is_continuous_between_centerline_points() {
        // The height under a wheel must not step at each centerline point:
        // a step taken in one tick reads as a violent suspension stroke.
        let track = graded_straight(0.03, 0.0);
        let mut previous: Option<f32> = None;
        let mut hint = None;
        for i in 0..2000 {
            let x = 100.0 + i as f32 * 0.005;
            let sample = query_track_surface_centerline(&track, x, 0.7, hint).unwrap();
            hint = Some(sample.nearest_point);
            assert!((sample.elevation - x * 0.03).abs() < 1e-3);
            if let Some(previous) = previous {
                assert!(
                    (sample.elevation - previous).abs() < 1e-3,
                    "height jumped {:.4} m at x={x}",
                    sample.elevation - previous
                );
            }
            previous = Some(sample.elevation);
        }
    }

    #[test]
    fn rear_loads_stay_even_at_speed_on_a_graded_straight() {
        // Flat out up a 3% grade, a hair off the centerline's heading so the
        // two rear wheels pass each centerline point on different ticks.
        // With the ground read from the nearest point the surface was a
        // staircase, the wheel that dropped a step first took the whole
        // axle's load from the damper, and the other one spun up under
        // drive: several times a second on the Mulsanne, and the car
        // wobbled and stepped out on the straight.
        let track = graded_straight(0.03, 0.0);
        let config = create_test_config();
        let mut state = create_test_car_state();
        state.pos_x = 20.0;
        state.pos_z = 20.0 * 0.03;
        state.yaw_rad = 0.01;
        state.vel_x = 70.0 * state.yaw_rad.cos();
        state.vel_y = 70.0 * state.yaw_rad.sin();
        state.speed_mps = 70.0;
        state.gear = 5;
        state.auto_gearbox = true;
        let input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        let dt = 1.0 / 240.0;
        let mut worst: f32 = 0.0;
        for tick in 0..(240 * 3) {
            update_car_3d(&mut state, &config, &input, &track, dt);
            if tick > 10 {
                let (rl, rr) = (state.weight_rear_left_n, state.weight_rear_right_n);
                worst = worst.max((rl - rr).abs() / (rl + rr));
            }
        }
        assert!(
            worst < 0.1,
            "rear loads split {:.0}% between the wheels on a straight",
            worst * 100.0
        );
        assert!(
            state.angular_vel_yaw.abs() < 0.01,
            "car is yawing at {:.3} rad/s",
            state.angular_vel_yaw
        );
    }

    #[test]
    fn banking_pulls_the_car_toward_the_low_edge() {
        // Positive banking lifts the left edge, so a car coasting down a
        // banked straight drifts right, toward the low side; driven the
        // other way it drifts toward the same edge, now on its left.
        let track = graded_straight(0.0, 0.2);
        let config = create_test_config();
        for (yaw, heading_sign) in [(0.0f32, 1.0f32), (std::f32::consts::PI, -1.0)] {
            let mut state = create_test_car_state();
            state.pos_x = 1500.0;
            state.yaw_rad = yaw;
            state.vel_x = 30.0 * heading_sign;
            state.speed_mps = 30.0;
            let input = PlayerInputData::default();
            let start_y = state.pos_y;
            for _ in 0..240 {
                update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            }
            assert!(
                state.pos_y < start_y - 0.05,
                "car heading {yaw:.2} moved {:.3} m across a bank lifting the left edge",
                state.pos_y - start_y
            );
        }
    }

    #[test]
    fn banked_road_does_not_load_the_lower_wheels() {
        // The body sits parallel to a banked road, so its suspension is
        // evenly compressed and left and right carry the same load. With
        // every hub at the centre's height the lower wheels were held
        // compressed, the upper ones hung out, and a straight line on a
        // cambered road ran with kilonewtons more on one side.
        let track = graded_straight(0.0, 0.06);
        let config = create_test_config();
        let mut state = create_test_car_state();
        state.pos_x = 100.0;
        let input = PlayerInputData::default();
        update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        let s = &state.suspension;
        assert!(
            (s.front_left_travel_m - s.front_right_travel_m).abs() < 1e-3,
            "front suspension {:.3} vs {:.3}",
            s.front_left_travel_m,
            s.front_right_travel_m
        );
        assert!(
            (state.weight_front_left_n - state.weight_front_right_n).abs() < 50.0,
            "front loads {:.0} vs {:.0}",
            state.weight_front_left_n,
            state.weight_front_right_n
        );
        assert!(
            (state.weight_rear_left_n - state.weight_rear_right_n).abs() < 50.0,
            "rear loads {:.0} vs {:.0}",
            state.weight_rear_left_n,
            state.weight_rear_right_n
        );
    }

    /// A straight far wider than any turn in these tests, so a car can
    /// corner for seconds without leaving the asphalt.
    fn open_asphalt() -> TrackConfig {
        TrackConfig {
            centerline: (0..2000)
                .map(|i| TrackPoint {
                    x: i as f32 * 4.0,
                    distance_from_start_m: i as f32 * 4.0,
                    width_left_m: 2000.0,
                    width_right_m: 2000.0,
                    ..Default::default()
                })
                .collect(),
            ..TrackConfig::default()
        }
    }

    /// Steering input held for `seconds` at a steady `speed`; the feedback
    /// collected over the last tick.
    fn feedback_after_steering(steering: f32, speed: f32, seconds: f32) -> DriverFeedback {
        let config = create_test_config();
        let track = open_asphalt();
        let mut state = create_test_car_state();
        state.vel_x = speed;
        state.speed_mps = speed;
        state.gear = 3;
        for _ in 0..(seconds * 240.0) as u32 {
            state.feedback.take(0);
            let input = PlayerInputData {
                throttle: ((speed - state.speed_mps) * 0.5 + 0.2).clamp(0.0, 1.0),
                steering,
                ..Default::default()
            };
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        }
        state.feedback.take(0)
    }

    /// The column stiffness is what lets a wheel answer its own movement
    /// without a round trip: one tick after the rim moves the car has not
    /// yet turned, so the torque's change is the tyres' slip changing with
    /// the steering, which is what the slope predicts.
    #[test]
    fn column_stiffness_predicts_the_torque_after_the_rim_moves() {
        let config = create_test_config();
        let track = open_asphalt();
        for (steer, speed) in [
            (0.03f32, 30.0f32),
            (0.08, 30.0),
            (0.05, 15.0),
            (-0.04, 45.0),
        ] {
            let mut state = create_test_car_state();
            state.vel_x = speed;
            state.speed_mps = speed;
            state.gear = 3;
            let drive = |state: &mut CarState, steering: f32| {
                state.feedback.take(0);
                let input = PlayerInputData {
                    throttle: ((speed - state.speed_mps) * 0.5 + 0.2).clamp(0.0, 1.0),
                    steering,
                    ..Default::default()
                };
                update_car_3d(state, &config, &input, &track, 1.0 / 240.0);
                state.feedback.take(0)
            };
            for _ in 0..360 {
                drive(&mut state, steer);
            }
            let settled = drive(&mut state, steer);
            assert_eq!(settled.steer_input, steer);
            // Against the same car driven on unchanged for the tick, so only
            // the rim's movement differs.
            let mut control = state.clone();
            let held = drive(&mut control, steer).steer_torque[0];
            // A couple of degrees of rim: a slope is a slope only so far,
            // and at the aligning crest (0.08 here) the torque curves.
            let step = 0.01 * steer.signum();
            let moved = drive(&mut state, steer + step);
            let predicted = held + settled.steer_stiffness * step;
            let actual = moved.steer_torque[0];
            let change = actual - held;
            assert!(
                settled.steer_stiffness < 0.0,
                "gripping, turning further in pushes back harder: stiffness {} at {steer}",
                settled.steer_stiffness
            );
            assert!(
                (predicted - actual).abs() < 0.25 * change.abs() + 0.01,
                "steer {steer} at {speed} m/s: predicted {predicted}, got {actual} (from {held})"
            );
        }
    }

    #[test]
    fn column_stiffness_falls_through_the_aligning_crest() {
        let config = create_test_config();
        let static_front = config.mass_kg * GRAVITY * config.weight_distribution_front;
        let stiffness = |n: f32| {
            steering_column_stiffness(
                front_axle_at(&config, n, 1.0),
                &config,
                static_front,
                0.0,
                config.max_steering_angle_rad,
            )
        };
        // A left turn: steering further left adds torque to the right.
        assert!(stiffness(0.1) < 0.0, "{}", stiffness(0.1));
        assert!(
            stiffness(0.2) < 0.5 * stiffness(0.05),
            "it softens toward the crest"
        );
        assert!(
            stiffness(1.0) > 0.0,
            "past the crest more lock is lighter: {}",
            stiffness(1.0)
        );
    }

    #[test]
    fn steering_torque_centres_the_wheel_and_is_quiet_straight_ahead() {
        let straight = feedback_after_steering(0.0, 30.0, 0.5);
        assert!(
            straight.steer_torque[0].abs() < 0.01,
            "straight ahead the wheel should carry no torque: {:?}",
            straight.steer_torque
        );

        // Positive steering is left; the tyres push back toward centre.
        let left = feedback_after_steering(0.05, 25.0, 1.5).steer_torque[0];
        let right = feedback_after_steering(-0.05, 25.0, 1.5).steer_torque[0];
        assert!(left < -0.05 && left > -1.5, "left turn torque {left}");
        assert!(right > 0.05 && right < 1.5, "right turn torque {right}");
        assert!(
            (left + right).abs() < 0.1 * left.abs(),
            "the two sides should mirror: {left} vs {right}"
        );
    }

    /// A front axle at `n` times the peak slip angle in a left turn, both
    /// tyres at `load_ratio` times their static load.
    fn front_axle_at(config: &CarConfig, n: f32, load_ratio: f32) -> [FrontTyre; 2] {
        let static_front = config.mass_kg * GRAVITY * config.weight_distribution_front;
        let load = static_front / 2.0 * load_ratio;
        let d = config.tire_config.grip_coefficient * load;
        let peak = config.tire_config.optimal_slip_angle_rad;
        // A left turn: the tyres run a negative slip angle and push left.
        let tyre = FrontTyre {
            fy: -pacejka(d, PACEJKA_C_LAT, -n),
            fx: 0.0,
            slip_angle: -n * peak,
            load_n: load,
            steer_rad: 0.0,
            grip_n: d,
            lateral_scale: 1.0,
        };
        [tyre, tyre]
    }

    fn column_torque(config: &CarConfig, front: [FrontTyre; 2]) -> f32 {
        let static_front = config.mass_kg * GRAVITY * config.weight_distribution_front;
        steering_column_torque(front, config, static_front)
    }

    #[test]
    fn steering_goes_light_as_the_front_tyres_pass_their_peak() {
        let config = create_test_config();
        let torque_at = |n: f32| column_torque(&config, front_axle_at(&config, n, 1.0));
        let weight = |n: f32| -torque_at(n);

        assert!(torque_at(0.5) < 0.0, "the torque is against the lock");
        assert!((weight(0.2) - 0.53).abs() < 0.02, "{}", weight(0.2));
        assert!((weight(0.45) - 0.69).abs() < 0.02, "{}", weight(0.45));
        assert!((weight(1.0) - 0.47).abs() < 0.02, "{}", weight(1.0));
        assert!((weight(1.5) - 0.21).abs() < 0.02, "{}", weight(1.5));
        assert!(weight(0.2) < weight(0.45), "it builds with cornering force");
        assert!(
            weight(1.0) < 0.75 * weight(0.45),
            "and is plainly lighter by the grip peak: {} vs {}",
            weight(1.0),
            weight(0.45)
        );
        assert!(
            weight(1.5) < 0.35 * weight(0.45),
            "a washed-out front is light: {}",
            weight(1.5)
        );
        assert!(weight(3.0) > 0.1, "the caster trail keeps some centring");
    }

    #[test]
    fn a_loaded_front_axle_makes_the_steering_heavier() {
        let config = create_test_config();
        let weight = |load: f32| -column_torque(&config, front_axle_at(&config, 0.4, load));
        // Braking or downforce: more load, a longer contact patch and more
        // force for the same slip.
        assert!(
            weight(1.4) > 1.5 * weight(1.0),
            "{} vs {}",
            weight(1.4),
            weight(1.0)
        );
        // Under power or over a crest the front goes light.
        assert!(
            weight(0.6) < 0.6 * weight(1.0),
            "{} vs {}",
            weight(0.6),
            weight(1.0)
        );

        // The outside tyre carries the torque in a corner: the same total
        // load split 1.6 : 0.4 steers heavier than an even split.
        let mut split = front_axle_at(&config, 0.4, 1.0);
        for (tyre, ratio) in split.iter_mut().zip([0.4f32, 1.6]) {
            let even = front_axle_at(&config, 0.4, ratio)[0];
            *tyre = even;
        }
        assert!(-column_torque(&config, split) > weight(1.0));
    }

    #[test]
    fn a_front_tyre_braking_alone_pulls_the_wheel_toward_it() {
        let config = create_test_config();
        let mut front = front_axle_at(&config, 0.0, 1.0);
        front[0].fx = -3000.0; // the left front brakes
        assert!(column_torque(&config, front) > 0.05, "pulls left");
        front[0].fx = 0.0;
        front[1].fx = -3000.0;
        assert!(column_torque(&config, front) < -0.05, "pulls right");
        front[0].fx = -3000.0;
        assert!(
            column_torque(&config, front).abs() < 1e-4,
            "even braking cancels"
        );
    }

    #[test]
    fn the_axle_weight_centres_a_steered_wheel_at_a_standstill() {
        let config = create_test_config();
        let mut front = front_axle_at(&config, 0.0, 1.0);
        for tyre in front.iter_mut() {
            tyre.steer_rad = 0.3;
        }
        let parked = column_torque(&config, front);
        assert!(parked < -0.05, "steered left, pulled back right: {parked}");
        for tyre in front.iter_mut() {
            tyre.load_n *= 1.5;
        }
        assert!(
            column_torque(&config, front) < 1.4 * parked,
            "heavier with load"
        );
    }

    #[test]
    fn a_hit_yanks_the_steering_toward_the_side_that_was_struck() {
        let config = create_test_config();
        let state = create_test_car_state(); // heading +X, left is +Y
                                             // Struck from the right: shoved left.
        let from_right = impact_steer_kick(&state, &config, (0.0, 4.0), 0.0, None);
        assert!(
            from_right < -0.5,
            "turns right, toward the hit: {from_right}"
        );
        let from_left = impact_steer_kick(&state, &config, (0.0, -4.0), 0.0, None);
        assert!((from_left + from_right).abs() < 1e-5);

        // The left front corner stopped by a wall ahead.
        let half_l = config.length_m / 2.0;
        let half_w = config.width_m / 2.0;
        let corner = impact_steer_kick(&state, &config, (-6.0, 0.0), 0.0, Some((half_l, half_w)));
        assert!(
            corner > 0.5,
            "turns left, toward the stopped corner: {corner}"
        );
        // The same jolt at the back is the body's, not the wheels'.
        let rear = impact_steer_kick(&state, &config, (-6.0, 0.0), 0.0, Some((-half_l, half_w)));
        assert_eq!(rear, 0.0);
        assert!(
            impact_steer_kick(&state, &config, (0.0, 1000.0), 0.0, None) >= -MAX_STEER_KICK,
            "capped"
        );
    }

    /// Which wheels touch the curb is judged per wheel and reported on the
    /// car's real sides, in FL, FR, RL, RR order.
    #[test]
    fn feedback_reports_the_surface_under_each_wheel() {
        // Asphalt to 10 m right of the centerline, then 1.5 m of curb. The
        // wheels sit about 0.8 m either side of the car's centre.
        let track = straight_track_with_right_curb(1.5);
        let config = create_test_config();
        let surfaces_at = |y: f32| {
            let mut state = create_test_car_state();
            state.pos_x = 100.0;
            state.pos_y = y;
            state.vel_x = 30.0;
            state.speed_mps = 30.0;
            state.gear = 3;
            let input = PlayerInputData::default();
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            state.feedback.take(0).surface
        };
        let (road, curb, off) = (
            ContactSurface::Road as u8,
            ContactSurface::Curb as u8,
            ContactSurface::Off as u8,
        );

        // Negative y is right of a car heading +x: right wheels on the curb.
        assert_eq!(surfaces_at(-10.2), [road, curb, road, curb]);
        // Further out: left wheels on the curb, right wheels past it.
        assert_eq!(surfaces_at(-11.2), [curb, off, curb, off]);
        assert_eq!(surfaces_at(0.0), [road; 4]);
    }

    #[test]
    fn feedback_tells_abs_from_a_locked_wheel() {
        let track = create_straight_test_track();
        let run = |abs: bool| {
            let mut config = create_test_config();
            config.abs_enabled = abs;
            let mut state = create_test_car_state();
            state.pos_x = 100.0;
            state.vel_x = 40.0;
            state.speed_mps = 40.0;
            state.gear = 4;
            let input = PlayerInputData {
                brake: 1.0,
                ..Default::default()
            };
            for _ in 0..24 {
                update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            }
            state.feedback.take(0)
        };

        let with_abs = run(true);
        assert!(with_abs.abs_active);
        let deepest = with_abs.slip_ratio.iter().cloned().fold(0.0f32, f32::min);
        assert!(
            (deepest + 1.0).abs() < 0.05,
            "ABS holds the tyres at their peak slip: {:?}",
            with_abs.slip_ratio
        );

        let locked = run(false);
        assert!(!locked.abs_active);
        assert!(
            locked.slip_ratio.iter().any(|&s| s < -5.0),
            "a locked wheel is far past its peak: {:?}",
            locked.slip_ratio
        );
    }

    use crate::walls::WallSegment;

    fn wall_across(x: f32, kind: u8) -> Walls {
        Walls::from_segments(vec![WallSegment {
            x0: x,
            y0: -30.0,
            x1: x,
            y1: 30.0,
            z: 0.0,
            height_m: 1.0,
            kind,
        }])
    }

    fn car_at(config: &CarConfig, x: f32, y: f32, yaw_rad: f32) -> CarState {
        let slot = GridSlot {
            position: 1,
            x,
            y,
            z: 0.0,
            yaw_rad,
        };
        CarState::new(Uuid::new_v4(), config.id, &slot)
    }

    /// Step a car on at its velocity, resolving walls each tick, and
    /// report whether it ever touched one.
    fn drive_into_walls(
        state: &mut CarState,
        config: &CarConfig,
        walls: &Walls,
        ticks: usize,
    ) -> bool {
        let dt = 1.0 / 240.0;
        let mut nearby = Vec::new();
        let mut hit = false;
        for _ in 0..ticks {
            state.pos_x += state.vel_x * dt;
            state.pos_y += state.vel_y * dt;
            state.is_colliding = false;
            resolve_wall_contacts(state, config, walls, dt, &mut nearby);
            hit |= state.is_colliding;
        }
        hit
    }

    #[test]
    fn a_wall_stops_a_car_driving_into_it() {
        let config = create_test_config();
        let mut state = car_at(&config, 0.0, 0.0, 0.0);
        state.vel_x = 40.0;
        state.speed_mps = 40.0;
        let walls = wall_across(10.0, 2);
        let dt = 1.0 / 240.0;
        let mut nearby = Vec::new();
        let mut hit = false;
        for _ in 0..240 {
            state.pos_x += state.vel_x * dt;
            resolve_wall_contacts(&mut state, &config, &walls, dt, &mut nearby);
            hit |= state.is_colliding;
            assert!(
                state.pos_x + config.length_m / 2.0 <= 10.0 + 0.2,
                "nose through the wall at x = {}",
                state.pos_x
            );
        }
        assert!(hit, "never touched the wall");
        assert!(state.vel_x <= 0.0, "still going forward at {}", state.vel_x);
        assert!(
            state.damage.front_damage_percent > 0.0,
            "a 144 km/h hit dents the nose"
        );
        assert!(
            state.feedback.take(0).impact_mps > 30.0,
            "the driver feels the hit"
        );
    }

    #[test]
    fn a_glancing_hit_keeps_the_car_moving_along_the_wall() {
        let config = create_test_config();
        // A wall along the road to the car's left, the car drifting into it.
        let wall_y = config.width_m / 2.0 + 0.4;
        let walls = Walls::from_segments(vec![WallSegment {
            x0: -50.0,
            y0: wall_y,
            x1: 300.0,
            y1: wall_y,
            z: 0.0,
            height_m: 1.0,
            kind: 0,
        }]);
        let mut state = car_at(&config, 0.0, 0.0, 0.0);
        state.vel_x = 30.0;
        state.vel_y = 2.0;
        state.speed_mps = state.vel_x.hypot(state.vel_y);
        assert!(drive_into_walls(&mut state, &config, &walls, 240));
        assert!(
            state.vel_y <= 0.0,
            "still moving into the wall at {}",
            state.vel_y
        );
        assert!(
            state.vel_x > 20.0,
            "a glancing armco hit is not a stop: {}",
            state.vel_x
        );
        assert!(
            state.pos_y + config.width_m / 2.0 <= wall_y + 0.1,
            "flank through the wall at y = {}",
            state.pos_y
        );
    }

    #[test]
    fn a_wall_the_car_is_not_level_with_is_ignored() {
        let config = create_test_config();
        for wall_z in [5.0f32, -3.0] {
            let mut segments = wall_across(10.0, 2).segments().to_vec();
            segments[0].z = wall_z;
            let walls = Walls::from_segments(segments);
            let mut state = car_at(&config, 0.0, 0.0, 0.0);
            state.vel_x = 40.0;
            let hit = drive_into_walls(&mut state, &config, &walls, 120);
            assert!(!hit, "hit a wall footed at z = {wall_z}");
            assert!(state.pos_x > 15.0);
        }
    }

    #[test]
    fn a_nose_first_hit_turns_the_car() {
        let config = create_test_config();
        // Yawed a little left, so the right-front corner meets the wall
        // first and the push on it swings the nose right (clockwise).
        let mut state = car_at(&config, 0.0, 0.0, 0.2);
        state.vel_x = 30.0;
        state.speed_mps = 30.0;
        let walls = wall_across(10.0, 2);
        assert!(drive_into_walls(&mut state, &config, &walls, 240));
        assert!(
            state.angular_vel_yaw < -0.1,
            "expected a clockwise kick, got {}",
            state.angular_vel_yaw
        );
    }

    #[test]
    fn tires_absorb_more_of_the_hit_than_concrete() {
        let config = create_test_config();
        let bounce = |kind: u8| {
            let mut state = car_at(&config, 0.0, 0.0, 0.0);
            state.vel_x = 30.0;
            drive_into_walls(&mut state, &config, &wall_across(10.0, kind), 120);
            -state.vel_x
        };
        assert!(
            bounce(1) < bounce(2),
            "tires {} vs concrete {}",
            bounce(1),
            bounce(2)
        );
        assert!(bounce(1) >= 0.0);
    }

    #[test]
    fn a_track_without_walls_stops_nothing() {
        let config = create_test_config();
        let track = TrackConfig::default();
        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());
        let mut state = car_at(&config, 0.0, 0.0, 0.0);
        state.vel_x = 40.0;
        let mut refs = vec![&mut state];
        check_wall_collisions(&mut refs, &configs, &track, 1.0 / 240.0);
        assert!(!state.is_colliding);
        assert_eq!(state.vel_x, 40.0);
    }

    #[test]
    fn a_collision_is_felt_by_both_drivers() {
        let config = create_test_config();
        // Nose to tail with half a metre of overlap, so the contact normal
        // is along the cars' length; closing at 8 m/s.
        let slot = |position: u8, x: f32| GridSlot {
            position,
            x,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let mut states = vec![
            CarState::new(Uuid::new_v4(), config.id, &slot(1, 0.0)),
            CarState::new(Uuid::new_v4(), config.id, &slot(2, config.length_m - 0.5)),
        ];
        states[0].vel_x = 5.0;
        states[1].vel_x = -3.0;
        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        check_obb_collisions_3d(&mut states, &configs);

        for state in &mut states {
            let impact = state.feedback.take(0).impact_mps;
            assert!((impact - 8.0).abs() < 0.01, "impact {impact}");
        }
    }

    #[test]
    fn steering_assist_gives_full_lock_slowly_and_less_at_speed() {
        let config = create_test_config();
        let full =
            |speed: f32, downforce: f32| assisted_steering(&config, 1.0, speed, downforce, 0.0);
        assert_eq!(full(0.0, 0.0), 1.0);
        assert_eq!(full(5.0, 0.0), 1.0);
        // Where full lock is allowed, the input maps straight through.
        assert_eq!(assisted_steering(&config, 0.5, 5.0, 0.0, 0.0), 0.5);
        let mut previous = 1.0;
        for speed in [20.0, 40.0, 60.0, 80.0] {
            let share = full(speed, 0.0);
            assert!(share < previous, "lock should shrink with speed");
            previous = share;
        }
        // Downforce is grip, and grip is lock worth having.
        assert!(full(80.0, 20000.0) > previous);
        // Centred is centred, whatever the car is doing.
        assert_eq!(assisted_steering(&config, 0.0, 60.0, 0.0, -0.2), 0.0);
    }

    /// Full input held for `seconds` at `speed`, then the car's sideslip at
    /// the rear axle (rad), lateral g, and the front tyres' slip angle (rad).
    fn hold_full_lock(assist: bool, speed: f32, seconds: f32) -> (f32, f32, f32) {
        let config = create_test_config();
        let track = TrackConfig {
            centerline: (0..2000)
                .map(|i| TrackPoint {
                    x: i as f32 * 4.0,
                    distance_from_start_m: i as f32 * 4.0,
                    width_left_m: 2000.0,
                    width_right_m: 2000.0,
                    ..Default::default()
                })
                .collect(),
            ..TrackConfig::default()
        };
        let mut state = create_test_car_state();
        state.vel_x = speed;
        state.speed_mps = speed;
        state.gear = 5;
        state.steering_assist = assist;
        for _ in 0..(seconds * 240.0) as u32 {
            let input = PlayerInputData {
                throttle: ((speed - state.speed_mps) * 0.5 + 0.3).clamp(0.0, 1.0),
                steering: 1.0,
                ..Default::default()
            };
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        }
        let v_long = state.vel_x * state.yaw_rad.cos() + state.vel_y * state.yaw_rad.sin();
        let v_lat = -state.vel_x * state.yaw_rad.sin() + state.vel_y * state.yaw_rad.cos();
        let rear = (v_lat + state.angular_vel_yaw * axle_positions(&config).1).atan2(v_long);
        (
            rear,
            state.g_forces.lateral_g,
            state.tires.front_left.slip_angle_rad,
        )
    }

    #[test]
    fn steering_assist_holds_the_limit_with_the_stick_at_the_stop() {
        // A pad stick held at the stop at 60 m/s. With the aid the car
        // corners hard with its tail in line and its front tyres near their
        // peak; the rack's full lock throws the fronts far past it, where
        // they only scrub.
        let peak = create_test_config().tire_config.optimal_slip_angle_rad;
        let (rear_slip, lateral_g, front_slip) = hold_full_lock(true, 60.0, 3.0);
        assert!(
            rear_slip.abs() < 0.15,
            "rear axle sliding at {rear_slip:.2} rad with the aid"
        );
        assert!(lateral_g > 0.7, "only {lateral_g:.2} g at the stop");
        assert!(
            front_slip.abs() < 1.5 * peak,
            "fronts at {front_slip:.2} rad with the aid, peak {peak:.2}"
        );

        let (_, _, front_slip) = hold_full_lock(false, 60.0, 3.0);
        assert!(
            front_slip.abs() > 2.5 * peak,
            "full rack lock left the fronts at {front_slip:.2} rad"
        );
    }

    #[test]
    fn steering_assist_lets_the_stick_catch_a_slide() {
        // The car points 0.15 rad left of where it is going: the tail is
        // out. Full opposite lock must reach past the direction the front
        // axle travels, or a slide at speed could never be caught; the other
        // way the lock stays at the grip limit.
        let config = create_test_config();
        let track = create_straight_test_track();
        let slide = |steering: f32| {
            let mut state = create_test_car_state();
            state.vel_x = 50.0;
            state.speed_mps = 50.0;
            state.yaw_rad = 0.15;
            state.gear = 5;
            state.steering_assist = true;
            let input = PlayerInputData {
                steering,
                ..Default::default()
            };
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            state.steering_input * config.max_steering_angle_rad
        };
        let grip_lock = grip_limit_lock_rad(&config, 50.0, 0.0)
            + STEERING_AID_SLIP_ALLOWANCE * config.tire_config.optimal_slip_angle_rad;
        let countersteer = slide(-1.0);
        assert!(
            countersteer < -0.15 && countersteer > -(0.15 + grip_lock + 0.01),
            "countersteer reached {countersteer:.3} rad"
        );
        let into = slide(1.0);
        assert!(
            into > 0.0 && into < grip_lock + 0.01,
            "into the slide reached {into:.3} rad"
        );
    }

    #[test]
    fn straight_line_braking_is_yaw_stable() {
        // Hard braking with a small sideways nudge must settle, not spin.
        // With the lateral load transfer signed the wrong way the loaded
        // inside wheels braked harder under ABS and a 0.05 m/s nudge became
        // a full spin within 1.5 s ("braking feels like ice").
        let mut state = create_test_car_state();
        state.vel_x = 50.0;
        state.vel_y = 0.05;
        state.speed_mps = 50.0;
        state.gear = 4;
        let config = create_test_config();
        let track = create_straight_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        let dt = 1.0 / 240.0;
        for _ in 0..480 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }
        let v_lat = -state.vel_x * state.yaw_rad.sin() + state.vel_y * state.yaw_rad.cos();
        assert!(
            state.yaw_rad.abs() < 0.02 && v_lat.abs() < 0.05,
            "car yawed {:.3} rad with {:.2} m/s lateral velocity under straight braking",
            state.yaw_rad,
            v_lat
        );
        assert!(
            state.g_forces.longitudinal_g < -0.9,
            "still braking hard at the end: {:.2} g",
            state.g_forces.longitudinal_g
        );
        // Load went to the outside: the nudge is leftward, the correction
        // accelerates right, so the LEFT wheels carry more than the right.
        assert!(state.weight_front_left_n >= state.weight_front_right_n - 1.0);
    }

    #[test]
    fn test_braking_without_abs_locks_wheels() {
        let mut state = create_test_car_state();
        state.vel_x = 40.0;
        state.speed_mps = 40.0;
        state.gear = 4;

        let mut config = create_test_config();
        config.abs_enabled = false;
        let track = create_straight_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };

        let dt = 1.0 / 240.0;
        for _ in 0..24 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }
        // Full pedal without ABS exceeds front traction -> locked fronts
        assert_eq!(
            state.tires.front_left.slip_ratio, -1.0,
            "front wheels should lock under full braking without ABS"
        );
        // A locked wheel stops rotating
        assert_eq!(state.wheel_angular_vel[0], 0.0);
    }

    #[test]
    fn test_partial_braking_stays_in_stable_region() {
        let mut state = create_test_car_state();
        state.vel_x = 40.0;
        state.speed_mps = 40.0;
        state.gear = 4;

        let mut config = create_test_config();
        config.abs_enabled = false;
        let track = create_straight_test_track();
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.2,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };

        let dt = 1.0 / 240.0;
        for _ in 0..24 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }
        let slip = state.tires.front_left.slip_ratio;
        assert!(
            slip < 0.0 && slip > -config.tire_config.optimal_slip_ratio,
            "light braking must stay below optimal slip, got {}",
            slip
        );
    }

    #[test]
    fn test_launch_is_not_idle_limited() {
        // From standstill at full throttle the clutch-slip launch model must
        // deliver strong acceleration (engine at launch RPM, traction-limited
        // rather than idle-torque-limited): 0-100 km/h in a plausible time
        // for a 300kW/1200kg car.
        let mut state = create_test_car_state();
        let config = create_test_config();
        let track = create_straight_test_track();

        let dt = 1.0 / 240.0;
        let mut ticks_to_100 = None;
        for tick in 0..240 * 10 {
            // Simple AI-style shift logic
            let gear = if state.engine_rpm > 7000.0 && state.gear < 6 {
                Some(state.gear + 1)
            } else {
                None
            };
            let input = PlayerInputData {
                throttle: 1.0,
                brake: 0.0,
                steering: 0.0,
                gear,
                clutch: Some(1.0),
                drs: false,
            };
            update_car_3d(&mut state, &config, &input, &track, dt);
            if state.speed_mps >= 27.8 {
                ticks_to_100 = Some(tick);
                break;
            }
        }
        let ticks = ticks_to_100.expect("car must reach 100 km/h within 10s");
        let secs = ticks as f32 / 240.0;
        assert!(
            secs < 7.0,
            "0-100 km/h should take well under 7s for this car, took {:.2}s",
            secs
        );
    }

    #[test]
    fn test_combined_slip_reduces_lateral_force_under_braking() {
        // Friction ellipse: heavy braking must consume grip that would
        // otherwise be available laterally.
        let tire = TireConfig::default();
        let braking = solve_wheel_forces(
            30.0,
            2.0,
            0.0,
            0.0,
            1.35,
            0.0,
            6000.0,
            0.33,
            3000.0,
            1.0,
            &tire,
            true,
            TractionControl::Low,
        );
        let coasting = solve_wheel_forces(
            30.0,
            2.0,
            0.0,
            0.0,
            1.35,
            0.0,
            0.0,
            0.33,
            3000.0,
            1.0,
            &tire,
            true,
            TractionControl::Low,
        );
        assert!(
            braking.fy.abs() < coasting.fy.abs() * 0.8,
            "lateral force under heavy braking ({:.0}N) must be well below \
             the free-rolling value ({:.0}N)",
            braking.fy,
            coasting.fy
        );
    }

    #[test]
    fn test_excess_drive_torque_causes_wheelspin() {
        let tire = TireConfig::default();
        // 5000 Nm on one wheel with 3000 N load: way past traction
        let spinning = solve_wheel_forces(
            5.0,
            0.0,
            0.0,
            0.0,
            -1.35,
            5000.0,
            0.0,
            0.33,
            3000.0,
            1.0,
            &tire,
            true,
            TractionControl::Off,
        );
        assert!(
            spinning.slip_ratio > 0.3,
            "massive torque oversupply should spin deep, got {}",
            spinning.slip_ratio
        );
        // Spun-up wheel rotates faster than free rolling
        assert!(spinning.omega > 5.0 / 0.33);
        // Force in the falloff region is below peak
        assert!(spinning.fx < 3000.0);
        assert!(spinning.fx > 0.0);
    }

    #[test]
    fn test_hybrid_assist_drains_battery_and_regen_charges() {
        let mut state = create_test_car_state();
        state.vel_x = 30.0;
        state.speed_mps = 30.0;
        state.gear = 3;

        let mut config = create_test_config();
        config.hybrid = HybridConfig {
            enabled: true,
            battery_capacity_kwh: 1.0,
            battery_max_discharge_kw: 120.0,
            battery_max_charge_kw: 100.0,
            motor_max_torque_nm: 200.0,
            motor_max_power_kw: 120.0,
            regen_max_power_kw: 100.0,
        };
        let track = create_straight_test_track();
        let dt = 1.0 / 240.0;

        let throttle_input = PlayerInputData {
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        for _ in 0..240 {
            update_car_3d(&mut state, &config, &throttle_input, &track, dt);
        }
        let after_assist = state.hybrid_battery_kwh;
        assert!(
            after_assist < 1.0,
            "assist must drain the battery, got {}",
            after_assist
        );

        let brake_input = PlayerInputData {
            throttle: 0.0,
            brake: 1.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };
        for _ in 0..120 {
            update_car_3d(&mut state, &config, &brake_input, &track, dt);
        }
        assert!(
            state.hybrid_battery_kwh > after_assist,
            "regen must charge the battery ({} -> {})",
            after_assist,
            state.hybrid_battery_kwh
        );
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
            drs: false,
        };

        let dt = 1.0 / 240.0;

        update_car_3d(&mut state, &config, &input, &track, dt);

        // Angular velocity should change with steering
        assert!(
            state.angular_vel_yaw.abs() > 0.0 || state.yaw_rad.abs() > 0.0001,
            "Steering should cause yaw change"
        );
    }

    #[test]
    fn test_collision_detection() {
        let config = create_test_config();
        let grid_slot1 = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let grid_slot2 = GridSlot {
            position: 2,
            x: 1.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };

        let mut states = vec![
            CarState::new(Uuid::new_v4(), config.id, &grid_slot1),
            CarState::new(Uuid::new_v4(), config.id, &grid_slot2),
        ];
        states[0].speed_mps = 10.0;
        states[1].speed_mps = 10.0;

        let mut configs = HashMap::new();
        configs.insert(config.id, config.clone());

        check_obb_collisions_3d(&mut states, &configs);

        assert!(states[0].is_colliding, "Car 1 should be colliding");
        assert!(states[1].is_colliding, "Car 2 should be colliding");
    }

    #[test]
    fn test_track_progress_update() {
        let track = create_test_track();
        let mut state = create_test_car_state();
        state.pos_x = track.centerline[1].x;
        state.pos_y = track.centerline[1].y;

        update_track_progress_3d(&mut state, &track, 0, 240);

        assert!(
            state.track_progress > 0.0,
            "Track progress should be positive"
        );
    }

    /// Move the car onto centerline point `idx` and update track progress.
    fn step_to_centerline_point(state: &mut CarState, track: &TrackConfig, idx: usize, tick: u32) {
        state.pos_x = track.centerline[idx].x;
        state.pos_y = track.centerline[idx].y;
        update_track_progress_3d(state, track, tick, 240);
    }

    #[test]
    fn test_lap_counts_when_driving_forward_through_all_checkpoints() {
        let track = create_test_track();
        let n = track.centerline.len();
        let mut state = create_test_car_state();

        let mut tick = 0;
        // Drive forward: one full loop plus a bit (lap 1 starts when
        // crossing 10% of track length, wrap ends it back at the line)
        step_to_centerline_point(&mut state, &track, 0, tick);
        for i in 1..=n {
            tick += 1;
            step_to_centerline_point(&mut state, &track, i % n, tick);
        }

        assert_eq!(
            state.current_lap, 2,
            "full forward lap through all checkpoints should count"
        );
        assert!(
            state.last_lap_time_ms.is_some(),
            "completed lap should record a lap time"
        );
    }

    #[test]
    fn test_backwards_across_finish_line_does_not_count_lap() {
        let track = create_test_track();
        let n = track.centerline.len();
        let mut state = create_test_car_state();

        // Car is mid-race on lap 1, sitting just past the start line
        let mut tick = 0;
        step_to_centerline_point(&mut state, &track, 1, tick);
        state.current_lap = 1;
        state.lap_start_tick = 0;

        // Reverse across the start/finish line and keep backing up
        for idx in [0, n - 1, n - 2, n - 3] {
            tick += 1;
            step_to_centerline_point(&mut state, &track, idx, tick);
        }
        // Now drive forward across the line again (line-crossing exploit)
        for idx in [n - 2, n - 1, 0, 1] {
            tick += 1;
            step_to_centerline_point(&mut state, &track, idx, tick);
        }

        assert_eq!(
            state.current_lap, 1,
            "crossing the finish line without hitting checkpoints must not count a lap"
        );
        assert!(
            state.last_lap_time_ms.is_none(),
            "no lap time should be recorded for an invalid lap"
        );

        // A subsequent full, honest lap must count again (checkpoint state
        // was reset at the rejected wrap)
        for i in 2..=(n + 1) {
            tick += 1;
            step_to_centerline_point(&mut state, &track, i % n, tick);
        }
        assert_eq!(
            state.current_lap, 2,
            "an honest full lap after the exploit attempt should count"
        );
    }

    #[test]
    fn test_teleport_past_checkpoint_invalidates_lap() {
        let track = create_test_track();
        let n = track.centerline.len();
        let track_length = track.centerline.last().unwrap().distance_from_start_m;
        let mut state = create_test_car_state();

        let mut tick = 0;
        step_to_centerline_point(&mut state, &track, 0, tick);
        state.current_lap = 1;
        state.lap_start_tick = 0;

        // Drive forward until just before the first virtual checkpoint (25%)
        let cp0 = track_length * 0.25;
        let mut idx = 0;
        while track.centerline[idx + 1].distance_from_start_m < cp0 - 20.0 {
            idx += 1;
            tick += 1;
            step_to_centerline_point(&mut state, &track, idx, tick);
        }

        // Teleport well past the checkpoint (> per-tick advance cap)
        let skip_to = idx + 6; // ~94m jump on the default oval (15.7m spacing)
        tick += 1;
        step_to_centerline_point(&mut state, &track, skip_to, tick);

        // Drive the rest of the lap normally
        for i in (skip_to + 1)..=n {
            tick += 1;
            step_to_centerline_point(&mut state, &track, i % n, tick);
        }

        assert_eq!(
            state.current_lap, 1,
            "lap with a skipped checkpoint (teleport) must not count"
        );
        assert!(state.last_lap_time_ms.is_none());
    }

    #[test]
    fn test_aerodynamic_forces() {
        let mut state = create_test_car_state();
        state.speed_mps = 50.0; // 180 km/h
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
        assert!(
            long_transfer < 0.0,
            "Weight should transfer forward under braking"
        );

        // Under acceleration
        let (long_transfer, _, _) = calculate_weight_transfer(&config, 5.0, 0.0, total_weight);
        assert!(
            long_transfer > 0.0,
            "Weight should transfer rearward under acceleration"
        );
    }

    #[test]
    fn test_tire_forces() {
        let tire_config = TireConfig::default();
        let load = 3000.0; // 3000N wheel load

        // Drive torque within traction: the tire transmits the request
        let driving = solve_wheel_forces(
            20.0,
            0.0,
            0.0,
            0.0,
            -1.35,
            300.0,
            0.0,
            0.33,
            load,
            1.0,
            &tire_config,
            true,
            TractionControl::Low,
        );
        assert!(
            (driving.fx - 300.0 / 0.33).abs() < 1.0,
            "stable-region tire must transmit the requested force, got {}",
            driving.fx
        );
        assert!(driving.slip_ratio > 0.0);
        assert!(
            driving.fy.abs() < 1.0,
            "no lateral force when tracking straight"
        );

        // Pure cornering: slip angle produces restoring lateral force
        let cornering = solve_wheel_forces(
            20.0,
            2.0,
            0.0,
            0.0,
            1.35,
            0.0,
            0.0,
            0.33,
            load,
            1.0,
            &tire_config,
            true,
            TractionControl::Low,
        );
        assert!(
            cornering.fx.abs() < 1.0,
            "no longitudinal force when coasting"
        );
        assert!(
            cornering.fy < 0.0,
            "positive slip angle must produce restoring (negative) lateral force"
        );
    }

    #[test]
    fn test_ackermann_steering() {
        let wheelbase = 2.7;
        let track_width = 1.6;

        // Positive steering is a LEFT turn (+y is left): the left wheel is
        // inner and turns further.
        let (left, right) = calculate_ackermann_steering(0.3, wheelbase, track_width);
        assert!(left > 0.0 && right > 0.0, "Both wheels should turn left");
        assert!(left > right, "Inner wheel (left) should turn more");

        // And a right turn mirrors it.
        let (left_r, right_r) = calculate_ackermann_steering(-0.3, wheelbase, track_width);
        assert!(
            left_r < 0.0 && right_r < 0.0,
            "Both wheels should turn right"
        );
        assert!(right_r < left_r, "Inner wheel (right) should turn more");
        assert!((left_r + right).abs() < 1e-6 && (right_r + left).abs() < 1e-6);

        // Test straight
        let (left, right) = calculate_ackermann_steering(0.0, wheelbase, track_width);
        assert!(
            left.abs() < 0.001 && right.abs() < 0.001,
            "No steering angle"
        );
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
            drs: false,
        };

        // Run several ticks
        for _ in 0..100 {
            update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
        }

        // Car should have moved and adopted track elevation
        assert!(
            state.pos_x != 0.0 || state.pos_y != 0.0,
            "Car should have moved"
        );
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
            content_crc: 0,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            raceline_distances: Vec::new(),
            drs_zones: Vec::new(),
            checkpoints: Vec::new(),
            sectors: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: None,
            ground: None,
            curbs: None,
            walls: None,
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
            drs: false,
        };

        let dt = 1.0 / 240.0;

        update_car_3d(&mut state, &config, &input, &track, dt);

        assert!(
            state.is_airborne,
            "Car should be airborne when above track surface"
        );
        assert!(state.vel_z < 0.0, "Gravity should accelerate car downward");
        assert_eq!(
            state.weight_front_left_n, 0.0,
            "Airborne car should have no tire load"
        );

        for _ in 0..400 {
            update_car_3d(&mut state, &config, &input, &track, dt);
        }

        assert!(!state.is_airborne, "Car should land back on track surface");
        assert!(
            state.pos_z.abs() < 0.2,
            "Car should settle near track elevation"
        );
        assert_eq!(
            state.vel_z, 0.0,
            "Vertical velocity should reset on landing"
        );
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
            content_crc: 0,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            raceline_distances: Vec::new(),
            drs_zones: Vec::new(),
            checkpoints: Vec::new(),
            sectors: Vec::new(),
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
            ground: None,
            curbs: None,
            walls: None,
        };

        let centerline_sample = query_track_surface_centerline(&track, 0.5, 0.5, None)
            .expect("centerline sample should exist");
        let mesh_sample = query_track_surface_mesh_heightfield_stub(&track, 0.5, 0.5, None)
            .expect("mesh stub sample should exist");

        assert_eq!(
            centerline_sample.elevation, 1.0,
            "centerline sample should use centerline z"
        );
        assert!(
            (mesh_sample.elevation - 5.0).abs() < 0.001,
            "mesh stub should use heightfield elevation"
        );

        assert_eq!(mesh_sample.nearest_point, centerline_sample.nearest_point);
        assert!((mesh_sample.banking_rad - centerline_sample.banking_rad).abs() < 0.0001);
        assert!((mesh_sample.slope_rad - centerline_sample.slope_rad).abs() < 0.0001);
        assert!((mesh_sample.heading_rad - centerline_sample.heading_rad).abs() < 0.0001);
        assert!((mesh_sample.lateral_offset - centerline_sample.lateral_offset).abs() < 0.0001);
        assert_eq!(mesh_sample.surface_type, centerline_sample.surface_type);
        assert!((mesh_sample.grip_modifier - centerline_sample.grip_modifier).abs() < 0.0001);
    }

    /// Ground rising from `left_z` at y = +1 to `right_z` at y = -1: for a
    /// car at the origin heading +x, its left side (+y) sits on `left_z`.
    fn create_split_height_track(left_z: f32, right_z: f32) -> TrackConfig {
        TrackConfig {
            id: Uuid::new_v4(),
            name: "Split Height Test".to_string(),
            centerline: vec![
                TrackPoint {
                    x: 0.0,
                    y: 1.0,
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
                    y: -1.0,
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
            content_crc: 0,
            start_positions: Vec::new(),
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            raceline_distances: Vec::new(),
            drs_zones: Vec::new(),
            checkpoints: Vec::new(),
            sectors: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: None,
            ground: None,
            curbs: None,
            walls: None,
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
            drs: false,
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

    /// Ground higher under the car's right side compresses the right wheels
    /// more. That side carries the extra spring load, and an anti-roll bar,
    /// twisted by the difference, loads it further still. (Before the wheel
    /// samplers were put on their real sides, the spring load went to the
    /// wrong wheel and this test asserted the bar evened the axle out.)
    #[test]
    fn test_anti_roll_bar_loads_the_more_compressed_wheel() {
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

        let track = create_split_height_track(0.0, 0.03);
        let input = PlayerInputData {
            throttle: 0.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: false,
        };

        // A second to settle: on the first tick the suspension travel jumps
        // from nothing, and the damper spike swamps everything else.
        let dt = 1.0 / 240.0;
        for _ in 0..240 {
            update_car_3d(&mut state_no_arb, &config_no_arb, &input, &track, dt);
            update_car_3d(&mut state_with_arb, &config_with_arb, &input, &track, dt);
        }

        // Right minus left: positive when the higher (right) side is loaded.
        let front_split_no_arb =
            state_no_arb.weight_front_right_n - state_no_arb.weight_front_left_n;
        let front_split_with_arb =
            state_with_arb.weight_front_right_n - state_with_arb.weight_front_left_n;

        assert!(
            front_split_no_arb > 0.0,
            "the more compressed right front should carry more load: split {front_split_no_arb:.0} N"
        );
        assert!(
            front_split_with_arb > front_split_no_arb,
            "the bar should load the compressed wheel further: {front_split_with_arb:.0} N with, \
             {front_split_no_arb:.0} N without"
        );
        let axle = |s: &CarState| s.weight_front_left_n + s.weight_front_right_n;
        assert!(
            (axle(&state_with_arb) - axle(&state_no_arb)).abs() < 1.0,
            "the bar moves load across the axle, it does not add any"
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
            drs: false,
        };

        let initial_fuel = state.fuel_liters;
        update_car_3d(&mut state, &config, &input, &track, 1.0);

        assert!(state.fuel_liters < initial_fuel, "Fuel should be consumed");
        assert!(
            state.fuel_consumption_lps > 0.0,
            "Fuel consumption rate should be positive"
        );
    }

    #[test]
    fn test_damage_system() {
        let config = create_test_config();
        let grid_slot1 = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        // First-contact geometry: barely overlapping along the approach
        // axis, as produced by incremental per-tick motion (a deep instant
        // overlap would make the minimum-translation axis lateral, where
        // the closing speed is zero).
        let grid_slot2 = GridSlot {
            position: 2,
            x: config.length_m - 0.1,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };

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

        check_obb_collisions_3d(&mut states, &configs);

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
            drs: false,
        };

        // Test that legacy API still works
        update_car_2d(&mut state, &config, &input, 1.0 / 240.0);

        assert!(state.speed_mps > 0.0, "Legacy API should work");
    }

    /// Low traction control holds the wheel at its peak; the friction
    /// ellipse then takes the lateral force down to fit. High leaves the
    /// lateral force alone and trims the drive instead.
    #[test]
    fn high_traction_control_keeps_the_lateral_force_low_spends_it() {
        let tire = TireConfig::default();
        let load = 3000.0;
        let run = |tc: TractionControl, drive_torque: f32| {
            solve_wheel_forces(
                20.0,
                2.0,
                0.0,
                0.0,
                -1.35,
                drive_torque,
                0.0,
                0.33,
                load,
                1.0,
                &tire,
                true,
                tc,
            )
        };
        let coasting = run(TractionControl::Off, 0.0);
        let low = run(TractionControl::Low, 5000.0);
        let high = run(TractionControl::High, 5000.0);
        let d = load;

        assert!(low.tc_active && high.tc_active);
        assert!(
            (low.fy.abs() - coasting.fy.abs()) < -0.1 * d,
            "low TC takes the whole circle and the ellipse scales the lateral force: {} vs coasting {}",
            low.fy,
            coasting.fy
        );
        assert!(
            (high.fy - coasting.fy).abs() < 1.0,
            "high TC keeps the cornering force: {} vs coasting {}",
            high.fy,
            coasting.fy
        );
        assert!(
            high.fx > 0.0 && high.fx < low.fx,
            "and spends the rest on drive"
        );
        let usage = ((high.fx / d).powi(2) + (high.fy / d).powi(2)).sqrt();
        assert!(usage < 1.0, "with a margin left: usage {usage}");

        // Off, the same torque spins the wheel into the falloff.
        let off = run(TractionControl::Off, 5000.0);
        assert!(!off.tc_active);
        assert!(off.slip_ratio > tire.optimal_slip_ratio * 1.2);
    }

    /// The driver's aids override the car's file: ABS off for this player
    /// locks the wheels on a car whose file has it on, and traction control
    /// from the player wins the same way. An unset aid is the file's.
    #[test]
    fn a_drivers_aids_override_the_cars_file() {
        let track = create_straight_test_track();
        let config = create_test_config();
        assert!(config.abs_enabled && config.traction_control_enabled);

        let brake = |abs: Option<bool>| {
            let mut state = create_test_car_state();
            state.pos_x = 100.0;
            state.vel_x = 40.0;
            state.speed_mps = 40.0;
            state.gear = 4;
            state.abs = abs;
            let input = PlayerInputData {
                brake: 1.0,
                ..Default::default()
            };
            for _ in 0..24 {
                update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            }
            state.feedback.take(0)
        };
        assert!(brake(None).abs_active, "unset: the file's ABS");
        assert!(brake(Some(true)).abs_active);
        let locked = brake(Some(false));
        assert!(!locked.abs_active);
        assert!(
            locked.slip_ratio.iter().any(|s| *s < -1.5),
            "ABS off for this driver locks a wheel: {:?}",
            locked.slip_ratio
        );

        let launch = |tc: Option<TractionControl>| {
            let mut state = create_test_car_state();
            state.pos_x = 100.0;
            state.vel_x = 5.0;
            state.speed_mps = 5.0;
            state.gear = 1;
            state.traction_control = tc;
            let input = PlayerInputData {
                throttle: 1.0,
                ..Default::default()
            };
            for _ in 0..24 {
                update_car_3d(&mut state, &config, &input, &track, 1.0 / 240.0);
            }
            state.feedback.take(0)
        };
        assert!(launch(None).tc_active, "unset: the file's traction control");
        assert!(launch(Some(TractionControl::High)).tc_active);
        assert!(!launch(Some(TractionControl::Off)).tc_active);
    }
}
