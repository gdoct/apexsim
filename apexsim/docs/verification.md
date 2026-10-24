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
Suzuka's real elevation (2026-10-18, `dem_elevation.py` with the crossover
held at 7 m) has not: the bridge deck and the road under it from the
road and from above, Degner-to-hairpin's new grades, and the stands and
barriers re-seated on it (`spots.py Sudoku`).

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
- Run 2026-10-24 (`r4-spa-snow`, after A5's winter-tyre AI): the snow on
  the ground, the falling snow, the stands and the evening light render;
  the props' faces-up snow is not readable at these poses. The player's
  stand-in Chiffon stood at 0 km/h in first gear from about 300 s to the
  end (560 s), its flat spots at 76%, more than 300 s behind: a car stuck
  in the snow, as this bullet's field check fears. Its client and server
  logs are in `out/verify/r4-spa-snow/`: find where it stopped and why
  before closing the snow rows.
- A Rally field (`oudi-kwattro-s1`, `pugeot-205-t16`, `lancha-037-rally`) on
  the covered road: the AI fits the studded snow tyre, the wheels draw the
  snow tread with its steel studs and the ice-white ring, and the field
  laps clearly quicker and stays on the road longer than a GT3 field.
- The rally cars themselves, in the garage and on the grid: the projected
  skins in every livery (colours as painted: Blender's preview shows the
  reds washed toward pink; text reading true on both flanks, the bonnet
  plate, the door roundels, the screen banner), the rally wheel and
  steering wheel, the cockpit view (both seats, the gauges), the mud
  flaps and the wings, and each car's engine sound.

### R5. Mandarina Bay at night, cloudy

`-ApexTrack=MandarinaBay -ApexTimeOfDay=21:00 -ApexWeather=cloudy`, a TV lap
plus `LookAt` stops ([apexsim-content/docs/props.md](../../apexsim-content/docs/props.md)).

- The cloud dome at night, from open ground (the pit building's ceiling
  filled the first shot), and in the menu's backdrop.
- The pit podium.
- Whether the sausage kerbs catch the AI: they lay 240 pieces here and 660
  on the Nordschleife, where the AI survey moved a little (more off-road
  and sliding, no retirements). Seen on screen inside the tight bends.
- Run 2026-10-24 (`r5-mandarina-night`): the cloud dome over the pit
  straight, the floodlit corners from above (grey tarmac, pools of light)
  and the city at night read well; two TV shots were a white and a black
  frame and the headlights blow the road white ahead of a car (both in
  follow-ups, Rendering). The pit podium was not in shot.
- The road under the floodlights after the floodlit exposure ceiling
  (2026-10-17): grey tarmac with pools of light from the TV and overhead
  cameras (the 2995 m shot's road 27% darker, blown pixels 7.6% to 4%);
  a human's eye on whether the night now reads too dark between lamps.

### R6. Nordschleife, sunny, TV lap and dressing stops

The TV camera follows a lap; `LookAt` stops come from the layout dossier
([apexsim-content/docs/circuits.md](../../apexsim-content/docs/circuits.md)). The dressing was imported and
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

Run 2026-10-24 (`r6-nordschleife`): the overpass at 3964 m, the timing
gantry, the camp villages (tents, caravans, flags), the K 73 bridge, the
skid marks and the road graffiti read well. Still not judged from these
shots: the marshal post's number and the grade plates are a few pixels at
the poses' distance, no rail tag is in frame, the toll booths and the
spectator banks sit far behind the catch fence, the Hohe Acht tower and
the Döttinger Höhe letters are hidden by the fence and trees from the
road, and Brünnchen's hedge is not in shot. The poses need moving closer
(`spots.py --prop`) before these close.

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
- The repainted seated crowd (`gen_crowd.py`) on a full stand with
  spectators on: figures read as people at 30-150 m, the masked edges
  hold at distance, the one chequered flag per strip does not repeat in
  a visible pattern.

### R9. Menus and dashboard

One `-ApexNoDemo` launch clicking through with `apexsim.ui.Texts` /
`apexsim.ui.Click`, and the dashboard driven over HTTP during it
([game/client.md](game/client.md), [game/hud-modding.md](game/hud-modding.md),
[server/operations.md](server/operations.md)).

Seen on 2026-10-15: the car stat sheet (with the car's own compounds),
See track guide, replay export, the HUD layout picker's row, a dashboard
broadcast in the menus and over the race HUD, and the dashboard ending a
session while driving (notice, then the menu).

- The layout picker switching the HUD in a race; per-circuit and per-class
  bindings and switching by place.
- The dossier trace page in a browser (`python tools/trace_layout.py SpaFrankenchamps --open` in `apexsim-content/`,
  [apexsim-content/docs/track-pipeline.md](../../apexsim-content/docs/track-pipeline.md#layout-dossiers-osm_layoutpy)):
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
- The dashboard's Incidents view in a browser ([server/operations.md](server/operations.md#admin-dashboard)):
  a running race's list growing, the kind filter, CLEAR on a penalty, a
  finished race's row opened, DOWNLOAD REPLAY and its command line putting
  the game's replay at the incident. The API is tested
  (`admin_test::incidents_are_listed_cleared_by_a_steward_and_kept_with_the_replay`);
  the page has passed a syntax check only.
- A streamed hotlap watch on a saved setup (S with setups saved for the
  car; only the stock lap and the "no saved setup" toast were run). Seen
  unattended at Monza: pause, ten seconds back, 2x back to live, the ghost
  of the first lap and its replay.
- The racing line aid planned again at a lap's end: after the first lap
  with a full tank, the dots repaint (braking points earlier) without a
  hitch and stay on the road; in rain the braking zones lengthen.
- The garage's compound cards built from a car's own `[[tires.compound]]`
  list: only an AC import files one (its compounds plus the two rain
  tyres), so only the default five have been on screen
  (`ApexSim.UI.Garage.CompoundCards` pins the rest).

### R10. Spa, sunny, 13:00

`-ApexTrack=SpaFrankenchamps -ApexWeather=sunny -ApexTimeOfDay=13:00`, a TV lap.

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
- The far haze since the fog's light follows the sky (`ApexSky::FogLuminance`,
  2026-10-17): seen at the Nordschleife in sun (`g3-horizon`: hills
  fading into pale haze past the 30 km skirt). Left: other circuits,
  overcast, rain, dusk and night, where a haze too bright or too dark
  would show first.
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

- A stop on a plan with a chosen pad set moving the garage's Brake pads
  knob.
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

- The Race tab's Race control rows (`r12-race-control`): on 2026-10-19 the
  four rows whole beside the pit rule, reached by arrow keys, and the
  section line and summary naming the picks. Still to see: a session made
  from them running under them (a rolling start or a formation lap, the
  reverse grid on the grid, a safety car), and the pad walk
  (`ApexSim.UI.PadWalk`) over the rows.

### R13. Race control and the radar, live

A race with the stand-in and a full field, in a chase camera, at a circuit
whose first lap bunches (Monza's first chicane). Replays carry neither
(follow-ups, flags).

The first `r13-race-control` (2026-10-17) showed YELLOW and DOUBLE YELLOW
in the `flags` component and the radar's RIGHT marker at the start.

- The AI lifting and holding station past an incident.
- A rolling start's form-up (`start: Rolling`, [server/sessions.md](server/sessions.md#race-start-formation-lap-rolling-start-false-starts)):
  the leader slowing to 80 km/h, the field closing up single file and
  pairing on the run to the line, two by two at the green with no
  contact; with the stand-in in the field, whose driver is held to the
  pace but given no side of the road.
- The AI weaving on a formation lap's straights (`CarState::warm_up`), and
  the tyre read-out warming from garage-cold over the lap.
- A lapped car let by under the safety car going round it on a lane
  beside it, the safety car dropping back as it comes alongside, with no
  overlap between the two bodies.
- The `race_phase` banner and the `flags` component up together (a yellow
  under the formation lap or a safety car): seen apart on 2026-10-20
  (`v-r13-phase`, `v-r13-blue`), never side by side.
- The `penalty` component as a jump start or a pass under a yellow is
  given (flashing for 8 s), then DRIVE-THROUGH with SERVE WITHIN n LAPS
  until it is served; PEN +5S in the standings and the results.
  `v-r13-phase` runs with penalties on, but the stand-in earned none.

### R14. Driver profiles

`n15-profiles` (the Profile screen: the first driver given picture 3, a
second driver made, back to the first) then `n15-who-drives` (the launch
asking "Who is driving?" with two profiles). Both write the save games:
back up `Saved/SaveGames` first and put it back after. The Profile screen
and the launch picker were judged good from the shots on 2026-10-23. Left:

- Pad and keys reach every control (`ApexSim.UI.PadWalk` lists the screen
  but has not walked it yet).
- Stats after an offline race and a hotlap, and two profiles joining one
  server, each under its own id (the leaderboard lights each one's laps).
  The runs' servers bound both profiles' keys (their `records/drivers.json`)
  with no notice; a second machine claiming one of those ids is unseen.

### R15. Offline season

`n7-season` (a three-round season of two-lap races, the stand-in driving,
round 1 at Zandervoort) then `n7-season-standings` (the Season screen on
the profile it wrote). Both write the save games: back them up first. Shot
2026-10-23: round 1 raced and filed, the standings right; the setup and
season cards judged good from the shots the same day. Left:

- A whole season by hand: three rounds, a DNF, End season and the
  summary on the next season's setup.

### R16. Multi-class racing

`n5-multi-class` (four LMP2s and three GT3s with the player's Caravan at
the Norisring, the stand-in driving): the lobby's grid by class, the
standings' class tags and the finish overlay's class places
([sessions.md](server/sessions.md#multi-class-racing)). Shot 2026-10-23:
the LMP2s gridded ahead, "LMP2 P4" / "GT3 P1.." on the standings and the
overlay, the server's results file with `class` and `class_position`,
all judged good. Left:

- The create screen's Other classes block with a server that offers more
  than one class (the rows need a connected lobby; shoot by hand).
- A multi-class race by hand: the blue flag as an LMP2 closes, the AI
  letting it by, the GT3s' race among themselves.

### R17. Over-rev cues

`b16-over-rev` (the Norisring, the rally car, the stand-in driving; the
`gear=1:3` debug event forces a missed downshift to 1st at 100 km/h and
holds it; the engine goes in under a second, so the shots sit on the event): the shift warning banner with the revs past the limit, the rev
counter and gear digit red, the cockpit dash's light bar flashing red
([hud-modding](game/hud-modding.md)). `b16-refused` asks the same of the
Caravan, whose box refuses it: the amber DOWNSHIFT REFUSED banner, the red
gear digit and the dash's amber blink. Shot 2026-10-23: "OVER-REV · SHIFT
UP · 145%" with the gear digit red, the engine zone at 95% and the counter
flashing as the 037 went to 1st at 100 km/h; "DOWNSHIFT REFUSED · TOO FAST
FOR THE GEAR" in amber with the dash bar amber in the Caravan; both judged
good. Left:

- The cues at the wheel with paddles, in a real missed downshift.

### R18. Dawn fog

`s3-dawn-fog` and `s3-dawn-nofog` (Spa, sunny, 05:30; the second with
`apexsim.sky.ServerFog 0`): the height fog thickened to the server's
`SkyNow.visibility_m` (density 3.9 / visibility, never under the weather's
haze; the client logs `Live sky: visibility N m, fog density D`). Run
2026-10-24: the auto race's 7 km/h breeze left the air clear (density
0.0015, the haze), so no fog was on screen, and the second run's GPU hung
(follow-ups #12). Left:

- A still, clear dawn (no wind) that the server fogs, against the same
  pose with the switch off: the fog grey and thick in the distance, the
  near road readable.

### R19. The radio

`n14-radio` (the Norisring, seven AI, three laps, the safety car on, the
stand-in driving; car 2's engine killed at 20 s of green, BoxThisLap at
60 s): read from the client log, not the shots. Every line said is a
`Radio: <id> (<channel>)` line in `client_notes.log`
([game/audio.md](game/audio.md#radio)): side calls from the start
(`car_left` / `car_right` and a `clear_*` after each), `yellow_ahead` or
`double_yellow` and `yellow_clear`, `safety_car`, `sc_in` and
`green_green`, `box_confirmed` after 60 s, `last_lap`, `chequered` and
`position_<n>`; no line twice back to back, no side call left without its
clear. `n14-radio-settings`: the Audio tab's Radio rows (Radio volume,
Spotter, Race engineer) laid out like the rows above them. Run 2026-10-24:
car_left/right each followed by its clear_* (eight pairs), safety_car,
box_confirmed, sc_in, green_green, last_lap, chequered, position_6, no
line twice in a row; no yellow line, as the stopped car went straight to a
safety car. Left: hearing it, and the settings shot.

### R20. Colour-vision palettes

`n22-colour-vision` (the Norisring, the stand-in, five AI cars): the race
HUD in each palette, switched from the console (`apexsim.hud.Palette`),
then the cockpit dash in each ([hud-modding](game/hud-modding.md#colour-vision)).

- The HUD rebuilt whole in each palette, no panel missing or left in the
  old colours; the red-green palette's blue good, vermilion bad, yellow
  accent and pale lilac session best, the blue-yellow palette's pale cold
  tyres.
- The redundant cues on screen: `SB` / `PB` / `+` under the sector splits,
  `-` / `+` / `!` on the temperatures, the radar's bar filling; the car
  panel's four tyre columns still fit with the signs on.
- The dash's light groups in each palette (standard green, amber, red,
  blue; red-green blue, yellow, vermilion, white; blue-yellow green, amber,
  red, white).
- The shots through a colour-blindness simulator (any CVD filter on the
  contact sheets): the states still tell apart.
- Settings > Gameplay > Colour vision changes the HUD while paused.

Shot 2026-10-24: the HUD rebuilt in all three palettes (red-green's blue
bars and yellow accent, the `PB` tags under the splits, the temperature
signs fitting their columns) and the cockpit dash in each. Left: the
shots through a CVD simulator, the radar fill with a car alongside, and
the settings row's BLUE-YELLOW pill width.

### R21. Photo mode

`n20-photo-hotlap` (a watched hotlap at the Norisring, 17:30) and
`n20-photo-replay` (the newest saved replay): photo mode on from the
console, the lens card and chip with the HUD gone and the stream held, the
free camera lifted and tilted, an orbit round the car, 135 mm at f/2 (the
background soft), exposure up and down, a 2x and a 4x shot (the toast; the
PNGs in `game-unreal/Saved/Screenshots/ApexSim` at 3840x2160 and
7680x4320, with no HUD or card in them), the card hidden, and the watch
view back with the replay playing on. Shot 2026-10-24 (`n20-photo-hotlap`):
the card, chip and key strip with the HUD gone, the free camera's lift,
the 135 mm f/2 close-up, +/-1.7 EV and the 3840x2160 toast all as meant;
the first run's orbit kept the TV shot's 80 m and swung into a tree line,
so an orbit now starts at most 12 m out (`MaxOrbitStartCm`). Left: the
replay recipe (`n20-photo-replay`, needs a saved replay), the PNGs opened,
and by hand: the pad's sticks, triggers and D-pad, the mouse's
held-button look and the wheel's zoom.

### R22. Race events in the game and on the dashboard

`r3c-event-lobby` and `r3c-event-refused`: the server starts with an event
kept at its race (`server.event` in runs.yml), as after a restart.

- The session lobby names the event and its phase: "VERIFY CUP · RACE
  NEXT", the format line RACE EVENT with practice and qualifying DONE,
  and WAITING FOR RACE CONTROL TO START THE RACE; the dashboard's start
  goes straight to the race's grid.
- An event with its entries closed refuses the player with the sentence
  "You are not entered in this race event, and its entry list is closed."
  (not "Server error 403").
- The dashboard's Events view (the page on 9403 during either run, token
  `verify`): the event listed with its phase and session, a row unfolding
  its entries and results, OPEN / START / NEXT PHASE / CLOSE / entries /
  DELETE answering, and an event created from the form.

Run 2026-10-24 (`r3c-event-lobby`): the browser lists the session under VERIFY CUP, the lobby reads PRACTICE 5 MIN DONE · QUALIFYING 5 MIN DONE · RACE 3 LAPS and WAITING FOR RACE CONTROL TO START THE RACE, and the dashboard's start took it to the grid. The lobby's top line ran off the left edge at first; its format text now fills what the bar has left and ends in an ellipsis, and the rerun shows VERIFY CUP · RACE NEXT, the circuit and the phases whole.

### R23. The race's incident list in the game

`r3c-incidents`: a short race with the AI and a stand-in driver, then the
results screen.

- The results screen's Incidents: the race's list (contacts, track limits,
  penalties) with race clock, lap, cars and detail, the player's own rows
  in the accent colour, WATCH on each once the session has ended.
- A row's jump (`apexsim.incidents watch 0`): the session left, the
  replay playing from three seconds before that incident.
- The dashboard's copied command with `-ApexWatchReplayAt=<s>` on a
  downloaded server replay: it seeks once loaded (no "nothing is
  playing").

Run 2026-10-24 (`r3c-incidents`): the jump played the client's replay from 0:50 and held it; the list shot catches the finish view ("YOU FINISHED P6"), not the results screen, which opens only once the session reaches Finished; `apexsim.incidents show` then turns a results screen that is not on screen yet. The recipe needs its list shot after the session's Finished state (or the list offered on the finish view too).

### R24. Night cameras at Mandarina Bay

`tv-inside-night`: the TV director round the street circuit at 21:00, a
shot every 7 s, cuts logged with their reason and the lens's position.

- No TV frame inside a hoarding or a building (`FWorldQueries::IsInside`,
  [game/cameras.md](game/cameras.md), TV director):
  no flat white or near-black frame, and how often the log's
  `cut: inside a mesh` comes (a street circuit's helicopter and trackside
  spots among the buildings).
- The cockpit display and mirrors at night dimmed with the exposure
  (`cockpit-screens-night`, [game/cameras.md](game/cameras.md), Cockpit):
  the SF-26's wheel display readable, not a white rectangle, with the
  old look (`apexsim.cockpit.ScreenReferenceEv -10`) beside it; the same
  display by day unchanged.

Run 2026-10-24: no white or black TV frame in 16 shots, three poses refused as inside a mesh (`cut: inside a mesh`), but one shot (10) sat inside a tree's foliage, which the traces do not catch. The cockpit display reads at night (`cockpit-screens-night`, the old look in shot 3 for comparison). The headlights still lay bright pools ahead of the cars from the TV's distance.

### R25. Marshals and recovery

`marshals-recovery`: car 2's engine killed 20 s after the green at
Monzarella, the shot camera parked on the scene (`apexsim.marshals.Look`).

- The marshals at a stopped car ([game/cameras.md](game/cameras.md),
  Marshals and recovery): two kit marshal cards facing the oncoming cars
  (not edge-on or turned away: the card's face is taken as the kit's local
  +Y), their yellow flags swinging, standing on the verge.
- The recovery tractor driving in along the verge with its beacon
  flashing, nose first (the kit vehicle's front taken as local +X), stopping
  behind the car before the tow; the car gone to its box, the flags down,
  the scene gone 5 s later. The TV director's trouble shot on the scene.

Run 2026-10-24 (`marshals-recovery`): two marshals with yellow flags stood at the verge beside the stopped car, facing the traffic; the tractor showed only at the frame's edge and the towed shot looked at a wall (the Look pose after the car left): the tractor's approach is still to be seen.

### R26. Driving school

`n16-school-screen` (the School screen, Snorisring's tests for the
Caravan, its GT3 guide made first if needed) and `n16-school-tests` (the
stand-in driving: the clean lap's test card, the lap judged, the brake test
failed by running through the box, the corner exit judged). The second
writes the profile's medals: back the save games up first. Look at: the
cards over the race view, the marks on the road (the yellow board, the
white stop line, the green gold box, a corner's white gates) lying on the
tarmac where the goal says, and the client log's `School:` lines. Left, by
hand: whether the targets are fair to a human (each test passed and failed
at every medal), the brake test's stop line where it lands in the corner,
and the pad through the screen and the cards.

Run 2026-10-24 (`n16-school-tests`): the clean lap's card, its FAILED (too slow, 1:05.411 against bronze 1:04.881) result card with RETRY / NEXT TEST / BACK TO SCHOOL, and the brake test's card for Grantig-Kehre read well.

### R27. Client-recorded replays in version 2

`n27c-record` (a two-lap race recorded by the client), then
`n27c-replays-list` (the Replays screen) and `n27c-watch`
(`-ApexWatchReplay=latest`), in that order. Look at: the recorded file in
`Saved/Replays/Recent/` reading as version 2 (`apexsim-replay info`) and
about half the size a version 1 recording of the race would be; each
Replays row's size and the total on the line above the list; the replay
playing from the start and after a seek, with no unpack error in
client.log. Left, by hand: Settings > Gameplay > Recent replays at 10
deleting the oldest recent replay as the next is written.

Run 2026-10-24: the client's recording of a 2-lap, 12-car Red Pull Ring race is 955 KB against 2.2 MB for the same race recorded before (43%); the Replays list shows each size and 23 MB in all; it plays from the start and after a seek to a minute in.

## 2. Needs a human: hands, ears and hardware

([game/input-and-ffb.md](game/input-and-ffb.md),
[game/spectator.md](game/spectator.md), [game/audio.md](game/audio.md))
Everything an unattended run cannot check: one sitting with a real
wheelbase, pedals, a shifter, a pad and speakers, in this order so each
step feeds the next. Until it has run, all of this is unverified. The
wheel rows are run as the blocks of [wheel-sitting.md](wheel-sitting.md).

**Client prediction at a ping.** ([game/client.md](game/client.md#client-prediction-raceapexpredictionsubsystemh))
A checkout's editor build with the physics library built
(`play_editor.ps1 -Build`), a local server, Monza, `-ApexPredict
-ApexFakeLatencyMs=80`, a wheel; then the same laps with `apexsim.net.Predict
0`. Seen unattended only with the stand-in driving
(`apexsim.net.PredictServerDriven 1`): a two-lap run drew the car from the
prediction 99% of frames, 57 rewinds a second about 47 ticks deep, the
puppet 6 m (143 ms) behind it; with the replay guessing the stand-in's
inputs (held), corrections were 1.5 mm at the median and up to 0.27 m when
it braked or turned in. A driver's own inputs are what the loop is built
for and only the automation tests have replayed them (0.7 mm at p95).

- Whether the car answers the wheel at once at 80 ms, against the puppet's
  90-150 ms.
- Whether corrections show: a twitch on a brake stab or a kerb, a slide
  that snaps, the cockpit view shaking as the offset blends out (0.1 s).
- Car-to-car contact with an AI field (not predicted: one blended
  correction).
- The switch to and from the puppet at the pit entry and exit and after the
  flag (a 0.15 s blend across a gap of the car's speed times the round
  trip).
- Force feedback, the tyre and engine sound and the HUD's dash follow the
  predicted car while it is drawn (the server's collector run in the
  library). Whether the rim feels right: as heavy as the server's, the
  understeer cue on time, no grain or lumps from rewinds (a correction
  replays ticks already felt; only the ticks the clock reaches are), and
  whether the latency correction (`steer_stiffness`) still helps or now
  overshoots. Whether a kerb or a hit thumps once at the switch to and from
  the puppet (the pit lane), and whether the engine note steps there.
  Unattended (stand-in, 80 ms, a client with no Monza files) the bundle
  came in 0.2 s and the torque and revs were 112 and 108 ms ahead of the
  server's; only hands and ears can say the rest.
- A packaged game (no track files) on a server: the bundle fetched on the
  first join and loaded from the cache on the next.

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
- Overheated tyres cooling (`tyre_thermal::hysteresis_share`): cook a set
  to 140 °C sliding a GT3, then drive gently; the HUD's temperatures back
  in the window within a lap or two and the car feeling normal again.
- The road's crown and millimetre bumps (S1) through the rim: the road
  texture, and the car on the crown down a straight.
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
- The flag cues over an engine (`EApexUiSound::FlagYellow` / `FlagBlue`):
  the yellow fired in `r7-flags-dash` ("Race flags:" in the client log,
  the light bar seen flashing yellow at 27 s), never listened to; the blue
  and the light bar's blue flash in `v-r13-blue`, neither yet seen or
  heard.
- The radio by ear in a race against the AI ([game/audio.md](game/audio.md#radio)):
  whether the two Windows voices read clearly through the radio band
  (`apexsim-content/audio/radio/*.wav`), whether the side calls come early
  enough to be of use (0.15 s hold) and the clears not too late (0.6 s),
  whether the engineer talks too much or too little, and the radio's level
  against the engine at the default 80%.
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
- Garage > Tracks' globe ([game/client.md](game/client.md#tracks-globe-uiapextrackglobepagecpp))
  by hand: a drag and its throw, the wheel's zoom, hovering a pin
  (label, the spin stopping), clicking a pin and a label, + / - / Reset;
  on a pad the left stick's turn and the triggers' zoom (rates set by
  arithmetic, never felt), Y to reset; a wheel's menu buttons on the
  pins. Seen unattended (`m2-tracks-globe`, `r9a-menus`, the pad walk;
  2026-10-21): the globe at home zoom and over Europe, Hockeyheimring
  chosen with its panel, the D-pad hopping between pins, Track guide
  opening the guide, every control of the page reached by pad, and a
  mouse drag (`apexsim.ui.Mouse`) turning and throwing the globe.
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
- An AC import's sun from its `north_yaw_deg` (`ac_import/site.py`):
  re-import Laguna Seca (north by AC's -z convention, 212° in its frame) and
  Silverstone or Spa (measured north), and see the sun's bearing against the
  real circuit's at a known hour. The server side (altitude, latitude) is
  the same keys the shipped circuits use; the client side has only the
  importer test behind it.

**Telemetry output.** ([game/telemetry-output.md](game/telemetry-output.md))

- The `telemetry_out` packet read by an app on real hardware: SimHub (a
  custom UDP plugin or game profile on the documented layout), a motion
  platform's surge, sway and heave from `accel` (sign and scale as felt),
  wheel rev lights from `rev_fraction`. Only the layout, mapping and
  motion tests (`ApexSim.TelemetryOutput.*`) have run.

**Setup sharing.**

- The garage's COPY AS TEXT and PASTE by hand: a line pasted into a chat
  and back, a line for another car refused, the note under the save line.
  Only `ApexSim.SetupShare.*` has run.

**Lap traces.** ([game/client.md](game/client.md#lap-traces-raceapexlaptraceh-uapexlaptracesubsystem))

- A few laps by hand, then the pause menu's LAP TRACES: the last against the
  best, the braking points lining up with the corners, the delta's sign; the
  CSVs in `Saved/Telemetry`. Only `ApexSim.LapTrace.*` has run.

**Render scale and HDR.** ([game/client.md](game/client.md#graphics-quality-uapexsettingssubsystemapplygraphics))

- The Graphics tab's Render scale at 50, 70 and 100 in a race: the frame
  rate against the sharpness, the HUD sharp at every scale, the cockpit
  mirrors and a triple's side views. HDR switched on with an HDR display
  (the row shows on this machine, which reports HDR support), and its
  paper white moved from 80 to 500 nits: the scene and the menus brighten
  together. Only the settings round trip has run.

**Chat.** ([server/protocol.md](server/protocol.md))

- Two clients chatting in a session lobby and in a race: the lines both
  ways, the chat line opened with T and closed with Enter and Escape
  without losing pad steering on the viewport, the rate limit's 429 toast,
  a mute from the dashboard. Seen on 2026-10-17 (`n9-chat`, one client,
  lines said with `apexsim.net.Chat`): the lobby's chat under the grid, the
  HUD's chat panel and its T CHAT hint, the line opened with T over the
  race and Escape closing it alone. Typing into it is left for a hand on
  the keyboard (`apexsim.ui.Key` sends no characters).

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
- MoTeC i2 opening a log from `scripts/motec_export.py` (two laps: the
  channels, their units and the laps split at the `.ldx` beacons). The
  format is written as the open readers parse it, and one of them
  (gotzl's `ldparser`) reads it back; i2 itself has not opened one.
- Windows on Arm (Snapdragon X), parked: the release is x64 only. On an
  Asus Zenbook A14 under x64 emulation DirectX 12 showed only the sky and
  DirectX 11 (`display.graphics_api: dx11`) drew about one frame a second,
  then hung. No log came off the machine.
