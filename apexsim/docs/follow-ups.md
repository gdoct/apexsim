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

### 31. Wheel forces at a standstill, and over-revving without protection

Found at the wheel sitting of 2026-10-23 (ClubSport V2.5, base FF 100%,
game force 0.50, damping 0.25; paddle shifters, no H-shifter). Run files
are local, not in git, under `out/sit/2026-10-23/`.

- Standing still, the rim starts shaking violently: an oscillation with no
  driver input, a loop between the rim's angle and the force at zero speed
  (self-aligning torque, rim correction or stiffness limit with too little
  damping when the tyres have no rolling speed). The first 15 s force line
  of the run reads torque |mean| 0.03, constant |mean| 0.02, correction
  0.011, stiffness limit 0.90: the 15 s means hide it, so a MARK or a
  frame trace at the grid is needed. Block A, Caravan GT3 at Monzarella;
  `a-monza-1/` (client log, server recording).
- The rally cars have no downshift protection (as the real ones), so a
  missed downshift still blows their engine with no warning: a shift light
  or a cue as the revs pass the limit, and the over-revved rpm in telemetry
  and the recording, which clamp `engine_rpm` at the limit and so hide it
  (the GT3's failure at Monza was read from the gear and speed instead).

**Check:** sit on the grid and in the pit box with hands off the rim for
30 s: the rim stays still or settles, no growing shake (the `Wheel forces
over 5 s standing:` log line says which force it is). A rally car's
missed downshift warns before it hurts.

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

- Armadillos: a tent stands over the track at turns 15 and 16 (seen at the
  wheel sitting of 2026-10-23). Find the prop in `Armadillos.ats` and why
  `ats-dress` did not push it clear of the road. The practice replay
  `out/sit/2026-10-23/cota-f1/practice.apxs` (local) drives past it.
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

### 28. HUD panels on top of each other

Seen on 2026-10-20's verification runs (`out/verify/v-r13-phase`,
`v-r12-go-out`, `v-r12-weekend-shootout`, `v-r13-blue`), on the layout in
`hud/custom/layout.json`; the components a layout does not name take their
region's place without looking at the panels pinned there.

- The `race_phase` banner (FORMATION LAP · HOLD YOUR PLACE · NO
  OVERTAKING, VIRTUAL SAFETY CAR · NO OVERTAKING · HOLD THE PACE) runs
  under the `race_state` panel at the top right; only VSC ENDING and BLUE
  FLAG are short enough to clear it.
- In qualifying the timing panel (QUALIFYING, OUT LAP, the lap clock, BEST,
  RECORD) draws over `damage` and `tyres` on the right.
- The `conditions` panel, once added, sits under `damage`: its forecast
  lines are cut ("…ain in 14:50") and the wind line clipped.
- The standings' gap to the leader is the distance over the car's own
  speed (`ApexHudData.cpp`, `gap_leader_s`), so a car slowing for a corner
  shows more than the car behind it on a straight (P2 +34.5, P3 +28.0, P4
  +30.0 in one frame) and the column jumps about.
- The qualifying results screen after a one-lap shoot-out shows a plain
  yellow block under LAP BY LAP.

**Check:** the same runs, every panel clear of the others and the gaps in
order down the standings.

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

## P4: Unrefined

### 13. Windshield wipers
- Add windshield wiper functionality to respond to rain and other weather conditions. This requires model updates, a key binding, and appropriate animation plus sound in first person view.

### 14. Improve sound system

- Enhance the sound system for better spatial audio, realistic engine sounds, and environmental effects. This may involve updating audio assets, adjusting sound cues, and integrating with the physics engine for dynamic audio responses.
- Improve sliding sound effects for better realism, including tire squeal and surface interaction audio.


### Top view
- If we use the top view with formula cars, somehow the image looks really cheap and lacks the visual fidelity seen in other camera angles. This needs to be further investigated and fixed.

## P5: Cleanup

### 99. Cleanup

Small and independent. Do one or two alongside whatever package touches
the same files.

- Server tests: `protocol_test::test_the_track_bundle_comes_in_chunks_on_request`
  times out (15 s) in the whole suite under load, and passes alone; the
  join and tick-rate tests do too in a debug build. Run the suite at
  opt-level 2, as CI does, or give the bundle test a longer timeout.

- Tick pacing: the game loop's tokio interval ticks on a 1 ms grid, so
  at 420 Hz single ticks run 1-4.5 ms apart round the 2.38 ms budget
  (verification, platforms). If a client shows it, pace the loop on a
  high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`)
  or sleep short and spin the last fraction; measure with
  `[debug] tick_jitter_seconds`.

- The main menu's Esc menu (Back to main menu / Exit game,
  `UApexMainMenuWidget::BuildQuitOverlay`) is built as a 420-wide card in
  the middle over a dim, and draws as a card filling the whole screen
  (`out/verify` shots of 2026-10-21, on the root page and on Garage >
  Tracks).

- `ApexSim.Backdrop.ShowcaseRenderMatchesBuild` and
  `ApexSim.Guide.MadeMatchesBuild` compare the game's own render with
  `build/showcase` / `build/guide`, which no stage writes since N28: on a
  tree that still has the old files they fail as soon as Zandervoort's
  content moves (seen 2026-10-23, files of 10-20, content of 10-22), and
  on a fresh one they skip. Render the reference with `apexsim-replay`
  in the test instead (the server's render is the reference), or drop
  them for the replay crate's own determinism tests.

**Check:** server and client build and tests.
