# Road state on the track: rubber, water, debris and snow

A proposal. Phase 1 is built (2026-10-10) and described as it is in
[../server/conditions.md](../server/conditions.md#the-road-on-screen); phases
2 and 3 (snow) are not. Each phase lands on its own.

## Where we stand

The server already simulates the road ([../server/conditions.md](../server/conditions.md),
`server/src/road_state.rs`): the lap in 10 m cells (`CELL_M`), each cut
across the road into 32 bins of 1 m (`BIN_M`, `HALF_SPAN_M`), and per bin
`rubber`, `marbles` and `dry` (0..1), per cell the water `depth` and the
raceline's lateral. Every wheel pass lays into it, the rain and the sun
step it every `STEP_TICKS`, and `RoadState::sample` is what every tyre
grips on. It is deterministic (positions and clock only) and it costs the
physics nothing it does not already pay. Debris pieces
(`GameSession::debris`, at most `MAX_DEBRIS` 32, `x`/`y`, a life of 90 s)
puncture tyres the same way.

None of it reaches the client. The wire carries one lap-mean figure each
for water and line rubber (`SkyNow.road_water_pct`, `line_rubber_pct`),
the client lerps every road material's `Roughness` toward
`ApexSky::WetRoadRoughness` by that one figure (`ApexRaceDirector::ApplyRoadWetness`),
and that is the whole picture: no rubbered line, no marbles, no dry line
in the rain, no puddles, no debris. `ApplyTrackLevelConditions` still
matches `MI_wear_*` bands the exporter no longer writes (the pipeline paints
no baked rubber line on purpose: the server owns it).

Two facts make drawing it cheap:

- **The road mesh's UV0 `u` is metres of station** (`ue_export.rs`
  `Builder::emit_quad`), which picks the cell. Its `v` turned out to be arc
  length across the strip from its first edge, not a lateral, so phase 1
  sends each cell's centerline point and left vector and the material takes
  the lateral from world position, as the server does.
- **The state is small.** Spa is 700 cells, Le Mans 1362, the
  Nordschleife 2077; at 32 bins and a byte a field that is 22 kB to 66 kB
  a field, 200 kB for the lot on the longest circuit.

Snow is new on both sides: a weather the server has no grip, water or
temperature rules for, and a look the client has no materials for.

## Phase 1: the road state on the wire and on the road (built)

Built on 2026-10-10; see
[../server/conditions.md](../server/conditions.md#the-road-on-screen). What
phases 2 and 3 build on:

- `RoadState` (TCP) and the stream's record 8 `Road` carry whole cells: a
  depth byte, then rubber, marbles and dry per bin, plus each cell's
  centerline point and left vector. Snow must not lengthen those rows (a
  client reads a row as `1 + 3 * Bins` bytes); it goes in a field of its
  own, which an older client skips.
- The client keeps the lap in `FApexRoadStateMap` and uploads two textures;
  `M_ApexTrackRoad` draws them on the road. Snow on the road is a second
  state texture (or a wider texel) in the same graph.

## Phase 2: snow on the server

### The weather

`Weather::Snow = 5`, appended (`ALL` becomes six; the compiler lists every
match: `data.rs`, `drs.rs`, `headlights.rs`, `network.rs`,
`replay_tools.rs`, `spectator.rs`). `SkyNow.weather` is already a `u8`, so
the wire does not change; an old client that gets a 5 shows "Overcast"
(its `WeatherLabel` fallback) and lights accordingly, which is close enough.

The host picks it on the create screen (a sixth tile, `-ApexWeather=snow`).
`clamp` holds the air to at most 2 °C under Snow and `resolve`'s auto air
sits at -4..1 °C. The forecast (`conditions::forecast`) only steps into
Snow from Overcast with the air under 1 °C, and out of it to Overcast; it
never steps from rain to snow, so a changeable summer sky stays rain and a
winter sky swings between overcast and snow.

Figures (`Weather::*`, all new rows, nothing existing moves):

| | Snow |
|---|---|
| `road_grip_factor` | 0.40 (the baked road is a snow-covered road: the racing line and the AI's speed profile are built for it, so the cars crawl as they should from lap one) |
| `curb_grip_factor` / `off_track_grip_factor` | 0.9 / 0.6 of the road's (a snowy verge is a wall of slush) |
| `water` | 0.3 (slush on the line: the treaded tyre's territory, not the full wet's) |
| cloud | 1.0; `through_cloud` 0.2 |
| auto wind | 10 km/h |
| `headlights_needed` | true (as rain over 0.1) |

### The road

`RoadState` gets a per-bin `snow: Vec<f32>` (depth in snow units: 1.0 is
what an hour of steady snowfall leaves, a few centimetres) and
`snowfall: f32` beside `rain` (set by `update_sky` from the live sky: a
Snow sky eases `snowfall` toward 1 over `RAIN_EASE_S` and `rain` to 0). In
`step`, per bin:

- snow rises toward the snowfall's standing depth (`SNOWFALL_RATE`);
- every wheel pass clears `SNOW_CLEAR_PER_PASS` of it from its bin and
  turns what it clears into water (`depth += cleared * SLUSH_WATER`), so
  the line goes from white to slush to wet asphalt as the field laps,
  exactly as `dry` makes a dry line in the rain;
- the sun and a track over 0 °C melt it into water (`melt(track_c, sun)`,
  from the sky as `evaporation` is), under 0 °C nothing melts and the
  water on a cleared line freezes: a bin with water and `track_c <
  FREEZE_C` (-1) gains `ice` (the fourth per-bin field, or a sign on
  `snow`; decide when writing) that only melts above 0.

`RoadSample::grip` multiplies in `snow_grip(snow, ice)`: untouched snow
1.0 (it is the baked 0.40), a cleared bin up to `CLEARED_SNOW_GAIN` 1.5
(0.60 absolute: a cleared line in snow is quicker than the field's first
lap, as a dry line in the rain is), ice 0.5 (0.20 absolute). The water the
tyre sees is the bin's slush, so `Compound::grip_on` and the AI's tyre
crossover (`pit::tyres_for_the_track` on `mean_line_water`) choose
treaded tyres without a snow branch. `TrackSurface::wet` is true under
snow (the road draws heat from the tyres); the tread temperature model
already punishes a tyre that never reaches its window, which at -3 °C it
will not. `rubber` does not lay on a snowy bin (`1 - wet` already gates it;
make it `1 - max(wet, snow)`).

`mean_line_snow()` joins `mean_line_water()` for telemetry and the AI.

No car.toml key is needed: every tyre reads the same road figures, and a
dry session simulates to the bit as before (`snow` stays 0, `snowfall` 0,
every multiplier 1.0). An optional `[tires] snow_grip` per compound
(default 1.0) can come later if a winter tyre is wanted.

### Wire and HUD

- `SkyNow` grows two positional fields at the end: `line_snow_pct` and
  `snowfall_pct` (golden `S_TelemetryCompactSky` regenerated; an old client
  skips them).
- `RoadState` gains a `SnowRows` field (a byte each of snow and ice per
  bin, the same cells as `Rows`), and the stream's `Road` record an element
  at its end; the existing rows keep their layout, so an older client skips
  the snow and still draws the rest.
- HUD data points `sky.snow`, `sky.snowfall` on every `ApexHudData::Build`
  path and in [../game/hud-modding.md](../game/hud-modding.md).

### Checking phase 2

- `track_evolution_test`: a Snow session's road starts at 0.40, the AI
  field clears a line that grips 1.5x in twenty laps, the line freezes at
  -3 °C overnight on a 60x clock and melts by noon, a session seeded the
  same replays the same.
- The AI survey at `SURVEY_WEATHER=snow` (add the env hook beside
  `SURVEY_WIND_KPH`): the field must finish without pile-ups at 0.40
  grip, which is the real test of the racing line's envelope at low grip.
  Compare the dry survey to the commit before: it must not move.
- Determinism test bit-identical.

## Phase 3: snow on the client

- **Falling snow**: `AApexRainActor` gets a flake mode (`SetSnow(Intensity)`):
  slower fall (`FallSpeedCmPerS` 120), a short fat streak, white, unlit,
  more drift with the wind. Same actor, same box, no particle assets.
- **The road**: the texture's second map (or the A channel once water moves
  to a 1D depth strip) carries snow and ice per bin. Snow lerps the road
  albedo to a cool white, roughness 0.8, normal flattened; slush is the wet
  look with a grey-white tint; ice is the puddle look (roughness 0.05)
  without the darkening. The cleared line appears as the field laps: dark
  asphalt between white edges.
- **Everything else**: a `SnowCover` scalar on every non-road track MID
  (grass, gravel, sand, run-off, the terrain and the horizon, the curbs),
  lerping albedo to white and roughness to 0.8 by a world-up mask in
  `M_ApexTrackBase` (so only up-facing ground whitens); props keep their
  look (a top-down snow blend in the prop materials is a later item).
  `ApplyTrackLevelConditions` sets it from `Sky.SnowCover`
  (`ApexSky::Derive` for a fixed sky, `DeriveLive` eased toward
  `snowfall` and `line_snow`).
- **The sky**: `ApexSky::Derive` gets the Snow row (swing 0.4, offset
  -2.5, through-cloud 0.2, fog as heavy rain, a cooler white balance,
  desaturated); headlights and floodlights as rain.
- **Spray**: none in phase 3 (rain spray does not exist either; one item
  for both, below).
- Create screen: the Snow tile (`ApexCreateSession` weather count 6,
  `EApexWeather::Snow = 5`, `WeatherLabel`); `-ApexWeather=snow`.
- Tests: `ApexSim.Sky.Snow` (derive figures), `ApexSim.UI.CreateSession.SkyChips`
  with six tiles, HUD `Stable`/`Documented`.
- On screen, unattended: `-ApexAutoRace -ApexWeather=snow -ApexTimeOfDay=10:00`
  from a TV camera after ten AI laps: white verges, a dark cleared line,
  flakes drifting with the wind.

## Order and what each phase needs

| Phase | Needs | Lands on its own |
|---|---|---|
| 1 wire + drawing | nothing | built 2026-10-10 |
| 2 snow server | phase 1 only for the rows' layout (the sim itself needs nothing) | yes: snow races on the server, the client shows overcast and a dark line |
| 3 snow client | 2 | yes |

Phase 2 can start now; phase 1 needs a client build and a look on screen
first (see the roadmap's "Built but never checked by hand").

## Left for later (goes into the gap lists when a phase lands)

- The server holds one water depth per cell: puddles have no lateral
  position (a road crown or camber per bin would put them at the low edge).
- Rain and snow spray behind the cars, on screen and as a visibility
  penalty for the AI's look-ahead.
- The AI drives the racing line whatever the road does: it does not seek
  the dry or the cleared line, nor the rubber (an existing gap).
- Snow on props, grandstands and trees; snow banks pushed off the line at
  the road edge; marshals clearing the run-off.
- Gritting and salting as a session rule; a winter tyre compound with a
  car.toml `snow_grip`.
- Sleet (snow at 1..3 °C that lands as water) and hail.
- A replay seek that lands mid-lap shows the road as of the last `Road`
  record before it, which can be a lap old on the Nordschleife; a file
  could carry a full-lap keyframe per block.
