# Trackside prop kit

The scenery every circuit is dressed from: an authored kit of GLBs,
`content/props/<kind>/<asset>.glb`, imported once into `/Game/Props` and
placed per track from the `.ats` scene's `props`. A prop in a scene is a
`kind` (coarse category: drives grooming, instancing, facing) plus an `asset`
key (the mesh), with a position, yaw, scale and optional `text` (a brand,
marker number, flag code or corner name) and, for stands and bridges,
`length_m` / `radius_m` / `span_m` written by the exporter. Props are
client-only visuals: the server never reads them, except the barriers and
buildings the bake turns into `walls.msgpack` (see
[track-pipeline.md](track-pipeline.md)).

## Code

- `content/props/<kind>/<asset>.{glb,blend}`: the kit. `board/brands/*.png`,
  `board/markers/*.png`, `sign/flags/*.png`, `decal/<set>/*.png`: loose
  textures. `_batches/` (scenes spanning kinds), `_textures/` (baked maps),
  `_preview/` (renders) are not kinds.
- `scripts/content/props/`: `apex_props.py` (bmesh builders, UVs, GLB export,
  preview render), `apex_tex.py` (numpy tileable PBR maps and card textures,
  `KIT_SLOTS`, `kit_material`), the batch builders (`build_barriers2.py`,
  `build_trees.py`, `build_near_trees.py`, `card_trees.py`,
  `build_batch_e_kit.py`, `build_batch_f_terrain.py`, `build_bull_statue.py`,
  `build_nordschleife_kit.py`, `build_marina_bay_{a1,a2,a3,b1,c1,d1}.py` on
  `marina_common.py`), `gen_brands.py`, `gen_graffiti.py`,
  `gen_marina_decals.py`, `retexture_kit.py`, `skyline_night.py`.
- `track-editor/core/src/props.rs`: `props::KIT`, the catalogue (kind, key,
  footprint per asset, default first), `assets_for`, `default_asset`, `find`,
  `resolve`. **The source of truth for which assets exist and how big they
  are.**
- `track-editor/core/src/ats.rs` (`PropKind`, `Dressing`), `groom.rs`
  (per-kind placement), `ue_export.rs` (`bake_props`, the pit complex).
- `game-unreal/Source/ApexTrackEditor/Private/ApexPropImportCommandlet.cpp`:
  the import.
- `game-unreal/Source/ApexSim/Public/Track/ApexPropLibrary.h` /
  `Private/Track/ApexPropLibrary.cpp` (`ApexProps::`): kind table, defaults,
  aliases, slot rules, variants, stand layout, rotor table, night glow.
- `game-unreal/Source/ApexSim/Private/Track/ApexTrackSceneBuilder.cpp`
  (`FApexTrackSceneBuilder::ResolveProp`, placement, slot overrides,
  `SpawnStartLights`); `Race/ApexPropActors.h` (`AApexSkyDriftActor`,
  `AApexRotorActor`); `Race/ApexStreetLights.h` (`ApexLights::`);
  `AApexRaceDirector::ApplyNightGlow`.

## Kinds

| Kind | Default asset | Instanced | Nanite | Faces road | Groomer |
|---|---|---|---|---|---|
| `barrier` | `armco_4m` | yes | | yes | owned by the barrier pass: laid as runs along the decided line |
| `tire_wall` | `tires_4m` | yes | | yes | as barrier |
| `board` | `hoarding_3m` | yes | | yes (some up-course) | snaps onto the nearest barrier run |
| `sign` | `marshal_post` | yes | | yes (some up-course) | pushed clear |
| `fence` | `mesh_4m` | yes | | yes | laid `FENCE_BEHIND_WALL_M` (1.5 m) behind the wall line |
| `grandstand` | `bay_10m` | | yes | yes | pushed clear |
| `building` | `clubhouse` | | yes | yes | pushed clear |
| `pit` | `garage_6m` | | yes | yes | left alone (the exporter owns the pit complex) |
| `bridge` | `start_gantry` | | yes | no | never pushed; seated at road height |
| `light` | `floodlight_tower` | yes | | yes | pushed clear |
| `tree` | `broadleaf_m` | yes | | no | pushed clear, seated |
| `vehicle` | `car_a` | yes | | no | pushed by footprint |
| `attraction` | `tent_6m` | | yes | only `video_screen`, `fanzone_stage`, `tent_6m` | pushed by footprint |
| `sky` | `blimp` | no (one actor each) | | no | never seated: `z` is altitude |
| `cone` | none | yes | | no | |
| `misc` | `bollard` | yes | | no | pushed clear |

The Unreal table is `kKinds` in `ApexPropLibrary.cpp`; `props.rs`'s first
entry per kind must match it (`defaults_match_the_importer`). Who owns which
props (dress, groom, export) is in [track-pipeline.md](track-pipeline.md).

**Up-course boards** (`ApexProps::FacesUpCourse`): `board/braking_marker`,
`board/light_panel`, the Nordschleife signs (`chevron_*`, `km_marker`,
`de_*`) and the pit lane's `sign/pit_exit_light` and `sign/pit_speed_limit`
are read by a driver coming down the road. Their authored +Y front is yawed
+90 degrees to look back up the course and they are never flipped by side.

## Catalogue

`props::KIT` lists every asset with its footprint (length along the road x
depth x height, metres). By family:

- **Track edge**: armco (`armco_4m`, `_fence`, `armco_end`), concrete
  (`concrete_4m`, `_rail`, `concrete_end`), Tecpro (`tecpro_2m`,
  `tecpro_corner`), tyres (`tires_4m`, `tires_corner`), `sausage_kerb_2m`
  (baked as a `kerb` wall: the sim jolts, never stops, a car), the
  Nordschleife `vangrail_*`, street `quay_rail_4m`, `spectator_handrail_4m`;
  boards (`hoarding_3m`, `hoarding_6m`, `braking_marker`, `light_panel`,
  `corner_sign`, `led_ribbon_3m`, `roof_wordmark_block`, German signs); signs
  (`marshal_post`, `flag_pole`, pit signs, `garage_number_board`, street
  signals); fences (`mesh_4m`, `catch_fence_6m`, `wood_4m`, `hedge_4m` ...).
- **Overhead**: bridges the road runs **under** (`start_gantry`,
  `truss_bridge`, `tyre_bridge`, `timing_gantry`, `span_building`,
  `viaduct_deck`, `sign_gantry`, `linkbridge_covered`, `banner_gantry`) and
  **on** (`bridge_arch_steel`, `bridge_deck_wide`); lights
  (`floodlight_tower`, `lamp_post`, `lamp_arm_twin`, `lamp_globe_pole`,
  `lamp_balloon_tether[_orange]`, `pit_light_truss_6m`).
- **Pit complex**: `garage_6m[_closed]`, `garage_end`, `pit_wall_6m`,
  `pit_wall_plain_6m`, `pit_wall_gantry_6m`, `box_kit`; building
  `pit_building_roofdeck` for street circuits.
- **Spectators**: stand bays (below), `scaffold_10m`, `banking_seats`, the
  street stand family; attractions (`video_screen`, `ferris_wheel`,
  `camera_tower`, `fanzone_stage`, `food_stall_6m`, `ticket_gate`, tents,
  `portaloo_row`).
- **Buildings**: paddock (`hospitality_3f`, `media_centre`, `control_tower`,
  `clubhouse`, `podium`), `observation_tower`, Styrian village houses, barn
  and chapel, `castle_ruin`, the `skyline_*` city backdrop, the Marina Bay
  landmarks.
- **Landscape**: card trees (`broadleaf_s/m/l`, `conifer_m/l`, `poplar`,
  `bush_cluster`, near-LOD `*_near`, `raintree_l`, palms), far-wood
  billboards (`forest_impostor[_conifer]`), ground scatter (`grass_clump`,
  `wildflower_clump`, `scrub_clump`, `rock_cluster`), vehicles, misc
  furniture, `power_pylon`, `bull_statue`.
- **Sky**: `blimp`, `balloon`, `helicopter`.

Variants the builder picks by itself are not in `KIT` and not named by any
scene: `_crowd` stands, `_autumn` trees, the wedge bays and the end caps
(`every_kit_file_is_catalogued` exempts them).

Brands are fictional and data, not meshes: `piretti`, `rolux`, `apexsim`,
`velocet`, `kronos`, `hexon`, `northwind`, `brix` (`gen_brands.py` writes
`board/brands/*.png` at 3:1 and `board/markers/*.png` at 3:4). Add a brand
there before using it as a `text`.

## Authoring conventions

- **Units and axes**: metres, Z up. Modelled in Blender with +X along the
  track and the **road on -Y**. glTF `(x, y, z)` lands in Unreal as
  `(x, z, y)`, so the road arrives on local **+Y**, the side the generated
  recipes use; no extra yaw is applied to authored assets.
- **Pivots** on the ground. The deep road-facing kinds (stand bays and caps,
  buildings, garages, pit walls, the stage, video screen, portaloos) have it
  on the **road-facing edge**, the footprint reaching away from the road;
  `props.rs` footprints and the groomer assume that. Thin modules (barriers,
  tyre walls, fences, boards) and free-standing pieces (`control_tower`,
  `camera_tower`, `ferris_wheel`, `tent_6m`, `observation_tower`,
  `skyline_*`, `castle_ruin`, `bull_statue`, the Marina Bay landmarks) are
  centred on their footprint. Bridges: the road centre on the ground, span
  across local Y. Sky props: origin at the hull centre. `pit_light_truss_6m`
  pivots at its bottom centre (placed at mounting height).
- **Modules** tile on 2 / 4 / 6 m multiples. Corner and end pieces
  (`armco_end`, `tires_corner`, `tecpro_corner`, `concrete_end`) tile onto
  the end of a run at local x = -1.
- **Material slot names are the API.** They are the glTF material names and
  everything at runtime keys on them (below); the import shares a material
  by name across a kind, so the same slot name must mean the same material
  in every GLB of the kind.
- **Masked cards** (`fence_mesh`, `crowd_cards`, `tree_card_*`, `scatter_*`,
  `tree_foliage*`) are exported as glTF `MASK` + `doubleSided`; a card's
  front faces out of its crown. `forest_billboard` is masked but one-sided
  (two cards back to back), and must not be named `tree_card_*`.
- **Baked textures**: new builders take materials from
  `apex_tex.kit_material`, which returns the baked material for a slot in
  `apex_tex.KIT_SLOTS` and a flat one otherwise. `retexture_kit.py`
  re-applies the baked maps to existing GLBs without touching geometry or
  slot names. Add a slot to `KIT_SLOTS` to bake it everywhere.
- **Building a batch**: each batch script takes `ASSET` (`"all"` or one key)
  and exports on run, inside Blender or headless on `bpy`. The tree kit
  shares `tree_bark` and the card slots by name, so after changing
  `card_trees.py` run `build_trees.py`, `build_near_trees.py` and
  `build_batch_f_terrain.py` together, then import `-kind=tree`.
- **A new asset** needs a GLB in `content/props/<kind>/`, a `KIT` row in
  `props.rs` (or `every_kit_file_is_catalogued` fails), and an import.

## Import (`ApexPropImport`)

Run once, and whenever a GLB changes, with the editor closed:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject -run=ApexPropImport -all
#   or -kind=barrier,board / -asset=barrier/armco_4m / -kind=decal; -dryrun, -source=<dir>, -dest=/Game/...
./scripts/build_track_levels.ps1 -ImportProps     # the same, before the track bake
```

`initialize_content.ps1` runs `-all` when any kit GLB has no mesh;
`build_release.ps1 -SkipProps` still checks `/Game/Props` holds meshes.

What it does, per GLB, through Interchange (the engine's glTF stack):

- One combined mesh at `/Game/Props/<kind>/SM_<asset>`, authored normals
  kept, Nanite on the Nanite kinds, box collision on everything but `tree`
  and `sky` (only the cameras' traces use it; nothing on the client
  collides with a car). An asset with a
  `rotor` node (`ferris_wheel`, `landmark_big_wheel_xl`) imports as
  `SM_<asset>` plus `SM_<asset>_rotor`.
- Materials and textures under `/Game/Props/<kind>/Materials`, shared by
  name across the kind (the first GLB to bring a slot in owns it). A
  kind-wide run clears that folder first.
- Every run is a fresh import: the target mesh package is deleted from disk
  and the registry rescanned first, because reimporting over an existing mesh
  keeps its old slots and makes no materials.
- The glTF materials come in as instances of the engine's glTF parent, which
  has neither the Nanite nor the instanced-mesh usage flag; a cooked build
  does not set those on the fly, so each instance is re-parented onto a
  flagged copy under `/Game/Props/_Parents`.
- Masked textures get `bDoScaleMipsForAlphaCoverage` (threshold 0.5), or the
  mips average the leaves away and trees thin to twigs at distance.
- The loose PNGs come with their kind: `T_brand_<name>` and `T_marker_<n>`
  under `/Game/Props/board/{Brands,Markers}`, `T_flag_<cc>` under
  `/Game/Props/sign/Flags`; `-kind=decal` (or `-all`) imports the decal sets
  (`ApexProps::DecalSets`: `graffiti`, `marina`) as
  `/Game/Props/decal/<Set>/T_<set>_<name>`.

Packaging cooks `/Game/Props` by path (`bCookAll`).

## Resolution and placement

`FApexTrackSceneBuilder::ResolveProp` turns each export prop into a mesh:

1. **Alias** (`ApexProps::ResolveAlias`, `kAliases`): pre-kit keys still in
   scenes map onto the kit, possibly changing the kind: `tree_generic`,
   `armco_generic`, `tire_wall_generic`, `sign/board_200m|100m|50m` ->
   `board/braking_marker` with the number as `text`, `grandstand_main` ->
   `bay_10m_large_roof`, `grandstand_corner` -> `bay_10m_roof`,
   `building/pit_garage` -> `pit/garage_6m`. Data, so no `.ats` is migrated;
   `props::resolve` mirrors it for the editor.
2. `/Game/Props/<kind>/SM_<asset>`, else the kind's default asset.
3. In autumn, the `_autumn` twin where one exists (`AutumnVariant`:
   broadleaf, poplar, bush; conifers stay).
4. Nothing authored: the original kind's generated stand-in recipe, else a
   placeholder box. A track still builds with `/Game/Props` empty.

Instanced kinds go into one HISM per resolved (kind, asset, text), actor
`Props_<kind>_<asset>[_<text>]`; a `corner_sign` with text gets its own actor
with a text component on its blank `board_text` face
(`ApexProps::HasTextFace`). Every prop actor is tagged `ApexProp`, which the
racing line's ground snap and the TV camera's traces use to ignore bridge
decks and garage roofs. Face-road kinds are flipped 180 degrees when the road
turns out to be on their local -Y.

**Text to material** (`ApplyAuthoredSlots`):

- a brand `text` overrides the brand slots (`board_brand`, `bridge_brand`,
  `bridge_brand_*`, `pit_team_board`, `tyre_bridge_brand`, `blimp_brand`,
  `balloon_envelope`; `IsBrandSlot`) with an `M_ApexBrand` instance showing
  `T_brand_<text>`;
- `board_marker` takes `T_marker_<text>` (braking markers 50-200,
  `km_marker` `km<N>`, `garage_number_board` 1-24);
- `flag_cloth` takes `T_flag_<text>` (`nl`, `de`, ... `chequer`);
- unknown text keeps the imported default and logs once per track.

**Emissive slots** (`IsEmissiveSlot`): `gantry_lamp`, `led_panel`,
`led_screen`, `floodlight_lamp`, `pit_light_red` / `pit_light_green`,
`mb_lit_panel`, `signal_red/amber/green` get an `M_ApexEmissive` instance and
the component a tag `ApexEmissive_<slot>`. `led_panel` / `led_screen` glow at
a fixed level (no flag state is on the wire). The race director drives the
pit exit lamps from telemetry's pit flags (`UpdatePitExitLights`) and lights
the floodlight lamps after dark.

**Night glow** (`ApexProps::NightGlowOf`, `AApexRaceDirector::ApplyNightGlow`):
window and light-strip slots (`pit_glass`, `pit_interior`, the `mb_glass_*` /
`mb_*` facades, `ferris_lights*`, `stand_led_*`, `globe_lamp`,
`balloon_lamp_*`, `step_light_orange`, `mb_lit_panel`) are tagged
`ApexNightGlow`; the director scales their `EmissiveFactor` by a per-slot
peak times the sky's window glow (0 by day, 1 at night). The imported parent
ignores the `EmissiveStrength` scalar, so the factor is what lights them.
**Street lights** (`ApexLights::SpecsFor`, by mesh name): after dark
`floodlight_tower`, `lamp_post`, `lamp_arm_twin`, `lamp_globe_pole`,
`lamp_balloon_tether*` and `pit_light_truss_6m` get light components, of
which only the nearest `apexsim.lights.Max` within `apexsim.lights.Range` of
the camera are on (`ApexLights::Select`); the per-lamp figures are in
`Private/Race/ApexStreetLights.cpp`. The sky that switches all of this on is
[../server/conditions.md](../server/conditions.md).

**Dressing**: the `.ats` carries a scene-wide `dressing { season, spectators }`
(`summer` / `autumn`, default summer with spectators), exported as-is. With
spectators every stand bay becomes its `_crowd` twin (`CrowdVariant`; caps
have none); a missing variant falls back to the base mesh. The editor has a
Dressing panel and the MCP a `set_dressing` tool.

### Grandstands

One `grandstand` prop is a whole stand (`ApexProps::LayoutGrandstand`, under
one `Grandstand_<n>` actor with an HISM per asset):

- `n = round(length_m x scale / 10)` bays (`length_m` 30 when absent; `scale`
  multiplies the length) at a 10 m pitch, plus two end caps at
  `x = +-(n x 5 + 0.5)` (`end_cap`, or `end_cap_large` for the large family).
- The exporter writes `radius_m`, the signed bend radius at the stand
  (positive on the outside). For the `bay_10m` and `bay_10m_roof` families
  within 150 m the builder lays wedges: `_curve6` (front radius 95 m) or
  `_curve12` (48 m) outside, `_curve6_in` inside, `_roof` carried over. A
  wedge's side edges meet on the local Y axis at `Rf = 5 / tan(theta / 2)`;
  bay i is bay 0 turned about that point by `(i - (n-1)/2) x theta`, so
  neighbours share a side edge, and the caps take the outer bays' turn.
  The large family is straight only.
- `scaffold_10m`, `banking_seats` and `street_stand_deck_10m` are not bay
  families: the module is tiled at 10 m, no caps.
- The street stand (`street_stand_tier_10m[_roof]`) is straight only, with
  `street_stand_tier_end` caps at `+-(L/2 + 2)`.

### Bridges and the start gantry

Bridges are never flipped or pushed. Local Y is scaled by `span_m / 15`
(`ApexProps::BridgeSpanScale`: the meshes are authored for a 15 m road;
`span_m` is the road width at the station, from the exporter). The start
lights use `bridge/SM_start_gantry` as the `StartLights` root when it exists,
with five `ApexStartLight` lens components over the panel's upper lamp row
(`GantryLampX/PitchY/Z`), so the race director's countdown contract
(`ApexStartLights` / `ApexStartLight` tags) is unchanged; the generated
gantry is the fallback.

### Pit complex

`ats-export` generates it from the pit lane (`ue_export.rs`): a
`pit/garage_6m` per box on the far side of the lane at 6 m pitch with a
`pit/box_kit` on the same pivot, `garage_end` beyond each end, `pit_wall_6m`
on the road side over the box span and `pit_wall_plain_6m` over the rest of
the lane wherever there is an apron of `PIT_TAPER_WALL_MIN_M`; on a street
circuit `pit_building_roofdeck`, `pit_wall_gantry_6m` and
`garage_number_board`s instead. It drops `building/pit_garage` stand-ins
when it does. The lane itself is [../server/pit-lane.md](../server/pit-lane.md).

### Moving props

`sky` props spawn as `AApexSkyDriftActor` (drifting +-150 m along the heading
at 2 m/s with a slow yaw sway). An asset in `ApexProps::FindRotorSpec`
spawns as `AApexRotorActor`, its rotor mesh at the hub offset turning about
local Y: `ferris_wheel` at 0.5 rpm, `landmark_big_wheel_xl` at one turn in
30 minutes.

## Road decals

Not props: an `.ats` `decals` entry (station, length, lateral centre and
width, `image = "<set>/<name>"`) that `ats-export` bakes as a road-hugging
grid under material key `decal_<set>_<name>`, drawn with `M_ApexDecal`
(masked by the PNG's alpha). Source PNGs are
`content/props/decal/<set>/<name>.png` (graffiti 1024 x 512 RGBA, drawn as a
driver sees them: top is the far end); `gen_graffiti.py [--sheet]` paints the
shipped graffiti, `gen_marina_decals.py` the Marina Bay set. A decal whose
texture is not imported is left out with a warning. `decal` is not a
`PropKind`: the groomer and `KIT` know nothing of it.

## The editor side

`props::KIT` drives the track editor: the inspector offers a kind's keys in a
dropdown (a free-text `key` row stays for anything else), a new prop starts
as the kind's default, stand-ins are drawn at the asset's footprint
(`scene.rs`), and the groomer pushes by it. The MCP's `list_prop_assets`
lists it. Per-circuit asset choices (vangrail, German signs, street Tecpro)
are `circuit_style.rs` ([circuits.md](circuits.md)).

## Checking it

- `cargo test -p track-core props::` (the catalogue: unique keys, every entry
  has a GLB, every GLB has an entry, defaults match the importer, legacy keys
  resolve) and the groom and export tests (`cargo test -p track-core`).
- Unreal automation: `ApexSim.Props.*` (`Aliases`, `Kinds`, `StraightStand`,
  `WedgeStand`, `StreetStand`, `Rotor`, `NightGlow`), `ApexSim.Lights.*`.
- Look at it: build a circuit and park the shot camera on it
  (`-ApexCameraLookAt`, [../game/cameras.md](../game/cameras.md)): boards read
  correctly from the driving side, stands face the road, the pit wall's fence
  is on the track side.

## Traps

- **Slot names are shared by name across a kind on import.** Two GLBs of one
  kind with different materials under the same slot name get whichever was
  imported first; a renamed slot silently loses its brand, glow or mask
  treatment.
- **Import fresh, never over.** Reimporting a GLB over an existing mesh keeps
  the old slots; the commandlet deletes the package first for this reason.
- **Ownership is by asset.** The groomer and `ats-dress` recognise their own
  props by asset key ([track-pipeline.md](track-pipeline.md)); an asset they
  lay but do not list gets doubled on the next run.
- The deep kinds pivot on the road-facing edge. Anything that needs a
  footprint (the groomer, the wall bake) offsets them half their depth
  behind the pivot; a new deep asset centred on its footprint stands half in
  the road.
- The stadium family (`bay_10m_stadium_roof`, `bay_10m_stadium_curve6_roof`,
  `end_cap_stadium`) is in the kit but `LayoutGrandstand` does not know it:
  a stand naming it is laid as `bay_10m_roof` bays. No shipped scene uses it.
- `sign/hillside_letters` is geometry spelling a real circuit's name
  (default text in `build_batch_e_kit.py`); no scene places it, and placing
  it as is would put a trademark on screen ([naming.md](naming.md)).
