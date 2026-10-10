# Modding ApexSim

Cars, tracks and the race HUD are files the game reads at runtime: nothing
is cooked into the client, so new content needs no Unreal editor and no
repackaging. This guide is the practical route for each. The developer docs
under [docs/](docs/README.md) go deeper; links below point at the right page.

## Two sides, one file

A server and a client each read their own copy of the content:

- **The server** simulates from `car.toml` files and track YAMLs (with the
  sidecars baked beside a YAML). It reads them at startup, so restart it
  after a change.
- **The client** builds each car from its `car.toml` and GLBs, and each
  circuit from its export (`<Stem>.uescene.json` + `<Stem>.uemesh`), the
  first time it draws them. Restart the game after a change; an editor or
  Development build can rescan instead with `apexsim.car.Rescan` /
  `apexsim.track.Rescan` in the console (a release package is a Shipping
  build and has no console).

**Content checksums.** Both sides take a CRC-32 of each `car.toml` and track
YAML, and the game compares the two when it loads a track or spawns your
car. A mismatch is a warning in the log and a toast on screen: the car or
circuit you see is not the one the server simulates. Every byte counts,
including client-only tables such as `[wheels]` or `[[livery]]` (line endings
do not). So whenever you change a car or a track, give every server and every
player the same `car.toml` and YAML, and re-export a track after any YAML
edit. Details: [docs/architecture.md](docs/architecture.md), "Content
checksums".

### Where things go

| | Source checkout | Installed release package |
|---|---|---|
| Your cars | `content/cars/custom/<folder>/` (read by both sides) | `Game/Cars/custom/<folder>/` (car.toml + GLBs + logos) and the same `car.toml` in `Server/content/cars/custom/<folder>/` |
| Class wheels | `content/wheels/` | `Game/Wheels/` |
| Your tracks, server side | `content/tracks/custom/<Stem>/` | `Server/content/tracks/custom/<Stem>/` |
| Your tracks, client side | `build/tracks/` (preview in `build/tracks/previews/`) | `Game/Tracks/` (`<Stem>.uescene.json`, `.uemesh`, `.png`) |
| Your HUD components | `content/hud/custom/<id>/` | `Game/Hud/custom/<id>/` |

`default/` holds the shipped content and is read first; `custom/` is yours,
gitignored in a checkout (but for its README) and left out of a package
unless `build_release.ps1` / `build_game_standalone.ps1` get
`-IncludeCustomCars` / `-IncludeCustomTracks`. Two rules hold across both
folders: **folder names and track stems must be unique**, and **a car's `id`
and a track's `track_id` must be unique**; a custom one reusing a shipped id
is skipped with a warning in the log.

From a checkout you need Rust (stable) and Python 3.11+ with `numpy`,
`Pillow` and `PyYAML` (the car generators also Blender, or `pip install bpy`).
On a fresh clone run `./scripts/initialize_content.ps1` once for the
materials, props and track exports git does not carry; `./scripts/play_editor.ps1`
then plays the editor build against a local server
([docs/building.md](docs/building.md)).

## Names on screen

ApexSim ships no trademarks: a real name lives in `name`, what the player
sees lives beside it. A track YAML's `display_name` is a sound-alike
(`Zandervoort`), `metadata.description` names the place, not the operator,
dossier corners carry a `display_name`, and car `name` / `brand` are
parodies (`Posh GT3 RS`). Class keys (`F1`, `GT3`...) stay in the files and
are shown as Formula, GT3 and so on. Your own content on your machine is
exempt, but follow the rule in anything you share; a track without
`display_name` shows its `name`. See
[docs/content/naming.md](docs/content/naming.md).

## Cars

A car is a folder holding a `car.toml` and the files it names: the body GLB
(`model`), optionally a driver figure, a DRS flap, livery logos and skins,
and its own road or steering wheels. Everything else (the class's road and
steering wheels) comes from the shared wheels folder. The server reads the
physics tables; the client reads the visual ones. The full reference is
[docs/content/cars.md](docs/content/cars.md); every physics key is in
[docs/server/vehicle-physics.md](docs/server/vehicle-physics.md).

### Making a car

**1. The body.** Either add a variant to a class generator, or bring your own
GLB.

- *Generator* (checkout, Blender): `scripts/content/cars/build_gt3.py`,
  `build_lmp2.py`, `build_hypercar.py` and `build_f1.py` each hold a
  `VARIANTS` table over a shared hull (`carlib.py`). Copy an entry, give it
  its own `folder`, `stem`, colours and shape and style keys, then in
  Blender's Python console:

  ```python
  VARIANT = "mycar"
  exec(open(r"E:\apexsim\scripts\content\cars\build_hypercar.py").read())
  ```

  Headless, set `APEXSIM_ROOT` to your checkout and run the same two lines
  under Python with `bpy`. It writes `<stem>.glb`, `<stem>_driver.glb` (and an
  F1's `<stem>_drs.glb`) into the car's folder and rewrites the generated
  tables in its `car.toml` (`[cockpit]`, `[driver]`, `[drs_flap]`).
  `APEX_EXPORT=0` skips the export while you iterate. The generators write
  into `content/cars/default/`; move the folder to `custom/` if it is yours.
- *Your own model*: metres, ground at z = 0, **nose on -Y**, driver on +X,
  exported as glTF with +Y up. **No wheels** (the client adds them;
  `scripts/content/cars/strip_wheels.py` measures and removes a model's own).
  Name the materials the client drives (`car_paint`, `car_accent`,
  `car_logo`, `car_brakelight`, `car_taillight`, ...; the table "Material
  slots the client drives" in [docs/content/cars.md](docs/content/cars.md)).
  Any other slot draws as authored and nothing changes it at runtime.

**2. The `car.toml`.** Copy one from a car of the same class in
`content/cars/default/` and change:

- `id`: a **fresh UUID** (`python -c "import uuid; print(uuid.uuid4())"`).
  It is how the client, the lobby and the lap records know the car; never
  reuse one, and keep it when you update the car.
- `name`, `brand`, `model_type`, `model_year`, `manufacturer_country`, and
  `model` (the body GLB's file name).
- `class`: the AI field is every car of the host's class. An existing class
  (`GT3`, `LMP2`, `Hypercar`, `F1`) races the others; a new one races only
  itself and needs its wheel in the wheels folder (`<class>.glb`, see
  "Wheels" in the cars doc).
- The physics tables (`[physics]`, `[aero]`, `[tires]`, `[engine]`, ...),
  range-checked at load: a car that fails is logged and skipped.
- The client tables: `[wheels]` (where the wheels sit; match your arches),
  `[cockpit]`, `[preview]`, `[sound]` (the engine synth) and
  `[[damage_part]]`. See the cars doc for each.

**3. Tune it** (checkout). Class grip is set so each car's ideal Silverstone
lap lands a few seconds off a real pole; `cargo test --release --test
grip_probe_test -- --ignored --nocapture` in `server/` prints every car's.
If yours is far from its class, adjust its grip, lift coefficients or power,
not the class. `scripts/content/cars/preview_cars.py` (Blender, `CARS =
["custom/<folder>"]`) renders it with the wheels where `[wheels]` puts them.

### Liveries

Livery 0 is the model as authored. The others are `[[livery]]` tables at the
end of `car.toml`:

```toml
[[livery]]
name = "Greenline Forest"
paint = [0.008, 0.090, 0.035]          # linear RGB, recolours car_paint
accent = [0.780, 0.680, 0.420]         # optional, car_accent
metallic = 0.60                        # optional
logo = "textures/livery_forest.png"    # optional, replaces the car_logo texture
```

A table needs a `name` and one of `paint`, `skin` (a texture for every
`car_skin*` slot) or `textures` (`["SLOT=file", ...]`, on **one line**);
paths are relative to the car's folder. A player's pick is saved as an
index, so **append** new liveries rather than reordering. The shipped cars'
liveries are written by `scripts/content/cars/liveries.py` (everything below
its marker line is rewritten on each run); for your own car, write the tables
by hand. A livery change is a change to `car.toml`, so it changes the
checksum too.

### Installing and updating a car

- **Checkout**: put the folder in `content/cars/custom/`. Restart the server
  and the game (or `apexsim.car.Rescan`).
- **Installed game**: copy the whole folder to `Game/Cars/custom/<folder>/`
  and its `car.toml` to `Server/content/cars/custom/<folder>/car.toml`. Restart
  both. A new class also needs its wheel in `Game/Wheels/`.
- **Updating**: edit, then restart (or rescan). A changed GLB needs only the
  game; a changed `car.toml` needs the server too, and every player the file.

## Tracks

A track is a folder named after its stem, `<Stem>/`, with every file
carrying the stem:

| File | What it is | Made by |
|---|---|---|
| `<Stem>.yaml` | the centerline (nodes, widths, banking), raceline, grid, sectors, DRS zones, metadata; what the server simulates | a converter, an OSM route or an importer, then the data tools |
| `<Stem>.ats` | the scene: curbs, run-off, props, pit lane, decals | `seed_scene.py`, then `ats-dress` / the track editor |
| `<Stem>.layout.json` | optional: the real furniture from OpenStreetMap | `scripts/osm_layout.py` |
| `<Stem>.dem.msgpack` | optional: real terrain | `scripts/dem_fetch.py` |
| `<Stem>.{ground,curbs,walls,road,pit}.msgpack` | the server's sidecars: terrain, track limits, barriers, the road mesh, the pit lane | `ats-export` |
| `build/tracks/<Stem>.uescene.json`, `.uemesh`, `previews/<Stem>.png` | what the client builds the circuit from, and its picture | `ats-export`, `build_track_catalog.py` |

The YAML needs a fixed `track_id` (a UUID; without one nothing can match
it), `name`, `display_name` and a place-based `metadata.description`
([docs/content/track-format.md](docs/content/track-format.md)).

### Making a track (checkout)

The full procedure, with the review points, is "Adding a new circuit" in
[docs/content/track-pipeline.md](docs/content/track-pipeline.md); the
Nordschleife in [docs/content/circuits.md](docs/content/circuits.md) is a
worked example. In short, from the repo root:

1. **A centerline.** From a CSV trace (`x_m,y_m,w_tr_right_m,w_tr_left_m`):

   ```powershell
   cd server
   cargo run --release --bin convert_track -- -t track.csv [-r raceline.csv] -o out.yaml -n "Real name" --display-name "Shown name"
   ```

   Save it as `content/tracks/custom/<Stem>/<Stem>.yaml`. Or route it over
   OpenStreetMap's raceway ways: add a `LAPS` entry to
   `scripts/osm_centerline.py` and a `BBOXES` entry to `scripts/osm_layout.py`,
   then `python scripts/osm_centerline.py <Stem> [--dry-run] [--plot out.png]`
   (it writes under `content/tracks/default/`; move the folder to `custom/`
   if it is yours, and the tools find it there). No raceline?
   `python scripts/generate_race_line.py --track <yaml>`.
2. **A scene.** `python scripts/seed_scene.py <Stem>` writes a first `.ats`:
   the start line, grass either side, curbs at every apex.
3. **Optional real data**, in this order, because each step is fitted to the
   one before (the "Refresh order" in the track pipeline doc):
   `osm_layout.py <Stem>` (the dossier), `dem_fetch.py <Stem>`,
   `dem_elevation.py <Stem>`, then `ats-smooth` and `ats-bank` on the YAML,
   `drs_zones.py`, the dossier and DEM refitted with `--offline`, and
   `track_location.py <Stem>`. Without a dossier or terrain the track is
   dressed and grounded generically.
4. **Bake it:**

   ```powershell
   ./scripts/build_track_levels.ps1 -Track <Stem> -SkipMaterials
   ```

   This dresses the scene (`ats-dress`), exports the client files and the
   server sidecars (`ats-export`) and draws the preview. Drop
   `-SkipMaterials` the first time on a machine whose track materials are not
   yet baked; `-SkipDress` keeps hand edits made in the track editor
   (`cargo run` in `track-editor/`) from being re-laid.
5. **Check it.** `python scripts/check_walls.py --openings <Stem>` finds holes
   in the barriers; the AI survey (`$env:SURVEY_TRACKS = "<Stem>"`, then
   `cargo test --release --test ai_race_start_test
   survey_ai_races_on_every_circuit -- --ignored --nocapture` in `server/`)
   drives a field round it and reports contact and off-road time.

### Installing and updating a track

- **Checkout**: the folder under `content/tracks/custom/` and the export in
  `build/tracks/`, which `build_track_levels.ps1` already put there. Restart
  the server and the game (or `apexsim.track.Rescan`).
- **Installed game**: copy `<Stem>.uescene.json`, `<Stem>.uemesh` and the
  preview (as `<Stem>.png`) into `Game/Tracks/`, and the YAML with its
  `.msgpack` sidecars into `Server/content/tracks/custom/<Stem>/`. Restart
  both. Without the sidecars the track still runs, but with no barriers,
  curbs counted as grass, the ground held at road height off the track and
  no pit stops.
- **Updating**: any YAML change changes its checksum, so re-run the bake and
  hand out the new YAML, sidecars and export together. A change to the
  centerline moves everything fitted to it: run the data steps again in
  order. Keep the `track_id`: lap records, ghosts and the catalog are keyed by
  it.

## Importing from Assetto Corsa

Cars and tracks from your own Assetto Corsa install convert in one step.
They stay on your machine: imports are never shipped or redistributed, and
content encrypted by Custom Shaders Patch is refused.

- **Installed game**: the launcher's **Manage content > Import from Assetto
  Corsa**. Pick a car or track folder (or a whole `cars` / `tracks` folder).
  It needs Python 3.11 or newer from python.org and sets up the rest on first
  use (that step needs internet). The files land in `Game/` and
  `Server/content/` directly.
- **Checkout**: `python scripts/ac_car_import.py "<AC>\content\cars\<car>"`
  (into `content/cars/custom/`) or `python scripts/ac_import.py
  "<AC>\content\tracks\<track>"` (into `content/tracks/custom/` and
  `build/tracks/`); `--list` shows layouts or skins, `--all <folder>` takes a
  whole collection, `--force` replaces an earlier import.

Then restart the server and the game. An imported track is marked in its
`.ats` and the bake, dressing and smoothing leave it alone: to rebuild one,
run the command its `<Stem>.import.json` records. Everything else, including
what is not supported, is in
[docs/content/ac-import.md](docs/content/ac-import.md).

## HUD

The race HUD is a set of components, one folder each, laid out in JSON and
bound to the data points the game publishes every frame. Nothing about it
touches the server. The full reference (every element, function and data
point) is [docs/game/hud-modding.md](docs/game/hud-modding.md).

**Moving things** needs no files: **Settings > Gameplay > HUD layout > Edit
layout** moves, resizes, adds and removes panels and saves to
`custom/layout.json` (several named layouts too, under `custom/layouts/`).

**Changing or adding a panel**, in `content/hud/custom/` (checkout) or
`Game/Hud/custom/` (installed game):

- *Change a shipped panel*: copy its folder from `default/` into `custom/`
  under the same name and edit the copy.
- *Hide one*: `custom/<id>/component.json` holding `{ "enabled": false }`.
- *Add one*: a new folder with a `component.json`, for example:

  ```jsonc
  { "name": "Big speed", "region": "bottom", "margin": [0, 0, 0, 170],
    "root": { "type": "text", "font": "display", "size": 64, "text": "{fmt(car.speed)}",
              "color": "=car.rpm_fraction > 0.95 ? 'error' : 'text'" } }
  ```

A component that fails to load is named on screen with the reason; a
misspelt data point is a warning in the log. Restart the game to see a
change, or in an editor or Development build run `apexsim.hud.Reload` during
a race (`apexsim.hud.Data [filter]` lists every data point and its value).
