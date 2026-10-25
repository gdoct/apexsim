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

- `server/sim/src/data.rs` - `SessionConditions` (the host's pick: `clamp`,
  `resolve`, `apply_to_track`, `air_density_ratio`, `headlights_needed`),
  `Weather` (the per-weather grip, water and cloud figures), the sun
  (`sun_elevation_deg`, `solar_gain_c`, `through_cloud`,
  `DEFAULT_LATITUDE_DEG`), `TrackConfig::climate_site`, `TrackSurface` (what is baked: grip, `wet`,
  `water`, `baked_water`, air and track temperature, `air_density_ratio`,
  `wind_mps`, `wind_now_mps`).
- `server/sim/src/conditions.rs` - `LiveConditions` (the sky now) and `forecast`.
- `server/sim/src/climate.rs` - `ClimateSite`, `climate_of` (the regions),
  `climate_offset_c`, `wet_season_pct`: where a dated session's air and
  humidity come from.
- `server/sim/src/road_state.rs` - `RoadState`: rubber, marbles, a dried line,
  water depth; `write_rows` / `write_geometry` pack it for the wire, and
  `read_rows` / `read_snow_rows` / `read_lane_snow` read the bytes back into
  another copy of the road (the physics library's `apx_track_road_rows`, for
  a predicting client).
- `server/server/src/game_loop/dispatch.rs` - `road_state_message`,
  `road_state_full` (the join burst); `game_loop/tick.rs` the round-robin
  send, `broadcast.rs` `broadcast_road`.
- `server/sim/src/wind.rs` - `gusting` and `hash01` (the hash the AI and
  racecraft use too).
- `server/sim/src/headlights.rs` - whose lights are on.
- `server/sim/src/game_session.rs` - `with_road`, `live_sky`, `update_sky`
  (top of every tick, acts once a second), `update_wind` and `update_road`
  (in `update_air`).
- `server/sim/src/physics.rs` - `calculate_aerodynamic_forces` (airspeed against
  `wind_now_mps`, density), `engine_density_factor`, the per-wheel road
  sample in `update_car_3d`.
- `apexsim-content/tools/track_location.py` - a circuit's altitude and position.
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
| `weather` | sunny, cloudy, overcast, light rain, heavy rain, snow | sunny |
| `time_of_day_minutes` | local clock, wrapped onto one day | 13:00 |
| `air_temp_c` | -5..45 °C (at most 2 under snow) | from weather and hour |
| `humidity_pct` | 0..100 (no host control; auto only) | from weather |
| `wind_kph` | 0..60, the mean it gusts about | from weather |
| `wind_from_deg` | from the start straight's direction: 0 head-on, 90 from its left | hashed from the session id |
| `time_scale` | 0 the clock holds, 1 real time, up to 60 | holds |
| `changeable` | 0 the sky holds, 1 settled, 2 changeable, 3 stormy | holds |
| `track_rubber_pct` | rubber on the line at the start: 0 green, 50 calibrated, 100 rubbered | 50 |
| `sky_seed` | the seed the forecast and the wind's direction are laid out from (`sky_seed_or`) | the session's id |
| `day_of_year` | the date, 1..365: the sun's declination and summer time | the model day, 25 May (declination 20°) |
| `solar_noon_min` | solar noon on the session's clock: the server's, from the track (`at_site`); a create's own is replaced | 13:00 |
| `forecast` | the host's own forecast: up to 10 `[at_min, weather]` steps (`HostForecast`), used instead of the seeded one | the seeded one, by `changeable` |

Every optional field is skipped on the wire when `None`, so an old client's
create and an old server's join keep their bytes. `clamp` holds each figure
to its range. `create_session` (`server.rs`) calls `resolve(session id)`,
which names every air figure (auto values: air 13-23 °C over the day, damped
under cloud, in whole degrees; humidity 50-97%; wind 8-28 km/h by weather; snow's air -4 to 1 °C, wind
10 km/h, humidity 85%),
so the join and the listing always say what is simulated. Before it,
`at_site(track)` names `solar_noon_min` from where the circuit is (below),
so the weather's own air follows the circuit's sun, and `resolve_at` takes
the track's latitude so it follows the season of a dated session (below). `replay_tools` seeds
the same resolution from `--seed`.

## The weather's grip (the bake)

`create_session` then calls `apply_to_track` on the session's copy of the
track: every centerline sample's `grip_modifier`, `track_surface.base_grip`
(which the racing line and the AI's speed profile read), `curb_grip` and
`off_track_grip` are multiplied by the weather's factors
(`Weather::road_grip_factor`: overcast 0.98, light rain 0.86, heavy 0.74,
snow 0.28; `curb_grip_factor` and `off_track_grip_factor` on top in the
rain). It also sets the temperatures, `wet`, `water` / `baked_water` (0 dry,
0.5 light, 1 heavy, 0.3 under snow: which tyre grips is read off it, see
[vehicle-physics.md](vehicle-physics.md)), the air density ratio and the
wind turned onto the track, and `baked_snow` (1 under snow: the road was
baked covered). Physics, AI and racing line then follow with no
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
- **The wind and the damp follow the weather**: under a changing sky the
  mean (`LiveConditions::wind_kph`) eases over `WIND_EASE_S` (3 min) toward
  the weather's own (`auto_wind_kph`) times the share of the start
  weather's own the host picked (`wind_target_kph`: a calm pick stays calm,
  twice the breeze is twice the storm's), and the humidity toward the
  weather's own plus the host's offset (`humidity_target`), which the air's
  density reads. `update_sky` bakes the mean onto the track
  (`data::wind_onto_track`, the same turn as the bake's) once it has moved
  off the start's figure, so a sky that keeps its weather keeps the bake's
  wind to the bit. A front of heavy rain on a sunny start brings 8 km/h up
  to 28.
- **A front turns the wind**: each change of weather in the forecast turns
  the mean's direction (`LiveConditions::veer_target_deg`): a change to
  wetter weather (a front coming in) backs it, a change to drier (the
  front through, the sky clearing) veers it, by `FRONT_VEER_MIN_DEG` (12°)
  plus up to `FRONT_VEER_SPREAD_DEG` (23°) hashed from the sky's seed and
  the change. The turn already taken is kept at three quarters, so a run of
  fronts wanders about the host's direction, never past
  `FRONT_VEER_MAX_DEG` (90°); south of the equator the same weather turns
  it the other way. `wind_veer_deg` eases toward it over `WIND_EASE_S`, and
  `wind_from_deg()` / `wind_mps()` read it (a veer is clockwise from
  above, so it takes the frame's anticlockwise angle down); `update_sky`
  bakes it with the mean. It is 0 to the bit until the weather first
  changes. The tow, the aero and telemetry's `wind_to_deg` (the client's
  flags and the rain's drift) all read `wind_now_mps`, so they turn with it.
- **Physics through the air**: downforce from the flow along the car, drag
  along the relative airflow, and a side force on the flank from air
  crossing it. `CarState::aero_load_share` (tyre load with this downforce
  over the load in still, clean air at this ground speed, capped at 1) is
  what the AI drives to, so a tailwind into a corner or dirty air slows it
  ([ai.md](ai.md)).

## Where a circuit is

The track YAML's `metadata.altitude_m`, `latitude_deg` and `longitude_deg`
are written by `python tools/track_location.py --all` (in `apexsim-content/`) from each
`<Stem>.dem.msgpack` georeference (altitude = first node's z less the DEM's
datum offset), with a `MANUAL` table for circuits without a DEM. The track
editor's `TrackMetadata` carries the keys through its rewrites
(`the_location_survives_a_rewrite_as_written`). Editing a YAML changes its
content checksum: re-run `ats-export --all` after. An AC import gets all
three for AC's own circuits from the importer's table, and for a mod with
real geotags their position and the Copernicus height there; its manifest
carries `north_yaw_deg` by AC's -z convention or a measured figure
([apexsim-content/docs/ac-import.md](../../../apexsim-content/docs/ac-import.md), "Where the circuit is").

The server works the sun out at `TrackConfig::latitude_deg()` (50° N when
the YAML has none) for the track temperature, the headlights and the
evaporation (`SessionConditions::sun_elevation_at`), the client's
`ApexSky::SunAt` to the letter:

- **The date** is the host's `day_of_year` (`sun_declination_deg`, Cooper's
  formula: +23.4° at midsummer, -23.4° at midwinter), else the model day,
  25 May, at exactly the old 20° declination.
- **The climate** moves the weather's own air with the date and the place
  (`server/sim/src/climate.rs`). The weather's own air is filed at a
  reference day: late May, 50° N, on the sea (`ClimateSite::REFERENCE`). A
  dated session is moved from there by `SessionConditions::climate_offset_c`
  (`climate::climate_offset_c`), the day's mean air at the circuit
  (`climate::mean_air_c`) less the reference's:
  - the latitude's sea-level annual mean (`sea_level_mean_c`: 27 °C on the
    equator, falling with the square of the latitude, 17 °C less at 50°);
  - the season, a cosine over the year (`data::season_air_c`), warmest on
    19 July (`WARMEST_DAY_OF_YEAR`) north of the equator and half a year
    on south of it, swinging half the latitude's annual range either way
    (`SEASON_RANGE_PER_DEG_C` 0.35 °C a degree of latitude, up to 20);
  - the region's climate (`climate::climate_of`, a coarse map by latitude
    and longitude like `legal_clock`'s): the season's range times
    `range_factor` and the mean moved by `mean_offset_c`. Atlantic Europe
    0.7 and +1.5 °C (the Gulf Stream), the Mediterranean 1.15, central
    Europe 1.2, the Gulf 1.8 and +4, Anatolia and the Caspian 1.4 and -2,
    East Asia 1.8 and -3, Florida 1.15, North America east of the Rockies
    1.7 and 0.4 °C colder a degree north of 30°, the Pacific coast 0.7 and
    -2, the interior west 1.8, the rest of the tropics 0.6, the southern
    mid-latitudes 0.8; a track without a longitude is the reference's;
  - the lapse with the circuit's `metadata.altitude_m`
    (`LAPSE_C_PER_KM` 5 °C a kilometre, the surface's: Mexico City is 11 °C
    under the sea level of its latitude).
  The monthly means hold the circuits' climate normals within 3.5 °C
  (`the_climate_follows_the_circuit`: Silverstone, Spa, Monza, Barcelona,
  the Hungaroring, Bahrain, Suzuka, Shanghai, Austin, Montréal, Melbourne,
  Singapore, Mexico City, Interlagos). `resolve_at(seed, site)` (the
  server's create, the replay tools, a weekend day; the site is
  `TrackConfig::climate_site`, or a bare latitude at sea level) names the
  auto air from `auto_air_temperature_c_at`, held to the host's -5..45 °C,
  and the humidity from `auto_humidity_pct_at`: the season's part alone
  (`season_offset_c`, the cosine by the region's range against the model
  day there) makes a degree of seasonal cold a percent damper
  (`SEASON_HUMIDITY_PER_C`, 20% at the driest), and in the tropics the wet
  season adds to it (`climate::wet_season_pct`: `WET_SEASON_HUMIDITY_PCT`
  12% from the dry season to the wet, wettest a month after midsummer, none
  within 3° of the equator, which is wet all year, full from 10° to 22°,
  none past 30° and none in the Gulf's desert). The live sky's air
  (`LiveConditions::at`, over the same site) follows the same. A sunny
  15:00 at Silverstone is 11 °C in mid-January, 21 °C in late May and 24 °C
  in mid-July. Snow's own air is the same near freezing in any month and
  place; what the climate moves is the snow line, how far an overcast sky
  stands above it (`snow_air_drop_c`: 17 °C on the reference day, less in
  winter or up a mountain), so a January overcast night at 50° N resolves
  under 1 °C and is a winter sky. With no date every figure is the
  reference day's to the bit wherever the circuit is, and a dated model day
  at the reference site is too
  (`a_date_moves_the_air_with_the_season_and_no_date_is_late_may`): an
  undated session, the AI survey's and the calibrations', is unmoved.
- **Solar noon** is `data::solar_noon_minutes(latitude, longitude, day)` for
  a track whose YAML names `longitude_deg`: 12:00 moved by the circuit's
  distance from its zone's meridian, summer time and the equation of time.
  The zone comes from `legal_clock`, a coarse map of where racing happens
  (Britain, Ireland and Portugal on Greenwich time, the rest of western
  Europe on Central European time, Eastern Europe, Singapore and Malaysia,
  Mexico, Argentina, the US and Canadian zones by longitude, Australia's
  with summer time in the south-east), else the meridian's own hour with no
  summer time (the Gulf, Asia, Japan, Brazil). Summer time runs by the
  European, North American or southern rule on the date. On the model day
  Silverstone's noon is 13:01, Spa's 13:33, Monza's 13:20, Suzuka's 11:51,
  Austin's 13:27, Melbourne's 12:17 (`solar_noon_stands_where_the_circuit_is`,
  within three minutes of the almanac). A track without a longitude keeps
  13:00. The weather's own air peaks two hours after solar noon.
- The server sends both in `SessionJoined.Conditions`, and the client's
  `ApexSky::Derive` reads them (`SolarNoonOf`, `DeclinationOf`), so the
  race never needs the longitude. Before a session exists the create
  screen does: the lobby lists each track's site
  (`TrackConfigSummary.LatitudeDeg` / `LongitudeDeg`, `TrackConfig::site`,
  left off for a track without a longitude; `AltitudeM` beside them when
  the YAML names a height off sea level), and the Conditions tab's Date
  row (late May, or the 15th of a month: `ApexCreateSession::DateChoices`,
  sent as `day_of_year`) stands the preview's sun over the circuit on that
  date at `ApexSky::SolarNoonMinutes`, a port of `solar_noon_minutes` and
  `legal_clock` (`ApexSim.UI.CreateSession.SolarNoon` holds it to the same
  almanac; `.Date`: a December create at Silverstone at 16:30 is dusk). The
  row names the noon it used ("SOLAR NOON 12:00 HERE"); with no site the
  preview keeps 13:00 and the catalog row's latitude. `ats-export` writes `latitude_deg`
and `north_yaw_deg` (true north in the track frame, `DemGeoref::north_yaw_deg`)
into the export manifest's `metadata`, so the client puts the sun where it
stands over the real circuit; re-export a track for the client to see a
change.

## The live sky (`conditions.rs`)

`GameSession::sky` is a `LiveConditions`, built by `live_sky` from the
resolved conditions, the track's latitude and altitude and the session id.
`update_sky` runs at the top of every tick and acts once a second:

- The clock runs at `time_scale` from the host's hour.
- With a `forecast` written by the host, the weather follows it
  (`forecast` returns its steps at `at_min` minutes of session, whatever
  the clock's scale), and the sky moves even with `changeable` unset
  (`is_static` is false). `HostForecast::from_steps` holds it to the rules
  on every `clamp`: in time order, each step a change of weather at least
  `MIN_GAP_MIN` (4 minutes) after the last and after the start, none past
  `HORIZON_MIN` (26 hours), at most `MAX_STEPS` (10); an unknown weather byte
  is dropped on reading. `resolve_at` keeps it to the sky's season as the
  seeded forecast is kept: a winter sky's rain falls as snow, a summer sky's
  snow as light rain. The echo and the listing carry it as simulated. It is
  packed a step to a `u16` (the minute over three bits of weather) so
  `SessionConditions` stays `Copy` and small (a bigger one trips clippy's
  `large_enum_variant` on `TransportEvent`).
- Else, with `changeable` set the weather follows `forecast(start, session id)`:
  one weather step at a time (sometimes two above settled), drawn back
  toward the host's pick, a stormy sky leaning wet, never sooner than 4
  minutes of session apart however fast the clock runs. The seed is the
  host's `sky_seed` when the create named one, else the first half of the
  session's id (`SessionConditions::sky_seed_or`), so a host who picked a
  seed on the create screen gets the forecast it showed.
- Rain eases toward the weather's over `RAIN_EASE_S` (3 min), cloud over
  `CLOUD_EASE_S` (5 min). The air follows the hour and the cloud with the
  host's offset kept (`AIR_EASE_S`); the asphalt lags over `TRACK_EASE_S`
  (15 min of day clock) toward the air plus the sun through the cloud, and
  sits a degree under the air while the road holds water.
- Each step bakes onto the session's track: air and track temperature, the
  air density, `TrackSurface::water` (the road's mean water: the tyre the
  track calls for) and `wet` (`conditions::WET_FROM_WATER`, 0.05), and hands
  the falling rain to the road (`RoadState::rain`).

- **Snow** is a winter sky (`is_winter`: the host picked snow, or overcast
  with the air under `WINTER_BELOW_C`, 1 °C). Its forecast swings only
  between overcast and snow (never into rain, so a summer sky stays rain), and
  its air keeps the winter's cold either way: snow's own day is overcast's
  17 degrees colder (`Weather::SNOW_AIR_DROP_C`), and an overcast winter sky
  is held that far down too. Snowfall eases in over `RAIN_EASE_S` like rain
  (`LiveConditions::snowfall`; `rain` is 0 under snow), and the lights come
  on as in rain.

- **Dew and fog** come with the overnight cooling (`climate.rs`). The
  session's humidity is read as the afternoon's: `LiveConditions::dew_point_c`
  is the dew point (`climate::dew_point_c`, Magnus) of the day's warmest air
  (`WARMEST_AFTER_NOON_H` after solar noon, the host's offset kept) at the
  humidity now, so the same water in the night's cooler air stands nearer
  saturation. A clear, still night takes the asphalt under the air
  (`sky_cooling_c`: `SKY_COOLING_C` 4 °C, times the clear share of the sky,
  the night's share, all of it with the sun under the horizon and none from
  8° up, and the calm's, all of it under 6 km/h and none from 20), and dew
  forms as far as that cooled asphalt lies under the dew point
  (`LiveConditions::dew`, `climate::dew_strength`: full `DEW_SPREAD_C` 3 °C
  under it; none while rain or snow falls). The road takes it
  (`RoadState::dew`, below). A radiation fog stands as the air itself comes
  within `FOG_SPREAD_C` (2 °C) of the dew point in still air, burnt off as
  the sun climbs past 15° (`LiveConditions::fog`, 0..1), and
  `LiveConditions::visibility_m` is the sight through it: 10 km clear
  (`CLEAR_VISIBILITY_M`) down to 150 m (`FOG_VISIBILITY_M`), evenly on a log
  scale. A sunny 04:00 on the model day at 50° N in a 3 km/h breeze, after
  a 23 °C afternoon at 50%, has its dew point at 12 °C under 13 °C air: full
  dew and a mist of some 1.7 km, both gone by 10:00
  (`a_still_clear_night_brings_dew_and_fog_and_the_sun_lifts_them`). The
  fog is a figure in the conditions and on the wire (`SkyNow.visibility_m`);
  nothing in the physics or the AI reads it.

A session whose clock and sky both hold (`is_static`) bakes nothing after
creation: it is the static bake above, plus the road state (and the dew
the held sky keeps laying on it).

A race weekend may give practice and qualifying a sky of their own: each
such day bakes its conditions into a fresh copy of the track and starts
the live sky over at the day (`GameSession::begin_day`, `sky_epoch_tick`;
the clock and the forecast run from it). See
[sessions](sessions.md#race-weekend).

## The road (`road_state.rs`)

Every session's track carries a `RoadState` (`TrackConfig::road_state`,
built by `with_road` from the baked water and the start's rubber). The lap
is cut into 10 m cells (`CELL_M`), each cut across the road into 1 m bins
(`BIN_M`, +-16 m, held to the road's width). A car crossing into a new cell
(`update_road`; not in the garage, towed or under 3 m/s) lays a pass for each
tyre at its contact patch, in the cell and bin the physics last sampled the
road for that tyre (`RoadWork::patch_ahead_m` / `patch_lateral_m`), by what
the tyre did since the car's last cell (`CarState::road_work`, added up each
tick after the tyre step):

- its **load** makes its pass (`pass_weights`): four times its share of the
  car's load, the load held to the car's weight at least, so a planted car
  counts four passes whatever its downforce, the loaded outside wheels of a
  corner more than the inside, a wheel in the air none. The pass is what
  wipes the water, packs the snow and sweeps the marbles.
- its **sliding** lays the rubber (`rubber_passes`): a pass's worth for
  `RUBBER_SLIDE_J` (700 J, an AI GT3's mean through a 10 m cell of Monza),
  up to six, so the braking zones and corners rubber in and a straight
  next to nothing.
- the tread the car's tyres **wore** is shed as marbles over the cell
  (`shed_for_wear`): a pass's worth for `MARBLE_WEAR_PCT` (0.06% over the
  four tyres), up to four, so the marbles come where the tyres work and
  faster from a soft compound.

The state steps every `STEP_TICKS`, draining by the sky's `evaporation()`.

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
  gather off the line in the corners. A tyre driven through them picks them
  up (`TireData::pickup`, a grip loss flung off over a lap:
  [vehicle physics](vehicle-physics.md#wear-flat-spots-punctures)).
- **dry**: how much of the cell's water the wheels have wiped there,
  rewetted by the rain, never past `line_dry_ceiling(rain)`.

- **snow** 0..1 (the cover) and **ice** 0..1: see below.

Per cell, the **water depth**: fed toward the falling rain, deeper by
`PUDDLE_GAIN` where the cell is low, drained above that. A cell is low where
the road lies `LOW_DEPTH_M` below its surroundings along the lap, or where it
is banked or cambered (`edge_low`): the whole width drains to the low edge,
which holds water as a cell `EDGE_LOW_MAX` (0.5) low from some 2 degrees of
banking, a stream along the inside of a cambered corner (and so never a
`puddle_cells` dip). Water over 1 is standing water. The puddle of a low
cell lies across part of the road (`RoadState::bin_depth`): at the low edge
where the road is banked over half a degree (positive banking lowers the
right), at a seeded one of the two edges (`wind::hash01` of the cell) on a
crowned one, at a seeded spot on a flat road without a crown (an imported
circuit), 1.5-3 m half-wide plus 3 m for a full dip; the rest of the cell
holds the flat road's share (`1 / (1 + PUDDLE_GAIN x low)`). A tyre, the ice
and the line's water read the bin's depth; the grass holds the cell's. The
crown (`apexsim_formats::road_profile`, `TrackConfig::road_profile`) counts
as a bank of its 2% grade for `edge_low`, fading out as the road's banking
takes over, so every crowned cell holds a strip of water along an edge in
the rain.

**Dew** (`RoadState::dew`, 0..1, set each second by `update_sky` from
`LiveConditions::dew`, a held sky too): each cell fills toward `DEW_FILM`
(0.04 of the weather's water, a damp road under `WET_FROM_WATER`'s 0.05 and
the least water that freezes, so the slicks stay on and nothing reads the
track as wet) times it, at `DEW_RATE` (half an hour to most of it), flat
across the road (no puddle gain), and the line's dried bins rewet with it.
Once it stops, it drains as the rain's water does, by the sky's
evaporation; the cars wipe their line of it as they wipe the rain. Full dew
costs the road a little over 1% of its grip and a slick some more on its
own water curve (`dew_dampens_the_road_and_the_line_and_the_sun_dry_it`). A
session that starts before solar noon finds the night's dew already lying
(`with_road`: `RoadState::lay_dew` at the start's dew), so a still, clear
dawn starts damp (`a_dawn_session_starts_on_the_nights_dew`); an evening
start lays it as the night comes. A road without dew is untouched by it to
the bit.

### Snow on the road

Under a snow sky the road's grip is the *covered* road's: `Weather::Snow`
bakes 0.28, a racing tyre's quarter of its dry grip on packed snow, and less
again on tyres that never reach their window at -3 °C. The racing line and
the AI's speed profile are planned for it, and the field still does not race
it cleanly: in the snow survey (Silverstone, Monza, Spa) the AI spends minutes
off the road, piles up and retires cars inside three minutes.
`RoadState::snow_grip(cover, ice, baked_cover)` is the cover's grip
(`1 - SNOW_COVER_GRIP * sqrt(cover)`: bare 1.0, covered 0.28, a thin film
already most of the way there) against the baked
cover, less the ice, and is multiplied into the sample's `grip`, so the road
is exactly as baked at the start of a snow session and a dry-baked road
under new snow falls to the covered figure. The slush is the cover's own:
in a snowy sample the water counts as water only as far as the road is bare
(times `1 - sqrt(cover)`, against the baked road worked out the same way), so
a covered road grips as the cover whatever lies under it, and a road melted
clear grips as wet asphalt while the melt water stands and as dry asphalt
(the covered bake's 0.28 back to 1.0) once it has drained. The run-off
(`wet_grip`), which no racing wheel clears, carries the verge's cover the
same way: buried under a snowfall, bare once the track melts it. A car
with a snow tyre (studs) is sent out on it on a covered road and grips
`snow_grip` times the weather's tyre on full cover, by the same root of
the cover, and `ice_grip` times on ice
([vehicle physics](vehicle-physics.md#compounds)).

The pit lane keeps a cover of its own (`RoadState::lane_snow`, one figure a
10 m cell of the lane's own station, `PitLane::length_m`; none on a track
without a lane). It goes out with every `RoadState` slice while the road
has snow (`LaneSnow`, a byte a cell, the whole lane: `write_lane_snow`),
and the client gives the pit lane's surfaces (`MI_pit_lane*`) its mean as
their `SnowCover` in place of the lap's (`FApexRoadStateMap::GetLaneSnow`,
`AApexRaceDirector::LaneSnowMids`). A car in the lane (`PitState::in_lane`) that crosses into a
new lap cell also lays its wheels' passes on the lane's cell at its
`lane_station_m` (`note_lane_pass`), which pack it toward `SNOW_FLOOR` at
`LANE_PACK_SHARE` (a quarter) of a racing bin's rate, the figure standing for
the whole width; the snowfall covers it and a warm track melts it like the
verge. A wheel in the lane reads it (`sample_in_lane`, from `update_car_3d`
at the car's lane station) as its `wet_grip`'s cover. Under a steady
snowfall the lane's few cars barely keep up with it; once the snow stops,
twenty cars an hour pack a driven stretch to about 0.7.

- The cover builds toward 1 under snowfall (`SNOWFALL_RATE`: ten minutes
  from bare to 90%). Every wheel pass packs its bin down toward
  `SNOW_FLOOR` (0.65, a film that grips 1.5 times a covered road) by
  `SNOW_CLEAR_PER_PASS`, so a field drives a darker, quicker line through the
  white that the snowfall keeps trying to cover.
- A track over 0 °C melts it (`SNOW_MELT_PER_C` of the cover per degree per
  second, ice thaws too). The verge, which no wheel clears, keeps a cover per
  cell (`Cell::verge_snow`) that moves the grass's grip the same way.
- The water on a bin the cars have cleared freezes into **ice** when the
  track is under `FREEZE_C` (-1 °C), the cover is under 0.9 and no snow is
  falling: the line freezes overnight and costs `ICE_GRIP_LOSS` (0.6) of
  its grip until it thaws: a covered bake's ice is a tenth of the dry road.
- The asphalt nobody races on glazes instead (`road_state::glazes`): the
  paved run-off's cover (the verge's, `Cell::verge_ice`) and the pit lane's
  (`RoadState::lane_ice`, a figure a lane cell) freeze at `ICE_FREEZE_RATE`
  on a track under `FREEZE_C` with no snowfall burying them, once packed or
  half melted (between `GLAZE_MIN_COVER`, 0.05, and the 0.9 of a fresh
  cover), and thaw as the line's ice does. The run-off's and the lane's
  `wet_grip` take it (`snow_grip(cover, paved_ice, ..)`), and a wheel there
  reads it as its ice (`RoadSample::paved_ice`, the snow tyre's bite): a
  packed lane at -5 °C after an afternoon's melt loses well over a third of
  its grip in the hour
  (`the_packed_lane_and_the_melted_run_off_glaze_on_a_cold_night`). The
  glaze is not on the wire.
- The slush is `SNOW_WATER` (0.3) of standing water while it snows (the
  `Weather::Snow.water()` that picks the tyre and that the dry line wipes),
  so `Compound::grip_on` and the AI's crossover choose treaded tyres with no
  snow branch. Rubber and marbles lay on a snowy bin as little as on a wet
  one.
- `mean_line_snow` is the line's mean cover: telemetry's `line_snow_pct`.
  Nothing moves in a session without snow (`snowy` false, every figure
  multiplies by 1.0): the dry bake and the determinism test are unchanged.

`RoadState::sample(station, lateral)` is what a tyre finds: `water` (for
the compound's grip), `grip` (water against the baked water, times rubber
and marbles: the racing surface), `wet_grip` (water alone: run-off, pit
lane), `curb_grip`, `off_grip`, and the snow a snow tyre bites into:
`snow` and `ice` on the racing surface, `paved_snow` (the run-off's, or
the lane's in `sample_in_lane`) and `verge_snow`, all 0 off a snowy road.
`update_car_3d` reads it per wheel by contact class, and at the car's
middle into `CarState::surface_grip_share` and `snow_tyre_share`, which the
AI reads through `tyre_grip_share`. `mean_line_water` drives the AI's tyre
crossover, and `line_snow_of` (`mean_line_snow` on a snowy road, the bake's
cover on a track without a road state) the snow tyre's
([ai.md](ai.md)).

## The road on screen

The server sends the road state as it is, and the client draws it on the
road: a darker, glossier rubbered line, marbles as dark crumbs beside it, a
dry line between wet edges, puddles (darker, mirror-flat, in light rain as in
heavy: a tenth of heavy rain's depth over the cell's flat share is a full
one), and the debris lying on it.

**On the wire.** `RoadState` (TCP, named, droppable) carries a slice of
whole cells from `FirstCell`, wrapping the lap. Each cell is `ROW_BYTES`:
its water depth as a byte (`DEPTH_BYTE`: 100 is heavy rain on the flat,
over 100 a puddle), then for each of the `BINS` bins its rubber, marbles and
dry, each a byte over 0..1. `WaterRows` (left off when no puddle stands)
is the water on each bin before the wheels dry it, a `DEPTH_BYTE` byte per
bin (`RoadState::write_water_rows`); the client fills the state texture's A
from it, else from the cell's depth. `Geometry` gives each cell's centerline point and
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
- `RoadSnow`: laid out as `RoadState`; R, G and B the snow (below), A the
  bin's puddle: its water over its cell's shallowest bin, the flat share
  (`WaterRows`; 0 where none stands). Bound whenever the road state is
  drawn, snow or not.
- `RoadGeometry`: a texel a cell, the cell's point in world cm and its left
  vector in Unreal axes (the server's Y flipped). Nearest: the material
  reads the two cells whose centres a pixel lies between (`RoadStateCells`,
  the cell count) and blends the lateral each measures by how far along it
  is, a cell not yet known giving way to the other
  (`FApexRoadStateMap::TexV` the same on the CPU). One cell's measure alone
  jumps at its edge wherever the road turns, and at a hairpin (30 degrees a
  cell) drew wedges and arcs in the snow.

The road itself (`family` `road`) is made from `M_ApexTrackRoad`, the base
graph plus `ApplyRoadStateGraph`, and so are the curbs and the paved run-off
(`surface_concrete`, `surface_asphalt_runoff`, `surface_astroturf`;
`ApexTrackMaterials::ReadsRoadStateOffRoad`), with `RoadStateOffRoad` 1:
they take their cell's water and puddles but hold rubber at the neutral
half and draw no marbles or dry line (the server keeps those over the
road's bins only), and keep the lap's snow figure. The pit lane (its `u` is
its own station, and the server keeps no state on it), the grass and the
other bands stay on `M_ApexTrackBase` and pay nothing. Every mesh swept
along the centerline has UV0 `u` in metres of lap station, which picks the
cell (`RoadStateU` = 1 / (cell length x cells));
`-dot(world - point, left)`, blended between two cells, is the lateral,
which picks the bin. Its `v` is
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

On the client (`AApexRaceCarActor::SetHeadlights`) a car with its lights
on shows two spot lamps at the nose and its tail lights at the running
level. An open-wheeler (`FApexCockpitLayout::bOpenWheel`) has no
headlamps: it keeps the switch and the tail lights but throws no beam
(`ThrowsHeadlightBeams`), so a formula car at dusk lays no bright discs on
the road ahead of its nose. Verify runs `g9-mirrors-dusk`,
`g9-mirrors-night`, `g9-headlights-gt3`.

## Wire

- `CreateSession.conditions`, `SessionJoined.Conditions`,
  `SessionSummary` - golden bytes: `cargo test --lib --
  assists_wire_format conditions_air_wire_format sky_wire_format
  --nocapture` ->
  `ApexGolden::C_CreateSessionAir` / `S_SessionJoinedAir` /
  `C_CreateSessionSky` / `S_SessionJoinedSky` / `C_CreateSessionSkySeed`
  (a create with the host's `sky_seed`, last among the sky's keys) /
  `C_CreateSessionDate` (the host's `day_of_year` after it) /
  `S_SessionJoinedSun` (the echo naming `solar_noon_min` last) /
  `C_CreateSessionForecast` and `S_SessionJoinedForecast` (the host's
  `forecast`, last of all: an array of `[at_min, weather]` pairs).
- `CompactTelemetry.sky` (`network::SkyNow`, 20 positional fields: clock,
  weather, rain, cloud, road water, air, track, the wind now and where it
  blows toward, line rubber, time scale, next weather and when, then the
  snow on the line and the snowfall, both percent, then the second and
  third changes ahead with their ETAs, then `visibility_m`, the sight
  through the fog in metres, 0 from a sender that does not say), appended
  after `race_clock` (nil then when there is no race clock); every frame.
  Golden: `ApexUdpGolden::S_TelemetryCompactSky` (the client skips the
  twentieth field). A spectator stream's state record carries all twenty
  (`StreamState::encode`, the client's `ApexSpectatorWriter` too), so a
  showcase, a live stream and a replay draw the fog as the race had it; an
  older stream's nineteen read `visibility_m` 0.
- `PlayerInput.headlights` / `flash`: `cargo test
  player_input_headlights_wire_format -- --nocapture` ->
  `ApexUdpGolden::C_PlayerInput`.
- `RoadState`: `cargo test road_state_wire_format -- --nocapture` ->
  `ApexGolden::S_RoadState` and `S_RoadStateSnow`; the stream's `Road` record
  in `spectator_wire_format` -> `ApexSpectatorGolden::Road`. Snow rides in a
  `SnowRows` field (named, left off the wire with no snow: `write_snow_rows`,
  `SNOW_ROW_BYTES` a cell, the verge's cover then each bin's cover and ice) and
  the stream record's twelfth element; the existing rows keep their layout.
  `Weather::Snow` is the sixth `Weather` byte (an older client shows a sunny
  sky).

See [protocol.md](protocol.md) for the append-only rules.

## Client

- `FApexSessionConditions` (net module: `Describe`, `ClockText`,
  `WeatherLabel`; unset optional fields are -128 / -1 and written only when
  picked; `Forecast`, an array of `FApexForecastStep`, clamped by
  `ClampedForecast` as the server does and written only when not empty), the flow's `CreateConditions` (kept on the profile),
  `UApexNetSubsystem::GetSessionConditions` and `GetLatestSky`
  (`FApexSkyNow` on `FApexTelemetryFrame::Sky`).
- The create screen's Conditions tab (it scrolls): weather tiles,
  time-of-day slider, air temperature stepper with AUTO, wind strength list
  and an eight-way direction dial, Clock (frozen to 60x), Weather changes
  and Track rubber rows, and under them the Forecast: the changes the pick
  lays out with the day's clock at each (`ApexCreateSession::Forecast`, a
  port of `conditions::forecast` to the bit, pinned by
  `ApexSim.UI.CreateSession.Forecast` against the Rust's
  `the_forecast_reference`). Picking a changeable sky gives the create a
  `sky_seed` of its own (`NewSkySeed`); ANOTHER rolls a new one; a sky that
  holds sends none.
- The AUTO air is the server's `resolve_at` over the pending circuit
  (`ConditionsAtSite`: its site, `ApexCreateSession::FClimateSite`, and its
  solar noon on the picked date). The site is the lobby's
  (`TrackConfigSummary.LatitudeDeg` / `LongitudeDeg` / `AltitudeM`), else
  the catalog row's from the export's metadata (`LatitudeDeg`, and with a
  longitude `LongitudeDeg` and `AltitudeM`), else 50° N on the sea.
  `ApexCreateSession::AirTempC(C, Site)` from `AutoAirTempCAt` (the model
  day's air peaking two hours after solar noon, moved by `ClimateOffsetC`,
  a port of `climate::climate_offset_c`: the latitude's sea-level mean, the
  region's climate (`ClimateOf`, `climate_of`'s map), the season and the
  lapse with height; snow's own air unmoved) and `SnowAirDropC`; `IsWinter`
  and the laid-out forecast read that air, so a January overcast night
  previews the winter sky the server makes of it.
  `ApexSim.UI.CreateSession.SeasonAir` pins them against the figures
  `climate::tests::client_season_air_figures` prints (bare latitudes and
  four real sites: Silverstone, Albert Park, Mexico City at 2240 m,
  Bahrain). The sky preview's air and asphalt use the same. The humidity is
  not previewed.
- ADD STEP writes a forecast: one row per step (at most 10), a minute
  stepper (5 minutes a press, the clock beside it when it runs) held a
  4-minute gap from its neighbours and the horizon, a weather stepper that
  skips the weather before it (a step left repeating the next is dropped),
  and REMOVE (`ApexCreateSession::AddForecastStep`, `MoveForecastStep`,
  `CycleForecastWeather`, `RemoveForecastStep`, so the list always passes
  `ClampedForecast`; picking a start weather the first step repeats drops
  that step). Rows are reached by keyboard and pad like every other
  control. The forecast line then reads "Written: ..." as the server will
  follow it (`KeptToSky`: a winter sky's rain as snow, a summer sky's snow
  as rain, with a note when that changes it), ANOTHER is off, and the
  forecast goes with the create's conditions (and each weekend day's sky).
  `Describe` names its first three steps ("forecast light rain at 20 min,
  heavy rain at 45 min, 1 more").
- The HUD's `sky.outlook`: the changes beyond the three telemetry carries,
  from `ApexCreateSession::SessionForecast`: the session's written forecast
  (`HasForecast`, as echoed), else the seeded one laid out from the
  session's seed (`ApexCreateSession::SessionSeed`: the host's `sky_seed`,
  else the first half of the session id, `SeedOfSessionId`). The next two
  changes place the session on the list; nothing is shown until they match.
- `-ApexAutoRace` takes `-ApexDayOfYear=N` and `-ApexForecast=MIN:weather,...`.
- The race director lights by `ApexSky::Derive` (static sky) or
  `ApexSky::DeriveLive` (blended from the server's cloud and rain,
  relit only when `LiveSkyMoved` says so: a game minute, a weather step, a few percent of cloud, rain, water or snow, a tenth of the fog's visibility); `FSkySite` (latitude, north from
  the catalog row's `bHasLocation` / `LatitudeDeg` / `NorthYawDeg`) turns
  the sun's bearing into the track frame; the sky is derived again once the
  track's actors are in (`ApplyTrackLevelConditions`, the `Track sky:` log
  line), as the first derive can come before the track is known and fall
  back to 50 N with north along +X. A director-owned unbound
  post-process volume carries exposure, wet desaturation and bloom. The
  night's exposure ceiling is EV 8, so a dark circuit's night stays dark
  with a lamp in view; a floodlit circuit (`ApexLights::FloodlitLampCount`,
  100 lamps lit: Mandarina Bay has some 450) may stop down to
  `FloodlitMaxEv` (11), or its road under the lights read near white, and
  its bias is raised by `FloodlitBiasEv` (0.25). Its arm lamps
  (`lamp_arm_twin`, 40 m apart) reach `ArmRadiusCm` (45 m), so the
  engine's attenuation window keeps the road between two lit and the
  exposure no longer stops down on bright pools over dark gaps.
- `ApplyTrackLevelConditions` (once the track is built and visible): fog
  by weather, each weather's haze a visibility (`ApexSky::HazeVisibilityM`:
  sunny 8 km, cloudy 6, overcast 4.5, light rain 3.5, heavy rain 2.2,
  snow 2.8), so a circuit's far end and horizon soften by day, thickened
  in a live sky to the server's fog (`SkyNow.visibility_m` ->
  `FApexSkyNow::VisibilityM`, `ApexSky::FogDensityFor`, never under the
  weather's haze). A visibility becomes the engine's fog density through
  `ApexSky::FogDensityForVisibility`: the engine takes the density per
  1000 cm in powers of two, so its extinction is density x ln 2 / 10 per
  metre and Koschmieder's 3.912 / extinction gives density = 56.4 /
  visibility (0.0015 is 37 km of air); `apexsim.sky.ServerFog 0` draws the haze alone, `apexsim.sky.FogVisibility N` the fog for N m in place of the server's) (its inscattering a luminance, `ApexSky::FogLuminance`: the
  weather's fog colour lit as the sky lights the clouds, x `HazeGain` 1.4,
  so far land fades into pale haze rather than sinking into dark blue), the road family's material roughness eased toward
  `ApexSky::WetRoadRoughness` (0.3) with the road's water (the road itself
  per bin from the road state once it arrives, above), the racing-line
  dots wet (`AApexRacingLineActor::SetWet`), kit flag poles (`SM_flag_pole`)
  turned downwind. After dark the lamp props get real lights, the nearest
  `apexsim.lights.Max` (90) within `apexsim.lights.Range` of the camera
  (`ApexStreetLights.h`), and the emissive lamp faces glow.
- Rain is `AApexRainActor`: up to 1500 streaks of the engine cube in a box
  that follows the camera, drifting with the wind (no particle assets). The
  same box is snow (`SetSnow`): flakes that fall at 1.2 m/s, short and fat
  until the camera's speed stretches them. They are a white `M_ApexEmissive`
  instance glowing at 1.1x the cloud sheet's luminance (`SetFlakeLuminance`
  from `FSkyState::CloudBrightness`), so they read white against the cloud
  at any exposure; lit, they took the sky's blue-grey and read as dark
  specks. Without the baked parent they fall back to a white copy of the
  drops' material.
- Cloud is `AApexCloudLayerActor`: a dome of the engine sphere round the
  camera drawn with `M_ApexCloudLayer` (unlit, translucent; baked by
  `-run=ApexMaterialBake`), which carries each view ray up to a plane
  1.5 km over the camera and reads a world-space noise there, so the cloud
  stands still over the circuit and drifts only with the wind: the actor
  adds the drift up each frame (`CloudOffset`, eight times the ground wind),
  so a gust speeds the cloud up rather than shifting the whole sky by the
  change in wind times the session's age. Cloud shows
  where the noise clears `1 - CloudCover`; from 0.8 the sheet is whole, and
  thicker cloud is darker. `ApexSky::FSkyState::CloudCover` is
  `Weather::cloud`'s rung (cloudy 0.5, overcast 0.85, rain 0.9 / 1, snow 1)
  for a fixed sky and the server's eased `cloud_pct` for a live one;
  `CloudLook` lights it (`CloudColor`, `CloudBrightness` in cd/m²: a few
  thousand under a high sun, near nothing at night). A fair sky carries a
  cirrus veil on a second plane six times higher (`FSkyState::CirrusVeil`,
  `ApexSky::CirrusVeilOf`: sunny 0.7, cloudy 0.35, none under a sheet):
  noise drawn out five to one into streaks, kept to patches by a coarse
  mask, at most 0.55 opaque and lit whiter than the cumulus, so a clear
  day is not a flat blue. The dome hides itself under a bare sky and
  spawns with the first sky that has cloud or veil; a parent baked before
  the veil (no `CirrusVeil` parameter) is baked again by
  `-run=ApexMaterialBake`. Verify runs `g8-sky-<stem>-<weather>`
  (Armadillos and Monzarella, sunny, cloudy, overcast).
- Spray: on a wet road each car throws pale puffs back and up from behind
  its wheels (`AApexRaceCarActor::UpdateSpray`, the rears twice as often),
  into the same smoke rings as the tyre smoke, as much as its tyres are wet
  (`TyreWetness`, the road state's water under the car soaked up and dried
  off) and it is fast (nothing under 12 m/s, full at 62).
- Snow on screen: `ApexSky` lights a snow sky as an overcast one made flatter,
  cooler and thicker in fog (`SnowIntensity`, `SnowCover`; a live sky blends
  overcast to snow by `snowfall_pct` and takes the cover from `line_snow_pct`).
  `M_ApexTrackBase` has a `SnowCover` parameter (0 default) that whitens
  whatever faces up (the geometry's world normal); the director sets it on
  the ground, verges, curbs and wear bands (`SnowMids`, curbs half, wear bands
  a third: they keep some paint), not on buildings and props. The road
  (`M_ApexTrackRoad`) draws the per-bin `RoadSnow` texture instead (R cover, G
  ice, B the verge, from `SnowRows`): white from 0.9 cover, dark slush on a
  packed line (0.65 to 0.75), ice a dark mirror. A base baked before snow has
  no `SnowCover`, so `-run=ApexMaterialBake` bakes it again unasked.
- Snow on the props: buildings, stands, barriers and trees read the lap's
  cover from the parameter collection `MPC_ApexSky` (`SnowCover`, written by
  `AApexRaceDirector::SetSkyCollectionSnow` with `ApplySnowCover`, 0 again
  when the track goes), so a kit's thousands of instances need no dynamic
  material. The material bake and the prop import put the blend into every
  prop parent under `/Game/Props/_Parents` that lacks it
  (`ApexTrackMaterialGraphs::SnowOnProps`: the parent's base colour and
  roughness go to snow by the cover over what faces up, plus the instance's
  `SnowFloor`), and set `SnowFloor` 0.4 on the masked tree-card instances
  under `/Game/Props/tree`, whose cards are vertical and would take none.
  A project without the collection draws the props as before.
- Rain lights: a car's `car_rainlight` material slot glows steadily
  (`apexsim.car.RainLightNits`, 3000) while the sky is wet, snowy or the road
  holds water (`AApexRaceDirector::IsWetSky`, `AApexRaceCarActor::SetRainLight`);
  a body without the slot has none.
- Car headlights: `AApexRaceCarActor::SetHeadlights`
  (`apexsim.car.HeadlightLumens`), from the telemetry bits; a flash is held
  at least 0.2 s so a tap shows. Each lamp is a spot with a narrow core (6°
  inner, 22° outer cone) aimed 1.5° down, so it meets the road well ahead
  of the nose instead of lighting a white disc at the bumper
  (`headlights-night`).
- HUD data points `sky.*` and the `conditions` component (off by default):
  [../game/hud-modding.md](../game/hud-modding.md).
- Unattended: `-ApexAutoRace -ApexWeather=heavyrain|snow -ApexTimeOfDay=22:15
  -ApexTimeScale=N -ApexChangeable=N -ApexTrackRubber=N`.

## Checking it

```bash
cd server/sim
cargo test --lib -- conditions:: road_state:: wind::
cargo test --test all air_test::                    # density, turbo, wind, the AI in a gale
SURVEY_WIND_KPH=25 cargo test --release --test all ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored --nocapture

cd ../server
cargo test --test all session_conditions_test::     # echo, listing, the grip bake
cargo test --test all track_evolution_test::        # rubber and marbles, forecast rain and the AI's crossover, 60x clock, determinism
cargo test --lib -- road_state_wire_format a_render_carries_the_road_state
cargo test --test all sim_wire_test::               # the sky ahead as the wire sends it (SkyNow)
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
