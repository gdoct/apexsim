# Cars

A car is a folder: a `car.toml` and the GLB files it names. The server reads
the car.toml for the physics; the game reads the same file and builds the car
from its GLBs at runtime. Nothing about a car is cooked, so a changed or new
car needs no editor import and no repackage. Physics keys:
[apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md); importing a
car from Assetto Corsa: [ac-import.md](ac-import.md).

## Code

- `cars/{default,custom}/<folder>/`; `wheels/` (class road
  wheels, `steering/` class steering wheels); `cars/_textures/`.
- `tools/generators/cars/` (`build_{gt3,lmp2,f1,hypercar,rally}.py` on `carlib.py`,
  `rally_cars.py`, `skin_layout.py`, `rally_skins.py`, `textures.py`, `liveries.py`, `damage_parts.py`, `preview_cars.py`,
  `strip_wheels.py`), `tools/generators/wheels/` (`build_wheels.py`,
  `build_steering_wheels.py`), `tools/lib/ApexCars.ps1` (packaging checks).
- Server: `apexsim/server/sim/src/car_loader.rs` (`car_toml_paths`, `CarConfig`).
- Client, `apexsim/game-unreal/Source/ApexSim/`: `Cars/` (`UApexCarContentSubsystem`
  and `ApexCarContent::`, `ApexCarToml`, `ApexGlbReader` (`ApexGlb::`),
  `ApexBlockCompress` (`ApexBc::`), `ApexCarMaterials`),
  `Catalog/ApexCatalogRows.h` (`FApexCarCatalogRow`), `Race/` (`ApexRaceCarActor`,
  `ApexCarWheels.h`, `ApexCarLivery.h`, `ApexCarDrsFlap.h`, `ApexCarDriver.h`,
  `ApexCarDamage.h`, `ApexCarEffectsActor.h`).
- Editor module: `ApexTrackMaterialGraphs.cpp` (car parent materials, via
  `ApexMaterialBake`), `ApexCarImportCommandlet` (editor inspection only).

## What a car is on disk

`<folder>/car.toml` (read by both sides) plus the files it names, all read by
the client only: the body GLB (`model`), `<stem>_drs.glb` (`[drs_flap]`),
`<stem>_driver.glb` (`[driver]`), livery logos and skins, and optionally the
car's own road wheels and steering wheel; class wheels come from
`wheels/`.

**Where cars are found.** The client searches `-ApexCarsDir=<dir>[+<dir>]`,
then `Cars/` beside `ApexSim.exe` in a package (wheels in `Wheels/`), else the
content root's `cars` (wheels in `wheels`)
(`UApexCarContentSubsystem::CarDirectories`). Each is read as `default/` then
`custom/`, or as it is when it has neither (`CarFolders`); the server does the
same (`car_loader::car_toml_paths`, `CAR_DIRS`). The first car to claim an
`id` wins on both sides, so a custom car reusing a shipped id is skipped with
a warning. A folder name must be unique across `default/` and `custom/`:
meshes and `/Game/Cars/<folder>` are keyed by it (`Test-ApexCars` flags a
clash).

**Adding a car**: a folder with a car.toml (fresh UUID `id`, `class`,
physics tables, `model`) and a body GLB in the frame below with the client's
slot names and no wheels; then `[wheels]`, `[cockpit]` and optionally the
other visual tables. In an installed game: its folder in `Game/Cars/custom`, its
class wheel in `Game/Wheels` if the class is new, and the same car.toml in
`Server/content/cars/custom/<folder>`. `build_game_standalone.ps1` /
`build_release.ps1` ship `custom/` only with `-IncludeCustomCars`; both abort
if a car.toml names a file that is not there.

## car.toml: the tables

| Table | Read by | What it holds |
|---|---|---|
| top level: `id`, `name`, `model`, `class`, ... | both | `id` (a UUID) is the catalog key, and with the name all the wire carries; `class` sets the AI field and the client's class defaults |
| `[physics]`, `[aero]`, `[tires]`, `[[tires.compound]]`, `[engine]`, `[[engine.torque_curve]]`, `[transmission]`, `[drivetrain]`, `[differential]`, `[fuel]`, `[hybrid]`, `[suspension]`, `[brakes]` | server | the car model: [apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md). The client also reads `[brakes] material` (the HUD's brake window), `[physics] mass_kg`, `max_steering_angle_rad`, `[suspension]` camber and toe, `[engine]` idle/redline/limiter, `[tires]` window and each compound's `name` / `kind` |
| `[wheels]` | client | where the road wheels go (below) |
| `[cockpit]`, `[preview]` | client | eye, steering wheel, mirrors; turntable framing |
| `[driver]` | client | the driver figure's GLB |
| `[drs_flap]` | client | the F1 flap's GLB and hinge |
| `[[damage_part]]` | client | boxes of the body that come off |
| `[damage]` | client | `engine_m`: where the engine sits, metres ahead of the model origin as the axles are (engine smoke comes from there; unset, from the tail) |
| `[[livery]]` | client (server: names only) | repaints |
| `[sound]` | client | the engine synth's description |

The server ignores the client-only tables, but they are part of the file's
CRC (`ContentCrc`, [apexsim/docs/architecture.md](../../apexsim/docs/architecture.md)).

Generated tables sit **above** the liveries' marker line
(`# --- liveries: written by tools/generators/cars/liveries.py ...`);
`liveries.py` owns everything below it. The build scripts (`[cockpit]`,
`[driver]`, `[drs_flap]`, via `carlib.write_table`) and `damage_parts.py`
(`[[damage_part]]`) each replace their own table on every run.

## The shipped cars and their generators

Seventeen cars are generated by five Blender scripts into
`cars/default/<folder>/`:

| Script | Class | Folders |
|---|---|---|
| `build_gt3.py` | GT3 | `posh-gt3rs`, `limbotiti-caravan-gt3`, `murcetes-amd-gt3` |
| `build_lmp2.py` | LMP2 | `yotota-lmp2`, `posh-lmp2`, `fugazzi-lmp2`, `jeanetti-lmp2` |
| `build_hypercar.py` | Hypercar | `panini-zomba-hypercar`, `fugazzi-994p-hypercar`, `bugotti-chiffon-hypercar` |
| `build_f1.py` | F1 | `fugazzi-sf26`, `murcetes-amd-w17`, `mclarsen-mcl40`, `ashton-marvin-amr26` |
| `build_rally.py` | Rally | `oudi-kwattro-s1`, `pugeot-205-t16`, `lancha-037-rally` |

The rally cars are Group B, 1984-85, on their real figures and not balanced
against each other: a front-engined five-cylinder turbo with four-wheel
drive, a mid-engined turbo four with four-wheel drive, and a supercharged
mid-engined four driving the rear wheels. Each has a tarmac slick, a wet
and a studded snow tyre (`kind = "snow"`, `snow_grip = 2.0`, `stud_loss = 0.03`: a third
of its tread worn on bare tarmac pulls all its studs,
[apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md#compounds)).
Their shape data is in `rally_cars.py` (plain Python, shared with the
livery painter); `build_rally.py` is the GT3 build's method with a rally
car's parts (box flares, period lamps, a pod of four driving lamps, mud flaps, a roof scoop, a shovel
splitter and boot wing, a roof wing or a ducktail) and both seats.

**Running one.** Inside Blender: set `VARIANT = "<name>"` (a key of the
script's `VARIANTS`), then
`exec(open(r"E:\apexsim\apexsim-content\tools\generators\cars\build_<class>.py").read())`.
Headless, on the `bpy` wheel (`pip install bpy`, Python 3.11): set
`APEXSIM_CONTENT` to the content root (without it a script exec'd in Blender
falls back to `E:\apexsim\apexsim-content`), define `VARIANT` and `exec` the script. Each run exports
`<stem>.glb`, `<stem>_driver.glb` (and the F1's `<stem>_drs.glb`) beside the
car.toml and rewrites its generated tables; `APEX_EXPORT=0` skips the export.

The variant scripts hold only shape and livery data; everything mechanical
is in `carlib.py`, so a fix there lands on every generated car. `textures.py`
(plain Python) writes the shared texture kit (carbon twill, sponsor atlas,
number panels, tyre lettering; all names invented).

**Frame.** Nose on -Y, ground at z = 0, metres, driver on +X; exported glTF
+Y up, the nose lands along Unreal local Y and `AApexRaceCarActor` yaws the
mesh -90 degrees onto world +X. **Stance**: `[wheels]` must agree with the
arches the script cut:

| Class | Tyre radius f / r | Track | Axles |
|---|---|---|---|
| GT3 | 0.345 / 0.355 m | 1.700 m | +-1.375 m |
| LMP2 | 0.355 / 0.365 m | 1.580 m | +-1.500 m |
| Hypercar | 0.360 / 0.360 m | 1.620 m | +-1.550 m |
| F1 | 0.360 / 0.360 m | 1.580 / 1.520 m | -1.650 / +1.750 m |
| Rally | 0.305-0.315 m | 1.43-1.50 m | each car's own (`rally_cars.py`) |

**Authoring rules.** Keep new parts inside the bounding box (a car without
an authored eye derives its cockpit from it, and the turntable frames by it).
Keep the loft's winding (`Loft.build`): inward faces render as flat paint.
The road must be visible from the eye: the builds print
`carlib.sightline(...)`, and over about 10 m is a bonnet or cowl too high;
judge with `preview_cars.py`'s `cockpit` and `mirror` views. No body carries
a steering wheel or road wheels: the client adds both.

**Previews.** `preview_cars.py` (Blender; headless `APEX_PREVIEW_CYCLES=1`)
renders into `props/_preview/cars/`: set `CARS` (folders,
`"custom/<folder>"` for the player's own) and optionally `VIEWS`, `LIVERY`,
`DRS_OPEN`, then `exec` it. It draws the wheels where the car.toml says. The
`<folder>_hero.png` renders are the marketing page's car pictures.

### Material slots the client drives

Slot names are the glTF material names; keep them when re-exporting.

| Slot | Meaning |
|---|---|
| `car_paint`, `car_accent` | body and accent colour: a livery repaints these (`BaseColorFactor`, `MetallicFactor`) |
| `car_logo` | the flank wordmark texture: a livery's `logo` replaces it |
| `car_skin`, `car_skin_*` | an imported car's skin textures: a livery's `skin` replaces them |
| `car_brakelight` | `AApexRaceCarActor` drives `EmissiveFactor`: black off, the authored colour x `apexsim.car.BrakeLightNits` (3000) once telemetry brake passes 2% |
| `car_taillight` | running lights, kept lit by the actor: x `apexsim.car.TailLightNits` (700) by day, a share of the brake level with the headlights on |
| `car_mirror_left`, `car_mirror_right` | mirror glass: the cockpit rig paints its rear-view captures on them (u from the driver's right to left, v top-down) |
| `car_glass`, `car_lens_tint` | translucent (glTF `BLEND`) |
| `car_carbon`, `car_sponsor`, `car_number` | the texture kit's twill, sponsor atlas and number panels; untouched by liveries |
| `car_trim`, `car_interior`, `car_headlight`, `car_rainlight`, `car_display`, ... | authored look only; nothing at runtime changes them |

The car parents ignore `KHR_materials_emissive_strength`; light levels come
from `EmissiveFactor`.

## Runtime loading

`UApexCarContentSubsystem` is an engine subsystem (a car's meshes belong to
no world). On first use it reads every car.toml (`ApexCarToml::Parse`) into
an `FApexCarCatalogRow` keyed by `id`, with `SourceCrc` (for
`VerifyCarContent`), the file paths (`RuntimeModel`, `Wheels.RuntimeModel`,
`DrsFlap.RuntimeModel`, livery `RuntimeLogo` / `RuntimeSkin` /
`RuntimeTextures`) and every visual table. `UApexMenuFlowSubsystem::FindCarRow`
returns it first.

`UApexCarContentSubsystem::LoadModel(Path)` builds a GLB on first use and
caches it until `apexsim.car.Rescan`:

1. **Read** (`ApexGlb::Parse`, any thread): the GLB flattened to one mesh
   through its node tree, one section per material; glTF `(x, y, z)` m to
   Unreal `(x, z, y)` cm (Interchange's frame; the swap also turns glTF's
   front face into Unreal's, so indices keep their order). Images are decoded
   with a box-filtered mip chain and block compressed on the worker
   (`ApexBc::CompressChain`): BC1 opaque, BC3 when a `MASK` / `BLEND`
   material reads the alpha, BGRA8 only for sizes not a multiple of four.
   Textures are transient and cached by content hash.
2. **Materials**: a dynamic instance per slot of a cooked parent under
   `/Game/Materials/Car`: `M_ApexCarTranslucent` (`BLEND`), `M_ApexCarMasked`
   (`MASK`), `M_ApexCarClearCoat` (`KHR_materials_clearcoat`), `M_ApexCarTyre`
   for an opaque `wheel_tyre` (when baked), else `M_ApexCarOpaque`, with Interchange's parameter names (`BaseColorFactor`,
   `BaseColorTexture`, `MetallicFactor`, `RoughnessFactor`, `EmissiveFactor`).
3. **Mesh**: built on the tracks' fast path, no collision.

The race director `Prefetch`es the roster's GLBs on the thread pool; a GLB
that will not read or build is logged once and skipped until a rescan.
Everything that draws a car goes through
`ApexCarContent::LoadBody` / `LoadBodyPieces` / `LoadMesh` / `LoadLogo`,
which take the runtime file when the row has one and the cooked soft pointer
otherwise.

**The parents** are baked by `-run=ApexMaterialBake [-force]` with the track
ones (all two-sided; the masked one clips `alpha - AlphaCutoff + 0.5` at a
fixed 0.5 because a dynamic instance cannot move the clip value). Without them
a car draws in flat colours and the log says so. `M_ApexCarSmoke` (damage
effects) is baked with them.

**Fallbacks.** `DT_CarCatalog` serves a car with no folder on this machine,
and hand-tuned framing and cockpit points a car.toml leaves out
(`ApexCarContent::MergeCockpit`). `ApexCarImport` imports a car as
`/Game/Cars/<folder>/SM_<folder>` for the editor only (`/Game/Cars` is never
cooked):

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ../apexsim/game-unreal/ApexSim.uproject -run=ApexCarImport -all
#   or -car=<folder> / -list / -remove=<folder> / -force / -dryrun
```

**Editing a car**: `apexsim.car.Rescan` (or restart); restart the server too
if the physics changed.

## Wheels

Bodies carry no wheels. Each class has a shared wheel,
`wheels/<class>.glb` (`f1`, `gt3`, `lmp2`, `hypercar`, `rally`), built by
`build_wheels.py` (Blender; set `WHEEL_CLASSES = ["hypercar"]` first to
rebuild only some): hub at the origin, axle along glTF X, the face on +X,
everything inside the tyre's radius and width. Slots `wheel_tyre`,
`wheel_mark` (sidewall lettering, which is what makes the spin visible),
`wheel_band`, `wheel_rim`, `wheel_nut`, `wheel_brake`, and `wheel_cover` on
the F1 wheel.

```toml
[wheels]
model = "lmp2"          # wheels/lmp2.glb
front_axle_m = 1.500    # ahead of the model origin
rear_axle_m = -1.500
front_track_m = 1.560   # hub centre to hub centre
rear_track_m = 1.560
front_radius_m = 0.355  # also the hub height: the tyre stands on z = 0
rear_radius_m = 0.365
front_width_m = 0.310
rear_width_m = 0.360
# rear_model = "..."    # optional: a different wheel on the rear pair
```

A `model` (or `rear_model`) ending in `.glb` or holding a `/` is a GLB in the
car's own folder (`ApexCarToml::IsCarLocalWheel`), in the class wheel's frame
and slot names; anything else is a class wheel.

`FApexCarWheelSet` (`Race/ApexCarWheels.h`) hangs four copies off the body,
sizes each from the mesh bounds to its axle's diameter and width, turns the
right-hand pair half round (not mirrored) so the face is outboard, leans and
turns each by its axle's `[suspension]` static camber and toe
(`camber_*_deg`, `toe_*_deg`: the car's own figures, not the driver's set-up),
steers the fronts by `steering x max_steering_angle_rad` and rolls all four by how far
the drawn car moved along its nose, capped at `apexsim.car.WheelMaxDegPerFrame`
(12) a frame so the spokes do not strobe. A row without wheels draws none.

**Tyre look.** The client reads each `[[tires.compound]]`'s `name` and
`kind` (`slick`, `intermediate`, `wet`) onto `Compounds`; without the tables a
car has the five defaults (soft, medium, hard, intermediate, wet). The
`wheel_tyre` slot is an instance of `M_ApexCarTyre` (`ApexMaterialBake`,
`BuildCarTyre`), one per wheel model like every slot; each wheel component's
custom primitive data (`ApexCarMaterials::TyreCpd`, from index 16, past the
damage graph the rims read) carries its tyre's own state, written by
`FApexCarWheelSet` only when a value changes. The class tyres carry no UVs
(`build_wheels.py` lathes them), so the graph lays everything from the mesh's
local position: `theta` round the axle (X) from +Y toward +Z, the radius and
`x` across the tread, both normalised by the tyre's size in mesh units.

- **Tread**: telemetry's `compound` picks the kind (`ApexWheels::TreadIndex`:
  0 slick, 1 intermediate, 2 wet, 3 snow). A snow tyre draws deep square
  blocks in three lanes a side, staggered, with a polished steel stud in
  each block (metallic in the graph).
  A wet draws four channels and swept blocks whose V points forward on top
  (`Chevron` is -1 on the left, whose model rolls about its own -X); an
  intermediate two thin channels and shallow sipes. Each pitch is a whole
  number round the tyre, so there is no seam. Grooves darken and roughen the
  rubber and tilt the normal (world space, by finite differences of the
  pattern, no tangents); below a pixel they fade to their average shade, and
  wear wears them down to 15%.
- **Compound ring**: an intermediate's sidewalls carry a green ring, a
  wet's a blue one and a snow tyre's a pale ice white (`ApexWheels::CompoundBandColour`). A model with its own
  `wheel_band` ring has it repainted (each wheel's own instance; a slick puts
  the model's colour back); the tyre parent draws the same ring at 87% of the
  radius (`TyreBandWidth` high) for a model without one (the GT3 wheel).
- **Wear** (`tyre_wear`): lighter, matte streaks round the tread, and from
  45% grained specks.
- **Flat spots** (`flat_spot`): a glazed lighter patch, longer the deeper the
  spot, laid at the wheel's contact angle when it is ground
  (`ApexWheels::ContactTheta`). The server sends one depth per tyre; the
  wheel set splits it by lock-up (`FApexCarWheelSet::SetTyreSurface`: a rise
  more than 0.5 s after the last one starts a new spot, a third deepens the
  nearer one, wear takes them down alike), up to two per tyre. The second
  rides in the tyre data's one free slot (`TyreCpd::FlatSpot2`, packed by
  `ApexWheels::PackFlatSpot`). A late joiner sees the depth so far as one
  spot.
- **Water**: each tyre soaks up the water under it, which a newer server
  sends per tyre (`tyre_water`, the physics' own road sample at the wheel);
  from an older one the race director's figure for the car stands in for
  all four (`TyreWaterFor`: the road state's cell, else the drawn sky's road
  wetness; rain falling counts as 40% of its intensity). Soaked in about
  1.5 s and dried over about 20 s (`ApexWheels::WetnessStep`): darker and
  glossy, water standing in the grooves.
- **Pickup** (`tyre_pickup`): the marbles under the tyre stick to the
  tread in a few seconds and are flung off over a lap, washed off on a wet
  road: lighter, matte crumbs across the tread. The server works it out
  (`TireData::pickup`, `tyre_thermal::step`, looks only) and sends it, so
  every viewer and a late joiner see the same tyre; from an older server
  or on a stream the car integrates the same sums from `tyre_marbles`
  (`AApexRaceCarActor::UpdateTyreSurface`). A fresh set starts clean.
- **Heat and blisters** (`tyre_c` against the catalog row's window,
  `SetTyreWindow`; `tyre_blister`): a tread over its optimal goes glossy and
  a shade browner; one run a window over its top blisters, dark torn pits
  on the shoulders, through in about a minute and kept until the set is
  changed. The blistering is the server's (`TireData::blister`) as the
  pickup is, the car's own sum without it.
- **Sidewall bulge**: on a deflated tyre the sidewall's normal is turned out
  along the axle where the tread meets the road, so the light shows the
  bulge the squat makes.
- **Deflation** (`tyre_kpa`): round down to 100 kPa, flat at the server's
  `PUNCTURED_KPA` (`ApexWheels::DeflatedShare`). A flat tyre squats 9% of its
  radius: the hub drops by that and the graph's WPO flattens the tread onto
  the road and bulges the sidewalls there.

A project baked before `M_ApexCarTyre` draws its tyres with `M_ApexCarOpaque`
and the old tint (`ApexWheels::TreadedTint`: matte blue-grey for a rain tyre).
Telemetry's `slide_flags` make the actor puff tyre smoke off a sliding or
locked wheel (`UpdateTyreSmoke`, through the effects actor).

`strip_wheels.py` (Blender) measures a GLB's tyres into `[wheels]` figures
(`measure`) or deletes its wheel parts (`strip`).

## Cockpit, steering wheel and driver

How the cockpit rig, mirrors and look-around work is in
[apexsim/docs/game/cameras.md](../../apexsim/docs/game/cameras.md). What a car declares:

```toml
[preview]                         # garage turntable: offset_cm, rotation_deg (pitch, yaw, roll), scale
[cockpit]
style = "closed"                  # open | closed (else derived from the class)
eye_cm = [-35.0, 38.0, 108.0]     # the car's frame: +X nose, +Y right, +Z up
wheel_cm = [10.0, 38.0, 90.0]
# mirror_centre_cm / mirror_left_cm / mirror_right_cm and their *_size_cm
wheel_rake_deg = 18.0             # rim tilt, positive = top toward the driver
wheel_lock_deg = 270.0            # rim turn at full steering, one way
steering_wheel_model = "steering_wheel.glb"   # the car's own wheel
rig_wheel = false                 # hide the rig's rim
rig_dash = false                  # hide the rig's hub display
```

Every key is optional; a zero `wheel_rake_deg` / `wheel_lock_deg` means
"derive from the style" (`0.01` for an upright rim), and a missing key keeps
the `DT_CarCatalog` row's value. Without an eye the client derives the layout
from the mesh bounds (`ApexCockpit::DeriveLayout`, mirrored by
`carlib.cockpit_points()`); the generated cars author theirs
(`carlib.authored_cockpit()`, `write_cockpit_table`). Mirrors are laid off
the derived eye unless named, so a car that authors its eye names its
mirrors too. `wheel_lock_deg` is also the Auto steering lock for a wheelbase
([apexsim/docs/game/input-and-ffb.md](../../apexsim/docs/game/input-and-ffb.md)).

**Steering wheels.** A car without its own drives with its class steering
wheel, `wheels/steering/<model>.glb` for its `[wheels] model`,
written with a `<model>.json` by
`python tools/generators/wheels/build_steering_wheels.py [f1 gt3 ...]`. The JSON's `dash_cm` / `dash_width_cm` put the
rig's hub display on that wheel's screen (`FApexCockpitOverrides::RuntimeDashCm`).
A car's own `steering_wheel_model` is drawn in place of the rig's rim and
turned exactly as it would be; author it straight and upright in the car
GLB's axes, hub at the origin, rim in glTF XY, column along glTF +Z toward the
nose, and give the real column tilt as `wheel_rake_deg` rather than baking it
in (it would be tilted twice).

**Driver.** Every generated car has a driver figure, `<stem>_driver.glb` in
the body's frame, named by `[driver] model`, built by
`carlib.driver_figure()`. The client (`FApexCarDriver`,
`AApexRaceCarActor::SetDriver` / `SetDriverVisible`) draws him on every car,
hides him for the car the cockpit camera sits in while keeping his shadow,
and paints his helmet with the livery. He does not move: the rig's wheel
turns under static gloves.

## Liveries

The works livery (index 0) is the GLB as authored; the others are
`[[livery]]` tables:

```toml
[[livery]]
name = "Greenline Forest"
paint = [0.008, 0.090, 0.035]     # linear RGB
accent = [0.780, 0.680, 0.420]    # optional
metallic = 0.60                   # optional
logo = "textures/livery_forest.png"   # optional: replaces car_logo

[[livery]]                        # a texture livery (an imported car's skin)
name = "Gulf"
skin = "skins/gulf/Skin_00.png"   # BaseColorTexture of every car_skin* slot
textures = ["EXT_RIM=skins/gulf/EXT_RIM.png", "INT_Banner=skins/gulf/banner.jpg"]
preview = "skins/gulf/preview.jpg"    # optional: shown beside the livery in car select
```

A table needs a `name` and one of `paint`, `skin` or `textures`. `textures`
must be on **one line** (the parser reads line by line); a slot the body
lacks is ignored, a missing file is a warning. A skin changes only the
texture, not the slot's factors. Paths are relative to the car folder.
`tools/generators/cars/liveries.py [folder ...]` (Pillow; fonts via
`APEX_FONTS`) owns the shipped cars' tables and logos, appending new schemes
so a saved pick keeps its index.

**Projected skins (the rally cars).** A rally livery is stripes, roundels
and sponsors, which two colours and a logo cannot draw, so the rally
bodies' painted shell is the `car_skin` slot and every livery is a skin.
`build_rally.py` projects each `car_skin` face flat onto one of five
regions of a square texture by its normal (`skin_layout.py`: both flanks,
the plan, the nose and the tail, each laid out as someone standing there
sees it, in a fixed `skin_box` per car), and `rally_skins.py` paints a
scheme in metres on the same projection: a design (`sweep`, `stripes`,
`chevron`, `split`, `solid`) in the scheme's colours, then what every
scheme carries (a number roundel on the front doors, sponsors on the doors,
quarters and bonnet, the event plate on the bonnet and the tail, a banner
on the screen's sun strip, the maker over the sills, the crew's names). The
works skin is `textures/skin_works.png`, the GLB's own; the others are
`[[livery]]` tables with `skin`, plus `paint` (mirrors, the driver's helmet)
and `accent` (wing, scoop, mud flaps). `liveries.py` draws them, so it runs
**before** `build_rally.py`. Every sponsor, event and crew name is invented.

**The player's own liveries** live outside the car, in
`cars/custom/liveries/<car folder>/<name>/livery.toml` (the player's, never
shipped): one or more `[[livery]]` tables in the format above, their paths
relative to that folder. `python tools/livery_template.py <car folder>`
writes the car's template beside them, in `.../<car folder>/template/`: the
UV layout of each `car_skin*` and `car_logo` slot as a wireframe PNG, the
textures the model carries there, and a `livery.toml.example`. A car built
from colours and a logo (the generated cars) has only a `car_logo` sheet;
its own liveries set `paint`, `accent` and `logo`. The game lists them after
the car's own liveries, named "<name> (yours)" (`FApexCarLivery::bOwn`,
`ApexLivery::OwnLiveryFolders`). They are this client's alone: the pick goes
to the server as 0, the works livery, and only the player's own car is
painted with it (`ApexLivery::ForServer`, `ForLocalCar`); other players see
the works. `-ApexLivery=N` picks livery N for an `-ApexAutoRace`.

The server keeps only the names (`CarConfig::livery_names`); the pick goes
out as `SelectCar.livery` and comes back to everyone as `RosterEntry.Livery`
(AI dealt their model's liveries in turn). `ApexLivery::Apply` repaints the
race car and the turntable on the car's own instances, tagging the slots it
textured (`ApexLiveryTextured:<slot>`) so livery 0 can put them back.

## DRS flap

The F1 bodies are exported without the rear wing's upper flap: `build_f1.py`
writes it as `<stem>_drs.glb`, origin on the hinge, and the table:

```toml
[drs_flap]
model = "fugazzi_sf26_drs.glb"
hinge_forward_m = -2.5298     # ahead of the body origin
hinge_up_m = 0.8226           # above its floor
open_deg = 25.0
```

`FApexCarDrsFlap` swings it open over `ApexDrs::SwingSeconds` (0.18 s) while
telemetry's `bDrsOpen` is set; liveries, the ghost tint and a detached rear
wing carry it along. The DRS rule is server-side
([apexsim/docs/server/vehicle-physics.md](../../apexsim/docs/server/vehicle-physics.md)).

## Visible damage

The server sends five percentages per car (front, rear, left, right,
engine) and where along each body zone's face the hits landed
(`DamageState::hit_at`, -1 to 1, the nose and tail across the car with the
left positive, the flanks along it with forward positive; `damage::hit_zone`
works it from the collision angle; `CompactCarState.damage_at` and the
stream's `CarExtra`). A zone has up to two dents: a hit folds into the nearer
one, moving its centre to the damage-weighted mean of its hits, unless the
zone has one and the hit lands more than `SECOND_DENT_APART` (0.6 of the
face) from it, which starts the second (`HitCentres::second`, `damage_at2`,
-128 for none). Everything visible is worked out from those on the client
(`Race/ApexCarDamage.h`).

- **Dents and scuffs** are in the four car parents (`BuildCarDamage` in
  `ApexTrackMaterialGraphs.cpp`): custom primitive data on the body
  (`ApexDamage::Cpd*`: each zone's visual amount, the body's box in its mesh
  frame, a seed, where each zone was hit, whether the car says, and each
  zone's second dent, off the face (`NoSecondDent`, 9) for none) drives a
  world position offset that dents a zone by up to `DamageDentCm`, with
  faceted shading and scuffed paint. The dent is a patch round the hit, as
  wide as the damage is deep (a tap is a dent the size of a hand, a write-off
  reaches the corners), and the same round a second dent; a server without
  the centres dents the whole face as before. The tyre graph's own primitive
  data starts at 18, where the second dents are on the body: a wheel has no
  damage amounts, so its rim draws none. `apexsim.car.DamagePreview` takes
  five, nine or thirteen numbers (the percentages, the centres, the second
  centres). The driver (in the body's frame) and the DRS flap (the box
  written from its hinge) carry the same data, so they dent with the body
  round them. Imported-track
  scenery shares the parents and has the offset switched off
  (`SetEvaluateWorldPositionOffset(false)`).
- **Parts** come off. `[[damage_part]]` boxes split the body GLB at runtime
  (`ApexGlb::SplitByBoxes`, `LoadModelPieces`, `ApexCarContent::LoadBodyPieces`):
  each triangle whose centre lies in a box goes to that part's mesh, drawn on
  the body (`AApexRaceCarActor::SetDamageParts`) until the zone passes
  `detach_pct`, then thrown off as an `AApexCarDebrisActor` (ballistic,
  `ApexDamage::StepDebris`, ground by trace against the track's collision,
  40 s, at most 40 pieces). From half its `detach_pct` a part hangs first
  (`ApexDamage::HangShare`, `UpdateHangingParts`): hinged on the edge nearest
  the body, its loose end droops up to 22° and flutters, straight again
  after a repair. A part holding the DRS hinge takes the flap. The body
  keeps the whole model's bounds, so the cockpit, lights and turntable do
  not move.
- **Glass cracks**: the translucent parent draws a spider web (`kGlassCrack`,
  radial lines and rings) round where the nose or the tail was hit, denser
  and wider the harder the hit, as pale lines that go opaque. A glass parent
  baked before the cracks is baked again by `-run=ApexMaterialBake`.

  ```toml
  [[damage_part]]
  name = "front_wing"
  zone = "front"            # front | rear | left | right
  detach_pct = 30
  min_m = [2.485, -1.012, -0.094]   # forward, left, up: car frame, metres
  max_m = [3.125, 1.012, 0.500]
  ```

  `python tools/generators/cars/damage_parts.py [folder or path ...] [--preview]`
  (`--preview` draws `build/damage_parts/<folder>.png`) writes the shipped
  cars' wing and nose tables from their geometry (`MANUAL_REAR_WING` for what
  its rules miss). Re-run it after a body changes.
- **Smoke, steam and sparks**: `AApexCarEffectsActor` (one per world) draws
  puffs on instanced engine shapes with `M_ApexCarSmoke` (no particle
  assets): oil smoke from 35% engine damage, out of the engine bay where
  `[damage] engine_m` puts it (the front-engined Murcetes-AMD GT3 sets it;
  every other shipped car's engine is behind the driver and smokes from
  the tail), steam from a damaged nose with
  the coolant past 104 °C, sparks on a hit and while scraping a wall.

`apexsim.car.DamagePreview "F,R,L,Rt,E[,aF,aR,aL,aRt]"` (percent, then where
each zone was hit, -1 to 1; empty for telemetry) overrides every car's damage, `apexsim.car.DamageEffects 0` keeps only the
dents; `-ApexExecAfter="22=apexsim.car.DamagePreview 60,50,35,20,85"` does it
mid-run so the parts fly on camera.

## Engine sound (`[sound]`)

The client simulates the engine and its exhaust
([apexsim/docs/game/audio.md](../../apexsim/docs/game/audio.md)) from the table, which the server
ignores: `cylinders` (the note is the firing rate, rpm/60 x cylinders/2),
`crossplane` (V8 burble), `turbo`, `exhaust_length_m` (0.2-2.8), `muffling`
(0 open pipes to 1 road muffler), `pops`, `gear_whine`, `intake_roar` (0..1).
The row's `EngineSound` also carries `[engine]`'s idle, redline and limiter
rpm, which the wire does not. Without the table a car gets its class default
(`ApexEngineAudio::MakeSpec`). `apexsim.audio.RenderCars` writes every car's
scripted drive to `Saved/Audio/<folder>.wav` for tuning:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ../apexsim/game-unreal/ApexSim.uproject \
    -ExecCmds="apexsim.audio.RenderCars,quit" -nullrhi -nosound -unattended
```

## Checking it

- Unreal automation ([apexsim/docs/building.md](../../apexsim/docs/building.md)): `ApexSim.Cars.*`
  (`Glb.*` the reader, `Glb.RepoCars` every repo GLB reads and is long along
  Y; `Toml*` the parser, `TomlRepoCars` every repo car and its class steering
  wheel; `Damage.*`, `Folders`, `CockpitMerge`, `Livery*`),
  `ApexSim.Wheels.*`, `ApexSim.Drs.FlapTransform`, `ApexSim.Cockpit.CarWheel`,
  `ApexSim.Audio.Engine*`.
- Server: the `car_loader` unit tests load every shipped car
  (`shipped_cars_are_geared_for_speeds_they_can_reach`).
- `Test-ApexCars` (`initialize_content.ps1`, `build_release.ps1`): every
  named file exists, folder names are unique.

## Traps

- **A car's own material instance must come from
  `ApexCarContent::OwnMaterialInstance`.** A runtime body's slot instances
  are shared by every car of the model, and `CreateDynamicMaterialInstance`
  returns that shared instance, so one car's lights, livery or ghost tint
  would change them all. The own instance copies the shared one's values onto
  its cooked parent; parenting it to the shared dynamic instance draws it
  untextured (the engine refuses a dynamic parent). The wheels' treaded tint
  follows the same rule.
- Material slot names are the API between the GLB and the client: renaming
  one on export silently disables a livery, light or mirror.
- Hand edits below the liveries' marker, or to a generated table above it,
  are lost on the next `liveries.py`, build or `damage_parts.py` run.
- A car with no catalog row, or whose body will not load, is drawn as the
  player's own: the cars are not cooked, so there is no other mesh to fall
  back on.
