# Assetto Corsa import

Two command-line tools turn content from the player's own Assetto Corsa
(AC) install into ApexSim content: `tools/ac_import.py` makes a complete
track (AC's geometry drawn by the client, AC's physics mesh driven on by
the server) and `tools/ac_car_import.py` makes a custom car (AC's model
as GLBs, AC's physics figures mapped into a `car.toml`). The launcher runs
both for a player who has no checkout.

## The legal line

- **Local only.** The tools read the player's own install and write to
  `{tracks,cars}/custom` and `build/tracks` (or an install's
  `Game/` and `Server/` with `--root`). Those folders are gitignored and
  never shipped: `build_release.ps1` / `build_game_standalone.ps1` leave
  them out unless `-IncludeCustomTracks` / `-IncludeCustomCars` is given,
  which is for the player's own package, not a release.
- **Nothing bundled.** ApexSim ships converters, never AC geometry,
  textures or physics data.
- **CSP-encrypted content is refused, never decrypted.** A kn5 protected by
  Custom Shaders Patch raises `kn5.EncryptedKn5` (including the
  `acd.checksum.e` block CSP appends after the node tree). A car's
  `data.acd` is not CSP encryption: it is Kunos' own packaging of every
  car, read routinely by AC tools, and the car importer decodes it.
- **The tools say so.** The first non-dry run prints a notice (personal
  use, mod authors' terms may forbid conversion), remembered in
  `.cache/ac_import/notice_shown`.
- An import keeps AC's real names (`ui_track.json`, `ui_car.json`): the
  no-trademarks rule ([naming](naming.md)) is for what ApexSim ships, and
  imports are never shipped.

## Code

- `tools/ac_import.py` -> `tools/ac_import/`: `kn5.py` (kn5 reader,
  shared with the car importer), `ai.py` (`.ai` splines), `ini.py`
  (`models*.ini`, `surfaces.ini`, `drs_zones.ini`, `ui_track.json`, layouts),
  `frame.py`, `trigrid.py` (triangle index), `physics.py` (physics world,
  contact classes, walls, ground), `centerline.py` (spine, cross-sections,
  YAML), `scene.py` (mesh selection, material classification, merging),
  `site.py` (height, place and north), `textures.py` (DDS conversion),
  `export.py` (export manifest and blob),
  `sidecars.py` (msgpack sidecars), `pit.py` (the pit sidecar),
  `validate.py` (checks), `prompt.py` (asking for a new name), `cli.py`.
- `tools/ac_car_import.py` -> `tools/ac_car_import/`: `acd.py`
  (`data.acd`), `data.py` (INIs, LUTs, `ui_car.json`, skins), `physics.py`
  (the mapping), `model.py` (part split, seating, lamps, mirrors,
  materials), `glb.py` (deterministic GLB writer), `liveries.py`,
  `toml_out.py`, `cli.py`.
- `tools/importer-requirements.txt` (numpy, Pillow, PyYAML, msgpack, as
  ranges), `tools/track_dirs.py`, `tools/generate_track_previews.py`.
- `apexsim/launcher/main.cpp` (`StartImport`, `FindPython`, `VenvDir`,
  `DetectKind`).
- Client side of imported tracks: `ApexSim/Track/ApexTrackSceneReader`,
  `ApexTrackSceneBuilder`, `ApexDdsReader`, `ApexGroundMaterials`.

## Tracks: `ac_import.py`

```powershell
python tools/ac_import.py "E:\SteamLibrary\steamapps\common\assettocorsa\content\tracks\ks_zandvoort"
python tools/ac_import.py <folder> --list                 # the layouts, their default stems, fast_lane.ai or not
python tools/ac_import.py <folder> --layout layout_gp     # one layout ("." = the folder's own models.ini)
python tools/ac_import.py --all <ac>\content\tracks       # every importable layout, one summary line each
#   --stem NAME, --display-name TEXT, --textures kit|ac|flat, --max-texture N (2048),
#   --force, --dry-run, --root <ApexSim install>, --dem fetch|cache|off
# then restart the server, and restart the game or run apexsim.track.Rescan
```

Without `--layout` every layout of the folder is imported. `--stem` needs a
single layout. Exit code 1 when any layout is skipped or fails a check.

### Outputs

Server, in `tracks/custom/<Stem>/`:

- `<Stem>.yaml`: 5 m nodes (widths, z, banking), `raceline`, `sectors`,
  `spawn_points`, `drs_zones`, a fixed `track_id`, `name` /
  `display_name`, `metadata` (`category: Imported`, and `altitude_m`,
  `latitude_deg` and `longitude_deg` where the place is known: see "Where
  the circuit is").
- `<Stem>.road.msgpack`, `.walls.msgpack`, `.ground.msgpack`,
  `.curbs.msgpack`: four sidecars, written by the importer, and
  `<Stem>.pit.msgpack` (see "The pit lane") when the layout has an
  `ai/pit_lane.ai` and `AC_PIT_n` markers; without one there are no pit stops.
- `<Stem>.ats`: a minimal scene with `"imported": "ac"` and
  `"external_sidecars": ["ground", "curbs", "walls", "road"]` (plus `"pit"`
  with a pit sidecar).
- `<Stem>.import.json`: the report (source folder and layout, tool
  version, the CRC of every AC file read, frame, physics surfaces and
  their friction, scene counts, every material's classification, textures,
  checks, warnings, and the exact `rebuild` command). It carries no clock.

Client, in `build/tracks/` (or `Game/Tracks` with `--root`):
`<Stem>.uescene.json` + `<Stem>.uemesh` in export format version 3,
`<Stem>.textures/*.dds`, and `previews/<Stem>.png` (AC's `preview.png`,
else `outline.png`, else drawn from the centerline).

Re-running writes byte-identical files. The stem defaults to CamelCase of
folder and layout (`KsRedBullRing_LayoutGp`); `track_id` is a UUID v5 of
the lower-cased folder and layout, so a re-import keeps its id. A stem
that is a shipped circuit (`default/<Stem>/<Stem>.yaml`) is refused; an
existing import is replaced only with `--force`. At a terminal the
refusal becomes a prompt for another name (`prompt.py`: empty skips, `!`
replaces an earlier import); with no terminal the refusal stands.

### How AC is read

- **Frame** (`frame.py`). AC is Y-up; the conversion is the pure rotation
  `(X, Y, Z) = (x, -z, y)`, no mirror, so the physics mesh's authored index
  order already faces up. Then the track frame: origin at the midpoint of
  `AC_TIME_0_L/R`, seated on the physics road under it (node 0 at z = 0),
  +X along the AI line's direction there, +Y left.
- **`.ai` files** (`ai.py`): header, `count x (x, y, z, length, id)`, then
  an `i32 extra_count`, then `extra_count x 18 f32` records (speed, gas,
  brake, radius, `side_left`, `side_right`, ...). `side_left`/`side_right`
  are only a fallback; the edges come from the physics mesh.
- **Physics** (`physics.py`). Every mesh named `<digits><KEY>` across
  **all** of the layout's kn5s is physics, as AC finds them (Kunos keep
  them in a non-renderable kn5, mods often draw them): the longest
  `surfaces.ini` KEY the name starts with; `<digits>WALL...` is a wall.
  `surfaces.ini` is the layout's plus `<ac>/system/data/surfaces.ini` and
  AC's system defaults. Contact class (`contact_for`): pit lane
  (`IS_PITLANE` or a `PIT` key), curb (`KERB`/`CURB`...), off
  (`GRASS`/`SAND`/`GRAVEL`/...), an invalid surface with friction >= 0.85
  run-off and below it off, else road; `IS_VALID_TRACK` is the track
  limits. AC's absolute friction becomes the server's multiplier
  `(f / road_f) / CLASS_GRIP[class]`, so the sim ends at AC's ratio after
  its own per-class grip. A surface keeps AC's KEY in the road mesh,
  except an off one that `ground_set_for` reads as gravel or sand, which
  is written `off_gravel` / `off_sand` (`road_mesh_key`) so the server's
  trap ground bogs a car down (`OffGround::from_surface_key`); AC's own
  `1SAND` drives as grass. Walls are the steep faces of the WALL meshes as
  segments, kind from the name (tyres, concrete, else armco). The ground
  sidecar is the highest physics surface on a 4 m grid, holes filled from
  neighbours (clamped at +-320 m from the start line).
- **Centerline** (`centerline.py`). The AI line, rolled to start at the
  start line, then a cross-section across the physics mesh every metre:
  the run of road-class triangles around the line gives both edges, its
  middle is the node, the kerb and run-off runs beyond the edges are the
  curbs sidecar, and z and banking come from the mesh. The middle is
  smoothed along the lap (a running median, then a Gaussian); quantised to
  the 0.25 m section step it zig-zags into a 100 m radius on straights.
  Node 0 is the road's middle at the timing gate's foot on the AI line, not
  the gate's middle (a gate off the line puts a kink in the raceline the
  speed profile brakes for). Sectors from `AC_TIME_1/2` (one line only: a
  second boundary halfway to the finish; none: even thirds). Grid slots
  from `AC_START_n` as `spawn_points`, offsets from node 0. DRS from
  `drs_zones.ini`, re-based from the AI line's start.
- **Scene** (`scene.py`). Every renderable kn5 mesh at `lodIn == 0`; dropped:
  lower LODs, `AC_*` logic objects, overlays by mesh or see-through material
  name (`GROOVE`, `SKIDMARK`, `KSLAYER`), and any material AC draws at
  `alpha = 0` (how mods hide renderable physics meshes). Classification is
  **per material**: each mesh is sampled at its triangles' centres against
  the physics (three quarters must lie on physics, 60% in one class) and
  the material's meshes vote by size. On road -> `road_ac` (family road,
  so the wet look applies); on grass/sand/gravel -> `ac_<set>` (family
  surface with a `ground_set`). With no physics under it: a
  grass/sand/gravel/road **name** first, and only then a plain
  `ksMultilayer` shader is terrain (`ksMultilayer_fresnel*` is road,
  `ksMultilayer_objsp` an object). A `KERB`/`CURB`/`CORDOL` name is never
  kit, alpha-tested or blended materials are never kit. Everything else is
  `scenery_<material>` with AC's diffuse texture on the car parents
  (opaque / masked / translucent from the kn5's alpha flags and shader,
  two-sided, roughness from `ksSpecularEXP`). Kit surfaces get world-metre
  planar UVs. Meshes merge by (material, 250 m cell, draw-distance bucket,
  collision) into a few hundred draw calls; `lodOut` is the draw distance;
  only meshes standing on physics carry collision (what the racing line
  and cameras trace).
- **`--textures`**: `kit` (default: kit ground sets for ground, AC for the
  rest), `ac` (AC's textures everywhere), `flat` (average colours, no
  textures).
- **Textures** (`textures.py`). Only diffuse maps of scenery materials. A
  BC1/BC3 source with a full mip chain is copied (top mips dropped over
  `--max-texture`); anything else (most Kunos DDS ship without mips) is
  decoded with Pillow, resized, box-mipped and range-fit encoded to BC1
  (opaque) or BC3 (alpha) in numpy. A texture that fails to convert is a
  warning and its material draws flat.

### The pit lane

`pit.py` turns `ai/pit_lane.ai` (the line from the track through the lane and
back) and the `AC_PIT_n` dummies into the server's `PitSidecar`
([pit-lane](../../apexsim/docs/server/pit-lane.md)):

- **The lane** is the part of AC's line that is off the track, with 30 m of
  lead-in at each end, resampled every 2 m, z from the physics under it. Off
  the track is past the road's edge by 0.5 m (measured on the road middle and
  widths) or behind a wall from the road's middle (`_walled_off`: a wall at
  the line's height, not a bridge's parapet): where the lane's asphalt joins
  the track's, the road's edge takes the lane in and only the pit wall tells
  them apart (Red Bull Ring's pit straight, Laguna Seca's start line), and
  reading the edge alone cut the lane to its stretch along the straight. The
  entry's lead-in is counted from where AC's line parts from the racing line
  (1.5 m off the AI line, at most 100 m before it leaves the track;
  `parts_from_racing_line_m` in the report), so a car coming in on the racing
  line is on the lane before it turns off (Spa's turns off at 40 degrees
  through the bus stop). AC's line starts and ends on the racing line,
  hundreds of metres before the lane proper, and the server hands the AI to
  the lane at its first node.
- **Stations** (`entry_station_m`, `exit_station_m`) are the arc length of the
  YAML's nodes, the server's lap, not the AI line's: the road's middle is
  longer (Red Bull Ring by 1.3%), and a station taken from the AI line put
  the pit entry 50 m wrong.
- **The speed limit** (80 km/h, AC's default: a session rule, not a file) holds over the lane's pit-surface triangles
  (`IS_PITLANE` / a `PIT` key), else over the stretch clear of the track by
  3 m; the lines are held at least 1 m past the road's edge, because the
  server takes a human's car over at the first line only once it is past the
  edge.
- **Boxes** are the `AC_PIT_n` positions (the car's stop, on the working lane
  5-11 m beside AC's through line), in order along the lane, facing down it.
  The lane's width takes in the farthest (`width_m` = max(12 m, 2 x the
  reach)): the server serves a car only within half the width of the lane's
  line. Markers further than 20 m from the line are dropped with a warning.
- No pit line, no markers, a line that never leaves the track or never has a
  box by it: a warning and no sidecar. The report's `pit_lane` carries the
  lane length, limit lines, boxes and width.

### Checks

In the report and printed (`validate.py`); a failed one is exit code 1:
every grid slot on the road mesh, road coverage every metre edge to edge,
the centerline on the road, wall openings (reported, never failed), and the
triangle / draw-call / texture budget. Refusals (no exit, skipped):
no `models.ini`, no readable kn5, no `fast_lane.ai`, an AI line that does
not close (point-to-point), fewer than 50 AI points, no physics surfaces.

### On the client (export format version 3)

The format is in [track-format](track-format.md). The Rust exporter still
writes version 2 unless a scene uses the new fields; the client reader
(`FApexTrackSceneReader`, `kSupportedVersion = 3`) takes 1-3. It parses the
DDS files on the worker thread with the scene (`ApexDdsReader`: the
compressed blocks go straight into transient textures; a bad one is logged
and its material draws flat). `FApexTrackSceneBuilder` draws `scenery`
materials on `/Game/Materials/Car`'s parents (`FApexTrackParents::
SceneryOpaque` / `SceneryMasked` / `SceneryTranslucent`; flat colours on the base
when the car parents are not baked), honours `ground_set`
(`ApexGround::LookForSet`), sets `SetCullDistance` from the draw distance
and skips the collision component where a mesh has `collision: false`.
Kerbs keep AC's textures on purpose: their stripes are authored into the
texture and the kit's would need an along-kerb UV the kn5 lacks.

## Where the circuit is (`site.py`)

AC stores no height, and most tracks (Kunos' own too) keep `geotags` at the
placeholder `["lat", "lon"]`; `data/lighting.ini`'s `SUN_HEADING_ANGLE` is
where AC's fixed sun path stands, not a compass (Silverstone's 45 and
Monza's -150 are both on north-up models). The importer takes the place,
in order, from:

1. `KNOWN_SITES`, AC's own circuits (and a few mods of shipped ones) by
   folder name: the shipped circuit's `altitude_m` / `latitude_deg` /
   `longitude_deg`, or for a circuit ApexSim does not ship (Laguna Seca,
   Vallelunga, Mugello, Imola, Magione) its position and the Copernicus
   height at its start line.
2. `geotags` with real coordinates: the latitude and longitude, and the
   Copernicus height there through `tools/dem_fetch.py` (the tile is
   downloaded into `.cache/dem`, 30-80 MB, unless `--dem cache` (cached
   tiles only) or `--dem off`; a failed lookup warns and leaves the height
   out).
3. Nothing: no keys, so the server's sea-level air and 50 N.

The server thins the air by `altitude_m` and puts the sun at the latitude
([conditions](../../apexsim/docs/server/conditions.md)). North goes to the
client as the manifest's `north_yaw_deg` (true north in the track frame, as
`ats-export` writes it): AC's own convention, -z towards north, yawed by
the start-line frame, unless the table holds a measured figure. Measured by
fitting `ai/fast_lane.ai` onto the shipped centerline (whose DEM
georeference knows north; rms 4-12 m), Silverstone, Monza, Spa, the Red
Bull Ring, Barcelona, Brands Hatch, the Nordschleife and the Le Mans mod
are within 3 degrees of the convention; Zandvoort (292), the Nurburgring GP
(135) and the Interlagos, Suzuka and Bahrain mods are not and carry their
own. A mod outside the table gets the convention, which may be wrong for
it. The report's `site` names each figure's source.

## The `imported` marker

The importer's `.ats` carries `"imported": "ac"` (`AtsScene::imported`,
read by `ats_io::imported_marker`; Python `track_dirs.imported_marker`;
PowerShell `Test-ApexImportedTrack` / `Get-ApexImportedTrackRebuild` in
`tools/lib/ApexTracks.ps1`). Everything that would rewrite a track's
files leaves such a track alone:

- `ats-export` refuses it (`UeExportError::Imported`; `--all` walks past).
- `ats-dress`, `ats-groom`, `ats-smooth`, `ats-bank` skip it.
- `bake.ps1 -Track` (and `build_track_levels.ps1 -Track`, which runs it)
  drops it and prints the rebuild command; the showcase stage does not
  render it.
- `initialize_content.ps1` never bakes it, and reports one whose export or
  road mesh is missing with the command from its `import.json`.
- `build_track_catalog.py` keeps the importer's preview.
- `build_game_standalone.ps1` / `build_release.ps1 -IncludeCustomTracks`
  copy `<Stem>.textures/` beside the export.

A car's marker is `imported = "ac"` at the top of its car.toml;
`liveries.py` only walks `default/`, so it never repaints an import, and
there is nothing to bake for a car.

## Cars: `ac_car_import.py`

```powershell
python tools/ac_car_import.py "E:\SteamLibrary\steamapps\common\assettocorsa\content\cars\ks_porsche_911_gt3_r_2016"
python tools/ac_car_import.py <folder> --list      # skins, compounds, parts, LODs
python tools/ac_car_import.py --all <ac>\content\cars [--dry-run]
#   --stem, --display-name, --class, --skin NAME, --compound N, --lod A|B,
#   --max-texture N (1024), --max-skin-texture N (2048), --cylinders N,
#   --keep-steering-wheel, --force, --dry-run, --root <ApexSim install>
# then restart the server, and restart the game or run apexsim.car.Rescan
```

### Outputs

In `cars/custom/<Stem>/` (stem: CamelCase of the folder; a folder
name or id under `default/` is refused, an existing import replaced only
with `--force`; with `--root` the files go to `Game/Cars/custom` and the
car.toml also to `Server/content/cars/custom`):

- `car.toml`: `imported = "ac"`, `id` a UUID v5 of `ac-car:<folder>` (two
  players importing the same car get the same id, which is what lets the
  content CRC check compare them), a `[source]` table, and every figure
  commented with the AC key it came from. Laid out for the client's
  line-based TOML parser (no comment after a header, arrays on one line).
  For a class with rules (GT3, LMP2, Hypercar, F1) it also gets the
  `[[damage_part]]` tables `tools/generators/cars/damage_parts.py` finds in
  the written body (the nose or front wing, the rear wing), as the shipped
  cars have, so an import sheds parts in a crash; the parts are listed in
  the report (`damage_parts`). The finder walks the GLB's node transforms.
- `<Stem>.glb`: the body, seated with its tyres on y = 0 and the origin
  midway between the axles; textures embedded (JPEG opaque, PNG when
  sampled by a blended or masked material).
- `wheels/front.glb`, `wheels/rear.glb`: car-local wheels in the class
  wheel's frame (hub at the origin, axle on glTF X), slots `wheel_tyre`,
  `wheel_rim`, `wheel_brake`. A `[wheels] model` ending in `.glb` or
  holding a `/` is looked up in the car folder (`ApexCarToml::IsCarLocalWheel`).
- `steering_wheel.glb`: `STEER_HR` in its own frame for the cockpit rig
  (`[cockpit] steering_wheel_model`).
- `skins/<skin>/...`, `skins/_kn5/`: every AC skin but the baked one as a
  texture `[[livery]]` (`skin`, `textures` slot overrides, `preview`). A
  livery lists every texture any skin overrides, taking the kn5's own
  (`_kn5/`) where this skin has none, so stepping liveries never leaves the
  previous one's banner behind.
- `<Stem>.import.json`: source files with CRCs, the physics as written,
  the fit, parts dropped, materials and slots, lamps, skins, checks,
  warnings, rebuild command.

Re-running writes byte-identical files. Re-run with `--force` after an
importer change.

### How a car is read

- **`data.acd`** (`acd.py`): records whose payload is every fourth byte,
  minus a key made of eight hashes of the lower-cased folder name. A wrong
  key (a renamed folder) is refused: the record names read in clear but
  `car.ini` comes out as noise.
- **The kn5** goes through the track importer's reader; meshes and dummies
  carry `path` (their ancestors' names), which is how parts are selected:
  `WHEEL_xx` subtrees, `STEER_HR`, `COCKPIT_LR`, `RIM_BLUR_*`,
  `DAMAGE_GLASS_*` and so on.
- **Model frame.** AC stores a car as (x left, y up, z forward), which is
  the client's glTF frame with the same winding: positions are copied.
- **Materials.** Paint materials sampling the skin become `car_skin*`
  slots with clear coat (not `car_paint`: the paint is a texture, and a
  colour livery would multiply into it); `lights.ini` nodes become
  `car_brakelight` / `car_taillight` / `car_headlight` / `car_rainlight`;
  the `mirrors.ini` glass is cut into pieces and the largest centre / left
  / right become `car_mirror_centre` / `_left` / `_right` with flat UVs,
  which `AApexCockpitRig::BindCarGlass` paints with its mirror captures.
- **Cockpit.** `style` open/closed from the tags or a ray cast up from the
  eye; `eye_cm` from `DRIVEREYES`; `wheel_cm`, `wheel_rake_deg`,
  `wheel_lock_deg` from `STEER_HR` and the lock; `mirror_*_cm` and
  `_size_cm` from the glass; `rig_dash = false` when the car has its own
  display, `rig_wheel = false` with `--keep-steering-wheel`.
- **Physics** (`physics.py`), the main mappings:
  - mass `TOTALMASS` (both exclude fuel); `max_steering_angle_rad` =
    `STEER_LOCK / STEER_RATIO`; geometry from `suspensions.ini`;
  - springs, dampers and bars 1:1, plus half an axle's heave spring at each
    corner; `STATIC_CAMBER`, the camber gain from the wishbone or strut
    points (`camber_gain`), `TOE_OUT` over the steering arm,
    `BUMP_STOP_RATE`;
  - the torque curve is `power.lut x (1 + boost)` (the LUT is the
    naturally aspirated torque); `ctrl_turbo<n>.ini` controllers evaluated
    at full throttle as the wastegate per gear, the curve taking the gear
    with the most boost; `[engine.turbo]` `boosted_share` and lags;
    redline = limiter - 200, max = limiter + 100;
  - one tyre compound (`COMPOUND_DEFAULT` or `--compound`): `grip_coefficient`
    from `DY0` with the speed sensitivity at 40 m/s, the per-axle balance in
    `front_grip_scale` / `rear_grip_scale`, `load_sensitivity_*` from
    `LS_EXPY`, `longitudinal_grip_factor` from `DX0/DY0`,
    `optimal_pressure_kpa` from `PRESSURE_IDEAL`; the temperature window
    from `[THERMAL_FRONT]`'s `PERFORMANCE_CURVE` (`thermal_window`);
    `blanket_temperature_c = 70` for a modern F1 (a rule, not an AC figure);
  - the other compounds of `tyres.ini` as the `[[tires.compound]]` list
    (`map_compounds`), softest slick first, the chosen compound the
    `reference` at grip 1, wear 1, shift 0: `grip` the compound's mean `DY0`
    (at 40 m/s) over the reference's, `wear` the reference's life over its
    own (the km its `WEAR_CURVE` takes to lose 5% of its best grip),
    `window_shift_c` its thermal window's middle less the reference's, `kind`
    from the name (`wet`, `rain`, `inter`; AC ships no rain tyre). A car whose
    compounds are all slicks gets ApexSim's `intermediate` and `wet` appended
    (named as such in the car.toml) so the garage keeps its rain tyres; a car
    with one compound has no list and keeps the server's default five. The
    grip level, the wear, the window's place and its width (`window_scale`,
    the compound's plateau over the reference's, written only when it is
    not 1: the 919's soft is 2) are per compound; its pressure and load
    sensitivity are not;
  - `[suspension] roll_centre_front_m` / `_rear_m` from the front view of the
    linkage (`roll_centre_m`): the upright's instant centre (the lower arm's
    line met by the upper arm's, or for a strut by the line at right angles
    to its axis through the top mount) joined to the contact patch, read at
    the centreline. Both axles or neither (the server reads a lone one as
    ground level on the other); held to 0-0.3 m with a warning. `camber_thrust`
    is `tyres.ini`'s `CAMBER_GAIN`, axles averaged (the same ratio of side
    force from lean to cornering stiffness, 0.1-0.2 on AC's tyres);
  - `[aero] yaw_sensitivity`: each wing's `YAW_CL_GAIN` (a share of its lift
    lost per radian of yaw) weighted by the downforce the wing makes, per
    degree. `[fuel] tank_front_share` from `car.ini` `[FUELTANK] POSITION`'s z
    by the lever rule between the axles;
  - `[brakes] material = "carbon"` when `brakes.ini`'s pad curves
    (`[TEMPS_FRONT]` / `[TEMPS_REAR]` `PERF_CURVE`, 35 of AC's 194 cars carry
    one) resemble the server's carbon friction table more than its steel one
    (`pad_material`; the tables are the server's, a test keeps them equal) and
    the class does not already race on carbon (`CARBON_CLASSES`, equal to
    `BrakeMaterial::for_class`). Nothing is written otherwise: a steel curve
    never takes carbon off a class (a prototype's curve is often a steel
    one), and a car with no curve takes its class's material;
  - aero: each wing at its `ANGLE`, and where it has `LUT_GH_CL` tables at
    the height it rides at 50 m/s (`aero_posture`, the server's aero
    reference), giving the lift coefficients and the `[aero]` ride-height
    map (`ride_height_sensitivity`, `rake_sensitivity`, `stall_height_m`);
    DRS reductions from the DRS wing's tables, **0 when there is no DRS
    wing** whatever the class;
  - brakes `2 x MAX_TORQUE x (share / r_front + (1 - share) / r_rear)`;
  - `[differential] simulated = true` with `POWER` / `COAST` / `PRELOAD`;
    `awd_front_share` from AC's AWD split;
  - hybrid (`map_ers`): the motor is `[KINETIC]`'s (torque by engine rpm,
    at the crank) or `[FRONT_MOTORS]`' (torque at the axle by wheel rpm:
    `axle = "front"`, the torque scaled by driven / front tyre radius so
    the force and power come out as AC's, recovering at its own peak
    power), whichever is the stronger; with both (the 918, the TS040) the
    other is dropped with a warning. Curves may be files or inline
    (`(0=150|...|)`, as the 499P mod writes them); the kept motor's curve
    is written as `[[hybrid.torque_curve]]` (a front motor's torque
    scaled as its peak is). The battery is the
    motor's peak power x `DISCHARGE_TIME`, `MAX_KJ_PER_LAP` the
    `deploy_kj_per_lap` budget, `[HEAT] TORQUE_PERC` x the motor power
    `heat_recovery_kw`. Of the delivery profiles, the default one
    (`DEFAULT_CONTROLLER`; `ctrl_ers_front_<n>.ini` for a front motor)
    gives `deploy_min_speed_kph`: the last speed its `SPEED_KMH` map is
    shut at before it opens. Its throttle map, speed cap, oversteer / slip
    cuts, gear and rpm masks, the other profiles and
    `FRONT_TORQUE_VECTORING_BIAS` are warned about, not carried: the car
    gets the sim's modes and pacing.
- **Class** from `ui_car.json` tags (`map_class`): `GT3`, `LMP2`,
  `Hypercar`, `F1` (a single-seater with a DRS wing and a `gp`/`f1` tag),
  else a group of its own (`Formula`, `GT4`, `GT`, `Vintage`, `Drift`,
  `Touring`, `Street`, `Race`) that races only the other imports in it.
  `--class` overrides.
- **Sound**: `[sound]` cylinders and layout guessed from the name and
  description (`--cylinders` overrides), `turbo` from `[TURBO_n]`.

### Checks

Fatal (exit code 1): the body is long along glTF Z, it sits above the
ground, the four wheels were found, every server-required `[physics]` key
is set and positive, the torque curve is sorted. Warnings: triangles over
400 k, decoded textures over 300 MB, the curve's power against
`ui_car.json`'s figure, an unnamed baked skin.

## Importing from the launcher

Launcher > Manage content > Import from Assetto Corsa runs the importers
under the player's own Python; nothing is frozen into an exe.

- `FindPython` tries `py -3`, `python`, `python3` and wants 3.11 or newer
  (a Store stub that prints nothing is skipped); with none it says what to
  install and offers python.org.
- The packages live in a venv at `%LOCALAPPDATA%\ApexSim\importer-venv`,
  made on first use and reinstalled when `importer-requirements.txt`
  differs from the copy stamped in the venv
  (`importer-requirements.installed.txt`). A package installs them from
  its own wheels first (`Tools/importer/wheels`, win_amd64 for Python
  3.11, 3.12 and 3.13, `pip install --no-index --find-links`), so a PC
  with no internet can import; only a Python the wheels do not cover (or
  a checkout, which has none) needs PyPI.
- The player picks a folder; `DetectKind` decides car or track (a folder
  named `cars` / `tracks` means `--all`; `data.acd` or `ui/ui_car.json` a
  car; `ui/ui_track.json` or `ai/` a track; else it asks). One console runs
  setup then the import, with `pause` after so the report can be read. In a
  package the launcher passes `--root <install>`.
- A package carries `Tools/importer/` (`ac_import.py`, `ac_car_import.py`,
  the two packages without `tests`, `track_dirs.py`,
  `generate_track_previews.py`, `importer-requirements.txt`,
  `cars/damage_parts.py` for the car importer's damage parts, and
  `wheels/`, which `pip download` fills while the release is built; a build
  with no network ships none and warns), copied by `build_release.ps1`. A
  checkout runs them from `tools/`.
- `--root` sends the client's files to `Game/Tracks` / `Game/Cars/custom`
  and the server's to `Server/content/{tracks,cars}/custom`, and checks
  stems and ids against `Server/content/*/default`. A taken name is asked
  for again at the console.

## Checking it

```powershell
python -m unittest discover -s tools/ac_import/tests -p "test_ac_import.py"
python -m unittest discover -s tools/ac_car_import/tests
```

Simulation and server (opt-in, `#[ignore]`d, because imports are the
player's own data), the first block in `../apexsim/server/sim`, the pit surveys in
`../apexsim/server/server`:

```bash
cargo test --release --test all imported_track_test:: -- --ignored        # every import loads with its sidecars
cargo test --release --test all imported_car_test:: -- --ignored --nocapture
cargo test --release --test all car_stability_test::imported -- --ignored
PROBE_CLASS=GT3 cargo test --release --test all grip_probe_test::silverstone_profile_lap_times -- --ignored --nocapture
SURVEY_TRACKS=KsZandvoort cargo test --release --test all ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored --nocapture
# an imported car's AI: a field of it, and one alone against its own plan
SURVEY_CARS=KsFerrariSf70h,fugazzi-sf26 SURVEY_TRACKS=Monza,Suzuka cargo test --release --test all ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored --nocapture
SURVEY_CARS=KsFerrariSf70h SURVEY_TRACKS=Suzuka cargo test --release --test all ai_race_start_test::lone_car_against_its_plan -- --ignored --nocapture
```

```bash
# every import's pit lane: a driven trip (autopilot, box, service, out) and an AI's stop
PIT_CUSTOM=1 cargo test --release --test all pit_stop_test::pit_lane_survey -- --ignored --nocapture
PIT_CUSTOM=1 cargo test --release --test all pit_stop_test::ai_pit_survey -- --ignored --nocapture
```

Debug-build test servers skip imported tracks via `[content]
skip_imported_tracks`. Rust: `a_version_3_manifest_carries_the_imported_extensions`
and `an_imported_track_is_not_baked_over` (`track-editor/core/tests/ue_export.rs`).
Client automation: `ApexSim.Track.Dds.Parse`, `ApexSim.Track.Reader.Version3`,
`ApexSim.Cars.Glb.RepoCars`, `ApexSim.Cars.TomlRepoCars`.

## Traps

- **Frame.** The track conversion is `(X, Y, Z) = (x, -z, y)`, a rotation,
  not a mirror; checked on every Kunos circuit against `ui_track.json`'s
  direction. Do not flip the winding. The car needs no conversion at all.
- **`.ai` `extra_count`.** An `i32` sits between the points and the
  18-float records; reading without it shifts every record by one float.
- **`DRIVEREYES` is in the model frame**, not the physics frame (read in the
  physics frame the eye ends up in the roof); the aero `POSITION`s are from
  the CG; the model sits at `physics - GRAPHICS_OFFSET`.
- **`wheel_rake_deg` is negative for a real car**: the rig's positive pitch
  lifts the column's forward end, and a real column runs forward and down.
- **`wheel_lock_deg` is `STEER_LOCK` itself** (or `driver3d.ini`'s
  `[STEER_ANIMATION] LOCK`): centre to lock, not half of it.
- **Turbo `GAMMA` is pedal sensitivity**, 1 at full throttle, not an rpm
  exponent. Skip the boost and a turbo car is 30-80% down on power.
- **An import is not shaped like a shipped car**, and the AI reads it as
  it is rather than the importer bending it: its tyres are load-sensitive
  (`LS_EXPY`), which the racing line plans with the lateral transfer and
  the weaker axle (`CarConfig::envelope_mu`), and its brakes are AC's with
  no ABS on an F1 or a prototype, which the AI brakes short of locking
  (`unlocked_pedal`; [apexsim/docs/server/ai.md](../../apexsim/docs/server/ai.md) traps). Read
  either the shipped cars' way and the AI slid off and locked its fronts
  into every slow corner.
- **Classify materials at triangle centres, never vertices**: a road
  ribbon's vertices all sit on its edges, where the physics is road or
  grass by chance.
- **Names before shaders** for unclaimed terrain: Spa's grass valley uses
  the tarmac shader.
- **`.ats` `external_sidecars`** is what keeps a later export from
  overwriting the importer's sidecars; the `imported` marker is what keeps
  every other tool off the track. A new tool that rewrites track files
  must honour the marker.
- **A key on a section's header line** (`[REAR]NAME=...`) occurs in mods;
  the shared `ac_import/ini.py` reads it.

## Not supported

- Tracks without `fast_lane.ai`; point-to-point stages (refused).
- Pit stops where a layout has no `pit_lane.ai` or `AC_PIT` markers; the
  pit exit light and speed-limit boards (AC's scenery draws its own).
- The height and north of a mod outside `KNOWN_SITES`: the height only
  with real `geotags` (and the DEM), north by AC's convention.
- Kit props in place of AC scenery; night lighting from AC's lights; AC's
  normal, `txMaps` and detail maps (diffuse only).
- A DRS flap split off a car's body: a DRS car gets the drag and downforce
  reductions but its wing is drawn closed.
- AC carries no figure for brake pads, `brake_by_wire`, body-roll aero or
  porpoising: an import gets the defaults (standard pads, brake-by-wire,
  none).
- AC's older `kers.ini`, a second ERS motor (one motor per car), and the
  ERS delivery profiles beyond their minimum speed; aero controllers are
  read as their authored figures, with a warning.
- `--lod B` exists but is untested.
