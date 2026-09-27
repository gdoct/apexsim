# Road mesh: wheel contact on triangles, beside the centerline

*Design, 2026-09-27; built the same day (steps 1–4 below), the default still `centerline` (step 5). It prepares the Assetto Corsa import (`docs/AC_IMPORT_FEASIBILITY.md`, blocker 2), whose tracks are triangle-mesh physics, and it is useful on its own: the road the server drives on becomes the road the client draws.*

## Status

Built: `track-core/src/road_mesh.rs` (the format and the welding builder),
the exporter's physics pass (every `strip_physics` call in `ue_export.rs`;
`ats-export --flat-curbs`), `write_road_sidecar`, the server's
`road_mesh.rs` (grid, planes, `contact_at`), `[physics] road_contact` /
`APEXSIM_PHYSICS_ROAD_CONTACT`, `TrackLoader::load_from_file_with`, the
query dispatch in `physics::query_track_surface` with the old
`APEXSIM_SURFACE_QUERY_BACKEND` stub removed, grid and hotlap seating
(`physics::seat_height`), the tests listed under "Verification", the
`*_mesh` benches, and the gitignore, `build_release.ps1` and
`initialize_content.ps1` entries.

Measured on Monza (`server/tests/road_mesh_test.rs`, 24 008 probes across
the road and the curbs every 5 m): no road probe without a triangle under
it, the mesh height within 4.0 mm of the server's `surface_elevation` at
most and 0.18 mm on average, the slope within 0.009 rad and the banking
within 0.002 rad, and every probe classed the same by `curbs.rs` and by
the mesh. Monza's sidecar is 4.1 MB, 185 k triangles. The two splines'
disagreement the design wanted measured is therefore millimetres.

What differs from the design below, on purpose:

- **`friction` is a multiplier** on the class's own grip for the track
  (the node's friction on the road, `curb_grip`, `off_track_grip`), not
  an absolute; a generated mesh writes 1.0 everywhere. So the weather bake
  (`SessionConditions::apply_to_track`) need not touch the mesh, which is
  shared between a session's copies of the track behind an `Arc`, and a
  generated track is neutral by construction. An importer scales from its
  own table.
- **Where the mesh has nothing under a point the centerline sample stands
  whole** (height, and the class from `curbs.rs`), not a forced `Off`:
  the mesh road is narrower than the centerline's on the clamped inside of
  a hairpin, and a road/curb T-junction leaves millimetre slivers; forcing
  `Off` there would slow a car on the asphalt.
- **Slope and banking come from the normal on road and pit-lane triangles
  only.** A curb's lip is a 10° facet, and a car whose centre crossed one
  was pulled sideways and rolled as if the road were banked that much; on
  a curb, run-off or ground triangle the centerline's values are used.
- **Near-ties go to the later triangle** (`TIE_M`, 2 cm): a band laid
  over another sits at the same physics height once the render lifts are
  gone, so the exporter bakes the bands in layer order (the tarmac run-off
  after the grass it crosses) and the server takes the higher index where
  two coincide, as the render draws them. The first version took the
  *lower* index and read Suzuka's run-off as grass. The pit lane, baked
  last, is kept off the road altogether: its physics quads are cut into
  1 m columns and every column with a corner on the track's road dropped.
- **The pit lane is `RoadContact::PitLane`** — road grip, but `valid_track:
  false` so a pit-lane pass cannot become a legal lap.
- **The physics bake keeps facets the render drops** (folds, slivers,
  twists) and drops only those without a footprint (a curb's outer face);
  the highest-under-the-wheel rule makes an overlap harmless.
- **The verge is a step**: ground bands sit at the true verge height,
  `VERGE_DROP_M` (8 cm) below the road edge, where the centerline backend
  blended over 1.5 m. Leaving the road on the mesh is an 8 cm drop, as on
  screen.

The AI survey on Monza, Spa, Suzuka and Zandvoort (three car classes,
180 s each, `SURVEY_ROAD_CONTACT=mesh`): out-of-shape counts, contact and
sliding car-seconds are the same or lower on the mesh; **off-track
car-seconds are higher** (Monza 0.4–0.6 to 5–8, Suzuka 0 to 4–18, Spa
0–0.7 to 2–3, Zandvoort 0–0 to 1–3). Every extra off tick is a car whose
centre is a few centimetres to a metre past the road edge where the scene
puts grass between the road and the tarmac run-off (Suzuka's left-hand
run-off from station 618 starts 3.1 m off the edge), or an astroturf strip
over the run-off: the mesh reads what is drawn, where `curbs.rs` counts
everything within the run-off's outer reach as tarmac, inner gap included.
So the mesh is stricter and more faithful than the centerline there, not
worse; whether the groomer should leave that gap is a content question.
Bench (`cargo bench`, Monza): `physics_step` 1.38 → 1.44 µs,
`session_tick_8cars` 12.1 → 15.8 µs.

Open: the survey on the remaining circuits and flat curbs before the
default goes to `mesh`; per-wheel grip; crown and bumps; the AC importer.

## Summary

The server keeps the centerline for everything that is about *where along the lap* a car is: progress, laps, sectors, checkpoints, track limits by station, the AI, the racing line and the grid. It gains a second, optional description of the road for everything that is about *what the tyre is standing on*: a triangle mesh of the drivable surfaces, each triangle tagged with a surface (road, curb, run-off, pit lane, grass...). The four wheels take their height, their surface normal and their surface class from the mesh when the track has one, and from the centerline as today when it does not.

The mesh is a new server sidecar, `<Stem>.road.msgpack`, beside the YAML like `.ground`, `.curbs` and `.walls`. There are two ways to write it:

- **Generated**, for every existing circuit: `ats-export` already lofts the road, curbs, run-off bands and pit lane into triangles for the client. It writes the same triangles, without the render-only offsets, as the physics mesh. The rendered road and the simulated road then share their vertices.
- **Imported**, later: the AC importer writes the kn5 physics mesh (`2.kn5`, `NN<KEY>` meshes) into the same file, with the surfaces from `surfaces.ini`. The format is designed so that is a translation, not a new format.

Rough effort for the generated path and the server side: **2–3 weeks**, most of it validation (AI survey, determinism, grip probe) rather than code.

## Why

Today the road under a wheel is a formula: the nearest centerline point, lerped, then `z − lateral × sin(banking)` on the asphalt (`server/src/physics.rs` `surface_elevation`), blending to the 4 m ground heightfield past the edge. That has three consequences:

1. **The server and the client do not drive the same road.** The exporter samples the centerline at 1 m on its own spline (`track_path.rs`, ~2 m Catmull-Rom), clamps laterals to `0.85/κ` on the inside of tight bends and drops folded facets; the server projects onto its own 1 m spline. Curbs are a 3D profile on screen (5 cm high) and flat in physics. The ground sidecar is 4 m bilinear i16, so it smooths the verge. The mismatches are centimetres, but they are there, and they grow wherever the road is odd (Suzuka's deck, Zandvoort's pit exit beside the banking).
2. **The road can carry nothing but banking.** No crown, no camber change across the road, no bumps, no curb profile, no second level: a centerline cannot say that there are two roads at this `(x, y)`, which is why the Suzuka crossover needs the "hold road height within 2.5 m of the edge" special case and a car leaving the bridge falls into the slot.
3. **An AC track cannot be driven as authored.** Its road is triangles. Reduced to a centerline it loses exactly what makes it an AC track, and the AC road mesh cannot be shown because it would not sit on the simulated surface.

A mesh fixes all three, and for the generated case it changes almost nothing about how a car drives: the generated mesh *is* the centerline formula, sampled.

## What stays on the centerline

These key on a centerline index or a station, and keep doing so under either backend. The mesh backend still computes the centerline sample for every query (it is cheap, and it is cached per car in `nearest_centerline_idx`); it only replaces the height, the normal and the surface class.

| Reader | Where | Uses |
|---|---|---|
| Lap progress, checkpoints, sectors, teleport guard | `physics.rs` `update_track_progress_3d`, `checkpoint_distance_m` | station |
| Grid and hotlap placement | `seed_track_progress`, `pose_at_station`; `game_session.rs` | station, heading, then a height (see "Seating a car") |
| Curb and run-off widths (centerline backend) | `road_contact` via `curbs.rs` | station, lateral |
| AI | `ai_driver.rs` (`nearest_track_point`, widths, `lateral_offset_m`, recovery toward the centerline) | index, lateral, widths |
| Racing line and its speed profile | `racing_line.rs` | raceline / centerline, `base_grip` |
| Demo mode's raceline walk | `game_session.rs` | index |
| Loader's checkpoints, sectors, grid slot z | `track_loader.rs` | index, station |

`SurfaceQuerySample`'s `nearest_point`, `lateral_offset`, `heading_rad`, `width_left/right` stay centerline values under both backends. `elevation`, `slope_rad`, `banking_rad`, `surface_type` and `grip_modifier` come from the mesh when there is one.

## The sidecar: `<Stem>.road.msgpack`

Same conventions as the other three: msgpack via `rmp_serde::to_vec_named`, optional, beside the YAML, written by `ats-export` (`ue_export_io.rs`, temp file then rename), read by the server at load (`track_loader.rs`), a bad file logged and ignored. Gitignored beside the others (`content/tracks/real/*.road.msgpack`), shipped in `Server/` by `build_release.ps1`.

```rust
pub struct RoadMeshFile {
    pub version: u32,                 // 1
    /// Server frame: metres, +X along the start line's heading, +Y left, +Z up,
    /// origin at the start/finish line. Not Unreal centimetres.
    pub vertices: Vec<[f32; 3]>,
    /// Counter-clockwise seen from above (normal up). Indices into `vertices`.
    pub triangles: Vec<[u32; 3]>,
    /// One entry per triangle: an index into `surfaces`.
    pub triangle_surface: Vec<u16>,
    pub surfaces: Vec<RoadSurface>,
    /// Which generator wrote it and from what: "ats-export <crc of the YAML>",
    /// "ac-import ks_zandvoort". Logged at load; not interpreted.
    pub source: String,
}

pub struct RoadSurface {
    pub key: String,          // "road", "curb_red_white", "runoff_asphalt", "grass", AC's "TRM-ZNDV"
    pub contact: u8,          // 0 road, 1 curb, 2 runoff, 3 off, 4 pit lane
    pub friction: f32,        // multiplies the car's grip, like a node's `friction`
    pub valid_track: bool,    // inside the track limits (AC: IS_VALID_TRACK)
    pub pit_lane: bool,       // AC: IS_PITLANE; nothing reads it yet
}
```

Notes on the choices:

- **f32 metres** are good to ~2 mm at 20 km from the origin, which covers the Nordschleife and every AC circuit. The ground sidecar's i16 centimetres (±327 m) would not hold a hill climb, and the mesh has no such limit.
- **Indexed, shared vertices.** The exporter's render meshes do not share vertices between quads; the physics bake welds them (exact-equality weld within a strip), which roughly halves the file and makes adjacency well-defined for the edge rule below.
- **The surface table is AC's `surfaces.ini` in miniature.** `friction` and `valid_track` translate directly; `contact` is ApexSim's own class (what the force feedback and the off-track drag need), which the AC importer derives from the key (`KERB` → curb, `GRASS`/`SAND`/`OUT` → off, `PITS` → pit lane, else road). AC's fake bumps (`SIN_HEIGHT`, `SIN_LENGTH`), damping and vibration are left out of v1; a field added with a serde default is a v1 file an older server reads.
- **Size.** A generated circuit is the road at 1 m (two triangles a metre), curbs, run-off and the other surface bands at 4 m, and the pit lane: roughly 50–150 k triangles, 2–4 MB. An AC physics mesh is 150 k–1.5 M triangles, 5–50 MB. Both load in well under a second.

## The server: `road_mesh.rs`

A new module beside `ground.rs` / `walls.rs`, stored as `TrackConfig.road_mesh: Option<RoadMesh>` (`#[serde(skip)]`, like the others).

**Loading** builds a uniform 2D grid over the triangles' bounding boxes, CSR-packed like `walls.rs` (`CELL_M` 4 m for a generated mesh; the loader may pick larger cells for a sparse one), plus a per-triangle plane (`n`, `d`) so a query is a dot product rather than a solve. It validates: indices in range, one surface per triangle, finite coordinates, no zero-area triangle (dropped with a count in the log).

**The query** is a vertical ray down at `(x, y)` from `z_ref + STEP_UP_M`:

```rust
pub struct MeshContact {
    pub z: f32,
    pub normal: [f32; 3],   // unit, z > 0
    pub surface: u16,
    pub triangle: u32,
}
pub fn contact_at(&self, x: f32, y: f32, z_ref: f32) -> Option<MeshContact>;
```

- Candidates are the triangles in `(x, y)`'s cell, in ascending index order.
- A candidate contains the point if its 2D barycentric coordinates are all ≥ −ε (ε ~1e-5, relative). On a shared edge both neighbours contain it; the **lowest triangle index wins**. With welded vertices both give the same `z` there, so the choice only matters for the surface, and it is deterministic.
- Of the candidates that contain the point, the result is the **highest one whose `z` is at or below `z_ref + STEP_UP_M`** (0.5 m). `z_ref` is the wheel's hub height from the previous tick, or the car's own `z` for the centre queries. That is what makes two levels work: under Suzuka's bridge the car in the slot is below the deck by more than half a metre, so it finds the slot floor; on the deck it finds the deck. A car that drops off an edge finds the next surface down, or nothing.
- `None` means no mesh here: the caller falls back (below).

It is pure: no allocation per query, no HashMap iteration, no RNG. The determinism test covers it by running once more with the mesh backend on.

**Cost.** Today a car costs six surface queries a tick (the centre before and after integration, four wheels) plus one nearest-centerline search. The mesh adds a cell lookup and a handful of point-in-triangle tests to each of the six: at 1 m road triangles and 4 m cells that is ~16–40 candidates, microseconds per car. `benches/physics_tick.rs` gets a mesh variant of `physics_step_monza` and `session_tick_8cars_monza` so the number is measured, not assumed.

## Physics integration

The switch is per track and per server, not per process:

- A track uses the mesh when it has a `road_mesh` **and** the server's `[physics] road_contact` is `"mesh"` (new key; `"centerline"` is the default until the validation below passes, then `"mesh"`). `APEXSIM_PHYSICS_ROAD_CONTACT` overrides it through the usual env mechanism. The existing `APEXSIM_SURFACE_QUERY_BACKEND` `OnceLock` and `query_track_surface_mesh_heightfield_stub` (which reads the *procedural* terrain, not the road) are removed.
- A track without the sidecar drives on the centerline whatever the setting, so a server with a mix of generated, imported and old tracks just works.

What changes in `update_car_3d` when the mesh is in use:

| Today (centerline) | Mesh |
|---|---|
| `elevation` = `surface_elevation` (banking shear, blend to ground) | `MeshContact.z`; fallback below |
| `slope_rad`, `banking_rad` from the centerline point | derived from `MeshContact.normal` against the centerline heading: `slope = asin(n·fwd)`, `banking = asin(n·left)`, signs as today. Everything downstream (gravity on the bank, pitch and roll, the hub plane) keeps reading slope and banking, so it needs no change. |
| Contact class from `curbs.rs` widths by station and lateral (`road_contact`) | the triangle's `surface.contact`; `off_track` = `!surface.valid_track`. The curbs sidecar stays for the centerline backend and for the fallback. |
| `grip_modifier` = the node's `friction` (× `RUNOFF_GRIP_FACTOR` on run-off) | the surface's `friction`, weather-scaled like the node's today (`SessionConditions::apply_to_track` scales the table too) |

Unchanged on purpose in the first cut:

- **Grip stays per car, not per wheel.** `effective_grip` is the car-centre surface's grip for all four tyres today. The mesh gives each wheel its own surface, and per-wheel grip is the right model, but it changes how every car behaves on every curb and verge; it goes in as its own step, after the survey has shown the mesh alone is neutral.
- **Hub heights** still come from the centre plane; only the contact heights under each wheel change.
- **Walls** stay `walls.msgpack`. The mesh is ground, not barriers.

**Fallback.** Where `contact_at` returns `None` (off the edge of the mesh, or a hole), the query uses the ground heightfield if there is one and the centerline edge hold otherwise, exactly as `surface_elevation` does past the road edge today, and the contact is `Off`. For a generated mesh this happens only past the outermost surface band, which is seated on the same terrain the ground sidecar samples, so there is no step at the boundary (the render lift is left out of the physics mesh; see below). For an AC mesh the physics mesh usually covers everything drivable, and the fallback is the grass beyond it.

**Seating a car.** The grid, the hotlap run-up and the garage place a car by station (`pose_at_station`), then set its `z` from `road_surface_z`. Under the mesh the `z` comes from `contact_at` with `z_ref` a few metres above the centerline's own height, so a grid slot on a bridge lands on the deck.

## Generating the mesh for centerline tracks (`ats-export`)

A new bake, `road_physics`, in `track-editor/core/src/ue_export.rs`, run in `bake_all_with_dem` after the curbs and surface bands, stored on `Baked`, written by a `write_road_sidecar` in `ue_export_io.rs` and listed by `ats-export`'s summary line. It reuses the render path's `extrude`, profiles and samples, so the vertices are the rendered ones, with three differences:

1. **No render lifts.** Curbs are drawn 2 cm up (`CURB_LIFT_M`), bands 3–7 cm (`0.03 + layer × 0.008`), the pit lane 2 cm (`PIT_LIFT_M`), markings and wear bands higher still: offsets against z-fighting, not geometry. The physics bake passes a lift of zero. Markings, decals, wear bands, edge lines and the grid boxes are paint on a surface that is already there, and are not in the physics mesh at all.
2. **Everything a wheel can touch, with its class.**

   | Render mesh | Physics surface | contact | friction | valid |
   |---|---|---|---|---|
   | `road` (per node `friction`) | `road` | road | the node's `friction`* | yes |
   | `curb_{style}` (3D profile) | `curb` | curb | `curb_grip` / base | yes |
   | `surface_asphalt_runoff`, `surface_concrete` | `runoff_*` | runoff | `RUNOFF_GRIP_FACTOR` | no |
   | `surface_grass`, `_gravel`, `_sand`, `_astroturf` | `off_*` | off | `off_track_grip` / base | no |
   | `pit_lane` | `pit_lane` | pit lane (drives as road) | 1.0 | yes |
   | underpass slot floor and deck | `road` / `off_*` by what they carry | | | |

   \* a node's `friction` varies along the lap, so the road's triangles carry it as one surface per distinct value (in practice one or two per circuit).
   The ground grid and the horizon are *not* in it: past the bands the ground sidecar is already the terrain, and the mesh would only duplicate it at a coarser resolution.
3. **No holes.** The render path may drop a facet (a fold on the inside of a tight bend, a sliver, a twist) and nobody notices a missing triangle a centimetre wide. In physics a hole is a wheel falling through to the fallback. The physics bake reports every dropped facet with its station, and the coverage test below fails on any gap inside the road's width.

**The curb profile** is the one behavioural change the generated mesh brings: today a curb is flat in physics and 5 cm high on screen. With the mesh it is 5 cm high in both, which is realistic and which the AI will feel. The bake takes a `--flat-curbs` option (curb triangles flattened onto the road edge's height) so the first survey can separate "the mesh" from "the curbs got a profile".

**Optional later, cheap once the mesh exists:** a crown (1–2% cross-fall to each edge), and bumps (the client's `RoadTug` value noise, baked into the vertices so the render and the physics agree). Both are profile changes in the exporter and need no server work. Neither is in v1: the point of v1 is that nothing changes.

## Verification

Each of these is a test, not a manual check:

- **Round trip** (`track-core`): write, read, identical; the file is byte-identical across two bakes (`repeated_bakes_are_byte_identical` extended).
- **Coverage** (`track-core`, every real circuit, `#[ignore]`d with the other whole-calendar bakes): every metre of the lap, probe laterals from `−width_right` to `+width_left` in 0.25 m steps and past the edge through each surface band; every probe inside the road and curbs hits a triangle. Report the misses by station.
- **Agreement with the centerline** (`track-core` and server): at those probes the mesh height matches `offset_point` (the render road's own height) within 1 mm on the road, and the server's centerline `surface_elevation` within 2 cm (the two splines' difference, which this measures for the first time). The classes match `curbs.rs` at every probe away from a band boundary.
- **Query** (`server`, `road_mesh.rs`): a point on a shared edge picks the lower index; two stacked levels pick by `z_ref`; a hole returns `None`; the normal of a banked quad reproduces its bank angle.
- **Physics**: the existing surface tests (`test_curb_counts_as_on_track`, `test_tarmac_runoff_is_asphalt_but_off_the_track`, `banking_pulls_the_car_toward_the_low_edge`, `banked_road_does_not_load_the_lower_wheels`, `feedback_reports_the_surface_under_each_wheel`, `test_bridge_deck_holds_road_height_past_the_edge`...) run under both backends. `test_mesh_backend_stub_overrides_elevation_from_heightfield` goes with the stub.
- **Determinism**: `tests/determinism_test.rs` runs Monza under both backends.
- **Behaviour**: the AI survey (`survey_ai_races_on_every_circuit`) and the grip probe's ideal-lap times, mesh against centerline, with `--flat-curbs` first. The acceptance bar is "no circuit measurably worse"; lap times should agree within noise.
- **Bench**: `physics_step_monza` and `session_tick_8cars_monza` with the mesh.

## Plan

| Step | What | Effort |
|---|---|---|
| 1 | Format: `RoadMeshFile` in `track-core` and the server (one struct each, as for the other sidecars); `road_mesh.rs` loader, grid, `contact_at`; unit tests | 2–3 days |
| 2 | Exporter: `road_physics` bake (no lifts, surface table, weld, hole report), the sidecar writer, `--flat-curbs`; round-trip, coverage and agreement tests | 3–4 days |
| 3 | Server: `[physics] road_contact`, `TrackConfig.road_mesh`, the query dispatch, slope/banking from the normal, class and grip from the surface, fallback, seating; the physics tests under both backends; stub removed | 3–4 days |
| 4 | Validation: determinism, bench, AI survey and grip probe against the centerline baseline, flat curbs then profiled; fix what they find; `build_release.ps1` and `initialize_content.ps1` treat a missing `.road.msgpack` like a missing sidecar | 2–4 days |
| 5 | Default to `mesh` | — |
| Later | Per-wheel grip; crown; bumps; AC importer writes the sidecar from `2.kn5` + `surfaces.ini` (then the AC road mesh can be the rendered road) | separately |

Nothing on the client changes: it has no physics. The racing-line dots and the TV cameras already trace the rendered road, which the physics road now equals.

## For the AC import

What this leaves for the importer, when it comes:

- Write `<Stem>.road.msgpack` from the physics kn5: every `NN<KEY>` mesh's triangles, converted to the server frame (AC is Y-up and mirrored; the origin moves to the start line, `AC_TIME_0_L/R`), one `RoadSurface` per `surfaces.ini` entry. Wall meshes (`WALL` keys) go to `walls.msgpack` instead, as the feasibility doc says.
- Write the YAML centerline from `fast_lane.ai` as before; it is still the lap's spine. Its heights no longer matter for contact, only for anything that reads the centerline's own `z` (the minimap's profile, the racing line's grade), so the importer can take them from the mesh.
- The kn5 road can then be drawn as it is, because the car drives on it.
- Expect an AC mesh to have holes and overlaps a generated one does not (groove meshes, duplicate kerb strips). The highest-below-`z_ref` rule handles overlaps; the coverage test, run against an imported track, finds the holes.

## Open questions

- **Class from the mesh or from `curbs.rs`?** This design takes it from the mesh when there is one, so an AC track needs no curb sidecar. For a generated track the two agree by construction, and the agreement test proves it.
- **Where a surface's friction comes from for a generated track.** The table above ties it to the constants that exist today (`curb_grip`, `RUNOFF_GRIP_FACTOR`, `off_track_grip`) so that nothing changes; they could later move into the `.ats` surfaces.
- **The pit lane** becomes drivable ground it never was (today it is whatever the ground sidecar says). Nothing simulates a pit lane yet, and `pit_lane` on the surface is there for when something does.
