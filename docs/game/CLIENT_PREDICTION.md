# Client-side prediction for the local car

Status: design, not implemented. Counterpart to the "authoritative contact"
advantage in docs/SIMULATION_GAPS.md — this is that architecture's one cost,
and how to pay it off.

## The problem

The client has no physics; every car, the player's included, is a telemetry
puppet (`Race/ApexCarMotion.h`). For the *local* car that means every input
makes a full round trip before the driver sees the result: ping, plus the
server's telemetry quantisation (60 Hz broadcast), plus the motion buffer's
deliberate two-frame interpolation delay (~33 ms), plus a render frame. On a
LAN that sums to something nobody notices. At 40–80 ms of internet ping it
is 90–150 ms between turning the wheel and the car answering — the one
degradation a driver coming from a locally-simulated sim feels within a
lap. The FFB already fought this once: the `steer_stiffness`
correction exists precisely because the column torque arrives a round trip
late and a delayed spring rings.

Remote cars do not have this problem — the motion buffer's interpolation is
the right tool for cars whose inputs the client can never know. Only the
local car needs prediction, which is what makes the feature tractable: one
car, whose inputs the client knows *before* the server does.

## The principle

Predict alone, reconcile continuously. The client runs the same physics for
its own car, ahead of the server by roughly the ping, and folds the server's
authoritative states in as they arrive. The server remains authoritative for
everything: the predicted pose is presentation, never sent anywhere, and a
disagreement is always resolved in the server's favour.

## One physics, not two

The classic approach — reimplement a simplified car model on the client —
is wrong for this project: two implementations drift forever, and every
physics feature (the whole of SIMULATION_GAPS.md) would have to land twice
or prediction quality silently rots.

Instead the server's own physics is compiled into the client. The sim is
deterministic by construction (no wall clock, no RNG, fixed dt, guarded by
`determinism_test`), so the crate can be built as a static library behind a
small C FFI and linked into the UE client like any third-party lib. One
implementation, bit-compatible with the server when compiled for the same
target. This is a payoff of the determinism discipline most codebases
cannot claim.

The FFI surface is small: create a context (car config, tuned setup clicks,
session conditions and the grip bake they imply, track data), step one car
one tick from an input, query the surface under a point. State crosses the
boundary as plain data. The client already has the car.toml (it builds the
car from it), knows its own setup clicks, and receives `SessionConditions`;
the weather bake is deterministic from those, so the library exposes the
same bake the server runs.

Exact bit-equality between the two builds is an optimisation target, not a
correctness requirement — reconciliation absorbs drift — but on one
platform, one compiler target, one crate, it is achievable and worth
asserting in a test (below), because near-zero correction error is what
makes prediction invisible.

## Track data on the client

The physics needs the track as the server sees it: the centerline from the
YAML and the `road`/`curbs`/`walls`/`ground` sidecars. The client today
ships only the render export. Options, in order of preference:

1. **A physics bundle served by the server**: on join, the client asks for
   the session track's physics data (a few MB, msgpack, keyed by
   `track_id` + the existing `ContentCrc`), cached on disk. One source of
   truth, and it covers custom and imported tracks a client has never seen.
2. Shipping the sidecars beside the exports in `Game/Tracks/`. Simpler, but
   duplicates content in the package and does nothing for a server's custom
   track.

Either way the content-checksum machinery already answers "is my copy the
session's copy"; a mismatch disables prediction for the session rather than
predicting on the wrong road.

## Wire changes

Both follow the appended-field rule and get golden bytes like every other
message.

- `PlayerInput` gains an **input sequence number**, appended. The client
  numbers every input it sends and keeps a ring buffer of recent ones.
- `DriverFeedback` gains an **authoritative snapshot**: the local car's
  pose and velocities as of the tick it was computed, plus the last input
  sequence applied to it. `DriverFeedback` is the right carrier because it
  is already per-driver and per-telemetry-tick — telemetry itself is
  serialized once per session and fanned out, so it cannot carry a field
  that differs per recipient.

An old client ignores both and behaves exactly as today; an old server
sends no snapshot and the client simply never enables prediction.

## The loop

- The client runs a fixed-step accumulator at the server's tick rate
  (240 Hz), stepping the local car with the same dt the server uses and
  interpolating the sub-tick remainder for the render frame. Fixed-step is
  load-bearing: it keeps the client's replay bit-compatible with what the
  server will compute from the same inputs, which is what keeps corrections
  near zero.
- On each authoritative snapshot: rewind the predicted car to it, replay
  the buffered inputs newer than its sequence (at 100 ms ping ~24 ticks of
  one car — microseconds against the bench), and compare with the pose the
  client was showing. The difference becomes a correction offset that
  decays over a few hundred ms rather than snapping; a gross disagreement
  (the 20 m rule the motion buffer already uses) re-seats outright.
- Aids come along for free: ABS, TC, the auto gearbox and the steering aid
  are part of `update_car_3d`, so the shared library predicts them.
  Server-decided flags (DRS allowed, headlights) are treated as inputs held
  at their last known value.

## What is not predicted

- **Other cars**, entirely — they stay on the motion buffer.
- **Car-car contact.** The local model steps the car alone (walls and road
  included, other cars not), so the moment of contact is where prediction
  visibly hands back to the server: the hit arrives as a correction, blended
  like any other. Every serious multiplayer title works this way; contact is
  exactly when the server knows something the client didn't.
- **Race control**: lap counting, sectors, track limits, records, grid
  seating and hotlap relocation all stay server-only. A relocation snaps by
  design.

## Knock-on wins

- **FFB**: computing the column torque from the *predicted* state is the
  principled replacement for the `steer_stiffness` latency correction — the
  rim answers the car it is actually showing.
- **Audio**: the own-car engine synth can take RPM from the predicted state
  instead of chasing 60 Hz telemetry.
- **HUD**: speed, gear and revs for the local car read from prediction.

All three come after the pose loop is proven, not with it.

## Phasing

1. **The library.** Build the physics crate as a staticlib behind the FFI,
   link it into the client build (prebuilt lib under ThirdParty, produced
   by the server build). Prove it offline: a test replays a recorded input
   trace through both the server entry point and the FFI and asserts the
   states match — the determinism test stretched across the boundary, and
   the CI tripwire that keeps the two builds from drifting as the physics
   evolves.
2. **The wire.** Input sequence and feedback snapshot, golden bytes on both
   sides.
3. **The loop.** Behind `apexsim.predict 0/1`, with an artificial-latency
   harness (delay the UDP receive path by a configurable N ms) so LAN
   development can feel what 80 ms feels like, and A/B it. Client
   automation tests drive the loop on synthetic streams like the
   `ApexSim.Motion.*` suite: lumpy arrivals, loss, a contact correction, a
   relocation.
4. **FFB, audio and HUD** migrate onto the predicted state.

## Risks

- **Build plumbing**: Rust-into-UE is unglamorous (prebuilt lib, link
  flags, a header) but done once.
- **Physics evolution**: every future physics feature must keep the FFI
  context complete (a new config field the client cannot supply becomes a
  silent misprediction). The phase-1 replay test is the guard; keep it in
  CI.
- **Content availability** for custom tracks — solved by the physics
  bundle, but that is its own small feature.
- **Cheating surface**: none added. The client gaining a physics model
  grants it no authority; the server still simulates from raw inputs and
  ignores everything else, exactly as today.
