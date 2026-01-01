use serde::{Deserialize, Serialize};
use uuid::Uuid;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = u64;

// --- Session State ---
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SessionState {
    Lobby,
    Starting,
    Racing,
    Finished,
    Closed,
}

impl std::fmt::Display for SessionState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SessionState::Lobby => write!(f, "Lobby"),
            SessionState::Starting => write!(f, "Starting"),
            SessionState::Racing => write!(f, "Racing"),
            SessionState::Finished => write!(f, "Finished"),
            SessionState::Closed => write!(f, "Closed"),
        }
    }
}

// --- Client to Server Messages ---
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "data")]
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
    RequestLobbyState,
    CreateSession {
        track_config_id: TrackConfigId,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    },
    JoinSession {
        session_id: SessionId,
    },
    JoinAsSpectator {
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
#[serde(tag = "type", content = "data")]
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
    // 3D Position
    pub pos_x: f32,
    pub pos_y: f32,
    pub pos_z: f32,
    // 3D Orientation
    pub yaw_rad: f32,
    pub pitch_rad: f32,
    pub roll_rad: f32,
    // Motion
    pub speed_mps: f32,
    pub throttle: f32,
    pub brake: f32,
    pub steering: f32,
    pub gear: i8,
    // Race progress
    pub current_lap: u16,
    pub track_progress: f32,
    pub finish_position: Option<u8>,
    // Status
    pub is_on_track: bool,
    pub is_colliding: bool,
}

/// Telemetry data sent to clients at high frequency (240Hz)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Telemetry {
    pub server_tick: u32,
    pub session_state: SessionState,
    pub countdown_ms: Option<u16>,
    pub car_states: Vec<CarStateTelemetry>,
}
