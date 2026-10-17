# Sessions, game modes and race rules

A session is one track, one set of rules and the cars on it, ticked by the
server as a `GameSession`. This doc covers session kinds, states and game
modes, the rules a host sets on create, the race's grid before the
countdown, race start and finish (over laps or against a clock), starting order and qualifying (timed, open-ended or a
one-lap shoot-out, with an optional lap allowance), the race weekend (practice,
knockout qualifying and the race in one session), lap timing and records,
recovering a stuck car, the hotlap mode with its ghost, and the watched
hotlap. The client pieces that
belong to these rules are described briefly beside them.

## Code

- `server/src/data.rs`: `SessionKind`, `SessionState`, `GameMode`,
  `RaceSession`, `DamageLevel`, `AllowedAssists`, `DriverAids`,
  `QualifyingFormat`, `clamp_race_seconds`, `clamp_qualifying_seconds`,
  `clamp_qualifying_lap_allowance`
- `server/src/game_session.rs`: per-mode ticks (`tick_*`), `set_game_mode`,
  `start_countdown_mode`, `line_up_on_grid`, `assign_finish_positions`,
  `update_race_clock`, `apply_grid_order`, `note_qualifying_lap`,
  `update_chequered_flag`, `hotlap_relocate`, the shoot-out
  (`begin_shootout`, `shootout_order`, `call_next_runner`,
  `update_shootout`, `shootout_state`), the lap allowance
  (`update_lap_allowance`), the race weekend
  (`advance_weekend`, `settle_weekend`, `weekend_state`,
  `begin_phase_damage`), the watched hotlap
  (`start_hotlap_watch`, `update_hotlap_watch`, `hotlap_watch_driver`)
- `server/src/weekend.rs`: `WeekendFormat`, `knockout_sizes`,
  `WeekendProgress`
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
- `server/src/race_control.rs` (`RaceControl`, `FlagCar`) and
  `GameSession::update_flags` / `yellow_flags`: the race's flags
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
| (none) | 2 | Retired (was `Sandbox`, which had no behaviour); refused, never reused. |
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
  unless started again, except a race weekend waiting for its host between
  phases while a human is connected (`awaits_weekend_advance`).
- `game_mode: GameMode` decides what `GameSession::tick` simulates:

| Mode | Value | Tick | On entry (`set_game_mode`) |
|---|---|---|---|
| `Lobby` | 0 | nothing | default |
| `Sandbox` | 1 | nothing; cars stand, free camera on the client | state `Racing` |
| `Countdown` | 2 | cars held on the grid (`update_car_on_grid`: gear and revs only), pit exit red; at zero `set_game_mode(next_mode)` | 10 s if set directly |
| `DemoLap` | 3 | an AI drives with full physics | humans removed from the cars and made spectators; an AI spawned if none |
| `FreePractice` | 4 | full physics, collisions, lap timing | state `Racing`, refuelled for practice |
| `Replay` | 5 | nothing (placeholder, never built) | |
| `Qualification` | 6 | `tick_hotlap` (see "Qualifying"); a timed session's flag and finish (`update_chequered_flag`); a shoot-out's runs (`update_shootout`) or the lap allowance (`update_lap_allowance`) | as Hotlap; the qualifying clock starts (a timed session's classification starts empty); a shoot-out begins (`begin_shootout`) |
| `Race` | 7 | `tick_racing`: physics, race clock, classification, finish | state `Racing`, `race_start_tick`, pole's lap starts on green, auto boxes take first |
| `Hotlap` | 8 | like practice, but cars `in_garage` are neither ticked nor collided | state `Racing`, every human put in the garage |
| `Practice` | 9 | `tick_hotlap` with qualifying's outlap, clock and flag (`update_chequered_flag`); laps timed, nothing classified (see "Race weekend") | as Qualification, the classification untouched |
| `Grid` | 10 | `tick_grid`: cars held as in `Countdown`, pit exit red, no clock; once every connected human is ready, `start_countdown_mode(grid_countdown_seconds, Race)` (see "The race's grid") | `line_up_on_grid`, state `Countdown`, nobody ready |

**Changing mode is the host's** (anyone else gets `Error 403`, and both
broadcast `GameModeChanged`): `SetGameMode { mode }` switches at once, with
no transition rules; `StartCountdown { countdown_seconds, next_mode }` runs
`start_countdown_mode` (capped at `MAX_COUNTDOWN_SECONDS`, 60), which hands
over to `next_mode` at zero. A countdown into `Race` first calls
`line_up_on_grid`: every car back on its slot in start order, clean (laps,
times, finish position, damage), out of its garage, with race fuel and grid
tyres, keeping its aids and damage rule. A countdown is taken from any
state, a finished session included: a finished qualifying session goes on
to its race this way. The create screen offers Free practice, Sandbox,
Hotlap, Qualifying and Race; the client never counts into a hotlap
(`UApexNetSubsystem::StartCountdown` sends `SetGameMode` for it).
`OpenGrid { countdown_seconds }` (host only) puts the race's grid up
instead of counting at once (see "The race's grid").
`StartSession` (host only) is a countdown into a race: from `Lobby` it runs
`start_countdown_mode(START_SESSION_COUNTDOWN_SECONDS, Race)` (5 s) and
sends `SessionStarting { countdown_seconds: 5 }`; for a session already
under way it is refused with `Error 409`.

**Joining a session under way.** `GameSession::refuses_drivers` decides
whether `JoinSession` may seat another driver now: a running practice,
sandbox, hotlap or qualifying session takes one (a hotlap or qualifying
driver arrives in the garage, as the drivers there when it began did), a
race takes them until the green light (a car joining once the grid is up
or in the countdown gets the slot behind the last car, so an empty slot
stays empty), and a race under way or a finished session
refuses with `Error 409`. A race weekend takes drivers in its lobby, its
practice, between phases (`Finished` while it waits for the host,
`awaits_weekend_advance`) and on the race's grid; only a running qualifying
round and the race under way or over refuse them (see "Race weekend").
`JoinAsSpectator` is never refused for the session's state. The lobby
leaves the call to the session (`LobbyManager::join_session` checks only
room and kind). The lobby's listing (`SessionSummary.State`) follows the
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
auto_gearbox, steering_assist, abs, traction_control, pit_autopilot }`, sent by
`UApexRootWidget::SendDriverAids` on join and on any change in the settings
Assists tab. They run on the server (only it has the tyres); ABS and TC are
`Option`s on `CarState`, `None` meaning "as the car.toml says" (the AI, an
old client). TC LOW holds a spinning wheel at peak slip; HIGH also caps drive
at what the friction circle leaves beside the lateral force
(`TC_HIGH_LATERAL_MARGIN`, `physics.rs`). The client dims a locked row
(`GetAllowedAssists`, `RefreshAssistLocks`) and keeps the player's choice for
the next session. `pit_autopilot` (absent: on) is never locked and is in no
preset: off, the driver drives the pit lane ([pit-lane.md](pit-lane.md)). An
aids change while the pit autopilot holds the car goes into the aids it hands
back.

**Damage**: `damage` (`DamageLevel` `Off` / `Reduced` / `Full` /
`VisualOnly`, 0-3, default Full), the same for every car, AI included:
stamped as `CarState::damage_level` when seated (`set_damage` for the AI
seated at creation) and multiplying every accrual to the body by
`DamageLevel::scale` (0, `REDUCED_SHARE` 0.5, 1, 1) and to the engine by
`engine_scale` (the same, but 0 for VisualOnly). `VisualOnly` dents the body
and sheds its parts on the client as Full does, and costs nothing: the
physics, the pit crew's repair time and the AI's stop plan read
`CarState::felt_damage` (no damage), a hit sheds no debris and punctures
nothing, and a zone at 100% does not retire the car
(`keep_visual_damage_drivable`). Client: the create screen's Damage row (Off,
Reduced, Full, Visual: `UApexMenuFlowSubsystem::CreateDamage`),
`GetSessionDamage`, HUD `damage.level`. The lobby lists it
(`SessionSummary.damage`, left off the wire when Full) and the session browser
names any rule but Full (`ApexBrowser::DamageWords`: "reduced damage", "no
damage", "visual damage"). The model is in
[vehicle physics](vehicle-physics.md).

**AI level**: `ai_skill` (`Option<u8>`, clamped to 70-110; absent is the
mixed field). `ai_driver::field_skills` spreads the field over
`AI_FIELD_SPREAD` (4) points round the level, or from novice to ace without
one; what skill changes is in [AI](ai.md). Client: the "AI level" stepper
(`CreateAiSkill`, `ApexAiSkill::Label`), `GetSessionAiSkill`.

**Length**: `lap_limit`, or `race_seconds` (below). A qualifying session's
own length is `qualifying_seconds` (`Option<u32>`, clamped by
`clamp_qualifying_seconds` to `MIN_QUALIFYING_SECONDS` 60 ..
`MAX_QUALIFYING_SECONDS` 2 h; absent or zero is open-ended qualifying), see
"Timed qualifying" below. A race weekend (`weekend`, see "Race weekend")
carries its own practice and round lengths, and `qualifying_seconds` is
ignored beside it.

**Qualifying format**: `qualifying_format` (`QualifyingFormat` `Open` /
`Shootout`, 0-1, default Open, left off the wire when Open). `Shootout`
runs the session's qualifying as a one-lap shoot-out, and a weekend's
final round (see "Qualifying shoot-out"). Echoed in
`SessionJoined.QualifyingFormat`; not listed in the lobby.

**Lap allowance**: `qualifying_lap_allowance` (`u8`,
`clamp_qualifying_lap_allowance` to at most
`MAX_QUALIFYING_LAP_ALLOWANCE` 30; 0, the default, is none and is left off
the wire). The most timed laps a car may end in a qualifying session, or in
each round of a weekend's knockout (`update_lap_allowance`): each crossing
of the line that ends a timed lap (the outlap's end does not count; a lap
struck for track limits does) counts, and the car that has used its
allowance goes to its garage at that crossing, its lap classified as any
other; going out again is refused with "Lap allowance used". Once every car
taking part has used it and is in its garage (or undrivable), the session
or round is `Finished`, clock or no clock. Entering `Qualification` again
counts afresh. A shoot-out ignores it (one flying lap each). Echoed in
`SessionJoined.QualifyingLapAllowance`; each car's count goes out in its
`RosterEntry.QualifyingLaps` (left off the wire at 0), the roster resent at
every counted lap and when a new qualifying session or round clears the
counts. Client: the create screen's Lap
allowance stepper (None, 1 to 30, in the qualifying and the weekend sections;
`CreateQualifyingLapAllowance` in the profile, `QualifyingLapAllowanceToSend`:
none outside qualifying and in a plain shoot-out).
`UApexNetSubsystem::GetQualifyingLapsLeft` takes the allowance less the
local car's `QualifyingLaps` (`GetQualifyingTimedLaps`; -1 with no
allowance, outside `Qualification` and in a shoot-out): GO OUT carries "2 timed
laps left" and reads LAP ALLOWANCE USED, disabled, at none, and the HUD's
`qualifying.laps_left` drives the race state's Laps left panel.
`-ApexLapAllowance=N` sets it for an auto race.

**Qualifying cut-off** (the 107% rule): `qualifying_cutoff_pct`
(`Option<u16>`, `clamp_qualifying_cutoff` to 101..150 %; absent or 0 is
none, the default). A car whose best qualifying lap is slower than that
share of pole, or that set no time, does not start the race
(`GameSession::non_qualifiers`): it takes no grid slot (the grid closes up
over it; it did not qualify, it is not a no-show), and `line_up_on_grid`
seats it behind the field and parks it out of the race
(`park_non_qualifiers`: undrivable and towed, in its pit box when the track
has a lane), so the race neither waits for it nor meets it. A plain
session measures it on its own qualifying, a weekend on Q1 (which every
car ran; a late entry ran none, so is out). Without qualifying, or with no
time set by anyone, nobody is out. Echoed in
`SessionJoined.QualifyingCutoffPct`; not listed in the lobby. A parked
non-qualifier is marked `RosterEntry.Dnq` (`GameSession::not_qualified`,
left off the wire when false), so a client names it DNQ rather than
retired: the HUD's `item.dnq`, the standings and tower's DNQ, the results
screen's DNQ (`ApexRaceResults::BuildRows`). Client: a Cut-off chip row
(None, 107%) under qualifying's length and in the Race weekend section
(`bCreateQualifyingCutoff`, kept in the profile;
`QualifyingCutoffToSend` sends 107 only for a qualifying session or a
weekend), "107% RULE" in the create screen's summary,
`GetSessionQualifyingCutoffPct`.

**Pit rule** (mandatory stops): `mandatory_stops` (`u8`, held to
`MAX_MANDATORY_STOPS` 3; 0 is none, the default) and `compound_change`
(`bool`: the race is run on two dry compounds), kept as
`RaceSession::pit_rule` (`data::PitRule`, `GameSession::set_pit_rule`),
both left off the wire at their default. Races only. A stop is a service in
the car's box (`PitState::stops`, a splash included), counted from the
green light; the second compound is met by a stop that fits a dry compound
other than the one coming off, and waived for a car that runs an
intermediate or wet at any point of the race (`PitState::compound_changed`,
set at the green on a treaded tyre and at each stop that fits one). A car
that takes the flag owing either is classified behind every finisher that
met the rule, in its own order among the others that missed it, however
many laps it ran (`assign_finish_positions`), and marked
`RosterEntry.MissedStops` (left off the wire when false). Echoed in
`SessionJoined.MandatoryStops` / `CompoundChange` and listed in the lobby
(`SessionSummary.MandatoryStops` / `CompoundChange`); a hosted session
takes it as `HostedSpec.pit_rule` and the admin API's session detail carries
it. The AI plans the stops (`pit::plan_stop`, [ai](ai.md#strategy)).
Client: the create screen's Race tab has a Pit stops section in a race or
a qualifying session (Stops None / 1 / 2 / 3 and Two compounds Free / Two,
kept in the profile, `UApexMenuFlowSubsystem::MandatoryStopsToSend` /
`CompoundChangeToSend`), named in its summary ("1 STOP · TWO COMPOUNDS",
`ApexMandatoryStops::Describe`); `-ApexAutoRace` takes
`-ApexMandatoryStops=N` and `-ApexCompoundChange`. The session browser's
row names the rule. In the race the HUD's race state shows the stops still
owed (`pit.stops_owed`, `pit.compound_owed`: the session's rule against the
stops the HUD has seen the car make), and a car with `bMissedStops` is
marked PEN in the standings, the timing tower, the watch view's results and
both results screens (`ApexRaceResults::FRow::bMissedStops`).

## The race's grid

Before a race the cars stand on the grid while each driver sets the car up,
picks the tyres it starts on and its livery, and says they are ready.

- `OpenGrid { countdown_seconds }` (host only, else `Error 403`; from any
  state, as a countdown is) runs `open_grid`: `GameMode::Grid`, the field
  lined up as for the start, state `Countdown` (so telemetry flows and
  drivers may still join, seated on the next slot), nobody ready. The
  session hears `GameModeChanged { Grid }`.
- `SetReady { ready }` (`set_ready`) marks a seated human ready or not; it is
  ignored outside the grid. The roster carries it (`RosterEntry.ready`, left
  off the wire when false and always false off the grid).
- When every connected human is ready (`grid_is_ready`: a driver away on a
  held seat is not waited for, and at least one human must be connected),
  the tick starts `countdown_seconds` (capped at `MAX_COUNTDOWN_SECONDS`)
  into the race. A driver leaving who was the only one not ready lets it
  start too. The countdown reaches clients in telemetry.
- The host's `StartCountdown` while the grid is up counts down at once: the
  ones not ready start on what they have set.
- Setup and tyres are `SetCarSetup` as anywhere (the compound is its
  `tyre_compound` knob). `start_countdown_mode` lines the field up again, so
  the tyres fitted are the compound set by then, with the fuel knob's fill.
  `SetLivery { livery }` repaints a driver's car in the session (the roster
  carries it) and keeps it for their next `SelectCar`.

**Client.** The menus start every race through the grid
(`UApexNetSubsystem::BeginSession`: `OpenGrid` for a race, `StartCountdown`
for anything else): the session lobby's Start, the one-click start, Drive
again and the results screen's next race. `-ApexAutoRace` still counts
straight in. While telemetry says `Grid`, `UApexRootWidget` opens the hotlap
garage card in grid mode (`UApexHotlapWidget::SetGridMode`) for a driver
with a car: the setup tabs, and down the left READY (with "n of m ready"),
the livery (hidden for a car with one), START NOW for the host
(`StartCountdown(5, Race)`) and the grid in start order with each driver's
mark (READY, SETTING UP, AWAY, AI). The card closes when the countdown
starts. Console: `apexsim.grid.Ready`, `apexsim.grid.Livery`,
`apexsim.grid.StartNow`.

A race weekend's race goes up on the same grid: the host's advance after
the final round opens it (see "Race weekend"), so drivers change tyres,
setup and livery between qualifying and the race.

## Race start and finish

A car is seeded onto the centerline when put on the grid
(`physics::seed_track_progress`); lap 1 starts as a grid car crosses the
line, or on green for pole (`physics::start_lap_on_green`).

`assign_finish_positions` (every race tick, after `update_race_clock`)
classifies a car (`CarState::finish_position`) when it completes the winning
lap (`finish_lap`: `lap_limit`, or a timed race's final lap), ordered by laps
completed, then crossing tick, then distance past the line (a car that
missed the pit rule behind every car that met it, above). When the winner
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

## Flags

Race control (`race_control.rs`, run by `GameSession::update_flags` each
racing tick after the collision passes) shows yellow and blue flags; nothing
before the green or once the session is `Finished`.

- **Yellow**, per marshal sector: the lap in `MARSHAL_SECTOR_M` (250 m)
  sectors from the line. A car is an incident when it is under
  `INCIDENT_SPEED_MPS` (8 m/s), off the road, backing up or in contact (not
  in the pit lane, garage, towed, retired or finished; a slow car is none
  for `START_GRACE_S`, 10 s, after the green). Its own sector shows a
  double yellow while it is on the road, a single off it, and the sector
  before it a single. A flag is held `YELLOW_HOLD_S` (5 s) after its last
  cause. A car passing another of the same lap in a yellow sector is
  recorded (`RaceControl::yellow_passes`) for penalties to come.
- **Blue**, per car: a car a lap or more up within `BLUE_GAP_S` (1.5 s) at
  its speed, or `BLUE_GAP_MIN_M` (30 m), behind.
- Each car is told (`CarState::flag_yellow`, the worse of its sector's and
  the next's; `flag_blue`). The AI lifts to 95% of its pace under a single
  and 85% under a double, and starts no pass under either unless the car
  ahead is crawling ([ai](ai.md)); its yield to a car lapping it is the
  racecraft's own.
- On the wire: `CompactCarState.race_flags` (bit 0 blue, bits 1-2 the
  yellow ahead) and `CompactTelemetry.yellow_flags` (`[sector_m, [[sector,
  level], ...]]`, left off while none is out), positional, appended.
- Client: `FApexCarTelemetry::bBlueFlag` / `YellowAhead`,
  `FApexTelemetryFrame::YellowSectors` / `YellowAt`; the HUD's `flag.*`
  data points, `item.blue_flag` / `item.yellow`, and the `flags` component
  ([hud-modding](../game/hud-modding.md)).

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
only in a timed race counting in or running, or in timed qualifying with
`final_lap` 0). Client: the create screen's
Laps / Time switch along `ApexRaceLength::LadderMinutes` (5 min to 24 h;
`bCreateTimedRace`, `CreateRaceMinutes`, `EffectiveRaceSeconds()`),
`FApexTelemetryFrame::RaceLeftMs` / `RaceFinalLap`, `GetSessionRaceSeconds`,
HUD `race.timed`, `race.time_left_s`, `race.final_lap`, `lap.final`,
`fmt_clock` (see [HUD modding](../game/hud-modding.md)).

The server's replay of a session is a spectator stream (`.apxs`). When the
lights go out `tick_sessions` makes the session's `SessionStream`
(`session_stream.rs`, the one producer the render tool and live spectators use
too) and starts a recording (`stream_recorder.rs`); every tick the stream's
records (events, frames at 30 Hz, the slow state, the road) are added to it.
A writer thread compresses each second into `replays/.recording_<session>.blocks`;
a full queue drops blocks rather than stall the loop, and finishing the race
assembles `replay_<session>_<unix>.apxs` (preamble, blocks, index) with a
`.meta.json` beside it holding the classification, which the dashboard's
History lists. A session removed without finishing has its recording discarded
by the 5 s orphan sweep. The client's Replays screen plays a copy of the file
as it does its own (format: [spectator](../game/spectator.md)).

## Starting order and qualifying

A race grids by **driver references**, because at create time only the AI
are seated. `CreateSession.grid_order` (`Vec<String>`, `grid_order::sanitize`)
lists drivers first to last as `@host`, `@ai:N` (the Nth AI car, 1-based, in
seating order) or a name (a human's or an AI's). `line_up_on_grid` calls
`apply_grid_order` first:

- a requested order: listed drivers lead in order (`grid_order::resolve`,
  each claimed once, a name matching the first unclaimed driver of it),
  the rest follow in seating order;
- else this session's qualifying: the classified lead, the rest behind
  (`grid_order::complete`);
- else the seating order stands.

**A no-show's slot stays empty**, as on a real grid: a name the order
lists that nobody seated has, the host gone, or a qualifier who has left
(the weekend's grid too) keeps their slot, and the cars behind start where
they earned (`Option<PlayerId>` slots in `grid_order_ids`). A reference
to no car at all (`@ai:N` past the session's AI) is skipped, and empty
slots at the back go. An order longer than the track's grid closes its
gaps from the back until it fits (`grid_order::fit`), and the seating
order stands if even the drivers do not fit. The roster's `GridSlot` shows
the gaps.

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
next crossing of the line.

**Timed qualifying.** Without `qualifying_seconds` qualifying is open-ended:
no clock, no flag, never `Finished`; the host moves on when they like. With
it:

- The clock starts on the tick the session enters `Qualification`
  (`set_game_mode` records `timed_start_tick`; ticks, never wall time).
  Entering `Qualification` again restarts the clock and, in timed
  qualifying, empties the classification (open-ended qualifying keeps it). Telemetry carries it in `race_clock` as `RaceClock {
  left_ms, final_lap: 0 }` while in `Qualification`; `left_ms` counts down
  to 0 and stays there, and 0 means the chequered flag is out
  (`chequered_flag_out`). The race's own clock (`race_seconds_left`,
  `laps_left`) never sees it.
- After the flag every car on track (not `in_garage`) takes it at its next
  crossing of the line, the end of an outlap included: the lap it completes
  there is timed and classified as any other (`note_lap_event` queues it
  before the car moves, and `tick.rs` classifies it after the tick), then
  the car goes to its garage (`hotlap_relocate` to `Garage`, AI cars too).
  A car in its garage when the flag comes out is done. Going out again
  (`hotlap_relocate` to `Track`) is refused with "Qualifying is over".
- The flag sets a deadline at max(`QUALIFYING_FLAG_GRACE_MIN_SECONDS` 60,
  `QUALIFYING_FLAG_GRACE_LAPS` 2 x the slowest lap of the cars still out:
  its last lap, else its best, else its planned lap, else 120 s). The
  session goes `Finished` when every car is in its garage or undrivable, or
  at the deadline, when the cars still out are garaged. The replay stops
  and the lobby hears `Finished` as for a race.
- From there the host counts into the race in the same session
  (`StartCountdown { next_mode: Race }`): `line_up_on_grid` takes the cars
  out of their garages and grids them by the classification.

**Qualifying shoot-out** (`QualifyingFormat::Shootout`): the cars run one
at a time in a fixed order, each an outlap from the pit exit and one
flying lap, while the rest wait in their garages; the classification is
the flying laps. `is_shootout_round` is true in a plain session's
`Qualification` and in a weekend's final round.

- **No clock**: `timed_session_seconds` is `None` in a shoot-out (a plain
  session's `qualifying_seconds` and the final round's length are not
  used), so there is no `race_clock` and no chequered flag.
- **Begin** (`begin_shootout`, from `set_game_mode(Qualification)`): the
  order is fixed (`shootout_order`), the classification and the timing
  sheets are emptied, every car taking part is put in its garage, AI
  included (`send_ai_out` sends none out), and the first car is called.
- **The order** reverses what the session stands on as it begins, so the
  car with the most to defend runs last: a plain session's start order
  (`starting_slots`: the host's `grid_order`, an earlier qualifying's
  result, else the seating order; the car that would start last runs
  first); a weekend's final in the reverse of the round before it
  (`WeekendProgress::last_round`: the slowest of the cars through first); a
  weekend whose only round is the shoot-out in the reverse of practice's
  best laps (`best_lap_time_ms`), cars with no time first, then by grid
  slot. Knocked-out cars take no part.
- **A run** (`call_next_runner`): the next car in the order is put at the
  pit exit on its outlap (`relocate` to `Track`: the outlap pose, cold
  tyres, qualifying fuel), and given until max(`SHOOTOUT_RUN_MIN_SECONDS`
  180, `SHOOTOUT_RUN_LAPS` 3 x its planned lap) to finish. A car that has
  left, or whose driver is away on a held seat, is passed over. The run
  ends (`update_shootout`, after the tick) when the car crosses the line at
  the end of its flying lap (the lap is queued before it moves and
  classified as any other, a lap struck for track limits setting no time),
  goes to its garage, is wrecked, leaves, or runs out of time; the car is
  garaged and the next called. With the order run through the session is
  `Finished`, and the host counts into the race, gridded by the
  classification (cars with no time behind, as ever).
- **Waiting**: going out (`hotlap_relocate` to `Track`) is refused in a
  shoot-out with "A shoot-out: each car is sent out in its turn"; the
  server sends each car out. Going to the garage ends one's own run. A
  driver who joins a plain session's shoot-out (in the garage, as any
  qualifying join) is added at the back of the order.
- **Wire**: `ShootoutState` (`ShootoutStateData`, PascalCase keys:
  `Order`, the car indices in run order, a car that left dropped;
  `Running`, the 1-based place in `Order` of the car on its run, 0 when
  none; `Done`, how many have run; `Flying`, the car on its run has started
  its flying lap, left off when false). Reliable, to every member of the
  session: right after `SessionJoined` while a shoot-out runs (create,
  join, rejoin, spectate: `GameSession::phase_state_messages`) and from the
  tick whenever a field changes (`take_shootout_state`), after the roster.
  It is `None` outside `Qualification`; leaving qualifying ends the
  shoot-out. The telemetry's `in_garage` bit says which car is out.
- **Client**: the codec reads `ShootoutState` into `FApexShootoutState`
  (`UApexNetSubsystem::GetShootoutState`, `OnShootoutStateChanged`) and
  `SessionJoined`'s format and allowance (`GetSessionQualifyingFormat`,
  `GetSessionQualifyingLapAllowance`); `CreateSessionWithOrder` sends both.
  The create screen's qualifying section has a Format row (Open,
  Shoot-out; a shoot-out greys the clock chips and hides the allowance) and
  the Race weekend section a Final round row (a shoot-out hides Each round
  when it is the only round, and the weekend's line reads "Q1-Q2 10 min
  · Q3 shoot-out", `ApexCreateSession::WeekendRoundsLabel`), kept as
  `CreateQualifyingFormat` in the profile and sent by
  `QualifyingFormatToSend` (Open for a session that does not qualify); the
  footer's summary says SHOOT-OUT. The qualifying garage
  (`UApexHotlapWidget::RefreshShootout`, from `ApexBoard::Shootout`) shows
  the run order with each car's name and DONE / OUTLAP / FLYING LAP, the car
  out now, and the local car's call ("YOU RUN 4TH · 2 TO GO", "YOU ARE
  NEXT", `ApexBoard::ShootoutCall`) on GO OUT, which stays disabled; while
  the driver waits the scoreboard opens once and the spectator camera
  follows each car as it is sent out (`OnWatchCar`), until the driver picks
  another car to watch. As the local car's `PlaceOf` becomes `Running` a
  YOUR TURN banner stands over the view for five seconds with the notice
  cue. The results' qualifying sheet (`UApexSessionResultsWidget::IsShootout`,
  `ApexBoard::IsShootoutRound`) is titled QUALIFYING SHOOT-OUT, its format
  line reads ONE FLYING LAP EACH, IN RUN ORDER, and each car carries RAN Nth.
  `-ApexQualifyingFormat=shootout` (with `-ApexGridOrder=@host,@ai:1,...` for
  the start order the run order reverses) runs one from an auto race.

**Client.** The create screen's Race tab has a Starting order row
(`ApexCreateSession::`): chips for the seating order, a "Custom" order and
the track's three newest stored results (`RequestQualifyingResults`,
`OnQualifyingResults`), a Driver picker and Grid slot stepper (keyboard and
pad: pick any driver, default you, and step it earlier or later;
`MovingRef`), and a grid preview where
drivers are dragged between slots (handled in `NativeOnMouseButtonDown/Move/Up`;
the slot buttons are hit-test invisible because a button activates on the
press). `ApexCreateSession::OrderFromResult` maps a stored result: host by
name, the result's AI to this session's AI in qualifying order, other humans
by name. The order lives on the flow (`CreateGridOrder`, `CreateGridResultId`,
`CreateGridTrackId`), resets with the track, and is sent through
`GridOrderToSend` (empty for the plain seating order). The roster announces
each car's start slot before the cars are lined up (`RosterEntry.GridSlot`
from `GameSession::starting_slots`: the order `apply_grid_order` will seat,
refreshed when a qualifying lap or a weekend round changes it), and the
session lobby's GRID list is in that order. In qualifying the
garage (`UApexHotlapWidget::SetQualifying`) drops the replay, ghost and tyre
toggles and has TUNE | SCOREBOARD: the scoreboard (`ApexBoard::Build`) lists
every car fastest first, and clicking one (`OnWatchCar`) points the spectator
camera at it (`SetSpectating`, `FocusCar`). A hotlap session with another
human in it (`SetSharedHotlap`, from the roster) has the same page, with
the hotlap rows kept.

The create screen's Qualifying length row (`QualifyingLengthButtons`,
`ApexQualifyingLength::ChoiceMinutes`: Open, 10, 15, 20, 30 or 60 min, kept
as `CreateQualifyingMinutes` in the profile) sends `qualifying_seconds`
(`EffectiveQualifyingSeconds`); `-ApexQualifyingMinutes=N` sets it for an
auto race. In timed qualifying the garage caption counts the clock down and
reads CHEQUERED FLAG at zero, when GO OUT is disabled
(`UApexHotlapWidget::RefreshQualifyingClock`), and the HUD's `race.timed` /
`race.time_left_s` carry it. On `Finished` the root widget opens
`UApexSessionResultsWidget`, which lays out qualifying from
`ApexBoard::Sheet`: grid order (no time at the back), gap to pole and to
the car ahead, each best lap's sectors with the fastest of anyone's in
purple, and valid/total laps; the side panel gives the driver's grid slot,
best lap, sectors against pole (against P2 for pole) and ideal lap (best
sectors added up). Its primary button is the host's Start the race
(`StartCountdown(5, Race)`), "Waiting for the host" for the rest. The
session recorder takes the mode from telemetry, not the create screen, so a
guest's and a same-session race's results are laid out for what was driven.

## Race weekend

A session can be a race weekend: optional timed practice, one to three
timed qualifying rounds run as a knockout, then the race, on one track with
one field. The host moves from one phase to the next; nothing advances on
its own.

**Format.** `CreateSession.weekend` (`WeekendFormat { practice_seconds,
qualifying_seconds: Vec<u32> }`, `weekend.rs`) is clamped by
`WeekendFormat::clamp`: practice 0 is none, else 60 s .. 2 h; one to
`MAX_ROUNDS` (3) rounds, extra ones dropped and none made one of
`DEFAULT_ROUND_SECONDS` (900), each 60 s .. 2 h. The race's length stays
`lap_limit` / `race_seconds`. Under `qualifying_format` `Shootout` the
final round (the only one, with one round) is a one-lap shoot-out (see
"Qualifying shoot-out"): its length is not used. The lap allowance holds in
every timed round.

**Each phase's damage**: `practice_damage` and `qualifying_damage`
(`Option<DamageLevel>`, left off the wire when absent) give practice and
every qualifying round a damage rule of their own (client: the Race weekend
section's Practice damage and Qualifying damage rows, Race's / Off / Reduced
/ Full / Visual, kept as `CreateWeekendPracticeDamage` /
`CreateWeekendQualifyingDamage` and sent in `WeekendToSend`, practice's only
with practice; the summary names them); the race, and a phase
not named, has the session's `damage`. When either is named, each phase
begins (`GameSession::begin_phase_damage`, from practice, Q1 and the race's
grid or countdown) by `set_damage` with its rule: every car takes it, a
late arrival is seated under it, and `RaceSession::damage` (so
`SessionJoined.Damage` for anyone joining then) is it while the phase runs;
the race day's is kept (`race_day_damage`). `WeekendState.Damage` carries
the phase's rule (left off the wire when Full), and the client's
`GetSessionDamage` follows it. Assists stay the session's for every phase.

**Each day's sky**: `practice_conditions` and `qualifying_conditions`
(`Option<Box<SessionConditions>>`, as `CreateSession.conditions` is given,
clamped; left off the wire when absent) give practice and the qualifying
day (every round) a sky of their own; the race day is the session's own
`conditions`, and a day not named takes it too. When either is named,
each day begins (`GameSession::begin_day`, from practice, Q1 and the
race's grid or countdown) by resolving its conditions at the track's
site and latitude (`at_site`, `resolve_at`: its solar noon and season; the
host's `sky_seed`, else the session's id mixed with the day, so each day has
its own forecast and auto air; a day's own written `forecast` if it names
one), baking them into a fresh copy of the track as
loaded (`set_source_track`, the server's shared copy) with a fresh road
(the day's rubber and water), and starting the live sky over from that
tick (`sky_epoch_tick`): the day's clock starts at its own hour. The AI's
speed plans and lap figures are worked out again. `RaceSession::conditions`
becomes the day's (late joiners are told it); the race day's is kept.
Everyone already in hears it in `WeekendState.Conditions` (the client's
`GetSessionConditions` follows it), the lobby's listing takes it with the
session's next state (`Lobby::set_session_state`), and every human's racing
line is planned again under the day's grip at once (`take_day_lines_due`;
`AidLineKey::day`), not at their next lap's end. A weekend naming no day's
sky runs one sky from creation, as before.
Client: the Race weekend section's Practice sky and Qualifying sky rows
(a weather stepper and an hour stepper each, "Race day's" by default;
`CreateWeekendPracticeWeather` / `Hour`, `CreateWeekendQualifyingWeather`
/ `Hour`, kept in the profile). A day with either set is sent as the race's
conditions under that weather and hour, with a sky seed of its own
(`ApexCreateSession::DaySky`: the race's seed mixed with the day), and the
row shows the forecast the server will lay out from it; practice's only
when there is practice. The clamped format is kept as
`RaceSession::weekend` and echoed in `SessionJoined.Weekend`;
`qualifying_seconds` is dropped beside it.

**Phases** (`WeekendProgress::phase`: 0 lobby, 1 practice, 2 qualifying, 3
race). `AdvanceWeekend` (host only, else `Error 403`;
`GameSession::advance_weekend`) moves:

- lobby -> practice, or Q1 when there is none;
- practice finished -> Q1;
- Qk finished, k < rounds -> Q(k+1);
- final round finished -> `open_grid(START_SESSION_COUNTDOWN_SECONDS)`,
  gridded by the weekend: the race's grid card, counted in once every
  driver is ready. With no driver connected (a hosted weekend nobody is in)
  there is nobody to say ready, and it is
  `start_countdown_mode(START_SESSION_COUNTDOWN_SECONDS, Race)` instead.

It is refused with `Error 409` while a phase runs (not the lobby, not
`Finished`), once the race has begun, and in a session that is not a
weekend. The session hears `GameModeChanged` (`Practice`, `Qualification`,
`Grid`, or `Countdown` into the race). A grid or a countdown into the race
by any other route (`OpenGrid`, `StartCountdown`, `StartSession`) also puts
the weekend at its race (`enter_weekend_race`).

**Practice** is `GameMode::Practice`: run on `tick_hotlap` as qualifying is
(humans in the garage, going out is the outlap from the pit exit on cold
tyres, fuelled for `HOTLAP_FUEL_LAPS`), with qualifying's clock and
chequered flag on `practice_seconds` (`race_clock` with `final_lap` 0, the
flag at 0, each car on track garaged at its next crossing with that lap
counted, no going out after the flag ("Practice is over"), `Finished` when
every car is in or undrivable, or at the deadline). Laps are timed and
sectors sent and records (`lap_records.json`) are kept, but
`note_qualifying_lap` ignores them: nothing classifies for the grid and
nothing goes to `qualifying.json`. The timed modes share one clock
(`timed_start_tick`, `timed_session_seconds`, `chequered_flag_out`).

**Each round starts fresh**: `set_game_mode(Qualification)` empties the
classification and starts the round's clock; every human is in the garage,
the AI still taking part are sent out onto their outlaps (`send_ai_out`, in
seating order; at practice too), and the knocked-out cars, AI included, are
garaged and stay there: `hotlap_relocate` to `Track` is refused with
"Eliminated in Q1" / "Eliminated in Q2".

**Knockout sizes**: `weekend::knockout_sizes(field, rounds)`, the number of
cars in each round, taken when Q1 begins from the cars seated then. With
two rounds or more the final has `ceil(field / 2)`; the cars between are
knocked out evenly over the earlier rounds, any remainder in the earliest;
one round is the whole field; never below 1. 20 cars over 3 rounds: 20, 15,
10; 22: 22, 16, 11; 4: 4, 3, 2; 5 over 2: 5, 3.

**End of a round** (`settle_weekend`, run by the game loop after the tick's
laps are classified, and by an advance): the round's cars are ranked by
this round's best legal lap, cars with no time behind every time in seating
(grid slot) order. Before the final, the ones beyond the next round's size
are knocked out (`WeekendProgress::eliminated`, kept in grid order), taking
grid slots `next + 1 ..` in that order with that round's lap (0 for none).
The final's order is the front of the grid (`final_order`).

**The race grid** (`WeekendProgress::grid`): the final round's order, then
the knocked-out cars by grid slot. `apply_grid_order` takes it before a
requested `grid_order` and before the plain qualifying order; a driver who
has left leaves their slot empty, as elsewhere.

**Stored qualifying.** Each improved lap submits the weekend's combined
classification (`classified_laps`): the round running (or, once ranked, the
final) by its laps, then every knocked-out car by grid slot with the lap of
the round that placed it; drivers with no time are left out. One entry per
session in `qualifying.json`, as for a plain session.

**Between phases** the session is `Finished`, and the lobby lists it with
`SessionSummary.weekend_waiting` (from `GameSession::awaits_weekend_advance`,
passed with each listed state change to `LobbyManager::set_session_state`;
left off the wire when false), so a client offers Join on it: the session
browser badges it "Between sessions", counts it Joinable and joins it
(`FApexSessionSummary::IsOver` / `IsJoinable`). It is not removed by
`[server] session_timeout_seconds` while a human is connected; with every
human away the reconnect grace ends it as any session. Each phase is its
own replay: the replay stops on `Finished`, and a phase begun by an advance
(already `Racing` when the tick sees it, `take_phase_started`) starts a new
one.

**Joining**: drivers are taken in the lobby, in practice, between any two
phases and on the race's grid; `JoinSession` is refused (`Error 409`) while
a qualifying round runs and once the race is under way. A driver who joins
after practice is in Q1 like the rest (the knockout is sized when Q1
begins). One who joins after Q1 began, between rounds or after the final,
takes no part in qualifying (`WeekendProgress::enter_late`): they are
listed as knocked out in the round just run with no time (`Eliminated::late`;
`KnockedOutData.Late` on the wire, left off when false; the client's
`FApexKnockedOut::bLate`, named a late entry in the garage, the timesheet
and the results summary rather than "out in Qn"), on the grid slot behind every car
that has one, and going out is refused with "Joined after qualifying began:
starts from the back". Spectators are always taken.

**`WeekendState`** (`WeekendStateData`, PascalCase map keys like its
neighbours): `Phase`, `Round` (1-based, 0 outside qualifying), `Rounds`,
`Finished` (the phase has ended: the session is `Finished`; from practice or
a round the host may advance, in the race it means the race is over),
`Advancing` (cars going through from this round, 0 in the final and outside
qualifying), `Damage` (the phase's damage rule, see "Each phase's damage";
left off when Full), `Conditions` (the phase's sky, only for a weekend
naming a day's sky; see "Each day's sky") and `Eliminated` (`KnockedOutData { CarIndex, Round,
GridPosition, LapTimeMs, Late }`, every car knocked out so far in grid order, a car
that left the session dropped). Reliable, to every member of the session,
drivers and spectators: right after `SessionJoined` (create, join, rejoin,
spectate) and from the tick whenever a field changes
(`take_weekend_state`), after the roster whose car indices it uses.

**Client.** The create screen's Weekend mode tile (`FModeOption::bWeekend`;
`CreateStartingMode` stays `Race`, `bCreateWeekend` on the flow) shows the
Race length row and a Race weekend section: practice (None, 10, 20, 30, 60
min), the knockout's rounds (one to three) and each round's length (5 to 20
min), kept in the profile and sent as `WeekendToSend()`; a weekend sends no
`grid_order` and hides the Starting order row. `UApexNetSubsystem` keeps the
format (`GetSessionWeekend`, `IsWeekendSession`) and the state
(`GetWeekendState`, `OnWeekendStateChanged`); `AdvanceWeekend()` sends the
message. The lobby's start, the main menu's one-click start and an auto race
send `AdvanceWeekend` instead of a countdown, and the lobby hides its mode
and countdown pickers and lists the weekend. Practice uses the qualifying
garage (`ApexIsOutlapMode`; `UApexHotlapWidget::SetPractice` names it
PRACTICE, a round reads QUALIFYING 2 OF 3); a car knocked out has GO OUT
replaced by OUT IN Q1 · P12 ON THE GRID. After each phase
`UApexSessionResultsWidget` shows its timesheet (practice: FASTEST, no grid;
a round: the cars still in it, then a red KNOCKED OUT IN Qn cut taken from
the server's list once the round is over, then the cars out in earlier rounds
at their grid slots; the final: the grid) and the host's primary button names
the next session (Start Q1, Start Q2, Start the race; "Waiting for the host"
for the rest, enabled only when `Finished`). The HUD's `session.mode` is
`timed_practice` in practice, which binds to the hotlap/qualifying layout.
`-ApexWeekend=P,R,M` runs one from an auto race.

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

**Leaderboards**: `RequestLeaderboard { track_config_id, car_config_id? }` is
answered with `Leaderboard`: the fastest laps on the track, best first (at most
100), every driver's best in every car or in the one car asked for
(`RecordStore::leaderboard`; equal times keep the earlier lap first). Each line
carries the driver, the car, the time, the three splits and the date. The
client's Leaderboards screen (Garage > Tracks, `UApexLeaderboardWidget`) steps
through the circuits and filters to the garage's car; the driver's own laps are
lit. Nothing is shared between servers.

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
  garage in a hotlap or qualifying session. In a race the server plans and
  drives the car's stops as an AI's (`plan_ai_stops`, out to the lane's
  end), so a long race does not run its tank or tyres out. The roster marks
  the car `Away`, so the other drivers' towers show it. A lap the server
  drives is never a record. The lobby keeps the seat counted (`LobbyManager::hold_seat`), so
  the session is not given away beyond its size. A held seat's player is
  sent nothing of the session (telemetry, driver feedback, roster, timing,
  road state) until `JoinSession` resumes it: a resumed client sits in the
  menu with the offer, not in the race.
- **Offer and rejoin.** On a resumed `Authenticate` the server sends
  `RejoinAvailable { session_id, track_id, session_kind }` after the lobby
  state. `JoinSession` with that id takes the seat back (`resume_seat`): the
  same car where it is now, with the join's `SessionJoined`, racing line,
  setup sheet, sectors and record, and the roster again on the next tick.
  A resume that overtakes the old connection's timeout drops the old
  connection; its end is then ignored (the player is already back).
- **Giving it up.** Creating or joining another session (not a demo or a
  watched hotlap) gives the held seat up (`give_up_seat`), as do
  `Disconnect`, `GiveUpSeat` (the menu banner's Give up seat) and a
  dashboard kick (the kick also forgets the resume token). `LeaveSession` leaves what the connection is in, never a seat
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
opens the race view again. A drop from a session the player was driving in
rejoins by itself when the offer comes within
`UApexNetSubsystem::AutoRejoinWindowSeconds` (30 s) of the drop; a later
one waits for the banner.

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
time, so it is where the record lap was at this point of this lap; see-through:
every slot is an instance of the car parents' glass (`M_ApexCarTranslucent`)
in a pale blue at 35% opacity with a faint glow, the slot's own picture kept
under it (without the baked parent it falls back to tinting the car's own
instances), muted, hidden while it overlaps the player, toggled by
`bGhostCar`. REPLAY LAP (`AApexRaceDirector::BeginGhostReplay`) drives the
ghost round in real time with cutting cameras (`CutReplayShot`): chase,
trackside, onboard, trackside. The trackside shots come from a TV director
of their own (`GhostTv`: the circuit's path, the ghost alone, held to
`ApexTv::EShot::Trackside`), so the camera stands where the broadcast's
would, on the ground with a clear view; the replay cuts on when that
director has done with the shot, or after 10 s. While it
runs the HUD is about the ghost (`UApexHudDataSubsystem`, from
`GetGhostReplaySamples`): the watched car's speed, steering, gear, revs,
pedals and lap clock are the ghost's. A streamed ghost carries its pose
only, so its speed is read from how far it went, its pedals from the speed
changing, and its gear and revs from what the live car showed at that
speed (`GearAtSpeed`); the sector splits stay the live board's.

## Watching a hotlap

Main menu > Garage > Tracks > Watch hotlap: one AI car laps a circuit alone,
full screen, minimal HUD. The server drives the lap on request, so any car,
weather, hour and circuit is one request.

**Session**: `CreateSession` with `HotlapWatch` makes one AI car
(`AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL)`, skill 110, in the
host's selected car, `exact_line`); `max_players`, `ai_count` and `ai_skill`
are ignored. Its `watch` block (`network::HotlapWatchOptions`, left off the
wire when absent) holds the rest:

- `setup`: the car runs it (`set_hotlap_watch_setup`: clamped as `SetCarSetup`
  is, the tuned config, the pressures fitted again).
- `stream`: the host watches on the session's spectator stream, as
  `SpectateSession` puts a viewer (`start_streamed_hotlap_watch`):
  `SpectatorJoined`, then `TrackCorners`, then the catch-up and the records.
  Nothing is joined. `ServerState::stop_watching` removes the session when
  the host stops watching it: `LeaveSpectate`, a disconnect, or the leave a
  create or join implies, which is also how a second watch replaces the
  first.

Without `stream` the host gets `SessionJoined`, `TrackSectors`, `TrackCorners`
and the racer telemetry. A second create from the same player replaces the
first without a `SessionLeft`.

**The lap**: `GameMode::Hotlap` from the first tick. The car is put on the
run-up and held on the brake for `HOTLAP_WATCH_HOLD_S` (6 s), then let go
at `HOTLAP_WATCH_ROLL_FRAC` (90%) of the AI speed plan's speed for its place
(`roll_hotlap_watch_car`, with a gear to match; the place is found on the
plan's own racing line as the AI finds it, `ai_driver::profile_point`, since
the plan runs along the line and not the centerline), so the first timed lap is the
same lap as the rest and not a standing start; it laps for ever, refitted at each crossing (fuel, tyres and brakes at optimum, damage
off). The exact line is not always drivable at 110, so the driver eases down
a ladder (`hotlap_watch_driver`): level 0 the exact line, 1 the same skill
without it, then `HOTLAP_WATCH_SKILL_STEP` (3) a step to
`HOTLAP_WATCH_FLOOR_SKILL` (80). A crash (`HOTLAP_WATCH_CRASH_PCT` 1% in a
body zone, undrivable or towed) or `HOTLAP_WATCH_STUCK_S` (8 s) without
headway steps it and resets the car to the run-up; a struck lap steps it.
`HOTLAP_WATCH_CLIMB_LAPS` (3) legal laps in a row climb it a step back up.
The level a watch is left at is kept by the record store
(`RecordStore::keep_watch_level`, `<records dir>/watch_levels.json`, so a
server restart keeps it; in memory when records are off), keyed by track,
car, weather, setup and sky (`hotlap_watch_key`; a setup's clicks folded by
`setup_key`, 0 stock; the hour, air and wind by `sky_key`;
`records::watch_level_key` spells it out), and the next watch of that key
starts at it (`resume_hotlap_watch`): the bad laps are not driven again.

**The ghost.** The watched car's laps are traced as a human's are
(`sample_ghost_trace`, `GHOST_SAMPLE_HZ`, up to `MAX_GHOST_SAMPLES`, ten
minutes). A legal lap faster than the ghost becomes the ghost
(`update_hotlap_watch`). The fastest one per track and car is kept in
the record store (`RecordStore::keep_watch_ghost`,
`<records dir>/watch_ghosts/<track>-<car>.msgpack`; `keep_hotlap_watch`,
before a watch's session goes and at cleanup), and the next watch of that track and car starts with
it, whatever the setup or sky. The stream carries it as a `Ghost` record
each time it is replaced, and in a newcomer's catch-up
([spectator](../game/spectator.md)). A watch's laps never reach the record
store.

**Corners** (`track_guide::wire_corners` -> `TrackCorners`): the guide's
`detect_corners` in lap order, named from the dossier's `display_name`
(`name_corners`; else "Turn N" on the client). See
[track guide](../content/track-guide.md).

**Client** (`UApexRootWidget::StartHotlapWatch` / `ChangeHotlapWatch`): the
pending circuit and car, the last sky, and stock. `SelectCar` comes first,
then the create. Against a server with `LobbyState.LiveStream` it is
streamed (`bHotlapStreamRoute`: `UApexDemoModeSubsystem::WatchHotlap` ->
`UApexSpectatorSubsystem::WatchHotlap` ->
`UApexNetSubsystem::SpectateHotlapWatch`) and played on the backdrop feed
like a live session. The timeshift gives it pause, seek and speed, and its
ghost rides beside the car. Against an older server it is joined like
`JoinAsSpectator` (`CreateHotlapWatch`). The lobby does not list the session,
so the net subsystem answers `FindSessionById` from the summary it made.

The keys:
- Left / Right: the car.
- `W`: the weather.
- `[` / `]`: the hour.
- Page Up / Down: the circuit.
- `S` / D-pad up: the setup (the garage's saved setups for the car, then stock).
- 1-3: the rubber (green, normal, rubbered).
- 4 / 5: the air, two degrees a press from what the server would name.
- 6 / 7: the wind (calm, 8, 15, 25, 40, 60 km/h).

All of these are batched for 0.8 s (`HotlapRestartAt`).

Streamed, the replay keys are a transport. `R` / D-pad down drives the ghost
round with the replay's cutting cameras (`ReplayHotlapLap`,
`AApexRaceDirector::BeginGhostReplay`), and `G` turns the ghost on or off
(`bGhostCar`). Joined, there is no transport: the seek keys move the air and
the speed keys the wind.

A watch with no answer in 15 s, or a stream that ends, returns to the menu.
The HUD is the `hotlap_watch` scene: while `hotlap.active` only the
`hotlap_*` components show (`hotlap_transport` is the stream's;
see [HUD modding](../game/hud-modding.md)). `-ApexWatchHotlap
-ApexTrack=<stem> -ApexCar=<name> -ApexWeather=<sky> -ApexTimeOfDay=HH:MM`
starts one at launch; `apexsim.watch setup|replaylap|ghost` work as the keys do.

## Checking it

```bash
cd server
cargo test --lib game_session                 # mode ticks, timed race, grid order, qualifying, timed qualifying, weekend
cargo test --lib weekend                      # knockout sizes, format clamp, round closing
cargo test --lib race_control                 # yellows, warnings, hold, blue, a pass under yellow
cargo test --lib a_car_stopped_on_the_road    # the yellows in a race at Monza, on telemetry, cleared
cargo test --test weekend_test                # a weekend from the lobby to the race over the wire
cargo test --test shootout_test               # a shoot-out run at the Norisring, order and classification; over the wire
cargo test --test race_flow_test              # incl. test_timed_race_flow_to_the_flag_monza
cargo test --test session_conditions_test     # rules echoed and enforced end to end
cargo test --test grid_order_test             # start order, stored results, over the wire
cargo test --test ready_grid_test             # the race's grid: OpenGrid, SetReady, the countdown
cargo test --test reconnect_test              # held seat, resume, takeover, grace, Disconnect
cargo test --test lap_timing_test             # sectors and track limits round Monza
cargo test --test hotlap_test                 # garage, run-up, queue, cold tyres, qualifying outlap
cargo test --test recovery_test               # back to track, a taken spot, back to pits, refused moving
cargo test --lib recovery                     # spot choice, traffic release
cargo test --test hotlap_watch_test           # hold, laps, determinism, corners, ghost, setup, over the wire, as a stream
cargo test --release --test hotlap_watch_test watch_probe -- --ignored --nocapture   # WATCH_TRACKS/CARS/LAPS
cargo test --release --test hotlap_test generate_ghost_fixture -- --ignored          # APEXSIM_GHOST_DIR, APEXSIM_GHOST_PLAYER
```

Golden bytes ([protocol](protocol.md)): `cargo test <name> -- --nocapture`
for `assists_wire_format`, `session_damage_wire_format`,
`session_ai_skill_wire_format`, `race_time_wire_format`,
`qualifying_time_wire_format`, `qualifying_cutoff_wire_format`, `pit_rule_wire_format`, `shootout_wire_format`, `weekend_wire_format`, `grid_wire_format`,
`lap_timing_wire_format`, `telemetry_compact_wire_format`,
`hotlap_wire_format`, `hotlap_watch_wire_format`, `recover_wire_format`, `track_corners_wire_format`,
`rejoin_wire_format`.

Client tests: `ApexSim.UI.CreateSession.StartOrder`,
`ApexSim.Race.QualifyingBoard`, `ApexSim.Race.RaceOrder`,
`ApexSim.Net.Protocol.GhostLap`, `ApexSim.Net.Udp.LapFields`,
`ApexSim.Net.Protocol.LapTimingDecode`, `ApexSim.Hotlap.*`,
`ApexSim.Spectate.Keys` / `.HotlapChoices`.

Unattended: `-ApexAutoRace -ApexMode=N` counts into `GameMode` N (6
qualifying, 8 hotlap), with `-ApexAiCount=N`, `-ApexDamage=off|reduced|full|visual`,
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
  In a shoot-out such a car has no time at all (one flying lap each).
- `hotlap_relocate` is the driver's request and refuses what the rules
  forbid (the flag, a knock-out, a shoot-out, a used lap allowance); the
  session's own moves (a shoot-out's runs, the allowance's garaging) go
  through `relocate`, which asks nothing. A new refusal belongs in the
  first, or the session cannot send its own cars out.
