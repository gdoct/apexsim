# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ApexSim is a source-available, proprietary simracing platform with a high-frequency authoritative Rust server (240Hz) and an Unreal Engine 5.8 client. The server owns physics simulation and distributes telemetry while handling lobby/session management over TCP+TLS with MessagePack serialization. Protocol v2: after a token handshake binds the client's UDP address, telemetry (compact positional encoding, session-scoped car indices announced via a reliable `SessionRoster` message) and player input flow over UDP, with TCP fallback for un-handshaken clients.

## Build Commands

### Server (Rust)
```bash
cd server
cargo build                    # Debug build
cargo build --release          # Release build
cargo run                      # Run server with default server.toml
cargo run -- --config path.toml --log-level debug  # Custom config
cargo fmt && cargo clippy --all-targets            # Lint (CI enforces fmt --check)
cargo bench                    # Criterion hot-loop benchmarks (benches/physics_tick.rs)
```

### Server Tests
```bash
cd server
cargo test                     # Unit + integration tests (server spawned in-process, no setup needed)
cargo test -- --ignored        # Long-running stress/soak tests
cargo test --test integration_test test_name -- --nocapture  # Single integration test
```

Integration tests spawn the server in-process on ephemeral ports via `apexsim_server::server::run_server` (see `tests/common/mod.rs`); no manually started server is required. `tests/determinism_test.rs` asserts bit-identical sim runs — keep the simulation free of HashMap-iteration-order dependence, wall-clock reads, and RNG.

### Unreal client (`game-unreal/`, UE 5.8, C++)
The project is `game-unreal/ApexSim.uproject` (`EngineAssociation` 5.8) with
the modules `ApexSim` (game), `ApexSimNet` (protocol), `ApexSimInput`
(DirectInput wheels), `ApexSimBoot` (splash hold) and `ApexTrackEditor`
(editor-only: the import commandlets). The scripts find the engine through
`-EngineRoot`, `$env:UE` / `UE_ROOT` / `UE5_ROOT`, the registry entry for the
`EngineAssociation`, then the launcher's default install folder
(`scripts/lib/ApexEngine.ps1`). Every commandlet below needs the editor closed.

```powershell
$UE = "C:\Program Files\Epic Games\UE_5.8"          # or wherever the engine is
# Editor target (what the commandlets and play_editor.ps1 run on)
& "$UE\Engine\Build\BatchFiles\Build.bat" ApexSimEditor Win64 Development `
    -Project="$PWD\game-unreal\ApexSim.uproject" -WaitMutex
# Or generate the .sln (right-click the .uproject -> Generate Visual Studio
# project files) and build ApexSimEditor / Development Editor from the IDE.

./scripts/play_editor.ps1 -Build        # build, start a local server, play (no cook)
./scripts/build_game_standalone.ps1     # cooked package -> artifacts/ApexSim-Win64
```

Automation tests (`ApexSim.*`) run headless on the editor build:

```powershell
& "$UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\game-unreal\ApexSim.uproject" `
    -ExecCmds="Automation RunTests ApexSim; Quit" -unattended -nullrhi -nosplash -log
```

Adding a `.cpp`: check it with `-DisableAdaptiveUnity` (anonymous-namespace
clashes only show once unity builds batch it with its neighbours).

### Fresh checkout: building the client's content
`game-unreal/Content/` is gitignored. Only the menu (`UI/`, `Maps/L_Menu`,
`Blueprints/`), the two catalog tables in `Data/` and the splash (`Splash/`)
are checked in; the props, ground textures and the track and car parent
materials are **generated** from `content/` by commandlets, and the tracks by
the Rust bake, so a fresh clone draws cars in flat colours and races in an
empty world until they have been run. (The cars themselves need no step: the
game builds them from `content/cars` at runtime.) One script does all of it,
with Rust, Python 3
(numpy, Pillow, PyYAML) and the engine installed and the editor closed:

```powershell
./scripts/initialize_content.ps1            # build only what is missing
./scripts/initialize_content.ps1 -DryRun    # list what is missing and the plan
./scripts/initialize_content.ps1 -Force     # redo every stage
```

It checks each output on disk and runs only the stages that lack one, so it is
safe on an existing checkout too. The stages, in order (run by hand if needed):

1. `Build.bat ApexSimEditor Win64 Development` - the commandlets live in it
   (incremental; `-SkipBuild` skips it).
2. `cargo build --release` in `server/`, if there is no server exe.
3. `bake_ground_textures.py` (the PNGs are normally checked in) and
   `-run=ApexGroundTexImport` -> `/Game/Ground`.
4. `-run=ApexMaterialBake` -> the four track parent materials under
   `/Game/Materials/Track` and the four car parents under
   `/Game/Materials/Car`, when one is missing or stage 3 ran (the track base
   parent's surface graph depends on the ground textures).
5. `-run=ApexPropImport -all` when any kit GLB has no mesh.
6. `build_track_levels.ps1 -Release -SkipMaterials` for every circuit missing
   its export (`.uescene.json` + `.uemesh`), its preview or a
   `{ground,curbs,walls}.msgpack` sidecar (which the server needs). No track
   is rebaked because stage 3 or 5 ran: the game dresses each circuit with
   what `/Game` holds when it builds it.
7. The showcases of `content/showcase.yml` (`apexsim-replay render`, see
   "Spectator stream" below) that are missing or stale by `info --check`:
   the menu's backdrop races, `build/showcase/*.apxs`, a few seconds each.

It also checks that every file each car.toml names (model, DRS flap, logos,
class wheel) is on disk, and imports no car: see "Cars" below.

Stage 6 is the long one on a fresh clone (all circuits), and needs no engine. The data refreshes in
the track sections below (`osm_layout.py`, `dem_fetch.py`, `dem_elevation.py`,
`ats-smooth`, `ats-bank`, `drs_zones.py`) are **not** part of it: their outputs
(dossiers, DEM sidecars, YAMLs, `.ats` scenes) are checked in.
`build_release.ps1` does not import ground textures, so run this script
first on a new machine.

### Track Editor (Rust + Bevy)
```bash
cd track-editor
cargo run                                    # Run track editor
cargo run --bin ats-export -- --all          # Bake every track for Unreal
cargo test                                   # Both crates; skips the whole-calendar bakes
cargo test -p track-core                     # Pipeline only, no Bevy build
cargo test -- --include-ignored              # Also bake every real circuit (~20 min)
```
`track-editor/` is a workspace of two crates: `track-core` (`core/`: the
`.ats` format, groom/dress/smooth/bank/terrain, the Unreal bake, every
`ats-*` tool and the integration tests) and `track-editor` (`src/`: the Bevy
viewport and MCP server on top of it). The pipeline has no Bevy dependency,
so an `ats-*` tool or `cargo test -p track-core` never compiles the renderer.

### Play from the editor build
`scripts/play_editor.ps1` runs the game through `UnrealEditor.exe -game` on
the ApexSimEditor binaries - no cook, so a C++ change is one incremental build
from playable. It starts a local server (`cargo build --release` first) when
settings.yml points at this machine and nothing is listening, and warns when
the running server or the editor build is older than the source. `-Build`,
`-RestartServer`, `-DryRun`; `-CreateShortcut` puts a desktop shortcut
that runs it with `-Interactive` (asks instead of warning).

### Standalone game build
`scripts/build_game_standalone.ps1` runs UAT BuildCookRun into `artifacts/ApexSim-Win64`,
then copies the track exports and previews into `Tracks\` beside
`ApexSim.exe`, where the game looks for them (`-SkipTracks` to leave them
out). Packaging relies on `bCookAll=True` in `DefaultGame.ini`: the catalog
tables, the track materials, the prop kit and the ground sets are only ever
found by path at runtime. `/Game/Tracks` (editor-only inspection levels) is
never cooked.

### Release package
`scripts/build_release.ps1` runs the whole pipeline front to back: server,
props, tracks (dress, export, previews, materials), client package, and
assembles a folder a player can unzip and run:

```powershell
./scripts/build_release.ps1 -Zip                     # artifacts/release/ApexSim-<ver>-Win64[.zip]
./scripts/build_release.ps1 -SkipClient -SkipTracks  # reuse what is already built
```

The circuits ship as data the game builds itself (see "Runtime tracks"
below): `Game/Tracks/` holds each one's `.uescene.json`, `.uemesh` and
preview `.png`, and the run aborts if a circuit has no export or the track
materials were never baked. A new circuit can be added to an installed
game by dropping those three files in `Game/Tracks` and its YAML (with the
sidecars) in `Server/content/tracks/default`. The cars ship the same way
(see "Cars" below): `Game/Cars/default/<folder>/` holds each car.toml with the GLBs
and logos it names and `Game/Wheels/` the class wheels; the run aborts if a
car.toml names a file that is not there or the car materials were never
baked. A new car is its folder in `Game/Cars/custom` and its car.toml in
`Server/content/cars/custom/<folder>`. `content/cars/custom` is shipped only
with `-IncludeCustomCars` (both scripts), like `-IncludeCustomTracks`.

Layout: `Game/` (the packaged client, `Tracks/`, `Cars/`, `Wheels/`, plus a `settings.sample.yml`), `Server/` (`apexsim-server.exe`,
`server.toml` and only the content the server reads � `car.toml` per car and
the track YAML, not the `.glb` models or `.ats` sidecars), plus `Play.bat`,
`Start-Server.bat`, `README.txt`, `LICENSE` and `release.json`.

The run aborts before any long build if the car or track data is missing, or if
a track YAML has no `track_id`. Every stage has a `-Skip*` switch for when one
piece is mid-refactor, but a skipped stage must still find the output it would
have produced � a `-SkipTracks` run checks every circuit has its export (and
the track materials are baked) rather than shipping a package that races in
an empty world.
`build_game_standalone.ps1` archives into `<dir>\Windows`, so the release
script points it straight at the release folder and renames that to `Game`
instead of copying 1.8 GB.

The Unreal Engine lookup shared by all three scripts lives in
`scripts/lib/ApexEngine.ps1`.

### Real-world layouts (`<Stem>.layout.json`, `ats-dress`)
The centerlines are real (public GPS traces), but everything beside the road
used to be invented: the procedural enrichment pass put grandstands wherever
there was room, guessed which side the pit lane went, gave no circuit its
landmarks and ringed every venue — dunes included — with the same tree belt.

A circuit's real furniture now comes from a **layout dossier** checked in
beside its YAML, `content/tracks/default/<Stem>.layout.json`: named corners, the
pit lane's real polyline and side, the grandstands with their names, sizes and
road-facing outlines, the buildings, what crosses over the road, landmarks,
and the outlines of the real woodland with its leaf type, plus the
*surroundings* layers added 2026-09-19: `barriers` (every `barrier=tyres`,
`wall`, `guard_rail` and `fence` run near the road), `roads`, `waterways`,
`areas` (car parks, camp sites, meadow, farmland, villages, scrub, water)
and `poi` (the statue, chapels, pylons, food, gates). Anything tagged
within 250 m that no rule claimed is counted in `unclassified`, so the next
circuit's gaps show up in the dossier rather than in a screenshot. It is
built from OpenStreetMap (ODbL; the attribution travels in the file) by

```bash
python scripts/osm_layout.py --all            # or: Monza Spa [--offline]
```

which georeferences the extract onto the track's own frame — an FFT
rotation/translation search, then trimmed ICP — and refuses to write a dossier
the fit does not explain (`fit` records the rmse and the coverage; every
circuit but Le Mans matches end to end, and there two thirds of the Sarthe is
public road in OSM rather than `highway=raceway`). Facts OSM lacks live in
`MANUAL_STANDS` / `MANUAL_CROSSINGS` / `MANUAL_LANDMARKS` in that script, from
each circuit's published seating map, and survive a refetch; a manual stand is
given as a station span and `outside`/`inside`, and the script lays its front
along the centerline so it curves with the bend. Raw extracts are cached,
gitignored, under `.cache/osm/`.

`ats-dress` turns a dossier into scenery and then grooms around it:

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
                                            # or: content/tracks/default/Spa.yaml [--dry-run]
```

It owns every prop of the kinds it lays (grandstand, building, attraction,
bridge, light, vehicle, sky) plus the pit lane, deleting and re-laying them
each run — so it also clears the old scatter — while barriers, tire walls,
distance boards and trees stay `groom`'s. It also owns, *by asset rather
than by kind* (`dress::dressed_prop`, `surroundings::OWNED`), the furniture
it lays from the new layers: cars and lamp posts in every mapped car park,
tents and campers on the camp sites, houses and barns across the villages
and farmyards, chapels and pylons from `poi`, a marshal post at every named
corner and every 400 m besides, a `corner_sign` carrying each corner's
`display_name` (never its real `name`; see "No trademarks on screen"),
and — only for a circuit whose dossier has no lighting masts of its
own — a ring of `floodlight_tower`s so a night session is not lit by
headlights alone. Ownership is by asset because these share kinds with
hand-placed props: a `sign` is also a distance board and a `misc` is also a
bollard.

Barriers are now **decided**, not inherited (`track-editor/core/src/barriers.rs`,
laid by `groom::lay_all_barriers`). The kind is decided per 4 m cell —
`barriers::decide` chooses from the dossier's mapped barriers first and
the geometry second: which side is the *outside* of the bend, how tight it
is, and how much run-off there is. That yields about 55% armco, 25% Tecpro
and 15% tyres at the Red Bull Ring, with `armco_4m_fence` wherever there
are people behind it, `concrete_4m_rail` where a mapped wall stands hard
against the road (a street circuit), and `armco_end` / `tecpro_corner` /
`tires_corner` / `concrete_end` closing every run. A corner's barrier
stands at least 14 m past the road edge and a straight's at least 7 m
(`CORNER_BARRIER_MIN_M`, `STRAIGHT_BARRIER_MIN_M`), further where run-off
is authored.

The **line** itself is a polyline, not one lateral per cell (2026-09-25).
The first version put a module at each cell's centre at that cell's
offset, and wherever the offset stepped — 7 m to 14 m sixty metres before
every corner, 7 m to 24 m where run-off began — neighbouring modules stood
metres apart sideways; on the outside of a tight bend 4 m of centerline
was more than 4 m of line; a grandstand swallowed the rail in front of it
(the groomer modelled a stand's slab centred on its pivot where every
other stage puts the pivot on the *front*: `groom::footprint_centre_of`,
`Slab`); the whole pit side of the pit straight was skipped; and any lamp
post or clump of grass within half a metre took a module out — each a
hole a car could leave the circuit through. Now the wanted offset is
smoothed along the lap at `BARRIER_GRADE` (0.25 m/m, dilated outward,
eroded toward a stand's front, held to the middle of the strip between
two close legs), the line is sampled every metre, `placeable` drops
points (clear of the pit lane except *behind* it, off any other leg's
verge, not in an underpass slot, not inside a stand or building — small
props no longer count), and the modules are laid **end to end along the
line by arc length**, each yawed to it, with caps a metre past each run's
ends and where two kinds meet. Behind the pit lane the line goes behind
the lane; the bake walls the road side of the lane wherever there is an
apron (`PIT_TAPER_WALL_MIN_M`), not only over the box span. The lower
road at an underpass gets its rail back wherever the ground is not
embanked (`UNDERPASS_STEP_M`). Measured by

```bash
python scripts/check_walls.py --openings --all      # -v lists the runs
```

which probes every 2 m of the lap for a ray from the road edge that meets
no wall within 45 m: 5 300 open probes over the calendar before, 384
after; Zandvoort 730 m of open edge to 2 m; stand runs with nothing in
front of them 91 to 18. The AI survey's off-road time fell with it, but a
continuous wall also *pins* a car the old holes let out: Oschersleben
occasionally shows one F1 car spending most of the survey against the
rail at 900 m (`SURVEY_TRACKS=Oschersleben SURVEY_DBG=1` finds it), and
the AI's recovery from a wall is the next thing to look at, not the wall.

The same pass hangs **hoardings** (`groom::lay_hoardings`): a
`board/hoarding_3m` behind every plain rail module on the pit straight,
through the braking zone into each corner and round its outside, and in
front of every grandstand, one brand per 36 m stretch, never on Tecpro or
tyres; about a quarter of the rail modules carry one. Owned by asset
(`HOARDING_ASSETS`), like the barriers.

Three traps are pinned by tests. Ownership is by asset, so the groomer must
recognise every asset it can lay or it doubles them on the next run. A
rule without the outside-of-bend signal puts Tecpro on both sides of every
bend, which gave eight Tecpro modules for each of armco. And distance is
not a detail: the old passes barely laid anything at a corner, so nobody
noticed that `wall_offset` falls back to a 6 m verge; laying the line at
every corner at that distance took the AI survey from 2 100 to 7 500
car-seconds off the road, most of it pinned against a rail. A stand is laid as 40 m runs along
its real front, each carrying its own `length_m`, so the Unreal side builds
bays that follow the outline; the family comes from the real depth and roof.
The pit lane is written with `authored: true`, which is what stops grooming
replacing it with a generated ribbon, and the Unreal bake generates the
garages, boxes and pit walls from it as before. Buildings inside the pit
complex are skipped for that reason.

Grooming is dossier-aware too (`groom_scene_with`, and `ats-groom` loads the
dossier by itself): dressed props are re-seated but never pushed, and tree
belts are planted **only inside the real woods**, with the species taken from
the wood's leaf type — which is what leaves Zandvoort's dunes bare, Spa's
Ardennes in spruce and the Parco di Monza in broadleaf. Both passes recycle
their own element ids, so re-running either writes a byte-identical file.

**AC survey overlays** (`scripts/ac_layout.py`, `scripts/ac_import/features.py`,
docs/AC_LAYOUT_SURVEY.md). Twelve circuits have an Assetto Corsa
counterpart (`content/tracks/ac_pairs.json`), and AC places their stands,
buildings, pit lane, bridges, masts and trees far better than OSM. The
tool surveys the AC layout (objects from the kn5 node tree, classified by
names, seated-crowd cards and a tiered-seating height profile; trees as
points; the pit spline trimmed to where it leaves the road), fits its
centerline onto the native one (`osm_layout.coarse_fit` + ICP: the frames
are 50-170 degrees and up to 300 m apart), then **rubber-sheets** every
point by the displacement of the native road beside it (Gaussian, sigma 12
m on the road to 60 m 120 m out, fading to nothing by 300 m), because the
two traces disagree by up to 10-20 m over whole stretches while agreeing
locally (Zandvoort's back section, Interlagos). Matching: a footprint
overlapping a dossier entry by 30% of the smaller takes AC's geometry and
keeps the dossier's name (an OSM grandstand stays a stand even when AC
calls it a building); AC-only entries are added, dossier-only ones kept
(the AC version may be older), manual (`MANUAL_*`, seating map, authored)
never replaced, footprints of very different size (under 1:5) left to the
dossier; woods are a union; the pit lane is replaced unless
`MANUAL_PIT_LANE` says otherwise, with AC's `AC_PIT_n` count as
`PitRoad::box_count` (capped by what the lane holds). Gates: rigid fit 40%
of the lap within 3 m, 90% after the field, scale within 0.5%, under 15%
reshaped. Output: `<Stem>.layout.ac.json` (checked in; positions and sizes
only, keyed to the centerline plan's CRC, so `osm_layout.py` re-applies it
after a refetch and drops it with a warning when the road has moved), the
merged `layout.json` with an `ac_survey` block that `--unapply` undoes
byte for byte, and `build/ac_layout/<Stem>/{report.json,overlay.png}`
(look at the picture before trusting a track). `--ac-root` or
`APEXSIM_AC_TRACKS` is the AC `content/tracks` folder; surveys are cached
under `.cache/ac_layout`. Tests: `python -m unittest
scripts/ac_import/tests/test_ac_layout.py` (a surveyed synthetic oval,
the fit and field on a rotated, shifted and bent trace, every merge rule).

Twenty-two of the twenty-six circuits have dossiers; a track without one is
groomed exactly as before.

### Real elevation (`<Stem>.dem.msgpack`, `dem_fetch.py`)

The ground used to be an inverse-distance average of the road's own
heights: a smooth blanket that decays to the mean track elevation, with no
hill or valley the road did not itself imply, ending 800 m out in fog and
then in a bare atmosphere gradient. `scripts/dem_fetch.py` fixes that from
the Copernicus GLO-30 elevation model (AWS open bucket, no credentials),
georeferenced onto the track frame by the *same* fit the dossier uses
(`osm_layout.fit_track`):

```bash
python scripts/dem_fetch.py --all         # or: Spielberg [--offline] [--dry-run]
```

It writes `content/tracks/default/<Stem>.dem.msgpack`: an `inner` grid at 10 m
over the circuit and a kilometre around it, and an `outer` grid at 90 m out
to eight kilometres, both in the server frame (the model is offset so it
agrees with the YAML's own z at the start/finish line). Unlike the other
sidecars this one is **checked in**, because regenerating it needs a few
hundred megabytes off the network; the raw tiles under
`.cache/dem/` are gitignored.

`terrain.rs` reads it (`TerrainHeightfield::from_paths_with_dem`) and
crossfades: within 40 m of a road the surveyed centerline wins, because a
30 m surface model reads the tree canopy and knows nothing of cuttings and
embankments, and past 220 m the model wins, because it is the only thing
that knows there is a hill there. The road-ceiling carve, the verge hold
and the 6–35 m blend are unchanged on top, so a car still follows the
rendered verge. `ue_export` then bakes the `outer` grid as a separate
`horizon` mesh — real geometry, not a painted backdrop, so the sun lights
it, it takes the weather's fog and it goes dark at dusk — with a hole cut
where the detailed ground already covers the land. At the Red Bull Ring
that is 71k triangles for 17 km of Murtal rising to 957 m, against a
previously flat horizon. A track with no sidecar bakes exactly as before.

The centerline's own `z` is a different matter: every real circuit's came
from invented keyframes in `enrich_all_tracks.py`, and since the surveyed
centerline wins near the road, the road, physics and AI drove that
profile through the real valley. Spa had a crest where the Eau Rouge dip
is (80 m out). `scripts/dem_elevation.py` re-derives it from the sidecar:
the median of the model across the road at each node, a Whittaker fit
along the lap (250 m half-power wavelength: ~700 m tightest vertical
radius) reweighted so samples more than 1.5 m above it count as canopy,
node 0 kept to the bit (the sidecar's datum), and the raceline carried by
the road's change. `--report` ranks every circuit by its disagreement:

```bash
python scripts/dem_elevation.py --report          # read-only
python scripts/dem_elevation.py Spa [--dry-run]   # rewrites node and raceline z only
```

Only Spa has been re-derived (2026-09-25); run it before `ats-smooth` in
the refresh order below. On a wooded circuit the model reads trees along
whole stretches, which no along-road filter can tell from a hill: look at
the fit before trusting `--report` there (Monza's park).

GLO-30 is a *surface* model and it is not square-posted: it keeps 1 arcsec
of latitude but decimates longitude by band, so a tile north of 50° is 2400
posts wide rather than 3600. Assuming square posts reads the ground
kilometres east of where it is.

### Smoothing a traced centerline (`ats-smooth`)

Every real circuit's YAML is a GPS trace resampled to 5 m nodes, and over a
5 m chord that trace's noise *is* curvature: Remus at the Red Bull Ring came
out at an 8 m radius on a 10.6 m road. The exporter's loft cannot draw that,
so it clamped offsets to `0.85/κ`, broke strips with under half a metre of
room and dropped inverted facets — corners shipped with holes in them.

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-smooth -- --all
                        # --report | --dry-run | --tolerance M | --tight-tol M | --min-radius M
```

`track_smooth.rs` filters the node positions with a Savitzky-Golay
quadratic applied by twicing, and separately re-walks each corner the trace
pinched: the turn at a node over the arc through it *is* the curvature, so
capping the turn and spilling the excess to its neighbours is a floor on
the radius that leaves the corner's total turn untouched. Each window is
pinned at both ends, so the change stays inside the corner. Two filters
were tried and rejected first and the module docstring says why: plain
Laplacian smoothing shrinks whatever it is run on, and Taubin has a gain
above one below its passband — both quietly rescale a surveyed circuit,
which the tests now catch. Across the calendar this takes the worst
curvature jump from 0.03–0.09 per metre to about 0.01 and leaves every
raceline inside the road edge. The raceline is carried by the displacement
of the road node beneath it rather than filtered on its own: filtered
independently it drifted two metres off the middle of the re-walked Ford
chicanes at Le Mans, and `ai_field_makes_the_first_lap_at_le_mans` caught
one car in four leaving the road. The pass also rewrites the stored
`metadata.length_m`, which the curb sidecar test checks against.

### Banking (`ats-bank`)

Positive `banking` lifts the road's **left** edge — the road mesh
(`track_path::offset_point`), the server's `surface_elevation` and, since
2026-09-23, the physics' gravity pull all agree — so a right-hander is banked
positive and a left-hander negative. The real circuits' banking came from
`enrich_all_tracks.py`, which laid every banked corner as a positive window
a tenth of a lap wide at a guessed lap fraction: Zandvoort's
Hugenholtzbocht (a left-hander) leaned out of the corner and the Arie
Luyendijkbocht's 18° started on the main straight after the corner.
`track_bank.rs` keeps each span's angle and re-lays it over the bend it
belongs to (the one it overlaps with the most turning, else the nearest
within 250 m), signed by the bend's hand, holding over the bend's core
(curvature ≥ 35% of its peak) with 30 m ramps; banking with no bend near it
is dropped.

```bash
cargo run --manifest-path track-editor/Cargo.toml --release --bin ats-bank -- content/tracks/default/Zandvoort.yaml
                                            # or --all [--dry-run]
```

Only Zandvoort has been re-laid so far; `--all --dry-run` lists what the
other circuits would get (Austin T1/T19, IMS, Suzuka and others are
inverted the same way). The physics used to add `+m g sin(bank)` on the
car's lateral axis — pushing it *up* the bank — and now pulls toward the
low edge, resolved against the car's heading
(`banking_pulls_the_car_toward_the_low_edge`).

**Refresh order.** Everything downstream is derived from the centerline,
so a change to it has to flow through in this order, and running a step
out of order produces data that is internally inconsistent:

```bash
python scripts/dem_elevation.py <Stem>                                        # elevation (from the checked-in DEM)
cargo run --manifest-path track-editor/Cargo.toml --bin ats-smooth -- --all   # centerline
cargo run --manifest-path track-editor/Cargo.toml --bin ats-bank -- --all     # banking onto its bends
python scripts/drs_zones.py --all                                             # DRS zones onto the corners
python scripts/osm_layout.py --all --offline                                  # dossiers (fit to the centerline; re-applies the AC overlays)
python scripts/ac_layout.py --all                                             # AC overlays, only where the centerline's plan moved
python scripts/dem_fetch.py --all --offline                                   # elevation (same fit, same datum)
python scripts/track_location.py --all                                        # altitude and position from the DEM
./scripts/build_track_levels.ps1                                              # dress, export, import
```

### DRS (`drs_zones.py`, `server/src/drs.rs`)

Each track YAML carries `drs_zones` (detection, activation and end
stations), written by `scripts/drs_zones.py` from the FIA event notes of
the circuit's last DRS season. The notes say "95 m before turn 7"; the
script detects the corner runs from the centerline and each entry in its
`ZONES` table names the turn by an approximate station, so a zone snaps
onto the corner it belongs to and survives `ats-smooth`
(`--report <Stem>` prints the detected corners to author against; the DTM
circuits get one zone on the pit straight, Le Mans and IMS none). Both
`TrackFile`s keep the key through every rewrite.

The server runs the rule once per tick, before the physics
(`GameSession::update_drs`): crossing a detection line arms the zone —
in a race only within `DRS_GAP_S` (1 s) of a car ahead on the road, in
practice and the hotlap always — the flap may open between activation and
end, the brake shuts it (`DRS_BRAKE_CLOSE`), and nothing opens in the
rain, on the first lap of a race or in the garage. An F1 car has
`DrsSpec::F1` (12% of the drag and 25% of the rear downforce off) unless
its `[physics]` table says `drs_drag_reduction` /
`drs_rear_downforce_reduction`; any other class has none. `PlayerInput.drs`
(optional; an old client never opens it) is the button, the AI asks
whenever `CarState::drs_allowed`, telemetry carries allowed/open as
`lap_flags` bits 3 and 4, and the racing line's profile uses the flap-open
drag inside the zones. The bake paints a line across the road at each
detection and activation station with a `corner_sign` board on each side
(`DRS DETECTION` / `DRS`). On the client: the `Drs` input action (Left
Shift, gamepad X, a wheel slot), `FApexCarTelemetry::bDrsAllowed/bDrsOpen`,
and a badge on the HUD rev counter (dark / lit / green when open). The F1
cars' upper rear-wing flap is its own mesh (`[drs_flap]` in car.toml,
`FApexDrsFlapSpec` on the catalog row) and swings open on the car while
`bDrsOpen` is set (docs/CAR_MODELS.md, DRS flap). Golden
bytes: `cargo test player_input_drs_wire_format -- --nocapture`.

### The Nordschleife, circuit styles and road decals (docs/NORDSCHLEIFE.md)

The 20.8 km Nordschleife has no GPS trace, so its centerline is routed over
OSM's raceway ways by `scripts/osm_centerline.py` (waypoints in `LAPS`, the
start offset and the Karussell banking in the spec too); the OSM extract
was cut from the planet file because the map API is unreachable from the
cloud sessions. `scripts/seed_scene.py` starts a new circuit's `.ats`
(start line, grass, curbs at every apex). Per-circuit grooming rules live
in `track-editor/core/src/circuit_style.rs`, keyed by the track stem:
the Nordschleife gets German guard rail (`vangrail_*`) 3 / 4.5 m off the
road and no Tecpro, a denser forest from 8.5 m, German signs (chevrons,
km boards, `de_*`) instead of braking boards and hoardings, no floodlight
ring and no pit lane; every other circuit is `CircuitStyle::DEFAULT`, the
old rules. Road graffiti is an `.ats` `decals` layer (`graffiti/<name>`
PNGs from `scripts/content/props/gen_graffiti.py`), laid by `ats-dress`
from the dossier's `graffiti` (`MANUAL_GRAFFITI`), baked road-hugging by
`ats-export` (family `decal`) and drawn with a masked `M_ApexDecal`
(`ApexPropImport -kind=decal` imports the textures). The kit pieces are
built by `scripts/content/props/build_nordschleife_kit.py` (headless
`bpy` works: run it with `python -I`).

### Run-off (`Surface::paint`, `RoadContact::Runoff`)

The curb sidecar is version 2: beside the curb width it carries how far
the `asphalt_runoff` / `concrete` bands reach past each edge
(`CurbBands::runoff_at`), and physics classes a point past the curb but
within it as `RoadContact::Runoff` — asphalt at `RUNOFF_GRIP_FACTOR` of
the road's grip and none of the grass drag, but **off the track for the
lap**: track limits are judged per wheel (`WheelState::off_track`), and
the feedback surface reads the run-off as road, because it is smooth and
the client's force feedback would otherwise rumble on it. A tarmac band
may carry a paint style (`red_yellow`, `blue_white`, `blue_red`,
`red_white`, `green_white`); the bake lays stripes parallel to the road
across it, and `ats-dress` gives every corner's tarmac run-off the
circuit's style from `dress::RUNOFF_PAINT` (Spa, Yas Marina, Bahrain).

The dossier and the elevation sidecar are both fitted to the centerline,
and a hand-authored pit lane is laid along it (`edge_run`), so either one
built against an earlier centerline describes a road that has moved.
`build_track_levels.ps1` runs only the last line; the first three are data
refreshes, run when the source data changes.

A hand-authored `MANUAL_PIT_LANE` now wins over OSM outright. It used to be
a fallback reached only when no OSM way qualified, and rebuilt from the
current Albert Park extract the untagged-way fallback matched a 711 m
"pit lane" running across the race track; the bake then stood pit walls in
the middle of the road and the AI spent a quarter of every race against
them. Likewise `tourism=artwork` only becomes a `statue` landmark where
`MANUAL_LANDMARKS` declares one: the kit's one statue mesh is a bull, and
every sculpture near every circuit would otherwise have become one.

**Checking a change.** `cargo test` in `track-editor` runs both the unit
tests and the integration tests under `core/tests/`, which groom every real
circuit — run the whole thing, not `--lib`. The two tests that bake every
circuit (`every_real_track_bakes`, `real_tracks_bake_plausible_curb_bands`)
take about twenty minutes and are `#[ignore]`d; run them with
`cargo test -p track-core --test ue_export -- --ignored` before shipping a
change to the bake. Anything that moves
barriers, walls, the centerline or the ground should also go through the
server's AI survey against a baseline, because the AI is sensitive to all
of it and the per-circuit assertions only cover Le Mans and Spa:

```bash
cargo test --release --test ai_race_start_test survey_ai_races_on_every_circuit -- --ignored --nocapture
```

### Track pipeline into Unreal
Circuits reach the Unreal client as exports the game builds at runtime (see
"Runtime tracks" below); both generated steps are regenerated wholesale and
neither output should be hand-edited.

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
                                                         # -> content/tracks/default/*.ats
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
                                                         # -> build/tracks/*.{uescene.json,uemesh} (gitignored)
python scripts/build_track_catalog.py                   # -> build/tracks/previews/*.png
```

`scripts/build_track_levels.ps1` runs all three, then bakes the shared track
materials (`ApexMaterialBake`, the missing ones; `-SkipMaterials` keeps the
engine out of it), optionally building the `ApexSimEditor` target first
(`-Build`) and importing the prop kit (`-ImportProps`) or the circuits as
editor-only levels to inspect (`-ImportLevels`). The dressing stage is new and is there
because the dossier-to-scene step used to be manual and silent: the Red
Bull Ring's bull statue went into the dossier, nobody re-ran `ats-dress`,
and the level was baked from the previous scene. Dressing is idempotent, so
running it every time costs seconds and removes the failure mode
(`-SkipDress` if you are editing a scene by hand). and finds the engine install from the
`.uproject`'s `EngineAssociation`; `-Track A,B` narrows it to a few circuits,
`-DryRun` reports without writing assets. Note that `ats-export` resolves
`content/tracks/{default,custom,export}` relative to the working directory, so it must be
run from the repo root — not from `track-editor/`.

The exporter also writes five gitignored sidecars (like the exports) into
`content/tracks/default/`, all loaded by the server from beside the YAML and
all shipped in `Server/` by `build_release.ps1`. A track whose sidecars
another tool wrote (an importer that measured the real road) lists them in
its `.ats` as `"external_sidecars": ["road", "walls", ...]`, and every
export, `--all` included, leaves those alone (`ats-export --keep-sidecars
LIST` does the same for one run). `scripts/seed_scene.py --from
survey.json` likewise seeds an `.ats` with measured curbs and bands rather
than guessed ones:

- `<Stem>.ground.msgpack` — a 4 m heightfield of the ground the client
  renders, in the server frame (`ground.rs`), so a car that leaves the
  asphalt follows the rendered verge and terrain instead of holding road
  height. Without it, off-track elevation falls back to the centerline.
- `<Stem>.curbs.msgpack` — how far the curbs reach past each road edge,
  one sample per metre of centerline station (`curbs.rs`), and (version
  2) how far the tarmac run-off reaches. The curbs are
  authored in the `.ats` and reach the server only as this number: physics
  counts a car within the band as on the track, with `curb_grip` and no
  off-track drag; past it but on tarmac the car is off the track for the
  lap yet still on asphalt. Without it the road edge is the track limit and
  a driver using the curbs is slowed as if on grass.
- `<Stem>.walls.msgpack` — the barriers as the sim needs them
  (`walls.rs`): every armco, tire wall, fence and pit wall as a line
  segment along its heading, every stand, building, garage and fairground
  piece as the four sides of its footprint, plus an underpass's abutment
  walls and deck parapets, each with the ground height at its base, its
  height and a material (armco / tires / concrete). Runs of thin walls
  with ends under 4.5 m apart are joined so a car cannot slip between two
  modules. `physics::check_wall_collisions` runs after the car-car pass
  every tick: the car's box against each nearby segment (uniform 16 m
  grid, ascending index order for determinism) with the same SAT as
  car-car, pushed out along the wall normal, bounced by the material's
  restitution, scrubbed by its friction, spun by the impulse's moment
  about the deepest corner, damaged and fed back like a car-car hit; a
  car resting on the wall grinds along it. A wall only counts when the
  car's height band overlaps it, which is what keeps a car on Suzuka's
  deck off the abutment walls below (written a metre short of the upper
  ground) and a car in the slot off the parapets. Without it nothing
  stops a car off the road: the client has no physics, cars are telemetry
  puppets, so barrier collision lives here and nowhere else.
- `<Stem>.road.msgpack` — the road as triangles (`road_mesh.rs`,
  docs/ROAD_MESH.md): the rendered road, curb, run-off and ground band
  and pit-lane strips, welded and without their render lifts, each
  triangle tagged with a surface (contact class, a grip multiplier that
  is 1.0 on every generated mesh, inside the track limits or not). Read
  only when `[physics] road_contact = "mesh"`; then each of the six
  surface queries a tick (`physics::query_track_surface`, given the
  querying point's height) takes its height, its normal (as slope and
  banking) and its contact class from the highest triangle under the
  point at or below `z_ref + STEP_UP_M`, so two levels of road work
  (Suzuka's crossover) and the curbs are 5 cm high in physics as on
  screen; wherever the mesh has nothing (past its outermost band, a
  hole, the clamped inside of a hairpin) the centerline sample stands.
  Everything keyed on a station or a centerline index — progress, laps,
  sectors, checkpoints, the AI, the racing line, the grid — stays on the
  centerline under either backend. The grid and the hotlap run-up are
  seated on the mesh (`physics::seat_height`). The pit lane is a surface
  of its own (`RoadContact::PitLane`: road grip, off the track for the
  lap), which it never was on the centerline. `ats-export --flat-curbs`
  bakes the curbs flat into the mesh for a like-for-like comparison.
  Tests: `track-core/tests/road_mesh.rs` (coverage and agreement with
  the rendered road on the test circuit;
  `every_real_track_road_mesh_covers_the_road` is `#[ignore]`d with the
  other whole-calendar bakes), `server/tests/road_mesh_test.rs` (the
  mesh against the server's own centerline on Monza, skipped until the
  sidecar is baked), the physics surface tests under both backends,
  `determinism_test` on the mesh, and `physics_step_monza_mesh` /
  `session_tick_8cars_monza_mesh` in the bench. `SURVEY_ROAD_CONTACT=mesh`
  runs the AI survey on the meshes.

- `<Stem>.pit.msgpack` — the pit lane and its boxes (`ue_export::
  PitSidecar`): the lane's centerline, the limit lines, a stop spot per
  box, where it leaves and rejoins the track. Without it a circuit has no
  pit stops (see "Tyre wear, compounds and pit stops").

Where the course passes over itself with at least 4 m to spare (Suzuka's
crossover) the terrain finds an `Underpass` (`terrain.rs`): behind wall lines
3.5 m past the lower road's edges the ground is the upper road's embankment,
so the lower road runs through a slot. The heightfield cannot draw that
vertical step, so `ue_export` cuts the ground grid along the walls and draws
the slot floor, abutment walls and the deck (parapets, yellow fascia; material
family `structure`) itself; ground-anchored strips of the upper road sit on the
deck or are dropped over the slot, and `ats-groom` lays no armco there. The
ground sidecar reports the lower level under the bridge, and the server holds
road height within 2.5 m of an edge when the ground there is a level away (the
deck). A heightfield still cannot hold both levels, so a car that leaves the
bridge past the parapet falls into the slot.

The exporter resolves `.ats` station spans against the YAML centerline and
bakes triangles (Unreal can't read YAML); the `ApexTrackEditor` module's
commandlet turns those buffers into static meshes, materials and a level per
track. The export is two files: `<Stem>.uescene.json`, a manifest of
everything small (materials, props, grid, centerline, and a header per
mesh; `version` 2, with the YAML's `source_crc`), and `<Stem>.uemesh`, a
little-endian blob of every mesh's buffers, zlib per mesh (Spa: ~2 MB + 15
MB). See `track-editor/TRACK_EDITOR.md` §5 for the format and the
coordinate/winding conventions; the reader still takes a version 1 file.

### Assetto Corsa track import (`scripts/ac_import.py`, docs/AC_TRACK_IMPORT.md)

A track from the player's own AC install becomes a complete ApexSim track
in one command: AC's geometry drawn by the client, AC's physics mesh driven
on by the server. Local only; no AC geometry, textures or physics data is
bundled (the dossier overlays below are positions and sizes only), and a
CSP-encrypted kn5 is refused, never decrypted.

```powershell
python scripts/ac_import.py "E:\SteamLibrary\steamapps\common\assettocorsa\content\tracks\ks_zandvoort"
python scripts/ac_import.py <folder> --list                 # the layouts
python scripts/ac_import.py <folder> --layout layout_gp     # one of a multi-layout track
python scripts/ac_import.py --all <ac>\content\tracks       # a whole collection, one line each
#   --stem, --display-name, --textures kit|ac|flat, --max-texture N, --force, --dry-run
python -m unittest discover -s scripts/ac_import/tests      # the importer's tests
# then restart the server, and restart the game or run apexsim.track.Rescan
```

It writes the **server's** files into `content/tracks/custom/`
(`<Stem>.yaml`, a minimal `<Stem>.ats`, the four sidecars, and
`<Stem>.import.json`: every file read with its CRC, the material
classification, the checks, the warnings and the exact command that
rebuilds it) and the **client's** into `build/tracks/` (`<Stem>.uescene.json`
+ `<Stem>.uemesh` in export format version 3, `<Stem>.textures/*.dds`, and
`previews/<Stem>.png` from AC's own preview). Re-running writes
byte-identical files. The stem defaults to CamelCase of folder and layout
(`KsRedBullRing_LayoutGp`), `track_id` is a UUID v5 of folder and layout, and
a stem that exists under `default/` is refused.

How it reads AC (`scripts/ac_import/`: `kn5.py`, `ai.py`, `ini.py`,
`frame.py`, `trigrid.py`, `physics.py`, `centerline.py`, `scene.py`,
`textures.py`, `export.py`, `sidecars.py`, `validate.py`, `cli.py`):

- **Frame.** AC is Y-up; checked against `ui_track.json`'s `run` on every
  Kunos circuit, a clockwise lap traces a positive signed area in AC's raw
  `(x, z)`, so the conversion is the pure rotation `(X, Y, Z) = (x, -z, y)`,
  no mirror, and the physics mesh's authored index order already faces up.
  The origin is the midpoint of `AC_TIME_0_L/R`, seated on the physics road,
  with +X along the AI line's direction there.
- **The `.ai` files** carry an `i32 extra_count` between the points and the
  18-float records (the "shifted by one float" of the feasibility study);
  `side_left`/`side_right` are only a fallback.
- **Physics** is every mesh named `NN<KEY>` (any number of digits: the
  Nordschleife's last 2 km is `100TRM-NRM`…) across *all* the layout's kn5s,
  as AC itself finds them (Kunos keep them in a non-renderable file, mods
  often draw them): the longest `surfaces.ini` KEY the name starts with,
  `WALL` walls. Contact classes from the key (`KERB/CURB` curb; `GRASS/
  SAND/GRAVEL/...` off; `IS_PITLANE` pit lane; an invalid surface with
  friction >= 0.85 run-off; else road) and `IS_VALID_TRACK` as the track
  limits. AC's absolute friction becomes the server's multiplier:
  `(f / road_f) / class_grip`, so a kerb at 0.96 on 0.98 asphalt ends up at
  exactly that ratio after the server's own `curb_grip`. Walls are the steep
  faces of the WALL meshes as segments (kind from the name: tyres, concrete,
  else armco); the ground sidecar is the highest physics surface on a 4 m
  grid, holes filled from their neighbours; the curbs sidecar is measured.
- **Centerline.** The AI line, rolled to start at the start line and
  resampled to 5 m nodes, then at every metre a cross-section across the
  physics mesh: the run of road-class triangles around the line gives the
  two edges, its middle is the node (the AI line is moved to the road's
  centre), the kerb and run-off runs beyond the edges are the curb sidecar,
  and z and banking come from the mesh. Sectors from `AC_TIME_1/2` (a
  two-sector AC track gets a synthetic boundary halfway to the finish),
  grid slots from `AC_START_n` as `spawn_points` with `position: 0` and
  offsets from node 0 (the road's middle at the gate's foot on the AI
  line, not the gate's middle: pinning it there put a kink in Spa's
  raceline the profile braked for), DRS from `drs_zones.ini` re-based from
  the AI start. The middle is smoothed along the lap (quantised to the
  0.25 m section step it zig-zagged into a 100 m radius on straights).
- **Scene.** Every renderable kn5 mesh at `lodIn == 0` (lower LODs, `AC_*`
  logic objects, crews, `GROOVE` overlays by mesh or see-through
  material name dropped, and any material AC draws at `alpha = 0`: how
  mods hide renderable physics meshes), classified **per
  material**: what physics lies under its meshes' triangle centres decides
  (on the road -> `road_ac`, family road, so the wet look applies; on
  grass/sand/gravel -> `ac_<set>`, family surface with `ground_set`; never
  sampled at vertices, which on a road ribbon all sit on the grass edge),
  else a grass/sand/gravel/road *name*, and only then a plain
  `ksMultilayer` shader, is kit terrain (`ksMultilayer_fresnel*` is road,
  `_objsp` an object; shader first drew Spa's `grass-ext-shad` valley as
  asphalt), a `KERB`/`CURB` name never,
  else an AC-textured `scenery_<material>` material on the **car parents**
  (opaque / masked / translucent by the kn5's alpha flags and shader, two
  sided, roughness from `ksSpecularEXP`). Alpha-tested or blended
  materials are never kit (they are the paint and the tree cards lying on
  a surface), and **kerbs keep AC's textures** on purpose: their stripes
  are authored into the texture and the kit's would need an along-kerb UV
  the kn5 lacks. Kit surfaces get world-metre planar UVs. Meshes merge by
  (material, 250 m cell, draw-distance bucket, collision) into a few
  hundred draw calls; `lodOut` becomes the mesh's draw distance; only
  meshes standing on physics carry collision (what the racing line and
  the cameras trace).
- **Textures.** Only the diffuse maps of the scenery materials. Most Kunos
  DDS ship without mips (196 of Zandvoort's 293), so a BC1/BC3 source with
  a full chain is copied verbatim (top mips dropped over `--max-texture`),
  everything else is decoded with Pillow, resized, box-mipped and
  range-fit encoded to BC1 (opaque) or BC3 (alpha) in numpy. The client
  streams the blocks straight into transient textures (`ApexDdsReader`).
- **Checks** (in the report, and a failed one is exit code 1): every grid
  slot on the road mesh, road coverage every metre edge to edge, the AI
  line on the road, wall openings (reported, never failed), and the
  triangle / draw-call / texture budget. `server/tests/imported_track_test.rs`
  loads every imported track through the loader with all four sidecars
  (`#[ignore]`d, since the imports are the player's own data: `cargo test
  --release --test imported_track_test -- --ignored`; the debug-build test
  servers skip imports via `[content] skip_imported_tracks`);
  the AI survey takes a custom stem (`SURVEY_TRACKS=KsZandvoort`).

**The `imported` marker.** The importer's `.ats` carries `"imported": "ac"`
(`AtsScene::imported`, `ats_io::imported_marker`), and everything that
would rewrite a track's files leaves such a track alone: `ats-export` refuses
it (`UeExportError::Imported`; `--all` walks past it), `ats-dress`,
`ats-groom`, `ats-smooth` and `ats-bank` skip it, `build_track_levels.ps1
-Track` drops it with the rebuild command, `initialize_content.ps1` never
bakes it and reports one whose export or road mesh is missing with the
command from its report (`Test-ApexImportedTrack`,
`Get-ApexImportedTrackRebuild` in `scripts/lib/ApexTracks.ps1`;
`track_dirs.imported_marker` in Python), and `build_track_catalog.py` keeps
its preview. `build_game_standalone.ps1` / `build_release.ps1
-IncludeCustomTracks` ship `<Stem>.textures/` beside the export.

**Export format version 3** (`TRACK_EDITOR.md` section 5; the version 2
reader stays and the Rust exporter still writes 2 unless a scene uses the
new fields): a material may carry `ground_set`, or `texture` (a DDS
relative to the manifest), `blend`, `two_sided`, `roughness`,
`alpha_cutoff` (family `scenery`); a mesh header `draw_distance_m` and
`collision: false`; the manifest an `imported` scalar ahead of the first
array. On the client `FApexTrackSceneReader` (`kSupportedVersion` 3) parses
the textures with the scene on the worker thread into
`FApexTrackScene::Textures` (a bad one is logged and its material draws
flat), `FApexTrackSceneBuilder` draws `scenery` on `/Game/Materials/Car`'s
opaque / masked / translucent parents (`FApexTrackParents::Scenery*`; flat
colours on the base when they are not baked), honours `ground_set`
(`ApexGround::LookForSet`), sets `SetCullDistance` from the draw distance
and skips the collision component where `bCollision` is false.
`ApexSim.Track.Dds.Parse` and `ApexSim.Track.Reader.Version3` pin it; the
Rust side `a_version_3_manifest_carries_the_imported_extensions` and
`an_imported_track_is_not_baked_over`.

Out of scope for this version, as the design says: tracks without
`fast_lane.ai`, point-to-point stages (refused), kit props in place of AC
scenery, night lighting from AC's lights, and cars.

### Assetto Corsa car import (`scripts/ac_car_import.py`, docs/AC_CAR_IMPORT.md)

A car from the player's own AC install becomes a custom car in one command,
local only like the tracks (a CSP-encrypted kn5 is refused):

```powershell
python scripts/ac_car_import.py "E:\SteamLibrary\steamapps\common\assettocorsa\content\cars\ks_porsche_911_gt3_r_2016"
python scripts/ac_car_import.py <folder> --list      # skins, compounds, parts, LODs
python scripts/ac_car_import.py --all <ac>\content\cars [--dry-run]
#   --stem, --class, --skin, --compound, --cylinders, --keep-steering-wheel, --force
python -m unittest discover -s scripts/ac_car_import/tests
```

It writes `content/cars/custom/<Stem>/`: a car.toml marked `imported =
"ac"` (id a UUID v5 of the AC folder; every figure commented with its AC
key), the body GLB seated with its tyres on y = 0, `wheels/front.glb` and
`rear.glb` (car-local wheels), `steering_wheel.glb` for the cockpit rig,
every other AC skin as a texture `[[livery]]` under `skins/`, and
`<Stem>.import.json`. Re-running writes byte-identical files. The package
(`scripts/ac_car_import/`) decrypts `data.acd` (`acd.py`: the key is a
hash of the folder name, so a renamed folder is refused) and shares the
track importer's kn5 reader, whose meshes and dummies carry `path` (their
ancestors' names) for the part split. Traps it settled: `DRIVEREYES` is in
the model frame, the aero positions are from the CG; `wheel_rake_deg` is
negative for a real car (the rig's positive pitch lifts the column's
forward end); `wheel_lock_deg` is `STEER_LOCK` itself (centre to lock);
turbo `GAMMA` is pedal sensitivity, not an rpm exponent. The server's newer
tables are mapped too: the tyre window from `[THERMAL_FRONT]`'s
`PERFORMANCE_CURVE`, blankets for a modern F1, and the `[aero]`
ride-height map from the wings' `LUT_GH_CL` tables, the lift coefficients
taken at the posture the car rides at 50 m/s (the server's reference); a
heave spring adds half its rate to each corner. Re-run an import with
`--force` after an importer change. Checks after an
import: `cargo test --release --test imported_car_test -- --ignored
--nocapture`, `cargo test --release --test car_stability_test imported --
--ignored`, `PROBE_CLASS=GT3 cargo test --release --test grip_probe_test
silverstone_profile_lap_times -- --ignored --nocapture`, and on the client
`ApexSim.Cars.Glb.RepoCars` and `ApexSim.Cars.TomlRepoCars`. Not done: a
DRS flap is not split off the body yet.

### Runtime tracks (`UApexTrackContentSubsystem`, `UApexTrackInstance`, docs/RUNTIME_CONTENT_LOADING.md)

Every circuit is built by the running game from its export; **there are no
cooked track levels**. The generation pipeline is unchanged up to the export
(`osm_layout.py` … `ats-dress` → `ats-export` → `build_track_catalog.py`
previews), and a changed circuit is playable as soon as it is re-exported:
restart the game or run `apexsim.track.Rescan`. The builder is
`FApexTrackSceneBuilder` (`ApexSim/Track/`, runtime module), behind an
asset factory: the game's makes dynamic material instances and fast-built
transient meshes straight into the menu world. The editor-only
`ApexTrackImport` runs the same builder with a factory that saves packages
and a level, to look at a circuit in the editor
(`build_track_levels.ps1 -ImportLevels`); the game never loads those
levels and `/Game/Tracks` is in `DirectoriesToNeverCook`. The reader,
`ApexPropLibrary` and `ApexGroundMaterials` live in `ApexSim/Track/`.

- **Materials** are the only track content that is cooked:
  `/Game/Materials/Track/{M_ApexTrackBase,M_ApexEmissive,M_ApexBrand,M_ApexDecal}`,
  baked by `-run=ApexMaterialBake [-force]` (`ApexTrackMaterialGraphs`;
  `build_track_levels.ps1` runs it for the missing ones, and it bakes the
  base again when `/Game/Ground` has been imported since). The start-light
  lenses use the emissive parent itself. Without them a track draws in flat
  colours and says so in the log.
- **Where tracks are found** (`UApexTrackContentSubsystem`, at startup;
  `apexsim.track.Rescan`): `-ApexTracksDir=<dir>[+<dir>]`, then `Tracks/`
  beside `ApexSim.exe` in a package (`build_game_standalone.ps1` and
  `build_release.ps1` copy the exports there) or the repo's
  `build/tracks` in the editor. Each manifest's head (the first
  64 KB: every field ahead of `materials`) becomes the catalog row for its
  `track_id`, with `SourceCrc` from the export and `RuntimePreview` from
  `<Stem>.png` (beside it or under `previews/`).
  `UApexMenuFlowSubsystem::FindTrackRow` returns it before any
  `DT_TrackCatalog` row, which is now only a fallback for a track with no
  export on the machine; previews are read through
  `UApexTrackContentSubsystem::PreviewOf`.
- **Loading** (`UApexTrackInstance`, a tickable UObject the subsystem hands
  out with `Acquire` and takes back with `Release`): the manifest and blob
  are read and the mesh descriptions filled (with tangents) on a worker
  thread, then materials, then meshes within `apexsim.track.BuildBudgetMs`
  (20) per frame, then the actors in one frame; the track counts as loaded
  once every surface's collision has cooked. A build that fails is handed
  back by the director (`DropFailedTrack`) and the circuit marked broken
  until the next rescan, so nothing waits on it. The last track released
  is kept hidden and handed back when the same circuit is asked for next
  (the demo, then the player's race on it; `apexsim.track.KeepLast 0` turns
  it off), with every material reset to how it was built.
- **Collision**: a mesh built in a cooked game cannot cook its own, so each
  track surface (not the decals) carries a `UApexTrackCollisionComponent`
  beside its mesh (ProcMesh-style: its own body setup, the triangles through
  `IInterface_CollisionDataProvider`, cooked async by Chaos); generated
  stand-in props keep a simple box. Only traces use it: the racing line's
  snap and the cameras' ground and line-of-sight tests. Hiding a track
  turns its collision off, and showing it rebuilds any body a cook finished
  into while it was hidden.
- **Tags**: every track surface actor carries `ApexTrackMesh` (the racing
  line's `IsRoadSurface` requires it), every prop `ApexProp`, the gantry
  `ApexStartLights` with lenses `ApexStartLight`. The director's conditions
  pass walks `UApexTrackInstance::GetActors`.
- **Not yet**: runtime meshes have no mesh distance fields, so software
  Lumen and DF shadows see the road and terrain less well than the old
  cooked levels did (risk 1 in the doc); worth an A/B against an
  `-ImportLevels` level in the editor. Cars are runtime-built too (see "Cars").

`ApexSim.Track.Reader.*` tests pin the blob layout against the exporter's
`the_mesh_blob_layout_is_pinned` (one golden 155-byte blob in both), the
header read and the rejects.

The track picker's names, metadata and preview art come from each track's
export, keyed by `track_id` (the wire protocol only carries id and name): the
manifest carries the name, metadata and checksum, and `build_track_catalog.py`
draws `previews/<Stem>.png`. The `DT_TrackCatalog` data table (synced by the
optional `-run=ApexTrackCatalogSync` from the same script's
`track_catalog.json`, which refreshes `SourceCrc`, `DisplayName` and
`Description` even on an additive run) is only a fallback for a track with
no export. Every
track YAML needs a fixed `track_id` — without one the server mints a new UUID
per start and no catalog row can ever match it.

### No trademarks on screen (`display_name`, `CORNER_DISPLAY`, `ApexCatalog::DisplayClass`)

The real circuit, corner and series names are mostly registered
trademarks (the operators', sponsors', famous drivers'), so the source data
keeps them and the product never shows them. Rule: **a real name lives in
`name`, what the player sees lives beside it.**

- **Tracks.** A YAML's `name` is the real one (`Circuit Zandvoort`);
  `display_name` is a sound-alike that still says which circuit it is
  (`Zandervoort`, `Spa-Frankenchamps`, `Red Pull Ring`, `Lemons – Circuit du
  Peuple`). The server sends `display_name` as the track's name
  (`TrackFileFormat::display_name`, falling back to `name`), so the lobby,
  sessions, replays, the export manifest (`track_display_name`, which the
  runtime catalog row prefers over `track_name`) and `DT_TrackCatalog`
  (`build_track_catalog.py`) all carry the parody. `metadata.description` says what it is modelled on by
  place ("Modelled on the circuit in the dunes at Zandvoort, Netherlands.";
  shown in the track picker, `FApexTrackCatalogRow::Description`). Both
  `TrackFile`s keep the key through rewrites. Stems stay the place names and
  `-ApexTrack=` takes the stem. Table: `content/tracks/default/README.md`.
- **Corners.** Each dossier corner has `name` (OSM's, what `apexsim-replay
  find --corner` and `drs_zones.py` look up) and `display_name`, written by
  `osm_layout.py` from `CORNER_DISPLAY`: a sound-alike for a sponsor or a
  person (`Würth Kurve` -> `Wurst-Kurve`, `S do Senna` -> `S do Sonho`),
  `null` for a way that is not a corner (the circuit's own name, a
  connector), the real name for a place (Eau Rouge). `ats-dress` lays a
  `corner_sign` only from `display_name`, so a corner without one gets no
  board. `python scripts/osm_layout.py --all --names-only` re-applies the
  table to the checked-in dossiers without the OSM cache; a refetch that
  finds a new corner name needs a look at the table.
- **Classes.** Car classes and track categories stay `F1` / `WEC` / `DTM` /
  `IndyCar` in `car.toml` and the YAML (the logic keys on `F1`), and every
  screen shows them through `ApexCatalog::DisplayClass`: Formula, Endurance,
  GT3, Independent (`ApexSim.Content.DisplayClass`).
- **Promo captions** (`scripts/promo/shots.yml`) use the display names too.

### Ground textures (`content/textures/ground`, `ApexGroundTexImport`)
The track surfaces sample baked tiling maps rather than the engine's 64-texel
noise tile. Seven sets (asphalt, grass, gravel, sand, concrete, astroturf,
kerb), each a 1024^2 colour, normal and roughness map tileable over 2 m:

```bash
python scripts/bake_ground_textures.py              # -> content/textures/ground/*.png (gitignored)
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexGroundTexImport                        # -> /Game/Ground/T_ground_<set>_{col,nrm,rough}
```

then re-bake the materials and levels (`-run=ApexMaterialBake`, then
`scripts/build_track_levels.ps1`; the import re-bakes the base material by
itself when it finds the ground sets newly imported). The generators
are numpy in `scripts/content/props/apex_tex.py` beside the prop kit's (its
`bpy` import is optional so the baker runs under plain Python). Colour maps
are normalised to a per-channel mean of 0.5 and the material doubles them,
so the exporter's per-key colour still decides a surface's hue — a map with
its own hue would tint twice. `apex_tex.GROUND_TILE_M` and
`ApexGround::TextureTileM` must agree. The importer fixes each map's class
by suffix (sRGB colour, BC5 normal, BC4 roughness: `TC_Alpha`, not
`TC_Grayscale`, which cooks to uncompressed G8), because a material
instance can only swap a texture for one of the sampler type the parent was
compiled with.

`M_ApexTrackBase` (shared, `/Game/Materials/Track`, `ApexMaterialBake`) is
generated in one of two shapes, chosen at bake time: with `/Game/Ground` imported, each map is sampled at the
instance's tiling and at 0.371 of it, mixed by a 20 m world-space noise so
the repeat does not show, pushed to the coarse sample and a flat normal by
200 m so the far field does not shimmer; without it, the old noise-tile
grain (a fresh clone, until the two commands above are run). Which set each
family and `surface_<kind>` key samples is `ApexGround::LookFor`
(`ApexGroundMaterials.h`, `ApexSim.Track.Ground.*` tests).

Seams: a run-off band is its own mesh beside the road, so grass met
asphalt as a line between two flat colours. The builder now writes a ramp
into each band's red vertex channel — 0 at the road-facing edge, 1 by
2.5 m in (`ApexGround::EdgeFactors`) — and the material frays it with its
macro noise toward a dusty tone. One-sided on purpose: fringing the outer
edge too drew a dust ring round the circuit where the band meets the
terrain. The road-facing edge is found against the centerline, because the
exporter merges both sides' bands into one mesh per section and orders each
profile right to left, so `v` starts at the inner edge on the left and the
outer edge on the right. It is a fringe, not a blend: the surfaces still
meet where they met, and a true blend would need the bands and the ground
to share a mesh in the exporter. The import logs the fringe's share of band
vertices (about 8% at Spielberg); zero or everything means the edge test
broke.

A ground band (the 130 m grass apron, run-off, gravel) stops halfway
between its own road edge and any *other* road — another leg of the
course or the pit lane (`ue_export::band_reach`). Past 40 m the apron's
columns are 10 m apart, and where another section ran within reach one
quad spanned it verge to verge, a plane the road's crown, camber and
banking poked through: "terrain over the track" at Zandvoort (1 450 m² of
grass above the asphalt, up to 1.3 m, before; none after). The reach is
worked out per cross-section and then limited to change by at most 0.5 m
per metre of course (`band_reach_profile`), because the columns are
fractions of the width and a width that jumps strings quads diagonally
across the very road it was capped for. Within 200 m of an underpass the
band keeps its full width: the slot and deck logic is built on the old
columns.

Stands and buildings are laid by their whole kit footprint, not their
front: `dress::clear_footprint` / the building row check probe the bay or
block from its front back to its full depth against every section of the
course and step it back (≤ 25 m) or leave it out. A building between two
legs (Zandvoort's Hunserug, one each at Melbourne and Silverstone) had its
back on the far road.

The terrain grid (`ue_export.rs`, `Bake::ground`) is 2 m within 60 m of a
centerline, 6 m to 200 m and 12 m beyond; the near radius has to clear the
verge blend (`BLEND_END_M` past the road edge) or the resolutions crack.

Where two roads run close at different heights the **nearest road owns
its verge** (`terrain.rs`, `DOMINANCE_M`). Zandvoort's pit exit runs a
few metres from the high side of the Hugenholtz banking, five metres
below it; both weighed the same inside their verges and the lower road's
"never bury a road" ceiling capped the ground under the higher road's
edge, so the grass band, the curb strip and the server's ground sidecar
all read a verge five metres down and the banked road stood on a drop. A
road's pull and its ceiling now fade out over 2 m past the nearest road's
edge (a road in an underpass slot keeps its slot), and a point straight
off the end of the pit lane polyline no longer counts as on it.
`the_verge_meets_the_road_edge_on_every_real_circuit` walks every lap
edge at 0.5 m out (worst left: Austin's flat pit entry on a banked
stretch, under a metre). The dressed pit lane is seated on its *own*
leg's road edge (`dress::nearest_cross_section_near`), not the banked
surface of another leg extrapolated to it, which had four of Zandvoort's
exit-road nodes 7 m in the air.

### Cars (`content/cars`, `UApexCarContentSubsystem`, docs/RUNTIME_CONTENT_LOADING.md)
A car is `content/cars/default/<folder>/car.toml` (a shipped car; the
player's own, imported or hand-made, go in `content/cars/custom/<folder>`,
gitignored but for its README) plus the GLB its `model` names, and,
like a track, **the game builds it from those files**; nothing about a car is
cooked (`/Game/Cars` is in `DirectoriesToNeverCook`). `UApexCarContentSubsystem`
(an engine subsystem, `ApexSim/Cars/`) reads every car.toml on first use
(`ApexCarToml::Parse`, the parser `ApexCarImport` used to own) into an
`FApexCarCatalogRow` keyed by the TOML's `id` (the wire only carries id and
name), and builds a GLB into a transient mesh the first time something draws
it: `ApexGlb` (its own glTF reader: node tree, triangles, PNG/JPEG images with
mips, block compressed at load to BC1/BC3 by `ApexBc` because the engine's
encoders are editor-only and BGRA8 put 50 MB per car in video memory;
identical images across GLBs are one texture; glTF `(x, y, z)` m -> Unreal
`(x, z, y)` cm, Interchange's frame, which also turns glTF's front face into
Unreal's), the tracks' fast mesh build, and dynamic instances of four cooked
parents under `/Game/Materials/Car` (`ApexMaterialBake`: opaque, clear coat,
masked, translucent) with Interchange's parameter names (`BaseColorFactor`, `BaseColorTexture`,
`MetallicFactor`, `RoughnessFactor`, `EmissiveFactor`), so the livery, ghost
and brake-light code is unchanged. Folders: `-ApexCarsDir=`, then `Cars/`
beside `ApexSim.exe` in a package (wheels in `Wheels/`), else the repo's
`content/cars` (wheels in `content/wheels`); each is read as `default/` then
`custom/` (`UApexCarContentSubsystem::CarFolders`; a folder with neither is
read as it is), and the server does the same (`car_loader::car_toml_paths`),
so a custom car reusing a shipped `id` is skipped with a warning. A folder
name must be unique across both (meshes and `/Game/Cars/<folder>` are keyed
by it; `Test-ApexCars` flags a clash). Edit a car, re-export, then
`apexsim.car.Rescan` or restart. The race director `Prefetch`es the roster's
GLBs on the thread pool before it dresses the cars.

A row carries file paths (`RuntimeModel`, `Wheels.RuntimeModel`,
`DrsFlap.RuntimeModel`, each livery's `RuntimeLogo`) beside the old soft
pointers; everything that draws a car goes through `ApexCarContent::LoadBody`
/ `LoadMesh` / `LoadLogo`, which take the file when there is one. A runtime
body's slot instances are shared by every car wearing it, so a car's own
instance (lights, livery, ghost tint) must come from
`ApexCarContent::OwnMaterialInstance`, never `CreateDynamicMaterialInstance`
(which returns the shared one and repaints every such car), and never a
dynamic instance parented to the shared one (the engine refuses a dynamic
instance as a parent and the child draws untextured): it is made from the
shared one's cooked parent with its values copied.

`DT_CarCatalog` is now a fallback for a car with no folder on this machine,
and the source of hand-tuned turntable framing and cockpit points for a car
whose car.toml has no `[preview]` table, or leaves a `[cockpit]` key out
(docs/CAR_MODELS.md).
`ApexCarImport` still imports a car as `/Game/Cars/<folder>/SM_<folder>` and
refreshes its row, for looking at it in the editor:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexCarImport -all          # or -car=yotota-lmp2 / -list / -remove=folder / -force / -dryrun
```

`AApexRaceCarActor` turns the mesh −90° about Z, so a car must be long along
its local Y (`ApexSim.Cars.Glb.RepoCars` checks every GLB in the repo).
`ApexSim.Cars.Toml*` and `ApexSim.Cars.Glb.*` test the TOML scan and the reader.

The body GLBs have no wheels: the client draws four copies of the class's
shared wheel (`content/wheels/<class>.glb`) where the car.toml's `[wheels]`
table puts them, steers the front pair and rolls all four from the telemetry
(`Race/ApexCarWheels.h`, row field `Wheels`; docs/CAR_MODELS.md).

Liveries: a car.toml's `[[livery]]` tables (written by `scripts/content/cars/liveries.py`) become the row's
`Liveries` (logo PNGs loaded at runtime). The pick travels as `SelectCar.livery`
-> `RosterEntry.Livery` (0 = the model as authored; AI dealt in turn per model), and
`ApexLivery::Apply` (`Race/ApexCarLivery.h`) repaints `car_paint`/`car_accent`/`car_logo` on the race
car and the garage turntable. docs/CAR_MODELS.md, Liveries.

### Content checksums (`content_crc.rs`, `ApexContentCrc.h`)
The client races on a level baked from the track YAML and shows a mesh
imported beside a `car.toml`, while the server simulates from those files
themselves, so each file carries one checksum computed the same way in three
places: the CRC-32 of zlib/PNG over the file's bytes with every carriage
return dropped (so `core.autocrlf` cannot split the two sides; check vector
`"123456789"` → `0xCBF43926`). The server computes it as it loads
(`CarConfig::content_crc`, `TrackConfig::content_crc`) and sends it as
`ContentCrc` in the lobby's car and track summaries; `SessionSummary` now also
carries `TrackId` so a client can find the session's track without matching
names. On the client `ats-export` writes the YAML's `source_crc` into the
track's manifest, which becomes the catalog row's `SourceCrc`;
the car row's `SourceCrc` is hashed from the car.toml the game read the car
from (`UApexCarContentSubsystem`; `ApexCarImport` does the same for a table
row). When the race
director loads a track, and when it spawns
the local player's car, `UApexMenuFlowSubsystem::VerifyTrackContent` /
`VerifyCarContent` compare row against server: a mismatch is a warning in the
log and a toast (`OnContentMismatch` → the root widget); a demo session only
logs; a side with no checksum (an old server, a row from before the field) is
"unknown" and logged once, never toasted. The first `LobbyState` also lists
every stale row at once (`ReportUnmatchedCatalogIds`). A row imported before
the field has `SourceCrc = 0`: re-run the sync or import to fill it in.
`ApexSim.Content.*` tests pin the check vector, the line-ending rule and the
compare semantics; `content_crc::tests` and `test_lobby_summaries_carry_content_crc`
do the same on the server.

### Prop kit (`content/props`, `ApexPropImport`)
The trackside scenery is an authored kit of GLBs, `content/props/<kind>/<asset>.glb`
(catalogue and conventions in `docs/PROPS.md`; the loader's design in
`docs/PROPS_LOADER.md`). It reaches Unreal once, not per track:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexPropImport -all          # or -kind=barrier,board / -asset=barrier/armco_4m
./scripts/build_track_levels.ps1 -ImportProps   # the same, before the bake + import
```

`ApexPropImportCommandlet` runs each GLB through Interchange (the engine's
glTF stack, adjusted: one combined mesh, authored normals kept, Nanite on
`grandstand/building/pit/bridge/attraction`, box collision on all but trees
and the sky) into `/Game/Props/<kind>/SM_<asset>`, with the materials and
textures under `/Game/Props/<kind>/Materials` shared by name across the kind
(the first GLB to bring `armco_galv` in owns it). Material slot names are the
glTF material names, which is what everything below keys on. The ferris
wheel imports as `SM_ferris_wheel` plus `SM_ferris_wheel_rotor` (the `rotor`
node, pivot at the hub). The loose PNGs become textures: brands and
markers as `/Game/Props/board/Brands/T_brand_<name>` and
`Markers/T_marker_<n>`, the flags (`sign/flags/<cc>.png`) as
`/Game/Props/sign/Flags/T_flag_<cc>`; each set comes along with its kind.
Every run is a fresh import (the target mesh package is deleted from disk
first and the registry rescanned): reimporting over an existing mesh keeps
its old slots and creates no materials. A kind-wide run clears the kind's
material folder first. The glTF materials come in as instances of the
engine's glTF library parent (importing them as full materials trips an
engine error on the library's `AlphaMode` input); that parent has neither
the Nanite nor the instanced-mesh usage flag, which the editor sets on the
fly but a cooked build does not, so each instance is re-parented onto a
flagged copy under `/Game/Props/_Parents`. The track parents
(`M_ApexTrackBase`, `M_ApexEmissive`, `M_ApexBrand`) carry the flags too.
`build_release.ps1` has a props stage (`-SkipProps` still checks
`/Game/Props` holds meshes).

Frames: glTF (x, y, z) lands in Unreal as (x, z, y), so a prop modelled in
Blender with the road on **-Y** arrives with the road on local **+Y** — the
same side the generated recipes use, and no extra yaw is applied to
authored assets. The exporter's yaw is the road heading; the builder flips
face-road kinds 180° by `RoadSideOf` as before. The two boards a driver
reads on the way in — `board/braking_marker` and `board/light_panel` — are
the exception (`ApexProps::FacesUpCourse`): their authored +Y front is
yawed +90° to look back up the course at the cars instead of across it at
the crowd, and they are never flipped by side.

The track builder (`ApexTrackAssetBuilder::ResolveProp`, data in
`ApexPropLibrary`) resolves every prop as `SM_<asset>` → the kind's default
(`armco_4m`, `tires_4m`, `hoarding_3m`, `mesh_4m`, `bay_10m`, `garage_6m`,
`start_gantry`, `floodlight_tower`, `broadleaf_m`, `blimp`, `marshal_post`,
`car_a`, `tent_6m`, `clubhouse`, `bollard`; only `cone` has none) → the generated
recipe for the prop's own kind → placeholder cube, so levels still build with
`/Game/Props` empty. Buildings face the road like the stands (every
authored one has a front); of the attractions the video screen, the stage
and the tent do, the ferris wheel, camera tower and portaloos do not;
vehicles are left as placed. An alias table maps the pre-kit keys the groomer and
scenes carry (`tree_generic`, `armco_generic`, `tire_wall_generic`,
`sign/board_200m|100m|50m` → `board/braking_marker` with the number as text,
`grandstand_main|corner`, `building/pit_garage`) — data, so the `.ats` files
are untouched. Instanced kinds go into one HISM per resolved (kind, asset,
text), actor `Props_<kind>_<asset>[_<text>]`; every prop actor is tagged
`ApexProp`, which the racing line's ground snap and the TV camera's ground
trace use to ignore bridge decks and garage roofs.

- **Text → material**: a `text` naming a brand (`piretti`, `rolux`, …)
  overrides the `board_brand` / `bridge_brand*` / `pit_team_board` /
  `tyre_bridge_brand` / `blimp_brand` / `balloon_envelope` slots with an
  instance of the shared `M_ApexBrand` showing `T_brand_<text>`; a
  braking marker's `board_marker` slot takes `T_marker_<text>`; a flag
  pole's `flag_cloth` takes `T_flag_<text>` (`nl`, `de`, … `chequer`).
  Unknown text keeps the imported default and logs once per track.
  `led_panel` / `led_screen` / `pit_light_green` slots get a lit
  `M_ApexEmissive` instance (`pit_light_red` a dark one: the pit exit shows
  open) and the component a tag `ApexEmissive_<slot>` (fixed glow: there is
  no flag state on the wire yet).
- **Dressing**: the `.ats` carries a scene-wide `dressing { season,
  spectators }` (`summer`/`autumn`, default summer with spectators),
  exported as-is; the builder swaps meshes, not keys: with spectators every
  stand module becomes its `_crowd` twin (`ApexProps::CrowdVariant`; the
  caps have none), in autumn the broadleaf trees, poplars and bushes become
  `_autumn` (`AutumnVariant`; conifers stay). A missing variant falls back
  to the base mesh. The editor has a Dressing panel and the MCP a
  `set_dressing` tool.
- **Grandstands**: one prop is a stand of `round(length_m × scale / 10)`
  bays (`length_m` from the `.ats`, 30 m when absent; scale is a length
  multiplier so the legacy 30 m × 2.6 stands come out as eight bays) plus
  two end caps, under one `Grandstand_<n>` actor with a HISM per asset.
  The exporter writes `radius_m` (signed bend radius, positive on the
  outside) and the builder picks the wedge whose front radius is nearest:
  `_curve6` (95 m), `_curve12` (48 m), `_curve6_in` inside; the large
  family is straight only. `scaffold_10m` and `banking_seats` are not bay
  families: the module is tiled at 10 m, straight, with no caps.
  `ApexProps::LayoutGrandstand` is the pure maths.
- **Bridges**: never flipped or pushed; local Y scaled by `span_m / 15`
  (`span_m` = road width at the station from the exporter, else measured
  from the centerline). The start lights use `bridge/SM_start_gantry` as the
  `StartLights` root when it exists, with the five `ApexStartLight` lens
  components over the panel's upper lamp row (x −37, y (i−2)·80, z 545 cm)
  so the race director's countdown contract is unchanged; the recipe gantry
  is the fallback.
- **Pit complex**: `ats-export` generates it from the pit lane — one
  `pit/garage_6m` per box on the far side of the parallel pit road (6 m
  pitch, centred) with a `pit/box_kit` on the same pivot (it reaches 2.6 m
  out of the door onto the working lane), `garage_end` beyond each end, `pit_wall_6m` on the road
  side over the box span and `pit_wall_plain_6m` over the rest of the pit
  road, all facing the track — and drops the legacy `building/pit_garage`
  stand-ins whenever it does. The plain walls carry on along the rest of
  the lane, tapers included, wherever there is an apron of
  `PIT_TAPER_WALL_MIN_M` between the two roads; the paint adds a
  speed-limit line across the lane at each end of the box span, a
  working-lane outline per box and a blend line along the road for 120 m
  past the exit.
- **Runtime**: `sky` props spawn as `AApexSkyDriftActor` (drift along the
  heading ±150 m at 2 m/s, a slow yaw sway); the ferris wheel as
  `AApexRotorActor`, its rotor at the hub turning at 0.5 rpm about local Y
  (`Race/ApexPropActors.h`).

New `PropKind`s `board, fence, pit, bridge, vehicle, attraction, sky` exist
in `ats.rs`; the groomer snaps a `board` onto the nearest barrier run, lays a
`fence` 1.5 m behind the wall line, seats a `bridge` at road height without
pushing it, leaves `pit` and `sky` alone and pushes `vehicle`/`attraction` by
their footprint. The editor's copy of the kit is `track-editor/core/src/props.rs`
(`props::KIT`: kind, key and authored footprint per asset, default first,
plus `resolve` for the legacy keys): the inspector offers the kind's keys
in a dropdown (a free-text `key` row stays for anything else), a new prop
starts as the kind's default, the stand-ins are drawn at the asset's size,
and the groomer pushes by that footprint. The MCP's `list_prop_assets`
lists it. `ApexSim.Props.*` automation tests cover the alias table, the
kind tables, the variants and the bay layout; `cargo test` in
`track-editor` covers the catalogue (every default is a kit file), the
export hints, the pit complex and the groom behaviours.

### Marketing page (`site/`, `scripts/site/`, `docs/index.html`)

GitHub Pages serves `docs/` from `main`, and `docs/index.html` with
`docs/assets/` is **generated**: never edit them. The source is `site/`
(`site/README.md`), in three kinds of content refreshed three ways:

```powershell
python scripts/site/build_site.py              # -> docs/index.html, docs/assets
python scripts/site/build_site.py --check      # CI (job `site`): exit 1 when docs/ is behind
python scripts/site/build_site.py --stale      # feature-doc sections changed since the copy was reviewed
python scripts/site/build_site.py --reviewed   # record them as covered
python scripts/site/make_shots.py [id ...]     # the in-engine pictures (opens the game)
python -m unittest discover -s scripts/site/tests
```

- **Facts** (`scripts/site/facts.py`): the car cards from every
  `content/cars/default/*/car.toml`, the circuits from the track YAMLs
  (display name, metadata, an SVG outline of the centerline, the dossier's
  corner display names) and the counts the copy quotes (`n.cars`,
  `n.tracks`, `n.km`, `n.setup_knobs` from the server's `KNOB_COUNT`,
  `n.hud_components`...). Read on every build, so a change to a car.toml, a
  track YAML or a car render makes `--check` fail until `docs/` is rebuilt
  and committed. Display names only: `facts.display_class` mirrors
  `ApexCatalog::DisplayClass`.
- **Words** (`site/copy.yml`): hand-written, every string a Jinja template
  over the facts (`{{ n.cars|words }}`, `{{ name('Spa') }}`,
  `{{ car('posh-gt3rs') }}`), so no number is typed. The build cannot know
  when prose is stale; it hashes every section of CLAUDE.md and
  docs/SIMULATION_GAPS.md into `site/copy.lock.json` and `--stale` lists
  the ones changed since `--reviewed` (a warning in CI, not a failure). The
  `/site-refresh` command (`.claude/commands/`) is the rewrite pass.
- **Media** (`site/media.yml`): each picture's source file in the repo and
  how to cut it (`width`, `aspect`, `focus`), encoded to WebP;
  `docs/assets/manifest.json` records the hash of the source each was made
  from, which is what `--check` compares (two WebP encoders need not agree
  on bytes). An entry's `zoom: px` adds `<name>-zoom.webp`, the uncropped
  frame at up to that width, which a click on the picture opens in a
  `<dialog>` (`site.js`; any `img[data-zoom]`, arrows step through its
  gallery, the file is fetched on the click). The car pictures are the Blender previews,
  `content/props/_preview/cars/<folder>_hero.png`; the page names an asset
  only through `asset()`, which fails the build on one `media.yml` lacks.

**In-engine pictures** (`site/shots.yml` -> `site/shots/<id>.webp`, checked
in): *action* shots reuse the promo pipeline (`make_clips.Planner` and
`render`): a seeded headless AI race, the moment found by `window`, played
with `-ApexReplay` under the TV director or a tripod, no HUD; `frames`
candidates are kept in `out/site/candidates/<id>/` with a `sheet.jpg`, and
the shot's `pick` names the one used (the race replays bit for bit, so a
retake after a visual change is the same moment). *ui* shots are live runs
(`-ApexAutoRace`..., `-ApexScreenshotAfter`), with `r.SetRes` because
settings.yml's borderless mode otherwise makes the grab the monitor's size,
`DisableAllScreenMessages`, and the game killed once the grabs are on disk
(it does not quit by itself). Both keep the player's own cars out: the
races are dealt from `cars_dir: content/cars/default` (a `defaults` / race
key of the promo shot list too) and the ui shots' server is started with
`APEXSIM_CONTENT_CARS_DIR` pointing there, because an AC import carries a
real team's colours. A fixed tripod (`mode: pan` with an `eye`) can end up
inside a stand after a re-dress; the TV director's `trackside` traces for
visibility and does not. A ui run rewrites the profile's "continue where
you left off" car, track and mode.

## Architecture

### Server (`server/`)
- **240Hz authoritative physics loop** using tokio async runtime
- **TCP+TLS** for auth, lobby, session management, rosters (port 9000). TLS is fail-closed by default (`require_tls = true`); dev opt-out in server.toml
- **UDP** (port 9001) for telemetry out / player input in, bound per connection via `UdpHandshake` with the token issued in `AuthSuccess`; telemetry broadcast rate is `tick_rate / network.telemetry_divisor` (default 60Hz)
- **HTTP endpoints** on port 9002: `/health`, `/ready`, `/metrics` (Prometheus text format)
- Key modules:
  - `server.rs` — `run_server()` entry point, `ServerState`, `ServerHandle` (used by binary and tests)
  - `game_loop/` — the tick orchestrator: `dispatch.rs` (message handlers), `tick.rs` (session ticking + panic boundary), `broadcast.rs` (telemetry fan-out, serialized once per session), `lifecycle.rs` (unified disconnect)
  - `transport.rs` — TCP/TLS/UDP IO, token auth, per-connection rate limiting, backpressure with priority-based drops
  - `physics.rs` — 4-wheel 3D vehicle model (per-wheel loads, Pacejka-style tires, suspension), yaw-aware OBB collision (SAT), windowed nearest-centerline search cached per car
  - `curbs.rs` — baked curb widths per station; the sim's track limits
  - `racing_line.rs` - the racing-line driving aid: a per-car speed profile along the raceline (throttle / partial / brake per point), sent to the joining player as `RacingLine`
  - `game_session.rs` — session/game-mode state machine, `lobby.rs` — matchmaking (single-lock), `metrics.rs`, `config.rs`, `car_loader.rs`, `track_loader.rs` (adaptive-density Catmull-Rom spline)
  - `track_content.rs` — tracks load lazily: startup parses every YAML in parallel (`TrackLoader::load_catalog_entry`, all the lobby needs) and the sidecars (ground, curbs, walls, pit, road mesh: ~600 MB across the calendar) load when the first session on a track is created (`load_sidecars`), then stay cached. `game_loop/track_loads.rs` does that load on a blocking task and holds the connection's `CreateSession`, and everything it sends after, until it is done, so running sessions never stall and a client's messages keep their order (`tests/track_content_test.rs`). A debug server starts in under a second instead of ~20 s

### Unreal client networking (`game-unreal/Source/ApexSimNet/`)
- Hand-written MessagePack codec matching Rust `rmp_serde` (named/`to_vec_named`) format — wire-format changes must be coordinated between server and client, and are pinned by golden bytes (`ApexGoldenBlobs.h`, `ApexUdpGoldenBlobs.h`) printed by the server's `network.rs` tests
- Network protocol: `[4-byte big-endian length][MessagePack data]` over TCP; telemetry/input over UDP after the handshake
- `ApexTcpConnection` / `ApexUdpConnection` receive on background threads; `UApexNetSubsystem` processes the messages on the game thread

### Unreal client input (`game-unreal/Source/ApexSim/`)
Driving uses Enhanced Input, with actions and the mapping context built in
C++ (`Input/ApexInputConfig.h`) rather than as `.uasset`s, so bindings are
readable in a diff. `AApexPlayerController` owns them and adds the mapping
context only while a race is running. Defaults: WASD to drive, Q/E to shift,
C to step through the cameras, `,`/`.` to look aside, B to look behind, L to
switch the headlights, H to flash them (D-pad up/down on a pad; both have a
wheel slot), Escape to leave.

Three traps worth remembering: the menu shell runs in `FInputModeUIOnly`, where
the viewport discards game input entirely and no binding produces an event
(the controller switches to game-and-UI for the race); a Blueprint game
mode can silently override `PlayerControllerClass`, which the C++ game mode
now logs an error about; and game-and-UI does not move focus by itself, so
the controller names the viewport as the widget to focus and
`FApexMenuInputProcessor` puts focus back on it whenever an event arrives while
driving. Focus left in the shell routes events through the focusable
`WBP_Root`, whose default Slate handler eats the left stick, D-pad and arrows
as menu navigation - pad steering died while throttle and shoulders worked.

### Race HUD (`content/hud`, `Hud/`, docs/HUD_MODDING.md)

The HUD is content, not code. Each panel is a component folder,
`content/hud/default/<id>/component.json` (`track_info`, `race_state`,
`status`, `minimap`, `standings`, `timing`, `pedals`, `damage`, `car_state`,
`mirror`): JSON with comments and trailing commas, a `region` of nine
(`top-left` ... `bottom-right`; a band's components sit side by side by
`order`, a `float` one alone at the corner) and a tree of elements (`row`,
`column`, `stack`, `panel`, `text`, `label`, `rect`, `bar`, `spacer`,
`divider`, `keycap`, `image`, `minimap`, `mirror`), with `repeat` over a
count (`index`) or a list (`item.<field>`, `max`, `focus`). Any attribute
starting with `=` is an expression, `text` takes `{expr}` holes
(`Hud/ApexHudExpression.h`: literals, data names, arithmetic, comparisons,
`?:`, `fmt_time`/`fmt_gap`/`ramp`/`mix`/`switch`...; compiled at load,
side-effect free). `content/hud/custom/<id>` (gitignored but for its README)
replaces a default of the same id, `{ "enabled": false }` hides one;
`-ApexHudDir=` adds directories on top. A package carries `Hud/default` and
an empty `Hud/custom` beside `ApexSim.exe` (`build_game_standalone.ps1
-SkipHud` to leave it out).

What the components read is `FApexHudData`, built every frame by
`ApexHudData::Build` from `FApexHudInputs` (gathered by
`UApexHudDataSubsystem` from the net, flow and settings subsystems; pure, so
tests feed it a synthetic frame). The derived state the old monolithic widget
kept (the delta's reference lap, fuel per lap, damage flashes, the rev scale)
is `FApexHudMemory`. **Every name is set on every path** (null via `SetNone`
when unknown) and **documented in docs/HUD_MODDING.md**: a new data point is one
`Out.Set` there plus a line in the doc, or `ApexSim.Hud.Data.Stable` /
`.Documented` fail. `UApexHudWidget` is only the host: it builds the tree once
per load and applies changed values per frame; `apexsim.hud.Reload` rereads
the folders, `apexsim.hud.Data [filter]` prints every data point. Load errors
drop the component and show on screen; warnings (unknown keys, a name the game
does not publish) go to the log, and `ApexSim.Hud.Shipped` fails on either for
the shipped set. Lua was considered and not used (the doc's last section says
why): logic belongs in a data point.

**The HUD editor** (Settings > Gameplay > HUD layout, `apexsim.hud.Edit`,
`UI/ApexHudEditorWidget`) moves, resizes, adds and removes panels with the
mouse, keys or pad and saves `FApexHudLayout` (`Hud/ApexHudLayout.h`) to
`<first HUD dir>/custom/layout.json`: per component `enabled`, an `anchor`
(0-1 each axis) + `position` (1080p units from that point of the screen) when
pinned, and `scale` (0.5-2). Moving anything first pins every panel where it
is (`UApexHudWidget::PinAll`, nearest of nine anchors by thirds); snapping to
gutters, centre lines and other panels is `ApexHudPlace::Snap`. Components
with `"default_enabled": false` (`relative`, `speed_gear`, `conditions`) ship
off and are added from the editor. Outside a race the editor shows
`FApexHudPreview`, a made-up race fed through the same `Build`. Settings is
hidden, not closed, while it runs; the input processor stands back for it
like it does for settings. Trap: the HUD keeps one `HostRoot` and swaps its
content on rebuild, because a user widget never rereads
`WidgetTree->RootWidget` after its Slate widgets exist (replacing it froze the
old tree on screen). Unattended: `-ApexOpenHudEditor=N
-ApexHudEditorSteps="select standings;move 500 -350;scale 0.25;save"`;
`grab standings;dragby 300 -200;release` and `grab car_state corner` drive the
real mouse path with synthesised Slate events, run from the world timer
because a widget's tick is inside paint, where the hit-test grid is half built
(a click synthesised there fell through to the menu screen).

### Racing line (`Race/ApexRacingLineActor`)
Gameplay settings -> Racing line: OFF / BRAKING ONLY / FULL (default off).
The server works the line out per car (`server/src/racing_line.rs`: the
track's raceline, or its centerline, with a quasi-steady-state speed profile
from the car's grip, downforce, power and brakes) and sends it as
`RacingLine` right after `SessionJoined`. `AApexRacingLineActor` draws it as
dots on the road, one instanced mesh per colour (green flat out, amber at the
grip limit or lifting, red braking), dropped onto the track's own road
meshes (tag `ApexTrackMesh`) by line traces once the track is built and
visible. BRAKING ONLY draws just the red.
For screenshot runs `-ApexRacingLine=off|braking|full` overrides the setting
and `-ApexCar=<name>` picks the auto-race car (otherwise the lobby's first).

### Car motion on the client (`Race/ApexCarMotion.h`)
Cars are puppets of the telemetry; there is no client physics. Each frame's
sample goes into the car actor's `ApexMotion::FApexCarMotionBuffer`, and
every render frame reads the pose from a *playhead* that runs two telemetry
frames (33 ms at 60 Hz) behind the newest sample, blended between the
samples either side: location lerp, rotation slerp (so the ±180° yaw seam is
crossed the short way), and steering, speed and revs blended the same way
for the cockpit wheel and the dials. The playhead's clock is in **server
ticks**, which are exact where arrival times are not; its ticks-per-second
is a least-squares fit of tick against arrival time over the last two
seconds of frames (240 assumed until a second has been seen; a surprise of
more than 10% re-seats the playhead once), trimmed by a critically damped
loop (0.4 s error filter, 1.6 s correction) that holds the delay. Past the
newest sample the pose is dead-reckoned from the last two for up to 250 ms
(position and turn rate), then held; a jump of more than 20 m between
samples, or a tick that runs back more than a second, restarts the buffer
at the new place. The previous `VInterpTo` chase lagged by speed/18 (over
4 m at 80 m/s) and pumped at the broadcast rate whenever frames arrived in
lumps. `apexsim.car.InterpDelayFrames`, `apexsim.car.InterpMaxExtrapolationMs`,
`apexsim.car.InterpDebug 1` (per-car readout: buffered, measured tick rate,
spacing, lag, extrapolating). `ApexSim.Motion.*` tests run the buffer on
synthetic streams (lumpy arrivals, lost frames, a stall, a 120 Hz server).

### Race start and finish (`game_session.rs`, `UApexRootWidget`)
A car is seeded onto the centerline when it is put on the grid
(`physics::seed_track_progress`); lap 1 starts as a grid car crosses the line,
or on green for pole. The server classifies a car (`finish_position`) when it
completes `lap_limit` laps; once the winner is in, the session finishes when
every car is classified or at a deadline of max(60 s, 2 x the winner's average
lap). A finished human's car is driven by a server cool-down AI, because the
last input received otherwise stays applied. `start_countdown_mode(_, Race)`
lines the field up on the grid again. On the client the HUD ranks finishers
first (`ApexRace::RanksAhead`); cars still racing get a toast when P1 takes
the flag, and 2.5 s after the local car finishes the root widget swaps the race
view for a provisional `SessionResults` that re-sorts until the session ends.

### Lap timing: sectors, track limits and records (`laps.rs`, `records.rs`)

The stopwatch is the server's. The lap *counter* stays in
`physics::update_track_progress_3d` (it hangs off the anti-shortcut
checkpoints), but everything a timing screen shows lives in `laps.rs`, which
that function now returns a `LapEvent` from.

**Sectors** are three per lap, split at stations along the centerline: the
track file's own `sectors` (node indices, resolved at load like the
checkpoints) when it names two, otherwise even thirds. A crossing is timed at
the full tick rate and sent as a reliable `ServerMessage::LapTiming`
(`car_index`, lap, sector, sector time, the lap time when it closes the lap,
validity, and flags for personal/session best lap and sector). The boundaries
themselves go out once with the racing line as `TrackSectors`, so the client
can place a car in a sector from the station telemetry already carries. The
final split is what the other two leave of the lap, so the three always add up
to the clock the driver saw.

**Track limits** strike a lap when all four wheels are off the track — past
the curb band, since `curbs.rs` already calls the curbs track — for
`laps::TRACK_LIMITS_SECONDS` (0.2 s; one tick of it at 240 Hz is a wheel
skimming a kerb edge). Physics sets `CarState::wheels_off_track` from the same
per-wheel surfaces the force feedback uses. A struck lap is still timed and
still counts as a lap; it just cannot become a best, and reaching the line
without every checkpoint (a cut course) strikes it too. Telemetry carries the
verdict as a `lap_flags` byte appended **after** `is_colliding` in
`CompactCarState` — positional encoding, so a field added at the end is one an
older client skips rather than one that shifts everything after it — along
with `last_lap_time_ms` and `best_lap_time_ms`, which the client no longer has
to infer from watching the lap counter roll over. Note the AI is sloppy enough
at Monza that most of its laps are struck; `best_lap_time_ms` is `None` for
those cars, which is why `race_flow_test` measures plausibility on the last
lap rather than the best.

**Records** (`records.rs`, `[records]` in server.toml) are the best legal lap
each driver has ever set on a track in a car, kept in `records/lap_records.json`
across restarts and keyed by the driver's *name* — the player id is minted per
connection, so a UUID key would forget every record on reconnect. AI never
sets one. The driver is told theirs on joining and again whenever they beat it
(`LapRecord`, which also carries the track record and who holds it). A corrupt
records file is logged and treated as empty; `submit` writes from a blocking
task in the broadcast phase, never under the state lock.

**The ghost.** Each new personal best carries a trace of the lap that set it —
the car's pose at `records::GHOST_SAMPLE_HZ` (20 Hz), line to line, in
`records/ghosts/*.msgpack`, replacing the record's own previous trace.
`GameSession` samples it per human driver and hands it over with the lap
event. `LapRecord.HasGhost` says one exists; the hotlap mode fetches it with
`RequestGhost` and drives a ghost car from it (see Hotlap, below).

On the client `FApexTimingBoard` (ApexSimNet) is the tally: every car's splits,
bests and the session bests, fed one `LapTiming` at a time and read back
through `UApexNetSubsystem::GetTimingBoard`. The HUD paints the sector strip
from it (purple session best, green personal best, amber slower, grey still
running), shows "LAP INVALID" while the lap is struck, puts the session's
fastest lap under the standings, and measures its delta against the quickest
*legal* lap it has watched. The results screen adds the best lap's splits and
the personal and track records. Golden bytes come from `network.rs`
`test_lap_timing_wire_format` and `test_telemetry_compact_wire_format`
(`cargo test lap_timing_wire_format -- --nocapture`) and are pinned as
`ApexGolden::S_TrackSectors` / `S_LapTiming` / `S_LapRecord` and
`ApexUdpGolden::S_TelemetryCompactLapFlags`; `tests/lap_timing_test.rs` drives
the whole thing round Monza.

### Cockpit view (`Race/ApexCockpitRig`, `Race/ApexCockpitLayout`)
The first-person view is the default (settings: Camera tab, "Start in"). The
car meshes are exteriors, so the cockpit is built at runtime by
`AApexCockpitRig`, spawned by the race director and attached to the followed
car: a flat-bottomed steering wheel from engine basic shapes that rolls with
the steering telemetry, a `UApexCockpitDashWidget` on its hub (gear, speed,
RPM lights, lap time; the countdown while on the grid), and mirrors that are
`USceneCaptureComponent2D`s into render targets shown on `UApexMirrorWidget`
faces (drawn flipped, as a mirror is). Captures are refreshed round-robin by
the rig, never "every frame"; the Mirror quality setting picks resolution and
how many refresh per frame. The screens are unlit widget components, which
at the race's 50 klux exposure would be black: `apexsim.cockpit.ScreenNits`
(default 4500) is the tint that keeps them readable.

Where everything sits comes from `ApexCockpit::DeriveLayout` - pure maths
over the mesh's bounds in the car frame (open cockpit vs closed cabin from
the catalog class, else from height), covered by `ApexSim.Cockpit.*` tests.
Per-car overrides live on the car catalog row (`FApexCarCatalogRow::Cockpit`:
eye, wheel, mirrors; zero means derive). The player's seat slide, height and
view pitch, horizon lock, head motion (inertia from speed and yaw-rate
estimates) and look-to-apex are in `UApexSettingsSave`'s camera block, applied
live through `AApexRaceDirector::ApplyCameraSettings`. A virtual mirror strip
at the top of the HUD reuses a fourth capture. `-ApexView=cockpit|chase` picks
the view for a screenshot run regardless of the setting.

### Chase cameras (`Race/ApexChaseView.h`)

C no longer flips between two views: it steps down a ladder of chase
distances and then back to the cockpit. The rungs are `ApexChase::Views()`,
closest first — `roof` (a boom 0.6 m back, lifted 1.4 m, near enough to the
driver's eyeline to place the car by), `close` (3 m), `near` (5.6 m) and
`far`, which is the 9 m boom the one chase camera used to be, so an existing
profile and `-ApexView=chase` are unchanged. Each rung carries its arm
length, how far the boom's origin is lifted off the car, the boom pitch, a
field-of-view delta on top of the chase FOV (a close camera wants a wider
lens) and the spring arm's lag speeds: the roof cam is all but welded on,
because a lagging camera that close swings the roofline about the frame.
Each close rung also clamps how far the lag may stretch it
(`CameraLagMaxDistance`): the spring arm's steady-state lag is speed over lag
speed, nine metres at 320 km/h, which without the clamp puts every rung at
the far one's distance down a straight.
`AApexRaceDirector::ApplyChaseView` pushes a rung onto the boom and
`UpdateLook` takes the pitch from it; `CycleView` is what C calls.

The chosen rung lives on `UApexSettingsSave::ChaseViewLevel` and has a
"Chase distance" row on the Camera settings tab. The row only stores it —
the director adopts it with the rest of the camera group, so picking a
distance while sitting in the cockpit does not throw the player out of the
cockpit — while C writes back through `UApexSettingsSubsystem::SetChaseLevel`,
so the distance survives the race. The ghost replay borrows the far rung and
puts the player's back afterwards. `apexsim.cam.Chase [0-3|roof|close|near|
far|cockpit]` picks one from the console (no argument steps), `-ApexView=roof`
opens a race on one, and `-ApexCameraCycleAfter=N[,N]` presses C N seconds
into an unattended run, so one run with a matching `-ApexScreenshotAfter`
list walks the whole ladder. `ApexSim.Camera.Chase*` tests pin the ordering
and the cycle.

### Screenshot camera (`Race/ApexShotCamera.h`)
A third camera on the race director, `ShotCamera`, parks anywhere on the
circuit for a screenshot. While a pose is set it is the only active camera,
the followed car's bodywork is shown and the cockpit rig hidden; cars
spawning, the followed car changing and C all go through `ApplyCameraMode`,
which keeps it. Every number is in the SERVER frame (metres, +Y left, yaw
counter-clockwise from +X, pitch positive up), so a shot can be worked out
straight from the track YAML's centerline. Console: `apexsim.cam.Goto X Y Z
[Yaw] [Pitch]`, `apexsim.cam.LookAt X Y Z TX TY TZ`, `apexsim.cam.Fov Deg`,
`apexsim.cam.Release`; each logs the pose back as an `-ApexCamera=` switch.
Command line, applied when the race view begins and held for that race:
`-ApexCamera=X,Y,Z,Yaw,Pitch` (yaw and pitch optional),
`-ApexCameraLookAt=X,Y,Z,TX,TY,TZ` (wins if both are given) and
`-ApexCameraFov=Deg` (default 70), e.g. `-game -ApexAutoRace -ApexTrack=Suzuka
-ApexCameraLookAt=... -ApexScreenshotAfter=12`. The conversions live in
`ApexRaceCoordinate.h`; `ApexSim.Camera.*` tests cover the parsing and the frame.

### Replay clips and the promo video (`replay_tools.rs`, `apexsim-replay`, `ApexReplaySubsystem`)

A promotional clip is filmed from a race nobody drove. `apexsim-replay`
(server bin) `simulate`s a headless AI race straight on `GameSession` (no
network; `--seed` fixes the AI ids and so the grid, and a seeded race
replays bit for bit) and writes it as an ordinary replay (`replay.rs`,
format v2: the header now carries the conditions, the start tick, the
track stem and the lap length; the live server's recorder fills them too).
`find` ranks the moments the field runs through a stretch of the lap
together (a dossier corner by name, or a station); `pose` works out a camera
point beside the road (`--side outside|inside` of the bend, seated on the
ground heightfield when there is one, `--look-landmark big_wheel`); `cut`
writes a few seconds as a spectator stream (`.apxs`, see "Spectator
stream" below; the older `.clip.json`, `replay_tools::ClipFile`, is still
written when the output ends in `.json` and still read by the client).

The client plays a clip with `-ApexReplay=<file>.apxs`
(`UApexReplaySubsystem`, created only for such a run): no server, no demo,
no splash hold; `AApexRaceDirector::BeginReplayView` builds the track by
the clip's stem, spawns the field from its roster, lights the clip's sky
and places every car with `AApexRaceCarActor::SetPlaybackPose` from
`FApexReplayClip::SampleAt` at the director's own clock: game time, not the
motion buffer, whose arrival clock is the platform's and would drift under
a fixed timestep. Cameras (`ApexReplayCam`): the TV director
(`FDirector::LockTarget` keeps it on one car), a fixed or panning tripod
(`-ApexCamera=`/`-ApexCameraLookAt=`, look bias toward a landmark, zoom to a
frame width), chase or cockpit. `-ApexReplayRecord=<dir>` sets a fixed
timestep (`-ApexReplayFps`) and writes every frame as a PNG through
`FScreenshotRequest`, then quits. `scripts/promo/make_clips.py` drives all
of it from `scripts/promo/shots.yml` and `stitch_video.py` cuts the clips
into the video; `docs/PROMO_VIDEO.md` has the keys. Tests:
`replay_tools::tests` (windows, cut, clip, poses, a seeded race on
Zandvoort), `ApexSim.Replay.*`, `ApexSim.Tv.LockTarget`.

### Spectator stream, showcases and the menu backdrop (`spectator.rs`, `showcase.rs`, `ApexSpectatorStream.h`, `UApexSpectatorSubsystem`; docs/SPECTATOR.md)

The menu's backdrop race is no longer simulated per client: it is
**played back** from a spectator stream (`.apxs`), a sequence of
self-contained records, `[u32 big-endian length][positional MessagePack
body]`, that goes to a file or over the wire unchanged. `Header` (track,
stem, `source_crc`, resolved conditions, ticks, the render's seed and
score), `Roster` (cars with their `content_crc`), `Frame` (a `bin` of
52-byte little-endian car rows (44 in a v1 file, which still plays with
no tyres): pose in mm, yaw/pitch/roll, speed, pedals,
gear, rpm, lap, station, finish, the lap/pit/ERS flag bytes and the five
damage percentages; nothing a spectator does not draw), `Event`
(`LapTiming`, `TrackSectors`, state, finish, retired, pit stop, contact),
`Path` (the centerline every 10 m, for the TV cameras) and, in a file,
zlib `Block`s of a second each plus an `Index`. **The epoch is the second
element of every viewer-facing record, always a full `uint 32`** at bytes
3..7, so a looping channel stamps a new one into the file's bytes
(`spectator::patch_epoch`) and forwards them undecoded; a client drops a
frame whose epoch or roster revision is not the one it holds. Fields are
only appended; readers skip what they do not know. Golden bytes:
`cargo test spectator_wire_format showcase_wire_format -- --nocapture`
-> `ApexSpectatorGoldenBlobs.h` (and the `File` copy in
`BackdropTests.cpp`).

**Rendering**: `apexsim-replay render --track ... --class GT3 --cars 20
--laps 2 --weather sunny --time 13:00 --seed 7 [--seeds N] --cars-dir
content/cars/default --out build/showcase/<Stem>.<class>.<variant>.apxs`
runs the grid, the countdown, the laps and a tail past the winner on the
server's own `GameSession`, byte-identical per seed (the session id is
seeded too; `a_seeded_render_is_byte_identical`), and keeps the best of
`--seeds` by `RaceScore` (close racing for, contact, off-road and
retirements against). `info <file> [--check]` prints it and exits non-zero
when the track YAML or a car.toml changed since; `convert` turns a replay
`.bin` into a stream (no pit, damage or hybrid rows, no lap timing).
`content/showcase.yml` lists what the pipeline renders;
`build_track_levels.ps1` and `initialize_content.ps1` have the stage
(`-SkipShowcase`), the packages copy `build/showcase/*.apxs` to
`Game/Showcase/` and the release to `Server/showcase/` with
`apexsim-replay.exe` beside the server. Rendering takes ~6 s a circuit; a
file is a few MB.

**The server** (`[showcase]` in server.toml: `dir`, `playlist`, `mode`
loop|rotate, `stream_divisor`; `APEXSIM_SHOWCASE_DIR`) reads every file's
preamble at startup and drops one whose track or cars it lacks or whose
CRCs disagree. A **channel** is one file on one clock shared by its
viewers; the game loop advances it per tick (`ShowcaseState::advance`),
sends frames as bare record bodies over UDP (TCP, droppable, before the
handshake) and everything else as `SpectatorRecord` (a `bin` of framed
records: a newcomer's whole preamble and the lap timing so far is one
message). Messages: `ListShowcases` -> `Showcases`, `SpectateShowcase
{id}` -> `SpectatorJoined` (400 inside a session, 404 unknown),
`LeaveSpectate` (implied by any session create/join); `LobbyState.
ShowcaseAvailable`; `/showcase` JSON and `apexsim_showcase_viewers` on the
health port. A channel nobody watches drops its inflated records; the
first viewer's join inflates the file off the loop. `tests/showcase_test.rs`.

**The client**: `FApexStreamFile` / `FApexSpectatorPlayer` (ApexSimNet,
pure) decode and assemble parts; `UApexSpectatorSubsystem` plays a local
file on the game clock (loop = the preamble again = a new epoch) or the
server's records as they arrive, and feeds both into
`UApexNetSubsystem`'s **backdrop feed** (`BeginBackdropFeed`,
`FeedBackdropRoster/Telemetry/Sectors/LapTiming`, `EndBackdropFeed`),
which makes `IsInDemoSession()` true and raises the same delegates a demo
session did, so the race director, TV camera, HUD data and engine sound
are untouched. `UApexDemoModeSubsystem` picks the source: the server's
showcase when the lobby lists one (a 3 s grace for a connection under
way, so the showcase wins the startup race), else a local `.apxs` from
`Showcase/` beside the exe (`build/showcase` in the editor) whose CRCs
match the catalog rows (`ChooseFile`; scanned at init so the splash hold
works offline), else a `SessionKind::Demo` session for an old server. The
sky roll only applies to demo sessions. `-ApexShowcase=<file|id>`,
`-ApexNoShowcase`, `-ApexShowcaseDir=`, `apexsim.spectate.Info`,
`apexsim.spectate.Next`. Trap found writing it: a test fixture whose
roster revision was not its frames' had every frame silently dropped;
the file test now plays the whole file through the player. Not done:
a live session as a stream (`SpectatorKind::Live` is reserved;
`JoinAsSpectator` sends racer telemetry, below).

### Watching a race and replays (`Race/ApexSpectatorView.h`, `UApexReplayRecorder`, `UApexReplaysWidget`; docs/SPECTATOR.md sections 5-6)

Any race on the director can be **watched** full screen with a spectator's
controls: Main menu > *Watch a race* (the backdrop, kept across its next
races), the session browser's *Watch* (a live session, `JoinAsSpectator`;
the server now resends the roster and sends `TrackSectors` to a spectator
who joins mid-race, without which every frame was dropped, and lists each
session's real `State` and `LapLimit`), and Main menu > *Replays*. The
director holds the watched car, the camera (TV locked on it / chase /
onboard with the rig) and the auto director (`SetSpectating`, `FocusCar`,
`StepFocus`, `SetSpectatorCamera`); `ApexSpectate` is the pure part
(order, stepping, the key map; `ApexSim.Spectate.*`). Keys reach
`UApexRootWidget::HandleWatchKey` through the input processor (arrows /
shoulders car, 1-0 position, C camera, A auto, T tower column, H overlay, N
next race, Space / , . / - = a replay's pause, seek and speed, Backspace
leave, Esc the pause menu); `apexsim.watch ...` and `-ApexWatch`,
`-ApexWatchSession`, `-ApexWatchReplay=<file|latest>` (+ `-ApexWatchCamera=`,
`-ApexWatchTower=`, `-ApexWatchCar=`, `-ApexWatchHideHud`) for unattended
runs. The HUD's local car is the watched one, its circuit, length and race
distance the race's own; `spectate.*`, `replay.*` and the new `standings`
fields are in docs/HUD_MODDING.md, drawn by the `spectator_*` components
(the driver-only ones hide).

**Replays**: `UApexReplayRecorder` records every session the client is in
(race, practice, hotlap, a race watched) as `.apxs` on the client (30 Hz,
`FApexStreamCarRow::FromTelemetry`, `FApexStreamWriter` in
`ApexSpectatorWriter.h`, encoders byte-identical to the server's:
`ApexSim.Spectator.Writer`), cutting time with every car in a hotlap
garage; the session goes to `Saved/Replays/Recent/` (ten kept) when it
ends, and SAVE REPLAY (pause menu, hotlap garage, `apexsim.replay.Save`)
keeps it in `Saved/Replays/`. Playing one is
`UApexDemoModeSubsystem::PlayReplay` (backdrop source `Replay`: no loop,
no moving on) with `UApexSpectatorSubsystem`'s `SetPaused` /
`SetPlaybackRate` / `SeekTo`. `apexsim-replay info` reads a client-saved
file like a rendered one.

### Demo mode and the broadcast camera (`ApexDemoModeSubsystem`, `Race/ApexTvDirector.h`)

The menu plays an AI race behind its screens, from a showcase stream when
one is to be had (above) and otherwise as a demo session. `UApexDemoModeSubsystem` asks the
server for a `SessionKind::Demo` session whenever the client is connected and
not in a session: unlisted, unjoinable, spectated by its creator, counted
straight into a race, no replay written, removed when the spectator leaves.
`SessionJoined` carries `SessionKind`, and `UApexNetSubsystem` keeps a demo out
of every session delegate (`OnDemoSessionChanged` instead; `IsInSession()` is
false) and leaves it by itself before any create or join. `SessionJoined` drops every
telemetry frame already in the UDP queue (after a hitch it holds seconds of
the session just left; telemetry carries no session id), and a frame is only
applied when every car index fits the current roster: a stale demo frame
would otherwise stamp the demo's `Countdown`/`Racing` state on a session still
in Lobby, putting the race view over the lobby screen with nothing counting
down. The race director's
demo view builds the track (the player's pending track when it has an export),
and the root widget fades page backgrounds by `GetDemoBackdropOpacity()` under a
left-heavy scrim. Car select and session create return false from
`WantsLiveBackdrop` because the turntable shares the world, so the demo world is
hidden behind them. It restarts on a finished race, a track change or after
`apexsim.demo.MaxMinutes`. Each demo race rolls its own sky
(`UApexDemoModeSubsystem::RollConditions`: weighted toward dry daylight, with
rain, dusk and night in the mix) and sends it as the create's `conditions`, so
the server bakes the wet grip for the AI like any session;
`apexsim.demo.RandomSky 0` keeps the default sunny 13:00. `-ApexNoDemo`/`apexsim.demo.Enabled 0` turn it off;
`-ApexAutoRace` never starts one.

`ApexTv::FDirector` is the TV director, pure logic with ground and visibility
traces injected: it picks a car (battles, the leader, an incident: off track or
stopped, at most once per 12 s) and cuts between grid, trackside (long lens
from the lobby centerline, outside of the bend), helicopter, tracking, chase,
onboard, nose and reverse shots, with lag-compensated pans and depth of field.
`-ApexView=tv` or `apexsim.tv.View 1` uses it in a race; `apexsim.tv.Shot <name>`,
`apexsim.tv.Cut`, `apexsim.tv.Pace`, `apexsim.tv.Debug 1`; each cut and its
reason is logged at `LogApexSim Verbose`. `ApexSim.Tv.*` tests drive it on a
synthetic ring. `-ApexScreenshotAfter` takes a comma list of times.

### Client startup settings (`settings.yml`)

The few settings a player may need to change *before* the game is usable -
resolution, window mode, vsync, frame limit and the server address - live in a
plain-text `settings.yml` rather than a binary save slot.
`UApexBootSettingsSubsystem` owns it: it reads the file at game-instance
startup, creates it with defaults (seeded from the desktop's own display mode)
when absent, and rewrites it when those values change in-game.

It sits next to the executable: `Game/settings.yml` in a release package,
`game-unreal/settings.yml` (gitignored) in the editor. `FPaths::ProjectDir()` is
`<Release>/Game/ApexSim/` in a packaged build, so its parent is the folder
holding `ApexSim.exe`.

The file wins over the save slots for the values it covers, which is the whole
point of it. `UApexSettingsSubsystem` and `UApexMenuFlowSubsystem` name it as an
`InitializeDependency`, adopt its values on load, and push back on save; a file
created this run is seeded from the slots instead, so an existing install keeps
what it had. Only a flat two-level subset of YAML is parsed (hand-rolled - Unreal
has no YAML reader); an unknown key or unparseable value is logged and skipped
rather than failing the load. Every write regenerates the file wholesale,
comments included, so hand-added comments do not survive a change made in-game.

`build_release.ps1` ships a `Game/settings.sample.yml` beside where the real
file appears: the same shape with the shipped defaults, so a player can read
every setting before the first run creates one. It is a sample and not a live
`settings.yml` on purpose - a shipped file would be adopted over the first-run
display detection, pinning an arbitrary monitor to a 1920x1080 guess. Its text
is a second copy of what `ApexBootSettingsIo::Serialise` writes; the two carry
comments pointing at each other.

### Startup splash hold (`ApexSimBoot`, `UApexStartupSplashSubsystem`)

The splash stays up until the menu's demo race is running behind it; the first
thing a player sees after it is the finished menu over a moving race, not the
UI on black, the window resize to the settings.yml mode and the race fading
in. The engine closes its own splash on the first frame, long before that.

`ApexSimBoot` is a UObject-free module at `LoadingPhase` `PostConfigInit`,
because the engine creates its game window in pre-init (the preload screen
manager), before any other project module loads. It installs two
thread-local hooks: when the first top-level `UnrealWindow` is created it
opens a copy of the splash (the same `Content/Splash/Splash.bmp` at the
engine splash's own rectangle, just beneath it, so the engine closing its
splash changes nothing on screen), and on that window's `WM_SHOWWINDOW` it
cloaks it with `DWMWA_CLOAK` (DWM refuses at creation with `E_HANDLE`). A
cloaked window is shown as far as the engine and Slate know: it ticks,
renders, lays out and takes its resize, but the compositor does not draw it.

`UApexStartupSplashSubsystem` claims the hold at game instance init, checks
the viewport's window is the cloaked one, and ends it: when
`AApexRaceDirector::IsDemoReady` (backdrop fully faded in), early when
`UApexDemoModeSubsystem::IsDemoExpected` is false (demo off, no server,
refused login, no track with a level, a refused create), or after
`apexsim.splash.MaxSeconds` (20). The reveal uncloaks the window beneath the
splash and fades the splash out over 0.35 s. An unclaimed hold is dropped at
engine init complete and a claimed one after 45 s, so a broken game cannot
stay invisible. Nothing is held in the editor, commandlets, `-nullrhi`,
`-nosplash`, `-ApexNoSplashHold`, `-ApexNoDemo`, `-ApexAutoRace`, or
exclusive fullscreen (a cloaked window cannot own the display). Screen
grabs that go through the compositor do not see a cloaked window, which is
how the hold was checked: a Python `ImageGrab` loop around a launch.

### Menu sound (`Audio/`)

The shell's cues are synthesised, not imported: there are no sound assets in
the project. `ApexUiSynth` (`Audio/ApexUiSound.h`) renders each `EApexUiSound`
(Move, Accept, Back, Denied, Adjust, Notice, Error) as a short tone from a
note table, and `UApexUiSoundWave` is a procedural `USoundWave` whose
`CreateSoundGenerator` renders the cue at the device's own sample rate when
the mixer asks for it. `UApexUiAudioSubsystem` (game instance) plays them via
`PlaySound2D` as UI sounds, scaled by `UApexSettingsSave::UiVolume` and
throttled per cue; widgets call `ApexUiAudio::Play(this, EApexUiSound::X)`.

Where the cues come from: a button's `FApexButtonSpec::Sound` (Accept by
default, Back on Back buttons), `UApexNavigableWidget` when a `HandleBack`
consumes the press, the root's `ShowToast`, and the settings sliders and
dropdowns (Adjust). The race cues (`CountdownTick`, `CountdownGo`, `LapLine`)
come from `AApexRaceDirector::UpdateRaceBleeps` on telemetry: a tick for each
of the last five countdown seconds (with the start lights), a higher tone on
green, and a double pip when the local car completes a lap; the demo is silent. Move is not raised by any widget: the root widget listens
to `FSlateApplication::OnFocusChanging` and plays it for focus moved by the
player, which is a change with cause `Navigation` or one made inside an
`ApexNav::FNavigationScope` (the scope is what separates a host's
`ApexNav::Focus` from the same call made when a screen opens). Scopes wrap
`RouteFromLeaf`, the navigable widget's key/analog handlers and the input
processor's focus recovery; do not add per-widget Move cues.

Volume lives on the settings overlay's Audio tab: master volume drives the
audio device's transient primary volume (`ApplyAudio`), menu-sound volume is
read at play time. `-ApexSettingsTab=6` opens that tab headlessly.
`ApexSim.UI.SoundCues*` automation tests check every cue is short, finite,
click-free and the same length at 44.1k and 48k.

### Car sound (`Audio/ApexEngineSound.h`, `Audio/ApexRoadSound.h`)

Nothing is recorded: the engine is **simulated** and the sound is what its
exhaust would hear. `ApexEngineSynth::Render` turns a crank at the
telemetry's RPM; each cylinder's firing angle puts an exhaust stroke into one
of two exhaust **banks**: a steep, short **blow-down** (the upper harmonics;
a first-difference "rasp" on it survives only open pipes, `Openness^8`) and
the piston's long, eased-in **swell** (nearly all the weight), both a fixed
share of the cycle, sized by throttle. Each bank is a weakly reflecting
quarter-wave header (a delay line reflected inverted); the banks meet in a
collector and go through the **silencer** (two poles, 400 Hz to 8 kHz by
`muffling`), a half-wave **tailpipe** resonance and the outlet's low-end
boom. The first version had only the blow-down, a ringing header and
*random* firing-to-firing variation at idle, and the user heard it at once:
"like a two-stroke", which is literally what those three are (a port
snapping open onto an expansion chamber, four-stroking at idle). What makes
it a four-stroke is that each cylinder differs from its neighbours the
*same way every cycle* (`CylinderTrait`: fixed gain and timing per
cylinder), a pattern that repeats every two revs and so puts power on the
crank's half-orders under the note, on every engine, flat-plane included. So the note is the firing rate, `rpm/60 x cylinders/2`,
proportional to RPM with nothing to saturate, and the character comes from
geometry: a crossplane V8 fires its banks L R L L R R L R, which puts power
on the crank's own frequency and its odd halves under the note (the
burble: ~12 dB under the firing note, and 600x what a flat-plane has
there), while everything else alternates banks. The two pipes are heard
unequally (`SecondBankLevel`/`SecondBankLength`) because two identical
pipes half a period apart sum back to an even pulse train and no crank
could be told from another. On top: overrun pops (a lift above a third of
the rev range opens a ~1 s budget in which firings randomly become
oversized pulses with a low-passed noise burst; a flat-out upshift cracks
once), the limiter's spark-cut stutter, induction roar, a gearbox whine at
a per-gear tooth-mesh frequency (`WhineHz`: it steps *up* on an upshift at
the same revs, replacing the old `GearTrim` hack), and a turbo's whistle
and blow-off. Two traps found by measuring rather than listening: a pulse
attack of 0.25 ms is a 640 Hz low-pass on the whole engine (it is two
samples now), and a first-difference "rasp" applied after the turbulence
noise is just hiss (it is taken from the pulse alone).

An engine is described by the `[sound]` table in its `car.toml`
(`cylinders`, `crossplane`, `turbo`, `exhaust_length_m`, `muffling`, `pops`,
`gear_whine`, `intake_roar`; the server ignores it), which the game reads
onto the catalog row as `FApexEngineSoundSpec EngineSound` with
`[engine]`'s idle, redline and limiter. That row is also how the client finally knows the rev range,
which is not on the wire. `ApexEngineAudio::MakeSpec` fills in a class
default (F1 turbo V6, LMP flat-plane V8, else crossplane V8) for a row from
before the field. `docs/CAR_MODELS.md` has the table key by key.

`AApexRaceCarActor` owns a `UApexEngineSoundWave` on an attenuated audio
component and feeds it the motion buffer's **blended** RPM every render
frame (the raw 60 Hz samples are a zipper on a synthesised crank) with the
newest throttle and gear; the generator renders on the audio thread from a
lock-free `FApexEngineLiveState`, picking up a changed engine spec by
serial under a lock that is all but never contended. The 20 KB synth state
(the exhaust's delay lines) lives on the heap.

**The mix.** Somebody else's car is a mono point in the world: full level
only within 4 m, then `NaturalSound` falloff to -50 dB at 150 m with an
air-absorption low-pass (20 kHz at 10 m to 1.5 kHz at 120 m), times the
"Other cars" setting (`OtherCarsVolume`, 0.5). It used to be full level
within 15 m, i.e. the whole grid as loud as the player. The **player's own
car** (the director's `FollowedCar`, unless TV view, shot camera or ghost
replay) plays a second, *stereo, unspatialised* engine instead
(`UApexEngineSoundWave::MakeOwnCar`, `OwnEngineAudio`;
`AApexRaceCarActor::SetListenerSeat`), the same synth run through
`ApexSpace` (`Audio/ApexListenerSpace.h`): a low shelf for weight, a
3.5 kHz bulkhead low-pass in a closed cabin, and a Freeverb-shaped stereo
reverb sized per seat (cabin 0.35 s, open cockpit 0.5 s, chase/trackside
1.1 s). The reverb send is high-passed at 250 Hz: fed the low orders, the
combs became room modes and cancelled a 70 Hz firing note against the
direct sound, eating the whole shelf (a test caught it). The seat is set
every frame from `AApexRaceDirector::UpdateCarAudio`; `ApexSim.Audio.Space*`
tests cover the shelf, the bulkhead, the ring times at both device rates
and that the tail is stereo.

**Tyres, kerbs, road and wind** (`ApexRoadSynth`, `UApexRoadSoundWave`) play
for the local car only, because they come from the server's
`DriverFeedback`: the race director reduces it with `ApexFfb::MakeSignals`
(kerbs, grass, hits: the very signals the pad and wheel get) and feeds the
car's unspatialised `RoadAudio`. The **tyres have their own thresholds**,
from the raw per-wheel slip (`ApexRoadSynth::SquealFromSlipAngle` and
friends): silent up to and at the grip peak, because a car cornering well
sits *at* its peak slip angle all day; the howl starts at 1.25x and is full
at 2.4x, lockup and wheelspin from 1.6x the peak slip ratio (ABS/TC hold
1.0). The FFB's scrub starts at 0.9 as a hint, and driven from that the
car screeched at every turn of the wheel. Slide levels also rise over
0.14 s, since the feedback is peak-held between messages and one tick
over a bump would otherwise chirp. A howl is a **tone** (fundamental ~600
rear / ~760 Hz front plus three falling harmonics, pitch wandering 3%,
level fluttering at ~35 Hz; a locked wheel flutters deeper) over a band of
low-mid scrub; none at a crawl, none on grass, mostly scrub in the wet.
It was first noise through a narrow resonance, which is a wavering whistle
over hiss: the user called it "shortwave radio" and it was mistaken for
the wind. No voice here is white noise gated by a level; a kerb is a
rib every 0.9 m (the force feedback's spacing) thudding through a 95 Hz
resonance, so its pitch is the car's speed; off-track is rumble and stones;
a suspension hit over the FFB's 0.3 m/s threshold is a thud and contact a
crunch, handed to the audio thread by atomic exchange so each plays once;
rolling and wind grow with speed. It stops in the garage, during a ghost
replay and when feedback is over 0.5 s stale.

The Audio settings tab has "Engines" and "Tyres and road" sliders
(`UApexSettingsSave::EngineVolume`/`RoadVolume` ->
`AApexRaceDirector::ApplyAudioSettings` -> `SetMixVolumes`, multiplied with
the demo's `SetEngineVolume` scale). `apexsim.audio.RenderCars [dir]`
renders every catalog car through a scripted drive, and the road voices, to
`Saved/Audio/*.wav` (`<folder>.wav` as the world hears it, `<folder>_own.wav`
in stereo from the driver's seat): the way to judge or tune a `[sound]` table.
`ApexSim.Audio.Engine*` / `ApexSim.Audio.Road*` tests pin the physics (power
on the firing note at 44.1k and 48k, half-orders on a crossplane only, GT3
under 2 kHz vs the F1 above, pops, limiter stutter, squeal notes, the kerb's
rib rate, hit thresholds); `ApexSim.Cars.TomlSound` the TOML scan. Both
synths build standalone against a ten-line `CoreMinimal.h` shim (only
`FMath` is used), which is how they were tuned: MSVC + a WAV writer +
numpy spectra, no editor build per iteration.

### Driving assists (`SetDriverAids`, `AllowedAssists`, the Assists settings tab)

Every assist runs on the server, because only it has the tyres: ABS,
traction control, the automatic gearbox and speed-sensitive steering are
`CarState` fields the physics reads every tick, and the racing line is built
there and sent on join. The client's settings overlay has an **Assists** tab
(`EApexSettingsTab::Assists`, group `EApexSettingsGroup::Assists`; the tab
order is gameplay, assists, graphics, camera, controls, wheel, audio, car
setup, so `-ApexSettingsTab=1` opens it) holding ABS, traction control OFF/LOW/HIGH,
gearbox, steering, damage OFF/REDUCED/FULL and the racing line.
`UApexRootWidget::SendDriverAids` sends `SetDriverAids { auto_gearbox,
steering_assist, abs, traction_control, damage }` on join and on any change
of the group. **Damage** (`DamageLevel`, `CarState::damage_level`, `None` =
full for the AI and an old client) multiplies every accrual in `damage.rs`
(impacts in `apply_damage_to_car`, overheating, over-revving) by
`DamageLevel::scale` (0, `REDUCED_SHARE` 0.5, 1); what damage already done
costs is unchanged. `damage` is left off the wire while unset, so the older
aids keep their bytes. ABS and traction control are
`Option`s on the server (`CarState::abs`, `CarState::traction_control`) so an
AI, or a client from before the fields, drives the car as its `car.toml`
describes it. Traction control LOW holds a spinning wheel at peak slip as
before; HIGH caps drive at what the friction circle has left beside the
lateral force, less a margin (`TC_HIGH_LATERAL_MARGIN`), so full throttle on
a corner exit keeps the cornering grip instead of pushing the rear wide.

A session's host decides which assists its drivers may use: the create
screen's "Allowed assists" chips go out as `CreateSession.allowed_assists`
(`AllowedAssists { abs, traction_control, auto_gearbox, steering_assist,
racing_line, damage }`, every field defaulting to true so an old client's
session allows everything; `damage` is only written when false, "Damage aid"
chip, and a forbidden one pins `DamageLevel::Full`), the session keeps them
on `RaceSession::allowed_assists`
and echoes them in `SessionJoined.AllowedAssists`. The server enforces the
rule (`AllowedAssists::clamp`): a forbidden aid is pinned off when a car is
seated and on every `SetDriverAids`, whatever the client asked, and a
session that forbids the racing line sends none. On the client the Assists
tab dims a locked row, disables its pills and shows an "Off in this session"
badge (`UApexNetSubsystem::GetAllowedAssists`, `RefreshAssistLocks`); the
player's own choice is kept for the next session; `-ApexAutoRace
-ApexLockAssists=abs,tc,gearbox,steering,line,damage` creates the auto-race session
with those forbidden, for a screenshot run of the locked tab
(`-ApexOpenSettings=N -ApexSettingsTab=1`). Golden bytes for all three
messages come from `network.rs` `test_assists_wire_format`
(`cargo test assists_wire_format -- --nocapture` prints them) and are pinned
in `ApexGoldenBlobs.h`; the damage aid's from `test_damage_assist_wire_format`
(`ApexGolden::C_SetDriverAidsDamage`, `S_SessionJoinedNoDamage`).

### Weather and time of day (`SessionConditions`, `Race/ApexSkyModel.h`)

A session's host picks its sky on the create screen: a **Weather** chip row
(sunny, cloudy, overcast, light rain, heavy rain) and a **Time of day**
slider in quarter hours. Both go out inside `CreateSession.conditions`
(`SessionConditions { weather, time_of_day_minutes }` and the optional
air, see "The air" below; defaulting to a sunny 13:00 so an old
client's session is unchanged), are kept on
`RaceSession::conditions`, echoed in `SessionJoined.Conditions` and listed
in every `SessionSummary` (the browser row shows "Light rain · 21:30").
The clock wraps onto one day on the way in (`SessionConditions::clamp`).

The **server owns the grip**: `create_session` bakes the weather into the
session's own copy of the track (`SessionConditions::apply_to_track`):
every centerline sample's `grip_modifier`, the surface's `base_grip` (which
the racing line and the AI's speed profile read) and the curb and off-track
grips, so physics, the AI and the racing line all follow with no branch in
the tick and a dry session is the track to the bit. Light rain is 0.86 of
the road's grip and heavy rain 0.74; painted curbs lose a further 15–25%
and grass 15–30% (`Weather::*_grip_factor`). Time of day is visual only.
Golden bytes come from the same `test_assists_wire_format`;
`tests/session_conditions_test.rs` checks the echo, the listing and the
bake end to end.

On the client `FApexSessionConditions` lives beside the assists in the net
module (`Describe`, `ClockText`, `WeatherLabel`), the flow keeps
`CreateConditions` (persisted on the profile), and `UApexNetSubsystem::
GetSessionConditions` holds the joined session's sky, demo included. The
race director turns it into light with `ApexSky::Derive` (pure maths,
`ApexSim.Sky.*` tests): the sun's elevation and azimuth for that hour on a
late-May day at 50° N with solar noon at 13:00, its lux from the airmass
and the cloud, its colour warming toward the horizon and greying under
cloud, the sky light scaled up under overcast, shadows off and the source
widened when the disc is gone, and after dark the directional light
becomes a one-lux blue moon that no longer drives the atmosphere. A
director-owned unbound post-process volume (priority 10, over the track's
own daylight clamp) carries the exposure floor and ceiling for the hour,
the wet desaturation and the bloom. What lives in the track is applied
once it is built and visible (`ApplyTrackLevelConditions`, over
`UApexTrackInstance::GetActors`, polled like the start lights): the
track's fog gets the weather's density, start and colour (dark at night);
in rain the road family's dynamic instances (`MI_road*`, `MI_pit_lane*`
and the `MI_wear_*` bands the racing line runs on, shared by every mesh of
the key and put back when the track is reused) get `Roughness` 0.3 for the
wet sheen, and the racing-line dots go glossy and
dark with them (`AApexRacingLineActor::SetWet`); after
dark every `floodlight_tower` / `lamp_post` instance gets a shadowless spot
light at its head (at most 96) and the `ApexEmissive_floodlight_lamp`
faces glow. Rain is `AApexRainActor`: up to 1500 slivers of the engine cube
in a box that travels with the active camera, falling in world space,
wrapped back in when they leave, each stretched along its apparent
velocity so they streak at speed (no particle assets exist in the
project). Cars get two lumen-rated spot headlights at the nose and dim
running tail lights (`AApexRaceCarActor::SetHeadlights`,
`apexsim.car.HeadlightLumens`). Whether they are on is the server's
(`headlights.rs`), because everyone sees them: `PlayerInput.headlights`
is the driver's switch (nil until the first press of the Headlights key,
which then flips what the car shows) and `PlayerInput.flash` the held flash
button; an untouched switch, the AI and an old client get the sky's rule,
sun under 6° or rain (`SessionConditions::headlights_needed`, the same sun
as `ApexSky::SunAt`). Telemetry carries on/flash as `lap_flags` bits 5 and
6 and the race director lights every car from them, a flash at full beam
(held at least 0.2 s so a tap is seen). Golden bytes: `cargo test
player_input_headlights_wire_format -- --nocapture` →
`ApexUdpGolden::C_PlayerInput`.
`-ApexWeather=heavyrain -ApexTimeOfDay=22:15` put an `-ApexAutoRace`
screenshot run under that sky.

### Car setup (`server/src/car_setup.rs`, `SetCarSetup`, the hotlap garage)

The garage: tyres, engine, transmission, torque, suspension, fuel and aero,
per driver.
A setup is **clicks** off the car's own `car.toml`, one `i8` per knob
(`CarSetup`, 19 knobs in `KNOBS` order; the fuel load and the four aero
knobs appended after the first 14), so neither the wire nor the client
needs the car's base figures; the client shows "+2  (+8%)" and the server
turns that into newtons per metre against the file it loaded. Every knob
has a fixed click range (±5; the rev limiter and torque map only go down)
and a fixed effect per click (`*_PER_CLICK`): tyre pressure 5 kPa per axle,
rev limiter −100 rpm (redline comes down with it, so the auto gearbox
follows), engine braking ±15%, final drive ±2%, gear spread ±2% on top gear
blended down the ladder with first untouched, torque map −4% on the whole
curve, brake bias ±1% front, springs ±4%, dampers ±5% (bump and rebound
together), anti-roll bars ±8%. Only knobs the sim reads are offered: there
is no differential model and rolling resistance is never consumed.

Tyre pressure is the one physics addition: `TireConfig` carries
`pressure_front_kpa` / `pressure_rear_kpa` / `optimal_pressure_kpa` (180
by default, not yet in any car.toml) and `pressure_grip_factor` costs an
axle grip quadratically off the optimum (4% at five clicks), exactly 1.0 at
it, so a stock car is bit-identical to before (`update_car_3d` passes
`effective_grip_front` / `_rear` to the wheel solver). Raising one end's
pressure loosens that end, which is what makes the knob a balance tool.

The client sends `SetCarSetup { tyre_pressure_front, … }` on join and on any
change of the group (`UApexRootWidget::SendCarSetup`, like the aids). The
server clamps (`CarSetup::clamp`) and bakes the result into a tuned copy of
the `CarConfig` (`CarSetup::apply`) kept on `GameSession::tuned_configs` per
player; the tick loops look the car up through `simulated_config`, tuned
first, shared otherwise, so the hot loop pays one map lookup and nothing
else. A stock setup drops the copy; leaving the session drops it. The setup
applies at once, on the grid or mid-lap. AI cars and the collision passes
(dimensions only) use the shared config. Golden bytes come from
`network.rs` `test_car_setup_wire_format` (`cargo test car_setup_wire_format
-- --nocapture`) and are pinned as `ApexGolden::C_SetCarSetup`.

On the client the setup lives on `UApexSettingsSave::CarSetup`
(`FApexCarSetup`, `TArray<int32> Clicks`, one setup shared by every car),
edited in the **hotlap garage** (`UApexHotlapWidget`, below; it used to be
a settings tab, `-ApexSettingsTab` now stops at 6, Audio) on four tabs
(Tyres, Suspension with front and rear side by side, Engine, Load / Save;
Q / E, the shoulders or Tab change tab, `apexsim.hotlap.Tab N` /
`-ApexGarageTab=N` for screenshots) of `UApexStepperWidget` rows: a − / +
pill pair around the knob **in real units** with its change from stock
under it. The units are the server's: `ServerMessage::CarSetupSheet`
(`server/src/setup_sheet.rs`, sent once after `SessionJoined`) carries each
knob's stock value, per-click step and bounds in display units (psi, N/mm,
Ns/m, mm, kg of downforce at 200 km/h, litres...), since many of those
figures are sim defaults in no car.toml, plus the gear ratios, wheel
radius, fuel per lap and the rake's balance shift, from which the garage
draws top speed per gear, fuel range and weight, rake and aero balance.
Every knob is linear in clicks, so the client needs no round trip
(`the_sheet_predicts_what_apply_does` pins that); without a sheet (an older
server) a knob reads in clicks through `ApexCarSetup::Describe`. Named
setups are kept per car on `UApexSettingsSave::SavedSetups`
(`FApexSavedSetup`: name, car id, date, clicks, the best legal lap driven
while the working setup matched it) with `LoadedSetupId` naming the one in
the header; `UApexSettingsSubsystem::SaveSetupAs` / `LoadSavedSetup` /
`OverwriteSavedSetup` / `RenameSavedSetup` / `DeleteSavedSetup` / `RecordSetupLap`. The steppers write through
`UApexSettingsSubsystem::SetCarSetupClick`, so the group's change still
reaches the server through `SendCarSetup`, and the setup is sent on joining
any session, so a car tuned in the garage races with that setup. The knob
table (`ApexCarSetup::Knob`: wire key, range, per-click size and unit)
lives in the net module beside the encoder and mirrors the server's
constants; `ApexSim.Net.CarSetup.Clicks` pins the ranges and read-outs, the
golden encode test the bytes. The garage's Reset returns every knob to stock.

### Fuel (`FuelConfig`, `GameSession::start_fuel_liters`, the fuel knob)

The first item of docs/SIMULATION_GAPS.md. `mass_kg` in car.toml is the car
with its driver and a **dry** tank (as AC's `TOTALMASS` is); the fuel rides
on top of it: `CarConfig::laden(fuel_liters)` gives the mass and front share
the physics step uses for the static loads, the axle lever arms, weight
transfer, gravity, the accelerations, yaw inertia, the steering aid's grip
lock (`grip_limit_lock_rad` now takes the mass) and the car-car impulse
(`physics::car_mass_kg`). `[fuel] tank_front_share` puts the tank somewhere
other than the car's own weight split, so draining it moves the balance;
no shipped car sets it. `density_kg_per_l` defaults to 0.745.

**Consumption is the engine's power**: `FuelConfig::burn_lps` = combustion
power (throttle x the curve's torque x the limiter cut, at the crank's
speed) / (`thermal_efficiency` x 43 MJ/kg x density), plus idle. A lift
burns idle only, so lifting and coasting saves fuel by itself. The shipped
cars say 0.50 (F1), 0.40 (Hypercar), 0.33 (LMP2, GT3); the default is 0.30.
A car.toml that still names `load_consumption_scale` and no efficiency
keeps the old throttle-times-revs rule. **A dry tank makes no combustion
torque** (engine braking and friction remain; a hybrid still drives).

**What a car is filled with** is the server's (`start_fuel_liters`), from
its own estimate of a lap: `racing_line::lap_fuel_liters` integrates the
car's speed profile (planned half full with `build_laden`), wide open where
the plan is power-limited (the curve averaged over the top quarter of the
revs, whatever a hybrid adds) and the power the speed change asks for
elsewhere, cached per car in `GameSession::lap_fuel`. A race gets its
distance x (1 + `RACE_FUEL_MARGIN`, 8%) + `RACE_FUEL_RESERVE_LAPS` (1); a
hotlap or qualifying run `HOTLAP_FUEL_LAPS` (3); practice a full tank;
never under `MIN_FUEL_LAPS` (1) or over the tank. The margin is measured,
not guessed: at Monza the AI burns 105% of the plan in the F1 and 79-85% in
the other classes (`fuel_test.rs` pins it per class; an estimate short of a
car at the limit runs a long race dry). Cars are filled where they are put
out: `add_player` (for the mode the session is heading into),
`line_up_on_grid` (race fuel), `hotlap_relocate` (the garage fills to the
hotlap load, out or in), entering FreePractice or Qualification.

**The fuel knob** is the setup's fifteenth, `fuel_load` (appended last on
the wire, ±5): laps over or under the session's fill, not a figure of the
car, so `CarSetup::apply` leaves it alone and `changes_car()` (not
`is_stock()`) decides whether a tuned copy is needed. It fills the car at
once only in a hotlap garage or before the start (lobby, countdown);
anywhere else it waits for the next time the car is fuelled, never mid-lap.

**The AI plans laden**: its speed profile is built at a race's starting
load (`build_laden`), the heaviest the car will be, so it only gets quicker
than the plan as the tank drains; its grip budget reads the laden mass. The
player's racing line is still planned dry (`racing_line::build`). Neither is
rebuilt as the fuel burns — a follow-up if the gap matters.

**Wire**: `CompactCarState.fuel_dl` (u16, tenths of a litre) appended after
`lap_flags`; `FApexCarTelemetry::FuelLiters` (-1 from an older server). The
HUD's footer has a Fuel cell (`UApexHudWidget::RefreshFuel`): the litres,
amber when the last lap's burn says the tank will not reach the flag, red
under a lap or dry. The garage has a "Fuel load" row. Golden bytes:
`cargo test telemetry_compact_wire_format -- --nocapture` ->
`ApexUdpGolden::S_TelemetryCompactFuel`, `cargo test car_setup_wire_format
-- --nocapture` -> `ApexGolden::C_SetCarSetup`. Tests: `tests/fuel_test.rs`
(race fill, per-class burn against the plan, the plan's cost of a full
tank, the hotlap garage and the knob, practice), the physics unit tests
(`a_full_tank_weighs_on_the_car`, `a_dry_tank_stops_the_engine`,
`the_engine_burns_by_the_power_it_makes`), `ApexSim.Net.Udp.LapFields`,
`ApexSim.Net.CarSetup.Clicks`.

### Tyre temperature (`server/src/tyre_thermal.rs`, `[tires]` window keys)

The second item of docs/SIMULATION_GAPS.md. Each tyre is two thermal
masses: a **tread** layer (2 kJ/K, the surface the HUD shows) cooled by
the air (growing with speed^0.8) and the road (with √speed, four times as
much on a wet track), and a **core** (7 kJ/K: the tread's bulk, carcass,
gas, hub) heated by the carcass flexing (0.8 of `rolling_resistance ·
load · speed`, the first use of that field in physics), cooled by the air
inside the wheel, with 350 W/K of conduction between the two. The slip
power (`|Fx|·slip speed along + |Fy|·slip speed across`,
`TyreWork::power_split_w`) is split at the tyre's peak slip: up to it the
power is the rubber's hysteresis and heats the rubber **by mass**, 30% to
the tread layer and 70% to the bulk (`TREAD_HYSTERESIS_SHARE`); past it
the rubber is sliding, and that friction splits with the road by
effusivity, 30% into the tread (`FRICTION_HEAT_TO_TYRE`). Two reports set
this (2026-10-01 and 10-02): with all the sliding heat in the tread, a
player's understeer at La Source took a GT3's fronts to 150 °C; with 85%
of the sub-peak power in the tread layer, a *careful* LMP2 driver on a pad
gained 10 °C per slight turn and went from 85 to 150 up Eau Rouge, while
the AI, smooth and at the peak, saw 109 on the same lap (an LMP2 front at
2° of slip at 70 m/s dissipates 22 kW: 9 °C/s into that layer). Handing
part of the sub-peak power to the road was tried and rejected, because the
AI at its peak slip sets every class's window: a quarter of it in the
tyre ran the LMP2's fronts at 66 °C at Monza against 82-102, 0.9 at 78.
The tyre keeps the energy it kept; only where it lands changed, and the
surface now follows the bulk over a lap instead of leading it through
every corner (Spa's hottest LMP2 tread at AI pace: 109 -> 99 °C, lap time
unchanged; `a_careful_corner_warms_the_tread_a_few_degrees`,
`a_lap_of_fast_corners_keeps_the_surface_near_the_bulk`). `update_car_3d`
reads the grip from the end of the last tick and steps the heat after the
force solve; the old stateless "temperature" in `update_telemetry_3d` is
gone. A hotlap goes out at the chosen compound's own optimum (a soft 6 °C
under the medium's).

**A pad without the steering aid cooks the tyres, and that is the
handling, not the heat.** The second report (170 °C at Zandvoort in the
994P "just trying to stay inside the white lines", on the model whose AI
peaks at 108 there) was a session whose create-screen chips forbade the
steering aid (`ApexProfile.sav` `AllowedAssists.bSteeringAssist`,
`LockedAssists=1` in the client log): without the aid a pad's stick is
the car's whole 23° of lock at any speed, so at 150 km/h anything past a
third of the travel is beyond the peak slip, and the car is driven on
sliding fronts. `tests/pad_driver_probe.rs` (`#[ignore]`d; `PAD_AID`,
`PAD_NOISE`, `PAD_HOLD_S`, `PAD_SKILL`, `PAD_CAR`, `PAD_TRACK`) drives the
AI's line with a pad's hand error on top, through the aid (the AI's wheel
angle inverted to a stick through `assisted_steering`) or raw: ±0.7° of
hand error held half a second, aid off, puts the 994P off the road 377 s
of 479 at Zandvoort (hottest tread 118 °C while it slides); the same hand
through the aid laps at the same pace, never off, hottest 101; ±1.4°
through the aid is 130 (the left front, past the peak 10% of the lap),
and the same without the aid is 141 and off the road 394 s. The AI with
no hand error is 90 either way. The server has no file log and a hotlap
writes no replay, so a player's lap cannot be read back after the fact:
reproduce it with the harness.

**The aid itself scrubbed the fronts in slow corners** (fixed 2026-10-02,
after the above). The third report was with the aid on: fronts at 169 °C
and 48% worn after one Zandvoort lap, rears cold at 3%. `assisted_steering`
set the stop's lock to the kinematic angle for the tightest turn plus the
slip allowance *plus* the angle the front axle already travels at, and in
a steady corner at the limit the travel angle is the kinematic angle less
the rear's slip, so the stop held the fronts at kinematic + allowance of
slip: a degree extra at speed, 10° at 20 m/s, 18° at 12 m/s (`hold_full_lock`
measured 0.31 rad against a 0.12 peak). Every pad driver leans the stick
on its stop in a slow corner. The lock is now `max(kinematic, travel +
allowance)`, the allowance 0.7 of the peak (what the stop gave at 60 m/s
before, inside the flat top of the curve at 95% of the force), so a fast
corner feels as it did, a crawl still gets full lock, and the stop holds
the fronts at the allowance in every corner
(`steering_assist_never_scrubs_the_fronts_in_a_slow_corner`, the stick
held on the stop at 12, 20 and 30 m/s). The harness's `PAD_FULL` mode
(stick to the stop whenever more than 30% is meant) cannot measure it: the
AI plans for the wheel angle it asked for and never completes a lap on the
stop, before or after.

**Grip** reads 0.7 tread + 0.3 core (`TREAD_GRIP_SHARE`): full inside
`optimal_temperature_c ± temperature_window_c`, less by
`temperature_grip_falloff` per degree outside it (eased in over the first
few degrees; the cold side charged 0.6 of it, because a cold tyre is also
soft and the pressure charges for that; never more than 25% off).
**Pressure is the gas law on the core**, and the garage's pressures are now
the *hot* ones, at the optimum (`tyre_thermal::pressure_kpa`): a cold tyre
is soft (180 kPa set is ~125 at 20 °C), a cooked one over-inflated, and
`pressure_grip_factor` charges both. A tyre at its optimum at the set
pressure grips to the bit what it did before temperatures, which is what
`CarState::new` gives a car nobody fitted (`tyres_fitted`), so the physics
unit tests are unchanged.

**Air and track temperature** come from the session's weather and clock
(`SessionConditions::air_temperature_c` / `track_temperature_c`: 13 °C
before dawn to 23 °C at 15:00 on the sky model's day, the sun adding up to
20 °C to the asphalt through the cloud), baked into the session's
`TrackSurface` with `wet` by `apply_to_track`, like the weather's grip.
Not on the wire yet and not host-pickable: that is the ambient-temperature
item of the gaps list.

**Where the tyres start** (`game_session::fit_tyres`): out of the garage /
on joining a practice (`add_player`) at the car's `blanket_temperature_c`
or the air; on a race grid (`line_up_on_grid`, and `add_player` when the
run ahead is a race, which is how a session set straight into `Race`
still gets it) half way from there to the optimum
(`FORMATION_LAP_WARMTH`: there is no formation lap to drive); a hotlap car
going out (`hotlap_relocate`) at the optimum, so a hotlap measures the car,
not its warm-up, unless the driver asked for cold tyres (the garage's
TYRES OUT toggle, `HotlapRelocate.cold_tyres`: then as from the garage,
and the first lap is the warm-up). Per class (car.toml `[tires]`): F1 91 ± 13 °C on 70 °C
blankets, Hypercar 105 ± 13, LMP2 92 ± 10, GT3 87 ± 10, all without
blankets but the F1. The windows were set where each car runs at the AI's
race pace at Monza (`tyre_temperature_probe`, below); the imported AC cars
take theirs from AC's `PERFORMANCE_CURVE` plateau (`thermal_window`).

**The AI drives to the grip it has**: `CarState::tyre_grip_share` (the
weaker axle's grip against the same tyres in their window at the set
pressure) scales its profile's speeds by its 0.75 power and its grip
budget outright, so cold tyres on lap 1 do not put it into the first
chicane at full-grip pace. The root (what the physics alone would say)
was tried first: the AI survey then found 13-25% more car-seconds off the
road than before tyre temperatures, because the grip moves under the car
through a corner as the tread heats. At 0.75, with fuel, tyres, slipstream
and ride-height aero all in, the survey against the commit before any of
them (87c3c6e, all 234 circuit-car runs): contact 23 374 car-seconds
against 24 166, off the road 11 345 against 11 381, out of shape at 3 s
175 against 197, sliding 5 078 against 4 807. Run by run the off-road
time still leans a little worse (30 runs clearly worse, 16 clearly
better). Single pile-ups dominate a run: a change as small as the
damaged-nose downforce loss moves a class's total by about 10%, so
compare totals, not circuits. Isolating one feature at a time (a scratch
copy with it switched off, `content` junctioned in) is how the root was
caught. At Monza every class is in its window by lap 2; lap 1 costs
0.1 s (F1, blankets) to ~2 s; a steady lap is 0.1-0.5 s off the old
full-grip AI (the hypercars ~1.2 s: they lean on their fronts, which settle
at the top of the window with the pressure up, and their tread spikes to
~150 °C somewhere each lap, a lead worth chasing). The player's racing
line is still planned on warm tyres.

**Wire**: `CompactCarState.tyre_c` and `tyre_kpa` (`[u8; 4]` each, FL FR RL
RR, °C and kPa rounded into 1..=255, 0 = not known; `network::tyre_bytes`)
appended after `fuel_dl`. On the client `FApexCarTelemetry::TyreTempC` /
`TyrePressureKpa` (plain arrays, -1 unknown, `HasTyres()`), the catalog
row's `TyreOptimalC` / `TyreWindowC` from car.toml, and a **Tyres** row
under the HUD footer (`UApexHudWidget::RefreshTyres`): each tread blue
under the window, green in it, amber over it, red 15 °C past it, the
pressure beneath. Golden bytes: `cargo test telemetry_compact_wire_format
-- --nocapture` -> `ApexUdpGolden::S_TelemetryCompactTyres`. Tests:
`tyre_thermal::tests`, `tests/tyre_temperature_test.rs` (every class warms
into its window and keeps its grip, rain runs cooler, blankets / grid /
hotlap starts), `ApexSim.Net.Udp.GoldenDecode`,
`ApexSim.Cars.TomlClientTables`; the harness
`cargo test --release --test tyre_temperature_test tyre_temperature_probe
-- --ignored --nocapture` (`TYRE_PROBE_CARS=a,b`, `TYRE_PROBE_LAPS=N`)
prints each lap's temperatures, grip and time per class.

### Slipstream and dirty air (`server/src/slipstream.rs`)

The third item of docs/SIMULATION_GAPS.md. Once per tick before the
physics (`GameSession::update_air`, beside the DRS rule; the demo lap too)
`slipstream::update` takes a snapshot of every car not in a garage and
sets each car's `CarState::wake` (`Wake { drag, downforce_front,
downforce_rear }`, 1.0 in clean air) from the strongest wake it sits in;
`calculate_aerodynamic_forces` multiplies them in. Behind a car, along its
velocity: the **tow** saves `TOW_MAX` (35%) of the drag bumper to bumper,
falling by e every `TOW_DECAY_M` (30 m); the **dirty air** takes
`DIRTY_AIR_MAX` (25%) of the downforce, falling by e every 15 m, 1.3x of
it off the front wing and 0.7x off the rear, so a close follower also
understeers. Both are Gaussian across a wake half the leader's width
wide at its tail, widening 4 cm per metre; nothing past 100 m, nothing
from a leader under 10 m/s (full by 25), less for a car not pointing down
the wake, and scaled by the root of the leader's drag area over the
follower's (0.6-1.4). The snapshot makes the answer independent of visit
order; it is O(n²) with a distance reject, trivial at 20 cars.

On Monza's straight an F1 10 m behind another gains ~14 km/h in four
seconds (291 -> 305 km/h: drag x0.71, front downforce x0.75;
`tests/slipstream_test.rs`). The AI's profile speeds also scale by the
root of `CarState::wake_load_share` (its weight and downforce now over
what it would have in clean air), so it backs off through a fast corner in
dirty air; its grip budget already read the real downforce. It does not
yet *use* the tow to pass: the traffic layer still caps a follower's speed
to a following distance. The racing line is planned in clean air.

**Wire**: `CompactCarState.tow_pct` (u8, percent of drag saved) appended
after `tyre_kpa`; `FApexCarTelemetry::TowShare` (0-1, -1 unknown) and a
**TOW** badge beside the HUD's DRS light, lit with the percentage from 3%
(`slipstream::TOW_SHOWN`). Golden bytes: `cargo test
telemetry_compact_wire_format -- --nocapture` ->
`ApexUdpGolden::S_TelemetryCompactTow` (the 26-field
`S_TelemetryCompactTyres` stays as an older server's frame).

### Ride-height aero and wings (`server/src/aero.rs`, `[aero]`, the aero knobs)

The fourth item of docs/SIMULATION_GAPS.md. The sim has no body heave (the
body follows the ground plane and the loads are analytic), so the ride
height is worked out: each axle sits at its `[aero]` static height less
its load change over its springs (`aero::ride_heights`, from last tick's
wheel loads, which carry the downforce, braking and acceleration),
stiffening 6x on the bump rubbers past 65% of its height. The map
(`aero::multipliers`): the downforce gains `ride_height_sensitivity` per
cm the mean height is below the reference, the floor gives back up to 40%
below `stall_height_m`, and rake over the reference moves
`rake_sensitivity` of the balance forward per cm; induced drag follows a
quarter of the total change. The **reference** is where the car rides at
50 m/s on its stock springs (`fit_reference`, at load), so there it makes
exactly its car.toml downforce and the class calibration holds: the
Silverstone profile laps moved under 0.2 s and the AI's Monza laps under
0.15 s. A car with no `[aero]` table makes exactly what it did. A damaged
nose loses up to half its front downforce (`FRONT_DAMAGE_AERO_LOSS`).

Per class (car.toml `[aero]`): F1 45/90 mm, 3% a cm, 1% balance a cm of
rake, stall at 12 mm; Hypercar and LMP2 50/80 mm, 2%, 0.8%; GT3 70/90 mm,
1%, 0.6%, stall at 15 mm. On Monza's straight an F1 goes from 39/54 mm at
173 km/h to 13/28 mm at 290 km/h (7% more downforce than filed) and dives
to 10/58 mm under braking (`tests/aero_test.rs`).

The racing line plans with the same map at steady state
(`aero::steady_downforce_factor`, tabulated per 2 m/s on the envelope;
the corner speed is solved again at its own answer), so the AI's plan
follows it. **Setup**: four knobs appended to `CarSetup` (19 now):
`front_wing` / `rear_wing` (5% of that axle's lift a click, 0.5% / 1.5%
drag), `ride_height_front` / `_rear` (2 mm a click, never under 10 mm; the
reference stays the file's). The springs knob now moves the aero too (a
stiffer car squats less). Client: `ApexCarSetup::FrontWing` ...
`RideHeightRear`, an **Aero** section in the hotlap garage. Golden bytes:
`cargo test car_setup_wire_format -- --nocapture` ->
`ApexGolden::C_SetCarSetup`. Not modelled: crests (the loads carry no
vertical acceleration), yaw and roll sensitivity, and porpoising.

### The air: temperature, density and wind (`SessionConditions`, `wind.rs`)

The environmental item of docs/SIMULATION_GAPS.md. `SessionConditions`
gained four optional figures, `air_temp_c` (i8, -5..45), `humidity_pct`,
`wind_kph` (0..60) and `wind_from_deg` (from the start straight's
direction: 0 head-on, 90 from its left); `None` is "from the weather and
the clock" and is left off the wire, so an old client's or server's bytes
are unchanged. `create_session` **resolves** them
(`SessionConditions::resolve`, the wind's direction hashed from the
session id; `replay_tools` seeds it from `--seed`), so `SessionJoined` and
the listing always name every figure. Auto values: the air as before
(13-23 °C over the day, damped under cloud; now whole degrees), humidity
50-97% and wind 8-28 km/h by weather. `apply_to_track` bakes into the
session's `TrackSurface`:

- `air_density_ratio`: moist-air density (standard-atmosphere pressure for
  the track's `metadata.altitude_m`, Tetens vapour pressure) over the
  **reference day** every car.toml is filed at (sea level, a sunny 13:00),
  exactly 1.0 there. It scales the aero (`calculate_aerodynamic_forces`,
  the racing line's envelope) and the combustion torque
  (`physics::engine_density_factor`: all of it for a naturally aspirated
  engine, `TURBO_DENSITY_SHARE` 0.25 for one with `[engine]
  forced_induction`, which the F1s and two hypercars set; an
  `[engine.turbo]` table implies it). Mexico City is 0.78: the LMP2's
  Monza plan is 5.4 s slower there, the turbo F1's top speed 339 -> 349
  km/h (`tests/air_test.rs`).
- `wind_mps` (rotated onto the track by the start line's heading: the
  real circuits keep their real orientation, so the start straight is not
  +X) and `wind_now_mps`, which `GameSession::update_wind` (in
  `update_air`, and the demo lap) sets each tick from `wind::gusting`:
  hash-based smooth noise over the session clock, speed +-35%, veering
  +-15° every few seconds, deterministic.

The physics drives through the air: downforce from the flow along the
car, drag along the relative airflow, and a side force on the flank
(Cs 0.9 on 0.75 of length x height) from air crossing it; a 35 km/h
headwind down Monza's straight costs an F1 13 km/h over four seconds, a
tailwind adds 12. `CarState::aero_load_share` (the tyres' load with this
downforce over the load in still, clean air at this ground speed, capped
at 1) replaced the wake-only share in the AI, so it backs off for a
tailwind into a corner as for dirty air; in a 50 km/h gale it laps Monza
without leaving the road. Humidity has no host control (auto only).
`SURVEY_WIND_KPH=25` runs the AI survey in wind.

**Where a circuit is**: `metadata.altitude_m`, `latitude_deg`,
`longitude_deg` in the track YAML, written by `scripts/track_location.py
--all` from each `.dem.msgpack`'s georeference (altitude = the first
node's z less `datum_offset_m`), with a `MANUAL` table for the four
shipped circuits without a DEM (Mexico City 2240 m). The track editor's
`TrackMetadata` carries the keys so its rewrites keep them
(`the_location_survives_a_rewrite_as_written`). Changing a YAML changes
its checksum: re-run `ats-export --all` after. Latitude is stored, not
yet used (the sun is still the sky model's 50° N).

**Client**: `FApexSessionConditions::AirTempC` / `HumidityPct` /
`WindKph` / `WindFromDeg` (-128 / -1 = auto), written only when picked;
`Describe()` names them ("Overcast · 08:00 · -3°C · wind 22 km/h from the
right"). The create screen has an **Air temperature** slider (first step
Auto) and a **Wind** chip row (Auto / Calm / Light / Breezy / Strong, and a
chip that steps the direction through auto, ahead, left, behind, right).
Golden bytes: `cargo test conditions_air_wire_format -- --nocapture` ->
`ApexGolden::C_CreateSessionAir` / `S_SessionJoinedAir`.

### Tyre wear, compounds and pit stops (`tyre_thermal.rs`, `pit.rs`, `<Stem>.pit.msgpack`)

The next package of docs/SIMULATION_GAPS.md. **Compounds**: every car has
`tyre_thermal::COMPOUNDS`, soft / medium / hard as changes to its own tyre
(soft +3% grip, 1.8x wear, window 6 °C lower; hard 0.97, 0.55x, 6 °C
higher; the medium is the tyre exactly, so every calibration holds).
`CarState::tyre_compound` is what is on the car, set when a set is fitted
(`tyre_thermal::fit`, which also zeroes the wear); the setup's twentieth
knob `tyre_compound` (-1 hard, 0 medium, +1 soft; `CarSetup::
compound_index`, like the fuel load not baked into the car) chooses the
*next* set: in the hotlap garage, on the grid, at a stop. **Wear** grows
with the same friction power that heats the tread
(`WEAR_PERCENT_PER_MJ` 7, x the compound, x `[tires] wear_rate`, 4% more a
degree over the window) and costs grip (`wear_grip_factor`: 6% across
the tyre's life, plus a cliff of 24% from 70% worn, quadratic); the AI's
`tyre_grip_share` reads it, so it slows as its tyres go off. At Monza a
medium wears 2.5-3.5% a lap at the AI's pace (the hypercars' hot fronts
the most). The old stateless `wear_percent` in `update_telemetry_3d` is
gone.

**The pit lane** reaches the server as a sidecar `ats-export` writes
beside the YAML (`ue_export::PitSidecar`, gitignored like the others,
shipped by `build_release.ps1`; `Sidecar::Pit`): the lane's centerline
every 2 m, the limit lines and the box row exactly as `pit_markings`
paints them, a stop spot in the middle of each box's working lane, and
the track stations where the lane leaves and rejoins. 26 of 27 shipped
circuits have one (not the Nordschleife); AC imports have none yet.
`TrackConfig::pit_lane` (`pit::PitLane`) replaced the never-filled
`PitLaneConfig`. **Each tick** (`GameSession::update_pits`, first in
`update_air`) every car is placed on the lane (`PitState`, windowed
search): between the lines `physics::update_car_3d` fades the throttle
out over the last m/s below the limit (80 km/h at Monza: 79.8 held flat
out); a car stopped within 2.5 m of its box's spot (its grid slot's,
shared past the box count) is serviced for `pit::service_seconds` (tyres
2.5 s for an F1 crew, 9 s for others; then refuelling at 2 L/s where the
rules allow it, not an F1: a race's remaining distance with its margin;
then repairs at 0.04 s a percent of damage), held still by the physics;
at the end the new set goes on at the car's start temperature (blankets
or air), the fuel in, the damage off. The pit lane is **on the track for
the lap** now (`RoadContact::off_track`), so a lap with a stop counts.
**The AI** in a race plans a stop (`pit::plan_stop`: a tyre 70% worn,
the nose 25% damaged, or short of fuel for the rest; hards with 15+ laps
left, mediums with 6+, softs otherwise), turns onto the pit route 250 m
before the lane leaves the track, races that run-up on the road held to
the speed the lane wants at its mouth (`pit::run_up_m`, `run_up_input`),
and from 10 m short of the lane `pit::drive_input` drives it (pure
pursuit along the lane onto its box's spot, the limit between the lines,
a stop at the box, the automatic box on for the route) until past the
lane's end, where the normal AI takes over. Pure pursuit from 250 m out
used to cut Monza's Parabolica over the run-off and swerve into the lane
at 37 m/s, into the armco beyond it. At Monza a GT3 AI on 72% worn
tyres pits on lap 1, 9 s in the box, rejoins on softs and races on
(`tests/pit_stop_test.rs`); on most other circuits the AI's stop still
fails (docs/SIMULATION_GAPS.md, "Not done yet").

**Wire**: `CompactCarState.tyre_wear` ([u8;4], %), `compound` (255
unknown), `pit_flags` (bit 0 limiter, 1 servicing, 2 in the lane),
`service_ds` (tenths), appended after `tow_pct` (31 fields). Client:
`FApexCarTelemetry::TyreWearPct` / `Compound` / `bInPitLane` /
`bPitLimiter` / `bPitServicing` / `ServiceSecondsLeft`; the HUD's tyre
row shows the compound in its caption and each tyre's wear beside its
pressure (amber from 70%, red from 90%), and a PIT badge beside TOW goes
amber for LIMITER and green with the SERVICE countdown; the hotlap garage
has a "Next tyres" row. Golden bytes: `cargo test
telemetry_compact_wire_format car_setup_wire_format -- --nocapture` ->
`ApexUdpGolden::S_TelemetryCompactPit`, `ApexGolden::C_SetCarSetup`.

### Brake and engine heat (`brakes.rs`, `engine_heat.rs`)

The brake/engine-thermal item of docs/SIMULATION_GAPS.md (progressive
damage is its own, below). **Brakes**: `CarState::brake_temp_c` per corner,
one thermal mass each, heated by what the pads absorb (the wheel's brake
force, capped at what its tyre carried, times the wheel's own rim speed:
a locked wheel heats its tyre, not its disc), cooled through the duct
(`BrakeMaterial::convection` x the car's `brake_duct_scale`, which the
setup's 21st knob `brake_ducts` moves 10% a click for 0.3% drag) and by
radiation. The pads' grip is `BrakeMaterial::friction(T)`: **carbon**
(F1, Hypercar, LMP2 by `for_class`, or car.toml `[brakes] material`) 0.55
cold, full 400-900 °C, fading past 1000; **steel** (GT3) 0.9 cold, full
200-600, fading past 700. The physics scales each wheel's brake force by
it. Brakes are fitted with the tyres (`fit_tyres`: the air from the
garage, carbon at 300 °C / steel half way to its window on a race grid,
in the window for a hotlap; a car nobody fitted starts warm); a pit stop
does not change them. At Monza at AI pace carbon peaks 650-850 °C and
cools to 250-390 down the straights; steel peaks ~300. An F1 from 288
km/h stops in 109 m on warm carbon, 129 m on cold; a GT3 in 182 m warm,
187 m cold (`tests/brake_heat_test.rs`). **The AI** looks further ahead
by the braking distance its pads lose (`CarState::brake_share`; carbon
judged 100 °C hotter than it is, since it comes in within the first moment
of a stop: judged at its end-of-straight temperature the AI braked early
at every corner and lost 2.5 s a lap, and judged 200 °C hotter it trusted
a race grid's 300 °C carbon into the first corner and the survey's
off-road time rose 6%. At 100, with everything since fuel in, the survey
against 87c3c6e: contact 23 397 car-seconds against 24 166, off the road
11 779 against 11 381, the F1 the one class above its baseline there
(5 019 against 4 625, inside a class's ~10% run-to-run noise).

**Engine heat**: `water_temp_c` (the old stateless engine, oil and water
figures are gone; `engine_temp_c` is the coolant, the oil follows with a
minute's lag) heated by 0.9 W per watt of combustion power plus idle,
cooled by a radiator sized per car (`engine_heat::radiator_conductance`:
at 65% of peak power, 60 m/s and 25 °C air it holds 95 °C; `[engine]
radiator_scale` shrinks or grows it) through the air the car meets, times
`wake.drag`, so a long tow runs it hotter; a fan below 6 m/s. Past 112 °C
the engine gives up 2% of its torque a degree (`power_factor`, floor 60%).
At Monza the classes settle at 84-101 °C. Cars go out at 80 °C.

**Wire**: `CompactCarState.brake_c` ([u16;4], °C) and `water_c` (u8)
appended after `service_ds` (33 fields); client `BrakeTempC` /
`WaterTempC`, a brake line under each tyre in the HUD's tyre row
(coloured from the temperature alone: the client does not know the
material) and a Water cell after it; the garage's "Brake ducts" row.
Golden bytes: `cargo test telemetry_compact_wire_format
car_setup_wire_format -- --nocapture` -> `ApexUdpGolden::
S_TelemetryCompactHeat`, `ApexGolden::C_SetCarSetup`. The probe
(`tyre_temperature_probe`) prints each lap's brake peak, brakes at the
line and coolant.

### Progressive damage (`damage.rs`)

`DamageState`'s five percentages (front, rear, left, right, engine) used to
be inert until 80% front or engine parked the car, and a hit cost at most
5%. Now every hit counts and every zone costs something first:

- **Accrual.** A car-car or wall contact does `damage::impact_damage` of
  its closing speed along the normal: `0.2 x (v - 2.5 m/s)^1.6` (nothing
  under 2.5 m/s, ~4% at 10 m/s, ~40% at 30, ~96% at the 50 m/s cap) to the
  zone the hit came from, and a nose hit puts `NOSE_TO_ENGINE` (0.3) of
  it on the engine. The engine also wears past `engine_heat::OVERHEAT_C`
  (0.08% a second per degree over) and when a missed downshift forces it
  past 1.02x `max_engine_rpm` (`over_rev_damage` from the unclamped
  `physics::geared_rpm`; not in top gear, where a tow down a hill spins it
  there with nobody to blame).
- **Effects.** Front: the existing downforce loss (`aero::
  FRONT_DAMAGE_AERO_LOSS`) and up to 60% of the radiator's cooling
  (`radiator_factor`), so a damaged nose runs hot and then wears the
  engine. Rear: up to 40% of the rear downforce. A side: its tyres up to
  12% of their grip (`side_grip_factor`) and the fronts toed toward it,
  up to 0.025 rad (`toe_offset_rad`): at 60% left a GT3 hands-off drifts
  10 m left in 2 s. Engine: up to 40% of the power
  (`engine_power_factor`, 16% at half). All exactly 1.0 undamaged.
- **Out** when any zone reaches 100% (`DamageState::refresh`); the car
  stops where it is (`update_car_3d` returns early) and is not towed off.
- **The AI** feels the aero through `aero_load_share` (now measured
  against the *undamaged* car, so the loss shows) and plans a stop at 25%
  in any body zone or 20% engine (`pit::AI_PIT_DAMAGE`,
  `AI_PIT_ENGINE_DAMAGE`); a stop repairs everything at
  `REPAIR_S_PER_PERCENT`. The steering pull is corrected by its steering
  loop.
- **Wire**: `CompactCarState.damage` ([u8;5], percent) appended after
  `water_c` (34 fields); client `FApexCarTelemetry::DamagePct` /
  `HasDamage()`, and a **damage panel** left of the HUD's car-state panel
  (`UApexHudWidget::BuildDamagePanel` / `RefreshDamage`, both detail
  levels, collapsed from an older server): a top-down car of five blocks
  (front, rear, the sides, the engine in the middle) grey while sound,
  amber by 25%, red by 60, flashing white for 0.8 s on a fresh hit of a
  percent or more, each zone's percentage beside it (OUT at 100) and the
  driver's damage level in the caption. The damage aid is under
  "Driving assists". Golden bytes: `cargo test
  telemetry_compact_wire_format -- --nocapture` -> `ApexUdpGolden::
  S_TelemetryCompactDamage`.

Tests: `damage::tests`, `tests/damage_test.rs` (the pull, the power, the
heat). The survey against the brake-heat run: contact 22 637 car-seconds
against 23 397, off 12 020 against 11 779, and no car retired on any
shipped circuit in its 3 minutes (it prints `retired N` per race now);
the player's AC imports, whose AI already spends hundreds of car-seconds
in contact, lose one to ten cars a race.

### Visible damage (`Race/ApexCarDamage.h`, `[[damage_part]]`, docs/CAR_MODELS.md, Damage)

The client draws the five damage percentages, with nothing new on the wire:

- **Dents and scuffs** in the four car parents (`BuildCarDamage` in
  `ApexTrackMaterialGraphs.cpp`): custom primitive data on the body
  (`ApexDamage::Cpd*`: each body zone's `Visual` amount, the body's box in
  its mesh frame, a seed) drives a world position offset that pushes a
  zone's panels in and crumples them, faceted shading on the moved surface
  and scuffed paint. Zero damage draws as before; imported-track scenery,
  which shares the parents, has the offset switched off
  (`SetEvaluateWorldPositionOffset(false)`). `ApexMaterialBake` re-bakes a
  car parent without `DamageDentCm` by itself.
- **Parts** that come off: car.toml `[[damage_part]]` boxes (car frame,
  metres; the server ignores them, but they change the car's CRC) split
  the body GLB at runtime (`ApexGlb::SplitByBoxes`,
  `ApexCarContent::LoadBodyPieces`, the body keeping the whole model's
  bounds); past `detach_pct` the race car throws the part off as an
  `AApexCarDebrisActor` (ballistic, `ApexDamage::StepDebris`, ground from
  a trace against the track's collision, 40 s, at most 40), and a repair
  puts it back. `scripts/content/cars/damage_parts.py [--preview]` writes
  the shipped cars' tables from their geometry (wings and noses).
- **Smoke, steam and sparks**: `AApexCarEffectsActor` (one per world)
  draws puffs on instanced engine spheres and cubes with the baked
  `M_ApexCarSmoke` (per-instance opacity, shade, glow, softness);
  `initialize_content.ps1` counts it among the car materials.

`apexsim.car.DamagePreview "F,R,L,Rt,E"` (percent) overrides every car's
damage, `apexsim.car.DamageEffects 0` keeps only the dents, and
`-ApexExecAfter="N=command|N=command"` runs console commands N seconds into
an unattended run (damage arriving mid-race, so the parts fly on camera).
Tests: `ApexSim.Cars.Damage.*` (TOML, split, shares, emitters, debris,
puffs, every repo car's parts).

### Hybrid deployment (`hybrid.rs`, `PlayerInput.ers_mode` / `ers_boost`)

The hybrid used to add its motor whenever the throttle was open and the
battery had charge. `hybrid::update` (called from `update_car_3d` where
`update_hybrid_system` was) now gives the driver a say:

- **Modes** (`ErsMode`, `CarState::ers_mode`, default Balanced):
  *Harvest* never deploys and recovers `HARVEST_SHARE` (0.5) of the regen
  power against the crank on the throttle; *Balanced* deploys from 60% to
  95% throttle, eased out below 35% charge (nothing under 15%) and paced
  against a car.toml lap budget: the motor gives (budget left + 5%) /
  (lap left + 5%) of its torque, capped at 1, so an even spend is full
  power and a spend ahead of it tapers. A step (full power until an
  allowance ran out, then none) cut every F1's motor two seconds past the
  line and the field ran into each other at the first corner, the survey's
  F1 contact at the Nordschleife 1.6 -> 102 car-seconds; *Attack* deploys at
  any throttle until the budget or the battery is gone. The **overtake button**
  (`ers_boost`, held) is Attack whatever the mode.
- **Recovery**: under braking as before (the brakes' own work, free);
  off both pedals `COAST_SHARE` (0.3) of the regen power against the crank
  (a little more engine braking); `heat_recovery_kw` (an MGU-H) charges
  at full throttle for nothing.
- **car.toml `[hybrid]`**: `deploy_kj_per_lap` (spent between two
  crossings of the line: reset when the car's lap share wraps, and when
  `current_lap` changes, since pole's lap 1 starts on green behind the
  line; the four F1s 4000), `deploy_min_speed_kph` (the WEC rule: the two hypercars 190),
  `heat_recovery_kw` (no shipped car). The racing line counts the motor
  only above the minimum speed (`hybrid::plan_power_w`) but still at full
  power, whatever the budget allows.
- **Filled** with the tyres (`fit_tyres` -> `hybrid::charge_full`): a full
  battery and a fresh budget out of the garage, on a grid, for a hotlap.
- **The AI** drives Balanced and holds the overtake button in a race from
  lap 2 within `AI_BOOST_GAP_S` (0.8 s) of the car ahead
  (`GameSession::ai_wants_boost`, `drs::gap_ahead_s`).
- **AC imports** (`ac_car_import/physics.py`): the battery is the motor's
  peak power x `DISCHARGE_TIME` (it used to be `MAX_KJ_PER_LAP`, which is
  the lap budget and is now `deploy_kj_per_lap`, unless absurd like a road
  car's 10^7), `[HEAT] TORQUE_PERC` x the motor's power becomes
  `heat_recovery_kw`; AC's `ctrl_ers_*` delivery profiles and
  `[FRONT_MOTORS]` are warned about, not carried.
- **Wire**: `PlayerInput.ers_mode` (u8, nil keeps the car's) and
  `ers_boost` appended after `flash` (11 fields); `CompactCarState.ers_pct`,
  `ers_lap_pct` (255: none) and `ers_flags` (bits 0-1 mode, 2 deploying, 3
  harvesting, 4 boost) after `damage` (37 fields). Golden bytes: `cargo
  test player_input_headlights_wire_format telemetry_compact_wire_format
  -- --nocapture` -> `ApexUdpGolden::C_PlayerInput`,
  `S_TelemetryCompactErs`.
- **Client**: actions `ErsMode` (pressed: steps Balanced -> Attack ->
  Harvest, `ApexErs::NextMode`; M, pad D-pad right, a wheel slot) and
  `ErsBoost` (held: Space, pad A, a wheel slot), rebindable on the
  Controls and Wheel tabs like DRS; the race director steps the mode from
  what the server reports (`LocalErsMode`, `ErsModeSwitch`, reset with the
  headlight switch) and plays the Adjust cue. HUD: an ERS badge beside PIT
  ("ERS BAL 64%  LAP 72%", lit while the motor drives, "OVERTAKE" while
  held, amber ink while harvesting) and an Ers battery bar beside the
  pedal bars, both collapsed on a car without a hybrid.

Tests: `hybrid::tests`, `ApexSim.Net.Udp.*`, `ApexSim.Net.Ers.Modes`, the
importer's `ErsImportTest`. The survey against the damage run (only the F1
is a hybrid there): F1 contact 7 876 car-seconds against 7 937, off 4 303
against 4 993, sliding 1 856 against 1 581; on the shipped circuits every
F1 run is as before but SaoPaulo, where a lap-1 wall graze now happens and
the two damaged cars retire into the walls later (the AI does not drive
around its damage).

### Hotlap (`GameMode::Hotlap`, `HotlapRelocate`, `GhostLap`, `UApexHotlapWidget`, `AApexGhostCarActor`)

Time attack, with the create screen's tile where "Demo lap" used to be (that
mode is still on the server and still drops its human players to spectators;
the menu no longer offers it). A hotlap session is any session counted into
`GameMode::Hotlap` — the client sends `SetGameMode` outright when asked to
`StartCountdown` into it, since there is no grid to count down from — and it
can be multiplayer: several drivers hotlap on one track, each with their own
garage.

**Two sides of the wall.** Every human starts *in the garage*: the car is
parked on its grid slot with `CarState::in_garage`, not ticked and left out
of both collision passes, and telemetry says so in `lap_flags` bit 2
(`LAP_FLAG_IN_GARAGE`, appended semantics: an old client reads it as a clean
lap). `ClientMessage::HotlapRelocate { destination, cold_tyres }` moves the
car (`cold_tyres` is optional and only written when set, so an older
client's bytes are unchanged; it sends the car out on its blankets or at
the air with cold brakes instead of at the compound's optimum):
`Track` puts it on the centerline `HOTLAP_RUNUP_M` (300 m) before the line,
in first, so the first flying lap starts timed as it crosses; a second car
going out is queued `HOTLAP_SPACING_M` behind the first slot still occupied
(`hotlap_runup_pose`, `physics::pose_at_station`). `Garage` parks it again,
repaired, with its bests kept (`hotlap_relocate`). Any participant may send
it; outside a hotlap it is refused with `Error 400`. The AI, if any, is
left driving. `tests/hotlap_test.rs` covers all of it, over the wire too.

**The client** (`UApexRootWidget::HandleTelemetryForHotlap`) reads the
local car's `bInGarage` and opens or closes the garage: `UApexHotlapWidget`
is the layer between the HUD and the pause menu, showing the garage
(a full-screen sheet: GO OUT, REPLAY LAP, GHOST CAR, TYRES OUT (Warm /
Cold, `UApexSettingsSave::bHotlapColdTyres`) and RESET SETUP down the
left, the four setup tabs on the right; see "Car setup"), the lap-by-lap
timing sheet on the track (every `LapTiming` lap end for the local car, with
the delta to the best legal lap and struck laps greyed; it survives trips to
the garage and clears when a hotlap begins) and the replay strip. While the card is up the HUD is hidden, driving input is off and the
card owns the keys like the pause menu (`IsGarageOpen`, honoured by
`FApexMenuInputProcessor`); on the track the pause menu gains BACK TO
GARAGE. Other drivers' garaged cars are hidden by the race director.

**The ghost.** `ClientMessage::RequestGhost` asks for the driver's record
lap on this track in this car and is answered with
`ServerMessage::GhostLap` (`GhostLapData`: struct-of-arrays, `t_ms`, a
six-float `pose` per sample, speed, steering, gear, rpm; empty when there is
no record; read from the store off the loop). The client asks on entering a
hotlap and again on every `LapRecord` that is new and has a trace.
`AApexGhostCarActor` is a race car actor (catalog mesh, wheels, cockpit)
whose pose comes from a clock read against the lap (`FApexGhostLap::SampleAt`,
yaw the short way round the seam, gear stepped), tinted cold and glowing
because the imported glTF materials cannot go translucent at runtime, muted,
and hidden while it overlaps the player's car. In a hotlap the race director
runs its clock from the local car's lap time (eased, snapped on a new lap),
so the ghost is where the record lap was at this point of *this* lap; the
garage's GHOST CAR toggle is `UApexSettingsSave::bGhostCar`.

**Replay.** REPLAY BEST LAP (`AApexRaceDirector::BeginGhostReplay`) drives
the ghost round its lap in real time from the line, with the camera cutting
chase → trackside → onboard → trackside (`CutReplayShot`; a trackside shot
stands ahead of the car, off to the side, and cuts once the car is past),
the followed car being the ghost for the duration; the pause key stops it
and the lap's end ends it. Golden bytes: `cargo test hotlap_wire_format --
--nocapture` → `ApexGolden::C_HotlapRelocate` / `C_HotlapRelocateCold` /
`C_RequestGhost` / `S_GhostLap`, and `ApexUdpGolden::S_TelemetryCompactGarage` (the lap-flags
blob with bit 2 set); `ApexSim.Net.Protocol.GhostLap` and
`ApexSim.Net.Udp.LapFields` decode them.

Checking it without a keyboard: `-ApexAutoRace -ApexMode=8 -ApexAiCount=0`
counts into a hotlap, `-ApexHotlapOutAfter=N`, `-ApexHotlapGarageAfter=N`
and `-ApexHotlapReplayAfter=N` press the garage's buttons N seconds in
(comma lists), and `apexsim.hotlap.Out|Garage|Replay|Stop` do the same from
the console. A ghost needs a record: `cargo test --release --test
hotlap_test generate_ghost_fixture -- --ignored` writes an AI lap round Monza
in the LMP2 into `APEXSIM_GHOST_DIR` (the server's `[records] dir`) under
`APEXSIM_GHOST_PLAYER` (default `Player`).

### Force feedback (`server/src/feedback.rs`, `Input/ApexForceFeedback.h`)

The server works out what the driver should feel, because only it has the
tyre forces. Every tick `update_car_3d` records a `FeedbackTick` into
`CarState::feedback`: the steering-column torque, slip per wheel as a
multiple of the tyre's peak, the surface under each wheel (road/curb/off,
from the same `contact_surface` that sets the track limits), suspension
speed, ABS/TC activity, and contact closing speed. On each telemetry tick the
game loop drains it into a `DriverFeedback` for the car's human driver:
positional encoding, UDP only (no TCP fallback: a late force is worse than
none), every torque sample kept and the transients peak-held. Golden bytes
live in `network.rs` and `ApexUdpGolden::S_DriverFeedback`.

The torque (`physics::steering_column_torque`; 1.0 = the front axle at its
static grip limit, positive turns the wheel left) is summed per front tyre
from that tyre's own forces and load:

- **aligning**: `-Fy x (pneumatic + caster trail)`. The pneumatic trail is
  the contact patch, growing with the square root of the tyre's load, times
  a shape that is zero at 1.4x the peak slip angle and slightly negative
  past it; caster is 0.35 of it and never shrinks. So the rim crests at
  about half the peak slip and is a third lighter by the grip peak (the
  understeer cue; it used to fall only 14%, which nobody could feel), and
  the loaded outside tyre, braking and downforce make it heavier.
- **scrub**: a front tyre braking harder than its partner tugs the rim
  toward it; equal forces cancel.
- **jacking**: the axle's weight centres a steered wheel, which is what a
  crawl or a hairpin feels.

A hit adds `steer_kick` (appended, so an older client skips it):
`physics::impact_steer_kick` from the velocity change a car-car or wall
contact gave the front axle, yanking the rim toward the side that was hit.

**The torque is a round trip late**, so on its own it is a spring that
answers late: let go of the rim in a corner and it held still for a few
hundredths, then jumped, and a delayed spring rings. Three fields appended
after `steer_kick` fix that: `steer_input` (the input the newest sample was
worked out at), `steer_stiffness` (`physics::steering_column_stiffness`, the
torque's slope per unit of input there: each front tyre's lateral force
re-solved on the magic formula at the Ackermann angles either side, under the
same share of the friction ellipse, times the aid's own slope when the aid is
on) and `front_load` (front axle load over static). The client adds
`slope x (input now - steer_input)` to the torque (`ApexFfb::MixWheel`,
`FWheelState::Correction`), so the rim answers its own movement at once and
only the car's motion arrives late. Only a slope that pushes back is carried
(past the aligning crest a local positive slope feeds itself), capped at 0.15
of input and faded in from 3 to 12 m/s (at a crawl the car turns with its
wheels, so the slope is a spring that is gone a moment later). On the Zomba
at 50 m/s the slope is -21 per unit straight ahead and doubles to about -37
under hard braking with the front at 2.4x static: that is what braking feels
like through the rim. `ApexSim.Input.ForceFeedback.WheelLetGo` lets go of a
rim in a corner against a 40 ms, 60 Hz server: home inside 0.3 s and settled,
where the torque alone still swings. `column_stiffness_predicts_the_torque_after_the_rim_moves`
checks the slope against the torque one tick after a real step.

**Road texture.** The physics road is smooth, so nothing came up the column
on asphalt. The wheel now plays a road laid along the lap
(`RoadTug`: value noise at 0.45, 1.6 and 6 m wavelengths, left front minus
right, octaves that would pass faster than half the frame rate dropped),
read at the telemetry's `TrackProgress` run on by speed between samples, so a
bump is in the same place every lap. It rides on the constant force after the
soft limit (so a saturated corner still has a road under it), scaled by Road
effects, speed (full at 40 m/s), `front_load^0.7` (braking passes more of the
road up) and 3x off track. **Braking grain**: the fronts' braking slip from
half to all of their peak (`FrontBrakeSlip`) is a 62 Hz grain on the
vibration channel, under the ABS pulse train.

`UApexPlayerController` logs a `Wheel forces over 15 s driving:` line (torque
in, constant force out, how often at the base's limit, the correction, the
road, the vibration, the settings) so a report of "it feels weak" can be read
against what the base was actually asked for.
`cargo test --release --test grip_probe_test steering_feel_probe -- --ignored
--nocapture` (`PROBE_CAR=`) prints torque against lateral g and front slip
while a car winds on lock: the harness to tune the constants against.

On the client `UApexNetSubsystem` merges everything that arrived since its
last tick (`FApexDriverFeedback::Absorb`) and `ApexFfb::MakeSignals` turns it
into device-agnostic signals, which two mixers read.

`ApexFfb::MixGamepad` drives a pad's two motors: front slide on the light
motor, rear slide on the heavy one, curb ribs at a speed-set rate, ABS/TC
pulse trains, grass noise, and decaying thumps for bumps, contact and shifts.
It assumes XInput's channel layout; the GameInput plugin would put the Small
channels on the trigger motors. Strength is the Controls tab's "Pad
vibration" slider (`UApexSettingsSave::Vibration`, 0.5 = as designed), which
pulses the pad while dragged.

`ApexFfb::MixWheel` drives a wheelbase: the torque *is* the feel (the rim
going light is the front tyres letting go), 0.6x the base's peak at the
reference at Force 50% and soft-limited past 0.8 so a car loaded past its
reference still feels stronger rather than clipping. (It was raised to 1.1
while no force reached the motor at all — see the traps below — and at 1.1
with that fixed 25% was undrivable on an 8 Nm base.) A **stiffness limit**
(`MaxRimStiffnessPerDeg`, 0.025 of the base per rim degree, from
`FWheelTuning::RimDegreesPerInput`) scales the whole torque down where the
tyres' slope would make the rim stiffer than the delayed loop can hold: a
hypercar at 50 m/s (slope -21) at Force 50% is 0.05 per degree, and let go
on a ClubSport V2.5 it rang at +-16 degrees for good whatever the damper;
at 0.025 it settles, braking at twice the slope included. Corners keep
their weight (near the grip limit the slope is small). While driving the
damper never drops under 0.3 of the force (`MinDriveDamper`): a belt rim has
almost no friction and overshot the centre 8.6 degrees at 0.05, 4.5 at 0.3.
The torque is and smoothed with a 12 ms pole
because the samples arrive in 60 Hz lumps and a direct drive base feels that
as grain. A hit's `SteerKick` is added unsmoothed and decays over 70 ms, and
sliding fronts put a quiet 55 Hz scrub on the vibration channel. On top of it one vibration channel
carries whichever of curbs, grass, ABS, lockup or a hit is loudest, plus a
damper that is heaviest at a standstill (35% of the setting at speed, never under the floor above:
a damper reads as a heavy wheel, not as cornering) and a centring spring used only in
the menus. Its settings are the Wheel tab's Force / Road effects / Damping /
Direction (`UApexSettingsSave::WheelForce` and friends).

`AApexPlayerController::UpdateForceFeedback` runs both every frame — it is
the one hook that ticks with no pawn, no race and a pause menu up.
`apexsim.ffb.Debug 1` prints the signals, the motor levels and the wheel's
forces on screen.

### Wheels and pedals (`Source/ApexSimInput/`)

XInput is the engine's pad path and knows nothing else; every wheelbase,
pedal set, shifter and button box on Windows speaks DirectInput. The
`ApexSimInput` module is an input-device plugin like the engine's XInput one:
`FApexDirectInputDevice` is created on the platform application's first poll,
ticked every frame, and told about WM_DEVICECHANGE, so a wheel plugged in
mid-session works without a restart. XInput devices are skipped (their path
carries `IG_`), or an Xbox pad would arrive twice with its triggers merged.

Every control is an ordinary `FKey` — `DInput1_X`, `DInput2_Button7`,
`DInput1_Hat1Up`, registered with `EKeys` at module startup — so Enhanced
Input, the rebinding screen and the settings slot need no second input path.
The number is a device **slot**, not an enumeration index: a slot belongs to
one physical device by instance GUID (falling back to the product GUID when
the same wheel moves USB port) and is remembered in
`Saved/ApexInputDevices.json`, so plugging in a button box cannot shift the
pedals' bindings onto it.

Three things about how it reads devices:

- **Axes are sent when they move and every frame they rest away from zero.**
  A pedal rests at -1, and `FlushPressedKeys` at the end of a race clears the
  player input's key state, so the resting value has to keep arriving.
- **A pedal's -1..1 is folded into 0..1 by a mapping modifier**
  (`UApexInputModifierPedal`), never by the handler — the same action is also
  fed by a trigger and a key, which are 0..1 already. The pad's deadzone and
  steering curve are likewise a modifier (`UApexInputModifierPadSteering`) so
  a wheel does not get a thumbstick's deadzone; both read the settings live,
  so dragging a slider needs no context rebuild.
- **A lost device sends its axes back to where they were when it arrived**,
  not to zero: zero is half throttle to a pedal binding.

Force feedback is taken **late and given back**: devices are opened shared,
and only the wheel the steering is bound to is taken exclusively, the first
frame a race asks for forces (`AcquireForForces`). That is what lets an open
editor and a launched game share a wheelbase. The centring and gain
properties are only touched once exclusive access has been granted (a
refused attempt used to switch another program's wheel's centring off and
on every 3 s).

The driver is treated as fragile, because a Fanatec driver hung twice
(reboot needed) while every effect was sent every rendered frame: the
constant force goes at most 250 times a second, the sine, damper and spring
30 (starting and stopping never wait), a refused update is logged once and
backs off 0.5 s, twenty refused rounds hand the wheel back, a lost device is
asked to reacquire twice a second rather than every frame, and it is torn
down and reopened only after 2 s of failed reads rather than 30 frames. Effects are a constant force, a
sine, a damper and a spring, updated only when they change. Three things
stop a wheel pulling when the game does not: a watchdog on the game thread
drops every force after 0.3 s without an update (a hitch); the constant
force is created with a 0.5 s duration *on the device* and restarted every
0.15 s while it plays (`ApexDirectInput::PlanConstantSend`), so a hung game
thread, a crash or a killed process stops it in the base itself (the damper
and spring only resist the rim, and the sine is zero-mean, so they stay
infinite); and on a crash `OnHandleSystemError` asks a thread made for the
purpose to send `DISFFC_STOPALL`, waiting at most 250 ms so a DirectInput
lock held by the crashed thread cannot hang the crash handler. A NaN is
never a force: `FMath::Clamp` returns its upper bound for one, so
`MixWheel` cleans its inputs and resets a poisoned state, and
`ApexDirectInput::SanitiseEffects` zeroes anything non-finite before the
hardware (`ApexSim.Input.ForceFeedback.WheelNotANumber`,
`ApexSim.Input.DirectInput.ConstantSchedule` / `.Sanitise`). Which way a positive force turns a rim is not
something DirectInput promises — hence the Wheel page's Direction test, which
pushes right and asks.

Bindings live in the settings overlay's **Wheel** tab (devices, forces, and
the wheel's own slots with live meters on steering, throttle and brake);
slots 4-6 are the wheel column. They are kept **per device**: rebinding with
one base plugged in leaves the other base's mapping alone, and unbinding
unbinds the device in front of the player
(`ApexInput::FindBinding`/`StoreBinding`). Binding an axis reads how far it
*moves*, not where it sits, and the direction of the move is what sets
`FApexKeyBinding::bInvert` — which is how a pedal that rests at the top of
its travel configures itself.

`apexsim.input.Devices` lists what is attached, with slots, capabilities and
live readings; `apexsim.input.Rescan` enumerates again. `ApexSim.Input.*`
automation tests cover the slot registry, the key names, the readings, the
binding rules and both mixers.

**Steering lock.** The Wheel page's "Wheel rotation" (what the base's own
driver is set to, default 900°: DirectInput reports only where the rim is
between its ends) and "Steering lock" (rim degrees lock to lock for the car's
full lock, default 480°) make a gain, `ApexInput::WheelSteeringScale`
(rotation / lock, never under 1), applied by `UApexInputModifierWheelSteering`
on the wheel's steering mapping. Before it a 1080° base needed 540° of rim for
full lock. The lock's first step, and the default, is **Auto (per car)**
(`UApexSettingsSave::bWheelSteeringLockAuto`): twice the driven car's
`[cockpit] wheel_lock_deg` (AC's `STEER_LOCK` on an import; every shipped
car names one: 180° F1 / Hypercar / LMP2, 270° GT3), pushed each frame by
`AApexRaceDirector::UpdateSteeringLock` through
`UApexSettingsSubsystem::SetCarSteeringLock`
(`GetWheelSteeringLockDeg` is the lock in use). The same frame hands the
cockpit rig that lock (`AApexCockpitRig::SetDriverRimLockDeg`) while a wheel
steers the player's own car, so the rim on screen turns exactly as far as
the one in their hands, a manual lock included. One lock for every car used
to show a Group C car's 360° each way at 1.5x the player's rim. Past half the lock either way `MixWheel` puts a soft stop on the
rim (0.9 of the base over 6°, with a damper), from the rim angle the player
controller reads off the device (`ApexInput::ReadWheelSteering`,
`FSignals::RimDegrees`). The same reading **centres the rim** when a car
arrives standing still (a session starting, leaving the hotlap garage): a
position loop on the constant force, damped by the rim's speed, let go once
it has settled, the car rolls or 3 s pass (`ApexFfb::RequestCentre`).
DirectInput's own spring is useless for that: its force is a share of the
base's whole travel, a few percent for a rim a quarter turn off.

One base can be two DirectInput devices: Fanatec's driver shows a ClubSport
V2.5 as HID collections COL01 (108 buttons) and COL02 (63 buttons, 4 hats),
both claiming forces; forces go to whichever the steering is bound to, which
should be COL01. Each device's HID path is in the log and in
`apexsim.input.Devices`.

**Two traps found on the user's ClubSport V2.5, both invisible from the
code.** (1) DirectInput gives a force's direction as where it comes *from*:
a positive constant force on the steering axis pushes the rim toward
negative X (left). The device layer sent the mixer's "positive = right"
unflipped, so every car's self-centring pushed the rim *away* from centre
and a straight line had to be balanced by hand; `ApplyEffects` now sends
`-Constant`, and the Wheel page's Direction toggle is for a driver that
breaks the convention. (2) After a Fanatec driver-package update the base's
firmware must be updated in the Fanatec App's firmware manager: until then
every DirectInput call succeeds and the motor plays nothing, while the
Fanatec app's own FFB test works. Rule out both on the hardware before
tuning the mixer: a standalone DirectInput probe that plays a constant force
and reads the rim (the first `GetDeviceState` after `Acquire` reads 0, so
settle before measuring) answers in seconds.

There is no wheel support for an H-pattern shifter (the wire protocol's gear
field is filled from a shift delta, not an absolute gear).

### Content (`content/`)
- `cars/` - Car physics definitions (TOML: `car.toml` per car; most physical parameters moddable with validated ranges): `cars/default/<folder>` the shipped cars, `cars/custom/<folder>` the player's own (gitignored but for its README, read after `default/`, shipped only with `-IncludeCustomCars`)
- No generator scripts live in `content/`: the Blender builders and texture generators that write the cars, wheels and prop kit are in `scripts/content/{cars,props,wheels}` (libraries `carlib.py`, `apex_props.py`, `apex_tex.py` beside them). The Blender ones find the repo through `APEXSIM_ROOT` (default `E:pexsim`), since `exec(open(...).read())` gives them no `__file__`; the plain-Python ones (`liveries.py`, `gen_graffiti.py`, `gen_brands.py`) from their own path. Either way they write into `content/`
- `hud/default/` - the race HUD's components, one folder each (`hud/custom/` the player's own); see "Race HUD"
- `tracks/default/` - the shipped circuits: YAML, `.ats`, dossier, DEM and the generated sidecars side by side
- `tracks/custom/` - the player's own tracks (imported or hand-made), same layout, gitignored but for its README and not shipped unless `build_release.ps1`/`build_game_standalone.ps1` get `-IncludeCustomTracks`. Every tool walks `default/` then `custom/` (`ue_export_io::TRACK_DIRS`, `scripts/track_dirs.py`, `scripts/lib/ApexTracks.ps1`); a stem must be unique across both (`ats-export --all` refuses a shared one, since exports are keyed by stem), and a custom track reusing a shipped `track_id` is skipped by the server with a warning
- `build/tracks/` (outside `content/`, gitignored) - the client exports baked from both; `.cache/osm` and `.cache/dem` hold the raw OpenStreetMap and elevation downloads
- Shared between server and clients

## Key Technical Details

- **Coordinate System**: Right-handed. Origin at track start/finish line center. +X is track direction, +Y is left of track. Angles (yaw) counter-clockwise from +X.
- **Serialization**: MessagePack via `rmp-serde` (Rust) and custom serializer (C#)
- **Physics**: 4-wheel 3D model with per-wheel loads and suspension; fixed timestep dt = 1/tick_rate (plumbed from config, not hardcoded)
- **Determinism**: `participants` is a `BTreeMap` (ordered iteration); AI noise is hash-based; no env/wall-clock reads in the sim path. Guarded by `tests/determinism_test.rs`
- **Tick timing on Windows**: the game loop sleeps between ticks on a tokio `interval`, and a Windows sleep is only as fine as the process's timer resolution (15.6 ms by default, per process since Windows 10 2004). `timer_resolution::HighResolutionTimer` holds 1 ms for the loop's lifetime; without it 240 Hz ran at 64 and the sim at 27% of real time. The loop warns when the achieved rate over 5 s drops below 90%; `test_session_ticks_at_the_configured_rate` guards it end to end
- **Hot loop**: nearest-centerline queries use a windowed search seeded by each car's cached index (`CarState::nearest_centerline_idx`) — keep new per-tick track queries on this path
- **AI Drivers**: deterministic synthetic input per tick from line look-ahead. The field races in the host car's `class` from `car.toml` (`game_session::class_field`: every car of that class, dealt round-robin by id starting after the host's; a car with no class races only against itself), and each `RosterEntry` carries `CarConfigId` so the client's race director draws every car with its own catalog mesh (an older server's roster falls back to the local player's car)
- **Bounded queues**: Network channels use bounded MPSC to prevent OOM; droppable messages (telemetry) may be dropped for slow clients

## Configuration

Server config in `server.toml` (validated at startup; the server refuses to start on a present-but-invalid file):
- `[server]`: `tick_rate_hz` (default 240), `max_sessions`, `session_timeout_seconds`
- `[network]`: TCP/UDP/health bind addresses, TLS cert paths (`require_tls` fail-closed default), heartbeat settings
- `[content]`: paths to car/track manifests
- `[logging]`: level, `console_enabled`, optional `file_enabled`/`file_dir` (JSON-lines, daily rotation)
- `[physics]`: `road_contact = "mesh"` (default) or `"centerline"` — whether a track with a baked `<Stem>.road.msgpack` drives on it (see the road mesh sidecar under "Track pipeline into Unreal", and docs/ROAD_MESH.md)
- `[auth]`: `mode = "dev"` (accept all, development only) or `mode = "token"` with shared secrets in `tokens`
- `[ai]`: AI driver defaults (optional)

Environment overrides use the `APEXSIM_` prefix, e.g. `APEXSIM_NETWORK_TCP_PORT=9100`, `APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9000`, `APEXSIM_SERVER_TICK_RATE_HZ=120`, `APEXSIM_PHYSICS_ROAD_CONTACT=mesh` (see `ServerConfig::apply_env_overrides`).

## Testing Notes

- `proptest` property tests live in `tests/physics_property_tests.rs` (physics invariants, serialization roundtrips)
- `tests/grip_probe_test.rs` (ignored) is the grip-tuning harness: skidpad lateral-g sweep, a follower driving Silverstone's first corner at the racing line's speed, and every car's ideal-lap time from its racing-line profile (`PROBE_MU_SCALE` / `PROBE_CLA_SCALE` / `PROBE_CLASS` sweep grip and downforce without editing TOMLs). Class grip levels are set so those lap times land a few seconds off real poles
- Integration tests simulate real client connections with `TestClient` structs against an in-process server
- Enable debug logging: `RUST_LOG=debug cargo test ...`
- CI (`.github/workflows/ci.yml`) runs fmt-check, clippy (`-D warnings` — keep the tree warning-free), build, and tests for the server; Dependabot auto-merge builds and tests before merging
