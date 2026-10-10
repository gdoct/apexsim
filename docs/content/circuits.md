# Circuits with special handling

Most circuits go through the general pipeline in
[track-pipeline.md](track-pipeline.md) unchanged: a GPS-traced centerline, an
OSM dossier, the default grooming rules. This doc covers what differs per
circuit: the circuit styles that change the generated furniture, the road
decal layer, and the two circuits built differently from the rest, the
Nordschleife and Mandarina Bay. Display names are in
[naming.md](naming.md).

## Code

- `track-editor/core/src/circuit_style.rs`: `CircuitStyle` and its variants,
  picked by stem (`CircuitStyle::for_stem`, `for_scene`).
- `track-editor/core/src/groom.rs`: barriers, hoardings, trees, German signs,
  reading the style.
- `track-editor/core/src/dress.rs`: dossier furniture, stands, landmarks,
  crossings, street lamps, decals, `RUNOFF_PAINT`.
- `track-editor/core/src/ue_export.rs`: ground restyling, water, bridge spans,
  the street pit building.
- `track-editor/core/src/terrain.rs`: water under the terrain.
- `scripts/osm_centerline.py`: centerlines routed over OSM (`LAPS`), for
  circuits without a GPS trace.
- `scripts/osm_layout.py`: the per-circuit `MANUAL_*` tables.
- `scripts/seed_scene.py`: starts a new circuit's `.ats` (start line, grass,
  curbs at every apex).
- `scripts/bridge_elevation.py`: bridge humps on a flat street circuit.

## Circuit styles

A `CircuitStyle` carries the groomer's per-circuit choices: which rail
(`Rail::Armco`, `Vangrail`, `Street`), the closest a straight's and a
corner's barrier may stand (`straight_barrier_min_m`,
`corner_barrier_min_m`), hoardings on or off, the tree belt (`TreeBelt`:
distance band, trees per 12 m cell, share of empty cells), `Flora`
(`Temperate`, `Desert`, `Tropical`), `Ground` (`Grass`, `Sand`, `Paved`),
`RoadSigns` (`BrakingBoards`, `German`), and flags for the floodlight ring,
the pit lane, city buildings, verge ground cover and the generated pit
garages.

| Style | Circuits (stem) | What differs from `DEFAULT` |
|---|---|---|
| `DEFAULT` | every other circuit | Armco 7 m (straights) / 14 m (corners) off the road, hoardings, temperate tree belt 22-90 m, braking boards, floodlight ring, pit lane and garages |
| `NORDSCHLEIFE` | `Nordschleife` | German guard rail 3 / 4.5 m off, no hoardings, dense forest 8.5-110 m with no clearings, German signs, no floodlight ring, no pit lane |
| `STREET` | `MarinaBay` | Tecpro 3 / 5 m off, no tree belt, tropical flora, paved ground, no floodlight ring, city buildings, no ground cover, no generated garages |
| `DESERT` | `YasMarina` | desert flora (palms and scrub whatever the wood is mapped as) |
| `SAKHIR` | `Sakhir` | desert flora in sparse clumps (two in three cells empty) on `Ground::Sand` |
| `TROPICAL` | `Sepang`, `SaoPaulo`, `MexicoCity` | broadleaf and palms (a needle-leaved wood keeps its conifers) |

How the fields act:

- **Rail.** `Rail::barrier` maps the barrier decision (see
  [track-pipeline.md](track-pipeline.md)): the vangrail style turns Tecpro
  into rail; the street style turns armco, armco-with-fence and tyres into
  Tecpro and keeps a mapped wall a wall. `Rail::asset` swaps kit keys for the
  vangrail family: `vangrail_4m` in corners, `vangrail_4m_triple` where the
  tightest radius is at least `VANGRAIL_TRIPLE_RADIUS_M` (400 m),
  `vangrail_4m_fence` where people stand behind it, `vangrail_end` closing a
  run. `VANGRAIL_ASSETS` lists them for the groomer's ownership test.
- **Ground.** `Sand` and `Paved` change only the look: `ats-export` gives the
  terrain, horizon and grass-band materials a `ground_set` and tint (sand in
  `DESERT_SAND_COLOR`; the **asphalt** set in `PAVED_COLOR`) and the groomer
  scatters no verge clumps. The server still drives the aprons as grass.
- **Signs.** `German` lays red-white chevrons round the outside of every
  corner tighter than 160 m, a kilometre board every kilometre, bend and
  danger warnings before the tightest corners and the "overtake on the left"
  board every 3 km (`CHEVRON_*`, `DANGER_*`, `OVERTAKE_*` in `groom.rs`; the
  `board/de_*` and chevron assets), and no braking boards.
- **City buildings.** A city circuit's buildings are `skyline_*` blocks by
  footprint (`dress::city_building_asset`), twin-arm street lamps are laid
  every 40 m alternating sides (`lay_street_lamps`), and the dossier keeps
  buildings out to 260 m from 600 m² (`CITY_STRUCTURE_RANGE_M`,
  `CITY_STRUCTURE_MIN_M2` in `osm_layout.py`).
- **Pit garages.** With `pit_garages` false the bake stands one
  `pit_building_roofdeck` (a door per box) where the garages would be, a
  `pit_wall_gantry_6m` for each pit wall module and a `garage_number_board`
  over each door; `garage_6m` and `garage_end` are not made.

**Run-off paint** is a separate per-circuit table, `dress::RUNOFF_PAINT`:
Spa `red_yellow`, YasMarina `blue_white`, Sakhir `blue_red` on every corner's
tarmac run-off.

To give a new circuit a style, add its stem to `CircuitStyle::for_stem` (and
the test beside it), re-run `ats-dress` and `ats-export`, and run the AI
survey: barrier distance changes what the AI hits.

## Road decals

Paint on the tarmac is an `.ats` `decals` layer (`ats.rs` `Decal`), not a
prop. The images are PNGs under `content/props/decal/<set>/`; `ats-dress` owns
every decal of the sets in `GRAFFITI_SETS` (`graffiti/`, `marina/`) and
re-lays them each run from the dossier (`graffiti`, expanded from
`MANUAL_GRAFFITI` and `MANUAL_DECAL_RUNS` in `osm_layout.py`). `ats-export`
bakes them as road-hugging meshes (family `decal`) that follow camber and
grade, drawn with the masked `M_ApexDecal`. Textures reach Unreal through
`-run=ApexPropImport -kind=decal`. A decal must lie on the road, so an edge
band sits just inside the edge. Art: `scripts/content/props/gen_graffiti.py`
(the graffiti), `scripts/content/props/gen_marina_decals.py` (the Marina Bay
set); format in [props.md](props.md).

## Nordschleife (`Nordschleife`)

The 20.8 km lap on its own, from the T13 start/finish, clockwise. The GP
circuit is a separate track (`Nuerburgring`).

**Centerline.** There is no GPS trace, so `scripts/osm_centerline.py
Nordschleife` routes the lap over OSM's `highway=raceway` ways, waypoint by
waypoint (`LAPS["Nordschleife"]`: OSM's own locality nodes per section, in
race order), resamples to 5 m nodes, takes widths from the ways' `width`
tags (9 m elsewhere) and solves a minimum-curvature raceline. The spec also
holds the start offset and the banking spans (the Karussell and the
Mini-Karussell, negative: both are left-handers). The YAML starts flat;
`dem_fetch.py` and `dem_elevation.py` give it its z. The OSM extract is large
and was cut from the planet file into `.cache/osm/Nordschleife.0.json`, so
`osm_layout.py` and `dem_fetch.py` run with `--offline`.

**Scene.** `seed_scene.py Nordschleife` starts the `.ats`, then the usual
`ats-dress` and `ats-export`. The dossier carries 40-odd named sections as
corners, the woods, barrier runs, the road bridges, Burg Nürburg
(`MANUAL_LANDMARKS`, anchored on OSM's `historic=castle` outline, laid as
`building/castle_ruin`) and the graffiti runs (`MANUAL_GRAFFITI`). The kit
pieces it needs (vangrail, German signs) are built by
`scripts/content/props/build_nordschleife_kit.py` (headless `bpy`: run it
with `python -I`).

**No pit lane.** The lane OSM maps at T13 merges into the road across the
start area and its walls stood on the racing line, so the style lays none and
the circuit has no pit sidecar (no pit stops).

**Checking it.** `python scripts/check_walls.py --openings Nordschleife`;
`SURVEY_TRACKS=Nordschleife` on the AI survey. The rails stand close, so a car
that leaves the road is more likely to be pinned against one.

**Traps.**
- The Nordschleife is wooded end to end: the DEM is a surface model and reads
  the canopy. Check a re-derived profile against the known relief (about
  300 m between Breidscheid and the Hohe Acht section) before trusting it.
- Tight rails change what the AI hits: run the survey after touching the
  style's distances.

## Mandarina Bay (`MarinaBay`)

The Singapore street circuit, current 19-turn layout, counter-clockwise,
about 4.9 km.

**Centerline.** OSM models the lap as one `type=circuit` relation (421263)
over ordinary streets plus the pit lane, with no raceway graph, so
`osm_centerline.py` walks the relation's own loop (`relation_loop`, from
`first_way`) instead of routing waypoints; widths are lanes x 3.5 m held to
10-14 m. `osm_layout.py` allow-lists the relation
(`STREET_CIRCUIT_RELATIONS`) so no other circuit's dossier moves.

**Order.** `osm_centerline.py`, `ats-smooth`, `bridge_elevation.py`,
`drs_zones.py` (authored zones), `track_location.py` (a `MANUAL` entry),
`osm_layout.py`, `seed_scene.py`, `ats-dress`, `ats-export`. No DEM and no
`ats-bank`: the land is reclaimed and flat, and a surface model reads the
roofs.

**Bridges.** The road is flat but for its two bridges.
`python scripts/bridge_elevation.py MarinaBay [--dry-run]` lifts the
dossier's `deck_arch` / `deck_wide` spans into raised-cosine humps
(`DECK_HEIGHT_M`: 2.5 m and 4 m, estimates; `RAMP_M` 60 m ramps either side),
rewriting only the `z:` lines of the nodes and the raceline, absolutely, so a
second run changes nothing. The bake leaves ground bands and curbs out of
every bridge span (`clip_surface`, `clip_curb`; the `.ats` `bridges` spans).

**Water** (`WATER_STEMS` in `osm_layout.py`): the dossier's `water` rings,
assembled from `natural=water` ways and multipolygon relations, become the
`.ats` `water` layer. `TerrainHeightfield::with_water` sinks the ground under
them (`WATER_BANK_M`, `WATER_DEPTH_M`, `WATER_BELOW_ROAD_M` in `terrain.rs`),
the bake draws the surface as a glossy untextured `scenery` material, and
dress and groom drop anything standing in water that a bridge does not carry
(`TerrainHeightfield::in_water`).

**Dossier tables in `osm_layout.py`** (all keyed `"MarinaBay"`):
- `MANUAL_STANDS` from the organiser's circuit park maps (stations
  approximate). `Stand.family` picks the kit: `"street"` is
  `street_stand_tier_10m` (`_roof` when covered), `"street_deck"`
  `street_stand_deck_10m` (`dress::stand_family`). Stands that fall in the pit
  zone are dropped by `ats-dress`.
- `MANUAL_STRUCTURE_ASSETS`: a `Structure.asset` naming a centred kit
  landmark building (`building_colonnade_hotel` ...); `"-"` leaves a footprint
  out.
- `MANUAL_LANDMARKS` (takes `yaw_toward`) with the landmark kinds
  `big_wheel_xl`, `skypark`, `lotus_museum`, `double_helix`.
- Crossings: `link` (covered walkway), `viaduct`, `gantry`, and the
  road-carrying `deck_arch` / `deck_wide` with `from_m` / `to_m`, found from
  the relation's `bridge=yes` ways (Anderson and Esplanade Bridge).
- `MANUAL_FURNITURE` (`layout::Furniture`: kind, asset, `from_m` / `to_m` /
  `every_m`, side, offset from the road edge): the start straight's props
  (banner gantry, finish tower, globe lamps, balloons, the roof wordmark, LED
  ribbons...), expanded by `dress::lay_furniture` after the pit-zone test.
- `MANUAL_DECAL_RUNS`: the painted edge bands (`marina/` decals).

**Raised furniture.** Grooming re-seats a dressed prop on the ground, so a
prop that stands higher (balloons and the wordmark on the pit roof, the LED
ribbon on the Tecpro) gets its height back from `dress::fixed_lift_m`, by
asset. A new raised asset must be added there.

**Street style specifics.** Tecpro 3 m off the road on straights and 5 m in
corners: more than `OTHER_LEG_CLEAR_M`, or the groomer refuses the wall beside
its own road, and at 3.5 m in corners the AI pinned cars in the Tecpro. A
hoarding behind every 3 m of rail, a brand per `STREET_BRAND_RUN_M`
(`lay_hoardings` with `street`). Walls may stand in front of buildings
(`building_clear`).

**Client.** The night slots of the start-straight props (`stand_led_*`,
`globe_lamp`, `balloon_lamp_*`, `step_light_orange`) and the street stand
family in `ApexProps::LayoutGrandstand` are in
`game-unreal/Source/ApexSim/Private/Track/ApexPropLibrary.cpp`
(`ApexSim.Props.*` tests).

**Checking it.** `python scripts/check_walls.py --openings MarinaBay`;
`SURVEY_TRACKS=MarinaBay` on the AI survey; `PIT_TRACKS=MarinaBay` on the pit
lane survey (every box served).

**Traps.**
- **Paved ground samples the asphalt set**, not concrete: the concrete set on
  the ground, horizon and grass-band keys hung the GPU
  (`DXGI_ERROR_DEVICE_HUNG`) a few seconds into a race, cause unknown. The
  doc comment on `Ground::Paved` still says concrete; the code
  (`paved_ground` in `ue_export.rs`) is what counts.
- Run `bridge_elevation.py` before the dossier is refitted and dressed: the
  dossier's spans are stations along that centerline.
- The dossier has no corner names of its own yet; its one "corner" is the
  circuit, and there is no `CORNER_DISPLAY` entry for it (see
  [naming.md](naming.md)).
