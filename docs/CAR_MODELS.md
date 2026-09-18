# Generated car models

Seven cars are generated from two Blender scripts in `content/cars/`:

| script | variants | folder / stem |
| --- | --- | --- |
| `build_lmp2.py` | `yotota`, `posh`, `fugazzi`, `jeanetti` | `yotota-lmp2/yotota_lmp2`, `posh-lmp2/posh_lmp2`, `fugazzi-lmp2/fugazzi_lmp2`, `jeanetti-lmp2/jeanetti_lmp2` |
| `build_gt3.py` | `posh`, `limbotiti`, `murcetes` | `posh-gt3rs/posh_gt3rs`, `limbotiti-caravan-gt3/limbotiti_caravan`, `murcetes-amd-gt3/murcetes_amd_gt3` |

Run inside Blender with `VARIANT = "<name>"` set first, then
`exec(open(r"D:\apexsim\content\cars\build_<class>.py").read())`. Each run
resets the scene, builds body → wheel arches → apertures → parts → joined mesh
(saving the `.blend` after every stage) and exports `<stem>.glb` beside
`car.toml`. Set `APEX_EXPORT=0` in the environment to skip the export while
iterating on shape.

Both scripts hold only shape and livery data; everything mechanical lives in
`content/cars/carlib.py`, so a fix lands on all seven cars at once.

### `carlib.py`

| piece | what it is for |
| --- | --- |
| `car_materials()` | the standard slot set, with the names the client drives |
| `Loft` | the section loft. `hard=(j, ...)` marks control indices as creases: the section is sampled either side of them, so a shoulder line or fender crown stays an edge instead of melting. `groove()` and `recess()` cut shut lines, ducts and vents *into* the shell by inserting their own stations and displacing them along the true surface normal |
| `fender_bump()` | raises the flank over each axle. Not decoration: if the arch opening tops out above the fender peak the section has no outer crossing at that height and anything sampling the skin there tears |
| `arch()` | cuts the wheel opening following the tyre at `gap` clearance. The shell is closed, so the boolean gives a watertight well for free - the cutter carries the liner slot and its own surface becomes the well's walls |
| `aperture()` / `cut_solid()` | the same trick for lamps, grilles and ducts: the opening comes out lined |
| `plate()`, `foil()`, `gurney()`, `swan_neck()`, `diffuser()`, `panel_xy()`, `floor_plan()` | aero parts with real thickness, and floor aero shaped in plan so it follows the bodywork above it |
| `lamp_cluster()`, `led_strip()`, `grille()`, `louvre_bank()`, `mirror()` | lit and vented detail set into the bodywork |
| `conform_decal()` | a wordmark that follows the flank instead of standing a flat quad off a curved panel |
| `surface_station()` | the first station where the skin is wide enough to carry a part. The nose station is the narrowest part of the car, so a corner part placed at `NOSE` hangs in mid-air |
| `bevel()`, `sharpen()` | a small radius on every hard edge, and shading normals split by angle. `sharpen()` replaces `shade_auto_smooth`, whose geometry-nodes asset is missing on some installs |
| `cockpit_points()` | the eye, wheel and mirror the client will derive, so the interior is built to the same points |
| `join_and_export()` | merge, unify slots, write the GLB. Drives the exporter through a real window context, so it works over the MCP bridge as well as from Blender's console |

Two things to know about the loft:

* **Normals point out.** The first generation of these bodies was wound
  inside-out - every face disagreed with a normal recalc - which is a large
  part of why the paint read as flat. Keep the winding in `Loft.build`.
* **`x_at`, `z_at` resolve the right crossing.** A section is a closed loop, so
  the floor, the flank and the roof share heights and half-widths. `x_at`
  returns the *outermost* crossing of a height and `z_at` the *upper* surface
  at a half-width; taking the nearest sample instead put the arch liner on the
  roofline and the roll cage on the floor.

### Previews

`content/cars/preview_cars.py`, run inside Blender, renders a consistent sheet
into `content/props/_preview/cars/`:

```python
CARS = ["posh-gt3rs"]; VIEWS = ["hero", "side", "front", "rear", "cockpit", "mirror"]
exec(open(r"D:\apexsim\content\cars\preview_cars.py").read())
```

It loads the exported GLB plus the class wheel and places the four wheels
where `car.toml` says, so the previews show what actually ships. `side`,
`front` and `top` are orthographic. `cockpit` and `mirror` sit at the points
the client derives from the mesh box - which is how a new car's cockpit gets
verified, per the section below.

## Frame

Nose on -Y, tail on +Y, ground at z = 0, metres, left-hand drive (driver on
+X). Exported with glTF +Y up, so the nose lands on glTF +Z like the other
cars in this folder, and `AApexRaceCarActor`'s -90° yaw puts it on world +X.

## Stance

Per class, and each `car.toml`'s `[wheels]` table has to agree or the client
draws the wheel where the arch is not:

| | tyre radius (f/r) | track | axles | arch clearance |
| --- | --- | --- | --- | --- |
| GT3 | 0.345 / 0.355 m | 1.700 m | ±1.375 m | 24 mm |
| LMP2 | 0.355 / 0.365 m | 1.580 m | ±1.500 m | 22 mm |

The arch follows the tyre at that clearance, and the fender crown sits above
the arch lip, so the wheel fills the opening. The first generation ran a
0.42 m arch around a 0.35 m tyre on a 1.60 m track inside a 2.0 m body: a
circular porthole with a 7 cm gap and the tyre 20 cm inboard of it.

## Cockpit

The client derives the driver's eye from the mesh bounds
(`ApexCockpit::DeriveLayout`, closed style): 70% of the box height (the box
now runs from the splitter at ~0.04 m to the wing endplates at
`wing_z + 0.235`), 5% of the length (splitter to diffuser) behind centre, 18%
of the width to the left; the wheel goes 40 cm ahead of and 18 cm below the
eye, the centre mirror 45 cm ahead and 12 cm above it. `carlib.cockpit_points()`
reproduces that arithmetic so both generators build the interior to the same
points. Note the 18% is of the car's **width** (the mesh box's X extent), not
of the section's local width at eye height - measuring it up by the roof, where
the shell is barely a metre across, seats the driver 18 cm too far inboard.
Both generators size the cockpit to those points — cowl height, dash top (`EYE_Z - 0.16`),
wheel rim at `(EYE_Y - 0.40, EYE_Z - 0.20)`, headrest below the mirror line,
A-pillar cage bars along the screen edge — and the windscreen and side glass
start low enough to see over. Rear-view: the engine deck behind the rear
glass must stay below the mirror point, and the deck louvres start behind the
glass, or the mirror shows only bodywork (the Limbotiti's deck drops to
0.84 m for this). Verify a new car with `preview_cars.py`'s `cockpit` and `mirror`
views, which sit at exactly those points; do not tune it by eye from outside. `FApexCarCatalogRow::Cockpit`
overrides remain the escape hatch for a car that still needs a nudge.

## Material slots the client drives

Every GLB carries these slot names; keep them when re-importing.

| slot | meaning | default in the GLB |
| --- | --- | --- |
| `car_paint`, `car_accent` | body colour, sill/accent colour | per car |
| `car_glass` | windscreen, side and rear glass | translucent (glTF `BLEND`, alpha 0.5) |
| `car_headlight` | lamps | emissive warm white |
| `car_taillight` | running lights | emissive red (on with the headlights) |
| `car_brakelight` | brake lights: a full-width LED strip along the rear wing's trailing edge (endplate to endplate), the lower strip in each tail cluster, and a wrap-around corner element | emissive red — `AApexRaceCarActor` switches `EmissiveFactor` on a dynamic instance of the slot: black when off, the authored colour times `apexsim.car.BrakeLightNits` (3000) once the car's telemetry brake passes 2% (the mesh ships lit so the material imports as emissive; the glTF parent ignores `EmissiveStrength` at runtime) |
| `car_rainlight` | FIA rain light, centre of the tail (vertical bar on the LMP2s) | emissive red — on in rain / low visibility, else off |
| `car_display` | dash display | emissive green |
| `car_logo` | door / flank wordmark | masked texture from `textures/` |

## Wheels

The bodies carry no wheels. Each class has one shared wheel model,
`content/wheels/<class>.glb` (`f1`, `gt3`, `lmp2`), built by
`content/wheels/build_wheels.py`: hub at the origin, axle along X, the face
(spokes, centre-lock nut) on +X, everything inside the tyre's width and
radius. Slots `wheel_tyre`, `wheel_mark` (sidewall lettering — what makes
the spin visible), `wheel_band`, `wheel_rim`, `wheel_nut`, `wheel_brake`.

Each car.toml says where they go (visual only; the physics reads `[physics]`):

```toml
[wheels]
model = "lmp2"          # content/wheels/lmp2.glb
front_axle_m = 1.500    # ahead of the model origin (the nose is -Y in Blender)
rear_axle_m = -1.500
front_track_m = 1.560   # hub centre to hub centre
rear_track_m = 1.560
front_radius_m = 0.355  # hub height too: the tyre stands on z = 0
rear_radius_m = 0.365
front_width_m = 0.310
rear_width_m = 0.360
```

`ApexCarImport` imports the wheel once as `/Game/Cars/Wheels/<model>/SM_Wheel_<model>`
and puts the figures, with `[physics] max_steering_angle_rad`, on the
catalog row as `Wheels`. The client (`FApexCarWheelSet`,
`Race/ApexCarWheels.h`) hangs four copies off the body mesh component, sizes
each from the wheel mesh's bounds to its axle's width and diameter, turns the
right-hand pair half round so the face is outboard, steers the fronts by
`steering × max_steering_angle_rad` and rolls all four by how far the drawn car moved along its nose each frame,
over their radius (backwards when it backs up; nothing when it stands, bobs
or slides sideways — the wire's speed is the length of the whole velocity
and has no sign), at most
`apexsim.car.WheelMaxDegPerFrame` (12°) a frame: true road speed is a
motion-blurred disc, and a step near the spoke pitch strobes. `ApexSim.Wheels.*`
tests pin the placement, the steering direction and the roll direction. A
row without wheels draws none, which is right for a body that still has its
own.

`content/cars/strip_wheels.py` (run in Blender) is how the wheels came off:
`measure(glb)` finds the four tyres from the faces whose material names a
tyre and prints the `[wheels]` figures; `strip(glb)` deletes every loose part
lying wholly inside a wheel cylinder (tyre, rim, disc, caliper) and writes the
GLB back, keeping the material names. The Red Horse has no tyre material, so
its figures were measured by hand and passed in. The generators no longer
build wheels.

## Catalog rows (`DT_CarCatalog`, hand-maintained)

| car | id |
| --- | --- |
| Yotota LMP2 | `f7f70d97-08b6-4787-ac47-2f096a75f371` |
| Posh LMP2 | `7ffd2617-850c-4aee-9289-930d99492a37` |
| Fugazzi LMP2 | `3f291d76-c43d-4515-a04d-e12c2a71e23f` |
| Jeanetti LMP2 | `e7584ffb-7fb9-4c55-83df-dfb76d029de1` |
| Posh GT3 RS | `bcfabec3-f0a2-4c4f-ae71-2478835fa423` |
| Limbotiti Caravan GT3 | `51a2eb0a-a3cf-4efc-b1dd-ca2b73b08ab1` |
| Murcetes-AMD GT3 | `a442d320-0aec-4d60-822a-289563bc558b` |
