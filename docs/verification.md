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
When a group is empty, delete it. The groups are in the order to run them:
the screen pass first, since what it finds feeds the look work after it.

## 1. Screen pass: an unattended screenshot batch

One batch (`-ApexAutoRace`, `-ApexExecAfter`, `-ApexScreenshotAfter`,
`-ApexCameraLookAt`, [game/client.md](game/client.md)) covers all of these.
Each shot is judged against what the feature's doc says it draws.

**Road state and weather** ([server/conditions.md](server/conditions.md),
[game/client.md](game/client.md)):

- A green start, a drying line after rain, puddles, marbles and debris.
  Only a rubbered start has been seen (Silverstone, `-ApexTrackRubber=100`,
  the band about 9% darker); the colour and gloss figures in
  `ApplyRoadStateGraph` are first guesses.
- The rain light (`apexsim.car.RainLightNits`) in the wet; a body with no
  `car_rainlight` slot has none.
- Snow: ice and a melt. Snow on the props was seen on one tree and the pit
  buildings at a distance: the `SnowFloor` 0.4 on tree cards and the
  faces-up blend on walls, roofs and barriers close up, and an AC-imported
  circuit's props.
- The cloud dome: its figures are first guesses from two Silverstone shots
  (`ApexSky::CloudLook`, 4500 cd/m² x sun, its colour, the noise scales and
  the 0.8 cover from which it is whole). Scattered cloud (cloudy 0.5), the
  dome at night and in the menu's backdrop.
- Spray (`UpdateSpray`) was judged from one chase shot at Silverstone in
  heavy rain: its rate, size and opacity from other cameras and lighter
  rain.

**Tyres** ([server/vehicle-physics.md](server/vehicle-physics.md)):

- A puncture's FLAT and a slow leak's LEAK read-out (`item.flat` /
  `item.leak`, checked only through the golden frame).
- Pickup, blisters and the heat tint on the tread, the sidewall bulge of a
  deflated tyre, a real wet and a flat-spotted slick, in a chase shot
  after a long stint.
- A car that lists its own compounds on the garage's reference card (the
  default list's card was seen).

**Damage** ([server/vehicle-physics.md](server/vehicle-physics.md)): a
seeded pileup replay screenshotted before and after.

- A part hanging before it comes off (`UpdateHangingParts`: the hinge edge,
  the 22° droop and the flutter are guesses).
- The glass cracks (`kGlassCrack`: the web's spokes, rings and reach, and
  whether it lands on the windscreen rather than a side window on a given
  model).
- A debris strike's dent.
- Four unattended runs (snow and heavy rain, chase and watch cameras) left
  the player's car unhit or exited silently (the silent exit is in
  [follow-ups.md](follow-ups.md), content tooling).

**Circuits and dressing** ([content/props.md](content/props.md)):

- Mandarina Bay, never seen in the running game.
- A stadium grandstand wedge on a corner.
- A curved pit box row tighter than about a 130 m radius (the garages
  scale down to 0.85 and may still overlap a little).
- Kerb markers, sausage kerbs and the pit podium, seen only at a distance
  in a Montreal chase shot. The sausage rule (inside curb of a bend under
  80 m radius, curbs of 6 to 60 m) lays 660 pieces on the Nordschleife and
  240 at Marina Bay, where the AI survey moved a little (more off-road and
  sliding, no retirements): a lap of each by hand, and whether they catch
  the AI.
- The track geometry pass of 2026-10-13 (real elevation, banking with
  ramps, the Karussell in concrete, paved aprons as run-off) on each
  circuit it changed: every circuit but Moscow Raceway, Shanghai, Monza and
  Yas Marina.
- The Nordschleife dressing ([content/circuits.md](content/circuits.md)),
  never imported or seen: the numbered posts and the grade plates read
  from the road; the rail tags wrap the W-beams on both sides of the road
  (not flipped off the rail, not floating); the skid marks show through
  `M_ApexDecal`'s mask; the track bridges' parapets stand behind the rail
  and the overpasses' abutments clear it at the 9 m road's scale; the
  gantry, the toll lanes and the camp villages; the spectator banks on a
  sloping verge; the lookout tower on the Hohe Acht seen from the Hohe
  Acht section; the hillside letters seen from the Döttinger Höhe (or
  hidden by the trees between).

**Menus, HUD and dashboard** ([game/client.md](game/client.md),
[game/hud-modding.md](game/hud-modding.md),
[server/operations.md](server/operations.md)):

- HUD layouts: the layout manager, per-circuit and per-class bindings and
  switching by place, by hand.
- A dashboard broadcast (`ServerNotice`) in the menus and over the race
  HUD, and a session the dashboard ends while driving (notice, then back
  to the menu).
- The launcher's AC import button, never clicked by hand; the import with
  the network off (it fetches its Python packages from PyPI on first use).
- A streamed hotlap watch on a saved setup (S with setups saved for the
  car; only the stock lap and the "no saved setup" toast were run), and its
  keys and transport from a pad. Seen unattended at Monza: pause, ten
  seconds back, 2x back to live, the ghost of the first lap and its
  replay.

## 2. Listen: replays against the live run

([game/spectator.md](game/spectator.md), [game/audio.md](game/audio.md))

- A replay's tyre and kerb sound (the watched car's road sound from its
  row's wheels, `UpdateWatchedCarRoadSound`) against the live run.
- A hotlap replay's ghost from its `Ghost` record.
- A promo clip's sound by ear ([marketing-site.md](marketing-site.md)):
  measured on one 5 s onboard cut at Spa (the note follows the car's rpm
  within two frames, 5.00 s of sound for 5.00 s of frames), never listened
  to; a slowed shot's lowered pitch; the clip and music levels of a whole
  cut (`clip_volume` 0.7).

## 3. Hardware session: wheels, pedals, shifter

([game/input-and-ffb.md](game/input-and-ffb.md)) One session with a real
wheelbase, pedals, a shifter and a pad. Until it has run, all of this is
unverified.

- A clutch pedal on the wheel tab's `Clutch` slot (sent as
  `PlayerInput.clutch`): whether its throw and the server's torque cut feel
  right.
- The PXN profiles (V10, V12, V12 Lite, V99: names and peaks from the
  maker's listings, no vendor id, so they match by name alone), and the
  `ApexWheelProfiles` name rules against what real bases report.
- The peak torques and the gear / hybrid minimum forces (5% / 3%) felt on
  a base.
- An H-pattern shifter on the `HGear*` slots.
- The handbrake's feel (`HANDBRAKE_SHARE`, how fast the tail comes round).

## 4. Platforms and GPUs

([building.md](building.md)) Each needs the machine.

- GPU stability: two races stopped with `DXGI_ERROR_DEVICE_HUNG` about
  10 s after the track was shown (Silverstone and Suzuka, `-ApexAutoRace`,
  5 AI, TV camera); reruns ran, and four 100 s Silverstone runs
  (2026-10-12) ran clean. Each run logs the VSM "Non-Nanite Marking Job
  Queue overflow" message once
  ([proposals/nanite-shadows.md](proposals/nanite-shadows.md)); whether it
  plays a part is not known. Next: a long unattended loop with GPU crash
  dumps on.
- Integrated GPUs: the LOW preset and the integrated-GPU first-run default
  are seen only on an RTX 3080 at LOW. No Intel Iris Xe / Arc or AMD
  Radeon 680M / 780M has run the game, so the frame rate there and whether
  `GRHIDeviceIsIntegrated` picks LOW are unknown. Until one has, the
  requirements say a discrete GPU is recommended.
- Windows on Arm (Snapdragon X), parked: the release is x64 only. On an
  Asus Zenbook A14 under x64 emulation DirectX 12 showed only the sky and
  DirectX 11 (`display.graphics_api: dx11`) drew about one frame a second,
  then hung. No log came off the machine.
