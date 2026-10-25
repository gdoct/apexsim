# COTA start/finish: live broadcast vs ApexSim

A side-by-side of `cota-startfinish.png` (Sky Sports F1 onboard, Leclerc, Q1,
Circuit of the Americas main straight, daytime) and
`cota-startfinish-apexsim.png` (ApexSim, Armadillos, chase "roof" rung, 14
cars, late sun), with what the game needs to reach the live frame's look.
Written 2026-10-24.

The work items are roadmap packages 39 to 50
([roadmap.md](roadmap.md#39-road-surface-detail)), one per gap, each
independent of the others, and in the work plan as G7 to G10, C6 to C12
and N29 ([work-plan.md](work-plan.md)). Each is a rule for every circuit,
not a fix for this one: COTA is where the gap showed, and the change is
judged on the reference frames of several circuits.

The reference frame is a run per circuit, `sf-ref-<stem>` in
`scripts/verify/runs.yml` ([verification.md](verification.md)): the SF-26
on pole, sunny at 13:00, racing line off, the HUD hidden, four shots
before the lights (the cockpit from the pole box, the chase `roof` rung
there, a driver's eye on the line down the lap and back over the grid).

## The two frames

| | Live | ApexSim (`sf-ref-armadillos`) |
|---|---|---|
| Camera | T-cam on the roll hoop, wide lens, halo and driver in frame | Cockpit from the pole box: the halo and its pillar across the top, both front wheels, the steering wheel, both mirrors; or the eye on the line, 1 m over the road |
| Time, sky | Midday, thin high cloud, flat white sky, soft shadows | 13:00 sunny: a clear blue sky with no cloud, hard short shadows, no haze; the far end of the straight as crisp as the kerb |
| Position | On the S/F line itself, grid boxes behind, start gantry 300 m ahead | The pole box, the gantry right over the line and filling the top of the frame |
| Car | Current F1 car, full livery, sponsor decals, visible wheel, hands, helmet | The SF-26: flat red bodywork and halo, its own wheel and screen, mirrors with live pictures |

The first comparison set this frame against a chase `roof` shot at
sunset. The reshoot at midday closed what the hour alone explained: the
orange gradient, the rim-lit stands and the road in shadow are gone, and
the two bright lobes on the road ahead of the car do not show at midday
exposure. They were the car's headlights, which the server switches on
with the sun under 6 degrees; an open-wheeler throws no beam since
(package 48), and the mirror faces seen from behind are the car's own
chrome glass reflecting the sky, not the cockpit's screens. What it
left are real gaps, and the same frames at Monzarella and Shiverstone
show each of them too: the same generic white box gantry on lattice legs
(46), a uniform charcoal road (39), grid boxes without numbers (40), no
aerial haze and no cloud at `sunny` (45). At Armadillos alone: the
stadium stand without a crowd (43), lamp posts along the pit-side fence
(42) and a bare prairie horizon (44).

## Differences, by region of the frame

### Sky, light and atmosphere

- **Live**: an overcast-bright sky, light from everywhere, shadows soft and
  short, the far hills and stands fading into pale haze. Colour is neutral
  and slightly cool.
- **ApexSim**: a clear sunset. Everything is orange, the stands' tops are
  rim-lit, the road is in shadow and reads flat. There is no haze: the far
  gantry, the pit building and the horizon line are as crisp as the kerb.
- Gap: the time of day alone explains most of this. What is a real gap is
  the missing aerial perspective by day (the server fog only thickens the
  haze in fog weather, `ApexSky::FogDensityFor`), and the sky's lack of cloud
  cover at `sunny`.

### Horizon and surroundings

- **Live**: a treeline and low hills close the horizon on both sides; stands
  and buildings stack in depth; a car park and the paddock show between them.
- **ApexSim**: the horizon is bare prairie, flat to the vanishing point. One
  red-roofed block and the veil tower stand on the left, nothing on the right
  beyond the stand. No treeline behind the pit building, no car-park texture.
- Gap: a far backdrop. The `PRAIRIE` tree style is sparse by design
  (correct for the infield), but the live frame's horizon is still green:
  live oak belts along the property line and the hills north of the circuit.
  A `forest_impostor` ring at the property edge and a DEM-driven far
  heightfield would close the horizon.

### The road

- **Live**: grey, worn asphalt with visible aggregate, darker rubber lines
  on the racing line, a lighter band mid-track, patch repairs, oil sheen in
  the grid boxes, tyre marks across the line. The S/F line and the grid
  boxes are crisp white with slot numbers. Blue-and-white painted apron on
  the right edge (COTA's signature kerb-side paint). White edge lines both sides.
- **ApexSim**: uniform charcoal, soft and blurred close up, no aggregate, no
  rubbering, no wear band, no patches. Grid boxes are dashed outlines
  without numbers. The chequer is a short strip. The right apron is plain
  tarmac with the racing-line dots on it. Two bright lobes shine on the road
  ahead of the car (the mirror or headlight emissives at dusk exposure).
- Gap: a road detail layer (aggregate normal and roughness, tiling at road
  scale, with a macro variation mask so it does not repeat); a worn
  racing-line band and rubbered grid-box paint (`RoadState` already carries
  the rubber figure on the wire, the material needs the texture to express
  it); `grid_slot` markings drawn solid with a slot number; a `custom`
  marking or decal set for COTA's painted apron; the dusk glow on the road
  looked at (`ScreenNits` at low EV, or the car's emissive parts).

### Left side (pit side)

- **Live**: low concrete pit wall at road level, then the pit-wall gantry
  with team stands, then the pit building's three storeys and the roof
  terrace with people on it; four big screens on its face; a debris fence
  with poles the length of the straight; sky-blue crowd on the far stand.
- **ApexSim**: armco with a short mesh fence, `VELOCET` hoardings on it,
  grass then sand verge, lamp posts along the fence, the pit building a
  plain red-roofed `office_block` a long way back, no pit wall, no team
  stands, no screens readable from here, no people.
- Gap: the pit wall family is generated from the pit lane (`pit_wall_6m`,
  garages, `box_kit`) but at this station the lane has not started, so the
  straight shows the plain edge treatment. Needed: a pit wall gantry with
  team stands along the whole box span, a `wing_media_centre` or a three-storey
  pit building with the four screens the dossier names
  (`MANUAL_LANDMARKS` screen entries at 20, 105, 190 and 275 m exist; check
  they are laid and face the right way), a tall `catch_fence_6m` run in
  place of the mesh, crowd on the terrace, and the lamp posts removed from a
  day venue (the same rule gap as the 24 `floodlight_tower`s).

### Right side (main grandstand)

- **Live**: concrete wall with `MSC` boards at the base, then a tall debris
  fence with regular poles, then the three-tier Main Grandstand under its
  canopy, full of spectators, team flags along the rail, TV cameras on the
  balcony. The stand is set back behind a wide run-off with the painted apron.
- **ApexSim**: `concrete_4m_rail` with `ROLUX` boards, then the stand
  (`bay_10m_stadium_roof`) starts almost at the wall with no fence between,
  red and blue seat stripes and no crowd, the roof a thick dark slab. The
  stand is too close to the road and too high at the front edge.
- Gap: the stadium family's `_crowd` twin (the `bay_10m` families have one;
  the stadium family needs its own seated strip), a catch fence between
  wall and stand, the run-off width and the stand's setback (the OSM
  outline is 19 m off the road; the laid stand looks nearer), flags and
  rail dressing along the front, and the canopy as a light tensile roof
  rather than a slab.

### The start gantry and the far end

- **Live**: a dark truss gantry with the start-light panel, a sector board
  and sponsor boards on its face, 300 m away and small; beyond it the
  straight drops to T1 and the T1 stand sits on the hill.
- **ApexSim**: a white box gantry on four lattice legs, a dot-matrix panel
  under it, close and dominant; the road beyond runs flat to a block of
  garages (the pit complex starts after the gantry here).
- Gap: there is no `bridge/start_gantry` mesh in the kit (the frame shows
  the generated fallback); it needs a truss look and a dark finish, a
  branded fascia, and the lights on a smaller housing; the pit complex's
  position relative to the S/F line is right by the dossier, so the
  difference is the gantry's size and finish, not its station.

### The car

- **Live**: full livery with decals and wordmarks, carbon weave on the halo,
  sponsor text on the halo and the wing mirrors, wheel with rim branding, a
  tyre with sidewall markings and visible graining, suspension arms and
  the steering wheel with its screen, the driver's gloves and helmet.
- **ApexSim**: a flat red body with a glossy coat, no decals, a translucent
  orange windscreen, flat black tyres, bare halo, bright pink-white blocks
  where the mirrors sit.
- Gap: a Formula-class livery kit (the `[[livery]]` and `car_sponsor` atlas
  exist for GT3; the single-seater needs a sponsor layout and wordmarks),
  tyre sidewall and rim textures, carbon on the halo and wings, and the
  mirror faces at a sensible brightness at low exposure.

### Camera and lens

- **Live**: the T-cam is ahead of and above the driver's helmet, so the halo
  arches through the frame and the front wheels show at the edges. The
  lens is wide (about 100 degrees horizontal), with a slight barrel, soft
  highlights and mild compression softness.
- **ApexSim**: the `roof` rung is further back and higher, so the rear
  bodywork fills the frame and there is no driver. The lens is narrower and
  clean.
- Gap: a `tcam` rung or shot: eye on the roll hoop, pitched 4 to 6 degrees
  down, wide field of view, horizon locked, with the halo and the driver's
  helmet from `<stem>_driver.glb` in frame. The shot camera has `onboard`;
  the chase ladder (`ApexChase::Views()`) is where a driver-selectable one
  goes.


## What to add, in payoff order

1. **Reshoot to match** (roadmap 38): cockpit view, sunny, midday, the SF26.
   Everything below is judged against that frame, not the sunset one.
2. **Road surface detail** (roadmap 39, 40): aggregate normal and roughness with a macro
   variation mask, a racing-line rubber band driven by `RoadState.rubber`,
   darker, wetter-looking paint in the grid boxes, solid numbered
   `grid_slot` markings. This is the single largest gap in the frame.
3. **Catch fences and the pit wall** (roadmap 41, 42): `catch_fence_6m` on both sides of the
   main straight (the audit's "confirm" item), a pit-wall gantry with team
   stands over the box span, the four pit-building screens laid and facing
   the stand, lamp posts and floodlights off at a day venue.
4. **Crowd in the Main Grandstand** (roadmap 43): a `_crowd` twin for
   `bay_10m_stadium_roof`, flags on the rail, the stand set back to the OSM
   outline, a lighter canopy.
5. **Horizon** (roadmap 44, 45): a treeline impostor ring at the property edge, hills from
   the DEM to the north, daytime haze on the sky (aerial perspective at
   `sunny`, not only in fog).
6. **Start gantry** (roadmap 46): a dark truss with a branded fascia and a small light
   housing, scaled to the real span.
7. **Formula livery and detail** (roadmap 47, 48): a sponsor layout for the single-seater,
   halo and wing carbon, tyre sidewalls, rim textures, mirror brightness
   fixed at low exposure.
8. **T-cam rung** (roadmap 49) with a wide lens and the driver in frame; optional
   barrel distortion and a touch of bloom to read as broadcast rather than
   render.
9. **COTA apron paint** (roadmap 40): a decal set for the blue-and-white edge strip on
   the main straight (a `custom` marking is the quick version).
10. **Broadcast HUD preset** (roadmap 50, optional): timing tower, lower third, sector
    bar as HUD components.

## Where the work lands

| Item | Doc | Code |
|---|---|---|
| Road material, markings, decals | [road-mesh.md](../../apexsim-content/docs/road-mesh.md), [track-format.md](../../apexsim-content/docs/track-format.md) | `ats-export`, `M_ApexTrackRoad` |
| Fences, pit wall, screens, lamp rule | [props.md](../../apexsim-content/docs/props.md), [track-improvements.md](../../apexsim-content/docs/track-improvements.md) | `ats-dress`, `osm_layout.py` tables, `ue_export.rs` pit complex |
| Stand crowd twin, canopy | [props.md](../../apexsim-content/docs/props.md) | `build_stand_crowds.py`, the stadium bay GLBs |
| Horizon, haze, clouds | [server/conditions.md](server/conditions.md), [game/client.md](game/client.md) | `ApexSky`, `ApexCloudLayer`, the DEM `horizon` mesh |
| Livery, tyres, carbon | [cars.md](../../apexsim-content/docs/cars.md) | `build_f1.py`, `liveries.py`, `textures.py` (`tools/generators/cars/`) |
| T-cam rung | [game/cameras.md](game/cameras.md) | `ApexChaseView.h`, `ApexCockpitRig` |
| HUD preset | [game/hud-modding.md](game/hud-modding.md) | `content/hud` components |

The audit of the whole circuit, with the stands and landmarks already
checked, is [Armadillos.md](../../apexsim-content/docs/circuit-references/audit/Armadillos.md).
