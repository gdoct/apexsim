# Generated car models

Fourteen cars are generated from four Blender scripts in `scripts/content/cars/`,
into `content/cars/default/<folder>/` (`content/cars/custom/` is the player's
own, never written by these scripts):

| script | variants | folder / stem |
| --- | --- | --- |
| `build_lmp2.py` | `yotota`, `posh`, `fugazzi`, `jeanetti` | `yotota-lmp2/yotota_lmp2`, `posh-lmp2/posh_lmp2`, `fugazzi-lmp2/fugazzi_lmp2`, `jeanetti-lmp2/jeanetti_lmp2` |
| `build_gt3.py` | `posh`, `limbotiti`, `murcetes` | `posh-gt3rs/posh_gt3rs`, `limbotiti-caravan-gt3/limbotiti_caravan`, `murcetes-amd-gt3/murcetes_amd_gt3` |
| `build_f1.py` | `fugazzi`, `murcetes`, `mclarsen`, `ashton` | `fugazzi-sf26/fugazzi_sf26`, `murcetes-amd-w17/murcetes_w17`, `mclarsen-mcl40/mclarsen_mcl40`, `ashton-marvin-amr26/ashton_amr26` |
| `build_hypercar.py` | `panini`, `fugazzi`, `bugotti` | `panini-zomba-hypercar/panini_zomba`, `fugazzi-994p-hypercar/fugazzi_994p`, `bugotti-chiffon-hypercar/bugotti_chiffon` |

Run inside Blender with `VARIANT = "<name>"` set first, then
`exec(open(r"E:\apexsim\content\cars\build_<class>.py").read())`. Each run
resets the scene, builds body → wheel arches → apertures → parts → joined mesh
(saving the `.blend` after every stage) and exports `<stem>.glb` beside
`car.toml`. Set `APEX_EXPORT=0` in the environment to skip the export while
iterating on shape.

They also run headless, outside Blender, on the `bpy` module
(`pip install bpy`, Python 3.11; pass 5 was built on bpy 5.0.1 in a cloud
session, eight seconds a car): set `APEXSIM_ROOT` to the checkout (the
scripts, `carlib.CARS_ROOT`, `apex_props.PROPS_ROOT` and `preview_cars.py`
fall back to `E:\apexsim` without it), define `VARIANT` and `exec` the
script. `preview_cars.py` renders the same way with Cycles where Eevee has
no GPU/EGL.

The scripts hold only shape and livery data; everything mechanical lives in
`scripts/content/cars/carlib.py`, so a fix lands on every generated car at once.

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
survival cell with the sidepods on it and the engine cover, nose cone to
crash structure: a ~0.48 m tub whose rim is at ~0.72, pod tops ten
centimetres under it with a crease where they meet (j5), the pod flank as
the widest line (j3) and under it the undercut turning in to the floor
edge (j2) - the void between pod and floor you see from the front
three-quarter. Everything below j2 is bare carbon. The floor (edge wing,
fences, plank, diffuser), wings, halo, suspension, brake ducts with their
wheel-wake deflectors, and mirrors are parts. The wings are inverted
wings - each element's trailing edge above its leading edge - and the
front wing's flaps rise and steepen towards the endplates
(`carlib.foil_path`). The eye is authored (pass 6, see Cockpit): 1.52 m
behind the front axle, 0.79 m up, on the centreline; the cockpit opening,
halo (a 4.8 cm section, body colour or carbon per car) and the mirrors
(aero heads whose glass is the `car_mirror_*` slot the rig paints) are
laid off it, and `[cockpit]` carries the eye, wheel and both mirrors as
built. `carlib.sightline(ob, eye=EYE, ray_x=0.032)` looks past the halo's
centre pillar from one eye. Brands (`VARIANTS`): shape factors
`nose_z`, `pod_w`/`pod_h`, `cover_h`, `coke`, an `inlet` shape and an
`airbox`; the livery is a list of loft boxes `(y0, y1, j0, j1)` painted in
the accent, `sponsors` the atlas cells for the pod, engine cover, nose,
both endplates and the DRS flap. Fugazzi SF-26 (red, white shoulder band and nose), Murcetes-AMD
W17 (silver over black, narrow pods, low nose), McLarsen MCL40 (papaya over
dark blue with a dark spine), Ashton Marvin AMR26 (green with a lime
pinstripe). Physics: 2026 power unit - 400 kW of V6 plus a 300 kW motor on a
1.1 kWh store - 780 kg, Cl*A ~4.3; the ideal Silverstone lap is
1:32.4-1:32.5, between the two imported F1s they were tuned against
(since removed).

**Hypercars** (`build_hypercar.py`, class `Hypercar`) are the LMH class: 5.0 m
long, 2.0 m wide, a 3.1 m wheelbase, and a different hull from the LMP2s -
a nose deck between the front fenders, a valley either side of a narrow,
tall cabin (pass 6, sections matched to the imported 499P: deck ~0.6, crowns
~0.75-0.8, cabin ~0.43 a side at its base and 1.11 high), a long tail, and a
wing carried on carbon endplates that grow out of the rear fenders instead
of swan necks. The cabin has a windscreen and door windows, not a glass
bubble; below the sill line (j < 1.7) the body is bare woven carbon; louvred
vents are cut through the front fenders into the arch wells; the door
carries a number panel (`numbers.png`) and the maker's wordmark, with
sponsors (`sponsors`) behind the window, on the tail, on the fender crowns,
the wing and its endplates. The eye is authored, 0.36 m ahead of the
wheelbase's middle, 0.16 m off centre, 0.83 m up (the 499P's: 0.40 /
0.15 / 0.74), the seat and pedals moved with it, and the mirrors are aero
heads on the front fenders' shoulders with `car_mirror_*` glass. Lamps sit
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
| `lower_roof()`, `tumblehome()`, `shift_upper()`, `drop_bonnet()` | key reshaping, applied before the loft: pull the greenhouse down towards the belt and lean it in; slide the cabin along the car; drop the bonnet between the fender crowns so the driver sees the road |
| `scale_width()`, `lift_points()`, `roofline()` | pass-5 key reshaping, measured against the imported cars: scale the half-widths to the class's real width; lift the belt or the fender crowns over a span; and author the roof as a *line in profile* - `(y, z)` targets the roof centre is scaled onto about the belt - so a fastback is a list of six points rather than a by-product of nine section tables |
| `authored_cockpit()`, `cockpit_table()`, `write_table()`, `write_cockpit_table()` | the eye the *car* says (see Cockpit): the wheel and mirror laid off it, and the `[cockpit]` table written into car.toml above the liveries' marker (`write_table` is the generic one; the F1 builds' `[drs_flap]` goes through it too) |
| `textured_mat()`, `atlas_uv()`, `number_uv()`; `car_materials(carbon_weave=, sponsors=)` | pass 6: base-colour textures from `content/cars/_textures/` (written by `textures.py`): the carbon twill on `car_carbon` (its UVs scaled by `CARBON_UV_SCALE` in `join_and_export()`), the sponsor atlas `car_sponsor` and the number panels `car_number`, both alpha-masked. Opt-in per build; the F1s and hypercars use them, the GT3s and LMP2s not yet |
| `conform_decal(uv_rect=)`, `top_decal()`, `flat_decal()` | a decal following the flank, lying on an upper surface (along or across the car), or flat on a plate (endplates, wings): each takes an atlas cell |
| `driver_figure()`, `export_driver()`, `ellipsoid()`, `capsule()`, `inside_bar()` | the driver (see Driver) and his GLB / `[driver]` table; a cage tube kept inside the shell |
| `foil_path()` | a wing element lofted through sections whose leading edge, pitch and chord vary along the span - a front-wing flap rising and steepening into the endplate |
| `aero_mirror()`, `mirror_glass()` | a teardrop mirror head with its glass in a `car_mirror_left` / `_right` slot, UVs as the cockpit rig expects (u from the driver's right to left, v top-down), so the rig paints its capture onto the car's own glass as it does an imported car's |
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

`scripts/content/cars/preview_cars.py`, run inside Blender, renders a consistent sheet
into `content/props/_preview/cars/`:

```python
CARS = ["posh-gt3rs", "custom/KsPorsche911Gt3R2016"]
VIEWS = ["hero", "side", "front", "rear", "cockpit", "mirror"]
exec(open(r"E:\apexsim\scripts\content\cars\preview_cars.py").read())
```

It loads the exported GLB plus the class wheel (or, for a `custom/` car such
as an AC import, the car's own `wheels/front.glb` and `rear_model`) and
places the four wheels where `car.toml` says, so the previews show what
actually ships, and what a generated car is measured against. Headless
without a GPU (a cloud session on the `bpy` wheel) `APEX_PREVIEW_CYCLES=1`
renders the same sheet with Cycles on the CPU (`APEX_PREVIEW_SAMPLES`,
default 48; about 80 s a view). Bounds are
taken from the vertices (a freshly imported object's `bound_box` is stale
until the depsgraph runs, and an eye derived from it sat 10 cm low), and
the `cockpit`/`mirror` eye is the car.toml's `[cockpit] eye_cm` when it has
one, else derived from the body's box alone, as the client's is. `side`,
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
| GT3 | 0.345 / 0.355 m | 1.700 m | ±1.375 m | 32 mm, the arch wrapping 28° under the hub |
| LMP2 | 0.355 / 0.365 m | 1.580 m | ±1.500 m | 22 mm |
| Hypercar | 0.360 / 0.360 m | 1.620 m | ±1.550 m | 20 mm |
| F1 (generated) | 0.360 / 0.360 m | 1.580 / 1.520 m | -1.650 / +1.750 m | open wheels |

The arch follows the tyre at that clearance, and the fender crown sits above
the arch lip, so the wheel fills the opening. The first generation ran a
0.42 m arch around a 0.35 m tyre on a 1.60 m track inside a 2.0 m body: a
circular porthole with a 7 cm gap and the tyre 20 cm inboard of it.

### Pass 5: measured against the imported cars (2026-10-01)

The AC importer put real GT3s and prototypes beside the generated ones
(`content/cars/custom/Ks*`), so the generated ones could be measured
against them: `preview_cars.py` takes `"custom/<folder>"` entries (and an
imported car's own wheels), and a GLB's silhouette was tabulated station by
station (top of the body at the centreline, at the shoulder and at the
flank, the half-width, the floor, every 20 cm; the project notes for the
pass have the script). What the numbers said, and what changed:

| | imported | generated, before | now |
| --- | --- | --- | --- |
| GT3 width over the arches | 2.00-2.06 m (AMG, 911, Huracán) | 2.10-2.14 m | 2.00-2.02 m (`scale_width` to 1.0) |
| GT3 floor / splitter | 85 / 70 mm | 75 / 52 mm | 85 / 62 mm (`FLOOR_LIFT`) |
| GT3 belt line at the door | 0.93-0.96 m, the flank standing to it | 0.80-0.84 m, the flank rolling in from 0.72 | 0.90-0.96 m (`belt_lift`; new Murcetes keys) |
| GT3 roof | 1.17-1.25 m, peaking behind the B-pillar, one fastback to the deck | 1.28-1.30 m, peaking ahead of the middle, a flat deck a metre long | 1.17-1.25 m, authored with `roofline` (Posh, Limbotiti) or new keys (Murcetes) |
| GT3 cabin | cab-rearward on the front-engined AMG (screen base 0.45 m ahead of the middle, roof peak 0.7 behind it) | slid 0.38 m forward to meet the derived eye | where the engine puts it, since the eye is authored |
| GT3 arch | a circle the tyre fills, the flare in body colour | a U as wide as the tyre down to the sill, a carbon ring | wraps 28° under the hub, flare in `car_paint` |
| GT3 mirrors | on the door at the belt | on the A-pillar | on the door, 0.36 m behind the screen base |
| prototype width | 1.90 m (by rule) | 2.04 m with the skirts | 1.93-1.98 m |
| prototype fender crowns | 0.73-0.76 m (LMP1), ~0.80 (LMP2) | 0.95 m | 0.76-0.81 m |
| prototype canopy base | ~0.9 m across | 1.2 m (widened 16% for the derived eye) | ~0.95 m |
| prototype engine deck at the rear axle | 0.45-0.55 m beside the fin | 0.87 m | 0.61 m, the fin standing out of it |
| prototype wing endplates | 10-15 cm over the plane | 25.5 cm | 13 cm |
| driver's eye, GT3 | 0.33-0.66 m behind the middle of the wheelbase (rear- to front-engined), 0.33-0.40 m off centre, 0.90-0.96 m up | derived: 0.24 m behind, 0.40 off, 0.93 up | authored per car: 0.05 / 0.33 / 0.60 behind, 0.36 off, 0.92-0.97 up |
| driver's eye, prototype | 0.21-0.28 m *ahead* of the middle, 0.15-0.20 m off centre, 0.78-0.79 m up | derived: 0.24 behind, 0.35 off, 0.92 up | authored: 0.25 ahead (+ the canopy shift), 0.18 off, 0.82 up |

The lamp kit, the faces, the flanks and the cabin kit are unchanged; what
the imported cars taught was proportion, and the one structural change
proportion needed - the eye - is the Cockpit section below. Still open
after this pass, in the order the renders show it: the class wheel's rim
is small in the tyre (an 18" GT3 rim nearly fills a 0.69 m tyre; the AC
rims do), the GT3 headlamps are slits where the real ones are pods that
wrap onto the fender, the nose faces are flatter than the real cars'
(the AMG's grille is most of its face), and the imported cars' panel
shut lines, bonnet vents and door furniture are denser than the kit's.

### Pass 7: the texture kit on the GT3s and LMP2s, drivers (2026-10-02)

The GT3s and LMP2s take the pass-6 kit (woven carbon, sponsors from the
atlas - sunstrip, quarter, fender, endplates on the GT3s; fender crowns,
wing and endplates on the LMP2s - number panels on the doors, bonnet, nose
and deck instead of the white plates and extruded numerals; bare carbon
below the LMP2 sill line), mirror heads whose glass is the rig's
`car_mirror_*` slot (`carlib.mirror(glass=)`, written into `[cockpit]`), and
the `gt3` / `lmp2` wheels the lettered sidewalls. Their wings were lifting
wings too (trailing edges below the leading) and are inverted. A ray probe
from outside (any interior or cage face hit before the shell) found the
cabins leaking: the LMP2 cabin walls at the pods' width, the GT3 dash and
door cards standing through the bonnet and the A-pillar foot, the cage's
A-pillar bars outside a tumblehome canopy. Fixed with `XIN` measured up to
the belt, the GT3 footwell kept inside the front wells (`ibox`), the door
cards starting where the skin covers them, and `carlib.inside_bar()` for
every cage tube. Every car now has a driver (below).

### Pass 6: the F1s and hypercars against the imports (2026-10-02)

The same method as pass 5 - silhouettes and full sections of each GLB
against the imported SF70H / Formula Hybrid 2021 and the 499P, aligned on
the front axle - plus a shared texture kit (`scripts/content/cars/textures.py`
-> `content/cars/_textures/`: carbon twill, sponsor atlas, number panels,
tyre lettering; every name in them invented).

| | imported | native before | now |
|---|---|---|---|
| F1 eye behind front axle / up | 1.43-1.57 / 0.67-0.78 | 1.87 / 0.84 (derived) | 1.52 / 0.79 (authored) |
| F1 section at the pods | tub, pods with a shoulder, undercut to a separate floor | one round blob from 0.03 to 0.66 | tub rim 0.72, pod top 0.58, flank 0.62 out, undercut to the floor edge |
| F1 wings | elements rise to the trailing edge | sloped down to it (a lifting wing) | inverted; front flaps rise into the endplates |
| F1 floor | 50-90 mm up | 12-28 mm | 28-40 mm, edge wing, 4 fences a side |
| LMH deck between front fenders | ~0.60, flat | 0.33-0.52, a scoop | 0.45-0.65 |
| LMH front fender crowns | ~0.70-0.78 | 0.83-0.89 | 0.73-0.80 |
| LMH cabin base / roof | ±0.42 / 1.12 | ±0.47-0.55 / 1.04 | ±0.43 / 1.11 |
| LMH eye ahead of middle / off / up | 0.40 / 0.15 / 0.74 | derived | 0.36 / 0.16 / 0.83 |
| bare carbon | wings, floor, lower third of the body, woven | flat near-black | twill texture (4 tiles a metre) on every carbon face, lower body carbon |
| livery | 15-25 decals | one wordmark, a white number plate | wordmark, 5-6 sponsors, number panels (LMH) |


## Cockpit

**The GT3 and LMP2 builds author the eye** (pass 5). Each GT3 variant
names it (`eye=(x, y, z)` in `build_gt3.py`'s `VARIANTS`; the LMP2 build
uses one point for the class plus the canopy shift), the cabin kit is laid
off it (`carlib.authored_cockpit()`: wheel 40 cm ahead and 20 cm below,
mirror 45 cm ahead and 12 cm up, the bucket's front edge 0.46-0.48 m
ahead, pedals 1.1-1.2 m ahead), `carlib.sightline(car, eye=EYE)` is judged
from it, and the build ends by writing it into car.toml as the `[cockpit]`
table (`eye_cm`, `wheel_cm`, `style`, the class's `wheel_lock_deg`;
`write_cockpit_table`, above the liveries' marker so `liveries.py` leaves
it alone - the hand-written table that used to sit below the marker was
one rerun away from being lost). The client takes those over its
derivation, and `preview_cars.py`'s `cockpit` and `mirror` views read them
too (`authored_eye`). The numbers come from the imported cars'
`DRIVEREYES`: a front-engined GT3 driver sits 0.65 m behind the middle of
the wheelbase, a mid-engined one on it, a rear-engined one a third of a
metre behind, all ~0.35 m off centre and 0.90-0.96 m up; a prototype
driver 0.2-0.3 m ahead of the middle, 0.15-0.20 m off centre and
0.78-0.80 m up. Every generated cabin used to be shifted, widened or
raised to meet the derived eye instead - the LMP2 driver sat 0.35 m off
centre and 0.92 m up in a canopy 1.2 m wide, and the Murcetes' greenhouse
was slid 0.38 m forward of a long-bonnet GT's - which is why the derived
eye is now the fallback, not the rule.

Since pass 6 the F1s (`style = "open"`) and the hypercars write `[cockpit]`
too, with `mirror_left_cm` / `mirror_right_cm` and their sizes as built:
the client lays the mirrors off the eye it *derives* unless they are named,
so an authored eye alone would leave them where the old box put them. The
F1 eye moved from the derived 1.87 m behind the front axle and 0.84 m up to
1.52 m and 0.79 m (the SF70H: 1.57 / 0.78; the 2021 car: 1.43 / 0.67).

For a car without a `[cockpit]` eye the client
derives the driver's eye from the mesh bounds
(`ApexCockpit::DeriveLayout`, closed style): 70% of the box height (the box
runs from the splitter at ~0.04 m to the wing endplates), 5% of the length
(splitter to diffuser) behind centre, 18%
of the width to the left; the wheel goes 40 cm ahead of and 18 cm below the
eye, the centre mirror 45 cm ahead and 12 cm above it. `carlib.cockpit_points()`
reproduces that arithmetic so a generator can build the interior to the same
points (`eye=` hands it an authored one instead). Note the 18% is of the car's **width** (the mesh box's X extent), not
of the section's local width at eye height - measuring it up by the roof, where
the shell is barely a metre across, seats the driver 18 cm too far inboard.
`cockpit_points()` takes `top_z`, the true top of the mesh (wing endplates or
the aerial, whichever stands taller): the box height decides the eye, and
an assumed `wing_z + 0.22` put it 5 cm out.

**No generated car carries a steering wheel.** The client's cockpit rig
(`AApexCockpitRig`) draws its own wheel and display at the derived wheel
point, so a mesh wheel is a second rim a few centimetres from the first. (A
car that brings its own wheel says so in `[cockpit]`; see "How a car
reaches the game".)
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

**The road has to be visible.** A flat bonnet at 0.87 m once put the cowl
5 cm under a 0.93 m eye and the road appeared forty metres out.
`drop_bonnet()` lowers the upper surface from the screen base to the nose
(the fender crowns stay, so the bonnet sits in a valley between them, which
is what a front-engined GT3 bonnet looks like), and the pass-5 keys keep
the bonnet valley 0.13-0.17 m under the eye at the screen base and falling
to the nose. Both builds print `carlib.sightline(car, eye=EYE)` after every
build: rays from the authored eye, steepening until the bodywork blocks one.
The GT3s see the road from 6.2-8.5 m (`road_from_m`; a real GT3 driver,
by the imported AMG's own eye and cowl, from about 12), the LMP2s from
5.5-6.2 m (the deck is held low to the foot of a steep screen by the key
at -0.95, which the canopy shift carries with it). Anything over 10 is a
letterbox and a shape problem, not a camera one - and the first thing a
0.36 m eye saw on the new Murcetes was a bonnet pin at x = 0.36.
Rear-view: the engine deck behind the rear
glass must stay below the mirror point, and the deck louvres start behind the
glass, or the mirror shows only bodywork (the Limbotiti's deck drops to
0.84 m for this). Verify a new car with `preview_cars.py`'s `cockpit` and `mirror`
views, which sit at exactly those points; do not tune it by eye from outside. `FApexCarCatalogRow::Cockpit`
overrides remain the escape hatch for a car that still needs a nudge.

## How a car reaches the game

Nothing about a car is cooked. The game reads each `content/cars/{default,custom}/<folder>/car.toml`
(a packaged game: `Game/Cars/{default,custom}/<folder>`) and builds the GLBs it names the
first time something draws them: its own glTF reader, the fast mesh build the
tracks use, and dynamic instances of four cooked parents under
`/Game/Materials/Car` (`ApexMaterialBake`) that keep the parameter names
Interchange's glTF parents had. Edit a car, re-export the GLB, and run
`apexsim.car.Rescan` (or restart the game); no editor import. The whole path
is `docs/RUNTIME_CONTENT_LOADING.md`, "Cars". `ApexCarImport` still imports
cars as assets under `/Game/Cars`, for looking at one in the editor; the game
does not use them.

The turntable framing and the cockpit points a person tuned on a
`DT_CarCatalog` row still apply; a car.toml can carry its own instead:

```toml
[preview]
offset_cm = [0.0, 0.0, 12.0]
rotation_deg = [0.0, 90.0, 0.0]   # pitch, yaw, roll
scale = 1.0

[cockpit]
style = "closed"                  # auto | open | closed
eye_cm = [-35.0, 38.0, 108.0]     # the car's frame: +X nose, +Y right, +Z up
wheel_cm = [10.0, 38.0, 90.0]
# mirror_centre_cm, mirror_left_cm, mirror_right_cm
wheel_rake_deg = 18.0             # optional: the rim's tilt, positive = top toward the driver
wheel_lock_deg = 270.0            # optional: rim turn at full steering, one way
steering_wheel_model = "steering_wheel.glb"   # optional: the car's own wheel, see below
rig_wheel = false                 # optional (default true): hide the rig's rim
rig_dash = false                  # optional (default true): hide the rig's hub display
```

Every key is optional, and a zero `wheel_rake_deg` / `wheel_lock_deg` means
"derive it from the style" as before (write `0.01` for a truly upright rim).
A key the table leaves out keeps the `DT_CarCatalog` row's hand-tuned value
when the car has one (`ApexCarContent::MergeCockpit`), so a car.toml that
only names its `wheel_lock_deg` keeps its framing. `wheel_lock_deg` is also
the car's steering lock for a wheel on the Auto steering lock: the real rim
turns twice it lock to lock, and the rim on screen follows it 1:1.
The last three are for a car with a real interior, an imported one:

- `steering_wheel_model` is a GLB beside the car.toml that the cockpit rig
  (`AApexCockpitRig`) draws **in place of** its generated rim and turns with
  the steering telemetry exactly as it turns the rim (same lock, same
  easing). Author it in the car GLB's own axes with the wheel **straight
  and upright**: hub at the origin, the rim in glTF XY (+Y up, +X the car's
  left), the column along glTF **+Z, toward the nose**; in AC terms, the
  `STEER_HR` dummy's local frame with its tilt taken out. The rig puts the
  hub at `wheel_cm`, tips it by the rake (`wheel_rake_deg`, else the style's
  12-22°) about the car's lateral axis and rolls it about the column; so
  write the real column's tilt as `wheel_rake_deg` rather than baking it
  into the mesh, or it is tilted twice. The rig's hub display stays on top
  of it unless `rig_dash = false`.
- `rig_wheel = false` hides the generated rim without supplying a model:
  for a body with a static steering wheel of its own.
- `rig_dash = false` hides the display the rig puts on the hub: for an
  interior with a display of its own.

The mirrors are unchanged by any of these. `ApexSim.Cars.TomlCockpitRig` and
`ApexSim.Cockpit.CarWheel` pin the keys and the mount.

## Material slots the client drives

Every GLB carries these slot names; keep them when re-exporting.

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
| `car_brakelight` | brake lights: a full-width LED strip along the rear wing's trailing edge (endplate to endplate), the lower strip in each tail cluster, and a wrap-around corner element | emissive red — `AApexRaceCarActor` switches `EmissiveFactor` on a dynamic instance of the slot: black when off, the authored colour times `apexsim.car.BrakeLightNits` (3000) once the car's telemetry brake passes 2% (the car parents ignore `KHR_materials_emissive_strength`, as Interchange's did) |
| `car_rainlight` | FIA rain light, centre of the tail (vertical bar on the LMP2s) | emissive red — on in rain / low visibility, else off |
| `car_display` | dash display | emissive green |
| `car_logo` | door / flank wordmark | masked texture from `textures/` |
| `car_carbon` | bare carbon: wings, floor, splitter, diffuser, the lower body of the F1s and prototypes | since pass 6/7 the twill texture (`content/cars/_textures/carbon_twill.png`, 4 tiles a metre) under a clearcoat |
| `car_sponsor`, `car_number` | sponsor decals and race-number panels | masked textures (`sponsors.png`, `numbers.png`); not touched by a livery |
| `car_mirror_left`, `car_mirror_right` | the door / pod mirrors' glass | the cockpit rig paints its rear-view capture onto them (u from the driver's right to left, v top-down), as on an imported car |
| driver GLB: `car_suit`, `car_visor` (+ `car_paint`, `car_accent`, `car_trim`, `car_harness`) | the driver figure (`[driver]`, below): race suit in the car's colour, dark visor; the helmet in the paint and accent, so a livery repaints it | |

## Driver (all generated cars, pass 7)

Every generated car has a driver in its seat, in a GLB of his own beside
the body, `<stem>_driver.glb`, in the body's frame (no transform), named by
a `[driver]` table the build writes above the liveries' marker:

```toml
[driver]
model = "posh_gt3rs_driver.glb"
```

`carlib.driver_figure()` builds him from the points the cabin is built to:
the authored eye (his eyes - the visor sits a few cm ahead of them), the
rig's wheel (gloves at 9 and 3, `wheel_hw` out: 16 cm closed, 13 cm open,
the rig's own rim half-widths), the hip joint the bucket seat puts him on and
his boots on the pedals; arms and legs are two-bone IK
(`_ik`), the torso and helmet ellipsoids, limbs capsules (`ellipsoid()`,
`capsule()`), with the HANS collar and belts. The F1 one (`style="open"`)
lies back in the tub with narrower shoulders. ~4k triangles, ~140 kB. The client
(`FApexCarDriver`, `AApexRaceCarActor::SetDriver` / `SetDriverVisible`) draws
him on every car and hides him for the car the cockpit camera sits in (the
camera is his eyes) while keeping his shadow (`bCastHiddenShadow`); he goes
with the bodywork (`SetMeshVisible`), takes the livery on his helmet and the
ghost's tint. `preview_cars.py` loads him for every view but `cockpit` and
`mirror`. He does not move: the rig's wheel turns under static gloves.

## DRS flap (F1)

The F1 bodies are exported without the rear wing's upper flap: `build_f1.py`
builds it as its own object and writes `<stem>_drs.glb` beside the car, its
origin on the hinge (the flap's trailing edge at the tips, the axis across
the car), and a `[drs_flap]` table in car.toml above the liveries' marker,
replaced on every build:

```toml
[drs_flap]
model = "fugazzi_sf26_drs.glb"
hinge_forward_m = -2.5298     # the wheels' convention: ahead of the body origin
hinge_up_m = 0.8226           # above its floor
open_deg = 25.0               # leading edge up, opening the slot over the main plane
```

The game builds the flap from its GLB like the body (its own mesh and
materials) and puts the figures on the row as `DrsFlap` (`FApexDrsFlapSpec`). `FApexCarDrsFlap` (`Race/ApexCarDrsFlap.h`) hangs it on the
body mesh: the race car swings it open over `ApexDrs::SwingSeconds` (0.18 s)
whenever the telemetry's `bDrsOpen` is set and shut when it clears; the
turntable shows it shut; liveries repaint it with the body and the ghost
tints it. `ApexSim.Drs.FlapTransform` and `ApexSim.Cars.TomlDrsFlap` pin the
maths and the TOML. A car without the table draws its whole wing in the
body, as before. `preview_cars.py` draws the flap too (`DRS_OPEN = 1` opens it).

## Damage

What a car's damage looks like is the client's (`Race/ApexCarDamage.h`);
the server only sends five percentages per car (front, rear, left, right,
engine; server `damage.rs`), so everything below is worked out from them.

- **Dents and scuffs** are drawn by the four car parents themselves
  (`ApexMaterialBake`, `BuildCarDamage`): the body's custom primitive data
  holds each body zone's visual amount (`ApexDamage::Visual`, the share to
  the 0.6 power, so a 4% tap already shows) and the body's box in its mesh
  frame. Within reach of a damaged face (15% of the half length or width,
  45% at full damage) the world position offset pushes the panels in by up
  to `DamageDentCm` (15; the sides 60% of it, the tail 80%), unevenly, with
  an 8 cm crumple; the moved surface is shaded flat per triangle (crushed
  metal) and the paint scuffed in patches to carbon with streaks of bare
  metal along the car. An undamaged car draws exactly as before, and so do
  the wheels, the flap and an imported track's scenery (which shares the
  parents and has the offset switched off). A parent baked before the
  graph is baked again by the next `ApexMaterialBake`.
- **Parts** come off: see below.
- **Smoke, steam and sparks** (`AApexCarEffectsActor`, drawn with the
  baked `M_ApexCarSmoke` on instanced engine spheres and cubes, as the rain
  is; no particle assets): oil smoke from the tail from 35% engine damage,
  darkening to black when it is out; steam from the nose when it is
  damaged and the coolant is past 104 °C; sparks off a zone's face on
  every hit (a zone growing 0.5% in a frame) and while the car scrapes a
  wall (`bIsColliding`).

`apexsim.car.DamagePreview "60,50,35,20,85"` draws that damage on every car
instead of the server's (empty for the telemetry), and
`apexsim.car.DamageEffects 0` leaves only the dents. An unattended run can
damage the field mid-race with `-ApexExecAfter="22=apexsim.car.DamagePreview
60,50,35,20,85"`, so the parts fly on camera.

### Damage parts

A part is a box of the body that comes off when its zone's damage reaches
`detach_pct`: the game splits the body GLB at runtime
(`ApexGlb::SplitByBoxes`, `UApexCarContentSubsystem::LoadModelPieces`),
every triangle whose centre lies in a box going to that part's own mesh,
which the race car draws on the body (`AApexRaceCarActor::SetDamageParts`)
until it is thrown off as an `AApexCarDebrisActor` (the car's velocity, a
kick off the damaged face, a spin; it bounces, slides and lies on the
track for 40 s). A repair puts it back. A part that carries the DRS flap's
hinge takes the flap with it. The body keeps the whole model's bounds, so
the cockpit, the headlights and the turntable do not move.

```toml
[[damage_part]]
name = "front_wing"
zone = "front"            # front | rear | left | right
detach_pct = 30           # the zone's damage, percent
min_m = [2.485, -1.012, -0.094]   # forward, left, up: the car's frame, metres
max_m = [3.125, 1.012, 0.500]
```

`scripts/content/cars/damage_parts.py` writes them for the shipped cars
from each body's geometry (above the liveries' marker, replaced on every
run; `--preview` draws `build/damage_parts/<folder>.png`): an F1 loses its
front wing with the nose tip (30%) and its rear wing (45%), a hypercar or
LMP2 its lower nose (45%) and rear wing (50%), a GT3 its splitter and
bumper (50%) and rear wing (55%). The script's docstring has the rules;
`MANUAL_REAR_WING` holds what they cannot find. A car without the tables
dents and smokes but keeps its bodywork. `ApexSim.Cars.Damage.*` test the
TOML, the split, the shares, the debris, the puffs and every repo car's
parts.

## Liveries

Every generated car has its works livery (the GLB as built) plus six more,
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
stays the same), `car_logo` takes the texture. `scripts/content/cars/liveries.py`
owns those tables - twenty-four sponsor schemes dealt six to a car, new ones
appended so a saved pick keeps its index - and draws the logos (Poppins and
Lora Regular; `APEX_FONTS` points it at them, see the script's header); it rewrites everything below its marker line, so edit the schemes
there and rerun it (`python scripts/content/cars/liveries.py [folder ...]`, Pillow).

Down the pipe: the server reads only the names (`CarConfig::livery_names`);
`SelectCar` carries a `livery` byte, the session keeps each driver's pick and
`RosterEntry.Livery` tells every client what each car wears - clamped to the
car's list, and AI cars dealt the liveries of their model in turn so a field
of one car is not a row of clones. The game reads the tables onto the
catalog row as `Liveries` and loads each logo PNG as a texture the first time
it is shown. `ApexLivery::Apply` puts the car's own dynamic instances on the
three slots (`BaseColorFactor`, `MetallicFactor`, `BaseColorTexture` of the
car parents); the race director applies the roster's pick, the garage
turntable the one being browsed. In the garage, Left / Right on a car (or the
livery button) steps through them; choosing the car sends the pick.
`preview_cars.py` renders a livery with `LIVERY = n`.

### Texture liveries (skins)

An imported car (Assetto Corsa's skins) is repainted by texture, not by
colour. A `[[livery]]` may name a skin instead of, or as well as, a paint:

```toml
[[livery]]
name = "Gulf"
skin = "skins/gulf/Skin_00.png"       # BaseColorTexture of every car_skin* slot
textures = ["EXT_RIM=skins/gulf/EXT_RIM.png", "INT_Banner=skins/gulf/banner.jpg"]
preview = "skins/gulf/preview.jpg"    # optional; carried on the row, nothing draws it yet
```

- `skin` replaces the base colour texture of every slot named `car_skin` or
  `car_skin_<anything>` (`car_skin_1`, ...), on the body, the DRS flap and
  the wheels.
- `textures` replaces the base colour texture of any other slot by name:
  `"SLOT=file"` strings, **one line** (the parser reads line by line), for
  the parts a skin also repaints (rims, banners). A slot the body does not
  have is ignored.
- Paths are relative to the car folder; PNG or JPEG. Each file is loaded
  once whatever number of cars wear it. A file that is not there is a
  warning and leaves its slots as authored.
- Only the texture changes: the slot keeps its authored factor, metallic,
  roughness and clear coat. `paint`, `accent`, `metallic` and `logo` still
  work beside a skin; with no `paint` the `car_paint` slot keeps the
  model's colour. A table needs a `name` and one of `paint`, `skin` or
  `textures`.
- Livery 0 (the model as authored) puts every swapped slot back.
  `ApexLivery::Apply` remembers the slots it textured on the component
  (tags `ApexLiveryTextured:<slot>`), because a skin may name any slot.

Rows: `FApexCarLivery::RuntimeSkin`, `RuntimeTextures` (`FApexLiveryTexture`:
slot, file), `RuntimePreview`. `ApexCarImport` does not import skins (the
game never uses its rows for a car on disk). `ApexSim.Cars.TomlTextureLivery`,
`LiverySkinSlots` and `LiveryTextures` (a JPEG skin and a PNG slot texture
applied to built bodies and taken off again) are the tests.

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
`scripts/content/wheels/build_wheels.py` (set `WHEEL_CLASSES = ["hypercar"]` first to
rebuild only some): hub at the origin, axle along X, the face
(spokes, centre-lock nut) on +X, everything inside the tyre's width and
radius. Slots `wheel_tyre`, `wheel_mark` (sidewall lettering — what makes
the spin visible), `wheel_band`, `wheel_rim`, `wheel_nut`, `wheel_brake`,
and `wheel_cover` on the F1 wheel (the 2022+ aero cover over the spokes).
Since pass 6 the lettering is `content/cars/_textures/tyre_marks.png`
wrapped twice round each sidewall, reading from that side (the client turns
the right-hand wheels round rather than mirroring them, and
`preview_cars.py` now does the same); the `f1` and `hypercar` wheels are
rebuilt with it, `gt3` and `lmp2` still carry the plain arcs until rebuilt.

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

`model` may instead name a wheel of the car's own, a GLB relative to its
folder: any value that ends in `.glb` or holds a `/` is looked for there
(`ApexCarToml::IsCarLocalWheel`), anything else is a class wheel in the
shared folder. `rear_model` (same rule, optional) draws a different wheel
on the rear pair, which an F1 car's wider, taller rears want: each model is
scaled to its own axle's radius and width, so a front rim stretched over
the rear axle is what it avoids. Both use the class wheel's frame (hub at
the origin, axle along glTF X, face on +X) and slot names, and are shipped
beside the car.toml by `build_game_standalone.ps1` / `build_release.ps1`
(`Get-ApexCarFiles`, `Test-ApexCars` in `scripts/lib/ApexCars.ps1`).

```toml
[wheels]
model = "wheels/front.glb"       # the car folder's
rear_model = "wheels/rear.glb"   # optional; else the rears draw `model`
```

The game builds the wheel once, from `content/wheels/<model>.glb`
(`Game/Wheels` in a package) or the car's own file, and puts the figures, with `[physics] max_steering_angle_rad`, on the
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

`scripts/content/cars/strip_wheels.py` (run in Blender) is how the wheels came off:
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
its `car.toml` — which the server ignores and the game reads onto the
catalog row as `EngineSound`, together with `[engine]`'s `idle_rpm`,
`redline_rpm` and `rev_limiter_rpm` (none of which is on the wire).

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
each tyre and road voice in turn. Edit the TOML, `apexsim.car.Rescan`, render,
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

## Car ids

Each car.toml's `id` is its catalog row's key (and its `DT_CarCatalog` row's,
where it has one):

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
