# Track formats

A circuit is a folder `content/tracks/{default,custom}/<Stem>/` holding a
logical track (`<Stem>.yaml`), a scene (`<Stem>.ats`) and the files the
pipeline derives from them. This doc is the reference for the three formats
people and tools read and write: the track YAML, the `.ats` scene, and the
client export (`.uescene.json` + `.uemesh`). How the files are produced is in
[track-pipeline.md](track-pipeline.md); the server sidecars are described
there too.

## Code

- `server/src/track_loader.rs`: `TrackFileFormat`, `TrackNode`, the loader and
  the Catmull-Rom sampler (`SplineInterpolator`); `server/src/data.rs`:
  `TrackMetadata`, `RacelinePoint`, `DrsZone`, `SurfaceType`, `TrackConfig`.
- `track-editor/core/src/track_data.rs`, `track_io.rs`: the pipeline's copy
  of the YAML model (`TrackFile`), kept serde-compatible with the server.
- `track-editor/core/src/ats.rs`, `ats_io.rs`: the `.ats` model and IO.
- `track-editor/core/src/ue_export.rs`, `ue_export_io.rs`: the bake and the
  export's reader/writer; `core/src/bin/ats-export.rs` is the CLI.
- Client reader: `game-unreal/Source/ApexSim/{Public,Private}/Track/ApexTrackSceneReader.*`
  (`FApexTrackSceneReader`, `kSupportedVersion = 3`), data in `ApexTrackSceneData.h`.
- `server/src/bin/convert_track.rs`: CSV centerline to YAML (below).

## Files of one circuit

| File | Written by | Read by | In git |
|---|---|---|---|
| `<Stem>.yaml` | hand, converters, the data tools (below) | server, every pipeline tool | yes |
| `<Stem>.ats` | `ats-dress`, `ats-groom`, the editor, `seed_scene.py` | `ats-export`, the editor | yes |
| `<Stem>.layout.json` | `scripts/osm_layout.py` | `ats-dress`, `ats-groom`, guide | yes |
| `<Stem>.dem.msgpack` | `scripts/dem_fetch.py` | terrain, `dem_elevation.py`, `track_location.py` | yes |
| `<Stem>.guide.yml` | hand | `apexsim-replay guide` ([track-guide.md](track-guide.md)) | yes |
| `<Stem>.{ground,curbs,walls,road,pit}.msgpack` | `ats-export` | server | no |
| `build/tracks/<Stem>.uescene.json`, `.uemesh` | `ats-export` | client | no |
| `build/tracks/previews/<Stem>.png` | `scripts/build_track_catalog.py` | client | no |

Every sidecar path is the YAML's path with another extension. The client
exports are flat, keyed by stem, so a stem must be unique across `default/`
and `custom/`.

## The track YAML

The logical track: a centerline of nodes with widths, banking and surface,
plus the grid, sectors, raceline, DRS zones and metadata. YAML, or JSON when
the content starts with `{` (both loaders detect it the same way). The frame
is the server's: metres, right-handed, origin at the start/finish line,
+X along the track there, +Y to the left, +Z up.

### Top-level keys

| Key | Meaning |
|---|---|
| `name` | The circuit's real name. Source data only, never shown ([naming.md](naming.md)) |
| `display_name` | What the game shows; the server sends it as the track's name, falling back to `name` |
| `track_id` | Fixed UUID. Required in practice: without one the server mints a new id every start and no catalog row, record or showcase can match it |
| `nodes` | The centerline (below); at least 2 |
| `default_width` | Width where a node gives none; must be > 0 unless every node has `width` (loader falls back to 12 m) |
| `closed_loop` | `true` for a circuit |
| `checkpoints` | `{index_start, index_end}` node indices; only `index_start` is used, resolved to a station. Empty means virtual checkpoints at even fractions of the lap (every shipped circuit) |
| `sectors` | Two node indices: the boundaries of three sectors. Anything else gives even thirds |
| `spawn_points` | `{position, offset_x, offset_y}`; `position` indexes the **interpolated** centerline (about 1 m spacing), offsets in metres in the world frame. Empty gives 16 slots, rows of two 8 m apart, staggered 4 m, columns 4 m apart, mirrored by the exporter's painted boxes |
| `raceline` | `{x, y, z}` points; the AI and racing line drive the centerline without it |
| `drs_zones` | `{detection_m, start_m, end_m}` stations, written by `scripts/drs_zones.py` |
| `metadata` | Below |

### Node keys

`x`, `y`, `z` (z defaults to 0), `width` or `width_left` + `width_right`
(metres; the pair wins), `banking` (**radians**, positive lifts the road's
left edge, so a right-hander banks positive), `friction` (grip multiplier,
default 1.0) and `surface_type` (`Asphalt`, `Concrete`, `Curb`, `Grass`,
`Gravel`, `Sand`, `Wet`; anything else is asphalt). Real circuits are 5 m
nodes.

The server samples the nodes with a uniform Catmull-Rom spline at about 1 m
spacing (2 to 50 points a segment). Widths and banking are interpolated
across a segment; `friction` and `surface_type` are held from the segment's
start node. Heading, slope and station come from the samples. The pipeline's
`track_path.rs` uses the same spline formula (at 2 m), so the rendered road
edge and the server's track limits agree.

### Metadata

`country`, `city`, `length_m` (rewritten by `ats-smooth`; the curb sidecar
test checks it), `description` (shown in the track picker; describes the
place, not the operator), `year_built`, `category` (`F1`, `WEC`, `DTM`,
`IndyCar`... the logic keys on these; screens show `ApexCatalog::DisplayClass`),
`environment_type` (`desert`, `forest`, `city`, `mountains`, `plains`: a
scenery hint `ats-dress` and `ats-groom` read), and the location written by
`scripts/track_location.py`: `altitude_m` (thins the air), `latitude_deg`
(places the sun), `longitude_deg`.

`terrain_seed`, `terrain_scale`, `terrain_detail`, `terrain_blend_width`,
`object_density` and `decal_profile` belong to the server's older
procedural-terrain path (`server/src/procgen/`, `--generate-terrain`,
`*.terrain.msgpack` caches); the shipped circuits carry them as nulls and
the pipeline does not use them.

### Who rewrites the YAML

Server and pipeline must agree on every key, so a key added to one
`TrackFile` must be added to the other, or a pipeline rewrite drops it
(`the_location_survives_a_rewrite_as_written`, `core/tests/compat.rs`).
Tools that rewrite it: `ats-smooth` (node and raceline positions, length),
`ats-bank` (banking), `scripts/drs_zones.py`, `dem_elevation.py` (node and
raceline z), `bridge_elevation.py` (z), `track_location.py` (metadata),
`generate_race_line.py --track` (the raceline block). The Python tools edit
the text in place. Any change to the YAML changes its checksum
(`content_crc`, CR-stripped CRC-32), so re-export the track afterwards or the
client warns that its track differs from the server's.

### Converting a CSV centerline

`convert_track` turns a TUM racetrack-database style CSV
(`x_m,y_m,w_tr_right_m,w_tr_left_m`, optional raceline CSV `x_m,y_m`) into a
YAML. It keeps the `track_id` of an existing output file unless `--track-id`
is given:

```bash
cd server
cargo run --release --bin convert_track -- -t track.csv [-r raceline.csv] -o out.yaml \
    -n "Real name" --display-name "Shown name" [--country --city --category --year-built --description]
```

`server/tests/convert_all_tracks.sh` is the old batch driver for the whole
database. A converted YAML is only the start of a circuit: see "Adding a new
circuit" in [track-pipeline.md](track-pipeline.md).

## The `.ats` scene

Everything the 3D scene adds to the logical track. Plain JSON,
`format: "apex-track-scene"`, `version: 2` (`ATS_VERSION`; a v1 file loads
with empty `surfaces`). The server never reads it.

| Field | Contents |
|---|---|
| `source_track`, `track_name` | The YAML's file name and the name at creation |
| `imported` | `"ac"` for an Assetto Corsa import ([ac-import.md](ac-import.md)); every pipeline tool then leaves the track alone |
| `surfaces` | Ground bands beside the road: `kind` (`grass`, `gravel`, `asphalt_runoff`, `concrete`, `sand`, `astroturf`), `side` (`left`/`right`), `start_m`/`end_m`, `inner_m` (gap from the road edge), `width_m`, optional `end_width_m` (a wedge), optional `paint` on tarmac run-off (`red_yellow`, `blue_white`, `blue_red`, `red_white`, `green_white`) |
| `curbs` | `side`, `start_m`/`end_m`, `width_m` outward from the edge, `style` |
| `markings` | Painted rectangles: `kind` (`start_finish`, `grid_slot`, `edge_line`, `pit_entry`, `pit_exit`, `custom`), stations, `lat_from_m`/`lat_to_m`, linear RGBA `color` |
| `pit_lane` | `nodes` (`[x,y,z]` world), `width_m`, `box_count`, `speed_limit_kmh`, `authored` (laid from the dossier: grooming never regenerates it) |
| `props` | `kind`, `asset`, `x`/`y`/`z`, `yaw_rad`, `scale`, optional `text` (brand, board number, flag) and `length_m` (a grandstand's length). Kinds and assets: [props.md](props.md) |
| `decals` | Pictures painted on the road: `image` (`set/name`), `start_m`, `length_m`, `lat_m`, `width_m`, `reversed` |
| `water` | Polygon rings in world metres, even-odd fill |
| `bridges` | Station spans carried over water (bands and curbs are clipped out) |
| `dressing` | `season` (`summer`/`autumn`) and `spectators` (bool) |
| `external_sidecars` | Sidecars another tool wrote (`ground`, `curbs`, `walls`, `road`, `pit`) that `ats-export` must not overwrite |
| `next_id` | Monotonic id counter |

**Anchoring.** Surfaces, curbs, markings, decals and bridges are
*track-anchored*: stations in metres along the sampled centerline, lateral
offsets positive left. They follow the road through corners and survive
small centerline edits; on a loop `end_m < start_m` wraps through the line.
Surfaces measure outward from the road edge, so a gravel trap keeps its shape
where the road widens; coincident patches stack by `SurfaceKind::layer()`
(grass, concrete, run-off, sand, gravel, astroturf). Props and pit-lane nodes
are *world-anchored*: absolute track-frame coordinates, yaw counter-clockwise
from +X.

**Determinism.** No wall clock, no unseeded randomness, no unordered-map
iteration: saving an unchanged scene writes the same bytes. Saves are
write-temp-then-rename. Both `ats-dress` and `ats-groom` recycle the element
ids they own, so a second run writes an identical file.

## The track editor

`track-editor/` is a two-crate workspace. `core/` is **track-core**: the
formats, the pipeline (dress, groom, smooth, bank, terrain, the bake) and the
`ats-*` binaries, with no Bevy dependency, so `cargo test -p track-core` or an
`ats-*` tool never builds the renderer. `src/` is **track-editor**, the Bevy +
`bevy_egui` viewport on top of it (`main.rs` panels, `scene.rs` viewport and
dragging, `preview_mesh.rs` preview strips, `mcp.rs`).

```bash
cd track-editor
cargo run                          # the editor
cargo run --bin ats-export -- --all   # any ats-* tool (from the repo root, see below)
cargo test                         # both crates; whole-calendar bakes are #[ignore]d
cargo test -p track-core           # the pipeline only
```

File > Open track.yaml loads the YAML read-only and its `.ats` (or a fresh
scene seeded with a start/finish marking); Save (Ctrl+S) writes only the
`.ats`; File > Export for Unreal bakes the open scene, unsaved edits
included. Every edit is undoable from the GUI and the MCP alike. The MCP
server (streamable HTTP on `127.0.0.1:8420/mcp`, `APEXSIM_TRACK_EDITOR_MCP_PORT`
to move it) offers inspection (`open_track`, `get_track_info`, `list_nodes`,
`get_node`, `get_scene`, `list_prop_assets`), editing (`add_*`/`update_*` for
surfaces, curbs, markings and props, `remove_element`, `set_pit_lane`,
`clear_pit_lane`, `set_dressing`, `undo`, `redo`, `save`) and `set_view` /
`screenshot` to look at the viewport. The preview strips are flat-normalled
stand-ins; only the bake is what the game draws.

## The client export

`ats-export` bakes the YAML and `.ats` into a self-contained export, because
every track-anchored element is a station span against a centerline Unreal
cannot read. Two files in `build/tracks/`:

- **`<Stem>.uescene.json`**: compact JSON manifest, `format: "apex-ue-scene"`,
  everything but the vertex data.
- **`<Stem>.uemesh`**: little-endian binary with every mesh's buffers.

The blob is written first and the manifest last, each temp-then-rename, so a
reader that finds a manifest finds its blob. The output is deterministic.

### Manifest

Top-level keys in this order: `format`, `version`, `track_id`, `track_name`,
`track_display_name`, `source_track`, `source_crc`, `closed_loop`,
`length_cm`, `metadata`, `dressing`, `imported`, `mesh_blob`, `materials`,
`meshes`, `props`, `grid`, `centerline`, `pit_lane`, `start_finish`. **The
order is part of the format**: the client's catalog scan reads only the
first 64 KB (`FApexTrackSceneReader::LoadHeader`) and stops at the first
array, so every scalar and object the catalog needs comes before
`materials`.

| Field | Contents |
|---|---|
| `version` | 2, or 3 when a scene uses a version 3 field (`ue_export_io::scene_version`) |
| `track_id`, `track_name`, `track_display_name`, `source_track` | From the YAML; the catalog row prefers the display name |
| `source_crc` | The YAML's CRC-32 with carriage returns dropped: the server's `ContentCrc` |
| `metadata` | Country, city, category, environment, description, `latitude_deg` and `north_yaw_deg` (true north's yaw in the track frame, from the DEM georeference; the client places the sun with them) |
| `dressing` | Season and spectators |
| `imported` | v3: which importer wrote the export whole (`"ac"`) |
| `mesh_blob` | The blob's file name, resolved beside the manifest |
| `materials` | Sorted by key: `key`, `family` (`road`, `curb`, `surface`, `marking`, `pit_lane`, `decal`, `scenery`; the bake also emits `structure_*` and `horizon` keys), linear `base_color`. v3 adds `ground_set` on a surface, and `texture` (DDS relative to the manifest), `blend` (`opaque`/`masked`/`translucent`), `two_sided`, `roughness`, `alpha_cutoff` on scenery |
| `meshes` | One header per mesh in blob order: `name` (`{material_key}_{section:03}`), `material_key`, `vertex_count`, `index_count`; v3 adds `draw_distance_m` and `collision: false` |
| `props` | Transforms and asset keys, plus hints (`length_m`, `radius_m`, `span_m`, `text`) |
| `grid` | The starting grid, resolved as `track_loader.rs` resolves it |
| `centerline` | The sampled centerline |
| `pit_lane` | Width, box count, speed limit (its ribbon is in `meshes`) |
| `start_finish` | Line centre on the road, direction, road width: the start gantry's anchor |

Version 1 (buffers inline in `meshes`, no `mesh_blob`) still reads on both
sides. The Rust exporter writes 2 unless a scene needs 3; `scripts/ac_import.py`
writes 3.

### Mesh blob

```text
bytes 0..8   "APEXMESH"
u32          blob version = 1
u32          mesh_count
per mesh, in manifest order:
  u32 name_len, name bytes (UTF-8)
  u32 key_len,  key bytes  (material_key)
  u32 vertex_count v
  u32 index_count i
  u32 flags       bit 0: payload is a zlib stream (RFC 1950 with header)
  u32 stored_size
  payload         inflates to 32v + 4i bytes: f32 positions[3v], normals[3v],
                  uvs[2v], u32 indices[i]
```

Each mesh is compressed on its own and stored raw when that is not smaller.
`ue_export_io::read_scene` and the client reject a bad magic, a blob version
past 1, a payload of the wrong size, an index out of range, trailing bytes,
or a blob that disagrees with the manifest's headers.

### Conventions at the boundary

The export is already in Unreal's frame; nothing downstream converts it.
Units are centimetres and degrees; position track `(x, y, z)` m becomes UE
`(100x, -100y, 100z)` cm; yaw `θ` rad becomes `-θ` in degrees. That mapping
is a mirror (determinant -1) and flips handedness by itself, so geometry is
emitted with ordinary counter-clockwise winding and **not** reversed again
(`winding_matches_unreals_front_face_convention`). Meshes are cut into 250 m
sections and merged per (section, material).

## Checking it

- `cargo test -p track-core`: `.ats` round trips and validation, v1
  migration, byte-identical repeat saves, saving never touches the YAML
  (`core/tests/ats_scene.rs`); every real YAML still reads (`compat.rs`);
  export round trip, key order, v1 compatibility, damaged blobs refused,
  `source_crc` check vector, winding (`ue_export.rs`).
- Golden blob pinned on both sides: `the_mesh_blob_layout_is_pinned` and
  `ApexSim.Track.Reader.GoldenBlob`; also `ApexSim.Track.Reader.Rejects`,
  `.Manifest`, `.Version3`, `ApexSim.Track.Dds.Parse`, and on the Rust side
  `a_version_3_manifest_carries_the_imported_extensions`.
- The server loader's unit tests: `cargo test --lib track_loader`.

## Traps

- `spawn_points[].position` is an index into the interpolated centerline,
  not into `nodes`; `checkpoints` and `sectors` are node indices.
- `banking` is radians and positive lifts the **left** edge.
- Run `ats-export` (and every `ats-*` tool) from the repo root: it resolves
  `content/tracks/{default,custom}` against the working directory.
- Never hand-edit an export or a sidecar; they are regenerated wholesale.
- A new manifest field the catalog needs must go before `materials`, or the
  64 KB head scan will not see it.
