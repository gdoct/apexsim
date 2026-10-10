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

- `server/src/spectator.rs`: records, the car row, the file (blocks, index),
  `BroadcastEncoder` (session state to records), `patch_epoch`, `RaceScore`.
- `server/src/replay_tools.rs` and `server/src/bin/apexsim_replay.rs`: the
  `apexsim-replay` tool (`simulate`, `info`, `find`, `pose`, `cut`, `render`,
  `convert`, `guide`); `server/src/replay.rs`: the older `.bin` replay format.
- `server/src/showcase.rs` (`ShowcaseState`, channels),
  `server/src/game_loop/showcase.rs` (messages), `server/src/health.rs`
  (`/showcase`).
- Client codec, pure: `ApexSimNet` `ApexSpectatorStream.h`
  (`FApexStreamFile`, `FApexSpectatorPlayer`), `ApexSpectatorWriter.h`
  (`FApexStreamWriter`, `FApexStreamCarRow::FromTelemetry`).
- Client playback: `UApexSpectatorSubsystem`, `UApexDemoModeSubsystem`
  (backdrop source), `UApexNetSubsystem` (backdrop feed),
  `UApexReplayRecorder`, `UI/ApexReplaysWidget`, `Race/ApexSpectatorView.h`
  (`ApexSpectate`), `UApexReplaySubsystem` (`-ApexReplay=` clips).
- `content/showcase.yml`, `scripts/lib/ApexShowcase.ps1`.

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
| 5 | `Block` | file only | a second of records, zlib |
| 6 | `Index` | file only | first tick and offset of every block |
| 7 | `Path` | TCP, with the header | the centerline every 10 m, for the TV cameras of a viewer with no lobby |

**Epoch.** The second element of every viewer-facing record is the epoch,
always written as a full `uint 32`, so it sits at bytes 3..7 of the body. A
showcase that loops stamps a new epoch into the file's bytes
(`spectator::patch_epoch`) and forwards them undecoded; a `Frame`'s and an
`Event`'s tick is written the same way right after it. The roster revision
counts roster changes within an epoch. The client drops a frame whose epoch or
revision is not the one it holds (`FApexSpectatorPlayer`).

**Frames are self-contained.** No frame refers to another, so a lost datagram
costs one frame and a viewer can join or a file seek anywhere. A field too
large for one datagram (more than `MAX_ROWS_PER_PART`, 26 cars) is sent in
parts of the same tick; the client applies whatever parts arrived once a newer
tick comes.

**The car row** (`ROW_SIZE` 52 bytes, little-endian; 44 in a version 1 file,
`ROW_SIZE_V1`, which still plays with the tyres unknown): status bits, pose
in mm and angles, speed, steering, pedals, gear, rpm, lap, station, finish
position, the lap / pit / ERS flag bytes as in `CompactCarState`, compound,
the five damage percentages, tyre wear and tread temperature. That is all a
spectator draws; pressures, brake temperatures, fuel and feedback are left
out, and the client sets them to "unknown" (-1). Layout: `spectator.rs`.

**Events**: `LapTiming` (the `LapTiming` message's own fields),
`TrackSectors`, `SessionState`, `Finish`, `Retired`, `PitStop` (entered,
serviced, left), `Contact` (a car started colliding: the TV director's
incident cue). `BroadcastEncoder::observe` raises them from one tick's state
to the next.

**The file** is `"APXS"` + u16 version, then a plain preamble (`Header`,
`Roster`, `Path`, `TrackSectors` event: what `info` and the client's catalog
scan read), zlib `Block`s of a second each, a plain `Index`, and a u64 offset
of the index as the trailer. A live stream is the same records without
blocks and index. A grid start plus two laps of 16 GT3s is a few MB.

**Golden bytes**: `cargo test spectator_wire_format showcase_wire_format --
--nocapture` prints them; the client pins them in `ApexSpectatorGoldenBlobs.h`
(and a `File` copy in `BackdropTests.cpp`). See [protocol](../server/protocol.md)
for the workflow.

## apexsim-replay

A server binary (run from the repo root: content paths are relative); every
reporting command prints JSON on stdout.

```bash
apexsim-replay render --track content/tracks/default/Zandvoort/Zandvoort.yaml --class GT3 \
    --cars 20 --laps 2 --weather sunny --time 13:00 --seed 7 [--seeds 10] \
    --cars-dir content/cars/default --out build/showcase/Zandvoort.gt3.day.apxs
apexsim-replay info  build/showcase/Zandvoort.gt3.day.apxs [--check]
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
  `--car` picks the host car instead. `--cars-dir content/cars/default` keeps
  the player's own cars out. The sky takes every `SessionConditions` field
  (`--weather`, `--time`, `--air`, `--wind`, `--wind-from`); the AI drove it,
  so it travels resolved in the `Header`. `--seeds N --pick best` renders N
  seeds and keeps the best `RaceScore` (close racing for; contact, off-road
  time and retirements against). Recorded at `--rate` (30). The same
  arguments write a byte-identical file (the session and host ids are seeded
  too).
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
`day`, `dusk`, `rain`, `night`) and rendered into `build/showcase/`
(gitignored) from `content/showcase.yml`. `build_track_levels.ps1` and
`initialize_content.ps1` have a showcase stage (`-SkipShowcase`) that renders
what is missing or stale by `info --check`. Packages copy them to
`Game/Showcase/`; the release also to `Server/showcase/` with
`apexsim-replay.exe` beside the server. Imported tracks are never rendered by
the pipeline. Rendering takes seconds per circuit in a release build.

## Server showcases

```toml
[showcase]
enabled = true
dir = "./showcase"        # absent or missing: no showcases
playlist = ["*"]          # file names without .apxs, channel order; "*" every file not named
mode = "loop"             # or "rotate": a channel moves on to the next file at its end
stream_divisor = 1        # send every Nth frame
```

`APEXSIM_SHOWCASE_ENABLED` / `APEXSIM_SHOWCASE_DIR` override. At startup the
server reads each file's preamble and drops (with a log line) one whose track
or cars it lacks or whose checksums disagree with its content.

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
| C to S | `LeaveSpectate`; also implied by `CreateSession`, `JoinSession`, `JoinAsSpectator` and a disconnect |
| S to C | `LobbyState.ShowcaseAvailable` tells a client to ask |

The health port serves `/showcase` (JSON) and the metrics
`apexsim_showcase_viewers{showcase=...}` and `apexsim_showcase_frames_sent`
(see [operations](../server/operations.md)). `SpectatorKind::Live` is
reserved on the wire and never sent.

## The client player and the menu backdrop

`UApexSpectatorSubsystem` plays records through `FApexSpectatorPlayer` from
one of two sources:

- **File**: the `.apxs` is read and inflated on the thread pool, then records
  are released as the game clock (delta time x the file's tick rate) reaches
  their tick. At the end the preamble is applied again, which the player takes
  as a new epoch: the cars jump back to the grid cleanly.
- **Net**: `UApexNetSubsystem` routes `SpectatorJoined`, `SpectatorRecord`
  and frame datagrams (`PopSpectatorRecord`) to `OnSpectatorRecords`.

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
2. A local file from `Showcase/` beside `ApexSim.exe` (the repo's
   `build/showcase` in the editor; `-ApexShowcaseDir=` adds folders), the
   pending track's when one matches (`ChooseFile`). Files are scanned at
   startup by preamble (`FApexStreamFile::ReadPreamble`) so the splash hold's
   `IsDemoExpected` can count on one offline. A file whose track or car
   checksums do not match this machine's catalog rows is skipped (0 is
   unknown and passes).
3. A `SessionKind::Demo` session, for a server that predates showcases.
4. The static page backgrounds.

A stream's sky is the file's (the AI raced in it); the random sky roll applies
only to a demo session. `-ApexShowcase=<file|id>`, `-ApexNoShowcase`,
`apexsim.spectate.Info` (source, epoch, rates, frames applied / dropped),
`apexsim.spectate.Next`, `apexsim.demo.Restart`, `apexsim.demo.MaxMinutes`.
Cameras and the demo session are in [cameras](cameras.md).

## Watching a race

Any race the director has on screen can be watched full screen
(`UApexRootWidget::WatchBackdrop`, `WatchSession`, `WatchReplay`):

- **Main menu > Watch a race**: the backdrop, kept across its next races.
- **Session browser > Watch**: a live session through `JoinAsSpectator`. The
  server answers `SessionJoined` with grid position 0 and the session's
  `TrackSectors`, and marks the roster to be sent again
  (`GameSession::mark_roster_dirty`); the spectator gets the full racer
  telemetry. `UApexNetSubsystem::IsSessionSpectator` is true and the race view
  opens with no car.
- **Main menu > Replays**: a saved replay, below.
- A watched hotlap: see [sessions](../server/sessions.md).

The race director holds the watched car (`FocusCar`, `StepFocus`,
`FocusPosition`), the camera (TV locked on the car, chase, onboard with the
cockpit rig; `SetSpectatorCamera`) and whether the TV director picks the car
(`SetSpectatorAuto`), under `SetSpectating`. `ApexSpectate` is the pure part
(order, stepping, the key map). Keys reach `UApexRootWidget::HandleWatchKey`
through the input processor, so none reaches the hidden menu or a car:

| Keyboard | Pad | |
|---|---|---|
| Up, Left / Down, Right | D-pad, LB / RB | the car ahead / behind |
| 1-9, 0 | | P1-P9, P10 |
| C | Y | camera: TV, chase, onboard |
| A | X | the TV director picks the car |
| T | View | timing tower column |
| H | R3 | hide the overlay |
| N | L3 | the next race (the backdrop) |
| Space | A | pause a replay |
| comma / full stop | LT / RT | ten seconds back / on in a replay |
| - / = | | slower / faster (0.25x to 4x) |
| Backspace | B | stop watching |
| Esc | Start | the pause menu |

The HUD follows the watched car and shows the race's own circuit and
distance; the `spectate.*` and `replay.*` data points and the `spectator_*`
components are in [HUD modding](hud-modding.md). Console: `apexsim.watch
[start|stop|next|prev|car N|camera|auto|tower|overlay|race|pause|back|forward|faster|slower]`.
Unattended: `-ApexWatch` (the backdrop), `-ApexWatchSession` (the first race
on the server), `-ApexWatchReplay=<file|latest>`, with `-ApexWatchCamera=`,
`-ApexWatchTower=`, `-ApexWatchCar=`, `-ApexWatchHideHud`.

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
(`UApexReplaysWidget`) lists both: Enter watches, K keeps a recent one, Delete
removes one. Playing is `UApexDemoModeSubsystem::PlayReplay` (backdrop source
`Replay`: no loop, never moved on from) with `UApexSpectatorSubsystem`'s
`SetPaused`, `SetPlaybackRate` and `SeekTo` (forward applies the timing on the
way; back replays the preamble first). A replay plays with the backdrop
switched off too. `apexsim-replay info` reads a client-saved file like a
rendered one.

The server's own recorder (`replay::ReplayRecorder`) is separate: it writes
`.bin` replays of live sessions (format v3, frames streamed compressed to
disk while the race runs); see [sessions](../server/sessions.md).

## Replay clips (`-ApexReplay=`)

`-ApexReplay=<file>.apxs` (or an old `.clip.json`) plays a clip frame-exact,
for filming: `UApexReplaySubsystem` is created only for such a run (no server,
no demo, no splash hold), `AApexRaceDirector::BeginReplayView` builds the
track by the clip's stem and spawns its roster, and every car is placed with
`AApexRaceCarActor::SetPlaybackPose` from `FApexReplayClip::SampleAt` at the
director's own game-time clock. `-ApexReplayRecord=<dir>` sets a fixed
timestep (`-ApexReplayFps`) and writes every frame as a PNG. Camera and
switch reference: the header of `ApexReplaySubsystem.h` and
[marketing site](../marketing-site.md).

## Checking it

- Rust: `spectator::tests` (round trips, the epoch patched in place, parts,
  file seek, newer writers' fields skipped, the score), `replay_tools::tests`
  (`a_seeded_render_is_byte_identical`, `check_stream`, windows, seed
  picking, convert, cut, poses), `showcase::tests` (one clock for two viewers,
  the loop's epoch, late joiners, divisor, rotate,
  `a_file_that_does_not_match_the_content_is_left_out`),
  `network.rs` `test_showcase_wire_format`, `tests/showcase_test.rs` (two
  in-process clients on one channel, the implied leave, the HTTP side),
  `tests/live_spectator_test.rs` (a mid-race spectator gets the roster).
- Client: `ApexSim.Spectator.Codec`, `.File` (the whole file through a
  player), `.Player`, `.Protocol`, `.Writer`, `.PitFlags`;
  `ApexSim.Backdrop.ChooseFile`; `ApexSim.Replay.Clip.Stream`;
  `ApexSim.Spectate.*`; `ApexSim.Tv.LockTarget`.

## Traps

- Every viewer-facing record must keep the epoch as a full `uint 32` at bytes
  3..7, or showcase looping (which patches bytes, never decodes) breaks.
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
