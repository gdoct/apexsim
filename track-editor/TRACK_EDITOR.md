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

Non-negotiable constraints:

- The editor never writes `track.yaml` or any other server-owned file.
- Existing logical YAML remains loadable by `server/src/track_loader.rs` (unchanged — the editor no longer produces YAML at all).
- Generated `.ats` output is deterministic.

## 5. Unreal import

`game-unreal/` consumes the `.ats` directly (JSON). The importer is expected to:

- rebuild the road/curb/marking geometry from the source YAML centerline + the `.ats` station spans (or import a mesh baked elsewhere),
- map `style` / `asset` keys to materials, meshes, and foliage types,
- convert coordinates: track `(x, y, z)` meters → UE `(x·100, −y·100, z·100)` cm (UE is left-handed, `+Y` right), yaw `θ` rad CCW → UE yaw `−θ·180/π` degrees.

## 6. Source layout

- `/track-editor` — this crate.
- `src/ats.rs`, `src/ats_io.rs` — `.ats` model + IO.
- `src/track_data.rs`, `src/track_io.rs` — read-only logical track model + loader (kept serde-compatible with the server; `save_track_file` exists only for the compat test suite).
- `src/track_path.rs` — station/arc-length sampling of the centerline.
- `src/track_mesh.rs` — preview strip meshes (track ribbon, curbs, markings, pit lane).
- `src/scene.rs` — viewport, selection, dragging.
- `src/main.rs` — egui panels.
- `src/mcp.rs` — MCP server.
