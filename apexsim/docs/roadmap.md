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

Built: a render scale row set by the presets, an Upscaler row (TSR, DLSS,
FSR, each where it runs; TSR the default), an HDR switch and its paper
white ([game/client.md](game/client.md#graphics-quality-uapexsettingssubsystemapplygraphics)).
Left: the check.

**Check:** the presets on the RTX machine and an integrated GPU; each
upscaler at 50, 75 and 100% in a race (`-ApexUpscaler=`), FSR 4 on a
Radeon, and a packaged build carrying the plugins' DLLs; mirrors and
triple screens at each scale; HDR on an HDR display.

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

- Built: phase 0 (two roots, then three with `apexsim-lib`, the content
  baking itself with `bake.ps1`, the starter set a release ships, the
  `apexsim-formats` crate) and phase 1 (loose content packs, `.acp`: a
  local install by the launcher and the server, a release shipping the
  content outside the starter set as `content/content.acp`, picking and
  uninstalling items).

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

### 53. The driver's hands and arms in the cockpit

The cockpit can draw gloved hands on every class steering wheel and arms
solved to the shoulders ([game/cameras.md](game/cameras.md), Driver's arms),
off by default: the first meshes (extruded prisms from `build_hands.py`)
read as toy hands, not as a driver's. Left:

- **Real gloves and sleeves.** Modelled in Blender (a generator beside the
  car scripts): each finger three phalanges with knuckles and the gaps
  between them, the palm and thumb closed round the grip, a wrist strap and
  stitched seams, subdivided (some 6-8k triangles a glove); sleeves with
  folds at the elbow, a knit cuff and the suit's seams. Leather and
  fire-suit fabric from CC0 scans with their normal and roughness maps,
  which needs the car material parents to take both (shared with package
  57). Judged beside the onboard reference frames (`g8-*` cockpit pairs)
  before the setting goes on by default.
- Hand over hand past about 180 degrees of lock (road and rally cars); the
  gloves now stay on the grips at every angle.
- A finger pulling the paddle at a gear change (gear from telemetry); the
  left hand to an H-shifter or the handbrake while the player uses them.
- Grip points for a car's own `steering_wheel_model` (today no gloves).
- The outside driver figure (`<stem>_driver.glb`) on the same grips and IK,
  so the TV, chase and replay views show the arms turning with the wheel.

**Check:** the `g8-*` cockpit pairs beside their references, cockpit shots
at full lock in a road car, a gear change caught mid-pull, an outside shot
of an AI car turning in; judged by the owner.

### 55. Trackside kit realism

The trackside kit
([props.md](../../apexsim-content/docs/props.md)) is built by bmesh
builders (`apex_props.py`) on numpy tileable maps (`apex_tex.py`,
`KIT_SLOTS`): clean uniform colours, hard edges with no highlight, no wear,
card trees and card crowds. That is why it reads as cartoonish. The
realism sheet ([game/realism.md](game/realism.md)) ranks it fourth (buildings,
stands, people) and seventh (close kit); the families seen closest go first (barriers, tyre
walls, catch fence, kerb edges, marshal posts, gantries, the pit building,
boards).

- **Sources:** CC0 textures (Poly Haven, ambientCG), the owner's choice of
  2026-10-31; meshes, trees and people stay generated. The kit must still
  rebuild from a fresh clone ([building.md](building.md)), so each sourced
  file is pinned by URL and hash, fetched by the content rebuild, and its
  licence recorded beside it.
- **Materials** through `KIT_SLOTS` and `retexture_kit.py`, so every GLB
  picks them up: albedo inside measured real ranges (no saturated primaries
  but paint), roughness and normal detail at close range, macro variation
  over a long barrier run, weathering by rule (dirt at the base, rain
  streaks under edges, rust on steel, rubber marks on barriers at the
  braking zones), decals.
- **Geometry**: bevelled edges for the highlights, real profiles and
  fixings on the close families (armco posts and bolts, concrete block
  joints, tyre-wall straps and conveyor belt, fence mesh with its posts and
  cables); LODs so the far kit costs no more than today.
- **Vegetation**: the card trees (`card_trees.py`) for 3D trees with
  Nanite foliage, or cards with proper normals, thickness and translucency
  at range; grass cards on verges and banks.
- **People**: the card crowd strips (`gen_crowd.py`) for low-poly 3D
  figures with a few animation variants (vertex-animation textures,
  instanced), the marshals among them.

**Check:** per family, a before/after shot run in `scripts/verify/runs.yml`
from the views on the realism sheet, judged by the owner; frame time on the
RTX machine no worse than before and the integrated-GPU budget kept
(work plan P3); `every_kit_file_is_catalogued` and the kit tests green.

### 57. Cockpit interior materials and lighting

The sheet's second gap. The cockpit is the car GLB's own interior (`carlib`,
`build_<class>.py`): switch panels, a display, door cards, seat, harness and
cage are modelled. Its panels are bare carbon at the twill's real tow size
(`carlib.UV_SCALES`) and its dash top, headliner and mats black suede from a
CC0 scan with its normal map ([cars.md](../../apexsim-content/docs/cars.md)).
Left:

- The closed cars' shell is lined inside (`carlib.inner_liner`, open at
  the glass) and the GT3's footwell closed by a front bulkhead; a patch of
  paint still shows under the dash on the passenger side of the GT3s, and
  the formula cars' tubs are not lined yet.
- Satin black paint on the bulkhead and floor, the seat's fabric and the
  harness webbing from CC0 scans, at measured albedos (0.02-0.04).
- The car material parents' default normal map sampled as a bump on every
  panel, so their normal scale defaults to 0 and only a GLB's map sets it;
  why the flat default did is not understood.

**Check:** the `g8-*` cockpit pairs in both classes before and after.

### 58. Ground and run-off materials

The sheet's third gap. Grass, gravel, asphalt and concrete sample CC0 scans
and the turf and run-off paint sit at measured albedos
([track-pipeline.md](../../apexsim-content/docs/track-pipeline.md), Ground
textures). Left:

- Run-off paint worn and dirty: tyre marks across it, faded at the edges.
- Macro variation over long distances (cut stripes on the verges, dry
  patches) and a gravel trap's raked lines.

**Check:** the `g8-*` TV, grandstand and woods pairs before and after.

### 60. Car paint

The sheet's sixth gap: clean, plastic-looking paint and one livery per
model across a field. Clearcoat with flake, a livery per AI car from the
car's livery set, dirt and rubber pickup over a stint.

**Check:** the `g8-*` chase and TV pairs before and after.

### 61. Car generators per car, and GT3 and rally bodies with character

The generated GT3s look alike and none looks special (the Limbotiti reads
as a hatchback, not a wedge), and the rally cars share their method. The
cause is the shape description, not only the script layout: every GT3 is
one `carlib.Loft` through 11 keys of 9 points with the same meaning per
index and the same creases (`hard=(2, 3, 4, 5)`), then the same
`fender_bump`, `tumblehome`, nose and tail remaps and arches, so a variant
only moves points inside one section grammar. The Limbotiti's flank keys
stand near-vertical to a hard belt crease, where the real wedge tumbles in
hard under a narrow cabin on wide hips with one arc nose to tail.
`build_rally.py` is the GT3 method with rally parts. The LMP2, hypercar and
F1 shapes are acceptable.

1. **The car drives the build.** One script per car in
   `apexsim-content/tools/generators/cars/` holds its data and calls the
   build in stages (body, face, lamps, tail, wing, vents, cockpit, decals,
   export); the class scripts become kits of those stages, which a car can
   replace one at a time. `carlib` keeps everything mechanical. This step
   moves no output: every car's `<stem>.glb`, `<stem>_driver.glb` (and the
   F1's `_drs.glb`) and car.toml generated tables come out byte-identical
   (or with identical mesh stats, if two runs of the old script already
   differ), built headless with Blender 5.1
   (`C:\Program Files\Blender Foundation\Blender 5.1`) on a scratch copy
   of the content root so `cars/default` is not touched. `docs/cars.md`'s
   generator table and "Running one" follow.
2. **Bodies for the GT3 and rally cars, the Limbotiti first.** The body
   stage each car's own: a loft with its own point count and creases where
   that suffices (the boxy rally cars), character curves (plan outline,
   side profile, belt and shoulder as 3D curves, surfaced between) for
   the wedges. Judged by eye on orthographic side, top and front views and
   a three-quarter view next to a reference silhouette (`preview_cars.py`),
   not by measured numbers alone. The imported AC cars give proportions
   only, never a mesh to fit tightly: a body evokes its car without copying
   a protected design (naming.md's rule for names, carried to shapes).

The realism work on the car interiors and paint (57, 60) touches the same
builds; take this after it lands.

**Check:** step 1 by the byte comparison over all seventeen cars; step 2 by
before/after preview sheets per car judged by the owner, then the arches,
`[wheels]` stance, cockpit eye and damage parts (`damage_parts.py`) still
agreeing with the new body, and the cars seen in a race.

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
- **Further realism candidates**, after packages 57-60: road surface
  detail (cracks, sealant lines, marbles off the line), tyre smoke and dust kicked off the verge, heat haze over the grid
  and behind exhausts, rain and dirt on the visor or windscreen, motion
  blur, a lens for the TV cameras (barrel, soft highlights), the outside
  driver figure's suit and helmet detail.
- **Shared leaderboards across servers.** Lap records and the track
  leaderboard screen are per server; sharing needs a service and a trust
  model.
- **More screens.** A five-wide row (`MaxSplitscreenPlayers` would need 5),
  a second monitor for telemetry, shared exposure across the triple-screen
  views.
