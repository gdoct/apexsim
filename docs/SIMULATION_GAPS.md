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
pressure, slipstream and dirty air, a simple hybrid deploy/regen, weather-baked grip, and collision
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

**Today:** `wear_rate` is parsed and inert. A tire is identical on lap 1 and
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

**Today:** one compound per car (front/rear scale factors exist for cars
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

**Today:** aero is three constants (drag, front lift, rear lift) plus DRS.
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

**Today:** braking is a constant force with a bias and ABS. Brakes never
heat, fade or wear, and there are no brake ducts.

**Missing:** per-corner brake temperature from braking energy, cooled by
speed; fade when overheated, weak brakes when cold; optionally wear and a
duct-size setup knob. Feeds the FFB and the HUD.

**Difficulty: low–medium.** Self-contained per wheel, similar shape to tire
temperature and best built on the same thermal plumbing.

**Impact: medium.** Mostly a management layer for endurance-style stints
and heavy-braking circuits; subtle in short races.

## Engine thermal, wear and progressive damage

**Today:** the engine is a torque curve with a limiter, turbo lag and an
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

**Today:** the hybrid deploys naively — full assist whenever the throttle is
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

**Today:** the environment is one enum. `SessionConditions` carries weather
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

*Since 2026-09-29 the air and track temperatures exist server-side*,
derived from the weather and the clock and baked into the session's track
(`SessionConditions::air_temperature_c` / `track_temperature_c`), because
the tyre model needed them; they are not on the wire, not host-pickable
and move nothing but the tyres.

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
