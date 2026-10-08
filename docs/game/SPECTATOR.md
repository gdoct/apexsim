# Spectator stream, showcase races and the menu backdrop

*Spec 2026-10-02; implemented the same day. Status: steps 1–7 of the
implementation order are in (format, render tool, client player, backdrop,
pipeline, server showcase, `.apxs` replays). 2026-10-03: the watch view
(any race on screen, full screen with the spectator's controls, section 5),
live sessions watched from the browser (with the racer telemetry; a live
session encoded as this stream is still to come) and replays recorded and
saved by the client (section 6).*

## Why

The menu plays an AI race behind its screens. That race used to be a live
`SessionKind::Demo` session on the server: every connected client in the menu
got its own 20-car simulation at 240 Hz and its own full telemetry stream
(`CompactCarState`, 37 fields, at the broadcast rate). One client is fine;
a hundred clients in the menu are a hundred races nobody drives.

The fix is to stop *simulating* the backdrop and start *playing it back*, in a
format that is also what a spectator of a live race will receive later:

1. **A spectator stream format** (`.apxs`): a sequence of self-contained
   records that can be written to a file or sent over the wire unchanged.
2. **A render tool** that runs a headless AI race for a given track, car
   class, field, weather and time of day and writes it as such a file.
3. **A server showcase endpoint** that plays a selected file in a loop to any
   number of viewers. Playback costs a file read and a fan-out, no physics;
   the frames in the file are already encoded and are forwarded as they are.
4. **A client spectator player** that renders a stream, from a local file
   or from the server, in place of the full race telemetry, through the car,
   camera, audio and HUD paths a live race already uses.

Later, spectating a *live* race is the same stream with the server's live
session as the source instead of a file.

## What exists and how it is reused

| Piece | Where | Reused how |
|---|---|---|
| Headless seeded AI race, bit-identical per seed | `replay_tools::start_race` / `simulate_race`, `apexsim-replay simulate` | `render` ticks the same `GameSession` |
| Replay file (named `Telemetry` per frame, MessagePack, ~200 MB a race) | `replay.rs` v2 | Kept for `find` / `pose`; `convert` and `cut` turn it into `.apxs` |
| Clip file (`.clip.json`) | `replay_tools::ClipFile`, `FApexReplayClip::LoadFromString` | Superseded: `cut` writes `.apxs` when the output ends in it; the JSON reader stays for old files |
| Clip playback on the client (`-ApexReplay=`) | `UApexReplaySubsystem`, `AApexRaceDirector::BeginReplayView`, `SetPlaybackPose` | Unchanged; `FApexReplayClip::LoadFromStream` fills the same table from an `.apxs` |
| Demo session | `SessionKind::Demo`, `UApexDemoModeSubsystem` | Third choice, for a server that predates showcases; see "The menu backdrop" |
| Spectators of a session | `JoinAsSpectator`, `lobby.rs`, `broadcast.rs` | Still receive the full racer telemetry (live spectating is the open item) |
| Car motion buffer (tick-clocked playhead, blending) | `Race/ApexCarMotion.h` | Fed by the backdrop feed for both sources, as a live session feeds it |
| Engine sound (RPM, throttle, gear) | `ApexEngineSynth`, `AApexRaceCarActor` | Needs only those three per frame; the row carries them |
| TV director | `ApexTv::FDirector` | The camera for the backdrop; its path comes from the stream |

Audio needs nothing new: the synth derives pops, limiter, turbo, blow-off and
gear whine from RPM, throttle and gear plus the catalog row. Tyre, kerb and
road sound come from `DriverFeedback` for the local car only, and a spectator
has no local car.

## 1. The stream format (`server/src/spectator.rs`, `ApexSpectatorStream.h`)

### Records

A stream is a sequence of records, framed like the TCP protocol:

```
[u32 big-endian length][MessagePack body]
```

The body is a positional MessagePack array. Its first element is the record
type (u8). Named maps are not used: the client's hand-written codec reads
positional arrays, and golden bytes pin every record (`cargo test
spectator_wire_format -- --nocapture` prints them; `ApexSpectatorGoldenBlobs.h`
holds them, and `ApexSim.Spectator.*` decode them), as for the rest of the
protocol.

**The epoch is the second element of every record a viewer receives, written
as a full `uint 32` (`0xCE` + 4 bytes)**, so it sits at bytes 3..7 of every
such body and a showcase channel can stamp it into the bytes read from a file
(`spectator::patch_epoch`) without decoding anything. A `Frame`'s and an
`Event`'s tick is written the same way right after it (bytes 8..12), so the
server finds a record's tick without decoding it either. A reader accepts any
integer width, as MessagePack allows.

| Type | Record | Live transport | Body |
|---|---|---|---|
| 1 | `Header` | TCP, on join and on loop | `[1, epoch, version, stream_id, tick_rate, frame_rate, row_size, track[track_id, stem, display_name, source_crc, length_m], conditions[weather, time_of_day_minutes, air_temp_c, humidity_pct, wind_kph, wind_from_deg], session_kind, game_mode, lap_limit, ticks[race_start_tick, start_tick, end_tick], render[seed, score]]` (nil where a figure is unknown) |
| 2 | `Roster` | TCP, on change | `[2, epoch, revision, [[car_index, car_config_id, content_crc, livery, name, is_ai], ...]]` |
| 3 | `Frame` | UDP | `[3, epoch, tick, revision, state, countdown_ms (0xFFFF none), part, parts, bin rows]` |
| 4 | `Event` | TCP | `[4, epoch, tick, kind, payload]` |
| 5 | `Block` | file only | `[5, first_tick, last_tick, records, raw_len, bin zlib]` |
| 6 | `Index` | file only | `[6, [[first_tick, offset, records], ...]]` |
| 7 | `Path` | TCP, with the header | `[7, epoch, spacing_m, bin points]`: the centerline every 10 m as `i32` mm pairs, where the broadcast cameras stand, so a viewer with no lobby (a file played offline) has one |

Every array is read by position with anything a later writer appends
skipped (`a_newer_writers_extra_fields_are_skipped`, `ApexSim.Spectator.Codec`).

**Epoch** counts restarts of the stream: a showcase that loops takes a fresh
one and sends a fresh `Header`, so a client restarts its cars cleanly
instead of seeing every car jump back to the grid. On the server the epoch
comes from one counter for all channels (from 1; a file holds 0), so a
frame's epoch alone tells a stale datagram of any other channel or loop from
the stream being watched. **Roster revision** counts roster changes within an
epoch. A `Frame` whose epoch or revision does not match what the client holds
is dropped (`FApexSpectatorPlayer`), the rule the client already applied to
stale demo frames.

### Frames are self-contained

No frame refers to another. A lost UDP datagram costs one frame, a viewer can
join at any frame, and a file seeks by index. Delta encoding against a
keyframe is a possible later mode (a `Header` flag), not part of v1.

A frame carries every car. If the rows would not fit one datagram (more than
26 cars at the 52-byte row under a 1 400-byte budget, `MAX_ROWS_PER_PART`)
the frame is sent in `parts`, each a self-contained subset of rows with the
same tick; the client puts the parts of a tick together and applies whatever
arrived once a newer tick comes (`ApexSim.Spectator.Player`).

### The car row (52 bytes, little-endian; version 1 was the first 44)

Packed as a MessagePack `bin` of `row_size x cars` bytes. `row_size` is in the
`Header`; a reader takes the fields it knows and skips the rest of each row,
so fields are only ever **appended** (the rule `CompactCarState` already
follows).

| Offset | Type | Field | Unit / note |
|---|---|---|---|
| 0 | u8 | car index | roster index |
| 1 | u8 | status | bit 0 on track, 1 colliding, 2 in garage, 3 retired, 4 finished |
| 2 | i32 | x | mm, server frame |
| 6 | i32 | y | mm |
| 10 | i32 | z | mm |
| 14 | u16 | yaw | 2π/65536 rad, counter-clockwise from +X |
| 16 | i16 | pitch | π/32768 rad |
| 18 | i16 | roll | π/32768 rad |
| 20 | u16 | speed | cm/s |
| 22 | i8 | steering | /127, positive left |
| 23 | u8 | throttle | /255 |
| 24 | u8 | brake | /255 |
| 25 | i8 | gear | -1 reverse, 0 neutral |
| 26 | u16 | engine rpm | rpm |
| 28 | u16 | lap | |
| 30 | u32 | station | cm along the centerline |
| 34 | u8 | finish position | 0 none |
| 35 | u8 | lap flags | as `CompactCarState.lap_flags` (struck, garage, DRS allowed/open, headlights, flash) |
| 36 | u8 | pit flags | as `CompactCarState.pit_flags` |
| 37 | u8 | compound | 255 unknown |
| 38 | u8 x 5 | damage | percent: front, rear, left, right, engine |
| 43 | u8 | ERS flags | as `CompactCarState.ers_flags` |
| 44 | u8 x 4 | tyre wear | percent, FL FR RL RR; 255 unknown (appended 2026-10-03) |
| 48 | u8 x 4 | tread temperature | °C as `CompactCarState.tyre_c`; 0 unknown |

A stream whose header says 44 (rendered before the tyres) still plays, its
tyres unknown; `apexsim-replay info --check` calls such a file stale, so the
pipeline renders it again.

That is what a spectator draws: pose, wheels (steering, speed), brake and
head lights, DRS flap, visible damage and debris, engine sound, standings
(lap, station, finish), the pit state, the tyre on the car with its wear and
tread temperature. Pressures, brake temperatures, fuel, feedback and setup
stay in the racer's telemetry; the
client's `FApexCarTelemetry` has them as "unknown" (`-1`), which is what the
HUD data and the car actor already handle for an older server.

Size: 20 cars are 1 040 bytes of rows plus 21 bytes of frame header (about
1 060 bytes a datagram, `a_big_field_is_sent_in_parts`). At 30 Hz that is
~32 KB/s per viewer; 100 viewers ~25 Mbit/s, 1 000 ~250 Mbit/s.

### Events

Reliable, timestamped by tick, carrying the existing payloads where one
exists:

| Kind | Payload |
|---|---|
| 1 `LapTiming` | `[car_index, lap, sector, sector_time_ms, lap_time_ms, is_lap_end, valid, flags]`: the `LapTiming` message's own fields |
| 2 `TrackSectors` | `[track_length_m, [boundaries_m...]]`, once after the `Header` |
| 3 `SessionState` | `[state]` (lobby / countdown / racing / finished) |
| 4 `Finish` | `[car_index, position]` |
| 5 `Retired` | `[car_index, reason]` (1: damage) |
| 6 `PitStop` | `[car_index, phase]` (0 entered the lane, 1 serviced, 2 left) |
| 7 `Contact` | `[car_index, other, speed_cms]`: a car started colliding; `other` is 255 (the sim does not say what it hit) and the speed is the car's own at the moment (the TV director's incident cue) |

`BroadcastEncoder::observe` raises them from one tick's state to the next:
the session's lap events, a finish position appearing, `damage.is_drivable`
dropping, the pit lane's `in_lane` / `servicing` edges, `is_colliding`
rising. A file stores every event; a viewer joining a live channel gets the
`Header`, `Roster`, `Path`, `TrackSectors` and every `LapTiming` so far, as
one TCP message, so the timing board starts filled.

### The file (`.apxs`)

```
"APXS" magic, u16 file version (big-endian)
Header, Roster, Path, TrackSectors event      (plain, so `info` and the client's catalog read the first kilobytes)
Block, Block, ...                             (zlib level 6, one second of ticks of Frame / Event / Roster records each)
Index                                         (plain)
u64 big-endian offset of the Index record     (trailer)
```

A live stream is the same records without `Block` and `Index`. Writing a
file is compressing the live sequence; playing a file is decompressing it
back. Measured: a grid start plus two laps at Zandvoort with 16 GT3s at
30 Hz is 260 s, 7 786 frames, 5.7 MB of records, **2.8 MB on disk**, rendered
in 6 s in a release build.

Determinism: the same render arguments and seed write a byte-identical file
(no timestamps, no hash-map order; the session id and host id are seeded
too), pinned by `a_seeded_render_is_byte_identical`. Every frame is a
keyframe in v1, so the index holds no keyframe list.

## 2. The render tool (`apexsim-replay`)

A subcommand of the existing server bin, so it simulates with the server's
own `GameSession`, physics and AI:

```
apexsim-replay render --track content/tracks/default/Zandvoort/Zandvoort.yaml \
    --class GT3 | --car posh-gt3rs   --cars 20   --laps 2 \
    --weather sunny --time 13:00 [--air 22] [--wind 15 --wind-from 90] \
    --seed 7 | --seeds 10 --pick best \
    --rate 30 --cars-dir content/cars/default --out build/showcase/Zandvoort.gt3.day.apxs
apexsim-replay info  build/showcase/Zandvoort.gt3.day.apxs [--check]   # header, roster, CRCs, stats
apexsim-replay convert out/promo/races/monza_rain.bin --rate 30 --out monza_rain.apxs
apexsim-replay cut out/zandvoort.bin --from-s 330 --to-s 345 --out out/luyendyk.apxs
```

Run from the repo root (content paths are relative). `render` prints the
seed, the score and `info`'s JSON; `info --check` prints the stream and a
`stale` list.

- **Content.** The race starts on the grid with the countdown (`--countdown`,
  8 s: the start is the best part of a backdrop) and runs `--laps` from
  green, then `--tail` (10 s) past the first car to take the flag, so the
  leader is seen crossing it without the field trailing in for a minute.
  `--from-tick` / `--to-tick` keep a window instead. `--max-seconds` (1800)
  is the stop when nobody finishes.
- **Field.** `--class` deals the class's cars as `game_session::class_field`
  does, with the class's first car by folder name as the host; `--car` picks
  the host car and the field follows its class. Liveries are dealt per model
  as in a live session. `--cars-dir` restricts the cars (the promo pipeline's
  rule: `content/cars/default`, no AC imports). A circuit's grid may hold
  fewer than `--cars` (Zandvoort seats 16).
- **Sky.** Every `SessionConditions` field the create screen offers. The AI
  drove those conditions (wet grip is baked into the session's track), so the
  sky travels in the `Header`, resolved (air, humidity, wind named in full),
  and the client lights the scene from it.
- **Seed picking.** `--seeds N` renders N seeds from `--seed` up and keeps the
  one with the best `RaceScore::score`: car-seconds spent within a second of
  the car ahead on the road count for it; contact (x2) and off-road
  car-seconds and retirements (60 each) count against. The chosen seed and
  its score are in the `Header`, so a rebake is reproducible.
- **Rate.** Recorded from the 240 Hz sim at `--rate` (default 30).
- **Freshness.** The `Header` carries the track's `source_crc` and every car's
  `content_crc`. `info --check` exits non-zero when the content on disk no
  longer matches (`check_stream`), which is how the pipeline decides to
  rebake; a checksum of 0 (a converted replay) is not compared.
- **`convert`** turns a replay (`.bin`) into a stream at `--rate`. A replay
  holds no pit state, compound, damage or hybrid, so those rows are blank,
  and its only events are state changes and finishes; the track YAML (found
  by the stem, or `--track`) fills in the checksum, the path and the
  sectors.

### Naming and where files live

`<Stem>.<class>.<variant>.apxs`, variant a short name for the sky
(`day`, `dusk`, `rain`, `night`). Rendered into `build/showcase/`
(gitignored, like the track exports) from the list in `content/showcase.yml`
(track, class, variants, seed; `scripts/lib/ApexShowcase.ps1` reads it):

- `scripts/build_track_levels.ps1` has a showcase stage after the export
  (`-SkipShowcase`), rendering only what is missing or stale by `info
  --check`, honouring `-Track` and `-DryRun`.
- `scripts/initialize_content.ps1` runs the same stage.
- `build_game_standalone.ps1` / `build_release.ps1` copy the files to
  `Game/Showcase/` (the client's local backdrop) and the release to
  `Server/showcase/` (the server's playlist), and ship `apexsim-replay.exe`
  in `Server/` so a modder can render a custom track or class with the same
  command.
- Imported tracks (`imported` marker) are never rendered by the pipeline,
  like their exports; render one by hand.

## 3. The server showcase endpoint (`server/src/showcase.rs`, `game_loop/showcase.rs`)

### Configuration

```toml
[showcase]
enabled = true
dir = "./showcase"                   # relative to the server's working dir; absent = no showcases
playlist = ["*"]                     # file names without .apxs, in channel order; "*" is every file not named
mode = "loop"                        # "loop": one file per channel, forever
                                     # "rotate": each channel moves on to the next file of the playlist at its end
stream_divisor = 1                   # send every Nth frame of the file
```

`APEXSIM_SHOWCASE_DIR` and `APEXSIM_SHOWCASE_ENABLED` override. At startup
the server reads every file's `Header` and `Roster`, drops a file whose
track or cars it does not have or whose CRCs disagree with its content
(logged, with "render it again"), and lists the rest
(`a_file_that_does_not_match_the_content_is_left_out`).

### Messages

| Direction | Message | Notes |
|---|---|---|
| C→S | `ListShowcases` | |
| S→C | `Showcases { Entries }` | per channel: `Id`, `TrackId`, `TrackName`, `Class`, `Conditions`, `DurationS`, `Cars`, `Viewers` |
| C→S | `SpectateShowcase { id: Option }` | none = the first channel; `Error 404` for an unknown one, `400` from inside a session |
| S→C | `SpectatorJoined { StreamId, Kind: Showcase, ShowcaseId }`, then one `SpectatorRecord` holding `Header`, `Roster`, `Path`, `TrackSectors` and the `LapTiming` so far | TCP |
| S→C | `Frame` records | UDP, each datagram one record body (no length, no envelope) once the existing `UdpHandshake` is done; TCP (droppable) before |
| S→C | `SpectatorRecord` | TCP: `{"type": "SpectatorRecord", "data": bin}` with the records inside in the stream's own `[u32 length][body]` framing, so one message carries a run and the bytes read from the file are forwarded as they are |
| C→S | `LeaveSpectate` | also implied by `CreateSession`, `JoinSession`, `JoinAsSpectator` and a disconnect |

`LobbyState` gained `ShowcaseAvailable: bool` so a client knows to ask. The
HTTP side lists the channels at `/showcase` (JSON) and counts viewers in
`/metrics` (`apexsim_showcase_viewers{showcase=...}`,
`apexsim_showcase_frames_sent`). Golden bytes: `cargo test
showcase_wire_format -- --nocapture` → `ApexShowcaseGolden::*`.

### Channels

A **channel** is one file playing on one clock, shared by all its viewers:
viewers join mid-race, which is what a broadcast is. The game loop advances
each channel with viewers once per tick (`ShowcaseState::advance`, no thread
per channel), finds the records whose tick has come, and fans out the
**record bytes from the file as they are**, the epoch stamped in: a `Frame`
is self-contained, so nothing is decoded or encoded on the way. A channel
with no viewers stops and drops its inflated records; the first viewer
starts it from the beginning of the content, the file inflated on a
blocking task off the loop and installed when it is in. A newcomer (or
everyone, after a loop) gets the preamble and the timing so far as one TCP
message before any frame of the epoch.

At the end of the content the channel takes the next epoch and sends the
`Header` again (`loop`) or moves on to the next file, whose `Header` and
`Roster` follow (`rotate`). `tests/showcase_test.rs` streams a channel to
two in-process clients: shared clock, a late joiner's preamble, the loop's
epoch, the implied leave and the HTTP side.

Cost per viewer: one UDP send per frame of ~900 bytes. No physics, no AI, no
per-viewer serialization.

### Replacing the demo session

The client asks for a showcase instead of creating a `SessionKind::Demo`
session when the server lists one. `SessionKind::Demo` stays for one release
so an older client still gets a backdrop, then goes.

### Live spectating, as built

The session browser's **Watch** button (enabled while a session's race is
being driven) sends `JoinAsSpectator`; the server answers `SessionJoined`
with grid position 0 and the session's `TrackSectors`, and marks its roster
to go out again on the next tick (`GameSession::mark_roster_dirty`): a
spectator arriving mid-race had no roster, and the client drops every
telemetry frame it cannot place (`tests/live_spectator_test.rs` fails
without it). The lobby's `SessionSummary` now carries the live session's
`State` (it read `Lobby` forever: `Lobby::update_session` had no caller)
and its `LapLimit`. The spectator gets the full racer telemetry, which is
more than a stream frame carries (pressures, brakes, fuel). On the client
`UApexNetSubsystem::IsSessionSpectator` is true, the race view opens with
no car, and the watch view (section 5) takes the keys.

### Later: live spectating as a stream

`JoinAsSpectator` still sends the full racer telemetry. With the stream in
place a spectator gets `SpectatorJoined { Kind: Live }`, the session's
`Header`/`Roster`, and `Frame`s encoded from the live session once per
broadcast tick (`broadcast.rs` already serializes once per session). The
encoder (`spectator::BroadcastEncoder`: session → records) is the one the
render tool writes files with, and the live session can record to a file
through it too. **Not done**; `SpectatorKind::Live` exists on the wire.

## 4. The client spectator player

### One player, two sources

`UApexSpectatorSubsystem` (game instance, `ApexSim`) owns the playback; the
record codec lives in `ApexSimNet` beside the protocol (`ApexSpectatorStream.h`:
`FApexStreamFile`, `FApexSpectatorPlayer`, pure, unit-tested on the golden
bytes).

- **File source**: reads an `.apxs` whole and inflates it on the thread pool,
  then releases records as the game's clock (delta time x the file's tick
  rate) reaches their tick, so a fixed-timestep recording run stays in step.
  At the end it applies the preamble again, which the player takes as a new
  epoch; the cars jump back to the grid and the motion buffers restart.
- **Net source**: `UApexNetSubsystem` routes `SpectatorJoined` and every
  `SpectatorRecord` (TCP) and frame datagram (UDP, `PopSpectatorRecord`) to
  `OnSpectatorRecords`, which the subsystem applies as they come. Clock:
  arrival fitting, as the motion buffer does for live telemetry.

Either way the records go through `FApexSpectatorPlayer` and out as a
**backdrop feed** on the net subsystem (`BeginBackdropFeed`, `FeedBackdropRoster`,
`FeedBackdropTelemetry`, `FeedBackdropSectors`, `FeedBackdropLapTiming`,
`EndBackdropFeed`): `IsInDemoSession()` turns true and the roster, frames
and timing go out through the same delegates a live session's do, so the
race director, the TV director, the HUD data, the recorder and the engine
sound see exactly what a demo session showed them. That is the one design
change from the spec: the motion buffer needed no external clock, because
the file source releases frames in real time and the buffer's arrival
fitting handles it; `SetPlaybackPose` and `FApexReplayClip` stay for the
frame-exact `-ApexReplay` path, which now reads `.apxs`.

| Record | Becomes | Consumer, unchanged |
|---|---|---|
| `Header` | conditions (`BeginBackdropFeed`), track stem/id for the demo view | `AApexRaceDirector::BeginDemoView` builds the track, `ApexSky::Derive` lights it |
| `Path` | the demo view's centerline | the TV director's trackside cameras (the lobby's centerline is the fallback) |
| `Roster` | `FApexSessionRoster` (player ids `stream-<index>`) | the director spawns and dresses the cars (`Prefetch`, liveries) |
| `Frame` row | `FApexCarTelemetry` per car (fields the row lacks set to unknown) | each car's `FApexCarMotionBuffer`: pose, wheels, lights, DRS flap, damage, engine sound |
| `Event` | `LapTiming`, `TrackSectors` | `FApexTimingBoard` |

### The menu backdrop

`UApexDemoModeSubsystem` picks a source in this order (`EApexBackdropSource`):

1. Connected and the lobby says `ShowcaseAvailable`: `ListShowcases`, then
   `SpectateShowcase` on the channel of the pending track if there is one
   (and this machine has its export), else another, never the same one twice
   running. A connection under way, or a channel list asked for, is given
   three seconds (`ServerGraceSeconds`) before the next source is tried, so
   with a server the showcase wins the race against the local file; offline
   the connect fails in under a second and nothing waits.
2. Otherwise a local file from `Showcase/` beside `ApexSim.exe` (the repo's
   `build/showcase` in the editor; `-ApexShowcaseDir=` adds folders): the
   pending track's file when one matches, else any
   (`UApexDemoModeSubsystem::ChooseFile`, `ApexSim.Backdrop.ChooseFile`).
   The files are scanned once at startup (`FApexStreamFile::ReadPreamble`,
   the first kilobytes), so the splash hold (`IsDemoExpected`) can count on
   one offline.
3. Otherwise a `SessionKind::Demo` session, for a server that predates
   showcases.
4. Otherwise the static page backgrounds, as with `-ApexNoDemo`.

The rest of the backdrop is unchanged: the TV director's camera, the root
widget's fade and scrim, the world hidden behind car select and session
create, the demo's engine volume scale, and `apexsim.demo.*` (`MaxMinutes`
and a finished race move on to the next file or showcase). The random sky
roll only applies to a demo session: a stream's sky is the file's, because
the AI raced in it.

A local file whose track `source_crc` or car CRCs do not match this
machine's catalog rows is skipped and logged (cars would drive a road that
has moved); 0 on either side is unknown and passes. A net stream is the
server's responsibility (it drops mismatching files at startup).

### Console and command line

`-ApexShowcase=<file|id>` (a file on disk, else a channel id),
`-ApexNoShowcase` (demo sessions only), `apexsim.spectate.Info` (source,
epoch, rates, frames applied / dropped / incomplete, the file's clock),
`apexsim.spectate.Next` and `apexsim.demo.Restart` (move on).

## 5. Watching a race (the watch view)

Anything the race director has on screen can be watched full screen with a
spectator's controls (`UApexRootWidget::WatchBackdrop`, `WatchSession`,
`WatchReplay`):

- **Main menu > Watch a race**: the race playing behind the menu (a
  showcase, a local file, a demo session). It survives the backdrop moving
  on to its next race; between two the plain page covers the gap.
- **Session browser > Watch**: a live session, as above.
- **Main menu > Replays**: a saved replay (section 6).

The race director keeps the spectator's choice (`SetSpectating`; the rules
are `ApexSpectate` in `Race/ApexSpectatorView.h`, pure and tested by
`ApexSim.Spectate.*`): the watched car (`FocusCar`, `StepFocus` through the
race order, `FocusPosition`), the camera (`ECamera`: the TV director locked
on that car, the chase camera, the onboard view with the cockpit rig) and
whether the TV director picks the car (`SetSpectatorAuto`). Watched, the
backdrop's engines play at full volume and its start is heard.

The keys go through `FApexMenuInputProcessor` to
`UApexRootWidget::HandleWatchKey`, so none reaches the hidden menu or a car:

| Keyboard | Pad | |
|---|---|---|
| Up, Left / Down, Right | D-pad, LB / RB | the car ahead / behind |
| 1-9, 0 | | P1-P9, P10 |
| C | Y | camera: TV, chase, onboard |
| A | X | the TV director picks the car |
| T | View | timing tower column: interval, gap, last lap, best lap, tyres |
| H | R3 | hide the overlay |
| N | L3 | the next race (the backdrop) |
| Space | A | pause a replay (at its end: from the start) |
| comma / full stop | LT / RT | ten seconds back / on in a replay |
| - / = | | slower / faster (0.25x to 4x) |
| Backspace | B | stop watching |
| Esc | Start | the pause menu ("STOP WATCHING") |

`apexsim.watch [start|stop|next|prev|car N|camera|auto|tower|overlay|race|
pause|back|forward|faster|slower]` does the same from the console;
`-ApexWatch` (the backdrop), `-ApexWatchSession` (the first race on the
server), `-ApexWatchReplay=<file|latest>`, with `-ApexWatchCamera=`,
`-ApexWatchTower=`, `-ApexWatchCar=` and `-ApexWatchHideHud`, set it up for
an unattended run.

The HUD follows the watched car: `FApexHudInputs::LocalCarIndex` is the car
on screen, so every component shows its figures, and the HUD's circuit,
length and race distance are the race's own (a stream's header, the
session's summary) rather than the player's picks. The `spectate.*`,
`replay.*` and new `standings` fields (interval, laps down, compound, tyre
age and wear, pit stops, retired, car model) are in docs/HUD_MODDING.md;
the shipped `spectator_tower`, `spectator_driver`, `spectator_controls` and
`spectator_replay` components show only while watching, and `standings`,
`race_state`, `track_info`, `status` and `mirror` hide.

## 6. Replays

`UApexReplayRecorder` records every session the client is in (a race, a
practice, a hotlap, a live race watched) as this stream, on the client:
every other telemetry frame (30 Hz) through `FApexStreamCarRow::FromTelemetry`,
the roster and its changes, the lap timing, the session's state and the
finishes, compressed a second at a time (`FApexStreamWriter`,
`ApexSpectatorWriter.h`; the record encoders are byte-identical to the
server's, pinned by `ApexSim.Spectator.Writer` on the golden bytes). Time
with every car parked in a hotlap garage is cut. The server's tick rate is
measured from the frames' ticks against their arrival. A 12-car race is
about 8 KB a second on disk.

When the session ends the recording goes to `Saved/Replays/Recent/` (the
newest ten kept); **SAVE REPLAY** in the pause menu or the hotlap garage
(`apexsim.replay.Save`) writes it to `Saved/Replays/` for good. The
**Replays** screen (`UApexReplaysWidget`) lists both, newest first: Enter
watches one, K keeps a recent one, Delete removes one. Playing one is
`UApexDemoModeSubsystem::PlayReplay`: the backdrop source `Replay`, a file
that does not loop and is never moved on from, with
`UApexSpectatorSubsystem`'s transport (`SetPaused`, `SetPlaybackRate`,
`SeekTo`: forward applies the timing on the way and only the last frame;
back applies the preamble again first). A replay plays with the menu
backdrop turned off too. A file the client wrote reads in `apexsim-replay
info` like a rendered one, and the other way round.

## Implementation order (as built)

| # | Step | Where |
|---|---|---|
| 1 | Stream codec in Rust (`spectator.rs`: records, row, file blocks/index, encoder, scoring), golden bytes, determinism test | done |
| 2 | `apexsim-replay render` / `info [--check]` / `convert`, seed scoring; `cut` to `.apxs` | done |
| 3 | Client codec (`ApexSpectatorStream`) on the golden bytes; file source; player into roster / motion buffer / timing board via the backdrop feed | done |
| 4 | Backdrop from a local file; splash hold; the sky roll kept for demo sessions only | done |
| 5 | Pipeline: `content/showcase.yml`, `build_track_levels.ps1`, `initialize_content.ps1`, release copies, ship `apexsim-replay.exe` | done |
| 6 | Server `[showcase]`, channels, messages, `/showcase`, metrics; client net source; showcase preferred over the demo session | done |
| 7 | `-ApexReplay` and `cut` on `.apxs`, promo pipeline moved over; `.clip.json` still readable, no longer written | done |
| 8 | The watch view; live spectating from the browser (racer telemetry); the tyre fields in the row; client-recorded replays and the Replays screen | done |
| later | Live spectating through `BroadcastEncoder`; the server's own replays recorded as `.apxs`; `convert` recovering lap timing from a replay | |

## Tests

- Rust: `spectator::tests` (record and row round-trips, the epoch patched in
  place, parts, a file round trip with a seek, a newer writer's fields
  skipped, the score), `replay_tools::tests` (a seeded render written twice
  byte-identical, `check_stream` fresh and stale, a window, the best of two
  seeds, a replay converted), `showcase::tests` (one clock for two viewers,
  the loop's epoch, a late joiner's catch-up, the divisor, rotate, files
  left out), `network.rs` `test_showcase_wire_format`, `metrics.rs`,
  `tests/showcase_test.rs` (two in-process `TestClient`s on one channel).
- Client: `ApexSim.Spectator.Codec` (golden bytes, unknown trailing row
  bytes skipped), `.File` (block decode, seek by index, the whole file
  through a player), `.Player` (stale epoch and revision dropped, roster
  change mid-stream, parts, a framed run), `.Protocol` (the showcase
  messages, a frame datagram, `ShowcaseAvailable`),
  `ApexSim.Replay.Clip.Stream` (`-ApexReplay` on an `.apxs`),
  `ApexSim.Backdrop.ChooseFile` (source order, CRC mismatch skips a file).
- Seen running: the menu over a GT3 race at Zandvoort from a local file with
  no server at all, and from the server's channel over UDP with
  `apexsim_showcase_viewers` at 1 (2026-10-02).

## Open questions, answered

1. **Replays as `.apxs`.** Kept `replay.rs` for `find` / `pose`; `cut` and
   `convert` write `.apxs`. Revisit with live spectating.
2. **Showcase vs local file at startup.** Server showcase first when
   connected (the server owner picks what the menu shows), with a short
   grace for the connection, local file otherwise.
3. **Frame rate.** 30 Hz for showcase files; `cut` keeps the replay's own
   rate (60 for promo material); `stream_divisor` thins a channel.
4. **Variants per circuit.** One per circuit in `content/showcase.yml` keeps
   the build short; more variants are one line each.
