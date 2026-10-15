# Simulation gaps

What the simulation and the racing rules still leave out, grouped by area.
Each gap has an **effort** (how hard to build and land safely) and an
**impact** (how much it changes driving and racing), both Low / Med / High.
Built features are documented under docs/ (see the [index](README.md)); this
file only lists what is missing. Product, UI and tooling features not started
live in the [roadmap](roadmap.md); bugs, edge cases and cleanup left on built
product features live in [follow-ups.md](follow-ups.md); built features not
yet seen, heard or felt live in [verification.md](verification.md).

Keep it current: when a gap is closed, delete its row; when a feature leaves
something for later, add a row here (simulation, racing rules, driver
feedback), to the follow-ups (anything else left on a built feature) or to
the roadmap (a feature not started) or verification.md (built, not yet
seen). Every deferred item is recorded in exactly one of the four.

## Car physics

| Gap | Effort | Impact |
|---|---|---|
| Roll centres are fixed heights: no migration with travel, no jacking, no instant centres; the roll axis is all the kinematics there are (`geometry.rs`). | High | Low |
| The crest model reads the road's grade along the car's path only: a crest taken diagonally, a banking's roll-over and a kerb's step are not vertical accelerations to it. | Med | Low |
| Porpoising is an oscillation of the downforce at a fixed frequency (`aero::PORPOISE_HZ`) with no body heave behind it; the car does not bounce on its springs. | Med | Low |
| Brake pads and discs are changed only at a pit stop that finds them past `brakes::PAD_CHANGE_PCT` / `DISC_CHANGE_PCT`: no pad choice at the stop (it fits the set the setup names). | Low | Low |
| The hybrid's stint budget (`[hybrid] stint_kj`) is electric energy only: the WEC rule limits the whole car's energy per stint, fuel included. | Med | Low |

## Tyres

| Gap | Effort | Impact |
|---|---|---|
| The three tread zones take the heat's landing from camber, lateral force and pressure (`tyre_thermal::zone_shares`); toe scrub and the carcass's deflection under braking are not in it, and the grip reads the mean rather than penalising a spread. | Med | Low |

## Track and environment

| Gap | Effort | Impact |
|---|---|---|
| The forecast is worked out from the session's id (`conditions::forecast`): a host cannot write one. | Med | Low |
| The road has no crown or bumps: the road mesh's cross-section is flat edge to edge (`ue_export` `road`) and the centerline backend's surface is the bank plane, so a car rides a road with no drainage camber, ripples or seams, and the road state's water runs only to a banked edge (`road_state::edge_low`). A crown has to land on both backends at once. | Med | Med |
| The wind's direction holds for the session (only its mean follows the weather, `LiveConditions::wind_target_kph`): a front does not veer it. | Low | Low |
| A date moves only the sun (`SessionConditions::day_of_year`): the weather's own air, humidity and the snow line are the late-May day's in any month, and a circuit's time zone is a coarse map (`data::legal_clock`), not the real zone boundaries. | Low | Low |
| Overnight cooling has no dew or fog; an evening session at a humid circuit is as clear as noon. | Med | Low |
| Snow: the run-off and the pit lane carry the verge's cover (one figure a cell, no ice): the cars in the pit lane never pack it, and the lane's own station is not the lap's, so it reads the cover of the lap cell beside it. | Low | Low |
| Snow: the AI knows the snow's grip exactly (its speed profile is baked on the covered road, `surface_grip_share` reads the line) and still cannot hold 0.28 on cold tyres, so a snow race is a pile-up; it has no feel for a road it misjudges and no snow technique (slower, earlier, straighter inputs) that would let a careful driver get round. There is no winter compound (a car.toml `snow_grip`, studs) and no gritting or salting as a session rule; sleet (snow at 1 to 3 °C that lands as water) and hail are not modelled. | Med | Med |

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
| No penalties of any kind: no pit-lane speeding penalty (the limiter is forced, and brakes a car over the limit), nothing stops a driver on a manual pit lane leaving past a red exit light, no drive-through or stop-go, no collision penalty and no race-start ghosting. | High | Med |
| No flags and no flag state on the wire: no yellow at an incident, no blue for a car about to be lapped, no safety car (so no lane closure). The trackside panels' `ApexEmissive_*` slots glow at a fixed level. | High | Med |
| Boxes are dealt in seating order (`pit::deal_box`), not by team. | Low | Low |
| A retired car jumps to its box after `TOW_AFTER_S`: no recovery vehicle, no marshals. Without a pit lane it stays where it stopped, out of everyone's way. | Med | Low |

## Race weekends: sessions across days

The goal: an **event** that runs practice on one day, qualifying with the
same drivers on the next, and the race the day after, gridded from the
qualifying result. Today a session lives only while someone is in it (it is
removed when its last driver or spectator leaves, and a finished one
`session_timeout_seconds` after it finished), and nothing about it survives
a restart but the lap records and the stored qualifying results
(`records/qualifying.json`). Within one session, Qualification -> Race
grids from the qualifying classification, and a race weekend runs timed
practice, one to three knockout qualifying rounds and the race as phases of
one session, advanced by the host (see
[sessions](server/sessions.md#race-weekend)).

| Gap | Effort | Impact |
|---|---|---|
| No event across days: a race weekend is one session with phases, live only while someone is in it. Nothing groups sessions on one track and car class with a name, an entry list and a schedule (start time per session, in real days). | Med | High |
| Sessions are not persisted: an event, its entry list, each session's state and its results must survive server restarts and empty hours (stored beside `records/`, deterministic ids). | Med | High |
| Entries are not stable drivers: a player is a per-connection id and records key on the name, so the same driver must be recognised across days (an account or token identity, with car and livery fixed for the event). | Med | High |
| Qualifying is a timed session or a weekend's knockout rounds (Q1/Q2/Q3). No one-lap shoot-out, no single-car runs with a fixed order, and no tyre or lap allowance. | Low | Med |
| In qualifying a car is put at the pit exit (`qualifying_outlap_pose`, a teleport): it does not drive down the pit lane, so there is no limiter or queue at the exit light, and the HUD shows race-style standings by track position. | Med | Low |
| The start order matches drivers by name (`grid_order::resolve`): a stored result's humans are found again only under the same name. | Low | Low |
| No schedule: a session opening, counting in and starting at its time whether or not everyone is there, with a late joiner going out from the pit lane. Needs a lobby view of upcoming sessions and a countdown. | Med | Med |
| No per-phase rules beyond lengths and skies: a weekend sets practice and round lengths and each day's sky, but assists and damage are the session's for every phase. | Low | Low |
| AI entries in an event need fixed identities and levels across the days, a qualifying run of their own, and their results kept like a human's. | Med | Med |
| Setups and tyre sets do not carry between sessions (no parc fermé, no tyre allocation per weekend). | Med | Low |
| No weekend results record: practice times, the combined qualifying sheet (stored) and the race classification are not kept together as one record, shown in the client or exported. | Med | Med |
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
