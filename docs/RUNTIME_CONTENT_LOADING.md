# Runtime-loaded tracks (and, later, cars)

*Current state as of 2026-09-26 ([gdoct/apexsim#48](https://github.com/gdoct/apexsim/pull/48)). Started as a design investigation that followed `docs/AC_IMPORT_FEASIBILITY.md`; the investigation's findings are kept, condensed, at the end.*

## Where it stands

| | State |
|---|---|
| Tracks | **Built at runtime, only.** The game builds every circuit from its export (`<Stem>.uescene.json` + `<Stem>.uemesh`) when it is raced. There are no cooked track levels: nothing under `/Game/Tracks` is loaded or cooked. |
| Track pipeline | Unchanged up to the export: dossier (`osm_layout.py`, `dem_fetch.py`), `ats-dress`, `ats-export`, plus `build_track_catalog.py` for the preview. What used to follow it — `ApexTrackImport` into a level, `ApexTrackCatalogSync` into `DT_TrackCatalog`, and cooking the level — is out of the build. |
| Cooked track content | The four parent materials under `/Game/Materials/Track` (`ApexMaterialBake`). Props, ground textures and wheels were already cooked assets and are found by path. |
| Cars | Still cooked (`ApexCarImport`). The runtime design is below, not started. |
| Verification | Rust side tested (unit, round trip, determinism, golden blob, whole-calendar export). **The Unreal code has not yet been compiled or run**; see "Open risks". |

The consequence the investigation predicted holds: the editor has dropped out of the track loop. Edit, `ats-export`, restart the game (or `apexsim.track.Rescan`). A packaged game takes a new circuit as three files dropped in its `Tracks/` folder.

## How a circuit reaches the game

```powershell
./scripts/build_track_levels.ps1                    # every circuit
./scripts/build_track_levels.ps1 -Track Spa         # one
./scripts/build_track_levels.ps1 -SkipMaterials     # Rust + Python only, no engine
```

It runs, in order: `ats-dress` (the scene from the dossier), `ats-export` (the export, plus the server's sidecars beside the YAML), `build_track_catalog.py` (the preview), and `-run=ApexMaterialBake` (the shared parent materials, only the missing ones). The script keeps its old name; it no longer builds levels unless asked (`-ImportLevels`, below).

What one circuit is on disk, and who reads it:

| File | Written by | Read by |
|---|---|---|
| `content/tracks/real/<Stem>.yaml` | hand / converters | server (simulation, `ContentCrc`); the exporter |
| `content/tracks/real/<Stem>.{ground,curbs,walls}.msgpack` | `ats-export` | server only |
| `content/tracks/export/<Stem>.uescene.json` | `ats-export` | client: catalog row (the head) and the build (all of it) |
| `content/tracks/export/<Stem>.uemesh` | `ats-export` | client: the build |
| `content/tracks/export/previews/<Stem>.png` | `build_track_catalog.py` | client: the track card |

Where the client looks for exports (`UApexTrackContentSubsystem::TrackDirectories`), first match per stem wins:

1. `-ApexTracksDir=<dir>` on the command line (several joined with `+`);
2. a packaged build: `Tracks/` beside `ApexSim.exe` (`<Release>/Game/Tracks`), where the preview sits as `<Stem>.png`;
3. the editor build: the repo's `content/tracks/export`, previews under `previews/`.

**Adding a circuit to an installed game:** its YAML (with a `track_id`) and the three sidecars in `Server/content/tracks/real`; its `.uescene.json`, `.uemesh` and `.png` in `Game/Tracks`. No editor, no cook, no repackage.

## The export

Format version 2, specified in `track-editor/TRACK_EDITOR.md` §5:

- **`<Stem>.uescene.json`** — a compact JSON manifest: ids, name, `source_crc` (the YAML's CRC-32 with carriage returns dropped, the server's `ContentCrc`), metadata, dressing, `mesh_blob`, materials, one header per mesh (name, material key, vertex and index counts), props, grid, centerline, pit lane, start/finish. Every field the catalog needs comes **before** the first array, so the client reads only the first 64 KB of each manifest at startup (`FApexTrackSceneReader::LoadHeader`).
- **`<Stem>.uemesh`** — little-endian binary: `APEXMESH`, version, count, then per mesh its name, key, counts, a zlib flag and the payload (`f32` positions, normals, uvs, `u32` indices), each mesh compressed on its own. Written first, manifest last, so a manifest always has its blob.

Version 1 (every buffer inline in the JSON) still reads, on both sides.

Measured, whole calendar (27 circuits, one `ats-export --all --release`, 2 min 23 s):

| | Before (v1, JSON) | Now (v2) |
|---|---|---|
| Zandvoort | 44.1 MB | 1.2 MB + 7.3 MB |
| Spa | 77.8 MB | 2.3 MB + 13.5 MB |
| Nordschleife | — | 7.3 MB + 37.2 MB |
| All 27 | — | 48 MB + 262 MB (22.7 M triangles) |

The exporter is deterministic (same scene → same bytes). One 155-byte golden blob is pinned on both sides: `the_mesh_blob_layout_is_pinned` in `track-editor/core/tests/ue_export.rs` and `ApexSim.Track.Reader.GoldenBlob`.

## Building a track in the game

`UApexTrackInstance` (a tickable UObject, `ApexSim/Track/`) takes a circuit from files to a loaded track in phases:

1. **Parse** (worker thread): read the manifest and blob (`FApexTrackSceneReader::LoadFromFile`) and fill a mesh description per mesh (`FApexTrackSceneBuilder::PrepareGeometry`): one vertex instance per source vertex, tangents from the UV gradient, the seam-fringe ramp in the run-off bands' vertex colours, bounds from the vertices.
2. **Materials** (game thread, one frame): a `UMaterialInstanceDynamic` per material key, named `MI_<key>`, from the cooked parents, with the family table's parameters (road, curb, surface, structure, marking, decal) and the ground set each family samples.
3. **Meshes** (game thread, within `apexsim.track.BuildBudgetMs`, 20 ms a frame): `UStaticMesh::BuildFromMeshDescriptions` on its fast path, bounds set from the vertices (the fast path has produced NaN bounds before), UV densities set for the texture streamer, then a sanity check on every mesh.
4. **Actors** (game thread, one frame): spawned into the menu world's persistent level.
5. **Cooking**: the track counts as loaded once every surface's collision has cooked.

The builder, `FApexTrackSceneBuilder`, is the old editor-only level builder moved into the runtime module behind `IApexTrackAssetFactory`. The game's factory (`FApexRuntimeTrackFactory`) makes transient objects. The editor's `FApexTrackAssetBuilder` saves packages instead and is only used by the inspection import. Because both use one builder, what the editor shows is what the game builds.

What a built track contains:

| Actor | Built as | Tags |
|---|---|---|
| One `AStaticMeshActor` per baked mesh (road, curbs, markings, run-off bands, terrain, structures, decals) | fast-built mesh, movable, `ShadowCacheInvalidationBehavior::Static`; no collision on the mesh itself | `ApexTrackMesh` (actor and components) |
| `UApexTrackCollisionComponent` on each of those except decals | its own `UBodySetup` with the mesh's triangles, complex-as-simple, cooked async by Chaos (the `UProceduralMeshComponent` pattern: a body setup owned by a component in a game world cooks at runtime; one owned by a mesh asset does not) | `ApexTrackMesh` |
| One actor + `UHierarchicalInstancedStaticMeshComponent` per (kind, asset, text) of instanced props, all instances added in one `AddInstances` | kit meshes `/Game/Props/<kind>/SM_<asset>` by path, or generated stand-ins | `ApexProp` |
| Grandstands (bays and caps), sky drifters, the ferris wheel, bridges stretched to the road, boards with text | as the level builder always laid them | `ApexProp` |
| Generated stand-ins `SM_Prop_<kind>` and the start gantry sized to the road | built from the recipes at runtime; a simple box for collision | `ApexProp` |
| Start lights, five lenses | the gantry mesh; each lens gets its own dynamic instance of `M_ApexEmissive` when the director finds it | `ApexStartLights`, lenses `ApexStartLight` |
| `TrackFog`, `TrackPostProcess` (unbound) | same constants as before | — |

The 16 `APlayerStart`s the levels used to carry are gone; nothing read them.

**Hide and show.** The demo hides the track behind car select (`SetVisible(false)`): every actor hidden and its collision off, the post-process volume disabled and the fog hidden, since those act on the whole view. Showing it again rebuilds any collision body whose cook finished while it was hidden.

**Reuse.** A released track is kept hidden and handed back, with every material parameter put back to how it was built, when the same circuit is asked for next — typically the demo, then the player's race on it (`apexsim.track.KeepLast 0` turns this off). One circuit is in the world at a time; asking for another drops the kept one. A rescan drops it too, so a re-exported circuit is rebuilt.

**Failure.** An unreadable export or a mesh that will not build fails the instance. The director hands it back (`DropFailedTrack`), and the circuit is marked broken until the next rescan, so the lobby's next snapshot does not start the same failure again. The race goes on in an empty world, as it did before for a track with no level.

## What the rest of the game reads from a track

All of it goes through the instance, a tag or a trace:

- **Race director**: `LoadTrackLevel` / `UnloadTrackLevel` acquire and release a `UApexTrackInstance` from `UApexTrackContentSubsystem`. The readiness gates (`IsTrackLevelLoaded`, `IsTrackVisible`) serve the demo fade, the replay and the start lights.
- **Conditions pass** (`ApplyTrackLevelConditions`) walks `UApexTrackInstance::GetActors`:
  - the fog actor gets the weather;
  - the road family's materials (by the `MI_road`, `MI_pit_lane` and `MI_wear_` name prefixes) get the wet roughness;
  - lamps tagged `ApexEmissive_floodlight_lamp` glow after dark;
  - floodlight and lamp-post instances (by mesh name) get spot lights.
- **Start lights**: found by tag; hidden gantries are ignored.
- **Racing line**: `IsRoadSurface` accepts a trace hit only on an actor tagged `ApexTrackMesh` and not `ApexProp`.
- **TV director and replay tripod**: complex `ECC_WorldStatic` traces, skipping `ApexProp`. These traces and the racing line are the only collision consumers. Cars, wheels, rain and dots collide with nothing.
- **Not the track at all**: the minimap, the TV director's path and the racing line itself come from the server (`FApexTrackConfigSummary.Centerline`, `RacingLine`).

## Catalog

`UApexTrackContentSubsystem::Rescan` (at startup, and `apexsim.track.Rescan`) turns each manifest's head into an `FApexTrackCatalogRow`:

- identity and text: `track_id`, name, country, city, category, environment and length;
- `YamlBaseName` from the stem;
- `SourceCrc` from `source_crc`;
- `RuntimePreview`, a transient texture loaded from the PNG with `FImageUtils::ImportFileAsTexture2D`.

`UApexMenuFlowSubsystem::FindTrackRow` / `FindTrackIdByStem` return that row before any `DT_TrackCatalog` row. The table is now only a fallback for names and art of a track with no export on the machine; `ApexTrackCatalogSync` still fills it, but nothing in the build runs it. The preview widgets read `UApexTrackContentSubsystem::PreviewOf(Row)`. The content-checksum check (`VerifyTrackContent`) is unchanged, and compares the checksum of the file the circuit on screen was built from with the server's.

Demo mode picks among lobby tracks that `HasTrack` (an export exists and has not failed to build).

## Materials

`-run=ApexMaterialBake [-force]` (`ApexTrackEditor/ApexTrackMaterialGraphs`) generates the four graphs the level builder used to generate per track and saves them once under `/Game/Materials/Track`:

- `M_ApexTrackBase`
- `M_ApexEmissive`
- `M_ApexBrand`
- `M_ApexDecal`

Without `-force` it bakes only the missing ones. It also bakes the base again when `/Game/Ground` has been imported or removed since, because the base carries one of two surface graphs depending on it. The runtime reads which graph it got from whether the base exposes `TextureAmount`. A game that finds no base parent logs an error and draws every track in flat colours on the engine's basic shape material, rather than drawing nothing.

## Packaging and scripts

- `DefaultGame.ini`: `bCookAll` as before. `/Game/Materials`, `/Game/Props`, `/Game/Ground` and `/Game/Cars/Wheels` are always cooked; `/Game/Tracks` is in `DirectoriesToNeverCook`.
- `build_game_standalone.ps1` copies every export and preview into `Tracks/` beside `ApexSim.exe` (`-SkipTracks` leaves them out).
- `build_release.ps1` always ships the circuits in `Game/Tracks`. It aborts if a circuit has no export or the materials were never baked. The `-RuntimeTracks` switch and the catalog-sync stage are gone.
- `initialize_content.ps1` has a material stage. It treats a missing `.uemesh`, preview or sidecar as a missing track. It no longer rebakes every circuit after a prop or ground-texture import, because the game dresses each track with what `/Game` holds when it builds it.
- `build_track_levels.ps1 -ImportLevels` still imports circuits as levels under `/Game/Tracks`. They are for opening a circuit in the editor, and for the lighting comparison below. The game never loads them.

## Switches

| | Default | What it does |
|---|---|---|
| `-ApexTracksDir=<dir>[+<dir>]` | — | Extra track folders, searched first |
| `apexsim.track.Rescan` | — | Scan the track folders again (picks up re-exported circuits; drops the kept track) |
| `apexsim.track.BuildBudgetMs` | 20 | Game-thread milliseconds per frame for building meshes |
| `apexsim.track.KeepLast` | 1 | Keep the last track hidden for reuse |

## Tests

- `track-core` (`cargo test -p track-core`): write/read round trip bit-identical; byte-identical re-export; manifest key order and header fields before the first array; v1 compatibility; v3 and damaged blobs refused; manifest/blob disagreement refused; the golden blob; `source_crc` check vector and CRLF.
- Unreal automation (not yet run):
  - `ApexSim.Track.Reader.GoldenBlob` — the same golden blob;
  - `ApexSim.Track.Reader.Rejects` — bad magic, truncation, trailing bytes, future version, index out of range;
  - `ApexSim.Track.Reader.Manifest` — header streaming, the 32-bit checksum, blob loading, a renamed mesh refused;
  - `ApexSim.Track.Builder.Prepare` — shared vertex instances, tangents along `u`, stand-in meshes;
  - `ApexSim.Track.Ground.*` and `ApexSim.Props.*`, which moved with their code.

## Open risks

1. **The Unreal code has not been compiled.** It was reviewed against UE 5.x APIs by reading (no errors found; the runtime bugs found were fixed). Build the editor target, run `-DisableAdaptiveUnity` once for the new `.cpp` files, then run the `ApexSim.Track.*` tests.
2. **Software Lumen needs mesh distance fields**, and runtime-built meshes have none. `DefaultEngine.ini` generates them for cooked meshes, and its comment records that without them "every surface used to render as one flat ambient term". Kit props keep theirs; the road and terrain (89 % of a track's triangles) now contribute nothing to DF AO, DF shadows or the global distance field.
   - Measure it: A/B a circuit against its `-ImportLevels` level in the editor.
   - Mitigations, if it shows: hardware ray-traced Lumen (`r.Lumen.HardwareRayTracing=1`), or a coarse heightfield proxy built at export time.
3. **Build time and memory** on the big circuits (Spa, Le Mans, the Nordschleife's 37 MB blob) are bounded by the frame budget but not measured. Measure with the log line `Track <Stem> built in … s`.
4. **Tangent handedness** on the normal-mapped ground sets is computed by hand for the fast build. Check it in the same A/B as risk 2.
5. **Collision cook** at runtime follows the ProcMesh pattern and is believed sound. It is unverified until the racing line snaps (`Racing line: … on the road` in the log).
6. Content trust is unchanged: loose files are player-editable, and the content-checksum toast still covers "your track differs from the server's".

## Cars (phase 2, not started)

The client needs a GLB → `UStaticMesh` loader with named material slots, and PBR materials whose parameters the livery, ghost and brake-light code addresses. Those are Interchange's names — `BaseColorFactor`, `MetallicFactor`, `BaseColorTexture`, `EmissiveFactor` — used in `ApexCarLivery.cpp`, `ApexRaceCarActor.cpp` and `ApexGhostCarActor.cpp`. Two options:

- **(a) glTFRuntime** (MIT, UE5, maintained). It loads static meshes with materials and textures at runtime, but has its own parent materials and parameter names, so the call sites need a parameter-name indirection.
- **(b) A minimal in-house GLB reader.** The car GLBs come from the project's own Blender scripts and use a known subset: one buffer, float attributes, u16/u32 indices, PNG textures, metallic-roughness, no skins or animations. That is about 600–800 lines on top of `BuildFromMeshDescriptions` (the same fast path the tracks use), `FImageUtils::ImportBufferAsTexture2D` and one authored `M_ApexCarPBR` parent with exactly the parameter names the game already uses.

(b) fits the codebase and keeps a plugin's material conventions out of game code; (a) is faster to prototype.

- Wheels (`SM_Wheel_<model>`) and the DRS flap GLB use the same loader.
- Catalog rows would come from `car.toml`, using `ApexCarImport`'s minimal TOML scanner, moved into the runtime module the way the track reader was.
- Livery logos are PNGs, loaded as runtime textures.
- Cockpit, headlights and wheel placement are all derived from the mesh bounds, so a runtime mesh with correct bounds needs nothing else. No sockets are used anywhere.

Estimate: 2–3 weeks.

## Background: the investigation

*At `1310b8b`. Question: can the pipeline emit files the packaged game loads at runtime, so that new tracks (from the AC survey route or anywhere else) and cars need no editor import and no repackage?*

**Verdict (then):** yes for tracks, and cheaper than it looked. The cooked level held nothing but three kinds of thing:

- generated triangle meshes with parameterised materials;
- instances of cooked kit props;
- a handful of actors identical on every track.

The `.uescene.json` was already a complete description of that; what was missing was a runtime consumer. The original estimate was 3–4 weeks for tracks, and two spikes first: runtime complex collision under Chaos, and software Lumen without distance fields.

**How the design landed.** The investigation proposed seven steps: split the export, one builder for both paths, materials as cooked assets, runtime meshes, a loader, a catalog provider, packaging. All seven were built as proposed, with these differences:

- **Collision** is a component beside each mesh, not the mesh's own body setup. A body setup owned by a mesh asset will not cook in a packaged game.
- **Source policy.** The first cut kept cooked levels, with a per-track source policy (`auto` / `runtime` / `cooked`). The second cut removed cooked levels from the game and the build entirely. The editor import survives only as an inspection tool.
- **Catalog.** The catalog reads the manifest's head rather than a separate index file, which is why the manifest's key order is part of the format.

The chronological record is [gdoct/apexsim#48](https://github.com/gdoct/apexsim/pull/48) and its commits.
