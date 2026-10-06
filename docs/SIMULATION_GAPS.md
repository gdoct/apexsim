# Simulation gaps

What the simulation still leaves out, grouped by area. Each gap has an
**effort** (how hard to build and land safely) and an **impact** (how much
it changes driving and racing), both Low / Med / High. Built features
are documented in CLAUDE.md; this file only lists what is missing.

Keep it current: when a gap is closed, delete its row; when a feature
leaves something for later, add a row.

## Car physics

| Gap | Effort | Impact |
|---|---|---|
| Crests do not unload the car: the loads carry no vertical acceleration. | Med | Med |
| No aero yaw or roll sensitivity, no porpoising. | Med | Low |
| No camber thrust; one bump-stop gap per car; full suspension kinematics (roll centres) deliberately not modelled. | High | Low |
| The force feedback's column stiffness ignores toe. | Low | Low |
| No damage from kerb strikes, bottoming or landings. On the road mesh no AI car reaches full travel or leaves the ground, and compression speeds are led by one-tick spikes where the mesh steps (18 m/s at Suzuka). Needs a filtered strike measure first. | Med | Low |
| A pit stop's repair also undoes engine wear, which no real crew could do. | Low | Low |
| Regen under braking is free energy: no brake-by-wire split. | Med | Low |
| No per-stint hybrid energy (the WEC limit) and no manual-override energy; the overtake button only overrides the pacing. | Med | Med |
| No brake wear and no pad choice; one duct knob for all four corners. | Med | Low |
| No radiator (grille) setup knob; `radiator_scale` is car.toml only. | Low | Low |
| The differential exists and every shipped car.toml has a `[differential]` table, but none sets `simulated = true` (it defaults to false), so no car runs it. | Med | Med |
| `[fuel] tank_front_share` exists; no shipped car sets one. | Low | Low |

## Tyres

| Gap | Effort | Impact |
|---|---|---|
| No inner / middle / outer temperatures: the tread is one node. | Med | Med |
| The surface layer's constants were set against the AI's windows, never against a real tyre's surface swing. | Low | Low |
| Dirty air costs grip and cooling but adds no extra sliding heat. | Low | Low |
| Compounds are the same five changes on every car; a car.toml cannot list its own. | Med | Low |
| A flat spot never wears round again, has no sound, and the AI does not stop for one. | Low | Low |
| Punctures come only from wearing through: no debris, no hit, no slow puncture. | Med | Low |
| Rain is one figure for the whole lap: no puddles, no drying line. | High | High |
| The client draws no treaded or wet tyre, and no tyre smoke. | Med | Low |

## Track and environment

| Gap | Effort | Impact |
|---|---|---|
| No track evolution: no rubbering-in along the line, no marbles, no drying line. The strongest "better than other sims" feature. | Med | High |
| Conditions are fixed for a session: no rain arriving, no drying, no evening cooling, no clock moving through a 24-hour race. | High | High |
| Latitude is stored per circuit but unused: the sun is the sky model's 50° N everywhere. | Low | Low |
| No humidity control on the create screen (auto only). | Low | Low |
| The client draws no wind in the world: no windsock, flags or rain drift (the create screen does pick a direction). | Med | Low |

## AI and strategy

| Gap | Effort | Impact |
|---|---|---|
| The AI does not pull out of a tow to pass: the traffic layer holds a follower back. No defending, no mistakes under pressure. | High | High |
| Recovering from a wall: a car can stay pinned against a barrier (Nordschleife at ~440 m in the 2026-10-06 survey: 107 s in the GT3 race). | Med | Med |
| Pit strategy is thresholds: no undercuts, no reaction to the cars around it, no mandatory stops, no fuel saving to skip a stop. | Med | Med |
| No timed-race endgame: no splash-and-dash, no last-stint planning (`laps_left` does estimate from the clock for the pit plan). | Med | Low |
| Fuel saving is lift-and-coast only, and only for cars that cannot refuel; no short-shifting. | Low | Low |
| The AI never harvests or saves hybrid energy on purpose, and does not defend with the overtake button (it does press it within 0.8 s of a car ahead). | Low | Low |
| The AI's speed plan assumes full engine power after damage; it drives around damage only through its steering loop and aero share. | Low | Low |
| The AI always takes the weather's tyre: no gamble on slicks in the damp. | Low | Low |
| One AI level for the whole field: drivers have names and a ±2 spread around the level, but no chosen per-driver levels or separate aggression and consistency. The levels are not calibrated against lap times. | Med | Med |
| `apexsim-replay render`, the showcases, the demo race and the AI survey race the mixed field, not a level. | Low | Low |

## Racing line aid (the player's)

| Gap | Effort | Impact |
|---|---|---|
| Planned dry-tank, in clean air, still air, on warm new mediums, at the session's air density and grip: fuel, the wake, the wind, tyre temperature, compound and wear never change it (the AI's own plan does follow fuel). | Med | Low |
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
| Qualifying has no length or format: it runs until the host moves on, so the classification (best legal lap, the earlier of equal times first) is whatever stands then. No session timer, and no results screen: the result shows only as a chip in the next race's Starting order row. | Low | Med |
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
| The HUD colours brakes by temperature alone (the client does not know carbon from steel). | Low | Low |
| The hybrid mode resets to Balanced each race; it is not saved. | Low | Low |
| A puncture shows only as 15 kPa and the wear; the garage's stock Medium card does not say which wet tyre it will fit. | Low | Low |
| Visible damage: one dent pattern per zone (no hit point on the wire), cosmetic debris the cars drive through, engine smoke always from the tail, no windscreen cracks or hanging parts, the DRS flap and driver never dented, wheels not drawn toed. | Med | Low |
| AC imports have no `[[damage_part]]` tables (the importer writes none): they dent and smoke but shed nothing. | Low | Low |

## Replays and spectating

| Gap | Effort | Impact |
|---|---|---|
| A live session is watched through racer telemetry, not as a stream; a mid-race joiner gets no earlier lap timing, and undercounts pit stops and tyre age. | Med | Med |
| Spectator streams carry no race clock (timed races show no time left) and no `PitService` (frames do carry the damage percentages). | Med | Low |
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

- Hotlap garage: the Intermediate and Wet cards, the Aero section, the Next tyres and Brake ducts rows.
- Create screen: the AI level stepper (laid out, never clicked through), the air temperature slider and the wind row, the Qualifying tile, and the Starting order row (the chips, your grid slot, dragging and click-to-move on the grid, a stored result loaded).
- HUD: the PIT badge lit, the ERS badge and keys, a punctured tyre, OUT in the standings.
- Race: a flat spot through a real wheel or pad, a towed car parked in its box, an AI pit stop as the client draws it, damage steam and sparks.
- Replays: SAVE REPLAY from the pause menu and garage, Keep and Delete, the watch view on a pad, a watched race finishing, replay speeds other than 2x, the session browser's Watch button clicked.
- The 420 Hz server (2026-10-06) through a real client: car motion, force feedback and the replay recorder's rate snap were tuned on 240 Hz ticks and 60 Hz telemetry; the divisor is 7 for the same 60 Hz. Not measured: tick jitter at 2.4 ms against the 1 ms Windows timer (only the mean rate is tested), and the AI survey at 420 against the 240 baseline (the AI's consistency noise is seeded from the tick number, so every race differs between the two rates by design, not by error).

## Before picking one up

Every change must keep the sim deterministic (`tests/determinism_test.rs`),
teach the AI and the racing line anything that moves grip over time, append
(never insert) wire fields with golden bytes on both sides, and stay off
allocations in the 420 Hz loop. Judge physics changes with the AI survey
against the commit before the work, as totals.
