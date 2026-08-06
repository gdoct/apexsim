# ApexSim Track Editor Specification

## 1. Purpose

The ApexSim Track Editor is the source-authoring tool for circuits. It is implemented in Bevy. It opens generated, imported, or hand-authored tracks; lets designers edit logical geometry, gameplay metadata, terrain, environment, and visual presentation; validates every layer; and emits deterministic artifacts for the Rust server and Unreal.

It is not an Unreal level editor. The editor owns the engine-independent source representation. The server owns simulation; Unreal and the Bevy viewport consume exported visual data.

## 2. Scope

### Required v1 outcomes

1. Open all existing server-compatible YAML tracks, with or without terrain and editor sidecars.
2. Stable file format for logical track, terrain, and visual sidecars. The editor must be able to read and write these files without losing information or introducing nondeterminism. Each track should be in its own folder. The editor must be able to read and write the following files:
   - `track.yaml` — logical track definition, including centerline, width, banking, surface, raceline, checkpoints, and spawn grid
   - `terrain.msgpack` — terrain heightmap cache
   - `overlay.msgpack` — kerb, vegetation, material, zone, and marking overlays
   - `visual.json` — visual sidecar for Unreal export (textures, materials, decals)
3. Edit centerline geometry, width, banking, physical surface, raceline, checkpoints, and spawn grid without hand-editing YAML.
4. Create kerb, terrain-height, vegetation, material, zone, and marking changes as undoable operations.
5. Preview the result in 3D, validate it, save the intended source layer, and export stable artifacts.
6. Preserve server behavior when only editor or visual data changes.


### Non-negotiable constraints

- The Rust server reads logical track YAML and terrain cache only. It does not load textures, visual descriptors, or Unreal output.
- Coordinates are right-handed: $+X$ follows the course from start/finish, $+Y$ is left, and $+Z$ is up. Distances use meters; angles use radians.
- Generated output must be deterministic: no wall clock, unseeded randomness, or unordered-map iteration in saved output.
- Existing logical YAML remains loadable by `server/src/track_loader.rs`.
- The application uses Rust, Bevy, and `bevy_egui`. Shared structures are either in a common crate or have serde compatibility tests.

## 3. Tests and Acceptance Criteria

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

## 4. Delivery Plan

* source folder is /track-editor

## 5. Definition of Done

V1 is complete when a non-programmer can open a real track, safely make logical and visual changes, undo mistakes, understand diagnostics, save only the intended data layer, and export deterministic Unreal-ready artifacts. The server must load the resulting logical track without any dependency on visual data, and automated tests must enforce every source, validation, mesh, and export contract in this document.