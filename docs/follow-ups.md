# Follow-ups

What is left on features that are built: bugs, edge cases, rough edges,
verification nobody has run yet, and the cleanup a feature left behind.
Features not started yet are in the [roadmap](roadmap.md); simulation and
racing-rule gaps are in [simulation-gaps.md](simulation-gaps.md). An item
lives in exactly one of the three.

Items are grouped into **packages** the same way as the roadmap: one
package is one go (same code, one check, one commit), it says what it is
for, what is in it and how it is checked, and it takes the priority of its
most urgent item. When an item is done, delete its bullet; when a package
has none left, delete the package. Git history keeps what was done.

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is wrong.
- **P2**: depth and polish.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

### 1. Race-day fixes

A small bug a driver meets in an ordinary race.

- **(bug)** At 1600x900 with the LMP2 (Fugazzi) the `pit_stop` component's
  panel sits on top of the centre speed and gear readout; with the F1 car's
  layout it clears it.

**Check:** a pit stop in the LMP2 at 1600x900 (`-ApexScreenshotAfter`).

## P2: depth

### 2. Look at what is built: screen pass

Features that are built but have never been looked at in the running game.
One unattended screenshot batch (`-ApexAutoRace`, `-ApexExecAfter`,
`-ApexScreenshotAfter`, `-ApexCameraLookAt`) covers all of them, and what
it finds feeds the look work after it (weather, tyres, damage, dressing,
here and in the roadmap). Do it before them.

- Road state: a green start, a drying line after rain, puddles, marbles and
  debris (only a rubbered start has been seen: Silverstone,
  `-ApexTrackRubber=100`, the band about 9% darker). The colour and gloss
  figures in `ApplyRoadStateGraph` are first guesses.
- The rain light (`apexsim.car.RainLightNits`) in the wet; a body with no
  `car_rainlight` slot has none.
- Snow: ice and a melt.
- The tyre look against a real wet and a flat-spotted slick.
- A stadium grandstand wedge on a corner.
- Mandarina Bay, which has never been seen in the running game.
- A curved pit box row tighter than about a 130 m radius (the garages scale
  down to 0.85 and may still overlap a little).
- HUD layouts: the layout manager, per-circuit and per-class bindings and
  switching by place, by hand.
- A dashboard broadcast (`ServerNotice`) in the menus and over the race
  HUD, and a session the dashboard ends while driving (notice, then back to
  the menu).

**Check:** the batch's screenshots, each item judged and either deleted or
turned into a bullet in the package that owns it.

### 3. Road state and snow on screen

Rough edges of the client's road state and snow.

- Road state per bin off the racing surface: the pit lane, curbs and
  run-off are wet by the lap's one figure (their UV `u` is not the lap's
  station). A puddle covers its cell's width (puddles have no lateral
  position on the server).
- Snowflakes are lit grey-blue cubes; they become white sprites (against
  the cloud sheet they read as dark specks).
- The cloud dome's figures are first guesses from two Silverstone shots: the
  sheet's brightness (`ApexSky::CloudLook`, 4500 cd/m² x sun), its colour, the
  noise scales and the 0.8 cover from which it is whole. Scattered cloud
  (cloudy 0.5) has not been looked at; nor the dome at night or in the
  menu's backdrop.
- Spray (`UpdateSpray`) was judged from one chase shot at Silverstone in heavy
  rain: a pale haze behind the car. Its rate, size and opacity are by eye.
- Snow on the props was seen on one tree and the pit buildings at a distance;
  the `SnowFloor` 0.4 on tree cards and the faces-up blend on walls, roofs and
  barriers have not been looked at close up, nor an AC-imported circuit's
  props.

**Check:** the screen pass's weather shots again, with a rain-to-dry and a
snow session at Silverstone.

### 4. Tyre state per tyre

Built tyre state the client draws or prints from telemetry.

- More than one flat spot per tyre, each where its lock-up happened (a
  second lock-up deepens the first today).
- Never seen in the running game: a puncture's FLAT and a slow leak's LEAK
  read-out (`item.flat` / `item.leak`, checked only through the golden
  frame), pickup, blisters and the heat tint on the tread, the sidewall
  bulge of a deflated tyre, and a car that lists its own compounds on the
  garage's reference card (the default list's card was seen).
- Pickup, blistering and the heat tint are integrated on the client from
  the marbles and tread temperature the server sends
  (`AApexRaceCarActor::UpdateTyreSurface`); their rates are guesses, and a
  spectator joining late starts them from zero.

**Check:** golden bytes on both sides; two lock-ups on one tyre on screen;
a puncture, a leak and a long stint's tyres in a chase shot.

### 5. Damage

The car parents, the damage actor and the importer's damage tables.

- Two hits on one face are two dents (today a zone's dent is centred on the
  damage-weighted mean of its hits).
- Engine smoke from the engine, not always the tail.
- The DRS flap and the driver can be dented; wheels are drawn toed.
- The AC car importer writes `[[damage_part]]` tables, so AC imports shed
  parts.
- Never seen in the running game: a part hanging before it comes off
  (`UpdateHangingParts`: the hinge edge, the 22° droop and the flutter are
  guesses), the glass cracks (`kGlassCrack`: the web's spokes, rings and
  reach, and whether it lands on the windscreen rather than a side window on
  a given model) and a debris strike's dent. Four unattended runs (snow and
  heavy rain, chase and watch cameras) left the player's car unhit or exited
  silently (the GPU-hang item in package 13).

**Check:** a seeded pileup replay screenshotted before and after; one AC
import re-run and crashed.

### 6. Hotlap and lap records

The ghost, the hotlap replay and watch.

- The ghost is see-through, not opaque and tinted (`AApexGhostCarActor`
  tints its own instances).
- The hotlap replay's trackside shots come from the TV director's cameras,
  not `CutReplayShot`'s geometric placement.
- Hotlap watch: the remembered ladder level is kept across server restarts,
  is keyed by hour, wind and air as well, and climbs back.
- The `hotlap_watch` HUD scene is in the HUD editor.
- Corners carry the circuit's official turn number in the dossier, not the
  detector's.
- Never heard: a replay's tyre and kerb sound (the watched car's road sound
  from its row's wheels, `UpdateWatchedCarRoadSound`) against the live run,
  and a hotlap replay's ghost from its `Ghost` record. A client-recorded
  replay carries the own car's wheels only; the other cars are silent in it,
  where a server-rendered stream carries every car's.

**Check:** a hotlap with ghost and replay; a server restart between two
watches.

### 7. Circuit dressing

Prop-kit and `ats-dress` work on circuits and pieces that are built. Laid by
the same `ApexProps` code.

- Mandarina Bay: corner names (the dossier has one "corner", the circuit
  itself), the F1 pit building (`building_pit_street`) placed, a quay rail
  along the water, a catch fence pass.
- Stadium grandstands: a scene names the stadium bays and cap that
  `LayoutGrandstand` lays; `grandstand/stair_tower` is built.
- **(trademark)** `sign/hillside_letters` spells "RED BULL RING"; it gets a
  display-name text before any scene uses it.
- Curved pit box rows: whatever the screen pass finds at tight bends.
- Kerb markers, sausage kerbs and the pit podium (2026-10-14) were seen
  only at a distance in a Montreal chase shot. The sausage rule (inside
  curb of a bend under 80 m radius, curbs of 6 to 60 m) lays 660 pieces on
  the Nordschleife and 240 at Marina Bay, where the AI survey moved a
  little (more off-road and sliding, no retirements); look at both by hand
  and tighten the rule if they catch the AI.

**Check:** each circuit by hand in the running game; the track-editor tests.

### 8. HUD editor on a pad

- The layout manager opens on more than L, and Reset all has a pad button.
- The HUD editor is in the pad walk (`ApexSim.UI.PadWalk`; its pad map is
  its own).

**Check:** the pad walk, then the same screens with a real pad.

### 9. Wheels, pads and rigs

The input path's loose ends, then one session with real wheels, a shifter,
pads and screens.

- `ApexFfb::MixGamepad` handles the GameInput motor layout (the plugin puts
  the small channels on the trigger motors).
- On hardware: a clutch pedal on the wheel tab's `Clutch` slot (sent as
  `PlayerInput.clutch`; whether its throw and the server's torque cut feel
  right); the PXN profiles (V10, V12, V12 Lite, V99: names and peaks from the
  maker's listings, no vendor id, so they match by name alone). Gomez Sim
  Industries and Heusinkveld make rims and pedals, not bases, so they get
  no profile. An H-pattern shifter on the `HGear*` slots; the handbrake's
  feel (`HANDBRAKE_SHARE`, how fast the tail comes round); the
  `ApexWheelProfiles` name rules against what real bases report; the peak
  torques and the gear / hybrid minimum forces (5% / 3%) felt on a base.

**Check:** the hardware session; until it has run, these stay unverified.

## P3: nice to have

### 10. Car content

Car data that is built but that the client and the shipped cars do not use.

- The livery `preview` image (parsed onto the row) is drawn in car select.
- Shipped car.toml files set their tyre pressure keys and their own
  `[[tires.compound]]` / `[brakes]` tables (all run on defaults today).

**Check:** server physics and class calibration tests (a car.toml edit moves
them, and changes its content checksum); car select with a livery preview.

### 11. Server dashboard

- Restarting a hosted session seats its players in the new one, instead of
  sending them back to the menu to join again (the client has never been
  seen taking a `SessionJoined` it did not ask for).

**Check:** a player stays in the car through a dashboard RESTART.

### 12. Content tooling

- Before the next release: re-render the showcases and track guides of the
  circuits the 2026-10-13 geometry pass changed (every circuit but Moscow
  Raceway, Shanghai and Yas Marina); they were built from the old geometry.
  **(bug)** Until then the backdrop skips them all and restarts on the three
  left, and in an unattended run (`-ApexWatch`, `-ApexWatchSession`) the
  game exits silently a few seconds after "Demo view ended", with nothing in
  the log and no crash report; a race with `-ApexNoDemo` runs. Seen on
  2026-10-14 with and without the weather-look changes.
- `ats-export --keep-sidecars` help text lists `pit` (it accepts it).
- `dem_elevation.py` handles a circuit that crosses itself (Suzuka: the DEM
  sees only the bridge, so the fit closes the crossover), and Shanghai's and
  Monza's surface models are checked against a bare-earth one (grandstands,
  the park's canopy); all three still drive their invented z.
- `initialize_content.ps1` checks `<Stem>.pit.msgpack` with the other
  sidecars.
- The launcher's AC import works offline (it fetches its Python packages
  from PyPI on first use); its import button has not been clicked by hand.

**Check:** the track-editor tests; a fresh-clone `initialize_content.ps1`;
the launcher import with the network off.

### 13. Rendering and platforms

- Increase GPU stability. Two races have stopped with
  `DXGI_ERROR_DEVICE_HUNG` about 10 s after the track was shown (Silverstone
  and Suzuka, `-ApexAutoRace`, 5 AI, TV camera); reruns ran, and four
  100 s Silverstone runs (2026-10-12) ran clean. Each run logs the VSM
  "Non-Nanite Marking Job Queue overflow" message once
  ([proposals/nanite-shadows.md](proposals/nanite-shadows.md)); whether it
  plays a part is not known. A long unattended loop with GPU crash dumps on
  is the next try.
- Runtime-built track meshes get mesh distance fields, or an A/B against an
  `-ImportLevels` level shows they do not need them (software Lumen and DF
  shadows see the road and terrain less well today).
- Integrated GPUs: the LOW preset and the integrated-GPU first-run default
  are seen only on an RTX 3080 at LOW; no Intel Iris Xe / Arc or AMD Radeon
  680M / 780M has run the game, so the frame rate there and whether
  `GRHIDeviceIsIntegrated` picks LOW are unknown. Until one has, the
  requirements say a discrete GPU is recommended.
- Windows on Arm (Snapdragon X), parked: the release is x64 only. On an Asus
  Zenbook A14 under x64 emulation DirectX 12 showed only the sky and
  DirectX 11 (`display.graphics_api: dx11`) drew about one frame a second,
  then hung. No log came off the machine.

**Check:** needs the hardware; the A/B is two screenshots.

### 14. Cleanup

Small and independent. Do one or two alongside whatever package touches
the same files.

- Unused: `WBP_SessionBrowser`, `WBP_SessionRow` and `UApexSessionRowWidget`
  (the browser is C++; `WBP_Root` still references the first); `procgen/`
  (`--generate-terrain`); `track_mesh.rs` (used only by
  `examples/track_export.rs`); `scripts/content/cars/build_yotota.py`
  (superseded by `build_lmp2.py`).
- `L_Menu` still names `/Game/Cars/RB20/SM_RB20` as `DefaultCarMesh`.
- `ApexRaceDirector.cpp` still matches `MI_wear_*` road bands that no
  exporter writes.
- Wire: `SessionKind::Sandbox` has no behaviour and no client sends it;
  `SessionKind::Demo` is kept only for old servers and can go once
  showcases are everywhere.

**Check:** server and client build and tests.
