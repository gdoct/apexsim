# Roadmap

Product features, content and tooling that are not started yet. What is
left on a feature that is built (bugs, edge cases, cleanup) is in
[follow-ups.md](follow-ups.md); what is built but has not been seen, heard
or felt yet is in [verification.md](verification.md); simulation and
racing-rule gaps (physics, tyres, AI, pit and race rules, race weekends)
are in [simulation-gaps.md](simulation-gaps.md). An item lives in exactly
one of the four.

The backlog is a list of **work packages**. A package is one go: the items
in it touch the same code, the same wire change or the same content
refresh, so they are built on one branch, verified with one check and
committed together. Each package says what it is for, what is in it, and
how it is checked. A package is done when its items are built and its
check has been run. When an item is built, delete its bullet (anything it
leaves behind goes to [follow-ups.md](follow-ups.md) or
[verification.md](verification.md)); when a package has none left, delete
the package. Git history keeps what was done.

Ideas that have no scope or check yet are not packages; they wait under
[Not yet scoped](#not-yet-scoped) until someone writes them one.

Priorities (a package takes the priority of its most urgent item), and
within a priority the packages are in the order to do them:

- **P0**: blocks someone from playing a race start to finish. Build before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is missing.
- **P2**: depth that keeps players coming back.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

None open.

## P2: depth

### 8. Public server list

Parked 2026-10-18 (not now): players join known hosts by address.

- A small self-hosted Rust service (a Dockerfile beside the server's)
  that servers register with and heartbeat to over HTTPS (opt-in per
  server, TLS fingerprint, verified by connecting back); an Internet tab in
  the session browser.

**Check:** two servers listed, one going away; joining from the list.

### 9. Chat: the session browser, voice

Built: lobby and session chat, the race's chat line and HUD component, the
dashboard's mute ([server/protocol.md](server/protocol.md)). Left: a chat
box on the session browser for the lobby's own lines, a profanity filter
(a decision), and voice. Long lines on the HUD wrap (`"wrap"` on a text).

### 10. Upscaling and HDR

Built: a render scale row (TSR) set by the presets, an HDR switch and its
paper white ([game/client.md](game/client.md#graphics-quality-uapexsettingssubsystemapplygraphics)).
Left:

- DLSS and FSR as plugins (decided 2026-10-18: both; TSR stays the
  default, and NVIDIA's DLSS SDK terms are accepted when it is
  downloaded).

**Check:** the presets on the RTX machine and an integrated GPU; mirrors
and triple screens at each scale; HDR on an HDR display.

### 11. Own liveries

Built: the import into `cars/custom/liveries`, the template tool, the
garage's editor (paint, accent, a number on the logo, a name) and own
liveries sent to the other clients
([cars.md](../../apexsim-content/docs/cars.md#liveries)). Left:

- Decals in the editor: an image placed over the paint (a skin painted in
  the game rather than in the template tool).

**Check:** a decal on the car in the turntable and in a race.

### 24. The simulation in the client: backdrops, showcases, replays

The client links the server's physics for prediction
([proposals/client-prediction.md](proposals/client-prediction.md)); the
same library can run whole sessions in the game where nobody races
against it: no server process, no recorded stream, one simulation.

- Built (2026-10-20): the library runs a whole seeded race, the server's
  own code behind the C API. The setup `apexsim-replay render` runs lives in
  the simulation (`apexsim_sim::seeded_race`: `start_race`, `tick_field`,
  `seeded_id`; the server's `replay_tools` re-exports it), and
  `apx_race_start` / `apx_race_tick` / `apx_race_cars` / `apx_race_digest`
  (ABI 4) start and tick it: `server/ffi/tests/seeded_race.rs` holds it to
  the render's race digest for digest each second.
- Built (2026-10-21): showcases made in the game. The wire, the stream and
  the render moved into `apexsim-protocol` (the server re-exports it), the
  library renders an entry of `showcase.yml` into the build's file byte for
  byte (`apx_showcase_render`, ABI 5), and the menu renders every showcase
  with no fresh file into `Saved/ApexSim/Showcases/` in the background
  ([game/spectator.md](game/spectator.md)); `apexsim-replay showcase` and the
  pipeline read the list through the same code. Track guides likewise
  (`apx_guide_build`): a guide of the car's class that is missing or stale
  is made when opened, in seconds, into `Saved/ApexSim/Guides`
  ([track-guide.md](../../apexsim-content/docs/track-guide.md)). Watch
  hotlap is simulated in the game too (`hotlap_watch::LocalWatch`,
  `apx_watch_*`), server or not, its level and ghost kept in
  `Saved/ApexSim/Watch` ([sessions](server/sessions.md#watching-a-hotlap)).
  No build stage renders a showcase or a guide and no package ships one:
  the server renders the showcases of its `[showcase] list` into its folder
  at startup the same way ([game/spectator.md](game/spectator.md#server-showcases)).
  A server refuses `Demo` and `HotlapWatch` creates: the game no longer asks
  it for a backdrop race or a watched hotlap.
- Showcases kept as a seed and re-simulated as they play, instead of a
  rendered `.apxs` in `Saved/`.
- Replays as seed plus inputs, re-simulated on load, instead of streamed
  state ([proposals/replay-size.md](proposals/replay-size.md)): far
  smaller files.
- Tools without a server: the marketing site's shots, the
  benchmark, a car's stat sheet (a lap estimate) and setup what-ifs in the
  garage run in the game process; the scripts stop launching a server.
- Later, perhaps: offline single-player with no server at all.

The server stays the only authority for any session a human races in
online (CLAUDE.md, "the server owns every rule"): the client's sim plays
only what nobody competes in.

**Watch out:** the session tick must be untangled from the game loop, as
prediction's car step was from the session; determinism across the two
builds (the client's lib and the server binary) is checked by prediction's
trace test (`server/ffi/tests/lone_car_trace.rs`) and has to hold for whole
seeded races; the client needs the track's physics data (the bundle a
server hands out) and matching content checksums; the game's binary grows
by the AI and session code.

**Check:** for what is left, a replay re-simulated from its seed and
inputs equal to the recorded stream; a tool (the stat sheet's lap
estimate, the benchmark) giving the server's figures with no server
running.

### 27. Content packs and a content repository

Split the cars, circuits, props and the tools that make them out of the
code into versioned packs that install and update apart from it
([proposals/split-off-content.md](proposals/split-off-content.md)). The code
depends on the content and never the reverse. A release copies a starter set
from the content (decided 2026-10-18: one car per class, plus Spa,
Zandvoort and Monza). The launcher, the server and the game get an upgrade
path in which a code release and a content release are separate events.

- Phase 0, two roots in this repository: `apexsim/` and
  `apexsim-content/` (cars, tracks, props, the track editor, `track-core`
  and the content tools), each laid out as its future repository. The
  roots are in place; the steps left are in the proposal, from a content
  `bake.ps1` and the starter set to a `formats` crate shared by the server
  and `track-core`.

- Phase 1, no network: `pack.json`, `installed.json`, `build_packs.ps1`,
  `apexsim-server content install <file>`, the launcher installing a local
  `.apxpack`.
- Phase 2, the split: each root made into its repository with its history
  (`git filter-repo --subdirectory-filter`, LFS kept), the code pinning a
  content commit (`content.lock`), CI on both sides.
- Phase 3, the signed index (GitHub release assets of `apexsim-content`,
  decided 2026-10-18) and updates: the dashboard's Updates section,
  the launcher's pack and code updates, `ContentVersion` on the wire.
- Phase 4, update-and-join and `require_match`.

**Check:** each phase's check in the proposal; at the end, the
client-has / server-has table in it, every row.

### 29. Circuit dressing to the references

Every circuit brought up to its real venue's look from the dossiers in
[circuit-references/](../../apexsim-content/docs/circuit-references/README.md),
column by column over all tracks: start/finish, landmarks, stands and
screens, trees, barriers, boards, then people. Method, status table and
kit gaps in
[track-improvements.md](../../apexsim-content/docs/track-improvements.md).

- Per column: the kit assets the audit found missing, then the dossier
  and rule edits per track.
- People last: a static-figure kit family (spectators on banks and at
  gates, marshals at posts) and its client instancing.

**Check:** per column, the before/after screenshot runs in
`scripts/verify/runs.yml` judged by a person; the AI survey against the
commit before for the barrier column; `--all --manual-only --dry-run`
reporting every other dossier unchanged after each dossier edit.

### 39. Road surface detail

The road is one asphalt set (`BuildTrackBase` in
`ApexTrackMaterialGraphs.cpp`, the `asphalt` maps from
`bake_ground_textures.py`) with a mottle noise. From a cockpit it reads as
uniform soft charcoal: no aggregate, no wear, no patches. `RoadState.rubber`
already darkens the racing line live; this is the static surface under it.

- Aggregate at road scale: the asphalt set's tiling, normal and roughness
  judged at 2 to 5 m from a cockpit, and a macro variation mask at 20 to
  50 m (a second octave over `NoiseAmount`) so the tile never repeats down
  a straight.
- A wear layer: a lighter band between the wheel tracks, darker tracks,
  patch repairs, as a static texture or noise term in the same graph,
  separate from the live rubber.

**Check:** the `sf-ref-*` frames (cockpit from the pole box, the eye on the
line) at Armadillos and at least two other circuits so the
tiling holds on every road; the material is shared, so one change moves
every track.

### 40. Grid boxes, the chequer and COTA's painted apron

- Grid boxes drawn solid with their slot number (`grid_boxes` in
  `ue_export.rs` paints outlines and a pole stub; the number as painted
  glyph quads or a decal per slot), and a darker, rubbered paint inside the
  box; the chequer's length against the FIA start line.
- The blue-and-white apron along the main straight's right edge: a `custom`
  marking (alternating colour over the straight's stations at the edge
  lateral) in the Armadillos YAML, or a decal set when the marking kind
  cannot stripe. The YAML edit changes the track's checksum: re-export.

**Check:** the `sf-ref-*` shots; the start lights and grid placement are
untouched (`c5-sf-armadillos`); a second circuit's grid still reads.

### 41. The main straight's walls: catch fences, pit wall, screens

- `barriers::decide`: concrete with `catch_fence_6m` on both sides of the
  main straight at a permanent circuit (the audit's open "confirm" cell for
  Armadillos' barriers), in place of armco with the short mesh.
- The pit wall over the box span: `pit_wall_gantry_6m` (team stands) at
  permanent circuits as on street ones (`ue_export.rs` pit complex), and
  the wall's start checked against the S/F line: the frame shows the plain
  edge treatment where the lane (950 m, from inside T20 to near T1) should
  already run.
- The pit building's four screens (`MANUAL_LANDMARKS` at 20, 105, 190 and
  275 m left) laid, facing the stand, readable from the line.

**Check:** the `sf-ref-*` shots and `c5-sf-armadillos`; the barrier change
moves `walls.msgpack`, so the AI survey against the commit before; the
dossier tool's `--all --manual-only --dry-run` reports every other dossier
unchanged.

### 42. No track lighting at a day venue

`CircuitStyle::floodlights` is true for the default and prairie styles, so
`surroundings::lay` rings any circuit whose OSM extract maps no lighting
with `floodlight_tower`s (23 at Armadillos, the same at AlbertParkour), and
the car parks' `lamp_post`s run along the trackside fence.

- A day-venue flag per circuit (the dossier or the style) that lays no
  floodlight ring; the car-park lamps kept to the car parks, off the fence
  line. A night session at such a circuit is lit by the sky and the cars
  alone, as the real place would be.

**Check:** re-dress `--all` and read the prop counts against the commit
before; the `sf-ref-*` left side; one night run at Armadillos to see the
headlights carry it.

### 43. The Main Grandstand

The stand is the stadium family (`bay_10m_stadium_roof`, roofed by station
`STAND_COVER "@102.8"`). It has no crowd twin, its canopy is a slab, it
starts nearly at the wall and its front edge stands too high.

- A `_crowd` twin for the stadium family (`build_stand_crowds.py`,
  `retexture_crowd.py`; `CrowdVariant` picks it with spectators on).
- The canopy as a light tensile roof on masts, and flags and rail dressing
  along the front, in the family's GLBs (`build_kit_gaps_stands.py`).
- The setback: the OSM outline is 19 m off the road; check where
  `LayoutGrandstand` puts the stadium family's road-facing edge (deep kinds
  pivot on it) and the front tier's height.

**Check:** `c5-stands-armadillos` and the `sf-ref-*` right side; the same
family on a bend (props.md's own caveat) before relying on the wedge.

### 44. A horizon for the open-country circuits

The prairie style is sparse by design (right for the infield), so the
horizon at Armadillos is bare to the vanishing point. The real one is
closed by live-oak belts at the property line and hills to the north.

- A `forest_impostor` belt along the property edge and the far roads for
  the open-country styles (`dress.rs` surroundings, `PRAIRIE` and Sakhir's
  clumps), leaving the infield as it is.
- The DEM horizon mesh checked at Armadillos: the `outer` grid and
  `horizon_skirt` should carry the hills north of the circuit over the pit
  building; if the relief is there but reads flat, the gap is package 45's.

**Check:** the `sf-ref-*` horizon and a `g3-horizon`-style shot at
Armadillos; re-dress `--all` counts for the other open-country circuits.

### 45. The daytime sky: haze and high cloud

- Aerial perspective at `sunny` midday on this circuit: the far gantry, the
  pit building and the horizon should soften with distance (the far haze
  since `ApexSky::FogLuminance` is verified at the Nordschleife only;
  verification.md's row names the other cases).
- A thin high cloud veil inside `sunny` (cloud cover is 0 there; the live
  day has cirrus): a cirrus sheet at low cover in `ApexCloudLayer`, or a
  `cloudy` variant with a high, thin look, so a bright-overcast day is a
  choice on the create screen.

**Check:** the `sf-ref-*` frame against the live one; `SkyModelTests` hold;
the sky ladder's other weathers unchanged in their verify shots.

### 46. The start gantry

The kit has no start gantry mesh (`props/bridge/gantries.blend` holds the
banner and sign gantries), so the generated white box on lattice legs is
what every circuit shows.

- `bridge/start_gantry.glb`: a dark truss, a branded fascia (invented
  brands, `gen_brands.py`), the lights on a small housing, authored for a
  15 m road and scaled by `BridgeSpanScale`, with the five `ApexStartLight`
  lenses over its lamp row (`GantryLampX/PitchY/Z`).

**Check:** the countdown still drives the lights (`ApexStartLights` tags,
`c5-sf-*` runs); the `sf-ref-*` shot down the straight.

### 47. The formula car up close

The F1 generator already lays a sponsor atlas, a carbon weave and a painted
or carbon halo (`build_f1.py`, `textures.py`), and the wheels carry
sidewall lettering. The sunset frame showed a flat red body, so first find
out on the `sf-ref-*` frame whether the gap is the car in the shot or the
kit's density at T-cam distance.

- If the sponsors still read as absent: larger wordmarks and cells on the
  pod, cover and wings (`sponsors=` per variant, `liveries.py`), carbon on
  the wing elements, the rim texture and sidewall lettering at close range.

**Check:** the `sf-ref-*` frame and the car-select turntable; every name
stays invented ([naming.md](../../apexsim-content/docs/naming.md)).

### 48. Mirror and display faces seen from outside at low exposure

`ScreenNitsAt` scales the cockpit display's and mirror faces' emissive with
the exposure so they stay readable from the seat. Seen from a chase rung at
dusk the mirror faces burn to pink-white and throw two bright lobes onto
the road ahead.

- Clamp the face brightness seen from outside the car, or apply the scaled
  nits only while the view is the cockpit (`ApexCockpitRig`); check the
  emissive does not light the road through Lumen.

**Check:** the old sunset frame's recipe (chase `roof`, 19:00) with and
without the change; the cockpit view at night unchanged (verification.md's
night row).

### 49. A T-cam rung

The closest chase rung (`roof`) sits 0.6 m back and 1.4 m up, so the rear
bodywork fills the frame and the driver is out of it. A broadcast T-cam is
on the roll hoop, ahead of and above the helmet.

- A `tcam` rung in `ApexChase::Views()`: eye at the roll hoop, pitched 4 to
  6 degrees down, a wide field of view through `FovDeltaDeg`, the horizon
  kept, the halo and the driver's helmet (`<stem>_driver.glb`) in frame.
  `ChaseViewLevel` is a stored index and the ladder is closest first, so
  the new rung must not shift the meaning of saved indices (map them, or
  keep the array order and sort for stepping).
- Optional, off by default: a broadcast look (a touch of barrel distortion
  and bloom) as a console variable, for shots.

**Check:** `-ApexView=tcam` at Armadillos in the SF-26 and in a closed car
(the rung must clear a hypercar's roof as the others do); the `C` ladder
still steps through every rung.

## P3: nice to have

### 50. A broadcast HUD layout

- A shipped layout with a timing tower, a lower third (position, driver,
  gap) and a sector bar built from HUD components (`content/hud`),
  selectable under Settings > Gameplay > Layout and bindable to watching.

**Check:** `ApexSim.Hud.Data.Stable` and `.Documented` pass (no new data
points, or they are listed); the layout in a watch-session shot.


### 17. League rules

- Success ballast by championship position (needs championships), a
  `CreateSession` rule; a car without it simulates as before. Built: the
  reverse top-N grid (`race_rules.reverse_grid`) and balance of performance
  per car (server.toml `[[balance]]`).

**Check:** session tests per rule; the class calibration unchanged.

### 21. Benchmark from the menu

- The benchmark (`scripts/benchmark.py`, `-ApexReplayBench`) from the main
  menu, with the clip shipped or rendered on first use.

**Check:** a run from the menu and its numbers on screen.

### 22. Localisation

- UI strings moved to string tables as screens are touched; the game stays
  English until there is demand.

**Check:** a screen in a test language with no English left on it.

### 23. VR

- OpenXR cockpit view, world-space HUD panels, a VR pointer for menus,
  mirrors within the frame budget.

**Check:** a race in a headset at a steady 90 Hz.

## Not yet scoped

Ideas without a scope or a check. Each needs a short design (what it is,
what it touches, how it is checked) before it becomes a package.

- **Driver rating / safety rating.** No definition yet of what is rated or
  where it is stored.
- **Driver pictures.** Portraits for the drivers (results, standings, the
  HUD's driver tags) from the owner's own contact sheet of portrait
  photos (a 4-wide grid): split into single square crops, one per driver.
  Not on the spectator figures: a photo on their block heads read as a
  sticker. Needs the people's consent before it ships.
- **Shared leaderboards across servers.** Lap records and the track
  leaderboard screen are per server; sharing needs a service and a trust
  model.
- **More screens.** A five-wide row (`MaxSplitscreenPlayers` would need 5),
  a second monitor for telemetry, shared exposure across the triple-screen
  views.
