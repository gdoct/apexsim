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
- A part hanging before it comes off (`UpdateHangingParts`: the hinge edge,
  the 22° droop): it droops fore and aft, so from the side.

### R4. Snow at Spa, changeable, into the evening

`-ApexWeather=snow -ApexChangeable=1 -ApexTimeScale=30` from 15:00. No
AC-imported circuit is installed; Spa stands in.

- Snow: ice (once the snowfall stops and the line freezes) and a melt (may
  never come at winter temperatures; if it does not show, it is a
  test-only item and moves to [follow-ups.md](follow-ups.md)).
- Snow on the props: the faces-up blend on walls, roofs and barriers close
  up, from a camera looking down on them (tree cards have been seen), and
  an AC-imported circuit's props. The first R4 race ended within five
  minutes with the whole field stuck off the road, before the ice.

### R5. Mandarina Bay at night, cloudy

`-ApexTrack=MarinaBay -ApexTimeOfDay=21:00 -ApexWeather=cloudy`, a TV lap
plus `LookAt` stops ([content/props.md](content/props.md)).

- The cloud dome at night, from open ground (the pit building's ceiling
  filled the first shot), and in the menu's backdrop.
- The pit podium.
- Whether the sausage kerbs catch the AI: they lay 240 pieces here and 660
  on the Nordschleife, where the AI survey moved a little (more off-road
  and sliding, no retirements). Seen on screen inside the tight bends.

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

### R8. Stadium stands and pit rows

`r8-ims` and `r8-mexicocity` aim at their `grandstand_corner`; the first
pass put the camera at road level and in a chase view: aim again, from
higher and further off. Run the R8 sweep with GPU crash dumps on: it is
also the long unattended loop of the GPU stability item (section 3).

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
- An empty grid slot ([server/sessions.md](server/sessions.md#starting-order-and-qualifying)):
  a Custom start order naming a driver who is not there, then the session
  lobby's GRID list, the grid card's list and the lined-up field with the
  gap kept. Only `grid_order` and `game_session` unit tests have run.

### R10. Spa, sunny, 13:00

`-ApexTrack=Spa -ApexWeather=sunny -ApexTimeOfDay=13:00`, a TV lap.

- The sun at the circuit's own noon (`SessionJoined.Conditions.solar_noon_min`,
  13:33 at Spa): at 13:00 it stands a little east of due south over the
  circuit.

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
- The peak torques and the gear / hybrid minimum forces (5% / 3%) felt on
  a base.
- The PXN profiles (V10, V12, V12 Lite, V99: names and peaks from the
  maker's listings, no vendor id, so they match by name alone), and the
  `ApexWheelProfiles` name rules against what real bases report.
- The sausage kerbs by hand on both circuits, and the Nordschleife's posts
  and grade plates read from the cockpit.

**Listen.** Start with the replay of the lap just driven.

- A replay's tyre and kerb sound (the watched car's road sound from its
  row's wheels, `UpdateWatchedCarRoadSound`) against the live run.
- A hotlap replay's ghost from its `Ghost` record.
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

**Last, with the network off.**

- The launcher's AC import button, never clicked by hand, and the import
  with the network off (it fetches its Python packages from PyPI on first
  use; the release's `Tools/importer/wheels` venv was checked only as the
  same `pip --no-index` command against downloaded wheels).

## 3. Platforms and GPUs

([building.md](building.md)) Each needs the machine.

- GPU stability: two races stopped with `DXGI_ERROR_DEVICE_HUNG` about
  10 s after the track was shown (Silverstone and Suzuka, `-ApexAutoRace`,
  5 AI, TV camera); reruns ran, and four 100 s Silverstone runs
  (2026-10-12) ran clean. Each run logs the VSM "Non-Nanite Marking Job
  Queue overflow" message once
  ([proposals/nanite-shadows.md](proposals/nanite-shadows.md)); whether it
  plays a part is not known. Next: a long unattended loop with GPU crash
  dumps on (R8 is one).
- Integrated GPUs: the LOW preset and the integrated-GPU first-run default
  are seen only on an RTX 3080 at LOW. No Intel Iris Xe / Arc or AMD
  Radeon 680M / 780M has run the game, so the frame rate there and whether
  `GRHIDeviceIsIntegrated` picks LOW are unknown. Until one has, the
  requirements say a discrete GPU is recommended.
- Windows on Arm (Snapdragon X), parked: the release is x64 only. On an
  Asus Zenbook A14 under x64 emulation DirectX 12 showed only the sky and
  DirectX 11 (`display.graphics_api: dx11`) drew about one frame a second,
  then hung. No log came off the machine.
