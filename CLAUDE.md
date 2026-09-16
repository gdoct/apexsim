# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ApexSim is a source-available, proprietary simracing platform with a high-frequency authoritative Rust server (240Hz) and a Godot 4.5 C# client (the client is deprecated; server work does not need to keep it in sync). The server owns physics simulation and distributes telemetry while handling lobby/session management over TCP+TLS with MessagePack serialization. Protocol v2: after a token handshake binds the client's UDP address, telemetry (compact positional encoding, session-scoped car indices announced via a reliable `SessionRoster` message) and player input flow over UDP, with TCP fallback for un-handshaken clients.

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

### Play from the editor build
`scripts/play_editor.ps1` runs the game through `UnrealEditor.exe -game` on
the ApexSimEditor binaries - no cook, so a C++ change is one incremental build
from playable. It starts a local server (`cargo build --release` first) when
settings.yml points at this machine and nothing is listening, and warns when
the running server or the editor build is older than the source. `-Build`,
`-RestartServer`, `-DryRun`; `-CreateShortcut` puts a desktop shortcut
that runs it with `-Interactive` (asks instead of warning).

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

### Real-world layouts (`<Stem>.layout.json`, `ats-dress`)
The centerlines are real (public GPS traces), but everything beside the road
used to be invented: the procedural enrichment pass put grandstands wherever
there was room, guessed which side the pit lane went, gave no circuit its
landmarks and ringed every venue — dunes included — with the same tree belt.

A circuit's real furniture now comes from a **layout dossier** checked in
beside its YAML, `content/tracks/real/<Stem>.layout.json`: named corners, the
pit lane's real polyline and side, the grandstands with their names, sizes and
road-facing outlines, the buildings, what crosses over the road, landmarks,
and the outlines of the real woodland with its leaf type. It is built from
OpenStreetMap (ODbL; the attribution travels in the file) by

```bash
python scripts/osm_layout.py --all            # or: Monza Spa [--offline]
```

which georeferences the extract onto the track's own frame — an FFT
rotation/translation search, then trimmed ICP — and refuses to write a dossier
the fit does not explain (`fit` records the rmse and the coverage; every
circuit but Le Mans matches end to end, and there two thirds of the Sarthe is
public road in OSM rather than `highway=raceway`). Facts OSM lacks live in
`MANUAL_STANDS` / `MANUAL_CROSSINGS` / `MANUAL_LANDMARKS` in that script, from
each circuit's published seating map, and survive a refetch; a manual stand is
given as a station span and `outside`/`inside`, and the script lays its front
along the centerline so it curves with the bend. Raw extracts are cached,
gitignored, under `content/tracks/osm-cache/`.

`ats-dress` turns a dossier into scenery and then grooms around it:

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
                                            # or: content/tracks/real/Spa.yaml [--dry-run]
```

It owns every prop of the kinds it lays (grandstand, building, attraction,
bridge, light, vehicle, sky) plus the pit lane, deleting and re-laying them
each run — so it also clears the old scatter — while barriers, tire walls,
distance boards and trees stay `groom`'s. A stand is laid as 40 m runs along
its real front, each carrying its own `length_m`, so the Unreal side builds
bays that follow the outline; the family comes from the real depth and roof.
The pit lane is written with `authored: true`, which is what stops grooming
replacing it with a generated ribbon, and the Unreal bake generates the
garages, boxes and pit walls from it as before. Buildings inside the pit
complex are skipped for that reason.

Grooming is dossier-aware too (`groom_scene_with`, and `ats-groom` loads the
dossier by itself): dressed props are re-seated but never pushed, and tree
belts are planted **only inside the real woods**, with the species taken from
the wood's leaf type — which is what leaves Zandvoort's dunes bare, Spa's
Ardennes in spruce and the Parco di Monza in broadleaf. Both passes recycle
their own element ids, so re-running either writes a byte-identical file.

Le Mans, Zandvoort, Spa, Monza and Silverstone have dossiers; a track without
one is groomed exactly as before.

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

The exporter also writes three gitignored sidecars (like the exports) into
`content/tracks/real/`, all loaded by the server from beside the YAML and
all shipped in `Server/` by `build_release.ps1`:

- `<Stem>.ground.msgpack` — a 4 m heightfield of the ground the client
  renders, in the server frame (`ground.rs`), so a car that leaves the
  asphalt follows the rendered verge and terrain instead of holding road
  height. Without it, off-track elevation falls back to the centerline.
- `<Stem>.curbs.msgpack` — how far the curbs reach past each road edge,
  one sample per metre of centerline station (`curbs.rs`). The curbs are
  authored in the `.ats` and reach the server only as this number: physics
  counts a car within the band as on the track, with `curb_grip` and no
  off-track drag. Without it the road edge is the track limit and
  a driver using the curbs is slowed as if on grass.
- `<Stem>.walls.msgpack` — the barriers as the sim needs them
  (`walls.rs`): every armco, tire wall, fence and pit wall as a line
  segment along its heading, every stand, building, garage and fairground
  piece as the four sides of its footprint, plus an underpass's abutment
  walls and deck parapets, each with the ground height at its base, its
  height and a material (armco / tires / concrete). Runs of thin walls
  with ends under 4.5 m apart are joined so a car cannot slip between two
  modules. `physics::check_wall_collisions` runs after the car-car pass
  every tick: the car's box against each nearby segment (uniform 16 m
  grid, ascending index order for determinism) with the same SAT as
  car-car, pushed out along the wall normal, bounced by the material's
  restitution, scrubbed by its friction, spun by the impulse's moment
  about the deepest corner, damaged and fed back like a car-car hit; a
  car resting on the wall grinds along it. A wall only counts when the
  car's height band overlaps it, which is what keeps a car on Suzuka's
  deck off the abutment walls below (written a metre short of the upper
  ground) and a car in the slot off the parapets. Without it nothing
  stops a car off the road: the client has no physics, cars are telemetry
  puppets, so barrier collision lives here and nowhere else.

Where the course passes over itself with at least 4 m to spare (Suzuka's
crossover) the terrain finds an `Underpass` (`terrain.rs`): behind wall lines
3.5 m past the lower road's edges the ground is the upper road's embankment,
so the lower road runs through a slot. The heightfield cannot draw that
vertical step, so `ue_export` cuts the ground grid along the walls and draws
the slot floor, abutment walls and the deck (parapets, yellow fascia; material
family `structure`) itself; ground-anchored strips of the upper road sit on the
deck or are dropped over the slot, and `ats-groom` lays no armco there. The
ground sidecar reports the lower level under the bridge, and the server holds
road height within 2.5 m of an edge when the ground there is a level away (the
deck). A heightfield still cannot hold both levels, so a car that leaves the
bridge past the parapet falls into the slot.

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

### Cars (`content/cars`, `ApexCarImport`)
A car is `content/cars/<folder>/car.toml` plus the GLB its `model` names.
The client finds its mesh through `/Game/Data/DT_CarCatalog`, keyed by the
TOML's `id` (the wire only carries id and name); a car with no row shows
placeholder art and an empty turntable, and a row whose mesh belongs to
another folder previews as that other car. `ApexCarImport` keeps both in
step:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexCarImport -all          # or -car=yotota-lmp2 / -list / -remove=folder / -force / -dryrun
```

Each GLB goes through Interchange (one combined mesh, no collision, no
Nanite, materials beside it) to `/Game/Cars/<folder>/SM_<folder>` (hyphens
become underscores), and the row gets name, brand, class, year, country,
mass, power, folder and mesh from the TOML. Without `-force` the run is
additive: existing rows keep their preview framing, cockpit points and
hand-tuned fields, and only a missing or foreign mesh is replaced; the four
cars imported by hand before the commandlet existed keep their meshes.
`AApexRaceCarActor` turns the mesh −90° about Z, so a car must be long along
its local Y; the import logs a warning when it is not. `ApexSim.Cars.Toml`
tests the TOML scan.

The body GLBs have no wheels: the client draws four copies of the class's
shared wheel (`content/wheels/<class>.glb` → `/Game/Cars/Wheels/<class>/SM_Wheel_<class>`)
where the car.toml's `[wheels]` table puts them, steers the front pair and
rolls all four from the telemetry (`Race/ApexCarWheels.h`, row field
`Wheels`, refreshed on every import like the checksum; docs/CAR_MODELS.md).
A GLB changed on disk needs `-force` to be re-imported.

### Content checksums (`content_crc.rs`, `ApexContentCrc.h`)
The client races on a level baked from the track YAML and shows a mesh
imported beside a `car.toml`, while the server simulates from those files
themselves, so each file carries one checksum computed the same way in three
places: the CRC-32 of zlib/PNG over the file's bytes with every carriage
return dropped (so `core.autocrlf` cannot split the two sides; check vector
`"123456789"` → `0xCBF43926`). The server computes it as it loads
(`CarConfig::content_crc`, `TrackConfig::content_crc`) and sends it as
`ContentCrc` in the lobby's car and track summaries; `SessionSummary` now also
carries `TrackId` so a client can find the session's track without matching
names. On the client `build_track_catalog.py` writes `source_crc` into the
manifest and `ApexTrackCatalogSync` puts it on the row as `SourceCrc`;
`ApexCarImport` hashes the TOML bytes onto the car row. Both commandlets
refresh that one field even on an additive run, because it is derived, never
hand-tuned. When the race director streams a track level, and when it spawns
the local player's car, `UApexMenuFlowSubsystem::VerifyTrackContent` /
`VerifyCarContent` compare row against server: a mismatch is a warning in the
log and a toast (`OnContentMismatch` → the root widget); a demo session only
logs; a side with no checksum (an old server, a row from before the field) is
"unknown" and logged once, never toasted. The first `LobbyState` also lists
every stale row at once (`ReportUnmatchedCatalogIds`). A row imported before
the field has `SourceCrc = 0`: re-run the sync or import to fill it in.
`ApexSim.Content.*` tests pin the check vector, the line-ending rule and the
compare semantics; `content_crc::tests` and `test_lobby_summaries_carry_content_crc`
do the same on the server.

### Prop kit (`content/props`, `ApexPropImport`)
The trackside scenery is an authored kit of GLBs, `content/props/<kind>/<asset>.glb`
(catalogue and conventions in `docs/PROPS.md`; the loader's design in
`docs/PROPS_LOADER.md`). It reaches Unreal once, not per track:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexPropImport -all          # or -kind=barrier,board / -asset=barrier/armco_4m
./scripts/build_track_levels.ps1 -ImportProps   # the same, before the bake + import
```

`ApexPropImportCommandlet` runs each GLB through Interchange (the engine's
glTF stack, adjusted: one combined mesh, authored normals kept, Nanite on
`grandstand/building/pit/bridge/attraction`, box collision on all but trees
and the sky) into `/Game/Props/<kind>/SM_<asset>`, with the materials and
textures under `/Game/Props/<kind>/Materials` shared by name across the kind
(the first GLB to bring `armco_galv` in owns it). Material slot names are the
glTF material names, which is what everything below keys on. The ferris
wheel imports as `SM_ferris_wheel` plus `SM_ferris_wheel_rotor` (the `rotor`
node, pivot at the hub). The loose PNGs become textures: brands and
markers as `/Game/Props/board/Brands/T_brand_<name>` and
`Markers/T_marker_<n>`, the flags (`sign/flags/<cc>.png`) as
`/Game/Props/sign/Flags/T_flag_<cc>`; each set comes along with its kind.
Every run is a fresh import (the target mesh package is deleted from disk
first and the registry rescanned): reimporting over an existing mesh keeps
its old slots and creates no materials. A kind-wide run clears the kind's
material folder first. The glTF materials come in as instances of the
engine's glTF library parent (importing them as full materials trips an
engine error on the library's `AlphaMode` input); that parent has neither
the Nanite nor the instanced-mesh usage flag, which the editor sets on the
fly but a cooked build does not, so each instance is re-parented onto a
flagged copy under `/Game/Props/_Parents`. The per-track parents
(`M_ApexTrackBase`, `M_ApexEmissive`, `M_ApexBrand`) carry the flags too.
`build_release.ps1` has a props stage (`-SkipProps` still checks
`/Game/Props` holds meshes).

Frames: glTF (x, y, z) lands in Unreal as (x, z, y), so a prop modelled in
Blender with the road on **-Y** arrives with the road on local **+Y** — the
same side the generated recipes use, and no extra yaw is applied to
authored assets. The exporter's yaw is the road heading; the builder flips
face-road kinds 180° by `RoadSideOf` as before.

The track builder (`ApexTrackAssetBuilder::ResolveProp`, data in
`ApexPropLibrary`) resolves every prop as `SM_<asset>` → the kind's default
(`armco_4m`, `tires_4m`, `hoarding_3m`, `mesh_4m`, `bay_10m`, `garage_6m`,
`start_gantry`, `floodlight_tower`, `broadleaf_m`, `blimp`, `marshal_post`,
`car_a`, `tent_6m`, `clubhouse`, `bollard`; only `cone` has none) → the generated
recipe for the prop's own kind → placeholder cube, so levels still build with
`/Game/Props` empty. Buildings face the road like the stands (every
authored one has a front); of the attractions the video screen, the stage
and the tent do, the ferris wheel, camera tower and portaloos do not;
vehicles are left as placed. An alias table maps the pre-kit keys the groomer and
scenes carry (`tree_generic`, `armco_generic`, `tire_wall_generic`,
`sign/board_200m|100m|50m` → `board/braking_marker` with the number as text,
`grandstand_main|corner`, `building/pit_garage`) — data, so the `.ats` files
are untouched. Instanced kinds go into one HISM per resolved (kind, asset,
text), actor `Props_<kind>_<asset>[_<text>]`; every prop actor is tagged
`ApexProp`, which the racing line's ground snap and the TV camera's ground
trace use to ignore bridge decks and garage roofs.

- **Text → material**: a `text` naming a brand (`piretti`, `rolux`, …)
  overrides the `board_brand` / `bridge_brand*` / `pit_team_board` /
  `tyre_bridge_brand` / `blimp_brand` / `balloon_envelope` slots with an
  instance of the per-track `M_ApexBrand` showing `T_brand_<text>`; a
  braking marker's `board_marker` slot takes `T_marker_<text>`; a flag
  pole's `flag_cloth` takes `T_flag_<text>` (`nl`, `de`, … `chequer`).
  Unknown text keeps the imported default and logs once per track.
  `led_panel` / `led_screen` / `pit_light_green` slots get a lit
  `M_ApexEmissive` instance (`pit_light_red` a dark one: the pit exit shows
  open) and the component a tag `ApexEmissive_<slot>` (fixed glow: there is
  no flag state on the wire yet).
- **Dressing**: the `.ats` carries a scene-wide `dressing { season,
  spectators }` (`summer`/`autumn`, default summer with spectators),
  exported as-is; the builder swaps meshes, not keys: with spectators every
  stand module becomes its `_crowd` twin (`ApexProps::CrowdVariant`; the
  caps have none), in autumn the broadleaf trees, poplars and bushes become
  `_autumn` (`AutumnVariant`; conifers stay). A missing variant falls back
  to the base mesh. The editor has a Dressing panel and the MCP a
  `set_dressing` tool.
- **Grandstands**: one prop is a stand of `round(length_m × scale / 10)`
  bays (`length_m` from the `.ats`, 30 m when absent; scale is a length
  multiplier so the legacy 30 m × 2.6 stands come out as eight bays) plus
  two end caps, under one `Grandstand_<n>` actor with a HISM per asset.
  The exporter writes `radius_m` (signed bend radius, positive on the
  outside) and the builder picks the wedge whose front radius is nearest:
  `_curve6` (95 m), `_curve12` (48 m), `_curve6_in` inside; the large
  family is straight only. `scaffold_10m` and `banking_seats` are not bay
  families: the module is tiled at 10 m, straight, with no caps.
  `ApexProps::LayoutGrandstand` is the pure maths.
- **Bridges**: never flipped or pushed; local Y scaled by `span_m / 15`
  (`span_m` = road width at the station from the exporter, else measured
  from the centerline). The start lights use `bridge/SM_start_gantry` as the
  `StartLights` root when it exists, with the five `ApexStartLight` lens
  components over the panel's upper lamp row (x −37, y (i−2)·80, z 545 cm)
  so the race director's countdown contract is unchanged; the recipe gantry
  is the fallback.
- **Pit complex**: `ats-export` generates it from the pit lane — one
  `pit/garage_6m` per box on the far side of the parallel pit road (6 m
  pitch, centred) with a `pit/box_kit` on the same pivot (it reaches 2.6 m
  out of the door onto the working lane), `garage_end` beyond each end, `pit_wall_6m` on the road
  side over the box span and `pit_wall_plain_6m` over the rest of the pit
  road, all facing the track — and drops the legacy `building/pit_garage`
  stand-ins whenever it does. Nothing goes on the entry/exit tapers.
- **Runtime**: `sky` props spawn as `AApexSkyDriftActor` (drift along the
  heading ±150 m at 2 m/s, a slow yaw sway); the ferris wheel as
  `AApexRotorActor`, its rotor at the hub turning at 0.5 rpm about local Y
  (`Race/ApexPropActors.h`).

New `PropKind`s `board, fence, pit, bridge, vehicle, attraction, sky` exist
in `ats.rs`; the groomer snaps a `board` onto the nearest barrier run, lays a
`fence` 1.5 m behind the wall line, seats a `bridge` at road height without
pushing it, leaves `pit` and `sky` alone and pushes `vehicle`/`attraction` by
their footprint. The editor's copy of the kit is `track-editor/src/props.rs`
(`props::KIT`: kind, key and authored footprint per asset, default first,
plus `resolve` for the legacy keys): the inspector offers the kind's keys
in a dropdown (a free-text `key` row stays for anything else), a new prop
starts as the kind's default, the stand-ins are drawn at the asset's size,
and the groomer pushes by that footprint. The MCP's `list_prop_assets`
lists it. `ApexSim.Props.*` automation tests cover the alias table, the
kind tables, the variants and the bay layout; `cargo test` in
`track-editor` covers the catalogue (every default is a kit file), the
export hints, the pit complex and the groom behaviours.

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

Three traps worth remembering: the menu shell runs in `FInputModeUIOnly`, where
the viewport discards game input entirely and no binding produces an event
(the controller switches to game-and-UI for the race); a Blueprint game
mode can silently override `PlayerControllerClass`, which the C++ game mode
now logs an error about; and game-and-UI does not move focus by itself, so
the controller names the viewport as the widget to focus and
`FApexMenuInputProcessor` puts focus back on it whenever an event arrives while
driving. Focus left in the shell routes events through the focusable
`WBP_Root`, whose default Slate handler eats the left stick, D-pad and arrows
as menu navigation - pad steering died while throttle and shoulders worked.

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

### Car motion on the client (`Race/ApexCarMotion.h`)
Cars are puppets of the telemetry; there is no client physics. Each frame's
sample goes into the car actor's `ApexMotion::FApexCarMotionBuffer`, and
every render frame reads the pose from a *playhead* that runs two telemetry
frames (33 ms at 60 Hz) behind the newest sample, blended between the
samples either side: location lerp, rotation slerp (so the ±180° yaw seam is
crossed the short way), and steering, speed and revs blended the same way
for the cockpit wheel and the dials. The playhead's clock is in **server
ticks**, which are exact where arrival times are not; its ticks-per-second
is a least-squares fit of tick against arrival time over the last two
seconds of frames (240 assumed until a second has been seen; a surprise of
more than 10% re-seats the playhead once), trimmed by a critically damped
loop (0.4 s error filter, 1.6 s correction) that holds the delay. Past the
newest sample the pose is dead-reckoned from the last two for up to 250 ms
(position and turn rate), then held; a jump of more than 20 m between
samples, or a tick that runs back more than a second, restarts the buffer
at the new place. The previous `VInterpTo` chase lagged by speed/18 (over
4 m at 80 m/s) and pumped at the broadcast rate whenever frames arrived in
lumps. `apexsim.car.InterpDelayFrames`, `apexsim.car.InterpMaxExtrapolationMs`,
`apexsim.car.InterpDebug 1` (per-car readout: buffered, measured tick rate,
spacing, lag, extrapolating). `ApexSim.Motion.*` tests run the buffer on
synthetic streams (lumpy arrivals, lost frames, a stall, a 120 Hz server).

### Race start and finish (`game_session.rs`, `UApexRootWidget`)
A car is seeded onto the centerline when it is put on the grid
(`physics::seed_track_progress`); lap 1 starts as a grid car crosses the line,
or on green for pole. The server classifies a car (`finish_position`) when it
completes `lap_limit` laps; once the winner is in, the session finishes when
every car is classified or at a deadline of max(60 s, 2 x the winner's average
lap). A finished human's car is driven by a server cool-down AI, because the
last input received otherwise stays applied. `start_countdown_mode(_, Race)`
lines the field up on the grid again. On the client the HUD ranks finishers
first (`ApexRace::RanksAhead`); cars still racing get a toast when P1 takes
the flag, and 2.5 s after the local car finishes the root widget swaps the race
view for a provisional `SessionResults` that re-sorts until the session ends.

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

### Screenshot camera (`Race/ApexShotCamera.h`)
A third camera on the race director, `ShotCamera`, parks anywhere on the
circuit for a screenshot. While a pose is set it is the only active camera,
the followed car's bodywork is shown and the cockpit rig hidden; cars
spawning, the followed car changing and C all go through `ApplyCameraMode`,
which keeps it. Every number is in the SERVER frame (metres, +Y left, yaw
counter-clockwise from +X, pitch positive up), so a shot can be worked out
straight from the track YAML's centerline. Console: `apexsim.cam.Goto X Y Z
[Yaw] [Pitch]`, `apexsim.cam.LookAt X Y Z TX TY TZ`, `apexsim.cam.Fov Deg`,
`apexsim.cam.Release`; each logs the pose back as an `-ApexCamera=` switch.
Command line, applied when the race view begins and held for that race:
`-ApexCamera=X,Y,Z,Yaw,Pitch` (yaw and pitch optional),
`-ApexCameraLookAt=X,Y,Z,TX,TY,TZ` (wins if both are given) and
`-ApexCameraFov=Deg` (default 70), e.g. `-game -ApexAutoRace -ApexTrack=Suzuka
-ApexCameraLookAt=... -ApexScreenshotAfter=12`. The conversions live in
`ApexRaceCoordinate.h`; `ApexSim.Camera.*` tests cover the parsing and the frame.

### Demo mode and the broadcast camera (`ApexDemoModeSubsystem`, `Race/ApexTvDirector.h`)

The menu plays an AI race behind its screens. `UApexDemoModeSubsystem` asks the
server for a `SessionKind::Demo` session whenever the client is connected and
not in a session: unlisted, unjoinable, spectated by its creator, counted
straight into a race, no replay written, removed when the spectator leaves.
`SessionJoined` carries `SessionKind`, and `UApexNetSubsystem` keeps a demo out
of every session delegate (`OnDemoSessionChanged` instead; `IsInSession()` is
false) and leaves it by itself before any create or join. `SessionJoined` drops every
telemetry frame already in the UDP queue (after a hitch it holds seconds of
the session just left; telemetry carries no session id), and a frame is only
applied when every car index fits the current roster: a stale demo frame
would otherwise stamp the demo's `Countdown`/`Racing` state on a session still
in Lobby, putting the race view over the lobby screen with nothing counting
down. The race director's
demo view streams the track (the player's pending track when it has a level),
and the root widget fades page backgrounds by `GetDemoBackdropOpacity()` under a
left-heavy scrim. Car select and session create return false from
`WantsLiveBackdrop` because the turntable shares the world, so the demo world is
hidden behind them. It restarts on a finished race, a track change or after
`apexsim.demo.MaxMinutes`. Each demo race rolls its own sky
(`UApexDemoModeSubsystem::RollConditions`: weighted toward dry daylight, with
rain, dusk and night in the mix) and sends it as the create's `conditions`, so
the server bakes the wet grip for the AI like any session;
`apexsim.demo.RandomSky 0` keeps the default sunny 13:00. `-ApexNoDemo`/`apexsim.demo.Enabled 0` turn it off;
`-ApexAutoRace` never starts one.

`ApexTv::FDirector` is the TV director, pure logic with ground and visibility
traces injected: it picks a car (battles, the leader, an incident: off track or
stopped, at most once per 12 s) and cuts between grid, trackside (long lens
from the lobby centerline, outside of the bend), helicopter, tracking, chase,
onboard, nose and reverse shots, with lag-compensated pans and depth of field.
`-ApexView=tv` or `apexsim.tv.View 1` uses it in a race; `apexsim.tv.Shot <name>`,
`apexsim.tv.Cut`, `apexsim.tv.Pace`, `apexsim.tv.Debug 1`; each cut and its
reason is logged at `LogApexSim Verbose`. `ApexSim.Tv.*` tests drive it on a
synthetic ring. `-ApexScreenshotAfter` takes a comma list of times.

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

### Startup splash hold (`ApexSimBoot`, `UApexStartupSplashSubsystem`)

The splash stays up until the menu's demo race is running behind it; the first
thing a player sees after it is the finished menu over a moving race, not the
UI on black, the window resize to the settings.yml mode and the race fading
in. The engine closes its own splash on the first frame, long before that.

`ApexSimBoot` is a UObject-free module at `LoadingPhase` `PostConfigInit`,
because the engine creates its game window in pre-init (the preload screen
manager), before any other project module loads. It installs two
thread-local hooks: when the first top-level `UnrealWindow` is created it
opens a copy of the splash (the same `Content/Splash/Splash.bmp` at the
engine splash's own rectangle, just beneath it, so the engine closing its
splash changes nothing on screen), and on that window's `WM_SHOWWINDOW` it
cloaks it with `DWMWA_CLOAK` (DWM refuses at creation with `E_HANDLE`). A
cloaked window is shown as far as the engine and Slate know: it ticks,
renders, lays out and takes its resize, but the compositor does not draw it.

`UApexStartupSplashSubsystem` claims the hold at game instance init, checks
the viewport's window is the cloaked one, and ends it: when
`AApexRaceDirector::IsDemoReady` (backdrop fully faded in), early when
`UApexDemoModeSubsystem::IsDemoExpected` is false (demo off, no server,
refused login, no track with a level, a refused create), or after
`apexsim.splash.MaxSeconds` (20). The reveal uncloaks the window beneath the
splash and fades the splash out over 0.35 s. An unclaimed hold is dropped at
engine init complete and a claimed one after 45 s, so a broken game cannot
stay invisible. Nothing is held in the editor, commandlets, `-nullrhi`,
`-nosplash`, `-ApexNoSplashHold`, `-ApexNoDemo`, `-ApexAutoRace`, or
exclusive fullscreen (a cloaked window cannot own the display). Screen
grabs that go through the compositor do not see a cloaked window, which is
how the hold was checked: a Python `ImageGrab` loop around a launch.

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
dropdowns (Adjust). The race cues (`CountdownTick`, `CountdownGo`, `LapLine`)
come from `AApexRaceDirector::UpdateRaceBleeps` on telemetry: a tick for each
of the last five countdown seconds (with the start lights), a higher tone on
green, and a double pip when the local car completes a lap; the demo is silent. Move is not raised by any widget: the root widget listens
to `FSlateApplication::OnFocusChanging` and plays it for focus moved by the
player, which is a change with cause `Navigation` or one made inside an
`ApexNav::FNavigationScope` (the scope is what separates a host's
`ApexNav::Focus` from the same call made when a screen opens). Scopes wrap
`RouteFromLeaf`, the navigable widget's key/analog handlers and the input
processor's focus recovery; do not add per-widget Move cues.

Volume lives on the settings overlay's Audio tab: master volume drives the
audio device's transient primary volume (`ApplyAudio`), menu-sound volume is
read at play time. `-ApexSettingsTab=6` opens that tab headlessly.
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

### Driving assists (`SetDriverAids`, `AllowedAssists`, the Assists settings tab)

Every assist runs on the server, because only it has the tyres: ABS,
traction control, the automatic gearbox and speed-sensitive steering are
`CarState` fields the physics reads every tick, and the racing line is built
there and sent on join. The client's settings overlay has an **Assists** tab
(`EApexSettingsTab::Assists`, group `EApexSettingsGroup::Assists`; the tab
order is gameplay, assists, graphics, camera, controls, wheel, audio, car
setup, so `-ApexSettingsTab=1` opens it) holding ABS, traction control OFF/LOW/HIGH,
gearbox, steering and the racing line. `UApexRootWidget::SendDriverAids`
sends `SetDriverAids { auto_gearbox, steering_assist, abs, traction_control }`
on join and on any change of the group; ABS and traction control are
`Option`s on the server (`CarState::abs`, `CarState::traction_control`) so an
AI, or a client from before the fields, drives the car as its `car.toml`
describes it. Traction control LOW holds a spinning wheel at peak slip as
before; HIGH caps drive at what the friction circle has left beside the
lateral force, less a margin (`TC_HIGH_LATERAL_MARGIN`), so full throttle on
a corner exit keeps the cornering grip instead of pushing the rear wide.

A session's host decides which assists its drivers may use: the create
screen's "Allowed assists" chips go out as `CreateSession.allowed_assists`
(`AllowedAssists { abs, traction_control, auto_gearbox, steering_assist,
racing_line }`, every field defaulting to true so an old client's session
allows everything), the session keeps them on `RaceSession::allowed_assists`
and echoes them in `SessionJoined.AllowedAssists`. The server enforces the
rule (`AllowedAssists::clamp`): a forbidden aid is pinned off when a car is
seated and on every `SetDriverAids`, whatever the client asked, and a
session that forbids the racing line sends none. On the client the Assists
tab dims a locked row, disables its pills and shows an "Off in this session"
badge (`UApexNetSubsystem::GetAllowedAssists`, `RefreshAssistLocks`); the
player's own choice is kept for the next session; `-ApexAutoRace
-ApexLockAssists=abs,tc,gearbox,steering,line` creates the auto-race session
with those forbidden, for a screenshot run of the locked tab
(`-ApexOpenSettings=N -ApexSettingsTab=1`). Golden bytes for all three
messages come from `network.rs` `test_assists_wire_format`
(`cargo test assists_wire_format -- --nocapture` prints them) and are pinned
in `ApexGoldenBlobs.h`.

### Weather and time of day (`SessionConditions`, `Race/ApexSkyModel.h`)

A session's host picks its sky on the create screen: a **Weather** chip row
(sunny, cloudy, overcast, light rain, heavy rain) and a **Time of day**
slider in quarter hours. Both go out inside `CreateSession.conditions`
(`SessionConditions { weather, time_of_day_minutes }`, defaulting to a
sunny 13:00 so an old client's session is unchanged), are kept on
`RaceSession::conditions`, echoed in `SessionJoined.Conditions` and listed
in every `SessionSummary` (the browser row shows "Light rain · 21:30").
The clock wraps onto one day on the way in (`SessionConditions::clamp`).

The **server owns the grip**: `create_session` bakes the weather into the
session's own copy of the track (`SessionConditions::apply_to_track`):
every centerline sample's `grip_modifier`, the surface's `base_grip` (which
the racing line and the AI's speed profile read) and the curb and off-track
grips, so physics, the AI and the racing line all follow with no branch in
the tick and a dry session is the track to the bit. Light rain is 0.86 of
the road's grip and heavy rain 0.74; painted curbs lose a further 15–25%
and grass 15–30% (`Weather::*_grip_factor`). Time of day is visual only.
Golden bytes come from the same `test_assists_wire_format`;
`tests/session_conditions_test.rs` checks the echo, the listing and the
bake end to end.

On the client `FApexSessionConditions` lives beside the assists in the net
module (`Describe`, `ClockText`, `WeatherLabel`), the flow keeps
`CreateConditions` (persisted on the profile), and `UApexNetSubsystem::
GetSessionConditions` holds the joined session's sky, demo included. The
race director turns it into light with `ApexSky::Derive` (pure maths,
`ApexSim.Sky.*` tests): the sun's elevation and azimuth for that hour on a
late-May day at 50° N with solar noon at 13:00, its lux from the airmass
and the cloud, its colour warming toward the horizon and greying under
cloud, the sky light scaled up under overcast, shadows off and the source
widened when the disc is gone, and after dark the directional light
becomes a one-lux blue moon that no longer drives the atmosphere. A
director-owned unbound post-process volume (priority 10, over the track
level's daylight clamp) carries the exposure floor and ceiling for the
hour, the wet desaturation and the bloom. What lives in the streamed level
is applied once its actors are in the world (`ApplyTrackLevelConditions`,
polled like the start lights): the bake's fog gets the weather's density,
start and colour (dark at night); in rain the road family's slots (`MI_road*`, `MI_pit_lane*` and the
`MI_wear_*` bands the racing line runs on) get dynamic instances with
`Roughness` 0.3 for the wet sheen, and the racing-line dots go glossy and
dark with them (`AApexRacingLineActor::SetWet`); after
dark every `floodlight_tower` / `lamp_post` instance gets a shadowless spot
light at its head (at most 96) and the `ApexEmissive_floodlight_lamp`
faces glow. Rain is `AApexRainActor`: up to 1500 slivers of the engine cube
in a box that travels with the active camera, falling in world space,
wrapped back in when they leave, each stretched along its apparent
velocity so they streak at speed (no particle assets exist in the
project). Cars get two lumen-rated spot headlights at the nose and dim
running tail lights (`AApexRaceCarActor::SetHeadlights`,
`apexsim.car.HeadlightLumens`) whenever the sun is under 6° or it rains.
`-ApexWeather=heavyrain -ApexTimeOfDay=22:15` put an `-ApexAutoRace`
screenshot run under that sky.

### Car setup (`server/src/car_setup.rs`, `SetCarSetup`, the Car setup settings tab)

The garage: tyres, engine, transmission, torque and suspension, per driver.
A setup is **clicks** off the car's own `car.toml`, one `i8` per knob
(`CarSetup`, 14 knobs in `KNOBS` order), so neither the wire nor the client
needs the car's base figures; the client shows "+2  (+8%)" and the server
turns that into newtons per metre against the file it loaded. Every knob
has a fixed click range (±5; the rev limiter and torque map only go down)
and a fixed effect per click (`*_PER_CLICK`): tyre pressure 5 kPa per axle,
rev limiter −100 rpm (redline comes down with it, so the auto gearbox
follows), engine braking ±15%, final drive ±2%, gear spread ±2% on top gear
blended down the ladder with first untouched, torque map −4% on the whole
curve, brake bias ±1% front, springs ±4%, dampers ±5% (bump and rebound
together), anti-roll bars ±8%. Only knobs the sim reads are offered: there
is no differential model and rolling resistance is never consumed.

Tyre pressure is the one physics addition: `TireConfig` carries
`pressure_front_kpa` / `pressure_rear_kpa` / `optimal_pressure_kpa` (180
by default, not yet in any car.toml) and `pressure_grip_factor` costs an
axle grip quadratically off the optimum (4% at five clicks), exactly 1.0 at
it, so a stock car is bit-identical to before (`update_car_3d` passes
`effective_grip_front` / `_rear` to the wheel solver). Raising one end's
pressure loosens that end, which is what makes the knob a balance tool.

The client sends `SetCarSetup { tyre_pressure_front, … }` on join and on any
change of the group (`UApexRootWidget::SendCarSetup`, like the aids). The
server clamps (`CarSetup::clamp`) and bakes the result into a tuned copy of
the `CarConfig` (`CarSetup::apply`) kept on `GameSession::tuned_configs` per
player; the tick loops look the car up through `simulated_config`, tuned
first, shared otherwise, so the hot loop pays one map lookup and nothing
else. A stock setup drops the copy; leaving the session drops it. The setup
applies at once, on the grid or mid-lap. AI cars and the collision passes
(dimensions only) use the shared config. Golden bytes come from
`network.rs` `test_car_setup_wire_format` (`cargo test car_setup_wire_format
-- --nocapture`) and are pinned as `ApexGolden::C_SetCarSetup`.

On the client the setup lives on `UApexSettingsSave::CarSetup`
(`FApexCarSetup`, `TArray<int32> Clicks`, one setup shared by every car),
edited on the settings overlay's **Car setup** tab
(`EApexSettingsTab::CarSetup`, appended after Audio so
`-ApexSettingsTab=7` opens it) as a two-column page of `UApexStepperWidget`
rows: a − / + pill pair around a read-out from `ApexCarSetup::Describe`. The
knob table (`ApexCarSetup::Knob`: wire key, range, per-click size and unit)
lives in the net module beside the encoder and mirrors the server's
constants; `ApexSim.Net.CarSetup.Clicks` pins the ranges and read-outs, the
golden encode test the bytes. Reset on that tab returns every knob to stock.

### Force feedback (`server/src/feedback.rs`, `Input/ApexForceFeedback.h`)

The server works out what the driver should feel, because only it has the
tyre forces. Every tick `update_car_3d` records a `FeedbackTick` into
`CarState::feedback`: the steering-column torque (each front tyre's `-Fy`
times a pneumatic trail that shrinks to zero at twice the peak slip angle,
plus a fixed caster share, so the wheel goes light as the fronts let go;
1.0 = the front axle at its static grip limit, positive turns the wheel
left), slip per wheel as a multiple of the tyre's peak, the surface under
each wheel (road/curb/off, from the same `contact_surface` that sets the
track limits), suspension speed, ABS/TC activity, and contact closing speed.
On each telemetry tick the game loop drains it into a `DriverFeedback` for
the car's human driver: positional encoding, UDP only (no TCP fallback: a late
force is worse than none), every torque sample kept and the transients
peak-held. Golden bytes live in `network.rs` and `ApexUdpGolden::S_DriverFeedback`.

On the client `UApexNetSubsystem` merges everything that arrived since its
last tick (`FApexDriverFeedback::Absorb`) and `ApexFfb::MakeSignals` turns it
into device-agnostic signals, which two mixers read.

`ApexFfb::MixGamepad` drives a pad's two motors: front slide on the light
motor, rear slide on the heavy one, curb ribs at a speed-set rate, ABS/TC
pulse trains, grass noise, and decaying thumps for bumps, contact and shifts.
It assumes XInput's channel layout; the GameInput plugin would put the Small
channels on the trigger motors. Strength is the Controls tab's "Pad
vibration" slider (`UApexSettingsSave::Vibration`, 0.5 = as designed), which
pulses the pad while dragged.

`ApexFfb::MixWheel` drives a wheelbase: the torque *is* the feel (the rim
going light is the front tyres letting go), soft-limited past 0.75 so a car
loaded past its reference still feels stronger rather than clipping, and
smoothed with a 12 ms pole because the samples arrive in 60 Hz lumps and a
direct drive base feels that as grain. On top of it one vibration channel
carries whichever of curbs, grass, ABS, lockup or a hit is loudest, plus a
damper that is heaviest at a standstill and a centring spring used only in
the menus. Its settings are the Wheel tab's Force / Road effects / Damping /
Direction (`UApexSettingsSave::WheelForce` and friends).

`AApexPlayerController::UpdateForceFeedback` runs both every frame — it is
the one hook that ticks with no pawn, no race and a pause menu up.
`apexsim.ffb.Debug 1` prints the signals, the motor levels and the wheel's
forces on screen.

### Wheels and pedals (`Source/ApexSimInput/`)

XInput is the engine's pad path and knows nothing else; every wheelbase,
pedal set, shifter and button box on Windows speaks DirectInput. The
`ApexSimInput` module is an input-device plugin like the engine's XInput one:
`FApexDirectInputDevice` is created on the platform application's first poll,
ticked every frame, and told about WM_DEVICECHANGE, so a wheel plugged in
mid-session works without a restart. XInput devices are skipped (their path
carries `IG_`), or an Xbox pad would arrive twice with its triggers merged.

Every control is an ordinary `FKey` — `DInput1_X`, `DInput2_Button7`,
`DInput1_Hat1Up`, registered with `EKeys` at module startup — so Enhanced
Input, the rebinding screen and the settings slot need no second input path.
The number is a device **slot**, not an enumeration index: a slot belongs to
one physical device by instance GUID (falling back to the product GUID when
the same wheel moves USB port) and is remembered in
`Saved/ApexInputDevices.json`, so plugging in a button box cannot shift the
pedals' bindings onto it.

Three things about how it reads devices:

- **Axes are sent when they move and every frame they rest away from zero.**
  A pedal rests at -1, and `FlushPressedKeys` at the end of a race clears the
  player input's key state, so the resting value has to keep arriving.
- **A pedal's -1..1 is folded into 0..1 by a mapping modifier**
  (`UApexInputModifierPedal`), never by the handler — the same action is also
  fed by a trigger and a key, which are 0..1 already. The pad's deadzone and
  steering curve are likewise a modifier (`UApexInputModifierPadSteering`) so
  a wheel does not get a thumbstick's deadzone; both read the settings live,
  so dragging a slider needs no context rebuild.
- **A lost device sends its axes back to where they were when it arrived**,
  not to zero: zero is half throttle to a pedal binding.

Force feedback is taken **late and given back**: devices are opened shared,
and only the wheel the steering is bound to is taken exclusively, the first
frame a race asks for forces (`AcquireForForces`). That is what lets an open
editor and a launched game share a wheelbase. Effects are a constant force, a
sine, a damper and a spring, updated only when they change; a watchdog drops
every force if the game stops updating them for 0.3 s, so a hitch or a crash
never leaves a wheel pulling. Which way a positive force turns a rim is not
something DirectInput promises — hence the Wheel page's Direction test, which
pushes right and asks.

Bindings live in the settings overlay's **Wheel** tab (devices, forces, and
the wheel's own slots with live meters on steering, throttle and brake);
slots 4-6 are the wheel column. They are kept **per device**: rebinding with
one base plugged in leaves the other base's mapping alone, and unbinding
unbinds the device in front of the player
(`ApexInput::FindBinding`/`StoreBinding`). Binding an axis reads how far it
*moves*, not where it sits, and the direction of the move is what sets
`FApexKeyBinding::bInvert` — which is how a pedal that rests at the top of
its travel configures itself.

`apexsim.input.Devices` lists what is attached, with slots, capabilities and
live readings; `apexsim.input.Rescan` enumerates again. `ApexSim.Input.*`
automation tests cover the slot registry, the key names, the readings, the
binding rules and both mixers.

There is no wheel support for an H-pattern shifter (the wire protocol's gear
field is filled from a shift delta, not an absolute gear) and no soft lock or
rotation setting; a wheelbase's own driver sets its rotation.

### Content (`content/`)
- `cars/` - Car physics definitions (TOML: `car.toml` per car; most physical parameters moddable with validated ranges)
- `tracks/` - Track definitions (YAML/JSON) + procedural terrain caches (`.terrain.msgpack`)
- Shared between server and clients

## Key Technical Details

- **Coordinate System**: Right-handed. Origin at track start/finish line center. +X is track direction, +Y is left of track. Angles (yaw) counter-clockwise from +X.
- **Serialization**: MessagePack via `rmp-serde` (Rust) and custom serializer (C#)
- **Physics**: 4-wheel 3D model with per-wheel loads and suspension; fixed timestep dt = 1/tick_rate (plumbed from config, not hardcoded)
- **Determinism**: `participants` is a `BTreeMap` (ordered iteration); AI noise is hash-based; no env/wall-clock reads in the sim path. Guarded by `tests/determinism_test.rs`
- **Tick timing on Windows**: the game loop sleeps between ticks on a tokio `interval`, and a Windows sleep is only as fine as the process's timer resolution (15.6 ms by default, per process since Windows 10 2004). `timer_resolution::HighResolutionTimer` holds 1 ms for the loop's lifetime; without it 240 Hz ran at 64 and the sim at 27% of real time. The loop warns when the achieved rate over 5 s drops below 90%; `test_session_ticks_at_the_configured_rate` guards it end to end
- **Hot loop**: nearest-centerline queries use a windowed search seeded by each car's cached index (`CarState::nearest_centerline_idx`) — keep new per-tick track queries on this path
- **AI Drivers**: deterministic synthetic input per tick from line look-ahead. The field races in the host car's `class` from `car.toml` (`game_session::class_field`: every car of that class, dealt round-robin by id starting after the host's; a car with no class races only against itself), and each `RosterEntry` carries `CarConfigId` so the client's race director draws every car with its own catalog mesh (an older server's roster falls back to the local player's car)
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
- `tests/grip_probe_test.rs` (ignored) is the grip-tuning harness: skidpad lateral-g sweep, a follower driving Silverstone's first corner at the racing line's speed, and every car's ideal-lap time from its racing-line profile (`PROBE_MU_SCALE` / `PROBE_CLA_SCALE` / `PROBE_CLASS` sweep grip and downforce without editing TOMLs). Class grip levels are set so those lap times land a few seconds off real poles
- Integration tests simulate real client connections with `TestClient` structs against an in-process server
- Enable debug logging: `RUST_LOG=debug cargo test ...`
- CI (`.github/workflows/ci.yml`) runs fmt-check, clippy (`-D warnings` — keep the tree warning-free), build, and tests for the server plus a dotnet build/test for the client; Dependabot auto-merge builds and tests before merging
