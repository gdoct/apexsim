# Spectator stream, showcase races and the menu backdrop

*Spec, 2026-10-02. Status: design, nothing implemented yet.*

## Why

The menu plays an AI race behind its screens. Today that race is a live
`SessionKind::Demo` session on the server: every connected client in the menu
gets its own 20-car simulation at 240 Hz and its own full telemetry stream
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

## What exists today

| Piece | Where | Reused how |
|---|---|---|
| Headless seeded AI race, bit-identical per seed | `replay_tools::simulate_race`, `apexsim-replay simulate` | The render tool's simulation, unchanged |
| Replay file (named `Telemetry` per frame, MessagePack, ~200 MB a race) | `replay.rs` v2 | Converted by the tool; may move to `.apxs` later (open question 1) |
| Clip file (`.clip.json`, 16 floats per car per frame, ~90 KB/s for 12 cars) | `replay_tools::ClipFile`, `FApexReplayClip` | Superseded by `.apxs`; `cut` writes `.apxs` once the client reads it |
| Clip playback on the client (`-ApexReplay=`) | `UApexReplaySubsystem`, `AApexRaceDirector::BeginReplayView`, `SetPlaybackPose` | Its director path becomes the player's file source |
| Demo session | `SessionKind::Demo`, `UApexDemoModeSubsystem` | Replaced by the showcase; kept one release for old clients |
| Spectators of a session | `JoinAsSpectator`, `lobby.rs` spectator counts, `broadcast.rs` | Receive the full racer telemetry today; move to the stream later |
| Car motion buffer (tick-clocked playhead, blending) | `Race/ApexCarMotion.h` | Fed by the player for both sources |
| Engine sound (RPM, throttle, gear) | `ApexEngineSynth`, `AApexRaceCarActor` | Needs only those three per frame; everything else is the car's `[sound]` table |
| TV director | `ApexTv::FDirector` | The camera for the backdrop and for spectating |

Audio needs nothing new: the synth derives pops, limiter, turbo, blow-off and
gear whine from RPM, throttle and gear plus the catalog row. Tyre, kerb and
road sound come from `DriverFeedback` for the local car only, and a spectator
has no local car. The race start launches properly because the physics has a
clutch-slip launch model (`physics::geared_rpm`).

## 1. The stream format

### Records

A stream is a sequence of records, framed like the TCP protocol:

```
[u32 big-endian length][MessagePack body]
```

The body is a positional MessagePack array whose first element is the record
type (u8). Named maps are not used: the client's hand-written codec already
reads positional arrays, and golden bytes pin every record (`network.rs`
tests print them, `ApexSpectatorGoldenBlobs.h` holds them), as for the rest of
the protocol.

| Type | Record | Live transport | Contents |
|---|---|---|---|
| 1 | `Header` | TCP, on join and on loop | format version, stream id, epoch, tick rate, frame rate, row size, track (`track_id`, stem, display name, `source_crc`, length), `SessionConditions` (resolved: weather, time, air, wind), session kind, game mode, lap limit, race start tick, start/end tick of the content |
| 2 | `Roster` | TCP, on change | epoch, roster revision, entries: car index, `car_config_id`, car `content_crc`, livery, display name, is AI |
| 3 | `Frame` | UDP | tick, epoch, roster revision, session state, countdown ms, part / parts, `bin` rows (below) |
| 4 | `Event` | TCP | tick, epoch, kind, payload (below) |
| 5 | `Block` | file only | zlib-compressed run of records |
| 6 | `Index` | file only | per block: first tick, byte offset; keyframe ticks |

**Epoch** counts restarts of the stream: a showcase that loops bumps it and
sends a fresh `Header`, so a client restarts its cars cleanly instead of
seeing every car jump back to the grid. **Roster revision** counts roster
changes within an epoch. A `Frame` whose epoch or revision does not match
what the client holds is dropped, the same rule the client applies today to
stale demo frames.

### Frames are self-contained

No frame refers to another. A lost UDP datagram costs one frame, a viewer can
join at any frame, and a file seeks by index. Delta encoding against a
keyframe is a possible later mode (a `Header` flag), not part of v1.

A frame carries every car. If the rows would not fit one datagram (more than
about 28 cars at the v1 row size under a 1 400-byte budget) the frame is sent
in `parts`, each a self-contained subset of rows with the same tick; the
client applies whatever parts arrive.

### The car row (v1, 44 bytes, little-endian)

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

That is what a spectator draws: pose, wheels (steering, speed), brake and
head lights, DRS flap, visible damage and debris, engine sound, standings
(lap, station, finish), the pit state and the tyre on the car. Tyre and brake
temperatures, fuel, feedback and setup stay in the racer's telemetry.

Size: 20 cars are 880 bytes of rows plus ~25 bytes of frame header. At 30 Hz
that is ~27 KB/s per viewer; 100 viewers ~22 Mbit/s, 1 000 ~220 Mbit/s.

### Events

Reliable, timestamped by tick, carrying the existing payloads where one
exists:

| Kind | Payload |
|---|---|
| `LapTiming` | the existing `LapTiming` body (sector and lap times, flags) |
| `TrackSectors` | the existing body, once after the `Header` |
| `SessionState` | lobby / countdown / racing / finished |
| `Finish` | car index, position |
| `Retired` | car index, reason |
| `PitStop` | car index, entered / serviced / left |
| `Contact` | car indices, closing speed (TV director's incident cue) |

A file stores every event; a viewer joining a live stream gets the session's
`TrackSectors` and the `LapTiming` history of the current lap so the timing
board starts filled.

### The file (`.apxs`)

```
"APXS" magic, u16 file version
Header                 (plain, so `info` and the client's catalog read it fast)
Roster                 (plain)
Block, Block, ...      (zlib, ~1 s of Frame / Event / Roster records each)
Index                  (plain)
u64 offset of Index    (trailer)
```

A live stream is the same records without `Block` and `Index`. Writing a
file is compressing the live sequence; playing a file is decompressing it
back. Expected size: a grid start plus two laps at a 1:40 circuit with 20 cars
at 30 Hz, ~5.5 MB of rows before zlib; a few MB on disk.

Determinism: the same render arguments and seed write a byte-identical file
(no timestamps, no hash-map order), checked by a test like
`tests/determinism_test.rs`.

## 2. The render tool

A subcommand of the existing server bin, so it simulates with the server's
own `GameSession`, physics and AI:

```
apexsim-replay render --track content/tracks/default/Zandvoort.yaml \
    --class GT3 | --car posh-gt3rs   --cars 20   --laps 2 \
    --weather sunny --time 13:00 [--air 22] [--wind 15 --wind-from 90] \
    --seed 7 | --seeds 10 --pick best \
    --rate 30 --out build/showcase/Zandvoort.gt3.day.apxs
apexsim-replay info  build/showcase/Zandvoort.gt3.day.apxs   # header, roster, CRCs, stats
apexsim-replay convert out/promo/races/monza_rain.bin --rate 30 --out monza_rain.apxs
```

- **Content.** The race starts on the grid with the countdown (the start is
  the best part of a backdrop) and runs `--laps` from green, then a short
  tail so the leader is seen taking the flag. `--from-tick` / `--to-tick`
  cut a window instead.
- **Field.** `--class` deals the class's cars as `game_session::class_field`
  does; `--car` picks the host car and the field follows its class. Liveries
  are dealt per model as in a live session. `--cars-dir` restricts the cars
  (the promo pipeline's rule: shipped cars only, no AC imports).
- **Sky.** Every `SessionConditions` field the create screen offers. The AI
  drove those conditions (wet grip is baked into the session's track), so the
  sky travels in the `Header` and the client lights the scene from it.
- **Seed picking.** `--seeds N --pick best` renders N seeds and keeps the one
  with the best score: no retirements, least contact and off-road car-seconds,
  most time with cars within a second of each other (the measures the AI
  survey and `find` already compute). The chosen seed and its score are in
  the `Header`, so a rebake is reproducible.
- **Rate.** Recorded from the 240 Hz sim at `--rate` (default 30; 60 for
  promo material).
- **Freshness.** The `Header` carries the track's `source_crc` and every car's
  `content_crc`. `info --check` exits non-zero when the content on disk no
  longer matches, which is how the pipeline decides to rebake.

### Naming and where files live

`<Stem>.<class>.<variant>.apxs`, variant a short name for the sky
(`day`, `dusk`, `rain`, `night`). Rendered into `build/showcase/`
(gitignored, like the track exports).

- `scripts/build_track_levels.ps1` gains a showcase stage after the export
  (`-SkipShowcase`), rendering the default variant per circuit from a list in
  `content/showcase.yml` (track, class, variants, seed) and only what is
  missing or stale.
- `scripts/initialize_content.ps1` gets the same stage.
- `build_game_standalone.ps1` / `build_release.ps1` copy the files to
  `Game/Showcase/` (the client's local backdrop) and `Server/showcase/` (the
  server's playlist), and ship `apexsim-replay.exe` in `Server/` so a modder
  can render a custom track or class with the same command.
- Imported tracks (`imported` marker) are rendered only on request, never by
  the pipeline, like their exports.

## 3. The server showcase endpoint

### Configuration

```toml
[showcase]
enabled = true
dir = "showcase"                     # relative to the server's working dir
playlist = ["Zandvoort.gt3.day", "Spa.lmp2.dusk"]   # or ["*"]
mode = "loop"                        # "loop": one file per channel, forever
                                     # "rotate": next file in the playlist at each end
stream_divisor = 1                   # send every Nth frame of the file
```

At startup the server reads every file's `Header` and `Roster` (a few KB
each), drops a file whose track or cars it does not have or whose CRCs
disagree with its content (logged), and lists the rest.

### Messages

| Direction | Message | Notes |
|---|---|---|
| C→S | `ListShowcases` | |
| S→C | `Showcases { entries }` | per entry: showcase id, track id, class, sky summary, duration, viewers |
| C→S | `SpectateShowcase { id: Option }` | none = the server's default (first in the playlist) |
| S→C | `SpectatorJoined { stream_id, kind: Showcase }` then `Header`, `Roster`, `TrackSectors` | TCP |
| S→C | `Frame` records | UDP, after the existing `UdpHandshake` |
| S→C | `Event` records | TCP |
| C→S | `LeaveSpectate` | also implied by joining or creating a session |

`LobbyState` gains `showcase_available: bool` so a client knows to ask.
The HTTP side lists the channels at `/showcase` and counts viewers in
`/metrics` (`apexsim_showcase_viewers{showcase=...}`).

### Channels

A **channel** is one file playing on one clock, shared by all its viewers:
viewers join mid-race, which is what a broadcast is. The game loop advances
each channel with viewers once per tick (no thread per channel), finds the
frames whose tick has come, and fans out the **record bytes from the file
as they are**: a `Frame` is self-contained, so nothing is decoded or encoded
on the way. A channel with no viewers stops and drops its decompressed
blocks; the first viewer starts it from the beginning of the content (or the
countdown, configurable).

At the end of the content the channel bumps its epoch and sends the `Header`
again (`loop`) or the next file's `Header` and `Roster` (`rotate`).

Cost per viewer: one UDP send per frame of ~900 bytes. No physics, no AI, no
per-viewer serialization.

### Replacing the demo session

The client asks for a showcase instead of creating a `SessionKind::Demo`
session when the server lists one. `SessionKind::Demo` stays for one release
so an older client still gets a backdrop, then goes.

### Later: live spectating

`JoinAsSpectator` today sends the full racer telemetry. With the stream in
place a spectator gets `SpectatorJoined { kind: Live }`, the session's
`Header`/`Roster`, and `Frame`s encoded from the live session once per
broadcast tick (`broadcast.rs` already serializes once per session). The
encoder (`BroadcastEncoder`: session + `Telemetry` → records) is the same one
the render tool writes files with, and the live session can record to a file
through it too. Out of scope for the first implementation; the format is
designed for it now.

## 4. The client spectator player

### One player, two sources

`UApexSpectatorSubsystem` (game instance, `ApexSim`) owns the playback; the
record codec lives in `ApexSimNet` beside the protocol (`ApexSpectatorStream`,
pure, unit-tested on golden bytes).

- **File source**: reads an `.apxs`, decompresses blocks on a worker thread,
  hands records to the game thread ahead of the playhead. Clock: game time,
  advanced by the subsystem, so a fixed-timestep recording run
  (`-ApexReplayRecord`) stays in step.
- **Net source**: `UApexNetSubsystem` routes `SpectatorJoined`, the stream
  records and `Event`s to the subsystem instead of the session path.
  `IsInSession()` stays false and the session delegates stay quiet, as the
  demo session does today. Clock: arrival fitting, as the motion buffer does
  for live telemetry.

### What the records drive

| Record | Becomes | Consumer, unchanged |
|---|---|---|
| `Header` | track stem/id, conditions, tick rate | `AApexRaceDirector` builds the track (`UApexTrackInstance`), `ApexSky::Derive` lights it |
| `Roster` | `FApexSessionRoster` | the director spawns and dresses the cars (`Prefetch`, liveries) |
| `Frame` row | `FApexCarTelemetry` per car (fields the row lacks set to unknown: tyres -1, fuel -1, ...) | each car's `FApexCarMotionBuffer`, so pose, wheels, lights, DRS flap, damage, engine sound work as in a live race |
| `Event` | `LapTiming`, sectors, finishes | `FApexTimingBoard`, TV director incident cues |

The motion buffer gains an optional external clock in ticks (the file
source's), so one blending path serves both sources; `SetPlaybackPose`
and `FApexReplayClip` retire once `-ApexReplay` reads `.apxs`.

### The menu backdrop

`UApexDemoModeSubsystem` picks a source in this order:

1. Connected and the server lists a showcase: `SpectateShowcase`.
2. Otherwise a local file from `Showcase/` beside `ApexSim.exe` (the repo's
   `build/showcase` in the editor): the pending track and the last car class
   if a file matches, else any. This needs no server, so the splash hold
   (`IsDemoExpected` / `IsDemoReady`) can end on it even offline.
3. Otherwise the static page backgrounds, as with `-ApexNoDemo`.

The rest of the backdrop is unchanged: the TV director's camera, the root
widget's fade and scrim, the world hidden behind car select and session
create, the demo's engine volume scale, and `apexsim.demo.*` (MaxMinutes
cycles to another file or showcase). The random sky roll goes: the sky is the
file's, because the AI raced in it.

A local file whose track `source_crc` or car CRCs do not match this
machine's content is skipped and logged (cars would drive a road that has
moved). A net stream that disagrees logs once, as a demo session does today.

### Console and command line

`-ApexShowcase=<file|id>`, `-ApexNoShowcase`, `apexsim.spectate.Info`
(source, epoch, frames buffered, rate, drops), `apexsim.spectate.Next`.

## Implementation order

| # | Step | Size |
|---|---|---|
| 1 | Stream codec in Rust (`spectator.rs`: records, row, file blocks/index), golden bytes, determinism test | 2-3 days |
| 2 | `apexsim-replay render` / `info` / `convert`, seed scoring | 1-2 days |
| 3 | Client codec (`ApexSimNet`) on the golden bytes; file source; player into roster / motion buffer / timing board | 3-4 days |
| 4 | Backdrop from a local file; splash hold; retire the sky roll | 1-2 days |
| 5 | Pipeline: `content/showcase.yml`, `build_track_levels.ps1`, `initialize_content.ps1`, release copies, ship `apexsim-replay.exe` | 1 day |
| 6 | Server `[showcase]`, channels, messages, `/showcase`, metrics; client net source; prefer showcase over the demo session | 2-3 days |
| 7 | `-ApexReplay` and `cut` on `.apxs`, promo pipeline moved over; retire `.clip.json` | 1 day |
| later | Live spectating through `BroadcastEncoder`; replays recorded as `.apxs` | |

Steps 1-5 solve the startup problem without touching the server's runtime;
step 6 moves the backdrop's source to the server.

## Tests

- Rust: record and row round-trips, golden bytes printed by
  `cargo test spectator_wire_format -- --nocapture`, a seeded render written
  twice byte-identical, `info --check` against changed content, a showcase
  channel streamed to two in-process `TestClient`s (shared clock, loop bumps
  the epoch, a late joiner gets `Header` and `Roster` first).
- Client: `ApexSim.Spectator.Codec` (golden bytes, unknown trailing row bytes
  skipped), `.File` (block decode, seek by index), `.Player` (stale epoch and
  revision dropped, roster change mid-stream, parts), `.Backdrop` (source
  order, CRC mismatch skips a file).

## Open questions

1. **Replays as `.apxs`.** One format everywhere is cleaner, but `find`,
   `pose` and `cut` read today's replay (full `Telemetry`, which also has
   what a spectator does not need). Proposal: keep `replay.rs` for now and
   `convert` from it; revisit with live spectating.
2. **Showcase vs local file at startup.** Proposed: server showcase first when
   connected (the server owner picks what the menu shows), local file
   otherwise. The alternative, local first and the server never involved in
   the backdrop, makes step 6 optional.
3. **Frame rate.** 30 Hz in files and streams keeps a viewer at ~27 KB/s;
   60 Hz matches live racing telemetry and the promo material. Proposed: 30
   for showcase files, 60 for `convert`/promo, `stream_divisor` for live.
4. **Variants per circuit.** One sky per circuit keeps the build short; three
   (day, dusk, rain) give the backdrop variety at about 3x the render time and
   disk.
