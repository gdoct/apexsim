# Sessions, game modes and race rules

A session is one track, one set of rules and the cars on it, ticked by the
server as a `GameSession`. This doc covers session kinds, states and game
modes, the rules a host sets on create, race start and finish (over laps or
against a clock), starting order and qualifying, lap timing and records,
recovering a stuck car, the hotlap mode with its ghost, and the watched
hotlap. The client pieces that
belong to these rules are described briefly beside them.

## Code

- `server/src/data.rs`: `SessionKind`, `SessionState`, `GameMode`,
  `RaceSession`, `DamageLevel`, `AllowedAssists`, `DriverAids`,
  `clamp_race_seconds`
- `server/src/game_session.rs`: per-mode ticks (`tick_*`), `set_game_mode`,
  `start_countdown_mode`, `line_up_on_grid`, `assign_finish_positions`,
  `update_race_clock`, `apply_grid_order`, `note_qualifying_lap`,
  `hotlap_relocate`, the watched hotlap (`start_hotlap_watch`,
  `update_hotlap_watch`, `hotlap_watch_driver`)
- `server/src/server.rs`: `ServerState::create_session` (resolves the
  conditions, clamps the rules, seeds the AI, starts a watched hotlap)
- `server/src/game_loop/dispatch.rs`: message handlers;
  `start_demo_session` for watch-only kinds
- `server/src/game_loop/tick.rs`: ticking, removal of empty sessions (and
  of sessions whose drivers are all away past the grace), lap timing out,
  qualifying and record submission
- `server/src/game_loop/lifecycle.rs`: disconnects, held seats
  (`hold_seat_for_rejoin`, `give_up_seat`); `GameSession::hold_seat` /
  `resume_seat` / `away_input`
- `server/src/recovery.rs` and `GameSession::recover_car` /
  `update_recoveries`: back to track and back to pits
- `server/src/laps.rs`, `records.rs`, `grid_order.rs`,
  `track_guide.rs` (`wire_corners`)
- Client: `UI/ApexRootWidget.cpp`, `UI/ApexSessionCreateWidget.cpp` (model
  `ApexCreateSessionModel.h`), `UApexMenuFlowSubsystem`, `UApexNetSubsystem`,
  `UApexHotlapWidget`, `AApexGhostCarActor`, `FApexTimingBoard` (ApexSimNet),
  `Race/ApexQualifyingBoard.h`

## Session kinds

`CreateSession.session_kind`:

| Kind | Value | Server behaviour |
|---|---|---|
| `Multiplayer` | 0 | A normal, listed session. |
| `Practice` | 1 | Same as Multiplayer on the server; the client uses it for single-player sessions. |
| `Sandbox` | 2 | Same as Multiplayer (no client sends it). |
| `Demo` | 3 | The menu's backdrop race: AI only, counted straight into a race (`DEMO_COUNTDOWN_SECONDS`, 8), no timing lines sent. |
| `HotlapWatch` | 4 | One AI car lapping alone for its creator (see "Watching a hotlap"); timing lines are sent. |

`SessionKind::is_watch_only` (Demo or HotlapWatch) is what matters: such a
session is unlisted (`SessionVisibility::Private`), `JoinSession` refuses it,
the creator joins as a spectator (`your_grid_position` 0), no replay is
recorded, debug hooks are not applied, and it lives as long as its spectator
(the "no human participant, remove it" rule in `tick.rs` skips it). The
demo's client side is in [cameras](../game/cameras.md) and
[spectator](../game/spectator.md).

## Session state and game mode

Two separate fields on `RaceSession`:

- `state: SessionState` (`Lobby`, `Countdown`, `Racing`, `Finished`)
  decides what is sent: rosters, telemetry and driver feedback go out only in
  `Countdown`, `Racing` and `Finished`; a replay records while `Racing`. A
  `Finished` session is removed after `[server] session_timeout_seconds`
  unless started again.
- `game_mode: GameMode` decides what `GameSession::tick` simulates:

| Mode | Value | Tick | On entry (`set_game_mode`) |
|---|---|---|---|
| `Lobby` | 0 | nothing | default |
| `Sandbox` | 1 | nothing; cars stand, free camera on the client | state `Racing` |
| `Countdown` | 2 | cars held on the grid (`update_car_on_grid`: gear and revs only), pit exit red; at zero `set_game_mode(next_mode)` | 10 s if set directly |
| `DemoLap` | 3 | an AI drives with full physics | humans removed from the cars and made spectators; an AI spawned if none |
| `FreePractice` | 4 | full physics, collisions, lap timing | state `Racing`, refuelled for practice |
| `Replay` | 5 | nothing (placeholder, never built) | |
| `Qualification` | 6 | `tick_hotlap` (see "Qualifying") | as Hotlap |
| `Race` | 7 | `tick_racing`: physics, race clock, classification, finish | state `Racing`, `race_start_tick`, pole's lap starts on green, auto boxes take first |
| `Hotlap` | 8 | like practice, but cars `in_garage` are neither ticked nor collided | state `Racing`, every human put in the garage |

**Changing mode is the host's** (anyone else gets `Error 403`, and both
broadcast `GameModeChanged`): `SetGameMode { mode }` switches at once, with
no transition rules; `StartCountdown { countdown_seconds, next_mode }` runs
`start_countdown_mode` (capped at `MAX_COUNTDOWN_SECONDS`, 60), which hands
over to `next_mode` at zero. A countdown into `Race` first calls
`line_up_on_grid`: every car back on its slot in start order, clean (laps,
times, finish position, damage), with race fuel and grid tyres, keeping its
aids and damage rule. The create screen offers Free practice, Sandbox,
Hotlap, Qualifying and Race; the client never counts into a hotlap
(`UApexNetSubsystem::StartCountdown` sends `SetGameMode` for it).
`StartSession` (host only) is a countdown into a race: from `Lobby` it runs
`start_countdown_mode(START_SESSION_COUNTDOWN_SECONDS, Race)` (5 s) and
sends `SessionStarting { countdown_seconds: 5 }`; for a session already
under way it is refused with `Error 409`.

**Joining a session under way.** `GameSession::refuses_drivers` decides
whether `JoinSession` may seat another driver now: a running practice,
sandbox, hotlap or qualifying session takes one (a hotlap or qualifying
driver arrives in the garage, as the drivers there when it began did), a
race takes them until the green light (a car joining in the countdown gets
the next free grid slot), and a race under way or a finished session
refuses with `Error 409`. `JoinAsSpectator` is never refused for the
session's state. The lobby's listing (`SessionSummary.State`) follows the
session: the tick hands each change of state to the lobby
(`GameSession::take_state_change`, `LobbyManager::set_session_state`).

## Session rules (set on create)

Each is a `CreateSession` field, kept on `RaceSession` and echoed in
`SessionJoined` (to spectators too). Optional fields are left off the wire at
their default, so an older client's bytes are unchanged. Weather and air are
in [conditions](conditions.md).

**Allowed assists**: `allowed_assists` (`AllowedAssists { abs,
traction_control, auto_gearbox, steering_assist, racing_line }`, all
default true). The server enforces it with `AllowedAssists::clamp` when a car
is seated and on every `SetDriverAids`, and sends no `RacingLine` when the
line is forbidden. The aids themselves are per driver: `SetDriverAids {
auto_gearbox, steering_assist, abs, traction_control }`, sent by
`UApexRootWidget::SendDriverAids` on join and on any change in the settings
Assists tab. They run on the server (only it has the tyres); ABS and TC are
`Option`s on `CarState`, `None` meaning "as the car.toml says" (the AI, an
old client). TC LOW holds a spinning wheel at peak slip; HIGH also caps drive
at what the friction circle leaves beside the lateral force
(`TC_HIGH_LATERAL_MARGIN`, `physics.rs`). The client dims a locked row
(`GetAllowedAssists`, `RefreshAssistLocks`) and keeps the player's choice for
the next session.

**Damage**: `damage` (`DamageLevel` `Off` / `Reduced` / `Full`, default
Full), the same for every car, AI included: stamped as
`CarState::damage_level` when seated (`set_damage` for the AI seated at
creation) and multiplying every accrual by `DamageLevel::scale` (0,
`REDUCED_SHARE` 0.5, 1). Client: `UApexMenuFlowSubsystem::CreateDamage`,
`GetSessionDamage`, HUD `damage.level`. The model is in
[vehicle physics](vehicle-physics.md).

**AI level**: `ai_skill` (`Option<u8>`, clamped to 70-110; absent is the
mixed field). `ai_driver::field_skills` spreads the field over
`AI_FIELD_SPREAD` (4) points round the level, or from novice to ace without
one; what skill changes is in [AI](ai.md). Client: the "AI level" stepper
(`CreateAiSkill`, `ApexAiSkill::Label`), `GetSessionAiSkill`.

**Length**: `lap_limit`, or `race_seconds` (below).

## Race start and finish

A car is seeded onto the centerline when put on the grid
(`physics::seed_track_progress`); lap 1 starts as a grid car crosses the
line, or on green for pole (`physics::start_lap_on_green`).

`assign_finish_positions` (every race tick, after `update_race_clock`)
classifies a car (`CarState::finish_position`) when it completes the winning
lap (`finish_lap`: `lap_limit`, or a timed race's final lap), ordered by laps
completed, then crossing tick, then distance past the line. When the winner
is in, a deadline is set at max(`FINISH_GRACE_MIN_SECONDS` 60,
`FINISH_GRACE_LAPS` 2 x the winner's average lap); the session goes
`Finished` (`is_race_complete`) when every car is classified or undrivable,
or at the deadline. A finished human's car is driven by the server
(`cooldown_input`, an AI at `COOLDOWN_SKILL` 75), because the last input
received otherwise stays applied.

Client: the HUD ranks finishers first (`ApexRace::RanksAhead`), cars still
racing get a toast when P1 takes the flag, and the race view stays up after
the local car finishes: 1 s after the flag the root widget begins the finish
view (`UApexRootWidget::BeginFinishView`: drive input off, HUD hidden, the
director's panorama over the car the cool-down driver now has; see
[cameras](../game/cameras.md#finish-view-apexfinishcamfmove)), and 3 s
later `UApexRaceResultsWidget` comes up over the race, translucent: the
classification (race time, gaps, laps down), provisional until the session
ends. A row of a car still racing films it (`FocusFinishCar`, the broadcast
camera locked on it), the player's own row brings the panorama back; H or
pad Y folds the card to a tab; Drive again (once the race is over), Back to
lobby, Save replay. A driver whose race ends without the flag (out of time,
or the last car home, where `Finished` and the flag arrive together) gets
the same view on `Finished`. The `SessionResults` screen remains for
spectators and the other modes. `UApexSessionRecorder` records the frame
that carries `Finished` before freezing, or the last car home would read DNF.
`apexsim.finish [view|car N|panorama|end]` opens the view mid-race for
unattended checks.

## Timed races

`CreateSession.race_seconds` is clamped to `MIN_RACE_SECONDS` (60) ..
`MAX_RACE_SECONDS` (24 h); a timed session has `lap_limit` 0. Echoed in
`SessionJoined.RaceSeconds` and listed in `SessionSummary.RaceSeconds`.

The rule is the endurance one. The clock starts on green. When it runs out
(`update_race_clock`) the leader's current lap becomes the last
(`RaceEnd::final_lap`); the first car to complete it wins, and every other
car takes the flag the next time it crosses the line, a lap down or not
(`RaceEnd::flag_laps`). Since classification is laps first, a lapped car
that crossed early drops behind a lead-lap car crossing later. If nobody on
the lead lap completes it, a deadline set when the clock ran out still ends
the race. Fuel and the AI's stop plan count laps from the clock (see
[AI](ai.md)).

Wire: `CompactTelemetry.race_clock` (`RaceClock { left_ms, final_lap }`,
only in a timed race counting in or running). Client: the create screen's
Laps / Time switch along `ApexRaceLength::LadderMinutes` (5 min to 24 h;
`bCreateTimedRace`, `CreateRaceMinutes`, `EffectiveRaceSeconds()`),
`FApexTelemetryFrame::RaceLeftMs` / `RaceFinalLap`, `GetSessionRaceSeconds`,
HUD `race.timed`, `race.time_left_s`, `race.final_lap`, `lap.final`,
`fmt_clock` (see [HUD modding](../game/hud-modding.md)).

The server's replay of a long race streams compressed frames from a writer
thread (`replay::ReplayRecorder`, `replays/.recording_<session>.frames`); a
full queue drops frames rather than stall the loop, and a session removed
without finishing has its recording discarded by the 5 s orphan sweep
(format: [spectator](../game/spectator.md)).

## Starting order and qualifying

A race grids by **driver references**, because at create time only the AI
are seated. `CreateSession.grid_order` (`Vec<String>`, `grid_order::sanitize`)
lists drivers first to last as `@host`, `@ai:N` (the Nth AI car, 1-based, in
seating order) or a name (a human's or an AI's). `line_up_on_grid` calls
`apply_grid_order` first:

- a requested order: listed drivers lead in order (`grid_order::resolve`,
  each claimed once, a name matching the first unclaimed driver of it),
  unmatched references are skipped, the rest follow in seating order;
- else this session's qualifying: the classified lead, the rest behind
  (`grid_order::complete`);
- else the seating order stands.

Names come from `GameSession::set_driver_name` (on create and join) and the
AI profiles.

**Qualifying classifies.** In `Qualification` every legal lap goes through
`note_qualifying_lap` (from `tick.rs`): each driver's best and the tick it
was set; `qualifying_order` is fastest first, earlier of equal times ahead.
The first qualifying lap clears any requested `grid_order`, so Qualification
then Race in one session grids from the result. Each improvement goes to
`RecordStore::submit_qualifying` off the loop, which keeps the newest
`MAX_QUALIFYING_RESULTS` (50) in `qualifying.json`, one per session, AI
included. `RequestQualifyingResults { track_config_id }` is answered with
`QualifyingResults`, newest first.

**Qualifying runs like a hotlap with an outlap**: `tick_hotlap` and
`hotlap_relocate`, humans start in the garage, the AI drive on. Going out
puts the car at the pit exit (`qualifying_outlap_pose`: the lane's
`exit_station_m` when in the first half of the lap, plus
`QUALIFYING_OUT_PAST_LINE_M` 30 m; a teleport, not a drive down the lane),
always on cold tyres, with `CarState::outlap` set so lap 1 starts only at the
next crossing of the line. There is no session length or qualifying results
screen.

**Client.** The create screen's Race tab has a Starting order row
(`ApexCreateSession::`): chips for the seating order, a "Custom" order and
the track's three newest stored results (`RequestQualifyingResults`,
`OnQualifyingResults`), a "Your grid slot" stepper, and a grid preview where
drivers are dragged between slots (handled in `NativeOnMouseButtonDown/Move/Up`;
the slot buttons are hit-test invisible because a button activates on the
press). `ApexCreateSession::OrderFromResult` maps a stored result: host by
name, the result's AI to this session's AI in qualifying order, other humans
by name. The order lives on the flow (`CreateGridOrder`, `CreateGridResultId`,
`CreateGridTrackId`), resets with the track, and is sent through
`GridOrderToSend` (empty for the plain seating order). In qualifying the
garage (`UApexHotlapWidget::SetQualifying`) drops the replay, ghost and tyre
toggles and has TUNE | SCOREBOARD: the scoreboard (`ApexBoard::Build`) lists
every car fastest first, and clicking one (`OnWatchCar`) points the spectator
camera at it (`SetSpectating`, `FocusCar`).

## Lap timing, track limits and records

The stopwatch is the server's. The lap counter lives in
`physics::update_track_progress_3d` (on the anti-shortcut checkpoints), which
returns a `laps::LapEvent` for every timing line crossed.

**Sectors**: three per lap, at the track file's own `sectors` (node indices)
when it names two, else even thirds. Each crossing is timed at the tick rate
and sent reliably as `LapTiming` (car index, lap, sector, times, validity,
personal/session best flags) to the session's humans; boundaries go out on
join as `TrackSectors`. The last split is what the other two leave, so the
three always add up to the lap.

**Track limits** strike a lap when all four wheels are past the kerb band for
`laps::TRACK_LIMITS_SECONDS` (0.2 s; `CarState::wheels_off_track`), or when
the car reaches the line without every checkpoint. A struck lap is timed and
counts, but cannot be a best. Telemetry's `lap_flags` bit 0 is "this lap
struck", bit 1 "last lap struck", beside `last_lap_time_ms` and
`best_lap_time_ms`.

**Records** (`[records] enabled` / `dir`, default `./records`): each
driver's best legal lap on a track in a car, in `lap_records.json`, keyed by
the driver's **name**. The AI never sets one. The driver gets `LapRecord`
(theirs, the track record and holder, `HasGhost`) on joining and on beating
it. A corrupt file is logged and treated as empty; writes run on a blocking
task from the broadcast phase, never under the state lock.

**Ghost traces**: each human's lap is sampled at `records::GHOST_SAMPLE_HZ`
(20 Hz); a new personal best stores its trace under `ghosts/`, replacing the
record's previous one.

Client: `FApexTimingBoard` (`UApexNetSubsystem::GetTimingBoard`) tallies
splits and bests; the HUD paints the sector strip (purple session best,
green personal best, amber slower), "LAP INVALID", and a delta against the
quickest legal lap seen.

## Reconnecting

A driver whose connection is lost keeps their seat. `AuthSuccess` hands every
client a `resume_token`; a client that connects again with it in
`Authenticate` gets the same player id back
([protocol.md](protocol.md#connection-flow)), and with it the seat.

- **Held seat.** A lost connection (stream closed, heartbeat timeout) holds
  the seat instead of removing the car (`GameSession::hold_seat`): the car
  stays in the session and the server drives it with the cool-down driver
  (`away_input`, steering aid off as after the flag), or parks it in its
  garage in a hotlap or qualifying session. A lap the server drives is never
  a record. The lobby keeps the seat counted (`LobbyManager::hold_seat`), so
  the session is not given away beyond its size.
- **Offer and rejoin.** On a resumed `Authenticate` the server sends
  `RejoinAvailable { session_id, track_id, session_kind }` after the lobby
  state. `JoinSession` with that id takes the seat back (`resume_seat`): the
  same car where it is now, with the join's `SessionJoined`, racing line,
  setup sheet, sectors and record, and the roster again on the next tick.
  A resume that overtakes the old connection's timeout drops the old
  connection; its end is then ignored (the player is already back).
- **Giving it up.** Creating or joining another session (not a demo or a
  watched hotlap) gives the held seat up (`give_up_seat`), as do
  `Disconnect` and a dashboard kick (the kick also forgets the resume
  token). `LeaveSession` leaves what the connection is in, never a seat
  held elsewhere: the menu sends it with nothing joined.
- **The last driver.** While any human in the session is connected, held
  seats wait for as long as the session runs. Once every human is away the
  session ends after `[server] reconnect_grace_seconds` (60) unless one
  comes back (`GameSession::orphaned_since_loop_tick`, checked in the tick).
  Spectators do not keep it alive.
- Resume tokens are kept only for connected players and players holding a
  seat (`TransportLayer::prune_resume_tokens`, every second).

Client: `UApexNetSubsystem` saves the resume token per server
(`[ApexSim.Resume]` in GameUserSettings), so a restarted game can rejoin too,
and keeps the offer (`HasRejoinOffer`, `OnRejoinOfferChanged`) until a join,
a lost connection, or the session leaving the lobby's list. The main menu
shows it as a banner over the hero ([client.md](../game/client.md#main-menu-uiapexmainmenuwidgetcpp)).
A lost connection takes the client back to the menu; a rejoin's first frame
opens the race view again.

## Recovering a stuck car

Walls stop cars, so a human can end up nose-in against a barrier.
`ClientMessage::RecoverCar { destination }` (`RecoverDestination`: `Track`
0, `Pits` 1) puts the car back at a cost (`GameSession::recover_car`,
`crate::recovery`). It is refused with `Error 400` outside free practice,
a running race, a hotlap or qualifying; for a car moving faster than
`MAX_SPEED_MPS` (5 m/s), out of the race (undrivable or towed), finished,
in the garage, on the pit route or already recovering.

- **Track**: on the centerline at the car's own station (its nearest
  centerline point), or the first of `SPOT_TRIES` spots `STEP_BACK_M` (15
  m) behind it that is `CLEARANCE_M` (10 m) clear of every other car,
  never back over the line, pointing along the lap, in first. The lap in
  progress is struck (`laps.invalid`) and its ghost trace dropped. The
  station barely moves, so the lap counter and its checkpoints carry on.
- **Pits**: parked at the car's own box. In a hotlap or qualifying session
  it is the garage (`hotlap_relocate`). A towed car crosses no checkpoint,
  so the lap it was on counts only if it had passed them all.

The car keeps its fuel, tyres, damage, aids and timing; only what moves is
reset (`place_still`). It is then **held** (`CarState::recovery`): still,
its input ignored (the physics freezes it as a serviced car), and a ghost
(`CarState::is_ghost`): out of both collision passes, the wake, the DRS
gaps and the AI's racecraft. `update_recoveries` counts the hold down:
`TRACK_HOLD_S` (8 s) on the track, `PITS_HOLD_S` (30 s) at the box. A car
on the track is then let go once no car is closing on it from behind
(`traffic_clear`: within 3 s at its speed or 30 m), for at most
`RELEASE_WAIT_MAX_S` (15 s) more. A car at its box is handed to the pit
autopilot: the crew services it (tyres, fuel where allowed, repairs) and
drives it out, as after a stop ([pit lane](pit-lane.md)).

Telemetry carries the hold as `recover_ds` (tenths, 0 none, at least 1
while waiting for traffic), appended after `ers_stint_pct`.

**Client.** The pause menu shows BACK TO TRACK and BACK TO PITS while the
local car is out on the track in a running session (BACK TO PITS gives way
to BACK TO GARAGE in a hotlap or qualifying); a refusal comes back as a
toast. The HUD's car state badge reads HELD and the seconds left
(`car.recovering`, `car.recover_s`). Console: `apexsim.recover [track|pits]`.

## Hotlap

`GameMode::Hotlap`: time attack, several drivers at once, each with their
own garage; any AI keep driving.

Every human starts **in the garage**: parked on its grid slot with
`CarState::in_garage`, out of the tick and both collision passes, and
`lap_flags` bit 2 (`LAP_FLAG_IN_GARAGE`) set. `HotlapRelocate { destination,
cold_tyres }` moves the driver's car (`hotlap_relocate`; refused with
`Error 400` outside a hotlap or qualifying):

- `Track`: on the centerline `HOTLAP_RUNUP_M` (300 m, less on a short track)
  before the line, in first, so the first flying lap is timed from the line;
  cars going out together are queued `HOTLAP_SPACING_M` (30 m) apart. Tyres
  at the compound's optimum, unless `cold_tyres` (optional on the wire) asks
  for the garage start.
- `Garage`: parked again, repaired.

The car comes back fresh but keeps its aids, damage rule and timing bests,
filled to `HOTLAP_FUEL_LAPS` (3) plus the fuel knob.

**The ghost**: `RequestGhost` is answered with `GhostLap` (`GhostLapData`,
struct of arrays: time, a six-float pose, speed, steering, gear, rpm; empty
without a record), read off the loop. The client asks on entering a hotlap
and on each new `LapRecord` with a trace.

**Client.** `UApexRootWidget::HandleTelemetryForHotlap` opens the garage from
the local car's `bInGarage`. `UApexHotlapWidget` (between the HUD and the
pause menu) holds the garage (GO OUT, REPLAY LAP, GHOST CAR, TYRES OUT from
`bHotlapColdTyres`, RESET SETUP, and the setup tabs: see
[vehicle physics](vehicle-physics.md)), a lap-by-lap timing sheet on track
and the replay strip. While the garage is up the HUD is hidden, driving input
is off and it owns the keys (`IsGarageOpen`, honoured by
`FApexMenuInputProcessor`); on track the pause menu gains BACK TO GARAGE.
Other drivers' garaged cars are hidden. `AApexGhostCarActor` is a race car
actor posed by `FApexGhostLap::SampleAt` on a clock run from the local lap
time, so it is where the record lap was at this point of this lap; tinted and
glowing rather than translucent (runtime car materials cannot go
translucent), muted, hidden while it overlaps the player, toggled by
`bGhostCar`. REPLAY LAP (`AApexRaceDirector::BeginGhostReplay`) drives the
ghost round in real time with cutting cameras (`CutReplayShot`).

## Watching a hotlap

Main menu > Garage > Tracks > Watch hotlap: one AI car laps a circuit alone,
full screen, minimal HUD. The server drives the lap on request, so any car,
weather, hour and circuit is one request.

**Session**: `CreateSession` with `HotlapWatch` makes one AI car
(`AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL)`, skill 110, in the
host's selected car, `exact_line`); `max_players`, `ai_count` and `ai_skill`
are ignored. The host gets `SessionJoined`, `TrackSectors` and
`TrackCorners`. A second such create from the same player replaces the first
without a `SessionLeft`.

**The lap**: `GameMode::Hotlap` from the first tick. The car is put on the
run-up and held on the brake for `HOTLAP_WATCH_HOLD_S` (6 s), then laps for
ever, refitted at each crossing (fuel, tyres and brakes at optimum, damage
off). The exact line is not always drivable at 110, so the driver eases down
a ladder (`hotlap_watch_driver`): level 0 the exact line, 1 the same skill
without it, then `HOTLAP_WATCH_SKILL_STEP` (3) a step to
`HOTLAP_WATCH_FLOOR_SKILL` (80). A crash (`HOTLAP_WATCH_CRASH_PCT` 1% in a
body zone, undrivable or towed) or `HOTLAP_WATCH_STUCK_S` (8 s) without
headway steps it and resets the car to the run-up; a struck lap steps it.

**Corners** (`track_guide::wire_corners` -> `TrackCorners`): the guide's
`detect_corners` in lap order, named from the dossier's `display_name`
(`name_corners`; else "Turn N" on the client). See
[track guide](../content/track-guide.md).

**Client** (`UApexRootWidget::StartHotlapWatch` / `ChangeHotlapWatch`,
`UApexNetSubsystem::CreateHotlapWatch`): the pending circuit and car and the
last sky; `SelectCar`, then the create, taken like `JoinAsSpectator`. The
lobby does not list the session, so the net subsystem answers
`FindSessionById` from the summary it made. Left / Right car, `W` weather,
`[` / `]` hour, Page Up / Down circuit, batched for 0.8 s
(`HotlapRestartAt`); unanswered in 15 s returns to the menu. The HUD is the
`hotlap_watch` scene: while `hotlap.active` only the `hotlap_*` components
show (see [HUD modding](../game/hud-modding.md)). `-ApexWatchHotlap
-ApexTrack=<stem> -ApexCar=<name> -ApexWeather=<sky> -ApexTimeOfDay=HH:MM`
starts one at launch.

## Checking it

```bash
cd server
cargo test --lib game_session                 # mode ticks, timed race, grid order, qualifying
cargo test --test race_flow_test              # incl. test_timed_race_flow_to_the_flag_monza
cargo test --test session_conditions_test     # rules echoed and enforced end to end
cargo test --test grid_order_test             # start order, stored results, over the wire
cargo test --test reconnect_test              # held seat, resume, takeover, grace, Disconnect
cargo test --test lap_timing_test             # sectors and track limits round Monza
cargo test --test hotlap_test                 # garage, run-up, queue, cold tyres, qualifying outlap
cargo test --test recovery_test               # back to track, a taken spot, back to pits, refused moving
cargo test --lib recovery                     # spot choice, traffic release
cargo test --test hotlap_watch_test           # hold, laps, determinism, corners, over the wire
cargo test --release --test hotlap_watch_test watch_probe -- --ignored --nocapture   # WATCH_TRACKS/CARS/LAPS
cargo test --release --test hotlap_test generate_ghost_fixture -- --ignored          # APEXSIM_GHOST_DIR, APEXSIM_GHOST_PLAYER
```

Golden bytes ([protocol](protocol.md)): `cargo test <name> -- --nocapture`
for `assists_wire_format`, `session_damage_wire_format`,
`session_ai_skill_wire_format`, `race_time_wire_format`, `grid_wire_format`,
`lap_timing_wire_format`, `telemetry_compact_wire_format`,
`hotlap_wire_format`, `recover_wire_format`, `track_corners_wire_format`,
`rejoin_wire_format`.

Client tests: `ApexSim.UI.CreateSession.StartOrder`,
`ApexSim.Race.QualifyingBoard`, `ApexSim.Race.RaceOrder`,
`ApexSim.Net.Protocol.GhostLap`, `ApexSim.Net.Udp.LapFields`,
`ApexSim.Net.Protocol.LapTimingDecode`, `ApexSim.Hotlap.*`,
`ApexSim.Spectate.Keys` / `.HotlapChoices`.

Unattended: `-ApexAutoRace -ApexMode=N` counts into `GameMode` N (6
qualifying, 8 hotlap), with `-ApexAiCount=N`, `-ApexDamage=off|reduced|full`,
`-ApexAiSkill=N`, `-ApexRaceMinutes=N`,
`-ApexLockAssists=abs,tc,gearbox,steering,line`; in a hotlap
`-ApexHotlapOutAfter=N`, `-ApexHotlapGarageAfter=N`,
`-ApexHotlapReplayAfter=N` (comma lists). Console:
`apexsim.hotlap.Out|Garage|Replay|Stop`, `apexsim.hotlap.Board [0|1]`,
`apexsim.hotlap.Watch N`. Clicking menus unattended: [client](../game/client.md).

## Traps

- `SessionState` and `GameMode` are separate: telemetry follows the state,
  simulation follows the mode, and `set_game_mode` sets the state per mode.
- The lobby keeps its own copy of each session's state for the listing;
  anything that changes `RaceSession::state` reaches it through the tick
  (`take_state_change`), so a change made between ticks is listed one tick
  later. Whether a driver may join is asked of the session itself
  (`refuses_drivers`), not of that copy.
- Records and stored qualifying are keyed by driver **name**, not player id
  (an id outlives a connection only through a resume token, and only while
  the player is connected or holds a seat).
- A held seat is the lobby's record (`get_player_session`), not the
  connection's `in_session`: a player holding a seat may be watching a menu
  backdrop, and `lobby.leave_session` / `remove_player` drop the seat as
  well as the watching. Leaving a watched session uses `leave_spectating`.
- Anything that removes "empty" sessions must keep `is_watch_only` sessions,
  which never have a human participant.
- Many AI laps are struck for track limits, so an AI car's
  `best_lap_time_ms` is often `None`; tests of AI pace read the last lap.
