# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ApexSim is an open-source simracing platform with a high-frequency authoritative Rust server (240Hz) and a Godot 4.5 C# client (the client is deprecated; server work does not need to keep it in sync). The server owns physics simulation and distributes telemetry while handling lobby/session management over TCP+TLS with MessagePack serialization. Protocol v2: after a token handshake binds the client's UDP address, telemetry (compact positional encoding, session-scoped car indices announced via a reliable `SessionRoster` message) and player input flow over UDP, with TCP fallback for un-handshaken clients.

## Build Commands

### Server (Rust)
```bash
cd server
cargo build                    # Debug build
cargo build --release          # Release build
cargo run                      # Run server with default server.toml
cargo run -- --config path.toml --log-level debug  # Custom config
cargo fmt && cargo clippy --all-targets            # Lint (CI enforces fmt --check)
cargo bench                    # Criterion hot-loop benchmarks (benches/physics_tick.rs)
```

### Server Tests
```bash
cd server
cargo test                     # Unit + integration tests (server spawned in-process, no setup needed)
cargo test -- --ignored        # Long-running stress/soak tests
cargo test --test integration_test test_name -- --nocapture  # Single integration test
```

Integration tests spawn the server in-process on ephemeral ports via `apexsim_server::server::run_server` (see `tests/common/mod.rs`); no manually started server is required. `tests/determinism_test.rs` asserts bit-identical sim runs — keep the simulation free of HashMap-iteration-order dependence, wall-clock reads, and RNG.

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
cargo run                                    # Run track editor
cargo run --bin ats-export -- --all          # Bake every track for Unreal
```

### Standalone game build
`scripts/build_game_standalone.ps1` runs UAT BuildCookRun into `artifacts/ApexSim-Win64`.
Packaging relies on `bCookAll=True` in `DefaultGame.ini`: the track levels and
catalog tables are only ever found by path at runtime, and the cooker's
`DirectoriesToAlwaysCook` scan ignores `.umap` files, so without cook-all a
packaged build races in an empty world with no previews.

### Track pipeline into Unreal
Circuits reach the Unreal client in two generated steps; both outputs are
regenerated wholesale and neither should be hand-edited.

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
                                                         # -> content/tracks/export/*.uescene.json (gitignored)
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject \
    -run=ApexTrackImport -all                            # -> game-unreal/Content/Tracks/<Track>/L_<Track>.umap
```

`scripts/build_track_levels.ps1` runs both steps (optionally building the
`ApexSimEditor` target first) and finds the engine install from the
`.uproject`'s `EngineAssociation`; `-Track A,B` narrows it to a few circuits,
`-DryRun` reports without writing assets. Note that `ats-export` resolves
`content/tracks/{real,export}` relative to the working directory, so it must be
run from the repo root — not from `track-editor/`.

The exporter resolves `.ats` station spans against the YAML centerline and
bakes triangles (Unreal can't read YAML); the `ApexTrackEditor` module's
commandlet turns those buffers into static meshes, materials and a level per
track. `AApexRaceDirector` streams `/Game/Tracks/<Stem>/L_<Stem>` as a level
instance when a race starts, resolving `<Stem>` from the session's
`TrackFile`. See `track-editor/TRACK_EDITOR.md` §5 for the format and the
coordinate/winding conventions.

The track picker's names, metadata and preview art come from a local
`DT_TrackCatalog` data table keyed by `track_id` (the wire protocol only
carries id and name). Adding or renaming a track means syncing that table:

```bash
python scripts/build_track_catalog.py          # -> content/tracks/export/{track_catalog.json,previews/*.png}
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexTrackCatalogSync                    # adds missing rows + imports T_Track_<Stem> textures
```

The sync is additive unless `-force`; existing rows keep their values. Every
track YAML needs a fixed `track_id` — without one the server mints a new UUID
per start and no catalog row can ever match it.

## Architecture

### Server (`server/`)
- **240Hz authoritative physics loop** using tokio async runtime
- **TCP+TLS** for auth, lobby, session management, rosters (port 9000). TLS is fail-closed by default (`require_tls = true`); dev opt-out in server.toml
- **UDP** (port 9001) for telemetry out / player input in, bound per connection via `UdpHandshake` with the token issued in `AuthSuccess`; telemetry broadcast rate is `tick_rate / network.telemetry_divisor` (default 60Hz)
- **HTTP endpoints** on port 9002: `/health`, `/ready`, `/metrics` (Prometheus text format)
- Key modules:
  - `server.rs` — `run_server()` entry point, `ServerState`, `ServerHandle` (used by binary and tests)
  - `game_loop/` — the tick orchestrator: `dispatch.rs` (message handlers), `tick.rs` (session ticking + panic boundary), `broadcast.rs` (telemetry fan-out, serialized once per session), `lifecycle.rs` (unified disconnect)
  - `transport.rs` — TCP/TLS/UDP IO, token auth, per-connection rate limiting, backpressure with priority-based drops
  - `physics.rs` — 4-wheel 3D vehicle model (per-wheel loads, Pacejka-style tires, suspension), yaw-aware OBB collision (SAT), windowed nearest-centerline search cached per car
  - `game_session.rs` — session/game-mode state machine, `lobby.rs` — matchmaking (single-lock), `metrics.rs`, `config.rs`, `car_loader.rs`, `track_loader.rs` (adaptive-density Catmull-Rom spline)

### Godot Client (`game-godot/`)
- C# scripts in `scripts/csharp/`
- Custom MessagePack serializer matching Rust `rmp_serde` (named/`to_vec_named`) format — wire-format changes must be coordinated between server and client
- Network protocol: `[4-byte big-endian length][MessagePack data]`
- Thread-safe networking: background receive, main thread processing

### Unreal client input (`game-unreal/Source/ApexSim/`)
Driving uses Enhanced Input, with actions and the mapping context built in
C++ (`Input/ApexInputConfig.h`) rather than as `.uasset`s, so bindings are
readable in a diff. `AApexPlayerController` owns them and adds the mapping
context only while a race is running. Defaults: WASD to drive, Q/E to shift,
C to swap cockpit/chase, Escape to leave.

Two traps worth remembering: the menu shell runs in `FInputModeUIOnly`, where
the viewport discards game input entirely and no binding produces an event
(the controller switches to game-and-UI for the race); and a Blueprint game
mode can silently override `PlayerControllerClass`, which the C++ game mode
now logs an error about.

### Content (`content/`)
- `cars/` - Car physics definitions (TOML: `car.toml` per car; most physical parameters moddable with validated ranges)
- `tracks/` - Track definitions (YAML/JSON) + procedural terrain caches (`.terrain.msgpack`)
- Shared between server and clients

## Key Technical Details

- **Coordinate System**: Right-handed. Origin at track start/finish line center. +X is track direction, +Y is left of track. Angles (yaw) counter-clockwise from +X.
- **Serialization**: MessagePack via `rmp-serde` (Rust) and custom serializer (C#)
- **Physics**: 4-wheel 3D model with per-wheel loads and suspension; fixed timestep dt = 1/tick_rate (plumbed from config, not hardcoded)
- **Determinism**: `participants` is a `BTreeMap` (ordered iteration); AI noise is hash-based; no env/wall-clock reads in the sim path. Guarded by `tests/determinism_test.rs`
- **Hot loop**: nearest-centerline queries use a windowed search seeded by each car's cached index (`CarState::nearest_centerline_idx`) — keep new per-tick track queries on this path
- **AI Drivers**: deterministic synthetic input per tick from line look-ahead
- **Bounded queues**: Network channels use bounded MPSC to prevent OOM; droppable messages (telemetry) may be dropped for slow clients

## Configuration

Server config in `server.toml` (validated at startup; the server refuses to start on a present-but-invalid file):
- `[server]`: `tick_rate_hz` (default 240), `max_sessions`, `session_timeout_seconds`
- `[network]`: TCP/UDP/health bind addresses, TLS cert paths (`require_tls` fail-closed default), heartbeat settings
- `[content]`: paths to car/track manifests
- `[logging]`: level, `console_enabled`, optional `file_enabled`/`file_dir` (JSON-lines, daily rotation)
- `[auth]`: `mode = "dev"` (accept all, development only) or `mode = "token"` with shared secrets in `tokens`
- `[ai]`: AI driver defaults (optional)

Environment overrides use the `APEXSIM_` prefix, e.g. `APEXSIM_NETWORK_TCP_PORT=9100`, `APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9000`, `APEXSIM_SERVER_TICK_RATE_HZ=120` (see `ServerConfig::apply_env_overrides`).

## Testing Notes

- `proptest` property tests live in `tests/physics_property_tests.rs` (physics invariants, serialization roundtrips)
- Integration tests simulate real client connections with `TestClient` structs against an in-process server
- Enable debug logging: `RUST_LOG=debug cargo test ...`
- CI (`.github/workflows/ci.yml`) runs fmt-check, clippy (`-D warnings` — keep the tree warning-free), build, and tests for the server plus a dotnet build/test for the client; Dependabot auto-merge builds and tests before merging
