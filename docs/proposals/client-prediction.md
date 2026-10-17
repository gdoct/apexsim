# Client-side prediction for the local car

Status: phases 1 (the library), 2 (the wire) and 3 (the loop, off by
default) built; phase 4 proposed. Whether it feels right over real latency
is for a driver on a wheel ([verification.md](../verification.md)).

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
- `DriverFeedback` (`server/src/feedback.rs`) is per driver and per telemetry
  tick, carries `server_tick`, torque samples and the column slope, and for
  a client that numbers its inputs the own car's snapshot and slices of its
  full state (phase 2, below).
- Telemetry is serialized once per session and fanned out
  (`game_loop/broadcast.rs`), so it cannot carry anything per recipient.
- The server crate is an rlib (`src/lib.rs`) plus two binaries. The car
  step is also a static library behind a C API, `server/ffi` (phase 1,
  below), which the client links (phase 3).
- The client has every car.toml (it builds cars from them), its own setup
  clicks and `SessionConditions`. A track's YAML and physics sidecars
  (`road`, `curbs`, `walls`, `ground`, `pit`) it has only in a checkout
  (`content/tracks`, read by the editor build); a release ships them in
  `Server/` only, so a packaged client does not predict until phase 4.

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

Options, in order of preference:

1. **A physics bundle served by the server** on join (msgpack keyed by
   `track_id` and `ContentCrc`, cached on disk): one source of truth, covering
   custom and imported tracks.
2. **Ship the sidecars in `Game/Tracks/`**: simpler, duplicates content, no
   help for a server's custom track.

Either way the content checksum answers "is my copy the session's copy"; a
mismatch disables prediction for the session.

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
4. Then the track physics bundle served on join (keyed by `ContentCrc`, so
   a packaged client and a custom track predict), force feedback
   (replacing the `steer_stiffness` correction), the own-car engine sound
   and the HUD read the predicted state.

## Phase 1: the library (built)

`server/ffi` is the crate `apexsim-physics`: a static library (and an rlib
for its own tests) on the server crate, so the server builds as before. How
to build it for Windows x64 MSVC and what it links against is in
[building.md](../building.md#the-physics-library-serverffi).

**One step.** `car_step::step_car` (server, `src/car_step.rs`) is one car's
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
| `apx_track_free`, `apx_car_free` | |

Opaque handles, `ApxStatus` codes, `catch_unwind` around every call. A
track handle is changed by each step (the wind), so a track and its cars
belong to one thread at a time. `ApxInput` is `PlayerInput`'s fields
flattened (28 bytes); `ApxCarPose` is 96 bytes. `APX_ABI_VERSION` is 2
(the snapshot and road calls).

**The check** (`server/ffi/tests/lone_car_trace.rs`, in CI): Monza, an
F1 car with a tuned setup, wind; a recorded 4400-tick trace (standing
start, shifts, weave, DRS, overtake, a hard stop with the handbrake, then
off the road over the grass) driven through a free-practice `GameSession`
with one car and through the extern functions, the C car seeded from the
session's through the blob. The full state is compared every tick, bit for
bit, except the fields the session's passes around the car own (pit lane,
DRS arming, wake, racecraft, the road state's bookkeeping, the hit record
debris reads, headlights). The road state is the session's world: the test
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
- Linking: the `.lib` carries the whole server crate (tokio, rustls ...),
  about 10 MB in release; the linker drops what the car step does not
  reach. A physics-only crate split out of the server would shrink it.

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
is generated from the server's source, and 80 MB). It linked into the
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

**Not predicted** (the puppet is drawn): the countdown and after the flag,
the pit lane (its limiter and autopilot are session passes), the garage, a
recovery, a car the server drives (the stand-in, an away driver; drawn with
`apexsim.net.PredictServerDriven 1` for unattended checks), and a packaged
game (no track YAML).

**What phase 4 takes from it.**

- The track physics bundle on join: until then only a checkout predicts,
  and only on tracks whose YAML and sidecars match the server's (the
  checksum covers the YAML, not the sidecars).
- Force feedback and the engine sound read `ApexPhysics::FCar` states the
  loop already has per tick (`GetPoseAtTick`); the HUD's own-car figures
  could read the predicted pose.
- The `.lib` carries the whole server crate; a physics-only crate would
  shrink it.

## Risks

Rust-in-UE build plumbing (done once); a new physics config field the FFI
context cannot supply is a silent misprediction (the phase-1 test guards
it); custom tracks need the physics bundle. No cheating surface is added:
the server still simulates from raw inputs.
