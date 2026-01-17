# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ApexSim is an open-source simracing platform with a high-frequency authoritative Rust server (240Hz) and a Godot 4.5 C# client. The server owns physics simulation and distributes telemetry via UDP while handling lobby/session management over TCP with MessagePack serialization.

## Build Commands

### Server (Rust)
```bash
cd server
cargo build                    # Debug build
cargo build --release          # Release build
cargo run                      # Run server with default server.toml
cargo run -- --config path.toml --log-level debug  # Custom config
cargo fmt && cargo clippy --all-targets            # Lint
```

### Server Tests
```bash
cd server
cargo test                     # Unit tests
cargo test --test integration_test test_name -- --ignored --nocapture  # Single integration test
cargo run --bin test-runner    # Interactive test runner (recommended)
./run-tests.sh                 # Alternative way to run interactive test runner
```

Integration tests marked with `[S]` require the server running on `127.0.0.1:9000`.

### Godot Client (C#)
```bash
cd game-godot
dotnet build                   # Build C# project
dotnet test                    # Run tests (in ApexSim.Tests/)
```
Open in Godot 4.5+ Mono editor, click Build, then F5 to run.

### Track Editor (Rust + Bevy)
```bash
cd track-editor
cargo run                      # Run track editor
```

## Architecture

### Server (`server/`)
- **240Hz authoritative physics loop** using tokio async runtime
- **TCP+TLS** for auth, lobby, session management (port 9000)
- **UDP** for high-frequency telemetry/input (port 9001)
- **HTTP health endpoints** on port 9002 (`/health`, `/ready`)
- Key modules: `transport.rs` (networking), `physics.rs` (2D bicycle model + AABB collision), `game_session.rs` (race logic), `lobby.rs` (player management)

### Godot Client (`game-godot/`)
- C# scripts in `scripts/csharp/`
- Custom MessagePack serializer matching Rust `rmp_serde` format
- Network protocol: `[4-byte big-endian length][MessagePack data]`
- Thread-safe networking: background receive, main thread processing

### Content (`content/`)
- `cars/` - Car physics definitions (YAML)
- `tracks/` - Track centerlines, width, racing lines (YAML + MessagePack meshes)
- Shared between server and clients

## Key Technical Details

- **Coordinate System**: Right-handed 2D. Origin at track start/finish line center. +X is track direction, +Y is left of track. Angles (yaw) counter-clockwise from +X.
- **Serialization**: MessagePack via `rmp-serde` (Rust) and custom serializer (C#)
- **Physics**: Simplified 2D bicycle model with AABB collision detection
- **AI Drivers**: Generated synthetic input each tick based on centerline look-ahead
- **Bounded queues**: Network channels use bounded MPSC to prevent OOM; droppable messages (telemetry) may be dropped for slow clients

## Configuration

Server config in `server.toml`:
- `[network]`: TCP/UDP bind addresses, TLS cert paths, heartbeat settings
- `[simulation]`: tick_rate_hz (default 240), max_players, countdown duration
- `[content]`: paths to car/track manifests
- `[logging]`: log level (trace/debug/info/warn/error)

Environment override: `APEXSIM_NETWORK_TCP_PORT=9001`

## Testing Notes

- Tests use `proptest` for property-based testing of physics invariants and serialization roundtrips
- Integration tests simulate real client connections with `TestClient` struct
- Enable debug logging: `RUST_LOG=debug cargo test ...`
