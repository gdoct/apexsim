# Importing Assetto Corsa cars into ApexSim: gaps and process

*Research pass, 2026-09-28, on the `apexsim` repo at `da36d11` (after the
track importer landed) and the AC install on this machine
(`E:\SteamLibrary\steamapps\common\assettocorsa\content\cars`: 196 car
folders, 121 Kunos and 75 mods). The cars section of
`docs/AC_IMPORT_FEASIBILITY.md` (2026-09-26) was written before runtime car
loading, the DRS flap, tyre pressure and the per-wheel contact model; this
document supersedes it for cars. The track importer is
`docs/AC_TRACK_IMPORT.md`; the car importer should be its sibling,
`scripts/ac_car_import.py`, reusing `scripts/ac_import/` (kn5, ini, textures).*

## Verdict

Feasible, and cheaper than the tracks were: nothing about a car needs a new
backend on the server, and the client already builds a car from a GLB at
runtime. An AC car is a kn5 (one file, 200-550 k triangles, 50-75 materials,
30 MB of DDS) plus an encrypted-but-trivial `data.acd` of plain INI files.
Every physics figure ApexSim's `car.toml` takes can be read or derived from
those INIs, the kn5's node tree names exactly the parts ApexSim wants split
off (the four wheels, the steering wheel, the low-detail cockpit, the blur
and damage meshes), and, measured on the SF70H, AC's car frame is the very
frame the client's glTF reader expects, with the same winding: the body
goes over with an identity transform.

Four things stand between "a tool" and "a player runs it on 200 cars":

1. **Done 2026-09-28.** ~~**The folder split the user wants** (`content/cars/default` for the
   shipped cars, `content/cars/custom` for imports) is not supported by the
   client, which looks one folder deep, and is only half supported by the
   server and the scripts (recursive, but flattening by leaf name and with
   no duplicate-id rule). A day's work across both sides, and it should come
   first, because every path below writes into `custom/`.~~ The cars are in
   `content/cars/default/`; server (`car_loader::car_toml_paths`), client
   (`UApexCarContentSubsystem::CarFolders`, the import commandlet), the
   PowerShell scripts (`Get-ApexCarTomls`, `-IncludeCustomCars`) and the
   Python content scripts read `default/` then `custom/`, first id wins.
   Not done from the table below: skipping a car.toml marked
   `imported = "ac"` in `liveries.py`/`initialize_content.ps1` (no such
   marker exists until the importer does), and a `CarConfig` source path.
2. **Done 2026-09-28.** The reader now lays a skinned node out as the
   appendix says, and tests the image magics by prefix. 183 of the 188 car
   folders with a kn5 now read. The other five are refused as
   `EncryptedKn5`: three RSS mods whose texture table does not parse, and
   two with Custom Shaders Patch's `acd.checksum.e` block appended after the
   tree (`test_a_skinned_node_reads_to_the_last_byte`,
   `test_jpeg_and_bmp_textures_are_images_not_encryption`,
   `test_csp_protected_cars_are_refused_as_encrypted`). The original text:
   ~~**The shared kn5 reader cannot read a car.** 67 of 187 readable cars on
   this machine fail on a skinned mesh (the gear knob, the belts: the
   reader lays the node out wrong) and 9 mods are refused as "encrypted"
   because they carry a JPEG texture and the reader's magic check compares
   a three-byte magic against a four-byte slice. Both are one-line fixes in
   `scripts/ac_import/kn5.py`, and the second also affects tracks.~~
3. **Done 2026-09-28** (see "Blockers 3 and 4: what the importer writes" below).
   ~~**Liveries, wheels and the cockpit** each need a small client change
   before an imported car is *good*: liveries are colours and a logo (AC's
   are 2048x2048 skin textures), a car cannot bring its own wheel model (the
   client looks in the shared `wheels/` folder only), and the cockpit rig
   always draws its own steering wheel over the one in the mesh. Each is a
   few days; none blocks a first drivable import.~~
4. **Done as far as the model allows, 2026-09-28** (below). ~~**Physics fidelity**, as before: ApexSim's tyre is one grip scalar,
   linear in load, with no camber, temperature or wear effect; there is no
   differential and no turbo model. An imported car is an ApexSim car with
   AC's numbers, balanced with the grip probe, not AC's handling.~~ Still
   lost: camber, toe, ride-height and yaw aero, speed sensitivity, tyre heat
   and wear, and a differential's yaw effect in a corner without wheelspin.

5. **The tool is built, 2026-09-28**, and imports the Kunos 911 GT3 R
   end to end (see "The tool" below): the server loads it, it passes the
   stability tests, it laps the Silverstone profile inside the shipped
   GT3s, and the game's own readers take its GLBs and car.toml. Not yet
   seen on screen, and no DRS flap split.

Everything else (turbo torque, aero from the wing tables, brakes, gearing,
the DRS flap, the lights, the driver's eye, the collision box) is data work,
and the numbers land in class: the Kunos 911 GT3 R's wing tables sum to a
Cl.A of 2.4 m2 and a Cd.A of 1.0 m2 against the shipped Posh GT3 RS's 2.2
and 0.8, its steering lock comes out at 0.50 rad against 0.51, and its brakes
at 22.9 kN against 15.5 kN.

Rough shape: the folder split 1-2 days, the tool to a first drivable car
about a week, the three client changes 1-2 weeks, balancing and a 12-car
acceptance run 3-5 days.

## What an AC car is (measured on this install)

**Folder.** `<car>/` holds the visual kn5 (`<name>.kn5`, 44 MB for the
911 GT3 R and the SF70H alike), three LOD kn5s (`_lod_B/C/D`, 3 / 0.7 /
0.2 MB, switched by `lods.ini` at 13 / 45 / 220 m), `collider.kn5` (a
100-triangle hull), `data.acd` (the physics: 45-70 INI and LUT files,
encrypted), `skins/<name>/` (the liveries), `animations/*.ksanim`
(steering, gear shift, doors, wipers, the DRS wing), `sfx/<car>.bank` (FMOD,
unusable), `texture/` (flames), `ui/ui_car.json` with `badge.png`, and the
body and tyre shadow PNGs. Of 196 folders, 194 use `data.acd`, none ships
an unpacked `data/`, and two have neither.

**The kn5**, read with the track importer's reader on the 112 cars it can
parse: 71-546 k triangles (median 217 k, two over 400 k), 50-75 materials,
every one of them carrying `WHEEL_LF/RF/LR/RR`, `STEER_HR` and `COCKPIT_HR`
dummies; 168 cars have three LODs, 22 none. The 911 GT3 R: 263 meshes,
298 k triangles, 56 materials, 74 textures (32 MB; 2048x2048 skin, mostly
DXT1/DXT5 and uncompressed RGBA8, and, as on the tracks, most **without mip
chains**). The tree separates what ApexSim wants separated: under each
`WHEEL_xx` the rim, tyre and `RIM_BLUR_xx` (a pre-blurred spin variant),
`SUSP_xx` for the arms and calipers, `DISC_xx` for the brake discs,
`STEER_HR`/`STEER_LR` for the steering wheel, `COCKPIT_HR`/`COCKPIT_LR` for
the interior at two detail levels (the LR one is what AC shows beyond 7 m),
`DOOR_L/R`, `DAMAGE_GLASS_*`, `CINTURE_ON/OFF` (belts with and without a
driver), and lit parts named by `lights.ini`. Skinned meshes (type 3 nodes,
bone-weighted: the shift boot, the belts) appear in about a third of the
cars.

**Frame.** Measured on the SF70H and the 911 GT3 R: `WHEEL_LF` sits at
x = +0.81, `WHEEL_RF` at x = -0.81, the front axle at z = +1.45 and the rear
at z = -2.14; the aero tables put the front wing at z = +2.7. So AC stores a
car as **(x left, y up, z forward)**, the floor at y = 0, the origin between
the axles at the CG's station. That is exactly the frame the client's glTF
reader wants (glTF +X left, +Y up, +Z nose; `AApexRaceCarActor` yaws it
-90 degrees so the nose lands on actor +X), and the authored index order
agrees with the authored vertex normals on 100.0% of the SF70H's 302 731
triangles, so the winding is glTF's counter-clockwise front face as well.
No transform, no mirror, no re-winding: the importer copies positions.

**Materials.** Twenty shader families over the collection, the same
Blinn-Phong set as the tracks: `ksPerPixelMultiMap` (and its `_damage_dirt`,
`_NMDetail`, `_AT` variants: diffuse + normal + `txMaps` (specular /
reflection / emissive packed) + a tiling detail map), `ksPerPixelNM`,
`ksPerPixelReflection` (chrome, glass), `ksPerPixel`, `ksPerPixelAT*`
(alpha-tested decals), `ksTyres`, `ksBrakeDisc`, `ksBrokenGlass`,
`ksWindscreen`, `ksCarPaintSimple` (8 cars), `ksPerPixelNM_UVMult` (54).
Per material: `alphaBlend`, `alphaTested`, `ksSpecular`, `ksSpecularEXP`,
`ksEmissive`, `fresnelC/EXP/MaxLevel`. The paint materials (`EXT_Carpaint`,
`EXT_SKIN_*`, `EXT_Car_Plastic`) all sample `Skin_00.dds`, which is what a
skin replaces.

**Skins.** `skins/<name>/` (2 394 across the collection, median 11 per car,
up to 46) overrides kn5 textures **by file name**: `Skin_00.dds` (the
paint), `Skin_00_MAP.dds`, `EXT_Banner.dds`, sometimes the rims, the glass
tint, the driver suit and helmet, plus `livery.png` (a 64 px badge),
`preview.jpg` and `ui_skin.json`. A skin is a texture set, never a colour.

**Physics** (`data.acd`, decrypted; the key is eight small hashes of the
folder name, see the appendix; the 911 GT3 R and the SF70H below):

| file | what it holds |
|---|---|
| `car.ini` | `TOTALMASS` (1325 / 728 kg), `INERTIA`, `STEER_LOCK` (360 / 180 deg) and `STEER_RATIO` (12.5 / 10), `FUEL` / `MAX_FUEL`, `CONSUMPTION`, `GRAPHICS_OFFSET` and `GRAPHICS_PITCH_ROTATION` (where the mesh sits on the physics), `DRIVEREYES` (the eye in the car frame), `MIRROR_POSITION` |
| `engine.ini` + `power.lut` | `LIMITER`, `MINIMUM` (idle), `INERTIA`, `[COAST_REF]` (engine braking: 100 Nm at 9000 rpm), `power.lut` as rpm -> Nm, and `[TURBO_n]` (`MAX_BOOST`, `WASTEGATE`, `REFERENCE_RPM`, `GAMMA`, lag). **The LUT is the naturally aspirated torque**: the SF70H's peaks at 146 Nm and the turbo's `MAX_BOOST=3.5` multiplies it by 4.5. 85 of 196 cars are turbocharged |
| `drivetrain.ini` | `TYPE` (159 RWD, 12 FWD, 23 AWD/AWD2), `GEAR_R`, `GEAR_1..n` (4-8 gears), `FINAL`, `[DIFFERENTIAL] POWER/COAST/PRELOAD`, shift times, autoclutch, autoblip |
| `suspensions.ini` | `WHEELBASE`, `CG_LOCATION` (the front weight share: 0.405 on the rear-engined 911, 0.456 on the SF70H), `[ARB]`, per axle `TRACK`, `BASEY` (hub height relative to the CG), `SPRING_RATE`, `DAMP_BUMP/REBOUND` (+ fast), bump stops, packers, `STATIC_CAMBER`, `TOE_OUT`, the full wishbone geometry |
| `tyres.ini` | one or more compounds per axle (`[FRONT]`, `[FRONT_1]`...; `COMPOUND_DEFAULT`): `RADIUS`, `WIDTH`, `RIM_RADIUS`, `DY0`/`DX0` (peak lateral / longitudinal friction at the reference load `FZ0`), `LS_EXPY/EXPX` (load sensitivity exponents), `SPEED_SENSITIVITY`, `FLEX`, `CAMBER_GAIN`, `DCAMBER_0/1`, `PRESSURE_IDEAL` (26 psi = 179 kPa), `RATE` (tyre spring), the thermal and wear tables |
| `aero.ini` + `wing_*.lut` | one `[WING_n]` per element (body, front, rear, diffuser; nine on the SF70H) with `CHORD x SPAN`, `POSITION`, `ANGLE`, a Cl and a Cd table by angle of attack and by ride height, `CL_GAIN`/`CD_GAIN`; positive Cl is downforce |
| `brakes.ini` | `MAX_TORQUE` per wheel (3950 Nm), `FRONT_SHARE` (0.68) |
| `electronics.ini` | ABS and TC `PRESENT`/`ACTIVE` with slip targets and LUTs |
| `ers.ini`, `ctrl_ers_*.ini`, `kers_torque.lut` | 16 hybrid cars: kinetic and heat recovery, `MAX_KJ_PER_LAP`, torque by rpm, deployment controllers |
| `drs.ini`, `wing_animations.ini`, `animations/car_wing.ksanim` | the DRS wing is a `[WING_n]` whose animation swings a named node (`Wing` on the SF70H, `MIN=0 MAX=16`); 147 cars have wing animations, most of them movable spoilers, not DRS |
| `colliders.ini` | three to six boxes (`CENTRE`, `SIZE` as width, height, length) |
| `lights.ini` | `[BRAKE_n]` and `[LIGHT_n]`: the mesh node each lamp lights and its colour (`RearLight_0` 450,6,0; `FrontLight_0` 340,350,370 with `FLASH=1`; the rain light `PITLINE=1 SPECIAL=1`) |
| `driver3d.ini`, `mirrors.ini`, `dash_cam.ini`, `cameras.ini` | the steer animation and lock, the mirror nodes (`GEO_Mirror_L/R`, `INT_MIRROR`), cameras |
| `ui_car.json` | `name`, `brand`, `class` (only `street` / `race`), `tags` (`gt3`, `#GTE-GT3`, `lmp1`, `singleseater`, `gp`, `turbo`, `awd`, `vintage`, the country), `specs`, a torque and a power curve for the UI |

Not used by ApexSim, by design: `setup.ini`, `damage.ini`, `ai.ini`,
`sounds.ini`, `digital_instruments.ini`, `flames.ini`, `escmode.ini`, the
suspension animation files, and the FMOD bank.

## What ApexSim has today (the relevant facts)

**Folders.** A car is `content/cars/<folder>/car.toml` plus the GLB it
names, and one shared wheel per class in `content/wheels/<class>.glb`. The
server's `load_cars_recursive` (`server/src/server.rs`) walks every depth
for a file named `car.toml`, so it would find `default/x/car.toml` and
`custom/x/car.toml` today, but a duplicate `id` silently replaces the
earlier car in `read_dir` order (the tracks sort `custom` last and keep the
first id with a warning). The client's `UApexCarContentSubsystem` lists the
immediate subfolders of `-ApexCarsDir=`, `Game/Cars` (a package) or
`content/cars` (the editor) and needs `<sub>/car.toml`, so a `default/`
folder is skipped with everything under it; wheels are found at
`<cars dir>/../wheels/<model>.glb`. `scripts/lib/ApexCars.ps1` finds
`car.toml` recursively but copies each car to `Cars\<leaf name>` and
`Server\content\cars\<leaf name>`, so two folders with one leaf name would
merge. `replay_tools::load_car_folder`, the gearing test in
`car_loader.rs`, `tests/car_stability_test.rs` and `tests/grip_probe_test.rs`
read one level; `scripts/content/cars/liveries.py` and
`scripts/import_cars.ps1` hardcode `content/cars/<folder>`.

**`car.toml` on the server** (`car_loader.rs`; unknown keys are ignored):
required are `id` (a UUID), `name`, `version`, `model` and
`[physics] mass_kg, max_engine_force_n, max_brake_force_n, drag_coefficient,
grip_coefficient, max_steering_angle_rad, wheelbase_m`; everything else
defaults. What the simulation reads: `[physics]` length / width / height
(the collision box and the yaw inertia), the two track widths, one
`wheel_radius_m`, `cog_height_m`, `weight_distribution_front`,
`brake_bias_front`, `frontal_area_m2` (shared by drag and downforce),
`lift_coefficient_front/rear` (negative is downforce), `abs_enabled`,
`traction_control_enabled`, `drs_drag_reduction` and
`drs_rear_downforce_reduction` (class `F1` gets 0.12 / 0.25 by default, any
other class none); `[engine]` idle, redline, max and limiter rpm,
`max_power_w` (only the racing line and the AI envelope; the physics uses
the curve), `friction_torque_nm`, `engine_brake_torque_nm`,
`[[engine.torque_curve]]`; `[transmission]` `gear_ratios` (reverse first,
forward strictly descending) or a `ratio_curve`, `final_drive_ratio`,
`efficiency`; `[drivetrain] layout` (AWD is a fixed 40/60 split);
`[fuel]`; `[hybrid]` (a motor at the crank with regen: real, and enough for
an ERS car); `[suspension]` per-axle springs, bump / rebound dampers,
anti-roll bars and `max_travel_m` (parsed; no shipped car sets it).
**Ignored**: `steering_ratio`, `inertia_kg_m2`, `idle_control_gain`,
`transmission_type`, `shift_time_s`, every `[differential]` key. **Not
settable**: anything on the tyre but `grip_coefficient` (slip optima,
pressures and the 180 kPa optimum, rolling resistance are `TireConfig`
defaults). The tyre's peak force is `grip x load`, the same grip laterally
and longitudinally on a friction ellipse, with a quadratic pressure factor;
no load sensitivity, camber, temperature or wear effect.

**`car.toml` on the client** (`ApexCarToml::Parse`): `id`, `name`, `model`,
`brand`, `class`, `manufacturer_country`, `model_year`, `[physics] mass_kg,
max_steering_angle_rad`, `[wheels]` (model and per-axle station, track,
radius, width; all required if the table is present), `[drs_flap]` (model,
hinge, opening angle), `[engine]` idle / redline / limiter and
`max_power_w`, `[[livery]]` (name, `paint`, `accent`, `metallic`, `logo`),
`[sound]`, `[preview]`, `[cockpit]`. The parser is line-based: arrays must
be on one line, a comment after a table header breaks it, only double-quoted
strings are unquoted. Both sides CRC-32 the car.toml bytes (CR stripped)
and the race director toasts a mismatch; the GLBs are not hashed.

**The GLB reader** (`ApexGlb`, `Cars/ApexGlbReader.cpp`) takes `.glb` or
`.gltf` with external or data-URI buffers and images, glTF 2 only, refuses
any `extensionsRequired` (Draco, meshopt, quantisation, basisu) and sparse
accessors, draws triangle lists only, needs `POSITION` (normals are built
if missing, tangents always recomputed from UV0), reads `TEXCOORD_0` only,
ignores vertex colours, skins, morphs and animations, flattens the node tree
into one mesh with one section per material (slot name = material name,
made unique with `_1`, `_2`). Materials: base colour factor and texture,
metallic and roughness factors, emissive factor, alpha mode and cutoff,
`KHR_materials_clearcoat`. **No normal, metallic-roughness, occlusion or
emissive texture is read** and the four cooked parents under
`/Game/Materials/Car` (opaque, clear coat, masked, translucent, all two
sided) have no inputs for them. Images go through the engine's image
wrapper (PNG, JPEG, BMP, TGA...; not a compressed DDS), become BGRA8 with
box mips only at power-of-two sizes, and stay uncompressed on the GPU: a
2048x2048 map is 21 MB of VRAM with mips. The tracks got a BC1/BC3 streaming
path for exactly this reason (`ApexDdsReader`); the cars have none.

**What the game drives on a car**: the `car_paint`, `car_accent` and
`car_logo` slots (liveries: colour, metallic and a logo texture; nothing can
swap the paint texture), `car_brakelight` and `car_taillight` (emissive
factor from the telemetry; `car_headlight` and `car_rainlight` are named in
`docs/CAR_MODELS.md` but nothing references them: the headlights are two
spot lights placed from the body's bounds), every slot for the ghost tint.
Four copies of the class wheel from `[wheels]`, steered and rolled; a body
may carry its own wheels but they then neither steer nor spin. The DRS flap
from `[drs_flap]`, its own GLB hinged about X at the given point, swung by
the telemetry. The cockpit rig (`AApexCockpitRig`) always builds its own
steering wheel, display and mirror captures at the derived or overridden
points; there is no per-car way to say "the mesh has a wheel". The
turntable frames the body's bounds with `[preview]`. The engine synth reads
`[sound]`, else a class default (F1: turbo V6; LMP / Hypercar: flat-plane
V8; else a crossplane V8).

**Class** decides two things on the server: the AI field
(`game_session::class_field`: every car of the host's class, dealt round
robin; a class of its own races alone) and the default DRS (`F1`). Shipped
classes: `F1`, `Hypercar`, `LMP2`, `GT3`. On screen `ApexCatalog::DisplayClass`
renames the four; any other string shows as itself.

## The folder split: `content/cars/default` and `content/cars/custom`

Mirror the tracks (`content/tracks/{default,custom}`, `TRACK_DIRS`,
`scripts/track_dirs.py`, `scripts/lib/ApexTracks.ps1`,
`.gitignore`'s `content/tracks/custom/*` with the README kept). Concretely:

| where | change |
|---|---|
| `content/cars/` | `git mv` the fourteen car folders into `default/`; add `custom/README.md` (the tracks' one, reworded) and `content/cars/custom/*` + `!README.md` to `.gitignore`. Ids, GLBs and car.toml bytes are unchanged, so no CRC or catalog row moves. `content/wheels/` stays shared |
| `server/src/server.rs` | `load_cars_recursive`: sort entries, `custom` last, first id wins with the tracks' warning; record the source folder on `CarConfig`; a test like `custom_tracks_load_beside_the_default_ones_and_never_replace_them` |
| `server/src/replay_tools.rs`, `car_loader.rs` (gearing test), `tests/car_stability_test.rs`, `tests/grip_probe_test.rs`, `tests/{ai_race_start,racing_line,hotlap}_test.rs` | walk `default/` and `custom/` (a `car_dirs()` helper beside the track one) instead of one level; the hardcoded `content/cars/<x>/car.toml` paths gain `default/` |
| `UApexCarContentSubsystem::CarDirectories` / the scan | a directory whose immediate subfolders have no `car.toml` is descended one level (`default`, `custom`, in that order, so a shipped id wins); `FindWheel` resolves `wheels` against the cars **root**, not the folder scanned (`content/cars/default/../wheels` is wrong); the same in `ApexCarImportCommandlet` |
| `scripts/lib/ApexCars.ps1`, `build_release.ps1`, `build_game_standalone.ps1` | keep `default\` / `custom\` under `Game\Cars` and `Server\content\cars` (the client then finds them by the rule above); add `-IncludeCustomCars` like `-IncludeCustomTracks`, off by default, so a release never ships an import |
| `scripts/content/cars/liveries.py`, `scripts/import_cars.ps1`, `scripts/initialize_content.ps1` | `HERE = content/cars/default`; skip a car.toml carrying `imported = "ac"` |
| `docs/CAR_MODELS.md`, `docs/RUNTIME_CONTENT_LOADING.md`, `CLAUDE.md`, the release README text | the paths |

Rules, as for tracks: a folder name and an `id` must be unique across
both; a custom car reusing a shipped id is skipped with a warning; an
imported car's `id` is a UUID v5 of `ac-car:<folder>[:<skin>]`, so two
players who import the same car get the same id, which is what lets the
CRC check compare their car.toml files.

## AC -> ApexSim: the mapping

### Physics (`car.toml`, read by the server)

| AC | `car.toml` | how, and what is lost |
|---|---|---|
| `car.ini TOTALMASS` | `[physics] mass_kg` | as is (both exclude fuel) |
| `STEER_LOCK / STEER_RATIO` | `max_steering_angle_rad` | `radians(lock / ratio)`: 360/12.5 = 0.503 on the 911 (the shipped GT3: 0.51); `steering_ratio` written for the record, unused |
| `suspensions.ini WHEELBASE`, `CG_LOCATION`, `TRACK` f/r | `wheelbase_m`, `weight_distribution_front`, `track_width_front/rear_m` | as is |
| `tyres.ini RADIUS`, `suspensions.ini BASEY` | `cog_height_m` | `RADIUS - BASEY`, averaged over the axles: 0.42 m on the 911, 0.26 m on the SF70H (plausible; the server default is 0.45) |
| `SPRING_RATE`, `[HEAVE_*] SPRING_RATE`, `DAMP_BUMP`, `DAMP_REBOUND`, `[ARB]` | `[suspension]` | 1:1 (AC's rates are at the wheel, as ApexSim's), plus half an axle's heave spring at each corner (`wheel_spring_rate`: in heave both work; the SF70H is 40 + 120/2 = 100 kN/m, where the corner spring alone sagged it 4.6 cm onto the floor); bump stops, packers, fast damping, geometry, camber, toe: **no home** |
| `car.ini [RIDE]`, `ROD_LENGTH`, the static sag | `[aero] ride_height_front_m / _rear_m` | the static ride heights the ground-height tables are read at (the 911: 60/60 mm; SF70H 37/77; R18 51/61) |
| `power.lut`, `[TURBO_n]` | `[[engine.torque_curve]]` | `torque(rpm) x (1 + boost(rpm))` per point, `boost = min(WASTEGATE, MAX_BOOST x min(rpm / REFERENCE_RPM, 1))` summed over turbos (Kunos's own comment in `engine.ini` makes `GAMMA` the boost's sensitivity to the *pedal*, which is 1 at full throttle, not an rpm exponent; `ctrl_turbo*.ini` controllers are not modelled). Skip this and a turbo car is 30-80% down (the SF70H 78%) |
| `LIMITER`, `MINIMUM` | `rev_limiter_rpm`, `redline_rpm`, `max_rpm`, `idle_rpm` | limiter as is, redline = limiter - 200, max = limiter + 100 (the server clamps rpm to `max_rpm`) |
| `[COAST_REF] TORQUE at RPM`, `INERTIA` | `engine_brake_torque_nm`, `friction_torque_nm` | the coast torque scaled to the redline; inertia written, unused |
| the curve | `max_power_w`, `max_torque_nm`, `max_engine_force_n` | power = max over the curve of `T x rpm x 2pi/60` (the racing line and the AI read it: get it right); force = peak torque x gear 1 x final x efficiency / radius (required, only a fallback) |
| `drivetrain.ini GEAR_R, GEAR_1..n, FINAL` | `[transmission] gear_ratios`, `final_drive_ratio` | reverse first; the server refuses ratios that are not strictly descending, so the tool checks and reports. `CHANGE_UP_TIME` -> `shift_time_s` (unused). The server warns when top gear at the limiter exceeds 1.35x the drag-limited speed: expect the warning on some road cars |
| `TYPE` | `[drivetrain] layout` | `AWD2` -> `AWD`; the split is 40/60 whatever AC says |
| `[DIFFERENTIAL]` | `[differential]` | written; the sim has no differential |
| `brakes.ini MAX_TORQUE, FRONT_SHARE` | `max_brake_force_n`, `brake_bias_front` | `2 x T x (share / r_front + (1 - share) / r_rear)` = 22.9 kN on the 911 (1.76 g at 1325 kg, right for a GT3 with aero); bias as is |
| `aero.ini` wings | `frontal_area_m2`, `drag_coefficient`, `lift_coefficient_front/rear` | evaluate each wing at its `ANGLE` and, when the car has `LUT_GH_CL` tables, at the height it rides at the server's aero reference speed (50 m/s, solved against the server's heave model: `aero_posture`; each wing's height interpolated between the axles by its station): `Cl_i x A_i`, `Cd_i x A_i`; `A` = the BODY wing's chord x span (2.2 m2 on the 911, 1.59 on the SF70H, i.e. AC's frontal area); `Cd = sum(Cd_i A_i) / A`; the downforce is split by each wing's station against the axles (`(z - z_rear) / wheelbase` to the front) and written as negative lift coefficients over the same `A`. The 911: Cl.A 2.45 at 55/49 mm, Cd.A 0.99. From the same tables the server's `[aero]` map (`aero_posture`): `ride_height_sensitivity` the summed tables' slope per cm of mean height around that posture, `rake_sensitivity` the front share's per cm of rake, `stall_height_m` where the sum falls under 90% of its peak on the way down (the 911: 1.8%/cm, 0.8%/cm, 25 mm; Kunos' 2015-17 F1 tables are flat where the cars run, so about 0). A table that loses downforce lower down (the R18 near its diffuser's cliff) clamps the slope at 0 and speaks through the stall height. Lost: yaw sensitivity, the DRS wing's own tables |
| `drs.ini` / the DRS wing | `drs_drag_reduction`, `drs_rear_downforce_reduction` | from the DRS wing's Cd and Cl at its closed and open angle over the totals; **0 when there is no DRS wing**, whatever the class (a `F1`-class 1967 car must not get the class default) |
| `tyres.ini` (the default compound, or `--compound`) | `grip_coefficient`, `wheel_radius_m` | `mu = DY0 x (Fz / FZ0)^(LS_EXPY - 1) x (1 - SPEED_SENSITIVITY x v)` at a reference: the loaded outside wheel at the car's typical corner load (static + downforce at 40 m/s), v = 40 m/s. The 911's DY0 1.668 at FZ0 3768 N gives 1.61 at 4500 N and 1.44 at 40 m/s; the shipped GT3 runs 1.43. Then the grip probe decides (below). Lost: DX0 (a separate longitudinal peak), camber gain, flex, AC's own heat model (the window is carried, see `[tires]` below), wear |
| `PRESSURE_IDEAL` (psi), `WIDTH`, `RADIUS` per axle | (nothing today) | 26 psi is 179 kPa, ApexSim's default optimum is 180: no loss, but `TireConfig` pressures are not TOML keys. Worth exposing (`[tires] optimal_pressure_kpa`) since the garage's clicks are relative to it |
| `electronics.ini ABS/TC PRESENT` | `abs_enabled`, `traction_control_enabled` | as is (a driver's aids override them anyway) |
| `ers.ini`, `kers_torque.lut` | `[hybrid]` | motor torque = the LUT's peak, power from torque x rpm, capacity from `MAX_KJ_PER_LAP` (a deployment budget, the nearest thing to a battery), charge and discharge power from the same. 16 cars |
| `colliders.ini`, the mesh bounds | `length_m`, `width_m`, `height_m` | the visual bounds (AC's colliders are thin slabs): 4.7 x 2.0 x 1.42 m on the 911. These set the OBB and the yaw inertia |
| `car.ini FUEL, MAX_FUEL, CONSUMPTION` | `[fuel]` | `capacity_liters = MAX_FUEL`. `CONSUMPTION` is not carried: consumption follows the power the engine makes at `[fuel] thermal_efficiency` (default 0.30; see CLAUDE.md, Fuel) |
| `ui_car.json` | `name`, `brand`, `class`, `manufacturer_country`, `model_year` (from the name when it has one), `version`, `model` | see "Class" below |

**Balancing.** The tool writes the fitted `grip_coefficient` and the report
prints the Silverstone profile lap it gives (`tests/grip_probe_test.rs`
`silverstone_profile_lap_times`, `PROBE_CLASS=<class>`), so an imported
GT3 can be checked against the shipped three; the acceptance for a class
is "within a few seconds of the shipped cars and of the real pole", not
AC's own lap time. `skidpad_sweep` and `car_stability_test` on the custom
cars catch a car that spins on the straight because a road car's
`weight_distribution_front` of 0.40 meets the server's default springs.

### Visuals (the GLB, read by the client)

| AC | GLB | notes |
|---|---|---|
| LOD A kn5 minus the subtrees below | `<stem>.glb`, identity transform | AC's frame **is** the client's (measured above): positions copied, index order kept. `GRAPHICS_OFFSET` / `GRAPHICS_PITCH_ROTATION` are how AC seats the mesh on its physics; the ApexSim car has no suspension motion, so the tool seats the mesh with the tyres on y = 0 from the wheel dummies and `RADIUS`, and the origin between the axles |
| `WHEEL_LF` subtree (rim, tyre, `DISC_LF`), in the dummy's local frame | `<stem>_wheel.glb` | hub at the origin, axle on X, face on +X: the left front wheel's local frame already. One mesh serves four; the client scales it to each axle's `[wheels]` radius and width, so the SF70H's 405 mm rears come out of its 305 mm fronts stretched (fine at a glance; a `[wheels] rear_model` would be exact). `RIM_BLUR_*` dropped; `SUSP_*` and the calipers stay on the body |
| `STEER_HR`, `STEER_LR` | dropped (default) | the rig draws its own wheel at the derived point; keep them with `--keep-steering-wheel` once the client has a "the mesh has a wheel" switch (below) |
| `COCKPIT_LR`, `DAMAGE_GLASS_*`, `CINTURE_ON`, meshes with `lodIn > 0`, `renderable = false` | dropped | the low-detail interior, the broken glass, the belts drawn over a driver, LOD stand-ins |
| the DRS wing node (the node `wing_animations.ini` animates for the wing `drs.ini` names) | `<stem>_drs.glb` with the node's pivot at the origin; `[drs_flap]` hinge from the pivot, `open_deg` from the animation's rotation | 16 cars carry a real DRS (the F1s, the Formula Hybrids); the 131 other animated wings are spoilers and stay in the body |
| the driver | none | AC's driver is a separate model (`content/driver`), never in the car; ApexSim shows no driver either |
| LODs B-D | none | the client has no LOD switching; 300 k triangles x 20 cars is 6 M on the grid. `--lod B` builds a car from its 50 k LOD for the AI field |

**Materials** (one glTF material per kn5 material, renamed where the game
drives a slot):

- `txDiffuse` -> `baseColorTexture` as PNG inside the GLB (DDS decoded with
  Pillow, downscaled over `--max-texture`, default 2048 for the skin and
  1024 for the rest; the client builds its own mips). Only textures a kept
  material uses are written. `txNormal`, `txMaps`, `txDetail` are dropped:
  the car parents cannot take them (see gap 6).
- `alphaBlend` -> `BLEND`, `alphaTested` -> `MASK` (cutoff 0.5), else
  `OPAQUE`; `roughness` from `ksSpecularEXP` by the track importer's rule;
  `metallic` 1.0 for `ksPerPixelReflection` with a high `fresnelMaxLevel`
  (chrome, the mirrors), 0 otherwise.
- Paint: the materials sampling the skin texture (`EXT_Carpaint`,
  `EXT_SKIN_*`, `EXT_Car_Plastic`...) get `KHR_materials_clearcoat`
  (factor 1, roughness 0.1) so the client picks the clear-coat parent, and
  are named `car_skin`, `car_skin_1`, ... **not** `car_paint`: the paint is
  a texture, and a `car_paint` slot would let a colour livery multiply into
  it.
- Glass: `ksWindscreen` and the `*Glass*` blend materials -> `car_glass`,
  translucent (the game does nothing with the slot, but the parent choice
  matters).
- Lights: the mesh nodes `lights.ini` lists get their own material copy
  named `car_brakelight` (`[BRAKE_n]`), `car_taillight` (a rear `[LIGHT_n]`
  without `SPECIAL`), `car_headlight` (a front one), `car_rainlight`
  (`SPECIAL=1 PITLINE=1`), with `emissiveFactor` from the `COLOR` normalised:
  the game switches the brake and tail slots by their authored colour.
  Headlights stay spot lights.
- Tyres: `ksTyres` -> `wheel_tyre`; the rim -> `wheel_rim`; the disc ->
  `wheel_brake` (the wheel GLB's slots).
- Everything else keeps its AC name (`INT_Belts`, `EXT_RIM`, ...), which
  is fine: a slot the game does not drive draws as authored.

### Liveries

Livery 0 is "the model as authored", so the first import is the kn5's own
`Skin_00.dds` (usually the first skin's) and no `[[livery]]` tables. Two
ways to more:

- **Now, without a client change:** `--skin <name>` picks which skin's
  textures the GLB embeds, and `--all-skins` writes one car folder per skin
  (`<Stem>_<skin>/`, its own id and name suffix). A 46-skin car becomes 46
  entries in the car select, which is honest but ugly, and the class field
  fills up with clones.
- **Texture liveries (client, 2-3 days):** a `[[livery]]` gains
  `skin = "skins/<name>/Skin_00.png"` (and optionally a list of
  `slot = "file.png"` overrides, since a skin may also replace the rims or
  the banner), the row carries it as a runtime path, and
  `ApexLivery::Apply` swaps `BaseColorTexture` on every `car_skin*` slot
  the way it swaps `car_logo` today. The importer then writes one table
  per skin with the skin's `ui_skin.json` name, `preview.jpg` beside it, and
  the garage's Left / Right steps through AC's skins. `liveries.py` must
  leave an imported car alone.

### Cockpit and eye

- `[cockpit] style = "closed"` or `"open"` from `car.ini` (a `singleseater`
  tag or no roof over `DRIVEREYES`), `eye_cm` from `DRIVEREYES` in the actor
  frame (`+X nose = z x 100`, `+Y right = -x x 100`, `+Z up = y x 100`),
  `wheel_cm` from the `STEER_HR` dummy, `mirror_*_cm` from the
  `mirrors.ini` nodes' positions: every point the rig derives from the
  bounds today comes measured from the real car. This alone puts the eye
  where AC's driver sits.
- The interior is real (`COCKPIT_HR`: a dash, a display, seats, belts) and
  drawn as part of the body, as the generated cars' cabins are. The rig's
  synthesized wheel and display sit a few centimetres from AC's, so the
  importer drops `STEER_HR` by default. The right fix is a
  `[cockpit] rig = "full" | "wheel" | "none"` switch on the row (a day):
  `wheel` keeps the rig's display and mirrors and hides its rim, and the
  importer keeps `STEER_HR` as a `car_steering_wheel` slot; rotating it with
  the steering telemetry is another half day.
- **Mirrors are the car's own glass.** AC renders one low-resolution rear
  view and maps it through each glass's UVs; imported as-is the glass was
  chrome reflecting the sky, with the rig's own mirror faces drawn over or
  beside it (two of every mirror). The importer now cuts the `mirrors.ini`
  meshes into pieces (connected triangles, joined within 3 cm: Kunos often
  model all the glass as one mesh, the 787B's `MIRROR` is both door
  mirrors), classes each piece centre (within 20 cm of the centreline) or
  left / right, and writes the largest of each as its own material slot,
  `car_mirror_centre` / `_left` / `_right` (`model.read_mirrors`,
  `mirror_parts`), with new UVs laid flat across the glass as the driver
  faces it: `u` from the driver's right to their left, `v` top down, which
  is the rig's rear-facing capture read as a mirror reads. The car.toml
  gets `mirror_*_cm` (the piece's centre) and `mirror_*_size_cm` (width,
  height). Outside the cockpit the slot is chrome. The cockpit rig
  (`AApexCockpitRig::BindCarGlass`) finds those slots on the followed
  car's body, sizes each capture to its glass (24 px/cm at high quality)
  and paints it onto the glass with the engine's opaque widget
  pass-through, the same material and brightness as its faces; it draws
  no face of its own where the car has glass, and none where a car with
  glass has no such mirror (the 787B has no interior mirror). The
  captures clip everything nearer than 25 cm (the housing behind the
  glass), and the centre capture hides the car's own body and wheels as
  the virtual mirror does, so it shows the road, not the cabin's rear
  bulkhead. Re-import with `--force` to get the slots.

### Sound

AC's engine sound is an FMOD bank and no data file says how many cylinders
the engine has. The tool writes a `[sound]` table from what it can see:
`turbo` from `[TURBO_n]`, `idle`/`redline`/`limiter` from `engine.ini`,
`cylinders` and `crossplane` from a small table keyed on brand and name
(`V8`, `V10`, `V12`, `flat-six`, `rotary`... appear in most `ui_car.json`
descriptions and names), `--cylinders N` to override, and the class default
otherwise. `apexsim.audio.RenderCars` is the way to judge the result.

### Class and names

`ui_car.json`'s `class` is only `street` or `race`; the tags carry the
series. Mapping (`--class` overrides): `gt3` / `#GTE-GT3` -> `GT3`, `lmp2`
-> `LMP2`, `lmp1` / `#LMP1` / `LMH` / `#Hypercars R` -> `Hypercar`,
`singleseater` + `gp` with a DRS wing -> `F1`; everything else keeps a
readable AC group as its class (`Street`, `Vintage`, `GT4`, `Touring`,
`Drift`, `Formula`), which means such a car races the AI field of its own
group, made of the other imports in it. The mapped classes join the shipped
fields, which is what the user wants from a Kunos GT3 next to the Posh.
The DRS default never comes from the class (see the aero row).

Names: the shipped cars are parodies because the real names are
trademarks that the product must not show (`docs/CAR_MODELS.md`,
CLAUDE.md "No trademarks on screen"). An imported car is the player's own
local content and keeps AC's real `name` and `brand`, exactly as an
imported track keeps its `ui_track.json` name; nothing in a release build
carries it.

## Blockers 3 and 4: what the importer writes

**Client (blocker 3).**
- *Skins.* Name AC's paint materials `car_skin`, `car_skin_1`, ... in the
  GLB. Livery 0 is the GLB's own embedded `Skin_00`.
- For each AC skin, write its `Skin_00.dds` as PNG (JPEG also works) to
  `skins/<skin>/Skin_00.png`, and a `[[livery]]` with:
  - `name` from `ui_skin.json`
  - `skin = "skins/<skin>/Skin_00.png"`
  - no `paint`
  - `textures = ["<slot>=skins/<skin>/<file>.png", ...]` on one line, for
    any other texture the skin replaces. The slot is the GLB material name,
    and rim slots in the wheel GLB count too, since the livery is applied
    to the wheels as well.
  - `preview = "skins/<skin>/preview.jpg"`. The row carries it, but nothing
    draws it yet.
- `ApexLivery::Apply` sets `BaseColorTexture` on the named slots through
  `OwnMaterialInstance`, and livery 0 restores them.
- *Wheels.* Write `wheels/front.glb` in the car folder, and
  `wheels/rear.glb` when the rears differ, in the class wheel's frame:
  - hub at the origin, axle along glTF X, face on +X
  - slots `wheel_tyre`, `wheel_rim` and `wheel_brake`
  - write `[wheels] model = "wheels/front.glb"` and
    `rear_model = "wheels/rear.glb"`
  A `model` that ends in `.glb` or contains a `/` is looked up in the car
  folder only (`ApexCarToml::IsCarLocalWheel`); anything else is a class
  wheel, as before. `Test-ApexCars` and `Copy-ApexRuntimeCars` check and
  ship these files.
- *Steering wheel.* Keep `STEER_HR` out of the body, and write its meshes to
  `steering_wheel.glb` in the dummy's local frame with the tilt removed:
  hub at the origin, the rim in glTF XY (+Y up, +X the car's left), the
  column along +Z toward the nose, the wheel straight. **Checked on the 911
  GT3 R:** `STEER_HR`'s local +Z is the column, pointing at the nose and
  15.6° down; its local Y is the rim's up. Its local frame is therefore
  the file's frame as it stands. Then write in `[cockpit]`:
  - `steering_wheel_model = "steering_wheel.glb"`
  - `wheel_cm`: `STEER_HR`'s origin in the actor frame
  - `wheel_rake_deg`: the pitch that turns the nose axis onto the column,
    in the rig's convention (Unreal pitch: positive lifts the column's
    forward end, tipping the top of the rim toward the driver). A real
    column runs forward and *down*, so a real car's figure is negative:
    -15.58 on the 911. Mounted that way, the file lands within 8 mm (95th
    percentile) of AC's own geometry; with the opposite sign it is 8-10 cm
    off.
  - `wheel_lock_deg`: `STEER_LOCK` itself (or `driver3d.ini`'s
    `[STEER_ANIMATION] LOCK`), not half of it: AC's `car.ini` documents
    it as "steer lock from center to right", which is the rim turn one way
    that the key means
  - `rig_dash = false` when the interior has its own display
  - or `rig_wheel = false` when a static wheel stays in the body

  The rig draws the model in place of its own rim and rolls it with the
  steering. The mirrors are unchanged.

**Server (blocker 4).** Every key is optional, and its default is the sim
as it was: the 14 shipped cars simulate bit-for-bit, and their Silverstone
profile laps are unchanged. `MODDING.md` lists the ranges.
- `[tires]`:
  - `optimal_pressure_kpa` = `PRESSURE_IDEAL` x 6.895. `pressure_front_kpa`
    and `pressure_rear_kpa` default to the optimum, and the garage's clicks
    move them.
  - `load_sensitivity_front` / `_rear` = each axle's `LS_EXPY`.
  - `reference_load_front_n` / `_rear_n` = `FZ0`. The default is the
    axle's static wheel load.
  - `front_grip_scale` / `rear_grip_scale` = each axle's `DY0` over the
    `grip_coefficient` written.
  - `longitudinal_grip_factor` = `DX0/DY0`, one value for the car.
  - `optimal_temperature_c`, `temperature_window_c`,
    `temperature_grip_falloff` from the front compound's `[THERMAL_FRONT]`
    `PERFORMANCE_CURVE` (`physics.thermal_window`): the middle and half
    the plateau within half a percent of the peak, and the average slope
    over the 30 degrees past each edge (the cold one divided by the 0.6 the
    server charges cold). AC's own thermal model (`FRICTION_K`,
    `SURFACE_TRANSFER`...) is not carried: the server heats the tyre its
    own way, so an imported car's tyres may run a little off AC's window
    (the 911 runs 86/95 °C front/rear at AI pace against 88 +- 8).
  - `blanket_temperature_c = 70` for an F1-class car from 1990 on (or with
    DRS when the year is unknown): a rule, not an AC figure.

  The sim's mu is `grip x scale x (Fz/FZ0)^(LS-1)`. The racing line and the
  AI plan at the load the car carries at 40 m/s. Keep fitting
  `grip_coefficient` with the grip probe.
- `[aero]` (only for a car whose wings have `LUT_GH_CL` tables): the static
  ride heights and the map above.
- `[drivetrain] awd_front_share`: AC's AWD front share. The default is 0.4.
- `[differential]`:
  - write `simulated = true` (opt-in: the shipped cars carry the table but
    were tuned without it)
  - `differential_type = "ClutchLSD"`, or `"Open"` / `"Locked"`
  - `lock_power = POWER`, `lock_coast = COAST`, `preload_nm = PRELOAD`

  The model is a torque-bias split: the gripping wheel may take what the
  other can transmit, plus the preload, plus the lock times the axle
  torque. Left and right wheels roll at one speed, so there is no yaw from
  the diff.
- `[engine.turbo]`: keep baking the boost into the torque curve, and add:
  - `boosted_share = boost/(1+boost)` at the peak-torque rpm
  - `lag_up_s` / `lag_down_s` from `LAG_UP` / `LAG_DN`, which are per-tick
    factors at 333 Hz: `tau = -(1/333)/ln(LAG)`, clamped to 0-5 s

  Only a tip-in is delayed. A steady pedal gets the curve as written.

## The gaps, ranked

1. **Folder split** (above). Both sides and the scripts; do it first.
   1-2 days.
2. **Reader fixes in `scripts/ac_import/kn5.py`**: (a) a type 3 (skinned)
   node carries the three flag bytes *before* its bone table and has no
   bounding sphere or `renderable` byte after `lodOut` (verified: the
   Abarth 500 parses to its last byte with that layout and no other);
   (b) `_IMAGE_MAGICS` holds a three-byte JPEG magic and a two-byte BMP one
   that a four-byte slice can never equal, so a kn5 with any JPEG texture
   is refused as encrypted: 9 mods here, and the same check guards the
   tracks. Half a day with tests (a synthetic skinned kn5 through
   `write_kn5`, a JPEG texture).
3. **`data.acd`**: every Kunos and nearly every mod car needs it read. The
   cipher is an additive byte key derived from the folder name (the
   appendix has the eight parts; one of them differs from the published
   description and was recovered from the ciphertext). This is Kunos's own
   packaging of every car, read routinely by Content Manager and every AC
   tool, and it is not the CSP encryption the track importer refuses; the
   legal position is the same as reading the kn5. A day, with a test that
   the key decrypts a checked-in synthetic `.acd` and that a car whose
   INIs come out as noise is refused.
4. **Per-car wheel model.** The client only finds a wheel in the shared
   `wheels/` folder. Either the importer writes `content/wheels/<Stem>.glb`
   (works today; litters the shared folder, and `Copy-ApexRuntimeCars`
   already copies referenced wheels) or `[wheels] model` may name a `.glb`
   relative to the car folder (`FindWheel`, the packaging copy, `Test-ApexCars`:
   half a day). Recommend the second.
5. **Texture liveries** (above): the one client change that turns "a car
   with one skin" into "the car with its 46 skins". 2-3 days.
6. **Material inputs**: the car parents take no normal map, and AC cars
   lean on `txNormal` (the 911 has it on 30 of 56 materials) and `txMaps`
   for paint flake, carbon weave and interior detail. Without them the
   interior is flat and the paint is a plain clear coat: acceptable for a
   first pass, not for "looks like AC". Adding `NormalTexture` (and an
   ORM-style `txMaps` decode) to the four parents, the reader and the
   importer is about a week. Independent of everything else.
7. **Texture memory**: 74 decoded BGRA8 maps per car is 100-200 MB of
   VRAM, times a 20-car grid. Mitigations in order: downscale everything
   but the skin to 1024 (`--max-texture`), drop textures no kept material
   samples (most of the `_MAP`, `_NM` and blur maps go anyway), and, the
   real fix, let the GLB reader accept a BC1/BC3 DDS image (an `image/dds`
   mime type, decoded by the tracks' `ApexDdsReader` into a compressed
   transient texture): 2-3 days, and the importer then re-encodes the way
   `ac_import` does for tracks.
8. **Physics ceiling**: as in the feasibility study. One grip scalar, no
   load sensitivity, no differential, a fixed AWD split, no turbo lag, no
   camber. A Kunos setup means nothing; the tool's fit plus the grip probe
   is the process. Exposing tyre pressures and a load-sensitivity exponent
   on `TireConfig` (the plumbing exists in `data.rs`) would let an import
   keep `LS_EXPY`; a few days on the server, and the shipped cars are
   bit-identical with the defaults.
9. **The cockpit rig switch** (above): a day; until then the mesh's
   steering wheel is dropped.
10. **No LODs, no damage, no wipers, no suspension animation, no driver**:
    not gaps in the importer, they are things ApexSim does not do for its
    own cars either.

## The tool: `scripts/ac_car_import.py` (built 2026-09-28)

```powershell
python scripts/ac_car_import.py "E:\SteamLibrary\...\content\cars\ks_porsche_911_gt3_r_2016"
python scripts/ac_car_import.py <folder> --list            # skins, compounds, LODs, what would be dropped
python scripts/ac_car_import.py --all <ac>\content\cars    # a collection, one line each
#   --stem, --display-name, --class, --skin NAME, --compound N, --lod A|B,
#   --max-texture N (1024), --max-skin-texture N (2048), --cylinders N,
#   --keep-steering-wheel, --force, --dry-run
python -m unittest discover -s scripts/ac_car_import/tests   # the importer's tests
# then restart the server, and restart the game or run apexsim.car.Rescan
```

**What it writes**, in `content/cars/custom/<Stem>/` (the stem defaults
to CamelCase of the folder, `KsPorsche911Gt3R2016`, `--stem` to rename; a
folder name or id that exists under `default/` is refused, and an existing
import is replaced only with `--force`):

| file | contents |
|---|---|
| `car.toml` | everything above, `imported = "ac"` at the top, an `id` that is a UUID v5 of the AC folder, a `[source]` table (folder, kn5, data, baked skin, tool version); every physics figure carries a comment naming the AC key it came from |
| `<Stem>.glb` | the body, seated (tyres on y = 0, origin midway between the axles), textures embedded as JPEG (opaque) or PNG (sampled by a blended or masked material) |
| `wheels/front.glb`, `wheels/rear.glb` | `WHEEL_LF` / `WHEEL_LR` with their discs, in the dummy's frame; slots `wheel_tyre`, `wheel_rim`, `wheel_brake` |
| `steering_wheel.glb` | `STEER_HR` in its own frame (see "Blockers 3 and 4") |
| `skins/<skin>/Skin_00.jpg`, other slot textures, `preview.jpg`; `skins/_kn5/` | one `[[livery]]` per AC skin except the baked one. A texture that some skin overrides and this one does not comes from the kn5 (`_kn5/`), so stepping between liveries never leaves the previous one's banner behind. Identical files are written once |
| `<Stem>.import.json` | the report: source files with CRCs, the physics as written, the fit (compound, reference speed, the tyre's mu at a corner load, every wing's Cl/Cd, static ride heights, the boost), the seat offset, the parts dropped and why, the materials and their slots, the lamps, the skins, the checks, the warnings and the command that rebuilds it |

Every file is byte-identical on a re-run (the 911: 21 files, 12 MB of
GLBs, 3.4 MB of skins, under 2 s).

**Package** (`scripts/ac_car_import/`, beside the track importer's and
sharing its `kn5.py`, `ini.py` and texture decoder): `acd.py` (the key and
the decryption; a wrong key, a renamed folder for instance, is refused
because `car.ini` comes out as noise), `data.py` (INIs, LUTs,
`ui_car.json`, skins), `physics.py` (the mapping), `model.py` (split, seat,
lamps, materials, the GLBs), `glb.py` (a small deterministic GLB writer:
positions, normals, UV0, uint32 indices, one primitive per material, clear
coat, embedded images), `liveries.py`, `toml_out.py` (laid out for the
client's line reader: no comment after a header, arrays on one line) and
`cli.py`. `kn5.py` gained `path` on every mesh and dummy (its ancestors'
names), which is what parts are selected by.

Decisions the build made that the plan left open:

- **Grip.** The sim now applies AC's load sensitivity itself, so
  `grip_coefficient` is the axles' mean `DY0 x (1 - SPEED_SENSITIVITY x
  40 m/s)` at each tyre's `FZ0`, and `front_grip_scale` / `rear_grip_scale`
  carry only the balance (each times the grip is that axle's own figure).
  The 911 GT3 R: 1.494, and 1.47 / 1.37 at a loaded outside wheel in a
  40 m/s corner. Its Silverstone profile lap is 2:04.49 against the shipped
  GT3s' 2:04.85-2:06.08, so no refit was needed.
- **Aero heights.** A wing's height table is read at its axle's static ride
  height: the design CG height at the pickup point, plus `ROD_LENGTH`, less
  the spring's and the tyre's sag under the axle load (the 911: 60 mm at
  both ends). Downforce is split to the axles by moment, so a wing behind
  the rear axle takes a little off the front.
- **Frames.** `DRIVEREYES` is in the *model's* frame (that puts the 911's
  eye 0.96 m up and 51 cm behind the rim; read in the physics frame it is
  in the roof); the aero `POSITION`s are from the CG; the model sits at
  `physics - GRAPHICS_OFFSET`.
- **Lamps.** `lights.ini` nodes become `car_brakelight` (a `[BRAKE_n]`
  behind the rear axle), `car_taillight`, `car_headlight` (ahead of the
  front axle) and `car_rainlight` (`SPECIAL`, written dark: nothing
  switches it yet), one material per slot since the game finds them by
  name. Display LEDs between the axles and glass lit from behind (a
  blended material) keep their own.
- **Height.** The collision box's height is the highest 2 cm slice of the
  body at least 30 cm across, so the 911's aerial (35 cm over the roof)
  does not make it a 1.54 m car.
- **Open or closed** comes from the tags (single seaters) or a ray cast up
  from the eye through the body.
- **Sound** comes from the name and description (`V8`, `flat-six`, a
  911...), else `--cylinders`; `turbo` from `[TURBO_n]`.

**Checks** (in the report and printed; a failed fatal one is exit code
1): the body is long along glTF Z, it sits above the ground, the four
wheels were found and measured, every server-required key is set and
positive, and the torque curve is sorted. The triangle (400 k) and decoded
texture (300 MB) budgets are warnings, and so is an unnamed baked skin.

**Verification on the 911 GT3 R.** `server/tests/imported_car_test.rs`
(`cargo test --release --test imported_car_test -- --ignored --nocapture`)
loads every import through the server's loader and bounds its figures;
`car_stability_test` has `imported_*` twins of its two tests (opt-in the
same way); the grip probe's profile laps already walk `custom/`. On the
client, `ApexSim.Cars.Glb.RepoCars` reads every GLB under `content/cars`
(a steering wheel is exempt from the body's long-along-Y rule) and
`ApexSim.Cars.TomlRepoCars` parses every car.toml with the game's own
parser and checks that each file it names is there. `Test-ApexCars`
passes it for packaging with `-IncludeCustomCars`. **Not yet looked at in
the game:** the car on the turntable, in the cockpit, on the track, and a
night race for the lamps.

**The collection, dry run** (`--all --dry-run`, read-only): 189 of the
196 folders import and pass every fatal check; the other seven are the
two Kunos Ferraris with no data at all and five mods refused as encrypted
(three RSS, two with CSP's appended block). A non-fatal `power` check
compares the curve (plus a hybrid's motor) with `ui_car.json`'s own
figure: 182 of 188 land within 0.75-1.33 of it. Three bugs the collection
found and the tests now pin: a negative `STEER_RATIO` (the Audi S1's, which
turns the rim the other way in AC), a key on a section's header line
(`[REAR]NAME=...`, fixed in the shared `ac_import/ini.py`, so tracks read
it too), and zero corner springs with a heave spring (the Tatuus FA01:
half the heave rate per wheel). And one mapping error: the 488 GTB's boost
is ten turbos, two switched on per gear by `ctrl_turbo<n>.ini`
controllers (an rpm LUT added, a gear LUT multiplied); summed, they made
1406 kW. The controllers are now evaluated at full throttle as the
wastegate, per gear, and the curve takes the gear with the most boost
(424 kW against the UI's 492; the SF70H's engine fell from 757 to 531 kW
the same way). Controllers on other inputs fall back to `WASTEGATE`, with
a warning.

The six below the band: the 919 Hybrid, 2015 and 2016 (its `ers.ini` curve is the rear
motor's, all zeros; the real one is on the front axle, which ApexSim's
hybrid cannot drive, so no hybrid is written), the R18 e-tron the same
way, the TS040 (the UI's 746 kW is a peak system figure), the LaFerrari
(AC's older `kers.ini`, not read), and the Maserati 250F.

**Not done yet:** splitting a DRS flap off the body (a DRS car's aero gets
the `drs_*` reductions, but its wing is drawn closed, and the report says
so); `--lod B` is untested; ERS deployment strategies and the aero
controllers are read as their authored figures (each warned); front-axle
ERS and `kers.ini` are not modelled; and no car but the 911 has been
written for real.

**Pipeline integration**: `imported = "ac"` in car.toml is the car's
`imported` marker. `liveries.py` only walks `default/`, `Test-ApexCars`
checks an import's files like any other, `build_release.ps1` leaves
`custom/` out unless `-IncludeCustomCars`, and `initialize_content.ps1`
has nothing to bake for a car.

## Phases

- **Phase 0 (1-2 days): the folder split.** Server, client, scripts,
  tests, docs, gitignore, README. Ship it alone; nothing else depends on
  AC.
- **Phase 1 (about a week): a drivable car.** Reader fixes, `data.acd`,
  the physics mapping with its tests, the body and wheel GLBs with AC's
  diffuse textures, one skin, lights, the eye and mirrors, the DRS flap,
  the report. The wheel goes to `content/wheels/<Stem>.glb` until gap 4
  lands. Check on the 911 GT3 R (closed, RWD, NA), the SF70H (open, turbo,
  ERS, DRS, skinned belts), a Kunos road car with a JPEG-textured mod
  beside it, and the grip probe on each.
- **Phase 2 (1-2 weeks): make it good.** Per-car wheel path, texture
  liveries, the cockpit rig switch with the mesh's steering wheel, DDS
  textures in the GLB, normal maps on the car parents.
- **Phase 3 (3-5 days): acceptance.** `--all` over the install (all 196
  expected to import once the reader is fixed; the report says which do
  not and why), the twelve-car run (three GT3s, an LMP1, three F1s, three
  road cars, two mods) through the profile lap times, the stability tests
  and an on-screen look at each: turntable, cockpit, chase, a night race
  for the lights. Frame time on a full grid of imports on the reference
  machine.

## Legal note

As for tracks (`docs/AC_IMPORT_FEASIBILITY.md`): a tool the player runs
on content they own, output that never leaves their machine, nothing
bundled. Two car-specific points. `data.acd` is Kunos's packaging of every
car and its decoding is the same routine act as reading a kn5; a kn5
encrypted by Custom Shaders Patch (none on this install; common in paid
mods) is refused, as it is for tracks. And the real car and brand names an
import carries are the reason `custom/` never ships: `-IncludeCustomCars`
exists for the player's own package, not a release.

## Appendix: format facts

**`data.acd`.** Optional 8-byte header (`i32 -1111`, `i32 version`), then
records `i32 name_len, name, i32 n, n x 4 bytes` where only every fourth
byte is payload: `plain[i] = (raw[4i] - key[i mod len(key)]) mod 256`. The
key is a string of eight decimal numbers joined by `-`, each a hash of the
lower-cased folder name `s` (`c[i]` its character codes, 32-bit wrapping
arithmetic, C division, each part `& 0xff`): (1) the sum of all codes;
(2) `k = k*c[i] - c[i+1]` for `i = 0, 2, 4...` while `i < n-1`; (3)
`k = k*c[i] / (c[i+1] + 27) + (-27 - c[i-1])` for `i = 1, 4, 7...` while
`i < n-3`; (4) `0x1683 - sum(c[1:])`; (5) `k = (c[i] + 15) * k * (c[i-1] +
15) + 22` from `k = 0x42` for `i = 1, 5, 9...` while `i < n-4`; (6) `0x65 -
sum(c[0::2])` over `i < n-2`; (7) **`k = k mod c[i]`** from `k = 0xab` for
`i = 0, 2, 4...` while `i < n-2` (the published description adds `0xab`
inside the modulus, which leaves the part at 171 for every name; the
ciphertext of `ks_porsche_911_gt3_r_2016` recovers 15, which this gives);
(8) `k = k / c[i] + c[i+1]` from `k = 0xab` for `i = 0..n-2`. The result for
that folder is `145-191-144-93-26-0-15-55`. A wrong key shows at once: the
record names are stored in clear, the contents come out as noise.

**kn5 skinned mesh (node type 3).** `u8 x3` flags, `i32 bones` then per
bone `str name, 64 bytes` (a 4x4), `i32 nverts` x 76 bytes (the 44-byte
vertex plus four bone weights and four bone indices as floats),
`i32 nidx` x `u16`, `i32 material`, `i32 layer`, `f32 lodIn`, `f32 lodOut`,
and **no** bounding sphere or `renderable` byte. The track reader assumes
the type 2 layout for both.

**`.ksanim`.** `i32 version` (2 on this install), `i32 nodes`, per node
`str name` and `i32 frames` of keyed transforms; only the node names
matter to the importer (which node the DRS animation moves), and the
opening angle is better taken from `wing_animations.ini`'s `MAX` and the
wing's table than decoded from the frames.

**AC frame, in one line.** (x, y, z) = (left, up, forward), metres, floor
at y = 0, counter-clockwise front faces; the client's glTF frame with no
change. The track importer's `(X, Y, Z) = (x, -z, y)` is the same data seen
from the server's Z-up frame.

**ApexSim files an importer touches.** `scripts/ac_import/kn5.py` (the two
fixes), `server/src/server.rs` (`load_cars_recursive`),
`server/src/car_loader.rs` and `data.rs` (tyre keys, if exposed),
`server/src/replay_tools.rs`, `server/tests/*` (car paths),
`game-unreal/Source/ApexSim/Private/Cars/ApexCarContentSubsystem.cpp`
(the scan, `FindWheel`), `ApexCarToml.cpp` (`skin`, `rig`),
`ApexGlbReader.cpp` (DDS images, normal textures),
`Private/Race/ApexCarLivery.cpp` (texture liveries), `ApexCockpitRig.cpp`
(the rig switch), `ApexTrackEditor/Private/ApexTrackMaterialGraphs.cpp`
(the car parents' inputs), `scripts/lib/ApexCars.ps1`,
`scripts/build_release.ps1`, `scripts/build_game_standalone.ps1`,
`scripts/content/cars/liveries.py`, `scripts/import_cars.ps1`,
`.gitignore`, `docs/CAR_MODELS.md`, `docs/RUNTIME_CONTENT_LOADING.md`,
`CLAUDE.md`.
