# Client-side prediction for the local car

Status: phase 1 (the library) built; phases 2-4 proposed.

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

- `ClientMessage::PlayerInput` carries `server_tick_ack` and the controls;
  there is no per-input sequence number.
- `DriverFeedback` (`server/src/feedback.rs`) is per driver and per telemetry
  tick, carries `server_tick`, torque samples and the column slope, but no
  pose or velocities.
- Telemetry is serialized once per session and fanned out
  (`game_loop/broadcast.rs`), so it cannot carry anything per recipient.
- The server crate is an rlib (`src/lib.rs`) plus two binaries. The car
  step is also a static library behind a C API, `server/ffi` (phase 1,
  below); the client does not link it yet.
- The client has every car.toml (it builds cars from them), its own setup
  clicks and `SessionConditions`, but only the render export of a track: the
  physics sidecars (`road`, `curbs`, `walls`, `ground`, `pit`) ship in
  `Server/` only.

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

Both are appended fields with golden bytes ([protocol.md](../server/protocol.md)):

- `PlayerInput` gains an input sequence number; the client keeps a ring
  buffer of recent inputs.
- `DriverFeedback` gains an authoritative snapshot of the local car (pose and
  velocities at its tick) and the last input sequence applied to it.

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
3. **Loop**: the library linked into the client as a prebuilt
   third-party module, the loop behind a console switch, with an
   artificial-latency harness
   (delay the UDP receive path by N ms) for LAN A/B, and automation tests on
   synthetic streams like `ApexSim.Motion.*` (loss, a contact correction, a
   relocation).
4. Then force feedback (replacing the `steer_stiffness` correction), the
   own-car engine sound and the HUD read the predicted state.

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
| `apx_car_state_save` / `apx_car_state_load` | the full state as a versioned blob (`car_step::encode_state`: `APXC`, version u16, then the state and the runtime fields serde skips, MessagePack with named fields, floats exact); refused across versions or cars |
| `apx_track_free`, `apx_car_free` | |

Opaque handles, `ApxStatus` codes, `catch_unwind` around every call. A
track handle is changed by each step (the wind), so a track and its cars
belong to one thread at a time. `ApxInput` is `PlayerInput`'s fields
flattened (28 bytes); `ApxCarPose` is 96 bytes.

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

**What phases 2-3 need from it.**

- The server snapshot on `DriverFeedback` decides what a rewind can set:
  pose and velocities alone leave tyre temperatures, wear, fuel, the
  hybrid and damage to drift on the client. Either the snapshot carries
  more, or the client sets the server's full blob at a lower rate (the blob
  is about 5.5 kB). A setter for pose and velocities on a saved state is
  not in the API yet.
- The road state and the live sky are the session's world; the client
  only gets the road quantized (`RoadState` rows), so its grip under the
  car can differ slightly from the server's. An API to update the track's
  road state and surface from what the server sends is not built.
- The tick the client steps is the server's `current_tick` for that tick
  (the wind and the lap timing read it); the rate is the session's.
- Linking: the `.lib` carries the whole server crate (tokio, rustls ...),
  about 10 MB in release; the linker drops what the car step does not
  reach. A physics-only crate split out of the server would shrink it.

## Risks

Rust-in-UE build plumbing (done once); a new physics config field the FFI
context cannot supply is a silent misprediction (the phase-1 test guards
it); custom tracks need the physics bundle. No cheating surface is added:
the server still simulates from raw inputs.
