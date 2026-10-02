# AC track import: `ac_import.py`

*Feature document, 2026-09-27. Supersedes the "survey" approach in
`docs/AC_IMPORT_FEASIBILITY.md`. That approach measured an AC track and
rebuilt it from ApexSim's own kit; this one brings the track over as it is.
It builds on work that has landed since the feasibility study: runtime-loaded
tracks and cars, the mesh road backend (now the default),
`content/tracks/custom`, and `external_sidecars`.*

## Status (2026-09-27)

Built, in the order the phases below describe, on the same day:

- `scripts/ac_import.py` and the `scripts/ac_import/` package: the readers,
  the frame, the YAML and all four server sidecars (phase 1); export format
  version 3, compressed runtime textures, scenery on the car parents and
  draw distances on the client (phase 2); scene selection, per-mesh
  material classification, merging into 250 m cells, texture conversion,
  previews and the report (phase 3); the `imported` marker across the
  Rust tools and the PowerShell scripts, `--all`, and the AI survey on a
  custom stem (phase 4). CLAUDE.md, "Assetto Corsa track import", is the
  working reference.
- Checked on the Kunos Zandvoort: 4 189 m, 153 k physics triangles, 1.1 M
  drawn triangles in ~570 draw calls, 129 textures (86 MB); grid, coverage
  and centerline checks pass; the server test loads it with every sidecar
  and the mesh agrees with the centerline within 15 cm; the Unreal editor
  target builds. **Not yet done:** the twelve-track acceptance run, the
  frame-rate and load-time measurement on the reference machine, and a
  look at an imported circuit on screen (the Unreal code compiled and its
  reader tests exist, but no imported track has been raced in the game
  yet).

What differs from the design below, on purpose:

- **Kerbs keep AC's textures** rather than the kit's curb family: their
  stripes are authored into the texture, and the kit's 2 m stripes would
  need an along-kerb UV the kn5 does not carry. A material or mesh named
  `KERB`/`CURB`/`CORDOL` is never kit, whatever lies under it: Spa's
  `CURB_B` stands on road physics in places and was drawn as asphalt.
- **Classification is per material, from its meshes' physics**: each
  mesh is sampled at its triangles' centres (three quarters must lie on
  physics, 60% of those in one class), and the meshes of a material that
  resolve vote, weighted by size, for the whole material. Vertices were
  tried first and failed: a road ribbon's vertices all lie on the road's
  edges, where the physics is road or grass by a coin toss, and Zandvoort's
  asphalt chunks came out as kit grass. A material none of whose meshes
  stand on physics falls to the hints, **names before shaders**: a
  gravel, sand, grass (`TERRAIN` included), earth or road name decides,
  and only then plain `ksMultilayer` is grass, `ksMultilayer_fresnel*`
  (Kunos' tarmac) road, and `ksMultilayer_objsp` (an object shader:
  railings, towers, stands) nothing. Shader first painted Spa's whole
  valley as asphalt: its terrain is `grass-ext-shad` on `grass-ext.dds`
  with the tarmac shader. Alpha-tested and blended materials are never a
  kit surface.
- **A material AC draws at `alpha = 0` is not drawn**: mods hide the
  physics meshes they leave renderable that way. Monza 2022's 216
  (`01WALL`..., the road and run-off) wear `physics`, `ksPerPixelAlpha`
  with `alpha = 0` on a flat normal map, and drawn they painted every wall
  and run-off lavender.
- **Overlays are dropped** by the mesh name, or by a see-through
  material's name (`GROOVE`, `SKIDMARK`, `KSLAYER`): Spa's rubber grooves
  are `Plane023` and `Loft286` wearing `groove3`, painted to darken AC's
  asphalt, and over the kit's they drew hard-edged dark sheets across the
  road after the grid and after La Source.
- **The spine starts at the timing gate's foot on the AI line**, not at
  the gate's middle: Spa's gate is 5.4 m off its line, and pinning the
  first point to it put a 5 m-radius kink in the raceline at the seam,
  which the speed profile braked for (red dots on the grid, and a La
  Source braking point planned for an entry 30 km/h slow). The road's
  middle, quantised to the 0.25 m cross-section step, is smoothed along
  the lap (a 9 m median, then a 6 m Gaussian); unsmoothed it zig-zagged
  0.3 m node to node, a 100 m radius on every straight.
- **Only the diffuse maps come over**, and most are re-encoded: the
  majority of Kunos textures ship without mip chains, so they are decoded,
  mipped and range-fit to BC1/BC3 by the tool; a BC1/BC3 source with its
  mips is copied verbatim. `txMaps` is not read; roughness is a scalar
  from `ksSpecularEXP`.
- **A two-sector AC track** (only `AC_TIME_1`) gets a synthetic second
  boundary halfway to the finish, since the server wants three sectors.
- **Validation is the tool's own** (the grid on the mesh, coverage, the
  line on the road, wall openings, the budget), plus
  `server/tests/imported_track_test.rs` through the real loader (opt-in:
  `cargo test --release --test imported_track_test -- --ignored`); the
  server binary is not invoked by the tool.
- Wall kinds come from the mesh and material names (tyres, concrete, else
  armco); AC carries no material for a wall.

## Summary

Many sim racers have large collections of Assetto Corsa tracks, most of them
mods. This feature lets them race those tracks in ApexSim.

`ac_import.py` is a command-line tool. Pointed at a track in the player's own
AC install, it writes a complete ApexSim track: AC's geometry, drawn by the
ApexSim client and driven on by the ApexSim server.

```powershell
python scripts/ac_import.py "E:\SteamLibrary\steamapps\common\assettocorsa\content\tracks\rt_suzuka"
# restart the server; restart the game or run apexsim.track.Rescan
```

- **The whole scene is imported.** Road, kerbs, run-off, terrain, buildings,
  stands, trees, barriers and signage all keep AC's geometry.
- **Surfaces use ApexSim's materials where the kit has one.** Asphalt, kerbs,
  grass, gravel, sand, concrete and astroturf are drawn with our ground
  texture sets. Everything else (a building's facade, a sponsor board, a
  tree card) keeps the track's own textures.
- **The server drives on AC's physics mesh.** Every wheel uses AC's surface
  and friction through the mesh road backend, and AC's walls stop the cars.
  What is drawn and what is driven relate exactly as they do in AC.
- **ApexSim supplies what AC does not carry, or what we do differently:**
  the sky, weather and time of day, the lighting and exposure, the cars,
  the AI, the racing line, timing and the TV camera.

## Goals

- **One command per track layout**, with no manual steps, for Kunos tracks
  and for mods of ordinary quality.
- **Faithful driving.** The server drives on AC's physics mesh: bumps,
  crown, camber, kerb profiles, per-surface friction and the real track
  limits (`IS_VALID_TRACK`).
- **Faithful looks, in ApexSim's style.** Ground surfaces use our kit, so an
  imported track sits beside the shipped ones under the same sky and the
  same wet-road look. Everything else looks like the AC track.
- **Playable performance.** A typical Kunos track (1.5 M triangles, 300 MB
  of textures) loads in under 30 s and holds the frame rate of the shipped
  circuits. A heavy mod (10 M+ triangles) degrades gracefully rather than
  failing.
- **Local only.** The output never leaves the player's machine by any path
  we provide.

## Non-goals

- **Distribution.** No AC geometry, textures or physics data is bundled,
  hosted, packaged or sent to other players. Release builds leave
  `content/tracks/custom` and its exports out. (Measurements of where a
  real circuit's buildings stand, read off an AC track as boxes and lines,
  are a different thing and may correct a shipped dossier: see
  `docs/AC_LAYOUT_SURVEY.md`.)
- **Encrypted content.** kn5 files encrypted by Custom Shaders Patch are
  refused with a clear message, never decrypted.
- **AC's shaders and effects.** CSP extensions, AC's lighting, reflection
  cubemaps, animated flags, crowds and the `ksGrass` fields are not
  reproduced. Materials are approximated (see "Materials").
- **Cars.** A separate feature.
- **Multiplayer content sync.** Every player in a session must have imported
  the same track. The existing content checksum check reports a mismatch.

## User flow

1. The player runs the tool on a track folder. `--list` shows its layouts,
   and `--layout` picks one.
2. The tool reads the track and writes:
   - the server's files into `content/tracks/custom/`;
   - the client's export into `build/tracks/`;
   - a report of what it found, what it approximated and what it dropped.
3. The player restarts the server, then restarts the game or runs
   `apexsim.track.Rescan`. The track is in the track picker.

Re-running on the same input writes byte-identical files. `--all <AC tracks
folder>` imports every importable layout of a whole collection and prints
one summary line per track. That summary is the intended way in for a player
with 200 mods.

## Command line

```
python scripts/ac_import.py <ac-track-folder> [options]
python scripts/ac_import.py --all <ac-content-tracks-folder> [options]

  --layout NAME        which layout of a multi-layout track (default: every layout)
  --list               list the layouts and exit
  --stem NAME          output stem (default: folder + layout, e.g. RtSuzuka_Gp)
  --display-name TEXT  the name the game shows (default: from ui_track.json)
  --textures MODE      kit (default: kit for ground, AC for the rest) | ac (AC everywhere) | flat
  --max-texture N      downscale textures larger than N pixels (default 2048)
  --force              replace an existing import of this stem
  --dry-run            read and report; write nothing
```

## What it reads

| AC source | Used for |
|---|---|
| `models*.ini` | which kn5 files make up the track (per layout) |
| visual `*.kn5` | geometry, materials and textures of everything drawn |
| physics `*.kn5` (the meshes named `NN<KEY>`) | the drivable surfaces and walls the server simulates |
| `data/surfaces.ini` | per-surface friction, track validity, pit lane, and the surface class of each physics mesh |
| `ai/fast_lane.ai` | the centerline (the lap's spine), road widths and the AI line |
| `ai/pit_lane.ai` | the pit lane polyline |
| the `AC_START_n`, `AC_PIT_n` and `AC_TIME_0/1/2_L/R` marker objects in the kn5 | grid slots, pit boxes, start/finish line and sector lines |
| `data/drs_zones.ini` | DRS zones |
| `ui/ui_track.json` (plus `preview.png` / `outline.png`) | name, country, length, pit count, and the picker's preview |

## What it writes

**Server** (in `content/tracks/custom/`):

| File | Contents |
|---|---|
| `<Stem>.yaml` | The centerline from `fast_lane.ai` (5 m nodes: widths, z, banking), `raceline`, `sectors`, `spawn_points`, `drs_zones`, `closed_loop`, a fixed `track_id`, `name` / `display_name` and `metadata`. The server still keys laps, sectors, the AI and the racing line on it. |
| `<Stem>.road.msgpack` | AC's physics mesh: every drivable triangle, with a surface table built from `surfaces.ini` (contact class, friction, valid track, pit lane). |
| `<Stem>.walls.msgpack` | The `WALL` physics meshes as wall segments with base height, height and material. |
| `<Stem>.ground.msgpack` | A heightfield rasterised from the ground meshes, used where the mesh has nothing under a wheel. |
| `<Stem>.curbs.msgpack` | Kerb and run-off widths per metre, for the centerline backend and the fallback. |
| `<Stem>.ats` | A minimal scene carrying `"imported": "ac"` (see "Pipeline integration"). |
| `<Stem>.import.json` | The report: source folder, layout, tool version, the CRC of every AC file read, and every warning. |

**Client** (in `build/tracks/`):

| File | Contents |
|---|---|
| `<Stem>.uescene.json` + `<Stem>.uemesh` | The export in format version 3 (below): the visual meshes merged into cells, the material table, the start lights, the grid, the centerline. |
| `<Stem>.textures/` | The textures the non-kit materials use, as DDS. |
| `previews/<Stem>.png` | The track picker's preview, from AC's `preview.png` or drawn from the centerline. |

## How it works

The tool is a Python package, `scripts/ac_import/`, sharing helpers with the
other track scripts (`track_dirs.py`, the msgpack writer). Stages:

1. **Read.** A kn5 reader (node tree, meshes, materials, embedded
   textures), the `.ai` readers and the INI files. Encrypted files are
   refused. A track whose physics or AI file is missing is reported and
   skipped.
2. **Frame.** AC is Y-up and mirrored; ApexSim is Z-up, in metres, with
   +Y to the left. The origin goes on the start line (the midpoint of
   `AC_TIME_0_L/R`), with +X along its heading. One transform is applied
   to everything after this stage. Its handedness is pinned by a test on a
   known corner.
3. **Centerline and race data.** As in the feasibility study: the
   `fast_lane.ai` points moved to the middle of the road and resampled to
   5 m, with widths from the side distances (bad values clamped). `z` and
   banking are measured on the road mesh. The sectors, grid, DRS zones and
   pit lane come from the files listed above.
4. **Physics.** The drivable physics meshes go into `road.msgpack`, the
   walls into `walls.msgpack`. Each `surfaces.ini` key maps to a contact
   class: `KERB` to curb, `GRASS` / `SAND` / `OUT` / gravel to off,
   `IS_PITLANE` to pit lane, anything else to road. Its friction becomes a
   multiplier relative to the track's asphalt, and `IS_VALID_TRACK` sets
   the track limits.
5. **Scene selection.** Which visual meshes to keep:
   - Kept: every mesh marked renderable.
   - Dropped: physics-only meshes, AC's shadow and "groove" (rubbered
     line) overlays, and the invisible logic objects.
   - Warned about: anything with a known performance trap, such as the
     animated grass meshes (`ksGrass`) or a single tree mesh with 200k
     triangles.
6. **Materials.** Each AC material is classified (see "Materials") as a
   **kit surface** (drawn with our ground sets) or as **AC-textured**
   (opaque, masked or translucent; its textures are converted into
   `<Stem>.textures/`).
7. **Merge and cull.** The kept meshes are merged by material into cells of
   250 m, the unit the export already uses for road sections. Each merged
   mesh carries a draw distance taken from the kn5's own `lodOut` values.
   This turns AC's 1 000–3 000 objects into a few hundred draw calls.
8. **Collision for traces.** The road, kerb and ground cells are tagged as
   track surface, which is what the racing line and the cameras trace
   against. Scenery gets no collision, which bounds the runtime collision
   cooking to the drivable triangles.
9. **Write and validate** (see "Validation"). The report is written, and a
   non-zero exit code signals a failed check.

## Materials

A material is classified from what it is used for and what it is called:
the physics surface under the meshes that use it, its shader (for example
the multi-layer terrain shader `ksMultilayer`, or `ksTree` for tree cards),
and its name and texture names ("asphalt", "kerb", "grass", "gravel",
"sand", "concrete", "astro", "tree", "crowd", "glass").

| Class | Drawn with | Notes |
|---|---|---|
| Road asphalt, pit lane | our `road` family on the asphalt set | picks up the wet-road look in rain like any road |
| Kerbs | our `curb` family | style (`red_white`, `yellow_black`...) from the texture's two dominant colours |
| Grass, gravel, sand, concrete, astroturf, terrain | our `surface` family on the matching ground set | a multi-layer terrain material maps to grass unless its mask says otherwise |
| Trees, fences, foliage, crowds | AC texture, masked | alpha-tested and two-sided |
| Glass, light cones | AC texture, translucent | |
| Everything else (buildings, stands, signage) | AC texture, opaque | diffuse only; AC's `txMaps` specular channel approximates a roughness |

`--textures ac` keeps AC's textures on the ground surfaces too (for a mod
whose tarmac is its character). `--textures flat` draws everything
AC-textured in its texture's average colour: the lightest option, for a
slow machine. The report lists the classification of every material, so a
mod that fools the heuristics shows up in the report rather than on screen.

**Sponsor boards and signage keep the textures the track came with.**
ApexSim's rule of no real trademarks on screen covers what we ship; an
imported track is the player's own content on the player's own machine.

## Changes needed in ApexSim

This route needs real engine and pipeline work. In dependency order:

1. **Export format version 3.**
   - A textured material entry: texture paths relative to
     `<Stem>.textures/`, a blend mode (opaque, masked, translucent), and
     two-sided.
   - A per-mesh draw distance.
   - A per-mesh flag for whether it is a traceable track surface.

   The version-2 reader stays, and `track_core` and `ApexTrackSceneReader`
   both gain the fields, pinned by a new golden blob.
2. **Compressed runtime textures.** The runtime builder creates textures
   from DDS files by copying their compressed blocks (BC1, BC3, BC5) straight
   into transient textures, with no decoding. That is what makes 300 MB of
   AC textures cost about 300 MB rather than 1.2 GB. The car loader's
   uncompressed BGRA8 path does not scale to a track.
3. **Scenery materials.** The runtime builder uses the car parents under
   `/Game/Materials/Car` (opaque, masked, translucent: two-sided, and the
   same parameter names) for the AC-textured materials, so no new cooked
   material is needed. The kit surfaces use the track families as today.
4. **Draw distances and cells.** The builder applies each mesh's draw
   distance, and builds cells within `apexsim.track.BuildBudgetMs` as it
   does now. With 10–20× the triangles of a generated track, the load needs
   measuring, and possibly a coarser budget or a progress bar.
5. **Imported tracks are left alone by the pipeline.** An `.ats` carrying
   `"imported": "ac"` tells `ats-export`, `ats-dress`, `ats-groom`,
   `initialize_content.ps1` and `build_track_levels.ps1` to skip the track
   entirely. The importer writes its export and all four sidecars itself.
   (`external_sidecars` covers the sidecars already; this extends the same
   idea to the export and to dressing.) `initialize_content.ps1` reports an
   imported track whose export is missing, with the `ac_import.py` command
   from its `import.json` that rebuilds it.
6. **The AI survey on custom tracks.** `survey_ai_races_on_every_circuit`
   takes a custom stem (`SURVEY_TRACKS=RtSuzuka_Gp`). Today it only walks
   `content/tracks/default`.

The server needs no change: the mesh road, walls, per-surface friction and
the track limits are all there.

## Names and ids

- **`track_id`** is a UUID v5 of the AC folder name plus the layout, so a
  re-import keeps its id and the content checksums stay meaningful. An id
  clash with a shipped track leaves the shipped one loaded, with a warning.
- **`name`** is the name from `ui_track.json`. **`display_name`** is
  `--display-name` when given, and otherwise the same name. The
  no-trademarks rule is for what ApexSim ships, not for the player's own
  imports.
- **Stem** is unique across `default/` and `custom/`. The tool checks
  before writing, and `ats-export --all` refuses a clash anyway.

## Validation

Every import is checked, and the report says pass or fail for each:

- **The server loads it.** The YAML goes through the server's loader, the
  sectors resolve, and every grid slot has road mesh under it.
- **Road coverage.** Every metre of the lap, from edge to edge, has
  physics triangles under it (the road mesh coverage test, run on this
  track). Holes are reported by station.
- **The centerline and the mesh agree.** The `fast_lane.ai` spine stays
  within the road mesh's edges all the way round, since the AI and the
  racing line follow it.
- **Walls.** `check_walls.py --openings`, with the open length reported.
  AC's own walls are the barrier; gaps are expected where the track has
  none, so this reports rather than fails.
- **Budget.** Triangle count, draw calls, texture memory and estimated
  build time, against thresholds that print a warning. A track above the
  hard limits is still written, but flagged.

**Acceptance for the feature:** four Kunos tracks, including a
multi-layout one, and eight popular mods of mixed quality and age all
import without manual edits. Each one loads, is driven by the AI survey
without a circuit that is measurably worse than a shipped track, and holds
60 fps at 1440p with a full grid on the reference machine.

## Legal position

This route converts the geometry and textures of content the player owns,
which is further than the survey approach went. The rules that keep it on
the right side:

- **Local only.** The tool runs on the player's own install and writes to
  the player's own machine. Output goes to the gitignored `custom/` folder
  and to `build/`, and release builds exclude both.
- **Nothing bundled.** ApexSim ships no AC geometry, textures or physics
  data (the dossier corrections of `docs/AC_LAYOUT_SURVEY.md` are
  positions and sizes only). It ships a converter, like
  the existing AC modding tools.
- **Encrypted content is refused.** A mod author who encrypted their track
  did not want it unpacked.
- **The tool says so.** A notice on first run explains that imported tracks
  are for personal use, and that some mod authors' terms forbid conversion
  to other games.

Whether ApexSim ships this tool to players at all, rather than keeping it a
developer utility, is a product and legal decision outside this document.

## Risks

1. **Performance of runtime-built meshes.** A mesh built while the game
   runs gets no Nanite and no mesh distance fields. That already applies to
   the generated tracks (risk 2 in `RUNTIME_CONTENT_LOADING.md`), but an AC
   track has 10–100× the scenery. Merging, draw distances and the kn5's own
   LODs are the mitigation. If they are not enough, the fallback is an
   editor-side import (`ApexTrackImport` already saves packages) for
   players with the editor, which is not most players.
2. **Load time.** 1–10 M triangles through the per-frame build budget, plus
   the texture uploads. This needs measuring early, in phase 2.
3. **Material classification on mods.** Naming conventions vary. The report
   and `--textures ac` are the escape hatches. The heuristics should grow
   from real imports, not guesses.
4. **Mismatch between the drawn and the driven road.** AC's visual road and
   its physics mesh are separate meshes, usually within a centimetre or
   two. It is the same relationship AC players already drive with, so it
   is accepted.
5. **The AI on unfamiliar geometry.** Mods have narrow roads, odd pit
   lanes and missing walls. The survey will find cars that leave the track,
   and the AI's recovery is already a known weak spot (the Oschersleben
   note in CLAUDE.md).
6. **Mod data quality.** Missing or broken `fast_lane.ai`, sector markers
   in the wrong place, and huge unoptimised meshes. The tool refuses rather
   than guesses where the result would be undrivable, and says why.

## Out of scope for this version

- Tracks without `fast_lane.ai` (a centerline derived from the road
  outline instead).
- Point-to-point stages and hill climbs (`closed_loop: false`).
- Replacing AC trees and stands with ApexSim kit props.
- Night lighting from AC's light definitions.
- Skin-texture variants and seasonal swaps.

## Phases

| Phase | Scope | Estimate |
|---|---|---|
| 1 | Readers, frame, YAML, all four server sidecars. The track is drivable on AC's physics in an empty world, and the AI survey runs on it. | 1 week |
| 2 | Export version 3, compressed runtime textures, scenery materials on the car parents, draw distances. Measure load time and frame rate on one Kunos track. | 1.5–2 weeks |
| 3 | Scene selection, material classification, merging into cells, texture conversion, previews, the report. | 1.5–2 weeks |
| 4 | The `"imported"` marker across the pipeline and scripts, `--all`, the acceptance run on twelve tracks, and tuning the heuristics and budgets from it. | 1–2 weeks |

Total: roughly 5–7 weeks. Phase 1 alone is useful: any AC track becomes
drivable, with exact physics, before it looks like anything.

## Already in place

| Piece | Where it came from |
|---|---|
| Tracks built by the game at runtime from exports | `RUNTIME_CONTENT_LOADING.md` |
| Driving on a triangle mesh with per-surface friction and track limits, now the default | `ROAD_MESH.md` |
| `content/tracks/custom`, walked by every tool and left out of releases | the track folder split |
| Importer-written sidecars kept by `ats-export` (`external_sidecars`) | the survey-route groundwork |
| `sectors` kept through YAML rewrites | the same |
| The client export in `build/tracks`, caches outside `content/` | the folder cleanup |

`seed_scene.py --from`, built for the survey approach, is not used by this
route: the kerbs and run-off come with AC's geometry.
