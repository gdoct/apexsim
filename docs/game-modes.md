# Game Modes - Implementation Reference

The server supports multiple game modes that control session behavior and telemetry. Sessions can transition between modes dynamically.

## Session Properties

A session has these properties:
- `players`: List of players in the session with their cars including any AI drivers
- `game_mode`: Current game mode (enum: GameMode)
- `track`: Current track configuration
- `timing`: Lap timing data for each player
- `start_time`: Session start timestamp
- `active_players`: List of players currently driving (not in pit or disconnected)
- `countdown_ticks_remaining`: Optional countdown timer state
- `demo_lap_progress`: Progress along racing line (0.0-1.0) for Demo Lap mode

## Game Mode Enum

```rust
pub enum GameMode {
    Lobby = 0,           // ✅ Implemented
    Sandbox = 1,         // ✅ Implemented
    Countdown = 2,       // ✅ Implemented
    DemoLap = 3,         // ✅ Implemented
    FreePractice = 4,    // ✅ Implemented
    Replay = 5,          // ⏸️ Placeholder
    Qualification = 6,   // ⏸️ Not implemented
    Race = 7,            // ⏸️ Not implemented
}
```

## Implemented Modes

### Lobby Mode (GameMode::Lobby)
**Status**: ✅ Fully Implemented
**Default**: Yes (default game mode for new sessions)

**Behavior**:
- No telemetry is sent to clients
- No physics simulation occurs
- Players are selecting cars and waiting for session to start
- Tick counter increments but no game updates happen

**Implementation**:
- File: `server/src/game_session.rs:87-91`
- Method: `tick_lobby()`
- Network: No telemetry broadcast in Lobby mode

**Transitions**:
- Host can change to any other mode via `SetGameMode` message
- Typically transitions to Sandbox, Countdown, or DemoLap to start session

---

### Sandbox Mode (GameMode::Sandbox)
**Status**: ✅ Fully Implemented

**Behavior**:
- Nothing moves, no physics updates
- No telemetry is sent or recorded
- No cars are displayed (participants exist but frozen)
- Players can freely move camera around the track
- No lap timing is recorded

**Implementation**:
- File: `server/src/game_session.rs:93-98`
- Method: `tick_sandbox()`
- Network: Telemetry includes `game_mode: Sandbox` but no car updates

**Use Cases**:
- Track exploration before starting session
- Camera positioning for spectators
- Track familiarization

**Transitions**:
- Host can transition to Countdown, DemoLap, or FreePractice

---

### Countdown Mode (GameMode::Countdown)
**Status**: ✅ Fully Implemented

**Behavior**:
- Players are placed in pit lane at start position (frozen)
- No physics simulation, cars cannot move
- Countdown timer runs (default: 10 seconds = 2400 ticks at 240Hz)
- Timer decrements each tick
- When timer reaches zero, countdown clears to `None`
- Sends countdown updates to clients via telemetry

**Implementation**:
- File: `server/src/game_session.rs:100-112`
- Method: `tick_countdown()`
- State: `session.countdown_ticks_remaining: Option<u16>`
- Network: `Telemetry.countdown_ms` sent to clients

**Starting Countdown**:
```rust
// Default 10 second countdown
game_session.set_game_mode(GameMode::Countdown);

// Custom duration with next mode specified
game_session.start_countdown_mode(5, GameMode::FreePractice);
```

**Client Messages**:
- `StartCountdown { countdown_seconds, next_mode }` - Start countdown
- `CountdownUpdate { seconds_remaining }` - Server sends updates

**Transitions**:
- After countdown finishes, host must manually transition to next mode
- Typically transitions to FreePractice, Race, or Qualification

---

### Demo Lap Mode (GameMode::DemoLap)
**Status**: ✅ Fully Implemented

**Behavior**:
- Only one car is displayed (first participant)
- Server "drives" the car along the track's racing line
- Fixed speed: 50 m/s (180 km/h)
- Car follows `track.raceline` points via interpolation
- Camera positioned at 1.2m above track surface
- When lap completes (progress >= 1.0), restarts from 0.0
- Continuous loop, no lap timing

**Implementation**:
- File: `server/src/game_session.rs:114-170`
- Method: `tick_demolap()`
- State: `session.demo_lap_progress: Option<f32>` (0.0 to 1.0)
- Speed: 50.0 m/s constant
- Height: `z + 1.2` meters

**Algorithm**:
```rust
progress += (demo_speed * dt) / raceline_len;
if progress >= 1.0 { progress = 0.0; }

index = (progress * raceline_len).floor();
next_index = (index + 1) % raceline_len;
t = (progress * raceline_len) - index;

// Interpolate position
pos = p1 + (p2 - p1) * t;

// Calculate orientation
yaw_rad = atan2(dy, dx);
```

**Requirements**:
- Track must have `raceline: Vec<RacelinePoint>` defined
- If raceline is empty, mode does nothing (graceful degradation)

**Use Cases**:
- Showcase track layout to spectators
- Demonstrate ideal racing line
- Attract mode for lobby/menu screens

---

### Free Practice Mode (GameMode::FreePractice)
**Status**: ✅ Fully Implemented

**Behavior**:
- Players send input to drive cars (throttle, brake, steering)
- Full 3D physics simulation at 240Hz
- Collision detection between cars (AABB)
- Track progress tracking for each car
- Track limits NOT enforced (free driving)
- No mandatory pit stops or penalties
- Lap timing available but optional

**Implementation**:
- File: `server/src/game_session.rs:172-204`
- Method: `tick_free_practice()`
- Physics: `physics::update_car_3d()` for each car
- Collisions: `physics::check_aabb_collisions_3d()`
- Progress: `physics::update_track_progress_3d()`

**Input Handling**:
```rust
PlayerInputData {
    throttle: f32,  // 0.0 - 1.0
    brake: f32,     // 0.0 - 1.0
    steering: f32,  // -1.0 to 1.0
}
```

**Features**:
- Players start at grid positions
- Full car dynamics (acceleration, braking, steering)
- Suspension, tire, and aerodynamics simulation
- Gear shifting (automatic in current implementation)
- Real-time telemetry broadcast to all clients

**Planned Features** (from spec, not yet implemented):
- Lap time recording on crossing start/finish
- Stop lap timing when off-track or in pit
- Pit entry/exit functionality
- Car reset to pit command
- Session-wide best lap tracking per car

---

### Replay Mode (GameMode::Replay)
**Status**: ⏸️ Placeholder

**Specification**:
- Sends telemetry for all cars based on previously recorded data
- View-only mode, no input accepted from players
- Players can switch between cars to view (frontend feature)

**Current Implementation**:
- File: `server/src/game_session.rs:206-211`
- Method: `tick_replay()` - empty placeholder
- Replay recording infrastructure exists in `server/src/replay.rs`
- Replay data is recorded during Racing sessions

**To Implement**:
1. Load replay file from disk
2. Play back telemetry frame-by-frame
3. Handle playback controls (pause, rewind, speed)
4. Synchronize clients to same replay timestamp

---

### Qualification Mode (GameMode::Qualification)
**Status**: ⏸️ Not Implemented

**Specification**: To be defined later

**Current Implementation**:
- Defined in enum as `Qualification = 6`
- Falls back to FreePractice behavior in `tick()`
- Reserved for future implementation

**Typical Qualification Features** (common in racing games):
- Each driver gets limited attempts (e.g., 3 flying laps)
- Grid positions determined by best lap time
- Session time limit (e.g., 15 minutes)
- Out lap / flying lap / in lap structure

---

### Race Mode (GameMode::Race)
**Status**: ⏸️ Not Implemented

**Specification**: To be defined later

**Current Implementation**:
- Defined in enum as `Race = 7`
- Falls back to FreePractice behavior in `tick()`
- Reserved for future implementation

**Typical Race Features** (common in racing games):
- Fixed lap count or time limit
- Standing or rolling start
- Position tracking and live leaderboard
- Mandatory pit stops (optional)
- Race finish detection
- Final results and podium

---

## Mode Transitions

### Host Control
Only the session host can change game modes:
- `SetGameMode { mode }` - Immediately switch to mode
- `StartCountdown { countdown_seconds, next_mode }` - Start countdown

### Programmatic Transitions
```rust
// Immediate mode change
game_session.set_game_mode(GameMode::Sandbox);

// Start countdown with next mode
game_session.start_countdown_mode(10, GameMode::FreePractice);

// Manual transition from countdown
game_session.transition_from_countdown(GameMode::FreePractice);
```

### Transition Lifecycle
1. **Mode Change Request** - Host sends `SetGameMode` or `StartCountdown`
2. **Validation** - Server checks if requester is host
3. **State Initialization** - New mode initializes required state:
   - Countdown → sets `countdown_ticks_remaining`
   - DemoLap → sets `demo_lap_progress = 0.0`
4. **Broadcast** - Server sends `GameModeChanged { mode }` to all participants
5. **Tick Updates** - New mode's `tick_*()` method executes each frame

---

## Network Protocol

### Client → Server Messages
```rust
SetGameMode {
    mode: GameMode,
}

StartCountdown {
    countdown_seconds: u16,
    next_mode: GameMode,
}
```

### Server → Client Messages
```rust
GameModeChanged {
    mode: GameMode,
}

CountdownUpdate {
    seconds_remaining: u16,
}

Telemetry {
    server_tick: u32,
    session_state: SessionState,
    game_mode: GameMode,        // Current mode included in every telemetry
    countdown_ms: Option<u16>,   // Countdown time remaining
    car_states: Vec<CarStateTelemetry>,
}
```

### Message Priorities
- `GameModeChanged` - **Critical** (must be delivered)
- `CountdownUpdate` - **Droppable** (can be dropped under load)
- `Telemetry` - **Droppable** (high frequency, 240Hz)

---

## Testing

### Test Coverage
All modes have comprehensive unit tests in `server/src/game_session.rs`:

**22 Tests Implemented**:
- ✅ `test_default_game_mode_is_lobby`
- ✅ `test_lobby_mode_tick`
- ✅ `test_sandbox_mode_tick`
- ✅ `test_countdown_mode_decrements`
- ✅ `test_countdown_mode_finishes`
- ✅ `test_demolap_mode_initializes_progress`
- ✅ `test_demolap_mode_advances_progress`
- ✅ `test_demolap_mode_loops`
- ✅ `test_demolap_without_raceline`
- ✅ `test_free_practice_mode_updates_physics`
- ✅ `test_replay_mode_does_nothing`
- ✅ `test_set_game_mode`
- ✅ `test_start_countdown_mode`
- ✅ `test_transition_from_countdown`
- ✅ `test_transition_from_countdown_to_demolap`
- ✅ `test_mode_persists_across_ticks`
- Plus 6 existing tests for session management

**Run Tests**:
```bash
cargo test --lib game_session
```

---

## Implementation Files

| File | Lines | Purpose |
|------|-------|---------|
| `server/src/data.rs` | +28 | GameMode enum, RaceSession fields |
| `server/src/game_session.rs` | +120, +270 tests | Mode tick methods, state management, tests |
| `server/src/network.rs` | +13 | Client/server messages, telemetry |
| `server/src/main.rs` | +68 | Message handlers, mode change logic |
| `server/src/replay.rs` | +2 | Test updates for new telemetry field |

**Total**: ~500 lines of implementation + tests

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    GameSession::tick()                  │
│                                                         │
│  match session.game_mode {                             │
│    ┌──────────────────────────────────────┐           │
│    │ Lobby       → tick_lobby()           │           │
│    │ Sandbox     → tick_sandbox()         │           │
│    │ Countdown   → tick_countdown()       │           │
│    │ DemoLap     → tick_demolap()         │           │
│    │ FreePractice → tick_free_practice()  │           │
│    │ Replay      → tick_replay()          │           │
│    │ Race/Qual   → tick_free_practice()   │ (fallback)│
│    └──────────────────────────────────────┘           │
│  }                                                     │
└─────────────────────────────────────────────────────────┘
                         │
                         ▼
          ┌──────────────────────────────┐
          │  Generate Telemetry          │
          │  (includes game_mode field)  │
          └──────────────────────────────┘
                         │
                         ▼
          ┌──────────────────────────────┐
          │  Broadcast to Clients        │
          │  (240Hz tick rate)           │
          └──────────────────────────────┘
```

---

## Usage Examples

### Example 1: Start Session with Demo Lap
```rust
// Create session
let session = RaceSession::new(host_id, track_id, SessionKind::Multiplayer, 8, 0, 3);
let mut game_session = GameSession::new(session, track_config, car_configs);

// Add demo car
game_session.add_player(demo_player_id, demo_car_id);

// Start demo lap
game_session.set_game_mode(GameMode::DemoLap);

// Run game loop
loop {
    game_session.tick(&HashMap::new());
    let telemetry = game_session.get_telemetry();
    broadcast_to_clients(telemetry);
}
```

### Example 2: Countdown to Practice Session
```rust
// Start in lobby
let mut game_session = create_session(); // Default: GameMode::Lobby

// Players join and select cars
game_session.add_player(player1_id, car1_id);
game_session.add_player(player2_id, car2_id);

// Host starts 5 second countdown
game_session.start_countdown_mode(5, GameMode::FreePractice);

// Countdown runs automatically
for _ in 0..5*240 {
    game_session.tick(&player_inputs);
}

// After countdown finishes, transition to practice
game_session.transition_from_countdown(GameMode::FreePractice);

// Practice session runs
loop {
    game_session.tick(&player_inputs);
    // ...
}
```

### Example 3: Free Practice with AI
```rust
// Create session with AI
let ai_profiles = generate_default_ai_profiles(2);
let mut game_session = GameSession::with_ai_profiles(
    session, track, car_configs, ai_profiles
);

// Spawn AI drivers
game_session.spawn_ai_drivers();

// Start practice
game_session.set_game_mode(GameMode::FreePractice);

// Game loop with AI input generation
loop {
    let mut inputs = HashMap::new();

    // Generate AI inputs
    for ai_id in &game_session.session.ai_player_ids {
        inputs.insert(*ai_id, game_session.generate_ai_input(ai_id));
    }

    // Add human player inputs
    // inputs.insert(human_id, human_input);

    game_session.tick(&inputs);
}
```

---

## Future Enhancements

### Short Term
1. **Lap Timing in Free Practice**
   - Detect start/finish line crossing
   - Record individual lap times
   - Track best lap per driver
   - Session-wide best lap tracking

2. **Pit Lane Support**
   - Pit entry/exit detection
   - Speed limits in pit lane
   - Car reset to pit command
   - Pit stop timing

3. **Track Limits**
   - Optional track boundary enforcement
   - Penalty system (time/position)
   - Off-track detection
   - Invalid lap flagging

### Medium Term
4. **Replay System**
   - Load and playback recorded sessions
   - Playback controls (play/pause/rewind)
   - Speed control (0.25x to 4x)
   - Camera switching between cars

5. **Qualification Mode**
   - Limited attempt system
   - Best lap grid ordering
   - Session time limits
   - Outlap/hotlap/inlap detection

6. **Race Mode**
   - Standing/rolling start
   - Position tracking and standings
   - Lap counting and race finish
   - Final results and classifications

### Long Term
7. **Dynamic Mode Transitions**
   - Automatic transition after countdown
   - Scheduled mode sequences (Qual → Race)
   - Session director controls

8. **Advanced Features**
   - Safety car periods
   - Weather conditions
   - Damage and repairs
   - Fuel management

---

## Status Summary

| Mode | Implementation | Testing | Documentation |
|------|---------------|---------|---------------|
| Lobby | ✅ Complete | ✅ 2 tests | ✅ This doc |
| Sandbox | ✅ Complete | ✅ 1 test | ✅ This doc |
| Countdown | ✅ Complete | ✅ 2 tests | ✅ This doc |
| DemoLap | ✅ Complete | ✅ 4 tests | ✅ This doc |
| FreePractice | ✅ Basic | ✅ 1 test | ✅ This doc |
| Replay | ⏸️ Placeholder | ✅ 1 test | ✅ This doc |
| Qualification | ⏸️ Not started | ❌ None | ⏸️ TBD |
| Race | ⏸️ Not started | ❌ None | ⏸️ TBD |

**Overall Status**: ✅ **5/8 modes implemented and tested**

---

## Version History

- **v1.0** (2026-01-02) - Initial implementation
  - Added Lobby, Sandbox, Countdown, DemoLap, FreePractice modes
  - Comprehensive test suite (22 tests)
  - Network protocol support
  - Mode transition system
