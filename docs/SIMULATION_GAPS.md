# Simulation gaps: car properties the physics does not yet model

What the 240 Hz car model leaves out, as a feature list: each gap says what
exists today, what is missing, a difficulty estimate (how hard to build and
land safely) and an impact estimate (how much it changes what driving and
racing feel like). High level on purpose — no formulas, no tuning numbers;
each item becomes its own design when it is picked up. The last two
sections widen the lens past the car: the environment's effect on grip, and
where the simulation should aim to be *better* than other simsrather
than merely catch up.

For orientation, what *is* simulated today: a 4-wheel 3D model with per-wheel
loads, vertical spring/damper suspension with anti-roll bars, Pacejka-style
tires with pressure, load sensitivity and a friction ellipse, static aero
(drag, front/rear downforce, DRS), turbo lag, an opt-in differential, fuel
consumption and fuel mass, two-node tyre temperatures with a gas-law
pressure, tyre wear and three compounds, pit stops, brake temperature and fade,
engine coolant temperature, slipstream and dirty air, ride-height and rake aero with wing
knobs, a simple hybrid deploy/regen, weather-baked grip, air density from altitude
and temperature, gusting wind, and collision
damage with a drivable/undrivable threshold.

## Cross-cutting constraints

Every item below has to respect the same four things, and most of the cost of
each feature is here rather than in its physics:

- **Determinism.** The sim must stay bit-identical run to run
  (`tests/determinism_test.rs`): no wall clock, no RNG, no HashMap iteration.
  Any stateful model (temperature, wear) must evolve purely from tick inputs.
- **The AI and the racing line read a static track.** Both are built from
  `grip_coefficient` and the weather-baked track grip at session start.
  Anything that moves grip *over time* (tire temperature, wear, fuel mass)
  makes the AI's speed profile and the player's racing-line aid wrong unless
  they are taught about it — historically the most common way a physics
  change has broken the AI survey.
- **The wire.** New driver-visible state (tire temps, wear, fuel, brake
  temps) has to reach the client: appended fields on the positional
  telemetry (old clients skip them), golden bytes re-pinned on both sides.
- **The hot loop.** 240 Hz × full grids; per-tick work must stay on the
  cached-index path and off allocations. Benchmarks (`cargo bench`) are the
  gate.

## Tire temperature

**Done 2026-09-29** (CLAUDE.md, "Tyre temperature"): a tread and a core per
tyre, heated by the patch's friction power and the carcass flexing, cooled
by the air, the road (much more when wet) and each other; grip flat in a
per-compound window and falling off either side; the garage's pressures
become the hot ones, the gas law moving the running pressure with the core;
air and track temperature derived from the session's weather and clock;
blankets for the F1, half a formation lap's warmth on a race grid, a hotlap
out on warm tyres; the AI scaling its plan by the grip its tyres have; the
temperatures and pressures on the telemetry and the HUD; AC imports take
their window from AC's performance curve. Left for later: the player's
racing line on cold tyres, tyre heat as an FFB and squeal cue, the
hypercars' front tread spikes, inner/middle/outer temperatures (they need
camber, below), and wear, which now has the heat to be built on.

**Was:** `TireConfig` carries `optimal_temperature_c` and
`temperature_grip_falloff`, but physics never reads them — the fields are
parsed and inert. Tire grip only varies with pressure, load and weather.

**Missing:** a per-tire temperature state heated by slip and load, cooled by
airflow and the road, with grip falling off either side of the optimum, and
pressure rising with temperature (which would make the existing pressure
model dynamic instead of static). Cold tires on the out-lap, overheating in
a long defence.

**Difficulty: medium.** The thermal model itself is small; the cost is the
integration — the AI and racing line assume constant grip, the setup garage's
pressure knob changes meaning, and the state must reach telemetry and the HUD.

**Impact: high.** This is the single biggest realism gap for anyone who has
driven another sim: warm-up laps, managing the tires through a stint and the
pressure/temperature interplay are core simracing gameplay. Also gives the
FFB and tire-squeal audio a temperature cue for free.

## Tire wear and degradation

**Done 2026-09-29** (CLAUDE.md, "Tyre wear, compounds and pit stops"):
wear from the patch's friction power (faster when hot), a gradual grip
loss and a cliff from 70%, read by the AI. Left for later: flat spots
from lockups and punctures.

**Was:** `wear_rate` is parsed and inert. A tire is identical on lap 1 and
lap 50.

**Missing:** per-tire wear accumulating from slip energy, costing peak grip
as it grows; flat spots from lockups; eventually punctures at the extreme.
Only meaningful once temperature exists (wear without heat models half the
mechanism) and mostly pointless without pit stops to reset it.

**Difficulty: medium** on its own, but it drags in pit stops and compound
choice (below) before it is a feature a player can act on.

**Impact: high for race sessions, near zero for hotlap.** Degradation is
what creates strategy — pace management, undercuts, tire offsets between
cars. Without it every race is a flat-out sprint, which is what races are
today.

## Tire compounds and pit stops

**Done 2026-09-29** (CLAUDE.md, "Tyre wear, compounds and pit stops"):
soft / medium / hard on every car, chosen by a setup knob; the pit lane
baked to the server (`<Stem>.pit.msgpack`), an automatic limiter, a stop
at the car's box with a timed service (tyres, fuel where the rules allow,
repairs), and an AI that plans a stop and drives the lane. Left for later
in the running list below.

**Was:** one compound per car (front/rear scale factors exist for cars
with different compounds per axle). No pit stops of any kind — the pit lane
is a surface and a speed-limit line, but nothing happens in the box.

**Missing:** a compound set per class (grip vs. longevity trade-off), a
compound pick on the grid and in the pit stop, and the pit stop itself
(stop-in-box detection, a serviced timer, tires/fuel/repairs). The pit lane
geometry, boxes and pit-lane surface class already exist on every track.

**Difficulty: high.** Pit stops touch the session state machine, the AI
(when to stop, driving the lane at the limiter), the HUD and the wire; the
compounds themselves are cheap once wear exists.

**Impact: high for races** — together with wear this is what turns a race
into a strategy game. Meaningless until wear and fuel mass exist.

## Fuel mass and strategy

**Done 2026-09-29** (CLAUDE.md, "Fuel"): the tank's mass on top of the dry
car (and optionally its position), consumption from the power the engine
makes over its thermal efficiency (so lift-and-coast saves fuel), a dry tank
cutting the engine, the session filling each car from its own lap estimate
(race distance + 8% + a lap, three laps for a hotlap, full for practice), a
`fuel_load` knob in laps, the AI planning at its starting load, and the tank
on the telemetry and the HUD. Left for later: rebuilding the AI's and the
racing line's profiles as the tank drains, an AI that saves fuel when short,
and a per-car `tank_front_share` (no shipped car sets one).

**Was:** fuel is consumed per tick and the level is tracked, but the mass
of the car never changes — a full tank and a dry tank corner identically —
and nothing happens when it reaches zero.

**Missing:** fuel load as mass (and its position, so a draining tank shifts
balance), running dry cutting the engine, a fuel knob in the setup garage,
and a lift-and-coast incentive. Cheap physics; the work is again the AI and
racing line, whose lap-time profile becomes fuel-dependent.

**Difficulty: low–medium.** The most self-contained item on this list.

**Impact: medium.** A few tenths a lap of natural pace evolution, quali vs.
race trim, and it makes the existing fuel plumbing mean something.

## Aerodynamics: ride height, wing settings and damage

**Done 2026-09-29** (CLAUDE.md, "Ride-height aero and wings"): ride heights
worked out from the axle loads over the springs with bump rubbers, a
per-class map (more downforce lower, a stalling floor, rake moving the
balance forward) referenced to where the car rides at 50 m/s so the
calibration holds, the racing line planning with it at steady state,
front and rear wing and ride-height knobs in the garage, and a damaged
nose losing front downforce. Left for later: crests (the loads carry no
vertical acceleration yet), yaw/roll sensitivity and porpoising. Imported
Assetto Corsa cars get their map from AC's own ground-height tables.

**Was:** aero is three constants (drag, front lift, rear lift) plus DRS.
The suspension moves but the aero never notices: pitch, heave and ride
height change nothing, and the setup garage deliberately offers no aero
knobs because none would do anything.

**Missing:** downforce and balance responding to ride height and rake
(which finally couples the suspension setup knobs to aero, the way real
setup work happens), front/rear wing angle as setup knobs, and aero loss
from front-end damage. Ground effect and stall behaviour are the deep end
of the same feature and can come later.

**Difficulty: medium.** The map from posture to coefficients is contained
in the per-car aero step; the subtlety is keeping the racing-line profile
honest and not destabilising the AI over curbs and crests.

**Impact: high for the driving feel of downforce classes.** It is what makes
an F1 car's behaviour change under braking and over crests, and it gives the
existing suspension knobs (springs, ride) their real consequences.

## Slipstream and dirty air

**Done 2026-09-29** (CLAUDE.md, "Slipstream and dirty air"): a wake behind
every car, set once per tick from a snapshot of the field — the tow taking
up to 35% off the follower's drag and fading over ~30 m, the dirty air up
to 25% off its downforce (the front wing worst) and fading over ~15 m,
both across a widening wake and scaled by the two cars' drag areas; the
AI backing off in dirty air; the tow on the telemetry and a TOW light on
the HUD. Left for later: an AI that pulls out of the tow to pass (the
traffic layer still holds a follower at a following distance, see "AI
that races" below), tyre heat from following closely, and the wake in the
racing line's plan.

**Was:** cars do not exist for each other aerodynamically. No tow on the
straights, no downforce loss following through a corner.

**Missing:** a wake behind each car reducing drag (the tow) and, closer and
in corners, reducing the follower's downforce. The car positions are all on
the server already; this is a per-pair proximity term in the aero step.

**Difficulty: medium.** The model is simple; the care goes into the hot loop
(a pairwise pass exists for collisions to piggyback on), determinism, and
the AI — which currently neither uses the tow to pass nor suffers for
following closely.

**Impact: high for racing.** Slipstream is the overtaking mechanic; DRS
already exists but has no tow to combine with. Directly changes how every
race and the demo mode look.

## Brake temperature and fade

**Done 2026-09-29** (CLAUDE.md, "Brake and engine heat"): a temperature per
corner from what the pads absorb, cooled through a duct (a setup knob) and
by radiation; carbon (weak cold, fading past 1000 °C) and steel (fine
cold, fading past 700 °C) by class; the AI braking earlier on brakes short
of their best; brake temperatures on the telemetry and the HUD. Left for
later: brake wear.

**Was:** braking is a constant force with a bias and ABS. Brakes never
heat, fade or wear, and there are no brake ducts.

**Missing:** per-corner brake temperature from braking energy, cooled by
speed; fade when overheated, weak brakes when cold; optionally wear and a
duct-size setup knob. Feeds the FFB and the HUD.

**Difficulty: low–medium.** Self-contained per wheel, similar shape to tire
temperature and best built on the same thermal plumbing.

**Impact: medium.** Mostly a management layer for endurance-style stints
and heavy-braking circuits; subtle in short races.

## Engine thermal, wear and progressive damage

**Engine thermal done 2026-09-29** (CLAUDE.md, "Brake and engine heat"):
the coolant heated by the power made, cooled by a radiator sized per car
through the air the car meets (less in a tow), power lost past 112 °C.
**Progressive damage done 2026-09-29** (CLAUDE.md, "Progressive
damage"): hits scaled by closing speed to a power, engine damage from
heat and missed downshifts; front costs downforce and cooling, rear its
downforce, a side grip and a pull, the engine power; out at 100%; the AI
pits for it; a HUD damage panel (a car diagram per zone). A per-driver
damage aid (off / reduced / full, host-lockable) followed 2026-10-01.
Engine wear over distance is still open.

**Was:** the engine is a torque curve with a limiter, turbo lag and an
inert damage percentage: `engine_damage_percent` accumulates from crashes
but changes nothing until the 80% drivable threshold parks the car.
Body-side damage percentages are likewise all-or-nothing.

**Missing:** damage that *does something* before the cliff — engine damage
costing power and temperature headroom, front damage costing downforce,
suspension damage pulling the car sideways — plus engine temperature from
sustained load, and over-rev damage from downshifts. Repairs belong to the
pit stop feature.

**Difficulty: medium.** The states are cheap; the design work is making
degradation readable to the driver (audio, HUD, FFB) rather than a silent
lap-time tax, and keeping damaged AI cars driving plausibly.

**Impact: medium–high.** Makes contact matter, which changes how humans and
the AI race each other; today a survivable hit is nearly free.

## Hybrid deployment control

**Done 2026-09-29** (CLAUDE.md, "Hybrid deployment"): three modes
(Harvest, Balanced, Attack) on a key, an overtake button, a per-lap
deployment budget the Balanced mode paces over the lap, coasting and
harvest-mode recovery against the crank, a turbo generator (MGU-H) for
the AC imports that have one, the WEC's minimum deployment speed for the
hypercars, the AI on Balanced with the button when chasing, and an ERS
badge and battery bar on the HUD. AC's `ers.ini` now maps to the battery
(from `DISCHARGE_TIME`), the lap budget (`MAX_KJ_PER_LAP`) and the heat
recovery (`TORQUE_PERC`).

**Was:** the hybrid deploys naively — full assist whenever the throttle is
open and the battery has charge, regen under braking. The driver has no say
and the battery balance over a lap is whatever falls out.

**Missing:** deployment modes or an overtake button, a per-lap energy
budget, and AI use of the same. Small physics, mostly input/wire/HUD work
(a new input field, appended like DRS was).

**Difficulty: low.** The plumbing pattern (DRS: input bit, telemetry flag,
HUD badge) already exists to copy.

**Impact: medium, concentrated in the hybrid classes** (WEC, F1). Without
it those cars are just cars with a bigger torque curve.

## Suspension geometry

**In progress 2026-09-29** (server side built, uncommitted): `server/src/geometry.rs`
— body roll from the roll stiffness, per-wheel contact camber (static +
roll less the linkage's gain less carcass lean) feeding a lateral and a
longitudinal grip multiplier normalised to the filed camber at the car's
reference cornering (Monza AI laps within 0.15 s of before), toe on all
four wheels (rears steered and rotated), bump stops past the static
compression. `[suspension]` keys `camber_*_deg`, `camber_gain_*`,
`toe_*_deg`, `bump_stop_gap_m`, `bump_stop_rate_n_per_m` on every shipped
car; `CarSetup` knobs 22-25 (`camber_front/rear`, `toe_front/rear`) with
the server's golden bytes; `tests/geometry_test.rs`. The AC importer maps
`STATIC_CAMBER`, the camber gain from the wishbone/strut points,
`TOE_OUT` over the steering arm and the bump stops (not yet re-run on the
911, no importer test yet). **Still to do:** the client (knob table and
garage rows for the four knobs, `C_SetCarSetup` golden), an importer
test, the AI survey, CLAUDE.md, and the not-done list (no camber thrust,
no inner/outer tyre temperatures, one bump-stop gap per car, FFB
stiffness ignores toe).

**Today:** the suspension is four vertical spring/damper units with
anti-roll bars — no camber, toe or caster as physics inputs (caster exists
only inside the FFB's trail model), no kinematics, no bump stops, and the
tire has no compliance of its own.

**Missing, in rough order of value:** camber and toe as per-axle setup knobs
feeding the tire model, bump stops (curbs currently bottom out politely),
and only much later real geometry (roll centres, camber gain). Full
kinematic suspension is deliberately last: it is the most work on this list
for the least feel per hour, and it multiplies the tuning surface of every
car.

**Difficulty: knobs low–medium; geometry high.**

**Impact: medium.** Camber/toe are expected setup vocabulary and cheap wins
once the tire model reads them; the deep geometry mostly matters to setup
enthusiasts.

## Environmental conditions: ambient temperature, humidity, wind

**Done 2026-09-29** (CLAUDE.md, "The air"): air temperature, humidity and
a gusting wind in `SessionConditions` (host-picked or from the weather,
named by the server on create and shown in the browser), the track
temperature from the air and the sun, the air's density from altitude
(every circuit's height from its elevation data), temperature and
humidity scaling the aero and the engines (a turbo keeps most of its
power), and the wind as the air the car drives through: drag, downforce
and a side force, felt differently on every straight. Left for later: a
humidity control, per-circuit latitude for the sun (stored, unused), a
drying or cooling day within a session, and the wind in the racing line's
plan (the AI reads it live).

**Was:** the environment is one enum. `SessionConditions` carries weather
and a clock; the weather is baked into the track's grip once at session
create (dry, light rain, heavy rain) and the time of day is visual only.
There is no temperature, no humidity, no wind — a summer noon race and a
cold dawn session are physically identical.

**Missing:**

- **Ambient and track temperature.** Air temperature for the session, and a
  track surface temperature derived from it, the sun (the clock and cloud
  cover already exist) and the weather. Track temperature is the natural
  input to the tire temperature model — it sets the warm-up rate and the
  operating window — and is the main reason to do this *after* tire
  thermals rather than before; without them it can still scale base grip
  modestly (a cold green track vs. a baking afternoon).
- **Air density.** Temperature, humidity and altitude set the density of
  the air, and density scales engine power, drag and downforce together —
  a cheap, fully deterministic effect that makes the same car genuinely
  faster on a cold morning. The track YAMLs know where their circuits are,
  so altitude is already implied by the DEM data.
- **Humidity.** Mostly an input to air density and to how damp a "drying"
  track feels; on its own a minor grip effect. Worth carrying on the wire
  from the start so the conditions struct only changes once.
- **Wind.** A session wind (speed, direction, mild gusting — deterministic,
  hash-based like the AI noise) entering the aero step as the relative air
  velocity: a headwind adds downforce and drag, a tailwind removes both
  into a braking zone, a crosswind pushes the car and asymmetrically loads
  it. Per-corner character falls out for free, because the same wind meets
  every straight at a different angle.

All of it extends `SessionConditions` (host-picked or auto from the
weather), is baked or evaluated server-side like the existing weather grip,
and needs the same integration care: the racing line and the AI profile
must read the same numbers, and the values belong in the session summary so
the browser can show "22°C · light wind NW".

**Difficulty: low–medium** for temperature/density/humidity (they are
session-constant scalars entering existing formulas); **medium** for wind,
which is a per-tick vector through the aero and needs the AI taught not to
be blown off line.

**Impact: medium on its own, high in combination.** Density and wind are
felt immediately (lap times, braking points, corner-by-corner balance);
temperature is what makes tire thermals a *management* problem rather than
a curve. Together they make every session slightly different, which is much
of what makes real racing inexhaustible — and none of it exists in most other 
sims either.

## Small inert fields worth resolving

`rolling_resistance` is parsed and never consumed (noted in
`car_setup.rs`); the differential model exists but every shipped car has
`simulated = false` because their lap times were tuned without one. Both
are cleanup-sized: either wire them in class by class (re-running the grip
probe lap-time calibration) or delete them, but a config field that looks
load-bearing and is not is a trap for car authors.

## Where to be better, not equal

Closing the list above makes ApexSim a credible multiplayer race simulation; it does not
give anyone a reason to switch. other sims single-car tire feel is a decade of
tuning plus the strongest modding community in the genre — the hardest
possible front to attack head-on. Its real weaknesses are structural,
baked into a client-authoritative, static-world architecture, and every
one of them lands on ground ApexSim already owns:

- **Authoritative contact physics.** Most sims resolve car-to-car contact on
  each client and reconciles positions afterwards, which is why online
  contact there is roulette — ghost taps that launch cars, collisions that
  resolve differently on each screen. ApexSim resolves every contact once,
  on the server, with real collision geometry. This is already built; it
  should be treated and sold as a simulation feature, not plumbing.
  *Wheel-to-wheel racing that works* is the single strongest reason to
  switch. Its one cost — the local car's input latency over the internet —
  has its own design: docs/CLIENT_PREDICTION.md.
- **Dynamic track evolution.** Most sims's grip is one static number per session.
  A track that rubbers in along the racing line as cars run, sheds grip
  off line (marbles), and dries along the driven line after rain is a pure
  function of car passes — deterministic, server-side, and a natural
  extension of the per-sample grip the weather bake already writes. The
  ground-truth state lives beside the centerline samples the physics
  already reads. Probably the highest-value "better, not equal" feature
  available: neither AC nor most of the market does it properly.
  Difficulty medium; the wire needs to tell clients enough to draw the
  darkening line (the `MI_wear_*` bands exist), and the AI/racing-line
  profiles need to read the evolving grip.
- **AI that races.** Most sims's AI is a fixed line with rubber-banding: trains,
  no real overtaking, hopeless in the wet. The ApexSim AI already runs
  class fields with a traffic layer and is surveyed on every circuit;
  investing in racecraft (using the tow once slipstream exists, defending,
  mistakes under pressure, recovering from a wall) compounds with every
  multiplayer-adjacent feature and is a differentiator most of the market
  lacks.
- **Live conditions.** Most sims have no rain, no night, no time progression;
  its community fills the gap with a mod stack that breaks on every
  update. ApexSim's weather and clock are first-class and server-baked.
  Transitional weather — rain arriving mid-race, a session running through
  dusk, combined with the drying line above — leapfrogs rather than
  matches.
- **Aero between cars.** Most sims's slipstream is crude and its dirty air barely
  exists, so the bar for "better calibrated" is low once the wake model
  from this list is built at all.
- **Low-speed and kerb behaviour.** AC's tire solver famously jitters at a
  standstill and can misbehave over aggressive kerbs. The mesh road
  contact with real curb heights is already a more modern foundation;
  making low-speed behaviour visibly clean is a quality win any AC driver
  recognises immediately.

One honest caveat: AC runs its local physics at 333 Hz, above our 240 —
the tick rate is not the advantage to claim. The advantage is *where* the
physics runs and what the world does around the car.

Strategy in one line: close the per-car gap to the point an AC driver
misses nothing in the first three laps, and win outright on the layer AC's
architecture cannot reach — authoritative contact, a track that evolves,
an AI that races, and weather that changes while you drive.

## Suggested order

Fuel mass first (self-contained, exercises the "AI must follow the physics"
plumbing on the cheapest possible feature), then tire temperature, then
slipstream, then aero ride-height/wing response — those four change the
driving and racing most per unit of work. Ambient temperature, air density
and wind slot in naturally beside tire temperature (they share the
conditions plumbing and the tires are what temperature is *for*). Wear,
compounds and pit stops are one package and come next; brake and engine
thermal ride on the temperature plumbing; progressive damage and hybrid
control are independent and can be slotted anywhere; suspension geometry
last. Of the "better, not equal" items, dynamic track evolution is worth
starting early — it is independent of the car-model work, and it is the
feature the others compound with.

## Not done yet (running list)

Everything a finished item above left for later, and what was built but
never checked on screen, in one place. Kept up to date as items land: a
line is struck off when it is done, and every new "left for later" goes
here as well as under its item.

**Fuel**
- The AI's and the racing line's speed profiles are built once, at the
  starting load; they are not rebuilt as the tank drains.
- No AI fuel saving when short (lift and coast).
- `[fuel] tank_front_share` exists; no shipped car sets one.

**Tyre temperature**
- The player's racing line is planned on warm tyres.
- Tyre heat is not yet an FFB or tyre-squeal cue.
- ~~The hypercars' front treads spike to ~150 °C somewhere each lap~~:
  the spike was the heat split (85% of the sub-peak slip power into the
  surface layer), fixed 2026-10-02; the hypercar's hottest tread at Monza
  is now 107 °C against a window topping at 118.
- The surface layer's constants (2 kJ/K, 350 W/K to the bulk, 30% of the
  hysteresis by mass) are set so the AI at Monza lands in each class's
  window and a careful corner costs a few degrees; nobody has checked a
  real tyre's surface swing through a corner against them.
- No inner/middle/outer temperatures (they need camber).
- Convective cooling reads the car's ground speed, not its airspeed, so
  a headwind does not cool the tyres.
- Following closely does not heat the tyres (no slide-heat link to the
  dirty air beyond the grip it costs).

**Slipstream**
- The AI does not pull out of a tow to pass: the traffic layer holds a
  follower at a following distance.
- The racing line is planned in clean air.

**Ride-height aero**
- Crests do not unload the car (the loads carry no vertical
  acceleration).
- No yaw or roll sensitivity, no porpoising.
- The steady-state map the racing line uses takes its heave at the
  reference air density (second order at altitude).
- The AI and the racing line plan on a new medium: a soft's extra grip is
  used through the AI's grip share (up to 3%), but the racing line shown
  to the player does not change with compound or wear.

**The air**
- No humidity control on the create screen (auto only).
- Latitude is stored per circuit but unused: the sun is still the sky
  model's 50° N for every track.
- Conditions are fixed for a session: no drying track, no cooling
  evening, no rain arriving.
- The wind is not in the racing line's plan (the AI reads it live).
- The client draws no wind (no windsock, flags or rain drift).

**Assetto Corsa imports**
- Kunos' 2015-17 F1 ground-height tables are nearly flat where those cars
  run, so they map to about zero ride-height sensitivity (faithful to AC,
  but it means those imports gain nothing from the aero map).
- AC's own tyre heat model (`FRICTION_K`, `SURFACE_TRANSFER`...) is not
  carried; only its grip-temperature window.
- Imported AC tracks have no altitude or position: they race at sea level.
- AC's dynamic aero controllers (wings moving with speed or throttle) are
  not modelled; a DRS flap is not split off an imported car's body.

**Tyre wear, compounds and pit stops**
- No flat spots from lockups, no punctures (wear stops at 100%).
- No wet or intermediate tyres: rain costs grip whatever is fitted.
- Compounds are the same three changes on every car; a car.toml cannot
  list its own compounds yet.
- Imported AC tracks have no pit sidecar, so no pit stops (AC's `AC_PIT_n`
  objects could give the boxes).
- A player can choose the next compound only in the hotlap garage (the
  setup is sent on joining any session); there is no in-race pit menu
  (compound, fuel, "tyres only").
- The AI's strategy is a threshold: no undercuts, no reaction to the
  cars around it, no fuel saving to skip a stop, no mandatory-stop rules.
- Boxes are shared by grid slot beyond the box count, not by team; no
  pit-lane speeding penalties (the limiter is automatic); the client draws
  no crew and no pit-lane time on the timing sheet.
- `initialize_content.ps1` does not check for the pit sidecar.

**Brake and engine heat**
- No brake wear, no brake bias or pad choice beyond the existing bias
  knob; the ducts are one knob for all four corners.
- No radiator (grille) setup knob; `[engine] radiator_scale` is car.toml
  only.
- The HUD colours the brakes by temperature alone: the client does not
  know whether a car's brakes are carbon or steel.
- The imported AC cars take carbon or steel from their class, not from
  AC's `brakes.ini` (which has no material, but its `[TEMPS_*]` could).
- The AI survey's F1 off-road time sits 8.5% over the pre-fuel baseline
  after brake heat (the field as a whole +3.5%, contact -3%): inside the
  per-class noise, but the one class trending up; worth a look at where
  (`SURVEY_DBG=1`) if it grows.

**Progressive damage**
- A retired car stops where it is: no tow-away, no retirement shown on the
  timing screens, and it stays an obstacle (its last contact/off-track
  state is what the survey counts for it).
- No engine wear over distance (only heat and over-rev damage).
- Damage is not an FFB, audio or visual cue: no bodywork falls off, the
  engine note does not change; the HUD's damage panel is the only readout.
- No damage for kerb strikes, bottoming out or airborne landings.
- The AI does not drive around its damage beyond its steering loop and
  the aero share: its speed profile still assumes full power.
- Damaged cars in the AC imports: their AI already crashes a lot, and one
  to ten cars a race now retire there.
- The damage aid (off / reduced / full) has no "visual only" level, and
  there is no visual damage for it to leave; the AI always takes full
  damage.

**Hybrid deployment**
- The racing line and the AI's plan count the motor's full power at every
  speed over its minimum, whatever the lap budget allows: the plan is
  optimistic for the F1s once the budget is spent.
- Regen under braking is still free energy (the brakes do all the
  stopping; no brake-by-wire split, no rear-brake correction).
- No per-stint energy (the WEC's real limit), no 2026-style manual
  override energy; the overtake button only overrides the pacing.
- AC's `ctrl_ers_*.ini` delivery profiles (speed, gear, throttle curves
  per mode) and `[FRONT_MOTORS]` are not carried; a front-axle-motor
  hypercar (919, TS040, R18) imports with its rear motor only.
- The AI never harvests or saves energy on purpose; it does not defend
  with the button.
- The mode resets to Balanced each race; it is not a saved preference.
- The survey's F1 slides 17% more than before (1 856 car-seconds against
  1 581) with less time off the road; SaoPaulo's F1 race now has a lap-1
  wall graze whose two damaged cars later retire into the walls.
- The HUD badge and the keys have never been seen in the running game.

**Built but never seen in the running game** (automation tests only)
- The HUD's tyre row and TOW badge.
- The hotlap garage's Aero section (wings, ride heights).
- The create screen's air temperature slider and wind row.
- The HUD's PIT badge and the tyre row's wear and compound; the garage's
  "Next tyres" row; an AI pit stop as the client draws it.
- The HUD's brake line and Water cell; the garage's "Brake ducts" row.
- The HUD's Damage cell.
