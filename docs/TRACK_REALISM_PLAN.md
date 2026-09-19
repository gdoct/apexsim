# Track realism plan — Red Bull Ring first

> Status (2026-09-19, evening): steps 1–6 are built, and step 7's assets
> were authored by the owner. See §7 for what landed, what was verified and
> what is still open. The Red Bull Ring (`Spielberg`) was the pilot; every
> mechanism runs over all 26 circuits.

## 1. What is actually wrong at Spielberg (measured, not guessed)

The eight complaints reduce to five root causes. Each is stated with where
it lives in the code, so the fixes in §3 can be checked against it.

| # | Complaint | Root cause | Where |
|---|---|---|---|
| 1 | Bull statue never visible | The dossier gained the `statue` landmark on 2026-09-18 (`c26d3b8`), but `ats-dress` was never re-run afterwards, so `Spielberg.ats` had no `misc` prop and this morning's level was baked from the stale scene. Nothing in `build_track_levels.ps1` runs `ats-dress`; the dossier → scene step is manual and silent. Also, the statue *is* in OSM (`tourism=artwork` node 5443064309, "Der Bulle vom Spielberg") but `osm_layout.py` only scans `man_made=tower` / `tower:type=lighting` / `attraction=big_wheel`, so it was hand-placed 31 m from its real spot. | `scripts/osm_layout.py:1553-1566`, `track-editor/src/dress.rs:519`, `scripts/build_track_levels.ps1` |
| 2 | Blocky, 90s look | Terrain is one flat colour (`GROUND_COLOR`) with procedural noise, tessellated at 4 m within 52 m of the road and 12 m beyond; surface bands are flat colours per kind with no textures; every prop is a low-poly flat-shaded box recipe or a scripted kit mesh with vertex/flat colours, no albedo/normal maps; trees are solid blobs. See §1.2. | `terrain.rs:49,136`, `ue_export.rs:2033-2044`, `ApexTrackAssetBuilder.cpp:1266-1279`, `content/props/_tools/apex_props.py` |
| 3 | Barriers not consistently tecpro | No barrier *type* is ever decided. Straights get `armco_generic` hard-coded; corner walls copy whatever asset the 2026-09 enrichment left as an anchor (`tire_wall_generic` everywhere). `tecpro_2m`, `concrete_4m*`, `armco_4m_fence`, `armco_end`, `tires_corner` are built and never emitted. Coverage at a corner depends on a legacy anchor being within 40 m. OSM has 53 `barrier=wall` and 27 `barrier=tyres` ways around the ring that are never read. | `groom.rs:192,982,1394`, `osm_layout.py` (no barrier extraction) |
| 4 | Not using all props | `ats-dress` lays only what the dossier has: stands, structures, crossings, landmarks, pit lane. The dossier has no layer for parking, camp sites, service roads, fences, marshal posts, lights, vehicles, sky, fanzone, and OSM's 17 car parks, 5 camp sites, 157 service roads, 20 bridges, 36 streams near the ring are dropped at extraction. `skyline_*`, `vehicle/*`, `attraction/*`, `fence/*`, `sign/marshal_post`, `sign/flag_pole`, `misc/*` have no placement rule at all. | `osm_layout.py:1243 extract()`, `dress.rs:107 dress_scene` |
| 5 | Dark at night | The only dynamic lights are spawned on `floodlight_tower` / `lamp_post` instances; Spielberg has none (the real ring is not floodlit) and the director has no fallback. Pit lane, garages, stands and the paddock have no lit windows or lamps. | `ApexRaceDirector.cpp:1572-1621`, `dress.rs:531` |
| 6 | Malformed corners | The centerline is a raw 5 m GPS polyline with kinks: at Remus (station 1394) the sampled radius is 8 m on an 10.6 m wide road, and curvature jumps 0.07/m between adjacent samples (T1 and Remus). Catmull-Rom interpolates *through* the kinks, the inner road edge folds, and curbs/edge lines follow. No smoothing or minimum-radius pass exists. | `content/tracks/real/Spielberg.yaml`, `server/src/track_loader.rs`, `track-editor/src/track_path.rs` |
| 7 | Basic surroundings | Ground is an inverse-distance blanket of the centerline's own heights: no hills, no valleys, no fields, no roads, no buildings beyond the 9 OSM structures; only 130 m grass bands, astroturf and asphalt run-off. No gravel, no meadow, no farmland. | `terrain.rs:326-342`, `Spielberg.ats` surfaces |
| 8 | No mountains | The ground mesh stops 800 m from the road and there is no backdrop of any kind: the track sublevel holds only a height-fog actor, the sky is the bare `SkyAtmosphere` in `L_Menu`. The centerline's own relief is also about half the real one (35 m modelled vs ~65 m real), so even the near hills are flat. | `terrain.rs:54`, `L_Spielberg.umap`, `ApexSkyModel.h` |

### 1.1 The data we already have and do not use

The cached OSM extract for the ring (`content/tracks/osm-cache/Spielberg.0.json`)
holds, within 250 m of the road: 110 service roads, 70 grass and 11 meadow
polygons, 53 walls and 27 tyre-barrier ways, 40 buildings, 21 streams, 15
shingle beds, 10 scrub patches, 8 car parks, 8 forest polygons, 9 named
grandstands, 4 chapels, the S36 motorway, the fan-zone restaurant "Bull's
Lane" and the statue. The dossier keeps the stands, 9 structures, the pit
lane, 8 woods and 7 corner names. Everything else is discarded at
`extract()`.

### 1.2 Why it reads as "90s"

Measured on the baked `Spielberg.uescene.json` and the import logs:

- **No surface art.** The whole circuit is 15 flat colours through one
  generated parent material whose only texture is the engine's 64×64
  grey-noise tile, repeated every 50 cm on the road and every metre on
  grass (`ApexTrackAssetBuilder.cpp:88, 883-1337`). No albedo, normal or
  roughness maps, no decals, no blend between surfaces: grass meets
  asphalt as a geometric seam between two colours.
- **The road is two triangles per metre.** `fn road()` lofts one quad
  across the full width (`ue_export.rs:1765-1780`): 8 632 triangles for
  4.3 km, no crown, no camber detail, nothing for a normal map to bite.
  The painted edge lines cost twice that because quads share no
  vertices.
- **Kerbs are a 5 cm two-facet ramp** with the red/white done in the
  shader (`ue_export.rs:1933-1990`); no sawtooth, no nose, no wear.
- **Width is a staircase.** Both samplers hold `width_left/right` and
  banking from the segment's start node across the 5 m segment
  (`track_path.rs:68-83`, `track_loader.rs:639-667`), so the edge, the
  white line, the kerb origin and the grass band all step; up to 0.85 m
  at Spielberg and 2.4 m at Austin. Spielberg also has zero banking in
  its YAML.
- **Corners degrade by deletion.** Three mechanisms in `ue_export.rs`
  (`clamp_profile` to 0.85/κ at `:1399-1453`, strip breaking under
  0.5 m of room, and `emit_quad` dropping folded, sliver or twisted
  facets at `:1523-1585`) trade holes for knots at every apex tighter
  than the offset, and the 8 m curvature window under-reads a real
  hairpin so the clamp fires late.
- **Terrain is 12 m quads of one colour**, 4 m only within 52 m of the
  road, shaped by the centerline's own heights and nothing else.
- **Foliage is at 1990s density and there is almost none.** A conifer is
  97 triangles, a broadleaf 202–322, solid blobs with no cards; the
  groomer plants only in a 22–90 m annulus and its clearance rules reject
  94 % of cells, so a `forest` circuit carries **183 trees**, none within
  22 m of the road and none beyond 90 m. One car mesh is four times the
  triangle count of all of them.
- **One quality level.** Props ship LOD0-only with no LOD settings in
  the import commandlet; track meshes have no Nanite, no lightmaps and
  fully dynamic lighting. 64 of the kit's 115 GLBs carry no texture at
  all (every barrier, every plain stand bay, all skyline buildings, all
  vehicles, the statue); only the 18 tree GLBs have normal and roughness
  maps.
- **Nothing between 90 m and the horizon**: no fields, hedgerows, farm
  roads, power lines, villages or hills; then fog; then a gradient.

## 2. Quick fixes already applied (this session)

1. `ats-dress` re-run on Spielberg: the scene now carries
   `misc/bull_statue` at (-453, 318, z 15.9), and `ats-export` has written
   it into `Spielberg.uescene.json`. The level still needs a bake
   (`scripts/build_track_levels.ps1 -Track Spielberg`) to show it; the
   Unreal side resolves `misc/bull_statue` to the imported
   `SM_bull_statue` (already in `/Game/Props/misc`) and nothing culls it.
2. `dress.rs`: a `misc` landmark was not owned by the dressing pass
   (`dressed_kind` had no `Misc` arm), so every re-dress would have laid a
   second statue on the same spot and grooming could push it. New
   `dressed_prop` owns the `misc` assets `lay_landmark` can produce and
   leaves hand-placed bollards alone; used by the sweep, the id recycling
   and the groom exemption; covered by
   `a_statue_landmark_is_laid_once_and_owned`.
3. This document.

Everything else below is proposed.

## 3. Strategy

The theme: **stop inventing, start measuring.** Every rule that guesses
(barrier offset 7 m, stands wherever there is room, grass 130 m wide,
ground from the centerline) is replaced by data from a public source with
a stated provenance, and the guess survives only as the fallback for a
circuit the source does not cover. Four data layers, in the order they
pay off at Spielberg.

### 3.1 Layer 1 — real terrain (fixes 7, 8, half of 2)

**Source.** A free 30 m DEM: Copernicus GLO-30 (worldwide, CC-BY-like
licence, AWS open bucket) or the national 10 m/1 m lidar where it exists
(Austria's `data.gv.at` ALS 1 m covers Styria; the UK, NL, DE, FR, IT, ES,
JP all publish ≤5 m). `scripts/osm_layout.py` already georeferences the
YAML frame onto WGS-84 with a 2.2 m rmse fit; the inverse of that
transform puts a DEM tile into the track frame for free.

**Pipeline.**

- New `scripts/dem_fetch.py <Stem>`: downloads the tiles covering the
  bbox inflated to **8 km** (the horizon a driver sees from a 700 m
  valley floor), caches under `content/tracks/dem-cache/`, resamples to
  the track frame and writes `content/tracks/real/<Stem>.dem.msgpack`
  (two grids: 10 m inner, 90 m outer ring to 8 km; f32 heights; the
  transform and source recorded).
- `terrain.rs` takes the DEM as the height *source* instead of the IDW
  blanket. The road-ceiling carve, verge hold and 6–35 m blend stay
  exactly as they are, applied on top, so the car still follows the
  rendered verge and the server's `ground.msgpack` contract does not
  change. Without a DEM the IDW path remains.
- **Road heights re-fitted to the DEM.** The YAML z is a smoothed GPS
  altitude with half the real relief. A one-off pass samples the DEM
  along the centerline, low-passes it (asphalt has no 30 m bumps), and
  writes it back with a `z_source: dem` note. This changes physics
  (braking into Remus downhill is the whole point of the circuit), so it
  goes through the grip probe and the lap-time survey before landing.
- `ue_export.rs` bakes the outer ring as a separate coarse mesh set
  (`SM_horizon_*`, 90 m cells, one material) so the hills are geometry,
  not a skybox card: the sun lights them, they catch fog and shadow
  correctly at dusk, and the TV camera can look at them. Beyond 8 km a
  simple distance-blended sky card is unnecessary at Spielberg (the
  valley is closed by the Seetaler Alpen and Gleinalpe within 6 km).
- Land cover on the outer ring from OSM `landuse` / `natural`: forest
  polygons planted with a cheap far-tree impostor (not the near kit),
  farmland/meadow as two ground colours, villages (`landuse=residential`)
  as clusters of `skyline_lowrise` at 1/3 scale or a new
  `building/village_house` set. This is what makes the Murtal look like
  the Murtal rather than a golf course.

**Check.** A screenshot from the grid looking up the T1 climb should show
the Red Bull Tribüne against a wooded ridge, not sky.

### 3.2 Layer 2 — a full furniture dossier (fixes 1, 3, 4, 7)

`osm_layout.py` becomes a general "everything within 400 m of the road"
extractor. New dossier sections, all in the track frame with `source`
stamped, all optional so old dossiers still load:

| section | OSM tags | who consumes it |
|---|---|---|
| `barriers[]` | `barrier=wall/tyres/fence/guard_rail`, `material=*`, plus `highway=raceway` edge geometry | `ats-dress` lays the real barrier lines (§3.3) |
| `roads[]` | `highway=service/track/unclassified/residential/motorway` within 400 m, with `bridge`/`tunnel` | new `road` surface kind: grey ribbons on the terrain, access roads behind the stands, the S36 |
| `landcover[]` | `landuse=grass/meadow/farmland/forest/residential/farmyard`, `natural=scrub/shingle/water/wood`, `leisure=park` | replaces the 130 m grass band; drives ground colour, scatter and tree planting |
| `parking[]` | `amenity=parking` polygons | asphalt/gravel surface + `vehicle/car_*` rows + `light/lamp_post` |
| `camps[]` | `tourism=camp_site` | `attraction/tent_6m` clusters + `vehicle/motorhome` (Spielberg has five named camps around the ring) |
| `water[]` | `waterway=stream/river`, `natural=water` | ground cut and a water material; 21 streams at the ring |
| `poi[]` | `tourism=artwork/information`, `amenity=restaurant/atm/fuel/place_of_worship`, `man_made=*`, `power=tower/line` | landmarks (the bull statue by its real node), chapels, pylons |
| `paths[]` | `highway=footway/path/steps`, `highway=crossing` | spectator paths behind the fences, footbridge approaches |
| `fences[]` | `barrier=fence/chain/gate` | `fence/mesh_4m` runs, gates |

Two rules for the extractor:

- **Never silently drop a class.** Every tagged way or node within range
  that no rule claims is written to `unclassified[]` with its tags, and
  `--verbose` lists the tag census, so the next circuit's gaps are
  visible instead of discovered in a screenshot.
- **Manual tables stay** (`MANUAL_*`) but every manual entry must cite its
  source, as they do now, and a manual entry is dropped when the automatic
  scan finds the same feature within 30 m (so the bull statue's hand
  position yields to the OSM node).

Where OSM is thin, two more sources fill in, both public:

- **Aerial imagery** for placement that OSM lacks (tyre-wall extents,
  gravel traps, kerb lengths, paddock layout). Not fetched automatically:
  a `scripts/trace_helper.py` opens the georeferenced fit in the track
  editor as a background layer (Bing/Esri imagery is viewable, not
  redistributable; nothing is stored), and the editor gets a "trace
  polyline → `.layout.json` manual entry" tool. The editor already has
  the MCP; this is one more tool on it.
- **Published circuit maps** (the fan map in the brief: stand names,
  fan zone, camps, ticket gates, the ferris wheel position) for names and
  the things imagery cannot tell apart. These go in `MANUAL_*` with the
  URL, as the existing stand entries do.

### 3.3 Layer 3 — dressing rules that decide instead of inherit (fixes 3, 4, 5)

`dress.rs` and `groom.rs` gain a **barrier pass with a decision**, and the
kit's unused assets get a home.

**Barriers.** The line comes from the dossier's `barriers[]` where it has
one, else from the run-off's outer edge as now. The *type* is decided per
4 m cell from a table, in this order:

1. dossier `barrier=tyres` → `tire_wall/tires_4m`, ends `tires_corner`
2. dossier `barrier=wall` with `material=concrete` or on the pit straight
   → `concrete_4m_rail`
3. curvature ahead: a cell whose approach speed (from the racing-line
   profile the server already computes) is over 200 km/h and whose
   impact angle to the wall is over 20° → `tecpro_2m` in front of armco
   (this is the FIA's own rule of thumb and what the ring actually does
   at T1, Remus, Schlossgold and Rindt)
4. anywhere a stand or path is within 12 m → `armco_4m_fence`
   (debris fence)
5. otherwise `armco_4m`, every run closed with `armco_end`.

Coverage is complete by construction: every cell on both sides gets a
type unless the pit lane or an underpass claims it, so a corner without a
legacy anchor is no longer bare. `walls.msgpack` already maps `tecpro*` to
the tyre restitution, so the physics side needs nothing.

**The rest of the kit.**

| kit | rule |
|---|---|
| `light/floodlight_tower` | dossier `tower:type=lighting` as now, **plus** a fallback when a session is after dark and the circuit has none: masts every 120 m along the pit straight and at each named corner's outside, behind the barrier. Flag-lit in the log so nobody mistakes it for the real ring |
| `light/lamp_post` | every `parking[]` polygon (one per 25 m of perimeter), the paddock roads, behind every stand |
| pit garages, `pit/garage_end`, `hospitality_3f` | after dark, emissive window slots (`M_ApexEmissive`) at a fixed warm level; the race director already drives `ApexEmissive_*` tags |
| `sign/marshal_post` | one per named corner on the outside at the apex, plus every 400 m on straights |
| `sign/flag_pole` | a row of 8 behind the start/finish stand; `text` from the host country and the session's drivers |
| `fence/mesh_4m` | along every spectator area edge (`landcover` grass polygons adjoining a stand or `paths[]`) |
| `vehicle/*` | `parking[]` fill at 60 %, `race_truck` × boxes/4 in the paddock, `ambulance`/`fire_truck` at marshal posts 1 and n/2 |
| `attraction/*` | `camps[]` → tents; fan zone (`poi` restaurant/stage within 150 m of a stand) → `fanzone_stage`, `video_screen` facing the biggest stand, `portaloo_row` |
| `sky/*` | one `blimp` over the main stand on a dry day, `helicopter` on a 200 m circle in demo sessions |
| `misc/*` | statue from `poi`, `bollard` rows at pit entry, `kerb_marker` at every kerb start |
| `building/skyline_*` | not at Spielberg; villages in `landcover` residential get the new low-rise set |

Every rule writes `source: rule/<name>` on the prop so the editor can show
why a thing is there and `ats-dress --explain` can list it.

### 3.4 Layer 4 — the look (fixes 2, and what remains of 7)

This is the part where the "single geometry" rule is in the way, and the
answer is to keep half of it.

**Keep:** one GLB per asset, one static mesh per asset, one HISM per
(asset, text) at runtime, box collision from the builder. That is what
keeps 2 000 barriers and 1 200 trees cheap, and nothing above needs to
change it.

**Drop:** "one material, flat colours, no textures". Replace with:

- **Textured kit.** Each GLB may carry several materials with baked albedo,
  normal and roughness maps (2 K per asset, atlased where the tool builds
  variants). `apex_props.py` gets a `bake_pbr` step (Blender's own baker)
  and a small procedural texture library (galvanised steel, painted
  concrete, tyre rubber, seat plastic, tarpaulin, chain-link). Interchange
  already imports glTF PBR textures; nothing in the commandlet changes
  except the shared-material rule (keep it, keyed by material *name*, so
  `armco_galv` is one material across the kind).
- **Ground textures.** `M_ApexTrackBase`'s `surface` family gets real
  tiling textures per kind (grass, dry grass, gravel, asphalt run-off,
  concrete, astroturf) with a macro variation map and a distance blend,
  and the terrain gets a **splat** from `landcover[]` (4 channels: grass,
  meadow, forest floor, bare) instead of one colour. Texture sources:
  ambientCG/Poly Haven (CC0) so the licence question does not arise.
- **Road.** A tiling asphalt normal + the existing wear bands; a kerb
  paint decal with chipped edges; painted lines as decals with edge wear.
  Plus two exporter changes: loft the road with lateral columns
  (edge, 1 m, quarter, centre: 8 verts per section) so camber, crown
  and a real kerb profile (sawtooth or rounded nose, 4–6 facets, the
  stripe still from the shader) exist as geometry, and share vertices
  along a strip so the edge lines stop costing more than the road.
- **Width and banking interpolated**, not held per node: `sample_at`
  in both samplers lerps `width_left/right` and banking between the
  segment's end nodes. One line each, and it removes every staircase
  notch. The server and the exporter must change together (the curbs
  sidecar and the track limits read the same width).
- **Finer terrain near the road**: 2 m tessellation within 60 m (from 4),
  6 m to 200 m, 12 m beyond. Spielberg's ground tiles would go from 56 to
  roughly 90; still trivial against the props.
- **Trees.** Keep the solid blobs as the far LOD, add a **near tree**
  set (`broadleaf_m_near`, `conifer_m_near`: real card foliage, 2–4 K
  tris, alpha-masked, imported with the LOD chain the commandlet does not
  set today) that the groomer uses within 60 m of the road and the
  builder LODs to the blob at 120 m. Open the belt: plant from the fence
  line to the wood polygon's edge (`landcover[]` forest), not a 22–90 m
  annulus, and relax the 40 m stand and 6 m prop clearances that reject
  94 % of cells. Target for Spielberg is 3 000–5 000 trees near the
  road plus far impostors on every forest polygon to 8 km. The autumn
  variants follow.
- **Grass scatter.** A runtime foliage layer (UE's own `GrassType` on the
  landscape material, or an HISM of grass cards the builder scatters on
  the `grass` surface within 40 m of the road) is the single cheapest
  change that removes the "flat green plane" reading. Density from a
  settings slider.
- **Crowds and life.** The stands already swap to `_crowd`; add flags on
  the roof line, banners on the fences (`mesh_4m_hoarding` with brand
  text), and the fan zone above.

### 3.5 Corner geometry (fix 6)

Small, self-contained, and it changes physics, so it lands first and
alone:

- `track_loader.rs` / `track_path.rs`: a **curvature-aware smoothing pass**
  on load: fit a clothoid-constrained spline (or, simpler, a 3-point
  running fit that caps `|Δκ|` per metre at a bound tuned so real hairpins
  keep their 15–20 m radius and GPS kinks are ironed), and enforce a
  minimum centerline radius of `max(width, 12 m)` so the inside edge never
  folds. Result written back to the YAML by a `track-fix` tool with a
  `smoothed: <date>` note, so the server and exporter read identical
  geometry and the determinism test is unaffected.
- With a smooth, bounded-curvature centerline the exporter's three
  repair mechanisms (`clamp_profile`, strip breaking, facet rejection)
  stop firing at real corners; they stay as guards but `ats-export`
  gains a `--strict` mode that fails the export when any facet is
  dropped, so a regression is caught in CI instead of a screenshot.
  `curvature_at`'s 8 m window can then shrink to 3 m.
- The OSM `highway=raceway` ways at the ring are far cleaner than the GPS
  trace (rmse 2.2 m of *fit*, but their own curvature is smooth): a second
  option is to re-derive the centerline from the two raceway *edges* where
  OSM has both (`area:highway` or `raceway` + `width`), and keep the trace
  only for z and where OSM is missing. Try Remus and T1 with both and pick
  by the racing-line lap time and a screenshot.

## 4. Order of work

Each step is independently shippable and visible in a screenshot at
Spielberg. Estimates are for a working, tested change on this repo's
patterns, not polish.

| step | delivers | fixes | size |
|---|---|---|---|
| 0 | Bake Spielberg from the re-dressed scene (done here up to the bake) | 1 | minutes |
| 1 | `osm_layout.py` reads `tourism=artwork`, `barrier=*`, `landuse`/`natural`, `amenity=parking`, `tourism=camp_site`, roads, water, `unclassified[]` census; `build_track_levels.ps1` runs `ats-dress` first and refuses a stale scene | 1, 4 (data) | 1–2 days |
| 2 | Corner smoothing + minimum radius, width/banking lerp in both samplers, `ats-export --strict`, re-probed lap times | 6, part of 2 | 1–2 days |
| 3 | Barrier decision pass with tecpro/concrete/fence/ends, full coverage | 3 | 1–2 days |
| 4 | Kit placement rules (lights with night fallback, marshal posts, fences, parking + vehicles, camps, flags, statue by node) | 4, 5 | 2–3 days |
| 5 | DEM fetch, terrain from DEM, road z re-fit, 8 km horizon ring with land cover | 7, 8 | 3–4 days |
| 6 | Ground textures + splat, road/kerb materials, road lateral loft + kerb profile, 2 m near tessellation, grass scatter | 2 | 3–4 days |
| 7 | Textured kit with LODs (barriers, stands, pit first), near trees, open tree belt | 2 | 1 week+, ongoing per asset |

Steps 1–4 are pure data and rules and touch no Unreal asset beyond a
re-bake. Steps 5–7 are where the look changes. Doing 5 before 6 is
deliberate: mountains and real relief change the picture more per day
than any texture.

## 5. What to verify at each step

- `ats-dress --dry-run --verbose` and `--explain` list every rule that
  fired and every dossier entry that could not be placed.
- The all-circuit AI survey test (`tests/`) still finishes at Spielberg
  after steps 2, 3 and 5 (walls and road z both move).
- `cargo test` in `track-editor` for the extractor census, the barrier
  table and the smoothing bound; the existing dressing-twice-is-a-no-op
  test stays green.
- Screenshots via `-ApexAutoRace -ApexTrack=Spielberg -ApexCameraLookAt=...`
  from four fixed poses (grid up the T1 climb, Remus exit, Schlossgold
  outside, Rindt to the finish) kept under `content/tracks/_shots/` so a
  before/after is one command.
- A night run (`-ApexTimeOfDay=22:00`) must show the pit straight lit.

## 6. Open questions for the owner

- DEM licence: Copernicus GLO-30 requires attribution in the credits;
  Austria's 1 m ALS is CC-BY 4.0. Both fine for a source-available game;
  confirm.
- Re-fitting road z from the DEM changes every lap time on every circuit.
  Do it per circuit as each is re-dressed, or all at once?
- Textures: CC0 libraries or authored in-house? Affects step 7 most.
- The `single geometry` wording in `PROPS.md` should read "one mesh, many
  materials"; agree before step 7 starts so assets are authored once.

## 7. Build log (2026-09-19)

What landed, against the order of work in §4. Every item runs over all 26
circuits, not just the pilot.

| step | built | verified by |
|---|---|---|
| 1 | `osm_layout.py` extracts `barriers`, `roads`, `waterways`, `areas`, `poi` and an `unclassified` census; the statue comes from its OSM node; `build_track_levels.ps1` runs `ats-dress` before every bake | dossiers rebuilt for 22 circuits; census shows the gaps per circuit |
| 2 | `ats-smooth` (Savitzky–Golay by twicing + local re-walk of pinched corners, raceline carried along); width and banking interpolated in both samplers | 8 unit tests; tightest radius ~21 m on every circuit bar Shanghai and the Norisring; all racelines stay on the road |
| 3 | `barriers.rs`: a decided barrier line with complete coverage, Tecpro/tyres/armco/fence/concrete and end caps | 12 unit tests; the AI survey (below) |
| 4 | `surroundings` pass: car parks, camps, village, chapels, pylons, marshal posts, named corner boards, floodlights where a circuit has none, forest impostors on the slopes; near trees and grass scatter in the tree pass | 4 unit tests; dressing is byte-identical on a second run |
| 5 | `dem_fetch.py` (Copernicus GLO-30) + `dem.rs`; terrain crossfades to the real land; a `horizon` mesh to 8 km | Spielberg's line reads 678.6 m against a published ~678 m; ground now reaches 368 m and the horizon 949 m |
| 6 | ground textures baked and imported; `M_ApexTrackBase` samples them with macro variation and distance fade; 2 m terrain near the road | built and bake-tested in the editor; not yet seen on screen |

### Mistakes caught on the way, and what caught them

These are recorded because each one passed a narrower check than the one
that found it.

- **Smoothing that rescaled the circuit.** Laplacian smoothing shrank a test
  ring by the full tolerance and Taubin grew it by 0.44 m; both passed
  "is it smoother". A test that measured *shape* caught them.
- **A raceline that drifted off the road.** Re-walking a corner moved the
  centerline up to 3 m and the raceline did not follow. The Le Mans AI test
  caught one car in four leaving the track.
- **A ceiling that flattened the mountains.** The road-protection ceiling
  limits how steeply ground rises, and clipped the real hills from 408 m to
  196 m. Caught by checking the exported elevation range, not the code.
- **Props seated on the wrong ground.** Dressing and grooming seated props
  on the old blanket while the exporter drew the real land, so everything
  far from the road would have floated or been buried. Caught by seating
  and drawing from one shared terrain.
- **Barriers the AI could not avoid.** Complete coverage at a 6 m verge,
  then pit walls in the middle of Albert Park's road from a stale pit lane.
  Neither showed in any unit test; the all-circuit AI survey caught both.
- **Bulls everywhere.** The artwork scan would have stood the Red Bull bull
  at every sculpture near every circuit. Caught by diffing every dossier
  against the committed one.

### Still open

- **On-screen verification.** Nothing here has been looked at in a race.
  The levels need `scripts/build_track_levels.ps1` (with `-Build` for the
  C++ changes) and a screenshot run before any number is trusted by eye.
- **AI at a few circuits.** See the AI survey figures in the hand-over;
  some circuits still spend more time off the road than they did.
- **Unused kit.** `concrete_4m*` has no circuit that warrants it (none of
  the 26 has a wall hard against the road); skyline and palms have no
  placement rule yet; sausage kerbs, flags, the podium and the hillside
  letters are not placed.
- **Shanghai and the Norisring** keep a hairpin under the 22 m floor: their
  traces are genuinely that tight, and `--tight-tol 10` would open them at
  the cost of moving the line several metres.
