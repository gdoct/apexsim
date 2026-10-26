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
| A pass is one lane into one braking zone, held to the apex (chosen by the run in, `racecraft::pass_here`): no switchback on the exit and no planned cutback, and DRS counts only when it is open on the straight now, not for a zone further on. A switchback tried as an attack kept on into the next corner when out of one alongside on its inside (the lane kept beside the rival): on the 16 circuits of the traffic row, seeds 1-3, contact 170.4 -> 186.2 car-s, off-road 21.8 -> 26.3, passes 1442 -> 1438, so it is not in. | Med | Low |
| Traffic is measured across the road (`avoid_traffic`: sides and edges from the centerline ahead, a car alongside and ahead that leaves too little road is followed in, a car too close beside is dropped behind, a squeeze is dropped out of behind the car ahead only; ai.md, Traffic). Against the dodge measured across the car's own line, on every circuit at seed 1 contact 216.9 -> 71.8 car-s and off-road 188.5 -> 66.5, but passes 858 -> 552: the passing is not won back (the Snorisring hairpin's lap-one pile-ups in a GT3 field, 17.4 car-s of contact over seeds 1-3, went to 5.5 with the squeeze). Tried on 16 circuits (`SURVEY_TRACKS=Snorisring,Zandervoort,Monzarella,SpaFrankenchamps,Shiverstone,Bahrainless,Hungoverring,Barcelunatic,AlbertParkour,Interlaggos,RedPullRing,Shanghaied,YasMarinara,Gnocchi,ChillyVilleneuve,Oskarsleben`), seeds 1-3, from contact 170.4 car-s, off 21.8, passes 1442: no drop-behind for a car giving this one room (`Tactic::Room`) 203.3 / 7.3 / 1517; the close-beside margin `CLOSE_BESIDE_M` 0.75 m 232.5 / 28.6 / 1663 (0.5 m before the squeeze fix: passes +19%, off-road and sliding up a half); a car counted ahead only half a length up 109.0 contact against 93.5 on eight circuits; an attacker not dropping behind its rival when pinned 98.2 against 93.5; a shorter tuck (`TUCK_PACE` 0.97), a shorter abort cooldown (1.5 s), a looser pass plan (`PLAN_SHARE` 0.45) and a later brake alongside (3.5%) all within noise of 1442 passes. The passing lost is in the drop-behind itself, which also is what keeps the contact down. | Med | Med |
| A defence is one move to the inside (before the braking, or early on a straight against a car closing in the tow): no squeeze toward the edge, and nothing against a car that has already pulled out of the tow. | Low | Low |
| The AI's stop plan works out where a stop would rejoin (`pit::pit_loss_s` against the gaps behind it on the road) and answers a car behind that stops with the overcut when covering would rejoin in traffic, but it reads no rival's plan: not when the car ahead is due to stop or on what tyres, nor the gain of a rival's out-lap on cold tyres. A stop under either safety car is taken when it would be made anyway; the plan does not weigh the bunched queue it rejoins, nor wait for the lane to open (it simply turns in at the next pass). | Med | Low |
| An AI mistake is rolled a corner (`racecraft::mistakes`: pressure, cold tyres, a wet patch, a long stint) and none is a spin of the driver's own making: a lock-up or a wide line ends in one only through the physics. Fatigue is the time in the car (the stint's, or with drivers sharing the car the driver's spell), counted faster in hot air (`racecraft::heat_fatigue_rate`): the cockpit's own heat (the car's engine and its ventilation) and the driver's fitness play no part. | Low | Low |
| A player gets room only once alongside (`racecraft::start_tactic`, judged from where it is): nothing reads a player's intent before it is there (a dummy, a car pulling out of the tow), nor judges a player's blocking or a move made in the braking. | Med | Low |
| The AI caps itself at its skill's share of its speed plan on the straights too (`profile_target_speed`), so it lifts short of what the car does wherever the plan is no more optimistic than the car: a hybrid's cap is the plan with the motor unpaced over the lap budget (`RacingLineProfile::unpaced_mps`), and a hypercar's motor, now planned only above its 190 km/h, costs it 0.8 s a lap at Monza against the plan that counted it everywhere. Capping on the corner and braking limits alone (flat out on the straights) took a lone skill-110 AI 0.9 s (F1) and 2.7 s (hypercar) a lap quicker at Monza, but put the AI survey's off-road time up 18% (GT3s 38%), so the skill's pace still comes off the straights. | Med | Low |
| The AI's hybrid use is rules on the gaps, the charge and the straight it is on (`hybrid::ai_ers`): it saves the overtake button for the long straights when chasing, but does not save the battery itself for a straight (harvesting on the ones before it), does not read the lap budget or the minimum deployment speed into where it attacks, and never changes its deploy map (`deploy_early`, the setup's). Harvesting on the short straights while chasing (within `AI_ALONE_GAP_S` ahead, nobody that close behind, under half charge) made no difference the survey can see (eight circuits, seeds 1-3: contact 93.5 -> 93.2 car-s, passes 693 -> 684): its races are 180 s and the hybrid rules start on lap 2, so a battery plan wants a longer race to judge it by. | Low | Low |
| Snow tyres: the AI's lead onto studs ahead of the forecast's snow is a fixed number of laps per driver (`pit::SNOW_FIT_LEAD_LAPS`, the gambler's shorter), not weighed against what the bare laps cost in wear and studs; it never stops for a fresh set once its studs are gone (`TireData::studs_lost`); and `studs_lost` is not on the wire, so neither the HUD nor the wheel shows it. | Low | Low |
| The AI drives the racing line whatever the road does: it does not move off a rubbered line in the wet (`road_state::rubber_grip`: the line's rubber costs up to some 6% there, while the cars' wiping leaves the line the drier band, so the two nearly cancel), nor seek a drier band off it. Its only offsets from the line are a missed apex and the traffic dodge (`Racecraft::wide_m`, `lane_shift`), which sweep across the road with the line, and its speed plan is planned on the line's own radius: a wet line wants a racing line and plan of its own, built from the road state's bins, not an offset. | High | Low |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| The client's window over the stretch ahead (`racing_line::LineWindow`) leaves out the wake of a car ahead (the client plans it without the other cars). The lap line it ends on is the server's, planned again only at a lap's end. The path itself is the track's minimum-curvature raceline, the same for every car. | Med | Low |

## Race rules and the pit lane

| Gap | Effort | Impact |
|---|---|---|
| Driver swaps ([sessions](server/sessions.md#driver-swaps)) have no minimum time per driver, no rest a driver must have between spells, and no drivers of their own: a car's drivers are numbered, share its AI profile, and a human plays every one of theirs. | Low | Low |
| Penalties cover jump starts, passes under a yellow and red-light pit exits ([server/sessions.md](server/sessions.md#penalties)); there is no collision blame (who caused a contact, penalised by its harm), no track-limit penalty in a race (a struck lap only loses its time), no pit-lane speeding (the limiter is forced), no black flag for a penalty ignored (it becomes time), no start ghosting, and no unsafe-rejoin watch (a car back on the road into traffic; the incident list would carry it). | High | Med |
| The virtual safety car and the safety car on the road exist ([server/sessions.md](server/sessions.md#safety-car)), but no red flag (the race stopped, the field into the pit lane, a restart from the grid), no double-file restart, and no rule that the safety car stays out a minimum number of laps. | High | Med |
| A retired car jumps to its box after `TOW_AFTER_S`: the marshals and the recovery tractor that come for it are the client's drawing (`ApexMarshals`, [cameras](game/cameras.md)), not on the server, so the car is not lifted and carried, it vanishes. Without a pit lane it stays where it stopped, out of everyone's way. | Low | Med |

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
[sessions](server/sessions.md#events-across-restarts)); a schedule starts
each phase at its time, whoever is there, a late driver going out from the
pit lane; the weekend's sheets are kept as one record, exported, and
browsable in the client with each phase's replay.

| Gap | Effort | Impact |
|---|---|---|
| An event ([sessions](server/sessions.md#events-across-restarts)) keeps finished phases only: a phase under way when the server stops is run again from its start (a knockout from Q1), and the session's own state (the cars, a round's laps) is not persisted. | Med | Low |

## Assetto Corsa imports

| Gap | Effort | Impact |
|---|---|---|
| An imported mod outside the importer's `KNOWN_SITES` table and without real `geotags` has no height or place (sea-level air, 50 N), and its north is AC's -z convention, which the fits show wrong for some mods (Interlagos 16°, Suzuka 40°, Bahrain 88° off). | Low | Low |
| AC's tyre heat model is carried only as each compound's heating against the reference's (`FRICTION_K`, `ROLLING_K` ratios): its absolute heat and cooling rates (`*_TRANSFER`, `COOL_FACTOR`) are against AC's own tyre masses and not mapped, and `GRAIN_*` / `BLISTER_*` are not read, so the reference compound heats as the class's shipped tyre. | Med | Low |
| AC's dynamic aero controllers on suspension travel (the 908 LH) are read flat out in a straight line; controllers on the driver are one map per input multiplied together (exact for one input alone, not for an ALA's gas, brake and lateral g together), with no `FILTER` lag and no side-to-side aero. | Med | Low |
| An AC car with two ERS motors (the 918, the TS040) recovers through the front one only (the crank motor's `COAST_CURVE` is not carried), and one delivery profile (the front motor's) drives both. Of AC's delivery profiles (`ctrl_ers_*.ini`) the default one's throttle, speed, gear and rpm maps are carried as one map per input multiplied together; its slip and oversteer cuts (`SLIPRATIO_MAX`, `OVERSTEER_FACTOR`), its `FILTER` lag and the choice between profiles are not (the sim's modes stand in for the profiles). | Low | Med |
| AC carries no figure for brake pads, `brake_by_wire`, body-roll aero or porpoising, so an imported car has the defaults for them. A compound's own load sensitivity is carried to the tick, but the racing line and the AI's grip envelope plan with the reference compound's. | Low | Low |

## Before picking one up

Every change must keep the sim deterministic (`server/sim/tests/all/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
(`survey_ai_races_on_every_circuit`, see [AI](server/ai.md)) against the
commit before the work, as totals: single pile-ups move a circuit's numbers
more than most changes do.
