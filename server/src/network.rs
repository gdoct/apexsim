use crate::data::*;
use serde::{Deserialize, Serialize};

fn deserialize_uuid_from_string<'de, D>(deserializer: D) -> Result<uuid::Uuid, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let s = String::deserialize(deserializer)?;
    uuid::Uuid::parse_str(&s).map_err(serde::de::Error::custom)
}

fn deserialize_option_uuid_from_string<'de, D>(
    deserializer: D,
) -> Result<Option<uuid::Uuid>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let opt = Option::<String>::deserialize(deserializer)?;
    match opt {
        Some(s) => uuid::Uuid::parse_str(&s)
            .map(Some)
            .map_err(serde::de::Error::custom),
        None => Ok(None),
    }
}

/// Version of the wire protocol. Bumped on breaking changes; the server
/// rejects `Authenticate` messages carrying a different version.
///
/// v2: UDP handshake + telemetry/input over UDP, compact positional
/// telemetry encoding with session-scoped car indices, gear/clutch inputs.
pub const PROTOCOL_VERSION: u8 = 2;

// --- Client to Server Messages ---
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "data")]
pub enum ClientMessage {
    // TCP - Auth & Lobby
    Authenticate {
        token: String,
        player_name: String,
        /// Client protocol version; defaults to 0 (pre-versioning clients)
        /// which the server rejects with a clear `AuthFailure`.
        #[serde(default)]
        protocol_version: u8,
    },
    Heartbeat {
        client_tick: u32,
    },
    SelectCar {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        car_config_id: CarConfigId,
    },
    RequestLobbyState,
    CreateSession {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        track_config_id: TrackConfigId,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
        #[serde(default)]
        session_kind: SessionKind,
    },
    JoinSession {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        session_id: SessionId,
    },
    JoinAsSpectator {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        session_id: SessionId,
    },
    LeaveSession,
    StartSession,
    SetGameMode {
        mode: GameMode,
    },
    StartCountdown {
        countdown_seconds: u16,
        next_mode: GameMode,
    },
    Disconnect,

    // UDP - Binds the sender's UDP address to the TCP connection that was
    // issued `token` in `AuthSuccess`. Server replies `UdpHandshakeAck` over
    // UDP; clients should re-send until acked (datagrams may be lost).
    UdpHandshake {
        token: String,
    },

    // UDP - High frequency
    PlayerInput {
        server_tick_ack: u32,
        throttle: f32,
        brake: f32,
        steering: f32,
        /// Desired gear (-1 = reverse, 0 = neutral, 1..). `None` keeps the
        /// current gear.
        #[serde(default)]
        gear: Option<i8>,
        /// Clutch engagement (0.0 = disengaged, 1.0 = engaged).
        #[serde(default)]
        clutch: Option<f32>,
    },
}

// --- Message Priority ---
/// Priority levels for server messages, used for drop/backpressure policies
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum MessagePriority {
    /// Can be dropped when queue is full (telemetry, heartbeats, periodic updates)
    Droppable = 0,
    /// Must be delivered or client should be disconnected (auth, errors, session control)
    Critical = 1,
}

// --- Helper structs for UUID serialization ---
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct AuthSuccessData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub player_id: PlayerId,
    pub server_version: u32,
    /// The server's wire protocol version (matches the client's, or auth
    /// would have failed).
    #[serde(default)]
    pub protocol_version: u8,
    /// One-time token to present in `UdpHandshake` to bind a UDP address to
    /// this connection.
    #[serde(default)]
    pub udp_token: String,
    /// UDP port the server listens on (same host as the TCP endpoint).
    #[serde(default)]
    pub udp_port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SessionJoinedData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    pub your_grid_position: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct PlayerDisconnectedData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub player_id: PlayerId,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LobbyStateData {
    pub players_in_lobby: Vec<LobbyPlayer>,
    pub available_sessions: Vec<SessionSummary>,
    pub car_configs: Vec<CarConfigSummary>,
    pub track_configs: Vec<TrackConfigSummary>,
}

// --- Server to Client Messages ---
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "data")]
pub enum ServerMessage {
    // TCP - Auth & Lobby
    AuthSuccess(AuthSuccessData),
    AuthFailure { reason: String },
    HeartbeatAck { server_tick: u32 },
    LobbyState(LobbyStateData),
    SessionJoined(SessionJoinedData),
    SessionLeft,
    SessionStarting { countdown_seconds: u8 },
    GameModeChanged { mode: GameMode },
    CountdownUpdate { seconds_remaining: u16 },
    Error { code: u16, message: String },
    PlayerDisconnected(PlayerDisconnectedData),

    // UDP - Confirms a `UdpHandshake`; from then on telemetry flows over UDP.
    UdpHandshakeAck,

    // TCP - Maps session-scoped car indices (used by compact telemetry) to
    // player identity. Sent on join and whenever session membership changes.
    SessionRoster(SessionRosterData),

    // Full (named-encoding) telemetry. Used internally for replays; the wire
    // uses `TelemetryCompact` since protocol v2.
    Telemetry(Telemetry),

    // UDP - High frequency telemetry, positional encoding (`rmp_serde::to_vec`).
    TelemetryCompact(CompactTelemetry),
}

impl ServerMessage {
    /// Returns the priority of this message for queue management
    pub fn priority(&self) -> MessagePriority {
        match self {
            // Critical messages - must be delivered or client disconnected
            ServerMessage::AuthSuccess(_) => MessagePriority::Critical,
            ServerMessage::AuthFailure { .. } => MessagePriority::Critical,
            ServerMessage::Error { .. } => MessagePriority::Critical,
            ServerMessage::SessionJoined(_) => MessagePriority::Critical,
            ServerMessage::SessionStarting { .. } => MessagePriority::Critical,
            ServerMessage::SessionLeft => MessagePriority::Critical,
            ServerMessage::GameModeChanged { .. } => MessagePriority::Critical,
            ServerMessage::SessionRoster(_) => MessagePriority::Critical,

            // Droppable messages - can be dropped when queue is full
            ServerMessage::HeartbeatAck { .. } => MessagePriority::Droppable,
            ServerMessage::CountdownUpdate { .. } => MessagePriority::Droppable,
            ServerMessage::LobbyState(_) => MessagePriority::Droppable,
            ServerMessage::Telemetry(_) => MessagePriority::Droppable,
            ServerMessage::TelemetryCompact(_) => MessagePriority::Droppable,
            ServerMessage::UdpHandshakeAck => MessagePriority::Droppable,
            ServerMessage::PlayerDisconnected(_) => MessagePriority::Droppable,
        }
    }
}

// --- Lightweight Lobby Structures ---
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LobbyPlayer {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string",
        rename = "Id"
    )]
    pub id: PlayerId,
    pub name: String,
    #[serde(
        serialize_with = "serialize_option_uuid_as_string",
        deserialize_with = "deserialize_option_uuid_from_string",
        rename = "SelectedCar"
    )]
    pub selected_car: Option<CarConfigId>,
    #[serde(
        serialize_with = "serialize_option_uuid_as_string",
        deserialize_with = "deserialize_option_uuid_from_string",
        rename = "InSession"
    )]
    pub in_session: Option<SessionId>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SessionSummary {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string",
        rename = "Id"
    )]
    pub id: SessionId,
    pub track_name: String,
    /// Track file relative to content folder (e.g. "tracks/real/Austin.yaml")
    pub track_file: String,
    pub host_name: String,
    pub session_kind: SessionKind,
    pub player_count: u8,
    pub max_players: u8,
    pub state: SessionState,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct CarConfigSummary {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string",
        rename = "Id"
    )]
    pub id: CarConfigId,
    pub name: String,
    pub model_path: String,
    pub mass_kg: f32,
    pub max_engine_force_n: f32,
}

fn serialize_uuid_as_string<S>(uuid: &uuid::Uuid, serializer: S) -> Result<S::Ok, S::Error>
where
    S: serde::Serializer,
{
    serializer.serialize_str(&uuid.to_string())
}

fn serialize_option_uuid_as_string<S>(
    uuid: &Option<uuid::Uuid>,
    serializer: S,
) -> Result<S::Ok, S::Error>
where
    S: serde::Serializer,
{
    match uuid {
        Some(u) => serializer.serialize_str(&u.to_string()),
        None => serializer.serialize_none(),
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPoint {
    pub x: f32,
    pub y: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct TrackConfigSummary {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string",
        rename = "Id"
    )]
    pub id: TrackConfigId,
    pub name: String,
    /// Simplified centerline points for visualization (every Nth point)
    #[serde(default)]
    pub centerline: Vec<TrackPoint>,
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
    pub engine_rpm: f32,
    pub suspension: SuspensionTelemetry,
    // Race progress
    pub current_lap: u16,
    pub track_progress: f32,
    pub finish_position: Option<u8>,
    pub current_lap_time_ms: u32,
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,
    // Status
    pub is_on_track: bool,
    pub is_colliding: bool,
}

/// Telemetry data sent to clients at high frequency (240Hz)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Telemetry {
    pub server_tick: u32,
    pub session_state: SessionState,
    pub game_mode: GameMode,
    pub countdown_ms: Option<u16>,
    pub car_states: Vec<CarStateTelemetry>,
}

// --- Compact wire telemetry (protocol v2) ---
//
// Same data as `Telemetry`, but identified by a session-scoped `car_index`
// instead of a UUID and always encoded positionally (`rmp_serde::to_vec`,
// no field names). The index → player mapping is delivered reliably over
// TCP via `ServerMessage::SessionRoster`.

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompactCarState {
    /// Index into the most recent `SessionRoster` for this session.
    pub car_index: u8,
    pub pos_x: f32,
    pub pos_y: f32,
    pub pos_z: f32,
    pub yaw_rad: f32,
    pub pitch_rad: f32,
    pub roll_rad: f32,
    pub speed_mps: f32,
    pub throttle: f32,
    pub brake: f32,
    pub steering: f32,
    pub gear: i8,
    pub engine_rpm: f32,
    pub suspension: SuspensionTelemetry,
    pub current_lap: u16,
    pub track_progress: f32,
    pub finish_position: Option<u8>,
    pub current_lap_time_ms: u32,
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,
    pub is_on_track: bool,
    pub is_colliding: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompactTelemetry {
    pub server_tick: u32,
    pub session_state: SessionState,
    pub game_mode: GameMode,
    pub countdown_ms: Option<u16>,
    pub car_states: Vec<CompactCarState>,
}

impl CompactCarState {
    pub fn from_car_state(state: &CarState, car_index: u8) -> Self {
        Self {
            car_index,
            pos_x: state.pos_x,
            pos_y: state.pos_y,
            pos_z: state.pos_z,
            yaw_rad: state.yaw_rad,
            pitch_rad: state.pitch_rad,
            roll_rad: state.roll_rad,
            speed_mps: state.speed_mps,
            throttle: state.throttle_input,
            brake: state.brake_input,
            steering: state.steering_input,
            gear: state.gear,
            engine_rpm: state.engine_rpm,
            suspension: state.suspension,
            current_lap: state.current_lap,
            track_progress: state.track_progress,
            finish_position: state.finish_position,
            current_lap_time_ms: state.current_lap_time_ms,
            last_lap_time_ms: state.last_lap_time_ms,
            best_lap_time_ms: state.best_lap_time_ms,
            is_on_track: state.is_on_track,
            is_colliding: state.is_colliding,
        }
    }
}

// --- Session roster (car index → player identity) ---

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct RosterEntry {
    pub car_index: u8,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub player_id: PlayerId,
    pub player_name: String,
    pub is_ai: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SessionRosterData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    pub entries: Vec<RosterEntry>,
}

impl From<&CarState> for CarStateTelemetry {
    fn from(state: &CarState) -> Self {
        Self {
            player_id: state.player_id,
            pos_x: state.pos_x,
            pos_y: state.pos_y,
            pos_z: state.pos_z,
            yaw_rad: state.yaw_rad,
            pitch_rad: state.pitch_rad,
            roll_rad: state.roll_rad,
            speed_mps: state.speed_mps,
            throttle: state.throttle_input,
            brake: state.brake_input,
            steering: state.steering_input,
            gear: state.gear,
            engine_rpm: state.engine_rpm,
            suspension: state.suspension,
            current_lap: state.current_lap,
            track_progress: state.track_progress,
            finish_position: state.finish_position,
            current_lap_time_ms: state.current_lap_time_ms,
            last_lap_time_ms: state.last_lap_time_ms,
            best_lap_time_ms: state.best_lap_time_ms,
            is_on_track: state.is_on_track,
            is_colliding: state.is_colliding,
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
            protocol_version: PROTOCOL_VERSION,
        };

        let serialized = rmp_serde::to_vec_named(&msg).unwrap();
        let deserialized: ClientMessage = rmp_serde::from_slice(&serialized).unwrap();

        match deserialized {
            ClientMessage::Authenticate {
                token,
                player_name,
                protocol_version,
            } => {
                assert_eq!(token, "test_token");
                assert_eq!(player_name, "Player1");
                assert_eq!(protocol_version, PROTOCOL_VERSION);
            }
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_authenticate_without_version_defaults_to_zero() {
        // A pre-v2 client that never sends protocol_version must still
        // deserialize (serde default = 0) so the server can reject it with a
        // clear AuthFailure instead of a parse error.
        #[derive(Serialize)]
        struct LegacyAuthData {
            token: String,
            player_name: String,
        }
        #[derive(Serialize)]
        struct LegacyEnvelope {
            r#type: String,
            data: LegacyAuthData,
        }
        let legacy = rmp_serde::to_vec_named(&LegacyEnvelope {
            r#type: "Authenticate".to_string(),
            data: LegacyAuthData {
                token: "t".to_string(),
                player_name: "old-client".to_string(),
            },
        })
        .unwrap();
        let deserialized: ClientMessage = rmp_serde::from_slice(&legacy).unwrap();
        match deserialized {
            ClientMessage::Authenticate {
                protocol_version, ..
            } => assert_eq!(protocol_version, 0),
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_server_message_serialization() {
        let player_id = Uuid::new_v4();
        let msg = ServerMessage::AuthSuccess(AuthSuccessData {
            player_id,
            server_version: 1,
            protocol_version: PROTOCOL_VERSION,
            udp_token: "udp-token".to_string(),
            udp_port: 9001,
        });

        let serialized = rmp_serde::to_vec_named(&msg).unwrap();
        let deserialized: ServerMessage = rmp_serde::from_slice(&serialized).unwrap();

        match deserialized {
            ServerMessage::AuthSuccess(data) => {
                assert_eq!(data.player_id, player_id);
                assert_eq!(data.server_version, 1);
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
            gear: Some(3),
            clutch: Some(1.0),
        };

        let serialized = rmp_serde::to_vec_named(&msg).unwrap();
        let deserialized: ClientMessage = rmp_serde::from_slice(&serialized).unwrap();

        match deserialized {
            ClientMessage::PlayerInput {
                server_tick_ack,
                throttle,
                brake,
                steering,
                gear,
                clutch,
            } => {
                assert_eq!(server_tick_ack, 100);
                assert_eq!(throttle, 0.8);
                assert_eq!(brake, 0.0);
                assert_eq!(steering, -0.5);
                assert_eq!(gear, Some(3));
                assert_eq!(clutch, Some(1.0));
            }
            _ => panic!("Wrong message type"),
        }
    }

    #[test]
    fn test_compact_telemetry_is_substantially_smaller() {
        // The compact positional encoding must save at least 40% over the
        // named encoding for a realistically sized session (8 cars).
        let player_id = Uuid::new_v4();
        let car_id = Uuid::new_v4();
        let grid_slot = GridSlot {
            position: 1,
            x: 1.0,
            y: 2.0,
            z: 0.0,
            yaw_rad: 0.5,
        };
        let mut state = CarState::new(player_id, car_id, &grid_slot);
        state.speed_mps = 42.0;
        state.current_lap = 3;
        state.last_lap_time_ms = Some(81_500);
        state.best_lap_time_ms = Some(80_900);

        let named = ServerMessage::Telemetry(Telemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: (0..8).map(|_| CarStateTelemetry::from(&state)).collect(),
        });
        let compact = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: (0..8u8)
                .map(|i| CompactCarState::from_car_state(&state, i))
                .collect(),
        });

        let named_bytes = rmp_serde::to_vec_named(&named).unwrap();
        let compact_bytes = rmp_serde::to_vec(&compact).unwrap();

        assert!(
            (compact_bytes.len() as f32) < (named_bytes.len() as f32) * 0.6,
            "compact telemetry should be at least 40% smaller: named={}B compact={}B",
            named_bytes.len(),
            compact_bytes.len()
        );

        // And it must round-trip through the positional encoding.
        let decoded: ServerMessage = rmp_serde::from_slice(&compact_bytes).unwrap();
        match decoded {
            ServerMessage::TelemetryCompact(t) => {
                assert_eq!(t.server_tick, 123_456);
                assert_eq!(t.car_states.len(), 8);
                assert_eq!(t.car_states[5].car_index, 5);
                assert_eq!(t.car_states[0].speed_mps, 42.0);
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
            z: 0.0,
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
