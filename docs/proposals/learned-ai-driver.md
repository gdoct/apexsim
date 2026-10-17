# Learned AI driver

Status: proposal, not built.

A small neural net takes over the AI's low-level driving (steering and
pedals along the plan), trained first to copy today's controller and then
to beat it on the real physics. Everything above that layer stays code:
racecraft, strategy, the pit lane, recovery, mistakes and the rules. The
target is the AI's open gaps that come from driving at a limit the plan
gets wrong, above all imported cars retiring one to ten a survey race
([../simulation-gaps.md](../simulation-gaps.md), AI and strategy), where the
speed profile asks for grip the car does not have.

## What changes and what does not

The AI's per-tick input (`AiDriverController::generate_input_in_traffic`,
[../server/ai.md](../server/ai.md)) is a stack. Only the middle of it moves
to the net.

| Layer | Today | After |
|---|---|---|
| What to do: lane, attack, defend, yield, yellow pace | `racecraft::decide` | unchanged |
| Strategy: stops, compound, fuel save, ERS, DRS | `plan_ai_stops`, `fuel_save`, `ai_ers` | unchanged |
| Special modes: pit lane, backing out, waiting, reset | `pit::drive_input`, `back_out`, `wants_reset` | unchanged |
| The target: line, precision pull, lane, traffic dodge, wander | controller | unchanged, fed to the net |
| **Driving the target: steering, throttle, brake** | `track_path`, `calculate_throttle_brake`, the grip limiters | **the net** |
| Gears | upshift share of `auto_upshift_rpm` | unchanged in v1 |
| Mistakes, coasting, short-shift, reaction-time hold | applied on the pedals | unchanged, applied on the net's output |
| Off the road | aim at the centerline 20 m ahead | unchanged in v1 |

The net drives; the code decides. Skill still works because the net drives
the plan it is given (the plan's speeds already carry `profile_pace` and the
grip and aero shares), and precision, wander and mistakes are applied
around it as now.

## The steps, in order

Each step has a check that must pass before the next one starts.

### 1. Cut the seam (refactor only)

Split the driving core out of `generate_input_in_traffic` into a pure
function:

```rust
pub struct DrivingObs { /* step 2 */ }
pub struct DrivingAction { pub steering: f32, pub pedal: f32 } // pedal < 0 is brake

pub trait DrivingCore {
    fn drive(&self, obs: &DrivingObs) -> DrivingAction;
}
pub struct RuleCore;            // today's track_path + pedals + limiters, moved as-is
pub struct LearnedCore(Mlp);    // step 5
```

The controller builds the target (line, lane, dodge, target speed) as now,
packs a `DrivingObs`, calls the core, then applies what sits on top today:
reaction-time hold, coasting, lock-up, missed shift, gears, DRS.

**Check:** `determinism_test` bit-identical, and the AI survey prints the
same totals as the commit before. The seam must change nothing.

### 2. Define the observation and the action

Everything in the car's own frame and normalised, so one net covers every
track and class:

- **Path ahead:** at fixed distances (for example 10 points from 5 m to
  300 m, spaced wider further out), the target path's lateral offset and
  heading relative to the car, its curvature, the plan's speed there, and
  the road edges left and right.
- **Car now:** speed, lateral and yaw velocity, the current steering and
  pedal (the previous action), gear and rpm share of the shift point,
  per-wheel slip angle (`CarState::wheel_slip_angle`) and slip ratio,
  `tyre_grip_share`, `aero_load_share`, `brake_share_of`.
- **Car identity:** a handful of numbers from `CarConfig` (mass, power,
  downforce, the tyres' grip and peak slip, `optimal_slip_angle_rad`). This
  is what lets one net drive an imported car it was not trained on.
- **Action:** steering in [-1, 1] and one pedal axis in [-1, 1]. One axis
  because the AI never overlaps throttle and brake; split it later if
  trail braking with throttle is wanted.

Write the layout down once (feature name, scale) and give it a version: the
weights file carries the version and the loader refuses a mismatch.

**Check:** a unit test that builds a `DrivingObs` on a known track pose and
asserts the values. Silent unit or sign errors here cost days later.

### 3. Record a dataset from today's AI

A new `#[ignore]`d harness beside the survey,
`record_driving_dataset` in `tests/ai_race_start_test.rs` (or its own test
file), runs survey-style races and logs `(obs, RuleCore's action)` for every
car on the road under `RuleCore`. The session holds the samples in a buffer
the test drains (`GameSession::take_driving_samples()`, filled only when the
test asks for it at session creation): no file I/O in the sim path.

- **Coverage:** every circuit, the survey's three classes plus every
  imported car, `SURVEY_AI_LEVEL` 70 to 110, seeds 1 to 5, dry, wet, wind
  and snow. Breadth matters more than length per race.
- **Rate:** every 4th tick is plenty (about 100 Hz). Neighbouring ticks are
  near duplicates.
- **Drop rows** where `RuleCore` is not in charge: pit route, backing out,
  waiting, off the road. Tag every row with track, car and seed so the
  split in step 4 can hold out whole tracks and cars.
- **Format:** flat little-endian `f32` rows plus a small JSON header
  (layout version, feature names, row count).

Start with 2 to 5 million rows (about 1 to 2 GB). More only if step 6 says
so.

**Check:** replot a few laps from the file (speed and steering against lap
distance) and compare with `SURVEY_TRACE` of the same car and seed.

### 4. Train behaviour cloning (offline, Python)

`scripts/ai/` with PyTorch:

- **Net:** an MLP, 3 hidden layers of 64, ReLU, outputs clamped to
  [-1, 1] (a hard clamp, not `tanh`: see step 5). About 15k weights for
  some 130 inputs. Go wider only if the closed-loop check in step 6 shows
  the net is too small, not because the offline loss would drop.
- **Normalisation:** per-feature mean and scale from the training set,
  stored in the weights file and applied in Rust.
- **Loss:** Huber on both outputs; weight steering above pedal at first.
- **Split by track and car, not by row.** Hold out 5 circuits and 3 cars.
  Rows from one lap are so alike that a random split measures memory, not
  driving.
- **Export:** a plain binary of the layer sizes, weights, biases and the
  normalisation, with the layout version.

The offline loss is only a smoke test. A net with a low loss can still
leave the road in two corners.

### 5. Run it in Rust, deterministically

A hand-written forward pass (`server/src/learned_driver.rs`, about 60
lines): matrix-vector products in a fixed loop order, `f32`, ReLU as
`max(0.0)`, a final clamp. No crate.

- **Why not `ort` or `tract`:** they pick SIMD kernels per CPU and can
  thread, so the same weights can give different bits on two machines.
  The hand-written loop uses only `+`, `*` and `max`, which IEEE rounds the
  same everywhere, and LLVM does not reorder float sums without fast-math.
  So the net adds no platform dependence the physics does not already
  have. Avoid `tanh`, `exp` and `mul_add` in it: they go to the platform's
  libm or change rounding.
- **Cost:** about 15k multiply-adds per car per evaluation (step 4's
  size). 20 cars at 420 Hz is 8,400 evaluations a second, roughly 125M
  multiply-adds: a few percent of one core if the loop vectorises, 10%+ if
  it does not. Write the layer as "for each input, add `x[i]` times its
  weight column to every output": the compiler vectorises that across
  outputs without reordering any sum, so it stays bit-exact. Measure it in
  `benches/physics_tick.rs` with a learned field. If it is too slow,
  evaluate every 2nd or 4th tick and hold the output between (steering
  held 10 ms is fine; the 200 ms hold that missed corners was 20x that).
- **Choosing the core:** passed in at session creation (a `SimulateOptions`
  field, and a survey variable `SURVEY_DRIVER=learned:<weights>` read by the
  test, not the sim). No env or config read in the tick. The live server
  keeps `RuleCore` until step 8 passes.
- **Guard:** a non-finite output, or the car off the road, hands that tick
  to `RuleCore`. Count how often it happens.

**Check:** `determinism_test` with a learned field added stays
bit-identical across two runs.

### 6. Evaluate closed loop, then fix the drift (DAgger)

Run the survey with `SURVEY_DRIVER=learned:...` on the same seeds as a
`RuleCore` baseline, and compare totals as for any AI change: off-road and
sliding car-seconds, contact, retirements, passes. Add a lone-car lap time
per track and class. Expect the first net to be clearly worse: once it
makes a small error it is somewhere the teacher never drove, and the error
grows.

The fix is DAgger, which works here because the teacher is code that can
be asked in any state:

1. Record with the **net driving** and `RuleCore` labelling: every row is
   the net's observation with the action `RuleCore` would have taken.
   `RuleCore::drive` takes `&self` and the observation only, so asking it
   changes nothing.
2. Add those rows to the dataset and retrain (step 4).
3. Survey again. Repeat 3 to 5 times.

For the first round a mixed driver helps: the teacher drives with a chance
that falls each round, drawn with `wind::hash01` of car and tick, so it
stays deterministic.

**Check (end of cloning):** survey totals within run-to-run noise (about
10% a class) of `RuleCore`, on held-out tracks and cars too. At this point
the net is as good as today's AI, not better. Do not ship it for that
alone: it is harder to debug than the code it copies.

### 7. Beat the teacher: reinforcement learning

Cloning cannot fix the imported cars, because the teacher crashes them.
This step trains on the outcome instead of the teacher.

- **The environment:** a Python binding to a headless session
  (`server/py/`, a PyO3 crate wrapping the same entry point as
  `apexsim-replay simulate`): `reset(track, car, seed, start_m)`,
  `step(action) -> (obs, reward, done)`. Run many in parallel; measure
  ticks per second first (`benches/physics_tick.rs`) to size the compute.
- **Start lone,** one car on an empty track from random points on the lap,
  60 s episodes. Add a `RuleCore` field around it once lone driving beats
  the teacher.
- **Reward per tick:** progress along the lap (metres of `track_progress`),
  minus off-road time, wall and car contact, a penalty on sliding beyond the
  peak slip, and a small one on input changes (no twitching). Weight the
  penalties so a crash costs more than any time saved.
- **Algorithm:** SAC or PPO for continuous control. Start the actor from the
  cloned weights and, early on, keep a loss pulling it toward `RuleCore`'s
  action, or the first updates wreck what cloning built.
- **The plan's speed is an input, not a command.** The net learns to brake
  earlier than a plan that asks too much, which is exactly the imported-car
  failure. To keep skill levels, add a reward for holding the plan's pace
  where the car can, or the net drives every car flat out whatever the
  `profile_pace`.

**Check:** survey totals better than `RuleCore` on retirements and contact
for imported cars, not worse on the three native classes, lap times not
slower.

### 8. Ship it

- Weights in `content/ai/`, with the layout version. They are content: a
  change to them changes AI behaviour and makes showcases and guides
  stale, like a track or car edit.
- One net for every car, with `RuleCore` as the fallback per car or class
  for anything that fails its check.
- Update [../server/ai.md](../server/ai.md) in the same commit, delete the
  imported-car row from the simulation-gaps doc, and move this proposal's
  remaining ideas there.

## A smaller first experiment

Before any of the above, a net (or plain regression) that predicts a car's
real cornering limit can go into the plan instead of the controller:
record lone laps at rising pace until each car leaves the road, fit the
limit speed against curvature, camber, grip and the car's numbers, and let
`racing_line::build` cap the profile with it. It is one evening of work,
needs no change to the controller, and attacks the same imported-car
retirements. If it fixes them, steps 7 and 8 may not be worth their cost.

## Traps

- **Determinism first.** The survey, replays and showcases only work if a
  seeded race replays bit for bit. Every chance in training-time mixing goes
  through `wind::hash01`; no inference crate, no threads, no transcendental
  functions in the forward pass.
- **Judge on the survey, never on the loss.** Offline error says almost
  nothing about whether a car stays on the road.
- **Hold out whole tracks and cars.** Otherwise the numbers are memory.
- **The teacher's mistakes are in the data.** Rows from `RuleCore` include
  whatever crashes imported cars today; cloning reproduces it. Only step 7
  goes past it.
- **Keep the decisions in code.** A net that learns lanes, flags or
  yielding has to be retrained for every rule change, and cannot be
  debugged. The net only drives the target it is handed.
- **The observation layout is a format.** Changing a feature means new
  data, retraining and a new version; the loader must refuse old weights.
- **The survey drives the centerline backend** while a live server drives
  the road mesh. Train and check on both, as for any surface change.
