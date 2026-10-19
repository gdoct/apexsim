# Architecture

ApexSim is a sim-racing platform in three programs that share one data tree,
kept in two roots: the code (`apexsim/`, which these docs speak from) and the
content with its tools (`apexsim-content/`, beside it). The code depends on
the content, never the other way round.
The **server** (Rust, `server/`) owns the simulation: physics, AI, race rules
and timing run there at 420 Hz and nowhere else. The **Unreal client**
(UE 5.8, `game-unreal/`) draws what the server says and sends the driver's
input; cars are telemetry puppets with no client physics. The **track
pipeline** (Rust `apexsim-content/track-editor/` plus Python in
`apexsim-content/tools/`) turns the source data in `apexsim-content/` into the
files both of them read. This page is the map;
the topic docs under `docs/` go deeper.

## Code

- `server/` - the authoritative server crate (`apexsim-server`, plus the
  offline tool `apexsim-replay`). Module map in [server/README.md](../server/README.md).
- `server/ffi/` - `apexsim-physics`, a separate crate on the server crate:
  the car step (`car_step::step_lone_car`) as a static library behind a C
  API (`apexsim_physics.h`), for the client's prediction of its own car
  ([proposals/client-prediction.md](proposals/client-prediction.md)). Not
  linked into the client yet.
- `game-unreal/Source/` - client modules: `ApexSim` (game), `ApexSimNet`
  (protocol, MessagePack codec), `ApexSimInput` (DirectInput wheels),
  `ApexSimBoot` (splash hold, loads at `PostConfigInit`), `ApexTrackEditor`
  (editor-only import commandlets). See [game/client.md](game/client.md).
- `apexsim-content/track-editor/` - workspace of `track-core` (`core/`: the `.ats` format,
  dress/groom/smooth/bank, the Unreal bake, the `ats-*` tools) and the Bevy
  editor (`src/`). See [apexsim-content/docs/track-pipeline.md](../../apexsim-content/docs/track-pipeline.md).
- `apexsim-lib/formats/` - `apexsim-formats`, the files the content writes
  and the code reads, modelled once: the track YAML, the five sidecars,
  `content_crc` and the format levels. The server and track-core both depend
  on it; it depends on neither. See [building.md](building.md#formats-apexsim-libformats).
- `apexsim-content/tools/` - the Python content tools (OSM, DEM, AC import,
  previews, the Blender generators in `generators/`); `scripts/` - the
  PowerShell build scripts and the code-side Python (site, promo, verify,
  benchmark). See [building.md](building.md).
- `launcher/` - the release package's `launcher.exe` (C++).
- `hud/` - the HUD components ([game/hud-modding.md](game/hud-modding.md));
  `showcase.yml` - the showcases to render.
- `apexsim-content/` - shared source data (below).

## How the programs talk

| From | To | How |
|---|---|---|
| Client | Server | TCP 9000 (auth, lobby, sessions, reliable messages), UDP 9001 (input) |
| Server | Client | TCP (reliable messages, telemetry fallback), UDP (telemetry, force feedback, spectator frames) |
| Pipeline | Server | Files beside each track YAML: `<Stem>.{ground,curbs,walls,road,pit}.msgpack` sidecars |
| Pipeline | Client | `apexsim-content/build/tracks/<Stem>.uescene.json` + `.uemesh` exports and previews (`Game/Tracks` in a package) |
| `apexsim-content/cars` | Both | The server reads `car.toml`; the client reads the same file and the GLBs it names |
| Operators | Server | HTTP 9002 (`/health`, `/ready`, `/metrics`), admin dashboard 9003/9004 |

The wire protocol is in [server/protocol.md](server/protocol.md); running a
server in [server/operations.md](server/operations.md).

## The server tick

`game_loop::run_game_loop` (`server/src/game_loop/mod.rs`) runs on a tokio
`interval` at `[server] tick_rate_hz` (default 420, valid 30-1000;
`game_session::DEFAULT_TICK_RATE_HZ`). Each tick:

1. Drain every inbound event without blocking (`try_recv`) and dispatch it
   (`dispatch.rs`). A `CreateSession` on a track whose sidecars are not yet in
   memory is held, with everything its connection sends after it, while a
   blocking task loads them (`track_loads.rs`).
2. Once a second, drop connections whose heartbeat is stale (`lifecycle.rs`);
   every two seconds, broadcast `LobbyState`.
3. Tick every session (`tick.rs`), each inside `catch_unwind` so one
   session's panic does not take the server down.
4. Fan out rosters, lap timing and telemetry (`broadcast.rs`). Telemetry is
   built and serialized once per session, every `[network] telemetry_divisor`
   ticks (default 7, so 60 Hz at 420 Hz); the client interpolates.
5. Advance the showcase channels and expire finished sessions.

Missed ticks are skipped, never burst (`MissedTickBehavior::Skip`): the sim
advances a fixed `dt = 1 / tick_rate` per tick. Everything in the sim is
dt-scaled or counts `tick_rate` ticks, so tests read `DEFAULT_TICK_RATE_HZ`
rather than a literal.

The hot loop's nearest-centerline queries are a windowed search seeded by the
car's cached index (`CarState::nearest_centerline_idx`); a new per-tick track
query belongs on that path, not on a full scan.

## Determinism

A seeded session replays bit for bit: `apexsim-replay render`, showcases,
replays and the AI survey all depend on it, and
`server/tests/determinism_test.rs` asserts it on the centerline and the road
mesh backends. Inside the sim path:

- No iteration over a `HashMap` where order can reach the result. Session
  participants are a `BTreeMap` (`RaceSession::participants`), and collision
  pairs are visited in that order.
- No RNG. Anything random-looking is a hash of stable inputs
  (`wind::hash01(seed, salt)`: driver, lap, corner, tick); AI noise, gusts,
  punctures and racecraft chances all go through it.
- No wall clock and no environment reads. Time is the tick counter.

The game loop's own bookkeeping (rate checks, rate limiting, heartbeats) uses
`Instant`, which is fine because it never feeds the simulation.

### Simulation and runtime modules

The server crate's modules are of two kinds. **Simulation**: the car
(`physics`, `car_step`, `aero`, `brakes`, `tyre_thermal`, `engine_heat`,
`damage`, `hybrid`, `geometry`, `feedback`, `car_setup`, `own_car`), the
track and its sidecars (`ground`, `curbs`, `walls`, `road_mesh`,
`road_state`, `track_loader`, `track_bundle`, `car_loader`, `content_crc`,
`data`), the race (`game_session`, `laps`, `wind`, `drs`, `slipstream`,
`recovery`, `ai_driver`, `racecraft`, `racing_line`, `pit`, `race_control`,
`race_start`, `safety_car`, `weekend`, `grid_order`, `headlights`,
`conditions`, `balance`, `debug_hooks`) and the plain data the session
hands out (`ghost`, `classification`). **Runtime**: everything else - the
wire (`network`, `transport`, `udp_seal`, `session_wire`), the loop
(`game_loop/`, `server`, `lobby`, `chat`, `main`, `config`), operations
(`health`, `metrics`, `tick_jitter`, `timer_resolution`, `webhook`,
`admin/`), files (`records`, `results`, `setup_sheet`, `track_content`) and
spectating and tools (`spectator`, `session_stream`, `stream_recorder`,
`showcase`, `replay`, `replay_tools`, `track_guide`, `bin/`).

Simulation modules do not use runtime modules: the session hands out sim
types (`WeekendState`, `PitService`, `RosterRow`, `TrackCorner`, ...) and
`session_wire` turns them into messages (`SessionWire` keeps
`get_compact_telemetry`, `build_roster` and the rest callable on a
`GameSession`). Where a value is the sim's own and the wire carries it as
is, the sim defines it and `network` re-exports it (`RecordBytes`,
`RaceClock`, `ground_byte`, `SLIDE_FLAG_LOCKED_SHIFT`, `DriverFeedback`);
`config` re-exports `RoadContactMode` the same way. This boundary becomes a
crate boundary (`apexsim-sim`). A few unit tests inside simulation modules
still reach the wire (`SkyNow`, `lap_flags_of`, `RACE_FLAG_YELLOW_SHIFT`,
`SessionWire`).

## Tick timing on Windows

A Windows sleep is only as fine as the process's timer resolution, 15.6 ms
by default (per process since Windows 10 2004), which held a 240 Hz loop to
64 ticks a second and the sim to about a quarter of real time, with lap times
(counted in ticks) hiding it. `timer_resolution::HighResolutionTimer` holds a
1 ms timer for the loop's lifetime and opts the process out of the power
throttling with which Windows 11 ignores that request for a minimized or
hidden window. The loop warns when the achieved rate over 5 s falls below
90% of the configured one; `protocol_test::test_session_ticks_at_the_configured_rate`
guards it end to end. The same trap is why dispatch uses `try_recv` and not
a short `tokio::time::timeout`: a sub-15 ms wait rounds up to 15 ms.

## Coordinate system

Server frame (every number on the wire, in track files and in sidecars):
right-handed, metres, origin at the centre of the start/finish line, +X the
track direction at the line, +Y to the left, +Z up; yaw counter-clockwise
from +X. Positive banking lifts the road's left edge.

Unreal is left-handed in centimetres with +Y to the right, so the client
scales by 100, flips Y and negates yaw and roll. That conversion lives only in
`game-unreal/Source/ApexSim/Public/Race/ApexRaceCoordinate.h`; go through it.
glTF assets land in Unreal as `(x, z, y)` (Interchange's frame), see
[apexsim-content/docs/cars.md](../../apexsim-content/docs/cars.md) and [apexsim-content/docs/props.md](../../apexsim-content/docs/props.md).

## Wire-format rules

The full workflow is in [server/protocol.md](server/protocol.md); the rules
that bind every change:

- MessagePack via `rmp_serde` on the server and a hand-written codec in
  `ApexSimNet` on the client. Reliable and client messages use the **named**
  encoding (`to_vec_named`, an internally tagged `{type, data}` envelope);
  `TelemetryCompact` and `DriverFeedback` use the **positional** encoding
  (`to_vec`), where a field is identified by its index.
- Fields are only ever **appended**. A new named field carries
  `#[serde(default)]` and is left off the wire when it has its default value,
  so an older peer's bytes are unchanged; a new positional field goes at the
  end, where an older reader skips it. Never reorder, rename or remove.
- Every message's bytes are pinned on both sides: a server test prints them
  (`cargo test <name>_wire_format -- --nocapture`), and the client's
  `ApexGoldenBlobs.h` / `ApexUdpGoldenBlobs.h` / `ApexSpectatorGoldenBlobs.h`
  hold the same bytes for its encode/decode tests. A wire change lands on
  both sides in one commit.
- Bump `network::PROTOCOL_VERSION` only for a change an older peer cannot
  survive; the server refuses any other version at `Authenticate`.

## Content layout

```
apexsim-content/
  cars/default/<folder>/car.toml      shipped cars (+ the GLBs they name)
  cars/custom/<folder>/               the player's own (gitignored but its README)
  tracks/default/<Stem>/<Stem>.yaml   shipped circuits, with .ats, .layout.json,
                                      .guide.yml, .dem.msgpack and the sidecars
  tracks/custom/<Stem>/               the player's own tracks, same layout
  wheels/, props/, textures/, artwork/
  build/tracks/                       the exports and previews (generated)
apexsim/
  hud/default/<id>/component.json     HUD components; hud/custom/ overrides
  showcase.yml                        the showcases to render
```

- A track is a folder `X/` holding `X.yaml`; every sidecar path is the YAML's
  path with another extension. Exports (`apexsim-content/build/tracks`, `Game/Tracks`) are
  flat, keyed by stem.
- Every reader walks `default/` then `custom/`: the server
  (`car_loader::car_toml_paths`, `ServerState`), the track tools
  (`ue_export_io::TRACK_DIRS`, `apexsim-content/tools/track_dirs.py`,
  `apexsim-content/tools/lib/ApexTracks.ps1`) and the client
  (`UApexCarContentSubsystem::CarFolders`). A custom car or track reusing a
  shipped id is skipped with a warning, so a player's file can never replace
  a shipped one. A stem or car folder name must be unique across both.
- Every track YAML needs a fixed `track_id`; without one the server mints a
  new UUID per start and no client catalog row can match it.
- Generated outputs (`build/` in both roots, the five sidecars, `game-unreal/Content/`
  beyond the checked-in menu) are gitignored; `.dem.msgpack`, dossiers and
  `.ats` scenes are checked in. See [building.md](building.md).
- `custom/` content ships only with `-IncludeCustomTracks` /
  `-IncludeCustomCars`; it may be converted from content that must not be
  redistributed.

## Lazy track loading

At startup the server parses every track YAML in parallel
(`TrackLoader::load_catalog_entry`), which is all the lobby needs. The
sidecars (ground, curbs, walls, pit, road mesh: hundreds of megabytes across
the calendar) load when the first session on a track is created
(`TrackLoader::load_sidecars`, cached in `track_content::TrackContent`). The
game loop does that on a blocking task (`game_loop/track_loads.rs`) and holds
the creating connection's messages in order meanwhile, so running sessions
never stall. Each sidecar is optional: a missing one is logged and the track
drives without it (the centerline stands in). Tested by
`server/tests/track_content_test.rs`.

## Content checksums

The client draws a track from its export and a car from its GLB while the
server simulates from the YAML and the `car.toml`; a checksum says whether the
two came from the same file. It is CRC-32 (zlib/PNG) over the file's bytes
with every carriage return dropped, so `core.autocrlf` cannot split the sides;
check vector `"123456789"` -> `0xCBF43926`.

- Server: `content_crc::content_crc`, computed at load
  (`CarConfig::content_crc`, `TrackConfig::content_crc`) and sent as
  `ContentCrc` in the lobby's car and track summaries. `SessionSummary`
  carries `TrackId`.
- Track exports: `ats-export` writes the YAML's `source_crc` into the
  manifest, which becomes the runtime catalog row's `SourceCrc`
  (`build_track_catalog.py` writes it into `track_catalog.json` for the
  fallback `DT_TrackCatalog`).
- Cars: `UApexCarContentSubsystem` hashes the `car.toml` it read
  (`ApexContentCrc.h`).
- Compare: `UApexMenuFlowSubsystem::VerifyTrackContent` / `VerifyCarContent`
  when the race director loads a track or spawns the local car. A mismatch is
  a log warning and a toast (`OnContentMismatch`); in a demo it only logs; a
  side with no checksum (0) is "unknown", logged once and never toasted.
  `ReportUnmatchedCatalogIds` lists every stale row on the first `LobbyState`.
- A changed YAML changes its checksum: re-export the track (`ats-export`)
  after any YAML edit, or every client shows a mismatch.
- The predicting client's copy of a track's physics is checked by a wider
  checksum, over the YAML and every sidecar the car step reads
  (`track_bundle::TrackFiles::checksum`, `SessionJoined.Physics.bundle_crc`,
  [server/protocol.md](server/protocol.md#the-own-car-for-prediction)): a
  sidecar of another bake is not the server's either, and the client
  predicts on the server's bundle instead.

Tests: `content_crc::tests`, `network::tests::test_lobby_summaries_carry_content_crc`,
`ApexSim.Content.CrcVector` / `.CrcLineEndings` / `.Compare`.

## Traps

- The client has no physics. Barrier collision, track limits, the pit lane
  and every rule exist only on the server; a client-side "fix" changes nothing
  anyone else sees.
- A wire change that is not pinned by golden bytes on both sides is a silent
  divergence: the other side decodes garbage or drops the frame.
- Running a step of the track pipeline out of order produces internally
  inconsistent data (the dossier and DEM are fitted to the centerline); see
  [apexsim-content/docs/track-pipeline.md](../../apexsim-content/docs/track-pipeline.md).
- A test that hard-codes 240 or 420 breaks the next time the rate moves; use
  `DEFAULT_TICK_RATE_HZ` and dt.
