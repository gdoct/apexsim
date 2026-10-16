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

### 15. AI in the snow

The AI's reset to the road (`Racecraft::wants_reset`) keeps a snow race
going, but cars still cook their engines on lap 1.

- On packed snow (grip 0.28) a car can stall on the road itself: Spa's
  Raidillon is 12.9%, and an AI that cannot climb it sits spinning its
  wheels at the limiter until the engine is out (R4 run, 2026-10-15: the
  stand-in and one AI out on lap 1, the rest racing). The reset counts only
  cars off the road or crawling against a wall, and putting a car back where
  it is would not get it up the hill. The AI wants to lift when its driven
  wheels spin with the car not moving, and a car that still gets nowhere a
  step back down the lap; the engine's cooking at a standstill may want a
  look of its own.

**Check:** the snow survey (`SURVEY_WEATHER=snow`, Spa) and a long R4 run:
no engine out without a hit.

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
- Spa: a large untextured white block stands left of Eau Rouge's
  compression (seen from the road and from above, 2026-10-15 R1/R3 runs);
  a prop or building without its material.
- Nordschleife: open sea on the horizon. Past the terrain's edge the view
  from the road (posts at 165 m, the descent, the Karussell) runs out to a
  flat blue band: the Eifel is inland, the horizon wants hills or haze.

**Check:** each circuit by hand in the running game; the track-editor tests.

## P3: nice to have

### 17. HUD data on a stream

- The spectator stream's `CarExtra` row does not carry `disc_wear` or
  `stint_energy_pct`, so a watcher's HUD shows `item.disc_wear_pct` and
  `fuel.stint_pct` as *null* while the driver's own shows them; a replay
  loses them too. Append both to the row (spectator.rs `CarExtra::write`
  / `read`, the client's `ApexSpectatorStream.cpp`).

**Check:** `spectator_wire_format` and `ApexSim.Net.Spectator.*`.

### 11. Content tooling

- The dressing's `surroundings` stamp is one for the whole layer (car
  parks, camps, village, marshal posts, corner signs, forest, floodlights,
  fan dressing): `--explain` tells them apart by asset only. Stamping each
  sub-layer in `surroundings::lay` would name the dossier area each came
  from.
- The shipped `.ats` scenes carry no source stamps until each is dressed
  again (`ats-dress --all` adds them and changes nothing else); until
  then `--explain` reports every prop as owned by no pass.
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
- Verification runs (`scripts/verify`): a tyre's tread (two flat spots,
  pickup, blisters, a deflated sidewall) cannot be judged on a moving car,
  and nothing freezes one unattended: `apexsim.recover` refuses a moving
  car, and `-ApexReplay` has no pause. A pause at a replay time (or a
  hold-still debug event) would make those checks a run. The stand-in, an
  AI, also pits for any leak and at a 0.6 flat spot, wear 70 or damage 25
  (`pit::plan_stop`), so the shot has to come within that lap.
- Imported pit lanes (`ac_import/pit.py`): the AI pits on all seven imports
  tried (Zandvoort, Silverstone, Spa, Barcelona, Red Bull Ring, Imola, Poznan:
  `PIT_CUSTOM=1 ... ai_pit_survey`), but the scripted human in
  `pit_lane_survey` is not taken over at Spa and Red Bull Ring: it cuts across
  La Source's mouth into a wall, and at the Red Bull Ring it drives 4 m
  inboard of AC's line, which is still on the road edge. The autopilot itself
  is untried there.

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

- Puddles do not show: the fully low cell at Eau Rouge (1035 m, low 1.0)
  and Spa's 0.71 cell look like the wet road round them in heavy and in
  light rain, at road height and from above (2026-10-15). The water rows
  reach the state texture's A; `ApplyRoadStateGraph`'s puddle term is too
  weak against the wet road's own gloss, or not read.
- At night under the floodlights (Mandarina Bay, 21:00) the road blows out
  to a near-white yellow from the TV and overhead cameras: the asphalt's
  texture is gone. Exposure against the floodlights' level.
- The cloud dome at cloudy (0.5 cover) is an even field of small puffs,
  more altocumulus than scattered cumulus: the noise scales in
  `BuildCloudLayer` (90 m and 25 m) read small from the ground.
- The chase-near camera behind the Bugotti Chiffon showed a dark blur
  filling the frame once (snow run, 2026-10-15): the camera inside a body.
- The pit lane's own snow cover (`RoadState::lane_snow`, packed by the
  cars in the lane) is not on the wire: the lane is drawn with the lap's
  mean cover (`SnowMids`). It wants a row of its own beside `SnowRows` and
  the lane's material reading it.
- A replay or a watched stream throws no dust and has no ground under its
  tyres: the spectator stream's rows carry each wheel's surface but not
  `CompactCarState::ground`, so `UpdateDust` and the road sound's gravel
  and sand are live only.

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
  (the Le Mans seam, the qualifying outlap, the racecraft pass). Either
  CI bakes the sidecars it needs, or a test that drives a circuit says
  which backend it expects and skips without it. Check a test change
  against `git archive HEAD server content` in a scratch folder, with its
  own `CARGO_TARGET_DIR`.

**Check:** server and client build and tests.
