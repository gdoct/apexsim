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
- **Tooling**: `scripts/content/props/apex_props.py` — bmesh builders (box,
  cylinder, torus, profile sweep, textured quads), planar UVs, GLB export and
  a preview render. Every asset so far is generated from a script with it, so
  variants are a parameter change. Brand and marker textures are generated
  by `scripts/content/props/gen_brands.py` into `board/brands/*.png` and
  `board/markers/*.png` (3:1 and 3:4).
  The batch scripts sit beside it in `scripts/content/props/`
  (`build_barriers2.py`, `build_near_trees.py`, `build_batch_e_kit.py`,
  `build_batch_f_terrain.py`, ...), apart from the content they write: the
  GLBs and the `.blend` sources stay under `content/props/<kind>/` (a scene
  spanning kinds under `content/props/_batches/`). Each takes `ASSET`
  (`"all"` or one key) and exports on run. `apex_tex.py` bakes
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
| tree | `broadleaf_s` / `_m` / `_l` | 6 / 10 / 16 m | default = `_m`; **leaf-card trees** (`card_trees.py`, 2026-10-03): a curved trunk with a branch to each lobe of a lumpy crown, the crown shelled with 260 / 440 / 620 folded cards on the alpha-MASKED two-sided `tree_card_broadleaf` slot (a 2x2 atlas of leaf sprays: sunlit, warm, cool, and a dark one inside the crown), crown-shaped custom normals; 1.4 / 2.2 / 3.1 K tris. Replaced the solid noise-bumped icosphere blobs, which read as low-poly beside the card trees — **done** | P2 |
| tree | `conifer_m` / `_l` | 12 / 20 m | spruce: whorls of frond cards on `tree_card_conifer` (feather-shaped spruce branches with light new growth at the tips), drooping low down and rising near the top; 1.4 / 2.0 K tris. Replaced the stacked solid cones — **done** | P2 |
| tree | `poplar` | 18 m | Lombardy poplar: a spindle of upright leaf clusters on `tree_card_broadleaf`, 1.9 K tris — **done** | P3 |
| tree | `bush_cluster` | 3 m | a low mound of leaf cards on a few stems, 0.6 K tris — **done** | P3 |
| tree | `broadleaf_s/m/l_autumn`, `broadleaf_m_near_autumn`, `poplar_autumn`, `bush_cluster_autumn` | as the base asset | the same tree (same seed) on `tree_card_autumn`; picked by the track's season — **done** | P3 |
| tree | `palm_ornamental` | 13.15 m | tall avenue date palm, ringed tapering trunk, drooping 2-segment fronds on `tree_foliage_a/b/c` — for Sakhir and Yas Marina landscaping; still the solid v2 build (no track plants one yet) — **done** | P1 |
| tree | `palm_oil` | 8.35 m | shorter, denser plantation palm, fuller radiating crown — for the oil-palm plantations around Sepang; solid v2 build — **done** | P1 |
| tree | `forest_impostor` / `_conifer` | 43 × 43 m, 17.5 m | a 40 m patch of wood: 46 trees, each three crossed billboards of a sprite rendered from the card trees themselves (`card_trees.forest_material`: four sprites, ambient-lit, on `forest_billboard`), 552 tris; mixed 60 % spruce, or all spruce; trunks reach 4 m below the pivot so a patch planted level on a slope does not float; planted per forest polygon out to 8 km, centred — **done** | P1 |
| tree | `broadleaf_m_near` | 10 m | near-LOD (first 60 m) version of `broadleaf_m`: the same generator with 440 smaller cards bent once along their length (4 K tris) — **done** | P2 |
| tree | `conifer_m_near` | 12 m | near-LOD spruce: denser whorls, each frond bent and its tip turned up (3.2 K tris) — **done** | P2 |
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

**Night (2026-10-09).** The glass slots are no longer `skyline_glass_*`: they
are the Marina Bay curtain-wall slots the night pass already drives
(`ApexPropLibrary.cpp` `NightGlowOf`, by slot name) with their lit-window
emissive atlas: blue -> `mb_glass_blue`, teal -> `mb_glass_teal`, bronze ->
`mb_glass_bronze`, grey -> `mb_glass_grey`, green (the pyramid) ->
`mb_glass_teal` (nearest hue; there is no `mb_glass_green`). Facade UVs are in
metres (`KHR_texture_transform` scale 1/tile, as the Marina Bay towers), so
windows sit on a floor grid from y = 0, each face and building shifted by whole
cells so the lit patterns differ; roof/floor glass faces are pinned to a
mullion strip and never light. Frame, roof and concrete slots are untouched.
The GLBs are patched by `scripts/content/props/skyline_night.py` (numpy only,
idempotent; the original Blender build script is not in the repo and the
`.blend` files were not updated). Each GLB is now ~1.2 MB (the three atlas PNGs
are embedded, like every `mb_*` asset). Lit share is whatever the shared atlas
has (about 17-40% of cells); a denser or per-floor pattern would need new
atlas images under the same slot names, which the prop import shares by name
across the `building` kind.

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

### 9. Nordschleife kit

Built by `scripts/content/props/build_nordschleife_kit.py` (headless `bpy`
or inside Blender; scene saved as `_batches/nordschleife_kit.blend`). Laid
by the groomer only where the circuit's style asks for them
(`track-editor/core/src/circuit_style.rs`, the Nordschleife): the barrier
pass swaps its armco for the vangrail, the sign pass lays the German signs
instead of braking boards, and `ats-dress` lays the castle from a
`castle` landmark.

| kind | asset | size | notes |
| --- | --- | --- | --- |
| barrier | `vangrail_4m` | 4 × 0.3 m, 1.25 m | German guard rail (Schutzplanke / *vangrail*): two A-profile W-beams with daylight between them, bolted to spacer blocks on C-posts every 2 m; slots `armco_galv`, `armco_post`, `armco_bolt` |
| barrier | `vangrail_4m_triple` | 4 × 0.3 m, 1.6 m | three beams: the fast stretches (tightest radius ≥ 400 m) |
| barrier | `vangrail_4m_fence` | 4 × 0.4 m, 3.55 m | double rail under a catch fence (`fence_post`, masked `fence_mesh`), where people stand |
| barrier | `vangrail_end` | 2 × 0.5 m, 1.25 m | rounded end pieces curling away from the road at both ends |
| board | `chevron_left` / `chevron_right` | 1.6 × 0.4 m face, 1.3 m | Zeichen 625: red chevrons on white, round the outside of a bend, pointing the way it turns |
| board | `km_marker` | 0.6 × 0.8 m face, 1.8 m | the kilometre board; `text` `km<N>` picks `T_marker_km<N>` on the `board_marker` slot (`board/markers/km1..20.png`) |
| board | `de_curve_left` / `de_curve_right` / `de_danger` | 0.9 m triangle on a 2.6 m post | Zeichen 103 / 105 / 101 |
| board | `de_overtake_left` | 1.5 × 1.0 m | "Überholen nur links", the Touristenfahrten board |
| building | `castle_ruin` | 70 × 56 m, 32 m | Burg Nürburg: round keep with battlements, inner ward, broken outer wall, gate tower, on a basalt knoll reaching 6 m below the pivot; centred on its footprint |

The signs are `board` kind and read by an oncoming driver, so the importer
turns them up the course like a braking marker (`ApexProps::FacesUpCourse`).

### 10. Road decals (graffiti)

Not meshes: pictures painted **on the road**, the Nordschleife's fan
graffiti first. A decal is an `.ats` `decals` entry (station, length along
the road, lateral centre and width, `image` = `"<set>/<name>"`); `ats-export`
bakes it as a road-hugging grid (1 m columns, 0.5 m rows, lifted 5 cm, so it
follows the crown, camber and grade like paint) under the material key
`decal_<set>_<name>`, family `decal`, with UVs spanning the picture once. The
importer draws it with `M_ApexDecal` (masked by the PNG's alpha, matte, the
texture's colour dimmed to a sprayed coat). A decal whose texture is not
imported is left out of the level with a warning, never drawn as a white
slab.

| set | source | Unreal texture |
| --- | --- | --- |
| `graffiti` | `content/props/decal/graffiti/<name>.png`, 1024 × 512 RGBA | `/Game/Props/decal/Graffiti/T_graffiti_<name>` |

The pictures are drawn as a driver sees them: top = far end, left = left of
the road. `ats-export` stretches them along the road (7 m across by 14-20 m
along is typical), which is how fans paint for an eye a metre off the
tarmac. `scripts/content/props/gen_graffiti.py` paints the shipped set (30
invented slogans, names, hearts, arrows and flags; stroke letters, not a
font, so the output is the same on every machine; `--sheet` writes a
contact sheet to `_preview/`). Hand-made art is any PNG of that size in
the folder: white or coloured paint, transparent elsewhere — painted in
Blender's texture paint mode on a 2:1 plane, or in any image editor.

```bash
python scripts/content/props/gen_graffiti.py --sheet
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject -run=ApexPropImport -kind=decal
```

`-all` imports them too. `decal` is not a prop kind (there are no GLBs), so
the groomer, `props::KIT` and the kind tables know nothing of it.

### 11. Marina Bay ("Mandarina Bay")

Landmarks and civic buildings for the Singapore street circuit (research and
placement in `docs/content/MARINA_BAY.md`). Built by
`scripts/content/props/build_marina_bay_{a1,a2,a3,b1,c1}.py` on top of
`marina_common.py`: facade textures are generated (curtain-wall tile + a
warm lit-window emissive map), everything else uses the kit generators.
Originals only: no brand marks, and the Merlion is deliberately **not**
modelled (trademark). All sizes are real-world; windows are texture, not
geometry.

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| building | `landmark_twin_domes` | 188 × 85 m, 36 m | two spiked-sunshade domes on glazed drums, link foyer, entrance canopy; centred, front on -Y | P1 |
| attraction | `landmark_big_wheel_xl` | 150 m wheel, hub 90 m, 165 m tall, 150 × 37 m | 28 capsules rigid to the `rotor` child (hub origin), one-sided A-frame, boarding platform; night slots `ferris_lights`, `ferris_lights_rim`, `ferris_lights_hub` — **done** | P1 |
| building | `building_wheel_terminal` | 64.5 × 31 m, 24 m | glazed two-storey terminal with fins and a ticket hall tail | P1 |
| building | `building_pit_street` | 120.8 × 33.5 m, 24.6 m | pit-lane side on -Y: 20 service bays under a cantilevered paddock club, glazed hospitality band, set-back terrace club, roof masts | P1 |
| building | `building_colonnade_hotel` | 97 × 60 m, 28 m | neo-classical hotel, two-storey Doric colonnade, central pediment | P1 |
| building | `building_domed_court` | 81 × 58 m, 28 m | hexastyle portico, drum and copper dome with lantern | P1 |
| building | `building_colonnade_civic` | 97 × 51 m, 23 m | giant-order colonnade of 18 columns, pediment, balustrade | P1 |
| building | `building_clock_tower_hall` | 63 × 45 m, 56 m | gabled hall wings, 40 m clock tower with four faces and a copper pyramid | P1 |
| building | `building_five_towers` | 144 × 73 m, 164 m | podium + five chamfered glass towers (58–160 m) with white belts | P1 |
| misc | `fountain_basin` | 48 m dia, 10.4 m | round pool, bronze ring on a dais, 28 jets; places at the base of the five towers | P1 |
| building | `building_club_pavilion` | 48 × 30 m, 22.5 m | white colonial pavilion, two-storey verandah on -Y, hipped tile roof, turret | P1 |
| building | `building_gothic_church` | 41 × 53 m, 61 m | nave + transept, west tower with octagonal spire and pinnacles | P2 |
| building | `building_deco_theatre` | 45 × 35 m, 42 m | three-step art-deco massing, vertical fins, marquee, neon bands (`mb_lit_panel`) | P2 |
| misc | `monument_four_columns` | 26 × 26 m, 66 m | four inward-leaning columns on a stepped plinth | P1 |
| bridge | `bridge_arch_steel` | 30.6 m long, 12.9 m | **road runs on it**: two through-arches, hangers, bracing, corner pylons; tile along the road for longer crossings | P1 |
| bridge | `bridge_deck_wide` | 40 m long, 9.5 m | **road runs on it**: fascia, parapets, footways, twin-arm lamps, end pylons | P1 |
| bridge | `viaduct_deck` | 26 m long, 13.8 m, 9.6 m clearance | **road runs under it**: box girder, parapets, four pier columns; scales with `span_m` | P1 |
| attraction | `bridge_double_helix` | 118 × 9 m, 8.8 m | footbridge with outer/inner counter-rotating helices, LED edge strips; pushed clear (not a road crossing) | P2 |
| attraction | `landmark_three_towers_skypark` | 358 × 96 m, 200 m | three leaning-slab towers, retail podium, ship-shaped sky deck with pool and gardens; ~460 m from the road, use as backdrop | P2 |
| attraction | `landmark_lotus_museum` | 54 × 50 m, 40 m | ten outer and ten inner cupped petals over a glazed ring podium | P2 |

| barrier | `quay_rail_4m` | 4 m, 1.3 m | waterfront balustrade: galvanised posts, top rail, two rails and balusters on a concrete kerb plinth; tiles end to end — **done** | P1 |
| fence | `catch_fence_6m` | 4 × 6.5 m | tall catch fence for street circuits: `fence_mesh` on both faces, three posts, top outriggers leaning to the road with two strands — **done** | P1 |
| sign | `traffic_signal_pole` | 3.6 m deep, 6.4 m | mast-arm signal: two three-aspect heads facing both ways along the road (`signal_red/amber/green`, emissive), pedestrian head, push button, street blade; arm reaches the road on -Y — **done** | P1 |
| sign | `road_sign_post` | 2.5 × 3.6 m | green direction sign on two posts with white border, arrow and text strips (blank, no text slot) — **done** | P2 |
| bridge | `sign_gantry` | 19.2 m span, 7.5 m | **road runs under it**: galvanised box-truss on two posts with two green panels, catwalk and lights; scales with `span_m` — **done** | P1 |
| bridge | `linkbridge_covered` | 4.2 m wide, 19.8 m span, 8.6 m | **road runs under it**: glazed covered walkway on a curved roof at 5.2 m, two columns off the verges; scales with `span_m` — **done** | P1 |
| misc | `bus_shelter` | 7.5 × 2.8 m, 3.5 m | roof canopy, glass back and ends, bench, lit advert case, stop pole; open to the road on -Y — **done** | P2 |
| misc | `station_entrance` | 7 × 5 m, 4.4 m | underground-station entrance: stair well with parapets and handrails under a glazed gable canopy, line-colour pylon, map case, planters — **done** | P2 |
| misc | `carpark_entrance` | 8.3 × 8.6 m, 4.6 m | ramp portal with sloping retaining walls, headroom bar, boom barrier, ticket booth, blue P board — **done** | P2 |
| misc | `monument_obelisk` | 7 × 7 m, 15.5 m | stepped plinth, plaque, tapered shaft with pyramid cap — **done** | P2 |
| misc | `monument_pagoda` | 10.4 × 9 m, 10.2 m | hexagonal memorial pavilion: stepped base, six red columns, two-tier tiled roof, stele inside — **done** | P2 |
| misc | `monument_statue_plinth` | 6 × 6 m, 8.5 m | stepped stone plinth with plaque and a generic bronze standing figure (not a likeness) — **done** | P2 |
| tree | `raintree_l` | 25.6 × 23.2 m, 19.2 m | umbrella-crowned rain tree: 9.5 m clear trunk, spreading limbs, wide flat leaf-card crown on `tree_card_broadleaf` — **done** | P1 |
| light | `lamp_arm_twin` | 6.9 m across, 9.6 m | pole with a swan-neck arm to each side, `floodlight_lamp` lens slot — **done** | P1 |


**Start/finish straight** (brief: `docs/content/MARINA_BAY_START_FINISH.md`). Built by
`scripts/content/props/build_marina_bay_d1.py` (`ASSET = "all"` builds, exports every GLB and
saves `_batches/marina_bay_d1.blend`); decals and garage plates by `gen_marina_decals.py`
(runs inside Blender, no PIL).

| kind | asset | size | notes | prio |
| --- | --- | --- | --- | --- |
| building | `pit_building_roofdeck` | 200 × 34 m, 26 m | 33 garage doors at 6 m pitch under a glazed band, two terraces stepped back from the lane (-Y), black roof deck with parapet, stair towers at both ends, roof masts; road-facing pivot. Replaces the generated garage block — **done** | P1 |
| board | `roof_wordmark_block` | 40 × 0.6 m, 7.5 m | 40 × 6 m panel on stilts, `board_brand` face (2 repeats, brand from `text`), alternating `mb_lit_panel` / `mb_red` rim; centred — **done** | P2 |
| misc | `stair_zigzag_scaffold` | 14 × 5 m, 8 m | two 20-step flights (3.5 m each) on tube scaffold, landings, rails, `step_light_orange` nosings; centred — **done** | P3 |
| grandstand | `street_stand_tier_10m` / `_crowd` | 10 × 26 m, 17 m | 18 rows (1.05 m tread, 0.72 m rise, first row 2.2 m), central aisle, scaffold under the rake, rear service deck at 15.16 m with balustrade and wind screen; fascia LED lines `stand_led_blue` / `stand_led_green`; crowd cards as `bay_10m_crowd` — **done** | P1 |
| grandstand | `street_stand_tier_end` | 4 × 26 m, 17 m | end cap symmetric about x = 0, place at ±(L/2 + 2): raked access stair with rails both sides, rear stair tower (five flights) roofed at 17 m, same LED fascia — **done** | P1 |
| grandstand | `street_stand_tier_10m_roof` / `_crowd` | 10 × 26 m, 22 m | as the bay plus a cantilever truss canopy (22 m at the back, 20.6 m at the front edge), lit soffit strips (`mb_lit_panel`), lamp bars (`floodlight_lamp`), canopy fascia with both LED lines — **done** | P2 |
| grandstand | `street_stand_deck_10m` | 10 × 14 m, 9 m | glazed ground floor, balcony slab with glass balustrade and one row of `seat_b`, glazed upper floor, `stand_led_blue` roof fascia — **done** | P3 |
| bridge | `banner_gantry` | 20 m span (legs at y = ±10, skewed 3 m in X), 7 m | box-truss legs and beam, 16 × 1.7 m double-sided banner (`board_brand`, 2 repeats) at 4.4–6.1 m, `led_panel` underside strip; scales with `span_m` — **done** | P1 |
| misc | `finish_tower_scaffold` | 4 × 4 m, 7 m | scaffold base, glazed cabin at 3.6 m, 14-step stair, roof camera, `flag_cloth` flag; centred — **done** | P2 |
| light | `lamp_globe_pole` | 0.6 × 0.6 m, 6.5 m | 0.8 m `globe_lamp` sphere on a tapered pole — **done** | P1 |
| light | `lamp_balloon_tether` / `_orange` | 1.6 m balloon, 9 m | anchor block, tether, `balloon_lamp_white` / `balloon_lamp_orange` — **done** | P2 |
| light | `pit_light_truss_6m` | 6 × 1.1 m, 1.2 m | tiles on 6 m; **pivot at the bottom centre** (place at mounting height); four `floodlight_lamp` bars underneath — **done** | P2 |
| board | `led_ribbon_3m` | 3 × 0.15 m, 0.9 m | `led_panel` face with UV 0..1 (brand from `text`); pivot at its base, sits on Tecpro/walls — **done** | P2 |
| fence | `catch_fence_post_lit_6m` | 4 × 0.6 m, 6.5 m | as `catch_fence_6m` with the top metre leaning 0.3 m to the road and an `mb_lit_panel` rail along the head — **done** | P3 |
| barrier | `spectator_handrail_4m` | 4 × 0.1 m, 1.1 m | `grandstand_rail`, posts at x = ±1 (2 m pitch across tiles) — **done** | P3 |
| sign | `pit_entry_board` | 3 × 0.3 m, 4.5 m | blue board at 3.2–4.4 m on two posts, "PIT" in geometry, arrow — **done** | P3 |
| sign | `garage_number_board` | 1.5 × 0.1 m, 0.8 m | `board_marker` face, UV 0..1, `text` 1–24 → `board/markers/<n>.png` (generated) — **done** | P3 |
| pit | `pit_wall_gantry_6m` | 6 × 2 m, 3.2 m | 1.1 m wall, crew deck, desk with four `led_screen` monitors and stools, hung timing screen, headset rack, `tent_colour` awning with `board_brand` fascia; road-facing pivot — **done** | P2 |
| attraction | `marquee_peak_12m` | 12 × 8 m, 6 m | two pyramid peaks, `tent_white` walls on three sides with `mb_lit_panel` panels, open front on -Y; centred — **done** | P2 |
| attraction | `led_wall_stage_16m` | 16 × 6 m, 9 m | 1.2 m deck, 14.6 × 6.7 m `led_screen` wall (UV 0..1), box-truss goalposts with lamps, speaker stacks; road-facing pivot — **done** | P3 |
| misc | `walkway_planter_4m` | 4 × 1.2 m, 1.2 m | stone planter, clipped shrubs and a rain-tree sapling — **done** | P3 |

Decals (`content/props/decal/marina/`, same convention as the graffiti set): `edge_yellow_blue.png`
(+ mirrored `_r`), `pit_exit_blue_green.png`, `wall_base_brand.png`, `finish_chequer_wide.png`
(opaque), `pit_lane_digits.png` (4 × 4 atlas: 0–9, roundels 60/80/100, PIT, arrow, blank).
Client wiring (2026-10-09, compiled and unit-tested, not seen in game): the six emissive slots
`stand_led_blue`/`_green` (120 nits), `globe_lamp` (150), `balloon_lamp_white`/`_orange` (100)
and `step_light_orange` (12) are in the night table (`ApexProps::NightGlowOf`, peaks in one
place). Stands now run `ApplyAuthoredSlots` on their rows, so the roofs' `mb_lit_panel` and
`floodlight_lamp` and the LED lines are driven too; `led_panel`, `led_screen` and
`floodlight_lamp` on the other new props use the existing emissive paths. The street stand
family is straight-only: `LayoutGrandstand("street_stand_tier_10m[_roof]")` gives
`round(L/10)` bays plus `street_stand_tier_end` caps at +-(L/2 + 2) m, never a wedge, with the
`_crowd` twins from `CrowdVariant` (the cap and `street_stand_deck_10m` have none).
`street_stand_deck_10m` tiles at 10 m with no caps (the 26 m x 17 m cap does not fit a 14 m
deck). The `marina` decal set is in `DecalSets()` (`/Game/Props/decal/Marina/T_marina_<name>`);
re-run `ApexPropImport -kind=decal`. Garage plates: `garage_number_board` text 1-24 uses the
`board_marker` rule and `T_marker_<n>`, imported with the `board` kind (or `-all`), not `sign`.

Night pass additions: the textured facade slots `mb_glass_blue/teal/bronze/grey/clear`,
`mb_classic`, `mb_colonial`, `mb_deco` carry a lit-window emissive texture
(`EmissiveStrength` 0 by day, ~1 at night, like `pit_glass`), and
`mb_lit_panel` is a plain emissive; the signal heads use `signal_red/amber/green`.

Wired in the Unreal client (2026-10-09; built and unit-tested, **not yet seen in the
running game**): `ApexProps::NightGlowOf` lists the night slots (the `mb_*` windows,
`pit_glass`, `pit_interior`, `ferris_lights*`, `mb_lit_panel`). The builder tags a
component with one `ApexNightGlow`; the race director makes the slot's material a
dynamic instance and `ApplyNightGlow` sets it from the sky's `WindowGlow` (0 by day, 1
at night, smooth through twilight, `ApexSky::WindowGlowAt`): the imported material's
`EmissiveFactor` times a per-slot peak (25 nits windows, 15 `pit_interior`, 60-80 the
wheel's strips), plus the `EmissiveStrength` scalar (0..1) — the Interchange glTF parent
ignores that scalar (see `AApexRaceCarActor`), so the factor is what actually lights the
slot. `mb_lit_panel` is a builder-made `M_ApexEmissive` instance (dark by day, 40 at
night via `EmissiveStrength`); `signal_red/amber/green` are `M_ApexEmissive` instances
lit all day (1200). The peak levels are first guesses, to tune by eye at night. A sky that
is day for the whole session touches none of these materials. Needs `ApexPropImport`
re-run only for the rotor below; the slots need no re-import.

Street lighting (2026-10-09, compiled and unit-tested, not seen in game): after dark every `lamp_arm_twin` (two shadowless 3300 K spots, 80 000 lm, 110 deg, 28 m, at +-3.4 m / 9.2 m, leaning toward the road), `lamp_globe_pole` (point, 30 000 lm, 12 m), `lamp_balloon_tether(_orange)` (point, 60 000 lm, 18 m), `pit_light_truss_6m` (one downward spot, 20 000 lm, 16 m) and the older `floodlight_tower` / `lamp_post` get a light component (`ApexLights::SpecsFor`, `Race/ApexStreetLights.h`, one table). All are spawned once and hidden but for the nearest `apexsim.lights.Max` (90) within `apexsim.lights.Range` (260 m) of the camera, re-chosen twice a second with 20% hysteresis (`ApexLights::Select`); the emissive heads stay on regardless. Units follow `lamp_post` (12 000 lm); levels need tuning by eye.

Rotor: `ApexProps::FindRotorSpec` lists the assets with a `rotor` node — `ferris_wheel`
(0.5 rpm) and `landmark_big_wheel_xl` (1/30 rpm, one turn in 30 minutes, hub offset
(0, 800, 9000) cm from the node's `[0, 90, 8]`). Both import as `SM_<asset>` +
`SM_<asset>_rotor` and spawn as an `AApexRotorActor` (re-run
`-run=ApexPropImport -asset=attraction/landmark_big_wheel_xl`).

## Unreal import notes

- Every asset in the tables above is authored (**done**); the recipe fallback
  only matters for unknown asset keys now.

- Masked materials (`fence_mesh`, `crowd_cards`) arrive as glTF `MASK` +
  `doubleSided`; the importer must make them Masked and two-sided or they render
  as opaque black cards (which is what happened to the first, alpha-card trees).
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
- **Masked card slots** `tree_card_*` and `scatter_*` (every tree but the palms,
  grass) are alpha MASK + two-sided like `fence_mesh`; `ApexPropLibrary::IsMaskedSlot`
  covers them. A card's front faces out of its crown so the two-sided
  back-face normal flip agrees with the crown normals.
- **Alpha coverage.** The importer turns on `bDoScaleMipsForAlphaCoverage`
  (threshold 0.5, the glTF clip value) for every texture a masked material
  uses; without it the box-filtered mips average the leaves away and a tree
  thins to its twigs a hundred metres out. The card textures also bleed
  their colour into the clear texels, so no mip has a black fringe.
- **`forest_billboard`** (the far woods) is masked but *one-sided*: each
  billboard is two cards back to back with sky-leaning normals, since the
  two-sided flip turned the back half of every far wood black. Not a
  `tree_card_*` name, or the importer would force it two-sided.
- **Trees are built together.** `tree_bark` and the card slots are shared by
  name across the kind on import (the first GLB brings them in), so run
  `build_trees.py`, `build_near_trees.py` and the two impostors of
  `build_batch_f_terrain.py` together after changing `card_trees.py`, then
  `-run=ApexPropImport -kind=tree`.
- **Text slots.** `corner_sign` carries the corner name on its blank
  `board_text` face; the importer (`ApexProps::HasTextFace`) spawns it as its
  own actor rather than an instance and hangs a text component just off the
  face at 2.2 m, sized to fit the 2.4 m board. `hillside_letters` is geometry.
- **Baked textures.** The whole track-edge, tyre-wall, grandstand (every bay,
  cap and `_crowd` variant), pit and statue kit carries albedo + roughness +
  normal maps (512², tileable, from `scripts/content/props/apex_tex.py`, saved under
  `content/props/_textures/`) on its existing slots; `scripts/content/props/retexture_kit.py`
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
