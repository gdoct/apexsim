# Follow-ups

What is left on features that are built: bugs, edge cases, rough edges and
the cleanup a feature left behind. What is built but has not been seen,
heard or felt yet is in [verification.md](verification.md); features not
started are in the [roadmap](roadmap.md); simulation and racing-rule gaps
are in [simulation-gaps.md](simulation-gaps.md). An item lives in exactly
one of the four.

Items are grouped into **packages** the same way as the roadmap: one
package is one go (same code, one check, one commit), it says what it is
for, what is in it and how it is checked, and it takes the priority of its
most urgent item. When an item is done, delete its bullet; when a package
has none left, delete the package. Git history keeps what was done.

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is wrong.
- **P2**: depth and polish.
- **P3**: nice to have.
- **P4**: unrefined.
- **P5**: cleanup.

## P0: playable end to end

None open.

## P1: core racing experience

### 18. Flags on the track and in the car

Race control's yellows and blues ([server/sessions.md](server/sessions.md#flags))
reach the HUD (`flags` component, `flag.*`); the rest of the car and the
circuit do not show them yet.

- The trackside light panels (`light_panel`, `ApexEmissive_*`): each
  panel's lap station, its sector's yellow flashing on it. The runtime
  builder shares one glow instance per slot round the circuit (one write
  switches every panel, as the pit exit light uses it), so a panel needs a
  dynamic instance of its own.
- An audible cue as a yellow or the blue comes up (`game/audio.md`'s
  synthesised UI cues), and the flag on the cockpit dash.

**Check:** a seeded pile-up (R7) and a lapping race on screen, the panels
seen flashing at the incident's sector.

## P2: depth

### 5. Hotlap and lap records

The ghost, the hotlap replay and watch.

- Corners carry the circuit's official turn number in the dossier, not the
  detector's.

**Check:** a hotlap with ghost and replay; a server restart between two
watches.

### 6. Circuit dressing

Prop-kit and `ats-dress` work on circuits and pieces that are built. Laid by
the same `ApexProps` code.

- Mandarina Bay: corner names (the dossier has one "corner", the circuit
  itself), the F1 pit building (`building_pit_street`) placed, a quay rail
  along the water, a catch fence pass.
- Stadium grandstands: a scene names the stadium bays and cap that
  `LayoutGrandstand` lays; `grandstand/stair_tower` is built.
- Nordschleife: rock cuttings. OSM maps no cliff, bare rock or cutting
  along the lap, and the DEM is a surface model that reads the forest's
  canopy as a slope, so no rule can tell a cutting from a tree line; it
  needs a bare-earth terrain model (Rhineland-Palatinate's DGM1) or a
  surveyed list of the cuttings, then a rock-face kit piece.
- Nordschleife: the lap's bridges over the K 73, the B 257 (Quiddelbach,
  Breidscheid), the L 92 and the L 93 are parapets only
  (`track_bridge_10m`). The ground under the lap is not cut away, so the
  road beneath is not there: it needs an `Underpass` for a dossier road in
  `terrain.rs` (a slot under the deck, as at Suzuka), the road drawn in it,
  and the AI survey.
- Nordschleife: the marshal posts are numbered in lap order from the
  line, not with the real post numbers (OSM maps one, "Posten 44", by the
  GP circuit); a real list goes in a dossier table.
- Nordschleife: the toll lanes stand on the grass: the entrance's access
  lanes and car park are not drawn (no dossier road is baked).
- Camp sites off the fan lap pitch their tents in a row: the walk in
  `dress::surroundings::camp_sites` uses one irrational step for both
  coordinates (0.618 x 1.618 is 1). The fan lap uses the R2 sequence
  (`r2_point`); switching every circuit moves their tents, which are walls
  (`attraction`), so it goes with a re-dress and an AI survey of those
  circuits.
- Nordschleife: the OSM extract the dossier was fitted from is gone from
  `.cache/osm/`, so the surveyed layers cannot be refitted; a fresh
  extract (the map API tiles in `.cache/osm/nordschleife_tiles/` are
  newer than the cut) would change the woods and barriers, and so the
  walls: a refit is a groom and an AI survey of its own.
- The kit's venue blocks (`media_centre`, the street circuits'
  `skyline_*`) read as untextured white boxes from the road: plain
  `pit_wall` / facade colours with no texture (Spa's Stand Endurance, the
  Norisring's towers in the R11 run of 2026-10-17). Facade textures are
  art.

**Check:** each circuit by hand in the running game; the track-editor tests.

### 19. AI in imported cars

An imported field finishes its survey races (none retired in 72, three
seeds, eight circuits), but its F1s and prototypes still leave the road
more than the shipped classes: off-road car-seconds over the 24 races
`RssFormulaHybrid2021` 313, `KsFerrariSf70h` 215, `KsPorsche919Hybrid2016`
196 against `fugazzi-sf26`'s 86, a fifth to a quarter of it at Laguna Seca.
A lone ace holds its plan on every circuit tried, so what is left is in
traffic.

- Find where an imported F1's traffic leaves the road (`SURVEY_EVENTS`,
  `SURVEY_TRACE` on the worst races): a lane or dodge its short lock
  (0.24-0.31 rad against the shipped F1's 0.34) cannot turn.

**Check:** `SURVEY_CARS=RssFormulaHybrid2021,KsFerrariSf70h,KsPorsche919Hybrid2016,fugazzi-sf26`
on seeds 1-3, totals against the commit before.

### 20. Race control on screen and its edges

The race-start rules and penalties ([server/sessions.md](server/sessions.md#race-start-formation-lap-rolling-start-false-starts),
[penalties](server/sessions.md#penalties))
are on the wire and run on the server; the client does not offer or show
them yet, and the server leaves a few edges.

- client: the create screen's race rules (`CreateSession.race_rules`:
  Start Standing / Rolling, Formation lap, False starts), kept in the
  profile, named in the summary; `SessionJoined.RaceRules` kept on the net
  subsystem.
- client: show the formation lap and the pace lap (`CompactTelemetry.race_phase`
  1 / 2, in `Countdown` with no countdown): a FORMATION LAP banner and HUD
  data point (`race.phase`), no lights until the countdown comes back.
- client: the create screen's Safety car row (`race_rules.safety_car`:
  Off / Virtual / On the road); show the virtual safety car (`race_phase` 3
  VSC, 4 VSC ENDING) and the safety car (5 SAFETY CAR, 6 SAFETY CAR IN THIS
  LAP) as a banner, a HUD data point and on the dash, with the pace.
- client: draw the safety car (`FApexTelemetryFrame::SafetyCar`, decoded
  and not drawn): a car model with roof light bars, lit until `race_phase`
  6, interpolated like a car; it drives through a lapped car passing it
  (it is no body on the server), so the pass is seen as an overlap.
- client: the create screen's penalty rules (`race_rules.yellow_penalty`,
  `pit_exit_penalty`; False starts is `jump_start`) and its Reverse grid
  row (`race_rules.reverse_grid`, Off / 6 / 8 / 10 / whole field).
- Balance of performance (`[[balance]]`, server only): the client's
  predicted car (`apexsim.net.Predict`) steps its own car.toml without the
  ballast, power and downforce, so a balanced car's prediction errs until
  the snapshot corrects it; the session browser does not say a server
  balances its cars.
- client: show penalties (`RosterEntry.Penalties`): a toast as one is given
  ("DRIVE-THROUGH: JUMP START", "+5 S: OVERTAKING UNDER YELLOW"), the owed
  one and its laps left on the HUD (`penalty.*` data points), PEN with the
  seconds in the standings and tower, and the results' times with them
  (the server's classification already adds them).
- The time penalty for a pass under a yellow is given even when the place
  is handed back at once.
- The formation lap holds the grid's tyre warmth but puts none in (a
  steady 30 m/s): weaving and the warm-up accelerations a driver does would,
  and then `FORMATION_LAP_WARMTH` could go.
- A rolling start runs single file; real ones line up two by two.
- A car let go of on the grid (`jump_start`) is not collided with the held
  cars; a hosted session (`HostedSpec`) and the lobby listing do not carry
  the race rules; the spectator stream has no `race_phase`.

**Check:** a formation lap and a rolling start on screen with a human in
the field, a jump start and a red-light exit served; `race_start_test`,
`penalty_test`.

## P3: nice to have

### 11. Content tooling

- The dressing's `surroundings` stamp is one for the whole layer (car
  parks, camps, village, marshal posts, corner signs, forest, floodlights,
  fan dressing): `--explain` tells them apart by asset only. Stamping each
  sub-layer in `surroundings::lay` would name the dossier area each came
  from.
- `trace_layout.py` skips Marina Bay: no DEM sidecar, so no georeference
  (`dem_fetch.py MarinaBay`, or a georeference from the dossier's own fit
  without a DEM).
- **(bug, not reproduced)** On 2026-10-14, with most showcases stale, an
  unattended run (`-ApexWatch`, `-ApexWatchSession`) exited silently a few
  seconds after "Demo view ended", with nothing in the log and no crash
  report. On 2026-10-15 a `-ApexWatch` run against a server holding only
  stale showcases skipped them, played a local file, restarted on another
  track and kept running; with fresh showcases a 7-minute run was clean.
  If it comes back, the run's `-abslog` and the Windows event log are the
  places to look.
- `dem_elevation.py` handles a circuit that crosses itself (Suzuka: the DEM
  sees only the bridge, so the fit closes the crossover), and Shanghai's and
  Monza's surface models are checked against a bare-earth one (grandstands,
  the park's canopy); all three still drive their invented z.
- Verification runs (`scripts/verify`): the tyre close-ups (two flat spots,
  pickup, blisters, a deflated sidewall) can now be framed on a held
  replay (`apexsim.replay.Seek` / `Hold`), but no recipe does it yet: it
  needs a race's replay saved by one run and played by the next. The
  stand-in, an AI, also pits for any leak and at a 0.6 flat spot, wear 70
  or damage 25 (`pit::plan_stop`), so the moment has to come within that lap.
- Imported pit lanes (`ac_import/pit.py`): Poznan's `drift` layout gets no
  pit lane: none of its 32 `AC_PIT` markers is within 20 m of its pit
  line's speed-limit stretch (the other four Poznan layouts, and Spa, Red
  Bull Ring, Laguna Seca, Zandvoort, Silverstone, Barcelona and Imola, pass
  both `PIT_CUSTOM=1` pit surveys).

**Check:** the track-editor tests; a fresh-clone `initialize_content.ps1`;
the launcher import with the network off.

### 12. Rendering

- Runtime-built track meshes have no mesh distance fields, so software
  Lumen and DF shadows see the road and terrain less well than the kit
  props. Neither build path gives them any: on 2026-10-15
  `ShowFlag.VisualizeMeshDistanceFields 1` at Norisring showed the road and
  terrain missing after the fast build and after a full
  `BuildFromMeshDescriptions` alike (in `-game` from the editor; a cooked
  game cannot build them at all), while an `-ImportLevels` import of the
  same circuit built them. What is left is the A/B: the race in an imported
  level against the runtime one, which needs the race director to load
  `/Game/Tracks/<Track>/L_<Track>` instead of building (the menu game mode
  takes over a level opened with `-game`). If it shows they matter, the
  ways out are shipping baked levels or hardware ray-traced Lumen.

- Puddles cover most of a low cell's width: since 2026-10-17 a puddle is
  drawn from the water over the cell's flat share and reads as standing
  water (`out/verify/g1-puddles-*`), and at Eau Rouge (low 1.0) and the
  0.71 cell at 6175 m it spans nearly the whole road in heavy and light
  rain alike. That is the server's model: 1.5-3 m half-wide plus 3 m for a
  full dip (`RoadState::bin_depth`) on a 12 m road. Judge the sheets; if
  it is too much, narrow the half-widths in `road_state.rs` (a sim change:
  AI survey).
- The chase-near camera behind the Bugotti Chiffon showed a dark blur
  filling the frame once (snow run, 2026-10-15): the camera inside a body.
- The bounds watch (`apexsim.debug.BoundsWatch`) reports the rain cloud
  dome (`ApexCloudLayerActor.Dome`, a 20 km radius on purpose) as out of
  the world on every rain or cloud start. It should skip primitives that
  cast no shadow: only casters reach the VSM culling it is there for.

**Check:** the A/B is two screenshots.

## P4: Unrefined

### 13. Windshield wipers
- Add windshield wiper functionality to respond to rain and other weather conditions. This requires model updates, a key binding, and appropriate animation plus sound in first person view.

### 14. Improve sound system

- Enhance the sound system for better spatial audio, realistic engine sounds, and environmental effects. This may involve updating audio assets, adjusting sound cues, and integrating with the physics engine for dynamic audio responses.
- Improve sliding sound effects for better realism, including tire squeal and surface interaction audio.


### Top view
- If we use the top view with formula cars, somehow the image looks really cheap and lacks the visual fidelity seen in other camera angles. This needs to be further investigated and fixed.

### Pit lane entry and exit 
- Review and improve the visual and functional aspects of pit lane entry and exit, ensuring smooth transitions and accurate representation in the game. 
- The pit lane entries should be closer to the actual pit boxes and clearly marked by a line.
- The pit lane exits should be immediately after the last pit box. That should be where the light is located. From that point, the player can already get control there and have no speed limit.


## P5: Cleanup

### 99. Cleanup

Small and independent. Do one or two alongside whatever package touches
the same files.

- Wire: `SessionKind::Demo` is kept for servers without showcases and can
  go once showcases are everywhere. The backdrop still falls back to it
  when a server has none rendered or all of them stale (a fresh clone
  before `initialize_content.ps1`, or right after a car.toml or track
  edit), and `HotlapWatch` shares its create path.
- Tests: CI's server job races a fresh checkout without the gitignored
  track sidecars (curbs, walls, ground, pit lane, road mesh), so an AI
  test at Monza or Le Mans drives a different road there than on a baked
  tree: curbs the line uses are off the track, and a seeded run spreads
  differently. Three tests have been narrowed to what they guard for it
  (the Le Mans seam, the qualifying outlap, the racecraft pass).
  `penalty_test` asserts the Norisring's pit-lane sidecar and fails
  outright in such a checkout (it passes with the sidecars). Either
  CI bakes the sidecars it needs, or a test that drives a circuit says
  which backend it expects and skips without it. Check a test change
  against `git archive HEAD server content` in a scratch folder, with its
  own `CARGO_TARGET_DIR`.

- Client prediction's physics library (`server/ffi`) links the whole
  server crate (tokio, rustls, hyper...): 80 MB of `.lib`, of which the
  linker keeps what the C API reaches. A physics-only crate would shrink
  it and the client's link, but the car step is not separable today:
  `data.rs` reaches the AI, race control, the safety car and racecraft,
  `physics.rs` reaches `game_session` and `network`, and the library calls
  `game_session::session_track`. Split those edges first (the session
  types out of `data.rs`, the bake out of `game_session`), then move the
  car step's modules to a crate the server re-exports; the server's tests,
  `determinism_test` and the AI survey must stay identical.
- Client prediction's bundle cache (`Saved/ApexSim/TrackBundles`) keeps
  every bundle it was handed, one per track version (2.6 MB for Monza,
  10 MB for the Nordschleife): prune the oldest past a size.

- Tick pacing: the game loop's tokio interval ticks on a 1 ms grid, so
  at 420 Hz single ticks run 1-4.5 ms apart round the 2.38 ms budget
  (verification, platforms). If a client shows it, pace the loop on a
  high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`)
  or sleep short and spin the last fraction; measure with
  `[debug] tick_jitter_seconds`.

**Check:** server and client build and tests.
