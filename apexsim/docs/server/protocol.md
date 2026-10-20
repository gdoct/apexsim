# Wire protocol

How the client and server talk: two sockets, MessagePack messages, a token
handshake that binds UDP to a TCP connection, and byte-exact golden blobs that
keep a hand-written C++ codec in step with `rmp_serde`. Protocol version 2
(`network::PROTOCOL_VERSION`).

## Code

- The `apexsim-protocol` crate (`protocol/`), which the server and the
  physics library both link and the server re-exports under the same paths:
- `protocol/src/network.rs` - `ClientMessage`, `ServerMessage`, their payload
  structs, `MessagePriority`, `PROTOCOL_VERSION`, and the `*_wire_format`
  tests that print golden bytes.
- `server/src/transport.rs` - TCP (+TLS) and UDP IO, framing, auth, the UDP
  handshake, rate limits, bounded queues, `TransportMetrics`.
- `server/src/game_loop/dispatch.rs` (handlers), `broadcast.rs` (fan-out,
  UDP or TCP fallback), `tick.rs` (telemetry and feedback serialization),
  `lifecycle.rs` (disconnects, held seats, stale connections).
- `protocol/src/spectator.rs` - the spectator stream records
  ([../game/spectator.md](../game/spectator.md)).
- Client, `game-unreal/Source/ApexSimNet/`: `ApexProtocolCodec` and
  `MsgPack/` (the codec), `ApexProtocolTypes.h` (the structs),
  `ApexTcpConnection` / `ApexUdpConnection` (sockets, background receive
  threads), `UApexNetSubsystem` (processes messages on the game thread),
  `Private/Tests/` (golden blobs and their tests).

## Transport

| Channel | Carries |
|---|---|
| TCP 9000 | Everything reliable: auth, lobby, sessions, rosters, lap timing, racing line, setup sheet, pit service, ghost laps, spectator records, a track's physics bundle on request. Telemetry for a client with no UDP binding. |
| UDP 9001 | `PlayerInput` and `UdpHandshake` in; `TelemetryCompact`, `DriverFeedback`, `UdpHandshakeAck` and spectator frames out. |

- **TCP framing**: `[u32 big-endian length][MessagePack body]`. The server
  rejects a client frame over 1,000,000 bytes; the client accepts server
  frames up to 4 MiB (`ApexTcpConnection::MaxFrameBytes`).
- **UDP**: one message per datagram, no framing. The client reads datagrams
  up to 64 KiB (`ApexUdpConnection::MaxDatagramBytes`); a full grid's
  telemetry frame is one datagram.
- **Sealed inbound UDP** (`server/src/udp_seal.rs`,
  `ApexProtocol::SealUdpDatagram`): every datagram the client sends, the
  handshake included, is `[0xC1][u64 big-endian seq][16-byte tag][message]`,
  the tag being HMAC-SHA1 over `seq ‖ message` under the UTF-8 bytes of
  `AuthSuccess.udp_key` (32 random bytes as hex, issued per connection over
  TLS). The server drops a datagram that is bare, carries a bad tag, or
  whose `seq` is not above the last one it accepted for that connection
  (`apexsim_udp_datagrams_rejected`), so a forged source address, a
  tampered input or a replay never reaches the game loop. 0xC1 is the one
  byte MessagePack never uses, so a bare datagram is told apart at once. The
  client's `seq` rises with every send, handshake retries included. Outbound
  datagrams (telemetry, feedback, the ack, spectator frames) are plain:
  nothing in them is secret and the client is a puppet of them anyway.
- **TLS**: the server wraps TCP in TLS (rustls, TLS 1.2/1.3) when it has a
  certificate ([operations.md](operations.md#tls-and-authentication)); the
  framing above is unchanged inside it. The game's `FApexTcpConnection` runs
  OpenSSL (the engine's) on memory BIOs over the same socket and thread
  (`FApexTlsSession`), checks the certificate (CA roots and host name, or a
  pinned SHA-256, or nothing with `tls_verify: false`) and only then sends
  `Authenticate`. With `server.tls: auto` (the default) a server that closes
  the connection on the ClientHello without a byte back, or answers with
  something that is not a TLS record, is connected to again in plaintext: a
  plaintext server reads the hello's first four bytes (`16 03 01 xx`) as a
  ~370 MB frame length and drops the connection. A failed certificate check
  never falls back. **UDP is never encrypted**: telemetry and input are
  plaintext. Inbound datagrams are authenticated by the seal above under
  the `udp_key` that `AuthSuccess` carries over the TCP (TLS) connection;
  the `udp_token` in the handshake only names the connection.

## Connection flow

1. Client opens TCP and sends `Authenticate { token, player_name,
   protocol_version, resume_token? }`. Anything else before it is dropped
   and counted as a violation.
2. Server checks the version (a mismatch, or an old client without the field,
   gets an `AuthFailure` naming both) and the token (`[auth]`), checks the ban
   list, and answers `AuthSuccess` with the `PlayerId`, a one-time
   `udp_token` and the `udp_port`, then puts the player in the lobby.
   `AuthSuccess.resume_token` is a secret the client presents as
   `Authenticate.resume_token` on a later connection to get the same
   `PlayerId` back (a still-open connection of that player is dropped); an
   unknown token is a new player. A player holding a seat in a session is
   then sent `RejoinAvailable` ([sessions.md](sessions.md#reconnecting)).
3. Client sends `UdpHandshake { token }` over UDP, sealed under `udp_key`,
   resending (with a higher `seq` each time) until it gets `UdpHandshakeAck`
   (datagrams get lost). The server looks the connection up by the token,
   checks the seal under that connection's key, and only then binds the
   datagram's source address to the connection; a later handshake re-binds
   it. A client with no `udp_key` (an older server) does not start UDP.
4. From then on a UDP datagram is looked up by its source address and
   accepted when its seal verifies under that connection's key; datagrams
   from an unbound address, or that fail the seal, are dropped. Telemetry
   goes over UDP; a client that never handshakes (or whose handshake is
   never accepted: a client from before sealing) gets telemetry over TCP
   (droppable) and no `DriverFeedback` at all, which is UDP only because a
   late force is worse than none.
5. Client sends `Heartbeat` over TCP every 2 s
   (`UApexNetSubsystem::HeartbeatIntervalSeconds`). The server drops a
   connection silent for `heartbeat_timeout_ms` (5 s) while it is in a
   session, 30 s while it is in the lobby. `HeartbeatAck.server_tick` is
   the game loop's tick count (wrapping `u32`, not a session's tick).
   `Heartbeat.ping_ms` carries the round trip the client measured from the
   last ack (left off before the first), for the dashboard's player list.

At any time the server may send `ServerNotice { message }`: the operator's
words (a dashboard broadcast, or why a session was ended), which the client
shows as an 8 s toast over whatever is on screen.

**Chat** (`crate::chat`, `dispatch::handle_chat`): `Chat { text }` from a
client goes on as `Chat(ChatData { PlayerId, From, Text, InSession })` to
the session the sender drives in or watches (its drivers and spectators,
`InSession` true), else to everyone connected outside a session (left off).
The server cleans the line (control characters out, whitespace runs as one,
200 characters at most) and holds each player to 5 lines in 10 s (`Error
429` past that); a player the operator muted gets `Error 403`. Golden bytes:
`cargo test chat_wire_format -- --nocapture` -> `ApexGolden::C_Chat`,
`S_ChatLobby`, `S_ChatSession`. Client: `UApexNetSubsystem::SendChat`,
`OnChat`, `GetChat` (the last 50 lines); the session lobby's chat under the
grid, and in a race the Chat key (T) opens a line over the HUD (the car's
controls off while it is open, as under the pause menu) with the `chat`
HUD component listing the session's recent lines
([../game/hud-modding.md](../game/hud-modding.md)).

Joining a session: `SelectCar`, then `CreateSession` or `JoinSession`, answered
by `SessionJoined`, a `SessionRoster` (car index to player, reliable, resent
whenever membership changes), `RacingLine`, `TrackSectors`, `CarSetupSheet`,
the whole lap's `RoadState` and, in a hotlap watch, `TrackCorners`. A
session then sends a `RoadState` slice every 2 s
([conditions.md](conditions.md#the-road-on-screen)). Session rules travel inside
`CreateSession` and are echoed in `SessionJoined`
([sessions.md](sessions.md)). A race weekend's members also get
`WeekendState` after `SessionJoined` and whenever it changes
([sessions.md](sessions.md#race-weekend)), and a qualifying shoot-out's
members `ShootoutState` the same way
([sessions.md](sessions.md#starting-order-and-qualifying)). A hotlap watched as a stream
(`CreateSession.watch.stream`) is answered as `SpectateSession` is instead:
`SpectatorJoined`, `TrackCorners` and the stream's records, no `SessionJoined`
([sessions.md](sessions.md#watching-a-hotlap)).

## Encodings

- **Named** (`rmp_serde::to_vec_named`): every client message and every
  server message but two. Enums are internally tagged, `{ "type": <variant>,
  "data": <payload> }`; payload structs are maps by field name, several in
  `PascalCase` (`#[serde(rename_all)]`).
- **Positional** (`rmp_serde::to_vec`): `TelemetryCompact` and
  `DriverFeedback`, the two per-tick streams. A struct is an array; a field
  is its index. Cars in telemetry are a session-scoped `car_index: u8`
  announced by `SessionRoster`, not UUIDs.
- `TelemetryCompact`'s fields after the cars are each left off while they
  and everything after them are empty, nil when something after them is
  sent: `race_clock`, `sky`, `yellow_flags`, `race_phase` (1 formation lap,
  2 pace lap, 3 / 4 virtual safety car out / ending, 5 safety car out, 6
  safety car in this lap and the restart), then `safety_car`, the safety
  car's pose while it is on the road or waiting at the pit exit: `[x, y, z,
  yaw_rad, speed_mps]`, float32s in the server's frame as a car's pose. It
  is not a car: no `car_index`, no roster row (client
  `FApexTelemetryFrame::RacePhase`, `::SafetyCar`).
- `DriverFeedback`'s two last fields, `own_car` and `own_state`, are left
  off while they and everything after them are empty, nil when something
  after them is sent (hand-written `Serialize`, as `TelemetryCompact`'s): a
  driver whose client does not number its inputs gets the 14-field message
  of before them. See [the own car](#the-own-car-for-prediction).
- `ServerMessage::Telemetry` (named, full state) is what the replay recorder
  stores; it is not broadcast.

## The own car, for prediction

Client prediction ([../proposals/client-prediction.md](../proposals/client-prediction.md))
rewinds the driver's own car to the server's and replays the inputs the
server had not applied yet. The wire for it (`sim/src/own_car.rs`):

- **`PlayerInput.seq`** (`Option<u32>`, after `handbrake`, left off when
  `None`): the client's number for the input, one higher with each it sends
  (the UDP worker stamps it, from 1; `FApexUdpConnection::GetLastInputSeq`).
  The game loop holds each driver's newest input with its number
  (`game_loop::HeldInput`) and, on every tick that input drives the car,
  records the number as applied (`GameSession::own_car_feeds`). The number
  never reaches the physics. An input without one drives exactly as
  before. A reordered or replayed datagram never reaches the loop (the
  seal's rising `seq`), so the applied number only rises.
- **`DriverFeedback.own_car`** (15th field): `OwnCarSnapshot`, the car at
  the message's `server_tick` (the tick just simulated), sent only once the
  driver's inputs carry a number, and while the debug stand-in drives the
  car in their place too (`input_seq` then names none applied). A positional array of 26: `tick`,
  `input_seq` (the newest number applied by that tick, nil for none), then
  what the car step carries from tick to tick: `pos[3]`, `attitude[3]` (yaw,
  pitch, roll), `vel[3]`, `speed_mps`, `angular_vel[3]`,
  `wheel_angular_vel[4]`, `wheel_load_n[4]`, `suspension_travel_m[4]`,
  `suspension_velocity_mps[4]`, `g_forces[3]`, `vertical_accel_mps2`,
  `ground_slope[2]`, `porpoise[2]`, `controls[4]` (throttle, brake,
  steering, clutch as applied), `gear` (i8), `engine_rpm`, `turbo_spool`,
  `auto_shift_hold_ticks`, `auto_reverse_ticks` (u16), `track_progress`,
  `lateral_offset_m`, `current_lap` (u16), `centerline_idx` (u32 or nil),
  `flags` (1 on track, 2 airborne, 4 colliding, 8 DRS open, 16 wheels off
  track). Server frame and units, exact `f32`s; per-wheel FL FR RL RR.
- **`DriverFeedback.own_state`** (16th field): `StateChunk`, `[tick,
  input_seq, index, count, bytes]`, one slice of at most 640 bytes of the
  car's full state blob (`car_step::encode_state`, `APXC`, about 5.5 kB, so
  9 slices): the tyres, fuel, hybrid, brakes and damage the snapshot leaves
  out. A blob is taken on a driver's first numbered message and every
  `own_car::STATE_BLOB_SECONDS` (0.5 s, 30 messages at 420 Hz / 7) after,
  and goes out a slice a message; every slice names the blob's tick and
  input. The client keeps the slices of one blob in order and drops it when
  one is lost (`FApexOwnCarStateAssembler`).
- **`SessionJoined.Physics`** (named, last, left off when `None`):
  `SessionPhysics { tick_rate_hz, road_contact }` (keys snake_case), what
  the client's copy of the car step needs that only the server knows: the
  session's tick rate (the step's `dt`) and the road its wheels stand on (0
  the centerline, 1 the road mesh: `GameSession::session_physics`, the mesh
  only when the session's track has one loaded). Sent to a driver (a
  create, a join, a rejoin); a spectator's and a demo's join leave it off,
  and without it the client does not predict. The client also keeps
  `SessionJoined.Conditions` as the bytes the server wrote
  (`FApexServerMessage::ConditionsBytes`), which is what the library bakes.
  Golden: `session_joined_physics_wire_format` into
  `ApexGolden::S_SessionJoinedPhysics`.
- **The track's physics bundle** (`sim/src/track_bundle.rs`): the files
  the car step reads, the track YAML and the sidecars beside it (ground,
  curbs, walls, pit lane, and the road mesh when the session drives on it),
  as the server read them when it loaded the track (`track_content`, once,
  the same bytes it drives on), deflated. `SessionPhysics` names it with
  `bundle_crc`, `bundle_bytes` and `bundle_chunks` (each left off at 0: a
  track built in memory, or one whose YAML changed on disk since the catalog
  read it). The checksum covers every file: CRC-32 over `APXT`, the version,
  and per file a presence byte and its CRC (the YAML's as `ContentCrc`,
  carriage returns dropped; a sidecar's over every byte), so a sidecar
  baked another day differs as a changed YAML does. A client that lacks
  the files or holds others asks for it a chunk at a time,
  `RequestTrackBundle { crc, chunk }` (named), answered with
  `TrackBundleChunk(TrackBundleChunkData { Crc, Chunk, Chunks, Bytes })`
  (Critical, PascalCase, `Bytes` a bin of 256 KiB at most); `Chunks` 0 says
  the server has no such bundle. Asked for, not pushed: a client whose own
  copy (or cached bundle) matches asks for nothing, and asking a few at a
  time keeps a 10 MB bundle from queueing ahead of the session's own
  messages; the requests count against the control-message rate (10/s,
  burst 20), which the client paces under (6/s, 3 in flight). Sizes, from
  the main checkout's sidecars (`server/ffi/tests/track_bundle.rs`
  `print_bundle_sizes`): Monza 5.3 MB of files, 2.6 MB in 10 chunks;
  Spa 6.7, 3.3 MB in 13; Le Mans 14.5, 6.9 MB in 27; the Nordschleife
  20.3, 10.2 MB in 40 (the road mesh is two thirds of each). The server
  deflates a bundle when it loads the track (Nordschleife 112 ms in a
  release build). Golden: `track_bundle_wire_format` into
  `S_SessionJoinedPhysicsBundle`, `C_RequestTrackBundle`,
  `S_TrackBundleChunk`, `S_TrackBundleChunkMissing`;
  `protocol_test::test_the_track_bundle_comes_in_chunks_on_request` fetches
  one over TCP and decodes it against its checksum.

Sizes at the default rates (`network.rs`
`own_car_feedback_fits_one_small_datagram`, every number at its widest):
158 bytes for the feedback alone, 425 with the snapshot, 1,087 with a
slice, under the 1,472 bytes of UDP payload a 1,500-byte MTU carries, so
no feedback datagram is fragmented. A numbered driver costs about 16 kB/s
of snapshots and 11 kB/s of state on top of the feedback's 9.5 kB/s.

The client takes each message's snapshot and slice off before folding the
messages of a frame together (`FApexDriverFeedback::Absorb`) and keeps them
on `UApexNetSubsystem` (`GetOwnCarSnapshot`, `GetOwnCarState`, their
serials, and when the UDP worker took the snapshot's datagram in,
`GetOwnCarSnapshotSeconds`); the prediction loop reads them
([../game/client.md](../game/client.md#client-prediction-raceapexpredictionsubsystemh)).

## Evolving a message

- **Append only.** Never reorder, rename, retype or remove a field.
- A new named field is `#[serde(default)]` and, where it has a default
  meaning, `skip_serializing_if` it is the default, so a message that does
  not use it keeps its old bytes (`CreateSession.race_seconds`,
  `grid_order`, `damage`, `ai_skill`, `qualifying_seconds`, `weekend`...).
  An older peer ignores unknown keys.
- A new positional field goes at the end of its struct. The client's decoder
  skips trailing elements it does not know (`ApexSim.Net.MsgPack.SkipValue`),
  so an older client survives a newer server. `CompactTelemetry` has a
  hand-written `Serialize` that leaves trailing empty optional fields off,
  so a frame that does not use a new field keeps its length. On the client,
  "not sent" is a sentinel (-1, 255...) the HUD shows as unknown.
- Bump `PROTOCOL_VERSION` only for a change an older peer cannot survive.
- Every server-to-client message has a priority (`ServerMessage::priority`):
  a new one must be added there, as Critical (must arrive: auth, errors,
  session control, roster, lap timing, anything asked for once) or Droppable
  (superseded by the next one: telemetry, feedback, lobby state, heartbeat
  acks, countdown).

## Golden bytes

Both sides pin each message to exact bytes. The server tests build a fixed
message and print it; the client holds the same bytes in
`ApexGoldenBlobs.h` (TCP, named), `ApexUdpGoldenBlobs.h` (UDP, positional) and
`ApexSpectatorGoldenBlobs.h` (spectator records), and its tests both encode to
them and decode from them.

Workflow for a wire change:

1. Change the server struct; extend the matching test in `network.rs`.
2. `cd protocol && cargo test <test> -- --nocapture` (`udp_seal_wire_format`:
   `cd server`) prints the new bytes as
   hex under the client constant's name; paste them into the client header as
   a **new** constant (keep the old blob: it is what an older peer sends, and
   the decoder must still take it).
3. Change `ApexProtocolTypes.h` / `ApexProtocolCodec.cpp`, then run the
   client's `ApexSim.Net.Protocol.GoldenEncode` / `.GoldenDecode` and
   `ApexSim.Net.Udp.GoldenEncode` / `.GoldenDecode`.
4. Land both sides together.

The printing tests, by area (`cargo test -- <a> <b> --nocapture` runs several):

| Area | Tests |
|---|---|
| Telemetry, feedback, input | `telemetry_compact_wire_format` (`S_TelemetryCompactFlags`: 54 fields, the last `race_flags`, and the frame's `yellow_flags`; `S_TelemetryCompactStint` is the 53-field frame before them), `driver_feedback_wire_format`, `driver_feedback_own_car_wire_format` (`S_DriverFeedbackOwnCar`: the snapshot; `S_DriverFeedbackOwnState`: and a state slice), `driver_feedback_speed_cap_wire_format` (`S_DriverFeedbackSpeedCap`: the pace held, after nil for both), `player_input_drs_wire_format`, `player_input_headlights_wire_format`, `player_input_seq_wire_format` (`C_PlayerInputSeq`), `udp_seal_wire_format` (udp_seal.rs: the seal around every inbound datagram) |
| Lap timing, line, corners | `lap_timing_wire_format`, `racing_line_wire_format`, `track_corners_wire_format` |
| Road state | `road_state_wire_format` |
| Session rules | `assists_wire_format`, `session_damage_wire_format`, `race_time_wire_format`, `qualifying_time_wire_format` (`CreateSession.qualifying_seconds`, `SessionJoined.QualifyingSeconds`, a qualifying `race_clock`), `weekend_wire_format` (`CreateSession.weekend` with and without a day's sky (`C_CreateSessionWeekendDays`, sent by the codec), `SessionJoined.Weekend`, `WeekendState`, a late entry (`S_WeekendStateLate`), a lobby listing a weekend between phases (`S_LobbyStateWeekendWaiting`), `AdvanceWeekend`, a `GameMode::Practice` frame with its clock), `session_ai_skill_wire_format`, `qualifying_cutoff_wire_format` (`CreateSession.qualifying_cutoff_pct`, `SessionJoined.QualifyingCutoffPct`), `pit_rule_wire_format` (`CreateSession.mandatory_stops` / `compound_change`, `SessionJoined.MandatoryStops` / `CompoundChange`, `RosterEntry.MissedStops`), `shootout_wire_format` (`CreateSession.qualifying_format` / `qualifying_lap_allowance`, `SessionJoined.QualifyingFormat` / `QualifyingLapAllowance`, `ShootoutState`, a weekend's `practice_damage` / `qualifying_damage`, `WeekendState.Damage`, a day's sky in `WeekendState.Conditions` (`S_WeekendStateConditions`)), `race_rules_wire_format` (`CreateSession.race_rules`, `SessionJoined.RaceRules`, a pace-lap frame's `race_phase`, `S_TelemetryCompactPaceLap`: the field after `yellow_flags`), `penalties_wire_format` (`race_rules.yellow_penalty` / `pit_exit_penalty`, `RosterEntry.Penalties`), `safety_car_wire_format` (`race_rules.safety_car`, a frame's `race_phase` 3), `reverse_grid_wire_format` (`race_rules.reverse_grid`, `C_CreateSessionReverseGrid`), `full_safety_car_wire_format` (`race_rules.safety_car` 2, `C_CreateSessionFullSafetyCar`; a frame with `race_phase` 5 and the safety car's pose, `S_TelemetryCompactSafetyCar`), `lobby_race_rules_wire_format` (a listed session's `RaceRules` and the lobby's `Balanced`, `S_LobbyStateRaceRules`), `conditions_air_wire_format`, `sky_wire_format`, `grid_wire_format` |
| Prediction | `session_joined_physics_wire_format`, `track_bundle_wire_format` (`SessionJoined.Physics` naming the bundle, `RequestTrackBundle`, `TrackBundleChunk` with and without its chunk) |
| Garage, pit, hotlap | `car_setup_wire_format`, `car_setup_sheet_wire_format`, `pit_service_wire_format`, `pit_plan_wire_format` (`SetPitPlan`, full and empty), `hotlap_wire_format`, `hotlap_watch_wire_format` (`CreateSession.watch`), `recover_wire_format` |
| Reconnect | `rejoin_wire_format` (`Authenticate.resume_token`, `AuthSuccess.ResumeToken`, `RejoinAvailable`, `GiveUpSeat`) |
| Roster | `roster_wire_format` (`RosterEntry.Away`, `RosterEntry.GridSlot`, `RosterEntry.Dnq`, `RosterEntry.QualifyingLaps`) |
| The race's grid | `ready_grid_wire_format` (`OpenGrid`, `SetReady`, `SetLivery`, `GameModeChanged { Grid }`, `RosterEntry.Ready`) |
| Lobby | `track_site_wire_format` (`TrackConfigSummary.LatitudeDeg` / `LongitudeDeg`) |
| Spectator | `showcase_wire_format` (network.rs), `spectator_wire_format` (spectator.rs), `recordings_wire_format` (`LobbyState.recordings_port`) |

`test_lobby_summaries_carry_content_crc` pins the lobby's checksum fields
([../architecture.md](../architecture.md#content-checksums)).

## Bounded queues and backpressure

Every channel a client can fill is bounded, so a slow or hostile client
cannot grow server memory:

| Queue | Capacity |
|---|---|
| Inbound events (TCP messages and bound UDP datagrams) | 1000 |
| UDP outbound (shared) | 2000 |
| Per-connection TCP outbound | 100 |

- **Critical** messages wait for room on the connection's queue; if that
  fails the send returns `TransportError::QueueFull`, counts
  `clients_disconnected_backpressure`, and the client is to be dropped.
- **Droppable** messages use `try_send`: a full queue drops the message and
  counts `tcp_messages_dropped` / `udp_messages_dropped` (logged every 100th
  / 1000th). UDP sends are always droppable.
- While a track's sidecars load, up to 4096 events per connection are held in
  order (`track_loads::MAX_HELD_EVENTS`); past that the newest are dropped.

The counters are exported on `/metrics` ([operations.md](operations.md#health-and-metrics)).

## Rate limiting

Token buckets per TCP connection: control traffic 10 msg/s (burst 20),
`PlayerInput` and `Heartbeat` 300 msg/s (burst 60); a bucket per UDP source
address at the input rate. A message over the limit is dropped and counted as
a violation (a pre-auth message counts ten); 200 violations close the
connection. Constants at the top of `transport.rs`.

## Checking it

- `server/tests/all/protocol_test.rs`: version rejection, the UDP binding info,
  the handshake, input and telemetry loopback, feedback over UDP, the own
  car (`test_input_sequence_comes_back_with_the_own_car_snapshot`: an
  unnumbered input drives with no snapshot; a numbered one comes back with
  the throttle it applied, and the slices make a blob that decodes), the seal
  (`test_udp_handshake_needs_the_seal`: bare, wrong-key and replayed
  handshakes never bind; `udp_seal.rs` unit tests cover tampering and
  reordering), the tick
  rate.
- `auth_test.rs` (token mode, pre-auth drops), `tls_requirement_test.rs`,
  `transport_backpressure_test.rs` (bounded queues, priorities, metrics),
  `live_spectator_test.rs`, `showcase_test.rs`.
- Client: `ApexSim.Net.Protocol.*`, `ApexSim.Net.Udp.*` (including
  `.Robustness`: truncated and garbage input, and `.OwnCarStateAssembler`),
  `ApexSim.Net.MsgPack.*`.

## Traps

- Telemetry carries no session id. After a hitch the UDP queue can hold
  seconds of the session just left, so the client drops queued frames on
  `SessionJoined` and applies a frame only when every car index fits the
  current roster; a test fixture whose roster does not match its frames has
  every frame silently dropped.
- A field added in the middle of a positional struct shifts everything after
  it for every older peer: append only.
- The seal's sequence number is per key and must rise on *every* send. A
  client that reuses a number (a second socket under the same key, a reset
  counter after a reconnect on the same `AuthSuccess`) has every datagram
  after the highest one dropped as a replay with nothing in the game but a
  car that stops answering; `apexsim_udp_datagrams_rejected` climbing is
  the tell. A fresh `Authenticate` gets a fresh key and counter.
- A full grid's telemetry frame is one datagram. A receive buffer smaller than
  the frame drops it whole and the cars freeze; the client's drop log is
  Verbose, so it looks like a server stall.
- A Critical message to a client whose queue stays full disconnects it; do not
  make a per-tick message Critical.
- A Windows wait shorter than the timer resolution rounds up to it: never put a
  short `tokio::time::timeout` on the loop's receive path.
