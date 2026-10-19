# Client-side prediction for the local car

Status: built, phases 1 (the library), 2 (the wire), 3 (the loop, off by
default) and 4 (the feel, the sound and the HUD on the predicted car; the
track's physics bundle served on join). Whether it feels right over real
latency is for a driver on a wheel ([verification.md](../verification.md)).

## The problem

Every car, the player's included, is a puppet of the telemetry
([client.md](../game/client.md#car-motion-buffer-raceapexcarmotionh)), so
each input makes a round trip before the driver sees it: ping, the 60 Hz
telemetry (420 Hz sim, every 7th tick), the motion buffer's two-frame delay
(about 33 ms) and a render frame. At 40-80 ms of ping that is 90-150 ms from
wheel to car, which a driver used to a local sim feels within a lap. Force
feedback already compensates (`DriverFeedback.steer_stiffness` /
`steer_input`, [input-and-ffb.md](../game/input-and-ffb.md)). Remote cars
are fine on the motion buffer; only the local car, whose inputs the client
knows first, needs prediction.

## Current state (checked against the code)

- `ClientMessage::PlayerInput` carries `server_tick_ack`, the controls and
  the client's number for the input (`seq`, phase 2).
- `DriverFeedback` (`sim/src/feedback.rs`) is per driver and per telemetry
  tick, carries `server_tick`, torque samples and the column slope, and for
  a client that numbers its inputs the own car's snapshot and slices of its
  full state (phase 2, below).
- Telemetry is serialized once per session and fanned out
  (`game_loop/broadcast.rs`), so it cannot carry anything per recipient.
- The simulation is its own crate (`apexsim-sim`, `sim/`), which the
  server crate (an rlib plus two binaries) links and re-exports. The car
  step is also a static library behind a C API, `server/ffi` (phase 1,
  below), on the simulation crate alone, which the client links (phase 3).
- The client has every car.toml (it builds cars from them), its own setup
  clicks and `SessionConditions`. A track's YAML and physics sidecars
  (`road`, `curbs`, `walls`, `ground`, `pit`) it has only in a checkout
  (`apexsim-content/tracks`, read by the editor build); a release ships them in
  `Server/` only, so a packaged client, or any client on a server's own
  track, gets them from the server (phase 4).

## Principle

Predict alone, reconcile continuously: the client steps its own car ahead by
about the ping and folds server states in as they arrive. The predicted pose
is presentation only; any disagreement resolves to the server.

## One physics, not two

A simplified client car model would drift forever and every physics feature
([vehicle-physics.md](../server/vehicle-physics.md)) would land twice.
Instead, compile the server's physics into the client: the sim is
deterministic (fixed dt, no wall clock, no RNG, `tests/determinism_test.rs`),
so the crate can be a static library behind a small C FFI, linked as a
prebuilt third-party lib.

The FFI surface: create a context (car config, setup clicks, session
conditions and the grip bake they imply, track data), step one car one tick
from an input, query the surface under a point. State crosses as plain data.
The weather bake is deterministic from `SessionConditions`, so the library
exposes the same bake the server runs.

Bit equality between the builds is a target, not a correctness need
(reconciliation absorbs drift), but near-zero correction is what makes
prediction invisible, so assert it.

## Track data on the client

A physics bundle served by the server (phase 4, below): one source of
truth, covering custom and imported tracks, rather than shipping the
sidecars in `Game/Tracks/` (duplicated content, no help for a server's own
track). Its checksum, over the YAML and every sidecar, answers "is my copy
the session's copy".

## Wire changes

Appended fields with golden bytes ([protocol.md](../server/protocol.md#the-own-car-for-prediction)),
built in phase 2 (below):

- `PlayerInput.seq`: the client's number for each input.
- `DriverFeedback.own_car`: the own car at the message's tick and the newest
  input number applied by then; `DriverFeedback.own_state`: a slice of the
  car's full state blob, a whole blob twice a second.

An old client ignores both; with an old server prediction never turns on.

## The loop

- A fixed-step accumulator at the server's tick rate, stepping the local car
  with the server's dt and interpolating the sub-tick remainder for
  rendering. Fixed step is what keeps the replay bit-compatible.
- On each snapshot: rewind to it, replay the buffered inputs newer than its
  sequence (at 100 ms ping about 42 ticks of one car), compare with the
  shown pose, and decay the difference as a correction offset over a few
  hundred ms. A gross disagreement (the motion buffer's 20 m rule) re-seats.
- ABS, TC, the auto gearbox and the steering aid are inside the car step, so
  they are predicted for free. Server-decided flags (DRS allowed, headlights)
  are held at their last known value.

## Not predicted

Other cars (motion buffer); car-car contact (the local step has road and
walls but no other cars, so a hit arrives as a blended correction); race
control (laps, sectors, track limits, records, grid seating, hotlap
relocation, which snaps by design).

## Phasing

1. **Library** (built, below): the car step as a staticlib behind a C API.
   A CI test replays a recorded input trace through a server session and
   the C API and asserts equal states.
2. **Wire**: input sequence and feedback snapshot, golden bytes both sides.
3. **Loop** (built, below): the library linked into the client as a
   prebuilt third-party module, the loop behind a console switch, an
   artificial-latency harness, automation tests on a recorded server
   sequence (loss, a contact correction, a relocation).
4. **The feel and the bundle** (built, below): force feedback, the own
   car's engine and road sound and the HUD's dash read the predicted
   state; the track's physics bundle served on request after the join, so
   a packaged client and a server's own track predict.

## Phase 1: the library (built)

`server/ffi` is the crate `apexsim-physics`: a static library (and an rlib
for its own tests) on the simulation crate (`apexsim-sim`), the code the
server runs, without the server's runtime. How
to build it for Windows x64 MSVC and what it links against is in
[building.md](../building.md#the-physics-library-serverffi).

**One step.** `car_step::step_car` (sim, `src/car_step.rs`) is one car's
tick as every session mode runs it: `update_car_3d`, a hook (the session
samples its ghost trace there), `update_track_progress_3d`.
`car_step::step_lone_car` is that plus what the contact passes do to a car
alone on the track: `physics::clear_contact` (the car-car pass with no other
car) and `physics::collide_with_walls` with the car's shared config (as the
session's wall pass reads it). The session's tick loops call `step_car` and
the contact passes call `clear_contact` / `collide_with_walls`; the C API
calls `step_lone_car`. The wind of the tick is `car_step::air_for_tick`, the
session's own `update_wind`. Everything else in the session's `update_air`
(pit lane, DRS, wake, the road state's passes, debris and punctures,
recoveries, racecraft) stays the server's and reaches the client as state.

**The C API** (`server/ffi/apexsim_physics.h`, hand-written, its sizes and
names checked by a test against `src/lib.rs`):

| Function | Does |
|---|---|
| `apx_abi_version`, `apx_state_blob_version`, `apx_last_error` | versions; the last failure's message on the thread |
| `apx_track_load(yaml, road_contact, conditions, len, &track)` | the track and its sidecars, with `game_session::session_track`: the resolved `SessionConditions` (MessagePack, named, as `SessionJoined` carries them) baked in and the road state laid, as the server makes a session's copy |
| `apx_car_create(track, car_toml, len, clicks, n, grid_slot, &car)` | `CarLoader::load_from_str`, the setup clicks clamped and baked as `SetCarSetup` does (`CarSetup::simulated_config`), a fresh state on the grid slot |
| `apx_car_step(car, track, &input, tick, tick_rate_hz)` | the input sanitised as the server's dispatch does (`PlayerInputData::sanitized`), the tick's wind, `step_lone_car`; `dt` is `1 / tick_rate_hz` computed as the server computes it, which is why the rate is passed and not a `dt` |
| `apx_car_pose(car, &pose)` | `ApxCarPose`: position, attitude, velocities, yaw/pitch/roll rates, inputs, rpm, progress, lateral offset, wheel speeds, lap, gear, flags (on track, airborne, colliding, DRS open) |
| `apx_car_state_save` / `apx_car_state_load` | the full state as a versioned blob (`car_step::encode_state`: `APXC`, version u16, then the state and the runtime fields serde skips, MessagePack with named fields, floats exact); refused across versions or cars. The server's blob from `DriverFeedback.own_state` loads as it is |
| `apx_car_snapshot` / `apx_car_snapshot_apply` | `ApxCarSnapshot` (192 bytes): the server's `OwnCarSnapshot` without its tick and input number; apply lays it over the car's state (`OwnCarSnapshot::apply_to`), the rest kept; a non-finite number is refused |
| `apx_track_road_rows(track, first_cell, rows, snow_rows, lane_snow)` | the track's road state held to the server's from a `RoadState` message's bytes (`RoadState::read_rows`, `read_snow_rows`, `read_lane_snow`), at the wire's byte resolution; empty snow rows stop the snow counting, as the server's field left off means |
| `apx_car_feedback_take(car, &feedback)` | `ApxFeedback` (212 bytes): what the driver feels over the steps since the last take, from the server's own collector (`CarState::feedback`, `FeedbackAccumulator::take`, as the game loop drains it into `DriverFeedback`); taken after every step it is that tick's, one torque sample (phase 4) |
| `apx_track_free`, `apx_car_free` | |

Opaque handles, `ApxStatus` codes, `catch_unwind` around every call. A
track handle is changed by each step (the wind), so a track and its cars
belong to one thread at a time. `ApxInput` is `PlayerInput`'s fields
flattened (28 bytes); `ApxCarPose` is 96 bytes. `APX_ABI_VERSION` is 3
(2 the snapshot and road calls, 3 phase 4's feedback and track bundle).

**The check** (`server/ffi/tests/lone_car_trace.rs`, in CI): Monza, an
F1 car with a tuned setup, wind; a recorded 4400-tick trace (standing
start, shifts, weave, DRS, overtake, a hard stop with the handbrake, then
off the road over the grass) driven through a free-practice `GameSession`
with one car and through the extern functions, the C car seeded from the
session's through the blob. The full state is compared every tick, bit for
bit, except the fields the session's passes around the car own (pit lane,
DRS arming, wake, racecraft, the road state's bookkeeping, the hit record
debris reads, headlights), and the feedback the step collects
(`apx_car_feedback_take` against the session's collector drained each
tick) bit for bit. The road state is the session's world: the test
hands the session's to the library's track each tick, as a client will hold
the server's. A race event the session raises (debris shed or picked up, a
puncture rolled) is a correction: the C car is reseeded and the comparison
goes on; any other difference fails. Run on the centerline (manual gears)
and on the road mesh (auto box, steering aid, ABS, TC high), with the
sidecars where baked (wall contacts included) and from the YAML alone, as
CI has it. A second test saves the state, steps 400 ticks, loads it and
replays them bit for bit.

**What phase 3 takes from it.**

- A rewind sets the server's state two ways: the whole blob from
  `DriverFeedback.own_state` (`apx_car_state_load`, exact, twice a second)
  and the snapshot from every `DriverFeedback.own_car` laid over the
  client's own state of that tick (`apx_car_snapshot_apply`; phase 2's
  figures below).
- The road state is the session's world; the client gets it quantized
  (`RoadState` rows) and holds the library's track to it
  (`apx_track_road_rows`): a rubbered, marbled Monza grips within 0.0003 of
  the server's from the bytes, where the road as loaded is 0.07 off
  (`the_road_from_the_wire_grips_as_the_servers`). The live sky's other
  figures (`rain`, `snowfall`, the asphalt's temperature) only drive the
  road's own step, which the client does not run.
- The tick the client steps is the server's `current_tick` for that tick
  (the wind and the lap timing read it); the rate is the session's.
- Linking: the `.lib` carries the simulation crate (`apexsim-sim`) and its
  few dependencies, no network or TLS code; the linker drops what the car
  step does not reach.

## Phase 2: the wire (built)

**The input's number.** `PlayerInput.seq` (`Option<u32>`, left off when
`None`). The client's UDP worker stamps every input it sends, from 1
(`FApexUdpConnection::SendPlayerInput`, `GetLastInputSeq`). The game loop
holds each driver's newest input with its number (`game_loop::HeldInput`)
and applies it every tick until the next replaces it, recording its number
on the driver's `own_car::OwnCarFeed` each tick it drives the car (not
while a stand-in or the away driver has it). The number never reaches the
physics, so a session steps exactly as before (`determinism_test`, the AI
survey unchanged). One input applies to several ticks (60 Hz of inputs, 420
Hz of ticks), so phase 3 replays its inputs at its own tick timing from the
snapshot's tick; the number only says which inputs the server had.

**What a rewind needs.** Phase 1 found that pose and velocities leave the
rest of the state to drift. Two questions decide the snapshot: what the
step integrates from one tick to the next, and how fast the rest moves.
`server/ffi/tests/lone_car_trace.rs`
(`a_snapshot_over_a_stale_state_replays_with_the_server`) measures it on the
recorded Monza trace (F1 car, aids, road mesh, wind): at 19 points on the
trace, a car whose whole state is the server's of 1 s earlier, with the
snapshot of now laid over it, is stepped 42 ticks (100 ms, a ping's worth)
beside the server, and the position error read:

| Laid over a 1 s stale state | Max error after 100 ms | After 400 ms |
|---|---|---|
| nothing (all 1 s stale) | 54 m | |
| pose and velocities only | 25 mm | 411 mm |
| the snapshot | 0.8 mm | 12.5 mm |
| the full blob | 0 (bit for bit) | 0 |

With the state 0.5 s stale (the blob's period) the snapshot leaves 0.55 mm
after 100 ms and 2.3 mm after 200 ms. Taken one field at a time, a 1 s stale
copy of the tyres costs 0.4 mm summed over all 19 points, the hybrid's
deployment 1.3 mm, the fuel less than a micron; the position, heading and
velocities are metres, and the next tier (vertical acceleration, gear,
ground slope, suspension, g, wheel loads) millimetres to centimetres.

**Decision: (a), a compact snapshot every message plus the full blob in
slices at 2 Hz.** The snapshot (`own_car::OwnCarSnapshot`, 26 fields, 267
bytes) is what the step integrates tick to tick: pose, velocities, wheel
spin, wheel loads, suspension travel and speed, g, vertical acceleration,
ground slope, porpoising, the controls as applied, gear, rpm, turbo, the
auto box's timers, the place on the lap and the centerline search's start
point, and the contact flags. `OwnCarSnapshot::apply_to` lays it over a
state; `of` reads it. The slow state (tyre temperatures and wear, fuel,
hybrid, brakes, damage, lap timing) comes exact in the `APXC` blob, which at
about 5.5 kB is too big for one unfragmented datagram: it goes as 640-byte
slices, one per feedback message, 9 messages of every 30, each slice naming
the blob's tick and input. Rejected: the whole blob every message (5.5 kB,
fragmented, 330 kB/s); a positional or deflated blob every message (still
about 2 kB, and a second state format beside the library's); pose and
velocities alone (25 mm in 100 ms, 0.4 m in 400 ms).

**Sizes** (`network.rs` `own_car_feedback_fits_one_small_datagram`, every
number at its widest, 7 torque samples at 420 Hz / 7): the feedback alone
158 bytes, with the snapshot 425, with a slice 1,087. A 1,500-byte MTU
carries 1,472 bytes of UDP payload, so no feedback datagram is fragmented;
the client's receive buffer is 64 KiB (`ApexUdpConnection::MaxDatagramBytes`),
and the 13-car telemetry trap ([protocol.md](../server/protocol.md#traps))
is the telemetry datagram's, which this does not change. A predicting
driver costs about 16 kB/s of snapshots and 11 kB/s of state on top of the
feedback's 9.5 kB/s. The snapshot and slices go only to a driver whose
inputs carry a number; anyone else gets the feedback byte for byte as
before.

**The client** decodes both (`FApexOwnCarSnapshot`, `FApexOwnCarStateChunk`
on `FApexDriverFeedback`), takes each message's off before a frame's
messages are folded together, and keeps the newest snapshot and the newest
whole blob on `UApexNetSubsystem` (`GetOwnCarSnapshot`, `GetOwnCarState`,
their serials; `FApexOwnCarStateAssembler` drops a blob a slice of which
was lost). Nothing reads them yet.

**Checks:** golden bytes both sides (`player_input_seq_wire_format`,
`driver_feedback_own_car_wire_format` into `ApexUdpGoldenBlobs.h`;
`ApexSim.Net.Udp.GoldenEncode` / `.GoldenDecode` / `.OwnCarStateAssembler`);
`protocol_test::test_input_sequence_comes_back_with_the_own_car_snapshot`
(over the real UDP path: an unnumbered input drives with no snapshot; each
snapshot carries the throttle of the input it names as applied; the slices
make a blob that decodes).

## Phase 3: the loop (built)

How to build and link the library, and the switches, are in
[building.md](../building.md#the-physics-library-serverffi) and
[client.md](../game/client.md#client-prediction-raceapexpredictionsubsystemh);
this is what was decided on the way.

**Linking.** `game-unreal/Source/ThirdParty/ApexPhysics`, an external
module: the header where it lives (`server/ffi`), the `.lib` from
`scripts/build_physics_lib.ps1`, built on demand and never checked in (it
is generated from the simulation's source, and 36 MB). It linked into the
editor's `UnrealEditor-ApexSim.dll` with no clash. A client without it
builds with `WITH_APEX_PHYSICS 0`, and `ApexPhysics::IsAvailable` says why.

**What the server had to add.** The client knew neither the session's tick
rate (the motion buffer estimates it) nor which road the server drives, so
`SessionJoined` carries them (`Physics`, [protocol.md](../server/protocol.md#the-own-car-for-prediction)).
The conditions go to the library as the bytes the server wrote, so a field
the client's struct does not hold still bakes as on the server.

**Timing the inputs** turned out to be the loop's real problem. The server
holds each 60 Hz input until the next arrives, so a replay is right only
if each input lands on the tick the server first applied it at; a few
ticks off is tens of millimetres at a brake stab 100 ms deep. Three things
put it there (`ApexSim.Prediction.Loop.AtPing` at 80 ms: the largest
correction went 41 -> 27 -> 3.5 mm, p95 0.9 -> 0.7 mm):

- the clock is timed by when the UDP worker took a snapshot in, not by the
  frame that read it (a 16 ms frame was most of the error);
- an input is placed at the clock of its sending, and one heard after its
  ticks were stepped is written into them and they are stepped again;
- the snapshot's input number against the ring's at that tick trims the
  clock's target (an early or a late timeline shows as a different input),
  which also absorbs the round trip's bias: the round trip is only known to
  within an input interval.

**The recorded sequence.** `server/ffi/tests/client_fixture.rs` writes
`ApexPredictionFixture.h`: a free-practice session at Monza from the YAML
alone (as CI and a fresh clone have it), the F1 car with aids and a tuned
setup, numbered 60 Hz inputs, the state blob at the window's start and a
snapshot every telemetry tick for 2 s. The library stepped from the blob
with those inputs on the road as loaded lands within 1.4 mm of the
server's (which lays rubber as it goes). A physics change or an edit to the
YAML or the car regenerates it; the tests name the command.

**Live, unattended** (Monza, the F1 car, `-ApexFakeLatencyMs=80`, the
debug stand-in driving with `apexsim.net.PredictServerDriven 1`, two laps,
230 s): rewinds on 57 snapshots a second, 47 ticks deep on average (129 at
most, a whole blob's), the clock 45 ticks (106 ms) ahead of the newest
snapshot, the car drawn from the prediction on 99% of frames, the puppet
6.0 m (143 ms) behind it. Corrections: median 1.5 mm, mean 9 mm, the worst
0.27 m, one re-seat and one clock jump in the run. Those are the stand-in's:
its inputs reach the client only as the controls it applied, which the
replay holds, so a lift or a turn-in is a correction; a driver's own inputs
are replayed as sent. In shadow (`apexsim.net.Predict 2`, the puppet drawn)
the same run gave the same figures (median 1.5 mm, mean 9 mm, 0.24 m worst,
the puppet 144 ms behind), so drawing the prediction changes nothing it
measures. To see a driver's figures needs a driver: until one has, they
are the automation test's.

**Not predicted** (the puppet is drawn): the countdown and after the flag,
the pit lane (its limiter and autopilot are session passes), the garage, a
recovery, and a car the server drives (the stand-in, an away driver; drawn
with `apexsim.net.PredictServerDriven 1` for unattended checks).

## Phase 4: the feel and the bundle (built)

**The feel from the server's collector.** The force feedback had to come
from the server's own code, not a client copy. It does without a new
seam: `physics::update_car_3d` and the wall pass already record each tick
into `CarState::feedback` (`FeedbackAccumulator`), which the library's car
carries, so `apx_car_feedback_take` drains it exactly as the game loop
drains it into `DriverFeedback` (`FeedbackAccumulator::take`), into
`ApxFeedback` (the message's fields at the wire's units, up to 32 torque
samples). The loop takes it after every step and keeps it in the ring
with the tick; a frame folds the ticks the clock reached since the last
one with the client's own `FApexDriverFeedback::Absorb`, the fold it
already applies to a frame's messages, and `MakeSignals` and both mixers
read the result unchanged. `lone_car_trace` checks the library's
feedback against the session's collector every tick, bit for bit (4,400
ticks on the centerline and on the road mesh with aids, over grass and
into a wall); from the server's recorded state the client folds every
`DriverFeedback` of the fixture's window again to within 0.0000
(`ApexSim.Prediction.Feel.ReplaysTheServer`). At 80 ms the torque is felt
79 ms before the server's message for the same tick, 0.006 of the
reference torque apart on average and 0.03 at p95 (an input that lands a
tick off at a weave); the revs lead by the same, 17 rpm apart at p95 (an
upshift a tick early is a moment's 3,000) (`Feel.AtPing`). Because the
torque is worked out at the input being sent, the `steer_stiffness`
correction is left with the input's movement within a frame. What stays
the server's: the car's condition (tyre heat, damage, the ground under the
wheels: telemetry, moving over laps), and on the HUD the lap, places,
gaps, radar and the station, which are read against the other cars.

**The sound and the dash.** `GetOwnCar` hands the actor the predicted
revs (blended across the sub-tick, as the motion buffer blends the
puppet's), throttle, brake and gear; the engine synth, the cockpit dash
and the brake lights follow, and `ApexHudData::ApplyPredicted` puts them
under `car.speed*`, `car.rpm*`, `car.gear*`, `car.throttle`, `car.brake`,
`car.steering`. The tyre and kerb voices read the predicted feel.

**Neutral when off.** Every reader takes the predicted source only while
the subsystem draws the car (`apexsim.net.Predict 1`, racing, not in the
pit lane...); otherwise the code path, the message and its serial are the
server's as before. A switch of source is not a new message, so a hit is
never played twice.

**The bundle.** What the step reads of a track is its YAML and five
sidecars: ground, curbs, walls, pit lane, and the road mesh when the
session drives on it. Sizes (main checkout, `print_bundle_sizes`):

| Circuit | Files | Road mesh | Deflated | Chunks of 256 KiB |
|---|---|---|---|---|
| Monza | 5.33 MB | 4.21 MB | 2.61 MB | 10 |
| Spa | 6.67 MB | 5.39 MB | 3.31 MB | 13 |
| Le Mans | 14.54 MB | 10.00 MB | 6.86 MB | 27 |
| Nordschleife | 20.33 MB | 13.20 MB | 10.24 MB | 40 |
| Suzuka | 5.88 MB | 4.79 MB | 2.91 MB | 12 |
| Norisring | 1.74 MB | 1.37 MB | 0.92 MB | 4 |

- *Transport: TCP, asked for a chunk at a time after `SessionJoined`.*
  Megabytes rule out putting it in `SessionJoined` (the client takes 4 MiB
  frames, and the join would wait) and pushing it unasked (most clients of
  a checkout hold the files already, and every join would send 10 MB). The
  health port's HTTP side was the other option: it is plaintext, often
  bound to loopback (`health_bind`), and opt-in for recordings, so a
  client could not count on it. Over the game's own TCP the bundle comes
  through TLS, with no new port, and a client pulling a chunk at a time
  (3 in flight, 6 a second after a burst of 10, under the server's 10
  control messages a second) never has more than a chunk queued ahead of
  the session's own messages: Monza comes in about a second, the
  Nordschleife in 5 s. The join is not held: the puppet is drawn until it
  is there.
- *Checksum: a new one over all the files* (`TrackFiles::checksum`): the
  YAML's content checksum (carriage returns dropped, as `ContentCrc`) and
  each sidecar's CRC-32 over every byte, with its presence. `ContentCrc`
  stays the YAML's: the lobby, the catalog rows and the race director
  compare it with the level the client draws, which a sidecar does not
  change.
- *The bytes are the ones the server drives on*: `track_content` reads the
  files once, loads the sidecars from those bytes
  (`TrackLoader::load_sidecars_from`) and deflates them; a YAML changed on
  disk since the catalog read it is not served. The library loads a bundle
  from bytes (`apx_track_load_bundle`, refused unless its checksum is the
  one the join named) through the same loader; `apx_track_files_crc` gives
  a client's own files' checksum, and a car on a bundle steps as on the
  files, bit for bit (`server/ffi/tests/track_bundle.rs`,
  `ApexSim.Prediction.Bundle`).
- *The car.toml is not bundled*: a car the client has no folder for it
  cannot draw either (no mesh), and a car.toml of another version is
  caught by its own `ContentCrc`; prediction stays off then.

**Live, unattended** (Monza, the road mesh, the stand-in driving with
`apexsim.net.PredictServerDriven 1`, `-ApexFakeLatencyMs=80`, a client
with no Monza files, `-ApexContentDir` empty, 240 s): the join named
bundle `3AE5C09A`; the client found no files and no cache, asked for it,
and had 2.61 MB in 10 chunks 0.20 s later; the track was loaded 0.07 s
after that and the car seated on the server's state 0.7 s after the join.
Drawn from the prediction on 98% of frames. The steering torque was felt
112 ms before the server's message for the same tick (14,045 samples),
the revs 108 ms before; 0.07 of the reference torque and 114 rpm apart on
average, because the stand-in's inputs reach the client only as the
controls it applied (the replay holds them, so a lift or a turn-in is a
correction: the largest 0.21 m); a driver's own inputs are replayed as
sent, which the automation tests measure (0.006, 17 rpm at p95).

**The library's size.** The `.lib` links the simulation crate
(`apexsim-sim`: the car, the track, the race and the session tick) and not
the server's runtime (tokio, rustls, hyper, ring): 36 MB with thin LTO
(a staticlib carries every dependency's objects whatever the LTO, the
standard library's among them), and beyond the standard library's own
system libraries it needs none. The client's linker keeps what the C API
reaches ([architecture.md](../architecture.md#simulation-and-runtime-modules)).

## Risks

Rust-in-UE build plumbing (done once); a new physics config field the FFI
context cannot supply is a silent misprediction (the phase-1 test guards
it); custom tracks need the physics bundle. No cheating surface is added:
the server still simulates from raw inputs.
