# Wire protocol

How the client and server talk: two sockets, MessagePack messages, a token
handshake that binds UDP to a TCP connection, and byte-exact golden blobs that
keep a hand-written C++ codec in step with `rmp_serde`. Protocol version 2
(`network::PROTOCOL_VERSION`).

## Code

- `server/src/network.rs` - `ClientMessage`, `ServerMessage`, their payload
  structs, `MessagePriority`, `PROTOCOL_VERSION`, and the `*_wire_format`
  tests that print golden bytes.
- `server/src/transport.rs` - TCP (+TLS) and UDP IO, framing, auth, the UDP
  handshake, rate limits, bounded queues, `TransportMetrics`.
- `server/src/game_loop/dispatch.rs` (handlers), `broadcast.rs` (fan-out,
  UDP or TCP fallback), `tick.rs` (telemetry and feedback serialization),
  `lifecycle.rs` (disconnects, held seats, stale connections).
- `server/src/spectator.rs` - the spectator stream records
  ([../game/spectator.md](../game/spectator.md)).
- Client, `game-unreal/Source/ApexSimNet/`: `ApexProtocolCodec` and
  `MsgPack/` (the codec), `ApexProtocolTypes.h` (the structs),
  `ApexTcpConnection` / `ApexUdpConnection` (sockets, background receive
  threads), `UApexNetSubsystem` (processes messages on the game thread),
  `Private/Tests/` (golden blobs and their tests).

## Transport

| Channel | Carries |
|---|---|
| TCP 9000 | Everything reliable: auth, lobby, sessions, rosters, lap timing, racing line, setup sheet, pit service, ghost laps, spectator records. Telemetry for a client with no UDP binding. |
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

Joining a session: `SelectCar`, then `CreateSession` or `JoinSession`, answered
by `SessionJoined`, a `SessionRoster` (car index to player, reliable, resent
whenever membership changes), `RacingLine`, `TrackSectors`, `CarSetupSheet`,
the whole lap's `RoadState` and, in a hotlap watch, `TrackCorners`. A
session then sends a `RoadState` slice every 2 s
([conditions.md](conditions.md#the-road-on-screen)). Session rules travel inside
`CreateSession` and are echoed in `SessionJoined`
([sessions.md](sessions.md)).

## Encodings

- **Named** (`rmp_serde::to_vec_named`): every client message and every
  server message but two. Enums are internally tagged, `{ "type": <variant>,
  "data": <payload> }`; payload structs are maps by field name, several in
  `PascalCase` (`#[serde(rename_all)]`).
- **Positional** (`rmp_serde::to_vec`): `TelemetryCompact` and
  `DriverFeedback`, the two per-tick streams. A struct is an array; a field
  is its index. Cars in telemetry are a session-scoped `car_index: u8`
  announced by `SessionRoster`, not UUIDs.
- `ServerMessage::Telemetry` (named, full state) is what the replay recorder
  stores; it is not broadcast.

## Evolving a message

- **Append only.** Never reorder, rename, retype or remove a field.
- A new named field is `#[serde(default)]` and, where it has a default
  meaning, `skip_serializing_if` it is the default, so a message that does
  not use it keeps its old bytes (`CreateSession.race_seconds`,
  `grid_order`, `damage`, `ai_skill`...). An older peer ignores unknown keys.
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
2. `cd server && cargo test <test> -- --nocapture` prints the new bytes as
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
| Telemetry, feedback, input | `telemetry_compact_wire_format`, `driver_feedback_wire_format`, `player_input_drs_wire_format`, `player_input_headlights_wire_format`, `udp_seal_wire_format` (udp_seal.rs: the seal around every inbound datagram) |
| Lap timing, line, corners | `lap_timing_wire_format`, `racing_line_wire_format`, `track_corners_wire_format` |
| Road state | `road_state_wire_format` |
| Session rules | `assists_wire_format`, `session_damage_wire_format`, `race_time_wire_format`, `session_ai_skill_wire_format`, `conditions_air_wire_format`, `sky_wire_format`, `grid_wire_format` |
| Garage, pit, hotlap | `car_setup_wire_format`, `car_setup_sheet_wire_format`, `pit_service_wire_format`, `hotlap_wire_format`, `recover_wire_format` |
| Reconnect | `rejoin_wire_format` (`Authenticate.resume_token`, `AuthSuccess.ResumeToken`, `RejoinAvailable`) |
| Spectator | `showcase_wire_format` (network.rs), `spectator_wire_format` (spectator.rs) |

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

- `server/tests/protocol_test.rs`: version rejection, the UDP binding info,
  the handshake, input and telemetry loopback, feedback over UDP, the seal
  (`test_udp_handshake_needs_the_seal`: bare, wrong-key and replayed
  handshakes never bind; `udp_seal.rs` unit tests cover tampering and
  reordering), the tick
  rate.
- `auth_test.rs` (token mode, pre-auth drops), `tls_requirement_test.rs`,
  `transport_backpressure_test.rs` (bounded queues, priorities, metrics),
  `live_spectator_test.rs`, `showcase_test.rs`.
- Client: `ApexSim.Net.Protocol.*`, `ApexSim.Net.Udp.*` (including
  `.Robustness`: truncated and garbage input), `ApexSim.Net.MsgPack.*`.

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
