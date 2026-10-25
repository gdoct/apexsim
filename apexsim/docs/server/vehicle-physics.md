# Vehicle physics

The server owns the car model: each tick `physics::update_car_3d` advances a car's
`CarState` from its driver's input, against its `CarConfig` (from `car.toml`) and the
session's `TrackConfig`. The client has no physics. Everything is a pure function of the
tick's inputs (no clock, no RNG; chances are `wind::hash01` of tick and car), so
`server/sim/tests/all/determinism_test.rs` holds. A car.toml without a newer table or key simulates exactly
as before that feature: every model defaults to off or to a multiplier of exactly 1.0 at the
car's reference state.

## Code

- `server/sim/src/physics.rs`: `update_car_3d`, tyre force solve, surface queries, collisions,
  steering aid, gearbox, track progress.
- `server/sim/src/data.rs`: `CarConfig` and its sub-configs, `CarState`, `TireData`,
  `DamageState`, `DrsSpec`. `car_loader.rs`: the car.toml schema.
- Per subsystem: `tyre_thermal.rs`, `aero.rs`, `slipstream.rs`, `brakes.rs`,
  `engine_heat.rs`, `damage.rs`, `geometry.rs`, `hybrid.rs`, `drs.rs`, `car_setup.rs`,
  `setup_sheet.rs`; `game_session.rs` for what runs around the physics (fuelling, tyre
  fitting, tuned configs).

Related: [conditions](conditions.md) (weather, air, wind, road state), [AI](ai.md), [pit
lane](pit-lane.md), [sessions](sessions.md) (assist and damage rules),
[protocol](protocol.md), [cars](../../../apexsim-content/docs/cars.md), [road mesh](../../../apexsim-content/docs/road-mesh.md),
[force feedback](../game/input-and-ffb.md).

## The tick

`GameSession::tick_racing` / `tick_free_practice` / `tick_hotlap` run `update_air`
(`update_retirements`, `update_pits`, `update_drs`, `update_wind`, `update_road`,
`update_debris`, `slipstream::update`, `update_racecraft`), then `car_step::step_car` per
car: `update_car_3d` with `simulated_config` (the driver's tuned config, else the shared
one), then `update_track_progress_3d` ([sessions](sessions.md)); then
`check_collisions_refs` and `check_wall_collisions`.

`car_step::step_lone_car` is that tick for a car alone on the track: `step_car`, then
`physics::clear_contact` (all the car-car pass does with no other car) and
`physics::collide_with_walls` with the car's shared config, as the wall pass reads it. It
is the step the client's prediction runs through the C API in `server/ffi`
([building](../building.md), [client prediction](../proposals/client-prediction.md));
`car_step::air_for_tick` is the tick's wind both set first. Everything else in
`update_air` (pit lane, DRS, wake, road-state passes, debris, recoveries) stays the
session's. `car_step::encode_state` / `decode_state` write a car's full state as a
versioned blob, including the runtime fields serde skips on `CarState` (centerline hint,
lap timing, DRS arming, road work; not the force-feedback collector, which nothing in the
sim reads), so a loaded state steps on bit for bit.

## The car model (`physics.rs`)

`update_car_3d`'s numbered comments are the map. After the early outs (not drivable; held
still while serviced) and the gearbox, limiter and DRS flap:

- **Loads**: from the laden mass (`CarConfig::laden`), axle lever arms at the CoG split.
  Weight transfer uses the previous tick's accelerations; lateral transfer goes through
  `geometry::lateral_transfer`. **Crests**: the contact plane's gradient, in the world frame
  and filtered (`CarState::ground_slope_f`, `ground_slope_y_f`), changes as the car moves;
  that change dotted with the car's velocity (`path_curvature_accel`: the surface's
  curvature along the car's actual path times speed squared) is the body's vertical
  acceleration (`CarState::vertical_accel_mps2`, ±1.5 g) and the static loads scale by
  `1 + a/g`. A crest unloads the car (fast enough, it leaves the ground) whether taken
  square, diagonally or sliding across it; a banked turn, its gradient turning with the car,
  loads it by the lateral acceleration times the bank's slope; a car yawing on a plain grade
  and a flat road are unchanged (`a_crest_unloads_the_car_and_a_dip_loads_it`,
  `the_crest_reads_the_surface_along_the_path`). Both road backends feed it through the
  same contact plane. A kerb's step is filtered out of it (the suspension takes kerbs).
- **Aero** (`calculate_aerodynamic_forces`): drag and per-axle downforce through the
  ride-height map, attitude, porpoising, wake, air density and wind (drag along the relative
  airflow, a side force on the flank).
- **Engine**: torque curve, turbo spool, limiter, `engine_density_factor`, coolant and
  damage factors; the hybrid motor; drive torques through the drivetrain and differential.
  Once the wheels turn the engine at or past `max_engine_rpm` in its gear, the limiter cuts
  all of its torque, so a car on the limiter cannot drive itself faster than the gear allows
  (`running_on_the_limiter_does_not_over_rev_the_engine`).
- **Gearbox**: a driver's shift engages at the end of the tick (`gear_engages`: no change
  of direction while rolling the other way). `[transmission] downshift_protection = true`
  (`protected_gear`) refuses a downshift that would spin the engine past `max_engine_rpm`
  at the road speed: a request for two gears down stops at the lowest that fits, one with
  none that fits is ignored. The F1, hypercar, LMP2 and GT3 cars have it, as the real cars'
  boxes do; the rally cars do not, and a missed downshift over-revs them.
  Either way the driver is told: `CarState::over_rev_ratio` (the revs the
  box forces past the limit, which `engine_rpm` clamps away),
  `shift_refused_s` (`SHIFT_REFUSED_CUE_S` after the protection stepped in)
  and `downshift_risk` (the next gear down would over-rev at this speed)
  go out as telemetry's `engine_flags` and `over_rev_pct`
  ([protocol](protocol.md)); the HUD's `shift_warning` banner, rev counter
  and gear digit and the cockpit dash's light bar show them
  ([hud-modding](../game/hud-modding.md)).
  **Brakes**: demand split by `brake_bias_front`, scaled per wheel by pad friction.
  **Handbrake** (`PlayerInput.handbrake`, held): `HANDBRAKE_SHARE` (0.6) of the car's pedal
  force on the rear axle on top, and ABS off for the rears, so they lock; a locked rear tyre
  then pushes with its sliding friction against the way its patch travels
  (`locked_wheel_force`), which leaves it almost no cornering force and swings the tail out.
  Nothing else moves while it is up.
- **Contact**: each tyre queries the ground under its own patch (`query_track_surface`: the
  road mesh when present and server.toml `[physics] road_contact = "mesh"`, else the centerline):
  height, normal and a `RoadContact`. The body settles on the plane through the four
  contacts (`fit_contact_plane`). The road state under each wheel (water, rubber, marbles)
  sets its grip ([conditions](conditions.md)).
- **Steering**: the speed-sensitive aid when on, damage toe, Ackermann, each wheel's toe.
- **Tyre forces** (`solve_wheel_forces`): a quasi-static torque balance on a Pacejka-style
  curve, no stiff wheel ODE. A request inside the grip circle is transmitted at the slip the
  curve gives; excess drive spins the wheel; excess brake locks it unless ABS holds peak
  slip; a friction ellipse couples the directions. TC LOW holds peak slip; HIGH caps drive
  at what the friction circle leaves beside the lateral force. Per-wheel grip = surface x
  `tyre_thermal::grip_multiplier` (temperature, pressure, compound, wear, flat spot,
  pickup, puncture) x camber grip x load sensitivity (`CarConfig::axle_tyre_mu`).
  What the racing line, the AI's grip budget and the steering aid read of a car is
  `CarConfig::envelope_mu`: `grip_coefficient` for a linear tyre (every shipped car); for a
  load-sensitive one (an AC import's `LS_EXPY`) each axle's two wheels at the corner limit,
  the outside one carrying the lateral transfer (`geometry::lateral_transfer`), and the
  weaker axle's figure, since that axle holds the car.
- **After the solve**: tyre heat and wear, brake heat and wear, coolant, engine wear, the
  force-feedback record (`CarState::feedback`), per-wheel track limits
  (`WheelState::off_track`), kerb strikes and bottoming.
- **Integration**: forces summed in the body frame (off-track and puncture rolling drag per
  tyre, gravity along the contact plane), yaw, velocities, position, landings, attitude from
  the contact plane; telemetry and fuel.

### Road contact classes (`RoadContact`)

| Class | Grip | Off the track for the lap |
|---|---|---|
| `Road` | the road's (weather, road state) | no |
| `Curb` | `curb_grip` | no |
| `Runoff` | `RUNOFF_GRIP_FACTOR` (0.95) x road | yes |
| `Off` | the ground's (`OffGround`), patchy (`grass_grip_patch`), rolling drag | yes |
| `PitLane` | road | no (a lap with a stop counts) |

Widths come from the curbs sidecar (centerline backend) or triangle tags (mesh,
`RoadContact::from_surface`); on the centerline backend the pit sidecar's strip is `PitLane`
(`on_pit_lane_strip`).

### Off-track ground (`OffGround`)

An `Off` tyre stands on one of four grounds. Grass keeps the track's own figures
(`off_track_grip` 0.35, weather-scaled, and `off_track_rolling_resistance` 0.06 at any
speed), so a track without trap data drives as before. The others scale the grass grip and
plough with a rolling resistance (a share of the tyre's load) that rises linearly with the
tyre's rolling speed from a standing figure to a full one at `TRAP_FULL_DRAG_MPS` (40 m/s):

| Ground | Grip x grass | Rolling resistance, standing .. full | Reports |
|---|---|---|---|
| `Grass` (also astroturf and any unknown key) | 1.0 | the track's (0.06) | `Grass` |
| `Gravel` (`off_gravel`) | 0.8 | 0.08 .. 0.40 | `Gravel` |
| `Sand` (`off_sand`, a sand trap) | 0.7 | 0.10 .. 0.50 | `Sand` |
| `Desert` (`off_desert`, a desert circuit's floor) | 0.9 | 0.07 .. 0.14 | `Sand` |

A car into a gravel trap at speed is bogged down hard; one that has stopped in it can still
crawl out, since the standing figure is below what a driven axle can push on that grip. On
the mesh the ground is the triangle's surface key (`RoadMesh::ground`, read once at load);
on the centerline backend, and wherever the mesh has nothing, it is the curb sidecar's trap
bands (`CurbBands::trap_at`, a trap laid over the run-off reach as the bake draws it) and
past them its `ground` (`CurbBands::base_ground`: `desert` on Sakhir).

The clients are told which: `CarState::wheel_ground` (set each tick from the wheels'
`soft_ground`, 0 for grass or a tyre not off or in the air) goes out packed two bits a
wheel as `CompactCarState::ground` (`GROUND_GRAVEL` 1, `GROUND_SAND` 2, `GROUND_DESERT`
3), for the dust plume, the road sound and the force feedback
([game/audio.md](../game/audio.md), [game/input-and-ffb.md](../game/input-and-ffb.md)).

### Collisions

- **Car against car** (`check_collisions_refs`): yaw-aware OBB overlap by separating axes,
  impulse by laden mass (`car_mass_kg`), damage to the zone the hit's angle names
  (`apply_damage_to_car`). Towed (and, in a hotlap, garaged) cars are left out.
- **Walls** (`check_wall_collisions`, after the car pass): the walls sidecar (`walls.rs`, 16
  m grid), each wall a thin rectangle with a height band; the same SAT push, restitution and
  friction by material, spin about the deepest corner, a resting car grinds along.

### Steering aid (`assisted_steering`)

Full input asks for the tightest turn the car can hold at its speed: lock = `max(kinematic
angle, front axle travel angle + 0.7 x peak slip angle)`, capped at full lock. Trap:
*adding* the travel angle to the kinematic angle held the fronts at extra slip in slow
corners and cooked them (`steering_assist_never_scrubs_the_fronts_in_a_slow_corner`).
Whether a driver may use it is a session rule.

## Tyres (`tyre_thermal.rs`)

### Heat

Each tyre is a **tread** in three zones across its width (`TireData::tread_c`
inner/middle/outer; `temperature_c` their mean, what the HUD shows) and a **core**
(`core_temperature_c`: carcass, gas, part of the wheel), conducting between them and between
zones.

- In: the slip power (`|Fx| x slip speed along + |Fy| x slip speed across`), split at the
  tyre's peak slip (`TyreWork::power_split_w`). Up to the peak it is hysteresis and heats
  the rubber by mass, `TREAD_HYSTERESIS_SHARE` (0.3) to the tread and the rest to the core;
  past it sliding friction is shared with the road, `FRICTION_HEAT_TO_TYRE` (0.3) into the
  tread. Carcass flexing (`CARCASS_HEAT_SHARE` x `rolling_resistance x load x speed`) heats
  the core. In a wake `DIRTY_AIR_SLIDE_HEAT` more of the gripping power goes to the tread.
- Hot rubber is less hysteretic (`hysteresis_share`): each layer's gripping and flexing heat
  is full up to `HYSTERESIS_FADE_FROM_C` (10 °C) over the window's top and falls 2% a degree
  past it, at most 60% off; sliding friction never fades. So an overheated tyre sheds it in
  about two gentle laps while the AI's running temperatures (a GT3's rear core runs some
  7 °C over the top) do not move. A one-node core cannot do that by its constants alone:
  its steady state is heat over cooling, its swing in a corner heat over capacity and its
  recovery capacity over cooling, so a smaller core swung as much as the surface.
- Out: air convection with speed^0.8 (the air the car meets, wind and wake included:
  `TyreWork::air_mps`), road conduction with the root of speed, several times more on a wet
  road.
- Zones: `zone_shares` (a `PatchLanding`) loads the inner shoulder with negative camber,
  the outer under lateral load and with toe-in (`ZONE_TOE_PER_RAD`: the wheel's toe against
  its axle's mean steer, so the setup's toe and the steering's Ackermann spread; toe-out the
  inner), the middle under a braking or driving force (`ZONE_LONGITUDINAL_SHARE` per g of
  `|Fx|`/load: the carcass bowed along the patch) and when over-inflated. The landing moves
  heat across the tread, never the total, so the mean is the one-node model's.
- Grip is read from the end of the last tick; heat steps after the solve.

Trap: the sub-peak split by mass is deliberate. Most of it in the thin tread made a careful
driver's tyres jump 10 °C per gentle turn while the AI stayed cool; giving part to the road
moved every class's window, which the AI's pace sets.
`the_surface_swings_like_a_real_tyre_over_a_lap` pins it.

### Grip and pressure

Grip reads 0.7 tread + 0.3 core (`TREAD_GRIP_SHARE`): full inside `optimal_temperature_c ±
temperature_window_c`, less `temperature_grip_falloff` a degree outside (eased in, the cold
side charged 0.6 of it, at most 25% off). Each tread zone is read against the core and the
three averaged (`tread_grip_factor`): a spread inside the window costs nothing, a shoulder
outside it costs that shoulder's grip although the mean is in the window, and an even tread
is the one-node factor exactly. Pressure is the gas law on the core
(`pressure_kpa`); the setup's pressures are the *hot* ones, so a cold tyre is soft and a
cooked one over-inflated, and `TireConfig::pressure_grip_factor` charges both quadratically
off `optimal_pressure_kpa`. A tyre at its optimum and set pressure grips as if there were no
thermal model, which is what an unfitted car gets (`CarState::tyres_fitted` false).

### Fitting (`game_session::fit_tyres`, `tyre_thermal::fit`)

Fitting zeroes wear, flat spots and punctures and sets the compound. From the garage or a
practice join: `blanket_temperature_c` or the air. On a race grid without a formation lap: half way to the optimum
(`FORMATION_LAP_WARMTH`, for the lap not driven); with one, as from the garage, the lap
warming them ([sessions](sessions.md#race-start-formation-lap-rolling-start-false-starts)). A hotlap out: the compound's optimum,
unless `HotlapRelocate.cold_tyres`. Brakes and the hybrid battery are fitted with them.

### Compounds

`TireConfig::compounds()` is the car's own `[[tires.compound]]` list in knob order (slicks
softest first, treaded last), else `default_compounds()` (soft, medium, hard, intermediate,
wet; medium the reference). The shipped cars list their class's own: F1 those five, Hypercar
soft / medium / hard / wet, LMP2 medium / wet, GT3 dry / wet (no intermediate). Each changes the car's tyre: grip,
wear, window shift, `water_grip` (dry / light / heavy rain, linear between; past 1.0 water a
puddle costs more). The **reference** compound is the car's tyre exactly, so calibration
holds.

The road's baked grip is for the tyre the weather calls for (`weather_compound_of`:
intermediate below `WET_FROM_WATER`, wet from it, none under `SLICK_BELOW_WATER`);
`Compound::grip_on` charges any other. A car with no treaded tyre races on slicks in the
rain. `CarState::tyre_compound` is what is fitted; the `tyre_compound` knob chooses the next
set (`CarSetup::compound_index_for`: a stock pick in the rain or on snow is the weather's
tyre). The AI always takes the weather's tyre; when it changes is in [AI](ai.md).

**Snow tyres** (`kind = "snow"`, a rally car's studs). On a snow-covered road the weather's
tyre is a car's first snow compound (`weather_compound_in`: the line's cover,
`RoadState::line_snow_of`, from `SNOW_TYRE_FROM_COVER` 0.3; a car without one gets the
water's tyre as before, and the rain never picks a snow tyre). The road is baked for the
tyre the weather would give any car, so a snow compound's bite is a factor on top,
`Compound::snow_factor(cover, ice)` = `(1 + (snow_grip - 1) sqrt(cover)) (1 + (ice_grip - 1) ice)`,
with the cover and ice under each wheel (`RoadSample`: the racing surface's on the road and
curbs, the run-off's or the lane's `paved_snow`, the verge's on the grass; no ice off the
racing surface). The root blends it in as the road's own snow grip falls
([conditions](conditions.md#snow-on-the-road)): a thin film is most of the way there. Its
`water_grip` still applies, so on full cover with the session's slush (0.3 water) studs at
`snow_grip` 2.0 and `water_grip = [0.8, 0.9, 0.85]` grip about 1.7 times the covered road
(0.48 of dry instead of 0.28), and on bare tarmac 0.8 to 0.9 of the weather's tyre.
`bare_wear` multiplies its wear by `1 + (bare_wear - 1)(1 - sqrt(cover))`: full on bare
tarmac, none on a covered road. All three keys default to 1.0, which is exactly the
compound without them, and off a snow session every cover and ice is 0.

**Studs pull out** on bare tarmac: each percent of tread a tyre wears adds
`stud_loss (1 - cover / STUDS_KEPT_FROM_COVER)` (0.3: a packed line or a film of snow keeps
them off the asphalt) to its `TireData::studs_lost` (0..1, `Compound::studs_pulled`,
in `tyre_thermal::step`), and a new set (`tyre_thermal::fit`) has them all again. The bite
goes with them (`Compound::snow_factor_with`, which `grip_multiplier` and `snow_tyre_share`
read with the set's mean): all of `ice_grip` over 1 is the studs', and `STUD_SNOW_SHARE`
(0.5) of `snow_grip` over 1, so a set that has lost them all keeps half its bite on snow and
none on ice. `stud_loss` defaults to 0, which loses none; the rally cars' studs are 0.03 (a
third of the tread worn on tarmac pulls them all).


### Wear, flat spots, punctures

- **Wear** grows with sliding friction power (`WEAR_PERCENT_PER_MJ` x the compound's wear x
  `TireConfig::wear_rate`, 4% more per degree over the window). `wear_grip_factor`: 6% over
  the life plus a quadratic 24% cliff from 70% worn.
- **Flat spots**: braking past `LOCK_SLIP_RATIO` above 3 m/s grows `TireData::flat_spot`
  (0..1) by the locked patch's energy once the lock-up holds: nothing for its first
  `FLAT_SPOT_DWELL_S` (0.1 s, `TireData::lock_s`), the full rate from `FLAT_SPOT_FULL_S`
  (0.3 s), so a brake on the edge of locking blips without spotting the tyre
  (`short_lock_ups_grind_no_flat_spot`); up to 4% grip; wears round with the tread
  (`FLAT_SPOT_ROUNDING_PER_PERCENT`). Sent as `DriverFeedback.flat_spot` (wheel, pad and
  road sound play it) and, for every car, telemetry's `flat_spot` (drawn on the tyre,
  [cars](../../../apexsim-content/docs/cars.md#wheels)).
- **Pickup**: a loaded tyre rolling over marbles past `PICKUP_FROM_MARBLES` (0.1; the
  swept line's crumbs stick to nothing) picks them up into `TireData::pickup` (0..1),
  which costs up to `PICKUP_GRIP_LOSS` (6%) of its grip and is flung off over about a lap
  (`PICKUP_SHED_PER_S`, faster on a wet road): a second through a bin half full of marbles
  costs some 1.5% for most of the next lap. Telemetry's `tyre_pickup` draws it.
- **Punctures** (`tyre_thermal::puncture`): a tyre worn to 100%; a hit of `PUNCTURE_HIT_PCT`
  or more (chance grows with the hit); debris (`GameSession::debris`, shed by hits of
  `DEBRIS_HIT_PCT`, picked up within `DEBRIS_REACH_M` with `DEBRIS_PUNCTURE_CHANCE`;
  `update_debris`; the piece also strikes the bodywork over that wheel, `DEBRIS_STRIKE_PCT`
  at 50 m/s on the nose or the tail, and scrubs `DEBRIS_STRIKE_SCRUB` of the speed, so a
  car does not drive through debris unmarked). Half are slow leaks (`TireData::leak_kpa_per_s`) charged through the
  pressure curve until flat at `PUNCTURE_FLAT_KPA`. The rate is the hole's, by its cause: debris
  in the tread `DEBRIS_LEAK_KPA_PER_S` (0.6-3.5 kPa/s), a hit's cut sidewall or bead
  `HIT_LEAK_KPA_PER_S` (2-8); `leak_rate_kpa_per_s` moves it as a fixed hole's flow, by
  `sqrt(T/T_opt · p_abs/p_set,abs · p/p_set)`: a hot, full tyre leaks faster, and the leak slows
  as the tyre goes down. Punctured: grip `PUNCTURE_GRIP` (0.35),
  pressure `PUNCTURED_KPA`, rolling drag `PUNCTURE_ROLLING_RESISTANCE` of its load.

### The AI and the tyres

`CarState::tyre_grip_share` (weaker axle's grip against new reference tyres in their window,
times the road's `surface_grip_share`, times `snow_tyre_share`: the fitted compound's
`snow_factor_with` on the cover and ice under the car's middle and the studs it has left, 1.0
for any tyre but a snow tyre)
scales the AI's profile speeds by its **0.75 power**
with `aero_load_share` (`ai_driver.rs`). Trap: the physically "right" square root put the AI
off the road more, because grip moves under the car as the tread heats through a corner. The
player's racing line is planned on warm tyres.

### car.toml `[tires]`

`optimal_temperature_c`, `temperature_window_c`, `temperature_grip_falloff`,
`blanket_temperature_c`, `optimal_pressure_kpa`, `pressure_front_kpa`, `pressure_rear_kpa`
(180 default; the shipped cars set them at the optimum, 175 Hypercar, 180 F1 and GT3, 185 LMP2),
`load_sensitivity` (`_front`/`_rear`), `reference_load_n`
(`_front_n`/`_rear_n`), `longitudinal_grip_factor`, `front_grip_scale`, `rear_grip_scale`;
`[[tires.compound]]`: `name`, `kind` (`slick`/`intermediate`/`wet`/`snow`), `grip`, `wear`,
`window_shift_c`, `water_grip = [dry, light, heavy]` (a snow kind's default `[0.8, 0.9, 0.85]`),
`reference`, `snow_grip`, `ice_grip`, `bare_wear`, `stud_loss` (0 when absent),
`window_scale` (1.0 each; `window_scale`
multiplies `temperature_window_c` for that compound, `tyre_thermal::window_c`, wherever the
window is read: grip, hot wear, blistering, the hysteresis fade and the AI's cold-tyre edge;
0.25-4), `heat_scale` and `rolling_heat_scale` (1.0 each, 0.25-4: `tyre_thermal::step`
multiplies the slip power's heat, tread and core, by `heat_scale` and the carcass's rolling
heat by `rolling_heat_scale`; the wear still goes by the slip's work), `pressure_shift_kpa`
(0, ±100: the compound grips best at `optimal_pressure_kpa` plus it,
`tyre_thermal::pressure_grip_factor`; the garage's pressures stay the car's, so a shifted
compound runs off its optimum until they are set for it), `load_sensitivity_front` /
`_rear` and `reference_load_front_n` / `_rear_n` (the car's `[tires]` figures when absent:
the compound's own exponent and reference load, which the tick grips with,
`CarConfig::compound_tyre_mu`; the racing line and the AI's grip envelope plan with the car's
reference compound's). `wear_rate` is a

`TireConfig` field, not a car.toml key. Shipped windows: F1 91 ± 13 °C on 70 °C blankets,
Hypercar 105 ± 13, LMP2 92 ± 10, GT3 87 ± 10, set where each class runs at AI race pace at
Monza.

### Checking tyres

In `server/sim/`:

- `tyre_thermal::tests`, `server/sim/tests/all/tyre_temperature_test.rs`.
- `cargo test --release --test all tyre_temperature_test::tyre_temperature_probe -- --ignored
  --nocapture` (`TYRE_PROBE_CARS`, `TYRE_PROBE_LAPS`, `TYRE_PROBE_TRACK`): per lap tyre,
  brake, coolant temperatures and time.
- `cargo test --release --test all tyre_temperature_test::overheat_recovery_probe -- --ignored
  --nocapture` (`COOK_TREAD`, `COOK_CORE`, `COOK_THROTTLE`, `COOK_BLIND`, `COOK_SECONDS`,
  `TYRE_PROBE_CARS`, `TYRE_PROBE_TRACK`): a lap at the AI's pace, then the set cooked and
  driven on, the tyres every 5 s. `COOK_BLIND=1` plans as if the tyres kept their grip, as a
  driver who keeps the pace does.
- `cargo test --release --test all pad_driver_probe:: -- --ignored --nocapture` (`PAD_AID`,
  `PAD_NOISE`, `PAD_HOLD_S`, `PAD_SKILL`, `PAD_CAR`, `PAD_TRACK`, `PAD_LAPS`, `PAD_FULL`):
  the AI's line plus a pad's hand error. A hotlap leaves no log or replay, so reproduce a
  player's hot-tyre report here; without the steering aid a pad runs on sliding fronts,
  which is handling, not the heat model.

## Fuel (`FuelConfig`, `GameSession::start_fuel_liters`)

- **Mass**: `mass_kg` is dry with driver; fuel rides on top (`CarConfig::laden`) for loads,
  lever arms, inertia, the steering aid and the collision impulse. `[fuel] tank_front_share`
  places the tank, so draining it moves the balance.
- **Burn** (`burn_lps`): crank power over `thermal_efficiency` x 43 MJ/kg x
  `density_kg_per_l` (0.745), plus idle; a lift burns idle only. F1 0.50, Hypercar 0.40,
  LMP2/GT3 0.33, default 0.30. `load_consumption_scale` without an efficiency keeps the
  legacy throttle-times-revs rule. A dry tank makes no combustion torque (a hybrid still
  drives).
- **Fill**: `racing_line::lap_fuel_liters` integrates the car's speed profile (planned half
  full, `build_laden`), cached in `GameSession::lap_fuel`. Race: distance x (1 +
  `RACE_FUEL_MARGIN` 8%) + `RACE_FUEL_RESERVE_LAPS`; hotlap and qualifying
  `HOTLAP_FUEL_LAPS` (3); practice a full tank; within `MIN_FUEL_LAPS` and the tank. Filled
  in `add_player`, `line_up_on_grid`, `hotlap_relocate` and on entering practice or
  qualifying. The margin is measured (`fuel_test.rs` pins each class's burn against the
  plan).
- **`fuel_load` knob**: laps over or under the fill, not a car figure, so `apply` ignores it
  and `changes_car()` (not `is_stock()`) decides on a tuned copy; it fills only in a hotlap
  garage or before the start.
- **Stint energy** (`[fuel] stint_energy_mj`, the WEC's energy per stint; absent, no limit):
  `CarState::stint_energy_used_mj` counts the fuel's chemical energy burnt (43 MJ/kg) plus
  what the motor deploys (telemetry `stint_energy_pct`, the HUD's `fuel.stint_pct`), reset with the hybrid's stint (`hybrid::new_stint`: a stop, a grid,
  the garage). Spent (`FuelConfig::stint_left_mj`), the engine cuts as on a dry tank and the
  motor stops. Fills and refuels stop at what a stint can burn (`pit::stint_fuel_cap`); the
  AI's short-of-fuel check reads `pit::usable_fuel_liters`. No shipped car names it; it is not
  on the wire.
- AI fuel plans and lift-and-coast: [AI](ai.md).
- car.toml `[fuel]`: `capacity_liters`, `idle_consumption_lps`, `thermal_efficiency`,
  `density_kg_per_l`, `tank_front_share`, `load_consumption_scale`, `stint_energy_mj`.
- Tests: `server/sim/tests/all/fuel_test.rs`; `a_full_tank_weighs_on_the_car`,
  `a_dry_tank_stops_the_engine`, `the_engine_burns_by_the_power_it_makes`,
  `a_stint_energy_limit_cuts_the_engine_with_fuel_in_the_tank`.

## Aero (`aero.rs`, `[aero]`)

There is no body heave, so ride height is computed (`ride_heights`): each axle's static
height less its load change (last tick's loads, carrying downforce, braking and
acceleration) over its springs, stiffening `BUMP_STIFFENING` x past `FREE_TRAVEL_SHARE` of
its height. The map (`multipliers_at`): `ride_height_sensitivity` more downforce per cm
below the reference, up to 40% given back below `stall_height_m`, `rake_sensitivity` of the
balance forward per cm of extra rake, induced drag a quarter of the change.

- **Reference** (`fit_reference`, at load): the ride at `REFERENCE_SPEED_MPS` (50 m/s) on
  stock springs makes exactly the car.toml downforce, so the class calibration holds.
- **Attitude** (`Posture`): `yaw_sensitivity` lost per degree of body slip,
  `roll_sensitivity` per degree of roll (floor 0.5).
- **Porpoising** (`step_porpoising`): within 1.6 stall heights above 50 m/s the downforce
  oscillates at `PORPOISE_HZ` (5), building over 0.6 s to `porpoising x PORPOISE_MAX_SWING`,
  dying in 0.3 s; the reported ride height swings ±15 mm.
- **Speed map** (`speed_multipliers`, `[[aero.speed_point]]`): wings that move with the
  car's speed (an AC import's `DYNAMIC_CONTROLLER`s) as multipliers on the front and rear
  downforce and the drag by the car's ground speed, linear between the points and held past
  either end, 1.0 at 50 m/s for an import (where its figures are taken). Applied on top of
  the posture in `calculate_aerodynamic_forces`; no points, exactly 1.0.
- **Input map** (`input_multipliers`, `[[aero.input_point]]`): wings that move with the
  driver (an air brake, a flap that stands up off the throttle or in a corner: an AC
  import's controllers on `BRAKE`, `GAS`, `STEER`, `LATG`) as multipliers on the same three
  by the last tick's throttle, brake, steering (a share of lock) and lateral g, each input
  linear between its own points and held past either end, the inputs multiplied together
  and into the speed map. Steer and lateral g are read by their size (no side-to-side
  aero). 1.0 flat out in a straight line (full throttle, no brake, no steer, no cornering),
  where an import's figures are taken; no points, exactly 1.0.
- **Damage**: front up to `FRONT_DAMAGE_AERO_LOSS` (half) of its downforce, rear up to 40%.
- The racing line uses the steady-state map on a straight car (`steady_downforce_factor`),
  the speed map's downforce in it, flat out (no input map); its drag stays the car.toml's.
- **Setup**: wings 5% of an axle's lift a click; ride heights 2 mm a click, never under 10
  mm (the reference stays the file's); springs move the aero too. No `[aero]`: wings work,
  ride heights do not.
- car.toml `[aero]`: `ride_height_front_m`, `ride_height_rear_m`, `ride_height_sensitivity`,
  `rake_sensitivity`, `stall_height_m`, `yaw_sensitivity`, `roll_sensitivity`, `porpoising`;
  `[[aero.speed_point]]`: `speed_mps` (rising, 0-150), `downforce_front`, `downforce_rear`,
  `drag` (0.2-3 each); `[[aero.input_point]]`: `input` (`throttle`, `brake`, `steer`,
  `lateral_g`), `at` (rising per input; 0-1, lateral g 0-6), `downforce_front`,
  `downforce_rear`, `drag` (0.2-3 each). Lift and drag stay in `[physics]` (`lift_coefficient_front/_rear`, `frontal_area_m2`,
  `drag_coefficient`). Shipped: F1 45/90 mm, 3%/cm, porpoising 0.6; Hypercar/LMP2 50/80 mm,
  2%/cm, 0.3; GT3 70/90 mm, 1%/cm, 0.
- Tests: `server/sim/tests/all/aero_test.rs`, `a_low_floor_porpoises_at_speed_and_a_raised_one_does_not`,
  `the_input_map_is_per_input_multiplied_and_absent_is_one`, `an_air_brake_stands_up_on_the_brake_pedal_only`.

## Slipstream and dirty air (`slipstream.rs`)

`slipstream::update` snapshots every car not in a garage and sets each `CarState::wake`
(`Wake { drag, downforce_front, downforce_rear }`, 1.0 in clean air) from the strongest wake
it sits in. **Tow**: up to `TOW_MAX` (35%) less drag bumper to bumper, falling by e every
`TOW_DECAY_M` (30 m). **Dirty air**: up to `DIRTY_AIR_MAX` (25%) less downforce, falling by
e every 15 m, 1.3x off the front and 0.7x off the rear. Gaussian across a wake that widens 4
cm/m; nothing past 100 m or from a leader under 10 m/s; less for a car not pointing down it;
scaled by the drag-area ratio. The snapshot makes it order-independent. The wake also slows
the coolant's and tyres' air; the AI reads it through `aero_load_share`; the racing line is
planned in clean air. Test: `server/sim/tests/all/slipstream_test.rs`.

## Brakes (`brakes.rs`)

- **Heat** (`CarState::brake_temp_c`): what the pads absorb (brake force capped at what the
  tyre carried, times rim speed: a locked wheel heats its tyre, not its disc), out through
  the duct (`brake_duct_scale`) and by radiation. Start: the air from the garage, carbon at
  300 °C / steel half way to its window on a grid, in the window for a hotlap.
- **Material** (`[brakes] material`, else `BrakeMaterial::for_class`: carbon for
  F1/Hypercar/LMP, else steel): carbon 0.55 cold, full 400-900 °C; steel 0.9 cold, full
  200-600 (`friction`).
- **Pads** (`[brakes] pads`, `brake_pads` knob): endurance 0.96 bite, 0.6x wear, window -40
  °C; sprint 1.04, 1.7x, +60 °C. **Wear** (`brake_wear_pct`) by absorbed energy, 10% bite
  lost over the life, `WORN_OUT_FRICTION` at 100%; a stop changes pads past
  `PAD_CHANGE_PCT`.
- **Discs** (`[brakes] disc_wear_per_mj`, percent per MJ absorbed; absent, discs never wear):
  `brake_disc_wear_pct` (telemetry `disc_wear`, 255 on a car without the key; the HUD's `item.disc_wear_pct`) by absorbed energy times the pads' wear scale, 1% faster per °C over
  `disc_oxidation_c` (carbon 650, steel 600). A worn disc holds less heat
  (`disc_capacity_scale`, 40% less at 100%) so runs hotter; at 100% it cracks
  (`WORN_OUT_FRICTION`). A stop past `DISC_CHANGE_PCT` fits discs and pads (`DISC_CHANGE_S`,
  counted in the repair time). No shipped car names the key; disc wear is not on the wire.
- **The AI** (`brake_share_of`) looks further ahead by the distance its pads lose. Trap:
  carbon is judged `CARBON_STOP_RISE_C` (100 °C) hotter than it is, since it comes in within
  the first moment of a stop; colder and the AI braked early everywhere, much hotter and it
  trusted cold grid carbon into turn 1.
- Tests: `brakes::tests`, `server/sim/tests/all/brake_heat_test.rs` (with
  `discs_wear_apart_from_the_pads_and_a_thin_one_runs_hot`), `the_rear_ducts_cool_the_rears_alone`.

## Engine heat (`engine_heat.rs`)

One mass (`CarState::engine_temp_c` / `water_temp_c`), heated by 0.9 W per watt of
combustion power plus idle, cooled by a radiator sized per car (`radiator_conductance`: 95
°C at 65% power, 60 m/s, 25 °C air) through the air the car meets times `wake.drag`, a fan
below 6 m/s. Traction control cuts the engine, not the tyre: the share of the drive asked
of the tyres that it let through (`traction_kept`) scales the combustion torque the coolant
and the fuel see, so a car held at peak slip on snow burns and heats for the drive it gets
(the car's motion is solved as before). Past `OVERHEAT_C` (112 °C) the engine loses 2% torque a degree (`power_factor`,
floor 60%) and takes damage. Out at `START_C` (80 °C). Keys: `[engine] radiator_scale`, the
`radiator` knob.

## Damage (`damage.rs`, `DamageState`)

Five percentages: front, rear, left, right, engine.

- **Impacts**: `impact_damage` of the closing speed, `0.2 x (v - 2.5)^1.6` (nothing under
  2.5 m/s, ~40% at 30), to the hit's zone; a nose hit puts `NOSE_TO_ENGINE` (0.3) on the
  engine.
- **Engine**: overheating (`OVERHEAT_DAMAGE_PER_C_S`), over-revving past 1.02x
  `max_engine_rpm` (`over_rev_damage` on `geared_rpm`, not in top gear: 400% a second per
  share past it, so 1.25x finishes an engine in about a second), and wear
  (`engine_wear`: ~3% an hour at the limiter, sixth power of the revs). Wear is kept apart
  (`engine_wear_percent`): a repair (`DamageState::repaired`) takes the engine back to its
  wear, not to zero.
- **Kerb strikes, bottoming, landings**: suspension speed filtered over `STRIKE_FILTER_S`
  (12 ms, `CarState::strike_mps`) so a one-tick road-mesh step does not count; a wheel on a
  kerb above `KERB_STRIKE_MPS` costs its side, one past its bump stop above
  `BOTTOMING_STRIKE_MPS` its end; `landing_damage` costs the ends by fall speed (free under
  2 m/s).
- **Session rule**: every accrual x `CarState::damage_scale()` from the session's
  `DamageLevel` (0, 0.5, 1); the engine's x `engine_damage_scale()`. Under
  `VisualOnly` the body is dented as under Full and the engine takes nothing, and
  every effect below reads `CarState::felt_damage()`, which is no damage: the car
  is drawn hurt and drives whole ([sessions](sessions.md#session-rules-set-on-create)).
- **Effects** (1.0 undamaged): front downforce and up to 60% of the radiator; rear up to 40%
  rear downforce; a side's tyres up to 12% grip (`side_grip_factor`) and the fronts toed
  toward it (`toe_offset_rad`); engine up to 40% power (`engine_power_factor`).
- **Out** at 100% in any zone (`DamageState::refresh`): `update_car_3d` stops the car where
  it is (velocities and `speed_mps` zeroed, so the telemetry reads it stopped) and returns; after
  `TOW_AFTER_S` (10 s) `update_retirements` moves the car to its pit box (if there is a
  lane) and out of every collision, wake, DRS and pit pass (`CarState::towed`).
- **Visible damage** is drawn by the client from the same percentages (dents,
  `[[damage_part]]` pieces, smoke): [cars](../../../apexsim-content/docs/cars.md).
- Tests: `damage::tests`, `server/sim/tests/all/damage_test.rs`,
  `a_landing_costs_the_ends_and_a_drop_is_free`,
  `a_hard_hit_sheds_debris_the_next_car_picks_up`.

## Suspension geometry (`geometry.rs`, `[suspension]`)

Four vertical spring/damper units; roll is computed from lateral load and roll stiffness
(`body_roll_rad`).

- **Camber**: static camber plus roll less the linkage's gain (`camber_gain_*`: 0 a beam,
  ~0.5 a double wishbone), less the carcass's lean. Lateral grip peaks at `OPTIMAL_LEAN_DEG`
  (2°) into the corner, falling quadratically; braking and traction lose a little with any
  camber (`camber_grip`). Normalised to the filed camber at the car's reference cornering.
  No camber keys: no model (`SuspensionConfig::camber_modelled`; the garage hides the rows).
- **Camber thrust** (`camber_thrust`): a slip-angle offset toward the lean
  (`camber_thrust_rad`).
- **Toe** (degrees per wheel, positive in): fronts on top of the steering, rears on their
  own (`toe_steer_rad`).
- **Roll centres**: lateral transfer splits into a geometric part through each axle's roll
  centre and an elastic part through springs and bars (`lateral_transfer`); without them it
  is the old formula. No migration, jacking or instant centres, on purpose.
- **Bump stops**: `bump_stop_force_n` past the gap.
- car.toml `[suspension]`: `spring_rate_*_n_per_m`, `damper_compression_*`,
  `damper_rebound_*`, `anti_roll_bar_*`, `max_travel_m`, `camber_*_deg`, `camber_gain_*`,
  `toe_*_deg`, `bump_stop_gap_m` (or `_front_m`/`_rear_m`), `bump_stop_rate_n_per_m`,
  `roll_centre_*_m`, `camber_thrust` (`*` = `front`/`rear`).
- Tests: `geometry::tests`, `server/sim/tests/all/geometry_test.rs`.

## Differential (`split_axle_torque`, `[differential]`)

Both wheels of an axle roll at one speed in the slip solution, so the differential only
decides the split once one tyre cannot carry half: open gives the other wheel no more than
that, locked everything the weak one cannot use, clutch / viscous / Torsen at most
`preload_nm + lock_power` (or `lock_coast`) x axle torque more (Assetto Corsa's semantics).
Only with `simulated = true` (every shipped car), else an even split. Keys:
`differential_type` (`Open`, `Locked`, `ClutchLSD`, `ViscousLSD`, `Torsen`), `preload_nm`,
`lock_power`, `lock_coast`, `simulated`. No knob.

## Hybrid and ERS (`hybrid.rs`, `[hybrid]`)

- **Modes** (`ErsMode`, `PlayerInput.ers_mode`, default Balanced): Harvest never deploys and
  charges on the throttle (`HARVEST_SHARE`); Balanced deploys from 60-95% throttle, eased
  out below 35% charge, paced against the lap budget as (budget left + 5%) / (lap left + 5%)
  of the torque; Attack deploys at any throttle until budget or battery is gone. The
  overtake button (`ers_boost`, held) is Attack. Trap: Balanced must taper; a step cut every
  F1's motor at the same place and the field piled up.
- **Budgets**: `deploy_kj_per_lap` (between line crossings), `stint_kj` (between stops;
  `new_stint`), `override_kj_per_lap` (the button's own per-lap allowance),
  `deploy_min_speed_kph` (WEC).
- **Recovery**: under braking up to `REGEN_AXLE_SHARE` (0.9) of the power the driven axle's
  tyres actually put down last tick (`CarState::driven_axle_brake_w`). With `brake_by_wire`
  (default true) it replaces that axle's hydraulics (the pedal feels the same, the discs run
  cooler); without, it brakes on top. `bbw_response_s` (default 0: at once) is how long the
  hydraulics take to follow the recovery (`bbw_offset_n`, a first-order lag): while they catch
  up the balance migrates, rearward as the recovery comes in at the start of a stop and
  forward as it drops away when the battery fills. Coasting charges at `COAST_SHARE`; `heat_recovery_kw`
  (MGU-H) charges at full throttle.
- **Setup** ([car setup](#car-setup-car_setuprs-setup_sheetrs)): `ers_regen` lowers
  `regen_max_power_kw` (down only: recovery is capped at `battery_max_charge_kw`, which
  every shipped hybrid files equal to it); `ers_deploy_map` sets `HybridConfig::deploy_early` (setup only, never
  the car.toml), which bends Balanced's "lap left" to `1 - lap_share^(1 - deploy_early)`
  (`spend_target`; exactly the even pace at 0), so a positive map spends the budget earlier;
  `ers_start_mode` is put on the car with the tyres (`fit_tyres`: AI and stock Balanced).
  The ERS key changes the mode for the current run: the client sends the picked mode
  (`PlayerInput.ers_mode`) until telemetry shows the car in it, then sends none, so the
  next run starts in the setup's mode again.
- `charge_full` refills with the tyres. The racing line (the player's aid `racing_line::build`,
  and the AI's plan and the fuel estimate `build_laden`) counts the motor as the tick gives it
  (`hybrid::MotorPlan`): nothing below the minimum speed, its torque curve at the revs it turns
  at (a crank motor in the gear the automatic box holds at that speed, upshifting at
  `physics::auto_upshift_rpm`; a front motor at the front wheels'), capped by its power and the
  battery's; and as Balanced paces the lap budget: it plans at full power, drives that lap
  through `hybrid::balanced_pace` from the line (`paced_shares`, spending only on the throttle)
  and replans on the share each point gets, up to three rounds. A budget the lap never reaches
  plans as none. The plan keeps its first, unpaced round as `unpaced_mps`, which the AI caps
  itself on (its skill's share of it): a cap at a share of the paced plan's straights lifts
  short of what the car does (2 s a lap at Monza in an F1, lone AI at skill 110). The AI boosts within `AI_BOOST_GAP_S` of the car ahead and
  `AI_DEFEND_BOOST_GAP_S` of the car behind, and harvests when low and alone (`ai_ers`,
  [AI](ai.md)).
- **Axle** (`axle`, `HybridAxle`): `"crank"` (the default) puts the motor behind the clutch
  and through the gearbox to the driven wheels; `"front"` puts it straight on the front axle
  (an LMH's motor, `MotorOutput::front_axle_torque_nm`): it turns with the front wheels, so
  its power limit is at their speed, `motor_max_torque_nm` is the torque at the axle, and it
  drives whatever the gear or the clutch. An RWD car is then driven at both ends while it
  deploys (above `deploy_min_speed_kph`); coasting and Harvest drag the fronts, and under
  braking it recovers through the front tyres, so brake-by-wire takes the recovery off the
  front hydraulics. The front differential locks by the front axle's own torque. The racing
  line's envelope gives an RWD car's front motor the fronts' share of the grip beside the
  engine's rear share (`CarEnvelope::motor_front_fraction`; FWD and AWD cars pool it as for a
  crank motor), and the AI's traction cap watches the front slip too while it deploys. No
  shipped car uses it; an AC import with `[FRONT_MOTORS]` does.
- **Torque curve** (`[[hybrid.torque_curve]]`, `rpm` / `torque_nm` as the engine's): the
  motor's torque by its own rpm (the crank's, or the front wheels' on the front axle), capped
  at `motor_max_torque_nm` (`HybridConfig::torque_at`), for deploying and recovering alike.
  Without one the motor gives its peak at every speed up to the power limit. No shipped car
  files one; AC imports do (`ers.ini` `TORQUE_CURVE`). The racing line and the AI's plan
  count it at the revs the motor turns at (`MotorPlan`, above).
- **A second motor at the crank** (`crank_motor_max_torque_nm`, `crank_motor_max_power_kw`,
  0 each when absent; read only with `axle = "front"`, `HybridConfig::has_crank_motor`):
  beside the front one, behind the clutch in gear, at its torque at the crank's rpm (its own
  curve, `[[hybrid.crank_torque_curve]]`, capped at its peak, `HybridConfig::crank_torque_at`;
  its peak without one) up to its power at the crank's speed, on the same deploy share,
  budget, stint and battery; the two together are held to `battery_max_discharge_kw`. Only
  the front motor recovers. The plan and the drag-limited top speed count both. No shipped
  car has one; an AC import with `[FRONT_MOTORS]` and a `[KINETIC]` curve (the 918, the
  TS040) does.
- **Delivery profile** (`[[hybrid.delivery_point]]`: `input` `throttle` / `speed_kph` /
  `gear` / `rpm`, `at` rising per input, `share` 0-2; none when absent,
  `hybrid::delivery_share`): the car's own electronics as an AC import's default
  `ctrl_ers_*.ini` has them. Each input's map is linear between its points and held past
  either end; the speed's, the gear's and the engine rpm's multiply what the mode asks for,
  and the throttle's caps the mode's own pedal (Balanced's 60-95% ramp, Attack's and the
  button's straight pedal), the product held to the whole motor: a profile only takes
  deployment away. One profile drives both motors. The plan reads it flat out at each speed
  in the gear the box holds. Without one, the modes alone, exactly as before.
- car.toml `[hybrid]`: `enabled`, `battery_capacity_kwh`, `battery_max_discharge_kw` /
  `_charge_kw`, `motor_max_torque_nm`, `motor_max_power_kw`, `regen_max_power_kw` and the
  keys above.
- Tests: `hybrid::tests` (`the_crank_motor_follows_its_own_curve`,
  `a_delivery_profile_shapes_what_the_mode_asks_for`), `brake_by_wire_takes_the_recovered_energy_off_the_discs`,
  `a_slow_brake_by_wire_moves_the_balance_forward_as_the_battery_fills`.

## DRS (`drs.rs`, `GameSession::update_drs`)

Zones (`TrackConfig::drs_zones`: detection, activation, end) come from the track YAML
([track pipeline](../../../apexsim-content/docs/track-pipeline.md)). Crossing a detection line arms the zone:
in a race only within `DRS_GAP_S` (1 s) of any car ahead (`gap_ahead_s`); in practice,
qualifying and hotlap always. Between activation and end an armed car is
`CarState::drs_allowed`, and `update_car_3d` opens the flap while `PlayerInput.drs` is held
and the brake is under `DRS_BRAKE_CLOSE`. Nothing opens in the session's rain, before lap
`DRS_RACE_FROM_LAP` of a race, or in the garage. An F1 gets `DrsSpec::F1` (12% drag, 25%
rear downforce off) unless `[physics] drs_drag_reduction` / `drs_rear_downforce_reduction`
say otherwise (0 drag switches it off); other classes have none. The AI opens whenever
allowed; the racing line uses open-flap drag in the zones. Telemetry: `lap_flags` bits 3-4.
Client: the `Drs` action, `FApexCarTelemetry::bDrsAllowed` / `bDrsOpen`, a HUD badge, and
the F1 flap mesh (`[drs_flap]`, `FApexDrsFlapSpec`, [cars](../../../apexsim-content/docs/cars.md)).

## Car setup (`car_setup.rs`, `setup_sheet.rs`)

A setup is **clicks** off the car.toml, one `i8` per knob (`CarSetup`, `KNOBS`, `KNOB_COUNT`
= 31), so the wire and client need no base figures. Wire order (groups appended, never
reordered; effects are the `*_PER_CLICK` consts):

| # | Knob | Range | Per click |
|---|---|---|---|
| 0-1 | `tyre_pressure_front/_rear` | ±5 | 5 kPa |
| 2 | `rev_limiter` | -5..0 | -100 rpm (redline too) |
| 3 | `engine_braking` | ±5 | 15% |
| 4-5 | `final_drive`, `gear_spread` | ±5 | 2% |
| 6 | `torque_map` | -5..0 | -4% |
| 7 | `brake_bias` | ±5 | 1% front |
| 8-13 | `spring_*`, `damper_*`, `anti_roll_*` | ±5 | 4%, 5%, 8% |
| 14 | `fuel_load` | ±5 | 1 lap |
| 15-16 | `front_wing`, `rear_wing` | ±5 | 5% lift |
| 17-18 | `ride_height_front/_rear` | ±5 | 2 mm |
| 19 | `tyre_compound` | -3..+1 | `compound_for_click` |
| 20, 25 | `brake_ducts`, `brake_ducts_rear` | ±5 | 10% cooling |
| 21-22 | `camber_front/_rear` | ±5 | -0.25° |
| 23-24 | `toe_front/_rear` | ±5 | 0.05° |
| 26 | `brake_pads` | -1..+1 | endurance/standard/sprint |
| 27 | `radiator` | ±5 | 8% |
| 28 | `ers_start_mode` | -1..+1 | harvest/balanced/attack |
| 29 | `ers_regen` | -5..0 | -10% regen power |
| 30 | `ers_deploy_map` | ±5 | 0.1 of `deploy_early` (+ earlier) |

- **Flow**: the client sends `SetCarSetup` on join and on change; the server clamps
  (`CarSetup::clamp`) and bakes a tuned `CarConfig` (`CarSetup::apply`) into
  `GameSession::tuned_configs`, looked up by `simulated_config`, so the hot loop pays one
  map lookup. It applies at once (fuel and compound wait for the next fill or set). AI cars
  and the collision passes use the shared config.
- **The sheet**: `ServerMessage::CarSetupSheet` (`CarSetupSheetData`, once after
  `SessionJoined`): each knob's stock, step and bounds in display units, gear ratios, wheel
  radius, lap fuel, rake balance, `CamberModelled`, `Compounds`, `ReferenceCompound`, and for a
  hybrid `HybridBatteryKwh`, `HybridMotorKw`, `HybridLapBudgetKj`, `HybridDeployMinKph` (off
  the wire at 0; no battery means no hybrid). Knobs
  are linear in clicks, so the client predicts without a round trip
  (`the_sheet_predicts_what_apply_does`).
- **Client**: the hotlap garage (`UApexHotlapWidget`: Tyres / Suspension / Engine / Aero /
  ERS / Save tabs of `UApexStepperWidget` rows in the sheet's units; Aero holds the wings and ride
  heights with the wing downforce, aero balance, rake and drag change they add up to, the drag
  from the `*_DRAG_PER_CLICK` consts mirrored in the widget; ERS, shown only when the sheet
  has a battery (or before a sheet), holds the three hybrid knobs, the system as filed and
  the share of the budget the deploy map spends by half distance) edits
  `UApexSettingsSave::CarSetup`, one setup for every car, with named setups per car in
  `SavedSetups`; the knob table mirror is `ApexCarSetup::FKnob`
  (`ApexSim.Net.CarSetup.Clicks`). More in [sessions](sessions.md).
- **Sharing**: the Load / Save tab's COPY AS TEXT puts the picked setup on the clipboard
  as one line (`ApexSetupShare`: `APXS1;car=<id>;crc=<car.toml checksum>;name=...;c=<clicks>`)
  and in `Saved/Setups/<car>-<name>.txt`; PASTE reads such a line from anywhere in the
  clipboard's text (a chat message, backticks) and saves it as a new setup for the car in
  the garage. A line for another car is refused (clicks are steps off one car.toml); one
  from another version of the car (its `crc` against the catalog row's `SourceCrc`) is
  taken with its clicks pinned into range and a warning. Tests: `ApexSim.SetupShare.*`.
- Tests: `car_setup::tests`, `setup_sheet::tests`.

## Wire fields

`CompactCarState` (positional, appended only; [protocol](protocol.md)): `fuel_dl`, `tyre_c`,
`tyre_kpa`, `tow_pct`, `tyre_wear`, `compound`, `brake_c`, `water_c`, `damage`, `ers_pct`,
`ers_lap_pct`, `ers_flags`, `tyre_c_edges`, `brake_wear`, `slide_flags`, `ers_stint_pct`,
`recover_ds` ([sessions](sessions.md#recovering-a-stuck-car)), `flat_spot`, `damage_at`,
`tyre_water`, `tyre_marbles` (the road under each tyre, `CarState::tyre_water` /
`tyre_marbles`, percent), `tyre_state` (bit 0 punctured, bit 1 a slow leak under way),
`tyre_pickup`, `tyre_blister`, `damage_at2` (each zone's second dent,
[cars](../../../apexsim-content/docs/cars.md#visible-damage))
(pit fields: [pit lane](pit-lane.md)); `PlayerInput.drs` / `ers_mode` / `ers_boost`;
`DriverFeedback.flat_spot`. Golden bytes (in `server/protocol/`, `-- --nocapture`): `cargo test
telemetry_compact_wire_format` -> `ApexUdpGolden::S_TelemetryCompactFlatSpot` (the older
`S_TelemetryCompact{Fuel,Tyres,Tow,Heat,Damage,Ers,Zones,Recover}` stay on the client as older servers'
frames); `car_setup_wire_format` -> `ApexGolden::C_SetCarSetup`;
`car_setup_sheet_wire_format` -> `S_CarSetupSheet`; `player_input_headlights_wire_format` ->
`ApexUdpGolden::C_PlayerInput`; `driver_feedback_wire_format` -> `S_DriverFeedbackFlatSpot`.

## Checking it

- `cargo test` in `server/sim/` runs the module tests and the per-area files named above, plus
  `car_stability_test`, `air_test` and `determinism_test`; `cargo test` in `server/server/` runs
  `physics_property_tests`.
- In `server/sim/`, `cargo test --release --test all grip_probe_test::silverstone_profile_lap_times -- --ignored
  --nocapture` (`PROBE_CLASS`, `PROBE_CAR`, `PROBE_MU_SCALE`, `PROBE_CLA_SCALE`): every
  car's ideal lap, the class grip calibration. Also `skidpad_sweep`,
  `silverstone_first_corner`, `steering_feel_probe`.
- A change to grip, aero or walls goes through the AI survey ([AI](ai.md)); compare totals,
  since single pile-ups dominate a circuit.

## Traps

- A new model must be exactly 1.0 (or absent) without its keys and at the car's reference
  state: the class calibration and unit tests rely on it.
- Weight transfer, tyre grip and ride height read last tick's values on purpose.
- Road-mesh steps give one-tick suspension-speed spikes; anything reading suspension speed
  (damage, sound) must filter it.
- `[tires]`, `[aero]`, `[brakes]` and `[[tires.compound]]` deny unknown keys: a typo fails
  the car's load.
- `car_setup.rs`'s module doc says there is no differential model and some comments name
  `tyre_thermal::COMPOUNDS`: both stale. The differential runs (no knob); compounds are
  `TireConfig::compounds()`.
