# Generated car models

Fourteen cars are generated from four Blender scripts in `content/cars/`:

| script | variants | folder / stem |
| --- | --- | --- |
| `build_lmp2.py` | `yotota`, `posh`, `fugazzi`, `jeanetti` | `yotota-lmp2/yotota_lmp2`, `posh-lmp2/posh_lmp2`, `fugazzi-lmp2/fugazzi_lmp2`, `jeanetti-lmp2/jeanetti_lmp2` |
| `build_gt3.py` | `posh`, `limbotiti`, `murcetes` | `posh-gt3rs/posh_gt3rs`, `limbotiti-caravan-gt3/limbotiti_caravan`, `murcetes-amd-gt3/murcetes_amd_gt3` |
| `build_f1.py` | `fugazzi`, `murcetes`, `mclarsen`, `ashton` | `fugazzi-sf26/fugazzi_sf26`, `murcetes-amd-w17/murcetes_w17`, `mclarsen-mcl40/mclarsen_mcl40`, `ashton-marvin-amr26/ashton_amr26` |
| `build_hypercar.py` | `panini`, `fugazzi`, `bugotti` | `panini-zomba-hypercar/panini_zomba`, `fugazzi-994p-hypercar/fugazzi_994p`, `bugotti-chiffon-hypercar/bugotti_chiffon` |

Run inside Blender with `VARIANT = "<name>"` set first, then
`exec(open(r"D:\apexsim\content\cars\build_<class>.py").read())`. Each run
resets the scene, builds body → wheel arches → apertures → parts → joined mesh
(saving the `.blend` after every stage) and exports `<stem>.glb` beside
`car.toml`. Set `APEX_EXPORT=0` in the environment to skip the export while
iterating on shape.

They also run headless, outside Blender, on the `bpy` module
(`pip install bpy`, Python 3.11): set `APEXSIM_ROOT` to the checkout (the
scripts, `carlib.CARS_ROOT`, `apex_props.PROPS_ROOT` and `preview_cars.py`
fall back to `D:\apexsim` without it), define `VARIANT` and `exec` the
script. `preview_cars.py` renders the same way with Cycles where Eevee has
no GPU/EGL.

The scripts hold only shape and livery data; everything mechanical lives in
`content/cars/carlib.py`, so a fix lands on every generated car at once.

### Character: one class, different cars

The first generations of each class were one car with different lamps and
mirrors: every GT3 had the same letterbox mouth, the same upright box vent at
the same station, the same valance and tail bar, and every prototype the same
twin radiator boxes, side intake and brake exit. Each variant now carries
its own design in data, on a shared kit:

* **the face** (`FACES` / `face`): shaped openings in the nose as `(x, z)`
  outlines (right-hand ones mirrored), cut back to where the skin is behind
  every corner (`poly_depth`), backed with mesh and barred in the car's own
  pattern (slats, vertical chrome bars, a diamond mesh from two crossed
  sets), with a chrome rim or a centre blade/strut where the design has one;
* **the flank** (`side` / `SIDES`): swept recesses whose edges lean, taper
  and bow, blades that lean with them, a dark floor that follows their
  edges, and character lines (`swage`) running along the car;
* **the hull** (prototypes): nose height, valley depth, canopy width, rear
  fender sweep and fullness per variant, on the class's shared keys;
* **the tail** (GT3 `tail`): a full-width bar with four-point brakes, lit Ys
  on black plates, or wrap-around corner lamps with a blade across the
  panel; each GT3 valance is its own outline;
* **the rear wing**: `carlib.wing()` lofts an element whose middle can dip
  (`spoon`), rise (`arch`) or run ahead of the tips (`swept`) while the tips
  stay put, so any endplate meets it; `spans()` walks its trailing edge for
  the gurney and brake strip. Each car picks a plan, an endplate outline
  (inside the class's first endplate envelope, whose top is the top of the
  eye's box), a mount (swan necks close or wide, a single central neck,
  pylons) and where its brake light goes (along the flap, across the middle,
  or up the endplates);
* **F1**: nose length and width, sidepod undercut and downwash, front wing
  (`classic`, `swept`, `low`), rear wing plan and endplates (`square`,
  `swept`, `curl`), beam wing and shark fin.

Keep a new part inside the box the client measures (the F1's fixed kit box,
and on the closed cars the box the eye is derived from): the Limbotiti's
first mouth blade stuck 20 cm out of the nose and moved the driver's eye 11
cm, and the Fugazzi hypercar's deeper valley dropped its mirrors onto the
wider fender and took the eye outside the canopy (`MIR_Z` now compensates).

**F1** (`build_f1.py`, class `F1`) is the open-wheeler, 2026 proportions:
a 3.4 m wheelbase, a 1.8 m front wing, a 1.0 m rear wing. The loft is the
survival cell, undercut sidepods and engine cover in one skin, nose cone to
crash structure; the floor, diffuser, wings, halo, suspension, brake ducts
and mirrors are parts. The client derives an open-wheel cockpit from the
mesh box (`ApexCockpit::DeriveLayout`: centreline, 8% of the length behind
centre, 82% of the height up; mirrors 70 cm ahead, 8 cm down, a quarter of
the width out), so the box is fixed by the class kit - front wing leading
edge to rear wing trailing edge, plank to T-cam - and the cockpit opening,
halo and mirror heads are built to `open_wheel_points()` before the parts
exist. `carlib.sightline(ob, open_wheel=True)` and `preview_cars.py` (for
any F1-class car) use the same eye. Brands (`VARIANTS`): shape factors
`nose_z`, `pod_w`/`pod_h`, `cover_h`, `coke`, an `inlet` shape and an
`airbox`; the livery is a list of loft boxes `(y0, y1, j0, j1)` painted in
the accent. Fugazzi SF-26 (red, white shoulder band and nose), Murcetes-AMD
W17 (silver over black, narrow pods, low nose), McLarsen MCL40 (papaya over
dark blue with a dark spine), Ashton Marvin AMR26 (green with a lime
pinstripe). Physics: 2026 power unit - 400 kW of V6 plus a 300 kW motor on a
1.1 kWh store - 780 kg, Cl*A ~4.3; the ideal Silverstone lap is
1:32.4-1:32.5, between the two reference cars. The reference F1s
(`2021-f1-fugazzi-sf21`, `redhorse-rb20`) are imported models and stay as
they are.

**Hypercars** (`build_hypercar.py`, class `Hypercar`) are the LMH class: 5.0 m
long, 2.0 m wide, a 3.1 m wheelbase, and a different hull from the LMP2s -
a low pointed nose between tall floating fenders, a deep valley either side
of a narrow teardrop canopy, a long tail, and a wing carried on carbon
endplates that grow out of the rear fenders instead of swan necks. Lamps sit
in slits across the fender fronts. Signatures (`VARIANTS`): Panini Zomba -
dark carbon blue, three round lamps a side, four round tail lamps, the quad
exhaust in the middle of the tail and a roof snorkel; Fugazzi 994P - rosso,
the lowest roof, a single blade over a slit lamp and a thin double stripe at
the back; Bugotti Chiffon - two-tone (`two_tone`: everything above the
fender's inner shoulder in the dark accent), the horseshoe grille, the
polished C-line round the side intake, a four-lamp bar and one light bar
across the tail. `rain_z` moves the rain light where the exhausts or the
tail bar would otherwise sit on it. Physics: ~1030-1045 kg, ~430-450 kW of
engine plus a 90-100 kW `[hybrid]` motor, AWD, Cl*A ~5 on capped aero; the
racing line's ideal Silverstone lap is 1:42.2-1:42.8, about two seconds
under the LMP2s.

### `carlib.py`

| piece | what it is for |
| --- | --- |
| `car_materials()` | the standard slot set, with the names the client drives |
| `Loft` | the section loft. `hard=(j, ...)` marks control indices as creases: the section is sampled either side of them, so a shoulder line or fender crown stays an edge instead of melting. `groove()` and `recess()` cut shut lines, ducts and vents *into* the shell by inserting their own stations and displacing them along the true surface normal |
| `fender_bump()` | raises the flank over each axle. Not decoration: if the arch opening tops out above the fender peak the section has no outer crossing at that height and anything sampling the skin there tears |
| `arch()` | cuts the wheel opening following the tyre at `gap` clearance. The shell is closed, so the boolean gives a watertight well for free - the cutter carries the liner slot and its own surface becomes the well's walls |
| `aperture()` / `cut_solid()` | the same trick for lamps, grilles and ducts: the opening comes out lined |
| `plate()`, `foil()`, `gurney()`, `swan_neck()`, `diffuser()`, `panel_xy()`, `floor_plan()` | aero parts with real thickness, and floor aero shaped in plan so it follows the bodywork above it |
| `projector()`, `light_guide()` / `guide_xyz()`, `front_lens()`, `led_grid()`, `tail_bar()`, `pocket_floor()` / `pocket_bezel()` | the lamp kit (see Lamps) |
| `lamp_cluster()`, `led_strip()`, `grille()`, `louvre_bank()`, `mirror()` | lit and vented detail set into the bodywork (`lamp_cluster`/`led_strip` are the old lamps; only the wing brake strip still uses `led_strip`) |
| `conform_decal()` | a wordmark that follows the flank instead of standing a flat quad off a curved panel. It lies on the *unrecessed* loft, so keep it clear of vents: the LMP2 wordmark used to run over the side intake and hid it |
| `Loft.recess(y0=(a, b), y1=(c, d), bow=...)`, `recess_edges()`, `swage()` | swept vents - each edge runs from one station at `j0` to another at `j1`, bent by `bow` - and lengthwise character lines (a groove, or a ridge with `bulge`). Swept features get dense stations but no transverse crease; a very steep lean wants a longer `rim` or its walls step between rings |
| `swept_blades()`, `swept_floor()` | blades across a swept vent parallel to its edges, and its dark floor. A slot picked per loft face cannot follow a slanted edge (the faces run square to the car) and comes out as a staircase, so swept vents get this floor instead of `face_mat`'s duct colour (`_offset(..., skip_swept=True)`) |
| `prism_xz()`, `aperture_poly()`, `rounded()`, `flip_x()`, `poly_depth()`, `poly_fill()`, `poly_bars()`, `poly_rim()` | shaped openings: cut an `(x, z)` outline, fill it (fan from the centroid, so keep it star-shaped), bar it at any angle clipped to the outline, rim it |
| `surface_station()` | the first station where the skin is wide enough to carry a part. The nose station is the narrowest part of the car, so a corner part placed at `NOSE` hangs in mid-air |
| `bevel()`, `sharpen()` | a small radius on every hard edge, and shading normals split by angle. `sharpen()` replaces `shade_auto_smooth`, whose geometry-nodes asset is missing on some installs |
| `lower_roof()`, `tumblehome()`, `shift_upper()`, `drop_bonnet()` | key reshaping, applied before the loft: pull the greenhouse down towards the belt and lean it in; slide the cabin along the car (the Murcetes' is 0.38 m forward of its keys, see Cockpit); drop the bonnet between the fender crowns so the driver sees the road |
| `top_patch()`, `top_text()` | a number plate and a race number lying on the bonnet or deck, each vertex dropped onto `z_at` so they bend over the crown instead of sinking in at the edges |
| `bucket_seat()`, `switch_panel()`, `door_card()`, `inner_skin()`, `extinguisher()` | the cabin kit (see Cockpit) |
| `sightline()` | the number the cockpit is judged by: from the eye the client will derive, how far ahead the road is visible |
| `cockpit_points()` | the eye, wheel and mirror the client will derive, so the interior is built to the same points |
| `join_and_export()` | merge, unify slots, write the GLB. Drives the exporter through a real window context, so it works over the MCP bridge as well as from Blender's console |

Two things to know about the loft:

* **`normal()` decides "outward" from the section's winding**, not from the
  direction to the section's centre. The old test flipped on any surface
  facing up-and-inward - the bonnet valley between the fender crowns, which
  is where the lamp pockets and louvres were - so those recesses came out as
  bumps and the projectors pointed into the wheel well.

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
where `car.toml` says, so the previews show what actually ships. Bounds are
taken from the vertices (a freshly imported object's `bound_box` is stale
until the depsgraph runs, and an eye derived from it sat 10 cm low), and
the `cockpit`/`mirror` eye comes from the body's box alone, as the client's
does. `side`,
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
| Hypercar | 0.360 / 0.360 m | 1.620 m | ±1.550 m | 20 mm |
| F1 (generated) | 0.360 / 0.360 m | 1.580 / 1.520 m | -1.650 / +1.750 m | open wheels |

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
`cockpit_points()` takes `top_z`, the true top of the mesh (wing endplates or
the aerial, whichever stands taller): the box height decides the eye, and
an assumed `wing_z + 0.22` put it 5 cm out.

**No generated car carries a steering wheel.** The client's cockpit rig
(`AApexCockpitRig`) draws its own wheel and display at the derived wheel
point, so a mesh wheel is a second rim a few centimetres from the first.
What the mesh does carry, all sized from those points (`build_gt3.py` and
`build_lmp2.py`, cabin kit in `carlib.py`): a cowl and dash whose top is `EYE_Z - 0.22`, its
face `WHEEL_Y - 0.28` (a forearm beyond the wheel - any closer or taller
and the cockpit view is felt), a cluster hood ahead of the driver, a data
display and switch panels, padded door cards with sill, pull and grab
handle inboard of the skin, a raked bucket with bolsters and a six-point
harness, tunnel with a switch panel, pedals and dead pedal, an
extinguisher on the passenger floor, a headliner hung under the roof
between the cage rails, and a bulkhead behind the seat (below the belt, so
the mirror sees over it). The main hoop is at `y = 0.96`, just behind the
seat back, and the A-pillar bars run down the edge of the screen from the
roof's outer edge - at `x = 0.46` the bar was across the driver's face.

**The road has to be visible.** The eye is 70% of the box up whatever the
silhouette, and a flat bonnet at 0.87 m put the cowl 5 cm under a 0.93 m
eye: the road appeared forty metres out. Two things fix it, both key
transforms in `carlib`: `drop_bonnet()` lowers the upper surface from the
screen base to the nose (the fender crowns stay, so the bonnet sits in a
valley between them, which is what a front-engined GT3 bonnet looks like),
and for the Murcetes `shift_upper()` moves the greenhouse 0.38 m forward
of its keys, because a driver seated 5% behind centre under a cab-rearward
roof had his eyes in the sunstrip. `build_gt3.py` prints
`carlib.sightline(car)` after every build: rays from the derived eye,
steepening until the bodywork blocks one. All three GT3s see the road from
6-7 m (`road_from_m`); a real GT3 driver sees it from 5-8. Anything over 10
is a letterbox and a shape problem, not a camera one. The LMP2s see it
from 6.4-7.6 m with a 2-5 cm `drop_bonnet` on the nose deck; their canopy
was also widened (j6 x1.16, j7 x1.14) and raised 4.5 cm in `apply_variant`,
because the derived eye — 18% of the width off centre, 70% of the box up —
sat inside the side glass and 6 cm under the roof of the original bubble.
Open: an LMP2 driver really sits nearer the centreline (~12% of the width),
and the left fender hump fills a third of the derived view; that is a
`FApexCockpitOverrides::Eye` per prototype, or a per-class fraction in
`ApexCockpit::DeriveLayout`, not a mesh change. Rear-view: the engine deck behind the rear
glass must stay below the mirror point, and the deck louvres start behind the
glass, or the mirror shows only bodywork (the Limbotiti's deck drops to
0.84 m for this). Verify a new car with `preview_cars.py`'s `cockpit` and `mirror`
views, which sit at exactly those points; do not tune it by eye from outside. `FApexCarCatalogRow::Cockpit`
overrides remain the escape hatch for a car that still needs a nudge.

## Material slots the client drives

Every GLB carries these slot names; keep them when re-importing.

| slot | meaning | default in the GLB |
| --- | --- | --- |
| `car_paint`, `car_accent` | body colour, sill/accent colour | per car. Metallic 0.55-0.9 under a clearcoat (`KHR_materials_clearcoat`), roughness ~0.22: a metallic paint that picks up the sky and the floodlights. The first generation shipped metallic 0.2 / roughness 0.035, which is glossy plastic |
| `car_trim` | satin-black glass surround: the band above the belt (GT3) or valley (LMP2) crease, the pillars, the cowl, and a sunstrip across the top of the screen (`face_mat` in either build script); also door pulls, bonnet/clam pins and the roof camera pod | black |
| `car_decal` | the white number plates on bonnet and deck (the race number itself is `car_trim` text) | white |
| `car_interior`, `car_alcantara`, `car_seat`, `car_harness`, `car_switch` | cabin: panels, dash top / headliner / door pads, bucket, belts, backlit buttons | dark grey, near-black, seat colour, red, amber emissive |
| `car_glass` | windscreen, side and rear glass | translucent (glTF `BLEND`, alpha 0.5) |
| `car_headlight` | lamp projector rings and cores, DRL guides | emissive warm white |
| `car_chrome`, `car_lens_tint` | projector bezels; smoked tail lenses | chrome; dark, alpha 0.22 (glTF `BLEND`) |
| `car_taillight` | running lights: the tail light guides and bars | emissive red — `AApexRaceCarActor` keeps a dynamic instance lit all session: the authored colour times `apexsim.car.TailLightNits` (700) by day, the brake glow's running share (0.12 × 3000) with the headlights on. Before that the slot was left at the GLB's own emission and never showed under the race exposure |
| `car_brakelight` | brake lights: a full-width LED strip along the rear wing's trailing edge (endplate to endplate), the lower strip in each tail cluster, and a wrap-around corner element | emissive red — `AApexRaceCarActor` switches `EmissiveFactor` on a dynamic instance of the slot: black when off, the authored colour times `apexsim.car.BrakeLightNits` (3000) once the car's telemetry brake passes 2% (the mesh ships lit so the material imports as emissive; the glTF parent ignores `EmissiveStrength` at runtime) |
| `car_rainlight` | FIA rain light, centre of the tail (vertical bar on the LMP2s) | emissive red — on in rain / low visibility, else off |
| `car_display` | dash display | emissive green |
| `car_logo` | door / flank wordmark | masked texture from `textures/` |

## Liveries

Every generated car has its works livery (the GLB as built) plus three more,
the `[[livery]]` tables at the end of its car.toml:

```toml
[[livery]]
name = "Greenline Forest"
paint = [0.008, 0.090, 0.035]     # linear RGB, like the build scripts' colours
accent = [0.780, 0.680, 0.420]    # optional: without it the model's accent stays
metallic = 0.60                   # optional: the paint's metallic
logo = "textures/livery_forest.png"   # optional: replaces the car_logo wordmark
```

A livery is a repaint of the same mesh, so it is exactly what the material
slots allow: `car_paint` and `car_accent` take the colours (which faces are
accent is the build script's `livery` / `two_tone` / sill-stripe choice and
stays the same), `car_logo` takes the texture. `content/cars/liveries.py`
owns those tables - twelve sponsor schemes dealt three to a car - and draws
the logos; it rewrites everything below its marker line, so edit the schemes
there and rerun it (`python content/cars/liveries.py [folder ...]`, Pillow).

Down the pipe: the server reads only the names (`CarConfig::livery_names`);
`SelectCar` carries a `livery` byte, the session keeps each driver's pick and
`RosterEntry.Livery` tells every client what each car wears - clamped to the
car's list, and AI cars dealt the liveries of their model in turn so a field
of one car is not a row of clones. `ApexCarImport` copies the tables onto the
catalog row as `Liveries` and imports each logo to
`/Game/Cars/<folder>/Liveries/T_<name>` (derived on every run, like the
wheels). `ApexLivery::Apply` puts dynamic instances on the three slots
(`BaseColorFactor`, `MetallicFactor`, `BaseColorTexture` of the Interchange
glTF parents); the race director applies the roster's pick, the garage
turntable the one being browsed. In the garage, Left / Right on a car (or the
livery button) steps through them; choosing the car sends the pick.
`preview_cars.py` renders a livery with `LIVERY = n`.

## Lamps

A lamp is a black cavity with things in it, not an emissive box. The kit in
`carlib.py`:

* `projector(centre, axis, r)` - one LED module, front face at `centre`
  looking along `axis`: chrome annulus (`car_chrome`), dark rim, lit ring,
  dark centre cap, lit core, all as short capped bars so it reads
  concentric from any angle.
* `light_guide()` (along the loft, by `(y, j)` waypoints) and `guide_xyz()`
  (explicit points): a thin lit tube, the DRL / tail signature.
* `front_lens()` - a clear lens flush with the skin over a box aperture,
  each grid vertex dropped onto its own station, so it follows the nose
  round the corner.
* `led_grid()` - a dark plate with a matrix of emissive dies under a clear
  cover: the FIA rain light and the brake blocks.
* `tail_bar()` - a full-width bar let into the tail panel: cavity, lips, lit
  tube, brake blocks, smoked lens (`car_lens_tint`, alpha 0.22).
* `pocket_floor()` / `pocket_bezel()` - black bottom and trim frame for a
  recessed pocket (the GT3 tail corners).

**GT3 headlamps** are a box aperture through the nose corner
(`LAMP_X = (0.50, 0.84)`, `LAMP_Z` 3-14.5 cm under `nose_top`), cut deep
enough to open on the flank so the lamp wraps the corner; inside it a back
wall, the projectors at their own station 3 cm behind the skin looking
`(±0.22, -0.95, 0.10)`, the signature, and the flush lens. They used to be a
pocket recessed into the corner's upper surface, which faces up and inward
- a shelf whose lamps were seen edge-on from the road. Signatures: Posh one
large projector with four DRL points; Limbotiti two projectors and a Y
guide; Murcetes three in a row under a hockey-stick guide along the top.
**GT3 tails** (`tail`): Posh a 7 cm full-width bar (the tip station is
pulled in behind the panel face, so the bar sits 4 mm proud of `TAIL`, as a
real bar does) with a four-point brake cluster under each end; Limbotiti a
lit Y at each corner on a black hexagonal plate with a brake triangle in
the fork; Murcetes the wrap-around corner pocket (black floor, trim bezel,
three guides, smoked lens) with a slim blade lamp across the panel climbing
to meet it and a brake row under it. Everything on the panel stands on a
plate that reaches back to the skin (`tail_skin`) and faces out at
`TAIL + 12 mm`: the tail is rounded, and parts placed at `TAIL - 4 mm`, as
the brake blocks were, sit just inside it and never show. Rain light: a
6×3 matrix in its aperture.

**LMP2 headlamps** keep their lined box aperture; in it a back wall, two
or three projectors each at its own station 3 cm in, a black mask plate
round each (or the hole shows the side of a can), the maker's DRL, and the
flush lens. **Tails**: back wall, the maker's guide, a brake matrix, smoked
lens; rain light a 2×6 vertical matrix. Four makers, four faces (`drl` /
`tail` in `VARIANTS`): Yotota a vertical blade with a bar off its middle,
tail two horizontal stripes over a brake row; Posh two projectors with
four DRL points, tail one stripe that runs on across the panel as a
`tail_bar` to the other lamp; Fugazzi a thin blade along the top, tail
twin light-guide rings each round a small tail projector, brake strip
along the bottom; Jeanetti three vertical claws at the outboard end, tail
three claws with a brake block inboard.

`preview_cars.py` has `lamps_front` / `lamps_rear` views - dusk lighting
(`dusk()`: lights ×0.06, sky 0.05), 70 mm, close on a corner - so the
lit parts carry the shot; use them to judge a lamp, not the hero view.

## Wheels

The bodies carry no wheels. Each class has one shared wheel model,
`content/wheels/<class>.glb` (`f1`, `gt3`, `lmp2`, `hypercar`), built by
`content/wheels/build_wheels.py` (set `WHEEL_CLASSES = ["hypercar"]` first to
rebuild only some): hub at the origin, axle along X, the face
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

## Engine sound

There are no engine recordings: the client simulates the engine and listens
to its exhaust (`Audio/ApexEngineSound.h`). A crank turns at the telemetry's
RPM; each cylinder's blow-down is a pressure pulse, sized by the throttle,
into one of two exhaust banks; each bank is a resonant pipe. What a car
sounds like is therefore a description of its engine, the `[sound]` table in
its `car.toml` — which the server ignores and `ApexCarImport` copies onto the
catalog row as `EngineSound`, together with `[engine]`'s `idle_rpm`,
`redline_rpm` and `rev_limiter_rpm` (none of which is on the wire). It is
derived like the wheels: every import run brings the row back in step, `-force`
or not.

```toml
[sound]
cylinders = 8
crossplane = true        # a crossplane V8's uneven banks; false fires the banks alternately
turbo = false
exhaust_length_m = 1.9   # primary pipe: where the exhaust resonates
muffling = 0.45          # 0 open pipes .. 1 road muffler
pops = 0.9               # overrun pops and shift cracks, 0..1
gear_whine = 0.3         # straight-cut gearbox, 0..1
intake_roar = 0.6        # induction noise under throttle, 0..1
```

| key | what it does to the sound |
| --- | --- |
| `cylinders` | The note *is* the firing rate, `rpm/60 × cylinders/2`: a V8 at 7500 is 500 Hz, the F1's V6 at 15,000 is 750 Hz, a V10 at 8500 is 708 Hz. |
| `crossplane` | Eight cylinders only. A crossplane's banks fire L R L L R R L R, so each pipe gets a 90-180-270-180° rhythm that repeats every two revs: power on the crank's own frequency and its odd halves, under the firing note. That is the V8 burble, and it is the whole difference between the Murcetes and the LMP2s' flat-plane eights at the same revs. |
| `exhaust_length_m` | The headers are quarter-wave resonators (modes at `c/4L`, `3c/4L`, …) and the tailpipe, 0.45 of this, a half-wave one: long pipes boom, short ones bark. 0.2–2.8. |
| `muffling` | The silencer: a two-pole low-pass on what leaves the collector, 400 Hz (1.0) to 8 kHz (0.0); how much of each blow-down's steep front gets out as rasp (next to none below ~0.2); how much of the firing is blow-down rather than swell; and the outlet's low-end boom. The scream of the F1 is `muffling = 0.05` as much as it is 15,000 rpm. |
| `pops` | On a lift above a third of the rev range, unburnt charge lights in the pipe for about a second: oversized pulses with a burst of noise. Also how likely a flat-out upshift is to crack. The limiter's stutter pops regardless. |
| `gear_whine` | A tone at the engaged pair's tooth-mesh frequency — it steps *up* on an upshift at the same revs. |
| `intake_roar` | Throttle-gated induction noise through an airbox resonance, pulsing with the firing. |
| `turbo` | A whistle that spools with load (lagging it), and a blow-off hiss on a lift. |

A car without the table gets its class's usual engine
(`ApexEngineAudio::MakeSpec`: F1 a turbo V6 to 15,000, LMP and Hypercar a flat-plane V8,
anything else a crossplane V8), so a new car makes a sound before anybody
tunes it.

Tuning is by ear, and a race is a poor place for it. `apexsim.audio.RenderCars`
(console, editor or game; optional output directory) drives every catalog
car through the same scripted run — idle, two blips, flat out through the
gears, the limiter, a lift and the overrun down the box, a part-throttle
cruise — and writes `Saved/Audio/<folder>.wav` (the engine as it leaves the
tailpipe, mono: what everybody else hears) and `<folder>_own.wav` (what its
driver hears, stereo, from the cabin or open cockpit), plus `road.wav` with
each tyre and road voice in turn. Edit the TOML, `ApexCarImport -car=<folder>`, render,
listen. Unattended:

```bash
"$UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" game-unreal/ApexSim.uproject \
    -ExecCmds="apexsim.audio.RenderCars,quit" -nullrhi -nosound -unattended
```

`ApexSim.Audio.Engine*` automation tests pin the physics rather than the
taste: the power is on the firing note at either device rate, a crossplane
has the half-orders and a flat-plane does not, the GT3 keeps its power under
2 kHz where the F1 has a third of it above, pops stand out of the overrun,
the limiter stutters, a dead engine is silent.

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
| Panini Zomba HR | `b840e586-11e6-494b-8a0c-2bbac31c2239` |
| Fugazzi 994P | `941a167f-9633-4624-9873-f833e67b647e` |
| Bugotti Chiffon | `b87b42c9-9157-483c-bac8-1b5061454834` |
| Fugazzi SF-26 | `41222d7b-5216-4011-ac68-4bc75ad26b35` |
| Murcetes-AMD W17 | `d1d4e751-ee64-4b0f-b0bf-5db1caa6bd43` |
| McLarsen MCL40 | `f3ec7fc8-1024-4576-8e7d-d500cede66fe` |
| Ashton Marvin AMR26 | `cd10083c-7f03-454a-b7f9-427025fce254` |
