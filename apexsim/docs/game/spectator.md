# Spectator stream, showcases, watching and replays

Everything a viewer sees of a race without driving in it goes through one
format, the **spectator stream** (`.apxs`): the menu's backdrop race, the
server's showcase channels, the client's own replays, the promo clips and the
track guide's recordings. A stream is rendered offline by `apexsim-replay`
(a headless AI race on the server's own `GameSession`), played from a file or
over the wire, and fed into the client through the same paths a live session
uses, so the race director, cameras, HUD and engine sound need nothing
special. Watching a *live* session from the browser still uses the racer
telemetry, not the stream.

## Code

- `server/protocol/src/spectator.rs`: records, the car row, the file (blocks, index),
  `BroadcastEncoder` (session state to records), `patch_epoch`, `RaceScore`;
  `server/protocol/src/spectator/columns.rs` (the inside of a version 2 block);
  `server/protocol/src/session_stream.rs` (`SessionStream`: a session, tick by tick, as
  the records the format promises) and `server/server/src/stream_recorder.rs` (the
  server's replay files).
- `server/server/src/replay_tools.rs` and `server/server/src/bin/apexsim_replay.rs`: the
  `apexsim-replay` tool (`simulate`, `info`, `find`, `pose`, `cut`, `render`,
  `convert`, `repack`, `guide`); `server/server/src/replay.rs`: the `.bin` replay format of the offline tools.
- `server/server/src/showcase.rs` (`ShowcaseState`, channels),
  `server/server/src/game_loop/showcase.rs` (messages), `server/server/src/health.rs`
  (`/showcase`).
- Client codec, pure: `ApexSimNet` `ApexSpectatorStream.h`
  (`FApexStreamFile`, `FApexSpectatorPlayer`), `ApexSpectatorWriter.h`
  (`FApexStreamWriter`, `FApexStreamCarRow::FromTelemetry`).
- Client playback: `UApexSpectatorSubsystem` (sources: a file, the
  server's records, or a watched hotlap simulated here, `Local`), `UApexDemoModeSubsystem`
  (backdrop source), `UApexNetSubsystem` (backdrop feed),
  `UApexReplayRecorder`, `UI/ApexReplaysWidget`, `Race/ApexSpectatorView.h`
  (`ApexSpectate`), `UApexReplaySubsystem` (`-ApexReplay=` clips).
- Photo mode: `Race/ApexPhotoMode.h` (`ApexPhoto`, pure), `UI/ApexPhotoModeWidget`,
  `AApexRaceDirector::BeginPhotoMode`, `UApexRootWidget::BeginPhotoMode`.
- `showcase.yml`, `scripts/lib/ApexShowcase.ps1`.

## The stream format

A stream is a sequence of records framed like the TCP protocol,
`[u32 big-endian length][MessagePack body]`. Each body is a **positional**
array whose first element is the record type. Fields are only ever appended
and readers skip what they do not know (`a_newer_writers_extra_fields_are_skipped`).

| Type | Record | Live transport | Carries |
|---|---|---|---|
| 1 | `Header` | TCP, on join and on loop | version, stream id, tick and frame rates, row size, track (id, stem, display name, `source_crc`, length), resolved conditions, session kind, mode, lap limit, ticks, the render's seed and score |
| 2 | `Roster` | TCP, on change | revision; per car: index, car config id, `content_crc`, livery, name, is_ai |
| 3 | `Frame` | UDP | tick, roster revision, state, countdown, part / parts, a `bin` of car rows |
| 4 | `Event` | TCP | tick, kind, payload |
| 5 | `Block` | file only (version 1) | a second of records, zlib |
| 6 | `Index` | file only | first tick and offset of every block |
| 7 | `Path` | TCP, with the header | the centerline every 10 m, for the TV cameras of a viewer with no lobby |
| 8 | `Road` | TCP | tick, a slice of the road state (lap, cell length, bins, half span, first cell, rows, debris, geometry, snow rows, water rows): the `RoadState` message's payload ([../server/conditions.md](../server/conditions.md#the-road-on-screen)) |
| 9 | `State` | TCP | tick, the sky as it is now (`SkyNow`), a timed race's clock, and per car the slow figures a row leaves out (`CarExtra`: tyre pressures, flat spots, brake temperatures and wear, tank, coolant, tread shoulders, hybrid, tow, where each body zone was hit); four times a second |
| 10 | `Ghost` | TCP | tick, a lap's trace as `GhostLap` carries it (lap time, sample rate, track and car ids, a `bin` of 20-byte samples: ms, x, y, z, yaw): a watched hotlap's fastest lap on the server's live stream (`StreamGhost`, each time it is replaced, and in the catch-up), and the record lap in a client-recorded hotlap replay (the recorder writes it when a hotlap's ghost arrives); `apexsim-replay`'s renders carry none |
| 11 | `ColumnBlock` | file only (version 2) | first and last tick, records, `raw_len` (their framed size), layout, the same records laid out by column, zlib |

**Joining mid-file.** What a frame does not hold is caught up from the
records already played: a showcase channel sends a newcomer, after the
preamble, the timing lines crossed so far, the road as the file last laid
each of its slices and the latest `State` (`showcase::catch_up_bodies`); a
file seek on the client applies every record on the way but the frames.

**Epoch.** The second element of every viewer-facing record is the epoch,
always written as a full `uint 32`, so it sits at bytes 3..7 of the body. A
showcase that loops stamps a new epoch into the file's bytes
(`spectator::patch_epoch`) and forwards them undecoded; a `Frame`'s and an
`Event`'s tick is written the same way right after it. The roster revision
counts roster changes within an epoch. The client drops a frame whose epoch or
revision is not the one it holds (`FApexSpectatorPlayer`).

**Frames are self-contained.** No frame refers to another (there are no delta
frames on the wire: a delta would need acknowledgements on UDP, and a file's
column blocks take the deltas inside a block only), so a lost datagram costs one frame and a
viewer can join or a file seek anywhere. A field too
large for one datagram (more than `MAX_ROWS_PER_PART`, 21 cars) is sent in
parts of the same tick; the client applies whatever parts arrived once a newer
tick comes.

**The car row** (`ROW_SIZE` 66 bytes, little-endian; 44 in a version 1 file,
`ROW_SIZE_V1`, which still plays with the tyres unknown, 52 in one from
before the wheels, `ROW_SIZE_V2`, which plays with the wheels unknown, 64
in one from before the ground and the flags, `ROW_SIZE_V3`, which plays
with both clear):
status bits, pose in mm and angles, speed, steering, pedals, gear, rpm, lap,
station, finish position, the lap / pit / ERS flag bytes as in
`CompactCarState`, compound, the five damage percentages, tyre wear and
tread temperature, then each wheel's slip ratio and slip angle
(`ROW_SLIP_SCALE`-ths of the tyre's peak, as the driver feedback carries
them, from `CarState::wheel_slip_*`) and the surface under it (0 road, 1
curb, 2 off): what the watched car's road sound plays from in a replay
(`AApexRaceDirector::UpdateWatchedCarRoadSound`); then the soft ground under
each tyre (`CompactCarState.ground`: a replay's dust and gravel) and race
control's flags (`race_flags`: the blue, the yellow ahead). The client's recorder fills
them for the own car from its driver feedback; the other cars of a
client-recorded replay have none. Pressures, flat spots, brake temperatures,
fuel and the hybrid ride in the `State` record instead; the steering forces
are in no stream, and the client leaves what it lacks "unknown" (-1).
Layout: `spectator.rs`.

**The state record** carries what changes too slowly to ride every 30 Hz
row: `SkyNow` (so a showcase, a replay and the backdrop show the sky the race
ran through, rain arriving and the clock moving, not the sky it started
under), a timed race's `RaceClock`, one 49-byte `CarExtra` per car (44 in a
stream from before the discs' wear and the stint's energy, 40 from before
the second dents, 36 from before the damage centres; layout in
`spectator.rs`), the race's yellow sectors (appended, nil while none
is out, absent from an older stream) and the race's phase after them
(`race_phase`: the formation or pace lap, the VSC, the safety car; nil at
green, absent from an older stream). The player keeps the latest and puts it on every frame it
completes (`Frame.Sky`, `RaceLeftMs`, `RacePhase`, each car's pressures,
flat spots, brakes and tank), so the race director, the HUD (the
`race_phase` banner) and the tyre material need nothing special. The
client's own replays write the same record. `BroadcastEncoder::state` makes it, `STATE_EVERY_S` (0.25 s) is its
rate.

**Events**: `LapTiming` (the `LapTiming` message's own fields),
`TrackSectors`, `SessionState`, `Finish`, `Retired`, `PitStop` (entered,
serviced, left), `Contact` (a car started colliding: the TV director's
incident cue), `PitService` (the crew's plan as a car stops at its box, which
the pit-stop panel shows; `BroadcastEncoder::pit_services`).
`BroadcastEncoder::observe` raises the others from one tick's state to the
next.

**The file** is `"APXS"` + u16 version, then a plain preamble (`Header`,
`Roster`, `Path`, `TrackSectors` event: what `info` and the client's catalog
scan read), the blocks, a plain `Index`, and a u64 offset of the index as the
trailer. A live stream is the same records without blocks and index.

Version 2 (`FILE_VERSION`, what the server, `apexsim-replay` and the physics
library write) holds `ColumnBlock`s of `BLOCK_SECONDS` each; version 1 (what
the client's own recorder writes, and every file from before) plain zlib
`Block`s of a second. Every reader takes both, and a block of either version
gives back the same record bodies in the same order, so nothing that plays,
seeks, catches up or cuts a file knows which it read. A column block holds
the block's frames rearranged before zlib (layout 1,
`spectator/columns.rs`): the frame headers as columns, each frame's car
order, then per row field the values car after car, the pose, speed, rpm,
lap and station as the difference from the car's row before in the block
(the block's first from 0, so a block still stands alone), every multi-byte
column split into byte planes; the other records follow as they are. Only a
frame of whole `ROW_SIZE` rows whose bytes this build would write for it is
laid out, anything else is kept verbatim, so the round trip is exact for any
block (`a_column_block_gives_back_every_body`). It is half the size of the
plain block: 16 GT3s over 6.4 min of Zandervoort are 3.27 MB in version 2
and 6.41 MB in version 1, 31 MB against 61 MB for a 16-car hour. The client's
codec unpacks a column block through the physics library
(`apx_stream_block_raw`, ABI 6, the server's own code); a client built
without it still plays version 1 files and names the library for a
version 2 one. `apexsim-replay repack <in.apxs> --out <out.apxs>` rewrites a
file of any version as this build writes it and checks it reads back to the
same records; `info` prints the `file_version` and a `records_crc` over the
records, the same for one race in either version.

**Golden bytes**: `cargo test spectator_wire_format showcase_wire_format --
--nocapture` prints them; the client pins them in `ApexSpectatorGoldenBlobs.h`
(and a `File` copy in `BackdropTests.cpp`): `File` is a version 1 file,
`FileV2` the same content in version 2 (`ApexSim.Spectator.FileV2` reads both
to the same records). See [protocol](../server/protocol.md)
for the workflow.

## apexsim-replay

A server binary (run from the code root: content paths are relative, and the
content is the sibling `../apexsim-content`); every reporting command prints
JSON on stdout.

```bash
apexsim-replay render --track ../apexsim-content/tracks/default/Zandervoort/Zandervoort.yaml --class GT3 \
    --cars 20 --laps 2 --weather sunny --time 13:00 --seed 7 [--seeds 10] \
    --cars-dir ../apexsim-content/cars/default --out build/showcase/Zandervoort.gt3.day.apxs
apexsim-replay showcase --name Zandervoort.gt3.day --out build/showcase/Zandervoort.gt3.day.apxs
apexsim-replay info  build/showcase/Zandervoort.gt3.day.apxs [--check]
apexsim-replay convert out/race.bin --rate 30 --out out/race.apxs
apexsim-replay simulate --track <yaml> --car yotota-lmp2 --ai 12 --laps 3 --seed 1 --out out/race.bin
apexsim-replay find out/race.bin --corner Hugenholtz --before 150 --after 120 --min-cars 3
apexsim-replay pose --track <yaml> --corner "Eau Rouge" --offset -60 --lateral -25 --side outside --height 6
apexsim-replay cut  out/race.bin --from-s 330 --to-s 345 --out out/clip.apxs
apexsim-replay guide ...    # the track guide, see track-guide.md
```

- **`render`** runs the grid, the countdown (`--countdown`, 8 s), `--laps`
  from green and `--tail` (10 s) past the first car to take the flag;
  `--from-tick` / `--to-tick` keep a window, `--max-seconds` (1800) stops a
  race nobody finishes. `--class` deals the class's cars as
  `game_session::class_field` does (the class's first car by folder hosts);
  `--car` picks the host car instead. `--cars-dir ../apexsim-content/cars/default` keeps
  the player's own cars out. The sky takes every `SessionConditions` field
  (`--weather`, `--time`, `--air`, `--wind`, `--wind-from`); the AI drove it,
  so it travels resolved in the `Header`. `--seeds N --pick best` renders N
  seeds and keeps the best `RaceScore` (close racing for; contact, off-road
  time and retirements against). `--ai-level 95` races the field at that
  AI level (`ai_driver::field_skills`, as a host's level); without it the
  mixed field, novice to ace. Recorded at `--rate` (30). The same
  arguments write a byte-identical file (the session and host ids are seeded
  too).
- **`showcase --name <Stem>.<class>.<variant>`** renders one entry of
  `showcase.yml` (`--list`, the code root's by default) on
  `--tracks-dir` / `--cars-dir` (`../apexsim-content/tracks` and
  `cars/default`): `render` with the entry's options, read by
  `apexsim_protocol::showcase_list`, which is also what the game's own
  render reads, so the two write the same file. The pipeline renders
  through it.
- **`info --check`** exits non-zero when the track's `source_crc` or a car's
  `content_crc` no longer matches the content on disk (`check_stream`); a 0
  checksum (a converted replay) is not compared. It also calls a version 1
  row size stale. This is how the pipeline decides to re-render.
- **`convert`** turns a `.bin` replay into a stream: no pit, compound, damage,
  hybrid or lap timing (a replay holds none), checksum, path and sectors from
  the track YAML.
- **`simulate` / `find` / `pose` / `cut`** are the promo tools: a seeded
  headless race as a `.bin` replay, the moments the field runs through a
  stretch together (a dossier corner by name, or `--station`), a camera point
  beside the road (`--side outside|inside` of the bend, seated on the ground
  heightfield, `--look-landmark big_wheel`), and a window cut to `.apxs`
  (`.json` still writes the old clip, `replay_tools::ClipFile`). See
  [marketing site](../marketing-site.md) for the pipeline that drives them.

**Showcase files** are named `<Stem>.<class>.<variant>.apxs` (variant a sky:
`day`, `dusk`, `rain`, `night`), each an entry of `showcase.yml` (an entry's
`ai_level` is the render's AI level). No build stage renders them and no
package ships one: the game renders its own (below) and the server its own
(Server showcases), both from the list and their content, with the code
`apexsim-replay showcase` runs, so every one of them writes the same bytes.
A package ships the list as `Game/showcase.yml` and `Server/showcase.yml`,
with `apexsim-replay.exe` beside the server for rendering one by hand
(`build/showcase/`, gitignored, is where the commands above write).
Imported tracks are never rendered from the list. Rendering takes seconds
per circuit in a release build (16 s for Zandervoort's 16 GT3s over two
laps).

**Showcases made in the game.** The physics library renders a showcase too
(`apx_showcase_render`, ABI 5 and on: the list, the tracks and cars, a name, an
output path and a cancel flag; `apx_showcase_names` lists the entries), with
the server's own code (`apexsim-protocol`'s `render` and `showcase_list`),
so its file is the build's byte for byte
(`server/ffi/tests/showcase.rs`, and its ignored
`the_shipped_showcase_renders_again_through_the_api` in release against
`build/showcase/`; `ApexSim.Backdrop.ShowcaseRenderMatchesBuild` in the
game's own build; both skip without that file, so render it with
`apexsim-replay showcase --name Zandervoort.gt3.day` first). `UApexDemoModeSubsystem` works out which entries have no
fresh local file (`ShowcasesToRender`: none of that name, or none whose
track and car checksums match the catalog, on a track this machine draws
and has the YAML of; the pending track's first) and renders them one at a
time on a below-normal thread (`FApexShowcaseRenderer`) into
`Saved/ApexSim/Showcases/`, which the backdrop scans; a finished file joins
the local files at once. On a fresh install the first one is ready some
seconds after the menu opens (the splash does not wait for it; the page
backgrounds show until then). It never starts one
while the player is in or joining a session, a failed one is not retried
that run, and leaving the game cancels one under way. The inputs
(`FApexShowcaseRenderer::FindInputs`): `-ApexShowcaseList=` and
`-ApexContentDir=`, else in the editor the code root's `showcase.yml` and
the content root, in a package `Game/showcase.yml` and
`Server/content`'s tracks and `cars/default`. `apexsim.showcase.Render 0`
or `-ApexNoShowcaseRender` turns it off; `-ApexShowcase=<file>` (one file
to play) does too, while `-ApexShowcase=<id>` keeps that one entry alone,
rendered if no file has it.

## Server showcases

```toml
[showcase]
enabled = true
dir = "./showcase"        # absent or missing: no showcases
list = "./showcase.yml"   # rendered into dir at startup; absent: nothing rendered
playlist = ["*"]          # file names without .apxs, channel order; "*" every file not named
mode = "loop"             # or "rotate": a channel moves on to the next file at its end
stream_divisor = 1        # send every Nth frame
```

`APEXSIM_SHOWCASE_ENABLED` / `APEXSIM_SHOWCASE_DIR` / `APEXSIM_SHOWCASE_LIST`
override. At startup the server reads each file's preamble and drops (with a
log line) one whose track or cars it lacks or whose checksums disagree with
its content. Then it renders every entry of `list` that the playlist takes
and that has no file left (`showcase::to_render`: on a track it has, not one
the AC importer wrote) into `dir`, one after another on a thread of its own
(`spawn_renders`, the field from `<cars>/default`), each written aside and
moved in, then listed as a channel at the end; the lobby's next
`ShowcaseAvailable` tells clients. So a release's first start renders its
showcases, and a car or track edit renders the ones it touched again. The
development `server/server/server.toml` names no list in reach, so a dev server
renders nothing; the Docker config renders into `/data/showcase`, which the
content job's sync leaves alone.

A **channel** is one file on one clock shared by all its viewers. The game
loop advances every watched channel once per tick (`ShowcaseState::advance`)
and fans out the file's record bytes as they are, epoch stamped in: no
physics, no per-viewer encoding. Frames go as bare record bodies over UDP
after the `UdpHandshake` (TCP, droppable, before it); everything else as
`SpectatorRecord`, a `bin` of framed records, so a newcomer's preamble and the
lap timing so far are one message. A channel nobody watches drops its inflated
records; the first viewer's join inflates the file on a blocking task off the
loop. At the end the channel takes a new epoch and resends the `Header`
(`loop`) or moves to the next file (`rotate`).

| Direction | Message |
|---|---|
| C to S | `ListShowcases` -> `Showcases` (per channel: id, track, class, conditions, duration, cars, viewers) |
| C to S | `SpectateShowcase { id }` (none: the first channel) -> `SpectatorJoined` then a `SpectatorRecord`; `Error 404` for an unknown id, `400` from inside a session |
| C to S | `SpectateSession { session_id }` -> `SpectatorJoined` (`kind` Live) then a `SpectatorRecord` holding the catch-up, then the session's records as it ticks; `Error 404` for an unknown session, `400` from inside a session |
| C to S | `LeaveSpectate`; also implied by `CreateSession`, `JoinSession`, `JoinAsSpectator`, `SpectateSession` and a disconnect |
| S to C | `LobbyState.ShowcaseAvailable` tells a client to ask; `LobbyState.LiveStream` that `SpectateSession` is understood (left off the wire by a server without it) |

The health port serves `/showcase` (JSON) and the metrics
`apexsim_showcase_viewers{showcase=...}` and `apexsim_showcase_frames_sent`
(see [operations](../server/operations.md)).

## Watching a live session as a stream

`SpectateSession` puts a viewer on the session's `SessionStream`
(`session_stream.rs`, made on the first viewer or when the recording starts,
dropped when neither is left). The viewer is not a participant: no seat, no
telemetry of its own, no roster message. `tick_sessions` hands the stream
the tick's lap events and pit stops, and the records that come back go to the
recorder and to `LiveWatchers`' viewers through the same `showcase::fan_out`
a showcase uses (frames as datagrams, the rest as `SpectatorRecord`).
A newcomer is excluded from the tick it joins in; it gets
`SessionStream::catch_up` instead: header, roster, path and sector lines, every
timing and pit event so far (`History`), the road as last laid and the latest
slow state. So a mid-race joiner sees the earlier laps and counts the earlier
stops, which the racer telemetry (`JoinAsSpectator`) never carried. The
game's own watched hotlap (`hotlap_watch::LocalWatch`,
[sessions](../server/sessions.md#watching-a-hotlap)) hands out the same
records, its ghost in the catch-up.

## The client player and the menu backdrop

`UApexSpectatorSubsystem` plays records through `FApexSpectatorPlayer` from
one of two sources:

- **File**: the `.apxs` is read and inflated on the thread pool, then records
  are released as the game clock (delta time x the file's tick rate) reaches
  their tick. At the end the preamble is applied again, which the player takes
  as a new epoch: the cars jump back to the grid cleanly.
- **Net**: `UApexNetSubsystem` routes `SpectatorJoined`, `SpectatorRecord`
  and frame datagrams (`PopSpectatorRecord`) to `OnSpectatorRecords`. A
  showcase is applied as it arrives. A live session's stream is also kept
  (the **timeshift**: `KeepLive`, every body with its tick, frames trimmed
  oldest first past `LiveBufferCapBytes`, 256 MB). At the live edge it plays
  as it comes in. Paused, slower than real time or seeked back, it plays the
  kept records on its own clock, as a file does, until it catches up
  (`CanTimeshift`, `IsAtLiveEdge`, `GetSecondsBehindLive`). A seek back
  starts from the first kept record, the catch-up's header.

Either way the result goes out as the net subsystem's **backdrop feed**
(`BeginBackdropFeed`, `FeedBackdropRoster`, `FeedBackdropTelemetry`,
`FeedBackdropSectors`, `FeedBackdropLapTiming`, `EndBackdropFeed`), which
makes `IsInDemoSession()` true and raises the delegates a live session does:
roster entries (player ids `stream-<index>`), `FApexCarTelemetry` per car into
each car's motion buffer, `TrackSectors` and `LapTiming` into
`FApexTimingBoard`. The `Header` gives the race director the track and the
sky; `Path` gives the TV director its trackside cameras.

`UApexDemoModeSubsystem` picks the backdrop source (`EApexBackdropSource`):

1. Connected and `ShowcaseAvailable`: the server's channel for the pending
   track if this machine has its export, else another, not the same one twice
   running. A connection under way gets `ServerGraceSeconds` (3 s), so with a
   server the showcase wins the startup race.
2. A local file from `Saved/ApexSim/Showcases/`, where the game renders
   them (`-ApexShowcaseDir=` adds folders), the pending track's when one
   matches (`ChooseFile`). Files are scanned at
   startup by preamble (`FApexStreamFile::ReadPreamble`) so the splash hold's
   `IsDemoExpected` can count on one offline. A file whose track or car
   checksums do not match this machine's catalog rows is skipped (0 is
   unknown and passes).
3. The static page backgrounds.

A stream's sky is the file's (the AI raced in it). `-ApexShowcase=<file|id>`, `-ApexNoShowcase`,
`apexsim.spectate.Info` (source, epoch, rates, frames applied / dropped),
`apexsim.spectate.Next`, `apexsim.demo.Restart`, `apexsim.demo.MaxMinutes`.
The cameras are in [cameras](cameras.md).

## Watching a race

Any race the director has on screen can be watched full screen
(`UApexRootWidget::WatchBackdrop`, `WatchSession`, `WatchReplay`):

- **Main menu > Watch a race > Live race**: the backdrop, kept across its next races.
  When one finishes, the `spectator_results` HUD component shows its result
  (winner's race time, gaps at the flag, best laps) and the race stays up
  for 30 s before the next (6 s when nobody watches it full screen;
  `UApexDemoModeSubsystem::SetWatched`). The next-race key moves on sooner.
- **Session browser > Watch**: a live session as a stream when
  `LobbyState.LiveStream` says the server has it (`WatchLiveStream`:
  `UApexDemoModeSubsystem::WatchLive` puts the session on the backdrop feed
  like a replay, source `Live`, which stays until the player leaves it). Against
  an older server it is `JoinAsSpectator`: the server answers `SessionJoined`
  with grid position 0 and the session's `TrackSectors`, and marks the roster to
  be sent again (`GameSession::mark_roster_dirty`); the spectator gets the full
  racer telemetry. `UApexNetSubsystem::IsSessionSpectator` is true and the race
  view opens with no car.
- **Main menu > Watch a race > Replays**: a saved replay, below.
- A watched hotlap: see [sessions](../server/sessions.md).

The race director holds the watched car (`FocusCar`, `StepFocus`,
`FocusPosition`), the camera (TV locked on the car, chase, onboard with the
cockpit rig; `SetSpectatorCamera`) and whether the TV director picks the car
(`SetSpectatorAuto`), under `SetSpectating`. `ApexSpectate` is the pure part
(order, stepping, the key map). Keys reach `UApexRootWidget::HandleWatchKey`
through the input processor, so none reaches the hidden menu or a car:

The keys are the defaults of Settings > Controls > Watching (`ApexInput::Slots`,
the `Watch*` actions; `ApexSpectate::CommandFor` reads the player's own), and
the HUD's hints print what is bound (`watch.key.*`). Not rebindable: the number
row, the left and right arrows and the D-pad's up and down (they also step cars).

| Keyboard | Pad | |
|---|---|---|
| Up, Left / Down, Right | LB / RB, D-pad up / down | the car ahead / behind |
| 1-9, 0 | | P1-P9, P10 |
| C | Y | camera: TV, chase, onboard |
| A | X | the TV director picks the car |
| T | View | timing tower column |
| H | R3 | hide the overlay |
| N | L3 | the next race (the backdrop) |
| Space | A | pause a replay or a live stream |
| comma / full stop | LT / RT | ten seconds back / on in a replay or a live stream (no further than its edge) |
| - / = | D-pad left / right | slower / faster (0.25x to 4x) |
| S, R, G | D-pad up, down | a watched hotlap's setup, its fastest lap again, its ghost ([sessions](../server/sessions.md#watching-a-hotlap)); elsewhere the D-pad's up and down step cars |
| 4 / 5, 6 / 7 | right stick down / up, left / right | a watched hotlap's air a step cooler / warmer and wind a step lighter / stronger (`EAction::AirCooler` and friends, not rebindable; console `apexsim.watch air- air+ wind- wind+`) |
| Backspace | B | stop watching |
| P | (pause menu: PHOTO MODE) | photo mode, below |
| Esc | Start | the pause menu |

With the mouse: a click on a timing tower row watches that car, on the replay's
bar goes to that place, and on the transport's buttons or a key hint does what
the key does (HUD `click`, [HUD modding](hud-modding.md#clicking);
`UApexRootWidget::HandleWatchClick`, taken in `FApexMenuInputProcessor`; console
`apexsim.watch click <x> <y>` for unattended runs). The Replays screen has Keep
and Delete beside each row for the mouse, and its hint bar follows the device
in use (`OnInputDeviceChanged`).

The HUD follows the watched car and shows the race's own circuit and
distance; the `spectate.*` and `replay.*` data points and the `spectator_*`
components are in [HUD modding](hud-modding.md). A live stream's transport is
the replay's (`replay.live`, LIVE or how far behind it plays).

A ghost on the feed (a streamed hotlap's, a hotlap replay's) rides beside the
watched car (`AApexRaceDirector::UpdateWatchGhost`). Its clock is the car's
lap time on the stream's ticks: the tick it last crossed the line
(`FCarProgress::LapStartTick`) to the latest frame's, so it pauses and changes
speed with the stream. It hides within 3 m of the car. `BeginGhostReplay`
also runs in the watch view, with the TV director off until it ends. Console: `apexsim.watch
[start|stop|next|prev|car N|camera|auto|tower|overlay|race|pause|back|forward|faster|slower]`.
Unattended: `-ApexWatch` (the backdrop), `-ApexWatchSession` (the first race
on the server), `-ApexWatchReplay=<file|latest>` (with `-ApexWatchReplayAt=<s>`
seeked there once it has loaded: `UApexRootWidget::WatchReplayAt`), with `-ApexWatchCamera=`,
`-ApexWatchTower=`, `-ApexWatchCar=`, `-ApexWatchHideHud`.

## Photo mode

Over any race in the watch view (a replay, a live stream, a watched hotlap,
the backdrop, a session joined as a spectator): P, the pause menu's PHOTO
MODE (the pad's way in) or `apexsim.photo on`. `UApexRootWidget::BeginPhotoMode`
hides the HUD, pauses a replay or a live stream that can be held back (and
plays it again on the way out, if it was photo mode that paused it), and
stops the TV director choosing the car, so the car in the picture stays the
one. A showcase channel or a racer-telemetry spectator cannot be paused;
the cars run on under the camera.

The camera is the race director's shot camera (`BeginPhotoMode`: it starts
where the watch camera was and keeps the view through spawns, focus changes
and the watch keys until `EndPhotoMode`), flown every frame by
`UApexPhotoModeWidget` through `ApexPhoto::Advance`:

- **Free**: it flies where it looks (8 m/s, five times that with Shift or L3,
  a quarter with Ctrl), up and down in the world, turned by the mouse with a
  button held, the right stick or I/J/K/L.
- **Orbit** (O / Y): round the middle of the watched car's body, taken from
  where the camera is; the stick and the mouse turn it round, forward closes
  in (to 2.5 m), up and down raise and lower it.
- Never more than 150 m from the car, and kept 25 cm over the ground under
  it (props aside: `AApexRaceDirector::FindGroundBelow`, the TV camera's
  trace).

**The lens** (the card on the right; up and down / the D-pad pick a row,
left and right step it): focal length 12-600 mm on a 36 mm sensor (the field
of view and the depth of field's sensor width agree; - and = / LB and RB and
the wheel step it from any row), focus (on the car, measured each frame, or
1-300 m), aperture (no blur, then f/1.4 to f/22: the shot camera's cinematic
depth of field), exposure (thirds of a stop, -4 to +4 EV, on top of the
sky's own bias, which the camera's override replaces) and the shot's size
(1x, 2x, 4x the screen). The lens starts at the focal length of the view
it took over. `AApexRaceDirector::SetPhotoCamera` puts it on the shot
camera's post-process; leaving clears it.

**The shot** (Enter / A): the engine's high-resolution screenshot
(`FHighResScreenshotConfig`: the scene alone, without the HUD, the card or
the toast) at the multiple, halved until it fits the GPU's largest texture,
written as `Saved/Screenshots/ApexSim/ApexSim_<stem>_<yyyymmdd>_<hhmmss>_<n>x.png`
(a second one in the same second gets `_2`); a toast names the file and its
size. H / View hides the card and the hints, Space / X pauses or plays the
replay, R / R3 goes back to where photo mode began, Esc, Backspace, P, B or
Start leaves. Every key, stick and wheel turn goes to the photo layer from
the input processor (`FApexMenuInputProcessor`), so nothing takes focus and
nothing reaches the watch view under it.

Console, for unattended runs (`-ApexExecAfter`): `apexsim.photo
on|off|shot|orbit|hide|reset|pause|info`, `move <forward> <right> <up>
<seconds>` (as if the keys were held), `look <yaw> <pitch>` (degrees),
`focal <mm>`, `focus <m, 0 on the car>`, `aperture <f, 0 off>`, `ev
<stops>`, `res <1|2|4>`; each logs the camera and the lens. Recipes
`n20-photo-hotlap` and `n20-photo-replay` in `scripts/verify/runs.yml`.

## Replays

`UApexReplayRecorder` records every session the client is in (race,
practice, hotlap, a race watched) as a stream on the client: every other
telemetry frame (30 Hz) through `FApexStreamCarRow::FromTelemetry`, the
roster, lap timing, state and finishes, compressed a second at a time by
`FApexStreamWriter`, whose encoders are byte-identical to the server's. Time
with every car in a hotlap garage is cut.

When the session ends the file goes to `Saved/Replays/Recent/` (the newest
`MaxRecent`, 10, kept). SAVE REPLAY (pause menu, hotlap garage,
`apexsim.replay.Save`) keeps it in `Saved/Replays/`; the results screen's
Save replay keeps the whole session (`KeepThisSession`). The Replays screen
(`UApexReplaysWidget`) lists both: Enter watches, K keeps a recent one, E
(pad View) exports one to `<Documents>/ApexSim/Replays` under its
`MakeReplayName` (`UApexReplayRecorder::ExportReplay`), Delete
removes one. Playing is `UApexDemoModeSubsystem::PlayReplay` (backdrop source
`Replay`: no loop, never moved on from) with `UApexSpectatorSubsystem`'s
`SetPaused`, `SetPlaybackRate` and `SeekTo` (forward applies the timing on the
way; back replays the preamble first). A replay plays with the backdrop
switched off too. `apexsim-replay info` reads a client-saved file like a
rendered one.

The server records the same way: its `stream_recorder` writes every finished
race as an `.apxs` in `replays/` (see [sessions](../server/sessions.md)), so a
server's replay and a client's replay are one format and play in the same
Replays screen. `replay.rs`'s `.bin` is only the offline tools' format.
The dashboard's incident list gives each incident's second in the server's
replay, for `-ApexWatchReplayAt` or `apexsim.watch seek`
([sessions](../server/sessions.md#incidents)); the results screen's
incident list jumps into the client's own replay of the session
(`UApexReplayRecorder::ReplaySecondsOf`: the server tick less the file's
first tick and cut garage time, over its tick rate).

## Replay clips (`-ApexReplay=`)

`-ApexReplay=<file>.apxs` (or an old `.clip.json`) plays a clip frame-exact,
for filming: `UApexReplaySubsystem` is created only for such a run (no server,
no demo, no splash hold), `AApexRaceDirector::BeginReplayView` builds the
track by the clip's stem and spawns its roster, and every car is placed with
`AApexRaceCarActor::SetPlaybackPose` from `FApexReplayClip::SampleAt` at the
director's own game-time clock. The clip also keeps the stream's sky (`SkyAt`)
and road slices (`GetRoads`); `UpdateReplayWorld` hands them to the director
as the clock reaches them, and lays the road again from the start after a jump
back, so a clip films the rain arriving and the line drying. `-ApexReplayAudio` (with `-deterministicaudio`) also writes the
sound, rendered in step with the frames, as `<dir>/audio.wav`
([marketing site](../marketing-site.md)). `-ApexReplayRecord=<dir>` sets a fixed
timestep (`-ApexReplayFps`) and writes every frame as a PNG. Camera and
switch reference: the header of `ApexReplaySubsystem.h` and
[marketing site](../marketing-site.md).

## Checking it

- Rust: `showcase_list::tests` (the list read, names, an unknown variant or
  key refused), `server/ffi/tests/showcase.rs` (the API's file is the
  tool's, cancel, names), `spectator::tests` (round trips, the epoch patched in place, parts,
  file seek, newer writers' fields skipped, the score), `replay_tools::tests`
  (`a_seeded_render_is_byte_identical`, `check_stream`, windows, seed
  picking, convert, cut, poses), `showcase::tests` (one clock for two viewers,
  the loop's epoch, late joiners, divisor, rotate,
  `a_file_that_does_not_match_the_content_is_left_out`,
  `the_list_names_what_the_folder_lacks_on_tracks_here`),
  `network.rs` `test_showcase_wire_format`, `tests/all/showcase_test.rs` (two
  in-process clients on one channel, the implied leave, the HTTP side),
  `tests/all/live_spectator_test.rs` (a mid-race spectator gets the roster).
- Client: `ApexSim.Spectator.Codec`, `.File` (the whole file through a
  player), `.Player`, `.Protocol`, `.Writer`, `.PitFlags`;
  `ApexSim.Backdrop.ChooseFile`, `.ShowcasesToRender`,
  `.ShowcaseRenderMatchesBuild`; `ApexSim.Replay.Clip.Stream`;
  `ApexSim.Spectate.*`; `ApexSim.Tv.LockTarget`; `ApexSim.Photo.*` (the
  lens and its steps, the free and orbit camera, the keys, the shot's size
  and name).

## Traps

- Every viewer-facing record must keep the epoch as a full `uint 32` at bytes
  3..7, or showcase looping (which patches bytes, never decodes) breaks.
- A record that supersedes earlier ones (`Road`, `State`) must be safe to
  catch up from its latest copy, and a road slice is replaced only by one of
  the same cells (`catch_up_bodies` keys on first cell and length).
- Only append fields to records and the car row; bump `ROW_SIZE`, never
  reorder. A reader must keep skipping trailing bytes it does not know.
- A test fixture whose roster revision differs from its frames' has every
  frame dropped silently; play a whole file through the player in tests
  rather than decoding records alone.
- A spectator joining mid-race needs the roster again: the client drops every
  telemetry frame it cannot place on a roster.
- `-ApexReplay` places cars by the director's game clock, not the motion
  buffer: the buffer's arrival clock is the platform's and drifts under a
  fixed timestep.
- A render is deterministic only while the sim is (seeded session and host
  ids, no wall clock); see [architecture](../architecture.md).
