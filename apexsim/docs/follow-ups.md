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

### 31. Wheel forces at a standstill

Found at the wheel sitting of 2026-10-23 (ClubSport V2.5, base FF 100%,
game force 0.50, damping 0.25; paddle shifters, no H-shifter). Run files
are local, not in git, under `out/sit/2026-10-23/`.

- Standing still, the rim starts shaking violently. Block A, Caravan GT3
  at Monzarella, after a crash at the first chicane: the server recording
  (`a-monza-1/replays/.recording_3139369d-*.blocks`, car 10) has the car
  standing off the road at 952 m for 46 s, front 57 %, engine 20 %, every
  wheel's slip 0. Ruled out: the server's signals at a standstill are
  clean (a car at rest sends a steady jacking torque, -0.023 at 0.1 input,
  no lock-up or wheelspin), the torque is held down by the stiffness limit,
  the rim correction is off under 3 m/s, and the engine shake at 20 % is
  0.02. Left: the DirectInput damper, 0.3 of the base at a standstill (a
  belt base can buzz under a strong damper with nothing loading the rim),
  and the centring spring re-armed each time the feedback goes stale and
  comes back. The `Wheel forces over 5 s standing:` log line (constant,
  rim speed, the loudest vibration and its Hz, damper, centring share,
  arrivals) tells them apart: one stop at the wheel, then read it.
  Built blind on 2026-10-23 against the second suspect: the centring no
  longer re-arms on a stale frame or two (only after the feedback has been
  gone 1 s, `CentreRearmAfterSeconds`) and gives up on a rim that does not
  move under it for 0.4 s (`CentreGiveUpSeconds`: hands). If the rim still
  shakes, the damper is what is left.

**Check:** sit on the grid and in the pit box with hands off the rim for
30 s: the rim stays still or settles, no growing shake (the `Wheel forces
over 5 s standing:` log line says which force it is).

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
- The Nurburgring's routes are three circuits (`Nuerburgerring`,
  `Mordschleife`, `Nuerburgerring24` built by `combine_tracks.py`), not one
  track with selectable routes: the track format, the session and the lap
  timing know one lap per track. The Nordschleife alone from the GP pits
  (the VLN/NLS layout without the GP loop) is not built; it is a
  `COMBINED` entry with the GP's pit straight and the two connectors.
- `extract()` in `tools/osm_layout.py` takes a stand only from its
  grandstand tags: an outline tagged only `leisure=bleachers`, carrying
  just a name ("BMW Tribüne") or tagged `building=yes` (Silverstone's
  Stowe, Vale and Club stands, Le Mans's tribunes) comes out as a building
  or not at all, and a wrapping outline (Hockenheim's Südtribüne) as one
  block on its centroid's side. Every known case is entered by hand in
  `MANUAL_STANDS`; a rule here only matters on the next full extract, and
  only two circuits have one cached.
- Stadium grandstands: a scene names the stadium bays and cap that
  `LayoutGrandstand` lays; `grandstand/stair_tower` is built.
- Nordschleife: rock cuttings. OSM maps no cliff, bare rock or cutting
  along the lap, and the DEM is a surface model that reads the forest's
  canopy as a slope, so no rule can tell a cutting from a tree line; it
  needs a bare-earth terrain model (Rhineland-Palatinate's DGM1) or a
  surveyed list of the cuttings, then a rock-face kit piece.
- Nordschleife: the marshal posts are numbered in lap order from the
  line, not with the real post numbers (OSM maps one, "Posten 44", by the
  GP circuit); a real list goes in a dossier table.
- Nordschleife: the toll lanes stand on the grass: the entrance's access
  lanes and car park are not drawn (no dossier road is baked).
- Nordschleife: the OSM extract the dossier was fitted from is gone from
  `apexsim-content/.cache/osm/`, so the surveyed layers cannot be refitted; a fresh
  extract (the map API tiles in `apexsim-content/.cache/osm/nordschleife_tiles/` are
  newer than the cut) would change the woods and barriers, and so the
  walls: a refit is a groom and an AI survey of its own.
- The kit's venue blocks read flat from the road: the paddock blocks
  (`media_centre`, `hospitality_3f`, `control_tower`, `clubhouse`) have
  slab bands, fins and frames since `build_paddock.py`, but their
  `pit_wall` is still a plain colour, and the street circuits' `skyline_*`
  are untextured facades (Spa's Stand Endurance, the Norisring's towers
  in the R11 run of 2026-10-17). A baked `pit_wall` changes every GLB of
  the building kind that carries the slot at once.
- The vehicle kind's shared slots disagree between GLBs: `vehicle_tyre`,
  `bridge_dark` and `pit_glass` are baked in some and flat in others, and
  whichever GLB the import meets first sets them for all
  (`vehicle_paint_camper` was split off `vehicle_paint_a` for this). One
  definition per slot, re-exported through `build_vehicles.py`.
- Circuits without a layout dossier (Endianapolis, HermanosRodriQueso,
  MoscowMule, YasMarinara) are not dressed: no marshal posts, so no light
  panels either, and race control's yellows show on the HUD alone there.
  Removing them from the calendar is the owner's option
  ([track-improvements.md](../../apexsim-content/docs/track-improvements.md#status)).
- Found by the dressing audit, outside dressing:
  - AlbertParkour's centerline is the pre-2021 layout (the Lakeside
    chicane at 2520-2670 m), and Barcelunatic's takes the RACC chicane
    (4100-4210 m) and the tight pre-2021 T10; both are geometry, and the
    stands placed from 2026 maps are counted against them.
  - Bahrainless' line sits at the upstream end of the main grandstand;
    the FIA map puts the control line about 40% of the way from T15 to T1.
  - Nuerburgerring's start lights stand at the finish line; the start
    line is 263 m further on.
  - Hockeyheimring's `metadata.longitude_deg` (8.48963) is about 5 km
    west of the venue (8.566): the sun and weather are placed there.
  - Gnocchi's dossier fit covers 35% of the centerline (OSM now maps only
    the short layout), under the 90% gate.
  - `maps/RedPullRing/redbullring-lageplan-2021.jpg` is a saved HTML
    error page, not the map: its `sources.json` URL needs replacing.

**Check:** each circuit by hand in the running game; the track-editor tests.

### 19. AI in imported cars

Since the traffic dodge is measured across the road (ai.md, Traffic) the
imported prototypes leave the road no more than the shipped classes: on
every circuit, seeds 1-3, 90 races each, off-road car-seconds
`AcFriendsFerrari499pLmh` 93.7 and `KsPorsche919Hybrid2016` 160.3 against
`fugazzi-sf26` 124.7 and `yotota-lmp2` 113.0, two thirds of it on the
Mordschleife and Laguna Seca, where the shipped cars go off as much
(contact 131.8 and 137.4 against 97.0 and 121.4). The imported F1s leave the road no more either (2026-10-24, every circuit,
seeds 1-3, 90 races each: off-road `RssFormulaHybrid2021` 102.3,
`KsFerrariSf70h` 67.8 against `fugazzi-sf26` 79.0, Laguna Seca and the
Mordschleife again the most), but they touch other cars more than twice
as much: contact 193.8 and 209.5 against 88.2 (the 919 137.9).

- Bring the imported F1s' contact to the shipped F1's. What it is
  (2026-10-25, `SURVEY_DBG` on eight circuits, AlbertParkour to
  Monzarella in the survey's order but the quiet ones, seeds 1-3, 24
  races a car): all of it is car against car, beside each other in the
  `Race` tactic (the traffic dodge, not a racecraft move), 3-6 m apart
  along the road. The field is no more bunched (within 10 m of a car
  6239 car-s against 5269), but beside one another the imports are closer
  across (within 2.3 m 7.9% of the time against 3.9%) and touch 4.5 times
  as often for the time beside (1.58 car-s of contact per 100 against
  0.35); the contacts start all race long, not on lap one. A trace
  (`SURVEY_TRACE`, AlbertParkour seed 1, car 8 at 33-36 s) shows the
  kind: a car run wide out of a corner at full lock swings back to its
  line across the car beside it, overshooting the dodge's room (2.95 m)
  to 2.1 m. It is the car's handling in the AI's hands, spread over the
  `[physics]` table: the shipped F1 with the RSS import's whole
  `[physics]` table touches 73.6 car-s (its own 15.6, the import 85.1),
  with only its mass, size, wheelbase, tracks, wheel radius, centre of
  gravity and weight split 52.2, and with any one of those 18-28 (the
  centre of gravity, 0.27 m against the shipped F1's default 0.45, and
  the weight split, 0.455 against 0.52, the most). Not the cause, each
  tried alone: ABS and the brakes, the steering lock, the tyres, the
  aero and grip, the engine and turbo lag, the hybrid, the suspension,
  dirty air (switched off, the imports still touched 10 times the
  shipped F1), the lock-up mistake on a car without ABS. Tried and kept
  out: keeping the dodge's room plus the drift across the road over
  0.5 s (or 1 s): imported contact 140 -> 97 (86) on the eight
  circuits, but the shipped classes over the whole calendar, seeds 1-3,
  contact 181 -> 250 and off-road 288 -> 415 (passes 1659 -> 2050, 163
  races in common). The next try is the steering's recovery to the line
  (the Stanley and yaw-damping gains are constants tuned on the shipped
  cars), judged by `SURVEY_DBG`'s tight-beside share.
- The 918 Spyder import (`ks_porsche_918_spyder`, a 1709 kg road car)
  fails `car_stability_test::imported_cars_survive_a_full_lock_flick_with_the_steering_aid`
  (sliding 9 m/s after the flick at 80 m/s); an import from before the
  speed map and the second motor fails both stability tests, so it is the
  car's own figures, not those keys.
- The SF70H import (`ks_ferrari_sf70h`) fails the same flick test once its
  ERS delivery profile is carried (`[[hybrid.delivery_point]]`): sliding
  from 76 m/s it deploys 0.2-0.4 of its motor (the profile's 270 km/h
  share and its fourth-gear share) where it had all of it, and spins
  (sliding 37 m/s at the end); with the profile left out it passes. With any
  one of its throttle, speed or gear maps left out it still spins, so it is
  the drive the car is left with mid-slide, not one map. The 2021 RSS
  hybrid fails the test with or without its profile.

**Check:** `SURVEY_CARS=RssFormulaHybrid2021,KsFerrariSf70h,KsPorsche919Hybrid2016,fugazzi-sf26`
on seeds 1-3, totals against the commit before.

### 20. Race control on screen and its edges

The race-start rules and penalties ([server/sessions.md](server/sessions.md#race-start-formation-lap-rolling-start-false-starts),
[penalties](server/sessions.md#penalties))
are on the wire and run on the server, and the create screen offers them;
the client does not show all of them yet, and the server leaves a few edges.

- The AI lifts only for a confirmed yellow (`INCIDENT_CONFIRM_S`, 1.5 s),
  not for trouble it could see sooner. When the confirmation came in, the
  survey's off-road time rose 485 -> 654 car-seconds; on 2026-10-23 at
  2523c4a5 it is 275 (seed 1) and 294 (seed 2), so that loss is gone. Two
  tries at an AI-only early lift did not pay: 2026-10-20 (a lift for a car
  within 150 m) and 2026-10-23 (a second, unconfirmed flag board, the
  same sector logic from the first tick of trouble, read only by the AI's
  pace and passing; patch kept outside the repo): contact -10% on both
  seeds, but off +3% and +31%, the Nordschleife +50 car-seconds on each.
  Blind crests make a lift for a car out of sight cost more than it saves;
  a third try would start from line of sight on the Nordschleife
  (`SURVEY_TRACKS=Mordschleife SURVEY_EVENTS=1`).

**Check:** a formation lap and a rolling start on screen with a human in
the field, a jump start and a red-light exit served; `race_start_test`,
`penalty_test`.

## P3: nice to have

### 11. Content tooling

- **(bug, not reproduced)** On 2026-10-14, with most showcases stale, an
  unattended run (`-ApexWatch`, `-ApexWatchSession`) exited silently a few
  seconds after "Demo view ended", with nothing in the log and no crash
  report. On 2026-10-15 a `-ApexWatch` run against a server holding only
  stale showcases skipped them, played a local file, restarted on another
  track and kept running; with fresh showcases a 7-minute run was clean.
  If it comes back, the run's `-abslog` and the Windows event log are the
  places to look.
- Loose packs (`.acp`): the launcher lists what one installed but has no
  Remove for a custom car, track or HUD component (the folders are deleted
  by hand), and nothing records which pack an item came from. A HUD pack's
  `layout.json` is not an item, so it is ignored. The installer reads a
  pack whole into memory before it writes, about 0.9 GB for a release's
  `content.acp`.
- Shanghai's and Monza's surface models are checked against a bare-earth
  one (grandstands, the park's canopy); both still drive their invented z.
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

- Circuit locations (`metadata.latitude_deg` / `longitude_deg`, from the
  DEM's georeference by `track_location.py`): Zandervoort's sits about
  100 km west of Zandvoort (52.33 N, 3.10 E, in the North Sea) and
  Barcelunatic's about 25 km north-east of Montmeló; the rest are within
  about 6 km. The sun is placed from them too. Their pins on Garage >
  Tracks' globe show the error from about 3x zoom.

**Check:** the track-editor tests; a fresh-clone `initialize_content.ps1`;
the launcher import with the network off.

### 12. Rendering

- Runtime-built track meshes stay out of the distance-field scene, so
  software Lumen and DF shadows see the road and terrain less well than the
  kit props. The fast build makes no distance field. `-ApexTrackFullBuild`
  (editor builds only) builds every track mesh with the editor's full build
  and waits for it: at Spa all 419 got a distance field (the log line
  `Track full build: <mesh>, distance field yes, radius N m`; road and
  ground chunks are under 300 m, only the horizon tiles pass the engine's
  1 km `r.DistanceFields.MaxObjectBoundingRadius`). Yet
  `ShowFlag.VisualizeMeshDistanceFields` a minute into the race still shows
  no road or terrain, and the lit shots at Eau Rouge and the grid are alike
  with and without it (`g4-df-fast` / `g4-df-full`, 2026-10-24). The road
  and ground are open single-sided sheets, whose distance field is close to
  empty, so shipping baked levels would not change it either. What is left
  to try: the terrain as a heightfield Lumen takes in (the engine's
  `SupportsHeightfieldRepresentation` path, which landscapes use), or
  hardware ray-traced Lumen, which needs no distance field (a project
  setting and a long shader rebuild); judge either against `g4-df-fast`'s
  sheet.

- Puddles cover most of a low cell's width: since 2026-10-17 a puddle is
  drawn from the water over the cell's flat share and reads as standing
  water (`out/verify/g1-puddles-*`), and at Eau Rouge (low 1.0) and the
  0.71 cell at 6175 m it spans nearly the whole road in heavy and light
  rain alike. That is the server's model: 1.5-3 m half-wide plus 3 m for a
  full dip (`RoadState::bin_depth`) on a 12 m road. Judge the sheets; if
  it is too much, narrow the half-widths in `road_state.rs` (a sim change:
  AI survey).
- A D3D12 GPU crash (`Error_DMA_PageFault`, Aftermath "MMU Fault") in the
  menu backdrop at Albert Park, 2026-10-20, in the same second as the
  game's own showcase render finished on its thread (`Showcase render:
  ... done`) and the local files were scanned again. Not seen in the next
  two runs, one with the render and one without; the render thread touches
  no GPU state. Watch for it in a soak with `-gpucrashdebugging`.
  Seen again 2026-10-23 in a Development release package
  (`artifacts/release-n26`, D3D12 device removed, the active breadcrumb
  `Nanite::DrawGeometry` MainPass) 7 s after Snorisring was built for an
  auto race; the same run again and a Zandervoort run were clean. The
  Aftermath dump is `Game/ApexSim/Saved/Logs/D3D12.0.2026.10.23-09.26.51.nv-gpudmp`
  in that package.
  Seen a third time 2026-10-23 at the wheel sitting, in the editor build
  (`UnrealEditor.exe -game`, block A: auto race at Monzarella), 15 s after
  launch at frame 110 while the race loaded: a compute shader
  (`compute_02`) MMU fault at VA `0xFFFFFFFFFFFFF000`, then an assert
  `Distance >= 0` in `D3D12Adapter.cpp`; last breadcrumb `EndDrawingViewport`,
  frame 108. Run without `-gpucrashdebugging`, so no dump; client log in
  `out/sit/2026-10-23/a-monza-2/` (local). The run before it, the same
  block, was clean.

**Check:** the A/B is two screenshots.

### 32. The sky's environment on the client

- Fog is drawn live (`FApexSkyNow::VisibilityM`, `ApexSky::FogDensityFor`
  in `DeriveLive`) but not in a recorded stream: carry the visibility in
  the spectator stream's state record (`StreamState::encode` and
  `ApexSpectatorWriter` write nineteen fields; both decoders already skip
  extra ones), with the golden `State` and `File` blobs moved together.
  The damp dawn road (`RoadState::dew`) is drawn already: it is the road
  state's water.
- The run-off's and the pit lane's glaze (`Cell::verge_ice`,
  `RoadState::lane_ice`) is not on the wire: the client draws a frozen lane
  as packed snow, and a predicting client (`apx_track_road_rows`) grips it
  as unfrozen until the server corrects it. Send it with `SnowRows` /
  `LaneSnow` (a byte more a cell) when the client draws ice off the line.

**Check:** `-ApexAutoRace -ApexTimeOfDay=04:30
-ApexWeather=sunny` with a calm wind shows a mist that lifts after sunrise
at `-ApexTimeScale=30`.

### 35. Rewind in offline practice

- The ring is large on a long circuit. Measured 2026-10-25
  (`determinism_test::snapshot_memory`, ignored: a counting allocator in
  the test binary, one human and the 15 AI practice seats after 60 s): one
  snapshot is 4.15 MB at the Nordschleife (the ring of 61, 253 MB) and
  1.16 MB at Monza (71 MB). The road state (`TrackConfig::road_state`, its
  per-cell rubber, water, marbles and snow) is 1.97 MB and 0.55 MB of it,
  about 95 bytes a metre; each car adds about 140 KB. The AI's speed and
  power plans are per car config and small beside them; the ghost traces
  are the humans' alone. The road state changes all over every few ticks
  (cars lay rubber cell by cell, the road steps), so sharing it between
  snapshots saves little. Choices: a byte budget for the ring (sparser
  snapshots further back on a long circuit), or the road state kept at a
  coarser interval than the cars and the restored one stepped forward
  (determinism must still hold).

**Check:** `snapshot_memory` against the figures above, and the
determinism test.

### 36. Recorded sessions on the client

For: the client holding the smaller version 2 stream
([game/spectator.md](game/spectator.md), the file and Replays).

- A live stream's timeshift is kept inflated up to `LiveBufferCapBytes`
  (256 MB, about two hours of 16 cars); held as column blocks it would hold
  ten times as long, at the cost of unpacking a block on a seek. Not a
  small change: `UApexSpectatorSubsystem` keeps `LiveRecords` /
  `LiveTicks` record by record for its cursor, the cap's trim and every
  seek; held packed, each of those works on blocks (pack the tail as it
  ages past ten seconds, unpack on a seek back).
- The lossy cuts of [proposals/replay-size.md](proposals/replay-size.md)
  phase 3 (wheel slip at a lower rate or coarser, cm poses, a second-order
  delta), each measured, the slip judged by the road sound by ear.

**Check:** `ApexSim.Spectator.*` green; a server replay downloaded from the
Recorded list, a showcase rendered in the game and a client-recorded replay
all play.

### 37. Own liveries beyond the race

([cars.md](../../apexsim-content/docs/cars.md#liveries)) Another
player's own livery is painted on their car in a session only.

- A spectator stream, a showcase and a replay carry the roster's livery
  index, so a car in its driver's own livery shows the works there. The
  stream's roster row would need the hash and the replay the package.
- A race event refuses an own livery (`Error 403`, as `SetLivery`): its
  entrants race in the car's liveries.

**Check:** a live spectator and a saved replay of a session with an own
livery show it.

## P4: Unrefined

### 13. Windshield wipers
- Add windshield wiper functionality to respond to rain and other weather conditions. This requires model updates, a key binding, and appropriate animation plus sound in first person view.

### 14. Improve sound system

- Enhance the sound system for better spatial audio, realistic engine sounds, and environmental effects. This may involve updating audio assets, adjusting sound cues, and integrating with the physics engine for dynamic audio responses.
- Improve sliding sound effects for better realism, including tire squeal and surface interaction audio.


### Top view
- If we use the top view with formula cars, somehow the image looks really cheap and lacks the visual fidelity seen in other camera angles. This needs to be further investigated and fixed.
- Looked at 2026-10-24 (`g6-top-view`: chase far, helicopter, tracking at
  Monzarella in the SF-26; `g6-grid-above`: the grid still from 35 and 15 m
  up, virtual shadow maps against cascades, contact shadows on): the cars
  are one runtime-built LOD (no LOD switch to blame), their materials hold
  up from above, and shadows work the same with either shadow method. At
  13:00 the sun is near overhead, so a formula car's shadow lies under its
  own floor and from above the car sits on the road with no dark edge: the
  likeliest "pasted on" look. Nothing was clearly broken in the shots;
  needs the person who saw it to name the view (a shot or a MARK), then
  try a contact shadow or AO term under the car (a floor-shadow decal or
  `bCastContactShadow` with a longer `r.ContactShadows` length).

## P5: Cleanup

### 99. Cleanup

Small and independent. Do one or two alongside whatever package touches
the same files.

- Server tests: `protocol_test::test_session_ticks_at_the_configured_rate`
  measures the tick rate and misses it in a debug build when the whole
  suite runs at once (it passes alone, and at opt-level 2 as CI runs).
  The protocol tests' waits are 60 s (`TEST_TIMEOUT`).

- Tick pacing: the game loop's tokio interval ticks on a 1 ms grid, so
  at 420 Hz single ticks run 1-4.5 ms apart round the 2.38 ms budget
  (verification, platforms). If a client shows it, pace the loop on a
  high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`)
  or sleep short and spin the last fraction; measure with
  `[debug] tick_jitter_seconds`.

- A `UBorder`'s `SetHorizontalAlignment` / `SetVerticalAlignment` after
  its content is set never reaches the content's slot (the slot keeps the
  alignment it was made with, and `UBorderSlot::SynchronizeProperties`
  wins at build): the main menu's Esc card filled the screen for that
  (fixed 2026-10-24, `menu-esc-overlay`). `ApexUI::AlignContent` sets
  both; the hotlap header and footer and the main menu's top bar use it.
  Other screens still call the border's setters after `MakePanel`
  (`grep -n "Bar->SetVerticalAlignment\|->SetHorizontalAlignment(HAlign_Center)"`
  in `Private/UI`): their rows mostly centre their own children, so look
  at each screen before moving it to the helper.

- An away driver's held car (`GameSession::away`, driven on the
  cool-down driver's input) gets no racecraft: put into a wall while its
  driver is gone, it is never backed out or put back. The debug stand-in
  now has it (`update_racecraft`'s stand-in profiles); an away car would
  want the put-back at least, judged against the cool-down pace.

**Check:** server and client build and tests.
