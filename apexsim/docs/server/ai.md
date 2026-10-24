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

- `server/sim/src/ai_driver.rs` - `AiDriverProfile` (skill and the attributes
  derived from it), `AiDriverController` (the per-tick driver),
  `field_skills` / `generate_ai_profiles`, `profile_pace`.
- `server/sim/src/racecraft.rs` - `decide`, `Racecraft` (kept on
  `CarState::racecraft`), `Tactic`, `next_corner`, the stuck recovery.
- `server/sim/src/racing_line.rs` - `build` / `build_laden`: the per-car speed
  profile the AI drives; `build_driven` is the player's aid planned again
  at a lap's end ([game/client.md](../game/client.md)).
- `server/sim/src/game_session.rs` - `spawn_ai_drivers`, `class_field`,
  `plan_ai_speeds` / `ai_speed_profile`, `ai_input_for`,
  `update_racecraft`, `plan_ai_stops`, `fuel_save`, `refuels`,
  `ai_ers`.
- `server/sim/src/pit.rs` - `plan_stop`, `tyres_for_the_track`, `fuel_save_for`,
  `run_up_input`,
  `drive_input` ([pit-lane.md](pit-lane.md)).

## The field

- **Skills.** A driver's `skill_level` is 70-110 (`MIN_SKILL_LEVEL`,
  `MAX_SKILL_LEVEL`). `AiDriverProfile::new` derives aggressiveness,
  precision, reaction time (200 ms novice to 50 ms ace), steering
  smoothness, randomness and consistency from it. `field_skills(count,
  level)` seats the field: with the host's AI level (`CreateSession.ai_skill`,
  see [sessions.md](sessions.md)) spread evenly over `AI_FIELD_SPREAD` (4)
  points round it, shifted to stay inside 70-110; without one the mixed
  field, novice to ace. The same level is `--ai-level` on `apexsim-replay
  render` and `simulate` (`SimulateOptions::ai_level`, a showcase's
  `ai_level`) and `SURVEY_AI_LEVEL` for the survey; every one defaults to the mixed
  field, so showcases and guides replay as before. Names come from a fixed list; ids are random v4 in
  a live session.
- **Cars.** The AI races in the host car's `class` (car.toml):
  `class_field` takes every car of that class, sorted by id and rotated to
  start after the host's, and `spawn_ai_drivers` deals them round-robin. A
  car with no class races only against itself. The roster's `CarConfigId`
  tells the client which mesh each car is. A multi-class session
  (`CreateSession.classes`, [sessions.md](sessions.md) "Multi-class
  racing") seats so many AI per class at the class's own level
  (`generate_class_profiles`, `AiDriverProfile::class`), each dealt from
  its class's cars (`class_pool`); in traffic a car yields to one of a
  quicker class as to a lapping car and defends against none of another
  class (`racecraft::Rival::class_rank`). `SURVEY_CLASSES=LMP2:6,GT3:6`
  runs the survey on such a field.
- **Plans.** When a car model is seated, `plan_ai_speeds` builds
  `AI_FUEL_PLAN_STEPS + 1` (11) speed profiles with
  `racing_line::build_laden`, one per tenth of its tank; a car drives the
  one for the next tenth up from what it carries (`ai_speed_profile`), so
  it is never planned lighter than it is and gets quicker as the tank
  drains. A car whose engine damage has cost it power
  (`damage::engine_power_factor`, in `AI_POWER_PLAN_STEP` 5% steps)
  drives a plan built for the power it has left
  (`plan_damaged_ai_speeds`, in `update_air` after the pits: the car's
  `max_engine_power_w` cut, one plan per car, step lost and step of the
  tank, built the tick after the damage and kept), so its straights,
  braking points and coasting are the car's as it is.
- **Grip a plan reads.** A plan corners and brakes at the car's
  `CarConfig::envelope_mu` ([vehicle-physics.md](vehicle-physics.md)):
  for a load-sensitive tyre (the AC imports) read at the load the car
  carries at each speed (`racing_line` `CarEnvelope::mu_table`, every
  2 m/s) with the lateral transfer at the limit, the weaker axle's figure.
  A linear tyre (every shipped car) is `grip_coefficient` at any speed.

## The controller (`AiDriverController::generate_input_in_traffic`)

Built per tick in `ai_input_for` with the car's speed profile
(`with_speed_profile`) and its fuel-save coast (`with_fuel_save`).

- **Line.** It follows the track's `raceline` (else the centerline; `python
  tools/generate_race_line.py --track <yaml>`, run in `apexsim-content/`, writes a raceline for a
  track without one). Precision pulls the target toward the centerline, and
  no racing AI tracks the line at more than `MAX_LINE_PRECISION` (0.85), an
  edge margin; only a profile with `exact_line` (the track guide's and the
  hotlap watch's lone car) drives the line itself.
- **Steering** every tick: curvature feedforward plus a Stanley term on the
  cross-track error and yaw-rate damping (`track_path`), limited to the
  fronts' grip, smoothed by skill. Off the road it aims at the centerline
  20 m ahead and slows to 4 or 12 m/s by heading error, so it rejoins
  rather than orbiting the raceline.
- **Speed.** The slowest profile point within reaction-plus-braking reach
  (`unpaced_mps`: a hybrid's motor counted unpaced over its lap budget,
  [vehicle-physics.md](vehicle-physics.md) Hybrid),
  times `profile_pace(skill)` (0.85 novice to 0.98 ace), times
  `(tyre_grip_share x aero_load_share)^0.75`. The look-ahead grows by the
  braking distance the brakes lose (`CarState::brake_share_of`: cold or
  faded pads, worn pads; carbon judged `CARBON_STOP_RISE_C` hotter, since
  it heats within the first moment of a stop). `tyre_grip_share` covers
  temperature, pressure, compound, wear and the road under the car
  (`surface_grip_share`: rubber, marbles, water; `snow_tyre_share`: a snow
  tyre's bite on the cover); `aero_load_share` covers
  the wake, the wind and a damaged nose.
- **Snow technique**, on a snow-covered line on any tyre, studs too (each
  scaled by `snow_technique_weight` of the line's cover as the sky last read
  it, `LiveConditions::line_snow`, in full from `SNOW_TECHNIQUE_FULL_COVER`
  0.3 and so on a packed line, none on a bare one; the bake's
  `TrackSurface::baked_snow` on a track without a road state;
  `GameSession::snow_technique` into `AiDriverController::with_snow`): snow
  that the forecast brings mid-race brings it, a melted line ends it, and a
  dry road is driven bit for bit as before. The target pace times `SNOW_PACE` (0.93), so the
  braking comes earlier than the plan's grip says; the steering lag times
  `SNOW_STEER_LAG_SCALE` (1.8); and the throttle squeezed in no faster
  than `SNOW_THROTTLE_STEP` (0.15) a decision. Against the controller
  without it, the snow survey (Silverstone, Monza, Spa, seeds 1-2) went
  from 712 to 432 car-seconds of contact, 4439 to 2271 off the road and
  773 to 461 sliding.
- **Pace noise** (`lap_noise`) is smooth along the lap, new each lap and
  only ever slower than the plan; a driver wanders up to 0.5 m about its
  line on straights by imprecision (`WANDER_*`).
- **Pedals and gear** are decided every `reaction_time_ms` and held between;
  the throttle and brake are limited to the friction circle the cornering
  leaves. A car without ABS (`abs_enabled = false`: the AC imports' F1s
  and prototypes) brakes no harder than locks an axle
  (`unlocked_pedal`: each axle's share of `max_brake_force_n` by
  `brake_bias_front` against its tyres' grip on its weight, downforce and
  the load the braking moves forward, `UNLOCKED_PEDAL_MARGIN` 0.95); with
  ABS (every shipped car) the pedal is not capped. Gears: upshift at a share (0.9 novice to 1.0 ace) of
  `physics::auto_upshift_rpm`, downshift when the gear below lands under
  0.85 of its shift point.
- **Traffic** (`avoid_traffic`): keeps a side margin from a car alongside
  and a following distance behind a car ahead in its lane.
- **Fuel save**: `with_fuel_save(coast_s)` shuts the throttle `coast_s`
  before every braking zone of the profile (brake left to the plan);
  `with_short_shift(share)` upshifts up to `SHORT_SHIFT_DROP` (30%) before
  the usual shift point, where the engine's friction (which grows with
  revs) takes less of what the fuel makes.
- **DRS** is asked for whenever `CarState::drs_allowed`.
- **Hybrid** (`ai_ers`, `hybrid::ai_ers`): in a race from lap 2 the
  overtake button is held within `hybrid::AI_BOOST_GAP_S` (0.8 s) of the
  car ahead, to attack, but only on a straight (or out of the corner onto
  one) at least `AI_ATTACK_STRAIGHT_SHARE` (0.6) of the lap's longest, so
  its energy goes where a pass is made, not on the short straights between
  (`hybrid::straights`: the `Throttle` runs of the car's full-tank plan,
  kept per car as `GameSession::ai_straights`); and within
  `AI_DEFEND_BOOST_GAP_S` (0.5 s) of the car behind, anywhere, to defend;
  with the battery under `AI_HARVEST_BELOW` (30%)
  and nobody within `AI_ALONE_GAP_S` (2 s) either way the mode is Harvest,
  so the motor is there in the next fight; Balanced otherwise, and always
  before lap 2, outside a race and on the pit route.

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
- **Where to pass** (`pass_here`): the corner is taken only if its run in
  (the throttle stretch of the plan before its braking point, up to
  `PLAN_RUN_MAX_M` 1500 m; doubled, `PLAN_DRS_WEIGHT`, with DRS open on it
  now) is at least `PLAN_SHARE` (0.6) of the best of the next
  `PLAN_CORNERS` (3) within `PLAN_AHEAD_M` (2500 m): a driver held up
  waits for the long straight, where the tow closes it most, rather than
  diving into the first corner. Held up `PLAN_PATIENCE_S` (6 s) past its
  hesitation, any corner will do; a car `MUCH_SLOWER_MPS` (4 m/s) slower
  is passed on any straight.
- **Defend** (races only): one move to the inside, held to the apex, by a
  hashed chance that grows with aggressiveness, and only one a corner
  (`Racecraft::defended_corner`): no second move back across. It is made
  before the braking against a car within `DEFEND_GAP_S` (0.5 s) that is
  attacking it or that, at the speed it closes now (over
  `DEFEND_CLOSING_MPS`), gets alongside by the braking point with
  `DEFEND_BRAKING_GAIN_S` (1 s) of braking on top: a car close behind but
  closing slowly is not defended against. Further out on the straight
  (the braking point up to `TOW_COVER_FROM_S`, 8 s, ahead) the same move
  covers the tow: against a car still in the defender's wake (within half
  a car's width across), within `TOW_COVER_GAP_S` (0.8 s), closing over
  `TOW_COVER_CLOSING_MPS` (2 m/s) and far enough back
  (`TOW_COVER_LEAD_S`, 1.2 s at that closing) that the move is made before
  it arrives. A car already pulled out of the wake is not moved on.
- **Room**: with an attacker alongside, the passed car takes the lane
  beside it and lifts 2% until clear. An AI attacker is read by its
  tactic; a player (`Rival::human`, any car without an AI profile) by
  where it is: near a corner, beside the AI (not nose to tail in its
  lane, `HUMAN_BESIDE_SLACK_M`), at least half a car alongside
  (`TURN_IN_OVERLAP`), and on the corner's inside or ahead. A player has
  no held lane, so the room is kept beside where it is and only ever
  widens: the AI moves away as the player comes over and never follows
  it back across.
- **Yield**: about to be lapped, it moves toward the outside and lifts 3%.
- **Under a yellow** (`CarState::flag_yellow`, [sessions](sessions.md#flags)):
  the pace is 95% of the plan under a single and 85% under a double, and no
  pass is begun unless the car ahead is crawling (the incident itself).
- **Under race control's pace** (`CarState::speed_cap_mps`, a formation or
  pace lap, the virtual safety car, the safety car's queue;
  [sessions](sessions.md#race-start-formation-lap-rolling-start-false-starts)):
  the driver aims at the pace as at a corner's speed (its target speed is
  never above it), no pass is begun unless the car ahead is crawling (a
  lapped car let by under the safety car is held to a pace well above the
  queue's, so it passes it this way); the physics holds the car to the
  pace, and over the approach to its slot on a standing start the server
  drives it. On a rolling start's form-up race control also gives it a
  side of the road (`CarState::formation_lane_m`): driven as a racecraft
  lane, with the traffic dodge's aim across the road dropped (it would
  take the car into the one beside it), its speed limit kept. Warming its
  tyres on a formation or pace lap (`CarState::warm_up`) it weaves across
  its line on the straights (`WEAVE_M`, `WEAVE_WAVELENGTH_M`, its phase
  its own), faded out by the line's curvature and an edge close by, as
  the wander is.
- **Mistakes**: with a car within 0.6 s behind, a driver now and then
  gets a corner wrong, by its inconsistency (`MISTAKE_CHANCE`, one roll a
  corner as it nears the braking zone). Away from pressure it does too, at
  a share of that chance for each thing against it (`MistakeRisk`, built
  per tick by `GameSession::mistake_risk`, each 0..1, the shares summed):
  tyres under their window (`COLD_MISTAKE` 0.15 when the colder axle is
  `COLD_RISK_SPAN_C` 25 °C under the window's cold edge), a wet patch
  (`WET_MISTAKE` 0.4 with a tyre on water `WET_RISK_SPAN` 0.3 over what the
  road's grip was baked at: a puddle, a wet line in a drying race, rain
  the session was not set for) and a long stint (`FATIGUE_MISTAKE` 0.15,
  from an hour in the car to full at four, the stint's laps at its lap
  time). An even draw says how (`Mistake`), of all four kinds under
  pressure or tired, and of the first three on cold tyres or a wet patch: it overcooks the braking (2.5-4.5% too fast,
  `Racecraft::overspeed`); it locks up late in the braking zone (80 m
  before the apex, or at the end of the braking: the pedal to the floor
  past the friction circle for 0.25-0.6 s, `lock_up_s`, which an ABS car's
  ABS catches); it misses the apex 1-2 m wide (`wide_m`, added to the
  steering target as the wander is, from the braking to the apex and
  eased back after); or it misses an upshift on the way out (no upshift
  for 0.4-1 s, on the limiter, `missed_shift_s`). A corner's mistake ends
  once the corner is passed.
- **Getting unstuck** (`recover`): under 2.5 m/s in contact or off the
  road for 1.5 s, the car reverses for up to 2.5 s steering its nose
  toward the road 20 m ahead (`AiDriverController::back_out`), stops, takes
  first and drives on. Before it moves at all (backing out or setting
  off) it looks behind: with a racing car within `REJOIN_LOOK_M` (200 m)
  behind on that stretch, or alongside, arriving within `REJOIN_GAP_S`
  (3 s), it waits on the brake (`Racecraft::waiting`), at most
  `REJOIN_WAIT_MAX_S` (6 s) a spell. It keeps looking while it backs:
  a car arriving stops it on the brake in reverse, its back-out held, until
  the car is by (within the same spell's wait). "The road ahead", backing out and off
  the road, is on the leg the car left (`Racecraft::leg_idx`, `leg_m`):
  its own nearest centerline point on the road, and off it the nearest
  within 32 points of the last, so a car whose lap position jumps to
  another leg running close by (Zandvoort's 887 m and 4282 m) still
  steers for its own. A car that cannot get back (penned in, or on snow
  where every attempt slides off) asks to be put back once it has been in
  one spell `RESET_AFTER_S` (15 s) without gaining `RESET_HEADWAY_M` (20 m)
  of lap (`Racecraft::wants_reset`). A spell is off the road, against a
  wall or backing off it, or crawling on the road with more than
  `STALL_THROTTLE` (0.3) of throttle (a grade on snow it cannot climb), and
  once begun it runs until the car is past `SPELL_ENDS_MPS` (6 m/s) or has
  the headway: rocking off a wall and back is one spell, not a new one each
  time. It brakes to a stop and the session recovers it to the track as a
  driver's back-to-track would (`GameSession::recover_car`, held for the
  recovery's cost); a car stuck on the road itself is put
  `AI_STALL_RUN_UP_M` (120 m) back down the lap, for a run-up.
- **Wheelspin at a standstill**: under `LAUNCH_SPIN_SPEED_MPS` (1 m/s)
  with the driven wheels at 95% of their peak slip or more the AI backs off
  the pedal it has applied (`limit_launch_spin`, to no less than 0.15),
  where the grip limiter above 3 m/s does not reach.

A lane is a place across the road (m left of the centerline), eased on and
off by `Racecraft::blend`; the controller holds it across the road
(`track_path`'s `hold`) and slows for the inside lane's tighter radius
(`v` with the root of the radius).

## Strategy

- **Under the virtual safety car or the safety car** (`StopContext::neutralised`,
  [sessions](sessions.md#virtual-safety-car)): a stop is cheap, so with
  more than `AI_STAY_OUT_LAPS` left a driver takes one it would make anyway:
  a stop owed to the pit rule, tyres `AI_NEUTRALISED_WEAR` (30%) worn, or
  short of fuel to the flag. While the safety car's queue forms the lane is
  closed: the wish to stop is kept and the car turns in at the next pass.
- **Penalties** ([sessions](sessions.md#penalties)): an owed drive-through
  or stop-go is served at the next pass of the lane (`plan_ai_stops` sets
  `wants_stop`), before any stop it plans.
- **Stops** (`plan_ai_stops`, races only, inside `update_pits`): first the
  weather. A car with a snow tyre reads the snow first
  (`pit::snow_tyres_for`): on the line's cover (`CrossoverContext::line_snow`,
  `RoadState::line_snow_of`; `pit::snow_tyres`) onto its snow tyre from
  `SNOW_ON_COVER` (0.4), on it while the line holds `SNOW_OFF_COVER` (0.15)
  or more (a packed line's 0.65 stays a snow tyre's); and against the
  forecast (`snowing`: the road's snowfall or the sky's snow step;
  `snow_turn_in_s`: the next step that starts or stops it) and the melt
  (`melting_per_s`: `road_state::SNOW_MELT_PER_C` times the road's degrees
  over freezing). It fits studs `SNOW_FIT_LEAD_LAPS` (2) laps before the
  forecast's snow and while snow falls that is not due to stop within
  `FORECAST_LAPS`; keeps them on a bare line while snow falls or more is
  due within `FORECAST_LAPS`; and, the snow stopped and none due, comes off
  them for the water's tyre or its reference once the melt will take the
  line under `SNOW_OFF_COVER` within `SNOW_MELT_LEAD_LAPS` (1), not onto
  them on a covered line that melts clear within
  `SNOW_GAMBLE_MELT_LEAD_LAPS`. A gambler (by `SNOW_GAMBLE_CHANCE` 0.6
  times its aggressiveness, drawn once a race from its id,
  `CrossoverContext::snow_roll`) fits them only `SNOW_GAMBLE_FIT_LEAD_LAPS`
  (0.5) ahead, waits for the cover once it snows, and comes off
  `SNOW_GAMBLE_MELT_LEAD_LAPS` (2.5) ahead of the melt. No change either way
  within `CROSSOVER_STAY_OUT_LAPS` of the flag; a stop planned for fuel or
  wear fits the same snow tyre. In a dry winter race at Monza with snow
  forecast six minutes in, three rally cars called for studs 245 s (two laps)
  ahead and the gambler 78 s ahead, all on a bare line, and all four were on
  them on the covered line; in a melt on a 7.5 °C track all four called for
  slicks with the line at 0.44-0.59, ahead of the 0.15 (`track_evolution_test`
  `the_ai_fits_studs_before_the_forecast_snow`,
  `the_ai_comes_off_studs_ahead_of_the_melt`). Off snow tyres on a clear line,
  and for a car without one, the rain decides as follows
  (`pit::tyres_for_the_track` on `RoadState::mean_line_water`: off
  slicks at `SLICK_OFF_WATER` 0.25, off treaded tyres under
  `TREADED_OFF_WATER` 0.06, inter/wet swaps 0.15 either side of
  `tyre_thermal::WET_FROM_WATER`; the gaps are hysteresis), read against
  the race (`CrossoverContext`, `GameSession::crossover_context`): no
  stop for the weather within `CROSSOVER_STAY_OUT_LAPS` (2) of the flag
  unless slicks hold over `SLICK_STAY_OUT_WATER` (0.4); none off treaded
  tyres while the forecast (`LiveConditions::changes_after`) brings rain
  back within `FORECAST_LAPS` (2) laps; and by a chance a lap of
  `SLICK_GAMBLE_CHANCE` (0.5) times its aggressiveness, a driver keeps
  slicks out in rain due to stop within a lap, or takes slicks early off
  intermediates once the rain has stopped, none is due and the line holds
  under `SLICK_GAMBLE_WATER` (0.14). It also reads how hard it will rain
  (`CrossoverContext::rain_ahead`: the heaviest `Weather::rainfall` the
  sky holds now or the forecast brings within `FORECAST_LAPS`), how fast
  the line is drying (`drying_per_s`: `GameSession::line_drying`, the
  line's water a second eased over `LINE_DRYING_EASE_S` 30 s, reset at the
  green) and what the rest of the running field is on (`field_treaded`,
  the share on rain tyres of the cars on slicks or rain tyres: a car on
  studs is neither): with `HEAVY_RAIN_AHEAD` (0.9, a downpour) on
  a line getting wetter, slicks come in from `SLICK_FOLLOW_WATER` (0.18)
  and straight onto the full wet; slicks in the rain follow half the field
  (`FIELD_FOLLOW_SHARE`) onto treaded tyres from 0.18 too; a wet becomes an
  intermediate with the rain due to stop within the forecast, and an
  intermediate in a downpour stays out with the rain stopping within the
  lap; slicks over their limit stay out on a dry sky when the line dries
  back under `SLICK_OFF_WATER` within the lap; and on intermediates, the
  rain over and none due, any driver takes slicks when the line will be
  under `TREADED_OFF_WATER` within the lap, or under
  `SLICK_GAMBLE_WATER` with half the field already on slicks. Then
  `pit::plan_stop` on a `StopContext` (`GameSession::stop_context`: the
  laps left, its fuel, the laps on its tyres since
  `PitState::stint_start_laps`, the gap to the car ahead, a car behind
  turning in, its aggressiveness and a roll a lap): a flat spot past
  `AI_PIT_FLAT_SPOT`, a puncture or leak, `AI_PIT_DAMAGE` (25%) in a body
  zone or `AI_PIT_ENGINE_DAMAGE` (20%) always; a tyre `AI_PIT_WEAR` (70%)
  worn unless it reaches the flag (`pit::tyres_reach_flag`: under
  `AI_WEAR_TO_FLAG` 92% at this stint's rate) within `AI_STAY_OUT_LAPS`
  (3); or short of fuel with under
  `AI_PIT_FUEL_LAPS` (1.6) laps in the tank (only for a car a stop refuels,
  `GameSession::refuels`: not an F1, unless the race is longer than its
  tank reaches on all the saving a driver can do, `MAX_FUEL_SAVE_SHORT`
  (15%) short, as a series racing that far would refuel; a stint energy
  limit counts as fuel, `pit::usable_fuel_liters`). Short of fuel on tyres
  that reach the flag, the stop is a
  splash (`StopPlan::fuel_only`, `PitState::fuel_only`: fuel alone, no
  tyres, no repairs); no more than `SAVE_TO_SKIP_SHORT` (10%) short on
  such tyres, it makes no stop for fuel at all and saves it instead
  (`saves_to_flag`). A stop it must make anyway (tyres that will not last,
  a tank that will not) is brought forward, no later than
  `AI_STRATEGY_MIN_LAPS` (4) from the flag: to **cover** a car within
  `AI_COVER_GAP_S` (2.5 s) behind that has turned in, on tyres
  `AI_COVER_WEAR` (40%) worn, and to **undercut** a car it is held up by
  within `AI_UNDERCUT_GAP_S` (1.2 s), on tyres `AI_UNDERCUT_WEAR` (50%)
  worn, by a chance a lap of `AI_UNDERCUT_CHANCE` (15% timid to 60%
  bold). **The rejoin**: `pit::pit_loss_s` is what a stop costs (the
  lane at its limit, and `LANE_TOP_SPEED_MPS` outside the lines, against
  the track it bypasses at the car's lap pace, plus `BOX_STOP_LOSS_S` (4 s)
  and the service with the fuel to the flag), and
  `GameSession::rejoins_in_traffic` reads it against how far behind on the
  road, at the car's pace, every other car out of the lane is: within
  `AI_REJOIN_TRAFFIC_S` (1.5 s) the stop rejoins in traffic
  (`StopContext::rejoin_traffic`). Then worn tyres wait for a clear rejoin
  up to `AI_REJOIN_WAIT_WEAR` (78%), a pit rule's stop up to
  `AI_REJOIN_WAIT_LAPS` (2) past its share, and nothing is brought forward:
  no undercut onto a busy road, and a car behind that has turned in is
  answered by the **overcut**, staying out on the road its stop cleared,
  instead of a cover. Faults, damage and a short tank never wait. Under the session's pit rule (`data::PitRule`,
  [sessions](sessions.md#session-rules-set-on-create); `StopContext::stops_owed`
  and `compound_owed`, `GameSession::stops_owed`) the stops it owes split
  the race into even stints: an owed stop is due once this stint has run
  its share of the laps left (`(stint + laps left) / (owed + 1)`), and is
  made at the latest with a lap left for each stop still owed; it counts
  as a stop it must make anyway, so a cover or an undercut brings it
  forward, worn tyres that reach the flag do not keep it out, and a car
  owing one does not save fuel to skip a stop (`saves_to_flag`). Under
  fixed stint lengths (`RaceRules::max_stint_laps` / `max_stint_minutes`,
  [sessions](sessions.md#fixed-stint-lengths); `StopContext::stint_left_laps`)
  a stint with less than `AI_STINT_STOP_LAPS` (1 lap) left short of the
  flag is a stop at once, traffic or not. A race started on the set a car
  qualified on ([sessions](sessions.md#tyre-sets)) counts that set's
  qualifying laps in its stint, so its wear rate is the set's. Compound:
  hard with 15+ laps left in the stint (the laps left over the stops owed),
  the reference with 6+, soft otherwise; in the wet or on a snowy line
  (`pit::snow_tyres`) the weather's tyre; a

  car owing the second compound takes the nearest other slick
  (`pit::other_dry_compound`) and never a splash. The car turns
  onto the pit route `AI_PIT_APPROACH_M` (250 m) before the lane, races the
  run-up held to the lane's mouth speed and hands over to `drive_input`
  `AI_PIT_HANDOVER_M` (10 m) short of it.
- **Fuel saving** (`fuel_save`, `pit::fuel_save_for`): in a race, a car
  that will not be refuelled (an F1 whose tank reaches the flag on saving;
  without a pit lane a car that would be refuelled is topped up at the
  line, `update_line_refuel`, and saves nothing), or one saving to
  skip a stop, and cannot reach the flag lifts and coasts `FUEL_SAVE_COAST_S_PER_SHORT` (10 s) per share of
  fuel it is short, at most `MAX_FUEL_SAVE_COAST_S` (2 s), and from
  `SHORT_SHIFT_SHORT` 10% short short-shifts as well, fully at 25%. At
  Monza an F1 20% short burns 16% less a lap for about 4 s; 13% short, 13%
  less for 2.5 s (`fuel_test`).
- **Engine nursing** (`GameSession::engine_nurse`,
  `pit::engine_nurse_for`), racing or not: a car whose coolant is over
  `NURSE_HOT_C` (105 °C), or whose engine damage less its wear is over 10%
  (`NURSE_DAMAGE_PERCENT`), lifts and coasts up to `NURSE_COAST_S` (1 s)
  into each braking zone and short-shifts, both in proportion, fully at
  `engine_heat::OVERHEAT_C` (112 °C) or 40%; the same machinery as fuel
  saving, the more of the two driven.
- **Damage**: a damaged engine is planned at its power (above, Plans);
  the aero loss reaches the driver through `aero_load_share` and the
  steering pull through its steering loop.

Fuel, tyres, brakes and damage themselves: [vehicle-physics.md](vehicle-physics.md).

## Other drivers built on it

All call `ai_input_for` or the controller: the showcase renders ([../game/spectator.md](../game/spectator.md)); the cool-down driver
that takes a finished human's car (`COOLDOWN_SKILL` 75); the `[debug]
stand_in_driver` hook ([operations.md](operations.md)); the hotlap watch's
lone car (skill 110, `exact_line`, easing down a skill ladder after a crash
or a struck lap: [sessions.md](sessions.md)); the track guide's solo runs
([apexsim-content/docs/track-guide.md](../../../apexsim-content/docs/track-guide.md)).

## Checking it

```bash
cd server/sim
cargo test --lib -- ai_driver:: racecraft::     # the unit tests
cargo test --test all racecraft_test::          # a quicker driver passes a slower one; a car in a barrier reverses out
cargo test --test all ai_race_start_test::      # composed off the grid; Le Mans first lap (the yaw seam)
cargo test --test all determinism_test::        # also fuel_test an_ai_short_of_fuel_lifts_and_coasts, air_test the_ai_gets_round_in_a_gale
```

**The AI survey** is the acceptance test for anything that moves the AI,
barriers, walls, the centerline, the ground or the car physics, run in `server/sim/`:

```bash
cargo test --release --test all ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored --nocapture
```

`python scripts/survey_compare.py before.txt after.txt` (from `apexsim/`)
lines two captured outputs up track by track, with the totals, and stars a
track that moved.

Every circuit under `apexsim-content/tracks/{default,custom}`, a mixed field of 10
AI in each of an LMP2, an F1 and a GT3 (`yotota-lmp2`, `fugazzi-sf26`,
`posh-gt3rs`), 180 s from the lights. Per race it prints cars out of shape
at 3 s, car-seconds of contact, off the road and sliding, retirements and
passes; the retired cars are split by what did most to the zone that
went (`car`, `wall`, or `own`: kerbs, bottoming, landings, the engine).
Environment:

| Variable | Effect |
|---|---|
| `SURVEY_TRACKS=SpaFrankenchamps,Monzarella` | only these stems (a custom import too) |
| `SURVEY_CARS=KsFerrariSf70h,posh-gt3rs` | these cars instead of the three (a folder under `apexsim-content/cars/default`, or `custom` for an import) |
| `AI_TEST_SEED=n` | another seeded field (default 1); run 1-3 for a comparison |
| `SURVEY_AI_LEVEL=95` | the field at that AI level (spread over 4 points round it), not the mixed field |
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

- `ai_race_start_test lone_car_against_its_plan`: one AI alone (an ace
  unless `SURVEY_AI_LEVEL`), `LONE_LAPS` (3) laps per `SURVEY_TRACKS` and
  `SURVEY_CARS`, printing each lap's time, off and sliding seconds and every
  corner where it slid or left the road against the speed its plan set:
  whether a car can hold its own plan, without traffic. `LONE_PLAN=880,1060`
  prints the plan between those metres (speed, the line's radius, the share
  of the lock it takes); `SURVEY_EVENTS` and `SURVEY_TRACE=1,from,to` work
  as in the survey.
- `grip_probe_test skidpad_sweep` (`PROBE_CAR`): the peak lateral g a car
  holds at 20-65 m/s against `grip_coefficient` and against what its plan
  reads (`plan`); `PROBE_LINEAR`, `PROBE_NO_CAMBER`, `PROBE_EVEN_ROLL` take
  the load sensitivity, camber or the bars out to find what costs it.
- Pit surveys, in `server/server/`: `cargo test --release --test all
  pit_stop_test::pit_lane_survey -- --ignored --nocapture` and `ai_pit_survey` ([pit-lane.md](pit-lane.md);
  `PIT_TRACKS`, `PIT_TRACE`).
- `server/sim/tests/all/pad_driver_probe.rs` (run in `server/sim/`): the AI's line with a pad's hand error on
  top, through the steering aid or raw, to reproduce a player's tyre
  temperatures (`PAD_AID`, `PAD_NOISE`, `PAD_HOLD_S`, `PAD_SKILL`,
  `PAD_CAR`, `PAD_TRACK`, `PAD_LAPS`, `PAD_FULL`):
  `PAD_AID=0 PAD_NOISE=0.12 cargo test --release --test all pad_driver_probe:: -- --ignored --nocapture`.
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
- **A load-sensitive tyre is not its static-load coefficient.** Read at
  the static load (plus downforce) at one 40 m/s reference, an import held
  only 75-90% of its plan's grip on a skidpad (an F1's light wheels are
  well under AC's `FZ0`, so it even planned over its `grip_coefficient`),
  and its AI slid through every fast corner on its own. The lateral
  transfer, the load at each speed and the weaker axle bring it to the
  82-95% the shipped cars hold (`grip_probe_test skidpad_sweep`, the
  `plan` column).
- **Full pedal is the grip limit only on a shipped car.** The shipped
  cars' `max_brake_force_n` is about their tyres' grip and they all have
  ABS; an import's brakes are AC's, sized for its downforce at speed, and
  an F1 or a prototype has no ABS. Without `unlocked_pedal` its AI locked
  the fronts into every slow corner (a 919 lapped Monza in 148 s instead
  of 103 s, off the road at both chicanes).
- **Grip share goes in at the 0.75 power, not the root.** The physics
  would say root; the grip moves under the car through a corner as the
  tread heats, and with the root the AI left the road much more.
- **Off the road, the lap position is not the leg.** The physics' nearest
  point falls back to a whole-lap search when a car leaves its window, and
  can land on another leg nearby; anything steering a car back to the
  road reads `racecraft::leg_m`, not `track_progress`.
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
