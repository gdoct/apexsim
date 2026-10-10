# AI drivers

The AI is server-side and drives exactly like a human: every tick it makes a
`PlayerInputData` (throttle, brake, steering, gear, clutch, DRS, ERS) that
goes through the same physics step as a player's input. It follows its car's
own speed profile along the racing line, races the cars around it
(racecraft), plans its pit stops and fuel, and recovers from walls. Every
choice is a pure function of the car states, the tick and the driver's
profile, with chances drawn from `wind::hash01`, so a session replays the
same ([architecture.md](../architecture.md), determinism).

## Code

- `server/src/ai_driver.rs` - `AiDriverProfile` (skill and the attributes
  derived from it), `AiDriverController` (the per-tick driver),
  `field_skills` / `generate_ai_profiles`, `profile_pace`.
- `server/src/racecraft.rs` - `decide`, `Racecraft` (kept on
  `CarState::racecraft`), `Tactic`, `next_corner`, the stuck recovery.
- `server/src/racing_line.rs` - `build` / `build_laden`: the per-car speed
  profile the AI drives.
- `server/src/game_session.rs` - `spawn_ai_drivers`, `class_field`,
  `plan_ai_speeds` / `ai_speed_profile`, `ai_input_for`,
  `update_racecraft`, `plan_ai_stops`, `fuel_save_coast_s`,
  `ai_wants_boost`.
- `server/src/pit.rs` - `plan_stop`, `tyres_for_the_track`, `run_up_input`,
  `drive_input` ([pit-lane.md](pit-lane.md)).

## The field

- **Skills.** A driver's `skill_level` is 70-110 (`MIN_SKILL_LEVEL`,
  `MAX_SKILL_LEVEL`). `AiDriverProfile::new` derives aggressiveness,
  precision, reaction time (200 ms novice to 50 ms ace), steering
  smoothness, randomness and consistency from it. `field_skills(count,
  level)` seats the field: with the host's AI level (`CreateSession.ai_skill`,
  see [sessions.md](sessions.md)) spread evenly over `AI_FIELD_SPREAD` (4)
  points round it, shifted to stay inside 70-110; without one the mixed
  field, novice to ace. Names come from a fixed list; ids are random v4 in
  a live session.
- **Cars.** The AI races in the host car's `class` (car.toml):
  `class_field` takes every car of that class, sorted by id and rotated to
  start after the host's, and `spawn_ai_drivers` deals them round-robin. A
  car with no class races only against itself. The roster's `CarConfigId`
  tells the client which mesh each car is.
- **Plans.** When a car model is seated, `plan_ai_speeds` builds
  `AI_FUEL_PLAN_STEPS + 1` (11) speed profiles with
  `racing_line::build_laden`, one per tenth of its tank; a car drives the
  one for the next tenth up from what it carries (`ai_speed_profile`), so
  it is never planned lighter than it is and gets quicker as the tank
  drains.

## The controller (`AiDriverController::generate_input_in_traffic`)

Built per tick in `ai_input_for` with the car's speed profile
(`with_speed_profile`) and its fuel-save coast (`with_fuel_save`).

- **Line.** It follows the track's `raceline` (else the centerline; `python
  scripts/generate_race_line.py --track <yaml>` writes a raceline for a
  track without one). Precision pulls the target toward the centerline, and
  no racing AI tracks the line at more than `MAX_LINE_PRECISION` (0.85), an
  edge margin; only a profile with `exact_line` (the track guide's and the
  hotlap watch's lone car) drives the line itself.
- **Steering** every tick: curvature feedforward plus a Stanley term on the
  cross-track error and yaw-rate damping (`track_path`), limited to the
  fronts' grip, smoothed by skill. Off the road it aims at the centerline
  20 m ahead and slows to 4 or 12 m/s by heading error, so it rejoins
  rather than orbiting the raceline.
- **Speed.** The slowest profile point within reaction-plus-braking reach,
  times `profile_pace(skill)` (0.85 novice to 0.98 ace), times
  `(tyre_grip_share x aero_load_share)^0.75`. The look-ahead grows by the
  braking distance the brakes lose (`CarState::brake_share_of`: cold or
  faded pads, worn pads; carbon judged `CARBON_STOP_RISE_C` hotter, since
  it heats within the first moment of a stop). `tyre_grip_share` covers
  temperature, pressure, compound, wear and the road under the car
  (`surface_grip_share`: rubber, marbles, water); `aero_load_share` covers
  the wake, the wind and a damaged nose.
- **Pace noise** (`lap_noise`) is smooth along the lap, new each lap and
  only ever slower than the plan; a driver wanders up to 0.5 m about its
  line on straights by imprecision (`WANDER_*`).
- **Pedals and gear** are decided every `reaction_time_ms` and held between;
  the throttle and brake are limited to the friction circle the cornering
  leaves. Gears: upshift at a share (0.9 novice to 1.0 ace) of
  `physics::auto_upshift_rpm`, downshift when the gear below lands under
  0.85 of its shift point.
- **Traffic** (`avoid_traffic`): keeps a side margin from a car alongside
  and a following distance behind a car ahead in its lane.
- **Fuel save**: `with_fuel_save(coast_s)` shuts the throttle `coast_s`
  before every braking zone of the profile (brake left to the plan).
- **DRS** is asked for whenever `CarState::drs_allowed`; in a hybrid the
  mode is Balanced and the overtake button is held in a race from lap 2
  within `hybrid::AI_BOOST_GAP_S` (0.8 s) of the car ahead
  (`ai_wants_boost`).

## Racecraft (`racecraft.rs`)

`GameSession::update_racecraft` runs last in `update_air`, before the
physics. It snapshots the field (`racecraft::Rival`), asks each AI
`racecraft::decide`, and stores the answer on `CarState::racecraft`, which
the controller reads next tick. It is kept on the car because it is a
commitment: a driver who has pulled out to pass does not reconsider every
tick. Corners are read off the car's own speed profile (`next_corner`: the
next `Brake` phase, its slowest point, the hand at the apex).

- **Attack**: held up, the driver pulls out of the tow approaching a
  braking zone, to the inside (or round the outside if brave and that is
  covered), and alongside on the inside brakes `ATTACK_LATE_BRAKE` (2%)
  later. Without `TURN_IN_OVERLAP` (half a car) alongside at the end of the
  braking it lifts and tucks in behind. The lane ends at the apex either
  way; an attack not alongside within 10 s is given up.
- **Defend** (races only): one move to the inside before the braking
  against a car closing within `DEFEND_GAP_S` (0.5 s), held to the apex, by
  a hashed chance that grows with aggressiveness.
- **Room**: with an AI attacker alongside, the passed car takes the lane
  beside it and lifts 2% until clear.
- **Yield**: about to be lapped, it moves toward the outside and lifts 3%.
- **Mistakes**: with a car within 0.6 s behind, a driver now and then
  overcooks a braking zone, by its inconsistency (`MISTAKE_*`).
- **Getting unstuck** (`recover`): under 2.5 m/s in contact or off the
  road for 1.5 s, the car reverses for up to 2.5 s steering its nose
  toward the road 20 m ahead (`AiDriverController::back_out`), stops, takes
  first and drives on.

A lane is a place across the road (m left of the centerline), eased on and
off by `Racecraft::blend`; the controller holds it across the road
(`track_path`'s `hold`) and slows for the inside lane's tighter radius
(`v` with the root of the radius).

## Strategy

- **Stops** (`plan_ai_stops`, races only, inside `update_pits`): first the
  weather (`pit::tyres_for_the_track` on `RoadState::mean_line_water`: off
  slicks at `SLICK_OFF_WATER` 0.25, off treaded tyres under
  `TREADED_OFF_WATER` 0.06, inter/wet swaps 0.15 either side of
  `tyre_thermal::WET_FROM_WATER`; the gaps are hysteresis). Then
  `pit::plan_stop`: a tyre `AI_PIT_WEAR` (70%) worn, a flat spot past
  `AI_PIT_FLAT_SPOT`, a puncture or leak, `AI_PIT_DAMAGE` (25%) in a body
  zone or `AI_PIT_ENGINE_DAMAGE` (20%), or short of fuel with under
  `AI_PIT_FUEL_LAPS` (1.6) laps in the tank (only for a car the rules
  refuel: not an F1). Compound: hard with 15+ laps left, the reference
  with 6+, soft otherwise; in the wet the weather's tyre. The car turns
  onto the pit route `AI_PIT_APPROACH_M` (250 m) before the lane, races the
  run-up held to the lane's mouth speed and hands over to `drive_input`
  `AI_PIT_HANDOVER_M` (10 m) short of it.
- **Fuel saving** (`fuel_save_coast_s`): in a race, a car that will not be
  refuelled (an F1, or no pit lane) and cannot reach the flag lifts and
  coasts `FUEL_SAVE_COAST_S_PER_SHORT` (10 s) per share of fuel it is
  short, at most `MAX_FUEL_SAVE_COAST_S` (2 s).
- **Damage**: no special driving; the aero loss reaches it through
  `aero_load_share` and the steering pull through its steering loop.

Fuel, tyres, brakes and damage themselves: [vehicle-physics.md](vehicle-physics.md).

## Other drivers built on it

All call `ai_input_for` or the controller: the demo session and showcase
renders ([../game/spectator.md](../game/spectator.md)); the cool-down driver
that takes a finished human's car (`COOLDOWN_SKILL` 75); the `[debug]
stand_in_driver` hook ([operations.md](operations.md)); the hotlap watch's
lone car (skill 110, `exact_line`, easing down a skill ladder after a crash
or a struck lap: [sessions.md](sessions.md)); the track guide's solo runs
([../content/track-guide.md](../content/track-guide.md)).

## Checking it

```bash
cd server
cargo test --lib -- ai_driver:: racecraft::
cargo test --test racecraft_test     # a quicker driver passes a slower one; a car in a barrier reverses out
cargo test --test ai_race_start_test # composed off the grid; Le Mans first lap (the yaw seam)
cargo test --test determinism_test   # also fuel_test an_ai_short_of_fuel_lifts_and_coasts, air_test the_ai_gets_round_in_a_gale
```

**The AI survey** is the acceptance test for anything that moves the AI,
barriers, walls, the centerline, the ground or the car physics:

```bash
cargo test --release --test ai_race_start_test survey_ai_races_on_every_circuit -- --ignored --nocapture
```

Every circuit under `content/tracks/{default,custom}`, a mixed field of 10
AI in each of an LMP2, an F1 and a GT3 (`yotota-lmp2`, `fugazzi-sf26`,
`posh-gt3rs`), 180 s from the lights. Per race it prints cars out of shape
at 3 s, car-seconds of contact, off the road and sliding, retirements and
passes. Environment:

| Variable | Effect |
|---|---|
| `SURVEY_TRACKS=Spa,Monza` | only these stems (a custom import too) |
| `AI_TEST_SEED=n` | another seeded field (default 1); run 1-3 for a comparison |
| `SURVEY_ROAD_CONTACT=mesh` | drive the road meshes (default: centerline backend) |
| `SURVEY_WIND_KPH=25` | race in wind |
| `SURVEY_EVENTS=1` | log each contact with both cars' tactics, and long off-road spells |
| `SURVEY_DBG=1` | contact by tactic and the worst stations per car |
| `SURVEY_TRACE=grid,from_s,to_s` | one car every 0.1 s |

Compare against a baseline built from the commit before the change, on the
same seeds, and compare **totals**, not single circuits: one pile-up
dominates a run, and run-to-run noise moves a class's total by about 10%.
Isolating one feature (a scratch copy with it switched off) is how a cause
is found.

Other harnesses (all `#[ignore]`d):

- Pit surveys: `cargo test --release --test pit_stop_test pit_lane_survey
  -- --ignored --nocapture` and `ai_pit_survey` ([pit-lane.md](pit-lane.md);
  `PIT_TRACKS`, `PIT_TRACE`).
- `tests/pad_driver_probe.rs`: the AI's line with a pad's hand error on
  top, through the steering aid or raw, to reproduce a player's tyre
  temperatures (`PAD_AID`, `PAD_NOISE`, `PAD_HOLD_S`, `PAD_SKILL`,
  `PAD_CAR`, `PAD_TRACK`, `PAD_LAPS`, `PAD_FULL`):
  `PAD_AID=0 PAD_NOISE=0.12 cargo test --release --test pad_driver_probe -- --ignored --nocapture`.
- `hotlap_watch_test watch_probe` (the lone car's ladder per circuit and
  car, [sessions.md](sessions.md)).

What the AI still lacks is in [../simulation-gaps.md](../simulation-gaps.md)
(AI and strategy).

## Traps

- **Held up is measured at the other car's place, and only where the plan
  sets the speed** (a `Brake` or `Partial` phase). Read at the follower's
  own place a car braking ahead looks slow; read on the straights every
  accelerating car is below plan. Either way equal drivers attack each
  other endlessly and the line stops rubbering in
  (`the_line_rubbers_in_and_the_marbles_gather_off_it`).
- **A lane is held across the road, not as an offset from the line.** An
  offset read at the look-ahead point sweeps across the road with the line
  on every corner entry and drives the passing car into the passed one.
  The per-tick traffic dodge is still an offset; holding it across the
  road was tried and made contact worse.
- **The pace noise must stay one-sided** (never faster than the plan): a
  fast side held over a whole corner doubled the field's sliding.
- **Grip share goes in at the 0.75 power, not the root.** The physics
  would say root; the grip moves under the car through a corner as the
  tread heats, and with the root the AI left the road much more.
- **Heading errors wrap with `rem_euclid`** (`normalize_angle`): `%` keeps
  the sign, and at Le Mans's westbound left-hander every car turned the
  wrong way across the +-PI seam.
- **Determinism**: racecraft decides from one snapshot of the field, the
  traffic list is built in `BTreeMap` order, and every chance is
  `hash01` of driver, lap and corner. Keep new AI choices that way.
- **Profiles come from skill alone**: there is no server.toml table for
  the AI (an `[ai]` table in an old file is ignored), and the controller's
  gains are constants (`YAW_DAMPING`, `STANLEY_GAIN_*`...), never read
  from the environment, which would break the determinism rule.
- **The survey runs on the centerline backend by default**, while a server
  defaults to the road mesh; check both when the change touches contact.
