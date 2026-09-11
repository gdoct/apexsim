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

### Release package
`scripts/build_release.ps1` runs the whole pipeline front to back � server,
track levels, track catalog, client package � and assembles a folder a player
can unzip and run:

```powershell
./scripts/build_release.ps1 -Zip                     # artifacts/release/ApexSim-<ver>-Win64[.zip]
./scripts/build_release.ps1 -SkipClient -SkipTracks  # reuse what is already built
```

Layout: `Game/` (the packaged client, plus a `settings.sample.yml`), `Server/` (`apexsim-server.exe`,
`server.toml` and only the content the server reads � `car.toml` per car and
the track YAML, not the `.glb` models or `.ats` sidecars), plus `Play.bat`,
`Start-Server.bat`, `README.txt`, `LICENSE` and `release.json`.

The run aborts before any long build if the car or track data is missing, or if
a track YAML has no `track_id`. Every stage has a `-Skip*` switch for when one
piece is mid-refactor, but a skipped stage must still find the output it would
have produced � a `-SkipTracks` run checks `L_<Stem>.umap` exists for every
circuit rather than shipping a package that races in an empty world.
`build_game_standalone.ps1` archives into `<dir>\Windows`, so the release
script points it straight at the release folder and renames that to `Game`
instead of copying 1.8 GB.

The Unreal Engine lookup shared by all three scripts lives in
`scripts/lib/ApexEngine.ps1`.

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

The exporter also writes two gitignored sidecars (like the exports) into
`content/tracks/real/`, both loaded by the server from beside the YAML and
both shipped in `Server/` by `build_release.ps1`:

- `<Stem>.ground.msgpack` — a 4 m heightfield of the ground the client
  renders, in the server frame (`ground.rs`), so a car that leaves the
  asphalt follows the rendered verge and terrain instead of holding road
  height. Without it, off-track elevation falls back to the centerline.
- `<Stem>.curbs.msgpack` — how far the curbs reach past each road edge,
  one sample per metre of centerline station (`curbs.rs`). The curbs are
  authored in the `.ats` and reach the server only as this number: physics
  counts a car within the band as on the track, with `curb_grip` and no
  off-track speed penalty. Without it the road edge is the track limit and
  a driver using the curbs is slowed as if on grass.

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
  - `curbs.rs` — baked curb widths per station; the sim's track limits
  - `racing_line.rs` - the racing-line driving aid: a per-car speed profile along the raceline (throttle / partial / brake per point), sent to the joining player as `RacingLine`
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
C to swap cockpit/chase, `,`/`.` to look aside, B to look behind, Escape to
leave.

Two traps worth remembering: the menu shell runs in `FInputModeUIOnly`, where
the viewport discards game input entirely and no binding produces an event
(the controller switches to game-and-UI for the race); and a Blueprint game
mode can silently override `PlayerControllerClass`, which the C++ game mode
now logs an error about.

### Racing line (`Race/ApexRacingLineActor`)
Gameplay settings -> Racing line: OFF / BRAKING ONLY / FULL (default off).
The server works the line out per car (`server/src/racing_line.rs`: the
track's raceline, or its centerline, with a quasi-steady-state speed profile
from the car's grip, downforce, power and brakes) and sends it as
`RacingLine` right after `SessionJoined`. `AApexRacingLineActor` draws it as
dots on the road, one instanced mesh per colour (green flat out, amber at the
grip limit or lifting, red braking), dropped onto the track level's own meshes
by line traces once the level is visible. BRAKING ONLY draws just the red.
For screenshot runs `-ApexRacingLine=off|braking|full` overrides the setting
and `-ApexCar=<name>` picks the auto-race car (otherwise the lobby's first).

### Cockpit view (`Race/ApexCockpitRig`, `Race/ApexCockpitLayout`)
The first-person view is the default (settings: Camera tab, "Start in"). The
car meshes are exteriors, so the cockpit is built at runtime by
`AApexCockpitRig`, spawned by the race director and attached to the followed
car: a flat-bottomed steering wheel from engine basic shapes that rolls with
the steering telemetry, a `UApexCockpitDashWidget` on its hub (gear, speed,
RPM lights, lap time; the countdown while on the grid), and mirrors that are
`USceneCaptureComponent2D`s into render targets shown on `UApexMirrorWidget`
faces (drawn flipped, as a mirror is). Captures are refreshed round-robin by
the rig, never "every frame"; the Mirror quality setting picks resolution and
how many refresh per frame. The screens are unlit widget components, which
at the race's 50 klux exposure would be black: `apexsim.cockpit.ScreenNits`
(default 4500) is the tint that keeps them readable.

Where everything sits comes from `ApexCockpit::DeriveLayout` - pure maths
over the mesh's bounds in the car frame (open cockpit vs closed cabin from
the catalog class, else from height), covered by `ApexSim.Cockpit.*` tests.
Per-car overrides live on the car catalog row (`FApexCarCatalogRow::Cockpit`:
eye, wheel, mirrors; zero means derive). The player's seat slide, height and
view pitch, horizon lock, head motion (inertia from speed and yaw-rate
estimates) and look-to-apex are in `UApexSettingsSave`'s camera block, applied
live through `AApexRaceDirector::ApplyCameraSettings`. A virtual mirror strip
at the top of the HUD reuses a fourth capture. `-ApexView=cockpit|chase` picks
the view for a screenshot run regardless of the setting.

### Client startup settings (`settings.yml`)

The few settings a player may need to change *before* the game is usable -
resolution, window mode, vsync, frame limit and the server address - live in a
plain-text `settings.yml` rather than a binary save slot.
`UApexBootSettingsSubsystem` owns it: it reads the file at game-instance
startup, creates it with defaults (seeded from the desktop's own display mode)
when absent, and rewrites it when those values change in-game.

It sits next to the executable: `Game/settings.yml` in a release package,
`game-unreal/settings.yml` (gitignored) in the editor. `FPaths::ProjectDir()` is
`<Release>/Game/ApexSim/` in a packaged build, so its parent is the folder
holding `ApexSim.exe`.

The file wins over the save slots for the values it covers, which is the whole
point of it. `UApexSettingsSubsystem` and `UApexMenuFlowSubsystem` name it as an
`InitializeDependency`, adopt its values on load, and push back on save; a file
created this run is seeded from the slots instead, so an existing install keeps
what it had. Only a flat two-level subset of YAML is parsed (hand-rolled - Unreal
has no YAML reader); an unknown key or unparseable value is logged and skipped
rather than failing the load. Every write regenerates the file wholesale,
comments included, so hand-added comments do not survive a change made in-game.

`build_release.ps1` ships a `Game/settings.sample.yml` beside where the real
file appears: the same shape with the shipped defaults, so a player can read
every setting before the first run creates one. It is a sample and not a live
`settings.yml` on purpose - a shipped file would be adopted over the first-run
display detection, pinning an arbitrary monitor to a 1920x1080 guess. Its text
is a second copy of what `ApexBootSettingsIo::Serialise` writes; the two carry
comments pointing at each other.

### Menu sound (`Audio/`)

The shell's cues are synthesised, not imported: there are no sound assets in
the project. `ApexUiSynth` (`Audio/ApexUiSound.h`) renders each `EApexUiSound`
(Move, Accept, Back, Denied, Adjust, Notice, Error) as a short tone from a
note table, and `UApexUiSoundWave` is a procedural `USoundWave` whose
`CreateSoundGenerator` renders the cue at the device's own sample rate when
the mixer asks for it. `UApexUiAudioSubsystem` (game instance) plays them via
`PlaySound2D` as UI sounds, scaled by `UApexSettingsSave::UiVolume` and
throttled per cue; widgets call `ApexUiAudio::Play(this, EApexUiSound::X)`.

Where the cues come from: a button's `FApexButtonSpec::Sound` (Accept by
default, Back on Back buttons), `UApexNavigableWidget` when a `HandleBack`
consumes the press, the root's `ShowToast`, and the settings sliders and
dropdowns (Adjust). Move is not raised by any widget: the root widget listens
to `FSlateApplication::OnFocusChanging` and plays it for focus moved by the
player, which is a change with cause `Navigation` or one made inside an
`ApexNav::FNavigationScope` (the scope is what separates a host's
`ApexNav::Focus` from the same call made when a screen opens). Scopes wrap
`RouteFromLeaf`, the navigable widget's key/analog handlers and the input
processor's focus recovery; do not add per-widget Move cues.

Volume lives on the settings overlay's Audio tab: master volume drives the
audio device's transient primary volume (`ApplyAudio`), menu-sound volume is
read at play time. `-ApexSettingsTab=4` opens that tab headlessly.
`ApexSim.UI.SoundCues*` automation tests check every cue is short, finite,
click-free and the same length at 44.1k and 48k.

### Car sound (`Audio/ApexEngineSound.h`, `ApexEngineSoundWave.h`)

Engines are synthesised too, per car: `AApexRaceCarActor` owns a
`UApexEngineSoundWave` on an attenuated audio component, feeds it RPM,
throttle and gear from every telemetry frame, and its generator renders
continuously on the audio thread from a lock-free `FApexEngineLiveState`.

The note is **proportional to RPM** (`ApexEngineSynth::NoteHz` — Rpm/60 times a
firing order), not a lerp across the rev range. That matters because neither
idle nor redline is on the wire: a lerp needs a guessed maximum, and any guess
too low pins the pitch part way up a gear while the revs keep climbing — the
F1 idles at 4500 and revs to 15,500, so an 8000rpm guess flattened the top half
of every gear to one note. The observed range (grown from the lowest and
highest readings seen: `ObservedIdleRpm`/`ObservedMaxRpm`) now only drives
timbre and loudness, where a stale value costs brightness rather than the
sweep. Each gear above first also trims the note down a little (`GearTrim`,
about two semitones across a six-speed) so a shift is audible in the instant
before the revs fall. `ApexSim.Audio.*` automation tests cover the monotonic
sweep, the per-gear trim, and that pitch still rises past a stale maximum.

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
