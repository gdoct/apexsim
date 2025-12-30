## SimRacing Server Backend Specification (Rust) - Initial Phase

**Project Goal:** Establish a performant, authoritative backend server in Rust capable of managing race sessions, processing player input, running a basic 2D physics simulation, and distributing telemetry to clients at 240Hz. Designed with future extensibility and modding in mind.

**Language:** Rust
**Target Performance:** 240Hz simulation loop, supporting up to 16 concurrent players per session.

---

### 1. High-Level Architecture

The server is a single Rust application running as a standalone async process using `tokio`. It communicates with clients over UDP for high-frequency game state (telemetry, input) and TCP for reliable, less frequent communication (lobby, session setup, auth).

**Coordinate System:** Right-handed 2D. Origin at track start/finish line center. +X is track direction at start, +Y is left of track direction. Angles (yaw) measured counter-clockwise from +X axis.

**Core Modules:**

*   **`main.rs`:** Entry point, CLI parsing, config loading, server bootstrap.
*   **`config`:** Configuration loading and validation from TOML files.
*   **`network`:** UDP/TCP connections, message serialization via `bincode`, connection-to-player mapping.
*   **`lobby`:** Manages players not currently in a race session.
*   **`session_manager`:** Manages active and pending race sessions.
*   **`game_session`:** Contains the state and logic for a single race session (physics, race rules).
*   **`physics`:** The core 2D physics engine with simple AABB collision.
*   **`data`:** Centralized definition of all core data structures.
*   **`templates`:** Game template loading and rotation logic.
*   **`content`:** Hot-reload watcher for track/car definitions.
*   **`persistence`:** SQLite access layer for templates, sessions, telemetry.
*   **`auth`:** Token validation stub and connection authentication.
*   **`health`:** Health check endpoint and metrics collection.

---

### 2. Core Data Structures (`data.rs`)

These structures define the state and configuration of players, cars, tracks, and race sessions.

```rust
use uuid::Uuid;
use std::collections::HashMap;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = u64; // Derived from socket address hash, used internally

// --- Player State ---
#[derive(Debug, Clone)]
pub struct Player {
    pub id: PlayerId,
    pub name: String,
    pub connection_id: ConnectionId,           // Maps to network connection
    pub selected_car_config_id: Option<CarConfigId>,
    pub is_ai: bool,                           // True for AI-controlled entries
}

// --- Car Configuration (Static / Moddable) ---
// Defines the physical properties of a car model.
#[derive(Debug, Clone)]
pub struct CarConfig {
    pub id: CarConfigId,
    pub name: String,
    pub mass_kg: f32,              // Total mass of the car (700-1500 typical)
    pub length_m: f32,             // Length for collision AABB
    pub width_m: f32,              // Width for collision AABB
    pub max_engine_force_n: f32,   // Peak engine force in Newtons (5000-15000 typical)
    pub max_brake_force_n: f32,    // Peak brake force in Newtons (10000-25000 typical)
    pub drag_coefficient: f32,     // Aerodynamic drag (0.3-0.5 typical)
    pub grip_coefficient: f32,     // Tire grip multiplier (0.8-1.2 typical)
    pub max_steering_angle_rad: f32, // Max steering angle (0.4-0.6 rad typical)
    pub wheelbase_m: f32,          // Distance between axles for turning calc
}

// --- Track Configuration (Static / Moddable) ---
#[derive(Debug, Clone)]
pub struct TrackConfig {
    pub id: TrackConfigId,
    pub name: String,
    pub centerline: Vec<TrackPoint>, // Ordered points defining track centerline
    pub width_m: f32,                // Track width (uniform for initial phase)
    pub start_positions: Vec<GridSlot>, // Starting grid positions
}

#[derive(Debug, Clone)]
pub struct TrackPoint {
    pub x: f32,
    pub y: f32,
    pub distance_from_start_m: f32, // Cumulative distance along centerline
}

#[derive(Debug, Clone)]
pub struct GridSlot {
    pub position: u8,  // 1 = pole, 2 = second, etc.
    pub x: f32,
    pub y: f32,
    pub yaw_rad: f32,  // Facing direction
}

// --- Car Dynamics State (Per-Tick, Server Authoritative) ---
#[derive(Debug, Clone)]
pub struct CarState {
    pub player_id: PlayerId,
    pub car_config_id: CarConfigId,
    pub grid_position: u8,           // Starting position (1-indexed)
    pub pos_x: f32,                  // World position X (meters)
    pub pos_y: f32,                  // World position Y (meters)
    pub yaw_rad: f32,                // Orientation (radians, CCW from +X)
    pub vel_x: f32,                  // Velocity X (m/s)
    pub vel_y: f32,                  // Velocity Y (m/s)
    pub speed_mps: f32,              // Magnitude of velocity (m/s)
    pub angular_vel_rad_s: f32,      // Angular velocity (rad/s)
    pub throttle_input: f32,         // Last received throttle (0.0-1.0)
    pub brake_input: f32,            // Last received brake (0.0-1.0)
    pub steering_input: f32,         // Last received steering (-1.0 to 1.0)
    pub track_progress: f32,         // Distance along centerline (meters, wraps at lap)
    pub current_lap: u16,            // Current lap (0 = not started)
    pub finish_position: Option<u8>, // Set when player crosses finish on final lap
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,
    pub is_colliding: bool,          // True if AABB overlaps another car
}

// --- Race Session State (Server Authoritative) ---
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    Lobby,      // Players are joining, setting up
    Countdown,  // 5-second countdown before race start
    Racing,     // Race is active
    Finished,   // Race concluded, results shown
}

#[derive(Debug, Clone)]
pub struct RaceSession {
    pub id: SessionId,
    pub track_config_id: TrackConfigId,
    pub host_player_id: PlayerId,
    pub state: SessionState,
    pub participants: HashMap<PlayerId, CarState>, // All cars (human + AI)
    pub max_players: u8,
    pub ai_count: u8,                // Number of AI slots requested
    pub lap_limit: u8,
    pub current_tick: u32,
    pub countdown_ticks_remaining: Option<u16>, // Ticks until race start (240 ticks = 1 sec)
    pub race_start_tick: Option<u32>,           // Tick when racing began
}
```

---

### 3. Network Message Formats (`network.rs`)

Packets are serialized with `bincode` (compact binary, serde-compatible). The server maintains a `ConnectionId → PlayerId` mapping established during TCP auth; UDP packets do not include player/session IDs—identity is derived from source address.

**Connection Flow:**
1. Client opens TCP connection, completes TLS handshake
2. Client sends `Authenticate` with token and desired name
3. Server responds `AuthSuccess` with assigned `PlayerId`
4. Server records `(socket_addr_hash) → PlayerId` mapping
5. Client opens UDP socket from same source IP; server correlates via IP
6. Client sends periodic `Heartbeat` (every 1s); server drops connection after 5s silence

#### 3.1. Client to Server

```rust
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub enum ClientMessage {
    // TCP - Auth & Lobby
    Authenticate { token: String, player_name: String },
    Heartbeat { client_tick: u32 },
    SelectCar { car_config_id: CarConfigId },
    CreateSession { track_config_id: TrackConfigId, max_players: u8, ai_count: u8, lap_limit: u8 },
    JoinSession { session_id: SessionId },
    LeaveSession,
    StartSession, // Host only, starts countdown
    Disconnect,

    // UDP - High frequency (no IDs needed, derived from connection)
    PlayerInput {
        server_tick_ack: u32, // Last server tick client received (for latency calc)
        throttle: f32,        // 0.0 to 1.0
        brake: f32,           // 0.0 to 1.0
        steering: f32,        // -1.0 (left) to 1.0 (right)
    },
}
```

#### 3.2. Server to Client

```rust
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub enum ServerMessage {
    // TCP - Auth & Lobby
    AuthSuccess { player_id: PlayerId, server_version: u32 },
    AuthFailure { reason: String },
    HeartbeatAck { server_tick: u32 },
    LobbyState {
        players_in_lobby: Vec<LobbyPlayer>,      // Lightweight player list
        available_sessions: Vec<SessionSummary>, // Joinable sessions
        car_configs: Vec<CarConfigSummary>,
        track_configs: Vec<TrackConfigSummary>,
    },
    SessionJoined { session_id: SessionId, your_grid_position: u8 },
    SessionLeft,
    SessionStarting { countdown_seconds: u8 },
    Error { code: u16, message: String },
    PlayerDisconnected { player_id: PlayerId },

    // UDP - High frequency telemetry
    Telemetry {
        server_tick: u32,
        session_state: SessionState,
        countdown_ms: Option<u16>,   // Milliseconds until race start
        car_states: Vec<CarStateTelemetry>, // Compact per-car data
    },
}

// Lightweight structs for lobby (avoid sending full configs repeatedly)
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct LobbyPlayer {
    pub id: PlayerId,
    pub name: String,
    pub selected_car: Option<CarConfigId>,
    pub in_session: Option<SessionId>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct SessionSummary {
    pub id: SessionId,
    pub track_name: String,
    pub host_name: String,
    pub player_count: u8,
    pub max_players: u8,
    pub state: SessionState,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CarConfigSummary {
    pub id: CarConfigId,
    pub name: String,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct TrackConfigSummary {
    pub id: TrackConfigId,
    pub name: String,
}

// Compact telemetry per car (sent at 240Hz, keep small)
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
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
```

---

### 4. Server Processes and Logic Flow

#### 4.1. Server Initialization (`main.rs`)

1. Parse CLI arguments (config path, log level, bind addresses)
2. Load and validate `server.toml` configuration
3. Initialize `tracing` subscriber with console + rolling file output
4. Open SQLite database, run migrations
5. Scan `./content/cars/` and `./content/tracks/` directories, load manifests
6. Load race templates from database
7. Start content watcher for hot-reload
8. Bind TCP socket (with TLS) and UDP socket
9. Spawn lobby manager and session manager actors
10. Enter main server loop

#### 4.2. Main Server Loop (`main.rs`)

The loop runs at a fixed 240Hz tick rate using `tokio::time::interval`.

```
Loop (240Hz, Δt = 4.1667ms):
  // 1. Process Network Input
  Poll TCP listener for new connections → spawn auth handler task
  Poll TCP streams for lobby/session messages → dispatch to managers
  Poll UDP socket for PlayerInput → update input buffer keyed by ConnectionId

  // 2. Update Game Sessions
  For each active RaceSession in Racing or Countdown state:
    Increment session.current_tick
    If Countdown: decrement countdown_ticks_remaining; transition to Racing when 0
    If Racing:
      For each participant:
        Fetch latest input from buffer (default to coast if missing)
        For AI participants: generate synthetic input via AI driver
        Call physics::update_car_2d()
        Update track_progress via centerline projection
        Detect lap completion (progress wrap)
        Check AABB collisions, set is_colliding flags
      Check race completion (all cars finished or timeout)

  // 3. Send Network Output
  For each client in a Racing session:
    Build Telemetry packet with all car states
    Send via UDP (unreliable, latest-state-wins)
  For clients in Lobby (at 4Hz, not 240Hz):
    Send LobbyState via TCP if state changed

  // 4. Persistence (batched)
  Every 60 ticks (250ms): flush accumulated telemetry frames to SQLite

  // 5. Housekeeping
  Check heartbeat timeouts → disconnect stale clients
  Process graceful shutdown signal (SIGTERM) → drain sessions, close connections
```

#### 4.3. `Lobby` Module

*   **Responsibility:** Track authenticated players not in a session.
*   **Data:** `HashMap<PlayerId, Player>`, `HashMap<ConnectionId, PlayerId>`.
*   **Actions:**
    *   `add_player(connection_id, player)` — on successful auth
    *   `remove_player(player_id)` — on disconnect or session join
    *   `get_lobby_state()` — returns `LobbyState` for broadcast
    *   Broadcast `LobbyState` to all lobby clients at 4Hz or on change

#### 4.4. `SessionManager` Module

*   **Responsibility:** Create, track, and destroy `RaceSession` instances.
*   **Data:** `HashMap<SessionId, RaceSession>`.
*   **Actions:**
    *   `create_session(host_id, track_id, max_players, ai_count, lap_limit)` — returns `SessionId`
    *   `join_session(player_id, session_id)` — assigns grid position, adds to participants
    *   `leave_session(player_id)` — removes from participants, reassign host if needed
    *   `start_session(session_id, requester_id)` — host-only, sets `Countdown` state
    *   `tick_all_sessions(inputs)` — called by main loop, advances all active sessions
    *   `cleanup_finished_sessions()` — archive results, remove from active map

#### 4.5. `GameSession` Module

*   **Responsibility:** Encapsulate simulation logic for one race.
*   **Data:** Owned `RaceSession` struct, reference to `TrackConfig` and `CarConfig` registry.
*   **Methods:**
    *   `tick(inputs: &HashMap<PlayerId, PlayerInputData>)` — main update
    *   `spawn_ai_drivers(count: u8)` — create AI entries at empty grid slots
    *   `get_telemetry() -> Telemetry` — build telemetry packet
    *   `is_race_complete() -> bool` — all cars finished or time limit
    *   `get_results() -> RaceResults` — final standings for persistence

#### 4.6. `Physics` Module (`physics.rs`)

Implements a simplified 2D bicycle model.

**`update_car_2d(state: &mut CarState, config: &CarConfig, input: &PlayerInputData, dt: f32)`:**

```rust
// 1. Longitudinal forces
let throttle_force = input.throttle * config.max_engine_force_n;
let brake_force = input.brake * config.max_brake_force_n;
let drag_force = config.drag_coefficient * state.speed_mps.powi(2);
let rolling_resistance = 100.0; // Constant N, keeps cars from rolling forever

let net_force = throttle_force - brake_force - drag_force - rolling_resistance.copysign(state.speed_mps);
let accel = net_force / config.mass_kg;

// 2. Update speed (clamp to prevent reversing under brake)
state.speed_mps = (state.speed_mps + accel * dt).max(0.0);

// 3. Steering (bicycle model)
let steering_angle = input.steering * config.max_steering_angle_rad;
let turn_radius = config.wheelbase_m / steering_angle.tan().abs().max(0.001);
state.angular_vel_rad_s = state.speed_mps / turn_radius * steering_angle.signum();

// 4. Apply grip limit (simplified: cap lateral accel)
let max_lateral_accel = config.grip_coefficient * 9.81; // ~1g for street tires
let actual_lateral_accel = state.speed_mps * state.angular_vel_rad_s.abs();
if actual_lateral_accel > max_lateral_accel {
    state.angular_vel_rad_s *= max_lateral_accel / actual_lateral_accel;
}

// 5. Integrate position and orientation
state.yaw_rad += state.angular_vel_rad_s * dt;
state.vel_x = state.speed_mps * state.yaw_rad.cos();
state.vel_y = state.speed_mps * state.yaw_rad.sin();
state.pos_x += state.vel_x * dt;
state.pos_y += state.vel_y * dt;

// 6. Store inputs for telemetry
state.throttle_input = input.throttle;
state.brake_input = input.brake;
state.steering_input = input.steering;
```

**`check_aabb_collisions(states: &mut [CarState], configs: &HashMap<CarConfigId, CarConfig>)`:**
- For each pair of cars, compute axis-aligned bounding boxes (rotated AABB approximation)
- If overlap detected, set `is_colliding = true` on both cars
- Apply simple separation impulse (push cars apart along collision normal)
- Reduce speed of colliding cars by 20% (energy loss)

**`update_track_progress(state: &mut CarState, centerline: &[TrackPoint], track_length: f32)`:**
- Project car position onto nearest centerline segment
- Update `track_progress` to cumulative distance at projection point
- Detect lap completion when `track_progress` wraps (crosses start/finish with sufficient progress)

#### 4.7. AI Drivers

*   **Representation:** AI drivers are `Player` entries with `is_ai = true`. They exist in `participants` alongside humans.
*   **Input Generation:** Each tick, `GameSession::generate_ai_input(player_id) -> PlayerInputData`:
    *   Find next target point on centerline (look-ahead based on speed)
    *   Compute steering to aim at target (proportional control)
    *   Set throttle to 0.8 on straights, reduce in curves (based on curvature)
    *   Brake if approaching sharp turn or slower car ahead
*   **Spawning:** On session creation or countdown start, fill empty grid slots up to `ai_count` with AI drivers.
*   **Networking:** AI cars have no network connection; their telemetry is broadcast like any other car.

---

### 5. Configuration (`config.rs`)

Server configuration is loaded from `server.toml` at startup. Environment variables can override any setting using `APEXSIM_` prefix (e.g., `APEXSIM_NETWORK_TCP_PORT=9001`).

```toml
# server.toml

[server]
tick_rate_hz = 240
max_sessions = 8
session_timeout_seconds = 300  # Cleanup finished sessions after 5 min

[network]
tcp_bind = "0.0.0.0:9000"
udp_bind = "0.0.0.0:9001"
tls_cert_path = "./certs/server.crt"
tls_key_path = "./certs/server.key"
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[content]
cars_dir = "./content/cars"
tracks_dir = "./content/tracks"
watch_interval_ms = 2000

[persistence]
database_path = "./data/apexsim.db"
telemetry_batch_size = 60          # Frames per batch insert
telemetry_retention_hours = 168    # 7 days

[logging]
level = "info"                     # trace, debug, info, warn, error
console_enabled = true
file_enabled = true
file_path = "./logs/apexsim.log"
file_rotation = "daily"
file_retention_days = 7

[auth]
require_token = false              # Stub: accept all tokens when false
```

**CLI Arguments:**

```
apexsim-server [OPTIONS]

OPTIONS:
    -c, --config <PATH>     Path to server.toml [default: ./server.toml]
    -l, --log-level <LEVEL> Override log level (trace|debug|info|warn|error)
    --tcp-port <PORT>       Override TCP bind port
    --udp-port <PORT>       Override UDP bind port
```

---

### 6. Authentication & Transport

*   **TCP Control Channel:** All TCP traffic is wrapped in TLS 1.3 using `rustls`. Self-signed certs are acceptable for development; production should use proper PKI.
*   **Authentication Flow:**
    1. Client connects via TCP+TLS
    2. Client sends `Authenticate { token, player_name }`
    3. Server validates token (stub: accept all non-empty tokens)
    4. Server responds `AuthSuccess { player_id, server_version }` or `AuthFailure`
    5. Server records `(source_ip, player_id)` mapping for UDP correlation
*   **UDP Telemetry:** Unencrypted for initial phase (latency-sensitive). The `source_ip → player_id` mapping provides implicit authentication. DTLS can be added later behind a feature flag.
*   **Heartbeat:** Clients send `Heartbeat` every 1 second via TCP. Server responds `HeartbeatAck`. Clients silent for 5 seconds are disconnected.
*   **Rate Limiting:** Max 10 TCP messages per second per connection. Max 300 UDP packets per second per source IP. Violations trigger warning log; persistent abuse triggers disconnect.
*   **Input Validation:** All numeric inputs are clamped server-side (throttle/brake to 0-1, steering to -1 to 1). Malformed packets are logged and dropped.

---

### 7. Logging

*   **Framework:** `tracing` with `tracing-subscriber` for structured, async-safe logging.
*   **Console Output:** Human-readable format with colors for dev. Includes timestamp, level, span context (session_id, player_id).
*   **File Output:** JSON-lines format via `tracing-appender`. Daily rotation, 7-day retention. Async buffered writes to avoid blocking game loop.
*   **Log Levels:**
    *   `error`: Unrecoverable failures, panics caught by guard
    *   `warn`: Malformed packets, connection timeouts, recoverable errors
    *   `info`: Session lifecycle (create/start/finish), player join/leave, server start/stop
    *   `debug`: Per-tick timing, physics edge cases, AI decisions
    *   `trace`: Every packet received/sent, full state dumps (feature-gated)
*   **Span Context:** Network handlers and session updates wrap operations in spans with `session_id` and `player_id` fields for correlation.
*   **Performance:** Use `tracing`'s compile-time filtering. `trace` level requires `--features trace-logging` to avoid any overhead in release builds.

---

### 8. Error Handling & Testing

**Error Handling:**
*   **Typed Errors:** Use `thiserror` for domain errors (`SessionError`, `NetworkError`, `PhysicsError`). Expose structured error codes to clients via `ServerMessage::Error { code, message }`.
*   **Network Resilience:** Malformed packets log at `warn` and are dropped. After 10 malformed packets in 60 seconds from one connection, disconnect with `AuthFailure { reason: "protocol violation" }`.
*   **Panic Policy:** The main loop must not panic. Wrap per-session `tick()` calls in `catch_unwind`. If a session panics, log the error, mark session as `Finished` with error result, notify players, and continue serving other sessions.
*   **Graceful Shutdown:** On `SIGTERM`/`SIGINT`, stop accepting new connections, send `Error { code: 503, message: "server shutting down" }` to all clients, wait up to 10 seconds for sessions to finish current lap, then force-close.

**Testing Strategy:**
*   **Unit Tests:** Required for:
    *   `physics::update_car_2d` — verify acceleration, braking, turning at known inputs
    *   `physics::check_aabb_collisions` — verify collision detection and response
    *   `physics::update_track_progress` — verify lap detection edge cases
    *   Message serialization round-trips
    *   Session state machine transitions
    *   Template parsing and validation
*   **Integration Tests:** `#[tokio::test]` with in-process server:
    *   Connect mock client, authenticate, join session, send inputs, receive telemetry
    *   Verify race start/finish flow
    *   Verify AI driver behavior (completes laps)
*   **Property Tests:** `proptest` for:
    *   Physics invariants: `speed_mps >= 0`, position changes bounded by speed × dt
    *   Serialization: `deserialize(serialize(msg)) == msg` for all message variants

---

### 9. Game Templates & Scheduling

**Template Schema** (stored in SQLite `race_templates` and/or `./content/templates/*.toml`):

```rust
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct RaceTemplate {
    pub id: Uuid,
    pub name: String,
    pub track_config_id: TrackConfigId,
    pub allowed_cars: Vec<CarConfigId>,  // Empty = all cars allowed
    pub max_players: u8,                 // 2-16
    pub default_ai_count: u8,            // AI slots to fill
    pub lap_limit: u8,                   // 1-99
    pub start_mode: StartMode,
    pub rotation_weight: u8,             // 0-100, higher = more frequent in rotation
    pub active: bool,                    // Inactive templates are skipped
}

#[derive(Debug, Clone, Copy, serde::Serialize, serde::Deserialize)]
pub enum StartMode {
    Manual,          // Host clicks start
    Countdown,       // Auto-start when min players reached, after 60s lobby
    Scheduled,       // Start at specific wall-clock time
}
```

**Rotation:** After a session finishes, the server selects the next template weighted by `rotation_weight`. The selected template is announced to lobby clients via `LobbyState`. Players have 60 seconds to join before countdown begins (if `Countdown` mode).

**Scheduling:** For `Scheduled` templates, the server maintains a cron-like table. 5 minutes before start time, a session is pre-created and announced. Players can join early. At start time, countdown begins regardless of player count.

**Overrides:** When a player creates a custom session (not from rotation), they can override `lap_limit` and `ai_count` within template-defined bounds.

---

### 10. Content Watcher (Tracks & Cars)

**Directory Structure:**
```
./content/
├── cars/
│   ├── gt3_generic/
│   │   └── car.toml
│   └── formula_basic/
│       └── car.toml
├── tracks/
│   ├── oval_simple/
│   │   ├── track.toml
│   │   └── centerline.csv
│   └── road_course_a/
│       ├── track.toml
│       └── centerline.csv
└── templates/
    ├── gt3_sprint.toml
    └── formula_endurance.toml
```

**Car Manifest (`car.toml`):**
```toml
id = "550e8400-e29b-41d4-a716-446655440000"
name = "GT3 Generic"
version = "1.0.0"

[physics]
mass_kg = 1300.0
length_m = 4.5
width_m = 2.0
max_engine_force_n = 8000.0
max_brake_force_n = 15000.0
drag_coefficient = 0.35
grip_coefficient = 1.0
max_steering_angle_rad = 0.5
wheelbase_m = 2.7
```

**Track Manifest (`track.toml`):**
```toml
id = "660e8400-e29b-41d4-a716-446655440001"
name = "Simple Oval"
version = "1.0.0"
width_m = 15.0
centerline_file = "centerline.csv"  # x,y,distance_from_start per row
```

**Watcher Behavior:**
*   On startup: scan `cars_dir` and `tracks_dir`, parse all manifests, build in-memory registry
*   Runtime: use `notify` crate to watch directories. On file change, re-parse affected manifest
*   On successful reload: update registry, broadcast `LobbyState` to all lobby clients
*   On parse error: log `warn`, keep previous version in registry, quarantine the entry
*   Templates referencing missing content IDs are marked `active = false` until content appears

---

### 11. Persistence (SQLite)

**Database:** SQLite with WAL mode, located at `./data/apexsim.db`. Use `sqlx` with compile-time query checking.

**Schema:**

```sql
-- Applied via embedded migrations on startup

CREATE TABLE race_templates (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    track_config_id TEXT NOT NULL,
    allowed_cars TEXT NOT NULL,      -- JSON array of CarConfigId
    max_players INTEGER NOT NULL,
    default_ai_count INTEGER NOT NULL,
    lap_limit INTEGER NOT NULL,
    start_mode TEXT NOT NULL,        -- 'manual', 'countdown', 'scheduled'
    rotation_weight INTEGER NOT NULL,
    active INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE race_sessions (
    id TEXT PRIMARY KEY,
    template_id TEXT,                -- NULL if custom session
    track_config_id TEXT NOT NULL,
    started_at INTEGER NOT NULL,     -- Unix timestamp
    finished_at INTEGER,
    result TEXT                      -- JSON: { winner_id, standings: [...], dnf: [...] }
);

CREATE TABLE telemetry_frames (
    session_id TEXT NOT NULL,
    tick INTEGER NOT NULL,
    payload BLOB NOT NULL,           -- bincode-serialized Telemetry struct
    PRIMARY KEY (session_id, tick)
) WITHOUT ROWID;                     -- Optimized for sequential writes

CREATE INDEX idx_telemetry_session ON telemetry_frames(session_id);
CREATE INDEX idx_sessions_finished ON race_sessions(finished_at);
```

**Write Strategy:**
*   Telemetry frames are buffered in memory (60 frames = 250ms worth)
*   Batch insert using `INSERT INTO telemetry_frames VALUES (?,?,?), (?,?,?), ...`
*   On session end, flush remaining buffer immediately
*   Use a dedicated background task for writes to avoid blocking game loop

**Retention:**
*   Nightly job deletes `telemetry_frames` older than `telemetry_retention_hours`
*   `race_sessions` summaries are kept indefinitely (small rows)
*   Database is vacuumed weekly

---

### 12. Health & Metrics

**Health Endpoint:** HTTP GET on port 9002 (configurable):
*   `/health` — returns 200 if server is accepting connections, 503 during shutdown
*   `/ready` — returns 200 if content loaded and database connected, 503 otherwise

**Metrics (via `metrics` crate, exposed as Prometheus on `/metrics`):**
*   `apexsim_connected_players` — gauge of current player count
*   `apexsim_active_sessions` — gauge of racing sessions
*   `apexsim_tick_duration_us` — histogram of main loop tick time
*   `apexsim_telemetry_packets_sent` — counter
*   `apexsim_input_packets_received` — counter
*   `apexsim_db_write_duration_ms` — histogram of batch insert time

---

### 13. Dependencies (Cargo.toml)

```toml
[dependencies]
tokio = { version = "1", features = ["full"] }
uuid = { version = "1", features = ["v4", "serde"] }
serde = { version = "1", features = ["derive"] }
bincode = "1"
thiserror = "1"
tracing = "0.1"
tracing-subscriber = { version = "0.3", features = ["json", "env-filter"] }
tracing-appender = "0.2"
rustls = "0.23"
tokio-rustls = "0.26"
sqlx = { version = "0.8", features = ["runtime-tokio", "sqlite"] }
notify = "6"
toml = "0.8"
clap = { version = "4", features = ["derive"] }
metrics = "0.23"
metrics-exporter-prometheus = "0.15"

[dev-dependencies]
proptest = "1"
tokio-test = "0.4"
```

---

### 14. Future Considerations (Post-Initial Phase)

*   **FlatBuffers Migration:** Once message schemas are stable, migrate from `bincode` to FlatBuffers for zero-copy deserialization and better cross-language support.
*   **DTLS for UDP:** Add encrypted UDP via DTLS 1.3 for production deployments.
*   **Advanced Physics:** Tire slip model, suspension, proper collision impulse resolution.
*   **Client-Side Prediction:** Requires deterministic physics; add input buffering and rollback on client.
*   **Spectator Mode:** Read-only telemetry stream for non-participants.
*   **Replays:** Serve telemetry frames from SQLite to clients for playback.
*   **Real Authentication:** JWT validation against external auth service.
*   **Horizontal Scaling:** Extract session state to shared storage; run multiple server instances behind load balancer.

---

