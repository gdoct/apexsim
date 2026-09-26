# Runtime-loadable tracks and cars — design investigation

*2026-09-26. Follows `docs/AC_IMPORT_FEASIBILITY.md`. Repo at `1310b8b`, Unreal 5.8 client. Question: can the content pipeline emit files the packaged game loads at runtime, so that new tracks (from the AC survey route or anywhere else) and cars don't need an editor import and a repackage?*

## Verdict

Yes for tracks, and it is cheaper than it looks, because ApexSim tracks are already "data + kit": the level the commandlet bakes contains nothing but generated triangle meshes with parameterised materials, instances of cooked kit props, and a handful of identical-everywhere actors. The `.uescene.json` that `ats-export` writes is already a complete runtime-loadable description; what's missing is a runtime consumer for it instead of the editor-only one. Cars are a separate, smaller job that needs a GLB loader.

The consequence is bigger than AC import: with runtime tracks, the editor drops out of the track loop entirely. `ats-export` → restart the game. No `UnrealEditor-Cmd -run=ApexTrackImport`, no "editor must be closed", no cook per track, and the release can ship all circuits as loose files (8–15 MB compressed each) instead of cooked levels.

Estimate: **3–4 weeks** for runtime tracks to parity with the cooked path (one shared code path for both), **2–3 weeks** for runtime cars. Two risks need a one-day spike each before committing: runtime complex collision under Chaos (low risk) and software Lumen without distance fields on runtime meshes (real risk, see Risks).

## What the cooked level actually contains

`FApexTrackAssetBuilder::BuildLevel` (`ApexTrackAssetBuilder.cpp:2600–3050`) puts into `L_<Stem>`:

| In the level | Track-specific? | Runtime equivalent |
|---|---|---|
| One `AStaticMeshActor` per baked mesh (road, curbs, markings, surface bands, ground, structures, decal meshes), complex-as-simple collision, Nanite off, no tags (:2625–2660) | geometry yes | `UStaticMesh::BuildFromMeshDescriptions` with `bFastBuild=true` (runtime-supported; the builder already uses this API at :2203–2217 with `bFastBuild=false`), or `UDynamicMeshComponent` |
| `MI_<key>` instances of a per-track generated `M_ApexTrackBase` + `M_ApexEmissive`/`M_ApexBrand`/`M_ApexDecal` (:969–1955) | parameters yes, graph **no** | one authored/cooked parent per family group + `UMaterialInstanceDynamic` per key. The code already says "swap `ParentMaterial` for a hand-authored asset later" (:966–968). The only per-track branch is `bGroundTextures` (:1193), decided by whether `/Game/Ground` exists — always true in a package |
| One `AActor` + `UHierarchicalInstancedStaticMeshComponent` per (kind, asset, text), tagged `ApexProp`, 1 custom-data float (:2682–2719) | transforms yes, meshes no | same, spawned at runtime; meshes via `StaticLoadObject` of `/Game/Props/<kind>/SM_<asset>` (cooked by `bCookAll`) |
| Grandstands: `SpawnGrandstand` rows of bays/caps (:2472–2554); ferris wheel `AApexRotorActor`; sky `AApexSkyDriftActor`; bridges scaled on Y; board text via `UTextRenderComponent` | yes | same code, moved to a runtime module (`AApexSkyDriftActor`/`AApexRotorActor` already live in ApexSim, `Race/ApexPropActors.h`) |
| Generated stand-ins `SM_Prop_<kind>` from `kPropRecipes` (:769–776) and `SM_Prop_start_gantry` sized to the road width (:2342–2377) | gantry width yes | recipes are MeshDescription code — runs at runtime too; or cook one gantry per width bucket |
| Start lights actor tagged `ApexStartLights` with `Light0..4` components tagged `ApexStartLight` (:2382–2470) | position yes | same, spawned at runtime with the same tags |
| `AExponentialHeightFog` "TrackFog", unbound `APostProcessVolume` "TrackPostProcess" (:2992–3038) | **no** — identical constants on every track | spawn once at runtime |
| `APlayerStart` × 16 grid (:2967–2977) | yes | **nothing reads them** at runtime; drop |
| Sun, sky light, atmosphere | — | not in the level at all; `L_Menu` provides them and the race director retunes them (`ApexRaceDirector.cpp:1371–1442`) |

Everything textured (ground sets in `/Game/Ground`, kit props, brand/flag/marker textures, decal PNGs) is a cooked asset referenced by path. A runtime track needs **no texture loading at all** — only a preview PNG for the catalog card.

## What the runtime reads from a loaded track

All of it is by tag, name or trace, none of it by asset path — which is why a runtime build can be a drop-in:

- Level streaming: `ResolveTrackLevelPath`/`LoadTrackLevel`/`UnloadTrackLevel` (`ApexRaceDirector.cpp:2154–2296`), gated on `FPackageName::DoesPackageExist`; readiness via `IsTrackLevelLoaded`/`IsLevelVisible` (:2656–2664); demo picks a track with the same `DoesPackageExist` (`ApexDemoModeSubsystem.cpp:223–231, 340–342`).
- `ApplyTrackLevelConditions` (:1523–1673) iterates `TrackLevel->GetLoadedLevel()->Actors` and finds the fog actor, road materials by name prefix (`MI_road`, `MI_pit_lane`, `MI_wear_`), lamp glow by tag `ApexEmissive_floodlight_lamp`, floodlights by mesh name.
- Start lights by tag (:716–753). Racing-line snap and TV-director ground/clear tests by complex `ECC_WorldStatic` traces (`ApexRacingLineActor.cpp:169–261`, `ApexRaceDirector.cpp:2380–2401, 2938–2960`); these are the **only** collision consumers — cars, wheels, rain, dots are all NoCollision.
- `IsRoadSurface` (`ApexRacingLineActor.cpp:65–81`) rejects hits whose actor is not in the track `ULevel` — the one place that assumes a streamed level; replace with a `ApexRoad` tag.
- Minimap, TV director path and racing line come from the **server** (`FApexTrackConfigSummary.Centerline`, `Net->GetRacingLine()`), not the level.
- Catalog: `UApexMenuFlowSubsystem::GetTrackCatalogRow`/`GetCarCatalogRow`/`FindTrackIdByStem` (`ApexMenuFlowSubsystem.cpp:243–296, 433–445`) are the single seam every UI and race consumer goes through (one debug exception in `ApexAudioPreview.cpp:171`). `FApexTrackCatalogRow` has no level pointer — the level path is derived from the YAML stem — and a `TSoftObjectPtr<UTexture2D> PreviewImage`.

## Measured: what a track is on disk

Built `ats-export` here and exported two circuits:

| | Zandvoort | Spa |
|---|---|---|
| `.uescene.json` | 44.1 MB | 77.8 MB |
| zlib-compressed | 8.2 MB | 15.1 MB |
| meshes / materials | 299 / 19 | 466 / 21 |
| vertices / triangles | 597 k / 614 k | 982 k / 1.04 M |
| of which `surface` (terrain) family | 89 % | 89 % |
| road + curb + pit_lane + marking | 67 k tris | 110 k tris |
| props (instances) / distinct assets | 7,383 / 41 | 15,801 / 48 |
| equivalent raw binary (f32 pos/nrm/uv, u32 idx) | 26 MB | 44 MB |
| DOM parse in Python | 0.8 s | 1.4 s |
| export time | 2.7 s | 4.1 s |

(`TRACK_EDITOR.md:69`'s "4–5 MB per track" predates the DEM terrain.) A DOM JSON parse of 78 MB in `FJsonSerializer` is several seconds and a few hundred MB of `TSharedPtr<FJsonValue>` — the vertex data should not be JSON at runtime.

## Design

**1. Split the export into manifest + mesh blob.** `ats-export` keeps writing `<Stem>.uescene.json` for everything small (format/version, ids, metadata, dressing, materials, props, grid, centerline, pit lane, start/finish — ~2 MB for Spa) and adds `<Stem>.uemesh` — a trivial little-endian binary: header, then per mesh `name, material_key, vertex count, index count, f32 positions/normals/uvs, u32 indices`, optionally zlib-compressed per mesh (`FCompression` on the UE side). The JSON `meshes` array becomes a list of offsets. `ApexTrackSceneReader` (Core + Json only, `ApexTrackSceneReader.cpp:5–8`) gains the blob reader; the commandlet keeps working unchanged apart from that. Rust side: one function in `ue_export_io.rs`.

**2. Move the builder's runtime-safe half into a runtime module** (`ApexSimContent`, or into `ApexSim`): `ApexTrackSceneData.h`, `ApexTrackSceneReader`, `ApexGroundMaterials` and `ApexPropLibrary` (both include only CoreMinimal), the family → parameter table (:1608–1670), `SpawnGrandstand`, prop resolution (`FindAuthoredMesh`/`ResolveProp` :1957–2031), start lights, fog/post-process constants, and the mesh recipes. Public API: `UApexTrackSceneBuilder::Build(UWorld*, const FApexTrackSceneData&, FApexTrackBuildOptions) → FApexBuiltTrack` (owned actors list, road actors, start lights, fog). `ApexTrackEditor` becomes a thin wrapper: create the `EWorldType::Inactive` world (:2609), call the same builder with `bEditorAssets=true` (materials as `UMaterialInstanceConstant` via `Set*ParameterValueEditorOnly`, meshes saved as packages), save. **One code path** for cooked and runtime tracks, so they cannot drift. The editor-asset flavour is worth keeping for the demo track and for anyone who wants Nanite/DF-built meshes on shipped circuits.

**3. Materials become assets.** Turn the generated `M_ApexTrackBase`, `M_ApexEmissive`, `M_ApexBrand`, `M_ApexDecal` graphs into four cooked assets under `/Game/Materials/Track/` (a one-off commandlet `ApexMaterialBake` that runs the existing graph-building code and saves — no hand editing, consistent with the repo's "nothing hand-made" rule). Runtime instances are `UMaterialInstanceDynamic::Create(Parent)` with the same parameter names (`BaseColor`, `SecondaryColor`, `Roughness`, `StripePeriod`, `Noise*`, `Edge*`, `Texture*`, `AlbedoMap/NormalMap/RoughnessMap`). Name the MIDs `MI_<key>` so the wet-road prefix matching in `ApplyTrackLevelConditions` keeps working, or better, tag the road components and match on tags.

**4. Runtime meshes.** `BuildFromMeshDescriptions` with `bFastBuild=true`, `bAllowCpuAccess=true`, `bBuildSimpleCollision=false`; then `BodySetup->CollisionTraceFlag=CTF_UseComplexAsSimple` and `CreatePhysicsMeshes()` (Chaos cooks trimeshes at runtime — this is how `UProceduralMeshComponent` works). Give collision only to the road/curb/pit_lane/marking families (67–110 k tris) plus `surface` if the TV director's ground trace needs it off-road; skip `structure`. Alternative: `UDynamicMeshComponent` (GeometryFramework, runtime) which has `EnableComplexAsSimpleCollision()` built in and no DDC at all; slightly more memory, simpler code. Expect 1–3 s for a Spa-sized track on a loading screen; can be async (`FBuildMeshDescriptionsParams` on a worker for the description fill, `BuildFromMeshDescriptions` on the game thread per mesh, spread over frames).

**5. Loader replaces level streaming.** `UApexTrackLoader` (subsystem): `Load(track_id)` → catalog says cooked or runtime → either `LoadLevelInstanceBySoftObjectPtr` as today or `SceneBuilder::Build` into the persistent world; exposes `IsLoaded/IsVisible/Unload/Actors()` so the director's gates (:450–459, :2656–2664, :2541) and `ApplyTrackLevelConditions` iterate the loader's actor list instead of `GetLoadedLevel()->Actors`. Change `IsRoadSurface` to a tag test. Demo mode: keep the demo circuit cooked, or let it pick a runtime one — either works once the catalog answers "exists".

**6. Catalog provider.** Behind `GetTrackCatalogRow`/`GetCarCatalogRow`/`FindTrackIdByStem`, merge the DataTable rows with rows discovered from a content directory the client scans at startup: the natural location is the same `content/` tree the release already ships for the server (`build_release.ps1` copies YAML + sidecars into `Server/content`), or a `Content/` folder beside `ApexSim.exe` next to `settings.yml` (`ApexBootSettings.cpp:252–263` resolves that folder already). Per track: `<Stem>.yaml` (for `ApexContent::Compute` → `SourceCrc`, and name/metadata/length), `<Stem>.uescene.json` + `<Stem>.uemesh`, `<Stem>.preview.png` (`build_track_catalog.py` already renders previews; load with `FImageUtils::ImportFileAsTexture2D`). Add a `TObjectPtr<UTexture2D> RuntimePreview` to `FApexTrackCatalogRow` (consumers do `LoadSynchronous` on the soft pointer at `ApexTrackSelectWidget.cpp:276, 531`, `ApexTrackCardWidget.cpp:62`, `ApexSessionCreateWidget.cpp:633` — four sites to teach the fallback). Checksums and the mismatch toast work unchanged because the CRC is computed from the same YAML the server has.

**7. Packaging.** Keep `bCookAll` but make the dependency explicit: `DirectoriesToAlwaysCook` += `/Game/Props`, `/Game/Ground`, `/Game/Materials`, `/Game/Cars/Wheels`, `/Game/Props/_Parents`. Stage the content directory as loose files (`DirectoriesToAlwaysStageAsNonUFS`) or copy it in `build_release.ps1` as it does for the server. Cooked levels become optional: `build_track_levels.ps1` can be skipped in the release for every track that ships as data.

**8. Cars (phase 2).** The client needs a GLB → `UStaticMesh` loader with named material slots and PBR materials whose parameters the livery/ghost/brake-light code addresses (`BaseColorFactor`, `MetallicFactor`, `BaseColorTexture`, `EmissiveFactor` — Interchange's names, `ApexCarLivery.cpp:11–33`, `ApexRaceCarActor.cpp:82–91`, `ApexGhostCarActor.cpp:20–40`). Two options: (a) **glTFRuntime** (MIT, UE5, actively maintained; loads static meshes with materials and textures at runtime, has its own parent materials and parameter names — the four call sites above would need a parameter-name indirection); (b) a **minimal in-house GLB reader**: the car GLBs are produced by the project's own Blender scripts and use a known subset (one buffer, float attributes, u16/u32 indices, PNG textures, metallic-roughness, no skins/animations) — ~600–800 lines on top of `BuildFromMeshDescriptions`, `FImageUtils::ImportBufferAsTexture2D` and one authored `M_ApexCarPBR` parent with exactly the parameter names the game already uses. (b) is more in keeping with the codebase and avoids a plugin whose material conventions leak into game code; (a) is faster to prototype. Wheels (`SM_Wheel_<model>`) and the DRS flap GLB use the same loader. Catalog rows from `car.toml`: the commandlet already has a minimal TOML scanner (`ApexCarImportCommandlet.h:128–133`) — move it. Livery logos are PNGs → runtime textures. Cockpit, headlights, wheel placement are all bounds-derived (`ApexRaceCarActor.cpp:260–375`, `ApexCarWheels.cpp:34–121`), so a runtime mesh with correct bounds needs nothing else. No sockets are used anywhere.

## What the pipeline emits, per track, after this

```
content/tracks/real/<Stem>.yaml            server + client CRC, catalog metadata
content/tracks/real/<Stem>.{ground,curbs,walls}.msgpack   server only (unchanged)
content/tracks/real/<Stem>.uescene.json    manifest: materials, props, grid, centerline (~1–2 MB)
content/tracks/real/<Stem>.uemesh          mesh blob (8–15 MB compressed)
content/tracks/real/<Stem>.preview.png     catalog card
```

All five are produced by tools that already exist (`ats-export`, `build_track_catalog.py`) with the split in step 1 as the only exporter change. The AC survey route (`ac-import-feasibility.md`, "the kn5 as a survey") produces the inputs to those tools, so an AC-derived circuit reaches a packaged game with zero Unreal involvement.

## Risks, in order

1. **Software Lumen needs mesh distance fields** (`DefaultEngine.ini:24–27`: `r.GenerateMeshDistanceFields=True`, `DynamicGlobalIlluminationMethod=1`; the comment there records that without DFs "every surface used to render as one flat ambient term"). Distance fields are built in the editor/DDC; runtime-built meshes have none. Kit props keep theirs, but the road and the terrain — 89 % of the triangles and the surfaces that bounce light — would not contribute to DF AO, DF shadows or software Lumen's global distance field. Mitigations: hardware ray-traced Lumen (`r.Lumen.HardwareRayTracing=1`; needs DX12 + RT GPU, which the target audience likely has); or a coarse per-track distance field/heightfield proxy built at export time (Lumen has a heightfield/landscape path — worth checking whether a runtime `ULandscape`-less heightfield can feed it); or accept flatter lighting on runtime tracks and keep the shipped circuits cooked. **Spike this first**: build Zandvoort at runtime with the prototype and A/B it against the cooked level under both Lumen modes. It decides whether shipped circuits stay cooked.
2. **Runtime complex collision under Chaos.** Expected to work (ProceduralMeshComponent depends on it); one-day spike to confirm on `BuildFromMeshDescriptions` meshes with `bAllowCpuAccess`, else use `UDynamicMeshComponent` for the road family.
3. **Load time and memory.** 1 M tris built on the game thread — measure; spread over frames or use a loading screen (there is a startup splash and a lobby, both fine places). Keep vertex data out of JSON (step 1) or memory triples.
4. **Two code paths drifting.** Avoided by design step 2 — the commandlet calls the runtime builder. Don't keep the editor builder as a separate implementation.
5. **Parameter/tag contracts.** The runtime reads materials by name prefix and props by tag/mesh name; retag on spawn and keep the `MI_<key>` naming, or migrate the three readers to tags in the same change.
6. **No Nanite on runtime meshes.** Irrelevant: track meshes are Nanite-off already (:2226); Nanite props stay cooked.
7. **Content trust.** Loose files are player-editable; the CRC toast already covers "your track differs from the server's". No new attack surface beyond what a modded `car.toml` already gives.

## Effort

| Step | Size |
|---|---|
| Manifest + `.uemesh` split in `ats-export` and the reader | 2–3 days |
| Runtime module: move reader/props/ground/family table; runtime builder producing the same actors/tags; editor wrapper calling it | 1.5–2 weeks |
| Materials as cooked assets + MIDs | 2–3 days |
| Loader subsystem, director gates, `IsRoadSurface` tag, conditions pass over loader actors | 3–4 days |
| Catalog provider + preview PNG + packaging/staging | 3–4 days |
| Spikes (Lumen DF, Chaos runtime collision) | 2 days, **first** |
| Cars: GLB loader, car PBR parent, catalog from `car.toml`, wheels/DRS | 2–3 weeks |

Tracks ≈ 3–4 weeks to parity; cars ≈ 2–3 weeks after. No server changes. The AC survey importer is independent and can proceed in parallel since it targets the existing YAML/dossier/DEM inputs.
