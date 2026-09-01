# ApexSim Track Editor Specification

## 1. Purpose

The ApexSim Track Editor is the **scene-authoring tool** for circuits. It is implemented in Rust with Bevy and `bevy_egui`.

Its job is to turn a logical track definition into a 3D scene Unreal can build a level from. The division of labor:

- **`track.yaml`** (in `content/tracks/`) is the logical track: centerline, widths, banking, surfaces, checkpoints, spawn grid. It is owned by the server tooling and is **read-only input** for the editor. The editor never writes YAML.
- **`<Track>.ats`** (Apex Track Scene) sits next to the YAML (same stem, `.ats` extension) and is **the file the editor edits**. It describes everything the 3D scene adds on top of the logical track: curbs, painted markings, the pit lane, and trackside props (trees, signs, barriers, buildings, grandstands, lights, …). This is the artifact the Unreal importer consumes.
- The Rust server keeps loading logical YAML only; it never reads `.ats`.

The editor is not an Unreal level editor. It owns the engine-independent scene representation; Unreal turns it into meshes/materials/foliage.

## 2. The `.ats` format

Plain JSON (readable by Unreal's `FJsonSerializer` and by humans), one file per track. Top level:

| Field | Meaning |
| --- | --- |
| `format` | Always `apex-track-scene` |
| `version` | Format version (currently 2; v1 files load and migrate in memory) |
| `source_track` | File name of the source YAML this scene decorates |
| `track_name` | Display name copied at creation time |
| `surfaces` | Ground beside the track: grass, gravel traps, asphalt runoff, concrete, sand, astroturf |
| `curbs` | Curb strips anchored to the track edge |
| `markings` | Painted rectangles on the track surface |
| `pit_lane` | Optional pit lane: own centerline nodes, width, box count, speed limit |
| `props` | World-anchored objects: trees, signs, barriers, tire walls, buildings, grandstands, lights, cones, misc |
| `next_id` | Monotonic id counter (determinism: no UUIDs, no wall clock) |

Anchoring:

- **Track-anchored** (surfaces, curbs, markings): *station* = distance in meters along the sampled centerline, plus lateral offsets in meters where **positive = left** of the centerline. Because they anchor by station, these elements follow the track through corners and survive small centerline edits. On closed loops, `end < start` wraps through start/finish.
- **Surfaces** additionally measure their borders *outward from the track edge* (`inner_m` clears the curb, `width_m` is the extent, optional `end_width_m` tapers the patch into a wedge), so a gravel trap keeps its shape where the track widens. Coincident patches stack by `SurfaceKind::layer()` — grass underneath, then concrete, runoff, sand, gravel, astroturf — and the whole layer renders below the track ribbon.
- **World-anchored** (props, pit-lane nodes): absolute track-space coordinates. Track space is right-handed: `+X` follows the course from start/finish, `+Y` is left, `+Z` is up; meters and radians. Prop `yaw_rad` is CCW from `+X`. The Unreal importer converts to UE conventions (cm, Z-up left-handed).

Determinism rules for saved output: no wall clock, no unseeded randomness, no unordered-map iteration. Repeated saves of an unchanged scene are byte-identical (enforced by tests). Saves are write-temp-then-rename, so a failed save leaves the previous file untouched.

## 3. Editor behavior

1. **File → Open track.yaml…** loads the YAML read-only. If a sibling `.ats` exists it is loaded; otherwise a fresh scene is created in memory (seeded with a start/finish line marking) and marked unsaved. A corrupt `.ats` is reported and never overwritten silently.
2. **Save (Ctrl+S)** writes the `.ats` only.
3. The viewport previews the track ribbon (from the same Catmull-Rom sampling the server uses), ground surfaces, curbs, markings, pit lane, and stand-in prop meshes. Props and pit-lane nodes are draggable; clicking selects; the side panel hosts layer lists and a property inspector.
4. All scene edits are undoable (Ctrl+Z / Ctrl+Shift+Z), from the GUI and from MCP alike.
5. An MCP server (streamable HTTP, `127.0.0.1:8420/mcp`, override with `APEXSIM_TRACK_EDITOR_MCP_PORT`) exposes read-only track inspection plus scene editing tools (`add_surface`, `add_curb`, `add_marking`, `add_prop`, `set_pit_lane`, `remove_element`, `undo`, `redo`, `save`, …).

## 4. Tests and Acceptance Criteria

| Level | Coverage |
| --- | --- |
| Unit | `.ats` serde round-trips, validation (duplicate ids, bad ranges), v1 → v2 load/migrate, station math (wrap, interpolation), strip meshes finite/non-empty, surface taper + layer order |
| Integration | every real track opens; default scene creation, save/reload equality, byte-identical repeated saves; saving a scene never touches the YAML (`tests/ats_scene.rs`) |
| Compatibility | the editor's logical-track model still reads every real YAML the server loads (`tests/compat.rs`) |
| Unreal export | winding against Unreal's front-face convention on every triangle of every real circuit, raised curb profiles, banking-following normals, grid resolution matching the server's, in-range indices, deterministic repeat bakes, exporting touches neither YAML nor `.ats` (`tests/ue_export.rs`) |

Non-negotiable constraints:

- The editor never writes `track.yaml` or any other server-owned file.
- Existing logical YAML remains loadable by `server/src/track_loader.rs` (unchanged — the editor no longer produces YAML at all).
- Generated `.ats` output is deterministic.

## 5. Unreal export

The `.ats` cannot be handed to Unreal as-is: every track-anchored element is a *station span* against a centerline that lives in the read-only YAML, and Unreal has no YAML parser. So the editor bakes both files into one self-contained artifact.

**`<Track>.uescene.json`** (format `apex-ue-scene`, v1) is written by `src/ue_export.rs` and consumed by the Unreal `ApexTrackImport` commandlet. It is a *generated* file: it lands in `content/tracks/export/` (gitignored — 26 circuits bake to ~120 MB of vertex data), never beside the source content.

| Field | Contents |
| --- | --- |
| `materials` | Every material key the meshes reference, with a `family` (`road`, `curb`, `surface`, `marking`, `pit_lane`) and the base color the editor previewed. Sorted by key |
| `meshes` | Baked triangle geometry, flattened buffers, named `{material_key}_{section:03}`. Includes the terrain ground tiles (key `ground`, family `surface`) |
| `props` | Prop transforms with asset keys |
| `grid` | Starting grid, resolved exactly the way `server/src/track_loader.rs` resolves it |
| `centerline` | The sampled centerline, for splines, minimaps and AI |
| `pit_lane` | Width, box count, speed limit (its ribbon is in `meshes`) |

Bake with `cargo run --bin ats-export -- --all`, or **File → Export for Unreal…** for the track in the editor (which bakes unsaved edits too).

### Terrain

Tracks carry no terrain of their own — only the centerline has heights — so `src/terrain.rs` derives one: centerline samples spread their height onto a coarse grid by inverse-distance weighting, giving a field that agrees with the road wherever the road is and rolls smoothly in between. Two things consume it, identically in the viewport and the bake:

- **Ground bands** (`surfaces` in the `.ats`) hug the road edge for their first ~6 m, blend into the terrain by ~35 m out, and beyond the shoulder are clamped to at most 0.3 m above the field — which, via the road ceiling, guarantees they can never cover any road. Bands are subdivided laterally (~10 m columns, in the preview and the bake alike) so that profile is actually sampled across their width; a band left as one quad would just span a plane over whatever lies between its borders.
- A **ground mesh** built straight from the grid (with a 180 m margin past the track's bounding box) sits 0.25 m under every authored surface, so the world is never a void.

Prop `z` stays absolute; newly placed props are seated on the terrain at placement time.

### Conventions at the boundary

Output is already in Unreal's frame, so the commandlet converts nothing:

- **Units** centimeters and degrees; **position** track `(x, y, z)` m → UE `(x·100, −y·100, z·100)` cm; **yaw** track `θ` rad CCW → UE `−θ·180/π` degrees.
- That mapping negates an axis, so its determinant is −1 — a mirror, not a rotation, and it flips triangle handedness on its own. Geometry is emitted with ordinary right-handed CCW winding and **not** reversed again; reversing as well lights every surface in the level from underneath. `winding_matches_unreals_front_face_convention` pins this to Unreal's rule that a front face is clockwise seen from its normal.
- Meshes are cut into 250 m sections and merged per (section, material) so a circuit becomes ~100 medium meshes rather than one 6 km mesh or one mesh per element.

### What the bake is not

These are not the viewport's preview strips. `track_mesh.rs` hardcodes an up-normal and draws curbs flat, which is fine for a preview and wrong for a lit level; the export derives normals from the banked surface frame and gives curbs a raised profile with a real outer face.

The bake also has to cope with source centerlines whose corners are tighter than the elements wrapped around them — Austin has 8 m radius apexes on a 15 m wide track. A station-anchored element measures straight out from the centerline, so past a radius of `1/κ` its offset curve folds through itself and the strip inverts. Three defences, in order: borders are clamped short of the centre of curvature; an element lying wholly inside the limit with no room to spare is dropped and the strip broken rather than compressed into a knot; and any facet that still folds, twists, or degenerates into a sub-degree sliver is discarded. Whatever survives is wound to agree with its own surface normal, so an inside-out triangle cannot reach the level.

## 6. Source layout

- `/track-editor` — this crate.
- `src/ats.rs`, `src/ats_io.rs` — `.ats` model + IO.
- `src/ue_export.rs`, `src/ue_export_io.rs` — the Unreal bake (§5); `src/bin/ats-export.rs` is its CLI.
- `src/groom.rs` — deterministic scene clean-up: walls/barriers deleted on straights and re-laid as continuous corner runs (12 m segments on a station-cell grid, gaps ≤ 45 m fused, seated at the runoff edge); every other prop pushed clear of the road and the pit lane by its footprint radius (pit-side buildings align to the lane), then seated on the terrain; and the pit lane regenerated first via `src/pit.rs` (entry taper off the road edge inside the start/finish straight, parallel pit road, exit taper back on; width/boxes/speed limit/side preserved). `src/bin/ats-groom.rs` is its CLI (`ats-groom --all`, idempotent, run from the repo root).
- `src/mcp.rs` also exposes `set_view` (aim the viewport camera at a station or point) and `screenshot` (capture the viewport to a PNG) so an agent can inspect tracks visually over MCP.
- `src/track_data.rs`, `src/track_io.rs` — read-only logical track model + loader (kept serde-compatible with the server; `save_track_file` exists only for the compat test suite).
- `src/track_path.rs` — station/arc-length sampling of the centerline.
- `src/terrain.rs` — derived terrain heightfield (§5 “Terrain”).
- `src/track_mesh.rs` — preview strip meshes (track ribbon, curbs, markings, pit lane, ground).
- `src/scene.rs` — viewport, selection, dragging.
- `src/main.rs` — egui panels.
- `src/mcp.rs` — MCP server.
