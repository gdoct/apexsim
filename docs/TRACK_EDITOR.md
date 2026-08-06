# ApexSim Track Editor Specification

## 1. Purpose

The ApexSim Track Editor is the source-authoring tool for circuits. It opens generated, imported, or hand-authored tracks; lets designers edit logical geometry, gameplay metadata, terrain, environment, and visual presentation; validates every layer; and emits deterministic artifacts for the Rust server and Unreal.

It is not an Unreal level editor. The editor owns the engine-independent source representation. The server owns simulation; Unreal and the Bevy viewport consume exported visual data.

## 2. Scope

### Required v1 outcomes

1. Open all existing server-compatible YAML tracks, with or without terrain and editor sidecars.
2. Edit centerline geometry, width, banking, physical surface, raceline, checkpoints, and spawn grid without hand-editing YAML.
3. Create kerb, terrain-height, vegetation, material, zone, and marking changes as undoable operations.
4. Preview the result in 3D, validate it, save the intended source layer, and export stable artifacts.
5. Preserve server behavior when only editor or visual data changes.

### Out of scope for v1

- Editing Unreal assets, Blueprints, or shader graphs.
- Multiplayer/collaborative editing or cloud asset storage.
- Dynamic weather, spectators, city building, and general-purpose landscape tooling.
- Runtime physics tuning beyond existing `surface_type` and `friction` fields.
- Auto-fixing invalid geometry without explicit user approval.

### Non-negotiable constraints

- The Rust server reads logical track YAML and terrain cache only. It does not load textures, visual descriptors, or Unreal output.
- Coordinates are right-handed: $+X$ follows the course from start/finish, $+Y$ is left, and $+Z$ is up. Distances use meters; angles use radians.
- Generated output must be deterministic: no wall clock, unseeded randomness, or unordered-map iteration in saved output.
- Existing logical YAML remains loadable by `server/src/track_loader.rs`.
- The application uses Rust, Bevy, and `bevy_egui`. Shared structures are either in a common crate or have serde compatibility tests.

## 3. Users and Workflow

| User | Primary task |
| --- | --- |
| Track author | Correct a converted layout, define gameplay data, validate and export. |
| Environment artist | Apply materials, markings, terrain layers, kerbs, and vegetation without changing physics. |
| Gameplay/QA owner | Inspect diagnostics, track limits, spawn safety, and raceline flow in Test mode. |

```text
Import or generate logical YAML
        ↓
Open source + optional sidecars
        ↓
Geometry and gameplay edits
        ↓
Terrain, kerb, vegetation, and visual edits
        ↓
Validate ── errors ──> select issue, repair, validate again
        ↓
Save authored layers and export deterministic build artifacts
        ↓
Rust server / Unreal importer
```

## 4. Data Ownership and Files

### 4.1 Source layers

| Layer | File | Owner | Contents | Server use |
| --- | --- | --- | --- | --- |
| Logical track | `<track>.yaml` | Track author | nodes, widths, banking, friction, physical surface, raceline, checkpoints, spawns, metadata | Required |
| Terrain cache | `<track>.terrain.msgpack` | Terrain generator | deterministic heightmap/world data | Optional |
| Editor overlay | `<track>.apexedit` | Editor | sparse terrain changes, node kerb overrides, placed trees | Never directly |
| Visual descriptor | `<track>.visual.yaml` | Environment artist | materials, zones, markings, terrain-layer paint | Never |
| Build output | `build/<track>/` | Exporter | preview cache, lock file, Unreal importer input | Never |

`surface_type` and `friction` are physical fields. A material ID only controls rendering. Assigning `road.asphalt.worn` must not change a node's `surface_type`, friction, or track limits.

```text
content/
├── materials/
│   ├── materials.yaml
│   └── textures/
└── tracks/real/
    ├── example.yaml
    ├── example.terrain.msgpack
    ├── example.apexedit
    ├── example.visual.yaml
    └── build/example/
        ├── preview.mesh
        ├── visual.lock.json
        └── unreal.json
```

All referenced paths are relative to their content root. The loader rejects absolute paths and paths that escape `content/materials` or the selected track root.

### 4.2 Stable identity

The editor requires a UUID `track_id` in logical YAML before it writes an overlay or visual descriptor. A legacy track with no ID still opens, but has an identity warning and an explicit **Assign Track ID** action. The editor never adds an ID as a side effect of unrelated save.

The overlay and visual descriptor record this UUID. A mismatched ID blocks their load; the user may inspect but must explicitly migrate or discard the sidecar.

### 4.3 Logical track contract

The YAML fields are defined in [TRACK_FILE_FORMAT.md](TRACK_FILE_FORMAT.md). The editor must preserve supported fields and avoid reordering unrelated YAML on a focused save.

| Data | Editing contract |
| --- | --- |
| Centerline | Move, add, duplicate, delete, or reorder nodes through topology-safe commands. |
| Width | Edit symmetric or left/right width in meters; always display the resolved width. |
| Banking | Store radians, display radians and degrees. |
| Physical surface | Edit `surface_type` and `friction` only in Gameplay mode. |
| Raceline | Create, edit, resample, and project it to the road surface. |
| Checkpoints | Place by centerline distance; resolve to existing node indices during YAML save. |
| Spawns | Place position, heading, order, and serialized node/offset values. |
| Metadata | Edit descriptive and terrain-generation values with explicit units and range checks. |

After a geometry edit the editor recomputes centerline interpolation, cumulative distance, road edges, normal/tangent frames, dependent gameplay locations, and all generated preview caches.

## 5. Application States and Layout

### 5.1 States

| State | Required behavior |
| --- | --- |
| Splash | Load bundled defaults and settings; external failures never block startup. |
| Browse | Select track root, scan logical YAML/JSON, display sidecar/validation status, and show a 2D preview. |
| Editor | Load logical data plus optional terrain, overlay, and visual sidecar into a working copy. |
| Export | Run final validation and atomically produce the build directory. |

Any parseable logical track appears in Browse even if it lacks a terrain cache or visual descriptor. Missing optional components are badges, not reasons to hide an asset.

### 5.2 Desktop layout

| Area | Contents |
| --- | --- |
| Toolbar | mode/tool selector, undo/redo, save overlay/logical/visual, validate, export, preview selector |
| Hierarchy | logical track, raceline, checkpoints, spawns, kerbs, terrain, trees, zones, markings |
| Viewport | 3D PBR preview, gizmos, selection, grid, track edges, and diagnostics |
| Inspector | selected-object properties, units, reset controls, and field validation |
| Bottom pane | errors, warnings, export log, Test mode controls |
| Status bar | track ID, dirty layers, active tool, camera position, world cursor, validation count |

Clicking any validation issue frames its location and selects its related object. Panels can collapse, but the viewport remains available.

## 6. Editing Behavior

Every edit is a command. Commands are atomic, undoable, and produce a single dirty layer. A pointer action that does not hit its valid target changes nothing.

### 6.1 Camera

- `W`, `A`, `S`, `D`: move on the ground plane; `Q`/`E`: vertical movement.
- Right mouse: rotate; middle mouse: pan; wheel: zoom.
- **Frame Selection** and **Frame Track** are required.
- Camera state is per-user editor state, never logical track data.

### 6.2 Geometry mode

Node movement uses axis gizmos and numeric input. Snapping supports grid, terrain, road edge, and node targets. Insertion splits the selected segment at clicked distance and interpolates elevation, width, banking, friction, surface type, and kerb values. Deletion is disabled when fewer than two nodes remain or dependent data cannot be resolved; the inspector names the blocker.

Changing nodes invalidates raceline/track-limit status and regenerates mesh, UV, tangent, zone, marking, and terrain-clearance previews before the next frame.

### 6.3 Gameplay mode

Raceline handles show lateral offset from the current road edges. Checkpoints are selected and moved by distance but serialize through the existing node-index format. Spawn controls provide position, heading, grid order, collision footprint, and road-boundary preview. Track limits are derived from road edges in v1; custom polygons are out of scope.

### 6.4 Kerb mode

Kerbs use the existing per-node `NodeKerbs` overlay. A brush selects left/right edge and adjusts height and width within configured limits. One mouse stroke creates one sparse, undoable override command, not a command every frame. Kerb mesh follows updated road edges. Legacy `KerbSegment` data loads for compatibility, but editor save writes per-node overlay values; flattening them into YAML is an explicit, diff-reviewed operation.

### 6.5 Terrain mode

Terrain mode requires a heightmap. When absent, **Generate Terrain** uses logical metadata: environment type, seed, scale, detail, and blend width. Identical inputs produce identical heightmaps.

The level brush operates on a circular meter radius with hard or linear falloff. It stores cells differing from base cache only. Brush replay samples a fixed spatial path so frame rate cannot change its result. Terrain edits update clearance diagnostics but never alter logical node height unless the user explicitly selects **Bake Terrain Elevation to Nodes**, which is a separate logical command.

### 6.6 Vegetation mode

Vegetation records type, position, rotation, scale, and stable instance ID in the overlay. Brush sampling derives from track UUID, stroke ID, and brush settings; replay is deterministic. The editor rejects placement on road, kerb, pit, or configured safety margin. Vegetation has no collision or simulation effect in v1.

### 6.7 Visual mode

Visual mode edits visual data only. It provides material picker, zone/marking handles, terrain-layer paint, and final/base-color/normal/roughness/UV/material-ID/diagnostic previews. The inspector always displays physical `surface_type` and `friction` beside the visual material as read-only context. Missing visual data uses a conspicuous fallback, never a silent gray material.

### 6.8 Test mode

Test mode locks authoring and moves a non-authoritative preview vehicle along the raceline. It includes chase/cockpit cameras, pause, scrub, and speed. It displays distance, lateral offset, active surface, terrain clearance, and checkpoint. This is a flow check, not a replacement for Rust server physics tests.

## 7. Save, Undo/Redo, and Recovery

| Action | File | Preconditions |
| --- | --- | --- |
| Save Overlay | `.apexedit` | overlay validation succeeds |
| Save Logical Track | `.yaml` | logical validation succeeds and target is writable |
| Save Visual Data | `.visual.yaml` | visual validation succeeds and logical UUID exists |
| Export | `build/<track>/` | all required validation succeeds |

Writes use a temporary sibling file, flush it, then atomically replace the target. Export builds a temporary directory and replaces the final output only after every artifact passes a hash check. Failure leaves prior files untouched.

Undo/redo is scoped to one loaded track. Terrain and vegetation strokes store compact deltas. Closing a dirty track presents independent save/discard/cancel choices for logical, overlay, and visual layers.

## 8. Visual Materials and Textures

### 8.1 Material library

`content/materials/materials.yaml` is a versioned allowlist. Track descriptors reference stable IDs, not arbitrary image paths or Unreal materials.

```yaml
version: 1
materials:
  - id: road.asphalt.worn
    surface_hint: Asphalt
    projection: track_uv
    tile_size_m: [4.0, 4.0] # longitudinal, lateral
    base_color: textures/asphalt_basecolor.png
    normal: textures/asphalt_normal.png
    orm: textures/asphalt_orm.png # R=occlusion, G=roughness, B=metallic
    height: textures/asphalt_height.png # optional
    normal_strength: 1.0
    height_scale_m: 0.002
    opacity: opaque
```

Required fields are `id`, `projection`, `tile_size_m`, `base_color`, `normal`, `orm`, and `opacity`. IDs match `[a-z0-9]+([.-][a-z0-9]+)*`. Base color is sRGB; normal, ORM, height, and masks are linear. Normal maps use tangent-space OpenGL convention; Unreal flips green once during import. V1 supports only `track_uv`, `world_x_y`, and `triplanar`; no custom shader graphs, opacity masks, or per-track copies of shared textures.

### 8.2 Track visual descriptor

`<track>.visual.yaml` is optional for preview and required for visual export. It uses meters from start/finish. A distance range is half-open `[start_m, end_m)`; on closed loops `end_m < start_m` wraps across start/finish. Main-road lateral coordinates are `-1.0` left edge, `0.0` centerline, `1.0` right edge.

```yaml
version: 1
track_id: "8e0a778d-335a-4d41-bbe6-7db4db653943"
road_material: road.asphalt.worn
terrain_material: terrain.grass.short
zones:
  - id: left_kerb_t1
    kind: edge_strip
    material: kerb.red_white
    side: left
    start_m: 82.0
    end_m: 148.0
    width_m: 1.2
    priority: 20
  - id: grass_runoff_t1
    kind: road_band
    material: terrain.grass.short
    start_m: 70.0
    end_m: 170.0
    lateral_start: -1.0
    lateral_end: -1.6
    blend_width_m: 0.5
    priority: 5
markings:
  - id: start_finish
    kind: start_finish_line
    material: marking.checker
    distance_m: 0.0
    width_m: 12.0
    length_m: 0.5
    z_offset_m: 0.003
```

`road_material` and `terrain_material` are required. Zones have unique IDs, valid material IDs, and priority. Higher priority draws above lower priority; equal-priority overlap on a mesh is invalid. `edge_strip` extends outward from a road edge; `road_band` follows main-road normalized lateral coordinates. `ribbon` is reserved for future named splines and rejects unknown names.

Markings are generated meshes or decals, never baked texture pixels. V1 supports start/finish, grid boxes, lane lines, and kerb stripes. Terrain painting stores at most four named layer weights per vertex/cell and normalizes them to $1.0 \pm 0.001$.

### 8.3 Mesh and UV contract

For road surfaces, let $s$ be accumulated centerline meters and $q$ be lateral meters from the rendered surface's left edge. `track_uv` material output is:

$$UV_0 = (s / tile\_length_m, q / tile\_width_m)$$

Road mesh must emit normals and tangents. A closed loop duplicates the first cross-section with $s=total\_length_m$ and does not reuse the $s=0$ vertices for the closing segment. Terrain `world_x_y` UVs use $(world_x/tile_x_m, world_y/tile_y_m)$; triplanar projection uses position and normal. All geometry edits invalidate UV, tangent, zone, marking, and preview caches.

## 9. Validation

Validation runs after load, before Test mode, on save, and on export. An issue includes stable ID, severity, source layer, YAML path/object ID, message, remediation, and optional world location.

| ID | Severity | Rule |
| --- | --- | --- |
| `TRK001` | Error | Fewer than two nodes or non-finite logical numeric value. |
| `TRK002` | Error | Resolved road width is not positive. |
| `TRK003` | Error | Nodes are less than 1 cm apart or make a zero-length interpolated segment. |
| `TRK004` | Error | Checkpoint/spawn has invalid reference or unresolvable order. |
| `TRK005` | Error | Raceline exceeds road bounds by more than 5 cm. |
| `TRK006` | Error | Spawn crosses an edge, overlaps another spawn, or lacks a heading. |
| `TRK007` | Error | Invalid terrain dimensions, cell size, or overlay terrain-cell index. |
| `TRK101` | Warning | Terrain intersects road/kerb outside 5 cm clearance. |
| `TRK102` | Warning | Curvature, grade, or banking discontinuity exceeds configured limits. |
| `VIS001` | Error | Unsupported visual version or mismatched/missing UUID. |
| `VIS002` | Error | Missing/duplicate material ID or path outside material root. |
| `VIS003` | Error | Image cannot decode, is zero-sized, or exceeds 8192 px per side. |
| `VIS004` | Error | Invalid color space, projection, tile size, or material map. |
| `VIS005` | Error | Duplicate/empty zone or marking, unknown material, or unknown spline. |
| `VIS006` | Error | Equal-priority zones overlap without explicit blend. |
| `VIS007` | Error | Invalid terrain-layer count or weights not totaling $1.0 \pm 0.001$. |
| `VIS101` | Warning | Fully clipped zone or visual hint differs from physical surface. |
| `VIS102` | Warning | Texture density outside 128-1024 texels per meter. |
| `EXP001` | Error | Non-finite mesh data, invalid index, missing tangent, or invalid seam. |
| `EXP002` | Error | Output cannot be atomically written or fails lock-file hash. |

Export ordering is defined: zones sort `(priority, id)`, trees by stable instance ID, and JSON keys/arrays follow serializer order. No visual descriptor is a warning with fallback preview, never a logical-track load failure.

## 10. Export Contract

Export never modifies source files. A validated visual export produces:

```text
build/<track>/
├── preview.mesh        # optional Bevy cache; non-authoritative
├── visual.lock.json    # SHA-256 input manifest
└── unreal.json         # deterministic importer input
```

The lock file includes logical UUID, editor version, descriptor hash, referenced material entries, and source image hashes. `unreal.json` includes relative paths, material IDs, mesh-section IDs, UV/tangent contract version, zones, markings, terrain layers, and lock hash.

The Unreal importer owns its master material and resolves stable material IDs. It fails on hash mismatch; it never guesses assets from texture filenames. The Rust server consumes only YAML and terrain cache.

## 11. Tests and Acceptance Criteria

| Level | Required coverage |
| --- | --- |
| Unit | serde, path containment, validation IDs, range wrapping, UV/tangent math, deterministic brushes |
| Property | finite mesh data, valid terrain indexing, seam invariants, no panics for malformed input |
| Integration | open existing tracks, apply overlay, save/reload semantic equality, two identical exports |
| Compatibility | editor-saved logical YAML loads through `TrackLoader` and preserves physical values |
| Visual smoke | each preview mode renders a non-empty mesh; missing visual sidecar shows fallback |

V1 is accepted only when all of these pass:

1. A minimal logical track opens and saves without terrain, overlay, or visuals.
2. A real track receives terrain, kerb, and tree overlay edits and reloads identically.
3. Material/visual examples validate and export.
4. Missing, duplicate, traversal, and undecodable textures produce `VIS002` or `VIS003`.
5. A 1,000 m closed loop has a duplicated seam whose UV differs by $1000/tile\_length_m$.
6. Adding a centerline sample moves a distance-based visual zone by at most 1 cm.
7. Equal-priority zone overlap fails; higher-priority overlap exports stably.
8. Visual-only edits leave server-loaded physical values byte-identical.
9. Repeated unchanged exports produce identical lock/import files.
10. Failed save/export leaves previous source and build files unchanged.

## 12. Delivery Plan

### Milestone 1: Logical editing reliability

Implement logical/overlay loading and saving, command undo/redo, atomic writes, dirty prompts, `TRK001`-`TRK007`, and complete Browse, Camera, Geometry, Gameplay, Kerb, Terrain, Vegetation, and Test mode flows. Add server compatibility tests.

### Milestone 2: Deterministic preview and terrain

Make terrain and brush replay deterministic; add terrain clearance diagnostics, cache invalidation, and diagnostic/base-color/UV previews.

### Milestone 3: Visual authoring

Add material/visual serde types, `VIS001`-`VIS007`, meter-based seam-safe UVs and tangents, Visual mode, zones, markings, terrain-layer paint, and PBR preview.

### Milestone 4: Export hardening

Generate lock/import files, add determinism tests across representative tracks, document Unreal importer behavior, and provide an explicit track-ID migration action.

## 13. Definition of Done

V1 is complete when a non-programmer can open a real track, safely make logical and visual changes, undo mistakes, understand diagnostics, save only the intended data layer, and export deterministic Unreal-ready artifacts. The server must load the resulting logical track without any dependency on visual data, and automated tests must enforce every source, validation, mesh, and export contract in this document.