# Trackside prop kit

The reusable scenery library every circuit is dressed from. Props are placed in
the track editor (`.ats` `props`, `add_prop`), baked by `ats-export` into the
`.uescene.json` `props` array, and resolved to meshes by the Unreal importer
(`ApexTrackAssetBuilder.cpp`). Assets are authored in Blender and imported as
static meshes; the generated box recipes stay as the fallback for any asset
that is not authored yet.

Every prop carries a `kind` (coarse category — drives grooming and import
behaviour) and an `asset` key (the specific mesh). This document is the shared
vocabulary for both.

## Conventions

- **Pivot** on the ground. The deep kinds that face the road — grandstand
  bays and caps, buildings, pit garages and pit walls, the fanzone stage,
  video screen and portaloos — have it on the **road-facing edge**, and the
  footprint reaches away from the road from there; only the thin modules
  (barriers, tyre walls, fences, boards) and the free-standing pieces
  (`control_tower`, `camera_tower`, `ferris_wheel`, `tent_6m`,
  `observation_tower`, the `skyline_*` backdrop buildings) are centred
  on their footprint (bridges: centre of the span; sky props: on the ground
  directly below). Anything that needs the footprint — the groomer, the
  server's wall bake — offsets the deep kinds half their depth behind the
  pivot.
- **Axes**: local +X runs along the track, +Y is the side the road is on
  (the importer flips a `faces_road` kind 180° when the road turns out to be
  on -Y). Z up.
- **Modules** are sized on 2/4/6 m multiples so runs tile without gaps.
- **Brands are data**, not meshes: one board mesh, the brand picked by the
  prop's `text` field (material instance / decal). All brands fictional.
- **Every kind has a `default` asset** so a track never falls back to cubes.
- **Source** lives in `content/props/<kind>/<asset>.blend` + exported `.glb`;
  imported meshes at `/Game/Props/<kind>/SM_<asset>`. The importer resolves
  `SM_<asset>` → `SM_default` → generated recipe → placeholder cube.
- **Performance tiers**: `nanite` for large one-offs (grandstands, buildings,
  bridges), `instanced` for anything placed by the hundred (armco, tyre
  walls, trees, boards, fence panels).
- **Blender frame**: props are modelled with the road on **-Y** in Blender
  (right-handed, Z up); glTF export (`+Y up`) and Unreal's import map that to
  the importer's "faces road" side. Verify the yaw once on the first imported
  asset; if it comes in backwards the fix is one sign in the importer, not in
  the assets. `sky` props have their origin at the hull centre (their `z` is
  the altitude), everything else at ground level.
- **Tooling**: `content/props/_tools/apex_props.py` — bmesh builders (box,
  cylinder, torus, profile sweep, textured quads), planar UVs, GLB export and
  a preview render. Every asset so far is generated from a script with it, so
  variants are a parameter change. Brand and marker textures are generated
  by `content/props/_tools/gen_brands.py` into `board/brands/*.png` and
  `board/markers/*.png` (3:1 and 3:4).
  Batch scripts live beside their kind (`barrier/build_barriers2.py`,
  `tree/build_near_trees.py`) or under `_batches/` when they span kinds
  (`build_batch_e_kit.py`, `build_batch_f_terrain.py`); each takes `ASSET`
  (`"all"` or one key) and exports on run. `_tools/apex_tex.py` bakes
  tileable PBR maps and masked card textures with numpy. The builders work
  from the MCP server as well as the console (no operators that need a
  window; sharp edges are marked in bmesh rather than by `shade_auto_smooth`).

## Fictional brands

Boards, bridges and the blimp rotate through these so the same name is never
seen twice in a row: `piretti` (tyres), `rolux` (timing), `apexsim`,
`velocet` (fuel), `kronos` (watches), `hexon` (oil), `northwind` (airline),
`brix` (tools). Add here before using a new one.

## Kinds

Existing `PropKind`s are kept; the ones marked *new* are additions to the
`.ats` enum, the groomer and the importer.

| kind | groom behaviour | import | status |
| --- | --- | --- | --- |
| `barrier` | re-laid as continuous runs at the runoff edge | instanced | exists |
| `tire_wall` | as barrier | instanced | exists |
| `board` *new* | snaps onto the nearest barrier run, faces road | instanced | new |
| `sign` | pushed clear of road, faces road | instanced | exists |
| `fence` *new* | as barrier, behind it | instanced | new |
| `grandstand` | pushed clear, faces road | nanite | exists |
| `building` | pushed clear; pit-side aligns to the lane | nanite | exists |
| `pit` *new* | aligned to the pit lane, positioned per box | nanite | new |
| `bridge` *new* | never pushed: spans the road, seated on both verges | nanite | new |
| `light` | pushed clear | instanced | exists |
| `tree` | pushed clear, seated | instanced | exists |
| `vehicle` *new* | pushed clear | instanced | new |
| `attraction` *new* | pushed clear, big footprint | nanite | new |
| `sky` *new* | not seated; z is absolute altitude | single actor | new |
| `cone` | none | instanced | exists |
| `misc` | pushed clear | instanced | exists |

## Asset list

Priority: **P1** seen every lap at speed · **P2** silhouettes and pit straight ·
**P3** background · **P4** nice to have. Build P1 first; P1 + the blimp changes
the look more than anything else.

### 1. Track edge

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| barrier | `armco_4m` | 4 m | default; straight, 3-rail — **done** | P1 |
| barrier | `armco_4m_fence` | 4 m, 3.5 m tall | with debris-fence posts and mesh (masked chain-link texture) — **done** | P1 |
| barrier | `armco_end` | 2 m | terminal ramp; tiles onto `armco_4m` at x = -1 — **done** | P1 |
| barrier | `concrete_4m` | 4 m, 1.0 m tall | precast wall, slanted faces — **done** | P1 |
| barrier | `concrete_4m_rail` | 4 m, 1.55 m | with top rail — **done** | P2 |
| barrier | `tecpro_2m` | 2 m, 1.15 m | red + white block pair — **done** | P2 |
| barrier | `sausage_kerb_2m` | 2 × 0.5 m, 0.1 m | yellow FIA sausage kerb (`kerb_yellow`); baked as a `kerb` wall (kind 3): the sim scrubs speed and rumbles the wheel, never pushes the car out — **done** | P1 |
| barrier | `tecpro_corner` | 1.4 m quarter annulus | 90° cap, tiles onto a `tecpro_2m` run at x = -1 and turns it away from the road (same convention as `tires_corner`) — **done** | P1 |
| barrier | `concrete_end` | 2 m | ramped terminal, full height at x = -1 down to 0.15 m; tiles onto `concrete_4m` — **done** | P1 |
| tire_wall | `tires_4m` | 4 m, 4 tyres tall, 2 rows | default; conveyor-belt strip in front — **done** | P1 |
| tire_wall | `tires_corner` | 2 m quarter arc | curved cap, tiles onto `tires_4m` at x = -1 — **done** | P2 |
| board | `hoarding_3m` | 3 × 1 m | default; brand from `text` — **done** | P1 |
| board | `hoarding_6m` | 6 × 1 m | brand from `text`, logo twice — **done** | P1 |
| board | `braking_marker` | 0.75 × 1 m on a 1.9 m post | `text` = 50/100/150/200 — **done** | P1 |
| board | `light_panel` | 1 × 0.6 m on a 2 m post | LED face is the `led_panel` slot (drive emissive for flags) — **done** | P2 |
| board | `corner_sign` | 2.4 × 0.8 m board, 2.6 m | named-corner board on two posts; the corner name is the prop's `text`, rendered on the white `board_text` face (importer: text render like `sign`) — **done** | P1 |
| sign | `marshal_post` | 2.4 × 2.4 m hut on a plinth, 3.1 m | flag pole, number board (`text` later) — **done** | P2 |
| sign | `pit_speed_limit` | 2.8 m | round sign on a post; number from `text` later — **done** | P2 |
| sign | `pit_exit_light` | 4.2 m | red/green lamps (`pit_light_red` / `pit_light_green` slots) — **done** | P2 |
| sign | `flag_pole` | 8 m, 1.5 × 1 m flag | `flag_cloth` slot; `text` = country code → `sign/flags/<cc>.png` (nl de at fr it be ie es jp ch fin chequer) — **done** | P3 |
| sign | `hillside_letters` | 47 × 0.8 m, 4.7 m | free-standing letters on steel feet (`letters_white`), centred pivot; the letters are geometry, so the GLB carries one string — rebuild with `TEXT = "..."` in `_batches/build_batch_e_kit.py` for another circuit (default "RED BULL RING") — **done** | P2 |
| fence | `mesh_4m` | 4 × 2.5 m | default; chain-link masked texture — **done** | P2 |
| fence | `mesh_4m_hoarding` | 4 × 2.5 m | 0.8 m brand strip at the bottom — **done** | P2 |
| fence | `wood_4m` | 4 × 1.2 m | post-and-rail — **done** | P3 |
| fence | `hedge_4m` | 4 × 1.6 m | — **done** | P3 |

### 2. Overhead

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| bridge | `start_gantry` | 15 m span | default; replaces the generated gantry; lamps are the `gantry_lamp` slot (drive emissive from the race director) — **done** | P2 |
| bridge | `truss_bridge` | 15 m span | box-truss footbridge, hoarding both faces, brand from `text` — **done** | P2 |
| bridge | `tyre_bridge` | 15 m span, 13.6 m tall, 11.5 m clearance | the donut, `piretti` along the sidewalls, walkway through it — **done** | P2 |
| bridge | `timing_gantry` | 15 m span, 10.5 m | 12 × 3 m `led_screen` facing the approach, brand strip — **done** | P3 |
| bridge | `span_building` | 15 m span (scales with `span_m` like the other bridges), 8 m deep, 22 m tall | wide glazed block on two piers, 10 m clearance; brand strip (`bridge_brand`) at the base, two glazing bands (`bridge_glass`) per face — for structures that cross the road, like the W hotel at Yas Marina or Shanghai's pit-building wings — **done** | P3 |
| light | `floodlight_tower` | 30 m lattice mast, 12 lamps | lamps are the `floodlight_lamp` slot — **done** | P2 |
| light | `lamp_post` | 8 m | `floodlight_lamp` slot — **done** | P3 |

Bridges take a `span_m` from the road width at their station; the mesh is
authored for a 15 m road (supports 1.5 m off each edge, so 18 m between the
footings) and the importer scales **local Y** only — the road runs along X,
so the span is across Y.

### 3. Pit complex

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| pit | `garage_6m` | 6 × 12 m, 9.9 m tall | default; door up, upper storey with balcony (branded balustrade) 2.2 m over the lane — **done** | P2 |
| pit | `garage_6m_closed` | 6 × 12 m | roller door down — **done** | P3 |
| pit | `garage_end` | 6 × 12 m, 15.9 m with mast | race-control block — **done** | P2 |
| pit | `pit_wall_6m` | 6 m, 3.8 m tall | 1.1 m wall + fence on the track side (-Y), team stand on the lane side — **done** | P2 |
| pit | `pit_wall_plain_6m` | 6 m, 2.9 m | wall + fence — **done** | P2 |
| pit | `box_kit` | 5.8 × 3 m in front of a garage | fuel rig, wheel-gun trolley, 4 tyre stacks, jack, pit board — **done** | P3 |
| building | `hospitality_3f` | 30 × 12 m, 12.4 m | glass front, roof terrace — **done** | P3 |
| building | `media_centre` | 40 × 15 m, 21 m with mast | 4 storeys — **done** | P4 |
| building | `control_tower` | 12 × 12 m cab on an 8 × 8 core, 29 m | glass cab, brand board on the track face, antenna — **done** | P3 |
| building | `clubhouse` | 24 × 14 m, 11.5 m | brick, pitched roof, timber balcony over the front, flag pole — **done** | P3 |
| building | `observation_tower` | 8 × 8 m footprint tapering to 2 × 2 m, 60.5 m | 4-leg steel lattice on a concrete lift core, glazed pod + spire; centred on its footprint like `control_tower` (not road-facing); generic enough to cover Austin's 77 m tower or the Sakhir Tower via `Prop.Scale` — **done** | P4 |
| building | `podium` | 12 × 4.5 m, floor at 9.95 m, 14 m | pit-roof podium: three steps, rail, 12 × 4 m backdrop with the brand (`board_brand`) tiled; placed at ground on the road-facing edge over a `garage_6m`, four slim columns hide inside the garage — **done** | P2 |
| vehicle | `race_truck` | 8.6 m | transporter, brand on both trailer sides (`board_brand`) — **done** | P3 |
| vehicle | `motorhome` | 10 m | with awning — **done** | P4 |
| vehicle | `camper_van` | 6 m, 3.1 m | high-roof camper with rolled awning and roof box (`vehicle_paint_a`) for the camp sites — **done** | P2 |
| vehicle | `coach` | 12 m, 4 m | tour bus, glass band, luggage doors (`vehicle_paint_b`) for the bus park — **done** | P2 |
| vehicle | `safety_car` | 4.8 m | silver estate with orange light bar (`safety_lightbar`, emissive) and lettering — **done** | P2 |
| vehicle | `medical_car` | 4.8 m | same body in orange (`vehicle_paint_medical`) — **done** | P2 |

The exporter emits one `pit`/`garage_6m` per pit box (positions from the
existing box layout), a `pit_wall_6m` run along the road side, and one
`garage_end` at each end of the row.

### 4. Spectators

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| grandstand | `bay_10m` | 10 × 9 m, 8 tiers, 5.6 m tall | default; open; 152 seats, central aisle — **done** | P2 |
| grandstand | `bay_10m_roof` | 10 × 10.4 m, 10.6 m tall | roofed: rear truss columns + cantilever — **done** | P2 |
| grandstand | `bay_10m_large` | 10 × 14.2 m, 14 tiers, 8.3 m tall | main-straight size — **done** | P2 |
| grandstand | `bay_10m_large_roof` | 10 × 15.7 m, 13.3 m tall | — **done** | P2 |
| grandstand | `bay_10m_curve6` / `_roof` | 10 m front, 6° wedge | outside-of-corner stand, front radius 95 m — **done** | P2 |
| grandstand | `bay_10m_curve12` / `_roof` | 10 m front, 12° wedge | outside-of-corner stand, front radius 48 m — **done** | P2 |
| grandstand | `bay_10m_curve6_in` / `_roof` | 10 m front, -6° wedge | inside-of-corner stand (front wider than back) — **done** | P2 |
| grandstand | `end_cap` | 1 × 9 m | stepped side block, symmetric about x=0: place at ±(L/2 + 0.5) — **done** | P2 |
| grandstand | `end_cap_large` | 1 × 14.2 m | end cap for the 14-tier bays — **done** | P2 |
| grandstand | `<any bay>_crowd`, `scaffold_10m_crowd`, `banking_seats_crowd` | same as the base asset | seated crowd on masked card strips per row (`crowd_cards` slot, `grandstand/T_crowd.png`, 16 people per 8 m tile, ~15 % empty seats); the importer picks `_crowd` when the session wants spectators — **done** | P2 |
| grandstand | `bay_10m_stadium_roof` | 10 × 32.7 m, 3 tiers, 27.96 m tall | stadium-scale roofed bay: three raked decks behind two podium breaks, cantilever roof over the top deck — for the IMS oval, Hockenheim's Motodrom, Foro Sol and the Shanghai/Yas/Sepang main stands — **done** | P2 |
| grandstand | `bay_10m_stadium_curve6_roof` | as `bay_10m_stadium_roof`, 6° wedge | curved counterpart, same `Rf` convention as `bay_10m_curve6` — **done** | P3 |
| grandstand | `end_cap_stadium` | 1 × 32.7 m, 25.46 m tall | end cap sized to `bay_10m_stadium_roof`'s seating bowl (roof excluded, same convention as `end_cap`/`end_cap_large`) — **done** | P2 |
| grandstand | `stair_tower` | 4 × 4 m | between bays | P3 |
| grandstand | `scaffold_10m` | 10 × 5 m, 5 tiers | tube-and-plank club stand — **done** | P3 |
| grandstand | `banking_seats` | 10 × 6 m | 4 bench rows on a grass bank — **done** | P4 |
| attraction | `video_screen` | 12 × 7 m screen, 11 m tall | `led_screen` slot for a render target; brand strip on top — **done** | P2 |
| attraction | `camera_tower` | 4 × 4 × 13 m | — **done** | P3 |
| attraction | `ferris_wheel` | 60 m dia, hub at 35 m, 45 × 15 m footprint | wheel plane faces the road; GLB has a child node `rotor` with its origin at the hub for in-game rotation (gondolas are rigid to it) — **done** | P2 |
| attraction | `tent_6m` | 6 × 6 m, 5.2 m | open front on -Y, `tent_colour` slot — **done** | P3 |
| attraction | `portaloo_row` | 5.4 m | 5 units — **done** | P4 |
| attraction | `fanzone_stage` | 12 × 8 m, 8.5 m | truss roof, `led_screen` back wall, brand strip — **done** | P4 |
| attraction | `food_stall_6m` | 6 × 3 m, 3.9 m | trailer concession, serving hatch and striped awning to the road (awning reaches 1.6 m in front of the pivot), brand fascia (`board_brand`) — **done** | P1 |
| attraction | `ticket_gate` | 7 × 1.8 m, 4.6 m | four tripod turnstiles between galvanised rails under a WELCOME arch with brand boards on the columns; one per fan-map entrance — **done** | P2 |

A stand of length L is `bay_*` repeated L/10 times plus two `end_cap`s. The
importer does the repetition from one `grandstand` prop with a `length_m`
(default 30), so the `.ats` keeps a single prop per stand.

**Straight bays** tile at 10 m pitch along local X; caps at x = ±(L/2 + 0.5).

**Curved bays** are wedges: the half-width is 5 m at the front edge (y = 0)
and grows linearly with y (shrinks for `_in`), so the two side edges converge
at a point on the y axis, `P = (0, -Rf)` with `Rf = 5 / tan(θ/2)` (θ = 6° →
95.4 m, 12° → 47.6 m; for `_in` the point is at `+Rf`, in front of the stand).
Bay i of n is bay 0 rotated by `(i - (n-1)/2)·θ` about P — equivalently
`yaw_i = yaw_0 + i·θ` and `pos_i = P + R(yaw_i)·(0, Rf)`. Adjacent bays then
share their side edges exactly, roof included. The end caps take the same
rotation as the outermost bays with an extra ±0.5 m along their local X.
The stand's `length_m` maps to `n = round(length_m / 10)` bays; pick the wedge
whose Rf is nearest the track's radius at that station (straight for
R > ~150 m). Verified in Blender with six `curve12_roof` bays.

### 5. Landscape

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| tree | `broadleaf_s` / `_m` / `_l` | 6 / 10 / 16 m | default = `_m`; **solid** low-poly foliage blobs (240–520 tris, no alpha), three green slots `tree_foliage_a/b/c` — **done** | P2 |
| tree | `conifer_m` / `_l` | 12 / 20 m | stacked cones, solid — **done** | P2 |
| tree | `poplar` | 18 m | — **done** | P3 |
| tree | `bush_cluster` | 3 m | — **done** | P3 |
| tree | `broadleaf_s/m/l_autumn`, `poplar_autumn`, `bush_cluster_autumn` | as the base asset | autumn colour set (`tree_autumn_a/b/c`); pick by the track's season — **done** | P3 |
| tree | `palm_ornamental` | 13.15 m | tall avenue date palm, ringed tapering trunk, drooping 2-segment fronds on `tree_foliage_a/b/c` (same shared slots as the broadleaf trees) — for Sakhir and Yas Marina landscaping — **done** | P1 |
| tree | `palm_oil` | 8.35 m | shorter, denser plantation palm, fuller radiating crown — for the oil-palm plantations around Sepang — **done** | P1 |
| tree | `forest_impostor` / `_conifer` | 43 × 43 m, 17.5 m | one flat-shaded cluster (46 blob/cone trees on a dark floor skirt, ~1 K tris) standing in for a 40 m patch of wood; mixed 60 % spruce, or all spruce; planted per forest polygon out to 8 km, centred — **done** | P1 |
| tree | `broadleaf_m_near` | 10 m | near-LOD (first 60 m) version of `broadleaf_m`: textured trunk + branches, ~250 bent leaf cards (2.2 K tris) on the alpha-MASKED two-sided `tree_card_broadleaf` slot — **done** | P2 |
| tree | `conifer_m_near` | 12 m | spruce: trunk + drooping frond cards on `tree_card_conifer` (1.2 K tris) — **done** | P2 |
| tree | `grass_clump` / `wildflower_clump` | 1 × 0.7 m | three crossed masked cards (`scatter_grass` / `scatter_flower`, 6 tris), sunk 0.1 m; scatter for the grass band within 40 m of the road — **done** | P2 |
| vehicle | `car_a` / `car_b` / `car_c` | 4–4.7 m | hatch / saloon / SUV, `vehicle_paint_*` slot for colour — **done** | P3 |
| vehicle | `fire_truck` | 5.5 m | — **done** | P3 |
| vehicle | `ambulance` | 6 m | — **done** | P3 |
| vehicle | `tractor` | 4.5 m | with front forks — **done** | P3 |
| misc | `bollard` | 0.9 m | — **done** | P3 |
| misc | `kerb_marker` | 0.7 m | yellow/black — **done** | P3 |
| misc | `generator` | 2.2 × 1.2 m | — **done** | P4 |
| misc | `photographer_stand` | 2 × 2 × 2.5 m | — **done** | P4 |
| misc | `scrub_clump` | 0.68 × 0.6 m, 0.29 m tall | low dry-scrub mound, two tones (`misc_scrub_a/b`); filed under `misc` rather than a dedicated kind — no `.ats`/groomer/importer change needed to place it — **done** | P1 |
| misc | `rock_cluster` | 0.92 × 0.7 m, 0.45 m tall | angular rock chunks, two tones (`misc_rock_a/b`); same `misc`-kind placement as `scrub_clump` — for Sakhir/Yas desert ground cover — **done** | P1 |
| misc | `bull_statue` | 22 × 11.5 m, 17.2 m tall | "Der Bulle vom Spielberg" at its real size: a 14.6 m Corten bull (`statue_corten`, plate seams baked in) with 7 m gilded horns (`statue_gold`), charging through its 17.2 m cast-aluminium arch (`statue_arch`), hooves on concrete pads; built by `_batches/build_bull_statue.py`, figures from the fabricator in its docstring. Centred on its own footprint like `control_tower`/`ferris_wheel` rather than road-facing: it is placed as a `statue`-kind landmark, bull charging along the course heading — **done** | P4 |
| misc | `tyre_stack` | 1.9 × 1.2 m, 0.8 m | three loose stacks + one leaning tyre (`tire_rubber`, `tire_rubber_worn`) for marshal posts and pit entry — **done** | P2 |
| misc | `gate_4m` | 4.3 m, 1.5 m | steel field gate on two galvanised posts (`gate_steel`, `armco_galv`) for OSM `barrier=gate` — **done** | P2 |
| misc | `power_pylon` | 8 × 14 m footprint, 38 m | tapered 4-leg lattice suspension tower, two crossarm levels with hanging insulators; centred, line along local X — **done** | P1 |
| cone | `cone` | | exists | — |

### 6. Sky

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| sky | `blimp` | 60 m | `rolux`; slow drift + yaw in-game — **done** | P1 |
| sky | `balloon` | 16 m envelope, 22 m with basket | origin at the envelope centre; `balloon_envelope` brand slot — **done** | P3 |
| sky | `helicopter` | 12 m, 11 m rotor | origin at the fuselage; `rotor_disc` slot — **done** | P4 |

### 7. Street-circuit skyline

Distant backdrop buildings for street circuits (Singapore, Baku, Monaco-style
venues) — filled the empty horizon those tracks leave. All `building` kind,
centred pivot (see Conventions), `nanite`, no `text`/brand slot. Ten distinct
massings so a scattered skyline doesn't repeat; five glass tints
(`skyline_glass_blue/green/bronze/teal/grey`) shared and re-used across them
on purpose, the way a real skyline repeats curtain-wall colours. Simple boxy
massing + a few `skyline_frame` belt bands stand in for floor lines — no
window grid geometry, since these are only ever seen at a distance.

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| building | `skyline_slab_a` | 24 × 16 m, 96.2 m | plain glass slab, blue — **done** | P2 |
| building | `skyline_slab_b` | 18 × 18 m, 135 m | taller slab, bronze, rooftop plant block — **done** | P2 |
| building | `skyline_step` | 30 × 22 m tapering to 10 × 8 m, 150 m | art-deco 3-tier setback tower, teal — **done** | P2 |
| building | `skyline_twin` | 32 × 12 m overall (two 12 × 12 m towers), 161.2 m | linked towers with a sky-bridge at 100 m, grey — **done** | P3 |
| building | `skyline_pyramid` | 20 × 20 m, 112 m | glass shaft with a pyramidal cap, green — **done** | P3 |
| building | `skyline_cylinder` | 20 m dia, 145 m | cylindrical glass tower, ring-belt floors, blue — **done** | P3 |
| building | `skyline_podium` | 40 × 30 m podium, 16 × 16 m tower, 121.2 m | retail podium (glazed band) + slender tower, teal — **done** | P2 |
| building | `skyline_needle` | 14 × 14 m tapering to 9 × 9 m, 190 m | tallest of the set, one setback + antenna spire, grey — **done** | P3 |
| building | `skyline_lowrise` | 26 × 18 m, 40.8 m | mid-rise infill block, concrete with bronze strip windows, roof tank + AC unit — **done** | P2 |
| building | `skyline_crane` | 20 × 16 m building (crane jib reaches to x = -20), 76.3 m | unfinished concrete-frame building topped with a working tower crane (`skyline_crane_yellow`) — **done** | P3 |

### 8. Village and farmyard (Spielberg)

Styrian gabled houses for the residential and farmyard polygons inside the
2 km ring, plus the chapel. `building` kind, pivot on the road-facing edge,
rendered walls (`house_render`, `house_render_b`), timber gables and
balconies (`house_timber`, `house_timber_dark`), tile or dark roofs
(`house_roof_tile`, `house_roof_dark`), `house_stone` chimneys.

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| building | `village_house_a` | 12 × 9 m, 8.3 m | 1.5-storey farmhouse, ridge along the road, timber balcony under the eaves — **done** | P1 |
| building | `village_house_b` | 10 × 8 m, 8.5 m | gable to the street, dark roof — **done** | P1 |
| building | `village_house_c` | 14 × 12 m, 6.9 m | L-shaped: main wing along the road plus a lower stable wing behind, yard wall — **done** | P1 |
| building | `barn` | 18 × 10 m, 8.2 m | timber barn on a stone plinth, double doors to the road — **done** | P1 |
| building | `chapel` | 8 × 22 m, 23.5 m | 5 m tower on the road end with clock and louvres, pointed spire, nave with apse — **done** | P1 |

## Unreal import notes

- Every asset in the tables above is authored (**done**); the recipe fallback
  only matters for unknown asset keys now.

- Masked materials (`fence_mesh`, `crowd_cards`) arrive as glTF `MASK` +
  `doubleSided`; the importer must make them Masked and two-sided or they render
  as opaque black cards. Trees no longer use alpha at all (solid geometry) after
  exactly that happened in the first import.
- Emissive-able slots: `gantry_lamp`, `led_panel`, `floodlight_lamp`,
  `led_screen` (the last is meant for a render target / media texture).
- **Night pass.** These slots carry an emissive colour/texture in the GLB and
  need one scalar (`EmissiveStrength`, 0 by day, 1 at night) on their material
  instances: `pit_glass` (window-glow texture on garages, `garage_end`,
  `hospitality_3f`, `media_centre`, `control_tower`, `clubhouse`, `motorhome`,
  `race_truck`), `pit_interior` (lit garage ceilings), and on the Ferris
  wheel `ferris_lights` (warm spoke + gondola strips), `ferris_lights_rim`
  (blue rim ring and leg strips) and `ferris_lights_hub`. A slow hue cycle on
  `ferris_lights_rim` is cheap and looks right.
- Chain-link goes sub-pixel at distance; mip bias or a fade helps.
- **Masked card slots** `tree_card_*` and `scatter_*` (near-LOD trees, grass) are
  alpha MASK + two-sided like `fence_mesh`; `ApexPropLibrary::IsMaskedSlot`
  covers them.
- **Text slots.** `corner_sign` carries the corner name on its blank
  `board_text` face; the importer (`ApexProps::HasTextFace`) spawns it as its
  own actor rather than an instance and hangs a text component just off the
  face at 2.2 m, sized to fit the 2.4 m board. `hillside_letters` is geometry.
- **Baked textures.** The whole track-edge, tyre-wall, grandstand (every bay,
  cap and `_crowd` variant), pit and statue kit carries albedo + roughness +
  normal maps (512², tileable, from `_tools/apex_tex.py`, saved under
  `content/props/_textures/`) on its existing slots; `_tools/retexture_kit.py`
  re-imports each GLB, swaps the slots listed in `apex_tex.KIT_SLOTS` and
  re-exports, so geometry and slot names are unchanged (it restores a slot
  name the importer suffixed). New builders take their materials from
  `apex_tex.kit_material`, which returns the baked material for a slot in
  `KIT_SLOTS` and a flat one otherwise, so a slot looks the same on every
  asset; add a slot there to bake it everywhere.
- `track-editor/core/src/ats.rs` — new `PropKind` variants.
- `track-editor/core/src/groom.rs` — behaviour per new kind (board snapping,
  bridge exemption, pit alignment, sky not seated).
- `track-editor/core/src/ue_export.rs` — pit garages/walls emitted per box;
  `length_m` on grandstands; `span_m` on bridges.
- `game-unreal/Source/ApexTrackEditor/Private/ApexTrackAssetBuilder.cpp` —
  asset lookup by `/Game/Props/<kind>/SM_<asset>` before the recipe fallback;
  bay repetition; brand/text → material parameter.
- `game-unreal/Content/Props/` — imported meshes and material instances.
