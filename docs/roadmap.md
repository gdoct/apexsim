# Roadmap

The product backlog: bugs, features, content and tooling not yet done.
Simulation and racing-rule gaps (physics, tyres, AI, pit and race rules,
race weekends) are in [simulation-gaps.md](simulation-gaps.md); an item
lives in exactly one of the two.

The backlog is a list of **work packages**. A package is one go: the items
in it touch the same code, the same wire change or the same content
refresh, so they are built on one branch, verified with one check and
committed together. Each package says what it is for, what is in it, and
how it is checked. A package is done when its items are built and its
check has been run. When an item is built, delete its bullet; when a
package has none left, delete the package. Git history keeps what was done.

Ideas that have no scope or check yet are not packages; they wait under
[Not yet scoped](#not-yet-scoped) until someone writes them one.

Priorities (a package takes the priority of its most urgent item), and
within a priority the packages are in the order to do them:

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is missing or wrong.
- **P2**: depth and polish that keeps players coming back.
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
it finds feeds the look packages after it (weather, tyres, damage, dressing).
Do it before them.

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

### 3. Weather look

What a wet or snowy race looks like on the client.

- Road state per bin off the racing surface: the pit lane, curbs and
  run-off are wet by the lap's one figure (their UV `u` is not the lap's
  station). A puddle covers its cell's width (puddles have no lateral
  position on the server).
- Snow on props: buildings, grandstands and trees get a top-down snow blend
  in the prop materials.
- Snow sky: a cloud layer (a snow day is a deep blue gradient lit as
  overcast today); flakes as white sprites, not lit grey-blue cubes; spray
  behind the cars.
- Forecast: the create screen shows the forecast (it is worked out from the
  session's id, which does not exist yet there); a radar or longer outlook
  beyond the `conditions` HUD component's next three changes.

**Check:** the screen pass's weather shots again, with a rain-to-dry and a
snow session at Silverstone.

### 4. Tyres and brakes on screen

The tyre material and the tyre read-outs share per-tyre state from
telemetry: send it per tyre once (a positional telemetry append), then draw
and print it.

- Per-tyre water (today one figure per car, read under its middle).
- More than one flat spot per tyre, each where its lock-up happened (a
  second lock-up deepens the first today).
- Tread detail: sidewall bulge in the normal, pickup and marbles on the
  tread, blistering, heat colour.
- Puncture read-out: today a puncture shows only as FLAT under the tyre (red
  under 60 kPa) and in the wear, and a slow leak only as falling pressure.
- The garage's stock compound card says which wet tyre it fits in the rain.

**Check:** golden bytes on both sides; a puncture, a slow leak and a lock-up
on screen; the wet-tyre card in the garage.

### 5. Damage

The car parents, the damage actor and the importer's damage tables.

- Two hits on one face are two dents (today a zone's dent is centred on the
  damage-weighted mean of its hits).
- Debris the cars collide with, not only drive through.
- Engine smoke from the engine, not always the tail.
- Windscreen cracks and hanging parts.
- The DRS flap and the driver can be dented; wheels are drawn toed.
- The AC car importer writes `[[damage_part]]` tables, so AC imports shed
  parts.

**Check:** a seeded pileup replay screenshotted before and after; one AC
import re-run and crashed.

### 6. Hotlap and lap records

A single driver's laps: the ghost, the hotlap replay and watch, and the
in-session board. The watch's seek, ghost and replay reuse the stream v2
controls ([game/spectator.md](game/spectator.md)).

- The ghost is see-through, not opaque and tinted (`AApexGhostCarActor`
  tints its own instances).
- The hotlap replay's trackside shots come from the TV director's cameras,
  not `CutReplayShot`'s geometric placement.
- A multiplayer hotlap has an in-session board across drivers (the garage
  scoreboard is shown in qualifying only).
- Hotlap watch: the remembered ladder level is kept across server restarts,
  is keyed by hour, wind and air as well, and climbs back.
- Hotlap watch: picks beyond stock setup, tyre and sky (wind, air
  temperature, rubber, a saved setup); pause, seek and speed; a ghost and a
  replay.
- The `hotlap_watch` HUD scene is in the HUD editor.
- Corners carry the circuit's official turn number in the dossier, not the
  detector's.

**Check:** a hotlap with ghost and replay; two clients in one hotlap
session; a server restart between two watches.

### 7. Replays carry everything

A stream's rows and `State` records leave out the ghost and the driver
feedback, so a replay is quieter and emptier than the race.

- Streams carry the ghost and the driver feedback (slip, slide and contact
  surface), so a replay has tyre and kerb sound.
- The promo pipeline records audio.

**Check:** a replay of a race and a hotlap side by side with the live run,
by ear; a promo clip with sound. (Frames stay whole: no delta encoding, see
[game/spectator.md](game/spectator.md).)

### 8. Circuit dressing

Prop-kit and `ats-dress` work, after the geometry pass. Laid by the same
`ApexProps` code.

- Mandarina Bay: corner names (the dossier has one "corner", the circuit
  itself), the F1 pit building (`building_pit_street`) placed, a quay rail
  along the water, a catch fence pass.
- Stadium grandstands: a scene names the stadium bays and cap that
  `LayoutGrandstand` lays; `grandstand/stair_tower` is built.
- Kit pieces with no pass that lays them: kerb markers, sausage kerbs, the
  podium, the hillside letters.
- **(trademark)** `sign/hillside_letters` spells "RED BULL RING"; it gets a
  display-name text before any scene uses it.
- Curved pit box rows: whatever the screen pass finds at tight bends.

**Check:** each circuit by hand in the running game; the track-editor tests.

### 9. Nordschleife dressing

One circuit, a long list; its own package so it does not hold up the rest.

- Numbered marshal posts, the track-over-road bridges (B257 at Breidscheid),
  rock cuttings, spectator hedges and banks, camp villages, the timing
  gantry as a crossing, more German signs and the T13 gates, the Hohe Acht
  tower, vertical graffiti on barriers, skid-mark decals.

**Check:** a lap by hand and a TV-camera lap in the running game.

### 10. Menus and HUD layouts

Shell screens and the HUD editor. All of it goes through one pad walk
(`ApexSim.UI.PadWalk`) at the end.

- Session browser: recorded sessions and filters.
- Replay export.
- A car stats view (the row opens car select today).
- A direct jump into a track guide (it opens the track picker today).
- A HUD layout picker outside the HUD editor.
- The layout manager opens on more than L, and Reset all has a pad button.
- The HUD editor is in the pad walk (its pad map is its own).

**Check:** the pad walk, then the same screens with a real pad.

### 11. Wheels, pads and rigs

The code that is left in the input path, then one session with real wheels,
a shifter, pads and screens.

- A clutch pedal is sent.
- More wheelbase makers in `ApexWheelProfiles` (PXN, Gomez, Heusinkveld
  direct drives...); they mix as Generic today.
- `ApexFfb::MixGamepad` handles the GameInput motor layout (the plugin puts
  the small channels on the trigger motors).
- On hardware: an H-pattern shifter on the `HGear*` slots; the handbrake's
  feel (`HANDBRAKE_SHARE`, how fast the tail comes round); the
  `ApexWheelProfiles` name rules against what real bases report; the peak
  torques and the gear / hybrid minimum forces (5% / 3%) felt on a base.

**Check:** the hardware session; until it has run, these stay unverified.

## P3: nice to have

### 12. Car content

Car data that the client and the shipped cars do not use yet.

- The livery `preview` image (parsed onto the row) is drawn in car select.
- The GT3 and LMP2 class wheels get sidewall lettering.
- Shipped car.toml files set their tyre pressure keys and their own
  `[[tires.compound]]` / `[brakes]` tables (all run on defaults today).
- The HUD colours brakes by material as well as temperature (the client
  learns carbon from steel from the `[brakes]` table).

**Check:** server physics and class calibration tests (a car.toml edit moves
them, and changes its content checksum); the HUD on a carbon and a steel car.

### 13. Server dashboard

For people running a server for others
([server/operations.md](server/operations.md)).

- Restarting a hosted session seats its players in the new one, instead of
  sending them back to the menu to join again (the client has never been
  seen taking a `SessionJoined` it did not ask for).
- Restarting the server process itself from the dashboard, for the config
  keys that need it.

**Check:** a player stays in the car through a dashboard RESTART; a saved
tick rate in force without touching the machine.

### 14. Content tooling

- `ats-dress --explain` and a per-prop source stamp.
- `ats-export --strict`, which fails on a dropped facet.
- An aerial-imagery tracing aid for dossiers.
- Before the next release: re-render the showcases and track guides of the
  circuits the 2026-10-13 geometry pass changed (every circuit but Moscow
  Raceway, Shanghai and Yas Marina); they were built from the old geometry.
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

### 15. Rendering and platforms

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

### 16. Cleanup

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

## Not yet scoped

Ideas without a scope or a check. Each needs a short design (what it is,
what it touches, how it is checked) before it becomes a package.

- **Driver rating / safety rating.** No definition yet of what is rated or
  where it is stored.
- **Shared leaderboards across servers.** Lap records and the track
  leaderboard screen are per server; sharing needs a service and a trust
  model.
- **UDP encryption.** Inbound datagrams are sealed (HMAC under a
  per-connection key from `AuthSuccess`, rising sequence), so forged,
  tampered or replayed input is dropped; only reading telemetry and input
  off the wire is left open, and it carries nothing the other players do
  not already get. TCP has TLS. Likely not worth DTLS.
- **More screens.** A five-wide row (`MaxSplitscreenPlayers` would need 5),
  a second monitor for telemetry, shared exposure across the triple-screen
  views.
