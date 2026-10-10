# Virtual shadow maps and non-Nanite track meshes

Status: proposal, not built. The cheap fixes (A1-A3 below) are built; the
options past them are research notes for a decision.

The question: fix virtual shadow maps' (VSM) "Non-Nanite Marking Job Queue
overflow" with Nanite, shadow tuning, or LODs?

## Current state (checked against the code)

- Tracks and cars are built at runtime with
  `UStaticMesh::BuildFromMeshDescriptions` and `bFastBuild = true`
  (`Track/ApexTrackInstance.cpp`, `Cars/ApexCarContentSubsystem.cpp`): one
  LOD, no Nanite data. Track surfaces come in 250 m sections
  (`SECTION_LEN_M` in `track-editor/core/src/ue_export.rs`).
- Track components are movable with `ShadowCacheInvalidationBehavior =
  Static` (`Track/ApexTrackSceneBuilder.cpp`).
- **Built (A1, A2)**: `apexsim.track.FlatShadows` (default 0) and
  `apexsim.track.HorizonShadows` (default 0). Flat surfaces
  (`FApexTrackMaterial::IsFlatSurface`: road, curb, marking, pit lane,
  decal and the `surface_*` bands) and the DEM horizon tiles cast no shadow;
  they still receive. Terrain, walls and structures cast (hills shadow the
  track). Both switches apply at once to the track on screen.
- **Built (A3)**: `apexsim.sky.SunStepDeg` (default 0.5). The sun is turned
  only once it has drifted that far (`ApexSky::SunNeedsTurning`), never for a
  cloud or rain change; every turn invalidates every cached shadow page.
- Mirrors drop dynamic shadows at Mirror quality 0 (`ApexCockpitRig.cpp`).
  The cooked prop kit has Nanite on for `grandstand`, `building`, `pit`,
  `bridge`, `attraction` (`Track/ApexPropLibrary.cpp`, `kKinds`).
- Tests: `ApexSim.Sky.SunStep`, `ApexSim.Track.Builder.FlatSurfaces`.

With A1-A3, unattended races mostly show the overflow once, in the frame the
track appears (every page uncached; the terrain tiles alone exceed the
queue), and occasionally when a crash swings the cockpit camera onto ground
whose pages were never requested. A one-frame overflow at load is a slow
frame, not a defect. `r.Shadow.Virtual.NonNanite.IncludeInCoarsePages 0`
made no measurable difference and is left at the default.

## What the warning means

Before VSM draws non-Nanite geometry, a compute pass marks the shadow pages
each instance covers, one thread per instance in groups of 64. An instance
covering more than 8 pages at a clipmap level goes into a shared large-job
queue of 128 per group; past that, the thread marks it alone and the
overflow flag is set. It is a performance warning: nothing is dropped. (The
"visible instances buffer overflow" warning is the one that loses shadows.)
`ShadowCacheInvalidationBehavior = Static` does not keep an instance out of
the queue; it only stops it invalidating cached pages.

What fills it: **large instances** in the uncached part of the map, at many
clipmap levels (defaults `r.Shadow.Virtual.Clipmap.FirstLevel 6` to
`LastLevel 22`). Our 250 m sections, ground patches and kilometre-wide
horizon tiles are large jobs at a dozen levels each; barriers, trees, boards
and cars are small jobs. Multipliers: every primary view (triple screens
triple it), mirror captures, and a cold cache (track just built, sun
rotated, a long camera cut). The log line prints once per session; on screen
it shows for 10 s after each overflow.

**Triangle count plays no part**: marking works on instance bounds, so fewer
or simpler LODs would not help.

## Measuring

`r.Shadow.Virtual.NonNanite.NumPageAreaDiagSlots 8` names the worst
primitives on screen (with `-ApexAutoRace -ApexTimeScale=60`, cockpit view:
terrain `ground_*` tiles, horizon tiles, the player's own car); also
`r.Shadow.Virtual.ShowStats 1`, `stat VirtualShadowMapCache`,
`r.Shadow.Virtual.Cache 0/1`.

## Why Nanite cannot simply be switched on

- In a non-editor build `BuildFromMeshDescriptions` requires `bFastBuild`,
  whose path never builds Nanite resources.
- The Nanite builder is a Developer module (not linked in Shipping) and takes
  seconds per million triangles against the 20 ms per-frame build budget
  (`apexsim.track.BuildBudgetMs`).
- Serialising prebuilt Nanite data outside the asset system would depend on
  engine internals (`FByteBulkData` streaming) that change per engine version.

So Nanite for tracks means cooking track meshes, the reverse of runtime
tracks ([track-pipeline.md](../content/track-pipeline.md)).

## Remaining options

**A4. Mirrors.** Check that mirror captures' VSM is actually cached; if not,
drop their dynamic shadows at lower Mirror quality levels too.

**A5. Scalability cvars.** `r.Shadow.Virtual.Clipmap.LastLevel 20` still
covers about 21 km (more than the 8 km DEM horizon) and removes two levels
of work per large instance. Per quality level in `DefaultScalability.ini`.

**Smaller terrain tiles** in `ue_export`: a tile under about 100 m is a small job.

**B. Nanite for more of the cooked prop kit.** Flip the `nanite` column for
`barrier`, `fence`, `board`, `light`, `tire_wall`, `misc` and re-run
`ApexPropImport -all`. Cuts non-Nanite instance count (helps the visible
instance buffer and raster cost), not the marking queue. Trees (masked
two-sided cards) need their own overdraw test; `sky` moves and stays
non-Nanite.

**C. Optional cooked track packages.** The editor-only `ApexTrackImport`
already runs the same builder with a factory that saves packages. Enable
Nanite on opaque surface meshes there, save to a cooked folder per circuit,
and have `UApexTrackInstance` use a cooked set whose `source_crc` matches the
export, else build at runtime as now. Gains: Nanite shadows for track
surfaces, **mesh distance fields** for software Lumen and DF shadows (the
runtime meshes have none), faster loads. Costs: every circuit back in the
cook (time, package size, build stages), two render paths to test, and a
re-exported, custom or imported track still on the runtime path, so A stays
needed. A CRC mismatch must never show a stale cooked mesh.

**D. Cascaded shadow maps.** No marking pass, but loses VSM detail and DF far
shadows need distance fields; a low-scalability fallback at most.

## Suggested order

1. If overflows appear during racing (not only at load): A5, then smaller
   terrain tiles.
2. B (minus trees) as general VSM headroom.
3. C only if missing distance fields (Lumen quality) or remaining overflows
   justify a per-track cook.
