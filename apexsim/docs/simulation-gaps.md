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
| A circuit's time zone is a coarse map (`data::legal_clock`), not the real zone boundaries, and so are its climate's regions (`climate::climate_of`); the tropics' wet season moves the humidity only, not how often a seeded forecast rains. | Low | Low |
| Rain falls on the whole circuit at once: the weather is one figure eased in time (`conditions.rs`), with no cells moving across the circuit on the wind, so one end cannot be wet while the other is dry, and the minimap's rain radar (`ApexRainRadar`) can only draw cells from the forecast. Rain by place goes into the road state's cells and the baked grip, on the server only. | High | Med |
| Dew never freezes into frost (its film, `road_state::DEW_FILM`, is under the least water that freezes), and a fog is a figure only: the AI drives a fog as it drives clear air. | Low | Low |
| Snow: the run-off carries the verge's cover and the pit lane one figure a cell of its own (`RoadState::lane_snow`, `lane_ice`): neither has a driven track across its width, their ice glazes from the cover alone (not from standing water), and the lane's water is still the lap cell's beside it. | Low | Low |
| Snow: the AI knows the snow's grip exactly (its speed profile is baked on the covered road, `surface_grip_share` reads the line) and still cannot hold 0.28 on cold tyres, so a snow race is a pile-up; it has no feel for a road it misjudges; its snow technique (`ai_driver::SNOW_PACE`, a slower wheel and throttle) roughly halves the time off the road, and the field still piles up. There is no gritting or salting as a session rule; sleet (snow at 1 to 3 °C that lands as water) and hail are not modelled: as `Weather` states they would be new bytes after `Snow`, which the current client reads as a sunny sky (`ParseSkyNow`, the conditions' decode) and which its ports of the forecast and the create screen's sky (`ApexCreateSession::Forecast`, `ApexSky`) do not know, so they need the client first. | Med | Med |

## AI and strategy

| Gap | Effort | Impact |
|---|---|---|
| A pass is one lane into one braking zone, held to the apex (chosen by the run in, `racecraft::pass_here`): no switchback on the exit and no planned cutback, and DRS counts only when it is open on the straight now, not for a zone further on. | Med | Low |
| Side by side past the apex the cars are back on the per-tick dodge (an offset from the line, which sweeps across the road with it); on a narrow road (Zandvoort) two GT3s alongside lean on each other. Holding the dodge across the road was tried and made contact worse. A two-class field (`SURVEY_CLASSES=LMP2:6,GT3:6`, 2026-10-23) passes eight times as often and shows it most: 18 car-s of contact at Monza, 28 at Spa against 6 and 1 single-class, most of it logged on the car yielding. Tried that day: room for any car alongside (Monza/Spa single-class halved, two-class Spa 28 -> 15 and Zandvoort 24 -> 13, but Snorisring 1 -> 8 and Spa off-road 1 -> 12 s) and room for the rival an attacker is still beside past the apex (two-class worse everywhere); neither kept. | High | Med |
| A defence is one move to the inside (before the braking, or early on a straight against a car closing in the tow): no squeeze toward the edge, and nothing against a car that has already pulled out of the tow. | Low | Low |
| The AI's stop plan works out where a stop would rejoin (`pit::pit_loss_s` against the gaps behind it on the road) and answers a car behind that stops with the overcut when covering would rejoin in traffic, but it reads no rival's plan: not when the car ahead is due to stop or on what tyres, not the gain of a rival's out-lap on cold tyres, and the rejoin is judged when the stop is decided, up to a lap before the lane. A stop under either safety car is taken when it would be made anyway; the plan does not weigh the bunched queue it rejoins, nor wait for the lane to open (it simply turns in at the next pass). | Med | Low |
| An AI mistake is rolled a corner (`racecraft::mistakes`: pressure, cold tyres, a wet patch, a long stint) and none is a spin of the driver's own making: a lock-up or a wide line ends in one only through the physics. Fatigue is the stint's time in the car alone: no heat in the cockpit, no driver swaps to rest. | Low | Low |
| A player gets room only once alongside (`racecraft::start_tactic`, judged from where it is): nothing reads a player's intent before it is there (a dummy, a car pulling out of the tow), nor judges a player's blocking or a move made in the braking. | Med | Low |
| The AI caps itself at its skill's share of its speed plan on the straights too (`profile_target_speed`), so it lifts short of what the car does wherever the plan is no more optimistic than the car: a hybrid's cap is the plan with the motor unpaced over the lap budget (`RacingLineProfile::unpaced_mps`), and a hypercar's motor, now planned only above its 190 km/h, costs it 0.8 s a lap at Monza against the plan that counted it everywhere. Capping on the corner and braking limits alone (flat out on the straights) took a lone skill-110 AI 0.9 s (F1) and 2.7 s (hypercar) a lap quicker at Monza, but put the AI survey's off-road time up 18% (GT3s 38%), so the skill's pace still comes off the straights. | Med | Low |
| The AI's hybrid use is rules on the gaps, the charge and the straight it is on (`hybrid::ai_ers`): it saves the overtake button for the long straights when chasing, but does not save the battery itself for a straight (harvesting on the ones before it), does not read the lap budget or the minimum deployment speed into where it attacks, and never changes its deploy map (`deploy_early`, the setup's). | Low | Low |
| Snow tyres: the AI's change onto and off a snow tyre reads the line's cover alone (`pit::snow_tyres`): no forecast of snow starting or stopping, no gamble, and `field_treaded` counts studs as treaded tyres for the rain's follow-the-field rule. Studs do not wear down or pull out beyond `bare_wear`, and no driving-on-snow technique comes with them. | Low | Low |
| The AI drives the racing line whatever the road does: it does not move off a rubbered line in the wet (`road_state::rubber_grip`: the line's rubber costs up to some 6% there, while the cars' wiping leaves the line the drier band, so the two nearly cancel), nor seek a drier band off it. Its only offsets from the line are a missed apex and the traffic dodge (`Racecraft::wide_m`, `lane_shift`), which sweep across the road with the line, and its speed plan is planned on the line's own radius: a wet line wants a racing line and plan of its own, built from the road state's bins, not an offset. | High | Low |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| Planned again only at a lap's end (`GameSession::refreshed_racing_line`), in still, clean air, and on one grip share for the whole lap (the car's weaker axle and the road under it at the line): the wake and the wind are not in it, and a wet or rubbered stretch elsewhere on the lap moves it only through the share read at the line. | Med | Low |

## Race rules and the pit lane

| Gap | Effort | Impact |
|---|---|---|
| No driver swaps: a stint (`RaceRules::max_stint_laps` / `max_stint_minutes`) is the car's, one driver to the flag. | Med | Low |
| Penalties cover jump starts, passes under a yellow and red-light pit exits ([server/sessions.md](server/sessions.md#penalties)); there is no collision blame (who caused a contact, penalised by its harm), no track-limit penalty in a race (a struck lap only loses its time), no pit-lane speeding (the limiter is forced), no black flag for a penalty ignored (it becomes time), no start ghosting, and no unsafe-rejoin watch (a car back on the road into traffic; the incident list would carry it). | High | Med |
| The virtual safety car and the safety car on the road exist ([server/sessions.md](server/sessions.md#safety-car)), but no red flag (the race stopped, the field into the pit lane, a restart from the grid), no double-file restart, and no rule that the safety car stays out a minimum number of laps. | High | Med |
| A retired car jumps to its box after `TOW_AFTER_S`: no recovery vehicle, no marshals. Without a pit lane it stays where it stopped, out of everyone's way. | Med | Low |

## Race weekends: sessions across days

The goal: an **event** that runs practice on one day, qualifying with the
same drivers on the next, and the race the day after, gridded from the
qualifying result. Within one session, Qualification -> Race grids from
the qualifying classification, and a race weekend runs timed practice, one
to three knockout qualifying rounds and the race as phases of one session,
advanced by the host (see [sessions](server/sessions.md#race-weekend)). An
event keeps such a weekend across server restarts: its entry list of
proved driver ids with their cars and liveries, its phase and each finished
phase's results, in `records/events/`; the dashboard opens it, starts and
advances it, and a restart opens it again at its next phase, the race
gridded from the stored qualifying (see
[sessions](server/sessions.md#events-across-restarts)). Nothing starts it
at a set time yet.

| Gap | Effort | Impact |
|---|---|---|
| An event ([sessions](server/sessions.md#events-across-restarts)) keeps finished phases only: a phase under way when the server stops is run again from its start (a knockout from Q1), and the session's own state (the cars, a round's laps) is not persisted. | Med | Low |
| In qualifying the HUD shows race-style standings by track position, not a qualifying table. | Med | Low |
| No schedule: a session opening, counting in and starting at its time whether or not everyone is there, with a late joiner going out from the pit lane. Needs a lobby view of upcoming sessions and a countdown. | Med | Med |
| Assists are the session's for every phase of a weekend (practice and qualifying name their own damage rule, lengths and sky): the client learns `AllowedAssists` only from `SessionJoined`, so a phase's own assists need them sent again (in `WeekendState`, as its `Damage` is) and the client's Assists locks refreshed on it. | Low | Low |
| No weekend results record: practice times, the combined qualifying sheet (stored) and the race classification are not kept together as one record, shown in the client or exported. | Med | Med |
| Replays and the timing board are per session; an event's replays and sheets should be browsable together. | Low | Low |

## Assetto Corsa imports

| Gap | Effort | Impact |
|---|---|---|
| An imported mod outside the importer's `KNOWN_SITES` table and without real `geotags` has no height or place (sea-level air, 50 N), and its north is AC's -z convention, which the fits show wrong for some mods (Interlagos 16°, Suzuka 40°, Bahrain 88° off). | Low | Low |
| AC's own tyre heat model is not carried, only its temperature window (`ac_car_import/physics.py` `thermal_window`). | Med | Low |
| AC's dynamic aero controllers (`aero.ini` `DYNAMIC_CONTROLLER`) are not modelled, and a DRS flap is not split off an imported body (the importer warns). | Med | Low |
| An AC car with two ERS motors (`[KINETIC]` and `[FRONT_MOTORS]` both driving, the 918, the TS040) imports with the stronger only: the sim has one motor per car. AC's delivery profiles (`ctrl_ers_*.ini`) give only the default profile's minimum speed; their throttle maps, speed caps, oversteer / slip cuts, gear masks and the choice between profiles are not carried. | Low | Med |
| AC carries no figure for brake pads, `brake_by_wire`, body-roll aero or porpoising, so an imported car has the defaults for them; a compound's own pressure and load sensitivity are not carried either (only its grip level, wear and window place and width). | Low | Low |

## Before picking one up

Every change must keep the sim deterministic (`server/sim/tests/all/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
(`survey_ai_races_on_every_circuit`, see [AI](server/ai.md)) against the
commit before the work, as totals: single pile-ups move a circuit's numbers
more than most changes do.
