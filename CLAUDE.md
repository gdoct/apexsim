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
`Blueprints/`), the two catalog tables in `Data/`, the splash (`Splash/`) and
three legacy hand-imported cars are checked in; the tracks, props, ground
textures and most car meshes are **generated** from `content/` by commandlets,
so a fresh clone opens to a menu with no car meshes and races in an empty world
until they have been run. One script does all of it, with Rust, Python 3
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
3. `import_cars.ps1` - cars whose `/Game/Cars/<folder>/SM_<folder>` is missing
   (every car when a class wheel mesh is missing).
4. `bake_ground_textures.py` (the PNGs are normally checked in) and
   `-run=ApexGroundTexImport` -> `/Game/Ground`.
5. `-run=ApexMaterialBake` -> the four track parent materials under
   `/Game/Materials/Track`, when one is missing or stage 4 ran (the base
   parent's surface graph depends on the ground textures).
6. `-run=ApexPropImport -all` when any kit GLB has no mesh.
7. `build_track_levels.ps1 -Release` for every circuit missing its level, its
   runtime export (`.uescene.json` + `.uemesh`) or a
   `{ground,curbs,walls}.msgpack` sidecar (which the server needs) - and for
   every circuit when stage 4 or 6 ran, because the bake picks the ground
   material and the import resolves the props from what `/Game` holds then.
8. `build_track_catalog.py` + `-run=ApexTrackCatalogSync` when a track was
   rebuilt or a `T_Track_<Stem>` preview is missing.

Stage 7 is the long one on a fresh clone (all circuits). The data refreshes in
the track sections below (`osm_layout.py`, `dem_fetch.py`, `dem_elevation.py`,
`ats-smooth`, `ats-bank`, `drs_zones.py`) are **not** part of it: their outputs
(dossiers, DEM sidecars, YAMLs, `.ats` scenes) are checked in.
`build_release.ps1` does not import cars or ground textures, so run this script
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
`scripts/build_game_standalone.ps1` runs UAT BuildCookRun into `artifacts/ApexSim-Win64`.
Packaging relies on `bCookAll=True` in `DefaultGame.ini`: the track levels and
catalog tables are only ever found by path at runtime, and the cooker's
`DirectoriesToAlwaysCook` scan ignores `.umap` files, so without cook-all a
packaged build races in an empty world with no previews.

### Release package
`scripts/build_release.ps1` runs the whole pipeline front to back � server,
track levels, track catalog, client package � and assembles a folder a player
can unzip and run:

```powershell
./scripts/build_release.ps1 -Zip                     # artifacts/release/ApexSim-<ver>-Win64[.zip]
./scripts/build_release.ps1 -SkipClient -SkipTracks  # reuse what is already built
./scripts/build_release.ps1 -RuntimeTracks -Zip      # circuits as data in Game/Tracks, no level import
```

With `-RuntimeTracks` the circuits ship as exports the game builds itself
(see "Runtime tracks" below): the bake stops before the Unreal import,
`ApexMaterialBake` runs so the cook carries the shared track materials, and
`Game/Tracks/` gets each circuit's `.uescene.json`, `.uemesh` and preview
`.png`. Levels already imported are cooked too and still preferred, so delete
`game-unreal/Content/Tracks` first for a package whose circuits are all
runtime-built.

Layout: `Game/` (the packaged client, plus a `settings.sample.yml`), `Server/` (`apexsim-server.exe`,
`server.toml` and only the content the server reads � `car.toml` per car and
the track YAML, not the `.glb` models or `.ats` sidecars), plus `Play.bat`,
`Start-Server.bat`, `README.txt`, `LICENSE` and `release.json`.

The run aborts before any long build if the car or track data is missing, or if
a track YAML has no `track_id`. Every stage has a `-Skip*` switch for when one
piece is mid-refactor, but a skipped stage must still find the output it would
have produced � a `-SkipTracks` run checks `L_<Stem>.umap` exists for every
circuit rather than shipping a package that races in an empty world.
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
beside its YAML, `content/tracks/real/<Stem>.layout.json`: named corners, the
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
gitignored, under `content/tracks/osm-cache/`.

`ats-dress` turns a dossier into scenery and then grooms around it:

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
                                            # or: content/tracks/real/Spa.yaml [--dry-run]
```

It owns every prop of the kinds it lays (grandstand, building, attraction,
bridge, light, vehicle, sky) plus the pit lane, deleting and re-laying them
each run — so it also clears the old scatter — while barriers, tire walls,
distance boards and trees stay `groom`'s. It also owns, *by asset rather
than by kind* (`dress::dressed_prop`, `surroundings::OWNED`), the furniture
it lays from the new layers: cars and lamp posts in every mapped car park,
tents and campers on the camp sites, houses and barns across the villages
and farmyards, chapels and pylons from `poi`, a marshal post at every named
corner and every 400 m besides, a `corner_sign` carrying each corner's real
name, and — only for a circuit whose dossier has no lighting masts of its
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

It writes `content/tracks/real/<Stem>.dem.msgpack`: an `inner` grid at 10 m
over the circuit and a kilometre around it, and an `outer` grid at 90 m out
to eight kilometres, both in the server frame (the model is offset so it
agrees with the YAML's own z at the start/finish line). Unlike the other
sidecars this one is **checked in**, because regenerating it needs a few
hundred megabytes off the network; the raw tiles under
`content/tracks/dem-cache/` are gitignored.

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
cargo run --manifest-path track-editor/Cargo.toml --release --bin ats-bank -- content/tracks/real/Zandvoort.yaml
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
python scripts/osm_layout.py --all --offline                                  # dossiers (fit to the centerline)
python scripts/dem_fetch.py --all --offline                                   # elevation (same fit, same datum)
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
PNGs from `content/props/_tools/gen_graffiti.py`), laid by `ats-dress`
from the dossier's `graffiti` (`MANUAL_GRAFFITI`), baked road-hugging by
`ats-export` (family `decal`) and drawn with a masked `M_ApexDecal`
(`ApexPropImport -kind=decal` imports the textures). The kit pieces are
built by `content/props/_batches/build_nordschleife_kit.py` (headless
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
Circuits reach the Unreal client in two generated steps; both outputs are
regenerated wholesale and neither should be hand-edited.

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
                                                         # -> content/tracks/real/*.ats
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
                                                         # -> content/tracks/export/*.{uescene.json,uemesh} (gitignored)
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject \
    -run=ApexTrackImport -all                            # -> game-unreal/Content/Tracks/<Track>/L_<Track>.umap
```

`scripts/build_track_levels.ps1` runs all three steps (optionally building
the `ApexSimEditor` target first). The dressing stage is new and is there
because the dossier-to-scene step used to be manual and silent: the Red
Bull Ring's bull statue went into the dossier, nobody re-ran `ats-dress`,
and the level was baked from the previous scene. Dressing is idempotent, so
running it every time costs seconds and removes the failure mode
(`-SkipDress` if you are editing a scene by hand). and finds the engine install from the
`.uproject`'s `EngineAssociation`; `-Track A,B` narrows it to a few circuits,
`-DryRun` reports without writing assets. Note that `ats-export` resolves
`content/tracks/{real,export}` relative to the working directory, so it must be
run from the repo root — not from `track-editor/`.

The exporter also writes three gitignored sidecars (like the exports) into
`content/tracks/real/`, all loaded by the server from beside the YAML and
all shipped in `Server/` by `build_release.ps1`:

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

### Runtime tracks (`UApexTrackContentSubsystem`, `UApexTrackInstance`, docs/RUNTIME_CONTENT_LOADING.md)

A circuit reaches the game either as a cooked level or as its export,
built by the running game. Both go through **one** builder,
`FApexTrackSceneBuilder` (`ApexSim/Track/`, runtime module): the editor's
`ApexTrackImport` runs it with a factory that saves material instance
constants and fully built meshes into packages and then saves the world as
`L_<Stem>`; the game runs it with a factory that makes dynamic material
instances and fast-built transient meshes straight into the menu world. The
actors, tags and material parameters are the same, so the two cannot drift.
The reader, `ApexPropLibrary` and `ApexGroundMaterials` moved into
`ApexSim/Track/` with it.

- **Materials** are shared cooked assets now, not generated per track:
  `/Game/Materials/Track/{M_ApexTrackBase,M_ApexEmissive,M_ApexBrand,M_ApexDecal}`,
  baked by `-run=ApexMaterialBake [-force]` (`ApexTrackMaterialGraphs`, the
  graphs that used to be built per track). `ApexTrackImport` bakes the
  missing ones first, and the base again when `/Game/Ground` has been
  imported since. The start-light lenses use the emissive parent itself.
- **Where tracks are found** (`UApexTrackContentSubsystem`, at startup;
  `apexsim.track.Rescan`): `-ApexTracksDir=<dir>[+<dir>]`, then
  `<Release>/Game/Tracks` in a package or the repo's `content/tracks/export`
  in the editor. Each manifest's head (the first 64 KB: every field ahead of
  `materials`) becomes a catalog row keyed by `track_id`, with `SourceCrc`
  from the export and `RuntimePreview` from `<Stem>.png` (beside it or under
  `previews/`). `UApexMenuFlowSubsystem::FindTrackRow` returns it when the
  table has no row for the id, or when the track will be built at runtime;
  previews are read through `UApexTrackContentSubsystem::PreviewOf`.
- **Which one loads** (`apexsim.track.Source` / `-ApexTrackSource=`): `auto`
  (default) the cooked level when there is one, else the export; `runtime`
  the export first; `cooked` levels only. Demo mode, the director and the
  catalog all ask `ResolveSource`, so the checksum compared with the
  server's is the file on screen.
- **Loading** (`UApexTrackInstance`, a tickable UObject the subsystem hands
  out with `Acquire` and takes back with `Release`): a cooked track is the
  streamed level instance as before. A runtime one reads the manifest and
  blob and fills the mesh descriptions (with tangents) on a worker thread,
  then builds materials, then meshes within `apexsim.track.BuildBudgetMs`
  (20) per frame, then spawns the actors in one frame, and counts as loaded
  once every surface's collision has cooked. The last runtime track
  released is kept hidden and handed back when the same circuit is asked
  for next (the demo, then the player's race on it; `apexsim.track.KeepLast
  0` turns it off), with every material reset to how it was built.
- **Collision**: a mesh built in a cooked game has no cooked collision and
  cannot cook its own, so each runtime track surface carries a
  `UApexTrackCollisionComponent` beside its mesh (ProcMesh-style: its own
  body setup, the triangles through `IInterface_CollisionDataProvider`,
  cooked async by Chaos). Only traces use it: the racing line's snap and
  the cameras' ground and line-of-sight tests.
- **Tags**: every track surface actor carries `ApexTrackMesh` (the racing
  line's `IsRoadSurface` accepts it; a level baked before the tag is still
  recognised by being the streamed level), every prop `ApexProp`, the
  gantry `ApexStartLights` with lenses `ApexStartLight`. The director's
  conditions pass walks `UApexTrackInstance::GetActors`, not a level.
- **Not yet**: runtime meshes have no mesh distance fields, so software
  Lumen and DF shadows see a runtime road and terrain less well than a
  cooked one (risk 1 in the doc: A/B it before shipping circuits runtime
  only). Cars are still cooked-only.

`ApexSim.Track.Reader.*` tests pin the blob layout against the exporter's
`the_mesh_blob_layout_is_pinned` (one golden 155-byte blob in both), the
header read and the rejects.

The track picker's names, metadata and preview art come from a local
`DT_TrackCatalog` data table keyed by `track_id` (the wire protocol only
carries id and name). Adding or renaming a track means syncing that table:

```bash
python scripts/build_track_catalog.py          # -> content/tracks/export/{track_catalog.json,previews/*.png}
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexTrackCatalogSync                    # adds missing rows + imports T_Track_<Stem> textures
```

The sync is additive unless `-force`; existing rows keep their values. Every
track YAML needs a fixed `track_id` — without one the server mints a new UUID
per start and no catalog row can ever match it.

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
are numpy in `content/props/_tools/apex_tex.py` beside the prop kit's (its
`bpy` import is optional so the baker runs under plain Python). Colour maps
are normalised to a per-channel mean of 0.5 and the material doubles them,
so the exporter's per-key colour still decides a surface's hue — a map with
its own hue would tint twice. `apex_tex.GROUND_TILE_M` and
`ApexGround::TextureTileM` must agree. The importer fixes each map's class
by suffix (sRGB colour, BC5 normal, grayscale roughness), because a material
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

### Cars (`content/cars`, `ApexCarImport`)
A car is `content/cars/<folder>/car.toml` plus the GLB its `model` names.
The client finds its mesh through `/Game/Data/DT_CarCatalog`, keyed by the
TOML's `id` (the wire only carries id and name); a car with no row shows
placeholder art and an empty turntable, and a row whose mesh belongs to
another folder previews as that other car. `ApexCarImport` keeps both in
step:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject     -run=ApexCarImport -all          # or -car=yotota-lmp2 / -list / -remove=folder / -force / -dryrun
```

Each GLB goes through Interchange (one combined mesh, no collision, no
Nanite, materials beside it) to `/Game/Cars/<folder>/SM_<folder>` (hyphens
become underscores), and the row gets name, brand, class, year, country,
mass, power, folder and mesh from the TOML. Without `-force` the run is
additive: existing rows keep their preview framing, cockpit points and
hand-tuned fields, and only a missing or foreign mesh is replaced; the four
cars imported by hand before the commandlet existed keep their meshes.
`AApexRaceCarActor` turns the mesh −90° about Z, so a car must be long along
its local Y; the import logs a warning when it is not. `ApexSim.Cars.Toml`
tests the TOML scan.

The body GLBs have no wheels: the client draws four copies of the class's
shared wheel (`content/wheels/<class>.glb` → `/Game/Cars/Wheels/<class>/SM_Wheel_<class>`)
where the car.toml's `[wheels]` table puts them, steers the front pair and
rolls all four from the telemetry (`Race/ApexCarWheels.h`, row field
`Wheels`, refreshed on every import like the checksum; docs/CAR_MODELS.md).
A GLB changed on disk needs `-force` to be re-imported.

Liveries: a car.toml's `[[livery]]` tables (written by `content/cars/liveries.py`) become the row's
`Liveries` (logos imported to `/Game/Cars/<folder>/Liveries/`). The pick travels as `SelectCar.livery`
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
names. On the client `build_track_catalog.py` writes `source_crc` into the
manifest and `ApexTrackCatalogSync` puts it on the row as `SourceCrc`;
`ApexCarImport` hashes the TOML bytes onto the car row. Both commandlets
refresh that one field even on an additive run, because it is derived, never
hand-tuned. When the race director streams a track level, and when it spawns
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

### Racing line (`Race/ApexRacingLineActor`)
Gameplay settings -> Racing line: OFF / BRAKING ONLY / FULL (default off).
The server works the line out per car (`server/src/racing_line.rs`: the
track's raceline, or its centerline, with a quasi-steady-state speed profile
from the car's grip, downforce, power and brakes) and sends it as
`RacingLine` right after `SessionJoined`. `AApexRacingLineActor` draws it as
dots on the road, one instanced mesh per colour (green flat out, amber at the
grip limit or lifting, red braking), dropped onto the track level's own meshes
by line traces once the level is visible. BRAKING ONLY draws just the red.
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
writes a few seconds as a JSON clip (`replay_tools::ClipFile`: cars as
16-number arrays in roster order, server frame).

The client plays a clip with `-ApexReplay=<file>.clip.json`
(`UApexReplaySubsystem`, created only for such a run): no server, no demo,
no splash hold; `AApexRaceDirector::BeginReplayView` streams the level by
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

### Demo mode and the broadcast camera (`ApexDemoModeSubsystem`, `Race/ApexTvDirector.h`)

The menu plays an AI race behind its screens. `UApexDemoModeSubsystem` asks the
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
demo view streams the track (the player's pending track when it has a level),
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
`gear_whine`, `intake_roar`; the server ignores it), which `ApexCarImport`
puts on the catalog row as `FApexEngineSoundSpec EngineSound` with
`[engine]`'s idle, redline and limiter: derived like `Wheels`, refreshed on
every run. That row is also how the client finally knows the rev range,
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
gearbox, steering and the racing line. `UApexRootWidget::SendDriverAids`
sends `SetDriverAids { auto_gearbox, steering_assist, abs, traction_control }`
on join and on any change of the group; ABS and traction control are
`Option`s on the server (`CarState::abs`, `CarState::traction_control`) so an
AI, or a client from before the fields, drives the car as its `car.toml`
describes it. Traction control LOW holds a spinning wheel at peak slip as
before; HIGH caps drive at what the friction circle has left beside the
lateral force, less a margin (`TC_HIGH_LATERAL_MARGIN`), so full throttle on
a corner exit keeps the cornering grip instead of pushing the rear wide.

A session's host decides which assists its drivers may use: the create
screen's "Allowed assists" chips go out as `CreateSession.allowed_assists`
(`AllowedAssists { abs, traction_control, auto_gearbox, steering_assist,
racing_line }`, every field defaulting to true so an old client's session
allows everything), the session keeps them on `RaceSession::allowed_assists`
and echoes them in `SessionJoined.AllowedAssists`. The server enforces the
rule (`AllowedAssists::clamp`): a forbidden aid is pinned off when a car is
seated and on every `SetDriverAids`, whatever the client asked, and a
session that forbids the racing line sends none. On the client the Assists
tab dims a locked row, disables its pills and shows an "Off in this session"
badge (`UApexNetSubsystem::GetAllowedAssists`, `RefreshAssistLocks`); the
player's own choice is kept for the next session; `-ApexAutoRace
-ApexLockAssists=abs,tc,gearbox,steering,line` creates the auto-race session
with those forbidden, for a screenshot run of the locked tab
(`-ApexOpenSettings=N -ApexSettingsTab=1`). Golden bytes for all three
messages come from `network.rs` `test_assists_wire_format`
(`cargo test assists_wire_format -- --nocapture` prints them) and are pinned
in `ApexGoldenBlobs.h`.

### Weather and time of day (`SessionConditions`, `Race/ApexSkyModel.h`)

A session's host picks its sky on the create screen: a **Weather** chip row
(sunny, cloudy, overcast, light rain, heavy rain) and a **Time of day**
slider in quarter hours. Both go out inside `CreateSession.conditions`
(`SessionConditions { weather, time_of_day_minutes }`, defaulting to a
sunny 13:00 so an old client's session is unchanged), are kept on
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
director-owned unbound post-process volume (priority 10, over the track
level's daylight clamp) carries the exposure floor and ceiling for the
hour, the wet desaturation and the bloom. What lives in the streamed level
is applied once its actors are in the world (`ApplyTrackLevelConditions`,
polled like the start lights): the bake's fog gets the weather's density,
start and colour (dark at night); in rain the road family's slots (`MI_road*`, `MI_pit_lane*` and the
`MI_wear_*` bands the racing line runs on) get dynamic instances with
`Roughness` 0.3 for the wet sheen, and the racing-line dots go glossy and
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

The garage: tyres, engine, transmission, torque and suspension, per driver.
A setup is **clicks** off the car's own `car.toml`, one `i8` per knob
(`CarSetup`, 14 knobs in `KNOBS` order), so neither the wire nor the client
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
a settings tab, `-ApexSettingsTab` now stops at 6, Audio) as two columns of
`UApexStepperWidget` rows: a − / + pill pair around a read-out from
`ApexCarSetup::Describe`. The steppers write through
`UApexSettingsSubsystem::SetCarSetupClick`, so the group's change still
reaches the server through `SendCarSetup`, and the setup is sent on joining
any session, so a car tuned in the garage races with that setup. The knob
table (`ApexCarSetup::Knob`: wire key, range, per-click size and unit)
lives in the net module beside the encoder and mirrors the server's
constants; `ApexSim.Net.CarSetup.Clicks` pins the ranges and read-outs, the
golden encode test the bytes. The garage's Reset returns every knob to stock.

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
lap). `ClientMessage::HotlapRelocate { destination }` moves the car:
`Track` puts it on the centerline `HOTLAP_RUNUP_M` (300 m) before the line,
in first, so the first flying lap starts timed as it crosses; a second car
going out is queued `HOTLAP_SPACING_M` behind the first slot still occupied
(`hotlap_runup_pose`, `physics::pose_at_station`). `Garage` parks it again,
repaired, with its bests kept (`hotlap_relocate`). Any participant may send
it; outside a hotlap it is refused with `Error 400`. The AI, if any, is
left driving. `tests/hotlap_test.rs` covers all of it, over the wire too.

**The client** (`UApexRootWidget::HandleTelemetryForHotlap`) reads the
local car's `bInGarage` and opens or closes the garage: `UApexHotlapWidget`
is the layer between the HUD and the pause menu, showing the garage card
(GO OUT, REPLAY BEST LAP, GHOST CAR, RESET SETUP and the fourteen setup
rows), the lap-by-lap timing sheet (every `LapTiming` lap end for the local
car, with the delta to the best legal lap and struck laps greyed; it
survives trips to the garage and clears when a hotlap begins) and the replay
strip. While the card is up the HUD is hidden, driving input is off and the
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
--nocapture` → `ApexGolden::C_HotlapRelocate` / `C_RequestGhost` /
`S_GhostLap`, and `ApexUdpGolden::S_TelemetryCompactGarage` (the lap-flags
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
sine, a damper and a spring, updated only when they change; a watchdog drops
every force if the game stops updating them for 0.3 s, so a hitch or a crash
never leaves a wheel pulling. Which way a positive force turns a rim is not
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
full lock. Past half the lock either way `MixWheel` puts a soft stop on the
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
- `cars/` - Car physics definitions (TOML: `car.toml` per car; most physical parameters moddable with validated ranges)
- `tracks/` - Track definitions (YAML/JSON) + procedural terrain caches (`.terrain.msgpack`)
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
- `[auth]`: `mode = "dev"` (accept all, development only) or `mode = "token"` with shared secrets in `tokens`
- `[ai]`: AI driver defaults (optional)

Environment overrides use the `APEXSIM_` prefix, e.g. `APEXSIM_NETWORK_TCP_PORT=9100`, `APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9000`, `APEXSIM_SERVER_TICK_RATE_HZ=120` (see `ServerConfig::apply_env_overrides`).

## Testing Notes

- `proptest` property tests live in `tests/physics_property_tests.rs` (physics invariants, serialization roundtrips)
- `tests/grip_probe_test.rs` (ignored) is the grip-tuning harness: skidpad lateral-g sweep, a follower driving Silverstone's first corner at the racing line's speed, and every car's ideal-lap time from its racing-line profile (`PROBE_MU_SCALE` / `PROBE_CLA_SCALE` / `PROBE_CLASS` sweep grip and downforce without editing TOMLs). Class grip levels are set so those lap times land a few seconds off real poles
- Integration tests simulate real client connections with `TestClient` structs against an in-process server
- Enable debug logging: `RUST_LOG=debug cargo test ...`
- CI (`.github/workflows/ci.yml`) runs fmt-check, clippy (`-D warnings` — keep the tree warning-free), build, and tests for the server; Dependabot auto-merge builds and tests before merging
