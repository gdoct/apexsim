# Track pipeline

How a circuit goes from a traced centerline to what the server simulates and
the client draws. The road comes from a GPS trace or OpenStreetMap, its
furniture from an OSM **layout dossier**, the land from a Copernicus
elevation model. Tools in `track-editor/` (Rust) and `tools/` (Python) turn
those into a scene (`.ats`), server sidecars and a client export, which the
game builds at runtime (no cooked track levels). Formats:
[track-format.md](track-format.md); per-circuit rules: [circuits.md](circuits.md).

## Code

- `tools/`: `osm_layout.py`, `trace_layout.py`, `dem_fetch.py`, `dem_elevation.py`,
  `drs_zones.py`, `track_location.py`, `bridge_elevation.py`,
  `osm_centerline.py`, `seed_scene.py`, `generate_race_line.py`,
  `build_track_catalog.py`, `check_walls.py`, `bake_ground_textures.py`;
  helpers `track_dirs.py`, `lib/ApexTracks.ps1`. The bake driver is
  `bake.ps1` at the content root (cargo and Python only); the code root's
  `apexsim/scripts/build_track_levels.ps1` runs it and adds the code's
  stage (materials).
- `track-editor/core/src/`: `layout.rs`, `dress.rs`, `groom.rs`,
  `barriers.rs`, `circuit_style.rs`, `pit.rs`, `track_smooth.rs`,
  `track_bank.rs`, `dem.rs`, `terrain.rs`, `strip_layout.rs`, `ue_export.rs`,
  `ue_export_io.rs`; binaries `ats-{dress,groom,smooth,bank,export}` in `bin/`.
- The sidecars' formats: `apexsim-lib/formats/src/` (`ground.rs`,
  `curbs.rs`, `walls.rs`, `road_mesh.rs`, `pit.rs`: the structs, their
  versions and paths), which track-core writes and the server reads.
- Server readers: `apexsim/server/sim/src/track_loader.rs`, `track_content.rs`,
  `ground.rs`, `curbs.rs`, `walls.rs`, `road_mesh.rs`, `pit.rs`.
- Client: `apexsim/game-unreal/Source/ApexSim/{Public,Private}/Track/`; editor-only
  commandlets in `ApexTrackEditor/` (`ApexMaterialBake`,
  `ApexGroundTexImport`, `ApexTrackImport`, `ApexTrackCatalogSync`).

All `ats-*` tools take `--all` or YAML paths and resolve
`tracks/{default,custom}` against the working directory: **run them
from the content root** (`cargo run --manifest-path track-editor/Cargo.toml
--bin ats-... -- ...`). Every tool skips a track whose `.ats` carries
`"imported": "ac"` ([ac-import.md](ac-import.md)).

## Refresh order

Everything downstream is derived from the centerline, and the dossier and
the DEM sidecar are both fitted to it. A change to the centerline has to flow
through in this order; a step run out of order produces data that disagrees
with itself (a pit lane laid along a road that has moved, a DEM offset from
the road it sits under):

```bash
python tools/dem_elevation.py <Stem>                                        # node z from the checked-in DEM
cargo run --manifest-path track-editor/Cargo.toml --bin ats-smooth -- --all   # centerline
cargo run --manifest-path track-editor/Cargo.toml --bin ats-bank -- --all     # banking onto its bends
python tools/drs_zones.py --all                                             # DRS zones onto the corners
python tools/osm_layout.py --all --offline                                  # dossiers, refitted to the centerline
python tools/dem_fetch.py --all --offline                                   # DEM sidecar (same fit, same datum)
python tools/track_location.py --all                                        # altitude and position from the DEM
./bake.ps1                                                                  # dress, export, previews
```

The first seven are data refreshes with checked-in outputs. The last is the
everyday step: `ats-dress` (idempotent, so always), `ats-export` and
`build_track_catalog.py` (`-Stage dress,export,previews`, the default, plus
`ground` for the ground PNGs when one is missing; `-Track A,B`, `-Release`,
`-DryRun`). From the code root, `scripts/build_track_levels.ps1` runs the
same bake and then the code's stage, the missing track materials (the
showcases a changed track touches are rendered again by the game and the
server themselves, [apexsim/docs/game/spectator.md](../../apexsim/docs/game/spectator.md)).
Switches: `-Track A,B`, `-SkipDress` (hand-edited scene), `-SkipExport`,
`-SkipPreviews`, `-SkipMaterials` (no engine), `-Build`,
`-ImportProps`, `-ImportLevels` (editor-only levels under `/Game/Tracks`),
`-Release`, `-DryRun`. After pulling a changed track, re-run `ats-export`
(sidecars are gitignored) and restart the server.

## Who owns what

Each prop in a `.ats` belongs to exactly one pass, which deletes and re-lays
what it owns on every run. Ownership is by **kind** for whole kinds and by
**asset** where a kind is shared with hand-placed props.

| Pass | Owns |
|---|---|
| `ats-dress` (`dress.rs`) | Kinds grandstand, building, attraction, bridge, light, vehicle, sky; the pit lane (`authored: true`); landmark assets (`dress::LANDMARK_ASSETS`: `misc/bull_statue`, `misc/toll_gate_lane`, `sign/hillside_letters`); the surroundings assets (`dress::surroundings::OWNED`: car-park cars and lamps, tents and campers, houses and barns, chapels, pylons, marshal posts (numbered ones too), the marshals' `light_panel`s (one beside each post, one in any 250 m race-control sector without a post), `corner_sign`, fallback `floodlight_tower`s, forest impostors, the fan dressing's banks and camp pieces, and the track edge's `kerb_marker` at each curb's ends and `sausage_kerb_2m` behind the inside curb of a bend under 80 m radius, `lay_kerb_pieces`); furniture runs (`lay_furniture`); road decals from the dossier |
| `ats-groom` (`groom.rs`) | Barriers, tyre walls, Tecpro, fences and their end caps (by asset); hoardings (`HOARDING_ASSETS`); braking and distance boards; tree belts, near trees and ground scatter; German signs on the Nordschleife (`GERMAN_SIGN_ASSETS`); the graffiti tags on the rails (`RAIL_TAG_ASSETS`) |
| `ats-export` | Nothing in the `.ats`: it generates the pit complex (garages, box kits, pit walls, pit signs, the podium on the middle garage's roof), road paint, DRS lines and boards, terrain and horizon in the export only |

`ats-dress` grooms after dressing (`--no-groom` to skip); `ats-groom` run
alone loads the dossier itself (`groom_scene_with`). Grooming re-seats dressed
props but never pushes them. Do not hand edit what a dossier field can
express: the next run throws it away.

## Layout dossiers (`osm_layout.py`)

`tracks/default/<Stem>/<Stem>.layout.json`, checked in, built from
OpenStreetMap (ODbL; the attribution travels in the file):

```bash
python tools/osm_layout.py --all          # or: Monza Spa [--offline] [--dry-run]
python tools/osm_layout.py --all --names-only   # re-apply CORNER_DISPLAY / display_name only
python tools/osm_layout.py <Stem> --manual-only # re-apply the MANUAL_* layers only (no extract)
```

**Fit.** The OSM extract is georeferenced onto the track frame by an FFT
rotation/translation search and trimmed ICP. The script refuses to write a
dossier unless the fit explains at least 90% of the centerline, or (public
road circuits such as Le Mans) at least 1.5 km at 3 m rmse or better; `fit`
in the file records rmse and coverage. "Centerline covered" is the gate;
"raceway matched" (the reverse) is low wherever the bbox holds other layouts
and does not matter. Raw extracts cache, gitignored, under `.cache/osm/`; a
bbox over the map API's 50k-node limit is split into tiles in `BBOXES`
(overlapping tiles are deduplicated by way id).

**Layers.** Named corners (`name` from OSM, `display_name` from
`CORNER_DISPLAY`, see [naming.md](naming.md)), the pit lane's polyline and
side, grandstands (name, side, length, depth, roof, road-facing front),
structures, crossings over the road, landmarks, woods with leaf type, and
the surroundings: `barriers` (tyres, wall, guard rail, fence near the road),
`roads`, `waterways`, `areas` (car parks, camp sites, meadow, farmland,
villages, scrub, water), `poi`. Anything tagged within 250 m that no rule
claims is counted in `unclassified`, so a circuit's gaps show in the file.

**Manual tables** in the script, keyed by stem, hold what OSM lacks and
survive a refetch; each entry's comment names its source. `MANUAL_STANDS`
(`name`, `from_m`/`to_m`, `side` as `left`/`right` driving the lap or
`outside`/`inside` of the bend, `depth_m`, optional `gap_m`, `covered`; the
front is laid along the centerline). `STAND_COVER` gives a stand its roof
by name from the seating map, OSM and manual stands alike: `covered` or
`open`, written as the stand's `roof`, which the dress pass takes over its
`covered` and the depth rule. `MANUAL_CROSSINGS` (`station_m`, or a
map anchor `at` / `from_at` + `to_at` (lon, lat); `kind`
`footbridge`/`link`/`viaduct`/`gantry`/`timing`, `overpass` or `road`
(a concrete bridge), `building` (a building spanning the road,
`span_building`), `sky_platform` (platforms cantilevered over the straight),
`banked_oval` (the banked oval on piers), road-carrying
`deck_arch`/`deck_wide` with `from_m`/`to_m` (ground left out of the span),
the lap's own bridge over a road `deck_track` (parapets, ground kept), else
(`arch`) a tyre bridge; `brand` from the kit's fictional brands;
`CROSSING_KINDS` renames the scan's kinds per circuit).
`MANUAL_LANDMARKS` (`kind` must be one `dress.rs` maps to an asset, see its
landmark table: besides the wheel, screens, stage, towers and sky props,
`observation_tower`, `glass_tower`, `media_wing`, `arena`,
`expo_pavilion`, `geodesic_dome`, `fuel_station` and `cauldron`; an unknown
kind is dropped silently; `tourism=artwork` becomes a `statue` only where
declared here). `MANUAL_STRUCTURE_ASSETS` names the kit building an OSM
outline stands as, laid once on its centre (an `arena_dome` for an ice
hall). `LANDMARK_KINDS` renames the scan's landmark kinds per circuit
(Austin's tower is an `observation_tower`), on a full extract and on
`--manual-only` alike; `DROP_SCANNED_LANDMARKS` leaves a scanned landmark
out by kind and station (a phone mast taken for a tower). An authored
landmark whose centre lies on an OSM outline (within 5 m) stands for that
structure, and its row is dropped. `MANUAL_PIT_LANE` (wins over
OSM outright), `PIT_LANE_REACH_M` (how far from the track a mapped pit lane may run before it is clipped, 70 m by default; Silverstone's cuts 102 m across the inside of Club), `MANUAL_STRUCTURES`, `MANUAL_STRUCTURE_ASSETS`,
`MANUAL_WOODS`, `MANUAL_FURNITURE`, `MANUAL_DECAL_RUNS`, `MANUAL_GRAFFITI`.

Prefer a manual entry to a code change. A change to `extract()` or the fit
must leave the other dossiers unchanged (re-run `--all --offline` and diff).

**Tracing against imagery** (`tools/trace_layout.py <Stem> [--open]`,
`--all`): writes `build/trace/<Stem>.html`, a page that draws the
centerline with a station mark every 100 m, the named corners and the
dossier (stands amber, manual ones orange, buildings blue, pit lane
magenta, crossings, landmarks, woods, water, barriers; each a layer) over
aerial imagery (Esri World Imagery, OpenStreetMap as the other base), through
the DEM sidecar's georeference, which is the dossier's own fit; a circuit
without a sidecar (Marina Bay) runs that fit itself (`osm_layout.fit_track`
over the cached extract, the same numbers the sidecar would record). Clicking traces a run of points: the panel
gives each as station, side and distance off the road edge, track `x`/`y`
and `lon`/`lat`, and writes the run out as the manual entry to paste: one
point as a `MANUAL_LANDMARKS` / `MANUAL_CROSSINGS` anchor, two or more as
a `MANUAL_STANDS` span (`from_m`, `to_m`, the side most points fall on,
`gap_m` from the nearest) and, from three, as a ring in both frames. GO TO
jumps to a station, which is how an `ats-dress --verbose` skip or a
`--explain` line is checked against the ground. The page only reads the
dossier: what is traced goes into the tables and `--manual-only` (or a
refetch) lays it.

**Without the extract** (`--manual-only`): the hand-authored layers (the
`MANUAL_STANDS` stands and the `STAND_COVER` roofs, the authored landmarks
and crossings, graffiti, decal runs and skid marks, furniture) are rebuilt
from the tables through the georeference the DEM
sidecar records, which is the dossier's own fit; the surveyed layers are
left byte for byte, and so is a manual stand whose table entry has not
changed (its front within 0.5 m: the centerline moves by centimetres when
it is smoothed after the dossier). A circuit with no DEM sidecar is
skipped. `python tools/dossier_check.py <Stem>` re-applies one circuit's
tables this way and shows what the dress pass lays differently, a
`STAND_COVER` key that names no stand, and any structure the dossier lost
(an authored landmark drops the OSM structures within its reach). With no table
changed, `--all --manual-only --dry-run` reports every dossier unchanged.

## Dressing (`ats-dress`)

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- --all
#   or <yaml> [--dry-run] [--verbose] [--no-groom]
```

The report gives placed counts; `--verbose` lists each dossier entry it could
not place, with the reason, and which entries lost props to the water, a
city block or the pit zone. Every prop the pass lays carries a **source
stamp** (`Prop::source` in the `.ats`: `stand Tribune F1`, `building Pit
building`, `bridge ...`, `landmark ...`, `furniture <asset>`, `street
lamps`, `surroundings: <sub-layer>` (`car parks`, `camp sites`,
`spectator banks`, `village`, `points of interest`, `forest`, `marshal
posts`, `corner signs`, `floodlights`; a named car park, camp site or
village as `surroundings: car park P1`), `kerb pieces`; the groom pass stamps `groom
barrier` / `wall` / `board` / `hoarding` / `rail tag` / `tree`, adopted props
included). `--explain` prints the scene by stamp and asset after the
counts, with the skips, and what no pass owns (hand-placed, or laid before
the stamps): a scene dressed before them gains its stamps on the next run,
and nothing else changes in it. A prop placed by hand in the editor has no
stamp.

- **Stands** are laid as 40 m runs along their real front, each with its own
  `length_m`, so the client's bays follow the outline; depth 13 m or more is
  the large family, 25 m or more roofed unless its `roof` says `open`,
  under 12 m dropped. A run over the garage row is left to the pit
  complex (the terrace the bake builds); a run in the pit lane or its
  apron anywhere else (behind the entry or exit road) is stepped back out
  of it. The pit zone takes the garage row from the export's own layout
  (`ue_export::garage_row_span`): the garages' depth along the row and
  12 m past its ends, the lane and its apron along the rest.
- **Footprints** of stands and buildings are probed whole against every leg
  (`clear_footprint`) and stepped back up to 25 m or left out.
- **A building block on a paved apron** (a unit of a venue row whose
  footprint reaches into an `asphalt_runoff` or `concrete` band of the
  scene's surfaces, `runoff_reach`) is left out, the rest of the row kept;
  a whole building that stands on one is skipped ("stands on the run-off").
  Gravel and sand are not counted: their wedges run out to the barriers.
- **Pit lane** (`build_pit_lane`): the dossier polyline resampled every 4 m
  and smoothed, read against its own leg (a point the window finds over
  `PIT_LEG_LOST_M` off is looked for again where the lane's progress from
  entry to exit puts it), pushed out to a pit wall's apron
  where mapped too close, blended onto the road edge at both ends, bends under
  30 m rounded (`round_tight_bends`), at least 24 boxes (`PIT_GRID_BOXES`),
  seated on its own leg's road edge (`nearest_cross_section_near`), and off
  the road any stretch steeper than 8% eased into a ramp
  (`ease_steep_grades`): a lane cutting from one leg's side to another's
  (Spa's exit past La Source) otherwise steps between their heights. `PitZone`
  keeps everything dressed off the lane, its apron and the garage depth. The
  server side is in [apexsim/docs/server/pit-lane.md](../../apexsim/docs/server/pit-lane.md).
- **Surroundings** (`dress::surroundings`): car-park cars and lamps, camp-site
  tents (pitched over the area on the R2 sequence, `r2_point`), village houses, chapels and pylons, a marshal post at every named
  corner and every 400 m, a `corner_sign` from each corner's `display_name`,
  forest impostors on far slopes, and `floodlight_tower`s only when the
  dossier maps no lighting masts.
- Anything standing in mapped water that no bridge carries is dropped.

## Grooming (`ats-groom`) and barriers

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-groom -- --all [--dry-run] [--verbose]
```

**Barrier kind** is decided per 4 m cell (`barriers::decide`): mapped dossier
barriers first, then geometry (outside of the bend, how tight, how much
run-off). Mostly armco; Tecpro on the outside of fast bends; tyres;
`armco_4m_fence` where people stand behind; `concrete_4m_rail` where a mapped
wall stands hard against the road; `armco_end` / `tecpro_corner` /
`tires_corner` / `concrete_end` closing each run. A corner's barrier stands at
least 14 m past the road edge, a straight's 7 m
(`CircuitStyle::corner_barrier_min_m` / `straight_barrier_min_m`), further
where run-off is authored.

**The line** (`groom::lay_all_barriers`) is a polyline: the wanted offset is
smoothed along the lap at `BARRIER_GRADE` (0.25 m/m), held mid-strip between
two close legs and eroded toward a stand's front; sampled every metre; points
dropped where not placeable (on the pit lane except behind it, on another
leg's verge, in an underpass slot, inside a stand or building); then modules
laid end to end by arc length, yawed to the line, capped a metre past each
run's end and where two kinds meet. Stand slabs are modelled from the pivot on
the *front* (`groom::footprint_centre_of`, `Slab`), as every other stage does.

**Hoardings** (`groom::lay_hoardings`): `board/hoarding_3m` behind plain rail
on the pit straight, through braking zones, round corners' outsides and in
front of stands, a brand per 36 m (15 m on a street circuit), never on Tecpro
or tyres. **Trees** go only inside the dossier's woods, species from the
leaf type and the circuit style's flora.

`python tools/check_walls.py --openings --all` (`-v` lists runs) probes
every 2 m of the lap for a ray from the road edge that meets no wall within
45 m; without `--openings` it flags wall segments standing on the road.

## The centerline

```bash
python tools/dem_elevation.py --report | <Stem> [--dry-run] [--wavelength M] [--canopy-tol M]
cargo run --manifest-path track-editor/Cargo.toml --bin ats-smooth -- --all
#   --report | --dry-run | --tolerance M | --tight-tol M | --min-radius M | --passes N | --window N
cargo run --manifest-path track-editor/Cargo.toml --release --bin ats-bank -- <yaml> | --all [--dry-run]
```

**Elevation** (`dem_elevation.py`). Most YAMLs' z came from invented
keyframes, and near the road the centerline wins over the DEM, so physics and
the AI drive whatever z says. The script re-derives node z from the DEM
sidecar: the median across the road at each node, a Whittaker fit along the
lap (250 m half-power wavelength) reweighted so samples 1.5 m above it count
as canopy, node 0 kept exactly (the sidecar's datum), the raceline carried by
the road's change. Every circuit with a DEM sidecar is re-derived except
Shanghai (its 11 m of relief on a flat site is grandstands the surface model
reads as ground), Monza (the park's canopy: 140 samples read as trees, and
the fit moved the racecraft, tyre-window and mesh-agreement tests). On a
wooded circuit the model reads trees over whole stretches: look at the fit
before trusting `--report`. **A circuit that crosses itself** (Suzuka):
the DEM sees only the bridge deck over both roads, so `crossovers` finds
each place the lap comes within `CROSS_XY_M` (6 m) of itself at least
`CROSS_APART_M` (200 m) along, the YAML says which road is lower, and the
lower road's samples within `CROSS_MASK_M` (80 m) of the crossing are left
out of the fit, which bridges them from the ground either side; with less
than `MIN_CLEARANCE_M` (7 m) left road to road the lower road is lowered to
it, tapered over `CLEARANCE_TAPER_M` (250 m). At Suzuka the fit alone left
3.1 m (the invented profile had 12.3 m); the AI survey on four seeds was
unchanged on the centerline backend within its noise and better on the
road mesh (off-road 162 to 117 s, sliding 36 to 23 s).

**Smoothing** (`ats-smooth`, `track_smooth.rs`). GPS traces at 5 m nodes turn
noise into curvature. Node positions are filtered by a Savitzky-Golay
quadratic applied by twicing, and pinched corners are re-walked: capping the
turn at a node and spilling the excess to its neighbours floors the radius
(`--min-radius`, 22 m) without changing the corner's total turn, each window
pinned at both ends. The raceline moves with the road node beneath it rather
than being filtered on its own (filtered alone it drifted off the road).
`metadata.length_m` is rewritten. The half-widths are filtered the same
way, each side on its own, within `DEFAULT_WIDTH_TOLERANCE_M` (0.3 m) of
the trace: a traced centerline wanders inside the road, one side widening
as the other narrows from node to node, and since a width is linear between
nodes every change in its slope is a corner in the road edge and its curb
(7 degrees from a 0.3 m wobble). `--widths-only` filters the widths alone,
leaving the line, its elevation and the raceline (and so everything fitted
to them) as they are. Laplacian and Taubin smoothing rescale a
surveyed circuit and were rejected; the tests check shape. The exporter's
corner guards (`clamp_profile`, strip breaking, dropping folded facets) remain
as a backstop.

**Banking** (`ats-bank`, `track_bank.rs`). Positive banking lifts the left
edge in the road mesh, the server's surface elevation and the gravity pull
alike, so a right-hander banks positive. Each banked span keeps its angle and
is re-laid over the bend it belongs to (most overlapping turn, else nearest
within 250 m), signed by the bend's hand, held over the bend's core
(curvature at least 35% of peak) with smoothstep ramps of 30 m or 4 m per
degree, whichever is longer (72 m for 18°), so the edges twist in
gradually; banking near no bend is dropped. Every circuit is re-laid. Spa's
invented 18° bank and Monza's 10° were removed rather than re-laid (they
would have landed on La Source and the Parabolica, both flat).

## Elevation model and horizon (`dem_fetch.py`)

`python tools/dem_fetch.py --all | <Stem> [--offline] [--dry-run]` writes
`<Stem>.dem.msgpack`, checked in (regenerating needs hundreds of MB; tiles
cache under `.cache/dem/`): Copernicus GLO-30 from the AWS open bucket,
georeferenced by the dossier's fit (`osm_layout.fit_track`), an `inner` 10 m
grid over the circuit plus 1 km and an `outer` 90 m grid to 8 km, in the
server frame, offset to the YAML's z at the start line. `terrain.rs`
(`TerrainHeightfield::from_paths_with_dem`) crossfades: within 40 m of a road
the centerline wins (a surface model reads canopy and knows no cuttings), past
220 m the model wins. `ue_export` bakes the `outer` grid as a `horizon` mesh
(real geometry, lit and fogged) with a hole under the detailed ground, and
a skirt past its edge (`horizon_skirt`): a spoke from every eighth edge
post out 30 km, held at that post's height and falling with the earth's
curvature, so the land runs on into the haze from a crest instead of
ending in a band of sky.
`track_location.py` writes `metadata.altitude_m` / `latitude_deg` /
`longitude_deg` from the georeference (its `MANUAL` table covers circuits
without a DEM); `ats-export` writes `latitude_deg`, `longitude_deg` and
`north_yaw_deg` into the export, for the sun and the menu's globe. GLO-30
is not square-posted: north of 50° a tile is 2400 posts wide, not 3600,
and assuming square posts reads the ground kilometres east of where it is.

## DRS zones (`drs_zones.py`)

`drs_zones` in each YAML (detection, activation, end stations) come from the
FIA notes of the circuit's last DRS season, in the script's `ZONES` table;
each entry names its turn by an approximate station and is snapped to the
corner detected from the centerline, so zones survive `ats-smooth`
(`--report <Stem>` prints the corners to author against). The bake paints a
line at each detection and activation station with `DRS DETECTION` / `DRS`
boards. The rule is in [apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md).

## Run-off and track limits

An `asphalt_runoff` or `concrete` band beside a curb is tarmac run-off. The
curb sidecar (version 3) carries per metre how far curbs and run-off reach
past each edge (`CurbBands::runoff_at`), each gravel and sand trap's inner
and outer edge from the road edge (`gravel_*_cm`, `sand_*_cm`,
`CurbBands::trap_at`), and the ground past the bands (`ground`: `desert`
on a `Ground::Sand` circuit, empty for grass), so the centerline backend
drives a trap as the road mesh does (`OffGround`,
[apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md)). A wheel within the curb band is on
track; past it but on run-off it is `RoadContact::Runoff` (asphalt grip times
`RUNOFF_GRIP_FACTOR`, no grass drag) but off the track for the lap, judged per
wheel. A band's `paint` style is baked as stripes; `ats-dress` paints corner
run-off by `dress::RUNOFF_PAINT` (Spa, Yas Marina, Bahrain).

## The bake (`ats-export`)

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- --all
#   or <yaml>; [--out DIR] [--keep-sidecars ground,curbs,walls,road,pit|all] [--flat-curbs] [--strict]
```

Writes the client export to `build/tracks/` and five server sidecars beside
the YAML, deterministically; sidecars listed in `external_sidecars` are kept.
The road bake drops a facet whose footprint in the ground plane is under
`road_mesh::MIN_FOOTPRINT_M2` and reports the count; a vertical face (a
curb's outer face, no footprint at all) is meant, a sliver (a hair of
footprint: a cross-section that folded or crossed its neighbour) is not.
`--strict` names each sliver by surface and station span and fails the
track on it (Zandvoort passes it with its 6159 vertical curb faces), so a
bake that fails it has folded a strip.

### Server sidecars

Gitignored; loaded by `TrackLoader::load_sidecars` when the first session on
the track is created; each optional (a missing one is logged).

| Sidecar | What | Without it |
|---|---|---|
| `<Stem>.ground.msgpack` | 4 m heightfield of the rendered ground, server frame (`ground.rs`) | Off-track height falls back to the centerline |
| `<Stem>.curbs.msgpack` | Curb and run-off reach, gravel and sand traps per metre, the off-track ground (`curbs.rs`) | The road edge is the track limit; curbs and traps read as grass |
| `<Stem>.walls.msgpack` | Every barrier, fence and pit wall as segments, every stand, building and garage as its footprint's sides, underpass walls and parapets; base height, height, material (armco, tyres, concrete). Runs with ends under 4.5 m apart are joined (`walls.rs`, `physics::check_wall_collisions`) | Nothing stops a car off the road: barrier collision exists only on the server |
| `<Stem>.road.msgpack` | The road as tagged triangles: see [road-mesh.md](road-mesh.md) | The centerline backend |
| `<Stem>.pit.msgpack` | Lane centerline, limit lines, box spots, entry and exit stations (`apexsim_formats::pit::PitSidecar`) | No pit stops |

### Terrain and the verge

`terrain.rs` answers "how high is the ground here" once
(`ground_height_at`), and everything that touches the ground samples it: the
ground mesh, the bands, curb outer faces, prop seating, the ground sidecar.
Within 6 m of any road edge (track or pit lane) it is the verge, the road
edge less 0.08 m (`VERGE_DROP_M`); it blends into the field by 35 m
(`BLEND_END_M`). The field is the DEM crossfade above or, without one, an
inverse-distance spread of the centerline's heights, capped by a road
ceiling so no hill buries a road.

- **Nearest road owns its verge** (`DOMINANCE_M`): where two roads run close
  at different heights, a road's pull and ceiling fade out 2 m past the
  nearest road's edge. Without it, a lower road's ceiling cut the ground under
  a higher road's banked edge (Zandvoort's pit exit under the Hugenholtz).
  `the_verge_meets_the_road_edge_on_every_real_circuit` checks every lap edge.
- **Terrain grid** (`Bake::ground`): 2 m within 60 m of a centerline, 6 m to
  200 m, 12 m beyond. The near radius has to clear the verge blend or the
  resolutions crack.
- **Band reach** (`ue_export::band_reach`, `band_reach_profile`): a ground
  band (grass apron, run-off, gravel) stops halfway between its own road edge
  and any other road or the pit lane, and its reach may change by at most
  0.5 m per metre of course, since a band's columns are fractions of its
  width and a jumping width strings quads across the road. Within 200 m of an
  underpass a band keeps its full width. Bands are cut into columns (2 m to
  12 m out, 4 m to 40 m, then 10 m) so they follow the verge.

### Underpasses

Where the course passes over itself with at least 4 m to spare (Suzuka) the
terrain finds an `Underpass`: behind wall lines 3.5 m past the lower road's
edges the ground is the upper road's embankment, so the lower road runs in a
slot. The bake cuts the ground grid along the walls and draws the slot floor,
abutment walls and the deck (parapets, fascia, material family `structure`);
`ats-groom` lays no armco there. The ground sidecar reports the lower level
under the bridge. A heightfield cannot hold both levels, so a car that leaves
the bridge past the parapet falls into the slot. Water and road bridges over
it (Marina Bay) are in [circuits.md](circuits.md).

### Road paint and the pit complex

Paint is geometry, one `marking_*` key per colour, each layer on its own
lift: edge lines (broken where the pit tapers meet the road), the
start/finish chequer, grid boxes laid in station space so back rows bend,
pit lines, the curb strips, run-off stripes and DRS lines. There is no
centre line and no baked rubber line (the server simulates rubber). From the
pit lane, `ue_export::pit_layout` decides once where the limit holds and
where the garages stand (the longest stretch with `PIT_GARAGE_ROOM_M` behind
the lane, clear of every road), shared by the sidecar, the garages and the
paint, so a car stops where it sees its box: `pit/garage_6m` per box with a
`box_kit`, `garage_end`s, pit walls facing the track (and along the tapers
where the apron is at least `PIT_TAPER_WALL_MIN_M`), speed-limit and exit
signs. `bake_walls` puts each garage's collision box behind its door.

## Ground textures (`ApexGroundTexImport`)

The track surfaces sample tiling maps: seven sets (asphalt, grass, gravel,
sand, concrete, astroturf, kerb), each a 1024² colour, normal and roughness
map tileable over 2 m. The PNGs are checked in under
`textures/ground/` and are deterministic; the generators are numpy in
`tools/generators/props/apex_tex.py`.

```bash
python tools/bake_ground_textures.py [asphalt grass ...] [--size 512]
UnrealEditor-Cmd.exe ../apexsim/game-unreal/ApexSim.uproject -run=ApexGroundTexImport   # -> /Game/Ground/T_ground_<set>_{col,nrm,rough}
```

The import re-bakes `M_ApexTrackBase` when it finds the sets newly imported.
It fixes each map's class by suffix (sRGB colour, BC5 normal, BC4 roughness
as `TC_Alpha`; `TC_Grayscale` cooks to uncompressed G8), because a material
instance can only swap a texture for one of the sampler type the parent was
compiled with. Colour maps are normalised to a mean of 0.5 and doubled in the
material, so the exporter's per-key colour still decides the hue.
`apex_tex.GROUND_TILE_M` and `ApexGround::TextureTileM` must agree.

`M_ApexTrackBase` (`ApexTrackMaterialGraphs`) has two shapes: with
`/Game/Ground` imported, each map is sampled at two tilings mixed by a 20 m
world noise and faded flat by 200 m; without it, a noise-tile grain (a fresh
clone). The runtime tells which by whether the base exposes `TextureAmount`.
`ApexGround::LookFor` / `LookForSet` decide which set a key samples.

**Band seams.** The builder writes a ramp into each ground band's red vertex
channel, 0 at the road-facing edge to 1 by 2.5 m in (`ApexGround::EdgeFactors`),
which the material frays toward dust. One-sided on purpose (fringing the outer
edge drew a dust ring at the terrain), and the edge is found against the
centerline because the exporter merges both sides' bands per section. The
import logs the fringe's share of band vertices (about 8%); zero or all means
the edge test broke.

## Runtime tracks on the client

The game builds every circuit from its export; `/Game/Tracks` is in
`DirectoriesToNeverCook`. Re-export a circuit, then restart the game or run
`apexsim.track.Rescan`.

**Where tracks are found** (`UApexTrackContentSubsystem::TrackDirectories`,
first match per stem wins): `-ApexTracksDir=<dir>[+<dir>]`; in a package
`Tracks/` beside `ApexSim.exe` (preview as `<Stem>.png`); in the editor the
content root's `build/tracks` (previews under `previews/`). An installed game takes a
new circuit as its `.uescene.json`, `.uemesh` and `.png` in `Game/Tracks`
plus its YAML folder (with sidecars) under `Server/content/tracks/custom`.

**Catalog.** `Rescan` turns each manifest's head (`LoadHeader`, first 64 KB)
into an `FApexTrackCatalogRow` keyed by `track_id`: name (display name
preferred), metadata, `SourceCrc`, location, and `RuntimePreview` from the
PNG (`PreviewOf`). `UApexMenuFlowSubsystem::FindTrackRow` returns it before
any `DT_TrackCatalog` row, which is only a fallback for a track with no export
(`ApexTrackCatalogSync` fills it from `build/tracks/track_catalog.json`; the
build does not run it). The content-checksum check is in
[apexsim/docs/architecture.md](../../apexsim/docs/architecture.md).

**Loading** (`UApexTrackInstance`, handed out by `Acquire` / `Release`):
manifest, blob and mesh descriptions (tangents, band fringe ramp:
`FApexTrackSceneBuilder::PrepareGeometry`) on a worker thread; then a
`UMaterialInstanceDynamic` per key (`MI_<key>`) from the cooked parents; then
fast-path `BuildFromMeshDescriptions` within `apexsim.track.BuildBudgetMs`
(20) ms a frame; then the actors in one frame into the menu world. The track
counts as loaded once every surface's collision has cooked. The builder sits
behind `IApexTrackAssetFactory`: the game's `FApexRuntimeTrackFactory` makes
transient objects, the editor's `FApexTrackAssetBuilder` saves packages for
`-ImportLevels`, so the editor shows what the game builds.

**Collision.** A mesh built in a cooked game cannot cook its own, so each
surface (not decals) carries a `UApexTrackCollisionComponent` (own body
setup, triangles via `IInterface_CollisionDataProvider`, cooked async by
Chaos). Only traces use it: the racing line's snap and the cameras. Cars
collide with nothing on the client.

**Tags.** Track surfaces carry `ApexTrackMesh` (the racing line's
`IsRoadSurface` requires it and not `ApexProp`), props `ApexProp`, the gantry
`ApexStartLights`, its lenses `ApexStartLight`. The director's conditions
pass walks `UApexTrackInstance::GetActors`.

**Reuse and failure.** Hiding a track turns its collision off. The last
released track is kept hidden and handed back, materials reset, when the
same circuit is asked for next (`apexsim.track.KeepLast 0` turns it off). A
failed build is handed back (`DropFailedTrack`) and the circuit marked broken
until the next rescan.

**Materials** are the only cooked track content:
`/Game/Materials/Track/{M_ApexTrackBase,M_ApexTrackRoad,M_ApexEmissive,M_ApexBrand,M_ApexDecal,M_ApexCloudLayer,MPC_ApexSky}`
(`M_ApexTrackRoad` is the base plus the road state, for the `road` family
only; `M_ApexCloudLayer` the cloud dome; `MPC_ApexSky` the collection the
prop materials read the snow from),
by `-run=ApexMaterialBake [-force]`, which also puts the snow blend on the
prop parents under `/Game/Props/_Parents`
([apexsim/docs/server/conditions.md](../../apexsim/docs/server/conditions.md)). Without them a track
draws in flat colours and the log says so.

## Adding a new circuit

1. **Centerline.** A YAML under `tracks/default/<Stem>/<Stem>.yaml`
   from a CSV trace (`convert_track`, [track-format.md](track-format.md)), an
   OSM route (`tools/osm_centerline.py <Stem>`, waypoints in its `LAPS`),
   or an AC import for a custom track ([ac-import.md](ac-import.md)). Give it
   a fixed `track_id`, `name`, `display_name` and a place-based
   `metadata.description` ([naming.md](naming.md)).
2. **Scene seed.** `python tools/seed_scene.py <Stem> [--from survey.json]`
   writes a starting `.ats` (start line, grass, curbs at every apex).
3. **Dossier.** Add a `BBOXES` entry in `osm_layout.py`; run
   `osm_layout.py <Stem> --dry-run`, then for real. Review on
   `trace_layout.py <Stem> --open` (after step 4's DEM): pit lane side and
   entry against the imagery, stands against the published seating map
   (`MANUAL_STANDS`, traced on the page), crossings, landmarks, woods. Add
   `CORNER_DISPLAY` entries for new corner names.
4. **Data.** `dem_fetch.py <Stem>`, then the refresh order from
   `dem_elevation.py` down (smooth, bank, DRS zones in `drs_zones.py`'s
   `ZONES`, dossier refit, DEM refit, `track_location.py`; a circuit with no
   DEM goes in its `MANUAL`). No `raceline`? `python tools/generate_race_line.py
   --track <yaml>`.
5. **Build.** `./bake.ps1 -Track <Stem>`. Read the
   `ats-dress --explain` output (what each entry became, what was skipped
   and why) and check a doubtful one on the trace page's GO TO; run dress
   twice and confirm the `.ats` does not change; `ats-export --strict
   <yaml>` for a folded strip.
6. **Check.** `cargo test` in `track-editor`; `check_walls.py <Stem>` and
   `--openings`; the AI survey (`SURVEY_TRACKS=<Stem>`, below) and the pit
   survey (`PIT_TRACKS=<Stem>`, [apexsim/docs/server/pit-lane.md](../../apexsim/docs/server/pit-lane.md));
   look at it in the game (`-ApexAutoRace -ApexTrack=<Stem>
   -ApexScreenshotAfter=25 [-ApexCameraLookAt=...]`): pit building on the pit
   side, stands facing the road, nothing on the tarmac.
7. **Optional.** Guide notes (`<Stem>.guide.yml`, [track-guide.md](track-guide.md)),
   a showcase entry in `apexsim/showcase.yml`.

Commit the YAML, `.ats`, dossier, DEM sidecar and script table changes.

## Checking it

- `cargo test` in `track-editor` (unit tests plus `core/tests/`, which groom
  every real circuit): run all of it, not `--lib`.
- The whole-calendar bakes (`every_real_track_bakes`,
  `real_tracks_bake_plausible_curb_bands`,
  `every_real_track_road_mesh_covers_the_road`) take about twenty minutes and
  are `#[ignore]`d: `cargo test -p track-core --test ue_export -- --ignored`
  before shipping a bake change. Ask before running them.
- Anything that moves barriers, walls, the centerline or the ground goes
  through the AI survey against a baseline ([apexsim/docs/server/ai.md](../../apexsim/docs/server/ai.md)),
  in `../apexsim/server/sim`: `cargo test --release --test all ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored --nocapture`
  (`SURVEY_TRACKS=A,B`, `SURVEY_DBG=1`).
- Client: `ApexSim.Track.*` (reader, builder, ground) and `ApexSim.Props.*`.

## Traps

- A step run out of the refresh order leaves the dossier, DEM, pit lane and
  road describing different roads.
- Ownership is by asset for shared kinds: a pass must recognise every asset it
  can lay, or it lays them twice on the next run (pinned by tests).
- A barrier rule without the outside-of-bend signal puts Tecpro on both sides
  of every bend; `wall_offset`'s 6 m verge fallback at every corner pins the AI
  against rails. Run the AI survey after moving walls.
- A hand-authored `MANUAL_PIT_LANE` wins over OSM; an OSM untagged-way
  fallback once matched a "pit lane" across the race track and the bake stood
  pit walls on the road.
- `Track.locate` in `osm_layout.py` is nearest-point: where two legs run close
  (Suzuka's crossover, IMS's infield) a feature can attach to the wrong leg
  with wrong station and side.
- Grooming re-seats props on the ground; a raised dressed prop needs an entry
  in `dress::fixed_lift_m`.
- Runtime-built meshes have no mesh distance fields (neither the fast nor
  the full `BuildFromMeshDescriptions` makes them, and a cooked game cannot),
  so software Lumen and DF shadows see the road and terrain less well than
  kit props. `ShowFlag.VisualizeMeshDistanceFields 1` (from `-ApexExecAfter`)
  shows what has one.
