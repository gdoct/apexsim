use crate::data::*;
use serde::{Deserialize, Serialize};

// --- Client to Server Messages ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ClientMessage {
    // TCP - Auth & Lobby
    Authenticate {
        token: String,
        player_name: String,
    },
    Heartbeat {
        client_tick: u32,
    },
    SelectCar {
        car_config_id: CarConfigId,
    },
    CreateSession {
        track_config_id: TrackConfigId,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    },
    JoinSession {
        session_id: SessionId,
    },
    LeaveSession,
    StartSession,
    Disconnect,

    // UDP - High frequency
    PlayerInput {
        server_tick_ack: u32,
        throttle: f32,
        brake: f32,
        steering: f32,
    },
}

// --- Server to Client Messages ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ServerMessage {
    // TCP - Auth & Lobby
    AuthSuccess {
        player_id: PlayerId,
        server_version: u32,
    },
    AuthFailure {
        reason: String,
    },
    HeartbeatAck {
        server_tick: u32,
    },
    LobbyState {
        players_in_lobby: Vec<LobbyPlayer>,
        available_sessions: Vec<SessionSummary>,
        car_configs: Vec<CarConfigSummary>,
        track_configs: Vec<TrackConfigSummary>,
    },
    SessionJoined {
        session_id: SessionId,
        your_grid_position: u8,
    },
    SessionLeft,
    SessionStarting {
        countdown_seconds: u8,
    },
    Error {
        code: u16,
        message: String,
    },
    PlayerDisconnected {
        player_id: PlayerId,
    },

    // UDP - High frequency telemetry
    Telemetry(Telemetry),
}

// --- Lightweight Lobby Structures ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LobbyPlayer {
    pub id: PlayerId,
    pub name: String,
    pub selected_car: Option<CarConfigId>,
    pub in_session: Option<SessionId>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionSummary {
    pub id: SessionId,
    pub track_name: String,
    pub host_name: String,
    pub player_count: u8,
    pub max_players: u8,
    pub state: SessionState,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarConfigSummary {
    pub id: CarConfigId,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackConfigSummary {
    pub id: TrackConfigId,
    pub name: String,
}

// --- Compact Telemetry ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarStateTelemetry {
    pub player_id: PlayerId,
    pub pos_x: f32,
    pub pos_y: f32,
    pub yaw_rad: f32,
    pub speed_mps: f32,
    pub throttle: f32,
    pub steering: f32,
    pub current_lap: u16,
    pub track_progress: f32,
    pub finish_position: Option<u8>,
}

/// Telemetry data sent to clients at high frequency (240Hz)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Telemetry {
    pub server_tick: u32,
    pub session_state: SessionState,
    pub countdown_ms: Option<u16>,
    pub car_states: Vec<CarStateTelemetry>,
}

impl From<&CarState> for CarStateTelemetry {
    fn from(state: &CarState) -> Self {
        Self {
            player_id: state.player_id,
            pos_x: state.pos_x,
            pos_y: state.pos_y,
            yaw_rad: state.yaw_rad,
            speed_mps: state.speed_mps,
            throttle: state.throttle_input,
            steering: state.steering_input,
            current_lap: state.current_lap,
            track_progress: state.track_progress,
            finish_position: state.finish_position,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    #[test]
    fn test_client_message_serialization() {
        let msg = ClientMessage::Authenticate {
            token: "test_token".to_string(),
            player_name: "Player1".to_string(),
        };

        let serialized = bincode::serialize(&msg).unwrap();
        let deserialized: ClientMessage = bincode::deserialize(&serialized).unwrap();

        match deserialized {
            ClientMessage::Authenticate { token, player_name } => {
                assert_eq!(token, "test_token");
                assert_eq!(player_name, "Player1");
            }
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_server_message_serialization() {
        let player_id = Uuid::new_v4();
        let msg = ServerMessage::AuthSuccess {
            player_id,
            server_version: 1,
        };

        let serialized = bincode::serialize(&msg).unwrap();
        let deserialized: ServerMessage = bincode::deserialize(&serialized).unwrap();

        match deserialized {
            ServerMessage::AuthSuccess {
                player_id: pid,
                server_version,
            } => {
                assert_eq!(pid, player_id);
                assert_eq!(server_version, 1);
            }
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_player_input_serialization() {
        let msg = ClientMessage::PlayerInput {
            server_tick_ack: 100,
            throttle: 0.8,
            brake: 0.0,
            steering: -0.5,
        };

        let serialized = bincode::serialize(&msg).unwrap();
        let deserialized: ClientMessage = bincode::deserialize(&serialized).unwrap();

        match deserialized {
            ClientMessage::PlayerInput {
                server_tick_ack,
                throttle,
                brake,
                steering,
            } => {
                assert_eq!(server_tick_ack, 100);
                assert_eq!(throttle, 0.8);
                assert_eq!(brake, 0.0);
                assert_eq!(steering, -0.5);
            }
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_telemetry_conversion() {
        let player_id = Uuid::new_v4();
        let car_id = Uuid::new_v4();
        let grid_slot = GridSlot {
            position: 1,
            x: 10.0,
            y: 20.0,
            yaw_rad: 0.5,
        };

        let mut car_state = CarState::new(player_id, car_id, &grid_slot);
        car_state.speed_mps = 50.0;
        car_state.throttle_input = 0.9;
        car_state.current_lap = 2;

        let telemetry = CarStateTelemetry::from(&car_state);

        assert_eq!(telemetry.player_id, player_id);
        assert_eq!(telemetry.pos_x, 10.0);
        assert_eq!(telemetry.pos_y, 20.0);
        assert_eq!(telemetry.speed_mps, 50.0);
        assert_eq!(telemetry.throttle, 0.9);
        assert_eq!(telemetry.current_lap, 2);
    }
}
