# Pit lane and pit stops

Each circuit's pit lane reaches the server as a baked sidecar. On it the
server runs a speed limiter, gives every car its own box, drives a human's
car down the lane on an autopilot, services the car at its box (tyres, fuel,
repairs, brake pads) and works a pit exit light. The AI plans its own stops
and drives the same route ([ai.md](ai.md)). Each service counts toward
the race's mandatory stops and second compound, when the session sets them
(`PitState::stops`, `compound_changed`;
[sessions.md](sessions.md#session-rules-set-on-create)). The client draws the lane, the
garages and the lights from the same geometry, and shows a pit-stop panel
from telemetry and a `PitService` message.

Tyres, compounds, fuel, damage and brakes themselves are in
[vehicle-physics.md](vehicle-physics.md).

## Code

- `apexsim-content/track-editor/core/src/dress.rs` - `build_pit_lane` (the lane laid from
  the dossier), `round_tight_bends`, `PitZone`.
- `apexsim-content/track-editor/core/src/pit.rs` - `generate_pit_lane` (circuits whose
  dossier maps no lane).
- `apexsim-content/track-editor/core/src/ue_export.rs` - `pit_layout` (limit stretch and
  garage run), `PitSidecar` / `bake_pit_sidecar`, the pit complex, paint and
  signs, `bake_walls` (garage collision boxes).
- `server/sim/src/pit.rs` - `PitLane` (the loaded sidecar), `PitState` (per
  car, on `CarState::pit`), `PitPlan` (a driver's orders), `deal_box`, `deal_team_boxes`,
  `exit_closed`, `takes_over`, `drive_input`, `run_up_input`,
  `plan_service` / `plan_service_for`, `plan_stop`,
  `tyres_for_the_track`, `recovery_pose`.
- `server/sim/src/game_session.rs` - `update_pits` (with `anchor_on_lap`),
  `set_pit_plan`, `start_service`, `finish_service`, `plan_ai_stops`,
  `pit_autopilot_inputs`, `lane_traffic`.
- `server/sim/src/physics.rs` - the limiter in `update_car_3d`,
  `RoadContact::PitLane`, `on_pit_lane_strip`.
- Client: `FApexCarTelemetry` pit fields, `UApexNetSubsystem::FindPitService`,
  `hud/default/pit_stop`, `ApexHudData::PitStopProgress`,
  `AApexRaceDirector::UpdatePitExitLights`.

## The lane in the track pipeline

The lane is part of the `.ats` scene, `authored: true` so grooming leaves it
alone ([apexsim-content/docs/track-pipeline.md](../../../apexsim-content/docs/track-pipeline.md)).

- `dress::build_pit_lane` walks the dossier's polyline by its own length
  (resampled every 4 m, smoothed), reads each point against its own leg of
  the course, and displaces it only where needed: out to a pit wall's apron
  (`PIT_APRON_M`, 3 m) off the road where it is mapped closer (never so far
  in on the inside of a bend that it folds), blended onto the road edge
  over a taper at each end so every lane leaves and rejoins the track, then
  any bend under `PIT_LANE_MIN_RADIUS_M` (30 m) rounded off. The exit taper
  is read against the road nearest it (a lane far from the course loses its
  leg). Boxes: `PIT_GRID_BOXES` (24), within the room.
  `pit::generate_pit_lane` lays the lanes the dossiers lack to the same
  rules.
- `dress::PitZone`: nothing dressed (stands, buildings, car parks,
  landmarks...) may stand on the lane, its apron or the garages' depth
  behind it, tested over the whole footprint.
- `ue_export::pit_layout` decides once where the garages stand (the
  longest run of the longest stretch clear of the road with
  `PIT_GARAGE_ROOM_M` behind the lane clear of every road) and where the
  limit holds: from `PIT_ENTRY_LEAD_M` (60 m) before the first box, as a
  real entry line sits, to the end of the clear stretch, where a real
  lane's exit light stands. The
  sidecar, the garages and the paint share it, so a car stops where it sees
  its box. It also places `sign/pit_speed_limit` at the first limit line and
  `sign/pit_exit_light` at the last (the client's `ApexProps::FacesUpCourse`
  turns them toward the cars).
- `bake_walls` puts a garage's collision box behind its door (the module
  opens to the right of its yaw).

## The sidecar (`<Stem>.pit.msgpack`)

Written by `ats-export` beside the YAML (gitignored, shipped by
`build_release.ps1`; `ats::Sidecar::Pit` when an `.ats` lists it as
external), loaded by
`TrackLoader::load_pit_lane` into `TrackConfig::pit_lane`. `PitSidecar`
(version 1): width, speed limit, `lane_side`, the lane's centerline every 2
m, its length, `limit_start_m` / `limit_end_m` (lane stations),
`entry_station_m` / `exit_station_m` (track stations), and a `PitBoxSpot`
per box (the middle of its working lane, facing down it). Every shipped
circuit but the Nordschleife has one; an AC import has one when the layout has a
pit line and pit boxes ([apexsim-content/docs/ac-import.md](../../../apexsim-content/docs/ac-import.md)). A
track without one has no pit stops.

## The lane as a surface

On the road mesh the lane is its own triangles (`RoadContact::PitLane`,
[apexsim-content/docs/road-mesh.md](../../../apexsim-content/docs/road-mesh.md)). On the centerline
backend a point past the road edge within half the lane's width of the
sidecar's middle is the lane (`physics::on_pit_lane_strip`). Either way it
grips like the road (the water alone, `RoadSample::wet_grip`) and is **on
the track for the lap** (`RoadContact::off_track` is false for it), so a
lap with a stop counts.

## Each tick (`GameSession::update_pits`)

Called inside `update_air` before the physics, and in the countdown.

- **Placement.** A car within `LANE_SEARCH_MARGIN_M` (30 m) of the lane's
  bounding box (`PitLane::near`) is located on it (`PitLane::locate`,
  windowed by `PitState::lane_node`; a hint more than 30 m off the lane is
  dropped for a full search). Farther away the hint is cleared. On the lane
  the car's place on the lap is re-anchored near where the lane's own
  progress puts it (`anchor_on_lap`).
- **Boxes.** Every car without one is dealt a box at once, by team
  (`pit::deal_team_boxes`), `PitState::box_index`. A team is the cars of
  one model in one livery as worn (`GameSession::teams`, from
  `worn_liveries`: a human's pick, the AI's dealt in turn from the car's
  list), so the AI of one car with two liveries form two teams. The cars
  without a box are dealt in grid order: the first of a team takes the
  lowest run of free boxes long enough for its team-mates still to come
  (kept for them), a car whose team-mate has a box takes the free box
  nearest it (a late joiner beside its team), and with no such run or no
  free box the car is dealt as `pit::deal_box` deals (the lowest box nobody
  holds; shared, fewest holders first, only past the box count). A field of
  one car in one livery is one team: boxes in grid order from the entry
  end.
- **Limiter.** Between the limit lines `PitState::limiter` is set and
  `update_car_3d` scales the throttle by `clamp(limit - speed, 0, 1)`: it
  fades out over the last m/s below the limit. A car more than
  `LIMITER_BRAKE_FROM_MPS` (1 m/s) over it is braked, harder the further
  over (full at 6 m/s more), at most `LIMITER_BRAKE_MAX` (0.6) of the pedal:
  this, not a penalty, is what holds a driver who drives the lane
  themselves to the limit.
- **Autopilot.** A human's car that crosses the pit entry line (the first
  limit line, by the speed-limit board: on the lane, within
  `TAKEOVER_WINDOW_M` (40 m) past the line, past the road edge on the lane's
  side, pointing down it: `pit::takes_over`) becomes the server's until it
  is `HANDBACK_PAST_LINE_M` (1 m) past the exit line (the last limit line,
  by the exit light): `PitState::driving`, its automatic gearbox on and
  steering aid off, the driver's own values kept in `restore_aids` and put
  back at the end. The route asks for first whenever the car is in reverse
  or neutral (`drive_input`): held still on the brake, in a queue or at a
  red light, the automatic box would put reverse in, and the route's light
  creep away was too little throttle to take first again. The entry and exit tapers are the driver's: the car is
  handed back at the limit, or at what a bend of the lane itself before
  the line allows. The AI's route runs on to the lane's end
  (`pit::route_end_m`): its racing line knows nothing of the exit taper.
- **Closed under the safety car** (`RaceControl::pit_closed`, from its call
  until its queue has formed; [sessions](sessions.md#safety-car)): the AI
  does not turn onto its route, the autopilot takes no car in, and a car
  that drives in while it is closed (`PitState::entered_closed`) is not
  taken on by its box on that visit.
- **Manual pit lane.** A driver whose `SetDriverAids` sent `pit_autopilot:
  false` (`CarState::manual_pits`; Settings > Assists > Pit lane) is never
  taken over: they drive the lane on the limiter, stop within
  `BOX_RADIUS_M` of their box's spot for the crew (the same service rule
  as the autopilot's), and drive out. A recovery to the pits leaves such a
  car at its box for the crew, not on the autopilot.
  `pit_autopilot_inputs` replaces its input with `pit::drive_input`; the
  driver's headlights and flash still count. The AI enters the same route
  from `plan_ai_stops`.
- **`drive_input`**: the lane's middle, swinging onto the box's spot over
  the last 20 m and back out after the service; a Stanley steer at the
  front axle damped by the yaw rate the route does not ask for, no more lock
  than the fronts can use (the aid is off), the limit between the lines,
  the route's bends at 6 m/s^2 (a human's only up to its hand-back, so it
  does not slow for the exit taper it never drives), slower while wide or pointing across the
  lane, 8 m/s through the swing, a stop on the spot, queueing behind a car
  ahead on the same path. A car making no headway for `AUTOPILOT_STUCK_S`
  (4 s, not in service or held) is put back on its route
  (`pit::recovery_pose`), a last resort.
- **Service.** A car on the limiter, unserviced, under 1 m/s within
  `BOX_RADIUS_M` (2.5 m) of its own box's spot starts its stop
  (`start_service` -> `pit::plan_service`), held still by the physics. The
  crew works in order, unless the driver's plan says otherwise (below): tyres (`F1_TYRE_CHANGE_S` 2.5 s, else
  `TYRE_CHANGE_S` 9 s; the driver's setup compound via
  `CarSetup::compound_index_for`, the AI's planned one, and the planned
  one too for a stop the server planned for a car it drives for its
  driver, a debug stand-in or an away driver, unless the driver's plan
  names a compound), fuel where the
  rules allow (not an F1, unless the race outruns its tank:
  `GameSession::refuels`, [ai.md](ai.md); 0.5 L or more at `REFUEL_LPS` 2 L/s; in a race to
  the remaining distance with the race margin; on a circuit with no pit
  lane, the Nordschleife, a car a stop would refuel is topped up at the
  line instead as it starts each lap, to the flag with the margin and
  reserve and no time lost: `GameSession::update_line_refuel`), then repairs (0.5% or more
  summed over the zones, `REPAIR_S_PER_PERCENT` 0.04 s a percent) and new
  brake pads past `brakes::PAD_CHANGE_PCT` (or discs and pads past
  `DISC_CHANGE_PCT`, `DISC_CHANGE_S`), all counted in the repair part.
  An AI's splash (`PitState::fuel_only`, [ai.md](ai.md)) is the fuel alone:
  the tyres stay on and nothing is repaired.
  `finish_service` fits the set at the car's start temperature (when the
  stop changes tyres: `PitState::service_tyres`), adds the fuel, repairs
  (engine wear stays), fits pads (or discs) and starts a new hybrid stint.
- **A driver's pit plan** (`ClientMessage::SetPitPlan`, `pit::PitPlan`,
  kept per driver by `GameSession::set_pit_plan` until the next one; any
  time in a session). Each field at its default is the crew's own choice
  above, so a driver who never sends one, and every AI, is serviced as
  described. `keep_tyres` leaves the set on (no tyre time); `compound`
  picks the set going on (an index into the car's compounds, clamped to
  the list) in place of the setup's; `fuel_l` puts in that many litres
  (0 for none) in place of the need, still capped by the tank and the
  stint's energy, and none for a car the rules do not refuel; `no_repair`
  leaves the damage; `pads` `true` fits new pads however worn the old
  ones are (discs too when past `DISC_CHANGE_PCT`), `false` leaves pads
  and discs on; `pad_set` (-1 endurance, 0 standard, +1 sprint) is the
  set new pads are, and fitting them moves the setup's `brake_pads` knob
  to it (`set_car_setup`: the car is simulated with them from the end of
  the stop). Each part's time follows what is done (`plan_service_for`).
  `box_this_lap` is the driver's pit request: `PitState::requested`, sent
  back in telemetry as `PIT_FLAG_REQUESTED` until the stop is made (or a
  plan without it). It is a call to the crew only: the driver still
  drives in.
- **Exit light** (`pit::exit_closed`, the same for every car): red before a
  race's start (countdown, or a race not yet racing) and while a car on the
  track, not on the pit route, is within max(3 s at its speed, 25 m) of
  where the lane rejoins. A serviced car on the route stops short of the
  last line while it is red, for at most `EXIT_HOLD_MAX_S` (10 s), then
  goes (`PitState::held`). A driver on a manual lane who crosses the last
  line while it is red is penalised under `pit_exit_penalty`.
- **Penalties** ([sessions](sessions.md#penalties)): a drive-through is a
  visit from the first limit line to the last with no stop
  (`PitState::drive_through`; the route goes on past the box), a stop-go
  is the next stop, held with no work (`PitState::stop_go`) and not a stop
  for the pit rule.
- A car towed after retiring is parked at its own box
  (`update_retirements`).

## Wire

- `CompactCarState`: `tyre_wear` ([u8;4] %), `compound` (255 unknown),
  `pit_flags` (`PIT_FLAG_LIMITER` 1, `SERVICING` 2, `IN_LANE` 4,
  `AUTOPILOT` 8, `EXIT_CLOSED` 16, `HELD` 32, `REQUESTED` 64),
  `service_ds` (seconds left, tenths), appended after `tow_pct`.
- `ClientMessage::SetPitPlan` (`pit::PitPlan`: `keep_tyres`, `compound`,
  `fuel_l`, `no_repair`, `pads`, `pad_set`, `box_this_lap`, each left off
  the wire at its default), reliable, from a driver. Client:
  `ApexProtocol::EncodeSetPitPlan(FApexPitPlan)`,
  `UApexNetSubsystem::SetPitPlan`; `FApexCarTelemetry::bPitRequested`.
  On screen: the pause menu's PIT PLAN page and the `BoxThisLap` key
  (`UApexPitPlanSubsystem`, [game/client.md](../game/client.md)), the
  HUD's `pit.requested` and BOX badge.
- `ServerMessage::PitService` (`PitServiceData`: car index, box, tyres /
  fuel / repair seconds in crew order, compound, litres, damage repaired,
  total), reliable, to every human in the session as the crew starts
  (`GameSession::take_pit_events`).
- `ServerMessage::PitBox` (`PitBoxData`: the box, its stop spot and yaw,
  the spot's distance off the lane's middle (positive left: the garage
  side), the lane's width, `BOX_RADIUS_M`, `manual` when the driver drives
  the lane, left off the wire when not), reliable, to each human alone once
  their box is dealt, again after `add_player` or `resume_seat`, and when
  their pit autopilot is switched (`GameSession::take_pit_box_news`).
- Golden bytes: `cargo test --lib -- telemetry_compact_wire_format
  pit_service_wire_format pit_box_wire_format pit_plan_wire_format
  car_setup_wire_format --nocapture` ->
  `ApexUdpGolden::S_TelemetryCompactPit`, `ApexGolden::S_PitService`,
  `ApexGolden::S_PitBox`, `ApexGolden::C_SetPitPlan` / `C_SetPitPlanEmpty`,
  `ApexGolden::C_SetCarSetup` (the next-tyre knob). See
  [protocol.md](protocol.md).

## Client

- `FApexCarTelemetry::TyreWearPct`, `Compound`, `bInPitLane`,
  `bPitLimiter`, `bPitServicing`, `bPitAutopilot`, `bPitExitClosed`,
  `bPitHeld`, `ServiceSecondsLeft`; `UApexNetSubsystem::FindPitService`,
  `GetOwnPitBox`.
- The box marker (`AApexRaceDirector::UpdatePitBoxMarker`,
  `ApexRaceDirectorPit.cpp`): for a driver on the manual pit lane (the
  `PitBox`'s `manual`, not telemetry's autopilot bit, which is off for
  everyone before a takeover), from
  coming into the lane until the crew have serviced the car, a yellow
  outline about a car's footprint on the ground at their box's spot, a stop
  line across its front and a lollipop board ahead of it. Instanced engine
  cubes, laid once per box on the ground traced from just above the spot
  (from higher it found the garage canopy; a trace that starts inside the
  pit building's collision, which reaches over the working lane, is made
  again without it).
- The `pit_stop` HUD component (`hud/default/pit_stop`, visible in
  the lane on the autopilot or the limiter, in service or held; not on the
  entry taper, which a car racing past the pit entry can touch): PIT LANE / AUTOPILOT with
  the box number, PIT EXIT CLOSED / WAIT, on the limiter without the
  autopilot BOX n · IN 85 M (`pit.box_ahead_m`) then PIT LANE · LIMITER,
  and during the stop the countdown,
  the part under way and a bar of up to three segments sized by each part's
  share (`ApexHudData::PitStopProgress`). Data points `pit.*`:
  [../game/hud-modding.md](../game/hud-modding.md). The tyre row shows
  compound and wear; the PIT badge LIMITER / AUTOPILOT / SERVICE.
- The pit exit light's lamps (`AApexRaceDirector::UpdatePitExitLights`, the
  shared `glow_pit_light_*` material instances) are red while any car's
  frame says the exit is closed.
- The hotlap garage's "Next tyres" row sets the compound knob.

## Checking it

```bash
cd server/server
cargo test --test all pit_stop_test::   # limiter, service, a driver's pit plan and pad set, a human in and out, aids put back,
                                        # every car its own box, the red light holds a car, the AI's stop, the lane without the road mesh
cargo test --manifest-path ../sim/Cargo.toml --lib -- pit::   # the unit tests live in the simulation crate
# Every circuit (ignored, minutes):
cargo test --release --test all pit_stop_test::pit_lane_survey -- --ignored --nocapture
cargo test --release --test all pit_stop_test::ai_pit_survey -- --ignored --nocapture
```

`pit_lane_survey` drives a human's car into every circuit's lane (along the
track, then down the lane's mouth, slowing for the lane's bends as a driver
would), to its box and out under the autopilot, and fails on a stall, a recovery, the
car's middle within 1.5 m of a lane edge, or a hand-back more than 1.5 m/s
under what the lane allows there (`pit::lane_bend_mps`). `ai_pit_survey` has an AI on worn
tyres plan, make and race on from a stop on every circuit. `PIT_TRACKS=A,B`
narrows either, `PIT_TRACE=1` prints the trip.
`the_pit_lane_is_a_pit_lane_without_the_road_mesh` checks every box and
every 10 m between the lines on every circuit on the centerline backend.

Client: `ApexSim.Hud.Pit.Progress`, `ApexSim.Net.Protocol.PitService`,
`ApexSim.Net.Protocol.PitBox`,
`ApexSim.Net.Udp.GoldenDecode`, `ApexSim.Spectator.PitFlags`.

To put a stop on screen unattended, the `[debug]` hooks (`pit`, `wear=P`,
`stand_in_driver`) in [operations.md](operations.md).

## Traps

- **The offline tools drive the centerline backend** (the AI survey, both
  pit surveys, the guide, showcase renders). A lane change must hold on it
  as well as on the road mesh; on the centerline backend the lane exists
  only through `on_pit_lane_strip`.
- **The window hint goes stale.** A lane hint carried round a lap drifts to
  whichever node is nearest; outside the lane's box the hint is cleared and
  a far hint triggers a full search. Keep both or the AI never finds the
  lane's mouth.
- **The lap position is anchored to the lane while in it** (`anchor_on_lap`):
  round the inside of a hairpin the physics' windowed centerline search
  latches onto the pit straight, and the car is handed back on the wrong
  leg.
- **Limit stretch, garages, paint, signs and sidecar must come from one
  `pit_layout`**, or a car stops where it cannot see its box.
- **Garage collision faces the door**, not away from the nearest
  centerline: on a circuit with an inner loop the wrong side puts the walls
  across the lane.
- **Nothing dressed may sit in the `PitZone`**: test the whole footprint,
  not its centre.
- **The AI's run-up races on the road** and only hands to `drive_input` 10 m
  short of the lane; letting the lane drive the whole 250 m approach cut
  across corners onto the run-off.
- **The pit lane counts as on the track** for lap validity; changing
  `RoadContact::off_track` changes every stop's lap.
