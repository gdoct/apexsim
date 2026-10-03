use crate::car_setup::CarSetup;
use crate::data::*;
pub use crate::feedback::DriverFeedback;
use serde::{Deserialize, Serialize};

pub(crate) fn deserialize_uuid_from_string<'de, D>(deserializer: D) -> Result<uuid::Uuid, D::Error>
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
        /// Which of the car's liveries: 0 the car as authored, 1.. its
        /// `[[livery]]` tables. Older clients leave it out.
        #[serde(default)]
        livery: u8,
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
        /// Which aids drivers may use here; every one when absent.
        #[serde(default)]
        allowed_assists: AllowedAssists,
        /// Weather and time of day; a sunny afternoon when absent.
        #[serde(default)]
        conditions: SessionConditions,
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
    /// Driver aids the server runs for this player. Sent on joining a
    /// session and whenever the setting changes; ignored outside a session.
    SetDriverAids {
        #[serde(default)]
        auto_gearbox: bool,
        /// Speed-sensitive steering: full input asks for the tightest turn
        /// the car can hold at its speed (`physics::assisted_steering`).
        #[serde(default)]
        steering_assist: bool,
        /// ABS on or off; absent (an older client) keeps the car's own.
        #[serde(default)]
        abs: Option<bool>,
        /// Traction control level; absent keeps the car's own.
        #[serde(default)]
        traction_control: Option<TractionControl>,
        /// How much damage the car takes; absent is full damage. Left off
        /// the wire when unset, so the older aids keep their bytes.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        damage: Option<DamageLevel>,
    },
    /// The garage setup for this player's car, as clicks per knob
    /// (`car_setup::CarSetup`). Sent on joining a session and whenever a
    /// knob changes; the server clamps every knob into its range and bakes
    /// the result into the car this driver is simulated with. Ignored
    /// outside a session; an all-zero setup is the car as filed.
    SetCarSetup(CarSetup),
    StartCountdown {
        countdown_seconds: u16,
        next_mode: GameMode,
    },
    /// A hotlap driver asks to be moved: into the garage or out onto the
    /// run-up before the line. Any participant may send it; refused with an
    /// `Error` outside `GameMode::Hotlap`. Which side of the wall the car is
    /// on comes back in telemetry (`lap_flags` bit 2).
    HotlapRelocate {
        destination: HotlapDestination,
        /// Go out on cold tyres (blankets or the air, cold brakes), as out
        /// of the garage, instead of at the compound's optimum. Left off
        /// the wire when false, so an older client's bytes are unchanged.
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        cold_tyres: bool,
    },
    /// Asks for the trace of the driver's record lap on the session's track
    /// in their car, for a ghost car or a replay. Answered with `GhostLap`,
    /// empty when no record lap is stored.
    RequestGhost,
    /// Asks which showcases the server plays (`crate::showcase`); answered
    /// with `Showcases`.
    ListShowcases,
    /// Watch a showcase: a rendered race played in a loop, as a spectator
    /// stream (`crate::spectator`). `id` names a channel from `Showcases`;
    /// none is the server's first. Answered with `SpectatorJoined` and the
    /// stream's records, or an `Error` when there is no such showcase.
    SpectateShowcase {
        #[serde(default)]
        id: Option<String>,
    },
    /// Stop watching. Also implied by creating or joining a session.
    LeaveSpectate,
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
        /// The DRS button is held. The server decides whether the flap
        /// actually opens (`crate::drs`); a client from before the field
        /// never opens it.
        #[serde(default)]
        drs: Option<bool>,
        /// The headlight switch. `None` (and a client from before the
        /// field) leaves the lights to the session's conditions.
        #[serde(default)]
        headlights: Option<bool>,
        /// The flash button is held.
        #[serde(default)]
        flash: Option<bool>,
        /// The hybrid's mode (`crate::hybrid::ErsMode`: 0 harvest, 1
        /// balanced, 2 attack). `None` (and a client from before the
        /// field) keeps the car's.
        #[serde(default)]
        ers_mode: Option<u8>,
        /// The overtake button is held.
        #[serde(default)]
        ers_boost: Option<bool>,
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
    /// What was joined, so a client can tell its menu's demo session from a
    /// session the player takes part in without waiting for a lobby snapshot
    /// (a demo session is never in one).
    #[serde(default)]
    pub session_kind: SessionKind,
    /// The aids the host allows here, so the client can lock the rest in its
    /// settings; every one from a server that predates the field.
    #[serde(default)]
    pub allowed_assists: AllowedAssists,
    /// The session's weather and clock, so the client can light the track
    /// before the first lobby snapshot; a sunny afternoon from a server that
    /// predates the field.
    #[serde(default)]
    pub conditions: SessionConditions,
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
    /// The server plays showcases (`SpectateShowcase`): a client in the menu
    /// watches one instead of asking for a demo session. False from a server
    /// that predates the field.
    #[serde(default)]
    pub showcase_available: bool,
}

/// What a spectator stream is of.
#[repr(u8)]
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    serde_repr::Serialize_repr,
    serde_repr::Deserialize_repr,
    Default,
)]
pub enum SpectatorKind {
    /// A rendered race played from a file.
    #[default]
    Showcase = 0,
    /// A session being raced now (not sent yet).
    Live = 1,
}

/// One showcase channel, as `Showcases` lists it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ShowcaseSummary {
    /// What `SpectateShowcase` takes.
    pub id: String,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    pub track_name: String,
    /// The field's class as car.toml spells it (`GT3`, `F1`).
    pub class: String,
    pub conditions: SessionConditions,
    /// Seconds of race before it starts over.
    pub duration_s: f32,
    pub cars: u8,
    pub viewers: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ShowcasesData {
    pub entries: Vec<ShowcaseSummary>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SpectatorJoinedData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub stream_id: uuid::Uuid,
    pub kind: SpectatorKind,
    /// The channel joined (a showcase's id).
    pub showcase_id: String,
}

/// A run of spectator stream records (`crate::spectator`) in the stream's
/// own framing, `[u32 big-endian length][body]` each: MessagePack `bin` on
/// the wire, so records read from a file are forwarded without being
/// decoded, and a joining viewer's whole preamble is one message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RecordBytes(pub Vec<u8>);

impl Serialize for RecordBytes {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(&self.0)
    }
}

impl<'de> Deserialize<'de> for RecordBytes {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct BytesVisitor;
        impl<'de> serde::de::Visitor<'de> for BytesVisitor {
            type Value = RecordBytes;
            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                f.write_str("a record's bytes")
            }
            fn visit_bytes<E: serde::de::Error>(self, v: &[u8]) -> Result<RecordBytes, E> {
                Ok(RecordBytes(v.to_vec()))
            }
            fn visit_byte_buf<E: serde::de::Error>(self, v: Vec<u8>) -> Result<RecordBytes, E> {
                Ok(RecordBytes(v))
            }
            fn visit_seq<A: serde::de::SeqAccess<'de>>(
                self,
                mut seq: A,
            ) -> Result<RecordBytes, A::Error> {
                let mut out = Vec::with_capacity(seq.size_hint().unwrap_or(0));
                while let Some(byte) = seq.next_element::<u8>()? {
                    out.push(byte);
                }
                Ok(RecordBytes(out))
            }
        }
        deserializer.deserialize_bytes(BytesVisitor)
    }
}

/// Stream records wrapped as the TCP message a client receives:
/// `{"type": "SpectatorRecord", "data": bin}` with the framed records as
/// the `bin`: the bytes `rmp_serde::to_vec_named` gives for
/// `ServerMessage::SpectatorRecord`, built without a pass through serde.
pub fn spectator_record_message<'a>(bodies: impl IntoIterator<Item = &'a [u8]>) -> Vec<u8> {
    const HEAD: &[u8] = b"\x82\xA4type\xAFSpectatorRecord\xA4data";
    let mut framed = Vec::new();
    for body in bodies {
        framed.extend_from_slice(&(body.len() as u32).to_be_bytes());
        framed.extend_from_slice(body);
    }
    let mut out = Vec::with_capacity(HEAD.len() + 5 + framed.len());
    out.extend_from_slice(HEAD);
    rmp::encode::write_bin_len(&mut out, framed.len() as u32).expect("vec write");
    out.extend_from_slice(&framed);
    out
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

    // TCP - The racing line for the joining player's car, for the client's
    // racing-line overlay. Sent once, right after `SessionJoined`.
    RacingLine(RacingLineData),

    // TCP - Where the session's sector lines are, so the client can tell
    // which sector a car is in from the station telemetry already carries.
    // Sent once, with the racing line.
    TrackSectors(TrackSectorsData),

    // TCP - One car crossed a timing line: a sector split, or the lap. Sent
    // to every human in the session as it happens. Reliable on purpose — a
    // dropped split leaves a hole in the driver's lap that nothing refills.
    LapTiming(LapTimingData),

    // TCP - The joining driver's stored best for this track and car, and
    // every time they beat it. `record` is absent when they have none yet.
    LapRecord(LapRecordData),

    // TCP - The trace of the driver's record lap, on request (`RequestGhost`):
    // the pose at `records::GHOST_SAMPLE_HZ` from the line to the line, for
    // the client to drive a ghost car through.
    GhostLap(GhostLapData),

    // TCP - The garage's reference card for the joining driver's car: each
    // setup knob's stock value and click in real units (`setup_sheet.rs`).
    // Sent once, right after `SessionJoined`.
    CarSetupSheet(crate::setup_sheet::CarSetupSheetData),

    // TCP - The showcases the server plays, answering `ListShowcases`.
    Showcases(ShowcasesData),

    // TCP - A showcase was joined (`SpectateShowcase`). The stream's
    // `Header`, `Roster`, `Path` and `TrackSectors` records follow.
    SpectatorJoined(SpectatorJoinedData),

    // TCP - Records of the spectator stream being watched, framed as the
    // stream frames them: everything but frames, and frames too for a
    // client with no UDP. Over UDP a frame is one record's body alone, with
    // no length and no envelope.
    SpectatorRecord(RecordBytes),

    // Full (named-encoding) telemetry. Used internally for replays; the wire
    // uses `TelemetryCompact` since protocol v2.
    Telemetry(Telemetry),

    // UDP - High frequency telemetry, positional encoding (`rmp_serde::to_vec`).
    TelemetryCompact(CompactTelemetry),

    // UDP - What one car's driver should feel (force feedback), positional
    // encoding, sent only to that car's human driver with each telemetry
    // frame. UDP only: a late force is worse than none.
    DriverFeedback(DriverFeedback),
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
            ServerMessage::RacingLine(_) => MessagePriority::Critical,
            ServerMessage::TrackSectors(_) => MessagePriority::Critical,
            ServerMessage::LapTiming(_) => MessagePriority::Critical,
            ServerMessage::LapRecord(_) => MessagePriority::Critical,
            // Asked for once; a reply that never came leaves the driver
            // with no ghost at all.
            ServerMessage::GhostLap(_) => MessagePriority::Critical,
            ServerMessage::CarSetupSheet(_) => MessagePriority::Critical,
            ServerMessage::Showcases(_) => MessagePriority::Critical,
            ServerMessage::SpectatorJoined(_) => MessagePriority::Critical,
            ServerMessage::SpectatorRecord(_) => MessagePriority::Critical,

            // Droppable messages - can be dropped when queue is full
            ServerMessage::HeartbeatAck { .. } => MessagePriority::Droppable,
            ServerMessage::CountdownUpdate { .. } => MessagePriority::Droppable,
            ServerMessage::LobbyState(_) => MessagePriority::Droppable,
            ServerMessage::Telemetry(_) => MessagePriority::Droppable,
            ServerMessage::TelemetryCompact(_) => MessagePriority::Droppable,
            ServerMessage::DriverFeedback(_) => MessagePriority::Droppable,
            ServerMessage::UdpHandshakeAck => MessagePriority::Droppable,
            ServerMessage::PlayerDisconnected(_) => MessagePriority::Droppable,
        }
    }
}

// --- Lightweight Lobby Structures ---
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
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

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SessionSummary {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string",
        rename = "Id"
    )]
    pub id: SessionId,
    pub track_name: String,
    /// Track file relative to content folder (e.g. "tracks/default/Austin.yaml")
    pub track_file: String,
    /// The track config the session runs on, so a client can find its
    /// `TrackConfigSummary` (and catalog row) without matching names.
    #[serde(
        default = "uuid::Uuid::nil",
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    pub host_name: String,
    pub session_kind: SessionKind,
    pub player_count: u8,
    pub max_players: u8,
    pub state: SessionState,
    /// Weather and time of day, for the session browser.
    #[serde(default)]
    pub conditions: SessionConditions,
    /// The race distance in laps, so a spectator's HUD can count down to it.
    /// Appended; 0 from an older server.
    #[serde(default)]
    pub lap_limit: u8,
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
    /// `content_crc` of the car.toml the server loaded; a client compares it
    /// with the checksum baked into its own catalog row. 0 when unknown.
    #[serde(default)]
    pub content_crc: u32,
    pub mass_kg: f32,
    pub max_engine_force_n: f32,
}

pub(crate) fn serialize_uuid_as_string<S>(
    uuid: &uuid::Uuid,
    serializer: S,
) -> Result<S::Ok, S::Error>
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
    /// `content_crc` of the track file the server loaded; a client compares it
    /// with the checksum baked into its own catalog row. 0 when unknown.
    #[serde(default)]
    pub content_crc: u32,
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
    /// Track limits (`crate::laps::LapTiming::flags`): bit 0 the lap in
    /// progress is struck, bit 1 the last completed lap was.
    #[serde(default)]
    pub lap_flags: u8,
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
    /// Track limits, as on `CarStateTelemetry`. Appended last: the encoding
    /// is positional, so a field added at the end is one an older client
    /// skips rather than one that shifts everything after it.
    pub lap_flags: u8,
    /// Fuel in the tank, in tenths of a litre ([`fuel_decilitres`]).
    /// Appended after `lap_flags`, for the same reason; an older server
    /// sends none.
    #[serde(default)]
    pub fuel_dl: u16,
    /// Each tyre's tread temperature, °C, FL FR RL RR ([`tyre_bytes`]).
    /// Appended after `fuel_dl`; 0 is "not known" (an older server sends
    /// none, a car not yet on its tyres has none).
    #[serde(default)]
    pub tyre_c: [u8; 4],
    /// Each tyre's running pressure, kPa (gauge), FL FR RL RR; 0 is "not
    /// known", as above.
    #[serde(default)]
    pub tyre_kpa: [u8; 4],
    /// The tow the car is in: the share of its drag the wake of the cars
    /// ahead saves, percent (`crate::slipstream`). Appended after
    /// `tyre_kpa`; 0 in clean air, and from an older server.
    #[serde(default)]
    pub tow_pct: u8,
    /// Each tyre's wear, percent, FL FR RL RR (`tyre_thermal`). Appended
    /// after `tow_pct`; all 0 from an older server.
    #[serde(default)]
    pub tyre_wear: [u8; 4],
    /// The compound on the car (`tyre_thermal::COMPOUNDS`: 0 soft, 1
    /// medium, 2 hard); [`COMPOUND_UNKNOWN`] before a set is fitted, or
    /// from an older server.
    #[serde(default = "compound_unknown")]
    pub compound: u8,
    /// The pit lane (`crate::pit`): [`PIT_FLAG_LIMITER`],
    /// [`PIT_FLAG_SERVICING`], [`PIT_FLAG_IN_LANE`].
    #[serde(default)]
    pub pit_flags: u8,
    /// Seconds left of a service under way, in tenths.
    #[serde(default)]
    pub service_ds: u16,
    /// Each corner's brake, °C, FL FR RL RR (`crate::brakes`). Appended
    /// after `service_ds`; all 0 from an older server.
    #[serde(default)]
    pub brake_c: [u16; 4],
    /// The engine's coolant, °C (`crate::engine_heat`); 0 from an older
    /// server.
    #[serde(default)]
    pub water_c: u8,
    /// The damage (`crate::damage`), percent: front, rear, left, right,
    /// engine. Appended after `water_c`; all 0 from an older server.
    #[serde(default)]
    pub damage: [u8; 5],
    /// The hybrid (`crate::hybrid`): the battery's charge and the lap's
    /// deployment budget left, percent (255: no hybrid / no budget), and
    /// `ers_flags` (bits 0-1 the mode, 2 deploying, 3 harvesting, 4 the
    /// overtake button). Appended after `damage`.
    #[serde(default = "no_hybrid")]
    pub ers_pct: u8,
    #[serde(default = "no_hybrid")]
    pub ers_lap_pct: u8,
    #[serde(default)]
    pub ers_flags: u8,
}

fn no_hybrid() -> u8 {
    255
}

/// The `pit_flags` byte of a car's telemetry.
pub fn pit_flags_of(state: &CarState) -> u8 {
    let mut flags = 0;
    if state.pit.limiter {
        flags |= PIT_FLAG_LIMITER;
    }
    if state.pit.servicing {
        flags |= PIT_FLAG_SERVICING;
    }
    if state.pit.in_lane {
        flags |= PIT_FLAG_IN_LANE;
    }
    flags
}

/// `compound` before a set is fitted.
pub const COMPOUND_UNKNOWN: u8 = 255;
fn compound_unknown() -> u8 {
    COMPOUND_UNKNOWN
}
/// `pit_flags` bit 0: the limiter holds the car (between the lane's lines).
pub const PIT_FLAG_LIMITER: u8 = 1;
/// `pit_flags` bit 1: stopped at its box, being serviced.
pub const PIT_FLAG_SERVICING: u8 = 2;
/// `pit_flags` bit 2: in the pit lane.
pub const PIT_FLAG_IN_LANE: u8 = 4;

/// A car's tyres as telemetry carries them: the tread temperatures and the
/// running pressures, each rounded to a whole degree / kPa and held within
/// 1..=255, so 0 is left to mean "not known" (a car whose tyres were never
/// fitted).
pub fn tyre_bytes(state: &CarState) -> ([u8; 4], [u8; 4]) {
    if !state.tyres_fitted {
        return ([0; 4], [0; 4]);
    }
    let byte = |v: f32| v.round().clamp(1.0, 255.0) as u8;
    let tyres = state.tires.each();
    (
        tyres.map(|t| byte(t.temperature_c)),
        tyres.map(|t| byte(t.pressure_kpa)),
    )
}

/// A tank's contents as telemetry carries them: tenths of a litre, rounded,
/// so a HUD can show one decimal and a msgpack uint16 carries a full tank.
pub fn fuel_decilitres(liters: f32) -> u16 {
    (liters.max(0.0) * 10.0).round().min(u16::MAX as f32) as u16
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
        let (tyre_c, tyre_kpa) = tyre_bytes(state);
        let (ers_pct, ers_lap_pct, ers_flags) = crate::hybrid::telemetry_bytes(state);
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
            lap_flags: lap_flags_of(state),
            fuel_dl: fuel_decilitres(state.fuel_liters),
            tyre_c,
            tyre_kpa,
            tow_pct: (state.wake.tow() * 100.0).round().clamp(0.0, 100.0) as u8,
            tyre_wear: state
                .tires
                .each()
                .map(|t| t.wear_percent.round().clamp(0.0, 100.0) as u8),
            compound: if state.tyres_fitted {
                state.tyre_compound
            } else {
                COMPOUND_UNKNOWN
            },
            pit_flags: pit_flags_of(state),
            brake_c: state
                .brake_temp_c
                .map(|t| t.round().clamp(0.0, u16::MAX as f32) as u16),
            water_c: state.water_temp_c.round().clamp(0.0, 255.0) as u8,
            damage: {
                let d = &state.damage;
                [
                    d.front_damage_percent,
                    d.rear_damage_percent,
                    d.left_damage_percent,
                    d.right_damage_percent,
                    d.engine_damage_percent,
                ]
                .map(|p| p.round().clamp(0.0, 100.0) as u8)
            },
            ers_pct,
            ers_lap_pct,
            ers_flags,
            service_ds: (state.pit.service_left_s.max(0.0) * 10.0)
                .round()
                .min(u16::MAX as f32) as u16,
        }
    }
}

/// The `lap_flags` byte of a car's telemetry: the track-limit bits from
/// `LapTiming::flags`, plus [`LAP_FLAG_IN_GARAGE`] for a hotlap car parked
/// in its garage.
pub fn lap_flags_of(state: &CarState) -> u8 {
    let mut flags = state.laps.flags();
    if state.in_garage {
        flags |= LAP_FLAG_IN_GARAGE;
    }
    if state.drs_allowed {
        flags |= crate::drs::LAP_FLAG_DRS_ALLOWED;
    }
    if state.drs_open {
        flags |= crate::drs::LAP_FLAG_DRS_OPEN;
    }
    if state.headlights {
        flags |= crate::headlights::LAP_FLAG_HEADLIGHTS;
    }
    if state.headlight_flash {
        flags |= crate::headlights::LAP_FLAG_HEADLIGHT_FLASH;
    }
    flags
}

/// `lap_flags` bit 2: the car is in the garage of a hotlap session — not
/// simulated, not to be drawn on the track.
pub const LAP_FLAG_IN_GARAGE: u8 = 4;

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
    /// The car this entry drives, so a client can draw each car with its own
    /// mesh: an AI field is a mix of the host class's cars.
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    /// The livery it wears: 0 the car as authored, 1.. its `[[livery]]`
    /// tables. Always within the car's own list.
    #[serde(default)]
    pub livery: u8,
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

// --- Racing line (driving aid) ---

/// A closed loop of evenly spaced points along the line a car should take,
/// each tagged with what the driver does there. Parallel arrays rather than
/// a list of point structs: a few thousand points, and a map per point would
/// triple the size of the message.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct RacingLineData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    /// Distance between consecutive points, metres.
    pub spacing_m: f32,
    /// Server-frame positions, metres. The last point joins back to the first.
    pub x: Vec<f32>,
    pub y: Vec<f32>,
    pub z: Vec<f32>,
    /// Per point: 0 full throttle, 1 partial throttle (at the grip limit or
    /// lifting), 2 braking.
    pub phase: Vec<u8>,
}

impl RacingLineData {
    pub fn from_profile(
        session_id: SessionId,
        profile: &crate::racing_line::RacingLineProfile,
    ) -> Self {
        Self {
            session_id,
            spacing_m: profile.spacing_m,
            x: profile.points.iter().map(|p| p[0]).collect(),
            y: profile.points.iter().map(|p| p[1]).collect(),
            z: profile.points.iter().map(|p| p[2]).collect(),
            phase: profile.phases.iter().map(|p| p.as_u8()).collect(),
        }
    }
}

// --- Lap timing ---

/// Where the sector lines are on this session's track: station in metres
/// from the start line, ascending, one short of the sector count (sector 1
/// always begins at the line).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct TrackSectorsData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    /// Lap length in metres, so the client can work in fractions of a lap.
    pub track_length_m: f32,
    pub boundaries_m: Vec<f32>,
}

/// A car crossed a timing line.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LapTimingData {
    /// Index into the current `SessionRoster`.
    pub car_index: u8,
    /// The lap the sector belongs to.
    pub lap: u16,
    /// Sector just completed, 0-based.
    pub sector: u8,
    pub sector_time_ms: u32,
    /// The lap time when this sector closed the lap, 0 otherwise.
    pub lap_time_ms: u32,
    /// Set when `lap_time_ms` is a completed lap.
    pub is_lap_end: bool,
    /// The lap was inside track limits.
    pub valid: bool,
    /// Bit 0 the driver's best lap this session, bit 1 the session's best
    /// lap by anyone, bit 2 the driver's best of this sector, bit 3 the
    /// session's best of it. What the HUD paints green and purple.
    pub flags: u8,
}

impl LapTimingData {
    pub const FLAG_PERSONAL_BEST_LAP: u8 = 1;
    pub const FLAG_SESSION_BEST_LAP: u8 = 2;
    pub const FLAG_PERSONAL_BEST_SECTOR: u8 = 4;
    pub const FLAG_SESSION_BEST_SECTOR: u8 = 8;
}

/// The driver's stored record for the session's track and car.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LapRecordData {
    /// The driver the record belongs to.
    pub player_name: String,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    /// The driver's best legal lap ever on this track in this car; 0 when
    /// they have none yet.
    pub lap_time_ms: u32,
    pub splits_ms: Vec<u32>,
    /// Set when this message announces a record just beaten rather than the
    /// one the driver arrived with.
    pub is_new: bool,
    /// The fastest lap anyone has set here in this car, 0 when unknown.
    pub track_record_ms: u32,
    pub track_record_holder: String,
    /// A trace of the record lap is stored, so a ghost can be driven from it.
    pub has_ghost: bool,
}

/// The trace of a record lap, for a ghost car (`ClientMessage::RequestGhost`).
/// The samples are struct-of-arrays: one lap is thousands of them, and a
/// named field per value would be most of the message.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct GhostLapData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    /// The lap the trace was recorded on; 0 when there is none, and then
    /// every array below is empty.
    pub lap_time_ms: u32,
    pub sample_hz: f32,
    /// Milliseconds since the lap started, one per sample.
    pub t_ms: Vec<u32>,
    /// Six per sample: x, y, z, yaw, pitch, roll (metres, radians, the
    /// server frame).
    pub pose: Vec<f32>,
    pub speed_mps: Vec<f32>,
    pub steering: Vec<f32>,
    pub gear: Vec<i8>,
    pub engine_rpm: Vec<f32>,
}

impl GhostLapData {
    /// A reply that says there is no recorded lap.
    pub fn empty(track_id: TrackConfigId, car_config_id: CarConfigId) -> Self {
        Self {
            track_id,
            car_config_id,
            lap_time_ms: 0,
            sample_hz: crate::records::GHOST_SAMPLE_HZ,
            t_ms: Vec::new(),
            pose: Vec::new(),
            speed_mps: Vec::new(),
            steering: Vec::new(),
            gear: Vec::new(),
            engine_rpm: Vec::new(),
        }
    }

    pub fn from_lap(
        track_id: TrackConfigId,
        car_config_id: CarConfigId,
        lap: &crate::records::GhostLap,
    ) -> Self {
        let n = lap.samples.len();
        let mut out = Self {
            track_id,
            car_config_id,
            lap_time_ms: lap.lap_time_ms,
            sample_hz: lap.sample_hz,
            t_ms: Vec::with_capacity(n),
            pose: Vec::with_capacity(n * 6),
            speed_mps: Vec::with_capacity(n),
            steering: Vec::with_capacity(n),
            gear: Vec::with_capacity(n),
            engine_rpm: Vec::with_capacity(n),
        };
        for s in &lap.samples {
            out.t_ms.push(s.t_ms);
            out.pose
                .extend_from_slice(&[s.x, s.y, s.z, s.yaw_rad, s.pitch_rad, s.roll_rad]);
            out.speed_mps.push(s.speed_mps);
            out.steering.push(s.steering);
            out.gear.push(s.gear);
            out.engine_rpm.push(s.engine_rpm);
        }
        out
    }

    pub fn sample_count(&self) -> usize {
        self.t_ms.len()
    }
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
            lap_flags: lap_flags_of(state),
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
    fn test_driver_aids_without_steering_assist_leave_it_off() {
        // A client from before the steering aid sends only auto_gearbox.
        #[derive(Serialize)]
        struct OldAids {
            auto_gearbox: bool,
        }
        #[derive(Serialize)]
        struct Envelope {
            r#type: String,
            data: OldAids,
        }
        let old = rmp_serde::to_vec_named(&Envelope {
            r#type: "SetDriverAids".to_string(),
            data: OldAids { auto_gearbox: true },
        })
        .unwrap();
        match rmp_serde::from_slice(&old).unwrap() {
            ClientMessage::SetDriverAids {
                auto_gearbox,
                steering_assist,
                abs,
                traction_control,
                damage,
            } => {
                assert!(auto_gearbox && !steering_assist);
                assert_eq!(abs, None, "an old client leaves the car's own ABS");
                assert_eq!(traction_control, None);
                assert_eq!(damage, None, "an old client takes full damage");
            }
            _ => panic!("Wrong message type"),
        }

        let both = ClientMessage::SetDriverAids {
            auto_gearbox: false,
            steering_assist: true,
            abs: Some(false),
            traction_control: Some(TractionControl::High),
            damage: Some(DamageLevel::Off),
        };
        let bytes = rmp_serde::to_vec_named(&both).unwrap();
        match rmp_serde::from_slice(&bytes).unwrap() {
            ClientMessage::SetDriverAids {
                auto_gearbox,
                steering_assist,
                abs,
                traction_control,
                damage,
            } => {
                assert!(!auto_gearbox && steering_assist);
                assert_eq!(abs, Some(false));
                assert_eq!(traction_control, Some(TractionControl::High));
                assert_eq!(damage, Some(DamageLevel::Off));
            }
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
            drs: Some(true),
            headlights: Some(false),
            flash: Some(true),
            ers_mode: Some(2),
            ers_boost: Some(true),
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
                drs,
                headlights,
                flash,
                ers_mode,
                ers_boost,
            } => {
                assert_eq!(ers_mode, Some(2));
                assert_eq!(ers_boost, Some(true));
                assert_eq!(server_tick_ack, 100);
                assert_eq!(throttle, 0.8);
                assert_eq!(brake, 0.0);
                assert_eq!(steering, -0.5);
                assert_eq!(gear, Some(3));
                assert_eq!(clutch, Some(1.0));
                assert_eq!(drs, Some(true));
                assert_eq!(headlights, Some(false));
                assert_eq!(flash, Some(true));
            }
            _ => panic!("Wrong message type"),
        }
    }

    /// The bytes the client sends for a `PlayerInput` with the DRS button
    /// held, for the C++ encoder to pin (`cargo test player_input_drs_wire_format
    /// -- --nocapture`); and a message from before the field decodes with
    /// the button up.
    #[test]
    fn test_player_input_drs_wire_format() {
        let msg = ClientMessage::PlayerInput {
            server_tick_ack: 7,
            throttle: 1.0,
            brake: 0.0,
            steering: 0.0,
            gear: None,
            clutch: None,
            drs: Some(true),
            headlights: None,
            flash: None,
            ers_mode: None,
            ers_boost: None,
        };
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        println!("C_PlayerInputDrs = {bytes:02x?}");
        // The field follows clutch: `a3 "drs" c3`, then the lights.
        let at = bytes
            .windows(5)
            .position(|w| w == [0xa3, b'd', b'r', b's', 0xc3])
            .expect("drs field");
        assert_eq!(bytes[at - 1], 0xc0, "clutch: nil just before drs");

        // The same message without the field, as an older client sends it.
        let old = rmp_serde::to_vec_named(&serde_json::json!({
            "type": "PlayerInput",
            "data": {
                "server_tick_ack": 7,
                "throttle": 1.0,
                "brake": 0.0,
                "steering": 0.0
            }
        }))
        .unwrap();
        match rmp_serde::from_slice::<ClientMessage>(&old).unwrap() {
            ClientMessage::PlayerInput {
                drs,
                gear,
                headlights,
                flash,
                ers_mode,
                ers_boost,
                ..
            } => {
                assert_eq!(ers_mode, None);
                assert_eq!(ers_boost, None);
                assert_eq!(drs, None);
                assert_eq!(gear, None);
                assert_eq!(headlights, None);
                assert_eq!(flash, None);
            }
            _ => panic!("wrong message"),
        }
    }

    /// The headlight switch and the flash button after `drs`, then the
    /// hybrid's mode and overtake button: this message is
    /// `ApexUdpGolden::C_PlayerInput` (`cargo test
    /// player_input_headlights_wire_format -- --nocapture`). The switch left
    /// to the conditions is nil, the flash a bool, the mode a byte.
    #[test]
    fn test_player_input_headlights_wire_format() {
        let mut msg = ClientMessage::PlayerInput {
            server_tick_ack: 4242,
            throttle: 1.0,
            brake: 0.0,
            steering: -0.5,
            gear: Some(4),
            clutch: None,
            drs: Some(false),
            headlights: None,
            flash: Some(false),
            ers_mode: Some(1),
            ers_boost: Some(false),
        };
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        println!("C_PlayerInput = {bytes:02x?}");
        let tail: &[u8] = &[
            0xa3, b'd', b'r', b's', 0xc2, // drs: false
            0xaa, b'h', b'e', b'a', b'd', b'l', b'i', b'g', b'h', b't', b's',
            0xc0, // headlights: nil
            0xa5, b'f', b'l', b'a', b's', b'h', 0xc2, // flash: false
            0xa8, b'e', b'r', b's', b'_', b'm', b'o', b'd', b'e', 0x01, // ers_mode: 1
            0xa9, b'e', b'r', b's', b'_', b'b', b'o', b'o', b's', b't',
            0xc2, // ers_boost: false
        ];
        assert!(bytes.ends_with(tail), "{bytes:02x?}");
        assert_eq!(bytes[23], 0x8b, "eleven fields in the data map");

        if let ClientMessage::PlayerInput {
            headlights, flash, ..
        } = &mut msg
        {
            *headlights = Some(true);
            *flash = Some(true);
        }
        let on = rmp_serde::to_vec_named(&msg).unwrap();
        assert_eq!(on.len(), bytes.len(), "a bool is as long as nil");
        assert_eq!(on[on.len() - 29], 0xc3, "headlights: true");
        assert_eq!(on[on.len() - 22], 0xc3, "flash: true");
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

    /// Golden bytes for the showcase messages (`ApexGoldenBlobs.h`):
    /// `cargo test showcase_wire_format -- --nocapture`.
    #[test]
    fn test_showcase_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let list = rmp_serde::to_vec_named(&ClientMessage::ListShowcases).unwrap();
        let spectate = rmp_serde::to_vec_named(&ClientMessage::SpectateShowcase {
            id: Some("Zandvoort.gt3.day".into()),
        })
        .unwrap();
        let spectate_any =
            rmp_serde::to_vec_named(&ClientMessage::SpectateShowcase { id: None }).unwrap();
        let leave = rmp_serde::to_vec_named(&ClientMessage::LeaveSpectate).unwrap();
        println!("C_ListShowcases: {}", hex(&list));
        println!("C_SpectateShowcase: {}", hex(&spectate));
        println!("C_SpectateShowcaseAny: {}", hex(&spectate_any));
        println!("C_LeaveSpectate: {}", hex(&leave));
        for bytes in [&list, &spectate, &spectate_any, &leave] {
            let _: ClientMessage = rmp_serde::from_slice(bytes).unwrap();
        }
        // An older client's message, without the id, reads as "any".
        let bare: ClientMessage =
            rmp_serde::from_slice(b"\x82\xA4type\xB0SpectateShowcase\xA4data\x80").unwrap();
        assert!(matches!(bare, ClientMessage::SpectateShowcase { id: None }));

        let showcases = ServerMessage::Showcases(ShowcasesData {
            entries: vec![ShowcaseSummary {
                id: "Zandvoort.gt3.day".into(),
                track_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
                track_name: "Zandervoort".into(),
                class: "GT3".into(),
                conditions: SessionConditions {
                    weather: Weather::Cloudy,
                    time_of_day_minutes: 16 * 60 + 30,
                    air_temp_c: Some(19),
                    humidity_pct: Some(60),
                    wind_kph: Some(12),
                    wind_from_deg: Some(90),
                },
                duration_s: 259.5,
                cars: 16,
                viewers: 2,
            }],
        });
        let joined = ServerMessage::SpectatorJoined(SpectatorJoinedData {
            stream_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            kind: SpectatorKind::Showcase,
            showcase_id: "Zandvoort.gt3.day".into(),
        });
        // Two tiny records in one message: the stream's own framing inside.
        let bodies: [&[u8]; 2] = [&[0x92, 0x07, 0xC0], &[0x93, 0x08, 0xC0, 0x01]];
        let record = spectator_record_message(bodies);
        let showcases_bytes = rmp_serde::to_vec_named(&showcases).unwrap();
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_Showcases: {}", hex(&showcases_bytes));
        println!("S_SpectatorJoined: {}", hex(&joined_bytes));
        println!("S_SpectatorRecord: {}", hex(&record));

        // The hand-built envelope is what serde would have written.
        let mut framed = Vec::new();
        for body in bodies {
            framed.extend_from_slice(&(body.len() as u32).to_be_bytes());
            framed.extend_from_slice(body);
        }
        assert_eq!(
            record,
            rmp_serde::to_vec_named(&ServerMessage::SpectatorRecord(RecordBytes(framed.clone())))
                .unwrap()
        );
        let back: ServerMessage = rmp_serde::from_slice(&record).unwrap();
        let ServerMessage::SpectatorRecord(run) = back else {
            panic!("not a record");
        };
        assert_eq!(run.0, framed);
        assert_eq!(
            crate::spectator::split_framed(&run.0).unwrap(),
            bodies.iter().map(|b| b.to_vec()).collect::<Vec<_>>()
        );
        let back: ServerMessage = rmp_serde::from_slice(&showcases_bytes).unwrap();
        let ServerMessage::Showcases(data) = back else {
            panic!("not the list");
        };
        assert_eq!(data.entries[0].viewers, 2);
        assert_eq!(data.entries[0].conditions.wind_from_deg, Some(90));
        let back: ServerMessage = rmp_serde::from_slice(&joined_bytes).unwrap();
        assert!(
            matches!(back, ServerMessage::SpectatorJoined(d) if d.kind == SpectatorKind::Showcase)
        );
        let text = String::from_utf8_lossy(&showcases_bytes);
        assert!(text.contains("TrackName") && text.contains("Viewers"));
    }
    /// The lobby summaries carry each content file's checksum, and a client
    /// that predates the field must still read the message.
    #[test]
    fn test_lobby_summaries_carry_content_crc() {
        let msg = ServerMessage::LobbyState(LobbyStateData {
            players_in_lobby: vec![],
            available_sessions: vec![SessionSummary {
                id: Uuid::nil(),
                track_name: "Monza".into(),
                track_file: "tracks/default/Monza.yaml".into(),
                track_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
                host_name: "host".into(),
                session_kind: SessionKind::Multiplayer,
                player_count: 1,
                max_players: 8,
                state: SessionState::Lobby,
                conditions: SessionConditions::DEFAULT,
                lap_limit: 5,
            }],
            car_configs: vec![CarConfigSummary {
                id: Uuid::nil(),
                name: "Car".into(),
                model_path: String::new(),
                content_crc: 0xCBF4_3926,
                mass_kg: 1000.0,
                max_engine_force_n: 1.0,
            }],
            track_configs: vec![TrackConfigSummary {
                id: Uuid::nil(),
                name: "Monza".into(),
                content_crc: 0xDEAD_BEEF,
                centerline: vec![],
            }],
            showcase_available: true,
        });
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        let text = String::from_utf8_lossy(&bytes);
        assert!(
            text.contains("ContentCrc"),
            "summaries must name the checksum"
        );
        assert!(text.contains("TrackId"), "a session must name its track");
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::LobbyState(l) => {
                assert_eq!(l.car_configs[0].content_crc, 0xCBF4_3926);
                assert_eq!(l.track_configs[0].content_crc, 0xDEAD_BEEF);
                assert_eq!(
                    l.available_sessions[0].track_id.to_string(),
                    "01234567-89ab-cdef-0123-456789abcdef"
                );
            }
            _ => panic!("wrong message type"),
        }

        // A summary written before the field existed decodes with 0.
        let legacy = rmp_serde::to_vec_named(&serde_json::json!({
            "Id": "00000000-0000-0000-0000-000000000000",
            "Name": "Old",
            "Centerline": [],
        }))
        .unwrap();
        let decoded: TrackConfigSummary = rmp_serde::from_slice(&legacy).unwrap();
        assert_eq!(decoded.content_crc, 0);
    }

    /// The exact bytes of a small `RacingLine`, pinned because the Unreal
    /// client parses them by hand: the same array is its golden blob
    /// `ApexGolden::S_RacingLine` (ProtocolCodecTests.cpp). Change both
    /// together.
    #[test]
    fn test_racing_line_wire_format() {
        const GOLDEN: [u8; 133] = [
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAA, 0x52, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x4C,
            0x69, 0x6E, 0x65, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73, 0x73,
            0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31,
            0x32, 0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65,
            0x66, 0xA8, 0x53, 0x70, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x4D, 0xCA, 0x40, 0x20, 0x00,
            0x00, 0xA1, 0x58, 0x92, 0xCA, 0x3F, 0xC0, 0x00, 0x00, 0xCA, 0xC0, 0x00, 0x00, 0x00,
            0xA1, 0x59, 0x92, 0xCA, 0x3E, 0x80, 0x00, 0x00, 0xCA, 0x40, 0x40, 0x00, 0x00, 0xA1,
            0x5A, 0x92, 0xCA, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x3F, 0x80, 0x00, 0x00, 0xA5, 0x50,
            0x68, 0x61, 0x73, 0x65, 0x92, 0x00, 0x02,
        ];
        let msg = ServerMessage::RacingLine(RacingLineData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            spacing_m: 2.5,
            x: vec![1.5, -2.0],
            y: vec![0.25, 3.0],
            z: vec![0.0, 1.0],
            phase: vec![0, 2],
        });
        assert_eq!(rmp_serde::to_vec_named(&msg).unwrap(), GOLDEN);
    }

    /// The exact bytes of a `DriverFeedback` datagram, pinned because the
    /// Unreal client reads them positionally: the same array is its golden
    /// blob `ApexUdpGolden::S_DriverFeedback` (UdpProtocolTests.cpp). Change
    /// both together.
    #[test]
    fn test_driver_feedback_wire_format() {
        const GOLDEN: [u8; 126] = [
            0x92, 0xAE, 0x44, 0x72, 0x69, 0x76, 0x65, 0x72, 0x46, 0x65, 0x65, 0x64, 0x62, 0x61,
            0x63, 0x6B, 0x9D, 0xCD, 0x04, 0xD2, 0x92, 0xCA, 0x3E, 0x80, 0x00, 0x00, 0xCA, 0xBF,
            0x00, 0x00, 0x00, 0x94, 0xCA, 0x3F, 0xC0, 0x00, 0x00, 0xCA, 0xC0, 0x00, 0x00, 0x00,
            0xCA, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x3F, 0x00, 0x00, 0x00, 0x94, 0xCA, 0x00, 0x00,
            0x00, 0x00, 0xCA, 0x3E, 0x80, 0x00, 0x00, 0xCA, 0xBF, 0x80, 0x00, 0x00, 0xCA, 0x40,
            0x40, 0x00, 0x00, 0x94, 0x00, 0x01, 0x02, 0x00, 0x94, 0xCA, 0x3F, 0x00, 0x00, 0x00,
            0xCA, 0xBE, 0x80, 0x00, 0x00, 0xCA, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x40, 0x00, 0x00,
            0x00, 0xC3, 0xC2, 0xCA, 0x40, 0x90, 0x00, 0x00, 0xCA, 0xBF, 0xC0, 0x00, 0x00, 0xCA,
            0x3E, 0x80, 0x00, 0x00, 0xCA, 0xC0, 0x80, 0x00, 0x00, 0xCA, 0x3F, 0xC0, 0x00, 0x00,
        ];
        let msg = ServerMessage::DriverFeedback(DriverFeedback {
            server_tick: 1234,
            steer_torque: vec![0.25, -0.5],
            slip_ratio: [1.5, -2.0, 0.0, 0.5],
            slip_angle: [0.0, 0.25, -1.0, 3.0],
            surface: [0, 1, 2, 0],
            suspension_mps: [0.5, -0.25, 0.0, 2.0],
            abs_active: true,
            tc_active: false,
            impact_mps: 4.5,
            steer_kick: -1.5,
            steer_input: 0.25,
            steer_stiffness: -4.0,
            front_load: 1.5,
        });
        let bytes = rmp_serde::to_vec(&msg).unwrap();
        assert_eq!(bytes, GOLDEN);

        match rmp_serde::from_slice(&bytes).unwrap() {
            ServerMessage::DriverFeedback(decoded) => match msg {
                ServerMessage::DriverFeedback(original) => assert_eq!(decoded, original),
                _ => unreachable!(),
            },
            other => panic!("Wrong message type: {:?}", other),
        }
    }

    /// The positional shape of `CompactCarState`, pinned because the Unreal
    /// client reads it by position: one field inserted rather than appended
    /// and every value after it lands in the wrong place. The bytes are the
    /// client's `ApexUdpGolden::S_TelemetryCompactLapFlags`; print them with
    /// `cargo test telemetry_compact_wire_format -- --nocapture`.
    #[test]
    fn test_telemetry_compact_wire_format() {
        let mut state = CarState::new(
            Uuid::new_v4(),
            Uuid::new_v4(),
            &GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        );
        state.pos_x = 100.5;
        state.pos_y = -20.25;
        state.pos_z = 0.5;
        state.yaw_rad = 1.5;
        state.roll_rad = -0.25;
        state.speed_mps = 42.0;
        state.throttle_input = 1.0;
        state.steering_input = -0.5;
        state.gear = 4;
        state.engine_rpm = 11_000.0;
        state.current_lap = 3;
        state.track_progress = 0.75;
        state.current_lap_time_ms = 91_234;
        state.last_lap_time_ms = Some(82_615);
        state.best_lap_time_ms = Some(82_615);
        state.is_on_track = true;
        state.is_colliding = false;
        state.laps.invalid = true;
        state.fuel_liters = 42.46;
        state.tyres_fitted = true;
        for (tyre, (c, kpa)) in state.tires.each_mut().into_iter().zip([
            (84.4, 176.0),
            (91.6, 181.2),
            (102.5, 190.0),
            (300.0, 0.2),
        ]) {
            tyre.temperature_c = c;
            tyre.pressure_kpa = kpa;
        }
        state.wake.drag = 0.83;
        state.tyre_compound = 0;
        for (tyre, wear) in state
            .tires
            .each_mut()
            .into_iter()
            .zip([12.4, 13.0, 30.6, 99.9])
        {
            tyre.wear_percent = wear;
        }
        state.pit.limiter = true;
        state.pit.in_lane = true;
        state.pit.servicing = true;
        state.pit.service_left_s = 7.34;
        state.brake_temp_c = [612.4, 598.0, 355.5, 350.0];
        state.water_temp_c = 104.6;
        state.damage.front_damage_percent = 23.4;
        state.damage.left_damage_percent = 7.6;
        state.damage.engine_damage_percent = 100.0;
        state.ers_charge_pct = 64;
        state.ers_budget_pct = 255;
        state.ers_mode = crate::hybrid::ErsMode::Attack as u8;
        state.ers_deploying = true;
        state.ers_boost = true;

        let msg = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![CompactCarState::from_car_state(&state, 0)],
        });
        let bytes = rmp_serde::to_vec(&msg).unwrap();
        println!(
            "S_TelemetryCompactErs: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );

        // The car is a 37-field array: 0xDC 0x00 0x25 is the array-16 header.
        assert!(
            bytes.windows(3).any(|w| w == [0xDC, 0x00, 0x25]),
            "CompactCarState must stay 37 fields; the client reads them by position"
        );

        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::TelemetryCompact(frame) => {
                let car = &frame.car_states[0];
                assert_eq!(car.lap_flags, 1, "the lap in progress is struck");
                assert_eq!(car.fuel_dl, 425, "42.46 L goes out as 42.5");
                assert_eq!(car.tyre_c, [84, 92, 103, 255], "rounded, held to a byte");
                assert_eq!(car.tyre_kpa, [176, 181, 190, 1], "0 is kept for unknown");
                assert_eq!(car.tow_pct, 17, "a 17% tow");
                assert_eq!(car.tyre_wear, [12, 13, 31, 100]);
                assert_eq!(car.compound, 0, "softs");
                assert_eq!(car.pit_flags, 7, "in the lane, on the limiter, in service");
                assert_eq!(car.service_ds, 73);
                assert_eq!(car.brake_c, [612, 598, 356, 350]);
                assert_eq!(car.water_c, 105);
                assert_eq!(car.damage, [23, 0, 8, 0, 100]);
                assert_eq!((car.ers_pct, car.ers_lap_pct), (64, 255));
                assert_eq!(car.ers_flags, 2 | 4 | 16, "attack, deploying, boost");
                assert_eq!(car.last_lap_time_ms, Some(82_615));
                assert_eq!(car.gear, 4);
            }
            other => panic!("Wrong message type: {other:?}"),
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

    #[test]
    fn test_create_session_without_allowed_assists_allows_everything() {
        #[derive(Serialize)]
        struct Envelope<T: Serialize> {
            r#type: String,
            data: T,
        }
        // A client from before the rule sends no allowed_assists.
        #[derive(Serialize)]
        struct OldCreate {
            track_config_id: String,
            max_players: u8,
            ai_count: u8,
            lap_limit: u8,
        }
        let legacy = rmp_serde::to_vec_named(&Envelope {
            r#type: "CreateSession".to_string(),
            data: OldCreate {
                track_config_id: Uuid::nil().to_string(),
                max_players: 4,
                ai_count: 0,
                lap_limit: 2,
            },
        })
        .unwrap();
        match rmp_serde::from_slice::<ClientMessage>(&legacy).unwrap() {
            ClientMessage::CreateSession {
                allowed_assists, ..
            } => assert_eq!(allowed_assists, AllowedAssists::ALL),
            _ => panic!("Wrong message type"),
        }

        // And a partial map allows what it does not mention.
        #[derive(Serialize)]
        struct SomeAssists {
            abs: bool,
        }
        #[derive(Serialize)]
        struct PartialCreate {
            track_config_id: String,
            max_players: u8,
            ai_count: u8,
            lap_limit: u8,
            allowed_assists: SomeAssists,
        }
        let partial = rmp_serde::to_vec_named(&Envelope {
            r#type: "CreateSession".to_string(),
            data: PartialCreate {
                track_config_id: Uuid::nil().to_string(),
                max_players: 4,
                ai_count: 0,
                lap_limit: 2,
                allowed_assists: SomeAssists { abs: false },
            },
        })
        .unwrap();
        match rmp_serde::from_slice::<ClientMessage>(&partial).unwrap() {
            ClientMessage::CreateSession {
                allowed_assists, ..
            } => assert_eq!(
                allowed_assists,
                AllowedAssists {
                    abs: false,
                    ..AllowedAssists::ALL
                }
            ),
            _ => panic!("Wrong message type"),
        }

        // An old SessionJoined has no allowed assists either.
        #[derive(Serialize)]
        #[serde(rename_all = "PascalCase")]
        struct OldJoined {
            session_id: String,
            your_grid_position: u8,
        }
        let joined = rmp_serde::to_vec_named(&OldJoined {
            session_id: Uuid::nil().to_string(),
            your_grid_position: 1,
        })
        .unwrap();
        let decoded: SessionJoinedData = rmp_serde::from_slice(&joined).unwrap();
        assert_eq!(decoded.allowed_assists, AllowedAssists::ALL);
    }

    /// The exact bytes the Unreal client's codec must produce and parse for
    /// the assists messages: the same arrays are its golden blobs
    /// `ApexGolden::C_CreateSession`, `C_SetDriverAids` and
    /// `S_SessionJoinedAssists` (ProtocolCodecTests.cpp). Change both
    /// together; `cargo test assists_wire_format -- --nocapture` prints them.
    /// Golden bytes for the lap-timing messages, the way the assists and the
    /// car setup are pinned: run
    /// `cargo test lap_timing_wire_format -- --nocapture` and paste the
    /// output into the client's `ApexGoldenBlobs.h`.
    #[test]
    fn test_lap_timing_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let session_id = Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap();
        let track_id = Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap();
        let car_id = Uuid::parse_str("11111111-2222-3333-4444-555555555555").unwrap();

        let sectors = ServerMessage::TrackSectors(TrackSectorsData {
            session_id,
            track_length_m: 5793.0,
            boundaries_m: vec![1931.0, 3862.0],
        });
        let sector_bytes = rmp_serde::to_vec_named(&sectors).unwrap();
        println!("S_TrackSectors: {}", hex(&sector_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&sector_bytes).unwrap() {
            ServerMessage::TrackSectors(data) => {
                assert_eq!(data.boundaries_m, vec![1931.0, 3862.0]);
                assert_eq!(data.track_length_m, 5793.0);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        let timing = ServerMessage::LapTiming(LapTimingData {
            car_index: 2,
            lap: 4,
            sector: 2,
            sector_time_ms: 27_431,
            lap_time_ms: 82_615,
            is_lap_end: true,
            valid: true,
            flags: LapTimingData::FLAG_PERSONAL_BEST_LAP | LapTimingData::FLAG_SESSION_BEST_SECTOR,
        });
        let timing_bytes = rmp_serde::to_vec_named(&timing).unwrap();
        println!("S_LapTiming: {}", hex(&timing_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&timing_bytes).unwrap() {
            ServerMessage::LapTiming(data) => {
                assert!(data.is_lap_end && data.valid);
                assert_eq!(data.lap_time_ms, 82_615);
                assert_eq!(data.flags & LapTimingData::FLAG_PERSONAL_BEST_LAP, 1);
                assert_eq!(data.flags & LapTimingData::FLAG_SESSION_BEST_LAP, 0);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        let record = ServerMessage::LapRecord(LapRecordData {
            player_name: "Ayrton".to_string(),
            track_id,
            car_config_id: car_id,
            lap_time_ms: 82_615,
            splits_ms: vec![27_100, 28_084, 27_431],
            is_new: true,
            track_record_ms: 81_900,
            track_record_holder: "Alain".to_string(),
            has_ghost: true,
        });
        let record_bytes = rmp_serde::to_vec_named(&record).unwrap();
        println!("S_LapRecord: {}", hex(&record_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&record_bytes).unwrap() {
            ServerMessage::LapRecord(data) => {
                assert!(data.is_new && data.has_ghost);
                assert_eq!(data.splits_ms.iter().sum::<u32>(), 82_615);
                assert_eq!(data.track_record_holder, "Alain");
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    #[test]
    fn test_telemetry_carries_lap_flags() {
        let mut state = CarState::new(
            Uuid::new_v4(),
            Uuid::new_v4(),
            &GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        );
        state.laps.invalid = true;
        state.laps.last_invalid = false;
        let compact = CompactCarState::from_car_state(&state, 3);
        assert_eq!(compact.lap_flags, 1, "bit 0 is the lap in progress");
        let full = CarStateTelemetry::from(&state);
        assert_eq!(full.lap_flags, 1);

        state.laps.invalid = false;
        state.laps.last_invalid = true;
        assert_eq!(
            CompactCarState::from_car_state(&state, 3).lap_flags,
            2,
            "bit 1 is the lap just completed"
        );
    }

    #[test]
    fn test_assists_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists {
                abs: true,
                traction_control: false,
                auto_gearbox: true,
                steering_assist: false,
                racing_line: true,
                damage: true,
            },
            conditions: SessionConditions {
                weather: Weather::LightRain,
                time_of_day_minutes: 21 * 60 + 30,
                ..SessionConditions::DEFAULT
            },
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSession: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession {
                allowed_assists,
                conditions,
                ..
            } => {
                assert!(!allowed_assists.traction_control && allowed_assists.abs);
                assert_eq!(conditions.weather, Weather::LightRain);
                assert_eq!(conditions.time_of_day_minutes, 1290);
            }
            _ => panic!("Wrong message type"),
        }

        let aids = ClientMessage::SetDriverAids {
            auto_gearbox: true,
            steering_assist: true,
            abs: Some(false),
            traction_control: Some(TractionControl::High),
            damage: None,
        };
        let aids_bytes = rmp_serde::to_vec_named(&aids).unwrap();
        println!("C_SetDriverAids: {}", hex(&aids_bytes));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists {
                abs: false,
                traction_control: true,
                auto_gearbox: false,
                steering_assist: true,
                racing_line: false,
                damage: true,
            },
            conditions: SessionConditions {
                weather: Weather::HeavyRain,
                time_of_day_minutes: 6 * 60 + 15,
                ..SessionConditions::DEFAULT
            },
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedAssists: {}", hex(&joined_bytes));

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION);
        assert_eq!(aids_bytes, GOLDEN_C_SET_DRIVER_AIDS);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_ASSISTS);
    }

    /// The damage aid: a `SetDriverAids` that asks for reduced damage, and
    /// a session that forbids it (the only time `AllowedAssists` names
    /// `damage`). Pinned on the client as `ApexGolden::C_SetDriverAidsDamage`
    /// / `S_SessionJoinedNoDamage`; `cargo test damage_assist_wire_format --
    /// --nocapture` prints them.
    #[test]
    fn test_damage_assist_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let aids = ClientMessage::SetDriverAids {
            auto_gearbox: false,
            steering_assist: false,
            abs: Some(true),
            traction_control: Some(TractionControl::Low),
            damage: Some(DamageLevel::Reduced),
        };
        let aids_bytes = rmp_serde::to_vec_named(&aids).unwrap();
        println!("C_SetDriverAidsDamage: {}", hex(&aids_bytes));

        let no_damage = AllowedAssists {
            damage: false,
            ..AllowedAssists::ALL
        };
        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Practice,
            allowed_assists: no_damage,
            conditions: SessionConditions::DEFAULT,
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedNoDamage: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.allowed_assists, no_damage),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(aids_bytes, GOLDEN_C_SET_DRIVER_AIDS_DAMAGE);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_NO_DAMAGE);
    }

    /// The session's air named in full: a create that picks every figure
    /// (a frost, so the signed temperature is pinned too) and the joined
    /// echo of a resolved session. Pinned on the client as
    /// `ApexGolden::C_CreateSessionAir` / `S_SessionJoinedAir`;
    /// `cargo test conditions_air_wire_format -- --nocapture` prints them.
    #[test]
    fn test_conditions_air_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let air = SessionConditions {
            weather: Weather::Overcast,
            time_of_day_minutes: 8 * 60,
            air_temp_c: Some(-3),
            humidity_pct: Some(80),
            wind_kph: Some(22),
            wind_from_deg: Some(270),
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: air,
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionAir: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { conditions, .. } => assert_eq!(conditions, air),
            _ => panic!("Wrong message type"),
        }

        // A session names every figure it left to the weather.
        let resolved = SessionConditions {
            weather: Weather::Sunny,
            time_of_day_minutes: 15 * 60,
            ..SessionConditions::DEFAULT
        }
        .resolve(7);
        assert_eq!(resolved.air_temp_c, Some(23));
        assert_eq!(resolved.humidity_pct, Some(50));
        assert_eq!(resolved.wind_kph, Some(8));
        assert!(resolved.wind_from_deg.is_some_and(|d| d < 360));
        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::default(),
            conditions: air,
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedAir: {}", hex(&joined_bytes));
    }

    /// The bytes of the hotlap messages, pinned on the client as
    /// `ApexGolden::C_HotlapRelocate`, `C_RequestGhost` and `S_GhostLap`;
    /// `cargo test hotlap_wire_format -- --nocapture` prints them.
    #[test]
    fn test_hotlap_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let relocate = ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Track,
            cold_tyres: false,
        };
        let relocate_bytes = rmp_serde::to_vec_named(&relocate).unwrap();
        println!("C_HotlapRelocate: {}", hex(&relocate_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&relocate_bytes).unwrap() {
            ClientMessage::HotlapRelocate {
                destination,
                cold_tyres,
            } => {
                assert_eq!(destination, HotlapDestination::Track);
                assert!(!cold_tyres);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
        // The cold-tyres flag is only written when set (`C_HotlapRelocateCold`).
        assert!(!String::from_utf8_lossy(&relocate_bytes).contains("cold"));
        let cold = ClientMessage::HotlapRelocate {
            destination: HotlapDestination::Track,
            cold_tyres: true,
        };
        let cold_bytes = rmp_serde::to_vec_named(&cold).unwrap();
        println!("C_HotlapRelocateCold: {}", hex(&cold_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&cold_bytes).unwrap() {
            ClientMessage::HotlapRelocate { cold_tyres, .. } => assert!(cold_tyres),
            other => panic!("Wrong message type: {other:?}"),
        }

        let request_bytes = rmp_serde::to_vec_named(&ClientMessage::RequestGhost).unwrap();
        println!("C_RequestGhost: {}", hex(&request_bytes));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&request_bytes).unwrap(),
            ClientMessage::RequestGhost
        ));

        let lap = crate::records::GhostLap {
            lap_time_ms: 82_615,
            sample_hz: 20.0,
            samples: vec![
                crate::records::GhostSample {
                    t_ms: 0,
                    x: 1.5,
                    y: -2.0,
                    z: 0.25,
                    yaw_rad: 0.5,
                    pitch_rad: 0.0,
                    roll_rad: -0.125,
                    speed_mps: 60.0,
                    steering: 0.1,
                    throttle: 1.0,
                    brake: 0.0,
                    gear: 4,
                    engine_rpm: 9000.0,
                },
                crate::records::GhostSample {
                    t_ms: 50,
                    x: 4.5,
                    y: -2.5,
                    z: 0.25,
                    yaw_rad: 0.5,
                    pitch_rad: 0.0,
                    roll_rad: 0.0,
                    speed_mps: 61.0,
                    steering: -0.2,
                    throttle: 1.0,
                    brake: 0.0,
                    gear: -1,
                    engine_rpm: 9100.0,
                },
            ],
        };
        let ghost = ServerMessage::GhostLap(GhostLapData::from_lap(
            Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            Uuid::parse_str("11111111-2222-3333-4444-555555555555").unwrap(),
            &lap,
        ));
        let ghost_bytes = rmp_serde::to_vec_named(&ghost).unwrap();
        println!("S_GhostLap: {}", hex(&ghost_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&ghost_bytes).unwrap() {
            ServerMessage::GhostLap(data) => {
                assert_eq!(data.sample_count(), 2);
                assert_eq!(data.lap_time_ms, 82_615);
                assert_eq!(data.pose.len(), 12);
                assert_eq!(data.pose[6], 4.5);
                assert_eq!(data.t_ms, vec![0, 50]);
                assert_eq!(data.gear, vec![4, -1]);
                assert_eq!(data.steering, vec![0.1, -0.2]);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        // A ghost is big: a five-minute lap must still fit the frame limit.
        let long = crate::records::GhostLap {
            lap_time_ms: 300_000,
            sample_hz: 20.0,
            samples: vec![lap.samples[0]; 20 * 300],
        };
        let long_bytes = rmp_serde::to_vec_named(&ServerMessage::GhostLap(GhostLapData::from_lap(
            Uuid::nil(),
            Uuid::nil(),
            &long,
        )))
        .unwrap();
        assert!(
            long_bytes.len() < 1_000_000,
            "a five-minute ghost is {} bytes",
            long_bytes.len()
        );
    }

    /// The bytes of a `SetCarSetup` with every knob named, the client's
    /// `ApexGolden::C_SetCarSetup`; `cargo test car_setup_wire_format --
    /// --nocapture` prints them. Negative clicks are msgpack negative
    /// fixints, so the client must write them as signed ints.
    #[test]
    fn test_car_setup_wire_format() {
        let setup = ClientMessage::SetCarSetup(CarSetup::from_clicks([
            1, -2, -3, 4, -5, 5, -1, 2, 3, -3, 0, 1, -4, 4, -2, 2, -1, -3, 1, 1, 2, -1, 2, 1, -2,
        ]));
        let bytes = rmp_serde::to_vec_named(&setup).unwrap();
        let hex = bytes
            .iter()
            .map(|b| format!("0x{:02X}", b))
            .collect::<Vec<_>>()
            .join(", ");
        println!("C_SetCarSetup: {}", hex);
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::SetCarSetup(decoded) => {
                assert_eq!(decoded.tyre_pressure_rear, -2);
                assert_eq!(decoded.anti_roll_rear, 4);
                assert_eq!(decoded.fuel_load, -2);
                assert_eq!((decoded.front_wing, decoded.rear_wing), (2, -1));
                assert_eq!(decoded.ride_height_rear, 1);
                assert_eq!(decoded.tyre_compound, 1);
                assert_eq!(decoded.brake_ducts, 2);
                assert_eq!((decoded.camber_front, decoded.camber_rear), (-1, 2));
                assert_eq!((decoded.toe_front, decoded.toe_rear), (1, -2));
            }
            _ => panic!("Wrong message type"),
        }
        // A partial message (an older client, or one that only names what
        // it changed) leaves the other knobs stock.
        let partial = rmp_serde::from_slice::<ClientMessage>(
            &rmp_serde::to_vec_named(&serde_json::json!({
                "type": "SetCarSetup",
                "data": { "brake_bias": -2 }
            }))
            .unwrap(),
        )
        .unwrap();
        match partial {
            ClientMessage::SetCarSetup(decoded) => {
                assert_eq!(decoded.brake_bias, -2);
                assert_eq!(decoded.spring_front, 0);
            }
            _ => panic!("Wrong message type"),
        }
        assert_eq!(bytes, GOLDEN_C_SET_CAR_SETUP);
    }

    /// The bytes of a small `CarSetupSheet`, the client's
    /// `ApexGolden::S_CarSetupSheet`; `cargo test car_setup_sheet_wire_format
    /// -- --nocapture` prints them.
    #[test]
    fn test_car_setup_sheet_wire_format() {
        use crate::setup_sheet::{CarSetupSheetData, SetupKnobFigure};
        let sheet = ServerMessage::CarSetupSheet(CarSetupSheetData {
            session_id: uuid::Uuid::from_u128(1),
            car_config_id: uuid::Uuid::from_u128(2),
            knobs: vec![
                SetupKnobFigure {
                    stock: 26.0,
                    step: 0.5,
                    lo: f32::MIN,
                    hi: f32::MAX,
                    decimals: 1,
                    unit: "psi".to_string(),
                },
                SetupKnobFigure {
                    stock: 58.0,
                    step: 2.0,
                    lo: 10.0,
                    hi: f32::MAX,
                    decimals: 0,
                    unit: "mm".to_string(),
                },
            ],
            gear_ratios: vec![3.5, 1.0],
            final_drive: 3.75,
            wheel_radius_m: 0.25,
            tyre_optimal_psi: 27.5,
            lap_fuel_l: 2.5,
            fuel_kg_per_l: 0.75,
            fill_laps: 3.0,
            camber_modelled: true,
            rake_balance_per_mm: 0.001,
        });
        let bytes = rmp_serde::to_vec_named(&sheet).unwrap();
        let hex = bytes
            .iter()
            .map(|b| format!("0x{:02X}", b))
            .collect::<Vec<_>>()
            .join(", ");
        println!("S_CarSetupSheet: {}", hex);
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::CarSetupSheet(decoded) => {
                assert_eq!(decoded.knobs.len(), 2);
                assert_eq!(decoded.knobs[1].unit, "mm");
                assert_eq!(decoded.gear_ratios, vec![3.5, 1.0]);
                assert!(decoded.camber_modelled);
            }
            _ => panic!("Wrong message type"),
        }
        assert_eq!(bytes, GOLDEN_S_CAR_SETUP_SHEET);
    }

    const GOLDEN_S_CAR_SETUP_SHEET: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x61, 0x72, 0x53, 0x65, 0x74, 0x75, 0x70,
        0x53, 0x68, 0x65, 0x65, 0x74, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x8C, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30,
        0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0xAB,
        0x43, 0x61, 0x72, 0x43, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x30,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30,
        0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x30, 0x30, 0x32, 0xA5, 0x4B, 0x6E, 0x6F, 0x62, 0x73, 0x92, 0x86, 0xA5, 0x53, 0x74,
        0x6F, 0x63, 0x6B, 0xCA, 0x41, 0xD0, 0x00, 0x00, 0xA4, 0x53, 0x74, 0x65, 0x70, 0xCA, 0x3F,
        0x00, 0x00, 0x00, 0xA2, 0x4C, 0x6F, 0xCA, 0xFF, 0x7F, 0xFF, 0xFF, 0xA2, 0x48, 0x69, 0xCA,
        0x7F, 0x7F, 0xFF, 0xFF, 0xA8, 0x44, 0x65, 0x63, 0x69, 0x6D, 0x61, 0x6C, 0x73, 0x01, 0xA4,
        0x55, 0x6E, 0x69, 0x74, 0xA3, 0x70, 0x73, 0x69, 0x86, 0xA5, 0x53, 0x74, 0x6F, 0x63, 0x6B,
        0xCA, 0x42, 0x68, 0x00, 0x00, 0xA4, 0x53, 0x74, 0x65, 0x70, 0xCA, 0x40, 0x00, 0x00, 0x00,
        0xA2, 0x4C, 0x6F, 0xCA, 0x41, 0x20, 0x00, 0x00, 0xA2, 0x48, 0x69, 0xCA, 0x7F, 0x7F, 0xFF,
        0xFF, 0xA8, 0x44, 0x65, 0x63, 0x69, 0x6D, 0x61, 0x6C, 0x73, 0x00, 0xA4, 0x55, 0x6E, 0x69,
        0x74, 0xA2, 0x6D, 0x6D, 0xAA, 0x47, 0x65, 0x61, 0x72, 0x52, 0x61, 0x74, 0x69, 0x6F, 0x73,
        0x92, 0xCA, 0x40, 0x60, 0x00, 0x00, 0xCA, 0x3F, 0x80, 0x00, 0x00, 0xAA, 0x46, 0x69, 0x6E,
        0x61, 0x6C, 0x44, 0x72, 0x69, 0x76, 0x65, 0xCA, 0x40, 0x70, 0x00, 0x00, 0xAC, 0x57, 0x68,
        0x65, 0x65, 0x6C, 0x52, 0x61, 0x64, 0x69, 0x75, 0x73, 0x4D, 0xCA, 0x3E, 0x80, 0x00, 0x00,
        0xAE, 0x54, 0x79, 0x72, 0x65, 0x4F, 0x70, 0x74, 0x69, 0x6D, 0x61, 0x6C, 0x50, 0x73, 0x69,
        0xCA, 0x41, 0xDC, 0x00, 0x00, 0xA8, 0x4C, 0x61, 0x70, 0x46, 0x75, 0x65, 0x6C, 0x4C, 0xCA,
        0x40, 0x20, 0x00, 0x00, 0xAA, 0x46, 0x75, 0x65, 0x6C, 0x4B, 0x67, 0x50, 0x65, 0x72, 0x4C,
        0xCA, 0x3F, 0x40, 0x00, 0x00, 0xA8, 0x46, 0x69, 0x6C, 0x6C, 0x4C, 0x61, 0x70, 0x73, 0xCA,
        0x40, 0x40, 0x00, 0x00, 0xAE, 0x43, 0x61, 0x6D, 0x62, 0x65, 0x72, 0x4D, 0x6F, 0x64, 0x65,
        0x6C, 0x6C, 0x65, 0x64, 0xC3, 0xB0, 0x52, 0x61, 0x6B, 0x65, 0x42, 0x61, 0x6C, 0x61, 0x6E,
        0x63, 0x65, 0x50, 0x65, 0x72, 0x4D, 0x6D, 0xCA, 0x3A, 0x83, 0x12, 0x6F,
    ];

    const GOLDEN_C_SET_CAR_SETUP: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAB, 0x53, 0x65, 0x74, 0x43, 0x61, 0x72, 0x53, 0x65,
        0x74, 0x75, 0x70, 0xA4, 0x64, 0x61, 0x74, 0x61, 0xDE, 0x00, 0x19, 0xB3, 0x74, 0x79, 0x72,
        0x65, 0x5F, 0x70, 0x72, 0x65, 0x73, 0x73, 0x75, 0x72, 0x65, 0x5F, 0x66, 0x72, 0x6F, 0x6E,
        0x74, 0x01, 0xB2, 0x74, 0x79, 0x72, 0x65, 0x5F, 0x70, 0x72, 0x65, 0x73, 0x73, 0x75, 0x72,
        0x65, 0x5F, 0x72, 0x65, 0x61, 0x72, 0xFE, 0xAB, 0x72, 0x65, 0x76, 0x5F, 0x6C, 0x69, 0x6D,
        0x69, 0x74, 0x65, 0x72, 0xFD, 0xAE, 0x65, 0x6E, 0x67, 0x69, 0x6E, 0x65, 0x5F, 0x62, 0x72,
        0x61, 0x6B, 0x69, 0x6E, 0x67, 0x04, 0xAB, 0x66, 0x69, 0x6E, 0x61, 0x6C, 0x5F, 0x64, 0x72,
        0x69, 0x76, 0x65, 0xFB, 0xAB, 0x67, 0x65, 0x61, 0x72, 0x5F, 0x73, 0x70, 0x72, 0x65, 0x61,
        0x64, 0x05, 0xAA, 0x74, 0x6F, 0x72, 0x71, 0x75, 0x65, 0x5F, 0x6D, 0x61, 0x70, 0xFF, 0xAA,
        0x62, 0x72, 0x61, 0x6B, 0x65, 0x5F, 0x62, 0x69, 0x61, 0x73, 0x02, 0xAC, 0x73, 0x70, 0x72,
        0x69, 0x6E, 0x67, 0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x03, 0xAB, 0x73, 0x70, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x72, 0x65, 0x61, 0x72, 0xFD, 0xAC, 0x64, 0x61, 0x6D, 0x70, 0x65, 0x72,
        0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x00, 0xAB, 0x64, 0x61, 0x6D, 0x70, 0x65, 0x72, 0x5F,
        0x72, 0x65, 0x61, 0x72, 0x01, 0xAF, 0x61, 0x6E, 0x74, 0x69, 0x5F, 0x72, 0x6F, 0x6C, 0x6C,
        0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0xFC, 0xAE, 0x61, 0x6E, 0x74, 0x69, 0x5F, 0x72, 0x6F,
        0x6C, 0x6C, 0x5F, 0x72, 0x65, 0x61, 0x72, 0x04, 0xA9, 0x66, 0x75, 0x65, 0x6C, 0x5F, 0x6C,
        0x6F, 0x61, 0x64, 0xFE, 0xAA, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x5F, 0x77, 0x69, 0x6E, 0x67,
        0x02, 0xA9, 0x72, 0x65, 0x61, 0x72, 0x5F, 0x77, 0x69, 0x6E, 0x67, 0xFF, 0xB1, 0x72, 0x69,
        0x64, 0x65, 0x5F, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74,
        0xFD, 0xB0, 0x72, 0x69, 0x64, 0x65, 0x5F, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x5F, 0x72,
        0x65, 0x61, 0x72, 0x01, 0xAD, 0x74, 0x79, 0x72, 0x65, 0x5F, 0x63, 0x6F, 0x6D, 0x70, 0x6F,
        0x75, 0x6E, 0x64, 0x01, 0xAB, 0x62, 0x72, 0x61, 0x6B, 0x65, 0x5F, 0x64, 0x75, 0x63, 0x74,
        0x73, 0x02, 0xAC, 0x63, 0x61, 0x6D, 0x62, 0x65, 0x72, 0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74,
        0xFF, 0xAB, 0x63, 0x61, 0x6D, 0x62, 0x65, 0x72, 0x5F, 0x72, 0x65, 0x61, 0x72, 0x02, 0xA9,
        0x74, 0x6F, 0x65, 0x5F, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x01, 0xA8, 0x74, 0x6F, 0x65, 0x5F,
        0x72, 0x65, 0x61, 0x72, 0xFE,
    ];

    const GOLDEN_C_CREATE_SESSION: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x87, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC2, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC2, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x03, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x05,
        0x0A,
    ];
    const GOLDEN_C_SET_DRIVER_AIDS: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x74, 0x44, 0x72, 0x69, 0x76, 0x65,
        0x72, 0x41, 0x69, 0x64, 0x73, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x84, 0xAC, 0x61, 0x75, 0x74,
        0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65,
        0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xA3, 0x61, 0x62,
        0x73, 0xC2, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E,
        0x74, 0x72, 0x6F, 0x6C, 0x02,
    ];
    const GOLDEN_C_SET_DRIVER_AIDS_DAMAGE: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x74, 0x44, 0x72, 0x69, 0x76, 0x65,
        0x72, 0x41, 0x69, 0x64, 0x73, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x85, 0xAC, 0x61, 0x75, 0x74,
        0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC2, 0xAF, 0x73, 0x74, 0x65, 0x65,
        0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC2, 0xA3, 0x61, 0x62,
        0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E,
        0x74, 0x72, 0x6F, 0x6C, 0x01, 0xA6, 0x64, 0x61, 0x6D, 0x61, 0x67, 0x65, 0x01,
    ];
    const GOLDEN_S_SESSION_JOINED_NO_DAMAGE: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x85, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x86, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xA6, 0x64, 0x61, 0x6D, 0x61, 0x67, 0x65,
        0xC2, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77,
        0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66,
        0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C,
    ];
    const GOLDEN_S_SESSION_JOINED_ASSISTS: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x85, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC2, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC2, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC2, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x04, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x01, 0x77,
    ];
}
