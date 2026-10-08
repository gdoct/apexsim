# ApexSim 0.2.4

Windows x64, built 2026-09-19 from `c51d575`. 10 cars, 26 circuits, 26 baked
track levels, server and client in one package: unzip and run `Play.bat`.

Thirty-one commits since 2026-09-15 (`e6e3ed5..c51d575`). The short version:
the sim grew a stopwatch, a garage and a sky; the cars grew an engine note and
wheels that turn; and the circuits stopped being a road with invented scenery
beside it.

---

## Driving and race control

**Lap timing that means something.** Three sectors a lap, split at stations
along the centerline (the track file's own `sectors` when it names two,
otherwise even thirds), timed at the full 240 Hz tick rate and sent as a
reliable message the moment a car crosses. The HUD paints a sector strip from
it — purple for a session best, green for a personal best, amber for slower —
and shows the session's fastest lap with a live delta against the quickest
*legal* lap it has seen.

**Track limits.** A lap is struck when all four wheels are off the track for
0.2 s, measured past the baked curb band, so using the kerbs is using the
kerbs and not a penalty. A struck lap is still timed and still counts as a lap;
it just cannot become a best. Cutting the course — reaching the line without
every checkpoint — strikes it too.

**Lap records, kept across restarts.** Your best legal lap per track per car
lives in `records/lap_records.json`, keyed by driver name (the player id is
minted per connection, so a UUID key would forget everything on reconnect).
You are told your record on joining and again whenever you beat it, along with
the track record and who holds it. AI never sets one.

**Hotlap mode**, in place of the old demo-lap tile on the create screen. Time
attack, and multiplayer if you want it: every driver starts in the garage —
car parked, not simulated, out of both collision passes — and GO OUT drops it
onto the centerline 300 m before the line in first gear, so the first flying
lap is timed as it crosses. A second car going out is queued behind the first.
The garage card holds a lap-by-lap timing sheet with the delta to your best,
and BACK TO GARAGE on the pause menu returns you, repaired, with your bests
intact.

**The ghost.** Every personal best now carries a 20 Hz trace of the lap that
set it. In a hotlap a ghost car runs your record lap against the clock of the
lap you are on, so it is where you were at this point of the lap rather than
just somewhere ahead. REPLAY BEST LAP drives it round in real time with the
camera cutting chase → trackside → onboard → trackside.

**The garage: a real setup screen.** Fourteen knobs — tyre pressures, rev
limiter, engine braking, final drive, gear spread, torque map, brake bias,
springs, dampers, anti-roll bars — edited as clicks off the car's own
definition and applied on the server, on the grid or mid-lap. Tyre pressure is
a new piece of physics rather than a number on a screen: each axle loses grip
quadratically away from its optimum, so raising one end's pressure loosens
that end and the knob is a balance tool. A stock setup simulates bit-identically
to before. Only knobs the sim actually reads are offered; there is no
differential model, so there is no differential page.

**Reverse gear.** You can back out of the gravel.

**Assists, and a host who can forbid them.** ABS, traction control (now OFF /
LOW / HIGH, where HIGH caps drive torque at what the friction circle has left
beside the cornering force, so full throttle on exit keeps the front end),
automatic gearbox, speed-sensitive steering and the racing line all live on an
Assists tab. The session's host picks which of them are allowed on the create
screen; the server pins a forbidden aid off whenever a car is seated and on
every request, whatever the client asks, and a session that forbids the racing
line simply never sends one.

**Weather and time of day.** The create screen picks a sky — sunny, cloudy,
overcast, light rain, heavy rain — and a time in quarter hours. The **server
owns the grip**: the weather is baked into that session's copy of the track, so
every centerline sample's grip, the curbs and the grass all move together and
physics, the AI and the racing line follow with no branch in the hot loop. Light
rain is 0.86 of dry, heavy rain 0.74; painted kerbs lose another 15–25%. On the
client the hour drives a real sun — elevation, azimuth, lux from the airmass and
cloud, colour warming toward the horizon — the fog takes the weather, the road
goes glossy and dark in the wet, headlights and tail lights come on under 6° of
sun, floodlights and lamp posts light up after dark, and rain falls as streaks
that stretch with your speed. The menu's demo race rolls its own sky each time.

**Chase cameras.** C no longer flips between two views: it steps down a ladder —
roof, close, near, far — and then back to the cockpit. Each rung clamps how far
the spring arm may stretch, because the arm's steady-state lag is nine metres at
320 km/h and without the clamp every rung looked like the far one down a
straight. The chosen distance is a settings row and survives the race.

---

## Sound

Nothing here is a recording. There are no audio assets in the project.

**The engine is simulated.** A crank turns at the telemetry's RPM; each
cylinder's firing angle puts an exhaust stroke into one of two banks, each a
weakly reflecting quarter-wave header; the banks meet in a collector and go
through a silencer, a tailpipe resonance and the outlet's low-end boom. The
note is the firing rate, so it is proportional to RPM with nothing to saturate,
and the character comes from geometry: a crossplane V8's L R L L R R L R order
puts power on the crank's own frequency and the odd halves under the note — the
burble — where a flat-plane V8 has six hundred times less. On top: overrun pops,
the limiter's spark-cut stutter, induction roar, a per-gear gearbox whine that
steps *up* on an upshift, and a turbo's whistle and blow-off. Each car describes
its engine in a `[sound]` table in its own `car.toml`.

**The mix.** Somebody else's car is a mono point in the world, full level only
within 4 m and falling to −50 dB at 150 m with air absorption on the way. Your
own car is a second, stereo, unspatialised engine run through a listener space:
a low shelf for weight, a bulkhead low-pass in a closed cabin, and a reverb sized
per seat.

**Tyres, kerbs, road and wind**, driven by the server's own force-feedback
signals. The tyres have their own thresholds from the raw per-wheel slip: silent
at the grip peak, because a car cornering well sits *at* its peak slip angle all
day, howling from 1.25× and full at 2.4×. A kerb is a rib every 0.9 m thudding
through a resonance, so its pitch is your speed. Off-track is rumble and stones;
a suspension hit is a thud and contact a crunch.

Engines and "Tyres and road" have their own volume sliders on the Audio tab.

---

## Circuits

The centerlines were always real. Everything beside the road was invented:
grandstands wherever there was room, a guessed pit-lane side, no landmarks, and
the same tree belt round every venue, dunes included. That is what this release
is mostly about.

**Real furniture, from OpenStreetMap, for 22 of the 26 circuits** (up from 5).
Each one has a layout dossier checked in beside its YAML holding the named
corners, the real pit lane and which side it is on, the grandstands with their
names and road-facing outlines, the buildings, the overhead crossings, the
landmarks, and the outlines of the real woodland with its leaf type — which is
what leaves Zandvoort's dunes bare, Spa in spruce and the Parco di Monza in
broadleaf. New this release: barriers, roads, waterways, car parks, camp sites,
farmland, villages, water and points of interest, laid as car parks with cars in
them, camp sites with tents, villages with houses, marshal posts at every named
corner, and a sign at each corner carrying its real name. Anything tagged within
250 m that no rule claimed is counted in the dossier, so the next circuit's gaps
show up in a file rather than in a screenshot. Added here: Sochi, Norisring,
Melbourne, Montreal, Sakhir and Budapest. Mexico City was attempted and refused
— the fit could not be explained, because the Foro Sol stadium section is
missing from OSM — which is the pipeline working as intended.

**Real elevation.** The ground used to be an inverse-distance average of the
road's own heights: a smooth blanket that decayed to the mean track elevation
and ended 800 m out in fog. It now comes from the Copernicus GLO-30 elevation
model, fitted onto the track's frame by the same fit the dossier uses, with the
surveyed centerline still winning within 40 m of a road (a 30 m surface model
reads the tree canopy and knows nothing of cuttings). Past the detailed ground,
the horizon is baked as real geometry rather than a painted backdrop — the sun
lights it, it takes the fog, it goes dark at dusk. At the Red Bull Ring that is
71k triangles of Murtal rising to 957 m, where there used to be a flat line.

**Barriers are decided, not inherited.** One pass walks every 4 m cell on both
sides of the circuit, so coverage is complete by construction, and chooses the
kind from the mapped barriers first and the geometry second — which side is the
outside of the bend, how tight it is, how much run-off there is. That gives
roughly 55% armco, 25% Tecpro and 15% tyres at the Red Bull Ring, with mesh
fencing wherever there are people behind it, concrete where a mapped wall stands
hard against the road, and a proper end cap closing every run. Every piece, cap
included, must be clear of the pit lane, clear of every fold of the course, and
not inside another prop.

**Smoothed centerlines.** Every real circuit is a GPS trace resampled to 5 m
nodes, and over a 5 m chord that trace's noise *is* curvature — Remus at the Red
Bull Ring came out at an 8 m radius on a 10.6 m road, which the exporter's loft
could not draw, so corners shipped with holes in them. A new pass filters node
positions and separately re-walks each pinched corner, capping the turn at a
node and spilling the excess to its neighbours so the corner's total turn is
untouched. Across the calendar the worst curvature jump drops from 0.03–0.09 per
metre to about 0.01, and every raceline stays inside the road edge.

**Ground textures.** Seven baked tiling sets — asphalt, grass, gravel, sand,
concrete, astroturf, kerb — sampled at two scales and mixed by a 20 m noise so
the repeat does not show, pushed to the coarse sample by 200 m so the far field
does not shimmer. Run-off bands now fray into the road at their road-facing edge
instead of meeting it as a line between two flat colours.

Also: the trees and their placement are better, the prop kit picked up a full
texture set, and Spielberg is dressed with it.

---

## Cars

The GT3 and LMP2 fleets were rebuilt from a shared builder library, and the
bodies had their wheels removed — because the client now draws four copies of
the class's shared wheel where the car's own `[wheels]` table puts them, steers
the front pair and rolls all four from the distance the car has actually
travelled. No more sliding on static discs. Every car also gained the `[sound]`
table the engine synth reads. The F1 and RB20 models are several megabytes
lighter for the same shape.

---

## Menus

A simulator settings screen with proper stepper rows, a focus frame that makes
it obvious what the keyboard or pad is pointing at, reworked car and track
cards, and menu sound on the new controls. The hotlap garage is its own layer
between the HUD and the pause menu, and takes the keys while it is up.

---

## Fixes

* **240 Hz on Windows, properly this time.** The game loop asks for a 1 ms
  system timer, but Windows 11 ignores that request while the process's window
  is minimized or occluded — which is exactly where a server console sits while
  the game is in front. The loop was dropping back to 64 Hz a few seconds after
  the window was hidden, and running the simulation at 27% of real time with lap
  times (counted in ticks) hiding it. The server now opts out of that power
  throttling before asking.
* **Monza's pit lane was twice its real length** (1485 m against 742.7 m): its
  OSM way is both directly tagged as a raceway *and* a pit-lane member of the
  circuit relation, and both code paths appended the same way with no dedup, so
  it was joined into a loop at double length. Found while re-validating the whole
  catalogue.
* **Albert Park's "pit lane" ran across the race track.** A hand-authored pit
  lane now wins over OSM outright instead of being a fallback; the untagged-way
  guess had matched a 711 m way through the circuit, and the bake stood pit walls
  in the middle of the road — the AI spent a quarter of every race against them.
* Hockenheim's degenerate Nordtribüne geometry dropped.
* The server's track directory scan no longer tries to load dossiers and OSM
  cache files as tracks.
* Melbourne, Montreal and Spielberg fit more accurately now that their circuit
  relations are used.

---

## Known limitations

The README's "Missing" list is shorter than it was — tyre and collision sound
now exist, and assists are settable and enforceable per session — but:

* Four circuits (Mexico City, IMS, Moscow Raceway, Yas Marina) have no layout
  dossier and are still dressed procedurally.
* Le Mans fits partially: two thirds of the Sarthe is public road in OSM rather
  than a tagged raceway.
* No H-pattern shifter support on a wheel — the wire protocol carries a shift
  delta, not an absolute gear — and no soft lock or rotation setting.
* No server-side player accounts or persistence; lap records are kept by the
  server you connect to, keyed by driver name.
* AI skill still cannot be set per session, and the AI is sloppy enough at some
  circuits that most of its laps fall foul of track limits.

---

## Package contents

```
ApexSim-0.2.4-Win64/
├── Game/            packaged client, plus settings.sample.yml
├── Server/          apexsim-server.exe, server.toml, the car and track data it reads
├── Play.bat         starts both
├── Start-Server.bat
├── README.txt
├── LICENSE
└── release.json     version, commit, build time, content counts
```

Edit `Game/settings.yml` (created on first run, seeded from your desktop's
display mode) for resolution, window mode, vsync, frame limit and the server
address. Everything else is in the in-game settings overlay.

Built with `./scripts/build_release.ps1 -Zip`; see the main
[README](../README.md) for building from source.
