use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use uuid::Uuid;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = u64;

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
    pub mass_kg: f32,
    pub length_m: f32,
    pub width_m: f32,
    pub max_engine_force_n: f32,
    pub max_brake_force_n: f32,
    pub drag_coefficient: f32,
    pub grip_coefficient: f32,
    pub max_steering_angle_rad: f32,
    pub wheelbase_m: f32,
}

impl Default for CarConfig {
    fn default() -> Self {
        Self {
            id: Uuid::new_v4(),
            name: "Default Car".to_string(),
            mass_kg: 1000.0,
            length_m: 4.0,
            width_m: 2.0,
            max_engine_force_n: 8000.0,
            max_brake_force_n: 15000.0,
            drag_coefficient: 0.35,
            grip_coefficient: 1.0,
            max_steering_angle_rad: 0.5,
            wheelbase_m: 2.5,
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
    pub start_positions: Vec<GridSlot>,
}

impl Default for TrackConfig {
    fn default() -> Self {
        // Create a simple oval track
        let mut centerline = Vec::new();
        let num_points = 20;
        let radius = 100.0;
        
        for i in 0..num_points {
            let angle = 2.0 * std::f32::consts::PI * (i as f32) / (num_points as f32);
            let x = radius * angle.cos();
            let y = radius * angle.sin();
            let distance = angle * radius;
            
            centerline.push(TrackPoint {
                x,
                y,
                distance_from_start_m: distance,
            });
        }
        
        // Create start positions
        let mut start_positions = Vec::new();
        for i in 0..16 {
            start_positions.push(GridSlot {
                position: (i + 1) as u8,
                x: radius - (i / 2) as f32 * 5.0,
                y: if i % 2 == 0 { -2.0 } else { 2.0 },
                yaw_rad: 0.0,
            });
        }
        
        Self {
            id: Uuid::new_v4(),
            name: "Default Oval".to_string(),
            centerline,
            width_m: 15.0,
            start_positions,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPoint {
    pub x: f32,
    pub y: f32,
    pub distance_from_start_m: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GridSlot {
    pub position: u8,
    pub x: f32,
    pub y: f32,
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
    pub pos_x: f32,
    pub pos_y: f32,
    pub yaw_rad: f32,
    pub vel_x: f32,
    pub vel_y: f32,
    pub speed_mps: f32,
    pub angular_vel_rad_s: f32,
    pub throttle_input: f32,
    pub brake_input: f32,
    pub steering_input: f32,
    pub track_progress: f32,
    pub current_lap: u16,
    pub finish_position: Option<u8>,
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,
    pub is_colliding: bool,
    
    // Telemetry
    pub tires: TireTelemetry,
    pub g_forces: GForces,
    pub suspension: SuspensionTelemetry,
    pub fuel_liters: f32,
    pub fuel_capacity_liters: f32,
    pub fuel_consumption_lps: f32, // Liters per second
    pub damage: DamageState,
    pub engine_rpm: f32,
}

impl CarState {
    pub fn new(player_id: PlayerId, car_config_id: CarConfigId, grid_slot: &GridSlot) -> Self {
        Self {
            player_id,
            car_config_id,
            grid_position: grid_slot.position,
            pos_x: grid_slot.x,
            pos_y: grid_slot.y,
            yaw_rad: grid_slot.yaw_rad,
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
            engine_rpm: 0.0,
        }
    }
}

// --- Race Session State (Server Authoritative) ---
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SessionState {
    Lobby,
    Countdown,
    Racing,
    Finished,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RaceSession {
    pub id: SessionId,
    pub track_config_id: TrackConfigId,
    pub host_player_id: PlayerId,
    pub state: SessionState,
    pub participants: HashMap<PlayerId, CarState>,
    pub max_players: u8,
    pub ai_count: u8,
    pub lap_limit: u8,
    pub current_tick: u32,
    pub countdown_ticks_remaining: Option<u16>,
    pub race_start_tick: Option<u32>,
}

impl RaceSession {
    pub fn new(
        host_player_id: PlayerId,
        track_config_id: TrackConfigId,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    ) -> Self {
        Self {
            id: Uuid::new_v4(),
            track_config_id,
            host_player_id,
            state: SessionState::Lobby,
            participants: HashMap::new(),
            max_players,
            ai_count,
            lap_limit,
            current_tick: 0,
            countdown_ticks_remaining: None,
            race_start_tick: None,
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
            connection_id: 12345,
            selected_car_config_id: None,
            is_ai: false,
        };
        
        assert_eq!(player.name, "TestPlayer");
        assert_eq!(player.connection_id, 12345);
        assert!(!player.is_ai);
    }

    #[test]
    fn test_car_config_default() {
        let car = CarConfig::default();
        assert_eq!(car.name, "Default Car");
        assert!(car.mass_kg > 0.0);
        assert!(car.max_engine_force_n > 0.0);
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
        let session = RaceSession::new(host_id, track_id, 8, 2, 5);
        
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
