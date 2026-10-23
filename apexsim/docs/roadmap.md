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

### 7. Championships

The offline season is built (Main menu > Drive > Season,
[client.md](game/client.md#offline-season-seasonapexseasonh-uiapexseasonwidgetcpp)). Left:

- Online leagues on the race weekends' persistence and the proved driver
  ids (R3): an event's entry list, standings across days.
- A fastest-lap point; each round's own rules (weather, a weekend with
  qualifying, a timed race) instead of the season's one race format.

**Check:** a league season with two humans across two server restarts.

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

- An in-game editor (colours, decals, number); online, own liveries sent
  to the other clients, size-capped and cached by hash. (The import into
  `cars/custom/liveries` and the template tool are built.)

**Check:** a custom livery on a second client.

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

## P3: nice to have

### 28. Smaller recorded sessions

Cut a `.apxs` stream file to about 40% of its size without losing a bit,
keep zlib on both sides, and bound what recordings may occupy
([proposals/replay-size.md](proposals/replay-size.md)). Today 16 cars are
45 to 57 MB an hour, on the server and again on every client in the
session; the frames are 87% of it.

- Phase 1, a columnar block: a `ColumnBlock` record (type 11), columns per
  car and field, deltas from the previous frame, byte planes, zlib-6;
  `FILE_VERSION` 2, readers keep reading version 1; the wire does not
  change. About 1.9x smaller. `scripts/replay_size.py` lands with it.
- Phase 2, ten-second blocks (`BLOCK_SECONDS` on both sides,
  `QUEUE_BLOCKS` 512 to 51): another 1.25-1.4x.
- Phase 3, lossy and measured first: wheel slip off every row (the
  watched car's road sound is the judge), cm poses, a second-order delta.
- Phase 4, how much is kept: `[records] keep_replays` / `keep_replays_mb`
  on the server (default unlimited), a size line on the client's Replays
  screen, the timeshift buffer held packed.
- `.gitignore` ignores `server/replays/` but not `/replays/`, where a server
  run from the repo root writes.

**Check:** a round trip test (`bodies` of `pack` is the input),
`a_session_is_recorded_as_a_stream_file`, a version 2 golden `File` beside
the version 1 one in `ApexSpectatorGoldenBlobs.h` / `BackdropTests.cpp`,
`apexsim-replay info --check` on every re-rendered showcase, and the
determinism of `render` (same seed, same bytes).


### 16. Driving school

- Challenges on the racing line and corner data: brake at a board, a corner
  under a target, a clean lap under time; medals.

**Check:** each challenge passed and failed by the stand-in.

### 17. League rules

- Success ballast by championship position (needs championships), a
  `CreateSession` rule; a car without it simulates as before. Built: the
  reverse top-N grid (`race_rules.reverse_grid`) and balance of performance
  per car (server.toml `[[balance]]`).

**Check:** session tests per rule; the class calibration unchanged.

### 20. Photo mode

- Pause, free camera, focal length, depth of field, exposure, no HUD, a
  high-resolution shot.

**Check:** a shot from a paused replay.

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
