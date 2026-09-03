# ApexSim SimRacing Platform

ApexSim is an open-source simracing platform composed of a high-frequency authoritative server written in Rust and an Unreal Engine 5 client. The codebase is tuned for realistic vehicle physics, low-latency multiplayer, and mod-friendly content pipelines.

## Project Status

This project is in active development. The simulation and the networking underneath it are solid; presentation and gameplay systems are still being filled in.

### Working

**Server**
* Authoritative 240 Hz physics loop — the server decides where every car is, and the client renders what it is told
* 4-wheel vehicle model with per-wheel loads, Pacejka-style tires, suspension, aero and drivetrain; yaw-aware OBB collision
* Deterministic simulation, guarded by a test that asserts bit-identical runs
* Protocol v2: TCP+TLS for auth, lobby and session management; UDP for telemetry out and player input in, bound by a token handshake
* AI drivers, lap timing and lap validation, race classification
* Prometheus metrics plus health and readiness endpoints

<img width="1271" height="743" alt="image" src="https://github.com/user-attachments/assets/ae9ceb40-dd4f-436e-b7fa-2a7d7ebd8c70" />
<img width="1275" height="746" alt="image" src="https://github.com/user-attachments/assets/f169dda7-a6ac-40fc-a815-cd889a85f881" />
<img width="1276" height="748" alt="image" src="https://github.com/user-attachments/assets/f7438b33-79e7-456b-b316-605f2ab38e54" />

**Content**
* 26 circuits with exact measured centerline, per-side track width, banking, surface type and **elevation** (Spa spans ~90 m of it)
* 4 cars with physics definitions and 3D models
* Both shared verbatim between the server, the track editor and the client

**Unreal client (`game-unreal/`)**
* Full menu shell — connect, session browser and create, car and track selection, lobby, session results — built as C++ widget trees rather than widget blueprints, so layout is reviewable in a diff
* Race view: a car per roster entry driven from telemetry, with the circuit streamed in as a level instance; cockpit and chase cameras
* Driving via Enhanced Input, with actions and bindings defined in C++
* Race HUD: position, gaps, standings, live delta, sector times, minimap, pedal and engine telemetry
* Pause menu and a settings overlay covering gameplay, graphics and rebindable controls
* Local profile and settings save slots (the server has no account model, so anything "yours" lives on your machine)

**Tooling**
* A Rust + Bevy track editor that authors the 3D scene on top of a logical track, and bakes it for Unreal

### Missing

* **No audio at all** in the Unreal client — no engine, tire or collision sound
* No client-side prediction; cars are pure telemetry puppets, smoothed by interpolation
* Driving aids (traction control, ABS) and AI skill cannot be set per session from the client — the wire protocol has no fields for them, so those settings are stored but inert
* No racing-line overlay and no mirrors
* No trackside environment art beyond the generated track meshes
* No server-side player accounts or persistence

## Architecture Overview

1. **Rust server (`server/`)** — runs the authoritative 240 Hz simulation loop, manages sessions, performs collision-aware physics, and streams telemetry over UDP while handling lobby and session traffic over TCP+TLS. See [server/README.md](server/README.md) for configuration, build and operations detail.
2. **Unreal client (`game-unreal/`)** — the player experience: menus, HUD, driving view, and the networking layer that talks to the backend. Three C++ modules: `ApexSimNet` (protocol and transport), `ApexSim` (game and UI), `ApexTrackEditor` (editor-only import commandlet).
3. **Track editor (`track-editor/`)** — a Rust + Bevy tool that turns a logical track into a 3D scene, and bakes that scene into buffers Unreal can build a level from.

This separation keeps critical simulation logic isolated from presentation while letting each component evolve independently.

> A Godot client (`game-godot/`) and a CLI test client (`game-cli/`) existed earlier in the project's history. Both were removed in favour of the Unreal client; integration testing now runs against an in-process server from the server crate's own test suite.

### Serialization

The client and server communicate using [MessagePack](https://msgpack.org/). All networked data structures are defined in Rust with `serde` and `rmp_serde`. High-frequency telemetry uses a compact positional encoding with session-scoped car indices (~60% smaller on the wire than the named encoding) and flows over UDP after a token handshake; reliable lobby and session traffic stays on TCP+TLS. This prioritizes performance and low bandwidth overhead, which is critical for real-time simulation.

### Track pipeline

A circuit reaches the client in two generated steps. Both outputs are regenerated wholesale, and neither should be hand-edited:

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
                                                         # -> content/tracks/export/*.uescene.json
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject \
    -run=ApexTrackImport -all                            # -> game-unreal/Content/Tracks/<Track>/L_<Track>.umap
```

Or run both steps for every circuit at once, which also locates the engine
install itself:

```powershell
./scripts/build_track_levels.ps1                 # all tracks
./scripts/build_track_levels.ps1 -Track Monza,Spa -Build   # two tracks, editor target rebuilt first
```

The exporter resolves the editor's `.ats` scene against the YAML centerline and bakes triangles, because Unreal cannot read YAML; the commandlet turns those buffers into static meshes, materials and one level per track. See [track-editor/TRACK_EDITOR.md](track-editor/TRACK_EDITOR.md) §5 for the format and the coordinate conventions.

## Performance

The simulation loop is, to put it modestly, not the bottleneck. Measured with the checked-in Criterion benchmarks (`cargo bench --bench physics_tick`, release build, single core, Monza with its full measured centerline):

| What | Cost | What that means |
|---|---|---|
| One car, one full physics step (per-wheel tire model, suspension, aero, drivetrain) | **~835 ns** | ~1.2 million car-steps per second per core |
| Nearest-centerline track query (windowed, cached) | **~106 ns** | effectively free |
| Complete 8-car session tick — physics, AI drivers, collision detection, lap validation | **~7.8 µs** | ~130,000 full session ticks per second |

At the default 240 Hz tick rate, simulating a full 8-car session consumes about **0.2% of the 4.17 ms tick budget**. The physics engine could sustain a tick rate in the six figures; the server caps `tick_rate_hz` at 1000 purely because async timer granularity — not simulation cost — becomes the limiting factor beyond that. In other words: the sim spends 99.8% of its time waiting politely for the next tick, and your network connection will give out long before the physics does.

These numbers held (within noise) through the move from a synthesized slip model to the current per-wheel torque-balance tire model with combined-slip friction ellipse, ABS/TC driver aids, and hybrid powertrain support — realism upgrades that cost nanoseconds, not milliseconds.

## Repository Layout

```
apexsim/
├── content/        # Car and track definitions, shared by server, editor and client
├── docs/           # Design and implementation notes
├── game-unreal/    # Unreal Engine 5 client
├── scripts/        # Track pipeline runner and Python content helpers
├── server/         # Rust backend (source, config, docs)
├── track-editor/   # Rust + Bevy circuit scene authoring tool
├── README.md       # This overview
└── LICENSE         # Project license
```

### Directory Highlights

- [content/](content): Authoring-ready data. Cars are `cars/<name>/car.toml`; tracks are `tracks/real/*.yaml` (the logical circuit the server simulates) alongside `.ats` scene sidecars (the 3D dressing, read only by the editor and the Unreal importer).
- [game-unreal/](game-unreal): Unreal Engine 5 client. Source lives in `Source/ApexSim`, `Source/ApexSimNet` and `Source/ApexTrackEditor`.
- [scripts/](scripts): Build and content helpers — `build_track_levels.ps1` runs the whole track pipeline; the Python scripts generate track preview images and racing lines.
- [server/](server): Full Rust crate with source, configuration files and supporting docs for the backend runtime.
- [track-editor/](track-editor): The circuit scene editor and the `ats-export` baker.

## Getting Started

### 1. Run the server

```bash
cd server
cargo run                 # uses server.toml
```

The server listens on TCP 9000, UDP 9001 and HTTP 9002 (`/health`, `/ready`, `/metrics`). See [server/README.md](server/README.md) for configuration and TLS setup — TLS is fail-closed by default, with a development opt-out in `server.toml`.

### 2. Build the track levels

The Unreal content directory is generated, not committed, so a fresh clone has
no circuits to drive on — the race view would load an empty world. Bake and
import all 26 of them once, before the first run:

```powershell
./scripts/build_track_levels.ps1 -Build
```

That runs the whole [track pipeline](#track-pipeline): it compiles the
`ApexSimEditor` target (`-Build`, needed the first time and after any C++
change), bakes every circuit with `ats-export`, and imports the result into
`game-unreal/Content/Tracks`. The engine install is located from the
`.uproject`, or pass `-EngineRoot <path>`. Expect a few minutes for the full
set; `-Track Monza,Spa` limits it to a couple of circuits, and re-running it
after editing a track picks up the changes.

### 3. Run the client

Requires Unreal Engine 5.8. Open `game-unreal/ApexSim.uproject` and press Play, or build and launch from the command line:

```bash
"$UE/Engine/Build/BatchFiles/Build.bat" ApexSimEditor Win64 Development \
    -Project="<repo>/game-unreal/ApexSim.uproject"
"$UE/Engine/Binaries/Win64/UnrealEditor.exe" "<repo>/game-unreal/ApexSim.uproject" -game
```

The client auto-connects to `127.0.0.1:9000`. Handy switches for unattended runs: `-ApexAutoRace` (create and start a session immediately), `-ApexAiCount=N`, `-ApexLaps=N`, `-ApexStartScreen=N`, `-ApexOpenPause=N` / `-ApexOpenSettings=N`, and `-ApexScreenshotAfter=N`, which drops a screenshot in `Saved/Screenshots/`.

### 4. Build a release package

To produce something other people can download and run:

```powershell
./scripts/build_release.ps1 -Zip
```

That runs the whole pipeline � `cargo build --release`, the track bake and
import, the track catalog sync, and the client package � and assembles
`artifacts/release/ApexSim-<version>-Win64/` (plus a zip to attach to a GitHub
release). The package holds the packaged client in `Game/`, the server with its
config and content in `Server/`, and a `Play.bat` that starts both. It is
gitignored, like everything under `artifacts/`.

The run aborts up front if the car or track data is missing, so a broken clone
fails in seconds rather than twenty minutes in. Each stage has a `-Skip*`
switch (`-SkipServer`, `-SkipTracks`, `-SkipCatalog`, `-SkipClient`) for
reusing what is already built.

## Windows Setup

Install the development prerequisites from an elevated PowerShell session. Approve any Windows UAC prompts shown by the installers.

```powershell
winget install --id Rustlang.Rustup --exact --source winget --include-unknown --accept-package-agreements --accept-source-agreements --silent
winget install --id Kitware.CMake --exact --source winget --accept-package-agreements --accept-source-agreements --silent
winget install --id NASM.NASM --exact --source winget --accept-package-agreements --accept-source-agreements --silent
winget install --id Python.Python.3.12 --exact --source winget --accept-package-agreements --accept-source-agreements --silent
```

Unreal Engine 5.8 is installed separately through the Epic Games Launcher. Python is only needed for the helpers in `scripts/`.

Restart VS Code after installation so new integrated terminals receive the updated `PATH`. Then verify the toolchain and build the Rust server:

```powershell
cargo --version
rustc --version
cmake --version

Set-Location server
cargo build
```

There is no `package.json` anywhere in the repository, so there are no npm dependencies to install.

## Contributing

Contributions are welcome across gameplay programming, engine tooling, networking, UI, and content creation. Please coordinate significant changes via issues or discussion threads, and keep server and client documentation up to date when workflows change.

Before submitting a pull request:

```bash
cd server
cargo fmt && cargo clippy --all-targets   # CI enforces fmt --check and treats warnings as errors
cargo test
```

Wire-format changes must be made on both sides at once — `server/src/network.rs` and `game-unreal/Source/ApexSimNet` — and the client's codec tests pin every message against a golden byte blob, so a mismatch fails loudly rather than silently decoding to an empty list.

## License

This project is licensed under the [MIT License](LICENSE).

---
