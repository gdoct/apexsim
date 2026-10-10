# Simulation gaps

What the simulation and the racing rules still leave out, grouped by area.
Each gap has an **effort** (how hard to build and land safely) and an
**impact** (how much it changes driving and racing), both Low / Med / High.
Built features are documented under docs/ (see the [index](README.md)); this
file only lists what is missing. Product, UI, tooling and bug items live in
the [roadmap](roadmap.md).

Keep it current: when a gap is closed, delete its row; when a feature leaves
something for later, add a row here (simulation, racing rules, driver
feedback) or to the roadmap (everything else). Every deferred item is
recorded in one of the two.

## Car physics

| Gap | Effort | Impact |
|---|---|---|
| Roll centres are fixed heights: no migration with travel, no jacking, no instant centres; the roll axis is all the kinematics there are (`geometry.rs`). | High | Low |
| The crest model reads the road's grade along the car's path only: a crest taken diagonally, a banking's roll-over and a kerb's step are not vertical accelerations to it. | Med | Low |
| Porpoising is an oscillation of the downforce at a fixed frequency (`aero::PORPOISE_HZ`) with no body heave behind it; the car does not bounce on its springs. | Med | Low |
| Brake pads are changed only at a pit stop that finds them past `brakes::PAD_CHANGE_PCT`: no pad choice at the stop, no disc wear separate from the pads. | Low | Low |
| The hybrid's stint budget (`[hybrid] stint_kj`) is electric energy only: the WEC rule limits the whole car's energy per stint, fuel included. | Med | Low |
| Brake-by-wire takes the recovery off the driven axle's hydraulics evenly: no front-rear migration of the balance as the battery fills. | Low | Low |

## Tyres

| Gap | Effort | Impact |
|---|---|---|
| The three tread zones take the heat's landing from camber, lateral force and pressure (`tyre_thermal::zone_shares`); toe scrub and the carcass's deflection under braking are not in it, and the grip reads the mean rather than penalising a spread. | Med | Low |
| The road state's passes are laid at the car's middle plus and minus half its track (`GameSession::update_road`), not at each tyre's contact patch, and count the same whatever the tyre is doing. | Low | Low |
| Puddles are read off the centerline's elevation along the lap (`road_state::LOW_DEPTH_M`): a road crowned to drain, a banking's low edge and a cambered corner's inside are not puddles to it. | Med | Low |
| A slow puncture is a fixed leak rate (`TireData::leak_kpa_per_s`); the tyre's temperature and the leak's cause do not change it. | Low | Low |

## Track and environment

| Gap | Effort | Impact |
|---|---|---|
| Rubber is laid per wheel pass, not by the tyre's slip energy, and marbles are shed by the car's lateral g (`road_state::shed_for_lateral_g`), not by tyre wear; a tyre driven through marbles does not pick them up (no grip lost for a lap after a run off line). | Med | Low |
| The forecast is worked out from the session's id (`conditions::forecast`): a host cannot write one. | Med | Low |
| The wind's mean and the humidity hold for the session whatever the weather does (`GameSession::update_wind` only gusts about the start figure): a storm front brings no gale. | Low | Low |
| The day is late May everywhere (no date, no season) and solar noon is 13:00 (`data::SOLAR_NOON_HOURS`) whatever the circuit's longitude and time zone. | Low | Low |
| Overnight cooling has no dew or fog; an evening session at a humid circuit is as clear as noon. | Med | Low |

## AI and strategy

| Gap | Effort | Impact |
|---|---|---|
| Passes are planned into a braking zone on one lane held to that corner's apex, or on a straight past a car at least 4 m/s slower (`racecraft::decide`): no switchback on the exit, no planned cutback, no use of the DRS zone or the tow to choose where. | Med | Med |
| Side by side past the apex the cars are back on the per-tick dodge (an offset from the line, which sweeps across the road with it); on a narrow road (Zandvoort) two GT3s alongside lean on each other. Holding the dodge across the road was tried and made contact worse. | High | Med |
| A defence is one move to the inside before the braking: no covering the tow on a straight, no squeeze toward the edge, no judging how fast the car behind is coming. | Med | Low |
| Room is left only for an AI attacker (`Tactic::Room` needs the rival's `Tactic::Attack`): a human alongside gets the per-tick dodge, and nothing reads a human's intent or judges a human's blocking. | Med | Med |
| Backing out of a wall (`AiDriverController::back_out`) is a timed reverse toward the road ahead: no look behind for traffic, no wait for a gap before rejoining, and a car whose progress jumped to another leg (Zandvoort 887 m / 4282 m) steers for that leg. | Med | Med |
| Mistakes under pressure are an overcooked braking zone only (`racecraft::MISTAKE_*`): no lock-up, spin, missed apex or missed shift. | Low | Low |
| Pit strategy is thresholds (`pit::plan_stop`): no undercuts, no reaction to the cars around it, no mandatory stops, no fuel saving to skip a stop. | Med | Med |
| No timed-race endgame: no splash-and-dash, no last-stint planning (`laps_left` does estimate from the clock for the pit plan). | Med | Low |
| Fuel saving is lift-and-coast only, and only for cars that cannot refuel; no short-shifting to save fuel. | Low | Low |
| The AI never harvests or saves hybrid energy on purpose (it drives Balanced), and does not defend with the overtake button (it presses it within `hybrid::AI_BOOST_GAP_S` of a car ahead). | Low | Low |
| The AI's tyre crossover is a threshold on the racing line's water (`pit::tyres_for_the_track`): it never gambles on slicks, never reads the forecast and never stays out because the race is nearly over. | Med | Med |
| The AI drives the racing line whatever the road does: it does not move off a rubbered line to a wet one for grip, nor aim for the dry line in the rain. | Med | Low |
| The AI's speed plan assumes full engine power after damage; it drives around damage only through its steering loop and aero share. | Low | Low |
| `apexsim-replay render` (the showcases), the demo race and the AI survey race the mixed field: none takes an AI level. | Low | Low |
| Imported AC cars' AI crashes a lot: one to ten cars retire per survey race. | Med | Med |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| Built once on joining (`racing_line::build`, from `game_loop/dispatch.rs`) for the stock car, dry tank, clean and still air, warm new tyres, and the session's grip and air as it started: the setup, fuel, the wake, the wind, tyre temperature, compound and wear, the rubber and changing weather never change it (the AI's own plan does follow fuel). | Med | Low |
| Counts the hybrid motor at full power whatever the lap budget allows (optimistic for the F1s). | Low | Low |

## Race rules and the pit lane

| Gap | Effort | Impact |
|---|---|---|
| No pit stop choices during a race and no pit request button: a stop always changes tyres (to the setup's next compound), fuels to need and repairs everything. | Med | Med |
| No driver swaps or fixed stint lengths. | Med | Low |
| An F1 in a race longer than its tank: the start fill is capped, no stop adds fuel, and the AI's lift-and-coast saves at most `MAX_FUEL_SAVE_COAST_S` a lap, so a car short by more runs dry. | Low | Low |
| No penalties of any kind: no pit-lane speeding penalty (the limiter is forced), no drive-through or stop-go, no collision penalty and no race-start ghosting. | High | Med |
| No flags and no flag state on the wire: no yellow at an incident, no blue for a car about to be lapped, no safety car (so no lane closure). The trackside panels' `ApexEmissive_*` slots glow at a fixed level. | High | Med |
| Boxes are dealt in seating order (`pit::deal_box`), not by team. | Low | Low |
| A retired car jumps to its box after `TOW_AFTER_S`: no recovery vehicle, no marshals. Without a pit lane it stays where it stopped, out of everyone's way. | Med | Low |
| The pit autopilot takes over wherever the driver crosses the road edge (`pit::takes_over`); a car that dives across the mouth runs 3-4 m off the lane's middle before it settles. | Low | Low |
| The session's damage rule (`DamageLevel`) has no "visual only" level. | Low | Low |

## Race weekends: sessions across days

The goal: an **event** that runs practice on one day, qualifying with the
same drivers on the next, and the race the day after, gridded from the
qualifying result. Today a session lives only while someone is in it (it is
removed when its last driver or spectator leaves, and a finished one
`session_timeout_seconds` after it finished), and nothing about it survives
a restart but the lap records and the stored qualifying results
(`records/qualifying.json`). Within one session, Qualification -> Race
grids from the qualifying classification (see
[sessions](server/sessions.md)).

| Gap | Effort | Impact |
|---|---|---|
| No event: nothing groups practice, qualifying and the race on one track and car class, with a name, an entry list and a schedule (start time per session, in real days). | Med | High |
| Sessions are not persisted: an event, its entry list, each session's state and its results must survive server restarts and empty hours (stored beside `records/`, deterministic ids). | Med | High |
| Entries are not stable drivers: a player is a per-connection id and records key on the name, so the same driver must be recognised across days (an account or token identity, with car and livery fixed for the event). | Med | High |
| Qualifying has no length or format: it runs until the host moves on, so the classification is whatever stands then. No session timer and no results screen after it (the garage scoreboard is live only; the result shows afterwards as a chip in the next race's Starting order row). | Low | Med |
| In qualifying a car is put at the pit exit (`qualifying_outlap_pose`, a teleport): it does not drive down the pit lane, so there is no limiter or queue at the exit light, and the HUD shows race-style standings by track position. | Med | Low |
| The start order matches drivers by name (`grid_order::resolve`): a stored result's humans are found again only under the same name, and a no-show closes the grid up rather than leaving the slot empty. | Low | Low |
| No schedule: a session opening, counting in and starting at its time whether or not everyone is there, with a late joiner going out from the pit lane. Needs a lobby view of upcoming sessions and a countdown. | Med | Med |
| No per-session rules within an event: practice length, qualifying format, race length, conditions per day (a different forecast each day), assists and damage. | Low | Med |
| AI entries in an event need fixed identities and levels across the days, a qualifying run of their own, and their results kept like a human's. | Med | Med |
| Setups and tyre sets do not carry between sessions (no parc fermé, no tyre allocation per weekend). | Med | Low |
| No event results: the practice times, the qualifying sheet and the race classification as one weekend record, shown in the client and exported. | Med | Med |
| Replays and the timing board are per session; an event's replays and sheets should be browsable together. | Low | Low |

## Driver feedback

| Gap | Effort | Impact |
|---|---|---|
| Tyre heat is not a force-feedback or squeal cue; damage changes neither the engine note nor the feel. | Med | Med |

## Assetto Corsa imports

| Gap | Effort | Impact |
|---|---|---|
| Imported tracks have no pit sidecar (so no pit stops) and no `altitude_m` / latitude / longitude metadata or north in their export: they race at sea-level air density and the sun is the sky model's default. | Med | Med |
| AC's own tyre heat model is not carried, only its temperature window (`ac_car_import/physics.py` `thermal_window`). | Med | Low |
| AC's dynamic aero controllers (`aero.ini` `DYNAMIC_CONTROLLER`) are not modelled, and a DRS flap is not split off an imported body (the importer warns). | Med | Low |
| AC's ERS delivery profiles (`ctrl_ers_*.ini`) and front-axle motors (`[FRONT_MOTORS]`) are not carried: a 919-style hypercar imports with its rear motor only. | Med | Low |
| Brakes take carbon or steel from the class (`BrakeMaterial::for_class`), not from `brakes.ini`. | Low | Low |
| The car importer maps none of the newer car.toml keys: one compound only (no `[[tires.compound]]` list), no roll centres, camber thrust, yaw/roll aero or porpoising, brake pads, `tank_front_share` or `brake_by_wire`. | Med | Med |

## Built but never seen in the running game

Covered by automation tests only; each needs a look on screen.

- Contact sparks: a contact lasts a few frames and no timed screenshot has caught one (the steam and the engine smoke have been seen).
- The garage's compound cards built from a car's own `[[tires.compound]]` list: no shipped car files one, so only the default five have been on screen (`ApexSim.UI.Garage.CompoundCards` pins the rest).
- Needs a human at the controls: a flat spot through a real wheel or pad, the flat-spot thump in the road sound, a puddle or a dried line felt from the seat, a slow puncture felt over a lap.
- Track and sky: the flags turning with the wind (no shipped circuit places `sign/flag_pole`), the sun at a circuit's real latitude and north, rain arriving and the road drying under a changeable sky, a rubbered start.
- Racecraft (`crate::racecraft`): passes, defensive moves, a car leaving room, a backmarker yielding and a car reversing out of a barrier have only been read off the AI survey's numbers and traces. Worth a look: whether leaving a lane after a pass and the timed reverse look natural, and how the side-by-side contact the survey counts looks on screen.
- The 420 Hz server through a real client: car motion, force feedback and the replay recorder's rate snap were tuned on 240 Hz ticks with 60 Hz telemetry (the divisor of 7 keeps 60 Hz). Not measured: tick jitter at 2.4 ms against the 1 ms Windows timer (only the mean rate is tested).

## Before picking one up

Every change must keep the sim deterministic (`tests/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
(`survey_ai_races_on_every_circuit`, see [AI](server/ai.md)) against the
commit before the work, as totals: single pile-ups move a circuit's numbers
more than most changes do.
