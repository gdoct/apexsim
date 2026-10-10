# Client-side prediction for the local car

Status: proposal, not built.

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
- The server crate is an rlib (`src/lib.rs`) plus two binaries; no FFI.
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

1. **Library**: physics crate as a staticlib behind the FFI, linked into the
   client. A CI test replays a recorded input trace through the server entry
   point and the FFI and asserts equal states.
2. **Wire**: input sequence and feedback snapshot, golden bytes both sides.
3. **Loop** behind a console switch, with an artificial-latency harness
   (delay the UDP receive path by N ms) for LAN A/B, and automation tests on
   synthetic streams like `ApexSim.Motion.*` (loss, a contact correction, a
   relocation).
4. Then force feedback (replacing the `steer_stiffness` correction), the
   own-car engine sound and the HUD read the predicted state.

## Risks

Rust-in-UE build plumbing (done once); a new physics config field the FFI
context cannot supply is a silent misprediction (the phase-1 test guards
it); custom tracks need the physics bundle. No cheating surface is added:
the server still simulates from raw inputs.
