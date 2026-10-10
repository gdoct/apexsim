# Road mesh

The road a car drives on, as triangles. `ats-export` writes
`<Stem>.road.msgpack` beside each track YAML: the rendered road, curbs,
run-off and ground bands and the pit lane, in the server frame, without the
render-only lifts, each triangle tagged with a surface. With
`[physics] road_contact = "mesh"` (the default) the server takes every wheel's
height, normal and contact class from it, so the road the sim drives on is
the road the client draws. Where along the lap a car is stays on the
centerline under either backend.

## Code

- `track-editor/core/src/road_mesh.rs`: the file format (`RoadMeshFile`,
  `RoadSurface`, the `CONTACT_*` classes) and the welding builder.
- `track-editor/core/src/ue_export.rs`: the physics bake. Every surface strip
  goes through `strip_physics`, which feeds the same triangles to the render
  mesh and the road mesh; `clipped_quad` keeps the pit lane off the road.
- `track-editor/core/src/ue_export_io.rs`: `write_road_sidecar`.
- `track-editor/core/src/bin/ats-export.rs`: `--flat-curbs`.
- `server/src/road_mesh.rs`: `RoadMesh` (load, grid, `contact_at`).
- `server/src/track_loader.rs`: `load_road_mesh`, called by
  `TrackLoader::load_from_file_with` / `load_sidecars` with the server's
  `RoadContactMode`.
- `server/src/physics.rs`: `query_track_surface` (the dispatch),
  `slope_and_banking`, `RoadContact::from_surface`, `seat_height`,
  `track_context_of` (grip).
- `server/src/config.rs`: `RoadContactMode`, `[physics] road_contact`.
- `scripts/ac_import/sidecars.py` (`road_mesh_payload`): the Assetto Corsa
  importer writes the same file from AC's physics meshes (see
  [ac-import.md](ac-import.md)).

## The file

`<Stem>.road.msgpack`, `rmp_serde::to_vec_named`, version 1
(`ROAD_MESH_VERSION`), gitignored like the other generated sidecars and
shipped in `Server/` by `build_release.ps1`; `initialize_content.ps1` rebakes
a circuit whose road mesh is missing.

- `vertices`: `[f32; 3]` in the server frame (metres, +X along the start
  line's heading, +Y left, +Z up, origin at the start/finish line). f32
  metres hold millimetres at 20 km from the origin.
- `triangles`: indices, counter-clockwise seen from above. The builder welds
  vertices by exact rounded equality and numbers them in first-seen order, so
  a bake is byte-identical run to run.
- `triangle_surface`: one index per triangle into `surfaces`.
- `surfaces`: `RoadSurface { key, contact, friction, valid_track, pit_lane }`.
  `contact` is 0 road, 1 curb, 2 run-off, 3 off, 4 pit lane. `friction` is a
  **multiplier** on the grip the class already has on that track (1.0 = the
  track's own figure); a generated mesh writes 1.0 everywhere, an importer
  scales from its own table. `valid_track` is "inside the track limits".
  `pit_lane` is informational; the server reads the class from `contact`.
- `source`: which generator wrote it, logged at load.

A generated mesh is the road at 1 m, the bands at their own steps, and the
pit lane: Monza is about 185 k triangles, 4 MB.

## The bake (`ats-export`)

The physics strips reuse the render path's profiles and samples, so the
vertices are the rendered ones, with these differences:

- **No render lifts.** Curbs, bands and the pit lane are drawn a few
  centimetres up against z-fighting; the physics copy is at the true height.
  Paint (markings, decals, wear bands, grid boxes) is not a surface and is
  not in the mesh. The ground grid and the horizon are not in it either:
  past the outermost band the ground sidecar is the terrain.
- **The verge is a step**: ground bands sit `VERGE_DROP_M` (8 cm,
  `terrain.rs`) below the road edge, as on screen.
- **Layer order matters.** The road is baked first, then the bands in layer
  order (tarmac run-off after the grass it crosses), then the curbs; the
  server breaks near-ties in favour of the later triangle (below).
- **The pit lane is kept off the road**: its physics quads are cut into 1 m
  columns (`CLIP_COLUMN_M`) and every column with a corner on the track's
  road is dropped, so a wheel on the road always reads road.
- **Facets the render drops are kept** (folds, slivers, twists); only
  triangles without a footprint are dropped, at load
  (`MIN_FOOTPRINT_M2`).
- `--flat-curbs` flattens the curbs onto the road edge's height, for a
  like-for-like comparison with the centerline backend (where curbs are
  flat). Without it a curb is 5 cm high in physics as on screen.

Surface classes: road and curbs are inside the track limits; run-off
(`runoff_*`), grass, gravel, sand and astroturf bands (`off_*`) are not; the
pit lane is class 4 with `valid_track: false`.

## The query (`RoadMesh::contact_at`)

At load the triangles are bucketed into a uniform grid (`CELL_M` 4 m,
coarser when the bounding box would need over `MAX_CELLS`), CSR-packed, with
a plane per triangle. `contact_at(x, y, z_ref)` is a vertical ray:

- Candidates are the cell's triangles in ascending index order; a point on a
  shared edge belongs to both (barycentric slack `EDGE_EPSILON`).
- Of the triangles containing the point, the result is the **highest at or
  below `z_ref + STEP_UP_M`** (0.5 m). `z_ref` is the asking point's own
  height (a wheel hub from the previous tick, the car's centre). This is what
  makes two levels work: under Suzuka's crossover a car in the slot finds the
  slot floor, one on the deck finds the deck.
- Two candidates within `TIE_M` (2 cm) of each other are one surface laid
  over another, and the **later index wins**, as the render draws them.
- `None` when nothing is under the point.

It allocates nothing and iterates nothing in hash order, which keeps the sim
deterministic.

## How physics uses it

`physics::query_track_surface` always samples the centerline first (nearest
point, lateral, heading, widths: everything keyed on a station or index).
With a mesh and a hit it then replaces:

- `elevation` with the triangle's `z`;
- the contact class with `RoadContact::from_surface` (road, curb, run-off,
  off, `PitLane`);
- slope and banking with the triangle normal's (`slope_and_banking`), **on
  road and pit-lane triangles only**. On a curb, run-off or ground triangle
  the centerline's slope and banking stand, because a curb's lip is a steep
  facet that would otherwise pull and roll the car as if the road were
  banked.
- grip: `track_context_of` multiplies the class's grip (weather and road
  state included) by the surface's `friction`.

Where the mesh has no triangle (past its outermost band, a hole, the clamped
inside of a tight hairpin where the mesh road is narrower than the
centerline's) the centerline sample stands whole, height and class from
`curbs.rs` included. It is not forced to "off": that would slow a car on the
asphalt at a sliver.

Each of the six queries a tick (the car's centre before and after
integration, four wheels) goes through this path. The four wheels each read
their own surface for grip and off-track drag, and the body settles on the
weighted plane through the four contact heights (`fit_contact_plane`); that
is the same under both backends (see
[../server/vehicle-physics.md](../server/vehicle-physics.md)).

**Seating.** The grid, the hotlap run-up and the garage place a car by
station, then take its height from `physics::seat_height`: the mesh under the
point, looked up from `SEAT_LOOKUP_M` (3 m) above the centerline's height, so
a slot on a bridge lands on the deck but never on a bridge over the road.

**Pit lane.** On the mesh the lane is its own class (road grip, off the
track for the lap). The centerline backend has no lane surface of its own and
recognises it from the pit sidecar instead (see
[../server/pit-lane.md](../server/pit-lane.md)).

## Configuration

- `[physics] road_contact = "mesh" | "centerline"` in `server.toml`
  (default `mesh`), env `APEXSIM_PHYSICS_ROAD_CONTACT`.
- A track without the sidecar drives on the centerline whatever the setting.
  With `centerline` the sidecar is left on disk and the load logs that it is
  present but unused.
- `TrackLoader::load_from_file` (no mode argument) loads **without** the
  mesh. The offline tools built on it (`apexsim-replay` simulate, render,
  guide and the other subcommands) and the AI survey by default drive the
  centerline backend.

## Checking it

```bash
cd server
cargo test --test road_mesh_test                    # the mesh against the centerline on Monza; grid seating
cargo test --test determinism_test                  # includes ..._on_the_road_mesh (Monza)
cargo test physics::                                # surface tests run under both backends (mesh_from_centerline)
cargo bench                                         # physics_step_monza_mesh, session_tick_8cars_monza_mesh
SURVEY_ROAD_CONTACT=mesh cargo test --release --test ai_race_start_test survey_ai_races_on_every_circuit -- --ignored --nocapture

cd track-editor
cargo test -p track-core --test road_mesh           # coverage, curb profile, band classes, byte-identical roundtrip
cargo test -p track-core --test road_mesh -- --ignored   # every_real_track_road_mesh_covers_the_road (whole calendar, slow)
```

The server-side tests on real circuits skip themselves when the sidecar has
not been baked. In the physics unit tests `mesh_from_centerline` lofts a mesh
from a synthetic track's own centerline formula, so the two backends are held
to the same answers.

## Traps

- **`friction` is a multiplier, not an absolute.** The weather and road-state
  bakes scale the class grip, never the mesh, which is shared between a
  session's copies of the track behind an `Arc`. Writing absolute friction
  into a mesh doubles the grip model.
- **Bake order decides near-ties.** A band baked after the one it overlaps
  wins the 2 cm tie. Reordering the bands in `ue_export.rs`, or taking the
  lower index in `contact_at`, reads tarmac run-off as the grass under it.
- **The mesh is stricter than `curbs.rs`**: it reads what is drawn, so a strip
  of grass between the road edge and a tarmac run-off is grass on the mesh,
  while `curbs.rs` counts everything within the run-off's reach as tarmac.
- **Offline tools drive the centerline**, the live server the mesh. A
  behaviour seen in a survey or a rendered showcase may differ on a live
  server; compare with `SURVEY_ROAD_CONTACT=mesh`.
- The mesh is ground, not barriers: walls stay in `<Stem>.walls.msgpack`.
- A change to the bake's surfaces or heights should go through the AI survey
  on both backends, since the AI is sensitive to verges and curbs.
