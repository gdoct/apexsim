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
///
/// Still v2 with sealed inbound UDP (`crate::udp_seal`): a client from
/// before it never gets its bare handshake acked and stays on TCP
/// telemetry, and a client after it gets no `udp_key` from an older server
/// and does the same, so neither pairing needs refusing at `Authenticate`.
pub const PROTOCOL_VERSION: u8 = 2;

/// How a watched hotlap is watched and what its car runs (`CreateSession`'s
/// `watch`).
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, Deserialize)]
pub struct HotlapWatchOptions {
    /// Asked a server to stream the watch to its creator; read by nothing
    /// since the game simulates its own watch (kept so older clients'
    /// messages decode).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub stream: bool,
    /// The setup the AI car runs (clamped as `SetCarSetup` is); stock when
    /// absent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub setup: Option<CarSetup>,
}

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
        /// The `resume_token` an earlier `AuthSuccess` from this server
        /// handed out: presented again, the player gets their old player id
        /// back, and with it any seat they hold in a session. Left off the
        /// wire when the client has none.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        resume_token: Option<String>,
        /// The driver profile's id (`DriverId`), made once when the player
        /// created the profile: the server knows them by it across
        /// connections (grid references, qualifying results, lap records).
        /// Left off the wire by a client without profiles.
        #[serde(
            default,
            skip_serializing_if = "Option::is_none",
            serialize_with = "serialize_option_uuid_as_string",
            deserialize_with = "deserialize_option_uuid_from_string"
        )]
        driver_id: Option<DriverId>,
        /// The profile's Ed25519 public key, hex (32 bytes). With it the
        /// server answers `AuthSuccess.key_challenge`, and the id counts
        /// only once `ProveDriverKey` has signed it. Left off the wire
        /// without one.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        driver_key: Option<String>,
    },
    /// The signature, hex (64 bytes), of the profile's key over
    /// `apexsim driver key v1:<key_challenge>:<driver_id>`: the driver id
    /// is this connection's from then on (`driver_keys.rs`). Sent once,
    /// right after `AuthSuccess`.
    ProveDriverKey {
        signature: String,
    },
    Heartbeat {
        client_tick: u32,
        /// The round trip of the client's last heartbeat, ms, for the
        /// dashboard's player list. Left off the wire before the first.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        ping_ms: Option<u32>,
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
        /// How much damage every car takes, AI included; full when absent,
        /// and left off the wire when full so older bytes are unchanged.
        #[serde(default, skip_serializing_if = "DamageLevel::is_full")]
        damage: DamageLevel,
        /// The AI field's level (`ai_driver::MIN_SKILL_LEVEL` to
        /// `MAX_SKILL_LEVEL`, see `ai_driver::field_skills`); the mixed
        /// field, novice to ace, when absent, and left off the wire then.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        ai_skill: Option<u8>,
        /// A timed race: its length in seconds from the green light
        /// (`data::clamp_race_seconds`), `lap_limit` then ignored. Absent
        /// for a race over laps, and left off the wire then.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        race_seconds: Option<u32>,
        /// The race's start order, first to last, as driver references
        /// (`crate::grid_order`: `@host`, `@ai:N` or a name); absent or empty
        /// for the order cars were seated in (or the session's own
        /// qualifying). Left off the wire when empty.
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        grid_order: Vec<String>,
        /// A timed qualifying session: its length in seconds from entering
        /// `GameMode::Qualification` (`data::clamp_qualifying_seconds`), after
        /// which the chequered flag is out. Absent for open-ended qualifying,
        /// and left off the wire then.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        qualifying_seconds: Option<u32>,
        /// A race weekend (`crate::weekend`): optional timed practice, one
        /// to three knockout qualifying rounds, then the race, advanced by
        /// the host (`AdvanceWeekend`). Clamped on create; with it the
        /// top-level `qualifying_seconds` is ignored. Left off the wire for
        /// a plain session.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        weekend: Option<crate::weekend::WeekendFormat>,
        /// A watched hotlap's own choices (`SessionKind::HotlapWatch`
        /// only, read by the game's `hotlap_watch::LocalWatch`; a server
        /// refuses the kind): the setup the AI car runs. Left off the wire
        /// when absent.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        watch: Option<HotlapWatchOptions>,
        /// The qualifying cut-off, percent of pole (107 for the 107% rule,
        /// `data::clamp_qualifying_cutoff`): a car slower in qualifying, or
        /// with no time, does not start the race. Absent for none, and left
        /// off the wire then.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        qualifying_cutoff_pct: Option<u16>,
        /// The race's mandatory pit stops (`data::PitRule`, at most
        /// `data::MAX_MANDATORY_STOPS`): a car that takes the flag owing one
        /// is classified behind every car that made them. 0 for none, and
        /// left off the wire then.
        #[serde(default, skip_serializing_if = "is_zero_u8")]
        mandatory_stops: u8,
        /// The race must be run on two dry compounds (`data::PitRule`;
        /// waived for a car that runs a treaded tyre). Left off the wire
        /// when false.
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        compound_change: bool,
        /// How qualifying runs (`data::QualifyingFormat`): 1 for a one-lap
        /// shoot-out (a weekend's final round). Left off the wire for the
        /// usual session (0).
        #[serde(default, skip_serializing_if = "QualifyingFormat::is_open")]
        qualifying_format: QualifyingFormat,
        /// The most flying laps a car may start in a qualifying session or
        /// round (`data::clamp_qualifying_lap_allowance`); 0 for no limit,
        /// and left off the wire then.
        #[serde(default, skip_serializing_if = "is_zero_u8")]
        qualifying_lap_allowance: u8,
        /// The race-control rules (`data::RaceRules`: the start, false
        /// starts), keys snake_case, each left off at its default; the whole
        /// table left off the wire when nothing is set.
        #[serde(default, skip_serializing_if = "RaceRules::is_default")]
        race_rules: RaceRules,
        /// A multi-class field (`data::ClassEntry`, at most
        /// `MAX_CLASSES`): each class's AI count and level. With two or
        /// more the server deals the AI per class (`ai_count` is then the
        /// total, which a server from before the field races in the host
        /// car's class), grids by class, classifies in class, and flags
        /// and yields across classes. Empty, and left off the wire, for
        /// the host car's class alone.
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        classes: Vec<ClassEntry>,
    },
    /// Asks for a track's fastest laps (`records::RecordStore::leaderboard`):
    /// each driver's best with each car, or in one car (`car_config_id`).
    /// Answered with `Leaderboard`.
    RequestLeaderboard {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        track_config_id: TrackConfigId,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        car_config_id: Option<String>,
    },
    /// Asks for the qualifying results the server holds for a track, newest
    /// first, to grid a race from. Answered with `QualifyingResults`.
    RequestQualifyingResults {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        track_config_id: TrackConfigId,
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
        /// The pit autopilot; `false` leaves the pit lane to the driver.
        /// Absent (an older client) keeps it on.
        #[serde(default)]
        pit_autopilot: Option<bool>,
    },
    /// The garage setup for this player's car, as clicks per knob
    /// (`car_setup::CarSetup`). Sent on joining a session and whenever a
    /// knob changes; the server clamps every knob into its range and bakes
    /// the result into the car this driver is simulated with. Ignored
    /// outside a session; an all-zero setup is the car as filed.
    SetCarSetup(CarSetup),
    /// The driver's orders for their next pit stop (`pit::PitPlan`): the
    /// tyres, the fuel, the repairs, the brake pads, and whether they box
    /// this lap. Sent at any time in a session and kept until the next
    /// one; each field at its default is the crew's own choice, so a driver
    /// who never sends one is serviced as before. Ignored outside a
    /// session. The request comes back in telemetry (`PIT_FLAG_REQUESTED`).
    SetPitPlan(crate::pit::PitPlan),
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
    /// Watch a live session as a spectator stream (`crate::session_stream`):
    /// the same records a showcase or a replay is made of, with the lap
    /// timing and pit stops so far and the road as it is caught up first.
    /// Answered with `SpectatorJoined { kind: Live }` and the stream's
    /// records, or an `Error` for an unknown session. Not a seat in the
    /// session: the viewer is not a participant. Ended by `LeaveSpectate`.
    SpectateSession {
        #[serde(
            serialize_with = "serialize_uuid_as_string",
            deserialize_with = "deserialize_uuid_from_string"
        )]
        session_id: SessionId,
    },
    /// A driver whose car is stuck asks for it back (`crate::recovery`):
    /// onto the track where it is, or to its pit box (the garage in a
    /// hotlap or qualifying session), held there for the time cost.
    /// Refused with an `Error` while the car is moving or cannot be
    /// recovered; the hold comes back in telemetry (`recover_ds`).
    RecoverCar {
        destination: crate::recovery::RecoverDestination,
    },
    Disconnect,
    /// The host moves a race weekend on to its next phase: from the lobby
    /// to practice (or Q1 without practice), from a finished practice to
    /// Q1, from a finished round to the next, from the finished final round
    /// to the race's countdown. `Error 403` for anyone else; `Error 409`
    /// while a phase runs, once the race has begun, or outside a weekend.
    AdvanceWeekend,
    /// Give up the seat held for this player in a session they lost the
    /// connection to (the menu's rejoin banner): the car leaves the
    /// session. Nothing happens when no seat is held.
    GiveUpSeat,
    /// The host puts the race's grid up (`GameMode::Grid`): the cars line
    /// up on their slots and wait for every driver's `SetReady`, then count
    /// `countdown_seconds` into the race. `Error 403` for anyone else.
    OpenGrid {
        countdown_seconds: u16,
    },
    /// A driver on the grid is ready to start, or not any more. Ignored
    /// outside `GameMode::Grid`; the roster carries it (`RosterEntry.Ready`).
    SetReady {
        ready: bool,
    },
    /// The livery this driver's car wears from now on, in a session (as
    /// `SelectCar.livery` picks it before one): 0 the car as authored, 1..
    /// its `[[livery]]` tables. The roster carries it to everyone.
    SetLivery {
        livery: u8,
    },
    /// A predicting client asks for one chunk of a track's physics bundle
    /// (`crate::track_bundle`), named by the checksum `SessionJoined.Physics`
    /// carried: answered with `TrackBundleChunk`. The client asks for the
    /// next as each arrives, a few at a time, so a bundle of megabytes never
    /// fills the connection's queue ahead of the session's own messages.
    RequestTrackBundle {
        crc: u32,
        chunk: u32,
    },

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
        /// The handbrake is held (`physics::HANDBRAKE_SHARE`). A client from
        /// before the field never brakes by hand.
        #[serde(default)]
        handbrake: Option<bool>,
        /// The client's number for this input, one higher with each it
        /// sends (`crate::own_car`): the server hands back the newest it
        /// has applied with the car's snapshot in `DriverFeedback`, so a
        /// predicting client knows which of its inputs to replay. `None`
        /// (and a client from before the field) gets no snapshot. Left off
        /// the wire when `None`.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        seq: Option<u32>,
    },

    // TCP - A line of text chat to the lobby, or to the session the player
    // drives in or watches (`crate::chat`): capped, rate-limited, and
    // dropped for a muted player.
    Chat {
        text: String,
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
    /// Token to present in `UdpHandshake` to bind a UDP address to this
    /// connection. It travels in the clear in that handshake, so it only
    /// names the connection; the handshake is accepted on `udp_key`.
    #[serde(default)]
    pub udp_token: String,
    /// UDP port the server listens on (same host as the TCP endpoint).
    #[serde(default)]
    pub udp_port: u16,
    /// Secret under which the client seals every datagram it sends
    /// (`crate::udp_seal`): hex, used as raw bytes. It never travels except
    /// here, over TLS. Empty from a server from before sealing, which also
    /// accepts no sealed datagram: such a client stays on TCP telemetry.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub udp_key: String,
    /// Secret to present as `Authenticate.resume_token` on a later
    /// connection to be this player again (the same player id, and the seat
    /// they hold in a session). Left off the wire when empty.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub resume_token: String,
    /// A random challenge, hex, for the client to sign with its driver key
    /// (`ProveDriverKey`); left off the wire when `Authenticate` carried no
    /// key.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub key_challenge: String,
}

/// A seat the player still holds in a session they lost the connection to:
/// their car is driven by the server (or parked in its garage) until they
/// come back with `JoinSession`. Sent after `Authenticate` on a resumed
/// connection; the session's `SessionSummary` in the lobby has the rest.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct RejoinData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    pub session_kind: SessionKind,
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
    /// The session's damage rule, the same for every car; left off the wire
    /// when full (and full from a server that predates the field).
    #[serde(default, skip_serializing_if = "DamageLevel::is_full")]
    pub damage: DamageLevel,
    /// The AI field's level, as the host picked it; left off the wire for
    /// the mixed field (and absent from a server that predates the field).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ai_skill: Option<u8>,
    /// A timed race's length in seconds; left off the wire for a race over
    /// laps (and absent from a server that predates the field).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub race_seconds: Option<u32>,
    /// A timed qualifying session's length in seconds; left off the wire
    /// for open-ended qualifying (and absent from a server that predates
    /// the field).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub qualifying_seconds: Option<u32>,
    /// A race weekend's format, clamped (its keys stay snake_case); left
    /// off the wire for a plain session. `WeekendState` follows.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weekend: Option<crate::weekend::WeekendFormat>,
    /// The qualifying cut-off, percent of pole, clamped; left off the wire
    /// for none (and absent from a server that predates the field).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub qualifying_cutoff_pct: Option<u16>,
    /// The race's mandatory pit stops, clamped; left off the wire for none
    /// (and absent from a server that predates the field).
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub mandatory_stops: u8,
    /// The race must be run on two dry compounds; left off the wire when
    /// false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub compound_change: bool,
    /// How qualifying runs (1 a one-lap shoot-out); left off the wire for
    /// the usual session (and absent from a server that predates the
    /// field). `ShootoutState` follows while a shoot-out runs.
    #[serde(default, skip_serializing_if = "QualifyingFormat::is_open")]
    pub qualifying_format: QualifyingFormat,
    /// The qualifying lap allowance, flying laps per car; left off the wire
    /// for none.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub qualifying_lap_allowance: u8,
    /// The race-control rules, as `CreateSession.race_rules` (its keys stay
    /// snake_case); left off the wire when nothing is set (and absent from
    /// a server that predates the field).
    #[serde(default, skip_serializing_if = "RaceRules::is_default")]
    pub race_rules: RaceRules,
    /// What a predicting client steps its own car with
    /// (docs/proposals/client-prediction.md): the session's tick rate and
    /// the road its cars drive on. Sent to a driver, not to a spectator or
    /// a demo's watcher; absent from a server that predates the field, and
    /// then the client does not predict.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub physics: Option<SessionPhysics>,
    /// The session's classes (`CreateSession.classes` as the server keeps
    /// them: sanitized, in rank order, the quickest first), so the client
    /// places each car in its class; left off the wire for a field of one
    /// class (and absent from a server that predates the field).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub classes: Vec<ClassEntry>,
}

/// The session's physics as the client's copy of the car step needs them
/// (`SessionJoined.Physics`; its keys stay snake_case).
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct SessionPhysics {
    /// The session's tick rate, Hz: the step's `dt` is its inverse.
    pub tick_rate_hz: u16,
    /// The road the wheels stand on: 0 the centerline, 1 the road mesh (the
    /// physics library's `apx_track_load` `road_contact`). The mesh only
    /// when the server drives on it and the track has one baked.
    pub road_contact: u8,
    /// The checksum of the track's physics files, the YAML and every
    /// sidecar the car step reads (`crate::track_bundle`), which names the
    /// bundle `RequestTrackBundle` asks for. Left off (0) when the server
    /// serves none: a track built in memory, or an older server.
    #[serde(default, skip_serializing_if = "is_zero_u32")]
    pub bundle_crc: u32,
    /// The bundle's size, bytes deflated, and how many chunks it comes in.
    #[serde(default, skip_serializing_if = "is_zero_u32")]
    pub bundle_bytes: u32,
    #[serde(default, skip_serializing_if = "is_zero_u32")]
    pub bundle_chunks: u32,
    /// The joining driver's car as the server balances it (`[[balance]]`,
    /// `crate::balance`): what the client's copy of the car step applies to
    /// its own car.toml. Left off for a car no table names.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub balance: Option<BalanceFigures>,
}

/// A car's balance of performance as figures (`crate::balance::CarBalance`
/// without the car's name).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct BalanceFigures {
    pub ballast_kg: f32,
    pub power: f32,
    pub downforce: f32,
}

impl BalanceFigures {
    /// The figures of the table that names `car`, if one does.
    pub fn of(tables: &[crate::balance::CarBalance], car: &CarConfig) -> Option<Self> {
        tables.iter().find(|t| t.names(car)).map(|t| Self {
            ballast_kg: t.ballast_kg,
            power: t.power,
            downforce: t.downforce,
        })
    }

    /// The table these figures make, naming no car (to apply directly).
    pub fn table(&self) -> crate::balance::CarBalance {
        crate::balance::CarBalance {
            car: String::new(),
            ballast_kg: self.ballast_kg,
            power: self.power,
            downforce: self.downforce,
        }
    }
}

impl SessionPhysics {
    /// The same, naming the track's physics bundle when the server has one.
    pub fn with_bundle(self, bundle: Option<&crate::track_bundle::TrackBundle>) -> Self {
        match bundle {
            Some(bundle) => Self {
                bundle_crc: bundle.crc,
                bundle_bytes: bundle.bytes.len() as u32,
                bundle_chunks: bundle.chunk_count(),
                ..self
            },
            None => self,
        }
    }
}

fn is_zero_u32(value: &u32) -> bool {
    *value == 0
}

/// One chunk of a track's physics bundle (`crate::track_bundle`), answering
/// `RequestTrackBundle`. `chunks` 0 (and no bytes) says the server has no
/// bundle with that checksum or no such chunk.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct TrackBundleChunkData {
    pub crc: u32,
    pub chunk: u32,
    pub chunks: u32,
    pub bytes: RecordBytes,
}

/// Where a race weekend stands (`crate::weekend`). Sent to every member of
/// the session, drivers and spectators, right after `SessionJoined` (and a
/// rejoin) and whenever a field changes.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct WeekendStateData {
    /// 0 lobby (not started), 1 practice, 2 qualifying, 3 race.
    pub phase: u8,
    /// The qualifying round, 1-based; 0 outside qualifying.
    pub round: u8,
    /// How many qualifying rounds the weekend has.
    pub rounds: u8,
    /// The current phase has ended (the session is `Finished`); from
    /// practice or qualifying the host may advance.
    pub finished: bool,
    /// How many cars go through from this round; 0 in the final round and
    /// outside qualifying.
    pub advancing: u8,
    /// Every car knocked out so far, in grid order.
    pub eliminated: Vec<KnockedOutData>,
    /// The phase's damage rule: practice's or qualifying's own when the
    /// format names one, else the session's (the race's). Left off the wire
    /// when full.
    #[serde(default, skip_serializing_if = "DamageLevel::is_full")]
    pub damage: DamageLevel,
    /// The sky the current phase runs under, for a weekend whose practice
    /// or qualifying has its own (`GameSession::begin_day`): it replaces
    /// what `SessionJoined.Conditions` said. Left off the wire for a
    /// weekend raced under one sky.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub conditions: Option<SessionConditions>,
}

/// A one-lap shoot-out's run order and where it stands
/// (`GameSession::shootout_state`). Sent to every member of the session
/// right after `SessionJoined` while a shoot-out runs, and whenever a field
/// changes.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ShootoutStateData {
    /// The cars' indices (as `SessionRoster` maps them) in run order, first
    /// out first; a car that left the session is dropped.
    pub order: Vec<u8>,
    /// The 1-based place in `order` of the car on its run; 0 when none is.
    pub running: u8,
    /// How many cars of `order` have had their run.
    pub done: u8,
    /// The car on its run has started its flying lap. Left off the wire
    /// when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub flying: bool,
}

/// A car knocked out of a weekend's qualifying.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct KnockedOutData {
    /// The car's index, as `SessionRoster` maps it.
    pub car_index: u8,
    /// The round it went out in, 1-based.
    pub round: u8,
    /// Its race grid slot, 1-based.
    pub grid_position: u8,
    /// Its best legal lap in that round, ms; 0 for none.
    pub lap_time_ms: u32,
    /// A late entry: the driver joined after Q1 began and ran no round
    /// (`weekend::Eliminated::late`). Left off the wire when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub late: bool,
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
    /// A live session can be watched as a spectator stream
    /// (`SpectateSession`) instead of through `JoinAsSpectator`'s racer
    /// telemetry. Left off the wire from a server that predates it.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub live_stream: bool,
    /// The port of the server's health endpoint when it hands out its
    /// recordings of finished sessions (`/recordings`, on the address the
    /// client connected to). Left off the wire when it does not.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub recordings_port: Option<u16>,
    /// The server balances its cars (`[[balance]]` in server.toml:
    /// ballast, power, downforce), so a car drives here unlike its
    /// car.toml; for the session browser. Left off the wire when it does
    /// not.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub balanced: bool,
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

pub use crate::own_car::RecordBytes;

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

    // TCP - The player holds a seat in a session they lost the connection
    // to; `JoinSession` with its id takes it back.
    RejoinAvailable(RejoinData),

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

    // TCP - The corners of the session's track, numbered in lap order and
    // named from the dossier, so a screen can say "T7 Eau Rouge" for the
    // station a car is at. Sent once, after `SessionJoined`, to the watcher
    // of a hotlap (`SessionKind::HotlapWatch`).
    TrackCorners(TrackCornersData),

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

    // TCP - The qualifying results stored for a track, on request
    // (`RequestQualifyingResults`).
    QualifyingResults(QualifyingResultsData),

    // TCP - A track's fastest laps, on request (`RequestLeaderboard`).
    Leaderboard(LeaderboardData),

    // TCP - The garage's reference card for the joining driver's car: each
    // setup knob's stock value and click in real units (`setup_sheet.rs`).
    // Sent once, right after `SessionJoined`.
    CarSetupSheet(crate::setup_sheet::CarSetupSheetData),

    // TCP - A car stopped at its box and the crew started work: which box,
    // what the stop does and how long each part takes, in the order the
    // crew does them (tyres, fuel, repairs). Sent to every human in the
    // session as the service starts; telemetry's `service_ds` counts the
    // whole of it down.
    PitService(PitServiceData),

    // TCP - The showcases the server plays, answering `ListShowcases`.
    Showcases(ShowcasesData),

    // TCP - A slice of the session's road state (`crate::road_state`):
    // rubber, marbles, a dry line and water, per cell across the road.
    // Sent round-robin over the lap while racing and all at once on join;
    // the client draws it on the road (droppable, a drop self-heals on the
    // next round).
    RoadState(RoadStateData),

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

    // TCP - Where a race weekend stands: its phase, round, whether the
    // phase has ended, and the cars knocked out so far. Sent to the whole
    // session after `SessionJoined` and whenever it changes.
    WeekendState(WeekendStateData),

    // TCP - The driver's own pit box: where its stop spot is and how it
    // sits in the lane. Sent to each human once their box is dealt, and
    // again after a rejoin. What a driver who drives the lane themselves
    // aims for, and where the camera stands while the crew works.
    PitBox(PitBoxData),

    // TCP - A message from the server's operator (the dashboard's
    // broadcast, or a session it ends), for the client to show as it is.
    ServerNotice { message: String },

    // TCP - A one-lap shoot-out's run order, the car on its run and how many
    // have run. Sent to the whole session after `SessionJoined` while one
    // runs, and whenever it changes.
    ShootoutState(ShootoutStateData),

    // TCP - A chat line (`ClientMessage::Chat`) to everyone it was said to.
    Chat(ChatData),

    // TCP - One chunk of a track's physics bundle, on request
    // (`RequestTrackBundle`): what a predicting client steps its car on.
    TrackBundleChunk(TrackBundleChunkData),
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
            ServerMessage::RejoinAvailable(_) => MessagePriority::Critical,
            ServerMessage::SessionStarting { .. } => MessagePriority::Critical,
            ServerMessage::SessionLeft => MessagePriority::Critical,
            ServerMessage::GameModeChanged { .. } => MessagePriority::Critical,
            ServerMessage::SessionRoster(_) => MessagePriority::Critical,
            ServerMessage::RacingLine(_) => MessagePriority::Critical,
            ServerMessage::TrackSectors(_) => MessagePriority::Critical,
            ServerMessage::TrackCorners(_) => MessagePriority::Critical,
            ServerMessage::LapTiming(_) => MessagePriority::Critical,
            ServerMessage::LapRecord(_) => MessagePriority::Critical,
            // Asked for once; a reply that never came leaves the driver
            // with no ghost at all.
            ServerMessage::GhostLap(_) => MessagePriority::Critical,
            ServerMessage::QualifyingResults(_) => MessagePriority::Critical,
            ServerMessage::Leaderboard(_) => MessagePriority::Critical,
            ServerMessage::CarSetupSheet(_) => MessagePriority::Critical,
            ServerMessage::PitService(_) => MessagePriority::Critical,
            ServerMessage::Showcases(_) => MessagePriority::Critical,
            // Periodic and self-healing on the next round, like telemetry.
            ServerMessage::RoadState(_) => MessagePriority::Droppable,
            ServerMessage::SpectatorJoined(_) => MessagePriority::Critical,
            ServerMessage::SpectatorRecord(_) => MessagePriority::Critical,
            ServerMessage::WeekendState(_) => MessagePriority::Critical,
            ServerMessage::ShootoutState(_) => MessagePriority::Critical,
            ServerMessage::PitBox(_) => MessagePriority::Critical,
            ServerMessage::ServerNotice { .. } => MessagePriority::Critical,
            ServerMessage::Chat(_) => MessagePriority::Critical,
            // Asked for once, chunk by chunk, a few in flight.
            ServerMessage::TrackBundleChunk(_) => MessagePriority::Critical,

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
    /// Track file relative to content folder (e.g. "tracks/default/Armadillos/Armadillos.yaml")
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
    /// How much damage the cars take here, for the session browser. Full
    /// (the default) is left off the wire.
    #[serde(default, skip_serializing_if = "DamageLevel::is_full")]
    pub damage: DamageLevel,
    /// The race distance in laps, so a spectator's HUD can count down to it.
    /// Appended; 0 from an older server.
    #[serde(default)]
    pub lap_limit: u8,
    /// A timed race's length in seconds (`lap_limit` is then 0); left off
    /// the wire for a race over laps.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub race_seconds: Option<u32>,
    /// A race weekend between phases, waiting for its host to advance it
    /// (`GameSession::awaits_weekend_advance`): listed `Finished`, it still
    /// takes drivers (`GameSession::refuses_drivers`). Left off the wire
    /// when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub weekend_waiting: bool,
    /// The race's mandatory pit stops, for the session browser; left off
    /// the wire for none.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub mandatory_stops: u8,
    /// The race must be run on two dry compounds, for the session browser;
    /// left off the wire when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub compound_change: bool,
    /// The race-control rules (`CreateSession.race_rules`), for the session
    /// browser; left off the wire at their defaults.
    #[serde(default, skip_serializing_if = "RaceRules::is_default")]
    pub race_rules: RaceRules,
    /// A multi-class field's class names, the quickest first, for the
    /// session browser; left off the wire for a field of one class.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub classes: Vec<String>,
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
    /// Where the circuit is, degrees north and east, when its metadata
    /// names a longitude (`TrackConfig::site`): what a session's solar noon
    /// is worked out from (`data::solar_noon_minutes`), so the create
    /// screen's sky preview stands the sun where the session will. Both
    /// left off the wire otherwise.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latitude_deg: Option<f32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub longitude_deg: Option<f32>,
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
    /// The compound on the car, an index into its `TireConfig::compounds()`
    /// (the defaults: 0 soft, 1 medium, 2 hard, 3 intermediate, 4 wet);
    /// [`COMPOUND_UNKNOWN`] before a set is fitted, or
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
    /// Each tread's shoulders, °C, inner then outer per tyre, FL FR RL RR
    /// (`tyre_thermal`: `tyre_c` is the mean, so the middle is three times
    /// it less the two). Appended after `ers_flags`; 0 is "not known".
    #[serde(default)]
    pub tyre_c_edges: [u8; 8],
    /// Each brake's pad wear, percent, FL FR RL RR (`crate::brakes`).
    #[serde(default)]
    pub brake_wear: [u8; 4],
    /// Which tyres are sliding hard (bits 0-3, FL FR RL RR: smoke) and
    /// which are locked (bits 4-7) ([`SLIDE_FLAG_LOCKED_SHIFT`]).
    #[serde(default)]
    pub slide_flags: u8,
    /// The hybrid's stint budget left, percent (255: no stint rule / no
    /// hybrid) (`crate::hybrid`).
    #[serde(default = "no_hybrid")]
    pub ers_stint_pct: u8,
    /// A recovery's hold left, in tenths of a second (`crate::recovery`):
    /// 0 for none, at least 1 while the car waits for traffic. Appended
    /// after `ers_stint_pct`; 0 from an older server.
    #[serde(default)]
    pub recover_ds: u16,
    /// How deep a flat spot each tyre carries, percent of the worst,
    /// FL FR RL RR (`TireData::flat_spot`): drawn on the tyre for every car,
    /// where `DriverFeedback` only tells the car's own driver. Appended
    /// after `recover_ds`; all 0 from an older server.
    #[serde(default)]
    pub flat_spot: [u8; 4],
    /// Where along each body zone's face the hits landed (`DamageState::hit_at`),
    /// front, rear, left, right, -127 to 127 for -1 to 1 (the nose and the tail
    /// across the car, positive left; the flanks along it, positive forward).
    /// Appended after `flat_spot`; all 0 from an older server.
    #[serde(default)]
    pub damage_at: [i8; 4],
    /// The road under each tyre, FL FR RL RR: the water, percent of what
    /// a downpour leaves on a flat road (`CarState::tyre_water` x 100,
    /// held to 255), and the marbles, percent. Appended after `damage_at`;
    /// all 0 from an older server, which a client reads as "not sent".
    #[serde(default)]
    pub tyre_water: [u8; 4],
    #[serde(default)]
    pub tyre_marbles: [u8; 4],
    /// Each tyre's state bits, FL FR RL RR: bit 0 punctured (down to its
    /// carcass, `TireData::punctured`), bit 1 a slow leak under way
    /// (`leak_kpa_per_s` > 0 and not yet flat). Appended after
    /// `tyre_marbles`; all 0 from an older server.
    #[serde(default)]
    pub tyre_state: [u8; 4],
    /// Each tyre's picked-up rubber and blistering, percent, FL FR RL RR
    /// (`TireData::pickup`, `blister`): drawn on the tyre for every car, the
    /// same for everyone watching. Appended after `tyre_state`; all 0 from
    /// an older server, which a client reads as "not sent".
    #[serde(default)]
    pub tyre_pickup: [u8; 4],
    #[serde(default)]
    pub tyre_blister: [u8; 4],
    /// Each body zone's second dent, front, rear, left, right, as `damage_at`
    /// (`HitCentres::second`), or `damage::NO_SECOND_DENT` (-128) for a
    /// zone with one. Appended after `tyre_blister`; an older server sends
    /// none, which a client reads as one dent a zone.
    #[serde(default = "no_second_dents")]
    pub damage_at2: [i8; 4],
    /// The soft ground under each tyre that is off the track, two bits a
    /// wheel, FL in the lowest (`GROUND_GRAVEL`, `GROUND_SAND`,
    /// `GROUND_DESERT`; 0 for the road, a kerb, grass or a wheel in the
    /// air): what throws a dust plume and rattles stones. Appended after
    /// `damage_at2`; 0 from an older server.
    #[serde(default)]
    pub ground: u8,
    /// Each disc's wear, percent, FL FR RL RR, apart from its pads
    /// (`CarState::brake_disc_wear_pct`, on a car with `[brakes]
    /// disc_wear_per_mj`); [`DISC_WEAR_NONE`] on a car whose discs never
    /// wear, and from an older server. Appended after `ground`.
    #[serde(default = "no_disc_wear")]
    pub disc_wear: [u8; 4],
    /// The whole car's stint energy left, percent of `[fuel]
    /// stint_energy_mj` (`CarState::stint_energy_pct`); 255 without the
    /// rule, and from an older server. Appended after `disc_wear`.
    #[serde(default = "no_hybrid")]
    pub stint_energy_pct: u8,
    /// The flags race control shows this car (`crate::race_control`): bit 0
    /// the blue flag ([`RACE_FLAG_BLUE`]), bits 1-2 the yellow in its sector
    /// and the next (0 none, 1 single, 2 double; [`RACE_FLAG_YELLOW_SHIFT`]).
    /// Appended after `stint_energy_pct`; 0 outside a race and from an
    /// older server.
    #[serde(default)]
    pub race_flags: u8,
    /// The gearbox's warnings (`crate::physics`): bit 0 the engine is
    /// forced past its limit now ([`ENGINE_FLAG_OVER_REV`],
    /// `CarState::over_rev_ratio`), bit 1 the box refused a downshift a
    /// moment ago ([`ENGINE_FLAG_SHIFT_REFUSED`], `shift_refused_s`), bit 2
    /// the next gear down would over-rev at this speed
    /// ([`ENGINE_FLAG_DOWNSHIFT_RISK`], `downshift_risk`). Appended after
    /// `race_flags`; 0 from an older server.
    #[serde(default)]
    pub engine_flags: u8,
    /// How far past its limit the engine is forced, percent of
    /// `max_engine_rpm` (held to a byte): the revs `engine_rpm` clamps
    /// away, so a dash can show 118%. Appended after `engine_flags`; 0
    /// within the limit and from an older server.
    #[serde(default)]
    pub over_rev_pct: u8,
}

/// `engine_flags` bits.
pub const ENGINE_FLAG_OVER_REV: u8 = 1;
pub const ENGINE_FLAG_SHIFT_REFUSED: u8 = 2;
pub const ENGINE_FLAG_DOWNSHIFT_RISK: u8 = 4;

/// The over-rev the flags warn of: the revs the damage starts at
/// (`damage::OVER_REV_TOLERANCE`), not a car pinned on its limiter.
pub const OVER_REV_FLAG_RATIO: f32 = crate::damage::OVER_REV_TOLERANCE;

/// The `engine_flags` byte of a car's telemetry.
pub fn engine_flags_of(state: &CarState) -> u8 {
    (((state.over_rev_ratio >= OVER_REV_FLAG_RATIO) as u8) * ENGINE_FLAG_OVER_REV)
        | (((state.shift_refused_s > 0.0) as u8) * ENGINE_FLAG_SHIFT_REFUSED)
        | ((state.downshift_risk as u8) * ENGINE_FLAG_DOWNSHIFT_RISK)
}

/// The `over_rev_pct` byte: percent past the limit, 0 within it or on it.
pub fn over_rev_pct_of(state: &CarState) -> u8 {
    if state.over_rev_ratio < OVER_REV_FLAG_RATIO {
        return 0;
    }
    ((state.over_rev_ratio - 1.0).max(0.0) * 100.0)
        .round()
        .min(255.0) as u8
}

/// `race_flags` bit 0: the blue flag.
pub const RACE_FLAG_BLUE: u8 = 1;
/// `race_flags` bits 1-2: the yellow ahead of the car.
pub const RACE_FLAG_YELLOW_SHIFT: u8 = 1;

/// The `race_flags` byte of a car's telemetry.
pub fn race_flags_of(state: &CarState) -> u8 {
    (state.flag_blue as u8) | ((state.flag_yellow.min(2)) << RACE_FLAG_YELLOW_SHIFT)
}

/// Every marshal sector showing a yellow (`crate::race_control`),
/// positional: `[sector_m, [[sector, level], ...]]`, a sector the
/// `sector_m` metres of lap from `sector * sector_m`, the level 1 single, 2
/// double.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct YellowFlags {
    pub sector_m: u16,
    pub sectors: Vec<[u16; 2]>,
}

/// `disc_wear` for discs that do not wear.
pub const DISC_WEAR_NONE: u8 = 255;

fn no_disc_wear() -> [u8; 4] {
    [DISC_WEAR_NONE; 4]
}

fn no_second_dents() -> [i8; 4] {
    [crate::damage::NO_SECOND_DENT; 4]
}

pub use crate::physics::{ground_byte, GROUND_DESERT, GROUND_GRAVEL, GROUND_SAND};

/// `CarState::wheel_ground` packed two bits a wheel, FL lowest.
pub fn pack_ground(wheel_ground: [u8; 4]) -> u8 {
    wheel_ground
        .iter()
        .enumerate()
        .fold(0, |bits, (i, g)| bits | ((g & 3) << (2 * i)))
}

/// `tyre_state` bits.
pub const TYRE_STATE_PUNCTURED: u8 = 1;
pub const TYRE_STATE_LEAKING: u8 = 2;

pub use crate::physics::SLIDE_FLAG_LOCKED_SHIFT;

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
    if state.pit.driving {
        flags |= PIT_FLAG_AUTOPILOT;
    }
    if state.pit.exit_closed {
        flags |= PIT_FLAG_EXIT_CLOSED;
    }
    if state.pit.held {
        flags |= PIT_FLAG_HELD;
    }
    if state.pit.requested {
        flags |= PIT_FLAG_REQUESTED;
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
/// `pit_flags` bit 3: the server drives the car along the pit route (the
/// pit autopilot: a human's car from the lane's mouth to its end, an AI's
/// from its turn onto the route).
pub const PIT_FLAG_AUTOPILOT: u8 = 8;
/// `pit_flags` bit 4: the pit exit light is red. The same for every car.
pub const PIT_FLAG_EXIT_CLOSED: u8 = 16;
/// `pit_flags` bit 5: this car is held at the red pit exit light.
pub const PIT_FLAG_HELD: u8 = 32;
/// `pit_flags` bit 6: the driver has asked to box this lap
/// (`SetPitPlan`'s `box_this_lap`), until the stop is made.
pub const PIT_FLAG_REQUESTED: u8 = 64;

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

#[derive(Debug, Clone, Deserialize)]
pub struct CompactTelemetry {
    pub server_tick: u32,
    pub session_state: SessionState,
    pub game_mode: GameMode,
    pub countdown_ms: Option<u16>,
    pub car_states: Vec<CompactCarState>,
    /// A timed race's clock (`GameSession::race_clock`). Appended, and left
    /// off in every other session (nil when the sky follows it), so a lap
    /// race's frame from before the sky keeps its bytes.
    #[serde(default)]
    pub race_clock: Option<RaceClock>,
    /// The sky as it is now (`crate::conditions`), appended after the race
    /// clock: every session's server sends it, since the wind and the clock
    /// are drawn in every one.
    #[serde(default)]
    pub sky: Option<SkyNow>,
    /// The yellow sectors of a race (`crate::race_control`), appended after
    /// the sky: left off while none is out.
    #[serde(default)]
    pub yellow_flags: Option<YellowFlags>,
    /// What the race is doing beyond its state and mode
    /// (`crate::race_start::RacePhase`): 1 the formation lap of a standing
    /// start, 2 a rolling start's pace lap (both run in the countdown's
    /// state, with no countdown), 3 / 4 the virtual safety car out /
    /// ending, 5 the safety car out, 6 safety car in this lap. Appended
    /// after the yellows; left off otherwise.
    #[serde(default)]
    pub race_phase: Option<u8>,
    /// The safety car on the road (`crate::safety_car`), from its call
    /// (waiting at the pit exit) until it is off the road. Appended after
    /// the phase; left off otherwise.
    #[serde(default)]
    pub safety_car: Option<SafetyCarPose>,
}

/// The safety car's pose, positional: `[x, y, z, yaw_rad, speed_mps]`, in
/// the server's frame as a car's `pos_x` / `pos_y` / `pos_z` / `yaw_rad` /
/// `speed_mps` are.
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, Deserialize)]
pub struct SafetyCarPose {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    pub speed_mps: f32,
}

impl From<crate::safety_car::ScPose> for SafetyCarPose {
    fn from(p: crate::safety_car::ScPose) -> Self {
        SafetyCarPose {
            x: p.x,
            y: p.y,
            z: p.z,
            yaw_rad: p.yaw_rad,
            speed_mps: p.speed_mps,
        }
    }
}

/// Positional like the struct it is, but with the trailing fields left off
/// when there is nothing in them: the race clock goes when none of it, the
/// sky, the yellows, the phase and the safety car is there, the sky when
/// none of it and what follows is, the yellows when neither they nor
/// anything after them is out, the phase when neither it nor the safety
/// car is, and the safety car when it is not on the road.
impl Serialize for CompactTelemetry {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeStruct;
        let with_sc = self.safety_car.is_some();
        let with_phase = self.race_phase.is_some() || with_sc;
        let with_flags = self.yellow_flags.is_some() || with_phase;
        let with_sky = self.sky.is_some() || with_flags;
        let with_clock = self.race_clock.is_some() || with_sky;
        let len = 5
            + with_clock as usize
            + with_sky as usize
            + with_flags as usize
            + with_phase as usize
            + with_sc as usize;
        let mut s = serializer.serialize_struct("CompactTelemetry", len)?;
        s.serialize_field("server_tick", &self.server_tick)?;
        s.serialize_field("session_state", &self.session_state)?;
        s.serialize_field("game_mode", &self.game_mode)?;
        s.serialize_field("countdown_ms", &self.countdown_ms)?;
        s.serialize_field("car_states", &self.car_states)?;
        if with_clock {
            s.serialize_field("race_clock", &self.race_clock)?;
        } else {
            s.skip_field("race_clock")?;
        }
        if with_sky {
            s.serialize_field("sky", &self.sky)?;
        } else {
            s.skip_field("sky")?;
        }
        if with_flags {
            s.serialize_field("yellow_flags", &self.yellow_flags)?;
        } else {
            s.skip_field("yellow_flags")?;
        }
        if with_phase {
            s.serialize_field("race_phase", &self.race_phase)?;
        } else {
            s.skip_field("race_phase")?;
        }
        if with_sc {
            s.serialize_field("safety_car", &self.safety_car)?;
        } else {
            s.skip_field("safety_car")?;
        }
        s.end()
    }
}

/// The sky as it is now (`crate::conditions::LiveConditions`) and what the
/// road under it holds, positional like the rest of `CompactTelemetry`:
/// `[clock_s, weather, rain_pct, cloud_pct, road_water_pct, air_c,
/// track_c, wind_kph, wind_to_deg, line_rubber_pct, time_scale,
/// next_weather, next_in_s, line_snow_pct, snowfall_pct, next2_weather,
/// next2_in_s, next3_weather, next3_in_s, visibility_m]`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SkyNow {
    /// The day's clock, seconds after midnight.
    pub clock_s: u32,
    /// The weather the forecast has reached (`Weather`).
    pub weather: u8,
    /// Rain falling, percent of heavy rain.
    pub rain_pct: u8,
    /// Cloud cover, percent.
    pub cloud_pct: u8,
    /// Water on the road, the lap's mean, percent of what heavy rain leaves
    /// on a flat road (over 100 is standing water).
    pub road_water_pct: u8,
    /// The air and the asphalt, °C.
    pub air_c: i8,
    pub track_c: i8,
    /// The wind this moment, gusts included, km/h.
    pub wind_kph: u8,
    /// Where it blows toward, degrees counter-clockwise from the track
    /// frame's +X (0..359).
    pub wind_to_deg: u16,
    /// Rubber on the racing line, the lap's mean, percent (50 is what the
    /// cars are calibrated on).
    pub line_rubber_pct: u8,
    /// How fast the day's clock runs (0 stands still).
    pub time_scale: u8,
    /// The forecast's next weather, 255 when it holds.
    pub next_weather: u8,
    /// Seconds of session until then, 0 when it holds.
    pub next_in_s: u32,
    /// The snow cover on the racing line, the lap's mean, percent; 0 in a
    /// session without snow.
    #[serde(default)]
    pub line_snow_pct: u8,
    /// Snow falling, percent of steady snowfall.
    #[serde(default)]
    pub snowfall_pct: u8,
    /// The second and third changes the forecast holds after the next, with
    /// the seconds until each; `_in_s` 0 when the forecast holds no more.
    #[serde(default)]
    pub next2_weather: u8,
    #[serde(default)]
    pub next2_in_s: u32,
    #[serde(default)]
    pub next3_weather: u8,
    #[serde(default)]
    pub next3_in_s: u32,
    /// How far one sees through the fog, m (`LiveConditions::visibility_m`:
    /// 10 000 in clear air, down to 150 in the thickest radiation fog); 0
    /// from a sender that does not say (an old server or stream).
    #[serde(default)]
    pub visibility_m: u16,
}

impl SkyNow {
    /// The sky of `live`, with the wind `wind_mps` (m/s toward where it
    /// blows, track frame) and the forecast read `session_s` into the
    /// session.
    pub fn of(
        live: &crate::conditions::LiveConditions,
        wind_mps: [f32; 2],
        session_s: f32,
    ) -> Self {
        let pct = |v: f32| (v * 100.0).round().clamp(0.0, 255.0) as u8;
        let deg = |v: f32| v.round().clamp(-128.0, 127.0) as i8;
        let speed = (wind_mps[0] * wind_mps[0] + wind_mps[1] * wind_mps[1]).sqrt();
        let to_deg = if speed > 0.0 {
            wind_mps[1]
                .atan2(wind_mps[0])
                .to_degrees()
                .rem_euclid(360.0)
                .round() as u16
                % 360
        } else {
            0
        };
        let next = live.next_change(session_s);
        let mut later = live.changes_after(session_s, 3).skip(1);
        let (second, third) = (later.next(), later.next());
        let eta = |c: Option<crate::conditions::WeatherChange>| {
            c.map_or((0u8, 0u32), |c| {
                (
                    c.weather as u8,
                    (c.at_s - session_s).max(1.0).round() as u32,
                )
            })
        };
        let ((next2_weather, next2_in_s), (next3_weather, next3_in_s)) = (eta(second), eta(third));
        Self {
            clock_s: ((live.clock_min * 60.0).floor() as u32) % 86_400,
            weather: live.weather as u8,
            rain_pct: pct(live.rain),
            cloud_pct: pct(live.cloud),
            road_water_pct: pct(live.road_water),
            air_c: deg(live.air_c),
            track_c: deg(live.track_c),
            wind_kph: (speed * 3.6).round().clamp(0.0, 255.0) as u8,
            wind_to_deg: to_deg,
            line_rubber_pct: pct(live.line_rubber),
            time_scale: live.start.clock_scale(),
            next_weather: next.map_or(255, |c| c.weather as u8),
            next_in_s: next.map_or(0, |c| (c.at_s - session_s).max(1.0).round() as u32),
            line_snow_pct: pct(live.line_snow),
            snowfall_pct: pct(live.snowfall),
            next2_weather,
            next2_in_s,
            next3_weather,
            next3_in_s,
            visibility_m: live.visibility_m().round().clamp(0.0, u16::MAX as f32) as u16,
        }
    }
}

/// Where a timed race's clock stands, positional like the rest of
/// `CompactTelemetry`: `[left_ms, final_lap]`.
pub use crate::game_session::RaceClock;

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
            tyre_c_edges: tyre_edge_bytes(state),
            brake_wear: state
                .brake_wear_pct
                .map(|w| w.round().clamp(0.0, 100.0) as u8),
            slide_flags: state.slide_flags,
            ers_stint_pct: state.ers_stint_pct,
            recover_ds: state.recovery.map_or(0, |r| r.deciseconds()),
            flat_spot: state
                .tires
                .each()
                .map(|t| (t.flat_spot.clamp(0.0, 1.0) * 100.0).round() as u8),
            damage_at: state.damage.hit_at.to_wire(),
            tyre_water: state
                .tyre_water
                .map(|w| (w.max(0.0) * 100.0).round().min(255.0) as u8),
            tyre_marbles: state
                .tyre_marbles
                .map(|m| (m.clamp(0.0, 1.0) * 100.0).round() as u8),
            tyre_state: state.tires.each().map(|t| {
                let mut bits = 0;
                if t.punctured {
                    bits |= TYRE_STATE_PUNCTURED;
                } else if t.leak_kpa_per_s > 0.0 {
                    bits |= TYRE_STATE_LEAKING;
                }
                bits
            }),
            tyre_pickup: state
                .tires
                .each()
                .map(|t| (t.pickup.clamp(0.0, 1.0) * 100.0).round() as u8),
            tyre_blister: state
                .tires
                .each()
                .map(|t| (t.blister.clamp(0.0, 1.0) * 100.0).round() as u8),
            damage_at2: state.damage.hit_at.to_wire_second(),
            ground: pack_ground(state.wheel_ground),
            disc_wear: if state.brake_discs_wear {
                state
                    .brake_disc_wear_pct
                    .map(|w| w.round().clamp(0.0, 100.0) as u8)
            } else {
                no_disc_wear()
            },
            stint_energy_pct: state.stint_energy_pct,
            race_flags: race_flags_of(state),
            engine_flags: engine_flags_of(state),
            over_rev_pct: over_rev_pct_of(state),
        }
    }
}

/// Each tread's inner and outer shoulder, °C, as `tyre_bytes` rounds them;
/// all 0 before the tyres are fitted.
pub fn tyre_edge_bytes(state: &CarState) -> [u8; 8] {
    if !state.tyres_fitted {
        return [0; 8];
    }
    let byte = |v: f32| v.round().clamp(1.0, 255.0) as u8;
    let mut out = [0u8; 8];
    for (i, t) in state.tires.each().into_iter().enumerate() {
        // A tyre from before the zones reads its mean on both shoulders.
        let (inner, outer) = if t.tread_c == [0.0; 3] {
            (t.temperature_c, t.temperature_c)
        } else {
            (t.tread_c[0], t.tread_c[2])
        };
        out[2 * i] = byte(inner);
        out[2 * i + 1] = byte(outer);
    }
    out
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
    /// The driver lost their connection and the server drives the car
    /// until they rejoin (a held seat). Left off the wire when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub away: bool,
    /// The car's starting place, 1 = pole: the order the race lines up in.
    /// 0 (left off the wire) when the session has not dealt one.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub grid_slot: u8,
    /// On the grid before the countdown (`GameMode::Grid`): the driver said
    /// they are ready. Left off the wire when false, and always false
    /// outside the grid.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub ready: bool,
    /// Did not qualify: the qualifying cut-off (`qualifying_cutoff_pct`)
    /// left it out of the race, parked and towed
    /// (`GameSession::park_non_qualifiers`), not retired. Left off the wire
    /// when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub dnq: bool,
    /// Took the flag owing a stop or its second compound under the
    /// session's pit rule (`data::PitRule`): classified behind every
    /// finisher that met it. Left off the wire when false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub missed_stops: bool,
    /// Qualifying under a lap allowance (`qualifying_lap_allowance`): the
    /// timed laps this car has ended in the session or round, as the
    /// server counts them against the allowance. 0 (left off the wire)
    /// outside one.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub qualifying_laps: u8,
    /// Race control's penalties given to this car this race
    /// (`race_control::Penalty`), in the order given; the roster is sent
    /// again whenever one is given, served or added as time. Left off the
    /// wire when none.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub penalties: Vec<PenaltyData>,
}

/// One penalty (`race_control::Penalty`) as a roster carries it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct PenaltyData {
    /// What for: 0 a jump start, 1 a pass under a yellow, 2 a red-light
    /// pit exit (`race_control::Offence`).
    pub reason: u8,
    /// How it is paid: 0 time, 1 a drive-through, 2 a stop-go
    /// (`race_control::PenaltyKind`). An unserved one becomes 0.
    pub kind: u8,
    /// A time penalty's seconds (what an unserved one added), a stop-go's
    /// hold; 0 for a drive-through.
    pub seconds: u16,
    /// The lap it was given on; a drive-through or stop-go is served by
    /// `lap + race_control::SERVE_WITHIN_LAPS`.
    pub lap: u16,
    /// 0 still to serve, 1 served, 2 added to the race time
    /// (`race_control::PenaltyStatus`).
    pub status: u8,
}

impl From<&crate::race_control::Penalty> for PenaltyData {
    fn from(p: &crate::race_control::Penalty) -> Self {
        PenaltyData {
            reason: p.offence as u8,
            kind: p.kind as u8,
            seconds: p.seconds,
            lap: p.lap,
            status: p.status as u8,
        }
    }
}

fn is_zero_u8(value: &u8) -> bool {
    *value == 0
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

/// One corner (or chicane, or esses) of the lap: a stop of the track guide
/// (`track_guide::detect_corners`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct CornerData {
    /// From 1, in lap order from the start line.
    pub number: u8,
    /// The dossier's display name for it; empty when it has none (the
    /// client says "Turn N").
    pub name: String,
    /// Stations in metres from the line, each in `[0, lap)`: the corner runs
    /// from `entry_m` forward (past the line, if need be) to `exit_m`.
    pub entry_m: f32,
    pub apex_m: f32,
    pub exit_m: f32,
    /// The main turn goes left.
    pub left: bool,
}

/// The corners of the session's track.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct TrackCornersData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    /// Lap length in metres.
    pub track_length_m: f32,
    pub corners: Vec<CornerData>,
}

/// A slice of a session's road state (`crate::road_state::RoadState`): the
/// lap is cut into cells of `cell_m`, each across the road into `bins` bins
/// reaching `half_span_m` either side of the centerline. `rows` holds
/// `count` cells from `first_cell`, `RoadState::ROW_BYTES` each (water depth
/// then the bins' rubber, marbles and dry), and `geometry` where each of
/// those cells lies. `debris` is every piece on the
/// road now, in the server frame. Sent round-robin while racing and whole
/// on join; the client reads the rows into a texture it samples by the road
/// mesh's `(station, lateral)` UV.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct RoadStateData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    /// The lap the cells cover, m.
    pub lap_m: f32,
    /// One cell's length along the lap, m (`road_state::CELL_M`).
    pub cell_m: f32,
    /// Bins across a cell (`road_state::BINS`).
    pub bins: u8,
    /// How far either side of the centerline the bins reach, m
    /// (`road_state::HALF_SPAN_M`).
    pub half_span_m: f32,
    /// The first cell `rows` covers.
    pub first_cell: u32,
    /// `count` cells, `RoadState::ROW_BYTES` bytes each.
    pub rows: RecordBytes,
    /// Every piece of debris on the road now, `[x, y]` in the server frame.
    pub debris: Vec<[f32; 2]>,
    /// Where each cell in `rows` lies, `RoadState::GEOMETRY_BYTES` each:
    /// the centerline at its middle and the unit vector to its left (four
    /// little-endian `f32`, server frame). A bin's lateral is measured from
    /// it, positive right.
    pub geometry: RecordBytes,
    /// The snow of the same cells, `RoadState::SNOW_ROW_BYTES` each: the
    /// verge's cover, then per bin the cover and the ice. Left off the wire
    /// for a road with no snow, and skipped by a client from before it.
    #[serde(default, skip_serializing_if = "RecordBytes::is_empty")]
    pub snow_rows: RecordBytes,
    /// The water standing on each bin of the same cells, `road_state::BINS`
    /// bytes each in the rows' depth units (`RoadState::write_water_rows`):
    /// where a puddle lies across the road. Left off the wire when no
    /// puddle stands; a client without it spreads each row's depth across.
    #[serde(default, skip_serializing_if = "RecordBytes::is_empty")]
    pub water_rows: RecordBytes,
    /// The pit lane's snow cover, one byte over 0..1 a cell of the lane's
    /// own station (`RoadState::write_lane_snow`), the whole lane in every
    /// slice: the cars in it pack it apart from the track. Left off the
    /// wire for a road with no snow or no lane.
    #[serde(default, skip_serializing_if = "RecordBytes::is_empty")]
    pub lane_snow: RecordBytes,
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

/// A car's pit stop, as the crew starts it (`ServerMessage::PitService`).
/// Each part's seconds are 0 when the stop does not do it; the parts run
/// one after the other, tyres first.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct PitServiceData {
    /// Index into the current `SessionRoster`.
    pub car_index: u8,
    /// The car's own box, 0-based from the lane's entry end.
    pub pit_box: u8,
    /// The tyre change, s, and the compound going on
    /// (an index into the car's `TireConfig::compounds()`).
    pub tyres_s: f32,
    pub compound: u8,
    /// Refuelling, s, and the litres going in.
    pub fuel_s: f32,
    pub fuel_l: f32,
    /// Repairs, s, and the damage repaired (percent, summed over the zones).
    pub repair_s: f32,
    pub repair_pct: f32,
    /// The whole stop, s: the three parts added up.
    pub total_s: f32,
}

/// A chat line as the server sends it on (`ServerMessage::Chat`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ChatData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub player_id: PlayerId,
    /// The player's name as the lobby lists it.
    pub from: String,
    /// The line, cleaned and capped (`crate::chat::clean`).
    pub text: String,
    /// Said in a session (to its drivers and watchers), not the lobby.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub in_session: bool,
}

/// A driver's own pit box (`ServerMessage::PitBox`), in the server's
/// frame: metres, yaw in radians, +Y left.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct PitBoxData {
    /// The box, 0-based from the lane's entry end (`PitServiceData::pit_box`).
    pub pit_box: u8,
    /// The stop spot: where the car's middle stands for the crew, within
    /// `pit::BOX_RADIUS_M`, facing down the lane.
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    /// How far the spot is off the lane's middle, m, positive to the left
    /// of the yaw: the garages are on that side, the fast lane on the other.
    pub lateral_m: f32,
    pub lane_width_m: f32,
    /// The car's middle stops within this of the spot, m.
    pub radius_m: f32,
    /// The driver drives the pit lane themselves (the pit autopilot is off):
    /// the client marks the box. Sent again whenever it changes.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub manual: bool,
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

/// One driver's line of a stored qualifying result.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct QualifyingEntryData {
    pub name: String,
    /// The driver's profile id, for a human who sent one; left off the wire
    /// otherwise. A client grids them by it (`@driver:`).
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        serialize_with = "serialize_option_uuid_as_string",
        deserialize_with = "deserialize_option_uuid_from_string"
    )]
    pub driver_id: Option<DriverId>,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    pub lap_time_ms: u32,
    pub is_ai: bool,
}

/// A stored qualifying result: the drivers fastest first.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct QualifyingResultData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub id: uuid::Uuid,
    /// The car class it was set in, for the picker's label.
    pub class: String,
    /// RFC 3339, display only.
    pub recorded_at: String,
    pub entries: Vec<QualifyingEntryData>,
}

/// The reply to `ClientMessage::RequestQualifyingResults`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct QualifyingResultsData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    pub results: Vec<QualifyingResultData>,
}

impl QualifyingResultsData {
    pub fn from_results(
        track_id: TrackConfigId,
        results: Vec<crate::records::QualifyingResult>,
    ) -> Self {
        Self {
            track_id,
            results: results
                .into_iter()
                .map(|r| QualifyingResultData {
                    id: r.id,
                    class: r.class,
                    recorded_at: r.recorded_at,
                    entries: r
                        .entries
                        .into_iter()
                        .map(|e| QualifyingEntryData {
                            name: e.name,
                            driver_id: e.driver_id,
                            car_config_id: e.car_config_id,
                            lap_time_ms: e.lap_time_ms,
                            is_ai: e.is_ai,
                        })
                        .collect(),
                })
                .collect(),
        }
    }
}

/// One line of a leaderboard: a driver's best lap in a car.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LeaderboardEntryData {
    pub player_name: String,
    /// The driver's profile id, when the lap was set under one; left off
    /// the wire otherwise. The client lights its own laps by it.
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        serialize_with = "serialize_option_uuid_as_string",
        deserialize_with = "deserialize_option_uuid_from_string"
    )]
    pub driver_id: Option<DriverId>,
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    pub lap_time_ms: u32,
    pub splits_ms: Vec<u32>,
    /// When the lap was set, RFC 3339. Display only.
    pub recorded_at: String,
}

/// The reply to `ClientMessage::RequestLeaderboard`: the fastest laps on a
/// track, best first.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct LeaderboardData {
    #[serde(
        serialize_with = "serialize_uuid_as_string",
        deserialize_with = "deserialize_uuid_from_string"
    )]
    pub track_id: TrackConfigId,
    /// The car the board is for, empty for every car.
    pub car_config_id: String,
    pub entries: Vec<LeaderboardEntryData>,
}

impl LeaderboardData {
    pub fn from_records(
        track_id: TrackConfigId,
        car_config_id: Option<CarConfigId>,
        records: Vec<crate::records::LapRecord>,
    ) -> Self {
        Self {
            track_id,
            car_config_id: car_config_id.map(|c| c.to_string()).unwrap_or_default(),
            entries: records
                .into_iter()
                .map(|r| LeaderboardEntryData {
                    player_name: r.player,
                    driver_id: r.driver_id,
                    car_config_id: r.car_config_id,
                    lap_time_ms: r.lap_time_ms,
                    splits_ms: r.splits_ms.to_vec(),
                    recorded_at: r.recorded_at,
                })
                .collect(),
        }
    }
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
            resume_token: None,
            driver_id: None,
            driver_key: None,
        };

        let serialized = rmp_serde::to_vec_named(&msg).unwrap();
        let deserialized: ClientMessage = rmp_serde::from_slice(&serialized).unwrap();

        match deserialized {
            ClientMessage::Authenticate {
                token,
                player_name,
                protocol_version,
                resume_token: _,
                driver_id: _,
                driver_key: _,
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
                pit_autopilot,
            } => {
                assert!(auto_gearbox && !steering_assist);
                assert_eq!(abs, None, "an old client leaves the car's own ABS");
                assert_eq!(traction_control, None);
                assert_eq!(pit_autopilot, None, "and the pit autopilot on");
            }
            _ => panic!("Wrong message type"),
        }

        let both = ClientMessage::SetDriverAids {
            auto_gearbox: false,
            steering_assist: true,
            abs: Some(false),
            traction_control: Some(TractionControl::High),
            pit_autopilot: Some(false),
        };
        let bytes = rmp_serde::to_vec_named(&both).unwrap();
        match rmp_serde::from_slice(&bytes).unwrap() {
            ClientMessage::SetDriverAids {
                auto_gearbox,
                steering_assist,
                abs,
                traction_control,
                pit_autopilot,
            } => {
                assert!(!auto_gearbox && steering_assist);
                assert_eq!(abs, Some(false));
                assert_eq!(traction_control, Some(TractionControl::High));
                assert_eq!(pit_autopilot, Some(false));
            }
            _ => panic!("Wrong message type"),
        }

        // Damage was a driver aid once; a client from then still sends the
        // key, and it no longer does anything (the session decides).
        #[derive(Serialize)]
        struct AidsWithDamage {
            auto_gearbox: bool,
            damage: u8,
        }
        #[derive(Serialize)]
        struct DamageEnvelope {
            r#type: String,
            data: AidsWithDamage,
        }
        let with_damage = rmp_serde::to_vec_named(&DamageEnvelope {
            r#type: "SetDriverAids".to_string(),
            data: AidsWithDamage {
                auto_gearbox: true,
                damage: 0,
            },
        })
        .unwrap();
        assert!(matches!(
            rmp_serde::from_slice(&with_damage).unwrap(),
            ClientMessage::SetDriverAids {
                auto_gearbox: true,
                ..
            }
        ));
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
            udp_key: "udp-key".to_string(),
            resume_token: String::new(),
            key_challenge: String::new(),
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
            handbrake: Some(true),
            seq: Some(9),
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
                handbrake,
                seq,
            } => {
                assert_eq!(seq, Some(9));
                assert_eq!(handbrake, Some(true));
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
            handbrake: None,
            seq: None,
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
    /// A clutch pedal: the client sends it as a float where it sent nil
    /// (`ApexUdpGolden::C_PlayerInputClutch`; `cargo test
    /// player_input_clutch_wire_format -- --nocapture`).
    #[test]
    fn test_player_input_clutch_wire_format() {
        let msg = ClientMessage::PlayerInput {
            server_tick_ack: 4242,
            throttle: 1.0,
            brake: 0.0,
            steering: -0.5,
            gear: Some(4),
            clutch: Some(0.5),
            drs: Some(false),
            headlights: None,
            flash: Some(false),
            ers_mode: Some(1),
            ers_boost: Some(false),
            handbrake: Some(false),
            seq: None,
        };
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        println!("C_PlayerInputClutch = {bytes:02x?}");
        let key = b"\xa6clutch";
        let at = bytes.windows(key.len()).position(|w| w == key).unwrap() + key.len();
        assert_eq!(bytes[at], 0xca, "the clutch is a float");
        assert_eq!(&bytes[at + 1..at + 5], &0.5f32.to_be_bytes());
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::PlayerInput { clutch, .. } => assert_eq!(clutch, Some(0.5)),
            _ => panic!("Wrong message type"),
        }
    }

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
            handbrake: Some(false),
            seq: None,
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
            0xa9, b'h', b'a', b'n', b'd', b'b', b'r', b'a', b'k', b'e',
            0xc2, // handbrake: false
        ];
        assert!(bytes.ends_with(tail), "{bytes:02x?}");
        assert_eq!(bytes[23], 0x8c, "twelve fields in the data map");

        if let ClientMessage::PlayerInput {
            headlights, flash, ..
        } = &mut msg
        {
            *headlights = Some(true);
            *flash = Some(true);
        }
        let on = rmp_serde::to_vec_named(&msg).unwrap();
        assert_eq!(on.len(), bytes.len(), "a bool is as long as nil");
        assert_eq!(on[on.len() - 40], 0xc3, "headlights: true");
        assert_eq!(on[on.len() - 33], 0xc3, "flash: true");
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
            race_clock: None,
            sky: None,
            yellow_flags: None,
            race_phase: None,
            safety_car: None,
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
            id: Some("Zandervoort.gt3.day".into()),
        })
        .unwrap();
        let spectate_any =
            rmp_serde::to_vec_named(&ClientMessage::SpectateShowcase { id: None }).unwrap();
        let leave = rmp_serde::to_vec_named(&ClientMessage::LeaveSpectate).unwrap();
        let watch = rmp_serde::to_vec_named(&ClientMessage::SpectateSession {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
        })
        .unwrap();
        println!("C_SpectateSession: {}", hex(&watch));
        println!("C_ListShowcases: {}", hex(&list));
        println!("C_SpectateShowcase: {}", hex(&spectate));
        println!("C_SpectateShowcaseAny: {}", hex(&spectate_any));
        println!("C_LeaveSpectate: {}", hex(&leave));
        for bytes in [&list, &spectate, &spectate_any, &leave, &watch] {
            let _: ClientMessage = rmp_serde::from_slice(bytes).unwrap();
        }
        // An older client's message, without the id, reads as "any".
        let bare: ClientMessage =
            rmp_serde::from_slice(b"\x82\xA4type\xB0SpectateShowcase\xA4data\x80").unwrap();
        assert!(matches!(bare, ClientMessage::SpectateShowcase { id: None }));

        let showcases = ServerMessage::Showcases(ShowcasesData {
            entries: vec![ShowcaseSummary {
                id: "Zandervoort.gt3.day".into(),
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
                    time_scale: None,
                    changeable: None,
                    track_rubber_pct: None,
                    sky_seed: None,
                    day_of_year: None,
                    solar_noon_min: None,
                    forecast: Default::default(),
                },
                duration_s: 259.5,
                cars: 16,
                viewers: 2,
            }],
        });
        let joined = ServerMessage::SpectatorJoined(SpectatorJoinedData {
            stream_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            kind: SpectatorKind::Showcase,
            showcase_id: "Zandervoort.gt3.day".into(),
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
    /// A lobby that hands out its recordings names the port
    /// (`ApexGolden::S_LobbyStateRecordings`):
    /// `cargo test recordings_wire_format -- --nocapture`.
    #[test]
    fn test_recordings_wire_format() {
        let lobby = |recordings_port| {
            ServerMessage::LobbyState(LobbyStateData {
                players_in_lobby: vec![],
                available_sessions: vec![],
                car_configs: vec![],
                track_configs: vec![],
                showcase_available: false,
                live_stream: true,
                recordings_port,
                balanced: false,
            })
        };
        let bytes = rmp_serde::to_vec_named(&lobby(Some(9002))).unwrap();
        let hex = bytes
            .iter()
            .map(|b| format!("0x{:02X}", b))
            .collect::<Vec<_>>()
            .join(", ");
        println!("S_LobbyStateRecordings: {hex}");
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::LobbyState(l) => assert_eq!(l.recordings_port, Some(9002)),
            _ => panic!("wrong message type"),
        }
        let none = rmp_serde::to_vec_named(&lobby(None)).unwrap();
        assert!(!String::from_utf8_lossy(&none).contains("RecordingsPort"));
    }

    /// A listed session names its race rules, and a lobby says when the
    /// server balances its cars (`ApexGolden::S_LobbyStateRaceRules`);
    /// both stay off the wire at their defaults. `cargo test
    /// lobby_race_rules_wire_format -- --nocapture` prints it.
    #[test]
    fn test_lobby_race_rules_wire_format() {
        let lobby = |race_rules: RaceRules, balanced| {
            ServerMessage::LobbyState(LobbyStateData {
                players_in_lobby: vec![],
                available_sessions: vec![SessionSummary {
                    id: Uuid::nil(),
                    track_name: "Monzarella".into(),
                    track_file: String::new(),
                    track_id: Uuid::nil(),
                    host_name: "host".into(),
                    session_kind: SessionKind::Multiplayer,
                    player_count: 1,
                    max_players: 8,
                    state: SessionState::Lobby,
                    conditions: SessionConditions::DEFAULT,
                    damage: DamageLevel::Full,
                    lap_limit: 5,
                    race_seconds: None,
                    weekend_waiting: false,
                    mandatory_stops: 0,
                    compound_change: false,
                    race_rules,
                    classes: Vec::new(),
                }],
                car_configs: vec![],
                track_configs: vec![],
                showcase_available: false,
                live_stream: true,
                recordings_port: None,
                balanced,
            })
        };
        let rules = RaceRules {
            formation_lap: true,
            jump_start: true,
            yellow_penalty: true,
            safety_car: crate::data::SafetyCar::Full,
            reverse_grid: 6,
            ..Default::default()
        };
        let bytes = rmp_serde::to_vec_named(&lobby(rules, true)).unwrap();
        let hex = bytes
            .iter()
            .map(|b| format!("0x{:02X}", b))
            .collect::<Vec<_>>()
            .join(", ");
        println!("S_LobbyStateRaceRules: {hex}");
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::LobbyState(l) => {
                assert!(l.balanced);
                assert_eq!(l.available_sessions[0].race_rules, rules);
            }
            _ => panic!("wrong message type"),
        }
        let plain = rmp_serde::to_vec_named(&lobby(RaceRules::default(), false)).unwrap();
        let text = String::from_utf8_lossy(&plain);
        assert!(!text.contains("RaceRules") && !text.contains("Balanced"));
    }

    /// A track that says where it is lists its site, so the create screen
    /// can stand the sun over it (`ApexGolden::S_LobbyStateTrackSite`);
    /// one that does not keeps its bytes. `cargo test
    /// track_site_wire_format -- --nocapture` prints it.
    #[test]
    fn test_track_site_wire_format() {
        let track = |site: Option<(f32, f32)>| TrackConfigSummary {
            id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            name: "Shiverstone".into(),
            content_crc: 0,
            centerline: vec![],
            latitude_deg: site.map(|s| s.0),
            longitude_deg: site.map(|s| s.1),
        };
        let lobby = |t| {
            ServerMessage::LobbyState(LobbyStateData {
                players_in_lobby: vec![],
                available_sessions: vec![],
                car_configs: vec![],
                track_configs: vec![t],
                showcase_available: false,
                live_stream: true,
                recordings_port: None,
                balanced: false,
            })
        };
        let bytes = rmp_serde::to_vec_named(&lobby(track(Some((52.0786, -1.0169))))).unwrap();
        println!(
            "S_LobbyStateTrackSite: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::LobbyState(l) => {
                assert_eq!(l.track_configs[0].latitude_deg, Some(52.0786));
                assert_eq!(l.track_configs[0].longitude_deg, Some(-1.0169));
            }
            _ => panic!("wrong message type"),
        }
        let none = rmp_serde::to_vec_named(&lobby(track(None))).unwrap();
        assert!(!String::from_utf8_lossy(&none).contains("LongitudeDeg"));
    }

    /// The lobby summaries carry each content file's checksum, and a client
    /// that predates the field must still read the message.
    #[test]
    fn test_lobby_summaries_carry_content_crc() {
        let msg = ServerMessage::LobbyState(LobbyStateData {
            players_in_lobby: vec![],
            available_sessions: vec![SessionSummary {
                id: Uuid::nil(),
                track_name: "Monzarella".into(),
                track_file: "tracks/default/Monzarella/Monzarella.yaml".into(),
                track_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
                host_name: "host".into(),
                session_kind: SessionKind::Multiplayer,
                player_count: 1,
                max_players: 8,
                state: SessionState::Lobby,
                conditions: SessionConditions::DEFAULT,
                damage: DamageLevel::Full,
                lap_limit: 5,
                race_seconds: None,
                weekend_waiting: false,
                mandatory_stops: 0,
                compound_change: false,
                race_rules: Default::default(),
                classes: Vec::new(),
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
                name: "Monzarella".into(),
                content_crc: 0xDEAD_BEEF,
                centerline: vec![],
                latitude_deg: None,
                longitude_deg: None,
            }],
            showcase_available: true,
            live_stream: true,
            recordings_port: None,
            balanced: false,
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
    /// blob `ApexUdpGolden::S_DriverFeedbackFlatSpot` (the 13-field
    /// `S_DriverFeedback` stays as an older server's). Change both together;
    /// `cargo test driver_feedback_wire_format -- --nocapture` prints them.
    #[test]
    fn test_driver_feedback_wire_format() {
        const GOLDEN: [u8; 131] = [
            0x92, 0xAE, 0x44, 0x72, 0x69, 0x76, 0x65, 0x72, 0x46, 0x65, 0x65, 0x64, 0x62, 0x61,
            0x63, 0x6B, 0x9E, 0xCD, 0x04, 0xD2, 0x92, 0xCA, 0x3E, 0x80, 0x00, 0x00, 0xCA, 0xBF,
            0x00, 0x00, 0x00, 0x94, 0xCA, 0x3F, 0xC0, 0x00, 0x00, 0xCA, 0xC0, 0x00, 0x00, 0x00,
            0xCA, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x3F, 0x00, 0x00, 0x00, 0x94, 0xCA, 0x00, 0x00,
            0x00, 0x00, 0xCA, 0x3E, 0x80, 0x00, 0x00, 0xCA, 0xBF, 0x80, 0x00, 0x00, 0xCA, 0x40,
            0x40, 0x00, 0x00, 0x94, 0x00, 0x01, 0x02, 0x00, 0x94, 0xCA, 0x3F, 0x00, 0x00, 0x00,
            0xCA, 0xBE, 0x80, 0x00, 0x00, 0xCA, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x40, 0x00, 0x00,
            0x00, 0xC3, 0xC2, 0xCA, 0x40, 0x90, 0x00, 0x00, 0xCA, 0xBF, 0xC0, 0x00, 0x00, 0xCA,
            0x3E, 0x80, 0x00, 0x00, 0xCA, 0xC0, 0x80, 0x00, 0x00, 0xCA, 0x3F, 0xC0, 0x00, 0x00,
            0x94, 0x00, 0x28, 0x00, 0x64,
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
            flat_spot: [0, 40, 0, 100],
            own_car: None,
            own_state: None,
            speed_cap_mps: 0.0,
        });
        let bytes = rmp_serde::to_vec(&msg).unwrap();
        println!("DriverFeedback ({} bytes):", bytes.len());
        for row in bytes.chunks(16) {
            let hex: Vec<String> = row.iter().map(|b| format!("0x{b:02X}")).collect();
            println!("		{},", hex.join(", "));
        }
        assert_eq!(bytes, GOLDEN);

        match rmp_serde::from_slice(&bytes).unwrap() {
            ServerMessage::DriverFeedback(decoded) => match msg {
                ServerMessage::DriverFeedback(original) => assert_eq!(decoded, original),
                _ => unreachable!(),
            },
            other => panic!("Wrong message type: {:?}", other),
        }
    }

    /// Prints `bytes` as rows of a C++ initializer, under `name`.
    fn print_golden(name: &str, bytes: &[u8]) {
        println!("{name} ({} bytes):", bytes.len());
        for row in bytes.chunks(16) {
            let hex: Vec<String> = row.iter().map(|b| format!("0x{b:02X}")).collect();
            println!("\t\t{},", hex.join(", "));
        }
    }

    /// The input sequence after the handbrake: `ApexUdpGolden::C_PlayerInputSeq`
    /// (`cargo test player_input_seq_wire_format -- --nocapture`). An input
    /// without one keeps its old bytes (`C_PlayerInput`).
    #[test]
    fn test_player_input_seq_wire_format() {
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
            handbrake: Some(false),
            seq: None,
        };
        let without = rmp_serde::to_vec_named(&msg).unwrap();
        assert_eq!(without[23], 0x8c, "no seq: twelve fields, the old bytes");
        if let ClientMessage::PlayerInput { seq, .. } = &mut msg {
            *seq = Some(70_000);
        }
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        print_golden("C_PlayerInputSeq", &bytes);
        assert_eq!(bytes[23], 0x8d, "thirteen fields");
        assert!(
            bytes.ends_with(&[0xa3, b's', b'e', b'q', 0xce, 0x00, 0x01, 0x11, 0x70]),
            "{bytes:02x?}"
        );
        assert_eq!(&bytes[..23], &without[..23]);
        assert_eq!(&bytes[24..bytes.len() - 9], &without[24..]);
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::PlayerInput { seq, .. } => assert_eq!(seq, Some(70_000)),
            _ => panic!("Wrong message type"),
        }
    }

    /// A snapshot with a value in every field, each exact in an `f32`.
    fn golden_snapshot() -> crate::own_car::OwnCarSnapshot {
        crate::own_car::OwnCarSnapshot {
            tick: 1234,
            input_seq: Some(300),
            pos: [100.5, -20.25, 3.0],
            attitude: [1.5, 0.0, -0.125],
            vel: [40.0, -2.5, 0.0],
            speed_mps: 40.0,
            angular_vel: [0.25, 0.0, 0.0],
            wheel_angular_vel: [120.0, 121.0, 122.5, 123.5],
            wheel_load_n: [2500.0, 2600.0, 3000.0, 3100.0],
            suspension_travel_m: [0.0, 0.0, -0.0625, 0.0],
            suspension_velocity_mps: [0.0, 0.5, 0.0, 0.0],
            g_forces: [-1.5, 0.25, 1.0],
            vertical_accel_mps2: 0.0,
            ground_slope: [0.0, 0.0],
            porpoise: [0.0, 0.0],
            controls: [1.0, 0.0, -0.25, 1.0],
            gear: 4,
            engine_rpm: 9000.0,
            turbo_spool: 0.0,
            auto_shift_hold_ticks: 0,
            auto_reverse_ticks: 0,
            track_progress: 512.5,
            lateral_offset_m: -1.5,
            current_lap: 2,
            centerline_idx: Some(205),
            flags: crate::own_car::FLAG_ON_TRACK | crate::own_car::FLAG_DRS_OPEN,
        }
    }

    /// The driver's own car on the feedback message
    /// (`cargo test driver_feedback_own_car_wire_format -- --nocapture`):
    /// `ApexUdpGolden::S_DriverFeedbackOwnCar` carries the snapshot and no
    /// slice of the state (the slice is left off, as nothing follows it);
    /// `S_DriverFeedbackOwnState` both. Without either the message keeps
    /// the bytes of `S_DriverFeedbackFlatSpot`.
    #[test]
    fn test_driver_feedback_own_car_wire_format() {
        let base = DriverFeedback {
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
            flat_spot: [0, 40, 0, 100],
            own_car: None,
            own_state: None,
            speed_cap_mps: 0.0,
        };
        let old = rmp_serde::to_vec(&ServerMessage::DriverFeedback(base.clone())).unwrap();

        let with_car = DriverFeedback {
            own_car: Some(Box::new(golden_snapshot())),
            ..base.clone()
        };
        let car_bytes =
            rmp_serde::to_vec(&ServerMessage::DriverFeedback(with_car.clone())).unwrap();
        print_golden("S_DriverFeedbackOwnCar", &car_bytes);
        // The old message's fields, then one more: the array grows by one.
        assert_eq!(old[16], 0x9e, "fourteen fields before");
        assert_eq!(car_bytes[16], 0x9f, "fifteen with the snapshot");
        assert_eq!(&car_bytes[17..old.len()], &old[17..]);
        assert_eq!(
            car_bytes[old.len()],
            0xdc,
            "the snapshot is an array of 26 (array16)"
        );
        assert_eq!(&car_bytes[old.len() + 1..old.len() + 3], &[0x00, 0x1a]);

        let with_state = DriverFeedback {
            own_state: Some(crate::own_car::StateChunk {
                tick: 1190,
                input_seq: Some(290),
                index: 1,
                count: 9,
                bytes: RecordBytes(vec![0x41, 0x50, 0x58, 0x43]),
            }),
            ..with_car.clone()
        };
        let state_bytes =
            rmp_serde::to_vec(&ServerMessage::DriverFeedback(with_state.clone())).unwrap();
        print_golden("S_DriverFeedbackOwnState", &state_bytes);
        assert_eq!(state_bytes[16], 0xdc, "sixteen fields (array16)");
        assert_eq!(&state_bytes[17..19], &[0x00, 0x10]);
        assert!(
            state_bytes.ends_with(&[
                0x95, 0xcd, 0x04, 0xa6, 0xcd, 0x01, 0x22, 0x01, 0x09, 0xc4, 0x04, 0x41, 0x50, 0x58,
                0x43
            ]),
            "the slice: [tick, seq, index, count, bin]"
        );

        // A slice without a snapshot sends the snapshot as nil.
        let only_state = DriverFeedback {
            own_car: None,
            ..with_state.clone()
        };
        let only = rmp_serde::to_vec(&ServerMessage::DriverFeedback(only_state.clone())).unwrap();
        assert_eq!(only[old.len() + 2], 0xc0);

        for msg in [base, with_car, with_state, only_state] {
            let bytes = rmp_serde::to_vec(&ServerMessage::DriverFeedback(msg.clone())).unwrap();
            match rmp_serde::from_slice(&bytes).unwrap() {
                ServerMessage::DriverFeedback(decoded) => assert_eq!(decoded, msg),
                other => panic!("Wrong message type: {other:?}"),
            }
        }
        // An older server's message decodes with neither.
        match rmp_serde::from_slice(&old).unwrap() {
            ServerMessage::DriverFeedback(decoded) => {
                assert!(decoded.own_car.is_none() && decoded.own_state.is_none())
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The pace race control holds the driver's car to
    /// (`cargo test driver_feedback_speed_cap_wire_format -- --nocapture`):
    /// `ApexUdpGolden::S_DriverFeedbackSpeedCap`, the fourteen fields, nil
    /// for the own car and its state, then the pace. At 0 it is left off and
    /// the message keeps the bytes of `S_DriverFeedbackFlatSpot`.
    #[test]
    fn test_driver_feedback_speed_cap_wire_format() {
        let base = DriverFeedback {
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
            flat_spot: [0, 40, 0, 100],
            own_car: None,
            own_state: None,
            speed_cap_mps: 0.0,
        };
        let old = rmp_serde::to_vec(&ServerMessage::DriverFeedback(base.clone())).unwrap();
        let capped = DriverFeedback {
            speed_cap_mps: 22.5,
            ..base.clone()
        };
        let bytes = rmp_serde::to_vec(&ServerMessage::DriverFeedback(capped.clone())).unwrap();
        print_golden("S_DriverFeedbackSpeedCap", &bytes);
        assert_eq!(old[16], 0x9e, "fourteen fields at 0");
        assert_eq!(
            &bytes[16..19],
            &[0xdc, 0x00, 0x11],
            "seventeen with the pace (array16)"
        );
        assert_eq!(&bytes[19..old.len() + 2], &old[17..]);
        assert!(
            bytes.ends_with(&[0xc0, 0xc0, 0xca, 0x41, 0xb4, 0x00, 0x00]),
            "nil, nil, then 22.5 as a float 32"
        );
        for msg in [base, capped] {
            let bytes = rmp_serde::to_vec(&ServerMessage::DriverFeedback(msg.clone())).unwrap();
            match rmp_serde::from_slice(&bytes).unwrap() {
                ServerMessage::DriverFeedback(decoded) => assert_eq!(decoded, msg),
                other => panic!("Wrong message type: {other:?}"),
            }
        }
    }

    /// The datagram a predicting driver gets, at its largest: a full
    /// telemetry interval of steering samples at the default rates, a
    /// snapshot with every number at its widest, and a full slice of the
    /// state. It stays well under a 1500-byte MTU (1472 bytes of UDP
    /// payload over IPv4), so it is never fragmented.
    #[test]
    fn own_car_feedback_fits_one_small_datagram() {
        // The server's default `network.telemetry_divisor` (its config.rs).
        let divisor = 7;
        // One steering sample per tick since the last message.
        let samples = divisor;
        let mut snapshot = golden_snapshot();
        snapshot.tick = u32::MAX;
        snapshot.input_seq = Some(u32::MAX);
        snapshot.gear = -1;
        snapshot.auto_shift_hold_ticks = u16::MAX;
        snapshot.auto_reverse_ticks = u16::MAX;
        snapshot.current_lap = u16::MAX;
        snapshot.centerline_idx = Some(u32::MAX);
        snapshot.flags = u8::MAX;
        let base = DriverFeedback {
            server_tick: u32::MAX,
            steer_torque: vec![0.1; samples],
            slip_ratio: [0.1; 4],
            slip_angle: [0.1; 4],
            surface: [2; 4],
            suspension_mps: [0.1; 4],
            abs_active: true,
            tc_active: true,
            impact_mps: 0.1,
            steer_kick: 0.1,
            steer_input: 0.1,
            steer_stiffness: 0.1,
            front_load: 0.1,
            flat_spot: [100; 4],
            own_car: None,
            own_state: None,
            speed_cap_mps: 0.0,
        };
        let size = |f: &DriverFeedback| {
            rmp_serde::to_vec(&ServerMessage::DriverFeedback(f.clone()))
                .unwrap()
                .len()
        };
        let plain = size(&base);
        let with_car = DriverFeedback {
            own_car: Some(Box::new(snapshot.clone())),
            ..base.clone()
        };
        let car = size(&with_car);
        let full = size(&DriverFeedback {
            own_state: Some(crate::own_car::StateChunk {
                tick: u32::MAX,
                input_seq: Some(u32::MAX),
                index: u16::MAX,
                count: u16::MAX,
                bytes: RecordBytes(vec![0xff; crate::own_car::STATE_CHUNK_BYTES]),
            }),
            ..with_car
        });
        println!(
            "DriverFeedback: {plain} bytes, {car} with the snapshot, {full} with a state slice"
        );
        assert!(
            car - plain < 300,
            "the snapshot costs {} bytes",
            car - plain
        );
        assert!(full <= 1200, "{full} bytes");
    }

    /// The positional shape of `CompactCarState`, pinned because the Unreal
    /// client reads it by position: one field inserted rather than appended
    /// and every value after it lands in the wrong place. The bytes are the
    /// client's `ApexUdpGolden::S_TelemetryCompactFlatSpot`; print them with
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
        state.over_rev_ratio = 1.18;
        state.shift_refused_s = 0.3;
        state.downshift_risk = true;
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
        state.damage.hit_at = crate::data::HitCentres {
            front: -0.5,
            rear: 0.0,
            left: 1.0,
            right: 0.25,
            // A second dent at the left flank's rear corner.
            second: [0.0, 0.0, -0.75, 0.0],
            second_pct: [0.0, 0.0, 3.0, 0.0],
        };
        state.ers_charge_pct = 64;
        state.ers_budget_pct = 255;
        state.ers_mode = crate::hybrid::ErsMode::Attack as u8;
        state.ers_deploying = true;
        state.ers_boost = true;
        for (tyre, (inner, outer)) in state.tires.each_mut().into_iter().zip([
            (88.0, 80.0),
            (90.0, 93.6),
            (100.0, 105.0),
            (300.0, 300.0),
        ]) {
            tyre.tread_c = [inner, tyre.temperature_c, outer];
        }
        state.brake_wear_pct = [12.4, 12.0, 30.6, 99.9];
        state.slide_flags = 0b0001_0010;
        state.ers_stint_pct = 73;
        state.recovery = Some(crate::recovery::Recovery {
            destination: crate::recovery::RecoverDestination::Track,
            left_s: 6.34,
            waited_s: 0.0,
        });
        for (tyre, spot) in state
            .tires
            .each_mut()
            .into_iter()
            .zip([0.0, 0.404, 0.0, 1.0])
        {
            tyre.flat_spot = spot;
        }
        // The road under each tyre and its state: the front right on a
        // puddle over marbles with a slow leak, the rear right flat.
        state.tyre_water = [0.05, 1.3, 0.0, 0.0];
        state.tyre_marbles = [0.0, 0.5, 0.0, 0.0];
        state.tires.front_right.leak_kpa_per_s = 2.0;
        state.tires.rear_right.punctured = true;
        state.tires.rear_right.leak_kpa_per_s = 2.0;
        // Rubber picked up on the front right, the rear left blistering.
        state.tires.front_right.pickup = 0.336;
        state.tires.rear_left.blister = 0.5;
        // Off into a gravel trap on the left, the right still on the grass.
        state.wheel_ground = [GROUND_GRAVEL, 0, GROUND_GRAVEL, 0];
        // Discs that wear, the front left half gone; 40% of the stint's
        // energy left.
        state.brake_discs_wear = true;
        state.brake_disc_wear_pct = [50.4, 12.0, 3.0, 0.0];
        state.stint_energy_pct = 40;
        // Shown the blue flag, a double yellow ahead.
        state.flag_blue = true;
        state.flag_yellow = 2;

        // Sector 4 (1000-1250 m) double, 3 warned.
        let msg = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![CompactCarState::from_car_state(&state, 0)],
            race_clock: None,
            sky: None,
            yellow_flags: Some(YellowFlags {
                sector_m: 250,
                sectors: vec![[3, 1], [4, 2]],
            }),
            race_phase: None,
            safety_car: None,
        });
        let bytes = rmp_serde::to_vec(&msg).unwrap();
        println!(
            "S_TelemetryCompactFlags: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );

        // The car is a 56-field array: 0xDC 0x00 0x38 is the array-16 header.
        assert!(
            bytes.windows(3).any(|w| w == [0xDC, 0x00, 0x38]),
            "CompactCarState must stay 56 fields; the client reads them by position"
        );

        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::TelemetryCompact(frame) => {
                let car = &frame.car_states[0];
                assert_eq!(car.lap_flags, 1, "the lap in progress is struck");
                assert_eq!(car.fuel_dl, 425, "42.46 L goes out as 42.5");
                assert_eq!(car.tyre_c, [84, 92, 103, 255], "rounded, held to a byte");
                assert_eq!(car.tyre_kpa, [176, 181, 190, 1], "0 is kept for unknown");
                assert_eq!(car.tow_pct, 17, "a 17% tow");
                assert_eq!(car.engine_flags, 7, "over-rev, refused and risk bits");
                assert_eq!(car.over_rev_pct, 18);
                assert_eq!(car.tyre_wear, [12, 13, 31, 100]);
                assert_eq!(car.compound, 0, "softs");
                assert_eq!(car.pit_flags, 7, "in the lane, on the limiter, in service");
                assert_eq!(car.service_ds, 73);
                assert_eq!(car.brake_c, [612, 598, 356, 350]);
                assert_eq!(car.water_c, 105);
                assert_eq!(car.damage, [23, 0, 8, 0, 100]);
                assert_eq!((car.ers_pct, car.ers_lap_pct), (64, 255));
                assert_eq!(car.ers_flags, 2 | 4 | 16, "attack, deploying, boost");
                assert_eq!(
                    car.tyre_c_edges,
                    [88, 80, 90, 94, 100, 105, 255, 255],
                    "the shoulders, rounded"
                );
                assert_eq!(car.brake_wear, [12, 12, 31, 100]);
                assert_eq!(car.slide_flags, 0b0001_0010, "FR sliding, FL locked");
                assert_eq!(car.ers_stint_pct, 73);
                assert_eq!(car.recover_ds, 63, "6.34 s of hold left");
                assert_eq!(car.flat_spot, [0, 40, 0, 100], "percent of the worst");
                assert_eq!(car.tyre_pickup, [0, 34, 0, 0], "percent picked up");
                assert_eq!(car.tyre_blister, [0, 0, 50, 0], "percent blistered");
                assert_eq!(
                    car.damage_at,
                    [-64, 0, 127, 32],
                    "127ths of the zone's half face"
                );
                assert_eq!(
                    car.damage_at2,
                    [-128, -128, -95, -128],
                    "a second dent on the left flank only"
                );
                assert_eq!(car.ground, 0b0001_0001, "gravel under the left wheels");
                assert_eq!(car.disc_wear, [50, 12, 3, 0], "the discs, rounded");
                assert_eq!(car.stint_energy_pct, 40);
                assert_eq!(
                    car.race_flags,
                    RACE_FLAG_BLUE | (2 << RACE_FLAG_YELLOW_SHIFT)
                );
                assert_eq!(car.last_lap_time_ms, Some(82_615));
                assert_eq!(car.gear, 4);
                assert_eq!(
                    frame.yellow_flags.as_ref().map(|f| f.sectors.clone()),
                    Some(vec![[3, 1], [4, 2]])
                );
                assert_eq!(frame.sky, None, "nil before the yellows");
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

    /// The driver's own box. Run
    /// `cargo test pit_box_wire_format -- --nocapture` into the client's
    /// `ApexGolden::S_PitBox` (ApexGoldenBlobs.h).
    #[test]
    fn test_pit_box_wire_format() {
        let msg = ServerMessage::PitBox(PitBoxData {
            pit_box: 4,
            x: 120.5,
            y: -36.25,
            z: 2.0,
            yaw_rad: 1.5,
            lateral_m: -2.5,
            lane_width_m: 12.0,
            radius_m: 2.5,
            manual: false,
        });
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{b:02X}"))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        println!("S_PitBox: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::PitBox(data) => {
                assert_eq!(data.pit_box, 4);
                assert_eq!((data.x, data.y, data.lateral_m), (120.5, -36.25, -2.5));
                assert!(!data.manual, "the autopilot, left off the wire");
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The pit stop message and the pit flag bits. Run
    /// `cargo test pit_service_wire_format -- --nocapture` and paste the
    /// output into the client's `ApexGoldenBlobs.h`.
    #[test]
    fn test_pit_service_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let service = ServerMessage::PitService(PitServiceData {
            car_index: 3,
            pit_box: 7,
            tyres_s: 9.0,
            compound: 0,
            fuel_s: 12.5,
            fuel_l: 25.0,
            repair_s: 2.0,
            repair_pct: 50.0,
            total_s: 23.5,
        });
        let bytes = rmp_serde::to_vec_named(&service).unwrap();
        println!("S_PitService: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::PitService(data) => {
                assert_eq!((data.car_index, data.pit_box, data.compound), (3, 7, 0));
                assert_eq!(data.total_s, 23.5);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
        assert_eq!(
            (
                PIT_FLAG_LIMITER,
                PIT_FLAG_SERVICING,
                PIT_FLAG_IN_LANE,
                PIT_FLAG_AUTOPILOT,
                PIT_FLAG_EXIT_CLOSED,
                PIT_FLAG_HELD,
                PIT_FLAG_REQUESTED
            ),
            (1, 2, 4, 8, 16, 32, 64),
            "the client decodes these bits by number"
        );
    }

    /// The bytes of a `SetPitPlan` with every order given, the client's
    /// `ApexGolden::C_SetPitPlan`, and of an empty one (`C_SetPitPlanEmpty`):
    /// `cargo test pit_plan_wire_format -- --nocapture` prints them.
    #[test]
    fn test_pit_plan_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let plan = ClientMessage::SetPitPlan(crate::pit::PitPlan {
            keep_tyres: true,
            compound: Some(2),
            fuel_l: Some(5.0),
            no_repair: true,
            pads: Some(true),
            pad_set: Some(-1),
            box_this_lap: true,
        });
        let bytes = rmp_serde::to_vec_named(&plan).unwrap();
        println!("C_SetPitPlan: {}", hex(&bytes));
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::SetPitPlan(p) => {
                assert!(p.keep_tyres && p.no_repair && p.box_this_lap);
                assert_eq!((p.compound, p.fuel_l), (Some(2), Some(5.0)));
                assert_eq!((p.pads, p.pad_set), (Some(true), Some(-1)));
            }
            other => panic!("Wrong message type: {other:?}"),
        }
        let empty = ClientMessage::SetPitPlan(crate::pit::PitPlan::default());
        let bytes = rmp_serde::to_vec_named(&empty).unwrap();
        println!("C_SetPitPlanEmpty: {}", hex(&bytes));
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::SetPitPlan(p) => assert_eq!(p, crate::pit::PitPlan::default()),
            other => panic!("Wrong message type: {other:?}"),
        }
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
    fn test_track_corners_wire_format() {
        let session_id = Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap();
        let corners = ServerMessage::TrackCorners(TrackCornersData {
            session_id,
            track_length_m: 5793.0,
            corners: vec![
                CornerData {
                    number: 1,
                    name: "Variante del Rettifilo".to_string(),
                    entry_m: 5700.0,
                    apex_m: 120.0,
                    exit_m: 210.0,
                    left: false,
                },
                CornerData {
                    number: 2,
                    name: String::new(),
                    entry_m: 900.0,
                    apex_m: 950.0,
                    exit_m: 1000.0,
                    left: true,
                },
            ],
        });
        let bytes = rmp_serde::to_vec_named(&corners).unwrap();
        println!(
            "S_TrackCorners: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::TrackCorners(data) => {
                assert_eq!(data.corners.len(), 2);
                assert_eq!(data.corners[0].name, "Variante del Rettifilo");
                assert!(data.corners[1].left);
                assert_eq!(data.track_length_m, 5793.0);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// Golden bytes for `RoadState` -> `ApexGolden::S_RoadState`; run
    /// `cargo test road_state_wire_format -- --nocapture` and paste the
    /// output into the client's `ApexGoldenBlobs.h`.
    #[test]
    fn test_road_state_wire_format() {
        let session_id = Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap();
        // Two cells of four bins each: a small fixture the client decodes,
        // not the real BINS. The row is depth then, per bin, rubber/marbles/dry.
        let rows = vec![
            80, // cell 0 depth (0.80 of heavy rain)
            128, 0, 0, 200, 10, 0, 255, 20, 40, 60, 5, 200, // its 4 bins
            0,   // cell 1 depth (dry)
            128, 0, 0, 170, 0, 0, 191, 0, 0, 128, 0, 0, // its 4 bins
        ];
        // Cell 0 at (100, -20) facing +X (left is +Y); cell 1 at (110, -20).
        let geometry: Vec<u8> = [100.0f32, -20.0, 0.0, 1.0, 110.0, -20.0, 0.0, 1.0]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect();
        let msg = ServerMessage::RoadState(RoadStateData {
            session_id,
            lap_m: 5793.0,
            cell_m: 10.0,
            bins: 4,
            half_span_m: 16.0,
            first_cell: 7,
            rows: RecordBytes(rows.clone()),
            debris: vec![[12.5, -3.25], [900.0, 4.0]],
            geometry: RecordBytes(geometry.clone()),
            snow_rows: RecordBytes(vec![]),
            water_rows: RecordBytes(vec![]),
            lane_snow: RecordBytes(vec![]),
        });
        let bytes = rmp_serde::to_vec_named(&msg).unwrap();
        println!(
            "S_RoadState: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::RoadState(data) => {
                assert_eq!(data.first_cell, 7);
                assert_eq!(data.bins, 4);
                assert_eq!(data.rows.0, rows);
                assert_eq!(data.debris.len(), 2);
                assert_eq!(data.debris[1], [900.0, 4.0]);
                assert_eq!(data.lap_m, 5793.0);
                assert_eq!(data.geometry.0, geometry);
                assert!(data.snow_rows.is_empty(), "left off the wire without snow");
            }
            other => panic!("Wrong message type: {other:?}"),
        }
        // The same road under snow: the verge, then per bin the cover and
        // the ice, for the same two cells of four bins.
        let snow_rows = vec![
            255, 255, 0, 200, 0, 128, 0, 60, 90, // cell 0
            0, 0, 0, 0, 0, 0, 0, 0, 0, // cell 1
        ];
        let snowy = ServerMessage::RoadState(RoadStateData {
            session_id,
            lap_m: 5793.0,
            cell_m: 10.0,
            bins: 4,
            half_span_m: 16.0,
            first_cell: 7,
            rows: RecordBytes(rows.clone()),
            debris: vec![[12.5, -3.25], [900.0, 4.0]],
            geometry: RecordBytes(geometry.clone()),
            snow_rows: RecordBytes(snow_rows.clone()),
            water_rows: RecordBytes(vec![]),
            lane_snow: RecordBytes(vec![]),
        });
        let snowy_bytes = rmp_serde::to_vec_named(&snowy).unwrap();
        println!(
            "S_RoadStateSnow: {}",
            snowy_bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&snowy_bytes).unwrap() {
            ServerMessage::RoadState(data) => assert_eq!(data.snow_rows.0, snow_rows),
            other => panic!("Wrong message type: {other:?}"),
        }
        // The same road with a puddle standing on cell 0's second bin
        // (1.6 of heavy rain), the flat road's 0.8 either side.
        let water_rows = vec![
            80, 160, 80, 80, // cell 0
            0, 0, 0, 0, // cell 1
        ];
        let puddled = ServerMessage::RoadState(RoadStateData {
            session_id,
            lap_m: 5793.0,
            cell_m: 10.0,
            bins: 4,
            half_span_m: 16.0,
            first_cell: 7,
            rows: RecordBytes(rows.clone()),
            debris: vec![[12.5, -3.25], [900.0, 4.0]],
            geometry: RecordBytes(geometry),
            snow_rows: RecordBytes(vec![]),
            water_rows: RecordBytes(water_rows.clone()),
            lane_snow: RecordBytes(vec![]),
        });
        let puddled_bytes = rmp_serde::to_vec_named(&puddled).unwrap();
        println!(
            "S_RoadStatePuddle: {}",
            puddled_bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&puddled_bytes).unwrap() {
            ServerMessage::RoadState(data) => {
                assert_eq!(data.water_rows.0, water_rows);
                assert!(data.snow_rows.is_empty());
            }
            other => panic!("Wrong message type: {other:?}"),
        }
        // The same road under snow with a pit lane: three lane cells, the
        // first packed by the cars in it.
        let lane = ServerMessage::RoadState(RoadStateData {
            session_id,
            lap_m: 5793.0,
            cell_m: 10.0,
            bins: 4,
            half_span_m: 16.0,
            first_cell: 7,
            rows: RecordBytes(rows.clone()),
            debris: vec![],
            geometry: RecordBytes(vec![0; 32]),
            snow_rows: RecordBytes(snow_rows.clone()),
            water_rows: RecordBytes(vec![]),
            lane_snow: RecordBytes(vec![128, 255, 255]),
        });
        let lane_bytes = rmp_serde::to_vec_named(&lane).unwrap();
        println!(
            "S_RoadStateLaneSnow: {}",
            lane_bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&lane_bytes).unwrap() {
            ServerMessage::RoadState(data) => assert_eq!(data.lane_snow.0, vec![128, 255, 255]),
            other => panic!("Wrong message type: {other:?}"),
        }
    }

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
            },
            conditions: SessionConditions {
                weather: Weather::LightRain,
                time_of_day_minutes: 21 * 60 + 30,
                ..SessionConditions::DEFAULT
            },
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
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
            pit_autopilot: Some(false),
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
            },
            conditions: SessionConditions {
                weather: Weather::HeavyRain,
                time_of_day_minutes: 6 * 60 + 15,
                ..SessionConditions::DEFAULT
            },
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedAssists: {}", hex(&joined_bytes));

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION);
        assert_eq!(aids_bytes, GOLDEN_C_SET_DRIVER_AIDS);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_ASSISTS);
    }

    /// A driver's `SessionJoined` names the session's tick rate and road
    /// (`Physics`, last); without it, the bytes above.
    /// `ApexGolden::S_SessionJoinedPhysics` (ProtocolCodecTests.cpp).
    #[test]
    fn session_joined_physics_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        const GOLDEN: &[u8] = &[
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E,
            0x4A, 0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53,
            0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66,
            0x2D, 0x30, 0x31, 0x32, 0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62,
            0x63, 0x64, 0x65, 0x66, 0xB0, 0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50,
            0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x02, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69,
            0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01, 0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
            0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
            0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74,
            0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72,
            0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F,
            0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67,
            0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69,
            0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
            0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69,
            0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA7, 0x50, 0x68, 0x79, 0x73, 0x69,
            0x63, 0x73, 0x82, 0xAC, 0x74, 0x69, 0x63, 0x6B, 0x5F, 0x72, 0x61, 0x74, 0x65, 0x5F,
            0x68, 0x7A, 0xCD, 0x01, 0xA4, 0xAC, 0x72, 0x6F, 0x61, 0x64, 0x5F, 0x63, 0x6F, 0x6E,
            0x74, 0x61, 0x63, 0x74, 0x01,
        ];
        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 2,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: Some(SessionPhysics {
                tick_rate_hz: 420,
                road_contact: 1,
                ..Default::default()
            }),
            classes: Vec::new(),
        });
        let bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedPhysics: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(
                data.physics,
                Some(SessionPhysics {
                    tick_rate_hz: 420,
                    road_contact: 1,
                    ..Default::default()
                })
            ),
            _ => panic!("Wrong message type"),
        }
        assert_eq!(bytes, GOLDEN);
    }

    /// A balanced car's figures in `SessionJoined.Physics`
    /// (`cargo test session_joined_balance_wire_format -- --nocapture` ->
    /// `ApexGolden::S_SessionJoinedPhysicsBalance`): `balance` after the
    /// rest, left off for a car no table names.
    #[test]
    fn session_joined_balance_wire_format() {
        let physics = SessionPhysics {
            tick_rate_hz: 420,
            road_contact: 1,
            balance: Some(BalanceFigures {
                ballast_kg: 25.0,
                power: 0.95,
                downforce: 1.1,
            }),
            ..Default::default()
        };
        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 2,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: Some(physics),
            classes: Vec::new(),
        });
        let bytes = rmp_serde::to_vec_named(&joined).unwrap();
        print_golden("S_SessionJoinedPhysicsBalance", &bytes);
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.physics, Some(physics)),
            _ => panic!("Wrong message type"),
        }
        let tail: &[u8] = b"balance";
        assert!(bytes.windows(tail.len()).any(|w| w == tail));
    }

    /// The track's physics bundle (`crate::track_bundle`): `SessionJoined`'s
    /// `Physics` names it (`bundle_crc`, `bundle_bytes`, `bundle_chunks`,
    /// left off without one: the bytes above), the client asks for a chunk
    /// (`RequestTrackBundle`) and gets it (`TrackBundleChunk`), or none
    /// (`Chunks` 0). `cargo test track_bundle_wire_format -- --nocapture`
    /// -> `ApexGolden::S_SessionJoinedPhysicsBundle`, `C_RequestTrackBundle`,
    /// `S_TrackBundleChunk`, `S_TrackBundleChunkMissing`.
    #[test]
    fn track_bundle_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        const GOLDEN_S_SESSION_JOINED_PHYSICS_BUNDLE: &[u8] = &[
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E,
            0x4A, 0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53,
            0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66,
            0x2D, 0x30, 0x31, 0x32, 0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62,
            0x63, 0x64, 0x65, 0x66, 0xB0, 0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50,
            0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x02, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69,
            0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01, 0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
            0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
            0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74,
            0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72,
            0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F,
            0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67,
            0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69,
            0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
            0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69,
            0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA7, 0x50, 0x68, 0x79, 0x73, 0x69,
            0x63, 0x73, 0x85, 0xAC, 0x74, 0x69, 0x63, 0x6B, 0x5F, 0x72, 0x61, 0x74, 0x65, 0x5F,
            0x68, 0x7A, 0xCD, 0x01, 0xA4, 0xAC, 0x72, 0x6F, 0x61, 0x64, 0x5F, 0x63, 0x6F, 0x6E,
            0x74, 0x61, 0x63, 0x74, 0x01, 0xAA, 0x62, 0x75, 0x6E, 0x64, 0x6C, 0x65, 0x5F, 0x63,
            0x72, 0x63, 0xCE, 0xC0, 0xFF, 0xEE, 0x01, 0xAC, 0x62, 0x75, 0x6E, 0x64, 0x6C, 0x65,
            0x5F, 0x62, 0x79, 0x74, 0x65, 0x73, 0xCE, 0x00, 0x21, 0x6A, 0xB0, 0xAD, 0x62, 0x75,
            0x6E, 0x64, 0x6C, 0x65, 0x5F, 0x63, 0x68, 0x75, 0x6E, 0x6B, 0x73, 0x09,
        ];
        const GOLDEN_C_REQUEST_TRACK_BUNDLE: &[u8] = &[
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xB2, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74,
            0x54, 0x72, 0x61, 0x63, 0x6B, 0x42, 0x75, 0x6E, 0x64, 0x6C, 0x65, 0xA4, 0x64, 0x61,
            0x74, 0x61, 0x82, 0xA3, 0x63, 0x72, 0x63, 0xCE, 0xC0, 0xFF, 0xEE, 0x01, 0xA5, 0x63,
            0x68, 0x75, 0x6E, 0x6B, 0x03,
        ];
        const GOLDEN_S_TRACK_BUNDLE_CHUNK: &[u8] = &[
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xB0, 0x54, 0x72, 0x61, 0x63, 0x6B, 0x42, 0x75,
            0x6E, 0x64, 0x6C, 0x65, 0x43, 0x68, 0x75, 0x6E, 0x6B, 0xA4, 0x64, 0x61, 0x74, 0x61,
            0x84, 0xA3, 0x43, 0x72, 0x63, 0xCE, 0xC0, 0xFF, 0xEE, 0x01, 0xA5, 0x43, 0x68, 0x75,
            0x6E, 0x6B, 0x03, 0xA6, 0x43, 0x68, 0x75, 0x6E, 0x6B, 0x73, 0x09, 0xA5, 0x42, 0x79,
            0x74, 0x65, 0x73, 0xC4, 0x05, 0x41, 0x50, 0x58, 0x54, 0x01,
        ];
        const GOLDEN_S_TRACK_BUNDLE_CHUNK_MISSING: &[u8] = &[
            0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xB0, 0x54, 0x72, 0x61, 0x63, 0x6B, 0x42, 0x75,
            0x6E, 0x64, 0x6C, 0x65, 0x43, 0x68, 0x75, 0x6E, 0x6B, 0xA4, 0x64, 0x61, 0x74, 0x61,
            0x84, 0xA3, 0x43, 0x72, 0x63, 0xCE, 0xC0, 0xFF, 0xEE, 0x01, 0xA5, 0x43, 0x68, 0x75,
            0x6E, 0x6B, 0x03, 0xA6, 0x43, 0x68, 0x75, 0x6E, 0x6B, 0x73, 0x00, 0xA5, 0x42, 0x79,
            0x74, 0x65, 0x73, 0xC4, 0x00,
        ];
        let physics = SessionPhysics {
            tick_rate_hz: 420,
            road_contact: 1,
            bundle_crc: 0xC0FF_EE01,
            bundle_bytes: 2_190_000,
            bundle_chunks: 9,
            balance: None,
        };
        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 2,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: Some(physics),
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedPhysicsBundle: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.physics, Some(physics)),
            _ => panic!("Wrong message type"),
        }

        let request = ClientMessage::RequestTrackBundle {
            crc: 0xC0FF_EE01,
            chunk: 3,
        };
        let request_bytes = rmp_serde::to_vec_named(&request).unwrap();
        println!("C_RequestTrackBundle: {}", hex(&request_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&request_bytes).unwrap() {
            ClientMessage::RequestTrackBundle { crc, chunk } => {
                assert_eq!((crc, chunk), (0xC0FF_EE01, 3))
            }
            _ => panic!("Wrong message type"),
        }

        let chunk = ServerMessage::TrackBundleChunk(TrackBundleChunkData {
            crc: 0xC0FF_EE01,
            chunk: 3,
            chunks: 9,
            bytes: RecordBytes(vec![0x41, 0x50, 0x58, 0x54, 0x01]),
        });
        let chunk_bytes = rmp_serde::to_vec_named(&chunk).unwrap();
        println!("S_TrackBundleChunk: {}", hex(&chunk_bytes));
        let missing = ServerMessage::TrackBundleChunk(TrackBundleChunkData {
            crc: 0xC0FF_EE01,
            chunk: 3,
            chunks: 0,
            bytes: RecordBytes(Vec::new()),
        });
        let missing_bytes = rmp_serde::to_vec_named(&missing).unwrap();
        println!("S_TrackBundleChunkMissing: {}", hex(&missing_bytes));
        for (bytes, chunks) in [(&chunk_bytes, 9), (&missing_bytes, 0)] {
            match rmp_serde::from_slice::<ServerMessage>(bytes).unwrap() {
                ServerMessage::TrackBundleChunk(data) => {
                    assert_eq!(
                        (data.crc, data.chunk, data.chunks),
                        (0xC0FF_EE01, 3, chunks)
                    );
                    assert_eq!(data.bytes.0.len(), if chunks > 0 { 5 } else { 0 });
                }
                _ => panic!("Wrong message type"),
            }
        }
        assert!(matches!(chunk.priority(), MessagePriority::Critical));
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_PHYSICS_BUNDLE);
        assert_eq!(request_bytes, GOLDEN_C_REQUEST_TRACK_BUNDLE);
        assert_eq!(chunk_bytes, GOLDEN_S_TRACK_BUNDLE_CHUNK);
        assert_eq!(missing_bytes, GOLDEN_S_TRACK_BUNDLE_CHUNK_MISSING);
    }

    /// A listed session names its damage rule, and Full stays off the wire.
    #[test]
    fn test_session_summary_names_a_damage_rule() {
        let summary = |damage| SessionSummary {
            id: Uuid::nil(),
            track_name: String::new(),
            track_file: String::new(),
            track_id: Uuid::nil(),
            host_name: String::new(),
            session_kind: SessionKind::Multiplayer,
            player_count: 1,
            max_players: 8,
            state: SessionState::Lobby,
            conditions: SessionConditions::DEFAULT,
            damage,
            lap_limit: 3,
            race_seconds: None,
            weekend_waiting: false,
            mandatory_stops: 0,
            compound_change: false,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let reduced = rmp_serde::to_vec_named(&summary(DamageLevel::Reduced)).unwrap();
        let full = rmp_serde::to_vec_named(&summary(DamageLevel::Full)).unwrap();
        let has = |bytes: &[u8]| {
            bytes
                .windows(7)
                .any(|w| w == [0xa6, b'D', b'a', b'm', b'a', b'g', b'e'])
        };
        assert!(has(&reduced) && !has(&full));
        let back: SessionSummary = rmp_serde::from_slice(&reduced).unwrap();
        assert_eq!(back.damage, DamageLevel::Reduced);
        let back: SessionSummary = rmp_serde::from_slice(&full).unwrap();
        assert_eq!(back.damage, DamageLevel::Full);
    }

    /// The session's damage rule: a create that picks reduced damage and
    /// the joined echo of a session without any. Full is the default and
    /// left off the wire. Pinned on the client as
    /// `ApexGolden::C_CreateSessionDamage` / `S_SessionJoinedNoDamage`;
    /// `cargo test session_damage_wire_format -- --nocapture` prints them.
    #[test]
    fn test_session_damage_wire_format() {
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
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Reduced,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionDamage: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { damage, .. } => assert_eq!(damage, DamageLevel::Reduced),
            _ => panic!("Wrong message type"),
        }

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Off,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedNoDamage: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.damage, DamageLevel::Off),
            _ => panic!("Wrong message type"),
        }

        // Full damage is the default and stays off the wire: a create and a
        // join from before the rule keep their bytes, and decode as full.
        let full = ClientMessage::CreateSession {
            track_config_id: Uuid::nil(),
            max_players: 2,
            ai_count: 0,
            lap_limit: 1,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let full_bytes = rmp_serde::to_vec_named(&full).unwrap();
        assert!(!full_bytes.windows(6).any(|w| w == b"damage"));
        match rmp_serde::from_slice::<ClientMessage>(&full_bytes).unwrap() {
            ClientMessage::CreateSession { damage, .. } => assert_eq!(damage, DamageLevel::Full),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_DAMAGE);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_NO_DAMAGE);

        // Visual-only damage is the next integer: the same echo with
        // `Damage: 3` (`ApexGolden::S_SessionJoinedVisualDamage`).
        let visual = match joined {
            ServerMessage::SessionJoined(data) => ServerMessage::SessionJoined(SessionJoinedData {
                damage: DamageLevel::VisualOnly,
                ..data
            }),
            _ => unreachable!(),
        };
        let visual_bytes = rmp_serde::to_vec_named(&visual).unwrap();
        println!("S_SessionJoinedVisualDamage: {}", hex(&visual_bytes));
        let differ: Vec<usize> = (0..visual_bytes.len())
            .filter(|&i| visual_bytes[i] != joined_bytes[i])
            .collect();
        assert_eq!(visual_bytes.len(), joined_bytes.len());
        assert_eq!(differ.len(), 1);
        assert_eq!(visual_bytes[differ[0]], 3);
        match rmp_serde::from_slice::<ServerMessage>(&visual_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => {
                assert_eq!(data.damage, DamageLevel::VisualOnly)
            }
            _ => panic!("Wrong message type"),
        }
    }

    /// The qualifying cut-off (the 107% rule): a create that sets it and
    /// the joined echo. None stays off the wire. Printed as
    /// `C_CreateSessionQualifyingCutoff` / `S_SessionJoinedQualifyingCutoff`
    /// (`cargo test qualifying_cutoff_wire_format -- --nocapture`); the
    /// client keeps the bytes until its codec sends and reads the key.
    #[test]
    fn test_qualifying_cutoff_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |cutoff: Option<u16>| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: cutoff,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(Some(107))).unwrap();
        println!("C_CreateSessionQualifyingCutoff: {}", hex(&create_bytes));
        assert!(create_bytes
            .windows(21)
            .any(|w| w == b"qualifying_cutoff_pct"));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession {
                qualifying_cutoff_pct,
                ..
            } => assert_eq!(qualifying_cutoff_pct, Some(107)),
            _ => panic!("Wrong message type"),
        }
        let none_bytes = rmp_serde::to_vec_named(&create(None)).unwrap();
        assert!(!String::from_utf8_lossy(&none_bytes).contains("qualifying_cutoff_pct"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: Some(107),
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedQualifyingCutoff: {}", hex(&joined_bytes));
        assert!(joined_bytes
            .windows(19)
            .any(|w| w == b"QualifyingCutoffPct"));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => {
                assert_eq!(data.qualifying_cutoff_pct, Some(107))
            }
            _ => panic!("Wrong message type"),
        }
    }

    /// A one-lap shoot-out with a lap allowance: the create, the joined
    /// echo, a `ShootoutState` with the second of three cars on its flying
    /// lap, a weekend whose practice and qualifying name their own damage
    /// rules, and a `WeekendState` under a phase's rule. The usual format,
    /// no allowance and full damage stay off the wire. Printed as
    /// `C_CreateSessionShootout` / `S_SessionJoinedShootout` /
    /// `S_ShootoutState` / `C_CreateSessionWeekendDamage` /
    /// `S_WeekendStateDamage`
    /// (`cargo test shootout_wire_format -- --nocapture`).
    #[test]
    fn test_shootout_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |format: QualifyingFormat,
                      allowance: u8,
                      weekend: Option<crate::weekend::WeekendFormat>| {
            ClientMessage::CreateSession {
                track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
                max_players: 8,
                ai_count: 3,
                lap_limit: 5,
                session_kind: SessionKind::Multiplayer,
                allowed_assists: AllowedAssists::default(),
                conditions: SessionConditions::DEFAULT,
                damage: DamageLevel::Full,
                ai_skill: None,
                race_seconds: None,
                grid_order: Vec::new(),
                qualifying_seconds: None,
                weekend,
                watch: None,
                qualifying_cutoff_pct: None,
                mandatory_stops: 0,
                compound_change: false,
                qualifying_format: format,
                qualifying_lap_allowance: allowance,
                race_rules: Default::default(),
                classes: Vec::new(),
            }
        };
        let create_bytes =
            rmp_serde::to_vec_named(&create(QualifyingFormat::Shootout, 3, None)).unwrap();
        println!("C_CreateSessionShootout: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession {
                qualifying_format,
                qualifying_lap_allowance,
                ..
            } => assert_eq!(
                (qualifying_format, qualifying_lap_allowance),
                (QualifyingFormat::Shootout, 3)
            ),
            _ => panic!("Wrong message type"),
        }
        let open = String::from_utf8_lossy(
            &rmp_serde::to_vec_named(&create(QualifyingFormat::Open, 0, None)).unwrap(),
        )
        .into_owned();
        assert!(!open.contains("qualifying_format") && !open.contains("lap_allowance"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Shootout,
            qualifying_lap_allowance: 3,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedShootout: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(
                (data.qualifying_format, data.qualifying_lap_allowance),
                (QualifyingFormat::Shootout, 3)
            ),
            _ => panic!("Wrong message type"),
        }

        let state = ServerMessage::ShootoutState(ShootoutStateData {
            order: vec![2, 0, 1],
            running: 2,
            done: 1,
            flying: true,
        });
        let state_bytes = rmp_serde::to_vec_named(&state).unwrap();
        println!("S_ShootoutState: {}", hex(&state_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&state_bytes).unwrap() {
            ServerMessage::ShootoutState(data) => {
                assert_eq!(data.order, vec![2, 0, 1]);
                assert_eq!((data.running, data.done, data.flying), (2, 1, true));
            }
            _ => panic!("Wrong message type"),
        }

        let weekend = crate::weekend::WeekendFormat {
            practice_seconds: 600,
            qualifying_seconds: vec![600],
            practice_damage: Some(DamageLevel::Off),
            qualifying_damage: Some(DamageLevel::Reduced),
            ..Default::default()
        };
        let weekend_bytes =
            rmp_serde::to_vec_named(&create(QualifyingFormat::Open, 0, Some(weekend.clone())))
                .unwrap();
        println!("C_CreateSessionWeekendDamage: {}", hex(&weekend_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&weekend_bytes).unwrap() {
            ClientMessage::CreateSession { weekend: back, .. } => {
                assert_eq!(back, Some(weekend))
            }
            _ => panic!("Wrong message type"),
        }
        let plain = crate::weekend::WeekendFormat {
            practice_seconds: 600,
            qualifying_seconds: vec![600],
            ..Default::default()
        };
        let plain_bytes = rmp_serde::to_vec_named(&plain).unwrap();
        assert!(!String::from_utf8_lossy(&plain_bytes).contains("damage"));

        let phase = ServerMessage::WeekendState(WeekendStateData {
            phase: 1,
            round: 0,
            rounds: 1,
            finished: false,
            advancing: 0,
            eliminated: vec![],
            damage: DamageLevel::Off,
            conditions: None,
        });
        let phase_bytes = rmp_serde::to_vec_named(&phase).unwrap();
        println!("S_WeekendStateDamage: {}", hex(&phase_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&phase_bytes).unwrap() {
            ServerMessage::WeekendState(data) => assert_eq!(data.damage, DamageLevel::Off),
            _ => panic!("Wrong message type"),
        }
        assert!(!String::from_utf8_lossy(&phase_bytes).contains("Conditions"));

        // A day with its own sky (`S_WeekendStateConditions`): practice in
        // the rain at 09:00, 14 °C.
        let day = SessionConditions {
            weather: Weather::HeavyRain,
            time_of_day_minutes: 540,
            air_temp_c: Some(14),
            ..SessionConditions::DEFAULT
        };
        let day_state = ServerMessage::WeekendState(WeekendStateData {
            phase: 1,
            round: 0,
            rounds: 1,
            finished: false,
            advancing: 0,
            eliminated: vec![],
            damage: DamageLevel::Full,
            conditions: Some(day),
        });
        let day_bytes = rmp_serde::to_vec_named(&day_state).unwrap();
        println!("S_WeekendStateConditions: {}", hex(&day_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&day_bytes).unwrap() {
            ServerMessage::WeekendState(data) => assert_eq!(data.conditions, Some(day)),
            _ => panic!("Wrong message type"),
        }
    }

    /// The pit rule: a create for two mandatory stops on two compounds,
    /// the joined echo, and a roster row that missed them. No rule stays
    /// off the wire. Printed as `C_CreateSessionPitRule` /
    /// `S_SessionJoinedPitRule` / `S_SessionRosterMissedStops`
    /// (`cargo test pit_rule_wire_format -- --nocapture`).
    #[test]
    fn test_pit_rule_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |stops: u8, change: bool| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: stops,
            compound_change: change,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(2, true)).unwrap();
        println!("C_CreateSessionPitRule: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession {
                mandatory_stops,
                compound_change,
                ..
            } => assert_eq!((mandatory_stops, compound_change), (2, true)),
            _ => panic!("Wrong message type"),
        }
        let none = String::from_utf8_lossy(&rmp_serde::to_vec_named(&create(0, false)).unwrap())
            .into_owned();
        assert!(!none.contains("mandatory_stops") && !none.contains("compound_change"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 2,
            compound_change: true,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedPitRule: {}", hex(&joined_bytes));
        assert!(joined_bytes.windows(14).any(|w| w == b"MandatoryStops"));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => {
                assert_eq!((data.mandatory_stops, data.compound_change), (2, true))
            }
            _ => panic!("Wrong message type"),
        }

        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![RosterEntry {
                car_index: 0,
                player_id: Uuid::parse_str("11111111-2222-3333-4444-555555555555").unwrap(),
                player_name: "Ana".into(),
                is_ai: true,
                car_config_id: Uuid::nil(),
                livery: 0,
                away: false,
                grid_slot: 0,
                ready: false,
                dnq: false,
                missed_stops: true,
                qualifying_laps: 0,
                penalties: Vec::new(),
            }],
        });
        let roster_bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!("S_SessionRosterMissedStops: {}", hex(&roster_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&roster_bytes).unwrap() {
            ServerMessage::SessionRoster(data) => assert!(data.entries[0].missed_stops),
            _ => panic!("Wrong message type"),
        }
    }

    /// The race-control rules: a create for a rolling start (with false
    /// starts watched and a formation lap asked for), the joined echo, and
    /// a telemetry frame on the pace lap (`race_phase` 2, after a nil
    /// clock, sky and yellows). No rule stays off the wire. Printed as
    /// `C_CreateSessionRaceRules` / `S_SessionJoinedRaceRules` /
    /// `S_TelemetryCompactPaceLap` (`cargo test race_rules_wire_format --
    /// --nocapture`).
    #[test]
    fn test_race_rules_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let rules = RaceRules {
            start: StartType::Rolling,
            formation_lap: true,
            jump_start: true,
            ..Default::default()
        };
        let create = |race_rules: RaceRules| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules,
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(rules)).unwrap();
        println!("C_CreateSessionRaceRules: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { race_rules, .. } => assert_eq!(race_rules, rules),
            _ => panic!("Wrong message type"),
        }
        let none = String::from_utf8_lossy(
            &rmp_serde::to_vec_named(&create(RaceRules::default())).unwrap(),
        )
        .into_owned();
        assert!(!none.contains("race_rules"));
        // One rule set: only its key goes.
        let one = String::from_utf8_lossy(
            &rmp_serde::to_vec_named(&create(RaceRules {
                jump_start: true,
                ..Default::default()
            }))
            .unwrap(),
        )
        .into_owned();
        assert!(one.contains("jump_start") && !one.contains("formation_lap"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: rules,
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedRaceRules: {}", hex(&joined_bytes));
        assert!(joined_bytes.windows(9).any(|w| w == b"RaceRules"));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.race_rules, rules),
            _ => panic!("Wrong message type"),
        }

        let frame = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 500,
            session_state: SessionState::Countdown,
            game_mode: GameMode::Countdown,
            countdown_ms: None,
            car_states: vec![],
            race_clock: None,
            sky: None,
            yellow_flags: None,
            race_phase: Some(crate::race_start::RacePhase::PaceLap as u8),
            safety_car: None,
        });
        let frame_bytes = rmp_serde::to_vec(&frame).unwrap();
        println!("S_TelemetryCompactPaceLap: {}", hex(&frame_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&frame_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => {
                assert_eq!(t.race_phase, Some(2));
                assert_eq!((t.race_clock, t.sky, t.yellow_flags), (None, None, None));
            }
            _ => panic!("Wrong message type"),
        }
        // Without a phase the frame is the five fields it always was.
        let bare = rmp_serde::to_vec(&ServerMessage::TelemetryCompact(CompactTelemetry {
            race_phase: None,
            safety_car: None,
            ..match frame {
                ServerMessage::TelemetryCompact(t) => t,
                _ => unreachable!(),
            }
        }))
        .unwrap();
        assert_eq!(
            bare.len(),
            frame_bytes.len() - 4,
            "nil clock, sky, yellows and the phase"
        );
    }

    /// Penalties: a create naming the penalised offences (a pass under a
    /// yellow, a red-light pit exit) and a roster row carrying a served
    /// drive-through and an added time penalty. Printed as
    /// `C_CreateSessionPenalties` / `S_SessionRosterPenalties` (`cargo test
    /// penalties_wire_format -- --nocapture`).
    #[test]
    fn test_penalties_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let rules = RaceRules {
            yellow_penalty: true,
            pit_exit_penalty: true,
            ..Default::default()
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: rules,
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionPenalties: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { race_rules, .. } => assert_eq!(race_rules, rules),
            _ => panic!("Wrong message type"),
        }

        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![RosterEntry {
                car_index: 0,
                player_id: Uuid::parse_str("11111111-2222-3333-4444-555555555555").unwrap(),
                player_name: "Ana".into(),
                is_ai: true,
                car_config_id: Uuid::nil(),
                livery: 0,
                away: false,
                grid_slot: 0,
                ready: false,
                dnq: false,
                missed_stops: false,
                qualifying_laps: 0,
                penalties: vec![
                    PenaltyData {
                        reason: 0,
                        kind: 1,
                        seconds: 0,
                        lap: 1,
                        status: 1,
                    },
                    PenaltyData {
                        reason: 1,
                        kind: 0,
                        seconds: 5,
                        lap: 4,
                        status: 2,
                    },
                ],
            }],
        });
        let roster_bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!("S_SessionRosterPenalties: {}", hex(&roster_bytes));
        assert!(roster_bytes.windows(9).any(|w| w == b"Penalties"));
        match rmp_serde::from_slice::<ServerMessage>(&roster_bytes).unwrap() {
            ServerMessage::SessionRoster(data) => {
                assert_eq!(data.entries[0].penalties.len(), 2);
                assert_eq!(data.entries[0].penalties[1].seconds, 5);
            }
            _ => panic!("Wrong message type"),
        }
    }

    /// The virtual safety car: a create asking for it, and a frame while it
    /// is out (`race_phase` 3, after a nil clock, sky and yellows). Printed
    /// as `C_CreateSessionSafetyCar` / `S_TelemetryCompactVsc` (`cargo test
    /// safety_car_wire_format -- --nocapture`).
    #[test]
    fn test_safety_car_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let rules = RaceRules {
            safety_car: SafetyCar::Virtual,
            ..Default::default()
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: rules,
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionSafetyCar: {}", hex(&create_bytes));
        assert!(create_bytes.windows(10).any(|w| w == b"safety_car"));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { race_rules, .. } => assert_eq!(race_rules, rules),
            _ => panic!("Wrong message type"),
        }
        let frame = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 500,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![],
            race_clock: None,
            sky: None,
            yellow_flags: None,
            race_phase: Some(crate::race_start::RacePhase::Vsc as u8),
            safety_car: None,
        });
        let frame_bytes = rmp_serde::to_vec(&frame).unwrap();
        println!("S_TelemetryCompactVsc: {}", hex(&frame_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&frame_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => assert_eq!(t.race_phase, Some(3)),
            _ => panic!("Wrong message type"),
        }
    }

    /// A reverse grid: a create asking for the first 8 slots reversed.
    /// Printed as `C_CreateSessionReverseGrid` (`cargo test
    /// reverse_grid_wire_format -- --nocapture`).
    #[test]
    fn test_reverse_grid_wire_format() {
        let rules = RaceRules {
            reverse_grid: 8,
            ..Default::default()
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: rules,
            classes: Vec::new(),
        };
        let bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!(
            "C_CreateSessionReverseGrid: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        // The rule is the table's only key, after the others' defaults.
        let tail = [&[0xAC][..], b"reverse_grid", &[0x08]].concat();
        assert!(bytes.ends_with(&tail));
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::CreateSession { race_rules, .. } => assert_eq!(race_rules, rules),
            _ => panic!("Wrong message type"),
        }
        // Off is left off the wire.
        assert!(
            rmp_serde::to_vec_named(&RaceRules::default())
                .unwrap()
                .len()
                == 1
        );
    }

    /// The safety car on the road: a create asking for it, and a frame
    /// while it is out (`race_phase` 5 and its pose, after a nil clock, sky
    /// and yellows). Printed as `C_CreateSessionFullSafetyCar` /
    /// `S_TelemetryCompactSafetyCar` (`cargo test
    /// full_safety_car_wire_format -- --nocapture`).
    #[test]
    fn test_full_safety_car_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let rules = RaceRules {
            safety_car: SafetyCar::Full,
            ..Default::default()
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: rules,
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionFullSafetyCar: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { race_rules, .. } => assert_eq!(race_rules, rules),
            _ => panic!("Wrong message type"),
        }
        let pose = SafetyCarPose {
            x: 120.5,
            y: -42.25,
            z: 3.0,
            yaw_rad: 1.5,
            speed_mps: 40.0,
        };
        let frame = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 500,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![],
            race_clock: None,
            sky: None,
            yellow_flags: None,
            race_phase: Some(crate::race_start::RacePhase::SafetyCar as u8),
            safety_car: Some(pose),
        });
        let frame_bytes = rmp_serde::to_vec(&frame).unwrap();
        println!("S_TelemetryCompactSafetyCar: {}", hex(&frame_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&frame_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => {
                assert_eq!(t.race_phase, Some(5));
                assert_eq!(t.safety_car, Some(pose));
            }
            _ => panic!("Wrong message type"),
        }
        // Without it the frame is the VSC's: the phase last.
        let without = rmp_serde::to_vec(&ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 500,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![],
            race_clock: None,
            sky: None,
            yellow_flags: None,
            race_phase: Some(crate::race_start::RacePhase::ScInThisLap as u8),
            safety_car: None,
        }))
        .unwrap();
        // The pose is a 5-array of float32s: 1 + 5 x 5 bytes.
        assert_eq!(without.len(), frame_bytes.len() - 26);
    }

    /// A timed race: a create for two hours, the joined echo, and a
    /// telemetry frame whose clock has run out with lap 37 the last. A race
    /// over laps leaves every one of them off the wire. Pinned on the client
    /// as `ApexGolden::C_CreateSessionRaceTime` / `S_SessionJoinedRaceTime`
    /// and `ApexUdpGolden::S_TelemetryCompactRaceClock`; `cargo test
    /// race_time_wire_format -- --nocapture` prints them.
    #[test]
    fn test_race_time_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |race_seconds| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(Some(7200))).unwrap();
        println!("C_CreateSessionRaceTime: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { race_seconds, .. } => {
                assert_eq!(race_seconds, Some(7200))
            }
            _ => panic!("Wrong message type"),
        }
        let laps_bytes = rmp_serde::to_vec_named(&create(None)).unwrap();
        assert!(!laps_bytes.windows(12).any(|w| w == b"race_seconds"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: Some(7200),
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedRaceTime: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.race_seconds, Some(7200)),
            _ => panic!("Wrong message type"),
        }

        let frame = |race_clock| {
            ServerMessage::TelemetryCompact(CompactTelemetry {
                server_tick: 123_456,
                session_state: SessionState::Racing,
                game_mode: GameMode::Race,
                countdown_ms: None,
                car_states: vec![],
                race_clock,
                sky: None,
                yellow_flags: None,
                race_phase: None,
                safety_car: None,
            })
        };
        let clock_bytes = rmp_serde::to_vec(&frame(Some(RaceClock {
            left_ms: 0,
            final_lap: 37,
        })))
        .unwrap();
        println!("S_TelemetryCompactRaceClock: {}", hex(&clock_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&clock_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => assert_eq!(
                t.race_clock,
                Some(RaceClock {
                    left_ms: 0,
                    final_lap: 37
                })
            ),
            _ => panic!("Wrong message type"),
        }
        // No clock: the frame is the five fields it always was, and one
        // without the sixth decodes as no clock.
        let lap_race = rmp_serde::to_vec(&frame(None)).unwrap();
        assert_eq!(lap_race.len() + 3, clock_bytes.len());
        match rmp_serde::from_slice::<ServerMessage>(&lap_race).unwrap() {
            ServerMessage::TelemetryCompact(t) => assert_eq!(t.race_clock, None),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_RACE_TIME);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_RACE_TIME);
        assert_eq!(clock_bytes, GOLDEN_S_TELEMETRY_COMPACT_RACE_CLOCK);
    }

    /// A timed qualifying session: a create for fifteen minutes, the joined
    /// echo, and a qualifying telemetry frame with 754.321 s left on the
    /// clock (`final_lap` 0; `left_ms` 0 would be the chequered flag). Open
    /// qualifying leaves the field off the wire. Pinned on the client as
    /// `ApexGolden::C_CreateSessionQualifyingTime` /
    /// `S_SessionJoinedQualifyingTime` and
    /// `ApexUdpGolden::S_TelemetryCompactQualifyingClock`; `cargo test
    /// qualifying_time_wire_format -- --nocapture` prints them.
    #[test]
    fn test_qualifying_time_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |qualifying_seconds| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(Some(900))).unwrap();
        println!("C_CreateSessionQualifyingTime: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession {
                qualifying_seconds, ..
            } => assert_eq!(qualifying_seconds, Some(900)),
            _ => panic!("Wrong message type"),
        }
        let open_bytes = rmp_serde::to_vec_named(&create(None)).unwrap();
        assert!(!open_bytes.windows(18).any(|w| w == b"qualifying_seconds"));
        match rmp_serde::from_slice::<ClientMessage>(&open_bytes).unwrap() {
            ClientMessage::CreateSession {
                qualifying_seconds, ..
            } => assert_eq!(qualifying_seconds, None),
            _ => panic!("Wrong message type"),
        }

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: Some(900),
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedQualifyingTime: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => {
                assert_eq!(data.qualifying_seconds, Some(900));
                assert_eq!(data.race_seconds, None);
            }
            _ => panic!("Wrong message type"),
        }

        let clock = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Qualification,
            countdown_ms: None,
            car_states: vec![],
            race_clock: Some(RaceClock {
                left_ms: 754_321,
                final_lap: 0,
            }),
            sky: None,
            yellow_flags: None,
            race_phase: None,
            safety_car: None,
        });
        let clock_bytes = rmp_serde::to_vec(&clock).unwrap();
        println!("S_TelemetryCompactQualifyingClock: {}", hex(&clock_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&clock_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => assert_eq!(
                t.race_clock,
                Some(RaceClock {
                    left_ms: 754_321,
                    final_lap: 0
                })
            ),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_QUALIFYING_TIME);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_QUALIFYING_TIME);
        assert_eq!(clock_bytes, GOLDEN_S_TELEMETRY_COMPACT_QUALIFYING_CLOCK);
    }

    /// A race weekend: a create with 30 min of practice and three rounds
    /// (18, 15 and 12 min), its joined echo, a `WeekendState` mid-Q2 with
    /// one car knocked out in Q1, the host's `AdvanceWeekend`, and a
    /// practice telemetry frame (`GameMode::Practice` = 9) with 754.321 s
    /// on the clock. A plain session leaves `weekend` off the wire. Pinned
    /// on the client as `ApexGolden::C_CreateSessionWeekend`,
    /// `S_SessionJoinedWeekend`, `S_WeekendState`, `C_AdvanceWeekend` and
    /// `ApexUdpGolden::S_TelemetryCompactPracticeClock`; `cargo test
    /// weekend_wire_format -- --nocapture` prints them.
    #[test]
    fn test_weekend_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let format = crate::weekend::WeekendFormat {
            practice_seconds: 1800,
            qualifying_seconds: vec![1080, 900, 720],
            practice_conditions: None,
            qualifying_conditions: None,
            ..Default::default()
        };
        let create = |weekend| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(Some(format.clone()))).unwrap();
        println!("C_CreateSessionWeekend: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { weekend, .. } => {
                assert_eq!(weekend, Some(format.clone()))
            }
            _ => panic!("Wrong message type"),
        }
        let plain_bytes = rmp_serde::to_vec_named(&create(None)).unwrap();
        assert!(!plain_bytes.windows(7).any(|w| w == b"weekend"));

        // Each day's own sky rides in the format when named, and only then
        // (`C_CreateSessionWeekendDays`): practice in heavy rain at 07:00,
        // qualifying under the session's sky.
        let days = crate::weekend::WeekendFormat {
            practice_conditions: Some(Box::new(SessionConditions {
                weather: crate::data::Weather::HeavyRain,
                time_of_day_minutes: 7 * 60,
                ..SessionConditions::DEFAULT
            })),
            ..format.clone()
        };
        let days_bytes = rmp_serde::to_vec_named(&create(Some(days.clone()))).unwrap();
        println!("C_CreateSessionWeekendDays: {}", hex(&days_bytes));
        assert!(days_bytes.windows(19).any(|w| w == b"practice_conditions"));
        assert!(!days_bytes
            .windows(21)
            .any(|w| w == b"qualifying_conditions"));
        assert!(!String::from_utf8_lossy(&create_bytes).contains("practice_conditions"));
        match rmp_serde::from_slice::<ClientMessage>(&days_bytes).unwrap() {
            ClientMessage::CreateSession { weekend, .. } => assert_eq!(weekend, Some(days)),
            _ => panic!("Wrong message type"),
        }

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: Some(format.clone()),
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedWeekend: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.weekend, Some(format)),
            _ => panic!("Wrong message type"),
        }

        let state = ServerMessage::WeekendState(WeekendStateData {
            phase: 2,
            round: 2,
            rounds: 3,
            finished: false,
            advancing: 10,
            eliminated: vec![KnockedOutData {
                car_index: 7,
                round: 1,
                grid_position: 16,
                lap_time_ms: 91_234,
                late: false,
            }],
            damage: DamageLevel::Full,
            conditions: None,
        });
        let state_bytes = rmp_serde::to_vec_named(&state).unwrap();
        println!("S_WeekendState: {}", hex(&state_bytes));
        // A late entry is named; every other knocked-out car keeps the
        // bytes above.
        let late = KnockedOutData {
            car_index: 7,
            round: 1,
            grid_position: 16,
            lap_time_ms: 0,
            late: true,
        };
        let late_bytes = rmp_serde::to_vec_named(&late).unwrap();
        assert!(late_bytes.windows(4).any(|w| w == b"Late"));
        assert!(!String::from_utf8_lossy(&state_bytes).contains("Late"));
        assert!(
            rmp_serde::from_slice::<KnockedOutData>(&late_bytes)
                .unwrap()
                .late
        );
        match rmp_serde::from_slice::<ServerMessage>(&state_bytes).unwrap() {
            ServerMessage::WeekendState(data) => {
                assert_eq!((data.phase, data.round, data.rounds), (2, 2, 3));
                assert!(!data.finished);
                assert_eq!(data.advancing, 10);
                assert_eq!(data.eliminated[0].grid_position, 16);
                assert_eq!(data.eliminated[0].lap_time_ms, 91_234);
            }
            _ => panic!("Wrong message type"),
        }

        // A late entry in the state (`S_WeekendStateLate`): joined after Q1
        // began, out with no time and named so.
        let late_state = ServerMessage::WeekendState(WeekendStateData {
            phase: 2,
            round: 1,
            rounds: 2,
            finished: true,
            advancing: 3,
            eliminated: vec![late],
            damage: DamageLevel::Full,
            conditions: None,
        });
        let late_state_bytes = rmp_serde::to_vec_named(&late_state).unwrap();
        println!("S_WeekendStateLate: {}", hex(&late_state_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&late_state_bytes).unwrap() {
            ServerMessage::WeekendState(data) => assert!(data.eliminated[0].late),
            _ => panic!("Wrong message type"),
        }

        // The lobby lists a weekend waiting for its host between phases
        // (`S_LobbyStateWeekendWaiting`): `Finished`, and joinable.
        let waiting = ServerMessage::LobbyState(LobbyStateData {
            players_in_lobby: vec![],
            available_sessions: vec![SessionSummary {
                id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
                track_name: "Monzarella".into(),
                track_file: String::new(),
                track_id: Uuid::nil(),
                host_name: "host".into(),
                session_kind: SessionKind::Multiplayer,
                player_count: 2,
                max_players: 8,
                state: SessionState::Finished,
                conditions: SessionConditions::DEFAULT,
                damage: DamageLevel::Full,
                lap_limit: 5,
                race_seconds: None,
                weekend_waiting: true,
                mandatory_stops: 0,
                compound_change: false,
                race_rules: Default::default(),
                classes: Vec::new(),
            }],
            car_configs: vec![],
            track_configs: vec![],
            showcase_available: false,
            live_stream: true,
            recordings_port: None,
            balanced: false,
        });
        let waiting_bytes = rmp_serde::to_vec_named(&waiting).unwrap();
        println!("S_LobbyStateWeekendWaiting: {}", hex(&waiting_bytes));
        assert!(waiting_bytes.windows(14).any(|w| w == b"WeekendWaiting"));
        match rmp_serde::from_slice::<ServerMessage>(&waiting_bytes).unwrap() {
            ServerMessage::LobbyState(l) => assert!(l.available_sessions[0].weekend_waiting),
            _ => panic!("Wrong message type"),
        }

        let advance_bytes = rmp_serde::to_vec_named(&ClientMessage::AdvanceWeekend).unwrap();
        println!("C_AdvanceWeekend: {}", hex(&advance_bytes));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&advance_bytes).unwrap(),
            ClientMessage::AdvanceWeekend
        ));

        let clock = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Practice,
            countdown_ms: None,
            car_states: vec![],
            race_clock: Some(RaceClock {
                left_ms: 754_321,
                final_lap: 0,
            }),
            sky: None,
            yellow_flags: None,
            race_phase: None,
            safety_car: None,
        });
        let clock_bytes = rmp_serde::to_vec(&clock).unwrap();
        println!("S_TelemetryCompactPracticeClock: {}", hex(&clock_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&clock_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => assert_eq!(t.game_mode, GameMode::Practice),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_WEEKEND);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_WEEKEND);
        assert_eq!(state_bytes, GOLDEN_S_WEEKEND_STATE);
        assert_eq!(advance_bytes, GOLDEN_C_ADVANCE_WEEKEND);
        assert_eq!(clock_bytes, GOLDEN_S_TELEMETRY_COMPACT_PRACTICE_CLOCK);
    }

    /// The AI field's level: a create that picks one and the joined echo.
    /// The mixed field is the default and left off the wire. Pinned on the
    /// client as `ApexGolden::C_CreateSessionAiSkill` /
    /// `S_SessionJoinedAiSkill`; `cargo test session_ai_skill_wire_format
    /// -- --nocapture` prints them.
    #[test]
    fn test_session_ai_skill_wire_format() {
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
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: Some(95),
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionAiSkill: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { ai_skill, .. } => assert_eq!(ai_skill, Some(95)),
            _ => panic!("Wrong message type"),
        }

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: Some(95),
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedAiSkill: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.ai_skill, Some(95)),
            _ => panic!("Wrong message type"),
        }

        // The mixed field stays off the wire: a create from before the
        // field keeps its bytes and decodes as mixed.
        let mixed = ClientMessage::CreateSession {
            track_config_id: Uuid::nil(),
            max_players: 2,
            ai_count: 1,
            lap_limit: 1,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let mixed_bytes = rmp_serde::to_vec_named(&mixed).unwrap();
        assert!(!mixed_bytes.windows(8).any(|w| w == b"ai_skill"));
        match rmp_serde::from_slice::<ClientMessage>(&mixed_bytes).unwrap() {
            ClientMessage::CreateSession { ai_skill, .. } => assert_eq!(ai_skill, None),
            _ => panic!("Wrong message type"),
        }

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_AI_SKILL);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_AI_SKILL);
    }

    /// A multi-class field: a create that names its classes and the joined
    /// echo in rank order. A field of one class leaves `classes` off the
    /// wire. Pinned on the client as `ApexGolden::C_CreateSessionClasses`
    /// / `S_SessionJoinedClasses`; `cargo test
    /// session_classes_wire_format -- --nocapture` prints them.
    #[test]
    fn test_session_classes_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let classes = vec![
            ClassEntry {
                class: "LMP2".into(),
                ai_count: 4,
                ai_skill: Some(100),
            },
            ClassEntry {
                class: "GT3".into(),
                ai_count: 6,
                ai_skill: None,
            },
        ];
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 12,
            ai_count: 10,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: classes.clone(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create).unwrap();
        println!("C_CreateSessionClasses: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { classes: back, .. } => assert_eq!(back, classes),
            _ => panic!("Wrong message type"),
        }

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 11,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: classes.clone(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedClasses: {}", hex(&joined_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&joined_bytes).unwrap() {
            ServerMessage::SessionJoined(data) => assert_eq!(data.classes, classes),
            _ => panic!("Wrong message type"),
        }

        // One class stays off the wire.
        let one = ClientMessage::CreateSession {
            track_config_id: Uuid::nil(),
            max_players: 2,
            ai_count: 1,
            lap_limit: 1,
            session_kind: SessionKind::Practice,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let one_bytes = rmp_serde::to_vec_named(&one).unwrap();
        assert!(!one_bytes.windows(7).any(|w| w == b"classes"));

        // The sanitizer: blanks and repeats dropped, levels clamped, at
        // most `MAX_CLASSES`.
        let cleaned = ClassEntry::sanitize(vec![
            ClassEntry {
                class: " GT3 ".into(),
                ai_count: 2,
                ai_skill: Some(200),
            },
            ClassEntry {
                class: "".into(),
                ai_count: 2,
                ai_skill: None,
            },
            ClassEntry {
                class: "gt3".into(),
                ai_count: 9,
                ai_skill: None,
            },
            ClassEntry {
                class: "LMP2".into(),
                ai_count: 1,
                ai_skill: None,
            },
            ClassEntry {
                class: "F1".into(),
                ai_count: 1,
                ai_skill: None,
            },
            ClassEntry {
                class: "Hypercar".into(),
                ai_count: 1,
                ai_skill: None,
            },
            ClassEntry {
                class: "Rally".into(),
                ai_count: 1,
                ai_skill: None,
            },
        ]);
        let names: Vec<&str> = cleaned.iter().map(|c| c.class.as_str()).collect();
        assert_eq!(names, vec!["GT3", "LMP2", "F1", "Hypercar"]);
        assert_eq!(cleaned[0].ai_skill, Some(110));

        assert_eq!(create_bytes, GOLDEN_C_CREATE_SESSION_CLASSES);
        assert_eq!(joined_bytes, GOLDEN_S_SESSION_JOINED_CLASSES);
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
            time_scale: None,
            changeable: None,
            track_rubber_pct: None,
            sky_seed: None,
            day_of_year: None,
            solar_noon_min: None,
            forecast: Default::default(),
        };
        let create = ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: air,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
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
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedAir: {}", hex(&joined_bytes));
    }

    /// The bytes of a changing sky: a create whose clock runs, whose
    /// weather follows a forecast and whose line starts rubbered in, its
    /// echo, and a telemetry frame carrying the sky (with no race clock,
    /// which then goes as nil). Pinned on the client as
    /// `ApexGolden::C_CreateSessionSky` / `S_SessionJoinedSky` and
    /// `ApexUdpGolden::S_TelemetryCompactSky`; `cargo test
    /// sky_wire_format -- --nocapture` prints them.
    #[test]
    fn test_sky_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let sky = SessionConditions {
            weather: Weather::Cloudy,
            time_of_day_minutes: 15 * 60,
            time_scale: Some(24),
            changeable: Some(2),
            track_rubber_pct: Some(90),
            sky_seed: None,
            day_of_year: None,
            solar_noon_min: None,
            ..SessionConditions::DEFAULT
        };
        let create = |conditions| ClientMessage::CreateSession {
            track_config_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            max_players: 8,
            ai_count: 3,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let create_bytes = rmp_serde::to_vec_named(&create(sky)).unwrap();
        println!("C_CreateSessionSky: {}", hex(&create_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&create_bytes).unwrap() {
            ClientMessage::CreateSession { conditions, .. } => assert_eq!(conditions, sky),
            _ => panic!("Wrong message type"),
        }
        // A held sky leaves all three off: every earlier create's bytes.
        let held = rmp_serde::to_vec_named(&create(SessionConditions::DEFAULT)).unwrap();
        let text = String::from_utf8_lossy(&held);
        assert!(!text.contains("time_scale") && !text.contains("changeable"));
        assert!(!text.contains("track_rubber_pct"));

        let joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: sky,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let joined_bytes = rmp_serde::to_vec_named(&joined).unwrap();
        println!("S_SessionJoinedSky: {}", hex(&joined_bytes));

        // The host's own seed rides last among the sky's keys, and comes
        // back in the echo; a create without one keeps the bytes above.
        let seeded = SessionConditions {
            sky_seed: Some(0x0123_4567_89AB_CDEF),
            ..sky
        };
        let seeded_bytes = rmp_serde::to_vec_named(&create(seeded)).unwrap();
        println!("C_CreateSessionSkySeed: {}", hex(&seeded_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&seeded_bytes).unwrap() {
            ClientMessage::CreateSession { conditions, .. } => assert_eq!(conditions, seeded),
            _ => panic!("Wrong message type"),
        }
        assert!(!String::from_utf8_lossy(&create_bytes).contains("sky_seed"));

        // The host's date rides after the seed, and the echo names solar
        // noon after it (the server's, from the track: 13:33 at Spa); a
        // create or a join without either keeps the bytes above.
        let dated = SessionConditions {
            day_of_year: Some(200),
            ..seeded
        };
        let dated_bytes = rmp_serde::to_vec_named(&create(dated)).unwrap();
        println!("C_CreateSessionDate: {}", hex(&dated_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&dated_bytes).unwrap() {
            ClientMessage::CreateSession { conditions, .. } => assert_eq!(conditions, dated),
            _ => panic!("Wrong message type"),
        }
        assert!(!String::from_utf8_lossy(&seeded_bytes).contains("day_of_year"));
        let sunned = SessionConditions {
            solar_noon_min: Some(13 * 60 + 33),
            ..dated
        };
        let sun_joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: sunned,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let sun_bytes = rmp_serde::to_vec_named(&sun_joined).unwrap();
        println!("S_SessionJoinedSun: {}", hex(&sun_bytes));
        assert!(!String::from_utf8_lossy(&joined_bytes).contains("solar_noon_min"));

        // The host's own forecast rides last, an array of [minute, weather]
        // pairs, in the create and in the echo; a create without one keeps
        // the bytes above.
        let written = SessionConditions {
            forecast: crate::data::HostForecast::from_steps(
                sky.weather,
                &[
                    crate::data::ForecastStep {
                        at_min: 20,
                        weather: Weather::LightRain,
                    },
                    crate::data::ForecastStep {
                        at_min: 45,
                        weather: Weather::HeavyRain,
                    },
                    crate::data::ForecastStep {
                        at_min: 90,
                        weather: Weather::Sunny,
                    },
                ],
            ),
            ..sky
        };
        let forecast_bytes = rmp_serde::to_vec_named(&create(written)).unwrap();
        println!("C_CreateSessionForecast: {}", hex(&forecast_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&forecast_bytes).unwrap() {
            ClientMessage::CreateSession { conditions, .. } => assert_eq!(conditions, written),
            _ => panic!("Wrong message type"),
        }
        assert!(!String::from_utf8_lossy(&create_bytes).contains("forecast"));
        let forecast_joined = ServerMessage::SessionJoined(SessionJoinedData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            your_grid_position: 3,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::default(),
            conditions: written,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            qualifying_seconds: None,
            weekend: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            physics: None,
            classes: Vec::new(),
        });
        let forecast_joined_bytes = rmp_serde::to_vec_named(&forecast_joined).unwrap();
        println!("S_SessionJoinedForecast: {}", hex(&forecast_joined_bytes));

        let now = SkyNow {
            clock_s: 15 * 3600 + 30 * 60 + 15,
            weather: Weather::LightRain as u8,
            rain_pct: 37,
            cloud_pct: 88,
            road_water_pct: 21,
            air_c: -2,
            track_c: 14,
            wind_kph: 23,
            wind_to_deg: 300,
            line_rubber_pct: 64,
            time_scale: 24,
            next_weather: Weather::HeavyRain as u8,
            next_in_s: 1250,
            line_snow_pct: 0,
            snowfall_pct: 0,
            next2_weather: Weather::Overcast as u8,
            next2_in_s: 2400,
            next3_weather: Weather::Cloudy as u8,
            next3_in_s: 4200,
            visibility_m: 850,
        };
        let frame = ServerMessage::TelemetryCompact(CompactTelemetry {
            server_tick: 123_456,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![],
            race_clock: None,
            sky: Some(now),
            yellow_flags: None,
            race_phase: None,
            safety_car: None,
        });
        let frame_bytes = rmp_serde::to_vec(&frame).unwrap();
        println!("S_TelemetryCompactSky: {}", hex(&frame_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&frame_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => {
                assert_eq!(t.race_clock, None);
                assert_eq!(t.sky, Some(now));
            }
            _ => panic!("Wrong message type"),
        }
        // Without a sky the frame is the five fields it always was.
        let bare = ServerMessage::TelemetryCompact(CompactTelemetry {
            sky: None,
            yellow_flags: None,
            race_phase: None,
            safety_car: None,
            ..match frame {
                ServerMessage::TelemetryCompact(t) => t,
                _ => unreachable!(),
            }
        });
        let bare_bytes = rmp_serde::to_vec(&bare).unwrap();
        match rmp_serde::from_slice::<ServerMessage>(&bare_bytes).unwrap() {
            ServerMessage::TelemetryCompact(t) => {
                assert!(t.sky.is_none() && t.race_clock.is_none())
            }
            _ => panic!("Wrong message type"),
        }
        assert!(bare_bytes.len() < frame_bytes.len());
    }

    /// The bytes of the start-order messages, pinned on the client as
    /// `ApexGolden::C_CreateSessionGrid`, `C_RequestQualifyingResults` and
    /// `S_QualifyingResults`; `cargo test grid_wire_format -- --nocapture`
    /// prints them.
    #[test]
    fn test_grid_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let track = Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap();
        let create = |grid_order: Vec<String>| ClientMessage::CreateSession {
            track_config_id: track,
            max_players: 8,
            ai_count: 2,
            lap_limit: 5,
            session_kind: SessionKind::Multiplayer,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: Default::default(),
            ai_skill: None,
            race_seconds: None,
            grid_order,
            qualifying_seconds: None,
            weekend: None,
            watch: None,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        // No order: the bytes of every earlier create.
        let plain = rmp_serde::to_vec_named(&create(Vec::new())).unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("grid_order"));
        let ordered = create(vec!["@ai:2".into(), "@host".into(), "Luna Swift".into()]);
        let ordered_bytes = rmp_serde::to_vec_named(&ordered).unwrap();
        println!("C_CreateSessionGrid: {}", hex(&ordered_bytes));
        match rmp_serde::from_slice::<ClientMessage>(&ordered_bytes).unwrap() {
            ClientMessage::CreateSession { grid_order, .. } => {
                assert_eq!(grid_order, ["@ai:2", "@host", "Luna Swift"]);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        let request = ClientMessage::RequestQualifyingResults {
            track_config_id: track,
        };
        let request_bytes = rmp_serde::to_vec_named(&request).unwrap();
        println!("C_RequestQualifyingResults: {}", hex(&request_bytes));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&request_bytes).unwrap(),
            ClientMessage::RequestQualifyingResults { .. }
        ));

        let results = ServerMessage::QualifyingResults(QualifyingResultsData::from_results(
            track,
            vec![crate::records::QualifyingResult {
                id: Uuid::parse_str("11111111-2222-3333-4444-555555555555").unwrap(),
                track_id: track,
                class: "GT3".into(),
                recorded_at: "2026-10-06T12:00:00Z".into(),
                entries: vec![
                    crate::records::QualifyingEntry {
                        name: "Luna Swift".into(),
                        driver_id: None,
                        car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555")
                            .unwrap(),
                        lap_time_ms: 91_234,
                        is_ai: true,
                    },
                    crate::records::QualifyingEntry {
                        name: "Guido".into(),
                        driver_id: Some(
                            Uuid::parse_str("dddddddd-0000-4000-8000-000000000001").unwrap(),
                        ),
                        car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555")
                            .unwrap(),
                        lap_time_ms: 91_876,
                        is_ai: false,
                    },
                ],
            }],
        ));
        let board_request = ClientMessage::RequestLeaderboard {
            track_config_id: track,
            car_config_id: Some("99999999-8888-7777-6666-555555555555".into()),
        };
        let board_request_bytes = rmp_serde::to_vec_named(&board_request).unwrap();
        println!("C_RequestLeaderboard: {}", hex(&board_request_bytes));
        let all_cars = rmp_serde::to_vec_named(&ClientMessage::RequestLeaderboard {
            track_config_id: track,
            car_config_id: None,
        })
        .unwrap();
        // Without a car the field is left off the wire.
        assert!(!String::from_utf8_lossy(&all_cars).contains("car_config_id"));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&all_cars).unwrap(),
            ClientMessage::RequestLeaderboard {
                car_config_id: None,
                ..
            }
        ));
        let board = ServerMessage::Leaderboard(LeaderboardData::from_records(
            track,
            Some(Uuid::parse_str("99999999-8888-7777-6666-555555555555").unwrap()),
            vec![
                crate::records::LapRecord {
                    player: "Luna Swift".into(),
                    driver_id: None,
                    track_id: track,
                    car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555").unwrap(),
                    lap_time_ms: 91_234,
                    splits_ms: [30_100, 31_000, 30_134],
                    recorded_at: "2026-10-06T12:00:00Z".into(),
                    ghost_file: None,
                },
                crate::records::LapRecord {
                    player: "Guido".into(),
                    driver_id: Some(
                        Uuid::parse_str("dddddddd-0000-4000-8000-000000000001").unwrap(),
                    ),
                    track_id: track,
                    car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555").unwrap(),
                    lap_time_ms: 91_876,
                    splits_ms: [30_400, 31_300, 30_176],
                    recorded_at: "2026-10-07T09:30:00Z".into(),
                    ghost_file: None,
                },
            ],
        ));
        let board_bytes = rmp_serde::to_vec_named(&board).unwrap();
        println!("S_Leaderboard: {}", hex(&board_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&board_bytes).unwrap() {
            ServerMessage::Leaderboard(data) => {
                assert_eq!(data.entries.len(), 2);
                assert_eq!(data.entries[1].player_name, "Guido");
                assert_eq!(data.entries[0].splits_ms, vec![30_100, 31_000, 30_134]);
                assert_eq!(data.car_config_id, "99999999-8888-7777-6666-555555555555");
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        let results_bytes = rmp_serde::to_vec_named(&results).unwrap();
        println!("S_QualifyingResults: {}", hex(&results_bytes));
        match rmp_serde::from_slice::<ServerMessage>(&results_bytes).unwrap() {
            ServerMessage::QualifyingResults(data) => {
                assert_eq!(data.track_id, track);
                assert_eq!(data.results.len(), 1);
                assert_eq!(data.results[0].entries[1].name, "Guido");
                assert_eq!(data.results[0].entries[0].lap_time_ms, 91_234);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The bytes of the hotlap messages, pinned on the client as
    /// `ApexGolden::C_HotlapRelocate`, `C_RequestGhost` and `S_GhostLap`;
    /// `RecoverCar`, as the client sends it: `C_RecoverCarTrack`,
    /// `C_RecoverCarPits`. `cargo test recover_wire_format -- --nocapture`
    /// prints them.
    #[test]
    fn test_recover_wire_format() {
        use crate::recovery::RecoverDestination;
        for (name, destination) in [
            ("C_RecoverCarTrack", RecoverDestination::Track),
            ("C_RecoverCarPits", RecoverDestination::Pits),
        ] {
            let bytes =
                rmp_serde::to_vec_named(&ClientMessage::RecoverCar { destination }).unwrap();
            println!(
                "{name}: {}",
                bytes
                    .iter()
                    .map(|b| format!("0x{:02X}", b))
                    .collect::<Vec<_>>()
                    .join(", ")
            );
            match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
                ClientMessage::RecoverCar { destination: back } => assert_eq!(back, destination),
                other => panic!("Wrong message type: {other:?}"),
            }
        }
    }

    /// `cargo test rejoin_wire_format -- --nocapture` prints them.
    #[test]
    fn test_rejoin_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        // Without a token the bytes are those of every earlier Authenticate.
        let plain = rmp_serde::to_vec_named(&ClientMessage::Authenticate {
            token: "dev-token".to_string(),
            player_name: "Player".to_string(),
            protocol_version: PROTOCOL_VERSION,
            resume_token: None,
            driver_id: None,
            driver_key: None,
        })
        .unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("resume_token"));
        let resume = rmp_serde::to_vec_named(&ClientMessage::Authenticate {
            token: "dev-token".to_string(),
            player_name: "Player".to_string(),
            protocol_version: PROTOCOL_VERSION,
            resume_token: Some("resume-tok".to_string()),
            driver_id: None,
            driver_key: None,
        })
        .unwrap();
        println!("C_AuthenticateResume: {}", hex(&resume));
        // A driver profile's id rides beside the name.
        let profile = rmp_serde::to_vec_named(&ClientMessage::Authenticate {
            token: "dev-token".to_string(),
            player_name: "Player".to_string(),
            protocol_version: PROTOCOL_VERSION,
            resume_token: None,
            driver_id: Some(Uuid::parse_str("dddddddd-0000-4000-8000-000000000001").unwrap()),
            driver_key: None,
        })
        .unwrap();
        println!("C_AuthenticateProfile: {}", hex(&profile));
        match rmp_serde::from_slice::<ClientMessage>(&profile).unwrap() {
            ClientMessage::Authenticate { driver_id, .. } => assert_eq!(
                driver_id,
                Some(Uuid::parse_str("dddddddd-0000-4000-8000-000000000001").unwrap())
            ),
            other => panic!("Wrong message type: {other:?}"),
        }
        // A profile's key beside its id, the server's challenge, the proof.
        let keyed = rmp_serde::to_vec_named(&ClientMessage::Authenticate {
            token: "dev-token".to_string(),
            player_name: "Player".to_string(),
            protocol_version: PROTOCOL_VERSION,
            resume_token: None,
            driver_id: Some(Uuid::parse_str("dddddddd-0000-4000-8000-000000000001").unwrap()),
            driver_key: Some("ab".repeat(32)),
        })
        .unwrap();
        println!("C_AuthenticateKey: {}", hex(&keyed));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&keyed).unwrap(),
            ClientMessage::Authenticate { driver_key: Some(k), .. } if k.len() == 64
        ));
        let prove = rmp_serde::to_vec_named(&ClientMessage::ProveDriverKey {
            signature: "cd".repeat(64),
        })
        .unwrap();
        println!("C_ProveDriverKey: {}", hex(&prove));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&prove).unwrap(),
            ClientMessage::ProveDriverKey { signature } if signature.len() == 128
        ));
        match rmp_serde::from_slice::<ClientMessage>(&resume).unwrap() {
            ClientMessage::Authenticate { resume_token, .. } => {
                assert_eq!(resume_token.as_deref(), Some("resume-tok"));
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        let player_id = Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap();
        let auth = |resume_token: &str| {
            ServerMessage::AuthSuccess(AuthSuccessData {
                player_id,
                server_version: 1,
                protocol_version: PROTOCOL_VERSION,
                udp_token: "udp-tok".to_string(),
                udp_port: 9001,
                udp_key: String::new(),
                resume_token: resume_token.to_string(),
                key_challenge: String::new(),
            })
        };
        let plain = rmp_serde::to_vec_named(&auth("")).unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("ResumeToken"));
        assert!(!String::from_utf8_lossy(&plain).contains("KeyChallenge"));
        let challenged = rmp_serde::to_vec_named(&ServerMessage::AuthSuccess(AuthSuccessData {
            player_id,
            server_version: 1,
            protocol_version: PROTOCOL_VERSION,
            udp_token: "udp-tok".to_string(),
            udp_port: 9001,
            udp_key: String::new(),
            resume_token: String::new(),
            key_challenge: "ef".repeat(32),
        }))
        .unwrap();
        println!("S_AuthSuccessChallenge: {}", hex(&challenged));
        match rmp_serde::from_slice::<ServerMessage>(&challenged).unwrap() {
            ServerMessage::AuthSuccess(data) => assert_eq!(data.key_challenge, "ef".repeat(32)),
            other => panic!("Wrong message type: {other:?}"),
        }
        let with_token = rmp_serde::to_vec_named(&auth("resume-tok")).unwrap();
        println!("S_AuthSuccessResume: {}", hex(&with_token));
        match rmp_serde::from_slice::<ServerMessage>(&with_token).unwrap() {
            ServerMessage::AuthSuccess(data) => assert_eq!(data.resume_token, "resume-tok"),
            other => panic!("Wrong message type: {other:?}"),
        }

        let give_up = rmp_serde::to_vec_named(&ClientMessage::GiveUpSeat).unwrap();
        println!("C_GiveUpSeat: {}", hex(&give_up));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&give_up).unwrap(),
            ClientMessage::GiveUpSeat
        ));

        let rejoin = ServerMessage::RejoinAvailable(RejoinData {
            session_id: player_id,
            track_id: Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
            session_kind: SessionKind::Multiplayer,
        });
        assert_eq!(rejoin.priority(), MessagePriority::Critical);
        let bytes = rmp_serde::to_vec_named(&rejoin).unwrap();
        println!("S_RejoinAvailable: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::RejoinAvailable(data) => {
                assert_eq!(data.session_id, player_id);
                assert_eq!(data.session_kind, SessionKind::Multiplayer);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The heartbeat's round trip and the operator's notice, pinned on the
    /// client as `ApexGolden::C_HeartbeatPing` and `S_ServerNotice`. `cargo
    /// test server_notice_wire_format -- --nocapture` prints them.
    #[test]
    fn test_server_notice_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        // Without a round trip the bytes are those of every earlier client.
        let plain = rmp_serde::to_vec_named(&ClientMessage::Heartbeat {
            client_tick: 300,
            ping_ms: None,
        })
        .unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("ping_ms"));
        let ping = rmp_serde::to_vec_named(&ClientMessage::Heartbeat {
            client_tick: 300,
            ping_ms: Some(42),
        })
        .unwrap();
        println!("C_HeartbeatPing: {}", hex(&ping));
        match rmp_serde::from_slice::<ClientMessage>(&ping).unwrap() {
            ClientMessage::Heartbeat { ping_ms, .. } => assert_eq!(ping_ms, Some(42)),
            other => panic!("Wrong message type: {other:?}"),
        }

        let notice = ServerMessage::ServerNotice {
            message: "Race starts in 5 minutes".to_string(),
        };
        assert_eq!(notice.priority(), MessagePriority::Critical);
        let bytes = rmp_serde::to_vec_named(&notice).unwrap();
        println!("S_ServerNotice: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::ServerNotice { message } => {
                assert_eq!(message, "Race starts in 5 minutes")
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// A chat line both ways, pinned on the client as `ApexGolden::C_Chat`,
    /// `S_ChatLobby` and `S_ChatSession`. `cargo test chat_wire_format --
    /// --nocapture` prints them.
    #[test]
    fn test_chat_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let said = rmp_serde::to_vec_named(&ClientMessage::Chat {
            text: "good luck".to_string(),
        })
        .unwrap();
        println!("C_Chat: {}", hex(&said));
        match rmp_serde::from_slice::<ClientMessage>(&said).unwrap() {
            ClientMessage::Chat { text } => assert_eq!(text, "good luck"),
            other => panic!("Wrong message type: {other:?}"),
        }
        let id = uuid::Uuid::parse_str("12345678-1234-5678-1234-567812345678").unwrap();
        let lobby = ServerMessage::Chat(ChatData {
            player_id: id,
            from: "Racer".to_string(),
            text: "good luck".to_string(),
            in_session: false,
        });
        assert_eq!(lobby.priority(), MessagePriority::Critical);
        let bytes = rmp_serde::to_vec_named(&lobby).unwrap();
        assert!(
            !String::from_utf8_lossy(&bytes).contains("InSession"),
            "left off in the lobby"
        );
        println!("S_ChatLobby: {}", hex(&bytes));
        let session = ServerMessage::Chat(ChatData {
            in_session: true,
            ..match lobby {
                ServerMessage::Chat(d) => d,
                _ => unreachable!(),
            }
        });
        let bytes = rmp_serde::to_vec_named(&session).unwrap();
        println!("S_ChatSession: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::Chat(d) => {
                assert_eq!(d.player_id, id);
                assert_eq!(d.from, "Racer");
                assert!(d.in_session);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The roster's away flag and grid slot, pinned on the client as
    /// `ApexUdpGolden::S_SessionRosterAwayGrid`. `cargo test
    /// roster_wire_format -- --nocapture` prints it.
    #[test]
    fn test_roster_wire_format() {
        let entry =
            |car_index: u8, name: &str, is_ai: bool, away: bool, grid_slot: u8| RosterEntry {
                car_index,
                player_id: Uuid::from_u128(car_index as u128 + 1),
                player_name: name.to_string(),
                is_ai,
                car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555").unwrap(),
                livery: 0,
                away,
                grid_slot,
                ready: false,
                dnq: false,
                missed_stops: false,
                qualifying_laps: 0,
                penalties: Vec::new(),
            };
        // Present and on the grid: no away key; no slot: no grid key.
        let plain = rmp_serde::to_vec_named(&entry(0, "Player", false, false, 0)).unwrap();
        let text = String::from_utf8_lossy(&plain);
        assert!(!text.contains("Away") && !text.contains("GridSlot"));

        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![
                entry(0, "Player", false, true, 2),
                entry(1, "Luna Swift", true, false, 1),
            ],
        });
        let bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!(
            "S_SessionRosterAwayGrid: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionRoster(data) => {
                assert!(data.entries[0].away);
                assert_eq!(data.entries[0].grid_slot, 2);
                assert!(!data.entries[1].away);
                assert_eq!(data.entries[1].grid_slot, 1);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        // A car the cut-off left out of the race is marked, and only it
        // (`S_SessionRosterDnq`): the pole sitter keeps the bytes above.
        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![
                entry(0, "Player", false, false, 1),
                RosterEntry {
                    dnq: true,
                    ..entry(1, "Luna Swift", true, false, 0)
                },
            ],
        });
        let bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!(
            "S_SessionRosterDnq: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        assert_eq!(String::from_utf8_lossy(&bytes).matches("Dnq").count(), 1);
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionRoster(data) => {
                assert!(!data.entries[0].dnq);
                assert!(data.entries[1].dnq);
            }
            other => panic!("Wrong message type: {other:?}"),
        }

        // Qualifying under a lap allowance: the timed laps each car has run
        // (`S_SessionRosterQualifyingLaps`); a car with none has no key.
        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![
                RosterEntry {
                    qualifying_laps: 2,
                    penalties: Vec::new(),
                    ..entry(0, "Player", false, false, 1)
                },
                entry(1, "Luna Swift", true, false, 2),
            ],
        });
        let bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!(
            "S_SessionRosterQualifyingLaps: {}",
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        );
        assert_eq!(
            String::from_utf8_lossy(&bytes)
                .matches("QualifyingLaps")
                .count(),
            1
        );
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionRoster(data) => {
                assert_eq!(data.entries[0].qualifying_laps, 2);
                assert_eq!(data.entries[1].qualifying_laps, 0);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// The race's grid before the countdown: `OpenGrid`, `SetReady`,
    /// `SetLivery`, a roster with a driver ready and the `Grid` mode, pinned
    /// on the client as `ApexGolden::C_OpenGrid`, `C_SetReady`, `C_SetLivery`,
    /// `S_GameModeChangedGrid` and `ApexUdpGolden::S_SessionRosterReady`.
    /// `cargo test ready_grid_wire_format -- --nocapture` prints them.
    #[test]
    fn test_ready_grid_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let open = rmp_serde::to_vec_named(&ClientMessage::OpenGrid {
            countdown_seconds: 10,
        })
        .unwrap();
        println!("C_OpenGrid: {}", hex(&open));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&open).unwrap(),
            ClientMessage::OpenGrid {
                countdown_seconds: 10
            }
        ));
        let ready = rmp_serde::to_vec_named(&ClientMessage::SetReady { ready: true }).unwrap();
        println!("C_SetReady: {}", hex(&ready));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&ready).unwrap(),
            ClientMessage::SetReady { ready: true }
        ));
        let livery = rmp_serde::to_vec_named(&ClientMessage::SetLivery { livery: 2 }).unwrap();
        println!("C_SetLivery: {}", hex(&livery));
        assert!(matches!(
            rmp_serde::from_slice::<ClientMessage>(&livery).unwrap(),
            ClientMessage::SetLivery { livery: 2 }
        ));
        let mode = rmp_serde::to_vec_named(&ServerMessage::GameModeChanged {
            mode: GameMode::Grid,
        })
        .unwrap();
        println!("S_GameModeChangedGrid: {}", hex(&mode));

        let entry = |car_index: u8, name: &str, ready: bool| RosterEntry {
            car_index,
            player_id: Uuid::from_u128(car_index as u128 + 1),
            player_name: name.to_string(),
            is_ai: false,
            car_config_id: Uuid::parse_str("99999999-8888-7777-6666-555555555555").unwrap(),
            livery: 1,
            away: false,
            grid_slot: car_index + 1,
            ready,
            dnq: false,
            missed_stops: false,
            qualifying_laps: 0,
            penalties: Vec::new(),
        };
        // Not ready: no key, so a roster off the grid keeps its bytes.
        let plain = rmp_serde::to_vec_named(&entry(0, "Player", false)).unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("Ready"));
        let roster = ServerMessage::SessionRoster(SessionRosterData {
            session_id: Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            entries: vec![entry(0, "Player", true), entry(1, "Guest", false)],
        });
        let bytes = rmp_serde::to_vec_named(&roster).unwrap();
        println!("S_SessionRosterReady: {}", hex(&bytes));
        match rmp_serde::from_slice::<ServerMessage>(&bytes).unwrap() {
            ServerMessage::SessionRoster(data) => {
                assert!(data.entries[0].ready);
                assert!(!data.entries[1].ready);
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

    /// `cargo test hotlap_wire_format -- --nocapture` prints them.
    /// The bytes of a `CreateSession` for a hotlap watched as a stream on a
    /// setup (`ApexGolden::C_CreateSessionHotlapWatchStream`):
    /// `cargo test hotlap_watch_wire_format -- --nocapture`.
    #[test]
    fn test_hotlap_watch_wire_format() {
        fn hex(bytes: &[u8]) -> String {
            bytes
                .iter()
                .map(|b| format!("0x{:02X}", b))
                .collect::<Vec<_>>()
                .join(", ")
        }
        let create = |watch| ClientMessage::CreateSession {
            track_config_id: Uuid::from_u128(7),
            max_players: 1,
            ai_count: 1,
            lap_limit: 3,
            session_kind: SessionKind::HotlapWatch,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            damage: DamageLevel::Full,
            ai_skill: None,
            race_seconds: None,
            grid_order: Vec::new(),
            qualifying_seconds: None,
            weekend: None,
            watch,
            qualifying_cutoff_pct: None,
            mandatory_stops: 0,
            compound_change: false,
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: Default::default(),
            classes: Vec::new(),
        };
        let setup = CarSetup {
            tyre_pressure_front: -2,
            rear_wing: 3,
            ..CarSetup::default()
        };
        let options = HotlapWatchOptions {
            stream: true,
            setup: Some(setup),
        };
        let bytes = rmp_serde::to_vec_named(&create(Some(options))).unwrap();
        println!("C_CreateSessionHotlapWatchStream: {}", hex(&bytes));
        match rmp_serde::from_slice::<ClientMessage>(&bytes).unwrap() {
            ClientMessage::CreateSession { watch, .. } => assert_eq!(watch, Some(options)),
            other => panic!("Wrong message type: {other:?}"),
        }
        // Absent, nothing of it is written: older bytes are unchanged.
        let plain = rmp_serde::to_vec_named(&create(None)).unwrap();
        assert!(!String::from_utf8_lossy(&plain).contains("watch"));
        // An empty block is just the key.
        let empty = rmp_serde::to_vec_named(&create(Some(HotlapWatchOptions::default()))).unwrap();
        assert!(!String::from_utf8_lossy(&empty).contains("stream"));
        match rmp_serde::from_slice::<ClientMessage>(&empty).unwrap() {
            ClientMessage::CreateSession { watch, .. } => {
                assert_eq!(watch, Some(HotlapWatchOptions::default()))
            }
            other => panic!("Wrong message type: {other:?}"),
        }
    }

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
            3, 1, -4, 1, -3, 2,
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
                assert_eq!(
                    (
                        decoded.brake_ducts_rear,
                        decoded.brake_pads,
                        decoded.radiator
                    ),
                    (3, 1, -4)
                );
                assert_eq!(
                    (
                        decoded.ers_start_mode,
                        decoded.ers_regen,
                        decoded.ers_deploy_map
                    ),
                    (1, -3, 2)
                );
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
            compounds: vec!["soft".to_string(), "medium".to_string()],
            reference_compound: 1,
            hybrid_battery_kwh: 4.0,
            hybrid_motor_kw: 350.0,
            hybrid_lap_budget_kj: 0.0,
            hybrid_deploy_min_kph: 190.0,
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
                assert_eq!(decoded.compounds, vec!["soft", "medium"]);
                assert_eq!(decoded.reference_compound, 1);
                assert_eq!(decoded.hybrid_battery_kwh, 4.0);
                assert_eq!(decoded.hybrid_lap_budget_kj, 0.0);
                assert_eq!(decoded.hybrid_deploy_min_kph, 190.0);
            }
            _ => panic!("Wrong message type"),
        }
        assert_eq!(bytes, GOLDEN_S_CAR_SETUP_SHEET);
    }

    const GOLDEN_S_CAR_SETUP_SHEET: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x61, 0x72, 0x53, 0x65, 0x74, 0x75, 0x70,
        0x53, 0x68, 0x65, 0x65, 0x74, 0xA4, 0x64, 0x61, 0x74, 0x61, 0xDE, 0x00, 0x11, 0xA9, 0x53,
        0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30,
        0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x31, 0xAB, 0x43, 0x61, 0x72, 0x43, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x49, 0x64, 0xD9, 0x24,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30,
        0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x2D, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0xA5, 0x4B, 0x6E, 0x6F, 0x62, 0x73, 0x92, 0x86, 0xA5,
        0x53, 0x74, 0x6F, 0x63, 0x6B, 0xCA, 0x41, 0xD0, 0x00, 0x00, 0xA4, 0x53, 0x74, 0x65, 0x70,
        0xCA, 0x3F, 0x00, 0x00, 0x00, 0xA2, 0x4C, 0x6F, 0xCA, 0xFF, 0x7F, 0xFF, 0xFF, 0xA2, 0x48,
        0x69, 0xCA, 0x7F, 0x7F, 0xFF, 0xFF, 0xA8, 0x44, 0x65, 0x63, 0x69, 0x6D, 0x61, 0x6C, 0x73,
        0x01, 0xA4, 0x55, 0x6E, 0x69, 0x74, 0xA3, 0x70, 0x73, 0x69, 0x86, 0xA5, 0x53, 0x74, 0x6F,
        0x63, 0x6B, 0xCA, 0x42, 0x68, 0x00, 0x00, 0xA4, 0x53, 0x74, 0x65, 0x70, 0xCA, 0x40, 0x00,
        0x00, 0x00, 0xA2, 0x4C, 0x6F, 0xCA, 0x41, 0x20, 0x00, 0x00, 0xA2, 0x48, 0x69, 0xCA, 0x7F,
        0x7F, 0xFF, 0xFF, 0xA8, 0x44, 0x65, 0x63, 0x69, 0x6D, 0x61, 0x6C, 0x73, 0x00, 0xA4, 0x55,
        0x6E, 0x69, 0x74, 0xA2, 0x6D, 0x6D, 0xAA, 0x47, 0x65, 0x61, 0x72, 0x52, 0x61, 0x74, 0x69,
        0x6F, 0x73, 0x92, 0xCA, 0x40, 0x60, 0x00, 0x00, 0xCA, 0x3F, 0x80, 0x00, 0x00, 0xAA, 0x46,
        0x69, 0x6E, 0x61, 0x6C, 0x44, 0x72, 0x69, 0x76, 0x65, 0xCA, 0x40, 0x70, 0x00, 0x00, 0xAC,
        0x57, 0x68, 0x65, 0x65, 0x6C, 0x52, 0x61, 0x64, 0x69, 0x75, 0x73, 0x4D, 0xCA, 0x3E, 0x80,
        0x00, 0x00, 0xAE, 0x54, 0x79, 0x72, 0x65, 0x4F, 0x70, 0x74, 0x69, 0x6D, 0x61, 0x6C, 0x50,
        0x73, 0x69, 0xCA, 0x41, 0xDC, 0x00, 0x00, 0xA8, 0x4C, 0x61, 0x70, 0x46, 0x75, 0x65, 0x6C,
        0x4C, 0xCA, 0x40, 0x20, 0x00, 0x00, 0xAA, 0x46, 0x75, 0x65, 0x6C, 0x4B, 0x67, 0x50, 0x65,
        0x72, 0x4C, 0xCA, 0x3F, 0x40, 0x00, 0x00, 0xA8, 0x46, 0x69, 0x6C, 0x6C, 0x4C, 0x61, 0x70,
        0x73, 0xCA, 0x40, 0x40, 0x00, 0x00, 0xAE, 0x43, 0x61, 0x6D, 0x62, 0x65, 0x72, 0x4D, 0x6F,
        0x64, 0x65, 0x6C, 0x6C, 0x65, 0x64, 0xC3, 0xB0, 0x52, 0x61, 0x6B, 0x65, 0x42, 0x61, 0x6C,
        0x61, 0x6E, 0x63, 0x65, 0x50, 0x65, 0x72, 0x4D, 0x6D, 0xCA, 0x3A, 0x83, 0x12, 0x6F, 0xA9,
        0x43, 0x6F, 0x6D, 0x70, 0x6F, 0x75, 0x6E, 0x64, 0x73, 0x92, 0xA4, 0x73, 0x6F, 0x66, 0x74,
        0xA6, 0x6D, 0x65, 0x64, 0x69, 0x75, 0x6D, 0xB1, 0x52, 0x65, 0x66, 0x65, 0x72, 0x65, 0x6E,
        0x63, 0x65, 0x43, 0x6F, 0x6D, 0x70, 0x6F, 0x75, 0x6E, 0x64, 0x01, 0xB0, 0x48, 0x79, 0x62,
        0x72, 0x69, 0x64, 0x42, 0x61, 0x74, 0x74, 0x65, 0x72, 0x79, 0x4B, 0x77, 0x68, 0xCA, 0x40,
        0x80, 0x00, 0x00, 0xAD, 0x48, 0x79, 0x62, 0x72, 0x69, 0x64, 0x4D, 0x6F, 0x74, 0x6F, 0x72,
        0x4B, 0x77, 0xCA, 0x43, 0xAF, 0x00, 0x00, 0xB2, 0x48, 0x79, 0x62, 0x72, 0x69, 0x64, 0x44,
        0x65, 0x70, 0x6C, 0x6F, 0x79, 0x4D, 0x69, 0x6E, 0x4B, 0x70, 0x68, 0xCA, 0x43, 0x3E, 0x00,
        0x00,
    ];

    const GOLDEN_C_SET_CAR_SETUP: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAB, 0x53, 0x65, 0x74, 0x43, 0x61, 0x72, 0x53, 0x65,
        0x74, 0x75, 0x70, 0xA4, 0x64, 0x61, 0x74, 0x61, 0xDE, 0x00, 0x1F, 0xB3, 0x74, 0x79, 0x72,
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
        0x72, 0x65, 0x61, 0x72, 0xFE, 0xB0, 0x62, 0x72, 0x61, 0x6B, 0x65, 0x5F, 0x64, 0x75, 0x63,
        0x74, 0x73, 0x5F, 0x72, 0x65, 0x61, 0x72, 0x03, 0xAA, 0x62, 0x72, 0x61, 0x6B, 0x65, 0x5F,
        0x70, 0x61, 0x64, 0x73, 0x01, 0xA8, 0x72, 0x61, 0x64, 0x69, 0x61, 0x74, 0x6F, 0x72, 0xFC,
        0xAE, 0x65, 0x72, 0x73, 0x5F, 0x73, 0x74, 0x61, 0x72, 0x74, 0x5F, 0x6D, 0x6F, 0x64, 0x65,
        0x01, 0xA9, 0x65, 0x72, 0x73, 0x5F, 0x72, 0x65, 0x67, 0x65, 0x6E, 0xFD, 0xAE, 0x65, 0x72,
        0x73, 0x5F, 0x64, 0x65, 0x70, 0x6C, 0x6F, 0x79, 0x5F, 0x6D, 0x61, 0x70, 0x02,
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
        0x72, 0x41, 0x69, 0x64, 0x73, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x85, 0xAC, 0x61, 0x75, 0x74,
        0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65,
        0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xA3, 0x61, 0x62,
        0x73, 0xC2, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E,
        0x74, 0x72, 0x6F, 0x6C, 0x02, 0xAD, 0x70, 0x69, 0x74, 0x5F, 0x61, 0x75, 0x74, 0x6F, 0x70,
        0x69, 0x6C, 0x6F, 0x74, 0xC2,
    ];
    const GOLDEN_C_CREATE_SESSION_DAMAGE: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xA6, 0x64, 0x61, 0x6D, 0x61, 0x67, 0x65, 0x01,
    ];
    const GOLDEN_C_CREATE_SESSION_WEEKEND: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xA7, 0x77, 0x65, 0x65, 0x6B, 0x65, 0x6E, 0x64, 0x82, 0xB0, 0x70, 0x72, 0x61, 0x63,
        0x74, 0x69, 0x63, 0x65, 0x5F, 0x73, 0x65, 0x63, 0x6F, 0x6E, 0x64, 0x73, 0xCD, 0x07, 0x08,
        0xB2, 0x71, 0x75, 0x61, 0x6C, 0x69, 0x66, 0x79, 0x69, 0x6E, 0x67, 0x5F, 0x73, 0x65, 0x63,
        0x6F, 0x6E, 0x64, 0x73, 0x93, 0xCD, 0x04, 0x38, 0xCD, 0x03, 0x84, 0xCD, 0x02, 0xD0,
    ];
    const GOLDEN_S_SESSION_JOINED_WEEKEND: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x00,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA7, 0x57, 0x65, 0x65, 0x6B, 0x65, 0x6E, 0x64,
        0x82, 0xB0, 0x70, 0x72, 0x61, 0x63, 0x74, 0x69, 0x63, 0x65, 0x5F, 0x73, 0x65, 0x63, 0x6F,
        0x6E, 0x64, 0x73, 0xCD, 0x07, 0x08, 0xB2, 0x71, 0x75, 0x61, 0x6C, 0x69, 0x66, 0x79, 0x69,
        0x6E, 0x67, 0x5F, 0x73, 0x65, 0x63, 0x6F, 0x6E, 0x64, 0x73, 0x93, 0xCD, 0x04, 0x38, 0xCD,
        0x03, 0x84, 0xCD, 0x02, 0xD0,
    ];
    const GOLDEN_S_WEEKEND_STATE: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAC, 0x57, 0x65, 0x65, 0x6B, 0x65, 0x6E, 0x64, 0x53,
        0x74, 0x61, 0x74, 0x65, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA5, 0x50, 0x68, 0x61, 0x73,
        0x65, 0x02, 0xA5, 0x52, 0x6F, 0x75, 0x6E, 0x64, 0x02, 0xA6, 0x52, 0x6F, 0x75, 0x6E, 0x64,
        0x73, 0x03, 0xA8, 0x46, 0x69, 0x6E, 0x69, 0x73, 0x68, 0x65, 0x64, 0xC2, 0xA9, 0x41, 0x64,
        0x76, 0x61, 0x6E, 0x63, 0x69, 0x6E, 0x67, 0x0A, 0xAA, 0x45, 0x6C, 0x69, 0x6D, 0x69, 0x6E,
        0x61, 0x74, 0x65, 0x64, 0x91, 0x84, 0xA8, 0x43, 0x61, 0x72, 0x49, 0x6E, 0x64, 0x65, 0x78,
        0x07, 0xA5, 0x52, 0x6F, 0x75, 0x6E, 0x64, 0x01, 0xAC, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F,
        0x73, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x10, 0xA9, 0x4C, 0x61, 0x70, 0x54, 0x69, 0x6D, 0x65,
        0x4D, 0x73, 0xCE, 0x00, 0x01, 0x64, 0x62,
    ];
    const GOLDEN_C_ADVANCE_WEEKEND: &[u8] = &[
        0x81, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAE, 0x41, 0x64, 0x76, 0x61, 0x6E, 0x63, 0x65, 0x57,
        0x65, 0x65, 0x6B, 0x65, 0x6E, 0x64,
    ];
    const GOLDEN_S_TELEMETRY_COMPACT_PRACTICE_CLOCK: &[u8] = &[
        0x92, 0xB0, 0x54, 0x65, 0x6C, 0x65, 0x6D, 0x65, 0x74, 0x72, 0x79, 0x43, 0x6F, 0x6D, 0x70,
        0x61, 0x63, 0x74, 0x96, 0xCE, 0x00, 0x01, 0xE2, 0x40, 0x02, 0x09, 0xC0, 0x90, 0x92, 0xCE,
        0x00, 0x0B, 0x82, 0x91, 0x00,
    ];
    const GOLDEN_C_CREATE_SESSION_QUALIFYING_TIME: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xB2, 0x71, 0x75, 0x61, 0x6C, 0x69, 0x66, 0x79, 0x69, 0x6E, 0x67, 0x5F, 0x73, 0x65,
        0x63, 0x6F, 0x6E, 0x64, 0x73, 0xCD, 0x03, 0x84,
    ];
    const GOLDEN_S_SESSION_JOINED_QUALIFYING_TIME: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x00,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xB1, 0x51, 0x75, 0x61, 0x6C, 0x69, 0x66, 0x79,
        0x69, 0x6E, 0x67, 0x53, 0x65, 0x63, 0x6F, 0x6E, 0x64, 0x73, 0xCD, 0x03, 0x84,
    ];
    const GOLDEN_S_TELEMETRY_COMPACT_QUALIFYING_CLOCK: &[u8] = &[
        0x92, 0xB0, 0x54, 0x65, 0x6C, 0x65, 0x6D, 0x65, 0x74, 0x72, 0x79, 0x43, 0x6F, 0x6D, 0x70,
        0x61, 0x63, 0x74, 0x96, 0xCE, 0x00, 0x01, 0xE2, 0x40, 0x02, 0x06, 0xC0, 0x90, 0x92, 0xCE,
        0x00, 0x0B, 0x82, 0x91, 0x00,
    ];
    const GOLDEN_C_CREATE_SESSION_RACE_TIME: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xAC, 0x72, 0x61, 0x63, 0x65, 0x5F, 0x73, 0x65, 0x63, 0x6F, 0x6E, 0x64, 0x73, 0xCD,
        0x1C, 0x20,
    ];
    const GOLDEN_S_SESSION_JOINED_RACE_TIME: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x00,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xAB, 0x52, 0x61, 0x63, 0x65, 0x53, 0x65, 0x63,
        0x6F, 0x6E, 0x64, 0x73, 0xCD, 0x1C, 0x20,
    ];
    const GOLDEN_S_TELEMETRY_COMPACT_RACE_CLOCK: &[u8] = &[
        0x92, 0xB0, 0x54, 0x65, 0x6C, 0x65, 0x6D, 0x65, 0x74, 0x72, 0x79, 0x43, 0x6F, 0x6D, 0x70,
        0x61, 0x63, 0x74, 0x96, 0xCE, 0x00, 0x01, 0xE2, 0x40, 0x02, 0x07, 0xC0, 0x90, 0x92, 0x00,
        0x25,
    ];
    const GOLDEN_C_CREATE_SESSION_AI_SKILL: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x08, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x03, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xA8, 0x61, 0x69, 0x5F, 0x73, 0x6B, 0x69, 0x6C, 0x6C, 0x5F,
    ];
    const GOLDEN_C_CREATE_SESSION_CLASSES: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x43, 0x72, 0x65, 0x61, 0x74, 0x65, 0x53, 0x65,
        0x73, 0x73, 0x69, 0x6F, 0x6E, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x88, 0xAF, 0x74, 0x72, 0x61,
        0x63, 0x6B, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x5F, 0x69, 0x64, 0xD9, 0x24, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x2D, 0x62, 0x62, 0x62, 0x62, 0x2D, 0x63, 0x63,
        0x63, 0x63, 0x2D, 0x64, 0x64, 0x64, 0x64, 0x2D, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0xAB, 0x6D, 0x61, 0x78, 0x5F, 0x70, 0x6C, 0x61, 0x79, 0x65,
        0x72, 0x73, 0x0C, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x0A, 0xA9, 0x6C,
        0x61, 0x70, 0x5F, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x05, 0xAC, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6F, 0x6E, 0x5F, 0x6B, 0x69, 0x6E, 0x64, 0x00, 0xAF, 0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65,
        0x64, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73, 0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3,
        0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72,
        0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F, 0x67, 0x65, 0x61, 0x72, 0x62, 0x6F,
        0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73,
        0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69, 0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E,
        0x65, 0xC3, 0xAA, 0x63, 0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7,
        0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3, 0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F,
        0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E, 0x75, 0x74, 0x65, 0x73, 0xCD, 0x03,
        0x0C, 0xA7, 0x63, 0x6C, 0x61, 0x73, 0x73, 0x65, 0x73, 0x92, 0x83, 0xA5, 0x63, 0x6C, 0x61,
        0x73, 0x73, 0xA4, 0x4C, 0x4D, 0x50, 0x32, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E,
        0x74, 0x04, 0xA8, 0x61, 0x69, 0x5F, 0x73, 0x6B, 0x69, 0x6C, 0x6C, 0x64, 0x82, 0xA5, 0x63,
        0x6C, 0x61, 0x73, 0x73, 0xA3, 0x47, 0x54, 0x33, 0xA8, 0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75,
        0x6E, 0x74, 0x06,
    ];
    const GOLDEN_S_SESSION_JOINED_CLASSES: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x0B, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x00,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA7, 0x43, 0x6C, 0x61, 0x73, 0x73, 0x65, 0x73,
        0x92, 0x83, 0xA5, 0x63, 0x6C, 0x61, 0x73, 0x73, 0xA4, 0x4C, 0x4D, 0x50, 0x32, 0xA8, 0x61,
        0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x04, 0xA8, 0x61, 0x69, 0x5F, 0x73, 0x6B, 0x69,
        0x6C, 0x6C, 0x64, 0x82, 0xA5, 0x63, 0x6C, 0x61, 0x73, 0x73, 0xA3, 0x47, 0x54, 0x33, 0xA8,
        0x61, 0x69, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x06,
    ];
    const GOLDEN_S_SESSION_JOINED_AI_SKILL: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA7, 0x41, 0x69, 0x53, 0x6B, 0x69, 0x6C, 0x6C,
        0x5F,
    ];
    const GOLDEN_S_SESSION_JOINED_NO_DAMAGE: &[u8] = &[
        0x82, 0xA4, 0x74, 0x79, 0x70, 0x65, 0xAD, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4A,
        0x6F, 0x69, 0x6E, 0x65, 0x64, 0xA4, 0x64, 0x61, 0x74, 0x61, 0x86, 0xA9, 0x53, 0x65, 0x73,
        0x73, 0x69, 0x6F, 0x6E, 0x49, 0x64, 0xD9, 0x24, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x2D, 0x38, 0x39, 0x61, 0x62, 0x2D, 0x63, 0x64, 0x65, 0x66, 0x2D, 0x30, 0x31, 0x32,
        0x33, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xB0,
        0x59, 0x6F, 0x75, 0x72, 0x47, 0x72, 0x69, 0x64, 0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F,
        0x6E, 0x03, 0xAB, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6F, 0x6E, 0x4B, 0x69, 0x6E, 0x64, 0x01,
        0xAE, 0x41, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x73,
        0x85, 0xA3, 0x61, 0x62, 0x73, 0xC3, 0xB0, 0x74, 0x72, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E,
        0x5F, 0x63, 0x6F, 0x6E, 0x74, 0x72, 0x6F, 0x6C, 0xC3, 0xAC, 0x61, 0x75, 0x74, 0x6F, 0x5F,
        0x67, 0x65, 0x61, 0x72, 0x62, 0x6F, 0x78, 0xC3, 0xAF, 0x73, 0x74, 0x65, 0x65, 0x72, 0x69,
        0x6E, 0x67, 0x5F, 0x61, 0x73, 0x73, 0x69, 0x73, 0x74, 0xC3, 0xAB, 0x72, 0x61, 0x63, 0x69,
        0x6E, 0x67, 0x5F, 0x6C, 0x69, 0x6E, 0x65, 0xC3, 0xAA, 0x43, 0x6F, 0x6E, 0x64, 0x69, 0x74,
        0x69, 0x6F, 0x6E, 0x73, 0x82, 0xA7, 0x77, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x00, 0xB3,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x66, 0x5F, 0x64, 0x61, 0x79, 0x5F, 0x6D, 0x69, 0x6E,
        0x75, 0x74, 0x65, 0x73, 0xCD, 0x03, 0x0C, 0xA6, 0x44, 0x61, 0x6D, 0x61, 0x67, 0x65, 0x00,
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
