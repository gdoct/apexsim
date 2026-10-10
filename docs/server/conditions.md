# Conditions: weather, time of day, air and road

A session's host picks a sky (weather, hour, air, wind, how the day and the
weather move, how rubbered the track is). The server resolves it, bakes what
the physics needs into the session's own copy of the track, steps a live sky
and a road state through the session, and sends the result to every client
in telemetry. The client only lights its world by it: the grip is the
server's.

Everything here is a function of the session's id, its clock and the cars'
positions, so a session replays the same (`a_changing_sky_is_deterministic`).

## Code

- `server/src/data.rs` - `SessionConditions` (the host's pick: `clamp`,
  `resolve`, `apply_to_track`, `air_density_ratio`, `headlights_needed`),
  `Weather` (the per-weather grip, water and cloud figures), the sun
  (`sun_elevation_deg`, `solar_gain_c`, `through_cloud`,
  `DEFAULT_LATITUDE_DEG`), `TrackSurface` (what is baked: grip, `wet`,
  `water`, `baked_water`, air and track temperature, `air_density_ratio`,
  `wind_mps`, `wind_now_mps`).
- `server/src/conditions.rs` - `LiveConditions` (the sky now) and `forecast`.
- `server/src/road_state.rs` - `RoadState`: rubber, marbles, a dried line,
  water depth; `write_rows` / `write_geometry` pack it for the wire.
- `server/src/game_loop/dispatch.rs` - `road_state_message`,
  `road_state_full` (the join burst); `game_loop/tick.rs` the round-robin
  send, `broadcast.rs` `broadcast_road`.
- `server/src/wind.rs` - `gusting` and `hash01` (the hash the AI and
  racecraft use too).
- `server/src/headlights.rs` - whose lights are on.
- `server/src/game_session.rs` - `with_road`, `live_sky`, `update_sky`
  (top of every tick, acts once a second), `update_wind` and `update_road`
  (in `update_air`).
- `server/src/physics.rs` - `calculate_aerodynamic_forces` (airspeed against
  `wind_now_mps`, density), `engine_density_factor`, the per-wheel road
  sample in `update_car_3d`.
- `scripts/track_location.py` - a circuit's altitude and position.
- Client: `ApexSimNet` (`FApexSessionConditions`, `FApexSkyNow`,
  `FApexRoadState`), `ApexSim/Public/Race/ApexSkyModel.h` (`ApexSky`),
  `ApexRaceDirector` (the road state in `ApexRaceDirectorRoad.cpp`),
  `Race/ApexRoadStateMap.h`, `ApexRainActor`, `ApexStreetLights.h`; the
  road material `M_ApexTrackRoad` (`ApexTrackMaterialGraphs.cpp`,
  `ApplyRoadStateGraph`).

## What the host picks (`SessionConditions`)

Sent inside `CreateSession.conditions`, kept on `RaceSession::conditions`,
echoed in `SessionJoined.Conditions` and listed in every `SessionSummary`.

| Field | Meaning | Absent |
|---|---|---|
| `weather` | sunny, cloudy, overcast, light rain, heavy rain | sunny |
| `time_of_day_minutes` | local clock, wrapped onto one day | 13:00 |
| `air_temp_c` | -5..45 °C | from weather and hour |
| `humidity_pct` | 0..100 (no host control; auto only) | from weather |
| `wind_kph` | 0..60, the mean it gusts about | from weather |
| `wind_from_deg` | from the start straight's direction: 0 head-on, 90 from its left | hashed from the session id |
| `time_scale` | 0 the clock holds, 1 real time, up to 60 | holds |
| `changeable` | 0 the sky holds, 1 settled, 2 changeable, 3 stormy | holds |
| `track_rubber_pct` | rubber on the line at the start: 0 green, 50 calibrated, 100 rubbered | 50 |

Every optional field is skipped on the wire when `None`, so an old client's
create and an old server's join keep their bytes. `clamp` holds each figure
to its range. `create_session` (`server.rs`) calls `resolve(session id)`,
which names every air figure (auto values: air 13-23 °C over the day, damped
under cloud, in whole degrees; humidity 50-97%; wind 8-28 km/h by weather),
so the join and the listing always say what is simulated. `replay_tools`
seeds the same resolution from `--seed`.

## The weather's grip (the bake)

`create_session` then calls `apply_to_track` on the session's copy of the
track: every centerline sample's `grip_modifier`, `track_surface.base_grip`
(which the racing line and the AI's speed profile read), `curb_grip` and
`off_track_grip` are multiplied by the weather's factors
(`Weather::road_grip_factor`: overcast 0.98, light rain 0.86, heavy 0.74;
`curb_grip_factor` and `off_track_grip_factor` on top in the rain). It also
sets the temperatures, `wet`, `water` / `baked_water` (0 dry, 0.5 light, 1
heavy: which tyre grips is read off it, see
[vehicle-physics.md](vehicle-physics.md)), the air density ratio and the
wind turned onto the track. Physics, AI and racing line then follow with no
branch in the tick, and a dry, sunny session is the track file to the bit
(the bake returns early when the factors are 1).

## The air

- **Density**: `air_density_ratio` is the moist-air density (standard
  atmosphere at the track's `metadata.altitude_m`, Tetens vapour pressure)
  over the reference day every car.toml is filed at (sea level, sunny
  13:00), exactly 1.0 there. It scales the aero and the racing line's
  envelope, and the combustion torque through
  `physics::engine_density_factor`: all of it for a naturally aspirated
  engine, `TURBO_DENSITY_SHARE` (0.25) of it for one with `[engine]
  forced_induction` (an `[engine.turbo]` table implies it). Mexico City
  is about 0.78.
- **Wind**: `SessionConditions::wind_mps` is in the start straight's frame;
  `apply_to_track` rotates it by the first centerline point's heading
  (real circuits keep their real orientation, so the straight is not +X)
  into `TrackSurface::wind_mps`. Each tick `update_wind` sets
  `wind_now_mps = wind::gusting(mean, session seconds)`: hash-based smooth
  noise, speed +-35% (`GUST_SHARE`), veering +-15° (`GUST_VEER_DEG`),
  gusts about 4 s long.
- **Physics through the air**: downforce from the flow along the car, drag
  along the relative airflow, and a side force on the flank from air
  crossing it. `CarState::aero_load_share` (tyre load with this downforce
  over the load in still, clean air at this ground speed, capped at 1) is
  what the AI drives to, so a tailwind into a corner or dirty air slows it
  ([ai.md](ai.md)).

## Where a circuit is

The track YAML's `metadata.altitude_m`, `latitude_deg` and `longitude_deg`
are written by `python scripts/track_location.py --all` from each
`<Stem>.dem.msgpack` georeference (altitude = first node's z less the DEM's
datum offset), with a `MANUAL` table for circuits without a DEM. The track
editor's `TrackMetadata` carries the keys through its rewrites
(`the_location_survives_a_rewrite_as_written`). Editing a YAML changes its
content checksum: re-run `ats-export --all` after.

The server works the sun out at `TrackConfig::latitude_deg()` (50° N when
the YAML has none) for the track temperature, the headlights and the
evaporation; the model day is late May with solar noon at 13:00, the
client's `ApexSky::SunAt` to the letter. `ats-export` writes `latitude_deg`
and `north_yaw_deg` (true north in the track frame, `DemGeoref::north_yaw_deg`)
into the export manifest's `metadata`, so the client puts the sun where it
stands over the real circuit; re-export a track for the client to see a
change.

## The live sky (`conditions.rs`)

`GameSession::sky` is a `LiveConditions`, built by `live_sky` from the
resolved conditions, the track's latitude and altitude and the session id.
`update_sky` runs at the top of every tick and acts once a second:

- The clock runs at `time_scale` from the host's hour.
- With `changeable` set the weather follows `forecast(start, session id)`:
  one weather step at a time (sometimes two above settled), drawn back
  toward the host's pick, a stormy sky leaning wet, never sooner than 4
  minutes of session apart however fast the clock runs.
- Rain eases toward the weather's over `RAIN_EASE_S` (3 min), cloud over
  `CLOUD_EASE_S` (5 min). The air follows the hour and the cloud with the
  host's offset kept (`AIR_EASE_S`); the asphalt lags over `TRACK_EASE_S`
  (15 min of day clock) toward the air plus the sun through the cloud, and
  sits a degree under the air while the road holds water.
- Each step bakes onto the session's track: air and track temperature, the
  air density, `TrackSurface::water` (the road's mean water: the tyre the
  track calls for) and `wet` (`conditions::WET_FROM_WATER`, 0.05), and hands
  the falling rain to the road (`RoadState::rain`).

A session whose clock and sky both hold (`is_static`) bakes nothing after
creation: it is the static bake above, plus the road state.

## The road (`road_state.rs`)

Every session's track carries a `RoadState` (`TrackConfig::road_state`,
built by `with_road` from the baked water and the start's rubber). The lap
is cut into 10 m cells (`CELL_M`), each cut across the road into 1 m bins
(`BIN_M`, +-16 m, held to the road's width). A car crossing into a new cell
(`update_road`; not in the garage, towed or under 3 m/s) lays a pass at each
of its two wheel paths (`RoadState::lateral_of`, the car's middle +- half its
track) and sheds marbles by how hard it corners (`shed_for_lateral_g`). The
state steps every `STEP_TICKS`, draining by the sky's `evaporation()`.

Per bin:
- **rubber** 0..1: laid by dry passes, washed by rain. At the start the
  raceline carries the host's level flat `START_LINE_HALF_M` (2 m) either
  side and fading beyond, and the road off the line the lesser of that
  level and `RUBBER_REFERENCE` (0.5, what the cars are calibrated on). So a
  default start grips exactly as filed from edge to edge.
  `rubber_grip`: +3% a unit over the reference in the dry, greasy in the
  wet.
- **marbles** 0..1: shed over the whole width, swept from a bin by each
  wheel through it, washed by rain; up to `MARBLE_GRIP` (15%) off. They
  gather off the line in the corners.
- **dry**: how much of the cell's water the wheels have wiped there,
  rewetted by the rain, never past `line_dry_ceiling(rain)`.

Per cell, the **water depth**: fed toward the falling rain, deeper by
`PUDDLE_GAIN` where the road lies `LOW_DEPTH_M` below its surroundings along
the lap, drained above that. Water over 1 is standing water.

`RoadState::sample(station, lateral)` is what a tyre finds: `water` (for
the compound's grip), `grip` (water against the baked water, times rubber
and marbles: the racing surface), `wet_grip` (water alone: run-off, pit
lane), `curb_grip`, `off_grip`. `update_car_3d` reads it per wheel by
contact class, and at the car's middle into `CarState::surface_grip_share`,
which the AI reads through `tyre_grip_share`. `mean_line_water` drives the
AI's tyre crossover ([ai.md](ai.md)).

## The road on screen

The server sends the road state as it is, and the client draws it on the
road: a darker, glossier rubbered line, marbles as dark crumbs beside it, a
dry line between wet edges, mirror-flat puddles, and the debris lying on it.

**On the wire.** `RoadState` (TCP, named, droppable) carries a slice of
whole cells from `FirstCell`, wrapping the lap. Each cell is `ROW_BYTES`:
its water depth as a byte (`DEPTH_BYTE`: 100 is heavy rain on the flat,
over 100 a puddle), then for each of the `BINS` bins its rubber, marbles and
dry, each a byte over 0..1. `Geometry` gives each cell's centerline point and
the unit vector to its left (four `f32`, server frame): the point and vector
`RoadState::lateral_of` measures a bin from. `Debris` lists every piece on
the road now. A joining driver, spectator or rejoiner gets the whole lap at
once (`road_state_full`, `ROAD_CELLS_PER_JOIN` cells a message), as does
the driver who creates a session and the host of a demo backdrop or a
watched hotlap. While the
session is live (countdown, race, finish) every session sends the next
`ROAD_CELLS_PER_SEND` (50) cells every `ROAD_SEND_SECONDS` (2 s), picked
from the loop's tick, so there is no per-session cursor. Monza refreshes
round the lap in about 23 s, the Nordschleife in about 83 s. Sending reads
the state and never steps it, so the sim is untouched.

A spectator stream carries the same payload as record 8 `Road`
([../game/spectator.md](../game/spectator.md)): `render_stream` writes the
whole lap with the first frame and a round-robin slice every 2 s after, and
the client's `UApexReplayRecorder` writes every slice it receives (the
join's burst under the first frame's tick).

**On the client.** `UApexNetSubsystem::OnRoadState` fires for each live
slice (`FeedBackdropRoad` for a stream's). The race director keeps the lap
in an `FApexRoadStateMap` from the join on, across race views, and forgets
it when the session is left, a new backdrop begins, or a slice comes from
another source (another session id, or a stream's empty one). Each frame it
uploads the map into two transient textures:

- `RoadState`: a column a cell, a row a bin; R rubber, G marbles, B dry, A
  the depth byte. Bilinear, wrapping round the lap.
- `RoadGeometry`: a texel a cell, the cell's point in world cm and its left
  vector in Unreal axes (the server's Y flipped). Nearest, so a pixel uses
  its own cell's point, as the server does.

Only the road itself (`family` `road`) is made from `M_ApexTrackRoad`, the
base graph plus `ApplyRoadStateGraph`; the pit lane, curbs and bands stay
on `M_ApexTrackBase` and pay nothing. The road mesh's UV0 `u` is metres of
station, which picks the cell (`RoadStateU` = 1 / (cell length x cells));
`-dot(world - point, left)` is the lateral, which picks the bin. Its `v` is
arc length across the strip from its first edge, not a lateral, so it is not
used. `RoadStateAmount` is 0 until the first slice arrives, which leaves the
road as built. While the road state is drawn, `ApplyRoadWetness` leaves the
road's own materials to it and wets only the pit lane by the lap's mean.
Debris is an instanced shard of the engine cube per piece, traced down onto
the track (`UpdateRoadDebris`).

A bake from before `M_ApexTrackRoad` existed builds the road on the base
parent and draws no road state; `-run=ApexMaterialBake` bakes it beside the
base. Two console switches compare its cost: `apexsim.road.Draw 0` stops the
director drawing it (the road as built, the lap's wetness back on it), and
`apexsim.track.RoadMaterial 0` builds the next track's road on the base
parent, with no road-state shader at all. Each time the road's materials take
the textures the log says `Road state to N road material(s): drawn, K of M
cells known, B bins rubbered past 0.8`.

## Headlights

The server decides, because everyone sees them (`headlights::update`, each
tick): `PlayerInput.headlights` is the driver's switch (nil until first
touched), `PlayerInput.flash` the held flash button; an untouched switch,
the AI and an old client get `LiveConditions::headlights_needed` (sun under
6° at the track's latitude, or rain over 0.1). Telemetry carries on / flash
as `lap_flags` bits 5 and 6 (`LAP_FLAG_HEADLIGHTS`,
`LAP_FLAG_HEADLIGHT_FLASH`).

## Wire

- `CreateSession.conditions`, `SessionJoined.Conditions`,
  `SessionSummary` - golden bytes: `cargo test --lib --
  assists_wire_format conditions_air_wire_format sky_wire_format
  --nocapture` ->
  `ApexGolden::C_CreateSessionAir` / `S_SessionJoinedAir` /
  `C_CreateSessionSky` / `S_SessionJoinedSky`.
- `CompactTelemetry.sky` (`network::SkyNow`, 13 positional fields: clock,
  weather, rain, cloud, road water, air, track, the wind now and where it
  blows toward, line rubber, time scale, next weather and when), appended
  after `race_clock` (nil then when there is no race clock); every frame.
  Golden: `ApexUdpGolden::S_TelemetryCompactSky`.
- `PlayerInput.headlights` / `flash`: `cargo test
  player_input_headlights_wire_format -- --nocapture` ->
  `ApexUdpGolden::C_PlayerInput`.
- `RoadState`: `cargo test road_state_wire_format -- --nocapture` ->
  `ApexGolden::S_RoadState`; the stream's `Road` record in
  `spectator_wire_format` -> `ApexSpectatorGolden::Road`.

See [protocol.md](protocol.md) for the append-only rules.

## Client

- `FApexSessionConditions` (net module: `Describe`, `ClockText`,
  `WeatherLabel`; unset optional fields are -128 / -1 and written only when
  picked), the flow's `CreateConditions` (kept on the profile),
  `UApexNetSubsystem::GetSessionConditions` and `GetLatestSky`
  (`FApexSkyNow` on `FApexTelemetryFrame::Sky`).
- The create screen's Conditions tab: weather tiles, time-of-day slider,
  air temperature stepper with AUTO, wind strength list and an eight-way
  direction dial, Clock (frozen to 60x), Weather changes and Track rubber
  rows.
- The race director lights by `ApexSky::Derive` (static sky) or
  `ApexSky::DeriveLive` (blended from the server's cloud and rain,
  relit only when `LiveSkyMoved` says so); `FSkySite` (latitude, north from
  the catalog row's `bHasLocation` / `LatitudeDeg` / `NorthYawDeg`) turns
  the sun's bearing into the track frame. A director-owned unbound
  post-process volume carries exposure, wet desaturation and bloom.
- `ApplyTrackLevelConditions` (once the track is built and visible): fog
  by weather, the road family's material roughness eased toward
  `ApexSky::WetRoadRoughness` (0.3) with the road's water (the road itself
  per bin from the road state once it arrives, above), the racing-line
  dots wet (`AApexRacingLineActor::SetWet`), kit flag poles (`SM_flag_pole`)
  turned downwind. After dark the lamp props get real lights, the nearest
  `apexsim.lights.Max` (90) within `apexsim.lights.Range` of the camera
  (`ApexStreetLights.h`), and the emissive lamp faces glow.
- Rain is `AApexRainActor`: up to 1500 streaks of the engine cube in a box
  that follows the camera, drifting with the wind (no particle assets).
- Car headlights: `AApexRaceCarActor::SetHeadlights`
  (`apexsim.car.HeadlightLumens`), from the telemetry bits; a flash is held
  at least 0.2 s so a tap shows.
- HUD data points `sky.*` and the `conditions` component (off by default):
  [../game/hud-modding.md](../game/hud-modding.md).
- Unattended: `-ApexAutoRace -ApexWeather=heavyrain -ApexTimeOfDay=22:15
  -ApexTimeScale=N -ApexChangeable=N -ApexTrackRubber=N`.

## Checking it

```bash
cd server
cargo test --test session_conditions_test     # echo, listing, the grip bake
cargo test --test air_test                     # density, turbo, wind, the AI in a gale
cargo test --test track_evolution_test         # rubber and marbles, forecast rain and the AI's crossover, 60x clock, determinism
cargo test --lib -- conditions:: road_state:: wind:: road_state_wire_format a_render_carries_the_road_state
SURVEY_WIND_KPH=25 cargo test --release --test ai_race_start_test survey_ai_races_on_every_circuit -- --ignored --nocapture
```

Client: `ApexSim.Sky.*` (incl. `.Site`, `.Live`), `ApexSim.Hud.Data.Sky`,
`ApexSim.UI.CreateSession.SkyChips`, `ApexSim.Race.RoadStateMap` (the
lateral against the server's), the golden decode tests
(`ApexSim.Net.Protocol.GoldenDecode` for `S_RoadState`, the spectator
stream tests for `Road`).

On screen, unattended: `-ApexAutoRace -ApexTrackRubber=0` against
`-ApexTrackRubber=100` from a TV camera shows the line light against dark;
`-ApexWeather=lightrain -ApexChangeable=0` after some AI laps shows the dry
line. The log says `Road state: N cells x 32 bins` when the textures are
made and `Road state to N road material(s): drawn, ...` when the road takes
them (above). The low camera barely shows the line; look from above, and a
screenshot with `apexsim.road.Draw 0` at the same pose diffs to the band.

## Traps

- **Grip lives in the session's copy of the track**, baked at creation and
  stepped by the live sky. Never branch on the weather in the tick; change
  the bake or the road state.
- **Every grip figure in `RoadState::sample` is relative to the baked
  water** (`baked_water`): the weather's road factor is already in the
  centerline samples. Multiplying the absolute wet grip on top counts the
  rain twice.
- **`RUBBER_REFERENCE` is the calibration point.** A default start must
  grip exactly as filed everywhere, including off the line; leaving the
  road off the line green at a default start pushed the AI wide in traffic
  and raised its off-road time.
- **Two `WET_FROM_WATER` constants**: `conditions::WET_FROM_WATER` (0.05,
  when the road counts as wet) and `tyre_thermal::WET_FROM_WATER` (0.75,
  where the wet tyre takes over from the intermediate). Import the one you
  mean.
- **Optional fields stay optional on the wire.** A new `SessionConditions`
  field must be `Option` with `skip_serializing_if`, or every old client's
  create changes bytes and the goldens break.
- **The wind's direction is relative to the start straight**, not +X; the
  rotation happens in `apply_to_track`.
- A changed YAML (location keys included) changes the track's content CRC;
  re-export or the client warns of a mismatch.
