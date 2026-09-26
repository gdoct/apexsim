# Modding ApexSim

We encourage modding this game. You can create custom content, modify existing assets, and extend the game's functionality through scripts and configuration files.

## Getting Started

Content lives in two places, and a mod has to reach both:

- **The server** simulates from the source files themselves: a car is its
  `car.toml`, a track is its YAML centerline plus the sidecars baked beside it
  (`.ground`, `.curbs`, `.walls` `.msgpack`). The server scans
  `content/cars/` and `content/tracks/` recursively on startup, so a new file
  there is picked up on the next start.
- **The client** draws what was imported into the Unreal project: a car's
  mesh and a track's level are Unreal assets, found through the
  `DT_CarCatalog` / `DT_TrackCatalog` tables by the car's `id` or the track's
  `track_id`. A packaged game cannot import anything, so a new car or track
  means importing it in the editor and packaging the client again.

Both sides checksum the files (CRC-32 of the `car.toml` / track YAML), so if
the server's copy and the one the client was built from disagree, the game
logs it and shows a toast. That is usually the first sign an update only
reached one side.

What you need:

- a checkout of this repository
- Rust (stable), for the server and the track tools in `track-editor/`
- Python 3.11 with `numpy`, `pyyaml` and `Pillow`; the car generators also
  need Blender, or the `bpy` module (`pip install bpy`) to run headless
- Unreal Engine 5.8 and the `ApexSimEditor` target built (see `CLAUDE.md`,
  "Unreal client"). Every commandlet below needs the editor **closed**.

The commands below assume PowerShell at the repo root, with the engine at
`$UE`:

```powershell
$UE = "C:\Program Files\Epic Games\UE_5.8"
$Cmd = "$UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
```

## Cars

A car is a folder under `content/cars/<folder>/` holding a `car.toml` (the
physics, engine, gearbox, sound, wheel placement and liveries) and the GLB
body its `model` key names. Folder names are lower-case with hyphens
(`bugotti-chiffon-hypercar`); the GLB stem uses underscores
(`bugotti_chiffon.glb`). `docs/CAR_MODELS.md` is the full reference.

### Creating a new car

There are two ways to get a body: add a variant to one of the class
generators (how the three hypercars were made), or bring your own model.

**1. The body, from a generator.** Each class has a Blender script in
`content/cars/`: `build_gt3.py`, `build_lmp2.py`, `build_hypercar.py` and
`build_f1.py`. A script is a table of `VARIANTS` on top of a shared hull and
the part library in `carlib.py`. To add a car to a class, copy an existing
entry in `VARIANTS` and change it:

```python
"mycar": dict(folder="mybrand-x1-hypercar", stem="mybrand_x1", logo="mybrand_logo.png",
              paint=(0.60, 0.015, 0.025), accent=(0.96, 0.78, 0.04), caliper=(0.96, 0.80, 0.05),
              paint_metallic=0.60, number="7",
              lights="slit", drl="blade", tail="double", exhaust="twin", grille="twin",
              nose_w=0.88, fender=1.05, roof=0.96, ...,
              face="boomerang", side="slash",
              wing_plan=("spoon", 0.055), endplate="horn", wing_led="endplate"),
```

Colours are linear RGB. The shape keys (`nose_w`, `fender`, `roof`,
`nose_z`, `valley`, `canopy_w`, ...) scale the shared hull, and the style
keys (`lights`, `face`, `side`, `wing_plan`, `endplate`, ...) pick from the
parts each script knows; the comments above the existing variants say what
each choice looks like. Give the car its own face, flank and wing rather than
only a new colour. "Character: one class, different cars" in
`docs/CAR_MODELS.md` explains why.

Create the folder first, then run the script in Blender's Python console:

```python
VARIANT = "mycar"
exec(open(r"D:\apexsim\content\cars\build_hypercar.py").read())
```

or headless, with `APEXSIM_ROOT` set to your checkout and the same two lines
run under Python with `bpy` installed. The script saves a `.blend` after each
stage and writes `<stem>.glb` into the folder. Set `APEX_EXPORT=0` to skip the
export while you iterate on the shape.

**1b. The body, from your own model.** Any GLB works if it follows the
conventions the generators follow:

- metres, ground at z = 0, **nose on -Y**, tail on +Y, driver on +X (left-hand
  drive). Exported with glTF +Y up.
- **No wheels.** The client draws the class's shared wheel four times, where
  `[wheels]` says. If your model has wheels, `content/cars/strip_wheels.py`
  measures them (`measure(glb)`) and removes them (`strip(glb)`).
- the **material slot names** the client drives: `car_paint`, `car_accent`,
  `car_logo`, `car_glass`, `car_headlight`, `car_taillight`,
  `car_brakelight`, `car_rainlight` and the others in "Material slots the
  client drives". A slot with another name renders as authored but won't
  take liveries, brake lights or headlights.

**2. The `car.toml`.** Start from a car of the same class. Copy
`content/cars/bugotti-chiffon-hypercar/car.toml`, then:

- give it a **new `id`** (a fresh UUID, e.g. `python -c "import uuid; print(uuid.uuid4())"`).
  The id is how the client finds the car's mesh. Never reuse one.
- set `name`, `brand`, `model_type`, `model_year`, `manufacturer_country`
  and `model` (the GLB file name).
- `class` decides who it races: the AI field is every car of the host's
  class. Use an existing class (`GT3`, `LMP2`, `Hypercar`, `F1`) to race the
  others, or a new one to race only itself.
- `[physics]`, `[engine]` (with its torque curve), `[transmission]`,
  `[drivetrain]`, `[differential]`, `[fuel]` and optionally `[hybrid]` are what
  the server simulates. Values are range-checked at load, and a car that fails
  validation is logged and skipped.
- `[wheels]` places the wheels on the body (visual only). It must agree with
  the arches: the generators build each class to the stance table in
  `docs/CAR_MODELS.md`.
- `[sound]` describes the engine to the client's synthesiser (cylinders,
  crossplane, turbo, exhaust length, muffling, ...). The server ignores it.
- F1 cars only: `build_f1.py` also writes the DRS flap GLB and a `[drs_flap]`
  table.

**3. Tune it.** The class grip levels are set so each car's ideal lap
lands a few seconds off real poles. The grip probe prints every car's ideal
Silverstone lap from its racing-line profile:

```powershell
cd server
cargo test --release --test grip_probe_test -- --ignored --nocapture
```

The hypercars sit at 1:42.2-1:42.8, about two seconds under the LMP2s. If
your car is far off its class, adjust `grip_coefficient`, the lift
coefficients or power, not the class.

**4. Preview it** (optional). Run `content/cars/preview_cars.py` in Blender
with `CARS = ["mybrand-x1-hypercar"]` to render hero, side, front, rear and
cockpit shots into `content/props/_preview/cars/`, with the wheels placed
where `car.toml` puts them.

### Adding liveries

Livery 0 is the model as built. The rest are `[[livery]]` tables at the end
of `car.toml`:

```toml
[[livery]]
name = "Greenline Forest"
paint = [0.008, 0.090, 0.035]         # linear RGB
accent = [0.780, 0.680, 0.420]        # optional
metallic = 0.60                       # optional
logo = "textures/livery_forest.png"   # optional: replaces the car_logo wordmark
```

A livery is a repaint of the same mesh: `paint` and `accent` recolour the
`car_paint` / `car_accent` slots and `logo` replaces the `car_logo` texture.
Which panels count as accent is fixed by the body.

`content/cars/liveries.py` owns everything below the marker line
`# --- liveries: written by content/cars/liveries.py ...` and draws the logo
PNGs. To add or change liveries, edit its schemes and rerun it for your car:

```powershell
python content/cars/liveries.py mybrand-x1-hypercar
```

Anything written below the marker by hand is lost on the next run. Append
new liveries to the end: a player's saved pick is an index, so reordering
changes what they drive. After changing liveries, re-import the car (below).

### Installing your car

1. **Server:** nothing to register. `car.toml` under `content/cars/` is found
   on the next server start. It shows up in the lobby's car list and, if its
   class matches, in AI fields.
2. **Client:** import the mesh and create its catalog row:

   ```powershell
   & $Cmd game-unreal/ApexSim.uproject -run=ApexCarImport -car=mybrand-x1-hypercar
   ```

   This imports the GLB to `/Game/Cars/<folder>/SM_<folder>` and adds a
   `DT_CarCatalog` row keyed by the `id`, with name, class, specs, wheels,
   engine sound, liveries (logos included) and the content checksum. If the
   class's wheel isn't imported yet, it is imported here too. The log warns
   if the body isn't long along Y.
3. **Play it:** `./scripts/play_editor.ps1` runs the editor build with a
   local server. To ship it, package the client again
   (`./scripts/build_game_standalone.ps1`, or `./scripts/build_release.ps1`,
   which also copies every `car.toml` into `Server/`).

A car with no catalog row still drives. It shows placeholder art and an empty
turntable.

### Updating your car

- **Physics, engine, gearbox:** edit `car.toml` and restart the server. Then
  re-run `ApexCarImport -car=<folder>` so the client's checksum, specs and
  sound match. Without it, the content-mismatch toast appears.
- **Wheels, sound, liveries, DRS flap:** re-run the import. These fields are
  derived and refreshed on every run.
- **The body:** rebuild the GLB, then import with `-force`. Without `-force`
  an existing mesh is kept. `-force` also resets the row's hand-tuned
  fields (preview framing, cockpit points), so re-check the garage turntable
  and the cockpit view.
- Keep the `id`. A new id is a new car to the client, and lap records are
  kept per car.

`ApexCarImport -list` shows what is imported, and `-remove=<folder>`
deletes a car's assets and row.

## Tracks

A track is a set of files under `content/tracks/real/` sharing a stem (for
example `Nordschleife`):

| file | what it is | made by |
| --- | --- | --- |
| `<Stem>.yaml` | centerline nodes, widths, banking, raceline, DRS zones, metadata; the server simulates this | you, via the scripts below |
| `<Stem>.layout.json` | the real furniture: corners, pit lane, stands, buildings, woods, barriers, surroundings | `scripts/osm_layout.py` from OpenStreetMap |
| `<Stem>.dem.msgpack` | real terrain elevation | `scripts/dem_fetch.py` from Copernicus GLO-30 |
| `<Stem>.ats` | the scene: curbs, grass, run-off, props, pit lane, decals | `seed_scene.py`, then `ats-dress` / groom |
| `<Stem>.{ground,curbs,walls}.msgpack` | ground, track limits and barriers as the server needs them | `ats-export` (not checked in) |

The Nordschleife is the worked example; `docs/NORDSCHLEIFE.md` records every
step and number. Most circuits started from a GPS trace. The Nordschleife
has none, so its centerline was routed over OpenStreetMap's
`highway=raceway` ways. That is the route below, and it works for any
circuit OSM maps as a raceway.

### Creating a new track

Pick a stem (CamelCase, no spaces, e.g. `Mugello`) and a fresh UUID for
its `track_id`.

**1. Tell the scripts where it is.** In `scripts/osm_layout.py` add a
`BBOXES["Mugello"]` entry (one or more `(min_lon, min_lat, max_lon, max_lat)`
boxes around the circuit and ~1 km of surroundings). In
`scripts/osm_centerline.py` add a `LAPS["Mugello"]` entry:

```python
"Mugello": {
    "name": "Autodromo del Mugello",
    "track_id": "<your uuid>",
    "start_finish": (11.3718, 43.9975),       # lon, lat of the line
    "start_offset_m": 0.0,                    # slide the line along the lap if the grid won't fit
    "banking": [],                            # (from_m, to_m, radians); negative = left-hander
    "waypoints": [ (lon, lat), ... ],         # in race direction, enough to pin the layout
    ...                                       # widths: see the Nordschleife entry
},
```

The waypoints only need to sit near the right stretch of tarmac. The lap
between them is routed over the raceway graph. Add more where two layouts
share tarmac, as at the Nordschleife's junctions with the GP circuit.

**2. Fetch the OSM extract.** `python scripts/osm_layout.py Mugello` downloads
the extract into `content/tracks/osm-cache/` (gitignored). No centerline
exists yet, so there is nothing to fit a dossier to, and it stops after
the download. If the OSM API refuses the box (it caps at 50k nodes), cut the
extract from a planet file instead, as was done for the Nordschleife.

**3. Route the centerline.**

```powershell
python scripts/osm_centerline.py Mugello --dry-run   # check the length and the route first
python scripts/osm_centerline.py Mugello             # -> content/tracks/real/Mugello.yaml
```

This writes 5 m nodes in the game's frame (origin on the start line, +X
along the course, +Y left), widths from OSM `width` tags where present, and a
minimum-curvature raceline. Elevation is left flat. Use `--plot out.png` to
see the route.

**4. The dossier, elevation, smoothing, banking and DRS.** Run these in this
order. Each step depends on the one before, and out-of-order runs produce
data that disagrees with itself:

```powershell
python scripts/osm_layout.py Mugello --offline        # dossier, fitted to the centerline
python scripts/dem_fetch.py Mugello                   # terrain (same fit); a few hundred MB of tiles
python scripts/dem_elevation.py Mugello               # the centerline's real z from the terrain
cargo run --manifest-path track-editor/Cargo.toml --bin ats-smooth -- content/tracks/real/Mugello.yaml
cargo run --manifest-path track-editor/Cargo.toml --release --bin ats-bank -- content/tracks/real/Mugello.yaml
python scripts/drs_zones.py Mugello                   # optional; needs a ZONES entry
python scripts/osm_layout.py Mugello --offline        # refit the dossier to the smoothed line
python scripts/dem_fetch.py Mugello --offline
```

Check the fit printed by `osm_layout.py` (rmse around a metre, coverage near
100%). It refuses to write a dossier the fit doesn't explain. On a wooded
circuit the terrain model reads the tree canopy, so compare the elevation
profile to the known one before trusting it (`dem_elevation.py --report`).

**5. Real details OSM lacks.** Named grandstands from the seating map,
bridges, landmarks, a hand-drawn pit lane or road graffiti go into the
`MANUAL_STANDS`, `MANUAL_CROSSINGS`, `MANUAL_LANDMARKS`, `MANUAL_PIT_LANE`,
`MANUAL_WOODS` and `MANUAL_GRAFFITI` tables in `osm_layout.py`, keyed by
stem. They survive every refetch. Re-run the dossier after editing them.

**6. Seed the scene.** `python scripts/seed_scene.py Mugello` writes the first
`Mugello.ats`: the start line, a 130 m grass band either side, and curbs on
the inside of every apex and the outside of every exit.

**7. A style of its own (optional).** Barrier kinds and distances, tree
density, signs, hoardings, floodlights and whether there is a pit lane come
from `CircuitStyle` in `track-editor/core/src/circuit_style.rs`. Every track
gets `DEFAULT` unless `for_stem` maps it to another. The Nordschleife's
German guard rail 3 m off the road, dense forest, German signs and no pit
lane are one entry there.

**8. Dress and export:**

```powershell
./scripts/build_track_levels.ps1 -Track Mugello
```

This runs `ats-dress` (stands, buildings, pit lane, barriers, trees,
boards, decals from the dossier), `ats-export` (`content/tracks/export/Mugello.uescene.json`
and `Mugello.uemesh`, which the game builds the circuit from, plus the
server's `.ground` / `.curbs` / `.walls` sidecars), `build_track_catalog.py`
(the track picker's preview) and `ApexMaterialBake` (the shared track
materials, only if they are missing; `-SkipMaterials` to leave the engine
out of it). There is no level to import: the game builds the circuit from
its export when it is raced. Add `-ImportProps` if you added new prop
meshes. For road graffiti, also run
`& $Cmd game-unreal/ApexSim.uproject -run=ApexPropImport -kind=decal`.
To look at the result in the editor, `-ImportLevels` also imports it as a
level under `/Game/Tracks` (editor only: the game never loads it).

**9. Check it.**

```powershell
python scripts/check_walls.py --openings Mugello   # holes in the barriers a car could leave through
cd server
$env:SURVEY_TRACKS = "Mugello"
cargo test --release --test ai_race_start_test survey_ai_races_on_every_circuit -- --ignored --nocapture
```

The AI survey reports contact and off-road time per class. A car pinned
against a wall for most of the run usually means a barrier sits too close
somewhere. You can also open the `.ats` in the track editor
(`cargo run` in `track-editor/`) to move props, curbs and run-off by hand.
Pass `-SkipDress` to the level build afterwards, or dressing re-lays what it
owns.

**Without OSM.** If you have a GPS trace or your own design instead, write
the YAML directly: `name`, a fixed `track_id`, `nodes` (x, y, z, `width_left`,
`width_right`, `banking`, `surface_type`), `default_width`,
`closed_loop: true`, `raceline` and `metadata` (see
`docs/TRACK_FILE_FORMAT.md`; `docs/TRACK_CONVERTER.md` converts
racetrack-database CSVs). Then start at step 4 with `ats-smooth`. Without a
dossier or terrain sidecar the track is dressed and grounded generically.

### Installing your track

1. **Server:** the YAML and the three `.msgpack` sidecars `ats-export` wrote
   beside it are all it needs. It is found on the next start and appears
   in the lobby. Without the sidecars it still runs, but with no barriers,
   curbs counted as grass and the ground held at road height off the track.
2. **Client:** the export from step 8 — `Mugello.uescene.json`,
   `Mugello.uemesh` and the preview `previews/Mugello.png`. In the editor
   build the game reads them straight from `content/tracks/export`; a
   packaged game reads them from `Tracks\` beside `ApexSim.exe` (the preview
   as `Mugello.png` there). The track picker's name, metadata, preview and
   content checksum all come from the export, keyed by `track_id`.
3. **Play it:** `./scripts/play_editor.ps1`, or drop the three files into a
   packaged game's `Tracks\` folder — no repackage. A release
   (`./scripts/build_release.ps1`) copies the YAML and sidecars into
   `Server/` and the exports into `Game/Tracks`, and refuses to run if a
   circuit has no export.

### Updating your track

- **Scenery only** (props in the track editor, `MANUAL_*` tables, the
  circuit style): re-run the dossier if you touched `osm_layout.py`, then
  `./scripts/build_track_levels.ps1 -Track Mugello`. Restart the server to
  pick up new walls and curbs, and the game (or `apexsim.track.Rescan` in its
  console) to pick up the new export.
- **The centerline** (widths, route, elevation, banking): everything is fitted
  to it, so run the whole of step 4 again in order, then step 8. A dossier or
  terrain built against the old line describes a road that has moved.
- **Anything in the YAML** changes its checksum. Re-export (step 8) so the
  client's export carries the new one and matches the server.
- Keep the `track_id`. The catalog, lap records and ghost laps are keyed by
  it.
- Anything that moves barriers, walls, the centerline or the ground should go
  through the AI survey again (step 9).

## Hud interface (not available yet)

### Creating a new hud element
(not available yet)

### Installing your hud element
(not available yet)

### Updating your hud element
(not available yet)
