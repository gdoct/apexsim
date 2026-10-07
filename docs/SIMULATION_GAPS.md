# Simulation gaps

What the simulation still leaves out, grouped by area. Each gap has an
**effort** (how hard to build and land safely) and an **impact** (how much
it changes driving and racing), both Low / Med / High. Built features
are documented in CLAUDE.md; this file only lists what is missing.

Keep it current: when a gap is closed, delete its row; when a feature
leaves something for later, add a row.

## Car physics

The 2026-10-07 pass closed the list (CLAUDE.md, "Car physics and tyre
gaps"); what it left for later:

| Gap | Effort | Impact |
|---|---|---|
| Roll centres are fixed heights: no migration with travel, no jacking, no instant centres; the roll axis is all the kinematics there are. | High | Low |
| The crest model reads the road's curvature along the path only: a crest taken diagonally, a banking's roll-over and a kerb's step are not vertical accelerations to it. | Med | Low |
| Porpoising is an oscillation of the downforce at a fixed frequency with no body heave behind it; the car does not actually bounce on its springs. | Med | Low |
| Brake pads are changed only at a pit stop that finds them past 60%: no pad choice in the pit menu, no disc wear separate from the pads. | Low | Low |
| The hybrid's stint budget is electric energy only: the WEC's rule limits the whole car's energy per stint (fuel included), and no HUD element shows the stint budget yet. | Med | Low |
| Brake-by-wire takes the recovery off the driven axle's hydraulics evenly: no front-rear migration of the balance as the battery fills. | Low | Low |

## Tyres

| Gap | Effort | Impact |
|---|---|---|
| The three tread zones read the heat's landing from camber, lateral force and pressure; toe scrub and the carcass's lateral deflection under braking are not in it, and the grip reads the mean rather than penalising a spread. | Med | Low |
| The road state's passes are laid at the car's middle plus and minus half its track, not at each tyre's own contact patch, and count the same whatever the tyre is doing. | Low | Low |
| Puddles are read off the centerline's elevation along the lap: a road crowned to drain, a banking's low edge and a cambered corner's inside are not puddles to it. | Med | Low |
| Debris is a point the physics does not draw: the client shows no piece on the road (`GameSession::debris` is not on the wire). | Low | Low |
| A slow puncture is a leak rate; a tyre's temperature and the leak's cause do not change it, and the client shows only the pressure falling. | Low | Low |

## Track and environment

| Gap | Effort | Impact |
|---|---|---|
The 2026-10-07 pass built track evolution and changing conditions
(CLAUDE.md, "Track evolution and changing conditions"); what it left for
later:

| Gap | Effort | Impact |
|---|---|---|
| Rubber is laid per wheel pass, not by the tyre's slip energy, and marbles are shed by the car's lateral g, not by the tyres' wear; a tyre driven through marbles does not pick them up (no grip lost for a lap after a run off line). | Med | Low |
| The client draws neither the rubbered line nor the marbles; the drying line is not drawn either (the road's sheen is one figure for the lap). | Med | Med |
| The forecast is worked out from the session's id: a host cannot write one, and nothing shows it ahead (no forecast on the create screen, no radar; the HUD names only the next change). | Med | Low |
| The wind's mean and the humidity hold for the session whatever the weather does: a storm front brings no gale. | Low | Low |
| The day is late May everywhere (no date, no season) and solar noon is 13:00 whatever the circuit's longitude and time zone. | Low | Low |
| Overnight cooling has no dew or fog; an evening session at a humid circuit is as clear as noon. | Med | Low |
| AC imports carry no latitude or north in their export (the sun is the sky model's default there). | Low | Low |

## AI and strategy

| Gap | Effort | Impact |
|---|---|---|
| The AI does not pull out of a tow to pass: the traffic layer holds a follower back. No defending, no mistakes under pressure. | High | High |
| Recovering from a wall: a car can stay pinned against a barrier (Nordschleife at ~440 m in the 2026-10-06 survey: 107 s in the GT3 race). | Med | Med |
| Pit strategy is thresholds: no undercuts, no reaction to the cars around it, no mandatory stops, no fuel saving to skip a stop. | Med | Med |
| No timed-race endgame: no splash-and-dash, no last-stint planning (`laps_left` does estimate from the clock for the pit plan). | Med | Low |
| Fuel saving is lift-and-coast only, and only for cars that cannot refuel; no short-shifting. | Low | Low |
| The AI never harvests or saves hybrid energy on purpose, and does not defend with the overtake button (it does press it within 0.8 s of a car ahead). | Low | Low |
| The AI's tyre crossover is a threshold on the racing line's water (`pit::tyres_for_the_track`): it never gambles on slicks, never reads the forecast and never stays out because the race is nearly over. | Med | Med |
| The AI drives the racing line whatever the road does: it does not move off a rubbered line to a wet one for grip, nor aim for the dry line in the rain. | Med | Low |
| The AI's speed plan assumes full engine power after damage; it drives around damage only through its steering loop and aero share. | Low | Low |
| `apexsim-replay render`, the showcases, the demo race and the AI survey race the mixed field, not a level. | Low | Low |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| Planned dry-tank, in clean air, still air, on warm new mediums, at the session's air density and grip as it started: fuel, the wake, the wind, tyre temperature, compound and wear, the rubber and the weather changing never change it (the AI's own plan does follow fuel). | Med | Low |
| Counts the hybrid motor at full power whatever the lap budget allows (optimistic for the F1s). | Low | Low |

## Race rules and the pit lane

| Gap | Effort | Impact |
|---|---|---|
| No in-race pit menu (compound, fuel, tyres only) and no pit request button: a stop always changes tyres, fuels to need and repairs everything. | Med | Med |
| No driver swaps or fixed stint lengths. | Med | Low |
| An F1 in a race longer than a tank runs dry: the start fill is capped and no stop adds fuel. | Low | Low |
| No pit-lane speeding penalties, drive-throughs or stop-go; no safety car, so no lane closure. | High | Med |
| Boxes are dealt in seating order, not by team. | Low | Low |
| A retired car jumps to its box: no recovery vehicle, no marshals, no yellow flag. Without a pit lane it stays where it stopped, out of everyone's way. | Med | Low |
| The autopilot takes over wherever the driver crosses the road edge; a car that dives across the mouth runs 3-4 m off the lane's middle before it settles. | Low | Low |
| On a curved box row (Silverstone) the 6 m garage modules overlap on the inside of the bend. | Low | Low |
| `initialize_content.ps1` does not check for the pit sidecar. | Low | Low |
| The session's damage rule has no "visual only" level, and the session browser does not show it. | Low | Low |

## Race weekends: sessions across days

The goal: an **event** that runs practice on one day, qualifying with the
same drivers on the next, and the race the day after, gridded from the
qualifying result. Today a session lives only while someone is connected
(removed `session_timeout_seconds`, 5 min, after the last leaves) and
nothing about it survives a restart but the lap records.
`GameMode::Qualification` is practice with timing, and every race grid
goes in seating order.

| Gap | Effort | Impact |
|---|---|---|
| No event: nothing groups practice, qualifying and the race on one track and car class, with a name, an entry list and a schedule (start time per session, in real days). | Med | High |
| Sessions are not persisted: an event, its entry list, each session's state and its results must survive server restarts and empty hours (stored beside `records/`, deterministic ids). | Med | High |
| Entries are not stable drivers: a player is a per-connection id and records key on the name, so the same driver must be recognised across days (an account or token identity, with car and livery fixed for the event). | Med | High |
| Qualifying has no length or format: it runs until the host moves on, so the classification (best legal lap, the earlier of equal times first) is whatever stands then. No session timer and no results screen after it (the garage scoreboard is live only; the result shows afterwards as a chip in the next race's Starting order row). | Low | Med |
| In qualifying a car appears at the pit exit (a teleport): it does not drive down the pit lane, so there is no pit limiter or queue at the exit light, and the HUD still shows race-style standings by track position (and the lap sheet says OUT LAP once). | Med | Low |
| The lobby's GRID list is in car-index order (the session's player ids), not the starting order the race will use: a host who asked for P1 can be listed third. The race itself grids as asked; the roster carries no grid slot to sort by. | Low | Med |
| The start order matches drivers by name: a stored result's humans are found again only under the same name, a no-show closes the grid up (their slot is not left empty), and the grid is reordered by mouse or by your own slot's stepper only, not by keyboard or pad. | Low | Low |
| No schedule: a session opens, counts in and starts at its scheduled time whether or not everyone is there; a driver joining late goes out from the pit lane. Needs a lobby view of upcoming sessions and a countdown. | Med | Med |
| No per-session rules within an event: practice length, qualifying format, race length, conditions per day (a different forecast each day), assists and damage fixed for all sessions. | Low | Med |
| AI entries in an event: they need fixed identities and levels across the days, a qualifying run of their own, and their results kept like a human's. | Med | Med |
| Setups and tyre sets do not carry between sessions (a parc fermé rule, tyres allocated per weekend). | Med | Low |
| No event results: the practice times, the qualifying sheet and the race classification as one weekend record, shown in the client and exported. | Med | Med |
| Replays and the timing board are per session; an event's replays and sheets should be browsable together. | Low | Low |

## Client feel, visuals and HUD

| Gap | Effort | Impact |
|---|---|---|
| Tyre heat is not a force-feedback or squeal cue; damage changes neither the engine note nor the feel. | Med | Med |
| The create screen's wind dial names its eight directions AHD / AR / R / BR / BHD...: terse, though the caption above it spells the pick out ("FROM THE RIGHT"). | Low | Low |
| The HUD colours brakes by temperature alone (the client does not know carbon from steel). | Low | Low |
| The hybrid mode resets to Balanced each race; it is not saved. | Low | Low |
| A puncture shows only as FLAT under the tyre (red, under 60 kPa) and the wear; the garage's stock Medium card does not say which wet tyre it will fit. | Low | Low |
| The chase camera ends up inside the garage when the car stops at its box or is towed there: the view is a pillar or the garage wall and the car is hidden (seen 2026-10-07 at Monza). | Low | Low |
| The intermediate and wet tyres are a tint only (bluer, matte): no tread pattern, so in a wide shot they hardly read apart from a slick. | Low | Low |
| Visible damage: one dent pattern per zone (no hit point on the wire), cosmetic debris the cars drive through, engine smoke always from the tail, no windscreen cracks or hanging parts, the DRS flap and driver never dented, wheels not drawn toed. | Med | Low |
| AC imports have no `[[damage_part]]` tables (the importer writes none): they dent and smoke but shed nothing. | Low | Low |

## Replays and spectating

| Gap | Effort | Impact |
|---|---|---|
| A live session is watched through racer telemetry, not as a stream; a mid-race joiner gets no earlier lap timing, and undercounts pit stops and tyre age. | Med | Med |
| Spectator streams carry no race clock (timed races show no time left), no `PitService` (frames do carry the damage percentages) and no live sky: a showcase, a watched replay and the backdrop show the sky the session started under, whatever its clock and forecast did. | Med | Low |
| Replays hold only stream rows: no tyre pressures, brake temperatures, fuel, ghost, or tyre and kerb sound. The server's own replays are still not `.apxs`. | Med | Low |
| The client's replay of a session stops at 512 MB compressed. | Low | Low |
| The timing tower is not clickable; the watch keys are not rebindable; the pad has no replay speed; no scrub bar. | Low | Low |
| The session browser is still the legacy, unstyled blueprint screen. | Med | Low |

## Assetto Corsa imports

| Gap | Effort | Impact |
|---|---|---|
| Imported tracks have no pit sidecar (so no pit stops) and no `altitude_m` / latitude / longitude metadata (they race at sea-level air density; the ground sidecar does carry heights). | Med | Med |
| AC's own tyre heat model is not carried, only its temperature window. | Med | Low |
| AC's dynamic aero controllers are not modelled, and a DRS flap is not split off an imported body. | Med | Low |
| AC's ERS delivery profiles and front-axle motors are not carried (a 919-style hypercar imports with its rear motor only). | Med | Low |
| Brakes take carbon or steel from the class, not from `brakes.ini`. | Low | Low |
| Kunos' 2015-17 F1 ground-height tables are flat, so those imports gain nothing from the aero map (faithful to AC). | — | Low |
| Imported AI crashes a lot: one to ten cars retire per survey race. | Med | Med |

## Built but never seen in the running game

Covered by automation tests only; each needs a look on screen.

- Race: contact sparks (a contact lasts a few frames and no timed screenshot of the 2026-10-07 runs caught one; the steam and the engine smoke were seen). Needs a human: a flat spot through a real wheel or pad.
- 2026-10-07: the garage's compound cards built from a car's own `[[tires.compound]]` list (no shipped car files one, so only the default five have been on screen; the own-list cards are pinned by `ApexSim.UI.Garage.CompoundCards`). Needs a human: the flat-spot thump in the road sound, a puddle or a dried line felt from the driver's seat, a slow puncture felt over a lap (its pressure falling to FLAT in the HUD was seen).
- Replays: SAVE REPLAY from the pause menu and garage, Keep and Delete, the watch view on a pad, a watched race finishing, replay speeds other than 2x, the session browser's Watch button clicked.
- 2026-10-07 track and sky: the flags turning with the wind (no shipped circuit places `sign/flag_pole` yet), the sun at a circuit's real latitude and north (needs the re-exported tracks), rain arriving and the road drying under a changeable sky, a rubbered start. A 60x clock at Monza was watched relighting at dusk with the floodlights coming on.
- The 420 Hz server (2026-10-06) through a real client: car motion, force feedback and the replay recorder's rate snap were tuned on 240 Hz ticks and 60 Hz telemetry; the divisor is 7 for the same 60 Hz. Not measured: tick jitter at 2.4 ms against the 1 ms Windows timer (only the mean rate is tested), and the AI survey at 420 against the 240 baseline (the AI's consistency noise is seeded from the tick number, so every race differs between the two rates by design, not by error).

## Before picking one up

Every change must keep the sim deterministic (`tests/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
against the commit before the work, as totals.
