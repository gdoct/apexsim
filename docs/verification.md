# Verification

Features that are built and tested but have not been seen in the running
game, heard, or felt on real hardware. Nothing here is known to be wrong;
each item is a look, a listen or a session that has not happened yet.
Bugs and rough edges on built features are in [follow-ups.md](follow-ups.md),
features not started in the [roadmap](roadmap.md), simulation and
racing-rule gaps in [simulation-gaps.md](simulation-gaps.md). An item
lives in exactly one of the four.

Items are grouped by **how they are checked**, so one run clears a whole
group. When an item has been checked, delete its bullet: if it looked
right nothing else happens; if it did not, the finding becomes a bullet
in [follow-ups.md](follow-ups.md) in the package that owns the feature.
When a run or group is empty, delete it.

## 1. Unattended screen runs

Each run is one launch: a circuit, a sky and a camera plan, with every item
that fits that combination on it. A launch costs minutes (server, track
build, countdown) and a shot costs nothing, so a run brings each feature to
the camera at a known second rather than waiting for it: the server's debug
events with the stand-in driver ([server/operations.md](server/operations.md#debug-hooks):
`APEXSIM_DEBUG_STAND_IN_DRIVER=1`, `APEXSIM_DEBUG_EVENTS`),
`apexsim.car.DamagePreview`, and `apexsim.cam.LookAt` / `Goto` through
`-ApexExecAfter`, with `-ApexAutoRace`, `-ApexScreenshotAfter` and the other
switches in [game/client.md](game/client.md#unattended-runs-and-debugging).
`python scripts/verify/run_checks.py <run>` runs one: the recipes are in
`scripts/verify/runs.yml`, each against a server of its own on port 9400,
and the shots land in `out/verify/<run>/` tiled 2x2 into labelled contact
sheets. `scripts/verify/spots.py <Track>` works out camera poses from the
track YAML and scene (low cells, grades, bends, a prop seen from the road).
Each shot is judged against what the feature's doc says it draws.

The geometry pass of 2026-10-13 (real elevation, banking with ramps, the
Karussell in concrete, paved aprons as run-off) has been seen on every
circuit it changed (2026-10-15, R1-R8).

### R1. Spa, heavy rain, stand-in driver

Events: `flatspot=FL` at two times some seconds apart, a `leak=FR`, a
`puncture=RR` late in the run.

- The rain light (`apexsim.car.RainLightNits`) in the wet; a body with no
  `car_rainlight` slot has none.
- Two lock-ups on one tyre drawn as two glazed patches at different places
  round the tread (`TyreCpd::FlatSpot2`). At racing speed the tread is a
  blur and the stand-in pits at a 0.6 spot: it needs a car held still
  ([follow-ups.md](follow-ups.md), content tooling).
- A slow leak's LEAK read-out (`item.leak`, checked only through the golden
  frame): grab it within the lap of the leak, before the stand-in pits.

### R2. Silverstone, cloudy, green start, long stint

`-ApexWeather=cloudy -ApexTrackRubber=0`, a long race; events: `wear=`,
`flatspot=`, `leak=`, `damage=rear:P` short of the wing's `detach_pct`.

- Marbles, and the rubbered line laid by a race: five laps of twelve cars
  from a green start lay about 0.24 rubber and 0.09 marbles
  (`RUBBER_PER_PASS`, `MARBLES_PER_PASS`), too little to see; it takes a
  race of about an hour. The colour and gloss figures in
  `ApplyRoadStateGraph` are first guesses.
- Pickup, blisters and the heat tint on the tread, the sidewall bulge of a
  deflated tyre and a flat-spotted slick, after a long stint
  ([server/vehicle-physics.md](server/vehicle-physics.md)): a moving car
  blurs them, so they wait for a car held still, as FlatSpot2 does.
- The static camber on the wheels close up.
- A rubbered start (`-ApexTrackRubber` above 0): the line dark from the
  first lap.
- The brakes coloured by their material (`brake.material`,
  `item.brake_state`: a steel GT3 cold under 100 °C and hot from 600, a
  carbon prototype cold under 300 and hot from 900), and the disc wear and
  stint energy data points (`item.disc_wear_pct`, `fuel.stint_pct`): no
  shipped car names `[brakes] disc_wear_per_mj` or `[fuel] stint_energy_mj`,
  so they need a custom car.toml with both. Only `ApexSim.Hud.Data.*` and
  the UDP golden tests have run.
- A part hanging before it comes off (`UpdateHangingParts`: the hinge edge,
  the 22° droop): it droops fore and aft, so from the side.

### R4. Snow at Spa, changeable, into the evening

`-ApexWeather=snow -ApexChangeable=1 -ApexTimeScale=30` from 15:00. No
AC-imported circuit is installed; Spa stands in.

- Snow: ice (once the snowfall stops and the line freezes) and a melt (may
  never come at winter temperatures; if it does not show, it is a
  test-only item and moves to [follow-ups.md](follow-ups.md)).
- The wind turning with a front (`LiveConditions::veer_target_deg`): the
  snow's drift and the cloud swinging round over a few minutes after a
  change of weather, the HUD's wind arrow with it.
- Snow on the props: the faces-up blend on walls, roofs and barriers close
  up, from a camera looking down on them (tree cards have been seen), and
  an AC-imported circuit's props. The first R4 race ended within five
  minutes with the whole field stuck off the road, before the ice.
- The field through a long snow race: no engine out without a hit, and no
  car left at the limiter on Raidillon or rocking against a wall (the AI's
  standstill spin limit, its no-headway reset with a run-up, and traction
  control cutting the engine's heat; `snow_stall_survey` holds every
  engine under 82 °C in 15 minutes of a Chiffon field).

### R5. Mandarina Bay at night, cloudy

`-ApexTrack=MarinaBay -ApexTimeOfDay=21:00 -ApexWeather=cloudy`, a TV lap
plus `LookAt` stops ([content/props.md](content/props.md)).

- The cloud dome at night, from open ground (the pit building's ceiling
  filled the first shot), and in the menu's backdrop.
- The pit podium.
- Whether the sausage kerbs catch the AI: they lay 240 pieces here and 660
  on the Nordschleife, where the AI survey moved a little (more off-road
  and sliding, no retirements). Seen on screen inside the tight bends.
- The road under the floodlights after the floodlit exposure ceiling
  (2026-10-17): grey tarmac with pools of light from the TV and overhead
  cameras (the 2995 m shot's road 27% darker, blown pixels 7.6% to 4%);
  a human's eye on whether the night now reads too dark between lamps.

### R6. Nordschleife, sunny, TV lap and dressing stops

The TV camera follows a lap; `LookAt` stops come from the layout dossier
([content/circuits.md](content/circuits.md)). The dressing was imported and
baked on 2026-10-14 and seen only in four unattended shots (Brünnchen's bank
and hedge, which still read a lighter green than the grass beside them; road
graffiti; a marshal post).

Seen on 2026-10-15: the Karussell, skid marks, a track bridge's parapet,
an overpass, the gantry, the camp villages, a rail tag on its beam. From
30 m at 1080p the rest was too small or out of frame: closer cameras.

- The numbered posts and the grade plates read from road height.
- The rail tags on the other side of the road (not flipped, not floating).
- The toll lanes; the spectator banks on a sloping verge.
- The lookout tower on the Hohe Acht seen from the Hohe Acht section; the
  hillside letters seen from the Döttinger Höhe (or hidden by the trees
  between); Brünnchen's hedge colour.

### R7. A seeded pile-up replay, before and after

Four unattended runs (snow and heavy rain, chase and watch cameras) left
the player's car unhit or exited silently (the silent exit is in
[follow-ups.md](follow-ups.md), content tooling), so this run watches a car
that is hit in a seeded replay (`-ApexReplay`, `-ApexWatchCar`).

- A debris strike's dent.
- A part hanging from a real hit: its flutter (frames a fifth of a second
  apart).
- Contact sparks: a contact lasts a few frames and no timed shot has
  caught one (the steam and the engine smoke have been seen).
- Racecraft (`crate::racecraft`): passes, defensive moves, a car leaving
  room, a backmarker yielding and a car reversing out of a barrier, read
  so far only off the AI survey's numbers and traces. Whether leaving a lane
  after a pass and the reverse look natural, and how the side-by-side
  contact the survey counts looks.
- An AI backing out of a barrier after the pile-up stopping in reverse for
  a car arriving, and backing on once it is by (`racecraft::recover`,
  [server/ai.md](server/ai.md)).

### R8. Stadium stands and pit rows

`r8-ims` and `r8-mexicocity` aim at their `grandstand_corner`; the first
pass put the camera at road level and in a chase view: aim again, from
higher and further off.

- A stadium grandstand wedge on a corner.
- A curved pit box row tighter than about a 130 m radius (the garages
  scale down to 0.85 and may still overlap a little).

### R9. Menus, dashboard and two clients

One `-ApexNoDemo` launch clicking through with `apexsim.ui.Texts` /
`apexsim.ui.Click`, the dashboard driven over HTTP during it, and a second
client on the same server for the grid card
([game/client.md](game/client.md), [game/hud-modding.md](game/hud-modding.md),
[server/operations.md](server/operations.md)).

Seen on 2026-10-15: the car stat sheet (with the car's own compounds),
See track guide, replay export, the HUD layout picker's row, a dashboard
broadcast in the menus and over the race HUD, and the dashboard ending a
session while driving (notice, then the menu).

- The layout picker switching the HUD in a race; per-circuit and per-class
  bindings and switching by place.
- The dossier trace page in a browser (`scripts/trace_layout.py Spa --open`,
  [content/track-pipeline.md](content/track-pipeline.md#layout-dossiers-osm_layoutpy)):
  the centerline and stands over the imagery where they belong (the
  Raidillon stand on its bank), the station marks, a traced span's
  `MANUAL_STANDS` snippet and GO TO. The pages build for every circuit
  with a DEM, the georeference round-trips in Python and the script
  passes a syntax check; nobody has opened one.
- The Config view's RESTART SERVER button in a browser ([server/operations.md](server/operations.md#admin-dashboard)):
  the two confirms, the toast, the page coming back on its own once the
  new process answers, and a player in a race getting the shutdown notice.
  The restart itself has run over HTTP (a saved tick rate in force in the
  new process, same ports, 2026-10-16) and `a_restart_is_asked_from_the_dashboard`.
- A streamed hotlap watch on a saved setup (S with setups saved for the
  car; only the stock lap and the "no saved setup" toast were run). Seen
  unattended at Monza: pause, ten seconds back, 2x back to live, the ghost
  of the first lap and its replay.
- The race's grid card ([server/sessions.md](server/sessions.md#the-races-grid)),
  never on screen: the grid mode of the garage card over the lined-up
  field (READY, the livery, START NOW, the grid list), the car repainting
  behind it on a livery change, the card closing as the countdown starts.
  Two clients on one server: the guest's card, the ready marks crossing
  over, and START NOW closing a card that is not ready. Only the server
  side (`ready_grid_test`) and the codec have run.
- A non-qualifier on screen ([server/sessions.md](server/sessions.md#session-rules-set-on-create)):
  a qualifying session (or a weekend's Q1) under the 107% rule with a car
  outside it, then the race: DNQ in the standings, the tower and the
  results screen, the car parked in its box. Seen only as the create
  screen's Cut-off row; the roster mark is checked by `roster_wire_format`
  and `ApexSim.RaceResults.*`.
- A weekend's day skies in a race ([server/sessions.md](server/sessions.md#race-weekend)):
  practice under its own weather and hour from the create screen's
  Practice sky row, the sky changing at the qualifying day and again at the
  race, and the forecast the row showed coming true; the garage's and the
  lobby listing's weather and the racing-line dots following each day.
  Only the codec (`C_CreateSessionWeekendDays`, `S_WeekendStateConditions`),
  `ApexSim.UI.CreateSession.WeekendDays` and the row on screen have run.
- Joining a race weekend between phases from a second client: the browser's
  Between sessions badge, Join, and the late entry named in the garage
  ("A LATE ENTRY"), the timesheet's LATE ENTRY tag and the results summary.
  Only the codec (`S_LobbyStateWeekendWaiting`, `S_WeekendStateLate`) and
  `ApexSim.UI.SessionBrowser` have run.
- An empty grid slot ([server/sessions.md](server/sessions.md#starting-order-and-qualifying)):
  a Custom start order naming a driver who is not there, then the session
  lobby's GRID list, the grid card's list and the lined-up field with the
  gap kept. Only `grid_order` and `game_session` unit tests have run.
- The racing line aid planned again at a lap's end: after the first lap
  with a full tank, the dots repaint (braking points earlier) without a
  hitch and stay on the road; in rain the braking zones lengthen.
- The garage's compound cards built from a car's own `[[tires.compound]]`
  list: only an AC import files one (its compounds plus the two rain
  tyres), so only the default five have been on screen
  (`ApexSim.UI.Garage.CompoundCards` pins the rest).

### R10. Spa, sunny, 13:00

`-ApexTrack=Spa -ApexWeather=sunny -ApexTimeOfDay=13:00`, a TV lap.

- The sun at the circuit's own noon (`SessionJoined.Conditions.solar_noon_min`,
  13:33 at Spa): at 13:00 it stands a little east of due south over the
  circuit.
- The create screen's Date row against a server that lists the track's
  site: the note naming the circuit's own noon ("SOLAR NOON ... HERE"), a
  December 16:30 at Silverstone previewed as dusk and raced as one. Seen
  only without a server (13:00, the row stepping to 15 Dec and the
  preview's sun dropping).
- The sun at a circuit's real north, and rain arriving and the road drying
  under a changeable sky (`-ApexChangeable=1`).
- The flags turning with the wind: no shipped circuit places
  `sign/flag_pole`, so this waits for one that does.

### R11. Pit plan and pit rule

`r11-pit-plan`, `r11-pit-rule` and `r11-create-pit-rule` in
`scripts/verify/runs.yml` ([game/client.md](game/client.md),
[server/pit-lane.md](server/pit-lane.md),
[server/sessions.md](server/sessions.md#session-rules-set-on-create)).

Seen on 2026-10-16: the pause menu's PIT PLAN page set by pad (tyres kept,
+5 L) and its row's summary, the `BoxThisLap` key lighting BOX until the
stop, the stop on that plan (the pit panel's bar the fuel alone,
`PitService` tyres 0 s, 5 L), the create screen's Stops and Two compounds
rows with the summary naming them, STOPS OWED on the race state, and PEN on
the finish overlay's rows after a one-lap race under a one-stop rule.

- PEN in the HUD's standings, the timing tower and the watch view's results
  (`item.missed_stops`), and the session results screen's PEN tag: the
  one-lap run ends on the finish overlay. Only `ApexSim.Hud.Data.PitRule`
  has run.
- The plan sent again after a rejoin (`Rejoined ...: sending the pit plan
  again` in the client log, the stop still on the plan), and a stop on a
  plan with a chosen pad set moving the garage's Brake pads knob. Only
  `ApexSim.Race.PitPlan.*` has run.
- The `BoxThisLap` wheel slot on a real wheel, and the second compound owed
  (`pit.compound_owed`, "2ND SET") under a Two compounds rule.

### R12. The create screen's sky and qualifying formats

`r12-create-sky`, `r12-create-qualifying`, `r12-shootout`,
`r12-lap-allowance` and `r12-forecast-hud` in `scripts/verify/runs.yml`
([server/conditions.md](server/conditions.md#client),
[server/sessions.md](server/sessions.md#starting-order-and-qualifying)).
The create runs save to the player's profile: keep a copy of
`game-unreal/Saved/SaveGames/ApexProfile.sav` and put it back after.

Seen on 2026-10-16: the Conditions tab on 15 Jan at the Norisring (AUTO
air 1 °C at 23:00 against late May's 15 °C, solar noon 12:25), a forecast
written with ADD STEP and moved by pad, the written line kept to a winter
sky (rain as snow) once the air was fixed at 0 °C; the qualifying Format
and Lap allowance rows, the weekend's Final round, Practice / Qualifying
damage rows and the summary naming them; a shoot-out against two AI: the
run order with names, OUTLAP / FLYING LAP / DONE, "YOU RUN 3RD · 2 TO GO"
then "1 TO GO" on the disabled GO OUT, the scoreboard opened with the
camera on each car as it went out, and the YOUR TURN banner as the player's
car was sent out; a written forecast echoed and laid out in `sky.outlook`
(`apexsim.hud.Data`), and `qualifying.laps_left` reading 2 on the track.
Seen on 2026-10-17 (after the stand-in fix): the player's shoot-out run
driven to its flying lap and the shoot-out's results sheet (QUALIFYING ·
SHOOT-OUT · ONE FLYING LAP EACH, RAN 3RD, pole); the lap allowance counting
2, 1, 0 (`qualifying.laps_left`, LAPS LEFT 1 on the race state) and the
session's results once every car's laps were used. Luna Swift's Murcetes
set no time in either run (0/1, 0/2): the AI's laps struck for track
limits, as in [server/sessions.md](server/sessions.md).

- The lap allowance's USED on the race state and LAP ALLOWANCE USED with
  GO OUT disabled in the garage: the session ends as the last car's laps
  are used, so it needs a player done before the AI (an allowance of one,
  or a longer AI field); and GO OUT's "2 timed laps left" badge (not up at
  12 s, before the session was in qualifying).
- A weekend whose final round is a shoot-out, live, and the HUD's
  conditions panel showing the written outlook (the race layout used had no
  conditions panel up).

### R13. Race control and the radar, live

A race with the stand-in and a full field, in a chase camera, at a circuit
whose first lap bunches (Monza's first chicane). Replays carry neither
(follow-ups, flags).

The first `r13-race-control` (2026-10-17) showed YELLOW and DOUBLE YELLOW
in the `flags` component and the radar's RIGHT marker at the start.

- The `flags` component's BLUE FLAG, with a leader a lap up close behind
  (a long race, or a slow stand-in).
- The AI lifting and holding station past an incident.
- The `radar` component's bars reading at a glance over a chase camera's
  car (widened to 14 px after the first run), red as a car closes.

## 2. Needs a human: hands, ears and hardware

([game/input-and-ffb.md](game/input-and-ffb.md),
[game/spectator.md](game/spectator.md), [game/audio.md](game/audio.md))
Everything an unattended run cannot check: one sitting with a real
wheelbase, pedals, a shifter, a pad and speakers, in this order so each
step feeds the next. Until it has run, all of this is unverified.

**Wheel, pedals and shifter.** Drive a lap of the Nordschleife and one of
Mandarina Bay.

- A clutch pedal on the wheel tab's `Clutch` slot (sent as
  `PlayerInput.clutch`): whether its throw and the server's torque cut feel
  right.
- An H-pattern shifter on the `HGear*` slots.
- The handbrake's feel (`HANDBRAKE_SHARE`, how fast the tail comes round).
- A front-axle hybrid motor (`[hybrid] axle = "front"`; an AC 919 import
  re-imported with `[FRONT_MOTORS]`): the pull at both ends on corner exit
  above its minimum speed, the front recovery under braking, and that the
  ERS HUD shows it deploying and harvesting. Only tests and a scripted
  drive have seen it.
- The peak torques and the gear / hybrid minimum forces (5% / 3%) felt on
  a base.
- The PXN profiles (V10, V12, V12 Lite, V99: names and peaks from the
  maker's listings, no vendor id, so they match by name alone), and the
  `ApexWheelProfiles` name rules against what real bases report.
- The sausage kerbs by hand on both circuits, and the Nordschleife's posts
  and grade plates read from the cockpit.
- Greasy tyres and damage by hand (`ApexFfb::ReadCarCondition`): fronts
  cooked past their window (the rim 15% lighter, the scrub lower), a side
  hit hard enough to bend it (the wobble once a wheel turn, the server's
  pull), and an engine past 10% damage (the half-order shake), on a wheel
  and on a pad. Only `ApexSim.Input.ForceFeedback.CarCondition` has run.
- A tread spread by hand: a setup with a lot of toe-in, a long stint of
  heavy braking, then the HUD's inner/outer tyre readouts (the outer and
  the middle running hot) and whether a cooked shoulder is felt as lost
  grip before the mean leaves the window (`tyre_thermal::tread_grip_factor`).
  Only the module tests have run.
- A banked turn and a crest taken across the road by hand
  (`physics::path_curvature_accel`): the bank's extra load felt as more
  grip through it, a diagonal crest as the car going light. Only the unit
  test and the hump test have run.
- A gravel trap by hand (a Monza chicane run straight on) and Sakhir's
  desert floor: how hard the trap bogs the car and whether it can be
  driven out (`OffGround`, [server/vehicle-physics.md](server/vehicle-physics.md)),
  and how each ground comes through the rim and the pad (gravel's rattle,
  sand's slow heave and drag, `ApexFfb::GroundMix`), and how it sounds
  (gravel's stones, sand's hiss). Only the automation tests have run.
- A car's own dust from a real trap (telemetry's `WheelGround`): the plume
  has been seen only forced on every car with `apexsim.car.GroundPreview`
  at Sakhir (2026-10-16), never from a car actually in the gravel.
- A flat spot through a real wheel and a pad, a puddle and a dried line
  felt from the seat, and a slow puncture felt over a lap.

**Listen.** Start with the replay of the lap just driven.

- A replay's tyre and kerb sound (the watched car's road sound from its
  row's wheels, `UpdateWatchedCarRoadSound`) against the live run.
- A hotlap replay's ghost from its `Ghost` record.
- An overheated tyre's squeal (sooner, lower, smeared) and a damaged
  engine's misfires and knock, by ear: `apexsim.audio.RenderCars` writes
  `road.wav` (overheated fronts) and `<car>_damaged.wav`, and
  `apexsim.car.DamagePreview "0,0,0,0,70"` plays the knock in a race.
  Judged only by the spectra in `ApexSim.Audio.RoadGreasy` / `EngineDamage`.
- The flat-spot thump in the road sound.
- A promo clip's sound by ear ([marketing-site.md](marketing-site.md)):
  measured on one 5 s onboard cut at Spa (the note follows the car's rpm
  within two frames, 5.00 s of sound for 5.00 s of frames), never listened
  to; a slowed shot's lowered pitch; the clip and music levels of a whole
  cut (`clip_volume` 0.7).

**Pad and mouse.**

- The session browser's filter pills (All, Joinable, Racing, Recorded) by
  mouse.
- The HUD editor on a real pad: R3 for the layout manager, L3 for Reset
  all (pressed through by the pad walk, not yet by hand); the layout
  manager by hand.
- The streamed hotlap watch's keys and transport from a pad.
- The grid card's pad and keyboard focus.
- A pad under the GameInput plugin (not enabled in the project): the light
  motor on `RightLarge`, the triggers quiet (`ApexFfb::ToPadChannels`).

**Racing the AI.** A race against a mixed field at Monza and Zandvoort.

- The room an AI leaves a player alongside into a corner
  ([server/ai.md](server/ai.md), Room): half a car alongside on the inside
  and the AI takes the outside lane and lifts, widening if the player
  comes over. Only the unit test `room_is_left_for_a_player_alongside`
  has run; the AI survey has no player in it.
- An AI defending against a player: one move to the inside, early on the
  straight when the player is closing in its tow, never a second one.

**An imported circuit's pit stop.**

- A human drives an AC-imported circuit's pit lane on screen: the speed
  limit, the autopilot taking over at the first line, the car stopping at its
  box among AC's own garages, the service, the hand-back at the exit line.
  The sidecar's lane, limit lines and boxes are checked only by the server's
  surveys (`PIT_CUSTOM=1 pit_stop_test`); AC's scenery draws the garages and
  boards, the exit light is not drawn.

**Telemetry output.** ([game/telemetry-output.md](game/telemetry-output.md))

- The `telemetry_out` packet read by an app on real hardware: SimHub (a
  custom UDP plugin or game profile on the documented layout), a motion
  platform's surge, sway and heave from `accel` (sign and scale as felt),
  wheel rev lights from `rev_fraction`. Only the layout, mapping and
  motion tests (`ApexSim.TelemetryOutput.*`) have run.

**Last, with the network off.**

- The launcher's AC import button, never clicked by hand, and the import
  with the network off (it fetches its Python packages from PyPI on first
  use; the release's `Tools/importer/wheels` venv was checked only as the
  same `pip --no-index` command against downloaded wheels).

## 3. Platforms and GPUs

([building.md](building.md)) Each needs the machine.

- The 420 Hz server through a real client: car motion, force feedback and
  the replay recorder's rate snap were tuned on 240 Hz ticks with 60 Hz
  telemetry (the divisor of 7 keeps 60 Hz). Tick jitter, measured idle on
  the RTX 3080 box (2026-10-17, `tick_jitter_seconds = 15`, four windows):
  the mean exact (6300 ticks a window), median 2.48-2.50 ms against the
  2.38 ms budget, p99 4.5 ms, the longest 11-25 ms; 17% of ticks within
  10% of the budget, 15% under half (catching up), 31% at 1.1-1.5x, 0.4%
  at twice or more. The tokio interval's 1 ms granularity on the 1 ms
  Windows timer. Whether a client feels it (the motion buffer interpolates
  over it) is the open part.
- Integrated GPUs: the LOW preset and the integrated-GPU first-run default
  are seen only on an RTX 3080 at LOW. No Intel Iris Xe / Arc or AMD
  Radeon 680M / 780M has run the game, so the frame rate there and whether
  `GRHIDeviceIsIntegrated` picks LOW are unknown. Until one has, the
  requirements say a discrete GPU is recommended.
- Windows on Arm (Snapdragon X), parked: the release is x64 only. On an
  Asus Zenbook A14 under x64 emulation DirectX 12 showed only the sky and
  DirectX 11 (`display.graphics_api: dx11`) drew about one frame a second,
  then hung. No log came off the machine.
