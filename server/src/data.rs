use serde::{Deserialize, Serialize};
use serde_repr::{Deserialize_repr, Serialize_repr};
use std::collections::HashMap;
use uuid::Uuid;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = Uuid;

// --- Player State ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Player {
    pub id: PlayerId,
    pub name: String,
    pub connection_id: ConnectionId,
    pub selected_car_config_id: Option<CarConfigId>,
    pub is_ai: bool,
}

// --- Car Configuration (Static / Moddable) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarConfig {
    pub id: CarConfigId,
    pub name: String,
    pub model: String,

    // Physical dimensions
    pub mass_kg: f32,
    pub length_m: f32,
    pub width_m: f32,
    pub height_m: f32,
    pub wheelbase_m: f32,
    pub track_width_front_m: f32,  // Distance between front wheels
    pub track_width_rear_m: f32,   // Distance between rear wheels
    pub wheel_radius_m: f32,
    
    // Center of gravity (relative to geometric center)
    pub cog_height_m: f32,          // Height of center of gravity
    pub cog_offset_x_m: f32,        // Forward (+) / backward (-) from center
    pub weight_distribution_front: f32, // 0.0-1.0, percentage of weight on front axle
    
    // Engine & drivetrain
    pub max_engine_power_w: f32,    // Peak engine power in Watts
    pub max_engine_torque_nm: f32,  // Peak engine torque in Nm
    pub max_engine_rpm: f32,
    pub idle_rpm: f32,
    pub redline_rpm: f32,
    pub gear_ratios: Vec<f32>,      // Gear ratios (including reverse as negative)
    pub final_drive_ratio: f32,
    pub drivetrain: Drivetrain,     // FWD, RWD, AWD
    
    // Braking
    pub max_brake_force_n: f32,
    pub brake_bias_front: f32,      // 0.0-1.0, percentage of braking on front
    pub abs_enabled: bool,
    
    // Aerodynamics
    pub drag_coefficient: f32,
    pub frontal_area_m2: f32,
    pub lift_coefficient_front: f32, // Negative = downforce
    pub lift_coefficient_rear: f32,
    
    // Steering
    pub max_steering_angle_rad: f32,
    pub steering_ratio: f32,        // Steering wheel turns : wheel angle
    
    // Suspension
    pub suspension: SuspensionConfig,
    
    // Tires
    pub tire_config: TireConfig,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum Drivetrain {
    FWD,  // Front-wheel drive
    RWD,  // Rear-wheel drive
    AWD,  // All-wheel drive
}

impl Default for Drivetrain {
    fn default() -> Self {
        Drivetrain::RWD
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SuspensionConfig {
    pub spring_rate_front_n_per_m: f32,
    pub spring_rate_rear_n_per_m: f32,
    pub damper_compression_front: f32,
    pub damper_compression_rear: f32,
    pub damper_rebound_front: f32,
    pub damper_rebound_rear: f32,
    pub anti_roll_bar_front: f32,
    pub anti_roll_bar_rear: f32,
    pub max_travel_m: f32,
}

impl Default for SuspensionConfig {
    fn default() -> Self {
        Self {
            spring_rate_front_n_per_m: 80000.0,
            spring_rate_rear_n_per_m: 70000.0,
            damper_compression_front: 3000.0,
            damper_compression_rear: 2800.0,
            damper_rebound_front: 4500.0,
            damper_rebound_rear: 4200.0,
            anti_roll_bar_front: 15000.0,
            anti_roll_bar_rear: 12000.0,
            max_travel_m: 0.15,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TireConfig {
    pub grip_coefficient: f32,           // Base grip coefficient (0.8-1.2)
    pub optimal_slip_ratio: f32,         // Peak longitudinal slip (typically 0.06-0.12)
    pub optimal_slip_angle_rad: f32,     // Peak lateral slip angle (typically 6-10 degrees)
    pub rolling_resistance: f32,         // Rolling resistance coefficient
    pub optimal_temperature_c: f32,      // Optimal tire temp for best grip
    pub temperature_grip_falloff: f32,   // Grip reduction per degree from optimal
    pub wear_rate: f32,                  // Wear rate multiplier
}

impl Default for TireConfig {
    fn default() -> Self {
        Self {
            grip_coefficient: 1.0,
            optimal_slip_ratio: 0.08,
            optimal_slip_angle_rad: 0.12,  // ~7 degrees
            rolling_resistance: 0.015,
            optimal_temperature_c: 90.0,
            temperature_grip_falloff: 0.005,
            wear_rate: 1.0,
        }
    }
}

impl Default for CarConfig {
    fn default() -> Self {
        Self {
            id: Uuid::new_v4(),
            name: "Default Car".to_string(),
            model: "default.glb".to_string(),

            // Physical dimensions
            mass_kg: 1200.0,
            length_m: 4.5,
            width_m: 1.9,
            height_m: 1.3,
            wheelbase_m: 2.7,
            track_width_front_m: 1.6,
            track_width_rear_m: 1.58,
            wheel_radius_m: 0.33,
            
            // Center of gravity
            cog_height_m: 0.45,
            cog_offset_x_m: 0.0,
            weight_distribution_front: 0.52,
            
            // Engine & drivetrain
            max_engine_power_w: 300000.0,  // 300 kW (~400 HP)
            max_engine_torque_nm: 450.0,
            max_engine_rpm: 8000.0,
            idle_rpm: 900.0,
            redline_rpm: 7500.0,
            gear_ratios: vec![-3.5, 3.8, 2.4, 1.7, 1.3, 1.0, 0.8],  // R, 1-6
            final_drive_ratio: 3.7,
            drivetrain: Drivetrain::RWD,
            
            // Braking
            max_brake_force_n: 25000.0,
            brake_bias_front: 0.6,
            abs_enabled: true,
            
            // Aerodynamics
            drag_coefficient: 0.32,
            frontal_area_m2: 2.2,
            lift_coefficient_front: -0.15,  // Slight downforce
            lift_coefficient_rear: -0.20,
            
            // Steering
            max_steering_angle_rad: 0.52,  // ~30 degrees
            steering_ratio: 14.0,
            
            // Suspension
            suspension: SuspensionConfig::default(),
            
            // Tires
            tire_config: TireConfig::default(),
        }
    }
}

// --- Track Configuration (Static / Moddable) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackConfig {
    pub id: TrackConfigId,
    pub name: String,
    pub centerline: Vec<TrackPoint>,
    pub width_m: f32,
    /// Path to the source track file, relative to the content folder (e.g. "tracks/real/Austin.yaml")
    #[serde(default)]
    pub source_path: Option<String>,
    pub start_positions: Vec<GridSlot>,
    pub track_surface: TrackSurface,
    pub pit_lane: Option<PitLaneConfig>,
    /// Optional optimal racing line for AI and visualization
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    /// Track metadata
    #[serde(default)]
    pub metadata: TrackMetadata,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RacelinePoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrackMetadata {
    pub country: Option<String>,
    pub city: Option<String>,
    pub length_m: Option<f32>,
    pub description: Option<String>,
    pub year_built: Option<u32>,
    pub category: Option<String>,  // e.g., \"F1\", \"DTM\", \"IndyCar\"
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackSurface {
    pub base_grip: f32,              // Base grip multiplier (1.0 = normal asphalt)
    pub curb_grip: f32,              // Grip on curbs
    pub off_track_grip: f32,         // Grip off track (grass/gravel)
    pub off_track_speed_penalty: f32, // Speed reduction factor off track
}

impl Default for TrackSurface {
    fn default() -> Self {
        Self {
            base_grip: 1.0,
            curb_grip: 0.85,
            off_track_grip: 0.4,
            off_track_speed_penalty: 0.15,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PitLaneConfig {
    pub entry_point: TrackPoint,
    pub exit_point: TrackPoint,
    pub speed_limit_mps: f32,
    pub pit_stalls: Vec<PitStall>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PitStall {
    pub position: u8,
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Default for TrackConfig {
    fn default() -> Self {
        // Create a simple oval track with elevation changes
        let mut centerline = Vec::new();
        let num_points = 40;
        let radius = 100.0;
        
        for i in 0..num_points {
            let angle = 2.0 * std::f32::consts::PI * (i as f32) / (num_points as f32);
            let x = radius * angle.cos();
            let y = radius * angle.sin();
            let distance = angle * radius;
            
            // Add some elevation variation
            let z = (angle * 2.0).sin() * 3.0;  // ±3m elevation
            
            // Calculate banking based on turn (more banking in turns)
            let banking = if x.abs() < radius * 0.3 { 
                0.12 * (1.0 - x.abs() / (radius * 0.3))  // ~7 degrees max
            } else { 
                0.0 
            };
            
            // Calculate track direction for camber
            let next_i = (i + 1) % num_points;
            let next_angle = 2.0 * std::f32::consts::PI * (next_i as f32) / (num_points as f32);
            let dx = (radius * next_angle.cos()) - x;
            let dy = (radius * next_angle.sin()) - y;
            let heading = dy.atan2(dx);
            
            // Calculate slope (grade) based on elevation change
            let prev_i = (i + num_points - 1) % num_points;
            let prev_angle = 2.0 * std::f32::consts::PI * (prev_i as f32) / (num_points as f32);
            let prev_z = (prev_angle * 2.0).sin() * 3.0;
            let segment_length = 2.0 * std::f32::consts::PI * radius / num_points as f32;
            let slope = (z - prev_z) / segment_length;
            
            centerline.push(TrackPoint {
                x,
                y,
                z,
                distance_from_start_m: distance,
                width_left_m: 7.5,
                width_right_m: 7.5,
                banking_rad: banking,
                camber_rad: 0.0,
                slope_rad: slope.atan(),
                heading_rad: heading,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            });
        }
        
        // Create start positions
        let mut start_positions = Vec::new();
        for i in 0..16 {
            start_positions.push(GridSlot {
                position: (i + 1) as u8,
                x: radius - (i / 2) as f32 * 8.0,
                y: if i % 2 == 0 { -2.0 } else { 2.0 },
                z: 0.0,
                yaw_rad: 0.0,
            });
        }
        
        Self {
            id: Uuid::new_v4(),
            name: "Default Oval".to_string(),
            centerline,
            width_m: 15.0,
            source_path: None,
            start_positions,
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            metadata: TrackMetadata::default(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SurfaceType {
    Asphalt,
    Concrete,
    Curb,
    Grass,
    Gravel,
    Sand,
    Wet,
}

impl Default for SurfaceType {
    fn default() -> Self {
        SurfaceType::Asphalt
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,                      // Elevation
    pub distance_from_start_m: f32,
    pub width_left_m: f32,           // Track width to the left of centerline
    pub width_right_m: f32,          // Track width to the right of centerline
    pub banking_rad: f32,            // Track banking angle (positive = banked towards inside)
    pub camber_rad: f32,             // Cross-slope (crown)
    pub slope_rad: f32,              // Uphill/downhill grade
    pub heading_rad: f32,            // Track direction at this point
    pub surface_type: SurfaceType,
    pub grip_modifier: f32,          // Local grip adjustment (1.0 = normal)
}

impl Default for TrackPoint {
    fn default() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            distance_from_start_m: 0.0,
            width_left_m: 7.5,
            width_right_m: 7.5,
            banking_rad: 0.0,
            camber_rad: 0.0,
            slope_rad: 0.0,
            heading_rad: 0.0,
            surface_type: SurfaceType::Asphalt,
            grip_modifier: 1.0,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GridSlot {
    pub position: u8,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
}

// --- Telemetry Data Structures ---
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct TireData {
    pub temperature_c: f32,
    pub pressure_kpa: f32,
    pub wear_percent: f32,
    pub slip_ratio: f32,
    pub slip_angle_rad: f32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct TireTelemetry {
    pub front_left: TireData,
    pub front_right: TireData,
    pub rear_left: TireData,
    pub rear_right: TireData,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct GForces {
    pub lateral_g: f32,      // Side-to-side (left negative, right positive)
    pub longitudinal_g: f32, // Forward/backward (braking negative, acceleration positive)
    pub vertical_g: f32,     // Up/down (compression positive)
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct SuspensionTelemetry {
    pub front_left_travel_m: f32,
    pub front_right_travel_m: f32,
    pub rear_left_travel_m: f32,
    pub rear_right_travel_m: f32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct DamageState {
    pub front_damage_percent: f32,
    pub rear_damage_percent: f32,
    pub left_damage_percent: f32,
    pub right_damage_percent: f32,
    pub engine_damage_percent: f32,
    pub is_drivable: bool,
}

// --- Car Dynamics State (Per-Tick, Server Authoritative) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarState {
    pub player_id: PlayerId,
    pub car_config_id: CarConfigId,
    pub grid_position: u8,
    
    // 3D Position
    pub pos_x: f32,
    pub pos_y: f32,
    pub pos_z: f32,                   // Elevation
    
    // 3D Orientation (Euler angles)
    pub yaw_rad: f32,                 // Heading (rotation around Z axis)
    pub pitch_rad: f32,               // Nose up/down (rotation around Y axis)
    pub roll_rad: f32,                // Body roll (rotation around X axis)
    
    // 3D Velocity
    pub vel_x: f32,
    pub vel_y: f32,
    pub vel_z: f32,
    pub speed_mps: f32,               // Magnitude of velocity vector
    
    // Angular velocities
    pub angular_vel_yaw: f32,         // Yaw rate (rad/s)
    pub angular_vel_pitch: f32,       // Pitch rate (rad/s)
    pub angular_vel_roll: f32,        // Roll rate (rad/s)
    
    // Inputs
    pub throttle_input: f32,
    pub brake_input: f32,
    pub steering_input: f32,
    pub gear: i8,                     // Current gear (-1 = reverse, 0 = neutral, 1-6+)
    pub clutch_input: f32,            // Clutch engagement (0 = disengaged, 1 = engaged)
    
    // Track position
    pub track_progress: f32,
    pub lateral_offset_m: f32,        // Distance from centerline (positive = right)
    pub current_lap: u16,
    pub finish_position: Option<u8>,
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,
    
    // Collision state
    pub is_colliding: bool,
    pub collision_normal_x: f32,
    pub collision_normal_y: f32,
    pub collision_normal_z: f32,
    
    // Surface state
    pub current_surface: SurfaceType,
    pub is_on_track: bool,
    pub surface_grip_modifier: f32,
    
    // Telemetry
    pub tires: TireTelemetry,
    pub g_forces: GForces,
    pub suspension: SuspensionTelemetry,
    pub fuel_liters: f32,
    pub fuel_capacity_liters: f32,
    pub fuel_consumption_lps: f32,
    pub damage: DamageState,
    pub engine_rpm: f32,
    pub engine_temp_c: f32,
    pub oil_temp_c: f32,
    pub oil_pressure_kpa: f32,
    pub water_temp_c: f32,
    
    // Weight transfer
    pub weight_front_left_n: f32,
    pub weight_front_right_n: f32,
    pub weight_rear_left_n: f32,
    pub weight_rear_right_n: f32,
    
    // Aerodynamics
    pub downforce_front_n: f32,
    pub downforce_rear_n: f32,
    pub drag_force_n: f32,
}

impl CarState {
    pub fn new(player_id: PlayerId, car_config_id: CarConfigId, grid_slot: &GridSlot) -> Self {
        Self {
            player_id,
            car_config_id,
            grid_position: grid_slot.position,
            
            // 3D Position
            pos_x: grid_slot.x,
            pos_y: grid_slot.y,
            pos_z: grid_slot.z,
            
            // Orientation
            yaw_rad: grid_slot.yaw_rad,
            pitch_rad: 0.0,
            roll_rad: 0.0,
            
            // Velocity
            vel_x: 0.0,
            vel_y: 0.0,
            vel_z: 0.0,
            speed_mps: 0.0,
            
            // Angular velocity
            angular_vel_yaw: 0.0,
            angular_vel_pitch: 0.0,
            angular_vel_roll: 0.0,
            
            // Inputs
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            gear: 1,
            clutch_input: 1.0,
            
            // Track position
            track_progress: 0.0,
            lateral_offset_m: 0.0,
            current_lap: 0,
            finish_position: None,
            last_lap_time_ms: None,
            best_lap_time_ms: None,
            
            // Collision
            is_colliding: false,
            collision_normal_x: 0.0,
            collision_normal_y: 0.0,
            collision_normal_z: 0.0,
            
            // Surface
            current_surface: SurfaceType::Asphalt,
            is_on_track: true,
            surface_grip_modifier: 1.0,
            
            // Telemetry
            tires: TireTelemetry::default(),
            g_forces: GForces::default(),
            suspension: SuspensionTelemetry::default(),
            fuel_liters: 100.0,
            fuel_capacity_liters: 100.0,
            fuel_consumption_lps: 0.0,
            damage: DamageState {
                is_drivable: true,
                ..Default::default()
            },
            engine_rpm: 900.0,
            engine_temp_c: 85.0,
            oil_temp_c: 90.0,
            oil_pressure_kpa: 350.0,
            water_temp_c: 80.0,
            
            // Weight (will be calculated)
            weight_front_left_n: 0.0,
            weight_front_right_n: 0.0,
            weight_rear_left_n: 0.0,
            weight_rear_right_n: 0.0,
            
            // Aerodynamics (will be calculated)
            downforce_front_n: 0.0,
            downforce_rear_n: 0.0,
            drag_force_n: 0.0,
        }
    }
}

// --- Race Session State (Server Authoritative) ---
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr)]
pub enum SessionState {
    Lobby = 0,
    Countdown = 1,
    Racing = 2,
    Finished = 3,
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr)]
pub enum SessionKind {
    Multiplayer = 0,
    Practice = 1,
    Sandbox = 2,
}

impl Default for SessionKind {
    fn default() -> Self {
        SessionKind::Multiplayer
    }
}

/// Game modes determine the behavior and rules during a session
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr)]
pub enum GameMode {
    /// Lobby state, no telemetry sent, players selecting cars
    Lobby = 0,
    /// Nothing moves, no telemetry, camera exploration only
    Sandbox = 1,
    /// Pre-race countdown, players frozen in pit lane
    Countdown = 2,
    /// Server drives a demo car along the racing line
    DemoLap = 3,
    /// Players drive freely, optional lap timing
    FreePractice = 4,
    /// Playback recorded telemetry (view-only)
    Replay = 5,
    /// Qualification mode (to be implemented)
    Qualification = 6,
    /// Race mode (to be implemented)
    Race = 7,
}

impl Default for GameMode {
    fn default() -> Self {
        GameMode::Lobby
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RaceSession {
    pub id: SessionId,
    pub track_config_id: TrackConfigId,
    pub host_player_id: PlayerId,
    #[serde(default)]
    pub session_kind: SessionKind,
    pub state: SessionState,
    #[serde(default)]
    pub game_mode: GameMode,
    pub participants: HashMap<PlayerId, CarState>,
    pub max_players: u8,
    pub ai_count: u8,
    pub lap_limit: u8,
    pub current_tick: u32,
    pub countdown_ticks_remaining: Option<u16>,
    pub race_start_tick: Option<u32>,
    /// AI driver player IDs (references to profiles stored elsewhere)
    pub ai_player_ids: Vec<PlayerId>,
    /// Demo lap state (used in DemoLap mode)
    pub demo_lap_progress: Option<f32>,
}

impl RaceSession {
    pub fn new(
        host_player_id: PlayerId,
        track_config_id: TrackConfigId,
        session_kind: SessionKind,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    ) -> Self {
        Self {
            id: Uuid::new_v4(),
            track_config_id,
            host_player_id,
            session_kind,
            state: SessionState::Lobby,
            game_mode: GameMode::Lobby,
            participants: HashMap::new(),
            max_players,
            ai_count,
            lap_limit,
            current_tick: 0,
            countdown_ticks_remaining: None,
            race_start_tick: None,
            ai_player_ids: Vec::new(),
            demo_lap_progress: None,
        }
    }
}

// --- Input Data ---
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct PlayerInputData {
    pub throttle: f32,
    pub brake: f32,
    pub steering: f32,
}

impl PlayerInputData {
    pub fn clamp(&mut self) {
        self.throttle = self.throttle.clamp(0.0, 1.0);
        self.brake = self.brake.clamp(0.0, 1.0);
        self.steering = self.steering.clamp(-1.0, 1.0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_player_creation() {
        let player = Player {
            id: Uuid::new_v4(),
            name: "TestPlayer".to_string(),
            connection_id: Uuid::new_v4(),
            selected_car_config_id: None,
            is_ai: false,
        };
        
        assert_eq!(player.name, "TestPlayer");
        assert!(!player.is_ai);
    }

    #[test]
    fn test_car_config_default() {
        let car = CarConfig::default();
        assert_eq!(car.name, "Default Car");
        assert!(car.mass_kg > 0.0);
    }

    #[test]
    fn test_track_config_default() {
        let track = TrackConfig::default();
        assert_eq!(track.name, "Default Oval");
        assert!(!track.centerline.is_empty());
        assert!(!track.start_positions.is_empty());
    }

    #[test]
    fn test_race_session_creation() {
        let host_id = Uuid::new_v4();
        let track_id = Uuid::new_v4();
        let session = RaceSession::new(host_id, track_id, SessionKind::Multiplayer, 8, 2, 5);
        
        assert_eq!(session.host_player_id, host_id);
        assert_eq!(session.track_config_id, track_id);
        assert_eq!(session.max_players, 8);
        assert_eq!(session.ai_count, 2);
        assert_eq!(session.lap_limit, 5);
        assert_eq!(session.state, SessionState::Lobby);
    }

    #[test]
    fn test_player_input_clamp() {
        let mut input = PlayerInputData {
            throttle: 1.5,
            brake: -0.5,
            steering: 2.0,
        };
        
        input.clamp();
        
        assert_eq!(input.throttle, 1.0);
        assert_eq!(input.brake, 0.0);
        assert_eq!(input.steering, 1.0);
    }

    #[test]
    fn test_car_state_creation() {
        let player_id = Uuid::new_v4();
        let car_id = Uuid::new_v4();
        let grid_slot = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        
        let state = CarState::new(player_id, car_id, &grid_slot);
        
        assert_eq!(state.player_id, player_id);
        assert_eq!(state.car_config_id, car_id);
        assert_eq!(state.grid_position, 1);
        assert_eq!(state.speed_mps, 0.0);
        assert_eq!(state.current_lap, 0);
    }
}
