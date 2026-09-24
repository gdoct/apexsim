# Prop loader — work spec

> Status (2026-09-12): implemented. Stages 1–5 as written below (with the
> deviations noted in CLAUDE.md "Prop kit": grandstand `scale` multiplies the
> length so legacy stands become bay rows; `building/pit_garage` stand-ins
> are dropped when the exporter generates the pit complex; a `video_screen`
> faces the road). Stage 6: blimp drift and the ferris rotor are in
> (`Race/ApexPropActors.h`); `led_panel`/`led_screen` glow at a fixed level
> and are tagged `ApexEmissive_<slot>` — no flag state is on the wire yet,
> and the floodlights stay off.

Goal: the Unreal track import places the authored props from `content/props/`
(see `docs/PROPS.md`) instead of the generated box stand-ins, without
changing how tracks are authored. Everything below is ordered; stages 1–2
give visible results on their own, 3–6 add the pieces that need exporter
changes.

## Where things stand

- Props reach Unreal as `props[]` in `<Track>.uescene.json` (written by
  `track-editor/core/src/ue_export.rs::bake_props`): `kind`, `asset`, `location`
  (UE cm), `yaw_deg` (UE, already negated from the server's CCW radians),
  `scale`, `text`. Parsed by `ApexTrackSceneReader.cpp` into
  `FApexTrackProp` (`ApexTrackSceneData.h`).
- `ApexTrackAssetBuilder.cpp::BuildPropMeshes` generates one stand-in mesh
  per **kind** from `kPropRecipes` (tree, tire_wall, barrier, grandstand,
  building, sign) and `BuildLevel` places them: recipes flagged `bInstanced`
  go into one `UHierarchicalInstancedStaticMeshComponent` per kind (actor
  `Props_<kind>`, one custom-data float `InstanceJitter` for colour swing),
  the rest become an `AStaticMeshActor` each; kinds without a recipe get a
  scaled `/Engine/BasicShapes/Cube`. `bFaceRoad` recipes are yawed 180°
  when `RoadSideOf` finds the road on local -Y. `Prop.Asset` is parsed but
  never used for lookup — that is the hook.
- `SpawnStartLights` builds the gantry from the `start_gantry` recipe plus
  five `start_light` components (actor tagged `ApexStartLights`, emissive
  material instance `start_light` from `M_ApexEmissive`) that the race
  director drives during the countdown. Keep that contract.
- Asset keys already in the wild (`groom.rs`): `tree_generic`,
  `armco_generic`, and `sign` props `board_200m` / `board_100m` / `board_50m`
  with `text` "200"/"100"/"50". Existing `.ats` files must keep working, so
  these get aliases (stage 2) rather than a data migration.
- `PropKind` (`track-editor/core/src/ats.rs`) is `tree, sign, barrier,
  tire_wall, building, grandstand, light, cone, misc`. `groom.rs` keys its
  behaviour (re-laying barriers, pushing props clear, seating on terrain) on
  the kind.
- Engine: UE **5.8**, `ApexTrackEditor` is an Editor module (deps: Json,
  MeshDescription, StaticMeshDescription, UnrealEd, AssetRegistry).
- Assets: `content/props/<kind>/<asset>.glb` (+ `.blend` source), metres,
  Z-up in Blender, exported with glTF `+Y up`; textures embedded; masked
  materials exported as glTF `MASK`; brand textures as loose PNGs in
  `content/props/board/brands/<brand>.png` (3:1) and
  `board/markers/<n>.png` (3:4). Full list and conventions in
  `docs/PROPS.md`.

The server never sees props; they are client-only visuals. No prop needs
physics collision (a car that reaches a grandstand has already left the
server's track limits). Simple box collision on barriers is optional and
only matters for camera traces.

## Stage 1 — prop asset import (`ApexPropImport` commandlet)

New commandlet in `ApexTrackEditor`, run once (and whenever GLBs change),
not per track:

```
UnrealEditor-Cmd.exe game-unreal/ApexSim.uproject -run=ApexPropImport [-all | -kind=barrier | -asset=barrier/armco_4m] [-dest=/Game/Props]
```

- Walk `content/props/<kind>/*.glb` (skip `_tools`, `_preview`, `brands`,
  `markers`). Import each with Interchange (glTF) as a static mesh at
  `/Game/Props/<kind>/SM_<asset>`; materials/textures under
  `/Game/Props/<kind>/Materials`. Combine all primitives into one static
  mesh; keep material slots (their names are the `<kind>_<surface>` keys the
  rest of this spec refers to). The one exception is `attraction/ferris_wheel`
  (two nodes, `ferris_wheel` and `rotor`): import as two meshes,
  `SM_ferris_wheel` and `SM_ferris_wheel_rotor`, and record the rotor's
  offset (hub at local (0, 0, 35 m)) — stage 6 spawns them together.
- Scale: glTF metres → UE cm (Interchange default 100). Verify
  `SM_armco_4m` bounds are 400 × ~20 × ~96 cm.
- Nanite on for kinds `grandstand`, `building`, `pit`, `bridge`,
  `attraction`; off for everything instanced (`tree`, `barrier`,
  `tire_wall`, `board`, `fence`, `sign`, `light`, `cone`, `misc`,
  `vehicle`) and for `sky`.
- Material fix-ups after import: glTF `MASK` materials (`fence_mesh`,
  `tree_foliage_*`) must be Masked and **two-sided**; trees carry custom
  normals (spherical) — do not recompute normals. Slots named
  `gantry_lamp`, `led_panel`, `floodlight_lamp`, `led_screen` are the
  emissive-able surfaces; leave them as ordinary materials here, stage 6
  swaps them.
- Import `content/props/board/brands/*.png` as `/Game/Props/board/Brands/T_brand_<name>`
  and `board/markers/*.png` as `T_marker_<n>` (sRGB, no mips clamp).
- Idempotent: re-running replaces assets in place (same package names, so
  levels referencing them stay valid). Log one line per asset with tri count
  and bounds; fail the run on any GLB that produces an empty mesh.
- Wire into `scripts/build_track_levels.ps1` as an optional first step
  (`-ImportProps`), and into `build_release.ps1` the same way as the track
  levels (a `-SkipProps` that still checks `/Game/Props` exists). Packaging
  already cooks by path (`bCookAll`), so nothing else is needed for props to
  ship.

## Stage 2 — resolution in `ApexTrackAssetBuilder`

Replace the kind-only lookup with a resolver, used for every prop and for
the gantry:

```
ResolvePropMesh(kind, asset) ->
    /Game/Props/<kind>/SM_<asset>                 (authored)
 -> /Game/Props/<kind>/SM_<default-for-kind>      (authored default)
 -> PropMeshes[kind]                              (generated recipe, as today)
 -> placeholder cube                              (as today)
```

- Alias table applied before the lookup (data, one place):
  `tree/tree_generic → tree/broadleaf_m`, `barrier/armco_generic →
  barrier/armco_4m`, `sign/board_200m|100m|50m → board/braking_marker` with
  `text` kept (or set from the name when empty). Aliases may change the
  kind, so instancing keys off the resolved (kind, asset).
- Default per kind (`docs/PROPS.md` "default"): barrier `armco_4m`,
  tire_wall `tires_4m`, board `hoarding_3m`, fence `mesh_4m`, grandstand
  `bay_10m`, pit `garage_6m`, bridge `start_gantry`, light
  `floodlight_tower`, tree `broadleaf_m`, sky `blimp`; sign/cone/misc/
  vehicle/attraction/building have no default yet (fall through).
- Instancing: one HISM per resolved (kind, asset) for the instanced kinds
  listed in stage 1, actor label `Props_<kind>_<asset>`; keep the
  custom-data float. Non-instanced kinds stay one actor each.
- Scale: authored meshes are 1 unit = 1 m at scale 1 like the recipes, so
  `Prop.Scale` applies uniformly as today. Ground pivot for everything but
  `sky`, whose origin is the hull centre (`Location.Z` is the altitude).
- **Facing check (do this first, on `armco_4m`):** the recipes expect the
  road on local **+Y** (UE) and `bFaceRoad` flips by `RoadSideOf`. Authored
  assets were modelled with the road on **-Y in Blender**; after the glTF
  Y-up round trip and Interchange's handedness change, confirm which UE
  local side the rails land on. If they face away from the road, add one
  fixed 180° yaw for authored assets in the resolver (never edit the GLBs).
  Then mark authored kinds `barrier, tire_wall, board, fence, grandstand,
  pit, sign, light` as face-road; `bridge`, `tree`, `attraction`, `sky` are
  not flipped.
- Text → material: for props whose resolved mesh has a slot named
  `board_brand`, `bridge_brand`, `bridge_brand_*`, `pit_team_board` or
  `tyre_bridge_brand`, and `text` names a brand with a `T_brand_<text>`
  texture, override that slot with a material instance (cache one MI per
  brand per parent material). `braking_marker` maps `text` 50/100/150/200
  to `T_marker_<n>` on slot `board_marker`. Unknown text: leave the
  imported default (Piretti/ApexSim), log once per track. Drop the
  `AddSignText` text-render for props that resolved to an authored mesh.
- Start lights: when `bridge/SM_start_gantry` exists use it as the
  `StartLights` root mesh (pivot at road centre on the ground, beam along
  local Y, lamp panel facing local -X, i.e. towards cars approaching along
  +X). Keep spawning the five `start_light` components; place them over the
  panel's five lamp columns: local x = -37 cm, y = (i-2)·80 cm, z = 545 cm,
  radius ~22 cm (the authored panel has a second row at z = 485 cm — leave
  it as decoration or add five more components if the director can drive
  ten). The recipe gantry remains the fallback when the asset is missing.

Acceptance: import Suzuka (or any circuit) and take screenshots with
`-ApexCameraLookAt` (see CLAUDE.md, "Screenshot camera"); armco, tyre
walls, trees and boards are the authored ones, facing the road; the old
levels still build when `/Game/Props` is empty.

## Stage 3 — grandstand runs (importer only)

A `grandstand` prop is one stand; the importer lays out bays:

- `n = round(length_m / 10)`, default `length_m = 30` until the exporter
  emits it (stage 5). Straight: `bay` at local x = (i - (n-1)/2)·10 m, caps
  `end_cap` at x = ±(n·5 + 0.5).
- Variant choice: `<asset>` is the family (`bay_10m`, `bay_10m_roof`,
  `bay_10m_large`, `bay_10m_large_roof`); if the prop carries `radius_m`
  (stage 5) pick the wedge whose front radius is nearest: 95 m → `_curve6`,
  48 m → `_curve12`, negative (inside of the corner) → `_curve6_in`;
  |R| > 150 m → straight. The `_roof` suffix carries over
  (`bay_10m_curve12_roof`). Large bays use `end_cap_large`.
- Curved placement: bay i is bay 0 rotated by `(i - (n-1)/2)·θ` about
  `P = (0, -Rf)` in the prop's local frame, `Rf = 5 / tan(θ/2)` (P at `+Rf`
  for `_in`); caps take the outermost bay's rotation plus ±0.5 m along
  their local X. Formula and a verified render are in `docs/PROPS.md`.
- All bays of one stand go into one HISM per (asset) under a single
  `Grandstand_<n>` actor so a stand is one thing in the outliner.

## Stage 4 — bridges (importer only)

- `bridge` props are never flipped or pushed; the pivot is the road centre
  on the ground, span along local Y. Scale local Y by
  `span_m / 15` where `span_m` is the road width at the prop's station (from
  `Scene.Centerline` half-widths, nearest point — or the exporter's
  `span_m` once stage 5 lands). X and Z stay 1.
- `tyre_bridge` and `truss_bridge` accept `text` for their brand slots like
  boards.

## Stage 5 — exporter and editor (Rust, `track-editor`)

- `ats.rs`: add `PropKind` variants `board, fence, pit, bridge, vehicle,
  attraction, sky` (serde snake_case; keep the order of the existing nine).
  Add optional prop fields `length_m: Option<f32>` (grandstands) and let
  `text` stay free-form. The editor's prop palette lists the new kinds.
- `ue_export.rs::bake_props`: pass `length_m` through; for grandstands add
  `radius_m` = signed centerline radius at the nearest station (positive
  when the stand is on the outside of the bend); for bridges add
  `span_m` = road width at the station. Emit the pit complex from
  `pit_lane` (the same box layout `marking_pit_line_*` uses): one
  `pit/garage_6m` per box on the garage side (pitch 6 m, yaw = lane
  heading, front face on the lane), `pit/garage_end` beyond the first and
  last box, and `pit/pit_wall_6m` every 6 m along the road side of the lane
  over the box span (plain walls for the tapers). Same rule as everything
  else in the export: generated, not hand-edited.
- `groom.rs`: behaviour per new kind — `board` snaps to the nearest barrier
  run and faces the road; `fence` is laid like barriers but 1.5 m behind
  them; `pit` is skipped (the exporter owns it); `bridge` is left where
  placed (no push-off, seated on the verge at both footings); `sky` is
  never seated (absolute z); `vehicle`/`attraction` are pushed clear by
  their footprint radius (`prop_radius`: attraction 25 m, vehicle 3 m).
  Add the new kinds to the footprint tables (`prop_radius`, the
  rectangle-footprint match at ~line 185) and tests alongside the existing
  ones.
- `.ats` v-bump: not needed if new fields are `#[serde(default)]`.

## Stage 6 — runtime bits (game module)

- `sky/blimp`: an actor with a slow drift (e.g. 2 m/s along its heading
  over ±150 m, ease in/out) and a 0.3°/s yaw sway; purely cosmetic, client
  side.
- `attraction/ferris_wheel`: spawn `SM_ferris_wheel` with a child
  `SM_ferris_wheel_rotor` at the hub offset rotating about the local Y axis
  at ~0.5 rpm.
- Emissive slots: `light_panel`'s `led_panel` shows the flag state the
  server already sends (yellow/green/red/blue/chequer as flat colours);
  `floodlight_lamp` on for night sessions; `led_screen` a render target or
  a static texture for now.

## Verification

- `ApexPropImport -all` completes with 0 failures; every `SM_*` has bounds
  matching `docs/PROPS.md` sizes (spot check armco 4 m, garage 6×12×9.9 m,
  blimp 60 m).
- Track import of at least Suzuka, Monza and one club track; screenshots
  from the cockpit at speed (`-ApexView=cockpit`) and from the shot camera
  at the start gantry and the main grandstand. Boards read correctly (not
  mirrored) from the driving side; stands face the road; the pit wall's
  fence is on the track side.
- Levels still build with `/Game/Props` deleted (recipe fallback), and the
  start-light countdown still works on the authored gantry.
- `cargo test` in `track-editor` for the new kinds; `ats-groom --all` stays
  idempotent.
