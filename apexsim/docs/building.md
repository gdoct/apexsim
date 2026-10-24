# Building and testing

How to build each part of ApexSim, fill a fresh checkout with the generated
content the game needs, play from the editor build, and package a release.
Running a server for other people is in [server/operations.md](server/operations.md).

## Layout

The repository holds three roots, laid out as three repositories that sit
side by side:

| Root | Holds |
|---|---|
| `apexsim/` (code) | `server/`, the Rust crates: `server/sim/` (the simulation), `server/protocol/` (the wire, the spectator stream, the render), `server/server/` (the server) and `server/ffi/` (the physics library the client links); `game-unreal/`, `hud/`, `launcher/`, `deploy/`, `site/`, `scripts/` (build, release, site, promo and verification scripts), `docs/`, `showcase.yml`, `server.toml`; its artefacts in `build/` (`benchmark/`, `showcase/` and `guide/` from `apexsim-replay` run by hand...), `out/`, `artifacts/`, `records/`, `replays/` |
| `apexsim-content/` (content) | `cars/`, `tracks/`, `props/`, `textures/`, `wheels/`, `artwork/`; `track-editor/` (track-core, the `ats-*` tools, the Bevy editor); `tools/` (the Python pipeline, the AC importers, `generators/` for the Blender-built cars, props and wheels, `lib/` for the shared PowerShell); `docs/`; the generated `build/tracks/` (exports, previews, `track_catalog.json`) and `.cache/` (OSM, DEM, previews, import caches) |
| `apexsim-lib/` (formats) | `formats/`, the `apexsim-formats` crate: the track YAML (`TrackFile`), the ground, curbs, walls, road mesh and pit sidecars, `content_crc` and the format levels |

`apexsim-lib` depends on nothing, the content depends on `apexsim-lib`, and
the code depends on both. The code reaches the content as the sibling
`../apexsim-content` from the code root (`../../apexsim-content` from
`game-unreal/`, `../../../apexsim-content` from a crate in `server/`); the simulation, the server and
track-core depend on `apexsim-lib/formats` by path. Nothing under `apexsim-content/` or
`apexsim-lib/` reads, builds or links the code; the one exception is a test,
the AC car importer's reading `server/sim/src/brakes.rs`.

Where each kind of tool runs from:

- **Code scripts** (`scripts/*.ps1`, `scripts/*.py`) run from the code root,
  `apexsim/`. They find the content root themselves and write their own
  artefacts under the code root.
- **Content tools** run from the content root, `apexsim-content/`: the
  `ats-*` tools and the editor resolve `tracks/...` and `build/tracks`
  against the working directory
  (`cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- ...`),
  and the Python tools are `python tools/<tool>.py`. `bake.ps1` there runs
  the content's whole bake (ground PNGs, dress, export, previews) with cargo
  and Python only; `scripts/build_track_levels.ps1` runs it and adds the
  code's stages.
- **Cargo** for the simulation runs in `apexsim/server/sim`, for the wire and the
  stream in `apexsim/server/protocol`, for the server in
  `apexsim/server/server` (and `apexsim/server/ffi`), for the formats in
  `apexsim-lib/formats`.

Paths in this doc are relative to the code root; a path that starts with
`apexsim-content/` is in the content root.

## Prerequisites

- Rust (stable) with `rustfmt` and `clippy`.
- Python 3 with numpy, Pillow and PyYAML (track pipeline, ground textures).
- Unreal Engine 5.8 (`EngineAssociation` in `game-unreal/ApexSim.uproject`)
  and Visual Studio's C++ toolchain, for the client and the commandlets.
- PowerShell for the build scripts (Windows; 5.1 works).

## Simulation (Rust, `server/sim/`)

`apexsim-sim` is the deterministic simulation: the car, the track and its
sidecars, the race and the session tick, with no network, TLS, clock or
RNG ([architecture.md](architecture.md#simulation-and-runtime-modules)).
The server re-exports its modules under the same paths
(`apexsim_server::physics`), and the client's physics library
(`server/ffi`) links it alone. A test that needs only the simulation lives
here, `determinism_test` among them; one that starts a server, speaks the
wire or reads the server's config lives in `server/server/`.

```bash
cd server/sim
cargo fmt --check && cargo clippy --all-targets -- -D warnings
CARGO_PROFILE_TEST_OPT_LEVEL=2 cargo test --no-fail-fast   # unit and integration tests, as CI runs them
cargo test --lib physics::                # one module's unit tests
cargo test --test all fuel_test::         # one integration file
```

The integration tests are one binary, `tests/all` (`main.rs` declares each
file as a module), so a change links one test executable, not one per file.
A new file is added there with a `mod` line; `--test all <file>::<name>`
picks a file or a test. The tests read the content as
`../../../apexsim-content` from `server/sim/`, as the server's do from `server/server/`. The crate has its own `Cargo.lock`, started
from the server's: keep the shared dependencies on the server's versions.

## Protocol (Rust, `server/protocol/`)

`apexsim-protocol` is what the server and its viewers exchange, apart from
the sockets: the wire messages (`network`, `session_wire`) and their
`*_wire_format` golden tests, the spectator stream and its file
(`spectator`, `session_stream`), the lap records and ghosts (`records`), the
setup sheet, and `render`, the seeded race rendered into a stream (what
`apexsim-replay render` writes). It depends on the simulation alone; the
server re-exports every module under the same path
(`apexsim_server::network`) and the physics library links it to write the
same showcase bytes as the server.

```bash
cd server/protocol
cargo fmt --check && cargo clippy --all-targets -- -D warnings && cargo test
cargo test spectator_wire_format -- --nocapture   # golden bytes for the client
```

The crate has its own `Cargo.lock`, started from the server's: keep the
shared dependencies on the server's versions.

## Server (Rust, `server/server/`)

```bash
cd server/server
cargo build                    # debug
cargo build --release          # release: target/release/apexsim-server(.exe), apexsim-replay(.exe)
cargo run -- --config path.toml --log-level debug   # default ./server.toml
cargo fmt && cargo clippy --all-targets
cargo bench                    # Criterion, benches/physics_tick.rs
```

```bash
cargo test                                         # the server's unit tests + integration
cargo test -- --ignored                            # long-running stress tests and probes
cargo test --test all integration_test::<name> -- --nocapture
RUST_LOG=debug cargo test <name>                   # with logging
```

- The integration tests are one binary, `tests/all`, as the simulation's
  are; a file uses the helpers with `use crate::common;`.
- Integration tests start the server in-process on ephemeral ports through
  `apexsim_server::server::run_server` (`tests/all/common/mod.rs`:
  `start_test_server`, `start_test_server_with_tick_rate`,
  `start_test_server_with_records`). No server needs to be running. Test
  servers skip AC-imported tracks in a debug build
  (`[content] skip_imported_tracks`) and disable the admin dashboard unless
  they test it.
- `cargo run --example host_race -- 127.0.0.1:9000 5 5 600` joins a running
  server as a host, starts a five-lap race with five AI and stays 600 s, so
  something can be watched (`-ApexWatchSession`, a stream check, a screenshot
  run). Point the game at a scratch server with `-ApexSettingsDir=`
  ([game/client.md](game/client.md)) instead of the player's `settings.yml`.
- `server/sim/tests/all/determinism_test.rs` asserts bit-identical runs on both road
  backends; see [architecture.md](architecture.md#determinism) for the rules
  it enforces.
- `tests/all/physics_property_tests.rs` holds the `proptest` invariants
  (physics bounds, serialization round trips).
- The `#[ignore]`d harnesses (AI survey, grip probe, pit surveys, tyre probe,
  imported content checks) are documented with the feature they measure:
  [server/ai.md](server/ai.md), [server/vehicle-physics.md](server/vehicle-physics.md),
  [server/pit-lane.md](server/pit-lane.md), [apexsim-content/docs/ac-import.md](../../apexsim-content/docs/ac-import.md).
- Benchmarks (`physics_step_monza`, `session_tick_8cars_monza`, ...): the
  `_mesh` ones are skipped until `Monzarella.road.msgpack` is baked. A physics
  step is measured on a fresh clone of a car rolling down the start straight
  and a session tick on a field rebuilt every 30 s of sim, so neither is
  timing a car the bench itself wore out (a car that is out returns from
  `update_car_3d` at once).

`apexsim/.cargo/config.toml` holds the build settings for every crate under
`apexsim/`: on Windows it links with `rust-lld` (shipped with the Rust
toolchain) instead of MSVC's `link.exe`, and third-party crates keep line
tables only in a debug build, so backtraces keep file and line while the
linker writes less. Cargo reads it when it runs from `apexsim/` or below;
`--manifest-path` from elsewhere skips it.

**A running server locks its exe on Windows.** `cargo build` then fails to
replace `target/*/apexsim-server.exe`; build and test into a scratch
`CARGO_TARGET_DIR` instead of stopping someone's server.

### The physics library (`server/ffi`)

`apexsim-physics` is the simulation's car step behind a C API for the
client's prediction of its own car ([proposals/client-prediction.md](proposals/client-prediction.md)),
a whole seeded race nobody drives (`apx_race_*`: `seeded_race`, the
setup and tick `apexsim-replay render` runs; roadmap #24), a showcase
rendered into a stream file (`apx_showcase_*`, the protocol crate's
`render` and `showcase_list`), a track guide (`apx_guide_build`,
`track_guide`) and a watched hotlap (`apx_watch_*`, `hotlap_watch`): a small crate of its own that depends on the
simulation and protocol crates alone, not on the server, so the server's
build is untouched and the library carries no network or TLS code.
It builds a static library and an rlib (for its tests); the header is the
hand-written `server/ffi/apexsim_physics.h`.

```bash
cd server/ffi
cargo test                     # the trace and rewind tests, the seeded race against the render, the showcase against the tool, the header check
cargo test --release --test showcase -- --ignored   # the real list's Zandervoort showcase against build/showcase (apexsim-replay showcase --name Zandervoort.gt3.day first)
cargo test --release --test guide -- --ignored      # Zandervoort's GT3 guide against build/guide (apexsim-replay guide on its YAML first)
cargo test --test watch                             # a watched hotlap: catch-up, frames, kept level, seeded
cargo build --release          # target/release/apexsim_physics.lib (Windows x64 MSVC)
cargo rustc --release --lib --crate-type staticlib -- --print native-static-libs
```

On Windows x64 (`x86_64-pc-windows-msvc`, the default host toolchain) the
release `.lib` is about 36 MB (the simulation's objects, its dependencies'
and the standard library's; the linker keeps only what the car step
reaches) and links against the standard library's `kernel32 ntdll userenv
ws2_32 dbghelp` and the dynamic CRT (`/MD`, as an Unreal module builds); the last command above prints the list for the
toolchain at hand. A C or C++ program includes `apexsim_physics.h` (it
compiles as C89 and C++17) and links the `.lib` and those libraries. The
crate has its own `Cargo.lock`, started from the server's: keep the shared
dependencies on the server's versions.

**In the client.** `game-unreal/Source/ThirdParty/ApexPhysics` is an
external module (`ApexPhysics.Build.cs`) that the `ApexSim` module depends
on: it puts `server/ffi` on the include path and, when
`lib/Win64/apexsim_physics.lib` is there, links it with the system libraries
above and defines `WITH_APEX_PHYSICS=1`. Without the library the client
builds and runs as before with `WITH_APEX_PHYSICS=0`, and prediction says it
is unavailable ([game/client.md](game/client.md#client-prediction-raceapexpredictionsubsystemh)).

```powershell
./scripts/build_physics_lib.ps1     # cargo build --release in server/ffi, copied into the module
```

The library is built on demand and never checked in (`*.lib` is
gitignored), like the server exe and the track sidecars: it is generated
from the simulation's source and only right for the source beside it, it is
36 MB, and cargo rebuilds it in seconds when nothing changed. LFS holds only
authored content. `initialize_content.ps1` builds it when it is missing,
`play_editor.ps1 -Build` and `build_game_standalone.ps1` every time, before
the client build. The script honours `CARGO_TARGET_DIR`, and touches
`ApexPhysics.Build.cs` when the library first appears: UBT reads a module's
rules again only when they change, so a client built before the library
existed would not link it otherwise.

### CI

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request; a newer push to the same branch or pull request cancels the run
before it. Every job checks out the whole repository (the code jobs read the
content), and none is filtered by path.

- `sim` (Ubuntu, in `server/sim/`): `cargo fmt --check`, `cargo clippy
  --all-targets -- -D warnings`, then `cargo test --no-fail-fast` at
  opt-level 2, as the server's.
- `server` (Ubuntu): `cargo fmt --check`, `cargo clippy --all-targets -- -D
  warnings` (which compiles every target, so there is no build step), then
  `cargo test --no-fail-fast` with the test profile at opt-level 2: the
  tests race whole sessions, and optimised they take a couple of minutes
  instead of twelve. Then fmt, clippy and test for `server/protocol/` and for
  `server/ffi`. CI has no
  baked sidecars (they are gitignored): a test that drives a circuit names
  the sidecars it needs (`TrackConfig::has_sidecars(&[Sidecar::Pit], ..)`,
  `Sidecar::BAKED_ROAD` for one judged on the curbs, walls and verge) and
  skips that part with a note without them, so the same test runs at full
  strength on a baked tree. Check a test change on a copy without them
  (`git archive HEAD` of the code and `apexsim-content/{tracks,cars}` in a
  scratch folder, with its own `CARGO_TARGET_DIR`).
- `track-editor` ("Track pipeline", from `apexsim-content/`):
  `tools/check_direction.py`, which fails on any path from the content
  into the code beyond the one allowed test-only edge (a path into
  `apexsim-lib/` is fine); `cargo fmt --check` over the workspace; clippy
  `-D warnings` and `cargo test` of `track-core` only. The Bevy editor is
  not compiled in CI (over twenty minutes on a runner).
- `formats`: fmt, clippy `--all-targets -D warnings` and `cargo test` in
  `apexsim-lib/formats`.
- `site` rebuilds the marketing page and fails when `docs/` is behind
  ([marketing-site.md](marketing-site.md)).
- `client` only checks the `.uproject` and the target/module files exist
  (`ApexPhysics.Build.cs` among them). The Unreal code is not compiled in
  CI, and nothing builds the client's copy of the physics library there.

`.github/workflows/dependabot-auto-merge.yml` runs when CI finishes on a
Dependabot pull request: if CI passed on the pull request's current commit
and the title's version change is a patch or minor one (below 1.0, a patch
only), it approves and squash-merges it. It builds nothing itself, and `main`
has no required checks, so this ordering is what keeps an untested update
out.

## Formats (`apexsim-lib/formats`)

The `apexsim-formats` crate models every file the content writes and the
code reads, once: the track YAML (`track::TrackFile` and its nodes,
checkpoints, spawn points, racing line, DRS zones and metadata), the five
sidecars `ats-export` bakes beside it (`ground`, `curbs`, `walls`,
`road_mesh`, `pit`: the serialized structs, their `*_VERSION` constants and
`sidecar_path`), `content_crc` and the format levels (`TRACK_YAML_LEVEL`,
`SIDECARS_LEVEL`). Its only dependency is serde. The simulation re-exports the
types under its own paths (`track_loader::TrackFileFormat`,
`data::TrackMetadata`, `ground::GroundHeightfield`, `pit::PitBox`...) and
keeps its queries on them in traits (`GroundQuery`, `CurbQuery`,
`WallSegmentKind`) and runtime types (`RoadMesh`, `Walls`, `PitLane`);
track-core re-exports them from `track_data`, `terrain`, `road_mesh` and
`ue_export`.

```bash
cd apexsim-lib/formats
cargo fmt --check && cargo clippy --all-targets -- -D warnings
cargo test                                # YAML rewrite fixtures, sidecar golden bytes, content_crc
cargo test --test round_trip -- --nocapture   # print the sidecar goldens
```

The serde attributes are the files' bytes: a tool that rewrites a YAML
writes it through `TrackFile`, so a changed `skip_serializing_if` changes
every rewritten track's content checksum, and a changed sidecar struct fails
the goldens. A format change lands in the crate with the simulation's and
track-core's use of it in one commit. The simulation's
`server/sim/tests/all/track_file_compat_test.rs` holds every real track to a rewrite
loading to the same physical track.

## Track editor and pipeline (Rust, `apexsim-content/track-editor/`)

A workspace of `track-core` (`core/`: the pipeline, every `ats-*` tool, no
Bevy) and `track-editor` (`src/`: the Bevy viewport and MCP server). Both are
default members.

```bash
cd apexsim-content/track-editor
cargo run                                 # the editor
cargo test                                # both crates; the whole-calendar bakes are #[ignore]d
cargo test -p track-core                  # pipeline only, no Bevy build
cargo test -p track-core --test ue_export -- --ignored   # bake every circuit (~20 min)
```

Run `cargo test` for the whole workspace, not `--lib`: the integration tests
under `core/tests/` groom every real circuit. The `ats-*` tools resolve
`tracks/...` and `build/tracks` relative to the working directory, so run
them **from the content root** with `--manifest-path`:

```bash
cd apexsim-content
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
```

The pipeline's order and tools are in [apexsim-content/docs/track-pipeline.md](../../apexsim-content/docs/track-pipeline.md).

## Unreal client (`game-unreal/`)

### Finding the engine

Every script resolves the engine through `Resolve-ApexEngineRoot` in
`scripts/lib/ApexEngine.ps1`, in this order: `-EngineRoot`, then `$env:UE`,
`UE_ROOT`, `UE5_ROOT`, then the registry entry named by the `.uproject`'s
`EngineAssociation`, then the Epic launcher's default install folders. A
candidate wins only if it holds the tool the script is about to run.

### Building

```powershell
$UE = "C:\Program Files\Epic Games\UE_5.8"
& "$UE\Engine\Build\BatchFiles\Build.bat" ApexSimEditor Win64 Development `
    -Project="$PWD\game-unreal\ApexSim.uproject" -WaitMutex
```

Or build `ApexSimEditor` / Development Editor from the generated Visual
Studio solution. The commandlets and `play_editor.ps1` run on this target.
Build the physics library first (`./scripts/build_physics_lib.ps1`, above)
for a client that can predict its own car; without it the build is the
same and prediction is unavailable.
Close the editor and any running game first (they hold the binaries). After
adding a `.cpp`, build once with `-DisableAdaptiveUnity`: an
anonymous-namespace clash with a neighbour only fails once a unity build
batches the files, which can be after commit.

### Automation tests

`ApexSim.*` tests run headless on the editor build:

```powershell
& "$UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\game-unreal\ApexSim.uproject" `
    -ExecCmds="Automation RunTests ApexSim; Quit" -unattended -nullrhi -nosplash -log
```

Narrow the filter (`ApexSim.Net`, `ApexSim.Hud.Data`...) to run a group.
`ApexSim.UI.PadWalk` needs the running game (`-game -RenderOffscreen`); see
[game/client.md](game/client.md#pad-walk-uiapexpadwalkh-apexsimuipadwalk).

## Fresh checkout: generated content

`game-unreal/Content/` is gitignored except the menu (`UI/`, `Maps/L_Menu`,
`Blueprints/`), the two catalog tables in `Data/` and `Splash/`. Props, ground
textures and the track and car parent materials are generated by commandlets;
the tracks by the Rust bake. Until they exist the cars draw in flat colours
and races run in an empty world. One script builds whatever is missing (the
editor closed; Rust, Python and the engine installed):

```powershell
./scripts/initialize_content.ps1            # only what is missing
./scripts/initialize_content.ps1 -DryRun    # report what is missing and the plan
./scripts/initialize_content.ps1 -Force     # redo every stage
./scripts/initialize_content.ps1 -SkipBuild # use the editor binaries already there
```

It checks each output on disk and runs only the stages that lack one, so it
is safe on an existing checkout. Stages, in order:

0. The physics library (`Build-ApexPhysicsLib`, as
   `build_physics_lib.ps1`), when the client module has none; it makes the
   editor build run too, which links it.
1. `Build.bat ApexSimEditor Win64 Development` (incremental; the commandlets
   live in it).
2. `cargo build --release` in `server/server/`, if there is no server exe.
3. `apexsim-content/bake.ps1 -Stage ground` (the ground PNGs), then
   `-run=ApexGroundTexImport` ->
   `/Game/Ground`.
4. `-run=ApexMaterialBake` -> the track parents under
   `/Game/Materials/Track` and the car parents under `/Game/Materials/Car`,
   when one is missing or stage 3 ran (`-force` with `-Force`).
5. `-run=ApexPropImport -all` when any kit GLB has no mesh.
6. `build_track_levels.ps1 -Release -SkipMaterials` for every
   circuit missing its export, its preview or a server sidecar (ground,
   curbs, walls, road, and pit when its `.ats` lays a pit lane). No circuit is
   rebaked because stage 3 or 5 ran: the game dresses a circuit with what
   `/Game` holds when it builds it.

Showcases and track guides have no stage: the game makes its own into
`game-unreal/Saved/ApexSim/` ([game/spectator.md](game/spectator.md),
[track guide](../../apexsim-content/docs/track-guide.md)).

It also checks every file each `car.toml` names is on disk; cars need no
stage (the game builds them at runtime). AC-imported tracks are never baked
here: one whose export is missing is reported with its rebuild command. The
data refreshes (`osm_layout.py`, `dem_fetch.py`, `ats-smooth`...) are not
stages: their outputs are checked in. Stage 6 is the long one on a fresh
clone and needs no engine.

## Play from the editor build

`scripts/play_editor.ps1` runs the game as `UnrealEditor.exe -game` on the
editor binaries: no cook, so a C++ change is one incremental build from
playable. If `game-unreal/settings.yml` points at this machine and nothing is
listening on that port, it builds the release server (`cargo build
--release`) and starts it in its own minimized window; it warns when the
running server or the editor build is older than the source.

| Switch | Effect |
|---|---|
| `-Build` | Build the physics library, then `ApexSimEditor`, first |
| `-NoServer` | Start no local server |
| `-RestartServer` | Replace the server on the client's port with a fresh build |
| `-Log` | Open the log console beside the game |
| `-DryRun` | Report what would be built, started and launched |
| `-Interactive` | Ask instead of warning; keep the window open on an error |
| `-CreateShortcut` | Put a desktop shortcut (runs with `-Interactive`) and exit |
| anything else | Passed to the game, e.g. `-ApexView=chase` |

The game's own command-line switches are in [game/client.md](game/client.md).

## Standalone client

`scripts/build_game_standalone.ps1` builds the physics library, then runs
UAT BuildCookRun into
`artifacts/ApexSim-Win64` (`-OutputDirectory`, `-Configuration`
Development by default, `-Clean`, `-ExtraUatArgs`), then copies beside
`ApexSim.exe` what the game reads as files: `Tracks\` (exports and previews
from `apexsim-content/build/tracks`), `Cars\` and `Wheels\` (from
`apexsim-content/cars` and `wheels`), `Hud\default` (from `hud/default`,
with an empty `Hud\custom`) and `showcase.yml`. No showcase or track guide
is copied: the game makes its own on the tracks and cars of
`..\Server\content`, which only a release has (`-ApexContentDir=` for a
package outside one). `-SkipTracks`, `-SkipCars`, `-SkipHud` leave a part
out; `-IncludeCustomTracks` / `-IncludeCustomCars` add the player's own
content (off by default).

Packaging relies on `bCookAll=True` in `game-unreal/Config/DefaultGame.ini`:
the catalog tables, materials, prop kit and ground sets are found by path at
runtime. `/Game/Tracks` and `/Game/Cars` are in `DirectoriesToNeverCook`
(editor-only inspection assets).

## Release package

`scripts/build_release.ps1` runs everything front to back and assembles
`artifacts/release/ApexSim-<Version>-Win64/` (version from `ProjectVersion`
in `DefaultGame.ini` unless `-Version`). A release ships the **starter
set**, `apexsim-content/starter.yml` (one car per class and three circuits):
only those cars and circuits are installed. The rest of
`apexsim-content`'s default cars and circuits are baked too and ship beside
them, not installed, as one loose pack, `content\content.acp`
([../../apexsim-content/docs/packs.md](../../apexsim-content/docs/packs.md#loose-packs-acp)):
`cars/<folder>/` as `Game\Cars` ships a car, `tracks/<Stem>/` with the
server's files and the export and preview in `tracks/<Stem>/game/`. The
launcher's Manage content > Install pack... puts the ones the player ticks
in the `custom/` folders. A pack carries no class wheels, so `Game\Wheels` holds the pack
cars' wheels as well. `-AllContent` installs every car and circuit and makes
no pack; `custom/` content is never packed. No showcase or track guide ships: the game renders the
showcases of `Game\showcase.yml` and makes a circuit's guide when it is
opened, both on `Server\content`'s tracks and cars, and the server renders
`Server\showcase.yml` into `Server\showcase\` at its first start
([game/spectator.md](game/spectator.md)), so every car in them is
installed. An entry of a class no shipped car covers fails at once and is
left out.

1. preflight: the car and track data exist, every track YAML has a
   `track_id`, every file a `car.toml` names is there
2. server: `cargo build --release`
3. props: `ApexPropImport`
4. tracks: `build_track_levels.ps1` (the content's `bake.ps1`: dress,
   export, previews; then materials)
5. client: `build_game_standalone.ps1` (Shipping by default), archived
   straight into the release folder
6. assemble: `Server\` (`apexsim-server.exe`, `apexsim-replay.exe`, the
   code root's `server.toml`, `server/server/CMDLINE.md` as `server.md`, the content
   the server reads, `showcase.yml`), `Game\` (with `Tracks`, `Cars`, `Wheels`,
   `Hud`, `showcase.yml` and `settings.sample.yml`), `launcher.exe`,
   `Tools/importer/`, `README.txt`, `LICENSE`, `release.json`, `unblock.txt`
7. pack: `content\content.acp`, staged in `<OutputDirectory>\<package>.content`
   and zipped
8. `-Zip`: compress it beside the folder

| Switch | Effect |
|---|---|
| `-SkipServer`, `-SkipLauncher`, `-SkipProps`, `-SkipTracks`, `-SkipClient` | Reuse that stage's existing output |
| `-ClientArtifactDirectory` | Where `-SkipClient` finds a client build |
| `-BuildEditor` | Build `ApexSimEditor` before the track stage |
| `-Configuration`, `-EngineRoot`, `-OutputDirectory`, `-Version` | As named |
| `-AllContent` | Install every car and circuit, not only the starter set, and make no `content.acp` |
| `-IncludeCustomTracks`, `-IncludeCustomCars` | Ship `custom/` content |

A skipped stage must still find the output it would have produced (a
`-SkipTracks` run checks every export and the materials exist). The run does
not import ground textures: run `initialize_content.ps1` first on a new machine.

A zip downloaded through a browser marks every extracted file with a
`Zone.Identifier` stream, and SmartScreen then stops `ApexSim.exe` or the
server. The assemble stage lists every `.exe` and `.dll` of the package in
`unblock.txt` (one root-relative path per line), and `launcher.exe` deletes
the streams of those files on every start (`launcher/unblock.cpp`, as
`Unblock-File` would), also when `launcher.show` is false. Nothing else is
touched: a car, track or HUD the player downloads later keeps its mark, a
package without `unblock.txt` unblocks nothing, and a source checkout never
runs it.

## Reclaiming disk space

A working tree grows to well over 100 GB: dev-profile cargo targets
(`opt-level = 3` dependencies, full debug info) run 15 to 90 GB each, and every
scratch `CARGO_TARGET_DIR` and agent session leaves another one behind.
`scripts/clean_workspace.ps1` removes them. It is a dry run until `-Apply`.

| Run | Removes | Costs |
|---|---|---|
| default | scratch targets (`<drive>:\apexsim-cargo-target*`, `target-scratch*`) and `%TEMP%\claude\*apexsim*` sessions older than `-KeepDays` (2); cargo `incremental/` caches; stale `%TEMP%` (`proc-macro-srv*`, `apex-target`); Unreal `Saved` crashes, old logs, screenshots older than 7 days; `__pycache__`; empty unregistered `.claude/worktrees` folders | the next build recreates the incremental cache |
| `-Deep` | `server/server/target`, `apexsim-content/track-editor/target`, `game-unreal/Intermediate/Build`, `out/promo`, `out/site`, `out/verify` | a full rebuild of each |
| `-IncludePip` | the pip download cache | re-downloads |
| `-RemoveMergedWorktrees` | worktrees that are clean and whose commits are all in `main` (`git worktree remove`, never `--force`) | none |
| `-Install` | nothing: registers a weekly Task Scheduler job for the default run | none |

It never touches `apexsim-content/build/tracks`, `apexsim-content/.cache`, the
content itself, `artifacts`, `.git`,
`server/ffi/target` (the client links the physics library built there) or the
shared Unreal derived-data cache, and it unlinks junctions instead of following
them. It skips cargo and Unreal targets while `cargo`/`rustc` or the editor run.
Each `-Apply` appends to `out/clean_workspace.log`. To keep scratch targets
small, point `CARGO_TARGET_DIR` at `<drive>:\apexsim-cargo-target-<name>` so the
script finds them.

## Traps

- Every commandlet needs the editor closed; `ats-*` tools need the content
  root (`apexsim-content/`) as working directory, code scripts the code root
  (`apexsim/`).
- The whole-calendar bake tests take about twenty minutes; do not run them
  (or `-- --include-ignored`) unasked.
- A YAML edit changes the track's checksum: re-export it, or clients report a
  content mismatch ([architecture.md](architecture.md#content-checksums)).
