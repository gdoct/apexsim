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
| A kerb's step is not a vertical acceleration of the body: the crest model filters the contact plane over 0.05 s and 0.08 s (`SLOPE_FILTER_S`, `VERTICAL_ACCEL_FILTER_S`) so mesh seams stay out, and a kerb reaches the car only through each wheel's spring and damper, with no wheel mass to hop. | Med | Low |
| Porpoising is an oscillation of the downforce at a fixed frequency (`aero::PORPOISE_HZ`) with no body heave behind it; the car does not bounce on its springs. | Med | Low |

## Track and environment

| Gap | Effort | Impact |
|---|---|---|
| The forecast is worked out from the session's id (`conditions::forecast`): a host cannot write one. | Med | Low |
| The road has no crown or bumps: the road mesh's cross-section is flat edge to edge (`ue_export` `road`) and the centerline backend's surface is the bank plane, so a car rides a road with no drainage camber, ripples or seams, and the road state's water runs only to a banked edge (`road_state::edge_low`). A crown has to land on both backends at once. | Med | Med |
| A circuit's time zone is a coarse map (`data::legal_clock`), not the real zone boundaries; the season's air is one cosine by latitude (`data::season_air_c`), with no continental or maritime climate, no wet or dry season in the tropics and no altitude lapse. | Low | Low |
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
| The AI's stop plan works out where a stop would rejoin (`pit::pit_loss_s` against the gaps behind it on the road) and answers a car behind that stops with the overcut when covering would rejoin in traffic, but it reads no rival's plan: not when the car ahead is due to stop or on what tyres, not the gain of a rival's out-lap on cold tyres, and the rejoin is judged when the stop is decided, up to a lap before the lane. A stop under a slow lap has nothing to use (no safety car). | Med | Low |
| An AI mistake is rolled a corner (`racecraft::mistakes`: pressure, cold tyres, a wet patch, a long stint) and none is a spin of the driver's own making: a lock-up or a wide line ends in one only through the physics. Fatigue is the stint's time in the car alone: no heat in the cockpit, no driver swaps to rest. | Low | Low |
| The AI's speed plan (`racing_line::build_laden`) counts a hybrid's motor at full power whatever the lap budget and the minimum deployment speed allow, and the AI caps itself at that plan times its skill's pace on the straights too: planned on the paced motor (as the player's aid is), an F1 AI lapped Monza 3 s slower than the car can. | Med | Low |
| The AI's hybrid use is rules on the gaps, the charge and the straight it is on (`hybrid::ai_ers`): it saves the overtake button for the long straights when chasing, but does not save the battery itself for a straight (harvesting on the ones before it), does not read the lap budget or the minimum deployment speed into where it attacks, and never changes its deploy map (`deploy_early`, the setup's). | Low | Low |
| The AI drives the racing line whatever the road does: it does not move off a rubbered line to a wet one for grip, nor aim for the dry line in the rain. | Med | Low |
| Imported AC cars' AI crashes a lot: one to ten cars retire per survey race. | Med | Med |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| Planned again only at a lap's end (`GameSession::refreshed_racing_line`), in still, clean air, and on one grip share for the whole lap (the car's weaker axle and the road under it at the line): the wake and the wind are not in it, and a wet or rubbered stretch elsewhere on the lap moves it only through the share read at the line. | Med | Low |

## Race rules and the pit lane

| Gap | Effort | Impact |
|---|---|---|
| No driver swaps or fixed stint lengths. | Med | Low |
| A race longer than a tank on a circuit with no pit lane (the Nordschleife): nothing refuels, the AI saves up to `pit::MAX_FUEL_SAVE_SHORT` and runs dry past it. | Low | Low |
| No penalties beyond the pit rule's (a car that takes the flag owing its mandatory stops is classified behind those that made them): no time penalty, no pit-lane speeding penalty (the limiter is forced, and brakes a car over the limit), nothing stops a driver on a manual pit lane leaving past a red exit light, no drive-through or stop-go, no collision penalty and no race-start ghosting. | High | Med |
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

## Assetto Corsa imports

| Gap | Effort | Impact |
|---|---|---|
| Imported tracks have no `altitude_m` and no north (AC carries neither), and most have no coordinates (only ~50 of AC's 175 layouts fill in `geotags`): they race at sea-level air density and the sun stands at the default latitude and compass. | Med | Med |
| AC's own tyre heat model is not carried, only its temperature window (`ac_car_import/physics.py` `thermal_window`). | Med | Low |
| AC's dynamic aero controllers (`aero.ini` `DYNAMIC_CONTROLLER`) are not modelled, and a DRS flap is not split off an imported body (the importer warns). | Med | Low |
| AC's ERS delivery profiles (`ctrl_ers_*.ini`) and front-axle motors (`[FRONT_MOTORS]`) are not carried: a 919-style hypercar imports with its rear motor only. | Med | Low |
| AC carries no figure for brake pads, `brake_by_wire`, body-roll aero or porpoising, so an imported car has the defaults for them; a compound's own window width, pressure and load sensitivity are not carried either (only its grip level, wear and window place). | Low | Low |

## Before picking one up

Every change must keep the sim deterministic (`tests/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
(`survey_ai_races_on_every_circuit`, see [AI](server/ai.md)) against the
commit before the work, as totals: single pile-ups move a circuit's numbers
more than most changes do.
