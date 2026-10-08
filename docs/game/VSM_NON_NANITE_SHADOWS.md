# The "Non-Nanite Marking Job Queue overflow" warning

Research note, 2026-10-08. The question: should we fix the warning by making
our meshes Nanite, or by tuning shadow maps or LODs? And if Nanite, how can
we do it without much work? Nothing below has been built yet. It is here so
we can choose.

**Short answer.** We can't simply turn on Nanite for the meshes that cause the
warning. The tracks and cars are built in the running game with
`UStaticMesh::BuildFromMeshDescriptions(bFastBuild = true)`. That path never
builds Nanite data, and the Nanite builder is an editor-only module. The
warning comes from a few *large* non-Nanite instances, such as ground
patches, road sections and the horizon, while the shadow cache is cold.
Their triangle count plays no part, so limiting LODs would not help at all.
The cheap fix is to stop flat ground-hugging surfaces casting shadows and
stop invalidating the whole shadow cache whenever the sun moves. Nanite for
the tracks would need **optional cooked track packages**, which is a real
architectural decision (section 5). It would also give the tracks the mesh
distance fields they lack today.

## Done and measured (2026-10-08)

Options A1, A2 and A3 below are built. Each can be switched from the console
for comparison, and a switch applies at once to the track on screen:

| Switch | Default | What it does |
|---|---|---|
| `apexsim.track.FlatShadows` | `0` | Road, curbs, paint, pit lane, decals and the `surface_*` bands cast no shadow (`FApexTrackMaterial::IsFlatSurface`). They still receive shadows. |
| `apexsim.track.HorizonShadows` | `0` | The DEM horizon tiles cast no shadow. |
| `apexsim.sky.SunStepDeg` | `0.5` | The sun is turned only once it has drifted this far (`ApexSky::SunNeedsTurning`). That is every two game minutes, and never just because the cloud or rain changed. |

Measured with 75 s unattended AI races at Zandvoort, cockpit view, against
the release server. Each line is a single run. The log reports only the
first overflow of a session.

| Run | Overflow |
|---|---|
| Baseline (old behaviour forced back on), 1× clock | In the frame the track finished building |
| Flat surfaces + sun step, 1× clock | None |
| Baseline, 60× clock | In the frame the track finished building |
| Flat surfaces + sun step, 60× clock | Once, ~11 s into the race, when cars crashed |
| All three, 60× clock (our car crashed, 51% front damage) | None |

`r.Shadow.Virtual.NonNanite.NumPageAreaDiagSlots 8` named the worst
offenders on screen: terrain tiles (`ground_*`, 500–1430 pages), horizon
tiles (`horizon_*`, up to 1005 pages) and the player's own car (~515 pages
at the finest levels). Cars, debris and props never appeared. A crash
overflows because the cockpit camera swings onto ground whose shadow pages
were never requested, so every big tile under it becomes a large job at
once. The terrain still casts shadows on purpose: hills shadow the track.

More runs later the same day (other cars, night and noon) still often
overflowed **once, as the track appears**. In the first frame every shadow
page is uncached, and the 49 terrain tiles alone are large jobs at levels
14–18 (about 245 against a queue of 128). Sampling the on-screen message
every 5 s through a 60 s race showed that one overflow near load and none
afterwards. `r.Shadow.Virtual.NonNanite.IncludeInCoarsePages 0` (A5) made
no difference, in the samples or in single runs, so it stays at the
default. A one-frame overflow at load is a slower frame, not a problem.
If overflows show up during racing, the next step is C, or smaller terrain
tiles in `ue_export` (at levels 14–18, a tile under about 100 m is a small
job).

Tests: `ApexSim.Sky.SunStep`, `ApexSim.Track.Builder.FlatSurfaces`.

---

## 1. What the warning actually means

Source: `Engine/Shaders/Private/VirtualShadowMaps/VirtualShadowMapBuildPerPageDrawCommands.usf`
(`CullPerPageDrawCommandsCs`) and `VirtualShadowMapCacheManager.cpp:1081`.

Before a virtual shadow map (VSM) frame draws non-Nanite geometry, a compute
pass culls every non-Nanite instance against every shadow view and *marks*
the shadow pages it covers. One thread handles one instance, in groups of
64 instances. For each (instance, clipmap level, view):

- If its page rectangle covers **8 pages or fewer**, the thread marks the
  pages itself.
- If it covers **more than 8 pages**, it goes into a group-shared "large job"
  queue, so all 64 threads can mark it together. The queue holds
  `2 × 64 = 128` jobs.
- When the queue is full, the remaining large jobs are marked by their own
  thread, one page at a time, and the overflow flag is set.

So this is a **performance warning, not a correctness bug**: nothing is
dropped, but one thread may walk thousands of pages serially. (The separate
"visible instances buffer overflow" warning is the one that loses shadows.)
Every non-Nanite instance writes dirty flags (`STATIC` or `DYNAMIC`), so
marking `ShadowCacheInvalidationBehavior = Static` does not make an instance
skip this queue. It only stops the instance *invalidating* cached pages.

**What fills the queue.** Only big instances, in the *uncached* part of the
map (`VirtualShadowMapGetUncachedScreenRect`), at many clipmap levels.
The defaults are `r.Shadow.Virtual.Clipmap.FirstLevel 6` and `LastLevel 22`,
which gives 17 levels. Level *L* is `2^(L+2)` cm across in 128 pages, so a
page is `2^(L-5)` cm wide: 2 cm at level 6 and 1.3 km at level 22.

| Our instance | Size | Clipmap levels where it is a large job (> 3×3 pages) |
|---|---|---|
| Road / band / curb section (`SECTION_LEN_M`) | 250 m | 6–18 when near the camera: up to **13 per view** |
| Ground patch | ~384 m | 6–19: up to **14 per view** |
| Horizon tile (64–81 of them over ~17 km) | ~1–2 km | the coarse levels, up to ~1000 pages measured |
| Barrier module, tree, board, car | 1–20 m | none above a few levels (small jobs) |

These are worked estimates; section 3 says how to measure. With about ten big
pieces near the camera in one 64-instance group, the queue is full. The
multipliers are:

- **Views.** The loop runs over every primary view, so triple screens triple
  it. Mirror captures add their own shadow views when `Mirror quality > 0`.
- **A cold cache.** Normally only the strips the clipmap scrolls into are
  uncached. Everything is uncached when:
  - the track has just been built. Every one of the 26 logs that show the
    warning shows it right after `Track … built`;
  - **the sun direction changes.** The clipmap cache key includes
    `LightDirection` (`VirtualShadowMapClipmap.cpp:334`), and
    `AApexRaceDirector::ApplySkyLighting` calls `Sun->SetActorRotation` on
    every live-sky step. That is every game-clock minute, or once a real
    second at a 60× time scale;
  - the camera cuts a long way (TV director, replays, the track guide's
    jump cuts).

The log line is printed **once per session** (`LoggedOverflowFlags`), so
`Saved/Logs` can't tell us how often it happens. On screen it stays up for
10 s after each overflow, with "(N seconds ago)".

## 2. Our content today

| Content | How it is built | Nanite possible? |
|---|---|---|
| Track surfaces, terrain, horizon, walls, paint (`FApexTrackSceneBuilder`, `ApexTrackInstance.cpp:96`) | Runtime, `bFastBuild = true`, one LOD, 250 m sections | **No, not at runtime** (see 4) |
| AC-imported scenery | Runtime, same path | No (and translucent scenery never can be) |
| Cars, wheels, DRS flaps (`ApexCarContentSubsystem.cpp:923`) | Runtime from GLB, fast build | No; they are small anyway |
| Prop kit (`/Game/Props`, `ApexPropImport`) | Cooked assets | **Yes.** Already on for grandstand, building, pit, bridge, attraction (`ApexPropLibrary.cpp` `kKinds`) |
| Materials | Cooked parents, dynamic instances | Nanite is a mesh property; the parents need the Nanite usage flag, which `M_ApexTrackBase` etc. already carry |

Everything is `Movable` with `ShadowCacheInvalidationBehavior = Static`
(`ApexTrackSceneBuilder.cpp:2010-2018`; the comment there is from an earlier
round of this same warning). **No track mesh sets `CastShadow`, so the road,
the paint, the run-off bands, the curbs and the 17 km horizon all render into
the VSM.**

## 3. Measure first (an afternoon, no code)

Run these in a race on two circuits (Zandvoort, Spa), single and triple
screen, cockpit and TV:

```
r.Shadow.Virtual.NonNanite.NumPageAreaDiagSlots 8   ; names the worst primitives on screen
r.Shadow.Virtual.ShowStats 1                         ; instance counts and page totals
stat gpu / stat VirtualShadowMapCache                ; cost of the passes
r.Shadow.Virtual.Cache 0/1                           ; worst case vs normal
apexsim.cam.Goto ... / -ApexTimeScale=60             ; force sun moves and cuts
```

The first cvar prints "Primitive '<actor>' overlapped N pages" for the worst
offenders, using our actor labels. That turns the table in section 1 into
measured numbers, and it tells us whether the warning is a load-time blip or
happens over and over during a race. **Only the second is worth
architecture.**

## 4. Why Nanite can't simply be switched on for the runtime meshes

- `UStaticMesh::BuildFromMeshDescriptions` in a non-editor build
  `check`s `bFastBuild` (`StaticMesh.cpp` ~8925). The fast path only fills
  `LODResources` and ray tracing data. It never touches `NaniteResourcesPtr`.
- Building Nanite data is `Engine/Source/Developer/NaniteBuilder`, a Developer
  module. A game target links it only with `bBuildDeveloperTools`, Shipping
  never. It is also slow: seconds per million triangles, against our 20 ms
  per-frame build budget (`apexsim.track.BuildBudgetMs`).
- `Nanite::FResources::Serialize` is public, so data could in principle be
  built offline and read back. But `StreamablePages` is `FByteBulkData`, which
  expects a package file to stream from. Doing this outside the asset system
  means depending on engine internals that change with every engine version.

So **Nanite for tracks means cooking the track meshes**, and cooking per
track is the reverse of the "Runtime tracks" decision. Cars have the same
problem but don't matter here.

## 5. Options

### A. Cheap fixes inside the runtime pipeline (recommended first)

1. **Turn off shadow casting on flat, ground-hugging families.** These are
   the road, curbs, curb strips, paint and decals, run-off and ground bands,
   the pit lane and wear bands. They receive shadows, which is unaffected,
   but cast nothing anyone can see: a 5 cm curb's shadow is below a VSM page
   texel at most levels. With `CastShadow = false` they leave the VSM passes
   entirely, and they are most of the 250 m sections. Keep it on for the
   terrain (hills shadow the track at dusk), walls, parapets and the
   underpass deck. The family is already on `FApexTrackMaterial`, so this is
   about 10 lines in the builder plus a check that the road still *receives*
   shadows. Of everything here, this is likely to make the biggest
   difference for the least work.
2. **The horizon.** It is already 64–81 tiles per circuit. Either
   `CastShadow = false` (we lose distant hills shadowing distant hills) or
   keep it. Decide after measuring.
3. **Stop invalidating the cache with the sun.** Only rotate the sun when it
   has moved more than about 0.25° (`ApexSky::LiveSkyMoved` already gates the
   relight, so add the angle to that check), and leave it alone in a frozen
   clock (`time_scale` 0). At 60× time scale this goes from a full shadow
   rebuild every second to about one a minute.
4. **Mirrors.** `bAlwaysPersistRenderingState` is already on, so captures
   keep a view state. Check that their VSM is actually cached (the
   `UniqueViewKey` path in `VirtualShadowMapClipmap.cpp:332`). If it isn't,
   drop dynamic shadows from the mirrors at the lower Mirror quality levels,
   as `SetDynamicShadows(Quality > 0)` already does at zero.
5. **Cvars to try** (scalability, so they can go per quality level in
   `DefaultScalability.ini`):
   - `r.Shadow.Virtual.NonNanite.IncludeInCoarsePages 0`. Its help text calls
     this "a significant performance win". Coarse pages are what far-field
     and volumetric lookups read.
   - `r.Shadow.Virtual.Clipmap.LastLevel 20`. The default reaches about
     168 km; 20 still covers about 21 km, more than the 8 km DEM horizon,
     and drops two levels of work for every big instance.

### B. Nanite for the cooked prop kit (cheap, modest gain)

Turn the `nanite` column on for `barrier`, `fence`, `board`, `light`,
`tire_wall` and `misc`, then rerun `ApexPropImport -all`. It is a table flip
plus a re-import. It cuts the non-Nanite instance count (thousands of
barrier modules and boards), which helps the visible-instance buffer and the
VSM raster cost. It **does not** help the marking queue much, because these
are small jobs.

- **Trees.** Masked, two-sided card trees on Nanite work in 5.8 but cost a
  lot in overdraw (Nanite's masked path). Test this on its own before
  enabling it.
- **The sky kind** (blimp) moves, so it stays non-Nanite. Nothing else in
  the kit uses WPO.

### C. Nanite for tracks via optional cooked track packages (big)

The editor-only `ApexTrackImport` (`build_track_levels.ps1 -ImportLevels`)
already runs the same builder with a factory that **saves packages**. The
change would be:

1. In the saving factory, enable `NaniteSettings.bEnabled` on the opaque
   surface meshes (not decals, not translucent AC scenery) and save them to
   `/Game/TrackMeshes/<Stem>/` instead of the never-cooked `/Game/Tracks`.
2. Cook those, either in the main package or as one pak/IoStore chunk per
   circuit (`bCookAll` already covers by-path content).
3. `UApexTrackInstance` checks for a cooked mesh set whose `source_crc`
   matches the export and uses it, otherwise it falls back to today's
   runtime build. Collision, props, materials and tags stay as they are.

What this gets us:

- The track surfaces render Nanite into VSM, so this warning disappears for
  good.
- The track meshes get **mesh distance fields** from the cook. This is
  "risk 1" in docs/content/RUNTIME_CONTENT_LOADING.md: software Lumen and
  DF shadows see the road and terrain poorly today.
- Cooked meshes load faster than the runtime build.

What it costs:

- Every shipped circuit goes back into the cook. That adds cook time (all
  circuits, the engine needed), package size (likely tens of MB per circuit,
  to be measured), and `build_release.ps1` / `initialize_content.ps1`
  stages.
- A re-exported circuit falls back to the slower non-Nanite path until it
  is re-cooked. A dropped-in custom or AC-imported track always does, so
  this warning can still appear there and option A is needed anyway.
- Two render paths to test. A CRC mismatch must never show a stale cooked
  mesh.

### D. Fall back to cascaded shadow maps (not recommended)

Classic CSM, plus distance-field shadows for the far field, is the usual
non-Nanite setup and has no marking pass. We would lose VSM's sharp,
consistent detail in the cockpit and on the cars, and the distance-field
shadows would need the very DFs the runtime meshes lack (see C). It would
also be a project-wide visual regression made to fix a warning. Keep it only
as a low-scalability fallback.

### What doesn't help: limiting LODs

Marking works on **instance bounds** (`LocalBoundsExtent`, the screen rect
per clipmap level), not on triangles. Fewer LODs or simpler LODs change the
raster cost of the shadow pages, not the marking queue. Our runtime meshes
have a single LOD anyway.

## 6. Proposed order

1. Measure (section 3). Is it a one-off at load, or repeated during a race?
2. A1 + A3 (no shadows from flat surfaces, quantised sun). These are small,
   low-risk and testable with the existing screenshot loop
   (`-ApexAutoRace -ApexTimeScale=60`, `NumPageAreaDiagSlots`).
3. Re-measure. If the warning only shows at load and on big cuts, stop here
   and add A5 per scalability level.
4. B (prop kit Nanite, minus trees) as general VSM headroom.
5. Decide on C if the lack of distance fields (Lumen quality) or remaining
   overflows justify bringing back a per-track cook.
