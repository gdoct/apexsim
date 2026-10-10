# Vehicle physics

The server owns the car model: each tick `physics::update_car_3d` advances a car's
`CarState` from its driver's input, against its `CarConfig` (from `car.toml`) and the
session's `TrackConfig`. The client has no physics. Everything is a pure function of the
tick's inputs (no clock, no RNG; chances are `wind::hash01` of tick and car), so
`tests/determinism_test.rs` holds. A car.toml without a newer table or key simulates exactly
as before that feature: every model defaults to off or to a multiplier of exactly 1.0 at the
car's reference state.

## Code

- `server/src/physics.rs`: `update_car_3d`, tyre force solve, surface queries, collisions,
  steering aid, gearbox, track progress.
- `server/src/data.rs`: `CarConfig` and its sub-configs, `CarState`, `TireData`,
  `DamageState`, `DrsSpec`. `car_loader.rs`: the car.toml schema.
- Per subsystem: `tyre_thermal.rs`, `aero.rs`, `slipstream.rs`, `brakes.rs`,
  `engine_heat.rs`, `damage.rs`, `geometry.rs`, `hybrid.rs`, `drs.rs`, `car_setup.rs`,
  `setup_sheet.rs`; `game_session.rs` for what runs around the physics (fuelling, tyre
  fitting, tuned configs).

Related: [conditions](conditions.md) (weather, air, wind, road state), [AI](ai.md), [pit
lane](pit-lane.md), [sessions](sessions.md) (assist and damage rules),
[protocol](protocol.md), [cars](../content/cars.md), [road mesh](../content/road-mesh.md),
[force feedback](../game/input-and-ffb.md).

## The tick

`GameSession::tick_racing` / `tick_free_practice` / `tick_hotlap` run `update_air`
(`update_retirements`, `update_pits`, `update_drs`, `update_wind`, `update_road`,
`update_debris`, `slipstream::update`, `update_racecraft`), then `update_car_3d` per car
with `simulated_config` (the driver's tuned config, else the shared one), then
`update_track_progress_3d` ([sessions](sessions.md)), then `check_collisions_refs` and
`check_wall_collisions`.

## The car model (`physics.rs`)

`update_car_3d`'s numbered comments are the map. After the early outs (not drivable; held
still while serviced) and the gearbox, limiter and DRS flap:

- **Loads**: from the laden mass (`CarConfig::laden`), axle lever arms at the CoG split.
  Weight transfer uses the previous tick's accelerations; lateral transfer goes through
  `geometry::lateral_transfer`. **Crests**: the contact plane's filtered pitch rate times
  speed is the body's vertical acceleration (`CarState::vertical_accel_mps2`, ±1.5 g) and
  the static loads scale by `1 + a/g`, so a crest unloads the car (fast enough, it leaves
  the ground) and a flat road is unchanged (`a_crest_unloads_the_car_and_a_dip_loads_it`).
- **Aero** (`calculate_aerodynamic_forces`): drag and per-axle downforce through the
  ride-height map, attitude, porpoising, wake, air density and wind (drag along the relative
  airflow, a side force on the flank).
- **Engine**: torque curve, turbo spool, limiter, `engine_density_factor`, coolant and
  damage factors; the hybrid motor; drive torques through the drivetrain and differential.
  **Brakes**: demand split by `brake_bias_front`, scaled per wheel by pad friction.
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
  puncture) x camber grip x load sensitivity (`CarConfig::axle_tyre_mu`).
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
| `Off` | grass/gravel, patchy (`grass_grip_patch`), rolling drag | yes |
| `PitLane` | road | no (a lap with a stop counts) |

Widths come from the curbs sidecar (centerline backend) or triangle tags (mesh,
`RoadContact::from_surface`); on the centerline backend the pit sidecar's strip is `PitLane`
(`on_pit_lane_strip`).

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
- Out: air convection with speed^0.8 (the air the car meets, wind and wake included:
  `TyreWork::air_mps`), road conduction with the root of speed, several times more on a wet
  road.
- Zones: `zone_shares` loads the inner shoulder with negative camber, the outer under
  lateral load, the middle when over-inflated. Grip reads the mean, so an evenly heated tyre
  is the one-node model.
- Grip is read from the end of the last tick; heat steps after the solve.

Trap: the sub-peak split by mass is deliberate. Most of it in the thin tread made a careful
driver's tyres jump 10 °C per gentle turn while the AI stayed cool; giving part to the road
moved every class's window, which the AI's pace sets.
`the_surface_swings_like_a_real_tyre_over_a_lap` pins it.

### Grip and pressure

Grip reads 0.7 tread + 0.3 core (`TREAD_GRIP_SHARE`): full inside `optimal_temperature_c ±
temperature_window_c`, less `temperature_grip_falloff` a degree outside (eased in, the cold
side charged 0.6 of it, at most 25% off). Pressure is the gas law on the core
(`pressure_kpa`); the setup's pressures are the *hot* ones, so a cold tyre is soft and a
cooked one over-inflated, and `TireConfig::pressure_grip_factor` charges both quadratically
off `optimal_pressure_kpa`. A tyre at its optimum and set pressure grips as if there were no
thermal model, which is what an unfitted car gets (`CarState::tyres_fitted` false).

### Fitting (`game_session::fit_tyres`, `tyre_thermal::fit`)

Fitting zeroes wear, flat spots and punctures and sets the compound. From the garage or a
practice join: `blanket_temperature_c` or the air. On a race grid: half way to the optimum
(`FORMATION_LAP_WARMTH`, there is no formation lap). A hotlap out: the compound's optimum,
unless `HotlapRelocate.cold_tyres`. Brakes and the hybrid battery are fitted with them.

### Compounds

`TireConfig::compounds()` is the car's own `[[tires.compound]]` list in knob order (slicks
softest first, treaded last), else `default_compounds()` (soft, medium, hard, intermediate,
wet; medium the reference; no shipped car lists its own). Each changes the car's tyre: grip,
wear, window shift, `water_grip` (dry / light / heavy rain, linear between; past 1.0 water a
puddle costs more). The **reference** compound is the car's tyre exactly, so calibration
holds.

The road's baked grip is for the tyre the weather calls for (`weather_compound_of`:
intermediate below `WET_FROM_WATER`, wet from it, none under `SLICK_BELOW_WATER`);
`Compound::grip_on` charges any other. A car with no treaded tyre races on slicks in the
rain. `CarState::tyre_compound` is what is fitted; the `tyre_compound` knob chooses the next
set (`CarSetup::compound_index_for`: a stock pick in the rain is the weather's tyre). The AI
always takes the weather's tyre; when it changes is in [AI](ai.md).

### Wear, flat spots, punctures

- **Wear** grows with sliding friction power (`WEAR_PERCENT_PER_MJ` x the compound's wear x
  `TireConfig::wear_rate`, 4% more per degree over the window). `wear_grip_factor`: 6% over
  the life plus a quadratic 24% cliff from 70% worn.
- **Flat spots**: braking past `LOCK_SLIP_RATIO` above 3 m/s grows `TireData::flat_spot`
  (0..1) by the locked patch's energy; up to 4% grip; wears round with the tread
  (`FLAT_SPOT_ROUNDING_PER_PERCENT`). Sent as `DriverFeedback.flat_spot` (wheel, pad and
  road sound play it).
- **Punctures** (`tyre_thermal::puncture`): a tyre worn to 100%; a hit of `PUNCTURE_HIT_PCT`
  or more (chance grows with the hit); debris (`GameSession::debris`, shed by hits of
  `DEBRIS_HIT_PCT`, picked up within `DEBRIS_REACH_M` with `DEBRIS_PUNCTURE_CHANCE`;
  `update_debris`). Half are slow leaks (`TireData::leak_kpa_per_s`) charged through the
  pressure curve until flat at `PUNCTURE_FLAT_KPA`. Punctured: grip `PUNCTURE_GRIP` (0.35),
  pressure `PUNCTURED_KPA`, rolling drag `PUNCTURE_ROLLING_RESISTANCE` of its load.

### The AI and the tyres

`CarState::tyre_grip_share` (weaker axle's grip against new reference tyres in their window,
times the road's `surface_grip_share`) scales the AI's profile speeds by its **0.75 power**
with `aero_load_share` (`ai_driver.rs`). Trap: the physically "right" square root put the AI
off the road more, because grip moves under the car as the tread heats through a corner. The
player's racing line is planned on warm tyres.

### car.toml `[tires]`

`optimal_temperature_c`, `temperature_window_c`, `temperature_grip_falloff`,
`blanket_temperature_c`, `optimal_pressure_kpa`, `pressure_front_kpa`, `pressure_rear_kpa`
(180 default, in no shipped car), `load_sensitivity` (`_front`/`_rear`), `reference_load_n`
(`_front_n`/`_rear_n`), `longitudinal_grip_factor`, `front_grip_scale`, `rear_grip_scale`;
`[[tires.compound]]`: `name`, `kind` (`slick`/`intermediate`/`wet`), `grip`, `wear`,
`window_shift_c`, `water_grip = [dry, light, heavy]`, `reference`. `wear_rate` is a
`TireConfig` field, not a car.toml key. Shipped windows: F1 91 ± 13 °C on 70 °C blankets,
Hypercar 105 ± 13, LMP2 92 ± 10, GT3 87 ± 10, set where each class runs at AI race pace at
Monza.

### Checking tyres

- `tyre_thermal::tests`, `tests/tyre_temperature_test.rs`.
- `cargo test --release --test tyre_temperature_test tyre_temperature_probe -- --ignored
  --nocapture` (`TYRE_PROBE_CARS`, `TYRE_PROBE_LAPS`, `TYRE_PROBE_TRACK`): per lap tyre,
  brake, coolant temperatures and time.
- `cargo test --release --test pad_driver_probe -- --ignored --nocapture` (`PAD_AID`,
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
- AI fuel plans and lift-and-coast: [AI](ai.md).
- car.toml `[fuel]`: `capacity_liters`, `idle_consumption_lps`, `thermal_efficiency`,
  `density_kg_per_l`, `tank_front_share`, `load_consumption_scale`.
- Tests: `tests/fuel_test.rs`; `a_full_tank_weighs_on_the_car`,
  `a_dry_tank_stops_the_engine`, `the_engine_burns_by_the_power_it_makes`.

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
- **Damage**: front up to `FRONT_DAMAGE_AERO_LOSS` (half) of its downforce, rear up to 40%.
- The racing line uses the steady-state map on a straight car (`steady_downforce_factor`).
- **Setup**: wings 5% of an axle's lift a click; ride heights 2 mm a click, never under 10
  mm (the reference stays the file's); springs move the aero too. No `[aero]`: wings work,
  ride heights do not.
- car.toml `[aero]`: `ride_height_front_m`, `ride_height_rear_m`, `ride_height_sensitivity`,
  `rake_sensitivity`, `stall_height_m`, `yaw_sensitivity`, `roll_sensitivity`, `porpoising`.
  Lift and drag stay in `[physics]` (`lift_coefficient_front/_rear`, `frontal_area_m2`,
  `drag_coefficient`). Shipped: F1 45/90 mm, 3%/cm, porpoising 0.6; Hypercar/LMP2 50/80 mm,
  2%/cm, 0.3; GT3 70/90 mm, 1%/cm, 0.
- Tests: `tests/aero_test.rs`, `a_low_floor_porpoises_at_speed_and_a_raised_one_does_not`.

## Slipstream and dirty air (`slipstream.rs`)

`slipstream::update` snapshots every car not in a garage and sets each `CarState::wake`
(`Wake { drag, downforce_front, downforce_rear }`, 1.0 in clean air) from the strongest wake
it sits in. **Tow**: up to `TOW_MAX` (35%) less drag bumper to bumper, falling by e every
`TOW_DECAY_M` (30 m). **Dirty air**: up to `DIRTY_AIR_MAX` (25%) less downforce, falling by
e every 15 m, 1.3x off the front and 0.7x off the rear. Gaussian across a wake that widens 4
cm/m; nothing past 100 m or from a leader under 10 m/s; less for a car not pointing down it;
scaled by the drag-area ratio. The snapshot makes it order-independent. The wake also slows
the coolant's and tyres' air; the AI reads it through `aero_load_share`; the racing line is
planned in clean air. Test: `tests/slipstream_test.rs`.

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
- **The AI** (`brake_share_of`) looks further ahead by the distance its pads lose. Trap:
  carbon is judged `CARBON_STOP_RISE_C` (100 °C) hotter than it is, since it comes in within
  the first moment of a stop; colder and the AI braked early everywhere, much hotter and it
  trusted cold grid carbon into turn 1.
- Tests: `brakes::tests`, `tests/brake_heat_test.rs`, `the_rear_ducts_cool_the_rears_alone`.

## Engine heat (`engine_heat.rs`)

One mass (`CarState::engine_temp_c` / `water_temp_c`), heated by 0.9 W per watt of
combustion power plus idle, cooled by a radiator sized per car (`radiator_conductance`: 95
°C at 65% power, 60 m/s, 25 °C air) through the air the car meets times `wake.drag`, a fan
below 6 m/s. Past `OVERHEAT_C` (112 °C) the engine loses 2% torque a degree (`power_factor`,
floor 60%) and takes damage. Out at `START_C` (80 °C). Keys: `[engine] radiator_scale`, the
`radiator` knob.

## Damage (`damage.rs`, `DamageState`)

Five percentages: front, rear, left, right, engine.

- **Impacts**: `impact_damage` of the closing speed, `0.2 x (v - 2.5)^1.6` (nothing under
  2.5 m/s, ~40% at 30), to the hit's zone; a nose hit puts `NOSE_TO_ENGINE` (0.3) on the
  engine.
- **Engine**: overheating (`OVERHEAT_DAMAGE_PER_C_S`), over-revving past 1.02x
  `max_engine_rpm` (`over_rev_damage` on `geared_rpm`, not in top gear), and wear
  (`engine_wear`: ~3% an hour at the limiter, sixth power of the revs). Wear is kept apart
  (`engine_wear_percent`): a repair (`DamageState::repaired`) takes the engine back to its
  wear, not to zero.
- **Kerb strikes, bottoming, landings**: suspension speed filtered over `STRIKE_FILTER_S`
  (12 ms, `CarState::strike_mps`) so a one-tick road-mesh step does not count; a wheel on a
  kerb above `KERB_STRIKE_MPS` costs its side, one past its bump stop above
  `BOTTOMING_STRIKE_MPS` its end; `landing_damage` costs the ends by fall speed (free under
  2 m/s).
- **Session rule**: every accrual x `CarState::damage_scale()` from the session's
  `DamageLevel` (0, 0.5, 1).
- **Effects** (1.0 undamaged): front downforce and up to 60% of the radiator; rear up to 40%
  rear downforce; a side's tyres up to 12% grip (`side_grip_factor`) and the fronts toed
  toward it (`toe_offset_rad`); engine up to 40% power (`engine_power_factor`).
- **Out** at 100% in any zone (`DamageState::refresh`): `update_car_3d` returns early; after
  `TOW_AFTER_S` (10 s) `update_retirements` moves the car to its pit box (if there is a
  lane) and out of every collision, wake, DRS and pit pass (`CarState::towed`).
- **Visible damage** is drawn by the client from the same percentages (dents,
  `[[damage_part]]` pieces, smoke): [cars](../content/cars.md).
- Tests: `damage::tests`, `tests/damage_test.rs`,
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
- Tests: `geometry::tests`, `tests/geometry_test.rs`.

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
  cooler); without, it brakes on top. Coasting charges at `COAST_SHARE`; `heat_recovery_kw`
  (MGU-H) charges at full throttle.
- **Setup** ([car setup](#car-setup-car_setuprs-setup_sheetrs)): `ers_regen` lowers
  `regen_max_power_kw` (down only: recovery is capped at `battery_max_charge_kw`, which
  every shipped hybrid files equal to it); `ers_deploy_map` sets `HybridConfig::deploy_early` (setup only, never
  the car.toml), which bends Balanced's "lap left" to `1 - lap_share^(1 - deploy_early)`
  (`spend_target`; exactly the even pace at 0), so a positive map spends the budget earlier;
  `ers_start_mode` is put on the car with the tyres (`fit_tyres`: AI and stock Balanced).
  A client that has pressed the ERS key keeps sending its own mode, which wins.
- `charge_full` refills with the tyres. The racing line counts the motor above the minimum
  speed at full power (`plan_power_w`). The AI drives Balanced and boosts within
  `AI_BOOST_GAP_S` of the car ahead.
- car.toml `[hybrid]`: `enabled`, `battery_capacity_kwh`, `battery_max_discharge_kw` /
  `_charge_kw`, `motor_max_torque_nm`, `motor_max_power_kw`, `regen_max_power_kw` and the
  keys above.
- Tests: `hybrid::tests`, `brake_by_wire_takes_the_recovered_energy_off_the_discs`.

## DRS (`drs.rs`, `GameSession::update_drs`)

Zones (`TrackConfig::drs_zones`: detection, activation, end) come from the track YAML
([track pipeline](../content/track-pipeline.md)). Crossing a detection line arms the zone:
in a race only within `DRS_GAP_S` (1 s) of any car ahead (`gap_ahead_s`); in practice,
qualifying and hotlap always. Between activation and end an armed car is
`CarState::drs_allowed`, and `update_car_3d` opens the flap while `PlayerInput.drs` is held
and the brake is under `DRS_BRAKE_CLOSE`. Nothing opens in the session's rain, before lap
`DRS_RACE_FROM_LAP` of a race, or in the garage. An F1 gets `DrsSpec::F1` (12% drag, 25%
rear downforce off) unless `[physics] drs_drag_reduction` / `drs_rear_downforce_reduction`
say otherwise (0 drag switches it off); other classes have none. The AI opens whenever
allowed; the racing line uses open-flap drag in the zones. Telemetry: `lap_flags` bits 3-4.
Client: the `Drs` action, `FApexCarTelemetry::bDrsAllowed` / `bDrsOpen`, a HUD badge, and
the F1 flap mesh (`[drs_flap]`, `FApexDrsFlapSpec`, [cars](../content/cars.md)).

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
- Tests: `car_setup::tests`, `setup_sheet::tests`.

## Wire fields

`CompactCarState` (positional, appended only; [protocol](protocol.md)): `fuel_dl`, `tyre_c`,
`tyre_kpa`, `tow_pct`, `tyre_wear`, `compound`, `brake_c`, `water_c`, `damage`, `ers_pct`,
`ers_lap_pct`, `ers_flags`, `tyre_c_edges`, `brake_wear`, `slide_flags`, `ers_stint_pct`,
`recover_ds` ([sessions](sessions.md#recovering-a-stuck-car)) (pit fields: [pit lane](pit-lane.md)); `PlayerInput.drs` / `ers_mode` / `ers_boost`;
`DriverFeedback.flat_spot`. Golden bytes (in `server/`, `-- --nocapture`): `cargo test
telemetry_compact_wire_format` -> `ApexUdpGolden::S_TelemetryCompactRecover` (the older
`S_TelemetryCompact{Fuel,Tyres,Tow,Heat,Damage,Ers,Zones}` stay on the client as older servers'
frames); `car_setup_wire_format` -> `ApexGolden::C_SetCarSetup`;
`car_setup_sheet_wire_format` -> `S_CarSetupSheet`; `player_input_headlights_wire_format` ->
`ApexUdpGolden::C_PlayerInput`; `driver_feedback_wire_format` -> `S_DriverFeedbackFlatSpot`.

## Checking it

- `cargo test` in `server/` runs the module tests and the per-area files named above, plus
  `car_stability_test`, `physics_property_tests`, `air_test` and `determinism_test`.
- `cargo test --release --test grip_probe_test silverstone_profile_lap_times -- --ignored
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
