# Sessions, game modes and race rules

A session is one track, one set of rules and the cars on it, ticked by the
server as a `GameSession`. This doc covers session kinds, states and game
modes, the rules a host sets on create, the race's grid before the
countdown, race start and finish (over laps or against a clock), starting order and qualifying (timed, open-ended or a
one-lap shoot-out, with an optional lap allowance), the race weekend (practice,
knockout qualifying and the race in one session), events kept across
server restarts, championships over them, lap timing and records,
recovering a stuck car, rewind in offline practice, the hotlap mode with its ghost, and the watched
hotlap. The client pieces that
belong to these rules are described briefly beside them.

## Code

- `server/sim/src/data.rs`: `SessionKind`, `SessionState`, `GameMode`,
  `RaceSession`, `DamageLevel`, `AllowedAssists`, `DriverAids`,
  `QualifyingFormat`, `clamp_race_seconds`, `clamp_qualifying_seconds`,
  `clamp_qualifying_lap_allowance`
- `server/sim/src/game_session.rs`: per-mode ticks (`tick_*`), `set_game_mode`,
  `start_countdown_mode`, `line_up_on_grid`, `assign_finish_positions`,
  `update_race_clock`, `apply_grid_order`, `note_qualifying_lap`,
  `update_chequered_flag`, `hotlap_relocate`, the shoot-out
  (`begin_shootout`, `shootout_order`, `call_next_runner`,
  `update_shootout`, `shootout_state`), the lap allowance
  (`update_lap_allowance`), the race weekend
  (`advance_weekend`, `settle_weekend`, `weekend_state`,
  `begin_phase_damage`, `begin_phase_assists`), the watched hotlap
  (`start_hotlap_watch`, `update_hotlap_watch`, `hotlap_watch_driver`)
- `server/sim/src/weekend.rs`: `WeekendFormat`, `knockout_sizes`,
  `WeekendProgress`; `GameSession::resume_weekend`
- `server/server/src/events.rs`: events kept across restarts (`Event`,
  `EventStore`, `finished_phase`, `open_session`, `run_once`), their
  schedule (`EventSchedule`, `schedule_step`, `run_schedule`) and the
  lobby's list (`EventStore::lobby_events`), the weekend's sheets
  (`sheets_json`, `sheets_csv`, `phase_replays`, `sheets_data`);
  `GameSession::on_schedule`
  and `seat_late_in_pits` in the sim
- `server/server/src/championships.rs`: championships over the events
  (`Championship`, `ChampionshipStore`, `standings`)
- `server/sim/src/tyre_sets.rs`: `TyreSet`, `CarTyres` (the tyre rules,
  "Tyre sets")
- `server/server/src/server.rs`: `ServerState::create_session` (resolves the
  conditions, clamps the rules, seeds the AI)
- `server/server/src/game_loop/dispatch.rs`: message handlers; a watch-only kind
  refused
- `server/server/src/game_loop/tick.rs`: ticking, removal of empty sessions (and
  of sessions whose drivers are all away past the grace), lap timing out,
  qualifying and record submission
- `server/server/src/game_loop/lifecycle.rs`: disconnects, held seats
  (`hold_seat_for_rejoin`, `give_up_seat`); `GameSession::hold_seat` /
  `resume_seat` / `away_input`
- `server/sim/src/recovery.rs` and `GameSession::recover_car` /
  `update_recoveries`: back to track and back to pits
- `server/sim/src/race_control.rs` (`RaceControl`, `FlagCar`) and
  `GameSession::update_flags` / `yellow_flags`: the race's flags
- `server/sim/src/incidents.rs` (`IncidentLog`, `Incident`) and
  `server/server/src/incident_report.rs` (`IncidentReport`): the race's
  incident list for stewards
- `server/sim/src/race_start.rs` (`RaceStart`, `Formation`) and
  `GameSession::begin_formation` / `tick_formation` / `hold_cars_on_grid`:
  the formation lap, the rolling start, false starts
- `server/server/src/rewind.rs` (`Rewind`, `sole_driver`,
  `takes_snapshots`), `GameSession::snapshot` / `restore` / `rewind_to`,
  `dispatch::handle_rewind`: rewind in offline practice
- `server/sim/src/laps.rs`, `records.rs`, `grid_order.rs`,
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
| `Practice` | 1 | Same as Multiplayer on the server, but that its one human may rewind it (see "Rewind in offline practice"); the client uses it for single-player sessions. |
| (none) | 2 | Retired (was `Sandbox`, which had no behaviour); refused, never reused. |
| `Demo` | 3 | An AI-only race nobody drives: what a showcase is rendered from. Refused by `CreateSession`. |
| `HotlapWatch` | 4 | One AI car lapping alone (see "Watching a hotlap"), simulated by the game. Refused by `CreateSession`. |

`SessionKind::is_watch_only` (Demo or HotlapWatch): a `CreateSession` of
either kind is answered with `Error 400` and nothing is made. Both stay on
the wire, so older streams and clients decode; the menu's backdrop is a
showcase ([spectator](../game/spectator.md)) and Watch hotlap the game's own
simulation (below).

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
to its race this way. The create screen offers Free practice, Hotlap,
Qualifying, Race and a race weekend (Sandbox is the server's only: the
client has no free camera for it, and a profile saved on it opens on Free
practice); the client never counts into a hotlap
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

**Classes**: `classes` (`Vec<ClassEntry { class, ai_count, ai_skill }>`,
at most `MAX_CLASSES` 4, left off the wire when empty): a multi-class
field, see "Multi-class racing". Client: the create screen's Classes
rows, `GetSessionClasses`.

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
laps left" on the line under its label and reads LAP ALLOWANCE USED, disabled, at none, and the HUD's
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

**Race rules**: `race_rules` (`data::RaceRules`, its keys snake_case,
each left off the wire at its default and the whole table left off when
nothing is set; echoed in `SessionJoined.RaceRules`):

- `start` (`StartType` `Standing` / `Rolling`, 0-1, default Standing);
- `formation_lap` (`bool`): a standing start is preceded by a formation lap;
- `jump_start` (`bool`): false starts are watched for, and penalised with a
  drive-through;
- `yellow_penalty` (`bool`): a pass under a yellow is penalised;
- `pit_exit_penalty` (`bool`): leaving a manual pit lane past its red light
  is penalised;
- `safety_car` (`SafetyCar` `Off` / `Virtual` / `Full`, 0-2, default
  Off): a car stranded on the road brings out a virtual safety car, or a
  safety car on the road. Its restart procedure is fixed (the leader
  restarts at the line), so it takes no further rule;
- `reverse_grid` (`u8`, 0 off): the first N slots of the starting order
  start in reverse (see "Starting order and qualifying");
- `qualifying_tyre_sets` (`u8`, 0 none): the qualifying tyre allowance,
  and `qualifying_tyre_start` (`u8`, 0 off, 255 every car with a time): the
  front of qualifying starts the race on the set that placed it, and
  `weekend_tyre_sets` (`u8`, 0 none): a weekend's tyre allocation (see
  "Tyre sets");
- `parc_ferme` (`bool`): a car's setup is held from its first qualifying
  run to the end of the race (see "Parc fermé");
- `max_stint_laps` / `max_stint_minutes` (`u16`, 0 none): fixed stint
  lengths (see "Fixed stint lengths").

The start rules are in "Race start: formation lap, rolling start, false
starts", the penalties in "Penalties", the safety cars in "Virtual safety
car" and "Safety car". The lobby lists them with each session
(`SessionSummary.race_rules`, left off at the defaults) and says when the
server balances its cars (`LobbyState.balanced`); the session browser shows
both on the session's row. A hosted session does not take them yet.

Client: `FApexRaceRules` (`ApexProtocolTypes.h`) is the table; the
CreateSession encoder writes it only when a rule is set, each key only off
its default, and `SessionJoined.RaceRules` is kept on the net subsystem
(`GetSessionRaceRules`). The create screen's Race tab has a Race control
section beside the pit rule, in a race or a qualifying session (as the pit
rule, `UApexMenuFlowSubsystem::RaceRulesToSend`; kept in the profile):
Start (Standing / Formation lap / Rolling: the formation lap is a standing
start's, a rolling start always drives its pace lap and sends no
`formation_lap`), Penalise (False starts, Yellow passes, Pit exit light,
each a toggle), Safety car (Off / Virtual / On the road) and Reverse grid
(Off / 6 / 8 / 10 / All, All sending 255, which `grid_order_ids` takes as
the whole field). A Tyres and stints section beside it carries the tyre,
parc fermé and stint keys: Qualifying sets (Free, 1-4), Race on them (Off,
Top 10, All: `qualifying_tyre_start` 10 or 255) and Parc fermé where the
session qualifies (a qualifying session or a weekend), Weekend sets (Free,
6, 8, 10, 13) in a weekend, Stint laps (Free, 10, 15, 20, 30) and Stint
time (Free, 20, 30, 45, 60 min) in any race; `RaceRulesToSend` leaves the
qualifying keys off where the session does not qualify and the weekend's
sets off outside a weekend (`QualifiesBeforeRace`). The sections' lines and
the create summary name them short (`FApexRaceRules::Describe`: "ROLLING
START · 2 PENALTIES · VSC · REVERSE GRID · 2 QUALIFYING SETS · PARC FERMÉ ·
STINTS 20 LAPS"). `-ApexAutoRace` takes `-ApexQualifyingTyreSets=N`,
`-ApexQualifyingTyreStart=N`, `-ApexWeekendTyreSets=N`, `-ApexParcFerme`,
`-ApexMaxStintLaps=N` and `-ApexMaxStintMinutes=N`.

**Ballast by driver**: `ballast` (`Vec<data::DriverBallast { driver, kg
}>`, left off the wire when empty; echoed in `SessionJoined.Ballast`): a
league's success ballast by championship position, as the client's
offline season hands it to each round. `driver` is a start-order
reference (`@host`, `@ai:N`, `@driver:UUID` or a name, resolved by
`grid_order::resolve_each` as the start order's are: each reference one
driver, the first listing of a driver wins), `kg` the mass added at the
car's centre of gravity as a `[[balance]]` table's `ballast_kg` is
(`balance::CarBalance::apply`). `data::sanitize_ballast` drops blank
references and zero masses and holds a mass to `MAX_DRIVER_BALLAST_KG`
(300) and the list to 64. `GameSession::set_ballast` keeps the list and
`apply_ballast` puts it on the drivers seated, again whenever one is
seated, named or given a profile id (a human joins after create);
`retune` bakes a driver's setup and then their ballast into their tuned
copy (`simulated_config_for`), so physics, tyres and fuel see the heavier
car. The AI's speed plans and the racing line are built on the car as
loaded, not on its ballast. Without a list nothing runs (the tuned
copies are as before). `GameSession::ballast_kg` reads it back. A
server's own balance of performance, by car, is server.toml's
`[[balance]]` ([operations](operations.md)); this is the host's, by
driver.

## Tyre sets

Without a tyre rule every run goes out on a new set: the garage, the grid
and a stop each fit one. The rules that count sets (`race_rules` keys) keep
each car's sets on the session (`GameSession::tyre_sets`,
`tyre_sets::CarTyres`); only slicks are counted, so a treaded tyre
(intermediate, wet, snow) is always new and rain never strands a car.
Under such a rule each car's roster row says what its garage has for the
next run (`GameSession::tyre_set_outlook`): `RosterEntry.NewSetsLeft`, the
new slick sets it may still take (the tighter of the limits that hold),
and at none `UsedSetWear`, the most worn tyre of the used set
`CarTyres::peek_used` picks for the setup's compound; both are left off
the wire without a rule, and the roster goes out again after every trip
to or from the garage. GO OUT's line under its label reads "2 new sets left"
or "used set · 12% worn" after the timed laps left.

- **Qualifying tyre allowance** (`qualifying_tyre_sets` N): in
  `Qualification` a car may go out on a new slick set N times; every
  round of a weekend's knockout counts together, a plain session counts
  afresh each time it enters qualifying (a weekend at Q1). A set coming off
  the car on any trip to the garage (`relocate`) is kept with its wear,
  flat spots, pick-up and blistering and the laps it ran
  (`tyre_sets::TyreSet`). Past the allowance a car goes out on the least
  worn kept set of the compound its setup picks, else the least worn kept
  set of any compound (`CarTyres::take_used`), as out of the garage
  (cold). The AI's runs are counted the same way: an AI sent out again in a
  later round goes on the set it came in on.
- **Starting on the qualifying tyres** (`qualifying_tyre_start` N): the
  set a car is on as it ends each timed lap in qualifying is noted
  (`CarTyres::lap_end`, in `tick_hotlap`), and the lap that improves its
  classification keeps it (`note_qualifying_lap`). `line_up_on_grid` fits
  the first N of the classification (`classified_laps`: a weekend's
  combined one) with that set (`fit_qualifying_sets`): its compound, worn
  as it was, warmed as the rest of the grid. The setup's compound and a
  grid-card change do not move it; the cars behind start on new sets as
  ever. The set's laps go into the car's stint (`PitState::stint_start_laps`
  set below zero), so the AI's tyre-wear rate (`pit::tyres_reach_flag`)
  reads the set's whole life. Waived when the race starts wet (the
  weather's tyre for a stock pick is a treaded one).
- **A weekend's allocation** (`weekend_tyre_sets` N): in a weekend's
  practice and qualifying together a car may take N new slick sets
  (`CarTyres::new_in_weekend`; `GameSession::set_limits` gives the limits
  that hold now, and a new set must be left under each). The sets carry:
  practice starts the count, Q1 goes on with the sets practice left (only
  the qualifying allowance's own count starts again), and past the
  allocation a car goes out on its least worn kept set as above. The
  race's stops always fit new sets: its sets are not counted.

A car's setup carries through a weekend as it is: one session, one
`car_setups` entry per driver, whatever phase it is sent in.

## Parc fermé

`race_rules.parc_ferme`: a car's setup is held from the moment it first
goes out in qualifying (`relocate` to `Track` in `Qualification`, which
keeps the setup it has then in `GameSession::parc_ferme`) to the end of
the race. Every `SetCarSetup` after that is applied as
`CarSetup::under_parc_ferme`: the driver keeps what a real one may still
change there (brake bias, engine braking, torque map, the hybrid's start
mode and deploy map, the tyre pressures and compound, the fuel load and
the front wing) and every other knob stays as held. A car that never went
out in qualifying (knocked out before it ran, a late entry) is not held. A
fresh qualifying (a plain session entering `Qualification` again, a
weekend's Q1) starts it over; any mode but `Qualification`, `Countdown`,
`Grid` and `Race` releases it. The AI runs stock setups, so it is never
touched. The applied setup is not sent back (`SetCarSetup` has no reply),
so the client follows the rule itself (`FApexParcFerme`, kept by
`UApexNetSubsystem` from telemetry: held at the setup last sent when the
local car first leaves its garage in `Qualification`, released as the
server releases it, `IsUnderParcFerme` / `GetParcFermeSetup`). Held, the
garage's setup tabs show the held knobs at their held clicks and disable
their steppers (`ApexCarSetup::FreeUnderParcFerme` names the free ones,
`FApexCarSetup::UnderParcFerme` mirrors `under_parc_ferme`), and the
setup line reads "PARC FERMÉ: SETUP HELD".

## Fixed stint lengths

`race_rules.max_stint_laps` and `max_stint_minutes`: no stint of a race
may run more than so many laps, or so many minutes. A stint runs from the
green light (no laps, `race_start_tick`) or the end of a stop's service in
the race (`GameSession::stint_from`: the laps run, `laps_run`, and the
tick; a splash and a stop-go are stops too, a drive-through is not) to the
next stop or the flag (`stint_of`). Each tick of a race under way
(`update_stints`, not on the formation lap) a car still racing whose stint
is past the rule, by more than `pit::STINT_GRACE_LAPS` (0.15 lap) over its
laps, has overrun it (`stint_overrun`, kept to the flag;
`GameSession::overran_stint`); the stint to the flag is checked again as
the car takes it. A car that overran a stint is classified as one that
missed the pit rule: behind every finisher that kept the rules, and marked
`RosterEntry.MissedStops` (the client's PEN). Driver swaps are not
simulated: the stint is the car's.

The AI plans for it (`StopContext::stint_left_laps`: the laps the stint may
still run, the minutes at the car's own lap pace, the smaller of the two):
with less than `pit::AI_STINT_STOP_LAPS` (1 lap) left and the flag further
away than that, `pit::plan_stop` stops at once, traffic or not; a car that
reaches the flag within its stint stays out. A driver's own car is theirs
to bring in (an away driver's is planned as the AI's): the HUD counts the
stint as the server does, from the green (no laps) or the end of a stop's
service as it saw them (`FApexHudMemory::FCarHistory::StintFromLaps` /
`StintFromTick`, minutes from the frames' ticks over the session's tick
rate), and publishes `stint.laps`, `stint.time_s`, `stint.laps_left` and
`stint.time_left_s` ([hud-modding.md](../game/hud-modding.md)); the
default race state shows a Stint left panel, lit under a lap or two
minutes, red once past.

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

## Multi-class racing

A session whose `CreateSession.classes` names two or more classes races a
mixed field: so many AI of each car.toml `class`, each class at its own
level (`ClassEntry::ai_skill`, else the session's `ai_skill`). The server
keeps the list on `RaceSession::classes` as `GameSession::set_classes`
cleans it: names trimmed and deduplicated ignoring case, a class no loaded
car belongs to dropped, the host car's class added with no AI when the
host left it out (so a lone GT3 among the LMP2s is still a class), and
ranked quickest first by each class's planned ideal lap of this track
(`rank_classes`: `racing_line::lap_time_s` of the empty-tank plan, over the
class's AI cars, else its first car by id). With fewer than two classes
left the session is the usual field of the host car's class, and nothing
below applies: every car then ranks 0 (`class_rank_of`).

- **The field.** `ai_driver::generate_class_profiles` stamps each AI with
  its class (`AiDriverProfile::class`; names run on across the classes) and
  `spawn_ai_drivers` deals it a car of that class (`class_pool`: every car
  of the class by id, from the host's model on), round-robin per class.
  `ai_count` is the total; a server from before the field ignores
  `classes` and races that many of the host's class.
- **Grid.** `grid_order_ids` sorts any start order (the seating order, the
  host's, qualifying's, a reverse grid's) by class, the quickest class at
  the front, each class in the order it had, closed up over empty slots.
- **Classification.** `ClassifiedCar::class` and `class_position` (its
  place among the cars of its class, in the overall order; set for every
  race, every car of a field of one class being in one class), in the
  results JSON, the CSV's last two columns and the Discord line.
- **Flags.** A car of a quicker class within `BLUE_GAP_S` behind shows the
  blue as a lapping car does (`FlagCar::class_rank`); a pass across classes
  under a yellow is not recorded, as a lapping pass is not.
- **The AI.** A car caught by one of a quicker class yields to it as to a
  lapping car (`Rival::class_rank`, racecraft `Yield`), and nobody defends
  against a car of another class; the quicker car is held up behind it as
  behind any slower car and passes where its plan says.
- **On the wire.** `SessionJoined.classes` echoes the list in rank order
  (to spectators too); `SessionSummary.classes` lists the names for the
  session browser. Each car's class is its car's (`RosterEntry.
  car_config_id`, the catalog's class): the client places every car in its
  class from that and the echoed order.

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

## Race start: formation lap, rolling start, false starts

`race_start.rs`, under the session's `race_rules`. With none set a race
starts as above: the countdown runs out and the lights go out on a held
grid.

**Formation lap** (`formation_lap`, a standing start). When the countdown
into a race runs out (`tick_countdown`), the formation lap begins
(`begin_formation`) instead of the race: mode and state stay `Countdown`,
with no countdown running (`countdown_ms` absent) and telemetry's
`race_phase` 1. Each car is held to a pace (`CarState::speed_cap_mps`,
which the physics enforces as the pit limiter does, on every car, a
human's included): `FORMATION_SPEED_MPS` (30 m/s), and no closer than
`FORMATION_GAP_M` (14 m) behind the car starting ahead of it that is still
in the lap, at that car's speed (`Formation::paces`), so the field moves
off one car after another in grid order and runs single file; a car off
the road or out is no pace for the rest. The AI starts no pass under a
pace (`racecraft`). Over the last `APPROACH_M` (150 m) to its slot the
server drives every car (`approach_input`: a Stanley steer to the slot's
place across the road, on the automatic box, slowing at
`APPROACH_DECEL_MPS2` to stop on it; the closest it comes to the car
ahead is where its own slot is behind that car's). A car stopped within
`HOME_M` (1.5 m) of its slot is home: put exactly on it, its lap state
reset (`reset_laps`: lap 0, nothing timed), in neutral, its own gearbox
setting back, held as on the grid. Nothing on the lap is timed (no
`LapTiming`). Once every car is home (an undrivable or towed car counts as
home), the lights count `LIGHTS_SECONDS` (5) down, as a countdown, into the
race. A car not home by `FORMATION_TIME_SHARE` (1.6) x the lap at the
formation pace plus `FORMATION_SPARE_S` (60 s) is put on its slot.
`JoinSession` is refused during the lap ("The formation lap is under way").
A mode set over it (`SetGameMode`, a new countdown or grid) ends it.

**Rolling start** (`start: Rolling`, always with its pace lap). The same
lap (`race_phase` 2), nobody stops and the server drives nobody: a car
behind its station may run `ROLLING_CATCH_MPS` (4 m/s) over the pace to
close a gap opened as the field got away. The field forms up two by two
on the way back (`Formation::update_forming`), from `ROLLING_FORM_M`
(300 m) before the line, the leader's distance to it, and earlier by the
field's spread past the grid's own length (`ROLLING_FORM_SPARE` 2.5 times
what the field closes at the leader's pace):

- The leader slows to `ROLLING_FORM_SPEED_MPS` (22 m/s, 80 km/h), the pace
  at the green, and every car behind is held to the lap's pace, so the
  field closes up through the corners too, single file at
  `ROLLING_FILE_GAP_M` (12 m) behind the car ahead.
- Within `ROLLING_PAIR_M` (300 m) of its slot, on a straight run to it
  (the road turning less than `ROLLING_STRAIGHT_RAD`, 0.12 rad, over the
  rest of the way; `race_start::straight_run`), a car pairs up and stays
  paired: it holds its slot's side of the road
  (`CarState::formation_lane_m`, which the AI drives as a racecraft lane,
  with no traffic dodge across the road), and once more than
  `ROLLING_BESIDE_M` (2.6 m) across from the car ahead it keeps its slot's
  distance behind that car and behind the one ahead of that (its own
  side's), both `ROLLING_ROW_SPREAD` (1.5) times the grid's: 6 m to the
  car beside, 12 m to the car ahead on its side.
- Station is kept both ways while forming (`race_start::station_cap_mps`):
  a car too close is held below the car ahead's speed until the gap opens,
  since a pace only cuts the throttle.

As the leader (the first car in grid order still out) comes back to its
slot on the line, the race goes green (`transition_from_countdown(Race)`),
every car's pace and lane lifted and its lap state reset, pole's lap 1
starting as it crosses the line. The front rows are formed by then; the
back of a long field may still be closing up, most where a chicane comes
just before the line (Spa's Bus Stop: only the front row forms with 16
cars, against four rows at the Norisring and seven at Zandvoort).

**Tyres.** A grid with a formation lap (or a rolling start's pace lap) to
drive is fitted with its tyres as from the garage, its blankets or the air
(`TyreStart::FormationGrid`; its brakes as on any grid), and the lap warms
them: on its straights every car still in the lap, more than
`WARM_UP_UNTIL_M` (300 m) from its slot and not yet forming up, is
`CarState::warm_up`, and the AI weaves across its line (`WEAVE_M` 2 m each
side, a weave every `WEAVE_WAVELENGTH_M` 45 m). At the Norisring a GT3
field goes 22.0 -> 39.2 °C over the lap (35.3 without the weave). A
driver who neither weaves nor pushes starts on colder tyres. A race
without a formation lap is fitted part-warm instead
(`tyre_thermal::FORMATION_LAP_WARMTH`, half way to the optimum: 54.5 °C
there), for the lap it does not drive.

**False start** (`jump_start`, a standing start). Through the countdown
that ends in the green (not the one before a formation lap), a driver's car
(not the AI's, a stand-in's or an away driver's) in gear with the throttle
past `JUMP_THROTTLE` is let go of (`race_start::releases`): it runs the
physics instead of being held, so it moves if the driver lets it. One that
is more than `JUMP_START_M` (0.5 m) off its slot before the lights go out
has jumped the start (`RaceStart::jumped`, penalised at the green, see
"Penalties"). A car on the automatic
box waits in neutral and cannot jump. Once a car is let go of, the
collision and wall passes run through the countdown, so it shunts the car
ahead rather than driving through it (the held car is pushed by the
contact, then held where it ends up). Without the rule the grid is held as
ever.

## Flags

Race control (`race_control.rs`, run by `GameSession::update_flags` each
racing tick after the collision passes) shows yellow and blue flags; nothing
before the green or once the session is `Finished`.

- **Yellow**, per marshal sector: the lap in `MARSHAL_SECTOR_M` (250 m)
  sectors from the line. A car is an incident when it has been under
  `INCIDENT_SPEED_MPS` (8 m/s), off the road, backing up or in contact
  for `INCIDENT_CONFIRM_S` (1.5 s) without a break (not in the pit lane,
  garage, towed, retired or finished; a slow car is none for
  `START_GRACE_S`, 10 s, after the green): a quick step over the white
  line or a brush raises no yellow, and a real one is out a moment after
  it happens. Its own sector shows a
  double yellow while it is on the road, a single off it, and the sector
  before it a single. A flag is held `YELLOW_HOLD_S` (5 s) after its last
  cause. A car passing another of the same lap in a yellow sector is
  recorded (`RaceControl::take_yellow_passes`, for "Penalties"), unless the
  car passed is in trouble: under `INCIDENT_SPEED_MPS`, off the road,
  backing up, or slower than `YELLOW_PASS_SLOWER_SHARE` (70%) of the passer.
- **Blue**, per car: a car a lap or more up within `BLUE_GAP_S` (1.5 s) at
  its speed, or `BLUE_GAP_MIN_M` (30 m), behind.
- Each car is told (`CarState::flag_yellow`, the worse of its sector's and
  the next's, `RaceControl::yellow_ahead_for`; `flag_blue`). A flag warns
  the cars coming up on an incident: one that a single car alone raised is
  not shown to that car (running wide does not slow it), and a car more
  than `PAST_INCIDENT_M` (10 m) past the rearmost incident in its own
  sector no longer sees that sector's flag (the warning sector before an
  incident covers all of it). The AI lifts to 95% of its pace under a single
  and 85% under a double, and starts no pass under either unless the car
  ahead is crawling ([ai](ai.md)); its yield to a car lapping it is the
  racecraft's own.
- On the wire: `CompactCarState.race_flags` (bit 0 blue, bits 1-2 the
  yellow ahead) and `CompactTelemetry.yellow_flags` (`[sector_m, [[sector,
  level], ...]]`, left off while none is out), positional, appended.
- Client: `FApexCarTelemetry::bBlueFlag` / `YellowAhead`,
  `FApexTelemetryFrame::YellowSectors` / `YellowAt`; the HUD's `flag.*`
  data points, `item.blue_flag` / `item.yellow`, and the `flags` component
  ([hud-modding](../game/hud-modding.md)). The race's phase
  (`CompactTelemetry.race_phase`: formation and pace laps, VSC, safety car)
  is `FApexTelemetryFrame::RacePhase`, the HUD's `race.phase` /
  `race.phase_name` and the `race_phase` banner.
- Trackside: every circuit's marshal light panels (`board/light_panel`,
  laid by `ats-dress` beside each marshal post and in any 250 m sector
  without one) flash yellow while their sector shows a yellow, and blue
  while a car shown the blue flag is within 250 m up the course
  (`AApexRaceDirector::UpdateFlagPanels`: each panel its own actor and
  lamp instance, its station the nearest centerline point's).
- In the car: a cue as a yellow comes up ahead of the local car or its
  blue flag does (`UpdateRaceBleeps`, [audio](../game/audio.md)), and the
  cockpit dash's light bar flashing the flag's colour while it is out
  ([cameras](../game/cameras.md)).

## Virtual safety car

Under `safety_car: Virtual` (`RaceControl::update_course`, run by
`update_flags` each racing tick):

- **Out**: a car stranded on the road (`FlagCar::stranded`: on the road,
  under `INCIDENT_SPEED_MPS` or broken down and still rolling, not in the
  pit lane, its garage, towed, finished, nor in the first `START_GRACE_S`
  after the green) for `VSC_STOPPED_S` (8 s), before it is towed
  (`TOW_AFTER_S`, 10 s).
- **The pace**: every car on the track (not in the lane, finished, towed)
  is held to `RaceControl::vsc_pace_mps` (`CarState::speed_cap_mps`, which
  the physics enforces as the pit limiter does): from `VSC_FROM_MPS` (90
  m/s) down at `VSC_SLOW_MPS2` (5 m/s²) to `VSC_SPEED_MPS` (35 m/s), so
  the field slows over some seconds instead of braking as one. The AI
  starts no pass under it; a pass anywhere is a pass under a yellow (and
  penalised under `yellow_penalty`). The pit lane stays open, and the AI
  takes a stop it would make anyway ([ai](ai.md#strategy)).
- **In**: with no car stranded for `VSC_CLEAR_S` (5 s) it is ending
  (`Course::VscEnding`, still at the pace) for `VSC_ENDING_S` (5 s), then
  green; a car stranded long enough meanwhile brings it back out.
- On the wire: `CompactTelemetry.race_phase` 3 (out) and 4 (ending).

## Safety car

Under `safety_car: Full` (`safety_car.rs`, `SafetyCar::tick`, run by
`update_flags` each racing tick) the same trigger as the virtual safety car
(a car stranded for `VSC_STOPPED_S`) brings out a safety car on the road
(`Course::Sc`). It is not a participant: no roster slot, no classification,
no lap timing, no AI profile; it lives on `RaceControl::safety_car` from its
call to the green.

- **Out of the pit exit.** It waits at the lane's last node (on a circuit
  with no lane, on the line) until the leader (the running car with the most
  laps) is within `SC_PICKUP_M` (200 m) of it on the road, then joins the
  racing line ahead of it at the lane's limit speed. Until then every car on
  the road is held to a pace coming down from `VSC_FROM_MPS` at
  `VSC_SLOW_MPS2` to `SC_CATCH_MPS` (55 m/s; `RaceControl::sc_ramp_mps`).
- **Its pace.** It drives the racing line planned for the first car of the
  field (`racing_line::build`; the centerline when there is none) at
  `SC_LINE_SHARE` (0.7) of the line's planned speed, at most `SC_SPEED_MPS`
  (45 m/s, 162 km/h), pulling away at `SC_ACCEL_MPS2` (4 m/s²). The share
  keeps the corners slow enough to follow, and squares into the braking: a
  queue at 24 m never has to brake harder than half what the car can. Its
  place on the lap is each line point's centerline station, found once.
- **The queue.** Each car on the road (not in its garage, towed, finished,
  broken, or between the lane's limit lines) is held
  (`CarState::speed_cap_mps`) no faster than the car ahead of it on the road
  (the safety car for the first) allows at `SC_GAP_M` (24 m, middle to
  middle) behind it, slowing at `race_start::APPROACH_DECEL_MPS2`
  (`race_start::follow_cap_mps`, the formation lap's own station keeping),
  and no faster than the catch pace: the field closes up single file. A car
  crawling or off the road is no pace for the cars behind it. The AI aims at
  the pace as at a corner's speed and starts no pass under it.
- **Formed.** Once every car is within `SC_QUEUED_M` (50 m, ten car lengths)
  of the one ahead, or after `SC_FORM_LAPS` (2) laps of the safety car, the
  queue has formed and the pit lane opens.
- **Lapped cars.** From the tick after it formed, a car with a car behind it
  in the queue a lap or more up on it is let by (`SafetyCar::let_by`): held
  only to the catch pace, free to pass and be passed (`FlagCar::free`), it
  passes the queue and the safety car and goes round to the back; it is done
  once it is more than `SC_PASS_WINDOW_M` (60 m) ahead of the safety car with
  nobody behind it a lap up. The lights do not wait for it to catch the
  queue up.
- **Its body.** On the road the safety car is a car in the AI's traffic
  and racecraft field (`GameSession::safety_car_body`: `SC_LENGTH_M` x
  `SC_WIDTH_M`, the Murcetes-AMD GT3's 4.7 x 2.0 m, given `SC_ROOM_M` 2 m
  and `SC_SIDE_ROOM_M` 0.5 m more), never on anybody's lap, so nobody
  yields to it or defends against it: a car let by passes it as it passes
  a crawling car, on a lane beside it. The safety car in turn keeps
  station on a running car within `SC_FOLLOW_WINDOW_M` (60 m) ahead of it
  on the lap, at its body and room (`race_start::station_cap_mps`, so it
  drops back from one alongside): it drives through nobody it has let by
  or joined behind. Neither is a body to the collision pass.
- **Passes.** Every pass under it is one under a yellow
  (`RaceControl::update`, any pass while the course is not green), and a car
  not let by that goes past the safety car is given the same offence
  (`Offence::YellowPass`); both are penalised under `yellow_penalty`.
- **The pit lane** is closed from the call until the queue has formed
  (`RaceControl::pit_closed`): the AI does not turn onto the pit route (it
  keeps its wish to stop and turns in at the next pass), the pit autopilot
  does not take a car in, and a car that drives into the lane while it is
  closed is not taken on by its box on that visit
  (`PitState::entered_closed`). A car already on the route when it closed is
  served. Once open, the AI's stop plan treats the race as neutralised, as
  under the virtual safety car ([ai](ai.md#strategy)).
- **In this lap** (`Course::ScInThisLap`): once the queue has formed, nobody
  is being let by, the road has been clear for `VSC_CLEAR_S` and the queue is
  closed up again (or `SC_FORM_LAPS` more laps have gone), its lights go out.
  At the pit entry's station it goes into the lane and along it at the limit
  to the first limit line, where it is gone (on a circuit with no lane it
  leaves the road at the line). A car stranded long enough before it reaches
  the entry puts its lights back on.
- **The restart.** With the safety car off the road the leader sets the
  pace: held to `SC_SPEED_MPS` until `SC_RESTART_ZONE_M` (250 m) before the
  line, free from there; every other car keeps station behind the car ahead
  at `SC_GAP_M`, no faster than the catch pace until the leader is free. The
  race goes green as the leader crosses the line (`ScEvent::Green`); every
  pace is lifted. This is the rolling start's idea (green as the leader
  reaches the line), driven from the queue rather than from the grid.
- Laps are counted and timed under it as under green.
- On the wire: `CompactTelemetry.race_phase` 5 (out) and 6 (in this lap, and
  the restart), and `CompactTelemetry.safety_car`, its pose from its call
  until it is off the road ([protocol](protocol.md)). Client:
  `FApexTelemetryFrame::RacePhase` / `SafetyCar`; the race director draws
  the safety car as a car of its own (`AApexRaceDirector::UpdateSafetyCar`:
  the Murcetes-AMD GT3's body, moved by the field's interpolation, amber
  roof lamps flashing until `race_phase` 6) and the cockpit dash flags the
  phase where the lap count is (`SC`, `SC IN`, `VSC`, `FORM`:
  `ApexRacePhase::DashLabel`), with the pace the driver's car is held to
  after it: `CarState::speed_cap_mps` rides the driver's own
  `DriverFeedback` (`speed_cap_mps`, appended, left off at 0).

## Penalties

Race control gives a penalty (`race_control::Penalty`,
`GameSession::penalise`, run from `update_penalties` each racing tick after
the flags, and from `update_pits`) for each offence the session's
`race_rules` name; none is given before the green, after a car's flag, or
for an offence the rules leave out.

| Offence (`Offence`) | Rule | Penalty |
|---|---|---|
| Jump start (0), found by the false-start watch, given at the green | `jump_start` | drive-through |
| Pass under a yellow (1), from the flags, given `YELLOW_PASS_HANDBACK_S` (10 s) after the pass if the passer is still ahead; handed back within it (the passed car ahead again), it is forgiven, and the order swapping back and forth is one pass | `yellow_penalty` | `YELLOW_PASS_PENALTY_S` (5 s) time |
| Out of a manual pit lane past the red exit light (2): across the last limit line while `pit::exit_closed`, not on the pit route | `pit_exit_penalty` | stop-go, `PIT_EXIT_STOP_GO_S` (10 s) |
| A steward's decision (3), given from the dashboard (`GameSession::give_penalty`), in a race, to a car not yet past the flag | none: it stands under any rules | the steward's: a time penalty or a stop-go of 1 to `STEWARD_MAX_S` (60) s, or a drive-through |

- **Time** penalties are counted at once (`PenaltyStatus::Added`).
- **Drive-through and stop-go** are owed and served in the pit lane within
  `SERVE_WITHIN_LAPS` (3) laps of the lap given on. Past that, or still owed
  at the car's flag, one is added as time (`Penalty::convert`:
  `DRIVE_THROUGH_TIME_S` 20 s, a stop-go that plus its hold); one given
  where it cannot be served (no pit lane, or fewer than 3 laps left) is
  added at once.
- **A drive-through** is served by crossing the lane's last limit line on a
  visit that crossed its first line owing one with no stop between
  (`PitState::drive_through`); the AI's route and the autopilot's go on past
  the box (`serviced` set as the car crosses the first line).
- **A stop-go** is the car's next stop in its box: held for its seconds
  (`PitState::stop_go`), no work done, not counted as a stop for the pit
  rule (`PitService` goes out with nothing but the total).
- The AI serves either at its next pass of the lane (`plan_ai_stops`: an
  owed penalty sets `wants_stop`), and a served drive-through clears it.
- **Classification**: a car's added seconds go on the tick it took the
  flag (`Finisher::penalty_ticks`): finishers on the same lap are ordered by
  that, and its race time and gap carry them (`ClassifiedCar::penalty_s`,
  the results file's `penalty_s` column).
- On the wire: `RosterEntry.Penalties` (`PenaltyData { Reason, Kind,
  Seconds, Lap, Status }`, in the order given, left off when none); the
  roster goes out again whenever one is given, served or added.
- Client: `FApexRosterEntry::Penalties` (`FApexPenalty`, `ApexPenalty`);
  the HUD's `penalty.*` data points and `item.penalty_s` /
  `item.penalty_owed`, the `penalty` component (the newest for 8 s, then
  one owed with its laps left), PEN with the added seconds in the
  standings, tower and results, and the results' times with the seconds
  in ([hud-modding](../game/hud-modding.md)).
- Every penalty goes when the field lines up again (`line_up_on_grid`).

## Incidents

Each race keeps a list for stewards (`GameSession::incidents`,
`incidents::IncidentLog`), filled while it is racing from what the tick
already finds; nothing in the simulation reads it, so a race runs the same
with it.

| Incident (`kind`) | From | Carries |
|---|---|---|
| `contact` | the collision pass's touching pairs (`physics::check_collisions_refs` returns them) | `car` and `other` (the lower id first), the hardest `closing_mps` over the contact and its `severity`: `touch` under `damage::IMPACT_THRESHOLD_MPS` (2.5 m/s, no damage), `light` under `CONTACT_HEAVY_MPS` (8 m/s), `heavy` |
| `yellow_pass` | race control's passes (`take_yellow_passes`), penalised or not | `car` the passer, `passed` |
| `track_limits` | the lap timing: the tick every wheel has been off for `TRACK_LIMITS_SECONDS`, or the lap struck at the line for a missed checkpoint (`cut`) | `car`, `cut` |
| `penalty` | `GameSession::penalise` | `offence`, `penalty` (`time`, `drive_through`, `stop_go`), `seconds` |
| `penalty_cleared` | a steward (`GameSession::clear_penalty`) | `offence`, `penalty` |

Every incident has its session `tick` (a contact: the tick it began) and the
`lap` of its `car`. A pair apart for `CONTACT_GAP_S` (1 s) and touching again
is a new contact; less is the same one rubbing on. The list is emptied as
the field lines up (`line_up_on_grid`), so it is the race's, and a
weekend's practice and qualifying keep none. Unsafe rejoins are not
detected, so not listed.

- **Kept**: a race that finishes writes `<records dir>/results/<session
  id>_<unix time>.incidents.json` beside its results (`[records] results`):
  the cars (id, name, AI, grid slot) and each incident with `race_s`
  (seconds since the green) and `replay_s` (seconds into the server's
  replay of the race: its tick less `IncidentLog::replay_start_tick`, the
  tick the recording began on).
- **Stewards**: the dashboard's Incidents view ([operations](operations.md#admin-dashboard))
  lists a running race's from the live session and a finished one's from
  the file, opens the replay at an incident, gives a penalty
  (`give_penalty`: `Offence::Steward`, the kind and seconds the steward
  picks, served or added as race control's own; the client names it
  STEWARDS' DECISION) and clears one (`clear_penalty`: gone from the car's
  list, owed or added; the roster goes out again without it, and an AI
  coming in only to serve it stays out).
- **In the game**: `RequestIncidents` from anyone in the session (driver
  or spectator) is answered with `Incidents` (`incident_report::wire`):
  the session's tick rate and green, and each incident (the latest
  `MAX_INCIDENTS_SENT`, 500) with its tick, lap, kind, car (roster index
  and name, `INCIDENT_NO_CAR` for one that has left), the other car, a
  contact's severity and closing speed, a cut, a penalty's offence, kind
  and seconds as `PenaltyData` codes. The results screen lists it and
  jumps into the client's own replay
  ([client.md](../game/client.md#session-results-uiapexsessionresultswidgetcpp)).

Tests: `server/sim/tests/all/incident_test.rs` (a forced contact, a car put
off the road, a penalty given and cleared),
`admin_test::incidents_are_listed_cleared_by_a_steward_and_kept_with_the_replay`
(live, cleared, on the wire to a spectator, written, matched to its replay),
`incidents_wire_format` (`C_RequestIncidents`, `S_Incidents`).

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
seating order), `@driver:UUID` (the human whose driver profile has that id,
`Authenticate.driver_id`, under whatever name they join) or a name (a
human's or an AI's). `line_up_on_grid` calls `apply_grid_order` first:

- a requested order: listed drivers lead in order (`grid_order::resolve`,
  each claimed once, a name matching the first unclaimed driver of it),
  the rest follow in seating order;
- else this session's qualifying: the classified lead, the rest behind
  (`grid_order::complete`);
- else the seating order stands.

Under a reverse grid (`race_rules.reverse_grid` N > 1) the first N slots
of whichever order stands are turned round, empty slots included (the
seating order too): the fastest qualifier starts from slot N, as in a
sprint race's reversed top eight.

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
AI profiles; profile ids from `set_driver_id` beside it, from the
connection's `Authenticate.driver_id`, proved by the profile's key where
the client sent one ([protocol.md](protocol.md#connection-flow)). A
stored qualifying line carries the driver's profile id
(`QualifyingEntry.driver_id`, left off the wire without one), and the
client grids that driver by `@driver:` rather than by name
([client.md](../game/client.md#driver-profiles-apexprofilesh-uiapexprofilewidgetcpp)).

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
puts the car in its own pit box (`outlap_box_pose`) when the track has a
pit lane whose exit is in the first half of the lap: served already
(`pit.serviced`), the pit autopilot drives it down the lane and hands it
back past the exit line, as after a stop (a driver who drives the lane
themselves, `manual_pits`, drives it out). Without such a lane, or before
the lane has dealt the car a box, it is put on the road at the pit exit
(`qualifying_outlap_pose`: the lane's `exit_station_m` when in the first
half of the lap, plus `QUALIFYING_OUT_PAST_LINE_M` 30 m), else just past
the line. Always on cold tyres, with `CarState::outlap` set so lap 1 starts
only at the next crossing of the line; a crossing inside the pit lane (a
box before the line) does not count, so the outlap runs on to the next.
The garage trip keeps the car's box and its `manual_pits`.

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
session lobby's GRID list is in that order, each car on its own slot: a
slot the order names a driver for who is not there is shown open. In qualifying the
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
`GetSessionDamage` follows it.

**Each phase's aids**: `practice_assists` and `qualifying_assists`
(`Option<AllowedAssists>`, left off the wire when absent) cap the aids in
practice and in every qualifying round; the race, and a phase not named,
has the session's `allowed_assists` (so that row is the race's caps). When
either is named, each phase begins (`GameSession::begin_phase_assists`,
beside `begin_phase_damage`) by making `RaceSession::allowed_assists` the
phase's (late arrivals are seated under it, `SessionJoined.AllowedAssists`
names it) and clamping every car's aids to it again from what its driver
last asked for (`GameSession::asked_aids`, kept by `set_driver_aids`; a car
whose driver never asked keeps the car.toml's ABS and TC where they are
allowed): an aid one phase forbids comes back in a phase that allows it,
without the client sending anything. A racing line allowed again is planned
for every human at once (`AidLineKey::NONE`, `take_day_lines_due`); the
race day's caps are kept (`race_day_assists`). `WeekendState.AllowedAssists`
carries the phase's caps (left off the wire for a weekend naming none); the
client's `GetAllowedAssists` follows it, the settings' Assists locks show
it the next time the page is built, and a line the phase forbids is
cleared (`ClearRacingLine`). Client: the Race weekend section's Practice
assists and Qualifying assists rows (Race's / Novice / Club / Pro, the
create screen's assist presets; `CreateWeekendPracticeAssists` /
`CreateWeekendQualifyingAssists`, kept in the profile, sent in
`WeekendToSend`, practice's only with practice; the summary names them, and
the lobby's format line adds ", PRO ASSISTS" after the phase;
`-ApexWeekendAssists=P,Q` for an auto race). A hosted session's or
event's spec carries them in its `weekend` as the create does; the
dashboard's form edits neither the session's aids nor a phase's.

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

## Events across restarts

An **event** is a race weekend that outlives its session and the server:
a hosted weekend (`admin::hosted::HostedSpec`: track, the AI's class and
level, lengths, sky, damage, rules) with a name, an entry list, the phase
it is at and each finished phase's results (`server/server/src/events.rs`).
The dashboard runs it ([operations.md](operations.md#admin-dashboard)).

- **Kept** in `<records>/events/<id>.json` (`EventStore`), written on every
  change (atomic: a `.tmp` renamed over the file). The id is the slug of
  the name (`events::slug`: "Autumn Cup" is `autumn-cup`, a second
  "Autumn cup" `autumn-cup-2`), so the same names make the same files; no
  random id is stored. A file that does not read is logged and left alone.
- **Phases** (`EventPhase`): `practice` (when the weekend has practice),
  `qualifying` (every knockout round together, as one day), `race`, then
  `done`. A spec with no weekend is a race alone.
- **Entries** (`EventEntry { driver_id, name, car_id, livery }`) are drivers
  by a **proved driver id**: the connection sent the profile's key and its
  proof checked out ([protocol.md](protocol.md#connection-flow), the id
  bound in `drivers.json`). `EventStore::admit`, from `JoinSession` before
  the lobby seats anyone: no proved id is `Error 403` ("This event takes
  drivers by their proved driver id"); an entered driver is seated in their
  entry's car and livery whatever they picked; a driver not entered is
  entered with the car and livery they join with while `entries_open` and
  fewer than `spec.player_slots` are entered, else `Error 403` ("Not
  entered in this event", "This event's entry list is full"). `SetLivery`
  (and `UploadLivery`, an own livery) in an event's session is `Error 403` ("The livery is fixed for this
  event"). Spectators are always taken.
- **Results** (`PhaseResult { phase, finished_at, lines }`, from
  `events::finished_phase`): practice by best lap (no time behind, by grid
  slot); qualifying as the grid it set (the final round's order, then the
  knocked-out cars by slot, each with its round's lap and
  `knocked_out_in`); the race its classification (`GameSession::classification`:
  laps, best lap, race time, status). Each line names a human by
  `driver_id` and an AI car by `ai`, its 1-based place in the seating
  order (what `@ai:N` names), with its car's id and name (`car_name`). Each
  kept phase names the session it ran in (`PhaseResult::session`). A phase counts once the session is
  `Finished` in it (qualifying: the final round ranked); recorded by the
  event runner and by every advance (`hosted::advance` records before it
  moves on), and then the event is at its next phase. The race recorded,
  the event is `done` and no longer opened.
- **The session.** While `opened` the server keeps one hosted session for
  the event (`events::open_session`; the lobby lists it under the event's
  name): the runner (`events::run_once`, every second while the server is
  healthy, started by `run_server_from` whether or not the dashboard is on)
  records what it has finished and opens a session for any opened event
  that has none: after a restart, every one. A session opened for an event
  at `qualifying` or `race` skips the phases before
  (`GameSession::resume_weekend`, `WeekendProgress::resume_at`: the lobby's
  advance goes to Q1, or to the race's grid or countdown), and at `race`
  grids from the stored qualifying: its lines as `@driver:` / `@ai:` / name
  references in `grid_order` (`Event::race_grid_refs`), so a driver who is
  not back leaves their slot empty, as anywhere. The runner reads the
  session under the server lock and hands the resume data in; nothing in
  the simulation reads the event.
- **On the wire**: every `SessionJoined` for an event's session (a
  driver, a spectator, a rejoin) carries `Event` (`EventStore::joined_event`:
  the id, the name and the phase as `EventPhase::wire` numbers it, 1
  practice to 4 done). A session opened at `qualifying` or `race` has its
  weekend at the lobby (`WeekendState.Phase` 0) until it is started, so the
  client's lobby shows the event's phase as the one that comes next
  ([client.md](../game/client.md#session-lobby-uiapexsessionlobbywidgetcpp)).
  The refusals above are `Error 403` with those reasons; the client puts
  them as sentences (`ApexServerError::Describe`).
- **A phase under way when the server stops is run again** from its start
  (qualifying from Q1): only finished phases are kept. Ending an event's
  session (STOP) ends this sitting; the runner opens it again at its phase.
  RESTART is refused for an event's session.
- **The weekend's sheets** are one record: practice times, the combined
  qualifying sheet and the race classification as kept, written beside the
  event's file on every change once a phase is kept, as JSON
  (`<id>.sheets.json`, `events::sheets_json`: the event's id, name, track
  and phase and every kept phase) and CSV (`<id>.sheets.csv`,
  `events::sheets_csv`: a line per car per phase, `phase,position,name,
  driver_id,ai_id,car,laps,best_lap_ms,race_time_ms,status,knocked_out_in`),
  as a race's results are; the store reads only `<id>.json` back. Each
  phase's replays are the recordings of the session it ran in
  (`events::phase_replays`: `replay_<session>_<unix>.apxs` written after the
  phase before it in that session was kept and up to a minute after it was;
  a qualifying phase has one a round). A client asks with
  `RequestEventSheets { event_id }` and is answered with `EventSheets`
  (`EventSheetsData { Id, Name, TrackId, Phase, Phases }`, each
  `EventSheetData { Phase, FinishedAt, Lines, Replays }`, each line
  `EventSheetLine { Position, Name, CarId, Ai, Laps, BestLapMs, RaceTimeMs,
  Status, KnockedOutIn }`; `events::sheets_data`), or `Error 404` for no
  such event; anyone connected may ask, in a session or not. The
  dashboard exports both files and links each phase's replays.
- **A schedule** (`Event::schedule`, `EventSchedule { practice_at,
  qualifying_at, race_at, open_minutes, round_break_s }`, left out of the
  file when no phase has a time) gives phases real start times, Unix s
  on the server's clock, read by the event runner only (nothing in the
  simulation reads a clock). The dashboard sets it on create or later
  (`EventSchedule::clamp`: each time no earlier than the one before, the
  opening at most a day ahead, the round break 10 s .. 1 h). On it:
  - the runner opens the event (`opened`) once its phase's session is due,
    `open_minutes` (15 by default) before the phase's time
    (`Event::due_to_open`), unless it was closed on the dashboard (`held`,
    cleared by OPEN);
  - the session is run `on_schedule` (`GameSession::on_schedule`, set by
    `open_session` and by the runner): a phase with a time starts at it
    from the lobby (`hosted::start`), a phase after it waits for its own
    time between phases and is advanced then, and the knockout runs on by
    itself, each next round `round_break_s` (120 s) after the last
    (`schedule_step` decides, `run_schedule` acts, every second); a phase
    with no time waits for the dashboard as without a schedule;
  - nobody is waited for: the weekend's race goes straight into its
    countdown (`begin_weekend_race`, no grid card waiting for ready), and
    `refuses_drivers` takes a driver after a qualifying round or the race
    has begun (a running round as a late entry, from the back, as between
    rounds). A driver seated in a race under way (`add_player` in
    `Race`/`Racing`) goes out from the pit lane
    (`GameSession::seat_late_in_pits`): dealt a free box, parked at it as
    a tow to the pits parks a car, held `LATE_JOIN_HOLD_S` (5 s), then the
    pit autopilot drives it out (the driver does with manual pits); a
    track without a pit lane puts the car on the road at its place, clear
    of traffic, as a recovery does. The formation lap still takes nobody.
  A server that was down at a phase's time starts it when it is back.
- **The lobby lists every event** (`LobbyState.Events`, `LobbyEventData {
  Id, Name, Phase, TrackId, StartsAt, SessionId, Entries, Slots }`,
  `EventStore::lobby_events`): `StartsAt` is the phase's time (0 for none),
  `SessionId` the session open for it. The client's session browser lists
  them with a countdown and its lobby counts down to the start
  ([client.md](../game/client.md#session-browser-uiapexsessionbrowserwidgetcpp)).
- **The AI field is the event's** (`Event::ai`, `EventAi { id, name,
  skill, car_id, class }`): fixed when its first session opens from the
  field that session was dealt from the spec, each driver given an id of
  the event's own (`events::ai_id`: a hash of the event's id and the
  driver's place), and stored. Every session of the event, the first
  included, replaces its dealt field with these (`EventStore::ai_field`,
  `GameSession::replace_ai_field`: each profile under its id, name and
  level, in its car), so the same AI drivers run every phase, seated in the
  order of their ids. Their result lines carry `ai_id` beside `ai`, and the
  race grids them by their place in that order (`@ai:N`), as a human by
  `@driver:`.

## Championships

A **championship** groups race events as its rounds and adds up their
races' points (`server/server/src/championships.rs`): `Championship { id,
name, events, points, fastest_lap_points, fastest_lap_any_place }`, kept in `<records>/championships/<id>.json`
(`ChampionshipStore`; the id the name's slug as an event's is). `events` are
event ids in round order (each once, at most `MAX_ROUNDS` 30; an id with no
event is a round still to be made), `points` the points by place, 1st
first (`DEFAULT_POINTS` 25, 18, 15, 12, 10, 8, 6, 4, 2, 1 when none is
given; a place past the table scores nothing). `fastest_lap_points` (0 to
`MAX_FASTEST_LAP_POINTS` 5; 0, the default, is no bonus) are added for
each round's fastest lap; `fastest_lap_any_place` (false by default) says
whether they need a points finish. Both are left out of the file at their
defaults, so a file from before them reads, scores and writes as it did.

**Standings** (`championships::standings`) are worked out whenever they
are asked for, from each round's kept race result (its classification,
"Events across restarts"): a human by their **proved driver id**, so the
same driver adds up across events, restarts and names (the latest name
shown); an AI driver by name, since an event's AI ids are its own. A car
classified retired keeps its place in the round and scores nothing; a
win is 1st and not retired. The table is ordered by points, then wins,
then the better places in turn (count-back), then the name; each
standing carries its place in every round (0 for none) and the rounds
whose fastest-lap points it scored (`fastest_laps`). A round whose
event is not raced yet, or no longer exists, counts nothing.

**The fastest lap** (`championships::fastest_lap`) is a raced round's
lowest best lap in the kept classification, a retired car's included: it
is named on the round whoever holds it, whatever the rule. On equal laps
the better-placed car holds it (the kept result has no time for when a
lap was set). It scores `fastest_lap_points` when there are any, its
holder is not retired, and, unless `fastest_lap_any_place`, their place
scored points; else nobody scores them (they do not pass to the next
fastest). They count in the points before the count-back, which is
unchanged.

The dashboard makes, edits and deletes them in the Events view
([operations.md](operations.md#admin-dashboard)); a client asks with
`RequestChampionships` and is answered with `Championships`
(`ChampionshipsData { Championships }`, each `ChampionshipData { Id, Name,
Rounds, Standings, FastestLapPoints, FastestLapAnyPlace }`,
`ChampionshipRoundData { EventId, Name, Phase, Raced, FastestLap,
FastestLapMs, FastestLapScored }` (the holder by name),
`ChampionshipStandingData { Position, Name, Ai, Points, Wins, Results,
FastestLaps }`; `ChampionshipStore::wire`; the fastest-lap fields left off
at their defaults), shown in the session browser's
LEAGUES list ([client.md](../game/client.md#session-browser-uiapexsessionbrowserwidgetcpp)).
Once an event's race is kept (by the event runner or a dashboard
advance) and the event is a round of any championship, the server sends
`Championships` unasked to that session's humans and spectators
(`championships::push_after_race`), so the client's results screen shows
standings that count the race just run; the client asks for them itself
on joining an event's session, for its lobby
([client.md](../game/client.md#session-lobby-uiapexsessionlobbywidgetcpp)).
The offline season against the AI is the client's own
([client.md](../game/client.md#offline-season-seasonapexseasonh-uiapexseasonwidgetcpp)).

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
the driver's **profile id** when they sent one (`Authenticate.driver_id`;
`LapRecord.driver_id`), else by their **name**. A driver with a profile
takes over the name-only record of their name the first time they are
looked up or set a faster lap (a record from before profiles); renamed, the
profile keeps its records. The AI never sets one. The driver gets `LapRecord`
(theirs, the track record and holder, `HasGhost`) on joining and on beating
it. A corrupt file is logged and treated as empty; writes run on a blocking
task from the broadcast phase, never under the state lock.

**Leaderboards**: `RequestLeaderboard { track_config_id, car_config_id? }` is
answered with `Leaderboard`: the fastest laps on the track, best first (at most
100), every driver's best in every car or in the one car asked for
(`RecordStore::leaderboard`; equal times keep the earlier lap first). Each line
carries the driver, their profile id (`DriverId`, when the lap was set
under one), the car, the time, the three splits and the date. The client's
Leaderboards screen (Garage > Tracks, `UApexLeaderboardWidget`) steps
through the circuits and filters to the garage's car; the driver's own laps
are lit (by profile id, or by name for a lap set without one). Nothing is shared between servers.

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
  end), so a long race does not run its tank or tyres out, and it decides
  as the AI does (`update_racecraft`, the cool-down driver's profile): a
  held car put into a wall backs out, and is put back after
  `RESET_AFTER_S` off the road, as is a debug stand-in's. The roster marks
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

## Rewind in offline practice

A driver alone can wind the session back and drive on from an earlier
moment (`crate::rewind`, `GameSession::snapshot` / `restore` /
`rewind_to`).

**Who.** A session is driven alone when it is of the `Practice` kind (what
the client creates for single-player) and one human sits in it, AI cars
beside them or not (`rewind::sole_driver`). Only that driver may rewind;
anyone else, in any other session, gets `Error 403`.

**The ring.** While such a session races (`SessionState::Racing`) in free
practice, a hotlap, qualifying, timed practice or a race
(`rewind::takes_snapshots`), the game loop takes a snapshot after the
session's tick every `SNAPSHOT_SECONDS` (1 s) of session time (a session
tick that is a multiple of the rate) and keeps `DEPTH_SECONDS` (60) of them
in a ring beside the session (`ServerState::rewinds`). The ring starts
again when the mode or the cars change, and is dropped when the session
stops racing (a countdown, the flag, the lobby), when a weekend phase
begins (a day may bake its own sky into the track) and with the session.

**The snapshot** is the session's whole state between ticks
(`GameSession::snapshot`, a clone of it): the cars, the race, the road, the
sky, the AI, the timing, the flags. It leaves out what a session never
changes, so the ring stays small: the shared car configs and the track's
lines and sidecars (`TrackStatic`: centerline, raceline, ground, curbs,
walls; the road mesh is shared behind its `Arc`). A restore takes those
back from the live session. It is taken outside the simulation and never
read by it; the sim is deterministic, so a restored session fed the same
inputs steps the same ticks to the bit
(`determinism_test::a_restored_snapshot_replays_the_same_ticks`, on
Monza's road mesh and walls, two restores of one snapshot). A snapshot of
a 6-car session takes about 0.4 ms in a release build.

**A rewind** (`ClientMessage::Rewind { seconds }`): the snapshot whose tick
is nearest `seconds` before the session's tick (the older of two as near;
the oldest when asked further back than the ring) is restored, and the
ring's newer snapshots are dropped, so a second rewind goes further back.
`Error 409` while the ring is empty (the first second, or straight after
it was dropped). What the server and the clients know stays as it is now
(`GameSession::restore`): the drivers' names and profile ids, who is away,
the session clocks of the loop, the dashboard's settings, the own-car
feeds, the racing line and pit box each client was last sent. The setup,
the aids and the pit plan go back with the car. The roster is sent again,
and a weekend's or shoot-out's state; the road catches up slice by slice.

**Laps.** Every car's lap in progress at the point rewound to is struck
(`LapTiming::invalid`, as for track limits): it still counts and is timed,
but cannot be a best, a record, a qualifying time or a purple sector. The
lap after it starts clean. Laps finished before the rewind and since
undone were already timed and sent; a record set on one stays.

**The hold.** The cars then stand still for `HOLD_MS` (3 s): the session
is not ticked (no input is made for it, the AI's included) and not
recorded, while its telemetry goes on carrying the tick rewound to. Then it
runs on.

**Recording.** A rewind cuts the recording: the run up to it is written as
a replay of its own, and when one was running a new one starts with the
first tick after the hold (`Rewind::restart_recording`); a live stream
starts again with a catch-up for its viewers. The `.apxs` stream itself
never goes back in time.

**The wire.** `Rewound(RewoundData { Tick, FromTick, HoldMs })` goes to the
session's drivers and spectators ([protocol](protocol.md)). A client drops
what it buffered of the run undone (its interpolation buffers, a
predicting client's history and replay inputs; the next own-car state blob
resynchronises it) and expects telemetry of `Tick` for the hold.

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
Other drivers' garaged cars are hidden. A driving-school session (a
hotlap the client made for a licence test) shows the school's card
instead of the garage ([client](../game/client.md#driving-school-schoolapexschoolh-uiapexschoolwidgetcpp)). `AApexGhostCarActor` is a race car
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
full screen, minimal HUD, simulated by the game itself, so any car, weather,
hour and circuit installed is one start, and no server is needed (a server
refuses the kind).

**Session**: `hotlap_watch::LocalWatch` (`apexsim-protocol`) builds the
session from the client's own `CreateSession` bytes with `HotlapWatch`: one
AI car (`AiDriverProfile::new("Hotlap", HOTLAP_WATCH_SKILL)`, skill 110, in
the asked-for car, `exact_line`), the sky resolved as a server resolves it,
the corners and the road mesh, with ids seeded from the circuit and car;
`max_players`, `ai_count` and `ai_skill` are ignored. Its `watch` block
(`network::HotlapWatchOptions`) carries the `setup` the car runs
(`set_hotlap_watch_setup`: clamped as `SetCarSetup` is, the tuned config,
the pressures fitted again); its `stream` flag is read by nothing now.

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
The level a watch is left at is kept by a record store in
`Saved/ApexSim/Watch` (`RecordStore::keep_watch_level`, `watch_levels.json`),
keyed by track, car, weather, setup and sky (`hotlap_watch_key`; a setup's
clicks folded by `setup_key`, 0 stock; the hour, air and wind by `sky_key`;
`records::watch_level_key` spells it out), and the next watch of that key
starts at it (`LocalWatch::start`): the bad laps are not driven again.

**The ghost.** The watched car's laps are traced as a human's are
(`sample_ghost_trace`, `GHOST_SAMPLE_HZ`, up to `MAX_GHOST_SAMPLES`, ten
minutes). A legal lap faster than the ghost becomes the ghost
(`update_hotlap_watch`). The fastest one per track and car is kept in
the same store (`RecordStore::keep_watch_ghost`,
`watch_ghosts/<track>-<car>.msgpack`, when the watch ends), and the next
watch of that track and car starts with it, whatever the setup or sky. The stream carries it as a `Ghost` record
each time it is replaced, and in a newcomer's catch-up
([spectator](../game/spectator.md)). A watch's laps never reach the record
store.

**Corners** (`track_guide::wire_corners` -> `TrackCorners`): the guide's
`detect_corners` in lap order, named from the dossier's `display_name`
(`name_corners`; else "Turn N" on the client). See
[track guide](../../../apexsim-content/docs/track-guide.md).

**Client** (`UApexRootWidget::StartHotlapWatch` / `ChangeHotlapWatch`): the
pending circuit and car, the last sky, and stock. It needs the physics
library and the content (`UApexSpectatorSubsystem::CanSimulateHotlap`:
`ApexSimContent`); without them it says why and does not start.
`UApexDemoModeSubsystem::WatchHotlap` -> `UApexSpectatorSubsystem::WatchHotlap`
starts the `LocalWatch` (`apx_watch_start` / `_tick` / `_take` /
`_corners` / `_free`, ABI 5); the spectator subsystem's `Local` source ticks
it on the game's clock and plays its records through the live path on the
backdrop feed, so the timeshift gives it pause, seek and speed, and its
ghost rides beside the car. `UApexNetSubsystem::BeginLocalHotlapWatch`
holds `IsHotlapWatch`, the corners and the summary `FindSessionById`
answers from. `server/ffi/tests/watch.rs` checks the API.

The keys:
- Left / Right: the car.
- `W`: the weather.
- `[` / `]`: the hour.
- Page Up / Down: the circuit.
- `S` / D-pad up: the setup (the garage's saved setups for the car, then stock).
- 1-3: the rubber (green, normal, rubbered).
- 4 / 5: the air, two degrees a press from what the sky resolves to.
- 6 / 7: the wind (calm, 8, 15, 25, 40, 60 km/h).

All of these are batched for 0.8 s (`HotlapRestartAt`).

The replay keys are a transport. `R` / D-pad down drives the ghost
round with the replay's cutting cameras (`ReplayHotlapLap`,
`AApexRaceDirector::BeginGhostReplay`), and `G` turns the ghost on or off
(`bGhostCar`).

A watch with no first lap's stream in 15 s, or one that stops, returns to
the menu.
The HUD is the `hotlap_watch` scene: while `hotlap.active` only the
`hotlap_*` components show (`hotlap_transport` is the stream's;
see [HUD modding](../game/hud-modding.md)). `-ApexWatchHotlap
-ApexTrack=<stem> -ApexCar=<name> -ApexWeather=<sky> -ApexTimeOfDay=HH:MM`
starts one at launch; `apexsim.watch setup|replaylap|ghost` work as the keys do.

## Checking it

```bash
cd server/sim                                        # the unit tests of the simulation crate
cargo test --lib game_session                 # mode ticks, timed race, grid order, qualifying, timed qualifying, weekend
cargo test --lib weekend                      # knockout sizes, format clamp, round closing
cargo test --lib race_control                 # yellows, warnings, hold, own and passed flags, blue (across classes too), a pass under yellow
cargo test --lib racecraft                    # attack, defend, room, a quicker class let by
cargo test --lib safety_car                   # the pick-up, the let-by, a pass of the safety car, the restart at the line
cargo test --lib time_penalty                 # a time penalty reorders the finishers
cargo test --lib race_start                   # the approach's pace, headway across the line
cargo test --lib recovery                     # spot choice, traffic release

cargo test --test all race_start_test::             # formation lap back to the grid, rolling start, a jump start, a held grid
cargo test --test all penalty_test::                # the AI's drive-through and stop-go at the Norisring
cargo test --test all lap_timing_test::             # sectors and track limits round Monza
cargo test --test all recovery_test::               # back to track, a taken spot, back to pits, refused moving
cargo test --test all determinism_test::            # incl. a restored snapshot replaying its ticks, a rewind striking the laps
cargo test --test all multi_class_test::            # the AI dealt per class and ranked, the grid and the classification by class
SURVEY_CLASSES=LMP2:6,GT3:6 SURVEY_TRACKS=Snorisring cargo test --release --test all ai_race_start_test::survey -- --ignored --nocapture   # a two-class AI race

cd ../server
cargo test --test all sim_wire_test::               # the yellows at Monza on telemetry; a weekend day's own sky
cargo test --test all weekend_test::                # a weekend from the lobby to the race over the wire
cargo test --test all event_test::                  # an event's practice and qualifying, a restart, its race gridded from them
cargo test --test all sim_wire_test::a_resumed      # a resumed weekend skips the phases run
cargo test --lib events                             # ids, entries, the event file, the grid from stored qualifying
cargo test --test all shootout_test::               # a shoot-out run at the Norisring, order and classification; over the wire
cargo test --test all race_flow_test::              # incl. test_timed_race_flow_to_the_flag_monza
cargo test --test all session_conditions_test::     # rules echoed and enforced end to end
cargo test --test all grid_order_test::             # start order, stored results, over the wire
cargo test --test all ready_grid_test::             # the race's grid: OpenGrid, SetReady, the countdown
cargo test --test all safety_car_test::             # the VSC out and in; the safety car out of the pit exit, the queue, a lapped car let by, the lane shut and open, the restart
cargo test --test all reconnect_test::              # held seat, resume, takeover, grace, Disconnect
cargo test --test all rewind_test::                 # a practice driven alone rewound: the hold, the struck lap, the cut recording; refused online
cargo test --test all hotlap_test::                 # garage, run-up, queue, cold tyres, qualifying outlap
cargo test --test all hotlap_watch_test::           # hold, laps, determinism, corners, ghost, setup, a server refusing the kind
cargo test --release --test all hotlap_watch_test::watch_probe -- --ignored --nocapture   # WATCH_TRACKS/CARS/LAPS
cargo test --release --test all hotlap_test::generate_ghost_fixture -- --ignored          # APEXSIM_GHOST_DIR, APEXSIM_GHOST_PLAYER
```

Golden bytes ([protocol](protocol.md)): `cargo test <name> -- --nocapture`
for `assists_wire_format`, `session_damage_wire_format`,
`session_ai_skill_wire_format`, `session_classes_wire_format`, `race_time_wire_format`,
`qualifying_time_wire_format`, `qualifying_cutoff_wire_format`, `pit_rule_wire_format`, `shootout_wire_format`, `weekend_wire_format`, `grid_wire_format`,
`lap_timing_wire_format`, `telemetry_compact_wire_format`,
`hotlap_wire_format`, `hotlap_watch_wire_format`, `recover_wire_format`, `track_corners_wire_format`,
`rejoin_wire_format`, `rewind_wire_format`.

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
