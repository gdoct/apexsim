# Pit lane and pit stops

Each circuit's pit lane reaches the server as a baked sidecar. On it the
server runs a speed limiter, gives every car its own box, drives a human's
car down the lane on an autopilot, services the car at its box (tyres, fuel,
repairs, brake pads) and works a pit exit light. The AI plans its own stops
and drives the same route ([ai.md](ai.md)). The client draws the lane, the
garages and the lights from the same geometry, and shows a pit-stop panel
from telemetry and a `PitService` message.

Tyres, compounds, fuel, damage and brakes themselves are in
[vehicle-physics.md](vehicle-physics.md).

## Code

- `track-editor/core/src/dress.rs` - `build_pit_lane` (the lane laid from
  the dossier), `round_tight_bends`, `PitZone`.
- `track-editor/core/src/pit.rs` - `generate_pit_lane` (circuits whose
  dossier maps no lane).
- `track-editor/core/src/ue_export.rs` - `pit_layout` (limit stretch and
  garage run), `PitSidecar` / `bake_pit_sidecar`, the pit complex, paint and
  signs, `bake_walls` (garage collision boxes).
- `server/src/pit.rs` - `PitLane` (the loaded sidecar), `PitState` (per
  car, on `CarState::pit`), `deal_box`, `exit_closed`, `takes_over`,
  `drive_input`, `run_up_input`, `plan_service`, `plan_stop`,
  `tyres_for_the_track`, `recovery_pose`.
- `server/src/game_session.rs` - `update_pits` (with `anchor_on_lap`),
  `start_service`, `finish_service`, `plan_ai_stops`,
  `pit_autopilot_inputs`, `lane_traffic`.
- `server/src/physics.rs` - the limiter in `update_car_3d`,
  `RoadContact::PitLane`, `on_pit_lane_strip`.
- Client: `FApexCarTelemetry` pit fields, `UApexNetSubsystem::FindPitService`,
  `content/hud/default/pit_stop`, `ApexHudData::PitStopProgress`,
  `AApexRaceDirector::UpdatePitExitLights`.

## The lane in the track pipeline

The lane is part of the `.ats` scene, `authored: true` so grooming leaves it
alone ([../content/track-pipeline.md](../content/track-pipeline.md)).

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
- `ue_export::pit_layout` decides once where the limit holds (the longest
  stretch clear of the road) and where the garages stand (the longest run
  of it with `PIT_GARAGE_ROOM_M` behind the lane clear of every road). The
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
circuit but the Nordschleife has one; AC imports have none. A track without
one has no pit stops.

## The lane as a surface

On the road mesh the lane is its own triangles (`RoadContact::PitLane`,
[../content/road-mesh.md](../content/road-mesh.md)). On the centerline
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
- **Boxes.** Every car without one is dealt a box at once
  (`pit::deal_box`: the lowest box nobody holds, in `BTreeMap` order;
  shared, fewest holders first, only past the box count),
  `PitState::box_index`.
- **Limiter.** Between the limit lines `PitState::limiter` is set and
  `update_car_3d` scales the throttle by `clamp(limit - speed, 0, 1)`: it
  fades out over the last m/s below the limit.
- **Autopilot.** A human's car that drives into the lane (on it, short of
  the first limit line, past the road edge on the lane's side, pointing down
  it: `pit::takes_over`) becomes the server's until the lane's end:
  `PitState::driving`, its automatic gearbox on and steering aid off, the
  driver's own values kept in `restore_aids` and put back at the end.
  `pit_autopilot_inputs` replaces its input with `pit::drive_input`; the
  driver's headlights and flash still count. The AI enters the same route
  from `plan_ai_stops`.
- **`drive_input`**: the lane's middle, swinging onto the box's spot over
  the last 20 m and back out after the service; a Stanley steer at the
  front axle damped by the yaw rate the route does not ask for, no more lock
  than the fronts can use (the aid is off), the limit between the lines,
  the route's bends at 6 m/s^2, slower while wide or pointing across the
  lane, 8 m/s through the swing, a stop on the spot, queueing behind a car
  ahead on the same path. A car making no headway for `AUTOPILOT_STUCK_S`
  (4 s, not in service or held) is put back on its route
  (`pit::recovery_pose`), a last resort.
- **Service.** A car on the limiter, unserviced, under 1 m/s within
  `BOX_RADIUS_M` (2.5 m) of its own box's spot starts its stop
  (`start_service` -> `pit::plan_service`), held still by the physics. The
  crew works in order: tyres always (`F1_TYRE_CHANGE_S` 2.5 s, else
  `TYRE_CHANGE_S` 9 s; the driver's setup compound via
  `CarSetup::compound_index_for`, the AI's planned one), fuel where the
  rules allow (not an F1; 0.5 L or more at `REFUEL_LPS` 2 L/s; in a race to
  the remaining distance with the race margin), then repairs (0.5% or more
  summed over the zones, `REPAIR_S_PER_PERCENT` 0.04 s a percent) and new
  brake pads past `brakes::PAD_CHANGE_PCT`, both counted in the repair part.
  `finish_service` fits the set at the car's start temperature, adds the
  fuel, repairs (engine wear stays), fits pads and starts a new hybrid
  stint.
- **Exit light** (`pit::exit_closed`, the same for every car): red before a
  race's start (countdown, or a race not yet racing) and while a car on the
  track, not on the pit route, is within max(3 s at its speed, 25 m) of
  where the lane rejoins. A serviced car on the route stops short of the
  last line while it is red, for at most `EXIT_HOLD_MAX_S` (10 s), then
  goes (`PitState::held`).
- A car towed after retiring is parked at its own box
  (`update_retirements`).

## Wire

- `CompactCarState`: `tyre_wear` ([u8;4] %), `compound` (255 unknown),
  `pit_flags` (`PIT_FLAG_LIMITER` 1, `SERVICING` 2, `IN_LANE` 4,
  `AUTOPILOT` 8, `EXIT_CLOSED` 16, `HELD` 32), `service_ds` (seconds left,
  tenths), appended after `tow_pct`.
- `ServerMessage::PitService` (`PitServiceData`: car index, box, tyres /
  fuel / repair seconds in crew order, compound, litres, damage repaired,
  total), reliable, to every human in the session as the crew starts
  (`GameSession::take_pit_events`).
- Golden bytes: `cargo test --lib -- telemetry_compact_wire_format
  pit_service_wire_format car_setup_wire_format --nocapture` ->
  `ApexUdpGolden::S_TelemetryCompactPit`, `ApexGolden::S_PitService`,
  `ApexGolden::C_SetCarSetup` (the next-tyre knob). See
  [protocol.md](protocol.md).

## Client

- `FApexCarTelemetry::TyreWearPct`, `Compound`, `bInPitLane`,
  `bPitLimiter`, `bPitServicing`, `bPitAutopilot`, `bPitExitClosed`,
  `bPitHeld`, `ServiceSecondsLeft`; `UApexNetSubsystem::FindPitService`.
- The `pit_stop` HUD component (`content/hud/default/pit_stop`, visible on
  the autopilot in the lane, in service or held): PIT LANE / AUTOPILOT with
  the box number, PIT EXIT CLOSED / WAIT, and during the stop the countdown,
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
cd server
cargo test --test pit_stop_test     # limiter, service, a human in and out, aids put back, every car its own box,
                                    # the red light holds a car, the AI's stop, the lane without the road mesh
cargo test --lib -- pit::
# Every circuit (ignored, minutes):
cargo test --release --test pit_stop_test pit_lane_survey -- --ignored --nocapture
cargo test --release --test pit_stop_test ai_pit_survey -- --ignored --nocapture
```

`pit_lane_survey` drives a human's car into every circuit's lane, to its
box and out under the autopilot, and fails on a stall, a recovery or the
car's middle within 1.5 m of a lane edge. `ai_pit_survey` has an AI on worn
tyres plan, make and race on from a stop on every circuit. `PIT_TRACKS=A,B`
narrows either, `PIT_TRACE=1` prints the trip.
`the_pit_lane_is_a_pit_lane_without_the_road_mesh` checks every box and
every 10 m between the lines on every circuit on the centerline backend.

Client: `ApexSim.Hud.Pit.Progress`, `ApexSim.Net.Protocol.PitService`,
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
